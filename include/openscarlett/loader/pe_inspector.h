#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

enum class DllCategory : uint8_t {
    WindowsNative = 0,
    ConsoleExclusive = 1,
    GdkRuntime = 2,
    Middleware = 3,
    Other = 4,
};

enum class DllRoutePolicy : uint8_t {
    Shim = 0,
    NativeGuestDll = 1,
    Windows = 2,
};

struct DllRouteRule {
    std::string canonical_name;
    DllRoutePolicy policy = DllRoutePolicy::Windows;
    int load_priority = 10;  // 0 = base shims, 1 = core shims, 2 = dependent shims, 10 = default
};

struct DllRouteDecision {
    DllRoutePolicy policy = DllRoutePolicy::Windows;
    std::string requested_name;  // Normalized requested DLL basename
    std::string canonical_name;  // Resolved target/canonical DLL basename
    std::wstring resolved_path;  // Non-empty when a backing Shim/NativeGuestDll file was located
    int load_priority = 10;
    bool from_override = false;
    bool aliased = false;        // True when requested_name routed via versioned/thunk alias or source=target override
};

struct PeImportSymbol {
    std::string name;       // Empty when imported by ordinal only
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    bool delay_loaded = false;
};

struct PeImportModule {
    std::string dll_name;
    DllCategory category = DllCategory::Other;
    std::vector<PeImportSymbol> symbols;
};

struct PeExportSymbol {
    std::string name;       // Empty when exported by ordinal only
    uint32_t ordinal = 0;
    uint32_t rva = 0;
    bool is_forwarded = false;
    std::string forwarder;  // e.g. "kernelbase.CreateFileW" or "other.#42"
};

struct PeForwarderTarget {
    std::string dll_name;     // Normalized with ".dll" suffix when omitted
    std::string symbol_name;  // Empty when by_ordinal is true
    uint16_t ordinal = 0;
    bool by_ordinal = false;
};

struct PeForwarderResolution {
    std::string final_dll;
    std::string final_symbol;
    uint16_t final_ordinal = 0;
    bool by_ordinal = false;
    uint32_t final_rva = 0;
    std::vector<std::string> chain;
};

struct PeInspectionReport {
    std::string file_path;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    uint64_t image_base = 0;
    uint32_t entry_point_rva = 0;
    uint32_t image_size = 0;
    bool is_pe64 = false;
    bool is_dll = false;
    bool has_relocations = false;
    bool has_tls = false;
    uint32_t section_count = 0;
    uint32_t total_imports = 0;
    uint32_t console_imports = 0;
    uint32_t gdk_imports = 0;
    uint32_t delay_load_imports = 0;
    std::vector<PeImportModule> imports;

    // Export directory & export forwarders (IMAGE_DIRECTORY_ENTRY_EXPORT)
    uint32_t export_dir_rva = 0;
    uint32_t export_dir_size = 0;
    uint32_t export_ordinal_base = 0;
    uint32_t malformed_export_names = 0;
    std::vector<PeExportSymbol> exports;

    // Base relocation directory (IMAGE_DIRECTORY_ENTRY_BASERELOC)
    uint32_t reloc_dir_rva = 0;
    uint32_t reloc_dir_size = 0;
    uint32_t reloc_block_count = 0;
    uint32_t reloc_entry_count = 0;

    // TLS directory & callback RVAs (IMAGE_DIRECTORY_ENTRY_TLS)
    uint32_t tls_dir_rva = 0;
    uint32_t tls_dir_size = 0;
    std::vector<uint32_t> tls_callback_rvas;

    // x64 .pdata exception table (IMAGE_DIRECTORY_ENTRY_EXCEPTION) & .xdata unwind validation
    uint32_t exception_dir_rva = 0;
    uint32_t exception_dir_size = 0;
    uint32_t exception_entry_count = 0;
    uint32_t xdata_valid_entries = 0;
    uint32_t xdata_invalid_entries = 0;
    uint32_t xdata_alloc_large_entries = 0;
    uint32_t xdata_epilog_entries = 0;
    uint32_t xdata_chained_entries = 0;
};

struct PeSectionProtectionSummary {
    uint32_t total_sections = 0;
    uint32_t protected_sections = 0;
    uint32_t skipped_sections = 0;
    uint32_t failed_sections = 0;
};

