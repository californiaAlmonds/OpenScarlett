#pragma once
#include <DirectXMath.h>

// Exact MIT ATG clear & UI colors matching Microsoft/Xbox-GDK-Samples Kits/ATGTK/ATGColors.h.
// Linear 0.052860655 converts to sRGB ~0.255 -> #414141 (RGB 65, 65, 65) in B8G8R8A8_UNORM_SRGB.
namespace ATG {
namespace Colors {
inline constexpr DirectX::XMVECTORF32 White = {{{1.0f, 1.0f, 1.0f, 1.0f}}};
inline constexpr DirectX::XMVECTORF32 LightGrey = {{{0.6f, 0.6f, 0.6f, 1.0f}}};
inline constexpr DirectX::XMVECTORF32 Background = {{{0.254901975f, 0.254901975f, 0.254901975f, 1.0f}}};
}  // namespace Colors
namespace ColorsLinear {
inline constexpr float Background[4] = {0.052860655f, 0.052860655f, 0.052860655f, 1.0f};
}  // namespace ColorsLinear
}  // namespace ATG
