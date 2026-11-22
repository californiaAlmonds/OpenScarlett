#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace openscarlett {

namespace {

uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t ReadLe64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(ReadLe32(p)) |
           (static_cast<uint64_t>(ReadLe32(p + 4)) << 32u);
}

float ReadLeF32(const uint8_t* p) noexcept {
    const uint32_t bits = ReadLe32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

void WriteLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

void WriteLe64(uint8_t* p, uint64_t v) noexcept {
    WriteLe32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    WriteLe32(p + 4, static_cast<uint32_t>((v >> 32u) & 0xFFFFFFFFu));
}

void WriteLeF32(uint8_t* p, float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(uint32_t));
    WriteLe32(p, bits);
}

uint32_t ComputeBvhCrc32(const uint8_t* data, size_t size) noexcept {
    static const std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
            }
            t[i] = c;
        }
        return t;
    }();
    if (!data || size == 0) {
        return 0u;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8u);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool IsVec3Finite(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsAabbValid(const RtAabb& b) noexcept {
    return IsVec3Finite(b.min) && IsVec3Finite(b.max) &&
           b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

bool IsMatrix3x4Finite(const float m[3][4]) noexcept {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(m[r][c])) {
                return false;
            }
        }
    }
    return true;
}

bool AabbEncloses(const RtAabb& outer, const RtAabb& inner, float eps) noexcept {
    const float sx = std::max(1.0f, std::max(std::fabs(outer.min.x), std::fabs(outer.max.x))) * eps;
    const float sy = std::max(1.0f, std::max(std::fabs(outer.min.y), std::fabs(outer.max.y))) * eps;
    const float sz = std::max(1.0f, std::max(std::fabs(outer.min.z), std::fabs(outer.max.z))) * eps;
    return outer.min.x - sx <= inner.min.x && outer.max.x + sx >= inner.max.x &&
           outer.min.y - sy <= inner.min.y && outer.max.y + sy >= inner.max.y &&
           outer.min.z - sz <= inner.min.z && outer.max.z + sz >= inner.max.z;
}

uint16_t Float32ToFp16Directed(float v, bool round_up) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(uint32_t));
    const uint32_t sign = (bits >> 31u) & 1u;
    const uint32_t exp = (bits >> 23u) & 0xFFu;
    const uint32_t mant = bits & 0x7FFFFFu;

    if (exp == 0xFFu) {
        if (mant != 0u) {
            return 0x7E00u;  // canonical quiet NaN
        }
        return static_cast<uint16_t>((sign << 15u) | 0x7C00u);  // +/- Inf
    }
    if (exp == 0u && mant == 0u) {
        return 0u;  // exact 0
    }

    const bool round_away = (round_up && sign == 0u) || (!round_up && sign != 0u);
    const int32_t e = static_cast<int32_t>(exp) - 127;

    if (e > 15) {
        return static_cast<uint16_t>((sign << 15u) | (round_away ? 0x7C00u : 0x7BFFu));
    }

    if (e >= -14) {
        uint32_t h = (static_cast<uint32_t>(e + 15) << 10u) | (mant >> 13u);
        const uint32_t rem = mant & 0x1FFFu;
        if (round_away && rem != 0u) {
            ++h;
        }
        return static_cast<uint16_t>((sign << 15u) | h);
    }

    if (e >= -24 && exp != 0u) {
        const uint32_t sig = (1u << 23u) | mant;
        const uint32_t shift = static_cast<uint32_t>(-1 - e);  // 14..23
        uint32_t h = sig >> shift;
        const uint32_t rem = sig & ((1u << shift) - 1u);
        if (round_away && rem != 0u) {
            ++h;
        }
        return static_cast<uint16_t>((sign << 15u) | h);
    }

    // Subnormal float32 or magnitude < 2^-24
    if (round_away) {
        return static_cast<uint16_t>((sign << 15u) | 0x0001u);
    }
    return 0u;
}

RtAabb TransformAabb3x4(const float m[3][4], const RtAabb& local) noexcept {
    RtAabb world{};
    const float xs[2] = {local.min.x, local.max.x};
    const float ys[2] = {local.min.y, local.max.y};
    const float zs[2] = {local.min.z, local.max.z};
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            for (int iz = 0; iz < 2; ++iz) {
                world.Expand(TransformPoint3x4(m, {xs[ix], ys[iy], zs[iz]}));
            }
        }
    }
    return world;
}

uint32_t AllocateNodeBytes(std::vector<uint8_t>& buf, uint32_t node_size) {
    const size_t aligned = (buf.size() + kRdna2BvhNodeAlignment - 1u) &
                           ~static_cast<size_t>(kRdna2BvhNodeAlignment - 1u);
    buf.resize(aligned + node_size, 0u);
    return static_cast<uint32_t>(aligned);
}

struct Bvh4EmitResult {
    uint32_t node_ptr = kRdna2BvhInvalidNodePtr;
    RtAabb bounds{};
    uint32_t max_depth = 0;
};

struct Bvh4BuildCounters {
    uint32_t internal_nodes = 0;
    uint32_t leaf_nodes = 0;
};

Bvh4EmitResult EmitSubtreeFromBvh2(
    const BvhAccelerationStructure& bvh2,
    uint32_t bvh2_node_idx,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& buf,
    Bvh4BuildCounters& counters);

Bvh4EmitResult EmitTriangleListAsBvh4(
    const BvhAccelerationStructure& bvh2,
    uint32_t first,
    uint32_t count,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& buf,
    Bvh4BuildCounters& counters) {
    const auto& tris = bvh2.Triangles();
    const auto& tri_indices = bvh2.TriangleIndices();

    if (count == 1u) {
        const uint32_t tri_idx = tri_indices[first];
        const RtTriangle& t = tris[tri_idx];
        Rdna2BvhTriangleNode tnode{};
        tnode.v0 = t.v0;
        tnode.v1 = t.v1;
        tnode.v2 = t.v2;
        tnode.v3 = Vec3{};
        tnode.triangle_id = 1u;
        tnode.primitive_index = tri_idx;
        tnode.geometry_index = options.geometry_index;
        tnode.geometry_flags = options.geometry_flags;
        tnode.extra_index = 0u;

        const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhTriangleNodeSize);
        EncodeRdna2TriangleNode(tnode, buf.data() + offset, kRdna2BvhTriangleNodeSize);
        ++counters.leaf_nodes;

        RtAabb box{};
        box.Expand(t.v0);
        box.Expand(t.v1);
        box.Expand(t.v2);
        return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Triangle0), box, 1u};
    }

    // Group 2..N leaf triangles into 4-wide internal node(s)
    const uint32_t groups = std::min<uint32_t>(4u, count);
    std::array<Bvh4EmitResult, 4> children{};
    RtAabb combined{};
    uint32_t max_child_depth = 0;

    uint32_t cursor = first;
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t remaining_tris = (first + count) - cursor;
        const uint32_t remaining_groups = groups - g;
        const uint32_t chunk = (remaining_tris + remaining_groups - 1u) / remaining_groups;
        children[g] = EmitTriangleListAsBvh4(bvh2, cursor, chunk, options, buf, counters);
        combined.Expand(children[g].bounds);
        max_child_depth = std::max(max_child_depth, children[g].max_depth);
        cursor += chunk;
    }

    ++counters.internal_nodes;
    if (options.use_box16) {
        Rdna2BvhBox16Node box16{};
        RtAabb quantized_combined{};
        for (uint32_t g = 0; g < groups; ++g) {
            box16.children[g] = children[g].node_ptr;
            EncodeBox16ChildAabb(children[g].bounds, box16.bbox_fp16[g]);
            quantized_combined.Expand(DecodeBox16ChildAabb(box16, g));
        }
        const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox16Size);
        EncodeRdna2Box16Node(box16, buf.data() + offset, kRdna2BvhBox16Size);
        return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box16), quantized_combined, max_child_depth + 1u};
    }

    Rdna2BvhBox32Node box32{};
    box32.child_count = groups;
    for (uint32_t g = 0; g < groups; ++g) {
        box32.children[g] = children[g].node_ptr;
        box32.bbox[g] = children[g].bounds;
    }
    const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox32Size);
    EncodeRdna2Box32Node(box32, buf.data() + offset, kRdna2BvhBox32Size);
    return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box32), combined, max_child_depth + 1u};
}

Bvh4EmitResult EmitSubtreeFromBvh2(
    const BvhAccelerationStructure& bvh2,
    uint32_t bvh2_node_idx,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& buf,
    Bvh4BuildCounters& counters) {
    BvhNodeView node{};
    if (!bvh2.GetNodeView(bvh2_node_idx, node)) {
        return {};
    }
    if (node.tri_count > 0u) {
        return EmitTriangleListAsBvh4(bvh2, node.left_first, node.tri_count, options, buf, counters);
    }

    // Classic SAH-preserving 2-to-4 wide collapse: repeatedly expand the internal child
    // with the largest surface area until we have up to 4 children or only leaves remain.
    std::vector<uint32_t> wide_children;
    wide_children.reserve(4u);
    wide_children.push_back(node.left_first);
    wide_children.push_back(node.right_child);

    while (wide_children.size() < 4u) {
        int best_slot = -1;
        float best_area = -1.0f;
        for (size_t i = 0; i < wide_children.size(); ++i) {
            BvhNodeView cv{};
            if (bvh2.GetNodeView(wide_children[i], cv) && cv.tri_count == 0u) {
                const float area = cv.bounds.SurfaceArea();
                if (area > best_area) {
                    best_area = area;
                    best_slot = static_cast<int>(i);
                }
            }
        }
        if (best_slot < 0) {
            break;
        }
        BvhNodeView expand_view{};
        bvh2.GetNodeView(wide_children[static_cast<size_t>(best_slot)], expand_view);
        wide_children[static_cast<size_t>(best_slot)] = expand_view.left_first;
        wide_children.push_back(expand_view.right_child);
    }

    const uint32_t child_count = static_cast<uint32_t>(wide_children.size());
    std::array<Bvh4EmitResult, 4> emitted{};
    RtAabb combined{};
    uint32_t max_child_depth = 0;

    for (uint32_t c = 0; c < child_count; ++c) {
        emitted[c] = EmitSubtreeFromBvh2(bvh2, wide_children[c], options, buf, counters);
        combined.Expand(emitted[c].bounds);
        max_child_depth = std::max(max_child_depth, emitted[c].max_depth);
    }

    ++counters.internal_nodes;
    if (options.use_box16) {
        Rdna2BvhBox16Node box16{};
        RtAabb quantized_combined{};
        for (uint32_t c = 0; c < child_count; ++c) {
            box16.children[c] = emitted[c].node_ptr;
            EncodeBox16ChildAabb(emitted[c].bounds, box16.bbox_fp16[c]);
            quantized_combined.Expand(DecodeBox16ChildAabb(box16, c));
        }
        const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox16Size);
        EncodeRdna2Box16Node(box16, buf.data() + offset, kRdna2BvhBox16Size);
        return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box16), quantized_combined, max_child_depth + 1u};
    }

    Rdna2BvhBox32Node box32{};
    box32.child_count = child_count;
    for (uint32_t c = 0; c < child_count; ++c) {
        box32.children[c] = emitted[c].node_ptr;
        box32.bbox[c] = emitted[c].bounds;
    }
    const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox32Size);
    EncodeRdna2Box32Node(box32, buf.data() + offset, kRdna2BvhBox32Size);
    return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box32), combined, max_child_depth + 1u};
}

