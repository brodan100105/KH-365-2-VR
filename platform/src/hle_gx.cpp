#include "hle_gx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "guest.h"
#include "hle.h"

namespace khvr::hle {

namespace {

// ---------------------------------------------------------------------------------------------
// Geometry engine state
// ---------------------------------------------------------------------------------------------

// Row-major 4x4, 20.12 fixed point. The DS multiplies row vectors: v' = v * M, and a new matrix
// goes in front of the current one (M = N * M).
using Mtx = std::array<int32_t, 16>;
// The same in doubles (the world capture's matrix work).
using Mat4d = std::array<double, 16>;

Mtx Identity() {
    Mtx m{};
    m[0] = m[5] = m[10] = m[15] = 0x1000;
    return m;
}

Mtx Mul(const Mtx &a, const Mtx &b) {
    Mtx r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            int64_t s = 0;
            for (int k = 0; k < 4; ++k) s += static_cast<int64_t>(a[i * 4 + k]) * b[k * 4 + j];
            r[i * 4 + j] = static_cast<int32_t>(s >> 12);
        }
    return r;
}

// A vertex in clip space (fixed-point values as floats: 4096 is 1.0), with its colour (6 bits a
// channel) and texture coordinates (texels).
struct Vertex {
    float x, y, z, w;
    float r, g, b;
    float s, t;
    float px, py, pz;  // through the position matrix only (the world capture's starting point)
};

constexpr int kMaxPolyVerts = 10;  // a quad clipped by six planes
struct Polygon {
    Vertex v[kMaxPolyVerts];
    int n = 0;
    uint32_t attr = 0, texParam = 0, palBase = 0;
    bool translucent = false;
    uint16_t proj = 0;  // the projection it was drawn with (Gx::projList)
    int32_t world = -1;  // its polygon in the world capture's list (Gx::worldList), when kept
};

constexpr size_t kMaxPolygons = 2048;

struct WorldPoly {
    Vertex v[4];
    int n = 0;
    uint32_t attr = 0, texParam = 0, palBase = 0;
    uint16_t proj = 0;  // index into the frame's projection matrices
};
constexpr int kW = 256, kH = 192;

struct Gx {
    int mtxMode = 0;
    Mtx proj = Identity(), pos = Identity(), vec = Identity(), tex = Identity();
    Mtx projStack = Identity(), texStack = Identity();
    Mtx posStack[32], vecStack[32];
    int projSp = 0, posSp = 0;
    bool stackError = false;
    Mtx clip = Identity();
    bool clipDirty = false;

    int16_t vx = 0, vy = 0, vz = 0;  // the last vertex, 4.12
    uint8_t cr = 31, cg = 31, cb = 31;  // vertex colour, 5 bits a channel
    int16_t rawS = 0, rawT = 0;  // TEXCOORD as given (12.4)
    int32_t s = 0, t = 0;        // after the texture matrix (12.4)
    int16_t normal[3] = {0, 0, 0};

    uint32_t polyAttrNext = 0, polyAttr = 0, texParam = 0, palBase = 0;
    uint32_t difAmb = 0, speEmi = 0;
    int32_t lightDir[4][3] = {};
    uint16_t lightColor[4] = {};
    uint8_t shininess[128] = {};

    int prim = 0;
    Vertex strip[4];
    int stripCount = 0;
    bool stripOdd = false;
    std::vector<Polygon> list, shown;
    // The world capture: every polygon as submitted (before the DS's culling and clipping), and
    // the projection matrices they were drawn with.
    std::vector<WorldPoly> worldList, worldShown;
    std::vector<Mtx> projList, projShown;
    uint32_t projSerial = 1, projRecorded = 0;
    std::vector<uint8_t> inSceneShown;  // per projShown: draws the scene (else a HUD piece)
    // NNS_G3dGlb's lens and camera as they were when each projection was first drawn with: the game
    // may set up the next frame's camera before it swaps, and a camera read at the swap would then
    // turn the world by the difference whenever the camera swings.
    struct GlbSnapshot { Mtx lens, camera; bool ok = false; };
    std::vector<GlbSnapshot> glbList, glbShown;
    std::vector<Mat4d> toWorldShown;  // per projShown: position-matrix space to world space
    std::vector<Mat4d> toCameraShown;  // per projShown: position-matrix space to camera space
    // HUD pieces the game draws in 3D, in front of its camera (the mission's heart counter): per
    // worldShown polygon, 1 when it is one (ClassifyHud). They stay in the DS picture and on the HUD.
    std::vector<uint8_t> hudShown;
    std::unordered_set<uint64_t> prevKeys, hudKeys, hudLooks;
    Mat4d prevCamera{};
    // The lock-on sight's target (world), as the game placed it while drawing a frame, and for the
    // shown frame.
    bool lockOnPending = false, lockOnShown = false;
    float lockOnPendingAt[3] = {}, lockOnShownAt[3] = {};
    int lockOnPendingScreen[2] = {-1, -1}, lockOnShownScreen[2] = {-1, -1};
    // The scene's lens and camera for the shown list (SceneOf), decided when it was swapped in.
    bool sceneKnown = false;
    bool sceneHasCamera = false;  // the scene's camera is the game's (not an event scene's stand-in)
    Mat4d sceneLens{}, sceneCamera{}, sceneLensInv{}, sceneCameraInv{};
    bool shownWBuffer = false, shownManualSort = false;
    std::array<int, 4> shownVp{0, 0, 255, 191};  // the viewport when the shown list was swapped in
    uint32_t vertexCount = 0;

    int vpX1 = 0, vpY1 = 0, vpX2 = 255, vpY2 = 191;

    // FIFO decoding: a packed word of up to four commands, then their parameters.
    uint8_t queue[4] = {};
    int queued = 0, queuePos = 0;
    uint32_t params[32];
    int paramCount = 0;
    // The command ports (0x440 + 4 * command): one parameter per write.
    int portCmd = -1, portCount = 0;
    uint32_t portParams[32];

    bool boxResult = false;
    int32_t posTest[4] = {};
    int16_t vecTest[3] = {};
};

Gx g;
// The shown list (and what SceneOf decided about it) is swapped in by the game's thread at
// SWAP_BUFFERS and read by the screens' drawing, which may run on another thread while the game
// runs its next frame (as the DS draws one frame while its CPU works on the next).
std::mutex gShownMu;
uint32_t gCommands = 0, gLastFrame = 0;

uint8_t ParamCount(uint8_t cmd) {
    switch (cmd) {
    case 0x10: case 0x12: case 0x13: case 0x14: return 1;
    case 0x16: case 0x18: return 16;
    case 0x17: case 0x19: return 12;
    case 0x1a: return 9;
    case 0x1b: case 0x1c: return 3;
    case 0x20: case 0x21: case 0x22: return 1;
    case 0x23: return 2;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: return 1;
    case 0x30: case 0x31: case 0x32: case 0x33: return 1;
    case 0x34: return 32;
    case 0x40: case 0x50: case 0x60: return 1;
    case 0x70: return 3;
    case 0x71: return 2;
    case 0x72: return 1;
    default: return 0;  // NOP, PUSH, IDENTITY, END_VTXS and unused codes
    }
}

int32_t Sext(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }

void UpdateClip() {
    if (!g.clipDirty) return;
    g.clip = Mul(g.pos, g.proj);
    g.clipDirty = false;
}

// The current matrix (or matrices) of the mode: a load, or N * M.
void Load(const Mtx &m) {
    switch (g.mtxMode) {
    case 0: g.proj = m; ++g.projSerial; break;
    case 1: g.pos = m; break;
    case 2: g.pos = m; g.vec = m; break;
    default: g.tex = m; break;
    }
    g.clipDirty = true;
}

void Apply(const Mtx &m, bool vecToo) {
    switch (g.mtxMode) {
    case 0: g.proj = Mul(m, g.proj); ++g.projSerial; break;
    case 1: g.pos = Mul(m, g.pos); break;
    case 2:
        g.pos = Mul(m, g.pos);
        if (vecToo) g.vec = Mul(m, g.vec);
        break;
    default: g.tex = Mul(m, g.tex); break;
    }
    g.clipDirty = true;
}


Mat4d ToDouble(const Mtx &m) {
    Mat4d d;
    for (int i = 0; i < 16; ++i) d[i] = m[i] / 4096.0;
    return d;
}

Mat4d MulD(const Mat4d &a, const Mat4d &b) {
    Mat4d r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

bool Invert(const Mat4d &m, Mat4d *out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
        double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) (*out)[i * 4 + j] = a[i][j + 4];
    return true;
}

uint64_t Fnv(const void *data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Whether a projection draws the game's scene: what it holds beyond NNS_G3dGlb's lens (nothing, or
// the camera when the game flushed it into the projection) is affine. Otherwise it draws a flat
// piece over the scene (an orthographic HUD element). `toWorld` gets position-matrix space to
// world space. NNSG3dGlb (decomp: include/nnsys/g3d.h): the lens at +0x08, the camera at +0x4c.
Gx::GlbSnapshot ReadGlb() {
    static const uint32_t glb = SymbolAddress("NNS_G3dGlb");
    Gx::GlbSnapshot s;
    if (!glb) return s;
    s.camera = Identity();
    for (int i = 0; i < 16; ++i) s.lens[i] = static_cast<int32_t>(Read32(glb + 0x08 + 4 * i));
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k) s.camera[r * 4 + k] = static_cast<int32_t>(Read32(glb + 0x4c + 4 * (r * 3 + k)));
    s.ok = true;
    return s;
}

bool InScene(const Mtx &proj, const Mat4d &lensInv) {
    Mat4d q = MulD(ToDouble(proj), lensInv);
    const double tol = 1e-3;
    return std::fabs(q[3]) < tol && std::fabs(q[7]) < tol && std::fabs(q[11]) < tol && std::fabs(q[15] - 1.0) < tol;
}

// Which projections of the shown list draw the scene, and its lens and camera. Normally the
// NNS_G3dGlb lens and camera each projection was drawn with (ReadGlb, InScene); the scene's own
// are those of the projection most of its polygons use. Event scenes (in-engine cutscenes) set
// the projection without that lens, so none matches: then the perspective projection most
// polygons use is the scene's lens, with no camera (the polygons stay in camera space). That is
// not world space, but it draws the same picture at any resolution, which is what the giant
// screen needs; standing in the world leaves event scenes on the giant screen anyway.
void SceneOf(const std::vector<Mtx> &projs, const std::vector<WorldPoly> &polys) {
    g.inSceneShown.assign(projs.size(), 0);
    g.toWorldShown.assign(projs.size(), ToDouble(Identity()));
    g.toCameraShown.assign(projs.size(), ToDouble(Identity()));
    g.sceneKnown = false;
    std::vector<uint32_t> uses(projs.size(), 0);
    for (const WorldPoly &w : polys)
        if (w.proj < uses.size()) ++uses[w.proj];
    int best = -1;
    Mat4d lens{}, camera{}, lensInv{}, cameraInv{};
    const Gx::GlbSnapshot *last = nullptr;
    bool lastOk = false;
    for (size_t i = 0; i < projs.size() && i < g.glbShown.size(); ++i) {
        const Gx::GlbSnapshot &s = g.glbShown[i];
        if (!s.ok) continue;
        if (!last || s.lens != last->lens || s.camera != last->camera) {
            last = &s;
            lens = ToDouble(s.lens);
            camera = ToDouble(s.camera);
            lastOk = Invert(lens, &lensInv) && Invert(camera, &cameraInv);
        }
        if (!lastOk || !InScene(projs[i], lensInv)) continue;
        g.inSceneShown[i] = 1;
        g.toCameraShown[i] = MulD(ToDouble(projs[i]), lensInv);
        g.toWorldShown[i] = MulD(g.toCameraShown[i], cameraInv);
        if (best < 0 || uses[i] > uses[best]) {
            best = static_cast<int>(i);
            g.sceneLens = lens;
            g.sceneCamera = camera;
            g.sceneLensInv = lensInv;
            g.sceneCameraInv = cameraInv;
        }
    }
    g.sceneHasCamera = best >= 0;
    if (best >= 0) {
        g.sceneKnown = true;
        return;
    }
    if (projs.empty()) return;
    size_t most = 0;
    for (size_t i = 1; i < uses.size(); ++i)
        if (uses[i] > uses[most]) most = i;
    const Mat4d l = ToDouble(projs[most]);
    Mat4d lInv;
    // A perspective projection: w follows z (an orthographic one draws flat HUD pieces).
    if (!uses[most] || std::fabs(l[11]) < 1e-3 || !Invert(l, &lInv)) return;
    g.sceneKnown = true;
    g.sceneLens = l;
    g.sceneLensInv = lInv;
    g.sceneCamera = ToDouble(Identity());
    g.sceneCameraInv = g.sceneCamera;
    for (size_t i = 0; i < projs.size(); ++i) {
        g.inSceneShown[i] = projs[i] == projs[most];
        if (g.inSceneShown[i]) g.toWorldShown[i] = MulD(ToDouble(projs[i]), lInv);
    }
}

// Which polygons of the shown list are HUD pieces drawn in 3D: fixed in front of the game's
// camera, they lie at the same place in camera space from frame to frame while the camera moves
// (the world's polygons move across its view then). A polygon seen exactly so once stays a HUD
// piece while it keeps that place, the camera still or not; one that changes (a counter's digit)
// is one too when its texture and depth match a HUD piece's.
void ClassifyHud(const std::vector<WorldPoly> &polys) {
    g.hudShown.assign(polys.size(), 0);
    if (!g.sceneHasCamera) {
        g.prevKeys.clear();
        g.hudKeys.clear();
        g.hudLooks.clear();
        return;
    }
    bool moved = false;
    for (int i = 0; i < 16 && !moved; ++i) moved = std::fabs(g.sceneCamera[i] - g.prevCamera[i]) > 1e-3;
    g.prevCamera = g.sceneCamera;
    std::unordered_set<uint64_t> keys, hud, looks;
    keys.reserve(polys.size());
    for (size_t k = 0; k < polys.size(); ++k) {
        const WorldPoly &w = polys[k];
        if (w.proj >= g.inSceneShown.size() || !g.inSceneShown[w.proj]) continue;
        const Mat4d &m = g.toCameraShown[w.proj];
        uint64_t key = Fnv(&w.texParam, 4, Fnv(&w.palBase, 4, Fnv(&w.attr, 4)));
        double depth = 0;
        for (int i = 0; i < w.n; ++i) {
            const double x = w.v[i].px / 4096.0, y = w.v[i].py / 4096.0, z = w.v[i].pz / 4096.0;
            const int32_t c[3] = {
                static_cast<int32_t>(std::lround((x * m[0] + y * m[4] + z * m[8] + m[12]) * 256)),
                static_cast<int32_t>(std::lround((x * m[1] + y * m[5] + z * m[9] + m[13]) * 256)),
                static_cast<int32_t>(std::lround((x * m[2] + y * m[6] + z * m[10] + m[14]) * 256))};
            depth += c[2];
            key = Fnv(c, sizeof c, key);
        }
        const int32_t look[3] = {static_cast<int32_t>(w.texParam), static_cast<int32_t>(w.palBase),
                                 static_cast<int32_t>(std::lround(depth / w.n / 16))};
        const uint64_t lookKey = Fnv(look, sizeof look);
        keys.insert(key);
        const bool same = g.prevKeys.count(key) != 0;
        const bool isHud = (same && (moved || g.hudKeys.count(key))) || (!same && g.hudLooks.count(lookKey));
        if (isHud) {
            g.hudShown[k] = 1;
            hud.insert(key);
            looks.insert(lookKey);
        }
    }
    g.prevKeys.swap(keys);
    g.hudKeys.swap(hud);
    g.hudLooks.swap(looks);
}

// ---------------------------------------------------------------------------------------------
// Polygons: culling, clipping, the polygon list
// ---------------------------------------------------------------------------------------------

Vertex Lerp(const Vertex &a, const Vertex &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t,
            a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.s + (b.s - a.s) * t, a.t + (b.t - a.t) * t,
            a.px + (b.px - a.px) * t, a.py + (b.py - a.py) * t, a.pz + (b.pz - a.pz) * t};
}

// Keeps the part of the polygon where dist >= 0 (Sutherland-Hodgman).
template <typename Dist>
int ClipPlane(const Vertex *in, int n, Vertex *out, Dist dist) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const Vertex &a = in[i], &b = in[(i + 1) % n];
        float da = dist(a), db = dist(b);
        if (da >= 0) {
            if (m < kMaxPolyVerts) out[m++] = a;
            if (db < 0 && m < kMaxPolyVerts) out[m++] = Lerp(a, b, da / (da - db));
        } else if (db >= 0 && m < kMaxPolyVerts) {
            out[m++] = Lerp(a, b, da / (da - db));
        }
    }
    return m;
}

