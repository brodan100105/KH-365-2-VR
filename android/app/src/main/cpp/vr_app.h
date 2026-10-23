// The VR application: owns the settings, the game source, the view rigs, motion combat and
// input mapping, and draws each headset frame. Platform plumbing (OpenXR, EGL, Android) stays
// in XrPlatform and main.cpp.
#pragma once

#include <memory>
#include <string>

#include "khvr/combat.h"
#include "khvr/demo_game.h"
#include "khvr/input_mapper.h"
#include "khvr/settings.h"
#include "khvr/settings_panel.h"
#include "khvr/view_rig.h"
#include "renderer.h"
#include "xr_platform.h"

namespace khvr {

class VrApp {
public:
    // Reads <filesDir>/khvr.ini (created with defaults when missing).
    void LoadSettings(const std::string &filesDir);
    const Settings &settings() const { return settings_; }

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
    static bool Laser(const XrFrameInput &in, const Placement &p, int texW, int texH, float *px,
                      float *py, float *dist);
    void RenderGiantScreenViews(const XrFrameInput &in);
    void RenderEye(const XrFrameInput &in, int eye);
    void DrawProps(const XrFrameInput &in, const Mat4 &vpTracking);

    XrPlatform *xr_ = nullptr;
    Renderer renderer_;
    std::string settingsPath_;
    Settings settings_;
    unsigned savedRevision_ = 0;

    std::unique_ptr<DemoGame> game_;
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

    Texture topTex_, hudTex_, bottomTex_, panelTex_;
    uint32_t screensRevision_ = ~0u;
    RenderTarget giantViews_[2];
    Placement giantScreen_, hudPanel_, bottomPanel_, settingsPanel_;
    bool placementsInitialized_ = false;
    Vec3 screenAnchor_;             // head position at the last recenter
    float screenYaw_ = 0.0f;
    float hudYaw_ = 0.0f;
    bool laserOnPanel_ = false, laserOnBottom_ = false;
    Vec3 laserEnd_;
};

}  // namespace khvr
