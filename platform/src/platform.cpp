#include "khvr/platform.h"

#include <atomic>
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
    if (!t.idle && t.halts == lastHalts) {
        // No halt since the last beat: the guest is busy (a long load) or stuck in a loop.
        if (++stuckBeats <= 4) ThreadsLogRunning("no frame finished since the last heartbeat");
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
