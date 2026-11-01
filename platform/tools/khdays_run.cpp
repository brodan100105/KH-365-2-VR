// Desktop runner: boots the game headless on the platform layer and reports how far it gets.
//
//   khdays_run --rom game.nds [--frames 600] [--save khdays.sav] [--log boot.log] [--screens prefix]
//   khdays_run --fake-rom [--frames 60]
//
// --fake-rom builds a minimal image with the right game code, an empty file system and an
// overlay table, so the boot path (OS, threads, interrupts, ARM7 handshakes, file system set-up,
// overlays) can be exercised without the game's data. The real game needs your own ROM.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "khvr/platform.h"
#include "../src/guest.h"
#include "../src/hle.h"
#include "../src/hle_gx.h"
#include "../src/sound.h"

namespace khvr::hle {
void Arm7Reset();
void Arm7OnSend(uint32_t word);
bool Arm7HasReply();
uint32_t Arm7PopReply();
void Arm7VBlank();
}  // namespace khvr::hle

namespace {

void Put32(std::vector<uint8_t> &b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }
void Put16(std::vector<uint8_t> &b, size_t o, uint16_t v) { std::memcpy(&b[o], &v, 2); }

std::string WriteFakeRom(const std::string &path) {
    const uint32_t kOverlays = 303, kFnt = 0x4000, kFat = 0x4100, kOvt = 0x6000;
    std::vector<uint8_t> rom(0x20000, 0);
    std::memcpy(&rom[0], "KH DAYS FAKE", 12);
    std::memcpy(&rom[0x0c], "YKGP", 4);
    std::memcpy(&rom[0x10], "8P", 2);
    // File name table: the root directory, empty.
    Put32(rom, 0x40, kFnt);
    Put32(rom, 0x44, 16);
    Put32(rom, kFnt + 0, 8);                 // sub-table offset
    Put16(rom, kFnt + 4, static_cast<uint16_t>(kOverlays));  // first file id after the overlays
    Put16(rom, kFnt + 6, 1);                 // one directory
    rom[kFnt + 8] = 0;                       // end of the root's entries
    // File allocation table: one empty file per overlay.
    Put32(rom, 0x48, kFat);
    Put32(rom, 0x4c, kOverlays * 8);
    for (uint32_t i = 0; i < kOverlays; ++i) {
        Put32(rom, kFat + 8 * i, 0x10000);
        Put32(rom, kFat + 8 * i + 4, 0x10000);
    }
    // ARM9 overlay table: id, RAM address, sizes 0, file id = id.
    Put32(rom, 0x50, kOvt);
    Put32(rom, 0x54, kOverlays * 32);
    for (uint32_t i = 0; i < kOverlays; ++i) {
        size_t o = kOvt + 32 * i;
        Put32(rom, o + 0, i);
        Put32(rom, o + 4, 0x0204cac0);
        Put32(rom, o + 24, i);
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(rom.data(), 1, rom.size(), f) != rom.size()) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fclose(f);
    return path;
}

void WritePpm(const char *path, const uint32_t *px) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6 256 192 255\n");
    for (int i = 0; i < 256 * 192; ++i) {
        uint8_t rgb[3] = {static_cast<uint8_t>(px[i]), static_cast<uint8_t>(px[i] >> 8), static_cast<uint8_t>(px[i] >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

// Checks the 2D renderer without a ROM: one text background half filled with a red tile over a
// blue backdrop, and a green 8x8 sprite.
int RenderTest(const char *ppm) {
    using khvr::PlatformTestWrite;
    khvr::PlatformInitForTests();
    PlatformTestWrite(0x04000304, 0x8000, 2);       // POWCNT1: engine A on the top screen
    PlatformTestWrite(0x04000240, 0x81, 1);         // bank A -> BG VRAM A
    PlatformTestWrite(0x04000241, 0x82, 1);         // bank B -> OBJ VRAM A
    PlatformTestWrite(0x04000000, 0x00011110, 4);   // display on, BG0 + OBJ, 1D sprites
    PlatformTestWrite(0x04000008, 8 << 8, 2);       // BG0: tiles at 0, map at 0x4000
    PlatformTestWrite(0x05000000, 0x7c00, 2);       // backdrop: blue
    PlatformTestWrite(0x05000002, 0x001f, 2);       // colour 1: red
    PlatformTestWrite(0x05000204, 0x03e0, 2);       // sprite colour 2: green
    for (int i = 0; i < 32; i += 4) PlatformTestWrite(0x06000020 + i, 0x11111111, 4);  // tile 1
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 16; ++col) PlatformTestWrite(0x06004000 + (row * 32 + col) * 2, 1, 2);
    for (int i = 0; i < 32; i += 4) PlatformTestWrite(0x06400000 + i, 0x22222222, 4);  // sprite tile 0
    for (int s = 1; s < 128; ++s) PlatformTestWrite(0x07000000 + 8 * s, 0x0200, 2);     // others off
    PlatformTestWrite(0x07000000, 50, 2);           // sprite 0: y 50
    PlatformTestWrite(0x07000002, 200, 2);          // x 200
    PlatformTestWrite(0x07000004, 0, 2);            // tile 0, palette 0
    static khvr::PlatformScreens s;
    khvr::Platform::Get().CaptureScreens(&s);
    if (ppm) WritePpm(ppm, s.top);
    auto at = [&](int x, int y) { return s.top[y * 256 + x] & 0xffffff; };
    bool ok = at(10, 10) == 0x0000ff && at(180, 10) == 0xff0000 && at(203, 53) == 0x00ff00 &&
              (s.topHud[180 * 256 + 180] >> 24) == 0 && (s.bottom[0] & 0xffffff) == 0xffffff;
    std::printf("render test: %s (bg 0x%06x, backdrop 0x%06x, sprite 0x%06x)\n", ok ? "ok" : "FAILED",
                at(10, 10), at(180, 10), at(203, 53));
    // The same screen through a snapshot file: saved, the state cleared, loaded, drawn again.
    static khvr::PlatformScreens again;
    const char *snap = "render_test_snap.bin";
    bool snapOk = khvr::PlatformSaveSnapshotForTests(snap);
    khvr::PlatformInitForTests();
    snapOk = snapOk && khvr::PlatformLoadSnapshotForTests(snap);
    khvr::Platform::Get().CaptureScreens(&again);
    snapOk = snapOk && std::memcmp(again.top, s.top, sizeof s.top) == 0 &&
             std::memcmp(again.bottom, s.bottom, sizeof s.bottom) == 0;
    std::remove(snap);
    std::printf("snapshot test: %s\n", snapOk ? "ok" : "FAILED");
    ok = ok && snapOk;
    return ok ? 0 : 1;
}

// Renders a screen snapshot from the headset (khvr_snap_N.bin) to <prefix>_top.ppm and
// <prefix>_bottom.ppm.
int RenderSnapshot(const char *path, const char *prefix) {
    khvr::PlatformInitForTests();
    if (!khvr::PlatformLoadSnapshotForTests(path)) {
        std::fprintf(stderr, "cannot read snapshot %s\n", path);
        return 1;
    }
    static khvr::PlatformScreens s;
    khvr::Platform::Get().SetWorldCapture(true);  // the split around the 3D layer too
    khvr::Platform::Get().CaptureScreens(&s);
    WritePpm((std::string(prefix) + "_top.ppm").c_str(), s.top);
    WritePpm((std::string(prefix) + "_bottom.ppm").c_str(), s.bottom);
    if (s.split) {
        WritePpm((std::string(prefix) + "_front.ppm").c_str(), s.topFront);
        WritePpm((std::string(prefix) + "_back.ppm").c_str(), s.topBack);
        int opaque = 0;
        for (uint32_t p : s.topFront) opaque += (p >> 24) == 255;
        std::printf("snapshot: in front of the 3D layer, %d of %d pixels are opaque\n", opaque, 256 * 192);
    }
    {
        int sprites = 0;
        for (uint32_t p : s.topObj) sprites += (p >> 24) != 0;
        WritePpm((std::string(prefix) + "_obj.ppm").c_str(), s.topObj);
        std::printf("snapshot: %d top-screen pixels show sprites\n", sprites);
    }
    std::printf("snapshot: rendered %s to %s_top.ppm and %s_bottom.ppm\n", path, prefix, prefix);
    return 0;
}

// The ARM7 stand-in's answers to requests exactly as the SDK sends them.
// The 3D engine without a ROM: commands through the ports, the picture through BG0.
int GxTest() {
    khvr::PlatformInitForTests();
    auto w32 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 4); };
    auto w8 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 1); };
    w32(0x04000304, 0x8203);                 // POWCNT: engine A on top, 2D A and 3D on
    w32(0x04000000, 0x00010108);             // DISPCNT A: graphics, BG0 on, BG0 is 3D
    w32(0x04000060, 0x0001);                 // DISP3DCNT: textures on
    w32(0x04000350, 0x00000000);             // clear colour: transparent
    w32(0x04000354, 0x7fff);                 // clear depth: far
    // An 8x8 direct-colour texture in bank A (texture slot 0): left half white, right half green.
    w8(0x04000240, 0x80);                    // bank A to LCDC to fill it
    for (int t = 0; t < 8; ++t)
        for (int s = 0; s < 8; ++s) khvr::PlatformTestWrite(0x06800000 + 2 * (t * 8 + s), s < 4 ? 0xffff : 0x83e0, 2);
    w8(0x04000240, 0x83);                    // bank A: texture image slot 0
    auto cmd = [&](uint32_t c, uint32_t v) { w32(0x04000400 + 4 * c, v); };
    auto vtx = [&](double x, double y) {
        cmd(0x23, (static_cast<uint32_t>(static_cast<int16_t>(x * 4096)) & 0xffff) |
                      (static_cast<uint32_t>(static_cast<int16_t>(y * 4096)) << 16));
        cmd(0x23, 0);
    };
    auto frame = [&](uint32_t attr, bool textured) {
        cmd(0x10, 0); cmd(0x15, 0); cmd(0x10, 2); cmd(0x15, 0);
        cmd(0x60, 0xbfff0000);               // viewport 0,0 - 255,191
        cmd(0x29, attr);
        cmd(0x2a, textured ? 7u << 26 : 0);
        cmd(0x20, textured ? 0x7fff : 0x001f);   // white / red
        if (textured) {
            cmd(0x40, 1);                    // a quad, counterclockwise, texture across it
            cmd(0x22, 0); vtx(-0.5, -0.5);
            cmd(0x22, 8 * 16); vtx(0.5, -0.5);
            cmd(0x22, 8 * 16 | (8 * 16) << 16); vtx(0.5, 0.5);
            cmd(0x22, (8 * 16) << 16); vtx(-0.5, 0.5);
        } else {
            cmd(0x40, 0);                    // a triangle, counterclockwise
            vtx(-0.5, -0.5); vtx(0.5, -0.5); vtx(0.0, 0.5);
        }
        cmd(0x41, 0);
        cmd(0x50, 0);                        // SWAP_BUFFERS
    };
    static khvr::PlatformScreens s;
    auto px = [&](int x, int y) { return s.top[y * 256 + x] & 0xffffff; };
    frame(0x001f00c0, false);                // both faces
    khvr::Platform::Get().CaptureScreens(&s);
    bool tri = px(128, 100) == 0x0000ff && px(128, 30) == 0 && px(10, 100) == 0;
    frame(0x001f0040, false);                // back faces only: the front-facing triangle goes
    khvr::Platform::Get().CaptureScreens(&s);
    bool culled = px(128, 100) == 0;
    // Alpha 0: a wireframe triangle, its edges drawn (the bottom one along y 144) and its inside
    // empty; the world capture has it as three edges.
    frame(0x000000c0, false);
    khvr::Platform::Get().CaptureScreens(&s);
    const bool wire = px(128, 100) == 0 && (px(128, 143) == 0x0000ff || px(128, 144) == 0x0000ff) &&
                      (px(128, 47) == 0x0000ff || px(128, 48) == 0x0000ff);
    tri = tri && wire;
    frame(0x001f0080, true);
    khvr::Platform::Get().CaptureScreens(&s);
    bool tex = px(90, 96) == 0xffffff && px(166, 96) == 0x00ff00;
    // The same quad with the 3D layer at three times the resolution: the large picture has it,
    // the DS-size one is unchanged.
    khvr::Platform::Get().Set3dScale(3);
    frame(0x001f0080, true);
    khvr::Platform::Get().CaptureScreens(&s);
    auto hpx = [&](int x, int y) { return s.topHi[static_cast<size_t>(y) * 768 + x] & 0xffffff; };
    bool hi = s.topScale == 3 && s.topHi.size() == 768u * 576u && hpx(270, 288) == 0xffffff &&
              hpx(498, 288) == 0x00ff00 && hpx(30, 30) == 0 && px(90, 96) == 0xffffff;
    tex = tex && hi;
    if (std::getenv("KHVR_GX_BENCH") == nullptr) khvr::Platform::Get().Set3dScale(1);
    if (std::getenv("KHVR_GX_BENCH")) {
        // 1500 textured quads of about 40 x 40 pixels, as a busy field frame might have.
        cmd(0x10, 0); cmd(0x15, 0); cmd(0x10, 2); cmd(0x15, 0);
        cmd(0x29, 0x001f00c0); cmd(0x2a, 7u << 26 | 1u << 16 | 1u << 17);  // the 8x8 texture, repeated cmd(0x20, 0x7fff);
        cmd(0x40, 1);
        for (int i = 0; i < std::atoi(std::getenv("KHVR_GX_BENCH")); ++i) {
            double cx = -0.8 + 1.6 * ((i * 37) % 100) / 100.0, cy = -0.8 + 1.6 * ((i * 53) % 100) / 100.0;
            cmd(0x22, 0); vtx(cx - 0.15, cy - 0.2);
            cmd(0x22, 64 * 16); vtx(cx + 0.15, cy - 0.2);
            cmd(0x22, 64 * 16 | (64 * 16) << 16); vtx(cx + 0.15, cy + 0.2);
            cmd(0x22, (64 * 16) << 16); vtx(cx - 0.15, cy + 0.2);
        }
        cmd(0x41, 0); cmd(0x50, 0);
        auto t0 = std::chrono::steady_clock::now();
        const int iters = std::getenv("KHVR_GX_ITERS") ? std::atoi(std::getenv("KHVR_GX_ITERS")) : 20;
        for (int i = 0; i < iters; ++i) khvr::Platform::Get().CaptureScreens(&s);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / iters;
        std::printf("gx bench: %.2f ms a frame for 1500 quads\n", ms);
    }
    std::printf("gx test: %s (triangle and wireframe %d, culling %d, texture and 3x picture %d; pixels %06x %06x)\n",
                tri && culled && tex ? "ok" : "FAILED", tri, culled, tex, px(90, 96), px(166, 96));
    return tri && culled && tex ? 0 : 1;
}

