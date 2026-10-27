#include "khvr/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "font_data.h"

namespace khvr {

void Canvas::Clear(uint32_t color) { std::fill(pixels_.begin(), pixels_.end(), color); }

void Canvas::Blend(int x, int y, uint32_t color, uint8_t coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || coverage == 0) return;
    uint32_t &dst = pixels_[static_cast<size_t>(y) * width_ + x];
    uint32_t a = ((color >> 24) * coverage) / 255;
    if (a == 255) {
        dst = color;
        return;
    }
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t s = (color >> shift) & 0xff, d = (dst >> shift) & 0xff;
        out |= ((s * a + d * (255 - a)) / 255) << shift;
    }
    uint32_t da = dst >> 24;
    out |= (a + da * (255 - a) / 255) << 24;
    dst = out;
}

void Canvas::FillRect(int x, int y, int w, int h, uint32_t color) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) Blend(xx, yy, color, 255);
}

void Canvas::StrokeRect(int x, int y, int w, int h, uint32_t color, int t) {
    FillRect(x, y, w, t, color);
    FillRect(x, y + h - t, w, t, color);
    FillRect(x, y + t, t, h - 2 * t, color);
    FillRect(x + w - t, y + t, t, h - 2 * t, color);
}

void Canvas::FillCircle(int cx, int cy, int r, uint32_t color) {
    for (int yy = -r; yy <= r; ++yy)
        for (int xx = -r; xx <= r; ++xx)
            if (xx * xx + yy * yy <= r * r) Blend(cx + xx, cy + yy, color, 255);
}

void Canvas::StrokeCircle(int cx, int cy, int r, uint32_t color, int thickness) {
    const int inner = std::max(0, r - thickness);
    for (int yy = -r; yy <= r; ++yy)
        for (int xx = -r; xx <= r; ++xx) {
            int d = xx * xx + yy * yy;
            if (d <= r * r && d > inner * inner) Blend(cx + xx, cy + yy, color, 255);
        }
}

void Canvas::DrawLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    for (int i = 0; i <= steps; ++i) {
        int x = steps ? x0 + (x1 - x0) * i / steps : x0, y = steps ? y0 + (y1 - y0) * i / steps : y0;
        FillRect(x - thickness / 2, y - thickness / 2, thickness, thickness, color);
    }
}

int Canvas::TextWidth(const std::string &text, int scale) {
    return static_cast<int>(text.size()) * kFontCellW * scale;
}

int Canvas::LineHeight(int scale) { return kFontCellH * scale; }

int Canvas::DrawText(int x, int y, const std::string &text, uint32_t color, int scale) {
    int pen = x;
    for (char ch : text) {
        int code = static_cast<unsigned char>(ch);
        if (code < kFontFirst || code >= kFontFirst + kFontCount) code = '?';
        const uint8_t *glyph = &kFontAlpha[(code - kFontFirst) * kFontCellW * kFontCellH];
        for (int gy = 0; gy < kFontCellH; ++gy)
            for (int gx = 0; gx < kFontCellW; ++gx) {
                uint8_t cov = glyph[gy * kFontCellW + gx];
                if (!cov) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                        Blend(pen + gx * scale + sx, y + gy * scale + sy, color, cov);
            }
        pen += kFontCellW * scale;
    }
    return pen - x;
}

std::vector<uint32_t> PaintTitleText(const std::string &text, int scale, int margin, uint32_t color,
                                     uint32_t glow, int *w, int *h) {
    // The glyphs' coverage at the font's own size, with a pixel of room around them.
    const int mw = Canvas::TextWidth(text) + 2, mh = kFontCellH + 2;
    std::vector<float> mask(static_cast<size_t>(mw) * mh, 0.0f);
    for (size_t i = 0; i < text.size(); ++i) {
        int code = static_cast<unsigned char>(text[i]);
        if (code < kFontFirst || code >= kFontFirst + kFontCount) code = '?';
        const uint8_t *glyph = &kFontAlpha[(code - kFontFirst) * kFontCellW * kFontCellH];
        for (int gy = 0; gy < kFontCellH; ++gy)
            for (int gx = 0; gx < kFontCellW; ++gx)
                mask[static_cast<size_t>(gy + 1) * mw + 1 + static_cast<int>(i) * kFontCellW + gx] =
                    glyph[gy * kFontCellW + gx] / 255.0f;
    }
    // The glow: the coverage spread out (three box blurs, about a Gaussian).
    std::vector<float> halo = mask, tmp(mask.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                float s = 0;
                for (int d = -1; d <= 1; ++d) s += halo[static_cast<size_t>(y) * mw + std::clamp(x + d, 0, mw - 1)];
                tmp[static_cast<size_t>(y) * mw + x] = s / 3;
            }
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                float s = 0;
                for (int d = -1; d <= 1; ++d) s += tmp[static_cast<size_t>(std::clamp(y + d, 0, mh - 1)) * mw + x];
                halo[static_cast<size_t>(y) * mw + x] = s / 3;
            }
    }
    auto sample = [&](const std::vector<float> &m, float fx, float fy) {
        // Nothing beyond the mask, so the glow fades out instead of smearing its edge outwards.
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const int x1 = x0 + 1, y1 = y0 + 1;
        const float ax = fx - x0, ay = fy - y0;
        auto at = [&](int x, int y) {
            return x < 0 || y < 0 || x >= mw || y >= mh ? 0.0f : m[static_cast<size_t>(y) * mw + x];
        };
        return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
    };
    *w = mw * scale + 2 * margin;
    *h = mh * scale + 2 * margin;
    std::vector<uint32_t> out(static_cast<size_t>(*w) * *h, 0);
    const float cr = (color & 0xff) / 255.0f, cg = ((color >> 8) & 0xff) / 255.0f, cb = ((color >> 16) & 0xff) / 255.0f;
    const float gr = (glow & 0xff) / 255.0f, gg = ((glow >> 8) & 0xff) / 255.0f, gb = ((glow >> 16) & 0xff) / 255.0f;
    const float glowAlpha = (glow >> 24) / 255.0f;
    for (int y = 0; y < *h; ++y)
        for (int x = 0; x < *w; ++x) {
            const float fx = (x - margin + 0.5f) / scale - 0.5f, fy = (y - margin + 0.5f) / scale - 0.5f;
            // Enlarged smoothly, then cut at half coverage with a soft edge a pixel or two wide.
            const float v = sample(mask, fx, fy);
            const float edge = 0.5f / scale + 0.04f;
            float a = std::clamp((v - (0.5f - edge)) / (2 * edge), 0.0f, 1.0f);
            a = a * a * (3 - 2 * a);
            const float g = std::clamp(sample(halo, fx, fy) * 1.6f, 0.0f, 1.0f) * glowAlpha;
            // The text over its glow.
            const float oa = a + g * (1 - a);
            if (oa <= 0.0f) continue;
            const float r = (cr * a + gr * g * (1 - a)) / oa, gn = (cg * a + gg * g * (1 - a)) / oa,
                        b = (cb * a + gb * g * (1 - a)) / oa;
            out[static_cast<size_t>(y) * *w + x] =
                Rgba(static_cast<uint8_t>(r * 255 + 0.5f), static_cast<uint8_t>(gn * 255 + 0.5f),
                     static_cast<uint8_t>(b * 255 + 0.5f), static_cast<uint8_t>(oa * 255 + 0.5f));
        }
    return out;
}

}  // namespace khvr
