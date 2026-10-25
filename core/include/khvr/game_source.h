// The thing the VR app plays. Today that is DemoGame (a stand-in arena that exercises every
// VR feature without the game); the decompiled Kingdom Hearts 358/2 Days will implement the
// same interface once the port's platform layer runs it (docs/ARCHITECTURE.md).
#pragma once

#include <cstdint>
#include <vector>

#include "khvr/game_state.h"
#include "khvr/math.h"

namespace khvr {

// A triangle mesh in game-world units, ready to draw. The decompiled game produces these from
// the GX geometry commands it issues each frame (positions already multiplied into world space).
struct WorldVertex {
    Vec3 position;
    Vec3 normal;
    uint32_t color;  // 0xAABBGGRR
};

struct WorldDrawList {
    std::vector<WorldVertex> triangles;  // every 3 vertices make a triangle
    void Clear() { triangles.clear(); }
};

// The game's 3D world as the DS drew it, textured and already lit (the port's world capture).
struct TexturedVertex {
    Vec3 position;
    float u, v;      // 0..1 across the texture
    uint32_t color;  // 0xAABBGGRR
};
struct WorldTexture {
    uint64_t key = 0;  // equal keys are the same image
    int width = 0, height = 0;
    bool repeatS = false, repeatT = false, flipS = false, flipT = false;
    std::vector<uint32_t> rgba;
};
enum TexturedTriangleFlags : uint8_t {
    kTriFront = 1, kTriBack = 2, kTriTranslucent = 4, kTriDecal = 8,
};
struct TexturedWorld {
    std::vector<TexturedVertex> vertices;  // three per triangle
    std::vector<int32_t> triangleTexture;  // index into textures, -1 for none
    std::vector<uint8_t> triangleFlags;
    std::vector<WorldTexture> textures;
    uint32_t revision = 0;  // bumped with every new frame of it
};

// The DS's two 256x192 screens, RGBA8. The top screen is the game's 3D view with its 2D HUD;
// the VR app shows it on the giant screen. The 2D-only layers (HUD) come separately so they can
// float on their own panel over the VR view.
struct DsScreens {
    static constexpr int kWidth = 256, kHeight = 192;
    std::vector<uint32_t> top;     // full top screen (3D + 2D), for the giant screen
    std::vector<uint32_t> topHud;  // top screen 2D layers only, alpha where nothing is drawn
    std::vector<uint32_t> bottom;  // touch screen
    uint32_t revision = 0;         // bumped whenever any of them changes
    bool movie = false;            // the top screen shows a movie
    // With the world capture: the top screen's 2D in front of its 3D (with the flat 3D HUD
    // pieces) and behind it, for the textured world between them. Empty otherwise.
    std::vector<uint32_t> topFront, topBack;
    // The top screen with its 3D at topScale times the resolution, when topScale > 1.
    int topScale = 1;
    std::vector<uint32_t> topHi;
};

class GameSource {
public:
    virtual ~GameSource() = default;
    // Runs the game up to `now` seconds (fixed game-frame steps inside), applying `requests`
    // and the pad bits `sampleButtons` returns at each game frame's pad sample.
    virtual void Advance(double now, const GameRequests &requests,
                         uint16_t (*sampleButtons)(void *ctx), void *ctx) = 0;
    virtual const GameFrame &frame() const = 0;
    virtual const WorldDrawList &world() const = 0;
    virtual const DsScreens &screens() const = 0;
    // The textured world, when the game source captures one (empty otherwise).
    virtual const TexturedWorld &texturedWorld() const {
        static const TexturedWorld empty;
        return empty;
    }
    // The character's own triangles, kept apart so first person can hide them.
    virtual const WorldDrawList &player() const {
        static const WorldDrawList empty;
        return empty;
    }
};

}  // namespace khvr
