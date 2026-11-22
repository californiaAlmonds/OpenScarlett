#ifndef OPENSCARLETT_BUILDING_D3D12_XS
#define OPENSCARLETT_BUILDING_D3D12_XS 1
#endif
#include "openscarlett/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

#include "gpu/d3d12_xs/command_list_x.h"
#include "gpu/d3d12_xs/device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

#if defined(_MSC_VER)
#pragma comment(linker, "/EXPORT:D3D12CreateDevice=OS_D3D12CreateDevice")
#pragma comment(linker, "/EXPORT:D3D12SerializeRootSignature=OS_D3D12SerializeRootSignature")
#pragma comment(linker, "/EXPORT:D3D12SerializeVersionedRootSignature=OS_D3D12SerializeVersionedRootSignature")
#pragma comment(linker, "/EXPORT:D3D12CreateRootSignatureDeserializer=OS_D3D12CreateRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12CreateVersionedRootSignatureDeserializer=OS_D3D12CreateVersionedRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12GetDebugInterface=OS_D3D12GetDebugInterface")
#pragma comment(linker, "/EXPORT:D3D12EnableExperimentalFeatures=OS_D3D12EnableExperimentalFeatures")
#endif

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<int> g_prefer_warp{0};

inline void RecordExport(const char* sym, HRESULT hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        sym,
        outcome,
        static_cast<int32_t>(hr),
        -1);
}

constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;
constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;
constexpr double kPqMaxNits = 10000.0;

inline float SanitizeNonNegative(float v) noexcept {
    if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
    return v;
}

inline float ToneMapAcesChannel(float x) noexcept {
    x = SanitizeNonNegative(x);
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float num = x * (a * x + b);
    const float den = x * (c * x + d) + e;
    if (den <= 0.0f) return 0.0f;
    return std::clamp(num / den, 0.0f, 1.0f);
}

inline float ToneMapReinhardChannel(float x) noexcept {
    x = SanitizeNonNegative(x);
    return std::clamp(x / (1.0f + x), 0.0f, 1.0f);
}

