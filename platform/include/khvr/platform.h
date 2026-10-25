// The platform layer's public face: boots the decompiled game on the DS hardware stand-ins and
// steps it one VBlank at a time. Used by the Quest app (DecompGame) and the desktop runner.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace khvr {

struct PlatformConfig {
    std::string romPath;   // the user's own EU (YKGP) ROM
    std::string savePath;  // the game's save (card backup), created on first save
    std::string logPath;   // boot log, written as the game runs
    std::string fileListPath;  // if set, the ROM's file names and sizes are written there
    int language = 1;      // firmware language: 1 English .. 5 Spanish
};

// Resetting: the game called OS_ResetSystem ("Return to title"); Restart() boots it again.
enum class PlatformState { NotStarted, Running, Crashed, Stopped, Resetting };

// What the DS's two screens show, rendered from the 2D engines (RGBA8, 0xAABBGGRR).
struct PlatformScreens {
    static constexpr int kWidth = 256, kHeight = 192;
    uint32_t top[kWidth * kHeight];
    uint32_t topHud[kWidth * kHeight];  // top screen 2D layers only: alpha 0 where nothing is drawn
    uint32_t bottom[kWidth * kHeight];
    bool top3d = false;                 // the top screen's BG0 shows the 3D engine
};

// Instantiates the guest and resets the hardware stand-ins without running the game, so tests
// can poke registers and memory (PlatformTestPoke) and render (Platform::CaptureScreens).
void PlatformInitForTests();
void PlatformTestWrite(uint32_t addr, uint32_t value, uint32_t size);
uint32_t PlatformTestRead(uint32_t addr, uint32_t size);
// After PlatformInitForTests: load a screen snapshot (khvr_snap_N.bin) for CaptureScreens.
bool PlatformLoadSnapshotForTests(const std::string &path);
bool PlatformSaveSnapshotForTests(const std::string &path);
// As if the game had called OS_ResetSystem: state() becomes Resetting, for Platform::Restart.
void PlatformTestRequestReset();

class Platform {
public:
    static Platform &Get();

    bool Start(const PlatformConfig &config, std::string *error);
    void Stop();
    // After the game asked for a reset (state() == Resetting): the DS reboots the cartridge, so
    // the guest is instantiated afresh and boots from main() again, keeping the ROM and the save.
    // Call from the thread that calls VBlank().
    void Restart();

    // Delivers one VBlank (the DS runs at ~59.83 Hz) and waits up to `timeoutMs` for the game to
    // finish the frame and go idle. Returns true if it did (screens and state are then stable).
    bool VBlank(int timeoutMs);

    // Renders both screens. Call after VBlank() returned true, while the game is idle.
    void CaptureScreens(PlatformScreens *out);
    // The game's sound, 48 kHz stereo (interleaved L, R): fills `frames` frames, silence where
    // nothing has been mixed yet; returns how many were real. For the audio thread.
    size_t ReadAudio(int16_t *stereo, size_t frames);
    static constexpr int kAudioRate = 48000;

    // Input for the next frames: DS buttons active high (khvr::DsButton bits), touch in pixels.
    void SetInput(uint16_t buttons, bool touching, int x, int y);

    // Every few seconds from the driver: logs how far the game got (frames, halts, display
    // registers) and, if the guest made no progress since the last call, where it is stuck.
    void Heartbeat();

    PlatformState state() const;
    uint32_t frames() const;
    std::string crashReason() const;
    // The last lines of the boot log, for the in-headset status panel.
    std::vector<std::string> RecentLog(size_t lines) const;
};

}  // namespace khvr
