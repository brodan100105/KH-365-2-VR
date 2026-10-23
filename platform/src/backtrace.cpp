// Native backtraces of the calling thread. Guest functions are native functions here (wasm2c
// keeps the decomp's names), so a backtrace taken where the game stopped shows the game's own
// call chain: by name in the desktop runner, as library offsets on the headset, which
// tools/symbolize.py resolves with the unstripped library CI keeps.
#include <dlfcn.h>
#include <unwind.h>

#include <cstdio>
#include <string>

#include "hle.h"

namespace khvr::hle {

namespace {
struct Frames {
    void *pcs[48];
    int n = 0;
};

_Unwind_Reason_Code Collect(struct _Unwind_Context *ctx, void *arg) {
    auto *f = static_cast<Frames *>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && f->n < 48) f->pcs[f->n++] = reinterpret_cast<void *>(pc);
    return f->n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
}  // namespace

void LogBacktrace(const char *why) {
    Frames f;
    _Unwind_Backtrace(Collect, &f);
    Log("backtrace (%s), %d frames:", why, f.n);
    for (int i = 1; i < f.n; ++i) {
        Dl_info info{};
        char line[256];
        if (dladdr(f.pcs[i], &info) && info.dli_fname) {
            const char *lib = info.dli_fname;
            for (const char *p = lib; *p; ++p)
                if (*p == '/') lib = p + 1;
            uintptr_t off = reinterpret_cast<uintptr_t>(f.pcs[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase);
            std::snprintf(line, sizeof line, "  #%02d %s+0x%zx %s", i, lib, static_cast<size_t>(off),
                          info.dli_sname ? info.dli_sname : "");
        } else {
            std::snprintf(line, sizeof line, "  #%02d %p", i, f.pcs[i]);
        }
        Log("%s", line);
    }
}

}  // namespace khvr::hle

namespace khvr::hle {
namespace {
thread_local const char *tLastTrap = "unknown";
}
const char *LastTrapName() { return tLastTrap; }
}  // namespace khvr::hle

extern "C" void khvr_on_trap(int code) {
    static const char *const kNames[] = {"none", "out-of-bounds memory access", "integer overflow",
                                         "division by zero", "invalid float conversion", "unreachable code",
                                         "indirect call to a bad function", "uncaught exception",
                                         "unaligned atomic", "call stack exhausted", "null reference"};
    khvr::hle::tLastTrap = code >= 0 && code < 11 ? kNames[code] : "unknown";
    khvr::hle::Log("trap: %s", khvr::hle::tLastTrap);
    khvr::hle::LogBacktrace("trap");
}

extern "C" void w2c_env_khvr_host_terminate(struct w2c_env *) {
    khvr::hle::Log("game: OS_Terminate - the game stopped on a fatal error");
    khvr::hle::LogBacktrace("OS_Terminate");
    khvr::hle::PlatformReportStop("the game stopped on a fatal error (OS_Terminate)");
}
