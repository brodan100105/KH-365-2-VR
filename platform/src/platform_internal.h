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

// platform.cpp: a guest thread hit a wasm trap (bad memory access, unreachable code).
void PlatformReportTrap(const char *where);

// log.cpp
void LogOpen(const char *path);

}  // namespace khvr::hle
