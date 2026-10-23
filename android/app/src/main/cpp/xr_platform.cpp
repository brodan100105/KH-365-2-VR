#include "xr_platform.h"

#include <GLES2/gl2ext.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "log.h"

namespace khvr {

namespace {

#define XR_CHECK(call)                                                   \
    do {                                                                 \
        XrResult r_ = (call);                                            \
        if (XR_FAILED(r_)) {                                             \
            LOGE("%s failed: %d (%s:%d)", #call, r_, __FILE__, __LINE__); \
            return false;                                                \
        }                                                                \
    } while (0)

PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT_ = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC glRenderbufferStorageMultisampleEXT_ = nullptr;

Pose ToPose(const XrPosef &p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

bool PoseValid(XrSpaceLocationFlags f) {
    return (f & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
}

template <typename T> bool GetProc(XrInstance inst, const char *name, T *out) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(inst, name, reinterpret_cast<PFN_xrVoidFunction *>(out)));
}

}  // namespace

bool XrPlatform::HasExtension(const char *name) const {
    for (const std::string &e : extensions_)
        if (e == name) return true;
    return false;
}

bool XrPlatform::Init(android_app *app, EGLDisplay display, EGLConfig config, EGLContext context,
                      float resolutionScale, int msaaSamples) {
    // The Khronos loader needs the VM and activity before anything else.
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                           reinterpret_cast<PFN_xrVoidFunction *>(&initLoader))) &&
        initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = app->activity->vm;
        li.applicationContext = app->activity->clazz;
        initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li));
    }

    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
    for (const XrExtensionProperties &p : props) extensions_.push_back(p.extensionName);

    std::vector<const char *> enable = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    for (const char *opt : {XR_FB_PASSTHROUGH_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                            XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME,
                            XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME})
        if (HasExtension(opt)) enable.push_back(opt);

    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = app->activity->vm;
    android.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android;
    std::strcpy(ci.applicationInfo.applicationName, "KH Days VR");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "khvr");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.data();
    XR_CHECK(xrCreateInstance(&ci, &instance_));

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &sg, &system_));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    GetProc(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", &getReq);
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(instance_, system_, &req);

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = display;
    binding.config = config;
    binding.context = context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system_;
    XR_CHECK(xrCreateSession(instance_, &sci, &session_));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(session_, &rs, &stage_))) {
        // No guardian floor: fall back to LOCAL, which still works for seated play.
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR_CHECK(xrCreateReferenceSpace(session_, &rs, &stage_));
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session_, &rs, &viewSpace_));

    glFramebufferTexture2DMultisampleEXT_ = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>(
        eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
    glRenderbufferStorageMultisampleEXT_ = reinterpret_cast<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC>(
        eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
    if (!glFramebufferTexture2DMultisampleEXT_ || !glRenderbufferStorageMultisampleEXT_) msaaSamples = 1;

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> vcv(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      viewCount, &viewCount, vcv.data());
    if (viewCount < 2) return false;
    for (int eye = 0; eye < 2; ++eye) {
        int w = std::min<int>(static_cast<int>(vcv[eye].recommendedImageRectWidth * resolutionScale),
                              vcv[eye].maxImageRectWidth);
        int h = std::min<int>(static_cast<int>(vcv[eye].recommendedImageRectHeight * resolutionScale),
                              vcv[eye].maxImageRectHeight);
        if (!CreateSwapchain(eye, w, h, msaaSamples)) return false;
    }

    if (HasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME)) {
        passthroughSupported_ =
            GetProc(instance_, "xrCreatePassthroughFB", &xrCreatePassthroughFB_) &&
            GetProc(instance_, "xrPassthroughStartFB", &xrPassthroughStartFB_) &&
            GetProc(instance_, "xrPassthroughPauseFB", &xrPassthroughPauseFB_) &&
            GetProc(instance_, "xrCreatePassthroughLayerFB", &xrCreatePassthroughLayerFB_) &&
            GetProc(instance_, "xrPassthroughLayerResumeFB", &xrPassthroughLayerResumeFB_) &&
            GetProc(instance_, "xrPassthroughLayerPauseFB", &xrPassthroughLayerPauseFB_);
    }
    if (HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
        GetProc(instance_, "xrRequestDisplayRefreshRateFB", &xrRequestDisplayRefreshRateFB_);
    if (HasExtension(XR_FB_FOVEATION_EXTENSION_NAME) && HasExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME)) {
        GetProc(instance_, "xrCreateFoveationProfileFB", &xrCreateFoveationProfileFB_);
        GetProc(instance_, "xrDestroyFoveationProfileFB", &xrDestroyFoveationProfileFB_);
        GetProc(instance_, "xrUpdateSwapchainFB", &xrUpdateSwapchainFB_);
    }
    return CreateActions();
}

