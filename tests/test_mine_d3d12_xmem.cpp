#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#endif

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
fs::path GetExeDir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (len > 0 && len < (sizeof(buf) / sizeof(buf[0]))) {
        return fs::path(buf).parent_path();
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

void VerifyGuidExport(HMODULE mod, const char* symbol_name, const GUID& expected) {
    const void* sym = reinterpret_cast<const void*>(GetProcAddress(mod, symbol_name));
    if (!sym) {
        std::fprintf(stderr, "Missing data export from d3d12_xs.dll: %s\n", symbol_name);
    }
    OS_REQUIRE(sym != nullptr);
    GUID actual{};
    std::memcpy(&actual, sym, sizeof(GUID));
    if (std::memcmp(&actual, &expected, sizeof(GUID)) != 0) {
        std::fprintf(
            stderr,
            "GUID mismatch for %s: actual={%08lX-%04X-%04X-...} expected={%08lX-%04X-%04X-...}\n",
            symbol_name,
            static_cast<unsigned long>(actual.Data1),
            static_cast<unsigned int>(actual.Data2),
            static_cast<unsigned int>(actual.Data3),
            static_cast<unsigned long>(expected.Data1),
            static_cast<unsigned int>(expected.Data2),
            static_cast<unsigned int>(expected.Data3));
    }
    OS_REQUIRE(std::memcmp(&actual, &expected, sizeof(GUID)) == 0);
}

void TestD3D12XsGuidExports() {
    std::printf("[1/2] d3d12_xs.dll exported GUID data symbols (31 IID_ID3D12* exports)...\n");

    const fs::path dll_path = GetExeDir() / L"d3d12_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    // Explicit byte-exact verification for IID_ID3D12Device: {189819f1-1db6-4b57-be54-1821339b85f7}
    constexpr GUID kExpectedDeviceGuid = {
        0x189819f1u, 0x1db6u, 0x4b57u, {0xbeu, 0x54u, 0x18u, 0x21u, 0x33u, 0x9bu, 0x85u, 0xf7u}};
    OS_REQUIRE(std::memcmp(&kExpectedDeviceGuid, &__uuidof(ID3D12Device), sizeof(GUID)) == 0);
    VerifyGuidExport(mod, "IID_ID3D12Device", kExpectedDeviceGuid);

    // Core sibling D3D12 COM interface GUID data exports
    VerifyGuidExport(mod, "IID_ID3D12Device1", __uuidof(ID3D12Device1));
    VerifyGuidExport(mod, "IID_ID3D12Device2", __uuidof(ID3D12Device2));
    VerifyGuidExport(mod, "IID_ID3D12Device3", __uuidof(ID3D12Device3));
    VerifyGuidExport(mod, "IID_ID3D12Device4", __uuidof(ID3D12Device4));
    VerifyGuidExport(mod, "IID_ID3D12Device5", __uuidof(ID3D12Device5));
    VerifyGuidExport(mod, "IID_ID3D12Device6", __uuidof(ID3D12Device6));
    VerifyGuidExport(mod, "IID_ID3D12Device7", __uuidof(ID3D12Device7));
    VerifyGuidExport(mod, "IID_ID3D12Device8", __uuidof(ID3D12Device8));
    VerifyGuidExport(mod, "IID_ID3D12CommandQueue", __uuidof(ID3D12CommandQueue));
    VerifyGuidExport(mod, "IID_ID3D12CommandAllocator", __uuidof(ID3D12CommandAllocator));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList", __uuidof(ID3D12GraphicsCommandList));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList1", __uuidof(ID3D12GraphicsCommandList1));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList2", __uuidof(ID3D12GraphicsCommandList2));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList3", __uuidof(ID3D12GraphicsCommandList3));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList4", __uuidof(ID3D12GraphicsCommandList4));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList5", __uuidof(ID3D12GraphicsCommandList5));
    VerifyGuidExport(mod, "IID_ID3D12GraphicsCommandList6", __uuidof(ID3D12GraphicsCommandList6));
    VerifyGuidExport(mod, "IID_ID3D12Resource", __uuidof(ID3D12Resource));
    VerifyGuidExport(mod, "IID_ID3D12Resource1", __uuidof(ID3D12Resource1));
    VerifyGuidExport(mod, "IID_ID3D12Resource2", __uuidof(ID3D12Resource2));
    VerifyGuidExport(mod, "IID_ID3D12Heap", __uuidof(ID3D12Heap));
    VerifyGuidExport(mod, "IID_ID3D12Heap1", __uuidof(ID3D12Heap1));
    VerifyGuidExport(mod, "IID_ID3D12Fence", __uuidof(ID3D12Fence));
    VerifyGuidExport(mod, "IID_ID3D12Fence1", __uuidof(ID3D12Fence1));
    VerifyGuidExport(mod, "IID_ID3D12PipelineState", __uuidof(ID3D12PipelineState));
    VerifyGuidExport(mod, "IID_ID3D12RootSignature", __uuidof(ID3D12RootSignature));
    VerifyGuidExport(mod, "IID_ID3D12DescriptorHeap", __uuidof(ID3D12DescriptorHeap));
    VerifyGuidExport(mod, "IID_ID3D12QueryHeap", __uuidof(ID3D12QueryHeap));
    VerifyGuidExport(mod, "IID_ID3D12CommandSignature", __uuidof(ID3D12CommandSignature));
    VerifyGuidExport(mod, "IID_ID3D12StateObject", __uuidof(ID3D12StateObject));

    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}

