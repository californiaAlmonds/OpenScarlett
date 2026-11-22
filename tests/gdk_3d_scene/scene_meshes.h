#pragma once

// OpenScarlett MIT 3D Multi-Mesh & Skinned Armature Geometry Builder.
// Builds rigid meshes (floor plane, beveled pedestal/monument, orbiting cubes/spheres, pillars)
// and a 3-bone linear-blend-skinned articulated banner/armature sharing one unified vertex layout.

#include "scene_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gdk3d {

struct SceneVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
    uint32_t bone_idx[4];
    float bone_weight[4];
};
static_assert(sizeof(SceneVertex) == 80, "SceneVertex must be 80 bytes");

struct MeshRange {
    uint32_t index_start = 0;
    uint32_t index_count = 0;
    int32_t base_vertex = 0;
};

inline SceneVertex MakeRigidVertex(Vec3 p, Vec3 n, Vec2 uv, const float col[4]) noexcept {
    SceneVertex v{};
    v.pos[0] = p.x;
    v.pos[1] = p.y;
    v.pos[2] = p.z;
    v.normal[0] = n.x;
    v.normal[1] = n.y;
    v.normal[2] = n.z;
    v.uv[0] = uv.x;
    v.uv[1] = uv.y;
    v.color[0] = col[0];
    v.color[1] = col[1];
    v.color[2] = col[2];
    v.color[3] = col[3];
    v.bone_idx[0] = 0u;
    v.bone_idx[1] = 0u;
    v.bone_idx[2] = 0u;
    v.bone_idx[3] = 0u;
    v.bone_weight[0] = 1.0f;
    v.bone_weight[1] = 0.0f;
    v.bone_weight[2] = 0.0f;
    v.bone_weight[3] = 0.0f;
    return v;
}

