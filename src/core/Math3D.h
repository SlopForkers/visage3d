#pragma once
// Minimal 3D math: Vec2/Vec3/Vec4, Quat, Mat3, Mat4 (column-major, OpenGL-style).
#include <cmath>
#include <algorithm>

struct Vec2 {
    float x = 0.f, y = 0.f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; } // component-wise
    Vec3 operator/(const Vec3& o) const { return {x / o.x, y / o.y, z / o.z}; } // component-wise
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const { float l = length(); return l > 1e-8f ? *this / l : Vec3{0, 1, 0}; }
};

struct Vec4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4 operator*(const Vec4& o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
};

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Quat normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 1e-8f ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
};

// Column-major 3x3. m[col*3 + row]
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    static Mat3 identity() { return Mat3{}; }
    float det() const {
        return m[0] * (m[4] * m[8] - m[7] * m[5])
             - m[3] * (m[1] * m[8] - m[7] * m[2])
             + m[6] * (m[1] * m[5] - m[4] * m[2]);
    }
    Mat3 transposed() const {
        Mat3 r;
        for (int c = 0; c < 3; ++c)
            for (int row = 0; row < 3; ++row)
                r.m[c * 3 + row] = m[row * 3 + c];
        return r;
    }
    Mat3 inverted() const {
        // Upper-left 3x3 of a (possibly scaled) rigid transform: full adjugate inverse.
        Mat3 r;
        float d = det();
        if (std::fabs(d) < 1e-12f) return identity();
        float inv = 1.f / d;
        r.m[0] =  (m[4] * m[8] - m[7] * m[5]) * inv;
        r.m[1] = -(m[1] * m[8] - m[7] * m[2]) * inv;
        r.m[2] =  (m[1] * m[5] - m[4] * m[2]) * inv;
        r.m[3] = -(m[3] * m[8] - m[6] * m[5]) * inv;
        r.m[4] =  (m[0] * m[8] - m[6] * m[2]) * inv;
        r.m[5] = -(m[0] * m[5] - m[3] * m[2]) * inv;
        r.m[6] =  (m[3] * m[7] - m[6] * m[4]) * inv;
        r.m[7] = -(m[0] * m[7] - m[6] * m[1]) * inv;
        r.m[8] =  (m[0] * m[4] - m[3] * m[1]) * inv;
        return r;
    }
};

// Column-major 4x4. m[col*4 + row]
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 scaling(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 rotation(const Quat& q_) {
        Quat q = q_.normalized();
        float x = q.x, y = q.y, z = q.z, w = q.w;
        float xx = x * x, yy = y * y, zz = z * z;
        float xy = x * y, xz = x * z, yz = y * z, xw = x * w, yw = y * w, zw = z * w;
        Mat4 r;
        r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + zw);     r.m[2] = 2 * (xz - yw);
        r.m[4] = 2 * (xy - zw);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + xw);
        r.m[8] = 2 * (xz + yw);     r.m[9] = 2 * (yz - xw);     r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
    static Mat4 trs(const Vec3& t, const Quat& r, const Vec3& s) {
        return translation(t) * rotation(r) * scaling(s);
    }
    static Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
        Mat4 r;
        for (float& v : r.m) v = 0.f;
        float f = 1.f / std::tan(fovYRad * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zFar + zNear) / (zNear - zFar);
        r.m[11] = -1.f;
        r.m[14] = 2.f * zFar * zNear / (zNear - zFar);
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).normalized();
        Vec3 s = f.cross(up).normalized();
        Vec3 u = s.cross(f);
        Mat4 r;
        r.m[0] = s.x;  r.m[1] = u.x;  r.m[2] = -f.x;
        r.m[4] = s.y;  r.m[5] = u.y;  r.m[6] = -f.y;
        r.m[8] = s.z;  r.m[9] = u.z;  r.m[10] = -f.z;
        r.m[12] = -s.dot(eye);
        r.m[13] = -u.dot(eye);
        r.m[14] = f.dot(eye);
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0.f;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    Mat3 upper3x3() const {
        Mat3 r;
        for (int c = 0; c < 3; ++c)
            for (int row = 0; row < 3; ++row)
                r.m[c * 3 + row] = m[c * 4 + row];
        return r;
    }
};
