#include "render2d.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "guest.h"
#include "hle.h"
#include "hle_gx.h"

namespace khvr::hle {

namespace {

constexpr uint32_t kTransparent = 0xffffffff;  // "no pixel" in a 15-bit colour slot

struct Engine {
    int index;                 // 0 = A, 1 = B
    uint32_t io;               // register offset: 0x000 or 0x1000
    const uint8_t *bgPal, *objPal, *oam;
    std::vector<uint8_t> bg, obj;  // CPU-visible VRAM, as mapped this frame
    uint32_t dispcnt;
};

uint16_t Pal(const uint8_t *pal, uint32_t i) { return static_cast<uint16_t>(pal[2 * i] | (pal[2 * i + 1] << 8)); }
uint16_t Rd16(const std::vector<uint8_t> &v, uint32_t a) {
    a %= static_cast<uint32_t>(v.size());
    return static_cast<uint16_t>(v[a] | (v[(a + 1) % v.size()] << 8));
}
uint8_t Rd8(const std::vector<uint8_t> &v, uint32_t a) { return v[a % v.size()]; }

uint32_t ToRgba(uint16_t c, uint8_t alpha = 255) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(alpha) << 24);
}

int32_t Sext(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }

// One layer's line: colour (15-bit) or kTransparent per pixel.
struct Line {
    uint32_t c[kScreenW];
};

uint16_t ExtOrPal(const Engine &e, bool obj, int slot, uint32_t pal, uint32_t idx) {
    if ((e.dispcnt >> (obj ? 31 : 30)) & 1) {
        if (const uint8_t *ext = VramExtPalette(e.index, obj, obj ? 0 : slot))
            return static_cast<uint16_t>(ext[2 * (pal * 256 + idx)] | (ext[2 * (pal * 256 + idx) + 1] << 8));
        return 0;
    }
    return Pal(obj ? e.objPal : e.bgPal, idx);
}

void TextBg(const Engine &e, int bg, int y, Line *out) {
    uint16_t cnt = IoReg16(e.io + 0x08 + 2 * bg);
    uint32_t hofs = IoReg16(e.io + 0x10 + 4 * bg) & 511, vofs = IoReg16(e.io + 0x12 + 4 * bg) & 511;
    uint32_t charBase = ((cnt >> 2) & 15) * 0x4000, screenBase = ((cnt >> 8) & 31) * 0x800;
    if (e.index == 0) {
        charBase += ((e.dispcnt >> 24) & 7) * 0x10000;
        screenBase += ((e.dispcnt >> 27) & 7) * 0x10000;
    }
    uint32_t w = (cnt & 0x4000) ? 512 : 256, h = (cnt & 0x8000) ? 512 : 256;
    bool c256 = cnt & 0x80;
    int slot = bg < 2 && (cnt & 0x2000) ? bg + 2 : bg;
    uint32_t yy = (y + vofs) & (h - 1);
    for (int x = 0; x < kScreenW; ++x) {
        uint32_t xx = (x + hofs) & (w - 1);
        uint32_t sb = (xx >= 256 ? 1 : 0) + (yy >= 256 ? (w == 512 ? 2 : 1) : 0);
        uint16_t entry = Rd16(e.bg, screenBase + sb * 0x800 + ((yy & 255) / 8) * 64 + ((xx & 255) / 8) * 2);
        uint32_t tile = entry & 0x3ff, pal = entry >> 12;
        uint32_t px = xx & 7, py = yy & 7;
        if (entry & 0x400) px = 7 - px;
        if (entry & 0x800) py = 7 - py;
        uint32_t idx;
        if (c256) {
            idx = Rd8(e.bg, charBase + tile * 64 + py * 8 + px);
            out->c[x] = idx ? ExtOrPal(e, false, slot, pal, idx) : kTransparent;
        } else {
            uint8_t b = Rd8(e.bg, charBase + tile * 32 + py * 4 + px / 2);
            idx = (px & 1) ? b >> 4 : b & 15;
            out->c[x] = idx ? Pal(e.bgPal, pal * 16 + idx) : kTransparent;
        }
    }
}

