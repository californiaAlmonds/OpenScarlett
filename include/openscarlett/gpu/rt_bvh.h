#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/status.h"

namespace openscarlett {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RtTriangle {
    Vec3 v0{};
    Vec3 v1{};
    Vec3 v2{};
    Vec3 color{0.8f, 0.8f, 0.8f};
    float reflectivity = 0.0f;
};

struct RtAabb {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};

    void Expand(Vec3 p) noexcept;
    void Expand(const RtAabb& other) noexcept;
    float SurfaceArea() const noexcept;
    Vec3 Center() const noexcept;
};

struct RtRay {
    Vec3 origin{};
    Vec3 direction{0.0f, 0.0f, 1.0f};
    float t_min = 0.001f;
    float t_max = 1e30f;
};

struct RtHit {
    bool hit = false;
    float t = 1e30f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t triangle_index = 0;
    Vec3 normal{};
    Vec3 color{};
    float reflectivity = 0.0f;
};

struct RtDispatchParams {
    uint32_t width = 160;
    uint32_t height = 120;
    Vec3 camera_pos{0.0f, 1.0f, -3.5f};
    Vec3 camera_look_at{0.0f, 0.5f, 0.0f};
    Vec3 light_pos{1.5f, 3.0f, -1.5f};
    uint32_t max_bounces = 2;
};

struct RtStats {
    uint64_t primary_rays = 0;
    uint64_t primary_hits = 0;
    uint64_t shadow_rays = 0;
    uint64_t shadow_hits = 0;
    uint64_t reflection_rays = 0;
    uint64_t aabb_tests = 0;
    uint64_t triangle_tests = 0;
    uint32_t bvh_nodes = 0;
    uint32_t bvh_leaf_nodes = 0;
    uint32_t max_tree_depth = 0;
};

// Row-major 3x4 affine instance transform helpers (matches D3D12_RAYTRACING_INSTANCE_DESC::Transform).
Vec3 TransformPoint3x4(const float m[3][4], Vec3 p) noexcept;
Vec3 TransformVector3x4(const float m[3][4], Vec3 v) noexcept;
RtTriangle TransformTriangle3x4(const float m[3][4], const RtTriangle& tri) noexcept;
void MakeIdentity3x4(float out[3][4]) noexcept;
void MakeTranslation3x4(float tx, float ty, float tz, float out[3][4]) noexcept;
void MakeScaleTranslation3x4(float sx, float sy, float sz,
                             float tx, float ty, float tz,
                             float out[3][4]) noexcept;
void MakeRotationY3x4(float radians, float tx, float ty, float tz, float out[3][4]) noexcept;
bool InvertTransform3x4(const float in[3][4], float out[3][4]) noexcept;

// Public ray-primitive intersection helpers shared by SAH BVH2 and RDNA2 BVH4 traversal.
bool IntersectRayAabb(const RtAabb& box,
                      const RtRay& ray,
                      float t_best,
                      float& out_t_near) noexcept;
bool IntersectRayTriangle(const RtTriangle& tri,
                          const RtRay& ray,
                          float t_best,
                          float& out_t,
                          float& out_u,
                          float& out_v,
                          Vec3& out_normal) noexcept;

// Read-only view of a single binary SAH BVH node for downstream encoders (e.g., RDNA2 BVH4).
struct BvhNodeView {
    RtAabb bounds;
    uint32_t left_first = 0;   // left child index if tri_count == 0, else first triangle index
    uint32_t right_child = 0;  // right child index if tri_count == 0
    uint32_t tri_count = 0;
};

// Binned Surface Area Heuristic (SAH) software BVH for DXR acceleration
// structure traversal and deterministic fallback on non-DXR GPUs and WARP.
class BvhAccelerationStructure {
public:
    Status Build(const std::vector<RtTriangle>& triangles);

    bool TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats = nullptr) const;
    bool TraceShadowRay(const RtRay& ray, RtStats* stats = nullptr) const;

    Status RenderScene(const RtDispatchParams& params,
                       FrameBufferRgba8& out_frame,
                       RtStats* out_stats = nullptr) const;

    uint32_t NodeCount() const noexcept { return static_cast<uint32_t>(nodes_.size()); }
    uint32_t LeafNodeCount() const noexcept { return leaf_node_count_; }
    uint32_t MaxTreeDepth() const noexcept { return max_tree_depth_; }
    uint32_t TriangleCount() const noexcept { return static_cast<uint32_t>(triangles_.size()); }
    RtAabb RootBounds() const noexcept { return nodes_.empty() ? RtAabb{} : nodes_[0].bounds; }

    // Read-only inspection accessors for RDNA2 BVH4 serialization/verification.
    bool GetNodeView(uint32_t index, BvhNodeView& out_node) const noexcept;
    const std::vector<RtTriangle>& Triangles() const noexcept { return triangles_; }
    const std::vector<uint32_t>& TriangleIndices() const noexcept { return tri_indices_; }

private:
    struct BvhNode {
        RtAabb bounds;
        uint32_t left_first = 0;   // left child index if tri_count == 0, else first triangle index
        uint32_t right_child = 0;  // right child index if tri_count == 0
        uint32_t tri_count = 0;
    };

    struct PrimInfo {
        RtAabb bounds;
        Vec3 centroid;
    };

    uint32_t BuildRecursive(uint32_t first,
                            uint32_t count,
                            uint32_t depth,
                            const std::vector<PrimInfo>& prims);

    Vec3 TraceColor(const RtRay& ray,
                    const Vec3& light_pos,
                    uint32_t depth,
                    RtStats* stats,
                    bool is_primary = false) const;

    std::vector<RtTriangle> triangles_;
    std::vector<uint32_t> tri_indices_;
    std::vector<BvhNode> nodes_;
    uint32_t leaf_node_count_ = 0;
    uint32_t max_tree_depth_ = 0;
};

// Creates a deterministic 3D Cornell-style test scene (reflective floor + colored walls + pyramid).
std::vector<RtTriangle> BuildDefaultRtTestScene();

// Creates a 12-triangle axis-aligned unit cube centered at the origin ([-0.5, 0.5]^3).
std::vector<RtTriangle> BuildUnitCubeMesh(Vec3 color, float reflectivity = 0.25f);

}  // namespace openscarlett
