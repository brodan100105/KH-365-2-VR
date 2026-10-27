// The loading area's stained-glass floor, painted procedurally: an original design in the style
// of the series' Station of Awakening windows (orange glass, a band of red petals, a ring of
// medallions, a Twilight Town sunset with a clock tower, a heart-shaped moon and a key in the
// middle). Plain C++ with no GL, so the desktop can render a preview of it.
#pragma once

#include <cstdint>
#include <vector>

namespace khvr {

// `size` x `size` RGBA pixels (0xAABBGGRR), row 0 at the top; transparent outside the disc.
// The top of the image is the side in front of you.
std::vector<uint32_t> PaintStationArt(int size);
// A mote of light: a soft round glow, white-gold at the centre, fading to transparent, with a
// faint four-pointed glint. `size` x `size` RGBA.
std::vector<uint32_t> PaintGlow(int size);

}  // namespace khvr
