#pragma once

// OpenScarlett MIT 3D Scene Math Utilities (C++20).
// Row-major 4x4 matrices with row-vector convention (v' = v * M) matching HLSL row_major float4x4.

#include <cmath>
#include <cstdint>

namespace gdk3d {

inline constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat4 {
    float m[4][4]{};
};

inline Vec3 Vec3Add(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 Vec3Sub(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 Vec3Scale(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

inline float Vec3Dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Vec3Cross(Vec3 a, Vec3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline Vec3 Vec3Normalize(Vec3 v) noexcept {
    const float len = std::sqrt(Vec3Dot(v, v));
    if (len < 1e-6f) {
        return {0.0f, 1.0f, 0.0f};
    }
    return {v.x / len, v.y / len, v.z / len};
}

inline Mat4 Mat4Identity() noexcept {
    Mat4 r{};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 Mat4Multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row][k] * b.m[k][col];
            }
            out.m[row][col] = sum;
        }
    }
    return out;
}

inline Mat4 Mat4Translation(float tx, float ty, float tz) noexcept {
    Mat4 r = Mat4Identity();
    r.m[3][0] = tx;
    r.m[3][1] = ty;
    r.m[3][2] = tz;
    return r;
}

inline Mat4 Mat4Scale(float sx, float sy, float sz) noexcept {
    Mat4 r{};
    r.m[0][0] = sx;
    r.m[1][1] = sy;
    r.m[2][2] = sz;
    r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 Mat4RotationX(float rad) noexcept {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4 r = Mat4Identity();
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

inline Mat4 Mat4RotationY(float rad) noexcept {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4 r = Mat4Identity();
    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
}

inline Mat4 Mat4RotationZ(float rad) noexcept {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4 r = Mat4Identity();
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    return r;
}

inline Mat4 Mat4LookAtRH(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 zaxis = Vec3Normalize(Vec3Sub(eye, target));
    const Vec3 xaxis = Vec3Normalize(Vec3Cross(up, zaxis));
    const Vec3 yaxis = Vec3Cross(zaxis, xaxis);

    Mat4 r{};
    r.m[0][0] = xaxis.x;
    r.m[0][1] = yaxis.x;
    r.m[0][2] = zaxis.x;
    r.m[0][3] = 0.0f;

    r.m[1][0] = xaxis.y;
    r.m[1][1] = yaxis.y;
    r.m[1][2] = zaxis.y;
    r.m[1][3] = 0.0f;

    r.m[2][0] = xaxis.z;
    r.m[2][1] = yaxis.z;
    r.m[2][2] = zaxis.z;
    r.m[2][3] = 0.0f;

    r.m[3][0] = -Vec3Dot(xaxis, eye);
    r.m[3][1] = -Vec3Dot(yaxis, eye);
    r.m[3][2] = -Vec3Dot(zaxis, eye);
    r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 Mat4PerspectiveFovRH(float fov_y_rad, float aspect, float near_z, float far_z) noexcept {
    const float y_scale = 1.0f / std::tan(fov_y_rad * 0.5f);
    const float x_scale = y_scale / aspect;
    Mat4 r{};
    r.m[0][0] = x_scale;
    r.m[1][1] = y_scale;
    r.m[2][2] = far_z / (near_z - far_z);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (near_z * far_z) / (near_z - far_z);
    return r;
}

inline Mat4 Mat4OrthoOffCenterRH(float left, float right, float bottom, float top,
                                 float near_z, float far_z) noexcept {
    Mat4 r{};
    r.m[0][0] = 2.0f / (right - left);
    r.m[1][1] = 2.0f / (top - bottom);
    r.m[2][2] = 1.0f / (near_z - far_z);
    r.m[3][0] = (left + right) / (left - right);
    r.m[3][1] = (bottom + top) / (bottom - top);
    r.m[3][2] = near_z / (near_z - far_z);
    r.m[3][3] = 1.0f;
    return r;
}

}  // namespace gdk3d
