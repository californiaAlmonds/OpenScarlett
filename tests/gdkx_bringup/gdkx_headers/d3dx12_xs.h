#pragma once
#include <d3d12_xs.h>

#include <cstddef>
#include <cstring>
#include <vector>

// MIT D3DX12 helper subset for untouched Xbox-GDK-Samples.

struct CD3DX12_DEFAULT {};
extern const DECLSPEC_SELECTANY CD3DX12_DEFAULT D3D12_DEFAULT;

struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type) noexcept {
        Type = type;
        CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        CreationNodeMask = 1;
        VisibleNodeMask = 1;
    }
};

struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC {
    CD3DX12_RESOURCE_DESC() = default;
    CD3DX12_RESOURCE_DESC(const D3D12_RESOURCE_DESC& o) noexcept : D3D12_RESOURCE_DESC(o) {}

    static CD3DX12_RESOURCE_DESC Tex2D(
        DXGI_FORMAT fmt, UINT64 w, UINT h, UINT16 arr = 1, UINT16 mips = 0,
        UINT sampleCount = 1, UINT sampleQuality = 0,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        UINT64 alignment = 0) noexcept {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Alignment = alignment;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = arr;
        d.MipLevels = mips;
        d.Format = fmt;
        d.SampleDesc.Count = sampleCount;
        d.SampleDesc.Quality = sampleQuality;
        d.Layout = layout;
        d.Flags = flags;
        return CD3DX12_RESOURCE_DESC(d);
    }

    static CD3DX12_RESOURCE_DESC Buffer(
        UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
        UINT64 alignment = 0) noexcept {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Alignment = alignment;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return CD3DX12_RESOURCE_DESC(d);
    }

    static CD3DX12_RESOURCE_DESC Buffer(
        const D3D12_RESOURCE_ALLOCATION_INFO& resAllocInfo,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
        return Buffer(resAllocInfo.SizeInBytes, flags, resAllocInfo.Alignment);
    }
};

struct CD3DX12_SUBRESOURCE_FOOTPRINT : public D3D12_SUBRESOURCE_FOOTPRINT {
    CD3DX12_SUBRESOURCE_FOOTPRINT() = default;
    explicit CD3DX12_SUBRESOURCE_FOOTPRINT(const D3D12_SUBRESOURCE_FOOTPRINT& o) noexcept
        : D3D12_SUBRESOURCE_FOOTPRINT(o) {}
    CD3DX12_SUBRESOURCE_FOOTPRINT(const D3D12_RESOURCE_DESC& resDesc, UINT rowPitch) noexcept {
        Format = resDesc.Format;
        Width = static_cast<UINT>(resDesc.Width);
        Height = resDesc.Height;
        Depth = (resDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? resDesc.DepthOrArraySize : 1u;
        RowPitch = rowPitch;
    }
};

struct CD3DX12_CLEAR_VALUE : public D3D12_CLEAR_VALUE {
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT fmt, const FLOAT color[4]) noexcept {
        Format = fmt;
        for (int i = 0; i < 4; ++i) Color[i] = color[i];
    }
    CD3DX12_CLEAR_VALUE(DXGI_FORMAT fmt, FLOAT depth, UINT8 stencil) noexcept {
        Format = fmt;
        DepthStencil.Depth = depth;
        DepthStencil.Stencil = stencil;
    }
};

struct CD3DX12_CPU_DESCRIPTOR_HANDLE : public D3D12_CPU_DESCRIPTOR_HANDLE {
    CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
    explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept { ptr = h.ptr; }
    CD3DX12_CPU_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE h, INT offset, UINT inc) noexcept {
        ptr = static_cast<SIZE_T>(
            static_cast<INT64>(h.ptr) + static_cast<INT64>(offset) * static_cast<INT64>(inc));
    }
};

struct CD3DX12_RESOURCE_BARRIER : public D3D12_RESOURCE_BARRIER {
    static D3D12_RESOURCE_BARRIER Transition(
        ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE) noexcept {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = flags;
        b.Transition.pResource = r;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = subresource;
        return b;
    }
};

struct CD3DX12_RASTERIZER_DESC : public D3D12_RASTERIZER_DESC {
    explicit CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT) noexcept {
        FillMode = D3D12_FILL_MODE_SOLID;
        CullMode = D3D12_CULL_MODE_BACK;
        FrontCounterClockwise = FALSE;
        DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        DepthClipEnable = TRUE;
        MultisampleEnable = FALSE;
        AntialiasedLineEnable = FALSE;
        ForcedSampleCount = 0;
        ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
};

struct CD3DX12_BLEND_DESC : public D3D12_BLEND_DESC {
    explicit CD3DX12_BLEND_DESC(CD3DX12_DEFAULT) noexcept {
        AlphaToCoverageEnable = FALSE;
        IndependentBlendEnable = FALSE;
        for (int i = 0; i < 8; ++i) {
            RenderTarget[i] = {
                FALSE, FALSE,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_LOGIC_OP_NOOP,
                D3D12_COLOR_WRITE_ENABLE_ALL};
        }
    }
};

