// Guest threads and interrupts.
//
// The SDK schedules its threads itself (OSi_RescheduleThread) and switches between them with
// OS_SaveContext / OS_LoadContext, which are register save and restore on the DS. Here every
// guest thread runs on its own host thread, and a baton says which one may run: "load context X"
// hands the baton to X's host thread and parks the current one until someone loads its context
// again. The SDK always calls `if (OS_SaveContext(cur) == 0) OS_LoadContext(next);`, so a parked
// thread that gets the baton back simply returns from load, which is the same as its save
// returning non-zero.
//
// Interrupts are delivered at the points the hardware could take them: when the idle thread
// halts (the normal case, once per frame for VBlank), and when the guest re-enables interrupts.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <pthread.h>

#include "guest.h"
#include "hle.h"
#include "platform_internal.h"

extern "C" {
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"
}

extern "C" {
// Per-thread guest stack pointer (guest/postprocess.py makes wasm's global thread-local).
thread_local u32 khvr_guest_sp;
}

namespace khvr::hle {

namespace {

struct GuestThread {
    uint32_t context = 0;
    uint32_t entry = 0, arg = 0, sp = 0, lr = 0;
    std::condition_variable cv;
    bool started = false;
    pthread_t host{};
    bool hostKnown = false;
    uint32_t gen = 0;  // the run it belongs to; a restart starts a new one (ThreadsReset)
};

// Guest code nests deeply and wasm2c's frames are large: give every guest thread a big host stack.
constexpr size_t kHostStack = 32u << 20;

void StartHostThread(void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &attr, fn, arg) != 0) Log("thread: pthread_create failed");
    pthread_attr_destroy(&attr);
}

std::mutex gMu;
std::condition_variable gIdleCv;
GuestThread *gRunning = nullptr;
std::unordered_map<uint32_t, GuestThread *> gByContext;
std::vector<GuestThread *> gAll;
thread_local GuestThread *tSelf = nullptr;

bool gIdle = false;        // the idle thread is halted: the guest has nothing to do
uint64_t gHalts = 0;       // how many times it halted
bool gWake = false;        // an event (VBlank) arrived for the halted guest
bool gStopping = false;
int gIrqNesting = 0;
uint32_t gGen = 0;         // the current run

// A thread of an earlier run (before a restart): it must never touch the guest again.
bool Stale() { return tSelf && tSelf->gen != gGen; }

// OSThreadInfo (decomp: data_0204430c +0x24): isNeedRescheduling, irqDepth.
uint32_t gThreadInfo = 0;

// KHVR_TRACE=threads logs every context switch with the SDK's scheduler state.
bool TraceThreads() {
    static const bool on = [] {
        const char *t = std::getenv("KHVR_TRACE");
        return t && std::strstr(t, "threads");
    }();
    return on;
}

void TraceSwitch(const char *what, uint32_t context) {
    if (!TraceThreads()) return;
    uint32_t g = SymbolAddress("data_0204430c");
    uint32_t curPtr = Read32(g + 0x08);
    uint32_t cur = curPtr ? Read32(curPtr) : 0;
    Log("trace: %s ctx 0x%08x | cur 0x%08x (state %u id %u) resched %u irqDepth %u needResched %u",
        what, context, cur, cur ? Read32(cur + 0x64) : 0, cur ? Read32(cur + 0x6c) : 0, Read32(g + 0x04),
        Read16(g + 0x26), Read16(g + 0x24));
}

void WaitForBaton(std::unique_lock<std::mutex> &lock) {
    tSelf->cv.wait(lock, [] { return gRunning == tSelf || gStopping || Stale(); });
    if ((gStopping && gRunning != tSelf) || Stale()) {
        // Shutting down: a parked thread must not run guest code alongside the others (they
        // would all fall through their switches at once). Stay parked until the process ends.
        lock.unlock();
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
}

void RunThread(GuestThread *t);

void HandTo(GuestThread *next, std::unique_lock<std::mutex> &lock) {
    gRunning = next;
    if (!next->started) {
        next->started = true;
        StartHostThread([](void *p) -> void * { RunThread(static_cast<GuestThread *>(p)); return nullptr; }, next);
    } else {
        next->cv.notify_one();
    }
}

}  // namespace

namespace {
uint32_t CpsrAddr() {
    static uint32_t addr = SymbolAddress("khvr_cpsr_i");
    return addr;
}
}  // namespace

bool GuestIrqsDisabled() { return CpsrAddr() && (Read32(CpsrAddr()) & 0x80); }

