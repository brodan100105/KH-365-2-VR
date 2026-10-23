// OpenXR on Quest: instance and session, one stereo swapchain per eye, Touch controller
// actions, haptics, and the Meta extensions the settings use (refresh rate, passthrough,
// fixed foveation). Everything game- and mode-specific lives in VrApp.
#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

#include "khvr/input_mapper.h"
#include "khvr/math.h"

struct android_app;

namespace khvr {

struct XrEye {
    Pose pose;          // in the stage (floor-level tracking) space
    XrFovf fov;
};

struct XrFrameInput {
    double time = 0.0;          // seconds, from the predicted display time
    float dt = 0.0f;
    bool shouldRender = false;
    XrEye eyes[2];
    Pose head;
    bool headValid = false;
    Pose grip[2], aim[2];
    bool gripValid[2] = {false, false}, aimValid[2] = {false, false};
    ControllerState buttons;
};

struct EyeTarget {
    GLuint framebuffer = 0;
    int width = 0, height = 0;
};

class XrPlatform {
public:
    bool Init(android_app *app, EGLDisplay display, EGLConfig config, EGLContext context,
              float resolutionScale, int msaaSamples);
    void Shutdown();

    // Handles session state changes. `exit` becomes true when the app should quit.
    void PollEvents(bool *exit);
    bool sessionRunning() const { return running_; }
    bool focused() const { return focused_; }

    bool BeginFrame(XrFrameInput *in);
    // Acquires this frame's image of `eye` and binds its framebuffer.
    EyeTarget AcquireEye(int eye);
    void ReleaseEye(int eye);
    void EndFrame(bool passthrough);

    void Haptic(int hand, float amplitude, float seconds);
    void SetRefreshRate(float hz);
    void SetPassthrough(bool on);
    void SetFoveation(int level);  // 0 off .. 3 high

private:
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int width = 0, height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers, depth;
        uint32_t acquired = 0;
    };
    bool CreateActions();
    bool CreateSwapchain(int eye, int width, int height, int samples);
    void SyncInput(XrFrameInput *in, XrTime time);
    bool HasExtension(const char *name) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace stage_ = XR_NULL_HANDLE, viewSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false, focused_ = false;
    std::vector<std::string> extensions_;
    Swapchain swapchains_[2];
    XrView views_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrFrameState frameState_{XR_TYPE_FRAME_STATE};
    double lastTime_ = 0.0;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE, stick_ = XR_NULL_HANDLE;
    XrAction stickClick_ = XR_NULL_HANDLE, buttonA_ = XR_NULL_HANDLE, buttonB_ = XR_NULL_HANDLE;
    XrAction buttonX_ = XR_NULL_HANDLE, buttonY_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE;
    XrPath hands_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace gripSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    // XR_FB_passthrough
    bool passthroughSupported_ = false, passthroughOn_ = false;
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
    // XR_FB_display_refresh_rate
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_ = nullptr;
    float refreshRate_ = 0.0f;
    // XR_FB_foveation
    PFN_xrCreateFoveationProfileFB xrCreateFoveationProfileFB_ = nullptr;
    PFN_xrDestroyFoveationProfileFB xrDestroyFoveationProfileFB_ = nullptr;
    PFN_xrUpdateSwapchainFB xrUpdateSwapchainFB_ = nullptr;
    int foveation_ = -1;
};

}  // namespace khvr
