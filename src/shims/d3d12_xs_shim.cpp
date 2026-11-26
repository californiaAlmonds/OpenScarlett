#ifndef OPENSCARLETT_BUILDING_D3D12_XS
#define OPENSCARLETT_BUILDING_D3D12_XS 1
#endif
#include "openscarlett/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#if defined(_MSC_VER)
#  include <intrin.h>
#endif
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/d3d12_xs/command_list_x.h"
#include "gpu/d3d12_xs/device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"

// DirectX 12 Agility SDK magic exports (public Microsoft D3D12 Agility SDK specification).
// Exported from d3d12_xs.dll so titles / D3D12 loader detect Agility SDK version & path.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 614u;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";

// Standard Microsoft Direct3D 12 COM interface GUID data symbols exported as DATA from
// d3d12_xs.dll for Xbox Series X|S binaries (such as UE5 Frosty-XSX-Shipping.exe) that
// statically import IID_ID3D12* from d3d12_xs.dll. Stored under OS_IID_* and aliased via
// linker /EXPORT:...,DATA (and d3d12_xs_mingw.def) to avoid duplicate-symbol collisions
// with uuid.lib / d3d12.lib while guaranteeing byte-exact Windows SDK GUIDs across toolchains.

// Base interfaces
extern const GUID OS_IID_ID3D12Object = {
    0xc4fec28fu, 0x7966u, 0x4e95u, {0x9fu, 0x94u, 0xf4u, 0x31u, 0xcbu, 0x56u, 0xc3u, 0xb8u}};
extern const GUID OS_IID_ID3D12DeviceChild = {
    0x905db94bu, 0xa00cu, 0x4140u, {0x9du, 0xf5u, 0x2bu, 0x64u, 0xcau, 0x9eu, 0xa3u, 0x57u}};
extern const GUID OS_IID_ID3D12Pageable = {
    0x63ee58fbu, 0x1268u, 0x4835u, {0x86u, 0xdau, 0xf0u, 0x08u, 0xceu, 0x62u, 0xf0u, 0xd6u}};

// Devices
extern const GUID OS_IID_ID3D12Device = {
    0x189819f1u, 0x1db6u, 0x4b57u, {0xbeu, 0x54u, 0x18u, 0x21u, 0x33u, 0x9bu, 0x85u, 0xf7u}};
extern const GUID OS_IID_ID3D12Device1 = {
    0x77acce80u, 0x638eu, 0x4e65u, {0x88u, 0x95u, 0xc1u, 0xf2u, 0x33u, 0x86u, 0x86u, 0x3eu}};
extern const GUID OS_IID_ID3D12Device2 = {
    0x30baa41eu, 0xb15bu, 0x475cu, {0xa0u, 0xbbu, 0x1au, 0xf5u, 0xc5u, 0xb6u, 0x43u, 0x28u}};
extern const GUID OS_IID_ID3D12Device3 = {
    0x81dadc15u, 0x2badu, 0x4392u, {0x93u, 0xc5u, 0x10u, 0x13u, 0x45u, 0xc4u, 0xaau, 0x98u}};
extern const GUID OS_IID_ID3D12Device4 = {
    0xe865df17u, 0xa9eeu, 0x46f9u, {0xa4u, 0x63u, 0x30u, 0x98u, 0x31u, 0x5au, 0xa2u, 0xe5u}};
extern const GUID OS_IID_ID3D12Device5 = {
    0x8b4f173bu, 0x2feau, 0x4b80u, {0x8fu, 0x58u, 0x43u, 0x07u, 0x19u, 0x1au, 0xb9u, 0x5du}};
extern const GUID OS_IID_ID3D12Device6 = {
    0xc70b221bu, 0x40e4u, 0x4a17u, {0x89u, 0xafu, 0x02u, 0x5au, 0x07u, 0x27u, 0xa6u, 0xdcu}};
extern const GUID OS_IID_ID3D12Device7 = {
    0x5c014b53u, 0x68a1u, 0x4b9bu, {0x8bu, 0xd1u, 0xddu, 0x60u, 0x46u, 0xb9u, 0x35u, 0x8bu}};
extern const GUID OS_IID_ID3D12Device8 = {
    0x9218e6bbu, 0xf944u, 0x4f7eu, {0xa7u, 0x5cu, 0xb1u, 0xb2u, 0xc7u, 0xb7u, 0x01u, 0xf3u}};
extern const GUID OS_IID_ID3D12Device9 = {
    0x4c80e962u, 0xf032u, 0x4f60u, {0xbcu, 0x9eu, 0xebu, 0xc2u, 0xcfu, 0xa1u, 0xd8u, 0x3cu}};
extern const GUID OS_IID_ID3D12Device10 = {
    0x517f8718u, 0xaa66u, 0x49f9u, {0xb0u, 0x2bu, 0xa7u, 0xabu, 0x89u, 0xc0u, 0x60u, 0x31u}};