struct TlasLeafRecord {
    uint32_t node_ptr = kRdna2BvhInvalidNodePtr;
    RtAabb bounds{};
    Vec3 centroid{};
};

Bvh4EmitResult BuildTlasHierarchy(
    std::vector<TlasLeafRecord>& leaves,
    size_t first,
    size_t count,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& buf,
    Bvh4BuildCounters& counters) {
    if (count == 1u) {
        return {leaves[first].node_ptr, leaves[first].bounds, 1u};
    }

    RtAabb bounds{};
    for (size_t i = 0; i < count; ++i) {
        bounds.Expand(leaves[first + i].bounds);
    }

    // Sort along longest axis for spatially coherent 4-way partitioning
    const float ex = bounds.max.x - bounds.min.x;
    const float ey = bounds.max.y - bounds.min.y;
    const float ez = bounds.max.z - bounds.min.z;
    int axis = 0;
    if (ey > ex) axis = 1;
    if (ez > (axis == 0 ? ex : ey)) axis = 2;

    std::sort(leaves.begin() + static_cast<ptrdiff_t>(first),
              leaves.begin() + static_cast<ptrdiff_t>(first + count),
              [axis](const TlasLeafRecord& a, const TlasLeafRecord& b) {
                  const float ca = axis == 0 ? a.centroid.x : (axis == 1 ? a.centroid.y : a.centroid.z);
                  const float cb = axis == 0 ? b.centroid.x : (axis == 1 ? b.centroid.y : b.centroid.z);
                  return ca < cb;
              });

    const uint32_t groups = static_cast<uint32_t>(std::min<size_t>(4u, count));
    std::array<Bvh4EmitResult, 4> children{};
    RtAabb combined{};
    uint32_t max_child_depth = 0;
    size_t cursor = first;

    for (uint32_t g = 0; g < groups; ++g) {
        const size_t rem_items = (first + count) - cursor;
        const size_t rem_groups = groups - g;
        const size_t chunk = (rem_items + rem_groups - 1u) / rem_groups;
        children[g] = BuildTlasHierarchy(leaves, cursor, chunk, options, buf, counters);
        combined.Expand(children[g].bounds);
        max_child_depth = std::max(max_child_depth, children[g].max_depth);
        cursor += chunk;
    }

    ++counters.internal_nodes;
    if (options.use_box16) {
        Rdna2BvhBox16Node box16{};
        RtAabb quantized_combined{};
        for (uint32_t g = 0; g < groups; ++g) {
            box16.children[g] = children[g].node_ptr;
            EncodeBox16ChildAabb(children[g].bounds, box16.bbox_fp16[g]);
            quantized_combined.Expand(DecodeBox16ChildAabb(box16, g));
        }
        const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox16Size);
        EncodeRdna2Box16Node(box16, buf.data() + offset, kRdna2BvhBox16Size);
        return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box16), quantized_combined, max_child_depth + 1u};
    }

    Rdna2BvhBox32Node box32{};
    box32.child_count = groups;
    for (uint32_t g = 0; g < groups; ++g) {
        box32.children[g] = children[g].node_ptr;
        box32.bbox[g] = children[g].bounds;
    }
    const uint32_t offset = AllocateNodeBytes(buf, kRdna2BvhBox32Size);
    EncodeRdna2Box32Node(box32, buf.data() + offset, kRdna2BvhBox32Size);
    return {EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Box32), combined, max_child_depth + 1u};
}

Status ValidateSubtreeRecursive(
    const uint8_t* data,
    size_t size,
    const Rdna2BvhHeader& hdr,
    uint32_t node_ptr,
    uint32_t depth,
    std::unordered_set<uint64_t>& on_stack,
    std::unordered_set<uint64_t>& visited,
    Rdna2BvhValidationReport& report,
    RtAabb& out_computed_aabb) {
    if (!IsRdna2NodePtrValid(node_ptr)) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 encountered invalid sentinel node pointer");
    }
    if (depth > kRdna2BvhMaxDepth) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 exceeds maximum tree depth");
    }

    const Rdna2BvhNodeType type = DecodeRdna2NodeType(node_ptr);
    const uint64_t byte_offset = DecodeRdna2NodeByteOffset(node_ptr);
    const uint32_t node_size = Rdna2NodeByteSize(type);
    const uint64_t nodes_end = static_cast<uint64_t>(hdr.nodes_offset_bytes) +
                               static_cast<uint64_t>(hdr.nodes_size_bytes);

    if (byte_offset < hdr.nodes_offset_bytes ||
        byte_offset + node_size > nodes_end ||
        byte_offset + node_size > size) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 node pointer out of node buffer bounds");
    }

    if (on_stack.count(byte_offset) != 0u) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 cycle detected in node graph");
    }
    if (visited.count(byte_offset) != 0u) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 aliased/shared node pointer detected");
    }

    on_stack.insert(byte_offset);
    visited.insert(byte_offset);
    report.max_depth_observed = std::max(report.max_depth_observed, depth);

    Status st = Status::Ok();
    out_computed_aabb = RtAabb{};

    if (type == Rdna2BvhNodeType::Box32) {
        Rdna2BvhBox32Node box32{};
        st = DecodeRdna2Box32Node(data, size, node_ptr, box32);
        if (st.ok()) {
            ++report.visited_internal_nodes;
            ++report.visited_box32_nodes;
            uint32_t valid_children = 0;
            for (uint32_t c = 0; c < 4u; ++c) {
                const uint32_t cptr = box32.children[c];
                if (!IsRdna2NodePtrValid(cptr)) {
                    continue;
                }
                ++valid_children;
                if (!IsAabbValid(box32.bbox[c])) {
                    st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Box32 child AABB invalid/NaN");
                    break;
                }
                RtAabb child_computed{};
                st = ValidateSubtreeRecursive(data, size, hdr, cptr, depth + 1u,
                                              on_stack, visited, report, child_computed);
                if (!st.ok()) {
                    break;
                }
                // For instance leaves whose local bounds aren't stored in the leaf itself, trust slot AABB
                if (DecodeRdna2NodeType(cptr) != Rdna2BvhNodeType::Instance) {
                    if (!AabbEncloses(box32.bbox[c], child_computed, 1e-4f)) {
                        st = Status::Error(StatusCode::ParseError,
                                           "RDNA2 BVH4 Box32 child AABB fails to enclose subtree bounds");
                        break;
                    }
                }
                out_computed_aabb.Expand(box32.bbox[c]);
            }
            if (st.ok() && valid_children == 0u) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Box32 node has zero valid children");
            }
            if (st.ok() && box32.child_count != 0u && box32.child_count != valid_children) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Box32 child_count metadata mismatch");
            }
        }
    } else if (type == Rdna2BvhNodeType::Box16) {
        Rdna2BvhBox16Node box16{};
        st = DecodeRdna2Box16Node(data, size, node_ptr, box16);
        if (st.ok()) {
            ++report.visited_internal_nodes;
            ++report.visited_box16_nodes;
            uint32_t valid_children = 0;
            for (uint32_t c = 0; c < 4u; ++c) {
                const uint32_t cptr = box16.children[c];
                if (!IsRdna2NodePtrValid(cptr)) {
                    continue;
                }
                ++valid_children;
                const RtAabb slot_box = DecodeBox16ChildAabb(box16, c);
                if (!IsAabbValid(slot_box)) {
                    st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Box16 child AABB invalid/NaN");
                    break;
                }
                RtAabb child_computed{};
                st = ValidateSubtreeRecursive(data, size, hdr, cptr, depth + 1u,
                                              on_stack, visited, report, child_computed);
                if (!st.ok()) {
                    break;
                }
                if (DecodeRdna2NodeType(cptr) != Rdna2BvhNodeType::Instance) {
                    if (!AabbEncloses(slot_box, child_computed, 1e-3f)) {
                        st = Status::Error(StatusCode::ParseError,
                                           "RDNA2 BVH4 Box16 child AABB fails to enclose subtree bounds");
                        break;
                    }
                }
                out_computed_aabb.Expand(slot_box);
            }
            if (st.ok() && valid_children == 0u) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Box16 node has zero valid children");
            }
        }
    } else if (IsRdna2TriangleNodeType(type)) {
        Rdna2BvhTriangleNode tnode{};
        st = DecodeRdna2TriangleNode(data, size, node_ptr, tnode);
        if (st.ok()) {
            const uint32_t tri_count = tnode.triangle_id & 0x3u;
            if (tri_count == 0u || tri_count > 2u) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Triangle node has invalid triangle count");
            } else if (!IsVec3Finite(tnode.v0) || !IsVec3Finite(tnode.v1) || !IsVec3Finite(tnode.v2) ||
                       (tri_count == 2u && !IsVec3Finite(tnode.v3))) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Triangle node contains non-finite vertex");
            } else {
                ++report.visited_triangle_nodes;
                out_computed_aabb.Expand(tnode.v0);
                out_computed_aabb.Expand(tnode.v1);
                out_computed_aabb.Expand(tnode.v2);
                if (tri_count == 2u) {
                    out_computed_aabb.Expand(tnode.v3);
                }
            }
        }
    } else if (type == Rdna2BvhNodeType::Instance) {
        Rdna2BvhInstanceNode inode{};
        st = DecodeRdna2InstanceNode(data, size, node_ptr, inode);
        if (st.ok()) {
            if (!IsMatrix3x4Finite(inode.world_to_object) || !IsMatrix3x4Finite(inode.object_to_world)) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Instance node has non-finite transform");
            } else if ((inode.blas_base_va & (kRdna2BvhNodeAlignment - 1u)) != 0u) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Instance BLAS VA not 64B-aligned");
            } else {
                ++report.visited_instance_nodes;
            }
        }
    } else if (type == Rdna2BvhNodeType::Procedural) {
        Rdna2BvhProceduralNode pnode{};
        st = DecodeRdna2ProceduralNode(data, size, node_ptr, pnode);
        if (st.ok()) {
            if (!IsAabbValid(pnode.bounds)) {
                st = Status::Error(StatusCode::ParseError, "RDNA2 BVH4 Procedural node AABB invalid");
            } else {
                ++report.visited_procedural_nodes;
                out_computed_aabb = pnode.bounds;
            }
        }
    } else {
        st = Status::Error(StatusCode::ParseError, "Unknown RDNA2 BVH4 node type");
    }

    on_stack.erase(byte_offset);
    return st;
}

}  // namespace

