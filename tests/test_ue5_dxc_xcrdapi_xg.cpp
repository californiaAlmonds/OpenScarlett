#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <objidl.h>
#  include <dxgiformat.h>
#endif

#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/dstorage_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)

fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0u && len < std::size(buf)) {
        return fs::path(buf).parent_path();
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

void FillPattern(std::vector<uint8_t>& buf, uint32_t seed) {
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < buf.size(); ++i) {
        s = s * 1664525u + 1013904223u;
        buf[i] = static_cast<uint8_t>((s >> 24u) ^ ((i * 29u + seed) & 0xFFu));
    }
}

// ---------------------------------------------------------------------------
// Minimal self-contained DXC COM definitions (matches Windows SDK dxcapi.h)
// ---------------------------------------------------------------------------
constexpr CLSID kClsidDxcUtils = {
    0x6245d6afu, 0x66e0u, 0x48fdu, {0x80u, 0xb4u, 0x4du, 0x27u, 0x17u, 0x96u, 0x74u, 0x8cu}};
constexpr IID kIidIDxcUtils = {
    0x4605c4cbu, 0x2019u, 0x492au, {0xadu, 0xa4u, 0x65u, 0xf2u, 0x0bu, 0xb7u, 0xd6u, 0x7fu}};
constexpr CLSID kClsidDxcCompiler = {
    0x73e22d93u, 0xe6ceu, 0x47f3u, {0xb5u, 0xbfu, 0xf0u, 0x66u, 0x4fu, 0x39u, 0xc1u, 0xb0u}};
constexpr IID kIidIDxcCompiler3 = {
    0x228b4687u, 0x5a6au, 0x4730u, {0x90u, 0x0cu, 0x97u, 0x02u, 0xb2u, 0x20u, 0x3fu, 0x54u}};
constexpr IID kIidIDxcLibrary = {
    0xe5204dc7u, 0xd18cu, 0x4c3cu, {0xbdu, 0xfbu, 0x85u, 0x16u, 0x73u, 0x98u, 0x0fu, 0xe7u}};