// The CPSR's I and F bits belong to each thread, as on the hardware: a thread that switches away
// with interrupts disabled must not leave them disabled for the next one. They live in the
// OSContext's cpsr word (+0) while the thread is switched out.
void SaveCpsrTo(uint32_t context) {
    if (!CpsrAddr() || !context || context == 0xffffffffu) return;
    Write32(context, (Read32(context) & ~0xc0u) | (Read32(CpsrAddr()) & 0xc0));
}

void LoadCpsrFrom(uint32_t context) {
    if (!CpsrAddr() || !context || context == 0xffffffffu) return;
    Write32(CpsrAddr(), (Read32(CpsrAddr()) & ~0xc0u) | (Read32(context) & 0xc0));
}

void RaiseIrq(uint32_t bits) { RegIf() |= bits; }

void DispatchIrqs() {
    if (!(RegIme() & 1) || gIrqNesting) return;
    if (!gThreadInfo) gThreadInfo = SymbolAddress("data_0204430c") + 0x24;
    ++gIrqNesting;
    Write16(gThreadInfo + 2, Read16(gThreadInfo + 2) + 1);  // irqDepth: rescheduling waits
    for (int guard = 0; guard < 32; ++guard) {
        uint32_t pending = RegIf() & RegIe();
        if (!pending) break;
        int bit = __builtin_ctz(pending);
        RegIf() &= ~(1u << bit);
        IoOnIrqTaken(bit);
        // OS_IRQTable lives at the start of DTCM (guest/build_guest.py).
        uint32_t handler = Read32(0x027e0000 + 4 * bit);
        if (handler) CallGuest(handler);
    }
    // OS_IrqHandler_ThreadSwitch: wake the threads waiting in OS_WaitIrq (their reschedule is
    // deferred while irqDepth is raised), then switch threads if anything asked for it.
    static const uint32_t irqQueue = SymbolAddress("data_027e006c");
    if (irqQueue && Read32(irqQueue)) w2c_khdays_OS_WakeupThread(Guest(), irqQueue);
    Write16(gThreadInfo + 2, Read16(gThreadInfo + 2) - 1);
    --gIrqNesting;
    if (Read16(gThreadInfo + 2) == 0 && Read16(gThreadInfo)) {
        Write16(gThreadInfo, 0);
        w2c_khdays_OSi_RescheduleThread(Guest());
    }
}

namespace {

// A guest thread's whole life on its host thread.
void RunThread(GuestThread *t) {
    {
        std::unique_lock<std::mutex> lock(gMu);
        tSelf = t;
        t->host = pthread_self();
        t->hostKnown = true;
        WaitForBaton(lock);
        if (gStopping) return;
    }
    khvr_guest_sp = t->sp;
    LoadCpsrFrom(t->context);  // OS_InitContext: interrupts enabled
    if (wasm_rt_impl_try() == 0) {
        CallGuest(t->entry, t->arg);
        // OS_CreateThread puts OS_ExitThread in lr; returning from the entry ends the thread.
        if (t->lr) CallGuest(t->lr);
    } else {
        PlatformReportTrap("guest thread");
    }
    // An exited thread never gets the baton again; park until shutdown.
    std::unique_lock<std::mutex> lock(gMu);
    tSelf->cv.wait(lock, [] { return gStopping || Stale(); });
}

}  // namespace

// ---- Imports the guest calls (guest/include/khvr_host.h) ----

