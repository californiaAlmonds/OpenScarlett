#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <numeric>
#include <string>

namespace openscarlett {

namespace {

inline Vec3 Add(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(Vec3 v) noexcept {
    const float len = std::sqrt(Dot(v, v));
    return len > 1e-8f ? Mul(v, 1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}
inline Vec3 Reflect(Vec3 dir, Vec3 n) noexcept {
    return Sub(dir, Mul(n, 2.0f * Dot(dir, n)));
}

inline float AxisVal(Vec3 v, int axis) noexcept {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

inline bool IntersectAabb(const RtAabb& box,
                          const RtRay& ray,
                          float t_best,
                          float& out_t_near) noexcept {
    float tmin = ray.t_min;
    float tmax = t_best;
    for (int a = 0; a < 3; ++a) {
        const float orig = AxisVal(ray.origin, a);
        const float dir = AxisVal(ray.direction, a);
        const float bmin = AxisVal(box.min, a);
        const float bmax = AxisVal(box.max, a);
        if (std::fabs(dir) < 1e-8f) {
            if (orig < bmin || orig > bmax) {
                return false;
            }
            continue;
        }
        const float inv = 1.0f / dir;
        float t0 = (bmin - orig) * inv;
        float t1 = (bmax - orig) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin) {
            return false;
        }
    }
    out_t_near = tmin;
    return true;
}

// Möller–Trumbore ray-triangle intersection returning distance, barycentrics, and front-facing normal.
inline bool IntersectTriangle(const RtTriangle& tri,
                              const RtRay& ray,
                              float t_best,
                              float& out_t,
                              float& out_u,
                              float& out_v,
                              Vec3& out_normal) noexcept {
    const Vec3 e1 = Sub(tri.v1, tri.v0);
    const Vec3 e2 = Sub(tri.v2, tri.v0);
    const Vec3 pvec = Cross(ray.direction, e2);
    const float det = Dot(e1, pvec);
    if (std::fabs(det) < 1e-7f) {
        return false;
    }
    const float inv_det = 1.0f / det;
    const Vec3 tvec = Sub(ray.origin, tri.v0);
    const float u = Dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = Cross(tvec, e1);
    const float v = Dot(ray.direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = Dot(e2, qvec) * inv_det;
    if (t < ray.t_min || t >= t_best) {
        return false;
    }
    out_t = t;
    out_u = u;
    out_v = v;
    Vec3 n = Normalize(Cross(e1, e2));
    if (Dot(n, ray.direction) > 0.0f) {
        n = Mul(n, -1.0f);
    }
    out_normal = n;
    return true;
}

inline uint8_t ToByte(float v) noexcept {
    const float clamped = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint8_t>(std::round(clamped * 255.0f));
}

}  // namespace

uint64_t FrameBufferRgba8::ComputeFnv1aHash() const noexcept {
    uint64_t h = 14695981039346656037ull;
    auto mix_u32 = [&](uint32_t v) noexcept {
        for (int shift = 0; shift < 32; shift += 8) {
            h ^= static_cast<uint8_t>((v >> shift) & 0xFFu);
            h *= 1099511628211ull;
        }
    };
    mix_u32(width);
    mix_u32(height);
    for (uint8_t b : pixels) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

Status FrameBufferRgba8::WritePpm(const std::string& file_path) const {
    if (width == 0 || height == 0 ||
        pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
        return Status::Error(StatusCode::InvalidArgument, "FrameBufferRgba8 dimensions/buffer invalid");
    }
    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open PPM output path: " + file_path);
    }
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    ofs.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::vector<uint8_t> rgb;
    rgb.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixel_count; ++i) {
        rgb.push_back(pixels[i * 4u + 0u]);
        rgb.push_back(pixels[i * 4u + 1u]);
        rgb.push_back(pixels[i * 4u + 2u]);
    }
    ofs.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed writing PPM image: " + file_path);
    }
    return Status::Ok();
}

namespace {

uint32_t PngCrc32(const uint8_t* data, size_t len) noexcept {
    static const auto kTable = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256u; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4],
                    const uint8_t* payload, size_t payload_size) {
    AppendBe32(out, static_cast<uint32_t>(payload_size));
    const size_t type_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (payload && payload_size > 0u) {
        out.insert(out.end(), payload, payload + payload_size);
    }
    const uint32_t crc = PngCrc32(out.data() + type_start, 4u + payload_size);
    AppendBe32(out, crc);
}

}  // namespace

Status FrameBufferRgba8::WritePng(const std::string& file_path) const {
    if (width == 0 || height == 0 ||
        pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
        return Status::Error(StatusCode::InvalidArgument, "FrameBufferRgba8 dimensions/buffer invalid");
    }

    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    const size_t raw_size = static_cast<size_t>(height) * (1u + row_bytes);

    std::vector<uint8_t> raw;
    raw.reserve(raw_size);
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0u);
        s2 += s1;
        if (s2 >= 65521u) s2 -= 65521u;
        const uint8_t* src_row = pixels.data() + static_cast<size_t>(y) * row_bytes;
        raw.insert(raw.end(), src_row, src_row + row_bytes);
        for (size_t x = 0; x < row_bytes; ++x) {
            s1 += src_row[x];
            if (s1 >= 65521u) s1 -= 65521u;
            s2 += s1;
            if (s2 >= 65521u) s2 -= 65521u;
        }
    }
    const uint32_t adler32 = (s2 << 16) | s1;

    constexpr size_t kMaxBlock = 65535u;
    const size_t num_blocks = (raw.size() + kMaxBlock - 1u) / kMaxBlock;
    std::vector<uint8_t> idat;
    idat.reserve(2u + num_blocks * 5u + raw.size() + 4u);
    idat.push_back(0x78u);
    idat.push_back(0x01u);

    size_t offset = 0u;
    while (offset < raw.size()) {
        const size_t chunk_len = std::min<size_t>(kMaxBlock, raw.size() - offset);
        const bool is_final = (offset + chunk_len >= raw.size());
        const uint16_t len16 = static_cast<uint16_t>(chunk_len);
        const uint16_t nlen16 = static_cast<uint16_t>(~len16);
        idat.push_back(is_final ? 0x01u : 0x00u);
        idat.push_back(static_cast<uint8_t>(len16 & 0xFFu));
        idat.push_back(static_cast<uint8_t>((len16 >> 8) & 0xFFu));
        idat.push_back(static_cast<uint8_t>(nlen16 & 0xFFu));
        idat.push_back(static_cast<uint8_t>((nlen16 >> 8) & 0xFFu));
        idat.insert(idat.end(), raw.data() + offset, raw.data() + offset + chunk_len);
        offset += chunk_len;
    }
    AppendBe32(idat, adler32);

    std::vector<uint8_t> png;
    png.reserve(64u + idat.size());
    const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    png.insert(png.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    ihdr.reserve(13u);
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8u);
    ihdr.push_back(6u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    AppendPngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    AppendPngChunk(png, "IDAT", idat.data(), idat.size());
    AppendPngChunk(png, "IEND", nullptr, 0u);

    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open PNG output path: " + file_path);
    }
    ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed writing PNG image: " + file_path);
    }
    return Status::Ok();
}

