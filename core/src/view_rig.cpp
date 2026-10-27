#include "khvr/view_rig.h"

#include <cmath>

namespace khvr {

namespace {

// A level change, respawn or warp moves the player farther than this in one frame; the rigs
// then re-place the view behind a blink instead of sliding across the level.
constexpr float kTeleportMeters = 8.0f;
// Teleport follow mode re-centers once the character is this far from the anchor.
constexpr float kTeleportFollowMeters = 2.5f;

Vec3 Flat(const Vec3 &v) { return {v.x, 0.0f, v.z}; }

float HandYaw(const TrackingFrame &t, int hand, float fallback) {
    return t.handValid[hand] ? YawOf(t.hands[hand].orientation) : fallback;
}

float CameraYaw(const GameCamera &c) {
    Vec3 dir = Flat(c.target - c.eye);
    return Length(dir) > 1e-4f ? YawOf(dir) : 0.0f;
}

float MoveBasis(MoveRelativeTo mode, float rigYaw, const TrackingFrame &t) {
    float headYaw = YawOf(t.head.orientation);
    switch (mode) {
    case MoveRelativeTo::Head: return rigYaw + headYaw;
    case MoveRelativeTo::LeftController: return rigYaw + HandYaw(t, 0, headYaw);
    case MoveRelativeTo::View: return rigYaw;
    }
    return rigYaw + headYaw;
}

// The game camera follows the headset, so the game culls, mixes 3D sound and aims
// camera-relative actions from where you actually look.
GameCamera HeadCamera(const Similarity &toWorld, const Pose &head, float lookDistance) {
    GameCamera c;
    c.eye = toWorld.Transform(head.position);
    c.target = c.eye + toWorld.TransformDir(head.orientation.Forward()) * lookDistance;
    c.up = toWorld.TransformDir(head.orientation.Up());
    return c;
}

float SeatedBoost(const Settings &s) {
    return s.Bool(SettingId::ComfortSeated) ? s.Get(SettingId::ComfortSeatedHeight) : 0.0f;
}

}  // namespace

float TurnController::Update(const Settings &s, float axis, float dt, bool *snapped) {
    *snapped = false;
    if (s.Bool(SettingId::GameInvertTurn)) axis = -axis;
    switch (s.Enum<TurnMode>(SettingId::ComfortTurn)) {
    case TurnMode::Snap:
        if (std::fabs(axis) < 0.3f) armed_ = true;
        if (armed_ && std::fabs(axis) > 0.7f) {
            armed_ = false;
            *snapped = true;
            // Stick right turns right, which is a negative rotation about +Y.
            return (axis > 0 ? -1.0f : 1.0f) * DegToRad(s.Get(SettingId::ComfortSnapAngle));
        }
        return 0.0f;
    case TurnMode::Smooth:
        return -axis * DegToRad(s.Get(SettingId::ComfortSmoothSpeed)) * dt;
    case TurnMode::Off:
        return 0.0f;
    }
    return 0.0f;
}

void MotionSmoother::Reset(const Vec3 &p) {
    from_ = to_ = pos_ = p;
    elapsed_ = 0.0f;
}

Vec3 MotionSmoother::Update(const Vec3 &target, uint32_t gameFrame, float dt, const Vec3 &shown) {
    elapsed_ += dt;
    if (gameFrame != frame_ || Length(target - to_) > 1e-6f) {
        frame_ = gameFrame;
        // Average the game's frame time, ignoring hitches (loads, pauses) longer than 0.1 s.
        if (elapsed_ > 0.0f && elapsed_ < 0.1f) interval_ += (elapsed_ - interval_) * 0.2f;
        from_ = pos_ + shown;
        to_ = target;
        elapsed_ = 0.0f;
    }
    const float k = Clamp(elapsed_ / std::fmax(interval_, 1e-3f), 0.0f, 1.0f);
    pos_ = from_ + (to_ - from_) * k;
    return pos_;
}

float ThirdPersonScale(const Settings &s) {
    return s.Get(SettingId::GameUnitsPerMeter) / s.Get(SettingId::TpWorldScale);
}

float FirstPersonScale(const Settings &s, float realEyeHeight) {
    float scale = s.Get(SettingId::GameUnitsPerMeter) / s.Get(SettingId::FpWorldScale);
    if (s.Bool(SettingId::FpMatchCharacterHeight) && realEyeHeight > 0.5f)
        scale *= s.Get(SettingId::FpEyeHeight) / realEyeHeight;
    return scale;
}

// ---------------------------------------------------------------------------------------------
// Third person (panoramic)
// ---------------------------------------------------------------------------------------------

RigOutput ThirdPersonRig::Update(const Settings &s, const GameFrame &game,
                                 const TrackingFrame &t) {
    RigOutput out;
    const float scale = ThirdPersonScale(s);
    const float headYaw = YawOf(t.head.orientation);

    bool reinit = !initialized_ || t.recenter;
    if (initialized_ && Length(game.playerPos - anchor_) > kTeleportMeters * scale) {
        reinit = true;
        out.blinkStarted = s.Bool(SettingId::ComfortBlink);
    }
    if (reinit) motion_.Reset(game.playerPos);
    const Vec3 target = s.Bool(SettingId::ViewSmoothMotion)
                            ? motion_.Update(game.playerPos, game.frameCounter, t.dt)
                            : game.playerPos;
    if (reinit) {
        // Look the way the game camera looks, from where you are standing now.
        viewYaw_ = CameraYaw(game.camera) - headYaw;
        anchor_ = target;
        initialized_ = true;
        if (t.recenter) out.blinkStarted = s.Bool(SettingId::ComfortBlink);
    }

    // Follow the character.
    Vec3 diff = target - anchor_;
    Vec3 flat = Flat(diff);
    float dist = Length(flat);
    float deadzone = s.Get(SettingId::TpDeadzone) * scale;
    Vec3 before = anchor_;
    switch (s.Enum<FollowMode>(SettingId::TpFollow)) {
    case FollowMode::Locked:
        anchor_ = target;
        break;
    case FollowMode::Smooth: {
        float k = 1.0f - std::exp(-s.Get(SettingId::TpFollowSpeed) * t.dt);
        if (dist > deadzone) anchor_ += (flat - Normalize(flat) * deadzone) * k;
        anchor_.y += diff.y * k;
        break;
    }
    case FollowMode::Teleport:
        if (dist > std::fmax(deadzone, kTeleportFollowMeters * scale)) {
            anchor_ = target;
            out.blinkStarted = s.Bool(SettingId::ComfortBlink);
        } else {
            anchor_.y += diff.y * (1.0f - std::exp(-4.0f * t.dt));
        }
        break;
    }

    bool snapped = false;
    float turn = turn_.Update(s, t.turnAxis, t.dt, &snapped);
    viewYaw_ = WrapAngle(viewYaw_ + turn);
    if (snapped) out.blinkStarted |= s.Bool(SettingId::ComfortBlink);

    // The character stands `distance` in front of the tracking origin and `height` below it.
    Vec3 placement{0.0f, -(s.Get(SettingId::TpHeight) + SeatedBoost(s)),
                   -s.Get(SettingId::TpDistance)};
    out.trackingToWorld.scale = scale;
    out.trackingToWorld.rotation = Quat::Yaw(viewYaw_);
    out.trackingToWorld.translation = anchor_ - out.trackingToWorld.rotation.Rotate(placement * scale);

    out.moveBasisYaw = MoveBasis(s.Enum<MoveRelativeTo>(SettingId::TpMoveRelativeTo), viewYaw_, t);

    // Artificial motion of the view (following, smooth turning) drives the vignette.
    float followSpeed = t.dt > 0 ? Length(anchor_ - before) / scale / t.dt : 0.0f;  // m/s
    float turnSpeed = t.dt > 0 && !snapped ? std::fabs(turn) / t.dt : 0.0f;          // rad/s
    out.vignette = s.Get(SettingId::ComfortVignette) *
                   Clamp(followSpeed / 2.0f + turnSpeed / 1.5f, 0.0f, 1.0f);

    out.requests.overrideCamera = true;
    out.requests.camera = HeadCamera(out.trackingToWorld, t.head, 10.0f * scale);
    out.requests.cullingScale = s.Get(SettingId::PicDrawDistance);
    return out;
}

// ---------------------------------------------------------------------------------------------
// First person
// ---------------------------------------------------------------------------------------------

RigOutput FirstPersonRig::Update(const Settings &s, const GameFrame &game, const TrackingFrame &t) {
    RigOutput out;
    const float headYaw = YawOf(t.head.orientation);
    const Vec3 headFloor = Flat(t.head.position);
    const float boost = SeatedBoost(s);

    bool reinit = !initialized_ || t.recenter;
    if (initialized_ && Length(game.playerPos - lastPlayerPos_) > kTeleportMeters * scale_) {
        reinit = true;
        out.blinkStarted = s.Bool(SettingId::ComfortBlink);
    }
    if (reinit) {
        motion_.Reset(game.playerPos);
        // Face the way the character faces, and measure your eye height once so that
        // crouching does not rescale the world.
        bodyYaw_ = game.playerYaw - headYaw;
        calibratedEye_ = t.head.position.y + boost;
        scale_ = FirstPersonScale(s, calibratedEye_);
        center_ = headFloor;
        lastHeadFloor_ = headFloor;
        pendingStep_ = {};
        lastGameFrame_ = game.frameCounter;
        initialized_ = true;
        if (t.recenter) out.blinkStarted = s.Bool(SettingId::ComfortBlink);
    }
    // Settings can change scale at any time (panel), the eye height stays calibrated.
    scale_ = FirstPersonScale(s, calibratedEye_);
    lastPlayerPos_ = game.playerPos;

    bool snapped = false;
    float turn = turn_.Update(s, t.turnAxis, t.dt, &snapped);
    if (snapped) out.blinkStarted |= s.Bool(SettingId::ComfortBlink);

    // With room-scale on, the head always sits over the character's feet (below), so turning
    // rotates the world around your head.
    bodyYaw_ = WrapAngle(bodyYaw_ + turn);

    Similarity &m = out.trackingToWorld;
    m.scale = scale_;
    m.rotation = Quat::Yaw(bodyYaw_);

    // The game applies a requested room-scale step on its next frame; until then, show it as
    // already applied so the view does not lag behind your head at the game's frame rate.
    Vec3 consumed;  // the room-scale steps the game has now applied, which the view already showed
    if (game.frameCounter != lastGameFrame_) {
        consumed = pendingStep_;
        pendingStep_ = {};
        lastGameFrame_ = game.frameCounter;
    }
    const Vec3 player = s.Bool(SettingId::ViewSmoothMotion)
                            ? motion_.Update(game.playerPos, game.frameCounter, t.dt, consumed)
                            : game.playerPos;

    Vec3 bodyFloor;  // tracking-space floor point that sits on the character's feet
    if (s.Bool(SettingId::FpRoomScale) && game.inGameplay) {
        Vec3 delta = headFloor - lastHeadFloor_;
        if (Length(delta) > 1e-5f) {
            Vec3 step = m.rotation.Rotate(delta * scale_);
            pendingStep_ += step;
            out.requests.hasRoomScaleStep = true;
            out.requests.roomScaleStep = step;
        }
        bodyFloor = headFloor;  // your head is always over the character
    } else {
        bodyFloor = center_;    // walking moves you away from the character's body
    }
    lastHeadFloor_ = headFloor;

    Vec3 feet = player + pendingStep_;
    feet.y -= boost * scale_;
    m.translation = feet - m.rotation.Rotate(bodyFloor * scale_);

    out.moveBasisYaw = MoveBasis(s.Enum<MoveRelativeTo>(SettingId::FpMoveRelativeTo), bodyYaw_, t);

    out.requests.overrideCamera = true;
    out.requests.camera = HeadCamera(m, t.head, 10.0f * scale_);
    out.requests.setPlayerYaw = true;
    out.requests.playerYaw = WrapAngle(bodyYaw_ + headYaw);
    out.requests.cullingScale = s.Get(SettingId::PicDrawDistance);

    float moving = Clamp(Length(t.moveAxis), 0.0f, 1.0f);
    float turnSpeed = t.dt > 0 && !snapped ? std::fabs(turn) / t.dt : 0.0f;
    out.vignette = s.Get(SettingId::ComfortVignette) * Clamp(moving + turnSpeed / 1.5f, 0.0f, 1.0f);
    return out;
}

}  // namespace khvr
