#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/loader/pe_inspector.h"

namespace openscarlett {

enum class ForwarderResolutionKind : uint8_t {
    OpenScarlettShim = 0,
    WindowsSystemDll = 1,
    ApiSetRedirected = 2,
    NativeGuestDll = 3,
    Unresolved = 4,
    CycleError = 5,
    DepthLimitError = 6,
    MalformedError = 7,
};

struct ParsedPeForwarder {
    std::string raw_forwarder;
    std::string raw_dll_part;
    std::string dll_stem;        // Lowercase basename with trailing ".dll" stripped
    std::string normalized_dll;  // Lowercase basename with trailing ".dll" ensured
    std::string symbol_name;     // Exact export symbol case preserved (empty if by_ordinal)
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    bool is_api_set = false;
};

struct PeForwarderStep {
    std::string dll_name;
    std::string symbol_name;
    uint16_t ordinal = 0;
    bool by_ordinal = false;
    bool via_api_set = false;
    bool via_shim_alias = false;
};

struct PeForwarderChainResult {
    bool ok = false;
    ForwarderResolutionKind kind = ForwarderResolutionKind::Unresolved;
    std::string initial_forwarder;
    std::string resolved_dll;
    std::string resolved_dll_stem;
    std::string resolved_symbol;
    uint16_t resolved_ordinal = 0;
    bool by_ordinal = false;
    bool api_set_resolved = false;
    bool routed_to_openscarlett_shim = false;
    bool routed_to_windows_system = false;
    bool cycle_detected = false;
    bool depth_exceeded = false;
    std::vector<PeForwarderStep> chain;
    std::string diagnostic;
};

// Optional callback for recursive export-forwarder lookup across modules.
// Given a case-normalized DLL name (e.g. "kernelbase.dll") and symbol or ordinal,
// returns true and fills `out_next_forwarder` if that export is itself a forwarder.
using PeForwarderLookupFn = std::function<bool(const std::string& normalized_dll,
                                               const std::string& symbol_name,
                                               uint16_t ordinal,
                                               bool by_ordinal,
                                               std::string& out_next_forwarder)>;

struct PeForwarderPreflightEntry {
    std::string source_module;
    std::string input_spec;
    PeForwarderChainResult result;
};

struct PeForwarderPreflightReport {
    std::string label;
    uint32_t total_entries = 0;
    uint32_t openscarlett_shim_count = 0;
    uint32_t windows_system_count = 0;
    uint32_t apiset_redirected_count = 0;
    uint32_t native_guest_count = 0;
    uint32_t unresolved_count = 0;
    uint32_t cycle_error_count = 0;
    uint32_t malformed_error_count = 0;
    bool ready_to_launch = false;
    std::vector<PeForwarderPreflightEntry> entries;
    std::vector<std::string> blockers;
};

const char* ForwarderResolutionKindName(ForwarderResolutionKind kind) noexcept;

// Trims whitespace, extracts basename, and strips ONLY a trailing ".dll" (case-insensitive)
// without disturbing internal dots (e.g. "Microsoft.Xbox.Services.GDK.C.DLL" -> "Microsoft.Xbox.Services.GDK.C").
std::string StripDllExtension(std::string_view dll_name);

// Ensures a trailing ".dll" suffix (case-insensitive check) without double-appending ".dll.dll".
std::string EnsureDllExtension(std::string_view dll_name);

// Normalizes mixed-case DLL names (and optional paths) to lowercase canonical form,
// either preserving or stripping the ".dll" suffix.
std::string NormalizeForwarderDllName(std::string_view raw_dll_name,
                                      bool include_dll_extension = true);

// Returns true for Windows/Xbox API-set contract DLLs ("api-ms-win-*" or "ext-ms-win-*").
bool IsApiSetContractName(std::string_view dll_name) noexcept;

// Parses mixed-case PE forwarder strings including:
//   - "API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress"
//   - "KernelBase.SleepConditionVariableSRW"
//   - "KERNELX.XMemAlloc"
//   - "NTDLL.#123"
//   - "KERNEL32.DLL.CreateFileW"
//   - "Microsoft.Xbox.Services.141.GDK.C.Thunks.XblInitialize"
bool ParseMixedCaseForwarder(std::string_view forwarder, ParsedPeForwarder& out_parsed);

// Mixed-case PE export/import forwarder & API-set compatibility resolver.
// Routes Xbox/GDK contracts to OpenScarlett's 30 built-in shims, resolves Windows API-sets
// to host system DLLs, handles mixed-case/suffix variants, and catches cycles safely.
class PeForwarderResolver {
public:
    PeForwarderResolver();
    explicit PeForwarderResolver(DllRoutingTable routing_table);