inline MeshRange AppendFloorPlane(std::vector<SceneVertex>& verts,
                                  std::vector<uint32_t>& indices,
                                  float half_span,
                                  float uv_repeat) {
    MeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    constexpr float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // Subdivided 8x8 ground plane for smooth vertex interpolation and crisp shadows.
    constexpr uint32_t kGrid = 8u;
    for (uint32_t gz = 0; gz <= kGrid; ++gz) {
        const float tz = static_cast<float>(gz) / static_cast<float>(kGrid);
        const float z = -half_span + tz * (2.0f * half_span);
        for (uint32_t gx = 0; gx <= kGrid; ++gx) {
            const float tx = static_cast<float>(gx) / static_cast<float>(kGrid);
            const float x = -half_span + tx * (2.0f * half_span);
            verts.push_back(MakeRigidVertex(
                {x, 0.0f, z}, up, {tx * uv_repeat, tz * uv_repeat}, kWhite));
        }
    }

    const uint32_t stride = kGrid + 1u;
    for (uint32_t gz = 0; gz < kGrid; ++gz) {
        for (uint32_t gx = 0; gx < kGrid; ++gx) {
            const uint32_t v00 = gz * stride + gx;
            const uint32_t v10 = gz * stride + (gx + 1u);
            const uint32_t v01 = (gz + 1u) * stride + gx;
            const uint32_t v11 = (gz + 1u) * stride + (gx + 1u);
            // CW when viewed from +Y in RH space
            indices.push_back(v00);
            indices.push_back(v10);
            indices.push_back(v11);
            indices.push_back(v00);
            indices.push_back(v11);
            indices.push_back(v01);
        }
    }

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

inline MeshRange AppendUnitBox(std::vector<SceneVertex>& verts,
                               std::vector<uint32_t>& indices,
                               float hx,
                               float hy,
                               float hz,
                               float y_offset,
                               const float col[4]) {
    MeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    struct Face {
        Vec3 n;
        Vec3 c[4];
    };
    const Face faces[6] = {
        {{ 0.0f,  0.0f,  1.0f}, {{-hx,  hy,  hz}, { hx,  hy,  hz}, { hx, -hy,  hz}, {-hx, -hy,  hz}}},
        {{ 0.0f,  0.0f, -1.0f}, {{ hx,  hy, -hz}, {-hx,  hy, -hz}, {-hx, -hy, -hz}, { hx, -hy, -hz}}},
        {{ 1.0f,  0.0f,  0.0f}, {{ hx,  hy,  hz}, { hx,  hy, -hz}, { hx, -hy, -hz}, { hx, -hy,  hz}}},
        {{-1.0f,  0.0f,  0.0f}, {{-hx,  hy, -hz}, {-hx,  hy,  hz}, {-hx, -hy,  hz}, {-hx, -hy, -hz}}},
        {{ 0.0f,  1.0f,  0.0f}, {{-hx,  hy, -hz}, { hx,  hy, -hz}, { hx,  hy,  hz}, {-hx,  hy,  hz}}},
        {{ 0.0f, -1.0f,  0.0f}, {{-hx, -hy,  hz}, { hx, -hy,  hz}, { hx, -hy, -hz}, {-hx, -hy, -hz}}},
    };
    const Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (uint32_t f = 0; f < 6u; ++f) {
        const uint32_t base = static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
        for (uint32_t k = 0; k < 4u; ++k) {
            Vec3 p = faces[f].c[k];
            p.y += y_offset;
            verts.push_back(MakeRigidVertex(p, faces[f].n, uvs[k], col));
        }
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

inline MeshRange AppendUvSphere(std::vector<SceneVertex>& verts,
                                std::vector<uint32_t>& indices,
                                float radius,
                                uint32_t lon_segs,
                                uint32_t lat_segs,
                                const float col[4]) {
    MeshRange range{};
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
            const Vec3 n{sin_phi * std::cos(theta), cos_phi, sin_phi * std::sin(theta)};
            const Vec3 p{n.x * radius, n.y * radius, n.z * radius};
            verts.push_back(MakeRigidVertex(p, n, {u * 2.0f, v}, col));
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

// Builds a stepped central monument (wide plinth + tapered core column + crown cap) in local space.
inline MeshRange AppendMonument(std::vector<SceneVertex>& verts,
                                std::vector<uint32_t>& indices,
                                const float col[4]) {
    MeshRange total{};
    total.index_start = static_cast<uint32_t>(indices.size());
    total.base_vertex = static_cast<int32_t>(verts.size());

    auto append_part = [&](float hx, float hy, float hz, float y_center) {
        const uint32_t local_base =
            static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - total.base_vertex);
        std::vector<SceneVertex> pv;
        std::vector<uint32_t> pi;
        AppendUnitBox(pv, pi, hx, hy, hz, y_center, col);
        verts.insert(verts.end(), pv.begin(), pv.end());
        for (uint32_t idx : pi) {
            indices.push_back(local_base + idx);
        }
    };

    append_part(0.95f, 0.16f, 0.95f, 0.16f);  // Lower plinth
    append_part(0.68f, 0.18f, 0.68f, 0.50f);  // Mid pedestal
    append_part(0.42f, 0.72f, 0.42f, 1.40f);  // Main obelisk tower
    append_part(0.56f, 0.12f, 0.56f, 2.24f);  // Crown capital

    total.index_count = static_cast<uint32_t>(indices.size()) - total.index_start;
    return total;
}

// Builds a 3-bone linear-blend-skinned solid articulated banner / mechanical wing extending
// along +X (x in [0, length]), with front (+Z) and back (-Z) faces so it renders cleanly
// from every camera orbit angle and casts animated shadows onto the BC1 ground plane!
inline MeshRange AppendSkinnedBanner(std::vector<SceneVertex>& verts,
                                     std::vector<uint32_t>& indices,
                                     float length,
                                     float half_height,
                                     float half_thick,
                                     uint32_t segs_x,
                                     const float col[4]) {
    MeshRange range{};
    range.index_start = static_cast<uint32_t>(indices.size());
    range.base_vertex = static_cast<int32_t>(verts.size());

    auto make_skinned = [&](Vec3 p, Vec3 n, Vec2 uv, float t) -> SceneVertex {
        SceneVertex v{};
        v.pos[0] = p.x;
        v.pos[1] = p.y;
        v.pos[2] = p.z;
        v.normal[0] = n.x;
        v.normal[1] = n.y;
        v.normal[2] = n.z;
        v.uv[0] = uv.x;
        v.uv[1] = uv.y;
        v.color[0] = col[0];
        v.color[1] = col[1];
        v.color[2] = col[2];
        v.color[3] = col[3];

        // Smooth 3-bone skinning weights:
        //   t in [0.0, 0.5]: blend bone 0 -> bone 1
        //   t in [0.5, 1.0]: blend bone 1 -> bone 2
        if (t <= 0.5f) {
            const float w1 = std::clamp(t * 2.0f, 0.0f, 1.0f);
            v.bone_idx[0] = 0u;
            v.bone_idx[1] = 1u;
            v.bone_weight[0] = 1.0f - w1;
            v.bone_weight[1] = w1;
        } else {
            const float w2 = std::clamp((t - 0.5f) * 2.0f, 0.0f, 1.0f);
            v.bone_idx[0] = 1u;
            v.bone_idx[1] = 2u;
            v.bone_weight[0] = 1.0f - w2;
            v.bone_weight[1] = w2;
        }
        v.bone_idx[2] = 0u;
        v.bone_idx[3] = 0u;
        v.bone_weight[2] = 0.0f;
        v.bone_weight[3] = 0.0f;
        return v;
    };

    auto add_face = [&](float z_sign) {
        const uint32_t base = static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
        const Vec3 n{0.0f, 0.0f, z_sign};
        const float z = z_sign * half_thick;
        for (uint32_t i = 0; i <= segs_x; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs_x);
            const float x = t * length;
            // Subtle taper toward banner tip
            const float hh = half_height * (1.0f - 0.25f * t);
            verts.push_back(make_skinned({x,  hh, z}, n, {t, 0.0f}, t));
            verts.push_back(make_skinned({x, -hh, z}, n, {t, 1.0f}, t));
        }
        for (uint32_t i = 0; i < segs_x; ++i) {
            const uint32_t top0 = base + i * 2u;
            const uint32_t bot0 = top0 + 1u;
            const uint32_t top1 = top0 + 2u;
            const uint32_t bot1 = top0 + 3u;
            if (z_sign > 0.0f) {
                indices.push_back(top0);
                indices.push_back(top1);
                indices.push_back(bot1);
                indices.push_back(top0);
                indices.push_back(bot1);
                indices.push_back(bot0);
            } else {
                indices.push_back(top0);
                indices.push_back(bot1);
                indices.push_back(top1);
                indices.push_back(top0);
                indices.push_back(bot0);
                indices.push_back(bot1);
            }
        }
    };

    add_face(1.0f);
    add_face(-1.0f);

    // Top & bottom edge strips so the banner is a solid watertight slab.
    auto add_edge = [&](float y_sign) {
        const uint32_t base = static_cast<uint32_t>(static_cast<int32_t>(verts.size()) - range.base_vertex);
        const Vec3 n{0.0f, y_sign, 0.0f};
        for (uint32_t i = 0; i <= segs_x; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs_x);
            const float x = t * length;
            const float y = y_sign * half_height * (1.0f - 0.25f * t);
            verts.push_back(make_skinned({x, y, -half_thick}, n, {t, 0.0f}, t));
            verts.push_back(make_skinned({x, y,  half_thick}, n, {t, 1.0f}, t));
        }
        for (uint32_t i = 0; i < segs_x; ++i) {
            const uint32_t a0 = base + i * 2u;
            const uint32_t b0 = a0 + 1u;
            const uint32_t a1 = a0 + 2u;
            const uint32_t b1 = a0 + 3u;
            if (y_sign > 0.0f) {
                indices.push_back(a0);
                indices.push_back(a1);
                indices.push_back(b1);
                indices.push_back(a0);
                indices.push_back(b1);
                indices.push_back(b0);
            } else {
                indices.push_back(a0);
                indices.push_back(b1);
                indices.push_back(a1);
                indices.push_back(a0);
                indices.push_back(b0);
                indices.push_back(b1);
            }
        }
    };

    add_edge(1.0f);
    add_edge(-1.0f);

    range.index_count = static_cast<uint32_t>(indices.size()) - range.index_start;
    return range;
}

}  // namespace gdk3d