// ---------------------------------------------------------------------------
// 1. Live dxcompiler_xs.dll verification
// ---------------------------------------------------------------------------
void TestDxcompilerXsLive() {
    std::printf("[Test 1] dxcompiler_xs.dll live LoadLibraryW + DxcCreateInstance / DxcCreateInstance2...\n");

    const fs::path dll_path = GetExeDir() / L"dxcompiler_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnDxcCreateInstance = HRESULT(__stdcall*)(REFCLSID, REFIID, LPVOID*);
    using PfnDxcCreateInstance2 = HRESULT(__stdcall*)(IMalloc*, REFCLSID, REFIID, LPVOID*);
    using PfnDxcTelDetail = int(__stdcall*)(const char*, uint64_t*, int32_t*);

    auto fn_create =
        reinterpret_cast<PfnDxcCreateInstance>(GetProcAddress(mod, "DxcCreateInstance"));
    auto fn_create2 =
        reinterpret_cast<PfnDxcCreateInstance2>(GetProcAddress(mod, "DxcCreateInstance2"));
    auto fn_tel =
        reinterpret_cast<PfnDxcTelDetail>(GetProcAddress(mod, "OpenScarlettDxcGetTelemetryDetail"));
    OS_REQUIRE(fn_create != nullptr);
    OS_REQUIRE(fn_create2 != nullptr);
    OS_REQUIRE(fn_tel != nullptr);

    // Null output pointer must fail cleanly with E_POINTER.
    OS_REQUIRE(fn_create(kClsidDxcUtils, kIidIDxcUtils, nullptr) == E_POINTER);
    OS_REQUIRE(fn_create2(nullptr, kClsidDxcUtils, kIidIDxcUtils, nullptr) == E_POINTER);

    // Create real IDxcUtils via DxcCreateInstance (or fallback IDxcLibrary if older host DXC).
    IUnknown* utils = nullptr;
    HRESULT hr_utils = fn_create(kClsidDxcUtils, kIidIDxcUtils, reinterpret_cast<void**>(&utils));
    if (FAILED(hr_utils)) {
        hr_utils = fn_create(kClsidDxcUtils, kIidIDxcLibrary, reinterpret_cast<void**>(&utils));
    }
    OS_REQUIRE(SUCCEEDED(hr_utils) && utils != nullptr);
    utils->AddRef();
    OS_REQUIRE(utils->Release() >= 1u);
    utils->Release();

    // Create real IDxcCompiler3 via DxcCreateInstance.
    IUnknown* compiler3 = nullptr;
    const HRESULT hr_comp =
        fn_create(kClsidDxcCompiler, kIidIDxcCompiler3, reinterpret_cast<void**>(&compiler3));
    OS_REQUIRE(SUCCEEDED(hr_comp) && compiler3 != nullptr);
    compiler3->Release();

    // Also verify DxcCreateInstance2 with default allocator (nullptr) creates IDxcUtils/IDxcCompiler3.
    IUnknown* comp_v2 = nullptr;
    const HRESULT hr_v2 = fn_create2(
        nullptr, kClsidDxcCompiler, kIidIDxcCompiler3, reinterpret_cast<void**>(&comp_v2));
    OS_REQUIRE(SUCCEEDED(hr_v2) && comp_v2 != nullptr);
    comp_v2->Release();

    uint64_t calls1 = 0u;
    int32_t last_hr1 = -1;
    OS_REQUIRE(fn_tel("DxcCreateInstance", &calls1, &last_hr1) == 1);
    OS_REQUIRE(calls1 >= 3u && last_hr1 == 0);

    uint64_t calls2 = 0u;
    int32_t last_hr2 = -1;
    OS_REQUIRE(fn_tel("DxcCreateInstance2", &calls2, &last_hr2) == 1);
    OS_REQUIRE(calls2 >= 2u && last_hr2 == 0);

    FreeLibrary(mod);
    std::printf("  PASS: dxcompiler_xs.dll created live IDxcUtils + IDxcCompiler3 via DxcCreateInstance/2!\n");
}

