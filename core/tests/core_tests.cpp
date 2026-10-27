#include <cstring>
// Unit tests for the platform-independent VR core. Run with `ctest` or ./khvr_core_tests.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "khvr/combat.h"
#include "khvr/demo_game.h"
#include "khvr/input_mapper.h"
#include "khvr/math.h"
#include "khvr/settings.h"
#include "khvr/upscale.h"
#include "khvr/settings_panel.h"
#include "khvr/view_rig.h"

using namespace khvr;

namespace {

int gFailures = 0;
std::vector<std::pair<const char *, std::function<void()>>> &Tests() {
    static std::vector<std::pair<const char *, std::function<void()>>> t;
    return t;
}
struct Register {
    Register(const char *name, std::function<void()> fn) { Tests().emplace_back(name, fn); }
};

#define TEST(name) \
    void name();   \
    Register reg_##name(#name, name); \
    void name()
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++gFailures;                                                         \
        }                                                                        \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

bool Near(const Vec3 &a, const Vec3 &b, float eps = 1e-3f) { return Length(a - b) <= eps; }

GameFrame MakeGame(Vec3 player, float yaw = 0.0f) {
    GameFrame g;
    g.inGameplay = true;
    g.playerPos = player;
    g.playerYaw = yaw;
    // Game camera behind the player, looking at it along -Z.
    g.camera.eye = player + Vec3{0, 3, 6};
    g.camera.target = player;
    return g;
}

TrackingFrame MakeTracking(Vec3 head = {0, 1.6f, 0}, float dt = 1.0f / 90.0f) {
    TrackingFrame t;
    t.dt = dt;
    t.head.position = head;
    return t;
}

// ---------------------------------------------------------------- upscale

TEST(UpscaleScale2xFlatAndDiagonal) {
    // A flat image stays flat.
    const uint32_t flat[4] = {7, 7, 7, 7};
    uint32_t out[16];
    Scale2x(flat, 2, 2, out);
    for (uint32_t v : out) CHECK(v == 7);
    // A diagonal edge: the corner between two equal neighbours takes their colour.
    //   1 1 0       1 1 1 1 0 0
    //   1 0 0  ->   1 1 1 0 0 0  (row 1, pixel (1,1): its top-left corner becomes 1)
    //   0 0 0
    const uint32_t diag[9] = {1, 1, 0, 1, 0, 0, 0, 0, 0};
    uint32_t big[36];
    Scale2x(diag, 3, 3, big);
    CHECK(big[2 * 6 + 2] == 1);  // (1,1)'s top-left corner
    CHECK(big[2 * 6 + 3] == 0);  // its top-right corner keeps its own colour
    CHECK(big[0] == 1 && big[5] == 0);
    uint32_t scratch[36], big4[144];
    Scale4x(diag, 3, 3, scratch, big4);
    CHECK(big4[0] == 1 && big4[143] == 0);
}

// ---------------------------------------------------------------- math

TEST(TitleTextLargeSmoothAndTransparentAround) {
    int w = 0, h = 0;
    std::vector<uint32_t> px = PaintTitleText("KH", 8, 10, Rgba(255, 255, 255), Rgba(150, 190, 255, 150), &w, &h);
    CHECK(w == (2 * 10 + 2) * 8 + 20 && h == (20 + 2) * 8 + 20);
    CHECK(px.size() == static_cast<size_t>(w) * h);
    CHECK(px[0] == 0);  // nothing in the corners
    int solid = 0, partial = 0;
    for (uint32_t p : px) {
        const uint32_t a = p >> 24;
        solid += a == 255 && (p & 0xffffff) == 0xffffff;
        partial += a > 0 && a < 255;
    }
    CHECK(solid > 2000);    // the letters, white
    CHECK(partial > 500);   // their soft edges and the glow
}

TEST(MathYawRoundTrip) {
    for (float deg : {-170.0f, -90.0f, 0.0f, 45.0f, 135.0f}) {
        Quat q = Quat::Yaw(DegToRad(deg));
        CHECK_NEAR(RadToDeg(YawOf(q)), deg, 1e-3f);
    }
}

