// OpenScarlett MIT Standalone Xbox-ABI Guest Sample (Wave 4):
// HDRCubeMap — High-Dynamic-Range FP16 6-face TextureCube + Default (nullptr desc)
// Texture2DArray SRV slice preservation (ScarlettX #46) + Reflective 3D Sphere +
// HDR10 ST.2084 PQ Encode Pass (R10G10B10A2_UNORM + G2084_NONE_P2020 + HDR10 Metadata) +
// XDisplayTryEnableHdrMode / XDisplayAcquireTimeoutDeferral + Automatic SDR ACES/Reinhard
// tone-mapped fallback verification.
//
// Deliberately does NOT link openscarlett_core. Runs inside an isolated zero-DLL package
// directory launched via `openscarlett run <package_dir>`.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kSceneHeight = 192u;
constexpr uint32_t kCubeFaceSize = 64u;
constexpr uint32_t kCubeFaceCount = 6u;
constexpr uint32_t kFrameCount = 2u;
constexpr uint32_t kExpectedTitleId = 0x5CA01003u;
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

struct SphereVertex {
    float position[3];
    float normal[3];
};
static_assert(sizeof(SphereVertex) == 24, "SphereVertex must be 24 bytes");

struct alignas(256) SceneConstants {
    float world[16];
    float view_proj[16];
    float camera_pos[4];
    float cam_right[4];
    float cam_up[4];
    float cam_forward[4];
    float viewport_params[4];  // width, height, scene_height, tan_half_fov
    float pad[12];
};
static_assert(sizeof(SceneConstants) == 256, "SceneConstants must be 256-byte aligned");

struct alignas(256) Hdr10EncodeConstants {
    float linear_to_nits;
    float max_display_nits;
    float pad[62];
};
static_assert(sizeof(Hdr10EncodeConstants) == 256, "Hdr10EncodeConstants must be 256-byte aligned");

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_hdr_cubemap] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

LRESULT CALLBACK HdrGuestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateHiddenGuestWindow(uint32_t client_w, uint32_t client_h) {
    static const wchar_t* kClassName = L"OpenScarlettGdkHdrCubeMapHiddenClass";
    static bool s_registered = false;
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &HdrGuestWndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        s_registered = true;
    }
    RECT wr{0, 0, static_cast<LONG>(client_w), static_cast<LONG>(client_h)};
    ::AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    return ::CreateWindowExW(
        0,
        kClassName,
        L"OpenScarlett HDRCubeMap",
        WS_OVERLAPPEDWINDOW,
        64,
        64,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);
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

Mat4 MakeIdentity() noexcept {
    Mat4 r{};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][3] = 1.0f;
    return r;
}

Mat4 MakeLookAtRH(Vec3 eye, Vec3 target, Vec3 up, Vec3& out_right, Vec3& out_up, Vec3& out_fwd) noexcept {
    const Vec3 zaxis = Normalize(Sub(eye, target));
    const Vec3 xaxis = Normalize(Cross(up, zaxis));
    const Vec3 yaxis = Cross(zaxis, xaxis);

    out_right = xaxis;
    out_up = yaxis;
    out_fwd = {-zaxis.x, -zaxis.y, -zaxis.z};

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

void BuildUvSphere(std::vector<SphereVertex>& verts, std::vector<uint32_t>& indices,
                   float radius, uint32_t lon_segs, uint32_t lat_segs) {
    verts.clear();
    indices.clear();
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

            SphereVertex vtx{};
            vtx.position[0] = nx * radius;
            vtx.position[1] = ny * radius;
            vtx.position[2] = nz * radius;
            vtx.normal[0] = nx;
            vtx.normal[1] = ny;
            vtx.normal[2] = nz;
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
}

uint16_t FloatToHalf(float value) noexcept {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;

    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mant = (mant | 0x00800000u) >> (1 - exp);
        return static_cast<uint16_t>(sign | ((mant + 0x00001000u) >> 13));
    }
    if (exp == 0xFF - 127 + 15) {
        if (mant == 0u) {
            return static_cast<uint16_t>(sign | 0x7C00u);
        }
        return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
    }
    mant += 0x00001000u;
    if ((mant & 0x00800000u) != 0u) {
        mant = 0u;
        ++exp;
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7BFFu);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}

// Procedural 6-face HDR FP16 environment cubemap (linear Rec.709; 1.0 = 80 nits, 8.0 = 640 nits).
// Each face has a distinct dominant color AND calibrated HDR dynamic range:
//   Face 0 (+X): Warm Amber-Red + 8.0 linear (~640 nit) solar lobe
//   Face 1 (-X): Emerald Green  + 6.5 linear (~520 nit) rim highlight
//   Face 2 (+Y): Sapphire Blue  + 8.2 linear (~656 nit) zenith star
//   Face 3 (-Y): Dark Obsidian Shadow (~7 nits; <50 nits everywhere)
//   Face 4 (+Z): Bright Cyan    + 7.6 linear (~608 nit) horizon sun
//   Face 5 (-Z): Royal Magenta  + 6.8 linear (~544 nit) specular beacon
void GenerateHdrCubeFaceFp16(uint32_t face, uint32_t size, std::vector<uint16_t>& out_rgba16f) {
    out_rgba16f.resize(static_cast<size_t>(size) * size * 4u);

    for (uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float dx = u - 0.5f;
            const float dy = v - 0.5f;
            const float r2 = dx * dx + dy * dy;
            const float lobe = std::exp(-r2 * 26.0f);
            const float grad = 0.85f + 0.25f * (1.0f - v);

            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            switch (face) {
                case 0u:  // +X: Red-Orange + bright HDR sun lobe (~640 nits)
                    r = 1.90f * grad + 6.40f * lobe;
                    g = 0.28f * grad + 2.80f * lobe;
                    b = 0.08f * grad + 0.90f * lobe;
                    break;
                case 1u:  // -X: Emerald Green + HDR rim lobe (~520 nits)
                    r = 0.14f * grad + 1.10f * lobe;
                    g = 1.95f * grad + 4.80f * lobe;
                    b = 0.26f * grad + 1.50f * lobe;
                    break;
                case 2u:  // +Y: Sapphire Blue + HDR zenith star (~660 nits)
                    r = 0.16f * grad + 6.80f * lobe;
                    g = 0.42f * grad + 7.20f * lobe;
                    b = 2.30f * grad + 6.20f * lobe;
                    break;
                case 3u:  // -Y: Dark Shadow Floor (~7 nits base, <25 nits everywhere!)
                    r = 0.07f + 0.04f * lobe;
                    g = 0.08f + 0.04f * lobe;
                    b = 0.11f + 0.05f * lobe;
                    break;
                case 4u:  // +Z: Bright Cyan Sky + HDR horizon sun (~610 nits)
                    r = 0.14f * grad + 3.20f * lobe;
                    g = 1.75f * grad + 5.60f * lobe;
                    b = 2.15f * grad + 5.90f * lobe;
                    break;
                case 5u:  // -Z: Royal Magenta + HDR specular beacon (~544 nits)
                default:
                    r = 2.00f * grad + 5.00f * lobe;
                    g = 0.16f * grad + 1.10f * lobe;
                    b = 1.95f * grad + 4.90f * lobe;
                    break;
            }

            const size_t off = (static_cast<size_t>(y) * size + x) * 4u;
            out_rgba16f[off + 0u] = FloatToHalf(r);
            out_rgba16f[off + 1u] = FloatToHalf(g);
            out_rgba16f[off + 2u] = FloatToHalf(b);
            out_rgba16f[off + 3u] = FloatToHalf(1.0f);
        }
    }
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

