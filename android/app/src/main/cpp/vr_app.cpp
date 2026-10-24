#include "vr_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "khvr/canvas.h"
#include "log.h"

namespace khvr {

namespace {

constexpr float kStickDeadzone = 0.2f;
constexpr float kNear = 0.05f, kFar = 500.0f;
constexpr float kIpd = 0.064f;
constexpr int kGiantViewW = 1024, kGiantViewH = 768;  // 4:3 like the DS top screen

float Deadzone(float v) { return std::fabs(v) < kStickDeadzone ? 0.0f : v; }

Mat4 EyeProjection(const XrFovf &f) {
    return Mat4::Projection(std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp),
                            std::tan(f.angleDown), kNear, kFar);
}

Mat4 ViewFromPose(const Pose &p) { return Mat4::FromPose(p.Inverse()); }

// A view matrix for a game-world camera, built the way MTX_LookAt does.
Mat4 LookAt(const Vec3 &eye, const Vec3 &target, const Vec3 &upHint) {
    Vec3 f = Normalize(target - eye);
    Vec3 r = Normalize(Cross(f, upHint));
    if (Length(r) < 1e-4f) r = {1, 0, 0};
    Vec3 u = Cross(r, f);
    Mat4 m = Mat4::Identity();
    m.at(0, 0) = r.x; m.at(0, 1) = r.y; m.at(0, 2) = r.z; m.at(0, 3) = -Dot(r, eye);
    m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z; m.at(1, 3) = -Dot(u, eye);
    m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z; m.at(2, 3) = Dot(f, eye);
    return m;
}

// The quad a placement draws faces +Z of its pose; this turns "facing the viewer at `from`".
Pose FacingFrom(const Vec3 &at, const Vec3 &from) {
    Vec3 d = from - at;
    return {Quat::Yaw(std::atan2(d.x, d.z)), at};
}

uint16_t SampleThunk(void *ctx) { return static_cast<InputMapper *>(ctx)->SampleForGameFrame(); }

}  // namespace

void VrApp::LoadSettings(const std::string &filesDir) {
    filesDir_ = filesDir;
    settingsPath_ = filesDir + "/khvr.ini";
    std::ifstream in(settingsPath_);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::vector<std::string> warnings;
        settings_.LoadIni(ss.str(), &warnings);
        for (const std::string &w : warnings) LOGW("khvr.ini %s", w.c_str());
        LOGI("loaded %s", settingsPath_.c_str());
    }
    savedRevision_ = settings_.revision();
    if (!in) SaveSettingsIfChanged(true);
}

void VrApp::SaveSettingsIfChanged(bool force) {
    if (settingsPath_.empty() || (!force && settings_.revision() == savedRevision_)) return;
    std::ofstream out(settingsPath_);
    out << settings_.SaveIni();
    savedRevision_ = settings_.revision();
}

bool VrApp::Init(XrPlatform *xr) {
    xr_ = xr;
    if (!renderer_.Init()) return false;
    StartGame();
    lastMode_ = settings_.Enum<ViewMode>(SettingId::ViewMode);
    blinkTime_ = -1.0f;
    ApplyPlatformSettings();
    return true;
}

void VrApp::SetSharedDir(const std::string &dir, bool hasAccess) {
    sharedDir_ = hasAccess ? dir : std::string();
    // Created up front so it shows up in SideQuest's file browser.
    if (hasAccess) mkdir(dir.c_str(), 0777);
}

void VrApp::StartGame() {
#ifdef KHVR_HAS_GAME
    if (settings_.Enum<GamePlay>(SettingId::GamePlay) == GamePlay::Auto) {
        auto game = std::make_unique<DecompGame>();
        std::string message;
        // The firmware's language numbers start at 1 (English) in the settings' order.
        int language = settings_.Int(SettingId::GameLanguage) + 1;
        // Recent Quest system versions refuse adb writes into Android/data, so the ROM normally
        // sits in the shared KHDaysVR (or Download) folder; files/game still works where adb can
        // write there.
        std::vector<std::string> romDirs;
        if (!sharedDir_.empty()) {
            romDirs.push_back(sharedDir_);
            romDirs.push_back("/storage/emulated/0/Download");
        }
        romDirs.push_back(filesDir_ + "/game");
        const std::string &dataDir = sharedDir_.empty() ? filesDir_ : sharedDir_;
        if (game->Start(romDirs, dataDir, language, &message)) {
            decomp_ = game.get();
            game_ = std::move(game);
            LOGI("game: %s", message.c_str());
            return;
        }
        if (sharedDir_.empty()) {
            needsStorageAccess_ = true;
            message = "KH Days VR needs \"All files access\" to read your ROM from the KHDaysVR "
                      "folder. Turn it on in the settings page that opened (or see the README).";
        } else {
            message += " Copy your own dump of the EU cartridge into the KHDaysVR folder on the "
                       "headset (any name ending in .nds).";
        }
        notice_ = message;
        LOGW("game: %s - running the demo arena", message.c_str());
    }
#else
    if (settings_.Enum<GamePlay>(SettingId::GamePlay) == GamePlay::Auto)
        notice_ = "This build has no game (built without the decomp guest). Running the demo arena.";
#endif
    auto demo = std::make_unique<DemoGame>();
    demo_ = demo.get();
    game_ = std::move(demo);
}