extern "C" {

u32 w2c_env_khvr_host_save_context(struct w2c_env *, u32 context) {
    TraceSwitch("save", context);
    std::lock_guard<std::mutex> lock(gMu);
    // Each host thread runs one guest thread for life: its OSThread (context) is bound at its
    // first save (the main thread) or at its start. A save of another context means the SDK's
    // current thread and the running host thread disagree; never remap on it, or a later load of
    // that context would land on this thread and do nothing.
    if (tSelf->context == 0) {
        tSelf->context = context;
        gByContext[context] = tSelf;
    } else if (tSelf->context != context) {
        static int reported = 0;
        if (reported++ < 5) {
            Log("thread: save of 0x%08x on the host thread of 0x%08x (SDK and host disagree)", context,
                tSelf->context);
            LogBacktrace("thread mismatch");
        }
    }
    return 0;
}

void w2c_env_khvr_host_load_context(struct w2c_env *, u32 context) {
    TraceSwitch("load", context);
    std::unique_lock<std::mutex> lock(gMu);
    // Leaving a thread that has exited (OSi_ExitThread_Destroy marked it TERMINATED and skipped
    // its save): it never runs again, and its OSThread memory may become a new thread later.
    if (tSelf->context && tSelf->context != context && Read32(tSelf->context + 0x64) == 2) {
        auto own = gByContext.find(tSelf->context);
        if (own != gByContext.end() && own->second == tSelf) gByContext.erase(own);
        tSelf->context = 0xffffffffu;  // bound to nothing from now on
    }
    GuestThread *next;
    auto it = gByContext.find(context);
    if (it != gByContext.end()) {
        next = it->second;
    } else {
        // A thread OS_CreateThread set up with OS_InitContext: start it on a new host thread.
        next = new GuestThread;
        next->gen = gGen;
        next->context = context;
        next->entry = Read32(context + 0x40) - 4;  // pc_plus4
        next->arg = Read32(context + 0x04);        // r0
        next->sp = Read32(context + 0x38);
        next->lr = Read32(context + 0x3c);
        gByContext[context] = next;
        gAll.push_back(next);
        const char *fn = FunctionName(next->entry);
        if (fn) Log("thread: start context 0x%08x entry %s sp 0x%08x", context, fn, next->sp);
        else Log("thread: start context 0x%08x entry %u sp 0x%08x", context, next->entry, next->sp);
    }
    if (next == tSelf) {
        if (tSelf->context != context) Log("thread: load of 0x%08x maps to the running host thread", context);
        return;
    }
    SaveCpsrTo(tSelf->context);
    HandTo(next, lock);
    WaitForBaton(lock);
    LoadCpsrFrom(tSelf->context);
}

void w2c_env_khvr_host_halt(struct w2c_env *) {
    {
        std::unique_lock<std::mutex> lock(gMu);
        gIdle = true;
        ++gHalts;
        gIdleCv.notify_all();
        gIdleCv.wait(lock, [] { return gWake || gStopping || Stale(); });
        if (Stale()) {
            lock.unlock();
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        }
        gWake = false;
        gIdle = false;
        if (gStopping) {
            lock.unlock();
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    DispatchIrqs();
}

void w2c_env_khvr_host_irq_window(struct w2c_env *) {
    if (RegIf() & RegIe()) DispatchIrqs();
}

void w2c_env_khvr_host_spin(struct w2c_env *) {
    if ((RegIf() & RegIe()) && !GuestIrqsDisabled()) DispatchIrqs();
}

void w2c_env_khvr_host_reset(struct w2c_env *) {
    PlatformRequestReset();
    ThreadsParkForever();
}

void w2c_env_khvr_host_print(struct w2c_env *, u32 text) { Log("guest: %s", ReadString(text).c_str()); }

void w2c_env_khvr_host_watch(struct w2c_env *, u32 addr, u32 size) { WatchSet(addr, size, "asked by the guest"); }

void w2c_env_khvr_host_trace(struct w2c_env *, u32 what, u32 a, u32 b) {
    if (ReadString(what) == "scene request") SnapshotOnSceneRequest(a);
    const char *fn = FunctionName(b);
    // A small number can be both a value and a function's table index: the value is shown too.
    if (fn) Log("trace: %s %u -> %s (0x%08x)", ReadString(what).c_str(), a, fn, b);
    else Log("trace: %s %u -> 0x%08x", ReadString(what).c_str(), a, b);
}

}  // extern "C"

// ---- The platform's side ----

void ThreadsStartMain(uint32_t initialSp) {
    auto *t = new GuestThread;
    t->gen = gGen;
    t->started = true;
    t->sp = initialSp;
    gAll.push_back(t);
    {
        std::lock_guard<std::mutex> lock(gMu);
        gRunning = t;
    }
    StartHostThread(
        [](void *p) -> void * {
            tSelf = static_cast<GuestThread *>(p);
            {
                std::lock_guard<std::mutex> lock(gMu);
                tSelf->host = pthread_self();
                tSelf->hostKnown = true;
            }
            khvr_guest_sp = tSelf->sp;
            if (wasm_rt_impl_try() == 0) {
                Log("boot: entering main()");
                w2c_khdays_main(Guest(), 0, 0);
                Log("boot: main() returned");
            } else {
                PlatformReportTrap("main thread");
            }
            std::unique_lock<std::mutex> lock(gMu);
            tSelf->cv.wait(lock, [] { return gStopping || Stale(); });
            return nullptr;
        },
        t);
}

bool ThreadsWaitIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lock(gMu);
    return gIdleCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return gIdle || gStopping; });
}

bool ThreadsIsIdle() {
    std::lock_guard<std::mutex> lock(gMu);
    return gIdle;
}

uint64_t ThreadsWakeIdle() {
    std::lock_guard<std::mutex> lock(gMu);
    gWake = true;
    gIdle = false;
    gIdleCv.notify_all();
    return gHalts;
}

