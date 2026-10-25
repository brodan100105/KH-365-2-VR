// The VR application: owns the settings, the game source, the view rigs, motion combat and
// input mapping, and draws each headset frame. Platform plumbing (OpenXR, EGL, Android) stays
// in XrPlatform and main.cpp.
#pragma once

#include <memory>
#include <string>

#include "khvr/canvas.h"
#include "khvr/combat.h"
#include "khvr/demo_game.h"
#include "khvr/input_mapper.h"
#include "khvr/settings.h"
#include "khvr/settings_panel.h"
#include "khvr/view_rig.h"
#include "renderer.h"
#include "xr_platform.h"
#ifdef KHVR_HAS_GAME
#include "decomp_game.h"
#endif

namespace khvr {

class VrApp {
public:
    // Reads <filesDir>/khvr.ini (created with defaults when missing).
    void LoadSettings(const std::string &filesDir);
    const Settings &settings() const { return settings_; }
    // The shared folder the ROM, the save and the boot log go in (/sdcard/KHDaysVR), when the app
    // has all-files access; `hasAccess` false keeps everything in the app's own folder.
    void SetSharedDir(const std::string &dir, bool hasAccess);
    // The game could not start for lack of all-files access: main.cpp opens the settings page.
    bool needsStorageAccess() const { return needsStorageAccess_; }

    bool Init(XrPlatform *xr);
    void Frame(const XrFrameInput &in);
    void SaveSettingsIfChanged(bool force = false);

private:
    struct Placement {
        Pose pose;      // tracking space
        float width = 0, height = 0;
    };
    ViewMode EffectiveMode() const;
    void ApplyPlatformSettings();
    void UpdateTextures();
    void UpdatePlacements(const XrFrameInput &in);
    // Laser from the right aim pose against a placed quad: canvas pixel coordinates on a hit.
    static bool Laser(const XrFrameInput &in, int hand, const Placement &p, int texW, int texH,
                      float *px, float *py, float *dist);
    void RenderGiantScreenViews(const XrFrameInput &in);
    void RenderEye(const XrFrameInput &in, int eye);
    void DrawProps(const XrFrameInput &in, const Mat4 &vpTracking);
    // Handheld > Handheld DS: a small DS where the touch screen is, both screens on it, drawn at
    // `scale` of its size and `alpha` opacity (the grow and fade of the hand screen).
    void DrawHandheld(const Mat4 &vpTracking, float scale, float alpha, float smoothing);
    void StartGame();
    // While the game boots and has not drawn anything yet, you stand in a loading area: a
    // stained-glass platform and a panel saying how far the boot got.
    bool Loading() const;
    void UpdateLoading(double now);
    void BuildLoadingArea(float floorY);
    void UpdateLoadingSparkles(double now);
    // The status panel: why the demo runs instead of the game, or the game's boot log.
    std::vector<std::string> StatusLines() const;
    void UpdateStatus(double now);

    XrPlatform *xr_ = nullptr;
    Renderer renderer_;
    std::string settingsPath_;
    Settings settings_;
    unsigned savedRevision_ = 0;

    std::unique_ptr<GameSource> game_;
    DemoGame *demo_ = nullptr;
#ifdef KHVR_HAS_GAME
    DecompGame *decomp_ = nullptr;
#endif
    std::string filesDir_;
    std::string sharedDir_;  // empty without all-files access
    bool needsStorageAccess_ = false;
    std::string notice_;  // why the demo arena runs instead of the game
    ThirdPersonRig thirdPerson_;
    FirstPersonRig firstPerson_;
    CombatRecognizer combat_;
    InputMapper mapper_;
    SettingsPanel panel_;

    RigOutput rig_;                 // this frame's rig result
    GameRequests nextRequests_;     // what the rig asked for, applied at the next Advance
    Vec3 pendingRoomScale_;         // room-scale movement not yet consumed by a game frame
    ViewMode lastMode_ = ViewMode::ThirdPerson;
    float blinkTime_ = 0.0f;        // seconds into the current blink, < 0 when none
    bool recenterNext_ = false;
    float prevTrigger_[2] = {0, 0};

    Texture topTex_, hudTex_, bottomTex_, panelTex_, statusTex_, loadingTex_, stationTex_, glowTex_;
    struct Mote {
        Vec3 position;
        float size, alpha;
    };
    std::vector<Mote> motes_;  // the loading area's drifting lights, this frame
    Canvas loadingCanvas_{768, 192};
    std::vector<uint32_t> sharpScratch_;  // the Sharp filter's enlarged screen
    std::vector<uint32_t> smoothScratch_;  // Smooth filter: the 2x step of Scale4x
    double loadingDrawn_ = -10.0;
    WorldDrawList loadingArea_, loadingBeam_, loadingSparkles_;
    Vec3 loadingCenter_;
    Placement loadingPanel_;
    Canvas statusCanvas_{768, 288};
    double statusDrawn_ = -10.0;
    bool statusVisible_ = false;
    uint32_t screensRevision_ = ~0u;
    RenderTarget giantViews_[2];
    Placement giantScreen_, hudPanel_, bottomPanel_, settingsPanel_, statusPanel_;
    bool placementsInitialized_ = false;
    Vec3 screenAnchor_;             // head position at the last recenter
    float screenYaw_ = 0.0f;
    float hudYaw_ = 0.0f;
    bool laserOnPanel_ = false, laserOnBottom_ = false;
    int laserHand_ = 1;             // the hand the laser comes from
    int handScreenSide_ = 0;        // the hand holding or wearing the bottom screen
    float handScreenShown_ = 0.0f;  // 0 small .. 1 full size (the grow/shrink animation)
    bool handheld_ = false;         // the touch screen is shown as a whole DS, top screen included
    Quat handScreenLock_;           // with Face your view off: its angle relative to the controller
    bool handScreenLockValid_ = false;
    Vec3 laserEnd_;
};

}  // namespace khvr