void RtAabb::Expand(Vec3 p) noexcept {
    min.x = std::min(min.x, p.x);
    min.y = std::min(min.y, p.y);
    min.z = std::min(min.z, p.z);
    max.x = std::max(max.x, p.x);
    max.y = std::max(max.y, p.y);
    max.z = std::max(max.z, p.z);
}

void RtAabb::Expand(const RtAabb& other) noexcept {
    Expand(other.min);
    Expand(other.max);
}

float RtAabb::SurfaceArea() const noexcept {
    const float dx = std::max(0.0f, max.x - min.x);
    const float dy = std::max(0.0f, max.y - min.y);
    const float dz = std::max(0.0f, max.z - min.z);
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

Vec3 RtAabb::Center() const noexcept {
    return {
        0.5f * (min.x + max.x),
        0.5f * (min.y + max.y),
        0.5f * (min.z + max.z),
    };
}

Vec3 TransformPoint3x4(const float m[3][4], Vec3 p) noexcept {
    return {
        m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
        m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
        m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
    };
}

Vec3 TransformVector3x4(const float m[3][4], Vec3 v) noexcept {
    return {
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z,
    };
}

RtTriangle TransformTriangle3x4(const float m[3][4], const RtTriangle& tri) noexcept {
    RtTriangle out = tri;
    out.v0 = TransformPoint3x4(m, tri.v0);
    out.v1 = TransformPoint3x4(m, tri.v1);
    out.v2 = TransformPoint3x4(m, tri.v2);
    return out;
}

void MakeIdentity3x4(float out[3][4]) noexcept {
    out[0][0] = 1.0f; out[0][1] = 0.0f; out[0][2] = 0.0f; out[0][3] = 0.0f;
    out[1][0] = 0.0f; out[1][1] = 1.0f; out[1][2] = 0.0f; out[1][3] = 0.0f;
    out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = 1.0f; out[2][3] = 0.0f;
}

void MakeTranslation3x4(float tx, float ty, float tz, float out[3][4]) noexcept {
    MakeIdentity3x4(out);
    out[0][3] = tx;
    out[1][3] = ty;
    out[2][3] = tz;
}

void MakeScaleTranslation3x4(float sx, float sy, float sz,
                             float tx, float ty, float tz,
                             float out[3][4]) noexcept {
    out[0][0] = sx;   out[0][1] = 0.0f; out[0][2] = 0.0f; out[0][3] = tx;
    out[1][0] = 0.0f; out[1][1] = sy;   out[1][2] = 0.0f; out[1][3] = ty;
    out[2][0] = 0.0f; out[2][1] = 0.0f; out[2][2] = sz;   out[2][3] = tz;
}

void MakeRotationY3x4(float radians, float tx, float ty, float tz, float out[3][4]) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    out[0][0] = c;    out[0][1] = 0.0f; out[0][2] = s;    out[0][3] = tx;
    out[1][0] = 0.0f; out[1][1] = 1.0f; out[1][2] = 0.0f; out[1][3] = ty;
    out[2][0] = -s;   out[2][1] = 0.0f; out[2][2] = c;    out[2][3] = tz;
}

