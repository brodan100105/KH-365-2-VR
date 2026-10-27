#include "station_art.h"

#include <algorithm>
#include <cmath>

namespace khvr {

namespace {

constexpr float kPi = 3.14159265f;

struct Rgb {
    float r, g, b;
};

Rgb Mix(const Rgb &a, const Rgb &b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb Scale(const Rgb &c, float k) { return {c.r * k, c.g * k, c.b * k}; }

float Hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffff) / 16777215.0f;
}

float ValueNoise(float x, float y) {
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

// Marbled glass: a few octaves of value noise, 0..1.
float Marble(float x, float y) {
    float v = 0.5f * ValueNoise(x * 9, y * 9) + 0.3f * ValueNoise(x * 23 + 7, y * 23 + 3) +
              0.2f * ValueNoise(x * 61 + 1, y * 61 + 9);
    return v;
}

// Irregular panes for the inner scene: distance to the nearest pane edge (Voronoi on a jittered
// grid) and a per-pane random value for its tint.
void Panes(float x, float y, float *edge, float *tint) {
    const float cell = 0.095f;
    float gx = x / cell, gy = y / cell;
    int cx = static_cast<int>(std::floor(gx)), cy = static_cast<int>(std::floor(gy));
    float d1 = 1e9f, d2 = 1e9f;
    int best = 0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            int px = cx + i, py = cy + j;
            float sx = (px + 0.15f + 0.7f * Hash(px, py * 7 + 1)) * cell;
            float sy = (py + 0.15f + 0.7f * Hash(px * 5 + 3, py)) * cell;
            float d = std::hypot(x - sx, y - sy);
            if (d < d1) {
                d2 = d1;
                d1 = d;
                best = px * 131 + py;
            } else if (d < d2) {
                d2 = d;
            }
        }
    *edge = (d2 - d1) * 0.5f;
    *tint = Hash(best, 17);
}

float SdBox(float x, float y, float hx, float hy) {
    float dx = std::fabs(x) - hx, dy = std::fabs(y) - hy;
    float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    return std::hypot(ox, oy) + std::min(std::max(dx, dy), 0.0f);
}

float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return std::hypot(wx - vx * t, wy - vy * t);
}

// A heart, point down, of half-width ~s around (cx, cy): negative inside.
bool InHeart(float x, float y, float cx, float cy, float s) {
    float X = (x - cx) / s, Y = -(y - cy) / s + 0.25f;
    float a = X * X + Y * Y - 1.0f;
    return a * a * a - X * X * Y * Y * Y < 0.0f;
}

const Rgb kLead{26, 14, 10};
const Rgb kGold{236, 186, 74};
const Rgb kOrange{236, 128, 34};
const Rgb kDeepOrange{196, 82, 22};
const Rgb kRed{176, 28, 22};