TEST(MathSimilarityInverse) {
    Similarity s{2.5f, Quat::Yaw(0.7f), {3, -1, 4}};
    Vec3 p{0.3f, 1.2f, -2.0f};
    CHECK(Near(s.Inverse().Transform(s.Transform(p)), p));
    Mat4 m = Mat4::FromSimilarity(s);
    CHECK(Near(m.TransformPoint(p), s.Transform(p)));
}

TEST(MathDsAngles) {
    CHECK_NEAR(DsAngleToRad(0x4000), kPi / 2, 1e-4f);
    CHECK(RadToDsAngle(kPi / 2) == 0x4000);
    CHECK(RadToDsAngle(-kPi / 2) == 0xc000);
    CHECK(FloatToFx32(1.5f) == 0x1800);
    CHECK_NEAR(Fx32ToFloat(0x800), 0.5f, 1e-6f);
}

// ---------------------------------------------------------------- settings

TEST(SettingsDefaultsAndClamp) {
    Settings s;
    CHECK(s.Enum<ViewMode>(SettingId::ViewMode) == ViewMode::ThirdPerson);
    CHECK_NEAR(s.Get(SettingId::ScreenDistance), 4.5f, 1e-6f);
    s.Set(SettingId::ScreenDistance, 100.0f);
    CHECK_NEAR(s.Get(SettingId::ScreenDistance), 12.0f, 1e-6f);
    s.Set(SettingId::ViewMode, 7.0f);
    CHECK(s.Int(SettingId::ViewMode) == 2);
}

TEST(SettingsIniRoundTrip) {
    Settings a;
    a.SetEnum(SettingId::ViewMode, ViewMode::FirstPerson);
    a.Set(SettingId::TpDistance, 2.25f);
    a.SetBool(SettingId::Passthrough, true);
    a.SetEnum(SettingId::ComfortTurn, TurnMode::Smooth);
    std::string ini = a.SaveIni();
    Settings b;
    std::vector<std::string> warnings;
    b.LoadIni(ini, &warnings);
    CHECK(warnings.empty());
    CHECK(b.Enum<ViewMode>(SettingId::ViewMode) == ViewMode::FirstPerson);
    CHECK_NEAR(b.Get(SettingId::TpDistance), 2.25f, 1e-6f);
    CHECK(b.Bool(SettingId::Passthrough));
    CHECK(b.Enum<TurnMode>(SettingId::ComfortTurn) == TurnMode::Smooth);
}

TEST(SettingsIniWarnings) {
    Settings s;
    std::vector<std::string> w;
    s.LoadIni("[View]\nmode = sideways\nbogus = 1\n[Screen]\ndistance = 99 ; too far\n"
              "passthrough = maybe\nnot a line\n",
              &w);
    CHECK(w.size() == 5);
    CHECK(s.Enum<ViewMode>(SettingId::ViewMode) == ViewMode::ThirdPerson);
    CHECK_NEAR(s.Get(SettingId::ScreenDistance), 12.0f, 1e-6f);
}

TEST(SettingsStepAndFormat) {
    Settings s;
    s.Step(SettingId::ScreenDistance, 1);
    CHECK_NEAR(s.Get(SettingId::ScreenDistance), 4.75f, 1e-5f);
    CHECK(s.FormatValue(SettingId::ScreenDistance) == "4.75 m");
    s.Step(SettingId::ViewMode, -1);  // wraps from the first to the last option
    CHECK(s.Enum<ViewMode>(SettingId::ViewMode) == ViewMode::GiantScreen);
    CHECK(s.FormatValue(SettingId::TpWorldScale) == "1.00x");
    s.Step(SettingId::Passthrough, 1);
    CHECK(s.FormatValue(SettingId::Passthrough) == "On");
    s.ResetTab("Screen");
    CHECK(!s.Bool(SettingId::Passthrough));
}

