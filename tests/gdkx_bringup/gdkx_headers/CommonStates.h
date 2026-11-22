#pragma once
#include <d3d12_xs.h>

// Clean-room MIT DirectXTK12 CommonStates subset for untouched Xbox-GDK-Samples.

namespace DirectX {

class CommonStates {
public:
    explicit CommonStates(ID3D12Device*) noexcept {}
    CommonStates(const CommonStates&) = delete;
    CommonStates& operator=(const CommonStates&) = delete;

    static constexpr D3D12_BLEND_DESC MakeBlend(
        BOOL enable,
        D3D12_BLEND srcBlend,
        D3D12_BLEND dstBlend) noexcept {
        D3D12_RENDER_TARGET_BLEND_DESC rt = {
            enable,
            FALSE,
            srcBlend,
            dstBlend,
            D3D12_BLEND_OP_ADD,
            srcBlend,
            dstBlend,
            D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        D3D12_BLEND_DESC desc = {
            FALSE,
            FALSE,
            {rt, rt, rt, rt, rt, rt, rt, rt},
        };
        return desc;
    }

    inline static const D3D12_BLEND_DESC Opaque =
        MakeBlend(FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO);
    inline static const D3D12_BLEND_DESC AlphaBlend =
        MakeBlend(TRUE, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA);
    inline static const D3D12_BLEND_DESC Additive =
        MakeBlend(TRUE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_ONE);
    inline static const D3D12_BLEND_DESC NonPremultiplied =
        MakeBlend(TRUE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA);
};

}  // namespace DirectX
