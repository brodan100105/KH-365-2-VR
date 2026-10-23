#include "khvr/demo_game.h"

#include <cmath>
#include <cstdio>

namespace khvr {

namespace {

constexpr float kMoveSpeed = 5.0f;     // units/s
constexpr float kDodgeSpeed = 11.0f;
constexpr float kJumpSpeed = 5.5f;
constexpr float kGravity = 16.0f;
constexpr float kPlayerRadius = 0.35f;
constexpr float kAttackReach = 2.2f;
constexpr float kAttackHalfAngle = 1.2f;  // radians either side of facing
const Vec3 kPillars[4] = {{-6, 0, -6}, {6, 0, -6}, {-6, 0, 6}, {6, 0, 6}};
constexpr float kPillarRadius = 1.2f;

const uint32_t kCoat = Rgba(24, 24, 30);
const uint32_t kSkin = Rgba(236, 200, 170);
const uint32_t kHair = Rgba(214, 170, 92);
const uint32_t kShadow = Rgba(40, 30, 70);
const uint32_t kShadowEyes = Rgba(250, 230, 60);

uint32_t Shade(uint32_t c, float k) {
    auto ch = [&](int s) {
        return static_cast<uint32_t>(Clamp(((c >> s) & 0xff) * k, 0.0f, 255.0f)) << s;
    };
    return ch(0) | ch(8) | ch(16) | (c & 0xff000000u);
}

}  // namespace

void AddQuad(WorldDrawList *l, const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d,
             uint32_t color) {
    Vec3 n = Normalize(Cross(b - a, c - a));
    for (const Vec3 &p : {a, b, c, a, c, d}) l->triangles.push_back({p, n, color});
}

void AddBox(WorldDrawList *l, const Vec3 &center, const Vec3 &h, float yaw, uint32_t color) {
    AddOrientedBox(l, center, h, Quat::Yaw(yaw), color);
}

void AddOrientedBox(WorldDrawList *l, const Vec3 &center, const Vec3 &h, const Quat &q,
                    uint32_t color) {
    auto P = [&](float x, float y, float z) { return center + q.Rotate({x * h.x, y * h.y, z * h.z}); };
    AddQuad(l, P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), color);       // +Z
    AddQuad(l, P(1, -1, -1), P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), color);   // -Z
    AddQuad(l, P(1, -1, 1), P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), color);       // +X
    AddQuad(l, P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1), color);   // -X
    AddQuad(l, P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1), P(-1, 1, -1), color);       // +Y
    AddQuad(l, P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), color);   // -Y
}

DemoGame::DemoGame()
    : top_(DsScreens::kWidth, DsScreens::kHeight),
      topHud_(DsScreens::kWidth, DsScreens::kHeight),
      bottom_(DsScreens::kWidth, DsScreens::kHeight) {
    frame_.inGameplay = true;
    frame_.playerPos = {0, 0, 4};
    frame_.playerHeight = 1.6f;
    frame_.camera.eye = {0, 3, 10};
    frame_.camera.target = {0, 1, 4};
    for (int i = 0; i < 3; ++i) dummies_[i].pos = {-4.0f + 4.0f * i, 0, -3};
    BuildStatic();
    BuildDynamic();
    DrawScreens();
}

void DemoGame::BuildStatic() {
    // Checkered floor in 2-unit tiles (Twilight Town's plaza colors), walls, four pillars.
    for (int z = -7; z < 7; ++z)
        for (int x = -7; x < 7; ++x) {
            float x0 = x * 2.0f, z0 = z * 2.0f;
            uint32_t c = ((x + z) & 1) ? Rgba(176, 120, 84) : Rgba(204, 160, 110);
            AddQuad(&static_, {x0, 0, z0 + 2}, {x0 + 2, 0, z0 + 2}, {x0 + 2, 0, z0}, {x0, 0, z0}, c);
        }
    const float H = 3.0f, A = kArenaHalf;
    uint32_t wall = Rgba(150, 96, 70);
    AddBox(&static_, {0, H / 2, -A - 0.25f}, {A + 0.5f, H / 2, 0.25f}, 0, wall);
    AddBox(&static_, {0, H / 2, A + 0.25f}, {A + 0.5f, H / 2, 0.25f}, 0, wall);
    AddBox(&static_, {-A - 0.25f, H / 2, 0}, {0.25f, H / 2, A}, 0, Shade(wall, 0.9f));
    AddBox(&static_, {A + 0.25f, H / 2, 0}, {0.25f, H / 2, A}, 0, Shade(wall, 0.9f));
    for (const Vec3 &p : kPillars) {
        AddBox(&static_, p + Vec3{0, 2.5f, 0}, {0.85f, 2.5f, 0.85f}, 0.785f, Rgba(120, 130, 150));
        AddBox(&static_, p + Vec3{0, 5.1f, 0}, {1.1f, 0.1f, 1.1f}, 0.785f, kHair);
    }
    // The clock tower in the distance, for a sense of scale and direction.
    AddBox(&static_, {0, 9, -A - 6}, {3, 9, 3}, 0, Rgba(196, 150, 110));
    AddBox(&static_, {0, 14, -A - 2.9f}, {1.6f, 1.6f, 0.05f}, 0, Rgba(240, 236, 220));
}

