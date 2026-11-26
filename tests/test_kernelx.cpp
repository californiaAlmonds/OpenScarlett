#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "openscarlett/kernel/gpu_tile_pool.h"
#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/kernel/xmemory.h"
#include "os_check.h"

namespace {

using namespace openscarlett;

constexpr size_t k64KiB = 65536u;
constexpr size_t k4MiB = 4ull * 1024ull * 1024ull;

uint64_t ComputeFnv1a64(const uint8_t* data, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Test 1: Real Win10 VirtualAlloc2 + MapViewOfFile3 64 KiB Aliasing + Coalesce
// ---------------------------------------------------------------------------
void TestOs64KiBAliasing() {
#if defined(_WIN32)
    OS_REQUIRE(UnifiedMemoryManager::SupportsOsPlaceholders());
#endif

    UnifiedMemoryManager mgr;

    // Allocate 2 physical 64 KiB pages (128 KiB physical section).
    PhysicalAllocation phys{};
    OS_REQUIRE(mgr.AllocatePhysicalPages(2u * k64KiB, MemoryPageSize::Page64KB, kMemFlagGraphics, phys).ok());
    OS_REQUIRE(phys.size == 2u * k64KiB);
    OS_REQUIRE(mgr.ActivePhysicalPoolCount() == 1u);

    // Reserve a 256 KiB placeholder so we exercise BOTH leading split AND middle split + coalescing.
    void* ph_base = nullptr;
    OS_REQUIRE(mgr.ReserveVirtualPlaceholder(4u * k64KiB, ph_base, MemoryPageSize::Page64KB).ok());
    OS_REQUIRE(ph_base != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(ph_base) & (k64KiB - 1u)) == 0u);
    OS_REQUIRE(mgr.PlaceholderSpanCount(ph_base) == 1u);

    // Map physical page 0 (offset 0) into VA_A (offset 0) and VA_B (offset 128 KiB, middle split).
    void* va_a_target = ph_base;
    void* va_b_target = static_cast<uint8_t*>(ph_base) + 2u * k64KiB;

    UnifiedMemoryAlias alias_a{};
    UnifiedMemoryAlias alias_b{};
    OS_REQUIRE(mgr.MapPhysicalAlias(phys.physical_id, 0u, k64KiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics, va_a_target, alias_a).ok());
    OS_REQUIRE(mgr.MapPhysicalAlias(phys.physical_id, 0u, k64KiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics, va_b_target, alias_b).ok());

    OS_REQUIRE(alias_a.host_ptr == va_a_target);
    OS_REQUIRE(alias_b.host_ptr == va_b_target);
    OS_REQUIRE(alias_a.host_ptr != alias_b.host_ptr);
    OS_REQUIRE(alias_a.from_placeholder && alias_b.from_placeholder);
    OS_REQUIRE(mgr.ActiveAliasCount() == 2u);

    auto* ptr_a = static_cast<uint8_t*>(alias_a.host_ptr);
    auto* ptr_b = static_cast<uint8_t*>(alias_b.host_ptr);

    // Write 64-bit magic pattern + full 64 KiB page payload through VA_A.
    const uint64_t magic = 0xCAFEBABE1337C0DEull;
    std::memcpy(ptr_a, &magic, sizeof(magic));
    for (size_t i = sizeof(magic); i < k64KiB; ++i) {
        ptr_a[i] = static_cast<uint8_t>((i * 131u + 0x5Au) & 0xFFu);
    }
    const uint64_t expected_hash = ComputeFnv1a64(ptr_a, k64KiB);

    // Assert VA_B immediately reads back identical magic and full-page checksum.
    uint64_t read_magic = 0;
    std::memcpy(&read_magic, ptr_b, sizeof(read_magic));
    OS_REQUIRE(read_magic == magic);
    OS_REQUIRE(std::memcmp(ptr_a, ptr_b, k64KiB) == 0);
    OS_REQUIRE(ComputeFnv1a64(ptr_b, k64KiB) == expected_hash);

    // Write at end of page through VA_B and confirm VA_A immediately reflects it.
    const uint64_t tail_magic = 0x0123456789ABCDEFull;
    std::memcpy(ptr_b + (k64KiB - sizeof(tail_magic)), &tail_magic, sizeof(tail_magic));
    uint64_t tail_check = 0;
    std::memcpy(&tail_check, ptr_a + (k64KiB - sizeof(tail_magic)), sizeof(tail_check));
    OS_REQUIRE(tail_check == tail_magic);

    // Unmap VA_B while VA_A remains valid.
    OS_REQUIRE(mgr.UnmapPhysicalAlias(alias_b.host_ptr).ok());
    OS_REQUIRE(mgr.ActiveAliasCount() == 1u);

    // VA_A must remain completely intact.
    uint64_t still_magic = 0;
    std::memcpy(&still_magic, ptr_a, sizeof(still_magic));
    OS_REQUIRE(still_magic == magic);

    // Remap VA_B to physical page 1 (offset 64 KiB) and verify full physical isolation!
    UnifiedMemoryAlias alias_b2{};
    OS_REQUIRE(mgr.MapPhysicalAlias(phys.physical_id, k64KiB, k64KiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics, va_b_target, alias_b2).ok());
    OS_REQUIRE(alias_b2.host_ptr == va_b_target);

    auto* ptr_b2 = static_cast<uint8_t*>(alias_b2.host_ptr);
    const uint64_t isolated_magic = 0xDEADBEEF55AA1234ull;
    std::memcpy(ptr_b2, &isolated_magic, sizeof(isolated_magic));

    uint64_t check_a_after = 0;
    uint64_t check_b2_after = 0;
    std::memcpy(&check_a_after, ptr_a, sizeof(check_a_after));
    std::memcpy(&check_b2_after, ptr_b2, sizeof(check_b2_after));
    OS_REQUIRE(check_a_after == magic);
    OS_REQUIRE(check_b2_after == isolated_magic);

    // Unmap both aliases and verify automatic placeholder coalescing restored 1 contiguous span!
    OS_REQUIRE(mgr.UnmapPhysicalAlias(alias_b2.host_ptr).ok());
    OS_REQUIRE(mgr.UnmapPhysicalAlias(alias_a.host_ptr).ok());
    OS_REQUIRE(mgr.ActiveAliasCount() == 0u);
    OS_REQUIRE(mgr.PlaceholderSpanCount(ph_base) == 1u);

    // Proves OS-level MEM_COALESCE_PLACEHOLDERS succeeded: mapping a single 128 KiB view across
    // the previously split region must now succeed.
    UnifiedMemoryAlias full_alias{};
    OS_REQUIRE(mgr.MapPhysicalAlias(phys.physical_id, 0u, 2u * k64KiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics, ph_base, full_alias).ok());
    OS_REQUIRE(mgr.UnmapPhysicalAlias(full_alias.host_ptr).ok());
    OS_REQUIRE(mgr.PlaceholderSpanCount(ph_base) == 1u);

    // Explicit SplitVirtualPlaceholder / CoalesceVirtualPlaceholders round-trip.
    OS_REQUIRE(mgr.SplitVirtualPlaceholder(static_cast<uint8_t*>(ph_base) + k64KiB, k64KiB).ok());
    OS_REQUIRE(mgr.PlaceholderSpanCount(ph_base) == 3u);
    OS_REQUIRE(mgr.CoalesceVirtualPlaceholders(ph_base, 4u * k64KiB).ok());
    OS_REQUIRE(mgr.PlaceholderSpanCount(ph_base) == 1u);

    OS_REQUIRE(mgr.ReleaseVirtualPlaceholder(ph_base).ok());
    OS_REQUIRE(mgr.FreePhysicalPages(phys.physical_id).ok());
    OS_REQUIRE(mgr.ActivePlaceholderCount() == 0u);
    OS_REQUIRE(mgr.ActivePhysicalPoolCount() == 0u);
    std::printf("[PASS] Test 1: Win10 VirtualAlloc2/MapViewOfFile3 64 KiB aliasing + split/coalesce\n");
}

// ---------------------------------------------------------------------------
// Test 2: Real 4 MiB Page Granule Aliasing Across Two Distinct 4 MiB VAs
// ---------------------------------------------------------------------------
void TestOs4MiBAliasing() {
    UnifiedMemoryManager mgr;

    PhysicalAllocation phys4m{};
    OS_REQUIRE(mgr.AllocatePhysicalPages(k4MiB, MemoryPageSize::Page4MB,
                                         kMemFlagGraphics | kMemFlagLargePages, phys4m).ok());
    OS_REQUIRE(phys4m.size == k4MiB);

    void* ph1 = nullptr;
    void* ph2 = nullptr;
    OS_REQUIRE(mgr.ReserveVirtualPlaceholder(k4MiB, ph1, MemoryPageSize::Page4MB).ok());
    OS_REQUIRE(mgr.ReserveVirtualPlaceholder(k4MiB, ph2, MemoryPageSize::Page4MB).ok());
    OS_REQUIRE(ph1 != nullptr && ph2 != nullptr && ph1 != ph2);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(ph1) % k4MiB) == 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(ph2) % k4MiB) == 0u);

    UnifiedMemoryAlias a1{};
    UnifiedMemoryAlias a2{};
    OS_REQUIRE(mgr.MapPhysicalAlias(phys4m.physical_id, 0u, k4MiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics | kMemFlagLargePages, ph1, a1).ok());
    OS_REQUIRE(mgr.MapPhysicalAlias(phys4m.physical_id, 0u, k4MiB, MemoryProtection::ReadWrite,
                                    kMemFlagGraphics | kMemFlagLargePages, ph2, a2).ok());
    OS_REQUIRE(a1.host_ptr == ph1);
    OS_REQUIRE(a2.host_ptr == ph2);
    OS_REQUIRE(a1.size == k4MiB && a2.size == k4MiB);

    auto* p1 = static_cast<uint8_t*>(a1.host_ptr);
    auto* p2 = static_cast<uint8_t*>(a2.host_ptr);

    const uint64_t head_val = 0x4D45474150414745ull;  // "MEGAPAGE"
    const uint64_t mid_val = 0x534341524C455454ull;   // "SCARLETT"
    const uint64_t tail_val = 0xFEDCBA9876543210ull;

    std::memcpy(p1 + 0u, &head_val, sizeof(head_val));
    std::memcpy(p1 + (k4MiB / 2u), &mid_val, sizeof(mid_val));
    std::memcpy(p1 + (k4MiB - sizeof(tail_val)), &tail_val, sizeof(tail_val));

    uint64_t r_head = 0;
    uint64_t r_mid = 0;
    uint64_t r_tail = 0;
    std::memcpy(&r_head, p2 + 0u, sizeof(r_head));
    std::memcpy(&r_mid, p2 + (k4MiB / 2u), sizeof(r_mid));
    std::memcpy(&r_tail, p2 + (k4MiB - sizeof(r_tail)), sizeof(r_tail));

    OS_REQUIRE(r_head == head_val);
    OS_REQUIRE(r_mid == mid_val);
    OS_REQUIRE(r_tail == tail_val);

    // Reverse direction write from VA2 -> VA1 at offset 4 MiB - 8.
    const uint64_t updated_tail = 0x1122334455667788ull;
    std::memcpy(p2 + (k4MiB - sizeof(updated_tail)), &updated_tail, sizeof(updated_tail));
    uint64_t check_tail = 0;
    std::memcpy(&check_tail, p1 + (k4MiB - sizeof(updated_tail)), sizeof(check_tail));
    OS_REQUIRE(check_tail == updated_tail);

    OS_REQUIRE(mgr.UnmapPhysicalAlias(a1.host_ptr).ok());
    OS_REQUIRE(mgr.UnmapPhysicalAlias(a2.host_ptr).ok());
    OS_REQUIRE(mgr.ReleaseVirtualPlaceholder(ph1).ok());
    OS_REQUIRE(mgr.ReleaseVirtualPlaceholder(ph2).ok());
    OS_REQUIRE(mgr.FreePhysicalPages(phys4m.physical_id).ok());
    OS_REQUIRE(mgr.ActiveAliasCount() == 0u);
    OS_REQUIRE(mgr.ActivePlaceholderCount() == 0u);
    OS_REQUIRE(mgr.ActivePhysicalPoolCount() == 0u);
    std::printf("[PASS] Test 2: Real 4 MiB page granule aliasing across distinct 4 MiB VAs\n");
}

