#include "vr_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "khvr/upscale.h"
#include "khvr/canvas.h"
#include "station_art.h"
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

// A pose at `at` whose +Z (a quad's face) points at `eye`, kept upright.
Pose FacingEye(const Vec3 &at, const Vec3 &eye) {
    Vec3 d = eye - at;
    float yaw = std::atan2(d.x, d.z);
    float pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
    return {Quat::Yaw(yaw) * Quat::AxisAngle({1, 0, 0}, -pitch), at};
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

namespace {
// Stands in for the game while the app waits for all-files access or a ROM: black screens, no
// world. The loading area shows meanwhile.
class WaitingGame : public GameSource {
public:
    WaitingGame() {
        const size_t n = DsScreens::kWidth * DsScreens::kHeight;
        screens_.top.assign(n, 0xff000000u);
        screens_.topHud.assign(n, 0);
        screens_.bottom.assign(n, 0xff000000u);
        frame_.inMenu = true;
        frame_.camera.eye = {0, 0, 1};
    }
    void Advance(double, const GameRequests &, uint16_t (*)(void *), void *) override {}
    const GameFrame &frame() const override { return frame_; }
    const WorldDrawList &world() const override { return world_; }
    const DsScreens &screens() const override { return screens_; }

private:
    GameFrame frame_;
    WorldDrawList world_;
    DsScreens screens_;
};
}  // namespace

void VrApp::StartGame() {
#ifdef KHVR_HAS_GAME
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
        waiting_ = false;
        notice_.clear();
        LOGI("game: %s", message.c_str());
        return;
    }
    if (sharedDir_.empty()) {
        needsStorageAccess_ = true;
        message = "Allow \"All files access\" for KH Days VR in the settings page that opened, so it "
                  "can read your ROM from the KHDaysVR folder. The game starts as soon as you do.";
    } else {
        message += " Copy your own dump of the EU cartridge into the KHDaysVR folder on the headset "
                   "(any name ending in .nds): the game starts as soon as it is there.";
    }
    if (notice_ != message) LOGW("game: %s - waiting", message.c_str());
    notice_ = message;
    waiting_ = true;
    if (!game_) game_ = std::make_unique<WaitingGame>();
#else
    notice_ = "This build has no game (built without the decomp guest). Running the demo arena.";
    auto demo = std::make_unique<DemoGame>();
    demo_ = demo.get();
    game_ = std::move(demo);
#endif
}

void VrApp::RetryGame(const std::string &sharedDir, bool hasAccess) {
    if (!waiting_) return;
    SetSharedDir(sharedDir, hasAccess);
    StartGame();
}

std::vector<std::string> VrApp::StatusLines() const {
    std::vector<std::string> lines;
    if (waiting_) return lines;  // the loading panel says what to do
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
    if (waiting_) return true;
    return decomp_ && decomp_->state() == PlatformState::Running && !decomp_->hasPicture();
#else
    return false;
#endif
}

void VrApp::BuildLoadingArea(float floorY) {
    // An original take on the series' Station of Awakening: a stained-glass pillar top floating in
    // darkness, lit from above, with a column of light around you.
    loadingArea_.triangles.clear();
    loadingBeam_.triangles.clear();
    const Vec3 up{0, 1, 0};
    const Vec3 c{screenAnchor_.x, floorY, screenAnchor_.z};
    const float kRadius = 3.2f;  // the painted window is 6.4 m across
    auto ringPoint = [&](float r, float a, float y = 0.0f) {
        return c + Vec3{r * std::cos(a), y, r * std::sin(a)};
    };
    // The glass itself is a painted texture (station_art.cpp), drawn as a disc in RenderEye.

    // The pillar below, fading into the dark.
    const int kSides = 48;
    for (int i = 0; i < kSides; ++i) {
        float a0 = 2 * kPi * i / kSides, a1 = 2 * kPi * (i + 1) / kSides;
        Vec3 t0 = ringPoint(kRadius, a0), t1 = ringPoint(kRadius, a1);
        Vec3 b0 = ringPoint(kRadius * 0.92f, a0, -24.0f), b1 = ringPoint(kRadius * 0.92f, a1, -24.0f);
        Vec3 n = Normalize(Vec3{std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2)});
        uint32_t top = Rgba(96, 52, 28), bottom = Rgba(8, 5, 4);
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
        uint32_t lo = Rgba(255, 236, 196), hi = Rgba(200, 120, 90);
        loadingBeam_.triangles.push_back({l0, n, lo});
        loadingBeam_.triangles.push_back({l1, n, lo});
        loadingBeam_.triangles.push_back({h1, n, hi});
        loadingBeam_.triangles.push_back({l0, n, lo});
        loadingBeam_.triangles.push_back({h1, n, hi});
        loadingBeam_.triangles.push_back({h0, n, hi});
    }
    // The platform's rim: a gold band around the edge, below the glass, as the series' stations
    // have.
    for (int i = 0; i < kSides * 2; ++i) {
        float a0 = kPi * i / kSides, a1 = kPi * (i + 1) / kSides;
        Vec3 n = Normalize(Vec3{std::cos((a0 + a1) / 2), 0, std::sin((a0 + a1) / 2)});
        Vec3 t0 = ringPoint(kRadius * 1.004f, a0, 0.004f), t1 = ringPoint(kRadius * 1.004f, a1, 0.004f);
        Vec3 b0 = ringPoint(kRadius * 1.004f, a0, -0.16f), b1 = ringPoint(kRadius * 1.004f, a1, -0.16f);
        uint32_t gold = Rgba(196, 150, 60), dark = Rgba(110, 76, 30);
        loadingArea_.triangles.push_back({t0, n, gold});
        loadingArea_.triangles.push_back({t1, n, gold});
        loadingArea_.triangles.push_back({b1, n, dark});
        loadingArea_.triangles.push_back({t0, n, gold});
        loadingArea_.triangles.push_back({b1, n, dark});
        loadingArea_.triangles.push_back({b0, n, dark});
        // Its top edge, a thin ring around the glass.
        Vec3 i0 = ringPoint(kRadius * 0.996f, a0, 0.004f), i1 = ringPoint(kRadius * 0.996f, a1, 0.004f);
        loadingArea_.triangles.push_back({i0, up, gold});
        loadingArea_.triangles.push_back({t1, up, gold});
        loadingArea_.triangles.push_back({i1, up, gold});
        loadingArea_.triangles.push_back({i0, up, gold});
        loadingArea_.triangles.push_back({t0, up, gold});
        loadingArea_.triangles.push_back({t1, up, gold});
    }
    loadingCenter_ = c;
    if (!stationTex_.id()) {
        std::vector<uint32_t> art = PaintStationArt(1024);
        stationTex_.Upload(1024, 1024, art.data(), true, true);
    }
    if (!glowTex_.id()) {
        std::vector<uint32_t> glow = PaintGlow(64);
        glowTex_.Upload(64, 64, glow.data(), true, true);
    }
}