struct PeXdataValidationResult {
    uint32_t runtime_function_count = 0;
    uint32_t valid_entries = 0;
    uint32_t invalid_entries = 0;
    uint32_t v1_entries = 0;
    uint32_t v2_entries = 0;
    uint32_t v3_entries = 0;
    uint32_t alloc_large_entries = 0;
    uint32_t epilog_code_entries = 0;
    uint32_t chained_entries = 0;
    uint32_t cycle_errors = 0;
    uint32_t alignment_errors = 0;
    uint32_t bounds_errors = 0;
    std::vector<std::string> errors;
};

const char* DllCategoryName(DllCategory category) noexcept;
DllCategory ClassifyImportDll(const std::string& dll_name);

// Windows KnownDLLs resolution (queries HKLM\...\Session Manager\KnownDLLs + standard fallback set)
// so System32 KnownDLLs are always resolved from System32 before package-local images.
bool IsWindowsKnownDll(std::string_view dll_name);
bool ResolveKnownDllSystem32PathW(std::wstring_view dll_name, std::wstring& out_system32_path);
std::vector<std::string> GetWindowsKnownDllList();

// Resolves known versioned / thunk GDK DLL names (e.g. Microsoft.Xbox.Services.141.GDK.C.Thunks.dll,
// libHttpClient.141.GDK.C.dll) to their canonical OpenScarlett shim DLL basename.
// Returns false if dll_name is already canonical or not a built-in alias.
bool ResolveShimAlias(std::string_view dll_name, std::string& out_canonical_shim);

const char* DllRoutePolicyName(DllRoutePolicy policy) noexcept;

// Data-driven per-DLL routing table shared by apc_injector and hook_dll.
class DllRoutingTable {
public:
    // Populates built-in routing rules for OpenScarlett's 19 core Xbox/GDK shims plus
    // 5 diagnostic/media shims (mfplat_xs, advapi32legacy, KernelTraceControl, hix_xs, xbtp)
    // and fallback alias routing (XGamingRuntimeThunks.dll -> xgameruntime.dll, and
    // Xbox-imported mfplat.dll -> mfplat_xs.dll).
    DllRoutingTable();

    // Builds the routing table with built-in rules plus OPENSCARLETT_DLL_ROUTING overrides.
    static DllRoutingTable CreateWithEnvOverrides();

    // Applies semicolon/comma-separated override rules such as:
    //   "foo.dll=guest;dstorage_xs.dll=shim;user32.dll=windows"
    //   "XGamingRuntimeThunks.dll=xgameruntime.dll;Microsoft.Xbox.Services.141.GDK.C.Thunks.dll=passthrough"
    //   "mfplat.dll=mfplat_xs.dll"
    void ApplyOverrides(std::string_view override_spec);

    // Explicitly sets or overrides a single DLL's routing rule.
    void SetRule(std::string_view dll_name, DllRoutePolicy policy, int load_priority = 1);

    // Explicitly aliases source_dll -> target_dll (superseding prior rules on source_dll).
    void SetAlias(std::string_view source_dll, std::string_view target_dll);

    // Looks up a routing rule for dll_name (case-insensitive, basename-normalized, alias-aware).
    const DllRouteRule* FindRule(std::string_view dll_name) const;

    // Decides whether to route dll_name to Shim, NativeGuestDll, or Windows.
    // Set xbox_import_context=true when resolving static imports of a PE module that imports
    // Xbox console-exclusive DLLs (enables safe mfplat.dll -> mfplat_xs.dll routing without
    // affecting desktop Win32 processes).
    DllRouteDecision ResolveRoute(std::string_view dll_name,
                                  std::wstring_view shim_dir,
                                  std::wstring_view guest_dir = {},
                                  bool xbox_import_context = false) const;

    DllRouteDecision ResolveRouteW(std::wstring_view dll_name,
                                   std::wstring_view shim_dir,
                                   std::wstring_view guest_dir = {},
                                   bool xbox_import_context = false) const;

    // Returns the core built-in shim rules plus any explicit overrides ordered by (load_priority, canonical_name).
    std::vector<DllRouteRule> ListRules() const;

    // Returns all configured rules (core + extended diagnostic/media shims + overrides)
    // ordered by (load_priority, canonical_name).
    std::vector<DllRouteRule> ListAllRules() const;

    // Returns the complete routing set (core + extended + Sprint 7 fixed-return stub shims + overrides).
    std::vector<DllRouteRule> ListCompleteRules() const;

private:
    static std::string NormalizeDllKey(std::string_view raw_name);
    static std::string NormalizeCanonicalName(std::string_view raw_name);