TEST(SettingsEveryTabHasOptions) {
    // The panel has two rows of six tabs, and room for eleven options on a tab.
    CHECK(SettingTabs().size() == 12);
    for (const char *tab : SettingTabs()) {
        int n = 0;
        for (const SettingDef &d : SettingDefs()) n += std::strcmp(d.tab, tab) == 0;
        CHECK(n >= 1 && n <= 11);
    }
    for (const SettingDef &d : SettingDefs()) {
        CHECK(d.help && *d.help);
        if (d.type == SettingType::Enum) CHECK(d.optionKeys.size() == d.optionLabels.size());
        if (d.type == SettingType::Float)
            CHECK(d.defaultValue >= d.minValue && d.defaultValue <= d.maxValue);
    }
}

// ---------------------------------------------------------------- third person rig

TEST(ThirdPersonPlacesCharacterInFront) {
    Settings s;
    s.Set(SettingId::GameUnitsPerMeter, 2.0f);
    ThirdPersonRig rig;
    GameFrame g = MakeGame({10, 0, -5});
    RigOutput o = rig.Update(s, g, MakeTracking());
    // The character appears `distance` m in front of the tracking origin, `height` m below.
    Vec3 inTracking = o.trackingToWorld.Inverse().Transform(g.playerPos);
    CHECK(Near(inTracking, {0, -s.Get(SettingId::TpHeight), -s.Get(SettingId::TpDistance)}));
    CHECK_NEAR(o.trackingToWorld.scale, 2.0f, 1e-6f);
    // Looking straight ahead looks the way the game camera did (-Z).
    CHECK_NEAR(rig.viewYaw(), 0.0f, 1e-4f);
    CHECK(o.requests.overrideCamera);
}

TEST(ThirdPersonDioramaScale) {
    Settings s;
    s.Set(SettingId::TpWorldScale, 0.1f);
    ThirdPersonRig rig;
    RigOutput o = rig.Update(s, MakeGame({0, 0, 0}), MakeTracking());
    CHECK_NEAR(o.trackingToWorld.scale, 10.0f, 1e-4f);
}

TEST(ThirdPersonDeadzoneAndFollow) {
    Settings s;
    ThirdPersonRig rig;
    GameFrame g = MakeGame({0, 0, 0});
    rig.Update(s, g, MakeTracking());
    g.playerPos = {0.3f, 0, 0};  // inside the 0.5 m dead zone
    rig.Update(s, g, MakeTracking());
    CHECK(Near(rig.anchor(), {0, 0, 0}));
    g.playerPos = {3.0f, 0, 0};
    for (int i = 0; i < 900; ++i) rig.Update(s, g, MakeTracking());
    // Settles at the edge of the dead zone.
    CHECK_NEAR(Length(g.playerPos - rig.anchor()), 0.5f, 0.01f);
}

TEST(ThirdPersonTeleportReinitializes) {
    Settings s;
    ThirdPersonRig rig;
    GameFrame g = MakeGame({0, 0, 0});
    rig.Update(s, g, MakeTracking());
    g.playerPos = {50, 0, 0};
    RigOutput o = rig.Update(s, g, MakeTracking());
    CHECK(o.blinkStarted);
    CHECK(Near(rig.anchor(), g.playerPos));
}

TEST(ThirdPersonSnapTurnAroundCharacter) {
    Settings s;
    ThirdPersonRig rig;
    GameFrame g = MakeGame({0, 0, 0});
    rig.Update(s, g, MakeTracking());
    TrackingFrame t = MakeTracking();
    t.turnAxis = 1.0f;  // right
    RigOutput o = rig.Update(s, g, t);
    CHECK_NEAR(RadToDeg(rig.viewYaw()), -45.0f, 1e-3f);
    // Holding the stick does not repeat the snap.
    o = rig.Update(s, g, t);
    CHECK_NEAR(RadToDeg(rig.viewYaw()), -45.0f, 1e-3f);
    // The character is still in front of you.
    Vec3 inTracking = o.trackingToWorld.Inverse().Transform(g.playerPos);
    CHECK_NEAR(inTracking.z, -s.Get(SettingId::TpDistance), 1e-3f);
}