void EmitPolygon(const Vertex *verts, int n) {
    const uint32_t attr = g.polyAttr;
    if (((attr >> 4) & 3) == 3) return;  // shadow volumes: not drawn yet
    if (g.projRecorded != g.projSerial || g.projList.empty()) {
        g.projList.push_back(g.proj);
        g.glbList.push_back(ReadGlb());
        g.projRecorded = g.projSerial;
    }
    int32_t worldIndex = -1;
    if (g.worldList.size() < 2 * kMaxPolygons && n <= 4) {
        worldIndex = static_cast<int32_t>(g.worldList.size());
        WorldPoly w;
        std::copy(verts, verts + n, w.v);
        w.n = n;
        w.attr = attr;
        w.texParam = g.texParam;
        w.palBase = g.palBase;
        w.proj = static_cast<uint16_t>(g.projList.size() - 1);
        g.worldList.push_back(w);
    }
    if (g.list.size() >= kMaxPolygons) return;

    // Facing: the sign of the homogeneous determinant of the first three vertices (CCW with y up
    // is the front, as in OpenGL).
    const Vertex &a = verts[0], &b = verts[1], &c = verts[2];
    double det = static_cast<double>(a.x) * (static_cast<double>(b.y) * c.w - static_cast<double>(c.y) * b.w) -
                 static_cast<double>(a.y) * (static_cast<double>(b.x) * c.w - static_cast<double>(c.x) * b.w) +
                 static_cast<double>(a.w) * (static_cast<double>(b.x) * c.y - static_cast<double>(c.x) * b.y);
    if (n == 4 && det == 0) {
        const Vertex &d = verts[3];
        det = static_cast<double>(a.x) * (static_cast<double>(c.y) * d.w - static_cast<double>(d.y) * c.w) -
              static_cast<double>(a.y) * (static_cast<double>(c.x) * d.w - static_cast<double>(d.x) * c.w) +
              static_cast<double>(a.w) * (static_cast<double>(c.x) * d.y - static_cast<double>(d.x) * c.y);
    }
    bool front = det > 0;
    if (det != 0 && !((attr >> (front ? 7 : 6)) & 1)) return;
    if (det == 0 && !(attr & 0xc0)) return;

    // Beyond the far plane: dropped whole unless the attribute asks to clip it.
    if (!(attr & (1u << 12)))
        for (int i = 0; i < n; ++i)
            if (verts[i].z > verts[i].w) return;

    Vertex bufA[kMaxPolyVerts], bufB[kMaxPolyVerts];
    int m = n;
    std::copy(verts, verts + n, bufA);
    m = ClipPlane(bufA, m, bufB, [](const Vertex &v) { return v.w + v.z; });
    m = ClipPlane(bufB, m, bufA, [](const Vertex &v) { return v.w - v.z; });
    m = ClipPlane(bufA, m, bufB, [](const Vertex &v) { return v.w + v.x; });
    m = ClipPlane(bufB, m, bufA, [](const Vertex &v) { return v.w - v.x; });
    m = ClipPlane(bufA, m, bufB, [](const Vertex &v) { return v.w + v.y; });
    m = ClipPlane(bufB, m, bufA, [](const Vertex &v) { return v.w - v.y; });
    if (m < 3) return;

    Polygon p;
    std::copy(bufA, bufA + m, p.v);
    p.n = m;
    p.attr = attr;
    p.texParam = g.texParam;
    p.palBase = g.palBase;
    p.proj = static_cast<uint16_t>(g.projList.size() - 1);
    p.world = worldIndex;
    uint32_t alpha = (attr >> 16) & 31;
    uint32_t format = (g.texParam >> 26) & 7;
    p.translucent = (alpha > 0 && alpha < 31) || format == 1 || format == 6;
    g.list.push_back(p);
    g.vertexCount += static_cast<uint32_t>(m);
}

void SubmitVertex() {
    UpdateClip();
    const int64_t x = g.vx, y = g.vy, z = g.vz;
    const Mtx &m = g.clip;
    Vertex v;
    v.x = static_cast<float>((x * m[0] + y * m[4] + z * m[8] + 0x1000ll * m[12]) >> 12);
    v.y = static_cast<float>((x * m[1] + y * m[5] + z * m[9] + 0x1000ll * m[13]) >> 12);
    v.z = static_cast<float>((x * m[2] + y * m[6] + z * m[10] + 0x1000ll * m[14]) >> 12);
    v.w = static_cast<float>((x * m[3] + y * m[7] + z * m[11] + 0x1000ll * m[15]) >> 12);
    if ((g.texParam >> 30) == 3) {  // texture coordinates from the vertex
        const Mtx &t = g.tex;
        g.s = static_cast<int32_t>(((x * t[0] + y * t[4] + z * t[8]) >> 24) + g.rawS);
        g.t = static_cast<int32_t>(((x * t[1] + y * t[5] + z * t[9]) >> 24) + g.rawT);
    }
    auto six = [](uint8_t c) { return static_cast<float>(c ? c * 2 + 1 : 0); };
    v.r = six(g.cr);
    v.g = six(g.cg);
    v.b = six(g.cb);
    v.s = static_cast<float>(g.s) / 16.0f;
    v.t = static_cast<float>(g.t) / 16.0f;
    const Mtx &pm = g.pos;
    v.px = static_cast<float>((x * pm[0] + y * pm[4] + z * pm[8] + 0x1000ll * pm[12]) >> 12);
    v.py = static_cast<float>((x * pm[1] + y * pm[5] + z * pm[9] + 0x1000ll * pm[13]) >> 12);
    v.pz = static_cast<float>((x * pm[2] + y * pm[6] + z * pm[10] + 0x1000ll * pm[14]) >> 12);

    Vertex *st = g.strip;
    st[g.stripCount++] = v;
    switch (g.prim) {
    case 0:  // triangles
        if (g.stripCount == 3) {
            EmitPolygon(st, 3);
            g.stripCount = 0;
        }
        break;
    case 1:  // quadrilaterals
        if (g.stripCount == 4) {
            EmitPolygon(st, 4);
            g.stripCount = 0;
        }
        break;
    case 2:  // triangle strip: every other triangle has its first two vertices swapped
        if (g.stripCount == 3) {
            if (g.stripOdd) {
                Vertex t[3] = {st[1], st[0], st[2]};
                EmitPolygon(t, 3);
            } else {
                EmitPolygon(st, 3);
            }
            g.stripOdd = !g.stripOdd;
            st[0] = st[1];
            st[1] = st[2];
            g.stripCount = 2;
        }
        break;
    default:  // quad strip: 0 1 3 2, then the next pair
        if (g.stripCount == 4) {
            Vertex q[4] = {st[0], st[1], st[3], st[2]};
            EmitPolygon(q, 4);
            st[0] = st[2];
            st[1] = st[3];
            g.stripCount = 2;
        }
        break;
    }
}

