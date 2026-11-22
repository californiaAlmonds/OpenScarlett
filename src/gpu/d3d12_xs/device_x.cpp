#include "device_x.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "command_list_x.h"
#include "command_queue_x.h"
#include "proxies_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"

#include <d3dcompiler.h>

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<bool> g_force_r9g9b9e5_uav_fallback{false};
std::atomic<uint32_t> g_backend_mode{D3D12XBOX_BACKEND_AUTO};

bool EqualsAsciiCaseInsensitive(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

bool EnvVarEqualsIgnoreCase(const char* env_name, const char* expected) noexcept {
    char buf[128]{};
    const DWORD len = GetEnvironmentVariableA(env_name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    // Trim leading/trailing ASCII whitespace.
    size_t start = 0u;
    size_t end = len;
    while (start < end && (buf[start] == ' ' || buf[start] == '\t')) ++start;
    while (end > start && (buf[end - 1u] == ' ' || buf[end - 1u] == '\t' || buf[end - 1u] == '\r' || buf[end - 1u] == '\n')) --end;
    buf[end] = '\0';
    return EqualsAsciiCaseInsensitive(buf + start, expected);
}

bool IsRawSpirvBytecode(const void* data, size_t size) noexcept {
    if (!data || size < 20u || (size % 4u) != 0u) return false;
    uint32_t magic = 0u;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == openscarlett::kSpirvMagicNumber;
}

std::vector<uint32_t> CopySpirvWords(const void* data, size_t size) {
    std::vector<uint32_t> out(size / 4u, 0u);
    if (!out.empty() && data) {
        std::memcpy(out.data(), data, out.size() * sizeof(uint32_t));
    }
    return out;
}

openscarlett::VulkanVertexFormat DxgiToVulkanVertexFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_UINT: return openscarlett::VulkanVertexFormat::R32Uint;
        case DXGI_FORMAT_R32_FLOAT: return openscarlett::VulkanVertexFormat::R32Sfloat;
        case DXGI_FORMAT_R32G32_FLOAT: return openscarlett::VulkanVertexFormat::R32G32Sfloat;
        case DXGI_FORMAT_R32G32B32_FLOAT: return openscarlett::VulkanVertexFormat::R32G32B32Sfloat;
        case DXGI_FORMAT_R32G32B32A32_UINT: return openscarlett::VulkanVertexFormat::R32G32B32A32Uint;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        default:
            return openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat;
    }
}

uint32_t DxgiVertexFormatBytes(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_FLOAT: return 4u;
        case DXGI_FORMAT_R32G32_FLOAT: return 8u;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12u;
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        default:
            return 16u;
    }
}

openscarlett::VulkanImageFormat DxgiToVulkanColorFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return openscarlett::VulkanImageFormat::Bgra8Unorm;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return openscarlett::VulkanImageFormat::Rgba16Sfloat;
        default:
            return openscarlett::VulkanImageFormat::Rgba8Unorm;
    }
}

struct FormatFootprintSpec {
    uint32_t block_width = 1u;
    uint32_t block_height = 1u;
    uint32_t bytes_per_block = 4u;
};

FormatFootprintSpec DescribeFormatForFootprint(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return {4u, 4u, 8u};
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return {4u, 4u, 16u};
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return {1u, 1u, 16u};
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
            return {1u, 1u, 12u};
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return {1u, 1u, 8u};
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return {1u, 1u, 4u};
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return {1u, 1u, 2u};
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return {1u, 1u, 1u};
        case DXGI_FORMAT_UNKNOWN:
            return {1u, 1u, 1u};
        default:
            // Safe 32-bit fallback for exotic/Xbox-specific DXGI formats refused by desktop D3D12.
            return {1u, 1u, 4u};
    }
}

void ComputeFallbackCopyableFootprints(
    const D3D12_RESOURCE_DESC& desc,
    UINT first_subresource,
    UINT num_subresources,
    UINT64 base_offset,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
    UINT* pNumRows,
    UINT64* pRowSizeInBytes,
    UINT64* pTotalBytes) noexcept {
    const uint32_t mip_levels = (desc.MipLevels > 0u) ? desc.MipLevels : 1u;
    const FormatFootprintSpec spec = DescribeFormatForFootprint(desc.Format);
    uint64_t total = 0ull;

    for (UINT i = 0u; i < num_subresources; ++i) {
        if (i > 0u) {
            total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1ull) &
                    ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1ull);
        }
        const UINT sub = first_subresource + i;
        const uint32_t mip = sub % mip_levels;

        uint64_t mip_w = 1ull;
        uint32_t mip_h = 1u;
        uint32_t mip_d = 1u;
        uint64_t row_bytes = 0ull;
        uint32_t num_rows = 1u;
        uint32_t row_pitch = 0u;

        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            mip_w = std::max<uint64_t>(1ull, desc.Width);
            mip_h = 1u;
            mip_d = 1u;
            row_bytes = mip_w;
            num_rows = 1u;
            row_pitch = static_cast<uint32_t>(
                (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1ull) &
                ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1ull));
        } else {
            mip_w = std::max<uint64_t>(1ull, desc.Width >> mip);
            mip_h = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                        ? 1u
                        : std::max<uint32_t>(1u, desc.Height >> mip);
            mip_d = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                        ? std::max<uint32_t>(1u, static_cast<uint32_t>(desc.DepthOrArraySize) >> mip)
                        : 1u;

            const uint64_t blocks_x = (mip_w + spec.block_width - 1ull) / spec.block_width;
            const uint32_t blocks_y = (mip_h + spec.block_height - 1u) / spec.block_height;
            row_bytes = std::max<uint64_t>(1ull, blocks_x) * spec.bytes_per_block;
            num_rows = std::max<uint32_t>(1u, blocks_y);
            row_pitch = static_cast<uint32_t>(
                (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1ull) &
                ~(static_cast<uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1ull));
        }

        if (pLayouts) {
            pLayouts[i].Offset = base_offset + total;
            pLayouts[i].Footprint.Format = desc.Format;
            pLayouts[i].Footprint.Width = static_cast<UINT>(mip_w);
            pLayouts[i].Footprint.Height = mip_h;
            pLayouts[i].Footprint.Depth = mip_d;
            pLayouts[i].Footprint.RowPitch = row_pitch;
        }
        if (pNumRows) {
            pNumRows[i] = num_rows;
        }
        if (pRowSizeInBytes) {
            pRowSizeInBytes[i] = row_bytes;
        }

        const uint64_t sub_bytes =
            (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? row_bytes
                : (static_cast<uint64_t>(row_pitch) * static_cast<uint64_t>(num_rows) *
                   static_cast<uint64_t>(mip_d));
        total += sub_bytes;
    }

    if (pTotalBytes) {
        *pTotalBytes = total;
    }
}

inline void RecordDev(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        outcome,
        static_cast<int32_t>(hr),
        slot);
}

DXGI_FORMAT MapTypelessFormatForSrv(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8_TYPELESS:
            return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_TYPELESS:
            return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_BC1_TYPELESS:
            return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:
            return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:
            return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:
            return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:
            return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:
            return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:
            return DXGI_FORMAT_BC7_UNORM;
        default:
            return fmt;
    }
}

bool IsReadableGuestVaRange(uint64_t va, uint64_t byte_size) noexcept {
    if (va == 0ull || byte_size == 0ull) return false;
    if (va > UINT64_MAX - byte_size) return false;
    uint64_t cur = va;
    const uint64_t end = va + byte_size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cur)),
                         &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kReadableMask =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadableMask) == 0u ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            return false;
        }
        const uint64_t region_end =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
            static_cast<uint64_t>(mbi.RegionSize);
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