// ---------------------------------------------------------------------------
// Node pointer & FP16 utilities
// ---------------------------------------------------------------------------
uint32_t EncodeRdna2NodePtr(uint64_t byte_offset, Rdna2BvhNodeType type) noexcept {
    if ((byte_offset & (kRdna2BvhNodeAlignment - 1u)) != 0u) {
        return kRdna2BvhInvalidNodePtr;
    }
    const uint64_t slot = byte_offset >> 6u;
    // Reserve slot 0x1FFFFFFF so tagged pointer never aliases kRdna2BvhInvalidNodePtr (0xFFFFFFFF)
    if (slot >= 0x1FFFFFFFull) {
        return kRdna2BvhInvalidNodePtr;
    }
    return static_cast<uint32_t>((slot << 3u) | (static_cast<uint32_t>(type) & 0x7u));
}

Rdna2BvhNodeType DecodeRdna2NodeType(uint32_t node_ptr) noexcept {
    return static_cast<Rdna2BvhNodeType>(node_ptr & 0x7u);
}

uint64_t DecodeRdna2NodeByteOffset(uint32_t node_ptr) noexcept {
    return static_cast<uint64_t>(node_ptr & ~0x7u) << 3u;
}

uint32_t Rdna2NodeByteSize(Rdna2BvhNodeType type) noexcept {
    switch (type) {
        case Rdna2BvhNodeType::Box32:
        case Rdna2BvhNodeType::Instance:
            return kRdna2BvhBox32Size;
        case Rdna2BvhNodeType::Triangle0:
        case Rdna2BvhNodeType::Triangle1:
        case Rdna2BvhNodeType::Triangle2:
        case Rdna2BvhNodeType::Triangle3:
        case Rdna2BvhNodeType::Box16:
        case Rdna2BvhNodeType::Procedural:
            return kRdna2BvhBox16Size;
        default:
            return 0u;
    }
}

bool IsRdna2NodePtrValid(uint32_t node_ptr) noexcept {
    return node_ptr != kRdna2BvhInvalidNodePtr;
}

bool IsRdna2TriangleNodeType(Rdna2BvhNodeType type) noexcept {
    return static_cast<uint8_t>(type) <= static_cast<uint8_t>(Rdna2BvhNodeType::Triangle3);
}

bool IsRdna2InternalNodeType(Rdna2BvhNodeType type) noexcept {
    return type == Rdna2BvhNodeType::Box16 || type == Rdna2BvhNodeType::Box32;
}

uint16_t Float32ToFp16RoundDown(float v) noexcept {
    return Float32ToFp16Directed(v, false);
}

uint16_t Float32ToFp16RoundUp(float v) noexcept {
    return Float32ToFp16Directed(v, true);
}

float Fp16ToFloat32(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) >> 15u) & 1u;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10u) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;

    uint32_t f_bits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            f_bits = sign << 31u;
        } else {
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x400u) == 0u) {
                m <<= 1u;
                --e;
            }
            m &= 0x3FFu;
            f_bits = (sign << 31u) | (static_cast<uint32_t>(e + 127) << 23u) | (m << 13u);
        }
    } else if (exp == 31u) {
        f_bits = (sign << 31u) | 0x7F800000u | (mant << 13u);
    } else {
        f_bits = (sign << 31u) | ((exp + 112u) << 23u) | (mant << 13u);
    }

    float out = 0.0f;
    std::memcpy(&out, &f_bits, sizeof(float));
    return out;
}

RtAabb DecodeBox16ChildAabb(const Rdna2BvhBox16Node& node, uint32_t child_index) noexcept {
    RtAabb box{};
    if (child_index >= 4u) {
        return box;
    }
    const auto& b = node.bbox_fp16[child_index];
    box.min = {Fp16ToFloat32(b[0]), Fp16ToFloat32(b[1]), Fp16ToFloat32(b[2])};
    box.max = {Fp16ToFloat32(b[3]), Fp16ToFloat32(b[4]), Fp16ToFloat32(b[5])};
    return box;
}

void EncodeBox16ChildAabb(const RtAabb& box, std::array<uint16_t, 6>& out_fp16) noexcept {
    out_fp16[0] = Float32ToFp16RoundDown(box.min.x);
    out_fp16[1] = Float32ToFp16RoundDown(box.min.y);
    out_fp16[2] = Float32ToFp16RoundDown(box.min.z);
    out_fp16[3] = Float32ToFp16RoundUp(box.max.x);
    out_fp16[4] = Float32ToFp16RoundUp(box.max.y);
    out_fp16[5] = Float32ToFp16RoundUp(box.max.z);
}

// ---------------------------------------------------------------------------
// Header & single-node binary encoders/decoders
// ---------------------------------------------------------------------------
Status EncodeRdna2BvhHeader(const Rdna2BvhHeader& header, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhHeaderSize) {
        return Status::Error(StatusCode::InvalidArgument, "RDNA2 BVH4 header destination buffer too small");
    }
    std::memset(dst, 0, kRdna2BvhHeaderSize);
    WriteLe32(dst + 0, header.magic);
    WriteLe32(dst + 4, header.version);
    WriteLe32(dst + 8, static_cast<uint32_t>(header.as_type));
    WriteLe32(dst + 12, header.build_flags);
    WriteLe32(dst + 16, header.total_size_bytes);
    WriteLe32(dst + 20, header.header_size_bytes);
    WriteLe32(dst + 24, header.nodes_offset_bytes);
    WriteLe32(dst + 28, header.nodes_size_bytes);
    WriteLe32(dst + 32, header.root_node_ptr);
    WriteLe32(dst + 36, header.internal_node_count);
    WriteLe32(dst + 40, header.leaf_node_count);
    WriteLe32(dst + 44, header.primitive_count);
    WriteLe32(dst + 48, header.geometry_count);
    WriteLe32(dst + 52, header.max_depth);
    WriteLeF32(dst + 56, header.root_aabb.min.x);
    WriteLeF32(dst + 60, header.root_aabb.min.y);
    WriteLeF32(dst + 64, header.root_aabb.min.z);
    WriteLeF32(dst + 68, header.root_aabb.max.x);
    WriteLeF32(dst + 72, header.root_aabb.max.y);
    WriteLeF32(dst + 76, header.root_aabb.max.z);
    WriteLe32(dst + 80, header.payload_crc32);
    return Status::Ok();
}

Status DecodeRdna2BvhHeader(const uint8_t* data, size_t size, Rdna2BvhHeader& out_header) {
    out_header = Rdna2BvhHeader{};
    if (!data || size < kRdna2BvhHeaderSize) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 buffer too small for 256-byte header");
    }

    out_header.magic = ReadLe32(data + 0);
    if (out_header.magic != kRdna2BvhMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid RDNA2 BVH4 header magic (expected 'BVH4')");
    }
    out_header.version = ReadLe32(data + 4);
    if (out_header.version != kRdna2BvhVersion) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported RDNA2 BVH4 header version");
    }

    const uint32_t raw_as_type = ReadLe32(data + 8);
    if (raw_as_type > static_cast<uint32_t>(Rdna2BvhAsType::BottomLevel)) {
        return Status::Error(StatusCode::ParseError, "Invalid RDNA2 BVH4 AS type");
    }
    out_header.as_type = static_cast<Rdna2BvhAsType>(raw_as_type);
    out_header.build_flags = ReadLe32(data + 12);
    out_header.total_size_bytes = ReadLe32(data + 16);
    out_header.header_size_bytes = ReadLe32(data + 20);
    out_header.nodes_offset_bytes = ReadLe32(data + 24);
    out_header.nodes_size_bytes = ReadLe32(data + 28);
    out_header.root_node_ptr = ReadLe32(data + 32);
    out_header.internal_node_count = ReadLe32(data + 36);
    out_header.leaf_node_count = ReadLe32(data + 40);
    out_header.primitive_count = ReadLe32(data + 44);
    out_header.geometry_count = ReadLe32(data + 48);
    out_header.max_depth = ReadLe32(data + 52);
    out_header.root_aabb.min.x = ReadLeF32(data + 56);
    out_header.root_aabb.min.y = ReadLeF32(data + 60);
    out_header.root_aabb.min.z = ReadLeF32(data + 64);
    out_header.root_aabb.max.x = ReadLeF32(data + 68);
    out_header.root_aabb.max.y = ReadLeF32(data + 72);
    out_header.root_aabb.max.z = ReadLeF32(data + 76);
    out_header.payload_crc32 = ReadLe32(data + 80);

    if (out_header.header_size_bytes != kRdna2BvhHeaderSize) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 header_size_bytes must be 256");
    }
    if (out_header.nodes_offset_bytes < kRdna2BvhHeaderSize ||
        (out_header.nodes_offset_bytes % kRdna2BvhNodeAlignment) != 0u) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 nodes_offset_bytes alignment invalid");
    }
    if ((out_header.nodes_size_bytes % kRdna2BvhNodeAlignment) != 0u ||
        (out_header.total_size_bytes % kRdna2BvhNodeAlignment) != 0u) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 sizes must be 64-byte aligned");
    }
    if (static_cast<uint64_t>(out_header.nodes_offset_bytes) +
            static_cast<uint64_t>(out_header.nodes_size_bytes) >
        out_header.total_size_bytes) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 node region exceeds total_size_bytes");
    }
    if (out_header.total_size_bytes > size) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 buffer truncated relative to header total_size_bytes");
    }
    if (out_header.max_depth > kRdna2BvhMaxDepth) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 header max_depth exceeds limit");
    }
    if (!IsAabbValid(out_header.root_aabb)) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 header root AABB invalid/NaN");
    }

    return Status::Ok();
}

