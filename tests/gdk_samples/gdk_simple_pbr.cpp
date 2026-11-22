// OpenScarlett MIT Standalone Xbox-ABI Guest Sample (Wave 2):
// SimplePBR — Multi-frame 3D Cook-Torrance PBR workload with 4x MSAA render target
// (ALLOW_DCC), 4x MSAA D32_FLOAT depth buffer, procedural 256x256 albedo/normal/roughness-metallic
// textures (CopyTextureRegion + SRV descriptor table + static sampler), per-frame dynamic
// constant buffers, ResolveSubresource, orbiting camera/light animation, PIX markers,
// GameInput polling, and MicrosoftGame.Config TitleId verification.
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
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kMsaaSamples = 4u;
constexpr uint32_t kTexSize = 256u;
constexpr uint32_t kFrameCount = 3u;
constexpr uint32_t kObjectsPerFrame = 3u;
constexpr uint32_t kCbSlotsPerFrame = 4u;
constexpr uint32_t kExpectedTitleId = 0x5CA01001u;

constexpr uint8_t kBgR = 15u;
constexpr uint8_t kBgG = 20u;
constexpr uint8_t kBgB = 33u;
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

struct PbrVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};
static_assert(sizeof(PbrVertex) == 48, "PbrVertex must be 48 bytes");

struct alignas(256) PbrConstants {
    float world[16];
    float view_proj[16];
    float camera_pos[4];
    float light_dir[4];
    float light_color[4];
    float albedo_tint[4];
    float material_params[4];
    float pad[12];
};
static_assert(sizeof(PbrConstants) == 256, "PbrConstants must be 256-byte aligned");

struct SubmeshRange {
    uint32_t index_start = 0;
    uint32_t index_count = 0;
    int32_t base_vertex = 0;
};

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_simple_pbr] FAIL: %s (code=0x%llX)\n",
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

Mat4 MakeTranslation(float tx, float ty, float tz) noexcept {
    Mat4 r{};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][0] = tx;
    r.m[3][1] = ty;
    r.m[3][2] = tz;
    r.m[3][3] = 1.0f;
    return r;
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

SubmeshRange AppendUvSphere(std::vector<PbrVertex>& verts, std::vector<uint32_t>& indices,
                            float radius, uint32_t lon_segs, uint32_t lat_segs) {
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
            const float sin_theta = std::sin(theta);
            const float cos_theta = std::cos(theta);

            const float nx = sin_phi * cos_theta;
            const float ny = cos_phi;
            const float nz = sin_phi * sin_theta;

            PbrVertex vtx{};
            vtx.position[0] = nx * radius;
            vtx.position[1] = ny * radius;
            vtx.position[2] = nz * radius;
            vtx.normal[0] = nx;
            vtx.normal[1] = ny;
            vtx.normal[2] = nz;
            vtx.tangent[0] = -sin_theta;
            vtx.tangent[1] = 0.0f;
            vtx.tangent[2] = cos_theta;
            vtx.tangent[3] = 1.0f;
            vtx.uv[0] = u;
            vtx.uv[1] = v;
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

            // Outward CW winding in right-handed view space
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

SubmeshRange AppendBeveledPedestal(std::vector<PbrVertex>& verts, std::vector<uint32_t>& indices,
                                   float r_top, float r_outer, float y_bot, float y_bevel,
                                   float y_top, uint32_t radial_segs) {
    SubmeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    auto local_idx = [&]() -> uint32_t {
        return static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
    };

    // 1. Flat top disc
    const uint32_t top_center = local_idx();
    verts.push_back(PbrVertex{
        {0.0f, y_top, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f},
    });
    const uint32_t top_ring_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_top * ca, y_top, r_top * sa},
            {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f, 1.0f},
            {0.5f + 0.5f * ca, 0.5f + 0.5f * sa},
        });
    }
    for (uint32_t i = 0; i < radial_segs; ++i) {
        indices.push_back(top_center);
        indices.push_back(top_ring_start + i);
        indices.push_back(top_ring_start + i + 1u);
    }

    // 2. Angled upper bevel ring
    const float dr = r_outer - r_top;
    const float dy = y_top - y_bevel;
    const float bn_len = std::sqrt(dr * dr + dy * dy);
    const float bn_r = (bn_len > 1e-5f) ? (dy / bn_len) : 0.7071f;
    const float bn_y = (bn_len > 1e-5f) ? (dr / bn_len) : 0.7071f;

    const uint32_t bevel_in_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_top * ca, y_top, r_top * sa},
            {bn_r * ca, bn_y, bn_r * sa},
            {-sa, 0.0f, ca, 1.0f},
            {u * 2.0f, 0.0f},
        });
    }
    const uint32_t bevel_out_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_outer * ca, y_bevel, r_outer * sa},
            {bn_r * ca, bn_y, bn_r * sa},
            {-sa, 0.0f, ca, 1.0f},
            {u * 2.0f, 0.25f},
        });
    }
    for (uint32_t i = 0; i < radial_segs; ++i) {
        const uint32_t v00 = bevel_in_start + i;
        const uint32_t v10 = bevel_out_start + i;
        const uint32_t v01 = bevel_in_start + i + 1u;
        const uint32_t v11 = bevel_out_start + i + 1u;
        indices.push_back(v00);
        indices.push_back(v10);
        indices.push_back(v01);
        indices.push_back(v10);
        indices.push_back(v11);
        indices.push_back(v01);
    }

    // 3. Outer cylindrical wall
    const uint32_t wall_top_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_outer * ca, y_bevel, r_outer * sa},
            {ca, 0.0f, sa},
            {-sa, 0.0f, ca, 1.0f},
            {u * 2.0f, 0.25f},
        });
    }
    const uint32_t wall_bot_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_outer * ca, y_bot, r_outer * sa},
            {ca, 0.0f, sa},
            {-sa, 0.0f, ca, 1.0f},
            {u * 2.0f, 1.0f},
        });
    }
    for (uint32_t i = 0; i < radial_segs; ++i) {
        const uint32_t v00 = wall_top_start + i;
        const uint32_t v10 = wall_bot_start + i;
        const uint32_t v01 = wall_top_start + i + 1u;
        const uint32_t v11 = wall_bot_start + i + 1u;
        indices.push_back(v00);
        indices.push_back(v10);
        indices.push_back(v01);
        indices.push_back(v10);
        indices.push_back(v11);
        indices.push_back(v01);
    }

    // 4. Bottom cap
    const uint32_t bot_center = local_idx();
    verts.push_back(PbrVertex{
        {0.0f, y_bot, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f},
    });
    const uint32_t bot_ring_start = local_idx();
    for (uint32_t i = 0; i <= radial_segs; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segs);
        const float a = u * 2.0f * kPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        verts.push_back(PbrVertex{
            {r_outer * ca, y_bot, r_outer * sa},
            {0.0f, -1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f, 1.0f},
            {0.5f + 0.5f * ca, 0.5f + 0.5f * sa},
        });
    }
    for (uint32_t i = 0; i < radial_segs; ++i) {
        indices.push_back(bot_center);
        indices.push_back(bot_ring_start + i + 1u);
        indices.push_back(bot_ring_start + i);
    }

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