void UnwrapPageables(
    UINT count,
    ID3D12Pageable* const* src,
    std::vector<ID3D12Pageable*>& dst) {
    dst.resize(count);
    for (UINT i = 0; i < count; ++i) {
        dst[i] = CommandQueueX::UnwrapHostPageable(UnwrapProxyPageable(src[i]));
    }
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // D3D12 pipeline state stream subobjects require pointer alignment
#endif
template <typename T>
struct alignas(void*) StreamSubobjLayout {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool IsShaderSubobjectType(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) noexcept {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            return true;
        default:
            return false;
    }
}

size_t GetSubobjectStrideBytes(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) noexcept {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            return sizeof(StreamSubobjLayout<ID3D12RootSignature*>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            return sizeof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            return sizeof(StreamSubobjLayout<D3D12_STREAM_OUTPUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            return sizeof(StreamSubobjLayout<D3D12_BLEND_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            return sizeof(StreamSubobjLayout<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            return sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            return sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            return sizeof(StreamSubobjLayout<D3D12_INPUT_LAYOUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            return sizeof(StreamSubobjLayout<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            return sizeof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            return sizeof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            return sizeof(StreamSubobjLayout<DXGI_FORMAT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            return sizeof(StreamSubobjLayout<DXGI_SAMPLE_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            return sizeof(StreamSubobjLayout<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            return sizeof(StreamSubobjLayout<D3D12_CACHED_PIPELINE_STATE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            return sizeof(StreamSubobjLayout<D3D12_PIPELINE_STATE_FLAGS>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            return sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            return sizeof(StreamSubobjLayout<D3D12_VIEW_INSTANCING_DESC>);
        default:
            return 0u;
    }
}

D3D12_HEAP_FLAGS SanitizeXboxHeapFlags(D3D12_HEAP_FLAGS flags) noexcept {
    constexpr uint32_t kMaskOut = static_cast<uint32_t>(D3D12_HEAP_FLAG_ALLOW_DISPLAY);
    return static_cast<D3D12_HEAP_FLAGS>(static_cast<uint32_t>(flags) & ~kMaskOut);
}

XG_TILE_MODE ExtractXboxGfx10TileMode(D3D12_TEXTURE_LAYOUT layout) noexcept {
    const uint32_t raw = static_cast<uint32_t>(layout);
    if (raw >= D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) {
        const uint32_t code = raw - D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE;
        if (code <= static_cast<uint32_t>(XG_TILE_MODE_3D_THICK_64KB_R_X)) {
            return static_cast<XG_TILE_MODE>(code);
        }
    }
    return XG_TILE_MODE_LINEAR;
}

D3D12_RESOURCE_DESC SanitizeXboxResourceDesc(const D3D12_RESOURCE_DESC& desc) noexcept {
    D3D12_RESOURCE_DESC out = desc;
    constexpr uint32_t kDesktopResourceFlagsMask = 0x000000FFu;
    out.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(desc.Flags) & kDesktopResourceFlagsMask);
    if (static_cast<uint32_t>(out.Layout) >= D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) {
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    return out;
}

D3D12_RESOURCE_DESC1 SanitizeXboxResourceDesc1(const D3D12_RESOURCE_DESC1& desc) noexcept {
    D3D12_RESOURCE_DESC1 out = desc;
    constexpr uint32_t kDesktopResourceFlagsMask = 0x000000FFu;
    out.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(desc.Flags) & kDesktopResourceFlagsMask);
    if (static_cast<uint32_t>(out.Layout) >= D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) {
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    return out;
}

// OpenScarlett software raytracing serialization driver GUID.
inline constexpr GUID kOpenScarlettSwAsDriverGuid = {
    0x4F534431u, 0x3258u, 0x4458u, {0x52u, 0x53u, 0x57u, 0x41u, 0x53u, 0x00u, 0x00u, 0x01u}};

// SM 5.0 compute shaders for fallback Sampler Feedback DECODE (u32 UAV -> R8 packed rows)
// and ENCODE (R8 packed rows -> u32 UAV) so ResolveSubresourceRegion works live on GPU & WARP.
constexpr const char* kFeedbackDecodeToR8Hlsl = R"(
cbuffer DecodeParams : register(b0) {
    uint tiles_x;
    uint tiles_y;
    uint row_pitch_bytes;
    uint slice_index;
};
ByteAddressBuffer src_u32 : register(t0);
RWByteAddressBuffer dst_r8 : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint row_dwords = (row_pitch_bytes + 3u) >> 2u;
    uint total_dwords = row_dwords * tiles_y;
    uint idx = tid.x;
    if (idx >= total_dwords) return;
    uint y = idx / row_dwords;
    uint dw_x = idx % row_dwords;
    uint x0 = dw_x * 4u;
    uint packed = 0u;
    [unroll]
    for (uint b = 0u; b < 4u; ++b) {
        uint x = x0 + b;
        uint val = 0xFFu;
        if (x < tiles_x) {
            uint elem = (slice_index * tiles_y + y) * tiles_x + x;
            uint raw = src_u32.Load(elem * 4u);
            val = (raw >= 0xFFu) ? 0xFFu : (raw & 0xFFu);
        }
        packed |= ((val & 0xFFu) << (b * 8u));
    }
    dst_r8.Store(y * row_pitch_bytes + dw_x * 4u, packed);
}
)";

constexpr const char* kFeedbackEncodeFromR8Hlsl = R"(
cbuffer EncodeParams : register(b0) {
    uint tiles_x;
    uint tiles_y;
    uint row_pitch_bytes;
    uint slice_index;
};
ByteAddressBuffer src_r8 : register(t0);
RWByteAddressBuffer dst_u32 : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint total = tiles_x * tiles_y;
    uint idx = tid.x;
    if (idx >= total) return;
    uint y = idx / tiles_x;
    uint x = idx % tiles_x;
    uint byte_off = y * row_pitch_bytes + x;
    uint word = src_r8.Load(byte_off & ~3u);
    uint val = (word >> ((byte_off & 3u) * 8u)) & 0xFFu;
    uint dst_elem = (slice_index * tiles_y + y) * tiles_x + x;
    dst_u32.Store(dst_elem * 4u, val);
}
)";

D3D12_RESOURCE_DESC Desc1ToDesc0(const D3D12_RESOURCE_DESC1& d1) noexcept {
    D3D12_RESOURCE_DESC d0{};
    d0.Dimension = d1.Dimension;
    d0.Alignment = d1.Alignment;
    d0.Width = d1.Width;
    d0.Height = d1.Height;
    d0.DepthOrArraySize = d1.DepthOrArraySize;
    d0.MipLevels = d1.MipLevels;
    d0.Format = d1.Format;
    d0.SampleDesc = d1.SampleDesc;
    d0.Layout = d1.Layout;
    d0.Flags = d1.Flags;
    return d0;
}

// Guard against raw-host escape for ANY ID3D12DeviceN IID (including future/higher SDK versions
// like ID3D12Device13/14 that might not be wrapped or supported by the host runtime).
bool IsKnownD3D12DeviceIid(REFIID riid) noexcept {
    static constexpr GUID kIidDevice7  = {0x5c014b53u, 0x68a1u, 0x4b9bu, {0x8bu, 0xd1u, 0xddu, 0x60u, 0x46u, 0xb9u, 0x35u, 0x8bu}};
    static constexpr GUID kIidDevice8  = {0x9218e6bbu, 0xf944u, 0x4f7eu, {0xa7u, 0x5cu, 0xb1u, 0xb2u, 0xc7u, 0xb7u, 0x01u, 0xf3u}};
    static constexpr GUID kIidDevice9  = {0x4c80e962u, 0xf032u, 0x4f60u, {0xbcu, 0x9eu, 0xebu, 0xc2u, 0xcfu, 0xa1u, 0xd8u, 0x3cu}};
    static constexpr GUID kIidDevice10 = {0x517f8718u, 0xaa66u, 0x49f9u, {0xb0u, 0x2bu, 0xa7u, 0xabu, 0x89u, 0xc0u, 0x60u, 0x31u}};
    static constexpr GUID kIidDevice11 = {0x5405c344u, 0xd457u, 0x444eu, {0xb4u, 0xddu, 0x23u, 0x66u, 0xe4u, 0x5au, 0xeeu, 0x39u}};
    static constexpr GUID kIidDevice12 = {0x5af5c532u, 0x4c91u, 0x4cd0u, {0xb5u, 0x41u, 0x15u, 0xa4u, 0x05u, 0x39u, 0x5fu, 0xc5u}};
    static constexpr GUID kIidDevice13 = {0x14eecffcu, 0x4df8u, 0x40f7u, {0xa1u, 0x18u, 0x5cu, 0x81u, 0x6fu, 0x45u, 0x69u, 0x5eu}};
    static constexpr GUID kIidDevice14 = {0x5f6c17beu, 0x6357u, 0x4c58u, {0x8bu, 0x68u, 0x4du, 0xc2u, 0x26u, 0x44u, 0xf8u, 0x92u}};
    return IsEqualIID(riid, OS_D3D12_IID(ID3D12Device)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device1)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device2)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device3)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device4)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device5)) ||
           IsEqualIID(riid, OS_D3D12_IID(ID3D12Device6)) ||
           IsEqualIID(riid, kIidDevice7) ||
           IsEqualIID(riid, kIidDevice8) ||
           IsEqualIID(riid, kIidDevice9) ||
           IsEqualIID(riid, kIidDevice10) ||
           IsEqualIID(riid, kIidDevice11) ||
           IsEqualIID(riid, kIidDevice12) ||
           IsEqualIID(riid, kIidDevice13) ||
           IsEqualIID(riid, kIidDevice14);
}

// Synthetic virtual IDXGIOutput returned when the host adapter has no physical display outputs
// (e.g. Optimus render-only dGPU, WARP, or headless CI runners).
class VirtualDxgiOutput final : public IDXGIOutput {
public:
    explicit VirtualDxgiOutput(IDXGIAdapter* parent_adapter) : parent_adapter_(parent_adapter) {
        if (parent_adapter_) parent_adapter_->AddRef();
    }
    ~VirtualDxgiOutput() {
        if (parent_adapter_) parent_adapter_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
            IsEqualIID(riid, OS_D3D12_IID(IDXGIObject)) ||
            IsEqualIID(riid, OS_D3D12_IID(IDXGIOutput))) {
            *ppvObject = static_cast<IDXGIOutput*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return parent_adapter_ ? parent_adapter_->QueryInterface(riid, ppParent) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        std::memset(pDesc, 0, sizeof(*pDesc));
        const wchar_t kName[] = L"\\\\.\\DISPLAY1";
        std::memcpy(pDesc->DeviceName, kName, sizeof(kName));
        pDesc->DesktopCoordinates = {0, 0, 1920, 1080};
        pDesc->AttachedToDesktop = TRUE;
        pDesc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        pDesc->Monitor = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(
        DXGI_FORMAT EnumFormat, UINT, UINT* pNumModes, DXGI_MODE_DESC* pDesc) override {
        if (!pNumModes) return E_INVALIDARG;
        if (!pDesc) {
            *pNumModes = 1u;
            return S_OK;
        }
        if (*pNumModes == 0u) return DXGI_ERROR_MORE_DATA;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc[0].Width = 1920u;
        pDesc[0].Height = 1080u;
        pDesc[0].RefreshRate = {60u, 1u};
        pDesc[0].Format = EnumFormat;
        *pNumModes = 1u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(
        const DXGI_MODE_DESC* pModeToMatch, DXGI_MODE_DESC* pClosestMatch, IUnknown*) override {
        if (!pClosestMatch) return E_INVALIDARG;
        if (pModeToMatch) {
            *pClosestMatch = *pModeToMatch;
        } else {
            std::memset(pClosestMatch, 0, sizeof(*pClosestMatch));
        }
        if (pClosestMatch->Width == 0u) pClosestMatch->Width = 1920u;
        if (pClosestMatch->Height == 0u) pClosestMatch->Height = 1080u;
        if (pClosestMatch->RefreshRate.Numerator == 0u) {
            pClosestMatch->RefreshRate = {60u, 1u};
        }
        if (pClosestMatch->Format == DXGI_FORMAT_UNKNOWN) {
            pClosestMatch->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { return S_OK; }
    void STDMETHODCALLTYPE ReleaseOwnership() override {}
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pCaps) override {
        if (!pCaps) return E_INVALIDARG;
        std::memset(pCaps, 0, sizeof(*pCaps));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) override {
        if (!pArray) return E_INVALIDARG;
        std::memset(pArray, 0, sizeof(*pArray));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        if (!pStats) return E_INVALIDARG;
        std::memset(pStats, 0, sizeof(*pStats));
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IDXGIAdapter* parent_adapter_ = nullptr;
};

class DxgiAdapterProxy final : public IDXGIAdapter1 {
public:
    DxgiAdapterProxy(IDXGIAdapter1* real_adapter, const D3D12AdapterInfo& info)
        : real_adapter_(real_adapter), info_(info) {
        if (real_adapter_) real_adapter_->AddRef();
    }
    ~DxgiAdapterProxy() {
        if (real_adapter_) real_adapter_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
            IsEqualIID(riid, OS_D3D12_IID(IDXGIObject)) ||
            IsEqualIID(riid, OS_D3D12_IID(IDXGIAdapter)) ||
            IsEqualIID(riid, OS_D3D12_IID(IDXGIAdapter1))) {
            *ppvObject = static_cast<IDXGIAdapter1*>(this);
            AddRef();
            return S_OK;
        }
        return real_adapter_ ? real_adapter_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return real_adapter_ ? real_adapter_->SetPrivateData(Name, DataSize, pData) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return real_adapter_ ? real_adapter_->SetPrivateDataInterface(Name, pUnknown) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return real_adapter_ ? real_adapter_->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return real_adapter_ ? real_adapter_->GetParent(riid, ppParent) : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override {
        if (!ppOutput) return E_POINTER;
        *ppOutput = nullptr;
        if (real_adapter_) {
            const HRESULT hr = real_adapter_->EnumOutputs(Output, ppOutput);
            if (SUCCEEDED(hr) && *ppOutput) {
                return S_OK;
            }
        }
        if (Output == 0u) {
            auto* vout = new (std::nothrow) VirtualDxgiOutput(this);
            if (!vout) return E_OUTOFMEMORY;
            *ppOutput = vout;
            return S_OK;
        }
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        if (real_adapter_) return real_adapter_->GetDesc(pDesc);
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->VendorId = info_.vendor_id;
        pDesc->DeviceId = info_.device_id;
        pDesc->DedicatedVideoMemory = static_cast<SIZE_T>(info_.dedicated_video_memory_bytes);
        pDesc->AdapterLuid.LowPart = info_.luid_low;
        pDesc->AdapterLuid.HighPart = info_.luid_high;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override {
        return real_adapter_
                   ? real_adapter_->CheckInterfaceSupport(InterfaceName, pUMDVersion)
                   : DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
        if (!pDesc) return E_INVALIDARG;
        if (real_adapter_) return real_adapter_->GetDesc1(pDesc);
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->VendorId = info_.vendor_id;
        pDesc->DeviceId = info_.device_id;
        pDesc->DedicatedVideoMemory = static_cast<SIZE_T>(info_.dedicated_video_memory_bytes);
        pDesc->AdapterLuid.LowPart = info_.luid_low;
        pDesc->AdapterLuid.HighPart = info_.luid_high;
        pDesc->Flags = info_.is_warp ? DXGI_ADAPTER_FLAG_SOFTWARE : DXGI_ADAPTER_FLAG_NONE;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IDXGIAdapter1* real_adapter_ = nullptr;
    D3D12AdapterInfo info_{};
};

}  // namespace

// Lightweight IDXGIDevice1 sub-interface exposed by DeviceX::QueryInterface so real Xbox
// DeviceResources.cpp (`m_d3dDevice.As(&dxgiDevice) -> GetAdapter() -> EnumOutputs(0)`) works.
class DxgiDeviceProxy final : public IDXGIDevice1 {
public:
    explicit DxgiDeviceProxy(DeviceX* parent) : parent_(parent) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        return parent_->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return parent_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return parent_->Release(); }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return parent_->SetPrivateData(Name, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return parent_->SetPrivateDataInterface(Name, pUnknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return parent_->GetPrivateData(Name, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        if (!ppParent) return E_POINTER;
        *ppParent = nullptr;
        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = GetAdapter(&adapter);
        if (FAILED(hr) || !adapter) return hr;
        hr = adapter->QueryInterface(riid, ppParent);
        if (FAILED(hr)) {
            hr = adapter->GetParent(riid, ppParent);
        }
        adapter->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override {
        if (!pAdapter) return E_POINTER;
        *pAdapter = nullptr;
        IDXGIAdapter1* real_adapter = nullptr;
        if (parent_->HostDevice()) {
            const LUID luid = parent_->HostDevice()->GetAdapterLuid();
            IDXGIFactory4* factory4 = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(OS_D3D12_IID(IDXGIFactory4), reinterpret_cast<void**>(&factory4))) &&
                factory4) {
                factory4->EnumAdapterByLuid(
                    luid, OS_D3D12_IID(IDXGIAdapter1), reinterpret_cast<void**>(&real_adapter));
                factory4->Release();
            }
        }
        auto* proxy = new (std::nothrow) DxgiAdapterProxy(real_adapter, parent_->AdapterInfo());
        if (real_adapter) real_adapter->Release();
        if (!proxy) return E_OUTOFMEMORY;
        *pAdapter = proxy;
        RecordDev("ID3D12DeviceX::DxgiGetAdapter", -1, S_OK);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(
        const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*, IDXGISurface** ppSurface) override {
        if (ppSurface) *ppSurface = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryResourceResidency(
        IUnknown* const*, DXGI_RESIDENCY* pResidencyStatus, UINT NumResources) override {
        if (pResidencyStatus) {
            for (UINT i = 0; i < NumResources; ++i) {
                pResidencyStatus[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override {
        gpu_priority_.store(Priority, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override {
        if (!pPriority) return E_POINTER;
        *pPriority = gpu_priority_.load(std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
        max_latency_.store(MaxLatency == 0u ? 3u : MaxLatency, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
        if (!pMaxLatency) return E_POINTER;
        *pMaxLatency = max_latency_.load(std::memory_order_relaxed);
        return S_OK;
    }

private:
    DeviceX* parent_ = nullptr;
    std::atomic<INT> gpu_priority_{0};
    std::atomic<UINT> max_latency_{3u};
};

// Secondary COM sub-interface wrapping ID3D12Device9..ID3D12Device12 (when defined by SDK)
// so DeviceX's primary vtable keeps GDKX ID3D12Device8 Xbox methods at slots 73..75 while
// QI for ID3D12Device9..12 returns a wrapped DeviceX proxy with zero raw-host escape.
#if defined(__ID3D12Device12_INTERFACE_DEFINED__)
using DeviceExtendedBase = ID3D12Device12;
#elif defined(__ID3D12Device11_INTERFACE_DEFINED__)
using DeviceExtendedBase = ID3D12Device11;
#elif defined(__ID3D12Device10_INTERFACE_DEFINED__)
using DeviceExtendedBase = ID3D12Device10;
#elif defined(__ID3D12Device9_INTERFACE_DEFINED__)
using DeviceExtendedBase = ID3D12Device9;
#endif

#if defined(__ID3D12Device9_INTERFACE_DEFINED__)
class DeviceExtendedProxy final : public DeviceExtendedBase {
public:
    explicit DeviceExtendedProxy(DeviceX* parent) : parent_(parent) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        return parent_->QueryInterface(riid, ppvObject);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return parent_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return parent_->Release(); }

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { return parent_->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { return parent_->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) override { return parent_->SetPrivateDataInterface(g, d); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return parent_->SetName(n); }

    // ID3D12Device
    UINT STDMETHODCALLTYPE GetNodeCount() override { return parent_->GetNodeCount(); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** q) override { return parent_->CreateCommandQueue(d, r, q); }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID r, void** a) override { return parent_->CreateCommandAllocator(t, r, a); }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID r, void** p) override { return parent_->CreateGraphicsPipelineState(d, r, p); }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID r, void** p) override { return parent_->CreateComputePipelineState(d, r, p); }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT m, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* s, REFIID r, void** l) override { return parent_->CreateCommandList(m, t, a, s, r, l); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE f, void* d, UINT s) override { return parent_->CheckFeatureSupport(f, d, s); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** h) override { return parent_->CreateDescriptorHeap(d, r, h); }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE t) override { return parent_->GetDescriptorHandleIncrementSize(t); }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT m, const void* b, SIZE_T s, REFIID r, void** o) override { return parent_->CreateRootSignature(m, b, s, r, o); }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateConstantBufferView(d, h); }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateShaderResourceView(res, d, h); }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* res, ID3D12Resource* cnt, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateUnorderedAccessView(res, cnt, d, h); }
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateRenderTargetView(res, d, h); }
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateDepthStencilView(res, d, h); }
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateSampler(d, h); }
    void STDMETHODCALLTYPE CopyDescriptors(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t) override { parent_->CopyDescriptors(nd, ds, dsz, ns, ss, ssz, t); }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t) override { parent_->CopyDescriptorsSimple(n, d, s, t); }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetResourceAllocationInfo;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO* r, UINT m, UINT n, const D3D12_RESOURCE_DESC* d) override { return parent_->GetResourceAllocationInfo(r, m, n, d); }
    using ID3D12Device::GetCustomHeapProperties;
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(D3D12_HEAP_PROPERTIES* r, UINT m, D3D12_HEAP_TYPE t) override { return parent_->GetCustomHeapProperties(r, m, t); }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT m, UINT n, const D3D12_RESOURCE_DESC* d) override { return parent_->GetResourceAllocationInfo(m, n, d); }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT m, D3D12_HEAP_TYPE t) override { return parent_->GetCustomHeapProperties(m, t); }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) override { return parent_->CreateCommittedResource(hp, hf, d, st, cv, r, o); }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* d, REFIID r, void** h) override { return parent_->CreateHeap(d, r, h); }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) override { return parent_->CreatePlacedResource(h, off, d, st, cv, r, o); }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) override { return parent_->CreateReservedResource(d, st, cv, r, o); }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* o, const SECURITY_ATTRIBUTES* sa, DWORD a, LPCWSTR n, HANDLE* h) override { return parent_->CreateSharedHandle(o, sa, a, n, h); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE h, REFIID r, void** o) override { return parent_->OpenSharedHandle(h, r, o); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR n, DWORD a, HANDLE* h) override { return parent_->OpenSharedHandleByName(n, a, h); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT n, ID3D12Pageable* const* o) override { return parent_->MakeResident(n, o); }
    HRESULT STDMETHODCALLTYPE Evict(UINT n, ID3D12Pageable* const* o) override { return parent_->Evict(n, o); }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 v, D3D12_FENCE_FLAGS f, REFIID r, void** o) override { return parent_->CreateFence(v, f, r, o); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return parent_->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* d, UINT f, UINT n, UINT64 b, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* l, UINT* nr, UINT64* rs, UINT64* tb) override { parent_->GetCopyableFootprints(d, f, n, b, l, nr, rs, tb); }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** h) override { return parent_->CreateQueryHeap(d, r, h); }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL e) override { return parent_->SetStablePowerState(e); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* rs, REFIID r, void** o) override { return parent_->CreateCommandSignature(d, rs, r, o); }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* res, UINT* nt, D3D12_PACKED_MIP_INFO* pm, D3D12_TILE_SHAPE* ts, UINT* nst, UINT fst, D3D12_SUBRESOURCE_TILING* st) override { parent_->GetResourceTiling(res, nt, pm, ts, nst, fst, st); }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetAdapterLuid;
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* r) override { return parent_->GetAdapterLuid(r); }
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return parent_->GetAdapterLuid(); }
#endif

    // ID3D12Device1..8
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* b, SIZE_T l, REFIID r, void** o) override { return parent_->CreatePipelineLibrary(b, l, r, o); }
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const* f, const UINT64* v, UINT n, D3D12_MULTIPLE_FENCE_WAIT_FLAGS fl, HANDLE e) override { return parent_->SetEventOnMultipleFenceCompletion(f, v, n, fl, e); }
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT n, ID3D12Pageable* const* o, const D3D12_RESIDENCY_PRIORITY* p) override { return parent_->SetResidencyPriority(n, o, p); }
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID r, void** o) override { return parent_->CreatePipelineState(d, r, o); }
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void* a, REFIID r, void** o) override { return parent_->OpenExistingHeapFromAddress(a, r, o); }
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE h, REFIID r, void** o) override { return parent_->OpenExistingHeapFromFileMapping(h, r, o); }
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS f, UINT n, ID3D12Pageable* const* o, ID3D12Fence* fn, UINT64 v) override { return parent_->EnqueueMakeResident(f, n, o, fn, v); }
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT m, D3D12_COMMAND_LIST_TYPE t, D3D12_COMMAND_LIST_FLAGS f, REFIID r, void** o) override { return parent_->CreateCommandList1(m, t, f, r, o); }
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* d, REFIID r, void** o) override { return parent_->CreateProtectedResourceSession(d, r, o); }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID r, void** o) override { return parent_->CreateCommittedResource1(hp, hf, d, st, cv, ps, r, o); }
    HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC* d, ID3D12ProtectedResourceSession* ps, REFIID r, void** o) override { return parent_->CreateHeap1(d, ps, r, o); }
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID r, void** o) override { return parent_->CreateReservedResource1(d, st, cv, ps, r, o); }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device4::GetResourceAllocationInfo1;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo1(D3D12_RESOURCE_ALLOCATION_INFO* r, UINT m, UINT n, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_ALLOCATION_INFO1* i) override { return parent_->GetResourceAllocationInfo1(r, m, n, d, i); }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(UINT m, UINT n, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_ALLOCATION_INFO1* i) override { return parent_->GetResourceAllocationInfo1(m, n, d, i); }
#endif
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner* o, REFIID r, void** t) override { return parent_->CreateLifetimeTracker(o, r, t); }
    void STDMETHODCALLTYPE RemoveDevice() override { parent_->RemoveDevice(); }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT* n, D3D12_META_COMMAND_DESC* d) override { return parent_->EnumerateMetaCommands(n, d); }
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE st, UINT* sz, UINT* n, D3D12_META_COMMAND_PARAMETER_DESC* d) override { return parent_->EnumerateMetaCommandParameters(id, st, sz, n, d); }
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID id, UINT m, const void* d, SIZE_T s, REFIID r, void** o) override { return parent_->CreateMetaCommand(id, m, d, s, r, o); }
    HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC* d, REFIID r, void** o) override { return parent_->CreateStateObject(d, r, o); }
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* d, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* i) override { parent_->GetRaytracingAccelerationStructurePrebuildInfo(d, i); }
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE t, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* i) override { return parent_->CheckDriverMatchingIdentifier(t, i); }
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE m, D3D12_MEASUREMENTS_ACTION a, HANDLE e, BOOL* b) override { return parent_->SetBackgroundProcessingMode(m, a, e, b); }
    HRESULT STDMETHODCALLTYPE AddToStateObject(const D3D12_STATE_OBJECT_DESC* a, ID3D12StateObject* g, REFIID r, void** o) override { return parent_->AddToStateObject(a, g, r, o); }
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* d, REFIID r, void** o) override { return parent_->CreateProtectedResourceSession1(d, r, o); }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device8::GetResourceAllocationInfo2;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo2(D3D12_RESOURCE_ALLOCATION_INFO* r, UINT m, UINT n, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_ALLOCATION_INFO1* i) override { return parent_->GetResourceAllocationInfo2(r, m, n, d, i); }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(UINT m, UINT n, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_ALLOCATION_INFO1* i) override { return parent_->GetResourceAllocationInfo2(m, n, d, i); }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource2(const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID r, void** o) override { return parent_->CreateCommittedResource2(hp, hf, d, st, cv, ps, r, o); }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource1(ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* d, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID r, void** o) override { return parent_->CreatePlacedResource1(h, off, d, st, cv, r, o); }
    void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(ID3D12Resource* tr, ID3D12Resource* fr, D3D12_CPU_DESCRIPTOR_HANDLE h) override { parent_->CreateSamplerFeedbackUnorderedAccessView(tr, fr, h); }
    void STDMETHODCALLTYPE GetCopyableFootprints1(const D3D12_RESOURCE_DESC1* d, UINT f, UINT n, UINT64 b, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* l, UINT* nr, UINT64* rs, UINT64* tb) override { parent_->GetCopyableFootprints1(d, f, n, b, l, nr, rs, tb); }

    // ID3D12Device9
    HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC* pDesc, REFIID riid, void** ppvSession) override {
        const HRESULT hr = parent_->host_dev9_ ? parent_->host_dev9_->CreateShaderCacheSession(pDesc, riid, ppvSession) : E_NOINTERFACE;
        RecordDev("ID3D12DeviceX::CreateShaderCacheSession", 73, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS Control) override {
        const HRESULT hr = parent_->host_dev9_ ? parent_->host_dev9_->ShaderCacheControl(Kinds, Control) : E_NOINTERFACE;
        RecordDev("ID3D12DeviceX::ShaderCacheControl", 74, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID CreatorID, REFIID riid, void** ppCommandQueue) override {
        if (!ppCommandQueue) {
            RecordDev("ID3D12DeviceX::CreateCommandQueue1", 75, E_POINTER);
            return E_POINTER;
        }
        *ppCommandQueue = nullptr;
        if (!parent_->host_dev9_) {
            RecordDev("ID3D12DeviceX::CreateCommandQueue1", 75, E_NOINTERFACE);
            return E_NOINTERFACE;
        }
        ID3D12CommandQueue* host_q = nullptr;
        HRESULT hr = parent_->host_dev9_->CreateCommandQueue1(
            pDesc, CreatorID, OS_D3D12_IID(ID3D12CommandQueue), reinterpret_cast<void**>(&host_q));
        if (FAILED(hr) || !host_q) {
            RecordDev("ID3D12DeviceX::CreateCommandQueue1", 75, hr);
            return hr;
        }
        auto* wrapper = new (std::nothrow) CommandQueueX(parent_, host_q, parent_->DefaultWindow());
        host_q->Release();
        if (!wrapper) {
            RecordDev("ID3D12DeviceX::CreateCommandQueue1", 75, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = wrapper->QueryInterface(riid, ppCommandQueue);
        wrapper->Release();
        RecordDev("ID3D12DeviceX::CreateCommandQueue1", 75, hr);
        return hr;
    }

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
#if defined(__MINGW32__)
    using CastableFormatPtr = DXGI_FORMAT*;
#else
    using CastableFormatPtr = const DXGI_FORMAT*;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource3(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
        UINT32 NumCastableFormats, CastableFormatPtr pCastableFormats,
        REFIID riidResource, void** ppvResource) override {
        if (!parent_->host_dev10_) {
            RecordDev("ID3D12DeviceX::CreateCommittedResource3", 76, E_NOINTERFACE);
            return E_NOINTERFACE;
        }
        D3D12_RESOURCE_DESC1 clean_desc{};
        const D3D12_RESOURCE_DESC1* desc_ptr = pDesc;
        if (pDesc) {
            clean_desc = SanitizeXboxResourceDesc1(*pDesc);
            desc_ptr = &clean_desc;
        }
        ID3D12Resource* host_res = nullptr;
        HRESULT hr = parent_->host_dev10_->CreateCommittedResource3(
            pHeapProperties, SanitizeXboxHeapFlags(HeapFlags), desc_ptr, InitialLayout,
            pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (SUCCEEDED(hr) && host_res && ppvResource) {
            const D3D12_RESOURCE_DESC d0 = desc_ptr ? Desc1ToDesc0(*desc_ptr) : D3D12_RESOURCE_DESC{};
            auto* proxy = new (std::nothrow) ResourceXProxy(
                parent_, host_res, desc_ptr ? &d0 : nullptr, pHeapProperties, SanitizeXboxHeapFlags(HeapFlags));
            host_res->Release();
            if (!proxy) {
                RecordDev("ID3D12DeviceX::CreateCommittedResource3", 76, E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
            hr = proxy->QueryInterface(riidResource, ppvResource);
            proxy->Release();
        } else if (host_res) {
            host_res->Release();
        }
        RecordDev("ID3D12DeviceX::CreateCommittedResource3", 76, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource2(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        UINT32 NumCastableFormats, CastableFormatPtr pCastableFormats,
        REFIID riid, void** ppvResource) override {
        if (!parent_->host_dev10_) {
            RecordDev("ID3D12DeviceX::CreatePlacedResource2", 77, E_NOINTERFACE);
            return E_NOINTERFACE;
        }
        D3D12_RESOURCE_DESC1 clean_desc{};
        const D3D12_RESOURCE_DESC1* desc_ptr = pDesc;
        if (pDesc) {
            clean_desc = SanitizeXboxResourceDesc1(*pDesc);
            desc_ptr = &clean_desc;
        }
        ID3D12Resource* host_res = nullptr;
        HRESULT hr = parent_->host_dev10_->CreatePlacedResource2(
            UnwrapHeap(pHeap), HeapOffset, desc_ptr, InitialLayout, pOptimizedClearValue,
            NumCastableFormats, pCastableFormats,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (SUCCEEDED(hr) && host_res && ppvResource) {
            const D3D12_RESOURCE_DESC d0 = desc_ptr ? Desc1ToDesc0(*desc_ptr) : D3D12_RESOURCE_DESC{};
            auto* proxy = new (std::nothrow) ResourceXProxy(parent_, host_res, desc_ptr ? &d0 : nullptr);
            host_res->Release();
            if (!proxy) {
                RecordDev("ID3D12DeviceX::CreatePlacedResource2", 77, E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
            hr = proxy->QueryInterface(riid, ppvResource);
            proxy->Release();
        } else if (host_res) {
            host_res->Release();
        }
        RecordDev("ID3D12DeviceX::CreatePlacedResource2", 77, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource2(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_BARRIER_LAYOUT InitialLayout,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession,
        UINT32 NumCastableFormats, CastableFormatPtr pCastableFormats,
        REFIID riid, void** ppvResource) override {
        if (!parent_->host_dev10_) {
            RecordDev("ID3D12DeviceX::CreateReservedResource2", 78, E_NOINTERFACE);
            return E_NOINTERFACE;
        }
        D3D12_RESOURCE_DESC clean_desc{};
        const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
        if (pDesc) {
            clean_desc = SanitizeXboxResourceDesc(*pDesc);
            desc_ptr = &clean_desc;
        }
        ID3D12Resource* host_res = nullptr;
        HRESULT hr = parent_->host_dev10_->CreateReservedResource2(
            desc_ptr, InitialLayout, pOptimizedClearValue, pProtectedSession,
            NumCastableFormats, pCastableFormats,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (SUCCEEDED(hr) && host_res && ppvResource) {
            auto* proxy = new (std::nothrow) ResourceXProxy(parent_, host_res, desc_ptr);
            host_res->Release();
            if (!proxy) {
                RecordDev("ID3D12DeviceX::CreateReservedResource2", 78, E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
            hr = proxy->QueryInterface(riid, ppvResource);
            proxy->Release();
        } else if (host_res) {
            host_res->Release();
        }
        RecordDev("ID3D12DeviceX::CreateReservedResource2", 78, hr);
        return hr;
    }
#endif

#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE CreateSampler2(const D3D12_SAMPLER_DESC2* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        if (parent_->host_dev11_) {
            parent_->host_dev11_->CreateSampler2(pDesc, DestDescriptor);
        }
        RecordDev("ID3D12DeviceX::CreateSampler2", 79, parent_->host_dev11_ ? S_OK : E_NOINTERFACE);
    }
#endif

#ifdef __ID3D12Device12_INTERFACE_DEFINED__
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device12::GetResourceAllocationInfo3;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo3(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet, UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC1* pResourceDescs, const UINT32* pNumCastableFormats,
        const DXGI_FORMAT* const* ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override {
        if (parent_->host_dev12_) {
            return parent_->host_dev12_->GetResourceAllocationInfo3(
                pRet, visibleMask, numResourceDescs, pResourceDescs,
                pNumCastableFormats, ppCastableFormats, pResourceAllocationInfo1);
        }
        return parent_->GetResourceAllocationInfo2(pRet, visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
    }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo3(
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC1* pResourceDescs, const UINT32* pNumCastableFormats,
        const DXGI_FORMAT* const* ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override {
        if (parent_->host_dev12_) {
            return parent_->host_dev12_->GetResourceAllocationInfo3(
                visibleMask, numResourceDescs, pResourceDescs,
                pNumCastableFormats, ppCastableFormats, pResourceAllocationInfo1);
        }
        return parent_->GetResourceAllocationInfo2(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
    }
#endif
#endif

private:
    DeviceX* parent_ = nullptr;
};
#endif  // __ID3D12Device9_INTERFACE_DEFINED__

DeviceX::DeviceX(
    ID3D12Device* host_device,
    const D3D12AdapterInfo& adapter_info,
    HWND default_hwnd)
    : host_dev_(host_device), adapter_info_(adapter_info), default_hwnd_(default_hwnd) {
    dxgi_proxy_ = new (std::nothrow) DxgiDeviceProxy(this);
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    ext_proxy_ = new (std::nothrow) DeviceExtendedProxy(this);
#endif
    if (host_dev_) {
        host_dev_->AddRef();
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device1), reinterpret_cast<void**>(&host_dev1_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device2), reinterpret_cast<void**>(&host_dev2_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device3), reinterpret_cast<void**>(&host_dev3_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device4), reinterpret_cast<void**>(&host_dev4_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device5), reinterpret_cast<void**>(&host_dev5_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device6), reinterpret_cast<void**>(&host_dev6_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device7), reinterpret_cast<void**>(&host_dev7_));
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device8), reinterpret_cast<void**>(&host_dev8_));
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device9), reinterpret_cast<void**>(&host_dev9_));
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device10), reinterpret_cast<void**>(&host_dev10_));
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device11), reinterpret_cast<void**>(&host_dev11_));
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
        host_dev_->QueryInterface(OS_D3D12_IID(ID3D12Device12), reinterpret_cast<void**>(&host_dev12_));
#endif

        D3D12_FEATURE_DATA_FORMAT_SUPPORT fmt_sup{};
        fmt_sup.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &fmt_sup, sizeof(fmt_sup)))) {
            supports_r9g9b9e5_uav_ =
                ((fmt_sup.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) &&
                ((fmt_sup.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
        }
        adapter_info_.supports_r9g9b9e5_uav = supports_r9g9b9e5_uav_;

        D3D12_FEATURE_DATA_D3D12_OPTIONS2 opts2{};
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS2, &opts2, sizeof(opts2)))) {
            supports_depth_bounds_test_ = (opts2.DepthBoundsTestSupported != FALSE);
            supports_programmable_sample_positions_ =
                (opts2.ProgrammableSamplePositionsTier !=
                 D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS3 opts3{};
        if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS3, &opts3, sizeof(opts3)))) {
            supports_view_instancing_ =
                (opts3.ViewInstancingTier != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED);
        }

        // Wire Worker 2 bindless descriptor bridge, Sprint 11 feedback/query islands & Worker 6 per-draw capture.
        (void)descriptor_bridge_.InitializeD3D12(host_dev_, 4096u, 256u);
        (void)sampler_feedback_mgr_.Initialize(host_dev_);
        (void)query_accumulator_.Initialize(host_dev_);
        (void)openscarlett::FrameCaptureRecorder::Global().InitializeFromEnv();
    }

    const uint32_t resolved_mode = ResolveEffectiveBackendMode();
    vulkan_routing_active_ = (resolved_mode == D3D12XBOX_BACKEND_VULKAN);
    if (vulkan_routing_active_) {
        vulkan_backend_ = std::make_unique<openscarlett::VulkanBackend>();
        const bool force_sw =
            EnvVarEqualsIgnoreCase("OPENSCARLETT_VK_FORCE_SOFTWARE", "1") ||
            EnvVarEqualsIgnoreCase("OPENSCARLETT_VK_FORCE_SOFTWARE", "true");
        if (force_sw) {
            vulkan_backend_->InitializeSoftwareFallback();
        } else {
            vulkan_backend_->Initialize(/*allow_software_fallback=*/true);
        }
    }
}

void DeviceX::SetGlobalBackendMode(uint32_t backend_mode) noexcept {
    if (backend_mode > D3D12XBOX_BACKEND_VULKAN) {
        backend_mode = D3D12XBOX_BACKEND_AUTO;
    }
    g_backend_mode.store(backend_mode, std::memory_order_release);
}

uint32_t DeviceX::ResolveEffectiveBackendMode() noexcept {
    const uint32_t explicit_mode = g_backend_mode.load(std::memory_order_acquire);
    if (explicit_mode == D3D12XBOX_BACKEND_VULKAN) return D3D12XBOX_BACKEND_VULKAN;
    if (explicit_mode == D3D12XBOX_BACKEND_D3D12) return D3D12XBOX_BACKEND_D3D12;
    if (EnvVarEqualsIgnoreCase("OPENSCARLETT_BACKEND", "vulkan") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_BACKEND", "vk") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_GPU_BACKEND", "vulkan") ||
        EnvVarEqualsIgnoreCase("OPENSCARLETT_GPU_BACKEND", "vk")) {
        return D3D12XBOX_BACKEND_VULKAN;
    }
    return D3D12XBOX_BACKEND_D3D12;
}

uint32_t STDMETHODCALLTYPE DeviceX::GetActiveBackendMode() {
    return vulkan_routing_active_ ? D3D12XBOX_BACKEND_VULKAN : D3D12XBOX_BACKEND_D3D12;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetVulkanRoutingStats(
    D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->vulkan_routing_active = vulkan_routing_active_ ? 1u : 0u;
    if (vulkan_backend_ && vulkan_backend_->IsInitialized()) {
        const auto& info = vulkan_backend_->GetDeviceInfo();
        out_stats->executed_on_vulkan_icd = vulkan_backend_->is_fallback_mode() ? 0u : 1u;
        out_stats->vulkan_software_fallback = vulkan_backend_->is_fallback_mode() ? 1u : 0u;
        out_stats->vulkan_api_major = info.api_major;
        out_stats->vulkan_api_minor = info.api_minor;
        out_stats->vulkan_api_patch = info.api_patch;
        out_stats->subgroup_size = info.subgroup_size;
        out_stats->max_push_constants_size = info.max_push_constants_size;
        const size_t n = std::min(info.device_name.size(), sizeof(out_stats->vulkan_device_name) - 1u);
        std::memcpy(out_stats->vulkan_device_name, info.device_name.data(), n);
        out_stats->vulkan_device_name[n] = '\0';
    }
    out_stats->routed_command_lists = vk_routed_cmd_lists_.load(std::memory_order_relaxed);
    out_stats->routed_draws = vk_routed_draws_.load(std::memory_order_relaxed);
    out_stats->routed_indexed_draws = vk_routed_indexed_draws_.load(std::memory_order_relaxed);
    out_stats->routed_dispatches = vk_routed_dispatches_.load(std::memory_order_relaxed);
    out_stats->routed_mesh_dispatches = vk_routed_mesh_dispatches_.load(std::memory_order_relaxed);
    out_stats->routed_copies = vk_routed_copies_.load(std::memory_order_relaxed);
    out_stats->routed_barriers = vk_routed_barriers_.load(std::memory_order_relaxed);
    out_stats->routed_rtv_clears = vk_routed_rtv_clears_.load(std::memory_order_relaxed);
    out_stats->routed_dsv_clears = vk_routed_dsv_clears_.load(std::memory_order_relaxed);
    out_stats->routed_presents = vk_routed_presents_.load(std::memory_order_relaxed);
    out_stats->vulkan_icd_executions = vk_icd_executions_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_draws = vk_spirv_direct_draws_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_dispatches = vk_spirv_direct_dispatches_.load(std::memory_order_relaxed);
    out_stats->spirv_direct_mesh_dispatches = vk_spirv_direct_mesh_dispatches_.load(std::memory_order_relaxed);
    return S_OK;
}

void DeviceX::NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept {
    if (!host_resource) return;
    sampler_feedback_mgr_.NotifyResourceDestroyed(host_resource);
    {
        std::lock_guard<std::mutex> lock(buffer_va_mutex_);
        buffer_va_map_.erase(host_resource);
    }
    {
        std::lock_guard<std::mutex> lock(tile_mode_mutex_);
        resource_tile_modes_.erase(host_resource);
    }
    {
        uint64_t mirrored_va = 0ull;
        {
            std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
            auto it = mirrored_res_to_va_.find(host_resource);
            if (it != mirrored_res_to_va_.end()) {
                mirrored_va = it->second;
                mirrored_res_to_va_.erase(it);
                mirrored_va_count_.store(
                    static_cast<uint32_t>(mirrored_res_to_va_.size()), std::memory_order_release);
            }
        }
        if (mirrored_va != 0ull) {
            (void)title_va_mirror_.UnregisterRegion(mirrored_va);
        }
    }
    {
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        r9g9b9e5_fallback_resources_.erase(host_resource);
    }
    if (vulkan_routing_active_) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_resources_.erase(host_resource);
        vk_rt_framebuffers_.erase(host_resource);
    }
}

void DeviceX::NotifyHeapDestroyed(ID3D12Heap* host_heap) noexcept {
    if (!host_heap) return;
    std::lock_guard<std::mutex> lock(unified_mutex_);
    address_heaps_.erase(host_heap);
}

void DeviceX::NotifyPipelineDestroyed(ID3D12PipelineState* pso_key) noexcept {
    if (!pso_key || !vulkan_routing_active_) return;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    vk_pipelines_.erase(pso_key);
}

void DeviceX::SetGuestGpuVaTranslatorHook(GuestGpuVaTranslatorFn fn, void* user_ctx) noexcept {
    va_translator_ctx_.store(user_ctx, std::memory_order_release);
    va_translator_hook_.store(fn, std::memory_order_release);
}

void DeviceX::SetDescriptorCreateHook(DescriptorCreateHookFn fn, void* user_ctx) noexcept {
    descriptor_create_ctx_.store(user_ctx, std::memory_order_release);
    descriptor_create_hook_.store(fn, std::memory_order_release);
}

HWND DeviceX::DefaultWindow() const noexcept {
    std::lock_guard<std::mutex> lock(window_mutex_);
    return default_hwnd_;
}

void DeviceX::SetDefaultWindow(HWND hwnd) noexcept {
    std::lock_guard<std::mutex> lock(window_mutex_);
    default_hwnd_ = hwnd;
}

bool DeviceX::AdoptTitleWindow(HWND hwnd) noexcept {
    if (!hwnd || IsWindow(hwnd) == FALSE) {
        return false;
    }
    std::lock_guard<std::mutex> lock(window_mutex_);
    default_hwnd_ = hwnd;
    return true;
}

HWND DeviceX::ResolveEffectiveOutputWindow(HWND explicit_hwnd) const noexcept {
    if (explicit_hwnd && IsWindow(explicit_hwnd) != FALSE) {
        return explicit_hwnd;
    }
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (default_hwnd_ && IsWindow(default_hwnd_) != FALSE) {
        return default_hwnd_;
    }
    return nullptr;
}

void DeviceX::TrackVulkanResource(
    IUnknown* obj, D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& desc) {
    if (!vulkan_routing_active_ || !obj) return;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(obj->QueryInterface(OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&res))) && res) {
        ID3D12Resource* host_res = UnwrapResource(res);
        VulkanTrackedResource tr{};
        tr.resource = host_res;
        tr.heap_type = heap_type;
        tr.desc = desc;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && host_res) {
            tr.gpu_va = host_res->GetGPUVirtualAddress();
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_resources_[host_res] = std::move(tr);
        }
        res->Release();
    }
}

bool DeviceX::LookupVulkanPipeline(ID3D12PipelineState* pso, VulkanPipelineInfo& out_info) const {
    if (!vulkan_routing_active_ || !pso) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_pipelines_.find(pso);
    if (it == vk_pipelines_.end()) return false;
    out_info = it->second;
    return true;
}

bool DeviceX::IsSpirvDirectPipeline(ID3D12PipelineState* pso) const {
    if (!vulkan_routing_active_ || !pso) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_pipelines_.find(pso);
    return (it != vk_pipelines_.end()) && it->second.is_spirv_direct;
}

ID3D12Resource* DeviceX::ResolveVulkanRtvResource(D3D12_CPU_DESCRIPTOR_HANDLE rtv) const {
    if (!vulkan_routing_active_ || rtv.ptr == 0u) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_rtv_map_.find(rtv.ptr);
    return (it != vk_rtv_map_.end()) ? it->second : nullptr;
}

ID3D12Resource* DeviceX::ResolveVulkanDsvResource(D3D12_CPU_DESCRIPTOR_HANDLE dsv) const {
    if (!vulkan_routing_active_ || dsv.ptr == 0u) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_dsv_map_.find(dsv.ptr);
    return (it != vk_dsv_map_.end()) ? it->second : nullptr;
}

ID3D12Resource* DeviceX::ResolveVulkanBufferByGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes) const {
    if (!vulkan_routing_active_ || va == 0ull) return nullptr;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    for (const auto& [res, tr] : vk_resources_) {
        if (tr.gpu_va != 0ull && tr.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (va >= tr.gpu_va && va < tr.gpu_va + tr.desc.Width) {
                if (out_offset_bytes) *out_offset_bytes = va - tr.gpu_va;
                return res;
            }
        }
    }
    return nullptr;
}

bool DeviceX::ReadVulkanBufferData(
    ID3D12Resource* res, uint64_t offset, uint64_t size, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    res = UnwrapResource(res);
    if (!res) return false;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    uint64_t res_width = res->GetDesc().Width;
    {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        auto it = vk_resources_.find(res);
        if (it != vk_resources_.end()) {
            heap_type = it->second.heap_type;
            if (!it->second.shadow_bytes.empty()) {
                const uint64_t avail = (offset < it->second.shadow_bytes.size())
                                           ? (it->second.shadow_bytes.size() - offset)
                                           : 0ull;
                const uint64_t to_copy = (size == 0ull) ? avail : std::min<uint64_t>(size, avail);
                out_bytes.assign(it->second.shadow_bytes.begin() + static_cast<ptrdiff_t>(offset),
                                 it->second.shadow_bytes.begin() + static_cast<ptrdiff_t>(offset + to_copy));
                return !out_bytes.empty();
            }
        }
    }
    if (offset >= res_width) return false;
    const uint64_t avail = res_width - offset;
    const uint64_t to_copy = (size == 0ull) ? avail : std::min<uint64_t>(size, avail);
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK) {
        void* mapped = nullptr;
        D3D12_RANGE range{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + to_copy)};
        if (SUCCEEDED(res->Map(0u, &range, &mapped)) && mapped) {
            out_bytes.resize(static_cast<size_t>(to_copy));
            std::memcpy(out_bytes.data(), static_cast<const uint8_t*>(mapped) + offset, static_cast<size_t>(to_copy));
            D3D12_RANGE write_none{0u, 0u};
            res->Unmap(0u, &write_none);
            return true;
        }
    }
    return false;
}

bool DeviceX::WriteVulkanBufferData(
    ID3D12Resource* res, uint64_t offset, const void* data, size_t size) {
    res = UnwrapResource(res);
    if (!res || !data || size == 0u) return false;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    uint64_t res_width = res->GetDesc().Width;
    {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        auto& tr = vk_resources_[res];
        tr.resource = res;
        heap_type = tr.heap_type;
        if (tr.shadow_bytes.size() < offset + size) {
            tr.shadow_bytes.resize(static_cast<size_t>(offset + size), 0u);
        }
        std::memcpy(tr.shadow_bytes.data() + offset, data, size);
    }
    if (offset + size <= res_width &&
        (heap_type == D3D12_HEAP_TYPE_UPLOAD || heap_type == D3D12_HEAP_TYPE_READBACK)) {
        void* mapped = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        if (SUCCEEDED(res->Map(0u, &read_none, &mapped)) && mapped) {
            std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, size);
            D3D12_RANGE written{static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + size)};
            res->Unmap(0u, &written);
        }
    }
    return true;
}

void DeviceX::StoreVulkanRenderTargetFramebuffer(
    ID3D12Resource* rt_res, const openscarlett::FrameBufferRgba8& fb) {
    rt_res = UnwrapResource(rt_res);
    if (!vulkan_routing_active_ || !rt_res) return;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    vk_rt_framebuffers_[rt_res] = fb;
}

bool DeviceX::TryGetVulkanRenderTargetFramebuffer(
    ID3D12Resource* rt_res, openscarlett::FrameBufferRgba8& out_fb) const {
    rt_res = UnwrapResource(rt_res);
    if (!vulkan_routing_active_ || !rt_res) return false;
    std::lock_guard<std::mutex> lock(vk_state_mutex_);
    auto it = vk_rt_framebuffers_.find(rt_res);
    if (it == vk_rt_framebuffers_.end() || it->second.pixels.empty()) return false;
    out_fb = it->second;
    return true;
}