void AffineBg(const Engine &e, int bg, int y, bool extended, Line *out) {
    uint16_t cnt = IoReg16(e.io + 0x08 + 2 * bg);
    uint32_t p = e.io + 0x20 + 0x10 * (bg - 2);
    int32_t pa = static_cast<int16_t>(IoReg16(p)), pb = static_cast<int16_t>(IoReg16(p + 2));
    int32_t pc = static_cast<int16_t>(IoReg16(p + 4)), pd = static_cast<int16_t>(IoReg16(p + 6));
    int32_t refX = Sext(IoReg32(p + 8), 28), refY = Sext(IoReg32(p + 12), 28);
    uint32_t charBase = ((cnt >> 2) & 15) * 0x4000, screenBase = ((cnt >> 8) & 31) * 0x800;
    if (e.index == 0) {
        charBase += ((e.dispcnt >> 24) & 7) * 0x10000;
        screenBase += ((e.dispcnt >> 27) & 7) * 0x10000;
    }
    bool wrap = cnt & 0x2000;
    uint32_t sizeBits = cnt >> 14;
    int mode = 0;  // 0 tile map (8-bit), 1 extended map (16-bit), 2 256-colour bitmap, 3 direct colour
    uint32_t w = 128u << sizeBits, h = w;
    if (extended) {
        if (!(cnt & 0x80)) mode = 1;
        else {
            mode = (cnt & 4) ? 3 : 2;
            static const uint32_t bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
            w = bw[sizeBits];
            h = bh[sizeBits];
        }
    }
    int32_t sx = refX + pb * y, sy = refY + pd * y;
    for (int x = 0; x < kScreenW; ++x) {
        int32_t tx = (sx + pa * x) >> 8, ty = (sy + pc * x) >> 8;
        if (wrap) {
            tx &= static_cast<int32_t>(w - 1);
            ty &= static_cast<int32_t>(h - 1);
        } else if (tx < 0 || ty < 0 || tx >= static_cast<int32_t>(w) || ty >= static_cast<int32_t>(h)) {
            out->c[x] = kTransparent;
            continue;
        }
        uint32_t ux = static_cast<uint32_t>(tx), uy = static_cast<uint32_t>(ty);
        switch (mode) {
        case 0: {
            uint32_t tile = Rd8(e.bg, screenBase + (uy / 8) * (w / 8) + ux / 8);
            uint32_t idx = Rd8(e.bg, charBase + tile * 64 + (uy & 7) * 8 + (ux & 7));
            out->c[x] = idx ? Pal(e.bgPal, idx) : kTransparent;
            break;
        }
        case 1: {
            uint16_t entry = Rd16(e.bg, screenBase + ((uy / 8) * (w / 8) + ux / 8) * 2);
            uint32_t px = ux & 7, py = uy & 7;
            if (entry & 0x400) px = 7 - px;
            if (entry & 0x800) py = 7 - py;
            uint32_t idx = Rd8(e.bg, charBase + (entry & 0x3ff) * 64 + py * 8 + px);
            out->c[x] = idx ? ExtOrPal(e, false, bg, entry >> 12, idx) : kTransparent;
            break;
        }
        case 2: {
            uint32_t idx = Rd8(e.bg, ((cnt >> 8) & 31) * 0x4000 + uy * w + ux);
            out->c[x] = idx ? Pal(e.bgPal, idx) : kTransparent;
            break;
        }
        default: {
            uint16_t c = Rd16(e.bg, ((cnt >> 8) & 31) * 0x4000 + (uy * w + ux) * 2);
            out->c[x] = (c & 0x8000) ? (c & 0x7fff) : kTransparent;
            break;
        }
        }
    }
}

struct ObjLine {
    uint32_t c[kScreenW];
    uint8_t prio[kScreenW];
    bool semi[kScreenW];
};

