#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rt_bvh.h"

namespace openscarlett {

// AMD RDNA2 (GFX10.3) BVH4 architecture constants.
// Nodes are 64-byte or 128-byte aligned; 32-bit node pointers tag the low 3 bits
// with the node type while bits [31:3] encode the 64-byte-shifted node byte offset:
//   byte_offset = uint64_t(ptr & ~0x7u) << 3   (== (ptr >> 3) * 64)
static constexpr uint32_t kRdna2BvhMagic = 0x34485642u;  // 'BVH4' little-endian
static constexpr uint32_t kRdna2BvhVersion = 1u;
static constexpr uint32_t kRdna2BvhHeaderSize = 256u;
static constexpr uint32_t kRdna2BvhNodeAlignment = 64u;
static constexpr uint32_t kRdna2BvhBox16Size = 64u;
static constexpr uint32_t kRdna2BvhBox32Size = 128u;
static constexpr uint32_t kRdna2BvhTriangleNodeSize = 64u;
static constexpr uint32_t kRdna2BvhInstanceNodeSize = 128u;
static constexpr uint32_t kRdna2BvhProceduralNodeSize = 64u;
static constexpr uint32_t kRdna2BvhInvalidNodePtr = 0xFFFFFFFFu;
static constexpr uint32_t kRdna2BvhMaxDepth = 64u;

// 3-bit low pointer tag matching AMD RDNA2 GFX10.3 image_bvh_intersect_ray node types.
enum class Rdna2BvhNodeType : uint8_t {
    Triangle0 = 0,   // 64B triangle leaf node (slot 0 / primary)
    Triangle1 = 1,   // 64B triangle leaf node (slot 1)
    Triangle2 = 2,   // 64B triangle leaf node (slot 2)
    Triangle3 = 3,   // 64B triangle leaf node (slot 3)
    Box16 = 4,       // 64B 4-wide internal node with FP16 child AABBs
    Box32 = 5,       // 128B 4-wide internal node with FP32 child AABBs
    Instance = 6,    // 128B TLAS instance leaf node (world<->object 3x4 + BLAS pointer)
    Procedural = 7,  // 64B procedural / custom AABB leaf node
};

enum class Rdna2BvhAsType : uint32_t {
    TopLevel = 0,
    BottomLevel = 1,
};

// 256-byte aligned RDNA2 BVH4 Acceleration Structure header.
struct Rdna2BvhHeader {
    uint32_t magic = kRdna2BvhMagic;
    uint32_t version = kRdna2BvhVersion;
    Rdna2BvhAsType as_type = Rdna2BvhAsType::BottomLevel;
    uint32_t build_flags = 0;
    uint32_t total_size_bytes = 0;
    uint32_t header_size_bytes = kRdna2BvhHeaderSize;
    uint32_t nodes_offset_bytes = kRdna2BvhHeaderSize;
    uint32_t nodes_size_bytes = 0;
    uint32_t root_node_ptr = kRdna2BvhInvalidNodePtr;
    uint32_t internal_node_count = 0;
    uint32_t leaf_node_count = 0;
    uint32_t primitive_count = 0;
    uint32_t geometry_count = 1;
    uint32_t max_depth = 0;
    RtAabb root_aabb{};
    uint32_t payload_crc32 = 0;
};

// 64-byte RDNA2 BVH4 Box16 internal node (4x child ptr + 4x FP16 AABB).
struct Rdna2BvhBox16Node {
    std::array<uint32_t, 4> children = {
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
    };
    // Per child: [min_x, min_y, min_z, max_x, max_y, max_z] encoded in IEEE-754 binary16.
    std::array<std::array<uint16_t, 6>, 4> bbox_fp16{};
};

// 128-byte RDNA2 BVH4 Box32 internal node (4x child ptr + 4x FP32 AABB + metadata).
struct Rdna2BvhBox32Node {
    std::array<uint32_t, 4> children = {
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
    };
    std::array<RtAabb, 4> bbox{};
    uint32_t parent_ptr = kRdna2BvhInvalidNodePtr;
    uint32_t child_count = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

// 64-byte RDNA2 Triangle leaf node (supports 1 or 2 triangles sharing vertices).
struct Rdna2BvhTriangleNode {
    Vec3 v0{};
    Vec3 v1{};
    Vec3 v2{};
    Vec3 v3{};
    uint32_t triangle_id = 0;  // bits [1:0] = triangle count (1 or 2), bits [31:2] = flags
    uint32_t primitive_index = 0;
    uint32_t geometry_index = 0;   // 24-bit geometry index
    uint32_t geometry_flags = 0;   // 8-bit geometry flags (e.g. opaque)
    uint32_t extra_index = 0;      // secondary primitive index if triangle_count == 2
};

// 128-byte RDNA2 TLAS Instance leaf node.
struct Rdna2BvhInstanceNode {
    uint64_t blas_base_va = 0;
    uint32_t blas_root_ptr = kRdna2BvhInvalidNodePtr;
    uint32_t instance_id = 0;         // 24-bit user ID
    uint32_t instance_mask = 0xFFu;   // 8-bit visibility mask
    uint32_t sbt_offset = 0;          // 24-bit hit group contribution
    uint32_t instance_flags = 0;      // 8-bit D3D12 instance flags
    uint32_t instance_index = 0;      // sequential index in TLAS
    float world_to_object[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    float object_to_world[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
};

// 64-byte RDNA2 Procedural / AABB leaf node.
struct Rdna2BvhProceduralNode {
    RtAabb bounds{};
    uint32_t primitive_index = 0;
    uint32_t geometry_index = 0;
    uint32_t geometry_flags = 0;
    std::array<uint32_t, 8> user_data{};
};

struct Rdna2BvhValidationReport {
    uint32_t visited_internal_nodes = 0;
    uint32_t visited_box16_nodes = 0;
    uint32_t visited_box32_nodes = 0;
    uint32_t visited_triangle_nodes = 0;
    uint32_t visited_instance_nodes = 0;
    uint32_t visited_procedural_nodes = 0;
    uint32_t max_depth_observed = 0;
    RtAabb computed_root_aabb{};
};

struct Rdna2BvhEncodeOptions {
    bool use_box16 = false;
    uint32_t build_flags = 0;
    uint32_t geometry_index = 0;
    uint32_t geometry_flags = 1u;  // opaque by default
};

struct Rdna2BvhTlasInstanceInput {
    uint64_t blas_base_va = 0;
    uint32_t blas_root_ptr = kRdna2BvhInvalidNodePtr;
    RtAabb blas_local_aabb{};
    float object_to_world[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    uint32_t instance_id = 0;
    uint32_t instance_mask = 0xFFu;
    uint32_t sbt_offset = 0;
    uint32_t instance_flags = 0;
};

struct Rdna2BvhProceduralInput {
    RtAabb bounds{};
    uint32_t primitive_index = 0;
    uint32_t geometry_index = 0;
    uint32_t geometry_flags = 0;
    std::array<uint32_t, 8> user_data{};
};

// ---------------------------------------------------------------------------
// Node pointer & IEEE-754 binary16 AABB helpers
// ---------------------------------------------------------------------------
uint32_t EncodeRdna2NodePtr(uint64_t byte_offset, Rdna2BvhNodeType type) noexcept;
Rdna2BvhNodeType DecodeRdna2NodeType(uint32_t node_ptr) noexcept;
uint64_t DecodeRdna2NodeByteOffset(uint32_t node_ptr) noexcept;
uint32_t Rdna2NodeByteSize(Rdna2BvhNodeType type) noexcept;
bool IsRdna2NodePtrValid(uint32_t node_ptr) noexcept;
bool IsRdna2TriangleNodeType(Rdna2BvhNodeType type) noexcept;
bool IsRdna2InternalNodeType(Rdna2BvhNodeType type) noexcept;

uint16_t Float32ToFp16RoundDown(float v) noexcept;
uint16_t Float32ToFp16RoundUp(float v) noexcept;
float Fp16ToFloat32(uint16_t h) noexcept;
RtAabb DecodeBox16ChildAabb(const Rdna2BvhBox16Node& node, uint32_t child_index) noexcept;
void EncodeBox16ChildAabb(const RtAabb& box, std::array<uint16_t, 6>& out_fp16) noexcept;

// ---------------------------------------------------------------------------
// Header & single-node binary encoders/decoders
// ---------------------------------------------------------------------------
Status EncodeRdna2BvhHeader(const Rdna2BvhHeader& header, uint8_t* dst, size_t dst_size);
Status DecodeRdna2BvhHeader(const uint8_t* data, size_t size, Rdna2BvhHeader& out_header);

Status EncodeRdna2Box16Node(const Rdna2BvhBox16Node& node, uint8_t* dst, size_t dst_size);
Status DecodeRdna2Box16Node(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhBox16Node& out_node);

Status EncodeRdna2Box32Node(const Rdna2BvhBox32Node& node, uint8_t* dst, size_t dst_size);
Status DecodeRdna2Box32Node(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhBox32Node& out_node);

Status EncodeRdna2TriangleNode(const Rdna2BvhTriangleNode& node, uint8_t* dst, size_t dst_size);
Status DecodeRdna2TriangleNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhTriangleNode& out_node);

Status EncodeRdna2InstanceNode(const Rdna2BvhInstanceNode& node, uint8_t* dst, size_t dst_size);
Status DecodeRdna2InstanceNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhInstanceNode& out_node);

Status EncodeRdna2ProceduralNode(const Rdna2BvhProceduralNode& node, uint8_t* dst, size_t dst_size);
Status DecodeRdna2ProceduralNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhProceduralNode& out_node);

// ---------------------------------------------------------------------------
// End-to-end BVH4 encoding, full tree validation, and ray-traversal decoder
// ---------------------------------------------------------------------------
// Collapses a binary SAH BvhAccelerationStructure into a 4-wide RDNA2 BVH4 BLAS buffer.
Status EncodeBvh2ToRdna2Bvh4(
    const BvhAccelerationStructure& bvh2,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& out_buffer,
    Rdna2BvhHeader* out_header = nullptr);

// Builds a 4-wide RDNA2 BVH4 TLAS buffer containing instance and/or procedural leaf nodes.
Status BuildRdna2Bvh4Tlas(
    const std::vector<Rdna2BvhTlasInstanceInput>& instances,
    const std::vector<Rdna2BvhProceduralInput>& procedurals,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& out_buffer,
    Rdna2BvhHeader* out_header = nullptr);

// Strictly validates an RDNA2 BVH4 buffer: header, alignment, CRC32, pointer bounds,
// cycle detection, depth limits, AABB finiteness/containment, and node counts.
Status ValidateRdna2Bvh4(
    const uint8_t* data,
    size_t size,
    Rdna2BvhValidationReport* out_report = nullptr);

// Validated RDNA2 BVH4 decoded acceleration structure capable of direct BVH4 ray traversal.
class Rdna2Bvh4Tree {
public:
    Status LoadFromBuffer(const uint8_t* data, size_t size);

    // Optional attachment of original triangle color/reflectivity metadata for full RtHit parity.
    void AttachTriangleMetadata(const std::vector<RtTriangle>& triangles);

    bool TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats = nullptr) const;
    bool TraceShadowRay(const RtRay& ray, RtStats* stats = nullptr) const;

    const Rdna2BvhHeader& Header() const noexcept { return header_; }
    const Rdna2BvhValidationReport& ValidationReport() const noexcept { return report_; }
    bool IsLoaded() const noexcept { return !buffer_.empty(); }

private:
    std::vector<uint8_t> buffer_;
    Rdna2BvhHeader header_{};
    Rdna2BvhValidationReport report_{};
    std::vector<RtTriangle> triangle_meta_;
};

}  // namespace openscarlett