void DeviceX::RecordVulkanRoutingCounters(
    uint64_t cmd_lists,
    uint64_t draws,
    uint64_t indexed_draws,
    uint64_t dispatches,
    uint64_t mesh_dispatches,
    uint64_t copies,
    uint64_t barriers,
    uint64_t rtv_clears,
    uint64_t dsv_clears,
    uint64_t presents,
    uint64_t icd_execs,
    uint64_t spirv_draws,
    uint64_t spirv_dispatches,
    uint64_t spirv_mesh_dispatches) noexcept {
    if (cmd_lists) vk_routed_cmd_lists_.fetch_add(cmd_lists, std::memory_order_relaxed);
    if (draws) vk_routed_draws_.fetch_add(draws, std::memory_order_relaxed);
    if (indexed_draws) vk_routed_indexed_draws_.fetch_add(indexed_draws, std::memory_order_relaxed);
    if (dispatches) vk_routed_dispatches_.fetch_add(dispatches, std::memory_order_relaxed);
    if (mesh_dispatches) vk_routed_mesh_dispatches_.fetch_add(mesh_dispatches, std::memory_order_relaxed);
    if (copies) vk_routed_copies_.fetch_add(copies, std::memory_order_relaxed);
    if (barriers) vk_routed_barriers_.fetch_add(barriers, std::memory_order_relaxed);
    if (rtv_clears) vk_routed_rtv_clears_.fetch_add(rtv_clears, std::memory_order_relaxed);
    if (dsv_clears) vk_routed_dsv_clears_.fetch_add(dsv_clears, std::memory_order_relaxed);
    if (presents) vk_routed_presents_.fetch_add(presents, std::memory_order_relaxed);
    if (icd_execs) vk_icd_executions_.fetch_add(icd_execs, std::memory_order_relaxed);
    if (spirv_draws) vk_spirv_direct_draws_.fetch_add(spirv_draws, std::memory_order_relaxed);
    if (spirv_dispatches) vk_spirv_direct_dispatches_.fetch_add(spirv_dispatches, std::memory_order_relaxed);
    if (spirv_mesh_dispatches) vk_spirv_direct_mesh_dispatches_.fetch_add(spirv_mesh_dispatches, std::memory_order_relaxed);
}

bool DeviceX::IsR9G9B9E5UavFallbackActive() const noexcept {
    return !supports_r9g9b9e5_uav_ ||
           g_force_r9g9b9e5_uav_fallback.load(std::memory_order_acquire);
}

void DeviceX::SetForceR9G9B9E5UavFallback(bool force) noexcept {
    g_force_r9g9b9e5_uav_fallback.store(force, std::memory_order_release);
}

bool DeviceX::ShouldRewriteR9G9B9E5Uav(const D3D12_RESOURCE_DESC& desc) const noexcept {
    return IsR9G9B9E5UavFallbackActive() &&
           desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
           (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}

void DeviceX::TrackFallbackResource(IUnknown* obj, bool is_fallback) {
    if (!obj) return;
    ID3D12Resource* res = nullptr;
    if (SUCCEEDED(obj->QueryInterface(OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&res))) && res) {
        ID3D12Resource* host_res = UnwrapResource(res);
        {
            std::lock_guard<std::mutex> lock(fallback_mutex_);
            if (is_fallback) {
                r9g9b9e5_fallback_resources_.insert(host_res);
            } else {
                r9g9b9e5_fallback_resources_.erase(host_res);
            }
        }
        res->Release();
    }
}

bool DeviceX::IsTrackedFallbackResource(ID3D12Resource* resource) const noexcept {
    resource = UnwrapResource(resource);
    if (!resource) return false;
    std::lock_guard<std::mutex> lock(fallback_mutex_);
    return r9g9b9e5_fallback_resources_.find(resource) != r9g9b9e5_fallback_resources_.end();
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::TranslateGuestGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS address) const noexcept {
    if (address == 0ull) return 0ull;
    if (const auto hook = va_translator_hook_.load(std::memory_order_acquire)) {
        const D3D12_GPU_VIRTUAL_ADDRESS hooked =
            hook(address, va_translator_ctx_.load(std::memory_order_acquire));
        if (hooked != address && hooked != 0ull) {
            return hooked;
        }
    }
    {
        uint64_t mirrored_va = 0ull;
        uint64_t mirrored_off = 0ull;
        if (title_va_mirror_.ResolveTitleVa(address, mirrored_va, mirrored_off) && mirrored_va != 0ull) {
            return mirrored_va;
        }
    }
    if (xmem_allocation_count_.load(std::memory_order_acquire) == 0u) {
        return address;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (const auto& rec : xmem_allocations_) {
        if (address >= rec.guest_va && address < rec.guest_va + rec.aligned_size) {
            return rec.desktop_gpu_va + (address - rec.guest_va);
        }
    }
    return address;
}

DeviceX::~DeviceX() {
    shader_cache_.WaitForPendingCompiles();
    {
        std::lock_guard<std::mutex> lock(synth_rs_mutex_);
        for (ID3D12RootSignature* rs : synthesized_root_sigs_) {
            if (rs) rs->Release();
        }
        synthesized_root_sigs_.clear();
    }
    ReleaseAaaRuntimeObjects();
    ReleaseUnifiedMemoryObjects();
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    delete ext_proxy_;
    ext_proxy_ = nullptr;
#endif
    delete dxgi_proxy_;
    dxgi_proxy_ = nullptr;
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (host_dev12_) host_dev12_->Release();
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (host_dev11_) host_dev11_->Release();
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (host_dev10_) host_dev10_->Release();
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (host_dev9_) host_dev9_->Release();
#endif
    if (host_dev8_) host_dev8_->Release();
    if (host_dev7_) host_dev7_->Release();
    if (host_dev6_) host_dev6_->Release();
    if (host_dev5_) host_dev5_->Release();
    if (host_dev4_) host_dev4_->Release();
    if (host_dev3_) host_dev3_->Release();
    if (host_dev2_) host_dev2_->Release();
    if (host_dev1_) host_dev1_->Release();
    if (host_dev_) host_dev_->Release();
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE DeviceX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordDev("ID3D12DeviceX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device1)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device2)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device3)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device4)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device5)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device6)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device7)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Device8)) ||
        IsEqualIID(riid, IID_ID3D12DeviceX)) {
        *ppvObject = static_cast<ID3D12DeviceX*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(IDXGIObject)) ||
        IsEqualIID(riid, OS_D3D12_IID(IDXGIDevice)) ||
        IsEqualIID(riid, OS_D3D12_IID(IDXGIDevice1))) {
        if (!dxgi_proxy_) {
            RecordDev("ID3D12DeviceX::QueryInterface", 0, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        *ppvObject = static_cast<IDXGIDevice1*>(dxgi_proxy_);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device9)) && host_dev9_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12Device9*>(ext_proxy_);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device10)) && host_dev10_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12Device10*>(ext_proxy_);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device11)) && host_dev11_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12Device11*>(ext_proxy_);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Device12)) && host_dev12_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12Device12*>(ext_proxy_);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
    if (IsEqualIID(riid, IID_IOpenScarlettUnifiedMemory)) {
        *ppvObject = static_cast<IOpenScarlettUnifiedMemory*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettVulkanRouting)) {
        *ppvObject = static_cast<IOpenScarlettVulkanRouting*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettAaaRuntime)) {
        *ppvObject = static_cast<IOpenScarlettAaaRuntime*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }

    // NEVER fall through to raw host_dev_->QueryInterface() for any ID3D12Device* IID!
    if (IsKnownD3D12DeviceIid(riid)) {
        RecordDev("ID3D12DeviceX::QueryInterface", 0, E_NOINTERFACE);
        return E_NOINTERFACE;
    }

    // Forward safe auxiliary diagnostic interfaces (e.g. ID3D12InfoQueue, ID3D12DebugDevice)
    // to host device.
    const HRESULT hr = host_dev_ ? host_dev_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::QueryInterface", 0, hr);
    return hr;
}