// ---------------------------------------------------------------------------
// 2. Live xcrdapi.dll -> DStorageGetFactory / DStorageSetConfiguration*
// ---------------------------------------------------------------------------
void TestXCrdApiDStorageForwarding() {
    std::printf("[Test 2] xcrdapi.dll live DStorageGetFactory / DStorageSetConfiguration* -> dstorage_xs.dll...\n");

    const fs::path dll_path = GetExeDir() / L"xcrdapi.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnDStorageGetFactory = int32_t(__stdcall*)(const DStorageGuid*, void**);
    using PfnDStorageSetConfig = int32_t(__stdcall*)(const DSTORAGE_CONFIGURATION*);
    using PfnDStorageSetConfig1 = int32_t(__stdcall*)(const DSTORAGE_CONFIGURATION1*);
    using PfnCrdTelDetail = int(__stdcall*)(const char*, uint64_t*, uint64_t*, int32_t*);

    auto fn_get_factory =
        reinterpret_cast<PfnDStorageGetFactory>(GetProcAddress(mod, "DStorageGetFactory"));
    auto fn_set_cfg =
        reinterpret_cast<PfnDStorageSetConfig>(GetProcAddress(mod, "DStorageSetConfiguration"));
    auto fn_set_cfg1 =
        reinterpret_cast<PfnDStorageSetConfig1>(GetProcAddress(mod, "DStorageSetConfiguration1"));
    auto fn_tel =
        reinterpret_cast<PfnCrdTelDetail>(GetProcAddress(mod, "OpenScarlettXCrdGetTelemetryDetail"));

    OS_REQUIRE(fn_get_factory != nullptr);
    OS_REQUIRE(fn_set_cfg != nullptr);
    OS_REQUIRE(fn_set_cfg1 != nullptr);
    OS_REQUIRE(fn_tel != nullptr);

    // Test configuration forwarding
    DSTORAGE_CONFIGURATION cfg{};
    cfg.NumSubmitThreads = 2u;
    cfg.NumBuiltInCpuDecompressionThreads = 2;
    OS_REQUIRE(fn_set_cfg(&cfg) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(fn_set_cfg(nullptr) == OS_DSTORAGE_E_INVALIDARG);

    DSTORAGE_CONFIGURATION1 cfg1{};
    cfg1.NumSubmitThreads = 2u;
    cfg1.NumBuiltInCpuDecompressionThreads = 2;
    cfg1.ForceFileBuffering = 1;
    OS_REQUIRE(fn_set_cfg1(&cfg1) == OS_DSTORAGE_S_OK);

    // Null output pointer returns E_POINTER cleanly
    OS_REQUIRE(fn_get_factory(&IID_IDStorageFactoryX, nullptr) == OS_DSTORAGE_E_POINTER);

    // Create real IDStorageFactoryX COM object via xcrdapi.dll -> dstorage_xs.dll
    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(fn_get_factory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) ==
               OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // Exercise the returned IDStorageFactoryX to verify it is a genuine live DirectStorage factory!
    DSTORAGE_QUEUE_DESC qdesc{};
    qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qdesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    qdesc.Name = "xcrdapi_forwarded_queue";

    IDStorageQueueX* queue = nullptr;
    OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&queue)) ==
               OS_DSTORAGE_S_OK);
    OS_REQUIRE(queue != nullptr);

    DSTORAGE_QUEUE_INFO qinfo{};
    queue->Query(&qinfo);
    OS_REQUIRE(qinfo.Desc.Capacity == DSTORAGE_MIN_QUEUE_CAPACITY);
    OS_REQUIRE(qinfo.EmptySlotCount == DSTORAGE_MIN_QUEUE_CAPACITY);

    queue->Close();
    queue->Release();
    factory->Release();

    // Verify xcrdapi.dll recorded CallTelemetry for DStorageGetFactory
    uint64_t total = 0u;
    uint64_t not_impl = 0u;
    int32_t last_hr = -1;
    OS_REQUIRE(fn_tel("DStorageGetFactory", &total, &not_impl, &last_hr) == 1);
    OS_REQUIRE(total >= 2u && not_impl == 0u && last_hr == OS_DSTORAGE_S_OK);

    FreeLibrary(mod);
    std::printf("  PASS: xcrdapi.dll forwarded DStorageGetFactory + CreateQueue + Query cleanly!\n");
}

// ---------------------------------------------------------------------------
// 3. Live xg_xs.dll -> XGCreateTextureComputer (+ 1D/2D/3D + OptimalTileMode)
// ---------------------------------------------------------------------------
struct TestXgResourceDesc {
    uint32_t Dimension;         // 2=1D, 3=2D, 4=3D
    uint64_t Alignment;
    uint64_t Width;
    uint32_t Height;
    uint16_t DepthOrArraySize;
    uint16_t MipLevels;
    uint32_t Format;
    uint32_t SampleCount;
    uint32_t SampleQuality;
    uint32_t Layout;
    uint32_t Flags;
};
static_assert(sizeof(TestXgResourceDesc) == 56u, "TestXgResourceDesc must match D3D12_RESOURCE_DESC");

struct TestXgTexture1DDesc {
    uint32_t Width;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;
    uint32_t BindFlags;
    XG_TILE_MODE TileMode;
};

struct TestXgPackedMipInfo {
    uint8_t NumStandardMips;
    uint8_t NumPackedMips;
    uint32_t NumTilesForPackedMips;
    uint32_t StartTileIndexInOverallResource;
};

struct TestXgTileShape {
    uint32_t WidthInTexels;
    uint32_t HeightInTexels;
    uint32_t DepthInTexels;
};

struct TestXgSubresourceTiling {
    uint32_t WidthInTiles;
    uint16_t HeightInTiles;
    uint16_t DepthInTiles;
    uint32_t StartTileIndexInOverallResource;
};

