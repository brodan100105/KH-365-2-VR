#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hle.h"
#include "khvr/platform.h"
#include "platform_internal.h"

#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <unwind.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace khvr::hle {

namespace {
std::mutex gLogMu;
FILE *gLogFile = nullptr;
std::deque<std::string> gRecent;
std::set<std::string> gOnce;

void Emit(const char *msg) {
    std::lock_guard<std::mutex> lock(gLogMu);
    if (gLogFile) {
        std::fputs(msg, gLogFile);
        std::fputc('\n', gLogFile);
        std::fflush(gLogFile);
    }
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "KHVR-game", msg);
#else
    if (!gLogFile) std::fprintf(stderr, "%s\n", msg);
#endif
    gRecent.emplace_back(msg);
    if (gRecent.size() > 200) gRecent.pop_front();
}
// A native crash (outside the guest's own traps) into the log before the app goes: the signal,
// the address it faulted on and the native call chain (library offsets; tools/symbolize.py).
// Written straight to the file's descriptor: the crash may be inside the logger's own lock.
int gCrashFd = -1;

struct CrashFrames {
    void *pcs[32];
    int n = 0;
};

_Unwind_Reason_Code CrashCollect(struct _Unwind_Context *ctx, void *arg) {
    auto *f = static_cast<CrashFrames *>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && f->n < 32) f->pcs[f->n++] = reinterpret_cast<void *>(pc);
    return f->n < 32 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void CrashWrite(const char *text) {
    if (gCrashFd < 0) return;
    size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t n = write(gCrashFd, text, left);
        if (n <= 0) return;
        text += n;
        left -= static_cast<size_t>(n);
    }
}

void OnCrash(int sig, siginfo_t *info, void *) {
    static volatile sig_atomic_t inside = 0;
    if (!inside) {
        inside = 1;
        char line[256];
        std::snprintf(line, sizeof line, "crash: native signal %d (%s), fault address %p\n", sig,
                      sig == SIGSEGV ? "bad memory access" : sig == SIGBUS ? "bus error" : sig == SIGABRT ? "abort"
                                                                    : sig == SIGILL ? "illegal instruction" : "arithmetic",
                      info ? info->si_addr : nullptr);
        CrashWrite(line);
        CrashFrames f;
        _Unwind_Backtrace(CrashCollect, &f);
        for (int i = 0; i < f.n; ++i) {
            Dl_info dl{};
            if (dladdr(f.pcs[i], &dl) && dl.dli_fname) {
                const char *lib = dl.dli_fname;
                for (const char *p = lib; *p; ++p)
                    if (*p == '/') lib = p + 1;
                std::snprintf(line, sizeof line, "  #%02d %s+0x%zx %s\n", i, lib,
                              static_cast<size_t>(reinterpret_cast<uintptr_t>(f.pcs[i]) -
                                                  reinterpret_cast<uintptr_t>(dl.dli_fbase)),
                              dl.dli_sname ? dl.dli_sname : "");
            } else {
                std::snprintf(line, sizeof line, "  #%02d %p\n", i, f.pcs[i]);
            }
            CrashWrite(line);
        }
    }
    // The system's own handling (the crash report, the app closing) goes on as before.
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction sa {};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}
}  // namespace

void LogOpen(const char *path) {
    std::lock_guard<std::mutex> lock(gLogMu);
    if (gLogFile) std::fclose(gLogFile);
    gLogFile = nullptr;
    if (!path || !*path) return;
    // Keep the previous sessions (a stop is often followed by a relaunch), unless the log has
    // grown past 1 MB.
    long size = 0;
    if (FILE *old = std::fopen(path, "rb")) {
        std::fseek(old, 0, SEEK_END);
        size = std::ftell(old);
        std::fclose(old);
    }
    gLogFile = std::fopen(path, size > (1L << 20) ? "w" : "a");
    if (gLogFile) {
        std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(gLogFile, "\n===== session %s =====\n", when);
        std::fflush(gLogFile);
        gCrashFd = fileno(gLogFile);
        InstallCrashHandler();
    }
}

void Log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Emit(buf);
}

void LogOnce(const char *key, const char *fmt, ...) {
    {
        std::lock_guard<std::mutex> lock(gLogMu);
        if (!gOnce.insert(key).second) return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Emit(buf);
}

std::vector<std::string> RecentLogLines(size_t n) {
    std::lock_guard<std::mutex> lock(gLogMu);
    std::vector<std::string> out;
    size_t start = gRecent.size() > n ? gRecent.size() - n : 0;
    for (size_t i = start; i < gRecent.size(); ++i) out.push_back(gRecent[i]);
    return out;
}

}  // namespace khvr::hle

// A guest import nobody implements yet (guest/postprocess.py makes weak stubs that call this).
extern "C" void khvr_unimplemented_import(const char *name) {
    khvr::hle::LogOnce(name, "guest called %s, which the platform does not implement yet", name);
}