// The world capture without a ROM: a camera and lens in NNS_G3dGlb, loaded the way
// NNS_G3dGlbFlush (camera in the position matrix) and NNS_G3dGlbFlushP (camera in the projection)
// load them, a textured quad moved by a model matrix, and an orthographic HUD quad. The capture
// must give the quad's corners in world space, the camera, the texture, and leave the HUD out.
int RenderWorld(const char *in, const char *outPpm);

int WorldTest() {
    khvr::PlatformInitForTests();
    auto w32 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 4); };
    auto fx = [](double v) { return static_cast<uint32_t>(static_cast<int32_t>(std::lround(v * 4096))); };
    w32(0x04000304, 0x8203);
    w32(0x04000000, 0x00010108);
    w32(0x04000060, 0x0001);
    w32(0x04000354, 0x7fff);  // clear depth: far
    khvr::PlatformTestWrite(0x04000240, 0x80, 1);
    for (int t = 0; t < 8; ++t)
        for (int x = 0; x < 8; ++x) khvr::PlatformTestWrite(0x06800000 + 2 * (t * 8 + x), x < 4 ? 0xffff : 0x83e0, 2);
    khvr::PlatformTestWrite(0x04000240, 0x83, 1);
    // Camera at (0, 2, 10) looking down -Z; a 60 degree lens, 4:3, near 1, far 100.
    const double n = 1, f = 100, cot = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0);
    double lens[16] = {cot / (4.0 / 3.0), 0, 0, 0, 0, cot, 0, 0, 0, 0, (n + f) / (n - f), -1, 0, 0, 2 * n * f / (n - f), 0};
    double cam[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -2, -10};
    const uint32_t glb = khvr::hle::SymbolAddress("NNS_G3dGlb");
    for (int i = 0; i < 16; ++i) w32(glb + 0x08 + 4 * i, fx(lens[i]));
    for (int i = 0; i < 12; ++i) w32(glb + 0x4c + 4 * i, fx(cam[i]));
    auto cmd = [&](uint32_t c, uint32_t v) { w32(0x04000400 + 4 * c, v); };
    auto vtx = [&](double x, double y) {
        cmd(0x23, (fx(x) & 0xffff) | (fx(y) << 16));
        cmd(0x23, 0);
    };
    auto quad = [&](uint32_t attr = 0x001f00c0) {
        cmd(0x29, attr);
        cmd(0x2a, 7u << 26);
        cmd(0x20, 0x7fff);
        cmd(0x40, 1);
        cmd(0x22, 0); vtx(-1, -1);
        cmd(0x22, 8 * 16); vtx(1, -1);
        cmd(0x22, 8 * 16 | (8 * 16) << 16); vtx(1, 1);
        cmd(0x22, (8 * 16) << 16); vtx(-1, 1);
        cmd(0x41, 0);
    };
    auto check = [&](bool cameraInProjection) {
        cmd(0x10, 0);
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
        if (cameraInProjection) {
            for (int i = 0; i < 12; ++i) cmd(0x19, fx(cam[i]));
            cmd(0x10, 2);
            cmd(0x15, 0);
        } else {
            cmd(0x10, 2);
            for (int i = 0; i < 12; ++i) cmd(0x17, fx(cam[i]));
        }
        cmd(0x1c, fx(5)); cmd(0x1c, 0); cmd(0x1c, 0);  // the model at x = 5
        quad();
        // A HUD quad with an orthographic projection.
        cmd(0x10, 0);
        double ortho[16] = {0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, -0.1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(ortho[i]));
        cmd(0x10, 2);
        cmd(0x15, 0);
        quad();
        // The game sets up the next frame's camera (turned and moved) before it swaps: the capture
        // keeps the camera the quads were drawn with.
        const double next[12] = {0.8, 0, -0.6, 0, 1, 0, 0.6, 0, 0.8, 3, -2, -10};
        for (int i = 0; i < 12; ++i) w32(glb + 0x4c + 4 * i, fx(next[i]));
        cmd(0x50, 0);
        for (int i = 0; i < 12; ++i) w32(glb + 0x4c + 4 * i, fx(cam[i]));
        static khvr::PlatformWorld w;
        khvr::Platform::Get().CaptureWorld(&w);
        auto near = [](double a, double b) { return std::fabs(a - b) < 0.02; };
        bool ok = w.vertices.size() == 6 && w.textures.size() == 1 && w.polygonsLeftOut == 1 && w.hasCamera;
        if (ok) {
            const khvr::PlatformWorldVertex &a = w.vertices[0], &c = w.vertices[2];
            ok = near(a.x, 4) && near(a.y, -1) && near(a.z, 0) && near(c.x, 6) && near(c.y, 1) && near(c.u, 1 - 0.5f / w.textures[0].width) &&
                 near(c.v, 1 - 0.5f / w.textures[0].height) &&  // inset half a texel (not tiled) near(w.eye[0], 0) && near(w.eye[1], 2) && near(w.eye[2], 10) && near(w.target[2], 9) &&
                 near(w.fovY, 3.14159265 / 3) && near(w.nearZ, 1) && std::fabs(w.farZ - 100) < 0.5 &&
                 (w.textures[0].rgba[0] & 0xffffff) == 0xffffff && (w.textures[0].rgba[7] & 0xffffff) == 0x00ff00 &&
                 w.triangleFlags[0] == (khvr::kWorldFront | khvr::kWorldBack) && near(w.lens[5], cot) &&
                 near(w.lens[11], -1) && near(w.view[13], -2) && near(w.view[14], -10);
            // The world corner through the captured view and lens (OpenGL column-major) lands where
            // the DS drew it: through the original camera and lens.
            auto clip = [](const float *m, const float *v, float *out) {
                for (int r = 0; r < 4; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
            };
            float p[4] = {a.x, a.y, a.z, 1}, viewSpace[4], c1[4];
            clip(w.view, p, viewSpace);
            clip(w.lens, viewSpace, c1);
            const double ex = cot / (4.0 / 3.0) * 4 / 10, ey = cot * (-3) / 10;  // (4, -3, -10) in view space
            ok = ok && near(c1[0] / c1[3], ex) && near(c1[1] / c1[3], ey);
            std::printf("  %s: corner (%.2f %.2f %.2f), eye (%.2f %.2f %.2f), fov %.3f, near %.2f, far %.1f\n",
                        cameraInProjection ? "camera in projection" : "camera in position", a.x, a.y, a.z, w.eye[0],
                        w.eye[1], w.eye[2], w.fovY, w.nearZ, w.farZ);
        } else {
            std::printf("  %zu vertices, %zu textures, %u left out\n", w.vertices.size(), w.textures.size(),
                        w.polygonsLeftOut);
        }
        return ok;
    };
    bool a = check(false), b = check(true);
    // A capture survives the file round trip, and the desktop draws it.
    {
        static khvr::PlatformWorld w, back;
        khvr::Platform::Get().CaptureWorld(&w);
        b = b && khvr::PlatformWorldWrite("khvr_world_test.bin", w) && khvr::PlatformWorldRead("khvr_world_test.bin", &back) &&
            back.vertices.size() == w.vertices.size() && back.textures.size() == w.textures.size() &&
            back.lens[5] == w.lens[5] && back.textures[0].rgba == w.textures[0].rgba &&
            RenderWorld("khvr_world_test.bin", "khvr_world_test.ppm") == 0;
    }
    // Around the captured world: the HUD quad (orthographic) in front, the backdrop behind.
    // The rear plane opaque blue, as the field has it opaque black: it must end up behind the world,
    // never in front of it.
    w32(0x04000350, 0x001f7c00);
    khvr::Platform::Get().SetWorldCapture(true);
    static khvr::PlatformScreens s;
    khvr::Platform::Get().CaptureScreens(&s);
    const int mid = 96 * 256 + 128, corner = 4 * 256 + 4;
    bool split = s.split && (s.topFront[mid] >> 24) == 255 && (s.topFront[corner] >> 24) == 0 &&
                 s.topBack[corner] == 0xffff0000u;
    // BG0's horizontal scroll moves the 3D layer: by -16 (0x1f0) the world lands 16 pixels right,
    // and so does the HUD quad in the 2D picture.
    bool scrolled = false;
    {
        static khvr::PlatformWorld w0, w1;
        static khvr::PlatformScreens s1;
        khvr::Platform::Get().CaptureWorld(&w0);
        khvr::PlatformTestWrite(0x04000010, 0x1f0, 2);
        khvr::Platform::Get().CaptureWorld(&w1);
        khvr::Platform::Get().CaptureScreens(&s1);
        khvr::PlatformTestWrite(0x04000010, 0, 2);
        int firstFront = -1, firstFrontShifted = -1;
        for (int x = 0; x < 256 && (firstFront < 0 || firstFrontShifted < 0); ++x) {
            if (firstFront < 0 && (s.topFront[96 * 256 + x] >> 24) == 255) firstFront = x;
            if (firstFrontShifted < 0 && (s1.topFront[96 * 256 + x] >> 24) == 255) firstFrontShifted = x;
        }
        scrolled = w1.viewport[0] == w0.viewport[0] + 16 && w1.viewport[2] == w0.viewport[2] + 16 && firstFront >= 0 &&
                   firstFrontShifted == firstFront + 16;
    }
    // An event scene sets its projection without NNS_G3dGlb's lens: with that lens changed, the
    // scene is still captured (in camera space, through its own projection), lands where the DS
    // drew it, and the orthographic HUD quad still stays in the DS picture.
    bool event = false;
    {
        // The lens the field left behind: another perspective (near 2, far 40).
        double stale[16];
        std::copy(lens, lens + 16, stale);
        stale[10] = (2.0 + 40) / (2.0 - 40);
        stale[14] = 2 * 2.0 * 40 / (2.0 - 40);
        for (int i = 0; i < 16; ++i) w32(glb + 0x08 + 4 * i, fx(stale[i]));
        cmd(0x10, 0);
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
        cmd(0x10, 2);
        for (int i = 0; i < 12; ++i) cmd(0x17, fx(cam[i]));
        cmd(0x1c, fx(5)); cmd(0x1c, 0); cmd(0x1c, 0);
        quad();
        cmd(0x10, 0);
        double ortho[16] = {0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, -0.1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(ortho[i]));
        cmd(0x10, 2);
        cmd(0x15, 0);
        quad();
        cmd(0x50, 0);
        static khvr::PlatformWorld w;
        khvr::Platform::Get().CaptureWorld(&w);
        if (w.vertices.size() == 6 && w.polygonsLeftOut == 1 && w.hasCamera) {
            auto clip = [](const float *m, const float *v, float *out) {
                for (int r = 0; r < 4; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
            };
            const khvr::PlatformWorldVertex &c0 = w.vertices[0];
            float p[4] = {c0.x, c0.y, c0.z, 1}, viewSpace[4], c1[4];
            clip(w.view, p, viewSpace);
            clip(w.lens, viewSpace, c1);
            const double ex = cot / (4.0 / 3.0) * 4 / 10, ey = cot * (-3) / 10;
            // Placed in the world with the camera the event drives (NNS_G3dGlb's): the quad's
            // corner where it is, (4, -1, 0).
            event = std::fabs(c1[0] / c1[3] - ex) < 0.02 && std::fabs(c1[1] / c1[3] - ey) < 0.02 && w.worldSpace &&
                    w.eventScene && std::fabs(c0.x - 4) < 0.02 && std::fabs(c0.y + 1) < 0.02 && std::fabs(c0.z) < 0.02;
            std::printf("  event scene: corner (%.2f %.2f %.2f), world space %d\n", c0.x, c0.y, c0.z, w.worldSpace);
        }
        // The event folds its camera into its projection: the polygons come in world space.
        {
            cmd(0x10, 0);
            for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
            for (int i = 0; i < 12; ++i) cmd(0x19, fx(cam[i]));
            cmd(0x10, 2);
            cmd(0x15, 0);
            cmd(0x1c, fx(5)); cmd(0x1c, 0); cmd(0x1c, 0);
            quad();
            cmd(0x50, 0);
            static khvr::PlatformWorld w;
            khvr::Platform::Get().CaptureWorld(&w);
            const bool folded = w.vertices.size() == 6 && w.worldSpace && std::fabs(w.vertices[0].x - 4) < 0.02 &&
                                std::fabs(w.vertices[0].y + 1) < 0.02 && std::fabs(w.vertices[0].z) < 0.02;
            std::printf("  event scene, camera in its projection: %zu vertices, corner (%.2f %.2f %.2f), world space %d\n",
                        w.vertices.size(), w.vertices.empty() ? 0.0 : w.vertices[0].x,
                        w.vertices.empty() ? 0.0 : w.vertices[0].y, w.vertices.empty() ? 0.0 : w.vertices[0].z,
                        w.worldSpace);
            event = event && folded;
        }
        for (int i = 0; i < 16; ++i) w32(glb + 0x08 + 4 * i, fx(lens[i]));
    }
    // A wireframe quad (alpha 0) is captured as its four edges.
    bool wire = false;
    {
        cmd(0x10, 0);
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
        cmd(0x10, 2);
        for (int i = 0; i < 12; ++i) cmd(0x17, fx(cam[i]));
        quad(0x000000c0);
        cmd(0x50, 0);
        static khvr::PlatformWorld w;
        khvr::Platform::Get().CaptureWorld(&w);
        int lines = 0;
        for (uint8_t f : w.triangleFlags) lines += (f & khvr::kWorldLine) != 0;
        wire = lines == 4 && w.triangleFlags.size() == 4 && (w.vertices[0].color >> 24) == 255;
    }
    // Sprites: a small quad square to the camera is one (its centre and offsets kept), the same
    // quad turned is not.
    bool sprites = false;
    {
        auto small = [&](double turn, uint32_t attr = 0x001000c0) {  // see-through (alpha 16)
            cmd(0x10, 0);
            for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
            cmd(0x10, 2);
            for (int i = 0; i < 12; ++i) cmd(0x17, fx(cam[i]));
            cmd(0x1c, fx(1)); cmd(0x1c, fx(2)); cmd(0x1c, 0);  // centred at (1, 2, 0)
            const double c = std::cos(turn), s = std::sin(turn);
            for (double k : {0.25}) {
                cmd(0x1b, fx(k)); cmd(0x1b, fx(k)); cmd(0x1b, fx(k));
            }
            const double rot[9] = {c, 0, -s, 0, 1, 0, s, 0, c};
            for (int i = 0; i < 9; ++i) cmd(0x1a, fx(rot[i]));
            quad(attr);
            cmd(0x50, 0);
            static khvr::PlatformWorld w;
            khvr::Platform::Get().CaptureWorld(&w);
            return w;
        };
        khvr::PlatformWorld facing = small(0), turned = small(0.6);
        // The same quad solid (a face, a table top square to the camera): no sprite.
        const bool solidLeft = small(0, 0x001f00c0).billboard.empty();
        bool flagged = facing.triangleFlags.size() == 2 && (facing.triangleFlags[0] & khvr::kWorldBillboard) &&
                       (facing.triangleFlags[1] & khvr::kWorldBillboard) && facing.billboard.size() == 6 * khvr::kWorldBillboardFloats;
        if (flagged) {
            const float *b = &facing.billboard[0];  // the first corner: (-0.25, -0.25) from (1, 2, 0)
            flagged = std::fabs(b[0] - 1) < 0.02 && std::fabs(b[1] - 2) < 0.02 && std::fabs(b[2]) < 0.02 &&
                      std::fabs(b[3] + 0.25) < 0.02 && std::fabs(b[4] + 0.25) < 0.02;
        }
        sprites = flagged && turned.triangleFlags.size() == 2 && !(turned.triangleFlags[0] & khvr::kWorldBillboard) &&
                  turned.billboard.empty() && facing.billboard[5] == 1.0f && solidLeft;
        // Upright: a see-through tree (alpha 16), 4 wide and 6 tall, standing square to the camera.
        cmd(0x10, 0);
        for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
        cmd(0x10, 2);
        for (int i = 0; i < 12; ++i) cmd(0x17, fx(cam[i]));
        cmd(0x1c, fx(1)); cmd(0x1c, fx(2)); cmd(0x1c, 0);
        cmd(0x1b, fx(2)); cmd(0x1b, fx(3)); cmd(0x1b, fx(1));
        quad(0x001000c0);
        cmd(0x50, 0);
        static khvr::PlatformWorld tree;
        khvr::Platform::Get().CaptureWorld(&tree);
        bool upright = tree.triangleFlags.size() == 2 && (tree.triangleFlags[0] & khvr::kWorldBillboard) &&
                       tree.billboard.size() == 6 * khvr::kWorldBillboardFloats;
        if (upright) {
            const float *b = &tree.billboard[0];  // the first corner: (-2, -3) from (1, 2, 0)
            upright = b[5] == 2.0f && std::fabs(b[0] - 1) < 0.02 && std::fabs(b[1] - 2) < 0.02 && std::fabs(b[2]) < 0.02 &&
                      std::fabs(b[3] + 2) < 0.02 && std::fabs(b[4] + 3) < 0.02;
        }
        std::printf("  sprites: facing %d, turned away %d, upright %d (%zu triangles, flags %d, %zu floats", flagged,
                    !turned.billboard.size(), upright, tree.triangleFlags.size(),
                    tree.triangleFlags.empty() ? -1 : tree.triangleFlags[0], tree.billboard.size());
        for (size_t i = 0; i < tree.billboard.size() && i < 6; ++i) std::printf(" %.2f", tree.billboard[i]);
        std::printf(")\n");
        sprites = sprites && upright;
    }
    // The lock-on sight's target, reported while a frame is drawn, comes with that frame's
    // capture (and not with the next one's when the game no longer reports it).
    bool lockOn = false;
    {
        const uint32_t at = 0x02300000;
        w32(at, fx(1.5)); w32(at + 4, fx(0.75)); w32(at + 8, fx(-2));
        khvr::hle::GxNoteLockOn(at, 2, true);
        quad();
        cmd(0x50, 0);
        static khvr::PlatformWorld w, after;
        khvr::Platform::Get().CaptureWorld(&w);
        quad();
        cmd(0x50, 0);
        khvr::Platform::Get().CaptureWorld(&after);
        lockOn = w.lockOn && std::fabs(w.lockOnAt[0] - 1.5f) < 1e-3f && std::fabs(w.lockOnAt[1] - 0.75f) < 1e-3f &&
                 std::fabs(w.lockOnAt[2] + 2) < 1e-3f && w.lockOnKind == 2 && w.lockOnLocked &&
                 !after.lockOn;
    }
    // A HUD piece drawn in 3D in front of the camera: where the world moves across the view as the
    // camera turns, it stays put; it is kept out of the world from then on.
    bool hudPiece = false;
    {
        auto frame = [&](double turn, khvr::PlatformWorld *w, double glbTurn = -1) {
            const double c = std::cos(turn), s = std::sin(turn);
            const double turned[12] = {c, 0, -s, 0, 1, 0, s, 0, c, 0, -2, -10};
            const double gc = std::cos(glbTurn < 0 ? turn : glbTurn), gs = std::sin(glbTurn < 0 ? turn : glbTurn);
            const double glbCam[12] = {gc, 0, -gs, 0, 1, 0, gs, 0, gc, 0, -2, -10};
            for (int i = 0; i < 12; ++i) w32(glb + 0x4c + 4 * i, fx(glbCam[i]));
            cmd(0x10, 0);
            for (int i = 0; i < 16; ++i) cmd(0x16, fx(lens[i]));
            cmd(0x10, 2);
            for (int i = 0; i < 12; ++i) cmd(0x17, fx(turned[i]));
            cmd(0x11, 0);  // MTX_PUSH the camera
            for (int q = 0; q < 8; ++q) {  // the world: a row of quads
                cmd(0x12, 1);  // MTX_POP: back to the camera
                cmd(0x11, 0);  // MTX_PUSH
                cmd(0x1c, fx(q - 4)); cmd(0x1c, 0); cmd(0x1c, 0);
                quad();
            }
            cmd(0x15, 0);  // the HUD piece: in camera space, 5 in front
            cmd(0x1c, fx(-0.5)); cmd(0x1c, fx(0.5)); cmd(0x1c, fx(-5));
            cmd(0x1b, fx(0.25)); cmd(0x1b, fx(0.25)); cmd(0x1b, fx(1));
            quad(0x001000c0);  // HUD art: see-through
            cmd(0x50, 0);
            khvr::Platform::Get().CaptureWorld(w);
        };
        static khvr::PlatformWorld w0, w1, w2, x0, x1;
        // The game's camera variable changes while what it draws stays put (a slight turn rounded
        // away, a menu over the field): nothing is taken for a HUD piece.
        frame(0.3, &x0);
        frame(0.3, &x1, 0.35);
        frame(0.0, &w0);
        frame(0.1, &w1);
        frame(0.1, &w2);  // the camera still: it stays on the HUD
        // Then the camera turns for real (0.3 to 0): the piece that kept its place is the HUD's.
        hudPiece = x1.triangleFlags.size() == 18 && w0.triangleFlags.size() == 16 && w0.polygonsLeftOut == 1 &&
                   w1.triangleFlags.size() == 16 && w2.triangleFlags.size() == 16;
        std::printf("  HUD piece: %zu, %zu, %zu triangles in the world (%zu with the scene still)\n",
                    w0.triangleFlags.size(), w1.triangleFlags.size(), w2.triangleFlags.size(), x1.triangleFlags.size());
        for (int i = 0; i < 12; ++i) w32(glb + 0x4c + 4 * i, fx(cam[i]));
    }
    const bool all = a && b && split && scrolled && event && wire && sprites && lockOn && hudPiece;
    std::printf("world test: %s (both camera placements %d, HUD and backdrop around the world %d, BG0 scroll %d, "
                "event scene %d, wireframe %d, sprites %d, lock-on %d, HUD piece %d)\n",
                all ? "ok" : "FAILED", a && b, split, scrolled, event, wire, sprites, lockOn, hudPiece);
    return all ? 0 : 1;
}

// Draws a captured world (khvr_world_N.bin) the way Screen > Game in 3D does, on the CPU: the
// game's camera and lens, its viewport, textures (nearest), depth, alpha and fades. Prints where
// the triangles land, for a world the headset showed wrong.
int RenderWorld(const char *in, const char *outPpm) {
    static khvr::PlatformWorld w;
    if (!khvr::PlatformWorldRead(in, &w)) {
        std::fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    const int W = 512, H = 384;
    std::vector<uint32_t> px(W * H, 0xff000000u);
    std::vector<float> depth(W * H, 1e30f);
    // Viewport: the DS's 256 x 192, bottom-left origin.
    const float vx = w.viewport[0] / 256.0f, vy = w.viewport[1] / 192.0f;
    const float vw = (w.viewport[2] - w.viewport[0] + 1) / 256.0f, vh = (w.viewport[3] - w.viewport[1] + 1) / 192.0f;
    auto mul = [](const float *m, const float *v, float *o) {
        for (int r = 0; r < 4; ++r) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    };
    int behind = 0, inside = 0, outside = 0;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minW = 1e9f, maxW = -1e9f;
    const size_t tris = w.triangleTexture.size();
    // The app's three passes (renderer.cpp): opaque; the solid texels of texture-alpha triangles;
    // translucent ones blended in order. Faces as each polygon asks.
    auto draw = [&](size_t t, int pass) {
        float sx[3], sy[3], sz[3], iw[3], su[3], sv[3];
        uint32_t col[3];
        bool skip = false;
        for (int k = 0; k < 3; ++k) {
            const khvr::PlatformWorldVertex &v = w.vertices[t * 3 + k];
            float p[4] = {v.x, v.y, v.z, 1}, e[4], c[4];
            mul(w.view, p, e);
            mul(w.lens, e, c);
            minW = std::min(minW, c[3]);
            maxW = std::max(maxW, c[3]);
            if (c[3] <= 1e-6f) {
                skip = true;
                continue;
            }
            float nx = c[0] / c[3], ny = c[1] / c[3];
            minX = std::min(minX, nx); maxX = std::max(maxX, nx);
            minY = std::min(minY, ny); maxY = std::max(maxY, ny);
            sx[k] = ((nx * 0.5f + 0.5f) * vw + vx) * W;
            sy[k] = (1.0f - ((ny * 0.5f + 0.5f) * vh + vy)) * H;
            sz[k] = c[2] / c[3];
            iw[k] = 1.0f / c[3];
            su[k] = v.u * iw[k];
            sv[k] = v.v * iw[k];
            col[k] = v.color;
        }
        if (skip) {
            if (pass == 0) ++behind;
            return;
        }
        bool in = false;
        for (int k = 0; k < 3; ++k) in |= sx[k] >= 0 && sx[k] < W && sy[k] >= 0 && sy[k] < H;
        if (pass == 0) (in ? inside : outside)++;
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-6f) return;
        // Screen y runs down: counterclockwise with y up is a negative area here.
        const uint8_t fl = w.triangleFlags[t];
        const bool front = area < 0;
        if (!((fl >> (front ? 0 : 1)) & 1)) return;
        const bool translucentTri = fl & khvr::kWorldTranslucent;
        if (pass == 0 && translucentTri) return;
        if (pass == 1 && !(fl & khvr::kWorldTexAlpha)) return;
        if (pass == 2 && !translucentTri) return;
        const float cut = pass == 1 ? 0.99f : 0.02f;
        const int x0 = std::max(0, static_cast<int>(std::min({sx[0], sx[1], sx[2]})));
        const int x1 = std::min(W - 1, static_cast<int>(std::max({sx[0], sx[1], sx[2]})) + 1);
        const int y0 = std::max(0, static_cast<int>(std::min({sy[0], sy[1], sy[2]})));
        const int y1 = std::min(H - 1, static_cast<int>(std::max({sy[0], sy[1], sy[2]})) + 1);
        const int32_t tex = w.triangleTexture[t];
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float qx = x + 0.5f, qy = y + 0.5f;
                float b0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
                float b1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
                float b2 = 1 - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                float z = b0 * sz[0] + b1 * sz[1] + b2 * sz[2];
                if (z < -1 || z > 1 || z > depth[y * W + x]) continue;
                float ww = 1.0f / (b0 * iw[0] + b1 * iw[1] + b2 * iw[2]);
                float c[4];
                for (int ch = 0; ch < 4; ++ch)
                    c[ch] = (b0 * ((col[0] >> (8 * ch)) & 255) + b1 * ((col[1] >> (8 * ch)) & 255) +
                             b2 * ((col[2] >> (8 * ch)) & 255)) / 255.0f;
                if (tex >= 0) {
                    const khvr::PlatformWorldTexture &tx = w.textures[tex];
                    float u = (b0 * su[0] + b1 * su[1] + b2 * su[2]) * ww, v = (b0 * sv[0] + b1 * sv[1] + b2 * sv[2]) * ww;
                    int iu = static_cast<int>(std::floor(u * tx.width)), iv = static_cast<int>(std::floor(v * tx.height));
                    iu = ((iu % tx.width) + tx.width) % tx.width;
                    iv = ((iv % tx.height) + tx.height) % tx.height;
                    uint32_t t4 = tx.rgba[static_cast<size_t>(iv) * tx.width + iu];
                    for (int ch = 0; ch < 4; ++ch) c[ch] *= ((t4 >> (8 * ch)) & 255) / 255.0f;
                }
                if (c[3] < cut) continue;
                for (int f = 0; f < 2; ++f) {
                    if (!w.fadeMode[f]) continue;
                    float k = w.fadeFactor[f] / 16.0f, to = w.fadeMode[f] == 1 ? 1.0f : 0.0f;
                    for (int ch = 0; ch < 3; ++ch) c[ch] += (to - c[ch]) * k;
                }
                uint32_t &d = px[y * W + x];
                float a = pass == 2 ? c[3] : 1.0f;
                uint32_t o = 0xff000000u;
                for (int ch = 0; ch < 3; ++ch) {
                    float old = ((d >> (8 * ch)) & 255) / 255.0f;
                    o |= static_cast<uint32_t>(std::clamp(c[ch] * a + old * (1 - a), 0.0f, 1.0f) * 255) << (8 * ch);
                }
                d = o;
                if (pass < 2 || (fl & khvr::kWorldDepthWrite)) depth[y * W + x] = z;
            }
    };
    for (int pass = 0; pass < 3; ++pass)
        for (size_t t = 0; t < tris; ++t) draw(t, pass);
    FILE *f = std::fopen(outPpm, "wb");
    if (!f) return 1;
    std::fprintf(f, "P6 %d %d 255\n", W, H);
    for (uint32_t p : px) {
        unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("world: %zu triangles (%d on screen, %d off it, %d behind the camera), %zu textures\n", tris, inside,
                outside, behind, w.textures.size());
    std::printf("  NDC x %.2f..%.2f, y %.2f..%.2f, clip w %.2f..%.2f; viewport %d %d %d %d; fades %d/%d %d/%d\n", minX,
                maxX, minY, maxY, minW, maxW, w.viewport[0], w.viewport[1], w.viewport[2], w.viewport[3], w.fadeMode[0],
                w.fadeFactor[0], w.fadeMode[1], w.fadeFactor[1]);
    std::printf("  camera eye (%.1f %.1f %.1f) target (%.1f %.1f %.1f), fov %.2f, near %.2f, far %.1f\n", w.eye[0],
                w.eye[1], w.eye[2], w.target[0], w.target[1], w.target[2], w.fovY, w.nearZ, w.farZ);
    return 0;
}

int PxiTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    auto reply = [](uint32_t tag) -> int64_t {
        while (Arm7HasReply()) {
            uint32_t w = Arm7PopReply();
            if ((w & 0x1f) == tag) return w >> 6;
        }
        return -1;
    };
    bool ok = true;
    // TP_RequestAutoSamplingStartAsync: two words, command 1 (AUTO_ON) in the start word. The
    // reply must name command 1, or TP_WaitBusy(2) on the title screen never returns.
    Arm7OnSend(6 | ((4 | 0x100 | 0x2000000) << 6));
    Arm7OnSend(6 | ((0 | 0x10000 | 0x1000000) << 6));
    int64_t r = reply(6);
    ok &= r >= 0 && (r & 0x01000000) && ((r >> 8) & 0x7f) == 1 && (r & 0xff) == 0;
    std::printf("pxi test: touch auto-sampling start -> %s (reply 0x%07llx)\n", ok ? "ok" : "WRONG",
                static_cast<long long>(r));
    // Once on, every frame brings an AUTO_SAMPLING (0x10) notification.
    Arm7VBlank();
    int64_t n = reply(6);
    bool okAuto = n >= 0 && ((n >> 8) & 0x7f) == 0x10;
    std::printf("pxi test: auto-sampling notification -> %s\n", okAuto ? "ok" : "WRONG");
    // TP_RequestAutoSamplingStopAsync: one word, command 2.
    Arm7OnSend(6 | (0x3000200u << 6));
    int64_t s = reply(6);
    bool okStop = s >= 0 && ((s >> 8) & 0x7f) == 2;
    std::printf("pxi test: touch auto-sampling stop -> %s\n", okStop ? "ok" : "WRONG");
    return ok && okAuto && okStop ? 0 : 1;
}

