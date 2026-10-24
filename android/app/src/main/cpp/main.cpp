// Android entry point: EGL context, OpenXR session, and the frame loop.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <cstdlib>
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

// The shared folder for the ROM, the save and the boot log: SideQuest and adb can always write
// there, unlike Android/data on recent Quest system versions.
constexpr const char *kSharedDir = "/storage/emulated/0/KHDaysVR";

bool ClearException(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

// Environment.isExternalStorageManager(): the "All files access" permission (Android 11+).
bool HasAllFilesAccess(android_app *app) {
    JNIEnv *env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass envClass = env->FindClass("android/os/Environment");
    jmethodID isManager = envClass ? env->GetStaticMethodID(envClass, "isExternalStorageManager", "()Z") : nullptr;
    bool granted = isManager && env->CallStaticBooleanMethod(envClass, isManager);
    if (ClearException(env)) granted = false;
    if (envClass) env->DeleteLocalRef(envClass);
    return granted;
}

// Opens the system page where the user turns on "All files access" for this app.
void RequestAllFilesAccess(android_app *app) {
    JNIEnv *env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass uriClass = env->FindClass("android/net/Uri");
    jclass intentClass = env->FindClass("android/content/Intent");
    jclass activityClass = env->GetObjectClass(app->activity->clazz);
    if (!uriClass || !intentClass || ClearException(env)) return;
    jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID intentWithUri = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jmethodID intentPlain = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID start = env->GetMethodID(activityClass, "startActivity", "(Landroid/content/Intent;)V");
    if (ClearException(env)) return;
    jstring pkg = env->NewStringUTF("package:com.khdays.vr");
    jobject uri = env->CallStaticObjectMethod(uriClass, parse, pkg);
    jstring perApp = env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jobject intent = env->NewObject(intentClass, intentWithUri, perApp, uri);
    env->CallVoidMethod(app->activity->clazz, start, intent);
    if (ClearException(env)) {
        // No per-app page on this system: the list of all apps instead.
        LOGW("per-app all-files access page unavailable, opening the list");
        jstring all = env->NewStringUTF("android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION");
        jobject intent2 = env->NewObject(intentClass, intentPlain, all);
        env->CallVoidMethod(app->activity->clazz, start, intent2);
        if (ClearException(env)) LOGE("cannot open the all-files access settings");
    }
}

void HandleCmd(android_app *app, int32_t cmd) {
    auto *st = static_cast<AppState *>(app->userData);
    switch (cmd) {
    case APP_CMD_RESUME: st->resumed = true; break;
    case APP_CMD_PAUSE: st->resumed = false; break;
    case APP_CMD_DESTROY: st->destroyRequested = true; break;
    default: break;
    }
}

// The app's whole life on one activity. Its locals (the game among them, whose destructor
// flushes the save) are gone when this returns.
void RunApp(android_app *app) {
    AppState state;
    app->userData = &state;
    app->onAppCmd = HandleCmd;

    // Settings live where `adb pull/push` can reach them, like the game files will.
    std::string filesDir = app->activity->externalDataPath ? app->activity->externalDataPath
                                                           : app->activity->internalDataPath;
    khvr::VrApp vr;
    vr.LoadSettings(filesDir);
    bool allFiles = HasAllFilesAccess(app);
    LOGI("all-files access: %s", allFiles ? "granted" : "not granted");
    vr.SetSharedDir(kSharedDir, allFiles);

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
    if (vr.needsStorageAccess()) RequestAllFilesAccess(app);

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

}  // namespace

void android_main(android_app *app) {
    RunApp(app);
    app->activity->vm->DetachCurrentThread();
    // Android may keep the process and start android_main again for a new activity; the game's
    // platform can only run once per process (its guest threads live on), so end the process
    // here (RunApp's game has flushed its save) and every launch starts fresh.
    LOGI("activity gone: exiting the process");
    std::exit(0);
}
