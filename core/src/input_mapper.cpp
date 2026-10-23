#include "khvr/input_mapper.h"

#include <cmath>

namespace khvr {

namespace {

constexpr float kStickDeadzone = 0.2f;
constexpr float kHoldSeconds = 0.8f;   // menu / right stick long press
constexpr float kGripPressed = 0.6f;
constexpr float kTriggerPressed = 0.6f;

}  // namespace

uint16_t BindingBits(DsBinding b) {
    switch (b) {
    case DsBinding::A: return kDsA;
    case DsBinding::B: return kDsB;
    case DsBinding::X: return kDsX;
    case DsBinding::Y: return kDsY;
    case DsBinding::L: return kDsL;
    case DsBinding::R: return kDsR;
    case DsBinding::None: return 0;
    }
    return 0;
}

uint16_t InputMapper::DpadFromDirection(float x, float y, float relativeYaw) {
    float mag = std::sqrt(x * x + y * y);
    if (mag < kStickDeadzone) return 0;
    // Stick angle, 0 = up, counter-clockwise positive, then rotated into the camera's frame.
    float angle = WrapAngle(std::atan2(-x, y) + relativeYaw);
    int octant = static_cast<int>(std::lround(angle / (kPi / 4.0f)));  // -4..4
    octant = ((octant % 8) + 8) % 8;  // 0 up, 1 up-left, 2 left, ... counter-clockwise
    static const uint16_t kOctants[8] = {
        kDsUp, kDsUp | kDsLeft, kDsLeft, kDsDown | kDsLeft,
        kDsDown, kDsDown | kDsRight, kDsRight, kDsUp | kDsRight,
    };
    return kOctants[octant];
}

void InputMapper::AddPulse(uint16_t bits) {
    if (!bits) return;
    for (Pulse &p : pulses_)
        if (p.bits == bits && p.framesLeft > 0) return;  // already pressing it
    for (Pulse &p : pulses_)
        if (p.framesLeft == 0 && !p.releasing) {
            p.bits = bits;
            p.framesLeft = kPulseGameFrames;
            return;
        }
}

void InputMapper::Update(const Settings &s, ViewMode mode, const ControllerState &c,
                         const CombatFrame *combat, float moveBasisYaw, float gameCameraYaw,
                         float dt) {
    actions_ = {};
    uint16_t held = 0;

    // ---- App actions: long presses never reach the game ----
    if (c.menu) {
        menuHeld_ += dt;
        if (menuHeld_ >= kHoldSeconds && !settingsFired_) {
            actions_.openSettingsPanel = true;
            settingsFired_ = true;
        }
    } else {
        // A short press of the menu button is START.
        if (menuWasDown_ && !settingsFired_) AddPulse(kDsStart);
        menuHeld_ = 0.0f;
        settingsFired_ = false;
    }
    menuWasDown_ = c.menu;

    if (c.stickClick[1]) {
        rightStickHeld_ += dt;
        if (rightStickHeld_ >= kHoldSeconds && !modeToggleFired_ &&
            s.Bool(SettingId::ModeSwitchButton)) {
            actions_.toggleViewMode = true;
            modeToggleFired_ = true;
        }
    } else {
        if (rightStickWasDown_ && !modeToggleFired_) actions_.recenter = true;
        rightStickHeld_ = 0.0f;
        modeToggleFired_ = false;
    }
    rightStickWasDown_ = c.stickClick[1];

    // ---- Buttons ----
    if (c.a) held |= kDsA;
    if (c.b) held |= kDsB;
    if (c.x) held |= kDsX;
    if (c.y) held |= kDsY;
    if (c.stickClick[0]) held |= kDsSelect;
    const bool firstPerson = mode == ViewMode::FirstPerson;
    if (firstPerson) {
        // The weapon hand's grip holds the Keyblade; the off hand's grip is the lock-on button.
        // Triggers: the off hand casts (Combat > Magic); the weapon-hand trigger is L.
        int weapon = s.Enum<Hand>(SettingId::FpWeaponHand) == Hand::Left ? 0 : 1;
        if (c.grip[1 - weapon] > kGripPressed)
            held |= BindingBits(s.Enum<DsBinding>(SettingId::CtrlLockOnButton));
        if (c.trigger[weapon] > kTriggerPressed) held |= kDsL;
        if (s.Enum<MagicGesture>(SettingId::CombatMagicGesture) == MagicGesture::Button &&
            c.trigger[1 - weapon] > kTriggerPressed)
            held |= BindingBits(s.Enum<DsBinding>(SettingId::CtrlMagicButton));
    } else {
        if (c.trigger[0] > kTriggerPressed) held |= kDsL;
        if (c.trigger[1] > kTriggerPressed) held |= kDsR;
    }

    // ---- Gestures ----
    if (combat) {
        if (combat->swingStarted) AddPulse(BindingBits(s.Enum<DsBinding>(SettingId::CtrlAttackButton)));
        if (combat->guarding) held |= BindingBits(s.Enum<DsBinding>(SettingId::CtrlGuardButton));
        if (combat->castMagic) AddPulse(BindingBits(s.Enum<DsBinding>(SettingId::CtrlMagicButton)));
        if (combat->jump) AddPulse(BindingBits(s.Enum<DsBinding>(SettingId::CtrlJumpButton)));
        if (combat->dodge) {
            AddPulse(BindingBits(s.Enum<DsBinding>(SettingId::CtrlDodgeButton)));
            // Hold the flick direction with the dodge button so the roll goes that way.
            dodgeDpad_ = DpadFromDirection(combat->dodgeDirection.x, -combat->dodgeDirection.z,
                                           moveBasisYaw - gameCameraYaw);
            dodgeFrames_ = kPulseGameFrames;
        }
    }

    // ---- Movement ----
    float mx = c.stickX[0], my = c.stickY[0];
    float mag = std::sqrt(mx * mx + my * my);
    if (mag >= kStickDeadzone) {
        float scaled = Clamp((mag - kStickDeadzone) / (1.0f - kStickDeadzone), 0.0f, 1.0f);
        // Stick up is "forward" along moveBasisYaw.
        Vec3 local{mx / mag * scaled, 0.0f, -my / mag * scaled};
        analogMove_ = Quat::Yaw(moveBasisYaw).Rotate(local);
        held |= DpadFromDirection(mx, my, moveBasisYaw - gameCameraYaw);
    } else {
        analogMove_ = {};
    }
    held_ = held;
}

uint16_t InputMapper::SampleForGameFrame() {
    uint16_t bits = held_;
    for (Pulse &p : pulses_) {
        if (p.framesLeft > 0) {
            bits |= p.bits;
            if (--p.framesLeft == 0) p.releasing = true;
        } else if (p.releasing) {
            // One released frame so the next press of the same button registers as new.
            bits &= static_cast<uint16_t>(~p.bits);
            p.releasing = false;
            p.bits = 0;
        }
    }
    if (dodgeFrames_ > 0) {
        bits = static_cast<uint16_t>((bits & ~(kDsUp | kDsDown | kDsLeft | kDsRight)) | dodgeDpad_);
        --dodgeFrames_;
    }
    return bits;
}

}  // namespace khvr
