// View rigs: each decides, every headset frame, where the player's tracking space sits in the
// game world. The renderer draws the captured game geometry with
//   eyeView = inverse(trackingToWorld * eyePoseInTracking)
// so the same code serves both modes; only the mapping differs.
//
//   ThirdPersonRig  - "panoramic": you stand in the level behind the character, at any scale
//                     from life size to a tabletop diorama, and look and walk around freely.
//   FirstPersonRig  - your head is the character's head; physical walking moves the
//                     character, the stick moves it artificially, turning rotates the body.
#pragma once

#include "khvr/game_state.h"
#include "khvr/math.h"
#include "khvr/settings.h"

namespace khvr {

struct TrackingFrame {
    float dt = 0.0f;              // seconds since the previous headset frame
    Pose head;                    // in tracking space (meters, floor at y = 0)
    Pose hands[2];                // [0] left, [1] right (grip pose)
    bool handValid[2] = {false, false};
    float turnAxis = 0.0f;        // right stick X, -1..1, after the deadzone
    Vec3 moveAxis;                // left stick in its own frame (x right, z back), length <= 1
    bool recenter = false;        // the recenter action fired this frame
};

struct RigOutput {
    Similarity trackingToWorld;   // tracking meters -> game units
    float moveBasisYaw = 0.0f;    // world yaw that "stick forward" points along
    float vignette = 0.0f;        // 0..1, how much artificial motion (moving, turning) there is now
    bool blinkStarted = false;    // a comfort blink should start this frame
    GameRequests requests;        // camera override, player yaw, room-scale step
};

// Turning (snap / smooth) shared by both rigs. Returns the yaw change this frame.
class TurnController {
public:
    float Update(const Settings &s, float axis, float dt, bool *snapped);

private:
    bool armed_ = true;  // snap turns re-arm when the stick returns to center
};

// The game moves the character once per game frame (30 or 60 a second) while the headset draws
// up to 120: following those steps directly shakes the view, most on a change of direction.
// This plays each step out over the time between game frames instead, a game frame behind.
class MotionSmoother {
public:
    void Reset(const Vec3 &p);
    // The game's position this headset frame; returns the smoothed one. `shown` is how much of
    // the newest step to show at once (room-scale steps the view already showed). `predict`
    // (walking him with the stick): aims at where he will be by the next game frame, at his
    // speed, instead of where the newest one put him (a game frame behind).
    Vec3 Update(const Vec3 &target, uint32_t gameFrame, float dt, const Vec3 &shown = {}, bool predict = false);
    const Vec3 &position() const { return pos_; }

private:
    Vec3 from_, to_, pos_, step_;  // step_: the newest game frame's move
    uint32_t frame_ = 0;
    float elapsed_ = 0.0f;           // since the newest game frame
    float interval_ = 1.0f / 30.0f;  // between game frames, averaged
};

class ThirdPersonRig {
public:
    void Reset() { initialized_ = false; }
    RigOutput Update(const Settings &s, const GameFrame &game, const TrackingFrame &tracking);

    float viewYaw() const { return viewYaw_; }
    const Vec3 &anchor() const { return anchor_; }

private:
    bool initialized_ = false;
    Vec3 anchor_;          // smoothed character position the view is built around
    float viewYaw_ = 0.0f; // world yaw that tracking -Z points along
    // Where your head was and which way it looked when the view was last placed (a recenter):
    // the character stands in front of that, your eyes at the set height above its floor.
    Vec3 placedHead_;
    float placedHeadYaw_ = 0.0f;
    // After (re)placing the view, the game's camera may still be swinging into place (a level's
    // spawn): the view keeps looking its way until it has held still a moment.
    float settleLeft_ = 0.0f, settleStill_ = 0.0f, lastCameraYaw_ = 0.0f;
    Vec3 lastCameraDir_;
    TurnController turn_;
    MotionSmoother motion_;
};

class FirstPersonRig {
public:
    void Reset() { initialized_ = false; }
    RigOutput Update(const Settings &s, const GameFrame &game, const TrackingFrame &tracking);

    float bodyYaw() const { return bodyYaw_; }

private:
    bool initialized_ = false;
    float bodyYaw_ = 0.0f;       // world yaw of tracking -Z
    float calibratedEye_ = 0.0f; // your eye height (m) measured at the last recenter
    float scale_ = 1.0f;         // game units per tracking meter
    Vec3 center_;                // tracking floor point over the character when room-scale is off
    Vec3 lastHeadFloor_;         // head projected on the floor, tracking space, last frame
    Vec3 pendingStep_;           // room-scale movement requested but not yet applied by the game
    Vec3 lastPlayerPos_;
    // Where the view stands over him: it follows him at once while you walk him with the stick,
    // and glides after him otherwise (his attacks lunge him a step at a time).
    Vec3 standing_;
    Vec3 lastStepTaken_;  // GameFrame::roomStepTaken as last seen
    float steppedAgo_ = 1e9f;  // seconds since your head last moved over the floor (room-scale)
    uint32_t lastGameFrame_ = 0;
    TurnController turn_;
    MotionSmoother motion_;
};

// In-engine cutscenes, standing in the scene: your head is where the cutscene's camera is,
// looking its way, and moves and turns with it (smoothed; a cut jumps behind a blink). Only its
// heading turns the world (the horizon stays level); your own head moves freely within it.
class CutsceneRig {
public:
    void Reset() { initialized_ = false; }
    RigOutput Update(const Settings &s, const GameFrame &game, const TrackingFrame &tracking);

private:
    bool initialized_ = false;
    Vec3 refHead_;               // tracking-space head position the camera's eye maps to
    Vec3 eye_;                   // the camera's eye, smoothed (world)
    float cameraYaw_ = 0.0f;     // its heading, smoothed
    float yawOffset_ = 0.0f;     // tracking -Z to the camera's heading at placement, plus turning
    float sinceCut_ = 1e9f;      // seconds since the camera last jumped
    TurnController turn_;
};

// Scale (game units per tracking meter) of a rig, from the settings.
float ThirdPersonScale(const Settings &s);
float FirstPersonScale(const Settings &s, float realEyeHeight);
float CutsceneScale(const Settings &s);

}  // namespace khvr
