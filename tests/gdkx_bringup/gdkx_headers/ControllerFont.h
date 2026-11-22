#pragma once
#include <DirectXMath.h>

#include "SpriteBatch.h"
#include "SpriteFont.h"

// MIT ATG ControllerFont helper subset for untouched Xbox-GDK-Samples.

namespace DX {

inline void DrawControllerString(
    DirectX::SpriteBatch* batch,
    DirectX::SpriteFont* textFont,
    DirectX::SpriteFont*,
    const wchar_t* text,
    DirectX::XMFLOAT2 position,
    DirectX::FXMVECTOR color = DirectX::g_XMOne,
    float scale = 1.0f) noexcept {
    if (batch && textFont && text) {
        textFont->DrawString(batch, text, position, color, 0.0f, DirectX::XMFLOAT2(0.0f, 0.0f), scale);
    }
}

}  // namespace DX
