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
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

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

    static void* const* GetPatchedVtbl(const void* const* raw_vtbl) {
        static void* s_vtbl[kVtblSlotCount]{};
        static std::once_flag s_once;
        std::call_once(s_once, [raw_vtbl]() {
            std::memcpy(s_vtbl, raw_vtbl, sizeof(s_vtbl));
            s_vtbl[18] = reinterpret_cast<void*>(&Thunk_GetDesc);
        });
        return s_vtbl;
    }

    static D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE Thunk_GetDesc(void* arg0, void* arg1) {
        ScarlettRetailCommandQueueProxy* self = nullptr;
        D3D12_COMMAND_QUEUE_DESC* out = nullptr;
        if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
            *reinterpret_cast<void* const* const*>(arg0) == s_patched_vtbl_ptr_) {
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

    explicit ScarlettRetailCommandQueueProxy(ID3D12CommandQueueX* inner)
        : inner_(inner) {
        if (inner_) {
            inner_->AddRef();
        }
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        void* const* patched = GetPatchedVtbl(raw_vtbl);
        s_patched_vtbl_ptr_ = patched;
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

    static bool IsRetailTraceEnabled() noexcept {
        static const bool enabled = []() {
            char v[16]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
            return n > 0u && v[0] != '0';
        }();
        return enabled;
    }

    HRESULT RecordUnimpl(int slot) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailCommandQueue::Slot_%d", slot);
        if (IsRetailTraceEnabled()) {
            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X)\n", buf, slot * 8);
            std::fflush(stderr);
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
    // Slot 30 (+0xF0): PresentX
    virtual HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) {
        return inner_->PresentX(PlaneCount, pPlaneParameters, pPresentParameters);
    }
    virtual HRESULT STDMETHODCALLTYPE Slot31() { return RecordUnimpl(31); }
    virtual HRESULT STDMETHODCALLTYPE Slot32() { return RecordUnimpl(32); }
    virtual HRESULT STDMETHODCALLTYPE Slot33() { return RecordUnimpl(33); }
    virtual HRESULT STDMETHODCALLTYPE Slot34() { return RecordUnimpl(34); }
    virtual HRESULT STDMETHODCALLTYPE Slot35() { return RecordUnimpl(35); }
    virtual HRESULT STDMETHODCALLTYPE Slot36() { return RecordUnimpl(36); }
    virtual HRESULT STDMETHODCALLTYPE Slot37() { return RecordUnimpl(37); }
    virtual HRESULT STDMETHODCALLTYPE Slot38() { return RecordUnimpl(38); }
    virtual HRESULT STDMETHODCALLTYPE Slot39() { return RecordUnimpl(39); }

private:
    static inline void* const* s_patched_vtbl_ptr_ = nullptr;
    std::atomic<ULONG> ref_count_{1u};
    ID3D12CommandQueueX* inner_ = nullptr;
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
    virtual void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) { inner_->SetDescriptorHeaps(n, h); }
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

    // Safe telemetry stubs for any extended retail Scarlett slots 60..139
    static bool IsRetailTraceEnabled() noexcept {
        static const bool enabled = []() {
            char v[16]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
            return n > 0u && v[0] != '0';
        }();
        return enabled;
    }

    void RecordClUnimpl(int slot) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailCmdList::Slot_%d", slot);
        if (IsRetailTraceEnabled()) {
            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X)\n", buf, slot * 8);
            std::fflush(stderr);
        }
        if (inner_) {
            inner_->RecordUnsupportedExtensionIgnored(buf, slot);
        } else {
            openscarlett::CallTelemetry::Global().Record(
                "d3d12_xs.dll", buf, openscarlett::CallOutcome::NotImplemented, E_NOTIMPL, slot);
        }
    }
    virtual void STDMETHODCALLTYPE Slot60() { RecordClUnimpl(60); }
    virtual void STDMETHODCALLTYPE Slot61() { RecordClUnimpl(61); }
    virtual void STDMETHODCALLTYPE Slot62() { RecordClUnimpl(62); }
    virtual void STDMETHODCALLTYPE Slot63() { RecordClUnimpl(63); }
    virtual void STDMETHODCALLTYPE Slot64() { RecordClUnimpl(64); }
    virtual void STDMETHODCALLTYPE Slot65() { RecordClUnimpl(65); }
    virtual void STDMETHODCALLTYPE Slot66() { RecordClUnimpl(66); }
    virtual void STDMETHODCALLTYPE Slot67() { RecordClUnimpl(67); }
    virtual void STDMETHODCALLTYPE Slot68() { RecordClUnimpl(68); }
    virtual void STDMETHODCALLTYPE Slot69() { RecordClUnimpl(69); }
    virtual void STDMETHODCALLTYPE Slot70() { RecordClUnimpl(70); }
    virtual void STDMETHODCALLTYPE Slot71() { RecordClUnimpl(71); }
    virtual void STDMETHODCALLTYPE Slot72() { RecordClUnimpl(72); }
    virtual void STDMETHODCALLTYPE Slot73() { RecordClUnimpl(73); }
    virtual void STDMETHODCALLTYPE Slot74() { RecordClUnimpl(74); }
    virtual void STDMETHODCALLTYPE Slot75() { RecordClUnimpl(75); }
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
    virtual void STDMETHODCALLTYPE Slot126() { RecordClUnimpl(126); }
    virtual void STDMETHODCALLTYPE Slot127() { RecordClUnimpl(127); }
    virtual void STDMETHODCALLTYPE Slot128() { RecordClUnimpl(128); }
    virtual void STDMETHODCALLTYPE Slot129() { RecordClUnimpl(129); }
    virtual void STDMETHODCALLTYPE Slot130() { RecordClUnimpl(130); }
    // Slot 131 (+0x418): Xbox ID3D12GraphicsCommandList6::BuildRaytracingAccelerationStructuresX
    virtual void STDMETHODCALLTYPE BuildRaytracingAccelerationStructuresX(
        UINT /*NumDescs*/, const void* /*pDescs*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailCmdList::BuildRaytracingAccelerationStructuresX",
            openscarlett::CallOutcome::Ok, S_OK, 131);
    }
    virtual void STDMETHODCALLTYPE Slot132() { RecordClUnimpl(132); }
    virtual void STDMETHODCALLTYPE Slot133() { RecordClUnimpl(133); }
    virtual void STDMETHODCALLTYPE Slot134() { RecordClUnimpl(134); }
    virtual void STDMETHODCALLTYPE Slot135() { RecordClUnimpl(135); }
    virtual void STDMETHODCALLTYPE Slot136() { RecordClUnimpl(136); }
    virtual void STDMETHODCALLTYPE Slot137() { RecordClUnimpl(137); }
    virtual void STDMETHODCALLTYPE Slot138() { RecordClUnimpl(138); }
    virtual void STDMETHODCALLTYPE Slot139() { RecordClUnimpl(139); }

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
    static constexpr size_t kVtblSlotCount = 128u;

    static void* const* GetPatchedVtbl(const void* const* raw_vtbl) {
        static void* s_vtbl[kVtblSlotCount]{};
        static std::once_flag s_once;
        std::call_once(s_once, [raw_vtbl]() {
            std::memcpy(s_vtbl, raw_vtbl, sizeof(s_vtbl));
            s_vtbl[25] = reinterpret_cast<void*>(&Thunk_GetResourceAllocationInfo);
            s_vtbl[26] = reinterpret_cast<void*>(&Thunk_GetCustomHeapProperties);
            s_vtbl[43] = reinterpret_cast<void*>(&Thunk_GetAdapterLuid);
            s_vtbl[95] = reinterpret_cast<void*>(&Thunk_GetResourceAllocationInfo1);
        });
        return s_vtbl;
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
        if (self && self->inner_) {
            res = self->inner_->GetResourceAllocationInfo1(
                visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
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
        if (self && self->inner_) {
            res = self->inner_->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs);
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

    explicit ScarlettRetailDeviceProxy(openscarlett::d3d12_xs::DeviceX* inner)
        : inner_(inner) {
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
        // Xbox Series X ID3D12Device8 IID (verified live against UE5 XSXD3D12Device.cpp DeserializeStateObjectX).
        static constexpr GUID kXboxIidD3D12Device8 = {
            0x728F1FA2u, 0xEA11u, 0x4A9Fu, {0x9Bu, 0x52u, 0x9Bu, 0x42u, 0xA4u, 0x84u, 0xD5u, 0xC9u}};
        static constexpr GUID kPcIidD3D12Device8 = {
            0x9218E6BBu, 0xF944u, 0x4F7Eu, {0xA7u, 0x5Cu, 0xB1u, 0xB2u, 0xC7u, 0xB7u, 0x01u, 0xF3u}};
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
            IsEqualIID(riid, kPcIidD3D12Device8)) {
            *ppvObject = static_cast<ID3D12Device*>(this);
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
            auto* retail_q = new (std::nothrow) ScarlettRetailCommandQueueProxy(qx);
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
        if (clx &&
            (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList)) ||
             IsEqualIID(riid, __uuidof(ID3D12CommandList)) ||
             IsEqualIID(riid, __uuidof(IUnknown)))) {
            auto* retail_cl = new (std::nothrow) ScarlettRetailCommandListProxy(clx);
            raw_cl->Release();
            if (!retail_cl) return E_OUTOFMEMORY;
            *ppCommandList = static_cast<void*>(retail_cl);
            return S_OK;
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

    // Retail Scarlett d3d12_xs.h ID3D12Device extended vtable (slots 44..127)
    static bool IsRetailTraceEnabled() noexcept {
        static const bool enabled = []() {
            char v[16]{};
            const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", v, static_cast<DWORD>(sizeof(v)));
            return n > 0u && v[0] != '0';
        }();
        return enabled;
    }

    HRESULT RecordUnimpl(int slot) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ScarlettRetailDevice::Slot_%d", slot);
        if (IsRetailTraceEnabled()) {
            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] UNIMPL %s (+0x%X)\n", buf, slot * 8);
            std::fflush(stderr);
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
    virtual HRESULT STDMETHODCALLTYPE Slot55() { return RecordUnimpl(55); }
    virtual HRESULT STDMETHODCALLTYPE Slot56() { return RecordUnimpl(56); }
    virtual HRESULT STDMETHODCALLTYPE Slot57() { return RecordUnimpl(57); }
    virtual HRESULT STDMETHODCALLTYPE Slot58() { return RecordUnimpl(58); }
    virtual HRESULT STDMETHODCALLTYPE Slot59() { return RecordUnimpl(59); }
    virtual HRESULT STDMETHODCALLTYPE Slot60() { return RecordUnimpl(60); }
    virtual HRESULT STDMETHODCALLTYPE Slot61() { return RecordUnimpl(61); }
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
    virtual HRESULT STDMETHODCALLTYPE Slot67() { return RecordUnimpl(67); }
    virtual HRESULT STDMETHODCALLTYPE Slot68() { return RecordUnimpl(68); }
    // Slot 69 (+0x228): SetDriverFlagX(uint32_t key, uint32_t value)
    virtual HRESULT STDMETHODCALLTYPE SetDriverFlagX(uint32_t /*key*/, uint32_t /*value*/) {
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::SetDriverFlagX",
            openscarlett::CallOutcome::Ok, S_OK, 69);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot70() { return RecordUnimpl(70); }
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
        UINT node_mask = 0u;
        if (pDesc) {
            const auto* words = static_cast<const uint32_t*>(pDesc);
            type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
            node_mask = words[1];
        }
        const HRESULT hr = WrapRetailCommandList(
            node_mask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
        openscarlett::CallTelemetry::Global().Record(
            "d3d12_xs.dll", "ScarlettRetailDevice::CreateCommandListX",
            SUCCEEDED(hr) ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error, hr, 73);
        return hr;
    }
    virtual HRESULT STDMETHODCALLTYPE Slot74() { return RecordUnimpl(74); }
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
        if (clx &&
            (IsEqualIID(riid, __uuidof(ID3D12GraphicsCommandList)) ||
             IsEqualIID(riid, __uuidof(ID3D12CommandList)) ||
             IsEqualIID(riid, __uuidof(IUnknown)))) {
            auto* retail_cl = new (std::nothrow) ScarlettRetailCommandListProxy(clx);
            raw_cl->Release();
            if (!retail_cl) return E_OUTOFMEMORY;
            *ppCommandList = static_cast<void*>(retail_cl);
            return S_OK;
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
    virtual HRESULT STDMETHODCALLTYPE Slot108() { return RecordUnimpl(108); }
    virtual HRESULT STDMETHODCALLTYPE Slot109() { return RecordUnimpl(109); }
    virtual HRESULT STDMETHODCALLTYPE Slot110() { return RecordUnimpl(110); }
    virtual HRESULT STDMETHODCALLTYPE Slot111() { return RecordUnimpl(111); }
    virtual HRESULT STDMETHODCALLTYPE Slot112() { return RecordUnimpl(112); }
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
    virtual HRESULT STDMETHODCALLTYPE Slot127() { return RecordUnimpl(127); }

private:
    static inline void* const* s_patched_vtbl_ptr_ = nullptr;
    std::atomic<ULONG> ref_count_{1u};
    openscarlett::d3d12_xs::DeviceX* inner_ = nullptr;
};

HRESULT CreateWrappedDeviceInternal(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL min_level,
    bool prefer_warp,
    bool enable_debug_layer,
    HWND default_hwnd,
    REFIID riid,
    void** ppDevice,
    bool retail_scarlett_vtable = false) {
    // Opt-in D3D12 Agility SDK discovery (graceful fallback to inbox d3d12.dll when absent).
    TryConfigureAgilitySdkOnce();

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
    if (retail_scarlett_vtable) {
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
    }
    if (retail_scarlett_vtable &&
        (IsEqualIID(riid, __uuidof(ID3D12Device)) || IsEqualIID(riid, __uuidof(IUnknown)))) {
        auto* retail = new (std::nothrow) ScarlettRetailDeviceProxy(dev_x);
        if (!retail) {
            dev_x->Release();
            return E_OUTOFMEMORY;
        }
        StoreActiveDevice(dev_x);
        dev_x->Release();
        *ppDevice = static_cast<ID3D12Device*>(retail);
        return S_OK;
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

    const HRESULT hr = CreateWrappedDeviceInternal(
        pAdapter,
        D3D_FEATURE_LEVEL_12_0,
        global_warp,
        debug_layer,
        nullptr,
        riid,
        ppDevice,
        retail_scarlett);
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