// ---------------------------------------------------------------------------
// Test 3: GpuTilePool 64 KiB + 4 MiB Super-Tile Aliasing, Remapping & Stats
// ---------------------------------------------------------------------------
void TestGpuTilePool() {
    UnifiedMemoryManager mgr;
    GpuTilePool tile_pool(mgr);

    uint64_t pool_id = 0;
    OS_REQUIRE(tile_pool.CreatePool(8u, kTilePoolFlagGraphics, pool_id).ok());
    OS_REQUIRE(pool_id != 0u);

    uint32_t count = 0;
    OS_REQUIRE(tile_pool.GetPoolTileCount(pool_id, count));
    OS_REQUIRE(count == 8u);

    uint64_t res_a = 0;
    uint64_t res_b = 0;
    OS_REQUIRE(tile_pool.ReserveTiledResource(4u, 2u, res_a).ok());
    OS_REQUIRE(tile_pool.ReserveTiledResource(2u, 2u, res_b).ok());

    // Map row 0 of res_a (4 tiles) -> physical tiles [0..3].
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_a, {0u, 0u, 0u}, pool_id, 0u, 4u, kTileMapFlagNone).ok());

    GpuTilePoolStats stats = tile_pool.GetStats();
    OS_REQUIRE(stats.total_pools == 1u);
    OS_REQUIRE(stats.total_resources == 2u);
    OS_REQUIRE(stats.total_physical_tiles == 8u);
    OS_REQUIRE(stats.mapped_virtual_tiles == 4u);
    OS_REQUIRE(stats.unique_mapped_physical_tiles == 4u);
    OS_REQUIRE(stats.aliased_tile_count == 0u);

    // Alias res_a(1, 1) and res_b(0, 0) + res_b(1, 0) all onto physical tile 0!
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_a, {1u, 1u, 0u}, pool_id, 0u, 1u, kTileMapFlagNone).ok());
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_b, {0u, 0u, 0u}, pool_id, 0u, 2u,
                                            kTileMapFlagReuseSingleTile).ok());

    stats = tile_pool.GetStats();
    OS_REQUIRE(stats.mapped_virtual_tiles == 7u);
    OS_REQUIRE(stats.unique_mapped_physical_tiles == 4u);
    OS_REQUIRE(stats.aliased_tile_count == 3u);

    GpuTileMappingInfo info_a00{};
    GpuTileMappingInfo info_a11{};
    GpuTileMappingInfo info_b00{};
    GpuTileMappingInfo info_b10{};
    OS_REQUIRE(tile_pool.QueryTileMapping(res_a, {0u, 0u, 0u}, info_a00).ok());
    OS_REQUIRE(tile_pool.QueryTileMapping(res_a, {1u, 1u, 0u}, info_a11).ok());
    OS_REQUIRE(tile_pool.QueryTileMapping(res_b, {0u, 0u, 0u}, info_b00).ok());
    OS_REQUIRE(tile_pool.QueryTileMapping(res_b, {1u, 0u, 0u}, info_b10).ok());
    OS_REQUIRE(info_a00.virtual_address != info_a11.virtual_address);
    OS_REQUIRE(info_a00.virtual_address != info_b00.virtual_address);
    OS_REQUIRE(info_b00.virtual_address != info_b10.virtual_address);

    // Write via res_a(0,0), immediately read back identically through all 3 aliased VAs.
    const uint64_t tile_pattern[4] = {
        0x1111222233334444ull,
        0x5555666677778888ull,
        0x9999AAAABBBBCCCCull,
        0xDDDDEEEEFFFF0001ull,
    };
    OS_REQUIRE(tile_pool.WriteTileBytes(res_a, {0u, 0u, 0u}, 1024u, tile_pattern, sizeof(tile_pattern)).ok());

    uint64_t read_buf[4] = {};
    OS_REQUIRE(tile_pool.ReadTileBytes(res_a, {1u, 1u, 0u}, 1024u, read_buf, sizeof(read_buf)).ok());
    OS_REQUIRE(std::memcmp(read_buf, tile_pattern, sizeof(tile_pattern)) == 0);

    std::memset(read_buf, 0, sizeof(read_buf));
    OS_REQUIRE(tile_pool.ReadTileBytes(res_b, {1u, 0u, 0u}, 1024u, read_buf, sizeof(read_buf)).ok());
    OS_REQUIRE(std::memcmp(read_buf, tile_pattern, sizeof(tile_pattern)) == 0);

    // Remap res_b(0,0) to physical tile 5; confirm isolation and updated alias count.
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_b, {0u, 0u, 0u}, pool_id, 5u, 1u, kTileMapFlagNone).ok());
    const uint64_t tile5_val = 0xAABBCCDDEEFF0011ull;
    OS_REQUIRE(tile_pool.WriteTileBytes(res_b, {0u, 0u, 0u}, 1024u, &tile5_val, sizeof(tile5_val)).ok());

    uint64_t check_a00 = 0;
    uint64_t check_b00 = 0;
    OS_REQUIRE(tile_pool.ReadTileBytes(res_a, {0u, 0u, 0u}, 1024u, &check_a00, sizeof(check_a00)).ok());
    OS_REQUIRE(tile_pool.ReadTileBytes(res_b, {0u, 0u, 0u}, 1024u, &check_b00, sizeof(check_b00)).ok());
    OS_REQUIRE(check_a00 == tile_pattern[0]);
    OS_REQUIRE(check_b00 == tile5_val);
    OS_REQUIRE(tile_pool.AliasedTileCount() == 2u);

    // Unmap res_b(1,0) via NULL flag and verify read fails with NotFound.
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_b, {1u, 0u, 0u}, 0u, 0u, 1u, kTileMapFlagNull).ok());
    OS_REQUIRE(!tile_pool.ReadTileBytes(res_b, {1u, 0u, 0u}, 0u, &check_b00, sizeof(check_b00)).ok());
    OS_REQUIRE(tile_pool.AliasedTileCount() == 1u);

    // Map res_b(1,1) read-only to tile 0: writes must be rejected while reads succeed.
    OS_REQUIRE(tile_pool.UpdateTileMappings(res_b, {1u, 1u, 0u}, pool_id, 0u, 1u, kTileMapFlagReadOnly).ok());
    OS_REQUIRE(!tile_pool.WriteTileBytes(res_b, {1u, 1u, 0u}, 0u, &tile5_val, sizeof(tile5_val)).ok());
    OS_REQUIRE(tile_pool.ReadTileBytes(res_b, {1u, 1u, 0u}, 1024u, &check_a00, sizeof(check_a00)).ok());
    OS_REQUIRE(check_a00 == tile_pattern[0]);

    // Pool cannot be destroyed while tiles remain mapped.
    OS_REQUIRE(!tile_pool.DestroyPool(pool_id).ok());

    // 4 MiB super-tile pool aliasing verification.
    uint64_t super_pool = 0;
    uint64_t super_res = 0;
    OS_REQUIRE(tile_pool.CreateSuperTilePool(2u, kTilePoolFlagGraphics, super_pool).ok());
    OS_REQUIRE(tile_pool.ReserveTiledResource(2u, 1u, super_res, kGpuSuperTileSizeBytes).ok());
    OS_REQUIRE(tile_pool.UpdateTileMappings(super_res, {0u, 0u, 0u}, super_pool, 0u, 2u,
                                            kTileMapFlagReuseSingleTile | kTileMapFlagSuperTile4MB).ok());

    const uint64_t super_magic = 0x535550455254494Cull;  // "SUPERTIL"
    OS_REQUIRE(tile_pool.WriteTileBytes(super_res, {0u, 0u, 0u}, k4MiB - sizeof(super_magic),
                                        &super_magic, sizeof(super_magic)).ok());
    uint64_t super_read = 0;
    OS_REQUIRE(tile_pool.ReadTileBytes(super_res, {1u, 0u, 0u}, k4MiB - sizeof(super_magic),
                                       &super_read, sizeof(super_read)).ok());
    OS_REQUIRE(super_read == super_magic);

    // Clean teardown and zero-leak stat check.
    OS_REQUIRE(tile_pool.ReleaseTiledResource(super_res).ok());
    OS_REQUIRE(tile_pool.DestroyPool(super_pool).ok());
    OS_REQUIRE(tile_pool.ReleaseTiledResource(res_a).ok());
    OS_REQUIRE(tile_pool.ReleaseTiledResource(res_b).ok());
    OS_REQUIRE(tile_pool.DestroyPool(pool_id).ok());

    stats = tile_pool.GetStats();
    OS_REQUIRE(stats.total_pools == 0u);
    OS_REQUIRE(stats.total_resources == 0u);
    OS_REQUIRE(stats.total_physical_tiles == 0u);
    OS_REQUIRE(stats.mapped_virtual_tiles == 0u);
    OS_REQUIRE(stats.unique_mapped_physical_tiles == 0u);
    OS_REQUIRE(stats.aliased_tile_count == 0u);
    OS_REQUIRE(stats.physical_bytes == 0u);
    std::printf("[PASS] Test 3: GpuTilePool 64 KiB + 4 MiB super-tile aliasing, remapping & stats\n");
}