    struct Entry {
        DllRouteRule rule;
        bool from_override = false;
        bool is_extended = false;
        bool is_fixed_stub = false;
    };
    std::unordered_map<std::string, Entry> rules_;
    std::unordered_map<std::string, std::string> aliases_;
};

Status InspectPeFile(const std::string& file_path, PeInspectionReport& out_report);
Status InspectPeBytes(const std::vector<uint8_t>& bytes,
                      const std::string& label,
                      PeInspectionReport& out_report);

// Walks IMAGE_DIRECTORY_ENTRY_BASERELOC safely (treats RVA==0 or Size==0 as absent, never reading offset 0).
Status ValidatePeBaseRelocations(const std::vector<uint8_t>& bytes,
                                 bool& out_has_relocations,
                                 uint32_t& out_block_count,
                                 uint32_t& out_entry_count);

// Validates x64 .pdata RUNTIME_FUNCTION entries and their .xdata UNWIND_INFO structures:
// checks v1/v2/v3 headers, UWOP_ALLOC_LARGE alignment & slot bounds, v2/v3 UWOP_EPILOG codes,
// and UNW_FLAG_CHAININFO chained metadata with cycle detection.
Status ValidatePeXdataUnwind(const std::vector<uint8_t>& bytes,
                             PeXdataValidationResult& out_result);

// Maps PE section characteristics (IMAGE_SCN_MEM_READ/WRITE/EXECUTE/NOT_CACHED) to Win32 PAGE_* flags.
uint32_t ComputePeSectionProtection(uint32_t section_characteristics) noexcept;

// Fault-free PE section protection transition helper: page-aligns non-empty section ranges,
// validates committed pages via VirtualQuery before VirtualProtect, and skips uncommitted/zero-size spans.
Status ApplyPeSectionProtections(void* module_base,
                                 PeSectionProtectionSummary* out_summary = nullptr);

// Parses a PE export forwarder string ("DLL.Symbol" or "DLL.#Ordinal").
bool ParseExportForwarder(std::string_view forwarder, PeForwarderTarget& out_target);

// Callback returning raw PE file bytes for a given DLL name (used for recursive forwarder resolution).
using PeModuleBytesResolver =
    std::function<Status(const std::string& dll_name, std::vector<uint8_t>& out_bytes)>;

// Resolves a PE export-forwarder chain recursively up to max_depth (default 8) with cycle detection.
Status ResolveExportForwarderChain(const std::string& start_dll,
                                   const std::string& start_symbol,
                                   uint16_t start_ordinal,
                                   bool by_ordinal,
                                   const PeModuleBytesResolver& resolver,
                                   PeForwarderResolution& out_resolution,
                                   uint32_t max_depth = 8);

// Inspects an in-memory mapped PE32+ image to detect whether `proc_name_or_ordinal`
// is a forwarded export and extracts its parsed forwarder target.
bool LookupMappedPeExportForwarder(const void* module_base,
                                   const char* proc_name_or_ordinal,
                                   PeForwarderTarget& out_target,
                                   std::string* out_raw_forwarder = nullptr);

// Reads TLS callback RVAs from an in-memory mapped PE32+ image.
bool ReadMappedPeTlsCallbackRvas(const void* module_base,
                                 std::vector<uint32_t>& out_callback_rvas);

// Helper to invoke all IMAGE_DIRECTORY_ENTRY_TLS callbacks on a mapped PE32+ image.
// Returns the number of callbacks invoked.
uint32_t InvokePeTlsCallbacks(void* module_base, uint32_t reason, void* reserved = nullptr);

// Registers an x64 .pdata RUNTIME_FUNCTION table with Windows SEH via RtlAddFunctionTable.
bool RegisterPeExceptionTable(void* module_base, uint32_t pdata_rva, uint32_t pdata_size);

// Convenience helper: reads IMAGE_DIRECTORY_ENTRY_EXCEPTION from a mapped PE32+ image and registers it.
bool RegisterPeExceptionTableFromImage(void* module_base, uint32_t* out_entry_count = nullptr);

// Unregisters an x64 .pdata RUNTIME_FUNCTION table via RtlDeleteFunctionTable.
bool UnregisterPeExceptionTable(void* module_base, uint32_t pdata_rva);

}  // namespace openscarlett
