#include "hle_gx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "hle.h"

namespace khvr::hle {

namespace {

// ---------------------------------------------------------------------------------------------
// Geometry engine state
// ---------------------------------------------------------------------------------------------

// Row-major 4x4, 20.12 fixed point. The DS multiplies row vectors: v' = v * M, and a new matrix
// goes in front of the current one (M = N * M).
using Mtx = std::array<int32_t, 16>;

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
};

constexpr int kMaxPolyVerts = 10;  // a quad clipped by six planes
struct Polygon {
    Vertex v[kMaxPolyVerts];
    int n = 0;
    uint32_t attr = 0, texParam = 0, palBase = 0;
    bool translucent = false;
};

constexpr size_t kMaxPolygons = 2048;
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
    bool shownWBuffer = false, shownManualSort = false;
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
    case 0: g.proj = m; break;
    case 1: g.pos = m; break;
    case 2: g.pos = m; g.vec = m; break;
    default: g.tex = m; break;
    }
    g.clipDirty = true;
}

void Apply(const Mtx &m, bool vecToo) {
    switch (g.mtxMode) {
    case 0: g.proj = Mul(m, g.proj); break;
    case 1: g.pos = Mul(m, g.pos); break;
    case 2:
        g.pos = Mul(m, g.pos);
        if (vecToo) g.vec = Mul(m, g.vec);
        break;
    default: g.tex = Mul(m, g.tex); break;
    }
    g.clipDirty = true;
}

// ---------------------------------------------------------------------------------------------
// Polygons: culling, clipping, the polygon list
// ---------------------------------------------------------------------------------------------

Vertex Lerp(const Vertex &a, const Vertex &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t,
            a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.s + (b.s - a.s) * t, a.t + (b.t - a.t) * t};
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