struct TestXgMipLevelLayoutXbox {
    uint64_t SizeBytes;
    uint64_t OffsetBytes;
    uint64_t Slice2DSizeBytes;
    uint32_t PitchPixels;
    uint32_t PitchBytes;
    uint32_t AlignmentBytes;
    uint32_t PaddedWidthElements;
    uint32_t PaddedHeightElements;
    uint32_t PaddedDepthOrArraySize;
    uint32_t WidthElements;
    uint32_t HeightElements;
    uint32_t DepthOrArraySize;
    uint32_t SampleCount;
    uint64_t MipTailOffsetBytes;
    uint32_t BankRotation;
    uint32_t PipeSwizzle;
    uint32_t Flags;
    uint32_t ReservedMid[4];
    uint32_t TileMode;
    uint32_t ReservedEnd[2];
};
static_assert(sizeof(TestXgMipLevelLayoutXbox) == 0x70u, "XG_MIPLEVEL_LAYOUT_XBOX must be 0x70 bytes");

struct TestXgPlaneLayoutXbox {
    uint32_t Usage;
    uint32_t Reserved0;
    uint64_t SizeBytes;
    uint64_t Slice2DSizeBytes;
    uint64_t BaseOffsetBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t BytesPerElement;
    uint32_t Reserved1;
    uint32_t MipTailStartLevel;
    uint32_t Reserved2;
    TestXgMipLevelLayoutXbox MipLayout[15];
};
static_assert(sizeof(TestXgPlaneLayoutXbox) == 0x6C8u, "XG_PLANE_LAYOUT_XBOX must be 0x6C8 bytes");

struct TestXgResourceLayoutXbox {
    uint64_t SizeBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t MipLevels;
    uint32_t Planes;
    TestXgPlaneLayoutXbox Plane[4];
    uint32_t Dimension;
    uint32_t Flags;
};
static_assert(sizeof(TestXgResourceLayoutXbox) == 0x1B40u, "XG_RESOURCE_LAYOUT_XBOX must be 0x1B40 bytes");

struct TestXGTextureAddressComputer {
    virtual uint32_t __stdcall AddRef() = 0;
    virtual uint32_t __stdcall Release() = 0;
    virtual int32_t __stdcall GetResourceLayout(TestXgResourceLayoutXbox* pLayout) const = 0;
    virtual uint64_t __stdcall GetResourceSizeBytes() const = 0;
    virtual uint64_t __stdcall GetResourceBaseAlignmentBytes() const = 0;
    virtual uint64_t __stdcall GetMipLevelOffsetBytes(uint32_t plane, uint32_t mipLevel) const = 0;
    virtual uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const = 0;
    virtual int32_t __stdcall GetTexelCoordinate(
        uint64_t resourceOffsetBytes,
        uint32_t plane,
        uint32_t mipLevel,
        uint64_t* pOutX,
        uint32_t* pOutY,
        uint32_t* pOutZOrSlice,
        uint32_t* pOutSample) const = 0;
    virtual int32_t __stdcall CopyIntoSubresource(
        void* pDstTiledResource,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcLinearData,
        uint32_t srcRowPitchBytes,
        uint32_t srcSlicePitchBytes) const = 0;
    virtual int32_t __stdcall CopyFromSubresource(
        void* pDstLinearData,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcTiledResource,
        uint32_t dstRowPitchBytes,
        uint32_t dstSlicePitchBytes) const = 0;
    virtual void __stdcall GetResourceTiling(
        uint32_t* pNumTilesForEntireResource,
        TestXgPackedMipInfo* pPackedMipDesc,
        TestXgTileShape* pStandardTileShapeForNonPackedMips,
        uint32_t* pNumSubresourceTilings,
        uint32_t firstSubresourceTilingToGet,
        TestXgSubresourceTiling* pSubresourceTilingsForNonPackedMips) const = 0;
};