std::vector<std::string> VrApp::StatusLines() const {
    std::vector<std::string> lines;
    if (!notice_.empty()) {
        lines.push_back("DEMO ARENA - the game is not running");
        // Wrap the notice at the panel's width (74 characters at scale 1, inside the margins).
        std::string rest = notice_;
        while (!rest.empty()) {
            size_t cut = rest.size() <= 74 ? rest.size() : rest.rfind(' ', 74);
            if (cut == std::string::npos || cut == 0) cut = std::min<size_t>(74, rest.size());
            lines.push_back(rest.substr(0, cut));
            rest = rest.substr(cut < rest.size() && rest[cut] == ' ' ? cut + 1 : cut);
        }
        lines.push_back("Then restart the app. (Game > Play switches between game and demo.)");
        return lines;
    }
#ifdef KHVR_HAS_GAME
    if (decomp_ && (decomp_->state() != PlatformState::Running || decomp_->frame().frameCounter < 2)) {
        for (const std::string &l : decomp_->Status(10)) lines.push_back(l.substr(0, 74));
        lines.push_back(sharedDir_.empty() ? "Full log: Android/data/com.khdays.vr/files/khvr_boot.log"
                                           : "Full log: KHDaysVR/khvr_boot.log");
    }
#endif
    return lines;
}

bool VrApp::Loading() const {
#ifdef KHVR_HAS_GAME
    return decomp_ && decomp_->state() == PlatformState::Running && !decomp_->hasPicture();
#else
    return false;
#endif
}

namespace {

uint32_t Shade(uint32_t c, float k) {
    auto ch = [&](int shift) {
        float v = static_cast<float>((c >> shift) & 0xff) * k;
        return static_cast<uint32_t>(v > 255.0f ? 255.0f : v) << shift;
    };
    return ch(0) | ch(8) | ch(16) | (c & 0xff000000u);
}

// The heart curve, |t| in [0, 2 pi): a unit-ish heart, point toward -y.
Vec3 HeartPoint(float t) {
    float s = std::sin(t);
    float x = 16.0f * s * s * s;
    float y = 13.0f * std::cos(t) - 5.0f * std::cos(2 * t) - 2.0f * std::cos(3 * t) - std::cos(4 * t);
    return {x / 17.0f, 0.0f, -y / 17.0f};
}

}  // namespace

