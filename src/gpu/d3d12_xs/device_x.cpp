#include "device_x.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
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

extern std::atomic<void*> g_bink_host_pso;
static std::atomic<bool> g_calib_pso_ready{false};
static std::mutex g_diag_srv_mu;
static std::unordered_map<uint64_t, ID3D12Resource*> g_diag_sh_to_res;

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
        if (hr != E_NOINTERFACE && hr != DXGI_ERROR_NOT_FOUND && CommandListX::IsDiagTraceEnabled()) {
            static std::mutex s_dev_log_mu;
            static std::atomic<uint32_t> s_fail_lines{0u};
            if (s_fail_lines.fetch_add(1u, std::memory_order_relaxed) < 200u) {
                std::lock_guard<std::mutex> lk(s_dev_log_mu);
                FILE* f = nullptr;
                if (::fopen_s(&f, CommandListX::DiagTraceLogPath(), "a") == 0 && f) {
                    std::fprintf(f, "[DeviceX:FAIL] %s slot=%d hr=0x%08X\n",
                                 method, slot, static_cast<unsigned>(hr));
                    std::fclose(f);
                }
            }
        }
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
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        default:
            return fmt;
    }
}

bool IsDepthOrPlanarFormatForCopy(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
        case DXGI_FORMAT_420_OPAQUE:
            return true;
        default:
            return false;
    }
}

// Resolves a console GPU-only alias VA (e.g. a paired PAGE_NOACCESS | PAGE_GPU_READONLY
// XMemVirtualAlloc window) back to its CPU-readable mirror VA when xmem.dll has paired them.
uint64_t ResolveGuestCpuAliasVa(uint64_t va) noexcept {
    if (va == 0ull) return 0ull;
    using Fn = uint64_t(__stdcall*)(uint64_t);
    static std::atomic<Fn> s_fn{nullptr};
    Fn fn = s_fn.load(std::memory_order_acquire);
    if (!fn) {
        if (HMODULE hm = ::GetModuleHandleW(L"xmem.dll")) {
            fn = reinterpret_cast<Fn>(::GetProcAddress(hm, "OpenScarlett_XMemResolveCpuAliasVa"));
        }
        if (!fn) {
            wchar_t shim_dir[MAX_PATH]{};
            const DWORD n = ::GetEnvironmentVariableW(L"OPENSCARLETT_SHIM_DIR", shim_dir, MAX_PATH);
            if (n > 0u && n < MAX_PATH) {
                std::wstring full(shim_dir, n);
                if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
                    full.push_back(L'\\');
                }
                full += L"xmem.dll";
                if (HMODULE hm2 = ::GetModuleHandleW(full.c_str())) {
                    fn = reinterpret_cast<Fn>(::GetProcAddress(hm2, "OpenScarlett_XMemResolveCpuAliasVa"));
                }
            }
        }
        if (fn) {
            s_fn.store(fn, std::memory_order_release);
        }
    }
    return fn ? fn(va) : va;
}

UINT SanitizeOrTranslateXboxShader4ComponentMapping(UINT m, DXGI_FORMAT srv_fmt) noexcept {
    if (srv_fmt == DXGI_FORMAT_A8_UNORM) {
        return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }
    // Valid Desktop D3D12 Shader4ComponentMapping MUST have bit 12 (0x1000) set and each 3-bit lane in 0..5.
    if ((m & D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES) != 0u &&
        (m & ~0x1FFFu) == 0u &&
        ((m & 0x7u) <= 5u) && (((m >> 3u) & 0x7u) <= 5u) &&
        (((m >> 6u) & 0x7u) <= 5u) && (((m >> 9u) & 0x7u) <= 5u)) {
        return m;
    }
    // RDNA2 SQ_IMG_RSRC_WORD3 12-bit DST_SEL_X/Y/Z/W (0=0, 1=1, 4=R, 5=G, 6=B, 7=A; bit 12 == 0).
    if (m != 0u && (m & ~0xFFFu) == 0u) {
        auto map_sel = [](UINT sel, UINT& out_d3d) noexcept -> bool {
            switch (sel & 0x7u) {
                case 0u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0; return true;
                case 1u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1; return true;
                case 4u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0; return true;
                case 5u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1; return true;
                case 6u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2; return true;
                case 7u: out_d3d = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3; return true;
                default: return false;
            }
        };
        UINT c0 = 0u, c1 = 1u, c2 = 2u, c3 = 3u;
        if (map_sel(m, c0) && map_sel(m >> 3u, c1) && map_sel(m >> 6u, c2) && map_sel(m >> 9u, c3)) {
            return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(c0, c1, c2, c3);
        }
    }
    return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
}

DXGI_FORMAT ResolveCompatibleSrvFormat(DXGI_FORMAT td_fmt, DXGI_FORMAT req_fmt) noexcept {
    if (req_fmt == DXGI_FORMAT_UNKNOWN || req_fmt == td_fmt) {
        return MapTypelessFormatForSrv(td_fmt);
    }
    switch (td_fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            if (req_fmt == DXGI_FORMAT_R8G8B8A8_UNORM || req_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                (td_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                 (req_fmt == DXGI_FORMAT_R8G8B8A8_UINT || req_fmt == DXGI_FORMAT_R8G8B8A8_SNORM ||
                  req_fmt == DXGI_FORMAT_R8G8B8A8_SINT))) {
                return (td_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? req_fmt : td_fmt;
            }
            break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            if (req_fmt == DXGI_FORMAT_B8G8R8A8_UNORM || req_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            if (req_fmt == DXGI_FORMAT_B8G8R8X8_UNORM || req_fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R8_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R8_UNORM || req_fmt == DXGI_FORMAT_R8_SNORM ||
                req_fmt == DXGI_FORMAT_R8_UINT || req_fmt == DXGI_FORMAT_R8_SINT) {
                return req_fmt;
            }
            if (req_fmt == DXGI_FORMAT_A8_UNORM) {
                return DXGI_FORMAT_R8_UNORM;
            }
            break;
        case DXGI_FORMAT_R8_UNORM:
            if (req_fmt == DXGI_FORMAT_A8_UNORM) {
                return DXGI_FORMAT_R8_UNORM;
            }
            break;
        case DXGI_FORMAT_R8G8_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R8G8_UNORM || req_fmt == DXGI_FORMAT_R8G8_SNORM ||
                req_fmt == DXGI_FORMAT_R8G8_UINT || req_fmt == DXGI_FORMAT_R8G8_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R16_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R16_FLOAT || req_fmt == DXGI_FORMAT_R16_UNORM ||
                req_fmt == DXGI_FORMAT_R16_SNORM || req_fmt == DXGI_FORMAT_R16_UINT ||
                req_fmt == DXGI_FORMAT_R16_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R16G16_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R16G16_FLOAT || req_fmt == DXGI_FORMAT_R16G16_UNORM ||
                req_fmt == DXGI_FORMAT_R16G16_SNORM || req_fmt == DXGI_FORMAT_R16G16_UINT ||
                req_fmt == DXGI_FORMAT_R16G16_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || req_fmt == DXGI_FORMAT_R16G16B16A16_UNORM ||
                req_fmt == DXGI_FORMAT_R16G16B16A16_SNORM || req_fmt == DXGI_FORMAT_R16G16B16A16_UINT ||
                req_fmt == DXGI_FORMAT_R16G16B16A16_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R32_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R32_FLOAT || req_fmt == DXGI_FORMAT_R32_UINT ||
                req_fmt == DXGI_FORMAT_R32_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R32G32_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R32G32_FLOAT || req_fmt == DXGI_FORMAT_R32G32_UINT ||
                req_fmt == DXGI_FORMAT_R32G32_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT || req_fmt == DXGI_FORMAT_R32G32B32A32_UINT ||
                req_fmt == DXGI_FORMAT_R32G32B32A32_SINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            if (req_fmt == DXGI_FORMAT_R10G10B10A2_UNORM || req_fmt == DXGI_FORMAT_R10G10B10A2_UINT) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC1_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC1_UNORM || req_fmt == DXGI_FORMAT_BC1_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC2_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC2_UNORM || req_fmt == DXGI_FORMAT_BC2_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC3_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC3_UNORM || req_fmt == DXGI_FORMAT_BC3_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC4_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC4_UNORM || req_fmt == DXGI_FORMAT_BC4_SNORM) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC5_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC5_UNORM || req_fmt == DXGI_FORMAT_BC5_SNORM) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC6H_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC6H_UF16 || req_fmt == DXGI_FORMAT_BC6H_SF16) {
                return req_fmt;
            }
            break;
        case DXGI_FORMAT_BC7_TYPELESS:
            if (req_fmt == DXGI_FORMAT_BC7_UNORM || req_fmt == DXGI_FORMAT_BC7_UNORM_SRGB) {
                return req_fmt;
            }
            break;
        default:
            break;
    }
    return MapTypelessFormatForSrv(td_fmt);
}

struct DecodedGfx10TSharp {
    uint64_t gpu_va = 0ull;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 1u;
    uint32_t base_array = 0u;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t raw_img_fmt = 0u;
    uint32_t dst_sel_12 = 0u;
    uint32_t sw_mode = 0u;
    uint32_t rsrc_type = 0u;
    uint32_t base_level = 0u;
    uint32_t last_level = 0u;
};

bool DecodeGfx10ImageRsrcDescriptor(const uint8_t bytes[32], DecodedGfx10TSharp& out) noexcept {
    uint32_t w[8]{};
    std::memcpy(w, bytes, 32u);
    const uint32_t rtype = (w[3] >> 28u) & 0xFu;
    // GFX10 SQ_RSRC_IMG_*: 8=1D, 9=2D, 10=3D, 11=CUBE, 12=1D_ARR, 13=2D_ARR, 14=2D_MSAA, 15=2D_MSAA_ARR
    if (rtype < 8u || rtype > 15u) return false;
    // w[6..7] upper metadata in standalone 32B T# is zero or small; reject obvious host pointers.
    const uint64_t gpu_va = ((static_cast<uint64_t>(w[1] & 0xFFu) << 32u) | static_cast<uint64_t>(w[0])) << 8u;
    // Canonical user-mode / Scarlett UMA VAs on Windows x64 are strictly < 16 TiB (0x100000000000).
    if (gpu_va < 0x10000ull || gpu_va >= 0x0000100000000000ull) return false;
    const uint32_t width = (((w[2] & 0xFFFu) << 2u) | (w[1] >> 30u)) + 1u;
    const uint32_t height = (rtype == 8u || rtype == 12u) ? 1u : (((w[2] >> 14u) & 0x3FFFu) + 1u);
    if (width == 0u || width > 16384u || height == 0u || height > 16384u) return false;
    uint32_t depth = 1u;
    uint32_t base_array = 0u;
    if (rtype == 10u) {
        depth = (w[4] & 0x1FFFu) + 1u;
        if (depth == 0u || depth > 2048u) return false;
    } else if (rtype == 11u || rtype == 12u || rtype == 13u || rtype == 15u) {
        const uint32_t last_arr = w[4] & 0x1FFFu;
        const uint32_t first_arr = (w[4] >> 16u) & 0x1FFFu;
        base_array = first_arr;
        depth = (last_arr >= first_arr) ? (last_arr - first_arr + 1u) : 1u;
        if (depth > 2048u) return false;
    }

    const uint32_t img_fmt = (w[1] >> 20u) & 0x1FFu;
    if (img_fmt == 0u) return false;
    DXGI_FORMAT dxgi_fmt = DXGI_FORMAT_UNKNOWN;
    switch (img_fmt) {
        case 1u:  dxgi_fmt = DXGI_FORMAT_R8_UNORM; break;
        case 2u:  dxgi_fmt = DXGI_FORMAT_R8_SNORM; break;
        case 4u:  dxgi_fmt = DXGI_FORMAT_R8_UINT; break;
        case 5u:  dxgi_fmt = DXGI_FORMAT_R8_SINT; break;
        case 9u:  dxgi_fmt = DXGI_FORMAT_R8G8_UNORM; break;
        case 10u: dxgi_fmt = DXGI_FORMAT_R16_UNORM; break;
        case 11u: dxgi_fmt = DXGI_FORMAT_R16_SNORM; break;
        case 12u: dxgi_fmt = DXGI_FORMAT_R16_FLOAT; break;
        case 14u: dxgi_fmt = DXGI_FORMAT_R16_UINT; break;
        case 15u: dxgi_fmt = DXGI_FORMAT_R16_SINT; break;
        case 20u: dxgi_fmt = DXGI_FORMAT_R32_UINT; break;
        case 21u: dxgi_fmt = DXGI_FORMAT_R32_SINT; break;
        case 22u: dxgi_fmt = DXGI_FORMAT_R32_FLOAT; break;
        case 24u: dxgi_fmt = DXGI_FORMAT_R16G16_UNORM; break;
        case 25u: dxgi_fmt = DXGI_FORMAT_R16G16_SNORM; break;
        case 26u: dxgi_fmt = DXGI_FORMAT_R16G16_FLOAT; break;
        case 35u: dxgi_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case 36u: dxgi_fmt = DXGI_FORMAT_R8G8B8A8_SNORM; break;
        case 38u: dxgi_fmt = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case 39u: dxgi_fmt = DXGI_FORMAT_R8G8B8A8_UINT; break;
        case 40u: dxgi_fmt = DXGI_FORMAT_R8G8B8A8_SINT; break;
        case 42u: dxgi_fmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        case 45u: dxgi_fmt = DXGI_FORMAT_R11G11B10_FLOAT; break;
        case 48u: dxgi_fmt = DXGI_FORMAT_R16G16B16A16_UNORM; break;
        case 49u: dxgi_fmt = DXGI_FORMAT_R16G16B16A16_SNORM; break;
        case 50u: dxgi_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case 51u: dxgi_fmt = DXGI_FORMAT_R32G32_UINT; break;
        case 52u: dxgi_fmt = DXGI_FORMAT_R32G32_SINT; break;
        case 53u: dxgi_fmt = DXGI_FORMAT_R32G32_FLOAT; break;
        case 54u: dxgi_fmt = DXGI_FORMAT_R32G32B32A32_UINT; break;
        case 55u: dxgi_fmt = DXGI_FORMAT_R32G32B32A32_SINT; break;
        case 56u: dxgi_fmt = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case 63u: dxgi_fmt = DXGI_FORMAT_R9G9B9E5_SHAREDEXP; break;
        case 67u: dxgi_fmt = DXGI_FORMAT_BC1_UNORM; break;
        case 68u: dxgi_fmt = DXGI_FORMAT_BC1_UNORM_SRGB; break;
        case 69u: dxgi_fmt = DXGI_FORMAT_BC2_UNORM; break;
        case 70u: dxgi_fmt = DXGI_FORMAT_BC2_UNORM_SRGB; break;
        case 71u: dxgi_fmt = DXGI_FORMAT_BC3_UNORM; break;
        case 72u: dxgi_fmt = DXGI_FORMAT_BC3_UNORM_SRGB; break;
        case 73u: dxgi_fmt = DXGI_FORMAT_BC4_UNORM; break;
        case 74u: dxgi_fmt = DXGI_FORMAT_BC4_SNORM; break;
        case 75u: dxgi_fmt = DXGI_FORMAT_BC5_UNORM; break;
        case 76u: dxgi_fmt = DXGI_FORMAT_BC5_SNORM; break;
        case 77u: dxgi_fmt = DXGI_FORMAT_BC6H_UF16; break;
        case 78u: dxgi_fmt = DXGI_FORMAT_BC6H_SF16; break;
        case 79u: dxgi_fmt = DXGI_FORMAT_BC7_UNORM; break;
        case 80u: dxgi_fmt = DXGI_FORMAT_BC7_UNORM_SRGB; break;
        default:  dxgi_fmt = DXGI_FORMAT_UNKNOWN; break;
    }

    out.gpu_va = gpu_va;
    out.width = width;
    out.height = height;
    out.depth = depth;
    out.base_array = base_array;
    out.format = dxgi_fmt;
    out.raw_img_fmt = img_fmt;
    out.dst_sel_12 = w[3] & 0xFFFu;
    out.sw_mode = (w[3] >> 20u) & 0x1Fu;
    out.rsrc_type = rtype;
    out.base_level = (w[3] >> 12u) & 0xFu;
    out.last_level = (w[3] >> 16u) & 0xFu;
    return true;
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

uint64_t MaxReadableGuestSpan(uint64_t va, uint64_t max_bytes) noexcept {
    if (va < 0x10000ull || max_bytes == 0ull) return 0ull;
    uint64_t cur = va;
    const uint64_t limit = (va > UINT64_MAX - max_bytes) ? UINT64_MAX : (va + max_bytes);
    while (cur < limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(cur)),
                         &mbi, sizeof(mbi)) == 0) {
            break;
        }
        if (mbi.State != MEM_COMMIT) break;
        constexpr DWORD kReadableMask =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadableMask) == 0u ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            break;
        }
        const uint64_t region_end =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
            static_cast<uint64_t>(mbi.RegionSize);
        if (region_end <= cur) break;
        cur = std::min<uint64_t>(region_end, limit);
    }
    return cur - va;
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
#if defined(__ID3D12Device10_INTERFACE_DEFINED__) && !defined(__MINGW32__)
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
            return sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC2>);
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
            return sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC1>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
            return sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC2>);
#endif
        default:
            return 0u;
    }
}

// ---------------------------------------------------------------------------
// Sprint 14: DXBC/DXIL PSV0 Mesh Topology & Stage Inspector (xbox-emulator #168, #176)
// Public Microsoft DXIL Pipeline State Validation (PSV0) container spec:
//   ShaderStage: 0=PS, 1=VS, 2=GS, 3=HS, 4=DS, 5=CS, 13=MS, 14=AS
//   PSVRuntimeInfo0::MS.OutputTopology: 1=Point, 2=Line, 3=Triangle
// ---------------------------------------------------------------------------
bool ExtractDxbcPsv0Info(
    const void* data,
    size_t size,
    uint8_t& out_stage,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE& out_mesh_topo) noexcept {
    out_stage = 0xFFu;
    out_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    if (!data || size < 36u) return false;
    const auto* b = static_cast<const uint8_t*>(data);
    if (b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return false;

    uint32_t chunk_count = 0u;
    std::memcpy(&chunk_count, b + 28u, sizeof(chunk_count));
    if (chunk_count == 0u || chunk_count > 128u) return false;
    if (32u + static_cast<size_t>(chunk_count) * 4u > size) return false;

    for (uint32_t i = 0u; i < chunk_count; ++i) {
        uint32_t off = 0u;
        std::memcpy(&off, b + 32u + static_cast<size_t>(i) * 4u, sizeof(off));
        if (off + 8u > size) continue;
        if (b[off + 0u] == 'P' && b[off + 1u] == 'S' && b[off + 2u] == 'V' && b[off + 3u] == '0') {
            uint32_t chunk_sz = 0u;
            std::memcpy(&chunk_sz, b + off + 4u, sizeof(chunk_sz));
            if (off + 8u + chunk_sz > size || chunk_sz < 4u) continue;
            const uint8_t* psv = b + off + 8u;
            uint32_t info_size = 0u;
            std::memcpy(&info_size, psv, sizeof(info_size));
            // PSVRuntimeInfo0 is 24 bytes (starts at psv + 4); PSVRuntimeInfo1 adds 12 bytes (ShaderStage at +24).
            if (info_size >= 28u && chunk_sz >= 4u + 28u) {
                const uint8_t* ri = psv + 4u;
                out_stage = ri[24u];
                if (out_stage == 13u) {
                    // PSVRuntimeInfo0::MS layout (public DXIL spec):
                    // [0..3]: GroupSharedBytesUsed, [4..7]: GroupSharedBytesDependentOnViewID,
                    // [8..11]: PayloadSizeInBytes, [12..13]: MaxOutputVertices,
                    // [14..15]: MaxOutputPrimitives, [16]: OutputTopology (or in byte 12..16 depending on version).
                    // Check standard PSVRuntimeInfo0 MS struct:
                    //   uint32 GroupSharedBytesUsed (0..3)
                    //   uint32 GroupSharedBytesDependentOnViewID (4..7)
                    //   uint32 PayloadSizeInBytes (8..11)
                    //   uint16 MaxOutputVertices (12..13)
                    //   uint16 MaxOutputPrimitives (14..15)
                    //   Followed by PSVRuntimeInfo1 at offset 24:
                    //   uint8 ShaderStage (24), uint8 UsesViewID (25),
                    //   uint16 MaxVertexCount / SigInputVectors (26..27) ...
                    //   Note: in DXIL PSVRuntimeInfo1, OutputTopology is stored in byte 26 (union with MaxVertexCount)
                    //   for MS: 0=Undefined, 1=Point, 2=Line, 3=Triangle (or D3D_PRIMITIVE_TOPOLOGY: 1=Point, 2=Line, 3=Triangle).
                    const uint8_t ms_topo = ri[26u] & 0x0Fu;
                    if (ms_topo == 1u) {
                        out_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
                    } else if (ms_topo == 2u) {
                        out_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                    } else if (ms_topo == 3u) {
                        out_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    } else {
                        out_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

// Reconcile fp16 / min16 half-precision varyings between VS output and PS input signatures (Leonida 81ec7f7).
uint32_t LinkFp16HalfStageSignatures(
    openscarlett::XboxParsedShaderContainer& vs_parsed,
    openscarlett::XboxParsedShaderContainer& ps_parsed) noexcept {
    uint32_t linked = 0u;
    auto is_16bit_prec = [](uint32_t min_prec) noexcept -> bool {
        // D3D_MIN_PRECISION: 1=FLOAT_16, 2=FLOAT_2_8, 4=SINT_16, 5=UINT_16, 0xF0=ANY_16
        return min_prec == 1u || min_prec == 2u || min_prec == 4u || min_prec == 5u || min_prec == 0xF0u;
    };
    auto ps_attr_has_fp16_cntl = [&](uint32_t reg_idx) noexcept -> bool {
        for (const auto& c : ps_parsed.reg_setup.ps_input_cntls) {
            if (c.attr_index == reg_idx && c.fp16_interp) return true;
        }
        return false;
    };

    for (auto& vs_out : vs_parsed.output_signature) {
        if (vs_out.system_value_type != 0u) continue;
        for (auto& ps_in : ps_parsed.input_signature) {
            if (ps_in.system_value_type != 0u) continue;
            const bool sem_match =
                EqualsAsciiCaseInsensitive(vs_out.semantic_name.c_str(), ps_in.semantic_name.c_str()) &&
                vs_out.semantic_index == ps_in.semantic_index;
            if (!sem_match) continue;

            const bool vs_f16 = is_16bit_prec(vs_out.min_precision);
            const bool ps_f16 = is_16bit_prec(ps_in.min_precision) || ps_attr_has_fp16_cntl(ps_in.register_index);
            if (vs_f16 || ps_f16) {
                // Harmonize precision & half-register masks across VS output and PS input so
                // packed f16 halves link cleanly without Desktop D3D12 PSO signature mismatch.
                const uint32_t target_prec = vs_f16 ? vs_out.min_precision : (ps_in.min_precision ? ps_in.min_precision : 1u);
                bool changed = false;
                if (vs_out.min_precision != target_prec) {
                    vs_out.min_precision = target_prec;
                    changed = true;
                }
                if (ps_in.min_precision != target_prec) {
                    ps_in.min_precision = target_prec;
                    changed = true;
                }
                if (vs_out.component_type != ps_in.component_type) {
                    ps_in.component_type = vs_out.component_type;
                    changed = true;
                }
                // Merge split low/high f16 half masks (e.g. .xy + .zw packed into one register)
                const uint8_t combined_mask = static_cast<uint8_t>((vs_out.mask | ps_in.mask) & 0x0Fu);
                if (combined_mask != 0u && (vs_out.mask != combined_mask || ps_in.mask != combined_mask)) {
                    vs_out.mask = combined_mask;
                    ps_in.mask = combined_mask;
                    changed = true;
                }
                if (changed || (vs_f16 && ps_f16)) {
                    ++linked;
                }
            }
        }
    }
    return linked;
}

// Portable SEH-guarded guest CPU memory helpers (xbox-emulator #153).
bool SafeCopyGuestMemory(void* dst, const void* src, size_t byte_count) noexcept {
    if (!dst || !src || byte_count == 0u) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(dst, src, byte_count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!IsReadableGuestVaRange(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src)), byte_count)) {
        return false;
    }
    std::memcpy(dst, src, byte_count);
    return true;
#endif
}

bool SafeHashGuestMemoryFnv1a64(const void* src, size_t byte_count, uint64_t& out_hash) noexcept {
    out_hash = 14695981039346656037ull;
    if (!src || byte_count == 0u) return false;
#if defined(_MSC_VER)
    __try {
        const auto* p = static_cast<const uint8_t*>(src);
        uint64_t h = 14695981039346656037ull ^ static_cast<uint64_t>(byte_count);
        if (byte_count > (256u * 1024u)) {
            // Fast O(1) probe (first 256B + mid 256B + 256 strided 8B words + last 64B) so
            // streaming/Bink2 planes (>256 KiB) catch localized CPU updates in <1 us without stalling.
            for (size_t off = 0u; off + 8u <= 256u && off + 8u <= byte_count; off += 8u) {
                uint64_t w = 0ull;
                std::memcpy(&w, p + off, sizeof(w));
                h = (h ^ w) * 1099511628211ull;
            }
            const size_t mid_base = ((byte_count / 2u) & ~size_t(7u));
            for (size_t off = mid_base; off + 8u <= mid_base + 256u && off + 8u <= byte_count; off += 8u) {
                uint64_t w = 0ull;
                std::memcpy(&w, p + off, sizeof(w));
                h = (h ^ w) * 1099511628211ull;
            }
            const size_t stride = std::max<size_t>(8u, (byte_count / 256u) & ~size_t(7u));
            for (size_t off = 0u; off + 8u <= byte_count; off += stride) {
                uint64_t w = 0ull;
                std::memcpy(&w, p + off, sizeof(w));
                h = (h ^ w) * 1099511628211ull;
            }
            for (size_t off = byte_count - 64u; off + 8u <= byte_count; off += 8u) {
                uint64_t w = 0ull;
                std::memcpy(&w, p + off, sizeof(w));
                h = (h ^ w) * 1099511628211ull;
            }
        } else {
            size_t i = 0u;
            for (; i + 8u <= byte_count; i += 8u) {
                uint64_t w = 0ull;
                std::memcpy(&w, p + i, sizeof(w));
                h = (h ^ w) * 1099511628211ull;
            }
            for (; i < byte_count; ++i) {
                h ^= static_cast<uint64_t>(p[i]);
                h *= 1099511628211ull;
            }
        }
        out_hash = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!IsReadableGuestVaRange(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src)), byte_count)) {
        return false;
    }
    const auto* p = static_cast<const uint8_t*>(src);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0u; i < byte_count; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    out_hash = h;
    return true;
#endif
}

D3D12_HEAP_FLAGS SanitizeXboxHeapFlags(D3D12_HEAP_FLAGS flags) noexcept {
    constexpr uint32_t kMaskOut = static_cast<uint32_t>(D3D12_HEAP_FLAG_ALLOW_DISPLAY);
    return static_cast<D3D12_HEAP_FLAGS>(static_cast<uint32_t>(flags) & ~kMaskOut);
}

// Sanitize zero-initialized D3D12_DEPTH_STENCIL_DESC enum fields (valid D3D12_COMPARISON_FUNC
// and D3D12_STENCIL_OP values are 1..8). Strict IHVs such as Intel Arc Battlemage can reject
// or fault on PSOs containing invalid 0 enum values even when DepthEnable/StencilEnable are FALSE.
bool SanitizeDepthStencilOp(D3D12_DEPTH_STENCILOP_DESC& op) noexcept {
    bool changed = false;
    if (static_cast<uint32_t>(op.StencilFailOp) == 0u) {
        op.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        changed = true;
    }
    if (static_cast<uint32_t>(op.StencilDepthFailOp) == 0u) {
        op.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        changed = true;
    }
    if (static_cast<uint32_t>(op.StencilPassOp) == 0u) {
        op.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        changed = true;
    }
    if (static_cast<uint32_t>(op.StencilFunc) == 0u) {
        op.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        changed = true;
    }
    return changed;
}

bool SanitizeDepthStencilDesc(D3D12_DEPTH_STENCIL_DESC& ds) noexcept {
    bool changed = false;
    if (static_cast<uint32_t>(ds.DepthFunc) == 0u) {
        ds.DepthFunc = ds.DepthEnable ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        changed = true;
    }
    if (SanitizeDepthStencilOp(ds.FrontFace)) changed = true;
    if (SanitizeDepthStencilOp(ds.BackFace)) changed = true;
    return changed;
}

bool SanitizeDepthStencilDesc1(D3D12_DEPTH_STENCIL_DESC1& ds) noexcept {
    bool changed = false;
    if (static_cast<uint32_t>(ds.DepthFunc) == 0u) {
        ds.DepthFunc = ds.DepthEnable ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        changed = true;
    }
    if (SanitizeDepthStencilOp(ds.FrontFace)) changed = true;
    if (SanitizeDepthStencilOp(ds.BackFace)) changed = true;
    return changed;
}

D3D12_BLEND SanitizeAlphaBlendFactor(D3D12_BLEND b, D3D12_BLEND zero_fallback) noexcept {
    const uint32_t v = static_cast<uint32_t>(b);
    if (v < 1u || v > 19u) return zero_fallback;
    switch (b) {
        case D3D12_BLEND_SRC_COLOR:      return D3D12_BLEND_SRC_ALPHA;
        case D3D12_BLEND_INV_SRC_COLOR:  return D3D12_BLEND_INV_SRC_ALPHA;
        case D3D12_BLEND_DEST_COLOR:     return D3D12_BLEND_DEST_ALPHA;
        case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
        case D3D12_BLEND_SRC1_COLOR:     return D3D12_BLEND_SRC1_ALPHA;
        case D3D12_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_ALPHA;
        default:                         return b;
    }
}

bool IsIntegerDxgiFormatForLogicOp(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
            return true;
        default:
            return false;
    }
}

void SanitizeGraphicsPsoDescForDesktop(D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpso) noexcept {
    gpso.StreamOutput = {};
    gpso.CachedPSO = {};
    gpso.NodeMask = 0u;
    gpso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (static_cast<uint32_t>(gpso.IBStripCutValue) >
        static_cast<uint32_t>(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)) {
        gpso.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    }
    if (gpso.SampleDesc.Count == 0u || gpso.SampleDesc.Count > 8u) {
        gpso.SampleDesc = {1u, 0u};
    } else if (gpso.SampleDesc.Count == 1u) {
        gpso.SampleDesc.Quality = 0u;
    }
    if (gpso.SampleMask == 0u) {
        gpso.SampleMask = UINT_MAX;
    }
    const uint32_t topo = static_cast<uint32_t>(gpso.PrimitiveTopologyType);
    if (topo < 1u || topo > 4u ||
        (gpso.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
         gpso.HS.BytecodeLength == 0u && gpso.DS.BytecodeLength == 0u)) {
        gpso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    if (gpso.RasterizerState.FillMode != D3D12_FILL_MODE_WIREFRAME &&
        gpso.RasterizerState.FillMode != D3D12_FILL_MODE_SOLID) {
        gpso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    }
    const uint32_t cull = static_cast<uint32_t>(gpso.RasterizerState.CullMode);
    if (cull < 1u || cull > 3u) {
        gpso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    gpso.RasterizerState.FrontCounterClockwise = gpso.RasterizerState.FrontCounterClockwise ? TRUE : FALSE;
    gpso.RasterizerState.DepthClipEnable = gpso.RasterizerState.DepthClipEnable ? TRUE : FALSE;
    gpso.RasterizerState.MultisampleEnable = gpso.RasterizerState.MultisampleEnable ? TRUE : FALSE;
    gpso.RasterizerState.AntialiasedLineEnable = gpso.RasterizerState.AntialiasedLineEnable ? TRUE : FALSE;
    if (!std::isfinite(gpso.RasterizerState.DepthBiasClamp)) {
        gpso.RasterizerState.DepthBiasClamp = 0.0f;
    }
    if (!std::isfinite(gpso.RasterizerState.SlopeScaledDepthBias)) {
        gpso.RasterizerState.SlopeScaledDepthBias = 0.0f;
    }
    if (gpso.RasterizerState.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF &&
        gpso.RasterizerState.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON) {
        gpso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
    if (gpso.RasterizerState.ForcedSampleCount != 0u &&
        gpso.RasterizerState.ForcedSampleCount != 1u &&
        gpso.RasterizerState.ForcedSampleCount != 2u &&
        gpso.RasterizerState.ForcedSampleCount != 4u &&
        gpso.RasterizerState.ForcedSampleCount != 8u &&
        gpso.RasterizerState.ForcedSampleCount != 16u) {
        gpso.RasterizerState.ForcedSampleCount = 0u;
    }
    gpso.BlendState.AlphaToCoverageEnable = gpso.BlendState.AlphaToCoverageEnable ? TRUE : FALSE;
    gpso.BlendState.IndependentBlendEnable = gpso.BlendState.IndependentBlendEnable ? TRUE : FALSE;
    for (UINT r = 0u; r < 8u; ++r) {
        auto& rt = gpso.BlendState.RenderTarget[r];
        rt.BlendEnable = rt.BlendEnable ? TRUE : FALSE;
        rt.LogicOpEnable = rt.LogicOpEnable ? TRUE : FALSE;
        const DXGI_FORMAT rfmt = (r < gpso.NumRenderTargets) ? gpso.RTVFormats[r] : DXGI_FORMAT_UNKNOWN;
        if (rt.LogicOpEnable &&
            (rt.BlendEnable || gpso.BlendState.IndependentBlendEnable || !IsIntegerDxgiFormatForLogicOp(rfmt))) {
            rt.LogicOpEnable = FALSE;
        }
        const uint32_t sb = static_cast<uint32_t>(rt.SrcBlend);
        const uint32_t db = static_cast<uint32_t>(rt.DestBlend);
        const uint32_t bo = static_cast<uint32_t>(rt.BlendOp);
        const uint32_t boa = static_cast<uint32_t>(rt.BlendOpAlpha);
        if (sb < 1u || sb > 19u) rt.SrcBlend = D3D12_BLEND_ONE;
        if (db < 1u || db > 19u) rt.DestBlend = D3D12_BLEND_ZERO;
        if (bo < 1u || bo > 5u) rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = SanitizeAlphaBlendFactor(rt.SrcBlendAlpha, D3D12_BLEND_ONE);
        rt.DestBlendAlpha = SanitizeAlphaBlendFactor(rt.DestBlendAlpha, D3D12_BLEND_ZERO);
        if (boa < 1u || boa > 5u) rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (!rt.LogicOpEnable || static_cast<uint32_t>(rt.LogicOp) > 15u) {
            rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        }
        rt.RenderTargetWriteMask &= 0x0Fu;
    }
    gpso.DepthStencilState.DepthEnable = gpso.DepthStencilState.DepthEnable ? TRUE : FALSE;
    gpso.DepthStencilState.StencilEnable = gpso.DepthStencilState.StencilEnable ? TRUE : FALSE;
    (void)SanitizeDepthStencilDesc(gpso.DepthStencilState);
    if (gpso.DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO &&
        gpso.DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL) {
        gpso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    }
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

bool MapGfx10SwizzleModeToXgTileMode(uint32_t xbox_sw, bool is_3d, XG_TILE_MODE& out_mode) noexcept {
    switch (xbox_sw) {
        case 0u:
            out_mode = XG_TILE_MODE_LINEAR;
            return true;
        case 1u:
            if (is_3d) return false;
            out_mode = XG_TILE_MODE_2D_THIN_256B_S;
            return true;
        case 2u:
            if (is_3d) return false;
            out_mode = XG_TILE_MODE_2D_THIN_256B_D;
            return true;
        case 3u:
            if (is_3d) return false;
            out_mode = XG_TILE_MODE_2D_THIN_256B_R;
            return true;
        case 4u:
        case 20u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_4KB_S : XG_TILE_MODE_2D_THIN_4KB_Z;
            return true;
        case 5u:
        case 21u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_4KB_S : XG_TILE_MODE_2D_THIN_4KB_S;
            return true;
        case 6u:
        case 22u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_4KB_S : XG_TILE_MODE_2D_THIN_4KB_D;
            return true;
        case 7u:
        case 23u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_4KB_S : XG_TILE_MODE_2D_THIN_4KB_R;
            return true;
        case 8u:
        case 12u:
        case 16u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_Z : XG_TILE_MODE_2D_THIN_64KB_Z;
            return true;
        case 9u:
        case 13u:
        case 17u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S : XG_TILE_MODE_2D_THIN_64KB_S;
            return true;
        case 10u:
        case 14u:
        case 18u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S : XG_TILE_MODE_2D_THIN_64KB_D;
            return true;
        case 11u:
        case 15u:
        case 19u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S : XG_TILE_MODE_2D_THIN_64KB_R;
            return true;
        case 24u:
        case 28u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_Z_X : XG_TILE_MODE_2D_THIN_64KB_Z_X;
            return true;
        case 25u:
        case 29u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S_X : XG_TILE_MODE_2D_THIN_64KB_S_X;
            return true;
        case 26u:
        case 30u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_S_X : XG_TILE_MODE_2D_THIN_64KB_D_X;
            return true;
        case 27u:
        case 31u:
            out_mode = is_3d ? XG_TILE_MODE_3D_THICK_64KB_R_X : XG_TILE_MODE_2D_THIN_64KB_R_X;
            return true;
        default:
            return false;
    }
}

DXGI_FORMAT SanitizeXboxDxgiFormat(DXGI_FORMAT fmt) noexcept;

XG_TILE_MODE ResolveRetailXboxTileMode(const D3D12_RESOURCE_DESC& desc) noexcept {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return XG_TILE_MODE_LINEAR;
    }
    const bool is_3d = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    const uint32_t raw = static_cast<uint32_t>(desc.Layout);
    if (raw == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || raw == D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) {
        return XG_TILE_MODE_LINEAR;
    }
    if (!is_3d && desc.Height == 1u && desc.DepthOrArraySize <= 1u && desc.MipLevels <= 1u) {
        return XG_TILE_MODE_LINEAR;
    }
    if (raw > D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) {
        XG_TILE_MODE mapped = XG_TILE_MODE_LINEAR;
        if (MapGfx10SwizzleModeToXgTileMode(raw - D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE, is_3d, mapped)) {
            return mapped;
        }
    }
    if (raw >= 4u && raw <= 31u) {
        XG_TILE_MODE mapped = XG_TILE_MODE_LINEAR;
        if (MapGfx10SwizzleModeToXgTileMode(raw, is_3d, mapped)) {
            return mapped;
        }
    }
    const DXGI_FORMAT san_fmt = MapTypelessFormatForSrv(SanitizeXboxDxgiFormat(desc.Format));
    if (raw == D3D12_TEXTURE_LAYOUT_UNKNOWN) {
        // Single-mip R8 / R8G8 / A8 surfaces with UNKNOWN layout (e.g. Scaleform glyph cache & Bink2 YUV planes)
        // are CPU-written linear surfaces; all multi-mip or BC/RGBA textures use optimal GFX10 tiling.
        if (!is_3d && desc.MipLevels <= 1u &&
            (san_fmt == DXGI_FORMAT_R8_UNORM || san_fmt == DXGI_FORMAT_A8_UNORM ||
             san_fmt == DXGI_FORMAT_R8G8_UNORM)) {
            return XG_TILE_MODE_LINEAR;
        }
        if (is_3d) {
            if (desc.MipLevels <= 1u && desc.Width <= 64u &&
                desc.Height <= 64u && desc.DepthOrArraySize <= 64u) {
                return XG_TILE_MODE_LINEAR;
            }
            return XG_TILE_MODE_3D_THICK_64KB_S;
        }
        const uint32_t h = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) ? 1u : desc.Height;
        return openscarlett::xg::ComputeOptimalTileMode(
            static_cast<uint32_t>(san_fmt), static_cast<uint32_t>(desc.Width), h, XG_BIND_SHADER_RESOURCE);
    }
    return is_3d ? XG_TILE_MODE_3D_THICK_64KB_S : XG_TILE_MODE_2D_THIN_64KB_S;
}

uint32_t RetailLog2Pow2(uint32_t v) noexcept {
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

struct RetailGfx10MipInfo2D {
    uint64_t macro_block_offset = 0ull;
    uint32_t pitch_elems = 1u;
    uint32_t height_elems = 1u;
    uint32_t width_elems = 1u;
    uint32_t raw_height_elems = 1u;
    uint32_t mip_tail_coord_x = 0u;
    uint32_t mip_tail_coord_y = 0u;
    bool in_mip_tail = false;
};

struct RetailGfx10Layout2D {
    uint64_t slice_size_bytes = 0ull;
    uint64_t surf_size_bytes = 0ull;
    uint32_t block_size_bytes = 65536u;
    uint32_t block_size_log2 = 16u;
    uint32_t block_w = 1u;
    uint32_t block_h = 1u;
    uint32_t micro_w = 1u;
    uint32_t micro_h = 1u;
    uint32_t bpe = 4u;
    uint32_t bpe_log2 = 2u;
    uint32_t first_mip_in_tail = 0u;
    RetailGfx10MipInfo2D mips[XG_MAX_MIP_LEVELS]{};
};

bool ComputeRetailGfx10Layout2D(
    const XG_TEXTURE2D_DESC& desc,
    const XG_RESOURCE_LAYOUT& xgl,
    RetailGfx10Layout2D& out) noexcept {
    const uint32_t bpe = xgl.FormatInfo.bytes_per_block;
    if (bpe != 1u && bpe != 2u && bpe != 4u && bpe != 8u && bpe != 16u) return false;
    const uint32_t num_mips = std::clamp<uint32_t>(desc.MipLevels ? desc.MipLevels : 1u, 1u, XG_MAX_MIP_LEVELS);
    const uint32_t num_slices = std::max<uint32_t>(1u, desc.ArraySize);

    uint32_t block_bytes = xgl.Mips[0].TileSizeBytes;
    switch (desc.TileMode) {
        case XG_TILE_MODE_2D_THIN_256B_S:
        case XG_TILE_MODE_2D_THIN_256B_D:
            block_bytes = 256u;
            break;
        case XG_TILE_MODE_2D_THIN_4KB_S:
        case XG_TILE_MODE_2D_THIN_4KB_D:
            block_bytes = 4096u;
            break;
        case XG_TILE_MODE_2D_THIN_64KB_S:
        case XG_TILE_MODE_2D_THIN_64KB_D:
        case XG_TILE_MODE_2D_THIN_64KB_S_X:
        case XG_TILE_MODE_2D_THIN_64KB_D_X:
        case XG_TILE_MODE_2D_THIN_64KB_R_X:
            block_bytes = 65536u;
            break;
        default:
            return false;
    }

    uint32_t micro_w = 1u, micro_h = 1u, bpe_log2 = 0u;
    switch (bpe) {
        case 1u:  micro_w = 16u; micro_h = 16u; bpe_log2 = 0u; break;
        case 2u:  micro_w = 16u; micro_h = 8u;  bpe_log2 = 1u; break;
        case 4u:  micro_w = 8u;  micro_h = 8u;  bpe_log2 = 2u; break;
        case 8u:  micro_w = 8u;  micro_h = 4u;  bpe_log2 = 3u; break;
        case 16u: micro_w = 4u;  micro_h = 4u;  bpe_log2 = 4u; break;
    }

    const uint32_t block_log2 = RetailLog2Pow2(block_bytes);
    const uint32_t macro_bits = (block_bytes == 65536u) ? 4u : ((block_bytes == 4096u) ? 2u : 0u);
    const uint32_t block_w = micro_w << macro_bits;
    const uint32_t block_h = micro_h << macro_bits;
    const uint32_t blk_px_w = std::max<uint32_t>(1u, xgl.FormatInfo.block_width);
    const uint32_t blk_px_h = std::max<uint32_t>(1u, xgl.FormatInfo.block_height);

    auto align_pow2 = [](uint32_t v, uint32_t a) noexcept -> uint32_t {
        return (v + a - 1u) & ~(a - 1u);
    };

    out = RetailGfx10Layout2D{};
    out.block_size_bytes = block_bytes;
    out.block_size_log2 = block_log2;
    out.block_w = block_w;
    out.block_h = block_h;
    out.micro_w = micro_w;
    out.micro_h = micro_h;
    out.bpe = bpe;
    out.bpe_log2 = bpe_log2;

    if (num_mips == 1u) {
        const uint32_t w_el = std::max<uint32_t>(1u, (desc.Width + blk_px_w - 1u) / blk_px_w);
        const uint32_t h_el = std::max<uint32_t>(1u, (desc.Height + blk_px_h - 1u) / blk_px_h);
        const uint32_t pitch = align_pow2(w_el, block_w);
        const uint32_t height = align_pow2(h_el, block_h);
        out.first_mip_in_tail = 1u;
        out.slice_size_bytes = static_cast<uint64_t>(pitch) * height * bpe;
        out.surf_size_bytes = out.slice_size_bytes * num_slices;
        out.mips[0].macro_block_offset = 0ull;
        out.mips[0].pitch_elems = pitch;
        out.mips[0].height_elems = height;
        out.mips[0].width_elems = w_el;
        out.mips[0].raw_height_elems = h_el;
        out.mips[0].in_mip_tail = false;
        return true;
    }

    // Exact AMD GFX10 2D Thin mip-chain & mip-tail layout (MIT Mesa addrlib gfx10addrlib.cpp):
    // - maxMipsInTail = (blockSizeLog2 <= 11) ? 1 : (blockSizeLog2 - 4)  (12 for 64KB, 8 for 4KB)
    // - tailMaxDim = { blockWidth / 2, blockHeight } for even blockSizeLog2 (64KB=16, 4KB=12, 256B=8)
    // - Offset 0..blockSize is the mip-tail block (when firstMipInTail < numMipLevels), followed by
    //   non-tail mips in REVERSE order: Mip(firstMipInTail - 1) .. Mip(0).
    const uint32_t max_mips_in_tail = (block_log2 <= 11u) ? 1u : (block_log2 - 4u);
    const uint32_t tail_max_w = std::max<uint32_t>(micro_w, block_w >> 1u);
    const uint32_t tail_max_h = block_h;

    uint64_t mip_slice_size[XG_MAX_MIP_LEVELS]{};
    uint32_t first_mip_in_tail = num_mips;
    uint64_t mip_chain_slice_bytes = 0ull;

    for (uint32_t i = 0u; i < num_mips; ++i) {
        const uint32_t mip_px_w = std::max<uint32_t>(1u, desc.Width >> i);
        const uint32_t mip_px_h = std::max<uint32_t>(1u, desc.Height >> i);
        const uint32_t w_el = std::max<uint32_t>(1u, (mip_px_w + blk_px_w - 1u) / blk_px_w);
        const uint32_t h_el = std::max<uint32_t>(1u, (mip_px_h + blk_px_h - 1u) / blk_px_h);
        out.mips[i].width_elems = w_el;
        out.mips[i].raw_height_elems = h_el;

        if (! (w_el <= tail_max_w && h_el <= tail_max_h && (num_mips - i) <= max_mips_in_tail)) {
            const uint32_t pitch = align_pow2(w_el, block_w);
            const uint32_t height = align_pow2(h_el, block_h);
            const uint64_t sz = static_cast<uint64_t>(pitch) * height * bpe;
            mip_slice_size[i] = sz;
            mip_chain_slice_bytes += sz;
            out.mips[i].pitch_elems = pitch;
            out.mips[i].height_elems = height;
            out.mips[i].in_mip_tail = false;
        } else {
            first_mip_in_tail = i;
            mip_chain_slice_bytes += block_bytes;
            for (uint32_t j = i + 1u; j < num_mips; ++j) {
                const uint32_t j_px_w = std::max<uint32_t>(1u, desc.Width >> j);
                const uint32_t j_px_h = std::max<uint32_t>(1u, desc.Height >> j);
                out.mips[j].width_elems = std::max<uint32_t>(1u, (j_px_w + blk_px_w - 1u) / blk_px_w);
                out.mips[j].raw_height_elems = std::max<uint32_t>(1u, (j_px_h + blk_px_h - 1u) / blk_px_h);
            }
            break;
        }
    }

    out.first_mip_in_tail = first_mip_in_tail;
    out.slice_size_bytes = mip_chain_slice_bytes;
    out.surf_size_bytes = mip_chain_slice_bytes * num_slices;

    uint64_t macro_off = (first_mip_in_tail < num_mips) ? static_cast<uint64_t>(block_bytes) : 0ull;
    for (int32_t i = static_cast<int32_t>(first_mip_in_tail) - 1; i >= 0; --i) {
        out.mips[i].macro_block_offset = macro_off;
        macro_off += mip_slice_size[i];
    }

    uint32_t tail_pitch = tail_max_w;
    uint32_t tail_height = tail_max_h;
    for (uint32_t i = first_mip_in_tail; i < num_mips; ++i) {
        const uint32_t m = max_mips_in_tail - 1u - (i - first_mip_in_tail);
        const uint32_t mip_off = (m > 6u) ? (16u << m) : (m << 8u);
        uint32_t mip_x = ((mip_off >> 9u)  & 1u)  |
                         ((mip_off >> 10u) & 2u)  |
                         ((mip_off >> 11u) & 4u)  |
                         ((mip_off >> 12u) & 8u)  |
                         ((mip_off >> 13u) & 16u) |
                         ((mip_off >> 14u) & 32u);
        uint32_t mip_y = ((mip_off >> 8u)  & 1u)  |
                         ((mip_off >> 9u)  & 2u)  |
                         ((mip_off >> 10u) & 4u)  |
                         ((mip_off >> 11u) & 8u)  |
                         ((mip_off >> 12u) & 16u) |
                         ((mip_off >> 13u) & 32u);
        if ((block_log2 & 1u) != 0u) {
            std::swap(mip_x, mip_y);
            if ((bpe_log2 & 1u) != 0u) {
                mip_y = (mip_y << 1u) | (mip_x & 1u);
                mip_x >>= 1u;
            }
        }
        out.mips[i].in_mip_tail = true;
        out.mips[i].macro_block_offset = 0ull;
        out.mips[i].pitch_elems = tail_pitch;
        out.mips[i].height_elems = tail_height;
        out.mips[i].mip_tail_coord_x = mip_x * micro_w;
        out.mips[i].mip_tail_coord_y = mip_y * micro_h;
        tail_pitch = std::max<uint32_t>(tail_pitch >> 1u, micro_w);
        tail_height = std::max<uint32_t>(tail_height >> 1u, micro_h);
    }
    return true;
}

uint32_t ComputeGfx10Thin2dInTileByteOffset(
    uint32_t x,
    uint32_t y,
    uint32_t bpe,
    uint32_t block_bytes,
    bool is_display_or_rot) noexcept {
    const uint32_t x0 = (x >> 0u) & 1u, x1 = (x >> 1u) & 1u, x2 = (x >> 2u) & 1u, x3 = (x >> 3u) & 1u;
    const uint32_t x4 = (x >> 4u) & 1u, x5 = (x >> 5u) & 1u, x6 = (x >> 6u) & 1u, x7 = (x >> 7u) & 1u;
    const uint32_t y0 = (y >> 0u) & 1u, y1 = (y >> 1u) & 1u, y2 = (y >> 2u) & 1u, y3 = (y >> 3u) & 1u;
    const uint32_t y4 = (y >> 4u) & 1u, y5 = (y >> 5u) & 1u, y6 = (y >> 6u) & 1u, y7 = (y >> 7u) & 1u;

    uint32_t nib01 = 0u, nib2 = 0u, nib3 = 0u;
    switch (bpe) {
        case 1u:
            nib01 = is_display_or_rot
                        ? ((x0 << 0u) | (x1 << 1u) | (x2 << 2u) | (y1 << 3u) |
                           (y0 << 4u) | (y2 << 5u) | (x3 << 6u) | (y3 << 7u))
                        : ((x0 << 0u) | (x1 << 1u) | (x2 << 2u) | (x3 << 3u) |
                           (y0 << 4u) | (y1 << 5u) | (y2 << 6u) | (y3 << 7u));
            nib2 = (y4 << 0u) | (x4 << 1u) | (y5 << 2u) | (x5 << 3u);
            nib3 = (y6 << 0u) | (x6 << 1u) | (y7 << 2u) | (x7 << 3u);
            break;
        case 2u:
            nib01 = (x0 << 1u) | (x1 << 2u) | (x2 << 3u) |
                    (y0 << 4u) | (y1 << 5u) | (y2 << 6u) | (x3 << 7u);
            nib2 = (y3 << 0u) | (x4 << 1u) | (y4 << 2u) | (x5 << 3u);
            nib3 = (y5 << 0u) | (x6 << 1u) | (y6 << 2u) | (x7 << 3u);
            break;
        case 4u:
            nib01 = is_display_or_rot
                        ? ((x0 << 2u) | (x1 << 3u) | (y0 << 4u) | (y1 << 5u) | (x2 << 6u) | (y2 << 7u))
                        : ((x0 << 2u) | (x1 << 3u) | (y0 << 4u) | (y1 << 5u) | (y2 << 6u) | (x2 << 7u));
            nib2 = (y3 << 0u) | (x3 << 1u) | (y4 << 2u) | (x4 << 3u);
            nib3 = (y5 << 0u) | (x5 << 1u) | (y6 << 2u) | (x6 << 3u);
            break;
        case 8u:
            nib01 = is_display_or_rot
                        ? ((x0 << 3u) | (y0 << 4u) | (x1 << 5u) | (x2 << 6u) | (y1 << 7u))
                        : ((x0 << 3u) | (y0 << 4u) | (y1 << 5u) | (x1 << 6u) | (x2 << 7u));
            nib2 = (y2 << 0u) | (x3 << 1u) | (y3 << 2u) | (x4 << 3u);
            nib3 = (y4 << 0u) | (x5 << 1u) | (y5 << 2u) | (x6 << 3u);
            break;
        case 16u:
        default:
            nib01 = is_display_or_rot
                        ? ((x0 << 4u) | (y0 << 5u) | (x1 << 6u) | (y1 << 7u))
                        : ((y0 << 4u) | (y1 << 5u) | (x0 << 6u) | (x1 << 7u));
            nib2 = (y2 << 0u) | (x2 << 1u) | (y3 << 2u) | (x3 << 3u);
            nib3 = (y4 << 0u) | (x4 << 1u) | (y5 << 2u) | (x5 << 3u);
            break;
    }

    if (block_bytes == 256u) return nib01;
    if (block_bytes == 4096u) return nib01 | (nib2 << 8u);
    return nib01 | (nib2 << 8u) | (nib3 << 12u);
}

// Clean-room hardware GFX10 RbPlus 2D Thin Standard/Displayable/Rotated untiler for cooked
// Xbox Series X|S UMA textures (exact per-BPE swizzle patterns, reverse mip chain, and mip-tail).
bool UntileRetailGfx10Surface2D(
    const XG_TEXTURE2D_DESC& desc,
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t slice_index,
    const uint8_t* src_tiled,
    uint64_t src_avail_bytes,
    uint8_t* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept {
    if (!src_tiled || !dst_linear || mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return false;
    }
    if (desc.TileMode == XG_TILE_MODE_2D_THIN_4KB_Z ||
        desc.TileMode == XG_TILE_MODE_2D_THIN_64KB_Z ||
        desc.TileMode == XG_TILE_MODE_2D_THIN_64KB_Z_X) {
        return openscarlett::xg::UntileSurface2DPlane(
            desc, 0u, mip_level, slice_index, src_tiled, src_avail_bytes, dst_linear, dst_row_pitch_bytes);
    }

    RetailGfx10Layout2D rl{};
    if (!ComputeRetailGfx10Layout2D(desc, layout, rl)) {
        return openscarlett::xg::UntileSurface2DPlane(
            desc, 0u, mip_level, slice_index, src_tiled, src_avail_bytes, dst_linear, dst_row_pitch_bytes);
    }

    const RetailGfx10MipInfo2D& rm = rl.mips[mip_level];
    const uint32_t bpe = rl.bpe;
    const uint32_t min_row = rm.width_elems * bpe;
    if (dst_row_pitch_bytes < min_row) return false;

    uint64_t slice_base = static_cast<uint64_t>(slice_index) * rl.slice_size_bytes + rm.macro_block_offset;
    // Fallback if guest allocated only single-mip Mip 0 bytes despite MipLevels > 1 metadata.
    if (mip_level == 0u && slice_index == 0u && slice_base >= src_avail_bytes && src_avail_bytes >= 256ull) {
        slice_base = 0ull;
    }
    if (slice_base >= src_avail_bytes) return false;

    const bool is_display_or_rot =
        (desc.TileMode == XG_TILE_MODE_2D_THIN_256B_D ||
         desc.TileMode == XG_TILE_MODE_2D_THIN_4KB_D ||
         desc.TileMode == XG_TILE_MODE_2D_THIN_64KB_D ||
         desc.TileMode == XG_TILE_MODE_2D_THIN_64KB_D_X ||
         desc.TileMode == XG_TILE_MODE_2D_THIN_64KB_R_X);

    const uint32_t tile_w = rl.block_w;
    const uint32_t tile_h = rl.block_h;
    const uint32_t tile_bytes = rl.block_size_bytes;
    const uint32_t tiles_x = rm.in_mip_tail ? 1u : std::max<uint32_t>(1u, rm.pitch_elems / tile_w);

    for (uint32_t y = 0u; y < rm.raw_height_elems; ++y) {
        uint8_t* dst_row = dst_linear + static_cast<uint64_t>(y) * dst_row_pitch_bytes;
        const uint32_t eff_y = y + rm.mip_tail_coord_y;
        const uint32_t ty = rm.in_mip_tail ? 0u : (eff_y / tile_h);
        const uint32_t iy = rm.in_mip_tail ? eff_y : (eff_y % tile_h);
        for (uint32_t x = 0u; x < rm.width_elems; ++x) {
            const uint32_t eff_x = x + rm.mip_tail_coord_x;
            const uint32_t tx = rm.in_mip_tail ? 0u : (eff_x / tile_w);
            const uint32_t ix = rm.in_mip_tail ? eff_x : (eff_x % tile_w);
            const uint32_t in_tile_off =
                ComputeGfx10Thin2dInTileByteOffset(ix, iy, bpe, tile_bytes, is_display_or_rot);
            const uint64_t src_off =
                slice_base + static_cast<uint64_t>(ty * tiles_x + tx) * tile_bytes + in_tile_off;
            if (src_off + bpe <= src_avail_bytes) {
                std::memcpy(dst_row + static_cast<size_t>(x) * bpe, src_tiled + src_off, bpe);
            } else {
                std::memset(dst_row + static_cast<size_t>(x) * bpe, 0, bpe);
            }
        }
    }
    return true;
}

// Translate Xbox Scarlett-exclusive DXGI_FORMAT values (116..121) that desktop Windows
// D3D12 rejects with E_INVALIDARG into equivalent desktop DXGI formats.
DXGI_FORMAT SanitizeXboxDxgiFormat(DXGI_FORMAT fmt) noexcept {
    switch (static_cast<uint32_t>(fmt)) {
        case 115u:  // DXGI_FORMAT_B4G4R4A4_UNORM (no desktop RT/UAV support -> BGRA8)
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case 116u:  // DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT
        case 117u:  // DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 118u:  // DXGI_FORMAT_D16_UNORM_S8_UINT
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case 119u:  // DXGI_FORMAT_R16_UNORM_X8_TYPELESS
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case 120u:  // DXGI_FORMAT_X16_TYPELESS_G8_UINT
            return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        case 121u:  // DXGI_FORMAT_R16G8_TYPELESS (if emitted)
            return DXGI_FORMAT_R24G8_TYPELESS;
        case 0xFFu:  // Retail Scarlett 8-bit sentinel for "unused / unknown format"
            return DXGI_FORMAT_UNKNOWN;
        default:
            return fmt;
    }
}

DXGI_FORMAT MapTypelessFormatForDsv(DXGI_FORMAT fmt) noexcept {
    fmt = SanitizeXboxDxgiFormat(fmt);
    switch (fmt) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:
            return DXGI_FORMAT_D16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default:
            return fmt;
    }
}

DXGI_FORMAT SanitizePsoDsvFormat(DXGI_FORMAT fmt) noexcept {
    if (fmt == DXGI_FORMAT_UNKNOWN || static_cast<uint32_t>(fmt) == 0xFFu) {
        return DXGI_FORMAT_UNKNOWN;
    }
    const DXGI_FORMAT mapped = MapTypelessFormatForDsv(fmt);
    switch (mapped) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D16_UNORM:
            return mapped;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsDepthOnlyDsvFormat(DXGI_FORMAT fmt) noexcept {
    return fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_D16_UNORM;
}

D3D12_RESOURCE_FLAGS SanitizeResourceFlagsForDimension(
    D3D12_RESOURCE_DIMENSION dim,
    D3D12_RESOURCE_FLAGS flags,
    UINT sample_count,
    bool* out_stripped = nullptr) noexcept {
    // Desktop D3D12 standard flags are bits 0..7 (0xFF), plus RAYTRACING_ACCELERATION_STRUCTURE (0x100) on buffers.
    // Xbox console-private flags (e.g. ALLOW_DCC, FORCE_64KB, TEXTURE_METADATA, etc.) live in upper bits.
    const uint32_t allowed_mask =
        (dim == D3D12_RESOURCE_DIMENSION_BUFFER) ? 0x000001FFu : 0x000000FFu;
    uint32_t raw = static_cast<uint32_t>(flags);
    uint32_t clean = raw & allowed_mask;

    if (dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        // Desktop D3D12 forbids DEPTH_STENCIL and SIMULTANEOUS_ACCESS on 3D textures (xvalkyrie c986c75).
        clean &= ~static_cast<uint32_t>(
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    }
    if ((clean & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0u) {
        // Desktop D3D12 forbids combining ALLOW_DEPTH_STENCIL with ALLOW_UNORDERED_ACCESS or
        // ALLOW_RENDER_TARGET (Xbox Series X permits DS+UAV for HTILE / UE5 Virtual Shadow Maps).
        clean &= ~(static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
                   static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    }
    if ((clean & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0u) {
        // DENY_SHADER_RESOURCE is only legal when ALLOW_DEPTH_STENCIL is set.
        clean &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    if ((clean & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0u ||
        sample_count > 1u) {
        clean &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    }
    if (out_stripped) {
        *out_stripped = (clean != raw);
    }
    return static_cast<D3D12_RESOURCE_FLAGS>(clean);
}

bool FormatForbidsDesktopUav(DXGI_FORMAT fmt) noexcept {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B5G6R5_UNORM ||
           fmt == DXGI_FORMAT_B5G5R5A1_UNORM ||
           fmt == DXGI_FORMAT_B4G4R4A4_UNORM ||
           fmt == DXGI_FORMAT_A8_UNORM ||
           (static_cast<uint32_t>(fmt) >= static_cast<uint32_t>(DXGI_FORMAT_BC1_TYPELESS) &&
            static_cast<uint32_t>(fmt) <= static_cast<uint32_t>(DXGI_FORMAT_BC5_SNORM)) ||
           (static_cast<uint32_t>(fmt) >= static_cast<uint32_t>(DXGI_FORMAT_BC6H_TYPELESS) &&
            static_cast<uint32_t>(fmt) <= static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM_SRGB));
}

D3D12_RESOURCE_DESC SanitizeXboxResourceDesc(const D3D12_RESOURCE_DESC& desc) noexcept {
    D3D12_RESOURCE_DESC out = desc;
    out.Format = SanitizeXboxDxgiFormat(out.Format);
    if (out.SampleDesc.Count == 0u) {
        out.SampleDesc.Count = 1u;
        out.SampleDesc.Quality = 0u;
    }
    out.Flags = SanitizeResourceFlagsForDimension(out.Dimension, desc.Flags, out.SampleDesc.Count);
    if (FormatForbidsDesktopUav(out.Format) || out.SampleDesc.Count > 1u) {
        out.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (static_cast<uint32_t>(out.Layout) > static_cast<uint32_t>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)) {
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    return out;
}

D3D12_RESOURCE_DESC1 SanitizeXboxResourceDesc1(const D3D12_RESOURCE_DESC1& desc) noexcept {
    D3D12_RESOURCE_DESC1 out = desc;
    out.Format = SanitizeXboxDxgiFormat(out.Format);
    if (out.SampleDesc.Count == 0u) {
        out.SampleDesc.Count = 1u;
        out.SampleDesc.Quality = 0u;
    }
    out.Flags = SanitizeResourceFlagsForDimension(out.Dimension, desc.Flags, out.SampleDesc.Count);
    if (FormatForbidsDesktopUav(out.Format) || out.SampleDesc.Count > 1u) {
        out.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (static_cast<uint32_t>(out.Layout) > static_cast<uint32_t>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)) {
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
void QueryConfiguredDisplayMode(uint32_t& out_w, uint32_t& out_h) noexcept {
    out_w = 1920u;
    out_h = 1080u;
    char cbuf[32]{};
    DWORD clen = ::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_TYPE", cbuf, sizeof(cbuf));
    if (clen == 0u || clen >= sizeof(cbuf)) {
        clen = ::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", cbuf, sizeof(cbuf));
    }
    if (clen > 0u && clen < sizeof(cbuf)) {
        std::string s(cbuf, clen);
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (s == "lockhart" || s == "series_s" || s == "series-s" || s == "3" || s == "s" || s == "8gb") {
            out_w = 1280u;
            out_h = 720u;
        }
    }
    char rbuf[64]{};
    const DWORD rlen = ::GetEnvironmentVariableA("OPENSCARLETT_DISPLAY_RES", rbuf, sizeof(rbuf));
    if (rlen > 0u && rlen < sizeof(rbuf)) {
        char* sep = nullptr;
        const unsigned long ew = std::strtoul(rbuf, &sep, 10);
        if (sep && (*sep == 'x' || *sep == 'X')) {
            const unsigned long eh = std::strtoul(sep + 1, nullptr, 10);
            if (ew >= 320ul && eh >= 180ul && ew <= 7680ul && eh <= 4320ul) {
                out_w = static_cast<uint32_t>(ew);
                out_h = static_cast<uint32_t>(eh);
            }
        }
    }
}

bool HasExplicitConsoleOrDisplayOverride() noexcept {
    char buf[64]{};
    if (::GetEnvironmentVariableA("OPENSCARLETT_DISPLAY_RES", buf, sizeof(buf)) > 0u) return true;
    if (::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_TYPE", buf, sizeof(buf)) > 0u) return true;
    if (::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", buf, sizeof(buf)) > 0u) return true;
    return false;
}

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
        uint32_t mw = 1920u, mh = 1080u;
        QueryConfiguredDisplayMode(mw, mh);
        pDesc->DesktopCoordinates = {0, 0, static_cast<LONG>(mw), static_cast<LONG>(mh)};
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
        uint32_t mw = 1920u, mh = 1080u;
        QueryConfiguredDisplayMode(mw, mh);
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc[0].Width = mw;
        pDesc[0].Height = mh;
        pDesc[0].RefreshRate = {60u, 1u};
        pDesc[0].Format = EnumFormat;
        *pNumModes = 1u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(
        const DXGI_MODE_DESC* pModeToMatch, DXGI_MODE_DESC* pClosestMatch, IUnknown*) override {
        if (!pClosestMatch) return E_INVALIDARG;
        uint32_t mw = 1920u, mh = 1080u;
        QueryConfiguredDisplayMode(mw, mh);
        if (pModeToMatch) {
            *pClosestMatch = *pModeToMatch;
        } else {
            std::memset(pClosestMatch, 0, sizeof(*pClosestMatch));
        }
        if (pClosestMatch->Width == 0u || HasExplicitConsoleOrDisplayOverride()) pClosestMatch->Width = mw;
        if (pClosestMatch->Height == 0u || HasExplicitConsoleOrDisplayOverride()) pClosestMatch->Height = mh;
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
    // Slot 19 (+0x98): IDXGIOutput1::GetDisplayModeList1 (PC)
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList1(
        DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes, DXGI_MODE_DESC1* pDesc) {
        (void)Flags;
        if (!pNumModes) return E_INVALIDARG;
        if (!pDesc) {
            *pNumModes = 1u;
            return S_OK;
        }
        if (*pNumModes == 0u) return DXGI_ERROR_MORE_DATA;
        uint32_t mw = 1920u, mh = 1080u;
        QueryConfiguredDisplayMode(mw, mh);
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc[0].Width = mw;
        pDesc[0].Height = mh;
        pDesc[0].RefreshRate = {60u, 1u};
        pDesc[0].Format = (EnumFormat == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_R8G8B8A8_UNORM : EnumFormat;
        pDesc[0].ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        pDesc[0].Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        pDesc[0].Stereo = FALSE;
        *pNumModes = 1u;
        return S_OK;
    }
    // Slot 20 (+0xA0): IDXGIOutput1::FindClosestMatchingMode1
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(
        const DXGI_MODE_DESC1* pModeToMatch, DXGI_MODE_DESC1* pClosestMatch, IUnknown*) {
        if (!pClosestMatch) return E_INVALIDARG;
        uint32_t mw = 1920u, mh = 1080u;
        QueryConfiguredDisplayMode(mw, mh);
        if (pModeToMatch) {
            *pClosestMatch = *pModeToMatch;
        } else {
            std::memset(pClosestMatch, 0, sizeof(*pClosestMatch));
        }
        if (pClosestMatch->Width == 0u || HasExplicitConsoleOrDisplayOverride()) pClosestMatch->Width = mw;
        if (pClosestMatch->Height == 0u || HasExplicitConsoleOrDisplayOverride()) pClosestMatch->Height = mh;
        if (pClosestMatch->RefreshRate.Numerator == 0u) pClosestMatch->RefreshRate = {60u, 1u};
        if (pClosestMatch->Format == DXGI_FORMAT_UNKNOWN) pClosestMatch->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return S_OK;
    }
    // Slot 21 (+0xA8): Xbox Scarlett IDXGIOutput::GetDisplayModeList1 / GetDisplayModeListX
    // (32-byte mode entries verified live on GRDK 2504/2510 game_scarlett_master.exe).
    virtual HRESULT STDMETHODCALLTYPE XboxGetDisplayModeListX(
        DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes, DXGI_MODE_DESC1* pDesc) {
        return GetDisplayModeList1(EnumFormat, Flags, pNumModes, pDesc);
    }
    // Slots 22..29: safe extended IDXGIOutput2..6 / Xbox output padding slots
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot22() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot23() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot24() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot25() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot26() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot27() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot28() { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE XboxOutputSlot29() { return S_OK; }

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
        if (Output == 0u && HasExplicitConsoleOrDisplayOverride()) {
            auto* vout = new (std::nothrow) VirtualDxgiOutput(this);
            if (!vout) return E_OUTOFMEMORY;
            *ppOutput = vout;
            return S_OK;
        }
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
            clean_desc = parent_->SanitizeAndClampResourceDesc1(*pDesc);
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
        ID3D12Heap* host_heap = UnwrapHeap(pHeap);
        D3D12_HEAP_DESC hd{};
        bool has_heap_desc = false;
        uint64_t heap_remaining_bytes = UINT64_MAX;
        if (host_heap) {
            hd = host_heap->GetDesc();
            has_heap_desc = true;
            heap_remaining_bytes = (hd.SizeInBytes > HeapOffset) ? (hd.SizeInBytes - HeapOffset) : 0ull;
        }
        D3D12_RESOURCE_DESC1 clean_desc{};
        const D3D12_RESOURCE_DESC1* desc_ptr = pDesc;
        bool should_promote_oversized = false;
        if (pDesc) {
            clean_desc = parent_->SanitizeAndClampResourceDesc1(*pDesc);
            desc_ptr = &clean_desc;
            if (has_heap_desc && clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(clean_desc);
                const D3D12_RESOURCE_ALLOCATION_INFO req_alloc =
                    parent_->host_dev_->GetResourceAllocationInfo(0u, 1u, &d0);
                const uint64_t eff_align = (req_alloc.Alignment > 0ull) ? req_alloc.Alignment : 65536ull;
                const bool size_exceeds_heap =
                    (req_alloc.SizeInBytes != UINT64_MAX && req_alloc.SizeInBytes > heap_remaining_bytes);
                const bool align_incompatible =
                    ((HeapOffset % eff_align) != 0ull) ||
                    (hd.Alignment > 0ull && eff_align > hd.Alignment && (hd.Alignment % eff_align) != 0ull);
                should_promote_oversized = size_exceeds_heap || align_incompatible;
            }
        }
        ID3D12Resource* host_res = nullptr;
        HRESULT hr = E_INVALIDARG;
        if (!should_promote_oversized) {
            hr = parent_->host_dev10_->CreatePlacedResource2(
                host_heap, HeapOffset, desc_ptr, InitialLayout, pOptimizedClearValue,
                NumCastableFormats, pCastableFormats,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
        bool promoted_oversized = false;
        D3D12_HEAP_PROPERTIES promo_hp{};
        if ((FAILED(hr) || !host_res) && desc_ptr &&
            desc_ptr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && has_heap_desc &&
            (should_promote_oversized || hr == E_INVALIDARG || hr == E_OUTOFMEMORY)) {
            promo_hp = hd.Properties;
            D3D12_RESOURCE_DESC1 promo_desc = *desc_ptr;
            promo_desc.Alignment = 0ull;
            hr = parent_->host_dev10_->CreateCommittedResource3(
                &promo_hp, D3D12_HEAP_FLAG_NONE, &promo_desc, InitialLayout,
                pOptimizedClearValue, nullptr, NumCastableFormats, pCastableFormats,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            if ((FAILED(hr) || !host_res) && promo_hp.Type != D3D12_HEAP_TYPE_DEFAULT) {
                promo_hp = D3D12_HEAP_PROPERTIES{};
                promo_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                hr = parent_->host_dev10_->CreateCommittedResource3(
                    &promo_hp, D3D12_HEAP_FLAG_NONE, &promo_desc, InitialLayout,
                    pOptimizedClearValue, nullptr, NumCastableFormats, pCastableFormats,
                    OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            }
            if (SUCCEEDED(hr) && host_res) {
                promoted_oversized = true;
                parent_->promoted_oversized_placed_resources_.fetch_add(1ull, std::memory_order_relaxed);
                RecordDev("ID3D12DeviceX::CreatePlacedResource2[PromotedOversized]", 77, S_OK);
            }
        }
        if (SUCCEEDED(hr) && host_res && ppvResource) {
            const D3D12_RESOURCE_DESC d0 = desc_ptr ? Desc1ToDesc0(*desc_ptr) : D3D12_RESOURCE_DESC{};
            if (desc_ptr) {
                const uint64_t est_bytes = parent_->EstimateResourceVramBytes(d0);
                const uint64_t sparse_va =
                    (parent_->sparse_window_iso_ && est_bytes > 0ull)
                        ? openscarlett::SparseVramAllocateTitleWindow(parent_->sparse_window_iso_, est_bytes)
                        : 0ull;
                parent_->TrackLiveResidencyObject(
                    static_cast<ID3D12Pageable*>(host_res),
                    est_bytes,
                    promoted_oversized ? promo_hp.Type : (has_heap_desc ? hd.Properties.Type : D3D12_HEAP_TYPE_DEFAULT),
                    /*is_placed_suballocation=*/!promoted_oversized,
                    /*spilled_to_l0=*/false,
                    sparse_va);
            }
            auto* proxy = new (std::nothrow) ResourceXProxy(
                parent_, host_res, desc_ptr ? &d0 : nullptr,
                promoted_oversized ? &promo_hp : nullptr, D3D12_HEAP_FLAG_NONE);
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
        bool had_cross_adapter = false;
        if (pDesc) {
            clean_desc = parent_->SanitizeAndClampResourceDesc(*pDesc);
            if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                clean_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN) {
                clean_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
            }
            had_cross_adapter =
                (clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
            desc_ptr = &clean_desc;
        }
        ID3D12Resource* host_res = nullptr;
        HRESULT hr = parent_->host_dev10_->CreateReservedResource2(
            desc_ptr, InitialLayout, pOptimizedClearValue, pProtectedSession,
            NumCastableFormats, pCastableFormats,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (had_cross_adapter) {
            if (SUCCEEDED(hr) && host_res) {
                parent_->reserved_cross_adapter_preserved_.fetch_add(1ull, std::memory_order_relaxed);
                RecordDev("ID3D12DeviceX::CreateReservedResource2[CrossAdapterPreserved]", 78, S_OK);
            } else if (desc_ptr) {
                D3D12_RESOURCE_DESC retry_desc = *desc_ptr;
                retry_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                    static_cast<uint32_t>(retry_desc.Flags) &
                    ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER));
                hr = parent_->host_dev10_->CreateReservedResource2(
                    &retry_desc, InitialLayout, pOptimizedClearValue, pProtectedSession,
                    NumCastableFormats, pCastableFormats,
                    OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
                if (SUCCEEDED(hr) && host_res) {
                    clean_desc = retry_desc;
                    parent_->reserved_cross_adapter_stripped_fallback_.fetch_add(1ull, std::memory_order_relaxed);
                    RecordDev("ID3D12DeviceX::CreateReservedResource2[CrossAdapterFallback]", 78, S_OK);
                }
            }
        }
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

        // Sprint 14: Initialize live VRAM Residency Manager profile, DXGI Adapter3 memory info,
        // and Sparse VRAM Window-0 isolator (Descriptor Heaps Window 0 vs Title Buffers Window 1+).
        sanitized_ihv_caps_ = openscarlett::SanitizeCrossIhvCapabilities(adapter_info_);
        sparse_window_iso_ = openscarlett::CreateSparseVramWindowIsolator();
        console_profile_ = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(
            adapter_info_.dedicated_video_memory_bytes, nullptr);
        (void)openscarlett::QueryHostAdapterVideoMemoryInfo(host_dev_, dxgi_mem_info_);
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
    UntrackLiveResidencyObject(static_cast<ID3D12Pageable*>(host_resource));
    sampler_feedback_mgr_.NotifyResourceDestroyed(host_resource);
    if (retail_scarlett_abi_) {
        {
            std::lock_guard<std::mutex> lk(retail_state_mutex_);
            retail_res_states_.erase(host_resource);
        }
        {
            std::lock_guard<std::mutex> lk(valid_view_mutex_);
            for (auto it = valid_rtv_handles_.begin(); it != valid_rtv_handles_.end();) {
                if (it->second == host_resource) {
                    valid_rtv_formats_.erase(it->first);
                    it = valid_rtv_handles_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = valid_dsv_handles_.begin(); it != valid_dsv_handles_.end();) {
                if (it->second == host_resource) {
                    valid_dsv_formats_.erase(it->first);
                    it = valid_dsv_handles_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> gblk(gpu_written_buf_mutex_);
        gpu_written_buffer_ranges_.erase(host_resource);
    }
    {
        std::lock_guard<std::mutex> lock(placed_tex_va_mutex_);
        auto tit = placed_tex_res_to_va_.find(host_resource);
        if (tit != placed_tex_res_to_va_.end()) {
            const uint64_t gva = tit->second;
            placed_tex_res_to_va_.erase(tit);
            auto vit = placed_tex_by_va_.find(gva);
            if (vit != placed_tex_by_va_.end()) {
                auto& vec = vit->second;
                vec.erase(std::remove(vec.begin(), vec.end(), host_resource), vec.end());
                if (vec.empty()) {
                    placed_tex_by_va_.erase(vit);
                }
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(buffer_va_mutex_);
        auto it = buffer_va_map_.find(host_resource);
        if (it != buffer_va_map_.end()) {
            if (it->second.owns_persistent_map && it->second.mapped_cpu_ptr) {
                D3D12_RANGE no_write{0u, 0u};
                host_resource->Unmap(0u, &no_write);
            }
            if (it->second.gpu_va != 0ull) {
                auto git = buffer_va_by_gpu_.find(it->second.gpu_va);
                if (git != buffer_va_by_gpu_.end() && git->second == host_resource) {
                    buffer_va_by_gpu_.erase(git);
                }
            }
            buffer_va_map_.erase(it);
            buffer_va_gen_.fetch_add(1ull, std::memory_order_release);
        }
    }
    if (tile_mode_count_.load(std::memory_order_acquire) != 0u) {
        std::lock_guard<std::mutex> lock(tile_mode_mutex_);
        if (resource_tile_modes_.erase(host_resource) > 0u) {
            tile_mode_count_.store(
                static_cast<uint32_t>(resource_tile_modes_.size()), std::memory_order_release);
        }
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
        ID3D12Resource* unified_res_release = nullptr;
        {
            std::lock_guard<std::mutex> lock(unified_mutex_);
            auto uit = unified_textures_.find(host_resource);
            if (uit != unified_textures_.end()) {
                if (uit->second.dirty) {
                    dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                }
                unified_res_release = uit->second.resource;
                unified_textures_.erase(uit);
                registered_unified_count_.store(
                    static_cast<uint32_t>(unified_textures_.size()), std::memory_order_release);
            }
        }
        if (unified_res_release) {
            if (retail_scarlett_abi_) {
                DeferHostResourceRelease(unified_res_release);
            } else {
                unified_res_release->Release();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(fallback_mutex_);
        r9g9b9e5_fallback_resources_.erase(host_resource);
    }
    {
        std::lock_guard<std::mutex> slk(retail_state_mutex_);
        retail_res_states_.erase(host_resource);
    }
    ID3D12Resource* expected_dii = host_resource;
    (void)last_dii_rtv_.compare_exchange_strong(expected_dii, nullptr, std::memory_order_acq_rel);
    if (vulkan_routing_active_) {
        std::lock_guard<std::mutex> lock(vk_state_mutex_);
        vk_resources_.erase(host_resource);
        vk_rt_framebuffers_.erase(host_resource);
    }
}

void DeviceX::DeferHostResourceRelease(ID3D12Resource* host_resource) noexcept {
    if (!host_resource) return;
    constexpr uint64_t kRetireFrameDelay = 16ull;
    const uint64_t retire = present_frame_count_.load(std::memory_order_acquire) + kRetireFrameDelay;
    std::lock_guard<std::mutex> lk(deferred_release_mutex_);
    deferred_release_queue_.push_back({host_resource, retire});
}

void DeviceX::OnPresentFrameComplete(uint64_t present_frame) noexcept {
    present_frame_count_.store(present_frame, std::memory_order_release);
    std::vector<ID3D12Resource*> ready;
    {
        std::lock_guard<std::mutex> lk(deferred_release_mutex_);
        while (!deferred_release_queue_.empty() &&
               deferred_release_queue_.front().retire_frame <= present_frame) {
            ready.push_back(deferred_release_queue_.front().resource);
            deferred_release_queue_.pop_front();
        }
    }
    for (ID3D12Resource* r : ready) {
        if (r) r->Release();
    }
}

void DeviceX::NotifyHeapDestroyed(ID3D12Heap* host_heap) noexcept {
    if (!host_heap) return;
    UntrackLiveResidencyObject(static_cast<ID3D12Pageable*>(host_heap));
    std::lock_guard<std::mutex> lock(unified_mutex_);
    address_heaps_.erase(host_heap);
}

void DeviceX::NotifyDescriptorHeapDestroyed(ID3D12DescriptorHeap* host_heap) noexcept {
    if (!host_heap) return;
    {
        std::lock_guard<std::mutex> lock(paged_desc_mutex_);
        auto pit = paged_desc_heaps_.find(host_heap);
        if (pit != paged_desc_heaps_.end()) {
            for (auto& page : pit->second.pages) {
                if (page.isolated_shadow_heap) {
                    page.isolated_shadow_heap->Release();
                    page.isolated_shadow_heap = nullptr;
                }
            }
            paged_desc_heaps_.erase(pit);
        }
    }
    {
        std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
        virtual_sampler_heaps_.erase(host_heap);
    }
    {
        std::lock_guard<std::mutex> glk(guest_dh_mutex_);
        auto it_end = std::remove_if(
            guest_backed_desc_heaps_.begin(), guest_backed_desc_heaps_.end(),
            [host_heap](const GuestBackedDescHeap& g) { return g.host_heap == host_heap; });
        if (it_end != guest_backed_desc_heaps_.end()) {
            guest_backed_desc_heaps_.erase(it_end, guest_backed_desc_heaps_.end());
            has_guest_backed_desc_heaps_.store(
                !guest_backed_desc_heaps_.empty(), std::memory_order_release);
        }
    }
    std::lock_guard<std::mutex> lock(residency_mutex_);
    auto it = desc_heap_window0_vas_.find(host_heap);
    if (it != desc_heap_window0_vas_.end()) {
        openscarlett::SparseVramFree(sparse_window_iso_, it->second);
        desc_heap_window0_vas_.erase(it);
    }
}

void DeviceX::NotifyRootSignatureDestroyed(ID3D12RootSignature* host_rs) noexcept {
    if (!host_rs) return;
    {
        std::lock_guard<std::mutex> glk(guest_dh_mutex_);
        rs_table_slot_counts_.erase(host_rs);
    }
    {
        std::lock_guard<std::mutex> dlk(diag_meta_mutex_);
        diag_rs_map_.erase(host_rs);
    }
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
    {
        std::lock_guard<std::mutex> lock(cbv_align_mutex_);
        if (cbv_align_ring_) {
            if (cbv_align_ring_cpu_.load(std::memory_order_relaxed)) {
                cbv_align_ring_->Unmap(0u, nullptr);
                cbv_align_ring_cpu_.store(nullptr, std::memory_order_release);
            }
            cbv_align_ring_->Release();
            cbv_align_ring_ = nullptr;
            cbv_align_ring_gpu_.store(0ull, std::memory_order_release);
        }
    }
    {
        std::lock_guard<std::mutex> tlk(transient_ring_mutex_);
        if (transient_ring_) {
            if (transient_ring_cpu_) {
                transient_ring_->Unmap(0u, nullptr);
                transient_ring_cpu_ = nullptr;
            }
            transient_ring_->Release();
            transient_ring_ = nullptr;
            transient_ring_gpu_ = 0ull;
        }
    }
    {
        std::lock_guard<std::mutex> dlk_uav(dummy_root_uav_mutex_);
        if (dummy_root_uav_buf_) {
            dummy_root_uav_buf_->Release();
            dummy_root_uav_buf_ = nullptr;
            dummy_root_uav_gpu_ = 0ull;
        }
    }
    {
        std::lock_guard<std::mutex> glk(guest_dh_mutex_);
        if (shadow_cbv_srv_uav_heap_) {
            shadow_cbv_srv_uav_heap_->Release();
            shadow_cbv_srv_uav_heap_ = nullptr;
            shadow_cbv_srv_uav_cpu_start_.ptr = 0u;
            shadow_null_srv_cpu_.ptr = 0u;
            shadow_cbv_srv_uav_cap_ = 0u;
            shadow_static_cursor_ = 1u;
            shadow_dynamic_cursor_ = 0u;
        }
        if (shadow_sampler_heap_) {
            shadow_sampler_heap_->Release();
            shadow_sampler_heap_ = nullptr;
            shadow_sampler_cpu_start_.ptr = 0u;
            shadow_sampler_cap_ = 0u;
        }
        shadow_slot_keys_.clear();
        shadow_slot_valid_.clear();
        desc32_to_shadow_cpu_.clear();
        rs_table_slot_counts_.clear();
        guest_backed_desc_heaps_.clear();
        has_guest_backed_desc_heaps_.store(false, std::memory_order_relaxed);
    }
    {
        std::vector<ID3D12Resource*> leftover;
        {
            std::lock_guard<std::mutex> dlk(deferred_release_mutex_);
            leftover.reserve(deferred_release_queue_.size());
            for (const auto& entry : deferred_release_queue_) {
                if (entry.resource) leftover.push_back(entry.resource);
            }
            deferred_release_queue_.clear();
        }
        for (ID3D12Resource* r : leftover) {
            r->Release();
        }
    }
    {
        std::lock_guard<std::mutex> alk(placed_alias_mutex_);
        for (auto& [bk, hp] : placed_alias_heaps_) {
            (void)bk;
            if (hp) hp->Release();
        }
        placed_alias_heaps_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(paged_desc_mutex_);
        for (auto& kv : paged_desc_heaps_) {
            for (auto& page : kv.second.pages) {
                if (page.isolated_shadow_heap) {
                    page.isolated_shadow_heap->Release();
                    page.isolated_shadow_heap = nullptr;
                }
            }
        }
        paged_desc_heaps_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
        virtual_sampler_heaps_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        for (auto& kv : residency_entries_) {
            if (kv.second.in_flight_fence) {
                kv.second.in_flight_fence->Release();
                kv.second.in_flight_fence = nullptr;
            }
        }
        residency_entries_.clear();
        residency_lru_.clear();
        desc_heap_window0_vas_.clear();
        openscarlett::DestroySparseVramWindowIsolator(sparse_window_iso_);
        sparse_window_iso_ = nullptr;
    }
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
    if (IsEqualIID(riid, IID_IOpenScarlettVramResidencyControl)) {
        *ppvObject = static_cast<IOpenScarlettVramResidencyControl*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettGraphicsParity)) {
        *ppvObject = static_cast<IOpenScarlettGraphicsParity*>(this);
        AddRef();
        RecordDev("ID3D12DeviceX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettGraphicsCompleteness)) {
        *ppvObject = static_cast<IOpenScarlettGraphicsCompleteness*>(this);
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
    D3D12_COMMAND_QUEUE_DESC clean_desc{};
    const D3D12_COMMAND_QUEUE_DESC* desc_ptr = pDesc;
    const bool requested_copy = (pDesc && pDesc->Type == D3D12_COMMAND_LIST_TYPE_COPY);
    if (pDesc) {
        clean_desc = *pDesc;
        // Strip Xbox console-private upper queue flags while preserving genuine D3D12_COMMAND_LIST_TYPE_COPY.
        clean_desc.Flags = static_cast<D3D12_COMMAND_QUEUE_FLAGS>(
            static_cast<uint32_t>(clean_desc.Flags) &
            static_cast<uint32_t>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT));
        desc_ptr = &clean_desc;
    }
    ID3D12CommandQueue* host_q = nullptr;
    HRESULT hr = host_dev_->CreateCommandQueue(desc_ptr, OS_D3D12_IID(ID3D12CommandQueue),
                                               reinterpret_cast<void**>(&host_q));
    bool genuine_copy_queue = (requested_copy && SUCCEEDED(hr) && host_q != nullptr);
    if ((FAILED(hr) || !host_q) && requested_copy && desc_ptr) {
        // Only fall back to DIRECT if the host genuinely failed to create a Copy Engine queue.
        D3D12_COMMAND_QUEUE_DESC fb_desc = *desc_ptr;
        fb_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = host_dev_->CreateCommandQueue(&fb_desc, OS_D3D12_IID(ID3D12CommandQueue),
                                           reinterpret_cast<void**>(&host_q));
    }
    if (FAILED(hr) || !host_q) {
        RecordDev("ID3D12DeviceX::CreateCommandQueue", 8, hr);
        return hr;
    }
    if (genuine_copy_queue) {
        dedicated_copy_queues_created_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::CreateCommandQueue[DedicatedCopyEngine]", 8, S_OK);
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

// Detects real retail Xbox Series X|S DXBC containers whose DXIL chunk carries
// an XDAT/XBCH/FSSM precompiled AMD RDNA2 microcode payload (verified live against
// UE5 Frosty-XSX-Shipping.exe / The Matrix Awakens cooked shader archives).
bool IsXboxXdatDxbcBlob(const void* data, size_t size) noexcept {
    if (!data || size < 32u) return false;
    const auto* b = static_cast<const uint8_t*>(data);
    if (std::memcmp(b, "DXBC", 4u) != 0) return false;
    uint32_t total = 0u, nchunks = 0u;
    std::memcpy(&total, b + 24u, 4u);
    std::memcpy(&nchunks, b + 28u, 4u);
    if (total < 32u || total > size || nchunks == 0u || nchunks > 128u) return false;
    if (32ull + static_cast<uint64_t>(nchunks) * 4ull > total) return false;
    for (uint32_t i = 0u; i < nchunks; ++i) {
        uint32_t off = 0u;
        std::memcpy(&off, b + 32u + static_cast<size_t>(i) * 4u, 4u);
        if (static_cast<uint64_t>(off) + 36ull > total) continue;
        if (std::memcmp(b + off, "DXIL", 4u) == 0) {
            uint32_t csz = 0u;
            std::memcpy(&csz, b + off + 4u, 4u);
            if (csz >= 28u && static_cast<uint64_t>(off) + 8ull + 28ull <= total &&
                std::memcmp(b + off + 8u + 24u, "XDAT", 4u) == 0) {
                return true;
            }
        }
    }
    return false;
}

// Returns 0=float4, 1=uint4, 2=int4 for D3D12 RTV pixel-shader output matching.
uint32_t ClassifyRtvOutputKind(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R8_UINT:
            return 1u;
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8_SINT:
            return 2u;
        default:
            return 0u;
    }
}

const std::vector<uint8_t>& GetCachedMinimalCs50() {
    static std::vector<uint8_t> s_bc;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        (void)CompileHlslWithSm50Fallback(
            "[numthreads(1,1,1)] void main() {}", "cs_5_0", s_bc);
    });
    return s_bc;
}

const std::vector<uint8_t>& GetCachedMinimalVs50() {
    static std::vector<uint8_t> s_bc;
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        (void)CompileHlslWithSm50Fallback(
            "float4 main() : SV_Position { return float4(0.0, 0.0, 0.0, 1.0); }", "vs_5_0", s_bc);
    });
    return s_bc;
}

const std::vector<uint8_t>& GetCachedMinimalPs50(UINT num_rts, const DXGI_FORMAT rtv_formats[8]) {
    const UINT clamped_rts = std::min<UINT>(num_rts, 8u);
    uint32_t key = clamped_rts;
    for (UINT i = 0u; i < clamped_rts; ++i) {
        key |= (ClassifyRtvOutputKind(rtv_formats[i]) << (4u + i * 2u));
    }
    static std::mutex s_mu;
    static std::unordered_map<uint32_t, std::vector<uint8_t>> s_cache;
    std::lock_guard<std::mutex> lock(s_mu);
    auto it = s_cache.find(key);
    if (it != s_cache.end()) {
        return it->second;
    }
    std::string hlsl;
    if (clamped_rts == 0u) {
        hlsl = "void main(float4 pos : SV_Position) {}";
    } else {
        hlsl = "struct PSOut {\n";
        for (UINT i = 0u; i < clamped_rts; ++i) {
            const uint32_t k = (key >> (4u + i * 2u)) & 3u;
            const char* ty = (k == 1u) ? "uint4" : ((k == 2u) ? "int4" : "float4");
            hlsl += "  ";
            hlsl += ty;
            hlsl += " c" + std::to_string(i) + " : SV_Target" + std::to_string(i) + ";\n";
        }
        hlsl += "};\nPSOut main(float4 pos : SV_Position) {\n  PSOut o = (PSOut)0;\n  return o;\n}\n";
    }
    std::vector<uint8_t> bc;
    (void)CompileHlslWithSm50Fallback(hlsl, "ps_5_0", bc);
    auto [ins, _] = s_cache.emplace(key, std::move(bc));
    return ins->second;
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

    const uint64_t stage_blob_hash = HashBytesFnv1a64(data, size);
    std::mutex& lift_stripe = shader_lift_stripes_[stage_blob_hash & (kShaderLiftLockStripes - 1u)];
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
            stage_blob_hash ^
            (static_cast<uint64_t>(highest_sm) << 33) ^
            (use_dxr11 ? 0xA5A50001ull : 0x5A5A0002ull);
        {
            std::shared_lock<std::shared_mutex> rlock(xbox_container_cache_mutex_);
            auto it = xbox_container_dxil_cache_.find(cache_key);
            if (it != xbox_container_dxil_cache_.end()) {
                out_bytecode = it->second;
                if (out_parsed) *out_parsed = std::move(parsed);
                RecordDev("ID3D12DeviceX::RayQueryBridgeLift", 11, S_OK);
                return true;
            }
        }

        std::lock_guard<std::mutex> stripe_lock(lift_stripe);
        {
            std::shared_lock<std::shared_mutex> rlock(xbox_container_cache_mutex_);
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
        const std::string target = use_dxr11
                                       ? std::string("cs_6_5")
                                       : ((highest_sm >= 0x65u && !adapter_info_.is_warp)
                                              ? std::string("cs_6_5")
                                              : std::string("cs_5_0"));
        if (!CompileHlslWithSm50Fallback(src_hlsl, target, out_bytecode)) {
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> wlock(xbox_container_cache_mutex_);
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
            stage_blob_hash ^
            (static_cast<uint64_t>(highest_sm) << 33) ^
            (mesh_has_payload ? 0x10101010ull : 0x20202020ull) ^
            (needs_paired_interp ? 0x33333333ull : 0ull);
        {
            std::shared_lock<std::shared_mutex> rlock(xbox_container_cache_mutex_);
            auto it = xbox_container_dxil_cache_.find(cache_key);
            if (it != xbox_container_dxil_cache_.end()) {
                out_bytecode = it->second;
                if (out_parsed) *out_parsed = std::move(parsed);
                RecordDev("ID3D12DeviceX::XboxContainerLift", 10, S_OK);
                return true;
            }
        }

        std::lock_guard<std::mutex> stripe_lock(lift_stripe);
        {
            std::shared_lock<std::shared_mutex> rlock(xbox_container_cache_mutex_);
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
        if (!openscarlett::LiftXboxShaderContainer(data, size, lift_res, xopts).ok()) {
            return false;
        }
        if (!CompileHlslWithSm50Fallback(lift_res.lift.hlsl_source, target, out_bytecode)) {
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> wlock(xbox_container_cache_mutex_);
            xbox_container_dxil_cache_[cache_key] = out_bytecode;
        }
        xbox_container_lifts_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::XboxContainerLift", 10, S_OK);
        if (out_parsed) *out_parsed = std::move(parsed);
        return true;
    }

    // 3. Standard ShaderCache path using per-hash lock striping for parallel PSO compilation!
    openscarlett::ShaderCacheLiftRequest req{};
    req.highest_shader_model = highest_sm;
    req.mesh_has_payload = mesh_has_payload;
    req.prefer_dxil_standin = prefer_dxil_standin;

    openscarlett::CachedShaderResult res{};
    std::string diag;
    openscarlett::Status st;
    {
        std::lock_guard<std::mutex> stripe_lock(lift_stripe);
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
        const uint64_t spv_hash = HashBytesFnv1a64(data, size);
        std::lock_guard<std::mutex> stripe_lock(shader_lift_stripes_[spv_hash & (kShaderLiftLockStripes - 1u)]);
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

        if (has_vs_parsed && has_ps_parsed) {
            const uint32_t f16_links = LinkFp16HalfStageSignatures(vs_parsed, ps_parsed);
            if (f16_links > 0u) {
                pso_f16_links_reconciled_.fetch_add(f16_links, std::memory_order_relaxed);
            }
            openscarlett::XboxPsoInterpLinkageReport interp_report{};
            (void)openscarlett::ValidateAndMatchStageInterpolation(vs_parsed, ps_parsed, interp_report);
            pso_interp_validations_.fetch_add(1ull, std::memory_order_relaxed);
            for (const auto& entry : interp_report.entries) {
                if (entry.matched) {
                    pso_interp_matches_.fetch_add(1ull, std::memory_order_relaxed);
                }
            }
        }

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
    if (retail_scarlett_abi_) {
        lifted_desc.DSVFormat = SanitizePsoDsvFormat(lifted_desc.DSVFormat);
        for (UINT r = 0u; r < 8u; ++r) {
            if (r >= lifted_desc.NumRenderTargets) {
                lifted_desc.RTVFormats[r] = DXGI_FORMAT_UNKNOWN;
            } else {
                lifted_desc.RTVFormats[r] = SanitizeXboxDxgiFormat(lifted_desc.RTVFormats[r]);
            }
        }
        const uint32_t raw_topo = static_cast<uint32_t>(lifted_desc.PrimitiveTopologyType);
        if (raw_topo == 0u || raw_topo > static_cast<uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)) {
            lifted_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    } else if (static_cast<uint32_t>(lifted_desc.PrimitiveTopologyType) >
               static_cast<uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)) {
        lifted_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    (void)SanitizeDepthStencilDesc(lifted_desc.DepthStencilState);
    if (retail_scarlett_abi_ && lifted_desc.DSVFormat == DXGI_FORMAT_UNKNOWN) {
        lifted_desc.DepthStencilState.DepthEnable = FALSE;
        lifted_desc.DepthStencilState.StencilEnable = FALSE;
    }
    {
        const DXGI_FORMAT msaa_fmt =
            (lifted_desc.NumRenderTargets > 0u && lifted_desc.RTVFormats[0] != DXGI_FORMAT_UNKNOWN)
                ? lifted_desc.RTVFormats[0]
                : ((lifted_desc.DSVFormat != DXGI_FORMAT_UNKNOWN) ? lifted_desc.DSVFormat
                                                                   : DXGI_FORMAT_R8G8B8A8_UNORM);
        (void)ClampMsaaSampleDescForFormat(msaa_fmt, lifted_desc.SampleDesc);
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev_->CreateGraphicsPipelineState(&lifted_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = E_FAIL;
    const bool gfx_has_xdat =
        retail_scarlett_abi_ &&
        (IsXboxXdatDxbcBlob(lifted_desc.VS.pShaderBytecode, lifted_desc.VS.BytecodeLength) ||
         IsXboxXdatDxbcBlob(lifted_desc.PS.pShaderBytecode, lifted_desc.PS.BytecodeLength) ||
         IsXboxXdatDxbcBlob(lifted_desc.DS.pShaderBytecode, lifted_desc.DS.BytecodeLength) ||
         IsXboxXdatDxbcBlob(lifted_desc.HS.pShaderBytecode, lifted_desc.HS.BytecodeLength) ||
         IsXboxXdatDxbcBlob(lifted_desc.GS.pShaderBytecode, lifted_desc.GS.BytecodeLength));
    bool used_standin_pso = false;
    if (!gfx_has_xdat) {
        hr = host_dev_->CreateGraphicsPipelineState(
            &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
        if (SUCCEEDED(hr) && host_pso) {
            RecordPsoVertexLayout(
                host_pso, lifted_desc.InputLayout.pInputElementDescs, lifted_desc.InputLayout.NumElements);
        }
    }
    if ((FAILED(hr) || !host_pso) && retail_scarlett_abi_) {
        const auto& vs_bc = GetCachedMinimalVs50();
        const auto& ps_bc = GetCachedMinimalPs50(lifted_desc.NumRenderTargets, lifted_desc.RTVFormats);
        if (!vs_bc.empty() && !ps_bc.empty()) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC safe = lifted_desc;
            safe.VS = {vs_bc.data(), vs_bc.size()};
            safe.PS = ((lifted_desc.PS.pShaderBytecode && lifted_desc.PS.BytecodeLength > 0u) ||
                       (!lifted_desc.VS.pShaderBytecode && safe.NumRenderTargets > 0u))
                          ? D3D12_SHADER_BYTECODE{ps_bc.data(), ps_bc.size()}
                          : D3D12_SHADER_BYTECODE{nullptr, 0u};
            safe.DS = {nullptr, 0u};
            safe.HS = {nullptr, 0u};
            safe.GS = {nullptr, 0u};
            safe.StreamOutput = {};
            safe.BlendState = {};
            for (UINT r = 0u; r < 8u; ++r) {
                safe.BlendState.RenderTarget[r].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                if (r >= safe.NumRenderTargets) {
                    safe.RTVFormats[r] = DXGI_FORMAT_UNKNOWN;
                }
            }
            safe.RasterizerState = {};
            safe.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            safe.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            safe.RasterizerState.DepthClipEnable = TRUE;
            safe.DepthStencilState = {};
            if (safe.DSVFormat != DXGI_FORMAT_UNKNOWN) {
                safe.DepthStencilState.DepthEnable = TRUE;
                safe.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                safe.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            }
            safe.InputLayout = {nullptr, 0u};
            safe.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
            safe.CachedPSO = {};
            safe.NodeMask = 0u;
            safe.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
            if (safe.SampleDesc.Count == 0u) safe.SampleDesc.Count = 1u;
            if (safe.SampleMask == 0u) safe.SampleMask = UINT_MAX;
            if (safe.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
                safe.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
                safe.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
            if (!safe.pRootSignature) {
                openscarlett::XboxRootSignatureDesc empty_rs{};
                safe.pRootSignature = SynthesizeHostRootSignatureFromXboxDesc(empty_rs);
            }
            const uint64_t standin_gfx_key =
                reinterpret_cast<uint64_t>(safe.pRootSignature) ^
                (static_cast<uint64_t>(safe.NumRenderTargets & 0xFu) << 56u) ^
                (static_cast<uint64_t>(safe.RTVFormats[0] & 0xFFu) << 48u) ^
                (static_cast<uint64_t>(safe.DSVFormat & 0xFFu) << 40u);
            static std::mutex s_standin_gfx_mu;
            static std::unordered_map<uint64_t, ID3D12PipelineState*> s_standin_gfx_cache;
            {
                std::lock_guard<std::mutex> slk(s_standin_gfx_mu);
                auto sit = s_standin_gfx_cache.find(standin_gfx_key);
                if (sit != s_standin_gfx_cache.end() && sit->second) {
                    host_pso = sit->second;
                    host_pso->AddRef();
                    hr = S_OK;
                }
            }
            if (!host_pso) {
                hr = host_dev_->CreateGraphicsPipelineState(
                    &safe, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
                if (SUCCEEDED(hr) && host_pso) {
                    std::lock_guard<std::mutex> slk(s_standin_gfx_mu);
                    auto [sit, inserted] = s_standin_gfx_cache.emplace(standin_gfx_key, host_pso);
                    if (inserted) {
                        host_pso->AddRef();
                    }
                }
            }
            if (SUCCEEDED(hr) && host_pso) {
                used_standin_pso = true;
                RecordDev("ID3D12DeviceX::XdatShaderStandinPso", 10, S_OK);
            }
        }
    }
    if (FAILED(hr) || !host_pso) {
        if (retail_scarlett_abi_) {
            static std::atomic<uint32_t> s_gpso_fail{0u};
            if (s_gpso_fail.fetch_add(1u, std::memory_order_relaxed) < 5u) {
                std::fprintf(stderr,
                             "[OpenScarlett][d3d12_xs] CreateGraphicsPipelineState FAILED hr=0x%08lX rs=%p rts=%u rtv0=%d dsv=%d samp=%u/%u\n",
                             static_cast<unsigned long>(hr), static_cast<void*>(lifted_desc.pRootSignature),
                             lifted_desc.NumRenderTargets, static_cast<int>(lifted_desc.RTVFormats[0]),
                             static_cast<int>(lifted_desc.DSVFormat),
                             lifted_desc.SampleDesc.Count, lifted_desc.SampleDesc.Quality);
                ID3D12InfoQueue* iq = nullptr;
                if (SUCCEEDED(host_dev_->QueryInterface(OS_D3D12_IID(ID3D12InfoQueue), reinterpret_cast<void**>(&iq))) && iq) {
                    const UINT64 n = iq->GetNumStoredMessages();
                    const UINT64 start = (n > 8u) ? (n - 8u) : 0u;
                    for (UINT64 i = start; i < n; ++i) {
                        SIZE_T len = 0u;
                        iq->GetMessage(i, nullptr, &len);
                        if (len > 0u) {
                            std::vector<uint8_t> buf(len);
                            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription) {
                                std::fprintf(stderr, "  [D3D12-GPSO] %s\n", m->pDescription);
                            }
                        }
                    }
                    iq->Release();
                }
                std::fflush(stderr);
            }
        }
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) PipelineStateXProxy(this, host_pso);
    host_pso->Release();
    if (!proxy) {
        RecordDev("ID3D12DeviceX::CreateGraphicsPipelineState", 10, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    if (used_standin_pso || (retail_scarlett_abi_ && gfx_has_xdat)) {
        proxy->SetStandinPso(true);
        if (retail_scarlett_abi_ && gfx_has_xdat && lifted_desc.pRootSignature) {
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->gpso = lifted_desc;
            pd->host_rs = lifted_desc.pRootSignature;
            pd->host_rs->AddRef();
            auto copy_bc = [](const D3D12_SHADER_BYTECODE& s, std::vector<uint8_t>& dst) {
                if (s.pShaderBytecode && s.BytecodeLength > 0u) {
                    const auto* bp = static_cast<const uint8_t*>(s.pShaderBytecode);
                    dst.assign(bp, bp + s.BytecodeLength);
                }
            };
            copy_bc(lifted_desc.VS, pd->vs_blob);
            copy_bc(lifted_desc.PS, pd->ps_blob);
            copy_bc(lifted_desc.HS, pd->hs_blob);
            copy_bc(lifted_desc.DS, pd->ds_blob);
            copy_bc(lifted_desc.GS, pd->gs_blob);
            if (lifted_desc.InputLayout.pInputElementDescs && lifted_desc.InputLayout.NumElements > 0u &&
                lifted_desc.InputLayout.NumElements <= 32u) {
                pd->il_names.reserve(lifted_desc.InputLayout.NumElements);
                pd->il_elems.assign(
                    lifted_desc.InputLayout.pInputElementDescs,
                    lifted_desc.InputLayout.pInputElementDescs + lifted_desc.InputLayout.NumElements);
                for (auto& e : pd->il_elems) {
                    pd->il_names.emplace_back(e.SemanticName ? e.SemanticName : "");
                    e.SemanticName = pd->il_names.back().c_str();
                }
                pd->gpso.InputLayout = {pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
            } else {
                pd->gpso.InputLayout = {nullptr, 0u};
            }
            if (!pd->vs_blob.empty()) {
                proxy->SetPendingXdatGfxDesc(std::move(pd));
            }
        }
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
    HRESULT hr = E_FAIL;
    const bool cs_is_xdat = retail_scarlett_abi_ &&
                            IsXboxXdatDxbcBlob(lifted_desc.CS.pShaderBytecode, lifted_desc.CS.BytecodeLength);
    bool used_standin_cs = false;
    if (!cs_is_xdat) {
        hr = host_dev_->CreateComputePipelineState(
            &lifted_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    }
    if ((FAILED(hr) || !host_pso) && retail_scarlett_abi_) {
        const auto& cs_bc = GetCachedMinimalCs50();
        if (!cs_bc.empty()) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC safe = lifted_desc;
            safe.CS = {cs_bc.data(), cs_bc.size()};
            safe.CachedPSO = {};
            safe.NodeMask = 0u;
            safe.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
            if (!safe.pRootSignature) {
                openscarlett::XboxRootSignatureDesc empty_rs{};
                safe.pRootSignature = SynthesizeHostRootSignatureFromXboxDesc(empty_rs);
            }
            const uint64_t standin_cs_key = reinterpret_cast<uint64_t>(safe.pRootSignature);
            static std::mutex s_standin_cs_mu;
            static std::unordered_map<uint64_t, ID3D12PipelineState*> s_standin_cs_cache;
            {
                std::lock_guard<std::mutex> slk(s_standin_cs_mu);
                auto sit = s_standin_cs_cache.find(standin_cs_key);
                if (sit != s_standin_cs_cache.end() && sit->second) {
                    host_pso = sit->second;
                    host_pso->AddRef();
                    hr = S_OK;
                }
            }
            if (!host_pso) {
                hr = host_dev_->CreateComputePipelineState(
                    &safe, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
                if (SUCCEEDED(hr) && host_pso) {
                    std::lock_guard<std::mutex> slk(s_standin_cs_mu);
                    auto [sit, inserted] = s_standin_cs_cache.emplace(standin_cs_key, host_pso);
                    if (inserted) {
                        host_pso->AddRef();
                    }
                }
            }
            if (SUCCEEDED(hr) && host_pso) {
                used_standin_cs = true;
                RecordDev("ID3D12DeviceX::XdatShaderStandinPso", 11, S_OK);
            }
        }
    }
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
    if (used_standin_cs || cs_is_xdat) {
        proxy->SetStandinPso(true);
        if (lifted_desc.pRootSignature && lifted_desc.CS.pShaderBytecode && lifted_desc.CS.BytecodeLength > 0u) {
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->is_compute = true;
            pd->host_rs = lifted_desc.pRootSignature;
            pd->host_rs->AddRef();
            const auto* bp = static_cast<const uint8_t*>(lifted_desc.CS.pShaderBytecode);
            pd->cs_blob.assign(bp, bp + lifted_desc.CS.BytecodeLength);
            proxy->SetPendingXdatGfxDesc(std::move(pd));
        }
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
    HRESULT hr = host_dev_->CheckFeatureSupport(
        Feature, pFeatureSupportData, FeatureSupportDataSize);

    // Xbox Series X|S d3d12_xs.h feature-data structs (notably D3D12_FEATURE_DATA_D3D12_OPTIONS
    // and OPTIONS1..12) can differ in size from desktop Windows 10 SDK structs, causing host
    // CheckFeatureSupport to return E_INVALIDARG. Bridge cleanly via a host-sized struct copy.
    if (hr == E_INVALIDARG && pFeatureSupportData != nullptr && FeatureSupportDataSize > 0u) {
        auto bridge_struct = [&](auto& host_data) -> HRESULT {
            std::memset(&host_data, 0, sizeof(host_data));
            const size_t in_copy = std::min<size_t>(FeatureSupportDataSize, sizeof(host_data));
            std::memcpy(&host_data, pFeatureSupportData, in_copy);
            HRESULT bhr = host_dev_->CheckFeatureSupport(
                Feature, &host_data, static_cast<UINT>(sizeof(host_data)));
            if (SUCCEEDED(bhr)) {
                std::memset(pFeatureSupportData, 0, FeatureSupportDataSize);
                std::memcpy(pFeatureSupportData, &host_data, in_copy);
            }
            return bhr;
        };

        switch (Feature) {
            case D3D12_FEATURE_D3D12_OPTIONS: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_ARCHITECTURE: {
                D3D12_FEATURE_DATA_ARCHITECTURE d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_FORMAT_SUPPORT: {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_FORMAT_INFO: {
                D3D12_FEATURE_DATA_FORMAT_INFO d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
                D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_SHADER_MODEL: {
                D3D12_FEATURE_DATA_SHADER_MODEL d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS1: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS1 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_ROOT_SIGNATURE: {
                D3D12_FEATURE_DATA_ROOT_SIGNATURE d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_ARCHITECTURE1: {
                D3D12_FEATURE_DATA_ARCHITECTURE1 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS2: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS2 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_SHADER_CACHE: {
                D3D12_FEATURE_DATA_SHADER_CACHE d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS3: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS4: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS4 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS5: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS5 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS6: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS6 d{};
                hr = bridge_struct(d);
                break;
            }
            case D3D12_FEATURE_D3D12_OPTIONS7: {
                D3D12_FEATURE_DATA_D3D12_OPTIONS7 d{};
                hr = bridge_struct(d);
                break;
            }
            default:
                break;
        }
    }

    if (SUCCEEDED(hr) && pFeatureSupportData != nullptr) {
        // Cross-IHV capability sanitization (Sprint 16): ensure wave sizes, DXR tiers,
        // and Mesh Shader tiers are accurately sanitized and never over-reported.
        if (Feature == D3D12_FEATURE_D3D12_OPTIONS1 &&
            FeatureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1)) {
            auto* o1 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1*>(pFeatureSupportData);
            if (o1->WaveOps) {
                o1->WaveLaneCountMin = sanitized_ihv_caps_.wave_lane_min;
                o1->WaveLaneCountMax = sanitized_ihv_caps_.wave_lane_max;
            }
        } else if (Feature == D3D12_FEATURE_D3D12_OPTIONS5 &&
                   FeatureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) {
            auto* o5 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5*>(pFeatureSupportData);
            o5->RaytracingTier = static_cast<D3D12_RAYTRACING_TIER>(sanitized_ihv_caps_.raytracing_tier);
        } else if (Feature == D3D12_FEATURE_D3D12_OPTIONS7 &&
                   FeatureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) {
            auto* o7 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7*>(pFeatureSupportData);
            o7->MeshShaderTier = static_cast<D3D12_MESH_SHADER_TIER>(sanitized_ihv_caps_.mesh_shader_tier);
        }
    }

    RecordDev("ID3D12DeviceX::CheckFeatureSupport", 13, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::CreateDescriptorHeap(
    const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) {
    D3D12_DESCRIPTOR_HEAP_DESC clean_desc{};
    const D3D12_DESCRIPTOR_HEAP_DESC* eff_desc = pDescriptorHeapDesc;
    if (pDescriptorHeapDesc &&
        (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
         pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
        (pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        clean_desc = *pDescriptorHeapDesc;
        clean_desc.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(
            clean_desc.Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        eff_desc = &clean_desc;
    }
    if (!ppvHeap) {
        const HRESULT hr = host_dev_->CreateDescriptorHeap(eff_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    *ppvHeap = nullptr;

    // Sprint 16: Unbounded Sampler Heap Virtualization (> 2048 console sampler limit).
    // Desktop D3D12 caps shader-visible SAMPLER heaps at 2048 descriptors. Virtualize arbitrarily
    // large console sampler heaps over a deduplicated 2048-slot backing host heap!
    constexpr UINT kDesktopMaxSamplers = 2048u;
    const bool is_unbounded_sampler =
        (pDescriptorHeapDesc != nullptr) &&
        (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) &&
        (pDescriptorHeapDesc->NumDescriptors > kDesktopMaxSamplers);

    D3D12_DESCRIPTOR_HEAP_DESC host_desc{};
    const D3D12_DESCRIPTOR_HEAP_DESC* host_desc_ptr = eff_desc;
    if (is_unbounded_sampler) {
        host_desc = *eff_desc;
        host_desc.NumDescriptors = kDesktopMaxSamplers;
        host_desc_ptr = &host_desc;
    }

    ID3D12DescriptorHeap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateDescriptorHeap(
        host_desc_ptr, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host_heap));
    if ((FAILED(hr) || !host_heap) && host_desc_ptr != nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC safe = *host_desc_ptr;
        safe.NodeMask = 0u;
        if (safe.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
            safe.Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
            safe.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        } else {
            safe.Flags = static_cast<D3D12_DESCRIPTOR_HEAP_FLAGS>(
                static_cast<UINT>(safe.Flags) & static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE));
        }
        if (safe.NumDescriptors == 0u) {
            safe.NumDescriptors = 1u;
        }
        if (safe.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
            (safe.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 &&
            safe.NumDescriptors > 2048u) {
            safe.NumDescriptors = 2048u;
        }
        // First retry with sanitized Flags/NodeMask while preserving the full console NumDescriptors
        // (Resource Binding Tier 3 desktop GPUs like Turing/Ampere/RDNA support >1,000,000 CBV_SRV_UAV).
        hr = host_dev_->CreateDescriptorHeap(
            &safe, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host_heap));
        if ((FAILED(hr) || !host_heap) &&
            safe.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            (safe.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 &&
            safe.NumDescriptors > 1000000u) {
            safe.NumDescriptors = 1000000u;
            hr = host_dev_->CreateDescriptorHeap(
                &safe, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&host_heap));
        }
    }
    if (SUCCEEDED(hr) && host_heap && CommandListX::IsDiagTraceEnabled() && pDescriptorHeapDesc) {
        const auto hd = host_heap->GetDesc();
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(hd.Type);
        const auto c0 = host_heap->GetCPUDescriptorHandleForHeapStart();
        const auto g0 = (hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                            ? host_heap->GetGPUDescriptorHandleForHeapStart()
                            : D3D12_GPU_DESCRIPTOR_HANDLE{0ull};
        std::fprintf(stderr,
                     "[OpenScarlett][DH] type=%d reqN=%u reqFlags=0x%X -> hostN=%u hostFlags=0x%X inc=%u cpu0=0x%llX gpu0=0x%llX\n",
                     static_cast<int>(pDescriptorHeapDesc->Type), pDescriptorHeapDesc->NumDescriptors,
                     static_cast<unsigned>(pDescriptorHeapDesc->Flags), hd.NumDescriptors,
                     static_cast<unsigned>(hd.Flags), inc,
                     static_cast<unsigned long long>(c0.ptr), static_cast<unsigned long long>(g0.ptr));
        std::fflush(stderr);
    }
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap", 14, hr);
        return hr;
    }
    if (retail_scarlett_abi_ && pDescriptorHeapDesc &&
        pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        pDescriptorHeapDesc->NumDescriptors > 0u) {
        D3D12_SAMPLER_DESC def_samp{};
        def_samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def_samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def_samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def_samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def_samp.MaxAnisotropy = 1u;
        def_samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        def_samp.MaxLOD = D3D12_FLOAT32_MAX;
        host_dev_->CreateSampler(&def_samp, host_heap->GetCPUDescriptorHandleForHeapStart());
    }
    if (pDescriptorHeapDesc && sparse_window_iso_) {
        const uint64_t heap_bytes = std::max<uint64_t>(
            65536ull, static_cast<uint64_t>(pDescriptorHeapDesc->NumDescriptors) * 64ull);
        const uint64_t w0_va = openscarlett::SparseVramAllocateWindow0(sparse_window_iso_, heap_bytes);
        if (w0_va != 0ull) {
            std::lock_guard<std::mutex> lock(residency_mutex_);
            desc_heap_window0_vas_[host_heap] = w0_va;
        }
    }

    DescriptorHeapXProxy* proxy = nullptr;
    if (is_unbounded_sampler) {
        const UINT inc = std::max<UINT>(
            1u, host_dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
        const uint64_t range_bytes =
            ((static_cast<uint64_t>(pDescriptorHeapDesc->NumDescriptors) * inc) + 65535ull) & ~65535ull;
        D3D12_CPU_DESCRIPTOR_HANDLE virt_cpu{0u};
        D3D12_GPU_DESCRIPTOR_HANDLE virt_gpu{0ull};
        {
            std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
            virt_cpu.ptr = static_cast<SIZE_T>(next_virtual_sampler_cpu_base_);
            next_virtual_sampler_cpu_base_ += range_bytes;
            if ((pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
                virt_gpu.ptr = next_virtual_sampler_gpu_base_;
                next_virtual_sampler_gpu_base_ += range_bytes;
            }
            VirtualSamplerHeapState vs{};
            vs.host_heap = host_heap;
            vs.virtual_desc = *pDescriptorHeapDesc;
            vs.virt_cpu_start = virt_cpu;
            vs.virt_gpu_start = virt_gpu;
            vs.host_cpu_start = host_heap->GetCPUDescriptorHandleForHeapStart();
            if ((pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
                vs.host_gpu_start = host_heap->GetGPUDescriptorHandleForHeapStart();
            }
            vs.inc_size = inc;
            vs.host_capacity = kDesktopMaxSamplers;
            vs.virt_to_host_slot.assign(pDescriptorHeapDesc->NumDescriptors, 0u);
            vs.virt_slot_keys.resize(pDescriptorHeapDesc->NumDescriptors);
            vs.virt_slot_valid.assign(pDescriptorHeapDesc->NumDescriptors, 0u);
            virtual_sampler_heaps_[host_heap] = std::move(vs);
        }
        unbounded_sampler_heaps_created_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::CreateDescriptorHeap[UnboundedSampler]", 14, S_OK);
        proxy = new (std::nothrow) DescriptorHeapXProxy(
            this, host_heap, pDescriptorHeapDesc, virt_cpu, virt_gpu);
    } else {
        // Sprint 16: Page-at-a-Time Console Descriptor Heap Manager (xcl #84) for shader-visible CBV/SRV/UAV.
        if (pDescriptorHeapDesc &&
            pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            (pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 &&
            pDescriptorHeapDesc->NumDescriptors > 0u) {
            const UINT inc = std::max<UINT>(
                1u, host_dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            const UINT descs_per_page = std::max<UINT>(1u, 65536u / std::max<UINT>(inc, 32u));
            const UINT page_count = std::max<UINT>(
                1u, (pDescriptorHeapDesc->NumDescriptors + descs_per_page - 1u) / descs_per_page);
            PagedDescriptorHeapState ps{};
            ps.primary_host_heap = host_heap;
            ps.cpu_start = host_heap->GetCPUDescriptorHandleForHeapStart();
            ps.gpu_start = host_heap->GetGPUDescriptorHandleForHeapStart();
            ps.inc_size = inc;
            ps.num_descriptors = pDescriptorHeapDesc->NumDescriptors;
            ps.descriptors_per_page = descs_per_page;
            ps.pages.resize(page_count);
            for (UINT p = 0u; p < page_count; ++p) {
                ps.pages[p].page_index = p;
                ps.pages[p].generation = 1ull;
                ps.pages[p].in_flight = false;
            }
            {
                std::lock_guard<std::mutex> lock(paged_desc_mutex_);
                paged_desc_heaps_[host_heap] = std::move(ps);
            }
            paged_descriptor_heaps_created_.fetch_add(1ull, std::memory_order_relaxed);
            descriptor_pages_allocated_.fetch_add(page_count, std::memory_order_relaxed);
        }
        proxy = new (std::nothrow) DescriptorHeapXProxy(this, host_heap);
    }

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
    HRESULT hr = E_FAIL;
    // On retail Scarlett, deserialize & append a fallback static linear-clamp sampler at
    // register(s0, space99) so lifted XDAT/FSSM shaders that use bindless samplers work cleanly
    // on SM 6.5 GPUs (e.g. GTX 1650 Ti) without shifting any root parameter indices.
    if (retail_scarlett_abi_ && pBlobWithRootSignature != nullptr && blobLengthInBytes > 0u) {
        using PfnCreateVrsDeser = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
        using PfnSerVrs = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
        auto fn_deser = reinterpret_cast<PfnCreateVrsDeser>(
            openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
        auto fn_ser = reinterpret_cast<PfnSerVrs>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
        if (fn_deser && fn_ser) {
            ID3D12VersionedRootSignatureDeserializer* deser = nullptr;
            if (SUCCEEDED(fn_deser(
                    pBlobWithRootSignature, blobLengthInBytes,
                    OS_D3D12_IID(ID3D12VersionedRootSignatureDeserializer),
                    reinterpret_cast<void**>(&deser))) && deser) {
                const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* orig = deser->GetUnconvertedRootSignatureDesc();
                if (orig && (orig->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ||
                             orig->Version == D3D_ROOT_SIGNATURE_VERSION_1_1)) {
                    D3D12_VERSIONED_ROOT_SIGNATURE_DESC aug = *orig;
                    D3D12_STATIC_SAMPLER_DESC fallback_samp{};
                    fallback_samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    fallback_samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    fallback_samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    fallback_samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    fallback_samp.MaxAnisotropy = 1u;
                    fallback_samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                    fallback_samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
                    fallback_samp.MinLOD = 0.0f;
                    fallback_samp.MaxLOD = D3D12_FLOAT32_MAX;
                    fallback_samp.ShaderRegister = 0u;
                    fallback_samp.RegisterSpace = 99u;
                    fallback_samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                    std::vector<D3D12_STATIC_SAMPLER_DESC> samps;
                    std::vector<D3D12_ROOT_PARAMETER1> params_1_1;
                    std::vector<D3D12_DESCRIPTOR_RANGE1> expanded_ranges_1_1;
                    auto sanitize_flags = [&](D3D12_ROOT_SIGNATURE_FLAGS& f) {
                        UINT bits = static_cast<UINT>(f) & 0x0FFFu;
                        if (adapter_info_.raytracing_tier == 0u) {
                            bits &= ~static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);
                        }
                        if (adapter_info_.highest_shader_model < 0x66u) {
                            bits &= ~(0x400u | 0x800u);
                        }
                        if ((bits & static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE)) == 0u) {
                            bits |= static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
                        }
                        f = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(bits);
                    };
                    if (aug.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
                        sanitize_flags(aug.Desc_1_1.Flags);
                        // Retail Scarlett engines share 1-table template root signatures (e.g. single
                        // b27 space1 range) across multiple PSOs whose shaders read descriptors at
                        // explicit offsets 0/1/2 from the same table. Alias the PSV0-verified slots
                        // at their exact hardware table offsets so PC D3D12 PSO validation passes.
                        if (aug.Desc_1_1.NumParameters == 1u && aug.Desc_1_1.pParameters &&
                            aug.Desc_1_1.pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                            aug.Desc_1_1.pParameters[0].DescriptorTable.NumDescriptorRanges == 1u &&
                            aug.Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges &&
                            aug.Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                            aug.Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].BaseShaderRegister == 27u &&
                            aug.Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0].RegisterSpace == 1u) {
                            params_1_1.assign(aug.Desc_1_1.pParameters, aug.Desc_1_1.pParameters + 1u);
                            D3D12_DESCRIPTOR_RANGE1 r0 = aug.Desc_1_1.pParameters[0].DescriptorTable.pDescriptorRanges[0];
                            r0.OffsetInDescriptorsFromTableStart = 0u;
                            auto make_rng = [](D3D12_DESCRIPTOR_RANGE_TYPE rt, UINT reg, UINT off) {
                                D3D12_DESCRIPTOR_RANGE1 r{};
                                r.RangeType = rt;
                                r.NumDescriptors = 1u;
                                r.BaseShaderRegister = reg;
                                r.RegisterSpace = 1u;
                                r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                                r.OffsetInDescriptorsFromTableStart = off;
                                return r;
                            };
                            auto make_s2_rng = [](D3D12_DESCRIPTOR_RANGE_TYPE rt, UINT count) {
                                D3D12_DESCRIPTOR_RANGE1 r{};
                                r.RangeType = rt;
                                r.NumDescriptors = count;
                                r.BaseShaderRegister = 0u;
                                r.RegisterSpace = 2u;
                                r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                                r.OffsetInDescriptorsFromTableStart = 0u;
                                return r;
                            };
                            // Slot @0..@15 space2 SRV, @16..@31 space2 CBV, @32..@47 space2 UAV
                            // maps every hardware s[8:9]+slot*32 SRV/CBV/UAV cleanly without collisions.
                            D3D12_DESCRIPTOR_RANGE1 s2_cbv =
                                make_s2_rng(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 16u);
                            s2_cbv.OffsetInDescriptorsFromTableStart = 16u;
                            D3D12_DESCRIPTOR_RANGE1 s98_srv =
                                make_s2_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16u);
                            s98_srv.RegisterSpace = 98u;
                            expanded_ranges_1_1 = {
                                r0,
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2u, 0u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3u, 0u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7u, 0u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5u, 1u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 72u, 1u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 277u, 1u),
                                make_rng(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 3u, 3u),
                                make_s2_rng(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16u),
                                s98_srv,
                                s2_cbv,
                            };
                            if (params_1_1[0].ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL) {
                                D3D12_DESCRIPTOR_RANGE1 s2_uav =
                                    make_s2_rng(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 16u);
                                s2_uav.OffsetInDescriptorsFromTableStart = 32u;
                                D3D12_DESCRIPTOR_RANGE1 s98_uav = s2_uav;
                                s98_uav.RegisterSpace = 98u;
                                expanded_ranges_1_1.push_back(s2_uav);
                                expanded_ranges_1_1.push_back(s98_uav);
                            }
                            params_1_1[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(expanded_ranges_1_1.size());
                            params_1_1[0].DescriptorTable.pDescriptorRanges = expanded_ranges_1_1.data();
                            aug.Desc_1_1.pParameters = params_1_1.data();
                            if (aug.Desc_1_1.NumStaticSamplers == 0u) {
                                for (UINT sreg = 0u; sreg < 22u; ++sreg) {
                                    D3D12_STATIC_SAMPLER_DESC ss = fallback_samp;
                                    ss.ShaderRegister = sreg;
                                    ss.RegisterSpace = 1u;
                                    samps.push_back(ss);
                                }
                            }
                        } else if (aug.Desc_1_1.NumParameters >= 1u && aug.Desc_1_1.pParameters) {
                            // Retail Scarlett Scaleform/UI pipelines share a root signature declaring
                            // CBV b0 space1 across both instanced (b0 space1) and non-instanced
                            // (b7 space1) shaders reading the exact same descriptor slot. Alias b7
                            // space1 at b0 space1's table offset when b7 space1 is not already present,
                            // and overlay space2 SRV/CBV slots @0..@15 on all retail descriptor tables
                            // so any lifted XDAT shader can bind its exact hardware descriptor slots.
                            int target_pi = -1;
                            int fallback_s2_space1_pi = -1;
                            int fallback_s2_any_pi = -1;
                            bool add_b7_alias = false;
                            UINT b0_table_off = 0u;
                            for (UINT pi = 0u; pi < aug.Desc_1_1.NumParameters && target_pi < 0; ++pi) {
                                const auto& rp = aug.Desc_1_1.pParameters[pi];
                                if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                                    !rp.DescriptorTable.pDescriptorRanges ||
                                    rp.DescriptorTable.NumDescriptorRanges == 0u) {
                                    continue;
                                }
                                bool has_b0_s1 = false;
                                bool has_b7_s1 = false;
                                bool has_any_s1 = false;
                                bool has_s2 = false;
                                bool is_sampler_table = false;
                                UINT cur_off = 0u;
                                for (UINT ri = 0u; ri < rp.DescriptorTable.NumDescriptorRanges; ++ri) {
                                    const auto& rg = rp.DescriptorTable.pDescriptorRanges[ri];
                                    const UINT off =
                                        (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                                            ? cur_off
                                            : rg.OffsetInDescriptorsFromTableStart;
                                    if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                                        is_sampler_table = true;
                                    }
                                    if (rg.RegisterSpace == 1u) {
                                        has_any_s1 = true;
                                    }
                                    if (rg.RegisterSpace == 2u) {
                                        has_s2 = true;
                                    }
                                    if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                                        rg.RegisterSpace == 1u) {
                                        if (rg.BaseShaderRegister <= 0u &&
                                            (rg.NumDescriptors == UINT_MAX ||
                                             rg.BaseShaderRegister + rg.NumDescriptors > 0u)) {
                                            has_b0_s1 = true;
                                            b0_table_off = off;
                                        }
                                        if (rg.BaseShaderRegister <= 7u &&
                                            (rg.NumDescriptors == UINT_MAX ||
                                             rg.BaseShaderRegister + rg.NumDescriptors > 7u)) {
                                            has_b7_s1 = true;
                                        }
                                    }
                                    cur_off = off + ((rg.NumDescriptors == UINT_MAX) ? 64u : rg.NumDescriptors);
                                }
                                if (has_b0_s1 && !has_b7_s1) {
                                    target_pi = static_cast<int>(pi);
                                    add_b7_alias = true;
                                } else if (!is_sampler_table && !has_s2) {
                                    if (has_any_s1 && fallback_s2_space1_pi < 0) {
                                        fallback_s2_space1_pi = static_cast<int>(pi);
                                    }
                                    if (fallback_s2_any_pi < 0) {
                                        fallback_s2_any_pi = static_cast<int>(pi);
                                    }
                                }
                            }
                            if (target_pi < 0) {
                                target_pi = (fallback_s2_space1_pi >= 0) ? fallback_s2_space1_pi : fallback_s2_any_pi;
                                add_b7_alias = false;
                            }
                            if (target_pi >= 0) {
                                params_1_1.assign(aug.Desc_1_1.pParameters,
                                                  aug.Desc_1_1.pParameters + aug.Desc_1_1.NumParameters);
                                const auto& dt = params_1_1[static_cast<size_t>(target_pi)].DescriptorTable;
                                expanded_ranges_1_1.reserve(dt.NumDescriptorRanges + 3u);
                                UINT cur_off = 0u;
                                bool has_s2 = false;
                                const bool scaleform_t2_b0_pair =
                                    add_b7_alias &&
                                    (dt.NumDescriptorRanges == 2u &&
                                     dt.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
                                     dt.pDescriptorRanges[0].BaseShaderRegister == 2u &&
                                     dt.pDescriptorRanges[0].NumDescriptors == 1u &&
                                     dt.pDescriptorRanges[0].RegisterSpace == 1u &&
                                     dt.pDescriptorRanges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                                     dt.pDescriptorRanges[1].BaseShaderRegister == 0u &&
                                     dt.pDescriptorRanges[1].NumDescriptors == 1u &&
                                     dt.pDescriptorRanges[1].RegisterSpace == 1u);
                                bool other_param_uses_s1_ext = false;
                                if (scaleform_t2_b0_pair) {
                                    const D3D12_SHADER_VISIBILITY target_vis =
                                        params_1_1[static_cast<size_t>(target_pi)].ShaderVisibility;
                                    for (UINT oi = 0u; oi < aug.Desc_1_1.NumParameters && !other_param_uses_s1_ext; ++oi) {
                                        if (static_cast<int>(oi) == target_pi) continue;
                                        const auto& orp = aug.Desc_1_1.pParameters[oi];
                                        const bool vis_overlap =
                                            (target_vis == D3D12_SHADER_VISIBILITY_ALL) ||
                                            (orp.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL) ||
                                            (target_vis == orp.ShaderVisibility);
                                        if (!vis_overlap) continue;
                                        if (orp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                                            orp.DescriptorTable.pDescriptorRanges) {
                                            for (UINT ori = 0u; ori < orp.DescriptorTable.NumDescriptorRanges; ++ori) {
                                                const auto& org = orp.DescriptorTable.pDescriptorRanges[ori];
                                                if (org.RegisterSpace != 1u || org.NumDescriptors == 0u) continue;
                                                const uint64_t rlo = org.BaseShaderRegister;
                                                const uint64_t rhi = (org.NumDescriptors == UINT_MAX)
                                                                         ? UINT64_MAX
                                                                         : (rlo + org.NumDescriptors - 1ull);
                                                if ((org.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                                                     rhi >= 1ull && rlo <= 31ull) ||
                                                    (org.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
                                                     rhi >= 3ull && rlo <= 15ull)) {
                                                    other_param_uses_s1_ext = true;
                                                    break;
                                                }
                                            }
                                        } else if (orp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                                                   orp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV) {
                                            if (orp.Descriptor.RegisterSpace == 1u) {
                                                if ((orp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV &&
                                                     orp.Descriptor.ShaderRegister >= 1u &&
                                                     orp.Descriptor.ShaderRegister <= 31u) ||
                                                    (orp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV &&
                                                     orp.Descriptor.ShaderRegister >= 3u &&
                                                     orp.Descriptor.ShaderRegister <= 15u)) {
                                                    other_param_uses_s1_ext = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                for (UINT ri = 0u; ri < dt.NumDescriptorRanges; ++ri) {
                                    D3D12_DESCRIPTOR_RANGE1 rg = dt.pDescriptorRanges[ri];
                                    const UINT off =
                                        (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                                            ? cur_off
                                            : rg.OffsetInDescriptorsFromTableStart;
                                    if (rg.RegisterSpace == 2u) has_s2 = true;
                                    rg.OffsetInDescriptorsFromTableStart =
                                        (scaleform_t2_b0_pair && ri == 1u) ? 0u : off;
                                    expanded_ranges_1_1.push_back(rg);
                                    cur_off = off + ((rg.NumDescriptors == UINT_MAX) ? 64u : rg.NumDescriptors);
                                }
                                if (add_b7_alias) {
                                    D3D12_DESCRIPTOR_RANGE1 b7_alias{};
                                    b7_alias.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                                    b7_alias.NumDescriptors = 1u;
                                    b7_alias.BaseShaderRegister = 7u;
                                    b7_alias.RegisterSpace = 1u;
                                    b7_alias.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                                    b7_alias.OffsetInDescriptorsFromTableStart =
                                        scaleform_t2_b0_pair ? 0u : b0_table_off;
                                    expanded_ranges_1_1.push_back(b7_alias);
                                    if (scaleform_t2_b0_pair && !other_param_uses_s1_ext) {
                                        // Retail UI/font pipelines sharing the {t2, b0} space1 root
                                        // signature all place their active CBV at table offset @0
                                        // under PSV0 registers b1..b15 (e.g. b1, b3, b10, b11).
                                        // Emit each register individually at @0 so b3/b10/b11 read
                                        // slot @0 rather than empty slots @2/@3, and alias t3..t15
                                        // individually at @1 without inflating table size past 16.
                                        for (UINT cb_reg = 1u; cb_reg <= 15u; ++cb_reg) {
                                            if (cb_reg == 7u) continue;
                                            D3D12_DESCRIPTOR_RANGE1 cb_r = b7_alias;
                                            cb_r.BaseShaderRegister = cb_reg;
                                            cb_r.NumDescriptors = 1u;
                                            cb_r.OffsetInDescriptorsFromTableStart = 0u;
                                            expanded_ranges_1_1.push_back(cb_r);
                                        }
                                        for (UINT srv_reg = 3u; srv_reg <= 15u; ++srv_reg) {
                                            D3D12_DESCRIPTOR_RANGE1 srv_r{};
                                            srv_r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                                            srv_r.NumDescriptors = 1u;
                                            srv_r.BaseShaderRegister = srv_reg;
                                            srv_r.RegisterSpace = 1u;
                                            srv_r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                                            srv_r.OffsetInDescriptorsFromTableStart = 1u;
                                            expanded_ranges_1_1.push_back(srv_r);
                                        }
                                    }
                                }
                                if (!has_s2) {
                                    for (UINT cpi = 0u; cpi < aug.Desc_1_1.NumParameters && !has_s2; ++cpi) {
                                        const auto& crp = aug.Desc_1_1.pParameters[cpi];
                                        if (crp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                                            crp.DescriptorTable.pDescriptorRanges) {
                                            for (UINT cri = 0u; cri < crp.DescriptorTable.NumDescriptorRanges; ++cri) {
                                                if (crp.DescriptorTable.pDescriptorRanges[cri].RegisterSpace == 2u) {
                                                    has_s2 = true;
                                                    break;
                                                }
                                            }
                                        } else if ((crp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                                                    crp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                                                    crp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) &&
                                                   crp.Descriptor.RegisterSpace == 2u) {
                                            has_s2 = true;
                                        }
                                    }
                                }
                                if (!has_s2) {
                                    const UINT s2_cbv_off = std::max<UINT>(16u, (cur_off + 15u) & ~15u);
                                    D3D12_DESCRIPTOR_RANGE1 s2_srv{};
                                    s2_srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                                    s2_srv.NumDescriptors = 16u;
                                    s2_srv.BaseShaderRegister = 0u;
                                    s2_srv.RegisterSpace = 2u;
                                    s2_srv.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
                                    s2_srv.OffsetInDescriptorsFromTableStart = 0u;
                                    D3D12_DESCRIPTOR_RANGE1 s98_srv = s2_srv;
                                    s98_srv.RegisterSpace = 98u;
                                    D3D12_DESCRIPTOR_RANGE1 s2_cbv = s2_srv;
                                    s2_cbv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                                    s2_cbv.OffsetInDescriptorsFromTableStart = s2_cbv_off;
                                    expanded_ranges_1_1.push_back(s2_srv);
                                    expanded_ranges_1_1.push_back(s98_srv);
                                    expanded_ranges_1_1.push_back(s2_cbv);
                                    if (params_1_1[static_cast<size_t>(target_pi)].ShaderVisibility ==
                                        D3D12_SHADER_VISIBILITY_ALL) {
                                        D3D12_DESCRIPTOR_RANGE1 s2_uav = s2_srv;
                                        s2_uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                                        s2_uav.OffsetInDescriptorsFromTableStart = s2_cbv_off + 16u;
                                        D3D12_DESCRIPTOR_RANGE1 s98_uav = s2_uav;
                                        s98_uav.RegisterSpace = 98u;
                                        expanded_ranges_1_1.push_back(s2_uav);
                                        expanded_ranges_1_1.push_back(s98_uav);
                                    }
                                }
                                params_1_1[static_cast<size_t>(target_pi)].DescriptorTable.NumDescriptorRanges =
                                    static_cast<UINT>(expanded_ranges_1_1.size());
                                params_1_1[static_cast<size_t>(target_pi)].DescriptorTable.pDescriptorRanges =
                                    expanded_ranges_1_1.data();
                                aug.Desc_1_1.pParameters = params_1_1.data();
                                static const bool trace_sf_rs = []() {
                                    char v[16]{};
                                    return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_SF_DRAW", v, sizeof(v)) > 0 && v[0] == '1';
                                }();
                                if (trace_sf_rs) {
                                    std::fprintf(stderr,
                                                 "[OpenScarlett][SF-RS] aliased b7->b0 space1 at param=%d off=%u (nRanges=%u nParams=%u nSamps=%u)\n",
                                                 target_pi, b0_table_off,
                                                 static_cast<unsigned>(expanded_ranges_1_1.size()),
                                                 aug.Desc_1_1.NumParameters,
                                                 aug.Desc_1_1.NumStaticSamplers);
                                    std::fflush(stderr);
                                }
                            }
                        }
                        if (aug.Desc_1_1.pStaticSamplers && aug.Desc_1_1.NumStaticSamplers > 0u) {
                            samps.insert(samps.begin(), aug.Desc_1_1.pStaticSamplers,
                                         aug.Desc_1_1.pStaticSamplers + aug.Desc_1_1.NumStaticSamplers);
                        } else if (samps.empty() && aug.Desc_1_1.pParameters) {
                            bool has_sampler_table = false;
                            bool has_space1 = false;
                            for (UINT pi = 0u; pi < aug.Desc_1_1.NumParameters; ++pi) {
                                const auto& rp = aug.Desc_1_1.pParameters[pi];
                                if (rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                                    rp.DescriptorTable.pDescriptorRanges) {
                                    for (UINT ri = 0u; ri < rp.DescriptorTable.NumDescriptorRanges; ++ri) {
                                        const auto& rg = rp.DescriptorTable.pDescriptorRanges[ri];
                                        if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                                            has_sampler_table = true;
                                        }
                                        if (rg.RegisterSpace == 1u) {
                                            has_space1 = true;
                                        }
                                    }
                                }
                            }
                            if (has_space1 && !has_sampler_table) {
                                for (UINT sreg = 0u; sreg < 22u; ++sreg) {
                                    D3D12_STATIC_SAMPLER_DESC ss = fallback_samp;
                                    ss.ShaderRegister = sreg;
                                    ss.RegisterSpace = 1u;
                                    samps.push_back(ss);
                                }
                            }
                        }
                        samps.push_back(fallback_samp);
                        aug.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samps.size());
                        aug.Desc_1_1.pStaticSamplers = samps.data();
                    } else {
                        sanitize_flags(aug.Desc_1_0.Flags);
                        if (aug.Desc_1_0.pStaticSamplers && aug.Desc_1_0.NumStaticSamplers > 0u) {
                            samps.assign(aug.Desc_1_0.pStaticSamplers,
                                         aug.Desc_1_0.pStaticSamplers + aug.Desc_1_0.NumStaticSamplers);
                        }
                        samps.push_back(fallback_samp);
                        aug.Desc_1_0.NumStaticSamplers = static_cast<UINT>(samps.size());
                        aug.Desc_1_0.pStaticSamplers = samps.data();
                    }
                    ID3DBlob* aug_blob = nullptr;
                    ID3DBlob* err_blob = nullptr;
                    if (SUCCEEDED(fn_ser(&aug, &aug_blob, &err_blob)) && aug_blob) {
                        hr = host_dev_->CreateRootSignature(
                            0u, aug_blob->GetBufferPointer(), aug_blob->GetBufferSize(),
                            OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
                        if (SUCCEEDED(hr) && host_rs) {
                            RecordRootSigTableSizes(host_rs, aug);
                        }
                        if (SUCCEEDED(hr) && host_rs &&
                            aug.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
                            std::string rsd = "{";
                            for (UINT pi = 0u; pi < aug.Desc_1_1.NumParameters; ++pi) {
                                const auto& rp = aug.Desc_1_1.pParameters[pi];
                                char pb[96];
                                if (rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                                    std::snprintf(pb, sizeof(pb), "p%u:DT[", pi);
                                    rsd += pb;
                                    UINT cur = 0u;
                                    for (UINT ri = 0u; ri < rp.DescriptorTable.NumDescriptorRanges; ++ri) {
                                        const auto& rg = rp.DescriptorTable.pDescriptorRanges[ri];
                                        const UINT off = (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                                                             ? cur : rg.OffsetInDescriptorsFromTableStart;
                                        const char tc = (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) ? 'b'
                                                        : (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) ? 't'
                                                        : (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) ? 'u' : 's';
                                        std::snprintf(pb, sizeof(pb), "%s%c%u(n=%u,sp=%u)@%u",
                                                      ri ? "," : "", tc, rg.BaseShaderRegister,
                                                      rg.NumDescriptors, rg.RegisterSpace, off);
                                        rsd += pb;
                                        cur = off + rg.NumDescriptors;
                                    }
                                    rsd += "] ";
                                } else {
                                    std::snprintf(pb, sizeof(pb), "p%u:T%d ", pi, static_cast<int>(rp.ParameterType));
                                    rsd += pb;
                                }
                            }
                            char sb[48];
                            std::snprintf(sb, sizeof(sb), "samps=%u}", aug.Desc_1_1.NumStaticSamplers);
                            rsd += sb;
                            RecordDiagRootSig(host_rs, std::move(rsd));
                        }
                        aug_blob->Release();
                    }
                    if (err_blob) err_blob->Release();
                }
                deser->Release();
            }
        }
    }
    if (FAILED(hr) || !host_rs) {
        hr = host_dev_->CreateRootSignature(
            nodeMask, pBlobWithRootSignature, blobLengthInBytes,
            OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
        if ((FAILED(hr) || !host_rs) && nodeMask != 0u) {
            hr = host_dev_->CreateRootSignature(
                0u, pBlobWithRootSignature, blobLengthInBytes,
                OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
        }
        if (SUCCEEDED(hr) && host_rs && pBlobWithRootSignature != nullptr && blobLengthInBytes > 0u) {
            using PfnCreateVrsDeser = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
            auto fn_deser = reinterpret_cast<PfnCreateVrsDeser>(
                openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
            ID3D12VersionedRootSignatureDeserializer* raw_deser = nullptr;
            if (fn_deser && SUCCEEDED(fn_deser(
                    pBlobWithRootSignature, blobLengthInBytes,
                    OS_D3D12_IID(ID3D12VersionedRootSignatureDeserializer),
                    reinterpret_cast<void**>(&raw_deser))) && raw_deser) {
                if (const auto* raw_desc = raw_deser->GetUnconvertedRootSignatureDesc()) {
                    RecordRootSigTableSizes(host_rs, *raw_desc);
                }
                raw_deser->Release();
            }
        }
    }
    if ((FAILED(hr) || !host_rs) && pBlobWithRootSignature != nullptr && blobLengthInBytes > 0u) {
        using PfnCreateVrsDeser = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
        using PfnSerVrs = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
        auto fn_deser = reinterpret_cast<PfnCreateVrsDeser>(
            openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
        auto fn_ser = reinterpret_cast<PfnSerVrs>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
        if (fn_deser && fn_ser) {
            ID3D12VersionedRootSignatureDeserializer* deser = nullptr;
            if (SUCCEEDED(fn_deser(
                    pBlobWithRootSignature, blobLengthInBytes,
                    OS_D3D12_IID(ID3D12VersionedRootSignatureDeserializer),
                    reinterpret_cast<void**>(&deser))) && deser) {
                const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* orig = deser->GetUnconvertedRootSignatureDesc();
                if (orig) {
                    D3D12_VERSIONED_ROOT_SIGNATURE_DESC safe = *orig;
                    auto sanitize_flags = [&](D3D12_ROOT_SIGNATURE_FLAGS& f) {
                        UINT bits = static_cast<UINT>(f);
                        // Strip Xbox-only extended high flag bits above standard desktop flags.
                        bits &= 0x0FFFu;
                        if (adapter_info_.raytracing_tier == 0u) {
                            bits &= ~static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);
                        }
                        if (adapter_info_.highest_shader_model < 0x66u) {
                            bits &= ~(0x400u | 0x800u);  // CBV_SRV_UAV / SAMPLER_HEAP_DIRECTLY_INDEXED
                        }
                        if (retail_scarlett_abi_ &&
                            (bits & static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE)) == 0u) {
                            bits |= static_cast<UINT>(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
                        }
                        f = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(bits);
                    };
                    if (safe.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
                        sanitize_flags(safe.Desc_1_0.Flags);
                    } else if (safe.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
                        sanitize_flags(safe.Desc_1_1.Flags);
                    }
                    ID3DBlob* safe_blob = nullptr;
                    ID3DBlob* err_blob = nullptr;
                    if (SUCCEEDED(fn_ser(&safe, &safe_blob, &err_blob)) && safe_blob) {
                        hr = host_dev_->CreateRootSignature(
                            0u, safe_blob->GetBufferPointer(), safe_blob->GetBufferSize(),
                            OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
                        if (SUCCEEDED(hr) && host_rs) {
                            RecordRootSigTableSizes(host_rs, safe);
                        }
                        safe_blob->Release();
                    }
                    if (err_blob) err_blob->Release();
                }
                deser->Release();
            }
        }
        // Final fallback for retail Scarlett on non-DXR/SM6.5 desktop GPUs (e.g. DXR local root
        // signatures or Xbox-only root signature blobs): create a valid empty root signature so
        // non-fatal subsystem init (like InitRayTracing on GTX 1650 Ti) can proceed cleanly.
        bool used_empty_fallback = false;
        if ((FAILED(hr) || !host_rs) && retail_scarlett_abi_ && fn_ser) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC empty_desc{};
            empty_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
            empty_desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            ID3DBlob* empty_blob = nullptr;
            ID3DBlob* err_blob = nullptr;
            if (SUCCEEDED(fn_ser(&empty_desc, &empty_blob, &err_blob)) && empty_blob) {
                hr = host_dev_->CreateRootSignature(
                    0u, empty_blob->GetBufferPointer(), empty_blob->GetBufferSize(),
                    OS_D3D12_IID(ID3D12RootSignature), reinterpret_cast<void**>(&host_rs));
                if (SUCCEEDED(hr) && host_rs) {
                    used_empty_fallback = true;
                }
                empty_blob->Release();
            }
            if (err_blob) err_blob->Release();
        }
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
        if (used_empty_fallback) {
            proxy->SetEmptyFallback(true);
        }
        hr = proxy->QueryInterface(riid, ppvRootSignature);
        proxy->Release();
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    if (FAILED(hr) || !host_rs) {
        RecordDev("ID3D12DeviceX::CreateRootSignature", 16, hr);
        return hr;
    }
    if (retail_scarlett_abi_ && pBlobWithRootSignature && blobLengthInBytes > 0u) {
        static const bool trace_rs = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RS", v, sizeof(v)) > 0 && v[0] != '0';
        }();
        static std::atomic<uint32_t> s_rs_log{0u};
        if (trace_rs) {
            const uint32_t ri = s_rs_log.fetch_add(1u, std::memory_order_relaxed);
            if (ri < 12u) {
                using PfnDeser = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
                auto fn_deser = reinterpret_cast<PfnDeser>(
                    openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
                ID3D12VersionedRootSignatureDeserializer* deser = nullptr;
                if (fn_deser && SUCCEEDED(fn_deser(
                        pBlobWithRootSignature, blobLengthInBytes,
                        OS_D3D12_IID(ID3D12VersionedRootSignatureDeserializer),
                        reinterpret_cast<void**>(&deser))) && deser) {
                    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d = deser->GetUnconvertedRootSignatureDesc();
                    auto dump_params = [&](UINT np, const auto* params, UINT ns, UINT flags, const char* ver) {
                        std::fprintf(stderr, "[OpenScarlett][RS #%u] ptr=%p %s nParams=%u nSamplers=%u flags=0x%X\n",
                                     ri, static_cast<void*>(host_rs), ver, np, ns, flags);
                        for (UINT p = 0u; p < np; ++p) {
                            const auto& rp = params[p];
                            if (rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                                std::fprintf(stderr, "  p[%u] TABLE vis=%d nRanges=%u:", p,
                                             static_cast<int>(rp.ShaderVisibility), rp.DescriptorTable.NumDescriptorRanges);
                                for (UINT r = 0u; r < rp.DescriptorTable.NumDescriptorRanges; ++r) {
                                    const auto& rg = rp.DescriptorTable.pDescriptorRanges[r];
                                    std::fprintf(stderr, " [type=%d base=%u space=%u num=%u off=%u]",
                                                 static_cast<int>(rg.RangeType), rg.BaseShaderRegister,
                                                 rg.RegisterSpace, rg.NumDescriptors, rg.OffsetInDescriptorsFromTableStart);
                                }
                                std::fprintf(stderr, "\n");
                            } else if (rp.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                                std::fprintf(stderr, "  p[%u] CONSTS vis=%d b%u space=%u n32=%u\n",
                                             p, static_cast<int>(rp.ShaderVisibility),
                                             rp.Constants.ShaderRegister, rp.Constants.RegisterSpace, rp.Constants.Num32BitValues);
                            } else {
                                std::fprintf(stderr, "  p[%u] ROOT_DESC(type=%d) vis=%d reg=%u space=%u\n",
                                             p, static_cast<int>(rp.ParameterType), static_cast<int>(rp.ShaderVisibility),
                                             rp.Descriptor.ShaderRegister, rp.Descriptor.RegisterSpace);
                            }
                        }
                    };
                    if (d && d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
                        dump_params(d->Desc_1_1.NumParameters, d->Desc_1_1.pParameters,
                                    d->Desc_1_1.NumStaticSamplers, static_cast<UINT>(d->Desc_1_1.Flags), "v1.1");
                    } else if (d && d->Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
                        dump_params(d->Desc_1_0.NumParameters, d->Desc_1_0.pParameters,
                                    d->Desc_1_0.NumStaticSamplers, static_cast<UINT>(d->Desc_1_0.Flags), "v1.0");
                    }
                    std::fflush(stderr);
                    deser->Release();
                }
            }
        }
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
    if (DestDescriptor.ptr == 0u) {
        RecordDev("ID3D12DeviceX::CreateConstantBufferView", 17, S_OK);
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC translated{};
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc_ptr = pDesc;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = pDesc ? pDesc->BufferLocation : 0ull;
    bool cbv_uploaded_to_ring = false;
    uint32_t cbv_aligned_size = 0u;
    if (pDesc && (pDesc->BufferLocation == 0ull || pDesc->SizeInBytes == 0u)) {
        translated.BufferLocation = 0ull;
        translated.SizeInBytes = 0u;
        desc_ptr = &translated;
        gpu_va = 0ull;
    } else if (pDesc && pDesc->BufferLocation != 0ull) {
        D3D12_GPU_VIRTUAL_ADDRESS mapped_va = TranslateGuestGpuVa(pDesc->BufferLocation);
        const UINT clamped_raw = std::min<UINT>(pDesc->SizeInBytes, 65536u);
        const UINT aligned_size = std::clamp<UINT>((clamped_raw + 255u) & ~255u, 256u, 65536u);
        cbv_aligned_size = aligned_size;
        bool uploaded_to_ring = false;
        uint64_t host_buf_off = 0ull;
        ID3D12Resource* host_buf = ResolveBufferResourceByGpuVa(pDesc->BufferLocation, &host_buf_off);
        const bool host_buf_gpu_written =
            host_buf != nullptr && IsGpuWrittenBufferRange(host_buf, host_buf_off, aligned_size);
        const bool host_buf_fits_aligned =
            host_buf != nullptr && (mapped_va & 255ull) == 0ull &&
            (host_buf_off + static_cast<uint64_t>(aligned_size) <= host_buf->GetDesc().Width);
        if (retail_scarlett_abi_ &&
            (mapped_va == pDesc->BufferLocation || !host_buf_fits_aligned || !host_buf_gpu_written)) {
            const uint64_t upload_len = std::max<uint64_t>(
                256ull, static_cast<uint64_t>(clamped_raw > 0u ? clamped_raw : aligned_size));
            const D3D12_GPU_VIRTUAL_ADDRESS uploaded = UploadTransientGuestBufferForDesktop(
                pDesc->BufferLocation, std::min<uint64_t>(upload_len, aligned_size), 256ull,
                /*allow_same_frame_cache=*/false);
            if (uploaded != 0ull) {
                mapped_va = uploaded;
                uploaded_to_ring = true;
            }
        } else if ((mapped_va & 255ull) != 0ull) {
            mapped_va = AlignRootCbvGpuVaForDesktop(mapped_va, pDesc->BufferLocation);
            uploaded_to_ring = (mapped_va != 0ull);
        }
        cbv_uploaded_to_ring = uploaded_to_ring;
        if (!uploaded_to_ring && !host_buf_fits_aligned) {
            translated.BufferLocation = 0ull;
            translated.SizeInBytes = 0u;
            desc_ptr = &translated;
            gpu_va = 0ull;
        } else if (mapped_va != pDesc->BufferLocation || aligned_size != pDesc->SizeInBytes) {
            translated = *pDesc;
            translated.BufferLocation = mapped_va;
            translated.SizeInBytes = aligned_size;
            desc_ptr = &translated;
            gpu_va = mapped_va;
        }
    }
    const bool use_shadow =
        retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
    uint64_t guest_wb = 0ull;
    D3D12_CPU_DESCRIPTOR_HANDLE eff_dest = DestDescriptor;
    if (use_shadow) {
        RedirectGuestDescDestIfNeeded(eff_dest, guest_wb);
    }
    host_dev_->CreateConstantBufferView(desc_ptr, eff_dest);
    CheckCpuDeviceRemoved("CreateConstantBufferView", gpu_va, desc_ptr ? desc_ptr->SizeInBytes : 0u);
    uint64_t cbv_shadow_ptr = 0ull;
    if (use_shadow) {
        SnapshotCreatedDescriptorToShadow(eff_dest, guest_wb, /*prefer_static_partition=*/false, &cbv_shadow_ptr);
    }
    if (retail_scarlett_abi_ && pDesc && pDesc->BufferLocation != 0ull && cbv_aligned_size > 0u) {
        uint64_t chk_off = 0ull;
        ID3D12Resource* chk_buf = ResolveBufferResourceByGpuVa(pDesc->BufferLocation, &chk_off);
        const uint64_t guest_cpu_va = ResolveGuestCpuAliasVa(pDesc->BufferLocation);
        const bool refresh_from_guest =
            cbv_uploaded_to_ring ||
            (!IsGpuWrittenBufferRange(chk_buf, chk_off, cbv_aligned_size) &&
             IsReadableGuestVaRange(guest_cpu_va, 16ull));
        std::lock_guard<std::mutex> lk(guest_dh_mutex_);
        const uint64_t raw_len = std::max<uint32_t>(
            256u, std::min<uint32_t>(cbv_aligned_size, std::max<uint32_t>(pDesc->SizeInBytes, 256u)));
        const ShadowCbvSource src{refresh_from_guest ? guest_cpu_va : 0ull, static_cast<uint32_t>(raw_len)};
        if (cbv_shadow_ptr != 0ull) {
            cbv_shadow_ptrs_.insert(cbv_shadow_ptr);
            shadow_cbv_sources_[cbv_shadow_ptr] = src;
        }
        if (eff_dest.ptr == DestDescriptor.ptr) {
            staging_cbv_sources_[DestDescriptor.ptr] = src;
        }
    }
    static const bool trace_sf_cbv = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_SF_DRAW", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_sf_cbv_log{0u};
    if (trace_sf_cbv && pDesc && pDesc->BufferLocation != 0ull &&
        (pDesc->SizeInBytes == 64u || pDesc->SizeInBytes == 256u ||
         pDesc->SizeInBytes == 1024u || pDesc->SizeInBytes == 4096u ||
         pDesc->BufferLocation < 0x10000000ull) &&
        s_sf_cbv_log.fetch_add(1u, std::memory_order_relaxed) < 12u) {
        std::fprintf(stderr,
                     "[OpenScarlett][SF-CBV] cpu=0x%llX raw=0x%llX -> host=0x%llX sz=%u",
                     static_cast<unsigned long long>(DestDescriptor.ptr),
                     static_cast<unsigned long long>(pDesc->BufferLocation),
                     static_cast<unsigned long long>(gpu_va),
                     pDesc->SizeInBytes);
        float f[16]{};
        if (SafeCopyGuestMemory(f, reinterpret_cast<const void*>(static_cast<uintptr_t>(pDesc->BufferLocation)), 64u)) {
            std::fprintf(stderr,
                         " f0..7=[%.3f,%.3f,%.3f,%.3f | %.3f,%.3f,%.3f,%.3f] f8..15=[%.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f]",
                         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                         f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    static const bool trace_dd = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DRAW_DATA", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_cbv_log{0u};
    if (trace_dd && pDesc && s_cbv_log.fetch_add(1u, std::memory_order_relaxed) < 20u) {
        std::fprintf(stderr,
                     "[OpenScarlett][CBV] cpu=0x%llX raw=0x%llX -> host=0x%llX sz=%u",
                     static_cast<unsigned long long>(DestDescriptor.ptr),
                     static_cast<unsigned long long>(pDesc->BufferLocation),
                     static_cast<unsigned long long>(gpu_va),
                     pDesc->SizeInBytes);
        void* mcpu = nullptr;
        uint64_t mrem = 0ull;
        if (pDesc->BufferLocation >= 0x10000000ull &&
            title_va_mirror_.ResolveTitleVaToCpu(pDesc->BufferLocation, mcpu, mrem) && mcpu) {
            const auto* bp = static_cast<const uint8_t*>(mcpu);
            if (pDesc->SizeInBytes == 96u && mrem >= 48u) {
                float c[4]{};
                std::memcpy(c, bp + 32u, 16u);
                std::fprintf(stderr, " b27[32]=(%.3f,%.3f,%.3f,%.3f)", c[0], c[1], c[2], c[3]);
            } else if (pDesc->SizeInBytes == 480u && mrem >= 416u) {
                float m[16]{};
                std::memcpy(m, bp + 352u, 64u);
                std::fprintf(stderr, " vp=[%.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f]",
                             m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                             m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    TrackPagedDescriptorWrite(DestDescriptor);
    if (CommandListX::IsDiagTraceEnabled() && pDesc) {
        char db[128];
        std::snprintf(db, sizeof(db), "CBV(raw=0x%llX->0x%llX sz=%u)",
                      static_cast<unsigned long long>(pDesc->BufferLocation),
                      static_cast<unsigned long long>(gpu_va), pDesc->SizeInBytes);
        RecordDiagDescriptor(DestDescriptor.ptr, db);
    }
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DestDescriptor, nullptr, gpu_va,
             descriptor_create_ctx_.load(std::memory_order_acquire));
    }
    RecordDev("ID3D12DeviceX::CreateConstantBufferView", 17, S_OK);
}

void DeviceX::CreatePlacedShaderResourceViewX(
    const D3D12_RESOURCE_DESC* pResourceDesc,
    const void* pViewDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    if (!host_dev_ || !pResourceDesc || !pViewDesc || destDescriptor.ptr == 0u) return;
    uint64_t gpu_va = 0ull;
    std::memcpy(&gpu_va, static_cast<const uint8_t*>(pViewDesc) + 0x28u, sizeof(gpu_va));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    std::memcpy(&srv, pViewDesc, sizeof(srv));
    if (srv.Shader4ComponentMapping == 0u) {
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }

    if (pResourceDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER && gpu_va != 0ull) {
        openscarlett::TitleVaRegionInfo rinfo{};
        const bool have_mirror =
            title_va_mirror_.LookupRegion(gpu_va, rinfo) && (rinfo.gpu_shadow_resource != nullptr);
        uint64_t host_buf_off = 0ull;
        ID3D12Resource* placed_host_buf = ResolveBufferResourceByGpuVa(gpu_va, &host_buf_off);
        uint32_t stride = srv.Buffer.StructureByteStride;
        if ((srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0u ||
            srv.Format == DXGI_FORMAT_R32_TYPELESS) {
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            srv.Buffer.StructureByteStride = 0u;
            stride = 4u;
        } else if (stride == 0u) {
            stride = 4u;
        }
        const uint64_t orig_first = srv.Buffer.FirstElement;
        const uint64_t eff_va = gpu_va + orig_first * stride;
        const uint64_t eff_cpu_va = ResolveGuestCpuAliasVa(eff_va);
        const uint64_t max_req_bytes =
            (srv.Buffer.NumElements > 0u)
                ? std::min<uint64_t>(pResourceDesc->Width, static_cast<uint64_t>(srv.Buffer.NumElements) * stride)
                : pResourceDesc->Width;
        const uint64_t cpu_span =
            MaxReadableGuestSpan(eff_cpu_va, std::min<uint64_t>(std::max<uint64_t>(max_req_bytes, stride), 262144ull));
        if (stride > 0u && (have_mirror || placed_host_buf != nullptr || cpu_span >= stride)) {
            const bool use_shadow =
                retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
            uint64_t guest_wb = 0ull;
            D3D12_CPU_DESCRIPTOR_HANDLE eff_dest = destDescriptor;
            if (use_shadow) {
                RedirectGuestDescDestIfNeeded(eff_dest, guest_wb);
            }
            ID3D12Resource* srv_res = nullptr;
            bool used_ring = false;
            const bool is_raw_or_structured =
                ((srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0u) ||
                (srv.Buffer.StructureByteStride > 0u);
            const uint64_t eff_want = (cpu_span >= stride) ? cpu_span : max_req_bytes;
            const uint64_t placed_sub_off = host_buf_off + orig_first * stride;
            if (is_raw_or_structured &&
                (!placed_host_buf || !IsGpuWrittenBufferRange(placed_host_buf, placed_sub_off, eff_want)) &&
                cpu_span >= stride && eff_want <= 65536ull && (256u % stride) == 0u) {
                const D3D12_GPU_VIRTUAL_ADDRESS ring_gpu = UploadTransientGuestBufferForDesktop(
                    eff_cpu_va, eff_want, 256ull, /*allow_same_frame_cache=*/false);
                if (ring_gpu != 0ull && transient_ring_ && ring_gpu >= transient_ring_gpu_) {
                    const uint64_t ring_off = ring_gpu - transient_ring_gpu_;
                    srv.Buffer.FirstElement = ring_off / stride;
                    srv.Buffer.NumElements = static_cast<UINT>(eff_want / stride);
                    srv_res = transient_ring_;
                    used_ring = true;
                }
            }
            if (!used_ring && placed_host_buf) {
                const uint64_t hb_width = placed_host_buf->GetDesc().Width;
                const uint64_t tot_off = host_buf_off + orig_first * stride;
                if (tot_off + stride <= hb_width) {
                    srv.Buffer.FirstElement = tot_off / stride;
                    const uint64_t max_elems = (hb_width - tot_off) / stride;
                    if (srv.Buffer.NumElements == 0u || srv.Buffer.NumElements > max_elems) {
                        srv.Buffer.NumElements = static_cast<UINT>(max_elems);
                    }
                    srv_res = placed_host_buf;
                }
            }
            if (!used_ring && !srv_res && have_mirror) {
                const uint64_t byte_off = (gpu_va >= rinfo.title_va) ? (gpu_va - rinfo.title_va) : 0ull;
                const uint64_t avail = (rinfo.size_bytes > byte_off) ? (rinfo.size_bytes - byte_off) : 0ull;
                if (avail >= stride) {
                    srv.Buffer.FirstElement = orig_first + (byte_off / stride);
                    const uint64_t max_elems = avail / stride;
                    if (srv.Buffer.NumElements == 0u || srv.Buffer.NumElements > max_elems) {
                        srv.Buffer.NumElements = static_cast<UINT>(max_elems);
                    }
                    uint64_t dummy_pc = 0ull, dummy_off = 0ull;
                    (void)title_va_mirror_.ResolveTitleVa(gpu_va, dummy_pc, dummy_off);
                    srv_res = rinfo.gpu_shadow_resource;
                }
            }
            if (srv_res) {
                host_dev_->CreateShaderResourceView(srv_res, &srv, eff_dest);
                CheckCpuDeviceRemoved("CreatePlacedShaderResourceViewX[Buf]", gpu_va, srv.Buffer.NumElements);
                uint64_t buf_sh_ptr = 0ull;
                if (use_shadow) {
                    SnapshotCreatedDescriptorToShadow(eff_dest, guest_wb, /*prefer_static_partition=*/false, &buf_sh_ptr);
                }
                if (retail_scarlett_abi_) {
                    ShadowBufSrvSource bsrc{};
                    bsrc.host_res = placed_host_buf;
                    bsrc.guest_cpu_va = (cpu_span >= 4ull) ? eff_cpu_va : 0ull;
                    bsrc.host_byte_off = host_buf_off + orig_first * stride;
                    bsrc.byte_size = std::max<uint64_t>(stride, eff_want);
                    bsrc.stride = (srv.Buffer.StructureByteStride > 0u) ? srv.Buffer.StructureByteStride : stride;
                    bsrc.format = srv.Format;
                    bsrc.flags = srv.Buffer.Flags;
                    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
                    if (buf_sh_ptr != 0ull) {
                        shadow_buf_srv_sources_[buf_sh_ptr] = bsrc;
                    }
                    if (eff_dest.ptr == destDescriptor.ptr) {
                        staging_buf_srv_sources_[destDescriptor.ptr] = bsrc;
                    }
                }
                TrackPagedDescriptorWrite(destDescriptor);
                if (CommandListX::IsDiagTraceEnabled()) {
                    char db[128];
                    std::snprintf(db, sizeof(db), "PLACED_BUF_SRV(va=0x%llX elems=%u)",
                                  static_cast<unsigned long long>(gpu_va), srv.Buffer.NumElements);
                    RecordDiagDescriptor(destDescriptor.ptr, db);
                }
                return;
            }
        }
    }
    if (pResourceDesc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER && gpu_va != 0ull) {
        ID3D12Resource* tex_res = ResolvePlacedTextureByGpuVa(gpu_va, pResourceDesc);
        if (!tex_res && retail_scarlett_abi_ &&
            (pResourceDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
             pResourceDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
             pResourceDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) &&
            pResourceDesc->Width >= 1ull && pResourceDesc->Width <= 16384ull &&
            pResourceDesc->Height >= 1u && pResourceDesc->Height <= 16384u &&
            pResourceDesc->SampleDesc.Count <= 1u) {
            const uint64_t cpu_va = ResolveGuestCpuAliasVa(gpu_va);
            if (IsReadableGuestVaRange(cpu_va, 256ull)) {
                const XG_TILE_MODE tile_mode = ResolveRetailXboxTileMode(*pResourceDesc);
                D3D12_RESOURCE_DESC lazy_desc = SanitizeAndClampResourceDesc(*pResourceDesc);
                const DXGI_FORMAT orig_san_fmt = SanitizeXboxDxgiFormat(lazy_desc.Format);
                lazy_desc.Format = orig_san_fmt;
                lazy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                lazy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                if (!IsDepthOrPlanarFormatForCopy(orig_san_fmt) &&
                    orig_san_fmt != DXGI_FORMAT_UNKNOWN) {
                    DXGI_FORMAT uav_chk_fmt = MapTypelessFormatForSrv(orig_san_fmt);
                    if (uav_chk_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                        uav_chk_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                    } else if (uav_chk_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                        uav_chk_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                    } else if (uav_chk_fmt == DXGI_FORMAT_A8_UNORM) {
                        uav_chk_fmt = DXGI_FORMAT_R8_UNORM;
                    }
                    if (orig_san_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                        lazy_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                    } else if (orig_san_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                        lazy_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                    } else if (orig_san_fmt == DXGI_FORMAT_A8_UNORM) {
                        lazy_desc.Format = DXGI_FORMAT_R8_UNORM;
                    }
                    if (!FormatForbidsDesktopUav(uav_chk_fmt) && HostFormatSupportsTypedUav(uav_chk_fmt)) {
                        lazy_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    }
                    D3D12_HEAP_PROPERTIES def_hp{};
                    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    ID3D12Resource* lazy_tex = nullptr;
                    HRESULT lazy_hr = host_dev_->CreateCommittedResource(
                        &def_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                    if ((FAILED(lazy_hr) || !lazy_tex) &&
                        (lazy_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
                        lazy_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                        lazy_desc.Format = orig_san_fmt;
                        lazy_hr = host_dev_->CreateCommittedResource(
                            &def_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                    }
                    if (FAILED(lazy_hr) || !lazy_tex) {
                        D3D12_HEAP_PROPERTIES l0_hp{};
                        l0_hp.Type = D3D12_HEAP_TYPE_CUSTOM;
                        l0_hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
                        l0_hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                        l0_hp.CreationNodeMask = 1u;
                        l0_hp.VisibleNodeMask = 1u;
                        lazy_hr = host_dev_->CreateCommittedResource(
                            &l0_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                    }
                    if (SUCCEEDED(lazy_hr) && lazy_tex) {
                        if (tile_mode != XG_TILE_MODE_LINEAR) {
                            SetTrackedResourceTileMode(lazy_tex, tile_mode);
                        }
                        TrackPlacedTextureVa(lazy_tex, gpu_va);
                        SetRetailResourceState(lazy_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        D3D12_RESOURCE_DESC sync_desc = lazy_desc;
                        sync_desc.Format = uav_chk_fmt;
                        AutoRegisterUnifiedResource(
                            lazy_tex, cpu_va, sync_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            /*mirrored_by_title_va=*/false,
                            /*use_footprint_row_pitch=*/(pResourceDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
                        static std::atomic<uint32_t> s_placed_srv_tex_log{0u};
                        if (s_placed_srv_tex_log.fetch_add(1u, std::memory_order_relaxed) < 24u) {
                            std::fprintf(stderr,
                                         "[OpenScarlett][PLACED-SRV-TEX] %llux%ux%u mips=%u fmt=%d layout=0x%X tm=%u va=0x%llX\n",
                                         static_cast<unsigned long long>(lazy_desc.Width),
                                         lazy_desc.Height, lazy_desc.DepthOrArraySize, lazy_desc.MipLevels,
                                         static_cast<int>(lazy_desc.Format),
                                         static_cast<unsigned>(pResourceDesc->Layout),
                                         static_cast<unsigned>(tile_mode),
                                         static_cast<unsigned long long>(gpu_va));
                            std::fflush(stderr);
                        }
                        bool owned_by_unified = false;
                        {
                            std::lock_guard<std::mutex> ulk(unified_mutex_);
                            owned_by_unified = (unified_textures_.find(lazy_tex) != unified_textures_.end());
                        }
                        if (owned_by_unified) {
                            tex_res = lazy_tex;
                            lazy_tex->Release();  // unified_textures_ holds the lifetime ref
                        } else {
                            TrackTransientGpuResource(lazy_tex);
                            tex_res = lazy_tex;
                            lazy_tex->Release();
                        }
                    }
                }
            }
        }
        if (tex_res) {
            const D3D12_RESOURCE_DESC td = tex_res->GetDesc();
            const bool non_typeless_depth =
                ((td.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) &&
                ((td.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
                 td.Format == DXGI_FORMAT_D32_FLOAT ||
                 td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                 td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                 td.Format == DXGI_FORMAT_D16_UNORM);
            if (!non_typeless_depth && td.Format != DXGI_FORMAT_UNKNOWN) {
                D3D12_SHADER_RESOURCE_VIEW_DESC clean_srv{};
                const DXGI_FORMAT req_fmt = SanitizeXboxDxgiFormat(srv.Format);
                clean_srv.Format = ResolveCompatibleSrvFormat(td.Format, req_fmt);
                if (req_fmt == DXGI_FORMAT_A8_UNORM && clean_srv.Format == DXGI_FORMAT_R8_UNORM) {
                    clean_srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
                } else {
                    clean_srv.Shader4ComponentMapping =
                        SanitizeOrTranslateXboxShader4ComponentMapping(srv.Shader4ComponentMapping, clean_srv.Format);
                }

                const UINT max_mips = (td.MipLevels > 0u) ? td.MipLevels : 1u;
                bool view_valid = false;
                if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                    if (td.SampleDesc.Count > 1u) {
                        if (srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY || td.DepthOrArraySize > 1u) {
                            clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                            clean_srv.Texture2DMSArray.FirstArraySlice = 0u;
                            clean_srv.Texture2DMSArray.ArraySize = td.DepthOrArraySize;
                        } else {
                            clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                        }
                        view_valid = true;
                    } else if (srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE && td.DepthOrArraySize >= 6u) {
                        clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                        clean_srv.TextureCube.MostDetailedMip =
                            (srv.TextureCube.MostDetailedMip < max_mips) ? srv.TextureCube.MostDetailedMip : 0u;
                        const UINT rem = max_mips - clean_srv.TextureCube.MostDetailedMip;
                        clean_srv.TextureCube.MipLevels =
                            (srv.TextureCube.MipLevels > 0u && srv.TextureCube.MipLevels <= rem)
                                ? srv.TextureCube.MipLevels
                                : rem;
                        clean_srv.TextureCube.ResourceMinLODClamp = 0.0f;
                        view_valid = true;
                    } else if (srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY && td.DepthOrArraySize >= 1u) {
                        clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                        clean_srv.Texture2DArray.MostDetailedMip =
                            (srv.Texture2DArray.MostDetailedMip < max_mips) ? srv.Texture2DArray.MostDetailedMip : 0u;
                        const UINT rem = max_mips - clean_srv.Texture2DArray.MostDetailedMip;
                        clean_srv.Texture2DArray.MipLevels =
                            (srv.Texture2DArray.MipLevels > 0u && srv.Texture2DArray.MipLevels <= rem)
                                ? srv.Texture2DArray.MipLevels
                                : rem;
                        clean_srv.Texture2DArray.FirstArraySlice =
                            (srv.Texture2DArray.FirstArraySlice < td.DepthOrArraySize)
                                ? srv.Texture2DArray.FirstArraySlice
                                : 0u;
                        const UINT rem_arr = td.DepthOrArraySize - clean_srv.Texture2DArray.FirstArraySlice;
                        clean_srv.Texture2DArray.ArraySize =
                            (srv.Texture2DArray.ArraySize > 0u && srv.Texture2DArray.ArraySize <= rem_arr)
                                ? srv.Texture2DArray.ArraySize
                                : rem_arr;
                        clean_srv.Texture2DArray.PlaneSlice = 0u;
                        clean_srv.Texture2DArray.ResourceMinLODClamp = 0.0f;
                        view_valid = true;
                    } else {
                        clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        clean_srv.Texture2D.MostDetailedMip =
                            (srv.Texture2D.MostDetailedMip < max_mips) ? srv.Texture2D.MostDetailedMip : 0u;
                        const UINT rem = max_mips - clean_srv.Texture2D.MostDetailedMip;
                        clean_srv.Texture2D.MipLevels =
                            (srv.Texture2D.MipLevels > 0u && srv.Texture2D.MipLevels <= rem)
                                ? srv.Texture2D.MipLevels
                                : rem;
                        clean_srv.Texture2D.PlaneSlice = 0u;
                        clean_srv.Texture2D.ResourceMinLODClamp = 0.0f;
                        view_valid = true;
                    }
                } else if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                           srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) {
                    clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    clean_srv.Texture3D.MostDetailedMip =
                        (srv.Texture3D.MostDetailedMip < max_mips) ? srv.Texture3D.MostDetailedMip : 0u;
                    const UINT rem = max_mips - clean_srv.Texture3D.MostDetailedMip;
                    clean_srv.Texture3D.MipLevels =
                        (srv.Texture3D.MipLevels > 0u && srv.Texture3D.MipLevels <= rem)
                            ? srv.Texture3D.MipLevels
                            : rem;
                    clean_srv.Texture3D.ResourceMinLODClamp = 0.0f;
                    view_valid = true;
                } else if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                    clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                    clean_srv.Texture1D.MostDetailedMip = 0u;
                    clean_srv.Texture1D.MipLevels = max_mips;
                    clean_srv.Texture1D.ResourceMinLODClamp = 0.0f;
                    view_valid = true;
                }

                if (view_valid && clean_srv.Format != DXGI_FORMAT_UNKNOWN) {
                    const bool use_shadow =
                        retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
                    uint64_t guest_wb = 0ull;
                    D3D12_CPU_DESCRIPTOR_HANDLE eff_dest = destDescriptor;
                    if (use_shadow) {
                        RedirectGuestDescDestIfNeeded(eff_dest, guest_wb, /*prefer_static_partition=*/true);
                    }
                    host_dev_->CreateShaderResourceView(tex_res, &clean_srv, eff_dest);
                    CheckCpuDeviceRemoved("CreatePlacedShaderResourceViewX[Tex]", gpu_va, clean_srv.Format);
                    if (HasRegisteredUnifiedTextures()) {
                        std::lock_guard<std::mutex> ulk(unified_mutex_);
                        auto uit = unified_textures_.find(tex_res);
                        if (uit != unified_textures_.end() && uit->second.allow_cpu_upload && !uit->second.dirty) {
                            uit->second.dirty = true;
                            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
                        }
                    }
                    uint64_t tex_sh_ptr = 0ull;
                    if (use_shadow) {
                        SnapshotCreatedDescriptorToShadow(eff_dest, guest_wb, /*prefer_static_partition=*/true, &tex_sh_ptr);
                        if (tex_sh_ptr != 0ull) {
                            RegisterShadowSrvUavPair(tex_sh_ptr, tex_res, /*created_is_uav=*/false, clean_srv.Format);
                        }
                    }
                    TrackPagedDescriptorWrite(destDescriptor);
                    if (CommandListX::IsDiagTraceEnabled()) {
                        char db[128];
                        std::snprintf(db, sizeof(db), "PLACED_TEX_SRV(va=0x%llX %llux%u fmt=%d)",
                                      static_cast<unsigned long long>(gpu_va),
                                      static_cast<unsigned long long>(td.Width), td.Height,
                                      static_cast<int>(clean_srv.Format));
                        RecordDiagDescriptor(destDescriptor.ptr, db);
                    }
                    return;
                }
            }
        }
    }

    // Fallback null SRV so descriptors never hold uninitialized garbage.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Texture2D.MipLevels = 1u;
    if (srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        null_srv.Format = DXGI_FORMAT_R32_TYPELESS;
        null_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        null_srv.Buffer.NumElements = 1u;
        null_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    }
    const bool use_null_shadow =
        retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
    uint64_t guest_wb = 0ull;
    D3D12_CPU_DESCRIPTOR_HANDLE eff_dest = destDescriptor;
    if (use_null_shadow) {
        RedirectGuestDescDestIfNeeded(eff_dest, guest_wb);
    }
    host_dev_->CreateShaderResourceView(nullptr, &null_srv, eff_dest);
    CheckCpuDeviceRemoved("CreatePlacedShaderResourceViewX[Null]", gpu_va, null_srv.Format);
    if (use_null_shadow) {
        SnapshotCreatedDescriptorToShadow(eff_dest, guest_wb);
    }
    TrackPagedDescriptorWrite(destDescriptor);
    if (CommandListX::IsDiagTraceEnabled()) {
        char db[128];
        std::snprintf(db, sizeof(db), "PLACED_NULL_SRV(dim=%d vdim=%d va=0x%llX %llux%u fmt=%d)",
                      static_cast<int>(pResourceDesc->Dimension), static_cast<int>(srv.ViewDimension),
                      static_cast<unsigned long long>(gpu_va),
                      static_cast<unsigned long long>(pResourceDesc->Width), pResourceDesc->Height,
                      static_cast<int>(pResourceDesc->Format));
        RecordDiagDescriptor(destDescriptor.ptr, db);
    }
}

void STDMETHODCALLTYPE DeviceX::CreateShaderResourceView(
    ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (DestDescriptor.ptr == 0u) {
        RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
        return;
    }
    ID3D12Resource* host_res = UnwrapResource(pResource);
    const bool use_shadow =
        retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
    uint64_t guest_wb = 0ull;
    const D3D12_CPU_DESCRIPTOR_HANDLE orig_dest = DestDescriptor;
    const bool srv_is_static_tex =
        (host_res != nullptr && host_res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER);
    if (use_shadow) {
        RedirectGuestDescDestIfNeeded(DestDescriptor, guest_wb, srv_is_static_tex);
    }
    const auto notify_hook = [&](DXGI_FORMAT eff_srv_fmt, const D3D12_SHADER_RESOURCE_VIEW_DESC* active_srv = nullptr) {
        if (use_shadow) {
            uint64_t srv_sh_ptr = 0ull;
            SnapshotCreatedDescriptorToShadow(DestDescriptor, guest_wb, srv_is_static_tex, &srv_sh_ptr);
            if (retail_scarlett_abi_ && srv_sh_ptr != 0ull && host_res != nullptr) {
                RegisterShadowSrvUavPair(srv_sh_ptr, host_res, /*created_is_uav=*/false, eff_srv_fmt);
            }
            if (retail_scarlett_abi_ && host_res != nullptr && active_srv != nullptr &&
                active_srv->ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
                host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                uint64_t base_guest_va = 0ull;
                {
                    std::shared_lock<std::shared_mutex> slk(buffer_va_mutex_);
                    auto bit = buffer_va_map_.find(host_res);
                    if (bit != buffer_va_map_.end()) {
                        base_guest_va = bit->second.guest_va;
                    }
                }
                uint32_t stride = active_srv->Buffer.StructureByteStride;
                if ((active_srv->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0u ||
                    active_srv->Format == DXGI_FORMAT_R32_TYPELESS) {
                    stride = 4u;
                } else if (stride == 0u) {
                    stride = 4u;
                }
                const uint64_t byte_off = active_srv->Buffer.FirstElement * stride;
                const uint64_t res_w = host_res->GetDesc().Width;
                const uint64_t rem_w = (res_w > byte_off) ? (res_w - byte_off) : 0ull;
                const uint64_t req_bytes =
                    (active_srv->Buffer.NumElements > 0u)
                        ? std::min<uint64_t>(rem_w, static_cast<uint64_t>(active_srv->Buffer.NumElements) * stride)
                        : rem_w;
                if (req_bytes >= 4ull) {
                    const uint64_t eff_cpu_va =
                        (base_guest_va != 0ull) ? (ResolveGuestCpuAliasVa(base_guest_va) + byte_off) : 0ull;
                    ShadowBufSrvSource bsrc{};
                    bsrc.host_res = host_res;
                    bsrc.guest_cpu_va =
                        (eff_cpu_va != 0ull && IsReadableGuestVaRange(eff_cpu_va, 4ull)) ? eff_cpu_va : 0ull;
                    bsrc.host_byte_off = byte_off;
                    bsrc.byte_size = req_bytes;
                    bsrc.stride = active_srv->Buffer.StructureByteStride;
                    bsrc.format = active_srv->Format;
                    bsrc.flags = active_srv->Buffer.Flags;
                    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
                    if (srv_sh_ptr != 0ull) {
                        shadow_buf_srv_sources_[srv_sh_ptr] = bsrc;
                    }
                    if (DestDescriptor.ptr == orig_dest.ptr) {
                        staging_buf_srv_sources_[orig_dest.ptr] = bsrc;
                    }
                }
            }
        }
        if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
            const D3D12_GPU_VIRTUAL_ADDRESS va =
                (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                    ? host_res->GetGPUVirtualAddress()
                    : 0ull;
            hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, orig_dest, host_res, va,
                 descriptor_create_ctx_.load(std::memory_order_acquire));
        }
    };
    // Non-DXR desktop GPUs (e.g. GTX 1650 Ti, raytracing_tier == 0) immediately remove the device
    // with DXGI_ERROR_INVALID_CALL on D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE.
    // Substitute a valid null raw-buffer SRV so titles with RT enabled can run cleanly.
    if (pDesc != nullptr &&
        pDesc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE &&
        adapter_info_.raytracing_tier == 0u) {
        D3D12_SHADER_RESOURCE_VIEW_DESC fallback_desc{};
        fallback_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        fallback_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        fallback_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        fallback_desc.Buffer.FirstElement = 0ull;
        fallback_desc.Buffer.NumElements = 1u;
        fallback_desc.Buffer.StructureByteStride = 0u;
        fallback_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        host_dev_->CreateShaderResourceView(nullptr, &fallback_desc, DestDescriptor);
        notify_hook(fallback_desc.Format);
        RecordDev("ID3D12DeviceX::CreateShaderResourceView[NonDxrAsFallback]", 18, S_OK);
        RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
        return;
    }
    // R9G9B9E5_SHAREDEXP UAV fallback: rewrite explicit R9G9B9E5 SRV views to R16G16B16A16_FLOAT
    // when the backing resource was promoted to RGBA16F.
    if (pDesc != nullptr && pDesc->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
        host_res != nullptr &&
        (IsTrackedFallbackResource(host_res) ||
         host_res->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC rw_desc = *pDesc;
        rw_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        host_dev_->CreateShaderResourceView(host_res, &rw_desc, DestDescriptor);
        notify_hook(rw_desc.Format);
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
            notify_hook(explicit_desc.Format);
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
            notify_hook(explicit_desc.Format);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]", 18, S_OK);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
        if (res_desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
            res_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            D3D12_SHADER_RESOURCE_VIEW_DESC explicit_desc{};
            explicit_desc.Format = MapTypelessFormatForSrv(res_desc.Format);
            explicit_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                explicit_desc.Texture3D.MipLevels = std::max<UINT>(1u, res_desc.MipLevels);
            } else if (res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                explicit_desc.Texture1D.MipLevels = std::max<UINT>(1u, res_desc.MipLevels);
            } else {
                explicit_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                explicit_desc.Texture2D.MipLevels = std::max<UINT>(1u, res_desc.MipLevels);
            }
            host_dev_->CreateShaderResourceView(host_res, &explicit_desc, DestDescriptor);
            notify_hook(explicit_desc.Format);
            RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
            return;
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC clean_srv{};
    const D3D12_SHADER_RESOURCE_VIEW_DESC* srv_ptr = pDesc;
    if (pDesc != nullptr) {
        DXGI_FORMAT san_fmt = SanitizeXboxDxgiFormat(pDesc->Format);
        if (san_fmt == DXGI_FORMAT_D32_FLOAT || san_fmt == DXGI_FORMAT_D16_UNORM ||
            san_fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || san_fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            san_fmt = MapTypelessFormatForSrv(san_fmt);
        }
        bool remap_a8_to_r8 = false;
        if (san_fmt == DXGI_FORMAT_A8_UNORM && host_res != nullptr) {
            const DXGI_FORMAT rdf = host_res->GetDesc().Format;
            if (rdf == DXGI_FORMAT_R8_UNORM || rdf == DXGI_FORMAT_R8_TYPELESS) {
                san_fmt = DXGI_FORMAT_R8_UNORM;
                remap_a8_to_r8 = true;
            }
        }
        const UINT san_map = remap_a8_to_r8
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0)
            : SanitizeOrTranslateXboxShader4ComponentMapping(pDesc->Shader4ComponentMapping, san_fmt);
        if (san_fmt != pDesc->Format || san_map != pDesc->Shader4ComponentMapping) {
            clean_srv = *pDesc;
            clean_srv.Format = san_fmt;
            clean_srv.Shader4ComponentMapping = san_map;
            srv_ptr = &clean_srv;
        }
    }
    if (retail_scarlett_abi_ && host_res != nullptr) {
        const D3D12_RESOURCE_DESC rd = host_res->GetDesc();
        const bool non_srv_depth =
            ((rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) ||
            rd.Format == DXGI_FORMAT_D32_FLOAT ||
            rd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
            rd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
            rd.Format == DXGI_FORMAT_D16_UNORM;
        if (non_srv_depth) {
            host_res = nullptr;
            clean_srv = D3D12_SHADER_RESOURCE_VIEW_DESC{};
            clean_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (pDesc && pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
                clean_srv.Format = DXGI_FORMAT_R32_TYPELESS;
                clean_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                clean_srv.Buffer.NumElements = 1u;
                clean_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            } else {
                clean_srv.Format = DXGI_FORMAT_R32_FLOAT;
                clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                clean_srv.Texture2D.MipLevels = 1u;
            }
            srv_ptr = &clean_srv;
        }
    }
    if (host_res == nullptr &&
        (srv_ptr == nullptr ||
         (srv_ptr->Format == DXGI_FORMAT_UNKNOWN &&
          (srv_ptr->ViewDimension != D3D12_SRV_DIMENSION_BUFFER ||
           srv_ptr->Buffer.StructureByteStride == 0u)))) {
        clean_srv = D3D12_SHADER_RESOURCE_VIEW_DESC{};
        clean_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (srv_ptr && srv_ptr->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            clean_srv.Format = DXGI_FORMAT_R32_TYPELESS;
            clean_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            clean_srv.Buffer.NumElements = 1u;
            clean_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        } else {
            clean_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            clean_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            clean_srv.Texture2D.MipLevels = 1u;
        }
        srv_ptr = &clean_srv;
    }
    host_dev_->CreateShaderResourceView(host_res, srv_ptr, DestDescriptor);
    CheckCpuDeviceRemoved(
        "CreateShaderResourceView",
        reinterpret_cast<uint64_t>(host_res),
        srv_ptr ? static_cast<uint64_t>(srv_ptr->Format) : 0ull);
    static const bool trace_dd = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DRAW_DATA", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_srv_log{0u};
    if (trace_dd && pDesc &&
        (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER ||
         (DestDescriptor.ptr & 0xFFFF0000ull) == 0x303B0000ull ||
         s_srv_log.load(std::memory_order_relaxed) < 5u)) {
        if (s_srv_log.fetch_add(1u, std::memory_order_relaxed) < 40u) {
            std::fprintf(stderr,
                         "[OpenScarlett][SRV] cpu=0x%llX res=%p fmt=%d vdim=%d buf=(first=%llu num=%u stride=%u flags=%u)\n",
                         static_cast<unsigned long long>(DestDescriptor.ptr),
                         static_cast<void*>(host_res),
                         static_cast<int>(pDesc->Format),
                         static_cast<int>(pDesc->ViewDimension),
                         (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
                             ? static_cast<unsigned long long>(pDesc->Buffer.FirstElement)
                             : 0ull,
                         (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
                             ? pDesc->Buffer.NumElements
                             : 0u,
                         (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
                             ? pDesc->Buffer.StructureByteStride
                             : 0u,
                         (pDesc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
                             ? static_cast<unsigned>(pDesc->Buffer.Flags)
                             : 0u);
            std::fflush(stderr);
        }
    }
    TrackPagedDescriptorWrite(orig_dest);
    if (CommandListX::IsDiagTraceEnabled()) {
        char db[144];
        if (host_res) {
            const auto rd = host_res->GetDesc();
            std::snprintf(db, sizeof(db), "SRV(res=%p dim=%d %llux%u fmt=%d vdim=%d)",
                          static_cast<void*>(host_res), static_cast<int>(rd.Dimension),
                          static_cast<unsigned long long>(rd.Width), rd.Height,
                          static_cast<int>(rd.Format), pDesc ? static_cast<int>(pDesc->ViewDimension) : -1);
        } else {
            std::snprintf(db, sizeof(db), "SRV(NULL vdim=%d)", pDesc ? static_cast<int>(pDesc->ViewDimension) : -1);
        }
        RecordDiagDescriptor(orig_dest.ptr, db);
    }
    notify_hook(
        srv_ptr ? srv_ptr->Format : (host_res ? MapTypelessFormatForSrv(host_res->GetDesc().Format) : DXGI_FORMAT_UNKNOWN),
        srv_ptr);
    RecordDev("ID3D12DeviceX::CreateShaderResourceView", 18, S_OK);
}

void STDMETHODCALLTYPE DeviceX::CreateUnorderedAccessView(
    ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (DestDescriptor.ptr == 0u) {
        RecordDev("ID3D12DeviceX::CreateUnorderedAccessView", 19, S_OK);
        return;
    }
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
    } else if (pDesc != nullptr) {
        DXGI_FORMAT san_fmt = SanitizeXboxDxgiFormat(pDesc->Format);
        if (san_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            san_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        } else if (san_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            san_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else if (san_fmt == DXGI_FORMAT_A8_UNORM && host_res != nullptr) {
            const DXGI_FORMAT rdf = host_res->GetDesc().Format;
            if (rdf == DXGI_FORMAT_R8_UNORM || rdf == DXGI_FORMAT_R8_TYPELESS) {
                san_fmt = DXGI_FORMAT_R8_UNORM;
            }
        }
        // Unify 1D UAV views onto TEXTURE2D (Width x 1) when backing resource is TEXTURE2D.
        const bool promote_1d_to_2d =
            retail_scarlett_abi_ && host_res != nullptr &&
            host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            (pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1D ||
             pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY);
        if (san_fmt != pDesc->Format || promote_1d_to_2d) {
            rw_desc = *pDesc;
            rw_desc.Format = (san_fmt != DXGI_FORMAT_UNKNOWN)
                                 ? san_fmt
                                 : MapTypelessFormatForSrv(host_res->GetDesc().Format);
            if (promote_1d_to_2d) {
                if (pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY) {
                    const UINT mip = pDesc->Texture1DArray.MipSlice;
                    const UINT fa = pDesc->Texture1DArray.FirstArraySlice;
                    const UINT asz = pDesc->Texture1DArray.ArraySize;
                    rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    rw_desc.Texture2DArray.MipSlice = mip;
                    rw_desc.Texture2DArray.FirstArraySlice = fa;
                    rw_desc.Texture2DArray.ArraySize = asz;
                    rw_desc.Texture2DArray.PlaneSlice = 0u;
                } else {
                    const UINT mip = pDesc->Texture1D.MipSlice;
                    rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    rw_desc.Texture2D.MipSlice = mip;
                    rw_desc.Texture2D.PlaneSlice = 0u;
                }
            }
            desc_ptr = &rw_desc;
        }
    } else if (host_res != nullptr) {
        const D3D12_RESOURCE_DESC rd = host_res->GetDesc();
        if (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            rw_desc.Format = MapTypelessFormatForSrv(rd.Format);
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                rw_desc.Texture3D.WSize = rd.DepthOrArraySize;
            } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            } else if (rd.DepthOrArraySize > 1u) {
                rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                rw_desc.Texture2DArray.ArraySize = rd.DepthOrArraySize;
            } else {
                rw_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            desc_ptr = &rw_desc;
        }
    }
    const bool use_shadow =
        retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire);
    uint64_t guest_wb = 0ull;
    const D3D12_CPU_DESCRIPTOR_HANDLE orig_dest = DestDescriptor;
    if (use_shadow) {
        RedirectGuestDescDestIfNeeded(DestDescriptor, guest_wb, /*prefer_static_partition=*/true);
    }
    const bool invalid_null_uav =
        host_res == nullptr &&
        (desc_ptr == nullptr ||
         (desc_ptr->Format == DXGI_FORMAT_UNKNOWN &&
          (desc_ptr->ViewDimension != D3D12_UAV_DIMENSION_BUFFER ||
           desc_ptr->Buffer.StructureByteStride == 0u)));
    if (invalid_null_uav ||
        (retail_scarlett_abi_ && host_res != nullptr &&
         (host_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)) {
        // Xbox allows UAVs on depth-stencil/non-UAV textures (e.g. HTILE/VSM); substitute a valid
        // null raw-buffer UAV so Desktop D3D12 does not trigger DXGI_ERROR_INVALID_CALL.
        D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav{};
        null_uav.Format = DXGI_FORMAT_R32_TYPELESS;
        null_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        null_uav.Buffer.FirstElement = 0ull;
        null_uav.Buffer.NumElements = 1u;
        null_uav.Buffer.StructureByteStride = 0u;
        null_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        host_dev_->CreateUnorderedAccessView(nullptr, nullptr, &null_uav, DestDescriptor);
        CheckCpuDeviceRemoved("CreateUnorderedAccessView[NullFallback]", 0ull, 0ull);
        if (use_shadow) {
            SnapshotCreatedDescriptorToShadow(DestDescriptor, guest_wb);
        }
        RecordDev("ID3D12DeviceX::CreateUnorderedAccessView[NonUavFallback]", 19, S_OK);
        RecordDev("ID3D12DeviceX::CreateUnorderedAccessView", 19, S_OK);
        return;
    }
    host_dev_->CreateUnorderedAccessView(host_res, host_counter, desc_ptr, DestDescriptor);
    CheckCpuDeviceRemoved(
        "CreateUnorderedAccessView",
        reinterpret_cast<uint64_t>(host_res),
        desc_ptr ? static_cast<uint64_t>(desc_ptr->Format) : 0ull);
    if (host_res != nullptr && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        uint64_t uav_off = 0ull;
        uint64_t uav_sz = host_res->GetDesc().Width;
        if (desc_ptr != nullptr && desc_ptr->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
            const uint32_t stride =
                ((desc_ptr->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0u ||
                 desc_ptr->Format == DXGI_FORMAT_R32_TYPELESS)
                    ? 4u
                    : (desc_ptr->Buffer.StructureByteStride > 0u ? desc_ptr->Buffer.StructureByteStride : 4u);
            uav_off = desc_ptr->Buffer.FirstElement * stride;
            if (desc_ptr->Buffer.NumElements > 0u) {
                uav_sz = static_cast<uint64_t>(desc_ptr->Buffer.NumElements) * stride;
            } else {
                uav_sz = (uav_sz > uav_off) ? (uav_sz - uav_off) : 0ull;
            }
        }
        NotifyGpuWrittenBufferRange(host_res, uav_off, uav_sz);
        if (host_counter != nullptr) {
            const uint64_t ctr_off =
                (desc_ptr != nullptr && desc_ptr->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
                    ? desc_ptr->Buffer.CounterOffsetInBytes
                    : 0ull;
            NotifyGpuWrittenBufferRange(host_counter, ctr_off, 4ull);
        }
    }
    if (host_res != nullptr && HasRegisteredUnifiedTextures() &&
        host_res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::lock_guard<std::mutex> ulk(unified_mutex_);
        auto uit = unified_textures_.find(host_res);
        if (uit != unified_textures_.end()) {
            uit->second.allow_cpu_upload = false;
            uit->second.cpu_readable = false;
            if (uit->second.dirty) {
                uit->second.dirty = false;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
        }
    }
    if (use_shadow) {
        uint64_t uav_sh_ptr = 0ull;
        SnapshotCreatedDescriptorToShadow(DestDescriptor, guest_wb, /*prefer_static_partition=*/true, &uav_sh_ptr);
        if (retail_scarlett_abi_ && uav_sh_ptr != 0ull) {
            RegisterShadowSrvUavPair(
                uav_sh_ptr, host_res, /*created_is_uav=*/true,
                desc_ptr ? desc_ptr->Format : (host_res ? MapTypelessFormatForSrv(host_res->GetDesc().Format) : DXGI_FORMAT_UNKNOWN));
        }
    }
    TrackPagedDescriptorWrite(orig_dest);
    if (const auto hook = descriptor_create_hook_.load(std::memory_order_acquire)) {
        const D3D12_GPU_VIRTUAL_ADDRESS va =
            (host_res && host_res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                ? host_res->GetGPUVirtualAddress()
                : 0ull;
        hook(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, orig_dest, host_res, va,
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
    } else if (pDesc != nullptr) {
        const DXGI_FORMAT san_fmt = SanitizeXboxDxgiFormat(pDesc->Format);
        if (san_fmt != pDesc->Format) {
            rw_desc = *pDesc;
            rw_desc.Format = san_fmt;
            desc_ptr = &rw_desc;
        }
    } else if (host_res != nullptr) {
        const D3D12_RESOURCE_DESC rd = host_res->GetDesc();
        if (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            rw_desc.Format = (rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
                                 ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                                 : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            rw_desc.ViewDimension = (rd.SampleDesc.Count > 1u)
                                        ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                                        : D3D12_RTV_DIMENSION_TEXTURE2D;
            desc_ptr = &rw_desc;
        }
    }
    const bool valid_rtv =
        host_res != nullptr &&
        (host_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if (!retail_scarlett_abi_ || valid_rtv) {
        host_dev_->CreateRenderTargetView(host_res, desc_ptr, DestDescriptor);
    }
    if (valid_rtv && host_res && HasRegisteredUnifiedTextures()) {
        // Once a texture is bound as an RTV on the GPU, never overwrite GPU output with CPU uploads.
        std::lock_guard<std::mutex> ulk(unified_mutex_);
        auto uit = unified_textures_.find(host_res);
        if (uit != unified_textures_.end()) {
            uit->second.allow_cpu_upload = false;
            uit->second.cpu_readable = false;
            if (uit->second.dirty) {
                uit->second.dirty = false;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
        }
    }
    if (retail_scarlett_abi_ && host_res && !valid_rtv && CommandListX::IsDiagTraceEnabled()) {
        const auto rd = host_res->GetDesc();
        std::fprintf(stderr, "[OpenScarlett][RTV-REJECT] res=%p dim=%d %llux%u fmt=%d flags=0x%X\n",
                     static_cast<void*>(host_res), static_cast<int>(rd.Dimension),
                     static_cast<unsigned long long>(rd.Width), rd.Height,
                     static_cast<int>(rd.Format), static_cast<unsigned>(rd.Flags));
        std::fflush(stderr);
    }
    if (retail_scarlett_abi_) {
        const DXGI_FORMAT eff_rtv_fmt =
            (desc_ptr && desc_ptr->Format != DXGI_FORMAT_UNKNOWN)
                ? desc_ptr->Format
                : (host_res ? host_res->GetDesc().Format : DXGI_FORMAT_UNKNOWN);
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        if (valid_rtv) {
            valid_rtv_handles_[DestDescriptor.ptr] = host_res;
            valid_rtv_formats_[DestDescriptor.ptr] = eff_rtv_fmt;
        } else {
            valid_rtv_handles_.erase(DestDescriptor.ptr);
            valid_rtv_formats_.erase(DestDescriptor.ptr);
        }
    }
    if (CommandListX::IsDiagTraceEnabled() && host_res) {
        const auto rd = host_res->GetDesc();
        char db[128];
        std::snprintf(db, sizeof(db), "RTV(res=%p %llux%u fmt=%d)",
                      static_cast<void*>(host_res),
                      static_cast<unsigned long long>(rd.Width), rd.Height,
                      static_cast<int>(rd.Format));
        RecordDiagDescriptor(DestDescriptor.ptr, db);
    }
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
    D3D12_DEPTH_STENCIL_VIEW_DESC clean_dsv{};
    const D3D12_DEPTH_STENCIL_VIEW_DESC* dsv_ptr = pDesc;
    bool is_readonly_dsv = false;
    if (pDesc != nullptr) {
        clean_dsv = *pDesc;
        // Mask out any Xbox console-private upper DSV flag bits while preserving READ_ONLY_DEPTH | READ_ONLY_STENCIL (AnyBox #54).
        constexpr uint32_t kDesktopDsvFlagsMask =
            static_cast<uint32_t>(D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL);
        clean_dsv.Flags = static_cast<D3D12_DSV_FLAGS>(
            static_cast<uint32_t>(clean_dsv.Flags) & kDesktopDsvFlagsMask);

        DXGI_FORMAT eff_fmt = clean_dsv.Format;
        if (eff_fmt == DXGI_FORMAT_UNKNOWN && host_res != nullptr) {
            eff_fmt = host_res->GetDesc().Format;
        }
        const DXGI_FORMAT mapped_fmt = MapTypelessFormatForDsv(eff_fmt);
        if (clean_dsv.Format != DXGI_FORMAT_UNKNOWN) {
            clean_dsv.Format = mapped_fmt;
        }
        if (IsDepthOnlyDsvFormat(mapped_fmt)) {
            // Desktop D3D12 rejects D3D12_DSV_FLAG_READ_ONLY_STENCIL on depth-only formats (D32_FLOAT / D16_UNORM).
            clean_dsv.Flags = static_cast<D3D12_DSV_FLAGS>(
                static_cast<uint32_t>(clean_dsv.Flags) &
                ~static_cast<uint32_t>(D3D12_DSV_FLAG_READ_ONLY_STENCIL));
        }
        is_readonly_dsv =
            (clean_dsv.Flags & (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL)) != 0;
        dsv_ptr = &clean_dsv;
    }
    const bool valid_dsv =
        host_res != nullptr &&
        (host_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    if (!retail_scarlett_abi_ || valid_dsv) {
        host_dev_->CreateDepthStencilView(host_res, dsv_ptr, DestDescriptor);
    }
    if (retail_scarlett_abi_) {
        const DXGI_FORMAT eff_dsv_fmt =
            (dsv_ptr && dsv_ptr->Format != DXGI_FORMAT_UNKNOWN)
                ? dsv_ptr->Format
                : (host_res ? MapTypelessFormatForDsv(host_res->GetDesc().Format) : DXGI_FORMAT_UNKNOWN);
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        if (valid_dsv) {
            valid_dsv_handles_[DestDescriptor.ptr] = host_res;
            valid_dsv_formats_[DestDescriptor.ptr] = eff_dsv_fmt;
            if (is_readonly_dsv) {
                readonly_dsv_handles_.insert(DestDescriptor.ptr);
            } else {
                readonly_dsv_handles_.erase(DestDescriptor.ptr);
            }
        } else {
            valid_dsv_handles_.erase(DestDescriptor.ptr);
            valid_dsv_formats_.erase(DestDescriptor.ptr);
            readonly_dsv_handles_.erase(DestDescriptor.ptr);
        }
    }
    if (is_readonly_dsv) {
        readonly_dsv_creations_.fetch_add(1ull, std::memory_order_relaxed);
        RecordDev("ID3D12DeviceX::CreateDepthStencilView[ReadOnly]", 21, S_OK);
    }
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
    if (!pDesc || DestDescriptor.ptr == 0u) {
        RecordDev("ID3D12DeviceX::CreateSampler", 22, S_OK);
        return;
    }
    if (!TryHandleVirtualSamplerCreate(pDesc, DestDescriptor)) {
        ID3D12DescriptorHeap* shadow_heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE shadow_slot{0u};
        const D3D12_CPU_DESCRIPTOR_HANDLE eff_dest =
            has_guest_backed_desc_heaps_.load(std::memory_order_acquire)
                ? TranslateGuestSamplerCpuHandle(DestDescriptor, &shadow_heap, &shadow_slot)
                : DestDescriptor;
        host_dev_->CreateSampler(pDesc, eff_dest);
        if (shadow_heap && shadow_slot.ptr != 0u) {
            host_dev_->CreateSampler(pDesc, shadow_slot);
        }
    }
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
    if (NumDestDescriptorRanges == 0u || NumSrcDescriptorRanges == 0u ||
        !pDestDescriptorRangeStarts || !pSrcDescriptorRangeStarts) {
        RecordDev("ID3D12DeviceX::CopyDescriptors", 23, S_OK);
        return;
    }
    if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        TryHandleVirtualSamplerCopy(
            NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
            NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes)) {
        RecordDev("ID3D12DeviceX::CopyDescriptors[UnboundedSampler]", 23, S_OK);
        RecordDev("ID3D12DeviceX::CopyDescriptors", 23, S_OK);
        return;
    }
    if ((DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
         DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) &&
        (retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire))) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        UINT dst_r = 0u, dst_i = 0u;
        UINT src_r = 0u, src_i = 0u;
        while (dst_r < NumDestDescriptorRanges && src_r < NumSrcDescriptorRanges) {
            const UINT dst_cnt = pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[dst_r] : 1u;
            const UINT src_cnt = pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[src_r] : 1u;
            if (dst_i >= dst_cnt) { ++dst_r; dst_i = 0u; continue; }
            if (src_i >= src_cnt) { ++src_r; src_i = 0u; continue; }
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_h{
                pDestDescriptorRangeStarts[dst_r].ptr + static_cast<SIZE_T>(dst_i) * inc};
            const D3D12_CPU_DESCRIPTOR_HANDLE src_h{
                pSrcDescriptorRangeStarts[src_r].ptr + static_cast<SIZE_T>(src_i) * inc};
            if (dst_h.ptr != 0u && src_h.ptr != 0u) {
                if (!TryCopyGuestBackedDescriptorsSimple(1u, dst_h, src_h, DescriptorHeapsType)) {
                    host_dev_->CopyDescriptorsSimple(1u, dst_h, src_h, DescriptorHeapsType);
                }
            }
            ++dst_i;
            ++src_i;
        }
    } else {
        host_dev_->CopyDescriptors(
            NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
            NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
            DescriptorHeapsType);
    }
    CheckCpuDeviceRemoved(
        "CopyDescriptors",
        pDestDescriptorRangeStarts ? pDestDescriptorRangeStarts[0].ptr : 0ull,
        pSrcDescriptorRangeStarts ? pSrcDescriptorRangeStarts[0].ptr : 0ull);
    if (CommandListX::IsDiagTraceEnabled() && DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        pDestDescriptorRangeStarts && pSrcDescriptorRangeStarts) {
        static std::atomic<uint32_t> s_cd_log{0u};
        if (s_cd_log.fetch_add(1u, std::memory_order_relaxed) < 8u) {
            std::fprintf(stderr, "[OpenScarlett][COPY-DESC] nd=%u ns=%u dst0=0x%llX src0=0x%llX\n",
                         NumDestDescriptorRanges, NumSrcDescriptorRanges,
                         static_cast<unsigned long long>(pDestDescriptorRangeStarts[0].ptr),
                         static_cast<unsigned long long>(pSrcDescriptorRangeStarts[0].ptr));
            std::fflush(stderr);
        }
    }
    if (retail_scarlett_abi_ &&
        (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
         DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
        pDestDescriptorRangeStarts && pSrcDescriptorRangeStarts &&
        NumDestDescriptorRanges > 0u && NumSrcDescriptorRanges > 0u) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto& view_map = (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                             ? valid_rtv_handles_
                             : valid_dsv_handles_;
        auto& fmt_map = (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                            ? valid_rtv_formats_
                            : valid_dsv_formats_;
        UINT dst_r = 0u, dst_i = 0u;
        UINT src_r = 0u, src_i = 0u;
        while (dst_r < NumDestDescriptorRanges && src_r < NumSrcDescriptorRanges) {
            const UINT dst_cnt = pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[dst_r] : 1u;
            const UINT src_cnt = pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[src_r] : 1u;
            if (dst_i >= dst_cnt) { ++dst_r; dst_i = 0u; continue; }
            if (src_i >= src_cnt) { ++src_r; src_i = 0u; continue; }
            const SIZE_T dst_ptr = pDestDescriptorRangeStarts[dst_r].ptr + static_cast<SIZE_T>(dst_i) * inc;
            const SIZE_T src_ptr = pSrcDescriptorRangeStarts[src_r].ptr + static_cast<SIZE_T>(src_i) * inc;
            auto sit = view_map.find(src_ptr);
            if (sit != view_map.end()) {
                view_map[dst_ptr] = sit->second;
                auto fit = fmt_map.find(src_ptr);
                if (fit != fmt_map.end()) fmt_map[dst_ptr] = fit->second;
                else fmt_map.erase(dst_ptr);
            } else {
                view_map.erase(dst_ptr);
                fmt_map.erase(dst_ptr);
            }
            if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
                if (readonly_dsv_handles_.find(src_ptr) != readonly_dsv_handles_.end()) {
                    readonly_dsv_handles_.insert(dst_ptr);
                } else {
                    readonly_dsv_handles_.erase(dst_ptr);
                }
            }
            if (CommandListX::IsDiagTraceEnabled()) {
                CopyDiagDescriptor(dst_ptr, src_ptr);
            }
            ++dst_i;
            ++src_i;
        }
    }
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
            if (CommandListX::IsDiagTraceEnabled()) {
                CopyDiagDescriptor(dst_h.ptr, src_h.ptr);
            }
            TrackPagedDescriptorWrite(dst_h);
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
    if (NumDescriptors == 0u || DestDescriptorRangeStart.ptr == 0u || SrcDescriptorRangeStart.ptr == 0u) {
        RecordDev("ID3D12DeviceX::CopyDescriptorsSimple", 24, S_OK);
        return;
    }
    if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && NumDescriptors > 0u) {
        if (TryHandleVirtualSamplerCopy(
                1u, &DestDescriptorRangeStart, &NumDescriptors,
                1u, &SrcDescriptorRangeStart, &NumDescriptors)) {
            RecordDev("ID3D12DeviceX::CopyDescriptorsSimple[UnboundedSampler]", 24, S_OK);
            RecordDev("ID3D12DeviceX::CopyDescriptorsSimple", 24, S_OK);
            return;
        }
    }
    const bool handled_guest_copy =
        (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
         DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) &&
        (retail_scarlett_abi_ || has_guest_backed_desc_heaps_.load(std::memory_order_acquire)) &&
        TryCopyGuestBackedDescriptorsSimple(
            NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    if (!handled_guest_copy) {
        host_dev_->CopyDescriptorsSimple(
            NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
    }
    CheckCpuDeviceRemoved(
        "CopyDescriptorsSimple", DestDescriptorRangeStart.ptr, SrcDescriptorRangeStart.ptr);
    if (CommandListX::IsDiagTraceEnabled() && DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        static std::atomic<uint32_t> s_cds_log{0u};
        if (s_cds_log.fetch_add(1u, std::memory_order_relaxed) < 8u) {
            std::fprintf(stderr, "[OpenScarlett][COPY-SIMPLE] n=%u dst=0x%llX src=0x%llX\n",
                         NumDescriptors,
                         static_cast<unsigned long long>(DestDescriptorRangeStart.ptr),
                         static_cast<unsigned long long>(SrcDescriptorRangeStart.ptr));
            std::fflush(stderr);
        }
    }
    if (retail_scarlett_abi_ &&
        (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
         DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
        NumDescriptors > 0u) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto& view_map = (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                             ? valid_rtv_handles_
                             : valid_dsv_handles_;
        auto& fmt_map = (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                            ? valid_rtv_formats_
                            : valid_dsv_formats_;
        for (UINT i = 0u; i < NumDescriptors; ++i) {
            const SIZE_T dst_ptr = DestDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc;
            const SIZE_T src_ptr = SrcDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc;
            auto sit = view_map.find(src_ptr);
            if (sit != view_map.end()) {
                view_map[dst_ptr] = sit->second;
                auto fit = fmt_map.find(src_ptr);
                if (fit != fmt_map.end()) fmt_map[dst_ptr] = fit->second;
                else fmt_map.erase(dst_ptr);
            } else {
                view_map.erase(dst_ptr);
                fmt_map.erase(dst_ptr);
            }
            if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
                if (readonly_dsv_handles_.find(src_ptr) != readonly_dsv_handles_.end()) {
                    readonly_dsv_handles_.insert(dst_ptr);
                } else {
                    readonly_dsv_handles_.erase(dst_ptr);
                }
            }
            if (CommandListX::IsDiagTraceEnabled()) {
                CopyDiagDescriptor(dst_ptr, src_ptr);
            }
        }
    }
    if (DescriptorHeapsType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && NumDescriptors > 0u) {
        const UINT inc = host_dev_->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
        for (UINT i = 0u; i < NumDescriptors; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE dst_h{
                DestDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc};
            D3D12_CPU_DESCRIPTOR_HANDLE src_h{
                SrcDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * inc};
            sampler_feedback_mgr_.CopyPreservedView(dst_h, src_h);
            if (CommandListX::IsDiagTraceEnabled()) {
                CopyDiagDescriptor(dst_h.ptr, src_h.ptr);
            }
        }
        TrackPagedDescriptorWrite(DestDescriptorRangeStart);
    }
    RecordDev("ID3D12DeviceX::CopyDescriptorsSimple", 24, S_OK);
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE DeviceX::GetResourceAllocationInfo(
    D3D12_RESOURCE_ALLOCATION_INFO* pRet,
    UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) {
    std::vector<D3D12_RESOURCE_DESC> rw_descs;
    const D3D12_RESOURCE_DESC* descs_ptr = pResourceDescs;
    if (pResourceDescs && numResourceDescs > 0u) {
        rw_descs.resize(numResourceDescs);
        const bool r9_fb = IsR9G9B9E5UavFallbackActive();
        for (UINT i = 0; i < numResourceDescs; ++i) {
            D3D12_RESOURCE_DESC clean = SanitizeAndClampResourceDesc(pResourceDescs[i]);
            if (r9_fb && ShouldRewriteR9G9B9E5Uav(clean)) {
                clean.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            rw_descs[i] = clean;
        }
        descs_ptr = rw_descs.data();
    }
    D3D12_RESOURCE_ALLOCATION_INFO* info =
        host_dev_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, descs_ptr);
    if (info && info->SizeInBytes == UINT64_MAX && !rw_descs.empty()) {
        uint64_t total = 0ull;
        for (const auto& d : rw_descs) {
            const uint64_t sz = std::max<uint64_t>(EstimateResourceVramBytes(d), 65536ull);
            total += (sz + 65535ull) & ~65535ull;
        }
        info->SizeInBytes = total;
        info->Alignment = 65536ull;
    }
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
    if (pResourceDescs && numResourceDescs > 0u) {
        rw_descs.resize(numResourceDescs);
        const bool r9_fb = IsR9G9B9E5UavFallbackActive();
        for (UINT i = 0; i < numResourceDescs; ++i) {
            D3D12_RESOURCE_DESC clean = SanitizeAndClampResourceDesc(pResourceDescs[i]);
            if (r9_fb && ShouldRewriteR9G9B9E5Uav(clean)) {
                clean.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            rw_descs[i] = clean;
        }
        descs_ptr = rw_descs.data();
    }
    D3D12_RESOURCE_ALLOCATION_INFO info =
        host_dev_->GetResourceAllocationInfo(visibleMask, numResourceDescs, descs_ptr);
    if (info.SizeInBytes == UINT64_MAX && !rw_descs.empty()) {
        uint64_t total = 0ull;
        for (const auto& d : rw_descs) {
            const uint64_t sz = std::max<uint64_t>(EstimateResourceVramBytes(d), 65536ull);
            total += (sz + 65535ull) & ~65535ull;
        }
        info.SizeInBytes = total;
        info.Alignment = 65536ull;
    }
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
        clean_desc = SanitizeAndClampResourceDesc(*pDesc);
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull) {
            // Retail Xbox UMA titles memset(Map(), 0, GetResourceAllocationInfo().SizeInBytes),
            // which is always 64 KiB aligned. Desktop drivers may only map ceil(Width, 4 KiB).
            clean_desc.Width = (clean_desc.Width + 65535ull) & ~65535ull;
        }
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
    D3D12_HEAP_PROPERTIES clean_heap_props{};
    const D3D12_HEAP_PROPERTIES* clean_heap_ptr = pHeapProperties;
    if (pHeapProperties) {
        clean_heap_props = *pHeapProperties;
        if (clean_heap_props.Type == D3D12_HEAP_TYPE_DEFAULT ||
            clean_heap_props.Type == D3D12_HEAP_TYPE_UPLOAD ||
            clean_heap_props.Type == D3D12_HEAP_TYPE_READBACK) {
            clean_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            clean_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        }
        clean_heap_props.CreationNodeMask = 1u;
        clean_heap_props.VisibleNodeMask = 1u;
        clean_heap_ptr = &clean_heap_props;
        if (clean_heap_props.Type == D3D12_HEAP_TYPE_UPLOAD) {
            InitialResourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else if (clean_heap_props.Type == D3D12_HEAP_TYPE_READBACK) {
            InitialResourceState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }
    if (retail_scarlett_abi_) {
        if (adapter_info_.raytracing_tier == 0u &&
            (static_cast<UINT>(InitialResourceState) &
             static_cast<UINT>(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) != 0u) {
            UINT st = static_cast<UINT>(InitialResourceState) &
                      ~static_cast<UINT>(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            if (st == 0u) {
                st = static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            InitialResourceState = static_cast<D3D12_RESOURCE_STATES>(st);
        }
        if (desc_ptr && desc_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            clear_ptr = nullptr;
        }
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreateCommittedResource(
            clean_heap_ptr, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    *ppvResource = nullptr;

    // Sprint 14: Live VramResidencyManager budget enforcement + LRU eviction + L0 system-RAM spill fallback.
    const uint64_t est_bytes = desc_ptr ? EstimateResourceVramBytes(*desc_ptr) : 0ull;
    const bool allow_l0_spill =
        (clean_heap_ptr != nullptr) &&
        (clean_heap_ptr->Type == D3D12_HEAP_TYPE_DEFAULT) &&
        (desc_ptr != nullptr) &&
        ((desc_ptr->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0);
    bool should_spill_l0 = false;
    if (est_bytes > 0ull && clean_heap_ptr && clean_heap_ptr->Type == D3D12_HEAP_TYPE_DEFAULT) {
        PrepareResidencyBudgetForAllocation(est_bytes, allow_l0_spill, should_spill_l0);
    }

    D3D12_HEAP_PROPERTIES eff_heap_props{};
    const D3D12_HEAP_PROPERTIES* eff_heap_ptr = clean_heap_ptr;
    bool did_l0_spill = false;
    bool try_wb_upload_promote = false;
    if (should_spill_l0 && clean_heap_ptr) {
        eff_heap_props = *clean_heap_ptr;
        eff_heap_props.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        eff_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_heap_ptr = &eff_heap_props;
    } else if (retail_scarlett_abi_ && clean_heap_ptr &&
               clean_heap_ptr->Type == D3D12_HEAP_TYPE_UPLOAD &&
               desc_ptr && desc_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Retail Xbox titles bind unaligned root CBVs out of UPLOAD buffers; promoting retail
        // UPLOAD buffers to L0 WRITE_BACK cached RAM avoids 500x slower Write-Combined CPU reads
        // during desktop 256-byte CBV alignment while keeping identical GPU L0 PCIe visibility.
        eff_heap_props = *clean_heap_ptr;
        eff_heap_props.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        eff_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_heap_ptr = &eff_heap_props;
        try_wb_upload_promote = true;
    }

    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev_->CreateCommittedResource(
        eff_heap_ptr, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (should_spill_l0 && SUCCEEDED(hr) && host_res) {
        did_l0_spill = true;
    } else if ((should_spill_l0 || try_wb_upload_promote) && (FAILED(hr) || !host_res)) {
        // Adapter rejected custom L0 pool for this resource; fall back cleanly to original heap props.
        hr = host_dev_->CreateCommittedResource(
            clean_heap_ptr, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }
    if (FAILED(hr) && retail_scarlett_abi_ && InitialResourceState != D3D12_RESOURCE_STATE_COMMON &&
        clean_heap_ptr && clean_heap_ptr->Type == D3D12_HEAP_TYPE_DEFAULT) {
        // Fallback: retry with COMMON initial state if desktop driver rejects an Xbox-only state mask.
        hr = host_dev_->CreateCommittedResource(
            clean_heap_ptr, clean_heap_flags, desc_ptr, D3D12_RESOURCE_STATE_COMMON,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource", 27, hr);
        return hr;
    }
    const uint64_t sparse_va =
        (sparse_window_iso_ && est_bytes > 0ull)
            ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
            : 0ull;
    TrackLiveResidencyObject(
        static_cast<ID3D12Pageable*>(host_res),
        est_bytes,
        pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT,
        /*is_placed_suballocation=*/false,
        did_l0_spill,
        sparse_va);
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (desc_ptr) {
        TrackBufferVaResource(host_res, *desc_ptr, 0ull);
    }
    XG_TILE_MODE placed_tile_mode = XG_TILE_MODE_LINEAR;
    if (pDesc) {
        placed_tile_mode =
            retail_scarlett_abi_ ? ResolveRetailXboxTileMode(*pDesc) : ExtractXboxGfx10TileMode(pDesc->Layout);
        if (placed_tile_mode != XG_TILE_MODE_LINEAR) {
            SetTrackedResourceTileMode(host_res, placed_tile_mode);
        }
    }
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 27, S_OK);
    }
    if (retail_scarlett_abi_ && host_res) {
        SetRetailResourceState(host_res, InitialResourceState);
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
    const uint64_t req_heap_bytes = heap_ptr ? heap_ptr->SizeInBytes : 0ull;
    const bool allow_l0_spill =
        (heap_ptr != nullptr) && (heap_ptr->Properties.Type == D3D12_HEAP_TYPE_DEFAULT);
    bool should_spill_l0 = false;
    if (req_heap_bytes > 0ull && allow_l0_spill) {
        PrepareResidencyBudgetForAllocation(req_heap_bytes, true, should_spill_l0);
    }

    D3D12_HEAP_DESC eff_heap{};
    const D3D12_HEAP_DESC* eff_heap_ptr = heap_ptr;
    bool did_l0_spill = false;
    bool try_wb_upload_promote = false;
    if (should_spill_l0 && heap_ptr) {
        eff_heap = *heap_ptr;
        eff_heap.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_heap.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        eff_heap.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_heap_ptr = &eff_heap;
    } else if (retail_scarlett_abi_ && heap_ptr &&
               heap_ptr->Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
        // Promote retail UPLOAD heaps to L0 WRITE_BACK cached RAM so placed CBV buffers
        // support full-speed L1/L2/L3 cached CPU reads during 256-byte root CBV alignment.
        eff_heap = *heap_ptr;
        eff_heap.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_heap.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        eff_heap.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_heap_ptr = &eff_heap;
        try_wb_upload_promote = true;
    }

    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev_->CreateHeap(
        eff_heap_ptr, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (should_spill_l0 && SUCCEEDED(hr) && host_heap) {
        did_l0_spill = true;
    } else if ((should_spill_l0 || try_wb_upload_promote) && (FAILED(hr) || !host_heap)) {
        hr = host_dev_->CreateHeap(
            heap_ptr, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    }
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap", 28, hr);
        return hr;
    }
    const uint64_t sparse_va =
        (sparse_window_iso_ && req_heap_bytes > 0ull)
            ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, req_heap_bytes)
            : 0ull;
    TrackLiveResidencyObject(
        static_cast<ID3D12Pageable*>(host_heap),
        req_heap_bytes,
        heap_ptr ? heap_ptr->Properties.Type : D3D12_HEAP_TYPE_DEFAULT,
        /*is_placed_suballocation=*/false,
        did_l0_spill,
        sparse_va,
        /*is_heap=*/true,
        /*parent_heap=*/nullptr);
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, sparse_va);
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
    D3D12_HEAP_DESC hd{};
    bool has_heap_desc = false;
    uint64_t heap_remaining_bytes = UINT64_MAX;
    if (host_heap) {
        hd = host_heap->GetDesc();
        has_heap_desc = true;
        heap_remaining_bytes = (hd.SizeInBytes > HeapOffset) ? (hd.SizeInBytes - HeapOffset) : 0ull;
        if (hd.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
            InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else if (hd.Properties.Type == D3D12_HEAP_TYPE_READBACK) {
            InitialState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }
    D3D12_RESOURCE_DESC clean_desc{};
    const D3D12_RESOURCE_DESC* desc_ptr = pDesc;
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    bool did_r9g9b9e5_fallback = false;
    bool should_promote_oversized = false;
    if (pDesc) {
        clean_desc = SanitizeAndClampResourceDesc(*pDesc);
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull) {
            // Retail Xbox UMA titles memset(Map(), 0, GetResourceAllocationInfo().SizeInBytes),
            // which is always 64 KiB aligned. Desktop drivers may only map ceil(Width, 4 KiB).
            const uint64_t rounded = (clean_desc.Width + 65535ull) & ~65535ull;
            if (rounded <= heap_remaining_bytes) {
                clean_desc.Width = rounded;
            }
        }
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
    if (adapter_info_.raytracing_tier == 0u &&
        (InitialState & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        InitialState = static_cast<D3D12_RESOURCE_STATES>(
            (InitialState & ~D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clear_ptr = nullptr;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev_->CreatePlacedResource(
            host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePlacedResource", 29, hr);
        return hr;
    }
    *ppvResource = nullptr;

    uint64_t address_heap_base_va = 0ull;
    if (host_heap) {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        auto it = address_heaps_.find(host_heap);
        if (it != address_heaps_.end()) {
            address_heap_base_va = it->second;
        }
    }

    // xcl #86: Oversized Placed-Texture Transparent Committed Memory Promotion (real Minecraft logo bug!).
    // Xbox console textures often have tighter memory footprints or alignments than PC desktop GPUs.
    // Check host GetResourceAllocationInfo before CreatePlacedResource; if the PC host texture requires
    // more bytes or stricter alignment than the remaining heap slice, promote transparently to a dedicated
    // CreateCommittedResource on matching heap properties instead of failing with E_INVALIDARG/E_OUTOFMEMORY!
    if (desc_ptr && desc_ptr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        has_heap_desc && address_heap_base_va == 0ull) {
        const D3D12_RESOURCE_ALLOCATION_INFO req_alloc =
            host_dev_->GetResourceAllocationInfo(0u, 1u, desc_ptr);
        const uint64_t eff_align = (req_alloc.Alignment > 0ull) ? req_alloc.Alignment : 65536ull;
        const bool size_exceeds_heap =
            (req_alloc.SizeInBytes != UINT64_MAX && req_alloc.SizeInBytes > heap_remaining_bytes);
        const bool align_incompatible =
            ((HeapOffset % eff_align) != 0ull) ||
            (hd.Alignment > 0ull && eff_align > hd.Alignment && (hd.Alignment % eff_align) != 0ull);
        const bool uma_large_texture_promo =
            sanitized_ihv_caps_.promote_large_placed_textures_to_committed &&
            sanitized_ihv_caps_.large_placed_texture_threshold_bytes > 0ull &&
            req_alloc.SizeInBytes != UINT64_MAX &&
            req_alloc.SizeInBytes >= sanitized_ihv_caps_.large_placed_texture_threshold_bytes;
        should_promote_oversized = size_exceeds_heap || align_incompatible || uma_large_texture_promo;
    }

    ID3D12Resource* host_res = nullptr;
    HRESULT hr = E_INVALIDARG;
    if (!should_promote_oversized) {
        hr = host_dev_->CreatePlacedResource(
            host_heap, HeapOffset, desc_ptr, InitialState, clear_ptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }

    bool placed_used_shadow_fallback = false;
    bool promoted_oversized_placed = false;
    D3D12_HEAP_PROPERTIES promoted_hp{};
    if ((FAILED(hr) || !host_res) && desc_ptr &&
        (desc_ptr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || retail_scarlett_abi_) &&
        has_heap_desc && address_heap_base_va == 0ull &&
        (should_promote_oversized || hr == E_INVALIDARG || hr == E_OUTOFMEMORY)) {
        promoted_hp = hd.Properties;
        D3D12_RESOURCE_DESC promo_desc = *desc_ptr;
        promo_desc.Alignment = 0ull;
        hr = host_dev_->CreateCommittedResource(
            &promoted_hp, D3D12_HEAP_FLAG_NONE, &promo_desc, InitialState, clear_ptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if ((FAILED(hr) || !host_res) && promoted_hp.Type != D3D12_HEAP_TYPE_DEFAULT) {
            promoted_hp = D3D12_HEAP_PROPERTIES{};
            promoted_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = host_dev_->CreateCommittedResource(
                &promoted_hp, D3D12_HEAP_FLAG_NONE, &promo_desc, InitialState, clear_ptr,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
        if (SUCCEEDED(hr) && host_res) {
            promoted_oversized_placed = true;
            promoted_oversized_placed_resources_.fetch_add(1ull, std::memory_order_relaxed);
            RecordDev("ID3D12DeviceX::CreatePlacedResource[PromotedOversized]", 29, S_OK);
        }
    }

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
    if (desc_ptr) {
        const uint64_t est_bytes = EstimateResourceVramBytes(*desc_ptr);
        const uint64_t sparse_va =
            (sparse_window_iso_ && est_bytes > 0ull)
                ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
                : 0ull;
        const bool is_placed = !(placed_used_shadow_fallback || promoted_oversized_placed);
        TrackLiveResidencyObject(
            static_cast<ID3D12Pageable*>(host_res),
            est_bytes,
            promoted_oversized_placed ? promoted_hp.Type : D3D12_HEAP_TYPE_DEFAULT,
            /*is_placed_suballocation=*/is_placed,
            /*spilled_to_l0=*/false,
            sparse_va,
            /*is_heap=*/false,
            /*parent_heap=*/is_placed ? static_cast<ID3D12Pageable*>(host_heap) : nullptr);
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 29, S_OK);
    }
    XG_TILE_MODE placed_tile_mode = XG_TILE_MODE_LINEAR;
    if (pDesc) {
        placed_tile_mode =
            retail_scarlett_abi_ ? ResolveRetailXboxTileMode(*pDesc) : ExtractXboxGfx10TileMode(pDesc->Layout);
        if (placed_tile_mode != XG_TILE_MODE_LINEAR) {
            SetTrackedResourceTileMode(host_res, placed_tile_mode);
        }
    }
    uint64_t tracked_va = 0ull;
    if (host_heap && desc_ptr) {
        if (vulkan_routing_active_) {
            TrackVulkanResource(
                host_res,
                (placed_used_shadow_fallback || promoted_oversized_placed)
                    ? (promoted_oversized_placed ? promoted_hp.Type : D3D12_HEAP_TYPE_DEFAULT)
                    : hd.Properties.Type,
                *desc_ptr);
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
            const bool is_rt_or_ds =
                (desc_ptr->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
            const bool register_retail_tex =
                desc_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
                desc_ptr->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
                (placed_tile_mode == XG_TILE_MODE_LINEAR && !is_rt_or_ds);
            if (!retail_scarlett_abi_ || register_retail_tex) {
                AutoRegisterUnifiedResource(host_res, tracked_va, *desc_ptr, InitialState, mirrored);
            }
        }
        TrackBufferVaResource(host_res, *desc_ptr, tracked_va);
    }
    auto* proxy = new (std::nothrow) ResourceXProxy(
        this, host_res, desc_ptr,
        promoted_oversized_placed ? &promoted_hp : nullptr,
        D3D12_HEAP_FLAG_NONE, tracked_va);
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
    bool had_cross_adapter = false;
    if (pDesc) {
        clean_desc = SanitizeAndClampResourceDesc(*pDesc);
        if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN) {
            // Desktop D3D12 requires 64KB_UNDEFINED_SWIZZLE for reserved textures.
            clean_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        }
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull) {
            // Retail Xbox UMA titles memset(Map(), 0, GetResourceAllocationInfo().SizeInBytes),
            // which is always 64 KiB aligned. Desktop drivers may only map ceil(Width, 4 KiB).
            clean_desc.Width = (clean_desc.Width + 65535ull) & ~65535ull;
        }
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
        had_cross_adapter =
            (clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
        desc_ptr = &clean_desc;
    }
    if (adapter_info_.raytracing_tier == 0u &&
        (InitialState & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        InitialState = static_cast<D3D12_RESOURCE_STATES>(
            (InitialState & ~D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clear_ptr = nullptr;
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
    // Leonida #4496: Preserve D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER when supported by the host adapter,
    // with graceful strip-and-retry if a software/legacy adapter rejects it.
    if (had_cross_adapter) {
        if (SUCCEEDED(hr) && host_res) {
            reserved_cross_adapter_preserved_.fetch_add(1ull, std::memory_order_relaxed);
            RecordDev("ID3D12DeviceX::CreateReservedResource[CrossAdapterPreserved]", 30, S_OK);
        } else if (desc_ptr) {
            D3D12_RESOURCE_DESC retry_desc = *desc_ptr;
            retry_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                static_cast<uint32_t>(retry_desc.Flags) &
                ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER));
            hr = host_dev_->CreateReservedResource(
                &retry_desc, InitialState, clear_ptr,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            if (SUCCEEDED(hr) && host_res) {
                clean_desc = retry_desc;
                reserved_cross_adapter_stripped_fallback_.fetch_add(1ull, std::memory_order_relaxed);
                RecordDev("ID3D12DeviceX::CreateReservedResource[CrossAdapterFallback]", 30, S_OK);
            }
        }
    }
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
    if (NumObjects == 0u) {
        RecordDev("ID3D12DeviceX::MakeResident", 34, S_OK);
        return S_OK;
    }
    if (!ppObjects) {
        RecordDev("ID3D12DeviceX::MakeResident", 34, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    for (ID3D12Pageable* obj : unwrapped) {
        if (!obj) {
            RecordDev("ID3D12DeviceX::MakeResident", 34, E_INVALIDARG);
            return E_INVALIDARG;
        }
    }

    // Proactively enforce budget before making evicted pageables resident.
    {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        uint64_t bytes_to_page_in = 0ull;
        for (ID3D12Pageable* obj : unwrapped) {
            auto it = residency_entries_.find(obj);
            if (it != residency_entries_.end() && !it->second.is_resident && !it->second.is_placed_suballocation) {
                bytes_to_page_in += it->second.size_bytes;
            }
        }
        const uint64_t budget = EffectiveResidencyBudgetBytesLocked();
        if (bytes_to_page_in > 0ull && budget > 0ull && tracked_resident_bytes_ + bytes_to_page_in > budget) {
            const uint64_t over = (tracked_resident_bytes_ + bytes_to_page_in) - budget;
            (void)EvictLruVictimsLocked(over, unwrapped[0]);
        }
    }

    const HRESULT hr = host_dev_->MakeResident(NumObjects, unwrapped.data());
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        ++make_resident_calls_;
        for (ID3D12Pageable* obj : unwrapped) {
            auto it = residency_entries_.find(obj);
            if (it != residency_entries_.end()) {
                auto& e = it->second;
                ++e.residency_ref_count;
                if (!e.is_resident) {
                    e.is_resident = true;
                    if (!e.is_placed_suballocation) {
                        if (tracked_evicted_bytes_ >= e.size_bytes) tracked_evicted_bytes_ -= e.size_bytes;
                        else tracked_evicted_bytes_ = 0ull;
                        tracked_resident_bytes_ += e.size_bytes;
                    }
                }
                if (e.in_lru) {
                    residency_lru_.erase(e.lru_it);
                }
                residency_lru_.push_front(obj);
                e.lru_it = residency_lru_.begin();
                e.in_lru = true;
            }
        }
    }
    RecordDev("ID3D12DeviceX::MakeResident", 34, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceX::Evict(
    UINT NumObjects, ID3D12Pageable* const* ppObjects) {
    if (NumObjects == 0u) {
        RecordDev("ID3D12DeviceX::Evict", 35, S_OK);
        return S_OK;
    }
    if (!ppObjects) {
        RecordDev("ID3D12DeviceX::Evict", 35, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    for (ID3D12Pageable* obj : unwrapped) {
        if (!obj) {
            RecordDev("ID3D12DeviceX::Evict", 35, E_INVALIDARG);
            return E_INVALIDARG;
        }
    }
    const HRESULT hr = host_dev_->Evict(NumObjects, unwrapped.data());
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        ++evict_calls_;
        for (ID3D12Pageable* obj : unwrapped) {
            auto it = residency_entries_.find(obj);
            if (it != residency_entries_.end()) {
                auto& e = it->second;
                if (e.residency_ref_count > 0u) {
                    --e.residency_ref_count;
                }
                if (e.residency_ref_count == 0u && e.is_resident) {
                    e.is_resident = false;
                    if (e.in_lru) {
                        residency_lru_.erase(e.lru_it);
                        e.in_lru = false;
                    }
                    if (!e.is_placed_suballocation) {
                        if (tracked_resident_bytes_ >= e.size_bytes) tracked_resident_bytes_ -= e.size_bytes;
                        else tracked_resident_bytes_ = 0ull;
                        tracked_evicted_bytes_ += e.size_bytes;
                    }
                }
            }
        }
    }
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
        const HRESULT prev = device_removed_reason_.exchange(host_hr, std::memory_order_acq_rel);
        if (SUCCEEDED(prev)) {
            if (retail_scarlett_abi_ && host_dev_) {
                std::fprintf(stderr,
                    "[OpenScarlett][d3d12_xs] DEVICE_REMOVED reason=0x%08lX\n",
                    static_cast<unsigned long>(host_hr));
                ID3D12InfoQueue* iq = nullptr;
                if (SUCCEEDED(host_dev_->QueryInterface(OS_D3D12_IID(ID3D12InfoQueue), reinterpret_cast<void**>(&iq))) && iq) {
                    const UINT64 n = iq->GetNumStoredMessages();
                    const UINT64 start = (n > 20) ? (n - 20) : 0;
                    for (UINT64 i = start; i < n; ++i) {
                        SIZE_T mlen = 0;
                        iq->GetMessage(i, nullptr, &mlen);
                        if (mlen > 0 && mlen < 4096) {
                            std::vector<uint8_t> mbuf(mlen);
                            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(mbuf.data());
                            if (SUCCEEDED(iq->GetMessage(i, msg, &mlen)) && msg->pDescription) {
                                std::fprintf(stderr, "  [D3D12Msg #%d sev=%d] %s\n",
                                    static_cast<int>(msg->ID), static_cast<int>(msg->Severity), msg->pDescription);
                            }
                        }
                    }
                    iq->Release();
                }
                ID3D12DeviceRemovedExtendedData* dred = nullptr;
                if (SUCCEEDED(host_dev_->QueryInterface(IID_PPV_ARGS(&dred))) && dred) {
                    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
                    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
                        std::fprintf(stderr, "  [DRED-PF] PageFaultVA=0x%llX\n",
                                     static_cast<unsigned long long>(pf.PageFaultVA));
                        const D3D12_DRED_ALLOCATION_NODE* n = pf.pHeadExistingAllocationNode;
                        for (int k = 0; n && k < 4; n = n->pNext, ++k) {
                            std::fprintf(stderr, "    existing[%d] type=%d name=%s\n",
                                         k, static_cast<int>(n->AllocationType),
                                         n->ObjectNameA ? n->ObjectNameA : "(unnamed)");
                        }
                        n = pf.pHeadRecentFreedAllocationNode;
                        for (int k = 0; n && k < 4; n = n->pNext, ++k) {
                            std::fprintf(stderr, "    freed[%d] type=%d name=%s\n",
                                         k, static_cast<int>(n->AllocationType),
                                         n->ObjectNameA ? n->ObjectNameA : "(unnamed)");
                        }
                    }
                    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
                    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
                        const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode;
                        int ni = 0;
                        while (node && ni < 8) {
                            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
                            if (node->BreadcrumbCount > 0u && done < node->BreadcrumbCount) {
                                std::fprintf(stderr, "  [DRED-BC #%d] completed=%u/%u ops:",
                                             ni, done, node->BreadcrumbCount);
                                const UINT s = (done > 3u) ? (done - 3u) : 0u;
                                const UINT e = std::min<UINT>(node->BreadcrumbCount, done + 4u);
                                for (UINT bi = s; bi < e; ++bi) {
                                    std::fprintf(stderr, " %s%d", (bi == done) ? ">>>" : "",
                                                 static_cast<int>(node->pCommandHistory[bi]));
                                }
                                std::fprintf(stderr, "\n");
                            }
                            node = node->pNext;
                            ++ni;
                        }
                    }
                    dred->Release();
                }
                std::fflush(stderr);
            }
            CommandQueueX::NotifyDeviceRemovedForDevice(this, host_hr);
        }
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
    if (!pDesc || pDesc->NumArgumentDescs == 0u || !pDesc->pArgumentDescs) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, E_INVALIDARG);
        return E_INVALIDARG;
    }
    // Validate indirect argument layout and minimum byte stride before forwarding to host D3D12.
    UINT min_stride = 0u;
    bool has_dispatch_mesh = false;
    for (UINT i = 0u; i < pDesc->NumArgumentDescs; ++i) {
        const auto& a = pDesc->pArgumentDescs[i];
        switch (a.Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: min_stride += 16u; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: min_stride += 20u; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: min_stride += 12u; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: min_stride += 16u; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: min_stride += 12u; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                if (a.Constant.Num32BitValuesToSet == 0u) {
                    RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, E_INVALIDARG);
                    return E_INVALIDARG;
                }
                min_stride += a.Constant.Num32BitValuesToSet * 4u;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
                min_stride += 8u;
                break;
            default:
                if (static_cast<UINT>(a.Type) == 10u) {
                    has_dispatch_mesh = true;
                    min_stride += 12u;
                } else if (static_cast<UINT>(a.Type) == 9u) {
                    min_stride += 104u;
                } else if (static_cast<UINT>(a.Type) >= 100u) {
                    // Xbox Series X|S extended indirect argument types (e.g., 0x69, 0x6B)
                    min_stride += 12u;
                } else {
                    RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, E_INVALIDARG);
                    return E_INVALIDARG;
                }
                break;
        }
    }
    if (pDesc->ByteStride < min_stride) {
        RecordDev("ID3D12DeviceX::CreateCommandSignature[InvalidStride]", 41, E_INVALIDARG);
        return E_INVALIDARG;
    }

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

    // Xbox Series X|S d3d12_xs.h defines Xbox-extended D3D12_INDIRECT_ARGUMENT_TYPE values
    // >= 100 (e.g., 0x69 = Xbox compute dispatch indirect, 0x6B = Xbox extended dispatch/draw).
    // Translate Xbox-extended argument types and non-DXR DispatchRays to desktop equivalents
    // when host rejects pDesc. (DispatchMesh on MeshTier<10 uses virtual_mesh_fallback below.)
    if ((FAILED(hr) || !host_sig) && pDesc && pDesc->pArgumentDescs && pDesc->NumArgumentDescs > 0u) {
        std::vector<D3D12_INDIRECT_ARGUMENT_DESC> mapped_args(
            pDesc->pArgumentDescs, pDesc->pArgumentDescs + pDesc->NumArgumentDescs);
        bool modified = false;
        for (auto& arg : mapped_args) {
            const uint32_t raw_type = static_cast<uint32_t>(arg.Type);
            if (raw_type >= 100u) {
                modified = true;
                if (raw_type == 0x69u || pDesc->ByteStride == 12u) {
                    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                } else if (pDesc->ByteStride == 20u) {
                    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
                } else if (pDesc->ByteStride == 16u) {
                    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
                } else {
                    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                }
            } else if (raw_type == 9u &&
                       (adapter_info_.raytracing_tier == 0u || sanitized_ihv_caps_.raytracing_tier == 0u)) {
                // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS on non-DXR GPU (e.g. GTX 1650 Ti)
                modified = true;
                arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            }
        }
        if (modified) {
            D3D12_COMMAND_SIGNATURE_DESC mapped_desc = *pDesc;
            mapped_desc.pArgumentDescs = mapped_args.data();
            if (mapped_desc.ByteStride < 12u) {
                mapped_desc.ByteStride = 12u;
            }
            hr = host_dev_->CreateCommandSignature(
                &mapped_desc,
                (mapped_desc.NumArgumentDescs == 1u &&
                 (mapped_args[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
                  mapped_args[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
                  mapped_args[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH))
                    ? nullptr
                    : host_rs,
                OS_D3D12_IID(ID3D12CommandSignature),
                reinterpret_cast<void**>(&host_sig));
        }
    }

    bool virtual_mesh_fallback = false;
    if ((FAILED(hr) || !host_sig) && has_dispatch_mesh) {
        // Fallback when host D3D12 rejects D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
        // (e.g. WARP / MeshTier=0 or inbox Win10 d3d12.dll without Agility SDK): create
        // validated virtual CommandSignatureXProxy so device startup proceeds cleanly.
        virtual_mesh_fallback = true;
        hr = S_OK;
        RecordDev("ID3D12DeviceX::CreateCommandSignature[DispatchMeshVirtualFallback]", 41, S_OK);
    }
    if (FAILED(hr) || (!host_sig && !virtual_mesh_fallback)) {
        if (retail_scarlett_abi_) {
            std::fprintf(stderr,
                "[OpenScarlett][d3d12_xs] CreateCommandSignature FAILED hr=0x%08lX stride=%u nargs=%u arg0=%u\n",
                static_cast<unsigned long>(hr),
                pDesc ? pDesc->ByteStride : 0u,
                pDesc ? pDesc->NumArgumentDescs : 0u,
                (pDesc && pDesc->pArgumentDescs && pDesc->NumArgumentDescs > 0u)
                    ? static_cast<unsigned>(pDesc->pArgumentDescs[0].Type) : 0u);
            std::fflush(stderr);
        }
        RecordDev("ID3D12DeviceX::CreateCommandSignature", 41, hr);
        return hr;
    }
    auto* proxy = new (std::nothrow) CommandSignatureXProxy(this, host_sig, pDesc, virtual_mesh_fallback);
    if (host_sig) host_sig->Release();
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
    if (NumObjects == 0u) {
        RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, S_OK);
        return S_OK;
    }
    if (!ppObjects || !pPriorities) {
        RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    for (ID3D12Pageable* obj : unwrapped) {
        if (!obj) {
            RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, E_INVALIDARG);
            return E_INVALIDARG;
        }
    }
    HRESULT hr = host_dev1_
                     ? host_dev1_->SetResidencyPriority(
                           NumObjects, unwrapped.data(), pPriorities)
                     : E_NOINTERFACE;
    // Some WDDM/WARP drivers return E_ACCESSDENIED for D3D12_RESIDENCY_PRIORITY_MAXIMUM when non-elevated;
    // retry with HIGH so Xbox titles don't abort on standard Windows user accounts (Leonida #2864).
    if (hr == E_ACCESSDENIED && host_dev1_) {
        std::vector<D3D12_RESIDENCY_PRIORITY> clamped(pPriorities, pPriorities + NumObjects);
        for (auto& pr : clamped) {
            if (pr == D3D12_RESIDENCY_PRIORITY_MAXIMUM) {
                pr = D3D12_RESIDENCY_PRIORITY_HIGH;
            }
        }
        hr = host_dev1_->SetResidencyPriority(NumObjects, unwrapped.data(), clamped.data());
    }
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        ++set_priority_calls_;
        for (UINT i = 0u; i < NumObjects; ++i) {
            auto it = residency_entries_.find(unwrapped[i]);
            if (it != residency_entries_.end()) {
                it->second.priority = pPriorities[i];
            }
        }
    }
    RecordDev("ID3D12DeviceX::SetResidencyPriority", 46, hr);
    return hr;
}

// ID3D12Device2 (slot 47)
HRESULT DeviceX::CreateDerivedGraphicsPipelineStateX(
    ID3D12PipelineState* pSourcePso,
    UINT numOverrides,
    const void* pOverrides,
    REFIID riid,
    void** ppPipelineState) {
    if (!ppPipelineState) return E_POINTER;
    *ppPipelineState = nullptr;
    if (!pSourcePso) return E_INVALIDARG;

    PipelineStateXProxy* src_proxy = nullptr;
    if (IsKnownProxyPointer(pSourcePso)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pSourcePso->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::PipelineState) {
                src_proxy = static_cast<PipelineStateXProxy*>(pSourcePso);
            }
            insp->Release();
        }
    }

    PendingXdatGfxDesc* src_pd = src_proxy ? src_proxy->PendingXdatDesc() : nullptr;
    if (!src_pd || src_pd->is_compute) {
        return pSourcePso->QueryInterface(riid, ppPipelineState);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC derived = src_pd->gpso;
    derived.pRootSignature = src_pd->host_rs;
    derived.VS = src_pd->vs_blob.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{src_pd->vs_blob.data(), src_pd->vs_blob.size()};
    derived.PS = src_pd->ps_blob.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{src_pd->ps_blob.data(), src_pd->ps_blob.size()};
    derived.HS = src_pd->hs_blob.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{src_pd->hs_blob.data(), src_pd->hs_blob.size()};
    derived.DS = src_pd->ds_blob.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{src_pd->ds_blob.data(), src_pd->ds_blob.size()};
    derived.GS = src_pd->gs_blob.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{src_pd->gs_blob.data(), src_pd->gs_blob.size()};

    // Apply retail Scarlett 0x14C-byte (332-byte) derived PSO override entries:
    //   type == 0: SampleDesc (+0x04/+0x08), NumRenderTargets (+0x14), DSVFormat (+0x15), RTVFormats[8] (+0x18)
    //   type == 2: D3D12_BLEND_DESC (+0x04..+0x14B, 328 bytes)
    //   type == 5: NumRenderTargets (+0x14), DSVFormat (+0x15)
    constexpr size_t kOverrideStride = 0x14Cu;
    if (pOverrides && numOverrides > 0u && numOverrides <= 16u) {
        const auto* base = static_cast<const uint8_t*>(pOverrides);
        for (UINT i = 0u; i < numOverrides; ++i) {
            const uint8_t* ent = base + static_cast<size_t>(i) * kOverrideStride;
            uint32_t otype = 0u;
            std::memcpy(&otype, ent, 4u);
            if (otype == 0u) {
                uint32_t sc = 1u, sq = 0u;
                std::memcpy(&sc, ent + 0x04u, 4u);
                std::memcpy(&sq, ent + 0x08u, 4u);
                if (sc > 0u && sc <= 16u) {
                    derived.SampleDesc.Count = sc;
                    derived.SampleDesc.Quality = sq;
                }
                const uint8_t num_rts = ent[0x14u];
                const uint8_t dsv_fmt = ent[0x15u];
                if (num_rts <= 8u) {
                    derived.NumRenderTargets = num_rts;
                    for (UINT r = 0u; r < 8u; ++r) {
                        uint32_t rfmt = 0u;
                        std::memcpy(&rfmt, ent + 0x18u + r * 4u, 4u);
                        derived.RTVFormats[r] = (r < num_rts)
                                                    ? SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(rfmt))
                                                    : DXGI_FORMAT_UNKNOWN;
                    }
                }
                derived.DSVFormat = SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(dsv_fmt));
            } else if (otype == 2u) {
                static_assert(sizeof(D3D12_BLEND_DESC) == 0x148u, "D3D12_BLEND_DESC size check");
                std::memcpy(&derived.BlendState, ent + 0x04u, sizeof(D3D12_BLEND_DESC));
            } else if (otype == 5u) {
                const uint8_t num_rts = ent[0x14u];
                const uint8_t dsv_fmt = ent[0x15u];
                if (num_rts <= 8u) {
                    derived.NumRenderTargets = num_rts;
                    for (UINT r = 0u; r < num_rts; ++r) {
                        uint32_t rfmt = 0u;
                        std::memcpy(&rfmt, ent + 0x18u + r * 4u, 4u);
                        const DXGI_FORMAT cand = SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(rfmt));
                        if (cand != DXGI_FORMAT_UNKNOWN) {
                            derived.RTVFormats[r] = cand;
                        } else if (derived.RTVFormats[r] == DXGI_FORMAT_UNKNOWN) {
                            derived.RTVFormats[r] = DXGI_FORMAT_R8G8B8A8_UNORM;
                        }
                    }
                }
                derived.DSVFormat = SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(dsv_fmt));
            }
        }
    }
    SanitizeGraphicsPsoDescForDesktop(derived);

    const auto& primary_src_blob = src_pd->is_mesh ? src_pd->ms_blob : src_pd->vs_blob;
    const bool src_is_xs2 =
        !primary_src_blob.empty() &&
        !IsXboxXdatDxbcBlob(primary_src_blob.data(), primary_src_blob.size()) &&
        openscarlett::IsXboxShaderContainerBlob(primary_src_blob.data(), primary_src_blob.size());
    if (src_is_xs2) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC standin_desc = derived;
        standin_desc.VS = {};
        standin_desc.PS = {};
        standin_desc.HS = {};
        standin_desc.DS = {};
        standin_desc.GS = {};
        ID3D12PipelineState* out_pso = nullptr;
        const HRESULT hr = CreateGraphicsPipelineState(
            &standin_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&out_pso));
        if (FAILED(hr) || !out_pso) return hr;
        if (IsKnownProxyPointer(out_pso)) {
            auto* dst_proxy = static_cast<PipelineStateXProxy*>(out_pso);
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->is_compute = false;
            pd->is_mesh = src_pd->is_mesh;
            if (src_pd->from_slot57) {
                pd->from_slot57 = true;
                pd->has_slot57_stencil_ref = src_pd->has_slot57_stencil_ref;
                pd->slot57_stencil_ref = src_pd->slot57_stencil_ref;
                pd->vs_cbv_slot_mask = src_pd->vs_cbv_slot_mask;
                pd->ps_cbv_slot_mask = src_pd->ps_cbv_slot_mask;
                pd->ms_cbv_slot_mask = src_pd->ms_cbv_slot_mask;
            }
            pd->gpso = derived;
            pd->host_rs = src_pd->host_rs;
            if (pd->host_rs) pd->host_rs->AddRef();
            pd->vs_blob = src_pd->vs_blob;
            pd->ps_blob = src_pd->ps_blob;
            pd->hs_blob = src_pd->hs_blob;
            pd->ds_blob = src_pd->ds_blob;
            pd->gs_blob = src_pd->gs_blob;
            pd->ms_blob = src_pd->ms_blob;
            pd->as_blob = src_pd->as_blob;
            pd->il_names = src_pd->il_names;
            pd->il_elems = src_pd->il_elems;
            for (size_t i = 0u; i < pd->il_elems.size() && i < pd->il_names.size(); ++i) {
                pd->il_elems[i].SemanticName = pd->il_names[i].c_str();
            }
            pd->gpso.InputLayout = pd->il_elems.empty()
                                       ? D3D12_INPUT_LAYOUT_DESC{nullptr, 0u}
                                       : D3D12_INPUT_LAYOUT_DESC{pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
            dst_proxy->SetStandinPso(true);
            dst_proxy->SetPendingXdatGfxDesc(std::move(pd));
        }
        const HRESULT qhr = out_pso->QueryInterface(riid, ppPipelineState);
        out_pso->Release();
        return qhr;
    }

    return CreateGraphicsPipelineState(&derived, riid, ppPipelineState);
}

namespace {

bool SafeMemcpyPoolIsa(void* dst, const void* src, size_t len) noexcept {
#if defined(_MSC_VER)
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, len);
    return true;
#endif
}

bool ExtractRetailFssmPoolIsa(
    uint64_t gpu_va,
    uint64_t max_bytes,
    openscarlett::ShaderStage stage,
    uint32_t tg_x,
    uint32_t tg_y,
    uint32_t tg_z,
    std::vector<uint8_t>& out_xs2_blob) {
    out_xs2_blob.clear();
    if (gpu_va < 0x10000ull || max_bytes < 40ull || max_bytes > 0x100000ull) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const auto* base_ptr = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(gpu_va));
    if (::VirtualQuery(base_ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    const uintptr_t reg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const uintptr_t req_end = static_cast<uintptr_t>(gpu_va) + static_cast<uintptr_t>(max_bytes);
    if (req_end > reg_end) {
        max_bytes = static_cast<uint64_t>(reg_end - static_cast<uintptr_t>(gpu_va));
    }
    const size_t word_cap = static_cast<size_t>(max_bytes / 4ull);
    if (word_cap < 10u) return false;

    std::vector<uint32_t> raw_words(word_cap);
    if (!SafeMemcpyPoolIsa(raw_words.data(), base_ptr, word_cap * 4u)) {
        return false;
    }

    size_t start_idx = 0u;
    // Retail Scarlett FSSM shader pool entries begin with a 36-byte (9-dword) v_fma_f64 hash header.
    if (word_cap >= 10u &&
        (raw_words[0] & 0xFFFF0000u) == 0xD5800000u &&
        raw_words[6] == 0xD5800000u &&
        (raw_words[7] & 0xFFFE0000u) == 0x03FE0000u) {
        start_idx = 9u;
    }

    std::vector<uint32_t> code_words;
    code_words.reserve(word_cap - start_idx);
    bool saw_endpgm = false;
    for (size_t i = start_idx; i < word_cap; ++i) {
        const uint32_t w = raw_words[i];
        if (w == 0xBF9F0000u) {  // s_code_end padding marks end of shader in pool
            break;
        }
        code_words.push_back(w);
        if (w == 0xBF810000u) {
            saw_endpgm = true;
        }
    }
    if (code_words.empty() || !saw_endpgm) {
        return false;
    }

    openscarlett::Xs2ShaderContainer xs2{};
    xs2.stage = stage;
    xs2.sgpr_count = 104u;
    xs2.vgpr_count = 128u;
    xs2.wave_size = 64u;
    xs2.threadgroup_x = std::clamp(tg_x, 1u, 1024u);
    xs2.threadgroup_y = std::clamp(tg_y, 1u, 1024u);
    xs2.threadgroup_z = std::clamp(tg_z, 1u, 1024u);
    xs2.code_words = std::move(code_words);
    out_xs2_blob = openscarlett::BuildXs2Container(xs2);
    return !out_xs2_blob.empty();
}

}  // namespace

HRESULT DeviceX::DeserializeComputePipelineStateX(
    const void* pDesc,
    REFIID riid,
    void** ppPipelineState) {
    if (!pDesc || !ppPipelineState) return E_POINTER;
    *ppPipelineState = nullptr;
    uint64_t q[9]{};
    if (!SafeMemcpyPoolIsa(q, pDesc, sizeof(q))) {
        if (!SafeMemcpyPoolIsa(q, pDesc, sizeof(uint64_t))) {
            return E_INVALIDARG;
        }
    }
    auto* guest_rs = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(q[0]));

    D3D12_COMPUTE_PIPELINE_STATE_DESC cdesc{};
    cdesc.pRootSignature = guest_rs;
    ID3D12PipelineState* out_pso = nullptr;
    const HRESULT hr = CreateComputePipelineState(
        &cdesc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&out_pso));
    if (FAILED(hr) || !out_pso) return hr;

    static const bool s_disable_s67 = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_DISABLE_SLOT67_LIFT", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    ID3D12RootSignature* host_rs = UnwrapRootSignature(guest_rs);
    if (!s_disable_s67 && host_rs && IsKnownProxyPointer(out_pso) && q[5] >= 0x10000ull && q[6] >= 40ull) {
        uint32_t tg_x = 8u, tg_y = 8u, tg_z = 1u;
        bool has_rsrc2 = false;
        uint32_t cs_user_sgprs = 0u;
        uint32_t cs_tgid_mask = 0x7u;
        if (q[1] >= 0x10000ull && q[2] >= 28ull) {
            uint32_t b1_u32[7]{};
            if (SafeMemcpyPoolIsa(b1_u32, reinterpret_cast<const void*>(static_cast<uintptr_t>(q[1])), sizeof(b1_u32))) {
                const uint32_t tx = b1_u32[3] & 0xFFFFu;
                const uint32_t ty = (b1_u32[3] >> 16u) & 0xFFFFu;
                const uint32_t tz = b1_u32[4] & 0xFFFFu;
                if (tx > 0u && tx <= 1024u) tg_x = tx;
                if (ty > 0u && ty <= 1024u) tg_y = ty;
                if (tz > 0u && tz <= 1024u) tg_z = tz;
                const uint32_t rsrc2 = b1_u32[6];
                has_rsrc2 = true;
                cs_user_sgprs = (rsrc2 >> 1u) & 0x1Fu;
                const uint32_t tgid_en = (rsrc2 >> 7u) & 0x7u;
                cs_tgid_mask = (tgid_en != 0u) ? tgid_en : 0x7u;
            }
        }
        std::vector<uint8_t> cs_xs2;
        if (ExtractRetailFssmPoolIsa(q[5], q[6], openscarlett::ShaderStage::Compute, tg_x, tg_y, tg_z, cs_xs2)) {
            auto* proxy = static_cast<PipelineStateXProxy*>(out_pso);
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->is_compute = true;
            pd->has_cs_rsrc2 = has_rsrc2;
            pd->cs_user_sgpr_count = cs_user_sgprs;
            pd->cs_tgid_en_mask = cs_tgid_mask;
            pd->host_rs = host_rs;
            pd->host_rs->AddRef();
            pd->cs_blob = std::move(cs_xs2);
            proxy->SetStandinPso(true);
            proxy->SetPendingXdatGfxDesc(std::move(pd));
        }
    }

    const HRESULT qhr = out_pso->QueryInterface(riid, ppPipelineState);
    out_pso->Release();
    return qhr;
}

HRESULT DeviceX::DeserializeGraphicsPipelineStateX(
    const void* pDesc,
    REFIID riid,
    void** ppPipelineState) {
    if (!pDesc || !ppPipelineState) return E_POINTER;
    *ppPipelineState = nullptr;
    uint64_t q[17]{};
    if (!SafeMemcpyPoolIsa(q, pDesc, sizeof(q))) {
        if (!SafeMemcpyPoolIsa(q, pDesc, 15u * sizeof(uint64_t)) &&
            !SafeMemcpyPoolIsa(q, pDesc, 13u * sizeof(uint64_t)) &&
            !SafeMemcpyPoolIsa(q, pDesc, 9u * sizeof(uint64_t)) &&
            !SafeMemcpyPoolIsa(q, pDesc, sizeof(uint64_t))) {
            return E_INVALIDARG;
        }
    }
    auto* guest_rs = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(q[0]));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gdesc{};
    gdesc.pRootSignature = guest_rs;
    for (UINT r = 0u; r < 8u; ++r) {
        auto& rt = gdesc.BlendState.RenderTarget[r];
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    gdesc.SampleMask = UINT_MAX;
    gdesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gdesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gdesc.RasterizerState.FrontCounterClockwise = FALSE;
    gdesc.RasterizerState.DepthClipEnable = TRUE;
    gdesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gdesc.NumRenderTargets = 1u;
    gdesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gdesc.SampleDesc.Count = 1u;

    bool has_b1_depth = false;
    bool has_b1_stencil_ref = false;
    uint8_t b1_stencil_ref = 0u;
    if (q[1] >= 0x10000ull && q[2] >= 0x130ull && q[2] <= 0x4000ull) {
        const size_t b1_len = static_cast<size_t>(q[2]);
        std::vector<uint8_t> b1_buf(b1_len);
        if (SafeMemcpyPoolIsa(b1_buf.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(q[1])), b1_len)) {
            const uint8_t* b1 = b1_buf.data();
            uint32_t b1_kind = 0u;
            std::memcpy(&b1_kind, b1 + 0x04u, 4u);
            const size_t reg_shift = (b1_kind == 6u && b1_len >= 0x184u) ? 0x54u : 0u;
            auto map_rdna2_blend = [](uint32_t f, bool for_alpha) noexcept -> D3D12_BLEND {
                D3D12_BLEND b = D3D12_BLEND_ONE;
                switch (f & 0x1Fu) {
                    case 0u:  b = D3D12_BLEND_ZERO; break;
                    case 1u:  b = D3D12_BLEND_ONE; break;
                    case 2u:  b = D3D12_BLEND_SRC_COLOR; break;
                    case 3u:  b = D3D12_BLEND_INV_SRC_COLOR; break;
                    case 4u:  b = D3D12_BLEND_SRC_ALPHA; break;
                    case 5u:  b = D3D12_BLEND_INV_SRC_ALPHA; break;
                    case 6u:  b = D3D12_BLEND_DEST_ALPHA; break;
                    case 7u:  b = D3D12_BLEND_INV_DEST_ALPHA; break;
                    case 8u:  b = D3D12_BLEND_DEST_COLOR; break;
                    case 9u:  b = D3D12_BLEND_INV_DEST_COLOR; break;
                    case 10u: b = D3D12_BLEND_SRC_ALPHA_SAT; break;
                    case 13u: b = D3D12_BLEND_BLEND_FACTOR; break;
                    case 14u: b = D3D12_BLEND_INV_BLEND_FACTOR; break;
                    case 16u: b = D3D12_BLEND_SRC1_COLOR; break;
                    case 17u: b = D3D12_BLEND_INV_SRC1_COLOR; break;
                    case 18u: b = D3D12_BLEND_SRC1_ALPHA; break;
                    case 19u: b = D3D12_BLEND_INV_SRC1_ALPHA; break;
                    default:  b = D3D12_BLEND_ONE; break;
                }
                if (for_alpha) {
                    if (b == D3D12_BLEND_SRC_COLOR) b = D3D12_BLEND_SRC_ALPHA;
                    else if (b == D3D12_BLEND_INV_SRC_COLOR) b = D3D12_BLEND_INV_SRC_ALPHA;
                    else if (b == D3D12_BLEND_DEST_COLOR) b = D3D12_BLEND_DEST_ALPHA;
                    else if (b == D3D12_BLEND_INV_DEST_COLOR) b = D3D12_BLEND_INV_DEST_ALPHA;
                    else if (b == D3D12_BLEND_SRC1_COLOR) b = D3D12_BLEND_SRC1_ALPHA;
                    else if (b == D3D12_BLEND_INV_SRC1_COLOR) b = D3D12_BLEND_INV_SRC1_ALPHA;
                }
                return b;
            };
            auto map_rdna2_blend_op = [](uint32_t op) noexcept -> D3D12_BLEND_OP {
                switch (op & 0x7u) {
                    case 0u: return D3D12_BLEND_OP_ADD;
                    case 1u: return D3D12_BLEND_OP_SUBTRACT;
                    case 2u: return D3D12_BLEND_OP_MIN;
                    case 3u: return D3D12_BLEND_OP_MAX;
                    case 4u: return D3D12_BLEND_OP_REV_SUBTRACT;
                    default: return D3D12_BLEND_OP_ADD;
                }
            };

            uint32_t cb_target_mask = 0xFFFFFFFFu;
            uint32_t db_depth_ctrl = 0u;
            uint32_t db_stencil_ctrl = 0u;
            uint32_t db_stencil_refmask = 0x0100FF00u;
            uint32_t sc_mode_cntl = 0u;
            std::memcpy(&cb_target_mask, b1 + 0x78u + reg_shift, 4u);
            std::memcpy(&db_depth_ctrl, b1 + 0xB8u + reg_shift, 4u);
            std::memcpy(&db_stencil_ctrl, b1 + 0xBCu + reg_shift, 4u);
            std::memcpy(&db_stencil_refmask, b1 + 0xC0u + reg_shift, 4u);
            std::memcpy(&sc_mode_cntl, b1 + 0x128u + reg_shift, 4u);
            has_b1_stencil_ref = true;
            b1_stencil_ref = static_cast<uint8_t>(db_stencil_refmask & 0xFFu);

            bool indep_blend = false;
            for (UINT r = 0u; r < 8u; ++r) {
                uint32_t bc = 0u;
                std::memcpy(&bc, b1 + 0xD0u + reg_shift + r * 4u, 4u);
                auto& rt = gdesc.BlendState.RenderTarget[r];
                rt.RenderTargetWriteMask = static_cast<UINT8>((cb_target_mask >> (r * 4u)) & 0xFu);
                const bool blend_en = (bc & 0x40000000u) != 0u;
                rt.BlendEnable = blend_en ? TRUE : FALSE;
                if (blend_en) {
                    rt.SrcBlend = map_rdna2_blend(bc & 0x1Fu, false);
                    rt.BlendOp = map_rdna2_blend_op((bc >> 5u) & 0x7u);
                    rt.DestBlend = map_rdna2_blend((bc >> 8u) & 0x1Fu, false);
                    if ((bc & 0x20000000u) != 0u) {
                        rt.SrcBlendAlpha = map_rdna2_blend((bc >> 16u) & 0x1Fu, true);
                        rt.BlendOpAlpha = map_rdna2_blend_op((bc >> 21u) & 0x7u);
                        rt.DestBlendAlpha = map_rdna2_blend((bc >> 24u) & 0x1Fu, true);
                    } else {
                        rt.SrcBlendAlpha = map_rdna2_blend(bc & 0x1Fu, true);
                        rt.BlendOpAlpha = rt.BlendOp;
                        rt.DestBlendAlpha = map_rdna2_blend((bc >> 8u) & 0x1Fu, true);
                    }
                }
                if (r > 0u) {
                    const auto& r0 = gdesc.BlendState.RenderTarget[0];
                    if (rt.BlendEnable != r0.BlendEnable ||
                        rt.SrcBlend != r0.SrcBlend || rt.DestBlend != r0.DestBlend ||
                        rt.BlendOp != r0.BlendOp || rt.SrcBlendAlpha != r0.SrcBlendAlpha ||
                        rt.DestBlendAlpha != r0.DestBlendAlpha || rt.BlendOpAlpha != r0.BlendOpAlpha ||
                        rt.RenderTargetWriteMask != r0.RenderTargetWriteMask) {
                        indep_blend = true;
                    }
                }
            }
            gdesc.BlendState.IndependentBlendEnable = indep_blend ? TRUE : FALSE;

            has_b1_depth = true;
            gdesc.DepthStencilState.DepthEnable = ((db_depth_ctrl & 0x2u) != 0u) ? TRUE : FALSE;
            gdesc.DepthStencilState.DepthWriteMask =
                ((db_depth_ctrl & 0x4u) != 0u) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            gdesc.DepthStencilState.DepthFunc =
                static_cast<D3D12_COMPARISON_FUNC>(((db_depth_ctrl >> 4u) & 0x7u) + 1u);

            auto map_rdna2_stencil_op = [](uint32_t hw_op) noexcept -> D3D12_STENCIL_OP {
                switch (hw_op & 0xFu) {
                    case 0u: return D3D12_STENCIL_OP_KEEP;
                    case 1u: return D3D12_STENCIL_OP_ZERO;
                    case 2u:
                    case 3u:
                    case 4u: return D3D12_STENCIL_OP_REPLACE;
                    case 5u: return D3D12_STENCIL_OP_INCR_SAT;
                    case 6u: return D3D12_STENCIL_OP_DECR_SAT;
                    case 7u: return D3D12_STENCIL_OP_INVERT;
                    case 8u: return D3D12_STENCIL_OP_INCR;
                    case 9u: return D3D12_STENCIL_OP_DECR;
                    default: return D3D12_STENCIL_OP_KEEP;
                }
            };
            gdesc.DepthStencilState.StencilEnable = ((db_depth_ctrl & 0x1u) != 0u) ? TRUE : FALSE;
            gdesc.DepthStencilState.StencilReadMask = static_cast<UINT8>((db_stencil_refmask >> 8u) & 0xFFu);
            gdesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>((db_stencil_refmask >> 16u) & 0xFFu);
            gdesc.DepthStencilState.FrontFace.StencilFunc =
                static_cast<D3D12_COMPARISON_FUNC>(((db_depth_ctrl >> 8u) & 0x7u) + 1u);
            gdesc.DepthStencilState.FrontFace.StencilFailOp = map_rdna2_stencil_op(db_stencil_ctrl >> 0u);
            gdesc.DepthStencilState.FrontFace.StencilPassOp = map_rdna2_stencil_op(db_stencil_ctrl >> 4u);
            gdesc.DepthStencilState.FrontFace.StencilDepthFailOp = map_rdna2_stencil_op(db_stencil_ctrl >> 8u);
            if ((db_depth_ctrl & 0x80u) != 0u) {
                gdesc.DepthStencilState.BackFace.StencilFunc =
                    static_cast<D3D12_COMPARISON_FUNC>(((db_depth_ctrl >> 20u) & 0x7u) + 1u);
                gdesc.DepthStencilState.BackFace.StencilFailOp = map_rdna2_stencil_op(db_stencil_ctrl >> 12u);
                gdesc.DepthStencilState.BackFace.StencilPassOp = map_rdna2_stencil_op(db_stencil_ctrl >> 16u);
                gdesc.DepthStencilState.BackFace.StencilDepthFailOp = map_rdna2_stencil_op(db_stencil_ctrl >> 20u);
            } else {
                gdesc.DepthStencilState.BackFace = gdesc.DepthStencilState.FrontFace;
            }

            const uint32_t cull_bits = sc_mode_cntl & 0x3u;
            if (cull_bits == 1u) gdesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
            else if (cull_bits == 2u) gdesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            else gdesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            gdesc.RasterizerState.FrontCounterClockwise = ((sc_mode_cntl & 0x4u) == 0u) ? TRUE : FALSE;
        }
    }

    std::vector<std::string> il_names;
    std::vector<D3D12_INPUT_ELEMENT_DESC> il_elems;
    if (q[3] >= 0x10000ull && q[4] >= 0x50ull && q[4] <= 0x4000ull) {
        const size_t b2_len = static_cast<size_t>(q[4]);
        std::vector<uint8_t> b2_buf(b2_len);
        if (SafeMemcpyPoolIsa(b2_buf.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(q[3])), b2_len)) {
            const uint8_t* b2 = b2_buf.data();
            uint32_t sample_mask = UINT_MAX;
            uint32_t pack24 = 0u;
            uint32_t sample_cnt = 1u;
            uint32_t sample_qual = 0u;
            uint32_t num_il = 0u;
            std::memcpy(&sample_mask, b2 + 0x1Cu, 4u);
            std::memcpy(&pack24, b2 + 0x24u, 4u);
            std::memcpy(&sample_cnt, b2 + 0x28u, 4u);
            std::memcpy(&sample_qual, b2 + 0x2Cu, 4u);
            std::memcpy(&num_il, b2 + 0x4Cu, 4u);

            const uint8_t dsv_fmt = static_cast<uint8_t>(pack24 & 0xFFu);
            const uint8_t num_rts = static_cast<uint8_t>((pack24 >> 8u) & 0xFFu);
            const uint8_t topo = static_cast<uint8_t>((pack24 >> 16u) & 0xFFu);
            if (sample_mask != 0u) gdesc.SampleMask = sample_mask;
            if (num_rts <= 8u) {
                gdesc.NumRenderTargets = num_rts;
                for (UINT r = 0u; r < 8u; ++r) {
                    gdesc.RTVFormats[r] = (r < num_rts)
                                              ? SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(b2[0x10u + r]))
                                              : DXGI_FORMAT_UNKNOWN;
                }
            }
            gdesc.DSVFormat = SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(dsv_fmt));
            if (gdesc.DSVFormat == DXGI_FORMAT_UNKNOWN) {
                gdesc.DepthStencilState.DepthEnable = FALSE;
                gdesc.DepthStencilState.StencilEnable = FALSE;
            } else if (!has_b1_depth) {
                gdesc.DepthStencilState.DepthEnable = TRUE;
                gdesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                gdesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            }
            if (topo >= 1u && topo <= 4u) {
                gdesc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(topo);
            } else {
                gdesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
            if (sample_cnt >= 1u && sample_cnt <= 16u) {
                gdesc.SampleDesc.Count = sample_cnt;
                gdesc.SampleDesc.Quality = sample_qual;
            }
            if (num_il > 0u && num_il <= 32u) {
                size_t pos = 0x50u;
                il_names.reserve(num_il);
                il_elems.reserve(num_il);
                for (uint32_t idx = 0u; idx < num_il; ++idx) {
                    if (pos + 4u > b2_len) {
                        il_names.clear();
                        il_elems.clear();
                        break;
                    }
                    uint32_t slen = 0u;
                    std::memcpy(&slen, b2 + pos, 4u);
                    pos += 4u;
                    if (slen == 0u || slen > 64u || pos + slen + 24u > b2_len) {
                        il_names.clear();
                        il_elems.clear();
                        break;
                    }
                    const char* raw_s = reinterpret_cast<const char*>(b2 + pos);
                    size_t actual_len = 0u;
                    while (actual_len < slen && raw_s[actual_len] != '\0') ++actual_len;
                    il_names.emplace_back(raw_s, actual_len);
                    pos += slen;

                    uint32_t fields[6]{};
                    std::memcpy(fields, b2 + pos, 24u);
                    pos += 24u;

                    D3D12_INPUT_ELEMENT_DESC ie{};
                    ie.SemanticName = il_names.back().c_str();
                    ie.SemanticIndex = fields[0];
                    ie.Format = SanitizeXboxDxgiFormat(static_cast<DXGI_FORMAT>(fields[1]));
                    ie.InputSlot = fields[2];
                    ie.AlignedByteOffset = fields[3];
                    ie.InputSlotClass = (fields[4] == 1u)
                                            ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    ie.InstanceDataStepRate = fields[5];
                    il_elems.push_back(ie);
                }
            }
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC standin_desc = gdesc;
    standin_desc.VS = {};
    standin_desc.PS = {};
    ID3D12PipelineState* out_pso = nullptr;
    const HRESULT hr = CreateGraphicsPipelineState(
        &standin_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&out_pso));
    if (FAILED(hr) || !out_pso) return hr;

    static const bool s_disable_s57 = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_DISABLE_SLOT57_LIFT", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    ID3D12RootSignature* host_rs = UnwrapRootSignature(guest_rs);
    const bool has_s57_vs = (q[9] >= 0x10000ull && q[10] >= 40ull);
    const bool has_s57_ms = (!has_s57_vs && q[13] >= 0x10000ull && q[14] >= 40ull);
    if (!s_disable_s57 && host_rs && IsKnownProxyPointer(out_pso) && (has_s57_vs || has_s57_ms)) {
        std::vector<uint8_t> vs_xs2;
        std::vector<uint8_t> ms_xs2;
        std::vector<uint8_t> ps_xs2;
        const bool vs_ok = !has_s57_vs || ExtractRetailFssmPoolIsa(
            q[9], q[10], openscarlett::ShaderStage::Vertex, 1u, 1u, 1u, vs_xs2);
        const bool ms_ok = !has_s57_ms || ExtractRetailFssmPoolIsa(
            q[13], q[14], openscarlett::ShaderStage::Mesh, 128u, 1u, 1u, ms_xs2);
        const bool has_ps = (q[5] >= 0x10000ull && q[6] >= 40ull);
        const bool ps_ok = !has_ps || ExtractRetailFssmPoolIsa(
            q[5], q[6], openscarlett::ShaderStage::Pixel, 1u, 1u, 1u, ps_xs2);
        if (vs_ok && ms_ok && ps_ok) {
            auto parse_stage_cbv_mask = [](uint64_t addr, uint64_t sz) noexcept -> uint32_t {
                if (addr < 0x10000ull || sz < 48ull || sz > 0x4000ull) return 0xFFFFu;
                uint8_t mbuf[4096]{};
                const size_t n = static_cast<size_t>(std::min<uint64_t>(sz, sizeof(mbuf)));
                if (!SafeMemcpyPoolIsa(mbuf, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), n)) {
                    return 0xFFFFu;
                }
                bool has_cbv = false;
                bool has_buf_srv = false;
                uint32_t explicit_cbv_slots = 0u;
                for (size_t off = 16u; off + 32u <= n; off += 32u) {
                    uint32_t tag = 0u;
                    std::memcpy(&tag, mbuf + off, 4u);
                    if (tag == 0x43u) break;
                    if (tag == 0x41u) {
                        const uint8_t rtype = mbuf[off + 4u];
                        uint32_t tbl_off = 0u;
                        std::memcpy(&tbl_off, mbuf + off + 16u, 4u);
                        if (rtype == 2u) {
                            has_cbv = true;
                            const uint32_t dslot = tbl_off / 8u;
                            if (dslot < 16u) explicit_cbv_slots |= (1u << dslot);
                        }
                    } else if (tag == 0x42u) {
                        const uint8_t rcls = mbuf[off + 9u];
                        const uint8_t rkind = mbuf[off + 10u];
                        if (rcls == 2u || rkind == 13u) {
                            has_cbv = true;
                        } else if (rcls == 0u && (rkind >= 10u && rkind <= 12u)) {
                            has_buf_srv = true;
                        }
                    }
                }
                if (has_cbv && !has_buf_srv) return 0xFFFFu;
                if (!has_cbv && has_buf_srv) return 0u;
                return explicit_cbv_slots ? explicit_cbv_slots : 0xFFFFu;
            };
            auto* proxy = static_cast<PipelineStateXProxy*>(out_pso);
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->is_compute = false;
            pd->is_mesh = has_s57_ms;
            pd->from_slot57 = true;
            pd->has_slot57_stencil_ref = has_b1_stencil_ref;
            pd->slot57_stencil_ref = b1_stencil_ref;
            pd->vs_cbv_slot_mask = has_s57_vs ? parse_stage_cbv_mask(q[11], q[12]) : 0xFFFFu;
            pd->ms_cbv_slot_mask = has_s57_ms ? parse_stage_cbv_mask(q[15], q[16]) : 0xFFFFu;
            pd->ps_cbv_slot_mask = has_ps ? parse_stage_cbv_mask(q[7], q[8]) : 0xFFFFu;
            pd->gpso = gdesc;
            pd->gpso.pRootSignature = host_rs;
            pd->host_rs = host_rs;
            pd->host_rs->AddRef();
            pd->vs_blob = std::move(vs_xs2);
            pd->ms_blob = std::move(ms_xs2);
            pd->ps_blob = std::move(ps_xs2);
            pd->il_names = std::move(il_names);
            pd->il_elems = std::move(il_elems);
            for (size_t i = 0u; i < pd->il_elems.size() && i < pd->il_names.size(); ++i) {
                pd->il_elems[i].SemanticName = pd->il_names[i].c_str();
            }
            pd->gpso.InputLayout = pd->il_elems.empty()
                                       ? D3D12_INPUT_LAYOUT_DESC{nullptr, 0u}
                                       : D3D12_INPUT_LAYOUT_DESC{pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
            proxy->SetStandinPso(true);
            proxy->SetPendingXdatGfxDesc(std::move(pd));
        }
    }

    const HRESULT qhr = out_pso->QueryInterface(riid, ppPipelineState);
    out_pso->Release();
    return qhr;
}

HRESULT BuildHostMeshPipelineState(
    ID3D12Device2* host_dev2,
    ID3D12RootSignature* host_rs,
    const std::vector<uint8_t>& ms_dxil,
    const std::vector<uint8_t>& as_dxil,
    const std::vector<uint8_t>& ps_dxil,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpso,
    ID3D12PipelineState** out_pso) {
    if (!host_dev2 || !host_rs || ms_dxil.empty() || !out_pso) return E_INVALIDARG;
    *out_pso = nullptr;

    std::vector<uint8_t> stream_bytes;
    stream_bytes.reserve(640u);
    auto append_subobj = [&](auto subobj) {
        const size_t aligned = (stream_bytes.size() + sizeof(void*) - 1u) & ~(sizeof(void*) - 1u);
        if (aligned > stream_bytes.size()) stream_bytes.resize(aligned, 0u);
        const size_t off = stream_bytes.size();
        stream_bytes.resize(off + sizeof(subobj), 0u);
        std::memcpy(stream_bytes.data() + off, &subobj, sizeof(subobj));
    };

    StreamSubobjLayout<ID3D12RootSignature*> s_rs{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, host_rs};
    append_subobj(s_rs);

    if (!as_dxil.empty()) {
        StreamSubobjLayout<D3D12_SHADER_BYTECODE> s_as{
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, {as_dxil.data(), as_dxil.size()}};
        append_subobj(s_as);
    }

    StreamSubobjLayout<D3D12_SHADER_BYTECODE> s_ms{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, {ms_dxil.data(), ms_dxil.size()}};
    append_subobj(s_ms);

    if (!ps_dxil.empty()) {
        StreamSubobjLayout<D3D12_SHADER_BYTECODE> s_ps{
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps_dxil.data(), ps_dxil.size()}};
        append_subobj(s_ps);
    }

    StreamSubobjLayout<D3D12_BLEND_DESC> s_blend{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, gpso.BlendState};
    append_subobj(s_blend);

    StreamSubobjLayout<UINT> s_smask{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, gpso.SampleMask ? gpso.SampleMask : UINT_MAX};
    append_subobj(s_smask);

    StreamSubobjLayout<D3D12_RASTERIZER_DESC> s_rast{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, gpso.RasterizerState};
    append_subobj(s_rast);

    StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC> s_ds{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, gpso.DepthStencilState};
    append_subobj(s_ds);

    D3D12_RT_FORMAT_ARRAY rta{};
    rta.NumRenderTargets = std::min<UINT>(gpso.NumRenderTargets, 8u);
    for (UINT i = 0u; i < rta.NumRenderTargets; ++i) {
        rta.RTFormats[i] = gpso.RTVFormats[i];
    }
    StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY> s_rts{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rta};
    append_subobj(s_rts);

    StreamSubobjLayout<DXGI_FORMAT> s_dsv{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, gpso.DSVFormat};
    append_subobj(s_dsv);

    StreamSubobjLayout<DXGI_SAMPLE_DESC> s_samp{
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,
        gpso.SampleDesc.Count ? gpso.SampleDesc : DXGI_SAMPLE_DESC{1u, 0u}};
    append_subobj(s_samp);

    const size_t words = (stream_bytes.size() + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    std::vector<uint64_t> aligned_words(words, 0ull);
    std::memcpy(aligned_words.data(), stream_bytes.data(), stream_bytes.size());
    D3D12_PIPELINE_STATE_STREAM_DESC sd{};
    sd.SizeInBytes = stream_bytes.size();
    sd.pPipelineStateSubobjectStream = aligned_words.data();
    return host_dev2->CreatePipelineState(
        &sd, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(out_pso));
}

void DeviceX::LiftPendingXdatGraphicsPso(PipelineStateXProxy* proxy) {
    if (!proxy || !host_dev_) return;
    PendingXdatGfxDesc* pd = proxy->PendingXdatDesc();
    if (!pd || !pd->host_rs) return;
    if (pd->is_compute ? pd->cs_blob.empty() : (pd->is_mesh ? pd->ms_blob.empty() : pd->vs_blob.empty())) return;

    static const bool s_disable_cs_lift = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_DISABLE_XDAT_CS_LIFT", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    if (pd->is_compute && s_disable_cs_lift) {
        return;
    }

    static std::mutex s_xdat_cache_mu;
    static std::unordered_map<uint64_t, std::vector<uint8_t>> s_xdat_dxil_cache;
    static const std::string s_disk_cache_dir = []() {
        char dir[260]{};
        if (::GetEnvironmentVariableA("OPENSCARLETT_DXIL_CACHE_DIR", dir, sizeof(dir)) > 0) {
            ::CreateDirectoryA(dir, nullptr);
            return std::string(dir);
        }
        const DWORD attr = ::GetFileAttributesA("E:\\OpenScarlett");
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ::CreateDirectoryA("E:\\OpenScarlett\\.tmp", nullptr);
            ::CreateDirectoryA("E:\\OpenScarlett\\.tmp\\dxil_cache", nullptr);
            return std::string("E:\\OpenScarlett\\.tmp\\dxil_cache");
        }
        return std::string{};
    }();
    auto lift_stage = [&](const std::vector<uint8_t>& blob, std::vector<uint8_t>& out_dxil,
                          std::string& out_err, bool is_vs_stage = false,
                          bool rectlist_zero_ia = false, bool is_ms_stage = false,
                          uint32_t max_ms_params = 0xFFFFFFFFu,
                          uint32_t* out_ps_param_in_count = nullptr) -> bool {
        if (blob.empty()) return true;
        const bool is_xdat = IsXboxXdatDxbcBlob(blob.data(), blob.size());
        const bool is_xs2 = !is_xdat && openscarlett::IsXboxShaderContainerBlob(blob.data(), blob.size());
        if (!is_xdat && !is_xs2) {
            out_dxil = blob;
            return true;
        }
        uint64_t h = 14695981039346656037ull;
        for (uint8_t b : blob) {
            h ^= b;
            h *= 1099511628211ull;
        }
        if (rectlist_zero_ia) {
            h ^= 0x524543544C495354ull;
            h *= 1099511628211ull;
        }
        if (is_ms_stage) {
            h ^= (static_cast<uint64_t>(max_ms_params) << 12u) ^
                 (pd->as_blob.empty() ? 0x4D5330ull : 0x4D5331ull);
            h *= 1099511628211ull;
        }
        const uint32_t stage_cbv_mask =
            pd->is_compute
                ? 0u
                : (pd->from_slot57
                       ? (is_vs_stage ? pd->vs_cbv_slot_mask
                                      : (is_ms_stage ? pd->ms_cbv_slot_mask : pd->ps_cbv_slot_mask))
                       : 0xFFFFu);
        if (is_xs2 && pd->is_compute) {
            h ^= (static_cast<uint64_t>(pd->cs_user_sgpr_count) << 8u) ^
                 (static_cast<uint64_t>(pd->cs_tgid_en_mask) << 16u) ^
                 (pd->has_cs_rsrc2 ? 0xC572ull : 0xC500ull);
            h *= 1099511628211ull;
        } else if (is_xs2 && !pd->is_compute) {
            h ^= (static_cast<uint64_t>(stage_cbv_mask) << 8u) ^ 0xA5A5ull;
            h *= 1099511628211ull;
        }
        if (is_vs_stage && is_xs2) {
            h ^= pd->ps_blob.empty() ? 0x10ull : 0x40ull;
            h *= 1099511628211ull;
            for (const auto& ie : pd->il_elems) {
                h ^= static_cast<uint64_t>(ie.Format) +
                     (static_cast<uint64_t>(ie.SemanticIndex) << 16u) +
                     (static_cast<uint64_t>(ie.AlignedByteOffset) << 24u);
                h *= 1099511628211ull;
            }
        }
        if (!out_ps_param_in_count) {
            std::lock_guard<std::mutex> lk(s_xdat_cache_mu);
            auto it = s_xdat_dxil_cache.find(h);
            if (it != s_xdat_dxil_cache.end()) {
                out_dxil = it->second;
                return !out_dxil.empty();
            }
        }
        if (!out_ps_param_in_count && !s_disk_cache_dir.empty()) {
            char cpath[320]{};
            std::snprintf(cpath, sizeof(cpath), "%s\\v25_%016llx.dxil",
                          s_disk_cache_dir.c_str(), static_cast<unsigned long long>(h));
            FILE* cf = nullptr;
            if (::fopen_s(&cf, cpath, "rb") == 0 && cf) {
                std::fseek(cf, 0, SEEK_END);
                const long csz = std::ftell(cf);
                std::fseek(cf, 0, SEEK_SET);
                if (csz > 32) {
                    out_dxil.resize(static_cast<size_t>(csz));
                    if (std::fread(out_dxil.data(), 1u, out_dxil.size(), cf) == out_dxil.size()) {
                        std::fclose(cf);
                        std::lock_guard<std::mutex> lk(s_xdat_cache_mu);
                        s_xdat_dxil_cache[h] = out_dxil;
                        return true;
                    }
                    out_dxil.clear();
                }
                std::fclose(cf);
            }
        }
        openscarlett::XboxContainerLiftResult lr{};
        openscarlett::XboxContainerLiftOptions lo{};
        lo.lift_options.use_sm65 = true;
        lo.lift_options.emit_spirv = false;
        lo.lift_options.compile_dxil_with_dxc = true;
        if (is_vs_stage && rectlist_zero_ia) {
            lo.lift_options.stage_contract.rectlist_zero_ia_vs = true;
        }
        if (is_ms_stage) {
            lo.lift_options.mesh_has_payload = !pd->as_blob.empty();
            lo.lift_options.stage_contract.max_param_outputs = max_ms_params;
        }
        if (is_xs2) {
            auto& pb = lo.lift_options.stage_contract.psv0_bindings;
            pb.enabled = true;
            pb.in0_is_srv = pd->is_compute || (stage_cbv_mask == 0u);
            pb.in0_is_cbv = !pb.in0_is_srv;
            pb.in0_reg = 0u;
            pb.in0_space = 2u;
            pb.space2_cbv_slot_mask = stage_cbv_mask;
            pb.has_tex0 = true;
            pb.tex0_reg = 0u;
            pb.tex0_space = 2u;
            pb.has_rwtex0 = true;
            pb.rwtex0_reg = 0u;
            pb.rwtex0_space = 2u;
            pb.rwtex0_res_kind = 0u;
            pb.has_rwimg_u32 = true;
            pb.rwimg_u32_reg = 1u;
            pb.rwimg_u32_space = 2u;
            pb.rwimg_u32_res_kind = 0u;
            pb.cs_fssm_abi = pd->is_compute;
            if (pd->is_compute && pd->has_cs_rsrc2) {
                pb.cs_user_sgpr_count = pd->cs_user_sgpr_count;
                pb.cs_tgid_en_mask = pd->cs_tgid_en_mask;
            }
        }
        if (is_vs_stage && is_xs2) {
            if (!pd->ps_blob.empty()) {
                lo.lift_options.stage_contract.min_param_outputs = 4u;
            }
            if (!pd->il_elems.empty()) {
                auto fmt_comp_type = [](DXGI_FORMAT fmt) noexcept -> uint8_t {
                    switch (fmt) {
                        case DXGI_FORMAT_R32G32B32A32_UINT:
                        case DXGI_FORMAT_R32G32B32_UINT:
                        case DXGI_FORMAT_R16G16B16A16_UINT:
                        case DXGI_FORMAT_R32G32_UINT:
                        case DXGI_FORMAT_R10G10B10A2_UINT:
                        case DXGI_FORMAT_R8G8B8A8_UINT:
                        case DXGI_FORMAT_R16G16_UINT:
                        case DXGI_FORMAT_R32_UINT:
                        case DXGI_FORMAT_R8G8_UINT:
                        case DXGI_FORMAT_R16_UINT:
                        case DXGI_FORMAT_R8_UINT:
                            return 1u;
                        case DXGI_FORMAT_R32G32B32A32_SINT:
                        case DXGI_FORMAT_R32G32B32_SINT:
                        case DXGI_FORMAT_R16G16B16A16_SINT:
                        case DXGI_FORMAT_R32G32_SINT:
                        case DXGI_FORMAT_R8G8B8A8_SINT:
                        case DXGI_FORMAT_R16G16_SINT:
                        case DXGI_FORMAT_R32_SINT:
                        case DXGI_FORMAT_R8G8_SINT:
                        case DXGI_FORMAT_R16_SINT:
                        case DXGI_FORMAT_R8_SINT:
                            return 2u;
                        default:
                            return 3u;
                    }
                };
                auto fmt_elem_sizes = [](DXGI_FORMAT fmt, uint8_t& elem_sz, uint8_t& comp_sz) noexcept {
                    switch (fmt) {
                        case DXGI_FORMAT_R32G32B32A32_FLOAT:
                        case DXGI_FORMAT_R32G32B32A32_UINT:
                        case DXGI_FORMAT_R32G32B32A32_SINT:
                        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
                            elem_sz = 16u; comp_sz = 4u; return;
                        case DXGI_FORMAT_R32G32B32_FLOAT:
                        case DXGI_FORMAT_R32G32B32_UINT:
                        case DXGI_FORMAT_R32G32B32_SINT:
                        case DXGI_FORMAT_R32G32B32_TYPELESS:
                            elem_sz = 12u; comp_sz = 4u; return;
                        case DXGI_FORMAT_R32G32_FLOAT:
                        case DXGI_FORMAT_R32G32_UINT:
                        case DXGI_FORMAT_R32G32_SINT:
                        case DXGI_FORMAT_R32G32_TYPELESS:
                            elem_sz = 8u; comp_sz = 4u; return;
                        case DXGI_FORMAT_R16G16B16A16_FLOAT:
                        case DXGI_FORMAT_R16G16B16A16_UNORM:
                        case DXGI_FORMAT_R16G16B16A16_SNORM:
                        case DXGI_FORMAT_R16G16B16A16_UINT:
                        case DXGI_FORMAT_R16G16B16A16_SINT:
                        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                            elem_sz = 8u; comp_sz = 2u; return;
                        case DXGI_FORMAT_R16G16_FLOAT:
                        case DXGI_FORMAT_R16G16_UNORM:
                        case DXGI_FORMAT_R16G16_SNORM:
                        case DXGI_FORMAT_R16G16_UINT:
                        case DXGI_FORMAT_R16G16_SINT:
                        case DXGI_FORMAT_R16G16_TYPELESS:
                            elem_sz = 4u; comp_sz = 2u; return;
                        case DXGI_FORMAT_R8G8B8A8_UNORM:
                        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                        case DXGI_FORMAT_R8G8B8A8_SNORM:
                        case DXGI_FORMAT_R8G8B8A8_UINT:
                        case DXGI_FORMAT_R8G8B8A8_SINT:
                        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                        case DXGI_FORMAT_B8G8R8A8_UNORM:
                        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                        case DXGI_FORMAT_B8G8R8X8_UNORM:
                            elem_sz = 4u; comp_sz = 1u; return;
                        case DXGI_FORMAT_R8G8_UNORM:
                        case DXGI_FORMAT_R8G8_SNORM:
                        case DXGI_FORMAT_R8G8_UINT:
                        case DXGI_FORMAT_R8G8_SINT:
                            elem_sz = 2u; comp_sz = 1u; return;
                        case DXGI_FORMAT_R16_FLOAT:
                        case DXGI_FORMAT_R16_UNORM:
                        case DXGI_FORMAT_R16_SNORM:
                        case DXGI_FORMAT_R16_UINT:
                        case DXGI_FORMAT_R16_SINT:
                            elem_sz = 2u; comp_sz = 2u; return;
                        case DXGI_FORMAT_R8_UNORM:
                        case DXGI_FORMAT_R8_SNORM:
                        case DXGI_FORMAT_R8_UINT:
                        case DXGI_FORMAT_R8_SINT:
                            elem_sz = 1u; comp_sz = 1u; return;
                        default:
                            elem_sz = 4u; comp_sz = 4u; return;
                    }
                };
                uint8_t cur_vgpr = 4u;
                for (size_t i = 0u; i < pd->il_elems.size() && i < pd->il_names.size(); ++i) {
                    openscarlett::VsIaInputBinding ib{};
                    ib.semantic_name = pd->il_names[i];
                    ib.semantic_index = pd->il_elems[i].SemanticIndex;
                    ib.mask = 0xFu;
                    ib.component_type = fmt_comp_type(pd->il_elems[i].Format);
                    fmt_elem_sizes(pd->il_elems[i].Format, ib.elem_byte_size, ib.comp_byte_size);
                    ib.dst_vgpr_base = cur_vgpr;
                    ib.byte_offset = pd->il_elems[i].AlignedByteOffset;
                    ib.has_byte_offset =
                        (pd->il_elems[i].InputSlot == 0u &&
                         pd->il_elems[i].AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT);
                    cur_vgpr = static_cast<uint8_t>(cur_vgpr + 4u);
                    lo.lift_options.stage_contract.vs_ia_inputs.push_back(std::move(ib));
                }
            }
        }
        const auto st = openscarlett::LiftXboxShaderContainer(blob.data(), blob.size(), lr, lo);
        const bool ok = st.ok() && !lr.lift.pc_dxil.empty();
        if (ok && out_ps_param_in_count) {
            uint32_t max_p = 0u;
            bool saw_si = false;
            for (const auto& bb : lr.lift.ir.blocks) {
                for (const auto& ins : bb.instructions) {
                    if (ins.op == openscarlett::IrOp::StageInput) {
                        saw_si = true;
                        const uint32_t ps = (ins.imm_u32 >> 2u) & 31u;
                        if (ps + 1u > max_p) max_p = ps + 1u;
                    }
                }
            }
            *out_ps_param_in_count = saw_si ? std::max(1u, max_p) : 0u;
        }
        if (!ok) {
            out_err = st.message;
        } else if (!s_disk_cache_dir.empty()) {
            char cpath[320]{};
            std::snprintf(cpath, sizeof(cpath), "%s\\v25_%016llx.dxil",
                          s_disk_cache_dir.c_str(), static_cast<unsigned long long>(h));
            FILE* cf = nullptr;
            if (::fopen_s(&cf, cpath, "wb") == 0 && cf) {
                std::fwrite(lr.lift.pc_dxil.data(), 1u, lr.lift.pc_dxil.size(), cf);
                std::fclose(cf);
            }
        }
        {
            std::lock_guard<std::mutex> lk(s_xdat_cache_mu);
            s_xdat_dxil_cache[h] = ok ? lr.lift.pc_dxil : std::vector<uint8_t>{};
        }
        if (ok) out_dxil = std::move(lr.lift.pc_dxil);
        return ok;
    };

    static std::atomic<uint32_t> s_bound_seq{0u};
    const uint32_t seq = s_bound_seq.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t lift_t0_ms = ::GetTickCount64();
    if (seq < 96u) {
        std::fprintf(stderr,
                     "[OpenScarlett][d3d12_xs] PRE-LIFT #%u pso=%p compute=%d vs=%zuB ps=%zuB cs=%zuB\n",
                     seq, static_cast<void*>(proxy), pd->is_compute ? 1 : 0,
                     pd->vs_blob.size(), pd->ps_blob.size(), pd->cs_blob.size());
        std::fflush(stderr);
    }

    if (pd->is_compute) {
        {
            char dump_dir[260]{};
            if (seq < 96u && ::GetEnvironmentVariableA("OPENSCARLETT_DUMP_BOUND_PSO", dump_dir, sizeof(dump_dir)) > 0) {
                ::CreateDirectoryA(dump_dir, nullptr);
                char pbuf[320]{};
                std::snprintf(pbuf, sizeof(pbuf), "%s\\bound_%02u_cs.bin", dump_dir, seq);
                FILE* f = nullptr;
                if (::fopen_s(&f, pbuf, "wb") == 0 && f) {
                    std::fwrite(pd->cs_blob.data(), 1u, pd->cs_blob.size(), f);
                    std::fclose(f);
                }
            }
        }
        std::vector<uint8_t> cs_dxil;
        std::string cs_err;
        const bool cs_ok = lift_stage(pd->cs_blob, cs_dxil, cs_err);
        lazy_lift_count_.fetch_add(1u, std::memory_order_release);
        if (!cs_ok || cs_dxil.empty()) {
            if (seq < 96u) {
                std::fprintf(stderr,
                             "[OpenScarlett][d3d12_xs] LAZY-LIFT-CS #%u FAIL (%llums) pso=%p cs(%zuB '%s')\n",
                             seq, static_cast<unsigned long long>(::GetTickCount64() - lift_t0_ms),
                             static_cast<void*>(proxy), pd->cs_blob.size(), cs_err.c_str());
                std::fflush(stderr);
            }
            return;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
        cpso.pRootSignature = pd->host_rs;
        cpso.CS = {cs_dxil.data(), cs_dxil.size()};
        ID3D12PipelineState* lifted_cs_pso = nullptr;
        const HRESULT chr = host_dev_->CreateComputePipelineState(
            &cpso, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&lifted_cs_pso));
        if (SUCCEEDED(chr) && lifted_cs_pso) {
            proxy->AdoptLiftedHostPso(lifted_cs_pso);
            lifted_cs_pso->Release();
        }
        if (seq < 96u) {
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] LAZY-LIFT-CS #%u (%llums) pso=%p rs=%p (raw=%zuB -> dxil=%zuB) -> hr=0x%08lX\n",
                         seq, static_cast<unsigned long long>(::GetTickCount64() - lift_t0_ms),
                         static_cast<void*>(proxy), static_cast<void*>(pd->host_rs),
                         pd->cs_blob.size(), cs_dxil.size(), static_cast<unsigned long>(chr));
            std::fflush(stderr);
        }
        return;
    }

    if (pd->is_mesh) {
        if (!host_dev2_ || adapter_info_.mesh_shader_tier == 0u) {
            return;
        }
        std::vector<uint8_t> ps_dxil, as_dxil, ms_dxil;
        std::string ps_err, as_err, ms_err;
        uint32_t ps_param_in_count = 0u;
        const bool ps_ok = lift_stage(pd->ps_blob, ps_dxil, ps_err, false, false, false, 0xFFFFFFFFu,
                                      pd->ps_blob.empty() ? nullptr : &ps_param_in_count);
        const bool as_ok = lift_stage(pd->as_blob, as_dxil, as_err);
        const uint32_t max_ms_params = pd->ps_blob.empty() ? 0u : ps_param_in_count;
        const bool ms_ok = lift_stage(pd->ms_blob, ms_dxil, ms_err, false, false, /*is_ms_stage=*/true, max_ms_params);
        lazy_lift_count_.fetch_add(1u, std::memory_order_release);
        if (!ms_ok || !as_ok || !ps_ok || ms_dxil.empty()) {
            if (seq < 96u) {
                std::fprintf(stderr,
                             "[OpenScarlett][d3d12_xs] LAZY-LIFT-MS #%u FAIL pso=%p ms(%zuB ok=%d '%s') ps(%zuB ok=%d '%s')\n",
                             seq, static_cast<void*>(proxy),
                             pd->ms_blob.size(), ms_ok ? 1 : 0, ms_err.c_str(),
                             pd->ps_blob.size(), ps_ok ? 1 : 0, ps_err.c_str());
                std::fflush(stderr);
            }
            return;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpso = pd->gpso;
        gpso.DSVFormat = SanitizePsoDsvFormat(gpso.DSVFormat);
        if (gpso.DSVFormat == DXGI_FORMAT_UNKNOWN) {
            gpso.DepthStencilState.DepthEnable = FALSE;
            gpso.DepthStencilState.StencilEnable = FALSE;
            gpso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        if (gpso.NumRenderTargets > 8u) gpso.NumRenderTargets = 8u;
        for (UINT r = 0u; r < gpso.NumRenderTargets; ++r) {
            gpso.RTVFormats[r] = SanitizeXboxDxgiFormat(gpso.RTVFormats[r]);
            if (gpso.RTVFormats[r] == DXGI_FORMAT_UNKNOWN && r == 0u && !ps_dxil.empty()) {
                gpso.RTVFormats[r] = DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }
        for (UINT r = gpso.NumRenderTargets; r < 8u; ++r) gpso.RTVFormats[r] = DXGI_FORMAT_UNKNOWN;
        SanitizeGraphicsPsoDescForDesktop(gpso);

        ID3D12PipelineState* lifted_ms_pso = nullptr;
        HRESULT mhr = BuildHostMeshPipelineState(
            host_dev2_, pd->host_rs, ms_dxil, as_dxil, ps_dxil, gpso, &lifted_ms_pso);
        if (FAILED(mhr) && gpso.NumRenderTargets == 0u && gpso.DSVFormat == DXGI_FORMAT_UNKNOWN && !ps_dxil.empty()) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC retry_gpso = gpso;
            retry_gpso.NumRenderTargets = 1u;
            retry_gpso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            mhr = BuildHostMeshPipelineState(
                host_dev2_, pd->host_rs, ms_dxil, as_dxil, ps_dxil, retry_gpso, &lifted_ms_pso);
            if (SUCCEEDED(mhr)) gpso = retry_gpso;
        }
        if (FAILED(mhr)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC safe_gpso = gpso;
            safe_gpso.SampleDesc = {1u, 0u};
            safe_gpso.SampleMask = UINT_MAX;
            safe_gpso.RasterizerState.ForcedSampleCount = 0u;
            safe_gpso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            safe_gpso.RasterizerState.MultisampleEnable = FALSE;
            safe_gpso.RasterizerState.AntialiasedLineEnable = FALSE;
            safe_gpso.BlendState.AlphaToCoverageEnable = FALSE;
            safe_gpso.BlendState.IndependentBlendEnable = FALSE;
            mhr = BuildHostMeshPipelineState(
                host_dev2_, pd->host_rs, ms_dxil, as_dxil, ps_dxil, safe_gpso, &lifted_ms_pso);
            if (SUCCEEDED(mhr)) gpso = safe_gpso;
        }
        if (SUCCEEDED(mhr) && lifted_ms_pso) {
            pd->lifted_gpso = gpso;
            pd->lifted_ms_dxil = std::move(ms_dxil);
            pd->lifted_as_dxil = std::move(as_dxil);
            pd->lifted_ps_dxil = std::move(ps_dxil);
            pd->has_lifted_gfx_dxil = true;
            proxy->AdoptLiftedHostPso(lifted_ms_pso);
            lifted_ms_pso->Release();
        }
        if (seq < 96u) {
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] LAZY-LIFT-MS #%u (%llums) pso=%p rs=%p (raw ms=%zuB ps=%zuB params<=%u -> dxil ms=%zuB ps=%zuB rts=%u fmt0=%d dsv=%d) -> hr=0x%08lX\n",
                         seq, static_cast<unsigned long long>(::GetTickCount64() - lift_t0_ms),
                         static_cast<void*>(proxy), static_cast<void*>(pd->host_rs),
                         pd->ms_blob.size(), pd->ps_blob.size(), max_ms_params,
                         pd->lifted_ms_dxil.size(), pd->lifted_ps_dxil.size(),
                         gpso.NumRenderTargets,
                         gpso.NumRenderTargets > 0u ? static_cast<int>(gpso.RTVFormats[0]) : 0,
                         static_cast<int>(gpso.DSVFormat), static_cast<unsigned long>(mhr));
            std::fflush(stderr);
        }
        return;
    }

    {
        char dump_dir[260]{};
        if (seq < 96u && ::GetEnvironmentVariableA("OPENSCARLETT_DUMP_BOUND_PSO", dump_dir, sizeof(dump_dir)) > 0) {
            ::CreateDirectoryA(dump_dir, nullptr);
            auto dump_b = [&](const char* stg, const std::vector<uint8_t>& b) {
                if (b.empty()) return;
                char pbuf[320]{};
                std::snprintf(pbuf, sizeof(pbuf), "%s\\bound_%02u_%s.bin", dump_dir, seq, stg);
                FILE* f = nullptr;
                if (::fopen_s(&f, pbuf, "wb") == 0 && f) {
                    std::fwrite(b.data(), 1u, b.size(), f);
                    std::fclose(f);
                }
            };
            dump_b("vs", pd->vs_blob);
            dump_b("ps", pd->ps_blob);
        }
    }

    std::vector<uint8_t> vs_dxil, ps_dxil, hs_dxil, ds_dxil, gs_dxil;
    std::string vs_err, ps_err, hs_err, ds_err, gs_err;
    const bool vs_ok = lift_stage(pd->vs_blob, vs_dxil, vs_err, /*is_vs_stage=*/true);
    const bool ps_ok = lift_stage(pd->ps_blob, ps_dxil, ps_err);
    const bool hs_ok = lift_stage(pd->hs_blob, hs_dxil, hs_err);
    const bool ds_ok = lift_stage(pd->ds_blob, ds_dxil, ds_err);
    const bool gs_ok = lift_stage(pd->gs_blob, gs_dxil, gs_err);

    lazy_lift_count_.fetch_add(1u, std::memory_order_release);
    if (!vs_ok || !ps_ok || !hs_ok || !ds_ok || !gs_ok) {
        if (seq < 96u) {
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] LAZY-LIFT #%u FAIL pso=%p vs(%zuB ok=%d '%s') ps(%zuB ok=%d '%s')\n",
                         seq, static_cast<void*>(proxy),
                         pd->vs_blob.size(), vs_ok ? 1 : 0, vs_err.c_str(),
                         pd->ps_blob.size(), ps_ok ? 1 : 0, ps_err.c_str());
            std::fflush(stderr);
        }
        return;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpso = pd->gpso;
    gpso.pRootSignature = pd->host_rs;
    gpso.VS = {vs_dxil.data(), vs_dxil.size()};
    gpso.PS = ps_dxil.empty() ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                              : D3D12_SHADER_BYTECODE{ps_dxil.data(), ps_dxil.size()};
    gpso.HS = hs_dxil.empty() ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                              : D3D12_SHADER_BYTECODE{hs_dxil.data(), hs_dxil.size()};
    gpso.DS = ds_dxil.empty() ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                              : D3D12_SHADER_BYTECODE{ds_dxil.data(), ds_dxil.size()};
    gpso.GS = gs_dxil.empty() ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                              : D3D12_SHADER_BYTECODE{gs_dxil.data(), gs_dxil.size()};
    if (!pd->il_elems.empty()) {
        for (size_t i = 0u; i < pd->il_elems.size() && i < pd->il_names.size(); ++i) {
            pd->il_elems[i].SemanticName = pd->il_names[i].c_str();
            pd->il_elems[i].Format = SanitizeXboxDxgiFormat(pd->il_elems[i].Format);
            if (pd->il_elems[i].InputSlot >= 32u) {
                pd->il_elems[i].InputSlot = 0u;
            }
            if (pd->il_elems[i].InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
                pd->il_elems[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                pd->il_elems[i].InstanceDataStepRate = 0u;
            }
        }
        gpso.InputLayout = {pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
    } else {
        gpso.InputLayout = {nullptr, 0u};
    }
    gpso.DSVFormat = SanitizePsoDsvFormat(gpso.DSVFormat);
    if (gpso.DSVFormat == DXGI_FORMAT_UNKNOWN) {
        gpso.DepthStencilState.DepthEnable = FALSE;
        gpso.DepthStencilState.StencilEnable = FALSE;
        gpso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    if (gpso.NumRenderTargets > 8u) {
        gpso.NumRenderTargets = 8u;
    }
    for (UINT r = 0u; r < gpso.NumRenderTargets; ++r) {
        gpso.RTVFormats[r] = SanitizeXboxDxgiFormat(gpso.RTVFormats[r]);
        if (gpso.RTVFormats[r] == DXGI_FORMAT_UNKNOWN && r == 0u && !ps_dxil.empty()) {
            gpso.RTVFormats[r] = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
    for (UINT r = gpso.NumRenderTargets; r < 8u; ++r) {
        gpso.RTVFormats[r] = DXGI_FORMAT_UNKNOWN;
    }
    SanitizeGraphicsPsoDescForDesktop(gpso);

    ID3D12PipelineState* lifted_pso = nullptr;
    HRESULT hr = host_dev_->CreateGraphicsPipelineState(
        &gpso, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&lifted_pso));
    if (FAILED(hr) && gpso.NumRenderTargets == 0u && gpso.DSVFormat == DXGI_FORMAT_UNKNOWN && !ps_dxil.empty()) {
        // Stream PSO omitted RENDER_TARGET_FORMATS subobject (Xbox dynamic RTV binding): default RT0 to RGBA8.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC retry_gpso = gpso;
        retry_gpso.NumRenderTargets = 1u;
        retry_gpso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = host_dev_->CreateGraphicsPipelineState(
            &retry_gpso, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&lifted_pso));
        if (SUCCEEDED(hr)) {
            gpso = retry_gpso;
        }
    }
    if (FAILED(hr)) {
        // Fallback sanitization pass on derived/stream PSOs whose raw Xbox override left incompatible state.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC safe_gpso = gpso;
        safe_gpso.SampleDesc = {1u, 0u};
        safe_gpso.SampleMask = UINT_MAX;
        safe_gpso.RasterizerState.ForcedSampleCount = 0u;
        safe_gpso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        safe_gpso.RasterizerState.MultisampleEnable = FALSE;
        safe_gpso.RasterizerState.AntialiasedLineEnable = FALSE;
        safe_gpso.BlendState.AlphaToCoverageEnable = FALSE;
        safe_gpso.BlendState.IndependentBlendEnable = FALSE;
        for (UINT r = 1u; r < 8u; ++r) {
            safe_gpso.BlendState.RenderTarget[r] = D3D12_RENDER_TARGET_BLEND_DESC{
                FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, 0x0Fu};
        }
        hr = host_dev_->CreateGraphicsPipelineState(
            &safe_gpso, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&lifted_pso));
        if (SUCCEEDED(hr)) {
            gpso = safe_gpso;
        }
    }
    if (SUCCEEDED(hr) && lifted_pso) {
        RecordPsoVertexLayout(
            lifted_pso, gpso.InputLayout.pInputElementDescs, gpso.InputLayout.NumElements);
    }
    if (FAILED(hr)) {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(host_dev_->QueryInterface(OS_D3D12_IID(ID3D12InfoQueue), reinterpret_cast<void**>(&iq))) && iq) {
            const UINT64 n = iq->GetNumStoredMessages();
            const UINT64 start = (n > 8u) ? (n - 8u) : 0u;
            for (UINT64 i = start; i < n; ++i) {
                SIZE_T len = 0u;
                iq->GetMessage(i, nullptr, &len);
                if (len > 0u) {
                    std::vector<uint8_t> buf(len);
                    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription) {
                        std::fprintf(stderr, "[OpenScarlett][D3D12DBG] %s\n", m->pDescription);
                    }
                }
            }
            iq->ClearStoredMessages();
            iq->Release();
            std::fflush(stderr);
        }
    }
    const size_t vs_dxil_sz = vs_dxil.size();
    const size_t ps_dxil_sz = ps_dxil.size();
    if (SUCCEEDED(hr) && lifted_pso) {
        if (pd->vs_blob.size() == 352u && pd->ps_blob.size() == 384u) {
            openscarlett::d3d12_xs::g_bink_host_pso.store(lifted_pso, std::memory_order_release);
        }
        if (pd->vs_blob.size() == 1436u && pd->ps_blob.size() == 1756u) {
            openscarlett::d3d12_xs::g_calib_pso_ready.store(true, std::memory_order_release);
        }
        pd->lifted_gpso = gpso;
        pd->lifted_vs_dxil = vs_dxil;
        if (pd->il_elems.empty() && !pd->vs_blob.empty()) {
            std::string r_err;
            (void)lift_stage(pd->vs_blob, pd->lifted_rectlist_vs_dxil, r_err,
                             /*is_vs_stage=*/true, /*rectlist_zero_ia=*/true);
        }
        pd->lifted_ps_dxil = ps_dxil;
        pd->lifted_hs_dxil = hs_dxil;
        pd->lifted_ds_dxil = ds_dxil;
        pd->lifted_gs_dxil = gs_dxil;
        pd->lifted_gpso.VS = {pd->lifted_vs_dxil.data(), pd->lifted_vs_dxil.size()};
        pd->lifted_gpso.PS = pd->lifted_ps_dxil.empty()
                                 ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                                 : D3D12_SHADER_BYTECODE{pd->lifted_ps_dxil.data(), pd->lifted_ps_dxil.size()};
        pd->lifted_gpso.HS = pd->lifted_hs_dxil.empty()
                                 ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                                 : D3D12_SHADER_BYTECODE{pd->lifted_hs_dxil.data(), pd->lifted_hs_dxil.size()};
        pd->lifted_gpso.DS = pd->lifted_ds_dxil.empty()
                                 ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                                 : D3D12_SHADER_BYTECODE{pd->lifted_ds_dxil.data(), pd->lifted_ds_dxil.size()};
        pd->lifted_gpso.GS = pd->lifted_gs_dxil.empty()
                                 ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                                 : D3D12_SHADER_BYTECODE{pd->lifted_gs_dxil.data(), pd->lifted_gs_dxil.size()};
        pd->has_lifted_gfx_dxil = true;
        proxy->AdoptLiftedHostPso(lifted_pso);
        lifted_pso->Release();
    }
    if (seq < 24u) {
        std::fprintf(stderr,
                     "[OpenScarlett][d3d12_xs] LAZY-LIFT #%u pso=%p rs=%p (raw vs=%zuB ps=%zuB -> dxil vs=%zuB ps=%zuB rts=%u fmt0=%d dsv=%d il=%u cull=%d depth=%d/%d stencil=%d blend0=%d/%d/%d wm=0x%x) -> hr=0x%08lX",
                     seq, static_cast<void*>(proxy), static_cast<void*>(pd->host_rs),
                     pd->vs_blob.size(), pd->ps_blob.size(),
                     vs_dxil_sz, ps_dxil_sz,
                     gpso.NumRenderTargets,
                     gpso.NumRenderTargets > 0u ? static_cast<int>(gpso.RTVFormats[0]) : 0,
                     static_cast<int>(gpso.DSVFormat), gpso.InputLayout.NumElements,
                     static_cast<int>(gpso.RasterizerState.CullMode),
                     gpso.DepthStencilState.DepthEnable ? 1 : 0,
                     static_cast<int>(gpso.DepthStencilState.DepthFunc),
                     gpso.DepthStencilState.StencilEnable ? 1 : 0,
                     gpso.BlendState.RenderTarget[0].BlendEnable ? 1 : 0,
                     static_cast<int>(gpso.BlendState.RenderTarget[0].SrcBlend),
                     static_cast<int>(gpso.BlendState.RenderTarget[0].DestBlend),
                     static_cast<unsigned>(gpso.BlendState.RenderTarget[0].RenderTargetWriteMask),
                     static_cast<unsigned long>(hr));
        for (UINT ei = 0u; ei < gpso.InputLayout.NumElements && gpso.InputLayout.pInputElementDescs; ++ei) {
            const auto& ie = gpso.InputLayout.pInputElementDescs[ei];
            std::fprintf(stderr, " [IL%u:%s%u fmt=%d slot=%u off=%u]", ei,
                         ie.SemanticName ? ie.SemanticName : "?", ie.SemanticIndex,
                         static_cast<int>(ie.Format), ie.InputSlot, ie.AlignedByteOffset);
        }
        if (FAILED(hr)) {
            const std::string rs_desc = DescribeDiagRootSig(pd->host_rs);
            if (!rs_desc.empty()) {
                std::fprintf(stderr, " RS=%s", rs_desc.c_str());
            }
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        char dump_dir[260]{};
        if (::GetEnvironmentVariableA("OPENSCARLETT_DUMP_BOUND_PSO", dump_dir, sizeof(dump_dir)) > 0) {
            auto dump_vec = [&](const char* suffix, const std::vector<uint8_t>& v) {
                if (v.empty()) return;
                char fp[320]{};
                std::snprintf(fp, sizeof(fp), "%s\\bound_%02u_%s.bin", dump_dir, seq, suffix);
                FILE* f = nullptr;
                if (::fopen_s(&f, fp, "wb") == 0 && f) {
                    std::fwrite(v.data(), 1u, v.size(), f);
                    std::fclose(f);
                }
            };
            dump_vec("vs", pd->vs_blob);
            dump_vec("ps", pd->ps_blob);
        }
    }
}

ID3D12PipelineState* DeviceX::CreateGraphicsPsoTargetVariant(
    PipelineStateXProxy* proxy,
    UINT num_rtvs,
    const DXGI_FORMAT* rtv_formats,
    DXGI_FORMAT dsv_format) {
    if (!proxy || !host_dev_) return nullptr;
    PendingXdatGfxDesc* pd = proxy->PendingXdatDesc();
    if (!pd || !pd->has_lifted_gfx_dxil || !pd->host_rs ||
        (pd->is_mesh ? pd->lifted_ms_dxil.empty() : pd->lifted_vs_dxil.empty())) {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC vdesc = pd->lifted_gpso;
    vdesc.pRootSignature = pd->host_rs;
    if (pd->is_mesh) {
        vdesc.NumRenderTargets = std::min<UINT>(num_rtvs, 8u);
        for (UINT r = 0u; r < 8u; ++r) {
            vdesc.RTVFormats[r] =
                (r < vdesc.NumRenderTargets && rtv_formats)
                    ? SanitizeXboxDxgiFormat(rtv_formats[r])
                    : DXGI_FORMAT_UNKNOWN;
        }
        vdesc.DSVFormat = SanitizePsoDsvFormat(dsv_format);
        if (vdesc.DSVFormat == DXGI_FORMAT_UNKNOWN) {
            vdesc.DepthStencilState.DepthEnable = FALSE;
            vdesc.DepthStencilState.StencilEnable = FALSE;
            vdesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        SanitizeGraphicsPsoDescForDesktop(vdesc);
        ID3D12PipelineState* var_ms_pso = nullptr;
        if (host_dev2_ &&
            SUCCEEDED(BuildHostMeshPipelineState(
                host_dev2_, pd->host_rs, pd->lifted_ms_dxil, pd->lifted_as_dxil, pd->lifted_ps_dxil,
                vdesc, &var_ms_pso)) &&
            var_ms_pso) {
            return var_ms_pso;
        }
        return nullptr;
    }
    vdesc.VS = {pd->lifted_vs_dxil.data(), pd->lifted_vs_dxil.size()};
    vdesc.PS = pd->lifted_ps_dxil.empty()
                   ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                   : D3D12_SHADER_BYTECODE{pd->lifted_ps_dxil.data(), pd->lifted_ps_dxil.size()};
    vdesc.HS = pd->lifted_hs_dxil.empty()
                   ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                   : D3D12_SHADER_BYTECODE{pd->lifted_hs_dxil.data(), pd->lifted_hs_dxil.size()};
    vdesc.DS = pd->lifted_ds_dxil.empty()
                   ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                   : D3D12_SHADER_BYTECODE{pd->lifted_ds_dxil.data(), pd->lifted_ds_dxil.size()};
    vdesc.GS = pd->lifted_gs_dxil.empty()
                   ? D3D12_SHADER_BYTECODE{nullptr, 0u}
                   : D3D12_SHADER_BYTECODE{pd->lifted_gs_dxil.data(), pd->lifted_gs_dxil.size()};
    if (!pd->il_elems.empty()) {
        for (size_t i = 0u; i < pd->il_elems.size() && i < pd->il_names.size(); ++i) {
            pd->il_elems[i].SemanticName = pd->il_names[i].c_str();
        }
        vdesc.InputLayout = {pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
    } else {
        vdesc.InputLayout = {nullptr, 0u};
    }

    vdesc.NumRenderTargets = std::min<UINT>(num_rtvs, 8u);
    for (UINT r = 0u; r < 8u; ++r) {
        vdesc.RTVFormats[r] =
            (r < vdesc.NumRenderTargets && rtv_formats)
                ? SanitizeXboxDxgiFormat(rtv_formats[r])
                : DXGI_FORMAT_UNKNOWN;
    }
    vdesc.DSVFormat = SanitizePsoDsvFormat(dsv_format);
    if (vdesc.DSVFormat == DXGI_FORMAT_UNKNOWN) {
        vdesc.DepthStencilState.DepthEnable = FALSE;
        vdesc.DepthStencilState.StencilEnable = FALSE;
        vdesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    SanitizeGraphicsPsoDescForDesktop(vdesc);

    ID3D12PipelineState* var_pso = nullptr;
    HRESULT hr = host_dev_->CreateGraphicsPipelineState(
        &vdesc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&var_pso));
    if (SUCCEEDED(hr) && var_pso) {
        RecordPsoVertexLayout(
            var_pso, vdesc.InputLayout.pInputElementDescs, vdesc.InputLayout.NumElements);
        if (pd->vs_blob.size() == 352u && pd->ps_blob.size() == 384u) {
            openscarlett::d3d12_xs::g_bink_host_pso.store(var_pso, std::memory_order_release);
        }
    }
    return var_pso;
}

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

    // Pass 1: Scan stream, decode Xbox Series subobject tags (AS=23, MS=24, ext 25..30 -> Desktop tags),
    // detect mesh pipelines, Xbox shaders, SPIR-V, wrapped root sigs, and VS/PS containers.
    const auto* raw_stream = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
    const size_t total_bytes = static_cast<size_t>(pDesc->SizeInBytes);
    constexpr size_t kBytecodeOffset = offsetof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>, value);
    constexpr size_t kRootSigOffset = offsetof(StreamSubobjLayout<ID3D12RootSignature*>, value);

    struct DecodedStreamEntry {
        size_t src_offset = 0u;
        size_t src_stride = 0u;
        uint32_t raw_tag = 0u;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        bool drop_subobject = false;
        bool remapped_from_xbox = false;
        bool convert_ds2_to_ds1 = false;
        bool convert_rast_ext_to_rast0 = false;
    };

    auto looks_like_mesh_shader_blob = [](const D3D12_SHADER_BYTECODE& bc) noexcept -> bool {
        if (!bc.pShaderBytecode || bc.BytecodeLength < 16u) return false;
        if (openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
            openscarlett::XboxParsedShaderContainer tmp{};
            if (openscarlett::ParseFullXboxShaderContainer(bc.pShaderBytecode, bc.BytecodeLength, tmp).ok() &&
                tmp.has_xs2 && tmp.xs2.stage == openscarlett::ShaderStage::Mesh) {
                return true;
            }
        }
        uint8_t psv_stage = 0xFFu;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        if (ExtractDxbcPsv0Info(bc.pShaderBytecode, bc.BytecodeLength, psv_stage, topo) &&
            psv_stage == 13u) {
            return true;
        }
        return false;
    };

    bool stream_uses_xbox_tags = retail_scarlett_abi_;
    bool needs_stream_rebuild = false;
    bool has_xbox_shader = false;
    bool has_spirv_shader = false;
    bool has_wrapped_root_sig = false;
    bool has_root_sig_subobj = false;
    bool root_sig_is_null = false;
    bool has_as_stage = false;
    bool has_ms_stage = false;
    bool has_prim_topo_subobj = false;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE stream_prim_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE psv0_mesh_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    uint32_t shader_stage_count = 0u;
    VulkanPipelineInfo spirv_stream_pi{};
    openscarlett::XboxParsedShaderContainer stream_vs_parsed{}, stream_ps_parsed{};
    bool has_stream_vs_parsed = false;
    bool has_stream_ps_parsed = false;

    std::vector<DecodedStreamEntry> entries;
    entries.reserve(16u);

    size_t pos = 0u;
    while (pos + sizeof(uint32_t) <= total_bytes) {
        uint32_t raw_tag = 0u;
        std::memcpy(&raw_tag, raw_stream + pos, sizeof(raw_tag));

        DecodedStreamEntry ent{};
        ent.src_offset = pos;
        ent.raw_tag = raw_tag;

        if (raw_tag == 23u) {
            // Xbox Series AS subobject tag (23) -> Desktop D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS (24).
            stream_uses_xbox_tags = true;
            ent.desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            ent.src_stride = sizeof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>);
            ent.remapped_from_xbox = true;
            needs_stream_rebuild = true;
            pso_remapped_as_tags_.fetch_add(1ull, std::memory_order_relaxed);
        } else if (raw_tag == 24u) {
            ent.src_stride = sizeof(StreamSubobjLayout<D3D12_SHADER_BYTECODE>);
            bool is_xbox_ms = stream_uses_xbox_tags;
            if (!is_xbox_ms && pos + ent.src_stride <= total_bytes) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, raw_stream + pos + kBytecodeOffset, sizeof(bc));
                if (looks_like_mesh_shader_blob(bc)) {
                    is_xbox_ms = true;
                    stream_uses_xbox_tags = true;
                }
            }
            if (is_xbox_ms) {
                ent.desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
                ent.remapped_from_xbox = true;
                needs_stream_rebuild = true;
                pso_remapped_ms_tags_.fetch_add(1ull, std::memory_order_relaxed);
            } else {
                ent.desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            }
        } else if (raw_tag >= 25u && raw_tag <= 30u && stream_uses_xbox_tags) {
            // Xbox Series extension subobjects 25..30 (xbox-emulator #168):
            //  25 = Xbox DEPTH_STENCIL2 -> remap to Desktop DEPTH_STENCIL1 (safe on Win10 & Win11)
            //  26 = Xbox RASTERIZER1 / 27 = Xbox RASTERIZER2 -> remap/down-convert to Desktop RASTERIZER
            //  28..30 = Xbox-only extension flags (UINT payload) -> strip cleanly on Desktop D3D12.
            ent.remapped_from_xbox = true;
            needs_stream_rebuild = true;
            pso_remapped_ext_tags_.fetch_add(1ull, std::memory_order_relaxed);
            if (raw_tag == 25u) {
#if defined(__ID3D12Device10_INTERFACE_DEFINED__) && !defined(__MINGW32__)
                const size_t ds2_stride = sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC2>);
                if (pos + ds2_stride <= total_bytes) {
                    ent.desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
                    ent.src_stride = ds2_stride;
                    ent.convert_ds2_to_ds1 = true;
                } else
#endif
                {
                    ent.src_stride = sizeof(StreamSubobjLayout<UINT>);
                    ent.drop_subobject = true;
                }
            } else if (raw_tag == 26u || raw_tag == 27u) {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                const size_t r_stride = (raw_tag == 26u)
                                            ? sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC1>)
                                            : sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC2>);
                if (pos + r_stride <= total_bytes) {
                    ent.desktop_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
                    ent.src_stride = r_stride;
                    ent.convert_rast_ext_to_rast0 = true;
                } else
#endif
                {
                    ent.src_stride = sizeof(StreamSubobjLayout<UINT>);
                    ent.drop_subobject = true;
                }
            } else {
                // Xbox-only extension subobjects 28..30 (e.g. Xbox pipeline flags/modes): drop safely.
                ent.src_stride = sizeof(StreamSubobjLayout<UINT>);
                ent.drop_subobject = true;
            }
        } else if (raw_tag > 28u && raw_tag <= 30u) {
            // Standalone Xbox extension subobjects 29..30 even without AS/MS tags.
            ent.remapped_from_xbox = true;
            ent.drop_subobject = true;
            ent.src_stride = sizeof(StreamSubobjLayout<UINT>);
            needs_stream_rebuild = true;
            pso_remapped_ext_tags_.fetch_add(1ull, std::memory_order_relaxed);
        } else {
            ent.desktop_type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(raw_tag);
            ent.src_stride = GetSubobjectStrideBytes(ent.desktop_type);
        }

        if (ent.src_stride == 0u || pos + ent.src_stride > total_bytes) {
            break;
        }

        const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE sub_type = ent.desktop_type;
        if (!ent.drop_subobject) {
            if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                has_root_sig_subobj = true;
                ID3D12RootSignature* rs = nullptr;
                std::memcpy(&rs, raw_stream + pos + kRootSigOffset, sizeof(rs));
                if (!rs) {
                    root_sig_is_null = true;
                } else if (UnwrapRootSignature(rs) != rs) {
                    has_wrapped_root_sig = true;
                }
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
                has_prim_topo_subobj = true;
                std::memcpy(&stream_prim_topo,
                            raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>, value),
                            sizeof(stream_prim_topo));
                if (stream_prim_topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
                    static_cast<uint32_t>(stream_prim_topo) >
                        static_cast<uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)) {
                    // Remap Xbox RECTLIST (0x13 = 19) / QUADLIST (0x11 = 17) / UNDEFINED -> TRIANGLE (3).
                    needs_stream_rebuild = true;
                }
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) {
                D3D12_DEPTH_STENCIL_DESC ds_probe{};
                std::memcpy(&ds_probe,
                            raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>, value),
                            sizeof(ds_probe));
                if (SanitizeDepthStencilDesc(ds_probe)) {
                    needs_stream_rebuild = true;
                }
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) {
                D3D12_DEPTH_STENCIL_DESC1 ds1_probe{};
                std::memcpy(&ds1_probe,
                            raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>, value),
                            sizeof(ds1_probe));
                if (SanitizeDepthStencilDesc1(ds1_probe)) {
                    needs_stream_rebuild = true;
                }
            } else if (IsShaderSubobjectType(sub_type)) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, raw_stream + pos + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                    ++shader_stage_count;
                    if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                        has_as_stage = true;
                    } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                        has_ms_stage = true;
                        uint8_t psv_st = 0xFFu;
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
                        if (ExtractDxbcPsv0Info(bc.pShaderBytecode, bc.BytecodeLength, psv_st, topo) &&
                            topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
                            psv0_mesh_topo = topo;
                        }
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
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
                D3D12_RT_FORMAT_ARRAY rts{};
                std::memcpy(&rts, raw_stream + pos + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value), sizeof(rts));
                if (rts.NumRenderTargets > 8u) {
                    needs_stream_rebuild = true;
                } else {
                    for (UINT ri = 0u; ri < 8u; ++ri) {
                        const DXGI_FORMAT san =
                            (ri < rts.NumRenderTargets) ? SanitizeXboxDxgiFormat(rts.RTFormats[ri]) : DXGI_FORMAT_UNKNOWN;
                        if (san != rts.RTFormats[ri]) {
                            needs_stream_rebuild = true;
                            break;
                        }
                    }
                }
                if (vulkan_routing_active_ && rts.NumRenderTargets > 0u) {
                    spirv_stream_pi.color_format = DxgiToVulkanColorFormat(rts.RTFormats[0]);
                }
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
                DXGI_FORMAT dsf = DXGI_FORMAT_UNKNOWN;
                std::memcpy(&dsf, raw_stream + pos + offsetof(StreamSubobjLayout<DXGI_FORMAT>, value), sizeof(dsf));
                if (SanitizePsoDsvFormat(dsf) != dsf) {
                    needs_stream_rebuild = true;
                }
            } else if (sub_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC) {
                DXGI_SAMPLE_DESC sd_probe{};
                std::memcpy(&sd_probe,
                            raw_stream + pos + offsetof(StreamSubobjLayout<DXGI_SAMPLE_DESC>, value),
                            sizeof(sd_probe));
                if (sd_probe.Count == 0u || sd_probe.Count > 1u) {
                    needs_stream_rebuild = true;
                }
            }
        }

        entries.push_back(ent);
        pos += ent.src_stride;
    }

    // Mesh shader PSO hardening (xbox-emulator #176): drop empty legacy VS/HS/DS/GS stages
    // (and empty INPUT_LAYOUT / IB_STRIP_CUT_VALUE) that Desktop D3D12 rejects on Mesh PSOs.
    if (has_ms_stage) {
        for (auto& ent : entries) {
            if (ent.drop_subobject) continue;
            if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ||
                ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS ||
                ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                if (!bc.pShaderBytecode || bc.BytecodeLength == 0u) {
                    ent.drop_subobject = true;
                    needs_stream_rebuild = true;
                    pso_dropped_legacy_stages_.fetch_add(1ull, std::memory_order_relaxed);
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
                D3D12_INPUT_LAYOUT_DESC il{};
                std::memcpy(&il, raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_INPUT_LAYOUT_DESC>, value), sizeof(il));
                if (il.NumElements == 0u || !il.pInputElementDescs) {
                    ent.drop_subobject = true;
                    needs_stream_rebuild = true;
                    pso_dropped_legacy_stages_.fetch_add(1ull, std::memory_order_relaxed);
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE) {
                ent.drop_subobject = true;
                needs_stream_rebuild = true;
                pso_dropped_legacy_stages_.fetch_add(1ull, std::memory_order_relaxed);
            }
        }
        if (!has_prim_topo_subobj || stream_prim_topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
            needs_stream_rebuild = true;
        }
    }

    // Wire f16 half linkage + ValidateAndMatchStageInterpolation into CreatePipelineState!
    if (has_stream_vs_parsed && has_stream_ps_parsed) {
        const uint32_t f16_links = LinkFp16HalfStageSignatures(stream_vs_parsed, stream_ps_parsed);
        if (f16_links > 0u) {
            pso_f16_links_reconciled_.fetch_add(f16_links, std::memory_order_relaxed);
        }
        openscarlett::XboxPsoInterpLinkageReport interp_report{};
        (void)openscarlett::ValidateAndMatchStageInterpolation(stream_vs_parsed, stream_ps_parsed, interp_report);
        pso_interp_validations_.fetch_add(1ull, std::memory_order_relaxed);
        for (const auto& ie : interp_report.entries) {
            if (ie.matched) {
                pso_interp_matches_.fetch_add(1ull, std::memory_order_relaxed);
            }
        }
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
    std::vector<uint64_t> rebuilt_words;
    std::deque<std::vector<uint8_t>> lifted_blobs;

    if (has_xbox_shader || has_wrapped_root_sig || needs_stream_rebuild) {
        // Pass 2: Rebuild a clean pointer-aligned Desktop D3D12 subobject stream.
        std::vector<uint8_t> rebuilt_bytes;
        rebuilt_bytes.reserve(total_bytes + 128u);

        auto append_aligned = [&](const void* src, size_t len) -> size_t {
            const size_t aligned = (rebuilt_bytes.size() + sizeof(void*) - 1u) & ~(sizeof(void*) - 1u);
            if (aligned > rebuilt_bytes.size()) {
                rebuilt_bytes.resize(aligned, 0u);
            }
            const size_t start = rebuilt_bytes.size();
            rebuilt_bytes.resize(start + len, 0u);
            if (src && len > 0u) {
                std::memcpy(rebuilt_bytes.data() + start, src, len);
            }
            return start;
        };

        const uint32_t highest_sm = adapter_info_.highest_shader_model ? adapter_info_.highest_shader_model : 0x60u;
        const bool prefer_dxil_standin = (shader_stage_count > 1u && highest_sm >= 0x60u);

        openscarlett::XboxRootSignatureDesc fallback_rs_desc{};
        bool has_fallback_rs = false;
        size_t rebuilt_rs_pos = SIZE_MAX;
        size_t rebuilt_topo_pos = SIZE_MAX;

        for (const auto& ent : entries) {
            if (ent.drop_subobject) continue;

            if (ent.convert_ds2_to_ds1) {
#if defined(__ID3D12Device10_INTERFACE_DEFINED__) && !defined(__MINGW32__)
                StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC2> ds2{};
                std::memcpy(&ds2, raw_stream + ent.src_offset, sizeof(ds2));
                StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1> ds1{};
                ds1.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
                ds1.value.DepthEnable = ds2.value.DepthEnable;
                ds1.value.DepthWriteMask = ds2.value.DepthWriteMask;
                ds1.value.DepthFunc = ds2.value.DepthFunc;
                ds1.value.StencilEnable = ds2.value.StencilEnable;
                ds1.value.StencilReadMask = ds2.value.FrontFace.StencilReadMask;
                ds1.value.StencilWriteMask = ds2.value.FrontFace.StencilWriteMask;
                ds1.value.FrontFace.StencilFailOp = ds2.value.FrontFace.StencilFailOp;
                ds1.value.FrontFace.StencilDepthFailOp = ds2.value.FrontFace.StencilDepthFailOp;
                ds1.value.FrontFace.StencilPassOp = ds2.value.FrontFace.StencilPassOp;
                ds1.value.FrontFace.StencilFunc = ds2.value.FrontFace.StencilFunc;
                ds1.value.BackFace.StencilFailOp = ds2.value.BackFace.StencilFailOp;
                ds1.value.BackFace.StencilDepthFailOp = ds2.value.BackFace.StencilDepthFailOp;
                ds1.value.BackFace.StencilPassOp = ds2.value.BackFace.StencilPassOp;
                ds1.value.BackFace.StencilFunc = ds2.value.BackFace.StencilFunc;
                ds1.value.DepthBoundsTestEnable = ds2.value.DepthBoundsTestEnable;
                (void)append_aligned(&ds1, sizeof(ds1));
#endif
                continue;
            }
            if (ent.convert_rast_ext_to_rast0) {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                StreamSubobjLayout<D3D12_RASTERIZER_DESC> r0{};
                r0.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
                if (ent.raw_tag == 26u) {
                    StreamSubobjLayout<D3D12_RASTERIZER_DESC1> r1{};
                    std::memcpy(&r1, raw_stream + ent.src_offset, sizeof(r1));
                    r0.value.FillMode = r1.value.FillMode;
                    r0.value.CullMode = r1.value.CullMode;
                    r0.value.FrontCounterClockwise = r1.value.FrontCounterClockwise;
                    r0.value.DepthBias = static_cast<INT>(r1.value.DepthBias);
                    r0.value.DepthBiasClamp = r1.value.DepthBiasClamp;
                    r0.value.SlopeScaledDepthBias = r1.value.SlopeScaledDepthBias;
                    r0.value.DepthClipEnable = r1.value.DepthClipEnable;
                    r0.value.MultisampleEnable = r1.value.MultisampleEnable;
                    r0.value.AntialiasedLineEnable = r1.value.AntialiasedLineEnable;
                    r0.value.ForcedSampleCount = r1.value.ForcedSampleCount;
                    r0.value.ConservativeRaster = r1.value.ConservativeRaster;
                } else {
                    StreamSubobjLayout<D3D12_RASTERIZER_DESC2> r2{};
                    std::memcpy(&r2, raw_stream + ent.src_offset, sizeof(r2));
                    r0.value.FillMode = r2.value.FillMode;
                    r0.value.CullMode = r2.value.CullMode;
                    r0.value.FrontCounterClockwise = r2.value.FrontCounterClockwise;
                    r0.value.DepthBias = static_cast<INT>(r2.value.DepthBias);
                    r0.value.DepthBiasClamp = r2.value.DepthBiasClamp;
                    r0.value.SlopeScaledDepthBias = r2.value.SlopeScaledDepthBias;
                    r0.value.DepthClipEnable = r2.value.DepthClipEnable;
                    r0.value.MultisampleEnable = (r2.value.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE ||
                                                  r2.value.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW)
                                                     ? TRUE
                                                     : FALSE;
                    r0.value.AntialiasedLineEnable =
                        (r2.value.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED) ? TRUE : FALSE;
                    r0.value.ForcedSampleCount = r2.value.ForcedSampleCount;
                    r0.value.ConservativeRaster = r2.value.ConservativeRaster;
                }
                (void)append_aligned(&r0, sizeof(r0));
#endif
                continue;
            }

            const size_t dst_pos = append_aligned(raw_stream + ent.src_offset, ent.src_stride);
            // Patch desktop subobject type tag (e.g. Xbox AS=23->24, MS=24->25).
            std::memcpy(rebuilt_bytes.data() + dst_pos, &ent.desktop_type, sizeof(ent.desktop_type));

            if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                rebuilt_rs_pos = dst_pos;
                ID3D12RootSignature* rs = nullptr;
                std::memcpy(&rs, rebuilt_bytes.data() + dst_pos + kRootSigOffset, sizeof(rs));
                ID3D12RootSignature* unwrapped_rs = UnwrapRootSignature(rs);
                if (unwrapped_rs != rs) {
                    std::memcpy(rebuilt_bytes.data() + dst_pos + kRootSigOffset, &unwrapped_rs, sizeof(unwrapped_rs));
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
                rebuilt_topo_pos = dst_pos;
                if (stream_prim_topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
                    static_cast<uint32_t>(stream_prim_topo) >
                        static_cast<uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)) {
                    const D3D12_PRIMITIVE_TOPOLOGY_TYPE tri_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    std::memcpy(
                        rebuilt_bytes.data() + dst_pos +
                            offsetof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>, value),
                        &tri_topo, sizeof(tri_topo));
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
                D3D12_RT_FORMAT_ARRAY rts{};
                const size_t rt_off = offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value);
                std::memcpy(&rts, rebuilt_bytes.data() + dst_pos + rt_off, sizeof(rts));
                rts.NumRenderTargets = std::min<UINT>(rts.NumRenderTargets, 8u);
                for (UINT ri = 0u; ri < 8u; ++ri) {
                    rts.RTFormats[ri] =
                        (ri < rts.NumRenderTargets) ? SanitizeXboxDxgiFormat(rts.RTFormats[ri]) : DXGI_FORMAT_UNKNOWN;
                }
                std::memcpy(rebuilt_bytes.data() + dst_pos + rt_off, &rts, sizeof(rts));
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
                DXGI_FORMAT dsf = DXGI_FORMAT_UNKNOWN;
                const size_t df_off = offsetof(StreamSubobjLayout<DXGI_FORMAT>, value);
                std::memcpy(&dsf, rebuilt_bytes.data() + dst_pos + df_off, sizeof(dsf));
                dsf = SanitizePsoDsvFormat(dsf);
                std::memcpy(rebuilt_bytes.data() + dst_pos + df_off, &dsf, sizeof(dsf));
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) {
                D3D12_DEPTH_STENCIL_DESC ds_val{};
                const size_t ds_off = offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>, value);
                std::memcpy(&ds_val, rebuilt_bytes.data() + dst_pos + ds_off, sizeof(ds_val));
                if (SanitizeDepthStencilDesc(ds_val)) {
                    std::memcpy(rebuilt_bytes.data() + dst_pos + ds_off, &ds_val, sizeof(ds_val));
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) {
                D3D12_DEPTH_STENCIL_DESC1 ds1_val{};
                const size_t ds1_off = offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>, value);
                std::memcpy(&ds1_val, rebuilt_bytes.data() + dst_pos + ds1_off, sizeof(ds1_val));
                if (SanitizeDepthStencilDesc1(ds1_val)) {
                    std::memcpy(rebuilt_bytes.data() + dst_pos + ds1_off, &ds1_val, sizeof(ds1_val));
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC) {
                DXGI_SAMPLE_DESC sd_val{};
                const size_t sd_off = offsetof(StreamSubobjLayout<DXGI_SAMPLE_DESC>, value);
                std::memcpy(&sd_val, rebuilt_bytes.data() + dst_pos + sd_off, sizeof(sd_val));
                DXGI_FORMAT msaa_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                for (const auto& fmt_ent : entries) {
                    if (fmt_ent.drop_subobject) continue;
                    if (fmt_ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
                        D3D12_RT_FORMAT_ARRAY rts{};
                        std::memcpy(&rts, raw_stream + fmt_ent.src_offset + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value), sizeof(rts));
                        if (rts.NumRenderTargets > 0u && rts.RTFormats[0] != DXGI_FORMAT_UNKNOWN) {
                            msaa_fmt = rts.RTFormats[0];
                            break;
                        }
                    } else if (fmt_ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
                        DXGI_FORMAT dsf = DXGI_FORMAT_UNKNOWN;
                        std::memcpy(&dsf, raw_stream + fmt_ent.src_offset + offsetof(StreamSubobjLayout<DXGI_FORMAT>, value), sizeof(dsf));
                        if (dsf != DXGI_FORMAT_UNKNOWN) {
                            msaa_fmt = dsf;
                        }
                    }
                }
                if (ClampMsaaSampleDescForFormat(msaa_fmt, sd_val)) {
                    std::memcpy(rebuilt_bytes.data() + dst_pos + sd_off, &sd_val, sizeof(sd_val));
                }
            } else if (has_xbox_shader && IsShaderSubobjectType(ent.desktop_type)) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, rebuilt_bytes.data() + dst_pos + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0u &&
                    openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                    const openscarlett::XboxParsedShaderContainer* paired = nullptr;
                    if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && has_stream_ps_parsed) {
                        paired = &stream_ps_parsed;
                    } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS && has_stream_vs_parsed) {
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
                    if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS &&
                        psv0_mesh_topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
                        uint8_t lifted_stage = 0xFFu;
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE lifted_topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
                        if (ExtractDxbcPsv0Info(host_bc.data(), host_bc.size(), lifted_stage, lifted_topo) &&
                            lifted_topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
                            psv0_mesh_topo = lifted_topo;
                        }
                    }
                    if (!has_fallback_rs ||
                        ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ||
                        ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
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
                    std::memcpy(rebuilt_bytes.data() + dst_pos + kBytecodeOffset, &bc, sizeof(bc));
                }
            }
        }

        // Match mesh PSO primitive topology from PSV0 metadata when missing/unspecified (xbox-emulator #176).
        if (has_ms_stage &&
            (!has_prim_topo_subobj || stream_prim_topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)) {
            const D3D12_PRIMITIVE_TOPOLOGY_TYPE resolved_topo =
                (psv0_mesh_topo != D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED)
                    ? psv0_mesh_topo
                    : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            if (rebuilt_topo_pos != SIZE_MAX) {
                std::memcpy(
                    rebuilt_bytes.data() + rebuilt_topo_pos +
                        offsetof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>, value),
                    &resolved_topo,
                    sizeof(resolved_topo));
            } else {
                StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE> topo_sub{};
                topo_sub.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
                topo_sub.value = resolved_topo;
                (void)append_aligned(&topo_sub, sizeof(topo_sub));
            }
            pso_inferred_mesh_topologies_.fetch_add(1ull, std::memory_order_relaxed);
        }

        // If stream omitted ROOT_SIGNATURE (or passed nullptr) alongside an Xbox container
        // without RTS0, synthesize & inject a host root signature from inferred bindings!
        if ((!has_root_sig_subobj || root_sig_is_null) && has_fallback_rs) {
            ID3D12RootSignature* synth_rs = SynthesizeHostRootSignatureFromXboxDesc(fallback_rs_desc);
            if (synth_rs) {
                if (rebuilt_rs_pos != SIZE_MAX && root_sig_is_null) {
                    std::memcpy(rebuilt_bytes.data() + rebuilt_rs_pos + kRootSigOffset, &synth_rs, sizeof(synth_rs));
                } else {
                    StreamSubobjLayout<ID3D12RootSignature*> rs_sub{};
                    rs_sub.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                    rs_sub.value = synth_rs;
                    (void)append_aligned(&rs_sub, sizeof(rs_sub));
                }
            }
        }

        const size_t word_count = (rebuilt_bytes.size() + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
        rebuilt_words.assign(word_count, 0ull);
        if (!rebuilt_bytes.empty()) {
            std::memcpy(rebuilt_words.data(), rebuilt_bytes.data(), rebuilt_bytes.size());
        }
        lifted_stream.SizeInBytes = rebuilt_bytes.size();
        lifted_stream.pPipelineStateSubobjectStream = rebuilt_words.data();
        final_stream_desc = &lifted_stream;
    }

    if (!ppPipelineState) {
        const HRESULT hr = host_dev2_->CreatePipelineState(final_stream_desc, riid, nullptr);
        RecordDev("ID3D12DeviceX::CreatePipelineState", 47, hr);
        return hr;
    }
    *ppPipelineState = nullptr;
    ID3D12PipelineState* host_pso = nullptr;
    HRESULT hr = E_FAIL;
    bool stream_has_xdat = false;
    bool stream_has_xdat_cs = false;
    std::unique_ptr<PendingXdatGfxDesc> pending_xdat;
    if (retail_scarlett_abi_) {
        for (const auto& ent : entries) {
            if (ent.drop_subobject || !IsShaderSubobjectType(ent.desktop_type)) continue;
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
            if (IsXboxXdatDxbcBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                stream_has_xdat = true;
                if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                    stream_has_xdat_cs = true;
                }
            }
        }
        if (stream_has_xdat && stream_has_xdat_cs) {
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->is_compute = true;
            for (const auto& ent : entries) {
                if (ent.drop_subobject) continue;
                if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                    ID3D12RootSignature* rs = nullptr;
                    std::memcpy(&rs, raw_stream + ent.src_offset + kRootSigOffset, sizeof(rs));
                    ID3D12RootSignature* hrs = UnwrapRootSignature(rs);
                    if (hrs) {
                        hrs->AddRef();
                        if (pd->host_rs) pd->host_rs->Release();
                        pd->host_rs = hrs;
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->cs_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                }
            }
            if (!pd->cs_blob.empty() && pd->host_rs) {
                pending_xdat = std::move(pd);
            }
        } else if (stream_has_xdat && !stream_has_xdat_cs) {
            auto pd = std::make_unique<PendingXdatGfxDesc>();
            pd->gpso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd->gpso.SampleMask = UINT_MAX;
            pd->gpso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd->gpso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd->gpso.RasterizerState.DepthClipEnable = TRUE;
            pd->gpso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd->gpso.SampleDesc = {1u, 0u};
            for (const auto& ent : entries) {
                if (ent.drop_subobject) continue;
                if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                    ID3D12RootSignature* rs = nullptr;
                    std::memcpy(&rs, raw_stream + ent.src_offset + kRootSigOffset, sizeof(rs));
                    ID3D12RootSignature* hrs = UnwrapRootSignature(rs);
                    if (hrs) {
                        hrs->AddRef();
                        if (pd->host_rs) pd->host_rs->Release();
                        pd->host_rs = hrs;
                        pd->gpso.pRootSignature = hrs;
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->vs_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->ps_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->hs_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->ds_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->gs_blob.assign(bp, bp + bc.BytecodeLength);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->ms_blob.assign(bp, bp + bc.BytecodeLength);
                        pd->is_mesh = true;
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                    if (bc.pShaderBytecode && bc.BytecodeLength > 0u) {
                        const auto* bp = static_cast<const uint8_t*>(bc.pShaderBytecode);
                        pd->as_blob.assign(bp, bp + bc.BytecodeLength);
                        pd->is_mesh = true;
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_INPUT_LAYOUT_DESC>) <= total_bytes) {
                        D3D12_INPUT_LAYOUT_DESC ild{};
                        std::memcpy(&ild,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_INPUT_LAYOUT_DESC>, value),
                            sizeof(ild));
                        if (ild.pInputElementDescs && ild.NumElements > 0u && ild.NumElements <= 32u) {
                            pd->il_names.reserve(ild.NumElements);
                            pd->il_elems.assign(ild.pInputElementDescs, ild.pInputElementDescs + ild.NumElements);
                            for (auto& e : pd->il_elems) {
                                pd->il_names.emplace_back(e.SemanticName ? e.SemanticName : "");
                                e.SemanticName = pd->il_names.back().c_str();
                                e.Format = SanitizeXboxDxgiFormat(e.Format);
                            }
                            pd->gpso.InputLayout = {pd->il_elems.data(), static_cast<UINT>(pd->il_elems.size())};
                        }
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_BLEND_DESC>) <= total_bytes) {
                        std::memcpy(&pd->gpso.BlendState,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_BLEND_DESC>, value),
                            sizeof(pd->gpso.BlendState));
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER) {
                    if (!ent.convert_rast_ext_to_rast0) {
                        if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC>) <= total_bytes) {
                            std::memcpy(&pd->gpso.RasterizerState,
                                raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_RASTERIZER_DESC>, value),
                                sizeof(pd->gpso.RasterizerState));
                        }
                    } else {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                        if (ent.raw_tag == 26u &&
                            ent.src_offset + sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC1>) <= total_bytes) {
                            StreamSubobjLayout<D3D12_RASTERIZER_DESC1> r1{};
                            std::memcpy(&r1, raw_stream + ent.src_offset, sizeof(r1));
                            pd->gpso.RasterizerState.FillMode = r1.value.FillMode;
                            pd->gpso.RasterizerState.CullMode = r1.value.CullMode;
                            pd->gpso.RasterizerState.FrontCounterClockwise = r1.value.FrontCounterClockwise;
                            pd->gpso.RasterizerState.DepthBias = static_cast<INT>(r1.value.DepthBias);
                            pd->gpso.RasterizerState.DepthBiasClamp = r1.value.DepthBiasClamp;
                            pd->gpso.RasterizerState.SlopeScaledDepthBias = r1.value.SlopeScaledDepthBias;
                            pd->gpso.RasterizerState.DepthClipEnable = r1.value.DepthClipEnable;
                        } else if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_RASTERIZER_DESC2>) <= total_bytes) {
                            StreamSubobjLayout<D3D12_RASTERIZER_DESC2> r2{};
                            std::memcpy(&r2, raw_stream + ent.src_offset, sizeof(r2));
                            pd->gpso.RasterizerState.FillMode = r2.value.FillMode;
                            pd->gpso.RasterizerState.CullMode = r2.value.CullMode;
                            pd->gpso.RasterizerState.FrontCounterClockwise = r2.value.FrontCounterClockwise;
                            pd->gpso.RasterizerState.DepthBias = static_cast<INT>(r2.value.DepthBias);
                            pd->gpso.RasterizerState.DepthBiasClamp = r2.value.DepthBiasClamp;
                            pd->gpso.RasterizerState.SlopeScaledDepthBias = r2.value.SlopeScaledDepthBias;
                            pd->gpso.RasterizerState.DepthClipEnable = r2.value.DepthClipEnable;
                        }
#endif
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>) <= total_bytes) {
                        std::memcpy(&pd->gpso.DepthStencilState,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC>, value),
                            sizeof(pd->gpso.DepthStencilState));
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) {
                    if (!ent.convert_ds2_to_ds1 &&
                        ent.src_offset + sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>) <= total_bytes) {
                        D3D12_DEPTH_STENCIL_DESC1 ds1{};
                        std::memcpy(&ds1,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC1>, value),
                            sizeof(ds1));
                        pd->gpso.DepthStencilState.DepthEnable = ds1.DepthEnable;
                        pd->gpso.DepthStencilState.DepthWriteMask = ds1.DepthWriteMask;
                        pd->gpso.DepthStencilState.DepthFunc = ds1.DepthFunc;
                        pd->gpso.DepthStencilState.StencilEnable = ds1.StencilEnable;
                        pd->gpso.DepthStencilState.StencilReadMask = ds1.StencilReadMask;
                        pd->gpso.DepthStencilState.StencilWriteMask = ds1.StencilWriteMask;
                        pd->gpso.DepthStencilState.FrontFace = ds1.FrontFace;
                        pd->gpso.DepthStencilState.BackFace = ds1.BackFace;
                    }
#if defined(__ID3D12Device10_INTERFACE_DEFINED__) && !defined(__MINGW32__)
                    else if (ent.convert_ds2_to_ds1 &&
                             ent.src_offset + sizeof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC2>) <= total_bytes) {
                        D3D12_DEPTH_STENCIL_DESC2 ds2{};
                        std::memcpy(&ds2,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_DEPTH_STENCIL_DESC2>, value),
                            sizeof(ds2));
                        pd->gpso.DepthStencilState.DepthEnable = ds2.DepthEnable;
                        pd->gpso.DepthStencilState.DepthWriteMask = ds2.DepthWriteMask;
                        pd->gpso.DepthStencilState.DepthFunc = ds2.DepthFunc;
                        pd->gpso.DepthStencilState.StencilEnable = ds2.StencilEnable;
                        pd->gpso.DepthStencilState.StencilReadMask = ds2.FrontFace.StencilReadMask;
                        pd->gpso.DepthStencilState.StencilWriteMask = ds2.FrontFace.StencilWriteMask;
                        pd->gpso.DepthStencilState.FrontFace = {
                            ds2.FrontFace.StencilFailOp, ds2.FrontFace.StencilDepthFailOp,
                            ds2.FrontFace.StencilPassOp, ds2.FrontFace.StencilFunc};
                        pd->gpso.DepthStencilState.BackFace = {
                            ds2.BackFace.StencilFailOp, ds2.BackFace.StencilDepthFailOp,
                            ds2.BackFace.StencilPassOp, ds2.BackFace.StencilFunc};
                    }
#endif
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>) <= total_bytes) {
                        D3D12_RT_FORMAT_ARRAY rta{};
                        std::memcpy(&rta,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value),
                            sizeof(rta));
                        pd->gpso.NumRenderTargets = std::min<UINT>(rta.NumRenderTargets, 8u);
                        for (UINT ri = 0u; ri < 8u; ++ri) {
                            pd->gpso.RTVFormats[ri] =
                                (ri < pd->gpso.NumRenderTargets)
                                    ? SanitizeXboxDxgiFormat(rta.RTFormats[ri])
                                    : DXGI_FORMAT_UNKNOWN;
                        }
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<DXGI_FORMAT>) <= total_bytes) {
                        DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN;
                        std::memcpy(&df,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<DXGI_FORMAT>, value),
                            sizeof(df));
                        pd->gpso.DSVFormat = SanitizePsoDsvFormat(df);
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<DXGI_SAMPLE_DESC>) <= total_bytes) {
                        std::memcpy(&pd->gpso.SampleDesc,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<DXGI_SAMPLE_DESC>, value),
                            sizeof(pd->gpso.SampleDesc));
                        if (pd->gpso.SampleDesc.Count == 0u || pd->gpso.SampleDesc.Count > 8u) {
                            pd->gpso.SampleDesc = {1u, 0u};
                        }
                    }
                } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
                    if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>) <= total_bytes) {
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE tp = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                        std::memcpy(&tp,
                            raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>, value),
                            sizeof(tp));
                        pd->gpso.PrimitiveTopologyType =
                            (static_cast<uint32_t>(tp) >= 1u && static_cast<uint32_t>(tp) <= 4u)
                                ? tp
                                : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    }
                }
            }
            if (pd->gpso.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH &&
                pd->hs_blob.empty() && pd->ds_blob.empty()) {
                pd->gpso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
            (void)SanitizeDepthStencilDesc(pd->gpso.DepthStencilState);
            if (pd->gpso.DSVFormat == DXGI_FORMAT_UNKNOWN) {
                pd->gpso.DepthStencilState.DepthEnable = FALSE;
                pd->gpso.DepthStencilState.StencilEnable = FALSE;
            }
            if ((!pd->vs_blob.empty() || !pd->ms_blob.empty()) && pd->host_rs) {
                pending_xdat = std::move(pd);
            }
        }
    }
    constexpr bool xdat_lifted_real_pso = false;
    if (!stream_has_xdat) {
        hr = host_dev2_->CreatePipelineState(
            final_stream_desc, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
    }
    if ((FAILED(hr) || !host_pso) && retail_scarlett_abi_) {
        bool stream_has_cs = false;
        bool stream_has_ps = false;
        ID3D12RootSignature* stream_rs = nullptr;
        D3D12_RT_FORMAT_ARRAY rt_fmts{};
        DXGI_FORMAT dsv_fmt = DXGI_FORMAT_UNKNOWN;
        DXGI_SAMPLE_DESC samp_desc{1u, 0u};
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        for (const auto& ent : entries) {
            if (ent.drop_subobject) continue;
            if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                stream_has_cs = true;
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                D3D12_SHADER_BYTECODE bc{};
                std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
                if (bc.pShaderBytecode && bc.BytecodeLength > 0u) stream_has_ps = true;
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
                ID3D12RootSignature* rs = nullptr;
                std::memcpy(&rs, raw_stream + ent.src_offset + kRootSigOffset, sizeof(rs));
                if (rs != nullptr && IsKnownProxyPointer(rs)) {
                    stream_rs = UnwrapRootSignature(rs);
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
                if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>) <= total_bytes) {
                    std::memcpy(&rt_fmts,
                        raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_RT_FORMAT_ARRAY>, value),
                        sizeof(rt_fmts));
                    if (rt_fmts.NumRenderTargets > 8u) {
                        rt_fmts = {};
                    }
                    for (UINT i = 0u; i < rt_fmts.NumRenderTargets; ++i) {
                        if (static_cast<uint32_t>(rt_fmts.RTFormats[i]) > 132u) {
                            rt_fmts.RTFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
                        }
                    }
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
                if (ent.src_offset + sizeof(StreamSubobjLayout<DXGI_FORMAT>) <= total_bytes) {
                    std::memcpy(&dsv_fmt,
                        raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<DXGI_FORMAT>, value),
                        sizeof(dsv_fmt));
                    if (static_cast<uint32_t>(dsv_fmt) > 132u) {
                        dsv_fmt = DXGI_FORMAT_UNKNOWN;
                    }
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC) {
                if (ent.src_offset + sizeof(StreamSubobjLayout<DXGI_SAMPLE_DESC>) <= total_bytes) {
                    std::memcpy(&samp_desc,
                        raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<DXGI_SAMPLE_DESC>, value),
                        sizeof(samp_desc));
                    if (samp_desc.Count == 0u || samp_desc.Count > 8u) {
                        samp_desc = {1u, 0u};
                    }
                }
            } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
                if (ent.src_offset + sizeof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>) <= total_bytes) {
                    std::memcpy(&topo,
                        raw_stream + ent.src_offset + offsetof(StreamSubobjLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>, value),
                        sizeof(topo));
                    if (static_cast<uint32_t>(topo) > static_cast<uint32_t>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)) {
                        topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    }
                }
            }
        }
        if (!stream_rs) {
            // Scan raw_stream aligned 8-byte slots for any known wrapped RootSignature proxy as backup.
            for (size_t off = 0u; off + sizeof(void*) <= total_bytes; off += sizeof(void*)) {
                void* cand = nullptr;
                std::memcpy(&cand, raw_stream + off, sizeof(cand));
                if (cand && IsKnownProxyPointer(cand)) {
                    ID3D12RootSignature* unwrapped = UnwrapRootSignature(static_cast<ID3D12RootSignature*>(cand));
                    if (unwrapped && unwrapped != cand) {
                        stream_rs = unwrapped;
                        break;
                    }
                }
            }
        }
        if (!stream_rs) {
            openscarlett::XboxRootSignatureDesc empty_rs{};
            stream_rs = SynthesizeHostRootSignatureFromXboxDesc(empty_rs);
        }
        if (stream_has_cs) {
            const auto& cs_bc = GetCachedMinimalCs50();
            if (!cs_bc.empty()) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC safe{};
                safe.pRootSignature = stream_rs;
                safe.CS = {cs_bc.data(), cs_bc.size()};
                hr = host_dev_->CreateComputePipelineState(
                    &safe, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
                if (SUCCEEDED(hr) && host_pso) {
                    RecordDev("ID3D12DeviceX::XdatShaderStandinPso", 47, S_OK);
                }
            }
        } else {
            const auto& vs_bc = GetCachedMinimalVs50();
            const auto& ps_bc = GetCachedMinimalPs50(rt_fmts.NumRenderTargets, rt_fmts.RTFormats);
            if (!vs_bc.empty() && !ps_bc.empty()) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC safe{};
                safe.pRootSignature = stream_rs;
                safe.VS = {vs_bc.data(), vs_bc.size()};
                if (stream_has_ps) {
                    safe.PS = {ps_bc.data(), ps_bc.size()};
                }
                safe.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                safe.SampleMask = UINT_MAX;
                safe.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                safe.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                safe.RasterizerState.DepthClipEnable = TRUE;
                safe.PrimitiveTopologyType =
                    (topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
                     topo == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
                        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                        : topo;
                safe.NumRenderTargets = std::min<UINT>(rt_fmts.NumRenderTargets, 8u);
                for (UINT i = 0u; i < safe.NumRenderTargets; ++i) {
                    safe.RTVFormats[i] = rt_fmts.RTFormats[i];
                }
                safe.DSVFormat = dsv_fmt;
                safe.SampleDesc.Count = (samp_desc.Count > 0u) ? samp_desc.Count : 1u;
                safe.SampleDesc.Quality = samp_desc.Quality;
                hr = host_dev_->CreateGraphicsPipelineState(
                    &safe, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
                if (FAILED(hr) || !host_pso) {
                    // Retry with sanitized default RT/DSV formats if stream subobject alignment differed.
                    safe.NumRenderTargets = 1u;
                    safe.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    for (UINT i = 1u; i < 8u; ++i) safe.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
                    safe.DSVFormat = DXGI_FORMAT_UNKNOWN;
                    safe.SampleDesc = {1u, 0u};
                    const auto& ps1_bc = GetCachedMinimalPs50(1u, safe.RTVFormats);
                    safe.PS = {ps1_bc.data(), ps1_bc.size()};
                    hr = host_dev_->CreateGraphicsPipelineState(
                        &safe, OS_D3D12_IID(ID3D12PipelineState), reinterpret_cast<void**>(&host_pso));
                }
                if (SUCCEEDED(hr) && host_pso) {
                    RecordDev("ID3D12DeviceX::XdatShaderStandinPso", 47, S_OK);
                }
            }
        }
    }
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
    if (retail_scarlett_abi_ && !xdat_lifted_real_pso && (stream_has_xdat || has_xbox_shader)) {
        proxy->SetStandinPso(true);
        if (pending_xdat) {
            proxy->SetPendingXdatGfxDesc(std::move(pending_xdat));
        }
    }
    if (vulkan_routing_active_ && has_xbox_shader) {
        VulkanPipelineInfo vk_pi = std::move(spirv_stream_pi);
        vk_pi.is_spirv_direct = false;
        bool has_any = false;
        for (const auto& ent : entries) {
            if (ent.drop_subobject || !IsShaderSubobjectType(ent.desktop_type)) continue;
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, raw_stream + ent.src_offset + kBytecodeOffset, sizeof(bc));
            if (bc.pShaderBytecode && bc.BytecodeLength > 0u &&
                openscarlett::IsXboxShaderContainerBlob(bc.pShaderBytecode, bc.BytecodeLength)) {
                std::vector<uint32_t> spv;
                if (LiftXboxStageToSpirv(bc.pShaderBytecode, bc.BytecodeLength,
                                         has_as_stage, nullptr, spv)) {
                    has_any = true;
                    if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) {
                        vk_pi.as_spirv = std::move(spv);
                    } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
                        vk_pi.is_mesh = true;
                        vk_pi.ms_spirv = std::move(spv);
                    } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                        vk_pi.vs_spirv = std::move(spv);
                    } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                        vk_pi.ps_spirv = std::move(spv);
                    } else if (ent.desktop_type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
                        vk_pi.is_compute = true;
                        vk_pi.cs_spirv = std::move(spv);
                    }
                }
            }
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
    if (!pFenceToSignal) {
        RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (NumObjects == 0u) {
        ID3D12Fence* host_fence = UnwrapFence(pFenceToSignal);
        const HRESULT hr = host_fence ? host_fence->Signal(FenceValueToSignal) : E_INVALIDARG;
        RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, hr);
        return hr;
    }
    if (!ppObjects) {
        RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, E_INVALIDARG);
        return E_INVALIDARG;
    }
    std::vector<ID3D12Pageable*> unwrapped;
    UnwrapPageables(NumObjects, ppObjects, unwrapped);
    for (ID3D12Pageable* obj : unwrapped) {
        if (!obj) {
            RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, E_INVALIDARG);
            return E_INVALIDARG;
        }
    }
    {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        uint64_t bytes_needed = 0ull;
        for (ID3D12Pageable* obj : unwrapped) {
            auto it = residency_entries_.find(obj);
            if (it != residency_entries_.end() && !it->second.is_resident && !it->second.is_placed_suballocation) {
                bytes_needed += it->second.size_bytes;
            }
        }
        const uint64_t budget = EffectiveResidencyBudgetBytesLocked();
        if (bytes_needed > 0ull && budget > 0ull && tracked_resident_bytes_ + bytes_needed > budget) {
            if ((Flags & D3D12_RESIDENCY_FLAG_DENY_OVERBUDGET) != 0) {
                RecordDev("ID3D12DeviceX::EnqueueMakeResident", 50, E_OUTOFMEMORY);
                return E_OUTOFMEMORY;
            }
            (void)EvictLruVictimsLocked((tracked_resident_bytes_ + bytes_needed) - budget, unwrapped[0]);
        }
    }
    const HRESULT hr = host_dev3_
                           ? host_dev3_->EnqueueMakeResident(
                                 Flags, NumObjects, unwrapped.data(),
                                 UnwrapFence(pFenceToSignal), FenceValueToSignal)
                           : E_NOINTERFACE;
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        ++enqueue_make_resident_calls_;
        for (ID3D12Pageable* obj : unwrapped) {
            auto it = residency_entries_.find(obj);
            if (it != residency_entries_.end()) {
                auto& e = it->second;
                ++e.residency_ref_count;
                if (!e.is_resident) {
                    e.is_resident = true;
                    if (!e.is_placed_suballocation) {
                        if (tracked_evicted_bytes_ >= e.size_bytes) tracked_evicted_bytes_ -= e.size_bytes;
                        else tracked_evicted_bytes_ = 0ull;
                        tracked_resident_bytes_ += e.size_bytes;
                    }
                }
                if (e.in_lru) {
                    residency_lru_.erase(e.lru_it);
                }
                residency_lru_.push_front(obj);
                e.lru_it = residency_lru_.begin();
                e.in_lru = true;
            }
        }
    }
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
        clean_desc = SanitizeAndClampResourceDesc(*pDesc);
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull) {
            // Retail Xbox UMA titles memset(Map(), 0, GetResourceAllocationInfo().SizeInBytes),
            // which is always 64 KiB aligned. Desktop drivers may only map ceil(Width, 4 KiB).
            clean_desc.Width = (clean_desc.Width + 65535ull) & ~65535ull;
        }
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
    if (pHeapProperties) {
        if (pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD) {
            InitialResourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else if (pHeapProperties->Type == D3D12_HEAP_TYPE_READBACK) {
            InitialResourceState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }
    if (adapter_info_.raytracing_tier == 0u &&
        (InitialResourceState & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        InitialResourceState = static_cast<D3D12_RESOURCE_STATES>(
            (InitialResourceState & ~D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clear_ptr = nullptr;
    }
    if (!ppvResource) {
        const HRESULT hr = host_dev4_->CreateCommittedResource1(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, pProtectedSession, riidResource, nullptr);
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    *ppvResource = nullptr;
    D3D12_HEAP_PROPERTIES eff_hp{};
    const D3D12_HEAP_PROPERTIES* eff_hp_ptr = pHeapProperties;
    bool try_wb_promote = false;
    if (retail_scarlett_abi_ && pHeapProperties &&
        pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD &&
        desc_ptr && desc_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        eff_hp = *pHeapProperties;
        eff_hp.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        eff_hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_hp_ptr = &eff_hp;
        try_wb_promote = true;
    }
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = host_dev4_->CreateCommittedResource1(
        eff_hp_ptr, clean_heap_flags, desc_ptr, InitialResourceState,
        clear_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    if (try_wb_promote && (FAILED(hr) || !host_res)) {
        hr = host_dev4_->CreateCommittedResource1(
            pHeapProperties, clean_heap_flags, desc_ptr, InitialResourceState,
            clear_ptr, pProtectedSession,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
    }
    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreateCommittedResource1", 53, hr);
        return hr;
    }
    if (desc_ptr) {
        const uint64_t est_bytes = EstimateResourceVramBytes(*desc_ptr);
        const uint64_t sparse_va =
            (sparse_window_iso_ && est_bytes > 0ull)
                ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
                : 0ull;
        TrackLiveResidencyObject(
            static_cast<ID3D12Pageable*>(host_res),
            est_bytes,
            pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT,
            /*is_placed_suballocation=*/false,
            /*spilled_to_l0=*/false,
            sparse_va);
        TrackBufferVaResource(host_res, *desc_ptr, 0ull);
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    if (vulkan_routing_active_ && pHeapProperties && desc_ptr) {
        TrackVulkanResource(host_res, pHeapProperties->Type, *desc_ptr);
    }
    if (did_r9g9b9e5_fallback) {
        RecordDev("ID3D12DeviceX::R9G9B9E5UavFallback", 53, S_OK);
    }
    if (retail_scarlett_abi_ && host_res) {
        SetRetailResourceState(host_res, InitialResourceState);
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
    D3D12_HEAP_DESC eff_heap{};
    const D3D12_HEAP_DESC* eff_heap_ptr = heap_ptr;
    bool try_wb_promote = false;
    if (retail_scarlett_abi_ && heap_ptr &&
        heap_ptr->Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
        eff_heap = *heap_ptr;
        eff_heap.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_heap.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        eff_heap.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_heap_ptr = &eff_heap;
        try_wb_promote = true;
    }
    ID3D12Heap* host_heap = nullptr;
    HRESULT hr = host_dev4_->CreateHeap1(
        eff_heap_ptr, pProtectedSession,
        OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    if (try_wb_promote && (FAILED(hr) || !host_heap)) {
        hr = host_dev4_->CreateHeap1(
            heap_ptr, pProtectedSession,
            OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&host_heap));
    }
    if (FAILED(hr) || !host_heap) {
        RecordDev("ID3D12DeviceX::CreateHeap1", 54, hr);
        return hr;
    }
    const uint64_t req_heap_bytes = heap_ptr ? heap_ptr->SizeInBytes : 0ull;
    const uint64_t sparse_va =
        (sparse_window_iso_ && req_heap_bytes > 0ull)
            ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, req_heap_bytes)
            : 0ull;
    TrackLiveResidencyObject(
        static_cast<ID3D12Pageable*>(host_heap),
        req_heap_bytes,
        heap_ptr ? heap_ptr->Properties.Type : D3D12_HEAP_TYPE_DEFAULT,
        /*is_placed_suballocation=*/false,
        /*spilled_to_l0=*/false,
        sparse_va,
        /*is_heap=*/true,
        /*parent_heap=*/nullptr);
    auto* proxy = new (std::nothrow) HeapXProxy(this, host_heap, sparse_va);
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
    bool had_cross_adapter = false;
    if (pDesc) {
        clean_desc = SanitizeAndClampResourceDesc(*pDesc);
        if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN) {
            clean_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        }
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull) {
            // Retail Xbox UMA titles memset(Map(), 0, GetResourceAllocationInfo().SizeInBytes),
            // which is always 64 KiB aligned. Desktop drivers may only map ceil(Width, 4 KiB).
            clean_desc.Width = (clean_desc.Width + 65535ull) & ~65535ull;
        }
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
        had_cross_adapter =
            (clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
        desc_ptr = &clean_desc;
    }
    if (adapter_info_.raytracing_tier == 0u &&
        (InitialState & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        InitialState = static_cast<D3D12_RESOURCE_STATES>(
            (InitialState & ~D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        clear_ptr = nullptr;
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
    if (had_cross_adapter) {
        if (SUCCEEDED(hr) && host_res) {
            reserved_cross_adapter_preserved_.fetch_add(1ull, std::memory_order_relaxed);
            RecordDev("ID3D12DeviceX::CreateReservedResource1[CrossAdapterPreserved]", 55, S_OK);
        } else if (desc_ptr) {
            D3D12_RESOURCE_DESC retry_desc = *desc_ptr;
            retry_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                static_cast<uint32_t>(retry_desc.Flags) &
                ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER));
            hr = host_dev4_->CreateReservedResource1(
                &retry_desc, InitialState, clear_ptr, pProtectedSession,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            if (SUCCEEDED(hr) && host_res) {
                clean_desc = retry_desc;
                reserved_cross_adapter_stripped_fallback_.fetch_add(1ull, std::memory_order_relaxed);
                RecordDev("ID3D12DeviceX::CreateReservedResource1[CrossAdapterFallback]", 55, S_OK);
            }
        }
    }
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
    if (retail_scarlett_abi_) {
        std::fprintf(stderr, "[OpenScarlett][d3d12_xs] GUEST CALLED DeviceX::RemoveDevice()!\n");
        std::fflush(stderr);
    }
    HRESULT final_reason = DXGI_ERROR_DEVICE_REMOVED;
    device_removed_reason_.store(final_reason, std::memory_order_release);
    if (host_dev5_) {
        host_dev5_->RemoveDevice();
    }
    if (host_dev_) {
        const HRESULT host_hr = host_dev_->GetDeviceRemovedReason();
        if (FAILED(host_hr)) {
            final_reason = host_hr;
            device_removed_reason_.store(final_reason, std::memory_order_release);
        }
    }
    // Wake all blocked FenceWaitDispatcher waits and pending fence callbacks immediately (Leonida #1933).
    CommandQueueX::NotifyDeviceRemovedForDevice(this, final_reason);
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

    // Sprint 14 integration: auto-lift embedded Xbox RDNA2 / Xbox-DXBC DXIL_LIBRARY
    // subobjects via LiftDxrStateObjectDesc (lib_6_5 / lib_6_6 + SBT remapping + DXR=0 fallback).
    openscarlett::DxrLiftedStateObjectResult lifted_so{};
    bool has_dxil_library = false;
    if (desc_ptr && desc_ptr->pSubobjects && desc_ptr->NumSubobjects > 0u) {
        for (UINT i = 0u; i < desc_ptr->NumSubobjects; ++i) {
            if (desc_ptr->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY &&
                desc_ptr->pSubobjects[i].pDesc != nullptr) {
                has_dxil_library = true;
                break;
            }
        }
    }
    if (has_dxil_library) {
        openscarlett::DxrStateObjectLiftOptions lift_opts{};
        lift_opts.host_dxr_tier = adapter_info_.raytracing_tier;
        lift_opts.prefer_sm66 = (adapter_info_.highest_shader_model >= 0x66u);
        lift_opts.compile_with_dxc = true;
        const openscarlett::Status lift_st =
            openscarlett::LiftDxrStateObjectDesc(desc_ptr, lift_opts, lifted_so);
        if (lift_st.ok() && lifted_so.modified && lifted_so.LiftedDesc() != nullptr) {
            desc_ptr = lifted_so.LiftedDesc();
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
        (adapter_info_.raytracing_tier == 0u || lifted_so.fallback_plan.requires_software_fallback) &&
        desc_ptr != nullptr &&
        desc_ptr->pSubobjects != nullptr &&
        desc_ptr->NumSubobjects > 0u &&
        (desc_ptr->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION ||
         desc_ptr->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)) {
        // Non-DXR adapter (e.g. GTX 1650 Ti / Win10 WARP): provide software StateObjectXProxy
        // supporting ID3D12StateObject + ID3D12StateObjectProperties shader identifier queries.
        auto* sw_proxy = new (std::nothrow) StateObjectXProxy(this, nullptr, desc_ptr);
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
    auto* proxy = new (std::nothrow) StateObjectXProxy(this, host_so, desc_ptr);
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
    if (pInfo) {
        std::memset(pInfo, 0, sizeof(*pInfo));
    }
    if (host_dev5_ && adapter_info_.raytracing_tier > 0u) {
        host_dev5_->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    } else if (pInfo && pDesc) {
        // Non-DXR adapter (e.g. GTX 1650 Ti): provide conservative 256B-aligned software AS sizes.
        const uint64_t num_descs = std::max<uint64_t>(1ull, static_cast<uint64_t>(pDesc->NumDescs));
        const uint64_t est = std::min<uint64_t>(64ull * 1024ull * 1024ull, (num_descs * 256ull + 4095ull) & ~4095ull);
        pInfo->ResultDataMaxSizeInBytes = est;
        pInfo->ScratchDataSizeInBytes = est;
        pInfo->UpdateScratchDataSizeInBytes = std::max<uint64_t>(4096ull, est / 2ull);
    }
    RecordDev("ID3D12DeviceX::GetRaytracingAccelerationStructurePrebuildInfo",
              63, (host_dev5_ || adapter_info_.raytracing_tier == 0u) ? S_OK : E_NOINTERFACE);
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
    if (!ppNewStateObject) {
        RecordDev("ID3D12DeviceX::AddToStateObject", 66, E_POINTER);
        return E_POINTER;
    }
    *ppNewStateObject = nullptr;
    ID3D12StateObject* host_so = nullptr;
    HRESULT hr = E_NOINTERFACE;
    if (host_dev7_ && adapter_info_.raytracing_tier > 0u) {
        hr = host_dev7_->AddToStateObject(
            pAddition, UnwrapStateObject(pStateObjectToGrowFrom),
            OS_D3D12_IID(ID3D12StateObject), reinterpret_cast<void**>(&host_so));
    }
    if ((FAILED(hr) || !host_so) &&
        (adapter_info_.raytracing_tier == 0u || retail_scarlett_abi_)) {
        auto* sw_proxy = new (std::nothrow) StateObjectXProxy(this, nullptr, pAddition, true);
        if (!sw_proxy) {
            RecordDev("ID3D12DeviceX::AddToStateObject", 66, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        hr = sw_proxy->QueryInterface(riid, ppNewStateObject);
        sw_proxy->Release();
        RecordDev("ID3D12DeviceX::AddToStateObject", 66, hr);
        return hr;
    }
    if (SUCCEEDED(hr) && host_so) {
        auto* proxy = new (std::nothrow) StateObjectXProxy(this, host_so, pAddition);
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

HRESULT DeviceX::DeserializeStateObjectX(
    const void* pBlob,
    SIZE_T blobSize,
    ID3D12RootSignature* pRootSignature,
    UINT flags,
    REFIID riid,
    void** ppStateObject) {
    (void)pBlob;
    (void)blobSize;
    (void)pRootSignature;
    (void)flags;
    if (!ppStateObject) {
        RecordDev("ID3D12DeviceX::DeserializeStateObjectX", 115, E_POINTER);
        return E_POINTER;
    }
    *ppStateObject = nullptr;
    auto* sw_proxy = new (std::nothrow) StateObjectXProxy(this, nullptr, nullptr, true);
    if (!sw_proxy) {
        RecordDev("ID3D12DeviceX::DeserializeStateObjectX", 115, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = sw_proxy->QueryInterface(riid, ppStateObject);
    sw_proxy->Release();
    RecordDev("ID3D12DeviceX::DeserializeStateObjectX", 115, hr);
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
        clean_desc1 = SanitizeAndClampResourceDesc1(*pDesc);
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

    D3D12_HEAP_PROPERTIES eff_hp2{};
    const D3D12_HEAP_PROPERTIES* eff_hp2_ptr = pHeapProperties;
    bool try_wb_promote2 = false;
    if (retail_scarlett_abi_ && pHeapProperties &&
        pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD &&
        desc1_ptr && desc1_ptr->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        eff_hp2 = *pHeapProperties;
        eff_hp2.Type = D3D12_HEAP_TYPE_CUSTOM;
        eff_hp2.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        eff_hp2.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        eff_hp2_ptr = &eff_hp2;
        try_wb_promote2 = true;
    }

    if (host_dev8_) {
        hr = host_dev8_->CreateCommittedResource2(
            eff_hp2_ptr, clean_hf, desc1_ptr,
            InitialResourceState, pOptimizedClearValue, pProtectedSession,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (try_wb_promote2 && (FAILED(hr) || !host_res)) {
            hr = host_dev8_->CreateCommittedResource2(
                pHeapProperties, clean_hf, desc1_ptr,
                InitialResourceState, pOptimizedClearValue, pProtectedSession,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
    } else if (desc1_ptr) {
        const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*desc1_ptr);
        hr = host_dev_->CreateCommittedResource(
            eff_hp2_ptr, clean_hf, &d0,
            InitialResourceState, pOptimizedClearValue,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (try_wb_promote2 && (FAILED(hr) || !host_res)) {
            hr = host_dev_->CreateCommittedResource(
                pHeapProperties, clean_hf, &d0,
                InitialResourceState, pOptimizedClearValue,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
    }
    if (SUCCEEDED(hr) && host_res && ppvResource) {
        const D3D12_RESOURCE_DESC d0 = desc1_ptr ? Desc1ToDesc0(*desc1_ptr) : D3D12_RESOURCE_DESC{};
        if (desc1_ptr) {
            const uint64_t est_bytes = EstimateResourceVramBytes(d0);
            const uint64_t sparse_va =
                (sparse_window_iso_ && est_bytes > 0ull)
                    ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
                    : 0ull;
            TrackLiveResidencyObject(
                static_cast<ID3D12Pageable*>(host_res),
                est_bytes,
                pHeapProperties ? pHeapProperties->Type : D3D12_HEAP_TYPE_DEFAULT,
                /*is_placed_suballocation=*/false,
                /*spilled_to_l0=*/false,
                sparse_va);
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
    ID3D12Heap* host_heap = UnwrapHeap(pHeap);
    D3D12_HEAP_DESC hd{};
    bool has_heap_desc = false;
    uint64_t heap_remaining_bytes = UINT64_MAX;
    if (host_heap) {
        hd = host_heap->GetDesc();
        has_heap_desc = true;
        heap_remaining_bytes = (hd.SizeInBytes > HeapOffset) ? (hd.SizeInBytes - HeapOffset) : 0ull;
        if (hd.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
            InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else if (hd.Properties.Type == D3D12_HEAP_TYPE_READBACK) {
            InitialState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }
    D3D12_RESOURCE_DESC1 clean_desc1{};
    const D3D12_RESOURCE_DESC1* desc1_ptr = pDesc;
    bool should_promote_oversized = false;
    if (pDesc) {
        clean_desc1 = SanitizeAndClampResourceDesc1(*pDesc);
        desc1_ptr = &clean_desc1;
        if (has_heap_desc && clean_desc1.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(clean_desc1);
            const D3D12_RESOURCE_ALLOCATION_INFO req_alloc =
                host_dev_->GetResourceAllocationInfo(0u, 1u, &d0);
            const uint64_t eff_align = (req_alloc.Alignment > 0ull) ? req_alloc.Alignment : 65536ull;
            const bool size_exceeds_heap =
                (req_alloc.SizeInBytes != UINT64_MAX && req_alloc.SizeInBytes > heap_remaining_bytes);
            const bool align_incompatible =
                ((HeapOffset % eff_align) != 0ull) ||
                (hd.Alignment > 0ull && eff_align > hd.Alignment && (hd.Alignment % eff_align) != 0ull);
            should_promote_oversized = size_exceeds_heap || align_incompatible;
        }
    }
    ID3D12Resource* host_res = nullptr;
    HRESULT hr = E_INVALIDARG;
    if (!should_promote_oversized) {
        if (host_dev8_) {
            hr = host_dev8_->CreatePlacedResource1(
                host_heap, HeapOffset, desc1_ptr, InitialState, pOptimizedClearValue,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        } else if (desc1_ptr) {
            const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(*desc1_ptr);
            hr = host_dev_->CreatePlacedResource(
                host_heap, HeapOffset, &d0, InitialState, pOptimizedClearValue,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
    }
    bool promoted_oversized = false;
    D3D12_HEAP_PROPERTIES promo_hp{};
    if ((FAILED(hr) || !host_res) && desc1_ptr &&
        desc1_ptr->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && has_heap_desc &&
        (should_promote_oversized || hr == E_INVALIDARG || hr == E_OUTOFMEMORY)) {
        promo_hp = hd.Properties;
        D3D12_RESOURCE_DESC1 promo_desc1 = *desc1_ptr;
        promo_desc1.Alignment = 0ull;
        auto try_commit_desc1 = [&](const D3D12_HEAP_PROPERTIES& hp) -> HRESULT {
            if (host_dev8_) {
                return host_dev8_->CreateCommittedResource2(
                    &hp, D3D12_HEAP_FLAG_NONE, &promo_desc1, InitialState, pOptimizedClearValue,
                    nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
            }
            const D3D12_RESOURCE_DESC d0 = Desc1ToDesc0(promo_desc1);
            return host_dev_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &d0, InitialState, pOptimizedClearValue,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        };
        hr = try_commit_desc1(promo_hp);
        if ((FAILED(hr) || !host_res) && promo_hp.Type != D3D12_HEAP_TYPE_DEFAULT) {
            promo_hp = D3D12_HEAP_PROPERTIES{};
            promo_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = try_commit_desc1(promo_hp);
        }
        if (SUCCEEDED(hr) && host_res) {
            promoted_oversized = true;
            promoted_oversized_placed_resources_.fetch_add(1ull, std::memory_order_relaxed);
            RecordDev("ID3D12DeviceX::CreatePlacedResource1[PromotedOversized]", 70, S_OK);
        }
    }
    if (SUCCEEDED(hr) && host_res && ppvResource) {
        const D3D12_RESOURCE_DESC d0 = desc1_ptr ? Desc1ToDesc0(*desc1_ptr) : D3D12_RESOURCE_DESC{};
        if (desc1_ptr) {
            const uint64_t est_bytes = EstimateResourceVramBytes(d0);
            const uint64_t sparse_va =
                (sparse_window_iso_ && est_bytes > 0ull)
                    ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
                    : 0ull;
            TrackLiveResidencyObject(
                static_cast<ID3D12Pageable*>(host_res),
                est_bytes,
                promoted_oversized ? promo_hp.Type : (has_heap_desc ? hd.Properties.Type : D3D12_HEAP_TYPE_DEFAULT),
                /*is_placed_suballocation=*/!promoted_oversized,
                /*spilled_to_l0=*/false,
                sparse_va);
            TrackBufferVaResource(host_res, d0, 0ull);
        }
        if (pDesc) {
            const XG_TILE_MODE tile_mode = ExtractXboxGfx10TileMode(pDesc->Layout);
            if (tile_mode != XG_TILE_MODE_LINEAR) {
                SetTrackedResourceTileMode(host_res, tile_mode);
            }
        }
        auto* proxy = new (std::nothrow) ResourceXProxy(
            this, host_res, desc1_ptr ? &d0 : nullptr,
            promoted_oversized ? &promo_hp : nullptr, D3D12_HEAP_FLAG_NONE);
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
    static const bool s_trace_fint = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_fint_logs{0u};
    if (s_trace_fint && s_fint_logs.fetch_add(1u, std::memory_order_relaxed) < 5u) {
        std::fprintf(stderr,
                     "[OpenScarlett][d3d12_xs] SetFrameIntervalX(period=%uus maxLat=%u flags=0x%X)\n",
                     PeriodMicroseconds, MaxFrameLatency, Flags);
        std::fflush(stderr);
    }
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
    D3D12_RESOURCE_DESC clean_desc = SanitizeAndClampResourceDesc(*pDesc);
    D3D12_CLEAR_VALUE clean_clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = pOptimizedClearValue;
    if (pOptimizedClearValue != nullptr) {
        const DXGI_FORMAT san_clear = SanitizeXboxDxgiFormat(pOptimizedClearValue->Format);
        if (san_clear != pOptimizedClearValue->Format) {
            clean_clear = *pOptimizedClearValue;
            clean_clear.Format = san_clear;
            clear_ptr = &clean_clear;
        }
    }
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

    // On Xbox Series X, color textures (e.g. video decode output targets) may be bound as RTVs
    // without explicitly setting ALLOW_RENDER_TARGET on CreatePlacedResourceX. Preserve the
    // original flags for unified-texture CPU upload decisions while enabling desktop RTV binding.
    const bool orig_had_gpu_only_flags =
        (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) &&
        ((clean_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0 ||
         clean_desc.SampleDesc.Count > 1u ||
         clean_desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    if (retail_scarlett_abi_) {
        if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            // Retail Xbox UMA titles bind buffers created via CreatePlacedResourceX as RWByteAddressBuffer /
            // RWStructuredBuffer UAVs without setting ALLOW_UNORDERED_ACCESS. All CreatePlacedResourceX buffers
            // reside on DEFAULT or L0 NOT_AVAILABLE heaps where ALLOW_UNORDERED_ACCESS is universally valid.
            clean_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        } else if ((clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 &&
                   clean_desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE &&
                   clean_desc.SampleDesc.Count <= 1u) {
            if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                clean_desc.Width >= 256u && clean_desc.Height >= 256u &&
                (clean_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                 clean_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                 clean_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 clean_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                 clean_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 clean_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                 clean_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)) {
                clean_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (HostFormatSupportsTypedUav(clean_desc.Format)) {
                clean_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
        }
    }
    D3D12_RESOURCE_STATES actual_initial_state = InitialState;

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

    // Retail Scarlett render-graph aliasing: place textures into a single shared 256 MiB
    // DEFAULT aliasing heap (plus 64 MiB MSAA heap) using VirtualAddress mod heap size.
    // On Xbox Series X, GTA VI aliases hundreds of transient 1440p/4K RT/DS/UAV textures
    // inside a ~256 MiB transient range (0xCE0000000..0xCEFFFFFFF); aliasing them into one
    // fixed 256 MiB Tier-2 ID3D12Heap keeps total RT VRAM bounded at 256 MiB on 4 GiB GPUs.
    D3D12_HEAP_PROPERTIES default_heap_props{};
    default_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    bool spilled_to_l0 = false;
    bool placed_in_alias_heap = false;
    if ((FAILED(hr) || !host_res) && retail_scarlett_abi_ && VirtualAddress != 0ull) {
        const bool is_buf = (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        const bool is_msaa = (!is_buf && clean_desc.SampleDesc.Count > 1u);
        const bool is_ds =
            !is_buf && (clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        const bool is_rt_ds =
            !is_buf &&
            (clean_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
        // Isolate Color RTs (map_key = 0) and Depth-Stencil targets (map_key = 5) into separate
        // ALLOW_ONLY_RT_DS_TEXTURES aliasing heaps so 64 MiB 4K DSV clears NEVER overwrite active
        // 4K Color Render Targets (e.g. Scaleform UI RT), and non-RT/DS textures stay on map_key = 4.
        // Scale dedicated commit thresholds on <= 4.5 GiB GPUs (e.g. GTX 1650 Ti, 3.34 GiB budget)
        // so 4K swapchains + UI RTs never spill into non-resident OOM fallbacks.
        const bool tight_4gb_vram =
            (adapter_info_.dedicated_video_memory_bytes > 0ull &&
             adapter_info_.dedicated_video_memory_bytes <= (4608ull * 1024ull * 1024ull));
        const uint64_t max_srv_vram_bytes =
            tight_4gb_vram ? (1024ull * 1024ull * 1024ull) : (1536ull * 1024ull * 1024ull);
        const uint64_t max_rt_ds_vram_bytes =
            tight_4gb_vram ? (768ull * 1024ull * 1024ull) : (1024ull * 1024ull * 1024ull);
        const bool non_rt_over_budget =
            (!is_buf && !is_rt_ds && !is_msaa) &&
            (committed_non_rt_vram_bytes_.load(std::memory_order_relaxed) >= max_srv_vram_bytes);
        const bool rt_ds_over_budget =
            (is_rt_ds && !is_msaa) &&
            (committed_rt_ds_vram_bytes_.load(std::memory_order_relaxed) >= max_rt_ds_vram_bytes);
        const bool want_alias = rt_ds_over_budget || is_msaa || non_rt_over_budget || (is_buf && clean_desc.Width <= 65536ull);
        if (want_alias) {
            const uint64_t pool_bytes = is_buf ? (64ull * 1024ull * 1024ull)
                                               : (is_msaa ? (64ull * 1024ull * 1024ull)
                                                          : (is_ds ? (128ull * 1024ull * 1024ull)
                                                                   : (256ull * 1024ull * 1024ull)));
            const D3D12_RESOURCE_ALLOCATION_INFO ainfo =
                is_buf ? D3D12_RESOURCE_ALLOCATION_INFO{65536ull, 65536ull}
                       : host_dev_->GetResourceAllocationInfo(0u, 1u, &clean_desc);
            const uint64_t req_align = std::max<uint64_t>(
                ainfo.Alignment ? ainfo.Alignment : 65536ull, 65536ull);
            if (ainfo.SizeInBytes > 0ull && ainfo.SizeInBytes <= pool_bytes) {
                uint64_t heap_off = (VirtualAddress & (pool_bytes - 1ull)) & ~(req_align - 1ull);
                if (heap_off + ainfo.SizeInBytes > pool_bytes) {
                    heap_off = 0ull;
                }
                const uint64_t map_key = is_buf ? 2ull : (is_msaa ? 1ull : (is_ds ? 5ull : (is_rt_ds ? 0ull : 4ull)));
                ID3D12Heap* alias_heap = nullptr;
                {
                    std::lock_guard<std::mutex> alk(placed_alias_mutex_);
                    auto it = placed_alias_heaps_.find(map_key);
                    if (it != placed_alias_heaps_.end() && it->second) {
                        alias_heap = it->second;
                        alias_heap->AddRef();
                    } else {
                        D3D12_HEAP_DESC hd{};
                        hd.SizeInBytes = pool_bytes;
                        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                        hd.Alignment = is_msaa ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                               : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                        hd.Flags = is_buf ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
                                          : ((is_rt_ds || is_msaa)
                                                 ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                                 : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
                        ID3D12Heap* new_heap = nullptr;
                        HRESULT hhr = host_dev_->CreateHeap(
                            &hd, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&new_heap));
                        if (SUCCEEDED(hhr) && new_heap) {
                            placed_alias_heaps_[map_key] = new_heap;
                            alias_heap = new_heap;
                            alias_heap->AddRef();
                        }
                    }
                }
                if (alias_heap) {
                    hr = host_dev_->CreatePlacedResource(
                        alias_heap, heap_off, &clean_desc, InitialState,
                        clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
                    if (SUCCEEDED(hr) && host_res) {
                        placed_in_alias_heap = true;
                    }
                    alias_heap->Release();
                }
            }
        }
    }

    // Fallback to committed default-heap resource if not attached or heap placement incompatible.
    if (FAILED(hr) || !host_res) {
        hr = host_dev_->CreateCommittedResource(
            &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (SUCCEEDED(hr) && host_res && retail_scarlett_abi_ &&
            clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            const uint64_t est_vram = EstimateResourceVramBytes(clean_desc);
            if ((clean_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                     D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
                committed_rt_ds_vram_bytes_.fetch_add(est_vram, std::memory_order_relaxed);
            } else {
                committed_non_rt_vram_bytes_.fetch_add(est_vram, std::memory_order_relaxed);
            }
        }
    }
    if ((FAILED(hr) || !host_res) && hr == E_OUTOFMEMORY) {
        // 4-8 GiB desktop GPUs can exhaust dedicated L1 VRAM when retail Scarlett titles
        // alias dozens of 1440p/4K render-graph targets onto the same physical VAs.
        // Spill cleanly into GPU-accessible L0 system memory (NOT_AVAILABLE supports RT/DS/UAV).
        D3D12_HEAP_PROPERTIES l0_props{};
        l0_props.Type = D3D12_HEAP_TYPE_CUSTOM;
        l0_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        l0_props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        l0_props.CreationNodeMask = 1u;
        l0_props.VisibleNodeMask = 1u;
        hr = host_dev_->CreateCommittedResource(
            &l0_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
            clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        if (SUCCEEDED(hr) && host_res) {
            default_heap_props = l0_props;
            spilled_to_l0 = true;
        } else {
            // Second fallback: page-demand non-resident DEFAULT allocation.
            hr = host_dev_->CreateCommittedResource(
                &default_heap_props, D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &clean_desc, InitialState,
                clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
        // Third fallback for large (>64 KiB) buffers on 4 GiB GPUs when both L1 and L0
        // committed allocations hit E_OUTOFMEMORY: place into an on-demand 64 MiB L0
        // ALLOW_ONLY_BUFFERS spillover aliasing heap (map_key = 3, never touches <=64 KiB pool).
        if ((FAILED(hr) || !host_res) && retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.Width > 0ull && clean_desc.Width <= 64ull * 1024ull * 1024ull) {
            constexpr uint64_t kSpillPoolBytes = 64ull * 1024ull * 1024ull;
            const uint64_t aligned_sz = (clean_desc.Width + 65535ull) & ~65535ull;
            uint64_t heap_off = (VirtualAddress & (kSpillPoolBytes - 1ull)) & ~65535ull;
            if (heap_off + aligned_sz > kSpillPoolBytes) {
                heap_off = 0ull;
            }
            ID3D12Heap* spill_heap = nullptr;
            {
                std::lock_guard<std::mutex> alk(placed_alias_mutex_);
                auto it = placed_alias_heaps_.find(3ull);
                if (it != placed_alias_heaps_.end() && it->second) {
                    spill_heap = it->second;
                    spill_heap->AddRef();
                } else {
                    D3D12_HEAP_DESC hd{};
                    hd.SizeInBytes = kSpillPoolBytes;
                    hd.Properties = l0_props;
                    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
                    ID3D12Heap* new_heap = nullptr;
                    if (SUCCEEDED(host_dev_->CreateHeap(
                            &hd, OS_D3D12_IID(ID3D12Heap), reinterpret_cast<void**>(&new_heap))) &&
                        new_heap) {
                        placed_alias_heaps_[3ull] = new_heap;
                        spill_heap = new_heap;
                        spill_heap->AddRef();
                    } else {
                        // Commit/VRAM already full: reuse the pre-allocated 64 MiB buffer heap (map_key=2)
                        // at zero additional memory cost so large buffers never fail with E_OUTOFMEMORY.
                        auto it2 = placed_alias_heaps_.find(2ull);
                        if (it2 != placed_alias_heaps_.end() && it2->second) {
                            spill_heap = it2->second;
                            spill_heap->AddRef();
                        }
                    }
                }
            }
            if (spill_heap) {
                hr = host_dev_->CreatePlacedResource(
                    spill_heap, heap_off, &clean_desc, InitialState,
                    clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
                if (SUCCEEDED(hr) && host_res) {
                    default_heap_props = l0_props;
                    spilled_to_l0 = true;
                    placed_in_alias_heap = true;
                }
                spill_heap->Release();
            }
        }
    }
    if ((FAILED(hr) || !host_res) && hr == E_INVALIDARG && retail_scarlett_abi_ &&
        clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        if ((clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            clean_desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            hr = host_dev_->CreateCommittedResource(
                &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, InitialState,
                clear_ptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
        }
        if (FAILED(hr) || !host_res) {
            const D3D12_RESOURCE_FLAGS keep_rt =
                clean_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            clean_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (keep_rt != D3D12_RESOURCE_FLAG_NONE) {
                clean_desc.Flags = keep_rt;
                hr = host_dev_->CreateCommittedResource(
                    &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
                if (SUCCEEDED(hr) && host_res) actual_initial_state = D3D12_RESOURCE_STATE_COMMON;
            }
            if (FAILED(hr) || !host_res) {
                clean_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                hr = host_dev_->CreateCommittedResource(
                    &default_heap_props, D3D12_HEAP_FLAG_NONE, &clean_desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&host_res));
                if (SUCCEEDED(hr) && host_res) actual_initial_state = D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }

    if (FAILED(hr) || !host_res) {
        RecordDev("ID3D12DeviceX::CreatePlacedResourceX", 76, hr);
        return hr;
    }
    {
        static std::atomic<uint32_t> s_cprx_cnt{0u};
        static std::atomic<uint64_t> s_cprx_vram{0ull};
        static std::atomic<uint64_t> s_cprx_l0{0ull};
        const uint64_t eb = placed_in_alias_heap ? 0ull : EstimateResourceVramBytes(clean_desc);
        const uint32_t n = s_cprx_cnt.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t tv = s_cprx_vram.fetch_add(spilled_to_l0 ? 0ull : eb, std::memory_order_relaxed) + (spilled_to_l0 ? 0ull : eb);
        const uint64_t tl = s_cprx_l0.fetch_add(spilled_to_l0 ? eb : 0ull, std::memory_order_relaxed) + (spilled_to_l0 ? eb : 0ull);
        static const bool trace_cprx = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        if (trace_cprx && ((n % 100u) == 0u || (spilled_to_l0 && (n % 25u) == 0u))) {
            size_t alias_buckets = 0u;
            {
                std::lock_guard<std::mutex> alk(placed_alias_mutex_);
                alias_buckets = placed_alias_heaps_.size();
            }
            std::fprintf(stderr,
                         "[OpenScarlett][CPRX] #%u va=0x%llX dim=%d %llux%u fmt=%d alias=%d(%zu*64MB) l0=%d | comm_vram=%lluMB comm_l0=%lluMB\n",
                         n, static_cast<unsigned long long>(VirtualAddress),
                         static_cast<int>(clean_desc.Dimension),
                         static_cast<unsigned long long>(clean_desc.Width), clean_desc.Height,
                         static_cast<int>(clean_desc.Format), placed_in_alias_heap ? 1 : 0, alias_buckets,
                         spilled_to_l0 ? 1 : 0,
                         static_cast<unsigned long long>(tv >> 20u),
                         static_cast<unsigned long long>(tl >> 20u));
            std::fflush(stderr);
        }
    }

    {
        const uint64_t est_bytes = EstimateResourceVramBytes(clean_desc);
        const uint64_t sparse_va =
            (sparse_window_iso_ && est_bytes > 0ull)
                ? openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, est_bytes)
                : 0ull;
        TrackLiveResidencyObject(
            static_cast<ID3D12Pageable*>(host_res),
            est_bytes,
            default_heap_props.Type,
            /*is_placed_suballocation=*/false,
            /*spilled_to_l0=*/spilled_to_l0,
            sparse_va);
    }
    TrackFallbackResource(host_res, did_r9g9b9e5_fallback);
    TrackBufferVaResource(host_res, clean_desc, VirtualAddress);
    if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && VirtualAddress != 0ull) {
        TrackPlacedTextureVa(host_res, VirtualAddress);
    }
    {
        const XG_TILE_MODE tile_mode =
            retail_scarlett_abi_ ? ResolveRetailXboxTileMode(*pDesc) : ExtractXboxGfx10TileMode(pDesc->Layout);
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
        // Skip per-buffer TitleVAMirror staging allocation for tiny (<=16 KiB) all-zero GPU
        // buffers (3,500+ 4 KiB render-graph pools) and L0-spilled OOM buffers so 16 GiB hosts
        // never exhaust commit charge while keeping all CPU-written ring buffers mirrored in full.
        bool skip_small_zero_mirror = false;
        const uint64_t cpu_va = ResolveGuestCpuAliasVa(VirtualAddress);
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            mirror_bytes > 0ull && mirror_bytes <= 16384ull &&
            IsReadableGuestVaRange(cpu_va, mirror_bytes)) {
            const auto* gp = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(cpu_va));
            const size_t nw = static_cast<size_t>(mirror_bytes >> 3u);
            uint64_t acc = 0ull;
            for (size_t i = 0u; i < nw; ++i) acc |= gp[i];
            if (acc == 0ull) skip_small_zero_mirror = true;
        }
        const XG_TILE_MODE xbox_tile_mode =
            retail_scarlett_abi_ ? ResolveRetailXboxTileMode(*pDesc) : ExtractXboxGfx10TileMode(pDesc->Layout);
        const bool is_cpu_linear_candidate =
            (xbox_tile_mode == XG_TILE_MODE_LINEAR) &&
            (pDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
             pDesc->Layout == static_cast<D3D12_TEXTURE_LAYOUT>(D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE) ||
             clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ||
             clean_desc.Format == DXGI_FORMAT_R8_UNORM ||
             clean_desc.Format == DXGI_FORMAT_R8_TYPELESS ||
             clean_desc.Format == DXGI_FORMAT_A8_UNORM ||
             clean_desc.Format == DXGI_FORMAT_R8G8_UNORM ||
             clean_desc.Format == DXGI_FORMAT_R8G8_TYPELESS);
        const bool is_uma_srv_tex_candidate =
            retail_scarlett_abi_ &&
            clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            clean_desc.SampleDesc.Count <= 1u &&
            !IsDepthOrPlanarFormatForCopy(clean_desc.Format) &&
            !orig_had_gpu_only_flags &&
            (pDesc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) == 0u;
        const bool is_swizzled_retail_tex =
            retail_scarlett_abi_ &&
            clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            !is_cpu_linear_candidate &&
            !is_uma_srv_tex_candidate;
        bool skip_non_ww_gpu_buf = false;
        if (retail_scarlett_abi_ &&
            clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            (orig_had_gpu_only_flags || (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) &&
            IsReadableGuestVaRange(cpu_va, std::min<uint64_t>(mirror_bytes, 4096ull))) {
            void* probe_p = nullptr;
            ULONG_PTR probe_c = 1u;
            DWORD probe_g = 4096u;
            if (GetWriteWatch(0u, reinterpret_cast<void*>(static_cast<uintptr_t>(cpu_va)),
                              4096u, &probe_p, &probe_c, &probe_g) != 0u) {
                skip_non_ww_gpu_buf = true;
            }
        }
        if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            mirror_bytes > 0ull && !skip_small_zero_mirror && !skip_non_ww_gpu_buf && !spilled_to_l0 &&
            IsReadableGuestVaRange(cpu_va, mirror_bytes)) {
            openscarlett::TitleVaMappingDesc mdesc{};
            mdesc.title_va = VirtualAddress;
            mdesc.size_bytes = mirror_bytes;
            mdesc.kind = openscarlett::TitleVaResourceKind::Buffer;
            mdesc.existing_write_watch_cpu_ptr =
                reinterpret_cast<void*>(static_cast<uintptr_t>(cpu_va));
            mdesc.gpu_shadow_resource = host_res;
            mdesc.initial_state = actual_initial_state;
            openscarlett::TitleVaRegionInfo reg_info{};
            if (title_va_mirror_.RegisterRegion(mdesc, host_dev_, reg_info).ok()) {
                std::lock_guard<std::mutex> lock(mirrored_res_mutex_);
                mirrored_res_to_va_[host_res] = VirtualAddress;
                mirrored_va_count_.store(
                    static_cast<uint32_t>(mirrored_res_to_va_.size()), std::memory_order_release);
                mirrored_by_title_va = true;
            }
        }
        if ((is_uma_srv_tex_candidate || !skip_small_zero_mirror) &&
            !skip_non_ww_gpu_buf && !is_swizzled_retail_tex &&
            (!spilled_to_l0 || is_cpu_linear_candidate || is_uma_srv_tex_candidate) && !orig_had_gpu_only_flags) {
            AutoRegisterUnifiedResource(
                host_res, cpu_va, clean_desc, actual_initial_state, mirrored_by_title_va,
                /*use_footprint_row_pitch=*/(pDesc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
        }
    }
    if (retail_scarlett_abi_ && host_res) {
        // Strip buffer-only state bits (e.g. VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER) from textures
        // so subsequent ResourceBarrier transitions never fail Close() with E_INVALIDARG.
        if (clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            constexpr UINT kTexAllowed =
                static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_READ) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
                static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            actual_initial_state = static_cast<D3D12_RESOURCE_STATES>(
                static_cast<UINT>(actual_initial_state) & kTexAllowed);
        }
        SetRetailResourceState(host_res, actual_initial_state);
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

HRESULT STDMETHODCALLTYPE DeviceX::GetGpuHardwareConfigurationX(
    D3D12XBOX_GPU_HARDWARE_CONFIGURATION* pOutConfig) {
    if (!pOutConfig) {
        RecordDev("ID3D12DeviceX::GetGpuHardwareConfigurationX", 80, E_POINTER);
        return E_POINTER;
    }
    std::memset(pOutConfig, 0, sizeof(*pOutConfig));
    uint32_t profile_id = 0u;
    uint64_t console_vram = 0ull;
    uint64_t eff_budget = 0ull;
    {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        profile_id = console_profile_.profile_id;
        console_vram = console_profile_.target_console_vram_bytes;
        eff_budget = EffectiveResidencyBudgetBytesLocked();
    }
    pOutConfig->StructSize = static_cast<uint32_t>(sizeof(*pOutConfig));
    pOutConfig->ConsoleProfile = profile_id;
    pOutConfig->ComputeUnitCount = (profile_id == 1u) ? 52u : 20u;
    pOutConfig->ShaderEngineCount = (profile_id == 1u) ? 4u : 2u;
    pOutConfig->GpuClockMhz = (profile_id == 1u) ? 1825u : 1565u;
    pOutConfig->HostVendorClass = static_cast<uint32_t>(sanitized_ihv_caps_.vendor_class);
    pOutConfig->HostVendorId = sanitized_ihv_caps_.vendor_id;
    pOutConfig->HostDeviceId = sanitized_ihv_caps_.device_id;
    pOutConfig->HostWaveLaneMin = sanitized_ihv_caps_.wave_lane_min;
    pOutConfig->HostWaveLaneMax = sanitized_ihv_caps_.wave_lane_max;
    pOutConfig->PreferredWaveSize = sanitized_ihv_caps_.preferred_wave_size;
    pOutConfig->HostRaytracingTier = sanitized_ihv_caps_.raytracing_tier;
    pOutConfig->HostMeshShaderTier = sanitized_ihv_caps_.mesh_shader_tier;
    pOutConfig->HostHighestShaderModel = sanitized_ihv_caps_.highest_shader_model;
    pOutConfig->IsWarp = sanitized_ihv_caps_.is_warp ? 1u : 0u;
    pOutConfig->IsUmaLowVram = sanitized_ihv_caps_.is_uma_low_vram ? 1u : 0u;
    pOutConfig->RequiresWave64Emulation = sanitized_ihv_caps_.requires_wave64_emulation ? 1u : 0u;
    pOutConfig->SupportsPageIsolatedDescriptorHeaps = 1u;
    pOutConfig->SupportsUnboundedSamplerHeaps = 1u;
    pOutConfig->SupportsDedicatedCopyEngine = 1u;
    pOutConfig->ConsoleVramBudgetBytes = console_vram;
    pOutConfig->HostDedicatedVramBytes = sanitized_ihv_caps_.dedicated_video_memory_bytes;
    pOutConfig->EffectiveVramBudgetBytes =
        (eff_budget > 0ull) ? eff_budget : sanitized_ihv_caps_.effective_vram_budget_bytes;
    const size_t n = std::min(sanitized_ihv_caps_.description.size(), sizeof(pOutConfig->AdapterName) - 1u);
    if (n > 0u) {
        std::memcpy(pOutConfig->AdapterName, sanitized_ihv_caps_.description.data(), n);
    }
    pOutConfig->AdapterName[n] = '\0';
    RecordDev("ID3D12DeviceX::GetGpuHardwareConfigurationX", 80, S_OK);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Title Unified-Memory Texture Remap Tracking (ivall/xcl fd74662/bdae5a4)
// ---------------------------------------------------------------------------
void DeviceX::NotifyGpuWrittenTexture(ID3D12Resource* host_resource) noexcept {
    ID3D12Resource* res = UnwrapResource(host_resource);
    if (!res || !HasRegisteredUnifiedTextures()) return;
    if (res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return;
    std::lock_guard<std::mutex> lock(unified_mutex_);
    auto it = unified_textures_.find(res);
    if (it != unified_textures_.end()) {
        it->second.allow_cpu_upload = false;
        it->second.cpu_readable = false;
        if (it->second.dirty) {
            it->second.dirty = false;
            dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
        }
    }
}

void DeviceX::NotifyGpuWrittenBufferRange(
    ID3D12Resource* host_resource, uint64_t offset, uint64_t size_bytes) noexcept {
    ID3D12Resource* res = UnwrapResource(host_resource);
    if (!res || size_bytes == 0ull) return;
    if (res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    const uint64_t r_start = offset;
    const uint64_t r_end = (size_bytes > UINT64_MAX - offset) ? UINT64_MAX : (offset + size_bytes);
    if (r_end <= r_start) return;

    std::lock_guard<std::mutex> lock(gpu_written_buf_mutex_);
    auto& vec = gpu_written_buffer_ranges_[res];
    if (vec.empty()) {
        vec.emplace_back(r_start, r_end);
        return;
    }
    auto it = std::lower_bound(
        vec.begin(), vec.end(), r_start,
        [](const std::pair<uint64_t, uint64_t>& a, uint64_t val) { return a.second < val; });
    if (it == vec.end() || it->first > r_end) {
        vec.insert(it, std::make_pair(r_start, r_end));
    } else {
        it->first = std::min(it->first, r_start);
        it->second = std::max(it->second, r_end);
        auto next = std::next(it);
        while (next != vec.end() && next->first <= it->second) {
            it->second = std::max(it->second, next->second);
            next = vec.erase(next);
        }
    }
    if (vec.size() > 256u) {
        const uint64_t min_s = vec.front().first;
        const uint64_t max_e = vec.back().second;
        vec.clear();
        vec.emplace_back(min_s, max_e);
    }
}

void DeviceX::NotifyGpuWrittenBuffer(ID3D12Resource* host_resource) noexcept {
    NotifyGpuWrittenBufferRange(host_resource, 0ull, UINT64_MAX);
}

bool DeviceX::IsGpuWrittenBufferRange(
    ID3D12Resource* host_resource, uint64_t offset, uint64_t size_bytes) const noexcept {
    ID3D12Resource* res = UnwrapResource(host_resource);
    if (!res) return false;
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    bool has_guest_uma_va = false;
    {
        std::shared_lock<std::shared_mutex> slk(buffer_va_mutex_);
        auto bit = buffer_va_map_.find(res);
        if (bit != buffer_va_map_.end()) {
            if (bit->second.guest_va != 0ull) {
                has_guest_uma_va = true;
            } else if (bit->second.mapped_cpu_ptr != nullptr) {
                // Committed UPLOAD / L0 buffers (guest_va == 0) are directly CPU-written via Map().
                return true;
            }
        }
    }
    if (!has_guest_uma_va && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        return true;
    }
    const uint64_t q_start = offset;
    const uint64_t q_end = (size_bytes == 0ull || size_bytes > UINT64_MAX - offset)
                               ? UINT64_MAX
                               : (offset + size_bytes);
    std::lock_guard<std::mutex> lock(gpu_written_buf_mutex_);
    auto it = gpu_written_buffer_ranges_.find(res);
    if (it == gpu_written_buffer_ranges_.end() || it->second.empty()) return false;
    const auto& ranges = it->second;
    auto rit = std::upper_bound(
        ranges.begin(), ranges.end(), q_start,
        [](uint64_t val, const std::pair<uint64_t, uint64_t>& r) { return val < r.first; });
    if (rit != ranges.begin()) {
        auto prev = std::prev(rit);
        if (prev->second > q_start) return true;
    }
    if (rit != ranges.end() && rit->first < q_end) {
        return true;
    }
    return false;
}

bool DeviceX::IsGpuWrittenBuffer(ID3D12Resource* host_resource) const noexcept {
    return IsGpuWrittenBufferRange(host_resource, 0ull, UINT64_MAX);
}

namespace {
struct PsoVertexLayoutEntry {
    uint32_t pos_off = 0u;
    uint32_t pos_comps = 2u;
    uint32_t non_float_mask = 0u;
};
std::mutex g_pso_vlayout_mu;
std::unordered_map<ID3D12PipelineState*, PsoVertexLayoutEntry> g_pso_vlayout_map;
std::unordered_set<ID3D12PipelineState*> g_pso_zero_ia_set;
}  // namespace

void DeviceX::RecordPsoVertexLayout(
    ID3D12PipelineState* host_pso,
    const D3D12_INPUT_ELEMENT_DESC* elems,
    UINT num_elems) noexcept {
    if (!host_pso) return;
    if (!elems || num_elems == 0u) {
        std::lock_guard<std::mutex> lk(g_pso_vlayout_mu);
        g_pso_zero_ia_set.insert(host_pso);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_pso_vlayout_mu);
        g_pso_zero_ia_set.erase(host_pso);
    }
    auto fmt_bytes_and_kind = [](DXGI_FORMAT fmt, uint32_t& out_bytes, uint32_t& out_float_comps, bool& out_non_float) noexcept {
        out_bytes = 4u;
        out_float_comps = 0u;
        out_non_float = true;
        switch (fmt) {
            case DXGI_FORMAT_R32_FLOAT:
                out_bytes = 4u; out_float_comps = 1u; out_non_float = false; return;
            case DXGI_FORMAT_R32G32_FLOAT:
                out_bytes = 8u; out_float_comps = 2u; out_non_float = false; return;
            case DXGI_FORMAT_R32G32B32_FLOAT:
                out_bytes = 12u; out_float_comps = 3u; out_non_float = false; return;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                out_bytes = 16u; out_float_comps = 4u; out_non_float = false; return;
            case DXGI_FORMAT_R32G32B32A32_UINT:
            case DXGI_FORMAT_R32G32B32A32_SINT:
            case DXGI_FORMAT_R32G32B32A32_TYPELESS:
                out_bytes = 16u; return;
            case DXGI_FORMAT_R32G32B32_UINT:
            case DXGI_FORMAT_R32G32B32_SINT:
            case DXGI_FORMAT_R32G32B32_TYPELESS:
                out_bytes = 12u; return;
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R32G32_SINT:
            case DXGI_FORMAT_R32G32_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM:
            case DXGI_FORMAT_R16G16B16A16_SNORM:
            case DXGI_FORMAT_R16G16B16A16_UINT:
            case DXGI_FORMAT_R16G16B16A16_SINT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                out_bytes = 8u; return;
            default:
                out_bytes = 4u; return;
        }
    };

    PsoVertexLayoutEntry entry{};
    bool found_pos = false;
    uint32_t running_off = 0u;
    for (UINT i = 0u; i < num_elems; ++i) {
        const auto& el = elems[i];
        if (el.InputSlot != 0u) continue;
        uint32_t sz = 4u, fcomps = 0u;
        bool non_flt = true;
        fmt_bytes_and_kind(el.Format, sz, fcomps, non_flt);
        const uint32_t off = (el.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT)
                                 ? running_off
                                 : el.AlignedByteOffset;
        running_off = off + sz;
        if (!found_pos && el.SemanticName && el.SemanticIndex == 0u &&
            EqualsAsciiCaseInsensitive(el.SemanticName, "POSITION") && fcomps >= 2u) {
            entry.pos_off = off;
            entry.pos_comps = fcomps;
            found_pos = true;
        }
        if (non_flt) {
            const uint32_t d0 = off >> 2u;
            const uint32_t d1 = (off + sz + 3u) >> 2u;
            for (uint32_t d = d0; d < d1 && d < 32u; ++d) {
                entry.non_float_mask |= (1u << d);
            }
        }
    }
    if (!found_pos) return;
    std::lock_guard<std::mutex> lk(g_pso_vlayout_mu);
    g_pso_vlayout_map[host_pso] = entry;
}

bool DeviceX::LookupPsoVertexLayout(
    ID3D12PipelineState* host_pso,
    uint32_t& out_pos_off,
    uint32_t& out_pos_comps,
    uint32_t& out_non_float_dword_mask) const noexcept {
    if (!host_pso) return false;
    std::lock_guard<std::mutex> lk(g_pso_vlayout_mu);
    auto it = g_pso_vlayout_map.find(host_pso);
    if (it == g_pso_vlayout_map.end()) return false;
    out_pos_off = it->second.pos_off;
    out_pos_comps = it->second.pos_comps;
    out_non_float_dword_mask = it->second.non_float_mask;
    return true;
}

bool DeviceX::PsoUsesZeroIaInputs(ID3D12PipelineState* host_pso) const noexcept {
    if (!host_pso) return false;
    std::lock_guard<std::mutex> lk(g_pso_vlayout_mu);
    return g_pso_zero_ia_set.find(host_pso) != g_pso_zero_ia_set.end();
}

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

void DeviceX::RegisterUmaShadowMapping(
    ID3D12Resource* resource, void* cpu_ptr, const D3D12_RESOURCE_DESC& desc) {
    if (!resource || !cpu_ptr) return;
    const uint64_t va = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cpu_ptr));
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
    {
        std::lock_guard<std::mutex> rlk(retail_state_mutex_);
        auto it = retail_res_states_.find(resource);
        if (it != retail_res_states_.end()) {
            st = it->second;
        }
    }
    AutoRegisterUnifiedResource(resource, va, desc, st, false, /*use_footprint_row_pitch=*/true);
}

void DeviceX::MarkUmaShadowDirty(ID3D12Resource* resource) noexcept {
    if (!resource) return;
    std::lock_guard<std::mutex> lock(unified_mutex_);
    auto it = unified_textures_.find(resource);
    if (it != unified_textures_.end() && !it->second.mirrored_by_title_va) {
        it->second.cpu_readable = true;
        if (!it->second.dirty) {
            it->second.dirty = true;
            ++it->second.page_generation;
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
        }
    }
}

void DeviceX::AutoRegisterUnifiedResource(
    ID3D12Resource* resource,
    uint64_t guest_va,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initial_state,
    bool mirrored_by_title_va,
    bool use_footprint_row_pitch) {
    if (!resource || guest_va == 0ull) return;
    guest_va = ResolveGuestCpuAliasVa(guest_va);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return;
    }

    uint64_t linear_size = 0ull;
    uint32_t row_pitch = 0u;
    const XG_TILE_MODE tile_mode = GetTrackedResourceTileMode(UnwrapResource(resource));
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        linear_size = desc.Width;
        row_pitch = static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
    } else if (tile_mode != XG_TILE_MODE_LINEAR &&
               (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
        const DXGI_FORMAT xg_fmt = MapTypelessFormatForSrv(SanitizeXboxDxgiFormat(desc.Format));
        XG_RESOURCE_LAYOUT xgl{};
        bool ok_l = false;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            XG_TEXTURE2D_DESC xg2d{};
            xg2d.Width = static_cast<uint32_t>(desc.Width);
            xg2d.Height = desc.Height;
            xg2d.MipLevels = std::max<uint32_t>(1u, desc.MipLevels);
            xg2d.ArraySize = std::max<uint32_t>(1u, desc.DepthOrArraySize);
            xg2d.Format = static_cast<uint32_t>(xg_fmt);
            xg2d.SampleCount = 1u;
            xg2d.BindFlags = XG_BIND_SHADER_RESOURCE;
            xg2d.TileMode = tile_mode;
            ok_l = openscarlett::xg::ComputeTexture2DLayout(xg2d, xgl);
        } else {
            XG_TEXTURE3D_DESC xg3d{};
            xg3d.Width = static_cast<uint32_t>(desc.Width);
            xg3d.Height = desc.Height;
            xg3d.Depth = std::max<uint32_t>(1u, desc.DepthOrArraySize);
            xg3d.MipLevels = std::max<uint32_t>(1u, desc.MipLevels);
            xg3d.Format = static_cast<uint32_t>(xg_fmt);
            xg3d.BindFlags = XG_BIND_SHADER_RESOURCE;
            xg3d.TileMode = tile_mode;
            ok_l = openscarlett::xg::ComputeTexture3DLayout(xg3d, xgl);
        }
        if (ok_l && xgl.SizeBytes > 0ull) {
            uint64_t surf_bytes = xgl.SizeBytes;
            if (retail_scarlett_abi_ && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                XG_TEXTURE2D_DESC xg2d_r{};
                xg2d_r.Width = static_cast<uint32_t>(desc.Width);
                xg2d_r.Height = desc.Height;
                xg2d_r.MipLevels = std::max<uint32_t>(1u, desc.MipLevels);
                xg2d_r.ArraySize = std::max<uint32_t>(1u, desc.DepthOrArraySize);
                xg2d_r.Format = static_cast<uint32_t>(xg_fmt);
                xg2d_r.SampleCount = 1u;
                xg2d_r.BindFlags = XG_BIND_SHADER_RESOURCE;
                xg2d_r.TileMode = tile_mode;
                RetailGfx10Layout2D rl{};
                if (ComputeRetailGfx10Layout2D(xg2d_r, xgl, rl) && rl.surf_size_bytes > surf_bytes) {
                    surf_bytes = rl.surf_size_bytes;
                }
            }
            const uint64_t max_rd = MaxReadableGuestSpan(guest_va, surf_bytes);
            linear_size = (max_rd >= 256ull && max_rd < surf_bytes) ? max_rd : surf_bytes;
            row_pitch = xgl.Mips[0].WidthElements * std::max<uint32_t>(1u, xgl.FormatInfo.bytes_per_block);
        }
    }
    if (linear_size == 0ull && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT num_rows = 0u;
        UINT64 row_bytes = 0ull;
        UINT64 total_bytes = 0ull;
        host_dev_->GetCopyableFootprints(&desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
        const bool aligned_pitch =
            (use_footprint_row_pitch || desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR) &&
            fp.Footprint.RowPitch >= row_bytes;
        row_pitch = aligned_pitch ? fp.Footprint.RowPitch : static_cast<uint32_t>(row_bytes);
        const uint32_t slices = (desc.DepthOrArraySize > 0u) ? desc.DepthOrArraySize : 1u;
        linear_size = static_cast<uint64_t>(row_pitch) * static_cast<uint64_t>(num_rows) *
                      static_cast<uint64_t>(slices);
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
    if (unified_fence_ && unified_fence_event_ &&
        unified_upload_ring_[0].alloc && unified_upload_ring_[0].cmd) {
        return S_OK;
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
    for (uint32_t i = 0u; i < kUnifiedUploadRingSlots; ++i) {
        auto& slot = unified_upload_ring_[i];
        if (!slot.alloc) {
            HRESULT hr = host_dev_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.alloc));
            if (FAILED(hr)) return hr;
        }
        if (!slot.cmd) {
            HRESULT hr = host_dev_->CreateCommandList(
                0u, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.alloc, nullptr,
                IID_PPV_ARGS(&slot.cmd));
            if (FAILED(hr)) return hr;
            slot.cmd->Close();
        }
    }
    return S_OK;
}

HRESULT DeviceX::AcquireUnifiedUploadRingSlotLocked(
    uint64_t required_bytes,
    uint32_t& out_slot_index) {
    HRESULT hr = EnsureUnifiedUploadObjectsLocked();
    if (FAILED(hr)) return hr;

    const uint32_t slot_idx = unified_upload_ring_cursor_ % kUnifiedUploadRingSlots;
    unified_upload_ring_cursor_ = (unified_upload_ring_cursor_ + 1u) % kUnifiedUploadRingSlots;
    auto& slot = unified_upload_ring_[slot_idx];

    if (slot.fence_val > 0ull && unified_fence_->GetCompletedValue() < slot.fence_val) {
        hr = unified_fence_->SetEventOnCompletion(slot.fence_val, unified_fence_event_);
        if (SUCCEEDED(hr)) {
            const DWORD wr = WaitForSingleObject(unified_fence_event_, 15000u);
            if (wr != WAIT_OBJECT_0) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
    }

    if (required_bytes > 0ull && (!slot.upload_buf || slot.capacity < required_bytes || !slot.mapped_ptr)) {
        if (slot.upload_buf) {
            if (slot.mapped_ptr) {
                D3D12_RANGE zero_write{0u, 0u};
                slot.upload_buf->Unmap(0u, &zero_write);
                slot.mapped_ptr = nullptr;
            }
            slot.upload_buf->Release();
            slot.upload_buf = nullptr;
            slot.capacity = 0ull;
        }
        const uint64_t alloc_bytes = AlignUp64(std::max<uint64_t>(required_bytes, 256ull * 1024ull), 65536ull);
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

        hr = host_dev_->CreateCommittedResource(
            &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.upload_buf));
        if (FAILED(hr) || !slot.upload_buf) {
            return FAILED(hr) ? hr : E_OUTOFMEMORY;
        }
        void* mapped = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        hr = slot.upload_buf->Map(0u, &read_none, &mapped);
        if (FAILED(hr) || !mapped) {
            slot.upload_buf->Release();
            slot.upload_buf = nullptr;
            return FAILED(hr) ? hr : E_FAIL;
        }
        slot.mapped_ptr = static_cast<uint8_t*>(mapped);
        slot.capacity = alloc_bytes;
    }

    out_slot_index = slot_idx;
    return S_OK;
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
        for (auto& slot : unified_upload_ring_) {
            if (slot.upload_buf) {
                if (slot.mapped_ptr) {
                    D3D12_RANGE zw{0u, 0u};
                    slot.upload_buf->Unmap(0u, &zw);
                    slot.mapped_ptr = nullptr;
                }
                slot.upload_buf->Release();
                slot.upload_buf = nullptr;
            }
            if (slot.cmd) { slot.cmd->Release(); slot.cmd = nullptr; }
            if (slot.alloc) { slot.alloc->Release(); slot.alloc = nullptr; }
            slot.capacity = 0ull;
            slot.fence_val = 0ull;
        }
        if (unified_fence_) { unified_fence_->Release(); unified_fence_ = nullptr; }
        if (unified_fence_event_) { CloseHandle(unified_fence_event_); unified_fence_event_ = nullptr; }
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
    static std::atomic<uint32_t> s_refresh_tick{0u};
    const uint32_t tick = s_refresh_tick.fetch_add(1u, std::memory_order_relaxed);
    const bool reprobe_reserved = ((tick & 31u) == 0u);
    const bool probe_non_ww_uploaded_tex = !retail_scarlett_abi_ || ((tick & 31u) == 0u);
    std::lock_guard<std::mutex> lock(unified_mutex_);
    for (auto& [res, m] : unified_textures_) {
        if (m.mirrored_by_title_va) continue;
        if (m.dirty) {
            any_dirty = true;
            continue;
        }
        if (m.guest_va == 0ull || m.byte_size == 0ull) {
            continue;
        }
        // Never hash or CPU->GPU upload textures actively used as RTV, DepthStencil, UAV, MSAA, or Tiled.
        if (!m.allow_cpu_upload) {
            m.cpu_readable = false;
            continue;
        }
        if (m.resource) {
            const D3D12_RESOURCE_DESC rdesc = m.resource->GetDesc();
            if (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                ((rdesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
                 rdesc.SampleDesc.Count > 1u ||
                 IsDepthOrPlanarFormatForCopy(rdesc.Format) ||
                 rdesc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE)) {
                m.cpu_readable = false;
                continue;
            }
        }
        // Single-mip R8/R8G8/A8 textures (Bink2 4K Y/cR/cB/A video planes, UI watermarks) on
        // MapViewOfFile3 physical-page CPU aliases cannot use MEM_WRITE_WATCH; probe every submit!
        const bool is_fast_planar_tex =
            (m.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && m.mip_levels <= 1u &&
             ((m.height == 1u && m.width <= 1024u) ||
              m.format == DXGI_FORMAT_R8_UNORM || m.format == DXGI_FORMAT_R8_TYPELESS ||
              m.format == DXGI_FORMAT_A8_UNORM || m.format == DXGI_FORMAT_R8G8_UNORM ||
              m.format == DXGI_FORMAT_R8G8_TYPELESS));
        if (!m.cpu_readable) {
            if (!reprobe_reserved && !is_fast_planar_tex) {
                continue;
            }
            m.guest_va = ResolveGuestCpuAliasVa(m.guest_va);
            if (!IsReadableGuestVaRange(m.guest_va, m.byte_size)) {
                continue;
            }
            m.cpu_readable = true;
            void* probe_page = nullptr;
            ULONG_PTR probe_cnt = 1u;
            DWORD probe_gran = 4096u;
            m.write_watch_enabled =
                (GetWriteWatch(0u, reinterpret_cast<void*>(static_cast<uintptr_t>(m.guest_va)),
                               static_cast<SIZE_T>(AlignUp64(m.byte_size, 4096ull)),
                               &probe_page, &probe_cnt, &probe_gran) == 0u);
            m.dirty = true;
            m.has_pending_hash = false;
            ++m.page_generation;
            dirty_unified_count_.fetch_add(1u, std::memory_order_acq_rel);
            any_dirty = true;
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
                m.has_pending_hash = false;
            }
        } else if (m.byte_size <= (64ull * 1024ull * 1024ull)) {
            if (m.dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                m.has_uploaded_once && !m.has_uploaded_nonzero && !is_fast_planar_tex) {
                uint64_t probe_q[32]{};
                bool any_nz = false;
                if (SafeCopyGuestMemory(probe_q, reinterpret_cast<const void*>(static_cast<uintptr_t>(m.guest_va)), sizeof(probe_q))) {
                    for (uint64_t q : probe_q) {
                        if (q != 0ull) { any_nz = true; break; }
                    }
                }
                if (!any_nz) continue;
            } else if (m.has_uploaded_once &&
                ((m.dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !probe_non_ww_uploaded_tex && !is_fast_planar_tex) ||
                 (retail_scarlett_abi_ && m.dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                  m.byte_size > 65536ull && ((tick & 7u) != 0u)))) {
                continue;
            }
            uint64_t cur_hash = 0ull;
            if (SafeHashGuestMemoryFnv1a64(
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(m.guest_va)),
                    static_cast<size_t>(m.byte_size),
                    cur_hash)) {
                if (!m.has_uploaded_once || cur_hash != m.last_uploaded_hash) {
                    modified = true;
                    m.pending_hash = cur_hash;
                    m.has_pending_hash = true;
                }
            } else {
                m.cpu_readable = false;
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

    const uint64_t fence_before = [&]() {
        std::lock_guard<std::mutex> lock(unified_upload_mutex_);
        return unified_fence_val_;
    }();

    HRESULT hr = S_OK;
    if (has_unified) {
        hr = FlushDirtyUnifiedTexturesInternal(sync_queue, nullptr, /*wait_for_gpu=*/false);
    }
    if (has_mirrored) {
        const HRESULT mhr = FlushMirroredTitleVaPagesOnQueue(
            sync_queue, sync_type, nullptr, /*wait_for_gpu=*/false);
        if (SUCCEEDED(hr)) hr = mhr;
    }

    // Cross-queue GPU-side ordering (Compute/Copy queue waits on Direct sync queue fence with ZERO CPU stall).
    if (sync_queue != host_queue) {
        std::lock_guard<std::mutex> lock(unified_upload_mutex_);
        if (unified_fence_ && unified_fence_val_ > fence_before) {
            (void)host_queue->Wait(unified_fence_, unified_fence_val_);
        }
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
    } else if (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
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
    return FlushDirtyUnifiedTexturesInternal(queue, out_flushed_count, /*wait_for_gpu=*/true);
}

HRESULT DeviceX::FlushDirtyUnifiedTexturesInternal(
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count,
    bool wait_for_gpu) {
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
        XG_TILE_MODE tile_mode = XG_TILE_MODE_LINEAR;
        std::vector<UINT> subresource_indices;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        std::vector<UINT> num_rows;
        std::vector<UINT64> row_sizes;
        uint64_t buffer_upload_offset = 0ull;
        uint64_t copy_bytes = 0ull;
        uint64_t snapshot_hash = 0ull;
        bool skip_gpu_copy_native_cpu_heap = false;
        bool uploaded_nonzero = true;
    };

    std::vector<PendingUpload> pending;
    {
        std::lock_guard<std::mutex> lock(unified_mutex_);
        for (auto& [res, m] : unified_textures_) {
            if (!m.dirty || !m.cpu_readable || !m.resource || m.mirrored_by_title_va) continue;
            const D3D12_RESOURCE_DESC rdesc = m.resource->GetDesc();
            // Never CPU->GPU copy onto actively bound RTVs, DepthStencil, MSAA, Depth/Planar, or Tiled textures.
            if (!m.allow_cpu_upload ||
                (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                 ((rdesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
                  rdesc.SampleDesc.Count > 1u ||
                  IsDepthOrPlanarFormatForCopy(rdesc.Format) ||
                  rdesc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE))) {
                m.dirty = false;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                continue;
            }
            PendingUpload item{};
            item.mapping = m;
            item.mapping.resource->AddRef();
            item.res_desc = rdesc;
            item.tile_mode = GetTrackedResourceTileMode(m.resource);
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
    // Cap each flush batch at 32 MiB so a burst of streaming textures never allocates a
    // 500+ MiB UPLOAD staging buffer or stalls ExecuteCommandLists for seconds.
    constexpr uint64_t kMaxBatchStagingBytes = 32ull * 1024ull * 1024ull;
    uint64_t staging_cursor = 0ull;
    size_t batch_limit = pending.size();
    for (size_t pi = 0u; pi < pending.size(); ++pi) {
        auto& item = pending[pi];
        if (item.skip_gpu_copy_native_cpu_heap) {
            item.copy_bytes = item.mapping.byte_size;
            continue;
        }
        const uint64_t prev_cursor = staging_cursor;
        staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            const uint64_t sz = std::min<uint64_t>(item.mapping.byte_size, item.res_desc.Width);
            if (prev_cursor > 0ull && staging_cursor + sz > kMaxBatchStagingBytes) {
                staging_cursor = prev_cursor;
                batch_limit = pi;
                break;
            }
            if (sz > kMaxBatchStagingBytes) {
                staging_cursor = prev_cursor;
                item.copy_bytes = 0ull;
                continue;
            }
            item.buffer_upload_offset = staging_cursor;
            item.copy_bytes = sz;
            staging_cursor += item.copy_bytes;
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            const uint32_t slices =
                (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                    ? 1u
                    : ((item.res_desc.DepthOrArraySize > 0u) ? item.res_desc.DepthOrArraySize : 1u);
            const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
            const uint32_t mip_limit =
                (item.tile_mode != XG_TILE_MODE_LINEAR &&
                 (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                  item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D))
                    ? std::min<uint32_t>(mips, XG_MAX_MIP_LEVELS)
                    : 1u;
            uint64_t total_copy = 0ull;
            bool footprint_ok = true;
            D3D12_RESOURCE_DESC copy_desc = item.res_desc;
            copy_desc.Format = MapTypelessFormatForSrv(copy_desc.Format);
            for (uint32_t s = 0u; s < slices && footprint_ok; ++s) {
                for (uint32_t mip = 0u; mip < mip_limit; ++mip) {
                    staging_cursor = AlignUp64(staging_cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                    UINT nr = 0u;
                    UINT64 rs = 0ull;
                    UINT64 sub_total = 0ull;
                    const UINT sub_idx = s * mips + mip;
                    host_dev_->GetCopyableFootprints(
                        &copy_desc, sub_idx, 1u, staging_cursor, &fp, &nr, &rs, &sub_total);
                    fp.Footprint.Format = copy_desc.Format;
                    if (sub_total == 0ull || sub_total == UINT64_MAX ||
                        nr == 0u || nr == UINT_MAX ||
                        rs == 0ull || rs == UINT64_MAX) {
                        if (mip == 0u) footprint_ok = false;
                        break;
                    }
                    item.subresource_indices.push_back(sub_idx);
                    item.footprints.push_back(fp);
                    item.num_rows.push_back(nr);
                    item.row_sizes.push_back(rs);
                    staging_cursor += sub_total;
                    total_copy += rs * static_cast<uint64_t>(nr);
                }
            }
            if (item.footprints.empty()) footprint_ok = false;
            if (prev_cursor > 0ull && staging_cursor > kMaxBatchStagingBytes) {
                staging_cursor = prev_cursor;
                item.footprints.clear();
                item.copy_bytes = 0ull;
                batch_limit = pi;
                break;
            }
            if (staging_cursor - prev_cursor > kMaxBatchStagingBytes) {
                staging_cursor = prev_cursor;
                footprint_ok = false;
            }
            item.copy_bytes = footprint_ok ? total_copy : 0ull;
            if (!footprint_ok) {
                item.footprints.clear();
            }
        }
    }
    if (batch_limit < pending.size()) {
        for (size_t ri = batch_limit; ri < pending.size(); ++ri) {
            if (pending[ri].mapping.resource) {
                pending[ri].mapping.resource->Release();
                pending[ri].mapping.resource = nullptr;
            }
        }
        pending.resize(batch_limit);
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
    uint32_t ring_slot_idx = 0u;
    HRESULT hr = AcquireUnifiedUploadRingSlotLocked(staging_cursor, ring_slot_idx);
    if (FAILED(hr)) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
        return hr;
    }
    auto& ring_slot = unified_upload_ring_[ring_slot_idx];
    uint8_t* dst_base = (staging_cursor > 0ull) ? ring_slot.mapped_ptr : nullptr;
    if (staging_cursor > 0ull && !dst_base) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, E_FAIL);
        return E_FAIL;
    }

    std::vector<size_t> uploaded_indices;
    std::vector<size_t> gpu_copy_indices;
    std::vector<size_t> skipped_unreadable_indices;
    uploaded_indices.reserve(pending.size());
    gpu_copy_indices.reserve(pending.size());

    for (size_t idx = 0; idx < pending.size(); ++idx) {
        auto& item = pending[idx];
        if (item.copy_bytes == 0ull ||
            !IsReadableGuestVaRange(item.mapping.guest_va, item.mapping.byte_size)) {
            skipped_unreadable_indices.push_back(idx);
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
        if (item.mapping.has_pending_hash) {
            item.snapshot_hash = item.mapping.pending_hash;
        } else if (!item.mapping.write_watch_enabled &&
                   item.mapping.byte_size <= (64ull * 1024ull * 1024ull)) {
            uint64_t snap_h = 0ull;
            if (!SafeHashGuestMemoryFnv1a64(src_base, static_cast<size_t>(item.mapping.byte_size), snap_h)) {
                skipped_unreadable_indices.push_back(idx);
                continue;
            }
            item.snapshot_hash = snap_h;
        }

        if (item.skip_gpu_copy_native_cpu_heap) {
            uploaded_indices.push_back(idx);
            continue;
        }
        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (!SafeCopyGuestMemory(dst_base + item.buffer_upload_offset, src_base,
                                     static_cast<size_t>(item.copy_bytes))) {
                skipped_unreadable_indices.push_back(idx);
                continue;
            }
            uploaded_indices.push_back(idx);
            gpu_copy_indices.push_back(idx);
        } else if (item.tile_mode != XG_TILE_MODE_LINEAR &&
                   (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
            const DXGI_FORMAT xg_fmt = MapTypelessFormatForSrv(SanitizeXboxDxgiFormat(item.res_desc.Format));
            const uint32_t mips = std::max<uint32_t>(1u, item.res_desc.MipLevels);
            XG_TEXTURE2D_DESC xg2d{};
            XG_TEXTURE3D_DESC xg3d{};
            XG_RESOURCE_LAYOUT xgl{};
            bool layout_ok = false;
            if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                xg2d.Width = static_cast<uint32_t>(item.res_desc.Width);
                xg2d.Height = item.res_desc.Height;
                xg2d.MipLevels = mips;
                xg2d.ArraySize = std::max<uint32_t>(1u, item.res_desc.DepthOrArraySize);
                xg2d.Format = static_cast<uint32_t>(xg_fmt);
                xg2d.SampleCount = 1u;
                xg2d.BindFlags = XG_BIND_SHADER_RESOURCE;
                xg2d.TileMode = item.tile_mode;
                layout_ok = openscarlett::xg::ComputeTexture2DLayout(xg2d, xgl);
            } else {
                xg3d.Width = static_cast<uint32_t>(item.res_desc.Width);
                xg3d.Height = item.res_desc.Height;
                xg3d.Depth = std::max<uint32_t>(1u, item.res_desc.DepthOrArraySize);
                xg3d.MipLevels = mips;
                xg3d.Format = static_cast<uint32_t>(xg_fmt);
                xg3d.BindFlags = XG_BIND_SHADER_RESOURCE;
                xg3d.TileMode = item.tile_mode;
                layout_ok = openscarlett::xg::ComputeTexture3DLayout(xg3d, xgl);
            }
            uint64_t target_surf_bytes = xgl.SizeBytes;
            if (layout_ok && retail_scarlett_abi_ &&
                item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                RetailGfx10Layout2D rl{};
                if (ComputeRetailGfx10Layout2D(xg2d, xgl, rl) && rl.surf_size_bytes > target_surf_bytes) {
                    target_surf_bytes = rl.surf_size_bytes;
                }
            }
            const uint64_t avail_src =
                layout_ok ? MaxReadableGuestSpan(item.mapping.guest_va, target_surf_bytes) : 0ull;
            bool untile_any_ok = false;
            if (layout_ok && avail_src >= 64ull) {
                std::vector<uint8_t> tiled_snap(static_cast<size_t>(avail_src));
                if (SafeCopyGuestMemory(tiled_snap.data(), src_base, tiled_snap.size())) {
                    bool any_nz = false;
                    const size_t probe_bytes = std::min<size_t>(tiled_snap.size() & ~7ull, 4096u);
                    for (size_t po = 0u; po < probe_bytes; po += 8u) {
                        uint64_t qw = 0ull;
                        std::memcpy(&qw, tiled_snap.data() + po, 8u);
                        if (qw != 0ull) { any_nz = true; break; }
                    }
                    item.uploaded_nonzero = any_nz;
                    std::vector<UINT> valid_subs;
                    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> valid_fps;
                    for (size_t k = 0u; k < item.footprints.size() && k < item.subresource_indices.size(); ++k) {
                        const UINT sub_idx = item.subresource_indices[k];
                        const uint32_t mip = sub_idx % mips;
                        const uint32_t slice = sub_idx / mips;
                        uint8_t* dst_sub = dst_base + item.footprints[k].Offset;
                        const uint32_t dst_rp = item.footprints[k].Footprint.RowPitch;
                        bool ok_sub = false;
                        if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                            if (retail_scarlett_abi_) {
                                ok_sub = UntileRetailGfx10Surface2D(
                                    xg2d, xgl, mip, slice, tiled_snap.data(), tiled_snap.size(), dst_sub, dst_rp);
                            } else {
                                ok_sub = openscarlett::xg::UntileSurface2DPlane(
                                    xg2d, 0u, mip, slice, tiled_snap.data(), tiled_snap.size(), dst_sub, dst_rp);
                            }
                        } else {
                            const uint32_t dst_sp = dst_rp * item.num_rows[k];
                            ok_sub = openscarlett::xg::UntileSurface3D(
                                xg3d, mip, tiled_snap.data(), tiled_snap.size(), dst_sub, dst_rp, dst_sp);
                        }
                        if (ok_sub) {
                            valid_subs.push_back(sub_idx);
                            valid_fps.push_back(item.footprints[k]);
                            untile_any_ok = true;
                        }
                    }
                    if (untile_any_ok) {
                        item.subresource_indices = std::move(valid_subs);
                        item.footprints = std::move(valid_fps);
                        static std::atomic<uint32_t> s_untile_up_log{0u};
                        const uint32_t un = s_untile_up_log.fetch_add(1u, std::memory_order_relaxed);
                        if (un < 24u) {
                            std::fprintf(stderr,
                                         "[OpenScarlett][UNTILE-UPLOAD] #%u %llux%ux%u subs=%zu fmt=%d tm=%u va=0x%llX nz=%d\n",
                                         un, static_cast<unsigned long long>(item.res_desc.Width),
                                         item.res_desc.Height, item.res_desc.DepthOrArraySize,
                                         item.footprints.size(), static_cast<int>(xg_fmt),
                                         static_cast<unsigned>(item.tile_mode),
                                         static_cast<unsigned long long>(item.mapping.guest_va),
                                         any_nz ? 1 : 0);
                            std::fflush(stderr);
                        }
                    }
                }
            }
            if (untile_any_ok) {
                uploaded_indices.push_back(idx);
                gpu_copy_indices.push_back(idx);
            } else {
                // Fall back to linear copy when untiling could not cover the subresource
                // (e.g. CPU-linear 3D color LUTs or sub-tile linear surfaces).
                const size_t slices = std::min<size_t>(item.footprints.size(), 1u);
                bool lin_ok = (slices > 0u);
                for (size_t s = 0u; s < slices && lin_ok; ++s) {
                    const uint64_t row_bytes = item.row_sizes[s];
                    const uint64_t dst_rp = item.footprints[s].Footprint.RowPitch;
                    uint8_t* dst_slice = dst_base + item.footprints[s].Offset;
                    const UINT depth =
                        (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                         item.footprints[s].Footprint.Depth > 0u)
                            ? item.footprints[s].Footprint.Depth
                            : 1u;
                    const uint64_t total_rows = static_cast<uint64_t>(depth) * item.num_rows[s];
                    if (!IsReadableGuestVaRange(item.mapping.guest_va, total_rows * row_bytes)) {
                        lin_ok = false;
                        break;
                    }
                    for (uint64_t r = 0ull; r < total_rows; ++r) {
                        if (!SafeCopyGuestMemory(dst_slice + r * dst_rp, src_base + r * row_bytes,
                                                 static_cast<size_t>(row_bytes))) {
                            lin_ok = false;
                            break;
                        }
                    }
                }
                if (lin_ok) {
                    item.subresource_indices.resize(1u);
                    item.footprints.resize(1u);
                    uploaded_indices.push_back(idx);
                    gpu_copy_indices.push_back(idx);
                } else {
                    skipped_unreadable_indices.push_back(idx);
                }
            }
        } else if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                   item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            const size_t slices = item.footprints.size();
            uint64_t src_slice_offset = 0ull;
            bool copy_ok = true;
            for (size_t s = 0; s < slices && copy_ok; ++s) {
                const uint64_t row_bytes = item.row_sizes[s];
                uint64_t src_row_pitch =
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
                const uint64_t total_rows = static_cast<uint64_t>(depth) * item.num_rows[s];
                uint64_t eff_readable_bytes = item.mapping.byte_size;
                // Auto-detect 256B-aligned row pitch (e.g. 2048B vs 1920B for 1080p chroma planes)
                // when GetCopyableFootprints pitch was used by the CPU producer without ROW_MAJOR layout.
                if (retail_scarlett_abi_ && s == 0u && total_rows >= 2u &&
                    src_row_pitch == row_bytes && dst_row_pitch > row_bytes &&
                    (row_bytes & 7ull) == 0ull && ((dst_row_pitch - row_bytes) & 7ull) == 0ull) {
                    const uint64_t padded_span = dst_row_pitch * (total_rows - 1u) + row_bytes;
                    if (IsReadableGuestVaRange(item.mapping.guest_va, padded_span)) {
                        auto any_nonzero_64 = [](const uint8_t* p, uint64_t len) noexcept -> bool {
                            uint64_t acc = 0ull;
                            for (uint64_t o = 0ull; o + 8ull <= len; o += 8ull) {
                                uint64_t w = 0ull;
                                std::memcpy(&w, p + o, sizeof(w));
                                if (w != 0ull) { acc = 1ull; break; }
                            }
                            return acc != 0ull;
                        };
                        const uint64_t pad_len = dst_row_pitch - row_bytes;
                        const bool row0_has_data = any_nonzero_64(src_base, std::min<uint64_t>(row_bytes, 256ull));
                        const bool pad0_zero = !any_nonzero_64(src_base + row_bytes, pad_len);
                        const bool pad1_zero = !any_nonzero_64(src_base + dst_row_pitch + row_bytes, pad_len);
                        const bool last_row_has_data =
                            any_nonzero_64(src_base + dst_row_pitch * (total_rows - 1u),
                                           std::min<uint64_t>(row_bytes, 256ull));
                        if (row0_has_data && pad0_zero && pad1_zero && last_row_has_data) {
                            src_row_pitch = dst_row_pitch;
                            eff_readable_bytes = std::max<uint64_t>(eff_readable_bytes, padded_span);
                        }
                    }
                }
                const uint64_t slice_bytes = src_row_pitch * total_rows;
                const uint64_t copy_bytes =
                    (total_rows > 0u) ? (dst_row_pitch * (total_rows - 1u) + row_bytes) : 0ull;
                if (src_row_pitch == dst_row_pitch && total_rows > 0u &&
                    src_slice_offset <= eff_readable_bytes &&
                    copy_bytes <= eff_readable_bytes - src_slice_offset) {
                    if (!SafeCopyGuestMemory(
                            dst_slice, src_base + src_slice_offset, static_cast<size_t>(copy_bytes))) {
                        copy_ok = false;
                    }
                } else {
                    for (UINT z = 0u; z < depth && copy_ok; ++z) {
                        for (UINT y = 0u; y < item.num_rows[s]; ++y) {
                            const uint64_t row_idx = static_cast<uint64_t>(z) * item.num_rows[s] + y;
                            const uint64_t src_off = src_slice_offset + row_idx * src_row_pitch;
                            if (src_off + row_bytes <= eff_readable_bytes) {
                                if (!SafeCopyGuestMemory(
                                        dst_slice + static_cast<size_t>(row_idx) * static_cast<size_t>(dst_row_pitch),
                                        src_base + src_off,
                                        static_cast<size_t>(row_bytes))) {
                                    copy_ok = false;
                                    break;
                                }
                            }
                        }
                    }
                }
                src_slice_offset += slice_bytes;
            }
            if (copy_ok) {
                if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    item.res_desc.Width >= 960u &&
                    (item.res_desc.Format == DXGI_FORMAT_R8_UNORM ||
                     item.res_desc.Format == DXGI_FORMAT_R8_TYPELESS)) {
                    static const bool s_log_r8 = []() {
                        char b[16]{};
                        return ::GetEnvironmentVariableA("OPENSCARLETT_CAPTURE_DIR", b, sizeof(b)) > 0;
                    }();
                    if (s_log_r8) {
                        static std::atomic<uint32_t> s_r8_cnt{0u};
                        const uint32_t rn = s_r8_cnt.fetch_add(1u, std::memory_order_relaxed);
                        if (rn < 12u || (rn % 121u) == 0u) {
                            const uint8_t b0 = dst_base[item.footprints[0].Offset];
                            const size_t mid_off = item.footprints[0].Offset +
                                static_cast<size_t>(item.num_rows[0] / 2u) * item.footprints[0].Footprint.RowPitch +
                                static_cast<size_t>(item.row_sizes[0] / 2u);
                            const uint8_t bmid = dst_base[mid_off];
                            std::fprintf(stderr,
                                         "[OpenScarlett][d3d12_xs] R8-UPLOAD #%u res=%p %llux%u va=0x%llX b0=0x%02X mid=0x%02X hash=0x%llX\n",
                                         rn, static_cast<void*>(item.mapping.resource),
                                         static_cast<unsigned long long>(item.res_desc.Width),
                                         item.res_desc.Height,
                                         static_cast<unsigned long long>(item.mapping.guest_va),
                                         static_cast<unsigned>(b0), static_cast<unsigned>(bmid),
                                         static_cast<unsigned long long>(item.snapshot_hash));
                            std::fflush(stderr);
                        }
                    }
                }
                uploaded_indices.push_back(idx);
                gpu_copy_indices.push_back(idx);
            }
        }
    }

    if (uploaded_indices.empty()) {
        release_pending();
        RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, S_OK);
        return S_OK;
    }

    uint64_t bytes_uploaded = 0ull;
    if (!gpu_copy_indices.empty()) {
        hr = ring_slot.alloc->Reset();
        if (SUCCEEDED(hr)) {
            hr = ring_slot.cmd->Reset(ring_slot.alloc, nullptr);
        }
        if (FAILED(hr)) {
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
            return hr;
        }

        for (size_t idx : gpu_copy_indices) {
            const auto& item = pending[idx];
            D3D12_RESOURCE_STATES st =
                retail_scarlett_abi_
                    ? GetRetailResourceState(item.mapping.resource, item.mapping.current_state)
                    : item.mapping.current_state;
            if (retail_scarlett_abi_) {
                if (item.res_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                    constexpr UINT kTexAllowed =
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_READ) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
                        static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    st = static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(st) & kTexAllowed);
                } else {
                    st = static_cast<D3D12_RESOURCE_STATES>(static_cast<UINT>(st) & 0x0FFFu);
                }
            }
            if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = item.mapping.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = st;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                ring_slot.cmd->ResourceBarrier(1u, &b);
            }

            if (item.res_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                ring_slot.cmd->CopyBufferRegion(
                    item.mapping.resource, 0ull,
                    ring_slot.upload_buf, item.buffer_upload_offset,
                    item.copy_bytes);
            } else {
                const uint32_t mips = (item.res_desc.MipLevels > 0u) ? item.res_desc.MipLevels : 1u;
                for (size_t s = 0; s < item.footprints.size(); ++s) {
                    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                    dst_loc.pResource = item.mapping.resource;
                    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst_loc.SubresourceIndex =
                        (s < item.subresource_indices.size())
                            ? item.subresource_indices[s]
                            : static_cast<UINT>(s) * mips;

                    D3D12_TEXTURE_COPY_LOCATION src_loc{};
                    src_loc.pResource = ring_slot.upload_buf;
                    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src_loc.PlacedFootprint = item.footprints[s];

                    ring_slot.cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
                }
            }

            if (st != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = item.mapping.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = st;
                ring_slot.cmd->ResourceBarrier(1u, &b);
            }
            bytes_uploaded += item.copy_bytes;
        }

        hr = ring_slot.cmd->Close();
        if (FAILED(hr)) {
            if (CommandListX::IsDiagTraceEnabled()) {
                std::fprintf(stderr, "[OpenScarlett][UNIFIED-CLOSE-FAIL] hr=0x%08lX n=%zu\n",
                             static_cast<unsigned long>(hr), gpu_copy_indices.size());
                std::fflush(stderr);
            }
            release_pending();
            RecordDev("ID3D12DeviceX::FlushDirtyUnifiedTextures", -1, hr);
            return hr;
        }

        ID3D12CommandList* cmd_lists[] = {ring_slot.cmd};
        host_queue->ExecuteCommandLists(1u, cmd_lists);
        if (CommandListX::IsDiagTraceEnabled() && host_dev_) {
            const HRESULT urem = host_dev_->GetDeviceRemovedReason();
            if (FAILED(urem)) {
                std::fprintf(stderr, "[OpenScarlett][UNIFIED-REMOVED] rem=0x%08lX n=%zu bytes=%llu\n",
                             static_cast<unsigned long>(urem), gpu_copy_indices.size(),
                             static_cast<unsigned long long>(bytes_uploaded));
                std::fflush(stderr);
            }
        }

        const uint64_t fence_target = ++unified_fence_val_;
        hr = host_queue->Signal(unified_fence_, fence_target);
        ring_slot.fence_val = fence_target;
        if (wait_for_gpu && SUCCEEDED(hr) && unified_fence_->GetCompletedValue() < fence_target) {
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
                it->second.has_pending_hash = false;
                it->second.uploaded_generation = item.mapping.page_generation;
                it->second.has_uploaded_once = true;
                if (item.uploaded_nonzero) {
                    it->second.has_uploaded_nonzero = true;
                }
                it->second.last_uploaded_hash = item.snapshot_hash;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                ++flushed_count;
            }
        }
        for (size_t idx : skipped_unreadable_indices) {
            const auto& item = pending[idx];
            auto it = unified_textures_.find(item.mapping.resource);
            if (it != unified_textures_.end() &&
                it->second.dirty &&
                it->second.page_generation == item.mapping.page_generation) {
                it->second.dirty = false;
                it->second.cpu_readable = false;
                dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
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

    // For CPU-accessible upload/custom-L0 buffers, establish a single persistent Map() once
    // at creation time so AlignRootCbvGpuVaForDesktop never calls GetHeapProperties/Map/Unmap
    // per draw call.
    const uint8_t* mapped_cpu = nullptr;
    bool owns_map = false;
    D3D12_HEAP_PROPERTIES hp{};
    if (SUCCEEDED(host_res->GetHeapProperties(&hp, nullptr)) &&
        (hp.Type == D3D12_HEAP_TYPE_UPLOAD ||
         (hp.Type == D3D12_HEAP_TYPE_CUSTOM &&
          hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE))) {
        void* p = nullptr;
        D3D12_RANGE zero_read{0u, 0u};
        if (SUCCEEDED(host_res->Map(0u, &zero_read, &p)) && p) {
            mapped_cpu = static_cast<const uint8_t*>(p);
            owns_map = true;
        }
    }

    std::unique_lock<std::shared_mutex> lock(buffer_va_mutex_);
    auto existing = buffer_va_map_.find(host_res);
    if (existing != buffer_va_map_.end()) {
        if (existing->second.owns_persistent_map && existing->second.mapped_cpu_ptr) {
            D3D12_RANGE no_write{0u, 0u};
            host_res->Unmap(0u, &no_write);
        }
        if (existing->second.gpu_va != 0ull) {
            auto git = buffer_va_by_gpu_.find(existing->second.gpu_va);
            if (git != buffer_va_by_gpu_.end() && git->second == host_res) {
                buffer_va_by_gpu_.erase(git);
            }
        }
    }
    buffer_va_map_[host_res] = TrackedBufferVa{gpu_va, desc.Width, guest_va, mapped_cpu, owns_map};
    if (gpu_va != 0ull) {
        buffer_va_by_gpu_[gpu_va] = host_res;
    }
    buffer_va_gen_.fetch_add(1ull, std::memory_order_release);
}

ID3D12Resource* DeviceX::ResolveBufferResourceByGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes) const {
    if (out_offset_bytes) *out_offset_bytes = 0ull;
    if (va == 0ull) return nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS translated = TranslateGuestGpuVa(va);
    std::shared_lock<std::shared_mutex> lock(buffer_va_mutex_);
    auto git = buffer_va_by_gpu_.upper_bound(translated);
    if (git != buffer_va_by_gpu_.begin()) {
        --git;
        auto mit = buffer_va_map_.find(git->second);
        if (mit != buffer_va_map_.end() &&
            translated >= mit->second.gpu_va &&
            translated < mit->second.gpu_va + mit->second.size_bytes) {
            if (out_offset_bytes) *out_offset_bytes = translated - mit->second.gpu_va;
            return git->second;
        }
    }
    for (const auto& [res, entry] : buffer_va_map_) {
        if (entry.guest_va != 0ull && va >= entry.guest_va &&
            va < entry.guest_va + entry.size_bytes) {
            if (out_offset_bytes) *out_offset_bytes = va - entry.guest_va;
            return res;
        }
    }
    return nullptr;
}

void DeviceX::TrackPlacedTextureVa(ID3D12Resource* host_res, uint64_t guest_va) {
    if (!host_res || guest_va == 0ull) return;
    std::lock_guard<std::mutex> lock(placed_tex_va_mutex_);
    auto existing = placed_tex_res_to_va_.find(host_res);
    if (existing != placed_tex_res_to_va_.end()) {
        auto vit = placed_tex_by_va_.find(existing->second);
        if (vit != placed_tex_by_va_.end()) {
            auto& vec = vit->second;
            vec.erase(std::remove(vec.begin(), vec.end(), host_res), vec.end());
            if (vec.empty()) placed_tex_by_va_.erase(vit);
        }
    }
    placed_tex_res_to_va_[host_res] = guest_va;
    placed_tex_by_va_[guest_va].push_back(host_res);
}

ID3D12Resource* DeviceX::ResolvePlacedTextureByGpuVa(
    uint64_t guest_va, const D3D12_RESOURCE_DESC* expected_desc) const {
    if (guest_va == 0ull) return nullptr;
    std::lock_guard<std::mutex> lock(placed_tex_va_mutex_);
    auto lookup_at = [&](uint64_t va) -> ID3D12Resource* {
        auto it = placed_tex_by_va_.find(va);
        if (it == placed_tex_by_va_.end() || it->second.empty()) return nullptr;
        const auto& vec = it->second;
        if (expected_desc) {
            const DXGI_FORMAT exp_san = SanitizeXboxDxgiFormat(expected_desc->Format);
            // Pass 1: exact Dimension + Width + Height + Format match (preferring latest created).
            for (auto rit = vec.rbegin(); rit != vec.rend(); ++rit) {
                ID3D12Resource* cand = *rit;
                if (!cand) continue;
                const D3D12_RESOURCE_DESC cd = cand->GetDesc();
                if (cd.Dimension == expected_desc->Dimension &&
                    cd.Width == expected_desc->Width &&
                    cd.Height == expected_desc->Height &&
                    cd.Format == exp_san) {
                    return cand;
                }
            }
            // Pass 2: exact Dimension + Width + Height match (typeless/SRGB view alias).
            for (auto rit = vec.rbegin(); rit != vec.rend(); ++rit) {
                ID3D12Resource* cand = *rit;
                if (!cand) continue;
                const D3D12_RESOURCE_DESC cd = cand->GetDesc();
                if (cd.Dimension == expected_desc->Dimension &&
                    cd.Width == expected_desc->Width &&
                    cd.Height == expected_desc->Height) {
                    return cand;
                }
            }
            return nullptr;
        }
        return vec.back();
    };
    if (ID3D12Resource* r = lookup_at(guest_va)) return r;
    const uint64_t cpu_alias = ResolveGuestCpuAliasVa(guest_va);
    if (cpu_alias != guest_va) {
        if (ID3D12Resource* r = lookup_at(cpu_alias)) return r;
    }
    // RDNA2 SQ_IMG_RSRC (T#) BASE_ADDRESS is 256-byte aligned, whereas sub-allocated linear
    // UMA textures (e.g. Bink2 YUV planes at +0x60 header offset) may be placed at a sub-256B
    // offset inside that 256B window. Match exact Dimension + Width + Height within [va, va+256).
    if (expected_desc && (guest_va & 0xFFull) == 0ull) {
        const DXGI_FORMAT exp_san = SanitizeXboxDxgiFormat(expected_desc->Format);
        auto scan_window = [&](uint64_t base_256) -> ID3D12Resource* {
            ID3D12Resource* dim_match = nullptr;
            for (const auto& [kva, vec] : placed_tex_by_va_) {
                if (kva <= base_256 || kva >= base_256 + 256ull || vec.empty()) continue;
                for (auto rit = vec.rbegin(); rit != vec.rend(); ++rit) {
                    ID3D12Resource* cand = *rit;
                    if (!cand) continue;
                    const D3D12_RESOURCE_DESC cd = cand->GetDesc();
                    if (cd.Dimension == expected_desc->Dimension &&
                        cd.Width == expected_desc->Width &&
                        cd.Height == expected_desc->Height) {
                        if (cd.Format == exp_san) return cand;
                        if (!dim_match) dim_match = cand;
                    }
                }
            }
            return dim_match;
        };
        if (ID3D12Resource* r = scan_window(guest_va)) return r;
        if (cpu_alias != guest_va && (cpu_alias & 0xFFull) == 0ull) {
            if (ID3D12Resource* r = scan_window(cpu_alias)) return r;
        }
    }
    return nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::UploadTransientGuestBufferForDesktop(
    D3D12_GPU_VIRTUAL_ADDRESS guest_va,
    uint64_t size_bytes,
    uint64_t alignment,
    bool allow_same_frame_cache) {
    if (guest_va == 0ull || size_bytes == 0ull || !host_dev_) {
        return 0ull;
    }
    guest_va = ResolveGuestCpuAliasVa(guest_va);
    constexpr uint64_t kRingBytes = 256ull * 1024ull * 1024ull;
    constexpr uint64_t kMaxSingleUpload = 16ull * 1024ull * 1024ull;
    const uint64_t copy_bytes = std::min<uint64_t>(size_bytes, kMaxSingleUpload);
    const uint64_t align = std::max<uint64_t>(alignment ? alignment : 16ull, 16ull);
    const uint64_t cur_frame = next_frame_token_.load(std::memory_order_relaxed);
    uint64_t live_hash = 0ull;
    const bool have_live_hash =
        allow_same_frame_cache &&
        SafeHashGuestMemoryFnv1a64(
            reinterpret_cast<const void*>(static_cast<uintptr_t>(guest_va)),
            static_cast<size_t>(copy_bytes),
            live_hash);

    std::lock_guard<std::mutex> lk(transient_ring_mutex_);
    if (allow_same_frame_cache && have_live_hash && transient_ring_cpu_ && transient_ring_gpu_ != 0ull) {
        for (const auto& s : transient_ring_cache_) {
            if (s.frame_token == cur_frame &&
                s.raw_start == guest_va &&
                s.byte_len == copy_bytes &&
                s.content_hash == live_hash &&
                s.host_gpu_start >= transient_ring_gpu_ &&
                (s.host_gpu_start & (align - 1ull)) == 0ull) {
                return s.host_gpu_start;
            }
        }
    }

    if (!transient_ring_cpu_) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kRingBytes;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* ring = nullptr;
        if (SUCCEEDED(host_dev_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&ring))) &&
            ring) {
            void* p = nullptr;
            D3D12_RANGE zero_read{0u, 0u};
            if (SUCCEEDED(ring->Map(0u, &zero_read, &p)) && p) {
                transient_ring_ = ring;
                transient_ring_cpu_ = static_cast<uint8_t*>(p);
                transient_ring_gpu_ = ring->GetGPUVirtualAddress();
                transient_ring_cursor_ = 0ull;
            } else {
                ring->Release();
            }
        }
    }
    if (!transient_ring_cpu_ || transient_ring_gpu_ == 0ull) {
        return 0ull;
    }

    const uint64_t alloc_bytes = AlignUp64(copy_bytes, align);
    uint64_t off = AlignUp64(transient_ring_cursor_, align);
    if (off + alloc_bytes > kRingBytes) {
        off = 0ull;
    }
    uint8_t* dst = transient_ring_cpu_ + off;
    if (!SafeCopyGuestMemory(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(guest_va)),
                             static_cast<size_t>(copy_bytes))) {
        return 0ull;
    }
    if (alloc_bytes > copy_bytes) {
        std::memset(dst + copy_bytes, 0, static_cast<size_t>(alloc_bytes - copy_bytes));
    }
    transient_ring_cursor_ = off + alloc_bytes;
    const D3D12_GPU_VIRTUAL_ADDRESS out_gpu = transient_ring_gpu_ + off;
    if (allow_same_frame_cache && have_live_hash) {
        transient_ring_cache_[transient_ring_cache_next_ & 63u] =
            TransientRingSlice{guest_va, copy_bytes, live_hash, out_gpu, cur_frame};
        ++transient_ring_cache_next_;
    }
    return out_gpu;
}

void DeviceX::RegisterGuestBackedDescHeap(
    ID3D12DescriptorHeap* host_heap,
    uint64_t host_gpu_start, uint64_t host_cpu_start, uint32_t host_num_descs,
    uint32_t inc_size, uint64_t guest_cpu_start, uint64_t guest_byte_size,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    if (!host_heap || host_gpu_start == 0ull || guest_cpu_start == 0ull || guest_byte_size == 0ull) return;
    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    const uint32_t ring_base =
        (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && host_num_descs >= 262144u) ? 65536u : 0u;
    guest_backed_desc_heaps_.push_back(GuestBackedDescHeap{
        host_heap, host_gpu_start, host_cpu_start, host_num_descs,
        std::max<uint32_t>(1u, inc_size), guest_cpu_start, guest_byte_size, ring_base, false, heap_type});
    has_guest_backed_desc_heaps_.store(true, std::memory_order_release);
}

bool DeviceX::EnsureShadowDescHeapLocked() noexcept {
    if (shadow_cbv_srv_uav_heap_) return true;
    if (!host_dev_) return false;
    constexpr uint32_t kShadowCap = 2097152u;
    constexpr uint32_t kStaticPartitionEnd = 524288u;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kShadowCap;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* sh = nullptr;
    if (FAILED(host_dev_->CreateDescriptorHeap(
            &hd, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&sh))) ||
        !sh) {
        return false;
    }
    shadow_cbv_srv_uav_heap_ = sh;
    shadow_cbv_srv_uav_cpu_start_ = sh->GetCPUDescriptorHandleForHeapStart();
    shadow_cbv_srv_uav_inc_ =
        std::max<uint32_t>(1u, host_dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    shadow_cbv_srv_uav_cap_ = kShadowCap;
    shadow_static_cursor_ = 3u;
    shadow_dynamic_cursor_ = kStaticPartitionEnd;
    shadow_slot_keys_.assign(kShadowCap, Desc32Key{});
    shadow_slot_valid_.assign(kShadowCap, 0u);
    // Reserve slots 0..2 for valid null SRV / CBV / UAV descriptors.
    shadow_null_srv_cpu_ = shadow_cbv_srv_uav_cpu_start_;
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
    null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_srv.Texture2D.MipLevels = 1u;
    host_dev_->CreateShaderResourceView(nullptr, &null_srv, shadow_null_srv_cpu_);

    shadow_null_cbv_cpu_.ptr = shadow_cbv_srv_uav_cpu_start_.ptr + shadow_cbv_srv_uav_inc_;
    host_dev_->CreateConstantBufferView(nullptr, shadow_null_cbv_cpu_);

    shadow_null_uav_cpu_.ptr = shadow_cbv_srv_uav_cpu_start_.ptr + 2ull * shadow_cbv_srv_uav_inc_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav{};
    null_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    host_dev_->CreateUnorderedAccessView(nullptr, nullptr, &null_uav, shadow_null_uav_cpu_);
    return true;
}

uint32_t DeviceX::AllocShadowSlotLocked(bool prefer_static_partition) noexcept {
    constexpr uint32_t kStaticStart = 3u;
    constexpr uint32_t kStaticEnd = 524288u;
    uint32_t slot = 3u;
    if (prefer_static_partition && shadow_cbv_srv_uav_cap_ > kStaticEnd) {
        slot = shadow_static_cursor_;
        shadow_static_cursor_ = (slot + 1u < kStaticEnd) ? (slot + 1u) : kStaticStart;
    } else {
        const uint32_t dyn_start = (shadow_cbv_srv_uav_cap_ > kStaticEnd) ? kStaticEnd : 3u;
        slot = shadow_dynamic_cursor_;
        if (slot < dyn_start || slot >= shadow_cbv_srv_uav_cap_) slot = dyn_start;
        shadow_dynamic_cursor_ = (slot + 1u < shadow_cbv_srv_uav_cap_) ? (slot + 1u) : dyn_start;
    }
    if (slot < shadow_slot_valid_.size() && shadow_slot_valid_[slot]) {
        const uint64_t slot_cpu =
            shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<uint64_t>(slot) * shadow_cbv_srv_uav_inc_;
        auto it = desc32_to_shadow_cpu_.find(shadow_slot_keys_[slot]);
        if (it != desc32_to_shadow_cpu_.end() && it->second == slot_cpu) {
            desc32_to_shadow_cpu_.erase(it);
        }
        shadow_slot_valid_[slot] = 0u;
    }
    return slot;
}

void DeviceX::CheckCpuDeviceRemoved(
    const char* api_name, uint64_t detail0, uint64_t detail1) noexcept {
    if (!host_dev_) return;
    static std::atomic<bool> s_reported{false};
    if (s_reported.load(std::memory_order_relaxed)) return;
    const HRESULT rem = host_dev_->GetDeviceRemovedReason();
    if (rem == S_OK) return;
    if (!s_reported.exchange(true, std::memory_order_acq_rel)) {
        void* frames[10]{};
        const USHORT n = ::RtlCaptureStackBackTrace(1u, 10u, frames, nullptr);
        std::fprintf(stderr,
                     "[OpenScarlett][CPU-DEV-REMOVED] api=%s rem=0x%08lX d0=0x%llX d1=0x%llX stack=",
                     api_name ? api_name : "?",
                     static_cast<unsigned long>(rem),
                     static_cast<unsigned long long>(detail0),
                     static_cast<unsigned long long>(detail1));
        for (USHORT i = 0u; i < n; ++i) {
            std::fprintf(stderr, "%s%p", i ? "<-" : "", frames[i]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
}

bool DeviceX::IsShaderVisibleHostDescHandle(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type) const noexcept {
    if (handle.ptr == 0u) return false;
    const uint64_t ptr = static_cast<uint64_t>(handle.ptr);
    if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        std::lock_guard<std::mutex> plk(paged_desc_mutex_);
        for (const auto& [h, ps] : paged_desc_heaps_) {
            (void)h;
            const uint64_t base = static_cast<uint64_t>(ps.cpu_start.ptr);
            const uint64_t end = base + static_cast<uint64_t>(ps.num_descriptors) * ps.inc_size;
            if (ptr >= base && ptr < end) {
                return true;
            }
        }
    }
    return false;
}

bool DeviceX::RedirectGuestDescDestIfNeeded(
    D3D12_CPU_DESCRIPTOR_HANDLE& dst, uint64_t& out_guest_writeback_addr,
    bool prefer_static_partition) {
    out_guest_writeback_addr = 0ull;
    if (dst.ptr == 0u) return false;
    const bool in_sv_host =
        IsShaderVisibleHostDescHandle(dst, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    bool in_guest = false;
    bool in_guest_host_backing = false;
    for (const auto& g : guest_backed_desc_heaps_) {
        if (g.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            if (dst.ptr >= g.guest_cpu_start && dst.ptr < g.guest_cpu_start + g.guest_byte_size) {
                in_guest = true;
                break;
            }
            const uint64_t h_end = g.host_cpu_start + static_cast<uint64_t>(g.host_num_descs) * g.inc_size;
            if (g.host_cpu_start != 0ull && dst.ptr >= g.host_cpu_start && dst.ptr < h_end) {
                in_guest_host_backing = true;
            }
        }
    }
    if (!in_guest && !in_sv_host && !in_guest_host_backing) return false;
    if (!EnsureShadowDescHeapLocked()) return false;
    const uint32_t slot = AllocShadowSlotLocked(prefer_static_partition);
    constexpr uint64_t kSvHostDestTag = (1ull << 63);
    out_guest_writeback_addr =
        in_guest ? static_cast<uint64_t>(dst.ptr)
                 : (static_cast<uint64_t>(dst.ptr) | kSvHostDestTag);
    dst.ptr = shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_;
    return true;
}

void DeviceX::SnapshotCreatedDescriptorToShadow(
    D3D12_CPU_DESCRIPTOR_HANDLE created_cpu_handle, uint64_t guest_writeback_addr,
    bool prefer_static_partition, uint64_t* out_shadow_ptr) {
    if (out_shadow_ptr) *out_shadow_ptr = 0ull;
    if (created_cpu_handle.ptr == 0u || !host_dev_) return;
    constexpr uint64_t kSvHostDestTag = (1ull << 63);
    const bool is_sv_host_dest = (guest_writeback_addr & kSvHostDestTag) != 0ull;
    const uint64_t raw_wb_addr = guest_writeback_addr & ~kSvHostDestTag;

    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    if (!EnsureShadowDescHeapLocked()) return;
    Desc32Key key{};
    const size_t copy_len = std::min<size_t>(sizeof(key.bytes), shadow_cbv_srv_uav_inc_);
    if (!SafeCopyGuestMemory(key.bytes, reinterpret_cast<const void*>(created_cpu_handle.ptr), copy_len)) {
        return;
    }

    if (is_sv_host_dest && raw_wb_addr != 0ull) {
        const D3D12_CPU_DESCRIPTOR_HANDLE sv_dst{static_cast<SIZE_T>(raw_wb_addr)};
        host_dev_->CopyDescriptorsSimple(
            1u, sv_dst, created_cpu_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CheckCpuDeviceRemoved("SnapshotToSvHostDest", sv_dst.ptr, created_cpu_handle.ptr);
        sv_cpu_to_shadow_cpu_[raw_wb_addr] = created_cpu_handle.ptr;
        bool all_zero = true;
        for (uint8_t b : key.bytes) {
            if (b != 0u) { all_zero = false; break; }
        }
        if (!all_zero) {
            const uint64_t rel =
                (created_cpu_handle.ptr >= shadow_cbv_srv_uav_cpu_start_.ptr)
                    ? (created_cpu_handle.ptr - shadow_cbv_srv_uav_cpu_start_.ptr) / shadow_cbv_srv_uav_inc_
                    : UINT64_MAX;
            if (rel < shadow_slot_keys_.size()) {
                shadow_slot_keys_[static_cast<size_t>(rel)] = key;
                shadow_slot_valid_[static_cast<size_t>(rel)] = 1u;
            }
            desc32_to_shadow_cpu_[key] = created_cpu_handle.ptr;
            if (out_shadow_ptr) *out_shadow_ptr = created_cpu_handle.ptr;
        }
        return;
    }

    bool all_zero = true;
    for (uint8_t b : key.bytes) {
        if (b != 0u) { all_zero = false; break; }
    }
    if (all_zero) return;

    if (raw_wb_addr != 0ull) {
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(raw_wb_addr)),
                    key.bytes, copy_len);
        const uint64_t rel =
            (created_cpu_handle.ptr >= shadow_cbv_srv_uav_cpu_start_.ptr)
                ? (created_cpu_handle.ptr - shadow_cbv_srv_uav_cpu_start_.ptr) / shadow_cbv_srv_uav_inc_
                : UINT64_MAX;
        if (rel < shadow_slot_keys_.size()) {
            shadow_slot_keys_[static_cast<size_t>(rel)] = key;
            shadow_slot_valid_[static_cast<size_t>(rel)] = 1u;
        }
        desc32_to_shadow_cpu_[key] = created_cpu_handle.ptr;
        if (out_shadow_ptr) *out_shadow_ptr = created_cpu_handle.ptr;
        return;
    }
    auto ex_it = desc32_to_shadow_cpu_.find(key);
    if (ex_it != desc32_to_shadow_cpu_.end()) {
        if (out_shadow_ptr) *out_shadow_ptr = ex_it->second;
        return;
    }
    const uint32_t slot = AllocShadowSlotLocked(prefer_static_partition);
    const D3D12_CPU_DESCRIPTOR_HANDLE shadow_dst{
        shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
    host_dev_->CopyDescriptorsSimple(
        1u, shadow_dst, created_cpu_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CheckCpuDeviceRemoved("SnapshotStagingToShadow", shadow_dst.ptr, created_cpu_handle.ptr);
    shadow_slot_keys_[slot] = key;
    shadow_slot_valid_[slot] = 1u;
    desc32_to_shadow_cpu_[key] = shadow_dst.ptr;
    if (out_shadow_ptr) *out_shadow_ptr = shadow_dst.ptr;
}

void DeviceX::RegisterShadowSrvUavPair(
    uint64_t shadow_ptr, ID3D12Resource* host_res, bool created_is_uav, DXGI_FORMAT view_fmt) {
    if (shadow_ptr == 0ull || !host_dev_) return;
    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    if (!EnsureShadowDescHeapLocked()) return;
    if (created_is_uav) {
        uav_shadow_ptrs_.insert(shadow_ptr);
        if (host_res != nullptr && host_res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            shadow_to_tex_res_[shadow_ptr] = host_res;
        }
    } else if (host_res != nullptr) {
        if (host_res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            shadow_to_tex_res_[shadow_ptr] = host_res;
        }
        std::lock_guard<std::mutex> dlk(g_diag_srv_mu);
        g_diag_sh_to_res[shadow_ptr] = host_res;
    }
    if (created_is_uav) {
        if (!host_res) return;
        const D3D12_RESOURCE_DESC rd = host_res->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || rd.SampleDesc.Count > 1u ||
            (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
            (rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
            return;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = ResolveCompatibleSrvFormat(rd.Format, view_fmt);
        if (srv.Format == DXGI_FORMAT_UNKNOWN || IsDepthOrPlanarFormatForCopy(srv.Format)) return;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
        fs.Format = srv.Format;
        if (FAILED(host_dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
            (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) == 0) {
            return;
        }
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const UINT mips = std::max<UINT>(1u, rd.MipLevels);
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MipLevels = mips;
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            srv.Texture1D.MipLevels = mips;
        } else if (rd.DepthOrArraySize > 1u) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = mips;
            srv.Texture2DArray.ArraySize = rd.DepthOrArraySize;
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = mips;
        }
        const uint32_t slot = AllocShadowSlotLocked(/*prefer_static_partition=*/true);
        const D3D12_CPU_DESCRIPTOR_HANDLE sh_srv{
            shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
        host_dev_->CreateShaderResourceView(host_res, &srv, sh_srv);
        CheckCpuDeviceRemoved("RegisterShadowSrvUavPair[SRV]", reinterpret_cast<uint64_t>(host_res), srv.Format);
        uav_to_srv_shadow_[shadow_ptr] = sh_srv.ptr;
        shadow_to_tex_res_[sh_srv.ptr] = host_res;
    } else if (host_res != nullptr) {
        const D3D12_RESOURCE_DESC rd = host_res->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || rd.SampleDesc.Count > 1u ||
            (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ||
            (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
            return;
        }
        // D3D12 strictly requires UAV format == resource format for typed resources, and a concrete
        // non-TYPELESS, non-SRGB format from the same family when the resource format is TYPELESS.
        const DXGI_FORMAT mapped_from_res = MapTypelessFormatForSrv(rd.Format);
        const bool res_is_typeless = (mapped_from_res != rd.Format);
        DXGI_FORMAT uav_fmt = res_is_typeless ? mapped_from_res : rd.Format;
        if (uav_fmt == DXGI_FORMAT_UNKNOWN || uav_fmt == rd.Format && res_is_typeless ||
            uav_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            uav_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            uav_fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
            IsDepthOrPlanarFormatForCopy(uav_fmt) || FormatForbidsDesktopUav(uav_fmt)) {
            return;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
        fs.Format = uav_fmt;
        if (FAILED(host_dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
            (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
            return;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = uav_fmt;
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.WSize = rd.DepthOrArraySize;
        } else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        } else if (rd.DepthOrArraySize > 1u) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.ArraySize = rd.DepthOrArraySize;
        } else {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        const uint32_t slot = AllocShadowSlotLocked(/*prefer_static_partition=*/true);
        const D3D12_CPU_DESCRIPTOR_HANDLE sh_uav{
            shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
        host_dev_->CreateUnorderedAccessView(host_res, nullptr, &uav, sh_uav);
        CheckCpuDeviceRemoved("RegisterShadowSrvUavPair[UAV]", reinterpret_cast<uint64_t>(host_res), uav.Format);
        srv_to_uav_shadow_[shadow_ptr] = sh_uav.ptr;
        shadow_to_tex_res_[sh_uav.ptr] = host_res;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE DeviceX::TranslateGuestSamplerCpuHandle(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    ID3D12DescriptorHeap** out_shadow_src,
    D3D12_CPU_DESCRIPTOR_HANDLE* out_shadow_slot) noexcept {
    if (out_shadow_src) *out_shadow_src = nullptr;
    if (out_shadow_slot) out_shadow_slot->ptr = 0u;
    if (handle.ptr == 0u || !host_dev_) return handle;

    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    for (const auto& g : guest_backed_desc_heaps_) {
        if (g.heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) continue;
        if (handle.ptr >= g.guest_cpu_start && handle.ptr < g.guest_cpu_start + g.guest_byte_size) {
            const uint64_t slot = (handle.ptr - g.guest_cpu_start) / g.inc_size;
            const uint32_t clamped =
                (g.host_num_descs > 0u)
                    ? static_cast<uint32_t>(std::min<uint64_t>(slot, g.host_num_descs - 1u))
                    : 0u;
            if ((out_shadow_src || out_shadow_slot) && !shadow_sampler_heap_) {
                D3D12_DESCRIPTOR_HEAP_DESC shd{};
                shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
                shd.NumDescriptors = 2048u;
                shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                ID3D12DescriptorHeap* sh = nullptr;
                if (SUCCEEDED(host_dev_->CreateDescriptorHeap(
                        &shd, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&sh))) &&
                    sh) {
                    shadow_sampler_heap_ = sh;
                    shadow_sampler_cpu_start_ = sh->GetCPUDescriptorHandleForHeapStart();
                    shadow_sampler_inc_ =
                        std::max<uint32_t>(1u, host_dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
                    shadow_sampler_cap_ = 2048u;
                    D3D12_SAMPLER_DESC def_samp{};
                    def_samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    def_samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    def_samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    def_samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    def_samp.MaxAnisotropy = 1u;
                    def_samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                    def_samp.MaxLOD = D3D12_FLOAT32_MAX;
                    for (uint32_t si = 0u; si < shadow_sampler_cap_; ++si) {
                        host_dev_->CreateSampler(
                            &def_samp,
                            D3D12_CPU_DESCRIPTOR_HANDLE{
                                shadow_sampler_cpu_start_.ptr + static_cast<SIZE_T>(si) * shadow_sampler_inc_});
                    }
                }
            }
            if (shadow_sampler_heap_ && clamped < shadow_sampler_cap_) {
                if (out_shadow_src) *out_shadow_src = shadow_sampler_heap_;
                if (out_shadow_slot) {
                    out_shadow_slot->ptr =
                        shadow_sampler_cpu_start_.ptr + static_cast<SIZE_T>(clamped) * shadow_sampler_inc_;
                }
            }
            return D3D12_CPU_DESCRIPTOR_HANDLE{
                static_cast<SIZE_T>(g.host_cpu_start + static_cast<uint64_t>(clamped) * g.inc_size)};
        }
    }
    return handle;
}

bool DeviceX::TryCopyGuestBackedDescriptorsSimple(
    UINT num_descs,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_start,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    if (num_descs == 0u || dst_start.ptr == 0u || src_start.ptr == 0u || !host_dev_) {
        return false;
    }

    const UINT inc = std::max<UINT>(1u, host_dev_->GetDescriptorHandleIncrementSize(heap_type));
    const bool src0_sv = IsShaderVisibleHostDescHandle(src_start, heap_type);
    const bool dst0_sv = IsShaderVisibleHostDescHandle(dst_start, heap_type);
    const bool src_end_sv =
        (num_descs > 1u)
            ? IsShaderVisibleHostDescHandle(
                  D3D12_CPU_DESCRIPTOR_HANDLE{src_start.ptr + static_cast<SIZE_T>(num_descs - 1u) * inc},
                  heap_type)
            : src0_sv;
    const bool dst_end_sv =
        (num_descs > 1u)
            ? IsShaderVisibleHostDescHandle(
                  D3D12_CPU_DESCRIPTOR_HANDLE{dst_start.ptr + static_cast<SIZE_T>(num_descs - 1u) * inc},
                  heap_type)
            : dst0_sv;

    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    auto find_guest_heap = [&](uint64_t addr) -> const GuestBackedDescHeap* {
        for (const auto& g : guest_backed_desc_heaps_) {
            if (g.heap_type == heap_type &&
                addr >= g.guest_cpu_start &&
                addr < g.guest_cpu_start + g.guest_byte_size) {
                return &g;
            }
        }
        return nullptr;
    };
    auto is_guest_host_backing = [&](uint64_t addr) -> bool {
        for (const auto& g : guest_backed_desc_heaps_) {
            const uint64_t h_end = g.host_cpu_start + static_cast<uint64_t>(g.host_num_descs) * g.inc_size;
            if (g.heap_type == heap_type && g.host_cpu_start != 0ull &&
                addr >= g.host_cpu_start && addr < h_end) {
                return true;
            }
        }
        return false;
    };

    const GuestBackedDescHeap* dst_g0 = find_guest_heap(dst_start.ptr);
    const GuestBackedDescHeap* src_g0 = find_guest_heap(src_start.ptr);
    const GuestBackedDescHeap* dst_gend =
        (num_descs > 1u) ? find_guest_heap(dst_start.ptr + static_cast<uint64_t>(num_descs - 1u) * inc) : dst_g0;
    const GuestBackedDescHeap* src_gend =
        (num_descs > 1u) ? find_guest_heap(src_start.ptr + static_cast<uint64_t>(num_descs - 1u) * inc) : src_g0;
    if (!dst_g0 && !src_g0 && !dst_gend && !src_gend &&
        !src0_sv && !src_end_sv && !dst0_sv && !dst_end_sv &&
        !is_guest_host_backing(src_start.ptr) && !is_guest_host_backing(dst_start.ptr)) {
        return false;
    }

    if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        if (!shadow_sampler_heap_) {
            D3D12_DESCRIPTOR_HEAP_DESC shd{};
            shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            shd.NumDescriptors = 2048u;
            shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ID3D12DescriptorHeap* sh = nullptr;
            if (SUCCEEDED(host_dev_->CreateDescriptorHeap(
                    &shd, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&sh))) &&
                sh) {
                shadow_sampler_heap_ = sh;
                shadow_sampler_cpu_start_ = sh->GetCPUDescriptorHandleForHeapStart();
                shadow_sampler_inc_ = inc;
                shadow_sampler_cap_ = 2048u;
                D3D12_SAMPLER_DESC def_samp{};
                def_samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                def_samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                def_samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                def_samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                def_samp.MaxAnisotropy = 1u;
                def_samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                def_samp.MaxLOD = D3D12_FLOAT32_MAX;
                for (uint32_t si = 0u; si < shadow_sampler_cap_; ++si) {
                    host_dev_->CreateSampler(
                        &def_samp,
                        D3D12_CPU_DESCRIPTOR_HANDLE{
                            shadow_sampler_cpu_start_.ptr + static_cast<SIZE_T>(si) * shadow_sampler_inc_});
                }
            }
        }
        for (UINT i = 0u; i < num_descs; ++i) {
            const uint64_t d_addr = dst_start.ptr + static_cast<uint64_t>(i) * inc;
            const uint64_t s_addr = src_start.ptr + static_cast<uint64_t>(i) * inc;
            const GuestBackedDescHeap* dg = find_guest_heap(d_addr);
            const GuestBackedDescHeap* sg = find_guest_heap(s_addr);
            D3D12_CPU_DESCRIPTOR_HANDLE eff_src{static_cast<SIZE_T>(s_addr)};
            if (sg) {
                const uint32_t s_slot = static_cast<uint32_t>(
                    std::min<uint64_t>((s_addr - sg->guest_cpu_start) / sg->inc_size,
                                       std::max<uint32_t>(1u, sg->host_num_descs) - 1u));
                if (!shadow_sampler_heap_ || s_slot >= shadow_sampler_cap_) continue;
                eff_src.ptr = shadow_sampler_cpu_start_.ptr + static_cast<SIZE_T>(s_slot) * shadow_sampler_inc_;
            }
            if (dg) {
                const uint32_t d_slot = static_cast<uint32_t>(
                    std::min<uint64_t>((d_addr - dg->guest_cpu_start) / dg->inc_size,
                                       std::max<uint32_t>(1u, dg->host_num_descs) - 1u));
                const D3D12_CPU_DESCRIPTOR_HANDLE eff_dst{
                    static_cast<SIZE_T>(dg->host_cpu_start + static_cast<uint64_t>(d_slot) * dg->inc_size)};
                host_dev_->CopyDescriptorsSimple(1u, eff_dst, eff_src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                if (shadow_sampler_heap_ && d_slot < shadow_sampler_cap_) {
                    const D3D12_CPU_DESCRIPTOR_HANDLE sh_dst{
                        shadow_sampler_cpu_start_.ptr + static_cast<SIZE_T>(d_slot) * shadow_sampler_inc_};
                    if (sh_dst.ptr != eff_src.ptr) {
                        host_dev_->CopyDescriptorsSimple(1u, sh_dst, eff_src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    }
                }
            } else {
                host_dev_->CopyDescriptorsSimple(
                    1u, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(d_addr)}, eff_src,
                    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            }
        }
        return true;
    }

    if (!EnsureShadowDescHeapLocked()) {
        return true;
    }
    const size_t copy_len = std::min<size_t>(sizeof(Desc32Key::bytes), shadow_cbv_srv_uav_inc_);
    for (UINT i = 0u; i < num_descs; ++i) {
        const uint64_t d_addr = dst_start.ptr + static_cast<uint64_t>(i) * inc;
        const uint64_t s_addr = src_start.ptr + static_cast<uint64_t>(i) * inc;
        const bool d_guest = (find_guest_heap(d_addr) != nullptr);
        const bool s_guest = (find_guest_heap(s_addr) != nullptr);

        if (d_guest && s_guest) {
            if (d_addr == s_addr) continue;
            Desc32Key tmp{};
            if (SafeCopyGuestMemory(tmp.bytes, reinterpret_cast<const void*>(static_cast<uintptr_t>(s_addr)), copy_len) &&
                IsReadableGuestVaRange(d_addr, copy_len)) {
                std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(d_addr)), tmp.bytes, copy_len);
            }
        } else if (d_guest && !s_guest) {
            auto sv_it = sv_cpu_to_shadow_cpu_.find(s_addr);
            const uint64_t readable_src =
                (sv_it != sv_cpu_to_shadow_cpu_.end()) ? sv_it->second : s_addr;
            Desc32Key key{};
            if (!SafeCopyGuestMemory(key.bytes, reinterpret_cast<const void*>(static_cast<uintptr_t>(readable_src)), copy_len)) {
                continue;
            }
            bool all_zero = true;
            for (uint8_t b : key.bytes) {
                if (b != 0u) { all_zero = false; break; }
            }
            auto ex_d_it = !all_zero ? desc32_to_shadow_cpu_.find(key) : desc32_to_shadow_cpu_.end();
            if (!all_zero && ex_d_it != desc32_to_shadow_cpu_.end()) {
                auto cbv_it = staging_cbv_sources_.find(s_addr);
                if (cbv_it != staging_cbv_sources_.end()) {
                    cbv_shadow_ptrs_.insert(ex_d_it->second);
                    shadow_cbv_sources_[ex_d_it->second] = cbv_it->second;
                }
                auto bsrv_it = staging_buf_srv_sources_.find(s_addr);
                if (bsrv_it != staging_buf_srv_sources_.end()) {
                    shadow_buf_srv_sources_[ex_d_it->second] = bsrv_it->second;
                }
            } else if (!all_zero &&
                sv_it == sv_cpu_to_shadow_cpu_.end() && !is_guest_host_backing(s_addr) &&
                !IsShaderVisibleHostDescHandle(D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(s_addr)}, heap_type)) {
                const uint32_t slot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                const D3D12_CPU_DESCRIPTOR_HANDLE shadow_dst{
                    shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
                host_dev_->CopyDescriptorsSimple(
                    1u, shadow_dst, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(s_addr)},
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                shadow_slot_keys_[slot] = key;
                shadow_slot_valid_[slot] = 1u;
                desc32_to_shadow_cpu_[key] = shadow_dst.ptr;
                auto cbv_it = staging_cbv_sources_.find(s_addr);
                if (cbv_it != staging_cbv_sources_.end()) {
                    cbv_shadow_ptrs_.insert(shadow_dst.ptr);
                    shadow_cbv_sources_[shadow_dst.ptr] = cbv_it->second;
                }
                auto bsrv_it = staging_buf_srv_sources_.find(s_addr);
                if (bsrv_it != staging_buf_srv_sources_.end()) {
                    shadow_buf_srv_sources_[shadow_dst.ptr] = bsrv_it->second;
                }
            }
            if (IsReadableGuestVaRange(d_addr, copy_len)) {
                std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(d_addr)), key.bytes, copy_len);
            }
        } else if (!d_guest && s_guest) {
            Desc32Key key{};
            D3D12_CPU_DESCRIPTOR_HANDLE eff_src = shadow_null_srv_cpu_;
            if (SafeCopyGuestMemory(key.bytes, reinterpret_cast<const void*>(static_cast<uintptr_t>(s_addr)), copy_len)) {
                auto it = desc32_to_shadow_cpu_.find(key);
                if (it != desc32_to_shadow_cpu_.end()) {
                    eff_src.ptr = static_cast<SIZE_T>(it->second);
                }
            }
            if (eff_src.ptr != 0u) {
                host_dev_->CopyDescriptorsSimple(
                    1u, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(d_addr)}, eff_src,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                sv_cpu_to_shadow_cpu_[d_addr] = eff_src.ptr;
            }
        } else {
            D3D12_CPU_DESCRIPTOR_HANDLE eff_src{static_cast<SIZE_T>(s_addr)};
            auto sv_it = sv_cpu_to_shadow_cpu_.find(s_addr);
            if (sv_it != sv_cpu_to_shadow_cpu_.end()) {
                eff_src.ptr = static_cast<SIZE_T>(sv_it->second);
            } else if (is_guest_host_backing(s_addr) ||
                       IsShaderVisibleHostDescHandle(eff_src, heap_type)) {
                // Never pass a SHADER_VISIBLE host CPU handle as CopyDescriptorsSimple source!
                eff_src = shadow_null_srv_cpu_;
            }
            uint64_t record_shadow_for_dst = eff_src.ptr;
            if (sv_it == sv_cpu_to_shadow_cpu_.end() && eff_src.ptr == static_cast<SIZE_T>(s_addr)) {
                // Staging (FLAG_NONE) -> SHADER_VISIBLE host heap: reuse existing canonical shadow
                // slot if SnapshotCreatedDescriptorToShadow already recorded this 32B key (preserving
                // uav_shadow_ptrs_, srv_to_uav_shadow_, uav_to_srv_shadow_, and shadow_cbv_sources_).
                Desc32Key pre_key{};
                const bool have_pre_key = SafeCopyGuestMemory(
                    pre_key.bytes, reinterpret_cast<const void*>(eff_src.ptr), copy_len);
                auto ex_it = have_pre_key ? desc32_to_shadow_cpu_.find(pre_key) : desc32_to_shadow_cpu_.end();
                if (ex_it != desc32_to_shadow_cpu_.end()) {
                    record_shadow_for_dst = ex_it->second;
                    auto cbv_it = staging_cbv_sources_.find(s_addr);
                    if (cbv_it != staging_cbv_sources_.end()) {
                        cbv_shadow_ptrs_.insert(record_shadow_for_dst);
                        shadow_cbv_sources_[record_shadow_for_dst] = cbv_it->second;
                    }
                    auto bsrv_it = staging_buf_srv_sources_.find(s_addr);
                    if (bsrv_it != staging_buf_srv_sources_.end()) {
                        shadow_buf_srv_sources_[record_shadow_for_dst] = bsrv_it->second;
                    }
                } else {
                    const uint32_t slot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                    const D3D12_CPU_DESCRIPTOR_HANDLE shadow_dst{
                        shadow_cbv_srv_uav_cpu_start_.ptr + static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
                    host_dev_->CopyDescriptorsSimple(
                        1u, shadow_dst, eff_src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    Desc32Key key{};
                    if (SafeCopyGuestMemory(key.bytes, reinterpret_cast<const void*>(shadow_dst.ptr), copy_len)) {
                        shadow_slot_keys_[slot] = key;
                        shadow_slot_valid_[slot] = 1u;
                        desc32_to_shadow_cpu_[key] = shadow_dst.ptr;
                        auto cbv_it = staging_cbv_sources_.find(s_addr);
                        if (cbv_it != staging_cbv_sources_.end()) {
                            cbv_shadow_ptrs_.insert(shadow_dst.ptr);
                            shadow_cbv_sources_[shadow_dst.ptr] = cbv_it->second;
                        }
                        auto bsrv_it = staging_buf_srv_sources_.find(s_addr);
                        if (bsrv_it != staging_buf_srv_sources_.end()) {
                            shadow_buf_srv_sources_[shadow_dst.ptr] = bsrv_it->second;
                        }
                    }
                    record_shadow_for_dst = shadow_dst.ptr;
                }
            }
            if (eff_src.ptr != 0u) {
                host_dev_->CopyDescriptorsSimple(
                    1u,
                    D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(d_addr)},
                    eff_src,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                sv_cpu_to_shadow_cpu_[d_addr] = record_shadow_for_dst;
            }
        }
    }
    return true;
}

bool DeviceX::HostFormatSupportsTypedUav(DXGI_FORMAT fmt) const noexcept {
    if (!host_dev_ || fmt == DXGI_FORMAT_UNKNOWN) return false;
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return false;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
            return true;
        default:
            break;
    }
    const uint32_t u = static_cast<uint32_t>(fmt);
    if ((u >= static_cast<uint32_t>(DXGI_FORMAT_BC1_TYPELESS) &&
         u <= static_cast<uint32_t>(DXGI_FORMAT_BC5_SNORM)) ||
        (u >= static_cast<uint32_t>(DXGI_FORMAT_BC6H_TYPELESS) &&
         u <= static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM_SRGB))) {
        return false;
    }
    if (u >= 256u) return false;
    static std::atomic<int8_t> s_uav_fmt_cache[256]{};
    const int8_t cached = s_uav_fmt_cache[u].load(std::memory_order_relaxed);
    if (cached != 0) {
        return cached > 0;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    const bool ok =
        SUCCEEDED(host_dev_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
        (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    s_uav_fmt_cache[u].store(ok ? int8_t{1} : int8_t{-1}, std::memory_order_relaxed);
    return ok;
}

void DeviceX::RecordRootSigTableSizes(void* host_rs, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    if (!host_rs) return;
    constexpr uint32_t kSamplerTableFlag = 0x80000000u;
    constexpr uint32_t kNonTableFlag = 0x40000000u;
    constexpr uint32_t kValidTableFlag = 0x20000000u;
    constexpr uint32_t kSlotCountMask = 0x0FFFFFFFu;
    std::vector<uint32_t> counts;
    std::vector<RsS2MirrorInfo> mirrors;
    if ((desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ||
         static_cast<uint32_t>(desc.Version) == 3u) &&
        desc.Desc_1_1.pParameters) {
        counts.assign(desc.Desc_1_1.NumParameters, kNonTableFlag);
        mirrors.resize(desc.Desc_1_1.NumParameters);
        for (UINT pi = 0u; pi < desc.Desc_1_1.NumParameters; ++pi) {
            const auto& rp = desc.Desc_1_1.pParameters[pi];
            if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                !rp.DescriptorTable.pDescriptorRanges) {
                continue;
            }
            UINT cur = 0u;
            UINT max_end = 0u;
            UINT native_max_end = 0u;
            uint32_t cbv_off = 0u;
            uint32_t uav_off = 0u;
            bool is_sampler = false;
            for (UINT ri = 0u; ri < rp.DescriptorTable.NumDescriptorRanges; ++ri) {
                const auto& rg = rp.DescriptorTable.pDescriptorRanges[ri];
                if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                    is_sampler = true;
                }
                const UINT off = (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                                     ? cur
                                     : rg.OffsetInDescriptorsFromTableStart;
                const bool is_s2_mirror_range =
                    ((rg.RegisterSpace == 2u || rg.RegisterSpace == 98u) &&
                     rg.BaseShaderRegister == 0u && rg.NumDescriptors == 16u && off >= 16u);
                if (is_s2_mirror_range && rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
                    cbv_off = off;
                } else if (is_s2_mirror_range && rg.RegisterSpace == 2u && rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
                    uav_off = off;
                }
                if (rg.NumDescriptors == UINT_MAX) {
                    const UINT end = std::min<UINT>(off + 64u, 128u);
                    max_end = std::max<UINT>(max_end, end);
                    if (!is_s2_mirror_range) native_max_end = std::max<UINT>(native_max_end, end);
                    cur = off + 64u;
                } else {
                    const UINT end = off + rg.NumDescriptors;
                    max_end = std::max<UINT>(max_end, end);
                    if (!is_s2_mirror_range) native_max_end = std::max<UINT>(native_max_end, end);
                    cur = end;
                }
            }
            counts[pi] = kValidTableFlag | (max_end & kSlotCountMask) |
                         (is_sampler ? kSamplerTableFlag : 0u);
            mirrors[pi].cbv_off = cbv_off;
            mirrors[pi].uav_off = uav_off;
            mirrors[pi].native_slots = std::clamp<uint32_t>(native_max_end > 0u ? native_max_end : max_end, 1u, 512u);
        }
    } else if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0 && desc.Desc_1_0.pParameters) {
        counts.assign(desc.Desc_1_0.NumParameters, kNonTableFlag);
        mirrors.resize(desc.Desc_1_0.NumParameters);
        for (UINT pi = 0u; pi < desc.Desc_1_0.NumParameters; ++pi) {
            const auto& rp = desc.Desc_1_0.pParameters[pi];
            if (rp.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
                !rp.DescriptorTable.pDescriptorRanges) {
                continue;
            }
            UINT cur = 0u;
            UINT max_end = 0u;
            bool is_sampler = false;
            for (UINT ri = 0u; ri < rp.DescriptorTable.NumDescriptorRanges; ++ri) {
                const auto& rg = rp.DescriptorTable.pDescriptorRanges[ri];
                if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                    is_sampler = true;
                }
                const UINT off = (rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                                     ? cur
                                     : rg.OffsetInDescriptorsFromTableStart;
                if (rg.NumDescriptors == UINT_MAX) {
                    max_end = std::max<UINT>(max_end, std::min<UINT>(off + 64u, 128u));
                    cur = off + 64u;
                } else {
                    const UINT end = off + rg.NumDescriptors;
                    max_end = std::max<UINT>(max_end, end);
                    cur = end;
                }
            }
            counts[pi] = kValidTableFlag | (max_end & kSlotCountMask) |
                         (is_sampler ? kSamplerTableFlag : 0u);
            mirrors[pi].native_slots = std::clamp<uint32_t>(max_end, 1u, 512u);
        }
    }
    {
        std::lock_guard<std::mutex> lk(guest_dh_mutex_);
        rs_table_slot_counts_[host_rs] = std::move(counts);
        rs_s2_mirrors_[host_rs] = std::move(mirrors);
    }
}

bool DeviceX::IsSamplerRootTableParam(void* host_rs, UINT root_param_index) const noexcept {
    bool is_samp = false;
    (void)IsValidRootDescriptorTableParam(host_rs, root_param_index, &is_samp);
    return is_samp;
}

bool DeviceX::IsValidRootDescriptorTableParam(
    void* host_rs, UINT root_param_index, bool* out_is_sampler) const noexcept {
    if (out_is_sampler) *out_is_sampler = false;
    if (!host_rs) return true;
    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    auto it = rs_table_slot_counts_.find(host_rs);
    if (it == rs_table_slot_counts_.end()) {
        return true;
    }
    if (root_param_index >= it->second.size()) {
        return false;
    }
    const uint32_t raw = it->second[root_param_index];
    if ((raw & 0x40000000u) != 0u) {
        return false;
    }
    if (out_is_sampler) {
        *out_is_sampler = (raw & 0x80000000u) != 0u;
    }
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE DeviceX::MaterializeGuestDescriptorTable(
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    ID3D12RootSignature* bound_host_rs,
    UINT root_param_index,
    uint32_t* out_copied_hits,
    uint32_t* out_slot_count,
    bool is_compute) {
    if (out_copied_hits) *out_copied_hits = 0u;
    if (out_slot_count) *out_slot_count = 0u;
    if (gpu_handle.ptr == 0ull || !host_dev_ ||
        !has_guest_backed_desc_heaps_.load(std::memory_order_acquire)) {
        return gpu_handle;
    }

    std::lock_guard<std::mutex> lk(guest_dh_mutex_);
    GuestBackedDescHeap* target = nullptr;
    for (auto& g : guest_backed_desc_heaps_) {
        if (g.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            gpu_handle.ptr >= g.host_gpu_start &&
            gpu_handle.ptr < g.host_gpu_start + g.guest_byte_size) {
            target = &g;
            break;
        }
    }
    if (!target || target->host_num_descs == 0u) {
        return gpu_handle;
    }

    const uint64_t byte_off = gpu_handle.ptr - target->host_gpu_start;
    const uint64_t guest_ptr = target->guest_cpu_start + byte_off;
    const uint64_t rem_slots = (target->guest_byte_size - byte_off) / target->inc_size;
    if (rem_slots == 0u) {
        return gpu_handle;
    }
    uint32_t want_slots = 16u;
    RsS2MirrorInfo s2_info{};
    if (bound_host_rs) {
        auto rit = rs_table_slot_counts_.find(bound_host_rs);
        if (rit != rs_table_slot_counts_.end() && root_param_index < rit->second.size()) {
            const uint32_t raw_entry = rit->second[root_param_index];
            if ((raw_entry & 0xC0000000u) != 0u) {
                return gpu_handle;
            }
            const uint32_t slot_cnt = raw_entry & 0x0FFFFFFFu;
            if (slot_cnt > 0u) {
                want_slots = slot_cnt;
            }
        }
        auto mit = rs_s2_mirrors_.find(bound_host_rs);
        if (mit != rs_s2_mirrors_.end() && root_param_index < mit->second.size()) {
            s2_info = mit->second[root_param_index];
        }
    }
    want_slots = std::clamp<uint32_t>(want_slots, 1u, 1024u);
    const bool has_s2_cbv_mirror = (s2_info.cbv_off >= 16u && s2_info.cbv_off + 16u <= want_slots);
    const bool has_s2_uav_mirror = (s2_info.uav_off >= 16u && s2_info.uav_off + 16u <= want_slots);
    if (!has_s2_cbv_mirror && !has_s2_uav_mirror) {
        want_slots = static_cast<uint32_t>(std::min<uint64_t>(want_slots, rem_slots));
    }
    if (want_slots == 0u || want_slots > target->host_num_descs) {
        return gpu_handle;
    }

    const uint32_t ring_base = (target->host_num_descs >= 262144u) ? 65536u : 0u;
    if (target->ring_cursor + want_slots > target->host_num_descs) {
        target->ring_cursor = ring_base;
        target->ring_wrapped = true;
    }
    const uint32_t base_slot = target->ring_cursor;
    target->ring_cursor += want_slots;

    (void)EnsureShadowDescHeapLocked();
    static std::unordered_map<uint64_t, uint64_t> s_srv_to_uav_shadow;
    const size_t copy_len = std::min<size_t>(sizeof(Desc32Key::bytes), target->inc_size);
    uint32_t hits = 0u;
    D3D12_CPU_DESCRIPTOR_HANDLE slot_cbv_shadow[16]{};
    D3D12_CPU_DESCRIPTOR_HANDLE slot_uav_shadow[16]{};
    const uint32_t primary_slots =
        (has_s2_cbv_mirror || has_s2_uav_mirror)
            ? std::min<uint32_t>(s2_info.native_slots, want_slots)
            : want_slots;
    const uint32_t guest_read_slots =
        static_cast<uint32_t>(std::min<uint64_t>(primary_slots, rem_slots));
    static const bool s_diag_bink = []() {
        char b[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_DIAG_BINK", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    const bool bink_ready =
        s_diag_bink && (openscarlett::d3d12_xs::g_bink_host_pso.load(std::memory_order_acquire) != nullptr);
    bool bink_like_table = false;
    std::string bink_slot_log;
    for (uint32_t i = 0u; i < primary_slots; ++i) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dst_sv{
            static_cast<SIZE_T>(target->host_cpu_start + static_cast<uint64_t>(base_slot + i) * target->inc_size)};
        Desc32Key key{};
        const uint64_t src_addr = guest_ptr + static_cast<uint64_t>(i) * target->inc_size;
        bool copied_valid = false;
        if (i < guest_read_slots &&
            SafeCopyGuestMemory(key.bytes, reinterpret_cast<const void*>(static_cast<uintptr_t>(src_addr)), copy_len)) {
            bool all_zero = true;
            for (uint8_t b : key.bytes) {
                if (b != 0u) { all_zero = false; break; }
            }
            if (!all_zero) {
                if (bink_ready && !is_compute && i < 8u && s2_info.native_slots == 16u && s2_info.cbv_off == 16u) {
                    uint32_t kw[8]{};
                    std::memcpy(kw, key.bytes, 32u);
                    char sb[256];
                    std::snprintf(sb, sizeof(sb), " s%u[%08X,%08X,%08X,%08X,%08X,%08X]",
                                  i, kw[0], kw[1], kw[2], kw[3], kw[4], kw[5]);
                    bink_slot_log += sb;
                }
                auto it = desc32_to_shadow_cpu_.find(key);
                if (it != desc32_to_shadow_cpu_.end()) {
                    const uint64_t sh_ptr = it->second;
                    if (bink_ready && !is_compute && i < 8u && s2_info.native_slots == 16u && s2_info.cbv_off == 16u) {
                        if (i == 5u || i == 6u) bink_like_table = true;
                        bink_slot_log += "(SH";
                        if (cbv_shadow_ptrs_.count(sh_ptr) > 0u) bink_slot_log += ":CBV";
                        if (shadow_buf_srv_sources_.count(sh_ptr) > 0u) bink_slot_log += ":BSRV";
                        {
                            std::lock_guard<std::mutex> dlk(g_diag_srv_mu);
                            auto dit = g_diag_sh_to_res.find(sh_ptr);
                            if (dit != g_diag_sh_to_res.end() && dit->second) {
                                const auto rd = dit->second->GetDesc();
                                char rb[96];
                                std::snprintf(rb, sizeof(rb), ":SRV=%p[%llux%ux%u f=%d]",
                                              static_cast<void*>(dit->second),
                                              static_cast<unsigned long long>(rd.Width), rd.Height, rd.DepthOrArraySize,
                                              static_cast<int>(rd.Format));
                                bink_slot_log += rb;
                            }
                        }
                        auto cbit = shadow_cbv_sources_.find(sh_ptr);
                        if (cbit != shadow_cbv_sources_.end() && cbit->second.guest_va != 0ull) {
                            float cf[16]{};
                            (void)SafeCopyGuestMemory(cf, reinterpret_cast<const void*>(static_cast<uintptr_t>(cbit->second.guest_va)), sizeof(cf));
                            char cb[128];
                            std::snprintf(cb, sizeof(cb), ":va=0x%llX sz=%u f0=[%.2f,%.2f,%.2f] f3=[%.2f,%.2f,%.2f]",
                                          static_cast<unsigned long long>(cbit->second.guest_va), cbit->second.aligned_size,
                                          cf[0], cf[1], cf[2], cf[12], cf[13], cf[14]);
                            bink_slot_log += cb;
                        }
                        bink_slot_log += ")";
                    }
                    if (uav_shadow_ptrs_.count(sh_ptr) > 0u) {
                        if (has_s2_uav_mirror && i < 16u) {
                            slot_uav_shadow[i] = D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(sh_ptr)};
                            if (is_compute && HasRegisteredUnifiedTextures()) {
                                auto tit = shadow_to_tex_res_.find(sh_ptr);
                                if (tit != shadow_to_tex_res_.end() && tit->second != nullptr) {
                                    std::lock_guard<std::mutex> ulk(unified_mutex_);
                                    auto uit_tex = unified_textures_.find(tit->second);
                                    if (uit_tex != unified_textures_.end()) {
                                        uit_tex->second.allow_cpu_upload = false;
                                        uit_tex->second.cpu_readable = false;
                                        if (uit_tex->second.dirty) {
                                            uit_tex->second.dirty = false;
                                            dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                                        }
                                    }
                                }
                            }
                        }
                        auto sit = uav_to_srv_shadow_.find(sh_ptr);
                        const D3D12_CPU_DESCRIPTOR_HANDLE srv_src =
                            (sit != uav_to_srv_shadow_.end())
                                ? D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(sit->second)}
                                : shadow_null_srv_cpu_;
                        if (srv_src.ptr != 0u) {
                            host_dev_->CopyDescriptorsSimple(
                                1u, dst_sv, srv_src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                            copied_valid = true;
                        }
                    } else {
                        D3D12_CPU_DESCRIPTOR_HANDLE sh_src{static_cast<SIZE_T>(sh_ptr)};
                        const bool is_known_cbv = (cbv_shadow_ptrs_.count(sh_ptr) > 0u);
                        auto cbv_it = shadow_cbv_sources_.find(sh_ptr);
                        if (cbv_it != shadow_cbv_sources_.end() &&
                            cbv_it->second.guest_va != 0ull && cbv_it->second.aligned_size > 0u) {
                            const UINT aligned_cb =
                                std::clamp<UINT>((cbv_it->second.aligned_size + 255u) & ~255u, 256u, 65536u);
                            const D3D12_GPU_VIRTUAL_ADDRESS live_gpu = UploadTransientGuestBufferForDesktop(
                                cbv_it->second.guest_va, aligned_cb, 256ull, /*allow_same_frame_cache=*/true);
                            if (live_gpu != 0ull) {
                                const uint32_t cslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                const D3D12_CPU_DESCRIPTOR_HANDLE live_sh{
                                    shadow_cbv_srv_uav_cpu_start_.ptr +
                                    static_cast<SIZE_T>(cslot) * shadow_cbv_srv_uav_inc_};
                                D3D12_CONSTANT_BUFFER_VIEW_DESC live_cbv{};
                                live_cbv.BufferLocation = live_gpu;
                                live_cbv.SizeInBytes = aligned_cb;
                                host_dev_->CreateConstantBufferView(&live_cbv, live_sh);
                                sh_src = live_sh;
                            }
                        }
                        if (has_s2_cbv_mirror && i < 16u &&
                            (is_known_cbv || cbv_it != shadow_cbv_sources_.end())) {
                            slot_cbv_shadow[i] = sh_src;
                            if (s_diag_bink && openscarlett::d3d12_xs::g_calib_pso_ready.load(std::memory_order_acquire) &&
                                !is_compute && i == 2u &&
                                cbv_it != shadow_cbv_sources_.end() && cbv_it->second.guest_va != 0ull) {
                                static std::atomic<uint32_t> s_hit_cnt{0u};
                                if (s_hit_cnt.fetch_add(1u, std::memory_order_relaxed) < 16u) {
                                    float cbf[16]{};
                                    (void)SafeCopyGuestMemory(
                                        cbf, reinterpret_cast<const void*>(static_cast<uintptr_t>(cbv_it->second.guest_va)), sizeof(cbf));
                                    std::fprintf(stderr,
                                                 "[OpenScarlett][CALIB-CB2-CBV] sz=%u cb0=[%.3f,%.3f,%.3f,%.3f] cb1=[%.3f,%.3f,%.3f,%.3f] cb2=[%.3f,%.3f,%.3f,%.3f] cb3=[%.4f,%.4f,%.4f,%.4f]\n",
                                                 cbv_it->second.aligned_size,
                                                 cbf[0], cbf[1], cbf[2], cbf[3],
                                                 cbf[4], cbf[5], cbf[6], cbf[7],
                                                 cbf[8], cbf[9], cbf[10], cbf[11],
                                                 cbf[12], cbf[13], cbf[14], cbf[15]);
                                    std::fflush(stderr);
                                }
                            }
                        }
                        auto bsrv_it = shadow_buf_srv_sources_.find(sh_ptr);
                        if (bsrv_it != shadow_buf_srv_sources_.end()) {
                            const ShadowBufSrvSource& bs = bsrv_it->second;
                            const bool bs_gpu_written =
                                (bs.host_res != nullptr) &&
                                IsGpuWrittenBufferRange(bs.host_res, bs.host_byte_off, bs.byte_size);
                            if (s_diag_bink && openscarlett::d3d12_xs::g_calib_pso_ready.load(std::memory_order_acquire) &&
                                !is_compute && i == 2u && bs.guest_cpu_va != 0ull) {
                                static std::atomic<uint32_t> s_bsrv_cnt{0u};
                                if (s_bsrv_cnt.fetch_add(1u, std::memory_order_relaxed) < 16u) {
                                    float cbf[16]{};
                                    (void)SafeCopyGuestMemory(
                                        cbf, reinterpret_cast<const void*>(static_cast<uintptr_t>(bs.guest_cpu_va)), sizeof(cbf));
                                    std::fprintf(stderr,
                                                 "[OpenScarlett][CALIB-CB2-BSRV] gw=%d sz=%llu cb0=[%.3f,%.3f,%.3f,%.3f] cb1=[%.3f,%.3f,%.3f,%.3f] cb2=[%.3f,%.3f,%.3f,%.3f] cb3=[%.4f,%.4f,%.4f,%.4f]\n",
                                                 bs_gpu_written ? 1 : 0, static_cast<unsigned long long>(bs.byte_size),
                                                 cbf[0], cbf[1], cbf[2], cbf[3],
                                                 cbf[4], cbf[5], cbf[6], cbf[7],
                                                 cbf[8], cbf[9], cbf[10], cbf[11],
                                                 cbf[12], cbf[13], cbf[14], cbf[15]);
                                    std::fflush(stderr);
                                }
                            }
                            D3D12_GPU_VIRTUAL_ADDRESS cbv_gpu_va = 0ull;
                            UINT cbv_aligned =
                                std::clamp<UINT>((static_cast<UINT>(std::min<uint64_t>(bs.byte_size, 65536ull)) + 255u) & ~255u,
                                                 256u, 65536u);
                            if (!bs_gpu_written && bs.guest_cpu_va != 0ull) {
                                const uint64_t up_len = std::min<uint64_t>(bs.byte_size, 65536ull);
                                const D3D12_GPU_VIRTUAL_ADDRESS ring_gpu = UploadTransientGuestBufferForDesktop(
                                    bs.guest_cpu_va, up_len, 256ull, /*allow_same_frame_cache=*/true);
                                if (ring_gpu != 0ull) {
                                    cbv_gpu_va = ring_gpu;
                                    const uint32_t eff_stride =
                                        ((bs.flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0u || bs.format == DXGI_FORMAT_R32_TYPELESS)
                                            ? 4u
                                            : bs.stride;
                                    if (transient_ring_ && ring_gpu >= transient_ring_gpu_ &&
                                        eff_stride > 0u && (256u % eff_stride) == 0u && up_len >= eff_stride) {
                                        D3D12_SHADER_RESOURCE_VIEW_DESC lsrv{};
                                        lsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                                        lsrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                                        if ((bs.flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0u || bs.format == DXGI_FORMAT_R32_TYPELESS) {
                                            lsrv.Format = DXGI_FORMAT_R32_TYPELESS;
                                            lsrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                                        } else {
                                            lsrv.Format = bs.format;
                                            lsrv.Buffer.StructureByteStride = bs.stride;
                                        }
                                        lsrv.Buffer.FirstElement = (ring_gpu - transient_ring_gpu_) / eff_stride;
                                        lsrv.Buffer.NumElements = static_cast<UINT>(up_len / eff_stride);
                                        const uint32_t sslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                        const D3D12_CPU_DESCRIPTOR_HANDLE live_srv_sh{
                                            shadow_cbv_srv_uav_cpu_start_.ptr +
                                            static_cast<SIZE_T>(sslot) * shadow_cbv_srv_uav_inc_};
                                        host_dev_->CreateShaderResourceView(transient_ring_, &lsrv, live_srv_sh);
                                        sh_src = live_srv_sh;
                                    }
                                }
                            } else if (bs.host_res != nullptr) {
                                const D3D12_GPU_VIRTUAL_ADDRESS raw_hva =
                                    bs.host_res->GetGPUVirtualAddress() + bs.host_byte_off;
                                cbv_gpu_va = AlignRootCbvGpuVaForDesktop(raw_hva, bs.guest_cpu_va);
                                const uint64_t hb_w = bs.host_res->GetDesc().Width;
                                if (cbv_gpu_va == raw_hva && bs.host_byte_off < hb_w) {
                                    const uint64_t avail_align = (hb_w - bs.host_byte_off) & ~255ull;
                                    if (avail_align >= 256ull) {
                                        cbv_aligned = static_cast<UINT>(std::min<uint64_t>(cbv_aligned, avail_align));
                                    } else {
                                        cbv_gpu_va = 0ull;
                                    }
                                }
                            }
                            if (has_s2_cbv_mirror && i < 16u && slot_cbv_shadow[i].ptr == 0u && cbv_gpu_va != 0ull) {
                                const uint32_t bslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                const D3D12_CPU_DESCRIPTOR_HANDLE sh_bcbv{
                                    shadow_cbv_srv_uav_cpu_start_.ptr +
                                    static_cast<SIZE_T>(bslot) * shadow_cbv_srv_uav_inc_};
                                D3D12_CONSTANT_BUFFER_VIEW_DESC b_cbv{};
                                b_cbv.BufferLocation = cbv_gpu_va;
                                b_cbv.SizeInBytes = cbv_aligned;
                                host_dev_->CreateConstantBufferView(&b_cbv, sh_bcbv);
                                slot_cbv_shadow[i] = sh_bcbv;
                            }
                        }
                        host_dev_->CopyDescriptorsSimple(
                            1u, dst_sv, sh_src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        if (has_s2_uav_mirror && i < 16u) {
                            auto uit = srv_to_uav_shadow_.find(sh_ptr);
                            if (uit != srv_to_uav_shadow_.end()) {
                                slot_uav_shadow[i] = D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(uit->second)};
                            } else {
                                auto uit2 = s_srv_to_uav_shadow.find(sh_ptr);
                                if (uit2 != s_srv_to_uav_shadow.end()) {
                                    slot_uav_shadow[i] = D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(uit2->second)};
                                }
                            }
                            if (is_compute && slot_uav_shadow[i].ptr != 0u && HasRegisteredUnifiedTextures()) {
                                auto tit = shadow_to_tex_res_.find(sh_ptr);
                                if (tit == shadow_to_tex_res_.end()) {
                                    tit = shadow_to_tex_res_.find(slot_uav_shadow[i].ptr);
                                }
                                if (tit != shadow_to_tex_res_.end() && tit->second != nullptr) {
                                    std::lock_guard<std::mutex> ulk(unified_mutex_);
                                    auto uit_tex = unified_textures_.find(tit->second);
                                    if (uit_tex != unified_textures_.end()) {
                                        uit_tex->second.allow_cpu_upload = false;
                                        uit_tex->second.cpu_readable = false;
                                        if (uit_tex->second.dirty) {
                                            uit_tex->second.dirty = false;
                                            dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                                        }
                                    }
                                }
                            }
                        }
                        copied_valid = true;
                    }
                    ++hits;
                } else if (retail_scarlett_abi_ && copy_len == 32u && shadow_cbv_srv_uav_heap_) {
                    // Synthesize SRV (and optional space2 UAV mirror at +32) when guest CPU-wrote
                    // a raw 32B RDNA2 SQ_IMG_RSRC (T#) directly into the guest-backed descriptor heap.
                    DecodedGfx10TSharp ts{};
                    if (DecodeGfx10ImageRsrcDescriptor(key.bytes, ts)) {
                        if (s_diag_bink && !is_compute && i < 8u && ts.width >= 960u) {
                            bink_like_table = true;
                        }
                        D3D12_RESOURCE_DESC exp{};
                        exp.Dimension = (ts.rsrc_type == 12u)
                                            ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                                            : (ts.rsrc_type == 10u ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                   : D3D12_RESOURCE_DIMENSION_TEXTURE2D);
                        exp.Width = ts.width;
                        exp.Height = ts.height;
                        exp.Format = ts.format;
                        ID3D12Resource* tex_res =
                            ResolvePlacedTextureByGpuVa(ts.gpu_va, (ts.format != DXGI_FORMAT_UNKNOWN) ? &exp : nullptr);
                        if (!tex_res) {
                            ID3D12Resource* any_res = ResolvePlacedTextureByGpuVa(ts.gpu_va, nullptr);
                            if (any_res && any_res->GetDesc().Dimension == exp.Dimension) {
                                tex_res = any_res;
                            }
                        }
                        if (s_diag_bink && !is_compute && i < 8u && s2_info.native_slots == 16u && s2_info.cbv_off == 16u) {
                            char tb[160];
                            std::snprintf(tb, sizeof(tb), "(T#:va=0x%llX %ux%ux%u fmt=%d sw=%u res=%p)",
                                          static_cast<unsigned long long>(ts.gpu_va),
                                          ts.width, ts.height, ts.depth,
                                          static_cast<int>(ts.format), ts.sw_mode,
                                          static_cast<void*>(tex_res));
                            bink_slot_log += tb;
                        }
                        if (!tex_res &&
                            (ts.rsrc_type >= 8u && ts.rsrc_type <= 13u) &&
                            ts.format != DXGI_FORMAT_UNKNOWN && !IsDepthOrPlanarFormatForCopy(ts.format)) {
                            const uint64_t cpu_va = ResolveGuestCpuAliasVa(ts.gpu_va);
                            const bool readable_cpu = IsReadableGuestVaRange(cpu_va, 256ull);
                            XG_TILE_MODE ts_tile_mode = XG_TILE_MODE_LINEAR;
                            bool valid_tm = MapGfx10SwizzleModeToXgTileMode(
                                ts.sw_mode, exp.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D, ts_tile_mode);
                            if (ts.rsrc_type == 10u && ts.width <= 64u && ts.height <= 64u && ts.depth <= 64u) {
                                ts_tile_mode = XG_TILE_MODE_LINEAR;
                                valid_tm = true;
                            }
                            if (valid_tm && (readable_cpu || (is_compute && has_s2_uav_mirror))) {
                                DXGI_FORMAT uav_chk_fmt = MapTypelessFormatForSrv(ts.format);
                                if (uav_chk_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                                    uav_chk_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                                } else if (uav_chk_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                                    uav_chk_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                                } else if (uav_chk_fmt == DXGI_FORMAT_A8_UNORM) {
                                    uav_chk_fmt = DXGI_FORMAT_R8_UNORM;
                                }
                                const bool is_srgb_res =
                                    (ts.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                                     ts.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                                D3D12_RESOURCE_DESC lazy_desc{};
                                lazy_desc.Dimension = exp.Dimension;
                                lazy_desc.Width = ts.width;
                                lazy_desc.Height = (exp.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) ? 1u : ts.height;
                                lazy_desc.DepthOrArraySize = static_cast<UINT16>(std::clamp<uint32_t>(
                                    (ts.rsrc_type == 11u) ? std::max<uint32_t>(6u, ts.depth) : ts.depth, 1u, 2048u));
                                lazy_desc.MipLevels = static_cast<UINT16>(
                                    (ts_tile_mode != XG_TILE_MODE_LINEAR && ts.last_level < 15u)
                                        ? (ts.last_level + 1u)
                                        : 1u);
                                lazy_desc.Format = is_srgb_res ? DXGI_FORMAT_R8G8B8A8_TYPELESS : ts.format;
                                if (ts.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                                    lazy_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                                }
                                lazy_desc.SampleDesc = {1u, 0u};
                                lazy_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                                D3D12_FEATURE_DATA_FORMAT_SUPPORT fsup{};
                                fsup.Format = uav_chk_fmt;
                                if ((ts_tile_mode == XG_TILE_MODE_LINEAR || (is_compute && has_s2_uav_mirror)) &&
                                    !FormatForbidsDesktopUav(uav_chk_fmt) &&
                                    SUCCEEDED(host_dev_->CheckFeatureSupport(
                                        D3D12_FEATURE_FORMAT_SUPPORT, &fsup, sizeof(fsup))) &&
                                    (fsup.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) {
                                    lazy_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                                }
                                const D3D12_RESOURCE_STATES init_st =
                                    (lazy_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
                                        ? D3D12_RESOURCE_STATE_COMMON
                                        : ((lazy_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                                               ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                D3D12_HEAP_PROPERTIES def_hp{};
                                def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                                ID3D12Resource* lazy_tex = nullptr;
                                HRESULT lazy_hr = host_dev_->CreateCommittedResource(
                                    &def_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                                    init_st, nullptr,
                                    OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                                if ((FAILED(lazy_hr) || !lazy_tex) &&
                                    (lazy_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0) {
                                    lazy_desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                                    const D3D12_RESOURCE_STATES retry_st =
                                        (lazy_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                                            ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                            : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                                    lazy_hr = host_dev_->CreateCommittedResource(
                                        &def_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                                        retry_st, nullptr,
                                        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                                }
                                if (FAILED(lazy_hr) || !lazy_tex) {
                                    D3D12_HEAP_PROPERTIES l0_hp{};
                                    l0_hp.Type = D3D12_HEAP_TYPE_CUSTOM;
                                    l0_hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
                                    l0_hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                                    l0_hp.CreationNodeMask = 1u;
                                    l0_hp.VisibleNodeMask = 1u;
                                    lazy_hr = host_dev_->CreateCommittedResource(
                                        &l0_hp, D3D12_HEAP_FLAG_NONE, &lazy_desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&lazy_tex));
                                }
                                if (SUCCEEDED(lazy_hr) && lazy_tex) {
                                    if (ts_tile_mode != XG_TILE_MODE_LINEAR) {
                                        SetTrackedResourceTileMode(lazy_tex, ts_tile_mode);
                                    }
                                    TrackPlacedTextureVa(lazy_tex, ts.gpu_va);
                                    SetRetailResourceState(lazy_tex, init_st);
                                    bool has_nonzero_cpu = false;
                                    if (readable_cpu) {
                                        uint64_t probe_q[32]{};
                                        if (SafeCopyGuestMemory(probe_q, reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu_va)), sizeof(probe_q))) {
                                            for (uint64_t q : probe_q) {
                                                if (q != 0ull) { has_nonzero_cpu = true; break; }
                                            }
                                        }
                                    }
                                    if (has_nonzero_cpu || readable_cpu) {
                                        D3D12_RESOURCE_DESC sync_desc = lazy_desc;
                                        sync_desc.Format = uav_chk_fmt;
                                        AutoRegisterUnifiedResource(
                                            lazy_tex, cpu_va, sync_desc, init_st, false, false);
                                    }
                                    bool owned = false;
                                    {
                                        std::lock_guard<std::mutex> ulk(unified_mutex_);
                                        owned = (unified_textures_.find(lazy_tex) != unified_textures_.end());
                                    }
                                    if (!owned) {
                                        TrackTransientGpuResource(lazy_tex);
                                    }
                                    tex_res = lazy_tex;
                                    lazy_tex->Release();
                                }
                            }
                        }
                        if (tex_res) {
                            const D3D12_RESOURCE_DESC td = tex_res->GetDesc();
                            const bool non_typeless_depth =
                                ((td.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) &&
                                ((td.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
                                 td.Format == DXGI_FORMAT_D32_FLOAT ||
                                 td.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                                 td.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                 td.Format == DXGI_FORMAT_D16_UNORM);
                            if (!non_typeless_depth && td.SampleDesc.Count <= 1u &&
                                (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                                 td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                                 td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)) {
                                D3D12_SHADER_RESOURCE_VIEW_DESC ts_srv{};
                                ts_srv.Format = ResolveCompatibleSrvFormat(td.Format, ts.format);
                                ts_srv.Shader4ComponentMapping =
                                    SanitizeOrTranslateXboxShader4ComponentMapping(ts.dst_sel_12, ts_srv.Format);
                                const UINT max_mips = (td.MipLevels > 0u) ? td.MipLevels : 1u;
                                const UINT mip0 = (ts.base_level < max_mips) ? ts.base_level : 0u;
                                const UINT rem = max_mips - mip0;
                                const UINT req_mips =
                                    (ts.last_level >= ts.base_level) ? (ts.last_level - ts.base_level + 1u) : rem;
                                const UINT mips = (req_mips > 0u && req_mips <= rem) ? req_mips : rem;
                                if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                                    if (ts.rsrc_type == 12u) {
                                        ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                                        ts_srv.Texture1DArray.MostDetailedMip = mip0;
                                        ts_srv.Texture1DArray.MipLevels = mips;
                                        ts_srv.Texture1DArray.FirstArraySlice =
                                            (ts.base_array < td.DepthOrArraySize) ? ts.base_array : 0u;
                                        const UINT rem_a = td.DepthOrArraySize - ts_srv.Texture1DArray.FirstArraySlice;
                                        ts_srv.Texture1DArray.ArraySize =
                                            (ts.depth > 0u && ts.depth <= rem_a) ? ts.depth : rem_a;
                                    } else {
                                        ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                                        ts_srv.Texture1D.MostDetailedMip = mip0;
                                        ts_srv.Texture1D.MipLevels = mips;
                                    }
                                } else if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                                    ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                                    ts_srv.Texture3D.MostDetailedMip = mip0;
                                    ts_srv.Texture3D.MipLevels = mips;
                                } else if (ts.rsrc_type == 13u) {
                                    ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                                    ts_srv.Texture2DArray.MostDetailedMip = mip0;
                                    ts_srv.Texture2DArray.MipLevels = mips;
                                    ts_srv.Texture2DArray.FirstArraySlice =
                                        (ts.base_array < td.DepthOrArraySize) ? ts.base_array : 0u;
                                    const UINT rem_a = td.DepthOrArraySize - ts_srv.Texture2DArray.FirstArraySlice;
                                    ts_srv.Texture2DArray.ArraySize =
                                        (ts.depth > 0u && ts.depth <= rem_a) ? ts.depth : rem_a;
                                } else if (ts.rsrc_type == 11u && td.DepthOrArraySize >= 6u) {
                                    ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                                    ts_srv.TextureCube.MostDetailedMip = mip0;
                                    ts_srv.TextureCube.MipLevels = mips;
                                } else {
                                    ts_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                    ts_srv.Texture2D.MostDetailedMip = mip0;
                                    ts_srv.Texture2D.MipLevels = mips;
                                }
                                const uint32_t slot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                const D3D12_CPU_DESCRIPTOR_HANDLE sh_dst{
                                    shadow_cbv_srv_uav_cpu_start_.ptr +
                                    static_cast<SIZE_T>(slot) * shadow_cbv_srv_uav_inc_};
                                host_dev_->CreateShaderResourceView(tex_res, &ts_srv, sh_dst);
                                shadow_slot_keys_[slot] = key;
                                shadow_slot_valid_[slot] = 1u;
                                desc32_to_shadow_cpu_[key] = sh_dst.ptr;
                                shadow_to_tex_res_[sh_dst.ptr] = tex_res;
                                host_dev_->CopyDescriptorsSimple(
                                    1u, dst_sv, sh_dst, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                                ++hits;
                                copied_valid = true;

                                if ((td.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
                                    D3D12_UNORDERED_ACCESS_VIEW_DESC ts_uav{};
                                    ts_uav.Format = ts_srv.Format;
                                    if (ts_uav.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                                        ts_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                                    } else if (ts_uav.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                                        ts_uav.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                    }
                                    if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                                        ts_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                                        ts_uav.Texture1D.MipSlice = mip0;
                                    } else if (td.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                                        ts_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                                        ts_uav.Texture3D.MipSlice = mip0;
                                        ts_uav.Texture3D.WSize = td.DepthOrArraySize;
                                    } else if (ts.rsrc_type == 13u) {
                                        ts_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                                        ts_uav.Texture2DArray.MipSlice = mip0;
                                        ts_uav.Texture2DArray.ArraySize = td.DepthOrArraySize;
                                    } else {
                                        ts_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                                        ts_uav.Texture2D.MipSlice = mip0;
                                    }
                                    const uint32_t uslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                    const D3D12_CPU_DESCRIPTOR_HANDLE sh_uav{
                                        shadow_cbv_srv_uav_cpu_start_.ptr +
                                        static_cast<SIZE_T>(uslot) * shadow_cbv_srv_uav_inc_};
                                    host_dev_->CreateUnorderedAccessView(tex_res, nullptr, &ts_uav, sh_uav);
                                    s_srv_to_uav_shadow[sh_dst.ptr] = sh_uav.ptr;
                                    shadow_to_tex_res_[sh_uav.ptr] = tex_res;
                                    if (has_s2_uav_mirror && i < 16u) {
                                        slot_uav_shadow[i] = sh_uav;
                                        if (is_compute && HasRegisteredUnifiedTextures()) {
                                            std::lock_guard<std::mutex> ulk(unified_mutex_);
                                            auto uit = unified_textures_.find(tex_res);
                                            if (uit != unified_textures_.end()) {
                                                uit->second.allow_cpu_upload = false;
                                                uit->second.cpu_readable = false;
                                                if (uit->second.dirty) {
                                                    uit->second.dirty = false;
                                                    dirty_unified_count_.fetch_sub(1u, std::memory_order_acq_rel);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        // Raw 16-byte RDNA2 SQ_BUF_RSRC (V#) constant/structured buffer descriptor
                        // CPU-written into a 32-byte guest descriptor slot (vw[4..7] may hold stale
                        // bytes from a prior 32B T# or adjacent 16B V#). Populate BOTH space2 CBV
                        // mirror (+16) and space2/native SRV/CBV (@0..@15) without stage guesswork.
                        uint32_t vw[8]{};
                        std::memcpy(vw, key.bytes, 32u);
                        const uint32_t v_type = (vw[3] >> 30u) & 0x3u;
                        const uint64_t v_va =
                            (static_cast<uint64_t>(vw[1] & 0xFFFFu) << 32u) | static_cast<uint64_t>(vw[0]);
                        const uint32_t v_stride = (vw[1] >> 16u) & 0x3FFFu;
                        const uint32_t v_num_rec = vw[2];
                        const uint64_t v_cpu_va = ResolveGuestCpuAliasVa(v_va);
                        if (v_type == 0u &&
                            v_va >= 0x10000ull && v_va < 0x0000800000000000ull &&
                            (v_va & 3ull) == 0ull && v_num_rec > 0u && v_num_rec <= 0x40000000u) {
                            const uint64_t max_span = MaxReadableGuestSpan(v_cpu_va, 65536ull);
                            if (max_span >= 16ull) {
                                const uint64_t req_cb =
                                    (v_stride > 0u && v_num_rec <= 4096u)
                                        ? (static_cast<uint64_t>(v_num_rec) * v_stride)
                                        : static_cast<uint64_t>(v_num_rec);
                                const uint64_t upload_cb =
                                    std::min<uint64_t>(max_span, std::clamp<uint64_t>(req_cb, 16ull, 65536ull));
                                const UINT aligned_cb =
                                    std::clamp<UINT>((static_cast<UINT>(upload_cb) + 255u) & ~255u, 256u, 65536u);
                                const D3D12_GPU_VIRTUAL_ADDRESS live_gpu = UploadTransientGuestBufferForDesktop(
                                    v_cpu_va, upload_cb, 256ull, /*allow_same_frame_cache=*/true);
                                D3D12_CPU_DESCRIPTOR_HANDLE sh_vcbv{0u};
                                if (live_gpu != 0ull) {
                                    const uint32_t vslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                    sh_vcbv.ptr = shadow_cbv_srv_uav_cpu_start_.ptr +
                                                  static_cast<SIZE_T>(vslot) * shadow_cbv_srv_uav_inc_;
                                    D3D12_CONSTANT_BUFFER_VIEW_DESC v_cbv{};
                                    v_cbv.BufferLocation = live_gpu;
                                    v_cbv.SizeInBytes = aligned_cb;
                                    host_dev_->CreateConstantBufferView(&v_cbv, sh_vcbv);
                                    CheckCpuDeviceRemoved("MatVSharpCbv", live_gpu, aligned_cb);
                                    if (has_s2_cbv_mirror && i < 16u) {
                                        slot_cbv_shadow[i] = sh_vcbv;
                                    }
                                    if (s_diag_bink && !is_compute && (i == 5u || i == 6u)) {
                                        bink_like_table = true;
                                        float cbf[16]{};
                                        (void)SafeCopyGuestMemory(
                                            cbf, reinterpret_cast<const void*>(static_cast<uintptr_t>(v_cpu_va)), sizeof(cbf));
                                        char vb[192];
                                        std::snprintf(vb, sizeof(vb), "(V#:va=0x%llX sz=%llu f0=[%.2f,%.2f,%.2f] f3=[%.2f,%.2f,%.2f])",
                                                      static_cast<unsigned long long>(v_cpu_va),
                                                      static_cast<unsigned long long>(upload_cb),
                                                      cbf[0], cbf[1], cbf[2], cbf[12], cbf[13], cbf[14]);
                                        bink_slot_log += vb;
                                    }
                                    if (s_diag_bink && openscarlett::d3d12_xs::g_calib_pso_ready.load(std::memory_order_acquire) &&
                                        !is_compute && i == 2u && upload_cb >= 32ull && upload_cb <= 1024ull) {
                                        static std::atomic<uint32_t> s_cb2_cnt{0u};
                                        if (s_cb2_cnt.fetch_add(1u, std::memory_order_relaxed) < 16u) {
                                            float cbf[16]{};
                                            (void)SafeCopyGuestMemory(
                                                cbf, reinterpret_cast<const void*>(static_cast<uintptr_t>(v_cpu_va)), sizeof(cbf));
                                            std::fprintf(stderr,
                                                         "[OpenScarlett][CALIB-CB2-VSH] sz=%llu cb0=[%.3f,%.3f,%.3f,%.3f] cb1=[%.3f,%.3f,%.3f,%.3f] cb2=[%.3f,%.3f,%.3f,%.3f] cb3=[%.4f,%.4f,%.4f,%.4f]\n",
                                                         static_cast<unsigned long long>(upload_cb),
                                                         cbf[0], cbf[1], cbf[2], cbf[3],
                                                         cbf[4], cbf[5], cbf[6], cbf[7],
                                                         cbf[8], cbf[9], cbf[10], cbf[11],
                                                         cbf[12], cbf[13], cbf[14], cbf[15]);
                                            std::fflush(stderr);
                                        }
                                    }
                                }
                                if (v_stride > 0u && (256u % v_stride) == 0u) {
                                    const uint64_t byte_len =
                                        std::min<uint64_t>(max_span,
                                                           std::min<uint64_t>(static_cast<uint64_t>(v_stride) * v_num_rec, 65536ull));
                                    const D3D12_GPU_VIRTUAL_ADDRESS ring_gpu = UploadTransientGuestBufferForDesktop(
                                        v_cpu_va, byte_len, 256ull, /*allow_same_frame_cache=*/true);
                                    if (ring_gpu != 0ull && transient_ring_ && ring_gpu >= transient_ring_gpu_ &&
                                        byte_len >= v_stride) {
                                        D3D12_SHADER_RESOURCE_VIEW_DESC vsrv{};
                                        vsrv.Format = DXGI_FORMAT_UNKNOWN;
                                        vsrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                                        vsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                                        vsrv.Buffer.FirstElement = (ring_gpu - transient_ring_gpu_) / v_stride;
                                        vsrv.Buffer.NumElements = static_cast<UINT>(byte_len / v_stride);
                                        vsrv.Buffer.StructureByteStride = v_stride;
                                        const uint32_t sslot = AllocShadowSlotLocked(/*prefer_static_partition=*/false);
                                        const D3D12_CPU_DESCRIPTOR_HANDLE sh_vsrv{
                                            shadow_cbv_srv_uav_cpu_start_.ptr +
                                            static_cast<SIZE_T>(sslot) * shadow_cbv_srv_uav_inc_};
                                        host_dev_->CreateShaderResourceView(transient_ring_, &vsrv, sh_vsrv);
                                        host_dev_->CopyDescriptorsSimple(
                                            1u, dst_sv, sh_vsrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                                        ++hits;
                                        copied_valid = true;
                                    }
                                }
                                if (!copied_valid && sh_vcbv.ptr != 0u) {
                                    host_dev_->CopyDescriptorsSimple(
                                        1u, dst_sv, sh_vcbv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                                    ++hits;
                                    copied_valid = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!copied_valid && shadow_null_srv_cpu_.ptr != 0u) {
            host_dev_->CopyDescriptorsSimple(
                1u, dst_sv, shadow_null_srv_cpu_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    if (has_s2_cbv_mirror) {
        const D3D12_CPU_DESCRIPTOR_HANDLE null_cbv =
            (shadow_null_cbv_cpu_.ptr != 0u) ? shadow_null_cbv_cpu_ : shadow_null_srv_cpu_;
        for (uint32_t k = 0u; k < 16u; ++k) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_cbv{
                static_cast<SIZE_T>(target->host_cpu_start +
                                    static_cast<uint64_t>(base_slot + s2_info.cbv_off + k) * target->inc_size)};
            const D3D12_CPU_DESCRIPTOR_HANDLE src_cbv =
                (slot_cbv_shadow[k].ptr != 0u) ? slot_cbv_shadow[k] : null_cbv;
            if (src_cbv.ptr != 0u) {
                host_dev_->CopyDescriptorsSimple(
                    1u, dst_cbv, src_cbv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
    }
    if (has_s2_uav_mirror) {
        const D3D12_CPU_DESCRIPTOR_HANDLE null_uav =
            (shadow_null_uav_cpu_.ptr != 0u) ? shadow_null_uav_cpu_ : shadow_null_srv_cpu_;
        for (uint32_t k = 0u; k < 16u; ++k) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_uav{
                static_cast<SIZE_T>(target->host_cpu_start +
                                    static_cast<uint64_t>(base_slot + s2_info.uav_off + k) * target->inc_size)};
            const D3D12_CPU_DESCRIPTOR_HANDLE src_uav =
                (slot_uav_shadow[k].ptr != 0u) ? slot_uav_shadow[k] : null_uav;
            if (src_uav.ptr != 0u) {
                host_dev_->CopyDescriptorsSimple(
                    1u, dst_uav, src_uav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
    }
    if (bink_ready && !is_compute && !bink_slot_log.empty() &&
        (bink_like_table || bink_slot_log.find(" s6[") != std::string::npos || bink_slot_log.find(" s5[") != std::string::npos)) {
        static std::atomic<uint32_t> s_bink_mat_cnt{0u};
        if (s_bink_mat_cnt.fetch_add(1u, std::memory_order_relaxed) < 24u) {
            std::fprintf(stderr, "[OpenScarlett][BINK-MAT] rp=%u%s\n", root_param_index, bink_slot_log.c_str());
            std::fflush(stderr);
        }
    }
    if (out_copied_hits) *out_copied_hits = hits;
    if (out_slot_count) *out_slot_count = want_slots;
    return D3D12_GPU_DESCRIPTOR_HANDLE{
        target->host_gpu_start + static_cast<uint64_t>(base_slot) * target->inc_size};
}

void DeviceX::RecordDiagDescriptor(uint64_t cpu_ptr, std::string desc) {
    if (cpu_ptr == 0ull) return;
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    if (diag_desc_map_.size() >= 20000u) return;
    diag_desc_map_[cpu_ptr] = std::move(desc);
}

void DeviceX::CopyDiagDescriptor(uint64_t dst_cpu_ptr, uint64_t src_cpu_ptr) {
    if (dst_cpu_ptr == 0ull) return;
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    auto it = diag_desc_map_.find(src_cpu_ptr);
    if (it != diag_desc_map_.end()) {
        diag_desc_map_[dst_cpu_ptr] = it->second;
    }
}

std::string DeviceX::DescribeDiagDescriptorTable(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, uint32_t max_slots) {
    if (gpu_handle.ptr == 0ull) return {};
    uint64_t cpu_base = 0ull;
    uint32_t inc = 32u;
    uint32_t avail = 0u;
    {
        std::lock_guard<std::mutex> lk(paged_desc_mutex_);
        for (const auto& [h, st] : paged_desc_heaps_) {
            (void)h;
            const uint64_t g0 = st.gpu_start.ptr;
            const uint64_t span = static_cast<uint64_t>(st.num_descriptors) * st.inc_size;
            if (g0 != 0ull && gpu_handle.ptr >= g0 && gpu_handle.ptr < g0 + span) {
                const uint64_t idx = (gpu_handle.ptr - g0) / st.inc_size;
                cpu_base = st.cpu_start.ptr + idx * st.inc_size;
                inc = st.inc_size;
                avail = static_cast<uint32_t>(std::min<uint64_t>(max_slots, st.num_descriptors - idx));
                break;
            }
        }
    }
    if (cpu_base == 0ull || avail == 0u) return {};
    std::string out;
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    for (uint32_t i = 0u; i < avail; ++i) {
        auto it = diag_desc_map_.find(cpu_base + static_cast<uint64_t>(i) * inc);
        if (it != diag_desc_map_.end()) {
            char pfx[24];
            std::snprintf(pfx, sizeof(pfx), "%s+%u:", out.empty() ? "{" : " ", i);
            out += pfx;
            out += it->second;
        }
    }
    if (out.empty()) {
        uint64_t gcpu = 0ull;
        {
            std::lock_guard<std::mutex> glk(guest_dh_mutex_);
            for (const auto& g : guest_backed_desc_heaps_) {
                if (gpu_handle.ptr >= g.host_gpu_start &&
                    gpu_handle.ptr < g.host_gpu_start + g.guest_byte_size) {
                    gcpu = g.guest_cpu_start + (gpu_handle.ptr - g.host_gpu_start);
                    break;
                }
            }
        }
        std::string gdesc;
        if (gcpu != 0ull) {
            for (uint32_t s = 0u; s < 4u; ++s) {
                const uint64_t saddr = gcpu + static_cast<uint64_t>(s) * inc;
                auto it = diag_desc_map_.find(saddr);
                uint32_t gd[8]{};
                const bool gok = SafeCopyGuestMemory(gd, reinterpret_cast<const void*>(static_cast<uintptr_t>(saddr)), 32u);
                char sb[160];
                if (it != diag_desc_map_.end()) {
                    std::snprintf(sb, sizeof(sb), " g+%u:%s", s, it->second.c_str());
                } else {
                    std::snprintf(sb, sizeof(sb), " g+%u[%d]=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X",
                                  s, gok ? 1 : 0, gd[0], gd[1], gd[2], gd[3], gd[4], gd[5], gd[6], gd[7]);
                }
                gdesc += sb;
            }
        }
        char nf[96];
        std::snprintf(nf, sizeof(nf), "{gcpu=0x%llX map=%zu",
                      static_cast<unsigned long long>(gcpu), diag_desc_map_.size());
        return std::string(nf) + gdesc + "}";
    }
    out += "}";
    return out;
}

std::string DeviceX::DescribeDiagRtv(uint64_t cpu_ptr) {
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    auto it = diag_desc_map_.find(cpu_ptr);
    return (it != diag_desc_map_.end()) ? it->second : std::string{};
}

void DeviceX::RecordDiagRootSig(void* host_rs, std::string desc) {
    if (!host_rs) return;
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    diag_rs_map_[host_rs] = std::move(desc);
}

std::string DeviceX::DescribeDiagRootSig(void* host_rs) {
    std::lock_guard<std::mutex> lk(diag_meta_mutex_);
    auto it = diag_rs_map_.find(host_rs);
    return (it != diag_rs_map_.end()) ? it->second : std::string{};
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::AlignRootCbvGpuVaForDesktop(
    D3D12_GPU_VIRTUAL_ADDRESS translated_va,
    D3D12_GPU_VIRTUAL_ADDRESS guest_va_fallback) {
    if (translated_va == 0ull || (translated_va & 255ull) == 0ull || !host_dev_) {
        return translated_va;
    }

    constexpr uint64_t kSlotBytes = 4096ull;
    constexpr uint32_t kSlotCount = 16384u;  // 64 MiB lock-free ring (power of 2)
    constexpr uint64_t kRingBytes = kSlotBytes * kSlotCount;

    uint8_t* ring_cpu = cbv_align_ring_cpu_.load(std::memory_order_acquire);
    D3D12_GPU_VIRTUAL_ADDRESS ring_gpu = cbv_align_ring_gpu_.load(std::memory_order_relaxed);
    if (!ring_cpu) {
        std::lock_guard<std::mutex> ring_lock(cbv_align_mutex_);
        ring_cpu = cbv_align_ring_cpu_.load(std::memory_order_relaxed);
        if (!ring_cpu) {
            D3D12_HEAP_PROPERTIES up_hp{};
            up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rdesc{};
            rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rdesc.Width = kRingBytes;
            rdesc.Height = 1u;
            rdesc.DepthOrArraySize = 1u;
            rdesc.MipLevels = 1u;
            rdesc.SampleDesc.Count = 1u;
            rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* ring = nullptr;
            if (SUCCEEDED(host_dev_->CreateCommittedResource(
                    &up_hp, D3D12_HEAP_FLAG_NONE, &rdesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&ring))) &&
                ring) {
                void* mapped_ring = nullptr;
                D3D12_RANGE zero_read{0u, 0u};
                if (SUCCEEDED(ring->Map(0u, &zero_read, &mapped_ring)) && mapped_ring) {
                    cbv_align_ring_ = ring;
                    ring_gpu = ring->GetGPUVirtualAddress();
                    ring_cpu = static_cast<uint8_t*>(mapped_ring);
                    cbv_align_ring_gpu_.store(ring_gpu, std::memory_order_relaxed);
                    cbv_align_ring_cpu_.store(ring_cpu, std::memory_order_release);
                } else {
                    ring->Release();
                }
            }
        } else {
            ring_gpu = cbv_align_ring_gpu_.load(std::memory_order_relaxed);
        }
    }
    if (!ring_cpu || ring_gpu == 0ull) {
        return translated_va & ~255ull;
    }

    // Concurrent shared-lock read across all command-list recording threads + per-thread
    // last-hit cache (O(1) hit for 99.9% of dynamic CBV draws) and O(log N) upper_bound fallback.
    struct ThreadLocalBufHint {
        const DeviceX* dev = nullptr;
        uint64_t gen = 0ull;
        uint64_t gpu_va = 0ull;
        uint64_t size_bytes = 0ull;
        const uint8_t* mapped_cpu = nullptr;
    };
    static thread_local ThreadLocalBufHint tl_hint{};

    std::shared_lock<std::shared_mutex> slock(buffer_va_mutex_);
    const uint64_t cur_gen = buffer_va_gen_.load(std::memory_order_acquire);
    const uint8_t* src_cpu = nullptr;
    uint64_t src_offset = 0ull;
    uint64_t src_total_size = 0ull;

    if (tl_hint.dev == this && tl_hint.gen == cur_gen && tl_hint.mapped_cpu != nullptr &&
        translated_va >= tl_hint.gpu_va &&
        translated_va < tl_hint.gpu_va + tl_hint.size_bytes) {
        src_cpu = tl_hint.mapped_cpu;
        src_offset = translated_va - tl_hint.gpu_va;
        src_total_size = tl_hint.size_bytes;
    } else {
        auto git = buffer_va_by_gpu_.upper_bound(translated_va);
        if (git != buffer_va_by_gpu_.begin()) {
            --git;
            auto mit = buffer_va_map_.find(git->second);
            if (mit != buffer_va_map_.end() &&
                mit->second.mapped_cpu_ptr != nullptr &&
                mit->second.guest_va == 0ull &&
                translated_va >= mit->second.gpu_va &&
                translated_va < mit->second.gpu_va + mit->second.size_bytes) {
                src_cpu = mit->second.mapped_cpu_ptr;
                src_offset = translated_va - mit->second.gpu_va;
                src_total_size = mit->second.size_bytes;
                tl_hint.dev = this;
                tl_hint.gen = cur_gen;
                tl_hint.gpu_va = mit->second.gpu_va;
                tl_hint.size_bytes = mit->second.size_bytes;
                tl_hint.mapped_cpu = src_cpu;
            }
        }
    }

    if (!src_cpu || src_offset >= src_total_size) {
        void* mirror_cpu = nullptr;
        uint64_t mirror_rem = 0ull;
        if (guest_va_fallback != 0ull &&
            title_va_mirror_.ResolveTitleVaToCpu(guest_va_fallback, mirror_cpu, mirror_rem) &&
            mirror_cpu && mirror_rem > 0ull) {
            src_cpu = static_cast<const uint8_t*>(mirror_cpu);
            src_offset = 0ull;
            src_total_size = mirror_rem;
        } else if (guest_va_fallback != 0ull) {
            const uint64_t cpu_va = ResolveGuestCpuAliasVa(guest_va_fallback);
            const uint64_t avail = MaxReadableGuestSpan(cpu_va, kSlotBytes);
            if (avail >= 16ull) {
                const uint32_t slot = cbv_align_ring_cursor_.fetch_add(1u, std::memory_order_relaxed) & (kSlotCount - 1u);
                const uint64_t slot_off = static_cast<uint64_t>(slot) * kSlotBytes;
                if (SafeCopyGuestMemory(
                        ring_cpu + slot_off,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu_va)),
                        static_cast<size_t>(avail))) {
                    const uint64_t pad_end = std::min<uint64_t>((avail + 255ull) & ~255ull, kSlotBytes);
                    if (pad_end > avail) {
                        std::memset(ring_cpu + slot_off + avail, 0, static_cast<size_t>(pad_end - avail));
                    }
                    return ring_gpu + slot_off;
                }
            }
            return translated_va & ~255ull;
        } else {
            return translated_va & ~255ull;
        }
    }

    const uint64_t copy_bytes = std::min<uint64_t>(kSlotBytes, src_total_size - src_offset);
    const uint32_t slot = cbv_align_ring_cursor_.fetch_add(1u, std::memory_order_relaxed) & (kSlotCount - 1u);
    const uint64_t slot_off = static_cast<uint64_t>(slot) * kSlotBytes;
    std::memcpy(ring_cpu + slot_off, src_cpu + src_offset, static_cast<size_t>(copy_bytes));
    if (copy_bytes < kSlotBytes) {
        const uint64_t pad_end = std::min<uint64_t>((copy_bytes + 255ull) & ~255ull, kSlotBytes);
        if (pad_end > copy_bytes) {
            std::memset(ring_cpu + slot_off + copy_bytes, 0,
                        static_cast<size_t>(pad_end - copy_bytes));
        }
    }
    return ring_gpu + slot_off;
}

uint64_t DeviceX::ResolveCpuAliasVa(uint64_t guest_va) const noexcept {
    return ResolveGuestCpuAliasVa(guest_va);
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::EnsureDummyRootUavBufferGpuVa() {
    if (!host_dev_) return 0ull;
    std::lock_guard<std::mutex> lk(dummy_root_uav_mutex_);
    if (dummy_root_uav_gpu_ != 0ull) return dummy_root_uav_gpu_;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 65536ull;
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = {1u, 0u};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* buf = nullptr;
    if (SUCCEEDED(host_dev_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&buf))) &&
        buf) {
        dummy_root_uav_buf_ = buf;
        dummy_root_uav_gpu_ = buf->GetGPUVirtualAddress();
    }
    return dummy_root_uav_gpu_;
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::ResolveOrUploadRootCbvGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS guest_va) {
    if (guest_va == 0ull) return 0ull;
    if (!retail_scarlett_abi_) {
        return AlignRootCbvGpuVaForDesktop(TranslateGuestGpuVa(guest_va), guest_va);
    }
    uint64_t off = 0ull;
    ID3D12Resource* res = ResolveBufferResourceByGpuVa(guest_va, &off);
    if (res && IsGpuWrittenBufferRange(res, off, 256ull)) {
        return AlignRootCbvGpuVaForDesktop(res->GetGPUVirtualAddress() + off, guest_va);
    }
    const uint64_t cpu_va = ResolveGuestCpuAliasVa(guest_va);
    const uint64_t span = MaxReadableGuestSpan(cpu_va, 4096ull);
    if (span >= 16ull) {
        const D3D12_GPU_VIRTUAL_ADDRESS up =
            UploadTransientGuestBufferForDesktop(cpu_va, span, 256ull, /*allow_same_frame_cache=*/true);
        if (up != 0ull) return up;
    }
    if (res) {
        return AlignRootCbvGpuVaForDesktop(res->GetGPUVirtualAddress() + off, guest_va);
    }
    return EnsureDummyRootUavBufferGpuVa();
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::ResolveOrUploadRootSrvGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS guest_va) {
    if (guest_va == 0ull) return 0ull;
    if (!retail_scarlett_abi_) {
        return TranslateGuestGpuVa(guest_va);
    }
    uint64_t off = 0ull;
    ID3D12Resource* res = ResolveBufferResourceByGpuVa(guest_va, &off);
    if (res && IsGpuWrittenBufferRange(res, off, 256ull)) {
        return (res->GetGPUVirtualAddress() + off) & ~3ull;
    }
    const uint64_t cpu_va = ResolveGuestCpuAliasVa(guest_va);
    const uint64_t span = MaxReadableGuestSpan(cpu_va, 262144ull) & ~3ull;
    if (span >= 4ull) {
        const D3D12_GPU_VIRTUAL_ADDRESS up =
            UploadTransientGuestBufferForDesktop(cpu_va, span, 16ull, /*allow_same_frame_cache=*/true);
        if (up != 0ull) return up;
    }
    if (res) {
        return (res->GetGPUVirtualAddress() + off) & ~3ull;
    }
    return EnsureDummyRootUavBufferGpuVa();
}

D3D12_GPU_VIRTUAL_ADDRESS DeviceX::ResolveOrDummyRootUavGpuVa(
    D3D12_GPU_VIRTUAL_ADDRESS guest_va) {
    if (guest_va == 0ull) return 0ull;
    if (!retail_scarlett_abi_) {
        return TranslateGuestGpuVa(guest_va);
    }
    uint64_t off = 0ull;
    if (ID3D12Resource* res = ResolveBufferResourceByGpuVa(guest_va, &off)) {
        if ((res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            return (res->GetGPUVirtualAddress() + off) & ~3ull;
        }
    }
    return EnsureDummyRootUavBufferGpuVa();
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
    uint64_t* out_bytes_uploaded,
    bool wait_for_gpu) {
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

    static std::atomic<uint32_t> s_mirror_probe_tick{0u};
    const uint32_t mp_tick = s_mirror_probe_tick.fetch_add(1u, std::memory_order_relaxed);
    const bool probe_non_ww_mirror = !retail_scarlett_abi_ || candidate_vas.size() <= 32u || ((mp_tick & 7u) == 0u);
    std::vector<uint64_t> dirty_vas;
    std::vector<openscarlett::DirtyPageRange> probe_ranges;
    for (uint64_t va : candidate_vas) {
        openscarlett::TitleVaRegionInfo rinfo{};
        if (!title_va_mirror_.LookupRegion(va, rinfo)) continue;
        if (!rinfo.write_watch_enabled && !probe_non_ww_mirror) continue;
        probe_ranges.clear();
        if (title_va_mirror_.QueryDirtyPageRanges(va, /*reset_watch=*/false, probe_ranges).ok() &&
            !probe_ranges.empty()) {
            dirty_vas.push_back(va);
        }
    }
    if (dirty_vas.empty()) return S_OK;

    std::lock_guard<std::mutex> upload_lock(unified_upload_mutex_);
    uint32_t ring_slot_idx = 0u;
    HRESULT hr = AcquireUnifiedUploadRingSlotLocked(0ull, ring_slot_idx);
    if (FAILED(hr)) return hr;
    auto& ring_slot = unified_upload_ring_[ring_slot_idx];
    hr = ring_slot.alloc->Reset();
    if (SUCCEEDED(hr)) {
        hr = ring_slot.cmd->Reset(ring_slot.alloc, nullptr);
    }
    if (FAILED(hr)) return hr;

    uint64_t total_uploaded = 0ull;
    uint64_t regions_flushed = 0ull;
    for (uint64_t va : dirty_vas) {
        openscarlett::TitleVaRegionInfo rinfo{};
        if (!title_va_mirror_.LookupRegion(va, rinfo)) continue;
        openscarlett::FlushStats fstats{};
        if (title_va_mirror_.FlushDirtyPagesToGpu(
                ring_slot.cmd, va, &fstats, rinfo.current_state).ok()) {
            total_uploaded += fstats.bytes_uploaded;
            regions_flushed += fstats.regions_flushed;
        }
    }

    hr = ring_slot.cmd->Close();
    if (FAILED(hr)) return hr;

    if (regions_flushed > 0ull) {
        ID3D12CommandList* lists[] = {ring_slot.cmd};
        host_queue->ExecuteCommandLists(1u, lists);
        if (host_dev_) {
            const HRESULT mrem = host_dev_->GetDeviceRemovedReason();
            if (FAILED(mrem)) {
                static std::atomic<bool> s_mr{false};
                if (!s_mr.exchange(true)) {
                    std::fprintf(stderr, "[OpenScarlett][MIRROR-REMOVED] rem=0x%08lX regions=%llu bytes=%llu\n",
                                 static_cast<unsigned long>(mrem),
                                 static_cast<unsigned long long>(regions_flushed),
                                 static_cast<unsigned long long>(total_uploaded));
                    std::fflush(stderr);
                }
            }
        }
        const uint64_t target = ++unified_fence_val_;
        hr = host_queue->Signal(unified_fence_, target);
        ring_slot.fence_val = target;
        if (wait_for_gpu && SUCCEEDED(hr) && unified_fence_->GetCompletedValue() < target) {
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
    if (!host_resource || tile_mode_count_.load(std::memory_order_acquire) == 0u) {
        return XG_TILE_MODE_LINEAR;
    }
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
    tile_mode_count_.store(
        static_cast<uint32_t>(resource_tile_modes_.size()), std::memory_order_release);
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
        std::unique_lock<std::shared_mutex> lock(buffer_va_mutex_);
        for (auto& [res, entry] : buffer_va_map_) {
            if (res && entry.owns_persistent_map && entry.mapped_cpu_ptr) {
                D3D12_RANGE no_write{0u, 0u};
                res->Unmap(0u, &no_write);
            }
        }
        buffer_va_map_.clear();
        buffer_va_by_gpu_.clear();
        buffer_va_gen_.fetch_add(1ull, std::memory_order_release);
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
    const uint32_t array_size =
        (dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            ? 1u
            : std::max<uint32_t>(1u, dst_desc.DepthOrArraySize);
    const uint32_t mip_level = dst_loc.SubresourceIndex % mips;
    const uint32_t slice_index = (dst_loc.SubresourceIndex / mips) % array_size;
    const uint32_t plane_slice = dst_loc.SubresourceIndex / (mips * array_size);

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
        xg2d.ArraySize = array_size;
        xg2d.Format = static_cast<uint32_t>(dst_desc.Format);
        xg2d.SampleCount = std::max<uint32_t>(1u, dst_desc.SampleDesc.Count);
        xg2d.TileMode = tile_mode;
        xg2d.BindFlags = XG_BIND_SHADER_RESOURCE;

        openscarlett::XgGpuSurface2DRequest req{};
        req.desc = xg2d;
        req.plane = plane_slice;
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
        (src_hp.Type != D3D12_HEAP_TYPE_UPLOAD &&
         !(src_hp.Type == D3D12_HEAP_TYPE_CUSTOM &&
           src_hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE))) {
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
            ok = openscarlett::xg::UntileSurface2DPlane(
                xg2d, plane_slice, mip_level, slice_index, tiled_src, tiled_bytes,
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

// ---------------------------------------------------------------------------
// Sprint 14: Live VRAM Residency Controller & IOpenScarlettVramResidencyControl
// ---------------------------------------------------------------------------
uint64_t DeviceX::EstimateResourceVramBytes(const D3D12_RESOURCE_DESC& desc) const noexcept {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return AlignUp64(std::max<uint64_t>(desc.Width, 65536ull), 65536ull);
    }
    if (host_dev_) {
        const D3D12_RESOURCE_ALLOCATION_INFO info = host_dev_->GetResourceAllocationInfo(0u, 1u, &desc);
        if (info.SizeInBytes > 0ull && info.SizeInBytes != UINT64_MAX) {
            return info.SizeInBytes;
        }
    }
    const FormatFootprintSpec spec = DescribeFormatForFootprint(desc.Format);
    const uint64_t w = std::max<uint64_t>(1ull, desc.Width);
    const uint64_t h = std::max<uint64_t>(1ull, desc.Height);
    const uint64_t d = std::max<uint64_t>(1ull, desc.DepthOrArraySize);
    const uint64_t blocks_x = (w + spec.block_width - 1ull) / spec.block_width;
    const uint64_t blocks_y = (h + spec.block_height - 1ull) / spec.block_height;
    return AlignUp64(std::max<uint64_t>(blocks_x * blocks_y * d * spec.bytes_per_block, 65536ull), 65536ull);
}

void DeviceX::RefreshDxgiVideoMemoryInfoLocked() noexcept {
    if (host_dev_) {
        (void)openscarlett::QueryHostAdapterVideoMemoryInfo(host_dev_, dxgi_mem_info_);
    }
}

uint64_t DeviceX::EffectiveResidencyBudgetBytesLocked() const noexcept {
    if (residency_budget_override_bytes_ > 0ull) {
        return residency_budget_override_bytes_;
    }
    if (dxgi_mem_info_.queried_dxgi_adapter3 &&
        (dxgi_mem_info_.local_budget_bytes > 0ull || dxgi_mem_info_.non_local_budget_bytes > 0ull)) {
        // Integrated UMA GPUs (e.g. Intel Arc B390 reporting 128 MB dedicated VRAM) expose
        // their full unified pool via non-local or combined DXGI segments.
        constexpr uint64_t kUmaLocalThreshold = 512ull * 1024ull * 1024ull;
        uint64_t host_budget = dxgi_mem_info_.local_budget_bytes;
        if (host_budget < kUmaLocalThreshold && dxgi_mem_info_.non_local_budget_bytes > host_budget) {
            host_budget = dxgi_mem_info_.non_local_budget_bytes;
        }
        return std::min<uint64_t>(host_budget, console_profile_.target_console_vram_bytes);
    }
    if (adapter_info_.dedicated_video_memory_bytes > 0ull) {
        const uint64_t safe_host = (adapter_info_.dedicated_video_memory_bytes * 85ull) / 100ull;
        return std::min<uint64_t>(safe_host, console_profile_.target_console_vram_bytes);
    }
    return console_profile_.target_console_vram_bytes;
}

bool DeviceX::IsResidencyEntryLiveProtectedLocked(const LiveResidencyEntry& e) const noexcept {
    if (e.priority == D3D12_RESIDENCY_PRIORITY_MAXIMUM) return true;
    if (e.active_pin_count > 0u) return true;
    if (e.residency_ref_count > 1u) return true;  // Explicitly pinned via MakeResident
    if (e.is_heap && e.placed_child_count > 0u) return true;  // Live heap backing placed resources
    if (e.in_flight_fence != nullptr &&
        e.in_flight_fence->GetCompletedValue() < e.in_flight_fence_value) {
        return true;  // Referenced by in-flight GPU work
    }
    return false;
}

uint64_t DeviceX::EvictLruVictimsLocked(uint64_t bytes_needed, ID3D12Pageable* protect_obj) noexcept {
    if (!host_dev_ || bytes_needed == 0ull || residency_lru_.empty()) return 0ull;
    uint64_t freed = 0ull;

    // Two-pass priority-aware LRU eviction with Sprint 16 live-heap hygiene:
    // NEVER auto-evict actively-referenced, MakeResident-pinned, placed-child-backing,
    // MAXIMUM-priority, or in-flight GPU heaps/resources!
    for (int pass = 0; pass < 2 && freed < bytes_needed; ++pass) {
        std::vector<ID3D12Pageable*> victims;
        for (auto it = residency_lru_.rbegin(); it != residency_lru_.rend() && freed < bytes_needed; ++it) {
            ID3D12Pageable* cand = *it;
            if (!cand || cand == protect_obj) continue;
            auto eit = residency_entries_.find(cand);
            if (eit == residency_entries_.end()) continue;
            const auto& e = eit->second;
            if (!e.is_resident || e.is_placed_suballocation || e.spilled_to_l0) continue;
            if (IsResidencyEntryLiveProtectedLocked(e)) {
                if (pass == 0) ++skipped_active_live_heaps_;
                continue;
            }
            if (pass == 0 && e.priority == D3D12_RESIDENCY_PRIORITY_HIGH) continue;
            victims.push_back(cand);
            freed += e.size_bytes;
        }
        for (ID3D12Pageable* v : victims) {
            auto eit = residency_entries_.find(v);
            if (eit == residency_entries_.end() || !eit->second.is_resident) continue;
            auto& e = eit->second;
            if (IsResidencyEntryLiveProtectedLocked(e)) {
                ++skipped_active_live_heaps_;
                continue;
            }
            const HRESULT ev_hr = host_dev_->Evict(1u, &v);
            if (SUCCEEDED(ev_hr)) {
                e.is_resident = false;
                e.residency_ref_count = 0u;
                if (e.in_lru) {
                    residency_lru_.erase(e.lru_it);
                    e.in_lru = false;
                }
                if (tracked_resident_bytes_ >= e.size_bytes) tracked_resident_bytes_ -= e.size_bytes;
                else tracked_resident_bytes_ = 0ull;
                tracked_evicted_bytes_ += e.size_bytes;
                ++auto_lru_evict_count_;
                ++evict_calls_;
            }
        }
    }
    return freed;
}

void DeviceX::PrepareResidencyBudgetForAllocation(
    uint64_t requested_bytes,
    bool allow_l0_spill,
    bool& out_should_spill_l0) {
    out_should_spill_l0 = false;
    if (requested_bytes == 0ull) return;

    std::lock_guard<std::mutex> lock(residency_mutex_);
    // Sprint 16 VRAM Residency Hygiene:
    // Never auto-evict live heaps unless an explicit budget cap is actually exceeded
    // (or real tracked usage exceeds the host DXGI / console VRAM budget).
    const uint64_t budget = EffectiveResidencyBudgetBytesLocked();
    if (budget == 0ull) return;

    // Only auto-evict live tracked objects via host ID3D12Device::Evict when an explicit
    // synthetic residency budget override is active (unit tests). In real gameplay, calling
    // Evict() on older resident resources/heaps without per-command-list MakeResident() causes
    // WDDM/GPU page faults -> DXGI_ERROR_INVALID_CALL / DXGI_ERROR_DEVICE_REMOVED; WDDM
    // already safely pages over-budget DEFAULT allocations and L0 spill keeps new cold buffers resident.
    if (residency_budget_override_bytes_ > 0ull && tracked_resident_bytes_ + requested_bytes > budget) {
        const uint64_t over = (tracked_resident_bytes_ + requested_bytes) - budget;
        (void)EvictLruVictimsLocked(over, nullptr);
    }
    // If still over budget after evicting eligible unpinned LRU victims, spill cold allocation to L0.
    if (allow_l0_spill && (tracked_resident_bytes_ + requested_bytes > budget)) {
        out_should_spill_l0 = true;
    }
}

void DeviceX::TrackLiveResidencyObject(
    ID3D12Pageable* host_obj,
    uint64_t size_bytes,
    D3D12_HEAP_TYPE heap_type,
    bool is_placed_suballocation,
    bool spilled_to_l0,
    uint64_t sparse_window_va,
    bool is_heap,
    ID3D12Pageable* parent_heap) {
    if (!host_obj) return;
    std::lock_guard<std::mutex> lock(residency_mutex_);
    auto it = residency_entries_.find(host_obj);
    if (it != residency_entries_.end()) {
        if (it->second.in_lru) {
            residency_lru_.erase(it->second.lru_it);
        }
        if (it->second.in_flight_fence) {
            it->second.in_flight_fence->Release();
            it->second.in_flight_fence = nullptr;
        }
        if (it->second.parent_heap) {
            auto pit = residency_entries_.find(it->second.parent_heap);
            if (pit != residency_entries_.end() && pit->second.placed_child_count > 0u) {
                --pit->second.placed_child_count;
            }
        }
        if (it->second.sparse_window_va != 0ull && sparse_window_iso_) {
            openscarlett::SparseVramFree(sparse_window_iso_, it->second.sparse_window_va);
        }
        residency_entries_.erase(it);
    }

    LiveResidencyEntry e{};
    e.host_obj = host_obj;
    e.size_bytes = size_bytes;
    e.sparse_window_va = sparse_window_va;
    e.heap_type = heap_type;
    e.priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
    e.residency_ref_count = 1u;
    e.is_heap = is_heap;
    e.parent_heap = parent_heap;
    e.is_resident = true;
    e.is_placed_suballocation = is_placed_suballocation;
    e.spilled_to_l0 = spilled_to_l0;
    if (parent_heap) {
        auto pit = residency_entries_.find(parent_heap);
        if (pit != residency_entries_.end()) {
            ++pit->second.placed_child_count;
        }
    }
    if (!is_placed_suballocation) {
        if (spilled_to_l0) {
            l0_spilled_bytes_ += size_bytes;
            ++l0_spill_count_;
        } else {
            tracked_resident_bytes_ += size_bytes;
            residency_lru_.push_front(host_obj);
            e.lru_it = residency_lru_.begin();
            e.in_lru = true;
        }
    }
    residency_entries_.emplace(host_obj, e);
}

void DeviceX::UntrackLiveResidencyObject(ID3D12Pageable* host_obj) noexcept {
    if (!host_obj) return;
    std::lock_guard<std::mutex> lock(residency_mutex_);
    auto it = residency_entries_.find(host_obj);
    if (it == residency_entries_.end()) return;
    auto& e = it->second;
    if (e.in_lru) {
        residency_lru_.erase(e.lru_it);
    }
    if (e.in_flight_fence) {
        e.in_flight_fence->Release();
        e.in_flight_fence = nullptr;
    }
    if (e.parent_heap) {
        auto pit = residency_entries_.find(e.parent_heap);
        if (pit != residency_entries_.end() && pit->second.placed_child_count > 0u) {
            --pit->second.placed_child_count;
        }
    }
    if (e.sparse_window_va != 0ull && sparse_window_iso_) {
        openscarlett::SparseVramFree(sparse_window_iso_, e.sparse_window_va);
    }
    if (!e.is_placed_suballocation) {
        if (e.spilled_to_l0) {
            if (l0_spilled_bytes_ >= e.size_bytes) l0_spilled_bytes_ -= e.size_bytes;
            else l0_spilled_bytes_ = 0ull;
        } else if (e.is_resident) {
            if (tracked_resident_bytes_ >= e.size_bytes) tracked_resident_bytes_ -= e.size_bytes;
            else tracked_resident_bytes_ = 0ull;
        } else {
            if (tracked_evicted_bytes_ >= e.size_bytes) tracked_evicted_bytes_ -= e.size_bytes;
            else tracked_evicted_bytes_ = 0ull;
        }
    }
    residency_entries_.erase(it);
}

HRESULT STDMETHODCALLTYPE DeviceX::GetVramResidencyStats(
    D3D12XBOX_VRAM_RESIDENCY_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));
    std::lock_guard<std::mutex> lock(residency_mutex_);
    RefreshDxgiVideoMemoryInfoLocked();
    out_stats->console_profile = console_profile_.profile_id;
    out_stats->queried_dxgi_adapter3 = dxgi_mem_info_.queried_dxgi_adapter3 ? 1u : 0u;
    out_stats->target_console_vram_bytes = console_profile_.target_console_vram_bytes;
    out_stats->host_dedicated_vram_bytes = adapter_info_.dedicated_video_memory_bytes;
    out_stats->dxgi_local_budget_bytes = dxgi_mem_info_.local_budget_bytes;
    out_stats->dxgi_local_current_usage_bytes = dxgi_mem_info_.local_current_usage_bytes;
    out_stats->dxgi_non_local_budget_bytes = dxgi_mem_info_.non_local_budget_bytes;
    out_stats->effective_budget_bytes = EffectiveResidencyBudgetBytesLocked();
    out_stats->tracked_resident_bytes = tracked_resident_bytes_;
    out_stats->tracked_evicted_bytes = tracked_evicted_bytes_;
    out_stats->l0_spilled_bytes = l0_spilled_bytes_;
    out_stats->tracked_object_count = static_cast<uint64_t>(residency_entries_.size());
    uint64_t res_cnt = 0ull;
    uint64_t ev_cnt = 0ull;
    for (const auto& [ptr, e] : residency_entries_) {
        (void)ptr;
        if (e.is_resident) ++res_cnt;
        else ++ev_cnt;
    }
    out_stats->resident_object_count = res_cnt;
    out_stats->evicted_object_count = ev_cnt;
    out_stats->l0_spill_count = l0_spill_count_;
    out_stats->auto_lru_eviction_count = auto_lru_evict_count_;
    out_stats->make_resident_calls = make_resident_calls_;
    out_stats->evict_calls = evict_calls_;
    out_stats->set_priority_calls = set_priority_calls_;
    out_stats->enqueue_make_resident_calls = enqueue_make_resident_calls_;
    openscarlett::SparseVramGetStats(
        sparse_window_iso_,
        out_stats->window0_descriptor_allocs,
        out_stats->window0_descriptor_bytes,
        out_stats->window1_title_buffer_allocs,
        out_stats->window1_title_buffer_bytes);
    out_stats->promoted_oversized_placed_resources =
        promoted_oversized_placed_resources_.load(std::memory_order_relaxed);
    out_stats->skipped_active_live_heaps = skipped_active_live_heaps_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetResidencyBudgetOverride(
    uint64_t budget_bytes_or_zero_for_auto) {
    std::lock_guard<std::mutex> lock(residency_mutex_);
    residency_budget_override_bytes_ = budget_bytes_or_zero_for_auto;
    if (residency_budget_override_bytes_ > 0ull &&
        tracked_resident_bytes_ > residency_budget_override_bytes_) {
        (void)EvictLruVictimsLocked(tracked_resident_bytes_ - residency_budget_override_bytes_, nullptr);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::SetConsoleProfileOverride(
    int32_t profile_or_neg1_for_auto) {
    std::lock_guard<std::mutex> lock(residency_mutex_);
    if (profile_or_neg1_for_auto == 0) {
        console_profile_ = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(
            adapter_info_.dedicated_video_memory_bytes, "series_s");
    } else if (profile_or_neg1_for_auto == 1) {
        console_profile_ = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(
            adapter_info_.dedicated_video_memory_bytes, "series_x");
    } else {
        console_profile_ = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(
            adapter_info_.dedicated_video_memory_bytes, nullptr);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetAgilitySdkInfo(
    D3D12XBOX_AGILITY_SDK_INFO* out_info) {
    if (!out_info) return E_POINTER;
    QueryAgilitySdkRuntimeSnapshot(out_info);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetPsoStreamDiagStats(
    D3D12XBOX_PSO_STREAM_DIAG_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    out_stats->remapped_as_tags = pso_remapped_as_tags_.load(std::memory_order_relaxed);
    out_stats->remapped_ms_tags = pso_remapped_ms_tags_.load(std::memory_order_relaxed);
    out_stats->remapped_ext_tags = pso_remapped_ext_tags_.load(std::memory_order_relaxed);
    out_stats->dropped_legacy_stages = pso_dropped_legacy_stages_.load(std::memory_order_relaxed);
    out_stats->inferred_mesh_topologies = pso_inferred_mesh_topologies_.load(std::memory_order_relaxed);
    out_stats->f16_stage_links_reconciled = pso_f16_links_reconciled_.load(std::memory_order_relaxed);
    out_stats->interp_validations_run = pso_interp_validations_.load(std::memory_order_relaxed);
    out_stats->interp_attributes_matched = pso_interp_matches_.load(std::memory_order_relaxed);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::AllocateSparseWindowVa(
    uint32_t is_descriptor_window0,
    uint64_t size_bytes,
    uint64_t* out_va) {
    if (!out_va) return E_POINTER;
    *out_va = 0ull;
    if (!sparse_window_iso_ || size_bytes == 0ull) return E_INVALIDARG;
    const uint64_t va = is_descriptor_window0
                            ? openscarlett::SparseVramAllocateWindow0(sparse_window_iso_, size_bytes)
                            : openscarlett::SparseVramAllocateTitleWindow(sparse_window_iso_, size_bytes);
    if (va == 0ull) return E_OUTOFMEMORY;
    *out_va = va;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::FreeSparseWindowVa(uint64_t va) {
    if (va == 0ull) return E_INVALIDARG;
    openscarlett::SparseVramFree(sparse_window_iso_, va);
    return S_OK;
}

bool DeviceX::ClampMsaaSampleDescForFormat(
    DXGI_FORMAT format,
    DXGI_SAMPLE_DESC& sample_desc) noexcept {
    bool changed = false;
    if (sample_desc.Count == 0u) {
        sample_desc.Count = 1u;
        sample_desc.Quality = 0u;
        return true;
    }
    if (sample_desc.Count <= 1u) {
        if (sample_desc.Quality != 0u) {
            sample_desc.Quality = 0u;
            msaa_quality_clamps_.fetch_add(1ull, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (!host_dev_) {
        return false;
    }

    DXGI_FORMAT probe_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (format) {
        case DXGI_FORMAT_UNKNOWN:
            probe_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            probe_fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
            break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            probe_fmt = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            break;
        case DXGI_FORMAT_D32_FLOAT:
            probe_fmt = DXGI_FORMAT_D32_FLOAT;
            break;
        case DXGI_FORMAT_D16_UNORM:
            probe_fmt = DXGI_FORMAT_D16_UNORM;
            break;
        default:
            probe_fmt = MapTypelessFormatForSrv(format);
            break;
    }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa{};
    msaa.Format = probe_fmt;
    msaa.SampleCount = sample_desc.Count;
    msaa.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    const HRESULT hr = host_dev_->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof(msaa));
    if (FAILED(hr) || msaa.NumQualityLevels == 0u) {
        // Requested sample count (e.g. 16x MSAA on an 8x-max desktop GPU) is unsupported
        // for this format; step down to the highest supported MSAA count (8 -> 4 -> 2 -> 1)
        // so both resources and PSOs stay valid and mutually consistent.
        uint32_t fallback_count = 1u;
        uint32_t fallback_levels = 0u;
        constexpr uint32_t kFallbackCounts[] = {8u, 4u, 2u};
        for (uint32_t cand : kFallbackCounts) {
            if (cand >= sample_desc.Count) {
                continue;
            }
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS probe{};
            probe.Format = probe_fmt;
            probe.SampleCount = cand;
            probe.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
            if (SUCCEEDED(host_dev_->CheckFeatureSupport(
                    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &probe, sizeof(probe))) &&
                probe.NumQualityLevels > 0u) {
                fallback_count = cand;
                fallback_levels = probe.NumQualityLevels;
                break;
            }
        }
        sample_desc.Count = fallback_count;
        msaa.SampleCount = fallback_count;
        msaa.NumQualityLevels = fallback_levels;
        changed = true;
    }

    if (sample_desc.Count <= 1u || msaa.NumQualityLevels == 0u) {
        if (sample_desc.Quality != 0u) {
            sample_desc.Quality = 0u;
            changed = true;
        }
    } else if (sample_desc.Quality == DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN ||
               sample_desc.Quality == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN) {
        const bool standard_count_ok =
            (sample_desc.Count == 2u || sample_desc.Count == 4u ||
             sample_desc.Count == 8u || sample_desc.Count == 16u);
        if (sample_desc.Quality == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN ||
            !standard_count_ok || msaa.NumQualityLevels <= 1u) {
            sample_desc.Quality = 0u;
            changed = true;
        }
    } else if (sample_desc.Quality >= msaa.NumQualityLevels) {
        sample_desc.Quality = msaa.NumQualityLevels - 1u;
        changed = true;
    }

    if (changed) {
        msaa_quality_clamps_.fetch_add(1ull, std::memory_order_relaxed);
    }
    return changed;
}

D3D12_RESOURCE_DESC DeviceX::SanitizeAndClampResourceDesc(const D3D12_RESOURCE_DESC& desc) noexcept {
    D3D12_RESOURCE_DESC out = desc;
    if (out.SampleDesc.Count == 0u) {
        out.SampleDesc.Count = 1u;
        out.SampleDesc.Quality = 0u;
    }
    bool stripped_flags = false;
    out.Flags = SanitizeResourceFlagsForDimension(
        out.Dimension, desc.Flags, out.SampleDesc.Count, &stripped_flags);
    if ((out.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && out.SampleDesc.Count <= 1u) {
        if (out.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            out.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        } else if (out.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            out.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        }
    }
    bool stripped_layout = false;
    if (static_cast<uint32_t>(out.Layout) > static_cast<uint32_t>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)) {
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        stripped_layout = true;
    }
    if (stripped_flags || stripped_layout) {
        console_flags_stripped_.fetch_add(1ull, std::memory_order_relaxed);
    }
    if (out.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        (void)ClampMsaaSampleDescForFormat(out.Format, out.SampleDesc);
    }
    return out;
}

D3D12_RESOURCE_DESC1 DeviceX::SanitizeAndClampResourceDesc1(const D3D12_RESOURCE_DESC1& desc) noexcept {
    D3D12_RESOURCE_DESC1 out = desc;
    if (out.SampleDesc.Count == 0u) {
        out.SampleDesc.Count = 1u;
        out.SampleDesc.Quality = 0u;
    }
    bool stripped_flags = false;
    out.Flags = SanitizeResourceFlagsForDimension(
        out.Dimension, desc.Flags, out.SampleDesc.Count, &stripped_flags);
    if ((out.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && out.SampleDesc.Count <= 1u) {
        if (out.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            out.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        } else if (out.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            out.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        }
    }
    bool stripped_layout = false;
    if (static_cast<uint32_t>(out.Layout) > static_cast<uint32_t>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)) {
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        stripped_layout = true;
    }
    if (stripped_flags || stripped_layout) {
        console_flags_stripped_.fetch_add(1ull, std::memory_order_relaxed);
    }
    if (out.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        (void)ClampMsaaSampleDescForFormat(out.Format, out.SampleDesc);
    }
    return out;
}

HRESULT STDMETHODCALLTYPE DeviceX::GetGraphicsParityStats(
    D3D12XBOX_GRAPHICS_PARITY_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->promoted_oversized_placed_resources =
        promoted_oversized_placed_resources_.load(std::memory_order_relaxed);
    out_stats->msaa_quality_clamps =
        msaa_quality_clamps_.load(std::memory_order_relaxed);
    out_stats->console_flags_stripped =
        console_flags_stripped_.load(std::memory_order_relaxed);
    out_stats->reserved_cross_adapter_preserved =
        reserved_cross_adapter_preserved_.load(std::memory_order_relaxed);
    out_stats->reserved_cross_adapter_stripped_fallback =
        reserved_cross_adapter_stripped_fallback_.load(std::memory_order_relaxed);
    out_stats->global_uav_barriers_recorded =
        global_uav_barriers_recorded_.load(std::memory_order_relaxed);
    out_stats->unsupported_cmdlist_extensions_ignored =
        unsupported_cmdlist_extensions_ignored_.load(std::memory_order_relaxed);
    out_stats->zero_count_noop_calls =
        zero_count_noop_calls_.load(std::memory_order_relaxed);
    out_stats->readonly_dsv_creations =
        readonly_dsv_creations_.load(std::memory_order_relaxed);
    out_stats->depth_stencil_plane_copies =
        depth_stencil_plane_copies_.load(std::memory_order_relaxed);
    out_stats->footprint_offset_copies =
        footprint_offset_copies_.load(std::memory_order_relaxed);
    out_stats->vline_notifications_signaled =
        vline_notifications_signaled_.load(std::memory_order_relaxed);
    out_stats->frame_notifications_signaled =
        frame_notifications_signaled_.load(std::memory_order_relaxed);
    return S_OK;
}

// ============================================================================
// Sprint 16: Page-at-a-Time Descriptor Heaps, Unbounded Sampler Heap (>2048),
// Live GPU FMASK/DCC/HTILE Resolve Wiring, & Active Residency Protection
// ============================================================================

DeviceX::SamplerDescKey DeviceX::CanonicalizeSamplerDesc(const D3D12_SAMPLER_DESC& desc) noexcept {
    D3D12_SAMPLER_DESC c = desc;
    auto norm_f = [](FLOAT f) noexcept -> FLOAT { return (f == 0.0f) ? 0.0f : f; };
    c.MipLODBias = norm_f(c.MipLODBias);
    c.MinLOD = norm_f(c.MinLOD);
    c.MaxLOD = norm_f(c.MaxLOD);
    for (int i = 0; i < 4; ++i) c.BorderColor[i] = norm_f(c.BorderColor[i]);
    if (c.AddressU != D3D12_TEXTURE_ADDRESS_MODE_BORDER &&
        c.AddressV != D3D12_TEXTURE_ADDRESS_MODE_BORDER &&
        c.AddressW != D3D12_TEXTURE_ADDRESS_MODE_BORDER) {
        c.BorderColor[0] = c.BorderColor[1] = c.BorderColor[2] = c.BorderColor[3] = 0.0f;
    }
    SamplerDescKey key{};
    std::memcpy(key.bytes, &c, sizeof(c));
    return key;
}

bool DeviceX::TryHandleVirtualSamplerCreate(
    const D3D12_SAMPLER_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) noexcept {
    if (!pDesc || DestDescriptor.ptr == 0u) return false;
    std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
    for (auto& [host_heap, vs] : virtual_sampler_heaps_) {
        (void)host_heap;
        const uint64_t base = static_cast<uint64_t>(vs.virt_cpu_start.ptr);
        const uint64_t end = base + static_cast<uint64_t>(vs.virtual_desc.NumDescriptors) * vs.inc_size;
        const uint64_t ptr = static_cast<uint64_t>(DestDescriptor.ptr);
        if (ptr < base || ptr >= end) continue;
        const uint32_t virt_slot = static_cast<uint32_t>((ptr - base) / vs.inc_size);
        if (virt_slot >= vs.virtual_desc.NumDescriptors) return true;

        const SamplerDescKey key = CanonicalizeSamplerDesc(*pDesc);
        unbounded_sampler_writes_.fetch_add(1ull, std::memory_order_relaxed);
        auto it = vs.dedup_map.find(key);
        uint16_t host_slot = 0u;
        if (it != vs.dedup_map.end()) {
            host_slot = it->second;
            unbounded_sampler_dedup_hits_.fetch_add(1ull, std::memory_order_relaxed);
        } else {
            if (vs.next_host_slot < vs.host_capacity) {
                host_slot = static_cast<uint16_t>(vs.next_host_slot++);
                unbounded_sampler_unique_host_slots_.fetch_add(1ull, std::memory_order_relaxed);
            } else {
                // Beyond 2048 truly distinct sampler states: wrap deterministically into backing slots.
                host_slot = static_cast<uint16_t>(virt_slot % vs.host_capacity);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE host_dst{
                vs.host_cpu_start.ptr + static_cast<SIZE_T>(host_slot) * vs.inc_size};
            host_dev_->CreateSampler(pDesc, host_dst);
            vs.dedup_map[key] = host_slot;
        }
        vs.virt_to_host_slot[virt_slot] = host_slot;
        vs.virt_slot_keys[virt_slot] = key;
        vs.virt_slot_valid[virt_slot] = 1u;
        return true;
    }
    return false;
}

bool DeviceX::TryHandleVirtualSamplerCopy(
    UINT NumDestDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
    const UINT* pDestDescriptorRangeSizes,
    UINT NumSrcDescriptorRanges,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
    const UINT* pSrcDescriptorRangeSizes) noexcept {
    if (!pDestDescriptorRangeStarts || !pSrcDescriptorRangeStarts ||
        NumDestDescriptorRanges == 0u || NumSrcDescriptorRanges == 0u) {
        return false;
    }
    std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
    if (virtual_sampler_heaps_.empty()) return false;

    auto find_virtual = [&](SIZE_T ptr, VirtualSamplerHeapState*& out_vs, uint32_t& out_slot) -> bool {
        for (auto& [host_heap, vs] : virtual_sampler_heaps_) {
            (void)host_heap;
            const uint64_t base = static_cast<uint64_t>(vs.virt_cpu_start.ptr);
            const uint64_t end = base + static_cast<uint64_t>(vs.virtual_desc.NumDescriptors) * vs.inc_size;
            if (static_cast<uint64_t>(ptr) >= base && static_cast<uint64_t>(ptr) < end) {
                out_vs = &vs;
                out_slot = static_cast<uint32_t>((static_cast<uint64_t>(ptr) - base) / vs.inc_size);
                return true;
            }
        }
        return false;
    };

    bool any_virtual = false;
    for (UINT r = 0u; r < NumDestDescriptorRanges && !any_virtual; ++r) {
        VirtualSamplerHeapState* vs = nullptr;
        uint32_t slot = 0u;
        if (find_virtual(pDestDescriptorRangeStarts[r].ptr, vs, slot)) any_virtual = true;
    }
    for (UINT r = 0u; r < NumSrcDescriptorRanges && !any_virtual; ++r) {
        VirtualSamplerHeapState* vs = nullptr;
        uint32_t slot = 0u;
        if (find_virtual(pSrcDescriptorRangeStarts[r].ptr, vs, slot)) any_virtual = true;
    }
    if (!any_virtual) return false;

    const UINT inc = std::max<UINT>(
        1u, host_dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
    UINT dst_r = 0u, dst_i = 0u, src_r = 0u, src_i = 0u;
    while (dst_r < NumDestDescriptorRanges && src_r < NumSrcDescriptorRanges) {
        const UINT dst_cnt = pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[dst_r] : 1u;
        const UINT src_cnt = pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[src_r] : 1u;
        if (dst_i >= dst_cnt) { ++dst_r; dst_i = 0u; continue; }
        if (src_i >= src_cnt) { ++src_r; src_i = 0u; continue; }

        const D3D12_CPU_DESCRIPTOR_HANDLE dst_h{
            pDestDescriptorRangeStarts[dst_r].ptr + static_cast<SIZE_T>(dst_i) * inc};
        const D3D12_CPU_DESCRIPTOR_HANDLE src_h{
            pSrcDescriptorRangeStarts[src_r].ptr + static_cast<SIZE_T>(src_i) * inc};

        VirtualSamplerHeapState* dst_vs = nullptr;
        VirtualSamplerHeapState* src_vs = nullptr;
        uint32_t dst_slot = 0u, src_slot = 0u;
        const bool dst_is_virt = find_virtual(dst_h.ptr, dst_vs, dst_slot);
        const bool src_is_virt = find_virtual(src_h.ptr, src_vs, src_slot);

        if (dst_is_virt && src_is_virt) {
            if (src_slot < src_vs->virt_slot_valid.size() && src_vs->virt_slot_valid[src_slot]) {
                const SamplerDescKey& key = src_vs->virt_slot_keys[src_slot];
                D3D12_SAMPLER_DESC sdesc{};
                std::memcpy(&sdesc, key.bytes, sizeof(sdesc));
                auto dit = dst_vs->dedup_map.find(key);
                uint16_t hslot = 0u;
                if (dit != dst_vs->dedup_map.end()) {
                    hslot = dit->second;
                    unbounded_sampler_dedup_hits_.fetch_add(1ull, std::memory_order_relaxed);
                } else {
                    hslot = (dst_vs->next_host_slot < dst_vs->host_capacity)
                                ? static_cast<uint16_t>(dst_vs->next_host_slot++)
                                : static_cast<uint16_t>(dst_slot % dst_vs->host_capacity);
                    D3D12_CPU_DESCRIPTOR_HANDLE hdst{
                        dst_vs->host_cpu_start.ptr + static_cast<SIZE_T>(hslot) * dst_vs->inc_size};
                    host_dev_->CreateSampler(&sdesc, hdst);
                    dst_vs->dedup_map[key] = hslot;
                    unbounded_sampler_unique_host_slots_.fetch_add(1ull, std::memory_order_relaxed);
                }
                dst_vs->virt_to_host_slot[dst_slot] = hslot;
                dst_vs->virt_slot_keys[dst_slot] = key;
                dst_vs->virt_slot_valid[dst_slot] = 1u;
                unbounded_sampler_writes_.fetch_add(1ull, std::memory_order_relaxed);
            }
        } else if (!dst_is_virt && src_is_virt) {
            if (src_slot < src_vs->virt_slot_valid.size() && src_vs->virt_slot_valid[src_slot]) {
                D3D12_SAMPLER_DESC sdesc{};
                std::memcpy(&sdesc, src_vs->virt_slot_keys[src_slot].bytes, sizeof(sdesc));
                host_dev_->CreateSampler(&sdesc, dst_h);
            }
        } else if (dst_is_virt && !src_is_virt) {
            const uint16_t hslot = (dst_vs->next_host_slot < dst_vs->host_capacity)
                                       ? static_cast<uint16_t>(dst_vs->next_host_slot++)
                                       : static_cast<uint16_t>(dst_slot % dst_vs->host_capacity);
            D3D12_CPU_DESCRIPTOR_HANDLE hdst{
                dst_vs->host_cpu_start.ptr + static_cast<SIZE_T>(hslot) * dst_vs->inc_size};
            host_dev_->CopyDescriptorsSimple(1u, hdst, src_h, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            dst_vs->virt_to_host_slot[dst_slot] = hslot;
            unbounded_sampler_writes_.fetch_add(1ull, std::memory_order_relaxed);
        }
        ++dst_i;
        ++src_i;
    }
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE DeviceX::TranslateGpuDescriptorHandle(
    D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
    if (handle.ptr == 0ull) return handle;
    std::lock_guard<std::mutex> lock(virt_sampler_mutex_);
    for (const auto& [host_heap, vs] : virtual_sampler_heaps_) {
        (void)host_heap;
        if (vs.virt_gpu_start.ptr == 0ull) continue;
        const uint64_t base = vs.virt_gpu_start.ptr;
        const uint64_t end = base + static_cast<uint64_t>(vs.virtual_desc.NumDescriptors) * vs.inc_size;
        if (handle.ptr >= base && handle.ptr < end) {
            const uint32_t virt_slot = static_cast<uint32_t>((handle.ptr - base) / vs.inc_size);
            const uint16_t host_slot = (virt_slot < vs.virt_to_host_slot.size())
                                           ? vs.virt_to_host_slot[virt_slot]
                                           : 0u;
            unbounded_sampler_table_translations_.fetch_add(1ull, std::memory_order_relaxed);
            D3D12_GPU_DESCRIPTOR_HANDLE out{};
            out.ptr = vs.host_gpu_start.ptr + static_cast<uint64_t>(host_slot) * vs.inc_size;
            return out;
        }
    }
    return handle;
}

void DeviceX::NotifyDescriptorHeapBoundOnCommandList(ID3D12DescriptorHeap* host_heap) noexcept {
    if (!host_heap) return;
    std::lock_guard<std::mutex> lock(paged_desc_mutex_);
    auto it = paged_desc_heaps_.find(host_heap);
    if (it == paged_desc_heaps_.end()) return;
    for (auto& page : it->second.pages) {
        page.in_flight = true;
    }
}

void DeviceX::TrackPagedDescriptorWrite(D3D12_CPU_DESCRIPTOR_HANDLE dest_cpu) noexcept {
    if (dest_cpu.ptr == 0u) return;
    std::lock_guard<std::mutex> lock(paged_desc_mutex_);
    for (auto& [host_heap, ps] : paged_desc_heaps_) {
        (void)host_heap;
        const uint64_t base = static_cast<uint64_t>(ps.cpu_start.ptr);
        const uint64_t end = base + static_cast<uint64_t>(ps.num_descriptors) * ps.inc_size;
        const uint64_t ptr = static_cast<uint64_t>(dest_cpu.ptr);
        if (ptr < base || ptr >= end) continue;
        const uint32_t slot = static_cast<uint32_t>((ptr - base) / ps.inc_size);
        const uint32_t pidx = slot / std::max<UINT>(1u, ps.descriptors_per_page);
        if (pidx < ps.pages.size()) {
            auto& page = ps.pages[pidx];
            if (page.in_flight) {
                ++page.generation;
                page.in_flight = false;
                descriptor_page_generations_advanced_.fetch_add(1ull, std::memory_order_relaxed);
                descriptor_page_rotations_.fetch_add(1ull, std::memory_order_relaxed);
            }
        }
        return;
    }
}

HRESULT STDMETHODCALLTYPE DeviceX::GetGraphicsCompletenessStats(
    D3D12XBOX_GRAPHICS_COMPLETENESS_STATS* out_stats) {
    if (!out_stats) return E_POINTER;
    std::memset(out_stats, 0, sizeof(*out_stats));
    out_stats->paged_descriptor_heaps_created =
        paged_descriptor_heaps_created_.load(std::memory_order_relaxed);
    out_stats->descriptor_pages_allocated =
        descriptor_pages_allocated_.load(std::memory_order_relaxed);
    out_stats->descriptor_page_rotations =
        descriptor_page_rotations_.load(std::memory_order_relaxed);
    out_stats->descriptor_page_generations_advanced =
        descriptor_page_generations_advanced_.load(std::memory_order_relaxed);
    out_stats->unbounded_sampler_heaps_created =
        unbounded_sampler_heaps_created_.load(std::memory_order_relaxed);
    out_stats->unbounded_sampler_writes =
        unbounded_sampler_writes_.load(std::memory_order_relaxed);
    out_stats->unbounded_sampler_dedup_hits =
        unbounded_sampler_dedup_hits_.load(std::memory_order_relaxed);
    out_stats->unbounded_sampler_unique_host_slots =
        unbounded_sampler_unique_host_slots_.load(std::memory_order_relaxed);
    out_stats->unbounded_sampler_table_translations =
        unbounded_sampler_table_translations_.load(std::memory_order_relaxed);
    out_stats->dedicated_copy_queues_created =
        dedicated_copy_queues_created_.load(std::memory_order_relaxed);
    out_stats->copy_queue_barriers_sanitized =
        copy_queue_barriers_sanitized_.load(std::memory_order_relaxed);
    out_stats->copy_queue_uav_barriers_suppressed =
        copy_queue_uav_barriers_suppressed_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_calls =
        execute_indirect_calls_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_with_count_buffer =
        execute_indirect_with_count_buffer_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_with_root_constants =
        execute_indirect_with_root_constants_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_dispatch_mesh =
        execute_indirect_dispatch_mesh_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_bounds_clamped =
        execute_indirect_bounds_clamped_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_rejected =
        execute_indirect_rejected_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_va_translated =
        execute_indirect_va_translated_.load(std::memory_order_relaxed);
    out_stats->execute_indirect_vulkan_routed =
        execute_indirect_vulkan_routed_.load(std::memory_order_relaxed);
    out_stats->gpu_fmask_resolves =
        gpu_fmask_resolves_.load(std::memory_order_relaxed);
    out_stats->gpu_dcc_decompresses =
        gpu_dcc_decompresses_.load(std::memory_order_relaxed);
    out_stats->gpu_htile_decompresses =
        gpu_htile_decompresses_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(residency_mutex_);
        out_stats->skipped_active_live_heaps = skipped_active_live_heaps_;
        uint64_t prot = 0ull;
        for (const auto& [ptr, e] : residency_entries_) {
            (void)ptr;
            if (e.is_resident && IsResidencyEntryLiveProtectedLocked(e)) ++prot;
        }
        out_stats->active_protected_objects = prot;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::AllocateDescriptorHeapPage(
    ID3D12DescriptorHeap* heap,
    uint32_t* out_page_index,
    uint64_t* out_generation,
    uint64_t* out_page_gpu_base) {
    if (!heap || !out_page_index || !out_generation || !out_page_gpu_base) return E_POINTER;
    ID3D12DescriptorHeap* host_heap = UnwrapDescriptorHeap(heap);
    if (!host_heap) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(paged_desc_mutex_);
    auto it = paged_desc_heaps_.find(host_heap);
    if (it == paged_desc_heaps_.end()) return E_INVALIDARG;

    auto& ps = it->second;
    // Find an idle page or allocate a fresh isolated shadow page so active in-flight pages
    // are never overwritten mid-frame (xcl #84 NVIDIA flicker fix).
    for (auto& page : ps.pages) {
        if (!page.in_flight) {
            page.in_flight = true;
            ++page.generation;
            descriptor_page_generations_advanced_.fetch_add(1ull, std::memory_order_relaxed);
            *out_page_index = page.page_index;
            *out_generation = page.generation;
            *out_page_gpu_base =
                ps.gpu_start.ptr +
                static_cast<uint64_t>(page.page_index) * ps.descriptors_per_page * ps.inc_size;
            return S_OK;
        }
    }

    // All existing pages are in-flight: allocate a genuine isolated shadow host descriptor page.
    D3D12_DESCRIPTOR_HEAP_DESC shadow_desc{};
    shadow_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shadow_desc.NumDescriptors = ps.descriptors_per_page;
    shadow_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* shadow_heap = nullptr;
    const HRESULT hr = host_dev_->CreateDescriptorHeap(
        &shadow_desc, OS_D3D12_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(&shadow_heap));
    if (FAILED(hr) || !shadow_heap) return hr;

    DescriptorPageSlot new_slot{};
    new_slot.page_index = static_cast<uint32_t>(ps.pages.size());
    new_slot.generation = 1ull;
    new_slot.in_flight = true;
    new_slot.isolated_shadow_heap = shadow_heap;
    const uint64_t gpu_base = shadow_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    ps.pages.push_back(new_slot);
    descriptor_pages_allocated_.fetch_add(1ull, std::memory_order_relaxed);
    descriptor_page_rotations_.fetch_add(1ull, std::memory_order_relaxed);
    descriptor_page_generations_advanced_.fetch_add(1ull, std::memory_order_relaxed);

    *out_page_index = new_slot.page_index;
    *out_generation = new_slot.generation;
    *out_page_gpu_base = gpu_base;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::RetireDescriptorHeapPage(
    ID3D12DescriptorHeap* heap,
    uint32_t page_index) {
    if (!heap) return E_POINTER;
    ID3D12DescriptorHeap* host_heap = UnwrapDescriptorHeap(heap);
    if (!host_heap) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(paged_desc_mutex_);
    auto it = paged_desc_heaps_.find(host_heap);
    if (it == paged_desc_heaps_.end() || page_index >= it->second.pages.size()) {
        return E_INVALIDARG;
    }
    it->second.pages[page_index].in_flight = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ExecuteGpuResolveFmask(
    uint32_t width,
    uint32_t height,
    uint32_t sample_count,
    uint32_t resolve_mode,
    uint32_t clear_rgba8,
    const uint8_t* msaa_color_bytes,
    uint32_t msaa_color_size,
    const uint8_t* fmask_bytes,
    uint32_t fmask_size,
    uint8_t* out_rgba8,
    uint32_t out_size,
    uint32_t* out_used_gpu) {
    if (!msaa_color_bytes || !fmask_bytes || !out_rgba8 || width == 0u || height == 0u) {
        return E_INVALIDARG;
    }
    if (out_used_gpu) *out_used_gpu = 0u;
    openscarlett::XgGpuResolveParams params{};
    params.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::FmaskMsaaResolve);
    params.width = width;
    params.height = height;
    params.sample_count = sample_count;
    params.mode = resolve_mode;
    params.bpe = 4u;
    params.src_row_pitch = width * sample_count * 4u;
    params.dst_row_pitch = width * 4u;
    params.clear_reg_0 = clear_rgba8;

    std::vector<uint8_t> src_vec(msaa_color_bytes, msaa_color_bytes + msaa_color_size);
    std::vector<uint8_t> meta_vec(fmask_bytes, fmask_bytes + fmask_size);
    std::vector<uint8_t> dst_vec(out_size, 0u);

    openscarlett::Status st = openscarlett::Status::Error(openscarlett::StatusCode::DeviceError, "Untiler unavailable");
    if (EnsureXgGpuUntilerReady()) {
        std::lock_guard<std::mutex> ulk(xg_untiler_mutex_);
        if (xg_untiler_ && xg_untiler_->IsD3D12Ready()) {
            st = xg_untiler_->ExecuteResolveD3D12(params, src_vec, meta_vec, dst_vec, nullptr);
            if (st.ok() && out_used_gpu) *out_used_gpu = 1u;
        }
    }
    if (!st.ok()) {
        st = openscarlett::ResolveFmaskCpuReference(params, src_vec, meta_vec, dst_vec);
        if (!st.ok()) return E_FAIL;
    }
    std::memcpy(out_rgba8, dst_vec.data(), std::min<size_t>(out_size, dst_vec.size()));
    gpu_fmask_resolves_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ExecuteGpuDecompressDcc(
    uint32_t width,
    uint32_t height,
    uint32_t pipe_aligned,
    uint32_t num_pipes,
    uint32_t clear_rgba8,
    const uint8_t* src_blocks,
    uint32_t src_size,
    const uint8_t* dcc_keys,
    uint32_t dcc_size,
    uint8_t* out_rgba8,
    uint32_t out_size,
    uint32_t* out_used_gpu) {
    if (!src_blocks || !dcc_keys || !out_rgba8 || width == 0u || height == 0u) {
        return E_INVALIDARG;
    }
    if (out_used_gpu) *out_used_gpu = 0u;
    openscarlett::XgGpuResolveParams params{};
    params.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::DccDecompress);
    params.width = width;
    params.height = height;
    params.sample_count = 1u;
    params.mode = (pipe_aligned != 0u) ? 1u : 0u;
    params.bpe = 4u;
    params.src_row_pitch = width * 4u;
    params.dst_row_pitch = width * 4u;
    params.meta_blocks_x = (width + 7u) / 8u;
    params.meta_num_pipes = (num_pipes > 0u) ? num_pipes : 16u;
    params.meta_slice_pitch = dcc_size;
    params.clear_reg_0 = clear_rgba8;

    std::vector<uint8_t> src_vec(src_blocks, src_blocks + src_size);
    std::vector<uint8_t> meta_vec(dcc_keys, dcc_keys + dcc_size);
    std::vector<uint8_t> dst_vec(out_size, 0u);

    openscarlett::Status st = openscarlett::Status::Error(openscarlett::StatusCode::DeviceError, "Untiler unavailable");
    if (EnsureXgGpuUntilerReady()) {
        std::lock_guard<std::mutex> ulk(xg_untiler_mutex_);
        if (xg_untiler_ && xg_untiler_->IsD3D12Ready()) {
            st = xg_untiler_->ExecuteResolveD3D12(params, src_vec, meta_vec, dst_vec, nullptr);
            if (st.ok() && out_used_gpu) *out_used_gpu = 1u;
        }
    }
    if (!st.ok()) {
        st = openscarlett::DecompressDccCpuReference(params, src_vec, meta_vec, dst_vec);
        if (!st.ok()) return E_FAIL;
    }
    std::memcpy(out_rgba8, dst_vec.data(), std::min<size_t>(out_size, dst_vec.size()));
    gpu_dcc_decompresses_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::ExecuteGpuDecompressHtile(
    uint32_t width,
    uint32_t height,
    uint32_t mode_flags,
    uint32_t clear_reg0,
    uint32_t clear_reg1,
    const uint8_t* src_tiles,
    uint32_t src_size,
    const uint8_t* htile_words,
    uint32_t htile_size,
    uint8_t* out_depth_and_hiz,
    uint32_t out_size,
    uint32_t* out_used_gpu) {
    if (!src_tiles || !htile_words || !out_depth_and_hiz || width == 0u || height == 0u) {
        return E_INVALIDARG;
    }
    if (out_used_gpu) *out_used_gpu = 0u;
    openscarlett::XgGpuResolveParams params{};
    params.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::HtileDecompress);
    params.width = width;
    params.height = height;
    params.sample_count = 1u;
    params.mode = mode_flags;
    params.bpe = 4u;
    params.src_row_pitch = width * 4u;
    params.dst_row_pitch = width * 4u;
    params.meta_blocks_x = (width + 7u) / 8u;
    params.clear_reg_0 = clear_reg0;
    params.clear_reg_1 = clear_reg1;
    params.spv_aux_word_offset = width * height;

    std::vector<uint8_t> src_vec(src_tiles, src_tiles + src_size);
    std::vector<uint8_t> meta_vec(htile_words, htile_words + htile_size);
    std::vector<uint8_t> dst_vec(out_size, 0u);

    openscarlett::Status st = openscarlett::Status::Error(openscarlett::StatusCode::DeviceError, "Untiler unavailable");
    if (EnsureXgGpuUntilerReady()) {
        std::lock_guard<std::mutex> ulk(xg_untiler_mutex_);
        if (xg_untiler_ && xg_untiler_->IsD3D12Ready()) {
            st = xg_untiler_->ExecuteResolveD3D12(params, src_vec, meta_vec, dst_vec, nullptr);
            if (st.ok() && out_used_gpu) *out_used_gpu = 1u;
        }
    }
    if (!st.ok()) {
        st = openscarlett::DecompressHtileCpuReference(params, src_vec, meta_vec, dst_vec, nullptr);
        if (!st.ok()) return E_FAIL;
    }
    std::memcpy(out_depth_and_hiz, dst_vec.data(), std::min<size_t>(out_size, dst_vec.size()));
    gpu_htile_decompresses_.fetch_add(1ull, std::memory_order_relaxed);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::MarkResidencyObjectActive(
    ID3D12Pageable* obj,
    int32_t delta_ref) {
    if (!obj) return E_INVALIDARG;
    ID3D12Pageable* host_obj = UnwrapProxyPageable(obj);
    if (!host_obj) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(residency_mutex_);
    auto it = residency_entries_.find(host_obj);
    if (it == residency_entries_.end()) return E_INVALIDARG;
    auto& e = it->second;
    if (delta_ref > 0) {
        e.active_pin_count += static_cast<uint32_t>(delta_ref);
    } else if (delta_ref < 0) {
        const uint32_t sub = static_cast<uint32_t>(-delta_ref);
        e.active_pin_count = (e.active_pin_count >= sub) ? (e.active_pin_count - sub) : 0u;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceX::MarkResidencyObjectInFlight(
    ID3D12Pageable* obj,
    ID3D12Fence* fence,
    uint64_t fence_value) {
    if (!obj) return E_INVALIDARG;
    ID3D12Pageable* host_obj = UnwrapProxyPageable(obj);
    ID3D12Fence* host_fence = UnwrapFence(fence);
    if (!host_obj) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(residency_mutex_);
    auto it = residency_entries_.find(host_obj);
    if (it == residency_entries_.end()) return E_INVALIDARG;
    auto& e = it->second;
    if (e.in_flight_fence != host_fence) {
        if (e.in_flight_fence) e.in_flight_fence->Release();
        e.in_flight_fence = host_fence;
        if (e.in_flight_fence) e.in_flight_fence->AddRef();
    }
    e.in_flight_fence_value = fence_value;
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