bool InvertTransform3x4(const float in[3][4], float out[3][4]) noexcept {
    const float a00 = in[0][0], a01 = in[0][1], a02 = in[0][2];
    const float a10 = in[1][0], a11 = in[1][1], a12 = in[1][2];
    const float a20 = in[2][0], a21 = in[2][1], a22 = in[2][2];

    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a12 * a20 - a10 * a22;
    const float c02 = a10 * a21 - a11 * a20;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-8f || !std::isfinite(det)) {
        return false;
    }

    const float inv_det = 1.0f / det;
    const float r00 = c00 * inv_det;
    const float r01 = (a02 * a21 - a01 * a22) * inv_det;
    const float r02 = (a01 * a12 - a02 * a11) * inv_det;
    const float r10 = c01 * inv_det;
    const float r11 = (a00 * a22 - a02 * a20) * inv_det;
    const float r12 = (a02 * a10 - a00 * a12) * inv_det;
    const float r20 = c02 * inv_det;
    const float r21 = (a01 * a20 - a00 * a21) * inv_det;
    const float r22 = (a00 * a11 - a01 * a10) * inv_det;

    const float tx = in[0][3], ty = in[1][3], tz = in[2][3];
    out[0][0] = r00; out[0][1] = r01; out[0][2] = r02; out[0][3] = -(r00 * tx + r01 * ty + r02 * tz);
    out[1][0] = r10; out[1][1] = r11; out[1][2] = r12; out[1][3] = -(r10 * tx + r11 * ty + r12 * tz);
    out[2][0] = r20; out[2][1] = r21; out[2][2] = r22; out[2][3] = -(r20 * tx + r21 * ty + r22 * tz);
    return true;
}

bool IntersectRayAabb(const RtAabb& box,
                      const RtRay& ray,
                      float t_best,
                      float& out_t_near) noexcept {
    return IntersectAabb(box, ray, t_best, out_t_near);
}

bool IntersectRayTriangle(const RtTriangle& tri,
                          const RtRay& ray,
                          float t_best,
                          float& out_t,
                          float& out_u,
                          float& out_v,
                          Vec3& out_normal) noexcept {
    return IntersectTriangle(tri, ray, t_best, out_t, out_u, out_v, out_normal);
}

bool IntersectRayAabbSlabs(const RtAabb& box,
                           const RtRay& ray,
                           float t_best,
                           float& out_t_near) noexcept {
    return IntersectAabb(box, ray, t_best, out_t_near);
}

