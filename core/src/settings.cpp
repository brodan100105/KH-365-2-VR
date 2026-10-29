#include "khvr/settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace khvr {

namespace {

using T = SettingType;
using S = SettingId;

SettingDef B(S id, const char *tab, const char *key, const char *label, bool def,
             const char *help) {
    return {id, tab, key, label, T::Bool, def ? 1.0f : 0.0f, 0, 1, 1, {}, {}, "", help};
}
SettingDef F(S id, const char *tab, const char *key, const char *label, float def, float lo,
             float hi, float step, const char *unit, const char *help) {
    return {id, tab, key, label, T::Float, def, lo, hi, step, {}, {}, unit, help};
}
SettingDef E(S id, const char *tab, const char *key, const char *label, int def,
             std::vector<const char *> keys, std::vector<const char *> labels, const char *help) {
    float hi = static_cast<float>(keys.size() - 1);
    return {id, tab, key, label, T::Enum, static_cast<float>(def), 0, hi, 1,
            std::move(keys), std::move(labels), "", help};
}

std::vector<SettingDef> BuildDefs() {
    std::vector<SettingDef> d;
    // ---- View ----
    d.push_back(E(S::ViewMode, "View", "mode", "View mode", 0,
                  {"third_person", "first_person", "giant_screen"},
                  {"Panoramic (3rd person)", "First person", "Giant screen"},
                  "Panoramic: stand around Roxas in 6DoF. First person: you are the character."));
    d.push_back(B(S::ViewStandInWorld, "View", "stand_in_world", "Stand in the world (experimental)", false,
                  "In gameplay, the game's 3D around you instead of on the giant screen, in the view mode "
                  "above. For now you see what the game's own camera sees; menus and event scenes stay "
                  "on the giant screen."));
    d.push_back(B(S::ViewSmoothMotion, "View", "smooth_motion", "Smooth camera motion", true,
                  "Standing in the world, the view follows Roxas smoothly between the game's frames "
                  "instead of in its steps, which shake most when he changes direction."));
    d.push_back(B(S::ViewDrawAll, "View", "draw_all", "Draw what the game's camera does not see", true,
                  "Standing in the world, the game draws enemies around you even where its own camera "
                  "does not look (it skips those outside its view for the DS). Costs some speed."));
    d.push_back(E(S::CutsceneView, "View", "cutscene_view", "Cutscenes", 0,
                  {"giant_screen", "same_as_gameplay"}, {"Giant screen", "Same as gameplay"},
                  "Watch in-engine cutscenes on the giant screen, or standing in the scene like gameplay "
                  "(with Stand in the world; scenes whose camera cannot be placed stay on the giant screen)."));
    d.push_back(F(S::CutsceneWorldScale, "View", "cutscene_world_scale", "Cutscene world scale", 1.75f, 0.5f,
                  4.0f, 0.05f, "x",
                  "How big in-engine cutscenes are around you when you stand in them: larger fills the "
                  "space more (1 is the game's own scale)."));
    d.push_back(B(S::ModeSwitchButton, "View", "mode_switch_button", "Quick mode switch", true,
                  "Hold the right thumbstick click to swap panoramic and first person."));
    // ---- Screen ----
    d.push_back(F(S::ScreenDistance, "Screen", "distance", "Screen distance", 4.5f, 1.0f, 12.0f,
                  0.25f, "m", "Distance to the giant screen."));
    d.push_back(F(S::ScreenWidth, "Screen", "width", "Screen width", 5.0f, 1.0f, 16.0f, 0.25f, "m",
                  "Width of the giant screen (the DS top screen is 4:3)."));
    d.push_back(F(S::ScreenCurvature, "Screen", "curvature", "Curvature", 0.2f, 0.0f, 1.0f, 0.05f,
                  "", "0 is flat; 1 wraps the screen around you."));
    d.push_back(B(S::ScreenStereo, "Screen", "stereo", "Stereoscopic 3D", true,
                  "Render the game scene separately for each eye on the giant screen."));
    d.push_back(F(S::ScreenDepth, "Screen", "depth", "3D depth", 1.0f, 0.0f, 2.0f, 0.1f, "",
                  "Strength of the stereoscopic effect on the giant screen."));
    d.push_back(E(S::ScreenPicture, "Screen", "game_picture", "Game picture", 1, {"ds", "sharp", "depth"},
                  {"DS picture", "Sharp", "3D depth (experimental)"},
                  "DS picture: the game's 3D as the DS draws it. Sharp: drawn again by the headset at its "
                  "resolution, with the game's own textures. 3D depth: the same, seen with both eyes, "
                  "with depth behind the screen. Menus and text stay as the DS draws them."));
    d.push_back(B(S::ScreenBoth, "Screen", "both_screens", "Both screens", false,
                  "The giant screen shows both DS screens, the top one above the touch screen, each "
                  "0.6 of the screen width and flat. Point at the lower one to touch it. Hand screen > "
                  "Placement: Hidden then leaves nothing in your hand."));
    d.push_back(B(S::Passthrough, "Screen", "passthrough", "Passthrough", false,
                  "Show your room around the giant screen and panels. Costs GPU time."));
    // ---- Camera (third person) ----
    d.push_back(F(S::TpWorldScale, "Camera", "world_scale", "World scale", 1.0f, 0.02f, 1.5f,
                  0.02f, "x", "1 is life size; small values turn the level into a diorama."));
    d.push_back(F(S::TpDistance, "Camera", "distance", "Distance behind", 3.0f, 0.0f, 10.0f,
                  0.25f, "m", "How far in front of you the character stands."));
    d.push_back(F(S::TpHeight, "Camera", "height", "Viewpoint height", 1.0f, -1.0f, 6.0f, 0.25f,
                  "m", "How far your eyes sit above a standing person's (1.6 m) over the character's floor, "
                  "measured from your head when you recenter (right stick click), however tall you "
                  "are or wherever you stand."));
    d.push_back(E(S::TpFollow, "Camera", "follow", "Follow", 0, {"smooth", "locked", "teleport"},
                  {"Smooth", "Locked", "Teleport when far"},
                  "How the view follows the character. Teleport is the most comfortable."));
    d.push_back(F(S::TpFollowSpeed, "Camera", "follow_speed", "Follow speed", 3.0f, 0.5f, 10.0f,
                  0.5f, "", "How quickly the view catches up in Smooth mode."));
    d.push_back(F(S::TpDeadzone, "Camera", "deadzone", "Follow dead zone", 0.5f, 0.0f, 4.0f,
                  0.1f, "m", "The character can move this far before the view follows."));
    d.push_back(E(S::TpMoveRelativeTo, "Camera", "move_relative_to", "Stick direction", 0,
                  {"head", "left_controller", "view"}, {"Head", "Left controller", "View"},
                  "What 'forward' on the movement stick means."));
    d.push_back(B(S::TpWallFade, "Camera", "wall_fade", "Fade walls", true,
                  "Fade geometry between you and the character."));
    // ---- First person ----
    d.push_back(F(S::FpWorldScale, "First person", "world_scale", "World scale", 1.75f, 0.5f, 4.0f, 0.05f, "x",
                  "How big the world looks around you in first person: above 1 larger, below 1 smaller "
                  "(on top of Match character height)."));
    d.push_back(F(S::FpEyeHeight, "First person", "eye_height", "Character eye height", 1.7f,
                  1.0f, 2.5f, 0.05f, "m", "Eye height of the character you play."));
    d.push_back(B(S::FpMatchCharacterHeight, "First person", "match_height",
                  "Match character height", true,
                  "Scale the world so your real eye height matches the character's."));
    d.push_back(E(S::FpMoveRelativeTo, "First person", "move_relative_to", "Stick direction", 1,
                  {"head", "left_controller", "view"}, {"Head", "Left controller", "View"},
                  "What 'forward' on the movement stick means."));
    d.push_back(B(S::FpRoomScale, "First person", "room_scale", "Room-scale walking", true,
                  "Walking in your room moves the character. Walls push you back."));
    // A new key: Body without head became the default, so an old file's choice is not used.
    d.push_back(E(S::FpShowBody, "First person", "body", "Show body", 3,
                  {"hidden", "hands_and_weapon", "full_body", "without_head"},
                  {"Hidden", "Hands and Keyblade", "Full body", "Body without head"},
                  "Which parts of the character model are drawn in first person. Body without head: "
                  "the game's Roxas and his Keyblade as the game draws them, without his head and hair "
                  "in front of your eyes (VR hands, no VR Keyblade). Hidden and Hands and Keyblade "
                  "leave the game's own Roxas out (the VR hands and Keyblade stay)."));
    // A new key: the default came down (a piece of his chin stayed at the base of his neck).
    d.push_back(F(S::FpHeadCut, "First person", "head_cut2", "Head cut height", 0.78f, 0.5f, 1.0f, 0.01f, "",
                  "Body without head: the part of Roxas's height above which he is not drawn (his head "
                  "and hair). Lower it if hair still shows; raise it if his shoulders go."));
    d.push_back(E(S::FpWeaponHand, "First person", "weapon_hand", "Keyblade hand", 1,
                  {"left", "right"}, {"Left", "Right"}, "The hand that holds the Keyblade."));
    d.push_back(B(S::FpDualWield, "First person", "dual_wield", "Dual wield", false,
                  "Put a second Keyblade in the other hand when the character has one."));
    d.push_back(F(S::FpGripAngle, "First person", "grip_angle", "Grip angle", 0.0f, -60.0f, 60.0f,
                  5.0f, "deg", "Tilts the Keyblade in your hand."));
    d.push_back(F(S::FpKeybladeLength, "First person", "keyblade_length", "Keyblade length",
                  1.0f, 0.5f, 1.5f, 0.05f, "x", "Visual and hit length of the Keyblade."));
    d.push_back(F(S::FpKeybladeGrip, "First person", "keyblade_grip", "Keyblade grip", 0.0f, -0.3f, 0.3f, 0.01f, "m",
                  "Slides the Keyblade along its blade in your hand: up to hold it lower on the handle, "
                  "down to hold it higher."));
    d.push_back(B(S::FpArmsFollow, "First person", "arms_follow", "Arms follow your hands", true,
                  "With Show body: Body without head or Full body, Roxas's own arms reach for your "
                  "controllers (his shoulders, elbows and wrists bend; his legs keep the game's "
                  "animation)."));
    // ---- Combat ----
    d.push_back(B(S::CombatMotion, "Combat", "motion_combat", "Motion combat", true,
                  "Swing the controller to attack (first person)."));
    d.push_back(B(S::CombatPhysicalHits, "Combat", "physical_hits", "Physical hits only", false,
                  "Attacks only land where the Keyblade touches an enemy."));
    d.push_back(F(S::CombatSwingSpeed, "Combat", "swing_speed", "Swing threshold", 2.2f, 1.0f,
                  6.0f, 0.1f, "m/s", "Blade-tip speed a swing needs to count as an attack."));
    // The swing keys are new with each raise of their defaults, so older files' values do not hold them back.
    d.push_back(F(S::CombatArmSpeed, "Combat", "arm_swing_speed2", "Arm swing speed", 1.3f, 0.0f, 4.0f, 0.1f, "m/s",
                  "How fast your hand itself must move for a swing to attack, so the arm swings and not "
                  "just the wrist (a flick moves the tip fast but the hand little). 0: the tip alone."));
    d.push_back(F(S::CombatArmDistance, "Combat", "swing_length2", "Swing length", 0.33f, 0.0f, 1.2f, 0.05f, "m",
                  "How far your hand must travel in one fast motion before the swing attacks: longer "
                  "asks for a full swing (the attack comes that much later in it). 0: at once."));
    d.push_back(F(S::CombatSwingCooldown, "Combat", "swing_delay", "Swing cooldown", 0.35f,
                  0.05f, 1.5f, 0.01f, "s", "Minimum time between two swings."));
    d.push_back(F(S::CombatAimAssist, "Combat", "aim_assist", "Aim assist", 0.5f, 0.0f, 1.0f,
                  0.05f, "", "How strongly swings turn the character toward the nearest enemy."));
    d.push_back(B(S::CombatGuardGesture, "Combat", "guard_gesture", "Guard gesture", true,
                  "Hold the Keyblade across your body to block."));
    d.push_back(E(S::CombatMagicGesture, "Combat", "magic_gesture", "Magic / commands", 1,
                  {"button", "point_and_trigger"}, {"Button", "Point and pull trigger"},
                  "How the selected command is cast."));
    d.push_back(B(S::CombatJumpGesture, "Combat", "jump_gesture", "Physical jump", false,
                  "Jumping in your room makes the character jump."));
    d.push_back(E(S::CombatDodge, "Combat", "dodge", "Dodge roll", 1,
                  {"off", "stick_flick", "button"}, {"Off", "Flick the stick", "Button"},
                  "How dodge rolls are triggered."));
    d.push_back(F(S::CombatHaptics, "Combat", "haptics", "Haptics", 0.7f, 0.0f, 1.0f, 0.1f, "",
                  "Controller vibration on hits, blocks and swings."));
    // ---- Controls ----
    // The defaults follow the game's Type A controls; change them here if you play Type B.
    d.push_back(E(S::CtrlFirstPersonButtons, "Controls", "first_person_buttons", "First person buttons", 0,
                  {"same", "first_person"}, {"Same as other views", "First person profile"},
                  "Same as other views: every button presses what it does in third person (the triggers "
                  "L and R); the gestures (swing, guard, dodge flick) add to them. First person profile: "
                  "the weapon hand's trigger is L, the other hand's trigger casts (pointing at an enemy) "
                  "and its grip locks on."));
    const std::vector<const char *> btnKeys{"a", "b", "x", "y", "l", "r", "none"};
    const std::vector<const char *> btnLabels{"A", "B", "X", "Y", "L", "R", "None"};
    d.push_back(E(S::CtrlAttackButton, "Controls", "attack", "Swing presses", 0, btnKeys, btnLabels,
                  "DS button a Keyblade swing presses (the attack command)."));
    d.push_back(E(S::CtrlGuardButton, "Controls", "guard", "Guard presses", 3, btnKeys, btnLabels,
                  "DS button held while you hold the guard pose (Block)."));
    d.push_back(E(S::CtrlDodgeButton, "Controls", "dodge", "Dodge presses", 3, btnKeys, btnLabels,
                  "DS button a dodge flick presses, with the flick direction held (Dodge Roll)."));
    d.push_back(E(S::CtrlMagicButton, "Controls", "magic", "Cast presses", 2, btnKeys, btnLabels,
                  "DS button pointing and pulling the trigger presses (the selected command)."));
    d.push_back(E(S::CtrlJumpButton, "Controls", "jump", "Physical jump presses", 1, btnKeys,
                  btnLabels, "DS button a physical jump presses."));
    d.push_back(E(S::CtrlLockOnButton, "Controls", "lock_on", "Grip presses", 5, btnKeys,
                  btnLabels, "DS button the off-hand grip presses in first person (lock-on), with First person "
                  "buttons: First person profile."));
    // ---- Comfort ----
    d.push_back(E(S::ComfortProfile, "Comfort", "profile", "Comfort profile", 2,
                  {"custom", "low", "medium", "high"}, {"Custom", "Low", "Medium", "High"},
                  "Sets the options below for how much motion you are used to. Low: the vignette at "
                  "full strength and snap turning. Medium: no vignette, snap turning. High: no "
                  "vignette, smooth turning. Changing them yourself makes it Custom."));
    d.push_back(E(S::ComfortTurn, "Comfort", "turn", "Turning", 0, {"snap", "smooth", "off"},
                  {"Snap", "Smooth", "Off"}, "Right stick turning."));
    d.push_back(F(S::ComfortSnapAngle, "Comfort", "snap_angle", "Snap angle", 45.0f, 15.0f, 90.0f,
                  15.0f, "deg", "Angle of one snap turn."));
    d.push_back(F(S::ComfortSmoothSpeed, "Comfort", "smooth_speed", "Smooth turn speed", 120.0f,
                  30.0f, 360.0f, 10.0f, "deg/s", "Speed of smooth turning."));
    d.push_back(B(S::ComfortVignetteOn, "Comfort", "vignette_on", "Vignette", false,
                  "Standing in the world, darkens the edges of your view while the view moves or turns "
                  "without you (the stick, following Roxas, a cutscene's camera), fading in and out "
                  "smoothly. Never on the giant screen."));
    d.push_back(F(S::ComfortVignette, "Comfort", "vignette", "Vignette strength", 0.6f, 0.1f, 3.0f, 0.05f,
                  "", "How dark the vignette gets: 0.1 faint but still there, 1 full black at the edges; "
                  "above 1 the full black reaches further in over its soft edge."));
    // A new key: the size's scale changed (1 is the old 0.5), so an old file's value is not used.
    d.push_back(F(S::ComfortVignetteSize, "Comfort", "vignette_area", "Vignette size", 1.0f, 0.1f, 2.0f,
                  0.05f, "", "How much of your view the vignette covers at its fullest, from the edges "
                  "in: one circle round both eyes. 2 leaves only a small clear circle."));
    d.push_back(B(S::ComfortBlink, "Comfort", "blink", "Blink on cuts", true,
                  "Briefly fade to black when the view jumps (camera cuts, snap turns)."));
    d.push_back(F(S::ComfortBlinkMs, "Comfort", "blink_ms", "Blink length", 150.0f, 50.0f, 500.0f,
                  10.0f, "ms", "Duration of a blink."));
    d.push_back(B(S::ComfortSeated, "Comfort", "seated", "Seated play", false,
                  "Adds height so you can play sitting down."));
    d.push_back(F(S::ComfortSeatedHeight, "Comfort", "seated_height", "Seated height boost",
                  0.45f, 0.0f, 1.0f, 0.05f, "m", "Height added in seated play."));
    // ---- Picture ----
    d.push_back(E(S::PicRefreshRate, "Picture", "refresh_rate", "Refresh rate", 2,
                  {"72", "90", "120"}, {"72 Hz", "90 Hz", "120 Hz"},
                  "Headset refresh rate. The game runs at its own 60 frames a second; at 120 Hz each "
                  "of its frames shows for exactly two, which keeps turning smooth."));
    d.push_back(B(S::PicParallelCapture, "Picture", "draw_while_running", "Draw while the game runs", true,
                  "Draw each frame's screens while the game runs the next one, as the DS does, so "
                  "heavy scenes keep all their frames. Turn off if pictures ever flicker or tear."));
    d.push_back(B(S::PicInBetweenFrames, "Picture", "in_between_frames", "In-between frames", true,
                  "Standing in the world, moving things glide between the game's 60 frames a second "
                  "at the headset's rate (they show a game frame later). Off: the game's own frames."));
    d.push_back(F(S::PicResolutionScale, "Picture", "resolution_scale", "Resolution scale", 1.0f,
                  0.5f, 1.5f, 0.05f, "x", "Per-eye render resolution relative to the headset's. Applies on restart."));
    d.push_back(E(S::PicMsaa, "Picture", "msaa", "Anti-aliasing", 2, {"off", "2x", "4x"},
                  {"Off", "2x MSAA", "4x MSAA"}, "Multisample anti-aliasing. Applies on restart."));
    d.push_back(E(S::PicTextureFilter, "Picture", "texture_filter", "Texture filtering", 2,
                  {"nearest", "bilinear", "sharp", "smooth"},
                  {"Nearest (DS look)", "Bilinear", "Sharp (pixel art)", "Smooth (upscale)"},
                  "How the DS screens are enlarged: Sharp keeps pixels crisp with smooth edges; Smooth "
                  "also rounds diagonal edges (Scale4x) instead of showing them as steps."));
    d.push_back(F(S::PicSharpen, "Picture", "sharpen", "Sharpening", 0.3f, 0.0f, 1.0f, 0.05f, "",
                  "Sharpens the final picture."));
    d.push_back(F(S::PicScreenSmoothing, "Picture", "screen_smoothing", "Screen smoothing", 0.4f, 0.0f, 1.0f, 0.05f, "",
                  "Blends neighbouring DS pixels of similar colour (gradients, dithering, jagged edges) "
                  "while leaving sharp contrast like text untouched. 0 is off."));
    d.push_back(E(S::Pic3dResolution, "Picture", "resolution_3d", "3D resolution", 1, {"1x", "2x", "3x", "4x"},
                  {"1x (the DS's)", "2x", "3x", "4x"},
                  "Draws the game's 3D at a multiple of the DS's 256 x 192; menus and text stay as the DS "
                  "draws them. Higher is sharper and costs more of the headset's processor."));
    d.push_back(E(S::PicFoveation, "Picture", "foveation", "Fixed foveation", 2,
                  {"off", "low", "medium", "high"}, {"Off", "Low", "Medium", "High"},
                  "Lowers resolution at the edges of the lenses to save GPU time."));
    d.push_back(F(S::PicDrawDistance, "Picture", "draw_distance", "Draw distance", 1.5f, 1.0f,
                  4.0f, 0.25f, "x", "Widens the game's culling so you can look around freely."));
    // ---- HUD ----
    d.push_back(E(S::HudTopScreen, "HUD", "top_screen", "HUD (top screen)", 0,
                  {"floating", "head_locked", "hidden"}, {"Floating panel", "Head-locked", "Hidden"},
                  "Where the health bars and command panel are shown."));
    d.push_back(F(S::HudDistance, "HUD", "distance", "Panel distance", 0.6f, 0.5f, 3.0f, 0.1f, "m",
                  "Distance of the floating panels."));
    d.push_back(F(S::HudHeight, "HUD", "height", "Panel height", -0.5f, -1.0f, 0.4f, 0.05f, "m",
                  "How far below your eyes (negative) or above them the HUD panel sits, so the health "
                  "bar and commands stay out of the way. Menus are on the giant screen."));
    d.push_back(B(S::HudFillView, "HUD", "fill_view", "Fill your view", true,
                  "The HUD as large as your view, centred on it, so the health bar and commands sit at "
                  "its edges as on the DS's screen. Off: a panel of Panel scale at Panel height."));
    d.push_back(F(S::HudFillAngle, "HUD", "fill_angle", "View it fills", 90.0f, 30.0f, 110.0f, 5.0f, "deg",
                  "How wide the HUD spans when it fills your view."));
    d.push_back(F(S::HudOffsetX, "HUD", "offset_x", "Left / right", 0.0f, -40.0f, 40.0f, 1.0f, "deg",
                  "Moves the HUD across your view: negative left, positive right."));
    d.push_back(F(S::HudOffsetY, "HUD", "offset_y", "Up / down", -16.0f, -45.0f, 30.0f, 1.0f, "deg",
                  "Moves the HUD up or down your view: negative down, so the portrait and commands sit "
                  "below Roxas."));
    d.push_back(F(S::HudScaleX, "HUD", "scale_x", "Width", 1.1f, 0.5f, 2.0f, 0.05f, "x",
                  "Stretches the HUD across."));
    d.push_back(F(S::HudScaleY, "HUD", "scale_y", "Height", 1.0f, 0.5f, 2.0f, 0.05f, "x",
                  "Stretches the HUD up and down."));
    d.push_back(F(S::HudFollowAngle, "HUD", "follow_angle", "Follow after turning", 45.0f, 5.0f, 60.0f, 5.0f,
                  "deg", "How far you can look away from the floating HUD before it comes back in front "
                  "of you."));
    d.push_back(F(S::HudFollowDelay, "HUD", "follow_delay", "Follow delay", 0.0f, 0.0f, 10.0f, 0.5f, "s",
                  "How long you look away before the HUD glides back in front of you, so glancing "
                  "around does not move it."));
    d.push_back(F(S::HudScale, "HUD", "scale", "Panel scale", 1.0f, 0.5f, 2.0f, 0.1f, "x",
                  "Size of the floating HUD panels."));
    d.push_back(E(S::HudTouchInput, "HUD", "touch_input", "Touch input", 0,
                  {"laser", "direct_touch"}, {"Laser pointer", "Touch with finger"},
                  "How you touch the bottom screen."));
    // ---- Hand screen ----
    d.push_back(E(S::HandScreenPlacement, "Hand screen", "placement", "Placement", 0,
                  {"hand", "floating", "hidden", "wrist"},
                  {"Above your hand", "Floating panel", "Hidden", "On your wrist"},
                  "Where the DS touch screen (map, menus) is: held, worn like a watch, or floating."));
    d.push_back(E(S::HandScreenSide, "Hand screen", "side", "Hand", 0, {"left", "right"},
                  {"Left", "Right"}, "Which hand holds or wears it; the other hand's laser touches it."));
    d.push_back(B(S::HandScreenAutoShow, "Hand screen", "auto_show", "Show when looked at", true,
                  "Full size when you look at your hand or wrist; small and see-through otherwise."));
    d.push_back(B(S::HandScreenFollowView, "Hand screen", "follow_view", "Face your view", true,
                  "On: turns to face your eyes. Off: fixed to your hand like a held object, at the "
                  "angle it had when you switched this off or last recentered."));
    d.push_back(F(S::HandScreenSize, "Hand screen", "size", "Size", 0.20f, 0.10f, 0.40f, 0.01f, "m",
                  "Width of the screen in your hand or on your wrist."));
    d.push_back(F(S::HandScreenOffsetX, "Hand screen", "move_right", "Move right / left", 0.0f, -0.3f,
                  0.3f, 0.01f, "m", "Moves the screen along its own width."));
    d.push_back(F(S::HandScreenOffsetY, "Hand screen", "move_up", "Move up / down", 0.0f, -0.3f, 0.3f,
                  0.01f, "m", "Moves the screen along its own height."));
    d.push_back(F(S::HandScreenOffsetZ, "Hand screen", "move_toward", "Move toward / away", 0.0f, -0.3f,
                  0.3f, 0.01f, "m", "Moves the screen toward you (+) or away from you (-)."));
    d.push_back(F(S::HandScreenTilt, "Hand screen", "tilt", "Tilt", 0.0f, -90.0f, 90.0f, 5.0f, "deg",
                  "The screen faces your eyes; this tilts its top toward you (+) or away (-)."));
    d.push_back(F(S::HandScreenTurn, "Hand screen", "turn", "Turn", 0.0f, -90.0f, 90.0f, 5.0f, "deg",
                  "Turns the screen left or right."));
    d.push_back(F(S::HandScreenRoll, "Hand screen", "roll", "Roll", 0.0f, -90.0f, 90.0f, 5.0f, "deg",
                  "Rolls the screen clockwise or counterclockwise."));
    // ---- Handheld ----
    d.push_back(E(S::HandScreenDs, "Handheld", "show", "Handheld DS", 0, {"off", "movies", "always", "only"},
                  {"Off (touch screen only)", "For movies", "Always", "DS only"},
                  "Holds a whole DS: the top screen above the touch screen on a small handheld, "
                  "opened on its hinge, where the touch screen is (Hand screen > Placement). It grows, "
                  "shrinks and fades as one. For movies: only while a movie plays. DS only: the "
                  "handheld is all there is, no giant screen, always at full size (with Screen > "
                  "Passthrough, a DS in your real room)."));
    d.push_back(F(S::HandScreenDsSize, "Handheld", "size", "Size", 0.20f, 0.10f, 0.50f, 0.01f, "m",
                  "Width of the whole handheld, screens and all (a real DS Lite: 0.13 m)."));
    d.push_back(F(S::HandScreenDsScreenScale, "Handheld", "screen_scale", "Screen size", 100.0f, 60.0f, 140.0f,
                  5.0f, "%", "The screens' size on the handheld: 100% is a DS Lite's, larger covers more of "
                  "the shell."));
    d.push_back(F(S::HandScreenDsHinge, "Handheld", "hinge", "Hinge angle", 160.0f, 90.0f, 175.0f, 5.0f, "deg",
                  "How far it is opened: the angle between the two halves (180 would be flat)."));
    d.push_back(B(S::HandScreenDsGiant, "Handheld", "giant_screen_too", "Giant screen too", false,
                  "Keep the giant screen (or the top screen panel) while the handheld shows the top screen."));
    d.push_back(E(S::HandScreenDsColor, "Handheld", "color", "Color", 0,
                  {"black", "white", "red", "blue", "silver"}, {"Black", "White", "Red", "Blue", "Silver"},
                  "The handheld's shell."));
    d.push_back(B(S::HandScreenDsTwoHands, "Handheld", "two_hands", "Hold with both hands", true,
                  "Bring your other controller up to the handheld and it moves to sit between your "
                  "two hands, as a DS is held. Pull the hands apart and it stays with the hand that "
                  "stayed by it, so this also passes it from one hand to the other. While you point "
                  "at the touch screen, the other hand must come much closer to take hold."));
    // ---- Game ----
    d.push_back(E(S::GameLanguage, "Game", "language", "Language", 0,
                  {"english", "french", "german", "italian", "spanish"},
                  {"English", "French", "German", "Italian", "Spanish"},
                  "Game text language (EU release). Applies on the next launch."));
    d.push_back(B(S::GameInvertTurn, "Game", "invert_turn", "Invert turning", false,
                  "Swap the direction of right-stick turning."));
    d.push_back(B(S::GameAnalogMove, "Game", "analog_move", "Move any direction", true,
                  "Roxas walks and runs the exact way the thumbstick points instead of the DS "
                  "D-pad's eight directions (in the field; menus keep the D-pad)."));
    d.push_back(F(S::GameUnitsPerMeter, "Game", "units_per_meter", "World units per meter", 1.0f,
                  0.05f, 64.0f, 0.05f, "",
                  "Calibration: game world units in one real meter at life size."));
    d.push_back(B(S::GameShowFps, "Game", "show_fps", "Show frame rate", false,
                  "Shows game and headset frame rates on the HUD."));
    return d;
}

bool ParseFloat(const std::string &s, float *out) {
    const char *begin = s.c_str();
    char *end = nullptr;
    float v = std::strtof(begin, &end);
    if (end == begin) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = v;
    return true;
}

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string FormatNumber(float v, float step) {
    char buf[32];
    // As many decimals as the step needs: 1 -> "3", 0.5 -> "3.5", 0.25 -> "3.25".
    int decimals = 0;
    while (decimals < 3 && std::fabs(step - std::round(step)) > 1e-4f) {
        step *= 10.0f;
        ++decimals;
    }
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

const std::vector<SettingDef> &SettingDefs() {
    static const std::vector<SettingDef> defs = [] {
        std::vector<SettingDef> d = BuildDefs();
        // The table must list every id once, in enum order, so DefOf can index it.
        for (size_t i = 0; i < d.size(); ++i)
            if (static_cast<size_t>(d[i].id) != i) std::abort();
        if (d.size() != static_cast<size_t>(SettingId::Count)) std::abort();
        return d;
    }();
    return defs;
}

const SettingDef &DefOf(SettingId id) { return SettingDefs()[static_cast<int>(id)]; }

const std::vector<const char *> &SettingTabs() {
    static const std::vector<const char *> tabs = [] {
        std::vector<const char *> t;
        for (const SettingDef &d : SettingDefs()) {
            bool seen = false;
            for (const char *x : t) seen |= std::strcmp(x, d.tab) == 0;
            if (!seen) t.push_back(d.tab);
        }
        return t;
    }();
    return tabs;
}

Settings::Settings() { ResetAll(); }

float Settings::Set(SettingId id, float value) {
    const float before = Get(id);
    value = Store(id, value);
    if (value != before) {
        if (id == SettingId::ComfortProfile) ApplyComfortProfile();
        else if (id == SettingId::ComfortTurn || id == SettingId::ComfortVignetteOn) MatchComfortProfile();
    }
    return value;
}

void Settings::ApplyComfortProfile() {
    switch (Enum<ComfortProfile>(SettingId::ComfortProfile)) {
    case ComfortProfile::Custom: return;
    case ComfortProfile::Low:
        Store(SettingId::ComfortVignetteOn, 1.0f);
        Store(SettingId::ComfortVignette, 1.0f);
        Store(SettingId::ComfortTurn, static_cast<float>(TurnMode::Snap));
        return;
    case ComfortProfile::Medium:
        Store(SettingId::ComfortVignetteOn, 0.0f);
        Store(SettingId::ComfortTurn, static_cast<float>(TurnMode::Snap));
        return;
    case ComfortProfile::High:
        Store(SettingId::ComfortVignetteOn, 0.0f);
        Store(SettingId::ComfortTurn, static_cast<float>(TurnMode::Smooth));
        return;
    }
}

// The profile names what the turning and vignette options are set to.
void Settings::MatchComfortProfile() {
    const bool vignette = Bool(SettingId::ComfortVignetteOn);
    ComfortProfile p = ComfortProfile::Custom;
    switch (Enum<TurnMode>(SettingId::ComfortTurn)) {
    case TurnMode::Snap: p = vignette ? ComfortProfile::Low : ComfortProfile::Medium; break;
    case TurnMode::Smooth: p = vignette ? ComfortProfile::Custom : ComfortProfile::High; break;
    case TurnMode::Off: break;
    }
    Store(SettingId::ComfortProfile, static_cast<float>(p));
}

float Settings::Store(SettingId id, float value) {
    const SettingDef &d = DefOf(id);
    if (std::isnan(value)) value = d.defaultValue;
    switch (d.type) {
    case SettingType::Bool: value = value != 0.0f ? 1.0f : 0.0f; break;
    case SettingType::Int:
    case SettingType::Enum: value = std::round(value); [[fallthrough]];
    case SettingType::Float:
        if (value < d.minValue) value = d.minValue;
        if (value > d.maxValue) value = d.maxValue;
        break;
    }
    float &slot = values_[static_cast<int>(id)];
    if (slot != value) {
        slot = value;
        ++revision_;
    }
    return value;
}

void Settings::Step(SettingId id, int direction) {
    const SettingDef &d = DefOf(id);
    float v = Get(id);
    switch (d.type) {
    case SettingType::Bool: Set(id, v != 0.0f ? 0.0f : 1.0f); break;
    case SettingType::Enum: {
        int n = static_cast<int>(d.optionKeys.size());
        int i = ((static_cast<int>(v) + direction) % n + n) % n;
        Set(id, static_cast<float>(i));
        break;
    }
    case SettingType::Int:
    case SettingType::Float: {
        // Snap to the step grid so repeated steps do not accumulate float error.
        float steps = std::round((v - d.minValue) / d.step) + static_cast<float>(direction);
        Set(id, d.minValue + steps * d.step);
        break;
    }
    }
}

std::string Settings::FormatValue(SettingId id) const {
    const SettingDef &d = DefOf(id);
    switch (d.type) {
    case SettingType::Bool: return Bool(id) ? "On" : "Off";
    case SettingType::Enum: return d.optionLabels[Int(id)];
    case SettingType::Int: return std::to_string(Int(id)) + (*d.unit ? std::string(" ") + d.unit : "");
    case SettingType::Float: {
        std::string s = FormatNumber(Get(id), d.step);
        if (*d.unit) s += (std::strcmp(d.unit, "x") == 0 ? "" : " ") + std::string(d.unit);
        return s;
    }
    }
    return "";
}

void Settings::ResetAll() {
    for (const SettingDef &d : SettingDefs()) values_[static_cast<int>(d.id)] = d.defaultValue;
    ++revision_;
}

void Settings::ResetTab(const char *tab) {
    for (const SettingDef &d : SettingDefs())
        if (std::strcmp(d.tab, tab) == 0) Set(d.id, d.defaultValue);
}

void Settings::LoadIni(const std::string &text, std::vector<std::string> *warnings) {
    auto warn = [&](int line, const std::string &msg) {
        if (warnings) warnings->push_back("line " + std::to_string(line) + ": " + msg);
    };
    std::istringstream in(text);
    std::string raw, section;
    int lineNo = 0;
    bool comfortSeen = false, profileSeen = false;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = Trim(raw);
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line = Trim(line.substr(0, comment));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') { warn(lineNo, "malformed section"); continue; }
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { warn(lineNo, "expected key = value"); continue; }
        std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        const SettingDef *def = nullptr;
        for (const SettingDef &d : SettingDefs())
            if (section == d.tab && key == d.key) def = &d;
        if (!def) { warn(lineNo, "unknown option [" + section + "] " + key); continue; }
        comfortSeen |= std::strcmp(def->tab, "Comfort") == 0;
        profileSeen |= def->id == SettingId::ComfortProfile;

        switch (def->type) {
        case SettingType::Bool:
            if (value == "true" || value == "on" || value == "1") SetBool(def->id, true);
            else if (value == "false" || value == "off" || value == "0") SetBool(def->id, false);
            else warn(lineNo, key + ": expected true or false");
            break;
        case SettingType::Enum: {
            bool found = false;
            for (size_t i = 0; i < def->optionKeys.size(); ++i)
                if (value == def->optionKeys[i]) {
                    Set(def->id, static_cast<float>(i));
                    found = true;
                }
            if (!found) warn(lineNo, key + ": unknown value '" + value + "'");
            break;
        }
        case SettingType::Int:
        case SettingType::Float: {
            float v;
            if (!ParseFloat(value, &v)) { warn(lineNo, key + ": expected a number"); break; }
            if (Set(def->id, v) != v) warn(lineNo, key + ": out of range, clamped");
            break;
        }
        }
    }
    // Files from before the comfort profiles had the vignette on only because it was the default
    // then; it is off by default now.
    if (comfortSeen && !profileSeen) SetBool(SettingId::ComfortVignetteOn, false);
}

std::string Settings::SaveIni() const {
    std::string out = "; Kingdom Hearts 358/2 Days VR settings. Edited by the in-game VR panel;\n"
                      "; see docs/SETTINGS.md for every option.\n";
    for (const char *tab : SettingTabs()) {
        out += "\n[";
        out += tab;
        out += "]\n";
        for (const SettingDef &d : SettingDefs()) {
            if (std::strcmp(d.tab, tab) != 0) continue;
            out += d.key;
            out += " = ";
            switch (d.type) {
            case SettingType::Bool: out += Bool(d.id) ? "true" : "false"; break;
            case SettingType::Enum: out += d.optionKeys[Int(d.id)]; break;
            case SettingType::Int: out += std::to_string(Int(d.id)); break;
            case SettingType::Float: {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", Get(d.id));
                out += buf;
                break;
            }
            }
            out += "\n";
        }
    }
    return out;
}

}  // namespace khvr