bool BoxVisible(const uint32_t *p) {
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
        if (g.mtxMode == 0) g.proj = g.projStack;
        else if (g.mtxMode == 3) g.tex = g.texStack;
        else {
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
    case 0x50:
        g.shown.swap(g.list);
        g.list.clear();
        g.vertexCount = 0;
        g.shownManualSort = p[0] & 1;
        g.shownWBuffer = p[0] & 2;
        break;
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

struct Frame {
    uint8_t r[kW * kH], gch[kW * kH], b[kW * kH], a[kW * kH];  // 6-bit colour, 5-bit alpha
    uint32_t depth[kW * kH];
    uint8_t fog[kW * kH];
};

struct TexMem {
    std::vector<uint8_t> tex, pal;
    uint16_t Pal(uint32_t a) const {
        a %= kTexPalSize;
        return static_cast<uint16_t>(pal[a] | (pal[(a + 1) % kTexPalSize] << 8));
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
    const int s = Wrap(static_cast<int>(std::floor(fs)), w, param & (1u << 16), param & (1u << 18));
    const int t = Wrap(static_cast<int>(std::floor(ft)), h, param & (1u << 17), param & (1u << 19));
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
    uint16_t disp3d;
    int alphaRef;
    bool wbuffer;
    const TexMem *mem;
    uint16_t toon[32];
};

void DrawPixel(Frame &f, const RenderParams &rp, const Polygon &p, int i, uint32_t depth, int vr, int vg, int vb,
               float s, float t) {
    const uint32_t attr = p.attr;
    const bool equal = attr & (1u << 14);
    if (equal ? (depth > f.depth[i] + 0x200 || depth + 0x200 < f.depth[i]) : depth >= f.depth[i]) return;
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

void DrawTriangle(Frame &f, const RenderParams &rp, const Polygon &p, const ScreenVertex &v0, const ScreenVertex &v1,
                  const ScreenVertex &v2) {
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (std::fabs(area) < 1e-6f) return;
    const float inv = 1.0f / area;
    int minX = std::max(0, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
    int maxX = std::min(kW - 1, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    int minY = std::max(0, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
    int maxY = std::min(kH - 1, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));
    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;
            float w0 = ((v1.x - px) * (v2.y - py) - (v2.x - px) * (v1.y - py)) * inv;
            float w1 = ((v2.x - px) * (v0.y - py) - (v0.x - px) * (v2.y - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            // Inside, with a small bias so shared edges leave no gaps.
            if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
            float invW = w0 * v0.invW + w1 * v1.invW + w2 * v2.invW;
            if (invW <= 0) continue;
            float wv = 1.0f / invW;
            float depthF = rp.wbuffer ? wv : w0 * v0.depth + w1 * v1.depth + w2 * v2.depth;
            uint32_t depth = static_cast<uint32_t>(std::clamp(depthF, 0.0f, 16777215.0f));
            int r = static_cast<int>((w0 * v0.r + w1 * v1.r + w2 * v2.r) * wv);
            int gg = static_cast<int>((w0 * v0.g + w1 * v1.g + w2 * v2.g) * wv);
            int b = static_cast<int>((w0 * v0.b + w1 * v1.b + w2 * v2.b) * wv);
            float s = (w0 * v0.s + w1 * v1.s + w2 * v2.s) * wv;
            float t = (w0 * v0.t + w1 * v1.t + w2 * v2.t) * wv;
            DrawPixel(f, rp, p, y * kW + x, depth, std::clamp(r, 0, 63), std::clamp(gg, 0, 63), std::clamp(b, 0, 63), s,
                      t);
        }
    }
}

ScreenVertex ToScreen(const Vertex &v, bool wbuffer) {
    ScreenVertex o;
    const float w = v.w != 0 ? v.w : 1.0f;
    const float vpW = static_cast<float>(g.vpX2 - g.vpX1 + 1), vpH = static_cast<float>(g.vpY2 - g.vpY1 + 1);
    o.x = (v.x / w + 1.0f) * vpW * 0.5f + g.vpX1;
    const float yUp = (v.y / w + 1.0f) * vpH * 0.5f + g.vpY1;
    o.y = 192.0f - yUp;
    o.depth = wbuffer ? w : ((v.z / w) * 0x4000 + 0x3fff) * 0x200;
    o.invW = 1.0f / w;
    o.r = v.r * o.invW;
    o.g = v.g * o.invW;
    o.b = v.b * o.invW;
    o.s = v.s * o.invW;
    o.t = v.t * o.invW;
    return o;
}

void DrawPolygon(Frame &f, const RenderParams &rp, const Polygon &p) {
    if (((p.attr >> 16) & 31) == 0) return;  // wireframe: edges only, not drawn yet
    ScreenVertex sv[kMaxPolyVerts];
    for (int i = 0; i < p.n; ++i) sv[i] = ToScreen(p.v[i], rp.wbuffer);
    for (int i = 1; i + 1 < p.n; ++i) DrawTriangle(f, rp, p, sv[0], sv[i], sv[i + 1]);
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
    for (int i = 0; i < kW * kH; ++i) {
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

void GxRender(uint16_t *color, uint8_t *alpha) {
    static Frame f;
    static TexMem mem;
    mem.tex.resize(kTexVramSize);
    mem.pal.resize(kTexPalSize);
    VramTexture(mem.tex.data(), mem.pal.data());

    RenderParams rp;
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
    std::fill(std::begin(f.r), std::end(f.r), static_cast<uint8_t>(cc.r));
    std::fill(std::begin(f.gch), std::end(f.gch), static_cast<uint8_t>(cc.g));
    std::fill(std::begin(f.b), std::end(f.b), static_cast<uint8_t>(cc.b));
    std::fill(std::begin(f.a), std::end(f.a), static_cast<uint8_t>(cc.a));
    std::fill(std::begin(f.depth), std::end(f.depth), depth24);
    std::fill(std::begin(f.fog), std::end(f.fog), static_cast<uint8_t>((clear >> 15) & 1));

    // Opaque polygons first, then translucent ones (sorted by their top, then bottom, row unless
    // the game sorts them itself).
    struct Sorted {
        const Polygon *p;
        float top, bottom;
    };
    std::vector<Sorted> translucent;
    for (const Polygon &p : g.shown) {
        if (!p.translucent) {
            DrawPolygon(f, rp, p);
            continue;
        }
        Sorted s{&p, 1e9f, -1e9f};
        for (int i = 0; i < p.n; ++i) {
            float y = ToScreen(p.v[i], false).y;
            s.top = std::min(s.top, y);
            s.bottom = std::max(s.bottom, y);
        }
        translucent.push_back(s);
    }
    if (!g.shownManualSort)
        std::stable_sort(translucent.begin(), translucent.end(), [](const Sorted &a, const Sorted &b) {
            return a.bottom != b.bottom ? a.bottom < b.bottom : a.top < b.top;
        });
    for (const Sorted &s : translucent) DrawPolygon(f, rp, *s.p);
    if (rp.disp3d & 0x80) ApplyFog(f, rp.disp3d);

    for (int i = 0; i < kW * kH; ++i) {
        color[i] = static_cast<uint16_t>((f.r[i] >> 1) | ((f.gch[i] >> 1) << 5) | ((f.b[i] >> 1) << 10));
        alpha[i] = f.a[i];
    }
}

}  // namespace khvr::hle