Status EncodeRdna2Box16Node(const Rdna2BvhBox16Node& node, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhBox16Size) {
        return Status::Error(StatusCode::InvalidArgument, "Box16 encode buffer too small");
    }
    for (uint32_t c = 0; c < 4u; ++c) {
        WriteLe32(dst + c * 4u, node.children[c]);
    }
    size_t off = 16u;
    for (uint32_t c = 0; c < 4u; ++c) {
        for (uint32_t k = 0; k < 6u; ++k) {
            WriteLe16(dst + off, node.bbox_fp16[c][k]);
            off += 2u;
        }
    }
    return Status::Ok();
}

Status DecodeRdna2Box16Node(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhBox16Node& out_node) {
    out_node = Rdna2BvhBox16Node{};
    if (!data || !IsRdna2NodePtrValid(node_ptr) || DecodeRdna2NodeType(node_ptr) != Rdna2BvhNodeType::Box16) {
        return Status::Error(StatusCode::ParseError, "Invalid Box16 node pointer");
    }
    const uint64_t byte_off = DecodeRdna2NodeByteOffset(node_ptr);
    if (byte_off + kRdna2BvhBox16Size > size) {
        return Status::Error(StatusCode::ParseError, "Box16 node out of buffer bounds");
    }
    const uint8_t* p = data + byte_off;
    for (uint32_t c = 0; c < 4u; ++c) {
        out_node.children[c] = ReadLe32(p + c * 4u);
    }
    size_t off = 16u;
    for (uint32_t c = 0; c < 4u; ++c) {
        for (uint32_t k = 0; k < 6u; ++k) {
            out_node.bbox_fp16[c][k] = ReadLe16(p + off);
            off += 2u;
        }
    }
    return Status::Ok();
}

Status EncodeRdna2Box32Node(const Rdna2BvhBox32Node& node, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhBox32Size) {
        return Status::Error(StatusCode::InvalidArgument, "Box32 encode buffer too small");
    }
    for (uint32_t c = 0; c < 4u; ++c) {
        WriteLe32(dst + c * 4u, node.children[c]);
    }
    size_t off = 16u;
    for (uint32_t c = 0; c < 4u; ++c) {
        WriteLeF32(dst + off + 0u, node.bbox[c].min.x);
        WriteLeF32(dst + off + 4u, node.bbox[c].min.y);
        WriteLeF32(dst + off + 8u, node.bbox[c].min.z);
        WriteLeF32(dst + off + 12u, node.bbox[c].max.x);
        WriteLeF32(dst + off + 16u, node.bbox[c].max.y);
        WriteLeF32(dst + off + 20u, node.bbox[c].max.z);
        off += 24u;
    }
    WriteLe32(dst + 112u, node.parent_ptr);
    WriteLe32(dst + 116u, node.child_count);
    WriteLe32(dst + 120u, node.flags);
    WriteLe32(dst + 124u, node.reserved);
    return Status::Ok();
}

Status DecodeRdna2Box32Node(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhBox32Node& out_node) {
    out_node = Rdna2BvhBox32Node{};
    if (!data || !IsRdna2NodePtrValid(node_ptr) || DecodeRdna2NodeType(node_ptr) != Rdna2BvhNodeType::Box32) {
        return Status::Error(StatusCode::ParseError, "Invalid Box32 node pointer");
    }
    const uint64_t byte_off = DecodeRdna2NodeByteOffset(node_ptr);
    if (byte_off + kRdna2BvhBox32Size > size) {
        return Status::Error(StatusCode::ParseError, "Box32 node out of buffer bounds");
    }
    const uint8_t* p = data + byte_off;
    for (uint32_t c = 0; c < 4u; ++c) {
        out_node.children[c] = ReadLe32(p + c * 4u);
    }
    size_t off = 16u;
    for (uint32_t c = 0; c < 4u; ++c) {
        out_node.bbox[c].min.x = ReadLeF32(p + off + 0u);
        out_node.bbox[c].min.y = ReadLeF32(p + off + 4u);
        out_node.bbox[c].min.z = ReadLeF32(p + off + 8u);
        out_node.bbox[c].max.x = ReadLeF32(p + off + 12u);
        out_node.bbox[c].max.y = ReadLeF32(p + off + 16u);
        out_node.bbox[c].max.z = ReadLeF32(p + off + 20u);
        off += 24u;
    }
    out_node.parent_ptr = ReadLe32(p + 112u);
    out_node.child_count = ReadLe32(p + 116u);
    out_node.flags = ReadLe32(p + 120u);
    out_node.reserved = ReadLe32(p + 124u);
    return Status::Ok();
}

Status EncodeRdna2TriangleNode(const Rdna2BvhTriangleNode& node, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhTriangleNodeSize) {
        return Status::Error(StatusCode::InvalidArgument, "Triangle node encode buffer too small");
    }
    WriteLeF32(dst + 0u, node.v0.x);
    WriteLeF32(dst + 4u, node.v0.y);
    WriteLeF32(dst + 8u, node.v0.z);
    WriteLeF32(dst + 12u, node.v1.x);
    WriteLeF32(dst + 16u, node.v1.y);
    WriteLeF32(dst + 20u, node.v1.z);
    WriteLeF32(dst + 24u, node.v2.x);
    WriteLeF32(dst + 28u, node.v2.y);
    WriteLeF32(dst + 32u, node.v2.z);
    WriteLeF32(dst + 36u, node.v3.x);
    WriteLeF32(dst + 40u, node.v3.y);
    WriteLeF32(dst + 44u, node.v3.z);
    WriteLe32(dst + 48u, node.triangle_id);
    WriteLe32(dst + 52u, node.primitive_index);
    const uint32_t geom_packed = (node.geometry_index & 0x00FFFFFFu) |
                                 ((node.geometry_flags & 0xFFu) << 24u);
    WriteLe32(dst + 56u, geom_packed);
    WriteLe32(dst + 60u, node.extra_index);
    return Status::Ok();
}

Status DecodeRdna2TriangleNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhTriangleNode& out_node) {
    out_node = Rdna2BvhTriangleNode{};
    if (!data || !IsRdna2NodePtrValid(node_ptr) || !IsRdna2TriangleNodeType(DecodeRdna2NodeType(node_ptr))) {
        return Status::Error(StatusCode::ParseError, "Invalid Triangle node pointer");
    }
    const uint64_t byte_off = DecodeRdna2NodeByteOffset(node_ptr);
    if (byte_off + kRdna2BvhTriangleNodeSize > size) {
        return Status::Error(StatusCode::ParseError, "Triangle node out of buffer bounds");
    }
    const uint8_t* p = data + byte_off;
    out_node.v0 = {ReadLeF32(p + 0u), ReadLeF32(p + 4u), ReadLeF32(p + 8u)};
    out_node.v1 = {ReadLeF32(p + 12u), ReadLeF32(p + 16u), ReadLeF32(p + 20u)};
    out_node.v2 = {ReadLeF32(p + 24u), ReadLeF32(p + 28u), ReadLeF32(p + 32u)};
    out_node.v3 = {ReadLeF32(p + 36u), ReadLeF32(p + 40u), ReadLeF32(p + 44u)};
    out_node.triangle_id = ReadLe32(p + 48u);
    out_node.primitive_index = ReadLe32(p + 52u);
    const uint32_t geom_packed = ReadLe32(p + 56u);
    out_node.geometry_index = geom_packed & 0x00FFFFFFu;
    out_node.geometry_flags = (geom_packed >> 24u) & 0xFFu;
    out_node.extra_index = ReadLe32(p + 60u);
    return Status::Ok();
}

Status EncodeRdna2InstanceNode(const Rdna2BvhInstanceNode& node, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhInstanceNodeSize) {
        return Status::Error(StatusCode::InvalidArgument, "Instance node encode buffer too small");
    }
    std::memset(dst, 0, kRdna2BvhInstanceNodeSize);
    WriteLe64(dst + 0u, node.blas_base_va);
    WriteLe32(dst + 8u, node.blas_root_ptr);
    const uint32_t id_mask = (node.instance_id & 0x00FFFFFFu) | ((node.instance_mask & 0xFFu) << 24u);
    const uint32_t sbt_flags = (node.sbt_offset & 0x00FFFFFFu) | ((node.instance_flags & 0xFFu) << 24u);
    WriteLe32(dst + 12u, id_mask);
    WriteLe32(dst + 16u, sbt_flags);
    WriteLe32(dst + 20u, node.instance_index);

    size_t off = 24u;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            WriteLeF32(dst + off, node.world_to_object[r][c]);
            off += 4u;
        }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            WriteLeF32(dst + off, node.object_to_world[r][c]);
            off += 4u;
        }
    }
    return Status::Ok();
}

