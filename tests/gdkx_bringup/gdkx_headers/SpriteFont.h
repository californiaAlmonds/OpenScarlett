#pragma once
#include <d3d12_xs.h>
#include <DirectXMath.h>

#include <cwchar>

#include "ResourceUploadBatch.h"
#include "SpriteBatch.h"

// MIT DirectXTK12 SpriteFont subset for untouched Xbox-GDK-Samples.
// Gracefully handles optional .spritefont HUD overlays without requiring binary font packs.

namespace DirectX {

class SpriteFont {
public:
    SpriteFont(
        ID3D12Device*,
        ResourceUploadBatch&,
        const wchar_t* filename,
        D3D12_CPU_DESCRIPTOR_HANDLE,
        D3D12_GPU_DESCRIPTOR_HANDLE,
        bool = false) noexcept
        : m_lineSpacing(36.0f) {
        if (filename && std::wcsstr(filename, L"18") != nullptr) {
            m_lineSpacing = 24.0f;
        }
    }

    SpriteFont(const SpriteFont&) = delete;
    SpriteFont& operator=(const SpriteFont&) = delete;

    void DrawString(
        SpriteBatch*,
        const wchar_t*,
        XMFLOAT2,
        FXMVECTOR = g_XMOne,
        float = 0.0f,
        XMFLOAT2 = XMFLOAT2(0.0f, 0.0f),
        float = 1.0f) const noexcept {}

    float GetLineSpacing() const noexcept { return m_lineSpacing; }

private:
    float m_lineSpacing;
};

}  // namespace DirectX
