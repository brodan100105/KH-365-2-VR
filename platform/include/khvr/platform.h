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
    uint32_t topObj[kWidth * kHeight];  // the top screen's sprites where they show: alpha 0 elsewhere
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
    kWorldTexAlpha = 16,    // translucent only through its texture's alpha: its solid texels are
                            // opaque (they hide what is behind them, as on the DS)
    kWorldDepthWrite = 32,  // translucent, and its pixels still set depth (POLYGON_ATTR bit 11)
    kWorldLine = 64,        // an edge of a wireframe polygon (alpha 0): the line from its first
                            // vertex to its second (the third repeats the second)
    kWorldBillboard = 128,  // part of a sprite drawn facing the game's camera (PlatformWorld::billboard)
};
constexpr int kWorldBillboardFloats = 6;
struct PlatformWorldTexture {
    uint64_t key = 0;  // its parameters, texels and palette: equal keys are the same image
    int width = 0, height = 0;
    bool repeatS = false, repeatT = false, flipS = false, flipT = false;
    std::vector<uint32_t> rgba;  // 0xAABBGGRR, row 0 at t = 0
};
// The player character in the field (Platform::ReadPlayer): where the game keeps it, in its own
// units (fixed point 1.0 = 4096 there, given here as floats), and its heading.
struct PlatformPlayer {
    bool valid = false;        // false outside the field, or before the player actor exists
    uint32_t actor = 0;        // guest address of the player actor
    float position[3] = {};    // feet, world space, game units
    float heading = 0.0f;      // radians; facing (sin, 0, cos)
    bool menu = false;         // a menu over the field runs (camp menu, shop, save point...)
    bool event = false;        // an event script (ov023) drives the scene: a cutscene
    bool choice = false;       // a choice waits for an answer (an NPC's question, a confirmation)
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
    // The game's own lens and camera (world to view) matrices, exactly as the DS used them. The
    // DS multiplies row vectors, so its row-major arrays are OpenGL's column-major ones as is.
    float lens[16] = {}, view[16] = {};
    // The DS's viewport (VIEWPORT: x1, y1 bottom-left, x2, y2, in its 256 x 192), and the
    // brightness changes the DS applies to its 3D layer (fades): up to two, each mode 0 none,
    // 1 towards white, 2 towards black, by factor / 16.
    int viewport[4] = {0, 0, 255, 191};
    int fadeMode[2] = {0, 0}, fadeFactor[2] = {0, 0};
    uint32_t polygonsLeftOut = 0;  // drawn with another projection (HUD pieces): in the DS picture
    // Sprites face the game's camera: flat across its view (munny, orbs, effects), or upright and
    // turned to it about the vertical (trees). Per vertex, when any triangle has kWorldBillboard:
    // the sprite's centre (world), the vertex's offset from it along the sprite's right and up,
    // and its kind (1 facing, 2 upright), so a viewer can turn the sprite to face itself.
    // kWorldBillboardFloats floats a vertex (zero for the other triangles), or empty. Not in files.
    std::vector<float> billboard;
    // The field's cursor is on a target at this world position (the game's cursors are 2D sprites
    // placed for its camera, or marks in front of it; a viewer draws its own here). Its kind: 1 or
    // 3 an enemy, 2 one of the field's entries (a person to talk to, a thing to use); locked when
    // locked on. Not in files.
    bool lockOn = false;
    float lockOnAt[3] = {0, 0, 0};
    int lockOnKind = 0;
    bool lockOnLocked = false;
};

// A captured world to a file and back (world_file.cpp).
bool PlatformWorldWrite(const std::string &path, const PlatformWorld &w);
bool PlatformWorldRead(const std::string &path, PlatformWorld *w);

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
    // The player character now (PlatformPlayer). Call when CaptureScreens may be called.
    bool ReadPlayer(PlatformPlayer *out);  // as of the game's last finished frame
    bool ReadPlayerNow(PlatformPlayer *out);  // from the game's memory right now
    // CaptureScreens also fills topFront / topBack.
    void SetWorldCapture(bool on);
    // The game asks the 3D engine whether a model's box is in its camera's view (BOX_TEST) before
    // drawing it: on, the answer is always yes, so it draws what lies around you too.
    void SetDrawAll(bool on);
    // The viewer draws the field's cursors at their targets itself: the game leaves its own out.
    void SetOwnCursors(bool on);
    // The 3D layer's resolution in CaptureScreens' topHi: 1 (the DS's) to 4 times it.
    void Set3dScale(int scale);
    // From the driver, for the heartbeat: a frame whose screens were skipped (it ran late), or
    // how long CaptureScreens took.
    void NoteDisplay(bool skipped, float captureMs);
    // From the viewer, for the heartbeat: a lock-on sight cut from the screen, the sprite pixels
    // it kept and those it dropped as other sprites.
    void NoteLockOnCut(int kept, int dropped);
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