// Command lists
extern const GUID OS_IID_ID3D12CommandList = {
    0x7116d91cu, 0xe7e4u, 0x47ceu, {0xb8u, 0xc6u, 0xecu, 0x81u, 0x68u, 0xf4u, 0x37u, 0xe5u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList = {
    0x5b160d0fu, 0xac1bu, 0x4185u, {0x8bu, 0xa8u, 0xb3u, 0xaeu, 0x42u, 0xa5u, 0xa4u, 0x55u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList1 = {
    0x553103fbu, 0x1fe7u, 0x4557u, {0xbbu, 0x38u, 0x94u, 0x6du, 0x7du, 0x0eu, 0x7cu, 0xa7u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList2 = {
    0x38c3e585u, 0xff17u, 0x412cu, {0x91u, 0x50u, 0x4fu, 0xc6u, 0xf9u, 0xd7u, 0x2au, 0x28u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList3 = {
    0x6fda83a7u, 0xb84cu, 0x4e38u, {0x9au, 0xc8u, 0xc7u, 0xbdu, 0x22u, 0x01u, 0x6bu, 0x3du}};
extern const GUID OS_IID_ID3D12GraphicsCommandList4 = {
    0x8754318eu, 0xd3a9u, 0x4541u, {0x98u, 0xcfu, 0x64u, 0x5bu, 0x50u, 0xdcu, 0x48u, 0x74u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList5 = {
    0x55050859u, 0x4024u, 0x474cu, {0x87u, 0xf5u, 0x64u, 0x72u, 0xeau, 0xeeu, 0x44u, 0xeau}};
extern const GUID OS_IID_ID3D12GraphicsCommandList6 = {
    0xc3827890u, 0xe548u, 0x4cfau, {0x96u, 0xcfu, 0x56u, 0x89u, 0xa9u, 0x37u, 0x0fu, 0x80u}};
extern const GUID OS_IID_ID3D12GraphicsCommandList7 = {
    0xdd171223u, 0x8b61u, 0x4769u, {0x90u, 0xe3u, 0x16u, 0x0cu, 0xcdu, 0xe4u, 0xe2u, 0xc1u}};

// Queue / Resource / Heap / Fence / PSO / RootSignature / Descriptor / Allocator / Query / State / Debug
extern const GUID OS_IID_ID3D12CommandQueue = {
    0x0ec870a6u, 0x5d7eu, 0x4c22u, {0x8cu, 0xfcu, 0x5bu, 0xaau, 0xe0u, 0x76u, 0x16u, 0xedu}};
extern const GUID OS_IID_ID3D12Resource = {
    0x696442beu, 0xa72eu, 0x4059u, {0xbcu, 0x79u, 0x5bu, 0x5cu, 0x98u, 0x04u, 0x0fu, 0xadu}};
extern const GUID OS_IID_ID3D12Resource1 = {
    0x9d5e227au, 0x4430u, 0x4161u, {0x88u, 0xb3u, 0x3eu, 0xcau, 0x6bu, 0xb1u, 0x6eu, 0x19u}};
extern const GUID OS_IID_ID3D12Resource2 = {
    0xbe36ec3bu, 0xea85u, 0x4aebu, {0xa4u, 0x5au, 0xe9u, 0xd7u, 0x64u, 0x04u, 0xa4u, 0x95u}};
extern const GUID OS_IID_ID3D12Heap = {
    0x6b3b2502u, 0x6e51u, 0x45b3u, {0x90u, 0xeeu, 0x98u, 0x84u, 0x26u, 0x5eu, 0x8du, 0xf3u}};
extern const GUID OS_IID_ID3D12Heap1 = {
    0x572f7389u, 0x2168u, 0x49e3u, {0x96u, 0x93u, 0xd6u, 0xdfu, 0x58u, 0x71u, 0xbfu, 0x6du}};
extern const GUID OS_IID_ID3D12Fence = {
    0x0a753dcfu, 0xc4d8u, 0x4b91u, {0xadu, 0xf6u, 0xbeu, 0x5au, 0x60u, 0xd9u, 0x5au, 0x76u}};
extern const GUID OS_IID_ID3D12Fence1 = {
    0x433685feu, 0xe22bu, 0x4ca0u, {0xa8u, 0xdbu, 0xb5u, 0xb4u, 0xf4u, 0xddu, 0x0eu, 0x4au}};
extern const GUID OS_IID_ID3D12PipelineState = {
    0x765a30f3u, 0xf624u, 0x4c6fu, {0xa8u, 0x28u, 0xacu, 0xe9u, 0x48u, 0x62u, 0x24u, 0x45u}};
extern const GUID OS_IID_ID3D12RootSignature = {
    0xc54a6b66u, 0x72dfu, 0x4ee8u, {0x8bu, 0xe5u, 0xa9u, 0x46u, 0xa1u, 0x42u, 0x92u, 0x14u}};
extern const GUID OS_IID_ID3D12DescriptorHeap = {
    0x8efb471du, 0x616cu, 0x4f49u, {0x90u, 0xf7u, 0x12u, 0x7bu, 0xb7u, 0x63u, 0xfau, 0x51u}};
extern const GUID OS_IID_ID3D12CommandAllocator = {
    0x6102dee4u, 0xaf59u, 0x4b09u, {0xb9u, 0x99u, 0xb4u, 0x4du, 0x73u, 0xf0u, 0x9bu, 0x24u}};
extern const GUID OS_IID_ID3D12QueryHeap = {
    0x0d9658aeu, 0xed45u, 0x469eu, {0xa6u, 0x1du, 0x97u, 0x0eu, 0xc5u, 0x83u, 0xcau, 0xb4u}};
extern const GUID OS_IID_ID3D12CommandSignature = {
    0xc36a797cu, 0xec80u, 0x4f0au, {0x89u, 0x85u, 0xa7u, 0xb2u, 0x47u, 0x50u, 0x82u, 0xd1u}};
extern const GUID OS_IID_ID3D12StateObject = {
    0x47016943u, 0xfca8u, 0x4594u, {0x93u, 0xeau, 0xafu, 0x25u, 0x8bu, 0x55u, 0x34u, 0x6du}};
extern const GUID OS_IID_ID3D12StateObjectProperties = {
    0xde5fa827u, 0x9bf9u, 0x4f26u, {0x89u, 0xffu, 0xd7u, 0xf5u, 0x6fu, 0xdeu, 0x38u, 0x60u}};
extern const GUID OS_IID_ID3D12Debug = {
    0x344488b7u, 0x6846u, 0x474bu, {0xb9u, 0x89u, 0xf0u, 0x27u, 0x44u, 0x82u, 0x45u, 0xe0u}};
extern const GUID OS_IID_ID3D12Debug1 = {
    0xaffaa4cau, 0x63feu, 0x4d8eu, {0xb8u, 0xadu, 0x15u, 0x90u, 0x00u, 0xafu, 0x43u, 0x04u}};
extern const GUID OS_IID_ID3D12InfoQueue = {
    0x0742a90bu, 0xc387u, 0x483fu, {0xb9u, 0x46u, 0x30u, 0xa7u, 0xe4u, 0xe6u, 0x14u, 0x58u}};
}

#if defined(_MSC_VER)
#pragma comment(linker, "/EXPORT:D3D12CreateDevice=OS_D3D12CreateDevice")
#pragma comment(linker, "/EXPORT:D3D12SerializeRootSignature=OS_D3D12SerializeRootSignature")
#pragma comment(linker, "/EXPORT:D3D12SerializeVersionedRootSignature=OS_D3D12SerializeVersionedRootSignature")
#pragma comment(linker, "/EXPORT:D3D12CreateRootSignatureDeserializer=OS_D3D12CreateRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12CreateVersionedRootSignatureDeserializer=OS_D3D12CreateVersionedRootSignatureDeserializer")
#pragma comment(linker, "/EXPORT:D3D12GetDebugInterface=OS_D3D12GetDebugInterface")
#pragma comment(linker, "/EXPORT:D3D12EnableExperimentalFeatures=OS_D3D12EnableExperimentalFeatures")

// Export COM interface GUIDs as DATA symbols (avoiding duplicate symbols with uuid.lib).
#pragma comment(linker, "/EXPORT:IID_ID3D12Object=OS_IID_ID3D12Object,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12DeviceChild=OS_IID_ID3D12DeviceChild,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Pageable=OS_IID_ID3D12Pageable,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device=OS_IID_ID3D12Device,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device1=OS_IID_ID3D12Device1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device2=OS_IID_ID3D12Device2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device3=OS_IID_ID3D12Device3,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device4=OS_IID_ID3D12Device4,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device5=OS_IID_ID3D12Device5,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device6=OS_IID_ID3D12Device6,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device7=OS_IID_ID3D12Device7,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device8=OS_IID_ID3D12Device8,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device9=OS_IID_ID3D12Device9,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Device10=OS_IID_ID3D12Device10,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandList=OS_IID_ID3D12CommandList,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList=OS_IID_ID3D12GraphicsCommandList,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList1=OS_IID_ID3D12GraphicsCommandList1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList2=OS_IID_ID3D12GraphicsCommandList2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList3=OS_IID_ID3D12GraphicsCommandList3,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList4=OS_IID_ID3D12GraphicsCommandList4,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList5=OS_IID_ID3D12GraphicsCommandList5,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList6=OS_IID_ID3D12GraphicsCommandList6,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12GraphicsCommandList7=OS_IID_ID3D12GraphicsCommandList7,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandQueue=OS_IID_ID3D12CommandQueue,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource=OS_IID_ID3D12Resource,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource1=OS_IID_ID3D12Resource1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Resource2=OS_IID_ID3D12Resource2,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Heap=OS_IID_ID3D12Heap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Heap1=OS_IID_ID3D12Heap1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Fence=OS_IID_ID3D12Fence,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Fence1=OS_IID_ID3D12Fence1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12PipelineState=OS_IID_ID3D12PipelineState,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12RootSignature=OS_IID_ID3D12RootSignature,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12DescriptorHeap=OS_IID_ID3D12DescriptorHeap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandAllocator=OS_IID_ID3D12CommandAllocator,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12QueryHeap=OS_IID_ID3D12QueryHeap,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12CommandSignature=OS_IID_ID3D12CommandSignature,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12StateObject=OS_IID_ID3D12StateObject,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12StateObjectProperties=OS_IID_ID3D12StateObjectProperties,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Debug=OS_IID_ID3D12Debug,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12Debug1=OS_IID_ID3D12Debug1,DATA")
#pragma comment(linker, "/EXPORT:IID_ID3D12InfoQueue=OS_IID_ID3D12InfoQueue,DATA")
#endif

namespace {

constexpr const char* kModule = "d3d12_xs.dll";
std::atomic<int> g_prefer_warp{0};

// Process-wide Xbox D3D12X state for D3D12Xbox* process/device/frame/error helpers.
std::atomic<uint32_t> g_process_gpu_priority{
    static_cast<uint32_t>(D3D12XBOX_PROCESS_GPU_PRIORITY_NORMAL)};
std::atomic<uint32_t> g_process_memory_priority{
    static_cast<uint32_t>(D3D12XBOX_PROCESS_MEMORY_PRIORITY_NORMAL)};
std::atomic<uint32_t> g_gpu_time_limit_us{0u};

struct FrameIntervalStore {
    std::mutex mutex;
    D3D12XBOX_FRAME_INTERVAL_STATE state{};
};

FrameIntervalStore& GetFrameIntervalStore() {
    static FrameIntervalStore store;
    return store;
}

struct ReportedErrorStore {
    std::mutex mutex;
    uint64_t count = 0ull;
    HRESULT last_hr = S_OK;
    std::string last_message;
};

ReportedErrorStore& GetReportedErrorStore() {
    static ReportedErrorStore store;
    return store;
}

// Private GUID for attaching a non-owning lifetime sentinel to DeviceX's underlying host
// ID3D12Device so D3D12XboxGetDevice / D3D12XboxSetFrameInterval never hold an extra COM
// reference on DeviceX (preserving immediate DeviceX::~DeviceX() teardown such as
// ShaderCache::WaitForPendingCompiles() and avoiding DLL_PROCESS_DETACH loader-lock issues).
// {4F534431-3258-4143-5444-455653454E54}
constexpr GUID kActiveDeviceSentinelGuid = {
    0x4F534431u, 0x3258u, 0x4143u, {0x54u, 0x44u, 0x45u, 0x56u, 0x53u, 0x45u, 0x4Eu, 0x54u}};

struct ActiveDeviceStore {
    std::mutex mutex;
    openscarlett::d3d12_xs::DeviceX* active_device = nullptr;
    uint64_t generation = 0ull;
};

ActiveDeviceStore& GetActiveDeviceStore() {
    static ActiveDeviceStore store;
    return store;
}

std::atomic<uint64_t> g_next_active_device_gen{1ull};

void ClearActiveDeviceIfGeneration(uint64_t generation) noexcept {
    auto& store = GetActiveDeviceStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    if (store.generation == generation) {
        store.active_device = nullptr;
        store.generation = 0ull;
    }
}

class ActiveDeviceSentinel final : public IUnknown {
public:
    explicit ActiveDeviceSentinel(uint64_t generation) noexcept : generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown))) {
            *ppvObject = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            ClearActiveDeviceIfGeneration(generation_);
            delete this;
        }
        return rem;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    uint64_t generation_ = 0ull;
};

void StoreActiveDevice(openscarlett::d3d12_xs::DeviceX* new_dev) noexcept {
    if (!new_dev) {
        auto& store = GetActiveDeviceStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        store.active_device = nullptr;
        store.generation = 0ull;
        return;
    }

    ID3D12Device* host_dev = new_dev->HostDevice();
    if (!host_dev) {
        return;
    }

    const uint64_t gen = g_next_active_device_gen.fetch_add(1ull, std::memory_order_relaxed);
    auto* sentinel = new (std::nothrow) ActiveDeviceSentinel(gen);
    if (!sentinel) {
        return;
    }

    // Attach sentinel OUTSIDE store.mutex so replacing any prior sentinel on the same host
    // device never deadlocks when the previous sentinel's Release() acquires store.mutex.
    const HRESULT hr = host_dev->SetPrivateDataInterface(kActiveDeviceSentinelGuid, sentinel);
    sentinel->Release();
    if (FAILED(hr)) {
        return;
    }

    auto& store = GetActiveDeviceStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.active_device = new_dev;
    store.generation = gen;
}

openscarlett::d3d12_xs::DeviceX* AcquireActiveDevice() noexcept {
    auto& store = GetActiveDeviceStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    if (store.active_device) {
        store.active_device->AddRef();
    }
    return store.active_device;
}

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
using PfnD3D12GetInterface = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

// Local clean-room definitions of public Microsoft Agility SDK CLSID/IID/COM interface so
// MSVC, Clang-CL, AND MinGW-w64 compile cleanly regardless of installed SDK header version.
constexpr GUID kClsidD3D12SDKConfiguration = {
    0x7cda6acau, 0xa03eu, 0x49c8u, {0x94u, 0x58u, 0x03u, 0x34u, 0xd2u, 0x0eu, 0x07u, 0xceu}};
constexpr GUID kIidID3D12SDKConfiguration = {
    0xe9eb5314u, 0x33aau, 0x42b2u, {0xa7u, 0x18u, 0xd7u, 0x7fu, 0x58u, 0xb1u, 0xf1u, 0xc7u}};

struct OS_ID3D12SDKConfiguration : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT SDKVersion, LPCSTR SDKPath) = 0;
};

struct AgilitySdkRuntimeState {
    std::once_flag init_once;
    std::atomic<bool> configured{false};
    std::atomic<bool> core_dll_found{false};
    std::atomic<bool> using_inbox_fallback{true};
    std::atomic<uint32_t> sdk_version{614u};
    std::atomic<HRESULT> last_hr{S_FALSE};
    std::mutex path_mutex;
    std::string discovered_dir;
};

AgilitySdkRuntimeState& GetAgilityState() {
    static AgilitySdkRuntimeState st;
    return st;
}

std::string GetModuleDirectoryA(HMODULE mod) {
    char buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameA(mod, buf, static_cast<DWORD>(sizeof(buf) - 1u));
    if (n == 0u || n >= sizeof(buf) - 1u) return {};
    std::error_code ec;
    const auto p = std::filesystem::path(buf).parent_path();
    return ec ? std::string() : p.string();
}

void TryConfigureAgilitySdkOnce() noexcept {
    auto& st = GetAgilityState();
    std::call_once(st.init_once, [&st]() {
        uint32_t desired_ver = D3D12SDKVersion;
        {
            char ver_buf[32]{};
            const DWORD vlen = GetEnvironmentVariableA(
                "OPENSCARLETT_AGILITY_SDK_VERSION", ver_buf, static_cast<DWORD>(sizeof(ver_buf)));
            if (vlen > 0u && vlen < sizeof(ver_buf)) {
                char* end = nullptr;
                const unsigned long parsed = std::strtoul(ver_buf, &end, 10);
                if (end && end != ver_buf && parsed > 0ul) {
                    desired_ver = static_cast<uint32_t>(parsed);
                }
            }
        }
        st.sdk_version.store(desired_ver, std::memory_order_release);

        // Build ordered opt-in candidate directories for D3D12Core.dll.
        std::vector<std::string> candidates;
        {
            char env_dir[MAX_PATH * 2]{};
            const DWORD elen = GetEnvironmentVariableA(
                "OPENSCARLETT_AGILITY_SDK_DIR", env_dir, static_cast<DWORD>(sizeof(env_dir)));
            if (elen > 0u && elen < sizeof(env_dir)) {
                candidates.emplace_back(env_dir, elen);
            }
        }

        HMODULE self_mod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&TryConfigureAgilitySdkOnce),
                &self_mod) != FALSE && self_mod != nullptr) {
            const std::string dll_dir = GetModuleDirectoryA(self_mod);
            if (!dll_dir.empty()) {
                candidates.push_back(dll_dir + "\\D3D12");
            }
        }

        const std::string exe_dir = GetModuleDirectoryA(nullptr);
        if (!exe_dir.empty()) {
            candidates.push_back(exe_dir + "\\D3D12");
        }
        candidates.emplace_back(".\\D3D12");

        std::string found_dir;
        for (const auto& dir : candidates) {
            if (dir.empty()) continue;
            std::error_code ec;
            const auto core_path = std::filesystem::path(dir) / "D3D12Core.dll";
            if (std::filesystem::exists(core_path, ec) && !ec) {
                found_dir = dir;
                break;
            }
        }

        if (found_dir.empty()) {
            // Graceful fallback to inbox d3d12.dll when D3D12Core.dll is absent.
            st.core_dll_found.store(false, std::memory_order_release);
            st.configured.store(false, std::memory_order_release);
            st.using_inbox_fallback.store(true, std::memory_order_release);
            st.last_hr.store(S_FALSE, std::memory_order_release);
            return;
        }

        st.core_dll_found.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(st.path_mutex);
            st.discovered_dir = found_dir;
        }

        auto get_iface = reinterpret_cast<PfnD3D12GetInterface>(
            openscarlett::ResolveHostD3D12Proc("D3D12GetInterface"));
        if (!get_iface) {
            st.using_inbox_fallback.store(true, std::memory_order_release);
            st.last_hr.store(E_NOINTERFACE, std::memory_order_release);
            return;
        }

        OS_ID3D12SDKConfiguration* sdk_cfg = nullptr;
        HRESULT hr = get_iface(
            kClsidD3D12SDKConfiguration,
            kIidID3D12SDKConfiguration,
            reinterpret_cast<void**>(&sdk_cfg));
        if (SUCCEEDED(hr) && sdk_cfg) {
            std::string sdk_path = found_dir;
            if (!sdk_path.empty() && sdk_path.back() != '\\' && sdk_path.back() != '/') {
                sdk_path.push_back('\\');
            }
            hr = sdk_cfg->SetSDKVersion(desired_ver, sdk_path.c_str());
            if (FAILED(hr)) {
                // Retry with canonical relative path ".\\D3D12\\" if absolute path wasn't accepted.
                hr = sdk_cfg->SetSDKVersion(desired_ver, D3D12SDKPath);
            }
            sdk_cfg->Release();
        }

        st.last_hr.store(hr, std::memory_order_release);
        if (SUCCEEDED(hr)) {
            st.configured.store(true, std::memory_order_release);
            st.using_inbox_fallback.store(false, std::memory_order_release);
        } else {
            st.configured.store(false, std::memory_order_release);
            st.using_inbox_fallback.store(true, std::memory_order_release);
        }
    });
}

bool EnvPrefersWarp() noexcept {
    char buf[32]{};
    const DWORD len = GetEnvironmentVariableA(
        "OPENSCARLETT_PREFER_WARP", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

bool EnvEnablesD3D12Debug() noexcept {
    char buf[16]{};
    const DWORD len = GetEnvironmentVariableA(
        "OPENSCARLETT_D3D12_DEBUG", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

inline D3D12_RESOURCE_DESC SanitizeRetailResourceDesc(const D3D12_RESOURCE_DESC& src) noexcept {
    D3D12_RESOURCE_DESC d = src;
    switch (static_cast<uint32_t>(d.Format)) {
        case 115u:  // DXGI_FORMAT_B4G4R4A4_UNORM (no RT/UAV support on desktop GPUs -> BGRA8)
            if ((static_cast<uint32_t>(d.Flags) &
                 (static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
                  static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) != 0u) {
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            break;
        case 116u:  // DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT
        case 117u:  // DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        case 118u:  // DXGI_FORMAT_D16_UNORM_S8_UINT
            d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            break;
        case 119u:  // DXGI_FORMAT_R16_UNORM_X8_TYPELESS
            d.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case 120u:  // DXGI_FORMAT_X16_TYPELESS_G8_UINT
            d.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            break;
        case 121u:  // DXGI_FORMAT_R16G8_TYPELESS
            d.Format = DXGI_FORMAT_R24G8_TYPELESS;
            break;
        default:
            break;
    }
    if (d.SampleDesc.Count == 0u) {
        d.SampleDesc.Count = 1u;
        d.SampleDesc.Quality = 0u;
    }
    uint32_t flags = static_cast<uint32_t>(d.Flags) & 0x0000FFFFu;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        flags &= ~static_cast<uint32_t>(
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    }
    if ((flags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0u) {
        flags &= ~static_cast<uint32_t>(
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    } else {
        flags &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    // Desktop D3D12 forbids ALLOW_UNORDERED_ACCESS on SRGB, MSAA, BC, and 16-bit packed (B5G6R5/B5G5R5A1/B4G4R4A4) formats.
    if ((flags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0u) {
        const bool no_uav_fmt =
            d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            d.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
            d.Format == DXGI_FORMAT_B5G6R5_UNORM ||
            d.Format == DXGI_FORMAT_B5G5R5A1_UNORM ||
            d.Format == DXGI_FORMAT_B4G4R4A4_UNORM ||
            d.Format == DXGI_FORMAT_A8_UNORM ||
            (static_cast<uint32_t>(d.Format) >= static_cast<uint32_t>(DXGI_FORMAT_BC1_TYPELESS) &&
             static_cast<uint32_t>(d.Format) <= static_cast<uint32_t>(DXGI_FORMAT_BC5_SNORM)) ||
            (static_cast<uint32_t>(d.Format) >= static_cast<uint32_t>(DXGI_FORMAT_BC6H_TYPELESS) &&
             static_cast<uint32_t>(d.Format) <= static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM_SRGB));
        if (no_uav_fmt || d.SampleDesc.Count > 1u) {
            flags &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }
    }
    if ((flags & static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0u ||
        d.SampleDesc.Count > 1u) {
        flags &= ~static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    }
    if (static_cast<uint32_t>(d.Layout) >= D3D12XBOX_TEXTURE_LAYOUT_GFX10_BASE ||
        (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)) {
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        d.Height = 1u;
        d.DepthOrArraySize = 1u;
        d.MipLevels = 1u;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        flags &= 0x000001FFu;
        flags &= ~static_cast<uint32_t>(
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    } else {
        flags &= 0x000000FFu;
    }
    d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return d;
}

inline D3D12_RESOURCE_STATES MapBarrierLayoutToResourceState(uint32_t initial_layout) noexcept {
    switch (initial_layout) {
        case 0u:           // D3D12_BARRIER_LAYOUT_COMMON
        case 0xFFFFFFFFu:  // D3D12_BARRIER_LAYOUT_UNDEFINED
            return D3D12_RESOURCE_STATE_COMMON;
        case 1u:           // D3D12_BARRIER_LAYOUT_GENERIC_READ
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case 2u:           // D3D12_BARRIER_LAYOUT_PRESENT
            return D3D12_RESOURCE_STATE_PRESENT;
        case 3u:           // D3D12_BARRIER_LAYOUT_RENDER_TARGET
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case 4u:           // D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case 5u:           // D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case 6u:           // D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case 7u:           // D3D12_BARRIER_LAYOUT_SHADER_RESOURCE
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case 8u:           // D3D12_BARRIER_LAYOUT_COPY_SOURCE
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case 9u:           // D3D12_BARRIER_LAYOUT_COPY_DEST
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case 10u:          // D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE
            return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case 11u:          // D3D12_BARRIER_LAYOUT_RESOLVE_DEST
            return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        case 12u:          // D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE
            return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

inline bool IsReadableRetailRange(uintptr_t addr, size_t byte_size) noexcept {
    if (addr < 0x10000u || byte_size == 0u) return false;
    if (addr > UINTPTR_MAX - byte_size) return false;
    uintptr_t cur = addr;
    const uintptr_t end = addr + byte_size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD kReadableMask =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & kReadableMask) == 0u ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
            return false;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + static_cast<uintptr_t>(mbi.RegionSize);
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
}

inline bool TryReadRetailDescriptorHeapExt(
    const D3D12_DESCRIPTOR_HEAP_DESC* desc,
    uint64_t& out_cpu_base,
    uint64_t& out_byte_size) noexcept {
    out_cpu_base = 0ull;
    out_byte_size = 0ull;
    if (!desc) return false;
    if (desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        return false;
    }
    if ((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        return false;
    }
    const uintptr_t ext_addr = reinterpret_cast<uintptr_t>(desc) + 0x10u;
    if (!IsReadableRetailRange(ext_addr, 16u)) {
        return false;
    }
    uint64_t cpu_base = 0ull;
    uint64_t byte_size = 0ull;
    std::memcpy(&cpu_base, reinterpret_cast<const void*>(ext_addr), sizeof(cpu_base));
    std::memcpy(&byte_size, reinterpret_cast<const void*>(ext_addr + 8u), sizeof(byte_size));
    if (cpu_base >= 0x10000ull && cpu_base < 0x00007FFFFFFFFFFFull &&
        byte_size >= 0x1000ull && byte_size <= 0x40000000ull) {
        out_cpu_base = cpu_base;
        out_byte_size = byte_size;
        return true;
    }
    return false;
}

// ===========================================================================
// Retail Xbox Series X (Scarlett d3d12_xs.h) ID3D12DescriptorHeap adapter
// preserving guest cpu_base_addr (+0x10) for GetCPUDescriptorHandleForHeapStart
// (slot 9) while returning the 8-byte handle in RAX to match Scarlett C-ABI.
// ===========================================================================
class ScarlettRetailDescriptorHeapProxy final : public ID3D12DescriptorHeap {
public:
    static constexpr size_t kVtblSlotCount = 11u;

    static void* const* GetPatchedVtbl(const void* const* raw_vtbl) {
        static void* s_vtbl[kVtblSlotCount]{};
        static std::once_flag s_once;
        std::call_once(s_once, [raw_vtbl]() {
            std::memcpy(s_vtbl, raw_vtbl, sizeof(s_vtbl));
            s_vtbl[8] = reinterpret_cast<void*>(&Thunk_GetDesc);
            s_vtbl[9] = reinterpret_cast<void*>(&Thunk_GetCPUDescriptorHandleForHeapStart);
            s_vtbl[10] = reinterpret_cast<void*>(&Thunk_GetGPUDescriptorHandleForHeapStart);
        });
        return s_vtbl;
    }

    static D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE Thunk_GetDesc(void* arg0, void* arg1) {
        ScarlettRetailDescriptorHeapProxy* self = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC* out = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
            self = static_cast<ScarlettRetailDescriptorHeapProxy*>(arg0);
            out = static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(arg1);
        } else {
            out = static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(arg0);
            self = static_cast<ScarlettRetailDescriptorHeapProxy*>(arg1);
        }
        if (out && self && self->inner_) {
            if (auto* dh = openscarlett::d3d12_xs::TryGetDescriptorHeapProxy(self->inner_)) {
                if (dh->IsVirtualUnboundedSamplerHeap() || !dh->HostDescriptorHeap()) {
                    *out = dh->CachedDesc();
                } else {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
                    dh->HostDescriptorHeap()->GetDesc(out);
#else
                    *out = dh->HostDescriptorHeap()->GetDesc();
#endif
                }
            } else {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
                self->inner_->GetDesc(out);
#else
                *out = self->inner_->GetDesc();
#endif
            }
        }
        return out;
    }

    static SIZE_T STDMETHODCALLTYPE Thunk_GetCPUDescriptorHandleForHeapStart(
        ScarlettRetailDescriptorHeapProxy* self) {
        if (!self) return 0u;
        if (self->guest_cpu_base_ != 0ull) {
            return static_cast<SIZE_T>(self->guest_cpu_base_);
        }
        if (self->inner_) {
            if (auto* dh = openscarlett::d3d12_xs::TryGetDescriptorHeapProxy(self->inner_)) {
                if (dh->IsVirtualUnboundedSamplerHeap()) return dh->VirtualCpuStart().ptr;
                return dh->HostDescriptorHeap()
                           ? dh->HostDescriptorHeap()->GetCPUDescriptorHandleForHeapStart().ptr
                           : 0u;
            }
            return self->inner_->GetCPUDescriptorHandleForHeapStart().ptr;
        }
        return 0u;
    }

    static UINT64 STDMETHODCALLTYPE Thunk_GetGPUDescriptorHandleForHeapStart(
        ScarlettRetailDescriptorHeapProxy* self) {
        if (!self) return 0ull;
        if (self->inner_) {
            if (auto* dh = openscarlett::d3d12_xs::TryGetDescriptorHeapProxy(self->inner_)) {
                if (dh->IsVirtualUnboundedSamplerHeap()) return dh->VirtualGpuStart().ptr;
                return dh->HostDescriptorHeap()
                           ? dh->HostDescriptorHeap()->GetGPUDescriptorHandleForHeapStart().ptr
                           : 0ull;
            }
            return self->inner_->GetGPUDescriptorHandleForHeapStart().ptr;
        }
        return 0ull;
    }

    static ID3D12DescriptorHeap* UnwrapIfRetailProxy(ID3D12DescriptorHeap* p) noexcept {
        if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000u || !s_patched_vtbl_ptr_) {
            return p;
        }
        if (*reinterpret_cast<void* const* const*>(p) == s_patched_vtbl_ptr_) {
            return static_cast<ScarlettRetailDescriptorHeapProxy*>(p)->inner_;
        }
        return p;
    }

    ScarlettRetailDescriptorHeapProxy(
        ID3D12DescriptorHeap* inner,
        uint64_t guest_cpu_base,
        uint64_t guest_byte_size)
        : inner_(inner),
          guest_cpu_base_(guest_cpu_base),
          guest_byte_size_(guest_byte_size) {
        if (inner_) {
            inner_->AddRef();
        }
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        void* const* patched = GetPatchedVtbl(raw_vtbl);
        s_patched_vtbl_ptr_ = patched;
        *reinterpret_cast<void* const**>(this) = patched;
    }

    ~ScarlettRetailDescriptorHeapProxy() {
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }
    }

    uint64_t GuestCpuBase() const noexcept { return guest_cpu_base_; }
    uint64_t GuestByteSize() const noexcept { return guest_byte_size_; }

    // IUnknown (0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D12Object)) ||
            IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualIID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualIID(riid, __uuidof(ID3D12DescriptorHeap))) {
            *ppvObject = static_cast<ID3D12DescriptorHeap*>(this);
            AddRef();
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    // ID3D12Object (3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return inner_ ? inner_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return inner_ ? inner_->SetPrivateData(guid, DataSize, pData) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return inner_ ? inner_->SetPrivateDataInterface(guid, pData) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return inner_ ? inner_->SetName(Name) : S_OK;
    }

    // ID3D12DeviceChild (7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        return inner_ ? inner_->GetDevice(riid, ppvDevice) : E_FAIL;
    }

    // ID3D12DescriptorHeap (8..10)
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE GetDesc(D3D12_DESCRIPTOR_HEAP_DESC* pRet) override {
        return Thunk_GetDesc(this, pRet);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart(
        D3D12_CPU_DESCRIPTOR_HANDLE* pRet) override {
        if (pRet) {
            pRet->ptr = Thunk_GetCPUDescriptorHandleForHeapStart(this);
        }
        return pRet;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart(
        D3D12_GPU_DESCRIPTOR_HANDLE* pRet) override {
        if (pRet) {
            pRet->ptr = Thunk_GetGPUDescriptorHandleForHeapStart(this);
        }
        return pRet;
    }
#else
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        (void)Thunk_GetDesc(this, &desc);
        return desc;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        D3D12_CPU_DESCRIPTOR_HANDLE h{};
        h.ptr = Thunk_GetCPUDescriptorHandleForHeapStart(this);
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        D3D12_GPU_DESCRIPTOR_HANDLE h{};
        h.ptr = Thunk_GetGPUDescriptorHandleForHeapStart(this);
        return h;
    }
#endif

private:
    static inline void* const* s_patched_vtbl_ptr_ = nullptr;
    std::atomic<ULONG> ref_count_{1u};
    ID3D12DescriptorHeap* inner_ = nullptr;
    uint64_t guest_cpu_base_ = 0ull;
    uint64_t guest_byte_size_ = 0ull;
};

// ===========================================================================
// Retail Xbox Series X (Scarlett d3d12_xs.h) ID3D12CommandQueue vtable adapter
// wrapping CommandQueueX. Maps exact retail Scarlett slots:
//   18 (+0x90): GetDesc (dual-ABI CINTERFACE / C++ sret)
//   21 (+0xA8): SuspendX(UINT Flags)
//   22 (+0xB0): ResumeX()
//   30 (+0xF0): PresentX(UINT PlaneCount, pPlaneParams, pPresentParams)
// ===========================================================================
class ScarlettRetailCommandQueueProxy final : public ID3D12CommandQueue {
public:
    static constexpr size_t kVtblSlotCount = 40u;

    static void* const* GetPatchedVtbl(const void* const* raw_vtbl, uint32_t retail_version) {
        if (retail_version >= 0x00020004u) {
            static void* s_vtbl_v4[kVtblSlotCount]{};
            static std::once_flag s_once_v4;
            std::call_once(s_once_v4, [raw_vtbl]() {
                std::memcpy(s_vtbl_v4, raw_vtbl, sizeof(s_vtbl_v4));
                // Xbox Series X|S Version >= 0x20004 inserts 2 slots before Signal (slots 14..15),
                // shifting Signal/Wait/GetTimestampFrequency/GetClockCalibration/GetDesc/SuspendX/
                // ResumeX/SignalX/PresentX by +2 (GetTimestampFrequency=18, GetDesc=20, PresentX=32).
                s_vtbl_v4[14] = reinterpret_cast<void*>(&Thunk_V4NoOpOk);
                s_vtbl_v4[15] = reinterpret_cast<void*>(&Thunk_V4NoOpOk);
                s_vtbl_v4[16] = const_cast<void*>(raw_vtbl[14]);  // Signal
                s_vtbl_v4[17] = const_cast<void*>(raw_vtbl[15]);  // Wait
                s_vtbl_v4[18] = const_cast<void*>(raw_vtbl[16]);  // GetTimestampFrequency
                s_vtbl_v4[19] = const_cast<void*>(raw_vtbl[17]);  // GetClockCalibration
                s_vtbl_v4[20] = reinterpret_cast<void*>(&Thunk_GetDesc);
                s_vtbl_v4[23] = const_cast<void*>(raw_vtbl[21]);  // SuspendX
                s_vtbl_v4[24] = const_cast<void*>(raw_vtbl[22]);  // ResumeX
                s_vtbl_v4[25] = const_cast<void*>(raw_vtbl[23]);  // SignalX
                s_vtbl_v4[32] = const_cast<void*>(raw_vtbl[30]);  // PresentX
            });
            return s_vtbl_v4;
        }
        static void* s_vtbl[kVtblSlotCount]{};
        static std::once_flag s_once;
        std::call_once(s_once, [raw_vtbl]() {
            std::memcpy(s_vtbl, raw_vtbl, sizeof(s_vtbl));
            s_vtbl[18] = reinterpret_cast<void*>(&Thunk_GetDesc);
        });
        return s_vtbl;
    }

    static HRESULT STDMETHODCALLTYPE Thunk_V4NoOpOk(ScarlettRetailCommandQueueProxy* /*self*/) {
        return S_OK;
    }

    static D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE Thunk_GetDesc(void* arg0, void* arg1) {
        ScarlettRetailCommandQueueProxy* self = nullptr;
        D3D12_COMMAND_QUEUE_DESC* out = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            (*reinterpret_cast<void* const* const*>(arg0) == s_patched_vtbl_ptr_ ||
             *reinterpret_cast<void* const* const*>(arg0) == s_patched_vtbl_v4_ptr_)) {
            self = static_cast<ScarlettRetailCommandQueueProxy*>(arg0);
            out = static_cast<D3D12_COMMAND_QUEUE_DESC*>(arg1);
        } else {
            out = static_cast<D3D12_COMMAND_QUEUE_DESC*>(arg0);
            self = static_cast<ScarlettRetailCommandQueueProxy*>(arg1);
        }
        if (out && self && self->inner_) {
            *out = self->inner_->GetDesc();
        }
        return out;
    }

    explicit ScarlettRetailCommandQueueProxy(ID3D12CommandQueueX* inner, uint32_t retail_version = 0u)
        : inner_(inner), retail_version_(retail_version) {
        if (inner_) {
            inner_->AddRef();
        }
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        void* const* patched = GetPatchedVtbl(raw_vtbl, retail_version_);
        if (retail_version_ >= 0x00020004u) {
            s_patched_vtbl_v4_ptr_ = patched;
        } else {
            s_patched_vtbl_ptr_ = patched;
        }
        *reinterpret_cast<void* const**>(this) = patched;
    }

    ~ScarlettRetailCommandQueueProxy() {
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }
    }

    // IUnknown (0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D12Object)) ||
            IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualIID(riid, __uuidof(ID3D12Pageable)) ||
            IsEqualIID(riid, __uuidof(ID3D12CommandQueue))) {
            *ppvObject = static_cast<ID3D12CommandQueue*>(this);
            AddRef();
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    // ID3D12Object (3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return inner_->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return inner_->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return inner_->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override { return inner_->SetName(Name); }

    // ID3D12DeviceChild (7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        return inner_->GetDevice(riid, ppvDevice);
    }

    // ID3D12CommandQueue (8..18)
    void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource* pResource, UINT NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
        ID3D12Heap* pHeap, UINT NumRanges,
        const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
        const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS Flags) override {
        inner_->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates,
                                   pResourceRegionSizes, pHeap, NumRanges, pRangeFlags,
                                   pHeapRangeStartOffsets, pRangeTileCounts, Flags);
    }
    void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override {
        inner_->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource,
                                 pSrcRegionStartCoordinate, pRegionSize, Flags);
    }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) override {
        inner_->ExecuteCommandLists(NumCommandLists, ppCommandLists);
    }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override {
        inner_->SetMarker(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override {
        inner_->BeginEvent(Metadata, pData, Size);
    }
    void STDMETHODCALLTYPE EndEvent() override { inner_->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override {
        return inner_->Signal(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override {
        return inner_->Wait(pFence, Value);
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override {
        return inner_->GetTimestampFrequency(pFrequency);
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override {
        return inner_->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* pRet) override {
        return inner_->GetDesc(pRet);
    }
#else
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return inner_->GetDesc(); }
#endif

    HRESULT RecordUnimpl(int slot, uintptr_t a1 = 0u, uintptr_t a2 = 0u, uintptr_t a3 = 0u, const void* ret = nullptr) {
        static std::atomic<uint32_t> s_qslot_counts[64]{};
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailCommandQueue::Slot_%d", slot);
        const uint32_t idx = (slot >= 0 && slot < 64) ? static_cast<uint32_t>(slot) : 63u;
        const uint32_t seen = s_qslot_counts[idx].fetch_add(1u, std::memory_order_relaxed);
        if (seen < 3u) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
            const uintptr_t rva = (ret && reinterpret_cast<uintptr_t>(ret) > base)
                                      ? (reinterpret_cast<uintptr_t>(ret) - base)
                                      : reinterpret_cast<uintptr_t>(ret);
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X) ret=+0x%llX a1=0x%llX a2=0x%llX a3=0x%llX\n",
                         buf, slot * 8,
                         static_cast<unsigned long long>(rva),
                         static_cast<unsigned long long>(a1),
                         static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3));
            std::fflush(stderr);
            OutputDebugStringA(buf);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", buf, openscarlett::CallOutcome::NotImplemented, E_NOTIMPL, slot);
        return E_NOTIMPL;
    }

    virtual HRESULT STDMETHODCALLTYPE Slot19() { return RecordUnimpl(19); }
    virtual HRESULT STDMETHODCALLTYPE Slot20() { return RecordUnimpl(20); }
    // Slot 21 (+0xA8): SuspendX
    virtual HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) { return inner_->SuspendX(Flags); }
    // Slot 22 (+0xB0): ResumeX
    virtual HRESULT STDMETHODCALLTYPE ResumeX() { return inner_->ResumeX(); }
    // Slot 23 (+0xB8): SignalX(ID3D12Fence* pFence, UINT64 Value, UINT FlushFlags)
    virtual HRESULT STDMETHODCALLTYPE SignalX(ID3D12Fence* pFence, UINT64 Value, UINT /*FlushFlags*/) {
        const HRESULT hr = inner_->Signal(pFence, Value);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCommandQueue::SignalX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 23);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot24() { return RecordUnimpl(24); }
    virtual HRESULT STDMETHODCALLTYPE Slot25() { return RecordUnimpl(25); }
    virtual HRESULT STDMETHODCALLTYPE Slot26() { return RecordUnimpl(26); }
    virtual HRESULT STDMETHODCALLTYPE Slot27() { return RecordUnimpl(27); }
    virtual HRESULT STDMETHODCALLTYPE Slot28() { return RecordUnimpl(28); }
    virtual HRESULT STDMETHODCALLTYPE Slot29() { return RecordUnimpl(29); }
    // Slot 30 (+0xF0): PresentX (also aliased to Slot 32 on Version >= 0x20004 via GetPatchedVtbl)
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        static std::atomic<uint32_t> s_px_calls{0u};
        const uint32_t idx = s_px_calls.fetch_add(1u, std::memory_order_relaxed);
        if (idx < 5u) {
            ID3D12Resource* r0 =
                (PlaneCount > 0u && pPlaneParameters && pPlaneParameters[0].ResourceCount > 0u &&
                 pPlaneParameters[0].ppResources)
                    ? pPlaneParameters[0].ppResources[0]
                    : nullptr;
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] PresentX(#%u: planes=%u pPlane=%p token=%llu resCnt=%u r0=%p pParams=%p)\n",
                         idx, PlaneCount, static_cast<const void*>(pPlaneParameters),
                         (PlaneCount > 0u && pPlaneParameters)
                             ? static_cast<unsigned long long>(pPlaneParameters[0].Token) : 0ull,
                         (PlaneCount > 0u && pPlaneParameters) ? pPlaneParameters[0].ResourceCount : 0u,
                         static_cast<void*>(r0),
                         static_cast<const void*>(pPresentParameters));
            std::fflush(stderr);
        }
        const uint64_t px_t0_us = []() noexcept -> uint64_t {
            LARGE_INTEGER c{}, f{};
            ::QueryPerformanceCounter(&c);
            ::QueryPerformanceFrequency(&f);
            return (f.QuadPart > 0) ? static_cast<uint64_t>((c.QuadPart * 1000000ull) / f.QuadPart) : 0ull;
        }();
        const HRESULT hr = inner_->PresentX(PlaneCount, pPlaneParameters, pPresentParameters);
        static const ULONGLONG s_t0 = ::GetTickCount64();
        if (idx < 8u || (idx % 10u) == 0u || (FAILED(hr) && idx < 15u)) {
            const uint64_t px_t1_us = []() noexcept -> uint64_t {
                LARGE_INTEGER c{}, f{};
                ::QueryPerformanceCounter(&c);
                ::QueryPerformanceFrequency(&f);
                return (f.QuadPart > 0) ? static_cast<uint64_t>((c.QuadPart * 1000000ull) / f.QuadPart) : 0ull;
            }();
            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] PresentX(#%u t=%llums px=%lluus) -> hr=0x%08lX\n",
                         idx, static_cast<unsigned long long>(::GetTickCount64() - s_t0),
                         static_cast<unsigned long long>(px_t1_us >= px_t0_us ? (px_t1_us - px_t0_us) : 0ull),
                         static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
        return hr;
    }
    // Slot 31 (+0xF8): UpdatePageMappingsBatchX(BatchCount, pBatches, PoolBaseVa, PoolPages, Flags)
    // Applies GPU tile-window page-mapping batches directly onto the GPU placeholder window
    // (adapted with attribution from MIT-licensed R0Y00/Xbox2Windows queue_page_mappings / X2WMapPageBatch)
    // so GPU-only tiles (Case 2) and CPU-unmapped / GPU-resident tiles (Case 0 -> Case 4) remain
    // backed by committed physical section views for DeviceX, DirectStorage, BVH, and shaders.
    virtual HRESULT STDMETHODCALLTYPE Slot31(
        UINT BatchCount,
        const void* pBatches,
        D3D12_GPU_VIRTUAL_ADDRESS BaseVa,
        UINT PageSizeOrFlags,
        UINT Reserved) {
        HRESULT hr = S_OK;
        if (BatchCount > 0u && pBatches != nullptr && BaseVa != 0ull) {
            using PfnMapTilePageBatch = int32_t(__stdcall*)(
                uint32_t, const void*, uint64_t, uint32_t, uint32_t);
            static PfnMapTilePageBatch s_map_batch = []() -> PfnMapTilePageBatch {
                HMODULE hkx = ::GetModuleHandleW(L"kernelx.dll");
                if (!hkx) hkx = ::LoadLibraryW(L"kernelx.dll");
                return hkx ? reinterpret_cast<PfnMapTilePageBatch>(
                                 ::GetProcAddress(hkx, "OpenScarlettMapTilePageBatch"))
                           : nullptr;
            }();
            if (s_map_batch) {
                hr = s_map_batch(BatchCount, pBatches, BaseVa, PageSizeOrFlags, Reserved);
                static std::atomic<uint32_t> s_slot31_logs{0u};
                const uint32_t log_idx = s_slot31_logs.fetch_add(1u, std::memory_order_relaxed);
                if (log_idx < 4u || FAILED(hr)) {
                    std::fprintf(
                        stderr,
                        "[OpenScarlett][d3d12_xs] Queue::UpdatePageMappingsBatchX_Slot31(#%u: batches=%u pool=0x%llX pages=%u flags=0x%X) -> hr=0x%08lX\n",
                        log_idx,
                        BatchCount,
                        static_cast<unsigned long long>(BaseVa),
                        PageSizeOrFlags,
                        Reserved,
                        static_cast<unsigned long>(hr));
                    std::fflush(stderr);
                }
            }
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCommandQueue::UpdatePageMappingsBatchX_Slot31",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 31);
        return hr;
    }
    // Slot 32 (+0x100): PresentX on Scarlett GRDK Version >= 0x20004 (kept stubbed on < 0x20004)
    virtual HRESULT STDMETHODCALLTYPE PresentX_Slot32(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        if (retail_version_ < 0x00020004u) {
            return RecordUnimpl(32);
        }
        static std::atomic<uint32_t> s_p32_calls{0u};
        const uint32_t idx = s_p32_calls.fetch_add(1u, std::memory_order_relaxed);
        const HRESULT hr = inner_->PresentX(PlaneCount, pPlaneParameters, pPresentParameters);
        if (idx < 5u || (FAILED(hr) && idx < 20u)) {
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] PresentX_Slot32(#%u: planes=%u pPlane=%p pParams=%p) -> hr=0x%08lX\n",
                         idx, PlaneCount, static_cast<const void*>(pPlaneParameters),
                         static_cast<const void*>(pPresentParameters),
                         static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCommandQueue::PresentX_Slot32",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 32);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot33() { return RecordUnimpl(33); }
    virtual HRESULT STDMETHODCALLTYPE Slot34() { return RecordUnimpl(34); }
    // Slot 35 (+0x118): ExecuteCommandListsX / SubmitCommandListsX(NumCommandLists, ppCommandLists, token, flags)
    virtual HRESULT STDMETHODCALLTYPE Slot35(
        UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists,
        uintptr_t /*token*/,
        UINT /*flags*/) {
        if (inner_ && NumCommandLists > 0u && ppCommandLists) {
            inner_->ExecuteCommandLists(NumCommandLists, ppCommandLists);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCommandQueue::ExecuteCommandListsX_Slot35",
            openscarlett::CallOutcome::Ok, S_OK, 35);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot36() { return RecordUnimpl(36); }
    virtual HRESULT STDMETHODCALLTYPE Slot37() { return RecordUnimpl(37); }
    virtual HRESULT STDMETHODCALLTYPE Slot38() { return RecordUnimpl(38); }
    virtual HRESULT STDMETHODCALLTYPE Slot39() { return RecordUnimpl(39); }

private:
    static inline void* const* s_patched_vtbl_ptr_ = nullptr;
    static inline void* const* s_patched_vtbl_v4_ptr_ = nullptr;
    std::atomic<ULONG> ref_count_{1u};
    ID3D12CommandQueueX* inner_ = nullptr;
    uint32_t retail_version_ = 0u;
};

// ===========================================================================
// Retail Xbox Series X (Scarlett d3d12_xs.h) ID3D12GraphicsCommandList adapter
// wrapping CommandListX.
// - Reserves zeroed +0x08..+0x187 header so inline Xbox PM4 fast-paths see
//   m_pCurrent (+0x18) == m_pLimit (+0x30) == 0 and branch to virtual methods.
// - Matches exact retail Scarlett vtable layout:
//     0..27  : identical to PC ID3D12GraphicsCommandList
//     28..31 : BeginQuery, EndQuery, ResolveQueryData, SetPredication
//     32..55 : SetDescriptorHeaps .. DiscardResource (PC 28..51 shifted +4)
//     56..59 : SetMarker, BeginEvent, EndEvent, ExecuteIndirect
// ===========================================================================
class ScarlettRetailCommandListProxy final : public IUnknown {
public:
    explicit ScarlettRetailCommandListProxy(openscarlett::d3d12_xs::CommandListX* inner)
        : inner_(inner) {
        if (inner_) {
            inner_->AddRef();
            openscarlett::d3d12_xs::CommandListX::RegisterExternalAlias(
                static_cast<const void*>(this), inner_);
        }
    }

    ~ScarlettRetailCommandListProxy() {
        openscarlett::d3d12_xs::CommandListX::UnregisterExternalAlias(
            static_cast<const void*>(this));
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }
    }

    // IUnknown (0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        // Xbox Series X ID3D12GraphicsCommandList5/6 use console-specific Data4 bytes
        // (verified live against UE5 FD3D12CommandListData constructor).
        static constexpr GUID kXboxIidCmdList5 = {
            0x55050859u, 0x4024u, 0x474Cu, {0x98u, 0xCFu, 0x64u, 0x5Bu, 0x51u, 0xDCu, 0x48u, 0x74u}};
        static constexpr GUID kXboxIidCmdList6 = {
            0xC3827890u, 0x4024u, 0x474Cu, {0x98u, 0xCFu, 0x64u, 0x5Bu, 0x51u, 0xDCu, 0x48u, 0x74u}};
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D12Object)) ||
            IsEqualIID(riid, __uuidof(ID3D12DeviceChild)) ||
            IsEqualIID(riid, __uuidof(ID3D12CommandList)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList1)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList2)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList3)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList4)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList5)) ||
            IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList6)) ||
            IsEqualIID(riid, kXboxIidCmdList5) ||
            IsEqualIID(riid, kXboxIidCmdList6)) {
            *ppvObject = static_cast<void*>(this);
            AddRef();
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    // ID3D12Object (3..6)
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* sz, void* d) { return inner_->GetPrivateData(g, sz, d); }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT sz, const void* d) { return inner_->SetPrivateData(g, sz, d); }
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) { return inner_->SetPrivateDataInterface(g, d); }
    virtual HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) { return inner_->SetName(n); }
    // ID3D12DeviceChild (7)
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** d) { return inner_->GetDevice(r, d); }
    // ID3D12CommandList (8)
    virtual D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() { return inner_->GetType(); }
    // ID3D12GraphicsCommandList (9..27: identical to PC)
    virtual HRESULT STDMETHODCALLTYPE Close() { return inner_->Close(); }
    virtual HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* s) { return inner_->Reset(a, s); }
    virtual void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* s) { inner_->ClearState(s); }
    virtual void STDMETHODCALLTYPE DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si) { inner_->DrawInstanced(vc, ic, sv, si); }
    virtual void STDMETHODCALLTYPE DrawIndexedInstanced(UINT ic, UINT inc, UINT si, INT bv, UINT sin) { inner_->DrawIndexedInstanced(ic, inc, si, bv, sin); }
    virtual void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) { inner_->Dispatch(x, y, z); }
    virtual void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* d, UINT64 doff, ID3D12Resource* s, UINT64 soff, UINT64 nb) { inner_->CopyBufferRegion(d, doff, s, soff, nb); }
    virtual void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) { inner_->CopyTextureRegion(d, x, y, z, s, b); }
    virtual void STDMETHODCALLTYPE CopyResource(ID3D12Resource* d, ID3D12Resource* s) { inner_->CopyResource(d, s); }
    virtual void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 boff, D3D12_TILE_COPY_FLAGS f) { inner_->CopyTiles(r, c, sz, b, boff, f); }
    virtual void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) { inner_->ResolveSubresource(d, ds, s, ss, f); }
    virtual void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) { inner_->IASetPrimitiveTopology(t); }
    virtual void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* v) { inner_->RSSetViewports(n, v); }
    virtual void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* r) { inner_->RSSetScissorRects(n, r); }
    virtual void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT b[4]) { inner_->OMSetBlendFactor(b); }
    virtual void STDMETHODCALLTYPE OMSetStencilRef(UINT r) { inner_->OMSetStencilRef(r); }
    virtual void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* s) { inner_->SetPipelineState(s); }
    virtual void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { inner_->ResourceBarrier(n, b); }
    virtual void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* b) { inner_->ExecuteBundle(b); }
    // Slots 28..31: retail Xbox Scarlett places Query/Predication here
    virtual void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT idx) { inner_->BeginQuery(qh, t, idx); }
    virtual void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT idx) { inner_->EndQuery(qh, t, idx); }
    virtual void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* qh, D3D12_QUERY_TYPE t, UINT si, UINT nq, ID3D12Resource* dst, UINT64 off) { inner_->ResolveQueryData(qh, t, si, nq, dst, off); }
    virtual void STDMETHODCALLTYPE SetPredication(ID3D12Resource* buf, UINT64 off, D3D12_PREDICATION_OP op) { inner_->SetPredication(buf, off, op); }
    // Slots 32..55: PC slots 28..51 shifted by +4
    virtual void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) {
        if (!h || n == 0u) {
            inner_->SetDescriptorHeaps(n, h);
            return;
        }
        constexpr UINT kStackHeaps = 8u;
        ID3D12DescriptorHeap* stack_heaps[kStackHeaps]{};
        std::vector<ID3D12DescriptorHeap*> heap_vec;
        ID3D12DescriptorHeap** unwrapped = stack_heaps;
        if (n > kStackHeaps) {
            heap_vec.resize(n);
            unwrapped = heap_vec.data();
        }
        for (UINT i = 0u; i < n; ++i) {
            ID3D12DescriptorHeap* cur = ScarlettRetailDescriptorHeapProxy::UnwrapIfRetailProxy(h[i]);
            unwrapped[i] = openscarlett::d3d12_xs::UnwrapDescriptorHeap(cur);
        }
        inner_->SetDescriptorHeaps(n, unwrapped);
    }
    virtual void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* rs) { inner_->SetComputeRootSignature(rs); }
    virtual void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* rs) { inner_->SetGraphicsRootSignature(rs); }
    virtual void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT rp, D3D12_GPU_DESCRIPTOR_HANDLE h) { inner_->SetComputeRootDescriptorTable(rp, h); }
    virtual void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT rp, D3D12_GPU_DESCRIPTOR_HANDLE h) { inner_->SetGraphicsRootDescriptorTable(rp, h); }
    virtual void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT rp, UINT v, UINT off) { inner_->SetComputeRoot32BitConstant(rp, v, off); }
    virtual void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT rp, UINT v, UINT off) { inner_->SetGraphicsRoot32BitConstant(rp, v, off); }
    virtual void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT rp, UINT n, const void* d, UINT off) { inner_->SetComputeRoot32BitConstants(rp, n, d, off); }
    virtual void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT rp, UINT n, const void* d, UINT off) { inner_->SetGraphicsRoot32BitConstants(rp, n, d, off); }
    virtual void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetComputeRootConstantBufferView(rp, va); }
    virtual void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetGraphicsRootConstantBufferView(rp, va); }
    virtual void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetComputeRootShaderResourceView(rp, va); }
    virtual void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetGraphicsRootShaderResourceView(rp, va); }
    virtual void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetComputeRootUnorderedAccessView(rp, va); }
    virtual void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT rp, D3D12_GPU_VIRTUAL_ADDRESS va) { inner_->SetGraphicsRootUnorderedAccessView(rp, va); }
    virtual void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) { inner_->IASetIndexBuffer(v); }
    virtual void STDMETHODCALLTYPE IASetVertexBuffers(UINT ss, UINT nv, const D3D12_VERTEX_BUFFER_VIEW* v) { inner_->IASetVertexBuffers(ss, nv, v); }
    virtual void STDMETHODCALLTYPE SOSetTargets(UINT ss, UINT nv, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { inner_->SOSetTargets(ss, nv, v); }
    virtual void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) { inner_->OMSetRenderTargets(n, rtv, single, dsv); }
    virtual void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT nr, const D3D12_RECT* r) { inner_->ClearDepthStencilView(dsv, f, d, s, nr, r); }
    virtual void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT c[4], UINT nr, const D3D12_RECT* r) { inner_->ClearRenderTargetView(rtv, c, nr, r); }
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const UINT v[4], UINT nr, const D3D12_RECT* r) { inner_->ClearUnorderedAccessViewUint(g, c, res, v, nr, r); }
    virtual void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const FLOAT v[4], UINT nr, const D3D12_RECT* r) { inner_->ClearUnorderedAccessViewFloat(g, c, res, v, nr, r); }
    virtual void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* r, const D3D12_DISCARD_REGION* reg) { inner_->DiscardResource(r, reg); }
    // Slots 56..59: identical to PC
    virtual void STDMETHODCALLTYPE SetMarker(UINT m, const void* d, UINT sz) { inner_->SetMarker(m, d, sz); }
    virtual void STDMETHODCALLTYPE BeginEvent(UINT m, const void* d, UINT sz) { inner_->BeginEvent(m, d, sz); }
    virtual void STDMETHODCALLTYPE EndEvent() { inner_->EndEvent(); }
    virtual void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* cs, UINT mc, ID3D12Resource* ab, UINT64 aoff, ID3D12Resource* cb, UINT64 coff) { inner_->ExecuteIndirect(cs, mc, ab, aoff, cb, coff); }

    // Safe telemetry stubs for any extended retail Scarlett slots 60..191
    void RecordClUnimpl(int slot, uintptr_t a1 = 0u, uintptr_t a2 = 0u, uintptr_t a3 = 0u, const void* ret = nullptr) {
        static std::atomic<uint32_t> s_cl_counts[256]{};
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailCmdList::Slot_%d", slot);
        const uint32_t idx = (slot >= 0 && slot < 256) ? static_cast<uint32_t>(slot) : 255u;
        const uint32_t seen = s_cl_counts[idx].fetch_add(1u, std::memory_order_relaxed);
        if (seen < 2u) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
            const uintptr_t rva = (ret && reinterpret_cast<uintptr_t>(ret) > base)
                                      ? (reinterpret_cast<uintptr_t>(ret) - base)
                                      : reinterpret_cast<uintptr_t>(ret);
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X) ret=+0x%llX a1=0x%llX a2=0x%llX a3=0x%llX\n",
                         buf, slot * 8,
                         static_cast<unsigned long long>(rva),
                         static_cast<unsigned long long>(a1),
                         static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3));
            std::fflush(stderr);
            OutputDebugStringA(buf);
        }
        if (inner_) {
            inner_->RecordUnsupportedExtensionIgnored(buf, slot);
        } else {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", buf, openscarlett::CallOutcome::NotImplemented, E_NOTIMPL, slot);
        }
    }
    virtual void STDMETHODCALLTYPE Slot60() { RecordClUnimpl(60); }
    // Slot 61 (+0x1E8): ExecuteIndirect (retail GRDK 2504+ +2 shifted from PC slot 59)
    virtual void STDMETHODCALLTYPE Slot61(
        ID3D12CommandSignature* cs, UINT mc,
        ID3D12Resource* ab, UINT64 aoff,
        ID3D12Resource* cb, UINT64 coff) {
        if (inner_) {
            inner_->ExecuteIndirect(cs, mc, ab, aoff, cb, coff);
        }
    }
    // Slot 62 (+0x1F0): GetCommandBufferGpuVirtualAddressX / status query on retail Scarlett ID3D12GraphicsCommandList
    virtual uint64_t STDMETHODCALLTYPE Slot62() {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::GetStatusX_Slot62",
            openscarlett::CallOutcome::Ok, S_OK, 62);
        return 0ull;
    }
    virtual void STDMETHODCALLTYPE Slot63() { RecordClUnimpl(63); }
    virtual void STDMETHODCALLTYPE Slot64() { RecordClUnimpl(64); }
    virtual void STDMETHODCALLTYPE Slot65() { RecordClUnimpl(65); }
    virtual void STDMETHODCALLTYPE Slot66() { RecordClUnimpl(66); }
    virtual void STDMETHODCALLTYPE Slot67() { RecordClUnimpl(67); }
    virtual void STDMETHODCALLTYPE Slot68() { RecordClUnimpl(68); }
    virtual void STDMETHODCALLTYPE Slot69() { RecordClUnimpl(69); }
    // Slot 70 (+0x230): OMSetDepthBounds(FLOAT Min, FLOAT Max) on retail Scarlett ID3D12GraphicsCommandList
    virtual void STDMETHODCALLTYPE Slot70(FLOAT Min, FLOAT Max) {
        if (inner_) {
            inner_->OMSetDepthBounds(Min, Max);
        }
    }
    // Slot 71 (+0x238): Write32BitValueBottomOfPipeX(Address, Value, Flags, Reserved)
    virtual void STDMETHODCALLTYPE Slot71(
        D3D12_GPU_VIRTUAL_ADDRESS Address, uint32_t Value, uint32_t Flags, uint32_t /*Reserved*/) {
        if (inner_) {
            inner_->Write32BitValueX(Address, Value, Flags);
        }
    }
    // Slot 72 (+0x240): Write64BitValueBottomOfPipeX / WriteTimestampToMemoryX(Address, Value, Reserved, Flags)
    virtual void STDMETHODCALLTYPE Slot72(
        D3D12_GPU_VIRTUAL_ADDRESS Address, uint64_t Value, uint32_t /*Reserved*/, uint32_t Flags) {
        if (inner_) {
            inner_->Write64BitValueX(Address, Value, Flags);
        }
    }
    // Slot 73 (+0x248): Wait32BitValueX(Address, Op, Ref, Mask, Flags)
    virtual void STDMETHODCALLTYPE Slot73(
        D3D12_GPU_VIRTUAL_ADDRESS Address, uint32_t Op, uint32_t Ref, uint32_t Mask, uint32_t /*Flags*/) {
        if (inner_ && Address != 0ull) {
            inner_->Wait32BitValueX(Address, Ref, Mask, static_cast<D3D12XBOX_WAIT_VALUE_OP>(Op));
        }
    }
    // Slot 74 (+0x250): Wait64BitValueX(Address, Op, Ref, Mask, Flags)
    virtual void STDMETHODCALLTYPE Slot74(
        D3D12_GPU_VIRTUAL_ADDRESS Address, uint32_t /*Op*/, uint64_t /*Ref*/, uint64_t /*Mask*/, uint32_t /*Flags*/) {
        // Non-blocking on CPU wait when wait-op is ALWAYS or title-side sync; log telemetry cleanly.
        if (inner_ && Address != 0ull) {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", "ScarlettRetailCmdList::Wait64BitValueX_Slot74",
                openscarlett::CallOutcome::Ok, S_OK, 74);
        }
    }
    // Slot 75 (+0x258): OMSetDepthBoundsEnableX(BOOL Enable)
    virtual void STDMETHODCALLTYPE Slot75(BOOL /*Enable*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::OMSetDepthBoundsEnableX_Slot75",
            openscarlett::CallOutcome::Ok, S_OK, 75);
    }
    virtual void STDMETHODCALLTYPE Slot76() { RecordClUnimpl(76); }
    virtual void STDMETHODCALLTYPE Slot77() { RecordClUnimpl(77); }
    virtual void STDMETHODCALLTYPE Slot78() { RecordClUnimpl(78); }
    virtual void STDMETHODCALLTYPE Slot79() { RecordClUnimpl(79); }
    virtual void STDMETHODCALLTYPE Slot80() { RecordClUnimpl(80); }
    virtual void STDMETHODCALLTYPE Slot81() { RecordClUnimpl(81); }
    virtual void STDMETHODCALLTYPE Slot82() { RecordClUnimpl(82); }
    // Slot 83 (+0x298): CopyTextureRegionX(pDst, DstX, DstY, DstZ, pSrc, pSrcBox, Flags)
    virtual void STDMETHODCALLTYPE CopyTextureRegionX(
        const D3D12_TEXTURE_COPY_LOCATION* pDst,
        UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc,
        const D3D12_BOX* pSrcBox,
        UINT /*Flags*/) {
        inner_->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
    }
    virtual void STDMETHODCALLTYPE Slot84() { RecordClUnimpl(84); }
    virtual void STDMETHODCALLTYPE Slot85() { RecordClUnimpl(85); }
    virtual void STDMETHODCALLTYPE Slot86() { RecordClUnimpl(86); }
    virtual void STDMETHODCALLTYPE Slot87() { RecordClUnimpl(87); }
    virtual void STDMETHODCALLTYPE Slot88() { RecordClUnimpl(88); }
    virtual void STDMETHODCALLTYPE Slot89() { RecordClUnimpl(89); }
    virtual void STDMETHODCALLTYPE Slot90() { RecordClUnimpl(90); }
    virtual void STDMETHODCALLTYPE Slot91() { RecordClUnimpl(91); }
    virtual void STDMETHODCALLTYPE Slot92() { RecordClUnimpl(92); }
    virtual void STDMETHODCALLTYPE Slot93() { RecordClUnimpl(93); }
    virtual void STDMETHODCALLTYPE Slot94() { RecordClUnimpl(94); }
    virtual void STDMETHODCALLTYPE Slot95() { RecordClUnimpl(95); }
    virtual void STDMETHODCALLTYPE Slot96() { RecordClUnimpl(96); }
    virtual void STDMETHODCALLTYPE Slot97() { RecordClUnimpl(97); }
    virtual void STDMETHODCALLTYPE Slot98() { RecordClUnimpl(98); }
    virtual void STDMETHODCALLTYPE Slot99() { RecordClUnimpl(99); }
    virtual void STDMETHODCALLTYPE Slot100() { RecordClUnimpl(100); }
    virtual void STDMETHODCALLTYPE Slot101() { RecordClUnimpl(101); }
    virtual void STDMETHODCALLTYPE Slot102() { RecordClUnimpl(102); }
    virtual void STDMETHODCALLTYPE Slot103() { RecordClUnimpl(103); }
    virtual void STDMETHODCALLTYPE Slot104() { RecordClUnimpl(104); }
    // Retail Scarlett ID3D12GraphicsCommandList1..6 (slots 105..124 = PC slots 60..79 shifted +45)
    virtual void STDMETHODCALLTYPE AtomicCopyBufferUINT(
        ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT dep,
        ID3D12Resource* const* dr, const D3D12_SUBRESOURCE_RANGE_UINT64* sr) {
        inner_->AtomicCopyBufferUINT(d, dof, s, sof, dep, dr, sr);
    }
    virtual void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
        ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT dep,
        ID3D12Resource* const* dr, const D3D12_SUBRESOURCE_RANGE_UINT64* sr) {
        inner_->AtomicCopyBufferUINT64(d, dof, s, sof, dep, dr, sr);
    }
    virtual void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT mn, FLOAT mx) { inner_->OMSetDepthBounds(mn, mx); }
    virtual void STDMETHODCALLTYPE SetSamplePositions(UINT ns, UINT np, D3D12_SAMPLE_POSITION* sp) {
        inner_->SetSamplePositions(ns, np, sp);
    }
    virtual void STDMETHODCALLTYPE ResolveSubresourceRegion(
        ID3D12Resource* d, UINT ds, UINT x, UINT y, ID3D12Resource* s, UINT ss,
        D3D12_RECT* sr, DXGI_FORMAT f, D3D12_RESOLVE_MODE rm) {
        inner_->ResolveSubresourceRegion(d, ds, x, y, s, ss, sr, f, rm);
    }
    virtual void STDMETHODCALLTYPE SetViewInstanceMask(UINT m) { inner_->SetViewInstanceMask(m); }
    virtual void STDMETHODCALLTYPE WriteBufferImmediate(
        UINT c, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p, const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) {
        inner_->WriteBufferImmediate(c, p, m);
    }
    virtual void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession* ps) {
        inner_->SetProtectedResourceSession(ps);
    }
    virtual void STDMETHODCALLTYPE BeginRenderPass(
        UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* r,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d, D3D12_RENDER_PASS_FLAGS f) {
        inner_->BeginRenderPass(n, r, d, f);
    }
    virtual void STDMETHODCALLTYPE EndRenderPass() { inner_->EndRenderPass(); }
    virtual void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand* mc, const void* d, SIZE_T s) {
        inner_->InitializeMetaCommand(mc, d, s);
    }
    virtual void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand* mc, const void* d, SIZE_T s) {
        inner_->ExecuteMetaCommand(mc, d, s);
    }
    virtual void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) {
        inner_->BuildRaytracingAccelerationStructure(d, n, p);
    }
    virtual void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n,
        const D3D12_GPU_VIRTUAL_ADDRESS* s) {
        inner_->EmitRaytracingAccelerationStructurePostbuildInfo(d, n, s);
    }
    // Slot 119 (+0x3B8): CopyRaytracingAccelerationStructure
    virtual void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) {
        inner_->CopyRaytracingAccelerationStructure(d, s, m);
    }
    virtual void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* so) { inner_->SetPipelineState1(so); }
    virtual void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) { inner_->DispatchRays(d); }
    virtual void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE r, const D3D12_SHADING_RATE_COMBINER* c) {
        inner_->RSSetShadingRate(r, c);
    }
    virtual void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource* r) { inner_->RSSetShadingRateImage(r); }
    virtual void STDMETHODCALLTYPE DispatchMesh(UINT x, UINT y, UINT z) { inner_->DispatchMesh(x, y, z); }
    virtual void STDMETHODCALLTYPE Slot125() { RecordClUnimpl(125); }
    // Slot 126 (+0x3F0): RSSetShadingRate on retail GRDK 2504+ ID3D12GraphicsCommandList
    virtual void STDMETHODCALLTYPE Slot126(
        D3D12_SHADING_RATE BaseShadingRate,
        const D3D12_SHADING_RATE_COMBINER* pCombiners) {
        if (inner_ && static_cast<uint32_t>(BaseShadingRate) <= static_cast<uint32_t>(D3D12_SHADING_RATE_4X4)) {
            inner_->RSSetShadingRate(BaseShadingRate, pCombiners);
        } else {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", "ScarlettRetailCmdList::RSSetShadingRate_Slot126",
                openscarlett::CallOutcome::Ok, S_OK, 126);
        }
    }
    virtual void STDMETHODCALLTYPE Slot127(ID3D12Resource* r) {
        if (inner_) inner_->RSSetShadingRateImage(r);
    }
    virtual void STDMETHODCALLTYPE Slot128() { RecordClUnimpl(128); }
    // Slot 129 (+0x408): DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ) on retail GRDK 2504+
    virtual void STDMETHODCALLTYPE Slot129(
        UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
        static std::atomic<uint32_t> s_dm129_logs{0u};
        const uint32_t n = s_dm129_logs.fetch_add(1u, std::memory_order_relaxed);
        if (n < 6u) {
            const void* ret = _ReturnAddress();
            const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
            const uintptr_t rva = (ret && reinterpret_cast<uintptr_t>(ret) > base)
                                      ? (reinterpret_cast<uintptr_t>(ret) - base)
                                      : reinterpret_cast<uintptr_t>(ret);
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] DispatchMesh_Slot129 #%u ret=+0x%llX (%u, %u, %u)\n",
                         n, static_cast<unsigned long long>(rva),
                         ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
            std::fflush(stderr);
        }
        if (inner_ &&
            ThreadGroupCountX > 0u && ThreadGroupCountX <= 65535u &&
            ThreadGroupCountY > 0u && ThreadGroupCountY <= 65535u &&
            ThreadGroupCountZ > 0u && ThreadGroupCountZ <= 65535u) {
            inner_->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        } else {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", "ScarlettRetailCmdList::DispatchMesh_Slot129",
                openscarlett::CallOutcome::Ok, S_OK, 129);
        }
    }
    virtual void STDMETHODCALLTYPE Slot130() { RecordClUnimpl(130); }
    // Slot 131 (+0x418): Xbox ID3D12GraphicsCommandList6::BuildRaytracingAccelerationStructuresX
    virtual void STDMETHODCALLTYPE BuildRaytracingAccelerationStructuresX(
        UINT /*NumDescs*/, const void* /*pDescs*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::BuildRaytracingAccelerationStructuresX",
            openscarlett::CallOutcome::Ok, S_OK, 131);
    }
    // Slot 132 (+0x420): SetCommandBufferRingSizeX(UINT RingSizeInBytes, UINT Flags, UINT Reserved)
    virtual void STDMETHODCALLTYPE Slot132(uintptr_t /*a1*/, uintptr_t /*a2*/, uintptr_t /*a3*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::SetCommandBufferRingSizeX_Slot132",
            openscarlett::CallOutcome::Ok, S_OK, 132);
    }
    virtual void STDMETHODCALLTYPE Slot133() { RecordClUnimpl(133); }
    virtual void STDMETHODCALLTYPE Slot134() { RecordClUnimpl(134); }
    virtual void STDMETHODCALLTYPE Slot135() { RecordClUnimpl(135); }
    virtual void STDMETHODCALLTYPE Slot136() { RecordClUnimpl(136); }
    virtual void STDMETHODCALLTYPE Slot137() { RecordClUnimpl(137); }
    virtual void STDMETHODCALLTYPE Slot138() { RecordClUnimpl(138); }
    // Slot 139 (+0x458): ResetX(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState, UINT Flags)
    virtual HRESULT STDMETHODCALLTYPE Slot139(
        ID3D12CommandAllocator* pAllocator,
        ID3D12PipelineState* pInitialState,
        UINT /*Flags*/) {
        return inner_ ? inner_->Reset(pAllocator, pInitialState) : S_OK;
    }
    virtual void STDMETHODCALLTYPE Slot140() { RecordClUnimpl(140); }
    virtual void STDMETHODCALLTYPE Slot141() { RecordClUnimpl(141); }
    // Slot 142 (+0x470): SetSampleMaskX(UINT64 SampleMask)
    virtual void STDMETHODCALLTYPE Slot142(uint64_t /*SampleMask*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::SetSampleMaskX_Slot142",
            openscarlett::CallOutcome::Ok, S_OK, 142);
    }
    virtual void STDMETHODCALLTYPE Slot143() { RecordClUnimpl(143); }
    virtual void STDMETHODCALLTYPE Slot144() { RecordClUnimpl(144); }
    virtual void STDMETHODCALLTYPE Slot145() { RecordClUnimpl(145); }
    // Slot 146 (+0x490): ID3D12GraphicsCommandList7::Barrier (Enhanced Barriers)
    virtual void STDMETHODCALLTYPE Barrier(
        UINT32 NumBarrierGroups, const void* pBarrierGroups) {
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
        if (inner_) {
            inner_->ExecuteEnhancedBarriers(
                NumBarrierGroups, static_cast<const D3D12_BARRIER_GROUP*>(pBarrierGroups));
            return;
        }
#else
        (void)NumBarrierGroups;
        (void)pBarrierGroups;
#endif
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::Barrier",
            openscarlett::CallOutcome::Ok, S_OK, 146);
    }
    virtual void STDMETHODCALLTYPE Slot147() { RecordClUnimpl(147); }
    virtual void STDMETHODCALLTYPE Slot148() { RecordClUnimpl(148); }
    virtual void STDMETHODCALLTYPE Slot149() { RecordClUnimpl(149); }
    virtual void STDMETHODCALLTYPE Slot150() { RecordClUnimpl(150); }
    virtual void STDMETHODCALLTYPE Slot151() { RecordClUnimpl(151); }
    virtual void STDMETHODCALLTYPE Slot152() { RecordClUnimpl(152); }
    virtual void STDMETHODCALLTYPE Slot153() { RecordClUnimpl(153); }
    virtual void STDMETHODCALLTYPE Slot154() { RecordClUnimpl(154); }
    virtual void STDMETHODCALLTYPE Slot155() { RecordClUnimpl(155); }
    virtual void STDMETHODCALLTYPE Slot156() { RecordClUnimpl(156); }
    virtual void STDMETHODCALLTYPE Slot157() { RecordClUnimpl(157); }
    virtual void STDMETHODCALLTYPE Slot158() { RecordClUnimpl(158); }
    virtual void STDMETHODCALLTYPE Slot159() { RecordClUnimpl(159); }
    virtual void STDMETHODCALLTYPE Slot160() { RecordClUnimpl(160); }
    virtual void STDMETHODCALLTYPE Slot161() { RecordClUnimpl(161); }
    virtual void STDMETHODCALLTYPE Slot162() { RecordClUnimpl(162); }
    virtual void STDMETHODCALLTYPE Slot163() { RecordClUnimpl(163); }
    virtual void STDMETHODCALLTYPE Slot164() { RecordClUnimpl(164); }
    virtual void STDMETHODCALLTYPE Slot165() { RecordClUnimpl(165); }
    virtual void STDMETHODCALLTYPE Slot166() { RecordClUnimpl(166); }
    virtual void STDMETHODCALLTYPE Slot167() { RecordClUnimpl(167); }
    virtual void STDMETHODCALLTYPE Slot168() { RecordClUnimpl(168); }
    virtual void STDMETHODCALLTYPE Slot169() { RecordClUnimpl(169); }
    virtual void STDMETHODCALLTYPE Slot170() { RecordClUnimpl(170); }
    virtual void STDMETHODCALLTYPE Slot171() { RecordClUnimpl(171); }
    virtual void STDMETHODCALLTYPE Slot172() { RecordClUnimpl(172); }
    virtual void STDMETHODCALLTYPE Slot173() { RecordClUnimpl(173); }
    virtual void STDMETHODCALLTYPE Slot174() { RecordClUnimpl(174); }
    virtual void STDMETHODCALLTYPE Slot175() { RecordClUnimpl(175); }
    virtual void STDMETHODCALLTYPE Slot176() { RecordClUnimpl(176); }
    virtual void STDMETHODCALLTYPE Slot177() { RecordClUnimpl(177); }
    virtual void STDMETHODCALLTYPE Slot178() { RecordClUnimpl(178); }
    virtual void STDMETHODCALLTYPE Slot179() { RecordClUnimpl(179); }
    virtual void STDMETHODCALLTYPE Slot180() { RecordClUnimpl(180); }
    virtual void STDMETHODCALLTYPE Slot181() { RecordClUnimpl(181); }
    virtual void STDMETHODCALLTYPE Slot182() { RecordClUnimpl(182); }
    virtual void STDMETHODCALLTYPE Slot183() { RecordClUnimpl(183); }
    virtual void STDMETHODCALLTYPE Slot184() { RecordClUnimpl(184); }
    virtual void STDMETHODCALLTYPE Slot185() { RecordClUnimpl(185); }
    virtual void STDMETHODCALLTYPE Slot186() { RecordClUnimpl(186); }
    virtual void STDMETHODCALLTYPE Slot187() { RecordClUnimpl(187); }
    virtual void STDMETHODCALLTYPE Slot188() { RecordClUnimpl(188); }
    virtual void STDMETHODCALLTYPE Slot189() { RecordClUnimpl(189); }
    virtual void STDMETHODCALLTYPE Slot190() { RecordClUnimpl(190); }
    virtual void STDMETHODCALLTYPE Slot191() { RecordClUnimpl(191); }

private:
    // MUST remain the first member so +0x18 (m_pCurrent) and +0x30 (m_pLimit) are both 0!
    uint8_t pm4_inline_zeros_[0x800]{};
    std::atomic<ULONG> ref_count_{1u};
    openscarlett::d3d12_xs::CommandListX* inner_ = nullptr;
};

// ===========================================================================
// Retail Xbox Series X (Scarlett d3d12_xs.h, Version >= 0x00020000) ID3D12Device
// vtable adapter wrapping DeviceX. Preserves PC/GDKX-test ID3D12Device8/DeviceX
// layout for Version < 0x00020000 while exposing exact retail Scarlett vtable
// slots (62: CreateComputePipelineStateX, 69: SetDriverFlagX, 73: CreateCommandListX,
// 78: SetFrameIntervalX, 79: ScheduleFrameEventX, 80: WaitFrameEventX,
// 81: GetFrameStatisticsX) for real Xbox Series X binaries.
// ===========================================================================
class ScarlettRetailDeviceProxy final : public ID3D12Device {
public:
    static constexpr size_t kVtblSlotCount = 192u;

    static void* const* GetPatchedVtbl(const void* const* raw_vtbl) {
        static void* s_vtbl[kVtblSlotCount]{};
        static std::once_flag s_once;
        std::call_once(s_once, [raw_vtbl]() {
            std::memcpy(s_vtbl, raw_vtbl, sizeof(s_vtbl));
            s_vtbl[25] = reinterpret_cast<void*>(&Thunk_GetResourceAllocationInfo);
            s_vtbl[26] = reinterpret_cast<void*>(&Thunk_GetCustomHeapProperties);
            s_vtbl[43] = reinterpret_cast<void*>(&Thunk_GetAdapterLuid);
            s_vtbl[95] = reinterpret_cast<void*>(&Thunk_GetResourceAllocationInfo1);
            s_vtbl[108] = reinterpret_cast<void*>(&Thunk_GetResourceAllocationInfo2);
        });
        return s_vtbl;
    }

    static uint64_t ComputeRdna2PlaneAllocBytes(
        D3D12_RESOURCE_DIMENSION dim,
        uint64_t width,
        uint32_t height,
        uint32_t depth_or_array,
        uint32_t mip_levels,
        uint32_t samples,
        uint32_t bpb,
        uint32_t block_dim) noexcept {
        const uint64_t w0 = std::max<uint64_t>(1ull, width);
        const uint32_t h0 = std::max<uint32_t>(1u, height);
        const uint32_t d0 = (dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                                ? std::max<uint32_t>(1u, depth_or_array)
                                : 1u;
        const uint32_t slices = (dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                                    ? 1u
                                    : std::max<uint32_t>(1u, depth_or_array);
        uint32_t mips = mip_levels;
        if (mips == 0u) {
            uint64_t m = std::max<uint64_t>(w0, std::max<uint64_t>(h0, d0));
            mips = 1u;
            while (m > 1ull) {
                m >>= 1u;
                ++mips;
            }
        }
        const uint32_t samp = std::max<uint32_t>(1u, samples);

        if (dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            uint64_t tx = 16ull, ty = 16ull, tz = 16ull;
            if (bpb <= 1u) { tx = 64ull; ty = 32ull; tz = 32ull; }
            else if (bpb <= 2u) { tx = 32ull; ty = 32ull; tz = 32ull; }
            else if (bpb <= 4u) { tx = 32ull; ty = 32ull; tz = 16ull; }
            else if (bpb <= 8u) { tx = 32ull; ty = 16ull; tz = 16ull; }
            uint64_t large_bytes = 0ull;
            uint64_t tail_bytes = 0ull;
            for (uint32_t m = 0u; m < mips; ++m) {
                const uint64_t mw = std::max<uint64_t>(1ull, w0 >> m);
                const uint64_t mh = std::max<uint64_t>(1ull, static_cast<uint64_t>(h0) >> m);
                const uint64_t md = std::max<uint64_t>(1ull, static_cast<uint64_t>(d0) >> m);
                const uint64_t bw = (mw + block_dim - 1ull) / block_dim;
                const uint64_t bh = (mh + block_dim - 1ull) / block_dim;
                const uint64_t raw = bw * bh * md * bpb;
                if (raw >= 65536ull || mips == 1u) {
                    const uint64_t aw = (bw + tx - 1ull) & ~(tx - 1ull);
                    const uint64_t ah = (bh + ty - 1ull) & ~(ty - 1ull);
                    const uint64_t ad = (md + tz - 1ull) & ~(tz - 1ull);
                    const uint64_t padded = (aw * ah * ad * bpb + 65535ull) & ~65535ull;
                    large_bytes += padded;
                } else {
                    tail_bytes += (raw + 255ull) & ~255ull;
                }
            }
            if (tail_bytes > 0ull) {
                large_bytes += (tail_bytes + 65535ull) & ~65535ull;
            }
            return std::max<uint64_t>(65536ull, large_bytes);
        }

        uint64_t tw = 64ull, th = 64ull;
        if (bpb <= 1u) { tw = 256ull; th = 256ull; }
        else if (bpb <= 2u) { tw = 256ull; th = 128ull; }
        else if (bpb <= 4u) { tw = 128ull; th = 128ull; }
        else if (bpb <= 8u) { tw = 128ull; th = 64ull; }
        uint32_t rem_s = samp;
        while (rem_s > 1u) {
            if (tw >= th && tw > 8ull) tw >>= 1u;
            else if (th > 8ull) th >>= 1u;
            rem_s >>= 1u;
        }

        uint64_t slice_bytes = 0ull;
        uint64_t tail_bytes = 0ull;
        for (uint32_t m = 0u; m < mips; ++m) {
            const uint64_t mw = std::max<uint64_t>(1ull, w0 >> m);
            const uint64_t mh = (dim == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                                    ? 1ull
                                    : std::max<uint64_t>(1ull, static_cast<uint64_t>(h0) >> m);
            const uint64_t bw = (mw + block_dim - 1ull) / block_dim;
            const uint64_t bh = (mh + block_dim - 1ull) / block_dim;
            const uint64_t raw = bw * bh * bpb * samp;
            if (raw >= 65536ull || mips == 1u) {
                const uint64_t aw = (bw + tw - 1ull) & ~(tw - 1ull);
                const uint64_t ah = (dim == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                                        ? 1ull
                                        : ((bh + th - 1ull) & ~(th - 1ull));
                const uint64_t padded = (aw * ah * bpb * samp + 65535ull) & ~65535ull;
                slice_bytes += padded;
            } else {
                tail_bytes += (raw + 255ull) & ~255ull;
            }
        }
        if (tail_bytes > 0ull) {
            slice_bytes += (tail_bytes + 65535ull) & ~65535ull;
        }
        slice_bytes = std::max<uint64_t>(65536ull, slice_bytes);
        return slice_bytes * static_cast<uint64_t>(slices);
    }

    static D3D12_RESOURCE_ALLOCATION_INFO ComputeRetailSingleAllocInfo(
        openscarlett::d3d12_xs::DeviceX* /*inner*/,
        UINT /*visibleMask*/,
        const D3D12_RESOURCE_DESC& raw_desc) noexcept {
        if (raw_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            uint64_t align = 256ull;
            if (raw_desc.Alignment >= 16ull && raw_desc.Alignment <= 65536ull &&
                (raw_desc.Alignment & (raw_desc.Alignment - 1ull)) == 0ull) {
                align = raw_desc.Alignment;
            }
            const uint64_t w = std::max<uint64_t>(1ull, raw_desc.Width);
            const uint64_t sz = (w + align - 1ull) & ~(align - 1ull);
            return D3D12_RESOURCE_ALLOCATION_INFO{sz, align};
        }

        const uint32_t fmt = static_cast<uint32_t>(raw_desc.Format);
        const uint32_t samples = std::max<uint32_t>(1u, raw_desc.SampleDesc.Count);

        // Planar depth+stencil formats on Xbox Series X|S RDNA2:
        // - D32_FLOAT_S8X24_UINT / R32G8X24_TYPELESS (19..22): 4-Bpp Z plane + 1-Bpp S8 plane (no X24 physical bits!)
        // - D16_UNORM_S8_UINT / R16G8_TYPELESS (118..121): 2-Bpp Z plane + 1-Bpp S8 plane
        if (fmt >= 19u && fmt <= 22u) {
            const uint64_t z_bytes = ComputeRdna2PlaneAllocBytes(
                raw_desc.Dimension, raw_desc.Width, raw_desc.Height,
                raw_desc.DepthOrArraySize, raw_desc.MipLevels, samples, 4u, 1u);
            const uint64_t s_bytes = ComputeRdna2PlaneAllocBytes(
                raw_desc.Dimension, raw_desc.Width, raw_desc.Height,
                raw_desc.DepthOrArraySize, raw_desc.MipLevels, samples, 1u, 1u);
            return D3D12_RESOURCE_ALLOCATION_INFO{z_bytes + s_bytes, 65536ull};
        }
        if (fmt >= 118u && fmt <= 121u) {
            const uint64_t z_bytes = ComputeRdna2PlaneAllocBytes(
                raw_desc.Dimension, raw_desc.Width, raw_desc.Height,
                raw_desc.DepthOrArraySize, raw_desc.MipLevels, samples, 2u, 1u);
            const uint64_t s_bytes = ComputeRdna2PlaneAllocBytes(
                raw_desc.Dimension, raw_desc.Width, raw_desc.Height,
                raw_desc.DepthOrArraySize, raw_desc.MipLevels, samples, 1u, 1u);
            return D3D12_RESOURCE_ALLOCATION_INFO{z_bytes + s_bytes, 65536ull};
        }

        uint32_t bpb = 4u;
        uint32_t block_dim = 1u;
        if ((fmt >= 70u && fmt <= 72u) || (fmt >= 79u && fmt <= 81u)) {
            bpb = 8u;       // BC1 / BC4 (8 bytes per 4x4 block)
            block_dim = 4u;
        } else if ((fmt >= 73u && fmt <= 78u) || (fmt >= 82u && fmt <= 84u) || (fmt >= 94u && fmt <= 99u)) {
            bpb = 16u;      // BC2 / BC3 / BC5 / BC6H / BC7 (16 bytes per 4x4 block)
            block_dim = 4u;
        } else if (fmt >= 1u && fmt <= 4u) {
            bpb = 16u;      // 128-bit RGBA32
        } else if (fmt >= 5u && fmt <= 8u) {
            bpb = 12u;      // 96-bit RGB32
        } else if (fmt >= 9u && fmt <= 18u) {
            bpb = 8u;       // 64-bit RGBA16 / RG32
        } else if ((fmt >= 23u && fmt <= 47u) || (fmt >= 87u && fmt <= 93u) || fmt == 116u || fmt == 117u) {
            bpb = 4u;       // 32-bit RGBA8 / BGRA8 / R32 / D32 / D24S8 / R10G10B10A2 / R11G11B10 / 7e3 / 6e4
        } else if ((fmt >= 48u && fmt <= 59u) || fmt == 85u || fmt == 86u || fmt == 115u) {
            bpb = 2u;       // 16-bit RG8 / R16 / D16 / B5G6R5 / B5G5R5A1 / B4G4R4A4
        } else if (fmt >= 60u && fmt <= 65u) {
            bpb = 1u;       // 8-bit R8 / A8
        }

        const uint64_t sz = ComputeRdna2PlaneAllocBytes(
            raw_desc.Dimension, raw_desc.Width, raw_desc.Height,
            raw_desc.DepthOrArraySize, raw_desc.MipLevels, samples, bpb, block_dim);
        return D3D12_RESOURCE_ALLOCATION_INFO{sz, 65536ull};
    }

    static D3D12_RESOURCE_ALLOCATION_INFO ComputeRetailBatchAllocInfo(
        openscarlett::d3d12_xs::DeviceX* inner,
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pInfo1) noexcept {
        if (!inner || !pResourceDescs || numResourceDescs == 0u) {
            return D3D12_RESOURCE_ALLOCATION_INFO{65536ull, 65536ull};
        }
        uint64_t total_offset = 0ull;
        uint64_t max_align = 256ull;
        for (UINT i = 0u; i < numResourceDescs; ++i) {
            const D3D12_RESOURCE_ALLOCATION_INFO one =
                ComputeRetailSingleAllocInfo(inner, visibleMask, pResourceDescs[i]);
            const uint64_t a = std::clamp<uint64_t>(one.Alignment ? one.Alignment : 65536ull, 16ull, 65536ull);
            max_align = std::max<uint64_t>(max_align, a);
            total_offset = (total_offset + a - 1ull) & ~(a - 1ull);
            if (pInfo1) {
                pInfo1[i].Offset = total_offset;
                pInfo1[i].Alignment = a;
                pInfo1[i].SizeInBytes = one.SizeInBytes;
            }
            total_offset += one.SizeInBytes;
        }
        total_offset = (total_offset + max_align - 1ull) & ~(max_align - 1ull);
        return D3D12_RESOURCE_ALLOCATION_INFO{total_offset, max_align};
    }

    static D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Thunk_GetResourceAllocationInfo2(
        void* arg0,
        void* arg1,
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC1* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        ScarlettRetailDeviceProxy* self = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO* out_ret = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<const void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
            self = static_cast<ScarlettRetailDeviceProxy*>(arg0);
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg1);
        } else {
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg0);
            self = static_cast<ScarlettRetailDeviceProxy*>(arg1);
        }
        D3D12_RESOURCE_ALLOCATION_INFO res{};
        if (self && self->inner_ && pResourceDescs && numResourceDescs > 0u) {
            std::vector<D3D12_RESOURCE_DESC> raw_descs(numResourceDescs);
            for (UINT i = 0u; i < numResourceDescs; ++i) {
                D3D12_RESOURCE_DESC d0{};
                d0.Dimension = pResourceDescs[i].Dimension;
                d0.Alignment = pResourceDescs[i].Alignment;
                d0.Width = pResourceDescs[i].Width;
                d0.Height = pResourceDescs[i].Height;
                d0.DepthOrArraySize = pResourceDescs[i].DepthOrArraySize;
                d0.MipLevels = pResourceDescs[i].MipLevels;
                d0.Format = pResourceDescs[i].Format;
                d0.SampleDesc = pResourceDescs[i].SampleDesc;
                d0.Layout = pResourceDescs[i].Layout;
                d0.Flags = pResourceDescs[i].Flags;
                raw_descs[i] = d0;
            }
            res = ComputeRetailBatchAllocInfo(
                self->inner_, visibleMask, numResourceDescs, raw_descs.data(), pResourceAllocationInfo1);
        }
        if (out_ret) {
            *out_ret = res;
        }
        return out_ret;
    }

    static D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Thunk_GetResourceAllocationInfo1(
        void* arg0,
        void* arg1,
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        ScarlettRetailDeviceProxy* self = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO* out_ret = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<const void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
            self = static_cast<ScarlettRetailDeviceProxy*>(arg0);
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg1);
        } else {
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg0);
            self = static_cast<ScarlettRetailDeviceProxy*>(arg1);
        }
        D3D12_RESOURCE_ALLOCATION_INFO res{};
        if (self && self->inner_ && pResourceDescs && numResourceDescs > 0u) {
            res = ComputeRetailBatchAllocInfo(
                self->inner_, visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
        }
        if (out_ret) {
            *out_ret = res;
        }
        return out_ret;
    }
    static uint64_t STDMETHODCALLTYPE Thunk_GetAdapterLuid(ScarlettRetailDeviceProxy* self) {
        if (!self || !self->inner_) return 0ull;
        const LUID luid = self->inner_->GetAdapterLuid();
        uint64_t raw = 0ull;
        static_assert(sizeof(LUID) == sizeof(uint64_t), "LUID must be 8 bytes");
        std::memcpy(&raw, &luid, sizeof(raw));
        return raw;
    }

    static bool IsProxyPtr(void* p, const void* const* expected_vtbl) noexcept {
        if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000u) return false;
        return *reinterpret_cast<const void* const* const*>(p) == expected_vtbl;
    }

    static D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Thunk_GetResourceAllocationInfo(
        void* arg0,
        void* arg1,
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) {
        // Auto-detect CINTERFACE (RCX=sret, RDX=this) vs C++ COM (RCX=this, RDX=sret).
        ScarlettRetailDeviceProxy* self = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO* out_ret = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<const void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
            self = static_cast<ScarlettRetailDeviceProxy*>(arg0);
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg1);
        } else {
            out_ret = static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(arg0);
            self = static_cast<ScarlettRetailDeviceProxy*>(arg1);
        }
        D3D12_RESOURCE_ALLOCATION_INFO res{};
        if (self && self->inner_ && pResourceDescs && numResourceDescs > 0u) {
            res = ComputeRetailBatchAllocInfo(
                self->inner_, visibleMask, numResourceDescs, pResourceDescs, nullptr);
            static const bool trace_alloc = []() {
                char v[16]{};
                const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
                return n > 0u && v[0] != '0';
            }();
            if (trace_alloc) {
                std::fprintf(stderr,
                    "[OpenScarlett][d3d12_xs] GetResourceAllocationInfo(n=%u dim=%u %llux%u fmt=%u flags=0x%X) -> size=0x%llX align=0x%llX\n",
                    numResourceDescs,
                    static_cast<unsigned>(pResourceDescs[0].Dimension),
                    static_cast<unsigned long long>(pResourceDescs[0].Width),
                    pResourceDescs[0].Height,
                    static_cast<unsigned>(pResourceDescs[0].Format),
                    static_cast<unsigned>(pResourceDescs[0].Flags),
                    static_cast<unsigned long long>(res.SizeInBytes),
                    static_cast<unsigned long long>(res.Alignment));
                std::fflush(stderr);
            }
        }
        if (out_ret) {
            *out_ret = res;
        }
        return out_ret;
    }

    static D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE Thunk_GetCustomHeapProperties(
        void* arg0,
        void* arg1,
        UINT nodeMask,
        D3D12_HEAP_TYPE heapType) {
        ScarlettRetailDeviceProxy* self = nullptr;
        D3D12_HEAP_PROPERTIES* out_ret = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<const void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
            self = static_cast<ScarlettRetailDeviceProxy*>(arg0);
            out_ret = static_cast<D3D12_HEAP_PROPERTIES*>(arg1);
        } else {
            out_ret = static_cast<D3D12_HEAP_PROPERTIES*>(arg0);
            self = static_cast<ScarlettRetailDeviceProxy*>(arg1);
        }
        D3D12_HEAP_PROPERTIES res{};
        if (self && self->inner_) {
            res = self->inner_->GetCustomHeapProperties(nodeMask, heapType);
        }
        if (out_ret) {
            *out_ret = res;
        }
        return out_ret;
    }

    explicit ScarlettRetailDeviceProxy(
        openscarlett::d3d12_xs::DeviceX* inner,
        uint32_t retail_version = 0u)
        : inner_(inner), retail_version_(retail_version) {
        if (inner_) {
            inner_->AddRef();
        }
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        void* const* patched = GetPatchedVtbl(raw_vtbl);
        s_patched_vtbl_ptr_ = patched;
        *reinterpret_cast<void* const**>(this) = patched;
    }

    ~ScarlettRetailDeviceProxy() {
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }
    }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        // Xbox Series X ID3D12Device8..11 IIDs (verified live against UE5 Matrix Awakens & GRDK 2504/2510 Scarlett titles).
        static constexpr GUID kXboxIidD3D12Device8 = {
            0x728F1FA2u, 0xEA11u, 0x4A9Fu, {0x9Bu, 0x52u, 0x9Bu, 0x42u, 0xA4u, 0x84u, 0xD5u, 0xC9u}};
        static constexpr GUID kXboxIidD3D12Device11 = {
            0x5405C344u, 0xD457u, 0x444Eu, {0xB4u, 0xDDu, 0x23u, 0x66u, 0xE4u, 0x5Au, 0xEEu, 0x39u}};
        static constexpr GUID kPcIidD3D12Device8 = {
            0x9218E6BBu, 0xF944u, 0x4F7Eu, {0xA7u, 0x5Cu, 0xB1u, 0xB2u, 0xC7u, 0xB7u, 0x01u, 0xF3u}};
        static constexpr GUID kPcIidD3D12Device9 = {
            0x4C80E962u, 0xF032u, 0x4F60u, {0xBCu, 0x9Eu, 0xEBu, 0xC2u, 0xCFu, 0xA1u, 0xD8u, 0x3Cu}};
        static constexpr GUID kPcIidD3D12Device10 = {
            0x517F8718u, 0xAA66u, 0x49F9u, {0xB0u, 0x2Bu, 0xA7u, 0xABu, 0x89u, 0xC0u, 0x60u, 0xFFu}};
        static constexpr GUID kPcIidD3D12Device11 = {
            0x5405C344u, 0xD457u, 0x444Eu, {0xB4u, 0xDDu, 0x23u, 0x66u, 0xE4u, 0x5Au, 0xEEu, 0x39u}};
        static constexpr GUID kPcIidD3D12Device12 = {
            0x5AF5C532u, 0x4C91u, 0x4CD0u, {0xB5u, 0x41u, 0x15u, 0xA4u, 0x05u, 0x7Au, 0x59u, 0x5Cu}};
        static constexpr GUID kPcIidD3D12Device13 = {
            0x14EECFFCu, 0x4DF8u, 0x40F7u, {0xA1u, 0x18u, 0x5Cu, 0x81u, 0x6Fu, 0x45u, 0x69u, 0x5Eu}};
        static constexpr GUID kPcIidD3D12Device14 = {
            0x5F6CE764u, 0x1867u, 0x4D1Cu, {0xA6u, 0x3Eu, 0xF6u, 0x1Bu, 0x55u, 0x9Eu, 0x2Cu, 0x6Bu}};
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ID3D12Object)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device1)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device2)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device3)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device4)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device5)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device6)) ||
            IsEqualIID(riid, __uuidof(ID3D12Device7)) ||
            IsEqualIID(riid, kXboxIidD3D12Device8) ||
            IsEqualIID(riid, kXboxIidD3D12Device11) ||
            IsEqualIID(riid, kPcIidD3D12Device8) ||
            IsEqualIID(riid, kPcIidD3D12Device9) ||
            IsEqualIID(riid, kPcIidD3D12Device10) ||
            IsEqualIID(riid, kPcIidD3D12Device11) ||
            IsEqualIID(riid, kPcIidD3D12Device12) ||
            IsEqualIID(riid, kPcIidD3D12Device13) ||
            IsEqualIID(riid, kPcIidD3D12Device14)) {
            *ppvObject = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        HRESULT hr = inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
        static const bool trace_retail_qi = []() {
            char v[16]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
            return n > 0u && v[0] != '0';
        }();
        if (FAILED(hr) && trace_retail_qi) {
            std::fprintf(stderr,
                "[OpenScarlett][d3d12_xs] ScarlettRetailDeviceProxy::QueryInterface MISS "
                "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} -> 0x%08lX\n",
                static_cast<unsigned long>(riid.Data1), riid.Data2, riid.Data3,
                riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7],
                static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) {
            delete this;
        }
        return n;
    }

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override {
        return inner_->GetPrivateData(guid, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override {
        return inner_->SetPrivateData(guid, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override {
        return inner_->SetPrivateDataInterface(guid, pData);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override {
        return inner_->SetName(Name);
    }

    // ID3D12Device (slots 7..43)
    UINT STDMETHODCALLTYPE GetNodeCount() override { return inner_->GetNodeCount(); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override {
        if (!ppCommandQueue) return E_POINTER;
        *ppCommandQueue = nullptr;
        ID3D12CommandQueueX* qx = nullptr;
        const HRESULT hr = inner_->CreateCommandQueue(
            pDesc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx));
        if (FAILED(hr) || !qx) return hr;
        if (IsEqualIID(riid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(riid, __uuidof(IUnknown))) {
            auto* retail_q = new (std::nothrow) ScarlettRetailCommandQueueProxy(qx, retail_version_);
            qx->Release();
            if (!retail_q) return E_OUTOFMEMORY;
            *ppCommandQueue = static_cast<ID3D12CommandQueue*>(retail_q);
            return S_OK;
        }
        const HRESULT qhr = qx->QueryInterface(riid, ppCommandQueue);
        qx->Release();
        return qhr;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override {
        return inner_->CreateCommandAllocator(type, riid, ppCommandAllocator);
    }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        return inner_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override {
        return inner_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
    }
    HRESULT WrapRetailCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;
        ID3D12GraphicsCommandList* raw_cl = nullptr;
        const HRESULT hr = inner_->CreateCommandList(
            nodeMask, type, pCommandAllocator, pInitialState,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&raw_cl));
        if (FAILED(hr) || !raw_cl) return hr;
        auto* clx = openscarlett::d3d12_xs::CommandListX::TryFromCommandList(raw_cl);
        if (clx) {
            auto* retail_cl = new (std::nothrow) ScarlettRetailCommandListProxy(clx);
            raw_cl->Release();
            if (!retail_cl) return E_OUTOFMEMORY;
            const HRESULT qhr = retail_cl->QueryInterface(riid, ppCommandList);
            retail_cl->Release();
            return qhr;
        }
        const HRESULT qhr = raw_cl->QueryInterface(riid, ppCommandList);
        raw_cl->Release();
        return qhr;
    }

    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override {
        return WrapRetailCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override {
        return inner_->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override {
        uint64_t guest_cpu_base = 0ull;
        uint64_t guest_byte_size = 0ull;
        if (ppvHeap && TryReadRetailDescriptorHeapExt(pDescriptorHeapDesc, guest_cpu_base, guest_byte_size)) {
            *ppvHeap = nullptr;
            ID3D12DescriptorHeap* inner_heap = nullptr;
            const HRESULT hr = inner_->CreateDescriptorHeap(
                pDescriptorHeapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&inner_heap));
            if (FAILED(hr) || !inner_heap) {
                return hr;
            }
            auto* retail_heap = new (std::nothrow) ScarlettRetailDescriptorHeapProxy(
                inner_heap, guest_cpu_base, guest_byte_size);
            if (retail_heap) {
                if (auto* dh = openscarlett::d3d12_xs::TryGetDescriptorHeapProxy(inner_heap)) {
                    if (ID3D12DescriptorHeap* hh = dh->HostDescriptorHeap()) {
                        const auto hd = hh->GetDesc();
                        if ((hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                             hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) &&
                            (hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
                            const UINT inc = inner_->GetDescriptorHandleIncrementSize(hd.Type);
                            const uint64_t hcpu = hh->GetCPUDescriptorHandleForHeapStart().ptr;
                            const uint64_t hgpu = hh->GetGPUDescriptorHandleForHeapStart().ptr;
                            inner_->RegisterGuestBackedDescHeap(
                                hh, hgpu, hcpu, hd.NumDescriptors, inc, guest_cpu_base, guest_byte_size, hd.Type);
                        }
                    }
                }
            }
            inner_heap->Release();
            if (!retail_heap) {
                return E_OUTOFMEMORY;
            }
            const HRESULT qhr = retail_heap->QueryInterface(riid, ppvHeap);
            retail_heap->Release();
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", "ScarlettRetailDevice::CreateDescriptorHeap[GuestBase]",
                SUCCEEDED(qhr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, qhr, 14);
            return qhr;
        }
        return inner_->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override {
        return inner_->GetDescriptorHandleIncrementSize(DescriptorHeapType);
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
        REFIID riid, void** ppvRootSignature) override {
        return inner_->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateConstantBufferView(pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateShaderResourceView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateRenderTargetView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateDepthStencilView(pResource, pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override {
        inner_->CreateSampler(pDesc, DestDescriptor);
    }
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        inner_->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                                NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                                DescriptorHeapsType);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override {
        inner_->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                                      DescriptorHeapsType);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet, UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override {
        return inner_->GetResourceAllocationInfo(pRet, visibleMask, numResourceDescs, pResourceDescs);
    }
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
        D3D12_HEAP_PROPERTIES* pRet, UINT nodeMask, D3D12_HEAP_TYPE heapType) override {
        return inner_->GetCustomHeapProperties(pRet, nodeMask, heapType);
    }
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override {
        return inner_->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override {
        return inner_->GetCustomHeapProperties(nodeMask, heapType);
    }
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override {
        return inner_->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                               pOptimizedClearValue, riidResource, ppvResource);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        return inner_->CreateHeap(pDesc, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override {
        return inner_->CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid,
                                            ppvResource);
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override {
        return inner_->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes, DWORD Access,
        LPCWSTR Name, HANDLE* pHandle) override {
        return inner_->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) override {
        return inner_->OpenSharedHandle(NTHandle, riid, ppvObj);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override {
        return inner_->OpenSharedHandleByName(Name, Access, pNTHandle);
    }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        return inner_->MakeResident(NumObjects, ppObjects);
    }
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) override {
        return inner_->Evict(NumObjects, ppObjects);
    }
    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override {
        return inner_->CreateFence(InitialValue, Flags, riid, ppFence);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return inner_->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override {
        inner_->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset,
                                      pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override {
        return inner_->CreateQueryHeap(pDesc, riid, ppvHeap);
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override {
        return inner_->SetStablePowerState(Enable);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override {
        return inner_->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature);
    }
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override {
        inner_->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc,
                                  pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
                                  FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips);
    }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* pRet) override { return inner_->GetAdapterLuid(pRet); }
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return inner_->GetAdapterLuid(); }
#endif

    // Retail Scarlett d3d12_xs.h ID3D12Device extended vtable (slots 44..191)
    HRESULT RecordUnimpl(int slot, uintptr_t a1 = 0u, uintptr_t a2 = 0u, uintptr_t a3 = 0u, const void* ret = nullptr) {
        static std::atomic<uint32_t> s_slot_log_counts[256]{};
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailDevice::Slot_%d", slot);
        const uint32_t idx = (slot >= 0 && slot < 256) ? static_cast<uint32_t>(slot) : 255u;
        const uint32_t seen = s_slot_log_counts[idx].fetch_add(1u, std::memory_order_relaxed);
        if (seen < 3u) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
            const uintptr_t rva = (ret && reinterpret_cast<uintptr_t>(ret) > base)
                                      ? (reinterpret_cast<uintptr_t>(ret) - base)
                                      : reinterpret_cast<uintptr_t>(ret);
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X) ret=+0x%llX a1=0x%llX a2=0x%llX a3=0x%llX\n",
                         buf, slot * 8,
                         static_cast<unsigned long long>(rva),
                         static_cast<unsigned long long>(a1),
                         static_cast<unsigned long long>(a2),
                         static_cast<unsigned long long>(a3));
            std::fflush(stderr);
            OutputDebugStringA(buf);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", buf, openscarlett::CallOutcome::NotImplemented, E_NOTIMPL, slot);
        return E_NOTIMPL;
    }

    // Slot 44 (+0x160): CreatePlacedResourceX(VirtualAddress, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource)
    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) {
        const HRESULT hr = inner_->CreatePlacedResourceX(
            VirtualAddress, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreatePlacedResourceX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 44);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot45() { return RecordUnimpl(45); }
    virtual HRESULT STDMETHODCALLTYPE Slot46() { return RecordUnimpl(46); }
    virtual HRESULT STDMETHODCALLTYPE Slot47() { return RecordUnimpl(47); }
    // Slot 48 (+0x180): CreateCommandQueueX(pDesc, riid, ppCommandQueue)
    // D3D12XBOX_COMMAND_QUEUE_DESC begins with D3D12_COMMAND_LIST_TYPE Type (uint32_t),
    // followed by Xbox flags/priority/pipe/queue index fields.
    virtual HRESULT STDMETHODCALLTYPE CreateCommandQueueX(
        const void* pDesc, REFIID riid, void** ppCommandQueue) {
        D3D12_COMMAND_QUEUE_DESC std_desc{};
        std_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        std_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        std_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        std_desc.NodeMask = 0u;
        if (pDesc) {
            const auto* words = static_cast<const uint32_t*>(pDesc);
            const uint32_t raw_type = words[0];
            if (raw_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                raw_type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
                raw_type == D3D12_COMMAND_LIST_TYPE_COPY) {
                std_desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(raw_type);
            }
        }
        const HRESULT hr = CreateCommandQueue(&std_desc, riid, ppCommandQueue);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateCommandQueueX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 48);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot49() { return RecordUnimpl(49); }
    virtual HRESULT STDMETHODCALLTYPE Slot50() { return RecordUnimpl(50); }
    virtual HRESULT STDMETHODCALLTYPE Slot51() { return RecordUnimpl(51); }
    virtual HRESULT STDMETHODCALLTYPE Slot52() { return RecordUnimpl(52); }
    virtual HRESULT STDMETHODCALLTYPE Slot53() { return RecordUnimpl(53); }
    virtual HRESULT STDMETHODCALLTYPE Slot54() { return RecordUnimpl(54); }
    // Slot 55 (+0x1B8): CreateDerivedGraphicsPipelineStateX(pSourcePso, NumOverrides, pOverrides, riid, ppv)
    virtual HRESULT STDMETHODCALLTYPE Slot55(
        ID3D12PipelineState* pSourcePso,
        UINT NumOverrides,
        const void* pOverrides,
        REFIID riid,
        void** ppPipelineState) {
        const HRESULT hr = inner_->CreateDerivedGraphicsPipelineStateX(
            pSourcePso, NumOverrides, pOverrides, riid, ppPipelineState);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateDerivedGraphicsPipelineStateX_Slot55",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 55);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot56() { return RecordUnimpl(56); }
    // Slot 57 (+0x1C8): DeserializeGraphicsPipelineStateX(pDesc, riid, ppPipelineState)
    virtual HRESULT STDMETHODCALLTYPE Slot57(
        const void* pDesc,
        REFIID riid,
        void** ppPipelineState) {
        if (!pDesc || !ppPipelineState) {
            return E_POINTER;
        }
        const auto* q = static_cast<const uint64_t*>(pDesc);
        static std::atomic<uint32_t> s_57_dump{0u};
        const uint32_t idx = s_57_dump.fetch_add(1u, std::memory_order_relaxed);
        if (idx < 3u) {
            char dump_dir[260]{};
            if (::GetEnvironmentVariableA("OPENSCARLETT_DUMP_BOUND_PSO", dump_dir, sizeof(dump_dir)) > 0) {
                ::CreateDirectoryA(dump_dir, nullptr);
                auto dump_slice = [&](const char* tag, uint64_t addr, uint64_t sz) {
                    if (addr < 0x10000ull || sz == 0ull || sz > 0x100000ull) return;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) == 0 ||
                        mbi.State != MEM_COMMIT) {
                        return;
                    }
                    const uintptr_t reg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    if (static_cast<uintptr_t>(addr) + static_cast<size_t>(sz) > reg_end) {
                        sz = static_cast<uint64_t>(reg_end - static_cast<uintptr_t>(addr));
                    }
                    if (sz == 0ull) return;
                    char pbuf[320]{};
                    std::snprintf(pbuf, sizeof(pbuf), "%s\\slot57_%02u_%s.bin", dump_dir, idx, tag);
                    FILE* f = nullptr;
                    if (::fopen_s(&f, pbuf, "wb") == 0 && f) {
                        std::fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), 1u, static_cast<size_t>(sz), f);
                        std::fclose(f);
                    }
                };
                dump_slice("desc", reinterpret_cast<uintptr_t>(pDesc), 104ull);
                dump_slice("b1", q[1], q[2]);
                dump_slice("b2", q[3], q[4]);
                dump_slice("isa", q[5], 8192ull);
                dump_slice("mps", q[7], q[8]);
                dump_slice("mvs", q[11], q[12]);
            }
        }
        const HRESULT hr = inner_->DeserializeGraphicsPipelineStateX(pDesc, riid, ppPipelineState);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::DeserializeGraphicsPipelineStateX_Slot57",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 57);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot58() { return RecordUnimpl(58); }
    virtual HRESULT STDMETHODCALLTYPE Slot59() { return RecordUnimpl(59); }
    // Slot 60 (+0x1E0): CreateGraphicsPipelineStateX(pDesc, pCache, pFlags, riid, ppv)
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineStateX(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
        void* /*pCache*/,
        void* /*pFlags*/,
        REFIID riid,
        void** ppPipelineState) {
        const HRESULT hr = inner_->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateGraphicsPipelineStateX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 60);
        return hr;
    }
    // Slot 61 (+0x1E8): CreateSamplerX / CreateSampler2
    virtual HRESULT STDMETHODCALLTYPE CreateSamplerX(
        const D3D12_SAMPLER_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
        (void)pDesc;
        (void)DestDescriptor;
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateSamplerX",
            openscarlett::CallOutcome::Ok, S_OK, 61);
        return S_OK;
    }
    // Slot 62 (+0x1F0): CreateComputePipelineStateX(pDesc, pCache, pFlags, riid, ppv)
    virtual HRESULT STDMETHODCALLTYPE CreateComputePipelineStateX(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
        void* /*pCache*/,
        void* /*pFlags*/,
        REFIID riid,
        void** ppPipelineState) {
        const HRESULT hr = inner_->CreateComputePipelineState(pDesc, riid, ppPipelineState);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateComputePipelineStateX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 62);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot63() { return RecordUnimpl(63); }
    virtual HRESULT STDMETHODCALLTYPE Slot64() { return RecordUnimpl(64); }
    virtual HRESULT STDMETHODCALLTYPE Slot65() { return RecordUnimpl(65); }
    virtual HRESULT STDMETHODCALLTYPE Slot66() { return RecordUnimpl(66); }
    // Slot 67 (+0x218): DeserializeComputePipelineStateX(pDesc, riid, ppPipelineState)
    virtual HRESULT STDMETHODCALLTYPE Slot67(
        const void* pDesc,
        REFIID riid,
        void** ppPipelineState) {
        if (!pDesc || !ppPipelineState) {
            return E_POINTER;
        }
        const auto* q = static_cast<const uint64_t*>(pDesc);
        static std::atomic<uint32_t> s_67_dump{0u};
        const uint32_t idx = s_67_dump.fetch_add(1u, std::memory_order_relaxed);
        if (idx < 3u) {
            char dump_dir[260]{};
            if (::GetEnvironmentVariableA("OPENSCARLETT_DUMP_BOUND_PSO", dump_dir, sizeof(dump_dir)) > 0) {
                ::CreateDirectoryA(dump_dir, nullptr);
                auto dump_slice = [&](const char* tag, uint64_t addr, uint64_t sz) {
                    if (addr < 0x10000ull || sz == 0ull || sz > 0x100000ull) return;
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) == 0 ||
                        mbi.State != MEM_COMMIT) {
                        return;
                    }
                    const uintptr_t reg_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                    if (static_cast<uintptr_t>(addr) + static_cast<size_t>(sz) > reg_end) {
                        sz = static_cast<uint64_t>(reg_end - static_cast<uintptr_t>(addr));
                    }
                    if (sz == 0ull) return;
                    char pbuf[320]{};
                    std::snprintf(pbuf, sizeof(pbuf), "%s\\slot67_%02u_%s.bin", dump_dir, idx, tag);
                    FILE* f = nullptr;
                    if (::fopen_s(&f, pbuf, "wb") == 0 && f) {
                        std::fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), 1u, static_cast<size_t>(sz), f);
                        std::fclose(f);
                    }
                };
                dump_slice("desc", reinterpret_cast<uintptr_t>(pDesc), 72ull);
                dump_slice("b1", q[1], q[2]);
                dump_slice("b2", q[3], q[4]);
                dump_slice("isa", q[5], 8192ull);
                dump_slice("mcs", q[7], q[8]);
            }
        }
        const HRESULT hr = inner_->DeserializeComputePipelineStateX(pDesc, riid, ppPipelineState);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::DeserializeComputePipelineStateX_Slot67",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 67);
        return hr;
    }
    // Slot 68 (+0x220): GetGpuHardwareConfigurationX
    virtual HRESULT STDMETHODCALLTYPE GetGpuHardwareConfigurationX(void* pOutConfig) {
        if (pOutConfig) {
            std::memset(pOutConfig, 0, 16u);
            auto* words = static_cast<uint32_t*>(pOutConfig);
            words[0] = 0x10030000u;  // GFX10.3 / Scarlett
            bool is_lockhart = false;
            char cbuf[32]{};
            DWORD clen = ::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_TYPE", cbuf, sizeof(cbuf));
            if (clen == 0u || clen >= sizeof(cbuf)) {
                clen = ::GetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", cbuf, sizeof(cbuf));
            }
            if (clen > 0u && clen < sizeof(cbuf)) {
                std::string s(cbuf, clen);
                for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                is_lockhart = (s == "lockhart" || s == "series_s" || s == "3" || s == "s" || s == "8gb");
            }
            words[1] = is_lockhart ? 20u : 52u;  // Lockhart (20 CU) vs Anaconda (52 CU)
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::GetGpuHardwareConfigurationX",
            openscarlett::CallOutcome::Ok, S_OK, 68);
        return S_OK;
    }
    // Slot 69 (+0x228): SetDriverFlagX(uint32_t key, uint32_t value)
    virtual HRESULT STDMETHODCALLTYPE SetDriverFlagX(uint32_t /*key*/, uint32_t /*value*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::SetDriverFlagX",
            openscarlett::CallOutcome::Ok, S_OK, 69);
        return S_OK;
    }
    // Slot 70 (+0x230): CreateShaderResourceViewFromRawDescriptorsX
    // Retail Scarlett ABI: (const D3D12_RESOURCE_DESC* pResDesc, const void* pViewDesc (desktop SRV desc + u64 gpuAddr @+0x28), CPU handle)
    virtual HRESULT STDMETHODCALLTYPE CreateShaderResourceViewFromRawDescriptorsX(
        const void* pTextureWords,
        const void* pViewDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
        static const bool trace_dd = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DRAW_DATA", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        static std::atomic<uint32_t> s_s70_log{0u};
        if (trace_dd && s_s70_log.fetch_add(1u, std::memory_order_relaxed) < 25u) {
            const auto* rd = static_cast<const D3D12_RESOURCE_DESC*>(pTextureWords);
            const auto* vd = static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(pViewDesc);
            uint64_t gpu_va = 0ull;
            if (pViewDesc) {
                std::memcpy(&gpu_va, static_cast<const uint8_t*>(pViewDesc) + 0x28u, sizeof(gpu_va));
            }
            std::fprintf(stderr,
                         "[OpenScarlett][SLOT70] cpu=0x%llX res=(dim=%d %llux%u fmt=%d) view=(fmt=%d vdim=%d) va=0x%llX\n",
                         static_cast<unsigned long long>(destDescriptor.ptr),
                         rd ? static_cast<int>(rd->Dimension) : -1,
                         rd ? static_cast<unsigned long long>(rd->Width) : 0ull,
                         rd ? rd->Height : 0u,
                         rd ? static_cast<int>(rd->Format) : -1,
                         vd ? static_cast<int>(vd->Format) : -1,
                         vd ? static_cast<int>(vd->ViewDimension) : -1,
                         static_cast<unsigned long long>(gpu_va));
            std::fflush(stderr);
        }
        if (inner_) {
            inner_->CreatePlacedShaderResourceViewX(
                static_cast<const D3D12_RESOURCE_DESC*>(pTextureWords),
                pViewDesc,
                destDescriptor);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateShaderResourceViewFromRawDescriptorsX",
            openscarlett::CallOutcome::Ok, S_OK, 70);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot71() { return RecordUnimpl(71); }
    virtual HRESULT STDMETHODCALLTYPE Slot72() { return RecordUnimpl(72); }
    // Slot 73 (+0x248): CreateCommandListX(pDesc, pCommandAllocator, pInitialState, riid, ppCommandList)
    virtual HRESULT STDMETHODCALLTYPE CreateCommandListX(
        const void* pDesc,
        ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState,
        REFIID riid,
        void** ppCommandList) {
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // D3D12XBOX_COMMAND_LIST_DESC puts Type at +0 and Xbox-only Flags at +4 (not PC NodeMask);
        // passing Xbox flags as a PC node_mask causes desktop D3D12 CreateCommandList -> E_INVALIDARG.
        constexpr UINT node_mask = 0u;
        if (pDesc) {
            const auto* words = static_cast<const uint32_t*>(pDesc);
            type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
        }
        const HRESULT hr = WrapRetailCommandList(
            node_mask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateCommandListX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 73);
        return hr;
    }
    // Slot 74 (+0x250): CreateTilePoolX / RegisterPagePoolX(pBaseAddress, numPages, pOutHandle)
    virtual HRESULT STDMETHODCALLTYPE CreateTilePoolX_Slot74(
        void* pBaseAddress,
        UINT numPages,
        uint64_t* pOutHandle) {
        if (pOutHandle) {
            *pOutHandle = reinterpret_cast<uint64_t>(pBaseAddress);
        }
        // When a retail Scarlett title allocates a full-budget physical tile pool (e.g. 99,776 pages =
        // 6,236 MiB on Anaconda) and decrements its soft eviction counter before async DirectStorage /
        // GPU-fenced unmap batches drain, reserve 10,240 physical pages (640 MiB) of transient unmap
        // slack in the soft eviction watermark so the physical free-list never hits 0x1FFFFF.
        if (pOutHandle && numPages >= 65536u) {
            const HMODULE main_exe = ::GetModuleHandleW(nullptr);
            if (main_exe) {
                const uintptr_t base = reinterpret_cast<uintptr_t>(main_exe);
                if (reinterpret_cast<uintptr_t>(pOutHandle) == base + 0x71BFCC8u &&
                    IsReadableRetailRange(base + 0x9C3E060u, sizeof(uint64_t)) &&
                    IsReadableRetailRange(base + 0x98D26D8u, sizeof(uint64_t))) {
                    auto* p_budget_cap = reinterpret_cast<uint64_t*>(base + 0x9C3E060u);
                    const auto* p_budget_used = reinterpret_cast<const uint64_t*>(base + 0x98D26D8u);
                    const uint64_t pool_bytes = static_cast<uint64_t>(numPages) << 16u;
                    if (*p_budget_cap == pool_bytes && *p_budget_used == 0ull) {
                        constexpr uint64_t kAsyncUnmapSlackPages = 10240ull;  // 640 MiB
                        const uint64_t safe_cap =
                            static_cast<uint64_t>(numPages - kAsyncUnmapSlackPages) << 16u;
                        *p_budget_cap = safe_cap;
                        std::fprintf(
                            stderr,
                            "[OpenScarlett][d3d12_xs] CreateTilePoolX_Slot74: pool=%p pages=%u (%.1f MiB), reserved %llu slack pages -> soft cap=%.1f MiB\n",
                            pBaseAddress,
                            numPages,
                            static_cast<double>(pool_bytes) / (1024.0 * 1024.0),
                            static_cast<unsigned long long>(kAsyncUnmapSlackPages),
                            static_cast<double>(safe_cap) / (1024.0 * 1024.0));
                        std::fflush(stderr);
                    }
                }
            }
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateTilePoolX_Slot74",
            openscarlett::CallOutcome::Ok, S_OK, 74);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot75() { return RecordUnimpl(75); }
    virtual HRESULT STDMETHODCALLTYPE Slot76() { return RecordUnimpl(76); }
    virtual HRESULT STDMETHODCALLTYPE Slot77() { return RecordUnimpl(77); }
    // Slot 78 (+0x270): SetFrameIntervalX
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput, uint32_t PeriodMicroseconds,
        uint32_t MaxFrameLatency, uint32_t Flags) {
        return inner_->SetFrameIntervalX(pOutput, PeriodMicroseconds, MaxFrameLatency, Flags);
    }
    // Slot 79 (+0x278): ScheduleFrameEventX
    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint64_t DelayMicroseconds,
        HANDLE hEvent, uint32_t Flags) {
        return inner_->ScheduleFrameEventX(Event, DelayMicroseconds, hEvent, Flags);
    }
    // Slot 80 (+0x280): WaitFrameEventX
    virtual HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint32_t TimeoutMilliseconds,
        void* pReserved, uint32_t Flags, D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) {
        return inner_->WaitFrameEventX(Event, TimeoutMilliseconds, pReserved, Flags, pOutToken);
    }
    // Slot 81 (+0x288): GetFrameStatisticsX
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatisticsX(
        D3D12XBOX_FRAME_PIPELINE_TOKEN /*Token*/,
        uint32_t MaxNumStatistics,
        uint32_t* pNumStatistics,
        void* pStatistics) {
        if (pNumStatistics) {
            *pNumStatistics = (pStatistics && MaxNumStatistics > 0u) ? 1u : 0u;
        }
        if (pStatistics && MaxNumStatistics > 0u) {
            std::memset(pStatistics, 0, 64u);
            LARGE_INTEGER qpc{};
            ::QueryPerformanceCounter(&qpc);
            auto* qwords = static_cast<uint64_t*>(pStatistics);
            qwords[3] = static_cast<uint64_t>(qpc.QuadPart);  // +0x18 OriginTimestamp
            qwords[4] = static_cast<uint64_t>(qpc.QuadPart);  // +0x20 PresentTimestamp
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::GetFrameStatisticsX",
            openscarlett::CallOutcome::Ok, S_OK, 81);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot82() { return RecordUnimpl(82); }
    // Retail Scarlett ID3D12Device1..7 (slots 83..106 = PC slots 44..67 shifted by +39)
    // Slot 83 (+0x298): ID3D12Device1::CreatePipelineLibrary
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
        const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) {
        return inner_->CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary);
    }
    // Slot 84 (+0x2A0): ID3D12Device1::SetEventOnMultipleFenceCompletion
    virtual HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
        ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) {
        return inner_->SetEventOnMultipleFenceCompletion(ppFences, pFenceValues, NumFences, Flags, hEvent);
    }
    // Slot 85 (+0x2A8): ID3D12Device1::SetResidencyPriority
    virtual HRESULT STDMETHODCALLTYPE SetResidencyPriority(
        UINT NumObjects, ID3D12Pageable* const* ppObjects, const D3D12_RESIDENCY_PRIORITY* pPriorities) {
        return inner_->SetResidencyPriority(NumObjects, ppObjects, pPriorities);
    }
    // Slot 86 (+0x2B0): ID3D12Device2::CreatePipelineState
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) {
        return inner_->CreatePipelineState(pDesc, riid, ppPipelineState);
    }
    // Slot 87 (+0x2B8): ID3D12Device3::OpenExistingHeapFromAddress
    virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
        const void* pAddress, REFIID riid, void** ppvHeap) {
        return inner_->OpenExistingHeapFromAddress(pAddress, riid, ppvHeap);
    }
    // Slot 88 (+0x2C0): ID3D12Device3::OpenExistingHeapFromFileMapping
    virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
        HANDLE hFileMapping, REFIID riid, void** ppvHeap) {
        return inner_->OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap);
    }
    // Slot 89 (+0x2C8): ID3D12Device3::EnqueueMakeResident
    virtual HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
        D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
        ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) {
        return inner_->EnqueueMakeResident(Flags, NumObjects, ppObjects, pFenceToSignal, FenceValueToSignal);
    }
    // Slot 90 (+0x2D0): ID3D12Device4::CreateCommandList1
    virtual HRESULT STDMETHODCALLTYPE CreateCommandList1(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** ppCommandList) {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;
        ID3D12GraphicsCommandList* raw_cl = nullptr;
        const HRESULT hr = inner_->CreateCommandList1(
            nodeMask, type, flags, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&raw_cl));
        if (FAILED(hr) || !raw_cl) return hr;
        auto* clx = openscarlett::d3d12_xs::CommandListX::TryFromCommandList(raw_cl);
        if (clx) {
            auto* retail_cl = new (std::nothrow) ScarlettRetailCommandListProxy(clx);
            raw_cl->Release();
            if (!retail_cl) return E_OUTOFMEMORY;
            const HRESULT qhr = retail_cl->QueryInterface(riid, ppCommandList);
            retail_cl->Release();
            return qhr;
        }
        const HRESULT qhr = raw_cl->QueryInterface(riid, ppCommandList);
        raw_cl->Release();
        return qhr;
    }
    // Slot 91 (+0x2D8): ID3D12Device4::CreateProtectedResourceSession
    virtual HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) {
        return inner_->CreateProtectedResourceSession(pDesc, riid, ppSession);
    }
    // Slot 92 (+0x2E0): ID3D12Device4::CreateCommittedResource1
    virtual HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) {
        return inner_->CreateCommittedResource1(
            pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue,
            pProtectedSession, riidResource, ppvResource);
    }
    // Slot 93 (+0x2E8): ID3D12Device4::CreateHeap1
    virtual HRESULT STDMETHODCALLTYPE CreateHeap1(
        const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvHeap) {
        return inner_->CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap);
    }
    // Slot 94 (+0x2F0): ID3D12Device4::CreateReservedResource1
    virtual HRESULT STDMETHODCALLTYPE CreateReservedResource1(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvResource) {
        return inner_->CreateReservedResource1(
            pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, ppvResource);
    }
    // Slot 95 (+0x2F8): ID3D12Device4::GetResourceAllocationInfo1 (patched via Thunk_GetResourceAllocationInfo1)
    virtual D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        return inner_->GetResourceAllocationInfo1(
            visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
    }
    // Slot 96 (+0x300): ID3D12Device5::CreateLifetimeTracker
    virtual HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
        ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) {
        return inner_->CreateLifetimeTracker(pOwner, riid, ppvTracker);
    }
    // Slot 97 (+0x308): ID3D12Device5::RemoveDevice
    virtual void STDMETHODCALLTYPE RemoveDevice() { inner_->RemoveDevice(); }
    // Slot 98 (+0x310): ID3D12Device5::EnumerateMetaCommands
    virtual HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
        UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) {
        return inner_->EnumerateMetaCommands(pNumMetaCommands, pDescs);
    }
    // Slot 99 (+0x318): ID3D12Device5::EnumerateMetaCommandParameters
    virtual HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
        UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
        D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) {
        return inner_->EnumerateMetaCommandParameters(
            CommandId, Stage, pTotalStructureSizeInBytes, pParameterCount, pParameterDescs);
    }
    // Slot 100 (+0x320): ID3D12Device5::CreateMetaCommand
    virtual HRESULT STDMETHODCALLTYPE CreateMetaCommand(
        REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
        SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) {
        return inner_->CreateMetaCommand(
            CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, ppMetaCommand);
    }
    // Slot 101 (+0x328): ID3D12Device5::CreateStateObject
    virtual HRESULT STDMETHODCALLTYPE CreateStateObject(
        const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) {
        return inner_->CreateStateObject(pDesc, riid, ppStateObject);
    }
    // Slot 102 (+0x330): ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo
    virtual void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) {
        inner_->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo);
    }
    // Slot 103 (+0x338): ID3D12Device5::CheckDriverMatchingIdentifier
    virtual D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) {
        return inner_->CheckDriverMatchingIdentifier(SerializedDataType, pIdentifierToCheck);
    }
    // Slot 104 (+0x340): ID3D12Device6::SetBackgroundProcessingMode
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(
        D3D12_BACKGROUND_PROCESSING_MODE Mode,
        D3D12_MEASUREMENTS_ACTION MeasurementsAction,
        HANDLE hEventToSignalUponCompletion,
        BOOL* pbFurtherMeasurementsDesired) {
        return inner_->SetBackgroundProcessingMode(
            Mode, MeasurementsAction, hEventToSignalUponCompletion, pbFurtherMeasurementsDesired);
    }
    // Slot 105 (+0x348): ID3D12Device7::AddToStateObject
    virtual HRESULT STDMETHODCALLTYPE AddToStateObject(
        const D3D12_STATE_OBJECT_DESC* pAddition, ID3D12StateObject* pStateObjectToGrowFrom,
        REFIID riid, void** ppNewStateObject) {
        return inner_->AddToStateObject(pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
    }
    // Slot 106 (+0x350): ID3D12Device7::CreateProtectedResourceSession1
    virtual HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc, REFIID riid, void** ppSession) {
        return inner_->CreateProtectedResourceSession1(pDesc, riid, ppSession);
    }
    virtual HRESULT STDMETHODCALLTYPE Slot107() { return RecordUnimpl(107); }
    // Slot 108 (+0x360): ID3D12Device8::GetResourceAllocationInfo2 (patched via Thunk_GetResourceAllocationInfo2)
    virtual D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) {
        std::vector<D3D12_RESOURCE_DESC> clean;
        if (pResourceDescs && numResourceDescs > 0u) {
            clean.resize(numResourceDescs);
            for (UINT i = 0u; i < numResourceDescs; ++i) {
                D3D12_RESOURCE_DESC d0{};
                d0.Dimension = pResourceDescs[i].Dimension;
                d0.Alignment = pResourceDescs[i].Alignment;
                d0.Width = pResourceDescs[i].Width;
                d0.Height = pResourceDescs[i].Height;
                d0.DepthOrArraySize = pResourceDescs[i].DepthOrArraySize;
                d0.MipLevels = pResourceDescs[i].MipLevels;
                d0.Format = pResourceDescs[i].Format;
                d0.SampleDesc = pResourceDescs[i].SampleDesc;
                d0.Layout = pResourceDescs[i].Layout;
                d0.Flags = pResourceDescs[i].Flags;
                clean[i] = SanitizeRetailResourceDesc(d0);
            }
        }
        return inner_->GetResourceAllocationInfo1(
            visibleMask, numResourceDescs, clean.empty() ? nullptr : clean.data(), pResourceAllocationInfo1);
    }
    // Slot 109 (+0x368): ID3D12Device8::CreateCommittedResource2
    virtual HRESULT STDMETHODCALLTYPE CreateCommittedResource2(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource,
        void** ppvResource) {
        (void)pProtectedSession;
        D3D12_RESOURCE_DESC desc0{};
        const D3D12_RESOURCE_DESC* desc_ptr = nullptr;
        if (pDesc) {
            desc0.Dimension = pDesc->Dimension;
            desc0.Alignment = pDesc->Alignment;
            desc0.Width = pDesc->Width;
            desc0.Height = pDesc->Height;
            desc0.DepthOrArraySize = pDesc->DepthOrArraySize;
            desc0.MipLevels = pDesc->MipLevels;
            desc0.Format = pDesc->Format;
            desc0.SampleDesc = pDesc->SampleDesc;
            desc0.Layout = pDesc->Layout;
            desc0.Flags = pDesc->Flags;
            desc0 = SanitizeRetailResourceDesc(desc0);
            desc_ptr = &desc0;
        }
        D3D12_RESOURCE_STATES state = InitialResourceState;
        if (static_cast<uint32_t>(state) == 0xFFFFFFFFu) {
            state = D3D12_RESOURCE_STATE_COMMON;
        }
        HRESULT hr = inner_->CreateCommittedResource(
            pHeapProperties, HeapFlags, desc_ptr, state, pOptimizedClearValue, riidResource, ppvResource);
        if (FAILED(hr) && state != D3D12_RESOURCE_STATE_COMMON) {
            hr = inner_->CreateCommittedResource(
                pHeapProperties, HeapFlags, desc_ptr, D3D12_RESOURCE_STATE_COMMON,
                pOptimizedClearValue, riidResource, ppvResource);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateCommittedResource2",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 109);
        return hr;
    }
    // Slot 110 (+0x370): ID3D12Device8::CreatePlacedResource1
    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResource1(
        ID3D12Heap* pHeap,
        UINT64 HeapOffset,
        const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) {
        D3D12_RESOURCE_DESC desc0{};
        const D3D12_RESOURCE_DESC* desc_ptr = nullptr;
        if (pDesc) {
            desc0.Dimension = pDesc->Dimension;
            desc0.Alignment = pDesc->Alignment;
            desc0.Width = pDesc->Width;
            desc0.Height = pDesc->Height;
            desc0.DepthOrArraySize = pDesc->DepthOrArraySize;
            desc0.MipLevels = pDesc->MipLevels;
            desc0.Format = pDesc->Format;
            desc0.SampleDesc = pDesc->SampleDesc;
            desc0.Layout = pDesc->Layout;
            desc0.Flags = pDesc->Flags;
            desc0 = SanitizeRetailResourceDesc(desc0);
            desc_ptr = &desc0;
        }
        D3D12_RESOURCE_STATES state = InitialState;
        if (static_cast<uint32_t>(state) == 0xFFFFFFFFu) {
            state = D3D12_RESOURCE_STATE_COMMON;
        }
        const HRESULT hr = inner_->CreatePlacedResource(
            pHeap, HeapOffset, desc_ptr, state, pOptimizedClearValue, riid, ppvResource);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreatePlacedResource1",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 110);
        return hr;
    }
    // Slot 111 (+0x378): ID3D12Device8::CreateSamplerFeedbackUnorderedAccessView
    virtual void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(
        ID3D12Resource* pTargetedResource,
        ID3D12Resource* pFeedbackResource,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
        inner_->CreateSamplerFeedbackUnorderedAccessView(
            pTargetedResource, pFeedbackResource, DestDescriptor);
    }
    // Slot 112 (+0x380): ID3D12Device8::GetCopyableFootprints1
    virtual void STDMETHODCALLTYPE GetCopyableFootprints1(
        const D3D12_RESOURCE_DESC1* pResourceDesc,
        UINT FirstSubresource,
        UINT NumSubresources,
        UINT64 BaseOffset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
        UINT* pNumRows,
        UINT64* pRowSizeInBytes,
        UINT64* pTotalBytes) {
        D3D12_RESOURCE_DESC desc0{};
        const D3D12_RESOURCE_DESC* desc_ptr = nullptr;
        if (pResourceDesc) {
            desc0.Dimension = pResourceDesc->Dimension;
            desc0.Alignment = pResourceDesc->Alignment;
            desc0.Width = pResourceDesc->Width;
            desc0.Height = pResourceDesc->Height;
            desc0.DepthOrArraySize = pResourceDesc->DepthOrArraySize;
            desc0.MipLevels = pResourceDesc->MipLevels;
            desc0.Format = pResourceDesc->Format;
            desc0.SampleDesc = pResourceDesc->SampleDesc;
            desc0.Layout = pResourceDesc->Layout;
            desc0.Flags = pResourceDesc->Flags;
            desc0 = SanitizeRetailResourceDesc(desc0);
            desc_ptr = &desc0;
        }
        inner_->GetCopyableFootprints(
            desc_ptr, FirstSubresource, NumSubresources, BaseOffset,
            pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
    }
    virtual HRESULT STDMETHODCALLTYPE Slot113() { return RecordUnimpl(113); }
    virtual HRESULT STDMETHODCALLTYPE Slot114() { return RecordUnimpl(114); }
    // Slot 115 (+0x398): Xbox ID3D12Device8::DeserializeStateObjectX
    virtual HRESULT STDMETHODCALLTYPE DeserializeStateObjectX(
        const void* pBlob, SIZE_T blobSize, ID3D12RootSignature* pRootSignature,
        UINT flags, REFIID riid, void** ppStateObject) {
        return inner_->DeserializeStateObjectX(pBlob, blobSize, pRootSignature, flags, riid, ppStateObject);
    }
    virtual HRESULT STDMETHODCALLTYPE Slot116() { return RecordUnimpl(116); }
    virtual HRESULT STDMETHODCALLTYPE Slot117() { return RecordUnimpl(117); }
    virtual HRESULT STDMETHODCALLTYPE Slot118() { return RecordUnimpl(118); }
    virtual HRESULT STDMETHODCALLTYPE Slot119() { return RecordUnimpl(119); }
    virtual HRESULT STDMETHODCALLTYPE Slot120() { return RecordUnimpl(120); }
    virtual HRESULT STDMETHODCALLTYPE Slot121() { return RecordUnimpl(121); }
    virtual HRESULT STDMETHODCALLTYPE Slot122() { return RecordUnimpl(122); }
    virtual HRESULT STDMETHODCALLTYPE Slot123() { return RecordUnimpl(123); }
    virtual HRESULT STDMETHODCALLTYPE Slot124() { return RecordUnimpl(124); }
    virtual HRESULT STDMETHODCALLTYPE Slot125() { return RecordUnimpl(125); }
    virtual HRESULT STDMETHODCALLTYPE Slot126() { return RecordUnimpl(126); }
    // Slot 127 (+0x3F8): CreateCommittedResource3
    virtual HRESULT STDMETHODCALLTYPE CreateCommittedResource3(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc,
        uint32_t InitialLayout,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        UINT32 NumCastableFormats,
        const DXGI_FORMAT* pCastableFormats,
        REFIID riidResource,
        void** ppvResource) {
        (void)pProtectedSession;
        (void)NumCastableFormats;
        (void)pCastableFormats;
        D3D12_RESOURCE_DESC desc0{};
        const D3D12_RESOURCE_DESC* desc_ptr = nullptr;
        if (pDesc) {
            desc0.Dimension = pDesc->Dimension;
            desc0.Alignment = pDesc->Alignment;
            desc0.Width = pDesc->Width;
            desc0.Height = pDesc->Height;
            desc0.DepthOrArraySize = pDesc->DepthOrArraySize;
            desc0.MipLevels = pDesc->MipLevels;
            desc0.Format = pDesc->Format;
            desc0.SampleDesc = pDesc->SampleDesc;
            desc0.Layout = pDesc->Layout;
            desc0.Flags = pDesc->Flags;
            desc0 = SanitizeRetailResourceDesc(desc0);
            desc_ptr = &desc0;
        }
        const D3D12_RESOURCE_STATES state = MapBarrierLayoutToResourceState(InitialLayout);
        const HRESULT hr = inner_->CreateCommittedResource(
            pHeapProperties, HeapFlags, desc_ptr, state, pOptimizedClearValue, riidResource, ppvResource);
        if (SUCCEEDED(hr) && desc_ptr) {
            // On Xbox Series X, D3D12X committed resources notify the title's exported
            // __XMemAllocationHooks__ = {alloc(size, attrs), free(ptr, attrs)} for allocatorId 0x80
            // (adapted from MIT R0Y00/Xbox2Windows title_resource_memory). Invoke the title hook
            // directly without XMemAllocDefault fallback, and release on ID3D12Resource destruction.
            using PfnTitleAllocHook = void*(__stdcall*)(size_t, uint64_t);
            using PfnTitleFreeHook = void(__stdcall*)(void*, uint64_t);
            struct TitleHookPair {
                PfnTitleAllocHook alloc_fn = nullptr;
                PfnTitleFreeHook free_fn = nullptr;
            };
            static const TitleHookPair s_title_hooks = []() -> TitleHookPair {
                TitleHookPair out{};
                HMODULE main_mod = ::GetModuleHandleW(nullptr);
                if (!main_mod) return out;
                const auto* tbl = reinterpret_cast<const uintptr_t*>(
                    ::GetProcAddress(main_mod, "__XMemAllocationHooks__"));
                if (tbl && tbl[0] >= 0x10000ull) {
                    out.alloc_fn = reinterpret_cast<PfnTitleAllocHook>(tbl[0]);
                    if (tbl[1] >= 0x10000ull) {
                        out.free_fn = reinterpret_cast<PfnTitleFreeHook>(tbl[1]);
                    }
                }
                return out;
            }();
            if (s_title_hooks.alloc_fn) {
                const D3D12_RESOURCE_ALLOCATION_INFO ai =
                    inner_->GetResourceAllocationInfo(0u, 1u, desc_ptr);
                if (ai.SizeInBytes > 0u && ai.SizeInBytes != UINT64_MAX) {
                    uint32_t log2_align = 0u;
                    while ((1ull << (log2_align + 1u)) <= ai.Alignment && log2_align < 31u) {
                        ++log2_align;
                    }
                    const uint64_t page_code = (ai.SizeInBytes >= (4ull << 20)) ? 2ull : 1ull;
                    const uint64_t attrs =
                        (page_code << 14) | (0x80ull << 16) | (static_cast<uint64_t>(log2_align) << 24);
                    void* title_mem = s_title_hooks.alloc_fn(static_cast<size_t>(ai.SizeInBytes), attrs);
                    if (title_mem && s_title_hooks.free_fn && ppvResource && *ppvResource) {
                        class TitleHookReleaseGuard final : public IUnknown {
                        public:
                            TitleHookReleaseGuard(PfnTitleFreeHook fn, void* p, uint64_t a) noexcept
                                : free_fn_(fn), ptr_(p), attrs_(a) {}
                            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
                                if (!out) return E_POINTER;
                                *out = nullptr;
                                if (riid == IID_IUnknown) {
                                    *out = static_cast<IUnknown*>(this);
                                    AddRef();
                                    return S_OK;
                                }
                                return E_NOINTERFACE;
                            }
                            ULONG STDMETHODCALLTYPE AddRef() override {
                                return static_cast<ULONG>(::InterlockedIncrement(&refs_));
                            }
                            ULONG STDMETHODCALLTYPE Release() override {
                                const ULONG r = static_cast<ULONG>(::InterlockedDecrement(&refs_));
                                if (r == 0u) delete this;
                                return r;
                            }
                        private:
                            ~TitleHookReleaseGuard() {
                                if (free_fn_ && ptr_) free_fn_(ptr_, attrs_);
                            }
                            LONG refs_ = 1;
                            PfnTitleFreeHook free_fn_ = nullptr;
                            void* ptr_ = nullptr;
                            uint64_t attrs_ = 0ull;
                        };
                        static constexpr GUID kTitleHookGuardGuid = {
                            0x91D3A741u, 0x5B2Cu, 0x4E8Au, {0x9Au, 0x62u, 0x31u, 0x7Eu, 0x44u, 0x10u, 0x8Bu, 0xC9u}};
                        ID3D12Object* obj = nullptr;
                        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(
                                __uuidof(ID3D12Object), reinterpret_cast<void**>(&obj))) &&
                            obj) {
                            auto* guard = new (std::nothrow)
                                TitleHookReleaseGuard(s_title_hooks.free_fn, title_mem, attrs);
                            if (guard) {
                                (void)obj->SetPrivateDataInterface(kTitleHookGuardGuid, guard);
                                guard->Release();
                            }
                            obj->Release();
                        }
                    }
                }
            }
        }
        static const bool trace_cr3 = []() {
            char v[16]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
            return n > 0u && v[0] != '0';
        }();
        if (trace_cr3) {
            std::fprintf(stderr,
                "[OpenScarlett][d3d12_xs] CreateCommittedResource3(dim=%u %llux%u fmt=%u layout=%u rawFlags=0x%X) -> hr=0x%08lX\n",
                pDesc ? static_cast<unsigned>(pDesc->Dimension) : 0u,
                pDesc ? static_cast<unsigned long long>(pDesc->Width) : 0ull,
                pDesc ? pDesc->Height : 0u,
                pDesc ? static_cast<unsigned>(pDesc->Format) : 0u,
                InitialLayout,
                pDesc ? static_cast<unsigned>(pDesc->Flags) : 0u,
                static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateCommittedResource3",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 127);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot128() { return RecordUnimpl(128); }
    virtual HRESULT STDMETHODCALLTYPE Slot129() { return RecordUnimpl(129); }
    virtual HRESULT STDMETHODCALLTYPE Slot130() { return RecordUnimpl(130); }
    virtual HRESULT STDMETHODCALLTYPE Slot131() { return RecordUnimpl(131); }
    virtual HRESULT STDMETHODCALLTYPE Slot132() { return RecordUnimpl(132); }
    // Slot 133 (+0x428): CreatePlacedResourceX2 (retail Scarlett GRDK variant using D3D12_BARRIER_LAYOUT)
    virtual HRESULT STDMETHODCALLTYPE CreatePlacedResourceX_Slot133(
        D3D12_GPU_VIRTUAL_ADDRESS gpuVirtualAddress,
        const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) {
        if (!pDesc || !ppvResource) {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", "ScarlettRetailDevice::CreatePlacedResourceX_Slot133",
                openscarlett::CallOutcome::Error, E_INVALIDARG, 133);
            return E_INVALIDARG;
        }
        const D3D12_RESOURCE_DESC clean_desc = SanitizeRetailResourceDesc(*pDesc);
        // Retail Slot 133 passes D3D12_BARRIER_LAYOUT (e.g. 0xFFFFFFFF UNDEFINED, 3 RT, 6 DEPTH_READ,
        // 7 SHADER_RESOURCE, 9 COPY_DEST); map to a valid desktop D3D12_RESOURCE_STATES value.
        D3D12_RESOURCE_STATES mapped_state =
            MapBarrierLayoutToResourceState(static_cast<uint32_t>(InitialState));
        if (clean_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            mapped_state = D3D12_RESOURCE_STATE_COMMON;
        }
        HRESULT hr = inner_->CreatePlacedResourceX(
            gpuVirtualAddress, &clean_desc, mapped_state, pOptimizedClearValue, riid, ppvResource);
        if (FAILED(hr) && mapped_state != D3D12_RESOURCE_STATE_COMMON) {
            hr = inner_->CreatePlacedResourceX(
                gpuVirtualAddress, &clean_desc, D3D12_RESOURCE_STATE_COMMON,
                pOptimizedClearValue, riid, ppvResource);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "[OpenScarlett][d3d12_xs] CreatePlacedResourceX_Slot133 FAILED va=0x%llX dim=%u %llux%u D=%u mips=%u fmt=%u layout=0x%X rawFlags=0x%X init=0x%X -> hr=0x%08lX\n",
                static_cast<unsigned long long>(gpuVirtualAddress),
                static_cast<unsigned>(pDesc->Dimension),
                static_cast<unsigned long long>(pDesc->Width),
                pDesc->Height,
                static_cast<unsigned>(pDesc->DepthOrArraySize),
                static_cast<unsigned>(pDesc->MipLevels),
                static_cast<unsigned>(pDesc->Format),
                static_cast<unsigned>(pDesc->Layout),
                static_cast<unsigned>(pDesc->Flags),
                static_cast<unsigned>(InitialState),
                static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreatePlacedResourceX_Slot133",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 133);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot134() { return RecordUnimpl(134); }
    virtual HRESULT STDMETHODCALLTYPE Slot135() { return RecordUnimpl(135); }
    virtual HRESULT STDMETHODCALLTYPE Slot136() { return RecordUnimpl(136); }
    virtual HRESULT STDMETHODCALLTYPE Slot137() { return RecordUnimpl(137); }
    virtual HRESULT STDMETHODCALLTYPE Slot138() { return RecordUnimpl(138); }
    virtual HRESULT STDMETHODCALLTYPE Slot139() { return RecordUnimpl(139); }
    virtual HRESULT STDMETHODCALLTYPE Slot140() { return RecordUnimpl(140); }
    virtual HRESULT STDMETHODCALLTYPE Slot141() { return RecordUnimpl(141); }
    virtual HRESULT STDMETHODCALLTYPE Slot142() { return RecordUnimpl(142); }
    virtual HRESULT STDMETHODCALLTYPE Slot143() { return RecordUnimpl(143); }
    virtual HRESULT STDMETHODCALLTYPE Slot144() { return RecordUnimpl(144); }
    virtual HRESULT STDMETHODCALLTYPE Slot145() { return RecordUnimpl(145); }
    virtual HRESULT STDMETHODCALLTYPE Slot146() { return RecordUnimpl(146); }
    virtual HRESULT STDMETHODCALLTYPE Slot147() { return RecordUnimpl(147); }
    virtual HRESULT STDMETHODCALLTYPE Slot148() { return RecordUnimpl(148); }
    virtual HRESULT STDMETHODCALLTYPE Slot149() { return RecordUnimpl(149); }
    virtual HRESULT STDMETHODCALLTYPE Slot150() { return RecordUnimpl(150); }
    virtual HRESULT STDMETHODCALLTYPE Slot151() { return RecordUnimpl(151); }
    virtual HRESULT STDMETHODCALLTYPE Slot152() { return RecordUnimpl(152); }
    virtual HRESULT STDMETHODCALLTYPE Slot153() { return RecordUnimpl(153); }
    virtual HRESULT STDMETHODCALLTYPE Slot154() { return RecordUnimpl(154); }
    virtual HRESULT STDMETHODCALLTYPE Slot155() { return RecordUnimpl(155); }
    virtual HRESULT STDMETHODCALLTYPE Slot156() { return RecordUnimpl(156); }
    virtual HRESULT STDMETHODCALLTYPE Slot157() { return RecordUnimpl(157); }
    virtual HRESULT STDMETHODCALLTYPE Slot158() { return RecordUnimpl(158); }
    virtual HRESULT STDMETHODCALLTYPE Slot159() { return RecordUnimpl(159); }
    virtual HRESULT STDMETHODCALLTYPE Slot160() { return RecordUnimpl(160); }
    virtual HRESULT STDMETHODCALLTYPE Slot161() { return RecordUnimpl(161); }
    virtual HRESULT STDMETHODCALLTYPE Slot162() { return RecordUnimpl(162); }
    virtual HRESULT STDMETHODCALLTYPE Slot163() { return RecordUnimpl(163); }
    virtual HRESULT STDMETHODCALLTYPE Slot164() { return RecordUnimpl(164); }
    virtual HRESULT STDMETHODCALLTYPE Slot165() { return RecordUnimpl(165); }
    virtual HRESULT STDMETHODCALLTYPE Slot166() { return RecordUnimpl(166); }
    virtual HRESULT STDMETHODCALLTYPE Slot167() { return RecordUnimpl(167); }
    virtual HRESULT STDMETHODCALLTYPE Slot168() { return RecordUnimpl(168); }
    virtual HRESULT STDMETHODCALLTYPE Slot169() { return RecordUnimpl(169); }
    virtual HRESULT STDMETHODCALLTYPE Slot170() { return RecordUnimpl(170); }
    virtual HRESULT STDMETHODCALLTYPE Slot171() { return RecordUnimpl(171); }
    virtual HRESULT STDMETHODCALLTYPE Slot172() { return RecordUnimpl(172); }
    virtual HRESULT STDMETHODCALLTYPE Slot173() { return RecordUnimpl(173); }
    virtual HRESULT STDMETHODCALLTYPE Slot174() { return RecordUnimpl(174); }
    virtual HRESULT STDMETHODCALLTYPE Slot175() { return RecordUnimpl(175); }
    virtual HRESULT STDMETHODCALLTYPE Slot176() { return RecordUnimpl(176); }
    virtual HRESULT STDMETHODCALLTYPE Slot177() { return RecordUnimpl(177); }
    virtual HRESULT STDMETHODCALLTYPE Slot178() { return RecordUnimpl(178); }
    virtual HRESULT STDMETHODCALLTYPE Slot179() { return RecordUnimpl(179); }
    virtual HRESULT STDMETHODCALLTYPE Slot180() { return RecordUnimpl(180); }
    virtual HRESULT STDMETHODCALLTYPE Slot181() { return RecordUnimpl(181); }
    virtual HRESULT STDMETHODCALLTYPE Slot182() { return RecordUnimpl(182); }
    virtual HRESULT STDMETHODCALLTYPE Slot183() { return RecordUnimpl(183); }
    virtual HRESULT STDMETHODCALLTYPE Slot184() { return RecordUnimpl(184); }
    virtual HRESULT STDMETHODCALLTYPE Slot185() { return RecordUnimpl(185); }
    virtual HRESULT STDMETHODCALLTYPE Slot186() { return RecordUnimpl(186); }
    virtual HRESULT STDMETHODCALLTYPE Slot187() { return RecordUnimpl(187); }
    virtual HRESULT STDMETHODCALLTYPE Slot188() { return RecordUnimpl(188); }
    virtual HRESULT STDMETHODCALLTYPE Slot189() { return RecordUnimpl(189); }
    virtual HRESULT STDMETHODCALLTYPE Slot190() { return RecordUnimpl(190); }
    virtual HRESULT STDMETHODCALLTYPE Slot191() { return RecordUnimpl(191); }

private:
    static inline void* const* s_patched_vtbl_ptr_ = nullptr;
    std::atomic<ULONG> ref_count_{1u};
    openscarlett::d3d12_xs::DeviceX* inner_ = nullptr;
    uint32_t retail_version_ = 0u;
};

HRESULT CreateWrappedDeviceInternal(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL min_level,
    bool prefer_warp,
    bool enable_debug_layer,
    HWND default_hwnd,
    REFIID riid,
    void** ppDevice,
    bool retail_scarlett_vtable = false,
    uint32_t retail_scarlett_version = 0u) {
    // Opt-in D3D12 Agility SDK discovery (graceful fallback to inbox d3d12.dll when absent).
    TryConfigureAgilitySdkOnce();
    {
        char dbg_env[16]{};
        if (::GetEnvironmentVariableA("OPENSCARLETT_ENABLE_D3D12_DEBUG", dbg_env, sizeof(dbg_env)) > 0 &&
            dbg_env[0] == '1') {
            auto fn_dbg = reinterpret_cast<PfnD3D12GetDebugInterface>(
                openscarlett::ResolveHostD3D12Proc("D3D12GetDebugInterface"));
            ID3D12Debug* dbg = nullptr;
            if (fn_dbg && SUCCEEDED(fn_dbg(IID_PPV_ARGS(&dbg))) && dbg) {
                dbg->EnableDebugLayer();
                dbg->Release();
                std::fprintf(stderr, "[OpenScarlett][d3d12_xs] D3D12 Debug Layer ENABLED via OPENSCARLETT_ENABLE_D3D12_DEBUG=1\n");
                std::fflush(stderr);
            }
        }
        if (::GetEnvironmentVariableA("OPENSCARLETT_ENABLE_DRED", dbg_env, sizeof(dbg_env)) > 0 &&
            dbg_env[0] == '1') {
            auto fn_dbg = reinterpret_cast<PfnD3D12GetDebugInterface>(
                openscarlett::ResolveHostD3D12Proc("D3D12GetDebugInterface"));
            ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
            if (fn_dbg && SUCCEEDED(fn_dbg(IID_PPV_ARGS(&dred))) && dred) {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->Release();
                std::fprintf(stderr, "[OpenScarlett][d3d12_xs] D3D12 DRED (AutoBreadcrumbs + PageFault) ENABLED\n");
                std::fflush(stderr);
            }
        }
    }

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

    if (pAdapter && !retail_scarlett_vtable) {
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
    if (enable_debug_layer && host_dev) {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(host_dev->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            iq->Release();
        }
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
        ID3D12InfoQueue1* iq1 = nullptr;
        if (SUCCEEDED(host_dev->QueryInterface(IID_PPV_ARGS(&iq1))) && iq1) {
            DWORD cb_cookie = 0u;
            (void)iq1->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY /*cat*/,
                   D3D12_MESSAGE_SEVERITY sev,
                   D3D12_MESSAGE_ID id,
                   LPCSTR desc,
                   void* /*ctx*/) {
                    if (sev <= D3D12_MESSAGE_SEVERITY_WARNING && desc != nullptr) {
                        static std::atomic<uint32_t> s_cb_cnt{0u};
                        const uint32_t n = s_cb_cnt.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 64u) {
                            std::fprintf(stderr, "[OpenScarlett][D3D12-CB #%u sev=%d id=%d] %s\n",
                                         n, static_cast<int>(sev), static_cast<int>(id), desc);
                            std::fflush(stderr);
                        }
                    }
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                nullptr,
                &cb_cookie);
            iq1->Release();
        }
#endif
    }
    if (retail_scarlett_vtable) {
        static std::once_flag s_crash_veh_once;
        std::call_once(s_crash_veh_once, []() {
            ::AddVectoredExceptionHandler(1u, [](EXCEPTION_POINTERS* ep) -> LONG {
                if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                const DWORD code = ep->ExceptionRecord->ExceptionCode;
                const auto* rip = reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rip);
                if (code == 0x0000087Du) {
                    static std::atomic<uint32_t> s_87d_cnt{0u};
                    const uint32_t hit = s_87d_cnt.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (hit <= 8u) {
                        std::fprintf(stderr, "[OpenScarlett][D3D12-0x87D] #%u tid=%lu flags=0x%lX rsp=0x%llX\n",
                                     hit, ::GetCurrentThreadId(),
                                     static_cast<unsigned long>(ep->ExceptionRecord->ExceptionFlags),
                                     static_cast<unsigned long long>(ep->ContextRecord->Rsp));
                        const uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
                        uint32_t bt_printed = 0u;
                        for (size_t si = 0u; si < 64u && bt_printed < 16u; ++si) {
                            const uintptr_t slot_addr = rsp + si * sizeof(uint64_t);
                            if (!IsReadableRetailRange(slot_addr, sizeof(uint64_t))) break;
                            uint64_t q = 0ull;
                            std::memcpy(&q, reinterpret_cast<const void*>(slot_addr), sizeof(q));
                            if (q < 0x10000000ull) continue;
                            HMODULE smod = nullptr;
                            if (::GetModuleHandleExA(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(q)), &smod) && smod) {
                                MEMORY_BASIC_INFORMATION smbi{};
                                if (::VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(q)), &smbi, sizeof(smbi)) != 0 &&
                                    (smbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0u) {
                                    char spath[MAX_PATH]{};
                                    ::GetModuleFileNameA(smod, spath, sizeof(spath));
                                    const char* base = std::strrchr(spath, '\\');
                                    const uintptr_t srva = static_cast<uintptr_t>(q) - reinterpret_cast<uintptr_t>(smod);
                                    std::fprintf(stderr, "  [0x87D-BT sp+0x%03zX] %s+0x%llX\n",
                                                 si * sizeof(uint64_t),
                                                 base ? (base + 1) : spath,
                                                 static_cast<unsigned long long>(srva));
                                    ++bt_printed;
                                }
                            }
                        }
                        std::fflush(stderr);
                    }
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                // Retail console hang-watchdog patterns:
                // 1) mov dword ptr [0], 0; int 3 (C7 04 25 00 00 00 00 00 00 00 00 CC, 12 bytes)
                // 2) mov dword ptr [0], <imm32> (C7 04 25 00 00 00 00 xx xx xx xx, 11 bytes, e.g. 0x4D8BD421 / 0x2EF1E94B)
                // Console titles set a strict ~30s QPC budget calibrated for Xbox NVMe + precompiled shaders;
                // on PC cold storage / on-demand DXC shader lifting, log and step cleanly to recovery.
                if (code == EXCEPTION_ACCESS_VIOLATION &&
                    ep->ExceptionRecord->NumberParameters >= 2u &&
                    ep->ExceptionRecord->ExceptionInformation[0] == 1u &&
                    ep->ExceptionRecord->ExceptionInformation[1] == 0u &&
                    IsReadableRetailRange(reinterpret_cast<uintptr_t>(rip), 13u)) {
                    static constexpr uint8_t kWatchdogPrefix[7] = {
                        0xC7u, 0x04u, 0x25u, 0x00u, 0x00u, 0x00u, 0x00u};
                    size_t skip_len = 0u;
                    if (std::memcmp(rip, kWatchdogPrefix, sizeof(kWatchdogPrefix)) == 0) {
                        skip_len = (rip[11] == 0xCCu && rip[12] != 0xCCu) ? 12u : 11u;
                    }
                    if (skip_len > 0u) {
                        static std::atomic<uint32_t> s_wd_hits{0u};
                        const uint32_t n = s_wd_hits.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 12u) {
                            std::fprintf(
                                stderr,
                                "[OpenScarlett][WATCHDOG] Suppressed console hang-watchdog trap #%u at rip=0x%llX (+%zuB, tid=%lu)\n",
                                n,
                                static_cast<unsigned long long>(ep->ContextRecord->Rip),
                                skip_len,
                                ::GetCurrentThreadId());
                            std::fflush(stderr);
                        }
                        ep->ContextRecord->Rip += skip_len;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                // Guard post-FTUE Scaleform slab teardown non-canonical pointer dereference
                // (`and dword ptr [rbx+rax*4+8], edx` = `21 54 83 08` at RVA +0x550A5F6) ->
                // unwind cleanly via the function epilogue (+0x550A733 / +0x550A742).
                if (code == EXCEPTION_ACCESS_VIOLATION &&
                    IsReadableRetailRange(reinterpret_cast<uintptr_t>(rip), 8u) &&
                    rip[0] == 0x21u && rip[1] == 0x54u && rip[2] == 0x83u && rip[3] == 0x08u) {
                    HMODULE hit_mod = nullptr;
                    const HMODULE main_exe = ::GetModuleHandleW(nullptr);
                    if (main_exe &&
                        ::GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(rip), &hit_mod) &&
                        hit_mod == main_exe) {
                        const uintptr_t base = reinterpret_cast<uintptr_t>(main_exe);
                        const uintptr_t rva = reinterpret_cast<uintptr_t>(rip) - base;
                        if (rva == 0x550A5F6u &&
                            IsReadableRetailRange(base + 0x550A733u, 24u)) {
                            const uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
                            if (IsReadableRetailRange(rsp + 0x50u, 16u)) {
                                std::memcpy(&ep->ContextRecord->Xmm6,
                                            reinterpret_cast<const void*>(rsp + 0x50u), 16u);
                            }
                            if (IsReadableRetailRange(static_cast<uintptr_t>(ep->ContextRecord->Rdi) + 0x20u, 8u)) {
                                ep->ContextRecord->Rip = base + 0x550A733u;
                            } else {
                                ep->ContextRecord->Rip = base + 0x550A742u;
                            }
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }
                // Guard hash-table linear scan (`cmp qword ptr [rcx+r10*8], r8` = `4E 39 04 D1` at
                // RVA +0x28CD253) if a partially-mapped dictionary hits an unmapped tail page:
                // return 0xFFFF ("not found") cleanly via `+0x28CD265` (`C3 ret`).
                if (code == EXCEPTION_ACCESS_VIOLATION &&
                    IsReadableRetailRange(reinterpret_cast<uintptr_t>(rip), 8u) &&
                    rip[0] == 0x4Eu && rip[1] == 0x39u && rip[2] == 0x04u && rip[3] == 0xD1u) {
                    HMODULE hit_mod = nullptr;
                    const HMODULE main_exe = ::GetModuleHandleW(nullptr);
                    if (main_exe &&
                        ::GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(rip), &hit_mod) &&
                        hit_mod == main_exe) {
                        const uintptr_t base = reinterpret_cast<uintptr_t>(main_exe);
                        const uintptr_t rva = reinterpret_cast<uintptr_t>(rip) - base;
                        if (rva == 0x28CD253u &&
                            IsReadableRetailRange(base + 0x28CD265u, 1u)) {
                            static std::atomic<uint32_t> s_hash_guard{0u};
                            const uint32_t hn = s_hash_guard.fetch_add(1u, std::memory_order_relaxed) + 1u;
                            if (hn <= 8u) {
                                std::fprintf(
                                    stderr,
                                    "[OpenScarlett][VEH-GUARD] LookupByHash +0x28CD253 #%u rcx=0x%llX rdx=0x%llX r9=0x%llX r8=0x%llX -> 0xFFFF\n",
                                    hn,
                                    static_cast<unsigned long long>(ep->ContextRecord->Rcx),
                                    static_cast<unsigned long long>(ep->ContextRecord->Rdx),
                                    static_cast<unsigned long long>(ep->ContextRecord->R9),
                                    static_cast<unsigned long long>(ep->ContextRecord->R8));
                                std::fflush(stderr);
                            }
                            ep->ContextRecord->Rax = (ep->ContextRecord->Rax & ~0xFFFFull) | 0xFFFFull;
                            ep->ContextRecord->Rip = base + 0x28CD265u;
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }
                // Guard empty task-graph predicate closure invoke (`call qword ptr [rax+0x50]` = `FF 50 50`,
                // e.g. RVA +0x4E53906 -> return RVA +0x4E53909) when a heap-allocated functor wrapper has
                // non-null storage (`rax != 0`) but null invoke pointer (`[rax+0x50] == 0`):
                // pop return address, set `al = 1` (matching the no-predicate `je +0x4E5392D` path), and resume.
                if (code == EXCEPTION_ACCESS_VIOLATION &&
                    ep->ExceptionRecord->NumberParameters >= 2u &&
                    ep->ExceptionRecord->ExceptionInformation[0] == 8u &&
                    ep->ContextRecord->Rip == 0ull) {
                    const uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
                    if (IsReadableRetailRange(rsp, sizeof(uint64_t))) {
                        uint64_t ret_ip = 0ull;
                        std::memcpy(&ret_ip, reinterpret_cast<const void*>(rsp), sizeof(ret_ip));
                        HMODULE hit_mod = nullptr;
                        const HMODULE main_exe = ::GetModuleHandleW(nullptr);
                        if (main_exe && ret_ip > 16ull &&
                            ::GetModuleHandleExA(
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ret_ip)),
                                &hit_mod) &&
                            hit_mod == main_exe &&
                            IsReadableRetailRange(static_cast<uintptr_t>(ret_ip) - 3u, 6u)) {
                            const auto* call_site =
                                reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ret_ip) - 3u);
                            if (call_site[0] == 0xFFu && call_site[1] == 0x50u && call_site[2] == 0x50u) {
                                static std::atomic<uint32_t> s_empty_fn_guard{0u};
                                const uint32_t fn_n = s_empty_fn_guard.fetch_add(1u, std::memory_order_relaxed) + 1u;
                                if (fn_n <= 16u) {
                                    const uintptr_t call_rva =
                                        (static_cast<uintptr_t>(ret_ip) - 3u) - reinterpret_cast<uintptr_t>(main_exe);
                                    std::fprintf(
                                        stderr,
                                        "[OpenScarlett][VEH-GUARD] Empty functor [rax+0x50]==0 at +0x%llX #%u (rax=0x%llX rdx=0x%llX) -> 1\n",
                                        static_cast<unsigned long long>(call_rva),
                                        fn_n,
                                        static_cast<unsigned long long>(ep->ContextRecord->Rax),
                                        static_cast<unsigned long long>(ep->ContextRecord->Rdx));
                                    std::fflush(stderr);
                                }
                                ep->ContextRecord->Rsp = rsp + sizeof(uint64_t);
                                ep->ContextRecord->Rax = 1ull;
                                ep->ContextRecord->Rip = ret_ip;
                                return EXCEPTION_CONTINUE_EXECUTION;
                            }
                        }
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            });
            ::SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
                if (ep && ep->ExceptionRecord && ep->ContextRecord) {
                    const DWORD code = ep->ExceptionRecord->ExceptionCode;
                    const uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
                    HMODULE mod = nullptr;
                    char mod_path[MAX_PATH]{};
                    uintptr_t rva = addr;
                    if (::GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
                        ::GetModuleFileNameA(mod, mod_path, sizeof(mod_path));
                        rva = addr - reinterpret_cast<uintptr_t>(mod);
                    }
                    std::fprintf(
                        stderr,
                        "[OpenScarlett][FATAL-CRASH] tid=%lu code=0x%08lX rip=0x%llX (%s+0x%llX) "
                        "av_op=%llu av_addr=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX rsp=0x%llX\n",
                        ::GetCurrentThreadId(),
                        static_cast<unsigned long>(code),
                        static_cast<unsigned long long>(addr),
                        mod_path[0] ? mod_path : "unknown",
                        static_cast<unsigned long long>(rva),
                        static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0]),
                        static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]),
                        static_cast<unsigned long long>(ep->ContextRecord->Rax),
                        static_cast<unsigned long long>(ep->ContextRecord->Rcx),
                        static_cast<unsigned long long>(ep->ContextRecord->Rdx),
                        static_cast<unsigned long long>(ep->ContextRecord->R8),
                        static_cast<unsigned long long>(ep->ContextRecord->R9),
                        static_cast<unsigned long long>(ep->ContextRecord->Rsp));
                    const uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
                    uint32_t bt_printed = 0u;
                    for (size_t si = 0u; si < 48u && bt_printed < 12u; ++si) {
                        const uintptr_t slot_addr = rsp + si * sizeof(uint64_t);
                        if (!IsReadableRetailRange(slot_addr, sizeof(uint64_t))) break;
                        uint64_t q = 0ull;
                        std::memcpy(&q, reinterpret_cast<const void*>(slot_addr), sizeof(q));
                        if (q < 0x10000000ull) continue;
                        HMODULE smod = nullptr;
                        if (::GetModuleHandleExA(
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(q)), &smod) && smod) {
                            MEMORY_BASIC_INFORMATION smbi{};
                            if (::VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(q)), &smbi, sizeof(smbi)) != 0 &&
                                (smbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0u) {
                                char spath[MAX_PATH]{};
                                ::GetModuleFileNameA(smod, spath, sizeof(spath));
                                const char* base = std::strrchr(spath, '\\');
                                const uintptr_t srva = static_cast<uintptr_t>(q) - reinterpret_cast<uintptr_t>(smod);
                                std::fprintf(stderr, "  [BT sp+0x%03zX] %s+0x%llX (0x%llX)\n",
                                             si * sizeof(uint64_t),
                                             base ? (base + 1) : spath,
                                             static_cast<unsigned long long>(srva),
                                             static_cast<unsigned long long>(q));
                                ++bt_printed;
                            }
                        }
                    }
                    std::fflush(stderr);
                }
                return EXCEPTION_EXECUTE_HANDLER;
            });
        });
        std::fprintf(stderr, "[OpenScarlett][d3d12_xs] Retail device created on '%s' (VRAM=%lluMB, FL=0x%X, SM=0x%X, RT=%u, Mesh=%u, pAdapter=%p)\n",
            info.description.c_str(),
            static_cast<unsigned long long>(info.dedicated_video_memory_mb),
            info.feature_level, info.highest_shader_model,
            info.raytracing_tier, info.mesh_shader_tier, static_cast<void*>(pAdapter));
        std::fflush(stderr);
    }

    auto* dev_x = new (std::nothrow) openscarlett::d3d12_xs::DeviceX(host_dev, info, default_hwnd);
    host_dev->Release();
    if (!dev_x) {
        return E_OUTOFMEMORY;
    }

    if (retail_scarlett_vtable) {
        dev_x->SetRetailScarlettAbi(true);
        auto* retail = new (std::nothrow) ScarlettRetailDeviceProxy(dev_x, retail_scarlett_version);
        if (!retail) {
            dev_x->Release();
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = retail->QueryInterface(riid, ppDevice);
        if (SUCCEEDED(hr) && *ppDevice != nullptr) {
            StoreActiveDevice(dev_x);
        }
        retail->Release();
        dev_x->Release();
        return hr;
    }

    const HRESULT hr = dev_x->QueryInterface(riid, ppDevice);
    if (SUCCEEDED(hr) && *ppDevice != nullptr) {
        StoreActiveDevice(dev_x);
    }
    dev_x->Release();
    return hr;
}

}  // namespace

