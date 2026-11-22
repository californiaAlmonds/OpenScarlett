#pragma once

// MIT Xbox BVH Acceleration Builder (<XboxBvhBuilder.h>) public C ABI header
// backed by XboxBvhBuilder.dll.

#include <cstddef>
#include <cstdint>

#define BVH_COMPAT_API __declspec(dllimport)
#define BVH_COMPAT_CALL __stdcall

typedef struct XboxBvhBuilderObject* XboxBvhBuilderHandle;

constexpr uint32_t XBOX_BVH_AS_TYPE_TOP_LEVEL = 0u;
constexpr uint32_t XBOX_BVH_AS_TYPE_BOTTOM_LEVEL = 1u;

constexpr uint32_t XBOX_BVH_BUILD_NONE = 0x0u;
constexpr uint32_t XBOX_BVH_BUILD_ALLOW_UPDATE = 0x1u;
constexpr uint32_t XBOX_BVH_BUILD_ALLOW_COMPACTION = 0x2u;
constexpr uint32_t XBOX_BVH_BUILD_PREFER_FAST_TRACE = 0x4u;
constexpr uint32_t XBOX_BVH_BUILD_PREFER_FAST_BUILD = 0x8u;
constexpr uint32_t XBOX_BVH_BUILD_MINIMIZE_MEMORY = 0x10u;

struct XboxBvhCreateDesc {
    uint32_t preferWarp;
    uint32_t forceSoftwarePath;
    uint32_t enableDebugLayer;
    void* nativeD3D12Device;
};

struct XboxBvhCapabilities {
    uint32_t hardwareTier;
    uint32_t supportsDevice5;
    uint32_t supportsStateObjects;
    uint32_t supportsInlineRt;
    uint32_t usingHardwarePath;
    uint32_t isWarp;
    uint32_t vendorId;
    uint32_t deviceId;
    uint64_t dedicatedVideoMemory;
    char adapterDescription[128];
};

struct XboxBvhPrebuildInputs {
    uint32_t asType;
    uint32_t buildFlags;
    uint32_t primitiveCount;
    uint32_t geometryCount;
    uint32_t vertexStride;
    uint32_t isOpaque;
};

struct XboxBvhPrebuildInfo {
    uint64_t resultDataMaxSizeBytes;
    uint64_t scratchDataSizeBytes;
    uint64_t updateScratchDataSizeBytes;
    uint32_t fromHardwareDriver;
};

struct XboxBvhTriangle {
    float v0[3];
    float v1[3];
    float v2[3];
    float color[3];
    float reflectivity;
};

struct XboxBvhInstanceDesc {
    uint64_t blasId;
    float transform[3][4];
    uint32_t instanceId;
    uint32_t instanceMask;
    uint32_t hitGroupContribution;
    uint32_t flags;
};

struct XboxBvhRay {
    float origin[3];
    float direction[3];
    float tMin;
    float tMax;
};

struct XboxBvhHit {
    uint32_t hit;
    float t;
    float u;
    float v;
    uint32_t triangleIndex;
    float normal[3];
    float color[3];
    float reflectivity;
};

struct XboxBvhDispatchDesc {
    uint32_t width;
    uint32_t height;
    uint32_t maxBounces;
    float cameraPos[3];
    float cameraLookAt[3];
    float lightPos[3];
};

struct XboxBvhDispatchStats {
    uint64_t primaryRays;
    uint64_t primaryHits;
    uint64_t shadowRays;
    uint64_t shadowHits;
    uint64_t reflectionRays;
    uint64_t aabbTests;
    uint64_t triangleTests;
    uint32_t bvhNodes;
    uint32_t bvhLeafNodes;
    uint32_t maxTreeDepth;
    uint64_t frameHash;
};

extern "C" {

BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhCreateBuilder(
    const XboxBvhCreateDesc* desc,
    XboxBvhBuilderHandle* outBuilder);
BVH_COMPAT_API void BVH_COMPAT_CALL XboxBvhDestroyBuilder(XboxBvhBuilderHandle builder);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhGetCapabilities(
    XboxBvhBuilderHandle builder,
    XboxBvhCapabilities* outCaps);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhGetPrebuildInfo(
    XboxBvhBuilderHandle builder,
    const XboxBvhPrebuildInputs* inputs,
    XboxBvhPrebuildInfo* outInfo);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhBuildBlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount,
    uint32_t buildFlags,
    uint64_t* outBlasId);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhUpdateBlas(
    XboxBvhBuilderHandle builder,
    uint64_t blasId,
    const XboxBvhTriangle* triangles,
    uint32_t triangleCount);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhBuildTlas(
    XboxBvhBuilderHandle builder,
    const XboxBvhInstanceDesc* instances,
    uint32_t instanceCount,
    uint32_t buildFlags,
    uint64_t* outTlasId);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhTraceRay(
    XboxBvhBuilderHandle builder,
    uint64_t asId,
    const XboxBvhRay* ray,
    XboxBvhHit* outHit);
BVH_COMPAT_API int32_t BVH_COMPAT_CALL XboxBvhDispatchRays(
    XboxBvhBuilderHandle builder,
    uint64_t tlasId,
    const XboxBvhDispatchDesc* desc,
    uint8_t* outRgba8,
    size_t outBufferBytes,
    XboxBvhDispatchStats* outStats);

}  // extern "C"
