// First-person motion combat: turns controller motion into game actions.
//
//  - Swing:  the Keyblade tip moves faster than Combat > Swing threshold. Each swing is one
//            attack (one press of the attack button), classified as horizontal, vertical,
//            diagonal or thrust so later iterations can pick matching combo animations.
//  - Guard:  the blade is held roughly level across the body in front of the head.
//  - Magic:  pointing the off hand forward and pulling its trigger casts the selected command.
//  - Jump:   a quick upward head movement (optional, Combat > Physical jump).
//  - Dodge:  flicking the movement stick from center to its edge and letting go.
//
// Everything works in tracking space (meters); the caller turns directions into world space
// with the rig's trackingToWorld.
#pragma once

#include <cstdint>

#include "khvr/math.h"
#include "khvr/settings.h"
#include "khvr/view_rig.h"

namespace khvr {

enum class SwingKind : uint8_t { Horizontal, Vertical, Diagonal, Thrust };

struct SwingEvent {
    int hand = 1;            // 0 left, 1 right
    SwingKind kind = SwingKind::Horizontal;
    Vec3 direction;          // normalized blade-tip velocity at detection, tracking space
    float speed = 0.0f;      // m/s at detection
    float strength = 0.0f;   // 0..1, how far above the threshold
};

struct CombatFrame {
    bool swingStarted = false;
    SwingEvent swing;
    bool guarding = false;
    bool guardStarted = false;
    bool castMagic = false;
    bool jump = false;
    bool dodge = false;
    Vec3 dodgeDirection;     // stick direction of the flick (x right, z back)
    // Haptic pulses to play, per hand: amplitude 0..1 and duration in seconds.
    float hapticAmplitude[2] = {0, 0};
    float hapticSeconds[2] = {0, 0};
};

struct CombatInput {
    float dt = 0.0f;
    Pose head;
    Pose hands[2];
    bool handValid[2] = {false, false};
    float trigger[2] = {0, 0};   // analog trigger value per hand
    Vec3 moveAxis;               // left stick
};

// Where the blade tip is for a grip pose: the Keyblade points along the controller's -Z,
// tilted by the grip angle about the controller's X axis.
Vec3 BladeTip(const Pose &grip, float lengthMeters, float gripAngleRad);
Vec3 BladeAxis(const Pose &grip, float gripAngleRad);

class CombatRecognizer {
public:
    // Base Keyblade length in meters at Keyblade length 1.0x.
    static constexpr float kBladeLength = 0.85f;

    void Reset();
    CombatFrame Update(const Settings &s, const CombatInput &in);

private:
    struct HandState {
        bool hasPrev = false;
        Vec3 prevTip;
        Vec3 velocity;           // smoothed tip velocity
        Vec3 prevGrip;
        Vec3 gripVelocity;       // smoothed hand velocity
        float armPath = 0.0f;    // metres the hand has travelled in the current fast motion
        int fastFrames = 0;      // consecutive frames above the threshold
        bool inSwing = false;
        float cooldown = 0.0f;   // seconds until the next swing may start
        bool triggerHeld = false;
    };
    HandState hands_[2];
    float guardTime_ = 0.0f;
    bool guarding_ = false;
    float prevHeadY_ = 0.0f;
    bool hasPrevHead_ = false;
    float headVy_ = 0.0f;
    float jumpCooldown_ = 0.0f;
    float stickRestTime_ = 0.0f;  // how long the stick has been near center
    float flickBandTime_ = 0.0f;  // how long the stick has been between center and edge
    bool stickWasCentered_ = false;
    bool flickPending_ = false;   // reached the edge from center; a dodge if released soon
    float flickHeldTime_ = 0.0f;
    Vec3 flickDirection_;
};

SwingKind ClassifySwing(const Vec3 &tipVelocity, const Vec3 &bladeAxis, const Pose &head);

}  // namespace khvr
