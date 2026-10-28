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
    // The blade: from the origin to the farthest point.
    Vec3 far{};
    for (const Vec3 &p : points)
        if (Dot(p, p) > Dot(far, far)) far = p;
    if (Length(far) < 1e-4f) return f;
    f.blade = Normalize(far);
    // Its extent along that way: an origin well inside it (a model centred on itself) is no grip.
    float lo = 0.0f, hi = 0.0f;
    for (const Vec3 &p : points) {
        lo = std::min(lo, Dot(p, f.blade));
        hi = std::max(hi, Dot(p, f.blade));
    }
    const float total = hi - lo;
    f.grip = {};
    if (-lo > 0.2f * total) f.grip = f.blade * (lo + 0.12f * total);  // the handle: a hand in from the end
    f.length = hi - Dot(f.grip, f.blade);
    // The teeth: the point farthest off the blade's line.
    Vec3 off{};
    for (const Vec3 &p : points) {
        const Vec3 d = p - f.grip;
        const Vec3 across = d - f.blade * Dot(d, f.blade);
        if (Dot(across, across) > Dot(off, off)) off = across;
    }
    if (Length(off) < 1e-5f) {
        // A plain rod: any way across will do.
        off = Cross(f.blade, std::fabs(f.blade.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    }
    f.side = Normalize(off);
    f.valid = f.length > 1e-4f;
    return f;
}

}  // namespace khvr