TEST(ThirdPersonMoveBasisFollowsHead) {
    Settings s;
    ThirdPersonRig rig;
    TrackingFrame t = MakeTracking();
    t.head.orientation = Quat::Yaw(DegToRad(90));
    rig.Update(s, MakeGame({0, 0, 0}), MakeTracking());
    RigOutput o = rig.Update(s, MakeGame({0, 0, 0}), t);
    CHECK_NEAR(RadToDeg(o.moveBasisYaw), 90.0f, 1e-3f);
}

// ---------------------------------------------------------------- first person rig

TEST(FirstPersonHeadOnCharacter) {
    Settings s;
    s.Set(SettingId::FpWorldScale, 1.0f);  // the rig's own geometry, at the scale of the game
    s.Set(SettingId::FpEyeHeight, 1.5f);
    s.SetBool(SettingId::FpMatchCharacterHeight, false);
    FirstPersonRig rig;
    GameFrame g = MakeGame({5, 2, 5}, DegToRad(30));
    TrackingFrame t = MakeTracking({0.4f, 1.7f, -0.2f});
    RigOutput o = rig.Update(s, g, t);
    Vec3 headWorld = o.trackingToWorld.Transform(t.head.position);
    CHECK(Near(headWorld, {5, 2 + 1.7f, 5}));
    // Looking ahead looks where the character faces.
    CHECK_NEAR(RadToDeg(YawOf(o.trackingToWorld.TransformDir(t.head.orientation.Forward()))), 30.0f, 1e-3f);
    CHECK(o.requests.setPlayerYaw);
}

TEST(FirstPersonMatchesCharacterHeight) {
    Settings s;  // character eye height 1.5 m
    s.Set(SettingId::FpWorldScale, 1.0f);  // the rig's own geometry, at the scale of the game
    s.Set(SettingId::FpEyeHeight, 1.5f);
    FirstPersonRig rig;
    GameFrame g = MakeGame({0, 0, 0});
    TrackingFrame t = MakeTracking({0, 1.8f, 0});
    RigOutput o = rig.Update(s, g, t);
    CHECK_NEAR(o.trackingToWorld.Transform(t.head.position).y, 1.5f, 1e-3f);
    // Crouching afterwards does not rescale the world.
    t.head.position.y = 1.0f;
    o = rig.Update(s, g, t);
    CHECK_NEAR(o.trackingToWorld.scale, 1.5f / 1.8f, 1e-4f);
}

TEST(FirstPersonRoomScaleStep) {
    Settings s;
    s.Set(SettingId::FpWorldScale, 1.0f);  // the rig's own geometry, at the scale of the game
    s.Set(SettingId::FpEyeHeight, 1.5f);
    s.SetBool(SettingId::FpMatchCharacterHeight, false);
    FirstPersonRig rig;
    GameFrame g = MakeGame({0, 0, 0});
    rig.Update(s, g, MakeTracking({0, 1.6f, 0}));
    RigOutput o = rig.Update(s, g, MakeTracking({0.1f, 1.6f, 0}));
    CHECK(o.requests.hasRoomScaleStep);
    CHECK(Near(o.requests.roomScaleStep, {0.1f, 0, 0}));
    // Until the game applies it, the view shows the step as already applied.
    CHECK(Near(o.trackingToWorld.Transform({0.1f, 1.6f, 0}), {0.1f, 1.6f, 0}));
    // The game frame advances but a wall blocked the move: you are pushed back, over the time of
    // a game frame (smooth motion), from where the view showed you.
    g.frameCounter++;
    o = rig.Update(s, g, MakeTracking({0.1f, 1.6f, 0}));
    CHECK(Near(o.trackingToWorld.Transform({0.1f, 1.6f, 0}), {0.1f, 1.6f, 0}));
    for (int i = 0; i < 6; ++i) o = rig.Update(s, g, MakeTracking({0.1f, 1.6f, 0}));
    CHECK(Near(o.trackingToWorld.Transform({0.1f, 1.6f, 0}), {0, 1.6f, 0}));
    // Without smooth motion, at once.
    s.SetBool(SettingId::ViewSmoothMotion, false);
    rig.Update(s, g, MakeTracking({0.2f, 1.6f, 0}));
    g.frameCounter++;
    o = rig.Update(s, g, MakeTracking({0.2f, 1.6f, 0}));
    CHECK(Near(o.trackingToWorld.Transform({0.2f, 1.6f, 0}), {0, 1.6f, 0}));
}