bool IntersectRayTriangleEx(const RtTriangle& tri,
                            const RtRay& ray,
                            float t_best,
                            uint32_t ray_flags,
                            uint32_t instance_flags,
                            float& out_t,
                            float& out_u,
                            float& out_v,
                            Vec3& out_normal,
                            bool& out_front_face) noexcept {
    const Vec3 e1 = Sub(tri.v1, tri.v0);
    const Vec3 e2 = Sub(tri.v2, tri.v0);
    const Vec3 pvec = Cross(ray.direction, e2);
    const float det = Dot(e1, pvec);
    if (std::fabs(det) < 1e-7f) {
        return false;
    }

    // Standard D3D12/DXR winding convention: det > 0 is front-facing unless
    // D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE flips it.
    const bool front_ccw = (instance_flags & kDxrInstanceTriangleFrontCounterClockwise) != 0u;
    const bool front_face = front_ccw ? (det < 0.0f) : (det > 0.0f);

    const bool cull_disable = (instance_flags & kDxrInstanceTriangleCullDisable) != 0u;
    if (!cull_disable) {
        if ((ray_flags & kRdna2RayFlagCullBackFacingTriangles) != 0u && !front_face) {
            return false;
        }
        if ((ray_flags & kRdna2RayFlagCullFrontFacingTriangles) != 0u && front_face) {
            return false;
        }
    }

    const float inv_det = 1.0f / det;
    const Vec3 tvec = Sub(ray.origin, tri.v0);
    const float u = Dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = Cross(tvec, e1);
    const float v = Dot(ray.direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = Dot(e2, qvec) * inv_det;
    if (t < ray.t_min || t >= t_best) {
        return false;
    }

    out_t = t;
    out_u = u;
    out_v = v;
    out_front_face = front_face;
    Vec3 n = Normalize(Cross(e1, e2));
    if (Dot(n, ray.direction) > 0.0f) {
        n = Mul(n, -1.0f);
    }
    out_normal = n;
    return true;
}

bool BvhAccelerationStructure::GetNodeView(uint32_t index, BvhNodeView& out_node) const noexcept {
    if (index >= nodes_.size()) {
        return false;
    }
    const BvhNode& n = nodes_[index];
    out_node.bounds = n.bounds;
    out_node.left_first = n.left_first;
    out_node.right_child = n.right_child;
    out_node.tri_count = n.tri_count;
    return true;
}

Status BvhAccelerationStructure::Build(const std::vector<RtTriangle>& triangles) {
    if (triangles.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot build BVH from 0 triangles");
    }

    triangles_ = triangles;
    tri_indices_.resize(triangles_.size());
    std::iota(tri_indices_.begin(), tri_indices_.end(), 0u);

    std::vector<PrimInfo> prims(triangles_.size());
    for (size_t i = 0; i < triangles_.size(); ++i) {
        const RtTriangle& t = triangles_[i];
        RtAabb box;
        box.Expand(t.v0);
        box.Expand(t.v1);
        box.Expand(t.v2);
        prims[i].bounds = box;
        prims[i].centroid = Mul(Add(Add(t.v0, t.v1), t.v2), 1.0f / 3.0f);
    }

    nodes_.clear();
    nodes_.reserve(triangles_.size() * 2u);
    leaf_node_count_ = 0;
    max_tree_depth_ = 0;
    BuildRecursive(0u, static_cast<uint32_t>(triangles_.size()), 1u, prims);
    return Status::Ok();
}

uint32_t BvhAccelerationStructure::BuildRecursive(uint32_t first,
                                                  uint32_t count,
                                                  uint32_t depth,
                                                  const std::vector<PrimInfo>& prims) {
    const uint32_t node_idx = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(BvhNode{});
    max_tree_depth_ = std::max(max_tree_depth_, depth);

    RtAabb bounds;
    RtAabb centroid_bounds;
    for (uint32_t i = 0; i < count; ++i) {
        const PrimInfo& p = prims[tri_indices_[first + i]];
        bounds.Expand(p.bounds);
        centroid_bounds.Expand(p.centroid);
    }
    nodes_[node_idx].bounds = bounds;

    constexpr uint32_t kMinLeafPrims = 2u;
    constexpr uint32_t kMaxSahLeafPrims = 4u;
    constexpr uint32_t kMaxDepth = 56u;
    if (count <= kMinLeafPrims || depth >= kMaxDepth) {
        nodes_[node_idx].left_first = first;
        nodes_[node_idx].tri_count = count;
        ++leaf_node_count_;
        return node_idx;
    }

    // Binned Surface Area Heuristic (SAH) split evaluation across all 3 axes.
    constexpr uint32_t kSahBins = 12u;
    struct SahBin {
        RtAabb bounds;
        uint32_t count = 0;
    };

    const float parent_area = bounds.SurfaceArea();
    const float leaf_cost = static_cast<float>(count);
    constexpr float kTraversalCost = 0.5f;

    int best_axis = -1;
    float best_split_pos = 0.0f;
    float best_cost = 1e30f;

    if (parent_area > 1e-12f) {
        for (int axis = 0; axis < 3; ++axis) {
            const float cmin = AxisVal(centroid_bounds.min, axis);
            const float cmax = AxisVal(centroid_bounds.max, axis);
            const float cextent = cmax - cmin;
            if (cextent < 1e-6f) {
                continue;
            }

            std::array<SahBin, kSahBins> bins{};
            const float scale = static_cast<float>(kSahBins) / cextent;
            for (uint32_t i = 0; i < count; ++i) {
                const PrimInfo& p = prims[tri_indices_[first + i]];
                int bin_idx = static_cast<int>((AxisVal(p.centroid, axis) - cmin) * scale);
                bin_idx = std::clamp(bin_idx, 0, static_cast<int>(kSahBins) - 1);
                bins[static_cast<size_t>(bin_idx)].bounds.Expand(p.bounds);
                ++bins[static_cast<size_t>(bin_idx)].count;
            }

            std::array<float, kSahBins - 1> left_area{};
            std::array<uint32_t, kSahBins - 1> left_count{};
            RtAabb accum_left;
            uint32_t accum_left_count = 0;
            for (uint32_t b = 0; b + 1u < kSahBins; ++b) {
                if (bins[b].count > 0) {
                    accum_left.Expand(bins[b].bounds);
                    accum_left_count += bins[b].count;
                }
                left_area[b] = accum_left_count > 0 ? accum_left.SurfaceArea() : 0.0f;
                left_count[b] = accum_left_count;
            }

            RtAabb accum_right;
            uint32_t accum_right_count = 0;
            for (uint32_t rev = kSahBins - 1u; rev > 0; --rev) {
                if (bins[rev].count > 0) {
                    accum_right.Expand(bins[rev].bounds);
                    accum_right_count += bins[rev].count;
                }
                const uint32_t split_idx = rev - 1u;
                const uint32_t lc = left_count[split_idx];
                const uint32_t rc = accum_right_count;
                if (lc == 0 || rc == 0) {
                    continue;
                }
                const float ra = accum_right.SurfaceArea();
                const float cost =
                    kTraversalCost +
                    (static_cast<float>(lc) * left_area[split_idx] + static_cast<float>(rc) * ra) /
                        parent_area;
                if (cost < best_cost) {
                    best_cost = cost;
                    best_axis = axis;
                    best_split_pos = cmin + cextent * (static_cast<float>(rev) / static_cast<float>(kSahBins));
                }
            }
        }
    }

    if (best_axis != -1 && best_cost >= leaf_cost && count <= kMaxSahLeafPrims) {
        nodes_[node_idx].left_first = first;
        nodes_[node_idx].tri_count = count;
        ++leaf_node_count_;
        return node_idx;
    }

    uint32_t mid = first;
    if (best_axis != -1) {
        auto it = std::partition(tri_indices_.begin() + static_cast<std::ptrdiff_t>(first),
                                 tri_indices_.begin() + static_cast<std::ptrdiff_t>(first + count),
                                 [&](uint32_t tri_idx) {
                                     return AxisVal(prims[tri_idx].centroid, best_axis) < best_split_pos;
                                 });
        mid = static_cast<uint32_t>(std::distance(tri_indices_.begin(), it));
    }

    // Fallback to balanced median split along longest centroid/box axis if SAH degenerates.
    if (mid <= first || mid >= first + count) {
        const Vec3 extent = Sub(bounds.max, bounds.min);
        int axis = 0;
        if (extent.y > extent.x) axis = 1;
        if (extent.z > AxisVal(extent, axis)) axis = 2;
        mid = first + count / 2u;
        std::nth_element(tri_indices_.begin() + static_cast<std::ptrdiff_t>(first),
                         tri_indices_.begin() + static_cast<std::ptrdiff_t>(mid),
                         tri_indices_.begin() + static_cast<std::ptrdiff_t>(first + count),
                         [&](uint32_t a, uint32_t b) {
                             return AxisVal(prims[a].centroid, axis) < AxisVal(prims[b].centroid, axis);
                         });
    }

    const uint32_t left_child = BuildRecursive(first, mid - first, depth + 1u, prims);
    const uint32_t right_child = BuildRecursive(mid, first + count - mid, depth + 1u, prims);
    nodes_[node_idx].left_first = left_child;
    nodes_[node_idx].right_child = right_child;
    nodes_[node_idx].tri_count = 0;
    return node_idx;
}

bool BvhAccelerationStructure::TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats) const {
    out_hit = RtHit{};
    out_hit.t = ray.t_max;
    if (nodes_.empty()) {
        return false;
    }

    float root_near = 0.0f;
    if (stats) ++stats->aabb_tests;
    if (!IntersectAabb(nodes_[0].bounds, ray, out_hit.t, root_near)) {
        return false;
    }

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0u;

    while (sp > 0) {
        const uint32_t idx = stack[--sp];
        const BvhNode& node = nodes_[idx];

        if (node.tri_count > 0) {
            for (uint32_t i = 0; i < node.tri_count; ++i) {
                const uint32_t tri_idx = tri_indices_[node.left_first + i];
                const RtTriangle& tri = triangles_[tri_idx];
                if (stats) ++stats->triangle_tests;
                float t = 0.0f;
                float u = 0.0f;
                float v = 0.0f;
                Vec3 n{};
                if (IntersectTriangle(tri, ray, out_hit.t, t, u, v, n)) {
                    out_hit.hit = true;
                    out_hit.t = t;
                    out_hit.u = u;
                    out_hit.v = v;
                    out_hit.triangle_index = tri_idx;
                    out_hit.normal = n;
                    out_hit.color = tri.color;
                    out_hit.reflectivity = tri.reflectivity;
                }
            }
            continue;
        }

        const uint32_t left_idx = node.left_first;
        const uint32_t right_idx = node.right_child;
        float t_left = 0.0f;
        float t_right = 0.0f;
        if (stats) stats->aabb_tests += 2u;
        const bool hit_left = IntersectAabb(nodes_[left_idx].bounds, ray, out_hit.t, t_left);
        const bool hit_right = IntersectAabb(nodes_[right_idx].bounds, ray, out_hit.t, t_right);

        if (hit_left && hit_right) {
            if (sp + 2 <= 64) {
                // Push farther child first so closer child is visited first.
                if (t_left <= t_right) {
                    stack[sp++] = right_idx;
                    stack[sp++] = left_idx;
                } else {
                    stack[sp++] = left_idx;
                    stack[sp++] = right_idx;
                }
            }
        } else if (hit_left && sp < 64) {
            stack[sp++] = left_idx;
        } else if (hit_right && sp < 64) {
            stack[sp++] = right_idx;
        }
    }

    return out_hit.hit;
}