void Lighting() {
    const Mtx &m = g.vec;
    int32_t n[3];
    for (int j = 0; j < 3; ++j)
        n[j] = static_cast<int32_t>((static_cast<int64_t>(g.normal[0]) * m[j] + static_cast<int64_t>(g.normal[1]) * m[4 + j] +
                                     static_cast<int64_t>(g.normal[2]) * m[8 + j]) >> 12);
    const int dif[3] = {static_cast<int>(g.difAmb & 31), static_cast<int>((g.difAmb >> 5) & 31),
                        static_cast<int>((g.difAmb >> 10) & 31)};
    const int amb[3] = {static_cast<int>((g.difAmb >> 16) & 31), static_cast<int>((g.difAmb >> 21) & 31),
                        static_cast<int>((g.difAmb >> 26) & 31)};
    const int spe[3] = {static_cast<int>(g.speEmi & 31), static_cast<int>((g.speEmi >> 5) & 31),
                        static_cast<int>((g.speEmi >> 10) & 31)};
    int c[3] = {static_cast<int>((g.speEmi >> 16) & 31), static_cast<int>((g.speEmi >> 21) & 31),
                static_cast<int>((g.speEmi >> 26) & 31)};
    for (int i = 0; i < 4; ++i) {
        if (!((g.polyAttr >> i) & 1)) continue;
        const int32_t *l = g.lightDir[i];
        int32_t diff = -(l[0] * n[0] + l[1] * n[1] + l[2] * n[2]) >> 10;
        diff = std::clamp(diff, 0, 255);
        int32_t shine = -(((l[0] >> 1) * n[0] + (l[1] >> 1) * n[1] + ((l[2] - 0x200) >> 1) * n[2]) >> 10);
        if (shine < 0) shine = 0;
        else if (shine > 255) shine = (0x100 - shine) & 0xff;
        shine = ((shine * shine) >> 7) - 0x100;
        if (shine < 0) shine = 0;
        if (g.speEmi & 0x8000) shine = g.shininess[std::min(shine >> 1, 127)];
        const int lc[3] = {g.lightColor[i] & 31, (g.lightColor[i] >> 5) & 31, (g.lightColor[i] >> 10) & 31};
        for (int k = 0; k < 3; ++k) {
            c[k] += (spe[k] * lc[k] * shine) >> 13;
            c[k] += (dif[k] * lc[k] * diff) >> 13;
            c[k] += (amb[k] * lc[k]) >> 5;
        }
    }
    g.cr = static_cast<uint8_t>(std::min(c[0], 31));
    g.cg = static_cast<uint8_t>(std::min(c[1], 31));
    g.cb = static_cast<uint8_t>(std::min(c[2], 31));
}

bool BoxInView(const uint32_t *p);
bool BoxVisible(const uint32_t *p) {
    const bool inView = BoxInView(p);
    if (inView) ++gWorldStats.boxesInView;
    else ++gWorldStats.boxesOutOfView;
    return inView || gDrawAll;
}

bool BoxInView(const uint32_t *p) {
    UpdateClip();
    int64_t x = static_cast<int16_t>(p[0]), y = static_cast<int16_t>(p[0] >> 16);
    int64_t z = static_cast<int16_t>(p[1]), w = static_cast<int16_t>(p[1] >> 16);
    int64_t h = static_cast<int16_t>(p[2]), d = static_cast<int16_t>(p[2] >> 16);
    // Outside if all eight corners are beyond one of the six planes; conservative otherwise.
    int outside[6] = {0, 0, 0, 0, 0, 0};
    const Mtx &m = g.clip;
    for (int i = 0; i < 8; ++i) {
        int64_t cx = x + ((i & 1) ? w : 0), cy = y + ((i & 2) ? h : 0), cz = z + ((i & 4) ? d : 0);
        int64_t v[4];
        for (int j = 0; j < 4; ++j) v[j] = (cx * m[j] + cy * m[4 + j] + cz * m[8 + j] + 0x1000ll * m[12 + j]) >> 12;
        outside[0] += v[0] < -v[3];
        outside[1] += v[0] > v[3];
        outside[2] += v[1] < -v[3];
        outside[3] += v[1] > v[3];
        outside[4] += v[2] < -v[3];
        outside[5] += v[2] > v[3];
    }
    for (int k = 0; k < 6; ++k)
        if (outside[k] == 8) return false;
    return true;
}

void Exec(uint8_t cmd, const uint32_t *p) {
    ++gCommands;
    switch (cmd) {
    case 0x10: g.mtxMode = static_cast<int>(p[0] & 3); break;
    case 0x11:  // MTX_PUSH
        if (g.mtxMode == 0) {
            if (g.projSp) g.stackError = true;
            g.projStack = g.proj;
            g.projSp = 1;
        } else if (g.mtxMode == 3) {
            g.texStack = g.tex;
        } else {
            if (g.posSp >= 31) g.stackError = true;
            g.posStack[g.posSp & 31] = g.pos;
            g.vecStack[g.posSp & 31] = g.vec;
            g.posSp = std::min(g.posSp + 1, 63);
        }
        break;
    case 0x12: {  // MTX_POP
        if (g.mtxMode == 0) {
            g.projSp = 0;
            g.proj = g.projStack;
            ++g.projSerial;
        } else if (g.mtxMode == 3) {
            g.tex = g.texStack;
        } else {
            int n = Sext(p[0], 6);
            g.posSp = std::clamp(g.posSp - n, 0, 63);
            if (g.posSp >= 31) g.stackError = true;
            g.pos = g.posStack[g.posSp & 31];
            g.vec = g.vecStack[g.posSp & 31];
        }
        g.clipDirty = true;
        break;
    }
    case 0x13:  // MTX_STORE
        if (g.mtxMode == 0) g.projStack = g.proj;
        else if (g.mtxMode == 3) g.texStack = g.tex;
        else {
            uint32_t n = p[0] & 31;
            if (n == 31) g.stackError = true;
            g.posStack[n] = g.pos;
            g.vecStack[n] = g.vec;
        }
        break;
    case 0x14:  // MTX_RESTORE
        if (g.mtxMode == 0) {
            g.proj = g.projStack;
            ++g.projSerial;
        } else if (g.mtxMode == 3) {
            g.tex = g.texStack;
        } else {
            uint32_t n = p[0] & 31;
            if (n == 31) g.stackError = true;
            g.pos = g.posStack[n];
            g.vec = g.vecStack[n];
        }
        g.clipDirty = true;
        break;
    case 0x15: Load(Identity()); break;
    case 0x16: {
        Mtx m;
        for (int i = 0; i < 16; ++i) m[i] = static_cast<int32_t>(p[i]);
        Load(m);
        break;
    }
    case 0x17: {
        Mtx m = Identity();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
        m[3] = m[7] = m[11] = 0;
        m[15] = 0x1000;
        Load(m);
        break;
    }
    case 0x18: {
        Mtx m;
        for (int i = 0; i < 16; ++i) m[i] = static_cast<int32_t>(p[i]);
        Apply(m, true);
        break;
    }
    case 0x19: {
        Mtx m = Identity();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
        m[3] = m[7] = m[11] = 0;
        m[15] = 0x1000;
        Apply(m, true);
        break;
    }
    case 0x1a: {
        Mtx m = Identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]);
        Apply(m, true);
        break;
    }
    case 0x1b: {  // MTX_SCALE: never the vector matrix
        Mtx m = Identity();
        m[0] = static_cast<int32_t>(p[0]);
        m[5] = static_cast<int32_t>(p[1]);
        m[10] = static_cast<int32_t>(p[2]);
        Apply(m, false);
        break;
    }
    case 0x1c: {
        Mtx m = Identity();
        m[12] = static_cast<int32_t>(p[0]);
        m[13] = static_cast<int32_t>(p[1]);
        m[14] = static_cast<int32_t>(p[2]);
        Apply(m, true);
        break;
    }
    case 0x20:
        g.cr = p[0] & 31;
        g.cg = (p[0] >> 5) & 31;
        g.cb = (p[0] >> 10) & 31;
        break;
    case 0x21:  // NORMAL: 1.9 fixed x, y, z
        g.normal[0] = static_cast<int16_t>(Sext(p[0], 10));
        g.normal[1] = static_cast<int16_t>(Sext(p[0] >> 10, 10));
        g.normal[2] = static_cast<int16_t>(Sext(p[0] >> 20, 10));
        if ((g.texParam >> 30) == 2) {
            const Mtx &t = g.tex;
            int64_t nx = g.normal[0], ny = g.normal[1], nz = g.normal[2];
            g.s = static_cast<int32_t>(((nx * t[0] + ny * t[4] + nz * t[8]) >> 21) + g.rawS);
            g.t = static_cast<int32_t>(((nx * t[1] + ny * t[5] + nz * t[9]) >> 21) + g.rawT);
        }
        Lighting();
        break;
    case 0x22:
        g.rawS = static_cast<int16_t>(p[0]);
        g.rawT = static_cast<int16_t>(p[0] >> 16);
        if ((g.texParam >> 30) == 1) {
            const Mtx &t = g.tex;
            int64_t s = g.rawS, tt = g.rawT;
            g.s = static_cast<int32_t>((s * t[0] + tt * t[4] + t[8] + t[12]) >> 12);
            g.t = static_cast<int32_t>((s * t[1] + tt * t[5] + t[9] + t[13]) >> 12);
        } else {
            g.s = g.rawS;
            g.t = g.rawT;
        }
        break;
    case 0x23:
        g.vx = static_cast<int16_t>(p[0]);
        g.vy = static_cast<int16_t>(p[0] >> 16);
        g.vz = static_cast<int16_t>(p[1]);
        SubmitVertex();
        break;
    case 0x24:
        g.vx = static_cast<int16_t>(Sext(p[0], 10) << 6);
        g.vy = static_cast<int16_t>(Sext(p[0] >> 10, 10) << 6);
        g.vz = static_cast<int16_t>(Sext(p[0] >> 20, 10) << 6);
        SubmitVertex();
        break;
    case 0x25:
        g.vx = static_cast<int16_t>(p[0]);
        g.vy = static_cast<int16_t>(p[0] >> 16);
        SubmitVertex();
        break;
    case 0x26:
        g.vx = static_cast<int16_t>(p[0]);
        g.vz = static_cast<int16_t>(p[0] >> 16);
        SubmitVertex();
        break;
    case 0x27:
        g.vy = static_cast<int16_t>(p[0]);
        g.vz = static_cast<int16_t>(p[0] >> 16);
        SubmitVertex();
        break;
    case 0x28:
        g.vx = static_cast<int16_t>(g.vx + Sext(p[0], 10));
        g.vy = static_cast<int16_t>(g.vy + Sext(p[0] >> 10, 10));
        g.vz = static_cast<int16_t>(g.vz + Sext(p[0] >> 20, 10));
        SubmitVertex();
        break;
    case 0x29: g.polyAttrNext = p[0]; break;
    case 0x2a: g.texParam = p[0]; break;
    case 0x2b: g.palBase = p[0] & 0x1fff; break;
    case 0x30:
        g.difAmb = p[0];
        if (p[0] & 0x8000) {
            g.cr = p[0] & 31;
            g.cg = (p[0] >> 5) & 31;
            g.cb = (p[0] >> 10) & 31;
        }
        break;
    case 0x31: g.speEmi = p[0]; break;
    case 0x32: {
        int i = static_cast<int>(p[0] >> 30);
        int32_t d[3] = {Sext(p[0], 10), Sext(p[0] >> 10, 10), Sext(p[0] >> 20, 10)};
        const Mtx &m = g.vec;
        for (int j = 0; j < 3; ++j)
            g.lightDir[i][j] = static_cast<int32_t>(
                (static_cast<int64_t>(d[0]) * m[j] + static_cast<int64_t>(d[1]) * m[4 + j] + static_cast<int64_t>(d[2]) * m[8 + j]) >> 12);
        break;
    }
    case 0x33: g.lightColor[p[0] >> 30] = static_cast<uint16_t>(p[0] & 0x7fff); break;
    case 0x34:
        for (int i = 0; i < 32; ++i)
            for (int b = 0; b < 4; ++b) g.shininess[i * 4 + b] = static_cast<uint8_t>(p[i] >> (8 * b));
        break;
    case 0x40:
        g.prim = static_cast<int>(p[0] & 3);
        g.polyAttr = g.polyAttrNext;
        g.stripCount = 0;
        g.stripOdd = false;
        break;
    case 0x41: break;
    case 0x50: {
        std::lock_guard<std::mutex> lock(gShownMu);
        g.shown.swap(g.list);
        g.list.clear();
        g.worldShown.swap(g.worldList);
        g.worldList.clear();
        g.projShown.swap(g.projList);
        g.projList.clear();
        g.glbShown.swap(g.glbList);
        g.glbList.clear();
        g.lockOnShown = g.lockOnPending;
        std::copy(g.lockOnPendingAt, g.lockOnPendingAt + 3, g.lockOnShownAt);
        std::copy(g.lockOnPendingScreen, g.lockOnPendingScreen + 2, g.lockOnShownScreen);
        g.lockOnPending = false;
        g.projRecorded = 0;
        SceneOf(g.projShown, g.worldShown);
        ClassifyHud(g.worldShown);
        g.vertexCount = 0;
        g.shownManualSort = p[0] & 1;
        g.shownWBuffer = p[0] & 2;
        g.shownVp = {g.vpX1, g.vpY1, g.vpX2, g.vpY2};
        NoteFrameSwapped();
        break;
    }
    case 0x60:
        g.vpX1 = static_cast<int>(p[0] & 0xff);
        g.vpY1 = static_cast<int>((p[0] >> 8) & 0xff);
        g.vpX2 = static_cast<int>((p[0] >> 16) & 0xff);
        g.vpY2 = static_cast<int>(p[0] >> 24);
        break;
    case 0x70: g.boxResult = BoxVisible(p); break;
    case 0x71: {
        g.vx = static_cast<int16_t>(p[0]);
        g.vy = static_cast<int16_t>(p[0] >> 16);
        g.vz = static_cast<int16_t>(p[1]);
        UpdateClip();
        const Mtx &m = g.clip;
        int64_t x = g.vx, y = g.vy, z = g.vz;
        for (int j = 0; j < 4; ++j)
            g.posTest[j] = static_cast<int32_t>((x * m[j] + y * m[4 + j] + z * m[8 + j] + 0x1000ll * m[12 + j]) >> 12);
        break;
    }
    case 0x72: {
        int64_t n[3] = {Sext(p[0], 10) << 3, Sext(p[0] >> 10, 10) << 3, Sext(p[0] >> 20, 10) << 3};
        const Mtx &m = g.vec;
        for (int j = 0; j < 3; ++j) {
            int64_t v = (n[0] * m[j] + n[1] * m[4 + j] + n[2] * m[8 + j]) >> 12;
            // 4.12 with the sign extended from bit 12 (the hardware keeps 1.12)
            g.vecTest[j] = static_cast<int16_t>(Sext(static_cast<uint32_t>(v), 13));
        }
        break;
    }
    default: break;
    }
}