void DemoGame::BuildDynamic() {
    world_ = static_;
    for (const Dummy &d : dummies_) {
        if (d.respawn > 0) continue;
        uint32_t body = d.flash > 0 ? Rgba(255, 255, 255) : kShadow;
        AddBox(&world_, d.pos + Vec3{0, 0.45f, 0}, {0.35f, 0.45f, 0.3f}, d.yaw, body);
        AddBox(&world_, d.pos + Vec3{0, 1.05f, 0}, {0.3f, 0.2f, 0.28f}, d.yaw, body);
        Vec3 fwd = Quat::Yaw(d.yaw).Forward();
        Vec3 right = Quat::Yaw(d.yaw).Right();
        for (float s : {-1.0f, 1.0f})
            AddBox(&world_, d.pos + Vec3{0, 1.08f, 0} + fwd * 0.29f + right * (0.12f * s),
                   {0.06f, 0.04f, 0.01f}, d.yaw, kShadowEyes);
        // Antennae.
        for (float s : {-1.0f, 1.0f})
            AddBox(&world_, d.pos + Vec3{0, 1.45f, 0} + right * (0.18f * s), {0.04f, 0.22f, 0.04f},
                   d.yaw, body);
    }

    player_.Clear();
    const Vec3 p = frame_.playerPos;
    const float yaw = frame_.playerYaw;
    Quat q = Quat::Yaw(yaw);
    uint32_t coat = guardTime_ > 0 ? Rgba(60, 90, 200) : kCoat;
    AddBox(&player_, p + Vec3{0, 0.45f, 0}, {0.22f, 0.45f, 0.16f}, yaw, coat);  // legs and coat tail
    AddBox(&player_, p + Vec3{0, 1.1f, 0}, {0.26f, 0.25f, 0.17f}, yaw, coat);    // torso
    AddBox(&player_, p + Vec3{0, 1.5f, 0}, {0.13f, 0.14f, 0.13f}, yaw, kSkin);   // head
    AddBox(&player_, p + Vec3{0, 1.66f, 0} + q.Rotate({0, 0, 0.03f}), {0.16f, 0.06f, 0.16f}, yaw,
           kHair);
    // The Keyblade in the right hand, raised while attacking.
    float swing = attackTime_ > 0 ? attackTime_ / 0.3f : 0.0f;
    Vec3 hand = p + q.Rotate({0.34f, 1.0f, -0.1f});
    float pitch = -0.4f - 1.6f * swing;
    Vec3 blade = q.Rotate(Quat::AxisAngle({1, 0, 0}, pitch).Rotate({0, 0, -1}));
    for (int i = 0; i < 6; ++i)
        AddBox(&player_, hand + blade * (0.12f * i + 0.06f), {0.03f, 0.03f, 0.06f}, yaw,
               i == 0 ? Rgba(220, 200, 80) : Rgba(200, 200, 215));
}

void DemoGame::Collide(Vec3 *pos, float r) const {
    float lim = kArenaHalf - r;
    pos->x = Clamp(pos->x, -lim, lim);
    pos->z = Clamp(pos->z, -lim, lim);
    for (const Vec3 &c : kPillars) {
        Vec3 d{pos->x - c.x, 0, pos->z - c.z};
        float len = Length(d);
        if (len < kPillarRadius + r) {
            Vec3 n = len > 1e-4f ? d / len : Vec3{1, 0, 0};
            pos->x = c.x + n.x * (kPillarRadius + r);
            pos->z = c.z + n.z * (kPillarRadius + r);
        }
    }
}