TEST(SmootherPlaysStepsOutBetweenGameFrames) {
    MotionSmoother m;
    m.Reset({0, 0, 0});
    uint32_t frame = 0;
    // The game at 30 frames a second, the headset at 120: a 1-unit step every 4 headset frames.
    float last = 0, biggest = 0;
    for (int i = 1; i <= 40; ++i) {
        if (i % 4 == 0) ++frame;
        Vec3 p = m.Update({static_cast<float>(frame), 0, 0}, frame, 1.0f / 120);
        if (i > 12) biggest = std::fmax(biggest, p.x - last);
        last = p.x;
    }
    CHECK(biggest < 0.4f);   // the steps shared out over the headset frames, not 1 at once
    CHECK(last > 8.0f);      // a game frame behind at most
}

// ---------------------------------------------------------------- combat

CombatInput Swing(float tipSpeed, float dt, int frame, bool moving) {
    CombatInput in;
    in.dt = dt;
    in.head.position = {0, 1.6f, 0};
    in.handValid[1] = true;
    in.hands[1].position = {0.2f, 1.2f, -0.3f};
    // Rotate the controller about +Y so the tip sweeps horizontally.
    float yaw = moving ? -frame * tipSpeed * dt / CombatRecognizer::kBladeLength : 0.0f;
    in.hands[1].orientation = Quat::Yaw(yaw);
    return in;
}

TEST(CombatSwingDetected) {
    Settings s;
    CombatRecognizer r;
    int swings = 0;
    SwingKind kind = SwingKind::Thrust;
    float dt = 1.0f / 90.0f;
    for (int i = 0; i < 10; ++i) r.Update(s, Swing(0, dt, i, false));
    for (int i = 0; i < 20; ++i) {
        CombatFrame f = r.Update(s, Swing(4.0f, dt, i, true));
        if (f.swingStarted) {
            ++swings;
            kind = f.swing.kind;
        }
    }
    CHECK(swings == 1);  // one continuous swing is one attack
    CHECK(kind == SwingKind::Horizontal);
}

TEST(CombatSlowMotionIsNotASwing) {
    Settings s;
    CombatRecognizer r;
    float dt = 1.0f / 90.0f;
    int swings = 0;
    for (int i = 0; i < 60; ++i) swings += r.Update(s, Swing(1.0f, dt, i, true)).swingStarted;
    CHECK(swings == 0);
}

TEST(CombatGuardPose) {
    Settings s;
    CombatRecognizer r;
    CombatInput in;
    in.dt = 1.0f / 90.0f;
    in.head.position = {0, 1.6f, 0};
    in.handValid[1] = true;
    in.hands[1].position = {0.15f, 1.45f, -0.35f};
    // Blade pointing left, level, in front of the face.
    in.hands[1].orientation = Quat::Yaw(DegToRad(90));
    bool guarding = false, started = false;
    for (int i = 0; i < 30; ++i) {
        CombatFrame f = r.Update(s, in);
        guarding = f.guarding;
        started |= f.guardStarted;
    }
    CHECK(guarding);
    CHECK(started);
}

TEST(CombatStickFlickDodges) {
    Settings s;
    CombatRecognizer r;
    CombatInput in;
    in.dt = 1.0f / 90.0f;
    for (int i = 0; i < 20; ++i) r.Update(s, in);
    in.moveAxis = {1, 0, 0};
    CHECK(!r.Update(s, in).dodge);  // not yet: it could be the start of walking
    r.Update(s, in);
    in.moveAxis = {};
    CombatFrame flick = r.Update(s, in);
    CHECK(flick.dodge);
    CHECK(flick.dodgeDirection.x > 0.9f);
    // Pushing the stick to the edge fast and holding it is walking, not a dodge.
    CombatRecognizer hold;
    for (int i = 0; i < 20; ++i) hold.Update(s, in);
    bool held = false;
    in.moveAxis = {0, 0, -1};
    for (int i = 0; i < 90; ++i) held |= hold.Update(s, in).dodge;
    in.moveAxis = {};
    for (int i = 0; i < 5; ++i) held |= hold.Update(s, in).dodge;
    CHECK(!held);
    // Walking up to full speed slowly is not a dodge.
    CombatRecognizer walk;
    in.moveAxis = {};
    for (int i = 0; i < 20; ++i) walk.Update(s, in);
    bool dodged = false;
    for (int i = 1; i <= 30; ++i) {
        in.moveAxis = {i / 30.0f, 0, 0};
        dodged |= walk.Update(s, in).dodge;
    }
    CHECK(!dodged);
}