void DrainQueue() {
    while (g.queuePos < g.queued && ParamCount(g.queue[g.queuePos]) == 0) Exec(g.queue[g.queuePos++], nullptr);
    if (g.queuePos >= g.queued) g.queued = 0;
}

void FifoWord(uint32_t w) {
    if (g.queued == 0) {
        // A packed command word: up to four commands, their parameters follow in order.
        for (int i = 0; i < 4; ++i) g.queue[i] = static_cast<uint8_t>(w >> (8 * i));
        g.queued = 4;
        // Trailing NOPs end the word early.
        while (g.queued > 1 && g.queue[g.queued - 1] == 0) --g.queued;
        g.queuePos = 0;
        g.paramCount = 0;
        DrainQueue();
        return;
    }
    g.params[g.paramCount++] = w;
    uint8_t cmd = g.queue[g.queuePos];
    if (g.paramCount >= ParamCount(cmd)) {
        Exec(cmd, g.params);
        g.paramCount = 0;
        ++g.queuePos;
        DrainQueue();
    }
}

void PortWord(uint8_t cmd, uint32_t w) {
    uint8_t n = ParamCount(cmd);
    if (n == 0) {
        Exec(cmd, nullptr);
        return;
    }
    if (g.portCmd != cmd) {
        g.portCmd = cmd;
        g.portCount = 0;
    }
    g.portParams[g.portCount++] = w;
    if (g.portCount >= n) {
        Exec(cmd, g.portParams);
        g.portCount = 0;
        g.portCmd = -1;
    }
}

// ---------------------------------------------------------------------------------------------
// Rendering engine
// ---------------------------------------------------------------------------------------------

// The rendering engine's buffers, at `scale` times the DS's 256 x 192.
struct Frame {
    int w = 0, h = 0;
    std::vector<uint8_t> r, gch, b, a;  // 6-bit colour, 5-bit alpha
    std::vector<uint32_t> depth;
    std::vector<uint8_t> fog;
    void Resize(int scale) {
        w = kW * scale;
        h = kH * scale;
        const size_t n = static_cast<size_t>(w) * h;
        for (auto *v : {&r, &gch, &b, &a, &fog}) v->resize(n);
        depth.resize(n);
    }
};

struct TexMem {
    std::vector<uint8_t> tex, pal;
    uint16_t Pal(uint32_t a) const {
        if (a + 1 >= kTexPalSize) return 0;
        return static_cast<uint16_t>(pal[a] | (pal[a + 1] << 8));
    }
    uint8_t Tex8(uint32_t a) const { return tex[a % kTexVramSize]; }
    uint16_t Tex16(uint32_t a) const { return static_cast<uint16_t>(Tex8(a) | (Tex8(a + 1) << 8)); }
    uint32_t Tex32(uint32_t a) const { return Tex16(a) | (static_cast<uint32_t>(Tex16(a + 2)) << 16); }
};

int Wrap(int c, int size, bool repeat, bool flip) {
    if (!repeat) return std::clamp(c, 0, size - 1);
    if (flip) {
        int period = 2 * size;
        c %= period;
        if (c < 0) c += period;
        return c >= size ? period - 1 - c : c;
    }
    return c & (size - 1);
}

struct Texel {
    int r, g, b, a;  // 6-bit colour, 5-bit alpha
};

Texel FromRgb15(uint16_t c, int a) {
    auto six = [](int v) { return v ? v * 2 + 1 : 0; };
    return {six(c & 31), six((c >> 5) & 31), six((c >> 10) & 31), a};
}

uint16_t Avg(uint16_t a, uint16_t b, int wa, int wb, int div) {
    int r = ((a & 31) * wa + (b & 31) * wb) / div;
    int gg = (((a >> 5) & 31) * wa + ((b >> 5) & 31) * wb) / div;
    int bb = (((a >> 10) & 31) * wa + ((b >> 10) & 31) * wb) / div;
    return static_cast<uint16_t>(r | (gg << 5) | (bb << 10));
}

Texel Sample(const TexMem &m, uint32_t param, uint32_t palBase, float fs, float ft) {
    const int w = 8 << ((param >> 20) & 7), h = 8 << ((param >> 23) & 7);
    auto floorInt = [](float v) {
        int i = static_cast<int>(v);
        return i - (v < static_cast<float>(i));
    };
    const int s = Wrap(floorInt(fs), w, param & (1u << 16), param & (1u << 18));
    const int t = Wrap(floorInt(ft), h, param & (1u << 17), param & (1u << 19));
    const uint32_t base = (param & 0xffff) * 8;
    const uint32_t format = (param >> 26) & 7;
    const bool zeroClear = param & (1u << 29);
    const uint32_t palOff = palBase * 16;
    const uint32_t texel = static_cast<uint32_t>(t * w + s);
    switch (format) {
    case 1: {  // A3I5
        uint8_t v = m.Tex8(base + texel);
        int a3 = v >> 5;
        return FromRgb15(m.Pal(palOff + (v & 31) * 2), (a3 << 2) + (a3 >> 1));
    }
    case 2: {  // 4 colours
        uint8_t v = (m.Tex8(base + texel / 4) >> (2 * (texel & 3))) & 3;
        if (zeroClear && v == 0) return {0, 0, 0, 0};
        return FromRgb15(m.Pal(palBase * 8 + v * 2), 31);
    }
    case 3: {  // 16 colours
        uint8_t v = (m.Tex8(base + texel / 2) >> (4 * (texel & 1))) & 15;
        if (zeroClear && v == 0) return {0, 0, 0, 0};
        return FromRgb15(m.Pal(palOff + v * 2), 31);
    }
    case 4: {  // 256 colours
        uint8_t v = m.Tex8(base + texel);
        if (zeroClear && v == 0) return {0, 0, 0, 0};
        return FromRgb15(m.Pal(palOff + v * 2), 31);
    }
    case 5: {  // 4x4 texel blocks with per-block palette data
        const uint32_t blockAddr = base + static_cast<uint32_t>(((t >> 2) * (w >> 2) + (s >> 2)) * 4);
        const uint32_t bits = m.Tex32(blockAddr);
        const uint32_t v = (bits >> (2 * ((t & 3) * 4 + (s & 3)))) & 3;
        const uint32_t slotOff = blockAddr & 0x1ffff;
        const uint32_t infoAddr = 0x20000 + slotOff / 2 + (blockAddr >= 0x40000 ? 0x10000 : 0);
        const uint16_t info = m.Tex16(infoAddr);
        const uint32_t p = palOff + (info & 0x3fff) * 4;
        const uint32_t mode = info >> 14;
        const uint16_t c0 = m.Pal(p), c1 = m.Pal(p + 2);
        switch (v) {
        case 0: return FromRgb15(c0, 31);
        case 1: return FromRgb15(c1, 31);
        case 2:
            if (mode == 0 || mode == 2) return FromRgb15(m.Pal(p + 4), 31);
            if (mode == 1) return FromRgb15(Avg(c0, c1, 1, 1, 2), 31);
            return FromRgb15(Avg(c0, c1, 5, 3, 8), 31);
        default:
            if (mode == 2) return FromRgb15(m.Pal(p + 6), 31);
            if (mode == 3) return FromRgb15(Avg(c0, c1, 3, 5, 8), 31);
            return {0, 0, 0, 0};
        }
    }
    case 6: {  // A5I3
        uint8_t v = m.Tex8(base + texel);
        return FromRgb15(m.Pal(palOff + (v & 7) * 2), v >> 3);
    }
    case 7: {  // direct colour
        uint16_t c = m.Tex16(base + texel * 2);
        return FromRgb15(c, (c & 0x8000) ? 31 : 0);
    }
    default: return {63, 63, 63, 31};
    }
}

// A polygon's vertex in screen space.
struct ScreenVertex {
    float x, y, depth, invW;
    float r, g, b, s, t;  // divided by w
};

struct RenderParams {
    int scale;  // the picture's size in DS pixels: 1 is the DS's own resolution
    uint16_t disp3d;
    int alphaRef;
    bool wbuffer;
    const TexMem *mem;
    uint16_t toon[32];
};