void DemoGame::Advance(double now, const GameRequests &requests,
                       uint16_t (*sampleButtons)(void *), void *ctx) {
    if (time_ < 0) time_ = now;
    int steps = 0;
    while (time_ + kFrameSeconds <= now && steps < 4) {
        Step(requests, sampleButtons ? sampleButtons(ctx) : requests.buttons);
        time_ += kFrameSeconds;
        ++steps;
    }
    if (time_ + kFrameSeconds <= now) time_ = now;  // fell far behind (paused): do not catch up
    // Between game frames the VR layer still overrides the camera.
    if (requests.overrideCamera) frame_.camera = requests.camera;
}

void DemoGame::Step(const GameRequests &req, uint16_t buttons) {
    const float dt = static_cast<float>(kFrameSeconds);
    const uint16_t trig = buttons & ~prevButtons_;
    prevButtons_ = buttons;
    GameCamera cam = req.overrideCamera ? req.camera : frame_.camera;
    float camYaw = YawOf(Vec3{cam.target.x - cam.eye.x, 0, cam.target.z - cam.eye.z});

    // ---- Movement: analog from the VR layer, else the D-pad relative to the camera ----
    Vec3 move;
    if (req.hasAnalogMove) {
        move = req.analogMoveWorld;
    } else {
        float x = ((buttons & kDsRight) ? 1.0f : 0.0f) - ((buttons & kDsLeft) ? 1.0f : 0.0f);
        float y = ((buttons & kDsUp) ? 1.0f : 0.0f) - ((buttons & kDsDown) ? 1.0f : 0.0f);
        if (x != 0 || y != 0) move = Quat::Yaw(camYaw).Rotate(Normalize(Vec3{x, 0, -y}));
    }
    bool moving = Length(move) > 0.05f;

    // ---- Actions ----
    attackTime_ = std::fmax(0.0f, attackTime_ - dt);
    dodgeTime_ = std::fmax(0.0f, dodgeTime_ - dt);
    guardTime_ = (buttons & kDsY) && !moving && dodgeTime_ <= 0 ? 0.1f : 0.0f;
    if ((trig & kDsY) && moving && dodgeTime_ <= 0) {
        dodgeTime_ = 0.35f;
        dodgeDir_ = Normalize(move);
    }
    if ((trig & kDsB) && frame_.playerGrounded) {
        velocity_.y = kJumpSpeed;
        frame_.playerGrounded = false;
    }

    if (req.setPlayerYaw && attackTime_ <= 0 && dodgeTime_ <= 0) frame_.playerYaw = req.playerYaw;
    else if (moving && !req.setPlayerYaw) frame_.playerYaw = YawOf(move);

    if ((trig & kDsA) && attackTime_ <= 0.15f) {
        attackTime_ = 0.3f;
        Vec3 facing = Quat::Yaw(frame_.playerYaw).Forward();
        for (Dummy &d : dummies_) {
            if (d.respawn > 0) continue;
            Vec3 to = d.pos - frame_.playerPos;
            to.y = 0;
            float dist = Length(to);
            if (dist > kAttackReach) continue;
            float ang = std::acos(Clamp(Dot(Normalize(to), facing), -1.0f, 1.0f));
            if (dist > 0.6f && ang > kAttackHalfAngle) continue;
            d.flash = 0.15f;
            d.knockback = Normalize(to) * 6.0f;
            ++hits_;
            if (--d.hp <= 0) d.respawn = 3.0f;
        }
    }

    // ---- Integrate ----
    Vec3 pos = frame_.playerPos;
    if (dodgeTime_ > 0) pos += dodgeDir_ * (kDodgeSpeed * dt);
    else pos += move * (kMoveSpeed * dt);
    if (req.hasRoomScaleStep) pos += Vec3{req.roomScaleStep.x, 0, req.roomScaleStep.z};
    if (!frame_.playerGrounded) {
        velocity_.y -= kGravity * dt;
        pos.y += velocity_.y * dt;
        if (pos.y <= 0) {
            pos.y = 0;
            velocity_.y = 0;
            frame_.playerGrounded = true;
        }
    }
    Collide(&pos, kPlayerRadius);
    frame_.playerPos = pos;

    // ---- Dummies: drift toward the player, stop short, get knocked back ----
    std::uniform_real_distribution<float> spot(-kArenaHalf + 2, kArenaHalf - 2);
    frame_.hasNearestEnemy = false;
    float nearest = 1e9f;
    for (Dummy &d : dummies_) {
        d.flash = std::fmax(0.0f, d.flash - dt);
        if (d.respawn > 0) {
            d.respawn -= dt;
            if (d.respawn <= 0) {
                d = Dummy();
                d.pos = {spot(rng_), 0, spot(rng_)};
            }
            continue;
        }
        Vec3 to = frame_.playerPos - d.pos;
        to.y = 0;
        float dist = Length(to);
        if (dist > 0.01f) d.yaw = YawOf(to);
        if (dist < 8.0f && dist > 1.4f) d.pos += Normalize(to) * (1.2f * dt);
        d.pos += d.knockback * dt;
        d.knockback *= 0.8f;
        Collide(&d.pos, 0.4f);
        if (dist < nearest) {
            nearest = dist;
            frame_.hasNearestEnemy = true;
            frame_.nearestEnemy = d.pos + Vec3{0, 0.8f, 0};
        }
    }

    // ---- The game's own chase camera ----
    if (req.overrideCamera) {
        frame_.camera = req.camera;
    } else {
        Vec3 back = Quat::Yaw(frame_.playerYaw).Forward() * -6.0f;
        Vec3 eye = frame_.playerPos + back + Vec3{0, 3, 0};
        frame_.camera.eye = Lerp(frame_.camera.eye, eye, moving ? 0.08f : 0.03f);
        frame_.camera.target = frame_.playerPos + Vec3{0, 1, 0};
    }
    frame_.frameCounter++;
    BuildDynamic();
    DrawScreens();
}

