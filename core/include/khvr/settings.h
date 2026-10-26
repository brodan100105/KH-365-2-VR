// VR settings: one table describes every option (tab, key, label, type, range, default, help),
// the in-headset panel is generated from that table, and the same table reads and writes the
// settings file, so an option only has to be added in one place (settings.cpp).
//
// The settings file is INI-style text at <app files dir>/khvr.ini:
//
//   [View]
//   mode = third_person
//   [Camera]
//   distance = 3.5
//
// Unknown keys are kept as warnings and ignored; values out of range are clamped.
#pragma once

#include <string>
#include <vector>

namespace khvr {

enum class SettingType { Bool, Float, Int, Enum };

// Every option. The order inside a tab is the order the panel lists them in.
enum class SettingId : int {
    // View
    ViewMode,
    CutsceneView,
    ModeSwitchButton,
    // Screen (the giant screen used for cutscenes, menus and "Giant screen" mode)
    ScreenDistance,
    ScreenWidth,
    ScreenCurvature,
    ScreenStereo,
    ScreenDepth,
    ScreenPicture,
    Passthrough,
    // Camera (third-person panoramic)
    TpWorldScale,
    TpDistance,
    TpHeight,
    TpFollow,
    TpFollowSpeed,
    TpDeadzone,
    TpMoveRelativeTo,
    TpWallFade,
    // First person
    FpEyeHeight,
    FpMatchCharacterHeight,
    FpMoveRelativeTo,
    FpRoomScale,
    FpShowBody,
    FpWeaponHand,
    FpDualWield,
    FpGripAngle,
    FpKeybladeLength,
    // Combat (first person motion controls)
    CombatMotion,
    CombatPhysicalHits,
    CombatSwingSpeed,
    CombatSwingCooldown,
    CombatAimAssist,
    CombatGuardGesture,
    CombatMagicGesture,
    CombatJumpGesture,
    CombatDodge,
    CombatHaptics,
    // Controls (which DS button each gesture presses)
    CtrlAttackButton,
    CtrlGuardButton,
    CtrlDodgeButton,
    CtrlMagicButton,
    CtrlJumpButton,
    CtrlLockOnButton,
    // Comfort
    ComfortTurn,
    ComfortSnapAngle,
    ComfortSmoothSpeed,
    ComfortVignette,
    ComfortBlink,
    ComfortBlinkMs,
    ComfortSeated,
    ComfortSeatedHeight,
    // Picture
    PicRefreshRate,
    PicResolutionScale,
    PicMsaa,
    PicTextureFilter,
    PicSharpen,
    PicScreenSmoothing,
    Pic3dResolution,
    PicFoveation,
    PicDrawDistance,
    // HUD
    HudTopScreen,
    HudDistance,
    HudScale,
    HudTouchInput,
    // Hand screen (the DS touch screen held in or worn on a hand)
    HandScreenPlacement,
    HandScreenSide,
    HandScreenAutoShow,
    HandScreenFollowView,
    HandScreenSize,
    HandScreenOffsetX,
    HandScreenOffsetY,
    HandScreenOffsetZ,
    HandScreenTilt,
    HandScreenTurn,
    HandScreenRoll,
    HandScreenDs,
    HandScreenDsSize,
    HandScreenDsScreenScale,
    HandScreenDsHinge,
    HandScreenDsGiant,
    HandScreenDsColor,
    HandScreenDsTwoHands,
    // Game
    GameLanguage,
    GameInvertTurn,
    GameUnitsPerMeter,
    GameShowFps,

    Count
};

// Values of the enum settings. The numbers are the option index stored in the file.
enum class ViewMode : int { ThirdPerson = 0, FirstPerson = 1, GiantScreen = 2 };
enum class CutsceneView : int { GiantScreen = 0, SameAsGameplay = 1 };
enum class FollowMode : int { Smooth = 0, Locked = 1, Teleport = 2 };
enum class MoveRelativeTo : int { Head = 0, LeftController = 1, View = 2 };
enum class BodyDisplay : int { Hidden = 0, HandsAndWeapon = 1, FullBody = 2 };
enum class Hand : int { Left = 0, Right = 1 };
enum class MagicGesture : int { Button = 0, PointAndTrigger = 1 };
enum class DodgeMode : int { Off = 0, StickFlick = 1, Button = 2 };
enum class TurnMode : int { Snap = 0, Smooth = 1, Off = 2 };
enum class TopScreenPlacement : int { Floating = 0, HeadLocked = 1, Hidden = 2 };
enum class BottomScreenPlacement : int { InHand = 0, Floating = 1, Hidden = 2, OnWrist = 3 };
enum class GamePicture : int { Ds = 0, Sharp = 1, Depth = 2 };
enum class HandheldDs : int { Off = 0, Movies = 1, Always = 2, Only = 3 };
enum class DsColor : int { Black = 0, White = 1, Red = 2, Blue = 3, Silver = 4 };
enum class TouchInput : int { Laser = 0, DirectTouch = 1 };
enum class TextureFilter : int { Nearest = 0, Bilinear = 1, Sharp = 2, Smooth = 3 };
// Buttons a gesture can press, in the order the Controls tab lists them.
enum class DsBinding : int { A = 0, B = 1, X = 2, Y = 3, L = 4, R = 5, None = 6 };

struct SettingDef {
    SettingId id;
    const char *tab;    // panel tab and INI section
    const char *key;    // INI key inside the section
    const char *label;  // panel text
    SettingType type;
    float defaultValue;
    float minValue;     // Float/Int range; ignored for Bool/Enum
    float maxValue;
    float step;         // panel increment for Float/Int
    std::vector<const char *> optionKeys;    // Enum: INI spellings
    std::vector<const char *> optionLabels;  // Enum: panel text
    const char *unit;   // shown after the value ("m", "deg", ...), may be ""
    const char *help;   // one-line description under the option
};

const std::vector<SettingDef> &SettingDefs();
const SettingDef &DefOf(SettingId id);
// Tabs in panel order.
const std::vector<const char *> &SettingTabs();

class Settings {
public:
    Settings();

    float Get(SettingId id) const { return values_[static_cast<int>(id)]; }
    bool Bool(SettingId id) const { return Get(id) != 0.0f; }
    int Int(SettingId id) const { return static_cast<int>(Get(id)); }
    template <typename E> E Enum(SettingId id) const { return static_cast<E>(Int(id)); }

    // Clamps to the option's range (or option count) and returns the stored value.
    float Set(SettingId id, float value);
    void SetBool(SettingId id, bool v) { Set(id, v ? 1.0f : 0.0f); }
    template <typename E> void SetEnum(SettingId id, E v) { Set(id, static_cast<float>(v)); }

    // Panel helpers: one step up/down (Float/Int step, Enum wraps, Bool toggles).
    void Step(SettingId id, int direction);
    std::string FormatValue(SettingId id) const;

    void ResetAll();
    void ResetTab(const char *tab);

    // INI text. Load leaves options missing from the text unchanged and appends a line to
    // `warnings` for every key it could not use.
    void LoadIni(const std::string &text, std::vector<std::string> *warnings = nullptr);
    std::string SaveIni() const;

    // Bumped on every change, so the app knows when to rewrite the file.
    unsigned revision() const { return revision_; }

private:
    float values_[static_cast<int>(SettingId::Count)];
    unsigned revision_ = 0;
};

}  // namespace khvr