struct CD3DX12_DEPTH_STENCIL_DESC : public D3D12_DEPTH_STENCIL_DESC {
    explicit CD3DX12_DEPTH_STENCIL_DESC(CD3DX12_DEFAULT) noexcept {
        DepthEnable = TRUE;
        DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        StencilEnable = FALSE;
        StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC defaultOp = {
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS};
        FrontFace = defaultOp;
        BackFace = defaultOp;
    }
};

struct CD3DX12_RANGE : public D3D12_RANGE {
    CD3DX12_RANGE(SIZE_T b, SIZE_T e) noexcept {
        Begin = b;
        End = e;
    }
};

template <typename T>
inline ID3D12CommandList* const* CommandListCast(T* const* pp) noexcept {
    return reinterpret_cast<ID3D12CommandList* const*>(pp);
}

inline UINT64 GetRequiredIntermediateSize(
    ID3D12Resource* pDestinationResource,
    UINT FirstSubresource,
    UINT NumSubresources) noexcept {
    if (!pDestinationResource || NumSubresources == 0u) return 0u;
    const D3D12_RESOURCE_DESC desc = pDestinationResource->GetDesc();
    ID3D12Device* pDevice = nullptr;
    if (FAILED(pDestinationResource->GetDevice(__uuidof(*pDevice), reinterpret_cast<void**>(&pDevice))) || !pDevice) {
        return 0u;
    }
    UINT64 requiredSize = 0u;
    pDevice->GetCopyableFootprints(
        &desc, FirstSubresource, NumSubresources, 0u, nullptr, nullptr, nullptr, &requiredSize);
    pDevice->Release();
    return requiredSize;
}

inline UINT64 UpdateSubresources(
    ID3D12GraphicsCommandList* pCmdList,
    ID3D12Resource* pDestinationResource,
    ID3D12Resource* pIntermediate,
    UINT64 IntermediateOffset,
    UINT FirstSubresource,
    UINT NumSubresources,
    const D3D12_SUBRESOURCE_DATA* pSrcData) noexcept {
    if (!pCmdList || !pDestinationResource || !pIntermediate || !pSrcData || NumSubresources == 0u) {
        return 0u;
    }

    const D3D12_RESOURCE_DESC dstDesc = pDestinationResource->GetDesc();
    const D3D12_RESOURCE_DESC intermediateDesc = pIntermediate->GetDesc();
    if (intermediateDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return 0u;
    }

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pDestinationResource->GetDevice(__uuidof(*pDevice), reinterpret_cast<void**>(&pDevice))) || !pDevice) {
        return 0u;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(NumSubresources);
    std::vector<UINT> numRows(NumSubresources, 0u);
    std::vector<UINT64> rowSizesInBytes(NumSubresources, 0u);
    UINT64 requiredSize = 0u;
    pDevice->GetCopyableFootprints(
        &dstDesc, FirstSubresource, NumSubresources, IntermediateOffset,
        layouts.data(), numRows.data(), rowSizesInBytes.data(), &requiredSize);
    pDevice->Release();

    if (requiredSize == 0u || intermediateDesc.Width < requiredSize + IntermediateOffset) {
        return 0u;
    }

    BYTE* pData = nullptr;
    if (FAILED(pIntermediate->Map(0u, nullptr, reinterpret_cast<void**>(&pData))) || !pData) {
        return 0u;
    }

    for (UINT i = 0u; i < NumSubresources; ++i) {
        if (rowSizesInBytes[i] > static_cast<UINT64>(SIZE_MAX)) {
            pIntermediate->Unmap(0u, nullptr);
            return 0u;
        }
        const UINT64 dstSlicePitch =
            static_cast<UINT64>(layouts[i].Footprint.RowPitch) * static_cast<UINT64>(numRows[i]);
        const auto srcRowPitch = static_cast<ptrdiff_t>(pSrcData[i].RowPitch);
        const auto srcSlicePitch = static_cast<ptrdiff_t>(pSrcData[i].SlicePitch);
        const auto* srcBase = static_cast<const BYTE*>(pSrcData[i].pData);
        for (UINT z = 0u; z < layouts[i].Footprint.Depth; ++z) {
            BYTE* dstSlice = pData + layouts[i].Offset + dstSlicePitch * static_cast<UINT64>(z);
            const BYTE* srcSlice = srcBase + srcSlicePitch * static_cast<ptrdiff_t>(z);
            for (UINT y = 0u; y < numRows[i]; ++y) {
                std::memcpy(
                    dstSlice + static_cast<UINT64>(layouts[i].Footprint.RowPitch) * static_cast<UINT64>(y),
                    srcSlice + srcRowPitch * static_cast<ptrdiff_t>(y),
                    static_cast<size_t>(rowSizesInBytes[i]));
            }
        }
    }
    pIntermediate->Unmap(0u, nullptr);

    if (dstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        pCmdList->CopyBufferRegion(
            pDestinationResource, 0u, pIntermediate, layouts[0].Offset, layouts[0].Footprint.Width);
    } else {
        for (UINT i = 0u; i < NumSubresources; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = pDestinationResource;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = FirstSubresource + i;

            D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
            srcLoc.pResource = pIntermediate;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = layouts[i];

            pCmdList->CopyTextureRegion(&dstLoc, 0u, 0u, 0u, &srcLoc, nullptr);
        }
    }
    return requiredSize;
}