void DrawPixel(Frame &f, const RenderParams &rp, const Polygon &p, int i, uint32_t depth, int vr, int vg, int vb,
               float s, float t) {
    const uint32_t attr = p.attr;  // the depth test has passed (DrawTriangle)
    const int polyAlpha = static_cast<int>((attr >> 16) & 31);
    const int mode = static_cast<int>((attr >> 4) & 3);
    int r = vr, gg = vg, b = vb, a = polyAlpha;
    const uint32_t format = (p.texParam >> 26) & 7;
    const bool textured = (rp.disp3d & 1) && format != 0;
    if (mode == 2) {
        // Toon: the vertex colour picks a toon table entry (by its red). Highlight adds it.
        const uint16_t tc = rp.toon[std::clamp(vr >> 1, 0, 31)];
        Texel toon = FromRgb15(tc, 31);
        if (!(rp.disp3d & 2)) {
            r = toon.r;
            gg = toon.g;
            b = toon.b;
        }
    }
    if (textured) {
        Texel tx = Sample(*rp.mem, p.texParam, p.palBase, s, t);
        if (mode == 1) {  // decal
            if (tx.a == 31) {
                r = tx.r; gg = tx.g; b = tx.b;
            } else if (tx.a > 0) {
                r = (tx.r * tx.a + r * (31 - tx.a)) >> 5;
                gg = (tx.g * tx.a + gg * (31 - tx.a)) >> 5;
                b = (tx.b * tx.a + b * (31 - tx.a)) >> 5;
            }
        } else {  // modulate (and toon/highlight)
            r = ((tx.r + 1) * (r + 1) - 1) >> 6;
            gg = ((tx.g + 1) * (gg + 1) - 1) >> 6;
            b = ((tx.b + 1) * (b + 1) - 1) >> 6;
            a = ((tx.a + 1) * (polyAlpha + 1) - 1) >> 5;
        }
    }
    if (mode == 2 && (rp.disp3d & 2)) {
        Texel toon = FromRgb15(rp.toon[std::clamp(vr >> 1, 0, 31)], 31);
        r = std::min(r + toon.r, 63);
        gg = std::min(gg + toon.g, 63);
        b = std::min(b + toon.b, 63);
    }
    if (a == 0) return;
    if ((rp.disp3d & 4) && a <= rp.alphaRef) return;
    const bool fog = attr & (1u << 15);
    if (a == 31) {
        f.r[i] = static_cast<uint8_t>(r);
        f.gch[i] = static_cast<uint8_t>(gg);
        f.b[i] = static_cast<uint8_t>(b);
        f.a[i] = 31;
        f.depth[i] = depth;
        f.fog[i] = fog;
        return;
    }
    if ((rp.disp3d & 8) && f.a[i] > 0) {
        f.r[i] = static_cast<uint8_t>((r * (a + 1) + f.r[i] * (31 - a)) >> 5);
        f.gch[i] = static_cast<uint8_t>((gg * (a + 1) + f.gch[i] * (31 - a)) >> 5);
        f.b[i] = static_cast<uint8_t>((b * (a + 1) + f.b[i] * (31 - a)) >> 5);
        f.a[i] = static_cast<uint8_t>(std::max<int>(a, f.a[i]));
    } else {
        f.r[i] = static_cast<uint8_t>(r);
        f.gch[i] = static_cast<uint8_t>(gg);
        f.b[i] = static_cast<uint8_t>(b);
        f.a[i] = static_cast<uint8_t>(a);
    }
    if (attr & (1u << 11)) f.depth[i] = depth;
    f.fog[i] = f.fog[i] && fog;
}

// Draws the rows [rowMin, rowMax] of a triangle (a band of the picture: bands render in parallel).
void DrawTriangle(Frame &f, const RenderParams &rp, const Polygon &p, const ScreenVertex &v0, const ScreenVertex &v1,
                  const ScreenVertex &v2, int rowMin, int rowMax) {
    const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (std::fabs(area) < 1e-6f) return;
    const float inv = 1.0f / area;
    // Barycentric weights are linear in x along a row: w0 = a0 x + b0, w1 = a1 x + b1, and every
    // interpolant k = k2 + w0 (k0 - k2) + w1 (k1 - k2).
    const float a0 = (v1.y - v2.y) * inv, a1 = (v2.y - v0.y) * inv;
    enum { kInvW, kDepth, kR, kG, kB, kS, kT, kN };
    const float k0[kN] = {v0.invW, v0.depth, v0.r, v0.g, v0.b, v0.s, v0.t};
    const float k1[kN] = {v1.invW, v1.depth, v1.r, v1.g, v1.b, v1.s, v1.t};
    const float k2[kN] = {v2.invW, v2.depth, v2.r, v2.g, v2.b, v2.s, v2.t};
    float d0[kN], d1[kN], dx[kN];
    for (int k = 0; k < kN; ++k) {
        d0[k] = k0[k] - k2[k];
        d1[k] = k1[k] - k2[k];
        dx[k] = a0 * d0[k] + a1 * d1[k];
    }
    const int minY = std::max(rowMin, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
    const int maxY = std::min(rowMax, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));
    const int boxMinX = std::max(0, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
    const int boxMaxX = std::min(f.w - 1, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    constexpr float kEps = 1e-5f;  // shared edges leave no gaps
    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;
        const float b0 = (v1.x * (v2.y - py) - v2.x * (v1.y - py)) * inv;
        const float b1 = (v2.x * (v0.y - py) - v0.x * (v2.y - py)) * inv;
        // The span where w0, w1 and w2 = 1 - w0 - w1 are all >= 0.
        float lo = -1e9f, hi = 1e9f;
        bool empty = false;
        auto limit = [&](float a, float b) {  // a x + b >= -eps
            if (a > 0) lo = std::max(lo, (-kEps - b) / a);
            else if (a < 0) hi = std::min(hi, (-kEps - b) / a);
            else if (b < -kEps) empty = true;
        };
        limit(a0, b0);
        limit(a1, b1);
        limit(-a0 - a1, 1.0f - b0 - b1);
        if (empty || lo > hi) continue;
        const int x0 = std::max(boxMinX, static_cast<int>(std::ceil(lo - 0.5f)));
        const int x1 = std::min(boxMaxX, static_cast<int>(std::floor(hi - 0.5f)));
        if (x0 > x1) continue;
        const float px = x0 + 0.5f;
        const float w0 = a0 * px + b0, w1 = a1 * px + b1;
        float v[kN];
        for (int k = 0; k < kN; ++k) v[k] = k2[k] + w0 * d0[k] + w1 * d1[k];
        uint32_t *depthRow = f.depth.data() + static_cast<size_t>(y) * f.w;
        for (int x = x0; x <= x1; ++x) {
            const float invW = v[kInvW];
            if (invW > 0) {
                const float wv = 1.0f / invW;
                const float depthF = rp.wbuffer ? wv : v[kDepth];
                const uint32_t depth = static_cast<uint32_t>(std::clamp(depthF, 0.0f, 16777215.0f));
                // The depth test first: most hidden pixels never get shaded.
                const bool equal = p.attr & (1u << 14);
                const uint32_t old = depthRow[x];
                if (equal ? !(depth > old + 0x200 || depth + 0x200 < old) : depth < old) {
                    int r = static_cast<int>(v[kR] * wv), gg = static_cast<int>(v[kG] * wv),
                        b = static_cast<int>(v[kB] * wv);
                    DrawPixel(f, rp, p, y * f.w + x, depth, std::clamp(r, 0, 63), std::clamp(gg, 0, 63),
                              std::clamp(b, 0, 63), v[kS] * wv, v[kT] * wv);
                }
            }
            for (int k = 0; k < kN; ++k) v[k] += dx[k];
        }
    }
}

ScreenVertex ToScreen(const Vertex &v, bool wbuffer, int scale) {
    ScreenVertex o;
    const float w = v.w != 0 ? v.w : 1.0f;
    const auto &vp = g.shownVp;
    const float vpW = static_cast<float>(vp[2] - vp[0] + 1), vpH = static_cast<float>(vp[3] - vp[1] + 1);
    o.x = (v.x / w + 1.0f) * vpW * 0.5f + vp[0];
    const float yUp = (v.y / w + 1.0f) * vpH * 0.5f + vp[1];
    o.y = 192.0f - yUp;
    o.x *= scale;
    o.y *= scale;
    o.depth = wbuffer ? w : ((v.z / w) * 0x4000 + 0x3fff) * 0x200;
    o.invW = 1.0f / w;
    o.r = v.r * o.invW;
    o.g = v.g * o.invW;
    o.b = v.b * o.invW;
    o.s = v.s * o.invW;
    o.t = v.t * o.invW;
    return o;
}

// A wireframe polygon (alpha 0): only its edges, opaque, one DS pixel wide (`scale` pixels in a
// larger picture). The game outlines furniture and the like this way.
void DrawEdges(Frame &f, const RenderParams &rp, const Polygon &p, const ScreenVertex *sv, int rowMin, int rowMax) {
    Polygon opaque = p;
    opaque.attr |= 31u << 16;
    const int dot = std::max(1, rp.scale);
    for (int e = 0; e < p.n; ++e) {
        const ScreenVertex &a = sv[e], &b = sv[(e + 1) % p.n];
        const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) / dot)));
        for (int k = 0; k <= steps; ++k) {
            const float t = static_cast<float>(k) / steps;
            const float invW = a.invW + (b.invW - a.invW) * t;
            if (invW <= 0) continue;
            const float wv = 1.0f / invW;
            const float depthF = rp.wbuffer ? wv : a.depth + (b.depth - a.depth) * t;
            const uint32_t depth = static_cast<uint32_t>(std::clamp(depthF, 0.0f, 16777215.0f));
            const int cx = static_cast<int>(std::floor(a.x + (b.x - a.x) * t)) / dot * dot;
            const int cy = static_cast<int>(std::floor(a.y + (b.y - a.y) * t)) / dot * dot;
            const int r = std::clamp(static_cast<int>((a.r + (b.r - a.r) * t) * wv), 0, 63);
            const int gg = std::clamp(static_cast<int>((a.g + (b.g - a.g) * t) * wv), 0, 63);
            const int bb = std::clamp(static_cast<int>((a.b + (b.b - a.b) * t) * wv), 0, 63);
            const float s = (a.s + (b.s - a.s) * t) * wv, tt = (a.t + (b.t - a.t) * t) * wv;
            for (int y = std::max(cy, rowMin); y < cy + dot && y <= rowMax && y < f.h; ++y) {
                for (int x = std::max(cx, 0); x < cx + dot && x < f.w; ++x) {
                    const size_t i = static_cast<size_t>(y) * f.w + x;
                    if (depth <= f.depth[i]) DrawPixel(f, rp, opaque, static_cast<int>(i), depth, r, gg, bb, s, tt);
                }
            }
        }
    }
}

void DrawPolygon(Frame &f, const RenderParams &rp, const Polygon &p, int rowMin, int rowMax) {
    ScreenVertex sv[kMaxPolyVerts];
    for (int i = 0; i < p.n; ++i) sv[i] = ToScreen(p.v[i], rp.wbuffer, rp.scale);
    if (((p.attr >> 16) & 31) == 0) {
        DrawEdges(f, rp, p, sv, rowMin, rowMax);
        return;
    }
    for (int i = 1; i + 1 < p.n; ++i) DrawTriangle(f, rp, p, sv[0], sv[i], sv[i + 1], rowMin, rowMax);
}

