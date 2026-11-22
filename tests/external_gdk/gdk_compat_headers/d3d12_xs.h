#pragma once

// MIT Xbox Series X|S D3D12X (<d3d12_xs.h>) public COM ABI header.

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#define D3D12XS_COMPAT_API __declspec(dllimport)

using D3D12XBOX_FRAME_PIPELINE_TOKEN = uint64_t;
constexpr D3D12XBOX_FRAME_PIPELINE_TOKEN D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL = 0ull;

enum D3D12XBOX_PROCESS_DEBUG_FLAGS : uint32_t {
    D3D12XBOX_PROCESS_DEBUG_FLAG_NONE = 0x0u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG = 0x1u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_VALIDATED = 0x2u,
    D3D12XBOX_PROCESS_DEBUG_FLAG_INSTRUMENTED = 0x4u,
};

enum D3D12XBOX_FRAME_EVENT : uint32_t {
    D3D12XBOX_FRAME_EVENT_ORIGIN = 0u,
    D3D12XBOX_FRAME_EVENT_DISPLAY = 1u,
};

enum D3D12XBOX_PRESENT_FLAGS : uint32_t {
    D3D12XBOX_PRESENT_FLAG_NONE = 0x0u,
    D3D12XBOX_PRESENT_FLAG_IMMEDIATE = 0x1u,
};

enum D3D12XBOX_COLOR_SPACE : uint32_t {
    D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709 = 0u,
    D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 1u,
    D3D12XBOX_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 = 2u,
};

struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    uint32_t Version;
    uint32_t ProcessDebugFlags;
    uint32_t GraphicsCommandQueueRingSizeBytes;
    uint32_t GraphicsScratchMemorySizeBytes;
    uint32_t ComputeScratchMemorySizeBytes;
    BOOL DisableGeometryShaderAllocations;
    BOOL DisableTessellationShaderAllocations;
    BOOL DisableDXR;
    uint32_t CreateDeviceFlags;
    uint32_t AutoAttachPix;
    uint32_t AmplificationShaderIndirectArgsBufferSize;
    uint32_t AmplificationShaderPayloadBufferSize;
};

struct D3D12XBOX_GPU_HARDWARE_INFO {
    uint32_t VendorId;
    uint32_t DeviceId;
    uint64_t DedicatedVideoMemoryBytes;
    uint32_t MeshShaderTier;
    uint32_t RaytracingTier;
    uint32_t HighestShaderModel;
    uint32_t WaveLaneCountMin;
    uint32_t WaveLaneCountMax;
    uint32_t IsWarp;
    char AdapterName[128];
};

struct D3D12XBOX_PRESENT_PLANE_PARAMETERS {
    D3D12XBOX_FRAME_PIPELINE_TOKEN Token;
    uint32_t ResourceCount;
    ID3D12Resource* const* ppResources;
    DXGI_COLOR_SPACE_TYPE ColorSpace;
    D3D12_RECT SourceRect;
    D3D12_RECT DestRect;
    uint32_t Flags;
    D3D12_RESOURCE_STATES ResourceState;
};

struct D3D12XBOX_PRESENT_PARAMETERS {
    uint32_t Flags;
    uint32_t SyncInterval;
    uint32_t ImmediateThresholdPercent;
    HWND OutputWindow;
};

inline constexpr GUID IID_ID3D12DeviceX = {
    0x4F534431u, 0x3258u, 0x4445u, {0x56u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u}};
inline constexpr GUID IID_ID3D12CommandQueueX = {
    0x4F534431u, 0x3258u, 0x434Du, {0x51u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u}};
inline constexpr GUID IID_ID3D12GraphicsCommandListX = {
    0x4F534431u, 0x3258u, 0x434Du, {0x4Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u}};

struct __declspec(uuid("4F534431-3258-4445-5600-000000000001")) ID3D12DeviceX : public ID3D12Device8 {
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput,
        uint32_t PeriodMicroseconds,
        uint32_t MaxFrameLatency,
        uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event,
        uint64_t DelayMicroseconds,
        HANDLE hEvent,
        uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event,
        uint32_t TimeoutMilliseconds,
        void* pReserved,
        uint32_t Flags,
        D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) = 0;
};

struct __declspec(uuid("4F534431-3258-434D-5100-000000000002")) ID3D12CommandQueueX : public ID3D12CommandQueue {
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
};

struct __declspec(uuid("4F534431-3258-434D-4C00-000000000003")) ID3D12GraphicsCommandListX : public ID3D12GraphicsCommandList6 {
    virtual void STDMETHODCALLTYPE SetGpuVirtualAddressHintX(uint64_t Hint) = 0;
    virtual ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetUnderlyingHostCommandList() = 0;
};

extern "C" {

D3D12XS_COMPAT_API HRESULT WINAPI D3D12CreateDeviceX(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);

D3D12XS_COMPAT_API HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice);

D3D12XS_COMPAT_API void WINAPI OpenScarlettD3D12SetPreferWarp(int prefer_warp);

}  // extern "C"