bool XrPlatform::CreateSwapchain(int eye, int width, int height, int samples) {
    Swapchain &sc = swapchains_[eye];
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = GL_SRGB8_ALPHA8;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(session_, &ci, &sc.handle));
    sc.width = width;
    sc.height = height;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(sc.handle, n, &n,
                               reinterpret_cast<XrSwapchainImageBaseHeader *>(sc.images.data()));
    sc.framebuffers.resize(n);
    sc.depth.resize(n);
    glGenFramebuffers(n, sc.framebuffers.data());
    glGenRenderbuffers(n, sc.depth.data());
    for (uint32_t i = 0; i < n; ++i) {
        glBindRenderbuffer(GL_RENDERBUFFER, sc.depth[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, sc.framebuffers[i]);
        if (samples > 1) {
            // Tile-based GPUs resolve MSAA on-chip with this extension: no extra bandwidth.
            glRenderbufferStorageMultisampleEXT_(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
            glFramebufferTexture2DMultisampleEXT_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                                  sc.images[i].image, 0, samples);
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[i].image, 0);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth[i]);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("eye %d framebuffer %u incomplete", eye, i);
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("eye %d swapchain %dx%d, %u images, %dx MSAA", eye, width, height, n, samples);
    return true;
}

bool XrPlatform::CreateActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(instance_, &asci, &actionSet_));

    xrStringToPath(instance_, "/user/hand/left", &hands_[0]);
    xrStringToPath(instance_, "/user/hand/right", &hands_[1]);

    auto make = [&](XrAction *a, const char *name, XrActionType type, bool bothHands) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, name);
        if (bothHands) {
            ci.countSubactionPaths = 2;
            ci.subactionPaths = hands_;
        }
        return XR_SUCCEEDED(xrCreateAction(actionSet_, &ci, a));
    };
    bool ok = make(&gripPose_, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
              make(&aimPose_, "aim_pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
              make(&trigger_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              make(&squeeze_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              make(&stick_, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) &&
              make(&stickClick_, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
              make(&buttonA_, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(&buttonB_, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(&buttonX_, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(&buttonY_, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(&menu_, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(&haptic_, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    if (!ok) return false;

    std::vector<XrActionSuggestedBinding> b;
    auto bind = [&](XrAction a, const char *path) {
        XrPath p;
        xrStringToPath(instance_, path, &p);
        b.push_back({a, p});
    };
    for (const char *side : {"left", "right"}) {
        std::string h = std::string("/user/hand/") + side + "/";
        bind(gripPose_, (h + "input/grip/pose").c_str());
        bind(aimPose_, (h + "input/aim/pose").c_str());
        bind(trigger_, (h + "input/trigger/value").c_str());
        bind(squeeze_, (h + "input/squeeze/value").c_str());
        bind(stick_, (h + "input/thumbstick").c_str());
        bind(stickClick_, (h + "input/thumbstick/click").c_str());
        bind(haptic_, (h + "output/haptic").c_str());
    }
    bind(buttonA_, "/user/hand/right/input/a/click");
    bind(buttonB_, "/user/hand/right/input/b/click");
    bind(buttonX_, "/user/hand/left/input/x/click");
    bind(buttonY_, "/user/hand/left/input/y/click");
    bind(menu_, "/user/hand/left/input/menu/click");

    XrPath profile;
    xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &profile);
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = profile;
    sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
    sb.suggestedBindings = b.data();
    XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &sb));

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.subactionPath = hands_[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        sci.action = gripPose_;
        XR_CHECK(xrCreateActionSpace(session_, &sci, &gripSpace_[h]));
        sci.action = aimPose_;
        XR_CHECK(xrCreateActionSpace(session_, &sci, &aimSpace_[h]));
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &actionSet_;
    XR_CHECK(xrAttachSessionActionSets(session_, &ai));
    return true;
}

void XrPlatform::PollEvents(bool *exit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *sc = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
            state_ = sc->state;
            LOGI("session state %d", state_);
            switch (state_) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) running_ = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(session_);
                running_ = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                *exit = true;
                break;
            default: break;
            }
            focused_ = state_ == XR_SESSION_STATE_FOCUSED;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            *exit = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void XrPlatform::SyncInput(XrFrameInput *in, XrTime time) {
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &si))) return;

    auto boolean = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session_, &gi, &s);
        return s.isActive && s.currentState;
    };
    auto value = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session_, &gi, &s);
        return s.isActive ? s.currentState : 0.0f;
    };
    ControllerState &c = in->buttons;
    c.a = boolean(buttonA_, XR_NULL_PATH);
    c.b = boolean(buttonB_, XR_NULL_PATH);
    c.x = boolean(buttonX_, XR_NULL_PATH);
    c.y = boolean(buttonY_, XR_NULL_PATH);
    c.menu = boolean(menu_, XR_NULL_PATH);
    for (int h = 0; h < 2; ++h) {
        c.trigger[h] = value(trigger_, hands_[h]);
        c.grip[h] = value(squeeze_, hands_[h]);
        c.stickClick[h] = boolean(stickClick_, hands_[h]);
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = hands_[h];
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(session_, &gi, &v);
        c.stickX[h] = v.isActive ? v.currentState.x : 0.0f;
        c.stickY[h] = v.isActive ? v.currentState.y : 0.0f;

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(gripSpace_[h], stage_, time, &loc);
        in->gripValid[h] = PoseValid(loc.locationFlags);
        in->grip[h] = ToPose(loc.pose);
        loc = {XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(aimSpace_[h], stage_, time, &loc);
        in->aimValid[h] = PoseValid(loc.locationFlags);
        in->aim[h] = ToPose(loc.pose);
    }
}

bool XrPlatform::BeginFrame(XrFrameInput *in) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    frameState_ = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &wi, &frameState_))) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &bi))) return false;

    in->time = static_cast<double>(frameState_.predictedDisplayTime) * 1e-9;
    in->dt = lastTime_ > 0 ? static_cast<float>(std::min(in->time - lastTime_, 0.1)) : 1.0f / 72.0f;
    lastTime_ = in->time;
    in->shouldRender = frameState_.shouldRender;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = frameState_.predictedDisplayTime;
    vli.space = stage_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    xrLocateViews(session_, &vli, &vs, 2, &n, views_);
    for (int eye = 0; eye < 2; ++eye) {
        in->eyes[eye].pose = ToPose(views_[eye].pose);
        in->eyes[eye].fov = views_[eye].fov;
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    xrLocateSpace(viewSpace_, stage_, frameState_.predictedDisplayTime, &head);
    in->headValid = PoseValid(head.locationFlags);
    in->head = ToPose(head.pose);
    SyncInput(in, frameState_.predictedDisplayTime);
    return true;
}