void VrApp::BuildLoadingArea(float floorY) {
    // An original take on the series' Station of Awakening: a stained-glass pillar top floating in
    // darkness, lit from above. Leaded rings and spokes, a ring of medallion windows, a heart in
    // the middle where you stand, and a column of light around you.
    loadingArea_.triangles.clear();
    loadingBeam_.triangles.clear();
    const Vec3 up{0, 1, 0};
    const Vec3 c{screenAnchor_.x, floorY, screenAnchor_.z};
    const float kRadius = 3.2f;
    const uint32_t kLead = Rgba(18, 16, 28);
    const uint32_t kGold = Rgba(226, 186, 84);
    auto tri = [&](WorldDrawList &l, const Vec3 &a, const Vec3 &b, const Vec3 &d, uint32_t ca,
                   uint32_t cb, uint32_t cd) {
        l.triangles.push_back({a, up, ca});
        l.triangles.push_back({b, up, cb});
        l.triangles.push_back({d, up, cd});
    };
    // Light from above: glass toward the middle glows brighter.
    auto lit = [&](uint32_t col, float r) { return Shade(col, 1.25f - 0.45f * (r / kRadius)); };
    auto ringPoint = [&](float r, float a, float y = 0.0f) {
        return c + Vec3{r * std::cos(a), y, r * std::sin(a)};
    };
    // A flat ring segment between radii r0..r1 and angles a0..a1, slightly above y.
    auto band = [&](float r0, float r1, float a0, float a1, uint32_t col, float y) {
        const int steps = 4;
        for (int i = 0; i < steps; ++i) {
            float b0 = a0 + (a1 - a0) * i / steps, b1 = a0 + (a1 - a0) * (i + 1) / steps;
            Vec3 p00 = ringPoint(r0, b0, y), p01 = ringPoint(r0, b1, y);
            Vec3 p10 = ringPoint(r1, b0, y), p11 = ringPoint(r1, b1, y);
            uint32_t i0 = lit(col, r0), o0 = lit(col, r1);
            tri(loadingArea_, p00, p01, p11, i0, i0, o0);
            tri(loadingArea_, p00, p11, p10, i0, o0, o0);
        }
    };
    auto disc = [&](const Vec3 &at, float r, uint32_t col, float y) {
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
            Vec3 o = at + Vec3{0, y, 0};
            uint32_t k = lit(col, Length(at - c));
            tri(loadingArea_, o, o + Vec3{r * std::cos(a1), 0, r * std::sin(a1)},
                o + Vec3{r * std::cos(a0), 0, r * std::sin(a0)}, k, k, k);
        }
    };

    // Background glass: lead first, then 36 panes in four blues and violets with lead between.
    disc(c, kRadius, kLead, 0.0f);
    const uint32_t kField[] = {Rgba(44, 78, 168), Rgba(78, 58, 150), Rgba(36, 112, 170), Rgba(60, 90, 190),
                               Rgba(96, 70, 168), Rgba(40, 96, 150)};
    const int kSpokes = 36;
    const float kGap = 0.012f;
    const float rings[] = {1.05f, 1.75f, 2.5f, 2.95f};
    for (int ring = 0; ring + 1 < 4; ++ring) {
        for (int i = 0; i < kSpokes; ++i) {
            float a0 = 2 * kPi * i / kSpokes + kGap, a1 = 2 * kPi * (i + 1) / kSpokes - kGap;
            uint32_t col = kField[(i * 7 + ring * 3) % 6];
            band(rings[ring] + 0.02f, rings[ring + 1] - 0.02f, a0, a1, col, 0.002f);
        }
    }
    // Gold border with dark notches, the outer lip of the pillar.
    for (int i = 0; i < 72; ++i) {
        float a0 = 2 * kPi * i / 72 + 0.004f, a1 = 2 * kPi * (i + 1) / 72 - 0.004f;
        band(2.97f, kRadius - 0.03f, a0, a1, (i % 2) ? kGold : Shade(kGold, 0.75f), 0.002f);
    }
    // Inner field around the heart: pale gold glass.
    disc(c, 1.03f, Rgba(250, 226, 150), 0.002f);
    // Eight medallion windows on the middle ring, each a gold frame around lighter glass.
    for (int m = 0; m < 8; ++m) {
        float a = 2 * kPi * (m + 0.5f) / 8;
        Vec3 at = ringPoint((rings[1] + rings[2]) * 0.5f, a);
        disc(at, 0.36f, kGold, 0.004f);
        disc(at, 0.31f, kLead, 0.005f);
        const uint32_t kMedal[] = {Rgba(150, 196, 240), Rgba(200, 170, 236), Rgba(160, 220, 210), Rgba(236, 200, 160)};
        disc(at, 0.29f, kMedal[m % 4], 0.006f);
    }
    // The heart, point toward where you face first, in a lead outline.
    float yaw = screenYaw_;
    Quat q = Quat::Yaw(yaw);
    auto heart = [&](float scale, uint32_t col, float y) {
        const int n = 64;
        Vec3 mid = c + q.Rotate(Vec3{0, 0, -0.05f} * scale) + Vec3{0, y, 0};
        for (int i = 0; i < n; ++i) {
            Vec3 p0 = c + q.Rotate(HeartPoint(2 * kPi * i / n) * scale) + Vec3{0, y, 0};
            Vec3 p1 = c + q.Rotate(HeartPoint(2 * kPi * (i + 1) / n) * scale) + Vec3{0, y, 0};
            uint32_t k = lit(col, 0.0f);
            // Wind so the face points up whichever way the curve runs.
            if (Cross(p1 - mid, p0 - mid).y > 0) tri(loadingArea_, mid, p1, p0, k, k, k);
            else tri(loadingArea_, mid, p0, p1, k, k, k);
        }
    };
    heart(0.82f, kLead, 0.008f);
    heart(0.76f, Rgba(196, 40, 70), 0.009f);
    heart(0.52f, Rgba(236, 84, 110), 0.010f);

    // The pillar below, fading into the dark.
    const int kSides = 48;
    for (int i = 0; i < kSides; ++i) {
        float a0 = 2 * kPi * i / kSides, a1 = 2 * kPi * (i + 1) / kSides;
        Vec3 t0 = ringPoint(kRadius, a0), t1 = ringPoint(kRadius, a1);
        Vec3 b0 = ringPoint(kRadius * 0.92f, a0, -24.0f), b1 = ringPoint(kRadius * 0.92f, a1, -24.0f);
        Vec3 n = Normalize(Vec3{std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2)});
        uint32_t top = Rgba(70, 64, 96), bottom = Rgba(6, 6, 12);
        loadingArea_.triangles.push_back({t0, n, top});
        loadingArea_.triangles.push_back({t1, n, top});
        loadingArea_.triangles.push_back({b1, n, bottom});
        loadingArea_.triangles.push_back({t0, n, top});
        loadingArea_.triangles.push_back({b1, n, bottom});
        loadingArea_.triangles.push_back({b0, n, bottom});
    }
    // The column of light: a faint cylinder around you, wound to be seen from inside.
    for (int i = 0; i < kSides; ++i) {
        float a0 = 2 * kPi * i / kSides, a1 = 2 * kPi * (i + 1) / kSides;
        Vec3 l0 = ringPoint(2.2f, a0), l1 = ringPoint(2.2f, a1);
        Vec3 h0 = ringPoint(1.4f, a0, 9.0f), h1 = ringPoint(1.4f, a1, 9.0f);
        Vec3 n = up;
        uint32_t lo = Rgba(255, 244, 210), hi = Rgba(120, 130, 200);
        loadingBeam_.triangles.push_back({l0, n, lo});
        loadingBeam_.triangles.push_back({l1, n, lo});
        loadingBeam_.triangles.push_back({h1, n, hi});
        loadingBeam_.triangles.push_back({l0, n, lo});
        loadingBeam_.triangles.push_back({h1, n, hi});
        loadingBeam_.triangles.push_back({h0, n, hi});
    }
    loadingCenter_ = c;
}