// ---------------------------------------------------------------- input mapper

TEST(MapperDpadOctants) {
    CHECK(InputMapper::DpadFromDirection(0, 1, 0) == kDsUp);
    CHECK(InputMapper::DpadFromDirection(1, 0, 0) == kDsRight);
    CHECK(InputMapper::DpadFromDirection(-0.7f, -0.7f, 0) == (kDsDown | kDsLeft));
    CHECK(InputMapper::DpadFromDirection(0.05f, 0.05f, 0) == 0);
    // Moving "forward" relative to a head turned 90 degrees left of the game camera is Left.
    CHECK(InputMapper::DpadFromDirection(0, 1, DegToRad(90)) == kDsLeft);
}

TEST(MapperMenuStickOneAxisWithRelease) {
    Settings s;
    InputMapper m;
    m.SetMenuStick(true);
    ControllerState c;
    auto dpad = [&](float x, float y) {
        c.stickX[0] = x;
        c.stickY[0] = y;
        m.Update(s, ViewMode::GiantScreen, c, nullptr, 0, 0, 0.011f);
        return static_cast<uint16_t>(m.heldButtons() & (kDsUp | kDsDown | kDsLeft | kDsRight));
    };
    CHECK(dpad(0.0f, 0.45f) == 0);              // a light push does not press yet
    CHECK(dpad(0.0f, 0.7f) == kDsUp);
    CHECK(dpad(0.5f, 0.6f) == kDsUp);           // drifting toward the diagonal keeps Up alone
    CHECK(dpad(0.0f, 0.4f) == kDsUp);           // easing off holds until the release point
    CHECK(dpad(0.0f, 0.2f) == 0);
    CHECK(dpad(0.75f, 0.7f) == kDsRight);       // a diagonal press is one axis
    CHECK(dpad(0.3f, 0.75f) == kDsUp);          // a clear move to the other axis switches
    CHECK(dpad(0.0f, -0.8f) == kDsDown);        // and so does reversing
}

TEST(MapperFieldOctantHysteresis) {
    Settings s;
    InputMapper m;
    ControllerState c;
    auto dpad = [&](float degrees) {
        c.stickX[0] = -std::sin(DegToRad(degrees));
        c.stickY[0] = std::cos(DegToRad(degrees));
        m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.011f);
        return static_cast<uint16_t>(m.heldButtons() & (kDsUp | kDsDown | kDsLeft | kDsRight));
    };
    CHECK(dpad(0) == kDsUp);
    CHECK(dpad(25) == kDsUp);                   // just past the edge: still Up
    CHECK(dpad(35) == (kDsUp | kDsLeft));
    CHECK(dpad(20) == (kDsUp | kDsLeft));       // and back just past it: still up-left
    CHECK(dpad(10) == kDsUp);
}

TEST(MapperSwingPulse) {
    Settings s;
    InputMapper m;
    ControllerState c;
    CombatFrame f;
    f.swingStarted = true;
    m.Update(s, ViewMode::FirstPerson, c, &f, 0, 0, 0.011f);
    f.swingStarted = false;
    m.Update(s, ViewMode::FirstPerson, c, &f, 0, 0, 0.011f);
    CHECK(m.SampleForGameFrame() & kDsA);
    CHECK(m.SampleForGameFrame() & kDsA);
    CHECK(!(m.SampleForGameFrame() & kDsA));  // released for one frame
    CHECK(!(m.SampleForGameFrame() & kDsA));
}

