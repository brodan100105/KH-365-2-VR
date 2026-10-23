// What the VR layer reads from the running game each game frame, and what it hands back.
//
// The port fills GameFrame from the decompiled game's own structures (see
// docs/ARCHITECTURE.md, "Game hooks"): the camera actor that Camera_CommitMatrices commits, the
// player actor of the active player overlay, and the scene id in data_027e0060. Positions are in
// game units (fx32 converted to float), with the game's Y up.
#pragma once

#include <cstdint>

#include "khvr/math.h"

namespace khvr {

// DS pad bits, as REG_KEYINPUT plus the ARM7's X/Y word (the layout Pad_Sample builds).
enum DsButton : uint16_t {
    kDsA = 1 << 0,
    kDsB = 1 << 1,
    kDsSelect = 1 << 2,
    kDsStart = 1 << 3,
    kDsRight = 1 << 4,
    kDsLeft = 1 << 5,
    kDsUp = 1 << 6,
    kDsDown = 1 << 7,
    kDsR = 1 << 8,
    kDsL = 1 << 9,
    kDsX = 1 << 10,
    kDsY = 1 << 11,
};

struct GameCamera {
    Vec3 eye;
    Vec3 target;
    Vec3 up{0, 1, 0};
    float fovYRad = 0.0f;  // 0 when unknown
};

struct GameFrame {
    bool inGameplay = false;     // a field scene with a controllable player
    bool inCutscene = false;     // an event script or camera animation drives the camera
    bool inMenu = false;         // pause / camp menu, shops, title: show the giant screen
    uint32_t frameCounter = 0;   // increments once per game frame (30 Hz or 60 Hz)
    GameCamera camera;           // the game's own camera this frame
    Vec3 playerPos;              // player actor feet position
    float playerYaw = 0.0f;      // heading, radians, 0 = facing -Z
    float playerHeight = 0.0f;   // collision height in game units, 0 when unknown
    bool playerGrounded = true;
    bool hasLockOnTarget = false;
    Vec3 lockOnTarget;
    // Nearest enemy, for swing aim assist.
    bool hasNearestEnemy = false;
    Vec3 nearestEnemy;
};

// Requests from the VR layer, applied by the port's hooks before the game's next update.
struct GameRequests {
    uint16_t buttons = 0;        // DS pad state to report from Pad_Sample
    // Analog movement. When set, the player controller uses it instead of the D-pad direction.
    bool hasAnalogMove = false;
    Vec3 analogMoveWorld;        // world-space direction, length 0..1
    // Overrides the game camera after the game computed it (culling, camera-relative
    // movement and the sound listener then follow the headset).
    bool overrideCamera = false;
    GameCamera camera;
    // First person: turn the player to this heading (radians) when it is free to turn.
    bool setPlayerYaw = false;
    float playerYaw = 0.0f;
    // First person room-scale: move the player by this world-space step through the game's
    // own collision (Actor_ResolveMove), so walls still stop you.
    bool hasRoomScaleStep = false;
    Vec3 roomScaleStep;
    // Widens the frustum the game culls with (Picture > Draw distance).
    float cullingScale = 1.0f;
    // Touch screen.
    bool touching = false;
    uint8_t touchX = 0, touchY = 0;  // 0..255, 0..191
};

}  // namespace khvr