void VrApp::UpdateLoadingSparkles(double now) {
    // Motes of light drifting up through the column, so the area is visibly alive.
    loadingSparkles_.triangles.clear();
    for (int i = 0; i < 40; ++i) {
        float seed = static_cast<float>(i) * 12.9898f;
        float a = std::fmod(seed * 3.7f, 2 * kPi) + static_cast<float>(now) * 0.05f;
        float r = 0.4f + std::fmod(seed * 0.618f, 2.4f);
        float y = std::fmod(static_cast<float>(now) * (0.12f + 0.05f * (i % 5)) + seed * 0.37f, 5.0f);
        Vec3 p = loadingCenter_ + Vec3{r * std::cos(a), 0.2f + y, r * std::sin(a)};
        float size = 0.012f + 0.006f * (i % 3);
        AddBox(&loadingSparkles_, p, {size, size, size}, static_cast<float>(now) + i, Rgba(255, 236, 180));
    }
}

void VrApp::UpdateLoading(double now) {
    if (!Loading()) return;
    UpdateLoadingSparkles(now);
    if (now - loadingDrawn_ < 0.25) return;
    loadingDrawn_ = now;
    Canvas &c = loadingCanvas_;
    c.Clear(Rgba(16, 18, 34, 220));
    c.StrokeRect(0, 0, c.width(), c.height(), Rgba(214, 178, 82), 3);
    const char *title = "KINGDOM HEARTS 358/2 Days";
    c.DrawText((c.width() - Canvas::TextWidth(title, 2)) / 2, 28, title, Rgba(236, 236, 244), 2);
    int dots = static_cast<int>(now * 3.0) % 4;
    std::string loading = "Loading" + std::string(static_cast<size_t>(dots), '.');
    c.DrawText((c.width() - Canvas::TextWidth("Loading...")) / 2, 92, loading, Rgba(214, 178, 82));
#ifdef KHVR_HAS_GAME
    char line[96];
    std::snprintf(line, sizeof line, "game frames: %u", decomp_->gameFrames());
    c.DrawText((c.width() - Canvas::TextWidth(line)) / 2, 132, line, Rgba(150, 154, 180));
#endif
    loadingTex_.Upload(c.width(), c.height(), c.data(), true);
}

void VrApp::UpdateStatus(double now) {
    UpdateLoading(now);
    std::vector<std::string> lines = Loading() ? std::vector<std::string>() : StatusLines();
    statusVisible_ = !lines.empty();
    if (!statusVisible_ || now - statusDrawn_ < 0.5) return;
    statusDrawn_ = now;
    Canvas &c = statusCanvas_;
    c.Clear(Rgba(16, 18, 34, 235));
    c.StrokeRect(0, 0, c.width(), c.height(), Rgba(214, 178, 82), 3);
    int y = 12;
    for (size_t i = 0; i < lines.size() && y + Canvas::LineHeight() < c.height(); ++i) {
        c.DrawText(14, y, lines[i], i == 0 ? Rgba(214, 178, 82) : Rgba(236, 236, 244));
        y += Canvas::LineHeight() + 2;
    }
    statusTex_.Upload(c.width(), c.height(), c.data(), true);
}

ViewMode VrApp::EffectiveMode() const {
    const GameFrame &g = game_->frame();
    ViewMode mode = settings_.Enum<ViewMode>(SettingId::ViewMode);
    if (g.inMenu || !g.inGameplay) return ViewMode::GiantScreen;
    if (g.inCutscene) {
        switch (settings_.Enum<CutsceneView>(SettingId::CutsceneView)) {
        case CutsceneView::GiantScreen: return ViewMode::GiantScreen;
        case CutsceneView::SameAsGameplay: return mode;
        }
    }
    return mode;
}

void VrApp::ApplyPlatformSettings() {
    static const float kRates[] = {72.0f, 90.0f, 120.0f};
    xr_->SetRefreshRate(kRates[settings_.Int(SettingId::PicRefreshRate)]);
    xr_->SetPassthrough(settings_.Bool(SettingId::Passthrough));
    xr_->SetFoveation(settings_.Int(SettingId::PicFoveation));
}

void VrApp::UpdateTextures() {
    const DsScreens &s = game_->screens();
    if (s.revision != screensRevision_) {
        bool linear = settings_.Enum<TextureFilter>(SettingId::PicTextureFilter) == TextureFilter::Bilinear;
        topTex_.Upload(DsScreens::kWidth, DsScreens::kHeight, s.top.data(), linear);
        hudTex_.Upload(DsScreens::kWidth, DsScreens::kHeight, s.topHud.data(), linear);
        bottomTex_.Upload(DsScreens::kWidth, DsScreens::kHeight, s.bottom.data(), linear);
        screensRevision_ = s.revision;
    }
    if (panel_.isOpen() && panel_.Redraw(settings_))
        panelTex_.Upload(SettingsPanel::kWidth, SettingsPanel::kHeight, panel_.canvas().data(), true);
}

