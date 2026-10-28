#include "khvr/input_mapper.h"

#include <cmath>

namespace khvr {

namespace {

constexpr float kStickDeadzone = 0.2f;
constexpr float kHoldSeconds = 0.8f;   // menu / right stick long press
constexpr float kGripPressed = 0.6f;
// Triggers press at half way and let go only near the top, so a light or wavering pull (paging a
// tutorial with L/R) neither misses nor flickers.
constexpr float kTriggerPressed = 0.5f, kTriggerReleased = 0.3f;
// Menus: a direction presses past kMenuPress and lets go below kMenuRelease, one axis at a time.
constexpr float kMenuPress = 0.55f;
constexpr float kMenuRelease = 0.35f;
// Field: an octant holds until the stick is this far past its edge.
constexpr float kOctantHysteresis = 8.0f * kPi / 180.0f;

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
        if (menuWasDown_ && !settingsFired_) {
            AddPulse(kDsStart);
            actions_.pressedStart = true;
        }
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

    for (int h = 0; h < 2; ++h)
        triggerDown_[h] = c.trigger[h] > (triggerDown_[h] ? kTriggerReleased : kTriggerPressed);

    // ---- Buttons ----
    if (c.a) held |= kDsA;
    if (c.b) held |= kDsB;
    if (c.x) held |= kDsX;
    if (c.y) held |= kDsY;
    if (c.stickClick[0]) held |= kDsSelect;
    // In a menu or a tutorial's pages the triggers are L and R in every view (first person keeps
    // one for magic otherwise, and the pages turn with L/R).
    const bool firstPerson = mode == ViewMode::FirstPerson && !menuStick_ &&
                             s.Enum<FirstPersonButtons>(SettingId::CtrlFirstPersonButtons) == FirstPersonButtons::Profile;
    if (firstPerson) {
        // The weapon hand's grip holds the Keyblade; the off hand's grip is the lock-on button.
        // Triggers: the off hand casts (Combat > Magic); the weapon-hand trigger is L.
        int weapon = s.Enum<Hand>(SettingId::FpWeaponHand) == Hand::Left ? 0 : 1;
        if (c.grip[1 - weapon] > kGripPressed)
            held |= BindingBits(s.Enum<DsBinding>(SettingId::CtrlLockOnButton));
        if (triggerDown_[weapon]) held |= kDsL;
        if (s.Enum<MagicGesture>(SettingId::CombatMagicGesture) == MagicGesture::Button &&
            triggerDown_[1 - weapon])
            held |= BindingBits(s.Enum<DsBinding>(SettingId::CtrlMagicButton));
    } else {
        if (triggerDown_[0]) held |= kDsL;
        if (triggerDown_[1]) held |= kDsR;
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
        if (!menuStick_) held |= FieldDpad(mx, my, moveBasisYaw - gameCameraYaw);
    } else {
        analogMove_ = {};
        octant_ = -1;
    }
    if (menuStick_) {
        held |= MenuDpad(mx, my);
        octant_ = -1;
    } else {
        menuDir_ = 0;
    }
    held_ = held;
}

uint16_t InputMapper::MenuDpad(float x, float y) {
    // A menu moves its cursor on each new press, so a stick wandering between directions (or
    // into a diagonal) skips items: hold one axis until the stick clearly leaves it.
    const float mag = std::sqrt(x * x + y * y);
    auto dominant = [&]() -> uint16_t {
        if (std::fabs(x) > std::fabs(y)) return x > 0 ? kDsRight : kDsLeft;
        return y > 0 ? kDsUp : kDsDown;
    };
    if (mag < kMenuRelease) {
        menuDir_ = 0;
    } else if (menuDir_ == 0) {
        if (mag >= kMenuPress) menuDir_ = dominant();
    } else {
        const bool horizontal = menuDir_ & (kDsLeft | kDsRight);
        const float along = horizontal ? std::fabs(x) : std::fabs(y);
        const float across = horizontal ? std::fabs(y) : std::fabs(x);
        const bool reversed = horizontal ? (x > 0) != (menuDir_ == kDsRight) : (y > 0) != (menuDir_ == kDsUp);
        if (reversed || across > along * 1.5f) menuDir_ = mag >= kMenuPress ? dominant() : 0;
    }
    return menuDir_;
}

uint16_t InputMapper::FieldDpad(float x, float y, float relativeYaw) {
    static const uint16_t kOctants[8] = {
        kDsUp, kDsUp | kDsLeft, kDsLeft, kDsDown | kDsLeft,
        kDsDown, kDsDown | kDsRight, kDsRight, kDsUp | kDsRight,
    };
    const float angle = WrapAngle(std::atan2(-x, y) + relativeYaw);
    if (octant_ >= 0) {
        // Stay in the octant while within its 45 degrees plus the margin on either side.
        const float off = std::fabs(WrapAngle(angle - octant_ * (kPi / 4.0f)));
        if (off <= kPi / 8.0f + kOctantHysteresis) return kOctants[octant_];
    }
    int octant = static_cast<int>(std::lround(angle / (kPi / 4.0f)));
    octant_ = ((octant % 8) + 8) % 8;
    return kOctants[octant_];
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
