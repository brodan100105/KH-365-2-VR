// The Keyblade's effects in first person, drawn from where your blade really goes (not Roxas's
// attack animation): a trail behind a fast swing, and a glint that runs up the blade as it
// appears in your hand. Our own procedural effects: no game art.
#pragma once

#include <deque>

#include "khvr/game_source.h"
#include "khvr/math.h"

namespace khvr {

class SwingTrail {
public:
    // The tip must move at least this fast (m/s) between two samples for that stretch of the
    // swing to leave a trail: turning the wrist or carrying the blade leaves none.
    static constexpr float kMinTipSpeed = 2.2f;

    void Reset() { samples_.clear(); }
    // Each rendered frame: the blade's base and tip in tracking space, whether it is out.
    void Update(const Vec3 &base, const Vec3 &tip, bool out, double time);
    // The trail now: a ribbon between the past blade positions over the last lifeSeconds,
    // brightest at the tip and newest, fading toward the hilt and with age. Both sides.
    void Build(WorldDrawList *out, double time, float lifeSeconds, uint32_t rgb = 0xfff0d8u) const;
    size_t samples() const { return samples_.size(); }

private:
    struct Sample {
        Vec3 base, tip;
        double time;
        bool fast;  // the tip moved fast from the previous sample to this one
    };
    std::deque<Sample> samples_;
};

// The summoning effect as the Keyblade appears (t from 0 to 1): a band wide enough to envelop it
// sweeping from below the hilt to past the tip, then a glow round the whole blade, brightest as
// the Keyblade fades in (about 70 %), facing the eye.
void BuildSummonGlint(WorldDrawList *out, const Vec3 &base, const Vec3 &tip, const Vec3 &eye, float t);

}  // namespace khvr