Status DecodeRdna2InstanceNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhInstanceNode& out_node) {
    out_node = Rdna2BvhInstanceNode{};
    if (!data || !IsRdna2NodePtrValid(node_ptr) || DecodeRdna2NodeType(node_ptr) != Rdna2BvhNodeType::Instance) {
        return Status::Error(StatusCode::ParseError, "Invalid Instance node pointer");
    }
    const uint64_t byte_off = DecodeRdna2NodeByteOffset(node_ptr);
    if (byte_off + kRdna2BvhInstanceNodeSize > size) {
        return Status::Error(StatusCode::ParseError, "Instance node out of buffer bounds");
    }
    const uint8_t* p = data + byte_off;
    out_node.blas_base_va = ReadLe64(p + 0u);
    out_node.blas_root_ptr = ReadLe32(p + 8u);
    const uint32_t id_mask = ReadLe32(p + 12u);
    const uint32_t sbt_flags = ReadLe32(p + 16u);
    out_node.instance_id = id_mask & 0x00FFFFFFu;
    out_node.instance_mask = (id_mask >> 24u) & 0xFFu;
    out_node.sbt_offset = sbt_flags & 0x00FFFFFFu;
    out_node.instance_flags = (sbt_flags >> 24u) & 0xFFu;
    out_node.instance_index = ReadLe32(p + 20u);

    size_t off = 24u;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out_node.world_to_object[r][c] = ReadLeF32(p + off);
            off += 4u;
        }
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out_node.object_to_world[r][c] = ReadLeF32(p + off);
            off += 4u;
        }
    }
    return Status::Ok();
}

Status EncodeRdna2ProceduralNode(const Rdna2BvhProceduralNode& node, uint8_t* dst, size_t dst_size) {
    if (!dst || dst_size < kRdna2BvhProceduralNodeSize) {
        return Status::Error(StatusCode::InvalidArgument, "Procedural node encode buffer too small");
    }
    WriteLeF32(dst + 0u, node.bounds.min.x);
    WriteLeF32(dst + 4u, node.bounds.min.y);
    WriteLeF32(dst + 8u, node.bounds.min.z);
    WriteLeF32(dst + 12u, node.bounds.max.x);
    WriteLeF32(dst + 16u, node.bounds.max.y);
    WriteLeF32(dst + 20u, node.bounds.max.z);
    WriteLe32(dst + 24u, node.primitive_index);
    const uint32_t geom_packed = (node.geometry_index & 0x00FFFFFFu) |
                                 ((node.geometry_flags & 0xFFu) << 24u);
    WriteLe32(dst + 28u, geom_packed);
    for (size_t i = 0; i < 8u; ++i) {
        WriteLe32(dst + 32u + i * 4u, node.user_data[i]);
    }
    return Status::Ok();
}

