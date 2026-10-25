// Desktop runner: boots the game headless on the platform layer and reports how far it gets.
//
//   khdays_run --rom game.nds [--frames 600] [--save khdays.sav] [--log boot.log] [--screens prefix]
//   khdays_run --fake-rom [--frames 60]
//
// --fake-rom builds a minimal image with the right game code, an empty file system and an
// overlay table, so the boot path (OS, threads, interrupts, ARM7 handshakes, file system set-up,
// overlays) can be exercised without the game's data. The real game needs your own ROM.
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "khvr/platform.h"
#include "../src/hle.h"
#include "../src/sound.h"

namespace khvr::hle {
void Arm7Reset();
void Arm7OnSend(uint32_t word);
bool Arm7HasReply();
uint32_t Arm7PopReply();
void Arm7VBlank();
}  // namespace khvr::hle

namespace {

void Put32(std::vector<uint8_t> &b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
void Put16(std::vector<uint8_t> &b, size_t o, uint16_t v) { std::memcpy(&b[o], &v, 2); }

std::string WriteFakeRom(const std::string &path) {
    const uint32_t kOverlays = 303, kFnt = 0x4000, kFat = 0x4100, kOvt = 0x6000;
    std::vector<uint8_t> rom(0x20000, 0);
    std::memcpy(&rom[0], "KH DAYS FAKE", 12);
    std::memcpy(&rom[0x0c], "YKGP", 4);
    std::memcpy(&rom[0x10], "8P", 2);
    // File name table: the root directory, empty.
    Put32(rom, 0x40, kFnt);
    Put32(rom, 0x44, 16);
    Put32(rom, kFnt + 0, 8);                 // sub-table offset
    Put16(rom, kFnt + 4, static_cast<uint16_t>(kOverlays));  // first file id after the overlays
    Put16(rom, kFnt + 6, 1);                 // one directory
    rom[kFnt + 8] = 0;                       // end of the root's entries
    // File allocation table: one empty file per overlay.
    Put32(rom, 0x48, kFat);
    Put32(rom, 0x4c, kOverlays * 8);
    for (uint32_t i = 0; i < kOverlays; ++i) {
        Put32(rom, kFat + 8 * i, 0x10000);
        Put32(rom, kFat + 8 * i + 4, 0x10000);
    }
    // ARM9 overlay table: id, RAM address, sizes 0, file id = id.
    Put32(rom, 0x50, kOvt);
    Put32(rom, 0x54, kOverlays * 32);
    for (uint32_t i = 0; i < kOverlays; ++i) {
        size_t o = kOvt + 32 * i;
        Put32(rom, o + 0, i);
        Put32(rom, o + 4, 0x0204cac0);
        Put32(rom, o + 24, i);
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(rom.data(), 1, rom.size(), f) != rom.size()) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fclose(f);
    return path;
}

void WritePpm(const char *path, const uint32_t *px) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6 256 192 255\n");
    for (int i = 0; i < 256 * 192; ++i) {
        uint8_t rgb[3] = {static_cast<uint8_t>(px[i]), static_cast<uint8_t>(px[i] >> 8), static_cast<uint8_t>(px[i] >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

// Checks the 2D renderer without a ROM: one text background half filled with a red tile over a
// blue backdrop, and a green 8x8 sprite.
int RenderTest(const char *ppm) {
    using khvr::PlatformTestWrite;
    khvr::PlatformInitForTests();
    PlatformTestWrite(0x04000304, 0x8000, 2);       // POWCNT1: engine A on the top screen
    PlatformTestWrite(0x04000240, 0x81, 1);         // bank A -> BG VRAM A
    PlatformTestWrite(0x04000241, 0x82, 1);         // bank B -> OBJ VRAM A
    PlatformTestWrite(0x04000000, 0x00011110, 4);   // display on, BG0 + OBJ, 1D sprites
    PlatformTestWrite(0x04000008, 8 << 8, 2);       // BG0: tiles at 0, map at 0x4000
    PlatformTestWrite(0x05000000, 0x7c00, 2);       // backdrop: blue
    PlatformTestWrite(0x05000002, 0x001f, 2);       // colour 1: red
    PlatformTestWrite(0x05000204, 0x03e0, 2);       // sprite colour 2: green
    for (int i = 0; i < 32; i += 4) PlatformTestWrite(0x06000020 + i, 0x11111111, 4);  // tile 1
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 16; ++col) PlatformTestWrite(0x06004000 + (row * 32 + col) * 2, 1, 2);
    for (int i = 0; i < 32; i += 4) PlatformTestWrite(0x06400000 + i, 0x22222222, 4);  // sprite tile 0
    for (int s = 1; s < 128; ++s) PlatformTestWrite(0x07000000 + 8 * s, 0x0200, 2);     // others off
    PlatformTestWrite(0x07000000, 50, 2);           // sprite 0: y 50
    PlatformTestWrite(0x07000002, 200, 2);          // x 200
    PlatformTestWrite(0x07000004, 0, 2);            // tile 0, palette 0
    static khvr::PlatformScreens s;
    khvr::Platform::Get().CaptureScreens(&s);
    if (ppm) WritePpm(ppm, s.top);
    auto at = [&](int x, int y) { return s.top[y * 256 + x] & 0xffffff; };
    bool ok = at(10, 10) == 0x0000ff && at(180, 10) == 0xff0000 && at(203, 53) == 0x00ff00 &&
              (s.topHud[180 * 256 + 180] >> 24) == 0 && (s.bottom[0] & 0xffffff) == 0xffffff;
    std::printf("render test: %s (bg 0x%06x, backdrop 0x%06x, sprite 0x%06x)\n", ok ? "ok" : "FAILED",
                at(10, 10), at(180, 10), at(203, 53));
    // The same screen through a snapshot file: saved, the state cleared, loaded, drawn again.
    static khvr::PlatformScreens again;
    const char *snap = "render_test_snap.bin";
    bool snapOk = khvr::PlatformSaveSnapshotForTests(snap);
    khvr::PlatformInitForTests();
    snapOk = snapOk && khvr::PlatformLoadSnapshotForTests(snap);
    khvr::Platform::Get().CaptureScreens(&again);
    snapOk = snapOk && std::memcmp(again.top, s.top, sizeof s.top) == 0 &&
             std::memcmp(again.bottom, s.bottom, sizeof s.bottom) == 0;
    std::remove(snap);
    std::printf("snapshot test: %s\n", snapOk ? "ok" : "FAILED");
    ok = ok && snapOk;
    return ok ? 0 : 1;
}

// Renders a screen snapshot from the headset (khvr_snap_N.bin) to <prefix>_top.ppm and
// <prefix>_bottom.ppm.
int RenderSnapshot(const char *path, const char *prefix) {
    khvr::PlatformInitForTests();
    if (!khvr::PlatformLoadSnapshotForTests(path)) {
        std::fprintf(stderr, "cannot read snapshot %s\n", path);
        return 1;
    }
    static khvr::PlatformScreens s;
    khvr::Platform::Get().CaptureScreens(&s);
    WritePpm((std::string(prefix) + "_top.ppm").c_str(), s.top);
    WritePpm((std::string(prefix) + "_bottom.ppm").c_str(), s.bottom);
    std::printf("snapshot: rendered %s to %s_top.ppm and %s_bottom.ppm\n", path, prefix, prefix);
    return 0;
}

// The ARM7 stand-in's answers to requests exactly as the SDK sends them.
int PxiTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    auto reply = [](uint32_t tag) -> int64_t {
        while (Arm7HasReply()) {
            uint32_t w = Arm7PopReply();
            if ((w & 0x1f) == tag) return w >> 6;
        }
        return -1;
    };
    bool ok = true;
    // TP_RequestAutoSamplingStartAsync: two words, command 1 (AUTO_ON) in the start word. The
    // reply must name command 1, or TP_WaitBusy(2) on the title screen never returns.
    Arm7OnSend(6 | ((4 | 0x100 | 0x2000000) << 6));
    Arm7OnSend(6 | ((0 | 0x10000 | 0x1000000) << 6));
    int64_t r = reply(6);
    ok &= r >= 0 && (r & 0x01000000) && ((r >> 8) & 0x7f) == 1 && (r & 0xff) == 0;
    std::printf("pxi test: touch auto-sampling start -> %s (reply 0x%07llx)\n", ok ? "ok" : "WRONG",
                static_cast<long long>(r));
    // Once on, every frame brings an AUTO_SAMPLING (0x10) notification.
    Arm7VBlank();
    int64_t n = reply(6);
    bool okAuto = n >= 0 && ((n >> 8) & 0x7f) == 0x10;
    std::printf("pxi test: auto-sampling notification -> %s\n", okAuto ? "ok" : "WRONG");
    // TP_RequestAutoSamplingStopAsync: one word, command 2.
    Arm7OnSend(6 | (0x3000200u << 6));
    int64_t s = reply(6);
    bool okStop = s >= 0 && ((s >> 8) & 0x7f) == 2;
    std::printf("pxi test: touch auto-sampling stop -> %s\n", okStop ? "ok" : "WRONG");
    return ok && okAuto && okStop ? 0 : 1;
}

// The sound driver stand-in without a ROM: a 16-bit PCM square wave on channel 0 and a periodic
// alarm, set up through SNDCommand lists as the SDK sends them, then a second of frames.
int SoundTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    const uint32_t wave = 0x02100000, cmds = 0x02101000, shared = 0x02102000;
    // 400 Hz at 32768 Hz: 82 samples per period (41 high, 41 low), looped.
    const int period = 82, samples = period * 8;
    for (int i = 0; i < samples; ++i)
        khvr::PlatformTestWrite(wave + 2 * i, (i % period) < period / 2 ? 0x4000 : 0xc000, 2);
    auto cmd = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds + 0x18 * n;
        khvr::PlatformTestWrite(at, next, 4);
        khvr::PlatformTestWrite(at + 4, id, 4);
        khvr::PlatformTestWrite(at + 8, a0, 4);
        khvr::PlatformTestWrite(at + 12, a1, 4);
        khvr::PlatformTestWrite(at + 16, a2, 4);
        khvr::PlatformTestWrite(at + 20, a3, 4);
        return at;
    };
    const uint32_t timer = 0x10000 - 512;  // 16756991 / 512 = 32728 Hz
    cmd(0, cmds + 0x18, 0x1d, shared, 0, 0, 0);                                    // SHARED_WORK
    cmd(1, cmds + 0x30, 0x0e, 0 | timer << 16, wave, (samples / 2) | 127u << 24,   // SETUP_CHANNEL_PCM
        0 | 64u << 16 | 1u << 24 | 1u << 26);                                      //  PCM16, loop, pan 64
    cmd(2, cmds + 0x48, 0x12, 3, 0, 52366, 7);                                     // SETUP_ALARM 3, 0.1 s
    cmd(3, 0, 0x0c, 1, 0, 1u << 3, 0);                                             // START_TIMER ch 0, alarm 3
    Arm7OnSend(7 | (cmds << 6));
    std::vector<int16_t> out;
    int alarms = 0;
    for (int f = 0; f < 60; ++f) {
        SoundFrame();
        while (Arm7HasReply()) {
            uint32_t w = Arm7PopReply();
            if ((w & 0x1f) == 7 && (w >> 6) == (3u | 7u << 8)) ++alarms;
        }
        int16_t buf[2 * 1024];
        size_t n = SoundRead(buf, 802);
        out.insert(out.end(), buf, buf + 2 * n);
    }
    // Zero crossings of the left channel over the second: about 800 for 400 Hz.
    int crossings = 0;
    double sum = 0;
    for (size_t i = 2; i < out.size(); i += 2) {
        if ((out[i] >= 0) != (out[i - 2] >= 0)) ++crossings;
        sum += static_cast<double>(out[i]) * out[i];
    }
    double rms = out.empty() ? 0 : std::sqrt(sum / (out.size() / 2));
    bool status = khvr::PlatformTestRead(shared + 8, 2) & 1;
    bool ok = out.size() / 2 > 40000 && crossings > 760 && crossings < 840 && rms > 1000 && alarms >= 9 &&
              alarms <= 11 && status && khvr::PlatformTestRead(shared, 4) == 1;
    std::printf("sound test: %s (%zu frames, %d zero crossings, rms %.0f, %d alarms, channel status %d)\n",
                ok ? "ok" : "FAILED", out.size() / 2, crossings, rms, alarms, status);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "--render-test") return RenderTest(argc >= 3 ? argv[2] : nullptr);
    if (argc >= 2 && std::string(argv[1]) == "--pxi-test") return PxiTest();
    if (argc >= 2 && std::string(argv[1]) == "--sound-test") return SoundTest();
    if (argc >= 4 && std::string(argv[1]) == "--render-snapshot") return RenderSnapshot(argv[2], argv[3]);
    khvr::PlatformConfig cfg;
    int frames = 600;
    std::string screens;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--rom") cfg.romPath = next();
        else if (a == "--fake-rom") cfg.romPath = WriteFakeRom("khdays_fake.nds");
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--save") cfg.savePath = next();
        else if (a == "--log") cfg.logPath = next();
        else if (a == "--file-list") cfg.fileListPath = next();
        else if (a == "--language") cfg.language = std::atoi(next().c_str());
        else if (a == "--screens") screens = next();
        else {
            std::fprintf(stderr, "usage: %s --rom game.nds | --fake-rom [--frames N] [--save f] [--log f]\n", argv[0]);
            return 2;
        }
    }
    if (cfg.romPath.empty()) {
        std::fprintf(stderr, "no ROM: pass --rom <your EU ROM> or --fake-rom\n");
        return 2;
    }
    if (cfg.savePath.empty()) cfg.savePath = "khdays.sav";

    khvr::Platform &p = khvr::Platform::Get();
    std::string error;
    if (!p.Start(cfg, &error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    int idleFrames = 0, busy = 0;
    for (int f = 0; f < frames && p.state() == khvr::PlatformState::Running; ++f) {
        if (f % 60 == 0) p.Heartbeat();
        if (f == 0 && std::getenv("KHVR_TEST_SAMPLE"))
            for (int i = 0; i < 20; ++i) {  // the watchdog's sample of a busy guest
                usleep(500);
                p.Heartbeat();
            }
        if (p.VBlank(200)) ++idleFrames;
        else ++busy;
    }
    std::printf("state: %s after %u frames (%d completed, %d busy)\n",
                p.state() == khvr::PlatformState::Running ? "running"
                : p.state() == khvr::PlatformState::Crashed ? "crashed" : "stopped",
                p.frames(), idleFrames, busy);
    if (p.state() == khvr::PlatformState::Crashed) std::printf("crash: %s\n", p.crashReason().c_str());
    if (!screens.empty()) {
        // Both screens as they were at the end, as <prefix>_top.ppm and <prefix>_bottom.ppm.
        static khvr::PlatformScreens s;
        p.CaptureScreens(&s);
        WritePpm((screens + "_top.ppm").c_str(), s.top);
        WritePpm((screens + "_bottom.ppm").c_str(), s.bottom);
        std::printf("screens written to %s_top.ppm / _bottom.ppm\n", screens.c_str());
    }
    std::printf("last log lines:\n");
    for (const std::string &l : p.RecentLog(25)) std::printf("  %s\n", l.c_str());
    p.Stop();
    std::fflush(stdout);
    std::_Exit(0);  // guest threads are parked; do not wait for them
}