struct DecodedNitsRgb {
    float r;
    float g;
    float b;
    float luma;
    float peak;
};

DecodedNitsRgb DecodeHdr10PixelAt(const std::vector<uint8_t>& raw_hdr10, uint32_t x, uint32_t y) noexcept {
    uint32_t packed = 0u;
    std::memcpy(&packed, raw_hdr10.data() + (static_cast<size_t>(y) * kWidth + x) * 4u, sizeof(packed));
    const float r_pq = static_cast<float>(packed & 0x3FFu) / 1023.0f;
    const float g_pq = static_cast<float>((packed >> 10) & 0x3FFu) / 1023.0f;
    const float b_pq = static_cast<float>((packed >> 20) & 0x3FFu) / 1023.0f;
    const float rn = OpenScarlettD3D12PqSt2084ToLinearNits(r_pq);
    const float gn = OpenScarlettD3D12PqSt2084ToLinearNits(g_pq);
    const float bn = OpenScarlettD3D12PqSt2084ToLinearNits(b_pq);
    const float luma = 0.2627f * rn + 0.6780f * gn + 0.0593f * bn;
    const float peak = std::max(rn, std::max(gn, bn));
    return {rn, gn, bn, luma, peak};
}

bool VerifyFaceColorSignature(uint32_t face, const DecodedNitsRgb& base_px, const DecodedNitsRgb& center_px) noexcept {
    switch (face) {
        case 0u:  // +X Red-Orange + HDR peak
            return (base_px.r > base_px.g * 1.4f) && (base_px.r > base_px.b * 2.0f) && (center_px.peak > 300.0f);
        case 1u:  // -X Emerald Green + HDR peak
            return (base_px.g > base_px.r * 1.4f) && (base_px.g > base_px.b * 1.4f) && (center_px.peak > 300.0f);
        case 2u:  // +Y Sapphire Blue base + HDR zenith peak
            return (base_px.b > base_px.r * 1.4f) && (base_px.b > base_px.g * 1.2f) && (center_px.peak > 350.0f);
        case 3u:  // -Y Dark Obsidian Shadow (< 35 nits everywhere!)
            return (base_px.peak < 30.0f) && (center_px.peak < 35.0f) && (base_px.luma > 1.0f);
        case 4u:  // +Z Cyan base + HDR horizon sun
            return (base_px.g > base_px.r * 1.25f) && (base_px.b > base_px.r * 1.25f) && (center_px.peak > 300.0f);
        case 5u:  // -Z Royal Magenta + HDR specular beacon
            return (base_px.r > base_px.g * 1.6f) && (base_px.b > base_px.g * 1.6f) && (center_px.peak > 300.0f);
        default:
            return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        }
    }

    PIXBeginEventOnCpu(0xFFFFAA22ull, "HDRCubeMap::Startup");

    // 1. GDK Game Runtime + TitleId + Scarlett DeviceType + XDisplay HDR & Timeout Deferral
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }

    uint32_t title_id = 0;
    if (XGameGetXboxTitleId(&title_id) != XGR_S_OK || title_id != kExpectedTitleId) {
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

    XDisplayTimeoutDeferralHandle timeout_deferral = nullptr;
    const XGameResult def_hr = XDisplayAcquireTimeoutDeferral(&timeout_deferral);
    if (def_hr != XGR_S_OK || !timeout_deferral) {
        PIXEndEventOnCpu();
        XGameRuntimeUninitialize();
        return Fail("XDisplayAcquireTimeoutDeferral", def_hr);
    }

    XDisplayHdrModeInfo display_hdr_info{};
    const XDisplayHdrModeResult hdr_result = XDisplayTryEnableHdrMode(
        XDisplayHdrModePreference_PreferHdr, &display_hdr_info);
    const bool hdr_enabled = (hdr_result == XDisplayHdrModeResult_Enabled);

    auto cleanup_gdk = [&]() {
        if (timeout_deferral) {
            XDisplayCloseTimeoutDeferralHandle(timeout_deferral);
            timeout_deferral = nullptr;
        }
        XGameRuntimeUninitialize();
    };

    // 2. Hidden Win32 window for real DXGI IDXGISwapChain4 HDR10 swapchain exercise
    HWND hwnd = CreateHiddenGuestWindow(kWidth, kHeight);

    // 3. Create Xbox Scarlett D3D12X device
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.DisableGeometryShaderAllocations = 1;
    dev_params.DisableTessellationShaderAllocations = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr) || ((gpu_info.IsWarp != 0u) != use_warp)) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("GetGpuHardwareInfoX / WARP mismatch", hr);
    }

    // 4. Load precompiled SM 6.5 (or SM 6.2 on WARP) shaders from G:\.
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    const bool supports_sm65 =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
        sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

    std::vector<uint8_t> skybox_vs_blob;
    std::vector<uint8_t> skybox_ps_blob;
    std::vector<uint8_t> sphere_vs_blob;
    std::vector<uint8_t> sphere_ps_blob;
    std::vector<uint8_t> hdr10_vs_blob;
    std::vector<uint8_t> hdr10_ps_blob;

    if (!ReadWholeFileW(supports_sm65 ? L"G:\\HdrSkyboxVS.cso" : L"G:\\HdrSkyboxVS_sm62.cso", skybox_vs_blob) ||
        !ReadWholeFileW(supports_sm65 ? L"G:\\HdrSkyboxPS.cso" : L"G:\\HdrSkyboxPS_sm62.cso", skybox_ps_blob) ||
        !ReadWholeFileW(supports_sm65 ? L"G:\\HdrSphereVS.cso" : L"G:\\HdrSphereVS_sm62.cso", sphere_vs_blob) ||
        !ReadWholeFileW(supports_sm65 ? L"G:\\HdrSpherePS.cso" : L"G:\\HdrSpherePS_sm62.cso", sphere_ps_blob) ||
        !ReadWholeFileW(supports_sm65 ? L"G:\\Hdr10EncodeVS.cso" : L"G:\\Hdr10EncodeVS_sm62.cso", hdr10_vs_blob) ||
        !ReadWholeFileW(supports_sm65 ? L"G:\\Hdr10EncodePS.cso" : L"G:\\Hdr10EncodePS_sm62.cso", hdr10_ps_blob)) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("Read DXIL CSO shaders from G:\\", ::GetLastError());
    }

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("SetFrameIntervalX", hr);
    }

    // 5. CommandQueueX, CommandAllocator, CommandListX, Fence
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommandQueue", hr);
    }

    // Configure HDR10 metadata & PresentX swapchain parameters (HDR10 ST.2084 vs SDR fallback)
    D3D12XBOX_HDR10_METADATA hdr_meta{};
    hdr_meta.MaxMasteringLuminance =
        static_cast<uint32_t>(std::max(1000.0f, display_hdr_info.maxToneMapLuminance));
    hdr_meta.MinMasteringLuminance = 50u;  // 0.005 nits
    hdr_meta.MaxContentLightLevel = 1000u;
    hdr_meta.MaxFrameAverageLightLevel = 400u;
    hr = OpenScarlettD3D12SetHdrMetaData(queue.Get(), &hdr_meta);
    if (FAILED(hr)) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("OpenScarlettD3D12SetHdrMetaData", hr);
    }

    D3D12XBOX_PRESENTX_CONFIG present_cfg{};
    present_cfg.OutputWindow = hwnd;
    present_cfg.PreferredFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    present_cfg.ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    present_cfg.ToneMapMode = hdr_enabled ? D3D12XBOX_TONEMAP_ACES : D3D12XBOX_TONEMAP_REINHARD;
    present_cfg.PaperWhiteNits = 200.0f;
    present_cfg.MaxDisplayNits =
        (display_hdr_info.maxToneMapLuminance > 0.0f) ? display_hdr_info.maxToneMapLuminance : 1000.0f;
    present_cfg.ForceSdrToneMapFallback = hdr_enabled ? 0u : 1u;
    hr = OpenScarlettD3D12SetPresentConfig(queue.Get(), &present_cfg);
    if (FAILED(hr)) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("OpenScarlettD3D12SetPresentConfig", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommandList", hr);
    }

    GuestComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr) || !fence) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateEventW(fence)", ::GetLastError());
    }
    uint64_t fence_val = 0;

    // 6. Create FP16 HDR Scene RT (R16G16B16A16_FLOAT) & HDR10 ST.2084 Backbuffer (R10G10B10A2_UNORM)
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_CLEAR_VALUE fp16_clear{};
    fp16_clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_clear.Color[3] = 1.0f;

    D3D12_RESOURCE_DESC fp16_rt_desc{};
    fp16_rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fp16_rt_desc.Width = kWidth;
    fp16_rt_desc.Height = kHeight;
    fp16_rt_desc.DepthOrArraySize = 1;
    fp16_rt_desc.MipLevels = 1;
    fp16_rt_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_rt_desc.SampleDesc.Count = 1;
    fp16_rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    fp16_rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> hdr_fp16_rt;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &fp16_rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &fp16_clear, IID_PPV_ARGS(hdr_fp16_rt.Put()));
    if (FAILED(hr) || !hdr_fp16_rt) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(FP16 HDR RT)", hr);
    }

    D3D12_CLEAR_VALUE hdr10_clear{};
    hdr10_clear.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    hdr10_clear.Color[3] = 1.0f;

    D3D12_RESOURCE_DESC hdr10_rt_desc{};
    hdr10_rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    hdr10_rt_desc.Width = kWidth;
    hdr10_rt_desc.Height = kHeight;
    hdr10_rt_desc.DepthOrArraySize = 1;
    hdr10_rt_desc.MipLevels = 1;
    hdr10_rt_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    hdr10_rt_desc.SampleDesc.Count = 1;
    hdr10_rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    hdr10_rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> hdr10_rt;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &hdr10_rt_desc,
        D3D12_RESOURCE_STATE_PRESENT, &hdr10_clear, IID_PPV_ARGS(hdr10_rt.Put()));
    if (FAILED(hr) || !hdr10_rt) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(HDR10 R10G10B10A2 RT)", hr);
    }

    // RTV heap (slot 0 = hdr_fp16_rt, slot 1 = hdr10_rt)
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 2;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE fp16_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE hdr10_rtv{fp16_rtv.ptr + rtv_inc};
    device->CreateRenderTargetView(hdr_fp16_rt.Get(), nullptr, fp16_rtv);
    device->CreateRenderTargetView(hdr10_rt.Get(), nullptr, hdr10_rtv);

    // 7. Create 6-Face HDR FP16 TextureCube (DepthOrArraySize = 6, R16G16B16A16_FLOAT)
    D3D12_RESOURCE_DESC cube_desc{};
    cube_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cube_desc.Width = kCubeFaceSize;
    cube_desc.Height = kCubeFaceSize;
    cube_desc.DepthOrArraySize = kCubeFaceCount;
    cube_desc.MipLevels = 1;
    cube_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cube_desc.SampleDesc.Count = 1;
    cube_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    GuestComPtr<ID3D12Resource> cubemap_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &cube_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(cubemap_tex.Put()));
    if (FAILED(hr) || !cubemap_tex) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(6-face FP16 TextureCube)", hr);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT cube_fps[kCubeFaceCount]{};
    UINT cube_num_rows[kCubeFaceCount]{};
    UINT64 cube_row_bytes[kCubeFaceCount]{};
    UINT64 cube_total_bytes = 0ull;
    device->GetCopyableFootprints(
        &cube_desc, 0u, kCubeFaceCount, 0ull,
        cube_fps, cube_num_rows, cube_row_bytes, &cube_total_bytes);

    const D3D12_RESOURCE_DESC cube_up_desc = MakeBufferDesc(cube_total_bytes);
    GuestComPtr<ID3D12Resource> cube_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cube_up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cube_upload.Put()));
    if (FAILED(hr) || !cube_upload) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(cube_upload)", hr);
    }

    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = cube_upload->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("cube_upload Map", hr);
        }
        auto* dst_base = static_cast<uint8_t*>(mapped);
        std::vector<uint16_t> face_fp16;
        for (uint32_t f = 0; f < kCubeFaceCount; ++f) {
            GenerateHdrCubeFaceFp16(f, kCubeFaceSize, face_fp16);
            for (uint32_t row = 0; row < kCubeFaceSize; ++row) {
                std::memcpy(
                    dst_base + cube_fps[f].Offset +
                        static_cast<size_t>(row) * cube_fps[f].Footprint.RowPitch,
                    face_fp16.data() + static_cast<size_t>(row) * kCubeFaceSize * 4u,
                    static_cast<size_t>(kCubeFaceSize) * 8u);
            }
        }
        cube_upload->Unmap(0, nullptr);
    }

    // 8. Shader-visible SRV Descriptor Heap:
    //    Slot 0 (t0): Explicit TextureCube SRV over cubemap_tex
    //    Slot 1 (t1): DEFAULT (pDesc == nullptr) SRV over cubemap_tex (ScarlettX #46 Texture2DArray!)
    //    Slot 2 (t0 in HDR10 pass): Texture2D SRV over hdr_fp16_rt
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.NumDescriptors = 3;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    GuestComPtr<ID3D12DescriptorHeap> srv_heap;
    hr = device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(srv_heap.Put()));
    if (FAILED(hr) || !srv_heap) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateDescriptorHeap(SRV)", hr);
    }

    const UINT srv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_1{srv_cpu_0.ptr + srv_inc};
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_2{srv_cpu_0.ptr + srv_inc * 2u};

    D3D12_SHADER_RESOURCE_VIEW_DESC cube_srv_desc{};
    cube_srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cube_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    cube_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cube_srv_desc.TextureCube.MostDetailedMip = 0u;
    cube_srv_desc.TextureCube.MipLevels = 1u;
    cube_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(cubemap_tex.Get(), &cube_srv_desc, srv_cpu_0);

    // Deliberately pass pDesc == nullptr to exercise ScarlettX #46 default Texture2DArray SRV preservation!
    device->CreateShaderResourceView(cubemap_tex.Get(), nullptr, srv_cpu_1);

    D3D12_SHADER_RESOURCE_VIEW_DESC fp16_srv_desc{};
    fp16_srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    fp16_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fp16_srv_desc.Texture2D.MipLevels = 1u;
    device->CreateShaderResourceView(hdr_fp16_rt.Get(), &fp16_srv_desc, srv_cpu_2);

    // 9. Root Signatures & Graphics PSOs (Skybox+Strips, Reflective 3D Sphere, HDR10 PQ Encode)
    GuestComPtr<ID3D12RootSignature> skybox_rs;
    GuestComPtr<ID3D12RootSignature> sphere_rs;
    GuestComPtr<ID3D12RootSignature> hdr10_rs;
    if (FAILED(device->CreateRootSignature(0, skybox_vs_blob.data(), skybox_vs_blob.size(), IID_PPV_ARGS(skybox_rs.Put()))) ||
        FAILED(device->CreateRootSignature(0, sphere_vs_blob.data(), sphere_vs_blob.size(), IID_PPV_ARGS(sphere_rs.Put()))) ||
        FAILED(device->CreateRootSignature(0, hdr10_vs_blob.data(), hdr10_vs_blob.size(), IID_PPV_ARGS(hdr10_rs.Put())))) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateRootSignature");
    }

    auto make_base_pso_desc = [](ID3D12RootSignature* rs, DXGI_FORMAT rt_fmt) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rs;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.FrontCounterClockwise = FALSE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.BlendState.RenderTarget[0].BlendEnable = FALSE;
        d.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        d.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        d.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        d.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        d.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        d.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.StencilEnable = FALSE;
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rt_fmt;
        d.SampleDesc.Count = 1;
        return d;
    };

    GuestComPtr<ID3D12PipelineState> skybox_pso;
    {
        auto pso_desc = make_base_pso_desc(skybox_rs.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        pso_desc.VS = {skybox_vs_blob.data(), skybox_vs_blob.size()};
        pso_desc.PS = {skybox_ps_blob.data(), skybox_ps_blob.size()};
        hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(skybox_pso.Put()));
        if (FAILED(hr) || !skybox_pso) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("CreateGraphicsPipelineState(Skybox)", hr);
        }
    }

    static const D3D12_INPUT_ELEMENT_DESC kSphereInputs[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    GuestComPtr<ID3D12PipelineState> sphere_pso;
    {
        auto pso_desc = make_base_pso_desc(sphere_rs.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        pso_desc.InputLayout = {kSphereInputs, 2};
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pso_desc.VS = {sphere_vs_blob.data(), sphere_vs_blob.size()};
        pso_desc.PS = {sphere_ps_blob.data(), sphere_ps_blob.size()};
        hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(sphere_pso.Put()));
        if (FAILED(hr) || !sphere_pso) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("CreateGraphicsPipelineState(Sphere)", hr);
        }
    }

    GuestComPtr<ID3D12PipelineState> hdr10_pso;
    {
        auto pso_desc = make_base_pso_desc(hdr10_rs.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
        pso_desc.VS = {hdr10_vs_blob.data(), hdr10_vs_blob.size()};
        pso_desc.PS = {hdr10_ps_blob.data(), hdr10_ps_blob.size()};
        hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(hdr10_pso.Put()));
        if (FAILED(hr) || !hdr10_pso) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("CreateGraphicsPipelineState(HDR10 PQ Encode)", hr);
        }
    }

    // 10. Reflective 3D UV-Sphere Geometry (VB + IB) & Constant Buffers
    std::vector<SphereVertex> sphere_verts;
    std::vector<uint32_t> sphere_indices;
    BuildUvSphere(sphere_verts, sphere_indices, 0.88f, 32u, 20u);

    const uint64_t vb_bytes = sphere_verts.size() * sizeof(SphereVertex);
    const uint64_t ib_bytes = sphere_indices.size() * sizeof(uint32_t);
    const D3D12_RESOURCE_DESC vb_desc = MakeBufferDesc(vb_bytes);
    const D3D12_RESOURCE_DESC ib_desc = MakeBufferDesc(ib_bytes);

    GuestComPtr<ID3D12Resource> vb;
    GuestComPtr<ID3D12Resource> ib;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vb.Put()))) ||
        FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ib.Put())))) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(VB/IB)");
    }

    {
        void* p = nullptr;
        D3D12_RANGE nr{0, 0};
        if (FAILED(vb->Map(0, &nr, &p)) || !p) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("VB Map");
        }
        std::memcpy(p, sphere_verts.data(), static_cast<size_t>(vb_bytes));
        vb->Unmap(0, nullptr);

        p = nullptr;
        if (FAILED(ib->Map(0, &nr, &p)) || !p) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("IB Map");
        }
        std::memcpy(p, sphere_indices.data(), static_cast<size_t>(ib_bytes));
        ib->Unmap(0, nullptr);
    }

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(SphereVertex);
    vbv.SizeInBytes = static_cast<UINT>(vb_bytes);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = ib->GetGPUVirtualAddress();
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = static_cast<UINT>(ib_bytes);

    const uint64_t cb_total_bytes =
        static_cast<uint64_t>(kFrameCount) * sizeof(SceneConstants) + sizeof(Hdr10EncodeConstants);
    const D3D12_RESOURCE_DESC cb_desc = MakeBufferDesc(cb_total_bytes);
    GuestComPtr<ID3D12Resource> cb_res;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cb_res.Put()));
    if (FAILED(hr) || !cb_res) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        PIXEndEventOnCpu();
        cleanup_gdk();
        return Fail("CreateCommittedResource(CB)", hr);
    }

    uint8_t* cb_mapped = nullptr;
    {
        void* p = nullptr;
        D3D12_RANGE nr{0, 0};
        hr = cb_res->Map(0, &nr, &p);
        if (FAILED(hr) || !p) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            PIXEndEventOnCpu();
            cleanup_gdk();
            return Fail("CB Map", hr);
        }
        cb_mapped = static_cast<uint8_t*>(p);
    }

    Hdr10EncodeConstants hdr10_cb{};
    hdr10_cb.linear_to_nits = 80.0f;
    hdr10_cb.max_display_nits = 1000.0f;
    const uint64_t hdr10_cb_offset = static_cast<uint64_t>(kFrameCount) * sizeof(SceneConstants);
    std::memcpy(cb_mapped + hdr10_cb_offset, &hdr10_cb, sizeof(hdr10_cb));

    PIXEndEventOnCpu();

    // 11. Render 2 frames: FP16 HDR Skybox + 3D Reflective Sphere + 6-Face Strips -> HDR10 ST.2084 PQ -> PresentX
    LARGE_INTEGER qpc_freq{};
    ::QueryPerformanceFrequency(&qpc_freq);

    const float fov_y = 50.0f * kPi / 180.0f;
    const float scene_aspect = static_cast<float>(kWidth) / static_cast<float>(kSceneHeight);
    const Mat4 proj = MakePerspectiveFovRH(fov_y, scene_aspect, 0.2f, 20.0f);
    const Mat4 world_id = MakeIdentity();

    uint64_t frame_hashes[kFrameCount]{};
    double frame_ms[kFrameCount]{};
    uint32_t last_out_w = 0u;
    uint32_t last_out_h = 0u;

    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
        hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
        if (FAILED(hr) || frame_token == 0) {
            cb_res->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("WaitFrameEventX", hr);
        }

        const float yaw = static_cast<float>(frame) * 0.28f;
        const Vec3 cam_pos{2.85f * std::sin(yaw), 0.18f, 2.85f * std::cos(yaw)};
        Vec3 cam_right{};
        Vec3 cam_up{};
        Vec3 cam_fwd{};
        const Mat4 view = MakeLookAtRH(cam_pos, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                       cam_right, cam_up, cam_fwd);
        const Mat4 view_proj = MultiplyMat4(view, proj);

        SceneConstants sc{};
        std::memcpy(sc.world, world_id.m, sizeof(sc.world));
        std::memcpy(sc.view_proj, view_proj.m, sizeof(sc.view_proj));
        sc.camera_pos[0] = cam_pos.x;
        sc.camera_pos[1] = cam_pos.y;
        sc.camera_pos[2] = cam_pos.z;
        sc.camera_pos[3] = 1.0f;
        sc.cam_right[0] = cam_right.x;
        sc.cam_right[1] = cam_right.y;
        sc.cam_right[2] = cam_right.z;
        sc.cam_right[3] = scene_aspect;
        sc.cam_up[0] = cam_up.x;
        sc.cam_up[1] = cam_up.y;
        sc.cam_up[2] = cam_up.z;
        sc.cam_up[3] = 0.0f;
        sc.cam_forward[0] = cam_fwd.x;
        sc.cam_forward[1] = cam_fwd.y;
        sc.cam_forward[2] = cam_fwd.z;
        sc.cam_forward[3] = 0.0f;
        sc.viewport_params[0] = static_cast<float>(kWidth);
        sc.viewport_params[1] = static_cast<float>(kHeight);
        sc.viewport_params[2] = static_cast<float>(kSceneHeight);
        sc.viewport_params[3] = std::tan(fov_y * 0.5f);
        std::memcpy(cb_mapped + static_cast<size_t>(frame) * sizeof(SceneConstants), &sc, sizeof(sc));

        LARGE_INTEGER t0{};
        LARGE_INTEGER t1{};
        ::QueryPerformanceCounter(&t0);

        if (frame > 0u) {
            if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc.Get(), nullptr))) {
                cb_res->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                if (hwnd) ::DestroyWindow(hwnd);
                cleanup_gdk();
                return Fail("CommandAllocator/CommandList Reset");
            }
        }

        char frame_label[48]{};
        std::snprintf(frame_label, sizeof(frame_label), "HDRCubeMap::Frame%u", frame);
        PIXBeginEventOnCommandList(cmd.Get(), 0xFFFF8822ull, frame_label);

        if (frame == 0u) {
            PIXSetMarkerOnCommandList(cmd.Get(), 0xFF44DDFFull, "Upload6FaceHdrFp16CubeMap");
            for (uint32_t f = 0; f < kCubeFaceCount; ++f) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = cubemap_tex.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = f;

                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = cube_upload.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = cube_fps[f];
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            const D3D12_RESOURCE_BARRIER cube_to_srv = MakeTransition(
                cubemap_tex.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &cube_to_srv);
        } else {
            const D3D12_RESOURCE_BARRIER fp16_to_rt = MakeTransition(
                hdr_fp16_rt.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &fp16_to_rt);
        }

        // Pass 1: Render Skybox + 6-face Cube/Default-Array Strips + 3D Reflective Sphere into FP16 HDR RT
        cmd->OMSetRenderTargets(1, &fp16_rtv, FALSE, nullptr);
        cmd->ClearRenderTargetView(fp16_rtv, fp16_clear.Color, 0, nullptr);

        D3D12_VIEWPORT full_vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
        D3D12_RECT full_sc{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        cmd->RSSetViewports(1, &full_vp);
        cmd->RSSetScissorRects(1, &full_sc);

        ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);

        const D3D12_GPU_VIRTUAL_ADDRESS scene_cb_va =
            cb_res->GetGPUVirtualAddress() + static_cast<uint64_t>(frame) * sizeof(SceneConstants);
        const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_base = srv_heap->GetGPUDescriptorHandleForHeapStart();

        // 1A: Fullscreen Skybox + 6-face TextureCube strip + 6-slice Default Texture2DArray strip
        cmd->SetGraphicsRootSignature(skybox_rs.Get());
        cmd->SetPipelineState(skybox_pso.Get());
        cmd->SetGraphicsRootConstantBufferView(0, scene_cb_va);
        cmd->SetGraphicsRootDescriptorTable(1, srv_gpu_base);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);

        // 1B: 3D Reflective Chrome Sphere in upper 256x192 viewport
        D3D12_VIEWPORT scene_vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kSceneHeight), 0.0f, 1.0f};
        D3D12_RECT scene_sc{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kSceneHeight)};
        cmd->RSSetViewports(1, &scene_vp);
        cmd->RSSetScissorRects(1, &scene_sc);

        cmd->SetGraphicsRootSignature(sphere_rs.Get());
        cmd->SetPipelineState(sphere_pso.Get());
        cmd->SetGraphicsRootConstantBufferView(0, scene_cb_va);
        cmd->SetGraphicsRootDescriptorTable(1, srv_gpu_base);
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->DrawIndexedInstanced(static_cast<UINT>(sphere_indices.size()), 1, 0, 0, 0);

        // Pass 2: HDR10 ST.2084 PQ Encode Pass (FP16 linear -> BT.2020 PQ R10G10B10A2_UNORM)
        const D3D12_RESOURCE_BARRIER to_hdr10[2] = {
            MakeTransition(hdr_fp16_rt.Get(),
                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            MakeTransition(hdr10_rt.Get(),
                           D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(2, to_hdr10);

        cmd->OMSetRenderTargets(1, &hdr10_rtv, FALSE, nullptr);
        cmd->RSSetViewports(1, &full_vp);
        cmd->RSSetScissorRects(1, &full_sc);
        cmd->SetGraphicsRootSignature(hdr10_rs.Get());
        cmd->SetPipelineState(hdr10_pso.Get());
        cmd->SetGraphicsRootConstantBufferView(0, cb_res->GetGPUVirtualAddress() + hdr10_cb_offset);
        const D3D12_GPU_DESCRIPTOR_HANDLE fp16_srv_gpu{srv_gpu_base.ptr + srv_inc * 2u};
        cmd->SetGraphicsRootDescriptorTable(1, fp16_srv_gpu);
        cmd->DrawInstanced(3, 1, 0, 0);

        const D3D12_RESOURCE_BARRIER hdr10_to_present = MakeTransition(
            hdr10_rt.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1, &hdr10_to_present);

        PIXEndEventOnCommandList(cmd.Get());

        hr = cmd->Close();
        if (FAILED(hr)) {
            cb_res->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("CommandListX::Close", hr);
        }

        ID3D12CommandList* cmd_lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, cmd_lists);

        ID3D12Resource* plane_res[] = {hdr10_rt.Get()};
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = frame_token;
        plane.ResourceCount = 1;
        plane.ppResources = plane_res;
        plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

        D3D12XBOX_PRESENT_PARAMETERS pres_params{};
        pres_params.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
        pres_params.SyncInterval = 0;
        pres_params.OutputWindow = hwnd;

        hr = queue->PresentX(1, &plane, &pres_params);
        if (FAILED(hr)) {
            cb_res->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("PresentX(HDR10)", hr);
        }

        ++fence_val;
        hr = queue->Signal(fence.Get(), fence_val);
        if (FAILED(hr)) {
            cb_res->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("CommandQueueX::Signal", hr);
        }
        if (fence->GetCompletedValue() < fence_val) {
            if (FAILED(fence->SetEventOnCompletion(fence_val, fence_event)) ||
                ::WaitForSingleObject(fence_event, 15000u) != WAIT_OBJECT_0) {
                cb_res->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                if (hwnd) ::DestroyWindow(hwnd);
                cleanup_gdk();
                return Fail("Fence wait failed");
            }
            PIXNotifyWakeFromFenceSignal(fence_event);
        }

        ::QueryPerformanceCounter(&t1);
        frame_ms[frame] = QpcMs(t0, t1, qpc_freq);
        PIXReportCounter(L"HDRCubeMap.FrameMs", static_cast<float>(frame_ms[frame]));

        hr = OpenScarlettD3D12GetLastPresentHash(
            queue.Get(), &frame_hashes[frame], &last_out_w, &last_out_h);
        if (FAILED(hr) || last_out_w != kWidth || last_out_h != kHeight || frame_hashes[frame] == 0ull) {
            cb_res->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
        }
    }

    cb_res->Unmap(0, nullptr);

    if (frame_hashes[0] == frame_hashes[1]) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("Multi-frame FNV-1a hashes are identical");
    }

    // 12. Verify HDR Swapchain State
    D3D12XBOX_HDR_SWAPCHAIN_STATE hdr_state{};
    hr = OpenScarlettD3D12GetHdrSwapChainState(queue.Get(), &hdr_state);
    if (FAILED(hr) ||
        hdr_state.LastPresentedSourceFormat != DXGI_FORMAT_R10G10B10A2_UNORM ||
        hdr_state.LastPresentedColorSpace != D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        hdr_state.ActiveMetaData.MaxMasteringLuminance < 1000u ||
        hdr_state.ActiveMetaData.MaxContentLightLevel != 1000u ||
        hdr_state.ActiveMetaData.MaxFrameAverageLightLevel != 400u) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("HDR swapchain state mismatch", hr);
    }
    if (!hdr_enabled &&
        (hdr_state.ToneMapFallbackActive != 1u || hdr_state.ToneMapMode != D3D12XBOX_TONEMAP_REINHARD)) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("SDR fallback tone-map state mismatch");
    }

    // 13. Read back RAW HDR10 ST.2084 R10G10B10A2_UNORM pixels & verify nits + 6 cube/array faces
    std::vector<uint8_t> raw_hdr10(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t raw_copied = 0u;
    DXGI_FORMAT raw_fmt = DXGI_FORMAT_UNKNOWN;
    hr = OpenScarlettD3D12CopyLastPresentRawPixels(
        queue.Get(), raw_hdr10.data(), static_cast<uint32_t>(raw_hdr10.size()), &raw_copied, &raw_fmt);
    if (FAILED(hr) || raw_copied != raw_hdr10.size() || raw_fmt != DXGI_FORMAT_R10G10B10A2_UNORM) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("OpenScarlettD3D12CopyLastPresentRawPixels", hr);
    }

    float scene_peak_nits = 0.0f;
    float scene_min_nits = 100000.0f;
    float overall_peak_nits = 0.0f;
    uint32_t hdr_bright_pixels = 0u;
    uint32_t hdr_dark_pixels = 0u;

    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const DecodedNitsRgb px = DecodeHdr10PixelAt(raw_hdr10, x, y);
            if (!std::isfinite(px.r) || !std::isfinite(px.g) || !std::isfinite(px.b) ||
                px.r < 0.0f || px.g < 0.0f || px.b < 0.0f) {
                ::CloseHandle(fence_event);
                if (hwnd) ::DestroyWindow(hwnd);
                cleanup_gdk();
                return Fail("Invalid/NaN decoded HDR10 pixel");
            }
            if (px.peak > overall_peak_nits) overall_peak_nits = px.peak;
            if (y < kSceneHeight) {
                if (px.peak > scene_peak_nits) scene_peak_nits = px.peak;
                if (px.luma < scene_min_nits) scene_min_nits = px.luma;
                if (px.peak > 250.0f) ++hdr_bright_pixels;
                if (px.luma < 50.0f) ++hdr_dark_pixels;
            }
        }
    }

    if (scene_peak_nits <= 300.0f || scene_min_nits >= 50.0f ||
        hdr_bright_pixels < 100u || hdr_dark_pixels < 200u) {
        std::fprintf(stderr,
                     "[gdk_hdr_cubemap] HDR10 range check failed: peak=%.1f min=%.1f bright=%u dark=%u\n",
                     scene_peak_nits, scene_min_nits, hdr_bright_pixels, hdr_dark_pixels);
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("HDR10 dynamic range check failed");
    }

    // Verify all 6 TextureCube faces (Row A: y=192..223) AND all 6 Default-SRV Texture2DArray slices (Row B: y=224..255)
    uint32_t cubemap_faces_ok = 0u;
    uint32_t default_array_slices_ok = 0u;
    float cube_vs_array_max_diff = 0.0f;
    float face_peak_nits[kCubeFaceCount]{};
    DecodedNitsRgb array_base_samples[kCubeFaceCount]{};

    const float panel_w = static_cast<float>(kWidth) / static_cast<float>(kCubeFaceCount);
    for (uint32_t f = 0; f < kCubeFaceCount; ++f) {
        const uint32_t x_base = static_cast<uint32_t>((static_cast<float>(f) + 0.22f) * panel_w);
        const uint32_t x_center = static_cast<uint32_t>((static_cast<float>(f) + 0.50f) * panel_w);

        const uint32_t y_cube_base = 192u + 7u;
        const uint32_t y_cube_center = 192u + 16u;
        const uint32_t y_arr_base = 224u + 7u;
        const uint32_t y_arr_center = 224u + 16u;

        const DecodedNitsRgb cube_base = DecodeHdr10PixelAt(raw_hdr10, x_base, y_cube_base);
        const DecodedNitsRgb cube_center = DecodeHdr10PixelAt(raw_hdr10, x_center, y_cube_center);
        const DecodedNitsRgb arr_base = DecodeHdr10PixelAt(raw_hdr10, x_base, y_arr_base);
        const DecodedNitsRgb arr_center = DecodeHdr10PixelAt(raw_hdr10, x_center, y_arr_center);

        face_peak_nits[f] = cube_center.peak;
        array_base_samples[f] = arr_base;

        if (VerifyFaceColorSignature(f, cube_base, cube_center)) {
            ++cubemap_faces_ok;
        } else {
            std::fprintf(stderr,
                         "[gdk_hdr_cubemap] TextureCube face %u mismatch: base=(%.1f,%.1f,%.1f) centerPeak=%.1f\n",
                         f, cube_base.r, cube_base.g, cube_base.b, cube_center.peak);
        }

        if (VerifyFaceColorSignature(f, arr_base, arr_center)) {
            ++default_array_slices_ok;
        } else {
            std::fprintf(stderr,
                         "[gdk_hdr_cubemap] Default Texture2DArray slice %u mismatch: base=(%.1f,%.1f,%.1f) centerPeak=%.1f\n",
                         f, arr_base.r, arr_base.g, arr_base.b, arr_center.peak);
        }

        const float dr = std::fabs(cube_base.r - arr_base.r);
        const float dg = std::fabs(cube_base.g - arr_base.g);
        const float db = std::fabs(cube_base.b - arr_base.b);
        const float d_base = std::max(dr, std::max(dg, db));
        const float d_center = std::fabs(cube_center.peak - arr_center.peak);
        cube_vs_array_max_diff = std::max(cube_vs_array_max_diff, std::max(d_base, d_center));

        const float base_tol = std::max(12.0f, 0.10f * std::max(cube_base.peak, arr_base.peak));
        const float center_tol = std::max(20.0f, 0.10f * std::max(cube_center.peak, arr_center.peak));
        if (d_base > base_tol || d_center > center_tol) {
            std::fprintf(stderr,
                         "[gdk_hdr_cubemap] Face %u Cube vs DefaultArray nit diff too large: base=%.1f center=%.1f\n",
                         f, d_base, d_center);
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("TextureCube vs Default Texture2DArray mismatch", f);
        }
    }

    // Confirm slices 1..5 in Default Texture2DArray are all distinctly different from slice 0
    for (uint32_t f = 1u; f < kCubeFaceCount; ++f) {
        const float dist0 =
            std::fabs(array_base_samples[f].r - array_base_samples[0].r) +
            std::fabs(array_base_samples[f].g - array_base_samples[0].g) +
            std::fabs(array_base_samples[f].b - array_base_samples[0].b);
        if (dist0 < 60.0f) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("Default Texture2DArray slice collapsed to slice 0", f);
        }
    }

    if (cubemap_faces_ok != kCubeFaceCount || default_array_slices_ok != kCubeFaceCount) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("6-face Cube/Array verification incomplete", cubemap_faces_ok * 10 + default_array_slices_ok);
    }

    // 14. Read back automatic SDR tone-mapped RGBA8 pixels & write T:\hdr_cubemap_sdr.ppm
    std::vector<uint8_t> sdr_rgba(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t sdr_copied = 0u;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), sdr_rgba.data(), static_cast<uint32_t>(sdr_rgba.size()), &sdr_copied);
    if (FAILED(hr) || sdr_copied != sdr_rgba.size()) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
    }

    std::unordered_set<uint32_t> unique_sdr_rgb;
    unique_sdr_rgb.reserve(16384);
    double luma_sum = 0.0;
    uint32_t sdr_bright = 0u;
    uint32_t sdr_dark = 0u;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = sdr_rgba[i * 4u + 0u];
        const uint8_t g = sdr_rgba[i * 4u + 1u];
        const uint8_t b = sdr_rgba[i * 4u + 2u];
        const uint8_t a = sdr_rgba[i * 4u + 3u];
        if (a != 255u) {
            ::CloseHandle(fence_event);
            if (hwnd) ::DestroyWindow(hwnd);
            cleanup_gdk();
            return Fail("SDR tone-mapped alpha != 255", a);
        }
        unique_sdr_rgb.insert((static_cast<uint32_t>(r) << 16) |
                              (static_cast<uint32_t>(g) << 8) |
                              static_cast<uint32_t>(b));
        const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        luma_sum += luma;
        if (luma >= 190.0) ++sdr_bright;
        if (luma <= 85.0) ++sdr_dark;
    }
    const double sdr_mean_luma = luma_sum / static_cast<double>(kWidth * kHeight);
    if (unique_sdr_rgb.size() < 500u || sdr_bright < 100u || sdr_dark < 500u) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("SDR tone-mapped dynamic range check failed",
                    static_cast<int64_t>(unique_sdr_rgb.size()));
    }

    if (!WritePpmW(L"T:\\hdr_cubemap_sdr.ppm", kWidth, kHeight, sdr_rgba.data())) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("Write T:\\hdr_cubemap_sdr.ppm", ::GetLastError());
    }

    const double gpu_total_ms = frame_ms[0] + frame_ms[1];
    char metrics_buf[2048]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=HDRCubeMap\n"
        "title_id=0x%08X\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "width=%u\n"
        "height=%u\n"
        "hdr_mode_enabled=%u\n"
        "hdr_mode_result=%u\n"
        "min_tone_map_nits=%.4f\n"
        "max_tone_map_nits=%.1f\n"
        "max_full_frame_nits=%.1f\n"
        "swapchain_format=%u\n"
        "has_swapchain4=%u\n"
        "color_space_applied=%u\n"
        "hdr_metadata_applied=%u\n"
        "tonemap_fallback_active=%u\n"
        "tonemap_mode=%u\n"
        "frame0_fnv1a=0x%016llX\n"
        "frame1_fnv1a=0x%016llX\n"
        "scene_peak_nits=%.1f\n"
        "scene_min_nits=%.2f\n"
        "overall_peak_nits=%.1f\n"
        "hdr_bright_pixels=%u\n"
        "hdr_dark_pixels=%u\n"
        "cubemap_faces_verified=%u\n"
        "default_array_slices_verified=%u\n"
        "cube_vs_array_max_nit_diff=%.2f\n"
        "face0_peak_nits=%.1f\n"
        "face1_peak_nits=%.1f\n"
        "face2_peak_nits=%.1f\n"
        "face3_peak_nits=%.1f\n"
        "face4_peak_nits=%.1f\n"
        "face5_peak_nits=%.1f\n"
        "sdr_unique_colors=%zu\n"
        "sdr_mean_luma=%.2f\n"
        "gpu_total_ms=%.3f\n",
        title_id,
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        last_out_w,
        last_out_h,
        hdr_enabled ? 1u : 0u,
        static_cast<unsigned>(hdr_result),
        display_hdr_info.minToneMapLuminance,
        display_hdr_info.maxToneMapLuminance,
        display_hdr_info.maxFullFrameToneMapLuminance,
        static_cast<unsigned>(hdr_state.SwapChainFormat),
        hdr_state.HasSwapChain4,
        hdr_state.ColorSpaceApplied,
        hdr_state.HdrMetaDataApplied,
        hdr_state.ToneMapFallbackActive,
        static_cast<unsigned>(hdr_state.ToneMapMode),
        static_cast<unsigned long long>(frame_hashes[0]),
        static_cast<unsigned long long>(frame_hashes[1]),
        scene_peak_nits,
        scene_min_nits,
        overall_peak_nits,
        hdr_bright_pixels,
        hdr_dark_pixels,
        cubemap_faces_ok,
        default_array_slices_ok,
        cube_vs_array_max_diff,
        face_peak_nits[0],
        face_peak_nits[1],
        face_peak_nits[2],
        face_peak_nits[3],
        face_peak_nits[4],
        face_peak_nits[5],
        unique_sdr_rgb.size(),
        sdr_mean_luma,
        gpu_total_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    // 15. Clean PresentX(0) blank teardown
    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        if (hwnd) ::DestroyWindow(hwnd);
        cleanup_gdk();
        return Fail("PresentX(0) teardown", hr);
    }

    std::printf(
        "[gdk_hdr_cubemap] PASS | TitleId=0x%08X | Adapter=%s (%s) | HDR=%u (max=%.0f nits) | "
        "SceneNits=[%.1f..%.1f] | Faces=%u/6 ArraySlices=%u/6 (diff=%.1f nits) | "
        "SDRColors=%zu | %.2f ms\n",
        title_id,
        gpu_info.AdapterName,
        use_warp ? "WARP" : "HW",
        hdr_enabled ? 1u : 0u,
        display_hdr_info.maxToneMapLuminance,
        scene_min_nits,
        scene_peak_nits,
        cubemap_faces_ok,
        default_array_slices_ok,
        cube_vs_array_max_diff,
        unique_sdr_rgb.size(),
        gpu_total_ms);
    std::fflush(stdout);

    // Clean teardown
    cb_res.Reset();
    ib.Reset();
    vb.Reset();
    hdr10_pso.Reset();
    sphere_pso.Reset();
    skybox_pso.Reset();
    hdr10_rs.Reset();
    sphere_rs.Reset();
    skybox_rs.Reset();
    srv_heap.Reset();
    cube_upload.Reset();
    cubemap_tex.Reset();
    rtv_heap.Reset();
    hdr10_rt.Reset();
    hdr_fp16_rt.Reset();
    ::CloseHandle(fence_event);
    fence.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    if (hwnd) ::DestroyWindow(hwnd);
    cleanup_gdk();
    return 0;
}