void Objects(const Engine &e, int y, ObjLine *out) {
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    for (int x = 0; x < kScreenW; ++x) {
        out->c[x] = kTransparent;
        out->prio[x] = 4;
        out->semi[x] = false;
    }
    bool map1d = e.dispcnt & 0x10;
    uint32_t bound = 32u << ((e.dispcnt >> 20) & 3);
    for (int i = 127; i >= 0; --i) {
        const uint8_t *o = e.oam + 8 * i;
        uint16_t a0 = static_cast<uint16_t>(o[0] | (o[1] << 8)), a1 = static_cast<uint16_t>(o[2] | (o[3] << 8));
        uint16_t a2 = static_cast<uint16_t>(o[4] | (o[5] << 8));
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;   // disabled
        int mode = (a0 >> 10) & 3;
        if (mode == 2) continue;                 // OBJ window (not implemented)
        int shape = a0 >> 14, size = a1 >> 14;
        if (shape == 3) continue;
        int w = kW[shape][size], h = kH[shape][size];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) {
            bw *= 2;
            bh *= 2;
        }
        int oy = a0 & 255;
        if (oy >= kScreenH) oy -= 256;
        int ox = Sext(a1 & 511, 9);
        int ly = y - oy;
        if (ly < 0 || ly >= bh) continue;
        if (mode == 3) {
            LogOnce("obj-bitmap", "render2d: bitmap sprites are not drawn yet");
            continue;
        }
        bool c256 = a0 & 0x2000;
        uint32_t tile = a2 & 0x3ff, pal = a2 >> 12, prio = (a2 >> 10) & 3;
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const uint8_t *pp = e.oam + 32 * ((a1 >> 9) & 31);
            pa = static_cast<int16_t>(pp[6] | (pp[7] << 8));
            pb = static_cast<int16_t>(pp[14] | (pp[15] << 8));
            pc = static_cast<int16_t>(pp[22] | (pp[23] << 8));
            pd = static_cast<int16_t>(pp[30] | (pp[31] << 8));
        }
        for (int lx = 0; lx < bw; ++lx) {
            int sx = ox + lx;
            if (sx < 0 || sx >= kScreenW) continue;
            int tx, ty;
            if (affine) {
                int cx = lx - bw / 2, cy = ly - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            uint32_t addr;
            uint32_t tcol = static_cast<uint32_t>(tx) / 8, trow = static_cast<uint32_t>(ty) / 8;
            uint32_t px = static_cast<uint32_t>(tx) & 7, py = static_cast<uint32_t>(ty) & 7;
            uint32_t idx;
            if (c256) {
                if (map1d) addr = tile * bound + (trow * (w / 8) + tcol) * 64;
                else addr = tile * 32 + trow * 1024 + tcol * 64;
                idx = Rd8(e.obj, addr + py * 8 + px);
            } else {
                if (map1d) addr = tile * bound + (trow * (w / 8) + tcol) * 32;
                else addr = tile * 32 + trow * 1024 + tcol * 32;
                uint8_t b = Rd8(e.obj, addr + py * 4 + px / 2);
                idx = (px & 1) ? b >> 4 : b & 15;
            }
            if (!idx || prio > out->prio[sx]) continue;
            out->c[sx] = c256 ? ExtOrPal(e, true, 0, pal, idx) : Pal(e.objPal, pal * 16 + idx);
            out->prio[sx] = static_cast<uint8_t>(prio);
            out->semi[sx] = mode == 1;
        }
    }
}

uint16_t Blend(uint16_t a, uint16_t b, uint32_t eva, uint32_t evb) {
    uint32_t out = 0;
    for (int s = 0; s < 15; s += 5) {
        uint32_t v = (((a >> s) & 31) * eva + ((b >> s) & 31) * evb) >> 4;
        out |= std::min<uint32_t>(v, 31) << s;
    }
    return static_cast<uint16_t>(out);
}

uint16_t Bright(uint16_t a, uint32_t evy, bool up) {
    uint32_t out = 0;
    for (int s = 0; s < 15; s += 5) {
        uint32_t v = (a >> s) & 31;
        v = up ? v + (((31 - v) * evy) >> 4) : v - ((v * evy) >> 4);
        out |= v << s;
    }
    return static_cast<uint16_t>(out);
}