void VrApp::UpdatePlacements(const XrFrameInput &in) {
    const Pose &head = in.head;
    float headYaw = YawOf(head.orientation);
    float hudScale = settings_.Get(SettingId::HudScale);
    float hudDist = settings_.Get(SettingId::HudDistance);

    // The giant screen stays in front of where you looked when you last recentered.
    const bool recenteredLoading = !placementsInitialized_ || recenterNext_;
    if (recenteredLoading) {
        screenAnchor_ = head.position;
        screenYaw_ = headYaw;
        hudYaw_ = headYaw;
        placementsInitialized_ = true;
    }
    Vec3 screenAt = screenAnchor_ + Quat::Yaw(screenYaw_).Forward() * settings_.Get(SettingId::ScreenDistance);
    giantScreen_.pose = FacingFrom(screenAt, screenAnchor_);
    giantScreen_.width = settings_.Get(SettingId::ScreenWidth);
    giantScreen_.height = giantScreen_.width * 0.75f;

    // HUD: floating panels lazily follow your heading; head-locked ones follow exactly.
    float diff = WrapAngle(headYaw - hudYaw_);
    if (std::fabs(diff) > DegToRad(35.0f)) hudYaw_ = WrapAngle(hudYaw_ + diff * 0.08f);
    hudPanel_.width = 0.55f * hudScale;
    hudPanel_.height = hudPanel_.width * 0.75f;
    if (settings_.Enum<TopScreenPlacement>(SettingId::HudTopScreen) == TopScreenPlacement::HeadLocked) {
        hudPanel_.pose = head * Pose{Quat(), {0, -0.05f, -hudDist}};
    } else {
        Vec3 fwd = Quat::Yaw(hudYaw_).Forward();
        Vec3 at = head.position + fwd * hudDist + Vec3{0, -0.18f, 0};
        hudPanel_.pose = FacingFrom(at, head.position);
    }

    // Bottom screen: on the back of the left wrist, facing up toward you, or floating low left.
    bottomPanel_.width = 0.2f * hudScale;
    bottomPanel_.height = bottomPanel_.width * 0.75f;
    if (settings_.Enum<BottomScreenPlacement>(SettingId::HudBottomScreen) == BottomScreenPlacement::LeftWrist) {
        if (in.gripValid[0]) {
            Pose wrist = in.grip[0] * Pose{Quat::AxisAngle({1, 0, 0}, -kPi / 2), {0.0f, 0.04f, 0.09f}};
            bottomPanel_.pose = wrist;
        }
    } else {
        bottomPanel_.width = 0.45f * hudScale;
        bottomPanel_.height = bottomPanel_.width * 0.75f;
        Vec3 fwd = Quat::Yaw(hudYaw_ + DegToRad(25)).Forward();
        Vec3 at = head.position + fwd * (hudDist * 0.8f) + Vec3{0, -0.55f, 0};
        bottomPanel_.pose = FacingFrom(at, head.position);
        bottomPanel_.pose.orientation = bottomPanel_.pose.orientation * Quat::AxisAngle({1, 0, 0}, -0.5f);
    }

    if (loadingArea_.triangles.empty() || recenteredLoading) {
        // STAGE space has its floor at 0; the LOCAL fallback has the head near 0.
        BuildLoadingArea(screenAnchor_.y > 0.8f ? 0.0f : screenAnchor_.y - 1.6f);
    }
    loadingPanel_.width = 1.0f;
    loadingPanel_.height = loadingPanel_.width * 192.0f / 768.0f;
    loadingPanel_.pose = FacingFrom(screenAnchor_ + Quat::Yaw(screenYaw_).Forward() * 2.2f + Vec3{0, 0.25f, 0}, screenAnchor_);

    // The status panel floats just below eye level, to the left of the HUD and clear of it.
    statusPanel_.width = 0.8f;
    statusPanel_.height = statusPanel_.width * 288.0f / 768.0f;
    {
        Vec3 fwd = Quat::Yaw(hudYaw_ + DegToRad(38)).Forward();
        Vec3 at = head.position + fwd * 1.3f + Vec3{0, -0.35f, 0};
        statusPanel_.pose = FacingFrom(at, head.position);
    }

    settingsPanel_.width = 1.0f;
    settingsPanel_.height = 0.75f;
}

bool VrApp::Laser(const XrFrameInput &in, const Placement &p, int texW, int texH, float *px,
                  float *py, float *dist) {
    if (!in.aimValid[1]) return false;
    Vec3 o = in.aim[1].position, d = in.aim[1].orientation.Forward();
    Vec3 n = p.pose.orientation.Rotate({0, 0, 1});
    float denom = Dot(d, n);
    if (std::fabs(denom) < 1e-5f) return false;
    float t = Dot(p.pose.position - o, n) / denom;
    if (t <= 0.0f) return false;
    Vec3 local = p.pose.Inverse().Transform(o + d * t);
    float u = local.x / p.width + 0.5f, v = 0.5f - local.y / p.height;
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    *px = u * texW;
    *py = v * texH;
    *dist = t;
    return true;
}

