#include "khvr/combat.h"

#include <cmath>

namespace khvr {

namespace {

// Frames the tip must stay above the threshold before a swing counts; filters tracking spikes.
constexpr int kFramesToStart = 2;
// A swing ends when the tip slows below this fraction of the threshold.
constexpr float kEndFraction = 0.45f;
// Velocity smoothing time constant (s).
constexpr float kVelocitySmoothing = 0.012f;
// Guard: blade within this angle of horizontal, hand at most this far from the head.
constexpr float kGuardMaxTiltDeg = 35.0f;
constexpr float kGuardMaxReach = 0.75f;
constexpr float kGuardHoldSeconds = 0.12f;
// Dodge: a flick is released within this long; held longer, the stick is walking.
constexpr float kFlickMaxHold = 0.25f;
// Physical jump: upward head speed (m/s).
constexpr float kJumpSpeed = 1.3f;

}  // namespace

Vec3 BladeAxis(const Pose &grip, float gripAngleRad) {
    // Controllers are held tilted forward; the grip angle lets the player correct that.
    Quat tilt = Quat::AxisAngle({1, 0, 0}, gripAngleRad);
    return (grip.orientation * tilt).Forward();
}

Vec3 BladeTip(const Pose &grip, float lengthMeters, float gripAngleRad) {
    return grip.position + BladeAxis(grip, gripAngleRad) * lengthMeters;
}

SwingKind ClassifySwing(const Vec3 &v, const Vec3 &axis, const Pose &head) {
    Vec3 dir = Normalize(v);
    // A thrust moves the tip along the blade, away from the body.
    if (Dot(dir, axis) > 0.75f) return SwingKind::Thrust;
    // Otherwise compare the vertical and the sideways parts of the motion, seen from the head.
    Vec3 right = Normalize(Vec3{head.orientation.Right().x, 0, head.orientation.Right().z});
    float vertical = std::fabs(dir.y);
    float side = std::fabs(Dot(dir, right));
    if (vertical > 2.0f * side) return SwingKind::Vertical;
    if (side > 2.0f * vertical) return SwingKind::Horizontal;
    return SwingKind::Diagonal;
}

void CombatRecognizer::Reset() { *this = CombatRecognizer(); }

CombatFrame CombatRecognizer::Update(const Settings &s, const CombatInput &in) {
    CombatFrame out;
    if (in.dt <= 0.0f) return out;

    const float threshold = s.Get(SettingId::CombatSwingSpeed);
    const float length = kBladeLength * s.Get(SettingId::FpKeybladeLength);
    const float gripAngle = DegToRad(s.Get(SettingId::FpGripAngle));
    const float haptics = s.Get(SettingId::CombatHaptics);
    const int weaponHand = s.Enum<Hand>(SettingId::FpWeaponHand) == Hand::Left ? 0 : 1;
    const bool dual = s.Bool(SettingId::FpDualWield);
    const bool motion = s.Bool(SettingId::CombatMotion);

    // ---- Swings ----
    for (int h = 0; h < 2; ++h) {
        HandState &st = hands_[h];
        st.cooldown = std::fmax(0.0f, st.cooldown - in.dt);
        if (!in.handValid[h]) {
            st.hasPrev = false;
            st.fastFrames = 0;
            st.inSwing = false;
            continue;
        }
        Vec3 tip = BladeTip(in.hands[h], length, gripAngle);
        const Vec3 grip = in.hands[h].position;
        if (st.hasPrev) {
            Vec3 raw = (tip - st.prevTip) / in.dt;
            float k = 1.0f - std::exp(-in.dt / kVelocitySmoothing);
            st.velocity = Lerp(st.velocity, raw, k);
            st.gripVelocity = Lerp(st.gripVelocity, (grip - st.prevGrip) / in.dt, k);
            // The hand's path through one fast motion: it must be the arm swinging, not the wrist.
            const float armSpeed = s.Get(SettingId::CombatArmSpeed);
            if (Length(st.gripVelocity) > armSpeed * 0.5f && Length(st.velocity) > threshold * kEndFraction)
                st.armPath += Length(grip - st.prevGrip);
            else
                st.armPath = 0.0f;
        }
        st.prevTip = tip;
        st.prevGrip = grip;
        st.hasPrev = true;

        bool armed = motion && (h == weaponHand || dual);
        float speed = Length(st.velocity);
        if (!armed) continue;

        if (st.inSwing) {
            if (speed < threshold * kEndFraction) {
                st.inSwing = false;
                st.cooldown = s.Get(SettingId::CombatSwingCooldown);
            }
            continue;
        }
        st.fastFrames = speed > threshold ? st.fastFrames + 1 : 0;
        const bool arm = Length(st.gripVelocity) >= s.Get(SettingId::CombatArmSpeed) &&
                         st.armPath >= s.Get(SettingId::CombatArmDistance);
        if (st.fastFrames >= kFramesToStart && arm && st.cooldown <= 0.0f && !out.swingStarted) {
            st.inSwing = true;
            st.fastFrames = 0;
            out.swingStarted = true;
            out.swing.hand = h;
            out.swing.direction = Normalize(st.velocity);
            out.swing.speed = speed;
            out.swing.strength = Clamp((speed - threshold) / threshold, 0.0f, 1.0f);
            out.swing.kind = ClassifySwing(st.velocity, BladeAxis(in.hands[h], gripAngle), in.head);
            out.hapticAmplitude[h] = haptics * (0.25f + 0.25f * out.swing.strength);
            out.hapticSeconds[h] = 0.03f;
        }
    }

    // ---- Guard ----
    bool guardPose = false;
    if (s.Bool(SettingId::CombatGuardGesture) && in.handValid[weaponHand]) {
        const Pose &g = in.hands[weaponHand];
        Vec3 axis = BladeAxis(g, gripAngle);
        float tilt = RadToDeg(std::asin(Clamp(std::fabs(axis.y), 0.0f, 1.0f)));
        Vec3 toHand = g.position - in.head.position;
        Vec3 fwd = in.head.orientation.Forward();
        bool inFront = Dot(Normalize(Vec3{toHand.x, 0, toHand.z}),
                           Normalize(Vec3{fwd.x, 0, fwd.z})) > 0.3f;
        bool reach = Length(toHand) < kGuardMaxReach;
        bool height = toHand.y > -0.55f && toHand.y < 0.25f;  // chest to just above the eyes
        bool slow = Length(hands_[weaponHand].velocity) < threshold * 0.5f;
        // Across the body: the blade points sideways, not forward.
        Vec3 right = in.head.orientation.Right();
        bool across = std::fabs(Dot(axis, Normalize(Vec3{right.x, 0, right.z}))) > 0.6f;
        guardPose = tilt < kGuardMaxTiltDeg && inFront && reach && height && slow && across;
    }
    guardTime_ = guardPose ? guardTime_ + in.dt : 0.0f;
    bool nowGuarding = guardTime_ >= kGuardHoldSeconds;
    out.guardStarted = nowGuarding && !guarding_;
    guarding_ = nowGuarding;
    out.guarding = guarding_;
    if (out.guardStarted) {
        out.hapticAmplitude[weaponHand] = std::fmax(out.hapticAmplitude[weaponHand], haptics * 0.3f);
        out.hapticSeconds[weaponHand] = std::fmax(out.hapticSeconds[weaponHand], 0.05f);
    }

    // ---- Magic: point the off hand and pull its trigger ----
    {
        int off = 1 - weaponHand;
        bool pressed = in.trigger[off] > 0.75f;
        bool released = in.trigger[off] < 0.35f;
        HandState &st = hands_[off];
        if (s.Enum<MagicGesture>(SettingId::CombatMagicGesture) == MagicGesture::PointAndTrigger &&
            in.handValid[off] && pressed && !st.triggerHeld && in.enemyTargeted) {
            Vec3 point = in.hands[off].orientation.Forward();
            Vec3 look = in.head.orientation.Forward();
            // The hand has to point roughly where you look.
            if (Dot(point, look) > 0.5f) out.castMagic = true;
        }
        if (pressed) st.triggerHeld = true;
        if (released) st.triggerHeld = false;
    }

    // ---- Physical jump ----
    jumpCooldown_ = std::fmax(0.0f, jumpCooldown_ - in.dt);
    if (hasPrevHead_) {
        float vy = (in.head.position.y - prevHeadY_) / in.dt;
        headVy_ = Lerp(headVy_, vy, 1.0f - std::exp(-in.dt / 0.03f));
    }
    prevHeadY_ = in.head.position.y;
    hasPrevHead_ = true;
    if (s.Bool(SettingId::CombatJumpGesture) && headVy_ > kJumpSpeed && jumpCooldown_ <= 0.0f) {
        out.jump = true;
        jumpCooldown_ = 0.6f;
    }

    // ---- Dodge: flick the stick from center to its edge and let go ----
    // Pushing the stick to start walking is as fast as a flick, so the flick only counts once the
    // stick springs back: holding it at the edge is walking.
    float mag = Length(in.moveAxis);
    if (mag < 0.2f) {
        if (flickPending_) {
            if (s.Enum<DodgeMode>(SettingId::CombatDodge) == DodgeMode::StickFlick) {
                out.dodge = true;
                out.dodgeDirection = flickDirection_;
            }
            flickPending_ = false;
        }
        // Resting at center arms a flick.
        stickRestTime_ += in.dt;
        flickBandTime_ = 0.0f;
        stickWasCentered_ = stickRestTime_ > 0.08f;
    } else if (mag > 0.9f) {
        if (stickWasCentered_) {
            flickPending_ = true;
            flickHeldTime_ = 0.0f;
            flickDirection_ = Normalize(in.moveAxis);
        }
        stickWasCentered_ = false;
        stickRestTime_ = 0.0f;
    } else {
        // Between center and edge: a flick has to cross this band quickly; walking does not.
        stickRestTime_ = 0.0f;
        flickBandTime_ += in.dt;
        if (flickBandTime_ > 0.07f) stickWasCentered_ = false;
    }
    if (flickPending_ && mag >= 0.2f) {
        flickHeldTime_ += in.dt;
        if (flickHeldTime_ > kFlickMaxHold) flickPending_ = false;
    }

    return out;
}

}  // namespace khvr