EyeTarget XrPlatform::AcquireEye(int eye) {
    Swapchain &sc = swapchains_[eye];
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &ai, &sc.acquired);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, sc.framebuffers[sc.acquired]);
    glViewport(0, 0, sc.width, sc.height);
    return {sc.framebuffers[sc.acquired], sc.width, sc.height};
}

void XrPlatform::ReleaseEye(int eye) {
    // Depth is never read back; telling the driver avoids storing it to memory.
    const GLenum discard = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discard);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchains_[eye].handle, &ri);
}

void XrPlatform::EndFrame(bool passthrough) {
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (int eye = 0; eye < 2; ++eye) {
        pv[eye].pose = views_[eye].pose;
        pv[eye].fov = views_[eye].fov;
        pv[eye].subImage.swapchain = swapchains_[eye].handle;
        pv[eye].subImage.imageRect = {{0, 0}, {swapchains_[eye].width, swapchains_[eye].height}};
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj.space = stage_;
    proj.viewCount = 2;
    proj.views = pv;
    XrCompositionLayerPassthroughFB pt{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};

    std::vector<const XrCompositionLayerBaseHeader *> layers;
    if (passthrough && passthroughOn_ && passthroughLayer_) {
        pt.layerHandle = passthroughLayer_;
        pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&pt));
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    }
    if (frameState_.shouldRender) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj));

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frameState_.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    xrEndFrame(session_, &ei);
}

