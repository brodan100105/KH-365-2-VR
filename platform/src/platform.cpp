#include "khvr/platform.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "guest.h"
#include "hle.h"
#include "hle_gx.h"
#include "platform_internal.h"
#include "render2d.h"

extern "C" thread_local uint32_t khvr_guest_sp;

namespace khvr {

namespace hle {
std::vector<std::string> RecentLogLines(size_t n);

namespace {
InputState gInput;
std::mutex gInputMu;
std::atomic<PlatformState> gState{PlatformState::NotStarted};
std::atomic<uint32_t> gFrames{0};
std::string gCrash;
std::mutex gCrashMu;
// ARM9 cycles per frame: 263 lines x 2130 cycles.
constexpr uint64_t kFrameCycles = 263ull * 2130ull;
}  // namespace

InputState &Input() { return gInput; }

void PlatformReportStop(const std::string &why) {
    {
        std::lock_guard<std::mutex> lock(gCrashMu);
        gCrash = why;
    }
    Log("stop: %s, after %u frames", why.c_str(), gFrames.load());
    gState = PlatformState::Crashed;
}

void PlatformReportTrap(const char *where) {
    PlatformReportStop(std::string(where) + " hit a wasm trap (" + LastTrapName() + ")");
}
}  // namespace hle

Platform &Platform::Get() {
    static Platform p;
    return p;
}

void PlatformInitForTests() {
    wasm_rt_init();
    wasm2c_khdays_instantiate(hle::Guest(), nullptr);
    hle::IoReset();
}

void PlatformTestWrite(uint32_t addr, uint32_t value, uint32_t size) { hle::BusWrite(addr, value, size); }

bool Platform::Start(const PlatformConfig &config, std::string *error) {
    using namespace hle;
    // The guest's threads and memory live for the whole process: a second start would run on
    // the first one's leftovers. The app exits its process when its activity goes away instead.
    static bool started = false;
    if (started) {
        *error = "the game already ran in this process; restart the app";
        return false;
    }
    started = true;
    LogOpen(config.logPath.c_str());
    Log("platform: starting (KH 358/2 Days VR, guest built from khdays-decomp)");
    if (!RomOpen(config.romPath, error)) {
        Log("platform: %s", error->c_str());
        return false;
    }
    SaveOpen(config.savePath);
    RomWriteFileList(config.fileListPath);

    wasm_rt_init();
    wasm2c_khdays_instantiate(Guest(), nullptr);
    Log("platform: guest instantiated, %llu MB of memory", static_cast<unsigned long long>(GuestMemSize() >> 20));
    IoReset();
    Arm7Reset();
    SetupSharedArea(config.language);
    OverlaysInit();
    gState = PlatformState::Running;
    // wasm2c's instantiate set the (thread-local) stack pointer on this thread: hand its initial
    // value to the main guest thread.
    ThreadsStartMain(khvr_guest_sp);
    return true;
}

void Platform::Stop() {
    hle::SaveFlush();
    hle::ThreadsStop();
    if (hle::gState == PlatformState::Running) hle::gState = PlatformState::Stopped;
}

bool Platform::VBlank(int timeoutMs) {
    using namespace hle;
    if (gState != PlatformState::Running) return false;
    // Only deliver a VBlank to a guest that finished its previous frame; a guest still busy
    // (loading, a long frame) keeps running and the VR view keeps the last frame.
    if (!ThreadsWaitIdle(timeoutMs)) return false;
    {
        std::lock_guard<std::mutex> lock(gInputMu);
        Arm7VBlank();
    }
    IoVBlank(kFrameCycles);
    GxVBlank();
    ++gFrames;
    // The idle thread leaves halt, the game runs its frame and halts again.
    uint64_t halts = ThreadsWakeIdle();
    return ThreadsWaitHaltAfter(halts, timeoutMs);
}

void Platform::CaptureScreens(PlatformScreens *out) { hle::Render2D(out); }

void Platform::SetInput(uint16_t buttons, bool touching, int x, int y) {
    std::lock_guard<std::mutex> lock(hle::gInputMu);
    hle::gInput.keys = buttons;
    hle::gInput.touching = touching;
    hle::gInput.touchX = static_cast<uint16_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
    hle::gInput.touchY = static_cast<uint16_t>(y < 0 ? 0 : (y > 191 ? 191 : y));
}

namespace {
// The field's action-script interpreter (Game_RunActionScript, state at root heap + 8): its depth,
// the current entry's command and the bytes around it, for a script that loops within a frame.
void LogActionScript() {
    using namespace hle;
    uint32_t heap = w2c_khdays_NNSi_FndGetCurrentRootHeap(Guest());
    if (heap < 0x02000000 || heap >= 0x02400000) return;
    uint32_t st = heap + 8;
    uint32_t depth = Read32(st + 0x124);
    if (depth > 8) return;
    uint32_t e = st + 4 + depth * 0x48;
    uint32_t cmd = Read32(e + 0x10);
    if (cmd < 0x02000000 || cmd >= 0x02400000) {
        Log("  action script: depth %u, command pointer 0x%08x (not in RAM)", depth, cmd);
        return;
    }
    static const uint32_t table = SymbolAddress("data_020425ec");
    uint32_t op = Read8(cmd), sub = Read8(cmd + 1), word = Read16(cmd + 2);
    uint32_t handlers = table ? Read32(table + 4 * op) : 0;
    uint32_t fn = handlers ? Read32(handlers + 8 * sub) : 0;
    char bytes[3 * 32 + 1] = {0};
    for (int i = 0; i < 32; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", Read8(cmd + i));
    Log("  action script: depth %u, command at 0x%08x: op %u sub %u length %u slot %u, handler fn %u, "
        "action slot %u",
        depth, cmd, op, sub, word & 0x7ff, (word >> 11) & 0x1f, fn, Read32(e + 0x18));
    Log("    bytes: %s", bytes);
}
}  // namespace

void Platform::Heartbeat() {
    using namespace hle;
    static uint64_t lastHalts = ~0ull;
    static int stuckBeats = 0;
    ThreadsSnapshot t = ThreadsSnapshotNow();
    // Display state: an image the 2D renderer cannot show yet (3D on BG0, a fade) still has
    // the engines on, so these tell a stuck game from an invisible one.
    Log("heartbeat: %u frames, %llu halts, %s, %d threads | DISPCNT A %08x B %08x | BRIGHT A %04x B %04x | "
        "POWCNT %04x | VRAMCNT %08x | IME %u IE %08x IF %08x",
        gFrames.load(), static_cast<unsigned long long>(t.halts), t.idle ? "idle" : "busy", t.threads,
        IoReg32(0x0000), IoReg32(0x1000), IoReg16(0x006c), IoReg16(0x106c), IoReg16(0x0304), IoReg32(0x0240),
        RegIme(), RegIe(), RegIf());
    // The background layers' control and scroll registers, both engines: what decides which part
    // of each layer shows.
    for (uint32_t e = 0; e < 2; ++e) {
        uint32_t base = e * 0x1000;
        Log("  engine %c: BGCNT %04x %04x %04x %04x | scroll %u,%u %u,%u %u,%u %u,%u", e ? 'B' : 'A',
            IoReg16(base + 0x08), IoReg16(base + 0x0a), IoReg16(base + 0x0c), IoReg16(base + 0x0e),
            IoReg16(base + 0x10) & 0x1ff, IoReg16(base + 0x12) & 0x1ff, IoReg16(base + 0x14) & 0x1ff,
            IoReg16(base + 0x16) & 0x1ff, IoReg16(base + 0x18) & 0x1ff, IoReg16(base + 0x1a) & 0x1ff,
            IoReg16(base + 0x1c) & 0x1ff, IoReg16(base + 0x1e) & 0x1ff);
    }
    // The VBlank task chain (RegisterNamedTask: name at +4, callback at +0x14, next at +0x18).
    static const uint32_t vblankWork = SymbolAddress("data_027e0088");
    if (vblankWork) {
        std::string chain;
        uint32_t node = Read32(vblankWork + 4);
        for (int i = 0; node && i < 8; ++i, node = Read32(node + 0x18)) chain += " " + ReadString(node + 4);
        Log("  vblank tasks:%s", chain.empty() ? " (none)" : chain.c_str());
    }
    // The opening scene (ov012), while it runs: its flags, movie request states, pending-frame
    // byte, fade counter and timeline position.
    static const uint32_t openingPtr = SymbolAddress("data_ov012_0205cb20");
    if (uint32_t ctx = openingPtr ? Read32(openingPtr) : 0) {
        if (ctx > 0x02000000 && ctx < 0x02400000)
            Log("  opening: flags %04x requests %d %d pending %u fade %d skip-keys %d timeline %d exit %d | movie frame %d",
                Read16(ctx + 2), static_cast<int>(Read32(ctx + 0x8bd8)), static_cast<int>(Read32(ctx + 0x8bdc)),
                Read8(ctx + 0x8be1), static_cast<int>(Read32(ctx + 0x8be8)), static_cast<int>(Read32(ctx + 0x8bec)),
                static_cast<int>(Read32(ctx + 0x8dec)), static_cast<int>(Read32(ctx + 0x130)),
                static_cast<int>(w2c_khdays_Ov024_MobiClip_BufferedFrameCount(Guest())));
    }
    if (!t.idle && t.halts == lastHalts) {
        // No halt since the last beat: the guest is busy (a long load) or stuck in a loop.
        if (++stuckBeats <= 4) {
            ThreadsLogRunning("no frame finished since the last heartbeat");
            LogActionScript();
        }
    } else {
        stuckBeats = 0;
    }
    lastHalts = t.halts;
}

PlatformState Platform::state() const { return hle::gState; }
uint32_t Platform::frames() const { return hle::gFrames; }

std::string Platform::crashReason() const {
    std::lock_guard<std::mutex> lock(hle::gCrashMu);
    return hle::gCrash;
}

std::vector<std::string> Platform::RecentLog(size_t lines) const { return hle::RecentLogLines(lines); }

}  // namespace khvr
