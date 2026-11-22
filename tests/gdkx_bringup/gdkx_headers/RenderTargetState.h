#pragma once
#include <d3d12_xs.h>

#include <cstdint>

// MIT DirectXTK12 RenderTargetState subset for untouched Xbox-GDK-Samples.
namespace DirectX {

class RenderTargetState {
public:
    RenderTargetState() noexcept
        : sampleMask(~0u),
          numRenderTargets(0u),
          rtvFormats{},
          dsvFormat(DXGI_FORMAT_UNKNOWN),
          sampleDesc{1u, 0u},
          nodeMask(0u) {}

    RenderTargetState(DXGI_FORMAT rtFormat, DXGI_FORMAT dsFormat) noexcept
        : sampleMask(~0u),
          numRenderTargets(1u),
          rtvFormats{},
          dsvFormat(dsFormat),
          sampleDesc{1u, 0u},
          nodeMask(0u) {
        rtvFormats[0] = rtFormat;
    }

    uint32_t sampleMask;
    uint32_t numRenderTargets;
    DXGI_FORMAT rtvFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    DXGI_FORMAT dsvFormat;
    DXGI_SAMPLE_DESC sampleDesc;
    uint32_t nodeMask;
};

}  // namespace DirectX