// One point of the window, x and y in -1..1 with y down. Returns false outside the disc.
bool Paint(float x, float y, Rgb *out) {
    const float r = std::hypot(x, y);
    if (r > 1.0f) return false;
    const float th = std::atan2(y, x);
    const float marble = Marble(x, y);
    Rgb c;

    auto lead = [&](float d, float w) { return std::fabs(d) < w; };

    if (r > 0.985f) {
        c = kLead;
    } else if (r > 0.968f) {
        c = Scale(kGold, 0.75f + 0.35f * marble);
    } else if (r > 0.955f) {
        c = kLead;
    } else if (r > 0.80f) {
        // The petal band: red lens-shaped petals pointing outward on orange glass.
        const int n = 30;
        float u = (r - 0.8775f) / 0.0775f;
        float f = th * n / (2 * kPi);
        float phi = f - std::floor(f) - 0.5f;
        float half = 0.36f * std::sqrt(std::max(0.0f, 1.0f - u * u));
        float edge = std::fabs(phi) - half;
        // Small orange diamonds between the petals' tips.
        float g = f + 0.5f;
        float phi2 = g - std::floor(g) - 0.5f;
        float diamond = std::fabs(phi2) * 2.6f + std::fabs(u) * 0.9f - 0.42f;
        if (lead(edge, 0.025f) || lead(diamond, 0.035f)) c = kLead;
        else if (edge < 0) c = Scale(kRed, 0.8f + 0.4f * marble);
        else if (diamond < 0) c = Scale(kGold, 0.8f + 0.3f * marble);
        else c = Scale(kOrange, 0.8f + 0.35f * marble);
    } else if (r > 0.785f) {
        c = kLead;
    } else if (r > 0.6f) {
        // The medallion ring: twenty round windows on deep orange glass.
        const int n = 20;
        float f = th * n / (2 * kPi) + 0.5f;
        int k = static_cast<int>(std::floor(f));
        float a = (k + 0.0f) * 2 * kPi / n;
        float mx = 0.692f * std::cos(a), my = 0.692f * std::sin(a);
        float dx = x - mx, dy = y - my;
        float d = std::hypot(dx, dy);
        c = Scale(kDeepOrange, 0.75f + 0.4f * marble);
        if (d < 0.082f) {
            if (d > 0.072f) c = Scale(kGold, 0.8f + 0.3f * marble);
            else if (d > 0.064f) c = kLead;
            else {
                // An original four-pointed star in each window, its points along the ring.
                float ca = std::cos(-a), sa = std::sin(-a);
                float u = (dx * ca - dy * sa) / 0.05f, v = (dx * sa + dy * ca) / 0.05f;
                float star = std::pow(std::fabs(u), 0.6f) + std::pow(std::fabs(v), 0.6f);
                if (std::fabs(star - 1.0f) < 0.09f || d < 0.012f) c = kLead;
                else if (star < 1.0f) c = Scale(kRed, 0.85f + 0.3f * marble);
                else c = Scale(Rgb{248, 170, 64}, 0.85f + 0.3f * marble);
            }
        } else if (d < 0.09f) {
            c = kLead;
        }
        // Leaded spokes between the windows.
        float between = std::fabs(f - std::floor(f) - 0.5f);
        if (between > 0.485f && d > 0.09f) c = kLead;
    } else if (r > 0.585f) {
        c = Scale(kGold, 0.75f + 0.35f * marble);
    } else if (r > 0.57f) {
        c = kLead;
    } else {
        // The inner scene: a Twilight Town sunset behind a clock tower, the heart-shaped moon,
        // and a key across the middle.
        float paneEdge, tint;
        Panes(x, y, &paneEdge, &tint);
        float sunset = std::clamp(0.55f * (y + 0.57f) / 1.14f + 0.45f * (x * 0.5f + y * 0.7f + 0.3f), 0.0f, 1.0f);
        Rgb sky = sunset < 0.5f ? Mix(Rgb{252, 186, 74}, Rgb{236, 104, 44}, sunset * 2)
                                : Mix(Rgb{236, 104, 44}, Rgb{126, 40, 118}, sunset * 2 - 1);
        c = Scale(sky, 0.82f + 0.22f * tint + 0.12f * marble);
        bool leadHere = paneEdge < 0.0065f;

        // Moon: a pale heart with a soft glow.
        const float hx = 0.26f, hy = -0.30f;
        float glow = std::exp(-std::pow(std::hypot(x - hx, y - hy) / 0.2f, 2.0f));
        c = Mix(c, Rgb{255, 230, 170}, glow * 0.45f);
        if (InHeart(x, y, hx, hy, 0.1f)) {
            c = Scale(Rgb{255, 240, 196}, 0.92f + 0.12f * marble);
            leadHere = false;
            if (!InHeart(x, y, hx, hy, 0.088f)) leadHere = true;
        }

        // Town skyline along the bottom, the clock tower on the left.
        const Rgb kBuilding{92, 34, 26};
        bool building = false;
        for (int i = 0; i < 9; ++i) {
            float bx = -0.56f + i * 0.14f, w = 0.05f + 0.02f * Hash(i, 3);
            float top = 0.30f + 0.12f * Hash(i, 9);
            float roof = top - 0.06f + 0.06f * std::fabs(x - bx) / w;
            if (std::fabs(x - bx) < w && y > std::min(top, roof)) building = true;
        }
        float tower = SdBox(x + 0.36f, y - 0.13f, 0.085f, 0.36f);
        float roofTop = -0.23f - 0.17f * (1.0f - std::fabs(x + 0.36f) / 0.11f);
        bool inRoof = std::fabs(x + 0.36f) < 0.11f && y > roofTop && y < -0.22f;
        if (tower < 0 || inRoof) building = true;
        if (building) {
            c = Scale(kBuilding, 0.8f + 0.3f * marble);
            leadHere = paneEdge < 0.004f;
            // Lit windows.
            float wx = (x + 0.6f) / 0.045f, wy = (y + 0.6f) / 0.06f;
            if (std::fabs(wx - std::floor(wx) - 0.5f) < 0.18f && std::fabs(wy - std::floor(wy) - 0.5f) < 0.16f &&
                Hash(static_cast<int>(wx), static_cast<int>(wy)) > 0.55f && y > -0.05f)
                c = Rgb{255, 214, 110};
        }
        if (lead(tower, 0.006f) && y > -0.22f) leadHere = true;
        // The clock face.
        float clock = std::hypot(x + 0.36f, y + 0.08f);
        if (clock < 0.06f) {
            c = Scale(Rgb{250, 236, 196}, 0.9f + 0.15f * marble);
            leadHere = clock > 0.052f ||
                       SdSegment(x, y, -0.36f, -0.08f, -0.36f, -0.125f) < 0.006f ||
                       SdSegment(x, y, -0.36f, -0.08f, -0.33f, -0.08f) < 0.006f;
        }

        // The key, handle up and right, teeth down and left.
        const float ax = 0.17f, ay = -0.14f, bx = -0.30f, by = 0.33f;
        float shaft = SdSegment(x, y, ax, ay, bx, by) - 0.022f;
        float ring = std::fabs(std::hypot(x - 0.22f, y + 0.19f) - 0.06f) - 0.02f;
        float guard = SdSegment(x, y, 0.10f, -0.17f, 0.20f, -0.07f) - 0.018f;
        // Teeth: a block on one side of the shaft's far end.
        float dirx = (bx - ax), diry = (by - ay);
        float len = std::hypot(dirx, diry);
        dirx /= len;
        diry /= len;
        float lx = x - bx, ly = y - by;
        float along = -(lx * dirx + ly * diry), across = lx * -diry + ly * dirx;
        float teeth = std::min(SdBox(along - 0.02f, across - 0.05f, 0.02f, 0.035f),
                               SdBox(along - 0.075f, across - 0.04f, 0.018f, 0.025f));
        float key = std::min(std::min(shaft, ring), std::min(guard, teeth));
        if (key < 0) {
            bool gold = ring < 0 || guard < 0;
            c = gold ? Scale(kGold, 0.9f + 0.25f * marble) : Scale(Rgb{214, 214, 228}, 0.88f + 0.2f * marble);
            leadHere = false;
        }
        if (lead(key, 0.007f)) leadHere = true;
        if (leadHere) c = kLead;
    }

    // Light from above: the middle glows.
    *out = Scale(c, 1.08f - 0.22f * r);
    return true;
}

}  // namespace