void ApplyFog(Frame &f, uint16_t disp3d) {
    const uint32_t fogColor = IoReg32(0x358);
    const uint32_t offset = (IoReg16(0x35c) & 0x7fff) * 0x200u;
    const int shift = (disp3d >> 8) & 15;
    uint8_t table[33];
    for (int i = 0; i < 32; ++i) table[i] = IoReg8(0x360 + i) & 0x7f;
    table[32] = table[31];
    const bool alphaOnly = disp3d & 0x40;
    const int fr = (fogColor & 31) * 2, fg = ((fogColor >> 5) & 31) * 2, fb = ((fogColor >> 10) & 31) * 2;
    const int fa = (fogColor >> 16) & 31;
    for (size_t i = 0; i < f.fog.size(); ++i) {
        if (!f.fog[i]) continue;
        uint32_t z = f.depth[i];
        int density;
        if (z < offset) {
            density = table[0];
        } else {
            uint64_t zz = (static_cast<uint64_t>(z - offset) >> 2) << shift;
            uint64_t id = zz >> 17;
            if (id >= 32) density = table[32];
            else {
                uint64_t frac = zz & 0x1ffff;
                density = static_cast<int>((table[id] * (0x20000 - frac) + table[id + 1] * frac) >> 17);
            }
        }
        if (density >= 127) density = 128;
        if (!alphaOnly) {
            f.r[i] = static_cast<uint8_t>((fr * density + f.r[i] * (128 - density)) >> 7);
            f.gch[i] = static_cast<uint8_t>((fg * density + f.gch[i] * (128 - density)) >> 7);
            f.b[i] = static_cast<uint8_t>((fb * density + f.b[i] * (128 - density)) >> 7);
        }
        f.a[i] = static_cast<uint8_t>((fa * density + f.a[i] * (128 - density)) >> 7);
    }
}

}  // namespace

void GxReset() {
    g = Gx();
    for (Mtx &m : g.posStack) m = Identity();
    for (Mtx &m : g.vecStack) m = Identity();
    g.list.reserve(kMaxPolygons);
    g.shown.reserve(kMaxPolygons);
}

void GxWrite(uint32_t addr, uint32_t value, uint32_t size) {
    uint32_t off = (addr - 0x04000000u) & ~3u;
    if (size < 4) {
        // Byte and halfword writes to the ports are not something the SDK does; take them as a
        // word so a command still runs.
        value &= size == 1 ? 0xffu : 0xffffu;
    }
    if (off < 0x440) FifoWord(value);
    else PortWord(static_cast<uint8_t>((off - 0x400) >> 2), value);
}

uint16_t GxReadRegister(uint32_t off) {
    auto half = [off](uint32_t v) { return static_cast<uint16_t>((off & 2) ? v >> 16 : v); };
    if (off == 0x600) {
        return static_cast<uint16_t>((g.boxResult ? 2 : 0) | ((g.posSp & 31) << 8) | (g.projSp << 13) |
                                     (g.stackError ? 0x8000 : 0));
    }
    if (off == 0x604) return static_cast<uint16_t>(g.list.size());
    if (off == 0x606) return static_cast<uint16_t>(g.vertexCount);
    if (off >= 0x620 && off < 0x630) return half(static_cast<uint32_t>(g.posTest[(off - 0x620) / 4]));
    if (off >= 0x630 && off < 0x636) return static_cast<uint16_t>(g.vecTest[(off - 0x630) / 2]);
    if (off >= 0x640 && off < 0x680) {
        UpdateClip();
        return half(static_cast<uint32_t>(g.clip[(off - 0x640) / 4]));
    }
    if (off >= 0x680 && off < 0x6a4) {
        uint32_t i = (off - 0x680) / 4;
        return half(static_cast<uint32_t>(g.vec[(i / 3) * 4 + i % 3]));
    }
    return 0;
}

uint32_t GxCommandsThisFrame() { return gLastFrame; }
uint32_t GxPolygonsShown() { return static_cast<uint32_t>(g.shown.size()); }

void GxVBlank() {
    gLastFrame = gCommands;
    gCommands = 0;
}

void GxRender(uint16_t *color, uint8_t *alpha, int scale, bool hudOnly) {
    std::lock_guard<std::mutex> lock(gShownMu);
    static Frame f;
    static TexMem mem;
    mem.tex.resize(kTexVramSize);
    mem.pal.resize(kTexPalSize);
    VramTexture(mem.tex.data(), mem.pal.data());

    RenderParams rp;
    rp.scale = std::clamp(scale, 1, kMax3dScale);
    rp.disp3d = IoReg16(0x060);
    rp.alphaRef = IoReg8(0x340) & 31;
    rp.wbuffer = g.shownWBuffer;
    rp.mem = &mem;
    for (int i = 0; i < 32; ++i) rp.toon[i] = IoReg16(0x380 + 2 * i);

    // The rear plane: CLEAR_COLOR and CLEAR_DEPTH.
    const uint32_t clear = IoReg32(0x350);
    const uint16_t clearDepth = IoReg16(0x354) & 0x7fff;
    const uint32_t depth24 = clearDepth * 0x200u + ((clearDepth + 1u) / 0x8000u) * 0x1ffu;
    Texel cc = FromRgb15(static_cast<uint16_t>(clear & 0x7fff), static_cast<int>((clear >> 16) & 31));
    // The HUD pieces alone go over the captured world: on nothing, not on the rear plane's colour.
    if (hudOnly) cc.a = 0;
    f.Resize(rp.scale);
    std::fill(f.r.begin(), f.r.end(), static_cast<uint8_t>(cc.r));
    std::fill(f.gch.begin(), f.gch.end(), static_cast<uint8_t>(cc.g));
    std::fill(f.b.begin(), f.b.end(), static_cast<uint8_t>(cc.b));
    std::fill(f.a.begin(), f.a.end(), static_cast<uint8_t>(cc.a));
    std::fill(f.depth.begin(), f.depth.end(), depth24);
    std::fill(f.fog.begin(), f.fog.end(), static_cast<uint8_t>((clear >> 15) & 1));

    // Opaque polygons first, then translucent ones (sorted by their top, then bottom, row unless
    // the game sorts them itself).
    struct Sorted {
        const Polygon *p;
        float top, bottom;
    };
    std::vector<const Polygon *> opaque;
    std::vector<Sorted> translucent;
    for (const Polygon &p : g.shown) {
        const bool hudPiece = p.world >= 0 && static_cast<size_t>(p.world) < g.hudShown.size() && g.hudShown[p.world];
        if (hudOnly && p.proj < g.inSceneShown.size() && g.inSceneShown[p.proj] && !hudPiece) continue;
        if (!p.translucent) {
            opaque.push_back(&p);
            continue;
        }
        Sorted s{&p, 1e9f, -1e9f};
        for (int i = 0; i < p.n; ++i) {
            float y = ToScreen(p.v[i], false, 1).y;
            s.top = std::min(s.top, y);
            s.bottom = std::max(s.bottom, y);
        }
        translucent.push_back(s);
    }
    if (!g.shownManualSort)
        std::stable_sort(translucent.begin(), translucent.end(), [](const Sorted &a, const Sorted &b) {
            return a.bottom != b.bottom ? a.bottom < b.bottom : a.top < b.top;
        });
    // Every band draws all polygons in order, so the picture is the same as drawn in one go.
    auto band = [&](int rowMin, int rowMax) {
        for (const Polygon *p : opaque) DrawPolygon(f, rp, *p, rowMin, rowMax);
        for (const Sorted &s : translucent) DrawPolygon(f, rp, *s.p, rowMin, rowMax);
    };
    const int bands = rp.scale > 1 && !g.shown.empty() ? std::min(4, rp.scale * 2) : 1;
    if (bands == 1) {
        band(0, f.h - 1);
    } else {
        std::vector<std::thread> workers;
        for (int b = 1; b < bands; ++b) workers.emplace_back(band, f.h * b / bands, f.h * (b + 1) / bands - 1);
        band(0, f.h / bands - 1);
        for (std::thread &t : workers) t.join();
    }
    if (rp.disp3d & 0x80) ApplyFog(f, rp.disp3d);

    for (size_t i = 0; i < f.a.size(); ++i) {
        color[i] = static_cast<uint16_t>((f.r[i] >> 1) | ((f.gch[i] >> 1) << 5) | ((f.b[i] >> 1) << 10));
        alpha[i] = f.a[i];
    }
}

GxWorldStats gWorldStats;
std::atomic<bool> gDrawAll{false};

