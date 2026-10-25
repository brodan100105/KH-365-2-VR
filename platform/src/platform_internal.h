#pragma once

#include <cstdint>

namespace khvr::hle {

// hle_thread.cpp
void ThreadsStartMain(uint32_t initialSp);  // runs main() on the first guest thread
bool ThreadsWaitIdle(int timeoutMs);
bool ThreadsIsIdle();
// Wakes the halted guest; returns how many halts happened so far, for ThreadsWaitHaltAfter.
uint64_t ThreadsWakeIdle();
bool ThreadsWaitHaltAfter(uint64_t halts, int timeoutMs);
void ThreadsStop();
// For a restart: every guest thread of the run so far parks for good (none of them touches the
// guest again) and the scheduler starts empty, for ThreadsStartMain.
void ThreadsReset();
// The calling guest thread never runs again (it asked for a reset).
[[noreturn]] void ThreadsParkForever();
struct ThreadsSnapshot {
    bool idle = false;
    uint64_t halts = 0;
    int threads = 0;
    uint32_t runningContext = 0;  // 0 for the main thread
};
ThreadsSnapshot ThreadsSnapshotNow();
// Logs the call chain of the guest thread that holds the baton (not while the guest is idle).
void ThreadsLogRunning(const char *why);
// OS_Terminate reached from a thread's exit path (its last switch did not happen): finish the
// switch the way the DS would. Does not return if it could; false if this was a real stop.
bool ThreadsFinishExit();

// platform.cpp: a guest thread hit a wasm trap (bad memory access, unreachable code).
void PlatformReportTrap(const char *where);
// platform.cpp: the game called OS_ResetSystem.
void PlatformRequestReset();

// log.cpp
void LogOpen(const char *path);

}  // namespace khvr::hle