std::vector<uint32_t> PaintGlow(int size) {
    std::vector<uint32_t> px(static_cast<size_t>(size) * size, 0);
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            float x = (i + 0.5f) / size * 2 - 1, y = (j + 0.5f) / size * 2 - 1;
            float r = std::hypot(x, y);
            float core = std::exp(-r * r * 30.0f);
            float halo = std::exp(-r * r * 5.0f) * 0.55f;
            float glint = std::exp(-std::fabs(x) * 26.0f) * std::exp(-std::fabs(y) * 3.5f) +
                          std::exp(-std::fabs(y) * 26.0f) * std::exp(-std::fabs(x) * 3.5f);
            float a = std::clamp(core + halo + glint * 0.35f, 0.0f, 1.0f) * std::clamp((1.0f - r) * 4.0f, 0.0f, 1.0f);
            Rgb c = Mix(Rgb{255, 214, 140}, Rgb{255, 252, 236}, core);
            auto ch = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f)); };
            px[static_cast<size_t>(j) * size + i] =
                (ch(a * 255) << 24) | (ch(c.b) << 16) | (ch(c.g) << 8) | ch(c.r);
        }
    return px;
}

std::vector<uint32_t> PaintStationArt(int size) {
    std::vector<uint32_t> px(static_cast<size_t>(size) * size, 0);
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            float x = (i + 0.5f) / size * 2 - 1, y = (j + 0.5f) / size * 2 - 1;
            Rgb c;
            if (!Paint(x, y, &c)) continue;
            auto ch = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f)); };
            px[static_cast<size_t>(j) * size + i] = 0xff000000u | (ch(c.b) << 16) | (ch(c.g) << 8) | ch(c.r);
        }
    return px;
}

}  // namespace khvr