ULONG STDMETHODCALLTYPE DeviceX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordDev("ID3D12DeviceX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE DeviceX::Release() {
    RecordDev("ID3D12DeviceX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE DeviceX::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_dev_->GetPrivateData(guid, pDataSize, pData);
    RecordDev("ID3D12DeviceX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_dev_->SetPrivateData(guid, DataSize, pData);
    RecordDev("ID3D12DeviceX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_dev_->SetPrivateDataInterface(guid, pData);
    RecordDev("ID3D12DeviceX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_dev_->SetName(Name);
    RecordDev("ID3D12DeviceX::SetName", 6, hr);
    return hr;
}

// ID3D12Device (slots 7..43)
UINT STDMETHODCALLTYPE DeviceX::GetNodeCount() {
    const UINT n = host_dev_->GetNodeCount();
    RecordDev("ID3D12DeviceX::GetNodeCount", 7, S_OK);
    return n;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandQueue(
    const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
    if (!ppCommandQueue) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, E_POINTER);
        return E_POINTER;
    }
    *ppCommandQueue = nullptr;
    ID3D12CommandQueue* host_q = nullptr;
    HRESULT hr = host_dev_->CreateCommandQueue(pDesc, IID_PPV_ARGS(&host_q));
    if (FAILED(hr) || !host_q) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandQueueX(this, host_q, DefaultWindow());
    host_q->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = wrapper->QueryInterface(riid, ppCommandQueue);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) {
    if (!ppCommandAllocator) {
        const HRESULT hr = host_dev_->CreateCommandAllocator(type, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
        return hr;
    }
    *ppCommandAllocator = nullptr;
    ID3D12CommandAllocator* host_alloc = nullptr;
    HRESULT hr = host_dev_->CreateCommandAllocator(
        type, OS_D3D12_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(&host_alloc));
    if (FAILED(hr) || !host_alloc) {
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) CommandAllocatorXProxy(this, host_alloc, type);
    host_alloc->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppCommandAllocator);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateCommandAllocator", 9, hr);
    return hr;
}

namespace {

uint64_t HashBytesFnv1a64(const void* data, size_t size, uint64_t seed = 14695981039346656037ull) noexcept {
    if (!data || size == 0u) return seed;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0u; i < size; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string SelectD3D12ProfileForStage(openscarlett::ShaderStage stage, uint32_t highest_sm) {
    const char* prefix = "cs";
    switch (stage) {
        case openscarlett::ShaderStage::Vertex:        prefix = "vs"; break;
        case openscarlett::ShaderStage::Pixel:         prefix = "ps"; break;
        case openscarlett::ShaderStage::Geometry:      prefix = "gs"; break;
        case openscarlett::ShaderStage::Hull:          prefix = "hs"; break;
        case openscarlett::ShaderStage::Domain:        prefix = "ds"; break;
        case openscarlett::ShaderStage::Mesh:          return "ms_6_5";
        case openscarlett::ShaderStage::Amplification: return "as_6_5";
        case openscarlett::ShaderStage::Compute:
        default:                                       prefix = "cs"; break;
    }
    if (highest_sm >= 0x65u) return std::string(prefix) + "_6_5";
    if (highest_sm >= 0x62u) return std::string(prefix) + "_6_2";
    if (highest_sm >= 0x61u) return std::string(prefix) + "_6_1";
    if (highest_sm >= 0x60u) return std::string(prefix) + "_6_0";
    return std::string(prefix) + "_5_0";
}

bool CompileHlslWithSm50Fallback(
    const std::string& hlsl,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode) {
    out_bytecode.clear();
    if (hlsl.empty() || target.empty()) return false;

    const bool is_sm6 = (target.find("_6_") != std::string::npos);
    if (is_sm6) {
        std::string diag;
        if (openscarlett::IsInProcessDxcAvailable() &&
            openscarlett::CompileHlslInProcessDxc(hlsl, target, "main", out_bytecode, diag).ok() &&
            out_bytecode.size() >= 32u) {
            if (!openscarlett::VerifyDxbcContainerHash(out_bytecode.data(), out_bytecode.size())) {
                (void)openscarlett::ResignDxilContainer(out_bytecode);
            }
            return true;
        }
        if (openscarlett::CompileHlslWithDxc(hlsl, target, "main", out_bytecode, diag).ok() &&
            out_bytecode.size() >= 32u) {
            if (!openscarlett::VerifyDxbcContainerHash(out_bytecode.data(), out_bytecode.size())) {
                (void)openscarlett::ResignDxilContainer(out_bytecode);
            }
            return true;
        }
        // Mesh and Amplification shaders strictly require SM 6.5 (cannot fall back to SM 5.0).
        if (target.rfind("ms_", 0) == 0 || target.rfind("as_", 0) == 0) {
            return false;
        }
    }

    std::string sm5_target = target;
    const size_t pos = sm5_target.find("_6_");
    if (pos != std::string::npos) {
        sm5_target.replace(pos, sm5_target.size() - pos, "_5_0");
    }
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(), "openscarlett_xbox_lifted.hlsl",
        nullptr, nullptr, "main", sm5_target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0u, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob || blob->GetBufferSize() < 32u) {
        if (blob) blob->Release();
        return false;
    }
    const auto* ptr = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out_bytecode.assign(ptr, ptr + blob->GetBufferSize());
    blob->Release();
    return true;
}

bool ContainerHasNonDefaultInterp(const openscarlett::XboxParsedShaderContainer& c) noexcept {
    for (const auto& s : c.input_signature) {
        if (s.interp_mode != openscarlett::Rdna2InterpMode::Perspective) return true;
    }
    for (const auto& s : c.output_signature) {
        if (s.interp_mode != openscarlett::Rdna2InterpMode::Perspective) return true;
    }
    return !c.reg_setup.ps_input_cntls.empty();
}

bool RootSigExpectsMultiWindowBvh4(const openscarlett::XboxRootSignatureDesc& rs) noexcept {
    if (!rs.present) return false;
    uint32_t srv_count = 0u;
    for (const auto& p : rs.parameters) {
        if (p.type == openscarlett::XboxRootParamType::Srv) {
            ++srv_count;
        } else if (p.type == openscarlett::XboxRootParamType::DescriptorTable) {
            for (const auto& r : p.descriptor_ranges) {
                if (r.range_type == openscarlett::XboxDescriptorRangeType::Srv) {
                    srv_count += r.num_descriptors;
                }
            }
        }
    }
    return srv_count >= 3u;
}

}  // namespace

void DeviceX::SyncXboxContainerSgprMapToBridge(const openscarlett::XboxParsedShaderContainer& parsed) {
    if (parsed.sgpr_map.bindings.empty()) {
        return;
    }
    if (descriptor_bridge_.IsD3D12Initialized()) {
        std::vector<uint32_t> zero_consts;
        std::vector<uint64_t> zero_vas;
        (void)openscarlett::ApplyRootSgprMapToDescriptorBridge(
            parsed.sgpr_map, zero_consts, zero_vas, descriptor_bridge_);
    }
    RecordDev("ID3D12DeviceX::XboxContainerSgprMap", -1, S_OK);
}

ID3D12RootSignature* DeviceX::SynthesizeHostRootSignatureFromXboxDesc(
    const openscarlett::XboxRootSignatureDesc& desc) {
    if (!host_dev_) return nullptr;

    std::vector<D3D12_ROOT_PARAMETER> params;
    std::deque<std::vector<D3D12_DESCRIPTOR_RANGE>> range_storage;
    params.reserve(desc.parameters.size());

    for (const auto& xp : desc.parameters) {
        D3D12_ROOT_PARAMETER p{};
        p.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(
            xp.shader_visibility <= 7u ? xp.shader_visibility : 0u);
        switch (xp.type) {
            case openscarlett::XboxRootParamType::Constants32Bit:
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                p.Constants.ShaderRegister = xp.shader_register;
                p.Constants.RegisterSpace = xp.register_space;
                p.Constants.Num32BitValues = xp.num_32bit_values ? xp.num_32bit_values : 1u;
                break;
            case openscarlett::XboxRootParamType::Cbv:
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p.Descriptor.ShaderRegister = xp.shader_register;
                p.Descriptor.RegisterSpace = xp.register_space;
                break;
            case openscarlett::XboxRootParamType::Srv:
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                p.Descriptor.ShaderRegister = xp.shader_register;
                p.Descriptor.RegisterSpace = xp.register_space;
                break;
            case openscarlett::XboxRootParamType::Uav:
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                p.Descriptor.ShaderRegister = xp.shader_register;
                p.Descriptor.RegisterSpace = xp.register_space;
                break;
            case openscarlett::XboxRootParamType::DescriptorTable:
            default: {
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
                ranges.reserve(xp.descriptor_ranges.size());
                for (const auto& xr : xp.descriptor_ranges) {
                    D3D12_DESCRIPTOR_RANGE r{};
                    switch (xr.range_type) {
                        case openscarlett::XboxDescriptorRangeType::Uav:
                            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                            break;
                        case openscarlett::XboxDescriptorRangeType::Cbv:
                            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                            break;
                        case openscarlett::XboxDescriptorRangeType::Sampler:
                            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                            break;
                        case openscarlett::XboxDescriptorRangeType::Srv:
                        default:
                            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                            break;
                    }
                    r.NumDescriptors = xr.num_descriptors ? xr.num_descriptors : 1u;
                    r.BaseShaderRegister = xr.base_shader_register;
                    r.RegisterSpace = xr.register_space;
                    r.OffsetInDescriptorsFromTableStart = xr.offset_in_descriptors_from_table_start;
                    ranges.push_back(r);
                }
                if (ranges.empty()) continue;
                range_storage.push_back(std::move(ranges));
                p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(range_storage.back().size());
                p.DescriptorTable.pDescriptorRanges = range_storage.back().data();
                break;
            }
        }
        params.push_back(p);
    }

    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
    static_samplers.reserve(desc.static_samplers.size());
    for (const auto& xs : desc.static_samplers) {
        D3D12_STATIC_SAMPLER_DESC s{};
        s.Filter = static_cast<D3D12_FILTER>(xs.filter);
        s.AddressU = xs.address_u ? static_cast<D3D12_TEXTURE_ADDRESS_MODE>(xs.address_u)
                                  : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.AddressV = xs.address_v ? static_cast<D3D12_TEXTURE_ADDRESS_MODE>(xs.address_v)
                                  : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.AddressW = xs.address_w ? static_cast<D3D12_TEXTURE_ADDRESS_MODE>(xs.address_w)
                                  : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MipLODBias = xs.mip_lod_bias;
        s.MaxAnisotropy = xs.max_anisotropy ? xs.max_anisotropy : 1u;
        s.ComparisonFunc = xs.comparison_func ? static_cast<D3D12_COMPARISON_FUNC>(xs.comparison_func)
                                              : D3D12_COMPARISON_FUNC_NEVER;
        s.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(xs.border_color);
        s.MinLOD = xs.min_lod;
        s.MaxLOD = (xs.max_lod > 0.0f) ? xs.max_lod : D3D12_FLOAT32_MAX;
        s.ShaderRegister = xs.shader_register;
        s.RegisterSpace = xs.register_space;
        s.ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(
            xs.shader_visibility <= 7u ? xs.shader_visibility : 0u);
        static_samplers.push_back(s);
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = static_cast<UINT>(params.size());
    rsd.pParameters = params.empty() ? nullptr : params.data();
    rsd.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
    rsd.pStaticSamplers = static_samplers.empty() ? nullptr : static_samplers.data();
    rsd.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(desc.flags);

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (err) err->Release();
    if (FAILED(hr) || !blob) {
        if (blob) blob->Release();
        return nullptr;
    }

    ID3D12RootSignature* host_rs = nullptr;
    hr = host_dev_->CreateRootSignature(
        0u, blob->GetBufferPointer(), blob->GetBufferSize(),
        OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
    blob->Release();
    if (FAILED(hr) || !host_rs) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(synth_rs_mutex_);
        synthesized_root_sigs_.push_back(host_rs);
    }
    return host_rs;
}

bool DeviceX::LiftXboxStageToHostBytecode(
    const void* data,
    size_t size,
    bool mesh_has_payload,
    bool prefer_dxil_standin,
    const openscarlett::XboxParsedShaderContainer* paired_other_stage,
    std::vector<uint8_t>& out_bytecode,
    openscarlett::XboxParsedShaderContainer* out_parsed) {
    out_bytecode.clear();
    if (!data || size < 4u) return false;

    openscarlett::XboxParsedShaderContainer parsed{};
    if (!openscarlett::ParseFullXboxShaderContainer(data, size, parsed).ok() || !parsed.has_xs2) {
        return false;
    }
    SyncXboxContainerSgprMapToBridge(parsed);
    if (parsed.root_signature_inferred) {
        mesh_inferred_root_lifts_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::MeshInferredRootBindings", 47, S_OK);
    }

    const uint32_t highest_sm = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;

    // 1. LIVE RayQuery bridge routing whenever image_bvh_intersect_ray / image_bvh64_intersect_ray is present.
    if (parsed.bvh_info.detected) {
        openscarlett::DxrCapabilities caps{};
        caps.hardware_tier = adapter_info_.raytracing_tier;
        caps.supports_device5 = (host_dev5_ != nullptr);
        caps.supports_state_objects = (adapter_info_.raytracing_tier >= 10u);
        caps.supports_inline_rt = (adapter_info_.raytracing_tier >= 11u);
        caps.using_hardware_path = (adapter_info_.raytracing_tier >= 11u);
        caps.adapter_is_warp = adapter_info_.is_warp;
        const bool force_bvh4_fallback = RootSigExpectsMultiWindowBvh4(parsed.root_signature);
        const openscarlett::Rdna2RayQueryBackend backend =
            openscarlett::SelectRayQueryBackend(caps, force_bvh4_fallback);
        const bool use_dxr11 = (backend == openscarlett::Rdna2RayQueryBackend::HardwareDxr11);

        const uint64_t cache_key =
            HashBytesFnv1a64(data, size) ^
            (static_cast<uint64_t>(highest_sm) << 33) ^
            (use_dxr11 ? 0xA5A50001ull : 0x5A5A0002ull);
        {
            std::lock_guard<std::mutex> lock(shader_lift_mutex_);
            auto it = xbox_container_dxil_cache_.find(cache_key);
            if (it != xbox_container_dxil_cache_.end()) {
                out_bytecode = it->second;
                if (out_parsed) *out_parsed = std::move(parsed);
                RecordDev("ID3D12DeviceX::RayQueryBridgeLift", 11, S_OK);
                return true;
            }
        }

        openscarlett::Rdna2BvhIntersectDesc rq_desc{};
        rq_desc.op = parsed.bvh_info.is_64bit
                         ? openscarlett::Rdna2BvhIntersectOp::ImageBvh64IntersectRay
                         : openscarlett::Rdna2BvhIntersectOp::ImageBvhIntersectRay32;
        rq_desc.a16 = parsed.bvh_info.a16;
        rq_desc.ray_flags = parsed.bvh_info.ray_flags;
        rq_desc.instance_mask = parsed.bvh_info.instance_mask;
        rq_desc.threadgroup_x = parsed.bvh_info.threadgroup_x ? parsed.bvh_info.threadgroup_x : 64u;
        rq_desc.emit_dxr11_hlsl = use_dxr11;
        rq_desc.emit_compute_bvh4_fallback_hlsl = !use_dxr11;
        rq_desc.emit_spirv_ray_query = false;

        openscarlett::Rdna2RayQueryTranslationResult rq_res{};
        if (!openscarlett::TranslateRdna2BvhIntersectToRayQuery(rq_desc, rq_res).ok()) {
            return false;
        }
        const std::string& src_hlsl = use_dxr11 ? rq_res.dxr11_hlsl : rq_res.compute_bvh4_hlsl;
        // Hardware DXR 1.1 requires cs_6_5; Compute BVH4 fallback uses cs_6_5 DXIL on SM 6.5+
        // hardware GPUs and cs_5_0 DXBC on WARP / pre-SM 6.5 adapters (matching
        // Rdna2RayQueryExecutor and avoiding Windows 10 WARP DXIL nested-stack JIT hangs).
        const std::string target = use_dxr11
                                       ? std::string("cs_6_5")
                                       : ((highest_sm >= 0x65u && !adapter_info_.is_warp)
                                              ? std::string("cs_6_5")
                                              : std::string("cs_5_0"));
        if (!CompileHlslWithSm50Fallback(src_hlsl, target, out_bytecode)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(shader_lift_mutex_);
            xbox_container_dxil_cache_[cache_key] = out_bytecode;
        }
        rayquery_bridge_lifts_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::RayQueryBridgeLift", 11, S_OK);
        if (out_parsed) *out_parsed = std::move(parsed);
        return true;
    }

    // 2. Container-aware lifting path when VS v3 (SV_InstanceID) auto-load OR paired VS<->PS
    //    interpolation linkage reconciliation is needed.
    const bool needs_vs_iid_prologue =
        (parsed.xs2.stage == openscarlett::ShaderStage::Vertex) &&
        (parsed.vs_prologue.uses_instance_id_v3 || parsed.vs_prologue.declared_instance_id_in_sig);
    const bool needs_paired_interp =
        (paired_other_stage != nullptr) &&
        (ContainerHasNonDefaultInterp(parsed) || ContainerHasNonDefaultInterp(*paired_other_stage));

    if (needs_vs_iid_prologue || needs_paired_interp) {
        const uint64_t cache_key =
            HashBytesFnv1a64(data, size) ^
            (static_cast<uint64_t>(highest_sm) << 33) ^
            (mesh_has_payload ? 0x10101010ull : 0x20202020ull) ^
            (needs_paired_interp ? 0x33333333ull : 0ull);
        {
            std::lock_guard<std::mutex> lock(shader_lift_mutex_);
            auto it = xbox_container_dxil_cache_.find(cache_key);
            if (it != xbox_container_dxil_cache_.end()) {
                out_bytecode = it->second;
                if (out_parsed) *out_parsed = std::move(parsed);
                RecordDev("ID3D12DeviceX::XboxContainerLift", 10, S_OK);
                return true;
            }
        }

        openscarlett::XboxContainerLiftOptions xopts{};
        xopts.enable_vs_system_id_prologue = true;
        xopts.infer_missing_root_signature = true;
        xopts.use_in_process_dxc = true;
        xopts.paired_other_stage = paired_other_stage;
        xopts.lift_options.emit_spirv = false;
        xopts.lift_options.emit_hlsl = true;
        xopts.lift_options.compile_dxil_with_dxc = false;  // Compile with unified SM6/SM5 fallback below
        xopts.lift_options.mesh_has_payload = mesh_has_payload;
        xopts.lift_options.use_sm65 =
            (highest_sm >= 0x60u) ||
            (parsed.xs2.stage == openscarlett::ShaderStage::Mesh) ||
            (parsed.xs2.stage == openscarlett::ShaderStage::Amplification);
        const std::string target = SelectD3D12ProfileForStage(parsed.xs2.stage, highest_sm);
        xopts.lift_options.target_profile_override = target;

        openscarlett::XboxContainerLiftResult lift_res{};
        {
            std::lock_guard<std::mutex> lock(shader_lift_mutex_);
            if (!openscarlett::LiftXboxShaderContainer(data, size, lift_res, xopts).ok()) {
                return false;
            }
        }
        if (!CompileHlslWithSm50Fallback(lift_res.lift.hlsl_source, target, out_bytecode)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(shader_lift_mutex_);
            xbox_container_dxil_cache_[cache_key] = out_bytecode;
        }
        xbox_container_lifts_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::XboxContainerLift", 10, S_OK);
        if (out_parsed) *out_parsed = std::move(parsed);
        return true;
    }

    // 3. Standard ShaderCache path (preserves MemoryHit/DiskHit/AsyncCompile telemetry & disk cache).
    openscarlett::ShaderCacheLiftRequest req{};
    req.highest_shader_model = highest_sm;
    req.mesh_has_payload = mesh_has_payload;
    req.prefer_dxil_standin = prefer_dxil_standin;

    openscarlett::CachedShaderResult res{};
    std::string diag;
    openscarlett::Status st;
    {
        std::lock_guard<std::mutex> lift_lock(shader_lift_mutex_);
        st = shader_cache_.GetOrLiftXboxShader(data, size, req, res, &diag);
    }
    if (!st.ok()) {
        return false;
    }
    out_bytecode = std::move(res.dxil);
    xbox_container_lifts_.fetch_add(1ull, std::memory_order_relaxed);
    if (out_parsed) *out_parsed = std::move(parsed);
    return true;
}

bool DeviceX::LiftXboxStageToSpirv(
    const void* data,
    size_t size,
    bool mesh_has_payload,
    const openscarlett::XboxParsedShaderContainer* paired_other_stage,
    std::vector<uint32_t>& out_spirv,
    openscarlett::XboxParsedShaderContainer* out_parsed) {
    out_spirv.clear();
    if (!data || size < 4u) return false;

    openscarlett::XboxParsedShaderContainer parsed{};
    if (!openscarlett::ParseFullXboxShaderContainer(data, size, parsed).ok() || !parsed.has_xs2) {
        return false;
    }
    SyncXboxContainerSgprMapToBridge(parsed);

    if (parsed.bvh_info.detected) {
        openscarlett::Rdna2BvhIntersectDesc rq_desc{};
        rq_desc.op = parsed.bvh_info.is_64bit
                         ? openscarlett::Rdna2BvhIntersectOp::ImageBvh64IntersectRay
                         : openscarlett::Rdna2BvhIntersectOp::ImageBvhIntersectRay32;
        rq_desc.threadgroup_x = parsed.bvh_info.threadgroup_x ? parsed.bvh_info.threadgroup_x : 64u;
        rq_desc.ray_flags = parsed.bvh_info.ray_flags;
        rq_desc.instance_mask = parsed.bvh_info.instance_mask;
        rq_desc.emit_dxr11_hlsl = false;
        rq_desc.emit_compute_bvh4_fallback_hlsl = false;
        rq_desc.emit_spirv_ray_query = true;

        openscarlett::Rdna2RayQueryTranslationResult rq_res{};
        if (!openscarlett::TranslateRdna2BvhIntersectToRayQuery(rq_desc, rq_res).ok() ||
            rq_res.spirv_ray_query_words.empty()) {
            return false;
        }
        out_spirv = std::move(rq_res.spirv_ray_query_words);
        rayquery_bridge_lifts_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::RayQueryBridgeLift", 11, S_OK);
        if (out_parsed) *out_parsed = std::move(parsed);
        return true;
    }

    openscarlett::XboxContainerLiftOptions xopts{};
    xopts.enable_vs_system_id_prologue = true;
    xopts.infer_missing_root_signature = true;
    xopts.paired_other_stage = paired_other_stage;
    xopts.lift_options.emit_spirv = true;
    xopts.lift_options.emit_mesh_spirv = true;
    xopts.lift_options.emit_hlsl = false;
    xopts.lift_options.compile_dxil_with_dxc = false;
    xopts.lift_options.mesh_has_payload = mesh_has_payload;

    openscarlett::XboxContainerLiftResult lift_res{};
    {
        std::lock_guard<std::mutex> lock(shader_lift_mutex_);
        if (!openscarlett::LiftXboxShaderContainer(data, size, lift_res, xopts).ok() ||
            lift_res.lift.spirv_words.empty()) {
            return false;
        }
    }
    out_spirv = std::move(lift_res.lift.spirv_words);
    xbox_container_lifts_.fetch_add(1ull, std::memory_order_relaxed);
    if (out_parsed) *out_parsed = std::move(parsed);
    return true;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (!pDesc) {
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }

    const bool vs_is_spirv = IsRawSpirvBytecode(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
    const bool ps_is_spirv = IsRawSpirvBytecode(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
    const bool lift_vs = openscarlett::IsXboxShaderContainerBlob(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
    const bool lift_ps = openscarlett::IsXboxShaderContainerBlob(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
    const bool lift_ds = openscarlett::IsXboxShaderContainerBlob(pDesc->DS.pShaderBytecode, pDesc->DS.BytecodeLength);
    const bool lift_hs = openscarlett::IsXboxShaderContainerBlob(pDesc->HS.pShaderBytecode, pDesc->HS.BytecodeLength);
    const bool lift_gs = openscarlett::IsXboxShaderContainerBlob(pDesc->GS.pShaderBytecode, pDesc->GS.BytecodeLength);

    // Direct SPIR-V graphics PSO path when Vulkan routing is active.
    if (vulkan_routing_active_ && vs_is_spirv && ps_is_spirv) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        VulkanPipelineInfo pi{};
        pi.is_spirv_direct = true;
        pi.is_compute = false;
        pi.is_mesh = false;
        pi.vs_spirv = CopySpirvWords(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength);
        pi.ps_spirv = CopySpirvWords(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);

        openscarlett::SpirvValidationInfo vs_val{}, ps_val{};
        if (!openscarlett::VulkanBackend::ValidateSpirvBinary(pi.vs_spirv, &vs_val).ok() ||
            !openscarlett::VulkanBackend::ValidateSpirvBinary(pi.ps_spirv, &ps_val).ok()) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_INVALIDARG);
            return E_INVALIDARG;
        }
        if (!vs_val.entry_point_name.empty()) pi.vs_entry = vs_val.entry_point_name;
        if (!ps_val.entry_point_name.empty()) pi.ps_entry = ps_val.entry_point_name;
        pi.enable_depth = (pDesc->DepthStencilState.DepthEnable != FALSE);
        pi.depth_write = (pDesc->DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO);
        if (pDesc->NumRenderTargets > 0u) {
            pi.color_format = DxgiToVulkanColorFormat(pDesc->RTVFormats[0]);
        }
        uint32_t running_off = 0u;
        for (UINT i = 0; i < pDesc->InputLayout.NumElements; ++i) {
            const auto& el = pDesc->InputLayout.pInputElementDescs[i];
            const uint32_t off = (el.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT)
                                     ? running_off
                                     : el.AlignedByteOffset;
            openscarlett::VulkanVertexAttribute attr{};
            attr.location = i;
            attr.binding = el.InputSlot;
            attr.format = DxgiToVulkanVertexFormat(el.Format);
            attr.offset = off;
            pi.vertex_attrs.push_back(attr);
            running_off = off + DxgiVertexFormatBytes(el.Format);
        }
        pi.vertex_stride_hint = running_off;

        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    lifted_desc.pRootSignature = UnwrapRootSignature(pDesc->pRootSignature);
    std::deque<std::vector<uint8_t>> lifted_blobs;

    if (lift_vs || lift_ps || lift_ds || lift_hs || lift_gs) {
        const uint32_t highest_sm = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
        const bool prefer_dxil_standin = (highest_sm >= 0x60u);

        openscarlett::XboxParsedShaderContainer vs_parsed{}, ps_parsed{};
        const bool has_vs_parsed = lift_vs &&
            openscarlett::ParseFullXboxShaderContainer(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength, vs_parsed).ok();
        const bool has_ps_parsed = lift_ps &&
            openscarlett::ParseFullXboxShaderContainer(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength, ps_parsed).ok();

        openscarlett::XboxRootSignatureDesc fallback_rs_desc{};
        bool has_fallback_rs = false;

        auto lift_stage = [&](D3D12_SHADER_BYTECODE& bc,
                              bool do_lift,
                              const openscarlett::XboxParsedShaderContainer* paired) -> bool {
            if (!do_lift) return true;
            std::vector<uint8_t> host_bc;
            openscarlett::XboxParsedShaderContainer stage_parsed{};
            if (!LiftXboxStageToHostBytecode(bc.pShaderBytecode, bc.BytecodeLength,
                                             /*mesh_has_payload=*/false, prefer_dxil_standin,
                                             paired, host_bc, &stage_parsed)) {
                return false;
            }
            if (!has_fallback_rs) {
                if (stage_parsed.root_signature.present) {
                    fallback_rs_desc = stage_parsed.root_signature;
                    has_fallback_rs = true;
                } else if (stage_parsed.root_signature_inferred) {
                    fallback_rs_desc = stage_parsed.inferred_root_signature;
                    has_fallback_rs = true;
                }
            }
            lifted_blobs.push_back(std::move(host_bc));
            bc.pShaderBytecode = lifted_blobs.back().data();
            bc.BytecodeLength = lifted_blobs.back().size();
            return true;
        };

        if (!lift_stage(lifted_desc.VS, lift_vs, has_ps_parsed ? &ps_parsed : nullptr) ||
            !lift_stage(lifted_desc.PS, lift_ps, has_vs_parsed ? &vs_parsed : nullptr) ||
            !lift_stage(lifted_desc.DS, lift_ds, nullptr) ||
            !lift_stage(lifted_desc.HS, lift_hs, nullptr) ||
            !lift_stage(lifted_desc.GS, lift_gs, nullptr)) {
            RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_INVALIDARG);
            return E_INVALIDARG;
        }
        if (!lifted_desc.pRootSignature && has_fallback_rs) {
            lifted_desc.pRootSignature = SynthesizeHostRootSignatureFromXboxDesc(fallback_rs_desc);
        }
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(&lifted_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev_->CreateGraphicsPipelineState(
        &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    if (vulkan_routing_active_ && (lift_vs || lift_ps)) {
        VulkanPipelineInfo vk_pi{};
        vk_pi.is_spirv_direct = false;
        vk_pi.is_compute = false;
        vk_pi.is_mesh = false;
        bool has_any = false;
        if (lift_vs && LiftXboxStageToSpirv(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength,
                                            false, nullptr, vk_pi.vs_spirv)) {
            has_any = true;
        }
        if (lift_ps && LiftXboxStageToSpirv(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength,
                                            false, nullptr, vk_pi.ps_spirv)) {
            has_any = true;
        }
        if (has_any) {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(proxy)] = std::move(vk_pi);
        }
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    const bool cs_is_spirv = pDesc && IsRawSpirvBytecode(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength);
    const bool cs_is_xbox = pDesc && openscarlett::IsXboxShaderContainerBlob(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength);

    if (vulkan_routing_active_ && pDesc && cs_is_spirv) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        VulkanPipelineInfo pi{};
        pi.is_spirv_direct = true;
        pi.is_compute = true;
        pi.cs_spirv = CopySpirvWords(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength);
        openscarlett::SpirvValidationInfo cs_val{};
        if (!openscarlett::VulkanBackend::ValidateSpirvBinary(pi.cs_spirv, &cs_val).ok() ||
            !cs_val.has_compute_entry) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_INVALIDARG);
            return E_INVALIDARG;
        }
        if (!cs_val.entry_point_name.empty()) pi.cs_entry = cs_val.entry_point_name;

        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }

    if (!pDesc) {
        const HRESULT hr = host_dev_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC lifted_desc = *pDesc;
    lifted_desc.pRootSignature = UnwrapRootSignature(pDesc->pRootSignature);
    std::vector<uint8_t> lifted_cs_bc;

    if (cs_is_xbox) {
        openscarlett::XboxParsedShaderContainer cs_parsed{};
        if (!LiftXboxStageToHostBytecode(
                pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength,
                /*mesh_has_payload=*/false,
                /*prefer_dxil_standin=*/false,
                nullptr, lifted_cs_bc, &cs_parsed)) {
            RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_INVALIDARG);
            return E_INVALIDARG;
        }
        lifted_desc.CS.pShaderBytecode = lifted_cs_bc.data();
        lifted_desc.CS.BytecodeLength = lifted_cs_bc.size();
        if (!lifted_desc.pRootSignature) {
            if (cs_parsed.root_signature.present) {
                lifted_desc.pRootSignature = SynthesizeHostRootSignatureFromXboxDesc(cs_parsed.root_signature);
            } else if (cs_parsed.root_signature_inferred) {
                lifted_desc.pRootSignature = SynthesizeHostRootSignatureFromXboxDesc(cs_parsed.inferred_root_signature);
            }
        }
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev_->CreateComputePipelineState(&lifted_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev_->CreateComputePipelineState(
        &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    if (vulkan_routing_active_ && cs_is_xbox) {
        VulkanPipelineInfo vk_pi{};
        vk_pi.is_spirv_direct = false;
        vk_pi.is_compute = true;
        if (LiftXboxStageToSpirv(pDesc->CS.pShaderBytecode, pDesc->CS.BytecodeLength,
                                 false, nullptr, vk_pi.cs_spirv)) {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(proxy)] = std::move(vk_pi);
        }
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateComputePipelineState", 11, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
    ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    if (!ppCommandList) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, E_POINTER);
        return E_POINTER;
    }
    *ppCommandList = nullptr;
    const bool initial_is_spirv =
        vulkan_routing_active_ && IsSpirvDirectPipeline(pInitialState);
    ID3D12PipelineState* host_initial =
        initial_is_spirv ? nullptr : UnwrapPipelineState(pInitialState);
    ID3D12GraphicsCommandList* host_cl = nullptr;
    HRESULT hr = host_dev_->CreateCommandList(
        nodeMask, type, UnwrapCommandAllocator(pCommandAllocator), host_initial,
        OS_D3D12_IID(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&host_cl));
    if (FAILED(hr) || !host_cl) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandListX(this, host_cl);
    host_cl->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandList", 12, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    if (initial_is_spirv) {
        wrapper->SetPipelineState(pInitialState);
    }
    hr = wrapper->QueryInterface(riid, ppCommandList);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandList", 12, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CheckFeatureSupport(
    D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) {
    const HRESULT hr = host_dev_->CheckFeatureSupport(
        Feature, pFeatureSupportData, FeatureSupportDataSize);
    RecordDev("ID3D12DeviceX::CheckFeatureSupport", 13, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateDescriptorHeap(
    const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) {
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateDescriptorHeap(pDescriptorHeapDesc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12DescriptorHeap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateDescriptorHeap(
        pDescriptorHeapDesc, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) DescriptorHeapXProxy(this, host_heap);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
    return hr;
}

UINT STDMETHODCALLTYPE DeviceX::GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) {
    const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    RecordDev("ID3D12DeviceX::GetDescriptorHandleIncrementSize", 15, S_OK);
    return inc;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateRootSignature(
    UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
    REFIID riid, void** ppvRootSignature) {
    if (!ppvRootSignature) {
        const HRESULT hr = host_dev_->CreateRootSignature(
            nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    *ppvRootSignature = nullptr;
    ID3D12RootSignature* host_rs = nullptr;
    HRESULT hr = host_dev_->CreateRootSignature(
        nodeMask, pBlobWithRootSignature, blobLengthInBytes,
        OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
    if (FAILED(hr) || !host_rs) {
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) RootSignatureXProxy(this, host_rs);
    host_rs->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvRootSignature);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::CreateConstantBufferView(
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC translated{};
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc_ptr = pDesc;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = pDesc ? pDesc->BufferLocation : 0ull;
    if (pDesc && pDesc->BufferLocation != 0ull) {
        const D3D12_GPU_VIRTUAL_ADDRESS mapped_va = TranslateGuestGpuVa(pDesc->BufferLocation);
        if (mapped_va != pDesc->BufferLocation) {
            translated = *pDesc;
            translated.BufferLocation = mapped_va;
            desc_ptr = &translated;
            gpu_va = mapped_va;
        }
    }
    host_dev_->CreateConstantBufferView(desc_ptr, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, nullptr, gpu_va,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateConstantBufferView", 17, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateShaderResourceView(
    ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    const auto notify_hook = [&]() {
        if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
            const D3D12_GPU_VIRTUAL_ADDRESS va =
                (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                    ? host_res->GetGPUVirtualAddress()
                    : 0ull;
            hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, host_res, va,
                 descriptor_create_ctx_.load(std::memory_order_acquire));
        }
    };
    // R9G9B9E5_SHAREDEXP UAV fallback: rewrite explicit R9G9B9E5 SRV views to R16G16B16A16_FLOAT
    // when the backing resource was promoted to RGBA16F.
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        host_res != nullptr &&
        (IsTrackedFallbackResource(host_res) ||
         host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        host_dev_->CreateShaderResourceView(host_res, &rw_desc, DestDescriptor);
        notify_hook();
        RecordDev("ID3D12DeviceX::CreateShaderResourceView[R9G9B9E5Fallback]", 18, S_OK);
        RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
        return;
    }
    // ScarlettX #46: Preserve array slices on default (nullptr desc) texture SRVs.
    if (pDesc == nullptr && host_res != nullptr) {
        const D3D12_RESOURCE_DESC res_desc = host_res->GetDesc();
        if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            res_desc.DepthOrArraySize > 1u) {
            D3D12_SHADER_RESOURCE_VIEW_DESC explicit_desc{};
            explicit_desc.Format = MapTypelessFormatForSrv(res_desc.Format);
            explicit_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (res_desc.SampleDesc.Count > 1u) {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                explicit_desc.Texture2DMSArray.FirstArraySlice = 0u;
                explicit_desc.Texture2DMSArray.ArraySize = res_desc.DepthOrArraySize;
            } else {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                explicit_desc.Texture2DArray.MostDetailedMip = 0u;
                explicit_desc.Texture2DArray.MipLevels =
                    (res_desc.MipLevels > 0u) ? res_desc.MipLevels : static_cast<UINT>(-1);
                explicit_desc.Texture2DArray.FirstArraySlice = 0u;
                explicit_desc.Texture2DArray.ArraySize = res_desc.DepthOrArraySize;
                explicit_desc.Texture2DArray.PlaneSlice = 0u;
                explicit_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            }
            host_dev_->CreateShaderResourceView(host_res, &explicit_desc, DestDescriptor);
            notify_hook();
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
        if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
            res_desc.DepthOrArraySize > 1u) {
            D3D12_SHADER_RESOURCE_VIEW_DESC explicit_desc{};
            explicit_desc.Format = MapTypelessFormatForSrv(res_desc.Format);
            explicit_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            explicit_desc.Texture1DArray.MostDetailedMip = 0u;
            explicit_desc.Texture1DArray.MipLevels =
                (res_desc.MipLevels > 0u) ? res_desc.MipLevels : static_cast<UINT>(-1);
            explicit_desc.Texture1DArray.FirstArraySlice = 0u;
            explicit_desc.Texture1DArray.ArraySize = res_desc.DepthOrArraySize;
            explicit_desc.Texture1DArray.ResourceMinLODClamp = 0.0f;
            host_dev_->CreateShaderResourceView(host_res, &explicit_desc, DestDescriptor);
            notify_hook();
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
    }
    host_dev_->CreateShaderResourceView(host_res, pDesc, DestDescriptor);
    notify_hook();
    RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateUnorderedAccessView(
    ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    ID3D12Resource* host_counter = UnwrapResource(pCounterResource);
    D3D12_UNORDERED_ACCESS_VIEW_DESC rw_desc{};
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc_ptr = pDesc;
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        (IsR9G9B9E5UavFallbackActive() ||
         (host_res != nullptr &&
          (IsTrackedFallbackResource(host_res) ||
           host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)))) {
        rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc_ptr = &rw_desc;
        RecordDev("ID3D12DeviceX::CreateUnorderedAccessView[R9G9B9E5Fallback]", 19, S_OK);
    }
    host_dev_->CreateUnorderedAccessView(host_res, host_counter, desc_ptr, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        const D3D12_GPU_VIRTUAL_ADDRESS va =
            (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? host_res->GetGPUVirtualAddress()
                : 0ull;
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, host_res, va,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateUnorderedAccessView", 19, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateRenderTargetView(
    ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    D3D12_RENDER_TARGET_VIEW_DESC rw_desc{};
    const D3D12_RENDER_TARGET_VIEW_DESC* desc_ptr = pDesc;
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        host_res != nullptr &&
        (IsTrackedFallbackResource(host_res) ||
         host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc_ptr = &rw_desc;
    }
    host_dev_->CreateRenderTargetView(host_res, desc_ptr, DestDescriptor);
    if (vulkan_routing_active_ && host_res) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_rtv_map_[DestDescriptor.ptr] = host_res;
    }
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, DestDescriptor, host_res, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateRenderTargetView", 20, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateDepthStencilView(
    ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    host_dev_->CreateDepthStencilView(host_res, pDesc, DestDescriptor);
    if (vulkan_routing_active_ && host_res) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_dsv_map_[DestDescriptor.ptr] = host_res;
    }
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DestDescriptor, host_res, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateDepthStencilView", 21, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateSampler(
    const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    host_dev_->CreateSampler(pDesc, DestDescriptor);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, DestDescriptor, nullptr, 0ull,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateSampler", 22, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CopyDescriptors(
    UINT NumDestDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
    const UINT* pDestDescriptorRangeSizes,
    UINT NumSrcDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
    const UINT* pSrcDescriptorRangeSizes,
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    host_dev_->CopyDescriptors(
        NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
        NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
        DescriptorHeapsType);
    if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        pDestDescriptorRangeStarts && pSrcDescriptorRangeStarts &&
        NumDestDescriptorRanges > 0u && NumSrcDescriptorRanges > 0u) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        UINT dst_r = 0u, dst_i = 0u;
        UINT src_r = 0u, src_i = 0u;
        while (dst_r < NumDestDescriptorRanges && src_r < NumSrcDescriptorRanges) {
            const UINT dst_cnt = pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[dst_r] : 1u;
            const UINT src_cnt = pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[src_r] : 1u;
            if (dst_i >= dst_cnt) { ++dst_r; dst_i = 0u; continue; }
            if (src_i >= src_cnt) { ++src_r; src_i = 0u; continue; }
            D3D12_CPU_DESCRIPTOR_HANDLE dst_h{
                pDestDescriptorRangeStarts[dst_r].ptr + static_cast<SIZE_T>(dst_i) * inc};
            D3D12_CPU_DESCRIPTOR_HANDLE src_h{
                pSrcDescriptorRangeStarts[src_r].ptr + static_cast<SIZE_T>(src_i) * inc};
            sampler_feedback_mgr_.CopyPreservedView(dst_h, src_h);
            ++dst_i;
            ++src_i;
        }
    }
    RecordDev("ID3D12DeviceX::CopyDescriptors", 23, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CopyDescriptorsSimple(
    UINT NumDescriptors,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
    D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
    D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) {
    host_dev_->CopyDescriptorsSimple(
        NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && NumDescriptors > 0u) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        for (UINT i = 0u; i < NumDescriptors; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE dst_h{
                DestDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc};
            D3D12_CPU_DESCRIPTOR_HANDLE src_h{
                SrcDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc};
            sampler_feedback_mgr_.CopyPreservedView(dst_h, src_h);
        }
    }
    RecordDev("ID3D12DeviceX::CopyDescriptorsSimple", 24, S_OK);
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info =
        host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, descs_ptr);
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo", 25, S_OK);
    return info;
}

D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE DeviceX::GetCustomHeapProperties(
    D3D12_HEAP_PROPERTIES* pRet,
    UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    D3D12_HEAP_PROPERTIES* props = host_dev_->GetCustomHeapProperties(pRet, nodeMask, heapType);
    RecordDev("ID3D12DeviceX::GetCustomHeapProperties", 26, S_OK);
    return props;
}
#else
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs_ptr);
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo", 25, S_OK);
    return info;
}

D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE DeviceX::GetCustomHeapProperties(
    UINT nodeMask, D3D12_HEAP_TYPE heapType) {
    const D3D12_HEAP_PROPERTIES props = host_dev_->GetCustomHeapProperties(nodeMask, heapType);
    RecordDev("ID3D12DeviceX::GetCustomHeapProperties", 26, S_OK);
    return props;
}
#endif

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    const D3D12_HEAP_FLAGS clean_heap_flags = SanitizeXboxHeapFlags(HeapFlags);
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreateCommittedResource(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreateCommittedResource(
        pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (desc_ptr) {
        TrackBufferVaResource(host_res, *desc_ptr, 0ull);
    }
    if (pDesc) {
        const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
        if (tile_mode != XG_TILE_MODE_LINEAR) {
            SetTrackedResourceTileMode(host_res, tile_mode);
        }
    }
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 27, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, pHeapProperties, clean_heap_flags, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riidResource, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap(
    const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    D3D12_HEAP_DESC clean_heap{};
    const D3D12_HEAP_DESC* heap_ptr = pDesc;
    if (pDesc) {
        clean_heap = *pDesc;
        clean_heap.Flags = SanitizeXboxHeapFlags(pDesc->Flags);
        heap_ptr = &clean_heap;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateHeap(heap_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateHeap(
        heap_ptr, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateHeap", 28, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResource(
    ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    ID3D12Heap* host_heap = UnwrapHeap(pHeap);
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreatePlacedResource(
            host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreatePlacedResource(
        host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    uint64_t address_heap_base_va = 0ull;
    if (host_heap) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = address_heaps_.find(host_heap);
        if (it != address_heaps_.end()) {
            address_heap_base_va = it->second;
        }
    }
    bool placed_used_shadow_fallback = false;
    if ((FAILED(hr) || !host_res) && address_heap_base_va != 0ull && desc_ptr) {
        // Non-UMA desktop GPUs may reject placed textures or UAV resources on OpenExistingHeapFromAddress heaps;
        // back with a DEFAULT-heap shadow resource synced automatically on ExecuteCommandLists.
        D3D12_HEAP_PROPERTIES def_hp{};
        def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC fallback_desc = *desc_ptr;
        fallback_desc.Alignment = 0ull;
        hr = host_dev_->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &fallback_desc, InitialState, clear_ptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        placed_used_shadow_fallback = SUCCEEDED(hr) && host_res != nullptr;
    }
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 29, S_OK);
    }
    if (pDesc) {
        const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
        if (tile_mode != XG_TILE_MODE_LINEAR) {
            SetTrackedResourceTileMode(host_res, tile_mode);
        }
    }
    uint64_t tracked_va = 0ull;
    if (host_heap && desc_ptr) {
        if (vulkan_routing_active_) {
            const D3D12_HEAP_DESC hd = host_heap->GetDesc();
            TrackVulkanResource(host_res, placed_used_shadow_fallback ? D3D12_HEAP_TYPE_DEFAULT : hd.Properties.Type, *desc_ptr);
        }
        if (address_heap_base_va != 0ull) {
            tracked_va = address_heap_base_va + HeapOffset;
            bool mirrored = false;
            if (placed_used_shadow_fallback &&
                desc_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                desc_ptr->Width > 0ull &&
                IsReadableGuestVaRange(tracked_va, desc_ptr->Width)) {
                openscarlett::TitleVaMappingDesc mdesc{};
                mdesc.title_va = tracked_va;
                mdesc.size_bytes = desc_ptr->Width;
                mdesc.kind = openscarlett::TitleVaResourceKind::Buffer;
                mdesc.existing_write_watch_cpu_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(tracked_va));
                mdesc.gpu_shadow_resource = host_res;
                mdesc.initial_state = InitialState;
                openscarlett::TitleVaRegionInfo reg_info{};
                if (title_va_mirror_.RegisterRegion(mdesc, host_dev_, reg_info).ok()) {
                    std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
                    mirrored_res_to_va_[host_res] = tracked_va;
                    mirrored_va_count_.store(
                        static_cast<uint32_t>(mirrored_res_to_va_.size()), std::memory_order_release);
                    mirrored = true;
                }
            }
            AutoRegisterUnifiedResource(host_res, tracked_va, *desc_ptr, InitialState, mirrored);
        }
        TrackBufferVaResource(host_res, *desc_ptr, tracked_va);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, tracked_va);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreateReservedResource(
            desc_ptr, InitialState, clear_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreateReservedResource(
        desc_ptr, InitialState, clear_ptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 30, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateReservedResource", 30, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateReservedResource", 30, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateSharedHandle(
    ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
    DWORD Access, LPCWSTR Name, HANDLE* pHandle) {
    const HRESULT hr = host_dev_->CreateSharedHandle(
        UnwrapProxyDeviceChild(pObject), pAttributes, Access, Name, pHandle);
    RecordDev("ID3D12DeviceX::CreateSharedHandle", 31, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenSharedHandle(
    HANDLE NTHandle, REFIID riid, void** ppvObj) {
    const HRESULT hr = host_dev_->OpenSharedHandle(NTHandle, riid, ppvObj);
    RecordDev("ID3D12DeviceX::OpenSharedHandle", 32, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenSharedHandleByName(
    LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) {
    const HRESULT hr = host_dev_->OpenSharedHandleByName(Name, Access, pNTHandle);
    RecordDev("ID3D12DeviceX::OpenSharedHandleByName", 33, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::MakeResident(
    UINT NumObjects, ID3D12Pageable* const* ppObjects) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev_->MakeResident(NumObjects, unwrapped.data());
    RecordDev("ID3D12DeviceX::MakeResident", 34, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::Evict(
    UINT NumObjects, ID3D12Pageable* const* ppObjects) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev_->Evict(NumObjects, unwrapped.data());
    RecordDev("ID3D12DeviceX::Evict", 35, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateFence(
    UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) {
    if (!ppFence) {
        const HRESULT hr = host_dev_->CreateFence(InitialValue, Flags, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
        return hr;
    }
    *ppFence = nullptr;
    ID3D12Fence* host_fence = nullptr;
    HRESULT hr = host_dev_->CreateFence(
        InitialValue, Flags, OS_D3D12_IID(ID3D12Fence), reinterpret_cast<void**>(&host_fence));
    if (FAILED(hr) || !host_fence) {
        RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) FenceXProxy(this, host_fence, Flags);
    host_fence->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateFence", 36, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppFence);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateFence", 36, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetDeviceRemovedReason() {
    const HRESULT host_hr = host_dev_ ? host_dev_->GetDeviceRemovedReason() : S_OK;
    if (FAILED(host_hr)) {
        device_removed_reason_.store(host_hr, std::memory_order_release);
        RecordDev("ID3D12DeviceX::GetDeviceRemovedReason", 37, host_hr);
        return host_hr;
    }
    const HRESULT tracked = device_removed_reason_.load(std::memory_order_acquire);
    RecordDev("ID3D12DeviceX::GetDeviceRemovedReason", 37, tracked);
    return tracked;
}

void STDMETHODCALLTYPE DeviceX::GetCopyableFootprints(
    const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
    UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
    UINT64* pRowSizeInBytes, UINT64* pTotalBytes) {
    if (pLayouts && NumSubresources > 0u) {
        std::memset(pLayouts, 0, sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * NumSubresources);
    }
    if (pNumRows && NumSubresources > 0u) {
        std::memset(pNumRows, 0, sizeof(UINT) * NumSubresources);
    }
    if (pRowSizeInBytes && NumSubresources > 0u) {
        std::memset(pRowSizeInBytes, 0, sizeof(UINT64) * NumSubresources);
    }
    if (pTotalBytes) {
        *pTotalBytes = 0ull;
    }
    if (!pResourceDesc) {
        RecordDev("ID3D12DeviceX::GetCopyableFootprints", 38, E_INVALIDARG);
        return;
    }

    D3D12_RESOURCE_DESC clean_desc = SanitizeXboxResourceDesc(*pResourceDesc);
    if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clean_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (clean_desc.Height == 0u) clean_desc.Height = 1u;
        if (clean_desc.DepthOrArraySize == 0u) clean_desc.DepthOrArraySize = 1u;
        if (clean_desc.MipLevels == 0u) clean_desc.MipLevels = 1u;
        if (clean_desc.SampleDesc.Count == 0u) clean_desc.SampleDesc.Count = 1u;
    }
    if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
        clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    UINT64 host_total = UINT64_MAX;
    host_dev_->GetCopyableFootprints(
        &clean_desc, FirstSubresource, NumSubresources, BaseOffset,
        pLayouts, pNumRows, pRowSizeInBytes, &host_total);

    if (host_total == UINT64_MAX && NumSubresources > 0u) {
        // Host D3D12 refused this Xbox-specific format/dimension/flag combination;
        // compute exact D3D12-spec aligned footprints so callers never observe UINT64_MAX or garbage.
        ComputeFallbackCopyableFootprints(
            clean_desc, FirstSubresource, NumSubresources, BaseOffset,
            pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
        RecordDev("ID3D12DeviceX::GetCopyableFootprints[XboxFallback]", 38, S_OK);
    } else if (pTotalBytes) {
        *pTotalBytes = (host_total == UINT64_MAX) ? 0ull : host_total;
    }
    RecordDev("ID3D12DeviceX::GetCopyableFootprints", 38, S_OK);
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateQueryHeap(
    const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) {
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateQueryHeap(pDesc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12QueryHeap* host_qheap = nullptr;
    HRESULT hr = host_dev_->CreateQueryHeap(
        pDesc, OS_D3D12_IID(ID3D12QueryHeap), reinterpret_cast<void**>(&host_qheap));
    if (FAILED(hr) || !host_qheap) {
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) QueryHeapXProxy(this, host_qheap, pDesc);
    host_qheap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateQueryHeap", 39, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetStablePowerState(BOOL Enable) {
    const HRESULT hr = host_dev_->SetStablePowerState(Enable);
    RecordDev("ID3D12DeviceX::SetStablePowerState", 40, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandSignature(
    const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
    REFIID riid, void** ppvCommandSignature) {
    ID3D12RootSignature* host_rs = UnwrapRootSignature(pRootSignature);
    if (!ppvCommandSignature) {
        const HRESULT hr = host_dev_->CreateCommandSignature(pDesc, host_rs, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
        return hr;
    }
    *ppvCommandSignature = nullptr;
    ID3D12CommandSignature* host_sig = nullptr;
    HRESULT hr = host_dev_->CreateCommandSignature(
        pDesc, host_rs, OS_D3D12_IID(ID3D12CommandSignature), reinterpret_cast<void**>(&host_sig));
    if (FAILED(hr) || !host_sig) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
        return hr;
    }
    const UINT stride = pDesc ? pDesc->ByteStride : 0u;
    const UINT num_args = pDesc ? pDesc->NumArgumentDescs : 0u;
    auto* proxy = new (std::nothrow) CommandSignatureXProxy(this, host_sig, stride, num_args);
    host_sig->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvCommandSignature);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::GetResourceTiling(
    ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
    D3D12_PACKED_MIP_INFO* pPackedMipDesc,
    D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
    UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
    D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) {
    host_dev_->GetResourceTiling(
        UnwrapResource(pTiledResource), pNumTilesForEntireResource, pPackedMipDesc,
        pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
        FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
    RecordDev("ID3D12DeviceX::GetResourceTiling", 42, S_OK);
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
LUID* STDMETHODCALLTYPE DeviceX::GetAdapterLuid(LUID* pRet) {
    LUID* luid = host_dev_->GetAdapterLuid(pRet);
    RecordDev("ID3D12DeviceX::GetAdapterLuid", 43, S_OK);
    return luid;
}
#else
LUID STDMETHODCALLTYPE DeviceX::GetAdapterLuid() {
    const LUID luid = host_dev_->GetAdapterLuid();
    RecordDev("ID3D12DeviceX::GetAdapterLuid", 43, S_OK);
    return luid;
}
#endif

// ID3D12Device1 (slots 44..46)
HRESULT STDMETHODCALLTYPE DeviceX::CreatePipelineLibrary(
    const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) {
    const HRESULT hr = host_dev1_
                           ? host_dev1_->CreatePipelineLibrary(
                                 pLibraryBlob, BlobLength, riid, ppPipelineLibrary)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreatePipelineLibrary", 44, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetEventOnMultipleFenceCompletion(
    ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
    D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) {
    if (!host_dev1_) {
        RecordDev("ID3D12DeviceX::SetEventOnMultipleFenceCompletion", 45, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    std::vector<ID3D12Fence*> unwrapped;
    ID3D12Fence* const* fence_ptr = ppFences;
    if (ppFences && NumFences > 0u) {
        unwrapped.resize(NumFences);
        for (UINT i = 0u; i < NumFences; ++i) {
            unwrapped[i] = UnwrapFence(ppFences[i]);
        }
        fence_ptr = unwrapped.data();
    }
    const HRESULT hr = host_dev1_->SetEventOnMultipleFenceCompletion(
        fence_ptr, pFenceValues, NumFences, Flags, hEvent);
    RecordDev("ID3D12DeviceX::SetEventOnMultipleFenceCompletion", 45, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetResidencyPriority(
    UINT NumObjects, ID3D12Pageable* const* ppObjects,
    const D3D12_RESIDENCY_PRIORITY* pPriorities) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev1_
                           ? host_dev1_->SetResidencyPriority(
                                 NumObjects, unwrapped.data(), pPriorities)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, hr);
    return hr;
}

// ID3D12Device2 (slot 47)
HRESULT STDMETHODCALLTYPE DeviceX::CreatePipelineState(
    const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    if (!host_dev2_) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!pDesc || !pDesc->pPipelineStateSubobjectStream ||
        pDesc->SizeInBytes < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
        const HRESULT hr = host_dev2_->CreatePipelineState(pDesc, riid, ppPipelineState);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }

    // Pass 1: zero-allocation scan of subobject stream for Xbox shaders, SPIR-V, and wrapped root signatures.
    const auto* raw_stream = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
    const size_t total_bytes = static_cast<size_t>(pDesc->SizeInBytes);
    constexpr size_t kBytecodeOffset = offsetof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>, value);
    constexpr size_t kRootSigOffset = offsetof(StreamSubobjLayout<ID3D12RootSignature*>, value);

    bool has_xbox_shader = false;
    bool has_spirv_shader = false;
    bool has_wrapped_root_sig = false;
    bool has_root_sig_subobj = false;
    bool root_sig_is_null = false;
    size_t root_sig_subobj_pos = 0u;
    bool has_as_stage = false;
    uint32_t shader_stage_count = 0;
    VulkanPipelineInfo spirv_stream_pi{};
    openscarlett::XboxParsedShaderContainer stream_vs_parsed{}, stream_ps_parsed{};
    bool has_stream_vs_parsed = false;
    bool has_stream_ps_parsed = false;

    size_t pos = 0;
    while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
        std::memcpy(&sub_type, raw_stream + pos, sizeof(sub_type));
        const size_t stride = GetSubobjectStrideBytes(sub_type);
        if (stride == 0u || pos + stride > total_bytes) {
            break;
        }
        if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            has_root_sig_subobj = true;
            root_sig_subobj_pos = pos;
            ID3D12RootSignature* rs = nullptr;
            std::memcpy(&rs, raw_stream + pos + kRootSigOffset, sizeof(rs));
            if (!rs) {
                root_sig_is_null = true;
            } else if (UnwrapRootSignature(rs) != rs) {
                has_wrapped_root_sig = true;
            }
        } else if (IsShaderSubobjectType(sub_type)) {
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, raw_stream + pos + kBytecodeOffset, sizeof(bc));
            if (bc.pShaderBytecode && bc.BytecodeLength > 0) {
                ++shader_stage_count;
                if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                    has_as_stage = true;
                }
                if (openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                    has_xbox_shader = true;
                    if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                        has_stream_vs_parsed = openscarlett::ParseFullXboxShaderContainer(
                            bc.pShaderBytecode, bc.BytecodeLength, stream_vs_parsed).ok();
                    } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                        has_stream_ps_parsed = openscarlett::ParseFullXboxShaderContainer(
                            bc.pShaderBytecode, bc.BytecodeLength, stream_ps_parsed).ok();
                    }
                }
                if (vulkan_routing_active_ && IsRawSpirvBytecode(bc.pShaderBytecode, bc.BytecodeLength)) {
                    has_spirv_shader = true;
                    auto words = CopySpirvWords(bc.pShaderBytecode, bc.BytecodeLength);
                    openscarlett::SpirvValidationInfo val{};
                    if (openscarlett::VulkanBackend::ValidateSpirvBinary(words, &val).ok()) {
                        if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                            spirv_stream_pi.as_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.as_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                            spirv_stream_pi.is_mesh = true;
                            spirv_stream_pi.ms_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.ms_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                            spirv_stream_pi.vs_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.vs_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                            spirv_stream_pi.ps_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.ps_entry = val.entry_point_name;
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                            spirv_stream_pi.is_compute = true;
                            spirv_stream_pi.cs_spirv = std::move(words);
                            if (!val.entry_point_name.empty()) spirv_stream_pi.cs_entry = val.entry_point_name;
                        }
                    }
                }
            }
        } else if (vulkan_routing_active_ &&
                   sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
            D3D12_RT_FORMAT_ARRAY rts{};
            std::memcpy(&rts, raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value), sizeof(rts));
            if (rts.NumRenderTargets > 0u) {
                spirv_stream_pi.color_format = DxgiToVulkanColorFormat(rts.RTFormats[0]);
            }
        }
        pos += stride;
    }

    if (vulkan_routing_active_ && has_spirv_shader) {
        if (!ppPipelineState) {
            RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_POINTER);
            return E_POINTER;
        }
        *ppPipelineState = nullptr;
        spirv_stream_pi.is_spirv_direct = true;
        auto* vk_pso = new (std::nothrow) PipelineStateXProxy(this, nullptr);
        if (!vk_pso) {
            RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(vk_pso)] = std::move(spirv_stream_pi);
        }
        const HRESULT hr = vk_pso->QueryInterface(riid, ppPipelineState);
        vk_pso->Release();
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }

    const D3D12_PIPELINE_STATE_STREAM_DESC* final_stream_desc = pDesc;
    D3D12_PIPELINE_STATE_STREAM_DESC lifted_stream{};
    std::vector<uint64_t> cloned_words;
    std::deque<std::vector<uint8_t>> lifted_blobs;

    if (has_xbox_shader || has_wrapped_root_sig) {
        // Pass 2: clone stream into pointer-aligned buffer, unwrap root sigs & lift Xbox shaders.
        const size_t base_words = (total_bytes + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
        cloned_words.assign(base_words, 0ull);
        auto* cloned_bytes = reinterpret_cast<uint8_t*>(cloned_words.data());
        std::memcpy(cloned_bytes, raw_stream, total_bytes);

        const uint32_t highest_sm = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
        const bool prefer_dxil_standin = (shader_stage_count > 1u && highest_sm >= 0x60u);

        openscarlett::XboxRootSignatureDesc fallback_rs_desc{};
        bool has_fallback_rs = false;

        pos = 0;
        while (pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
            std::memcpy(&sub_type, cloned_bytes + pos, sizeof(sub_type));
            const size_t stride = GetSubobjectStrideBytes(sub_type);
            if (stride == 0u || pos + stride > total_bytes) {
                break;
            }
            if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                ID3D12RootSignature* rs = nullptr;
                std::memcpy(&rs, cloned_bytes + pos + kRootSigOffset, sizeof(rs));
                ID3D12RootSignature* unwrapped_rs = UnwrapRootSignature(rs);
                if (unwrapped_rs != rs) {
                    std::memcpy(cloned_bytes + pos + kRootSigOffset, &unwrapped_rs, sizeof(unwrapped_rs));
                }
            } else if (has_xbox_shader && IsShaderSubobjectType(sub_type)) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, cloned_bytes + pos + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0 &&
                    openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                    const openscarlett::XboxParsedShaderContainer* paired = nullptr;
                    if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && has_stream_ps_parsed) {
                        paired = &stream_ps_parsed;
                    } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS && has_stream_vs_parsed) {
                        paired = &stream_vs_parsed;
                    }
                    std::vector<uint8_t> host_bc;
                    openscarlett::XboxParsedShaderContainer stage_parsed{};
                    if (!LiftXboxStageToHostBytecode(
                            bc.pShaderBytecode, bc.BytecodeLength,
                            has_as_stage, prefer_dxil_standin, paired, host_bc, &stage_parsed)) {
                        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_INVALIDARG);
                        return E_INVALIDARG;
                    }
                    // Prefer MS/AS/CS inferred or embedded root signature when stream omitted ROOT_SIGNATURE
                    if (!has_fallback_rs ||
                        sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ||
                        sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                        if (stage_parsed.root_signature.present) {
                            fallback_rs_desc = stage_parsed.root_signature;
                            has_fallback_rs = true;
                        } else if (stage_parsed.root_signature_inferred) {
                            fallback_rs_desc = stage_parsed.inferred_root_signature;
                            has_fallback_rs = true;
                        }
                    }
                    lifted_blobs.push_back(std::move(host_bc));
                    bc.pShaderBytecode = lifted_blobs.back().data();
                    bc.BytecodeLength = lifted_blobs.back().size();
                    std::memcpy(cloned_bytes + pos + kBytecodeOffset, &bc, sizeof(bc));
                }
            }
            pos += stride;
        }

        // If stream omitted ROOT_SIGNATURE (or passed nullptr) alongside an Xbox Mesh/Compute/Graphics
        // container without RTS0, synthesize & inject a host root signature from inferred bindings!
        size_t final_size_bytes = total_bytes;
        if ((!has_root_sig_subobj || root_sig_is_null) && has_fallback_rs) {
            ID3D12RootSignature* synth_rs = SynthesizeHostRootSignatureFromXboxDesc(fallback_rs_desc);
            if (synth_rs) {
                if (has_root_sig_subobj && root_sig_is_null) {
                    std::memcpy(cloned_bytes + root_sig_subobj_pos + kRootSigOffset, &synth_rs, sizeof(synth_rs));
                } else {
                    StreamSubobjLayout<ID3D12RootSignature*> rs_sub{};
                    rs_sub.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                    rs_sub.value = synth_rs;
                    const size_t aligned_end =
                        (final_size_bytes + sizeof(void*) - 1u) & ~(sizeof(void*) - 1u);
                    const size_t new_total = aligned_end + sizeof(rs_sub);
                    cloned_words.resize((new_total + sizeof(uint64_t) - 1u) / sizeof(uint64_t), 0ull);
                    cloned_bytes = reinterpret_cast<uint8_t*>(cloned_words.data());
                    std::memcpy(cloned_bytes + aligned_end, &rs_sub, sizeof(rs_sub));
                    final_size_bytes = new_total;
                }
            }
        }

        lifted_stream.SizeInBytes = final_size_bytes;
        lifted_stream.pPipelineStateSubobjectStream = cloned_bytes;
        final_stream_desc = &lifted_stream;
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev2_->CreatePipelineState(final_stream_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = host_dev2_->CreatePipelineState(
        final_stream_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    if (FAILED(hr) || !host_pso) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    if (vulkan_routing_active_ && has_xbox_shader) {
        VulkanPipelineInfo vk_pi = std::move(spirv_stream_pi);
        vk_pi.is_spirv_direct = false;
        bool has_any = false;
        size_t sp_pos = 0u;
        while (sp_pos + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= total_bytes) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type{};
            std::memcpy(&sub_type, raw_stream + sp_pos, sizeof(sub_type));
            const size_t stride = GetSubobjectStrideBytes(sub_type);
            if (stride == 0u || sp_pos + stride > total_bytes) break;
            if (IsShaderSubobjectType(sub_type)) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, raw_stream + sp_pos + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0u &&
                    openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                    std::vector<uint32_t> spv;
                    if (LiftXboxStageToSpirv(bc.pShaderBytecode, bc.BytecodeLength,
                                             has_as_stage, nullptr, spv)) {
                        has_any = true;
                        if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                            vk_pi.as_spirv = std::move(spv);
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                            vk_pi.is_mesh = true;
                            vk_pi.ms_spirv = std::move(spv);
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                            vk_pi.vs_spirv = std::move(spv);
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                            vk_pi.ps_spirv = std::move(spv);
                        } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                            vk_pi.is_compute = true;
                            vk_pi.cs_spirv = std::move(spv);
                        }
                    }
                }
            }
            sp_pos += stride;
        }
        if (has_any) {
            std::lock_guard<std::mutex> lock(vk_state_mutex_);
            vk_pipelines_[static_cast<ID3D12PipelineState*>(proxy)] = std::move(vk_pi);
        }
    }
    hr = proxy->QueryInterface(riid, ppPipelineState);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
    return hr;
}

// ID3D12Device3 (slots 48..50)
HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromAddress(
    const void* pAddress, REFIID riid, void** ppvHeap) {
    if (!host_dev3_) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev3_->OpenExistingHeapFromAddress(pAddress, riid, nullptr);
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev3_->OpenExistingHeapFromAddress(
        pAddress, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
        return hr;
    }
    const uint64_t addr_va = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pAddress));
    if (pAddress) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        address_heaps_[host_heap] = addr_va;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, addr_va);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::OpenExistingHeapFromAddress", 48, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::OpenExistingHeapFromFileMapping(
    HANDLE hFileMapping, REFIID riid, void** ppvHeap) {
    if (!host_dev3_) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev3_->OpenExistingHeapFromFileMapping(hFileMapping, riid, nullptr);
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev3_->OpenExistingHeapFromFileMapping(
        hFileMapping, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::OpenExistingHeapFromFileMapping", 49, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::EnqueueMakeResident(
    D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
    ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) {
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    const HRESULT hr = host_dev3_
                           ? host_dev3_->EnqueueMakeResident(
                                 Flags, NumObjects, unwrapped.data(),
                                 UnwrapFence(pFenceToSignal), FenceValueToSignal)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, hr);
    return hr;
}

// ID3D12Device4 (slots 51..56)
HRESULT STDMETHODCALLTYPE DeviceX::CreateCommandList1(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid, void** ppCommandList) {
    if (!ppCommandList) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_POINTER);
        return E_POINTER;
    }
    *ppCommandList = nullptr;
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    ID3D12GraphicsCommandList* host_cl = nullptr;
    HRESULT hr = host_dev4_->CreateCommandList1(nodeMask, type, flags, IID_PPV_ARGS(&host_cl));
    if (FAILED(hr) || !host_cl) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, hr);
        return hr;
    }
    auto* wrapper = new (std::nothrow) CommandListX(this, host_cl);
    host_cl->Release();
    if (!wrapper) {
        RecordDev("ID3D12DeviceX::CreateCommandList1", 51, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = wrapper->QueryInterface(riid, ppCommandList);
    wrapper->Release();
    RecordDev("ID3D12DeviceX::CreateCommandList1", 51, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateProtectedResourceSession(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) {
    const HRESULT hr = host_dev4_
                           ? host_dev4_->CreateProtectedResourceSession(pDesc, riid, ppSession)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateProtectedResourceSession", 52, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommittedResource1(
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource, void** ppvResource) {
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    const D3D12_HEAP_FLAGS clean_heap_flags = SanitizeXboxHeapFlags(HeapFlags);
    if (!ppvResource) {
        const HRESULT hr = host_dev4_->CreateCommittedResource1(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, pProtectedSession, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev4_->CreateCommittedResource1(
        pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 53, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, pHeapProperties, clean_heap_flags, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riidResource, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateHeap1(
    const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvHeap) {
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_HEAP_DESC clean_heap{};
    const D3D12_HEAP_DESC* heap_ptr = pDesc;
    if (pDesc) {
        clean_heap = *pDesc;
        clean_heap.Flags = SanitizeXboxHeapFlags(pDesc->Flags);
        heap_ptr = &clean_heap;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev4_->CreateHeap1(heap_ptr, pProtectedSession, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
        return hr;
    }
    *ppvHeap = nullptr;
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev4_->CreateHeap1(
        heap_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, 0ull);
    host_heap->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvHeap);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateReservedResource1(
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riid, void** ppvResource) {
    if (!host_dev4_) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (pDesc) {
        clean_desc = SanitizeXboxResourceDesc(*pDesc);
        if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
            clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            did_r9g9b9e5_fallback = true;
            if (pOptimizedClearValue &&
                pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                clean_clear = *pOptimizedClearValue;
                clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                clear_ptr = &clean_clear;
            }
        }
        desc_ptr = &clean_desc;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev4_->CreateReservedResource1(
            desc_ptr, InitialState, clear_ptr, pProtectedSession, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
        return hr;
    }
    *ppvResource = nullptr;
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev4_->CreateReservedResource1(
        desc_ptr, InitialState, clear_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
        return hr;
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 55, S_OK);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr, nullptr, D3D12_HEAP_FLAG_NONE, 0ull);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateReservedResource1", 55, hr);
    return hr;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info = nullptr;
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            pRet, visibleMask, numResourceDescs, descs_ptr, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, descs_ptr);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, E_NOINTERFACE);
    }
    return info;
}
#else
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo1(
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u && IsR9G9B9E5UavFallbackActive()) {
        bool need_rw = false;
        for (UINT i = 0; i < numResourceDescs; ++i) {
            if (ShouldRewriteR9G9B9E5Uav(pResourceDescs[i])) {
                need_rw = true;
                break;
            }
        }
        if (need_rw) {
            rw_descs.assign(pResourceDescs, pResourceDescs + numResourceDescs);
            for (auto& d : rw_descs) {
                if (ShouldRewriteR9G9B9E5Uav(d)) d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            descs_ptr = rw_descs.data();
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    if (host_dev4_) {
        info = host_dev4_->GetResourceAllocationInfo1(
            visibleMask, numResourceDescs, descs_ptr, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, S_OK);
    } else {
        info = host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs_ptr);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo1", 56, E_NOINTERFACE);
    }
    return info;
}
#endif

// ID3D12Device5 (slots 57..64)
HRESULT STDMETHODCALLTYPE DeviceX::CreateLifetimeTracker(
    ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) {
    const HRESULT hr = host_dev5_
                           ? host_dev5_->CreateLifetimeTracker(pOwner, riid, ppvTracker)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateLifetimeTracker", 57, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::RemoveDevice() {
    device_removed_reason_.store(DXGI_ERROR_DEVICE_REMOVED, std::memory_order_release);
    if (host_dev5_) {
        host_dev5_->RemoveDevice();
    }
    if (host_dev_) {
        const HRESULT host_hr = host_dev_->GetDeviceRemovedReason();
        if (FAILED(host_hr)) {
            device_removed_reason_.store(host_hr, std::memory_order_release);
        }
    }
    RecordDev("ID3D12DeviceX::RemoveDevice", 58, S_OK);
}

HRESULT STDMETHODCALLTYPE DeviceX::EnumerateMetaCommands(
    UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) {
    if (!pNumMetaCommands) {
        RecordDev("ID3D12DeviceX::EnumerateMetaCommands", 59, E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (host_dev5_) {
        HRESULT hr = host_dev5_->EnumerateMetaCommands(pNumMetaCommands, pDescs);
        if (hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED) {
            *pNumMetaCommands = 0u;
            hr = S_OK;
        }
        RecordDev("ID3D12DeviceX::EnumerateMetaCommands", 59, hr);
        return hr;
    }
    *pNumMetaCommands = 0u;
    RecordDev("ID3D12DeviceX::EnumerateMetaCommands", 59, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::EnumerateMetaCommandParameters(
    REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
    UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
    D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) {
    if (!host_dev5_) {
        if (pTotalStructureSizeInBytes) *pTotalStructureSizeInBytes = 0u;
        if (pParameterCount) *pParameterCount = 0u;
        RecordDev("ID3D12DeviceX::EnumerateMetaCommandParameters", 60, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const HRESULT hr = host_dev5_->EnumerateMetaCommandParameters(
        CommandId, Stage, pTotalStructureSizeInBytes, pParameterCount, pParameterDescs);
    RecordDev("ID3D12DeviceX::EnumerateMetaCommandParameters", 60, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateMetaCommand(
    REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
    SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) {
    if (!ppMetaCommand) {
        RecordDev("ID3D12DeviceX::CreateMetaCommand", 61, E_POINTER);
        return E_POINTER;
    }
    *ppMetaCommand = nullptr;
    if (!host_dev5_) {
        RecordDev("ID3D12DeviceX::CreateMetaCommand", 61, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const HRESULT hr = host_dev5_->CreateMetaCommand(
        CommandId, NodeMask, pCreationParametersData,
        CreationParametersDataSizeInBytes, riid, ppMetaCommand);
    RecordDev("ID3D12DeviceX::CreateMetaCommand", 61, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateStateObject(
    const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) {
    if (!host_dev5_) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_NOINTERFACE);
        return E_NOINTERFACE;
    }

    const D3D12_STATE_OBJECT_DESC* desc_ptr = pDesc;
    D3D12_STATE_OBJECT_DESC cloned_desc{};
    std::vector<D3D12_STATE_SUBOBJECT> cloned_subs;
    std::deque<D3D12_GLOBAL_ROOT_SIGNATURE> cloned_global_rs;
    std::deque<D3D12_LOCAL_ROOT_SIGNATURE> cloned_local_rs;
    std::deque<D3D12_EXISTING_COLLECTION_DESC> cloned_collections;
    std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> cloned_assocs;

    if (pDesc && pDesc->pSubobjects && pDesc->NumSubobjects > 0u) {
        bool needs_clone = false;
        for (UINT i = 0u; i < pDesc->NumSubobjects; ++i) {
            const auto& sub = pDesc->pSubobjects[i];
            if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && sub.pDesc) {
                const auto* grs = static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(sub.pDesc);
                if (grs->pGlobalRootSignature &&
                    UnwrapRootSignature(grs->pGlobalRootSignature) != grs->pGlobalRootSignature) {
                    needs_clone = true;
                    break;
                }
            } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && sub.pDesc) {
                const auto* lrs = static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(sub.pDesc);
                if (lrs->pLocalRootSignature &&
                    UnwrapRootSignature(lrs->pLocalRootSignature) != lrs->pLocalRootSignature) {
                    needs_clone = true;
                    break;
                }
            } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && sub.pDesc) {
                const auto* ec = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(sub.pDesc);
                if (ec->pExistingCollection &&
                    UnwrapStateObject(ec->pExistingCollection) != ec->pExistingCollection) {
                    needs_clone = true;
                    break;
                }
            }
        }

        if (needs_clone) {
            cloned_subs.assign(pDesc->pSubobjects, pDesc->pSubobjects + pDesc->NumSubobjects);
            for (UINT i = 0u; i < pDesc->NumSubobjects; ++i) {
                auto& sub = cloned_subs[i];
                if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && sub.pDesc) {
                    D3D12_GLOBAL_ROOT_SIGNATURE grs =
                        *static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(sub.pDesc);
                    grs.pGlobalRootSignature = UnwrapRootSignature(grs.pGlobalRootSignature);
                    cloned_global_rs.push_back(grs);
                    sub.pDesc = &cloned_global_rs.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && sub.pDesc) {
                    D3D12_LOCAL_ROOT_SIGNATURE lrs =
                        *static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(sub.pDesc);
                    lrs.pLocalRootSignature = UnwrapRootSignature(lrs.pLocalRootSignature);
                    cloned_local_rs.push_back(lrs);
                    sub.pDesc = &cloned_local_rs.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && sub.pDesc) {
                    D3D12_EXISTING_COLLECTION_DESC ec =
                        *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(sub.pDesc);
                    ec.pExistingCollection = UnwrapStateObject(ec.pExistingCollection);
                    cloned_collections.push_back(ec);
                    sub.pDesc = &cloned_collections.back();
                } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION &&
                           sub.pDesc) {
                    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc =
                        *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(sub.pDesc);
                    if (assoc.pSubobjectToAssociate >= pDesc->pSubobjects &&
                        assoc.pSubobjectToAssociate < pDesc->pSubobjects + pDesc->NumSubobjects) {
                        const ptrdiff_t idx = assoc.pSubobjectToAssociate - pDesc->pSubobjects;
                        assoc.pSubobjectToAssociate = &cloned_subs[static_cast<size_t>(idx)];
                    }
                    cloned_assocs.push_back(assoc);
                    sub.pDesc = &cloned_assocs.back();
                }
            }
            cloned_desc.Type = pDesc->Type;
            cloned_desc.NumSubobjects = pDesc->NumSubobjects;
            cloned_desc.pSubobjects = cloned_subs.data();
            desc_ptr = &cloned_desc;
        }
    }

    if (!ppStateObject) {
        const HRESULT hr = host_dev5_->CreateStateObject(desc_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    *ppStateObject = nullptr;
    ID3D12StateObject* host_so = nullptr;
    HRESULT hr = host_dev5_->CreateStateObject(
        desc_ptr, OS_D3D12_IID(ID3D12StateObject), reinterpret_cast<void**>(&host_so));
    if ((FAILED(hr) || !host_so) &&
        adapter_info_.raytracing_tier == 0u &&
        pDesc != nullptr &&
        pDesc->pSubobjects != nullptr &&
        pDesc->NumSubobjects > 0u &&
        (pDesc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION ||
         pDesc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)) {
        // Non-DXR adapter (e.g. GTX 1650 Ti / Win10 WARP): provide software StateObjectXProxy
        // supporting ID3D12StateObject + ID3D12StateObjectProperties shader identifier queries.
        auto* sw_proxy = new (std::nothrow) StateObjectXProxy(this, nullptr, pDesc);
        if (!sw_proxy) {
            RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = sw_proxy->QueryInterface(riid, ppStateObject);
        sw_proxy->Release();
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    if (FAILED(hr) || !host_so) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) StateObjectXProxy(this, host_so, pDesc);
    host_so->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateStateObject", 62, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppStateObject);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreateStateObject", 62, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::GetRaytracingAccelerationStructurePrebuildInfo(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) {
    if (host_dev5_) {
        host_dev5_->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    }
    RecordDev("ID3D12DeviceX::GetRaytracingAccelerationStructurePrebuildInfo",
              63, host_dev5_ ? S_OK : E_NOINTERFACE);
}

D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE DeviceX::CheckDriverMatchingIdentifier(
    D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
    const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) {
    if (!pIdentifierToCheck) {
        RecordDev("ID3D12DeviceX::CheckDriverMatchingIdentifier", 64, E_INVALIDARG);
        return D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
    }
    if (SerializedDataType == D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE &&
        IsEqualGUID(pIdentifierToCheck->DriverOpaqueGUID, kOpenScarlettSwAsDriverGuid)) {
        RecordDev("ID3D12DeviceX::CheckDriverMatchingIdentifier", 64, S_OK);
        return D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
    }
    if (host_dev5_) {
        const auto status = host_dev5_->CheckDriverMatchingIdentifier(
            SerializedDataType, pIdentifierToCheck);
        RecordDev("ID3D12DeviceX::CheckDriverMatchingIdentifier", 64, S_OK);
        return status;
    }
    const auto fallback =
        (SerializedDataType == D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE)
            ? D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION
            : D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
    RecordDev("ID3D12DeviceX::CheckDriverMatchingIdentifier", 64, S_OK);
    return fallback;
}

// ID3D12Device6 (slot 65)
HRESULT STDMETHODCALLTYPE DeviceX::SetBackgroundProcessingMode(
    D3D12_BACKGROUND_PROCESSING_MODE Mode,
    D3D12_MEASUREMENTS_ACTION MeasurementsAction,
    HANDLE hEventToSignalUponCompletion,
    BOOL* pbFurtherMeasurementsDesired) {
    const HRESULT hr = host_dev6_
                           ? host_dev6_->SetBackgroundProcessingMode(
                                 Mode, MeasurementsAction, hEventToSignalUponCompletion,
                                 pbFurtherMeasurementsDesired)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::SetBackgroundProcessingMode", 65, hr);
    return hr;
}

// ID3D12Device7 (slots 66..67)
HRESULT STDMETHODCALLTYPE DeviceX::AddToStateObject(
    const D3D12_STATE_OBJECT_DESC* pAddition,
    ID3D12StateObject* pStateObjectToGrowFrom,
    REFIID riid,
    void** ppNewStateObject) {
    if (!host_dev7_) {
        RecordDev("ID3D12DeviceX::AddToStateObject", 66, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    ID3D12StateObject* host_so = nullptr;
    HRESULT hr = host_dev7_->AddToStateObject(
        pAddition, UnwrapStateObject(pStateObjectToGrowFrom),
        OS_D3D12_IID(ID3D12StateObject), reinterpret_cast<void**>(&host_so));
    if (SUCCEEDED(hr) && host_so && ppNewStateObject) {
        auto* proxy = new (std::nothrow) StateObjectXProxy(this, host_so);
        host_so->Release();
        if (!proxy) {
            RecordDev("ID3D12DeviceX::AddToStateObject", 66, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = proxy->QueryInterface(riid, ppNewStateObject);
        proxy->Release();
    } else if (host_so) {
        host_so->Release();
    }
    RecordDev("ID3D12DeviceX::AddToStateObject", 66, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateProtectedResourceSession1(
    const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc,
    REFIID riid,
    void** ppSession) {
    const HRESULT hr = host_dev7_
                           ? host_dev7_->CreateProtectedResourceSession1(pDesc, riid, ppSession)
                           : E_NOINTERFACE;
    RecordDev("ID3D12DeviceX::CreateProtectedResourceSession1", 67, hr);
    return hr;
}

// ID3D12Device8 (slots 68..72)
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo2(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask,
    UINT numResourceDescs,
    const D3D12_RESOURCE_DESC1* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    if (host_dev8_) {
        D3D12_RESOURCE_ALLOCATION_INFO* info = host_dev8_->GetResourceAllocationInfo2(
            pRet, visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo2", 68, S_OK);
        return info;
    }
    std::vector<D3D12_RESOURCE_DESC> d0s;
    if (pResourceDescs && numResourceDescs > 0u) {
        d0s.reserve(numResourceDescs);
        for (UINT i = 0; i < numResourceDescs; ++i) d0s.push_back(Desc1ToDesc0(pResourceDescs[i]));
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info = host_dev_->GetResourceAllocationInfo(
        pRet, visibleMask, numResourceDescs, d0s.empty() ? nullptr : d0s.data());
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo2", 68, S_OK);
    return info;
}
#else
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo2(
    UINT visibleMask,
    UINT numResourceDescs,
    const D3D12_RESOURCE_DESC1* pResourceDescs,
    D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
    if (host_dev8_) {
        const D3D12_RESOURCE_ALLOCATION_INFO info = host_dev8_->GetResourceAllocationInfo2(
            visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        RecordDev("ID3D12DeviceX::GetResourceAllocationInfo2", 68, S_OK);
        return info;
    }
    std::vector<D3D12_RESOURCE_DESC> d0s;
    if (pResourceDescs && numResourceDescs > 0u) {
        d0s.reserve(numResourceDescs);
        for (UINT i = 0; i < numResourceDescs; ++i) d0s.push_back(Desc1ToDesc0(pResourceDescs[i]));
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info = host_dev_->GetResourceAllocationInfo(
        visibleMask, numResourceDescs, d0s.empty() ? nullptr : d0s.data());
    RecordDev("ID3D12DeviceX::GetResourceAllocationInfo2", 68, S_OK);
    return info;
}
#endif

HRESULT STDMETHODCALLTYPE DeviceX::CreateCommittedResource2(
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    ID3D12ProtectedResourceSession* pProtectedSession,
    REFIID riidResource,
    void** ppvResource) {
    D3D12_RESOURCE_DESC1 clean_desc1{};
    const D3D12_RESOURCE_DESC1* desc1_ptr = pDesc;
    if (pDesc) {
        clean_desc1 = SanitizeXboxResourceDesc1(*pDesc);
        desc1_ptr = &clean_desc1;
    }
    const D3D12_HEAP_FLAGS clean_hf = SanitizeXboxHeapFlags(HeapFlags);
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = E_NOINTERFACE;

    // Live SamplerFeedbackManager intercept: transparently supports opaque feedback formats
    // on both Tier 0.9/1.0 hardware AND desktop GPUs / WARP lacking native sampler feedback.
    if (desc1_ptr && IsSamplerFeedbackFormat(desc1_ptr->Format)) {
        SamplerFeedbackMapDesc sf_desc{};
        sf_desc.kind =
            (static_cast<uint32_t>(desc1_ptr->Format) == kDxgiFormatSamplerFeedbackMinMipOpaqueValue)
                ? SamplerFeedbackKind::MinMipOpaque
                : SamplerFeedbackKind::MipRegionUsedOpaque;
        sf_desc.target_width = static_cast<uint32_t>(std::max<UINT64>(1ull, desc1_ptr->Width));
        sf_desc.target_height = std::max<UINT>(1u, desc1_ptr->Height);
        sf_desc.target_array_size = std::max<UINT16>(1u, desc1_ptr->DepthOrArraySize);
        sf_desc.target_mip_levels = std::max<UINT16>(1u, desc1_ptr->MipLevels);
        sf_desc.mip_region.width =
            desc1_ptr->SamplerFeedbackMipRegion.Width ? desc1_ptr->SamplerFeedbackMipRegion.Width : 16u;
        sf_desc.mip_region.height =
            desc1_ptr->SamplerFeedbackMipRegion.Height ? desc1_ptr->SamplerFeedbackMipRegion.Height : 16u;
        sf_desc.mip_region.depth =
            desc1_ptr->SamplerFeedbackMipRegion.Depth ? desc1_ptr->SamplerFeedbackMipRegion.Depth : 1u;

        bool created_native = false;
        if (sampler_feedback_mgr_.HasNativeSamplerFeedback() && host_dev8_) {
            hr = host_dev8_->CreateCommittedResource2(
                pHeapProperties, clean_hf, desc1_ptr,
                InitialResourceState, pOptimizedClearValue, pProtectedSession,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            if (SUCCEEDED(hr) && host_res) {
                created_native = true;
                (void)sampler_feedback_mgr_.RegisterExternalFeedbackMap(
                    host_res, nullptr, sf_desc, /*is_native_resource=*/true, nullptr);
            }
        }
        if (!created_native) {
            openscarlett::Status sf_st = sampler_feedback_mgr_.CreateFeedbackMap(
                sf_desc, nullptr, &host_res, nullptr);
            hr = sf_st.ok() ? S_OK : E_FAIL;
        }
        if (SUCCEEDED(hr) && host_res && ppvResource) {
            const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*desc1_ptr);
            auto* proxy = new (std::nothrow) ResourceXProxy(
                this, host_res, &d0, pHeapProperties, clean_hf);
            host_res->Release();
            if (!proxy) {
                RecordDev("ID3D12DeviceX::CreateCommittedResource2", 69, E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
            hr = proxy->QueryInterface(riidResource, ppvResource);
            proxy->Release();
        } else if (host_res) {
            host_res->Release();
        }
        RecordDev("ID3D12DeviceX::CreateCommittedResource2[SamplerFeedback]", 69, hr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource2", 69, hr);
        return hr;
    }

    if (host_dev8_) {
        hr = host_dev8_->CreateCommittedResource2(
            pHeapProperties, clean_hf, desc1_ptr,
            InitialResourceState, pOptimizedClearValue, pProtectedSession,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    } else if (desc1_ptr) {
        const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*desc1_ptr);
        hr = host_dev_->CreateCommittedResource(
            pHeapProperties, clean_hf, &d0,
            InitialResourceState, pOptimizedClearValue,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }
    if (SUCCEEDED(hr) && host_res && ppvResource) {
        const D3D12_RESOURCE_DESC d0 = desc1_ptr ? Desc1ToDesc0(*desc1_ptr) : D3D12_RESOURCE_DESC{};
        if (desc1_ptr) {
            TrackBufferVaResource(host_res, d0, 0ull);
        }
        if (pDesc) {
            const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
            if (tile_mode != XG_TILE_MODE_LINEAR) {
                SetTrackedResourceTileMode(host_res, tile_mode);
            }
        }
        if (vulkan_routing_active_ && pHeapProperties && desc1_ptr) {
            TrackVulkanResource(host_res, pHeapProperties->Type, d0);
        }
        auto* proxy = new (std::nothrow) ResourceXProxy(
            this, host_res, desc1_ptr ? &d0 : nullptr, pHeapProperties, clean_hf);
        host_res->Release();
        if (!proxy) {
            RecordDev("ID3D12DeviceX::CreateCommittedResource2", 69, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = proxy->QueryInterface(riidResource, ppvResource);
        proxy->Release();
    } else if (host_res) {
        host_res->Release();
    }
    RecordDev("ID3D12DeviceX::CreateCommittedResource2", 69, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResource1(
    ID3D12Heap* pHeap,
    UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC1* pDesc,
    D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid,
    void** ppvResource) {
    D3D12_RESOURCE_DESC1 clean_desc1{};
    const D3D12_RESOURCE_DESC1* desc1_ptr = pDesc;
    if (pDesc) {
        clean_desc1 = SanitizeXboxResourceDesc1(*pDesc);
        desc1_ptr = &clean_desc1;
    }
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = E_NOINTERFACE;
    if (host_dev8_) {
        hr = host_dev8_->CreatePlacedResource1(
            UnwrapHeap(pHeap), HeapOffset, desc1_ptr, InitialState, pOptimizedClearValue,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    } else if (desc1_ptr) {
        const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*desc1_ptr);
        hr = host_dev_->CreatePlacedResource(
            UnwrapHeap(pHeap), HeapOffset, &d0, InitialState, pOptimizedClearValue,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }
    if (SUCCEEDED(hr) && host_res && ppvResource) {
        const D3D12_RESOURCE_DESC d0 = desc1_ptr ? Desc1ToDesc0(*desc1_ptr) : D3D12_RESOURCE_DESC{};
        if (desc1_ptr) {
            TrackBufferVaResource(host_res, d0, 0ull);
        }
        if (pDesc) {
            const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
            if (tile_mode != XG_TILE_MODE_LINEAR) {
                SetTrackedResourceTileMode(host_res, tile_mode);
            }
        }
        auto* proxy = new (std::nothrow) ResourceXProxy(this, host_res, desc1_ptr ? &d0 : nullptr);
        host_res->Release();
        if (!proxy) {
            RecordDev("ID3D12DeviceX::CreatePlacedResource1", 70, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = proxy->QueryInterface(riid, ppvResource);
        proxy->Release();
    } else if (host_res) {
        host_res->Release();
    }
    RecordDev("ID3D12DeviceX::CreatePlacedResource1", 70, hr);
    return hr;
}

void STDMETHODCALLTYPE DeviceX::CreateSamplerFeedbackUnorderedAccessView(
    ID3D12Resource* pTargetedResource,
    ID3D12Resource* pFeedbackResource,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    ID3D12Resource* host_target = UnwrapResource(pTargetedResource);
    ID3D12Resource* host_feedback = UnwrapResource(pFeedbackResource);

    HRESULT hr = S_OK;
    if (host_feedback && DestDescriptor.ptr != 0u) {
        SamplerFeedbackMapInfo existing{};
        if (!sampler_feedback_mgr_.LookupFeedbackMap(host_feedback, existing)) {
            SamplerFeedbackMapDesc auto_desc{};
            const D3D12_RESOURCE_DESC fb_rd = host_feedback->GetDesc();
            const bool is_native_fmt = IsSamplerFeedbackFormat(fb_rd.Format);
            auto_desc.kind =
                (static_cast<uint32_t>(fb_rd.Format) == kDxgiFormatSamplerFeedbackMipRegionUsedOpaqueValue)
                    ? SamplerFeedbackKind::MipRegionUsedOpaque
                    : SamplerFeedbackKind::MinMipOpaque;
            if (host_target) {
                const D3D12_RESOURCE_DESC tdesc = host_target->GetDesc();
                auto_desc.target_width = static_cast<uint32_t>(std::max<UINT64>(1ull, tdesc.Width));
                auto_desc.target_height = std::max<UINT>(1u, tdesc.Height);
                auto_desc.target_array_size = std::max<UINT16>(1u, tdesc.DepthOrArraySize);
                auto_desc.target_mip_levels = std::max<UINT16>(1u, tdesc.MipLevels);
            } else {
                auto_desc.target_width = static_cast<uint32_t>(std::max<UINT64>(16ull, fb_rd.Width));
                auto_desc.target_height = std::max<UINT>(16u, fb_rd.Height);
            }
            (void)sampler_feedback_mgr_.RegisterExternalFeedbackMap(
                host_feedback, host_target, auto_desc, is_native_fmt, nullptr);
        }
        openscarlett::Status st = sampler_feedback_mgr_.CreateSamplerFeedbackUnorderedAccessView(
            host_target, host_feedback, DestDescriptor, nullptr);
        if (!st.ok()) {
            hr = E_FAIL;
        }
    } else if (host_dev8_) {
        host_dev8_->CreateSamplerFeedbackUnorderedAccessView(
            host_target, host_feedback, DestDescriptor);
    } else {
        hr = E_NOINTERFACE;
    }
    RecordDev("ID3D12DeviceX::CreateSamplerFeedbackUnorderedAccessView", 71, hr);
}

void STDMETHODCALLTYPE DeviceX::GetCopyableFootprints1(
    const D3D12_RESOURCE_DESC1* pResourceDesc,
    UINT FirstSubresource,
    UINT NumSubresources,
    UINT64 BaseOffset,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
    UINT* pNumRows,
    UINT64* pRowSizeInBytes,
    UINT64* pTotalBytes) {
    if (host_dev8_) {
        host_dev8_->GetCopyableFootprints1(
            pResourceDesc, FirstSubresource, NumSubresources, BaseOffset,
            pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    } else if (pResourceDesc) {
        const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*pResourceDesc);
        host_dev_->GetCopyableFootprints(
            &d0, FirstSubresource, NumSubresources, BaseOffset,
            pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    }
    RecordDev("ID3D12DeviceX::GetCopyableFootprints1", 72, S_OK);
}

// ID3D12DeviceX Xbox extensions (slots 73..79; slots 73..75 match real GDKX ID3D12Device8)
HRESULT STDMETHODCALLTYPE DeviceX::SetFrameIntervalX(
    IDXGIOutput* pOutput, uint32_t PeriodMicroseconds,
    uint32_t MaxFrameLatency, uint32_t Flags) {
    (void)pOutput;
    (void)Flags;
    frame_interval_us_.store(PeriodMicroseconds, std::memory_order_relaxed);
    max_frame_latency_.store(MaxFrameLatency == 0 ? 1u : MaxFrameLatency, std::memory_order_relaxed);
    RecordDev("ID3D12DeviceX::SetFrameIntervalX", 73, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ScheduleFrameEventX(
    D3D12XBOX_FRAME_EVENT Event, uint64_t DelayMicroseconds,
    HANDLE hEvent, uint32_t Flags) {
    (void)Event;
    (void)DelayMicroseconds;
    (void)Flags;
    if (hEvent) {
        SetEvent(hEvent);
    }
    RecordDev("ID3D12DeviceX::ScheduleFrameEventX", 74, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::WaitFrameEventX(
    D3D12XBOX_FRAME_EVENT Event, uint32_t TimeoutMilliseconds,
    void* pReserved, uint32_t Flags,
    D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) {
    (void)Event;
    (void)TimeoutMilliseconds;
    (void)pReserved;
    (void)Flags;
    const uint64_t token = next_frame_token_.fetch_add(1ull, std::memory_order_relaxed);
    if (pOutToken) {
        *pOutToken = token;
    }
    RecordDev("ID3D12DeviceX::WaitFrameEventX", 75, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreatePlacedResourceX(
    D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
    REFIID riid, void** ppvResource) {
    if (!pDesc || !ppvResource) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 76, E_INVALIDARG);
        return E_INVALIDARG;
    }
    *ppvResource = nullptr;
    D3D12_RESOURCE_DESC clean_desc = SanitizeXboxResourceDesc(*pDesc);
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    if (ShouldRewriteR9G9B9E5Uav(clean_desc)) {
        clean_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        did_r9g9b9e5_fallback = true;
        if (pOptimizedClearValue &&
            pOptimizedClearValue->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
            clean_clear = *pOptimizedClearValue;
            clean_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            clear_ptr = &clean_clear;
        }
    }

    // If VirtualAddress falls inside an attached XMem graphics allocation, place directly
    // into its backing desktop ID3D12Heap so GPU virtual addresses bind cleanly on desktop!
    HRESULT hr = E_FAIL;
    ID3D12Resource* host_res = nullptr;
    if (VirtualAddress != 0ull && xmem_allocation_count_.load(std::memory_order_acquire) > 0u) {
        ID3D12Heap* backing_heap = nullptr;
        uint64_t heap_offset = 0ull;
        uint64_t heap_size = 0ull;
        {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            for (const auto& rec : xmem_allocations_) {
                if (rec.heap && VirtualAddress >= rec.guest_va &&
                    VirtualAddress < rec.guest_va + rec.aligned_size) {
                    backing_heap = rec.heap;
                    backing_heap->AddRef();
                    heap_offset = VirtualAddress - rec.guest_va;
                    heap_size = rec.aligned_size;
                    break;
                }
            }
        }
        if (backing_heap) {
            const D3D12_RESOURCE_ALLOCATION_INFO alloc_info =
                host_dev_->GetResourceAllocationInfo(0u, 1u, &clean_desc);
            const uint64_t req_align = std::max<uint64_t>(
                alloc_info.Alignment ? alloc_info.Alignment : 65536ull, 65536ull);
            if (alloc_info.SizeInBytes > 0ull &&
                (heap_offset % req_align) == 0ull &&
                heap_offset + alloc_info.SizeInBytes <= heap_size) {
                hr = host_dev_->CreatePlacedResource(
                    backing_heap, heap_offset, &clean_desc, InitialState,
                    clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            }
            backing_heap->Release();
        }
    }

    // Fallback to committed default-heap resource if not attached or heap placement incompatible.
    D3D12_HEAP_PROPERTIES default_heap_props{};
    default_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(hr) || !host_res) {
        hr = host_dev_->CreateCommittedResource(
            &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }

    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 76, hr);
        return hr;
    }

    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    TrackBufferVaResource(host_res, clean_desc, VirtualAddress);
    {
        const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
        if (tile_mode != XG_TILE_MODE_LINEAR) {
            SetTrackedResourceTileMode(host_res, tile_mode);
        }
    }
    if (vulkan_routing_active_) {
        TrackVulkanResource(host_res, D3D12_HEAP_TYPE_DEFAULT, clean_desc);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 76, S_OK);
    }
    if (VirtualAddress != 0ull) {
        uint64_t mirror_bytes = clean_desc.Width;
        if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            UINT64 total_fp_bytes = 0ull;
            host_dev_->GetCopyableFootprints(
                &clean_desc, 0u, 1u, 0ull, nullptr, nullptr, nullptr, &total_fp_bytes);
            if (total_fp_bytes > 0ull && total_fp_bytes != UINT64_MAX) {
                mirror_bytes = total_fp_bytes;
            }
        }
        bool mirrored_by_title_va = false;
        if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            mirror_bytes > 0ull &&
            IsReadableGuestVaRange(VirtualAddress, mirror_bytes)) {
            openscarlett::TitleVaMappingDesc mdesc{};
            mdesc.title_va = VirtualAddress;
            mdesc.size_bytes = mirror_bytes;
            mdesc.kind = openscarlett::TitleVaResourceKind::Buffer;
            mdesc.existing_write_watch_cpu_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(VirtualAddress));
            mdesc.gpu_shadow_resource = host_res;
            mdesc.initial_state = InitialState;
            openscarlett::TitleVaRegionInfo reg_info{};
            if (title_va_mirror_.RegisterRegion(mdesc, host_dev_, reg_info).ok()) {
                std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
                mirrored_res_to_va_[host_res] = VirtualAddress;
                mirrored_va_count_.store(
                    static_cast<uint32_t>(mirrored_res_to_va_.size()), std::memory_order_release);
                mirrored_by_title_va = true;
            }
        }
        AutoRegisterUnifiedResource(host_res, VirtualAddress, clean_desc, InitialState, mirrored_by_title_va);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, &clean_desc, &default_heap_props, D3D12_HEAP_FLAG_NONE, VirtualAddress);
    host_res->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 76, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppvResource);
    proxy->Release();
    RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 76, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::SuspendX(uint32_t Flags) {
    (void)Flags;
    suspended_.store(true, std::memory_order_release);
    RecordDev("ID3D12DeviceX::SuspendX", 77, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ResumeX() {
    suspended_.store(false, std::memory_order_release);
    RecordDev("ID3D12DeviceX::ResumeX", 78, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) {
    if (!pOutInfo) {
        RecordDev("ID3D12DeviceX::GetGpuHardwareInfoX", 79, E_POINTER);
        return E_POINTER;
    }
    std::memset(pOutInfo, 0, sizeof(*pOutInfo));
    pOutInfo->VendorId = adapter_info_.vendor_id;
    pOutInfo->DeviceId = adapter_info_.device_id;
    pOutInfo->DedicatedVideoMemoryBytes = adapter_info_.dedicated_video_memory_bytes;
    pOutInfo->MeshShaderTier = adapter_info_.mesh_shader_tier;
    pOutInfo->RaytracingTier = adapter_info_.raytracing_tier;
    pOutInfo->HighestShaderModel = adapter_info_.highest_shader_model;
    pOutInfo->WaveLaneCountMin = adapter_info_.wave_lane_min;
    pOutInfo->WaveLaneCountMax = adapter_info_.wave_lane_max;
    pOutInfo->IsWarp = adapter_info_.is_warp ? 1u : 0u;
    const size_t copy_len =
        (adapter_info_.description.size() < sizeof(pOutInfo->AdapterName) - 1u)
            ? adapter_info_.description.size()
            : (sizeof(pOutInfo->AdapterName) - 1u);
    if (copy_len > 0) {
        std::memcpy(pOutInfo->AdapterName, adapter_info_.description.data(), copy_len);
    }
    pOutInfo->AdapterName[copy_len] = '\0';
    RecordDev("ID3D12DeviceX::GetGpuHardwareInfoX", 79, S_OK);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Title Unified-Memory Texture Remap Tracking (ivall/xcl fd74662/bdae5a4)
// ---------------------------------------------------------------------------
void DeviceX::NotifyResourceStateTransition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES new_state) noexcept {
    resource = UnwrapResource(resource);
    if (!resource) return;
    if (HasRegisteredUnifiedTextures()) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = unified_textures_.find(resource);
        if (it != unified_textures_.end()) {
            it->second.current_state = new_state;
        }
    }
    if (HasMirroredTitleVaRegions()) {
        uint64_t va = 0ull;
        if (LookupMirroredTitleVaForResource(resource, va) && va != 0ull) {
            (void)title_va_mirror_.NotifyGpuResourceState(va, new_state);
        }
    }
}

void DeviceX::AutoRegisterUnifiedResource(
    ID3D12Resource* resource,
    uint64_t guest_va,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initial_state,
    bool mirrored_by_title_va) {
    if (!resource || guest_va == 0ull) return;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }

    uint64_t linear_size = 0ull;
    uint32_t row_pitch = 0u;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        linear_size = desc.Width;
        row_pitch = static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
    } else {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT num_rows = 0u;
        UINT64 row_bytes = 0ull;
        UINT64 total_bytes = 0ull;
        host_dev_->GetCopyableFootprints(&desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
        row_pitch = static_cast<uint32_t>(row_bytes);
        const uint32_t slices = (desc.DepthOrArraySize > 0u) ? desc.DepthOrArraySize : 1u;
        linear_size = row_bytes * static_cast<uint64_t>(num_rows) * static_cast<uint64_t>(slices);
    }

    D3D12XBOX_UNIFIED_TEXTURE_DESC udesc{};
    udesc.Resource = resource;
    udesc.GuestVirtualAddress = guest_va;
    udesc.ByteSize = linear_size;
    udesc.RowPitchBytes = row_pitch;
    udesc.ResourceState = initial_state;
    RegisterUnifiedTexture(&udesc);
    if (mirrored_by_title_va) {
        ID3D12Resource* host_res = UnwrapResource(resource);
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = unified_textures_.find(host_res);
        if (it != unified_textures_.end()) {
            it->second.mirrored_by_title_va = true;
            // TitleVAMirror owns write-watch & CPU<->GPU coherency for this buffer; avoid duplicate upload.
            if (it->second.dirty) {
                it->second.dirty = false;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
            it->second.write_watch_enabled = false;
        }
    }
}

HRESULT DeviceX::EnsureUnifiedUploadObjectsLocked() {
    if (unified_alloc_ && unified_cmd_ && unified_fence_ && unified_fence_event_) {
        return S_OK;
    }
    if (!unified_alloc_) {
        HRESULT hr = host_dev_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&unified_alloc_));
        if (FAILED(hr)) return hr;
    }
    if (!unified_cmd_) {
        HRESULT hr = host_dev_->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, unified_alloc_, nullptr,
            IID_PPV_ARGS(&unified_cmd_));
        if (FAILED(hr)) return hr;
        unified_cmd_->Close();
    }
    if (!unified_fence_) {
        HRESULT hr = host_dev_->CreateFence(
            0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unified_fence_));
        if (FAILED(hr)) return hr;
    }
    if (!unified_fence_event_) {
        unified_fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!unified_fence_event_) return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT DeviceX::EnsureUnifiedUploadBufferLocked(uint64_t required_bytes) {
    if (required_bytes == 0ull) return S_OK;
    if (unified_upload_buf_ && unified_upload_capacity_ >= required_bytes) {
        return S_OK;
    }
    if (unified_upload_buf_) {
        unified_upload_buf_->Release();
        unified_upload_buf_ = nullptr;
        unified_upload_capacity_ = 0ull;
    }

    const uint64_t alloc_bytes = AlignUp64(std::max<uint64_t>(required_bytes, 65536ull), 4096ull);
    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = alloc_bytes;
    up_desc.Height = 1u;
    up_desc.DepthOrArraySize = 1u;
    up_desc.MipLevels = 1u;
    up_desc.Format = DXGI_FORMAT_UNKNOWN;
    up_desc.SampleDesc.Count = 1u;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = host_dev_->CreateCommittedResource(
        &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&unified_upload_buf_));
    if (SUCCEEDED(hr)) {
        unified_upload_capacity_ = alloc_bytes;
    }
    return hr;
}

void DeviceX::ReleaseUnifiedMemoryObjects() {
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto& [res, mapping] : unified_textures_) {
            if (mapping.resource) {
                mapping.resource->Release();
            }
        }
        unified_textures_.clear();
        address_heaps_.clear();
        for (auto& xmem : xmem_allocations_) {
            if (xmem.anchor_buffer) xmem.anchor_buffer->Release();
            if (xmem.heap) xmem.heap->Release();
        }
        xmem_allocations_.clear();
        xmem_allocation_count_.store(0u, std::memory_order_release);
        registered_unified_count_.store(0u, std::memory_order_release);
        dirty_unified_count_.store(0u, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        r9g9b9e5_fallback_resources_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(unified_upload_mutex_);
        if (unified_sync_direct_queue_) {
            unified_sync_direct_queue_->Release();
            unified_sync_direct_queue_ = nullptr;
        }
        if (unified_upload_buf_) { unified_upload_buf_->Release(); unified_upload_buf_ = nullptr; }
        if (unified_cmd_) { unified_cmd_->Release(); unified_cmd_ = nullptr; }
        if (unified_alloc_) { unified_alloc_->Release(); unified_alloc_ = nullptr; }
        if (unified_fence_) { unified_fence_->Release(); unified_fence_ = nullptr; }
        if (unified_fence_event_) { CloseHandle(unified_fence_event_); unified_fence_event_ = nullptr; }
        unified_upload_capacity_ = 0ull;
    }
}

HRESULT DeviceX::EnsureInternalDirectSyncQueueLocked() {
    if (unified_sync_direct_queue_) return S_OK;
    if (!host_dev_) return E_FAIL;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0u;
    return host_dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&unified_sync_direct_queue_));
}

bool DeviceX::RefreshDirtyUnifiedResources() noexcept {
    if (!HasRegisteredUnifiedTextures()) {
        return false;
    }
    bool any_dirty = HasDirtyUnifiedTextures();
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (auto& [res, m] : unified_textures_) {
        if (m.mirrored_by_title_va) continue;
        if (m.dirty) {
            any_dirty = true;
            continue;
        }
        if (!m.cpu_readable || m.guest_va == 0ull || m.byte_size == 0ull) {
            continue;
        }
        bool modified = false;
        if (m.write_watch_enabled) {
            void* page_buf[4]{};
            ULONG_PTR page_cnt = 4u;
            DWORD gran = 4096u;
            const SIZE_T watch_len = static_cast<SIZE_T>(AlignUp64(m.byte_size, 4096ull));
            const UINT rc = GetWriteWatch(
                0u /*peek only*/,
                reinterpret_cast<void*>(static_cast<uintptr_t>(m.guest_va)),
                watch_len,
                page_buf,
                &page_cnt,
                &gran);
            if (rc == 0u && page_cnt > 0u) {
                modified = true;
            }
        } else if (m.byte_size <= (64ull * 1024ull * 1024ull) &&
                   IsReadableGuestVaRange(m.guest_va, m.byte_size)) {
            const uint64_t cur_hash = HashBytesFnv1a64(
                reinterpret_cast<const void*>(static_cast<uintptr_t>(m.guest_va)),
                static_cast<size_t>(m.byte_size));
            if (!m.has_uploaded_once || cur_hash != m.last_uploaded_hash) {
                modified = true;
            }
        }
        if (modified) {
            m.dirty = true;
            ++m.page_generation;
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
            any_dirty = true;
        }
    }
    return any_dirty;
}

HRESULT DeviceX::SyncAllUnifiedResourcesBeforeSubmit(
    ID3D12CommandQueue* host_queue,
    D3D12_COMMAND_LIST_TYPE queue_type) {
    if (!host_queue) return E_INVALIDARG;

    const bool has_unified = RefreshDirtyUnifiedResources();
    const bool has_mirrored = HasMirroredTitleVaRegions();
    if (!has_unified && !has_mirrored) {
        return S_OK;
    }

    ID3D12CommandQueue* sync_queue = host_queue;
    D3D12_COMMAND_LIST_TYPE sync_type = queue_type;
    if (queue_type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> lock(unified_upload_mutex_);
        if (SUCCEEDED(EnsureInternalDirectSyncQueueLocked()) && unified_sync_direct_queue_) {
            sync_queue = unified_sync_direct_queue_;
            sync_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        } else {
            return S_OK;
        }
    }

    HRESULT hr = S_OK;
    if (has_unified) {
        hr = FlushDirtyUnifiedTextures(sync_queue, nullptr);
    }
    if (has_mirrored) {
        const HRESULT mhr = FlushMirroredTitleVaPagesOnQueue(sync_queue, sync_type, nullptr);
        if (SUCCEEDED(hr)) hr = mhr;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::RegisterUnifiedTexture(
    const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) {
    if (!desc || !desc->Resource || desc->GuestVirtualAddress == 0ull) {
        RecordDev("ID3D12DeviceX::RegisterUnifiedTexture", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    ID3D12Resource* host_res = UnwrapResource(desc->Resource);
    const D3D12_RESOURCE_DESC rdesc = host_res->GetDesc();
    uint64_t byte_size = desc->ByteSize;
    uint32_t row_pitch = desc->RowPitchBytes;
    if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (byte_size == 0ull) byte_size = rdesc.Width;
        if (row_pitch == 0u) row_pitch = static_cast<uint32_t>(std::min<uint64_t>(byte_size, UINT32_MAX));
    } else if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT num_rows = 0u;
        UINT64 row_bytes = 0ull;
        UINT64 total_bytes = 0ull;
        host_dev_->GetCopyableFootprints(&rdesc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
        if (row_pitch == 0u) row_pitch = static_cast<uint32_t>(row_bytes);
        if (byte_size == 0ull) {
            const uint32_t slices = (rdesc.DepthOrArraySize > 0u) ? rdesc.DepthOrArraySize : 1u;
            byte_size = static_cast<uint64_t>(row_pitch) *
                        static_cast<uint64_t>(num_rows) *
                        static_cast<uint64_t>(slices);
        }
    }

    const bool readable = IsReadableGuestVaRange(desc->GuestVirtualAddress, byte_size);
    bool write_watch = false;
    if (readable && byte_size > 0ull) {
        void* probe_page = nullptr;
        ULONG_PTR probe_cnt = 1u;
        DWORD probe_gran = 4096u;
        const UINT rc = GetWriteWatch(
            0u,
            reinterpret_cast<void*>(static_cast<uintptr_t>(desc->GuestVirtualAddress)),
            static_cast<SIZE_T>(AlignUp64(byte_size, 4096ull)),
            &probe_page,
            &probe_cnt,
            &probe_gran);
        write_watch = (rc == 0u);
    }

    std::lock_guard<std::mutex> lock(unified_mutex_);
    auto it = unified_textures_.find(host_res);
    if (it != unified_textures_.end()) {
        if (it->second.dirty) {
            dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
        }
        it->second.guest_va = desc->GuestVirtualAddress;
        it->second.byte_size = byte_size;
        it->second.row_pitch_bytes = row_pitch;
        it->second.current_state = desc->ResourceState;
        it->second.cpu_readable = readable;
        it->second.write_watch_enabled = write_watch;
        it->second.has_uploaded_once = false;
        it->second.dirty = readable;
        ++it->second.page_generation;
        if (it->second.dirty) {
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    } else {
        UnifiedTextureMapping m{};
        m.resource = host_res;
        m.resource->AddRef();
        m.guest_va = desc->GuestVirtualAddress;
        m.byte_size = byte_size;
        m.width = static_cast<uint32_t>(rdesc.Width);
        m.height = rdesc.Height;
        m.depth_or_array_size = (rdesc.DepthOrArraySize > 0u) ? rdesc.DepthOrArraySize : 1u;
        m.mip_levels = (rdesc.MipLevels > 0u) ? rdesc.MipLevels : 1u;
        m.format = rdesc.Format;
        m.dimension = rdesc.Dimension;
        m.current_state = desc->ResourceState;
        m.row_pitch_bytes = row_pitch;
        m.page_generation = 1ull;
        m.uploaded_generation = 0ull;
        m.cpu_readable = readable;
        m.write_watch_enabled = write_watch;
        m.has_uploaded_once = false;
        m.dirty = readable;
        unified_textures_.emplace(host_res, m);
        registered_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        if (m.dirty) {
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    }

    RecordDev("ID3D12DeviceX::RegisterUnifiedTexture", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::UnregisterUnifiedTexture(ID3D12Resource* resource) {
    resource = UnwrapResource(resource);
    if (!resource) {
        RecordDev("ID3D12DeviceX::UnregisterUnifiedTexture", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    ID3D12Resource* to_release = nullptr;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = unified_textures_.find(resource);
        if (it != unified_textures_.end()) {
            if (it->second.dirty) {
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
            to_release = it->second.resource;
            unified_textures_.erase(it);
            registered_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
        }
    }
    if (to_release) {
        to_release->Release();
    }
    RecordDev("ID3D12DeviceX::UnregisterUnifiedTexture", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::NotifyVirtualRemap(
    uint64_t guest_va_base,
    uint64_t byte_size,
    uint64_t new_backing_va,
    uint32_t* out_flagged_count) {
    if (out_flagged_count) *out_flagged_count = 0u;
    if (guest_va_base == 0ull || byte_size == 0ull) {
        RecordDev("ID3D12DeviceX::NotifyVirtualRemap", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    const uint64_t range_end =
        (guest_va_base > UINT64_MAX - byte_size) ? UINT64_MAX : (guest_va_base + byte_size);

    uint32_t flagged = 0u;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        ++total_remap_notifications_;
        for (auto& [res, m] : unified_textures_) {
            const uint64_t m_end =
                (m.guest_va > UINT64_MAX - m.byte_size) ? UINT64_MAX : (m.guest_va + m.byte_size);
            const bool overlaps = (m.guest_va < range_end) && (guest_va_base < m_end);
            if (!overlaps) continue;

            if (new_backing_va != 0ull) {
                const uint64_t rel_offset =
                    (m.guest_va >= guest_va_base) ? (m.guest_va - guest_va_base) : 0ull;
                m.guest_va = new_backing_va + rel_offset;
            }
            m.cpu_readable = IsReadableGuestVaRange(m.guest_va, m.byte_size);
            ++m.page_generation;
            if (!m.dirty && m.cpu_readable) {
                m.dirty = true;
                dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
            }
            ++flagged;
        }
    }

    if (out_flagged_count) *out_flagged_count = flagged;
    RecordDev("ID3D12DeviceX::NotifyVirtualRemap", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::FlushDirtyUnifiedTextures(
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count) {
    if (out_flushed_count) *out_flushed_count = 0u;
    if (!queue) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    (void)RefreshDirtyUnifiedResources();
    if (!HasDirtyUnifiedTextures()) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    ID3D12CommandQueue* host_queue = CommandQueueX::UnwrapHostQueue(queue);
    if (!host_queue) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    struct PendingUpload {
        UnifiedTextureMapping mapping;
        D3D12_RESOURCE_DESC res_desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> num_rows;
        std::vector<UINT64> row_sizes;
        uint64_t buffer_upload_offset = 0ull;
        uint64_t copy_bytes = 0ull;
        uint64_t snapshot_hash = 0ull;
        bool skip_gpu_copy_native_cpu_heap = false;
    };

    std::vector<PendingUpload> pending;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto& [res, m] : unified_textures_) {
            if (!m.dirty || !m.cpu_readable || !m.resource || m.mirrored_by_title_va) continue;
            PendingUpload item{};
            item.mapping = m;
            item.mapping.resource->AddRef();
            item.res_desc = m.resource->GetDesc();
            D3D12_HEAP_PROPERTIES hp{};
            D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
            if (SUCCEEDED(m.resource->GetHeapProperties(&hp, &hf))) {
                // Buffers on UPLOAD/READBACK or OpenExistingHeapFromAddress CUSTOM CPU-accessible L0 heaps
                // cannot be transitioned to COPY_DEST or already share CPU pages directly.
                if (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK ||
                    (hp.Type == D3D12_HEAP_TYPE_CUSTOM &&
                     item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                     (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
                      hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE))) {
                    item.skip_gpu_copy_native_cpu_heap = true;
                }
            }
            pending.push_back(std::move(item));
        }
    }

    if (pending.empty()) {
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    // Compute staging buffer layout (512-byte aligned per subresource).
    uint64_t staging_cursor = 0ull;
    for (auto& item : pending) {
        if (item.skip_gpu_copy_native_cpu_heap) {
            item.copy_bytes = item.mapping.byte_size;
            continue;
        }
        staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            item.buffer_upload_offset = staging_cursor;
            item.copy_bytes = std::min<uint64_t>(item.mapping.byte_size, item.res_desc.Width);
            staging_cursor += item.copy_bytes;
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            const uint32_t slices =
                (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                    ? 1u
                    : ((item.res_desc.DepthOrArraySize > 0u) ? item.res_desc.DepthOrArraySize : 1u);
            const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
            item.footprints.resize(slices);
            item.num_rows.resize(slices, 0u);
            item.row_sizes.resize(slices, 0ull);
            uint64_t total_copy = 0ull;
            for (uint32_t s = 0u; s < slices; ++s) {
                staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                UINT64 sub_total = 0ull;
                const UINT sub_idx = s * mips;  // mip 0 of each slice
                host_dev_->GetCopyableFootprints(
                    &item.res_desc, sub_idx, 1u, staging_cursor,
                    &item.footprints[s], &item.num_rows[s], &item.row_sizes[s], &sub_total);
                staging_cursor += sub_total;
                total_copy += item.row_sizes[s] * static_cast<uint64_t>(item.num_rows[s]);
            }
            item.copy_bytes = total_copy;
        }
    }

    auto release_pending = [&]() {
        for (auto& item : pending) {
            if (item.mapping.resource) {
                item.mapping.resource->Release();
                item.mapping.resource = nullptr;
            }
        }
    };

    std::lock_guard<std::mutex> upload_lock(unified_upload_mutex_);
    HRESULT hr = S_OK;
    if (staging_cursor > 0ull) {
        hr = EnsureUnifiedUploadObjectsLocked();
        if (SUCCEEDED(hr)) {
            hr = EnsureUnifiedUploadBufferLocked(staging_cursor);
        }
        if (FAILED(hr)) {
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
            return hr;
        }
    }

    uint8_t* dst_base = nullptr;
    if (staging_cursor > 0ull) {
        void* mapped_ptr = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        hr = unified_upload_buf_->Map(0u, &read_none, &mapped_ptr);
        if (FAILED(hr) || !mapped_ptr) {
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, FAILED(hr) ? hr : E_FAIL);
            return FAILED(hr) ? hr : E_FAIL;
        }
        dst_base = static_cast<uint8_t*>(mapped_ptr);
    }

    std::vector<size_t> uploaded_indices;
    std::vector<size_t> gpu_copy_indices;
    uploaded_indices.reserve(pending.size());
    gpu_copy_indices.reserve(pending.size());

    for (size_t idx = 0; idx < pending.size(); ++idx) {
        auto& item = pending[idx];
        if (item.copy_bytes == 0ull ||
            !IsReadableGuestVaRange(item.mapping.guest_va, item.mapping.byte_size)) {
            continue;
        }
        if (item.mapping.write_watch_enabled) {
            // Reset write-watch right BEFORE reading CPU bytes so any subsequent CPU mutation is caught.
            (void)ResetWriteWatch(
                reinterpret_cast<void*>(static_cast<uintptr_t>(item.mapping.guest_va)),
                static_cast<SIZE_T>(AlignUp64(item.mapping.byte_size, 4096ull)));
        }
        const auto* src_base =
            reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(item.mapping.guest_va));
        if (item.mapping.byte_size <= (64ull * 1024ull * 1024ull)) {
            item.snapshot_hash = HashBytesFnv1a64(src_base, static_cast<size_t>(item.mapping.byte_size));
        }

        if (item.skip_gpu_copy_native_cpu_heap) {
            uploaded_indices.push_back(idx);
            continue;
        }
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            std::memcpy(dst_base + item.buffer_upload_offset, src_base,
                        static_cast<size_t>(item.copy_bytes));
            uploaded_indices.push_back(idx);
            gpu_copy_indices.push_back(idx);
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            const size_t slices = item.footprints.size();
            uint64_t src_slice_offset = 0ull;
            for (size_t s = 0; s < slices; ++s) {
                const uint64_t row_bytes = item.row_sizes[s];
                const uint64_t src_row_pitch =
                    (item.mapping.row_pitch_bytes >= row_bytes)
                        ? static_cast<uint64_t>(item.mapping.row_pitch_bytes)
                        : row_bytes;
                const uint64_t dst_row_pitch = item.footprints[s].Footprint.RowPitch;
                uint8_t* dst_slice = dst_base + item.footprints[s].Offset;
                const UINT depth =
                    (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                     item.footprints[s].Footprint.Depth > 0u)
                        ? item.footprints[s].Footprint.Depth
                        : 1u;
                for (UINT z = 0u; z < depth; ++z) {
                    for (UINT y = 0u; y < item.num_rows[s]; ++y) {
                        const uint64_t row_idx = static_cast<uint64_t>(z) * item.num_rows[s] + y;
                        const uint64_t src_off = src_slice_offset + row_idx * src_row_pitch;
                        if (src_off + row_bytes <= item.mapping.byte_size) {
                            std::memcpy(
                                dst_slice + static_cast<size_t>(row_idx) * static_cast<size_t>(dst_row_pitch),
                                src_base + src_off,
                                static_cast<size_t>(row_bytes));
                        }
                    }
                }
                src_slice_offset += src_row_pitch * static_cast<uint64_t>(item.num_rows[s]) * depth;
            }
            uploaded_indices.push_back(idx);
            gpu_copy_indices.push_back(idx);
        }
    }

    if (staging_cursor > 0ull) {
        D3D12_RANGE written_range{0u, static_cast<SIZE_T>(staging_cursor)};
        unified_upload_buf_->Unmap(0u, &written_range);
    }

    if (uploaded_indices.empty()) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    uint64_t bytes_uploaded = 0ull;
    if (!gpu_copy_indices.empty()) {
        hr = unified_alloc_->Reset();
        if (SUCCEEDED(hr)) {
            hr = unified_cmd_->Reset(unified_alloc_, nullptr);
        }
        if (FAILED(hr)) {
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
            return hr;
        }

        for (size_t idx : gpu_copy_indices) {
            const auto& item = pending[idx];
            const D3D12_RESOURCE_STATES st = item.mapping.current_state;
            if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = item.mapping.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = st;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                unified_cmd_->ResourceBarrier(1u, &b);
            }

            if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                unified_cmd_->CopyBufferRegion(
                    item.mapping.resource, 0ull,
                    unified_upload_buf_, item.buffer_upload_offset,
                    item.copy_bytes);
            } else {
                const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
                for (size_t s = 0; s < item.footprints.size(); ++s) {
                    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                    dst_loc.pResource = item.mapping.resource;
                    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst_loc.SubresourceIndex = static_cast<UINT>(s) * mips;

                    D3D12_TEXTURE_COPY_LOCATION src_loc{};
                    src_loc.pResource = unified_upload_buf_;
                    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src_loc.PlacedFootprint = item.footprints[s];

                    unified_cmd_->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
                }
            }

            if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = item.mapping.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = st;
                unified_cmd_->ResourceBarrier(1u, &b);
            }
            bytes_uploaded += item.copy_bytes;
        }

        hr = unified_cmd_->Close();
        if (FAILED(hr)) {
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
            return hr;
        }

        ID3D12CommandList* cmd_lists[] = {unified_cmd_};
        host_queue->ExecuteCommandLists(1u, cmd_lists);

        const uint64_t fence_target = ++unified_fence_val_;
        hr = host_queue->Signal(unified_fence_, fence_target);
        if (SUCCEEDED(hr) && unified_fence_->GetCompletedValue() < fence_target) {
            hr = unified_fence_->SetEventOnCompletion(fence_target, unified_fence_event_);
            if (SUCCEEDED(hr)) {
                const DWORD wr = WaitForSingleObject(unified_fence_event_, 15000u);
                if (wr != WAIT_OBJECT_0) {
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
            }
        }
    }

    uint32_t flushed_count = 0u;
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (size_t idx : uploaded_indices) {
            const auto& item = pending[idx];
            auto it = unified_textures_.find(item.mapping.resource);
            if (it != unified_textures_.end() &&
                it->second.dirty &&
                it->second.page_generation == item.mapping.page_generation) {
                it->second.dirty = false;
                it->second.uploaded_generation = item.mapping.page_generation;
                it->second.has_uploaded_once = true;
                it->second.last_uploaded_hash = item.snapshot_hash;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                ++flushed_count;
            }
        }
        ++total_unified_flushes_;
        total_unified_bytes_uploaded_ += bytes_uploaded;
    }

    release_pending();
    if (out_flushed_count) *out_flushed_count = flushed_count;
    RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetUnifiedTextureStats(
    D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) {
    if (!out_stats) {
        RecordDev("ID3D12DeviceX::GetUnifiedTextureStats", -1, E_POINTER);
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    out_stats->RegisteredCount = static_cast<uint32_t>(unified_textures_.size());
    out_stats->DirtyCount = dirty_unified_count_.load(std::memory_order_acquire);
    out_stats->TotalRemapNotifications = total_remap_notifications_;
    out_stats->TotalFlushes = total_unified_flushes_;
    out_stats->TotalBytesUploaded = total_unified_bytes_uploaded_;
    RecordDev("ID3D12DeviceX::GetUnifiedTextureStats", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::AttachXMemAllocation(
    uint64_t guest_va,
    uint64_t byte_size,
    uint64_t* out_desktop_gpu_va) {
    if (out_desktop_gpu_va) *out_desktop_gpu_va = 0ull;
    if (guest_va == 0ull || byte_size == 0ull || !host_dev_) {
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const uint64_t aligned_size = AlignUp64(byte_size, 65536ull);

    // Check if this guest_va is already attached with sufficient size.
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (const auto& rec : xmem_allocations_) {
            if (rec.guest_va == guest_va && rec.aligned_size >= aligned_size) {
                if (out_desktop_gpu_va) *out_desktop_gpu_va = rec.desktop_gpu_va;
                RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, S_OK);
                return S_OK;
            }
        }
    }

    // Create a desktop D3D12 default heap and anchor buffer so every attached xmem graphics
    // allocation binds immediately to a real, non-zero desktop GPU virtual address.
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = aligned_size;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;

    ID3D12Heap* heap = nullptr;
    HRESULT hr = host_dev_->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr) || !heap) {
        // Fallback for ResourceHeapTier 1 devices where D3D12_HEAP_FLAG_NONE is disallowed.
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        hr = host_dev_->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
        if (FAILED(hr) || !heap) {
            RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, hr);
            return hr;
        }
    }

    D3D12_RESOURCE_DESC anchor_desc{};
    anchor_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    anchor_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    anchor_desc.Width = aligned_size;
    anchor_desc.Height = 1u;
    anchor_desc.DepthOrArraySize = 1u;
    anchor_desc.MipLevels = 1u;
    anchor_desc.Format = DXGI_FORMAT_UNKNOWN;
    anchor_desc.SampleDesc.Count = 1u;
    anchor_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    anchor_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* anchor_buf = nullptr;
    hr = host_dev_->CreatePlacedResource(
        heap, 0ull, &anchor_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&anchor_buf));
    if (FAILED(hr) || !anchor_buf) {
        heap->Release();
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, hr);
        return hr;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS desktop_va = anchor_buf->GetGPUVirtualAddress();
    if (desktop_va == 0ull) {
        anchor_buf->Release();
        heap->Release();
        RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, E_FAIL);
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        // Replace any previous record at the exact same guest_va.
        for (auto it = xmem_allocations_.begin(); it != xmem_allocations_.end(); ) {
            if (it->guest_va == guest_va) {
                if (it->anchor_buffer) it->anchor_buffer->Release();
                if (it->heap) it->heap->Release();
                it = xmem_allocations_.erase(it);
            } else {
                ++it;
            }
        }
        XMemAllocationRecord rec{};
        rec.guest_va = guest_va;
        rec.byte_size = byte_size;
        rec.aligned_size = aligned_size;
        rec.heap = heap;
        rec.anchor_buffer = anchor_buf;
        rec.desktop_gpu_va = desktop_va;
        xmem_allocations_.push_back(rec);
        xmem_allocation_count_.store(
            static_cast<uint32_t>(xmem_allocations_.size()), std::memory_order_release);
    }

    if (out_desktop_gpu_va) *out_desktop_gpu_va = desktop_va;
    RecordDev("ID3D12DeviceX::AttachXMemAllocation", -1, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::DetachXMemAllocation(uint64_t guest_va) {
    if (guest_va == 0ull) {
        RecordDev("ID3D12DeviceX::DetachXMemAllocation", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    ID3D12Heap* old_heap = nullptr;
    ID3D12Resource* old_anchor = nullptr;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto it = xmem_allocations_.begin(); it != xmem_allocations_.end(); ++it) {
            if (it->guest_va == guest_va) {
                old_heap = it->heap;
                old_anchor = it->anchor_buffer;
                xmem_allocations_.erase(it);
                xmem_allocation_count_.store(
                    static_cast<uint32_t>(xmem_allocations_.size()), std::memory_order_release);
                found = true;
                break;
            }
        }
    }
    if (old_anchor) old_anchor->Release();
    if (old_heap) old_heap->Release();
    const HRESULT hr = found ? S_OK : S_FALSE;
    RecordDev("ID3D12DeviceX::DetachXMemAllocation", -1, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::ResolveXMemGpuVa(
    uint64_t guest_va,
    uint64_t* out_desktop_gpu_va) {
    if (!out_desktop_gpu_va) {
        RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, E_POINTER);
        return E_POINTER;
    }
    *out_desktop_gpu_va = 0ull;
    if (guest_va == 0ull) {
        RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (const auto& rec : xmem_allocations_) {
        if (guest_va >= rec.guest_va && guest_va < rec.guest_va + rec.aligned_size) {
            *out_desktop_gpu_va = rec.desktop_gpu_va + (guest_va - rec.guest_va);
            RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, S_OK);
            return S_OK;
        }
    }
    RecordDev("ID3D12DeviceX::ResolveXMemGpuVa", -1, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT STDMETHODCALLTYPE DeviceX::GetR9G9B9E5UavFallbackActive(int* out_active) {
    if (!out_active) {
        RecordDev("ID3D12DeviceX::GetR9G9B9E5UavFallbackActive", -1, E_POINTER);
        return E_POINTER;
    }
    *out_active = IsR9G9B9E5UavFallbackActive() ? 1 : 0;
    RecordDev("ID3D12DeviceX::GetR9G9B9E5UavFallbackActive", -1, S_OK);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Sprint 12 Live AAA GPU Island Helpers & IOpenScarlettAaaRuntime
// ---------------------------------------------------------------------------
void DeviceX::TrackBufferVaResource(
    ID3D12Resource* host_res,
    const D3D12_RESOURCE_DESC& desc,
    uint64_t guest_va) {
    if (!host_res || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    const D3D12_GPU_VIRTUAL_ADDRESS gpu_va = host_res->GetGPUVirtualAddress();
    if (gpu_va == 0ull && guest_va == 0ull) return;
    std::lock_guard<std::mutex> lock(buffer_va_mutex_);
    buffer_va_map_[host_res] = TrackedBufferVa{gpu_va, desc.Width, guest_va};
}

ID3D12Resource* DeviceX::ResolveBufferResourceByGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes) const {
    if (out_offset_bytes) *out_offset_bytes = 0ull;
    if (va == 0ull) return nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS translated = TranslateGuestGpuVa(va);
    std::lock_guard<std::mutex> lock(buffer_va_mutex_);
    for (const auto& [res, entry] : buffer_va_map_) {
        if (entry.gpu_va != 0ull && translated >= entry.gpu_va &&
            translated < entry.gpu_va + entry.size_bytes) {
            if (out_offset_bytes) *out_offset_bytes = translated - entry.gpu_va;
            return res;
        }
        if (entry.guest_va != 0ull && va >= entry.guest_va &&
            va < entry.guest_va + entry.size_bytes) {
            if (out_offset_bytes) *out_offset_bytes = va - entry.guest_va;
            return res;
        }
    }
    return nullptr;
}

bool DeviceX::LookupMirroredTitleVaForResource(
    ID3D12Resource* host_resource, uint64_t& out_title_va) const noexcept {
    out_title_va = 0ull;
    host_resource = UnwrapResource(host_resource);
    if (!host_resource) return false;
    std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
    auto it = mirrored_res_to_va_.find(host_resource);
    if (it == mirrored_res_to_va_.end()) return false;
    out_title_va = it->second;
    return true;
}

HRESULT DeviceX::FlushMirroredTitleVaPagesOnQueue(
    ID3D12CommandQueue* host_queue,
    D3D12_COMMAND_LIST_TYPE queue_type,
    uint64_t* out_bytes_uploaded) {
    if (out_bytes_uploaded) *out_bytes_uploaded = 0ull;
    if (!host_queue || !HasMirroredTitleVaRegions() ||
        queue_type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return S_OK;
    }

    std::vector<uint64_t> candidate_vas;
    {
        std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
        candidate_vas.reserve(mirrored_res_to_va_.size());
        for (const auto& [res, va] : mirrored_res_to_va_) {
            (void)res;
            if (va != 0ull) candidate_vas.push_back(va);
        }
    }
    if (candidate_vas.empty()) return S_OK;

    // Fast dirty probe before touching any GPU command allocator.
    std::vector<uint64_t> dirty_vas;
    std::vector<openscarlett::DirtyPageRange> probe_ranges;
    for (uint64_t va : candidate_vas) {
        probe_ranges.clear();
        if (title_va_mirror_.QueryDirtyPageRanges(va, /*reset_watch=*/false, probe_ranges).ok() &&
            !probe_ranges.empty()) {
            dirty_vas.push_back(va);
        }
    }
    if (dirty_vas.empty()) return S_OK;

    std::lock_guard<std::mutex> upload_lock(unified_upload_mutex_);
    HRESULT hr = EnsureUnifiedUploadObjectsLocked();
    if (FAILED(hr)) return hr;
    hr = unified_alloc_->Reset();
    if (SUCCEEDED(hr)) {
        hr = unified_cmd_->Reset(unified_alloc_, nullptr);
    }
    if (FAILED(hr)) return hr;

    uint64_t total_uploaded = 0ull;
    uint64_t regions_flushed = 0ull;
    for (uint64_t va : dirty_vas) {
        openscarlett::TitleVaRegionInfo rinfo{};
        if (!title_va_mirror_.LookupRegion(va, rinfo)) continue;
        openscarlett::FlushStats fstats{};
        if (title_va_mirror_.FlushDirtyPagesToGpu(
                unified_cmd_, va, &fstats, rinfo.current_state).ok()) {
            total_uploaded += fstats.bytes_uploaded;
            regions_flushed += fstats.regions_flushed;
        }
    }

    hr = unified_cmd_->Close();
    if (FAILED(hr)) return hr;

    if (regions_flushed > 0ull) {
        ID3D12CommandList* lists[] = {unified_cmd_};
        host_queue->ExecuteCommandLists(1u, lists);
        const uint64_t target = ++unified_fence_val_;
        hr = host_queue->Signal(unified_fence_, target);
        if (SUCCEEDED(hr) && unified_fence_->GetCompletedValue() < target) {
            hr = unified_fence_->SetEventOnCompletion(target, unified_fence_event_);
            if (SUCCEEDED(hr)) {
                const DWORD wr = WaitForSingleObject(unified_fence_event_, 15000u);
                if (wr != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
        if (SUCCEEDED(hr)) {
            va_queue_flushes_.fetch_add(1ull, std::memory_order_relaxed);
        }
    }
    if (out_bytes_uploaded) *out_bytes_uploaded = total_uploaded;
    return hr;
}

XG_TILE_MODE DeviceX::GetTrackedResourceTileMode(ID3D12Resource* host_resource) const noexcept {
    host_resource = UnwrapResource(host_resource);
    if (!host_resource) return XG_TILE_MODE_LINEAR;
    std::lock_guard<std::mutex> lock(tile_mode_mutex_);
    auto it = resource_tile_modes_.find(host_resource);
    return (it != resource_tile_modes_.end()) ? it->second : XG_TILE_MODE_LINEAR;
}

void DeviceX::SetTrackedResourceTileMode(ID3D12Resource* host_resource, XG_TILE_MODE mode) {
    host_resource = UnwrapResource(host_resource);
    if (!host_resource) return;
    std::lock_guard<std::mutex> lock(tile_mode_mutex_);
    if (mode == XG_TILE_MODE_LINEAR) {
        resource_tile_modes_.erase(host_resource);
    } else {
        resource_tile_modes_[host_resource] = mode;
    }
}

bool DeviceX::EnsureXgGpuUntilerReady() {
    std::lock_guard<std::mutex> lock(xg_untiler_mutex_);
    if (xg_untiler_ && xg_untiler_->IsD3D12Ready()) return true;
    if (xg_untiler_attempted_) return false;
    xg_untiler_attempted_ = true;
    xg_untiler_ = std::make_unique<openscarlett::XgGpuUntiler>();
    const openscarlett::Status st = xg_untiler_->InitializeD3D12WithDevice(host_dev_);
    return st.ok() && xg_untiler_->IsD3D12Ready();
}

void DeviceX::TrackTransientGpuResource(ID3D12Resource* res) {
    if (!res) return;
    std::lock_guard<std::mutex> lock(transient_res_mutex_);
    transient_gpu_resources_.push_back(res);
}

void DeviceX::ReleaseAaaRuntimeObjects() {
    {
        std::lock_guard<std::mutex> lock(xg_untiler_mutex_);
        if (xg_untiler_) {
            xg_untiler_->Shutdown();
            xg_untiler_.reset();
        }
        xg_untiler_attempted_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(feedback_codec_mutex_);
        if (fb_decode_pso_) { fb_decode_pso_->Release(); fb_decode_pso_ = nullptr; }
        if (fb_encode_pso_) { fb_encode_pso_->Release(); fb_encode_pso_ = nullptr; }
        if (fb_codec_rs_) { fb_codec_rs_->Release(); fb_codec_rs_ = nullptr; }
    }
    {
        std::lock_guard<std::mutex> lock(transient_res_mutex_);
        for (ID3D12Resource* r : transient_gpu_resources_) {
            if (r) r->Release();
        }
        transient_gpu_resources_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
        mirrored_res_to_va_.clear();
        mirrored_va_count_.store(0u, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(buffer_va_mutex_);
        buffer_va_map_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(tile_mode_mutex_);
        resource_tile_modes_.clear();
    }
    query_accumulator_.Shutdown();
    sampler_feedback_mgr_.Shutdown();
    descriptor_bridge_.ShutdownD3D12();
    title_va_mirror_.Clear();
}

HRESULT DeviceX::RecordUntileUploadOnCommandList(
    ID3D12GraphicsCommandList* host_list,
    D3D12_COMMAND_LIST_TYPE list_type,
    const D3D12_TEXTURE_COPY_LOCATION& dst_loc,
    UINT dst_x,
    UINT dst_y,
    UINT dst_z,
    const D3D12_TEXTURE_COPY_LOCATION& src_loc,
    const D3D12_BOX* src_box,
    XG_TILE_MODE tile_mode) {
    if (!host_list || !dst_loc.pResource || !src_loc.pResource) return E_INVALIDARG;
    const D3D12_RESOURCE_DESC dst_desc = dst_loc.pResource->GetDesc();
    if (dst_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        dst_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        return E_INVALIDARG;
    }

    const uint32_t mips = std::max<uint32_t>(1u, dst_desc.MipLevels);
    const uint32_t mip_level = dst_loc.SubresourceIndex % mips;
    const uint32_t slice_index = dst_loc.SubresourceIndex / mips;

    openscarlett::XgGpuUntileParams params{};
    uint64_t linear_bytes = 0ull;
    uint64_t tiled_bytes = 0ull;
    openscarlett::Status pst = openscarlett::Status::Error(
        openscarlett::StatusCode::InvalidArgument, "Unsupported dimension");

    XG_TEXTURE2D_DESC xg2d{};
    XG_TEXTURE3D_DESC xg3d{};
    if (dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        xg2d.Width = static_cast<uint32_t>(dst_desc.Width);
        xg2d.Height = dst_desc.Height;
        xg2d.MipLevels = mips;
        xg2d.ArraySize = std::max<uint32_t>(1u, dst_desc.DepthOrArraySize);
        xg2d.Format = static_cast<uint32_t>(dst_desc.Format);
        xg2d.SampleCount = std::max<uint32_t>(1u, dst_desc.SampleDesc.Count);
        xg2d.TileMode = tile_mode;
        xg2d.BindFlags = XG_BIND_SHADER_RESOURCE;

        openscarlett::XgGpuSurface2DRequest req{};
        req.desc = xg2d;
        req.mip_level = mip_level;
        req.slice_index = slice_index;
        req.direction = openscarlett::XgGpuDirection::Untile;
        req.linear_row_pitch_bytes = src_loc.PlacedFootprint.Footprint.RowPitch;
        pst = openscarlett::BuildXgGpuUntileParams2D(req, params, &linear_bytes, &tiled_bytes);
    } else {
        xg3d.Width = static_cast<uint32_t>(dst_desc.Width);
        xg3d.Height = dst_desc.Height;
        xg3d.Depth = std::max<uint32_t>(1u, dst_desc.DepthOrArraySize);
        xg3d.MipLevels = mips;
        xg3d.Format = static_cast<uint32_t>(dst_desc.Format);
        xg3d.TileMode = tile_mode;
        xg3d.BindFlags = XG_BIND_SHADER_RESOURCE;

        openscarlett::XgGpuSurface3DRequest req{};
        req.desc = xg3d;
        req.mip_level = mip_level;
        req.direction = openscarlett::XgGpuDirection::Untile;
        req.linear_row_pitch_bytes = src_loc.PlacedFootprint.Footprint.RowPitch;
        pst = openscarlett::BuildXgGpuUntileParams3D(req, params, &linear_bytes, &tiled_bytes);
    }
    if (!pst.ok() || linear_bytes == 0ull) return E_FAIL;

    const bool can_gpu_dispatch =
        (list_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) &&
        EnsureXgGpuUntilerReady();

    if (can_gpu_dispatch && xg_untiler_) {
        // Allocate transient 256B CBV upload buffer + linear UAV output buffer.
        D3D12_HEAP_PROPERTIES up_hp{};
        up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbv_desc{};
        cbv_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbv_desc.Width = 256ull;
        cbv_desc.Height = 1u;
        cbv_desc.DepthOrArraySize = 1u;
        cbv_desc.MipLevels = 1u;
        cbv_desc.SampleDesc.Count = 1u;
        cbv_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* cbv_buf = nullptr;
        HRESULT hr = host_dev_->CreateCommittedResource(
            &up_hp, D3D12_HEAP_FLAG_NONE, &cbv_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&cbv_buf));
        if (FAILED(hr) || !cbv_buf) return hr;

        void* cbv_ptr = nullptr;
        D3D12_RANGE rnone{0u, 0u};
        if (SUCCEEDED(cbv_buf->Map(0u, &rnone, &cbv_ptr)) && cbv_ptr) {
            std::memset(cbv_ptr, 0, 256u);
            std::memcpy(cbv_ptr, &params, sizeof(params));
            D3D12_RANGE wfull{0u, sizeof(params)};
            cbv_buf->Unmap(0u, &wfull);
        }

        const uint64_t uav_bytes = AlignUp64(linear_bytes, 256ull);
        D3D12_HEAP_PROPERTIES def_hp{};
        def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC uav_desc = cbv_desc;
        uav_desc.Width = uav_bytes;
        uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ID3D12Resource* linear_uav = nullptr;
        hr = host_dev_->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &uav_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&linear_uav));
        if (FAILED(hr) || !linear_uav) {
            cbv_buf->Release();
            return hr;
        }

        const uint64_t src_va =
            src_loc.pResource->GetGPUVirtualAddress() + src_loc.PlacedFootprint.Offset;
        const openscarlett::Status dst_st = xg_untiler_->RecordDispatchD3D12(
            host_list, params,
            cbv_buf->GetGPUVirtualAddress(),
            src_va, 0ull,
            linear_uav->GetGPUVirtualAddress());
        if (!dst_st.ok()) {
            linear_uav->Release();
            cbv_buf->Release();
            return E_FAIL;
        }

        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = linear_uav;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = linear_uav;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        host_list->ResourceBarrier(2u, barriers);

        D3D12_TEXTURE_COPY_LOCATION linear_src = src_loc;
        linear_src.pResource = linear_uav;
        linear_src.PlacedFootprint.Offset = 0ull;
        host_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &linear_src, src_box);

        TrackTransientGpuResource(cbv_buf);
        TrackTransientGpuResource(linear_uav);
        xg_gpu_untile_dispatches_.fetch_add(1ull, std::memory_order_relaxed);
        return S_OK;
    }

    // Safe CPU untile fallback (e.g. COPY command list or minimal host without DXC).
    D3D12_HEAP_PROPERTIES src_hp{};
    D3D12_HEAP_FLAGS src_hf = D3D12_HEAP_FLAG_NONE;
    if (FAILED(src_loc.pResource->GetHeapProperties(&src_hp, &src_hf)) ||
        src_hp.Type != D3D12_HEAP_TYPE_UPLOAD) {
        return E_FAIL;
    }

    void* src_mapped = nullptr;
    D3D12_RANGE srange{
        static_cast<SIZE_T>(src_loc.PlacedFootprint.Offset),
        static_cast<SIZE_T>(src_loc.PlacedFootprint.Offset + tiled_bytes)};
    HRESULT hr = src_loc.pResource->Map(0u, &srange, &src_mapped);
    if (FAILED(hr) || !src_mapped) return FAILED(hr) ? hr : E_FAIL;

    D3D12_HEAP_PROPERTIES up_hp{};
    up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC lin_desc{};
    lin_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    lin_desc.Width = AlignUp64(linear_bytes, 256ull);
    lin_desc.Height = 1u;
    lin_desc.DepthOrArraySize = 1u;
    lin_desc.MipLevels = 1u;
    lin_desc.SampleDesc.Count = 1u;
    lin_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* lin_up = nullptr;
    hr = host_dev_->CreateCommittedResource(
        &up_hp, D3D12_HEAP_FLAG_NONE, &lin_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lin_up));
    if (FAILED(hr) || !lin_up) {
        D3D12_RANGE wnone{0u, 0u};
        src_loc.pResource->Unmap(0u, &wnone);
        return hr;
    }

    void* dst_mapped = nullptr;
    D3D12_RANGE rnone{0u, 0u};
    bool ok = false;
    if (SUCCEEDED(lin_up->Map(0u, &rnone, &dst_mapped)) && dst_mapped) {
        const auto* tiled_src =
            static_cast<const uint8_t*>(src_mapped) + src_loc.PlacedFootprint.Offset;
        if (dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            ok = openscarlett::xg::UntileSurface2D(
                xg2d, mip_level, slice_index, tiled_src, tiled_bytes,
                dst_mapped, src_loc.PlacedFootprint.Footprint.RowPitch);
        } else {
            ok = openscarlett::xg::UntileSurface3D(
                xg3d, mip_level, tiled_src, tiled_bytes, dst_mapped,
                src_loc.PlacedFootprint.Footprint.RowPitch, params.linear_slice_pitch);
        }
        D3D12_RANGE wfull{0u, static_cast<SIZE_T>(linear_bytes)};
        lin_up->Unmap(0u, &wfull);
    }
    D3D12_RANGE wnone{0u, 0u};
    src_loc.pResource->Unmap(0u, &wnone);

    if (!ok) {
        lin_up->Release();
        return E_FAIL;
    }

    D3D12_TEXTURE_COPY_LOCATION linear_src = src_loc;
    linear_src.pResource = lin_up;
    linear_src.PlacedFootprint.Offset = 0ull;
    host_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &linear_src, src_box);
    TrackTransientGpuResource(lin_up);
    xg_cpu_fallbacks_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

HRESULT DeviceX::EnsureFeedbackCodecPipelinesLocked() {
    if (fb_codec_rs_ && fb_decode_pso_ && fb_encode_pso_) return S_OK;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0u;
    params[0].Constants.RegisterSpace = 0u;
    params[0].Constants.Num32BitValues = 4u;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0u;
    params[1].Descriptor.RegisterSpace = 0u;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0u;
    params[2].Descriptor.RegisterSpace = 0u;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3u;
    rs_desc.pParameters = params;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    using PfnSerialize = HRESULT(WINAPI*)(
        const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto serialize_fn = reinterpret_cast<PfnSerialize>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    if (!serialize_fn) return E_FAIL;

    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = serialize_fn(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err_blob);
    if (err_blob) err_blob->Release();
    if (FAILED(hr) || !rs_blob) return FAILED(hr) ? hr : E_FAIL;

    hr = host_dev_->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&fb_codec_rs_));
    rs_blob->Release();
    if (FAILED(hr) || !fb_codec_rs_) return FAILED(hr) ? hr : E_FAIL;

    auto compile_cs = [&](const char* src, ID3D12PipelineState** out_pso) -> HRESULT {
        ID3DBlob* cs_blob = nullptr;
        ID3DBlob* cs_err = nullptr;
        HRESULT chr = D3DCompile(
            src, std::strlen(src), nullptr, nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &cs_blob, &cs_err);
        if (cs_err) cs_err->Release();
        if (FAILED(chr) || !cs_blob) return FAILED(chr) ? chr : E_FAIL;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = fb_codec_rs_;
        pd.CS.pShaderBytecode = cs_blob->GetBufferPointer();
        pd.CS.BytecodeLength = cs_blob->GetBufferSize();
        chr = host_dev_->CreateComputePipelineState(
            &pd, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(out_pso));
        cs_blob->Release();
        return chr;
    };

    hr = compile_cs(kFeedbackDecodeToR8Hlsl, &fb_decode_pso_);
    if (FAILED(hr)) return hr;
    hr = compile_cs(kFeedbackEncodeFromR8Hlsl, &fb_encode_pso_);
    return hr;
}

HRESULT DeviceX::RecordFeedbackFallbackDecode(
    ID3D12GraphicsCommandList* host_list,
    ID3D12Resource* host_feedback_src,
    ID3D12Resource* host_dst_tex,
    UINT dst_subresource,
    UINT dst_x,
    UINT dst_y) {
    if (!host_list || !host_feedback_src || !host_dst_tex) return E_INVALIDARG;
    SamplerFeedbackMapInfo info{};
    if (!sampler_feedback_mgr_.LookupFeedbackMap(host_feedback_src, info)) return E_INVALIDARG;

    {
        std::lock_guard<std::mutex> lock(feedback_codec_mutex_);
        const HRESULT hr = EnsureFeedbackCodecPipelinesLocked();
        if (FAILED(hr)) return hr;
    }

    const D3D12_RESOURCE_DESC dst_desc = host_dst_tex->GetDesc();
    const UINT sub = (dst_subresource == UINT_MAX) ? 0u : dst_subresource;
    const uint32_t slices =
        (dst_subresource == UINT_MAX) ? std::max<uint32_t>(1u, info.target_array_size) : 1u;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_bytes = 0ull;
    UINT64 total_bytes = 0ull;
    host_dev_->GetCopyableFootprints(&dst_desc, sub, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
    if (total_bytes == 0ull || total_bytes == UINT64_MAX) return E_FAIL;

    const uint64_t staging_bytes = AlignUp64(total_bytes, 256ull);
    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC st_desc{};
    st_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    st_desc.Width = staging_bytes;
    st_desc.Height = 1u;
    st_desc.DepthOrArraySize = 1u;
    st_desc.MipLevels = 1u;
    st_desc.SampleDesc.Count = 1u;
    st_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    st_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* staging_r8 = nullptr;
    HRESULT hr = host_dev_->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &st_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&staging_r8));
    if (FAILED(hr) || !staging_r8) return FAILED(hr) ? hr : E_FAIL;

    // Caller transitioned feedback_src to RESOLVE_SOURCE (remapped by CommandListX::ResourceBarrier
    // to NON_PIXEL_SHADER_RESOURCE for fallback buffers) and dst_tex to RESOLVE_DEST.
    D3D12_RESOURCE_BARRIER pre{};
    pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre.Transition.pResource = host_dst_tex;
    pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    host_list->ResourceBarrier(1u, &pre);

    for (uint32_t s = 0u; s < slices; ++s) {
        const uint32_t target_slice = (dst_subresource == UINT_MAX) ? s : sub;
        if (s > 0u) {
            D3D12_RESOURCE_BARRIER to_uav{};
            to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_uav.Transition.pResource = staging_r8;
            to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            host_list->ResourceBarrier(1u, &to_uav);
        }
        const uint32_t consts[4] = {
            info.map_tiles_x,
            info.map_tiles_y,
            fp.Footprint.RowPitch,
            target_slice,
        };
        host_list->SetComputeRootSignature(fb_codec_rs_);
        host_list->SetPipelineState(fb_decode_pso_);
        host_list->SetComputeRoot32BitConstants(0u, 4u, consts, 0u);
        host_list->SetComputeRootShaderResourceView(1u, host_feedback_src->GetGPUVirtualAddress());
        host_list->SetComputeRootUnorderedAccessView(2u, staging_r8->GetGPUVirtualAddress());
        const uint32_t row_dwords = (fp.Footprint.RowPitch + 3u) >> 2u;
        const uint32_t total_dwords = std::max<uint32_t>(1u, row_dwords * info.map_tiles_y);
        host_list->Dispatch((total_dwords + 63u) / 64u, 1u, 1u);

        D3D12_RESOURCE_BARRIER mid[2]{};
        mid[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        mid[0].UAV.pResource = staging_r8;
        mid[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        mid[1].Transition.pResource = staging_r8;
        mid[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        mid[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        mid[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        host_list->ResourceBarrier(2u, mid);

        D3D12_TEXTURE_COPY_LOCATION dloc{};
        dloc.pResource = host_dst_tex;
        dloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dloc.SubresourceIndex = target_slice;
        D3D12_TEXTURE_COPY_LOCATION sloc{};
        sloc.pResource = staging_r8;
        sloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sloc.PlacedFootprint = fp;
        sloc.PlacedFootprint.Offset = 0ull;
        host_list->CopyTextureRegion(&dloc, dst_x, dst_y, 0u, &sloc, nullptr);
    }

    D3D12_RESOURCE_BARRIER post{};
    post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post.Transition.pResource = host_dst_tex;
    post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    host_list->ResourceBarrier(1u, &post);

    TrackTransientGpuResource(staging_r8);
    sf_decode_intercepts_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

HRESULT DeviceX::RecordFeedbackFallbackEncode(
    ID3D12GraphicsCommandList* host_list,
    ID3D12Resource* host_src_tex,
    UINT src_subresource,
    ID3D12Resource* host_feedback_dst) {
    if (!host_list || !host_src_tex || !host_feedback_dst) return E_INVALIDARG;
    SamplerFeedbackMapInfo info{};
    if (!sampler_feedback_mgr_.LookupFeedbackMap(host_feedback_dst, info)) return E_INVALIDARG;

    {
        std::lock_guard<std::mutex> lock(feedback_codec_mutex_);
        const HRESULT hr = EnsureFeedbackCodecPipelinesLocked();
        if (FAILED(hr)) return hr;
    }

    const D3D12_RESOURCE_DESC src_desc = host_src_tex->GetDesc();
    const UINT sub = (src_subresource == UINT_MAX) ? 0u : src_subresource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_bytes = 0ull;
    UINT64 total_bytes = 0ull;
    host_dev_->GetCopyableFootprints(&src_desc, sub, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
    if (total_bytes == 0ull || total_bytes == UINT64_MAX) return E_FAIL;

    const uint64_t staging_bytes = AlignUp64(total_bytes, 256ull);
    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC st_desc{};
    st_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    st_desc.Width = staging_bytes;
    st_desc.Height = 1u;
    st_desc.DepthOrArraySize = 1u;
    st_desc.MipLevels = 1u;
    st_desc.SampleDesc.Count = 1u;
    st_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* staging_r8 = nullptr;
    HRESULT hr = host_dev_->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &st_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&staging_r8));
    if (FAILED(hr) || !staging_r8) return FAILED(hr) ? hr : E_FAIL;

    // Caller transitioned host_feedback_dst to RESOLVE_DEST (remapped to UNORDERED_ACCESS
    // for fallback buffers) and host_src_tex to RESOLVE_SOURCE.
    D3D12_RESOURCE_BARRIER pre{};
    pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre.Transition.pResource = host_src_tex;
    pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    host_list->ResourceBarrier(1u, &pre);

    D3D12_TEXTURE_COPY_LOCATION dloc{};
    dloc.pResource = staging_r8;
    dloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dloc.PlacedFootprint = fp;
    dloc.PlacedFootprint.Offset = 0ull;
    D3D12_TEXTURE_COPY_LOCATION sloc{};
    sloc.pResource = host_src_tex;
    sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sloc.SubresourceIndex = sub;
    host_list->CopyTextureRegion(&dloc, 0u, 0u, 0u, &sloc, nullptr);

    D3D12_RESOURCE_BARRIER to_srv{};
    to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_srv.Transition.pResource = staging_r8;
    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    host_list->ResourceBarrier(1u, &to_srv);

    const uint32_t consts[4] = {
        info.map_tiles_x,
        info.map_tiles_y,
        fp.Footprint.RowPitch,
        sub,
    };
    host_list->SetComputeRootSignature(fb_codec_rs_);
    host_list->SetPipelineState(fb_encode_pso_);
    host_list->SetComputeRoot32BitConstants(0u, 4u, consts, 0u);
    host_list->SetComputeRootShaderResourceView(1u, staging_r8->GetGPUVirtualAddress());
    host_list->SetComputeRootUnorderedAccessView(2u, host_feedback_dst->GetGPUVirtualAddress());
    const uint32_t total = std::max<uint32_t>(1u, info.map_tiles_x * info.map_tiles_y);
    host_list->Dispatch((total + 63u) / 64u, 1u, 1u);

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    post[0].UAV.pResource = host_feedback_dst;
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition.pResource = host_src_tex;
    post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    host_list->ResourceBarrier(2u, post);

    TrackTransientGpuResource(staging_r8);
    sf_encode_intercepts_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

// IOpenScarlettAaaRuntime implementation
HRESULT STDMETHODCALLTYPE DeviceX::GetAaaWiringStats(D3D12XBOX_AAA_WIRING_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));

    const SamplerFeedbackManagerStats sf = sampler_feedback_mgr_.GetStats();
    out_stats->sf_host_tier = static_cast<uint32_t>(sf.host_tier);
    out_stats->sf_active_maps = sf.active_maps;
    out_stats->sf_preserved_views = sf.preserved_views;
    out_stats->sf_total_clears = sf.total_clears;
    out_stats->sf_total_resolves = sf.total_resolves;
    out_stats->sf_decode_intercepts = sf_decode_intercepts_.load(std::memory_order_relaxed);
    out_stats->sf_encode_intercepts = sf_encode_intercepts_.load(std::memory_order_relaxed);

    const QueryAccumulatorStats qa = query_accumulator_.GetStats();
    out_stats->qa_initialized = query_accumulator_.IsInitialized() ? 1u : 0u;
    out_stats->qa_begin_count = qa.begin_count;
    out_stats->qa_end_count = qa.end_count;
    out_stats->qa_resolve_count = qa.resolve_count;
    out_stats->qa_cl_begin_count = qa_cl_begin_count_.load(std::memory_order_relaxed);
    out_stats->qa_cl_end_count = qa_cl_end_count_.load(std::memory_order_relaxed);
    out_stats->qa_cl_resolve_count = qa_cl_resolve_count_.load(std::memory_order_relaxed);
    out_stats->qa_gpu_accumulate_dispatches = qa.gpu_accumulate_dispatches;
    out_stats->qa_active_frame = qa.active_frame_index;

    const openscarlett::TitleVaMirrorStats va = title_va_mirror_.GetStats();
    out_stats->va_active_regions = va.active_registrations;
    out_stats->va_dirty_pages_flushed = va.dirty_pages_flushed;
    out_stats->va_bytes_uploaded = va.bytes_uploaded;
    out_stats->va_bytes_readback = va.bytes_readback;
    out_stats->va_pending_readbacks = va.pending_readbacks;
    out_stats->va_completed_readbacks = va.completed_readbacks;
    out_stats->va_queue_flushes = va_queue_flushes_.load(std::memory_order_relaxed);
    out_stats->va_indirect_flushes = va_indirect_flushes_.load(std::memory_order_relaxed);

    const openscarlett::DescriptorBridgeStats db = descriptor_bridge_.GetStats();
    out_stats->db_initialized = descriptor_bridge_.IsD3D12Initialized() ? 1u : 0u;
    out_stats->db_resource_slots = db.allocated_resource_slots;
    out_stats->db_sampler_slots = db.allocated_sampler_slots;
    out_stats->db_vsharp_bindings = db.vsharp_bindings;
    out_stats->db_tsharp_bindings = db.tsharp_bindings;
    out_stats->db_ssharp_bindings = db.ssharp_bindings;
    out_stats->db_heap_binds = db_heap_binds_.load(std::memory_order_relaxed);
    out_stats->db_table_binds = db_table_binds_.load(std::memory_order_relaxed);
    out_stats->db_root_const_updates = db_root_const_updates_.load(std::memory_order_relaxed);
    out_stats->db_root_cbv_updates = db_root_cbv_updates_.load(std::memory_order_relaxed);
    for (uint32_t i = 0u; i < 16u; ++i) {
        out_stats->db_root_sgprs[i] = descriptor_bridge_.GetRootSgpr(i);
    }

    {
        std::lock_guard<std::mutex> lock(xg_untiler_mutex_);
        out_stats->xg_untiler_ready = (xg_untiler_ && xg_untiler_->IsD3D12Ready()) ? 1u : 0u;
    }
    {
        std::lock_guard<std::mutex> lock(tile_mode_mutex_);
        out_stats->xg_tagged_resources = static_cast<uint32_t>(resource_tile_modes_.size());
    }
    out_stats->xg_gpu_untile_dispatches = xg_gpu_untile_dispatches_.load(std::memory_order_relaxed);
    out_stats->xg_gpu_retile_dispatches = xg_gpu_retile_dispatches_.load(std::memory_order_relaxed);
    out_stats->xg_cpu_fallbacks = xg_cpu_fallbacks_.load(std::memory_order_relaxed);

    out_stats->cmdlist_kickoffs = cmdlist_kickoffs_.load(std::memory_order_relaxed);
    out_stats->cmdlist_write32_calls = cmdlist_write32_calls_.load(std::memory_order_relaxed);
    out_stats->cmdlist_predicationx_calls = cmdlist_predicationx_calls_.load(std::memory_order_relaxed);
    out_stats->cmdlist_flush_cache_calls = cmdlist_flush_cache_calls_.load(std::memory_order_relaxed);
    out_stats->cmdlist_dispatch_mesh_calls = cmdlist_dispatch_mesh_calls_.load(std::memory_order_relaxed);
    out_stats->last_mesh_x = last_mesh_x_.load(std::memory_order_relaxed);
    out_stats->last_mesh_y = last_mesh_y_.load(std::memory_order_relaxed);
    out_stats->last_mesh_z = last_mesh_z_.load(std::memory_order_relaxed);
    out_stats->total_mesh_groups = total_mesh_groups_.load(std::memory_order_relaxed);
    out_stats->device_removed_reason =
        static_cast<int32_t>(device_removed_reason_.load(std::memory_order_acquire));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetResourceTileMode(
    ID3D12Resource* resource, uint32_t xg_tile_mode) {
    if (!resource || xg_tile_mode > static_cast<uint32_t>(XG_TILE_MODE_3D_THICK_64KB_R_X)) {
        return E_INVALIDARG;
    }
    SetTrackedResourceTileMode(resource, static_cast<XG_TILE_MODE>(xg_tile_mode));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetResourceTileMode(
    ID3D12Resource* resource, uint32_t* out_xg_tile_mode) {
    if (!resource || !out_xg_tile_mode) return E_INVALIDARG;
    *out_xg_tile_mode = static_cast<uint32_t>(GetTrackedResourceTileMode(resource));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetQueryAccumulatorHeap(
    uint32_t query_kind, ID3D12QueryHeap** out_heap) {
    if (!out_heap) return E_POINTER;
    *out_heap = nullptr;
    if (query_kind > static_cast<uint32_t>(QueryKind::PipelineStatistics)) return E_INVALIDARG;
    ID3D12QueryHeap* qh = query_accumulator_.GetQueryHeap(static_cast<QueryKind>(query_kind));
    if (!qh) return E_FAIL;
    qh->AddRef();
    *out_heap = qh;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::FlushTitleVaMirrorOnQueue(
    ID3D12CommandQueue* queue, uint64_t* out_bytes_uploaded) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueue* host_q = CommandQueueX::UnwrapHostQueue(queue);
    if (!host_q) return E_INVALIDARG;
    return FlushMirroredTitleVaPagesOnQueue(host_q, host_q->GetDesc().Type, out_bytes_uploaded);
}

HRESULT STDMETHODCALLTYPE DeviceX::ScheduleTitleVaWriteback(
    ID3D12GraphicsCommandList* cmd_list,
    uint64_t title_va,
    uint64_t fence_value,
    uint64_t offset,
    uint64_t size_bytes) {
    if (!cmd_list || title_va == 0ull) return E_INVALIDARG;
    ID3D12GraphicsCommandList* host_cl = CommandListX::UnwrapHostGraphicsList(cmd_list);
    openscarlett::TitleVaRegionInfo rinfo{};
    const D3D12_RESOURCE_STATES restore =
        title_va_mirror_.LookupRegion(title_va, rinfo)
            ? rinfo.current_state
            : D3D12_RESOURCE_STATE_COMMON;
    const openscarlett::Status st = title_va_mirror_.ScheduleGpuReadback(
        host_cl, title_va, fence_value, offset, size_bytes, restore);
    return st.ok() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceX::CompleteTitleVaWritebacks(
    uint64_t completed_fence_value, uint64_t* out_completed_count) {
    uint64_t completed = 0ull;
    const openscarlett::Status st =
        title_va_mirror_.CompleteReadbacksOnFence(completed_fence_value, &completed, nullptr);
    if (out_completed_count) *out_completed_count = completed;
    return st.ok() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceX::ClearFeedbackMap(
    ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* feedback_resource) {
    if (!cmd_list || !feedback_resource) return E_INVALIDARG;
    const openscarlett::Status st = sampler_feedback_mgr_.ClearSamplerFeedbackMap(
        CommandListX::UnwrapHostGraphicsList(cmd_list), UnwrapResource(feedback_resource));
    return st.ok() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceX::WriteFeedbackSamples(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* feedback_resource,
    const uint32_t* packed_xy_slice_mip_quads,
    uint32_t sample_count) {
    if (!cmd_list || !feedback_resource || (!packed_xy_slice_mip_quads && sample_count > 0u)) {
        return E_INVALIDARG;
    }
    std::vector<SamplerFeedbackSampleWrite> samples;
    samples.reserve(sample_count);
    for (uint32_t i = 0u; i < sample_count; ++i) {
        SamplerFeedbackSampleWrite s{};
        s.tile_x = packed_xy_slice_mip_quads[i * 4u + 0u];
        s.tile_y = packed_xy_slice_mip_quads[i * 4u + 1u];
        s.array_slice = packed_xy_slice_mip_quads[i * 4u + 2u];
        s.mip_level = static_cast<uint8_t>(packed_xy_slice_mip_quads[i * 4u + 3u] & 0xFFu);
        samples.push_back(s);
    }
    const openscarlett::Status st = sampler_feedback_mgr_.RecordFeedbackSampleWrites(
        CommandListX::UnwrapHostGraphicsList(cmd_list), UnwrapResource(feedback_resource), samples);
    return st.ok() ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceX::ReadbackFeedbackMinMip(
    ID3D12CommandQueue* queue,
    ID3D12Resource* feedback_resource,
    uint8_t* out_tiles,
    uint32_t max_tiles,
    uint32_t* out_tiles_x,
    uint32_t* out_tiles_y) {
    if (!queue || !feedback_resource) return E_INVALIDARG;
    SamplerFeedbackMinMipReadback rb{};
    const openscarlett::Status st = sampler_feedback_mgr_.ResolveAndReadbackMinMip(
        CommandQueueX::UnwrapHostQueue(queue), UnwrapResource(feedback_resource), rb);
    if (!st.ok()) return E_FAIL;
    if (out_tiles_x) *out_tiles_x = rb.tiles_x;
    if (out_tiles_y) *out_tiles_y = rb.tiles_y;
    if (out_tiles && max_tiles > 0u && !rb.min_mip_tiles.empty()) {
        const size_t n = std::min<size_t>(max_tiles, rb.min_mip_tiles.size());
        std::memcpy(out_tiles, rb.min_mip_tiles.data(), n);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::CalibrateQueueClock(
    ID3D12CommandQueue* queue,
    D3D12XBOX_CLOCK_CALIBRATION_SUMMARY* out_summary) {
    if (!queue || !out_summary) return E_INVALIDARG;
    std::memset(out_summary, 0, sizeof(*out_summary));
    GpuClockCalibrationResult res{};
    const openscarlett::Status st =
        CalibrateGpuClockWithQpc(CommandQueueX::UnwrapHostQueue(queue), res, 3u, 1u);
    if (!st.ok()) return E_FAIL;
    out_summary->valid = res.gpu_timestamp_frequency_valid ? 1u : 0u;
    out_summary->gpu_timestamp_frequency_hz = res.gpu_timestamp_frequency_hz;
    out_summary->cpu_qpc_frequency_hz = res.cpu_qpc_frequency_hz;
    out_summary->anchor_gpu_ticks = res.anchor.gpu_timestamp_ticks;
    out_summary->anchor_cpu_qpc_ticks = res.anchor.cpu_qpc_ticks;
    out_summary->estimated_offset_seconds = res.estimated_offset_seconds;
    out_summary->max_bracket_error_us = res.max_bracket_error_us;
    out_summary->estimated_drift_ppm = res.estimated_drift_ppm;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::BindBridgeBufferSrv(
    ID3D12Resource* resource,
    uint32_t num_elements,
    uint32_t stride_bytes,
    uint32_t* out_bindless_index) {
    if (!resource || !out_bindless_index) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    openscarlett::Rdna2BufferDescriptor vsharp{};
    vsharp.base_address = host_res->GetGPUVirtualAddress();
    vsharp.num_records = num_elements;
    vsharp.stride = static_cast<uint16_t>(stride_bytes);
    vsharp.format = static_cast<uint8_t>(openscarlett::Rdna2MtbufFormat::R32_UINT);
    uint32_t idx = 0u;
    const openscarlett::Status st = descriptor_bridge_.BindVSharpSrv(
        vsharp, &title_va_mirror_, host_res, idx);
    if (!st.ok()) return E_FAIL;
    *out_bindless_index = idx;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::BindBridgeHeaps(ID3D12GraphicsCommandList* cmd_list) {
    if (!cmd_list) return E_INVALIDARG;
    descriptor_bridge_.BindHeapsToCommandList(CommandListX::UnwrapHostGraphicsList(cmd_list));
    RecordBridgeHeapBind();
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