// The sound driver stand-in without a ROM: a 16-bit PCM square wave on channel 0 and a periodic
// alarm, set up through SNDCommand lists as the SDK sends them, then a second of frames.
int SoundTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    const uint32_t wave = 0x02100000, cmds = 0x02101000, shared = 0x02102000;
    // 400 Hz at 32768 Hz: 82 samples per period (41 high, 41 low), looped.
    const int period = 82, samples = period * 8;
    for (int i = 0; i < samples; ++i)
        khvr::PlatformTestWrite(wave + 2 * i, (i % period) < period / 2 ? 0x4000 : 0xc000, 2);
    auto cmd = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds + 0x18 * n;
        khvr::PlatformTestWrite(at, next, 4);
        khvr::PlatformTestWrite(at + 4, id, 4);
        khvr::PlatformTestWrite(at + 8, a0, 4);
        khvr::PlatformTestWrite(at + 12, a1, 4);
        khvr::PlatformTestWrite(at + 16, a2, 4);
        khvr::PlatformTestWrite(at + 20, a3, 4);
        return at;
    };
    const uint32_t timer = 512;  // the divisor, as the SDK passes it: 16756991 / 512 = 32728 Hz
    cmd(0, cmds + 0x18, 0x1d, shared, 0, 0, 0);                                    // SHARED_WORK
    cmd(1, cmds + 0x30, 0x0e, 0 | timer << 16, wave, (samples / 2) | 127u << 24,   // SETUP_CHANNEL_PCM
        0 | 64u << 16 | 1u << 24 | 1u << 26);                                      //  PCM16, loop, pan 64
    cmd(2, 0, 0x12, 3, 52366, 52366, 7);                                           // SETUP_ALARM 3, 0.1 s
    Arm7OnSend(7 | (cmds << 6));
    // Half a second between setup and start, as a stream has between its prepare and its start:
    // the alarm counts from the start, so none may come early or in a burst.
    for (int f = 0; f < 30; ++f) {
        SoundFrame();
        int16_t buf[2 * 1024];
        SoundRead(buf, 802);
    }
    int early = 0;
    while (Arm7HasReply()) early += (Arm7PopReply() & 0x1f) == 7;
    cmd(3, 0, 0x0c, 1, 0, 1u << 3, 0);                                             // START_TIMER ch 0, alarm 3
    Arm7OnSend(7 | ((cmds + 0x48) << 6));
    std::vector<int16_t> out;
    int alarms = -early;
    for (int f = 0; f < 60; ++f) {
        SoundFrame();
        while (Arm7HasReply()) {
            uint32_t w = Arm7PopReply();
            if ((w & 0x1f) == 7 && (w >> 6) == (3u | 7u << 8)) ++alarms;
        }
        int16_t buf[2 * 1024];
        size_t n = SoundRead(buf, 802);
        out.insert(out.end(), buf, buf + 2 * n);
    }
    // Zero crossings of the left channel, a second's worth: about 800 for 400 Hz. The first 0.1 s
    // is left out: the output's cushion still holds silence from before the start.
    int counted = 0;
    double sum = 0;
    const size_t from = 2 * (khvr::hle::kSoundOutputRate / 10);
    for (size_t i = from + 2; i < out.size(); i += 2) {
        if ((out[i] >= 0) != (out[i - 2] >= 0)) ++counted;
        sum += static_cast<double>(out[i]) * out[i];
    }
    const size_t measured = out.size() > from ? (out.size() - from) / 2 : 1;
    const int crossings = static_cast<int>(counted * static_cast<double>(khvr::hle::kSoundOutputRate) / measured);
    double rms = std::sqrt(sum / measured);
    bool status = khvr::PlatformTestRead(shared + 8, 2) & 1;
    bool ok = out.size() / 2 > 40000 && crossings > 760 && crossings < 840 && rms > 1000 && alarms >= 9 &&
              alarms <= 11 && status && khvr::PlatformTestRead(shared, 4) == 2;  // both batches done
    std::printf("sound test: %s (%zu frames, %d zero crossings, rms %.0f, %d alarms, channel status %d)\n",
                ok ? "ok" : "FAILED", out.size() / 2, crossings, rms, alarms, status);
    return ok ? 0 : 1;
}

