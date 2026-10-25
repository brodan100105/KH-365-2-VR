// Pixel-art enlargement for the DS screens on the big VR panels: Scale2x (also known as EPX or
// AdvMAME2x), which doubles each pixel and, where two neighbours meet on a diagonal, fills the
// corner with their colour instead of the pixel's own, so edges come out smoothed rather than as
// staircases. Applied twice it gives Scale4x. Exact colour comparisons, so flat-shaded art gains
// the most; photographic content (movies) passes through unchanged as plain doubled pixels.
#pragma once

#include <cstdint>

namespace khvr {

// `dst` is (2 w) x (2 h) pixels. Pixels are compared whole (RGBA).
void Scale2x(const uint32_t *src, int w, int h, uint32_t *dst);

// `dst` is (4 w) x (4 h); `scratch` holds (2 w) x (2 h).
void Scale4x(const uint32_t *src, int w, int h, uint32_t *scratch, uint32_t *dst);

}  // namespace khvr