uint8_t FloatToByte(float v) noexcept {
    const float clamped = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

void GenerateProceduralPbrTextures(uint32_t size,
                                   std::vector<uint8_t>& out_albedo_rgba,
                                   std::vector<uint8_t>& out_normal_rm_rgba) {
    out_albedo_rgba.resize(static_cast<size_t>(size) * size * 4u);
    out_normal_rm_rgba.resize(static_cast<size_t>(size) * size * 4u);

    auto height_at = [size](uint32_t x, uint32_t y) -> float {
        const float u = static_cast<float>(x % size) / static_cast<float>(size);
        const float v = static_cast<float>(y % size) / static_cast<float>(size);
        const float brush = 0.04f * std::sin(v * 2.0f * kPi * 24.0f + 0.35f * std::sin(u * 2.0f * kPi * 6.0f));
        const float panel_u = std::abs(std::fmod(u * 4.0f, 1.0f) - 0.5f);
        const float panel_v = std::abs(std::fmod(v * 4.0f, 1.0f) - 0.5f);
        const float seam = (panel_u > 0.47f || panel_v > 0.47f) ? -0.18f : 0.0f;
        return 0.5f + brush + seam;
    };

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const size_t off = (static_cast<size_t>(y) * size + x) * 4u;
            const uint32_t cx = (x / 32u) & 1u;
            const uint32_t cy = (y / 32u) & 1u;
            const bool checker = ((cx ^ cy) != 0u);

            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            const float grain = 0.06f * std::sin(v * 2.0f * kPi * 32.0f) +
                                0.03f * std::cos((u * 8.0f + v * 4.0f) * 2.0f * kPi);

            // Albedo map: bright brushed metal with subtle checker tile variation
            const float base_lum = (checker ? 0.94f : 0.78f) + grain;
            out_albedo_rgba[off + 0u] = FloatToByte(base_lum);
            out_albedo_rgba[off + 1u] = FloatToByte(base_lum * 0.985f);
            out_albedo_rgba[off + 2u] = FloatToByte(base_lum * 0.965f);
            out_albedo_rgba[off + 3u] = 255u;

            // Normal + Roughness + Metallic map
            const float hL = height_at((x + size - 1u) % size, y);
            const float hR = height_at((x + 1u) % size, y);
            const float hD = height_at(x, (y + size - 1u) % size);
            const float hU = height_at(x, (y + 1u) % size);
            const float nx = std::max(-0.85f, std::min(0.85f, (hL - hR) * 2.2f));
            const float ny = std::max(-0.85f, std::min(0.85f, (hD - hU) * 2.2f));

            const float rough = (checker ? 0.34f : 0.56f) + 0.08f * std::sin(u * 2.0f * kPi * 8.0f);
            const float metal = checker ? 0.88f : 0.62f;

            out_normal_rm_rgba[off + 0u] = FloatToByte(nx * 0.5f + 0.5f);
            out_normal_rm_rgba[off + 1u] = FloatToByte(ny * 0.5f + 0.5f);
            out_normal_rm_rgba[off + 2u] = FloatToByte(rough);
            out_normal_rm_rgba[off + 3u] = FloatToByte(metal);
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

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        }
    }

    PIXBeginEventOnCpu(0xFF44AAFFull, "SimplePBR::Startup");

    // 1. GDK Game Runtime + TitleId (from MicrosoftGame.Config) + Scarlett DeviceType + GameInput
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }

    uint32_t title_id = 0;
    const XGameResult tid_hr = XGameGetXboxTitleId(&title_id);
    if (tid_hr != XGR_S_OK || title_id != kExpectedTitleId) {
        PIXEndEventOnCpu();
        return Fail("XGameGetXboxTitleId mismatch (expected 0x5CA01001)", title_id);
    }

    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        PIXEndEventOnCpu();
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    IGameInput* game_input = nullptr;
    {
        const int32_t gi_hr = GameInputCreate(&game_input);
        if (gi_hr != GAMEINPUT_S_OK || !game_input) {
            PIXEndEventOnCpu();
            return Fail("GameInputCreate", gi_hr);
        }
    }

    // 2. Create Xbox Scarlett D3D12X device
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.DisableGeometryShaderAllocations = 1;
    dev_params.DisableTessellationShaderAllocations = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if ((gpu_info.IsWarp != 0u) != use_warp) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("Adapter WARP mode mismatch", gpu_info.IsWarp);
    }

    // Verify 4x MSAA support on RGBA8 color & D32_FLOAT depth
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa_color{};
    msaa_color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaa_color.SampleCount = kMsaaSamples;
    hr = device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa_color, sizeof(msaa_color));
    if (FAILED(hr) || msaa_color.NumQualityLevels == 0u) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("4x MSAA unsupported for R8G8B8A8_UNORM", hr);
    }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa_depth_check{};
    msaa_depth_check.Format = DXGI_FORMAT_D32_FLOAT;
    msaa_depth_check.SampleCount = kMsaaSamples;
    hr = device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa_depth_check, sizeof(msaa_depth_check));
    if (FAILED(hr) || msaa_depth_check.NumQualityLevels == 0u) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("4x MSAA unsupported for D32_FLOAT", hr);
    }

    // 3. Load precompiled SM 6.5 (or SM 6.2 on WARP) Cook-Torrance PBR shaders from G:\.
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    const bool supports_sm65 =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
        sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

    std::vector<uint8_t> vs_blob;
    std::vector<uint8_t> ps_blob;
    const wchar_t* vs_path = supports_sm65 ? L"G:\\PbrVS.cso" : L"G:\\PbrVS_sm62.cso";
    const wchar_t* ps_path = supports_sm65 ? L"G:\\PbrPS.cso" : L"G:\\PbrPS_sm62.cso";
    if (!ReadWholeFileW(vs_path, vs_blob)) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("Read PbrVS CSO from G:\\", ::GetLastError());
    }
    if (!ReadWholeFileW(ps_path, ps_blob)) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("Read PbrPS CSO from G:\\", ::GetLastError());
    }

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("SetFrameIntervalX", hr);
    }

    // 4. CommandQueueX, CommandAllocator, CommandListX, Fence
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommandQueue", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommandList", hr);
    }

    GuestComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr) || !fence) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateEventW(fence)", ::GetLastError());
    }
    uint64_t fence_val = 0;

    // 5. Create 4x MSAA Color RT (with Xbox ALLOW_DCC), 4x MSAA D32_FLOAT Depth Buffer,
    //    and 1x Resolve / Present RT (with Xbox ALLOW_DISPLAY | ALLOW_DCC).
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

    D3D12_RESOURCE_DESC msaa_rt_desc{};
    msaa_rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    msaa_rt_desc.Width = kWidth;
    msaa_rt_desc.Height = kHeight;
    msaa_rt_desc.DepthOrArraySize = 1;
    msaa_rt_desc.MipLevels = 1;
    msaa_rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaa_rt_desc.SampleDesc.Count = kMsaaSamples;
    msaa_rt_desc.SampleDesc.Quality = 0;
    msaa_rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    msaa_rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> msaa_rt;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &msaa_rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &color_clear,
        IID_PPV_ARGS(msaa_rt.Put()));
    if (FAILED(hr) || !msaa_rt) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(4x MSAA RT | ALLOW_DCC)", hr);
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
    depth_desc.SampleDesc.Count = kMsaaSamples;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    GuestComPtr<ID3D12Resource> msaa_depth;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depth_clear,
        IID_PPV_ARGS(msaa_depth.Put()));
    if (FAILED(hr) || !msaa_depth) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(4x MSAA D32_FLOAT Depth)", hr);
    }

    D3D12_RESOURCE_DESC present_rt_desc{};
    present_rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    present_rt_desc.Width = kWidth;
    present_rt_desc.Height = kHeight;
    present_rt_desc.DepthOrArraySize = 1;
    present_rt_desc.MipLevels = 1;
    present_rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    present_rt_desc.SampleDesc.Count = 1;
    present_rt_desc.SampleDesc.Quality = 0;
    present_rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    present_rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> present_rt;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_ALLOW_DISPLAY,
        &present_rt_desc,
        D3D12_RESOURCE_STATE_PRESENT,
        &color_clear,
        IID_PPV_ARGS(present_rt.Put()));
    if (FAILED(hr) || !present_rt) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(1x Resolve Present RT)", hr);
    }

    // RTV Heap (1 slot for 4x MSAA color RT)
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(msaa_rt.Get(), &rtv_desc, rtv_handle);

    // DSV Heap (1 slot for 4x MSAA D32_FLOAT depth buffer)
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    GuestComPtr<ID3D12DescriptorHeap> dsv_heap;
    hr = device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(dsv_heap.Put()));
    if (FAILED(hr) || !dsv_heap) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(DSV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    device->CreateDepthStencilView(msaa_depth.Get(), &dsv_desc, dsv_handle);

    // 6. Create two 256x256 RGBA8 procedural PBR textures (t0=Albedo, t1=Normal/Roughness/Metallic)
    //    + shader-visible SRV descriptor heap + staging upload buffer
    std::vector<uint8_t> albedo_rgba;
    std::vector<uint8_t> normal_rm_rgba;
    GenerateProceduralPbrTextures(kTexSize, albedo_rgba, normal_rm_rgba);

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kTexSize;
    tex_desc.Height = kTexSize;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    GuestComPtr<ID3D12Resource> albedo_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(albedo_tex.Put()));
    if (FAILED(hr) || !albedo_tex) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(albedo_tex)", hr);
    }

    GuestComPtr<ID3D12Resource> normal_rm_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(normal_rm_tex.Put()));
    if (FAILED(hr) || !normal_rm_tex) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(normal_rm_tex)", hr);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_footprint{};
    UINT tex_num_rows = 0;
    UINT64 tex_row_bytes = 0;
    UINT64 tex_total_bytes = 0;
    device->GetCopyableFootprints(
        &tex_desc, 0, 1, 0, &tex_footprint, &tex_num_rows, &tex_row_bytes, &tex_total_bytes);

    const UINT64 tex_slot_stride = (tex_total_bytes + 511ull) & ~511ull;
    const D3D12_RESOURCE_DESC tex_up_desc = MakeBufferDesc(tex_slot_stride * 2ull);
    GuestComPtr<ID3D12Resource> tex_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &tex_up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(tex_upload.Put()));
    if (FAILED(hr) || !tex_upload) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(tex_upload)", hr);
    }

    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = tex_upload->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            game_input->Release();
            PIXEndEventOnCpu();
            return Fail("tex_upload Map", hr);
        }
        auto* dst_base = static_cast<uint8_t*>(mapped);
        for (uint32_t row = 0; row < kTexSize; ++row) {
            std::memcpy(dst_base + static_cast<size_t>(row) * tex_footprint.Footprint.RowPitch,
                        albedo_rgba.data() + static_cast<size_t>(row) * kTexSize * 4u,
                        static_cast<size_t>(kTexSize) * 4u);
            std::memcpy(dst_base + tex_slot_stride +
                            static_cast<size_t>(row) * tex_footprint.Footprint.RowPitch,
                        normal_rm_rgba.data() + static_cast<size_t>(row) * kTexSize * 4u,
                        static_cast<size_t>(kTexSize) * 4u);
        }
        tex_upload->Unmap(0, nullptr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.NumDescriptors = 2;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    GuestComPtr<ID3D12DescriptorHeap> srv_heap;
    hr = device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(srv_heap.Put()));
    if (FAILED(hr) || !srv_heap) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(SRV)", hr);
    }

    const UINT srv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_1{srv_cpu_0.ptr + srv_inc};

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(albedo_tex.Get(), &srv_desc, srv_cpu_0);
    device->CreateShaderResourceView(normal_rm_tex.Get(), &srv_desc, srv_cpu_1);

    // 7. Root Signature (embedded in PbrVS.cso) & 4x MSAA + D32_FLOAT Graphics PSO
    GuestComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, vs_blob.data(), vs_blob.size(), IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateRootSignature(PbrVS.cso)", hr);
    }

    static const D3D12_INPUT_ELEMENT_DESC kInputElements[4] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.InputLayout = {kInputElements, 4};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.VS = {vs_blob.data(), vs_blob.size()};
    pso_desc.PS = {ps_blob.data(), ps_blob.size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.MultisampleEnable = TRUE;
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
    pso_desc.SampleDesc.Count = kMsaaSamples;
    pso_desc.SampleDesc.Quality = 0;

    GuestComPtr<ID3D12PipelineState> pso;
    hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()));
    if (FAILED(hr) || !pso) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateGraphicsPipelineState(4x MSAA + D32_FLOAT)", hr);
    }

    // 8. Build 3D Scene Geometry:
    //    - Submesh 0: Front smaller occluding UV-sphere (drawn FIRST -> tests LESS depth buffer!)
    //    - Submesh 1: Main large UV-sphere (drawn SECOND -> occluded behind front sphere!)
    //    - Submesh 2: Beveled metallic pedestal beneath main sphere (drawn THIRD)
    std::vector<PbrVertex> vertices;
    std::vector<uint32_t> indices;
    const SubmeshRange front_sphere = AppendUvSphere(vertices, indices, 0.33f, 16u, 10u);
    const SubmeshRange main_sphere = AppendUvSphere(vertices, indices, 0.86f, 24u, 16u);
    const SubmeshRange pedestal = AppendBeveledPedestal(
        vertices, indices, 0.92f, 1.12f, -1.06f, -0.82f, -0.74f, 32u);

    const uint64_t vb_bytes = vertices.size() * sizeof(PbrVertex);
    const uint64_t ib_bytes = indices.size() * sizeof(uint32_t);
    const D3D12_RESOURCE_DESC vb_desc = MakeBufferDesc(vb_bytes);
    const D3D12_RESOURCE_DESC ib_desc = MakeBufferDesc(ib_bytes);

    GuestComPtr<ID3D12Resource> vb;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vb.Put()));
    if (FAILED(hr) || !vb) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(VB)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = vb->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            game_input->Release();
            PIXEndEventOnCpu();
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
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(IB)", hr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = ib->Map(0, &no_read, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            game_input->Release();
            PIXEndEventOnCpu();
            return Fail("IB Map", hr);
        }
        std::memcpy(mapped, indices.data(), static_cast<size_t>(ib_bytes));
        ib->Unmap(0, nullptr);
    }

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(PbrVertex);
    vbv.SizeInBytes = static_cast<UINT>(vb_bytes);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = ib->GetGPUVirtualAddress();
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = static_cast<UINT>(ib_bytes);

    // 9. Per-frame dynamic constant buffer (3 frames x 4 slots x 256 bytes = 3072 bytes)
    const uint64_t cb_total_bytes = static_cast<uint64_t>(kFrameCount) * kCbSlotsPerFrame * sizeof(PbrConstants);
    const D3D12_RESOURCE_DESC cb_desc = MakeBufferDesc(cb_total_bytes);
    GuestComPtr<ID3D12Resource> cb_resource;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cb_resource.Put()));
    if (FAILED(hr) || !cb_resource) {
        ::CloseHandle(fence_event);
        game_input->Release();
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(CB)", hr);
    }

    uint8_t* cb_mapped = nullptr;
    {
        void* ptr = nullptr;
        D3D12_RANGE no_read{0, 0};
        hr = cb_resource->Map(0, &no_read, &ptr);
        if (FAILED(hr) || !ptr) {
            ::CloseHandle(fence_event);
            game_input->Release();
            PIXEndEventOnCpu();
            return Fail("CB Map", hr);
        }
        cb_mapped = static_cast<uint8_t*>(ptr);
    }

    PIXEndEventOnCpu();

    // 10. Render 3 animated frames with camera & key-light orbit + MSAA resolve + PresentX
    LARGE_INTEGER qpc_freq{};
    ::QueryPerformanceFrequency(&qpc_freq);

    const Vec3 kFrontSpherePos{0.0f, 0.14f, 1.16f};
    const Vec3 kMainSpherePos{0.0f, 0.12f, 0.0f};
    const Vec3 kPedestalPos{0.0f, 0.0f, 0.0f};
    const Vec3 kLookTarget{0.0f, -0.05f, 0.0f};
    const Vec3 kUp{0.0f, 1.0f, 0.0f};
    const Mat4 proj = MakePerspectiveFovRH(45.0f * kPi / 180.0f, 1.0f, 0.2f, 20.0f);

    uint64_t frame_hashes[kFrameCount]{};
    double frame_ms[kFrameCount]{};
    Mat4 frame2_view_proj{};
    uint32_t last_out_w = 0;
    uint32_t last_out_h = 0;

    for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
        // Poll GameInput once per frame
        {
            IGameInputReading* reading = nullptr;
            if (game_input->GetCurrentReading(GameInputKindGamepad, nullptr, &reading) == GAMEINPUT_S_OK &&
                reading) {
                GameInputGamepadState gp{};
                (void)reading->GetGamepadState(&gp);
                reading->Release();
            }
        }

        D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
        hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
        if (FAILED(hr) || frame_token == 0) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            game_input->Release();
            return Fail("WaitFrameEventX", hr);
        }

        // Orbit camera and directional key light smoothly across frames 0, 1, 2
        const float cam_theta = (static_cast<float>(frame) - 1.0f) * 0.24f;
        const Vec3 cam_pos{
            3.42f * std::sin(cam_theta),
            0.68f + 0.06f * static_cast<float>(frame),
            3.42f * std::cos(cam_theta),
        };
        const Mat4 view = MakeLookAtRH(cam_pos, kLookTarget, kUp);
        const Mat4 view_proj = MultiplyMat4(view, proj);
        if (frame == kFrameCount - 1u) {
            frame2_view_proj = view_proj;
        }

        const float light_theta = 0.48f + static_cast<float>(frame) * 0.24f;
        const Vec3 light_dir = Normalize({std::sin(light_theta), 0.72f, std::cos(light_theta)});

        struct ObjectDraw {
            SubmeshRange range;
            Mat4 world;
            float tint[4];      // rgb + normal_strength
            float material[4];  // metallic, roughness, uv_scale, unused
        };
        const ObjectDraw draws[kObjectsPerFrame] = {
            // Draw 0: Front smaller occluding sphere (cool cobalt-blue) drawn FIRST!
            {
                front_sphere,
                MakeTranslation(kFrontSpherePos.x, kFrontSpherePos.y, kFrontSpherePos.z),
                {0.14f, 0.50f, 0.98f, 0.45f},
                {0.28f, 0.26f, 2.0f, 0.0f},
            },
            // Draw 1: Main back sphere (warm copper-gold) drawn SECOND -> occluded behind Draw 0!
            {
                main_sphere,
                MakeTranslation(kMainSpherePos.x, kMainSpherePos.y, kMainSpherePos.z),
                {0.98f, 0.56f, 0.14f, 0.55f},
                {0.55f, 0.24f, 3.0f, 0.0f},
            },
            // Draw 2: Brushed titanium-steel metallic pedestal drawn THIRD
            {
                pedestal,
                MakeTranslation(kPedestalPos.x, kPedestalPos.y, kPedestalPos.z),
                {0.76f, 0.79f, 0.85f, 0.50f},
                {0.65f, 0.30f, 2.0f, 0.0f},
            },
        };

        for (uint32_t o = 0; o < kObjectsPerFrame; ++o) {
            PbrConstants cb{};
            std::memcpy(cb.world, draws[o].world.m, sizeof(cb.world));
            std::memcpy(cb.view_proj, view_proj.m, sizeof(cb.view_proj));
            cb.camera_pos[0] = cam_pos.x;
            cb.camera_pos[1] = cam_pos.y;
            cb.camera_pos[2] = cam_pos.z;
            cb.camera_pos[3] = 1.0f;
            cb.light_dir[0] = light_dir.x;
            cb.light_dir[1] = light_dir.y;
            cb.light_dir[2] = light_dir.z;
            cb.light_dir[3] = 4.4f;  // key radiance intensity
            cb.light_color[0] = 1.0f;
            cb.light_color[1] = 0.97f;
            cb.light_color[2] = 0.92f;
            cb.light_color[3] = 0.032f;  // low ambient for true dark shadowed hemisphere
            std::memcpy(cb.albedo_tint, draws[o].tint, sizeof(cb.albedo_tint));
            std::memcpy(cb.material_params, draws[o].material, sizeof(cb.material_params));

            const size_t slot_offset =
                (static_cast<size_t>(frame) * kCbSlotsPerFrame + o) * sizeof(PbrConstants);
            std::memcpy(cb_mapped + slot_offset, &cb, sizeof(PbrConstants));
        }

        LARGE_INTEGER t0{};
        LARGE_INTEGER t1{};
        ::QueryPerformanceCounter(&t0);

        if (frame > 0u) {
            hr = alloc->Reset();
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                game_input->Release();
                return Fail("CommandAllocator::Reset", hr);
            }
            hr = cmd->Reset(alloc.Get(), nullptr);
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                game_input->Release();
                return Fail("CommandListX::Reset", hr);
            }
        }

        char frame_label[48]{};
        std::snprintf(frame_label, sizeof(frame_label), "SimplePBR::Frame%u", frame);
        PIXBeginEventOnCommandList(cmd.Get(), 0xFF22CC88ull, frame_label);

        // Frame 0 uploads both 256x256 procedural PBR textures via CopyTextureRegion
        if (frame == 0u) {
            PIXSetMarkerOnCommandList(cmd.Get(), 0xFFFFAA00ull, "UploadProceduralPbrTextures");

            D3D12_TEXTURE_COPY_LOCATION dst0{};
            dst0.pResource = albedo_tex.Get();
            dst0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst0.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src0{};
            src0.pResource = tex_upload.Get();
            src0.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src0.PlacedFootprint = tex_footprint;
            src0.PlacedFootprint.Offset = 0;
            cmd->CopyTextureRegion(&dst0, 0, 0, 0, &src0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst1{};
            dst1.pResource = normal_rm_tex.Get();
            dst1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst1.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src1{};
            src1.pResource = tex_upload.Get();
            src1.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src1.PlacedFootprint = tex_footprint;
            src1.PlacedFootprint.Offset = tex_slot_stride;
            cmd->CopyTextureRegion(&dst1, 0, 0, 0, &src1, nullptr);

            const D3D12_RESOURCE_BARRIER tex_barriers[2] = {
                MakeTransition(albedo_tex.Get(),
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                MakeTransition(normal_rm_tex.Get(),
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            cmd->ResourceBarrier(2, tex_barriers);
        } else {
            const D3D12_RESOURCE_BARRIER to_rt = MakeTransition(
                msaa_rt.Get(),
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &to_rt);
        }

        cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
        cmd->ClearRenderTargetView(rtv_handle, color_clear.Color, 0, nullptr);
        cmd->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scissor);

        cmd->SetGraphicsRootSignature(root_sig.Get());
        cmd->SetPipelineState(pso.Get());

        ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());

        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);

        const D3D12_GPU_VIRTUAL_ADDRESS cb_base_va = cb_resource->GetGPUVirtualAddress();
        for (uint32_t o = 0; o < kObjectsPerFrame; ++o) {
            const uint64_t slot_offset =
                (static_cast<uint64_t>(frame) * kCbSlotsPerFrame + o) * sizeof(PbrConstants);
            cmd->SetGraphicsRootConstantBufferView(0, cb_base_va + slot_offset);
            cmd->DrawIndexedInstanced(
                draws[o].range.index_count, 1,
                draws[o].range.index_start,
                draws[o].range.base_vertex, 0);
        }

        // Resolve 4x MSAA render target into 1x present target
        const D3D12_RESOURCE_BARRIER pre_resolve[2] = {
            MakeTransition(msaa_rt.Get(),
                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
            MakeTransition(present_rt.Get(),
                           D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_RESOLVE_DEST),
        };
        cmd->ResourceBarrier(2, pre_resolve);

        cmd->ResolveSubresource(
            present_rt.Get(), 0, msaa_rt.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);

        const D3D12_RESOURCE_BARRIER post_resolve = MakeTransition(
            present_rt.Get(),
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1, &post_resolve);

        PIXEndEventOnCommandList(cmd.Get());

        hr = cmd->Close();
        if (FAILED(hr)) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            game_input->Release();
            return Fail("CommandListX::Close", hr);
        }

        ID3D12CommandList* cmd_lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, cmd_lists);

        ID3D12Resource* plane_res[] = {present_rt.Get()};
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
            game_input->Release();
            return Fail("PresentX(1)", hr);
        }

        // Explicit GPU fence synchronization before next frame allocator reset
        ++fence_val;
        hr = queue->Signal(fence.Get(), fence_val);
        if (FAILED(hr)) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            game_input->Release();
            return Fail("CommandQueueX::Signal", hr);
        }
        if (fence->GetCompletedValue() < fence_val) {
            hr = fence->SetEventOnCompletion(fence_val, fence_event);
            if (FAILED(hr)) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                game_input->Release();
                return Fail("Fence::SetEventOnCompletion", hr);
            }
            const DWORD wr = ::WaitForSingleObject(fence_event, 15000u);
            if (wr != WAIT_OBJECT_0) {
                cb_resource->Unmap(0, nullptr);
                ::CloseHandle(fence_event);
                game_input->Release();
                return Fail("Fence wait timeout", wr);
            }
            PIXNotifyWakeFromFenceSignal(fence_event);
        }

        ::QueryPerformanceCounter(&t1);
        frame_ms[frame] = QpcMs(t0, t1, qpc_freq);
        PIXReportCounter(L"SimplePBR.FrameMs", static_cast<float>(frame_ms[frame]));

        hr = OpenScarlettD3D12GetLastPresentHash(
            queue.Get(), &frame_hashes[frame], &last_out_w, &last_out_h);
        if (FAILED(hr) || last_out_w != kWidth || last_out_h != kHeight || frame_hashes[frame] == 0ull) {
            cb_resource->Unmap(0, nullptr);
            ::CloseHandle(fence_event);
            game_input->Release();
            return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
        }
    }

    cb_resource->Unmap(0, nullptr);

    // 11. Verify all 3 frames have distinct non-zero hashes (proving animation & CB update)
    if (frame_hashes[0] == frame_hashes[1] ||
        frame_hashes[1] == frame_hashes[2] ||
        frame_hashes[0] == frame_hashes[2]) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Multi-frame FNV-1a hashes are not distinct");
    }

    // 12. Read back final frame (frame 2) pixels and verify PBR shading & depth occlusion
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied);
    if (FAILED(hr) || copied != pixels.size()) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
    }

    auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return &pixels[(static_cast<size_t>(y) * kWidth + x) * 4u];
    };

    // Corners must be exact background clear color
    const uint8_t* tl = pixel_at(0, 0);
    const uint8_t* tr = pixel_at(kWidth - 1u, 0);
    if (tl[0] != kBgR || tl[1] != kBgG || tl[2] != kBgB || tl[3] != kBgA ||
        tr[0] != kBgR || tr[1] != kBgG || tr[2] != kBgB || tr[3] != kBgA) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Background clear corner pixel mismatch");
    }

    // Depth occlusion check: project Front Occluding Sphere center onto frame 2.
    // Because Front Sphere (cobalt blue) was drawn FIRST and Main Sphere (copper gold)
    // was drawn SECOND, the projected front sphere center MUST be cool blue-dominant!
    int32_t front_px = 0;
    int32_t front_py = 0;
    if (!ProjectWorldToPixel(kFrontSpherePos, frame2_view_proj, kWidth, kHeight, front_px, front_py)) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Front sphere center projection out of viewport");
    }

    double front_sum_r = 0.0;
    double front_sum_g = 0.0;
    double front_sum_b = 0.0;
    uint32_t front_samples = 0;
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            const int32_t sx = std::max(0, std::min(static_cast<int32_t>(kWidth) - 1, front_px + dx));
            const int32_t sy = std::max(0, std::min(static_cast<int32_t>(kHeight) - 1, front_py + dy));
            const uint8_t* p = pixel_at(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
            front_sum_r += static_cast<double>(p[0]);
            front_sum_g += static_cast<double>(p[1]);
            front_sum_b += static_cast<double>(p[2]);
            ++front_samples;
        }
    }
    const double front_avg_r = front_sum_r / static_cast<double>(front_samples);
    const double front_avg_g = front_sum_g / static_cast<double>(front_samples);
    const double front_avg_b = front_sum_b / static_cast<double>(front_samples);
    if (!(front_avg_b > front_avg_r + 30.0 && front_avg_b > 80.0)) {
        std::fprintf(stderr,
                     "[gdk_simple_pbr] Depth occlusion check at (%d,%d) failed: avgRGB=(%.1f, %.1f, %.1f)\n",
                     front_px, front_py, front_avg_r, front_avg_g, front_avg_b);
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Depth occlusion check failed (front cobalt sphere overwritten by back gold sphere)");
    }

    // Verify visible lit flank of Main Back Sphere is warm copper-gold
    int32_t back_px = 0;
    int32_t back_py = 0;
    const Vec3 main_lit_pt{kMainSpherePos.x + 0.48f, kMainSpherePos.y + 0.28f, kMainSpherePos.z + 0.64f};
    if (!ProjectWorldToPixel(main_lit_pt, frame2_view_proj, kWidth, kHeight, back_px, back_py)) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Main sphere lit point projection out of viewport");
    }
    double back_sum_r = 0.0;
    double back_sum_b = 0.0;
    uint32_t back_samples = 0;
    for (int32_t dy = -3; dy <= 3; ++dy) {
        for (int32_t dx = -3; dx <= 3; ++dx) {
            const int32_t sx = std::max(0, std::min(static_cast<int32_t>(kWidth) - 1, back_px + dx));
            const int32_t sy = std::max(0, std::min(static_cast<int32_t>(kHeight) - 1, back_py + dy));
            const uint8_t* p = pixel_at(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
            back_sum_r += static_cast<double>(p[0]);
            back_sum_b += static_cast<double>(p[2]);
            ++back_samples;
        }
    }
    const double back_avg_r = back_sum_r / static_cast<double>(back_samples);
    const double back_avg_b = back_sum_b / static_cast<double>(back_samples);
    if (!(back_avg_r > back_avg_b + 25.0 && back_avg_r > 100.0)) {
        std::fprintf(stderr,
                     "[gdk_simple_pbr] Back gold sphere check at (%d,%d) failed: avgR=%.1f avgB=%.1f\n",
                     back_px, back_py, back_avg_r, back_avg_b);
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Main warm copper-gold sphere shading check failed");
    }

    // Full-frame pixel classification metrics
    uint32_t bg_pixels = 0;
    uint32_t non_bg_pixels = 0;
    uint32_t specular_pixels = 0;
    uint32_t diffuse_pixels = 0;
    uint32_t shadow_pixels = 0;
    uint32_t front_occluder_pixels = 0;
    uint32_t back_sphere_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(16384);

    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
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

        if (max_c >= 210u && luma >= 180u) {
            ++specular_pixels;
        } else if (luma >= 60u) {
            ++diffuse_pixels;
        } else if (bg_diff >= 12) {
            ++shadow_pixels;
        }

        if (static_cast<int32_t>(b) >= static_cast<int32_t>(r) + 25 && b >= 60u) {
            ++front_occluder_pixels;
        }
        if (static_cast<int32_t>(r) >= static_cast<int32_t>(b) + 30 && r >= 55u) {
            ++back_sphere_pixels;
        }
    }

    if (bg_pixels < 10000u || non_bg_pixels < 12000u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Unexpected scene coverage", non_bg_pixels);
    }
    if (specular_pixels < 30u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Missing Cook-Torrance specular highlights", specular_pixels);
    }
    if (diffuse_pixels < 2500u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Insufficient diffuse PBR shaded pixels", diffuse_pixels);
    }
    if (shadow_pixels < 400u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Insufficient dark shadowed hemisphere pixels", shadow_pixels);
    }
    if (front_occluder_pixels < 400u || back_sphere_pixels < 2500u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Insufficient front/back sphere pixel coverage", front_occluder_pixels);
    }
    if (unique_rgb.size() < 2000u) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Too few unique PBR shaded colors", static_cast<int64_t>(unique_rgb.size()));
    }

    // 13. Write T:\simple_pbr_frame2.ppm and T:\metrics.txt
    if (!WritePpmW(L"T:\\simple_pbr_frame2.ppm", kWidth, kHeight, pixels.data())) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Write T:\\simple_pbr_frame2.ppm", ::GetLastError());
    }

    const double gpu_total_ms = frame_ms[0] + frame_ms[1] + frame_ms[2];
    char metrics_buf[1024]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=SimplePBR\n"
        "title_id=0x%08X\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "width=%u\n"
        "height=%u\n"
        "msaa_samples=%u\n"
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
        "front_occluder_pixels=%u\n"
        "back_sphere_pixels=%u\n"
        "unique_colors=%zu\n"
        "depth_occlusion_verified=1\n"
        "gpu_frame0_ms=%.3f\n"
        "gpu_frame1_ms=%.3f\n"
        "gpu_frame2_ms=%.3f\n"
        "gpu_total_ms=%.3f\n",
        title_id,
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        last_out_w,
        last_out_h,
        kMsaaSamples,
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
        front_occluder_pixels,
        back_sphere_pixels,
        unique_rgb.size(),
        frame_ms[0],
        frame_ms[1],
        frame_ms[2],
        gpu_total_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    // 14. Clean PresentX(0) blank teardown (Microsoft DeviceResources pattern)
    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        game_input->Release();
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_simple_pbr] PASS | TitleId=0x%08X | Adapter=%s (%s) | %ux%u %uxMSAA | "
        "Tris=%zu | Spec=%u Diff=%u Shad=%u | H0=0x%016llX H1=0x%016llX H2=0x%016llX | %.2f ms\n",
        title_id,
        gpu_info.AdapterName,
        use_warp ? "WARP" : "HW",
        last_out_w,
        last_out_h,
        kMsaaSamples,
        indices.size() / 3u,
        specular_pixels,
        diffuse_pixels,
        shadow_pixels,
        static_cast<unsigned long long>(frame_hashes[0]),
        static_cast<unsigned long long>(frame_hashes[1]),
        static_cast<unsigned long long>(frame_hashes[2]),
        gpu_total_ms);
    std::fflush(stdout);

    // Clean teardown
    cb_resource.Reset();
    ib.Reset();
    vb.Reset();
    pso.Reset();
    root_sig.Reset();
    srv_heap.Reset();
    tex_upload.Reset();
    normal_rm_tex.Reset();
    albedo_tex.Reset();
    dsv_heap.Reset();
    rtv_heap.Reset();
    present_rt.Reset();
    msaa_depth.Reset();
    msaa_rt.Reset();
    ::CloseHandle(fence_event);
    fence.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    game_input->Release();
    XGameRuntimeUninitialize();
    return 0;
}
