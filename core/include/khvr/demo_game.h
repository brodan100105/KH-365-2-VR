// A stand-in for the game: a small arena with a placeholder character and three training
// dummies ("Shadows"). It behaves like the parts of 358/2 Days the VR layer touches (a pad
// sampled once per 30 Hz game frame, camera-relative D-pad movement, jump on B, attack on A,
// block/dodge on Y, a follow camera, collision that stops room-scale steps), so panoramic and
// first-person modes, motion combat and every setting can be tried on the headset before the
// decompiled game runs.
#pragma once

#include <random>

#include "khvr/canvas.h"
#include "khvr/game_source.h"

namespace khvr {

class DemoGame : public GameSource {
public:
    static constexpr double kFrameSeconds = 1.0 / 30.0;  // 358/2 Days runs its field at 30 Hz
    static constexpr float kArenaHalf = 14.0f;            // arena is 28 x 28 units

    DemoGame();

    void Advance(double now, const GameRequests &requests, uint16_t (*sampleButtons)(void *ctx),
                 void *ctx) override;
    const GameFrame &frame() const override { return frame_; }
    const WorldDrawList &world() const override { return world_; }
    const DsScreens &screens() const override { return screens_; }
    // The character's own triangles, separate so first person can hide them.
    const WorldDrawList &playerMesh() const { return player_; }

    // One fixed game frame (public for tests).
    void Step(const GameRequests &requests, uint16_t buttons);
    int hits() const { return hits_; }

private:
    struct Dummy {
        Vec3 pos;
        float yaw = 0;
        int hp = 3;
        float flash = 0;      // seconds of hit flash left
        float respawn = 0;    // seconds until it comes back
        Vec3 knockback;
    };
    void Collide(Vec3 *pos, float radius) const;
    void BuildStatic();
    void BuildDynamic();
    void DrawScreens();

    GameFrame frame_;
    WorldDrawList static_, world_, player_;
    DsScreens screens_;
    Canvas top_, topHud_, bottom_;
    double time_ = -1.0;
    uint16_t prevButtons_ = 0;
    Vec3 velocity_;
    float attackTime_ = 0, guardTime_ = 0, dodgeTime_ = 0;
    Vec3 dodgeDir_;
    int hp_ = 30, hits_ = 0;
    Dummy dummies_[3];
    std::mt19937 rng_{358};
};

// Mesh helpers, shared with the app's own props (hands, Keyblade).
void AddBox(WorldDrawList *list, const Vec3 &center, const Vec3 &half, float yaw, uint32_t color);
void AddOrientedBox(WorldDrawList *list, const Vec3 &center, const Vec3 &half, const Quat &q,
                    uint32_t color);
void AddQuad(WorldDrawList *list, const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d,
             uint32_t color);

}  // namespace khvr
