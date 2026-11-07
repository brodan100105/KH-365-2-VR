#include "khvr/swing_trail.h"

#include <algorithm>
#include <cmath>

namespace khvr {

namespace {

// The lit shader shades by the normal: one along its light makes the colour full brightness.
const Vec3 kFacingLight = Normalize(Vec3{0.4f, 0.8f, 0.45f});

uint32_t WithAlpha(uint32_t rgb, float a) {
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (rgb & 0xffffffu) | (alpha << 24);
}

// A quad as two triangles, each side (the shader culls back faces).
void Quad(WorldDrawList *out, const Vec3 p[4], const uint32_t c[4]) {
    auto v = [&](int i) { return WorldVertex{p[i], kFacingLight, c[i]}; };
    const int front[6] = {0, 1, 2, 0, 2, 3}, back[6] = {0, 2, 1, 0, 3, 2};
    for (int i : front) out->triangles.push_back(v(i));
    for (int i : back) out->triangles.push_back(v(i));
}

}  // namespace

void SwingTrail::Update(const Vec3 &base, const Vec3 &tip, bool out, double time) {
    if (!out) {
        samples_.clear();
        return;
    }
    bool fast = false;
    if (!samples_.empty()) {
        const Sample &last = samples_.back();
        const double dt = time - last.time;
        if (dt <= 0.0) return;
        fast = Length(tip - last.tip) / static_cast<float>(dt) >= kMinTipSpeed;
    }
    samples_.push_back({base, tip, time, fast});
    // Half a second is more than any trail lasts.
    while (!samples_.empty() && time - samples_.front().time > 0.5) samples_.pop_front();
}

void SwingTrail::Build(WorldDrawList *out, double time, float lifeSeconds, uint32_t rgb) const {
    if (lifeSeconds <= 0.0f) return;
    // From a third of the way up the blade to its tip: the hilt end stays clear.
    constexpr float kInner = 0.35f;
    for (size_t i = 1; i < samples_.size(); ++i) {
        const Sample &a = samples_[i - 1], &b = samples_[i];
        if (!b.fast) continue;
        const float ageA = static_cast<float>(time - a.time) / lifeSeconds;
        const float ageB = static_cast<float>(time - b.time) / lifeSeconds;
        if (ageB >= 1.0f) continue;
        auto fade = [](float age) {
            const float f = 1.0f - std::clamp(age, 0.0f, 1.0f);
            return f * f;
        };
        const float fa = fade(ageA) * 0.85f, fb = fade(ageB) * 0.85f;
        const Vec3 innerA = a.base + (a.tip - a.base) * kInner, innerB = b.base + (b.tip - b.base) * kInner;
        const Vec3 p[4] = {innerA, a.tip, b.tip, innerB};
        const uint32_t c[4] = {WithAlpha(rgb, 0.0f), WithAlpha(rgb, fa), WithAlpha(rgb, fb), WithAlpha(rgb, 0.0f)};
        Quad(out, p, c);
    }
}

void BuildSummonGlint(WorldDrawList *out, const Vec3 &base, const Vec3 &tip, const Vec3 &eye, float t) {
    if (t < 0.0f || t >= 1.0f) return;
    const Vec3 along = tip - base;
    const float length = Length(along);
    if (length < 1e-4f) return;
    const Vec3 axis = along * (1.0f / length);
    // Across the blade, square to the line of sight.
    Vec3 across = Cross(axis, eye - (base + tip) * 0.5f);
    if (Length(across) < 1e-5f) return;
    across = Normalize(across);
    const float halfWidth = 0.035f, halfBand = 0.09f * length;
    const float centre = t * length;
    const float lo = std::max(0.0f, centre - halfBand), hi = std::min(length, centre + halfBand);
    if (hi <= lo) return;
    const float strength = 1.0f - t * t;  // bright, then gone by the tip
    const Vec3 mid = base + axis * centre;
    const Vec3 p0 = base + axis * lo, p1 = base + axis * hi;
    const uint32_t white = 0xfffff4u;
    // Two quads: from the band's ends to its bright middle.
    const Vec3 lower[4] = {p0 - across * halfWidth, p0 + across * halfWidth, mid + across * halfWidth, mid - across * halfWidth};
    const uint32_t lowerC[4] = {WithAlpha(white, 0), WithAlpha(white, 0), WithAlpha(white, strength), WithAlpha(white, strength)};
    Quad(out, lower, lowerC);
    const Vec3 upper[4] = {mid - across * halfWidth, mid + across * halfWidth, p1 + across * halfWidth, p1 - across * halfWidth};
    const uint32_t upperC[4] = {WithAlpha(white, strength), WithAlpha(white, strength), WithAlpha(white, 0), WithAlpha(white, 0)};
    Quad(out, upper, upperC);
}

}  // namespace khvr
