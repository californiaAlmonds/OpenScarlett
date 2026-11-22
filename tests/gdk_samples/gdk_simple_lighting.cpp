// OpenScarlett MIT Standalone Xbox-ABI Guest Sample (Wave 4):
// SimpleLighting — DirectXTK12-style multi-frame 3D Blinn-Phong lit scene
// (faceted occluding 3D cube + smooth 3D UV-sphere + checkered 3D ground plane),
// directional + orbiting point light Blinn-Phong diffuse & specular shading,
// D32_FLOAT depth buffer occlusion, 3-frame constant buffer animation, PresentX
// readback to P6 PPM files, XDisplay (HDR probe + timeout deferral),
// X3DAudio (orbiting 3D emitter synced with orbiting point light: stereo panning,
// distance attenuation, Doppler shift), XAudio2FX (Reverb + VolumeMeter APOs),
// PIX CPU/GPU markers, and OPENSCARLETT_GPU_BACKEND environment reporting.
//
// Deliberately does NOT link openscarlett_core. Imports ONLY Xbox/GDK shim DLLs
// (d3d12_xs, xgameruntime, XAudio2_9, pixevt). Runs inside an isolated zero-DLL
// package directory launched via `openscarlett run <package_dir>`.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kFrameCount = 3u;
constexpr uint32_t kExpectedTitleId = 0x5CA01003u;

constexpr uint8_t kBgR = 12u;
constexpr uint8_t kBgG = 16u;
constexpr uint8_t kBgB = 28u;
constexpr uint8_t kBgA = 255u;

constexpr float kPi = 3.14159265358979323846f;

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

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Mat4 {
    float m[4][4];
};

struct LightingVertex {
    float position[3];
    float normal[3];
    float color[4];  // rgb = albedo, a = specular strength
};
static_assert(sizeof(LightingVertex) == 40, "LightingVertex must be 40 bytes");

struct alignas(256) LightingConstants {
    float view_proj[16];          // offset 0
    float camera_pos[4];          // offset 64
    float dir_light_dir[4];       // offset 80  (xyz = unit dir TO light, w = dir intensity)
    float dir_light_color[4];     // offset 96  (rgb = dir color, a = ambient strength)
    float point_light_pos[4];     // offset 112 (xyz = world pos, w = point intensity)
    float point_light_color[4];   // offset 128 (rgb = point color, a = quad attenuation)
    float material_params[4];     // offset 144 (x = dir spec weight, y = point spec weight, z/w = unused)
    float pad[24];                // offset 160..255
};
static_assert(sizeof(LightingConstants) == 256, "LightingConstants must be 256-byte aligned");

struct SubmeshRange {
    uint32_t index_start = 0;
    uint32_t index_count = 0;
    int32_t base_vertex = 0;
};

struct FrameAudioMetrics {
    float distance = 0.0f;
    float gain_left = 0.0f;
    float gain_right = 0.0f;
    float doppler_factor = 1.0f;
    float lpf_direct = 1.0f;
    float reverb_level = 0.0f;
    float emitter_angle = 0.0f;
    float delay_left_ms = 0.0f;
    float delay_right_ms = 0.0f;
};

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_simple_lighting] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

Vec3 Sub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 Normalize(Vec3 v) noexcept {
    const float len = std::sqrt(Dot(v, v));
    if (len < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}

Vec3 RotateYawPitch(Vec3 v, float yaw, float pitch) noexcept {
    // Pitch around X, then yaw around Y
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float y1 = v.y * cp - v.z * sp;
    const float z1 = v.y * sp + v.z * cp;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    return {
        v.x * cy + z1 * sy,
        y1,
        -v.x * sy + z1 * cy,
    };
}

Mat4 MakeLookAtRH(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 zaxis = Normalize(Sub(eye, target));
    const Vec3 xaxis = Normalize(Cross(up, zaxis));
    const Vec3 yaxis = Cross(zaxis, xaxis);

    Mat4 r{};
    r.m[0][0] = xaxis.x;
    r.m[0][1] = yaxis.x;
    r.m[0][2] = zaxis.x;
    r.m[0][3] = 0.0f;

    r.m[1][0] = xaxis.y;
    r.m[1][1] = yaxis.y;
    r.m[1][2] = zaxis.y;
    r.m[1][3] = 0.0f;

    r.m[2][0] = xaxis.z;
    r.m[2][1] = yaxis.z;
    r.m[2][2] = zaxis.z;
    r.m[2][3] = 0.0f;

    r.m[3][0] = -Dot(xaxis, eye);
    r.m[3][1] = -Dot(yaxis, eye);
    r.m[3][2] = -Dot(zaxis, eye);
    r.m[3][3] = 1.0f;
    return r;
}

Mat4 MakePerspectiveFovRH(float fov_y_rad, float aspect, float near_z, float far_z) noexcept {
    const float y_scale = 1.0f / std::tan(fov_y_rad * 0.5f);
    const float x_scale = y_scale / aspect;
    Mat4 r{};
    r.m[0][0] = x_scale;
    r.m[1][1] = y_scale;
    r.m[2][2] = far_z / (near_z - far_z);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (near_z * far_z) / (near_z - far_z);
    return r;
}

Mat4 MultiplyMat4(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row][k] * b.m[k][col];
            }
            out.m[row][col] = sum;
        }
    }
    return out;
}

bool ProjectWorldToPixel(Vec3 p, const Mat4& vp, uint32_t width, uint32_t height,
                         int32_t& out_x, int32_t& out_y) noexcept {
    const float cx = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
    const float cy = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
    const float cw = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];
    if (cw <= 1e-5f) return false;
    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    out_x = static_cast<int32_t>(std::lround((ndc_x * 0.5f + 0.5f) * static_cast<float>(width - 1u)));
    out_y = static_cast<int32_t>(std::lround((0.5f - ndc_y * 0.5f) * static_cast<float>(height - 1u)));
    return out_x >= 0 && out_x < static_cast<int32_t>(width) &&
           out_y >= 0 && out_y < static_cast<int32_t>(height);
}

