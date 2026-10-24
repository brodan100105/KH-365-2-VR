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