bool BvhAccelerationStructure::TraceShadowRay(const RtRay& ray, RtStats* stats) const {
    if (nodes_.empty()) {
        return false;
    }

    float root_near = 0.0f;
    if (stats) ++stats->aabb_tests;
    if (!IntersectAabb(nodes_[0].bounds, ray, ray.t_max, root_near)) {
        return false;
    }

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0u;

    while (sp > 0) {
        const uint32_t idx = stack[--sp];
        const BvhNode& node = nodes_[idx];
        if (node.tri_count > 0) {
            for (uint32_t i = 0; i < node.tri_count; ++i) {
                const uint32_t tri_idx = tri_indices_[node.left_first + i];
                const RtTriangle& tri = triangles_[tri_idx];
                if (stats) ++stats->triangle_tests;
                float t = 0.0f;
                float u = 0.0f;
                float v = 0.0f;
                Vec3 n{};
                if (IntersectTriangle(tri, ray, ray.t_max, t, u, v, n)) {
                    return true;
                }
            }
            continue;
        }

        float t_near = 0.0f;
        if (stats) ++stats->aabb_tests;
        if (IntersectAabb(nodes_[node.left_first].bounds, ray, ray.t_max, t_near) && sp < 64) {
            stack[sp++] = node.left_first;
        }
        if (stats) ++stats->aabb_tests;
        if (IntersectAabb(nodes_[node.right_child].bounds, ray, ray.t_max, t_near) && sp < 64) {
            stack[sp++] = node.right_child;
        }
    }

    return false;
}