void VrApp::Frame(const XrFrameInput &in) {
    const float dt = in.dt;
    ViewMode mode = EffectiveMode();
    UpdatePlacements(in);
    recenterNext_ = false;

    ControllerState buttons = in.buttons;

    // ---- Laser pointer: the settings panel, then the bottom (touch) screen ----
    float px = 0, py = 0, dist = 0;
    bool click = buttons.trigger[1] > 0.6f && prevTrigger_[1] <= 0.6f;
    prevTrigger_[0] = buttons.trigger[0];
    prevTrigger_[1] = buttons.trigger[1];
    laserOnPanel_ = laserOnBottom_ = false;
    GameRequests touch;
    if (panel_.isOpen()) {
        laserOnPanel_ = Laser(in, settingsPanel_, SettingsPanel::kWidth, SettingsPanel::kHeight, &px, &py, &dist);
        panel_.Pointer(laserOnPanel_, px, py, click, settings_);
        laserEnd_ = in.aim[1].position + in.aim[1].orientation.Forward() * (laserOnPanel_ ? dist : 2.0f);
        // While the panel is open the game gets no input.
        buttons = ControllerState{};
        buttons.menu = in.buttons.menu;
    } else if (settings_.Enum<BottomScreenPlacement>(SettingId::HudBottomScreen) != BottomScreenPlacement::Hidden &&
               settings_.Enum<TouchInput>(SettingId::HudTouchInput) == TouchInput::Laser &&
               Laser(in, bottomPanel_, DsScreens::kWidth, DsScreens::kHeight, &px, &py, &dist)) {
        laserOnBottom_ = true;
        laserEnd_ = in.aim[1].position + in.aim[1].orientation.Forward() * dist;
        touch.touching = buttons.trigger[1] > 0.6f;
        touch.touchX = static_cast<uint8_t>(Clamp(px, 0, 255));
        touch.touchY = static_cast<uint8_t>(Clamp(py, 0, 191));
        buttons.trigger[1] = 0.0f;  // the trigger touches the screen instead of pressing R
    }

    // ---- Tracking for the rigs and gestures ----
    TrackingFrame t;
    t.dt = dt;
    t.head = in.head;
    for (int h = 0; h < 2; ++h) {
        t.hands[h] = in.grip[h];
        t.handValid[h] = in.gripValid[h];
    }
    t.turnAxis = panel_.isOpen() ? 0.0f : Deadzone(buttons.stickX[1]);
    t.moveAxis = {Deadzone(buttons.stickX[0]), 0.0f, -Deadzone(buttons.stickY[0])};

    CombatFrame combat;
    const CombatFrame *combatPtr = nullptr;
    if (mode == ViewMode::FirstPerson && !panel_.isOpen()) {
        CombatInput ci;
        ci.dt = dt;
        ci.head = in.head;
        for (int h = 0; h < 2; ++h) {
            ci.hands[h] = in.grip[h];
            ci.handValid[h] = in.gripValid[h];
            ci.trigger[h] = buttons.trigger[h];
        }
        ci.moveAxis = t.moveAxis;
        combat = combat_.Update(settings_, ci);
        combatPtr = &combat;
        for (int h = 0; h < 2; ++h) xr_->Haptic(h, combat.hapticAmplitude[h], combat.hapticSeconds[h]);
    }

    const GameFrame &before = game_->frame();
    Vec3 camDir = before.camera.target - before.camera.eye;
    float gameCamYaw = YawOf(Vec3{camDir.x, 0, camDir.z});
    float moveBasis = mode == ViewMode::GiantScreen ? gameCamYaw : rig_.moveBasisYaw;
    mapper_.Update(settings_, mode, buttons, combatPtr, moveBasis, gameCamYaw, dt);

    const AppActions &act = mapper_.actions();
    if (act.openSettingsPanel) {
        if (panel_.isOpen()) {
            panel_.Close();
        } else {
            Vec3 fwd = Quat::Yaw(YawOf(in.head.orientation)).Forward();
            settingsPanel_.pose = FacingFrom(in.head.position + fwd * 1.1f + Vec3{0, -0.1f, 0}, in.head.position);
            panel_.Open();
        }
    }
    if (!panel_.isOpen()) SaveSettingsIfChanged();
    if (act.toggleViewMode) {
        ViewMode m = settings_.Enum<ViewMode>(SettingId::ViewMode);
        settings_.SetEnum(SettingId::ViewMode, m == ViewMode::FirstPerson ? ViewMode::ThirdPerson : ViewMode::FirstPerson);
        mode = EffectiveMode();
    }
    t.recenter = act.recenter;
    if (act.recenter) recenterNext_ = true;
    if (mode != lastMode_) {
        thirdPerson_.Reset();
        firstPerson_.Reset();
        combat_.Reset();
        if (settings_.Bool(SettingId::ComfortBlink)) blinkTime_ = 0.0f;
        lastMode_ = mode;
    }

    // ---- Run the game ----
    GameRequests req;
    if (mode != ViewMode::GiantScreen) req = nextRequests_;
    req.hasAnalogMove = Length(mapper_.analogMoveWorld()) > 0.0f;
    req.analogMoveWorld = mapper_.analogMoveWorld();
    req.hasRoomScaleStep = Length(pendingRoomScale_) > 0.0f;
    req.roomScaleStep = pendingRoomScale_;
    req.touching = touch.touching;
    req.touchX = touch.touchX;
    req.touchY = touch.touchY;
    uint32_t frameBefore = game_->frame().frameCounter;
    game_->Advance(in.time, req, SampleThunk, &mapper_);
    if (game_->frame().frameCounter != frameBefore) pendingRoomScale_ = {};

    // ---- Place the view ----
    const GameFrame &g = game_->frame();
    if (mode == ViewMode::ThirdPerson) rig_ = thirdPerson_.Update(settings_, g, t);
    else if (mode == ViewMode::FirstPerson) rig_ = firstPerson_.Update(settings_, g, t);
    else rig_ = RigOutput{};
    nextRequests_ = rig_.requests;
    if (rig_.requests.hasRoomScaleStep) pendingRoomScale_ += rig_.requests.roomScaleStep;
    if (rig_.blinkStarted) blinkTime_ = 0.0f;
    if (blinkTime_ >= 0.0f) {
        blinkTime_ += dt;
        if (blinkTime_ > settings_.Get(SettingId::ComfortBlinkMs) / 1000.0f) blinkTime_ = -1.0f;
    }

    ApplyPlatformSettings();
    UpdateTextures();
    UpdateStatus(in.time);
    if (!in.shouldRender) return;
    // The game's own picture needs no 3D re-render until the geometry capture exists.
    if (mode == ViewMode::GiantScreen && !game_->world().triangles.empty()) RenderGiantScreenViews(in);
    for (int eye = 0; eye < 2; ++eye) {
        xr_->AcquireEye(eye);
        RenderEye(in, eye);
        xr_->ReleaseEye(eye);
    }
}

