#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcommon.h>
#include <cstdint>

// MIT GDKX public D3D12.XS declarations for compiling untouched
// _GAMING_XBOX_SCARLETT workloads against OpenScarlett's d3d12_xs.dll.
// Struct layouts and COM vtable slots match GXDK 221000+ / OpenScarlett d3d12_xs_abi.h.

#ifndef IID_GRAPHICS_PPV_ARGS
#define IID_GRAPHICS_PPV_ARGS(ppType) IID_PPV_ARGS(ppType)
#endif

#ifndef D3D12_SDK_VERSION
#define D3D12_SDK_VERSION 1
#endif

#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH ((HRESULT)0x887E0002L)
#endif

static constexpr UINT D3D12_PROCESS_DEBUG_FLAG_DEBUG_LAYER_ENABLED = 0x1u;
static constexpr UINT D3D12XBOX_PROCESS_DEBUG_FLAG_INSTRUMENTED = 0x4u;
static constexpr SIZE_T D3D12XBOX_DEFAULT_SIZE_BYTES = 0u;
static constexpr UINT D3D12XBOX_FRAME_INTERVAL_60_HZ = 16667u;
static constexpr UINT D3D12XBOX_FRAME_INTERVAL_FLAG_NONE = 0u;
static constexpr UINT D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE = 0u;
static constexpr UINT D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE = 0u;
static constexpr UINT D3D12XBOX_TEXTURE_DATA_PITCH_ALIGNMENT = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

// Xbox-exclusive resource flags (upper bits stripped cleanly by OpenScarlett DeviceX)
static const D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC =
    static_cast<D3D12_RESOURCE_FLAGS>(0x80000000u);
static const D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY =
    static_cast<D3D12_RESOURCE_FLAGS>(0x40000000u);
static const D3D12_RESOURCE_FLAGS D3D12XBOX_RESOURCE_FLAG_ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP =
    static_cast<D3D12_RESOURCE_FLAGS>(0x20000000u);

enum D3D12XBOX_CREATE_DEVICE_FLAGS : uint32_t {
    D3D12XBOX_CREATE_DEVICE_FLAG_NONE = 0u,
};

enum D3D12XBOX_FRAME_EVENT : uint32_t {
    D3D12XBOX_FRAME_EVENT_ORIGIN = 0u,
    D3D12XBOX_FRAME_EVENT_DISPLAY = 1u,
};

using D3D12XBOX_FRAME_PIPELINE_TOKEN = uint64_t;
static constexpr D3D12XBOX_FRAME_PIPELINE_TOKEN D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL = 0ull;

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

// Real GDKX exposes SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX directly on
// the device vtable (slots 73..75 on OpenScarlett's ID3D12DeviceX : ID3D12Device8) and
// PresentX (3-arg) / SuspendX / ResumeX directly on ID3D12CommandQueue (slots 19..21).
// Exact Windows/GDKX COM IIDs are preserved so live QueryInterface / D3D12XboxCreateDevice
// ABI is exercised verbatim.
struct __declspec(uuid("9218e6bb-f944-4f7e-a75c-b1b2c7b701f3")) GdkxDevice8 : public ID3D12Device8 {
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput, UINT PeriodMicroseconds, UINT MaxFrameLatency, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, UINT64 DelayMicroseconds, HANDLE hEvent, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, UINT TimeoutMilliseconds, void* pReserved, UINT Flags,
        D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) = 0;
};

struct __declspec(uuid("0ec870a6-5d7e-4c22-8cfc-5baae07616ed")) GdkxCommandQueue : public ID3D12CommandQueue {
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendX(UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeX() = 0;
};

// Both ID3D12Device (used by older samples like SimpleLighting) and ID3D12Device8
// (used by newer samples like SimpleTriangle) map to the GDKX device vtable.
#define ID3D12Device8 GdkxDevice8
#define ID3D12Device GdkxDevice8
#define ID3D12CommandQueue GdkxCommandQueue

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice);
