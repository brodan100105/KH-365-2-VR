#include "khvr/upscale.h"

namespace khvr {

void Scale2x(const uint32_t *src, int w, int h, uint32_t *dst) {
    const int dw = 2 * w;
    for (int y = 0; y < h; ++y) {
        const uint32_t *row = src + y * w;
        const uint32_t *up = y > 0 ? row - w : row;          // edges repeat the border pixel
        const uint32_t *down = y + 1 < h ? row + w : row;
        uint32_t *out0 = dst + (2 * y) * dw, *out1 = out0 + dw;
        for (int x = 0; x < w; ++x) {
            //   B        E0 E1
            // D E F  ->  E2 E3
            //   H
            const uint32_t e = row[x], b = up[x], h2 = down[x];
            const uint32_t d = row[x > 0 ? x - 1 : x], f = row[x + 1 < w ? x + 1 : x];
            uint32_t e0 = e, e1 = e, e2 = e, e3 = e;
            if (b != h2 && d != f) {
                if (d == b) e0 = d;
                if (b == f) e1 = f;
                if (d == h2) e2 = d;
                if (h2 == f) e3 = f;
            }
            out0[2 * x] = e0;
            out0[2 * x + 1] = e1;
            out1[2 * x] = e2;
            out1[2 * x + 1] = e3;
        }
    }
}

void Scale4x(const uint32_t *src, int w, int h, uint32_t *scratch, uint32_t *dst) {
    Scale2x(src, w, h, scratch);
    Scale2x(scratch, 2 * w, 2 * h, dst);
}

}  // namespace khvr