namespace openscarlett::d3d12_xs {

void QueryAgilitySdkRuntimeSnapshot(D3D12XBOX_AGILITY_SDK_INFO* out_info) noexcept {
    if (!out_info) return;
    std::memset(out_info, 0, sizeof(*out_info));
    TryConfigureAgilitySdkOnce();
    auto& st = GetAgilityState();
    out_info->exported_sdk_version = D3D12SDKVersion;
    out_info->configured_sdk_version = st.sdk_version.load(std::memory_order_acquire);
    out_info->core_dll_found = st.core_dll_found.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->agility_active = st.configured.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->using_inbox_fallback = st.using_inbox_fallback.load(std::memory_order_acquire) ? 1u : 0u;
    out_info->last_config_hr = st.last_hr.load(std::memory_order_acquire);
    if (D3D12SDKPath) {
        const size_t plen = std::min<size_t>(std::strlen(D3D12SDKPath), sizeof(out_info->exported_sdk_path) - 1u);
        std::memcpy(out_info->exported_sdk_path, D3D12SDKPath, plen);
        out_info->exported_sdk_path[plen] = '\0';
    }
    std::lock_guard<std::mutex> lk(st.path_mutex);
    if (!st.discovered_dir.empty()) {
        const size_t dlen = std::min<size_t>(st.discovered_dir.size(), sizeof(out_info->discovered_core_dir) - 1u);
        std::memcpy(out_info->discovered_core_dir, st.discovered_dir.data(), dlen);
        out_info->discovered_core_dir[dlen] = '\0';
    }
}

}  // namespace openscarlett::d3d12_xs

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
        EnvEnablesD3D12Debug() ||
        (pParameters && (pParameters->ProcessDebugFlags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0);

    const bool retail_scarlett = (pParameters && pParameters->Version >= 0x00020000u);
    const uint32_t retail_version = pParameters ? pParameters->Version : 0u;

    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        D3D_FEATURE_LEVEL_12_0,
        global_warp,
        debug_layer,
        nullptr,
        riid,
        ppDevice,
        retail_scarlett,
        retail_version);
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
    if (!fn) {
        RecordExport("D3D12SerializeRootSignature", E_FAIL);
        return E_FAIL;
    }
    HRESULT hr = fn(pRootSignature, Version, ppBlob, ppErrorBlob);
    if (FAILED(hr) && pRootSignature != nullptr &&
        (static_cast<UINT>(pRootSignature->Flags) & ~0x00FFu) != 0u) {
        static constexpr UINT kMasks[] = {0x0FFFu, 0x03FFu, 0x00FFu};
        for (UINT mask : kMasks) {
            D3D12_ROOT_SIGNATURE_DESC safe = *pRootSignature;
            safe.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
                static_cast<UINT>(safe.Flags) & mask);
            if (ppErrorBlob && *ppErrorBlob) {
                (*ppErrorBlob)->Release();
                *ppErrorBlob = nullptr;
            }
            if (ppBlob && *ppBlob) {
                (*ppBlob)->Release();
                *ppBlob = nullptr;
            }
            hr = fn(&safe, Version, ppBlob, ppErrorBlob);
            if (SUCCEEDED(hr)) {
                break;
            }
        }
    }
    RecordExport("D3D12SerializeRootSignature", hr);
    return hr;
}