void XrPlatform::Haptic(int hand, float amplitude, float seconds) {
    if (amplitude <= 0.0f || seconds <= 0.0f) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = std::min(amplitude, 1.0f);
    v.duration = static_cast<XrDuration>(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = hands_[hand];
    xrApplyHapticFeedback(session_, &hi, reinterpret_cast<XrHapticBaseHeader *>(&v));
}

void XrPlatform::SetRefreshRate(float hz) {
    if (!xrRequestDisplayRefreshRateFB_ || hz == refreshRate_) return;
    if (XR_SUCCEEDED(xrRequestDisplayRefreshRateFB_(session_, hz))) LOGI("refresh rate %.0f Hz", hz);
    refreshRate_ = hz;
}

void XrPlatform::SetPassthrough(bool on) {
    if (!passthroughSupported_ || on == passthroughOn_) return;
    if (on && !passthrough_) {
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        if (XR_FAILED(xrCreatePassthroughFB_(session_, &pci, &passthrough_))) return;
        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = passthrough_;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        if (XR_FAILED(xrCreatePassthroughLayerFB_(session_, &lci, &passthroughLayer_))) return;
    }
    if (on) {
        xrPassthroughStartFB_(passthrough_);
        xrPassthroughLayerResumeFB_(passthroughLayer_);
    } else if (passthrough_) {
        xrPassthroughLayerPauseFB_(passthroughLayer_);
        xrPassthroughPauseFB_(passthrough_);
    }
    passthroughOn_ = on;
}

void XrPlatform::SetFoveation(int level) {
    if (!xrCreateFoveationProfileFB_ || !xrUpdateSwapchainFB_ || level == foveation_) return;
    XrFoveationLevelProfileCreateInfoFB lp{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    lp.level = static_cast<XrFoveationLevelFB>(std::clamp(level, 0, 3));
    lp.verticalOffset = 0;
    lp.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    XrFoveationProfileCreateInfoFB pci{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    pci.next = &lp;
    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateFoveationProfileFB_(session_, &pci, &profile))) return;
    for (Swapchain &sc : swapchains_) {
        XrSwapchainStateFoveationFB st{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
        st.profile = profile;
        xrUpdateSwapchainFB_(sc.handle, reinterpret_cast<XrSwapchainStateBaseHeaderFB *>(&st));
    }
    xrDestroyFoveationProfileFB_(profile);
    foveation_ = level;
}

void XrPlatform::Shutdown() {
    for (Swapchain &sc : swapchains_) {
        if (!sc.framebuffers.empty()) glDeleteFramebuffers(static_cast<GLsizei>(sc.framebuffers.size()), sc.framebuffers.data());
        if (!sc.depth.empty()) glDeleteRenderbuffers(static_cast<GLsizei>(sc.depth.size()), sc.depth.data());
        if (sc.handle) xrDestroySwapchain(sc.handle);
        sc = Swapchain();
    }
    if (session_) xrDestroySession(session_);
    if (instance_) xrDestroyInstance(instance_);
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
}

}  // namespace khvr
