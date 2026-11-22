// OpenScarlett MIT Wave 2 Standalone Xbox-ABI Guest Sample:
// MeshletCull — Amplification Shader (as_6_5) + Mesh Shader (ms_6_5) + Pixel Shader (ps_6_5)
// GPU 6-plane frustum & normal-cone culling pipeline via ID3D12GraphicsCommandListX::DispatchMesh,
// with graceful Compute (cs_6_2) + Graphics (vs_6_2/ps_6_2) fallback for WARP / non-MeshTier GPUs.
//
// Deliberately does NOT link openscarlett_core! Imports ONLY d3d12_xs.dll, xgameruntime.dll,
// and pixevt.dll (plus standard Windows system DLLs) and runs from a zero-DLL package folder
// via `openscarlett run <package_dir>` with MicrosoftGame.Config (TitleId = 0x5CA01002).

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"

namespace {

constexpr uint32_t kExpectedTitleId = 0x5CA01002u;
constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kMeshletCount = 16u;

constexpr uint8_t kBgR = 0x12u;
constexpr uint8_t kBgG = 0x16u;
constexpr uint8_t kBgB = 0x22u;
constexpr uint8_t kBgA = 0xFFu;

template <typename T>
class GuestComPtr {
public:
    GuestComPtr() = default;
    ~GuestComPtr() { Reset(); }
    GuestComPtr(const GuestComPtr&) = delete;
    GuestComPtr& operator=(const GuestComPtr&) = delete;

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // D3D12 pipeline state stream subobjects require pointer alignment
#endif
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeVal, typename InnerType>
struct alignas(void*) PsoStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = TypeVal;
    InnerType inner{};
};

struct MeshletCullPsoStream {
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_sig;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv_format;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct MeshletDesc {
    float center[3];
    float radius;
    float cone_axis[3];
    float cone_cutoff;
    float color[3];
    float half_extent;
};
static_assert(sizeof(MeshletDesc) == 48, "MeshletDesc must be 48 bytes");

struct FrameConstants {
    float view_proj[4][4];       // 4 explicit rows (world -> clip)
    float frustum_planes[6][4];  // 6 inward-pointing unit planes (nx, ny, nz, d)
    float camera_pos[4];         // xyz = camera position
    float light_dir[4];          // xyz = normalized direction toward light
    uint32_t meshlet_count;
    uint32_t pad[15];
};
static_assert(sizeof(FrameConstants) == 256, "FrameConstants must be 256 bytes");

enum class CullOutcome : uint32_t {
    Visible = 0,
    FrustumCulled = 1,
    ConeCulled = 2,
};

struct CpuGroundTruth {
    uint32_t surviving = 0;
    uint32_t frustum_culled = 0;
    uint32_t cone_culled = 0;
    std::array<CullOutcome, kMeshletCount> outcomes{};
};

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_meshlet_cull] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

bool ReadWholeFileW(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD total = 0;
    while (total < static_cast<DWORD>(out.size())) {
        DWORD chunk = 0;
        if (!::ReadFile(h, out.data() + total,
                        static_cast<DWORD>(out.size()) - total, &chunk, nullptr) ||
            chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool WriteWholeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written_total = 0;
    while (written_total < size) {
        DWORD chunk = 0;
        const DWORD to_write = static_cast<DWORD>(size - written_total);
        if (!::WriteFile(h, bytes + written_total, to_write, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        written_total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool WritePpmW(const wchar_t* path, uint32_t width, uint32_t height, const uint8_t* rgba8) {
    char header[64]{};
    const int hdr_len = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", width, height);
    if (hdr_len <= 0) return false;

    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> ppm;
    ppm.reserve(static_cast<size_t>(hdr_len) + pixel_count * 3u);
    ppm.insert(ppm.end(), header, header + hdr_len);
    for (size_t i = 0; i < pixel_count; ++i) {
        ppm.push_back(rgba8[i * 4u + 0u]);
        ppm.push_back(rgba8[i * 4u + 1u]);
        ppm.push_back(rgba8[i * 4u + 2u]);
    }
    return WriteWholeFileW(path, ppm.data(), ppm.size());
}

double QpcMs(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) {
    if (freq.QuadPart == 0) return 0.0;
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

void NormalizePlane(float plane[4]) {
    const float len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (len > 1e-8f) {
        plane[0] /= len;
        plane[1] /= len;
        plane[2] /= len;
        plane[3] /= len;
    }
}

void BuildScene(FrameConstants& fc, std::array<MeshletDesc, kMeshletCount>& meshlets) {
    std::memset(&fc, 0, sizeof(fc));

    // Camera at (0, 0, -4.0) looking straight along +Z; 90 deg vertical FOV, aspect 1:1, near=1, far=20.
    constexpr float kCamZ = -4.0f;
    constexpr float kNear = 1.0f;
    constexpr float kFar = 20.0f;
    constexpr float kProjA = kFar / (kFar - kNear);             // 20/19
    constexpr float kProjB = -kNear * kFar / (kFar - kNear);    // -20/19

    // Row 0: clip.x = x
    fc.view_proj[0][0] = 1.0f;
    // Row 1: clip.y = y
    fc.view_proj[1][1] = 1.0f;
    // Row 2: clip.z = kProjA * (z - kCamZ) + kProjB = kProjA * z + (kProjA * (-kCamZ) + kProjB)
    fc.view_proj[2][2] = kProjA;
    fc.view_proj[2][3] = kProjA * (-kCamZ) + kProjB;
    // Row 3: clip.w = z - kCamZ
    fc.view_proj[3][2] = 1.0f;
    fc.view_proj[3][3] = -kCamZ;

    // Gribb-Hartmann extraction of the 6 D3D12 clip-space frustum planes (inward-pointing):
    //   0: Left   (row3 + row0)
    //   1: Right  (row3 - row0)
    //   2: Bottom (row3 + row1)
    //   3: Top    (row3 - row1)
    //   4: Near   (row2)
    //   5: Far    (row3 - row2)
    for (int j = 0; j < 4; ++j) {
        fc.frustum_planes[0][j] = fc.view_proj[3][j] + fc.view_proj[0][j];
        fc.frustum_planes[1][j] = fc.view_proj[3][j] - fc.view_proj[0][j];
        fc.frustum_planes[2][j] = fc.view_proj[3][j] + fc.view_proj[1][j];
        fc.frustum_planes[3][j] = fc.view_proj[3][j] - fc.view_proj[1][j];
        fc.frustum_planes[4][j] = fc.view_proj[2][j];
        fc.frustum_planes[5][j] = fc.view_proj[3][j] - fc.view_proj[2][j];
    }
    for (int i = 0; i < 6; ++i) {
        NormalizePlane(fc.frustum_planes[i]);
    }

    fc.camera_pos[0] = 0.0f;
    fc.camera_pos[1] = 0.0f;
    fc.camera_pos[2] = kCamZ;
    fc.camera_pos[3] = 1.0f;

    // Directional light from upper-left-front toward the surface (-0.4, +0.6, -1.0) normalized
    const float l_len = std::sqrt(0.4f * 0.4f + 0.6f * 0.6f + 1.0f * 1.0f);
    fc.light_dir[0] = -0.4f / l_len;
    fc.light_dir[1] = +0.6f / l_len;
    fc.light_dir[2] = -1.0f / l_len;
    fc.light_dir[3] = 0.0f;

    fc.meshlet_count = kMeshletCount;

    // 16 meshlets arranged in a 4x4 grid (row r in 0..3 top->bottom, col c in 0..3 left->right):
    //   - 6 Frustum-Culled meshlets (one outside EACH of the 6 frustum planes: L, T, R, B, Near, Far!)
    //   - 4 Normal-Cone Culled meshlets (inside frustum, back-facing cone_axis = (0, 0, +1))
    //   - 6 Surviving meshlets (inside frustum, front-facing cone_axis = (0, 0, -1), vivid distinct RGB)
    constexpr float kRadius = 0.75f;
    constexpr float kHalfExtent = 0.52f;
    constexpr float kConeCutoff = 0.40f;

    for (uint32_t r = 0; r < 4u; ++r) {
        for (uint32_t c = 0; c < 4u; ++c) {
            const uint32_t idx = r * 4u + c;
            MeshletDesc& m = meshlets[idx];
            m.center[0] = -2.7f + static_cast<float>(c) * 1.8f;
            m.center[1] = +2.7f - static_cast<float>(r) * 1.8f;
            m.center[2] = 0.0f;
            m.radius = kRadius;
            m.cone_axis[0] = 0.0f;
            m.cone_axis[1] = 0.0f;
            m.cone_axis[2] = -1.0f;  // front-facing default
            m.cone_cutoff = kConeCutoff;
            m.color[0] = 0.8f;
            m.color[1] = 0.8f;
            m.color[2] = 0.8f;
            m.half_extent = kHalfExtent;
        }
    }

    // 6 Frustum-culled meshlets (exercise ALL 6 frustum planes individually!)
    meshlets[0].center[0] = -6.5f;   // Outside LEFT plane
    meshlets[1].center[1] = +6.5f;   // Outside TOP plane
    meshlets[3].center[0] = +6.5f;   // Outside RIGHT plane
    meshlets[12].center[1] = -6.5f;  // Outside BOTTOM plane
    meshlets[14].center[2] = -5.0f;  // Behind NEAR plane (z < -3.0 - radius)
    meshlets[15].center[2] = +20.0f; // Beyond FAR plane (z > +16.0 + radius)

    // 4 Normal-cone culled (in-frustum, back-facing) meshlets -> distinct warning color if ever leaked!
    constexpr uint32_t kConeCulledIds[4] = {5u, 6u, 9u, 11u};
    for (uint32_t id : kConeCulledIds) {
        meshlets[id].cone_axis[0] = 0.0f;
        meshlets[id].cone_axis[1] = 0.0f;
        meshlets[id].cone_axis[2] = +1.0f;  // facing away from camera
        meshlets[id].color[0] = 1.0f;
        meshlets[id].color[1] = 1.0f;
        meshlets[id].color[2] = 1.0f;
    }

    // 6 Surviving front-facing meshlets with vivid, unambiguous distinct RGB signatures:
    //   idx  2 (r=0, c=2): Crimson Red    (1.00, 0.15, 0.15)
    //   idx  4 (r=1, c=0): Emerald Green  (0.15, 1.00, 0.20)
    //   idx  7 (r=1, c=3): Cobalt Blue    (0.15, 0.30, 1.00)
    //   idx  8 (r=2, c=0): Amber Gold     (1.00, 0.85, 0.10)
    //   idx 10 (r=2, c=2): Cyan Aqua      (0.10, 0.95, 0.95)
    //   idx 13 (r=3, c=1): Magenta Violet (0.95, 0.15, 0.95)
    meshlets[2].color[0]  = 1.00f; meshlets[2].color[1]  = 0.15f; meshlets[2].color[2]  = 0.15f;
    meshlets[4].color[0]  = 0.15f; meshlets[4].color[1]  = 1.00f; meshlets[4].color[2]  = 0.20f;
    meshlets[7].color[0]  = 0.15f; meshlets[7].color[1]  = 0.30f; meshlets[7].color[2]  = 1.00f;
    meshlets[8].color[0]  = 1.00f; meshlets[8].color[1]  = 0.85f; meshlets[8].color[2]  = 0.10f;
    meshlets[10].color[0] = 0.10f; meshlets[10].color[1] = 0.95f; meshlets[10].color[2] = 0.95f;
    meshlets[13].color[0] = 0.95f; meshlets[13].color[1] = 0.15f; meshlets[13].color[2] = 0.95f;
}

CpuGroundTruth EvaluateCpuGroundTruth(
    const FrameConstants& fc,
    const std::array<MeshletDesc, kMeshletCount>& meshlets) {
    CpuGroundTruth gt{};
    for (uint32_t idx = 0; idx < kMeshletCount; ++idx) {
        const MeshletDesc& m = meshlets[idx];

        bool in_frustum = true;
        for (int p = 0; p < 6; ++p) {
            const float dist =
                fc.frustum_planes[p][0] * m.center[0] +
                fc.frustum_planes[p][1] * m.center[1] +
                fc.frustum_planes[p][2] * m.center[2] +
                fc.frustum_planes[p][3];
            if (dist < -m.radius) {
                in_frustum = false;
                break;
            }
        }
        if (!in_frustum) {
            gt.outcomes[idx] = CullOutcome::FrustumCulled;
            ++gt.frustum_culled;
            continue;
        }

        const float vx = m.center[0] - fc.camera_pos[0];
        const float vy = m.center[1] - fc.camera_pos[1];
        const float vz = m.center[2] - fc.camera_pos[2];
        const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        const float dot_cone =
            (m.cone_axis[0] * vx + m.cone_axis[1] * vy + m.cone_axis[2] * vz) / vlen;
        if (dot_cone >= m.cone_cutoff) {
            gt.outcomes[idx] = CullOutcome::ConeCulled;
            ++gt.cone_culled;
            continue;
        }

        gt.outcomes[idx] = CullOutcome::Visible;
        ++gt.surviving;
    }
    return gt;
}

void ProjectCenterToPixel(
    const FrameConstants& fc,
    const float center[3],
    uint32_t& out_px,
    uint32_t& out_py) {
    float clip[4]{};
    for (int r = 0; r < 4; ++r) {
        clip[r] = fc.view_proj[r][0] * center[0] +
                  fc.view_proj[r][1] * center[1] +
                  fc.view_proj[r][2] * center[2] +
                  fc.view_proj[r][3];
    }
    const float ndc_x = clip[0] / clip[3];
    const float ndc_y = clip[1] / clip[3];
    const float sx = (ndc_x * 0.5f + 0.5f) * static_cast<float>(kWidth);
    const float sy = (0.5f - ndc_y * 0.5f) * static_cast<float>(kHeight);
    out_px = static_cast<uint32_t>(std::clamp(std::lround(sx), 0l, static_cast<long>(kWidth - 1u)));
    out_py = static_cast<uint32_t>(std::clamp(std::lround(sy), 0l, static_cast<long>(kHeight - 1u)));
}

D3D12_RESOURCE_DESC MakeBufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER MakeTransition(
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    bool force_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            force_warp = true;
        }
    }

    // 1. GDK Game Runtime initialization, Scarlett device check, and MicrosoftGame.Config TitleId check
    PIXBeginEventOnCpu(0xFF33AAFFull, "MeshletCull_Main");

    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }
    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        PIXEndEventOnCpu();
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    uint32_t title_id = 0;
    if (XGameGetXboxTitleId(&title_id) != XGR_S_OK || title_id != kExpectedTitleId) {
        PIXEndEventOnCpu();
        return Fail("XGameGetXboxTitleId mismatch (expected 0x5CA01002)", static_cast<int64_t>(title_id));
    }

    // 2. Build deterministic 16-meshlet scene & compute analytical CPU ground truth
    FrameConstants frame_cb{};
    std::array<MeshletDesc, kMeshletCount> meshlets{};
    BuildScene(frame_cb, meshlets);
    const CpuGroundTruth gt = EvaluateCpuGroundTruth(frame_cb, meshlets);
    if (gt.surviving != 6u || gt.frustum_culled != 6u || gt.cone_culled != 4u) {
        PIXEndEventOnCpu();
        return Fail("Unexpected analytical CPU ground truth counts", gt.surviving);
    }

    // 3. Create Xbox D3D12X device and probe MeshShaderTier + ShaderModel
    OpenScarlettD3D12SetPreferWarp(force_warp ? 1 : 0);
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        PIXEndEventOnCpu();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if (force_warp && gpu_info.IsWarp == 0u) {
        PIXEndEventOnCpu();
        return Fail("Expected WARP adapter when --warp specified");
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
        opt7.MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    }

    const bool use_hw_mesh =
        (!force_warp) &&
        (gpu_info.IsWarp == 0u) &&
        (sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5) &&
        (static_cast<uint32_t>(opt7.MeshShaderTier) >= 10u);

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("SetFrameIntervalX", hr);
    }
    D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
    hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
    if (FAILED(hr) || frame_token == 0) {
        PIXEndEventOnCpu();
        return Fail("WaitFrameEventX", hr);
    }

    // 4. Create Command Queue, Allocator, CommandListX, and Synchronization Fence
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandQueue", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandList", hr);
    }

    GuestComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr) || !fence) {
        PIXEndEventOnCpu();
        return Fail("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        PIXEndEventOnCpu();
        return Fail("CreateEventW", ::GetLastError());
    }

    // 5. Create Render Target (256x256 RGBA8) + Depth Buffer (256x256 D32_FLOAT) + Descriptor Heaps
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kWidth;
    rt_desc.Height = kHeight;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    D3D12_CLEAR_VALUE rt_clear{};
    rt_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_clear.Color[0] = static_cast<float>(kBgR) / 255.0f;
    rt_clear.Color[1] = static_cast<float>(kBgG) / 255.0f;
    rt_clear.Color[2] = static_cast<float>(kBgB) / 255.0f;
    rt_clear.Color[3] = 1.0f;

    GuestComPtr<ID3D12Resource> rt_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, &rt_clear, IID_PPV_ARGS(rt_tex.Put()));
    if (FAILED(hr) || !rt_tex) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(RT)", hr);
    }

    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = kWidth;
    depth_desc.Height = kHeight;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    depth_clear.DepthStencil.Stencil = 0;

    GuestComPtr<ID3D12Resource> depth_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(depth_tex.Put()));
    if (FAILED(hr) || !depth_tex) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(Depth D32_FLOAT)", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_handle);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    GuestComPtr<ID3D12DescriptorHeap> dsv_heap;
    hr = device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(dsv_heap.Put()));
    if (FAILED(hr) || !dsv_heap) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(DSV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth_tex.Get(), nullptr, dsv_handle);

    // 6. Create Constant Buffer, Meshlet StructuredBuffer, GPU UAV CullStats Buffer + Readback Buffer
    constexpr uint64_t kMeshletBytes = sizeof(MeshletDesc) * kMeshletCount;
    constexpr uint64_t kStatsBytes = 4u * sizeof(uint32_t);
    constexpr uint64_t kVisibleBytes = 32u * sizeof(uint32_t);

    const D3D12_RESOURCE_DESC cb_desc = MakeBufferDesc(sizeof(FrameConstants));
    GuestComPtr<ID3D12Resource> cb_buf;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cb_buf.Put()));
    if (FAILED(hr) || !cb_buf) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(CB)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        hr = cb_buf->Map(0, &none, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CB Map", hr);
        }
        std::memcpy(mapped, &frame_cb, sizeof(frame_cb));
        cb_buf->Unmap(0, nullptr);
    }

    const D3D12_RESOURCE_DESC meshlet_desc_res = MakeBufferDesc(kMeshletBytes);
    GuestComPtr<ID3D12Resource> meshlet_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &meshlet_desc_res,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(meshlet_upload.Put()));
    if (FAILED(hr) || !meshlet_upload) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(MeshletUpload)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        hr = meshlet_upload->Map(0, &none, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("MeshletUpload Map", hr);
        }
        std::memcpy(mapped, meshlets.data(), static_cast<size_t>(kMeshletBytes));
        meshlet_upload->Unmap(0, nullptr);
    }

    GuestComPtr<ID3D12Resource> meshlet_buf;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &meshlet_desc_res,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(meshlet_buf.Put()));
    if (FAILED(hr) || !meshlet_buf) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(MeshletBuf)", hr);
    }

    const D3D12_RESOURCE_DESC stats_upload_desc = MakeBufferDesc(kStatsBytes);
    GuestComPtr<ID3D12Resource> stats_zero_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &stats_upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(stats_zero_upload.Put()));
    if (FAILED(hr) || !stats_zero_upload) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(StatsZeroUpload)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        hr = stats_zero_upload->Map(0, &none, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("StatsZeroUpload Map", hr);
        }
        std::memset(mapped, 0, static_cast<size_t>(kStatsBytes));
        stats_zero_upload->Unmap(0, nullptr);
    }

    const D3D12_RESOURCE_DESC stats_uav_desc = MakeBufferDesc(kStatsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    GuestComPtr<ID3D12Resource> stats_buf;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &stats_uav_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(stats_buf.Put()));
    if (FAILED(hr) || !stats_buf) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(StatsUavBuf)", hr);
    }

    GuestComPtr<ID3D12Resource> stats_readback;
    hr = device->CreateCommittedResource(
        &readback_heap, D3D12_HEAP_FLAG_NONE, &stats_upload_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(stats_readback.Put()));
    if (FAILED(hr) || !stats_readback) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(StatsReadback)", hr);
    }

    // Pipeline objects (kept alive until GPU completes)
    GuestComPtr<ID3D12RootSignature> root_sig_primary;
    GuestComPtr<ID3D12RootSignature> root_sig_secondary;
    GuestComPtr<ID3D12PipelineState> pso_primary;
    GuestComPtr<ID3D12PipelineState> pso_secondary;
    GuestComPtr<ID3D12Resource> visible_buf;

    // Common Blend / Rasterizer / DepthStencil descriptors shared by HW AS+MS+PS and WARP CS+VS+PS
    D3D12_BLEND_DESC blend_desc{};
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].LogicOpEnable = FALSE;
    blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // CULL_MODE_NONE intentionally ensures that if AS/CS ever failed to normal-cone cull a
    // back-facing meshlet, it would visibly leak into the RT and be caught by pixel checks!
    D3D12_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode = D3D12_FILL_MODE_SOLID;
    rast_desc.CullMode = D3D12_CULL_MODE_NONE;
    rast_desc.FrontCounterClockwise = FALSE;
    rast_desc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC ds_desc{};
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds_desc.StencilEnable = FALSE;
    ds_desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds_desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    ds_desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds_desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    ds_desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    ds_desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds_desc.BackFace = ds_desc.FrontFace;

    // 7. Create Shaders & Pipeline State(s) for active mode
    if (use_hw_mesh) {
        std::vector<uint8_t> as_blob;
        std::vector<uint8_t> ms_blob;
        std::vector<uint8_t> ps_blob;
        if (!ReadWholeFileW(L"G:\\MeshletCullAS.cso", as_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullAS.cso", ::GetLastError());
        }
        if (!ReadWholeFileW(L"G:\\MeshletCullMS.cso", ms_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullMS.cso", ::GetLastError());
        }
        if (!ReadWholeFileW(L"G:\\MeshletCullPS.cso", ps_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullPS.cso", ::GetLastError());
        }

        hr = device->CreateRootSignature(
            0, as_blob.data(), as_blob.size(), IID_PPV_ARGS(root_sig_primary.Put()));
        if (FAILED(hr) || !root_sig_primary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateRootSignature(MeshletCullAS.cso)", hr);
        }

        MeshletCullPsoStream stream{};
        stream.root_sig.inner = root_sig_primary.Get();
        stream.as.inner = {as_blob.data(), as_blob.size()};
        stream.ms.inner = {ms_blob.data(), ms_blob.size()};
        stream.ps.inner = {ps_blob.data(), ps_blob.size()};
        stream.blend.inner = blend_desc;
        stream.rasterizer.inner = rast_desc;
        stream.depth_stencil.inner = ds_desc;
        stream.sample_mask.inner = UINT_MAX;
        stream.sample_desc.inner = {1, 0};
        stream.rt_formats.inner.NumRenderTargets = 1;
        stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.dsv_format.inner = DXGI_FORMAT_D32_FLOAT;

        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{};
        stream_desc.SizeInBytes = sizeof(stream);
        stream_desc.pPipelineStateSubobjectStream = &stream;

        hr = device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(pso_primary.Put()));
        if (FAILED(hr) || !pso_primary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("ID3D12Device2::CreatePipelineState(AS+MS+PS+D32)", hr);
        }
    } else {
        std::vector<uint8_t> cs_blob;
        std::vector<uint8_t> vs_blob;
        std::vector<uint8_t> ps_blob;
        if (!ReadWholeFileW(L"G:\\MeshletCullFallbackCS.cso", cs_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullFallbackCS.cso", ::GetLastError());
        }
        if (!ReadWholeFileW(L"G:\\MeshletCullFallbackVS.cso", vs_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullFallbackVS.cso", ::GetLastError());
        }
        if (!ReadWholeFileW(L"G:\\MeshletCullFallbackPS.cso", ps_blob)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\MeshletCullFallbackPS.cso", ::GetLastError());
        }

        hr = device->CreateRootSignature(
            0, cs_blob.data(), cs_blob.size(), IID_PPV_ARGS(root_sig_primary.Put()));
        if (FAILED(hr) || !root_sig_primary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateRootSignature(MeshletCullFallbackCS.cso)", hr);
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC cs_pso_desc{};
        cs_pso_desc.pRootSignature = root_sig_primary.Get();
        cs_pso_desc.CS = {cs_blob.data(), cs_blob.size()};
        hr = device->CreateComputePipelineState(&cs_pso_desc, IID_PPV_ARGS(pso_primary.Put()));
        if (FAILED(hr) || !pso_primary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateComputePipelineState(FallbackCS)", hr);
        }

        hr = device->CreateRootSignature(
            0, vs_blob.data(), vs_blob.size(), IID_PPV_ARGS(root_sig_secondary.Put()));
        if (FAILED(hr) || !root_sig_secondary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateRootSignature(MeshletCullFallbackVS.cso)", hr);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx_desc{};
        gfx_desc.pRootSignature = root_sig_secondary.Get();
        gfx_desc.VS = {vs_blob.data(), vs_blob.size()};
        gfx_desc.PS = {ps_blob.data(), ps_blob.size()};
        gfx_desc.BlendState = blend_desc;
        gfx_desc.SampleMask = UINT_MAX;
        gfx_desc.RasterizerState = rast_desc;
        gfx_desc.DepthStencilState = ds_desc;
        gfx_desc.InputLayout = {nullptr, 0};
        gfx_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gfx_desc.NumRenderTargets = 1;
        gfx_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gfx_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        gfx_desc.SampleDesc = {1, 0};

        hr = device->CreateGraphicsPipelineState(&gfx_desc, IID_PPV_ARGS(pso_secondary.Put()));
        if (FAILED(hr) || !pso_secondary) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateGraphicsPipelineState(FallbackVS+PS)", hr);
        }

        const D3D12_RESOURCE_DESC vis_desc =
            MakeBufferDesc(kVisibleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &vis_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(visible_buf.Put()));
        if (FAILED(hr) || !visible_buf) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("CreateCommittedResource(VisibleIndices)", hr);
        }
    }

    // 8. Record Command List, Execute, PresentX, and Synchronize GPU
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    PIXBeginEventOnCommandList(
        cmd.Get(),
        0xFF44DD88ull,
        use_hw_mesh ? "MeshletCull_HW_AS_MS_PS" : "MeshletCull_WARP_CS_VS_PS");

    // Upload meshlets & zero UAV culling statistics buffer
    cmd->CopyBufferRegion(meshlet_buf.Get(), 0, meshlet_upload.Get(), 0, kMeshletBytes);
    cmd->CopyBufferRegion(stats_buf.Get(), 0, stats_zero_upload.Get(), 0, kStatsBytes);

    D3D12_RESOURCE_BARRIER init_barriers[3] = {
        MakeTransition(meshlet_buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        MakeTransition(stats_buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        MakeTransition(rt_tex.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, init_barriers);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};

    if (use_hw_mesh) {
        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
        cmd->ClearRenderTargetView(rtv_handle, rt_clear.Color, 0, nullptr);
        cmd->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scissor);

        cmd->SetGraphicsRootSignature(root_sig_primary.Get());
        cmd->SetPipelineState(pso_primary.Get());
        cmd->SetGraphicsRootConstantBufferView(0, cb_buf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(1, meshlet_buf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootUnorderedAccessView(2, stats_buf->GetGPUVirtualAddress());

        PIXSetMarkerOnCommandList(cmd.Get(), 0xFFFFCC33ull, "DispatchMesh_16Meshlets");
        // Live Hardware Amplification + Mesh Shader dispatch (vtable slot 79!)
        cmd->DispatchMesh(1, 1, 1);

        D3D12_RESOURCE_BARRIER end_barriers[2] = {
            MakeTransition(rt_tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PRESENT),
            MakeTransition(stats_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmd->ResourceBarrier(2, end_barriers);
    } else {
        // Compute culling pass (cs_6_2)
        cmd->SetComputeRootSignature(root_sig_primary.Get());
        cmd->SetPipelineState(pso_primary.Get());
        cmd->SetComputeRootConstantBufferView(0, cb_buf->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(1, meshlet_buf->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, stats_buf->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3, visible_buf->GetGPUVirtualAddress());

        PIXSetMarkerOnCommandList(cmd.Get(), 0xFFFFCC33ull, "FallbackCS_Cull_16Meshlets");
        cmd->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER mid_barriers[2] = {
            MakeTransition(stats_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            MakeTransition(visible_buf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, mid_barriers);

        // Standard VS+PS draw pass (vs_6_2 / ps_6_2) rendering compacted visible meshlets
        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
        cmd->ClearRenderTargetView(rtv_handle, rt_clear.Color, 0, nullptr);
        cmd->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scissor);

        cmd->SetGraphicsRootSignature(root_sig_secondary.Get());
        cmd->SetPipelineState(pso_secondary.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->SetGraphicsRootConstantBufferView(0, cb_buf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(1, meshlet_buf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(2, visible_buf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(3, stats_buf->GetGPUVirtualAddress());

        cmd->DrawInstanced(18, kMeshletCount, 0, 0);

        D3D12_RESOURCE_BARRIER end_barriers[2] = {
            MakeTransition(rt_tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PRESENT),
            MakeTransition(stats_buf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cmd->ResourceBarrier(2, end_barriers);
    }

    cmd->CopyBufferRegion(stats_readback.Get(), 0, stats_buf.Get(), 0, kStatsBytes);
    PIXEndEventOnCommandList(cmd.Get());

    hr = cmd->Close();
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CommandListX::Close", hr);
    }

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    PIXBeginEventOnCommandQueue(queue.Get(), 0xFFFF6644ull, "MeshletCull_PresentX");
    ID3D12Resource* plane_res[] = {rt_tex.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = frame_token;
    plane.ResourceCount = 1;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres_params{};
    pres_params.SyncInterval = 1;

    hr = queue->PresentX(1, &plane, &pres_params);
    PIXEndEventOnCommandQueue(queue.Get());
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("PresentX(1)", hr);
    }

    hr = queue->Signal(fence.Get(), 1u);
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CommandQueueX::Signal", hr);
    }
    if (fence->GetCompletedValue() < 1u) {
        hr = fence->SetEventOnCompletion(1u, fence_event);
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Fence::SetEventOnCompletion", hr);
        }
        const DWORD wait_res = ::WaitForSingleObject(fence_event, 15000u);
        if (wait_res != WAIT_OBJECT_0) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("GPU fence wait timed out", wait_res);
        }
        PIXNotifyWakeFromFenceSignal(fence_event);
    }
    ::CloseHandle(fence_event);

    ::QueryPerformanceCounter(&qpc_end);
    const double frame_ms = QpcMs(qpc_start, qpc_end, qpc_freq);

    // 9. Read back GPU UAV culling statistics buffer and assert exact match against CPU ground truth
    uint32_t gpu_stats[4]{};
    {
        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(kStatsBytes)};
        hr = stats_readback->Map(0, &read_range, &mapped);
        if (FAILED(hr) || !mapped) {
            PIXEndEventOnCpu();
            return Fail("StatsReadback Map", hr);
        }
        std::memcpy(gpu_stats, mapped, sizeof(gpu_stats));
        D3D12_RANGE written_none{0, 0};
        stats_readback->Unmap(0, &written_none);
    }

    const uint32_t gpu_surviving = gpu_stats[0];
    const uint32_t gpu_frustum_culled = gpu_stats[1];
    const uint32_t gpu_cone_culled = gpu_stats[2];
    const uint32_t gpu_tested = gpu_stats[3];

    PIXReportCounter(L"MeshletCull.Surviving", static_cast<float>(gpu_surviving));
    PIXReportCounter(L"MeshletCull.FrustumCulled", static_cast<float>(gpu_frustum_culled));
    PIXReportCounter(L"MeshletCull.ConeCulled", static_cast<float>(gpu_cone_culled));

    if (gpu_tested != kMeshletCount) {
        PIXEndEventOnCpu();
        return Fail("GPU UAV tested meshlet count mismatch", gpu_tested);
    }
    if (gpu_surviving != gt.surviving) {
        PIXEndEventOnCpu();
        return Fail("GPU UAV surviving meshlet count mismatch", gpu_surviving);
    }
    if (gpu_frustum_culled != gt.frustum_culled) {
        PIXEndEventOnCpu();
        return Fail("GPU UAV frustum-culled count mismatch", gpu_frustum_culled);
    }
    if (gpu_cone_culled != gt.cone_culled) {
        PIXEndEventOnCpu();
        return Fail("GPU UAV normal-cone-culled count mismatch", gpu_cone_culled);
    }

    // 10. Read back 256x256 rendered frame and verify exact visible & culled meshlet patches
    uint64_t frame_hash = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    hr = OpenScarlettD3D12GetLastPresentHash(queue.Get(), &frame_hash, &out_w, &out_h);
    if (FAILED(hr) || out_w != kWidth || out_h != kHeight || frame_hash == 0ull) {
        PIXEndEventOnCpu();
        return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied);
    if (FAILED(hr) || copied != pixels.size()) {
        PIXEndEventOnCpu();
        return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
    }

    auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return &pixels[(static_cast<size_t>(y) * kWidth + x) * 4u];
    };

    const uint8_t* bg = pixel_at(0, 0);
    if (bg[0] != kBgR || bg[1] != kBgG || bg[2] != kBgB || bg[3] != kBgA) {
        PIXEndEventOnCpu();
        return Fail("Background clear pixel mismatch at (0,0)");
    }

    // Verify every in-frustum meshlet cell:
    //  - Cone-culled cells MUST remain 100% pure background across a 9x9 probe box!
    //  - Surviving cells MUST display their exact bright dominant RGB color (and NOT the dark occluded back-plate!)
    for (uint32_t idx = 0; idx < kMeshletCount; ++idx) {
        if (gt.outcomes[idx] == CullOutcome::FrustumCulled) {
            continue;
        }
        uint32_t cx = 0;
        uint32_t cy = 0;
        ProjectCenterToPixel(frame_cb, meshlets[idx].center, cx, cy);

        if (gt.outcomes[idx] == CullOutcome::ConeCulled) {
            for (int dy = -4; dy <= 4; ++dy) {
                for (int dx = -4; dx <= 4; ++dx) {
                    const uint8_t* p = pixel_at(
                        static_cast<uint32_t>(static_cast<int>(cx) + dx),
                        static_cast<uint32_t>(static_cast<int>(cy) + dy));
                    if (p[0] != kBgR || p[1] != kBgG || p[2] != kBgB) {
                        PIXEndEventOnCpu();
                        return Fail("Normal-cone-culled meshlet leaked pixels into RT", idx);
                    }
                }
            }
        } else {
            // Surviving visible meshlet: at the apex (cx, cy), normal = (0,0,-1), shade ~= 0.877
            const uint8_t* p = pixel_at(cx, cy);
            const uint8_t exp_r = static_cast<uint8_t>(std::clamp(meshlets[idx].color[0] * 0.877f * 255.0f, 0.0f, 255.0f));
            const uint8_t exp_g = static_cast<uint8_t>(std::clamp(meshlets[idx].color[1] * 0.877f * 255.0f, 0.0f, 255.0f));
            const uint8_t exp_b = static_cast<uint8_t>(std::clamp(meshlets[idx].color[2] * 0.877f * 255.0f, 0.0f, 255.0f));
            const int dr = std::abs(static_cast<int>(p[0]) - static_cast<int>(exp_r));
            const int dg = std::abs(static_cast<int>(p[1]) - static_cast<int>(exp_g));
            const int db = std::abs(static_cast<int>(p[2]) - static_cast<int>(exp_b));
            if (dr > 28 || dg > 28 || db > 28 || p[3] != 255u) {
                std::fprintf(stderr,
                             "[gdk_meshlet_cull] Surviving meshlet %u at (%u,%u) got RGB(%u,%u,%u) expected ~RGB(%u,%u,%u)\n",
                             idx, cx, cy, p[0], p[1], p[2], exp_r, exp_g, exp_b);
                PIXEndEventOnCpu();
                return Fail("Surviving meshlet color or depth-occlusion check failed", idx);
            }
        }
    }

    uint32_t non_bg_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(1024);
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
        if (r != kBgR || g != kBgG || b != kBgB) {
            ++non_bg_pixels;
        }
        unique_rgb.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b));
    }

    // 6 surviving ~33x33 pixel patches -> ~6500 non-background pixels with smooth 3D directional lighting
    if (non_bg_pixels < 5500u || non_bg_pixels > 8000u) {
        PIXEndEventOnCpu();
        return Fail("Unexpected surviving meshlet pixel coverage", non_bg_pixels);
    }
    if (unique_rgb.size() < 40u) {
        PIXEndEventOnCpu();
        return Fail("Too few shaded colors across 6 lit 3D meshlets",
                    static_cast<int64_t>(unique_rgb.size()));
    }

    // 11. Export T:\meshlet_cull.ppm and T:\metrics.txt
    if (!WritePpmW(L"T:\\meshlet_cull.ppm", kWidth, kHeight, pixels.data())) {
        PIXEndEventOnCpu();
        return Fail("Write T:\\meshlet_cull.ppm", ::GetLastError());
    }

    const char* mode_str = use_hw_mesh ? "hw_as_ms" : "warp_fallback";
    char metrics_buf[1024]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=MeshletCull\n"
        "title_id=0x%08X\n"
        "mode=%s\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "mesh_tier=%u\n"
        "shader_model=0x%02X\n"
        "width=%u\n"
        "height=%u\n"
        "total_meshlets=%u\n"
        "surviving=%u\n"
        "frustum_culled=%u\n"
        "cone_culled=%u\n"
        "non_bg_pixels=%u\n"
        "unique_colors=%zu\n"
        "fnv1a=0x%016llX\n"
        "frame_ms=%.3f\n",
        title_id,
        mode_str,
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        static_cast<unsigned>(opt7.MeshShaderTier),
        static_cast<unsigned>(sm.HighestShaderModel),
        out_w,
        out_h,
        gpu_tested,
        gpu_surviving,
        gpu_frustum_culled,
        gpu_cone_culled,
        non_bg_pixels,
        unique_rgb.size(),
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        PIXEndEventOnCpu();
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_meshlet_cull] PASS | Mode=%s | Adapter=%s (MeshTier=%u, SM=0x%02X) | "
        "Cull=%u/%u surviving (%u frustum, %u cone) | NonBG=%u | Colors=%zu | FNV1a=0x%016llX | %.2f ms\n",
        mode_str,
        gpu_info.AdapterName,
        static_cast<unsigned>(opt7.MeshShaderTier),
        static_cast<unsigned>(sm.HighestShaderModel),
        gpu_surviving,
        gpu_tested,
        gpu_frustum_culled,
        gpu_cone_culled,
        non_bg_pixels,
        unique_rgb.size(),
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    std::fflush(stdout);

    PIXEndEventOnCpu();

    visible_buf.Reset();
    pso_secondary.Reset();
    pso_primary.Reset();
    root_sig_secondary.Reset();
    root_sig_primary.Reset();
    stats_readback.Reset();
    stats_buf.Reset();
    stats_zero_upload.Reset();
    meshlet_buf.Reset();
    meshlet_upload.Reset();
    cb_buf.Reset();
    dsv_heap.Reset();
    rtv_heap.Reset();
    depth_tex.Reset();
    rt_tex.Reset();
    fence.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    XGameRuntimeUninitialize();
    return 0;
}
