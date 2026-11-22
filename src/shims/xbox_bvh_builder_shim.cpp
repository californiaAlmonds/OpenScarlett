#define OPENSCARLETT_BUILDING_XBOX_BVH 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rt_bvh.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct XboxBvhBuilderObject {
    std::mutex mutex;
    openscarlett::DxrHardwareEngine engine;
    std::unordered_map<uint64_t, std::vector<openscarlett::RtTriangle>> blas_triangles;
    std::unordered_map<uint64_t, std::shared_ptr<openscarlett::BvhAccelerationStructure>> bvh_cache;
    std::unordered_map<uint64_t, openscarlett::XboxBvhConvertedBlas> converted_blas;
    std::unordered_map<uint64_t, openscarlett::XboxBvhConvertedTlas> converted_tlas;
    std::unordered_map<uint64_t, std::vector<uint64_t>> tlas_instance_blas_ids;
    openscarlett::Rdna2Bvh64MultiWindow bvh64_windows;
    uint64_t next_xbox_va = 0x0000001000000000ull;

    uint64_t AllocXboxVa(uint64_t size_bytes) noexcept {
        const uint64_t va = next_xbox_va;
        const uint64_t padded = (size_bytes + 65535ull) & ~65535ull;
        next_xbox_va += (padded < 65536ull) ? 65536ull : padded;
        return va;
    }
};

namespace {

constexpr const char* kModName = "XboxBvhBuilder.dll";

struct BvhGlobalState {
    std::mutex mutex;
    std::unordered_map<XboxBvhBuilderHandle, std::shared_ptr<XboxBvhBuilderObject>> builders;
};

BvhGlobalState& GetBvhGlobal() {
    static BvhGlobalState* s = new BvhGlobalState();
    return *s;
}

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

std::shared_ptr<XboxBvhBuilderObject> LookupBuilder(XboxBvhBuilderHandle handle) {
    if (!handle) return nullptr;
    auto& g = GetBvhGlobal();
    std::lock_guard<std::mutex> lk(g.mutex);
    auto it = g.builders.find(handle);
    return it == g.builders.end() ? nullptr : it->second;
}

std::vector<openscarlett::RtTriangle> ConvertTriangles(
    const XboxBvhTriangle* triangles,
    uint32_t count) {
    std::vector<openscarlett::RtTriangle> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        openscarlett::RtTriangle t{};
        t.v0 = {triangles[i].v0[0], triangles[i].v0[1], triangles[i].v0[2]};
        t.v1 = {triangles[i].v1[0], triangles[i].v1[1], triangles[i].v1[2]};
        t.v2 = {triangles[i].v2[0], triangles[i].v2[1], triangles[i].v2[2]};
        t.color = {triangles[i].color[0], triangles[i].color[1], triangles[i].color[2]};
        t.reflectivity = triangles[i].reflectivity;
        out.push_back(t);
    }
    return out;
}

