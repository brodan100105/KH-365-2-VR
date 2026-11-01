#include "khvr/held_weapon.h"

#include <algorithm>
#include <cmath>

namespace khvr {

Vec3 WeaponFrame::ToHand(const Vec3 &p) const {
    const Vec3 d = p - grip;
    // Right-handed: +X the side, +Y side x blade, -Z the blade.
    const Vec3 up = Cross(side, blade);
    const float s = length > 1e-6f ? 1.0f / length : 0.0f;
    return Vec3{Dot(d, side), Dot(d, up), -Dot(d, blade)} * s;
}

WeaponFrame FindWeaponFrame(const std::vector<Vec3> &points) {
    WeaponFrame f;
    if (points.size() < 3) return f;
    // The blade: between the two points farthest apart (from the first, then from that one),
    // wherever the model's own origin is.
    auto farthest = [&](const Vec3 &from) {
        Vec3 best = from;
        for (const Vec3 &p : points)
            if (Dot(p - from, p - from) > Dot(best - from, best - from)) best = p;
        return best;
    };
    Vec3 a = farthest(points[0]), b = farthest(a);
    const float total = Length(b - a);
    if (total < 1e-4f) return f;
    // The teeth: the point farthest off that line. They are at the tip, so the end nearer them is
    // the tip and the other the pommel.
    const Vec3 axis = (b - a) / total;
    Vec3 off{}, teeth = a;
    for (const Vec3 &p : points) {
        const Vec3 d = p - a;
        const Vec3 across = d - axis * Dot(d, axis);
        if (Dot(across, across) > Dot(off, off)) {
            off = across;
            teeth = p;
        }
    }
    if (Dot(teeth - a, axis) < 0.5f * total) std::swap(a, b);
    f.blade = Normalize(b - a);
    // The handle: inside the guard, a fifth of the way from the pommel's end (past the chain that
    // hangs from it in the Kingdom Key).
    f.grip = a + f.blade * (0.2f * total);
    f.length = total * 0.8f;
    if (Length(off) < 1e-5f) {
        // A plain rod: any way across will do.
        off = Cross(f.blade, std::fabs(f.blade.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    }
    f.side = Normalize(off - f.blade * Dot(off, f.blade));
    f.valid = f.length > 1e-4f;
    return f;
}

Vec3 HandFrame::ToHand(const Vec3 &p) const {
    const Vec3 d = p - centre;
    const Vec3 across = Cross(palm, fingers);
    const float s = length > 1e-6f ? 1.0f / length : 0.0f;
    return Vec3{Dot(d, palm), Dot(d, fingers), Dot(d, across)} * s;
}

HandFrame FindHandFrame(const std::vector<Vec3> &points) {
    HandFrame f;
    if (points.size() < 3) return f;
    Vec3 c{0, 0, 0};
    for (const Vec3 &p : points) c += p;
    c = c * (1.0f / static_cast<float>(points.size()));
    // From the wrist (the joint's origin) out to the middle of the hand; with the middle on the
    // wrist, out to the point farthest from it.
    Vec3 out = c;
    if (Length(out) < 1e-4f)
        for (const Vec3 &p : points)
            if (Dot(p, p) > Dot(out, out)) out = p;
    if (Length(out) < 1e-4f) return f;
    f.fingers = Normalize(out);
    // Across: the thinnest way of the spread about the middle, square to the fingers' way.
    Vec3 u = Cross(f.fingers, std::fabs(f.fingers.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
    u = Normalize(u);
    const Vec3 v = Cross(f.fingers, u);
    double uu = 0, uv = 0, vv = 0;
    float lo = 1e30f, hi = -1e30f;
    for (const Vec3 &p : points) {
        const Vec3 d = p - c;
        const double a = Dot(d, u), b = Dot(d, v);
        uu += a * a;
        uv += a * b;
        vv += b * b;
        lo = std::min(lo, Dot(p, f.fingers));
        hi = std::max(hi, Dot(p, f.fingers));
    }
    // The smaller eigenvalue's axis of [[uu uv] [uv vv]].
    const double theta = 0.5 * std::atan2(2 * uv, uu - vv);  // the larger's angle from u
    const float cs = static_cast<float>(std::cos(theta)), sn = static_cast<float>(std::sin(theta));
    f.palm = Normalize(u * -sn + v * cs);
    // One of its two ways, always the same for the same model: its largest part positive.
    const float ax = std::fabs(f.palm.x), ay = std::fabs(f.palm.y), az = std::fabs(f.palm.z);
    const float big = ax >= ay && ax >= az ? f.palm.x : ay >= az ? f.palm.y : f.palm.z;
    if (big < 0) f.palm = f.palm * -1.0f;
    f.centre = c;
    f.length = hi - lo;
    f.valid = f.length > 1e-4f;
    return f;
}

}  // namespace khvr