// ---------------------------------------------------------------------------
// Test 4: kernelx.dll Exports, Live Physical Aliasing & CallTelemetry
// ---------------------------------------------------------------------------
void AssertTelemetryRecorded(const char* symbol_name) {
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t errors = 0;
    OS_REQUIRE(KernelXGetTelemetryCallCount(symbol_name, &total, &ok, &errors) == 1);
    OS_REQUIRE(total >= 1u);
    OS_REQUIRE(ok >= 1u);
}

void TestKernelXExports() {
    KernelXResetTelemetry();

    // 1. Console identity, OS version & processor count.
    OS_REQUIRE(GetConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA);

    KERNELX_OS_VERSION os_ver{};
    OS_REQUIRE(GetSystemOSVersion(&os_ver) == 1);
    OS_REQUIRE(os_ver.MajorVersion == 10u);
    OS_REQUIRE(os_ver.BuildNumber >= 26100u);

    const uint32_t procs = XGetProcessorCount();
    OS_REQUIRE(procs == 14u);

    // 2. TitleMemoryStatus & ToolingMemoryStatus baseline.
    TITLEMEMORYSTATUS tms_before{};
    tms_before.dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    OS_REQUIRE(TitleMemoryStatus(&tms_before) == 1);
    constexpr uint64_t kExpectedSeriesXTitleBudget = 27ull * 512ull * 1024ull * 1024ull;  // 13.5 GiB
    OS_REQUIRE(tms_before.ullTotalMem == kExpectedSeriesXTitleBudget);
    OS_REQUIRE(tms_before.ullTitleAvail <= tms_before.ullTotalMem);

    TOOLINGMEMORYSTATUS tool_ms{};
    tool_ms.dwLength = static_cast<uint32_t>(sizeof(TOOLINGMEMORYSTATUS));
    OS_REQUIRE(ToolingMemoryStatus(&tool_ms) == 1);
    constexpr uint64_t kExpectedSeriesXToolingBudget = 5ull * 512ull * 1024ull * 1024ull;  // 2.5 GiB
    OS_REQUIRE(tool_ms.ullTotalMem == kExpectedSeriesXToolingBudget);

    // 3. XMemAlloc / XMemFree / XMemSize (64B, 4KB, 64KB alignments + zero-init).
    const uint64_t attr_64 = MAKE_XALLOC_ATTRIBUTES(
        0, 1, 0, 0, 1, XALLOC_ALIGNMENT_64, XALLOC_MEMTYPE_HEAP, XALLOC_PAGESIZE_DEFAULT, 1);
    const uint64_t attr_4k = MAKE_XALLOC_ATTRIBUTES(
        0, 1, 0, 0, 2, XALLOC_ALIGNMENT_4K, XALLOC_MEMTYPE_HEAP, XALLOC_PAGESIZE_4KB, 1);
    const uint64_t attr_64k = MAKE_XALLOC_ATTRIBUTES(
        0, 1, 0, 0, 3, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE, XALLOC_PAGESIZE_64KB, 1);

    void* p64 = XMemAlloc(300u, attr_64);
    void* p4k = XMemAlloc(5000u, attr_4k);
    void* p64k = XMemAllocDefault(100000u, attr_64k);
    OS_REQUIRE(p64 != nullptr && p4k != nullptr && p64k != nullptr);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p64) % 64u) == 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p4k) % 4096u) == 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(p64k) % 65536u) == 0u);

    // Zero-init verification on p64k.
    const auto* p64k_bytes = static_cast<const uint8_t*>(p64k);
    OS_REQUIRE(p64k_bytes[0] == 0u && p64k_bytes[65535] == 0u && p64k_bytes[99999] == 0u);

    OS_REQUIRE(XMemSize(p64, attr_64) == 300u);
    OS_REQUIRE(XMemSize(p4k, attr_4k) == 5000u);
    OS_REQUIRE(XMemSizeDefault(p64k, attr_64k) == 100000u);

    TITLEMEMORYSTATUS tms_during{};
    tms_during.dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    OS_REQUIRE(TitleMemoryStatus(&tms_during) == 1);
    OS_REQUIRE(tms_during.ullTitleUsed > tms_before.ullTitleUsed);

    XMemFree(p64, attr_64);
    XMemFree(p4k, attr_4k);
    XMemFreeDefault(p64k, attr_64k);

    // 4. Auxiliary title memory.
    size_t aux_actual = 0;
    void* aux_ptr = XMemGetAuxiliaryTitleMemory(200000u, &aux_actual);
    OS_REQUIRE(aux_ptr != nullptr);
    OS_REQUIRE(aux_actual == 262144u);  // 200000 aligned up to 64 KiB = 256 KiB
    OS_REQUIRE((reinterpret_cast<uintptr_t>(aux_ptr) % k64KiB) == 0u);
    std::memset(aux_ptr, 0x3C, aux_actual);
    OS_REQUIRE(XMemReleaseAuxiliaryTitleMemory(aux_ptr) == 1);

    // 5. AllocateTitlePhysicalPages + MapTitleVirtualMemory + XMemMapPhysicalPages live aliasing.
    uint64_t page_count = 1u;
    uint64_t pfn64 = 0u;
    OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_COMMIT, &page_count, &pfn64) == 1);
    OS_REQUIRE(page_count == 1u && pfn64 != 0u);

    void* va1 = MapTitleVirtualMemory(nullptr, 1u, MEM_GRAPHICS, PAGE_READWRITE, &pfn64);
    void* va2 = XMemMapPhysicalPages(nullptr, 1u, MEM_GRAPHICS, PAGE_READWRITE, &pfn64);
    OS_REQUIRE(va1 != nullptr && va2 != nullptr && va1 != va2);

    const uint64_t dll_magic = 0x09E55CA41E770042ull;
    std::memcpy(va1, &dll_magic, sizeof(dll_magic));
    uint64_t dll_read = 0;
    std::memcpy(&dll_read, va2, sizeof(dll_read));
    OS_REQUIRE(dll_read == dll_magic);

    // Cannot free PFN while still mapped.
    OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 1u, &pfn64) == 0);

    // Also verify duplicated PFN mapping inside a caller-reserved placeholder from VirtualAllocFromApp.
    void* ph_app = KernelXVirtualAllocFromApp(nullptr, 2u * k64KiB, MEM_RESERVE | MEM_GRAPHICS, PAGE_NOACCESS);
    OS_REQUIRE(ph_app != nullptr);
    const uint64_t dup_pfns[2] = {pfn64, pfn64};
    void* va_dup = MapTitlePhysicalPages(ph_app, 2u, MEM_GRAPHICS, PAGE_READWRITE, dup_pfns);
    OS_REQUIRE(va_dup == ph_app);
    uint64_t dup_page0 = 0;
    uint64_t dup_page1 = 0;
    std::memcpy(&dup_page0, va_dup, sizeof(dup_page0));
    std::memcpy(&dup_page1, static_cast<uint8_t*>(va_dup) + k64KiB, sizeof(dup_page1));
    OS_REQUIRE(dup_page0 == dll_magic && dup_page1 == dll_magic);

    uint64_t q_phys = 0;
    uint64_t q_size = 0;
    uint32_t q_flags = 0;
    OS_REQUIRE(QueryTitlePhysicalPages(va1, &q_phys, &q_size, &q_flags) == 1);
    OS_REQUIRE(q_phys != 0u && q_size == k64KiB && (q_flags & MEM_GRAPHICS) != 0u);

    // Also verify VirtualProtectFromApp on mapped alias (ReadWrite -> ReadOnly -> ReadWrite).
    uint32_t old_prot = 0u;
    OS_REQUIRE(KernelXVirtualProtectFromApp(va1, k64KiB, PAGE_READONLY, &old_prot) == 1);
    OS_REQUIRE(old_prot == PAGE_READWRITE);
    OS_REQUIRE(KernelXVirtualProtectFromApp(va1, k64KiB, PAGE_READWRITE, &old_prot) == 1);
    OS_REQUIRE(old_prot == PAGE_READONLY);

    OS_REQUIRE(UnmapTitlePhysicalPages(va_dup, 2u) == 1);
    OS_REQUIRE(KernelXVirtualFree(ph_app, 0u, MEM_RELEASE) == 1);
    OS_REQUIRE(UnmapTitleVirtualMemory(va1, 1u) == 1);
    OS_REQUIRE(XMemUnmapPhysicalPages(va2, 1u) == 1);
    OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 1u, &pfn64) == 1);

    // 5b. KernelX GPU Tile Pool C ABI live aliasing check.
    uint64_t kx_pool = 0u;
    uint64_t kx_res = 0u;
    void* kx_base = nullptr;
    OS_REQUIRE(KernelXCreateTilePool(2u, KERNELX_TILE_POOL_GRAPHICS, &kx_pool) == 1);
    OS_REQUIRE(KernelXReserveTiledResource(2u, 1u, &kx_res, &kx_base) == 1);
    OS_REQUIRE(kx_base != nullptr);
    OS_REQUIRE(KernelXMapTileRange(kx_res, 0u, 0u, kx_pool, 0u, 2u, KERNELX_TILE_MAP_REUSE_SINGLE_TILE) == 1);
    const uint64_t kx_tile_magic = 0x711E00C0DEADBEEFull;
    std::memcpy(kx_base, &kx_tile_magic, sizeof(kx_tile_magic));
    uint64_t kx_tile_alias = 0u;
    std::memcpy(&kx_tile_alias, static_cast<uint8_t*>(kx_base) + k64KiB, sizeof(kx_tile_alias));
    OS_REQUIRE(kx_tile_alias == kx_tile_magic);
    OS_REQUIRE(KernelXReleaseTiledResource(kx_res) == 1);
    OS_REQUIRE(KernelXDestroyTilePool(kx_pool) == 1);

    // 6. 4 MiB physical page aliasing via kernelx.dll ABI.
    uint64_t large_count = 1u;
    uint64_t pfn4m = 0u;
    OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_4MB_PAGES, &large_count, &pfn4m) == 1);
    void* large_va1 = MapTitleVirtualMemory(nullptr, 1u, MEM_GRAPHICS | MEM_4MB_PAGES, PAGE_READWRITE, &pfn4m);
    void* large_va2 = MapTitleVirtualMemory(nullptr, 1u, MEM_GRAPHICS | MEM_4MB_PAGES, PAGE_READWRITE, &pfn4m);
    OS_REQUIRE(large_va1 != nullptr && large_va2 != nullptr && large_va1 != large_va2);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(large_va1) % k4MiB) == 0u);
    OS_REQUIRE((reinterpret_cast<uintptr_t>(large_va2) % k4MiB) == 0u);

    const uint64_t large_magic = 0x4D42414C49415321ull;
    std::memcpy(static_cast<uint8_t*>(large_va1) + (k4MiB - sizeof(large_magic)),
                &large_magic, sizeof(large_magic));
    uint64_t large_read = 0;
    std::memcpy(&large_read, static_cast<uint8_t*>(large_va2) + (k4MiB - sizeof(large_magic)),
                sizeof(large_read));
    OS_REQUIRE(large_read == large_magic);

    OS_REQUIRE(UnmapTitleVirtualMemory(large_va1, 1u) == 1);
    OS_REQUIRE(UnmapTitleVirtualMemory(large_va2, 1u) == 1);
    OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 1u, &pfn4m) == 1);

    // 7. Thread naming (wraps Win10 SetThreadDescription / GetThreadDescription).
    const wchar_t* kWorkerName = L"OpenScarlettWorker";
    OS_REQUIRE(SetThreadName(nullptr, kWorkerName) == 0);
    wchar_t name_buf[64] = {};
    size_t req_len = 0;
    OS_REQUIRE(GetThreadName(nullptr, name_buf, 64u, &req_len) == 0);
    OS_REQUIRE(std::wcscmp(name_buf, kWorkerName) == 0);
    OS_REQUIRE(req_len == std::wcslen(kWorkerName) + 1u);

    // 8. Waitable timer & unbiased interrupt time helpers + unprefixed PE export verification.
    uint64_t t1 = 0;
    uint64_t t2 = 0;
    OS_REQUIRE(KernelXQueryUnbiasedInterruptTime(&t1) == 1);
    OS_REQUIRE(KernelXQueryUnbiasedInterruptTime(&t2) == 1);
    OS_REQUIRE(t2 >= t1);

    void* timer = KernelXCreateWaitableTimerExW(nullptr, nullptr, 0u, 0x001F0003u);  // TIMER_ALL_ACCESS
    OS_REQUIRE(timer != nullptr);