void VrApp::UpdateLoadingSparkles(double now) {
    // Motes of light drifting up through the column and swaying as they rise, each fading in at
    // the floor, twinkling, and fading out high above, so the area is visibly alive.
    loadingSparkles_.triangles.clear();
    motes_.clear();
    const float t = static_cast<float>(now);
    for (int i = 0; i < 64; ++i) {
        float seed = static_cast<float>(i) * 12.9898f;
        float a = std::fmod(seed * 3.7f, 2 * kPi) + t * (0.03f + 0.02f * (i % 3));
        float r = 0.35f + std::fmod(seed * 0.618f, 2.6f);
        const float rise = 6.0f;
        float y = std::fmod(t * (0.10f + 0.04f * (i % 5)) + seed * 0.37f, rise);
        float sway = 0.08f * std::sin(t * 0.7f + seed);
        Vec3 p = loadingCenter_ + Vec3{(r + sway) * std::cos(a), 0.15f + y, (r + sway) * std::sin(a)};
        float life = std::min(y / 0.6f, (rise - y) / 1.5f);
        float twinkle = 0.75f + 0.25f * std::sin(t * (2.0f + (i % 4)) + seed * 5.0f);
        motes_.push_back({p, 0.035f + 0.02f * static_cast<float>(i % 4) / 3.0f,
                          std::clamp(life, 0.0f, 1.0f) * twinkle});
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
    if (waiting_) {
        // What the game is waiting for, wrapped to the panel.
        std::string rest = notice_;
        int y = 80;
        while (!rest.empty() && y + Canvas::LineHeight() <= c.height() - 8) {
            size_t cut = rest.size() <= 74 ? rest.size() : rest.rfind(' ', 74);
            if (cut == std::string::npos || cut == 0) cut = std::min<size_t>(74, rest.size());
            std::string line = rest.substr(0, cut);
            c.DrawText((c.width() - Canvas::TextWidth(line)) / 2, y, line, Rgba(214, 178, 82));
            rest = rest.substr(cut < rest.size() && rest[cut] == ' ' ? cut + 1 : cut);
            y += Canvas::LineHeight() + 2;
        }
        loadingTex_.Upload(c.width(), c.height(), c.data(), true);
        return;
    }
    int dots = static_cast<int>(now * 3.0) % 4;
    std::string loading = "Loading" + std::string(static_cast<size_t>(dots), '.');
    c.DrawText((c.width() - Canvas::TextWidth("Loading...")) / 2, 92, loading, Rgba(214, 178, 82));
#ifdef KHVR_HAS_GAME
    if (decomp_) {
        char line[96];
        std::snprintf(line, sizeof line, "game frames: %u", decomp_->gameFrames());
        c.DrawText((c.width() - Canvas::TextWidth(line)) / 2, 132, line, Rgba(150, 154, 180));
    }
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
        const TextureFilter filter = settings_.Enum<TextureFilter>(SettingId::PicTextureFilter);
        auto upload = [&](Texture &tex, const std::vector<uint32_t> &px) {
            if (filter == TextureFilter::Smooth) {
                // Smooth: Scale4x rounds diagonal edges, then bilinear filtering softens the rest.
                constexpr int W = DsScreens::kWidth * 4, H = DsScreens::kHeight * 4;
                smoothScratch_.resize(static_cast<size_t>(W / 2) * (H / 2));
                sharpScratch_.resize(static_cast<size_t>(W) * H);
                Scale4x(px.data(), DsScreens::kWidth, DsScreens::kHeight, smoothScratch_.data(), sharpScratch_.data());
                tex.Upload(W, H, sharpScratch_.data(), true);
                return;
            }
            if (filter != TextureFilter::Sharp) {
                tex.Upload(DsScreens::kWidth, DsScreens::kHeight, px.data(), filter == TextureFilter::Bilinear);
                return;
            }
            // Sharp: each DS pixel becomes a kSharp x kSharp block, then bilinear filtering only
            // softens the block edges. Crisp pixel art without nearest-neighbour shimmer.
            constexpr int kSharp = 3, W = DsScreens::kWidth * kSharp, H = DsScreens::kHeight * kSharp;
            sharpScratch_.resize(static_cast<size_t>(W) * H);
            for (int y = 0; y < DsScreens::kHeight; ++y) {
                uint32_t *row = &sharpScratch_[static_cast<size_t>(y) * kSharp * W];
                const uint32_t *src = &px[static_cast<size_t>(y) * DsScreens::kWidth];
                for (int x = 0; x < DsScreens::kWidth; ++x)
                    for (int k = 0; k < kSharp; ++k) row[x * kSharp + k] = src[x];
                for (int k = 1; k < kSharp; ++k) std::copy(row, row + W, row + static_cast<size_t>(k) * W);
            }
            tex.Upload(W, H, sharpScratch_.data(), true);
        };
        if (s.topScale > 1 && !s.topHi.empty()) {
            // The 3D was drawn large: no upscaler needed, only filtering.
            topTex_.Upload(DsScreens::kWidth * s.topScale, DsScreens::kHeight * s.topScale, s.topHi.data(),
                           filter != TextureFilter::Nearest);
        } else {
            upload(topTex_, s.top);
        }
        upload(hudTex_, s.topHud);
        if (!s.topFront.empty()) {
            upload(frontTex_, s.topFront);
            upload(backTex_, s.topBack);
        }
        upload(bottomTex_, s.bottom);
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
    if (BothScreens()) {
        // The DS's two screens one above the other, about where the one screen was.
        const Pose middle = giantScreen_.pose;
        const float w = giantScreen_.width * 0.6f, h = w * 0.75f, half = (h + w * 0.03f) * 0.5f;
        giantScreen_ = {middle * Pose{Quat(), {0.0f, half, 0.0f}}, w, h};
        giantBottom_ = {middle * Pose{Quat(), {0.0f, -half, 0.0f}}, w, h};
    }

    // HUD: floating panels lazily follow your heading; head-locked ones follow exactly.
    float diff = WrapAngle(headYaw - hudYaw_);
    if (std::fabs(diff) > DegToRad(35.0f)) hudYaw_ = WrapAngle(hudYaw_ + diff * 0.08f);
    hudPanel_.width = 0.55f * hudScale;
    hudPanel_.height = hudPanel_.width * 0.75f;
    if (settings_.Enum<TopScreenPlacement>(SettingId::HudTopScreen) == TopScreenPlacement::HeadLocked) {
        hudPanel_.pose = head * Pose{Quat(), {0, settings_.Get(SettingId::HudHeight), -hudDist}};
    } else {
        Vec3 fwd = Quat::Yaw(hudYaw_).Forward();
        Vec3 at = head.position + fwd * hudDist + Vec3{0, settings_.Get(SettingId::HudHeight), 0};
        hudPanel_.pose = FacingFrom(at, head.position);
    }

    // Bottom screen: held above a hand like a handheld, worn on the wrist like a watch, or
    // floating low left. Held and worn screens can be moved and turned along their own axes, and
    // grow in only while you look toward them.
    const BottomScreenPlacement placement = settings_.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement);
    const int sideSetting = settings_.Enum<Hand>(SettingId::HandScreenSide) == Hand::Left ? 0 : 1;
    if (sideSetting != heldHandSetting_) {
        heldHand_ = -1;  // the setting changed: it decides again
        heldHandSetting_ = sideSetting;
    }
    if (handScreenSide_ != (heldHand_ >= 0 ? heldHand_ : sideSetting)) handScreenLockValid_ = false;
    handScreenSide_ = heldHand_ >= 0 ? heldHand_ : sideSetting;
    const HandheldDs handheld = settings_.Enum<HandheldDs>(SettingId::HandScreenDs);
    handheld_ = placement != BottomScreenPlacement::Hidden && !Loading() &&
                (handheld == HandheldDs::Always || handheld == HandheldDs::Only ||
                 (handheld == HandheldDs::Movies && game_->screens().movie));
    if (placement == BottomScreenPlacement::InHand || placement == BottomScreenPlacement::OnWrist) {
        bottomPanel_.width = handheld_ ? settings_.Get(SettingId::HandScreenDsSize) / 2.15f *
                                             settings_.Get(SettingId::HandScreenDsScreenScale) / 100.0f
                                       : settings_.Get(SettingId::HandScreenSize);
        bottomPanel_.height = bottomPanel_.width * 0.75f;
        const int h = handScreenSide_;
        if (in.gripValid[h]) {
            // Anchored to the hand (just above the controller) or to the wrist (where a watch
            // sits, behind the controller along the forearm), always facing your eyes upright:
            // the controller's own axes vary too much with how it is held to orient a screen.
            Vec3 anchor = placement == BottomScreenPlacement::InHand
                              ? in.grip[h].position + Vec3{0.0f, 0.03f + bottomPanel_.height * 0.5f, 0.0f}
                              : in.grip[h].Transform({0.0f, -0.02f, 0.13f});
            // Held with both hands: the other controller brought up to the handheld takes it to
            // the middle of the two hands; it goes back to one hand once they are pulled apart.
            const int other = 1 - h;
            if (handheld_ && placement == BottomScreenPlacement::InHand &&
                settings_.Bool(SettingId::HandScreenDsTwoHands) && in.gripValid[other]) {
                float toHandheld = Length(in.grip[other].position - bottomPanel_.pose.position);
                float apart = Length(in.grip[other].position - in.grip[h].position);
                // While that hand points at the touch screen it comes much closer before taking
                // hold, so the pointer stays usable right in front of the handheld.
                const float reach = laserOnBottom_ ? 0.05f : 0.14f;
                if (!twoHanded_ && toHandheld < reach) {
                    twoHanded_ = true;
                } else if (twoHanded_ && apart > 0.40f) {
                    // Released: the hand that stayed by the handheld keeps it; the one that moved
                    // away is free (and becomes the pointer hand).
                    twoHanded_ = false;
                    const Vec3 at = bottomPanel_.pose.position;
                    const int stays = Length(in.grip[other].position - at) < Length(in.grip[h].position - at) ? other : h;
                    if (stays != h) {
                        heldHand_ = stays;
                        handScreenSide_ = stays;
                        handScreenLockValid_ = false;
                    }
                }
            } else {
                twoHanded_ = false;
            }
            twoHandBlend_ = Clamp(twoHandBlend_ + (twoHanded_ ? 1.0f : -1.0f) * in.dt / 0.2f, 0.0f, 1.0f);
            if (twoHandBlend_ > 0.0f) {
                Vec3 middle = (in.grip[h].position + in.grip[other].position) * 0.5f +
                              Vec3{0.0f, 0.03f + bottomPanel_.height * 0.5f, 0.0f};
                float e = twoHandBlend_ * twoHandBlend_ * (3.0f - 2.0f * twoHandBlend_);
                anchor = anchor + (middle - anchor) * e;
            }
            Quat turn = Quat::AxisAngle({0, 1, 0}, DegToRad(settings_.Get(SettingId::HandScreenTurn))) *
                        Quat::AxisAngle({1, 0, 0}, DegToRad(settings_.Get(SettingId::HandScreenTilt))) *
                        Quat::AxisAngle({0, 0, 1}, -DegToRad(settings_.Get(SettingId::HandScreenRoll)));
            Vec3 move{settings_.Get(SettingId::HandScreenOffsetX), settings_.Get(SettingId::HandScreenOffsetY),
                      settings_.Get(SettingId::HandScreenOffsetZ)};
            Quat face = FacingEye(anchor, head.position).orientation;
            if (settings_.Bool(SettingId::HandScreenFollowView)) {
                handScreenLockValid_ = false;
            } else {
                // Fixed to the hand: keep the angle relative to the controller that it had when
                // it last faced you (switching this off, or a recenter).
                if (!handScreenLockValid_ || recenterNext_) {
                    handScreenLock_ = in.grip[h].orientation.Conjugate() * face;
                    handScreenLockValid_ = true;
                }
                face = in.grip[h].orientation * handScreenLock_;
            }
            bottomPanel_.pose = Pose{face * turn, anchor} * Pose{Quat(), move};
        }
        // Shown while you look toward it (wider to keep it than to bring it up), or always.
        bool wanted = !settings_.Bool(SettingId::HandScreenAutoShow) || laserOnBottom_ ||
                      (handheld_ && handheld == HandheldDs::Only) || twoHanded_;
        if (!wanted && in.gripValid[h]) {
            Vec3 to = bottomPanel_.pose.position - head.position;
            float dist = Length(to);
            float angle = std::acos(Clamp(Dot(to * (1.0f / std::max(dist, 1e-4f)), head.orientation.Forward()), -1.0f, 1.0f));
            // Measured from where the head points; people glance down at a hand mostly with their
            // eyes, so the cone is wide.
            wanted = dist < 1.2f && angle < DegToRad(handScreenShown_ > 0.5f ? 60.0f : 45.0f);
        }
        if (!in.gripValid[h]) wanted = false;
        float step = in.dt / 0.18f;  // grow or shrink in about 0.18 s
        handScreenShown_ = Clamp(handScreenShown_ + (wanted ? step : -step), 0.0f, 1.0f);
    } else {
        bottomPanel_.width = 0.45f * hudScale;
        bottomPanel_.height = bottomPanel_.width * 0.75f;
        Vec3 fwd = Quat::Yaw(hudYaw_ + DegToRad(25)).Forward();
        Vec3 at = head.position + fwd * (hudDist * 0.8f) + Vec3{0, -0.55f, 0};
        bottomPanel_.pose = FacingFrom(at, head.position);
        bottomPanel_.pose.orientation = bottomPanel_.pose.orientation * Quat::AxisAngle({1, 0, 0}, -0.5f);
        handScreenShown_ = 1.0f;
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

bool VrApp::Laser(const XrFrameInput &in, int hand, const Placement &p, int texW, int texH, float *px,
                  float *py, float *dist) {
    if (!in.aimValid[hand]) return false;
    Vec3 o = in.aim[hand].position, d = in.aim[hand].orientation.Forward();
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
#ifdef KHVR_HAS_GAME
    if (decomp_) {
        // While the headset draws the game's 3D itself, the DS picture's 3D is not shown: no need
        // to draw it large too.
        const bool headsetDraws3d = settings_.Enum<GamePicture>(SettingId::ScreenPicture) != GamePicture::Ds;
        decomp_->Set3dScale(headsetDraws3d ? 1 : settings_.Int(SettingId::Pic3dResolution) + 1);
        const bool standInWorld = settings_.Bool(SettingId::ViewStandInWorld);
        decomp_->SetStandInWorld(standInWorld);
        decomp_->SetParallelCapture(settings_.Bool(SettingId::PicParallelCapture));
        decomp_->SetHidePlayer(lastMode_ == ViewMode::FirstPerson &&
                               settings_.Enum<BodyDisplay>(SettingId::FpShowBody) != BodyDisplay::FullBody);
        decomp_->SetWorldCapture(settings_.Enum<GamePicture>(SettingId::ScreenPicture) != GamePicture::Ds || standInWorld);
    }
#endif
    ViewMode mode = EffectiveMode();
    UpdatePlacements(in);
    recenterNext_ = false;

    ControllerState buttons = in.buttons;

    // ---- Laser pointer: the settings panel (right hand), then the bottom (touch) screen, which
    // the hand that is not holding it points at ----
    const BottomScreenPlacement placement = settings_.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement);
    const bool held = placement == BottomScreenPlacement::InHand || placement == BottomScreenPlacement::OnWrist;
    const int touchHand = held ? 1 - handScreenSide_ : 1;
    float px = 0, py = 0, dist = 0;
    bool click = buttons.trigger[1] > 0.6f && prevTrigger_[1] <= 0.6f;
    prevTrigger_[0] = buttons.trigger[0];
    prevTrigger_[1] = buttons.trigger[1];
    laserOnPanel_ = laserOnBottom_ = false;
    laserHand_ = 1;
    GameRequests touch;
    if (panel_.isOpen()) {
        laserOnPanel_ = Laser(in, 1, settingsPanel_, SettingsPanel::kWidth, SettingsPanel::kHeight, &px, &py, &dist);
        panel_.Pointer(laserOnPanel_, px, py, click, settings_);
        // Either thumbstick up or down scrolls the page: a line at once, then repeating.
        const float stick = std::fabs(in.buttons.stickY[1]) > std::fabs(in.buttons.stickY[0]) ? in.buttons.stickY[1]
                                                                                           : in.buttons.stickY[0];
        if (std::fabs(stick) > 0.6f) {
            panelScrollWait_ -= dt;
            if (panelScrollWait_ <= 0.0f) {
                panel_.Scroll(stick > 0 ? -1 : 1);
                panelScrollWait_ = panelScrollHeld_ ? 0.09f : 0.35f;
                panelScrollHeld_ = true;
            }
        } else {
            panelScrollWait_ = 0.0f;
            panelScrollHeld_ = false;
        }
        laserEnd_ = in.aim[1].position + in.aim[1].orientation.Forward() * (laserOnPanel_ ? dist : 2.0f);
        // While the panel is open the game gets no input.
        buttons = ControllerState{};
        buttons.menu = in.buttons.menu;
    } else if (settings_.Enum<TouchInput>(SettingId::HudTouchInput) == TouchInput::Laser &&
               ((placement != BottomScreenPlacement::Hidden && handScreenShown_ > 0.9f &&
                 Laser(in, touchHand, bottomPanel_, DsScreens::kWidth, DsScreens::kHeight, &px, &py, &dist)) ||
                (GiantBottomShown(mode) &&
                 Laser(in, touchHand, giantBottom_, DsScreens::kWidth, DsScreens::kHeight, &px, &py, &dist)))) {
        laserOnBottom_ = true;
        laserHand_ = touchHand;
        laserEnd_ = in.aim[touchHand].position + in.aim[touchHand].orientation.Forward() * dist;
        // The pointer hand's trigger touches; its face buttons stay the DS's (A confirms the
        // highlighted choice as usual).
        touch.touching = buttons.trigger[touchHand] > 0.6f;
        touch.touchX = static_cast<uint8_t>(Clamp(px, 0, 255));
        touch.touchY = static_cast<uint8_t>(Clamp(py, 0, 191));
        buttons.trigger[touchHand] = 0.0f;  // the trigger touches the screen instead of pressing L/R
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
    mapper_.SetMenuStick(before.menuInput);
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
    if (mode == ViewMode::GiantScreen && (!game_->world().triangles.empty() || World3d()) && !(handheld_ && DsOnly()))
        RenderGiantScreenViews(in);
    for (int eye = 0; eye < 2; ++eye) {
        xr_->AcquireEye(eye);
        RenderEye(in, eye);
        xr_->ReleaseEye(eye);
    }
}

bool VrApp::World3d() const {
    const DsScreens &s = game_->screens();
    return settings_.Enum<GamePicture>(SettingId::ScreenPicture) != GamePicture::Ds &&
           !game_->texturedWorld().vertices.empty() && !s.movie &&
           !s.topBack.empty();
}

void VrApp::RenderGiantScreenViews(const XrFrameInput &in) {
    // The game's own camera, rendered at headset resolution instead of 256x192. With stereo on,
    // each eye gets its own camera, so the level has depth behind the screen.
    const GameFrame &g = game_->frame();
    // Sharp: the game's 3D drawn again flat, one picture for both eyes, like the DS's.
    bool stereo = settings_.Bool(SettingId::ScreenStereo) &&
                  !(World3d() && settings_.Enum<GamePicture>(SettingId::ScreenPicture) == GamePicture::Sharp);
    float sep = kIpd * settings_.Get(SettingId::ScreenDepth) * settings_.Get(SettingId::GameUnitsPerMeter);
    float fovY = g.camera.fovYRad > 0 ? g.camera.fovYRad : DegToRad(55.0f);
    float ty = std::tan(fovY * 0.5f), tx = ty * 4.0f / 3.0f;
    const bool world3d = World3d();
    const float nearZ = g.camera.nearZ > 0 ? g.camera.nearZ * 0.5f : 0.1f;
    const float farZ = g.camera.farZ > 0 ? g.camera.farZ * 1.5f : 1000.0f;
    Mat4 proj = Mat4::Projection(-tx, tx, ty, -ty, nearZ, farZ);
    Vec3 f = Normalize(g.camera.target - g.camera.eye);
    Vec3 right = Normalize(Cross(f, g.camera.up));
    for (int eye = 0; eye < (stereo ? 2 : 1); ++eye) {
        float offset = stereo ? (eye == 0 ? -0.5f : 0.5f) * sep : 0.0f;
        Vec3 shift = right * offset;
        Mat4 view = LookAt(g.camera.eye + shift, g.camera.target + shift, g.camera.up);
        Mat4 eyeProj = proj;
        if (world3d && g.camera.hasMatrices) {
            // The game's own lens and camera, with the eye moved sideways in its view space.
            std::copy(g.camera.proj, g.camera.proj + 16, eyeProj.m);
            Mat4 cam;
            std::copy(g.camera.view, g.camera.view + 16, cam.m);
            Mat4 side = Mat4::Identity();
            side.at(0, 3) = -offset;
            view = side * cam;
            // The DS's viewport: the lens's picture lands in that part of the 256 x 192 screen.
            const int *vp = game_->texturedWorld().viewport;
            const float vw = (vp[2] - vp[0] + 1) / 256.0f, vh = (vp[3] - vp[1] + 1) / 192.0f;
            Mat4 place = Mat4::Identity();
            place.at(0, 0) = vw;
            place.at(1, 1) = vh;
            place.at(0, 3) = (vp[0] + (vp[2] - vp[0] + 1) * 0.5f) / 128.0f - 1.0f;
            place.at(1, 3) = (vp[1] + (vp[3] - vp[1] + 1) * 0.5f) / 96.0f - 1.0f;
            eyeProj = place * eyeProj;
        }
        giantViews_[eye].Ensure(kGiantViewW, kGiantViewH);
        giantViews_[eye].Bind();
        glClearColor(0.42f, 0.58f, 0.85f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (world3d) {
            // The DS's 2D behind the 3D (a painted sky, a backdrop) fills the view first.
            renderer_.DrawQuad(backTex_.id(), Pose{}, 2.0f, 2.0f, Mat4::Identity());
            glClear(GL_DEPTH_BUFFER_BIT);
            const TextureFilter filter = settings_.Enum<TextureFilter>(SettingId::PicTextureFilter);
            renderer_.DrawTexturedWorld(game_->texturedWorld(), eyeProj * view,
                                        filter == TextureFilter::Nearest || filter == TextureFilter::Sharp);
        } else {
            renderer_.DrawTriangles(game_->world(), proj * view);
            renderer_.DrawTriangles(game_->player(), proj * view);
        }
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

void VrApp::DrawHandheld(const Mat4 &vpTracking, float scale, float alpha, float smoothing) {
    // A generic clamshell in DS Lite proportions: two halves of 2.15u x 1.19u (u is a DS Lite
    // screen's width at this size), the lower one with a cross pad and four face buttons beside
    // the touch screen, the upper one on a hinge along the lower one's top front edge, opened to
    // Hinge angle. The screens are w wide (Screen size scales them from u).
    const float u = settings_.Get(SettingId::HandScreenDsSize) / 2.15f * scale;
    const float w = bottomPanel_.width * scale, h = w * 0.75f;
    const float bodyW = u * 2.15f, bodyH = u * 1.19f, thick = u * 0.12f;
    static const uint32_t kShell[] = {Rgba(28, 28, 32), Rgba(232, 232, 236), Rgba(170, 28, 36),
                                      Rgba(36, 64, 150), Rgba(170, 174, 184)};
    static const uint32_t kTrim[] = {Rgba(48, 48, 54), Rgba(200, 200, 206), Rgba(130, 20, 28),
                                     Rgba(26, 46, 112), Rgba(130, 134, 144)};
    const int color = std::clamp(settings_.Int(SettingId::HandScreenDsColor), 0, 4);
    const uint32_t shell = kShell[color], trim = kTrim[color];
    const uint32_t bezel = Rgba(12, 12, 14), button = color == 1 ? Rgba(150, 150, 158) : Rgba(70, 70, 78);

    WorldDrawList ds;
    auto box = [&ds](const Pose &frame, const Vec3 &center, const Vec3 &half, uint32_t c) {
        AddOrientedBox(&ds, frame.Transform(center), half, frame.orientation, c);
    };
    // Lower half: the touch screen's center is the pose's origin, its face at z = 0.
    const Pose &lower = bottomPanel_.pose;
    box(lower, {0, 0, -thick * 0.5f - 0.0012f}, {bodyW * 0.5f, bodyH * 0.5f, thick * 0.5f}, shell);
    box(lower, {0, 0, -0.0009f}, {w * 0.54f, h * 0.56f, 0.0003f}, bezel);
    const float side = (w + bodyW) * 0.25f;  // halfway from the screen's edge to the shell's
    const float padY = u * 0.135f, key = u * 0.028f;
    box(lower, {-side, padY, 0.0f}, {u * 0.085f, key, 0.0015f}, button);
    box(lower, {-side, padY, 0.0f}, {key, u * 0.085f, 0.0015f}, button);
    const float r = u * 0.06f;
    const Vec3 face[] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}};
    for (const Vec3 &o : face) box(lower, Vec3{side, padY, 0.0f} + o, {key, key, 0.0015f}, button);
    box(lower, {side, -u * 0.22f, 0.0f}, {u * 0.035f, u * 0.012f, 0.001f}, button);  // start
    box(lower, {side, -u * 0.31f, 0.0f}, {u * 0.035f, u * 0.012f, 0.001f}, button);  // select
    box(lower, {0, bodyH * 0.5f, -thick * 0.5f - 0.0012f}, {bodyW * 0.45f, thick * 0.45f, thick * 0.45f},
        trim);  // hinge

    // Upper half, in the hinge's frame: the axis along the lower half's top front edge, so closing
    // it (turning toward you) would lay the top screen face down on the touch screen.
    const float closed = DegToRad(180.0f - settings_.Get(SettingId::HandScreenDsHinge));
    const Pose upper = lower * Pose{Quat::AxisAngle({1, 0, 0}, closed), {0, bodyH * 0.5f + thick * 0.15f, -0.0012f}};
    box(upper, {0, bodyH * 0.5f, -thick * 0.5f}, {bodyW * 0.5f, bodyH * 0.5f, thick * 0.5f}, shell);
    box(upper, {0, bodyH * 0.52f, 0.0003f}, {w * 0.54f, h * 0.56f, 0.0003f}, bezel);
    for (float sx : {-side, side})  // speaker grilles
        box(upper, {sx, bodyH * 0.52f, 0.0002f}, {u * 0.05f, u * 0.05f, 0.0002f}, trim);

    renderer_.DrawTriangles(ds, vpTracking, alpha);
    const Pose topScreen = upper * Pose{Quat(), {0, bodyH * 0.52f, 0.0012f}};
    renderer_.SetScreenSmoothing(smoothing);
    renderer_.DrawQuad(topTex_.id(), topScreen, w, h, vpTracking, alpha);
    renderer_.DrawQuad(bottomTex_.id(), lower, w, h, vpTracking, alpha);
    renderer_.SetScreenSmoothing(0.0f);
}

void VrApp::RenderEye(const XrFrameInput &in, int eye) {
    const ViewMode mode = lastMode_;
    // DS screens: edge-preserving smoothing (Picture > Screen smoothing).
    const float screenSmoothing = settings_.Get(SettingId::PicScreenSmoothing);
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
        if (stationTex_.id()) {
            // The window lies face up on the pillar, its top toward where you first looked.
            Pose floor{Quat::Yaw(screenYaw_) * Quat::AxisAngle({1, 0, 0}, -kPi / 2),
                       loadingCenter_ + Vec3{0, 0.002f, 0}};
            renderer_.DrawQuad(stationTex_.id(), floor, 6.4f, 6.4f, vpTracking);
        }
        renderer_.DrawTriangles(loadingBeam_, vpTracking, 0.10f);
        // The motes face your eye, farthest first so their glows blend over each other.
        const Vec3 eyePos = in.eyes[eye].pose.position;
        std::vector<const Mote *> order;
        for (const Mote &m : motes_) order.push_back(&m);
        std::sort(order.begin(), order.end(), [&](const Mote *a, const Mote *b) {
            return Length(a->position - eyePos) > Length(b->position - eyePos);
        });
        glDepthMask(GL_FALSE);
        for (const Mote *m : order)
            if (m->alpha > 0.02f)
                renderer_.DrawQuad(glowTex_.id(), FacingFrom(m->position, eyePos), m->size, m->size, vpTracking,
                                   m->alpha);
        glDepthMask(GL_TRUE);
    } else if (mode == ViewMode::GiantScreen && handheld_ && (!settings_.Bool(SettingId::HandScreenDsGiant) || DsOnly())) {
        // The handheld shows the top screen instead.
    } else if (mode == ViewMode::GiantScreen) {
        float curv = settings_.Get(SettingId::ScreenCurvature);
        float dist = settings_.Get(SettingId::ScreenDistance);
        const bool world3d = World3d();
        if (BothScreens()) curv = 0.0f;  // flat, as one surface with the touch screen below
        if (GiantBottomShown(mode)) {
            renderer_.SetScreenSmoothing(screenSmoothing);
            renderer_.DrawQuad(bottomTex_.id(), giantBottom_.pose, giantBottom_.width, giantBottom_.height, vpTracking);
            renderer_.SetScreenSmoothing(0.0f);
        }
        if (game_->world().triangles.empty() && !world3d) {
            // The DS's top screen exactly as the game draws it.
            renderer_.SetScreenSmoothing(screenSmoothing);
            renderer_.DrawQuad(topTex_.id(), giantScreen_.pose, giantScreen_.width, giantScreen_.height,
                               vpTracking, 1.0f, curv, dist);
            renderer_.SetScreenSmoothing(0.0f);
        } else {
            const bool sharpFlat = world3d && settings_.Enum<GamePicture>(SettingId::ScreenPicture) == GamePicture::Sharp;
            int view = settings_.Bool(SettingId::ScreenStereo) && !sharpFlat ? eye : 0;
            renderer_.DrawQuad(giantViews_[view].texture(), giantScreen_.pose, giantScreen_.width,
                               giantScreen_.height, vpTracking, 1.0f, curv, dist, true);
            // The 2D HUD sits on the screen surface, a few millimeters in front.
            Pose hudOnScreen = giantScreen_.pose * Pose{Quat(), {0, 0, 0.005f}};
            renderer_.SetScreenSmoothing(screenSmoothing);
            renderer_.DrawQuad(world3d ? frontTex_.id() : hudTex_.id(), hudOnScreen, giantScreen_.width,
                               giantScreen_.height, vpTracking, 1.0f, curv, dist);
            renderer_.SetScreenSmoothing(0.0f);
        }
    } else {
        // The game world, through the rig's mapping from tracking space into the world.
        Mat4 worldToTracking = Mat4::FromSimilarity(rig_.trackingToWorld.Inverse());
        Mat4 vpWorld = vpTracking * worldToTracking;
        renderer_.DrawTriangles(game_->world(), vpWorld);
        if (!game_->texturedWorld().vertices.empty()) {
            // The game's own 3D, captured in world space (View > Stand in the world).
            const TextureFilter filter = settings_.Enum<TextureFilter>(SettingId::PicTextureFilter);
            // Sprites turn to face you: across your view, upright (the world's up stays up).
            WorldMotion motion;
            motion.inBetween = settings_.Bool(SettingId::PicInBetweenFrames);
            motion.sprites = true;
            const Vec3 f = rig_.trackingToWorld.rotation.Rotate(in.head.orientation.Forward());
            const Vec3 r{-f.z, 0.0f, f.x};
            motion.right = Length(r) > 0.05f ? Normalize(r)
                                             : rig_.trackingToWorld.rotation.Rotate(in.head.orientation.Rotate({1, 0, 0}));
            motion.up = Normalize(Cross(motion.right, f));
            renderer_.DrawTexturedWorld(game_->texturedWorld(), vpWorld,
                                        filter == TextureFilter::Nearest || filter == TextureFilter::Sharp, motion);
        }
        bool drawPlayer = mode == ViewMode::ThirdPerson ||
                          settings_.Enum<BodyDisplay>(SettingId::FpShowBody) == BodyDisplay::FullBody;
        if (drawPlayer) renderer_.DrawTriangles(game_->player(), vpWorld);
    }
    DrawProps(in, vpTracking);

    // Panels float in front of the world: never let a pillar, the floor or the character cut into
    // them. They still sort against each other and the laser.
    glClear(GL_DEPTH_BUFFER_BIT);
    if (mode != ViewMode::GiantScreen && (!handheld_ || (settings_.Bool(SettingId::HandScreenDsGiant) && !DsOnly())) &&
        settings_.Enum<TopScreenPlacement>(SettingId::HudTopScreen) != TopScreenPlacement::Hidden)
    {
        renderer_.SetScreenSmoothing(screenSmoothing);
        renderer_.DrawQuad(hudTex_.id(), hudPanel_.pose, hudPanel_.width, hudPanel_.height, vpTracking);
        renderer_.SetScreenSmoothing(0.0f);
    }
    {
        const BottomScreenPlacement placement = settings_.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement);
        const bool held = placement == BottomScreenPlacement::InHand || placement == BottomScreenPlacement::OnWrist;
        if (placement != BottomScreenPlacement::Hidden && (!held || in.gripValid[handScreenSide_])) {
            // Not looked at: small and see-through, so it is easy to find; looked at: grows to
            // full size and opacity with a smooth ease.
            float t = handScreenShown_, e = t * t * (3.0f - 2.0f * t);
            float scale = 0.45f + 0.55f * e, alpha = 0.55f + 0.45f * e;
            if (handheld_) {
                DrawHandheld(vpTracking, scale, alpha, screenSmoothing);
            } else {
                renderer_.SetScreenSmoothing(screenSmoothing);
                renderer_.DrawQuad(bottomTex_.id(), bottomPanel_.pose, bottomPanel_.width * scale,
                                   bottomPanel_.height * scale, vpTracking, alpha);
                renderer_.SetScreenSmoothing(0.0f);
            }
        }
    }
    if (Loading() && loadingTex_.id())
        renderer_.DrawQuad(loadingTex_.id(), loadingPanel_.pose, loadingPanel_.width, loadingPanel_.height, vpTracking);
    if (statusVisible_ && statusTex_.id())
        renderer_.DrawQuad(statusTex_.id(), statusPanel_.pose, statusPanel_.width, statusPanel_.height, vpTracking);
    if (panel_.isOpen())
        renderer_.DrawQuad(panelTex_.id(), settingsPanel_.pose, settingsPanel_.width,
                           settingsPanel_.height, vpTracking);
    if (laserOnPanel_ || laserOnBottom_ || panel_.isOpen())
        renderer_.DrawLine(in.aim[laserHand_].position, laserEnd_, Rgba(214, 178, 82, 220), vpTracking);

    float blink = 0.0f;
    if (blinkTime_ >= 0.0f) {
        float len = settings_.Get(SettingId::ComfortBlinkMs) / 1000.0f;
        float x = blinkTime_ / len;  // 0..1: fade out, hold, fade in
        blink = x < 0.3f ? x / 0.3f : (x > 0.7f ? (1.0f - x) / 0.3f : 1.0f);
    }
    renderer_.DrawComfortOverlay(mode == ViewMode::GiantScreen ? 0.0f : rig_.vignette, blink);
}

}  // namespace khvr
