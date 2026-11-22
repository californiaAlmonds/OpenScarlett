#pragma once
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

// MIT DirectXTK12 SimpleMath subset for untouched Xbox-GDK-Samples.

namespace DirectX {
namespace SimpleMath {

struct Vector3 : public XMFLOAT3 {
    Vector3() noexcept : XMFLOAT3(0.f, 0.f, 0.f) {}
    constexpr Vector3(float ix, float iy, float iz) noexcept : XMFLOAT3(ix, iy, iz) {}
    Vector3(const XMFLOAT3& v) noexcept : XMFLOAT3(v) {}
    Vector3(const Vector3&) = default;
    Vector3& operator=(const Vector3&) = default;
    Vector3& operator=(const XMFLOAT3& v) noexcept {
        x = v.x;
        y = v.y;
        z = v.z;
        return *this;
    }
    Vector3(FXMVECTOR v) noexcept { XMStoreFloat3(this, v); }
    Vector3& operator=(FXMVECTOR v) noexcept {
        XMStoreFloat3(this, v);
        return *this;
    }
    operator XMVECTOR() const noexcept { return XMLoadFloat3(this); }
};

struct Vector4 : public XMFLOAT4 {
    Vector4() noexcept : XMFLOAT4(0.f, 0.f, 0.f, 0.f) {}
    constexpr Vector4(float ix, float iy, float iz, float iw) noexcept : XMFLOAT4(ix, iy, iz, iw) {}
    Vector4(const XMFLOAT4& v) noexcept : XMFLOAT4(v) {}
    Vector4(const Vector4&) = default;
    Vector4& operator=(const Vector4&) = default;
    Vector4& operator=(const XMFLOAT4& v) noexcept {
        x = v.x;
        y = v.y;
        z = v.z;
        w = v.w;
        return *this;
    }
    Vector4(FXMVECTOR v) noexcept { XMStoreFloat4(this, v); }
    Vector4& operator=(FXMVECTOR v) noexcept {
        XMStoreFloat4(this, v);
        return *this;
    }
    operator XMVECTOR() const noexcept { return XMLoadFloat4(this); }
};

struct Matrix : public XMFLOAT4X4 {
    Matrix() noexcept
        : XMFLOAT4X4(
              1.f, 0.f, 0.f, 0.f,
              0.f, 1.f, 0.f, 0.f,
              0.f, 0.f, 1.f, 0.f,
              0.f, 0.f, 0.f, 1.f) {}

    Matrix(const Matrix&) = default;
    Matrix& operator=(const Matrix&) = default;

    Matrix(CXMMATRIX M) noexcept { XMStoreFloat4x4(this, M); }

    Matrix& operator=(CXMMATRIX M) noexcept {
        XMStoreFloat4x4(this, M);
        return *this;
    }

    operator XMMATRIX() const noexcept { return XMLoadFloat4x4(this); }
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;

    static RECT ComputeTitleSafeArea(UINT backBufferWidth, UINT backBufferHeight) noexcept {
        const LONG safeX = static_cast<LONG>((backBufferWidth + 19u) / 20u);
        const LONG safeY = static_cast<LONG>((backBufferHeight + 19u) / 20u);
        RECT r{};
        r.left = safeX;
        r.top = safeY;
        r.right = static_cast<LONG>(backBufferWidth) - safeX;
        r.bottom = static_cast<LONG>(backBufferHeight) - safeY;
        return r;
    }
};

}  // namespace SimpleMath
}  // namespace DirectX