inline float LinearToSrgbChannel(float x) noexcept {
    x = std::clamp(SanitizeNonNegative(x), 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

inline uint8_t FloatToU8(float v) noexcept {
    if (!std::isfinite(v)) return 0u;
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using PfnD3D12SerializeVersionedRootSignature = HRESULT(WINAPI*)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using PfnD3D12CreateRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12CreateVersionedRootSignatureDeserializer = HRESULT(WINAPI*)(
    LPCVOID, SIZE_T, REFIID, void**);
using PfnD3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
using PfnD3D12EnableExperimentalFeatures = HRESULT(WINAPI*)(
    UINT, const IID*, void*, UINT*);

bool EnvPrefersWarp() noexcept {
    char buf[32]{};
    const DWORD len = GetEnvironmentVariableA(
        "OPENSCARLETT_PREFER_WARP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

HRESULT CreateWrappedDeviceInternal(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL min_level,
    bool prefer_warp,
    bool enable_debug_layer,
    HWND default_hwnd,
    REFIID riid,
    void** ppDevice) {
    auto host_create = reinterpret_cast<PfnD3D12CreateDevice>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!host_create) {
        return E_FAIL;
    }

    // D3D12 probe mode (ppDevice == nullptr)
    if (!ppDevice) {
        return host_create(pAdapter, min_level, riid, nullptr);
    }
    *ppDevice = nullptr;

    ID3D12Device* host_dev = nullptr;
    IDXGIAdapter1* adapter1 = nullptr;
    openscarlett::D3D12AdapterInfo info{};

    if (pAdapter) {
        HRESULT hr = host_create(pAdapter, min_level, IID_PPV_ARGS(&host_dev));
        if (FAILED(hr) || !host_dev) {
            return hr;
        }
        pAdapter->QueryInterface(IID_PPV_ARGS(&adapter1));
        openscarlett::QueryD3D12DeviceAdapterInfo(host_dev, adapter1, info);
        if (adapter1) adapter1->Release();
    } else {
        openscarlett::D3D12AdapterSelectionOptions opts{};
        opts.prefer_warp = prefer_warp;
        opts.allow_warp_fallback = true;
        opts.enable_debug_layer = enable_debug_layer;
        opts.min_feature_level = static_cast<uint32_t>(min_level);
        openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
            opts, &host_dev, &adapter1, &info);
        if (adapter1) adapter1->Release();
        if (!st.ok() || !host_dev) {
            return DXGI_ERROR_UNSUPPORTED;
        }
    }

    auto* dev_x = new (std::nothrow) openscarlett::d3d12_xs::DeviceX(host_dev, info, default_hwnd);
    host_dev->Release();
    if (!dev_x) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = dev_x->QueryInterface(riid, ppDevice);
    dev_x->Release();
    return hr;
}

}  // namespace

extern "C" {

HRESULT WINAPI OS_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp(),
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDevice", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12CreateDeviceX(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice) {
    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        MinimumFeatureLevel,
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp(),
        false,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12CreateDeviceX", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    REFIID riid,
    void** ppDevice) {
    // Real GXDK 221000+ D3D12XBOX_CREATE_DEVICE_PARAMETERS does NOT carry OutputWindow or
    // PreferWarp fields — never misinterpret AmplificationShader* sizes as HWND/flags!
    const bool global_warp =
        (g_prefer_warp.load(std::memory_order_acquire) != 0) || EnvPrefersWarp();
    const bool debug_layer =
        (pParameters && (pParameters->ProcessDebugFlags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0);

    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        D3D_FEATURE_LEVEL_12_0,
        global_warp,
        debug_layer,
        nullptr,
        riid,
        ppDevice);
    RecordExport("D3D12XboxCreateDevice", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeVersionedRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
    const HRESULT hr = fn ? fn(pRootSignature, ppBlob, ppErrorBlob) : E_FAIL;
    RecordExport("D3D12SerializeVersionedRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData,
    SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
    auto fn = reinterpret_cast<PfnD3D12CreateVersionedRootSignatureDeserializer>(
        openscarlett::ResolveHostD3D12Proc("D3D12CreateVersionedRootSignatureDeserializer"));
    const HRESULT hr = fn
                           ? fn(pSrcData, SrcDataSizeInBytes,
                                pRootSignatureDeserializerInterface,
                                ppRootSignatureDeserializer)
                           : E_FAIL;
    RecordExport("D3D12CreateVersionedRootSignatureDeserializer", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    auto fn = reinterpret_cast<PfnD3D12GetDebugInterface>(
        openscarlett::ResolveHostD3D12Proc("D3D12GetDebugInterface"));
    const HRESULT hr = fn ? fn(riid, ppvDebug) : E_FAIL;
    RecordExport("D3D12GetDebugInterface", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12EnableExperimentalFeatures(
    UINT NumFeatures,
    const IID* pIIDs,
    void* pConfigurationStructs,
    UINT* pConfigurationStructSizes) {
    auto fn = reinterpret_cast<PfnD3D12EnableExperimentalFeatures>(
        openscarlett::ResolveHostD3D12Proc("D3D12EnableExperimentalFeatures"));
    const HRESULT hr = fn
                           ? fn(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes)
                           : E_FAIL;
    RecordExport("D3D12EnableExperimentalFeatures", hr);
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetPreferWarp(int prefer_warp) {
    g_prefer_warp.store(prefer_warp ? 1 : 0, std::memory_order_release);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetLastPresentHash(
    ID3D12CommandQueue* queue,
    uint64_t* out_hash,
    uint32_t* out_width,
    uint32_t* out_height) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->GetLastPresentStats(out_hash, out_width, out_height, nullptr);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_rgba8,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentCapture* capture = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                       reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = capture->CopyLastPresentPixels(out_rgba8, buffer_bytes, out_copied_bytes);
    capture->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ResetTelemetry() {
    openscarlett::CallTelemetry::Global().Reset();
}

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryEntry(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_vtable_slot) *out_vtable_slot = entry.vtable_slot;
            if (out_total_calls) *out_total_calls = entry.total_calls;
            return 1;
        }
    }
    if (out_vtable_slot) *out_vtable_slot = -1;
    if (out_total_calls) *out_total_calls = 0;
    return 0;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentWindowState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_WINDOW_STATE* out_state) {
    if (!queue) return E_INVALIDARG;
    if (!out_state) return E_POINTER;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->GetPresentWindowState(out_state);
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ToggleBorderlessFullscreen(
    ID3D12CommandQueue* queue) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->ToggleBorderlessFullscreen();
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HANDLE WINAPI OpenScarlettD3D12GetFrameLatencyWaitableObject(
    ID3D12CommandQueue* queue) {
    if (!queue) return nullptr;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return nullptr;
    }
    HANDLE h = win->GetFrameLatencyWaitableObject();
    win->Release();
    return h;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetWindowCloseBehavior(
    ID3D12CommandQueue* queue,
    int post_quit_on_close) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentWindow* win = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentWindow,
                                       reinterpret_cast<void**>(&win));
    if (FAILED(hr) || !win) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = win->SetWindowCloseBehavior(post_quit_on_close ? TRUE : FALSE);
    win->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetPresentConfig(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_PRESENTX_CONFIG* config) {
    if (!queue || !config) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->SetPresentConfig(config);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12SetHdrMetaData(
    ID3D12CommandQueue* queue,
    const D3D12XBOX_HDR10_METADATA* metadata) {
    if (!queue || !metadata) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->SetHdrMetaData(metadata);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetHdrSwapChainState(
    ID3D12CommandQueue* queue,
    D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state) {
    if (!queue) return E_INVALIDARG;
    if (!out_state) return E_POINTER;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->GetHdrSwapChainState(out_state);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CopyLastPresentRawPixels(
    ID3D12CommandQueue* queue,
    uint8_t* out_raw,
    uint32_t buffer_bytes,
    uint32_t* out_copied_bytes,
    DXGI_FORMAT* out_format) {
    if (!queue) return E_INVALIDARG;
    IOpenScarlettPresentHdr* hdr = nullptr;
    HRESULT hr = queue->QueryInterface(IID_IOpenScarlettPresentHdr,
                                       reinterpret_cast<void**>(&hdr));
    if (FAILED(hr) || !hdr) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = hdr->CopyLastPresentRawPixels(out_raw, buffer_bytes, out_copied_bytes, out_format);
    hdr->Release();
    return hr;
}

OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12LinearNitsToPqSt2084(float nits) {
    if (!std::isfinite(nits) || nits <= 0.0f) {
        return 0.0f;
    }
    const double y = std::clamp(static_cast<double>(nits) / kPqMaxNits, 0.0, 1.0);
    const double ym1 = std::pow(y, kPqM1);
    const double num = kPqC1 + kPqC2 * ym1;
    const double den = 1.0 + kPqC3 * ym1;
    const double pq = std::pow(num / den, kPqM2);
    return static_cast<float>(std::clamp(pq, 0.0, 1.0));
}

OPENSCARLETT_D3D12_API float WINAPI OpenScarlettD3D12PqSt2084ToLinearNits(float pq) {
    if (!std::isfinite(pq) || pq <= 0.0f) {
        return 0.0f;
    }
    const double n = std::clamp(static_cast<double>(pq), 0.0, 1.0);
    const double np = std::pow(n, 1.0 / kPqM2);
    const double num = std::max(np - kPqC1, 0.0);
    const double den = kPqC2 - kPqC3 * np;
    if (den <= 0.0) {
        return static_cast<float>(kPqMaxNits);
    }
    const double y = std::pow(num / den, 1.0 / kPqM1);
    return static_cast<float>(std::clamp(y * kPqMaxNits, 0.0, kPqMaxNits));
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
    float r,
    float g,
    float b,
    float a,
    D3D12XBOX_TONEMAP_MODE mode,
    uint8_t out_rgba8[4]) {
    if (!out_rgba8) return;
    float mr = 0.0f;
    float mg = 0.0f;
    float mb = 0.0f;
    switch (mode) {
        case D3D12XBOX_TONEMAP_REINHARD:
            mr = ToneMapReinhardChannel(r);
            mg = ToneMapReinhardChannel(g);
            mb = ToneMapReinhardChannel(b);
            break;
        case D3D12XBOX_TONEMAP_NONE:
            mr = std::clamp(SanitizeNonNegative(r), 0.0f, 1.0f);
            mg = std::clamp(SanitizeNonNegative(g), 0.0f, 1.0f);
            mb = std::clamp(SanitizeNonNegative(b), 0.0f, 1.0f);
            break;
        case D3D12XBOX_TONEMAP_AUTO:
        case D3D12XBOX_TONEMAP_ACES:
        default:
            mr = ToneMapAcesChannel(r);
            mg = ToneMapAcesChannel(g);
            mb = ToneMapAcesChannel(b);
            break;
    }
    out_rgba8[0] = FloatToU8(LinearToSrgbChannel(mr));
    out_rgba8[1] = FloatToU8(LinearToSrgbChannel(mg));
    out_rgba8[2] = FloatToU8(LinearToSrgbChannel(mb));
    out_rgba8[3] = FloatToU8(std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 1.0f);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12RegisterUnifiedTexture(
    ID3D12Device* device,
    const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) {
    if (!device || !desc) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->RegisterUnifiedTexture(desc);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12UnregisterUnifiedTexture(
    ID3D12Device* device,
    ID3D12Resource* resource) {
    if (!device || !resource) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->UnregisterUnifiedTexture(resource);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12NotifyVirtualRemap(
    ID3D12Device* device,
    uint64_t guest_va_base,
    uint64_t byte_size,
    uint64_t new_backing_va,
    uint32_t* out_flagged_count) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->NotifyVirtualRemap(guest_va_base, byte_size, new_backing_va, out_flagged_count);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12FlushDirtyUnifiedTextures(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    uint32_t* out_flushed_count) {
    if (!device || !queue) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->FlushDirtyUnifiedTextures(queue, out_flushed_count);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetUnifiedTextureStats(
    ID3D12Device* device,
    D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) {
    if (!device) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(IID_IOpenScarlettUnifiedMemory,
                                        reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) {
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }
    hr = um->GetUnifiedTextureStats(out_stats);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API int WINAPI OpenScarlettD3D12GetTelemetryDetail(
    const char* symbol_name,
    int32_t* out_vtable_slot,
    uint64_t* out_total_calls,
    uint64_t* out_ok_calls,
    uint64_t* out_not_impl_calls,
    uint64_t* out_error_calls,
    int32_t* out_last_hr) {
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.symbol_name == symbol_name) {
            if (out_vtable_slot) *out_vtable_slot = entry.vtable_slot;
            if (out_total_calls) *out_total_calls = entry.total_calls;
            if (out_ok_calls) *out_ok_calls = entry.ok_calls;
            if (out_not_impl_calls) *out_not_impl_calls = entry.not_impl_calls;
            if (out_error_calls) *out_error_calls = entry.error_calls;
            if (out_last_hr) *out_last_hr = entry.last_hr;
            return 1;
        }
    }
    if (out_vtable_slot) *out_vtable_slot = -1;
    if (out_total_calls) *out_total_calls = 0;
    if (out_ok_calls) *out_ok_calls = 0;
    if (out_not_impl_calls) *out_not_impl_calls = 0;
    if (out_error_calls) *out_error_calls = 0;
    if (out_last_hr) *out_last_hr = 0;
    return 0;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxCreateDmaCommandQueue(
    ID3D12Device* pDevice,
    const void* pDesc,
    REFIID riid,
    void** ppCommandQueue) {
    (void)pDevice;
    (void)pDesc;
    (void)riid;
    if (ppCommandQueue) {
        *ppCommandQueue = nullptr;
    }
    RecordExport("D3D12XboxCreateDmaCommandQueue", E_NOTIMPL);
    return E_NOTIMPL;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxEnableDccMetadata(
    ID3D12Device* pDevice,
    ID3D12Resource* pResource,
    uint32_t Flags) {
    (void)pDevice;
    (void)pResource;
    (void)Flags;
    RecordExport("D3D12XboxEnableDccMetadata", E_NOTIMPL);
    return E_NOTIMPL;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueKickoffX(
    ID3D12CommandQueue* queue) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->KickoffX();
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait32BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint32_t value,
    uint32_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->Wait32BitValueX(address, value, mask, operation);
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12QueueWait64BitValueX(
    ID3D12CommandQueue* queue,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    uint64_t value,
    uint64_t mask,
    D3D12XBOX_WAIT_VALUE_OP operation) {
    if (!queue) return E_INVALIDARG;
    ID3D12CommandQueueX* qx = nullptr;
    HRESULT hr = queue->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
    if (FAILED(hr) || !qx) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = qx->Wait64BitValueX(address, value, mask, operation);
    qx->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListKickoffX(
    ID3D12GraphicsCommandList* cmd_list) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    clx->KickoffX();
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListRSSetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float depth_bias,
    float depth_bias_clamp,
    float slope_scaled_depth_bias) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    clx->RSSetDepthBiasX(depth_bias, depth_bias_clamp, slope_scaled_depth_bias);
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12CommandListGetDepthBiasX(
    ID3D12GraphicsCommandList* cmd_list,
    float* out_depth_bias,
    float* out_depth_bias_clamp,
    float* out_slope_scaled_depth_bias) {
    if (!cmd_list) return E_INVALIDARG;
    ID3D12GraphicsCommandListX* clx = nullptr;
    HRESULT hr = cmd_list->QueryInterface(
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx));
    if (FAILED(hr) || !clx) return FAILED(hr) ? hr : E_NOINTERFACE;
    auto* impl = static_cast<openscarlett::d3d12_xs::CommandListX*>(clx);
    impl->GetDepthBiasXState(out_depth_bias, out_depth_bias_clamp, out_slope_scaled_depth_bias);
    clx->Release();
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12AttachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t byte_size,
    uint64_t* out_desktop_gpu_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->AttachXMemAllocation(guest_va, byte_size, out_desktop_gpu_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12DetachXMemAllocation(
    ID3D12Device* device,
    uint64_t guest_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->DetachXMemAllocation(guest_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12ResolveXMemGpuVa(
    ID3D12Device* device,
    uint64_t guest_va,
    uint64_t* out_desktop_gpu_va) {
    if (!device) return E_INVALIDARG;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->ResolveXMemGpuVa(guest_va, out_desktop_gpu_va);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetForceR9G9B9E5UavFallback(
    int force_fallback) {
    openscarlett::d3d12_xs::DeviceX::SetForceR9G9B9E5UavFallback(force_fallback != 0);
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetR9G9B9E5UavFallbackActive(
    ID3D12Device* device,
    int* out_active) {
    if (!device) return E_INVALIDARG;
    if (!out_active) return E_POINTER;
    IOpenScarlettUnifiedMemory* um = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettUnifiedMemory, reinterpret_cast<void**>(&um));
    if (FAILED(hr) || !um) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = um->GetR9G9B9E5UavFallbackActive(out_active);
    um->Release();
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetPresentTimingStats(
    ID3D12CommandQueue* queue,
    D3D12XBOX_PRESENT_TIMING_STATS* out_stats) {
    if (!queue) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettPresentTiming* timing = nullptr;
    HRESULT hr = queue->QueryInterface(
        IID_IOpenScarlettPresentTiming, reinterpret_cast<void**>(&timing));
    if (FAILED(hr) || !timing) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = timing->GetPresentTimingStats(out_stats);
    timing->Release();
    return hr;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12SetBackendMode(
    uint32_t backend_mode) {
    openscarlett::d3d12_xs::DeviceX::SetGlobalBackendMode(backend_mode);
}

OPENSCARLETT_D3D12_API uint32_t WINAPI OpenScarlettD3D12GetActiveBackend(
    ID3D12Device* device) {
    if (!device) {
        return openscarlett::d3d12_xs::DeviceX::ResolveEffectiveBackendMode();
    }
    IOpenScarlettVulkanRouting* routing = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettVulkanRouting, reinterpret_cast<void**>(&routing));
    if (FAILED(hr) || !routing) {
        return D3D12XBOX_BACKEND_D3D12;
    }
    const uint32_t mode = routing->GetActiveBackendMode();
    routing->Release();
    return mode;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetVulkanRoutingStats(
    ID3D12Device* device,
    D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) {
    if (!device) return E_INVALIDARG;
    if (!out_stats) return E_POINTER;
    IOpenScarlettVulkanRouting* routing = nullptr;
    HRESULT hr = device->QueryInterface(
        IID_IOpenScarlettVulkanRouting, reinterpret_cast<void**>(&routing));
    if (FAILED(hr) || !routing) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = routing->GetVulkanRoutingStats(out_stats);
    routing->Release();
    return hr;
}

}  // extern "C"