void TestXMemTransferMemory() {
    std::printf("[2/2] xmem.dll XMemTransferMemory (normal, overlapping, zero-size, null checks, telemetry)...\n");

    const fs::path dll_path = GetExeDir() / L"xmem.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnXMemTransferMemory = HRESULT(WINAPI*)(void*, const void*, SIZE_T, UINT64);
    using PfnGetTelemetry = int32_t(WINAPI*)(const char*, uint64_t*, uint64_t*);

    auto transfer_fn = reinterpret_cast<PfnXMemTransferMemory>(
        GetProcAddress(mod, "XMemTransferMemory"));
    auto telemetry_fn = reinterpret_cast<PfnGetTelemetry>(
        GetProcAddress(mod, "XMemGetTelemetryCallCountForTest"));
    OS_REQUIRE(transfer_fn != nullptr);
    OS_REQUIRE(telemetry_fn != nullptr);

    // 1. Normal non-overlapping copy with guard sentinels
    {
        constexpr size_t kGuard = 16u;
        constexpr size_t kSize = 4096u;
        std::vector<uint8_t> src(kSize);
        for (size_t i = 0; i < kSize; ++i) {
            src[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
        }
        std::vector<uint8_t> dst(kSize + 2u * kGuard, 0xCCu);
        const HRESULT hr = transfer_fn(dst.data() + kGuard, src.data(), kSize, 0ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(std::memcmp(dst.data() + kGuard, src.data(), kSize) == 0);
        for (size_t i = 0; i < kGuard; ++i) {
            OS_REQUIRE(dst[i] == 0xCCu);
            OS_REQUIRE(dst[kGuard + kSize + i] == 0xCCu);
        }
    }

    // 2. Overlapping copy (forward: dst > src) with non-zero Scarlett transfer flags
    {
        constexpr size_t kBufSize = 512u;
        constexpr size_t kShift = 37u;
        constexpr size_t kCopyLen = 300u;
        std::vector<uint8_t> buf(kBufSize);
        for (size_t i = 0; i < kBufSize; ++i) {
            buf[i] = static_cast<uint8_t>((i * 17u + 5u) & 0xFFu);
        }
        std::vector<uint8_t> expected = buf;
        std::memmove(expected.data() + kShift, expected.data(), kCopyLen);

        const HRESULT hr = transfer_fn(buf.data() + kShift, buf.data(), kCopyLen, 0x1ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(buf == expected);
    }

    // 3. Overlapping copy (backward: dst < src)
    {
        constexpr size_t kBufSize = 512u;
        constexpr size_t kShift = 41u;
        constexpr size_t kCopyLen = 300u;
        std::vector<uint8_t> buf(kBufSize);
        for (size_t i = 0; i < kBufSize; ++i) {
            buf[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        }
        std::vector<uint8_t> expected = buf;
        std::memmove(expected.data(), expected.data() + kShift, kCopyLen);

        const HRESULT hr = transfer_fn(buf.data(), buf.data() + kShift, kCopyLen, 0ull);
        OS_REQUIRE(hr == S_OK);
        OS_REQUIRE(buf == expected);
    }

    // 4. Self-copy (dst == src)
    {
        uint8_t buf[64]{};
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = static_cast<uint8_t>(i ^ 0xA5u);
        }
        uint8_t expected[64]{};
        std::memcpy(expected, buf, sizeof(buf));
        OS_REQUIRE(transfer_fn(buf, buf, sizeof(buf), 0ull) == S_OK);
        OS_REQUIRE(std::memcmp(buf, expected, sizeof(buf)) == 0);
    }

    // 5. Zero-size copy with valid pointers leaves destination untouched
    {
        uint8_t src[16]{};
        uint8_t dst[16]{};
        std::memset(src, 0x11, sizeof(src));
        std::memset(dst, 0xEE, sizeof(dst));
        OS_REQUIRE(transfer_fn(dst, src, 0u, 0ull) == S_OK);
        for (uint8_t b : dst) {
            OS_REQUIRE(b == 0xEEu);
        }
    }

    // 6. Null-pointer validation
    {
        uint8_t buf[32]{};
        const HRESULT hr_null_dst = transfer_fn(nullptr, buf, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_null_dst) && (hr_null_dst == E_POINTER || hr_null_dst == E_INVALIDARG));

        const HRESULT hr_null_src = transfer_fn(buf, nullptr, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_null_src) && (hr_null_src == E_POINTER || hr_null_src == E_INVALIDARG));

        const HRESULT hr_both_null = transfer_fn(nullptr, nullptr, sizeof(buf), 0ull);
        OS_REQUIRE(FAILED(hr_both_null) && (hr_both_null == E_POINTER || hr_both_null == E_INVALIDARG));

        const HRESULT hr_null_zero = transfer_fn(nullptr, buf, 0u, 0ull);
        OS_REQUIRE(FAILED(hr_null_zero) && (hr_null_zero == E_POINTER || hr_null_zero == E_INVALIDARG));
    }

    // 7. CallTelemetry recorded all 9 calls (5 ok + 4 errors)
    {
        uint64_t total_calls = 0;
        uint64_t ok_calls = 0;
        OS_REQUIRE(telemetry_fn("XMemTransferMemory", &total_calls, &ok_calls) == 1);
        OS_REQUIRE(total_calls == 9ull);
        OS_REQUIRE(ok_calls == 5ull);
    }

    // 8. Real 2-arg Xbox Series X ABI for XMemGetWorkingSetStatistics(flags=0x20000001, outStats)
    {
        using PfnWs2 = int32_t(WINAPI*)(uintptr_t, void*);
        auto ws_fn = reinterpret_cast<PfnWs2>(GetProcAddress(mod, "XMemGetWorkingSetStatistics"));
        OS_REQUIRE(ws_fn != nullptr);
        struct XboxWsStats32 {
            uint64_t total_bytes;
            uint64_t available_bytes;
            uint64_t working_set_bytes;
            uint64_t peak_working_set_bytes;
        };
        XboxWsStats32 st{};
        std::memset(&st, 0xCC, sizeof(st));
        OS_REQUIRE(ws_fn(0x20000001u, &st) != 0);
        OS_REQUIRE(st.total_bytes > 0ull);
        OS_REQUIRE(st.available_bytes <= st.total_bytes);
    }

    // 9. Retail 3-arg XMemMapPhysicalPages(va, count, nullptr) unmaps cleanly & idempotently
    {
        using PfnXMemVirtualAlloc = void*(WINAPI*)(void*, SIZE_T, uint32_t, uint32_t, uint32_t);
        using PfnXMemAllocPhys = int32_t(WINAPI*)(uint32_t, uint64_t*, uint64_t*);
        using PfnXMemFreePhys = int32_t(WINAPI*)(uint64_t, const uint64_t*);
        using PfnXMemMap3 = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
        using PfnKxVirtFree = int32_t(WINAPI*)(void*, SIZE_T, uint32_t);

        auto valloc_fn = reinterpret_cast<PfnXMemVirtualAlloc>(GetProcAddress(mod, "XMemVirtualAlloc"));
        auto alloc_phys_fn = reinterpret_cast<PfnXMemAllocPhys>(GetProcAddress(mod, "XMemAllocatePhysicalPages"));
        auto free_phys_fn = reinterpret_cast<PfnXMemFreePhys>(GetProcAddress(mod, "XMemFreePhysicalPages"));
        auto map3_fn = reinterpret_cast<PfnXMemMap3>(GetProcAddress(mod, "XMemMapPhysicalPages"));
        HMODULE kx_mod = GetModuleHandleW(L"kernelx.dll");
        OS_REQUIRE(kx_mod != nullptr);
        auto vfree_fn = reinterpret_cast<PfnKxVirtFree>(GetProcAddress(kx_mod, "VirtualFree"));
        OS_REQUIRE(valloc_fn && alloc_phys_fn && free_phys_fn && map3_fn && vfree_fn);

        void* ph = valloc_fn(nullptr, 128u * 1024u, 0x20402000u, 0x181u, PAGE_NOACCESS);
        OS_REQUIRE(ph != nullptr);
        OS_REQUIRE(reinterpret_cast<uintptr_t>(ph) < (1ull << 34));  // <=2 GiB placeholder prefers <16 GiB (sdsm 34-bit VA)

        uint64_t page_cnt = 2ull;
        uint64_t pfns[2]{};
        OS_REQUIRE(alloc_phys_fn(0x20400000u, &page_cnt, pfns) != 0);
        OS_REQUIRE(page_cnt == 2ull);

        OS_REQUIRE(map3_fn(ph, 2ull, pfns) == ph);
        static_cast<uint32_t*>(ph)[0] = 0xCAFEBABEu;
        OS_REQUIRE(static_cast<const uint32_t*>(ph)[0] == 0xCAFEBABEu);

        OS_REQUIRE(map3_fn(nullptr, 2ull, nullptr) == nullptr);
        OS_REQUIRE(map3_fn(ph, 0ull, nullptr) == ph);
        OS_REQUIRE(map3_fn(ph, 2ull, nullptr) == ph);
        OS_REQUIRE(map3_fn(ph, 2ull, nullptr) == ph);  // idempotent cleanup unmap

        OS_REQUIRE(free_phys_fn(2ull, pfns) != 0);
        OS_REQUIRE(vfree_fn(ph, 0u, MEM_RELEASE) != 0);

        // 10. 0x283 large-page pool block is 2 MiB aligned and strictly below 2^40 (1 TiB)
        constexpr SIZE_T kTwoMiB = 2u * 1024u * 1024u;
        void* blk283 = valloc_fn(nullptr, kTwoMiB, 0x20003000u, 0x283u, 0x2004u);
        OS_REQUIRE(blk283 != nullptr);
        const uintptr_t addr283 = reinterpret_cast<uintptr_t>(blk283);
        OS_REQUIRE((addr283 % kTwoMiB) == 0u);
        OS_REQUIRE(addr283 <= ((1ull << 40) - kTwoMiB));
        static_cast<uint64_t*>(blk283)[0] = 0x1122334455667788ull;
        OS_REQUIRE(vfree_fn(blk283, 0u, MEM_RELEASE) != 0);

        // 11. >=64 MiB 0x283 large-page pool block uses <2^40 NTFS sparse backing when
        //     OPENSCARLETT_FORCE_SPARSE_COMMIT="large" and supports sub-range MEM_DECOMMIT.
        constexpr SIZE_T kSixtyFourMiB = 64u * 1024u * 1024u;
        SetEnvironmentVariableA("OPENSCARLETT_FORCE_SPARSE_COMMIT", "large");
        void* sparse283 = valloc_fn(nullptr, kSixtyFourMiB, 0x20003000u, 0x283u, 0x2004u);
        SetEnvironmentVariableA("OPENSCARLETT_FORCE_SPARSE_COMMIT", nullptr);
        OS_REQUIRE(sparse283 != nullptr);
        const uintptr_t saddr = reinterpret_cast<uintptr_t>(sparse283);
        OS_REQUIRE((saddr % kTwoMiB) == 0u);
        OS_REQUIRE(saddr <= ((1ull << 40) - kSixtyFourMiB));
        MEMORY_BASIC_INFORMATION smbi{};
        OS_REQUIRE(VirtualQuery(sparse283, &smbi, sizeof(smbi)) != 0u);
        OS_REQUIRE(smbi.Type == MEM_MAPPED);
        static_cast<uint64_t*>(sparse283)[0] = 0xFEEDFACECAFEBEEFull;
        OS_REQUIRE(static_cast<const uint64_t*>(sparse283)[0] == 0xFEEDFACECAFEBEEFull);
        OS_REQUIRE(vfree_fn(static_cast<uint8_t*>(sparse283) + 65536u, 65536u, MEM_DECOMMIT) != 0);
        OS_REQUIRE(vfree_fn(sparse283, 0u, MEM_RELEASE) != 0);
    }

    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}

#pragma section(".text")
// Exact byte-pattern replicas of RAGE9 `LookupResourceVar` (+0x5429D30) and
// `ResolveShaderTextureView` (+0x94E6F0) placed in `.text` of the main executable
// so `d3d12_xs` retail VEH module-origin validation exercises the real code path:
//
// Lookup stub (offset 0x00, 58 bytes):
//   0x00: 48 89 C8              mov rax, rcx
//   0x03: 0F B7 09              movzx ecx, word ptr [rcx]   <-- Case D fault when rcx == 0
//   0x06: 48 85 C9              test rcx, rcx
//   0x09: 74 2B                 je +0x2B (0x36)
//   0x0B..0x34: NOP padding
//   0x35: C3                    ret
//   0x36: 31 C0                 xor eax, eax                <-- Case D recovery target (rip + 0x33)
//   0x38: EB FB                 jmp -5 (0x35)
//
// Resolve stub (offset 0x40, 44 bytes):
//   0x40: 57                    push rdi
//   0x41: 48 83 EC 40           sub rsp, 40h
//   0x45: 48 8B 7A 28           mov rdi, qword ptr [rdx+28h]
//   0x49: E8 B2 FF FF FF        call Lookup (0x00) with rcx = arg1 ([effect+0xC90], here nullptr -> rax=0)
//   0x4E: 48 85 C0              test rax, rax
//   0x51: 74 13                 je +0x13 (0x66 -> jumps back to 0x53!)
//   0x53: F6 00 40              test byte ptr [rax], 40h    <-- Case C fault when rax == 0
//   0x56: 75 21                 jne +0x21 (0x79)
//   0x58: 48 89 F8              mov rax, rdi                <-- Case C recovery target (rip + 5)
//   0x5B: 48 83 C4 40           add rsp, 40h
//   0x5F: 5F                    pop rdi
//   0x60: C3                    ret
//   0x61..0x65: CC padding
//   0x66: EB EB                 jmp 0x53 (replicates `+0x94E749: jmp +0x94E725` unchecked fallback!)
//   0x68..0x79: C3 padding
__declspec(allocate(".text")) alignas(16) const uint8_t kRetailShaderVarStub[128] = {
    // 0x00: Lookup_5429D30 replica
    0x48u, 0x89u, 0xC8u, 0x0Fu, 0xB7u, 0x09u, 0x48u, 0x85u,
    0xC9u, 0x74u, 0x2Bu, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0xC3u, 0x31u, 0xC0u,
    0xEBu, 0xFBu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu,
    // 0x40: Resolve_94E6F0 replica
    0x57u, 0x48u, 0x83u, 0xECu, 0x40u, 0x48u, 0x8Bu, 0x7Au,
    0x28u, 0xE8u, 0xB2u, 0xFFu, 0xFFu, 0xFFu, 0x48u, 0x85u,
    0xC0u, 0x74u, 0x13u, 0xF6u, 0x00u, 0x40u, 0x75u, 0x21u,
    0x48u, 0x89u, 0xF8u, 0x48u, 0x83u, 0xC4u, 0x40u, 0x5Fu,
    0xC3u, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xEBu, 0xEBu,
    0x31u, 0xC0u, 0x48u, 0x83u, 0xC4u, 0x40u, 0x5Fu, 0xC3u,
    0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu,
    0xCCu, 0x31u, 0xC0u, 0x48u, 0x83u, 0xC4u, 0x40u, 0x5Fu
};

// Replica of `+0x17ABC50..+0x17ABCB9` (`phExtension::Clone` triplet Case E):
//   0x00: 56                    push rsi
//   0x01: 48 83 EC 20           sub rsp, 20h
//   0x05: 48 89 D6              mov rsi, rdx  (out_inst)
//   0x08: 48 8B 01              mov rax, [rcx] (0 when unplaced!)
//   0x0B: FF 50 10              call qword ptr [rax+10h]  <-- Case E fault
//   0x0E: 48 89 06              mov [rsi], rax            <-- Case E recovery target (rip + 3)
//   0x11: 48 8D 0D 00 00 00 00  lea rcx, [rip+0]
//   0x18: 48 83 C4 20           add rsp, 20h
//   0x1C: 5E                    pop rsi
//   0x1D: C3                    ret
__declspec(allocate(".text")) alignas(16) const uint8_t kRetailExtCloneStub[32] = {
    0x56u, 0x48u, 0x83u, 0xECu, 0x20u, 0x48u, 0x89u, 0xD6u,
    0x48u, 0x8Bu, 0x01u, 0xFFu, 0x50u, 0x10u, 0x48u, 0x89u,
    0x06u, 0x48u, 0x8Du, 0x0Du, 0x00u, 0x00u, 0x00u, 0x00u,
    0x48u, 0x83u, 0xC4u, 0x20u, 0x5Eu, 0xC3u, 0xCCu, 0xCCu
};

// Replica of `+0x50E5520..+0x50E5568` (Case F evicted paged `fwPool` weak-handle probe):
//   0x00: 56                       push rsi
//   0x01: 48 83 EC 20              sub rsp, 20h
//   0x05: 80 B9 72 0F 00 00 00     cmp byte ptr [rcx+0F72h], 0   <-- Case F fault (rip)
//   0x0C: 75 37                    jne +0x45 (rip + 0x40)
//   0x0E: B0 01                    mov al, 1
//   0x10: EB 2D                    jmp +0x3F (epilogue)
//   0x12..0x3E: 90...              padding matching real function layout
//   0x3F: 48 83 C4 20              add rsp, 20h                  (rip + 0x3A)
//   0x43: 5E                       pop rsi                       (rip + 0x3E)
//   0x44: C3                       ret                           (rip + 0x3F)
//   0x45: 31 C0                    xor eax, eax                  (rip + 0x40) <-- Case F recovery
//   0x47: EB F6                    jmp -10 -> 0x3F               (rip + 0x42)
__declspec(allocate(".text")) alignas(16) const uint8_t kRetailEvictedFwPoolProbeStub[80] = {
    0x56u, 0x48u, 0x83u, 0xECu, 0x20u,
    0x80u, 0xB9u, 0x72u, 0x0Fu, 0x00u, 0x00u, 0x00u,
    0x75u, 0x37u,
    0xB0u, 0x01u, 0xEBu, 0x2Du,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
    0x48u, 0x83u, 0xC4u, 0x20u, 0x5Eu, 0xC3u,
    0x31u, 0xC0u, 0xEBu, 0xF6u,
    0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu
};

void TestRetailShaderVarNullBindingGuard() {
    std::printf("[3/3] Retail Scarlett +0x94E725 / +0x5429D33 unbound shader texture var VEH guard...\n");

    const fs::path dll_path = GetExeDir() / L"d3d12_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnXboxCreateDevice = HRESULT(WINAPI*)(
        IUnknown*, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void**);
    auto create_fn = reinterpret_cast<PfnXboxCreateDevice>(GetProcAddress(mod, "D3D12XboxCreateDevice"));
    OS_REQUIRE(create_fn != nullptr);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 0x00020000u;  // Activates retail Scarlett proxy & VEH recovery guards
    params.GraphicsCommandQueueRingSizeBytes = 256u * 1024u;
    params.GraphicsScratchMemorySizeBytes = 64u * 1024u;
    params.ComputeScratchMemorySizeBytes = 64u * 1024u;

    // Test across both default hardware adapter and WARP fallback
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            SetEnvironmentVariableA("OPENSCARLETT_D3D12_WARP", "1");
        }
        IUnknown* dev = nullptr;
        const HRESULT hr = create_fn(nullptr, &params, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
        if (pass == 1) {
            SetEnvironmentVariableA("OPENSCARLETT_D3D12_WARP", nullptr);
        }
        OS_REQUIRE(SUCCEEDED(hr) && dev != nullptr);

        struct FakeGrcTexture {
            uint64_t pad[5]{};
            uint64_t default_srv = 0xCAFEBABE12345678ull;  // +0x28
        } fake_tex{};

        using PfnLookupStub = const uint8_t*(__fastcall*)(const void* table, uint32_t var_hash);
        using PfnResolveStub = uint64_t(__fastcall*)(const void* table, const FakeGrcTexture* tex, uint32_t var_hash);

        auto lookup_fn = reinterpret_cast<PfnLookupStub>(&kRetailShaderVarStub[0x00]);
        auto resolve_fn = reinterpret_cast<PfnResolveStub>(&kRetailShaderVarStub[0x40]);

        // 1) Direct null-table call (+0x5429D33 Case D) returns nullptr cleanly
        OS_REQUIRE(lookup_fn(nullptr, 0x84C69350u) == nullptr);

        // 2) Full fallback miss (+0x94E725 Case C) returns `fake_tex.default_srv` (`rdi`) cleanly
        const uint64_t resolved_srv = resolve_fn(nullptr, &fake_tex, 0x84C69350u);
        OS_REQUIRE(resolved_srv == 0xCAFEBABE12345678ull);

        // 3) Empty non-null table (`count == 0`) also misses and recovers via Case C cleanly
        const uint64_t empty_table[2] = {0ull, 0ull};
        OS_REQUIRE(resolve_fn(empty_table, &fake_tex, 0x84C69350u) == 0xCAFEBABE12345678ull);

        // 4) Unplaced extension def (`*(uint64_t*)def == 0`) recovers via Case E (+0x17ABCA1) and writes `*out = 0`
        using PfnExtCloneStub = void(__fastcall*)(const uint64_t* def, uint64_t* out_inst);
        auto ext_clone_fn = reinterpret_cast<PfnExtCloneStub>(&kRetailExtCloneStub[0]);
        const uint64_t unplaced_def[2] = {0ull, 0x12345678ull};
        uint64_t cloned_inst = 0xDEADBEEFCAFEBABEull;
        ext_clone_fn(unplaced_def, &cloned_inst);
        OS_REQUIRE(cloned_inst == 0ull);

        // 5) Evicted paged `fwPool` weak-handle probe (+0x50E5525 Case F) returns 0 on MEM_RESERVE page
        using PfnFwPoolProbe = uint8_t(__fastcall*)(const void* slot_base);
        auto fw_probe_fn = reinterpret_cast<PfnFwPoolProbe>(&kRetailEvictedFwPoolProbeStub[0]);
        alignas(64) uint8_t valid_slot[0x1000]{};
        OS_REQUIRE(fw_probe_fn(valid_slot) == 1u);
        valid_slot[0xF72] = 1u;
        OS_REQUIRE(fw_probe_fn(valid_slot) == 0u);
        void* evicted_page = VirtualAlloc(nullptr, 65536u, MEM_RESERVE, PAGE_NOACCESS);
        OS_REQUIRE(evicted_page != nullptr);
        OS_REQUIRE(fw_probe_fn(evicted_page) == 0u);
        OS_REQUIRE(VirtualFree(evicted_page, 0u, MEM_RELEASE) != FALSE);

        dev->Release();
    }

    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}