// The sequence player without a ROM: a wave archive with one looped PCM16 square wave (one
// period, 82 samples: 399 Hz at its original key 60), a bank with that one instrument, and a
// sequence of two one-second notes, key 60 then 72, then its end.
// A stream as the NNS stream player runs one: a looped buffer of two segments, an alarm each
// time the channel finishes a segment, and the segment just played refilled with the next data
// at that alarm. Every segment holds one level, rising: if an alarm came early, the refill would
// land in sound not played yet, and the output would step back down.
int StreamTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    const uint32_t buf = 0x02120000, cmds = 0x02124000, shared = 0x02125000;
    const uint32_t seg = 1024;  // samples per segment, PCM16
    auto fill = [&](int segment, int seq) {
        for (uint32_t i = 0; i < seg; ++i) khvr::PlatformTestWrite(buf + 2 * (segment * seg + i), seq * 450, 2);
    };
    fill(0, 0);
    fill(1, 1);
    auto cmd = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds + 0x18 * n;
        khvr::PlatformTestWrite(at, next, 4);
        khvr::PlatformTestWrite(at + 4, id, 4);
        khvr::PlatformTestWrite(at + 8, a0, 4);
        khvr::PlatformTestWrite(at + 12, a1, 4);
        khvr::PlatformTestWrite(at + 16, a2, 4);
        khvr::PlatformTestWrite(at + 20, a3, 4);
    };
    // NNS_SndStrmSetup: channel timer = t << 5, alarm = t * samples per segment (here t = 16).
    const uint32_t t = 16, period = t * seg;
    cmd(0, cmds + 0x18, 0x1d, shared, 0, 0, 0);
    cmd(1, cmds + 0x30, 0x0e, 0 | (t << 5) << 16, buf, (2 * seg / 2) | 127u << 24, 0 | 64u << 16 | 1u << 24 | 1u << 26);
    cmd(2, cmds + 0x48, 0x12, 2, period, period, 9);
    cmd(3, 0, 0x0c, 1, 0, 1u << 2, 0);
    Arm7OnSend(7 | (cmds << 6));
    int seq = 2, played = 1;  // the segment the channel plays next, and the data to put in it
    std::vector<int16_t> out;
    auto respond = [&] {
        while (Arm7HasReply()) {
            uint32_t w = Arm7PopReply();
            if ((w & 0x1f) != 7 || (w >> 6) != (2u | 9u << 8)) continue;
            // The channel just finished segment `played ^ 1`... it now plays `played`: refill the other.
            fill(played ^ 1, seq++);
            played ^= 1;
        }
    };
    for (int f = 0; f < 120; ++f) {
        SoundFrame(respond);
        int16_t b[2 * 1024];
        size_t n = SoundRead(b, 802);
        out.insert(out.end(), b, b + 2 * n);
    }
    // Levels only rise (interpolation blends each step over one sample, never back).
    int drops = 0, last = -1, steps = 0;
    for (size_t i = 0; i < out.size(); i += 2) {
        int v = out[i];
        if (last >= 0 && v < last - 20) ++drops;
        if (v > last + 20) ++steps;
        if (std::getenv("KHVR_STREAM_TRACE") && (v > last + 20 || v < last - 20) && i < 60000)
            std::printf("  sample %zu: %d -> %d\n", i / 2, last, v);
        last = v;
    }
    bool ok = drops == 0 && seq > 40 && steps > 20;
    std::printf("stream test: %s (%d refills, %d level steps, %d steps back)\n", ok ? "ok" : "FAILED", seq - 2,
                steps, drops);
    return ok ? 0 : 1;
}