bool ThreadsWaitHaltAfter(uint64_t halts, int timeoutMs) {
    std::unique_lock<std::mutex> lock(gMu);
    return gIdleCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [halts] { return (gHalts > halts && gIdle) || gStopping; });
}

namespace {
// SIGUSR2 on the running guest thread: it records its own call chain for the watchdog.
void *gSamplePcs[48];
std::atomic<int> gSampleCount{-1};

void OnSampleSignal(int) { gSampleCount = CaptureBacktrace(gSamplePcs, 48); }
}  // namespace

ThreadsSnapshot ThreadsSnapshotNow() {
    std::lock_guard<std::mutex> lock(gMu);
    ThreadsSnapshot s;
    s.idle = gIdle;
    s.halts = gHalts;
    s.threads = static_cast<int>(gAll.size());
    s.runningContext = gRunning ? gRunning->context : 0;
    return s;
}

void ThreadsLogRunning(const char *why) {
    static bool installed = [] {
        struct sigaction sa {};
        sa.sa_handler = OnSampleSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        return sigaction(SIGUSR2, &sa, nullptr) == 0;
    }();
    pthread_t target{};
    uint32_t context = 0;
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (!installed || !gRunning || !gRunning->hostKnown || gIdle) return;
        target = gRunning->host;
        context = gRunning->context;
    }
    gSampleCount = -1;
    if (pthread_kill(target, SIGUSR2) != 0) return;
    for (int i = 0; i < 100 && gSampleCount < 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int n = gSampleCount;
    if (n < 0) {
        Log("watchdog: guest thread (context 0x%08x) did not answer the sample", context);
        return;
    }
    char what[96];
    std::snprintf(what, sizeof what, "%s, guest thread context 0x%08x", why, context);
    LogFrames(what, gSamplePcs, n, 0);
}

bool ThreadsFinishExit() {
    // OSi_RescheduleThread's view of the scheduler (decomp: libs/nitro/os/calls/OSi_RescheduleThread.c).
    static const uint32_t g = SymbolAddress("data_0204430c");
    if (!g) return false;
    const uint32_t cur = Read32(g + 0x28);
    if (!cur || Read32(cur + 0x64) != 2) return false;  // not a thread on its way out (TERMINATED)

    Log("thread: exit of 0x%08x did not switch threads | rescheduleCount %u irqDepth %u needResched %u",
        cur, Read32(g + 0x04), Read16(g + 0x26), Read16(g + 0x24));
    uint32_t idle = 0, idlePrio = 0;
    std::string list;
    uint32_t t = Read32(g + 0x2c);
    for (int i = 0; t && i < 32; ++i, t = Read32(t + 0x68)) {
        char item[64];
        uint32_t state = Read32(t + 0x64), prio = Read32(t + 0x70);
        std::snprintf(item, sizeof item, " 0x%08x(id %u prio %u state %u)", t, Read32(t + 0x6c), prio, state);
        list += item;
        if (prio >= idlePrio) {
            idle = t;
            idlePrio = prio;
        }
    }
    Log("thread: list:%s", list.c_str());

    // On the DS the exiting thread can never run again: finish its switch by hand. The next
    // thread is the SDK's own choice, or the lowest-priority one (the idle thread) if none is ready.
    uint32_t next = w2c_khdays_OSi_SelectThread(Guest());
    if (!next) next = idle;
    if (!next || next == cur) return false;
    Log("thread: switching to 0x%08x by hand", next);
    if (uint32_t cb = Read32(g + 0x00)) CallGuest(cb, cur, next);  // systemCallback
    if (uint32_t cb = Read32(g + 0x30)) CallGuest(cb, cur, next);  // switchCallback
    Write32(g + 0x28, next);
    w2c_env_khvr_host_load_context(nullptr, next);  // parks this host thread for good
    return false;
}

void ThreadsReset() {
    std::lock_guard<std::mutex> lock(gMu);
    ++gGen;
    // The old run's threads wake, see they are stale and park. Their GuestThreads stay allocated
    // (a parked host thread still points at its own), so no new thread can share an address.
    for (GuestThread *t : gAll) t->cv.notify_all();
    gIdleCv.notify_all();
    gAll.clear();
    gByContext.clear();
    gRunning = nullptr;
    gIdle = false;
    gWake = false;
    gIrqNesting = 0;
}

void ThreadsParkForever() {
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

void ThreadsStop() {
    {
        std::lock_guard<std::mutex> lock(gMu);
        gStopping = true;
        gIdleCv.notify_all();
        for (GuestThread *t : gAll) t->cv.notify_all();
    }
    // Guest threads are detached: one may be deep inside guest code and never come back.
}

}  // namespace khvr::hle