// Renders one engine. `hud` makes the backdrop and the 3D layer transparent.
void RenderEngine(int index, uint32_t *rgba, uint32_t *hud, bool *has3d) {
    Engine e;
    e.index = index;
    e.io = index ? 0x1000 : 0;
    e.bgPal = GuestMem() + 0x05000000 + (index ? 0x400 : 0);
    e.objPal = e.bgPal + 0x200;
    e.oam = GuestMem() + 0x07000000 + (index ? 0x400 : 0);
    e.dispcnt = IoReg32(e.io);
    uint32_t displayMode = (e.dispcnt >> 16) & 3;
    bool bg0is3d = index == 0 && (e.dispcnt & 8);
    if (has3d) *has3d = bg0is3d && (e.dispcnt & 0x100);

    if (displayMode == 0 || (e.dispcnt & 0x80)) {  // display off / forced blank: white
        for (int i = 0; i < kScreenW * kScreenH; ++i) {
            rgba[i] = 0xffffffff;
            if (hud) hud[i] = 0;
        }
        return;
    }
    if (index == 0 && displayMode == 2) {  // VRAM display: a LCDC bank shown as is (movies)
        const uint8_t *bank = VramBank(static_cast<int>((e.dispcnt >> 18) & 3));
        for (int i = 0; i < kScreenW * kScreenH; ++i) {
            rgba[i] = ToRgba(static_cast<uint16_t>(bank[2 * i] | (bank[2 * i + 1] << 8)));
            if (hud) hud[i] = 0;
        }
        return;
    }
    e.bg.resize(index ? 0x20000 : 0x80000);
    e.obj.resize(index ? 0x20000 : 0x40000);
    VramCopyRegion(index ? 0x06200000 : 0x06000000, static_cast<uint32_t>(e.bg.size()), e.bg.data());
    VramCopyRegion(index ? 0x06600000 : 0x06400000, static_cast<uint32_t>(e.obj.size()), e.obj.data());

    uint32_t bgMode = e.dispcnt & 7;
    uint16_t bldcnt = IoReg16(e.io + 0x50), bldalpha = IoReg16(e.io + 0x52), bldy = IoReg16(e.io + 0x54);
    uint32_t eva = std::min(bldalpha & 31, 16), evb = std::min((bldalpha >> 8) & 31, 16), evy = std::min(bldy & 31, 16);
    uint32_t effect = (bldcnt >> 6) & 3;
    uint16_t winH[2] = {IoReg16(e.io + 0x40), IoReg16(e.io + 0x42)};
    uint16_t winV[2] = {IoReg16(e.io + 0x44), IoReg16(e.io + 0x46)};
    uint16_t winIn = IoReg16(e.io + 0x48), winOut = IoReg16(e.io + 0x4a);
    bool windows = e.dispcnt & 0x6000;
    uint16_t master = IoReg16(e.io + 0x6c);
    uint16_t backdrop = Pal(e.bgPal, 0);

    // The 3D layer, when BG0 shows it: rendered once for the whole screen.
    static uint16_t color3d[kScreenW * kScreenH];
    static uint8_t alpha3d[kScreenW * kScreenH];
    const bool draw3d = bg0is3d && (e.dispcnt & 0x100);
    if (draw3d) GxRender(color3d, alpha3d);

    Line bgl[4];
    ObjLine objl;
    for (int y = 0; y < kScreenH; ++y) {
        bool on[4];
        for (int b = 0; b < 4; ++b) {
            on[b] = (e.dispcnt >> (8 + b)) & 1;
            if (!on[b]) continue;
            if (b == 0 && bg0is3d) {
                for (int x = 0; x < kScreenW; ++x) {
                    int i = y * kScreenW + x;
                    bgl[0].c[x] = draw3d && alpha3d[i] ? color3d[i] : kTransparent;
                }
                continue;
            }
            bool affine = (b == 2 && (bgMode == 2 || bgMode == 4)) || (b == 3 && (bgMode == 1 || bgMode == 2));
            bool extended = (b == 3 && bgMode >= 3 && bgMode <= 5) || (b == 2 && bgMode == 5);
            if (b >= 2 && bgMode == 6) {
                on[b] = false;  // large bitmap mode: not drawn yet
                continue;
            }
            if (affine || extended) AffineBg(e, b, y, extended, &bgl[b]);
            else TextBg(e, b, y, &bgl[b]);
        }
        bool objOn = e.dispcnt & 0x1000;
        if (objOn) Objects(e, y, &objl);

        for (int x = 0; x < kScreenW; ++x) {
            // Which layers and effects the windows allow here.
            uint32_t mask = 0x3f;
            if (windows) {
                mask = winOut & 0x3f;
                for (int w = 1; w >= 0; --w) {
                    if (!((e.dispcnt >> (13 + w)) & 1)) continue;
                    int x1 = winH[w] >> 8, x2 = winH[w] & 255, y1 = winV[w] >> 8, y2 = winV[w] & 255;
                    bool inX = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
                    bool inY = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
                    if (inX && inY) mask = (winIn >> (8 * w)) & 0x3f;
                }
            }
            // The two front-most layers: (colour, layer id 0-3 BG, 4 OBJ, 5 backdrop).
            uint32_t top = backdrop, second = backdrop;
            int topId = 5, secondId = 5, topPrio = 4, secondPrio = 4;
            bool semi = false;
            // Layers come back to front (BG3 .. BG0, then sprites), so on equal priority the
            // later one is in front: a lower BG number, and sprites over BGs.
            auto consider = [&](uint32_t c, int id, int prio) {
                if (prio <= topPrio) {
                    second = top; secondId = topId; secondPrio = topPrio;
                    top = c; topId = id; topPrio = prio;
                } else if (prio <= secondPrio) {
                    second = c; secondId = id; secondPrio = prio;
                }
            };
            for (int b = 3; b >= 0; --b) {
                if (!on[b] || !((mask >> b) & 1) || bgl[b].c[x] == kTransparent) continue;
                consider(bgl[b].c[x], b, IoReg16(e.io + 0x08 + 2 * b) & 3);
            }
            if (objOn && (mask & 0x10) && objl.c[x] != kTransparent) {
                consider(objl.c[x], 4, objl.prio[x]);
                semi = topId == 4 && objl.semi[x];
            }
            uint16_t c = static_cast<uint16_t>(top);
            bool fx = (mask & 0x20) != 0;
            bool firstTarget = (bldcnt >> topId) & 1, secondTarget = (bldcnt >> (8 + secondId)) & 1;
            const bool top3dPixel = topId == 0 && bg0is3d;
            const int a3d = top3dPixel ? alpha3d[y * kScreenW + x] : 31;
            if (top3dPixel && a3d < 31 && secondTarget) {
                // The 3D layer blends with what is behind it by its own alpha.
                uint32_t a = static_cast<uint32_t>(a3d + 1) / 2;
                c = Blend(c, static_cast<uint16_t>(second), a, 16 - a);
            } else if (semi && secondTarget) c = Blend(c, static_cast<uint16_t>(second), eva, evb);
            else if (fx && firstTarget) {
                if (effect == 1 && secondTarget) c = Blend(c, static_cast<uint16_t>(second), eva, evb);
                else if (effect == 2) c = Bright(c, evy, true);
                else if (effect == 3) c = Bright(c, evy, false);
            }
            uint32_t mode = master >> 14, factor = std::min<uint32_t>(master & 31, 16);
            if (mode == 1) c = Bright(c, factor, true);
            else if (mode == 2) c = Bright(c, factor, false);
            int i = y * kScreenW + x;
            rgba[i] = ToRgba(c);
            // The HUD image leaves the 3D layer out: what is behind it instead.
            if (hud) hud[i] = top3dPixel ? (secondId == 5 ? 0 : ToRgba(static_cast<uint16_t>(second)))
                                         : (topId == 5 ? 0 : ToRgba(c));
        }
    }
}

}  // namespace

void Render2D(PlatformScreens *out) {
    // POWCNT1 bit 15: engine A on the top screen.
    bool aOnTop = IoReg16(0x304) & 0x8000;
    bool has3d = false;
    if (aOnTop) {
        RenderEngine(0, out->top, out->topHud, &has3d);
        RenderEngine(1, out->bottom, nullptr, nullptr);
    } else {
        RenderEngine(1, out->top, out->topHud, nullptr);
        RenderEngine(0, out->bottom, nullptr, &has3d);
        has3d = false;  // 3D on the bottom screen: the VR view shows the top
    }
    out->top3d = has3d;
}

}  // namespace khvr::hle
