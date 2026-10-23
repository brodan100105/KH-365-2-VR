#include "khvr/canvas.h"

#include <algorithm>

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

}  // namespace khvr
