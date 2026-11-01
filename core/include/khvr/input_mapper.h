// Turns Quest controller state and recognized gestures into what the game reads: the DS pad
// bits Pad_Sample reports, plus an analog world-space movement vector.
//
// The headset runs at 72-120 Hz and the game samples its pad once per game frame (30 or 60 Hz),
// so one-shot actions (a swing, a cast) are latched as pulses that stay pressed for a fixed
// number of game frames, then released for one frame so the game sees a fresh press next time.
#pragma once

#include <cstdint>

#include "khvr/combat.h"
#include "khvr/game_state.h"
#include "khvr/settings.h"

namespace khvr {

struct ControllerState {
    // Face buttons: A/B on the right controller, X/Y on the left.
    bool a = false, b = false, x = false, y = false;
    bool menu = false;                     // left controller menu button
    bool stickClick[2] = {false, false};   // [0] left, [1] right
    float trigger[2] = {0, 0};
    float grip[2] = {0, 0};
    float stickX[2] = {0, 0};              // right = +1
    float stickY[2] = {0, 0};              // up/forward = +1
};

// App-level actions the mapper recognizes on the controllers (not forwarded to the game).
struct AppActions {
    bool openSettingsPanel = false;  // menu held
    bool recenter = false;           // right stick tapped
    bool toggleViewMode = false;     // right stick held (View > Quick mode switch)
    bool pressedStart = false;       // menu tapped: START went to the game (for the boot log)
};

class InputMapper {
public:
    static constexpr int kPulseGameFrames = 2;

    // Called every headset frame. `combat` may be null outside first person.
    void Update(const Settings &s, ViewMode mode, const ControllerState &c,
                const CombatFrame *combat, float moveBasisYaw, float gameCameraYaw, float dt);

    // Called once per game frame, right before the game's Pad_Sample; returns the pad bits
    // and advances the pulse timers.
    uint16_t SampleForGameFrame();

    // The game took an action from the buttons (GameFrame::actionsTaken went up): a swing waiting
    // to be taken has been, and the next one queued (swung meanwhile) starts.
    void NoteActionTaken();
    // A swing's attack press waiting for the game to take it (tests).
    bool attackPending() const { return attackFrames_ > 0; }

    const AppActions &actions() const { return actions_; }
    // Analog movement in world space (length 0..1), for the analog movement hook.
    const Vec3 &analogMoveWorld() const { return analogMove_; }
    uint16_t heldButtons() const { return held_; }
    // The thumbstick's direction as the game's D-pad heading (0..0xffff: 0 the game camera's
    // forward, 0x4000 its left) while it moves Roxas in the field, -1 otherwise.
    int fieldHeading() const { return fieldHeading_; }

    // The game shows a menu: the stick then moves its cursor one axis at a time, with a firmer
    // press and a release point, instead of steering 8 ways relative to the camera.
    void SetMenuStick(bool menu) { menuStick_ = menu; }

    // Quantizes a direction relative to the game camera into the 8-way D-pad.
    static uint16_t DpadFromDirection(float stickX, float stickY, float relativeYaw);

private:
    struct Pulse {
        uint16_t bits = 0;
        int framesLeft = 0;
        bool releasing = false;
    };
    void AddPulse(uint16_t bits);
    uint16_t MenuDpad(float x, float y);
    uint16_t FieldDpad(float x, float y, float relativeYaw);

    uint16_t held_ = 0;           // continuously held buttons this headset frame
    Pulse pulses_[4];
    Vec3 analogMove_;
    AppActions actions_;
    float menuHeld_ = 0.0f;
    float rightStickHeld_ = 0.0f;
    bool rightStickWasDown_ = false;
    bool modeToggleFired_ = false;
    bool settingsFired_ = false;
    bool menuWasDown_ = false;
    // Chain attacks: a swing presses its button again every fourth game frame until the game takes
    // it (mid-attack it drops a press it cannot use yet) or kAttackWaitFrames pass; one more swing
    // made meanwhile waits its turn.
    static constexpr int kAttackWaitFrames = 36;  // 0.6 s of DS frames
    uint16_t attackBits_ = 0;
    int attackFrames_ = 0, attackPhase_ = 0;
    bool attackQueued_ = false;
    uint16_t dodgeDpad_ = 0;
    int dodgeFrames_ = 0;
    bool menuStick_ = false;
    bool triggerDown_[2] = {false, false};
    bool menuArmed_ = false;  // the menu button has been up since the start
    int fieldHeading_ = -1;  // each trigger pressed (with hysteresis)
    uint16_t menuDir_ = 0;  // the menu direction held, 0 none
    int octant_ = -1;       // the field octant held, -1 none
};

uint16_t BindingBits(DsBinding b);

}  // namespace khvr