TEST(MapperMenuTapIsStartHoldIsPanel) {
    Settings s;
    InputMapper m;
    ControllerState c;
    c.menu = true;
    m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
    c.menu = false;
    m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
    CHECK(m.SampleForGameFrame() & kDsStart);

    InputMapper hold;
    c.menu = true;
    bool opened = false;
    for (int i = 0; i < 10; ++i) {
        hold.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
        opened |= hold.actions().openSettingsPanel;
    }
    c.menu = false;
    hold.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
    CHECK(opened);
    CHECK(!(hold.SampleForGameFrame() & kDsStart));
}

TEST(MapperRightStickTapRecentersHoldToggles) {
    Settings s;
    InputMapper m;
    ControllerState c;
    c.stickClick[1] = true;
    m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
    c.stickClick[1] = false;
    m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
    CHECK(m.actions().recenter);
    c.stickClick[1] = true;
    bool toggled = false;
    for (int i = 0; i < 10; ++i) {
        m.Update(s, ViewMode::ThirdPerson, c, nullptr, 0, 0, 0.1f);
        toggled |= m.actions().toggleViewMode;
    }
    CHECK(toggled);
}

TEST(MapperAnalogMoveWorld) {
    Settings s;
    InputMapper m;
    ControllerState c;
    c.stickY[0] = 1.0f;
    m.Update(s, ViewMode::FirstPerson, c, nullptr, DegToRad(90), 0, 0.011f);
    // Forward along a basis turned 90 degrees left is -X.
    CHECK(Near(m.analogMoveWorld(), {-1, 0, 0}));
    CHECK(m.heldButtons() & kDsLeft);
}

// ---------------------------------------------------------------- settings panel