// Replica of RAGE9 lock-free descriptor pool pop (`+0x55AC251..+0x55AC2A7`):
//   0x00: 56 57 53              push rsi; push rdi; push rbx
//   0x03: 45 31 C0              xor r8d, r8d
//   0x06: 41 BB FF FF FF FF     mov r11d, 0FFFFFFFFh
//   0x0C: 48 BE 00..FF          mov rsi, 0FFFFFFFF00000000h
//   0x16: 49 89 D2              mov r10, rdx  (stride)
//   0x19: 4D 89 C1              mov r9, r8_in -> passed via r9 (pool_base)
//   0x19: 48 8B 15 30 00 00 00  mov rdx, qword ptr [rip+0x30] -> g_test_desc_pool_head at +0x50
//   0x20: 89 D7                 mov edi, edx
//   0x22: 4C 39 DF              cmp rdi, r11
//   0x25: 74 2B                 je +0x2B (0x52)
//   0x27: 49 0F AF FA           imul rdi, r10
//   0x2B: 48 89 D3              mov rbx, rdx
//   0x2E: 48 21 F3              and rbx, rsi
//   0x31: 42 8B 04 0F           mov eax, dword ptr [rdi+r9]   <-- faulting rip
//   0x35: 48 09 C3              or rbx, rax
//   0x38: 48 89 D0              mov rax, rdx
//   0x3B: F0 48 0F B1 1D 19 00 00 00  lock cmpxchg qword ptr [rip+0x19], rbx -> +0x5D? Let's place RW head via page!
// Note: `.text` is RX, whereas `head_va` must be RW! So we allocate a 64 KiB RW+RX two-page block via VirtualAlloc
// in `TestRetailDescPoolVehGuard` so `rip+disp32` points directly to a writable `uint64_t` global just like `.data`.
void TestRetailDescPoolVehGuard() {
    std::printf("[4/4] Retail Scarlett +0x55AC280 descriptor pool corrupted free-list VEH guard...\n");

    const fs::path dll_path = GetExeDir() / L"d3d12_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnXboxCreateDevice = HRESULT(WINAPI*)(
        IUnknown*, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void**);
    auto create_fn = reinterpret_cast<PfnXboxCreateDevice>(GetProcAddress(mod, "D3D12XboxCreateDevice"));
    OS_REQUIRE(create_fn != nullptr);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 0x00020000u;
    params.GraphicsCommandQueueRingSizeBytes = 256u * 1024u;
    params.GraphicsScratchMemorySizeBytes = 64u * 1024u;
    params.ComputeScratchMemorySizeBytes = 64u * 1024u;

    uint8_t* region = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x2000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    OS_REQUIRE(region != nullptr);

    // Page 0 (0x0000..0x0FFF): executable Pop stub
    // Page 1 (0x1000..0x1FFF): writable `.data` globals:
    //   +0x1000: uint64_t stride = 32
    //   +0x1008: uint64_t pool_base = reinterpret_cast<uint64_t>(region + 0x1100)
    //   +0x1010: uint64_t free_head
    static constexpr uint8_t kPopTemplate[] = {
        0x56u, 0x57u, 0x53u,                                            // 0x00: push rsi; push rdi; push rbx
        0x4Cu, 0x8Bu, 0x15u, 0xF3u, 0x0Fu, 0x00u, 0x00u,                // 0x03: mov r10, [rip+0xFF3] -> +0x1000 (stride)
        0x4Cu, 0x8Bu, 0x0Du, 0xF4u, 0x0Fu, 0x00u, 0x00u,                // 0x0A: mov r9,  [rip+0xFF4] -> +0x1008 (pool_base)
        0x45u, 0x31u, 0xC0u,                                            // 0x11: xor r8d, r8d
        0x41u, 0xBBu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,                      // 0x14: mov r11d, 0FFFFFFFFh
        0x48u, 0xBEu, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, // 0x1A: mov rsi, 0xFFFFFFFF00000000
        0x48u, 0x8Bu, 0x15u, 0xDFu, 0x0Fu, 0x00u, 0x00u,                // 0x24: mov rdx, [rip+0xFDF] -> +0x100A? Fix disp below!
        0x89u, 0xD7u,                                                   // 0x2B: mov edi, edx
        0x4Cu, 0x39u, 0xDFu,                                            // 0x2D: cmp rdi, r11
        0x74u, 0x2Bu,                                                   // 0x30: je +0x2B (0x5D)
        0x49u, 0x0Fu, 0xAFu, 0xFAu,                                     // 0x32: imul rdi, r10
        0x48u, 0x89u, 0xD3u,                                            // 0x36: mov rbx, rdx
        0x48u, 0x21u, 0xF3u,                                            // 0x39: and rbx, rsi
        0x42u, 0x8Bu, 0x04u, 0x0Fu,                                     // 0x3C: mov eax, [rdi+r9]  <-- faulting rip
        0x48u, 0x09u, 0xC3u,                                            // 0x40: or rbx, rax
        0x48u, 0x89u, 0xD0u,                                            // 0x43: mov rax, rdx
        0xF0u, 0x48u, 0x0Fu, 0xB1u, 0x1Du, 0xBFu, 0x0Fu, 0x00u, 0x00u, // 0x46: lock cmpxchg [rip+disp], rbx
        0x48u, 0x39u, 0xC2u,                                            // 0x4F: cmp rdx, rax
        0x48u, 0x89u, 0xC2u,                                            // 0x52: mov rdx, rax
        0x75u, 0xD4u,                                                   // 0x55: jne 0x2B
        0x4Cu, 0x01u, 0xCFu,                                            // 0x57: add rdi, r9
        0x49u, 0x89u, 0xF8u,                                            // 0x5A: mov r8, rdi
        0x4Cu, 0x89u, 0x01u,                                            // 0x5D: mov [rcx], r8  (0x3C + 0x21 = 0x5D!)
        0x5Bu, 0x5Fu, 0x5Eu, 0xC3u                                      // 0x60: pop rbx; pop rdi; pop rsi; ret
    };
    std::memcpy(region, kPopTemplate, sizeof(kPopTemplate));
    // Patch exact RIP-relative displacements to +0x1000, +0x1008, +0x1010:
    const int32_t d_stride = 0x1000 - 0x0A;
    const int32_t d_base   = 0x1008 - 0x11;
    const int32_t d_head1  = 0x1010 - 0x2B;
    const int32_t d_head2  = 0x1010 - 0x4F;
    std::memcpy(region + 0x06, &d_stride, 4);
    std::memcpy(region + 0x0D, &d_base,   4);
    std::memcpy(region + 0x27, &d_head1,  4);
    std::memcpy(region + 0x4B, &d_head2,  4);

    DWORD old_prot = 0;
    OS_REQUIRE(VirtualProtect(region, 0x1000u, PAGE_EXECUTE_READ, &old_prot) != FALSE);
    FlushInstructionCache(GetCurrentProcess(), region, 0x1000u);

    auto* p_stride = reinterpret_cast<uint64_t*>(region + 0x1000);
    auto* p_base   = reinterpret_cast<uint64_t*>(region + 0x1008);
    auto* p_head   = reinterpret_cast<volatile uint64_t*>(region + 0x1010);
    uint8_t* slots = region + 0x1100;
    *p_stride = 32ull;
    *p_base   = reinterpret_cast<uint64_t>(slots);

    using PfnPop = void(__fastcall*)(uint64_t* out_va);
    auto pop_fn = reinterpret_cast<PfnPop>(region);

    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            SetEnvironmentVariableA("OPENSCARLETT_D3D12_WARP", "1");
        }
        IUnknown* dev = nullptr;
        const HRESULT hr = create_fn(nullptr, &params, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
        if (pass == 1) {
            SetEnvironmentVariableA("OPENSCARLETT_D3D12_WARP", nullptr);
        }
        OS_REQUIRE(SUCCEEDED(hr) && dev != nullptr);

        // 1) Valid slot 0 -> 0xFFFFFFFF pops cleanly
        *reinterpret_cast<uint32_t*>(slots) = 0xFFFFFFFFu;
        *p_head = 0x0000000500000000ull;
        uint64_t out_va = 0ull;
        pop_fn(&out_va);
        OS_REQUIRE(out_va == reinterpret_cast<uint64_t>(slots));
        OS_REQUIRE(*p_head == 0x00000005FFFFFFFFull);

        // 2) Corrupted head (0x2E576014920F -> +48 GiB unmapped VA) recovers cleanly via VEH,
        //    seals *p_head to 0x2E58FFFFFFFF, and writes out_va = 0!
        *p_head = 0x00002E576014920Full;
        out_va = 0xDEADDEADDEADDEADull;
        pop_fn(&out_va);
        OS_REQUIRE(out_va == 0ull);
        OS_REQUIRE(*p_head == 0x00002E58FFFFFFFFull);

        dev->Release();
    }

    OS_REQUIRE(VirtualFree(region, 0, MEM_RELEASE) != FALSE);
    OS_REQUIRE(FreeLibrary(mod) != FALSE);
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    TestD3D12XsGuidExports();
    TestXMemTransferMemory();
    TestRetailShaderVarNullBindingGuard();
    TestRetailDescPoolVehGuard();
    std::printf("ALL test_mine_d3d12_xmem checks PASSED.\n");
    return 0;
#else
    std::printf("test_mine_d3d12_xmem skipped on non-Windows host.\n");
    return 0;
#endif
}