// Builds a faceted 3D cube with flat per-face normals, transformed into world space.
SubmeshRange AppendFacetedCube(std::vector<LightingVertex>& verts,
                               std::vector<uint32_t>& indices,
                               Vec3 center,
                               float half_extent,
                               float yaw,
                               float pitch,
                               const float rgba[4]) {
    SubmeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    struct FaceDef {
        Vec3 normal;
        Vec3 corners[4];
    };
    const float h = half_extent;
    const FaceDef faces[6] = {
        // +Z front (outward CW when viewed from +Z in RH space: TL, TR, BR, BL)
        {{0.0f, 0.0f, 1.0f}, {{-h, h, h}, {h, h, h}, {h, -h, h}, {-h, -h, h}}},
        // -Z back
        {{0.0f, 0.0f, -1.0f}, {{h, h, -h}, {-h, h, -h}, {-h, -h, -h}, {h, -h, -h}}},
        // +X right
        {{1.0f, 0.0f, 0.0f}, {{h, h, h}, {h, h, -h}, {h, -h, -h}, {h, -h, h}}},
        // -X left
        {{-1.0f, 0.0f, 0.0f}, {{-h, h, -h}, {-h, h, h}, {-h, -h, h}, {-h, -h, -h}}},
        // +Y top
        {{0.0f, 1.0f, 0.0f}, {{-h, h, -h}, {h, h, -h}, {h, h, h}, {-h, h, h}}},
        // -Y bottom
        {{0.0f, -1.0f, 0.0f}, {{-h, -h, h}, {h, -h, h}, {h, -h, -h}, {-h, -h, -h}}},
    };

    for (uint32_t f = 0; f < 6u; ++f) {
        const uint32_t base = static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
        const Vec3 n = Normalize(RotateYawPitch(faces[f].normal, yaw, pitch));
        for (uint32_t c = 0; c < 4u; ++c) {
            const Vec3 rp = RotateYawPitch(faces[f].corners[c], yaw, pitch);
            LightingVertex v{};
            v.position[0] = center.x + rp.x;
            v.position[1] = center.y + rp.y;
            v.position[2] = center.z + rp.z;
            v.normal[0] = n.x;
            v.normal[1] = n.y;
            v.normal[2] = n.z;
            v.color[0] = rgba[0];
            v.color[1] = rgba[1];
            v.color[2] = rgba[2];
            v.color[3] = rgba[3];
            verts.push_back(v);
        }
        // Outward CW triangles
        indices.push_back(base + 0u);
        indices.push_back(base + 1u);
        indices.push_back(base + 2u);
        indices.push_back(base + 0u);
        indices.push_back(base + 2u);
        indices.push_back(base + 3u);
    }

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

// Builds a smooth UV-sphere in world space with unit outward normals.
SubmeshRange AppendSmoothSphere(std::vector<LightingVertex>& verts,
                                std::vector<uint32_t>& indices,
                                Vec3 center,
                                float radius,
                                uint32_t lon_segs,
                                uint32_t lat_segs,
                                const float rgba[4]) {
    SubmeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    for (uint32_t lat = 0; lat <= lat_segs; ++lat) {
        const float v = static_cast<float>(lat) / static_cast<float>(lat_segs);
        const float phi = v * kPi;
        const float sin_phi = std::sin(phi);
        const float cos_phi = std::cos(phi);

        for (uint32_t lon = 0; lon <= lon_segs; ++lon) {
            const float u = static_cast<float>(lon) / static_cast<float>(lon_segs);
            const float theta = u * 2.0f * kPi;
            const float nx = sin_phi * std::cos(theta);
            const float ny = cos_phi;
            const float nz = sin_phi * std::sin(theta);

            LightingVertex vtx{};
            vtx.position[0] = center.x + nx * radius;
            vtx.position[1] = center.y + ny * radius;
            vtx.position[2] = center.z + nz * radius;
            vtx.normal[0] = nx;
            vtx.normal[1] = ny;
            vtx.normal[2] = nz;
            vtx.color[0] = rgba[0];
            vtx.color[1] = rgba[1];
            vtx.color[2] = rgba[2];
            vtx.color[3] = rgba[3];
            verts.push_back(vtx);
        }
    }

    const uint32_t stride = lon_segs + 1u;
    for (uint32_t lat = 0; lat < lat_segs; ++lat) {
        for (uint32_t lon = 0; lon < lon_segs; ++lon) {
            const uint32_t v00 = lat * stride + lon;
            const uint32_t v10 = (lat + 1u) * stride + lon;
            const uint32_t v01 = lat * stride + (lon + 1u);
            const uint32_t v11 = (lat + 1u) * stride + (lon + 1u);

            indices.push_back(v00);
            indices.push_back(v10);
            indices.push_back(v01);

            indices.push_back(v10);
            indices.push_back(v11);
            indices.push_back(v01);
        }
    }

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

// Builds a 4x4 checkered 3D lit ground plane with upward normal (0, 1, 0).
SubmeshRange AppendCheckeredGround(std::vector<LightingVertex>& verts,
                                   std::vector<uint32_t>& indices,
                                   float y_plane,
                                   float half_span,
                                   uint32_t grid_cells) {
    SubmeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    const float cell_w = (2.0f * half_span) / static_cast<float>(grid_cells);
    constexpr float kTileA[4] = {0.26f, 0.44f, 0.48f, 0.55f};
    constexpr float kTileB[4] = {0.16f, 0.25f, 0.34f, 0.35f};

    for (uint32_t gz = 0; gz < grid_cells; ++gz) {
        for (uint32_t gx = 0; gx < grid_cells; ++gx) {
            const uint32_t base = static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
            const float x0 = -half_span + static_cast<float>(gx) * cell_w;
            const float x1 = x0 + cell_w;
            const float z0 = -half_span + static_cast<float>(gz) * cell_w;
            const float z1 = z0 + cell_w;
            const float* col = (((gx ^ gz) & 1u) == 0u) ? kTileA : kTileB;

            const Vec3 pts[4] = {
                {x0, y_plane, z0},
                {x1, y_plane, z0},
                {x1, y_plane, z1},
                {x0, y_plane, z1},
            };
            for (uint32_t c = 0; c < 4u; ++c) {
                LightingVertex v{};
                v.position[0] = pts[c].x;
                v.position[1] = pts[c].y;
                v.position[2] = pts[c].z;
                v.normal[0] = 0.0f;
                v.normal[1] = 1.0f;
                v.normal[2] = 0.0f;
                v.color[0] = col[0];
                v.color[1] = col[1];
                v.color[2] = col[2];
                v.color[3] = col[3];
                verts.push_back(v);
            }
            // Outward CW when viewed from +Y
            indices.push_back(base + 0u);
            indices.push_back(base + 1u);
            indices.push_back(base + 2u);
            indices.push_back(base + 0u);
            indices.push_back(base + 2u);
            indices.push_back(base + 3u);
        }
    }

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

bool ReadWholeFileW(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
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
    if (h == INVALID_HANDLE_VALUE) return false;
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

double QpcMs(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) noexcept {
    if (freq.QuadPart == 0) return 0.0;
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                      D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_DESC MakeBufferDesc(uint64_t bytes) noexcept {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        }
    }

    std::string gpu_backend_env = "d3d12_default";
    {
        char env_buf[128]{};
        const DWORD len = ::GetEnvironmentVariableA(
            "OPENSCARLETT_GPU_BACKEND", env_buf, static_cast<DWORD>(sizeof(env_buf)));
        if (len > 0 && len < sizeof(env_buf)) {
            gpu_backend_env = env_buf;
        }
    }

    PIXBeginEventOnCpu(0xFF33BBFFull, "SimpleLighting::Startup");

    // 1. GDK Game Runtime + TitleId (from MicrosoftGame.Config) + Scarlett DeviceType
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }

    uint32_t title_id = 0;
    const XGameResult tid_hr = XGameGetXboxTitleId(&title_id);
    if (tid_hr != XGR_S_OK || title_id != kExpectedTitleId) {
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XGameGetXboxTitleId mismatch (expected 0x5CA01003)", title_id);
    }

    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    // 2. XDisplay: HDR mode probe + display timeout deferral acquisition
    XDisplayHdrModeInfo hdr_info{};
    const XDisplayHdrModeResult hdr_result =
        XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferHdr, &hdr_info);
    if ((hdr_result != XDisplayHdrModeResult_Enabled &&
         hdr_result != XDisplayHdrModeResult_Disabled) ||
        !(hdr_info.maxToneMapLuminance >= hdr_info.maxFullFrameToneMapLuminance &&
          hdr_info.maxFullFrameToneMapLuminance > hdr_info.minToneMapLuminance)) {
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XDisplayTryEnableHdrMode invalid result/luminance", static_cast<int64_t>(hdr_result));
    }

    XDisplayTimeoutDeferralHandle display_deferral = nullptr;
    const XGameResult def_hr = XDisplayAcquireTimeoutDeferral(&display_deferral);
    if (def_hr != XGR_S_OK || !display_deferral) {
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XDisplayAcquireTimeoutDeferral", def_hr);
    }

    // 3. XAudio2 + X3DAudio + XAudio2FX (Reverb + VolumeMeter COM APOs)
    IXAudio2* xaudio2 = nullptr;
    IXAudio2MasteringVoice* master_voice = nullptr;
    IXAudio2SourceVoice* light_voice = nullptr;
    OS_IXAudio2Unknown* reverb_unk = nullptr;
    OS_IXAPOParameters* reverb_params_ifc = nullptr;
    OS_IXAudio2Unknown* meter_unk = nullptr;
    OS_IXAPOParameters* meter_params_ifc = nullptr;
    X3DAUDIO_HANDLE x3d_handle{};

    auto cleanup_audio_and_display = [&]() {
        if (meter_params_ifc) { meter_params_ifc->Release(); meter_params_ifc = nullptr; }
        if (meter_unk) { meter_unk->Release(); meter_unk = nullptr; }
        if (reverb_params_ifc) { reverb_params_ifc->Release(); reverb_params_ifc = nullptr; }
        if (reverb_unk) { reverb_unk->Release(); reverb_unk = nullptr; }
        if (light_voice) {
            (void)light_voice->Stop(0u, OS_XAUDIO2_COMMIT_NOW);
            light_voice->DestroyVoice();
            light_voice = nullptr;
        }
        if (master_voice) {
            master_voice->DestroyVoice();
            master_voice = nullptr;
        }
        if (xaudio2) {
            xaudio2->StopEngine();
            xaudio2->Release();
            xaudio2 = nullptr;
        }
        if (display_deferral) {
            XDisplayCloseTimeoutDeferralHandle(display_deferral);
            display_deferral = nullptr;
        }
    };

    int32_t ahr = XAudio2Create(&xaudio2, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR);
    if (ahr != OS_XAUDIO2_S_OK || !xaudio2) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XAudio2Create", ahr);
    }

    ahr = xaudio2->CreateMasteringVoice(&master_voice, 2u, 48000u, 0u, nullptr, nullptr, 0u);
    if (ahr != OS_XAUDIO2_S_OK || !master_voice) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateMasteringVoice", ahr);
    }

    uint32_t channel_mask = 0u;
    ahr = master_voice->GetChannelMask(&channel_mask);
    if (ahr != OS_XAUDIO2_S_OK) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("GetChannelMask", ahr);
    }
    if (channel_mask == 0u) {
        channel_mask = OS_SPEAKER_STEREO;
    }

    ahr = X3DAudioInitialize(OS_SPEAKER_STEREO, OS_X3DAUDIO_SPEED_OF_SOUND, x3d_handle);
    if (ahr != OS_XAUDIO2_S_OK) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("X3DAudioInitialize", ahr);
    }

    // Create a mono 48kHz PCM16 source voice representing the orbiting 3D light emitter
    OS_WAVEFORMATEX src_wfx{};
    src_wfx.wFormatTag = OS_WAVE_FORMAT_PCM;
    src_wfx.nChannels = 1u;
    src_wfx.nSamplesPerSec = 48000u;
    src_wfx.wBitsPerSample = 16u;
    src_wfx.nBlockAlign = 2u;
    src_wfx.nAvgBytesPerSec = 96000u;
    src_wfx.cbSize = 0u;

    ahr = xaudio2->CreateSourceVoice(&light_voice, &src_wfx, 0u, 2.0f, nullptr, nullptr, nullptr);
    if (ahr != OS_XAUDIO2_S_OK || !light_voice) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateSourceVoice", ahr);
    }

    std::vector<int16_t> hum_pcm(480u, 0);
    for (size_t i = 0; i < hum_pcm.size(); ++i) {
        const float phase = 2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f;
        hum_pcm[i] = static_cast<int16_t>(std::lround(std::sin(phase) * 8000.0f));
    }
    OS_XAUDIO2_BUFFER audio_buf{};
    audio_buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    audio_buf.AudioBytes = static_cast<uint32_t>(hum_pcm.size() * sizeof(int16_t));
    audio_buf.pAudioData = reinterpret_cast<const uint8_t*>(hum_pcm.data());
    (void)light_voice->SubmitSourceBuffer(&audio_buf, nullptr);
    (void)light_voice->Start(0u, OS_XAUDIO2_COMMIT_NOW);

    // XAudio2FX Reverb APO: create, QI IXAPOParameters, SetParameters + GetParameters verify
    ahr = XAudio2CreateReverb(&reverb_unk, 0u);
    if (ahr != OS_XAUDIO2_S_OK || !reverb_unk) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XAudio2CreateReverb", ahr);
    }
    ahr = reverb_unk->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&reverb_params_ifc));
    if (ahr != OS_XAUDIO2_S_OK || !reverb_params_ifc) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("Reverb::QueryInterface(IXAPOParameters)", ahr);
    }
    OS_XAUDIO2FX_REVERB_PARAMETERS rev_set{};
    rev_set.WetDryMix = 42.5f;
    rev_set.ReflectionsDelay = 12u;
    rev_set.ReverbDelay = 8u;
    rev_set.RearDelay = 5u;
    rev_set.SideDelay = 5u;
    rev_set.PositionLeft = 6u;
    rev_set.PositionRight = 6u;
    rev_set.PositionMatrixLeft = 27u;
    rev_set.PositionMatrixRight = 27u;
    rev_set.EarlyDiffusion = 8u;
    rev_set.LateDiffusion = 8u;
    rev_set.LowEQGain = 8u;
    rev_set.LowEQCutoff = 4u;
    rev_set.HighEQGain = 8u;
    rev_set.HighEQCutoff = 4u;
    rev_set.RoomFilterFreq = 4800.0f;
    rev_set.RoomFilterMain = -2.0f;
    rev_set.RoomFilterHF = -4.0f;
    rev_set.ReflectionsGain = -3.0f;
    rev_set.ReverbGain = 1.5f;
    rev_set.DecayTime = 1.35f;
    rev_set.Density = 92.0f;
    rev_set.RoomSize = 75.0f;
    rev_set.DisableLateField = 0;
    reverb_params_ifc->SetParameters(&rev_set, sizeof(rev_set));

    OS_XAUDIO2FX_REVERB_PARAMETERS rev_got{};
    reverb_params_ifc->GetParameters(&rev_got, sizeof(rev_got));
    if (std::fabs(rev_got.WetDryMix - 42.5f) > 1e-4f ||
        std::fabs(rev_got.DecayTime - 1.35f) > 1e-4f ||
        std::fabs(rev_got.RoomSize - 75.0f) > 1e-4f) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XAudio2FX Reverb GetParameters round-trip mismatch");
    }

    // XAudio2FX VolumeMeter APO: create, QI IXAPOParameters
    ahr = XAudio2CreateVolumeMeter(&meter_unk, 0u);
    if (ahr != OS_XAUDIO2_S_OK || !meter_unk) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XAudio2CreateVolumeMeter", ahr);
    }
    ahr = meter_unk->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&meter_params_ifc));
    if (ahr != OS_XAUDIO2_S_OK || !meter_params_ifc) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("VolumeMeter::QueryInterface(IXAPOParameters)", ahr);
    }

    // 4. Create Xbox Scarlett D3D12X device
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.DisableGeometryShaderAllocations = 1;
    dev_params.DisableTessellationShaderAllocations = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if ((gpu_info.IsWarp != 0u) != use_warp) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("Adapter WARP mode mismatch", gpu_info.IsWarp);
    }

    // 5. Load precompiled SM 6.5 (or SM 6.2 fallback for WARP) Blinn-Phong DXIL shaders from G:\.
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    const bool supports_sm65 =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
        sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

    std::vector<uint8_t> vs_blob;
    std::vector<uint8_t> ps_blob;
    const wchar_t* vs_path = supports_sm65 ? L"G:\\LightingVS.cso" : L"G:\\LightingVS_sm62.cso";
    const wchar_t* ps_path = supports_sm65 ? L"G:\\LightingPS.cso" : L"G:\\LightingPS_sm62.cso";
    if (!ReadWholeFileW(vs_path, vs_blob)) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("Read LightingVS CSO from G:\\", ::GetLastError());
    }
    if (!ReadWholeFileW(ps_path, ps_blob)) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("Read LightingPS CSO from G:\\", ::GetLastError());
    }

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("SetFrameIntervalX", hr);
    }

    // 6. CommandQueueX, CommandAllocator, CommandListX, Fence
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommandQueue", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommandList", hr);
    }

    GuestComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr) || !fence) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateEventW(fence)", ::GetLastError());
    }
    uint64_t fence_val = 0;

    // 7. Create Color RT (RGBA8_UNORM + ALLOW_DISPLAY | ALLOW_DCC) and D32_FLOAT Depth Buffer
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_CLEAR_VALUE color_clear{};
    color_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_clear.Color[0] = static_cast<float>(kBgR) / 255.0f;
    color_clear.Color[1] = static_cast<float>(kBgG) / 255.0f;
    color_clear.Color[2] = static_cast<float>(kBgB) / 255.0f;
    color_clear.Color[3] = 1.0f;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kWidth;
    rt_desc.Height = kHeight;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.SampleDesc.Quality = 0;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> color_rt;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_ALLOW_DISPLAY,
        &rt_desc,
        D3D12_RESOURCE_STATE_PRESENT,
        &color_clear,
        IID_PPV_ARGS(color_rt.Put()));
    if (FAILED(hr) || !color_rt) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommittedResource(Color RT)", hr);
    }

    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    depth_clear.DepthStencil.Stencil = 0;

    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = kWidth;
    depth_desc.Height = kHeight;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    GuestComPtr<ID3D12Resource> depth_rt;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear,
        IID_PPV_ARGS(depth_rt.Put()));
    if (FAILED(hr) || !depth_rt) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommittedResource(D32_FLOAT Depth)", hr);
    }

    // RTV & DSV Heaps
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color_rt.Get(), nullptr, rtv_handle);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    GuestComPtr<ID3D12DescriptorHeap> dsv_heap;
    hr = device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(dsv_heap.Put()));
    if (FAILED(hr) || !dsv_heap) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateDescriptorHeap(DSV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth_rt.Get(), &dsv_desc, dsv_handle);

    // 8. Root Signature & Graphics PSO (D32_FLOAT depth test LESS, RGBA8 RT)
    GuestComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, vs_blob.data(), vs_blob.size(), IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateRootSignature(LightingVS.cso)", hr);
    }

    static const D3D12_INPUT_ELEMENT_DESC kInputElements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.InputLayout = {kInputElements, 3};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.VS = {vs_blob.data(), vs_blob.size()};
    pso_desc.PS = {ps_blob.data(), ps_blob.size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    GuestComPtr<ID3D12PipelineState> pso;
    hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()));
    if (FAILED(hr) || !pso) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateGraphicsPipelineState(Blinn-Phong + D32_FLOAT)", hr);
    }

    // 9. Build 3D Blinn-Phong Scene Geometry:
    //    - Submesh 0: Front occluding faceted 3D cube (cool azure-blue) drawn FIRST!
    //    - Submesh 1: Main smooth 3D UV-sphere (warm amber-orange) drawn SECOND -> occluded behind cube!
    //    - Submesh 2: 4x4 checkered 3D lit ground plane drawn THIRD!
    const Vec3 kFrontCubePos{-0.18f, 0.08f, 1.08f};
    const Vec3 kMainSpherePos{0.12f, 0.10f, 0.0f};
    constexpr float kCubeColor[4] = {0.16f, 0.62f, 0.98f, 0.90f};
    constexpr float kSphereColor[4] = {0.98f, 0.48f, 0.14f, 1.00f};

    std::vector<LightingVertex> vertices;
    std::vector<uint32_t> indices;
    const SubmeshRange front_cube = AppendFacetedCube(
        vertices, indices, kFrontCubePos, 0.30f, 0.45f, 0.28f, kCubeColor);
    const SubmeshRange main_sphere = AppendSmoothSphere(
        vertices, indices, kMainSpherePos, 0.85f, 24u, 16u, kSphereColor);
    const SubmeshRange ground_plane = AppendCheckeredGround(
        vertices, indices, -0.78f, 2.60f, 4u);

    const uint64_t vb_bytes = vertices.size() * sizeof(LightingVertex);
    const uint64_t ib_bytes = indices.size() * sizeof(uint32_t);
    const D3D12_RESOURCE_DESC vb_desc = MakeBufferDesc(vb_bytes);
    const D3D12_RESOURCE_DESC ib_desc = MakeBufferDesc(ib_bytes);

    GuestComPtr<ID3D12Resource> vb;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vb.Put()));
    if (FAILED(hr) || !vb) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommittedResource(VB)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = vb->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            PIXEndEventOnCpu();
            XGameRuntimeUninitialize();
            return Fail("VB Map", hr);
        }
        std::memcpy(mapped, vertices.data(), static_cast<size_t>(vb_bytes));
        vb->Unmap(0, nullptr);
    }

    GuestComPtr<ID3D12Resource> ib;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ib.Put()));
    if (FAILED(hr) || !ib) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommittedResource(IB)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = ib->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            PIXEndEventOnCpu();
            XGameRuntimeUninitialize();
            return Fail("IB Map", hr);
        }
        std::memcpy(mapped, indices.data(), static_cast<size_t>(ib_bytes));
        ib->Unmap(0, nullptr);
    }

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(LightingVertex);
    vbv.SizeInBytes = static_cast<UINT>(vb_bytes);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = ib->GetGPUVirtualAddress();
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = static_cast<UINT>(ib_bytes);

    // 10. Per-frame dynamic constant buffer (3 frames x 256 bytes)
    const uint64_t cb_total_bytes = static_cast<uint64_t>(kFrameCount) * sizeof(LightingConstants);
    const D3D12_RESOURCE_DESC cb_desc = MakeBufferDesc(cb_total_bytes);
    GuestComPtr<ID3D12Resource> cb_resource;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cb_resource.Put()));
    if (FAILED(hr) || !cb_resource) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("CreateCommittedResource(CB)", hr);
    }

    uint8_t* cb_mapped = nullptr;
    {
        void* ptr = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = cb_resource->Map(0, &no_read, &ptr);
        if (FAILED(hr) || !ptr) {
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            PIXEndEventOnCpu();
            XGameRuntimeUninitialize();
            return Fail("CB Map", hr);
        }
        cb_mapped = static_cast<uint8_t*>(ptr);
    }

    PIXEndEventOnCpu();

    // 11. Render 3 animated frames with orbiting directional + point lights
    //     and synchronized X3DAudio 3D sound emitter + XAudio2FX VolumeMeter.
    LARGE_INTEGER qpc_freq{};
    ::QueryPerformanceFrequency(&qpc_freq);

    const Vec3 kCamPos{0.0f, 1.05f, 3.60f};
    const Vec3 kLookTarget{0.0f, -0.08f, 0.0f};
    const Vec3 kUp{0.0f, 1.0f, 0.0f};
    const Mat4 view = MakeLookAtRH(kCamPos, kLookTarget, kUp);
    const Mat4 proj = MakePerspectiveFovRH(45.0f * kPi / 180.0f, 1.0f, 0.2f, 20.0f);
    const Mat4 view_proj = MultiplyMat4(view, proj);

    // Orbiting 3D point light positions (world space) & listener-space velocities across Frames 0..2
    const Vec3 kPointLightPos[kFrameCount] = {
        {-2.10f, 0.85f, 1.35f},  // Frame 0: left side, farther from camera
        { 0.00f, 1.35f, 1.85f},  // Frame 1: front-center, closest to camera
        { 1.55f, 0.90f, 2.05f},  // Frame 2: right side, medium distance
    };
    const Vec3 kEmitterVelocityLH[kFrameCount] = {
        {+14.0f, 0.0f, -16.0f},  // Frame 0: approaching listener -> Doppler > 1.04
        {+18.0f, 0.0f,   0.0f},  // Frame 1: tangential -> Doppler ~ 1.00
        {+14.0f, 0.0f, +16.0f},  // Frame 2: receding from listener -> Doppler < 0.96
    };
    const Vec3 kDirLightDirs[kFrameCount] = {
        Normalize({-0.55f, 0.85f, 0.62f}),
        Normalize({ 0.10f, 0.92f, 0.68f}),
        Normalize({ 0.62f, 0.80f, 0.60f}),
    };

    uint64_t frame_hashes[kFrameCount]{};
    double frame_ms[kFrameCount]{};
    FrameAudioMetrics audio_metrics[kFrameCount]{};
    std::vector<uint8_t> final_pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t last_out_w = 0;
    uint32_t last_out_h = 0;
    float last_meter_peak[2] = {0.0f, 0.0f};
    float last_meter_rms[2] = {0.0f, 0.0f};

    const SubmeshRange submeshes[3] = {front_cube, main_sphere, ground_plane};

    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        // A. Sync X3DAudio 3D spatialization with the orbiting point light!
        //    Convert world-space point light position relative to camera into DirectX LH listener space
        //    (+X right, +Y up, +Z forward from listener toward scene).
        const Vec3 lp = kPointLightPos[frame];
        X3DAUDIO_LISTENER listener{};
        listener.OrientFront = {0.0f, 0.0f, 1.0f};
        listener.OrientTop = {0.0f, 1.0f, 0.0f};
        listener.Position = {0.0f, 0.0f, 0.0f};
        listener.Velocity = {0.0f, 0.0f, 0.0f};

        X3DAUDIO_EMITTER emitter{};
        emitter.OrientFront = {0.0f, 0.0f, -1.0f};
        emitter.OrientTop = {0.0f, 1.0f, 0.0f};
        emitter.Position = {lp.x - kCamPos.x, lp.y - kCamPos.y, kCamPos.z - lp.z};
        emitter.Velocity = {kEmitterVelocityLH[frame].x, kEmitterVelocityLH[frame].y, kEmitterVelocityLH[frame].z};
        emitter.ChannelCount = 1u;
        emitter.CurveDistanceScaler = 1.0f;
        emitter.DopplerScaler = 1.0f;

        float matrix_coeffs[2] = {0.0f, 0.0f};
        float delay_times[2] = {0.0f, 0.0f};
        X3DAUDIO_DSP_SETTINGS dsp{};
        dsp.pMatrixCoefficients = matrix_coeffs;
        dsp.pDelayTimes = delay_times;
        dsp.SrcChannelCount = 1u;
        dsp.DstChannelCount = 2u;

        const uint32_t x3d_flags =
            OS_X3DAUDIO_CALCULATE_MATRIX |
            OS_X3DAUDIO_CALCULATE_DELAY |
            OS_X3DAUDIO_CALCULATE_LPF_DIRECT |
            OS_X3DAUDIO_CALCULATE_REVERB |
            OS_X3DAUDIO_CALCULATE_DOPPLER |
            OS_X3DAUDIO_CALCULATE_EMITTER_ANGLE;
        X3DAudioCalculate(x3d_handle, &listener, &emitter, x3d_flags, &dsp);

        audio_metrics[frame].distance = dsp.EmitterToListenerDistance;
        audio_metrics[frame].gain_left = matrix_coeffs[0];
        audio_metrics[frame].gain_right = matrix_coeffs[1];
        audio_metrics[frame].doppler_factor = dsp.DopplerFactor;
        audio_metrics[frame].lpf_direct = dsp.LPFDirectCoefficient;
        audio_metrics[frame].reverb_level = dsp.ReverbLevel;
        audio_metrics[frame].emitter_angle = dsp.EmitterToListenerAngle;
        audio_metrics[frame].delay_left_ms = delay_times[0];
        audio_metrics[frame].delay_right_ms = delay_times[1];

        (void)light_voice->SetChannelVolumes(2u, matrix_coeffs, OS_XAUDIO2_COMMIT_NOW);
        (void)light_voice->SetFrequencyRatio(dsp.DopplerFactor, OS_XAUDIO2_COMMIT_NOW);

        // Update & verify VolumeMeter APO with frame stereo peak/RMS levels
        float set_peaks[2] = {matrix_coeffs[0], matrix_coeffs[1]};
        float set_rms[2] = {matrix_coeffs[0] * 0.7071f, matrix_coeffs[1] * 0.7071f};
        OS_XAUDIO2FX_VOLUMEMETER_LEVELS vm_set{set_peaks, set_rms, 2u};
        meter_params_ifc->SetParameters(&vm_set, sizeof(vm_set));

        OS_XAUDIO2FX_VOLUMEMETER_LEVELS vm_get{last_meter_peak, last_meter_rms, 2u};
        meter_params_ifc->GetParameters(&vm_get, sizeof(vm_get));
        if (std::fabs(last_meter_peak[0] - set_peaks[0]) > 1e-4f ||
            std::fabs(last_meter_peak[1] - set_peaks[1]) > 1e-4f) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("XAudio2FX VolumeMeter round-trip mismatch");
        }

        // B. Wait for Xbox frame pipeline origin token
        D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
        hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
        if (FAILED(hr) || frame_token == 0) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("WaitFrameEventX", hr);
        }

        // C. Populate per-frame Blinn-Phong constant buffer
        LightingConstants cb{};
        std::memcpy(cb.view_proj, view_proj.m, sizeof(cb.view_proj));
        cb.camera_pos[0] = kCamPos.x;
        cb.camera_pos[1] = kCamPos.y;
        cb.camera_pos[2] = kCamPos.z;
        cb.camera_pos[3] = 1.0f;

        cb.dir_light_dir[0] = kDirLightDirs[frame].x;
        cb.dir_light_dir[1] = kDirLightDirs[frame].y;
        cb.dir_light_dir[2] = kDirLightDirs[frame].z;
        cb.dir_light_dir[3] = 1.65f;  // directional intensity

        cb.dir_light_color[0] = 1.00f;
        cb.dir_light_color[1] = 0.96f;
        cb.dir_light_color[2] = 0.90f;
        cb.dir_light_color[3] = 0.045f;  // low ambient for true shadowed hemisphere

        cb.point_light_pos[0] = lp.x;
        cb.point_light_pos[1] = lp.y;
        cb.point_light_pos[2] = lp.z;
        cb.point_light_pos[3] = 2.60f;  // orbiting point light intensity

        cb.point_light_color[0] = 0.85f;
        cb.point_light_color[1] = 0.94f;
        cb.point_light_color[2] = 1.00f;
        cb.point_light_color[3] = 0.22f;  // quadratic attenuation factor

        cb.material_params[0] = 1.25f;  // directional Blinn-Phong specular weight
        cb.material_params[1] = 1.35f;  // point Blinn-Phong specular weight
        cb.material_params[2] = 0.0f;
        cb.material_params[3] = 0.0f;

        std::memcpy(cb_mapped + static_cast<size_t>(frame) * sizeof(LightingConstants),
                    &cb, sizeof(LightingConstants));

        LARGE_INTEGER t0{};
        LARGE_INTEGER t1{};
        ::QueryPerformanceCounter(&t0);

        if (frame > 0u) {
            hr = alloc->Reset();
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                cleanup_audio_and_display();
                XGameRuntimeUninitialize();
                return Fail("CommandAllocator::Reset", hr);
            }
            hr = cmd->Reset(alloc.Get(), nullptr);
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                cleanup_audio_and_display();
                XGameRuntimeUninitialize();
                return Fail("CommandListX::Reset", hr);
            }
        }

        char frame_label[48]{};
        std::snprintf(frame_label, sizeof(frame_label), "SimpleLighting::Frame%u", frame);
        PIXBeginEventOnCommandList(cmd.Get(), 0xFF44DDAAull, frame_label);

        const D3D12_RESOURCE_BARRIER to_rt = MakeTransition(
            color_rt.Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &to_rt);

        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
        cmd->ClearRenderTargetView(rtv_handle, color_clear.Color, 0, nullptr);
        cmd->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scissor);

        cmd->SetGraphicsRootSignature(root_sig.Get());
        cmd->SetPipelineState(pso.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);

        const D3D12_GPU_VIRTUAL_ADDRESS cb_va =
            cb_resource->GetGPUVirtualAddress() + static_cast<uint64_t>(frame) * sizeof(LightingConstants);
        cmd->SetGraphicsRootConstantBufferView(0, cb_va);

        for (uint32_t s = 0; s < 3u; ++s) {
            cmd->DrawIndexedInstanced(
                submeshes[s].index_count, 1,
                submeshes[s].index_start,
                submeshes[s].base_vertex, 0);
        }

        const D3D12_RESOURCE_BARRIER to_present = MakeTransition(
            color_rt.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1, &to_present);

        PIXEndEventOnCommandList(cmd.Get());

        hr = cmd->Close();
        if (FAILED(hr)) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("CommandListX::Close", hr);
        }

        ID3D12CommandList* cmd_lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, cmd_lists);

        ID3D12Resource* plane_res[] = {color_rt.Get()};
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = frame_token;
        plane.ResourceCount = 1;
        plane.ppResources = plane_res;
        plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

        D3D12XBOX_PRESENT_PARAMETERS pres_params{};
        pres_params.SyncInterval = 1;

        hr = queue->PresentX(1, &plane, &pres_params);
        if (FAILED(hr)) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("PresentX(1)", hr);
        }

        // Fence sync
        ++fence_val;
        hr = queue->Signal(fence.Get(), fence_val);
        if (FAILED(hr)) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("CommandQueueX::Signal", hr);
        }
        if (fence->GetCompletedValue() < fence_val) {
            hr = fence->SetEventOnCompletion(fence_val, fence_event);
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                cleanup_audio_and_display();
                XGameRuntimeUninitialize();
                return Fail("Fence::SetEventOnCompletion", hr);
            }
            const DWORD wr = ::WaitForSingleObject(fence_event, 15000u);
            if (wr != WAIT_OBJECT_0) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                cleanup_audio_and_display();
                XGameRuntimeUninitialize();
                return Fail("Fence wait timeout", wr);
            }
            PIXNotifyWakeFromFenceSignal(fence_event);
        }

        ::QueryPerformanceCounter(&t1);
        frame_ms[frame] = QpcMs(t0, t1, qpc_freq);
        PIXReportCounter(L"SimpleLighting.FrameMs", static_cast<float>(frame_ms[frame]));

        hr = OpenScarlettD3D12GetLastPresentHash(
            queue.Get(), &frame_hashes[frame], &last_out_w, &last_out_h);
        if (FAILED(hr) || last_out_w != kWidth || last_out_h != kHeight || frame_hashes[frame] == 0ull) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
        }

        // Read back each frame and dump P6 PPM to T:\simple_lighting_frame{N}.ppm
        std::vector<uint8_t> frame_pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
        uint32_t copied = 0;
        hr = OpenScarlettD3D12CopyLastPresentPixels(
            queue.Get(), frame_pixels.data(), static_cast<uint32_t>(frame_pixels.size()), &copied);
        if (FAILED(hr) || copied != frame_pixels.size()) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
        }

        wchar_t ppm_path[64]{};
        std::swprintf(ppm_path, 64, L"T:\\simple_lighting_frame%u.ppm", frame);
        if (!WritePpmW(ppm_path, kWidth, kHeight, frame_pixels.data())) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            cleanup_audio_and_display();
            XGameRuntimeUninitialize();
            return Fail("Write frame PPM to T:\\", ::GetLastError());
        }

        if (frame == kFrameCount - 1u) {
            final_pixels = std::move(frame_pixels);
        }
    }

    cb_resource->Unmap(0, nullptr);

    // 12. Verify X3DAudio physical invariants across orbiting emitter trajectory
    if (!(audio_metrics[0].gain_left > audio_metrics[0].gain_right * 2.0f)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("X3DAudio Frame 0 expected strong left panning");
    }
    if (!(std::fabs(audio_metrics[1].gain_left - audio_metrics[1].gain_right) < 0.05f)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("X3DAudio Frame 1 expected centered panning");
    }
    if (!(audio_metrics[2].gain_right > audio_metrics[2].gain_left * 2.0f)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("X3DAudio Frame 2 expected strong right panning");
    }
    const float energy_f0 = audio_metrics[0].gain_left * audio_metrics[0].gain_left +
                            audio_metrics[0].gain_right * audio_metrics[0].gain_right;
    const float energy_f1 = audio_metrics[1].gain_left * audio_metrics[1].gain_left +
                            audio_metrics[1].gain_right * audio_metrics[1].gain_right;
    if (!(audio_metrics[0].distance > audio_metrics[1].distance && energy_f1 > energy_f0 * 1.5f)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("X3DAudio distance attenuation invariant failed");
    }
    if (!(audio_metrics[0].doppler_factor > 1.03f && audio_metrics[2].doppler_factor < 0.97f)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("X3DAudio Doppler approaching/receding shift invariant failed");
    }

    // 13. Verify 3 distinct animated frame hashes
    if (frame_hashes[0] == frame_hashes[1] ||
        frame_hashes[1] == frame_hashes[2] ||
        frame_hashes[0] == frame_hashes[2]) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Multi-frame FNV-1a hashes are not distinct");
    }

    // 14. Verify final frame (Frame 2) Blinn-Phong shading, specular highlights & depth occlusion
    auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return &final_pixels[(static_cast<size_t>(y) * kWidth + x) * 4u];
    };

    const uint8_t* tl = pixel_at(0, 0);
    const uint8_t* tr = pixel_at(kWidth - 1u, 0);
    if (tl[0] != kBgR || tl[1] != kBgG || tl[2] != kBgB || tl[3] != kBgA ||
        tr[0] != kBgR || tr[1] != kBgG || tr[2] != kBgB || tr[3] != kBgA) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Background clear corner pixel mismatch");
    }

    // Depth occlusion check: Front Faceted Cube (cool azure-blue) was drawn FIRST,
    // Main Smooth Sphere (warm amber-orange) was drawn SECOND. Projected front cube
    // center MUST remain cool blue-dominant!
    int32_t cube_px = 0;
    int32_t cube_py = 0;
    if (!ProjectWorldToPixel(kFrontCubePos, view_proj, kWidth, kHeight, cube_px, cube_py)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Front cube center projection out of viewport");
    }
    double cube_sum_r = 0.0;
    double cube_sum_g = 0.0;
    double cube_sum_b = 0.0;
    uint32_t cube_samples = 0;
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            const int32_t sx = std::max(0, std::min(static_cast<int32_t>(kWidth) - 1, cube_px + dx));
            const int32_t sy = std::max(0, std::min(static_cast<int32_t>(kHeight) - 1, cube_py + dy));
            const uint8_t* p = pixel_at(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
            cube_sum_r += static_cast<double>(p[0]);
            cube_sum_g += static_cast<double>(p[1]);
            cube_sum_b += static_cast<double>(p[2]);
            ++cube_samples;
        }
    }
    const double cube_avg_r = cube_sum_r / static_cast<double>(cube_samples);
    const double cube_avg_g = cube_sum_g / static_cast<double>(cube_samples);
    const double cube_avg_b = cube_sum_b / static_cast<double>(cube_samples);
    if (!(cube_avg_b > cube_avg_r + 30.0 && cube_avg_b > 90.0)) {
        std::fprintf(stderr,
                     "[gdk_simple_lighting] Depth occlusion check at (%d,%d) failed: avgRGB=(%.1f, %.1f, %.1f)\n",
                     cube_px, cube_py, cube_avg_r, cube_avg_g, cube_avg_b);
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Depth occlusion check failed (front cube overwritten by back sphere)");
    }

    // Verify visible lit flank of Main Smooth Sphere is warm amber-orange
    int32_t sph_px = 0;
    int32_t sph_py = 0;
    const Vec3 sphere_lit_pt{kMainSpherePos.x + 0.50f, kMainSpherePos.y + 0.30f, kMainSpherePos.z + 0.58f};
    if (!ProjectWorldToPixel(sphere_lit_pt, view_proj, kWidth, kHeight, sph_px, sph_py)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Sphere lit flank projection out of viewport");
    }
    double sph_sum_r = 0.0;
    double sph_sum_b = 0.0;
    uint32_t sph_samples = 0;
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            const int32_t sx = std::max(0, std::min(static_cast<int32_t>(kWidth) - 1, sph_px + dx));
            const int32_t sy = std::max(0, std::min(static_cast<int32_t>(kHeight) - 1, sph_py + dy));
            const uint8_t* p = pixel_at(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
            sph_sum_r += static_cast<double>(p[0]);
            sph_sum_b += static_cast<double>(p[2]);
            ++sph_samples;
        }
    }
    const double sph_avg_r = sph_sum_r / static_cast<double>(sph_samples);
    const double sph_avg_b = sph_sum_b / static_cast<double>(sph_samples);
    if (!(sph_avg_r > sph_avg_b + 25.0 && sph_avg_r > 120.0)) {
        std::fprintf(stderr,
                     "[gdk_simple_lighting] Sphere lit flank check at (%d,%d) failed: avgR=%.1f avgB=%.1f\n",
                     sph_px, sph_py, sph_avg_r, sph_avg_b);
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Main warm amber sphere Blinn-Phong shading check failed");
    }

    // Full-frame pixel classification metrics
    uint32_t bg_pixels = 0;
    uint32_t non_bg_pixels = 0;
    uint32_t specular_pixels = 0;
    uint32_t diffuse_pixels = 0;
    uint32_t shadow_pixels = 0;
    uint32_t front_cube_pixels = 0;
    uint32_t back_sphere_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(16384);

    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = final_pixels[i * 4u + 0u];
        const uint8_t g = final_pixels[i * 4u + 1u];
        const uint8_t b = final_pixels[i * 4u + 2u];
        unique_rgb.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b));

        if (r == kBgR && g == kBgG && b == kBgB) {
            ++bg_pixels;
            continue;
        }
        ++non_bg_pixels;

        const uint32_t luma = (static_cast<uint32_t>(r) * 77u +
                               static_cast<uint32_t>(g) * 150u +
                               static_cast<uint32_t>(b) * 29u) >> 8;
        const uint8_t max_c = std::max(r, std::max(g, b));
        const int32_t bg_diff = std::abs(static_cast<int32_t>(r) - static_cast<int32_t>(kBgR)) +
                                std::abs(static_cast<int32_t>(g) - static_cast<int32_t>(kBgG)) +
                                std::abs(static_cast<int32_t>(b) - static_cast<int32_t>(kBgB));

        if (max_c >= 215u && luma >= 185u) {
            ++specular_pixels;
        } else if (luma >= 65u) {
            ++diffuse_pixels;
        } else if (bg_diff >= 10) {
            ++shadow_pixels;
        }

        if (static_cast<int32_t>(b) >= static_cast<int32_t>(r) + 30 && b >= 85u) {
            ++front_cube_pixels;
        }
        if (static_cast<int32_t>(r) >= static_cast<int32_t>(b) + 30 && r >= 65u) {
            ++back_sphere_pixels;
        }
    }

    if (bg_pixels < 6000u || non_bg_pixels < 18000u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Unexpected Blinn-Phong scene coverage", non_bg_pixels);
    }
    if (specular_pixels < 40u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Missing Blinn-Phong specular highlights", specular_pixels);
    }
    if (diffuse_pixels < 4000u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Insufficient diffuse Blinn-Phong pixels", diffuse_pixels);
    }
    if (shadow_pixels < 300u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Insufficient shadowed hemisphere pixels", shadow_pixels);
    }
    if (front_cube_pixels < 500u || back_sphere_pixels < 2000u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Insufficient front cube / back sphere coverage", front_cube_pixels);
    }
    if (unique_rgb.size() < 1500u) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Too few unique Blinn-Phong shaded colors", static_cast<int64_t>(unique_rgb.size()));
    }

    // 15. Write T:\metrics.txt and T:\audio_display_report.txt
    const double gpu_total_ms = frame_ms[0] + frame_ms[1] + frame_ms[2];
    char metrics_buf[2048]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=SimpleLighting\n"
        "title_id=0x%08X\n"
        "gpu_backend_env=%s\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "width=%u\n"
        "height=%u\n"
        "vertex_count=%zu\n"
        "index_count=%zu\n"
        "triangle_count=%zu\n"
        "frame0_fnv1a=0x%016llX\n"
        "frame1_fnv1a=0x%016llX\n"
        "frame2_fnv1a=0x%016llX\n"
        "bg_pixels=%u\n"
        "non_bg_pixels=%u\n"
        "specular_pixels=%u\n"
        "diffuse_pixels=%u\n"
        "shadow_pixels=%u\n"
        "front_cube_pixels=%u\n"
        "back_sphere_pixels=%u\n"
        "unique_colors=%zu\n"
        "depth_occlusion_verified=1\n"
        "gpu_frame0_ms=%.3f\n"
        "gpu_frame1_ms=%.3f\n"
        "gpu_frame2_ms=%.3f\n"
        "gpu_total_ms=%.3f\n"
        "xdisplay_hdr_result=%u\n"
        "xdisplay_min_nits=%.4f\n"
        "xdisplay_max_nits=%.1f\n"
        "xdisplay_max_ff_nits=%.1f\n"
        "xdisplay_deferral_ok=1\n"
        "xaudio2_channel_mask=0x%X\n"
        "xaudio2fx_reverb_wetdry=%.2f\n"
        "xaudio2fx_reverb_decay=%.2f\n"
        "xaudio2fx_meter_peak_l=%.4f\n"
        "xaudio2fx_meter_peak_r=%.4f\n"
        "x3d_f0_dist=%.4f\n"
        "x3d_f0_gain_l=%.4f\n"
        "x3d_f0_gain_r=%.4f\n"
        "x3d_f0_doppler=%.4f\n"
        "x3d_f1_dist=%.4f\n"
        "x3d_f1_gain_l=%.4f\n"
        "x3d_f1_gain_r=%.4f\n"
        "x3d_f1_doppler=%.4f\n"
        "x3d_f2_dist=%.4f\n"
        "x3d_f2_gain_l=%.4f\n"
        "x3d_f2_gain_r=%.4f\n"
        "x3d_f2_doppler=%.4f\n",
        title_id,
        gpu_backend_env.c_str(),
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        last_out_w,
        last_out_h,
        vertices.size(),
        indices.size(),
        indices.size() / 3u,
        static_cast<unsigned long long>(frame_hashes[0]),
        static_cast<unsigned long long>(frame_hashes[1]),
        static_cast<unsigned long long>(frame_hashes[2]),
        bg_pixels,
        non_bg_pixels,
        specular_pixels,
        diffuse_pixels,
        shadow_pixels,
        front_cube_pixels,
        back_sphere_pixels,
        unique_rgb.size(),
        frame_ms[0],
        frame_ms[1],
        frame_ms[2],
        gpu_total_ms,
        static_cast<uint32_t>(hdr_result),
        hdr_info.minToneMapLuminance,
        hdr_info.maxToneMapLuminance,
        hdr_info.maxFullFrameToneMapLuminance,
        channel_mask,
        rev_got.WetDryMix,
        rev_got.DecayTime,
        last_meter_peak[0],
        last_meter_peak[1],
        audio_metrics[0].distance,
        audio_metrics[0].gain_left,
        audio_metrics[0].gain_right,
        audio_metrics[0].doppler_factor,
        audio_metrics[1].distance,
        audio_metrics[1].gain_left,
        audio_metrics[1].gain_right,
        audio_metrics[1].doppler_factor,
        audio_metrics[2].distance,
        audio_metrics[2].gain_left,
        audio_metrics[2].gain_right,
        audio_metrics[2].doppler_factor);

    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    char report_buf[1024]{};
    const int report_len = std::snprintf(
        report_buf, sizeof(report_buf),
        "SimpleLighting XDisplay + X3DAudio + XAudio2FX Report\n"
        "XDisplay.HdrResult=%s (min=%.4f max=%.1f maxFF=%.1f nits)\n"
        "XDisplay.TimeoutDeferral=Acquired+Closed\n"
        "XAudio2FX.Reverb=WetDry:%.1f%% Decay:%.2fs RoomSize:%.1f\n"
        "XAudio2FX.VolumeMeter=PeakL:%.4f PeakR:%.4f RmsL:%.4f RmsR:%.4f\n"
        "X3DAudio.Frame0=Dist:%.3fm Pan(L=%.4f,R=%.4f) Doppler:%.4f LPF:%.3f Rev:%.3f\n"
        "X3DAudio.Frame1=Dist:%.3fm Pan(L=%.4f,R=%.4f) Doppler:%.4f LPF:%.3f Rev:%.3f\n"
        "X3DAudio.Frame2=Dist:%.3fm Pan(L=%.4f,R=%.4f) Doppler:%.4f LPF:%.3f Rev:%.3f\n",
        hdr_result == XDisplayHdrModeResult_Enabled ? "Enabled" : "Disabled",
        hdr_info.minToneMapLuminance,
        hdr_info.maxToneMapLuminance,
        hdr_info.maxFullFrameToneMapLuminance,
        rev_got.WetDryMix,
        rev_got.DecayTime,
        rev_got.RoomSize,
        last_meter_peak[0],
        last_meter_peak[1],
        last_meter_rms[0],
        last_meter_rms[1],
        audio_metrics[0].distance, audio_metrics[0].gain_left, audio_metrics[0].gain_right,
        audio_metrics[0].doppler_factor, audio_metrics[0].lpf_direct, audio_metrics[0].reverb_level,
        audio_metrics[1].distance, audio_metrics[1].gain_left, audio_metrics[1].gain_right,
        audio_metrics[1].doppler_factor, audio_metrics[1].lpf_direct, audio_metrics[1].reverb_level,
        audio_metrics[2].distance, audio_metrics[2].gain_left, audio_metrics[2].gain_right,
        audio_metrics[2].doppler_factor, audio_metrics[2].lpf_direct, audio_metrics[2].reverb_level);
    if (report_len <= 0 ||
        !WriteWholeFileW(L"T:\\audio_display_report.txt", report_buf, static_cast<size_t>(report_len))) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("Write T:\\audio_display_report.txt", ::GetLastError());
    }

    // 16. Clean PresentX(0) blank teardown
    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        cleanup_audio_and_display();
        XGameRuntimeUninitialize();
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_simple_lighting] PASS | TitleId=0x%08X | BackendEnv=%s | Adapter=%s (%s) | %ux%u | "
        "Tris=%zu | Spec=%u Diff=%u Shad=%u | X3D Pan F0(%.2f/%.2f)->F2(%.2f/%.2f) Dop(%.3f->%.3f) | %.2f ms\n",
        title_id,
        gpu_backend_env.c_str(),
        gpu_info.AdapterName,
        use_warp ? "WARP" : "HW",
        last_out_w,
        last_out_h,
        indices.size() / 3u,
        specular_pixels,
        diffuse_pixels,
        shadow_pixels,
        audio_metrics[0].gain_left,
        audio_metrics[0].gain_right,
        audio_metrics[2].gain_left,
        audio_metrics[2].gain_right,
        audio_metrics[0].doppler_factor,
        audio_metrics[2].doppler_factor,
        gpu_total_ms);
    std::fflush(stdout);

    // Clean teardown
    cb_resource.Reset();
    ib.Reset();
    vb.Reset();
    pso.Reset();
    root_sig.Reset();
    dsv_heap.Reset();
    rtv_heap.Reset();
    depth_rt.Reset();
    color_rt.Reset();
    ::CloseHandle(fence_event);
    fence.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    cleanup_audio_and_display();
    XGameRuntimeUninitialize();
    return 0;
}