// Markers over things in the field (the lock-on sight over an enemy) are drawn flat on the
// screen, with a projection of their own, so the capture leaves them in the DS picture. Each run
// of such polygons near the middle of the screen goes into the world instead: on the game
// camera's line through its middle, a little in front of the first thing that line meets, at the
// size it had on the screen, as a sprite. Runs at the screen's edges (HUD pieces) stay out.
template <typename TextureOf>
void PlaceMarkers(PlatformWorld *out, const std::vector<const WorldPoly *> &pieces, TextureOf &texture) {
    const double *li = g.sceneLensInv.data(), *ci = g.sceneCameraInv.data();
    // A screen point (NDC) to the camera-space point on its line at the near (-1) or far (1) plane.
    auto unproject = [&](double nx, double ny, double nz, double *cam) {
        const double v[4] = {nx, ny, nz, 1};
        double r[4];
        for (int c = 0; c < 4; ++c) r[c] = v[0] * li[c] + v[1] * li[4 + c] + v[2] * li[8 + c] + v[3] * li[12 + c];
        for (int k = 0; k < 3; ++k) cam[k] = r[k] / r[3];
    };
    auto toWorld = [&](const double *c, double *w) {
        for (int k = 0; k < 3; ++k) w[k] = c[0] * ci[k] + c[1] * ci[4 + k] + c[2] * ci[8 + k] + ci[12 + k];
    };
    // The nearest captured triangle on a world-space segment, as a fraction of it (2 when none).
    const size_t sceneTriangles = out->triangleFlags.size();
    auto firstHit = [&](const double *a, const double *b) {
        double best = 2;
        const double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        for (size_t t = 0; t < sceneTriangles; ++t) {
            if (out->triangleFlags[t] & (kWorldLine | kWorldBillboard)) continue;
            const PlatformWorldVertex &p0 = out->vertices[t * 3], &p1 = out->vertices[t * 3 + 1], &p2 = out->vertices[t * 3 + 2];
            const double e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
            const double h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const double det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
            if (std::fabs(det) < 1e-12) continue;
            const double s[3] = {a[0] - p0.x, a[1] - p0.y, a[2] - p0.z};
            const double u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) / det;
            if (u < 0 || u > 1) continue;
            const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
            if (v < 0 || u + v > 1) continue;
            const double f = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
            if (f > 0 && f < best) best = f;
        }
        return best;
    };
    for (size_t first = 0; first < pieces.size();) {
        size_t end = first + 1;
        while (end < pieces.size() && pieces[end]->proj == pieces[first]->proj) ++end;
        // The run's extent on the screen (NDC).
        double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        bool ok = true;
        for (size_t i = first; i < end && ok; ++i)
            for (int k = 0; k < pieces[i]->n; ++k) {
                const Vertex &v = pieces[i]->v[k];
                if (!(v.w > 1e-6f)) { ok = false; break; }
                x0 = std::min<double>(x0, v.x / v.w); x1 = std::max<double>(x1, v.x / v.w);
                y0 = std::min<double>(y0, v.y / v.w); y1 = std::max<double>(y1, v.y / v.w);
            }
        const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        // Near the middle (not the HUD's corners and bottom strip), and marker-sized.
        ok = ok && std::fabs(cx) < 0.8 && cy > -0.55 && cy < 0.85 && x1 - x0 < 0.5 && y1 - y0 < 0.6;
        double camNear[3], camFar[3], wNear[3], wFar[3];
        double hit = 2;
        if (ok) {
            unproject(cx, cy, -1, camNear);
            unproject(cx, cy, 1, camFar);
            toWorld(camNear, wNear);
            toWorld(camFar, wFar);
            hit = firstHit(wNear, wFar);
        }
        if (hit > 1) {
            first = end;
            continue;
        }
        // A little in front of what it marks (half a unit, about the depth of a Heartless), never
        // nearer than the near plane.
        double zHit = camNear[2] + (camFar[2] - camNear[2]) * hit;
        const double z = std::min(zHit + 0.5, camNear[2] - 1e-3);
        auto onPlane = [&](double nx, double ny, double *cam) {
            double a[3], b[3];
            unproject(nx, ny, -1, a);
            unproject(nx, ny, 1, b);
            const double f = (z - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; ++k) cam[k] = a[k] + (b[k] - a[k]) * f;
        };
        double centre[3], centreWorld[3];
        onPlane(cx, cy, centre);
        toWorld(centre, centreWorld);
        if (out->billboard.empty()) out->billboard.assign(out->vertices.size() * kWorldBillboardFloats, 0.0f);
        out->billboard.resize(out->vertices.size() * kWorldBillboardFloats, 0.0f);
        for (size_t i = first; i < end; ++i) {
            const WorldPoly &w = *pieces[i];
            const uint32_t alpha = (w.attr >> 16) & 31;
            if (alpha == 0) continue;
            const int32_t tex = texture(w.texParam, w.palBase);
            const uint32_t format = (w.texParam >> 26) & 7;
            uint8_t flags = kWorldFront | kWorldBack | kWorldBillboard;  // a marker shows from either side
            if (alpha < 31 || (tex >= 0 && (format == 1 || format == 6))) flags |= kWorldTranslucent;
            if (alpha == 31 && (flags & kWorldTranslucent)) flags |= kWorldTexAlpha;
            if (((w.attr >> 4) & 3) == 1) flags |= kWorldDecal;
            PlatformWorldVertex pv[4];
            double off[4][2];
            for (int k = 0; k < w.n; ++k) {
                const Vertex &v = w.v[k];
                double cam[3], wp[3];
                onPlane(v.x / v.w, v.y / v.w, cam);
                toWorld(cam, wp);
                pv[k].x = static_cast<float>(wp[0]);
                pv[k].y = static_cast<float>(wp[1]);
                pv[k].z = static_cast<float>(wp[2]);
                off[k][0] = cam[0] - centre[0];
                off[k][1] = cam[1] - centre[1];
                if (tex >= 0) {
                    pv[k].u = v.s / out->textures[tex].width;
                    pv[k].v = v.t / out->textures[tex].height;
                } else {
                    pv[k].u = pv[k].v = 0;
                }
                auto ch = [](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 63.0f) * 255 / 63); };
                pv[k].color = ch(v.r) | (ch(v.g) << 8) | (ch(v.b) << 16) | ((alpha * 255 / 31) << 24);
            }
            for (int k = 1; k + 1 < w.n; ++k) {
                for (int j : {0, k, k + 1}) {
                    out->vertices.push_back(pv[j]);
                    out->billboard.insert(out->billboard.end(),
                                          {static_cast<float>(centreWorld[0]), static_cast<float>(centreWorld[1]),
                                           static_cast<float>(centreWorld[2]), static_cast<float>(off[j][0]),
                                           static_cast<float>(off[j][1]), 1.0f});
                }
                out->triangleTexture.push_back(tex);
                out->triangleFlags.push_back(flags);
            }
        }
        out->polygonsLeftOut -= static_cast<uint32_t>(end - first);
        ++gWorldStats.markers;
        first = end;
    }
}

void GxNoteLockOn(uint32_t position, int screenX, int screenY) {
    if (!position || position + 12 > GuestMemSize()) return;
    // The game's thread alone writes the pending sight (and moves it to the shown one at the swap).
    for (int k = 0; k < 3; ++k) g.lockOnPendingAt[k] = static_cast<int32_t>(Read32(position + 4 * k)) / 4096.0f;
    const bool on = screenX >= 0 && screenY >= 0 && screenX < 256 && screenY < 192;
    g.lockOnPendingScreen[0] = on ? screenX : -1;
    g.lockOnPendingScreen[1] = on ? screenY : -1;
    g.lockOnPending = true;
    ++gWorldStats.lockOns;
}