HRESULT WINAPI OS_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob,
    ID3DBlob** ppErrorBlob) {
    auto fn = reinterpret_cast<PfnD3D12SerializeVersionedRootSignature>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeVersionedRootSignature"));
    if (!fn) {
        RecordExport("D3D12SerializeVersionedRootSignature", E_FAIL);
        return E_FAIL;
    }
    HRESULT hr = fn(pRootSignature, ppBlob, ppErrorBlob);
    if (FAILED(hr) && pRootSignature != nullptr) {
        static constexpr UINT kMasks[] = {0x0FFFu, 0x03FFu, 0x00FFu};
        for (UINT mask : kMasks) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC safe = *pRootSignature;
            if (safe.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
                safe.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
                    static_cast<UINT>(safe.Desc_1_0.Flags) & mask);
            } else if (safe.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
                safe.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
                    static_cast<UINT>(safe.Desc_1_1.Flags) & mask);
            } else {
                break;
            }
            if (ppErrorBlob && *ppErrorBlob) {
                (*ppErrorBlob)->Release();
                *ppErrorBlob = nullptr;
            }
            if (ppBlob && *ppBlob) {
                (*ppBlob)->Release();
                *ppBlob = nullptr;
            }
            hr = fn(&safe, ppBlob, ppErrorBlob);
            if (SUCCEEDED(hr)) {
                break;
            }
        }
    }
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

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12ConfigureVirtualMemoryX(
    const void* pDesc) {
    (void)pDesc;
    RecordExport("D3D12ConfigureVirtualMemoryX", S_OK);
    return S_OK;
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

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxSetProcessGpuPriority(
    D3D12XBOX_PROCESS_GPU_PRIORITY Priority) {
    const uint32_t raw = static_cast<uint32_t>(Priority);
    if (raw > static_cast<uint32_t>(D3D12XBOX_PROCESS_GPU_PRIORITY_REALTIME)) {
        RecordExport("D3D12XboxSetProcessGpuPriority", E_INVALIDARG);
        return E_INVALIDARG;
    }
    g_process_gpu_priority.store(raw, std::memory_order_release);
    RecordExport("D3D12XboxSetProcessGpuPriority", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxGetProcessGpuPriority(
    D3D12XBOX_PROCESS_GPU_PRIORITY* pOutPriority) {
    if (!pOutPriority) {
        RecordExport("D3D12XboxGetProcessGpuPriority", E_POINTER);
        return E_POINTER;
    }
    *pOutPriority = static_cast<D3D12XBOX_PROCESS_GPU_PRIORITY>(
        g_process_gpu_priority.load(std::memory_order_acquire));
    RecordExport("D3D12XboxGetProcessGpuPriority", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxSetProcessMemoryPriority(
    D3D12XBOX_PROCESS_MEMORY_PRIORITY Priority) {
    const uint32_t raw = static_cast<uint32_t>(Priority);
    if (raw > static_cast<uint32_t>(D3D12XBOX_PROCESS_MEMORY_PRIORITY_HIGH)) {
        RecordExport("D3D12XboxSetProcessMemoryPriority", E_INVALIDARG);
        return E_INVALIDARG;
    }
    g_process_memory_priority.store(raw, std::memory_order_release);
    RecordExport("D3D12XboxSetProcessMemoryPriority", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxGetProcessMemoryPriority(
    D3D12XBOX_PROCESS_MEMORY_PRIORITY* pOutPriority) {
    if (!pOutPriority) {
        RecordExport("D3D12XboxGetProcessMemoryPriority", E_POINTER);
        return E_POINTER;
    }
    *pOutPriority = static_cast<D3D12XBOX_PROCESS_MEMORY_PRIORITY>(
        g_process_memory_priority.load(std::memory_order_acquire));
    RecordExport("D3D12XboxGetProcessMemoryPriority", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxGetDevice(
    REFIID riid,
    void** ppDevice) {
    if (!ppDevice) {
        RecordExport("D3D12XboxGetDevice", E_POINTER);
        return E_POINTER;
    }
    *ppDevice = nullptr;

    openscarlett::d3d12_xs::DeviceX* dev = AcquireActiveDevice();
    if (!dev) {
        RecordExport("D3D12XboxGetDevice", DXGI_ERROR_NOT_FOUND);
        return DXGI_ERROR_NOT_FOUND;
    }

    const HRESULT hr = dev->QueryInterface(riid, ppDevice);
    dev->Release();
    RecordExport("D3D12XboxGetDevice", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxSetFrameInterval(
    IDXGIOutput* pOutput,
    uint32_t PeriodMicroseconds,
    uint32_t MaxFrameLatency,
    uint32_t Flags) {
    if (MaxFrameLatency > 16u) {
        RecordExport("D3D12XboxSetFrameInterval", E_INVALIDARG);
        return E_INVALIDARG;
    }

    const uint32_t effective_latency = (MaxFrameLatency == 0u) ? 1u : MaxFrameLatency;
    {
        auto& store = GetFrameIntervalStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        store.state.PeriodMicroseconds = PeriodMicroseconds;
        store.state.MaxFrameLatency = effective_latency;
        store.state.Flags = Flags;
        store.state.UpdateCount += 1ull;
    }

    openscarlett::d3d12_xs::DeviceX* dev = AcquireActiveDevice();
    HRESULT hr = S_OK;
    if (dev) {
        hr = dev->SetFrameIntervalX(pOutput, PeriodMicroseconds, effective_latency, Flags);
        dev->Release();
    }

    RecordExport("D3D12XboxSetFrameInterval", hr);
    return hr;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxReportError(
    HRESULT ErrorCode,
    const char* Message) {
    const char* safe_msg = Message ? Message : "";
    {
        auto& store = GetReportedErrorStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        store.count += 1ull;
        store.last_hr = ErrorCode;
        store.last_message.assign(safe_msg, std::min<size_t>(std::strlen(safe_msg), 256u));
    }

    char diag[384]{};
    std::snprintf(
        diag,
        sizeof(diag),
        "[OpenScarlett][d3d12_xs] D3D12XboxReportError hr=0x%08lX msg=%.256s\n",
        static_cast<unsigned long>(ErrorCode),
        safe_msg);
    OutputDebugStringA(diag);

    RecordExport("D3D12XboxReportError", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxSetProcessDebugFlags(
    D3D12XBOX_PROCESS_DEBUG_FLAGS Flags) {
    static std::atomic<uint32_t> s_process_debug_flags{D3D12XBOX_PROCESS_DEBUG_FLAG_NONE};
    s_process_debug_flags.store(static_cast<uint32_t>(Flags), std::memory_order_release);
    RecordExport("D3D12XboxSetProcessDebugFlags", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxSetGpuTimeLimit(
    uint32_t TimeLimitMicroseconds) {
    g_gpu_time_limit_us.store(TimeLimitMicroseconds, std::memory_order_release);
    RecordExport("D3D12XboxSetGpuTimeLimit", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxGetDisplayMode(
    D3D12XBOX_DISPLAY_MODE* pOutMode) {
    if (!pOutMode) {
        RecordExport("D3D12XboxGetDisplayMode", E_POINTER);
        return E_POINTER;
    }
    const int cx = GetSystemMetrics(SM_CXSCREEN);
    const int cy = GetSystemMetrics(SM_CYSCREEN);
    uint32_t period_us = D3D12XBOX_FRAME_INTERVAL_60_HZ;
    {
        auto& store = GetFrameIntervalStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        if (store.state.PeriodMicroseconds > 0u) {
            period_us = store.state.PeriodMicroseconds;
        }
    }
    const uint32_t hz = (period_us > 0u)
                            ? std::clamp<uint32_t>(
                                  static_cast<uint32_t>((1000000ull + period_us / 2ull) / period_us),
                                  24u,
                                  240u)
                            : 60u;

    pOutMode->Width = (cx > 0) ? static_cast<uint32_t>(cx) : 1920u;
    pOutMode->Height = (cy > 0) ? static_cast<uint32_t>(cy) : 1080u;
    {
        char rbuf[64]{};
        const DWORD rlen = ::GetEnvironmentVariableA("OPENSCARLETT_DISPLAY_RES", rbuf, sizeof(rbuf));
        if (rlen > 0u && rlen < sizeof(rbuf)) {
            char* sep = nullptr;
            const unsigned long ew = std::strtoul(rbuf, &sep, 10);
            if (sep && (*sep == 'x' || *sep == 'X')) {
                const unsigned long eh = std::strtoul(sep + 1, nullptr, 10);
                if (ew >= 320ul && eh >= 180ul && ew <= 7680ul && eh <= 4320ul) {
                    pOutMode->Width = static_cast<uint32_t>(ew);
                    pOutMode->Height = static_cast<uint32_t>(eh);
                }
            }
        }
    }
    pOutMode->RefreshRateNumerator = hz;
    pOutMode->RefreshRateDenominator = 1u;
    pOutMode->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pOutMode->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    RecordExport("D3D12XboxGetDisplayMode", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetFrameIntervalState(
    D3D12XBOX_FRAME_INTERVAL_STATE* out_state) {
    if (!out_state) return E_POINTER;
    auto& store = GetFrameIntervalStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    *out_state = store.state;
    return S_OK;
}

OPENSCARLETT_D3D12_API uint32_t WINAPI OpenScarlettD3D12GetGpuTimeLimit() {
    return g_gpu_time_limit_us.load(std::memory_order_acquire);
}

OPENSCARLETT_D3D12_API uint64_t WINAPI OpenScarlettD3D12GetReportedErrorCount(
    HRESULT* out_last_hr) {
    auto& store = GetReportedErrorStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    if (out_last_hr) {
        *out_last_hr = store.last_hr;
    }
    return store.count;
}

OPENSCARLETT_D3D12_API void WINAPI OpenScarlettD3D12ResetTrackedDevice() {
    StoreActiveDevice(nullptr);
}

}  // extern "C"

namespace {

struct RegisteredVLineEvent {
    UINT vline = 0u;
    HANDLE dup_event = nullptr;
};

struct DisplayNotificationRegistry {
    std::mutex mutex;
    std::unordered_map<UINT, RegisteredVLineEvent> vline_events;
    std::unordered_map<UINT, HANDLE> frame_events;
    uint64_t total_vline_signaled = 0ull;
    uint64_t total_frame_signaled = 0ull;

    ~DisplayNotificationRegistry() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& kv : vline_events) {
            if (kv.second.dup_event) CloseHandle(kv.second.dup_event);
        }
        vline_events.clear();
        for (auto& kv : frame_events) {
            if (kv.second) CloseHandle(kv.second);
        }
        frame_events.clear();
    }
};

DisplayNotificationRegistry& GetDisplayNotificationRegistry() {
    static DisplayNotificationRegistry s_reg;
    return s_reg;
}

bool DuplicateEventHandleForSignaling(HANDLE src_event, HANDLE& out_dup) noexcept {
    out_dup = nullptr;
    if (!src_event || src_event == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE cur_proc = GetCurrentProcess();
    if (DuplicateHandle(cur_proc, src_event, cur_proc, &out_dup, EVENT_MODIFY_STATE, FALSE, 0) != FALSE &&
        out_dup != nullptr) {
        return true;
    }
    if (DuplicateHandle(cur_proc, src_event, cur_proc, &out_dup, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE &&
        out_dup != nullptr) {
        return true;
    }
    return false;
}

}  // namespace

extern "C" {

OPENSCARLETT_D3D12_API HRESULT WINAPI DXGIXSetVLineNotification(
    UINT VLineCounter,
    UINT VLine,
    HANDLE hEvent) {
    if (hEvent == INVALID_HANDLE_VALUE) {
        RecordExport("DXGIXSetVLineNotification", E_INVALIDARG);
        return E_INVALIDARG;
    }
    auto& reg = GetDisplayNotificationRegistry();
    if (hEvent == nullptr) {
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.vline_events.find(VLineCounter);
        if (it != reg.vline_events.end()) {
            if (it->second.dup_event) CloseHandle(it->second.dup_event);
            reg.vline_events.erase(it);
        }
        RecordExport("DXGIXSetVLineNotification", S_OK);
        return S_OK;
    }

    HANDLE dup = nullptr;
    if (!DuplicateEventHandleForSignaling(hEvent, dup)) {
        RecordExport("DXGIXSetVLineNotification", E_INVALIDARG);
        return E_INVALIDARG;
    }

    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.vline_events.find(VLineCounter);
        if (it != reg.vline_events.end()) {
            if (it->second.dup_event) CloseHandle(it->second.dup_event);
            it->second.vline = VLine;
            it->second.dup_event = dup;
        } else {
            reg.vline_events.emplace(VLineCounter, RegisteredVLineEvent{VLine, dup});
        }
    }
    RecordExport("DXGIXSetVLineNotification", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI DXGIXSetFrameNotification(
    UINT FrameCounter,
    HANDLE hEvent) {
    if (hEvent == INVALID_HANDLE_VALUE) {
        RecordExport("DXGIXSetFrameNotification", E_INVALIDARG);
        return E_INVALIDARG;
    }
    auto& reg = GetDisplayNotificationRegistry();
    if (hEvent == nullptr) {
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.frame_events.find(FrameCounter);
        if (it != reg.frame_events.end()) {
            if (it->second) CloseHandle(it->second);
            reg.frame_events.erase(it);
        }
        RecordExport("DXGIXSetFrameNotification", S_OK);
        return S_OK;
    }

    HANDLE dup = nullptr;
    if (!DuplicateEventHandleForSignaling(hEvent, dup)) {
        RecordExport("DXGIXSetFrameNotification", E_INVALIDARG);
        return E_INVALIDARG;
    }

    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.frame_events.find(FrameCounter);
        if (it != reg.frame_events.end()) {
            if (it->second) CloseHandle(it->second);
            it->second = dup;
        } else {
            reg.frame_events.emplace(FrameCounter, dup);
        }
    }
    RecordExport("DXGIXSetFrameNotification", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI OpenScarlettD3D12GetGpuHardwareConfigurationX(
    ID3D12Device* device,
    D3D12XBOX_GPU_HARDWARE_CONFIGURATION* out_config) {
    if (!out_config) {
        RecordExport("OpenScarlettD3D12GetGpuHardwareConfigurationX", E_POINTER);
        return E_POINTER;
    }
    std::memset(out_config, 0, sizeof(*out_config));
    if (!device) {
        RecordExport("OpenScarlettD3D12GetGpuHardwareConfigurationX", E_INVALIDARG);
        return E_INVALIDARG;
    }
    ID3D12DeviceX* dev_x = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_ID3D12DeviceX, reinterpret_cast<void**>(&dev_x))) && dev_x) {
        const HRESULT hr = dev_x->GetGpuHardwareConfigurationX(out_config);
        dev_x->Release();
        RecordExport("OpenScarlettD3D12GetGpuHardwareConfigurationX", hr);
        return hr;
    }
    // Fallback for raw desktop ID3D12Device*: synthesize sanitized IHV configuration directly.
    openscarlett::D3D12AdapterInfo raw_info{};
    (void)openscarlett::QueryD3D12DeviceAdapterInfo(device, nullptr, raw_info);
    const openscarlett::SanitizedIhvCapabilities caps =
        openscarlett::SanitizeCrossIhvCapabilities(raw_info);
    const auto profile = openscarlett::SelectXboxConsoleMemoryProfileFromHostVram(
        raw_info.dedicated_video_memory_bytes, nullptr);
    out_config->StructSize = static_cast<uint32_t>(sizeof(*out_config));
    out_config->ConsoleProfile = profile.profile_id;
    out_config->ComputeUnitCount = (profile.profile_id == 1u) ? 52u : 20u;
    out_config->ShaderEngineCount = (profile.profile_id == 1u) ? 4u : 2u;
    out_config->GpuClockMhz = (profile.profile_id == 1u) ? 1825u : 1565u;
    out_config->HostVendorClass = static_cast<uint32_t>(caps.vendor_class);
    out_config->HostVendorId = caps.vendor_id;
    out_config->HostDeviceId = caps.device_id;
    out_config->HostWaveLaneMin = caps.wave_lane_min;
    out_config->HostWaveLaneMax = caps.wave_lane_max;
    out_config->PreferredWaveSize = caps.preferred_wave_size;
    out_config->HostRaytracingTier = caps.raytracing_tier;
    out_config->HostMeshShaderTier = caps.mesh_shader_tier;
    out_config->HostHighestShaderModel = caps.highest_shader_model;
    out_config->IsWarp = caps.is_warp ? 1u : 0u;
    out_config->IsUmaLowVram = caps.is_uma_low_vram ? 1u : 0u;
    out_config->RequiresWave64Emulation = caps.requires_wave64_emulation ? 1u : 0u;
    out_config->SupportsPageIsolatedDescriptorHeaps = 1u;
    out_config->SupportsUnboundedSamplerHeaps = 1u;
    out_config->SupportsDedicatedCopyEngine = 1u;
    out_config->ConsoleVramBudgetBytes = profile.target_console_vram_bytes;
    out_config->HostDedicatedVramBytes = caps.dedicated_video_memory_bytes;
    out_config->EffectiveVramBudgetBytes = caps.effective_vram_budget_bytes;
    const size_t n = std::min(caps.description.size(), sizeof(out_config->AdapterName) - 1u);
    if (n > 0u) std::memcpy(out_config->AdapterName, caps.description.data(), n);
    out_config->AdapterName[n] = '\0';
    RecordExport("OpenScarlettD3D12GetGpuHardwareConfigurationX", S_OK);
    return S_OK;
}

OPENSCARLETT_D3D12_API HRESULT WINAPI D3D12XboxGetGpuHardwareConfiguration(
    ID3D12Device* device,
    D3D12XBOX_GPU_HARDWARE_CONFIGURATION* out_config) {
    const HRESULT hr = OpenScarlettD3D12GetGpuHardwareConfigurationX(device, out_config);
    RecordExport("D3D12XboxGetGpuHardwareConfiguration", hr);
    return hr;
}

}  // extern "C"

namespace openscarlett::d3d12_xs {

void SignalRegisteredDisplayNotificationsOnPresent(
    uint64_t* out_vline_signaled,
    uint64_t* out_frame_signaled) noexcept {
    auto& reg = GetDisplayNotificationRegistry();
    uint64_t vline_sig = 0ull;
    uint64_t frame_sig = 0ull;
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        for (const auto& kv : reg.vline_events) {
            if (kv.second.dup_event && SetEvent(kv.second.dup_event) != FALSE) {
                ++vline_sig;
            }
        }
        for (const auto& kv : reg.frame_events) {
            if (kv.second && SetEvent(kv.second) != FALSE) {
                ++frame_sig;
            }
        }
        reg.total_vline_signaled += vline_sig;
        reg.total_frame_signaled += frame_sig;
    }
    if (out_vline_signaled) *out_vline_signaled = vline_sig;
    if (out_frame_signaled) *out_frame_signaled = frame_sig;
}

void GetDisplayNotificationCounts(
    uint64_t* out_vline_signaled,
    uint64_t* out_frame_signaled) noexcept {
    auto& reg = GetDisplayNotificationRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    if (out_vline_signaled) *out_vline_signaled = reg.total_vline_signaled;
    if (out_frame_signaled) *out_frame_signaled = reg.total_frame_signaled;
}

}  // namespace openscarlett::d3d12_xs