Vec3 BvhAccelerationStructure::TraceColor(const RtRay& ray,
                                          const Vec3& light_pos,
                                          uint32_t depth,
                                          RtStats* stats,
                                          bool is_primary) const {
    RtHit hit{};
    if (!TraceRay(ray, hit, stats)) {
        // Sky gradient miss shader
        const float t = 0.5f * (Normalize(ray.direction).y + 1.0f);
        return Add(Mul({0.06f, 0.09f, 0.16f}, 1.0f - t), Mul({0.18f, 0.34f, 0.58f}, t));
    }

    if (is_primary && stats) {
        ++stats->primary_hits;
    }

    const Vec3 hit_pos = Add(ray.origin, Mul(ray.direction, hit.t));
    const Vec3 to_light = Sub(light_pos, hit_pos);
    const float light_dist = std::sqrt(Dot(to_light, to_light));
    const Vec3 light_dir = Mul(to_light, 1.0f / std::max(light_dist, 1e-6f));

    // Shadow occlusion ray
    RtRay shadow_ray{};
    shadow_ray.origin = Add(hit_pos, Mul(hit.normal, 0.002f));
    shadow_ray.direction = light_dir;
    shadow_ray.t_min = 0.001f;
    shadow_ray.t_max = std::max(0.002f, light_dist - 0.002f);
    if (stats) ++stats->shadow_rays;
    const bool in_shadow = TraceShadowRay(shadow_ray, stats);
    if (in_shadow && stats) {
        ++stats->shadow_hits;
    }

    const float ndotl = std::max(0.0f, Dot(hit.normal, light_dir));
    const float ambient = 0.22f;
    const float diffuse = in_shadow ? 0.0f : (0.78f * ndotl);
    Vec3 local_color = Mul(hit.color, ambient + diffuse);

    if (depth > 0 && hit.reflectivity > 0.01f) {
        RtRay refl_ray{};
        refl_ray.origin = Add(hit_pos, Mul(hit.normal, 0.002f));
        refl_ray.direction = Normalize(Reflect(ray.direction, hit.normal));
        if (stats) ++stats->reflection_rays;
        const Vec3 refl_col = TraceColor(refl_ray, light_pos, depth - 1u, stats, false);
        local_color = Add(Mul(local_color, 1.0f - hit.reflectivity), Mul(refl_col, hit.reflectivity));
    }

    return local_color;
}

