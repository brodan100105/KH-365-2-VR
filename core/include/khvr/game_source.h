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

// The DS's two 256x192 screens, RGBA8. The top screen is the game's 3D view with its 2D HUD;
// the VR app shows it on the giant screen. The 2D-only layers (HUD) come separately so they can
// float on their own panel over the VR view.
struct DsScreens {
    static constexpr int kWidth = 256, kHeight = 192;
    std::vector<uint32_t> top;     // full top screen (3D + 2D), for the giant screen
    std::vector<uint32_t> topHud;  // top screen 2D layers only, alpha where nothing is drawn
    std::vector<uint32_t> bottom;  // touch screen
    uint32_t revision = 0;         // bumped whenever any of them changes
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
};

}  // namespace khvr