#if defined(_WIN32)
    OS_REQUIRE(CloseHandle(static_cast<HANDLE>(timer)) != 0);

    HMODULE h_kx = GetModuleHandleW(L"kernelx.dll");
    OS_REQUIRE(h_kx != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "VirtualAllocFromApp") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "VirtualProtectFromApp") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "CreateWaitableTimerExW") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "QueryUnbiasedInterruptTime") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "MapTitleVirtualMemory") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "XMemMapPhysicalPages") != nullptr);
    OS_REQUIRE(GetProcAddress(h_kx, "XMemAlloc") != nullptr);
#endif

    // 9. Verify CallTelemetry recorded every exercised export inside kernelx.dll!
    AssertTelemetryRecorded("GetConsoleType");
    AssertTelemetryRecorded("GetSystemOSVersion");
    AssertTelemetryRecorded("XGetProcessorCount");
    AssertTelemetryRecorded("TitleMemoryStatus");
    AssertTelemetryRecorded("ToolingMemoryStatus");
    AssertTelemetryRecorded("XMemAlloc");
    AssertTelemetryRecorded("XMemAllocDefault");
    AssertTelemetryRecorded("XMemFree");
    AssertTelemetryRecorded("XMemFreeDefault");
    AssertTelemetryRecorded("XMemSize");
    AssertTelemetryRecorded("XMemSizeDefault");
    AssertTelemetryRecorded("XMemGetAuxiliaryTitleMemory");
    AssertTelemetryRecorded("XMemReleaseAuxiliaryTitleMemory");
    AssertTelemetryRecorded("AllocateTitlePhysicalPages");
    AssertTelemetryRecorded("FreeTitlePhysicalPages");
    AssertTelemetryRecorded("MapTitleVirtualMemory");
    AssertTelemetryRecorded("UnmapTitleVirtualMemory");
    AssertTelemetryRecorded("MapTitlePhysicalPages");
    AssertTelemetryRecorded("UnmapTitlePhysicalPages");
    AssertTelemetryRecorded("XMemMapPhysicalPages");
    AssertTelemetryRecorded("XMemUnmapPhysicalPages");
    AssertTelemetryRecorded("QueryTitlePhysicalPages");
    AssertTelemetryRecorded("VirtualAllocFromApp");
    AssertTelemetryRecorded("VirtualProtectFromApp");
    AssertTelemetryRecorded("KernelXVirtualFree");
    AssertTelemetryRecorded("KernelXCreateTilePool");
    AssertTelemetryRecorded("KernelXDestroyTilePool");
    AssertTelemetryRecorded("KernelXReserveTiledResource");
    AssertTelemetryRecorded("KernelXReleaseTiledResource");
    AssertTelemetryRecorded("KernelXMapTileRange");
    AssertTelemetryRecorded("SetThreadName");
    AssertTelemetryRecorded("GetThreadName");
    AssertTelemetryRecorded("CreateWaitableTimerExW");
    AssertTelemetryRecorded("QueryUnbiasedInterruptTime");

    std::printf("[PASS] Test 4: kernelx.dll exports, live aliasing & CallTelemetry verified\n");
}

