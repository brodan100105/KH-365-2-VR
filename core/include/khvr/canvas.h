// A small RGBA8 software canvas for the in-headset UI: the settings panel, the demo's DS
// screens and status text are drawn here on the CPU and uploaded as textures.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace khvr {

// 0xAABBGGRR, the byte order GL_RGBA/GL_UNSIGNED_BYTE uploads.
constexpr uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { Resize(w, h); }
    void Resize(int w, int h) {
        width_ = w;
        height_ = h;
        pixels_.assign(static_cast<size_t>(w) * h, 0);
    }
    int width() const { return width_; }
    int height() const { return height_; }
    const uint32_t *data() const { return pixels_.data(); }
    std::vector<uint32_t> &pixels() { return pixels_; }

    void Clear(uint32_t color);
    void FillRect(int x, int y, int w, int h, uint32_t color);
    void StrokeRect(int x, int y, int w, int h, uint32_t color, int thickness = 1);
    void FillCircle(int cx, int cy, int r, uint32_t color);
    // Text at an integer scale of the 10x20 font cell. Returns the width drawn in pixels.
    int DrawText(int x, int y, const std::string &text, uint32_t color, int scale = 1);
    static int TextWidth(const std::string &text, int scale = 1);
    static int LineHeight(int scale = 1);

private:
    void Blend(int x, int y, uint32_t color, uint8_t coverage);
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> pixels_;
};

}  // namespace khvr
