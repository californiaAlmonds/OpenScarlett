#pragma once
#include <d3d12_xs.h>
#include <DirectXMath.h>

// Clean-room MIT DirectXTK12 DirectXHelpers subset for untouched Xbox-GDK-Samples.

namespace DirectX {

inline void TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter) noexcept {
    if (!commandList || !resource || stateBefore == stateAfter) {
        return;
    }
    D3D12_RESOURCE_BARRIER desc = {};
    desc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    desc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    desc.Transition.pResource = resource;
    desc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    desc.Transition.StateBefore = stateBefore;
    desc.Transition.StateAfter = stateAfter;
    commandList->ResourceBarrier(1u, &desc);
}

inline XMUINT2 GetTextureSize(ID3D12Resource* res) noexcept {
    if (!res) {
        return XMUINT2(0u, 0u);
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    return XMUINT2(static_cast<uint32_t>(desc.Width), static_cast<uint32_t>(desc.Height));
}

inline void CreateShaderResourceView(
    ID3D12Device* device,
    ID3D12Resource* tex,
    D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptor,
    bool isCubeMap = false) noexcept {
    if (!device || !tex) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (isCubeMap) {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = desc.MipLevels;
    } else {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
    }
    device->CreateShaderResourceView(tex, &srvDesc, srvDescriptor);
}

}  // namespace DirectX