int SeqTest() {
    using namespace khvr::hle;
    khvr::PlatformInitForTests();
    Arm7Reset();
    auto w8 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 1); };
    auto w16 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 2); };
    auto w32 = [](uint32_t a, uint32_t v) { khvr::PlatformTestWrite(a, v, 4); };
    const uint32_t arc = 0x02110000, bank = 0x02112000, seq = 0x02114000, cmds = 0x02116000, shared = 0x02117000;
    w32(arc + 0x38, 1);                 // SWAR: one wave
    w32(arc + 0x3c, 0x40);              //  at +0x40
    const uint32_t wav = arc + 0x40;    // SWAV: PCM16, looped, 32768 Hz, timer 512, loop 0..41 words
    w8(wav, 1); w8(wav + 1, 1); w16(wav + 2, 32768); w16(wav + 4, 512); w16(wav + 6, 0); w32(wav + 8, 41);
    for (int i = 0; i < 82; ++i) w16(wav + 12 + 2 * i, i < 41 ? 0x4000 : 0xc000);
    w32(bank + 0x18, arc);              // SBNK: wave archive 0
    w32(bank + 0x38, 1);                //  one instrument: PCM at +0x40
    w32(bank + 0x3c, 1 | 0x40 << 8);
    const uint8_t inst[10] = {0, 0, 0, 0, 60, 127, 127, 127, 127, 64};  // wave 0 of arc 0, key 60, ADSR, pan
    for (int i = 0; i < 10; ++i) w8(bank + 0x40 + i, inst[i]);
    const uint8_t code[] = {0xfe, 0x01, 0x00, 0xc1, 0x7f, 60, 0x7f, 0x60, 72, 0x7f, 0x60, 0xff};
    for (size_t i = 0; i < sizeof code; ++i) w8(seq + static_cast<uint32_t>(i), code[i]);
    auto cmd = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds + 0x18 * n;
        w32(at, next); w32(at + 4, id); w32(at + 8, a0); w32(at + 12, a1); w32(at + 16, a2); w32(at + 20, a3);
    };
    cmd(0, cmds + 0x18, 0x1d, shared, 0, 0, 0);  // SHARED_WORK
    cmd(1, 0, 0x00, 0, seq, 0, bank);            // START_SEQ player 0
    Arm7OnSend(7 | (cmds << 6));
    std::vector<int16_t> out;
    bool statusDuring = false;
    for (int f = 0; f < 180; ++f) {
        SoundFrame();
        while (Arm7HasReply()) Arm7PopReply();
        if (f == 30) statusDuring = khvr::PlatformTestRead(shared + 4, 4) & 1;
        int16_t buf[2 * 1024];
        size_t n = SoundRead(buf, 802);
        out.insert(out.end(), buf, buf + 2 * n);
    }
    auto crossings = [&](size_t from, size_t to) {  // per second, left channel, in output frames
        int c = 0;
        for (size_t i = from + 1; i < to && i < out.size() / 2; ++i)
            if ((out[2 * i] >= 0) != (out[2 * i - 2] >= 0)) ++c;
        return c * 48000.0 / (to - from);
    };
    double first = crossings(4800, 43200), second = crossings(52800, 91200), third = crossings(110000, 140000);
    bool statusAfter = khvr::PlatformTestRead(shared + 4, 4) & 1;
    // A sequence that allocates a second track and opens it later: muting that track between
    // PREPARE_SEQ and START_PREPARED_SEQ (as the game's sound effects set their tracks up) keeps
    // it silent, so the track must exist from the moment the sequence is prepared.
    const uint32_t seq2 = 0x02115000, cmds2 = 0x02116800;
    const uint8_t code2[] = {0xfe, 0x03, 0x00, 0x93, 0x01, 0x10, 0x00, 0x00, 0x80, 0x81, 0x00, 0xff};
    for (size_t i = 0; i < sizeof code2; ++i) w8(seq2 + static_cast<uint32_t>(i), code2[i]);
    const uint8_t track1[] = {0xc1, 0x7f, 60, 0x7f, 0x60, 0xff};
    for (size_t i = 0; i < sizeof track1; ++i) w8(seq2 + 0x10 + static_cast<uint32_t>(i), track1[i]);
    auto cmd2 = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds2 + 0x18 * n;
        w32(at, next); w32(at + 4, id); w32(at + 8, a0); w32(at + 12, a1); w32(at + 16, a2); w32(at + 20, a3);
    };
    cmd2(0, cmds2 + 0x18, 0x02, 1, seq2, 0, bank);  // PREPARE_SEQ player 1
    cmd2(1, cmds2 + 0x30, 0x08, 1, 0x2, 1, 0);       // MUTE_TRACK player 1, track 1
    cmd2(2, 0, 0x03, 1, 0, 0, 0);                    // START_PREPARED_SEQ player 1
    Arm7OnSend(7 | (cmds2 << 6));
    out.clear();
    for (int f = 0; f < 40; ++f) {
        SoundFrame();
        while (Arm7HasReply()) Arm7PopReply();
        int16_t buf[2 * 1024];
        size_t n = SoundRead(buf, 802);
        out.insert(out.end(), buf, buf + 2 * n);
    }
    double muted = crossings(4800, 28800);
    // A note with no length (0) on a one-shot wave, as the game's sound effects are: it sounds
    // until the wave ends (here half a second of square wave), not just for its first frame.
    const uint32_t arc3 = 0x02119000, seq3 = 0x02115400, cmds3 = 0x02116c00, bank3 = 0x02113000;
    const uint32_t shot = arc3 + 0x40;
    const int shotWords = 8192;  // 16384 PCM16 samples at 32768 Hz
    w32(arc3 + 0x38, 1);
    w32(arc3 + 0x3c, 0x40);
    w8(shot, 1); w8(shot + 1, 0); w16(shot + 2, 32768); w16(shot + 4, 512); w16(shot + 6, 0); w32(shot + 8, shotWords);
    for (int i = 0; i < 2 * shotWords; ++i) w16(shot + 12 + 2 * i, (i / 20) % 2 ? 0x4000 : 0xc000);
    w32(bank3 + 0x18, arc3);
    w32(bank3 + 0x38, 1);
    w32(bank3 + 0x3c, 1 | 0x40 << 8);
    const uint8_t inst3[10] = {0, 0, 0, 0, 60, 127, 127, 127, 127, 64};  // wave 0 of its archive
    for (int i = 0; i < 10; ++i) w8(bank3 + 0x40 + i, inst3[i]);
    const uint8_t code3[] = {0x3c, 0x7f, 0x00, 0xff};  // key 60, length 0, then the end at once
    for (size_t i = 0; i < sizeof code3; ++i) w8(seq3 + static_cast<uint32_t>(i), code3[i]);
    auto cmd3 = [&](int n, uint32_t next, uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
        uint32_t at = cmds3 + 0x18 * n;
        w32(at, next); w32(at + 4, id); w32(at + 8, a0); w32(at + 12, a1); w32(at + 16, a2); w32(at + 20, a3);
    };
    cmd3(0, 0, 0x00, 2, seq3, 0, bank3);  // START_SEQ player 2
    Arm7OnSend(7 | (cmds3 << 6));
    out.clear();
    for (int f = 0; f < 40; ++f) {
        SoundFrame();
        while (Arm7HasReply()) Arm7PopReply();
        int16_t buf[2 * 1024];
        size_t n = SoundRead(buf, 802);
        out.insert(out.end(), buf, buf + 2 * n);
    }
    double oneShot = crossings(4800, 19200);
    bool ok = first > 760 && first < 840 && second > 1520 && second < 1680 && third < 20 && statusDuring && !statusAfter &&
              muted < 20 && oneShot > 700;
    std::printf("seq test: %s (%.0f, %.0f, %.0f zero crossings a second; player status %d during, %d after; "
                "muted track before start %.0f; one-shot note without a length %.0f)\n",
                ok ? "ok" : "FAILED", first, second, third, statusDuring, statusAfter, muted, oneShot);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "--render-test") return RenderTest(argc >= 3 ? argv[2] : nullptr);
    if (argc >= 2 && std::string(argv[1]) == "--pxi-test") return PxiTest();
    if (argc >= 2 && std::string(argv[1]) == "--gx-test") return GxTest();
    if (argc >= 2 && std::string(argv[1]) == "--world-test") return WorldTest();
    if (argc >= 2 && std::string(argv[1]) == "--sound-test") return SoundTest();
    if (argc >= 2 && std::string(argv[1]) == "--seq-test") return SeqTest();
    if (argc >= 2 && std::string(argv[1]) == "--stream-test") return StreamTest();
    if (argc >= 4 && std::string(argv[1]) == "--render-snapshot") return RenderSnapshot(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "--render-world") return RenderWorld(argv[2], argv[3]);
    khvr::PlatformConfig cfg;
    int frames = 600;
    std::string screens;
    bool restart = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--rom") cfg.romPath = next();
        else if (a == "--fake-rom") cfg.romPath = WriteFakeRom("khdays_fake.nds");
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--save") cfg.savePath = next();
        else if (a == "--log") cfg.logPath = next();
        else if (a == "--file-list") cfg.fileListPath = next();
        else if (a == "--language") cfg.language = std::atoi(next().c_str());
        else if (a == "--screens") screens = next();
        else if (a == "--restart") restart = true;  // run, reset as "Return to title" does, run again
        else {
            std::fprintf(stderr, "usage: %s --rom game.nds | --fake-rom [--frames N] [--save f] [--log f]\n", argv[0]);
            return 2;
        }
    }
    if (cfg.romPath.empty()) {
        std::fprintf(stderr, "no ROM: pass --rom <your EU ROM> or --fake-rom\n");
        return 2;
    }
    if (cfg.savePath.empty()) cfg.savePath = "khdays.sav";

    khvr::Platform &p = khvr::Platform::Get();
    std::string error;
    if (!p.Start(cfg, &error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    int idleFrames = 0, busy = 0;
    auto run = [&] {
        for (int f = 0; f < frames && p.state() == khvr::PlatformState::Running; ++f) {
            if (f % 60 == 0) p.Heartbeat();
            if (f == 0 && std::getenv("KHVR_TEST_SAMPLE"))
                for (int i = 0; i < 20; ++i) {  // the watchdog's sample of a busy guest
                    usleep(500);
                    p.Heartbeat();
                }
            if (p.VBlank(200)) ++idleFrames;
            else ++busy;
        }
    };
    run();
    if (restart) {
        khvr::PlatformTestRequestReset();
        p.Restart();
        run();
    }
    std::printf("state: %s after %u frames (%d completed, %d busy)\n",
                p.state() == khvr::PlatformState::Running ? "running"
                : p.state() == khvr::PlatformState::Crashed ? "crashed" : "stopped",
                p.frames(), idleFrames, busy);
    if (p.state() == khvr::PlatformState::Crashed) std::printf("crash: %s\n", p.crashReason().c_str());
    if (!screens.empty()) {
        // Both screens as they were at the end, as <prefix>_top.ppm and <prefix>_bottom.ppm.
        static khvr::PlatformScreens s;
        p.CaptureScreens(&s);
        WritePpm((screens + "_top.ppm").c_str(), s.top);
        WritePpm((screens + "_bottom.ppm").c_str(), s.bottom);
        std::printf("screens written to %s_top.ppm / _bottom.ppm\n", screens.c_str());
    }
    std::printf("last log lines:\n");
    for (const std::string &l : p.RecentLog(25)) std::printf("  %s\n", l.c_str());
    p.Stop();
    std::fflush(stdout);
    std::_Exit(0);  // guest threads are parked; do not wait for them
}
