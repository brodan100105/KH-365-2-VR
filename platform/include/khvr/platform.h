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
    bool movie = false;                 // the top screen shows a movie's picture
    // The top screen with its 3D layer at topScale times the resolution (Set3dScale), when it
    // has one: (256 x topScale) x (192 x topScale). topScale 1: use `top`.
    int topScale = 1;
    std::vector<uint32_t> topHi;
    // With the world capture on (SetWorldCapture): the top screen's 2D in front of the 3D layer
    // with the flat 3D HUD pieces, and its 2D behind the 3D layer (opaque), to go around the
    // captured world. `split` says they are filled.
    bool split = false;
    uint32_t topFront[kWidth * kHeight];
    uint32_t topBack[kWidth * kHeight];
};

// The game's 3D as geometry (Platform::CaptureWorld): every polygon of the last finished 3D
// frame drawn with the game's main camera, in world space, game units (fx32 1.0 = 1.0), with the
// game's Y up. Triangles; the DS has already lit their vertex colours.
struct PlatformWorldVertex {
    float x, y, z;
    float u, v;      // across the texture: 0..1 is its width/height (repeats beyond, as it asks)
    uint32_t color;  // 0xAABBGGRR: vertex colour, polygon alpha
};
enum PlatformWorldFlags : uint8_t {
    kWorldFront = 1,        // draw front faces (counterclockwise seen from the front)
    kWorldBack = 2,         // draw back faces
    kWorldTranslucent = 4,  // blended: the polygon's alpha below 31, or alpha in its texture
    kWorldDecal = 8,        // texture over the colour rather than multiplied with it
};
struct PlatformWorldTexture {
    uint64_t key = 0;  // its parameters, texels and palette: equal keys are the same image
    int width = 0, height = 0;
    bool repeatS = false, repeatT = false, flipS = false, flipT = false;
    std::vector<uint32_t> rgba;  // 0xAABBGGRR, row 0 at t = 0
};
struct PlatformWorld {
    std::vector<PlatformWorldVertex> vertices;  // three per triangle
    std::vector<int32_t> triangleTexture;       // per triangle: index into textures, -1 none
    std::vector<uint8_t> triangleFlags;         // per triangle: PlatformWorldFlags
    std::vector<PlatformWorldTexture> textures;
    // The game's camera for this frame, in the same space.
    bool hasCamera = false;
    float eye[3] = {0, 0, 0}, target[3] = {0, 0, -1}, up[3] = {0, 1, 0};
    float fovY = 0, nearZ = 0, farZ = 0;  // radians, game units
    uint32_t polygonsLeftOut = 0;  // drawn with another projection (HUD pieces): in the DS picture
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
    // The game's 3D world (PlatformWorld). Call when CaptureScreens may be called.
    void CaptureWorld(PlatformWorld *out);
    // CaptureScreens also fills topFront / topBack.
    void SetWorldCapture(bool on);
    // The 3D layer's resolution in CaptureScreens' topHi: 1 (the DS's) to 4 times it.
    void Set3dScale(int scale);
    // From the driver, for the heartbeat: a frame whose screens were skipped (it ran late), or
    // how long CaptureScreens took.
    void NoteDisplay(bool skipped, float captureMs);
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