void VrApp::RenderGiantScreenViews(const XrFrameInput &in) {
    // The game's own camera, rendered at headset resolution instead of 256x192. With stereo on,
    // each eye gets its own camera, so the level has depth behind the screen.
    const GameFrame &g = game_->frame();
    bool stereo = settings_.Bool(SettingId::ScreenStereo);
    float sep = kIpd * settings_.Get(SettingId::ScreenDepth) * settings_.Get(SettingId::GameUnitsPerMeter);
    float fovY = g.camera.fovYRad > 0 ? g.camera.fovYRad : DegToRad(55.0f);
    float ty = std::tan(fovY * 0.5f), tx = ty * 4.0f / 3.0f;
    Mat4 proj = Mat4::Projection(-tx, tx, ty, -ty, 0.1f, 1000.0f);
    Vec3 f = Normalize(g.camera.target - g.camera.eye);
    Vec3 right = Normalize(Cross(f, g.camera.up));
    for (int eye = 0; eye < (stereo ? 2 : 1); ++eye) {
        float offset = stereo ? (eye == 0 ? -0.5f : 0.5f) * sep : 0.0f;
        Vec3 shift = right * offset;
        Mat4 view = LookAt(g.camera.eye + shift, g.camera.target + shift, g.camera.up);
        giantViews_[eye].Ensure(kGiantViewW, kGiantViewH);
        giantViews_[eye].Bind();
        glClearColor(0.42f, 0.58f, 0.85f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderer_.DrawTriangles(game_->world(), proj * view);
        renderer_.DrawTriangles(game_->player(), proj * view);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void VrApp::DrawProps(const XrFrameInput &in, const Mat4 &vpTracking) {
    WorldDrawList props;
    const ViewMode mode = lastMode_;
    BodyDisplay body = settings_.Enum<BodyDisplay>(SettingId::FpShowBody);
    int weapon = settings_.Enum<Hand>(SettingId::FpWeaponHand) == Hand::Left ? 0 : 1;
    bool dual = settings_.Bool(SettingId::FpDualWield);
    for (int h = 0; h < 2; ++h) {
        if (!in.gripValid[h]) continue;
        const Pose &g = in.grip[h];
        if (mode == ViewMode::FirstPerson && body != BodyDisplay::Hidden) {
            // A gloved hand, and the Keyblade along the blade axis the combat code uses.
            AddOrientedBox(&props, g.position, {0.045f, 0.05f, 0.06f}, g.orientation, Rgba(20, 20, 26));
            if (h == weapon || dual) {
                // Shaft, guard and teeth of a Kingdom Key, along the axis swings are measured on.
                float len = CombatRecognizer::kBladeLength * settings_.Get(SettingId::FpKeybladeLength);
                float grip = DegToRad(settings_.Get(SettingId::FpGripAngle));
                Vec3 axis = BladeAxis(g, grip);
                Quat q = g.orientation * Quat::AxisAngle({1, 0, 0}, grip);
                AddOrientedBox(&props, g.position + axis * (len * 0.5f), {0.016f, 0.016f, len * 0.5f}, q,
                               Rgba(205, 205, 220));
                AddOrientedBox(&props, g.position + axis * 0.07f, {0.08f, 0.02f, 0.015f}, q, Rgba(230, 200, 70));
                AddOrientedBox(&props, g.position + axis * (len - 0.04f) + q.Rotate({0.05f, 0, 0}),
                               {0.05f, 0.016f, 0.03f}, q, Rgba(205, 205, 220));
            }
        }
        // Outside first person the hands are not drawn: the wrist screen and the laser are enough.
    }
    renderer_.DrawTriangles(props, vpTracking);
}

void VrApp::RenderEye(const XrFrameInput &in, int eye) {
    const ViewMode mode = lastMode_;
    const bool passthrough = settings_.Bool(SettingId::Passthrough);
    Mat4 proj = EyeProjection(in.eyes[eye].fov);
    Mat4 vpTracking = proj * ViewFromPose(in.eyes[eye].pose);

    if (passthrough) glClearColor(0, 0, 0, 0);
    else if (Loading()) glClearColor(0.01f, 0.01f, 0.03f, 1.0f);
    else if (mode == ViewMode::GiantScreen) glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
    else glClearColor(0.42f, 0.58f, 0.85f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    if (Loading()) {
        // The loading area instead of a black giant screen, until the game draws something.
        renderer_.DrawTriangles(loadingArea_, vpTracking);
        renderer_.DrawTriangles(loadingSparkles_, vpTracking);
        renderer_.DrawTriangles(loadingBeam_, vpTracking, 0.10f);
    } else if (mode == ViewMode::GiantScreen) {
        float curv = settings_.Get(SettingId::ScreenCurvature);
        float dist = settings_.Get(SettingId::ScreenDistance);
        if (game_->world().triangles.empty()) {
            // The DS's top screen exactly as the game draws it.
            renderer_.DrawQuad(topTex_.id(), giantScreen_.pose, giantScreen_.width, giantScreen_.height,
                               vpTracking, 1.0f, curv, dist);
        } else {
            int view = settings_.Bool(SettingId::ScreenStereo) ? eye : 0;
            renderer_.DrawQuad(giantViews_[view].texture(), giantScreen_.pose, giantScreen_.width,
                               giantScreen_.height, vpTracking, 1.0f, curv, dist, true);
            // The 2D HUD sits on the screen surface, a few millimeters in front.
            Pose hudOnScreen = giantScreen_.pose * Pose{Quat(), {0, 0, 0.005f}};
            renderer_.DrawQuad(hudTex_.id(), hudOnScreen, giantScreen_.width, giantScreen_.height,
                               vpTracking, 1.0f, curv, dist);
        }
    } else {
        // The game world, through the rig's mapping from tracking space into the world.
        Mat4 worldToTracking = Mat4::FromSimilarity(rig_.trackingToWorld.Inverse());
        Mat4 vpWorld = vpTracking * worldToTracking;
        renderer_.DrawTriangles(game_->world(), vpWorld);
        bool drawPlayer = mode == ViewMode::ThirdPerson ||
                          settings_.Enum<BodyDisplay>(SettingId::FpShowBody) == BodyDisplay::FullBody;
        if (drawPlayer) renderer_.DrawTriangles(game_->player(), vpWorld);
    }
    DrawProps(in, vpTracking);

    // Panels float in front of the world: never let a pillar, the floor or the character cut into
    // them. They still sort against each other and the laser.
    glClear(GL_DEPTH_BUFFER_BIT);
    if (mode != ViewMode::GiantScreen &&
        settings_.Enum<TopScreenPlacement>(SettingId::HudTopScreen) != TopScreenPlacement::Hidden)
        renderer_.DrawQuad(hudTex_.id(), hudPanel_.pose, hudPanel_.width, hudPanel_.height, vpTracking);
    if (settings_.Enum<BottomScreenPlacement>(SettingId::HudBottomScreen) != BottomScreenPlacement::Hidden &&
        (settings_.Enum<BottomScreenPlacement>(SettingId::HudBottomScreen) != BottomScreenPlacement::LeftWrist ||
         in.gripValid[0]))
        renderer_.DrawQuad(bottomTex_.id(), bottomPanel_.pose, bottomPanel_.width, bottomPanel_.height, vpTracking);
    if (Loading() && loadingTex_.id())
        renderer_.DrawQuad(loadingTex_.id(), loadingPanel_.pose, loadingPanel_.width, loadingPanel_.height, vpTracking);
    if (statusVisible_ && statusTex_.id())
        renderer_.DrawQuad(statusTex_.id(), statusPanel_.pose, statusPanel_.width, statusPanel_.height, vpTracking);
    if (panel_.isOpen())
        renderer_.DrawQuad(panelTex_.id(), settingsPanel_.pose, settingsPanel_.width,
                           settingsPanel_.height, vpTracking);
    if (laserOnPanel_ || laserOnBottom_ || panel_.isOpen())
        renderer_.DrawLine(in.aim[1].position, laserEnd_, Rgba(214, 178, 82, 220), vpTracking);

    float blink = 0.0f;
    if (blinkTime_ >= 0.0f) {
        float len = settings_.Get(SettingId::ComfortBlinkMs) / 1000.0f;
        float x = blinkTime_ / len;  // 0..1: fade out, hold, fade in
        blink = x < 0.3f ? x / 0.3f : (x > 0.7f ? (1.0f - x) / 0.3f : 1.0f);
    }
    renderer_.DrawComfortOverlay(mode == ViewMode::GiantScreen ? 0.0f : rig_.vignette, blink);
}

}  // namespace khvr
