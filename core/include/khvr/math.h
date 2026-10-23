// Small math library shared by the VR layer. Right-handed, Y up, -Z forward (OpenXR convention).
// Matrices are column-major (m[col*4 + row]) so they can be handed straight to OpenGL ES.
#pragma once

#include <cmath>
#include <cstdint>

namespace khvr {

constexpr float kPi = 3.14159265358979323846f;

inline float DegToRad(float d) { return d * (kPi / 180.0f); }
inline float RadToDeg(float r) { return r * (180.0f / kPi); }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Wraps an angle in radians to (-pi, pi].
inline float WrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3 &operator-=(const Vec3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3 &operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3 &a, const Vec3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3 &v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3 &v) {
    float l = Length(v);
    return l > 1e-6f ? v / l : Vec3{};
}
inline Vec3 Lerp(const Vec3 &a, const Vec3 &b, float t) { return a + (b - a) * t; }

// Yaw of a direction on the XZ plane, 0 = facing -Z, positive = turning left (counter-clockwise
// seen from above), matching a rotation about +Y.
inline float YawOf(const Vec3 &dir) { return std::atan2(-dir.x, -dir.z); }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat AxisAngle(const Vec3 &axis, float angle) {
        Vec3 a = Normalize(axis);
        float s = std::sin(angle * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
    }
    static Quat Yaw(float angle) { return AxisAngle({0, 1, 0}, angle); }

    Quat operator*(const Quat &q) const {
        return {w * q.x + x * q.w + y * q.z - z * q.y,
                w * q.y - x * q.z + y * q.w + z * q.x,
                w * q.z + x * q.y - y * q.x + z * q.w,
                w * q.w - x * q.x - y * q.y - z * q.z};
    }
    Quat Conjugate() const { return {-x, -y, -z, w}; }
    Vec3 Rotate(const Vec3 &v) const {
        Vec3 u{x, y, z};
        Vec3 t = Cross(u, v) * 2.0f;
        return v + t * w + Cross(u, t);
    }
    Vec3 Forward() const { return Rotate({0, 0, -1}); }
    Vec3 Up() const { return Rotate({0, 1, 0}); }
    Vec3 Right() const { return Rotate({1, 0, 0}); }
};

inline Quat Normalize(const Quat &q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-6f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}

// Yaw-only part of an orientation (the heading of its forward vector).
inline float YawOf(const Quat &q) { return YawOf(q.Forward()); }

struct Pose {
    Quat orientation;
    Vec3 position;
    Vec3 Transform(const Vec3 &p) const { return orientation.Rotate(p) + position; }
    Pose operator*(const Pose &o) const {  // this ∘ o
        return {orientation * o.orientation, Transform(o.position)};
    }
    Pose Inverse() const {
        Quat inv = orientation.Conjugate();
        return {inv, inv.Rotate(-position)};
    }
};

// Similarity transform: uniform scale, then rotation, then translation. Used for the mapping
// from the player's tracking space (meters) into the game world (game units).
struct Similarity {
    float scale = 1.0f;
    Quat rotation;
    Vec3 translation;
    Vec3 Transform(const Vec3 &p) const { return rotation.Rotate(p * scale) + translation; }
    Vec3 TransformDir(const Vec3 &d) const { return rotation.Rotate(d); }
    Similarity Inverse() const {
        Quat inv = rotation.Conjugate();
        return {1.0f / scale, inv, inv.Rotate(-translation) / scale};
    }
};

struct Mat4 {
    float m[16];
    static Mat4 Identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    float &at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    Mat4 operator*(const Mat4 &o) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    Vec3 TransformPoint(const Vec3 &p) const {
        float x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
        float y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
        float z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
        float w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
        return w != 0.0f && w != 1.0f ? Vec3{x / w, y / w, z / w} : Vec3{x, y, z};
    }
    static Mat4 FromSimilarity(const Similarity &s) {
        Mat4 r = Identity();
        Vec3 cx = s.rotation.Rotate({s.scale, 0, 0});
        Vec3 cy = s.rotation.Rotate({0, s.scale, 0});
        Vec3 cz = s.rotation.Rotate({0, 0, s.scale});
        r.at(0, 0) = cx.x; r.at(1, 0) = cx.y; r.at(2, 0) = cx.z;
        r.at(0, 1) = cy.x; r.at(1, 1) = cy.y; r.at(2, 1) = cy.z;
        r.at(0, 2) = cz.x; r.at(1, 2) = cz.y; r.at(2, 2) = cz.z;
        r.at(0, 3) = s.translation.x; r.at(1, 3) = s.translation.y; r.at(2, 3) = s.translation.z;
        return r;
    }
    static Mat4 FromPose(const Pose &p) { return FromSimilarity({1.0f, p.orientation, p.position}); }
    // OpenXR-style asymmetric projection from the four half-angle tangents of an eye's FOV.
    static Mat4 Projection(float tanLeft, float tanRight, float tanUp, float tanDown, float nearZ,
                           float farZ) {
        Mat4 r{};
        float w = tanRight - tanLeft, h = tanUp - tanDown;
        r.at(0, 0) = 2.0f / w;
        r.at(0, 2) = (tanRight + tanLeft) / w;
        r.at(1, 1) = 2.0f / h;
        r.at(1, 2) = (tanUp + tanDown) / h;
        r.at(2, 2) = -(farZ + nearZ) / (farZ - nearZ);
        r.at(2, 3) = -2.0f * farZ * nearZ / (farZ - nearZ);
        r.at(3, 2) = -1.0f;
        return r;
    }
};

// The DS works in fixed point: fx32 has 12 fractional bits, so FX32_ONE = 0x1000.
inline float Fx32ToFloat(int32_t v) { return static_cast<float>(v) / 4096.0f; }
inline int32_t FloatToFx32(float v) { return static_cast<int32_t>(std::lround(v * 4096.0f)); }

// DS angles are 16-bit binary angles: 0x10000 is a full turn.
inline float DsAngleToRad(uint16_t a) { return static_cast<float>(a) * (2.0f * kPi / 65536.0f); }
inline uint16_t RadToDsAngle(float r) {
    float turns = WrapAngle(r) / (2.0f * kPi);
    return static_cast<uint16_t>(static_cast<int32_t>(std::lround(turns * 65536.0f)) & 0xffff);
}

}  // namespace khvr
