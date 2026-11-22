#pragma once

// MIT AMD FidelityFX SDK 2.0 Loader & Upscaler (<ffx_api/ffx_api.h>) public C ABI header
// backed by amd_fidelityfx_loader_xbox.dll and amd_fidelityfx_upscaler_xbox.dll.

#include <cstddef>
#include <cstdint>

#define FFX_COMPAT_API __declspec(dllimport)
#define FFX_COMPAT_CALL __stdcall

typedef void* FfxContext;

enum FfxReturnCode : int32_t {
    FFX_API_RETURN_OK = 0,
    FFX_API_RETURN_ERROR = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
    FFX_API_RETURN_NO_PROVIDER = 4,
    FFX_API_RETURN_ERROR_MEMORY = 5,
    FFX_API_RETURN_ERROR_PARAMETER = 6,
};

constexpr uint32_t FFX_UPSCALER_FLAG_NONE = 0x0u;
constexpr uint32_t FFX_UPSCALER_FLAG_PREFER_WARP = 0x1u;
constexpr uint32_t FFX_UPSCALER_FLAG_ENABLE_RCAS = 0x2u;

enum FfxUpscalerQualityMode : uint32_t {
    FFX_UPSCALER_QUALITY_NATIVEAA = 0,
    FFX_UPSCALER_QUALITY_ULTRA_QUALITY = 1,
    FFX_UPSCALER_QUALITY_QUALITY = 2,
    FFX_UPSCALER_QUALITY_BALANCED = 3,
    FFX_UPSCALER_QUALITY_PERFORMANCE = 4,
    FFX_UPSCALER_QUALITY_ULTRA_PERFORMANCE = 5,
};

struct FfxUpscalerDispatchResult {
    uint64_t dispatchCount;
    uint64_t outputFnv1aHash;
    uint32_t easuGroupsX;
    uint32_t easuGroupsY;
    uint32_t rcasGroupsX;
    uint32_t rcasGroupsY;
    uint32_t usedGpuCompute;
    uint32_t isWarp;
    char adapterDescription[128];
};

struct FfxApiHeader {
    uint64_t type;
    FfxApiHeader* pNext;
};

typedef FfxApiHeader FfxCreateContextDescHeader;
typedef FfxApiHeader FfxConfigureDescHeader;
typedef FfxApiHeader FfxDispatchDescHeader;
typedef FfxApiHeader FfxQueryDescHeader;

typedef void*(FFX_COMPAT_CALL* FfxAllocFn)(void* pUserData, uint64_t size);
typedef void(FFX_COMPAT_CALL* FfxDeallocFn)(void* pUserData, void* pMem);

struct FfxAllocationCallbacks {
    void* pUserData;
    FfxAllocFn alloc;
    FfxDeallocFn dealloc;
};

enum FfxApiStructType : uint64_t {
    FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 = 0x00000002ull,
    FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE = 0x00010000ull,
    FFX_API_DISPATCH_DESC_TYPE_UPSCALE = 0x00010001ull,
    FFX_API_QUERY_DESC_TYPE_GET_VERSIONS = 0x00010002ull,
    FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE = 0x00010003ull,
    FFX_API_QUERY_DESC_TYPE_UPSCALE_GETADAPTERINFO = 0x00010004ull,
    FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE = 0x00010005ull,
};

enum FfxUpscaleConfigureKey : uint64_t {
    FFX_UPSCALE_CONFIGURE_KEY_SHARPNESS = 1ull,
    FFX_UPSCALE_CONFIGURE_KEY_ENABLE_RCAS = 2ull,
};

struct FfxCreateContextDescUpscale {
    FfxCreateContextDescHeader header;
    uint32_t flags;
    uint32_t maxRenderWidth;
    uint32_t maxRenderHeight;
    uint32_t maxUpscaleWidth;
    uint32_t maxUpscaleHeight;
    float defaultSharpness;
};

struct FfxDispatchDescUpscale {
    FfxDispatchDescHeader header;
    const uint8_t* inputRgba8;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint8_t* outputRgba8;
    size_t outputBufferBytes;
    uint32_t outputWidth;
    uint32_t outputHeight;
    float sharpness;
    uint32_t enableRcas;
    FfxUpscalerDispatchResult* outResult;
};

struct FfxConfigureDescUpscaleKeyValue {
    FfxConfigureDescHeader header;
    uint64_t key;
    uint64_t u64Value;
    void* ptr;
};

struct FfxQueryDescGetVersions {
    FfxQueryDescHeader header;
    uint64_t createDescType;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
};

struct FfxQueryDescUpscaleGetRenderResolutionFromQualityMode {
    FfxQueryDescHeader header;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t qualityMode;
    uint32_t* pOutRenderWidth;
    uint32_t* pOutRenderHeight;
};

struct FfxQueryDescUpscaleGetAdapterInfo {
    FfxQueryDescHeader header;
    uint32_t outIsWarp;
    char adapterDescription[128];
};

extern "C" {

FFX_COMPAT_API uint32_t FFX_COMPAT_CALL ffxUpscalerGetVersion(void);
FFX_COMPAT_API int32_t FFX_COMPAT_CALL ffxQuery(
    FfxContext* context,
    FfxQueryDescHeader* header);
FFX_COMPAT_API int32_t FFX_COMPAT_CALL ffxCreateContext(
    FfxContext* context,
    FfxCreateContextDescHeader* desc,
    const FfxAllocationCallbacks* memCb);
FFX_COMPAT_API int32_t FFX_COMPAT_CALL ffxDestroyContext(
    FfxContext* context,
    const FfxAllocationCallbacks* memCb);
FFX_COMPAT_API int32_t FFX_COMPAT_CALL ffxConfigure(
    FfxContext* context,
    const FfxConfigureDescHeader* desc);
FFX_COMPAT_API int32_t FFX_COMPAT_CALL ffxDispatch(
    FfxContext* context,
    const FfxDispatchDescHeader* desc);

}  // extern "C"