#if defined(_WIN32)
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        char buf[256] = {};
        const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
        if (n > 0u && n < sizeof(buf)) {
            had_old_ = true;
            old_.assign(buf, n);
        }
        ::SetEnvironmentVariableA(name, value);
    }
    ~ScopedEnvVar() {
        ::SetEnvironmentVariableA(name_, had_old_ ? old_.c_str() : nullptr);
    }

private:
    const char* name_;
    bool had_old_ = false;
    std::string old_;
};

void TestLockhartBudgetWriteWatchAndSparseFallback() {
    ScopedEnvVar guard_type("OPENSCARLETT_CONSOLE_TYPE", nullptr);
    ScopedEnvVar guard_prof("OPENSCARLETT_CONSOLE_PROFILE", nullptr);
    ScopedEnvVar guard_mb("OPENSCARLETT_TITLE_MEM_MB", nullptr);
    ScopedEnvVar guard_sparse("OPENSCARLETT_FORCE_SPARSE_COMMIT", nullptr);

    HMODULE h_kx = ::GetModuleHandleW(L"kernelx.dll");
    OS_REQUIRE(h_kx != nullptr);

    using PfnGlobalMemoryStatusEx = int32_t(__stdcall*)(MEMORYSTATUSEX*);
    using PfnQueryJobObj = int32_t(__stdcall*)(HANDLE, int, void*, uint32_t, uint32_t*);
    using PfnJobMemStatus = int32_t(__stdcall*)(TITLEMEMORYSTATUS*);
    using PfnWsStats = int32_t(__stdcall*)(uintptr_t, void*);

    auto p_gms = reinterpret_cast<PfnGlobalMemoryStatusEx>(
        ::GetProcAddress(h_kx, "GlobalMemoryStatusEx"));
    auto p_qjob = reinterpret_cast<PfnQueryJobObj>(
        ::GetProcAddress(h_kx, "QueryInformationJobObject"));
    auto p_jmem = reinterpret_cast<PfnJobMemStatus>(
        ::GetProcAddress(h_kx, "JobMemoryStatus"));
    auto p_ws = reinterpret_cast<PfnWsStats>(
        ::GetProcAddress(h_kx, "XMemGetWorkingSetStatistics"));
    OS_REQUIRE(p_gms != nullptr && p_qjob != nullptr && p_jmem != nullptr && p_ws != nullptr);

    // 1. Verify OPENSCARLETT_CONSOLE_PROFILE Lockhart aliases & 8 GiB budget across all APIs.
    constexpr uint64_t kLockhartBytes = 8ull * 1024ull * 1024ull * 1024ull;
    const char* kLockhartTokens[] = {"lockhart", "series_s", "series-s", "3", "s", "8GB"};
    for (const char* tok : kLockhartTokens) {
        ::SetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", tok);
        OS_REQUIRE(GetConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART);

        TITLEMEMORYSTATUS tms{};
        tms.dwLength = sizeof(tms);
        OS_REQUIRE(TitleMemoryStatus(&tms) == 1);
        OS_REQUIRE(tms.ullTotalMem == kLockhartBytes);
        OS_REQUIRE(tms.ullTitleAvail <= kLockhartBytes);

        TITLEMEMORYSTATUS jms{};
        jms.dwLength = sizeof(jms);
        OS_REQUIRE(p_jmem(&jms) == 1);
        OS_REQUIRE(jms.ullTotalMem == kLockhartBytes);

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        OS_REQUIRE(p_gms(&ms) == 1);
        OS_REQUIRE(ms.ullTotalPhys == kLockhartBytes);
        OS_REQUIRE(ms.ullTotalPageFile == kLockhartBytes);

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_ext{};
        uint32_t ret_len = 0u;
        OS_REQUIRE(p_qjob(nullptr,
                          static_cast<int>(JobObjectExtendedLimitInformation),
                          &job_ext,
                          sizeof(job_ext),
                          &ret_len) == 1);
        OS_REQUIRE(job_ext.JobMemoryLimit == kLockhartBytes);
        OS_REQUIRE(job_ext.ProcessMemoryLimit == kLockhartBytes);

        struct Ws32 {
            uint64_t total;
            uint64_t avail;
            uint64_t ws;
            uint64_t peak;
        } ws32{};
        std::memset(&ws32, 0xCC, sizeof(ws32));
        OS_REQUIRE(p_ws(0x20000001u, &ws32) == 1);
        OS_REQUIRE(ws32.total == kLockhartBytes);
        OS_REQUIRE(ws32.avail <= kLockhartBytes);
    }

    ::SetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", "series-x");
    OS_REQUIRE(GetConsoleType() == CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA);
    MEMORYSTATUSEX series_x_ms{};
    series_x_ms.dwLength = sizeof(series_x_ms);
    OS_REQUIRE(p_gms(&series_x_ms) != FALSE);
    OS_REQUIRE(series_x_ms.ullTotalPhys == 27ull * 512ull * 1024ull * 1024ull);

    // 2. Verify "auto" against the host, even when the executable's normal
    // GlobalMemoryStatusEx import resolves to the guest-budget export in kernelx.dll.
    ::SetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", "auto");
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    OS_REQUIRE(kernel32 != nullptr);
    auto host_memory_status = reinterpret_cast<PfnGlobalMemoryStatusEx>(
        ::GetProcAddress(kernel32, "GlobalMemoryStatusEx"));
    OS_REQUIRE(host_memory_status != nullptr);
    MEMORYSTATUSEX real_host_ms{};
    real_host_ms.dwLength = sizeof(real_host_ms);
    OS_REQUIRE(host_memory_status(&real_host_ms) != FALSE);
    OS_REQUIRE(real_host_ms.ullTotalPhys > 0ull);
    const uint32_t expected_auto =
        (real_host_ms.ullTotalPhys <= (18ull * 1024ull * 1024ull * 1024ull))
            ? CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART
            : CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA;
    OS_REQUIRE(GetConsoleType() == expected_auto);

    // 3. Verify OPENSCARLETT_TITLE_MEM_MB explicit budget override (e.g. 6144 MiB).
    ::SetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", "lockhart");
    ::SetEnvironmentVariableA("OPENSCARLETT_TITLE_MEM_MB", "6144");
    constexpr uint64_t kOverride6GiB = 6144ull * 1024ull * 1024ull;
    {
        TITLEMEMORYSTATUS tms{};
        tms.dwLength = sizeof(tms);
        OS_REQUIRE(TitleMemoryStatus(&tms) == 1);
        OS_REQUIRE(tms.ullTotalMem == kOverride6GiB);

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        OS_REQUIRE(p_gms(&ms) == 1);
        OS_REQUIRE(ms.ullTotalPhys == kOverride6GiB);
    }
    ::SetEnvironmentVariableA("OPENSCARLETT_TITLE_MEM_MB", nullptr);
    ::SetEnvironmentVariableA("OPENSCARLETT_CONSOLE_PROFILE", nullptr);

    // 4. Verify hardware MEM_WRITE_WATCH on VirtualAllocFromApp (MEM_RESERVE|MEM_COMMIT & MEM_RESERVE),
    //    XMemAllocDefault (graphics WC), and UnifiedMemoryManager::Allocate.
    {
        void* rw_mem = KernelXVirtualAllocFromApp(
            nullptr, 4u * 4096u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE);
        OS_REQUIRE(rw_mem != nullptr);
        static_cast<uint32_t*>(rw_mem)[(2u * 4096u) / 4u] = 0x12345678u;
        void* dirty[8] = {};
        ULONG_PTR count = 8u;
        DWORD gran = 0u;
        OS_REQUIRE(::GetWriteWatch(WRITE_WATCH_FLAG_RESET, rw_mem, 4u * 4096u, dirty, &count, &gran) == 0u);
        OS_REQUIRE(count == 1u && dirty[0] == static_cast<uint8_t*>(rw_mem) + 2u * 4096u);
        OS_REQUIRE(KernelXVirtualFree(rw_mem, 0u, MEM_RELEASE) == 1);

        void* res_only = KernelXVirtualAllocFromApp(nullptr, k64KiB, MEM_RESERVE, PAGE_READWRITE);
        OS_REQUIRE(res_only != nullptr);
        void* committed = KernelXVirtualAllocFromApp(res_only, k64KiB, MEM_COMMIT, PAGE_READWRITE);
        OS_REQUIRE(committed == res_only);
        static_cast<uint32_t*>(committed)[4096u / 4u] = 0xCAFEBABEu;
        count = 8u;
        OS_REQUIRE(::GetWriteWatch(WRITE_WATCH_FLAG_RESET, committed, k64KiB, dirty, &count, &gran) == 0u);
        OS_REQUIRE(count == 1u && dirty[0] == static_cast<uint8_t*>(committed) + 4096u);
        OS_REQUIRE(KernelXVirtualFree(res_only, 0u, MEM_RELEASE) == 1);

        const uint64_t gfx_attr = MAKE_XALLOC_ATTRIBUTES(
            0, 1, 0, 0, 4, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE, XALLOC_PAGESIZE_64KB, 1);
        void* xm_gfx = XMemAllocDefault(2u * k64KiB, gfx_attr);
        OS_REQUIRE(xm_gfx != nullptr);
        static_cast<uint64_t*>(xm_gfx)[0] = 0xABCD0123456789EFULL;
        count = 8u;
        OS_REQUIRE(::GetWriteWatch(WRITE_WATCH_FLAG_RESET, xm_gfx, 2u * k64KiB, dirty, &count, &gran) == 0u);
        OS_REQUIRE(count == 1u && dirty[0] == xm_gfx);
        XMemFreeDefault(xm_gfx, gfx_attr);

        UnifiedMemoryManager umm;
        UnifiedMemoryBlock ublk{};
        OS_REQUIRE(umm.Allocate(2u * k64KiB, MemoryPageSize::Page64KB, true, true, ublk).ok());
        static_cast<uint32_t*>(ublk.host_ptr)[(3u * 4096u) / 4u] = 0x778899AAu;
        count = 8u;
        OS_REQUIRE(::GetWriteWatch(WRITE_WATCH_FLAG_RESET, ublk.host_ptr, ublk.size, dirty, &count, &gran) == 0u);
        OS_REQUIRE(count == 1u && dirty[0] == static_cast<uint8_t*>(ublk.host_ptr) + 3u * 4096u);
        OS_REQUIRE(umm.Free(ublk.gpu_va).ok());
    }

    // 5. Verify anti-OOM sparse file-mapping fallback on E:\OpenScarlett\.tmp (never C:\).
    ::SetEnvironmentVariableA("OPENSCARLETT_FORCE_SPARSE_COMMIT", "1");
    {
        void* sp_va = KernelXVirtualAllocFromApp(
            nullptr, 2u * k64KiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        OS_REQUIRE(sp_va != nullptr);
        static_cast<uint64_t*>(sp_va)[0] = 0x5350415253455641ull;
        OS_REQUIRE(static_cast<uint64_t*>(sp_va)[0] == 0x5350415253455641ull);
        OS_REQUIRE(KernelXVirtualFree(sp_va, 0u, MEM_RELEASE) == 1);

        const uint64_t def_attr = MAKE_XALLOC_ATTRIBUTES(
            0, 1, 0, 0, 5, XALLOC_ALIGNMENT_64K, XALLOC_MEMTYPE_GRAPHICS_CACHEABLE, XALLOC_PAGESIZE_64KB, 1);
        void* sp_xm = XMemAllocDefault(2u * k64KiB, def_attr);
        OS_REQUIRE(sp_xm != nullptr);
        static_cast<uint64_t*>(sp_xm)[1] = 0x535041525345584Dull;
        OS_REQUIRE(static_cast<uint64_t*>(sp_xm)[1] == 0x535041525345584Dull);
        XMemFreeDefault(sp_xm, def_attr);

        uint64_t sp_pages = 2u;
        uint64_t sp_pfns[2] = {};
        OS_REQUIRE(AllocateTitlePhysicalPages(nullptr, MEM_GRAPHICS | MEM_COMMIT, &sp_pages, sp_pfns) == 1);
        void* sp_alias = MapTitleVirtualMemory(nullptr, 2u, MEM_GRAPHICS, PAGE_READWRITE, sp_pfns);
        OS_REQUIRE(sp_alias != nullptr);
        static_cast<uint64_t*>(sp_alias)[0] = 0x5048595353504152ull;
        OS_REQUIRE(UnmapTitleVirtualMemory(sp_alias, 2u) == 1);
        OS_REQUIRE(FreeTitlePhysicalPages(nullptr, 2u, sp_pfns) == 1);
    }
    ::SetEnvironmentVariableA("OPENSCARLETT_FORCE_SPARSE_COMMIT", nullptr);

    std::printf("[PASS] Test 5: Lockhart 8 GiB budget, MEM_WRITE_WATCH & E:\\OpenScarlett\\.tmp sparse fallback verified\n");
}
#endif

}  // namespace

int main() {
    TestOs64KiBAliasing();
    TestOs4MiBAliasing();
    TestGpuTilePool();
    TestKernelXExports();
#if defined(_WIN32)
    TestLockhartBudgetWriteWatchAndSparseFallback();
#endif
    std::printf("[PASS] test_kernelx: all test suites passed!\n");
    return 0;
}
