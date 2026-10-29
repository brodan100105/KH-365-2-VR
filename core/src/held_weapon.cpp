#include "khvr/held_weapon.h"

#include <algorithm>

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

}  // namespace khvr