void GxCaptureWorld(PlatformWorld *out) {
    std::lock_guard<std::mutex> lock(gShownMu);
    out->lockOn = g.lockOnShown;
    std::copy(g.lockOnShownAt, g.lockOnShownAt + 3, out->lockOnAt);
    std::copy(g.lockOnShownScreen, g.lockOnShownScreen + 2, out->lockOnScreen);
    out->vertices.clear();
    out->triangleTexture.clear();
    out->triangleFlags.clear();
    out->textures.clear();
    out->hasCamera = false;
    out->polygonsLeftOut = 0;
    if (g.worldShown.empty()) return;
    if (!g.sceneKnown) return;
    const Mat4d &lensD = g.sceneLens, &cameraD = g.sceneCamera, &cameraInv = g.sceneCameraInv;
    const std::vector<Mat4d> &toWorld = g.toWorldShown;
    std::vector<bool> inScene(g.inSceneShown.begin(), g.inSceneShown.end());

    out->hasCamera = true;
    for (int i = 0; i < 16; ++i) {
        out->lens[i] = static_cast<float>(lensD[i]);
        out->view[i] = static_cast<float>(cameraD[i]);
    }
    for (int k = 0; k < 3; ++k) {
        out->eye[k] = static_cast<float>(cameraInv[12 + k]);
        out->up[k] = static_cast<float>(cameraInv[4 + k]);
        out->target[k] = static_cast<float>(cameraInv[12 + k] - cameraInv[8 + k]);
    }
    if (lensD[5] > 0 && std::fabs(lensD[11] + 1.0) < 1e-3) {  // a perspective lens
        out->fovY = static_cast<float>(2.0 * std::atan(1.0 / lensD[5]));
        const double m22 = lensD[10], m32 = lensD[14];
        if (std::fabs(m22 - 1.0) > 1e-9 && std::fabs(m22 + 1.0) > 1e-9) {
            out->nearZ = static_cast<float>(std::fabs(m32 / (m22 - 1.0)));
            out->farZ = static_cast<float>(std::fabs(m32 / (m22 + 1.0)));
        }
    }

    static TexMem mem;
    mem.tex.resize(kTexVramSize);
    mem.pal.resize(kTexPalSize);
    VramTexture(mem.tex.data(), mem.pal.data());
    const bool texturesOn = IoReg16(0x060) & 1;
    std::unordered_map<uint64_t, int32_t> byParams;
    // Decoded textures by their parameters, kept while texture memory stays the same (most
    // frames): decoding them all again each frame costs milliseconds on the headset.
    static std::unordered_map<uint64_t, PlatformWorldTexture> decoded;
    static uint64_t decodedVram = 0;
    const uint64_t vram = Fnv(mem.pal.data(), mem.pal.size(), Fnv(mem.tex.data(), mem.tex.size()));
    if (vram != decodedVram || decoded.size() > 4096) {
        decoded.clear();
        decodedVram = vram;
    }
    auto texture = [&](uint32_t param, uint32_t palBase) -> int32_t {
        const uint32_t format = (param >> 26) & 7;
        if (!texturesOn || format == 0) return -1;
        param &= 0x3fffffffu;  // the coordinate transform mode is not part of the image
        const uint64_t id = (static_cast<uint64_t>(param) << 13) | palBase;
        auto it = byParams.find(id);
        if (it != byParams.end()) return it->second;
        auto known = decoded.find(id);
        if (known != decoded.end()) {
            out->textures.push_back(known->second);
            int32_t index = static_cast<int32_t>(out->textures.size() - 1);
            byParams[id] = index;
            return index;
        }
        PlatformWorldTexture t;
        t.width = 8 << ((param >> 20) & 7);
        t.height = 8 << ((param >> 23) & 7);
        t.repeatS = param & (1u << 16);
        t.repeatT = param & (1u << 17);
        t.flipS = param & (1u << 18);
        t.flipT = param & (1u << 19);
        t.rgba.resize(static_cast<size_t>(t.width) * t.height);
        for (int y = 0; y < t.height; ++y)
            for (int x = 0; x < t.width; ++x) {
                Texel tx = Sample(mem, param, palBase, x + 0.5f, y + 0.5f);
                uint32_t r = static_cast<uint32_t>(tx.r * 255 / 63), gg = static_cast<uint32_t>(tx.g * 255 / 63);
                uint32_t b = static_cast<uint32_t>(tx.b * 255 / 63), a = static_cast<uint32_t>(tx.a * 255 / 31);
                t.rgba[static_cast<size_t>(y) * t.width + x] = r | (gg << 8) | (b << 16) | (a << 24);
            }
        t.key = Fnv(t.rgba.data(), t.rgba.size() * 4, Fnv(&id, sizeof id));
        decoded[id] = t;
        out->textures.push_back(std::move(t));
        int32_t index = static_cast<int32_t>(out->textures.size() - 1);
        byParams[id] = index;
        return index;
    };

    out->billboard.clear();
    // Sprites, two kinds. Facing the camera: runs of polygons with one texture whose every vertex
    // lies at one depth in front of the game's camera (munny, orbs, effects). Upright: see-through
    // polygons standing straight up, turned square to the camera about the vertical (trees). Each
    // sprite has a frame in the world (an origin, its right and up) and its vertices' places in it;
    // the centre is the middle of its extent.
    struct Sprite {
        int mode = 0;  // 0 none, 1 facing the camera, 2 upright
        size_t firstVertex = 0;
        int32_t tex = -2;
        uint16_t proj = 0;
        double key[4] = {};  // facing: depth; upright: the plane's normal (x, z) and offset
        double origin[3] = {}, right[3] = {}, up[3] = {};
        double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        std::vector<std::array<double, 2>> across;  // per vertex: along right, along up
    } sprite;
    auto endSprite = [&]() {
        if (sprite.mode == 0) return;
        if (out->billboard.empty()) out->billboard.assign(out->vertices.size() * kWorldBillboardFloats, 0.0f);
        out->billboard.resize(out->vertices.size() * kWorldBillboardFloats, 0.0f);
        const double cx = (sprite.x0 + sprite.x1) / 2, cy = (sprite.y0 + sprite.y1) / 2;
        double wc[3];
        for (int k = 0; k < 3; ++k) wc[k] = sprite.origin[k] + sprite.right[k] * cx + sprite.up[k] * cy;
        for (size_t i = 0; i < sprite.across.size(); ++i) {
            float *b = &out->billboard[(sprite.firstVertex + i) * kWorldBillboardFloats];
            b[0] = static_cast<float>(wc[0]);
            b[1] = static_cast<float>(wc[1]);
            b[2] = static_cast<float>(wc[2]);
            b[3] = static_cast<float>(sprite.across[i][0] - cx);
            b[4] = static_cast<float>(sprite.across[i][1] - cy);
            b[5] = static_cast<float>(sprite.mode);
        }
        for (size_t v = sprite.firstVertex; v < out->vertices.size(); v += 3) out->triangleFlags[v / 3] |= kWorldBillboard;
        sprite.mode = 0;
        sprite.across.clear();
    };
    std::vector<const WorldPoly *> flatPieces;  // drawn with another projection, in screen order
    for (const WorldPoly &w : g.worldShown) {
        const size_t index = static_cast<size_t>(&w - g.worldShown.data());
        if (index < g.hudShown.size() && g.hudShown[index]) {
            ++out->polygonsLeftOut;  // a HUD piece in front of the game's camera: on the HUD
            ++gWorldStats.hudPieces;
            continue;
        }
        if (w.proj >= inScene.size() || !inScene[w.proj]) {
            ++out->polygonsLeftOut;
            flatPieces.push_back(&w);
            continue;
        }
        const uint32_t alpha = (w.attr >> 16) & 31;
        const bool wire = alpha == 0;  // wireframe: its edges, opaque
        const Mat4d &m = toWorld[w.proj];
        const int32_t tex = texture(w.texParam, w.palBase);
        const uint32_t format = (w.texParam >> 26) & 7;
        uint8_t flags = 0;
        if (w.attr & 0x80) flags |= kWorldFront;
        if (w.attr & 0x40) flags |= kWorldBack;
        if (!wire && (alpha < 31 || (tex >= 0 && (format == 1 || format == 6)))) flags |= kWorldTranslucent;
        if (alpha == 31 && (flags & kWorldTranslucent)) flags |= kWorldTexAlpha;
        if (w.attr & (1u << 11)) flags |= kWorldDepthWrite;
        if (((w.attr >> 4) & 3) == 1) flags |= kWorldDecal;
        PlatformWorldVertex pv[4];
        for (int i = 0; i < w.n; ++i) {
            const Vertex &v = w.v[i];
            const double x = v.px / 4096.0, y = v.py / 4096.0, z = v.pz / 4096.0;
            pv[i].x = static_cast<float>(x * m[0] + y * m[4] + z * m[8] + m[12]);
            pv[i].y = static_cast<float>(x * m[1] + y * m[5] + z * m[9] + m[13]);
            pv[i].z = static_cast<float>(x * m[2] + y * m[6] + z * m[10] + m[14]);
            if (tex >= 0) {
                pv[i].u = v.s / out->textures[tex].width;
                pv[i].v = v.t / out->textures[tex].height;
            } else {
                pv[i].u = pv[i].v = 0;
            }
            auto ch = [](float c) { return static_cast<uint32_t>(std::clamp(c, 0.0f, 63.0f) * 255 / 63); };
            pv[i].color = ch(v.r) | (ch(v.g) << 8) | (ch(v.b) << 16) | ((wire ? 255u : alpha * 255 / 31) << 24);
        }
        // A sprite facing the camera: flat across its view (camera space z alike), sprite-sized.
        double cs[4][3];
        int mode = 0;
        double key[4] = {}, origin[3] = {}, right[3] = {}, up[3] = {};
        double along[4][2];
        const double *ci = cameraInv.data();
        bool flat = g.sceneHasCamera && !wire;
        for (int i = 0; flat && i < w.n; ++i) {
            const double *c = cameraD.data();
            const double x = pv[i].x, y = pv[i].y, z = pv[i].z;
            cs[i][0] = x * c[0] + y * c[4] + z * c[8] + c[12];
            cs[i][1] = x * c[1] + y * c[5] + z * c[9] + c[13];
            cs[i][2] = x * c[2] + y * c[6] + z * c[10] + c[14];
            flat = cs[i][2] < 0 && std::fabs(cs[i][2] - cs[0][2]) < 0.004 + 0.001 * std::fabs(cs[0][2]);
        }
        if (flat) {
            // Facing sprites are small; a wall the camera happens to face square on is not one.
            double x0 = cs[0][0], x1 = x0, y0 = cs[0][1], y1 = y0;
            for (int i = 1; i < w.n; ++i) {
                x0 = std::min(x0, cs[i][0]); x1 = std::max(x1, cs[i][0]);
                y0 = std::min(y0, cs[i][1]); y1 = std::max(y1, cs[i][1]);
            }
            flat = x1 - x0 > 0.01 && y1 - y0 > 0.01 && x1 - x0 < 1.2 && y1 - y0 < 1.2;
        }
        if (flat) {
            mode = 1;
            key[0] = cs[0][2];
            for (int k = 0; k < 3; ++k) {
                origin[k] = cs[0][2] * ci[8 + k] + ci[12 + k];  // on the camera's axis at that depth
                right[k] = ci[k];
                up[k] = ci[4 + k];
            }
            for (int i = 0; i < w.n; ++i) {
                along[i][0] = cs[i][0];
                along[i][1] = cs[i][1];
            }
        } else if (g.sceneHasCamera && !wire && tex >= 0 && (flags & kWorldTranslucent)) {
            // An upright sprite: a see-through polygon standing straight up, square to the camera
            // about the vertical (its normal along the way to the camera, or the way it looks).
            const double e1[3] = {pv[1].x - pv[0].x, pv[1].y - pv[0].y, pv[1].z - pv[0].z};
            const double e2[3] = {pv[2].x - pv[0].x, pv[2].y - pv[0].y, pv[2].z - pv[0].z};
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            double centre[3] = {0, 0, 0};
            for (int i = 0; i < w.n; ++i) {
                centre[0] += pv[i].x / w.n;
                centre[1] += pv[i].y / w.n;
                centre[2] += pv[i].z / w.n;
            }
            auto flatUnit = [](double x, double z, double *ox, double *oz) {
                const double l = std::sqrt(x * x + z * z);
                if (l < 1e-9) return false;
                *ox = x / l;
                *oz = z / l;
                return true;
            };
            double nx, nz, tx, tz, fx, fz;
            if (len > 1e-9 && std::fabs(n[1] / len) < 0.02 && flatUnit(n[0], n[2], &nx, &nz) &&
                flatUnit(ci[12] - centre[0], ci[14] - centre[2], &tx, &tz) && flatUnit(-ci[8], -ci[10], &fx, &fz)) {
                const double toCamera = nx * tx + nz * tz, looking = -(nx * fx + nz * fz);
                const double square = std::cos(0.6 * 3.14159265358979 / 180);
                if (std::fabs(toCamera) > square || std::fabs(looking) > square) {
                    const double s = (std::fabs(toCamera) > square ? toCamera : looking) < 0 ? -1 : 1;
                    nx *= s;
                    nz *= s;
                    // Right: up x normal.
                    right[0] = nz; right[1] = 0; right[2] = -nx;
                    up[0] = 0; up[1] = 1; up[2] = 0;
                    origin[0] = pv[0].x; origin[1] = 0; origin[2] = pv[0].z;
                    double a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9;
                    for (int i = 0; i < w.n; ++i) {
                        along[i][0] = (pv[i].x - origin[0]) * right[0] + (pv[i].z - origin[2]) * right[2];
                        along[i][1] = pv[i].y;
                        a0 = std::min(a0, along[i][0]); a1 = std::max(a1, along[i][0]);
                        b0 = std::min(b0, along[i][1]); b1 = std::max(b1, along[i][1]);
                    }
                    if (a1 - a0 > 0.05 && a1 - a0 < 6 && b1 - b0 > 0.05 && b1 - b0 < 12) {
                        mode = 2;
                        key[0] = nx;
                        key[1] = nz;
                        key[2] = nx * pv[0].x + nz * pv[0].z;
                    }
                }
            }
        }
        flat = mode != 0;
        if (flat) {
            double x0 = along[0][0], x1 = x0, y0 = along[0][1], y1 = y0;
            for (int i = 1; i < w.n; ++i) {
                x0 = std::min(x0, along[i][0]); x1 = std::max(x1, along[i][0]);
                y0 = std::min(y0, along[i][1]); y1 = std::max(y1, along[i][1]);
            }
            const double tol = mode == 1 ? 0.004 + 0.001 * std::fabs(key[0]) : 0.01;
            const bool same = sprite.mode == mode && sprite.tex == tex && sprite.proj == w.proj &&
                              (mode == 1 ? std::fabs(sprite.key[0] - key[0]) < tol
                                         : std::fabs(sprite.key[0] - key[0]) < 1e-3 &&
                                               std::fabs(sprite.key[1] - key[1]) < 1e-3 &&
                                               std::fabs(sprite.key[2] - key[2]) < tol);
            if (!same) {
                endSprite();
                sprite.mode = mode;
                sprite.tex = tex;
                sprite.proj = w.proj;
                std::copy(key, key + 4, sprite.key);
                std::copy(origin, origin + 3, sprite.origin);
                std::copy(right, right + 3, sprite.right);
                std::copy(up, up + 3, sprite.up);
                sprite.firstVertex = out->vertices.size();
                sprite.x0 = x0; sprite.x1 = x1; sprite.y0 = y0; sprite.y1 = y1;
            } else {
                // Places in the run's own frame (an upright run shares its plane, so its origin).
                if (mode == 2)
                    for (int i = 0; i < w.n; ++i)
                        along[i][0] = (pv[i].x - sprite.origin[0]) * sprite.right[0] + (pv[i].z - sprite.origin[2]) * sprite.right[2];
                if (mode == 2) {
                    x0 = x1 = along[0][0];
                    for (int i = 1; i < w.n; ++i) { x0 = std::min(x0, along[i][0]); x1 = std::max(x1, along[i][0]); }
                }
                sprite.x0 = std::min(sprite.x0, x0); sprite.x1 = std::max(sprite.x1, x1);
                sprite.y0 = std::min(sprite.y0, y0); sprite.y1 = std::max(sprite.y1, y1);
            }
            for (int i = 1; i + 1 < w.n; ++i)
                for (int k : {0, i, i + 1}) sprite.across.push_back({along[k][0], along[k][1]});
        }
        if (!flat) endSprite();
        if (wire) {
            for (int i = 0; i < w.n; ++i) {
                out->vertices.push_back(pv[i]);
                out->vertices.push_back(pv[(i + 1) % w.n]);
                out->vertices.push_back(pv[(i + 1) % w.n]);
                out->triangleTexture.push_back(tex);
                out->triangleFlags.push_back(static_cast<uint8_t>(kWorldLine | kWorldFront | kWorldBack));
            }
            continue;
        }
        for (int i = 1; i + 1 < w.n; ++i) {
            out->vertices.push_back(pv[0]);
            out->vertices.push_back(pv[i]);
            out->vertices.push_back(pv[i + 1]);
            out->triangleTexture.push_back(tex);
            out->triangleFlags.push_back(flags);
        }
    }
    endSprite();
    if (g.sceneHasCamera && !flatPieces.empty()) PlaceMarkers(out, flatPieces, texture);
    if (!out->billboard.empty()) out->billboard.resize(out->vertices.size() * kWorldBillboardFloats, 0.0f);
    // BG0's horizontal scroll moves the whole 3D layer on the screen (render2d.cpp, Bg0Scroll3d):
    // the world is drawn that much the other way.
    int scroll = IoReg16(0x10) & 511;
    if (scroll >= 256) scroll -= 512;
    out->viewport[0] = g.shownVp[0] - scroll;
    out->viewport[1] = g.shownVp[1];
    out->viewport[2] = g.shownVp[2] - scroll;
    out->viewport[3] = g.shownVp[3];
    // Fades: engine A's BLDY brightness when the 3D layer (BG0) is its first target, then its
    // master brightness.
    const uint16_t bldcnt = IoReg16(0x50), master = IoReg16(0x6c);
    const uint32_t effect = (bldcnt >> 6) & 3;
    out->fadeMode[0] = (bldcnt & 1) && (effect == 2 || effect == 3) ? static_cast<int>(effect - 1) : 0;
    out->fadeFactor[0] = std::min<int>(IoReg16(0x54) & 31, 16);
    out->fadeMode[1] = (master >> 14) == 1 ? 1 : (master >> 14) == 2 ? 2 : 0;
    out->fadeFactor[1] = std::min<int>(master & 31, 16);
    gWorldStats.triangles = static_cast<uint32_t>(out->triangleTexture.size());
    gWorldStats.textures = static_cast<uint32_t>(out->textures.size());
    gWorldStats.leftOut = out->polygonsLeftOut;
    ++gWorldStats.captures;
}

}  // namespace khvr::hle