Status BvhAccelerationStructure::RenderScene(const RtDispatchParams& params,
                                             FrameBufferRgba8& out_frame,
                                             RtStats* out_stats) const {
    if (nodes_.empty()) {
        return Status::Error(StatusCode::RaytracingError, "BVH is empty");
    }
    if (params.width == 0 || params.height == 0) {
        return Status::Error(StatusCode::InvalidArgument, "RT dispatch dimensions must be > 0");
    }

    RtStats local_stats{};
    local_stats.bvh_nodes = NodeCount();
    local_stats.bvh_leaf_nodes = LeafNodeCount();
    local_stats.max_tree_depth = MaxTreeDepth();

    const Vec3 forward = Normalize(Sub(params.camera_look_at, params.camera_pos));
    const Vec3 right = Normalize(Cross({0.0f, 1.0f, 0.0f}, forward));
    const Vec3 up = Cross(forward, right);
    const float aspect = static_cast<float>(params.width) / static_cast<float>(params.height);
    const float fov_scale = 0.65f;

    out_frame.width = params.width;
    out_frame.height = params.height;
    out_frame.pixels.resize(static_cast<size_t>(params.width) * static_cast<size_t>(params.height) * 4u);

    for (uint32_t y = 0; y < params.height; ++y) {
        const float ndc_y = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(params.height);
        for (uint32_t x = 0; x < params.width; ++x) {
            const float ndc_x = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(params.width) - 1.0f;
            RtRay ray{};
            ray.origin = params.camera_pos;
            ray.direction = Normalize(Add(Add(forward, Mul(right, ndc_x * aspect * fov_scale)),
                                          Mul(up, ndc_y * fov_scale)));
            ++local_stats.primary_rays;
            const Vec3 col = TraceColor(ray, params.light_pos, params.max_bounces, &local_stats, true);
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(params.width) + x) * 4u;
            out_frame.pixels[idx + 0u] = ToByte(col.x);
            out_frame.pixels[idx + 1u] = ToByte(col.y);
            out_frame.pixels[idx + 2u] = ToByte(col.z);
            out_frame.pixels[idx + 3u] = 255u;
        }
    }

    if (out_stats) {
        *out_stats = local_stats;
    }
    return Status::Ok();
}