    // Registers or overrides an API-set contract (exact name or prefix ending with '-').
    void RegisterApiSetContract(std::string_view contract_or_prefix, std::string_view target_dll);

    // Marks a package-local DLL as an available native guest DLL.
    void RegisterNativeGuestDll(std::string_view dll_name);

    // Resolves an API-set contract ("api-ms-win-*" / "ext-ms-win-*") to its target DLL.
    bool ResolveApiSetContract(std::string_view contract_name, std::string& out_target_dll) const;

    // Resolves a single mixed-case DLL name to its canonical OpenScarlett shim, Windows system DLL,
    // API-set target, native guest DLL, or Unresolved.
    bool ResolveTargetDll(std::string_view raw_dll_name,
                          std::string& out_canonical_dll,
                          ForwarderResolutionKind& out_kind,
                          bool& out_via_api_set,
                          bool& out_via_shim_alias) const;

    // Parses and resolves a forwarder string (recursively when `next_lookup` is supplied),
    // enforcing case-insensitive cycle detection and max_depth protection.
    PeForwarderChainResult ResolveForwarder(std::string_view forwarder_string,
                                            const PeForwarderLookupFn& next_lookup = {},
                                            uint32_t max_depth = 16u) const;

    // Resolves a PE import (dll_name + symbol/ordinal) through API-set/shim routing and optional forwarders.
    PeForwarderChainResult ResolveImportSymbol(std::string_view dll_name,
                                               std::string_view symbol_name,
                                               uint16_t ordinal = 0,
                                               bool by_ordinal = false,
                                               const PeForwarderLookupFn& next_lookup = {},
                                               uint32_t max_depth = 16u) const;

    // Produces a full preflight readiness report for all imports & forwarded exports of a PE image.
    PeForwarderPreflightReport BuildPreflightReport(const PeInspectionReport& pe_report,
                                                    const PeForwarderLookupFn& next_lookup = {},
                                                    uint32_t max_depth = 16u) const;

    // Produces a preflight readiness report for a standalone list of forwarder strings.
    PeForwarderPreflightReport BuildPreflightReportFromForwarders(
        const std::string& label,
        const std::vector<std::string>& forwarders,
        const PeForwarderLookupFn& next_lookup = {},
        uint32_t max_depth = 16u) const;

private:
    struct PrefixContract {
        std::string prefix;  // Lowercase
        std::string target_dll;
    };

    bool IsKnownWindowsSystemDll(std::string_view normalized_dll) const;
    bool ResolveConsoleLegacyAlias(std::string_view normalized_dll, std::string& out_canonical) const;
    PeForwarderChainResult ResolveChainInternal(const std::string& start_spec,
                                                const std::string& start_dll_raw,
                                                const std::string& start_symbol,
                                                uint16_t start_ordinal,
                                                bool start_by_ordinal,
                                                const PeForwarderLookupFn& next_lookup,
                                                uint32_t max_depth) const;

    DllRoutingTable routing_table_;
    std::unordered_map<std::string, std::string> exact_api_sets_;
    std::vector<PrefixContract> prefix_api_sets_;
    std::unordered_set<std::string> native_guest_dlls_;
};

std::string FormatPeForwarderPreflightText(const PeForwarderPreflightReport& report);
std::string FormatPeForwarderPreflightJson(const PeForwarderPreflightReport& report);

}  // namespace openscarlett