int32_t StatusToHr(const openscarlett::Status& st) noexcept {
    if (st.ok()) return OS_SHIM_S_OK;
    if (st.code == openscarlett::StatusCode::InvalidArgument) return OS_SHIM_E_INVALIDARG;
    if (st.code == openscarlett::StatusCode::NotFound) return OS_SHIM_E_NOT_FOUND;
    if (st.code == openscarlett::StatusCode::OutOfMemory) return OS_SHIM_E_OUTOFMEMORY;
    return OS_SHIM_E_FAIL;
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL XboxBvhCreateBuilder(
    const XboxBvhCreateDesc* desc,
    XboxBvhBuilderHandle* outBuilder) {
    if (!outBuilder) {
        return RecordHr("XboxBvhCreateBuilder", OS_SHIM_E_POINTER);
    }
    *outBuilder = nullptr;

    auto obj = std::make_shared<XboxBvhBuilderObject>();
    openscarlett::Status st{};
    if (desc && desc->nativeD3D12Device != nullptr) {
        st = obj->engine.Initialize(
            static_cast<ID3D12Device*>(desc->nativeD3D12Device),
            desc->forceSoftwarePath != 0u);
    } else {
        openscarlett::DxrInitOptions opts{};
        if (desc) {
            opts.prefer_warp = desc->preferWarp != 0u;
            opts.force_software_path = desc->forceSoftwarePath != 0u;
            opts.enable_debug_layer = desc->enableDebugLayer != 0u;
        }
        st = obj->engine.Initialize(opts);
    }

    if (!st.ok()) {
        return RecordHr("XboxBvhCreateBuilder", StatusToHr(st));
    }

    XboxBvhBuilderHandle raw = obj.get();
    {
        auto& g = GetBvhGlobal();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.builders[raw] = std::move(obj);
    }
    *outBuilder = raw;
    return RecordHr("XboxBvhCreateBuilder", OS_SHIM_S_OK);
}

void OS_SHIM_CALL XboxBvhDestroyBuilder(XboxBvhBuilderHandle builder) {
    std::shared_ptr<XboxBvhBuilderObject> removed;
    if (builder) {
        auto& g = GetBvhGlobal();
        std::lock_guard<std::mutex> lk(g.mutex);
        auto it = g.builders.find(builder);
        if (it != g.builders.end()) {
            removed = std::move(it->second);
            g.builders.erase(it);
        }
    }
    if (removed) {
        std::lock_guard<std::mutex> lk(removed->mutex);
        removed->bvh64_windows.Clear();
        removed->converted_tlas.clear();
        removed->converted_blas.clear();
        removed->tlas_instance_blas_ids.clear();
        removed->bvh_cache.clear();
        removed->blas_triangles.clear();
        removed->engine.Shutdown();
    }
    RecordOk("XboxBvhDestroyBuilder");
}

int32_t OS_SHIM_CALL XboxBvhGetCapabilities(
    XboxBvhBuilderHandle builder,
    XboxBvhCapabilities* outCaps) {
    if (!builder || !outCaps) {
        return RecordHr("XboxBvhGetCapabilities", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhGetCapabilities", OS_SHIM_E_HANDLE);
    }

    std::lock_guard<std::mutex> lk(obj->mutex);
    const auto& c = obj->engine.Capabilities();
    std::memset(outCaps, 0, sizeof(XboxBvhCapabilities));
    outCaps->hardwareTier = c.hardware_tier;
    outCaps->supportsDevice5 = c.supports_device5 ? 1u : 0u;
    outCaps->supportsStateObjects = c.supports_state_objects ? 1u : 0u;
    outCaps->supportsInlineRt = c.supports_inline_rt ? 1u : 0u;
    outCaps->usingHardwarePath = c.using_hardware_path ? 1u : 0u;
    outCaps->isWarp = c.adapter_is_warp ? 1u : 0u;
    outCaps->vendorId = c.vendor_id;
    outCaps->deviceId = c.device_id;
    outCaps->dedicatedVideoMemory = c.dedicated_video_memory;
    std::snprintf(outCaps->adapterDescription, sizeof(outCaps->adapterDescription),
                  "%s", c.adapter_description.c_str());
    return RecordHr("XboxBvhGetCapabilities", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhGetPrebuildInfo(
    XboxBvhBuilderHandle builder,
    const XboxBvhPrebuildInputs* inputs,
    XboxBvhPrebuildInfo* outInfo) {
    if (!builder || !inputs || !outInfo) {
        return RecordHr("XboxBvhGetPrebuildInfo", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhGetPrebuildInfo", OS_SHIM_E_HANDLE);
    }

    openscarlett::DxrAsInputs in{};
    in.type = (inputs->asType == XBOX_BVH_AS_TYPE_TOP_LEVEL)
                  ? openscarlett::DxrAsType::TopLevel
                  : openscarlett::DxrAsType::BottomLevel;
    in.build_flags = inputs->buildFlags;
    in.primitive_count = inputs->primitiveCount;
    in.geometry_count = inputs->geometryCount == 0u ? 1u : inputs->geometryCount;
    in.vertex_stride = inputs->vertexStride == 0u ? 12u : inputs->vertexStride;
    in.is_opaque = inputs->isOpaque != 0u;

    openscarlett::DxrPrebuildInfo info{};
    openscarlett::Status st{};
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        st = obj->engine.QueryPrebuildInfo(in, info);
    }
    if (!st.ok()) {
        return RecordHr("XboxBvhGetPrebuildInfo", StatusToHr(st));
    }

    outInfo->resultDataMaxSizeBytes = info.result_data_max_size_bytes;
    outInfo->scratchDataSizeBytes = info.scratch_data_size_bytes;
    outInfo->updateScratchDataSizeBytes = info.update_scratch_data_size_bytes;
    outInfo->fromHardwareDriver = info.from_hardware_driver ? 1u : 0u;
    return RecordHr("XboxBvhGetPrebuildInfo", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhBuildBlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount,
    uint32_t buildFlags,
    uint64_t* outBlasId) {
    if (!builder || !triangles || triangleCount == 0u || !outBlasId) {
        return RecordHr("XboxBvhBuildBlas", OS_SHIM_E_INVALIDARG);
    }
    *outBlasId = 0ull;
    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhBuildBlas", OS_SHIM_E_HANDLE);
    }

    auto tris = ConvertTriangles(triangles, triangleCount);
    uint64_t blas_id = 0ull;

    std::lock_guard<std::mutex> lk(obj->mutex);
    const openscarlett::Status st = obj->engine.BuildBlas(tris, buildFlags, blas_id);
    if (!st.ok()) {
        return RecordHr("XboxBvhBuildBlas", StatusToHr(st));
    }

    auto bvh = std::make_shared<openscarlett::BvhAccelerationStructure>();
    if (bvh->Build(tris).ok()) {
        obj->bvh_cache[blas_id] = std::move(bvh);
    }

    // Build Xbox RDNA2 BVH4 representation + register 64-bit multi-window buffer.
    openscarlett::XboxBvhBlasBuildInput conv_in{};
    conv_in.build_flags = buildFlags;
    conv_in.use_box16 = (buildFlags & XBOX_BVH_BUILD_MINIMIZE_MEMORY) != 0u;
    openscarlett::XboxBvhGeometryDesc gdesc{};
    gdesc.vertices.reserve(tris.size() * 3u);
    for (const auto& t : tris) {
        gdesc.vertices.push_back(t.v0);
        gdesc.vertices.push_back(t.v1);
        gdesc.vertices.push_back(t.v2);
    }
    if (!tris.empty()) {
        gdesc.default_color = tris[0].color;
        gdesc.default_reflectivity = tris[0].reflectivity;
    }
    conv_in.geometries.push_back(std::move(gdesc));

    openscarlett::XboxBvhConvertedBlas conv_blas{};
    const uint64_t xbox_va = obj->AllocXboxVa(
        static_cast<uint64_t>(tris.size()) * 256ull + 4096ull);
    if (openscarlett::ConvertXboxBlasToHostAndBvh4(conv_in, xbox_va, conv_blas).ok()) {
        conv_blas.host_triangles = tris;
        obj->bvh64_windows.RegisterWindow(xbox_va, conv_blas.xbox_bvh4_buffer);
        obj->converted_blas[blas_id] = std::move(conv_blas);
    }

    obj->blas_triangles[blas_id] = std::move(tris);
    *outBlasId = blas_id;
    return RecordHr("XboxBvhBuildBlas", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhUpdateBlas(
    XboxBvhBuilderHandle builder,
    uint64_t blasId,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount) {
    if (!builder || blasId == 0ull || !triangles || triangleCount == 0u) {
        return RecordHr("XboxBvhUpdateBlas", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhUpdateBlas", OS_SHIM_E_HANDLE);
    }

    auto tris = ConvertTriangles(triangles, triangleCount);
    std::lock_guard<std::mutex> lk(obj->mutex);
    const openscarlett::Status st = obj->engine.UpdateBlas(blasId, tris);
    if (!st.ok()) {
        return RecordHr("XboxBvhUpdateBlas", StatusToHr(st));
    }

    auto bvh = std::make_shared<openscarlett::BvhAccelerationStructure>();
    if (bvh->Build(tris).ok()) {
        obj->bvh_cache[blasId] = std::move(bvh);
    }

    auto cit = obj->converted_blas.find(blasId);
    if (cit != obj->converted_blas.end()) {
        const uint64_t existing_va = cit->second.xbox_gpu_va;
        openscarlett::XboxBvhBlasBuildInput conv_in{};
        conv_in.build_flags = cit->second.host_build_flags;
        conv_in.use_box16 = (cit->second.host_build_flags & XBOX_BVH_BUILD_MINIMIZE_MEMORY) != 0u;
        openscarlett::XboxBvhGeometryDesc gdesc{};
        gdesc.vertices.reserve(tris.size() * 3u);
        for (const auto& t : tris) {
            gdesc.vertices.push_back(t.v0);
            gdesc.vertices.push_back(t.v1);
            gdesc.vertices.push_back(t.v2);
        }
        conv_in.geometries.push_back(std::move(gdesc));
        openscarlett::XboxBvhConvertedBlas rebuilt{};
        if (openscarlett::ConvertXboxBlasToHostAndBvh4(conv_in, existing_va, rebuilt).ok()) {
            rebuilt.host_triangles = tris;
            obj->bvh64_windows.RegisterWindow(existing_va, rebuilt.xbox_bvh4_buffer);
            cit->second = std::move(rebuilt);
        }
    }

    obj->blas_triangles[blasId] = std::move(tris);
    return RecordHr("XboxBvhUpdateBlas", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhBuildTlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhInstanceDesc* instances,
    uint32_t instanceCount,
    uint32_t buildFlags,
    uint64_t* outTlasId) {
    if (!builder || !instances || instanceCount == 0u || !outTlasId) {
        return RecordHr("XboxBvhBuildTlas", OS_SHIM_E_INVALIDARG);
    }
    *outTlasId = 0ull;
    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhBuildTlas", OS_SHIM_E_HANDLE);
    }

    std::vector<openscarlett::DxrInstanceDesc> dxr_insts;
    dxr_insts.reserve(instanceCount);
    for (uint32_t i = 0; i < instanceCount; ++i) {
        openscarlett::DxrInstanceDesc d{};
        d.blas_id = instances[i].blasId;
        std::memcpy(d.transform, instances[i].transform, sizeof(d.transform));
        d.instance_id = instances[i].instanceId;
        d.instance_mask = instances[i].instanceMask;
        d.instance_contribution_to_hit_group_index = instances[i].hitGroupContribution;
        d.flags = instances[i].flags;
        dxr_insts.push_back(d);
    }

    std::lock_guard<std::mutex> lk(obj->mutex);
    uint64_t tlas_id = 0ull;
    const openscarlett::Status st = obj->engine.BuildTlas(dxr_insts, buildFlags, tlas_id);
    if (!st.ok()) {
        return RecordHr("XboxBvhBuildTlas", StatusToHr(st));
    }

    // Build flattened world-space BVH mirror AND real 64-bit multi-window RDNA2 BVH4 TLAS.
    std::vector<openscarlett::RtTriangle> world_tris;
    std::vector<openscarlett::XboxBvhTlasInstanceInput> bvh4_insts;
    std::vector<openscarlett::XboxBvhConvertedBlas> blas_snapshot;
    std::vector<uint64_t> instance_blas_ids;
    bvh4_insts.reserve(dxr_insts.size());
    instance_blas_ids.reserve(dxr_insts.size());

    for (const auto& inst : dxr_insts) {
        auto bit = obj->converted_blas.find(inst.blas_id);
        if (bit != obj->converted_blas.end()) {
            openscarlett::XboxBvhTlasInstanceInput ti{};
            ti.blas_xbox_va = bit->second.xbox_gpu_va;
            ti.host_blas_id = inst.blas_id;
            std::memcpy(ti.object_to_world, inst.transform, sizeof(ti.object_to_world));
            ti.instance_id = inst.instance_id;
            ti.instance_mask = inst.instance_mask;
            ti.hit_group_contribution = inst.instance_contribution_to_hit_group_index;
            ti.instance_flags = inst.flags;
            bvh4_insts.push_back(ti);
            blas_snapshot.push_back(bit->second);
            instance_blas_ids.push_back(inst.blas_id);
        }

        if (inst.instance_mask == 0u) continue;
        auto it = obj->blas_triangles.find(inst.blas_id);
        if (it == obj->blas_triangles.end()) continue;
        for (const auto& tri : it->second) {
            world_tris.push_back(openscarlett::TransformTriangle3x4(inst.transform, tri));
        }
    }

    if (!bvh4_insts.empty()) {
        openscarlett::XboxBvhConvertedTlas conv_tlas{};
        const uint64_t tlas_va = obj->AllocXboxVa(
            static_cast<uint64_t>(bvh4_insts.size()) * 256ull + 4096ull);
        const bool use_box16 = (buildFlags & XBOX_BVH_BUILD_MINIMIZE_MEMORY) != 0u;
        if (openscarlett::ConvertXboxTlasToHostAndBvh4(
                bvh4_insts, blas_snapshot, tlas_va, buildFlags, use_box16, conv_tlas).ok()) {
            obj->bvh64_windows.RegisterWindow(tlas_va, conv_tlas.xbox_bvh4_buffer);
            obj->converted_tlas[tlas_id] = std::move(conv_tlas);
            obj->tlas_instance_blas_ids[tlas_id] = std::move(instance_blas_ids);
        }
    }

    if (!world_tris.empty()) {
        auto bvh = std::make_shared<openscarlett::BvhAccelerationStructure>();
        if (bvh->Build(world_tris).ok()) {
            obj->bvh_cache[tlas_id] = std::move(bvh);
        }
    }

    *outTlasId = tlas_id;
    return RecordHr("XboxBvhBuildTlas", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhTraceRay(
    XboxBvhBuilderHandle builder,
    uint64_t asId,
    const XboxBvhRay* ray,
    XboxBvhHit* outHit) {
    if (!builder || asId == 0ull || !ray || !outHit) {
        return RecordHr("XboxBvhTraceRay", OS_SHIM_E_INVALIDARG);
    }
    std::memset(outHit, 0, sizeof(XboxBvhHit));

    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhTraceRay", OS_SHIM_E_HANDLE);
    }

    std::shared_ptr<openscarlett::BvhAccelerationStructure> bvh;
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        auto it = obj->bvh_cache.find(asId);
        if (it == obj->bvh_cache.end()) {
            return RecordHr("XboxBvhTraceRay", OS_SHIM_E_NOT_FOUND);
        }
        bvh = it->second;
    }

    openscarlett::RtRay r{};
    r.origin = {ray->origin[0], ray->origin[1], ray->origin[2]};
    r.direction = {ray->direction[0], ray->direction[1], ray->direction[2]};
    r.t_min = ray->tMin > 0.0f ? ray->tMin : 0.001f;
    r.t_max = ray->tMax > r.t_min ? ray->tMax : 1e30f;

    // Prefer real 64-bit multi-window BVH4 traversal when the AS has a converted BVH4 window,
    // verifying exact parity and enriching shading metadata from the hit BLAS triangles.
    bool traced_via_bvh64 = false;
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        uint64_t root_va = 0ull;
        bool is_tlas = false;
        if (auto tit = obj->converted_tlas.find(asId); tit != obj->converted_tlas.end()) {
            root_va = tit->second.xbox_gpu_va;
            is_tlas = true;
        } else if (auto bit = obj->converted_blas.find(asId); bit != obj->converted_blas.end()) {
            root_va = bit->second.xbox_gpu_va;
        }

        if (root_va != 0ull) {
            openscarlett::Rdna2RayQueryInput rq{};
            rq.origin = r.origin;
            rq.t_min = r.t_min;
            rq.direction = r.direction;
            rq.t_max = r.t_max;
            rq.ray_flags = openscarlett::kRdna2RayFlagNone;
            rq.instance_mask = 0xFFu;

            openscarlett::Rdna2RayQueryHit rq_hit{};
            if (obj->bvh64_windows.TraceRay64(root_va, rq, rq_hit).ok()) {
                traced_via_bvh64 = true;
                if (rq_hit.committed_status == openscarlett::kRdna2CommittedTriangleHit) {
                    outHit->hit = 1u;
                    outHit->t = rq_hit.t;
                    outHit->u = rq_hit.bary_u;
                    outHit->v = rq_hit.bary_v;
                    outHit->triangleIndex = rq_hit.primitive_index;

                    const openscarlett::RtTriangle* hit_tri = nullptr;
                    if (is_tlas) {
                        auto ib_it = obj->tlas_instance_blas_ids.find(asId);
                        if (ib_it != obj->tlas_instance_blas_ids.end() &&
                            rq_hit.instance_index < ib_it->second.size()) {
                            const uint64_t hit_blas_id = ib_it->second[rq_hit.instance_index];
                            auto bt_it = obj->blas_triangles.find(hit_blas_id);
                            if (bt_it != obj->blas_triangles.end() &&
                                rq_hit.primitive_index < bt_it->second.size()) {
                                hit_tri = &bt_it->second[rq_hit.primitive_index];
                            }
                        }
                    } else {
                        auto bt_it = obj->blas_triangles.find(asId);
                        if (bt_it != obj->blas_triangles.end() &&
                            rq_hit.primitive_index < bt_it->second.size()) {
                            hit_tri = &bt_it->second[rq_hit.primitive_index];
                        }
                    }

                    if (hit_tri) {
                        outHit->color[0] = hit_tri->color.x;
                        outHit->color[1] = hit_tri->color.y;
                        outHit->color[2] = hit_tri->color.z;
                        outHit->reflectivity = hit_tri->reflectivity;
                        float tmp_t = 0.0f, tmp_u = 0.0f, tmp_v = 0.0f;
                        openscarlett::Vec3 n{};
                        if (!is_tlas && openscarlett::IntersectRayTriangle(
                                *hit_tri, r, r.t_max, tmp_t, tmp_u, tmp_v, n)) {
                            outHit->normal[0] = n.x;
                            outHit->normal[1] = n.y;
                            outHit->normal[2] = n.z;
                        } else if (bvh) {
                            openscarlett::RtHit h_meta{};
                            if (bvh->TraceRay(r, h_meta, nullptr) && h_meta.hit) {
                                outHit->normal[0] = h_meta.normal.x;
                                outHit->normal[1] = h_meta.normal.y;
                                outHit->normal[2] = h_meta.normal.z;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!traced_via_bvh64 && bvh) {
        openscarlett::RtHit h{};
        if (bvh->TraceRay(r, h, nullptr) && h.hit) {
            outHit->hit = 1u;
            outHit->t = h.t;
            outHit->u = h.u;
            outHit->v = h.v;
            outHit->triangleIndex = h.triangle_index;
            outHit->normal[0] = h.normal.x;
            outHit->normal[1] = h.normal.y;
            outHit->normal[2] = h.normal.z;
            outHit->color[0] = h.color.x;
            outHit->color[1] = h.color.y;
            outHit->color[2] = h.color.z;
            outHit->reflectivity = h.reflectivity;
        }
    }

    return RecordHr("XboxBvhTraceRay", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL XboxBvhDispatchRays(
    XboxBvhBuilderHandle builder,
    uint64_t tlasId,
    const XboxBvhDispatchDesc* desc,
    uint8_t* outRgba8,
    size_t outBufferBytes,
    XboxBvhDispatchStats* outStats) {
    if (!builder || tlasId == 0ull || !desc || desc->width == 0u || desc->height == 0u) {
        return RecordHr("XboxBvhDispatchRays", OS_SHIM_E_INVALIDARG);
    }
    const size_t required =
        static_cast<size_t>(desc->width) * static_cast<size_t>(desc->height) * 4u;
    if (outRgba8 && outBufferBytes < required) {
        return RecordHr("XboxBvhDispatchRays", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    }

    auto obj = LookupBuilder(builder);
    if (!obj) {
        return RecordHr("XboxBvhDispatchRays", OS_SHIM_E_HANDLE);
    }

    openscarlett::RtDispatchDesc rt{};
    rt.width = desc->width;
    rt.height = desc->height;
    rt.depth = 1u;
    rt.max_bounces = desc->maxBounces == 0u ? 2u : desc->maxBounces;
    rt.camera_pos = {desc->cameraPos[0], desc->cameraPos[1], desc->cameraPos[2]};
    rt.camera_look_at = {desc->cameraLookAt[0], desc->cameraLookAt[1], desc->cameraLookAt[2]};
    rt.light_pos = {desc->lightPos[0], desc->lightPos[1], desc->lightPos[2]};

    openscarlett::FrameBufferRgba8 frame{};
    openscarlett::RtStats stats{};
    uint64_t hash = 0ull;
    openscarlett::Status st{};
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        st = obj->engine.DispatchRays(tlasId, rt, frame, &stats, &hash);
    }
    if (!st.ok()) {
        return RecordHr("XboxBvhDispatchRays", StatusToHr(st));
    }

    if (outRgba8 && frame.pixels.size() >= required) {
        std::memcpy(outRgba8, frame.pixels.data(), required);
    }
    if (outStats) {
        std::memset(outStats, 0, sizeof(XboxBvhDispatchStats));
        outStats->primaryRays = stats.primary_rays;
        outStats->primaryHits = stats.primary_hits;
        outStats->shadowRays = stats.shadow_rays;
        outStats->shadowHits = stats.shadow_hits;
        outStats->reflectionRays = stats.reflection_rays;
        outStats->aabbTests = stats.aabb_tests;
        outStats->triangleTests = stats.triangle_tests;
        outStats->bvhNodes = stats.bvh_nodes;
        outStats->bvhLeafNodes = stats.bvh_leaf_nodes;
        outStats->maxTreeDepth = stats.max_tree_depth;
        outStats->frameHash = hash;
    }

    return RecordHr("XboxBvhDispatchRays", OS_SHIM_S_OK);
}

}  // extern "C"