void DemoGame::DrawScreens() {
    const int W = DsScreens::kWidth, H = DsScreens::kHeight;
    char buf[64];
    // Top screen HUD layer: HP gauge bottom right, hit counter top left, the way the game
    // places its gauges. Everything else stays transparent.
    topHud_.Clear(0);
    topHud_.FillRect(W - 110, H - 26, 100, 14, Rgba(0, 0, 0, 160));
    topHud_.FillRect(W - 108, H - 24, 96 * hp_ / 30, 10, Rgba(90, 220, 90));
    std::snprintf(buf, sizeof buf, "HITS %d", hits_);
    topHud_.DrawText(8, 6, buf, Rgba(255, 255, 255));
    topHud_.DrawText(8, H - 26, "DEMO ARENA", Rgba(214, 178, 82));

    // Top screen as the giant screen shows it when no 3D view is rendered behind it.
    top_.Clear(Rgba(30, 40, 80));
    for (int y = 0; y < H / 2; ++y)
        top_.FillRect(0, y, W, 1, Rgba(60 + y, 90 + y / 2, 160, 255));
    top_.DrawText(W / 2 - Canvas::TextWidth("358/2 DAYS VR") / 2, 70, "358/2 DAYS VR", Rgba(255, 255, 255));
    top_.DrawText(W / 2 - Canvas::TextWidth("demo arena") / 2, 96, "demo arena", Rgba(214, 178, 82));
    for (size_t i = 0; i < top_.pixels().size(); ++i) {
        uint32_t hud = topHud_.pixels()[i];
        if (hud >> 24) top_.pixels()[i] = hud | 0xff000000u;
    }

    // Bottom screen: a map, as the game shows on the touch screen.
    bottom_.Clear(Rgba(20, 24, 40));
    const float s = (H - 20) / (2 * kArenaHalf);
    auto mx = [&](float x) { return static_cast<int>(W / 2 + x * s); };
    auto my = [&](float z) { return static_cast<int>(H / 2 + z * s); };
    bottom_.StrokeRect(mx(-kArenaHalf), my(-kArenaHalf), static_cast<int>(2 * kArenaHalf * s),
                       static_cast<int>(2 * kArenaHalf * s), Rgba(214, 178, 82), 2);
    for (const Vec3 &p : kPillars)
        bottom_.FillCircle(mx(p.x), my(p.z), static_cast<int>(kPillarRadius * s), Rgba(120, 130, 150));
    for (const Dummy &d : dummies_)
        if (d.respawn <= 0) bottom_.FillCircle(mx(d.pos.x), my(d.pos.z), 4, Rgba(200, 60, 200));
    bottom_.FillCircle(mx(frame_.playerPos.x), my(frame_.playerPos.z), 5, Rgba(255, 255, 255));
    Vec3 f = Quat::Yaw(frame_.playerYaw).Forward();
    bottom_.FillCircle(mx(frame_.playerPos.x + f.x * 1.2f), my(frame_.playerPos.z + f.z * 1.2f), 2,
                       Rgba(255, 220, 80));

    screens_.top = top_.pixels();
    screens_.topHud = topHud_.pixels();
    screens_.bottom = bottom_.pixels();
    screens_.revision++;
}

}  // namespace khvr