std::vector<RtTriangle> BuildDefaultRtTestScene() {
    std::vector<RtTriangle> tris;
    tris.reserve(12);
    // Reflective floor (y = 0)
    tris.push_back({{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f}, {0.72f, 0.75f, 0.80f}, 0.45f});
    tris.push_back({{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 3.0f}, {0.72f, 0.75f, 0.80f}, 0.45f});

    // Back wall (z = 2.5)
    tris.push_back({{-3.0f, 0.0f, 2.5f}, {3.0f, 0.0f, 2.5f}, {3.0f, 3.0f, 2.5f}, {0.25f, 0.30f, 0.38f}, 0.10f});
    tris.push_back({{-3.0f, 0.0f, 2.5f}, {3.0f, 3.0f, 2.5f}, {-3.0f, 3.0f, 2.5f}, {0.25f, 0.30f, 0.38f}, 0.10f});

    // Red left wall
    tris.push_back({{-2.5f, 0.0f, -2.5f}, {-2.5f, 0.0f, 2.5f}, {-2.5f, 3.0f, 2.5f}, {0.85f, 0.18f, 0.18f}, 0.05f});
    tris.push_back({{-2.5f, 0.0f, -2.5f}, {-2.5f, 3.0f, 2.5f}, {-2.5f, 3.0f, -2.5f}, {0.85f, 0.18f, 0.18f}, 0.05f});

    // Green right wall
    tris.push_back({{2.5f, 0.0f, 2.5f}, {2.5f, 0.0f, -2.5f}, {2.5f, 3.0f, -2.5f}, {0.10f, 0.78f, 0.22f}, 0.05f});
    tris.push_back({{2.5f, 0.0f, 2.5f}, {2.5f, 3.0f, -2.5f}, {2.5f, 3.0f, 2.5f}, {0.10f, 0.78f, 0.22f}, 0.05f});

    // Floating glossy pyramid in center (casts shadow onto floor + reflects walls)
    const Vec3 apex{0.0f, 1.55f, 0.2f};
    const Vec3 b0{-0.65f, 0.35f, -0.35f};
    const Vec3 b1{0.65f, 0.35f, -0.35f};
    const Vec3 b2{0.65f, 0.35f, 0.75f};
    const Vec3 b3{-0.65f, 0.35f, 0.75f};
    tris.push_back({b0, b1, apex, {0.95f, 0.78f, 0.18f}, 0.30f});
    tris.push_back({b1, b2, apex, {0.18f, 0.72f, 0.95f}, 0.30f});
    tris.push_back({b2, b3, apex, {0.85f, 0.25f, 0.85f}, 0.30f});
    tris.push_back({b3, b0, apex, {0.95f, 0.42f, 0.18f}, 0.30f});
    return tris;
}

std::vector<RtTriangle> BuildUnitCubeMesh(Vec3 color, float reflectivity) {
    const Vec3 p000{-0.5f, -0.5f, -0.5f};
    const Vec3 p100{ 0.5f, -0.5f, -0.5f};
    const Vec3 p110{ 0.5f,  0.5f, -0.5f};
    const Vec3 p010{-0.5f,  0.5f, -0.5f};
    const Vec3 p001{-0.5f, -0.5f,  0.5f};
    const Vec3 p101{ 0.5f, -0.5f,  0.5f};
    const Vec3 p111{ 0.5f,  0.5f,  0.5f};
    const Vec3 p011{-0.5f,  0.5f,  0.5f};

    std::vector<RtTriangle> tris;
    tris.reserve(12);
    auto add_quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        tris.push_back({a, b, c, color, reflectivity});
        tris.push_back({a, c, d, color, reflectivity});
    };
    add_quad(p000, p100, p110, p010);  // -Z
    add_quad(p101, p001, p011, p111);  // +Z
    add_quad(p001, p000, p010, p011);  // -X
    add_quad(p100, p101, p111, p110);  // +X
    add_quad(p010, p110, p111, p011);  // +Y
    add_quad(p001, p101, p100, p000);  // -Y
    return tris;
}

}  // namespace openscarlett
