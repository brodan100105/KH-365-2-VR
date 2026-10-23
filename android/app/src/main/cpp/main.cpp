// Android entry point: EGL context, OpenXR session, and the frame loop.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>

#include <string>

#include "log.h"
#include "vr_app.h"
#include "xr_platform.h"

namespace {

struct Egl {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    bool Init() {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(display, nullptr, nullptr)) return false;
        const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                  EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                  EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                  EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(display, attribs, &config, 1, &n) || n == 0) return false;
        const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
        if (context == EGL_NO_CONTEXT) return false;
        // OpenXR renders into its own swapchains; the context only needs a dummy surface.
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pb);
        return eglMakeCurrent(display, surface, surface, context);
    }
    void Shutdown() {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }
};

struct AppState {
    bool resumed = false;
    bool destroyRequested = false;
};

void HandleCmd(android_app *app, int32_t cmd) {
    auto *st = static_cast<AppState *>(app->userData);
    switch (cmd) {
    case APP_CMD_RESUME: st->resumed = true; break;
    case APP_CMD_PAUSE: st->resumed = false; break;
    case APP_CMD_DESTROY: st->destroyRequested = true; break;
    default: break;
    }
}

}  // namespace

void android_main(android_app *app) {
    AppState state;
    app->userData = &state;
    app->onAppCmd = HandleCmd;

    // Settings live where `adb pull/push` can reach them, like the game files will.
    std::string filesDir = app->activity->externalDataPath ? app->activity->externalDataPath
                                                           : app->activity->internalDataPath;
    khvr::VrApp vr;
    vr.LoadSettings(filesDir);

    Egl egl;
    khvr::XrPlatform xr;
    static const int kMsaa[] = {1, 2, 4};
    if (!egl.Init()) {
        LOGE("EGL init failed");
        return;
    }
    if (!xr.Init(app, egl.display, egl.config, egl.context,
                 vr.settings().Get(khvr::SettingId::PicResolutionScale),
                 kMsaa[vr.settings().Int(khvr::SettingId::PicMsaa)]) ||
        !vr.Init(&xr)) {
        LOGE("OpenXR init failed");
        egl.Shutdown();
        return;
    }

    bool exit = false;
    while (!app->destroyRequested && !state.destroyRequested && !exit) {
        // Block for events only while the session is not running, so the headset keeps its
        // frame rate the rest of the time.
        int events;
        android_poll_source *source;
        int timeout = (!state.resumed && !xr.sessionRunning()) ? -1 : 0;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = (!state.resumed && !xr.sessionRunning()) ? -1 : 0;
        }
        xr.PollEvents(&exit);
        if (!xr.sessionRunning()) continue;
        khvr::XrFrameInput in;
        if (!xr.BeginFrame(&in)) continue;
        vr.Frame(in);
        xr.EndFrame(vr.settings().Bool(khvr::SettingId::Passthrough));
    }
    vr.SaveSettingsIfChanged(true);
    xr.Shutdown();
    egl.Shutdown();
}