void TestXgCreateTextureComputerLive() {
    std::printf("[Test 3] xg_xs.dll live XGCreateTextureComputer (+ 1D/2D/3D + OptimalTileMode)...\n");

    const fs::path dll_path = GetExeDir() / L"xg_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnCreateTexComp =
        int32_t(__stdcall*)(const TestXgResourceDesc*, TestXGTextureAddressComputer**);
    using PfnCreateTex1D =
        int32_t(__stdcall*)(const TestXgTexture1DDesc*, IXGTextureAddressComputer**);
    using PfnCreateTex2D =
        int32_t(__stdcall*)(const XG_TEXTURE2D_DESC*, IXGTextureAddressComputer**);
    using PfnCreateTex3D =
        int32_t(__stdcall*)(const XG_TEXTURE3D_DESC*, IXGTextureAddressComputer**);
    using PfnOptimalMode = XG_TILE_MODE(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t);

    auto fn_create =
        reinterpret_cast<PfnCreateTexComp>(GetProcAddress(mod, "XGCreateTextureComputer"));
    auto fn_create_1d =
        reinterpret_cast<PfnCreateTex1D>(GetProcAddress(mod, "XGCreateTexture1DComputer"));
    auto fn_create_2d =
        reinterpret_cast<PfnCreateTex2D>(GetProcAddress(mod, "XGCreateTexture2DComputer"));
    auto fn_create_3d =
        reinterpret_cast<PfnCreateTex3D>(GetProcAddress(mod, "XGCreateTexture3DComputer"));
    auto fn_opt_mode =
        reinterpret_cast<PfnOptimalMode>(GetProcAddress(mod, "XGComputeOptimalTileMode"));

    OS_REQUIRE(fn_create != nullptr);
    OS_REQUIRE(fn_create_1d != nullptr);
    OS_REQUIRE(fn_create_2d != nullptr);
    OS_REQUIRE(fn_create_3d != nullptr);
    OS_REQUIRE(fn_opt_mode != nullptr);

    OS_REQUIRE(fn_opt_mode(DXGI_FORMAT_R8G8B8A8_UNORM, 256u, 256u, XG_BIND_SHADER_RESOURCE) ==
               XG_TILE_MODE_2D_THIN_64KB_S);

    // 3A. 256x256 RGBA8 with 5 mips via XGCreateTextureComputer
    {
        TestXgResourceDesc desc{};
        desc.Dimension = 3u;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
        desc.Width = 256u;
        desc.Height = 256u;
        desc.DepthOrArraySize = 1u;
        desc.MipLevels = 5u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = 1u;
        desc.Layout = 0u;  // D3D12_TEXTURE_LAYOUT_UNKNOWN -> optimal GFX10 swizzle
        desc.Flags = 0u;

        TestXGTextureAddressComputer* comp = nullptr;
        OS_REQUIRE(fn_create(&desc, &comp) == 0 && comp != nullptr);

        // Verify retail Xbox xg_xs.h refcount vtable (AddRef=0, Release=1, GetResourceLayout=2)
        OS_REQUIRE(comp->AddRef() == 2u);
        OS_REQUIRE(comp->Release() == 1u);

        TestXgResourceLayoutXbox layout{};
        OS_REQUIRE(comp->GetResourceLayout(&layout) == 0);
        const uint64_t size_bytes = comp->GetResourceSizeBytes();
        const uint64_t align_bytes = comp->GetResourceBaseAlignmentBytes();
        OS_REQUIRE(size_bytes == layout.SizeBytes && size_bytes >= 256ull * 256ull * 4ull);
        OS_REQUIRE(align_bytes == layout.BaseAlignmentBytes && align_bytes == 65536ull);
        OS_REQUIRE(layout.MipLevels == 5u && layout.Planes == 1u);
        OS_REQUIRE(layout.Plane[0].MipLayout[0].TileMode == 9u);  // SW_64KB_S

        // Also verify real Matrix Awakens Xbox layout=0x105 (SW_4KB_S) 843x5 B8G8R8A8_TYPELESS
        {
            TestXgResourceDesc mdesc{};
            mdesc.Dimension = 3u;
            mdesc.Width = 843u;
            mdesc.Height = 5u;
            mdesc.DepthOrArraySize = 1u;
            mdesc.MipLevels = 1u;
            mdesc.Format = 90u;  // DXGI_FORMAT_B8G8R8A8_TYPELESS
            mdesc.SampleCount = 1u;
            mdesc.Layout = 0x105u;  // Xbox D3D12_TEXTURE_LAYOUT | ADDR_SW_4KB_S
            TestXGTextureAddressComputer* mcomp = nullptr;
            OS_REQUIRE(fn_create(&mdesc, &mcomp) == 0 && mcomp != nullptr);
            TestXgResourceLayoutXbox mlay{};
            OS_REQUIRE(mcomp->GetResourceLayout(&mlay) == 0);
            OS_REQUIRE(mlay.BaseAlignmentBytes == 4096ull);
            OS_REQUIRE(mlay.SizeBytes == 0x1B000ull);  // 27 * 4 KiB tiles
            OS_REQUIRE(mlay.Plane[0].MipLayout[0].TileMode == 5u);
            mcomp->Release();
        }

        // Verify mip offsets & texel offset <-> coordinate inverse round-trip
        for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
            const uint64_t mip_off = comp->GetMipLevelOffsetBytes(0u, m);
            OS_REQUIRE(mip_off != UINT64_MAX && mip_off < size_bytes);
        }
        const uint64_t px_off = comp->GetTexelElementOffsetBytes(0u, 0u, 73u, 149u, 0u, 0u);
        OS_REQUIRE(px_off != UINT64_MAX && px_off < size_bytes);
        uint64_t out_x = 0u;
        uint32_t out_y = 0u, out_z = 0u, out_sample = 99u;
        OS_REQUIRE(comp->GetTexelCoordinate(px_off, 0u, 0u, &out_x, &out_y, &out_z, &out_sample) == 0);
        OS_REQUIRE(out_x == 73u && out_y == 149u && out_z == 0u && out_sample == 0u);

        // Verify GetResourceTiling
        uint32_t total_tiles = 0u;
        TestXgPackedMipInfo packed{};
        TestXgTileShape shape{};
        std::array<TestXgSubresourceTiling, 5> tilings{};
        uint32_t num_tilings = static_cast<uint32_t>(tilings.size());
        comp->GetResourceTiling(&total_tiles, &packed, &shape, &num_tilings, 0u, tilings.data());
        OS_REQUIRE(total_tiles >= 4u);
        OS_REQUIRE(num_tilings == 5u);
        OS_REQUIRE(shape.WidthInTexels == 128u && shape.HeightInTexels == 128u && shape.DepthInTexels == 1u);
        OS_REQUIRE(packed.NumStandardMips + packed.NumPackedMips == 5u);

        // Round-trip CopyIntoSubresource -> CopyFromSubresource across all 5 mips
        std::vector<uint8_t> tiled(static_cast<size_t>(size_bytes), 0xA5u);
        std::vector<std::vector<uint8_t>> srcs(desc.MipLevels);
        for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, static_cast<uint32_t>(desc.Width) >> m);
            const uint32_t mh = std::max(1u, desc.Height >> m);
            const uint32_t rp = mw * 4u;
            srcs[m].resize(static_cast<size_t>(rp) * mh);
            FillPattern(srcs[m], 0x1000u + m * 17u);
            OS_REQUIRE(comp->CopyIntoSubresource(tiled.data(), 0u, m, srcs[m].data(), rp, 0u) == 0);
        }

        // Verify genuine GFX10 swizzle occurred on mip 0
        size_t diffs = 0u;
        for (size_t i = 0u; i < srcs[0].size(); ++i) {
            if (srcs[0][i] != tiled[i]) ++diffs;
        }
        OS_REQUIRE(diffs > srcs[0].size() / 4u);

        for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, static_cast<uint32_t>(desc.Width) >> m);
            const uint32_t rp = mw * 4u;
            std::vector<uint8_t> dst(srcs[m].size(), 0x11u);
            OS_REQUIRE(comp->CopyFromSubresource(dst.data(), 0u, m, tiled.data(), rp, 0u) == 0);
            OS_REQUIRE(std::memcmp(srcs[m].data(), dst.data(), dst.size()) == 0);
        }

        comp->Release();
        std::printf("  2D RGBA8 256x256 (5 mips): size=%llu B, align=%llu B, round-trip BIT-EXACT!\n",
                    static_cast<unsigned long long>(size_bytes),
                    static_cast<unsigned long long>(align_bytes));
    }

    // 3B. 256x256 BC7_UNORM (4 mips) via XGCreateTextureComputer
    {
        TestXgResourceDesc desc{};
        desc.Dimension = 3u;
        desc.Width = 256u;
        desc.Height = 256u;
        desc.DepthOrArraySize = 1u;
        desc.MipLevels = 4u;
        desc.Format = DXGI_FORMAT_BC7_UNORM;
        desc.SampleCount = 1u;
        desc.Layout = 0u;

        TestXGTextureAddressComputer* comp = nullptr;
        OS_REQUIRE(fn_create(&desc, &comp) == 0 && comp != nullptr);

        const uint64_t size_bytes = comp->GetResourceSizeBytes();
        const uint64_t align_bytes = comp->GetResourceBaseAlignmentBytes();
        OS_REQUIRE(size_bytes >= 65536ull && align_bytes == 65536ull);

        const uint64_t blk_off = comp->GetTexelElementOffsetBytes(0u, 0u, 19u, 41u, 0u, 0u);
        OS_REQUIRE(blk_off != UINT64_MAX && blk_off < size_bytes);
        uint64_t bx_out = 0u;
        uint32_t by_out = 0u, bz_out = 0u, bs_out = 99u;
        OS_REQUIRE(comp->GetTexelCoordinate(blk_off, 0u, 0u, &bx_out, &by_out, &bz_out, &bs_out) == 0);
        OS_REQUIRE(bx_out == 19u && by_out == 41u && bz_out == 0u && bs_out == 0u);

        std::vector<uint8_t> tiled(static_cast<size_t>(size_bytes), 0xCCu);
        std::vector<std::vector<uint8_t>> srcs(desc.MipLevels);
        for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, static_cast<uint32_t>(desc.Width) >> m);
            const uint32_t mh = std::max(1u, desc.Height >> m);
            const uint32_t bx = (mw + 3u) / 4u;
            const uint32_t by = (mh + 3u) / 4u;
            const uint32_t rp = bx * 16u;
            srcs[m].resize(static_cast<size_t>(rp) * by);
            FillPattern(srcs[m], 0xBC70u + m * 31u);
            OS_REQUIRE(comp->CopyIntoSubresource(tiled.data(), 0u, m, srcs[m].data(), rp, 0u) == 0);
        }
        for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
            const uint32_t mw = std::max(1u, static_cast<uint32_t>(desc.Width) >> m);
            const uint32_t bx = (mw + 3u) / 4u;
            const uint32_t rp = bx * 16u;
            std::vector<uint8_t> dst(srcs[m].size(), 0x22u);
            OS_REQUIRE(comp->CopyFromSubresource(dst.data(), 0u, m, tiled.data(), rp, 0u) == 0);
            OS_REQUIRE(std::memcmp(srcs[m].data(), dst.data(), dst.size()) == 0);
        }
        comp->Release();
        std::printf("  2D BC7   256x256 (4 mips): size=%llu B, align=%llu B, round-trip BIT-EXACT!\n",
                    static_cast<unsigned long long>(size_bytes),
                    static_cast<unsigned long long>(align_bytes));
    }

    // 3C. 1D texture via XGCreateTexture1DComputer + 3D volume via XGCreateTextureComputer
    {
        TestXgTexture1DDesc d1d{};
        d1d.Width = 512u;
        d1d.MipLevels = 4u;
        d1d.ArraySize = 2u;
        d1d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d1d.BindFlags = XG_BIND_SHADER_RESOURCE;
        d1d.TileMode = XG_TILE_MODE_2D_THIN_4KB_S;

        IXGTextureAddressComputer* comp1d = nullptr;
        OS_REQUIRE(fn_create_1d(&d1d, &comp1d) == 0 && comp1d != nullptr);
        XG_RESOURCE_LAYOUT l1d{};
        OS_REQUIRE(comp1d->GetResourceLayout(&l1d) == 0 && l1d.SizeBytes > 0ull);
        std::vector<uint8_t> tiled1d(static_cast<size_t>(l1d.SizeBytes), 0u);
        std::vector<uint8_t> src1d(d1d.Width * 4u);
        FillPattern(src1d, 0x1D01u);
        std::vector<uint8_t> dst1d(src1d.size(), 0u);
        OS_REQUIRE(comp1d->CopyIntoSubresource(tiled1d.data(), 0u, 0u, src1d.data(), d1d.Width * 4u, 0u) == 0);
        OS_REQUIRE(comp1d->CopyFromSubresource(dst1d.data(), 0u, 0u, tiled1d.data(), d1d.Width * 4u, 0u) == 0);
        OS_REQUIRE(std::memcmp(src1d.data(), dst1d.data(), src1d.size()) == 0);
        comp1d->Release();

        TestXgResourceDesc d3d{};
        d3d.Dimension = 4u;  // D3D12_RESOURCE_DIMENSION_TEXTURE3D
        d3d.Width = 32u;
        d3d.Height = 32u;
        d3d.DepthOrArraySize = 16u;
        d3d.MipLevels = 3u;
        d3d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d3d.SampleCount = 1u;
        d3d.Layout = 0u;

        TestXGTextureAddressComputer* comp3d = nullptr;
        OS_REQUIRE(fn_create(&d3d, &comp3d) == 0 && comp3d != nullptr);
        const uint64_t sz3d = comp3d->GetResourceSizeBytes();
        OS_REQUIRE(sz3d >= 32ull * 32ull * 16ull * 4ull);
        const uint64_t vox_off = comp3d->GetTexelElementOffsetBytes(0u, 0u, 11u, 23u, 9u, 0u);
        OS_REQUIRE(vox_off != UINT64_MAX);
        uint64_t vx = 0u;
        uint32_t vy = 0u, vz = 0u, vs = 99u;
        OS_REQUIRE(comp3d->GetTexelCoordinate(vox_off, 0u, 0u, &vx, &vy, &vz, &vs) == 0);
        OS_REQUIRE(vx == 11u && vy == 23u && vz == 9u && vs == 0u);

        std::vector<uint8_t> tiled3d(static_cast<size_t>(sz3d), 0u);
        const uint32_t rp = 32u * 4u;
        const uint32_t sp = rp * 32u;
        std::vector<uint8_t> src3d(static_cast<size_t>(sp) * 16u);
        FillPattern(src3d, 0x3D99u);
        std::vector<uint8_t> dst3d(src3d.size(), 0u);
        OS_REQUIRE(comp3d->CopyIntoSubresource(tiled3d.data(), 0u, 0u, src3d.data(), rp, sp) == 0);
        OS_REQUIRE(comp3d->CopyFromSubresource(dst3d.data(), 0u, 0u, tiled3d.data(), rp, sp) == 0);
        OS_REQUIRE(std::memcmp(src3d.data(), dst3d.data(), src3d.size()) == 0);
        comp3d->Release();
        std::printf("  1D (512x4 mips) + 3D (32x32x16) via XGCreateTexture1D/TextureComputer BIT-EXACT!\n");
    }

    FreeLibrary(mod);
    std::printf("  PASS: xg_xs.dll XGCreateTextureComputer + 1D/2D/3D + OptimalTileMode verified!\n");
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    TestDxcompilerXsLive();
    TestXCrdApiDStorageForwarding();
    TestXgCreateTextureComputerLive();
    std::printf("\n[PASS] test_ue5_dxc_xcrdapi_xg (all 3 UE5 bringup shims live-verified!)\n");
#else
    std::printf("[SKIP] test_ue5_dxc_xcrdapi_xg (Windows-only live DLL test)\n");
#endif
    return 0;
}