Status DecodeRdna2ProceduralNode(const uint8_t* data, size_t size, uint32_t node_ptr, Rdna2BvhProceduralNode& out_node) {
    out_node = Rdna2BvhProceduralNode{};
    if (!data || !IsRdna2NodePtrValid(node_ptr) || DecodeRdna2NodeType(node_ptr) != Rdna2BvhNodeType::Procedural) {
        return Status::Error(StatusCode::ParseError, "Invalid Procedural node pointer");
    }
    const uint64_t byte_off = DecodeRdna2NodeByteOffset(node_ptr);
    if (byte_off + kRdna2BvhProceduralNodeSize > size) {
        return Status::Error(StatusCode::ParseError, "Procedural node out of buffer bounds");
    }
    const uint8_t* p = data + byte_off;
    out_node.bounds.min = {ReadLeF32(p + 0u), ReadLeF32(p + 4u), ReadLeF32(p + 8u)};
    out_node.bounds.max = {ReadLeF32(p + 12u), ReadLeF32(p + 16u), ReadLeF32(p + 20u)};
    out_node.primitive_index = ReadLe32(p + 24u);
    const uint32_t geom_packed = ReadLe32(p + 28u);
    out_node.geometry_index = geom_packed & 0x00FFFFFFu;
    out_node.geometry_flags = (geom_packed >> 24u) & 0xFFu;
    for (size_t i = 0; i < 8u; ++i) {
        out_node.user_data[i] = ReadLe32(p + 32u + i * 4u);
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// End-to-end BVH2 -> RDNA2 BVH4 Encoding & TLAS Building
// ---------------------------------------------------------------------------
Status EncodeBvh2ToRdna2Bvh4(
    const BvhAccelerationStructure& bvh2,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& out_buffer,
    Rdna2BvhHeader* out_header) {
    out_buffer.clear();
    if (bvh2.NodeCount() == 0u || bvh2.TriangleCount() == 0u) {
        return Status::Error(StatusCode::RaytracingError, "Cannot encode empty BvhAccelerationStructure to BVH4");
    }

    out_buffer.assign(kRdna2BvhHeaderSize, 0u);
    Bvh4BuildCounters counters{};
    const Bvh4EmitResult root = EmitSubtreeFromBvh2(bvh2, 0u, options, out_buffer, counters);
    if (!IsRdna2NodePtrValid(root.node_ptr)) {
        out_buffer.clear();
        return Status::Error(StatusCode::RaytracingError, "Failed emitting RDNA2 BVH4 nodes");
    }

    const size_t aligned_total = (out_buffer.size() + kRdna2BvhNodeAlignment - 1u) &
                                 ~static_cast<size_t>(kRdna2BvhNodeAlignment - 1u);
    out_buffer.resize(aligned_total, 0u);

    Rdna2BvhHeader hdr{};
    hdr.magic = kRdna2BvhMagic;
    hdr.version = kRdna2BvhVersion;
    hdr.as_type = Rdna2BvhAsType::BottomLevel;
    hdr.build_flags = options.build_flags;
    hdr.total_size_bytes = static_cast<uint32_t>(out_buffer.size());
    hdr.header_size_bytes = kRdna2BvhHeaderSize;
    hdr.nodes_offset_bytes = kRdna2BvhHeaderSize;
    hdr.nodes_size_bytes = static_cast<uint32_t>(out_buffer.size() - kRdna2BvhHeaderSize);
    hdr.root_node_ptr = root.node_ptr;
    hdr.internal_node_count = counters.internal_nodes;
    hdr.leaf_node_count = counters.leaf_nodes;
    hdr.primitive_count = bvh2.TriangleCount();
    hdr.geometry_count = 1u;
    hdr.max_depth = root.max_depth;
    hdr.root_aabb = root.bounds;
    hdr.payload_crc32 = ComputeBvhCrc32(out_buffer.data() + hdr.nodes_offset_bytes, hdr.nodes_size_bytes);

    Status st = EncodeRdna2BvhHeader(hdr, out_buffer.data(), out_buffer.size());
    if (!st.ok()) {
        out_buffer.clear();
        return st;
    }

    if (out_header) {
        *out_header = hdr;
    }
    return Status::Ok();
}

Status BuildRdna2Bvh4Tlas(
    const std::vector<Rdna2BvhTlasInstanceInput>& instances,
    const std::vector<Rdna2BvhProceduralInput>& procedurals,
    const Rdna2BvhEncodeOptions& options,
    std::vector<uint8_t>& out_buffer,
    Rdna2BvhHeader* out_header) {
    out_buffer.clear();
    if (instances.empty() && procedurals.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Cannot build RDNA2 BVH4 TLAS with zero leaves");
    }

    out_buffer.assign(kRdna2BvhHeaderSize, 0u);
    std::vector<TlasLeafRecord> leaves;
    leaves.reserve(instances.size() + procedurals.size());
    Bvh4BuildCounters counters{};

    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& in = instances[i];
        if (!IsAabbValid(in.blas_local_aabb) || !IsMatrix3x4Finite(in.object_to_world)) {
            out_buffer.clear();
            return Status::Error(StatusCode::InvalidArgument, "Invalid TLAS instance bounds or transform");
        }
        if ((in.blas_base_va & (kRdna2BvhNodeAlignment - 1u)) != 0u) {
            out_buffer.clear();
            return Status::Error(StatusCode::InvalidArgument, "TLAS instance BLAS VA must be 64-byte aligned");
        }

        Rdna2BvhInstanceNode inode{};
        inode.blas_base_va = in.blas_base_va;
        inode.blas_root_ptr = in.blas_root_ptr;
        inode.instance_id = in.instance_id;
        inode.instance_mask = in.instance_mask;
        inode.sbt_offset = in.sbt_offset;
        inode.instance_flags = in.instance_flags;
        inode.instance_index = static_cast<uint32_t>(i);
        std::memcpy(inode.object_to_world, in.object_to_world, sizeof(inode.object_to_world));
        if (!InvertTransform3x4(in.object_to_world, inode.world_to_object)) {
            out_buffer.clear();
            return Status::Error(StatusCode::InvalidArgument, "TLAS instance transform matrix is singular");
        }

        const uint32_t offset = AllocateNodeBytes(out_buffer, kRdna2BvhInstanceNodeSize);
        EncodeRdna2InstanceNode(inode, out_buffer.data() + offset, kRdna2BvhInstanceNodeSize);
        ++counters.leaf_nodes;

        const RtAabb world_box = TransformAabb3x4(in.object_to_world, in.blas_local_aabb);
        leaves.push_back({
            EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Instance),
            world_box,
            world_box.Center(),
        });
    }

    for (const auto& proc : procedurals) {
        if (!IsAabbValid(proc.bounds)) {
            out_buffer.clear();
            return Status::Error(StatusCode::InvalidArgument, "Invalid procedural leaf AABB");
        }
        Rdna2BvhProceduralNode pnode{};
        pnode.bounds = proc.bounds;
        pnode.primitive_index = proc.primitive_index;
        pnode.geometry_index = proc.geometry_index;
        pnode.geometry_flags = proc.geometry_flags;
        pnode.user_data = proc.user_data;

        const uint32_t offset = AllocateNodeBytes(out_buffer, kRdna2BvhProceduralNodeSize);
        EncodeRdna2ProceduralNode(pnode, out_buffer.data() + offset, kRdna2BvhProceduralNodeSize);
        ++counters.leaf_nodes;

        leaves.push_back({
            EncodeRdna2NodePtr(offset, Rdna2BvhNodeType::Procedural),
            proc.bounds,
            proc.bounds.Center(),
        });
    }

    const Bvh4EmitResult root = BuildTlasHierarchy(leaves, 0u, leaves.size(), options, out_buffer, counters);
    const size_t aligned_total = (out_buffer.size() + kRdna2BvhNodeAlignment - 1u) &
                                 ~static_cast<size_t>(kRdna2BvhNodeAlignment - 1u);
    out_buffer.resize(aligned_total, 0u);

    Rdna2BvhHeader hdr{};
    hdr.magic = kRdna2BvhMagic;
    hdr.version = kRdna2BvhVersion;
    hdr.as_type = Rdna2BvhAsType::TopLevel;
    hdr.build_flags = options.build_flags;
    hdr.total_size_bytes = static_cast<uint32_t>(out_buffer.size());
    hdr.header_size_bytes = kRdna2BvhHeaderSize;
    hdr.nodes_offset_bytes = kRdna2BvhHeaderSize;
    hdr.nodes_size_bytes = static_cast<uint32_t>(out_buffer.size() - kRdna2BvhHeaderSize);
    hdr.root_node_ptr = root.node_ptr;
    hdr.internal_node_count = counters.internal_nodes;
    hdr.leaf_node_count = counters.leaf_nodes;
    hdr.primitive_count = static_cast<uint32_t>(leaves.size());
    hdr.geometry_count = 1u;
    hdr.max_depth = root.max_depth;
    hdr.root_aabb = root.bounds;
    hdr.payload_crc32 = ComputeBvhCrc32(out_buffer.data() + hdr.nodes_offset_bytes, hdr.nodes_size_bytes);

    Status st = EncodeRdna2BvhHeader(hdr, out_buffer.data(), out_buffer.size());
    if (!st.ok()) {
        out_buffer.clear();
        return st;
    }

    if (out_header) {
        *out_header = hdr;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Full RDNA2 BVH4 Tree Validation
// ---------------------------------------------------------------------------
Status ValidateRdna2Bvh4(
    const uint8_t* data,
    size_t size,
    Rdna2BvhValidationReport* out_report) {
    Rdna2BvhHeader hdr{};
    Status st = DecodeRdna2BvhHeader(data, size, hdr);
    if (!st.ok()) {
        return st;
    }

    if (hdr.payload_crc32 != 0u) {
        const uint32_t actual_crc = ComputeBvhCrc32(data + hdr.nodes_offset_bytes, hdr.nodes_size_bytes);
        if (actual_crc != hdr.payload_crc32) {
            return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 node buffer CRC32 mismatch");
        }
    }

    if (!IsRdna2NodePtrValid(hdr.root_node_ptr)) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 root node pointer is invalid");
    }

    Rdna2BvhValidationReport report{};
    std::unordered_set<uint64_t> on_stack;
    std::unordered_set<uint64_t> visited;
    RtAabb computed_root{};

    st = ValidateSubtreeRecursive(data, size, hdr, hdr.root_node_ptr, 1u,
                                  on_stack, visited, report, computed_root);
    if (!st.ok()) {
        return st;
    }

    const uint32_t total_leaves = report.visited_triangle_nodes +
                                  report.visited_instance_nodes +
                                  report.visited_procedural_nodes;
    if (hdr.internal_node_count != report.visited_internal_nodes) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 internal_node_count header mismatch");
    }
    if (hdr.leaf_node_count != total_leaves) {
        return Status::Error(StatusCode::ParseError, "RDNA2 BVH4 leaf_node_count header mismatch");
    }

    if (DecodeRdna2NodeType(hdr.root_node_ptr) != Rdna2BvhNodeType::Instance) {
        if (!AabbEncloses(hdr.root_aabb, computed_root, 1e-3f)) {
            return Status::Error(StatusCode::ParseError,
                                 "RDNA2 BVH4 header root AABB does not enclose decoded tree bounds");
        }
    }

    report.computed_root_aabb = computed_root;
    if (out_report) {
        *out_report = report;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Decoded RDNA2 BVH4 Direct Ray Traversal
// ---------------------------------------------------------------------------
Status Rdna2Bvh4Tree::LoadFromBuffer(const uint8_t* data, size_t size) {
    buffer_.clear();
    header_ = Rdna2BvhHeader{};
    report_ = Rdna2BvhValidationReport{};

    Rdna2BvhValidationReport rep{};
    Status st = ValidateRdna2Bvh4(data, size, &rep);
    if (!st.ok()) {
        return st;
    }

    Rdna2BvhHeader hdr{};
    st = DecodeRdna2BvhHeader(data, size, hdr);
    if (!st.ok()) {
        return st;
    }

    buffer_.assign(data, data + hdr.total_size_bytes);
    header_ = hdr;
    report_ = rep;
    return Status::Ok();
}

void Rdna2Bvh4Tree::AttachTriangleMetadata(const std::vector<RtTriangle>& triangles) {
    triangle_meta_ = triangles;
}

bool Rdna2Bvh4Tree::TraceRay(const RtRay& ray, RtHit& out_hit, RtStats* stats) const {
    out_hit = RtHit{};
    out_hit.t = ray.t_max;
    if (buffer_.empty() || !IsRdna2NodePtrValid(header_.root_node_ptr)) {
        return false;
    }

    float root_near = 0.0f;
    if (stats) ++stats->aabb_tests;
    if (!IntersectRayAabb(header_.root_aabb, ray, out_hit.t, root_near)) {
        return false;
    }

    constexpr int kStackCapacity = 256;
    uint32_t stack[kStackCapacity];
    int sp = 0;
    stack[sp++] = header_.root_node_ptr;

    while (sp > 0) {
        const uint32_t ptr = stack[--sp];
        const Rdna2BvhNodeType type = DecodeRdna2NodeType(ptr);

        if (IsRdna2TriangleNodeType(type)) {
            Rdna2BvhTriangleNode tnode{};
            if (!DecodeRdna2TriangleNode(buffer_.data(), buffer_.size(), ptr, tnode).ok()) {
                continue;
            }
            const uint32_t tri_count = tnode.triangle_id & 0x3u;
            for (uint32_t ti = 0; ti < tri_count; ++ti) {
                RtTriangle tri{};
                uint32_t prim_idx = tnode.primitive_index;
                if (ti == 0u) {
                    tri.v0 = tnode.v0;
                    tri.v1 = tnode.v1;
                    tri.v2 = tnode.v2;
                } else {
                    tri.v0 = tnode.v1;
                    tri.v1 = tnode.v2;
                    tri.v2 = tnode.v3;
                    prim_idx = tnode.extra_index;
                }
                if (stats) ++stats->triangle_tests;
                float t = 0.0f, u = 0.0f, v = 0.0f;
                Vec3 n{};
                if (IntersectRayTriangle(tri, ray, out_hit.t, t, u, v, n)) {
                    out_hit.hit = true;
                    out_hit.t = t;
                    out_hit.u = u;
                    out_hit.v = v;
                    out_hit.triangle_index = prim_idx;
                    out_hit.normal = n;
                    if (prim_idx < triangle_meta_.size()) {
                        out_hit.color = triangle_meta_[prim_idx].color;
                        out_hit.reflectivity = triangle_meta_[prim_idx].reflectivity;
                    }
                }
            }
            continue;
        }

        if (type == Rdna2BvhNodeType::Box32 || type == Rdna2BvhNodeType::Box16) {
            std::array<uint32_t, 4> children{};
            std::array<RtAabb, 4> child_boxes{};

            if (type == Rdna2BvhNodeType::Box32) {
                Rdna2BvhBox32Node b32{};
                if (!DecodeRdna2Box32Node(buffer_.data(), buffer_.size(), ptr, b32).ok()) {
                    continue;
                }
                children = b32.children;
                child_boxes = b32.bbox;
            } else {
                Rdna2BvhBox16Node b16{};
                if (!DecodeRdna2Box16Node(buffer_.data(), buffer_.size(), ptr, b16).ok()) {
                    continue;
                }
                children = b16.children;
                for (uint32_t c = 0; c < 4u; ++c) {
                    child_boxes[c] = DecodeBox16ChildAabb(b16, c);
                }
            }

            struct HitChild {
                uint32_t ptr;
                float t_near;
            };
            HitChild hits[4]{};
            int hit_count = 0;

            for (uint32_t c = 0; c < 4u; ++c) {
                if (!IsRdna2NodePtrValid(children[c])) {
                    continue;
                }
                if (stats) ++stats->aabb_tests;
                float t_near = 0.0f;
                if (IntersectRayAabb(child_boxes[c], ray, out_hit.t, t_near)) {
                    hits[hit_count++] = {children[c], t_near};
                }
            }

            // Sort descending by t_near so closest child is pushed last and popped first.
            for (int i = 0; i < hit_count; ++i) {
                for (int j = i + 1; j < hit_count; ++j) {
                    if (hits[j].t_near > hits[i].t_near) {
                        std::swap(hits[i], hits[j]);
                    }
                }
            }
            for (int i = 0; i < hit_count; ++i) {
                if (sp < kStackCapacity) {
                    stack[sp++] = hits[i].ptr;
                }
            }
        }
    }

    return out_hit.hit;
}

bool Rdna2Bvh4Tree::TraceShadowRay(const RtRay& ray, RtStats* stats) const {
    if (buffer_.empty() || !IsRdna2NodePtrValid(header_.root_node_ptr)) {
        return false;
    }

    float root_near = 0.0f;
    if (stats) ++stats->aabb_tests;
    if (!IntersectRayAabb(header_.root_aabb, ray, ray.t_max, root_near)) {
        return false;
    }

    constexpr int kStackCapacity = 256;
    uint32_t stack[kStackCapacity];
    int sp = 0;
    stack[sp++] = header_.root_node_ptr;

    while (sp > 0) {
        const uint32_t ptr = stack[--sp];
        const Rdna2BvhNodeType type = DecodeRdna2NodeType(ptr);

        if (IsRdna2TriangleNodeType(type)) {
            Rdna2BvhTriangleNode tnode{};
            if (!DecodeRdna2TriangleNode(buffer_.data(), buffer_.size(), ptr, tnode).ok()) {
                continue;
            }
            const uint32_t tri_count = tnode.triangle_id & 0x3u;
            for (uint32_t ti = 0; ti < tri_count; ++ti) {
                RtTriangle tri{};
                if (ti == 0u) {
                    tri.v0 = tnode.v0;
                    tri.v1 = tnode.v1;
                    tri.v2 = tnode.v2;
                } else {
                    tri.v0 = tnode.v1;
                    tri.v1 = tnode.v2;
                    tri.v2 = tnode.v3;
                }
                if (stats) ++stats->triangle_tests;
                float t = 0.0f, u = 0.0f, v = 0.0f;
                Vec3 n{};
                if (IntersectRayTriangle(tri, ray, ray.t_max, t, u, v, n)) {
                    return true;
                }
            }
            continue;
        }

        if (type == Rdna2BvhNodeType::Box32 || type == Rdna2BvhNodeType::Box16) {
            std::array<uint32_t, 4> children{};
            std::array<RtAabb, 4> child_boxes{};
            if (type == Rdna2BvhNodeType::Box32) {
                Rdna2BvhBox32Node b32{};
                if (!DecodeRdna2Box32Node(buffer_.data(), buffer_.size(), ptr, b32).ok()) {
                    continue;
                }
                children = b32.children;
                child_boxes = b32.bbox;
            } else {
                Rdna2BvhBox16Node b16{};
                if (!DecodeRdna2Box16Node(buffer_.data(), buffer_.size(), ptr, b16).ok()) {
                    continue;
                }
                children = b16.children;
                for (uint32_t c = 0; c < 4u; ++c) {
                    child_boxes[c] = DecodeBox16ChildAabb(b16, c);
                }
            }

            for (uint32_t c = 0; c < 4u; ++c) {
                if (!IsRdna2NodePtrValid(children[c])) {
                    continue;
                }
                if (stats) ++stats->aabb_tests;
                float t_near = 0.0f;
                if (IntersectRayAabb(child_boxes[c], ray, ray.t_max, t_near) && sp < kStackCapacity) {
                    stack[sp++] = children[c];
                }
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// 64-bit Multi-Window RDNA2 BVH4 Traversal (image_bvh64_intersect_ray)
// ---------------------------------------------------------------------------
Status Rdna2Bvh64MultiWindow::RegisterWindow(uint64_t base_gpu_va, const uint8_t* data, size_t size) {
    if (base_gpu_va == 0ull || (base_gpu_va & (kRdna2BvhNodeAlignment - 1u)) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "BVH64 window base_gpu_va must be non-zero and 64-byte aligned");
    }
    if (!data || size < kRdna2BvhHeaderSize) {
        return Status::Error(StatusCode::InvalidArgument, "BVH64 window buffer invalid or too small");
    }

    Rdna2BvhValidationReport report{};
    Status st = ValidateRdna2Bvh4(data, size, &report);
    if (!st.ok()) {
        return st;
    }

    Rdna2BvhHeader hdr{};
    st = DecodeRdna2BvhHeader(data, size, hdr);
    if (!st.ok()) {
        return st;
    }

    const uint64_t new_end = base_gpu_va + static_cast<uint64_t>(hdr.total_size_bytes);
    if (new_end < base_gpu_va) {
        return Status::Error(StatusCode::InvalidArgument, "BVH64 window virtual address overflow");
    }

    // Replace existing window at identical base VA or verify non-overlapping range.
    for (auto& existing : windows_) {
        if (existing.base_gpu_va == base_gpu_va) {
            existing.header = hdr;
            existing.buffer.assign(data, data + hdr.total_size_bytes);
            return Status::Ok();
        }
        const uint64_t ex_end = existing.base_gpu_va + existing.buffer.size();
        if (base_gpu_va < ex_end && new_end > existing.base_gpu_va) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "BVH64 window overlaps an already registered 64-bit VA range");
        }
    }

    Rdna2Bvh64Window win{};
    win.base_gpu_va = base_gpu_va;
    win.header = hdr;
    win.buffer.assign(data, data + hdr.total_size_bytes);
    windows_.push_back(std::move(win));
    return Status::Ok();
}

Status Rdna2Bvh64MultiWindow::RegisterWindow(uint64_t base_gpu_va, std::vector<uint8_t> buffer) {
    return RegisterWindow(base_gpu_va, buffer.data(), buffer.size());
}

bool Rdna2Bvh64MultiWindow::UnregisterWindow(uint64_t base_gpu_va) {
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if (it->base_gpu_va == base_gpu_va) {
            windows_.erase(it);
            return true;
        }
    }
    return false;
}

void Rdna2Bvh64MultiWindow::Clear() {
    windows_.clear();
}

const Rdna2Bvh64Window* Rdna2Bvh64MultiWindow::FindWindow(uint64_t base_gpu_va) const noexcept {
    for (const auto& w : windows_) {
        if (w.base_gpu_va == base_gpu_va) {
            return &w;
        }
    }
    return nullptr;
}

Status Rdna2Bvh64MultiWindow::StepBvh64IntersectRay(
    uint64_t window_base_va,
    uint32_t node_ptr,
    const RtRay& ray,
    float t_best,
    uint32_t ray_flags,
    uint32_t instance_mask,
    uint32_t active_instance_flags,
    Rdna2Bvh64SingleStepResult& out_step) const {
    out_step = Rdna2Bvh64SingleStepResult{};
    if (!IsRdna2NodePtrValid(node_ptr)) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BVH4 node pointer");
    }

    const Rdna2Bvh64Window* win = FindWindow(window_base_va);
    if (!win) {
        return Status::Error(StatusCode::NotFound, "64-bit BVH4 window VA not registered");
    }

    const Rdna2BvhNodeType type = DecodeRdna2NodeType(node_ptr);
    out_step.node_type = type;

    if (IsRdna2TriangleNodeType(type)) {
        out_step.is_triangle = true;
        Rdna2BvhTriangleNode tnode{};
        Status st = DecodeRdna2TriangleNode(win->buffer.data(), win->buffer.size(), node_ptr, tnode);
        if (!st.ok()) {
            return st;
        }

        const bool is_opaque = (tnode.geometry_flags & 1u) != 0u;
        const bool effective_opaque =
            ((ray_flags & kRdna2RayFlagForceOpaque) != 0u ||
             (active_instance_flags & kDxrInstanceForceOpaque) != 0u)
                ? true
                : (((ray_flags & kRdna2RayFlagForceNonOpaque) != 0u ||
                    (active_instance_flags & kDxrInstanceForceNonOpaque) != 0u)
                       ? false
                       : is_opaque);
        if ((ray_flags & kRdna2RayFlagCullOpaque) != 0u && effective_opaque) {
            return Status::Ok();
        }
        if ((ray_flags & kRdna2RayFlagCullNonOpaque) != 0u && !effective_opaque) {
            return Status::Ok();
        }

        const uint32_t tri_count = std::min<uint32_t>(2u, tnode.triangle_id & 0x3u);
        float best_local_t = t_best;
        for (uint32_t ti = 0; ti < tri_count; ++ti) {
            RtTriangle tri{};
            uint32_t prim_idx = tnode.primitive_index;
            if (ti == 0u) {
                tri.v0 = tnode.v0;
                tri.v1 = tnode.v1;
                tri.v2 = tnode.v2;
            } else {
                tri.v0 = tnode.v1;
                tri.v1 = tnode.v2;
                tri.v2 = tnode.v3;
                prim_idx = tnode.extra_index;
            }

            float t = 0.0f;
            float u = 0.0f;
            float v = 0.0f;
            Vec3 normal{};
            bool front_face = true;
            if (IntersectRayTriangleEx(
                    tri, ray, best_local_t, ray_flags, active_instance_flags, t, u, v, normal, front_face)) {
                best_local_t = t;
                out_step.triangle_hit = true;
                out_step.hit_t = t;
                out_step.bary_u = u;
                out_step.bary_v = v;
                out_step.primitive_index = prim_idx;
                out_step.geometry_index = tnode.geometry_index;
                out_step.front_face = front_face;
                if ((ray_flags & kRdna2RayFlagAcceptFirstHitAndEndSearch) != 0u) {
                    break;
                }
            }
        }
        return Status::Ok();
    }

    if (type == Rdna2BvhNodeType::Box32 || type == Rdna2BvhNodeType::Box16) {
        out_step.is_box = true;
        std::array<uint32_t, 4> children{};
        std::array<RtAabb, 4> boxes{};
        if (type == Rdna2BvhNodeType::Box32) {
            Rdna2BvhBox32Node b32{};
            Status st = DecodeRdna2Box32Node(win->buffer.data(), win->buffer.size(), node_ptr, b32);
            if (!st.ok()) {
                return st;
            }
            children = b32.children;
            boxes = b32.bbox;
        } else {
            Rdna2BvhBox16Node b16{};
            Status st = DecodeRdna2Box16Node(win->buffer.data(), win->buffer.size(), node_ptr, b16);
            if (!st.ok()) {
                return st;
            }
            children = b16.children;
            for (uint32_t c = 0; c < 4u; ++c) {
                boxes[c] = DecodeBox16ChildAabb(b16, c);
            }
        }

        struct ChildHit {
            uint32_t ptr = kRdna2BvhInvalidNodePtr;
            float t_near = 1e30f;
        };
        ChildHit hits[4]{};
        uint32_t hit_count = 0u;
        for (uint32_t c = 0; c < 4u; ++c) {
            if (!IsRdna2NodePtrValid(children[c])) {
                continue;
            }
            float t_near = 0.0f;
            if (IntersectRayAabbSlabs(boxes[c], ray, t_best, t_near)) {
                hits[hit_count++] = {children[c], t_near};
            }
        }
        // Sort ascending by t_near (closest first in sorted_children[0..hit_child_count-1]).
        for (uint32_t i = 0; i < hit_count; ++i) {
            for (uint32_t j = i + 1u; j < hit_count; ++j) {
                if (hits[j].t_near < hits[i].t_near) {
                    std::swap(hits[i], hits[j]);
                }
            }
        }
        out_step.hit_child_count = hit_count;
        for (uint32_t i = 0; i < hit_count; ++i) {
            out_step.sorted_children[i] = hits[i].ptr;
        }
        return Status::Ok();
    }

    if (type == Rdna2BvhNodeType::Instance) {
        out_step.is_instance = true;
        Rdna2BvhInstanceNode inode{};
        Status st = DecodeRdna2InstanceNode(win->buffer.data(), win->buffer.size(), node_ptr, inode);
        if (!st.ok()) {
            return st;
        }
        if ((inode.instance_mask & (instance_mask & 0xFFu)) == 0u) {
            return Status::Ok();
        }
        out_step.next_blas_base_va = inode.blas_base_va;
        out_step.next_blas_root_ptr = inode.blas_root_ptr;
        out_step.instance_index = inode.instance_index;
        out_step.instance_id = inode.instance_id;
        out_step.instance_flags = inode.instance_flags;
        out_step.sbt_offset = inode.sbt_offset;

        out_step.transformed_ray = ray;
        out_step.transformed_ray.origin = TransformPoint3x4(inode.world_to_object, ray.origin);
        out_step.transformed_ray.direction = TransformVector3x4(inode.world_to_object, ray.direction);
        return Status::Ok();
    }

    if (type == Rdna2BvhNodeType::Procedural) {
        out_step.is_procedural = true;
        Rdna2BvhProceduralNode pnode{};
        Status st = DecodeRdna2ProceduralNode(win->buffer.data(), win->buffer.size(), node_ptr, pnode);
        if (!st.ok()) {
            return st;
        }
        float t_near = 0.0f;
        if (IntersectRayAabbSlabs(pnode.bounds, ray, t_best, t_near)) {
            out_step.triangle_hit = true;
            out_step.hit_t = t_near;
            out_step.primitive_index = pnode.primitive_index;
            out_step.geometry_index = pnode.geometry_index;
        }
        return Status::Ok();
    }

    return Status::Error(StatusCode::ParseError, "Unsupported RDNA2 BVH4 node type");
}

Status Rdna2Bvh64MultiWindow::TraceRay64(
    uint64_t tlas_base_va,
    const Rdna2RayQueryInput& ray_in,
    Rdna2RayQueryHit& out_hit,
    RtStats* out_stats) const {
    out_hit = Rdna2RayQueryHit{};
    out_hit.committed_status = kRdna2CommittedNothing;
    out_hit.t = ray_in.t_max;

    const Rdna2Bvh64Window* root_win = FindWindow(tlas_base_va);
    if (!root_win) {
        return Status::Error(StatusCode::NotFound, "Root 64-bit BVH4 window VA not found");
    }

    RtRay world_ray{};
    world_ray.origin = ray_in.origin;
    world_ray.direction = ray_in.direction;
    world_ray.t_min = ray_in.t_min;
    world_ray.t_max = ray_in.t_max;

    float root_near = 0.0f;
    if (out_stats) ++out_stats->aabb_tests;
    if (!IntersectRayAabbSlabs(root_win->header.root_aabb, world_ray, out_hit.t, root_near)) {
        return Status::Ok();
    }

    const bool accept_first = (ray_in.ray_flags & kRdna2RayFlagAcceptFirstHitAndEndSearch) != 0u;

    constexpr int kStackCap = 128;
    uint32_t top_stack[kStackCap];
    int top_sp = 0;
    top_stack[top_sp++] = root_win->header.root_node_ptr;

    while (top_sp > 0) {
        const uint32_t ptr = top_stack[--top_sp];
        Rdna2Bvh64SingleStepResult step{};
        Status st = StepBvh64IntersectRay(
            tlas_base_va, ptr, world_ray, out_hit.t, ray_in.ray_flags, ray_in.instance_mask, 0u, step);
        if (!st.ok()) {
            return st;
        }

        if (step.is_box) {
            if (out_stats) out_stats->aabb_tests += 4u;
            // Push far-to-near so closest child is popped first.
            for (int i = static_cast<int>(step.hit_child_count) - 1; i >= 0; --i) {
                if (top_sp < kStackCap) {
                    top_stack[top_sp++] = step.sorted_children[static_cast<size_t>(i)];
                }
            }
            continue;
        }

        if (step.is_triangle) {
            if (out_stats) ++out_stats->triangle_tests;
            if (step.triangle_hit && step.hit_t < out_hit.t) {
                out_hit.committed_status = kRdna2CommittedTriangleHit;
                out_hit.t = step.hit_t;
                out_hit.bary_u = step.bary_u;
                out_hit.bary_v = step.bary_v;
                out_hit.primitive_index = step.primitive_index;
                out_hit.instance_index = 0u;
                out_hit.instance_id = 0u;
                out_hit.geometry_index = step.geometry_index;
                out_hit.sbt_offset = 0u;
                out_hit.front_face = step.front_face ? 1u : 0u;
                if (accept_first) {
                    return Status::Ok();
                }
            }
            continue;
        }

        if (step.is_procedural) {
            if (step.triangle_hit && step.hit_t < out_hit.t) {
                out_hit.committed_status = kRdna2CommittedProceduralHit;
                out_hit.t = step.hit_t;
                out_hit.primitive_index = step.primitive_index;
                out_hit.geometry_index = step.geometry_index;
                if (accept_first) {
                    return Status::Ok();
                }
            }
            continue;
        }

        if (step.is_instance && step.next_blas_base_va != 0ull && IsRdna2NodePtrValid(step.next_blas_root_ptr)) {
            const Rdna2Bvh64Window* blas_win = FindWindow(step.next_blas_base_va);
            if (!blas_win) {
                continue;
            }
            uint32_t blas_stack[kStackCap];
            int blas_sp = 0;
            blas_stack[blas_sp++] = step.next_blas_root_ptr;

            while (blas_sp > 0) {
                const uint32_t bptr = blas_stack[--blas_sp];
                Rdna2Bvh64SingleStepResult bstep{};
                Status bst = StepBvh64IntersectRay(
                    step.next_blas_base_va,
                    bptr,
                    step.transformed_ray,
                    out_hit.t,
                    ray_in.ray_flags,
                    ray_in.instance_mask,
                    step.instance_flags,
                    bstep);
                if (!bst.ok()) {
                    return bst;
                }
                if (bstep.is_box) {
                    if (out_stats) out_stats->aabb_tests += 4u;
                    for (int i = static_cast<int>(bstep.hit_child_count) - 1; i >= 0; --i) {
                        if (blas_sp < kStackCap) {
                            blas_stack[blas_sp++] = bstep.sorted_children[static_cast<size_t>(i)];
                        }
                    }
                } else if (bstep.is_triangle) {
                    if (out_stats) ++out_stats->triangle_tests;
                    if (bstep.triangle_hit && bstep.hit_t < out_hit.t) {
                        out_hit.committed_status = kRdna2CommittedTriangleHit;
                        out_hit.t = bstep.hit_t;
                        out_hit.bary_u = bstep.bary_u;
                        out_hit.bary_v = bstep.bary_v;
                        out_hit.primitive_index = bstep.primitive_index;
                        out_hit.instance_index = step.instance_index;
                        out_hit.instance_id = step.instance_id;
                        out_hit.geometry_index = bstep.geometry_index;
                        out_hit.sbt_offset = step.sbt_offset;
                        out_hit.front_face = bstep.front_face ? 1u : 0u;
                        if (accept_first) {
                            return Status::Ok();
                        }
                    }
                }
            }
        }
    }

    return Status::Ok();
}

Status Rdna2Bvh64MultiWindow::PackForGpuCompute(
    uint64_t tlas_base_va,
    std::vector<Rdna2GpuBvhWindowDesc>& out_window_table,
    std::vector<uint32_t>& out_bvh_dwords,
    uint32_t& out_tlas_window_index) const {
    out_window_table.clear();
    out_bvh_dwords.clear();
    out_tlas_window_index = 0xFFFFFFFFu;

    if (windows_.empty()) {
        return Status::Error(StatusCode::RaytracingError, "No BVH64 windows registered to pack");
    }

    out_window_table.reserve(windows_.size());
    for (size_t i = 0; i < windows_.size(); ++i) {
        const auto& w = windows_[i];
        if ((w.buffer.size() % 4u) != 0u) {
            return Status::Error(StatusCode::ParseError, "BVH4 window buffer size is not 4-byte aligned");
        }
        const uint32_t dw_offset = static_cast<uint32_t>(out_bvh_dwords.size());
        const uint32_t dw_count = static_cast<uint32_t>(w.buffer.size() / 4u);
        out_bvh_dwords.resize(out_bvh_dwords.size() + dw_count);
        for (uint32_t d = 0; d < dw_count; ++d) {
            out_bvh_dwords[dw_offset + d] = ReadLe32(w.buffer.data() + static_cast<size_t>(d) * 4u);
        }

        Rdna2GpuBvhWindowDesc desc{};
        desc.base_va_lo = static_cast<uint32_t>(w.base_gpu_va & 0xFFFFFFFFull);
        desc.base_va_hi = static_cast<uint32_t>((w.base_gpu_va >> 32u) & 0xFFFFFFFFull);
        desc.dword_offset = dw_offset;
        desc.dword_count = dw_count;
        desc.root_node_ptr = w.header.root_node_ptr;
        desc.as_type = static_cast<uint32_t>(w.header.as_type);
        out_window_table.push_back(desc);

        if (w.base_gpu_va == tlas_base_va) {
            out_tlas_window_index = static_cast<uint32_t>(i);
        }
    }

    if (out_tlas_window_index == 0xFFFFFFFFu) {
        out_window_table.clear();
        out_bvh_dwords.clear();
        return Status::Error(StatusCode::NotFound, "Requested TLAS base_gpu_va not found in registered windows");
    }

    return Status::Ok();
}

}  // namespace openscarlett