TEST(PanelClicksChangeSettings) {
    Settings s;
    SettingsPanel p;
    p.Open();
    CHECK(p.Redraw(s));
    CHECK(!p.Redraw(s));  // nothing changed
    using B = SettingsPanel::Button;
    float x = 0, y = 0;
    // Quick setup lists the view mode among the most used options; its ">" steps it.
    CHECK(p.tab() == 0);
    CHECK(p.Find(B::Increase, static_cast<int>(SettingId::ViewMode), &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(s.Enum<ViewMode>(SettingId::ViewMode) == ViewMode::FirstPerson);
    CHECK(p.Redraw(s));
    // The Screens page, then its Defaults button undoes a change.
    s.Set(SettingId::ScreenDistance, 8.0f);
    CHECK(p.Find(B::Tab, 1, &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(p.tab() == 1);
    CHECK(p.Find(B::Defaults, 0, &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK_NEAR(s.Get(SettingId::ScreenDistance), 4.5f, 1e-6f);
    // The button guide toggles; a page leaves it.
    CHECK(p.Find(B::Guide, 0, &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(p.guideOpen());
    CHECK(p.Redraw(s));
    CHECK(p.Find(B::Tab, 1, &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(!p.guideOpen());
    CHECK(p.Find(B::Close, 0, &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(!p.isOpen());
}

TEST(PanelProfilesSetEverythingAtOnce) {
    Settings s;
    SettingsPanel p;
    p.Open();
    using B = SettingsPanel::Button;
    float x = 0, y = 0;
    CHECK(CurrentProfile(s) == SettingsProfile::GiantScreen);  // the defaults
    // "DS in your hands": passthrough, the handheld alone, held in a hand.
    CHECK(p.Find(B::Profile, static_cast<int>(SettingsProfile::DsInHands), &x, &y));
    p.Pointer(true, x, y, true, s);
    CHECK(s.Bool(SettingId::Passthrough));
    CHECK(s.Enum<HandheldDs>(SettingId::HandScreenDs) == HandheldDs::Only);
    CHECK(s.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement) == BottomScreenPlacement::InHand);
    CHECK(CurrentProfile(s) == SettingsProfile::DsInHands);
    // "Both screens": no handheld, nothing in the hand, both screens on the giant screen.
    ApplyProfile(SettingsProfile::BothScreens, s);
    CHECK(!s.Bool(SettingId::Passthrough));
    CHECK(s.Bool(SettingId::ScreenBoth));
    CHECK(s.Enum<HandheldDs>(SettingId::HandScreenDs) == HandheldDs::Off);
    CHECK(s.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement) == BottomScreenPlacement::Hidden);
    CHECK(CurrentProfile(s) == SettingsProfile::BothScreens);
    // Changing one of them by hand leaves the profile.
    s.SetBool(SettingId::Passthrough, true);
    CHECK(CurrentProfile(s) == SettingsProfile::Count);
    ApplyProfile(SettingsProfile::GiantScreen, s);
    CHECK(CurrentProfile(s) == SettingsProfile::GiantScreen);
}

TEST(PanelPagesScroll) {
    Settings s;
    SettingsPanel p;
    p.Open();
    p.SetTab(3);  // View: four sections, more lines than fit
    using B = SettingsPanel::Button;
    float x = 0, y = 0;
    CHECK(p.Find(B::ScrollDown, 0, &x, &y));
    CHECK(!p.Find(B::Increase, static_cast<int>(SettingId::ComfortSeatedHeight), &x, &y));  // far down
    for (int i = 0; i < 40; ++i) p.Scroll(1);
    CHECK(p.scroll() > 0);
    CHECK(p.Find(B::Increase, static_cast<int>(SettingId::ComfortSeatedHeight), &x, &y));
    float before = s.Get(SettingId::ComfortSeatedHeight);
    p.Pointer(true, x, y, true, s);
    CHECK(s.Get(SettingId::ComfortSeatedHeight) != before);
    p.Scroll(-1000);
    CHECK(p.scroll() == 0);
    // Every option is on some page.
    for (const SettingDef &d : SettingDefs()) {
        bool found = false;
        for (int page = 1; page < SettingsPanel::PageCount() && !found; ++page) {
            p.SetTab(page);
            for (int i = 0; i < 60 && !found; ++i) {
                found = p.Find(B::Row, static_cast<int>(d.id), &x, &y);
                p.Scroll(1);
            }
        }
        CHECK(found);
    }
}

// ---------------------------------------------------------------- demo game

TEST(DemoMovesWithDpadRelativeToCamera) {
    DemoGame g;
    GameRequests r;
    Vec3 start = g.frame().playerPos;
    for (int i = 0; i < 10; ++i) g.Step(r, kDsUp);
    // The demo camera looks along -Z at the start, so Up walks toward -Z.
    CHECK(g.frame().playerPos.z < start.z - 1.0f);
}

TEST(DemoPillarBlocksRoomScaleStep) {
    DemoGame g;
    GameRequests r;
    r.hasRoomScaleStep = true;
    r.roomScaleStep = {0, 0, -0.5f};
    // Walk physically into the pillar at (6, 0, 6) from its +Z side.
    r.analogMoveWorld = {};
    for (int i = 0; i < 40; ++i) {
        GameRequests toPillar;
        toPillar.hasAnalogMove = true;
        Vec3 d = Vec3{6, 0, 6} - g.frame().playerPos;
        d.y = 0;
        toPillar.analogMoveWorld = Normalize(d);
        g.Step(toPillar, 0);
    }
    Vec3 d = g.frame().playerPos - Vec3{6, 0, 6};
    d.y = 0;
    CHECK(Length(d) >= 1.2f + 0.35f - 1e-3f);
}

TEST(DemoAttackHitsDummyInFront) {
    DemoGame g;
    GameRequests r;
    r.hasAnalogMove = true;
    r.analogMoveWorld = {0, 0, -1};
    for (int i = 0; i < 60 && g.hits() == 0; ++i) {
        g.Step(r, (i % 4 == 0) ? kDsA : 0);
    }
    CHECK(g.hits() > 0);
}

}  // namespace

int main() {
    for (auto &t : Tests()) {
        int before = gFailures;
        t.second();
        std::printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.first);
    }
    std::printf("%zu tests, %d failed checks\n", Tests().size(), gFailures);
    return gFailures == 0 ? 0 : 1;
}
