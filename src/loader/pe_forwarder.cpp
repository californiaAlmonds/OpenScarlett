#include "openscarlett/loader/pe_forwarder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <utility>

namespace openscarlett {

namespace {

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string TrimAscii(std::string_view s) {
    size_t b = 0u;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1u])) != 0) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

bool EndsWithDllIgnoreCase(std::string_view s) noexcept {
    if (s.size() < 4u) {
        return false;
    }
    const size_t n = s.size();
    return s[n - 4u] == '.' &&
           (s[n - 3u] == 'd' || s[n - 3u] == 'D') &&
           (s[n - 2u] == 'l' || s[n - 2u] == 'L') &&
           (s[n - 1u] == 'l' || s[n - 1u] == 'L');
}

bool StartsWithIgnoreCase(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0u; i < prefix.size(); ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool IsValidSymbolName(std::string_view sym) noexcept {
    if (sym.empty() || sym.size() > 512u) {
        return false;
    }
    for (char c : sym) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20u || uc >= 0x7Fu || c == '.' || c == '\\' || c == '/') {
            return false;
        }
    }
    return true;
}

bool IsValidDllStem(std::string_view stem) noexcept {
    if (stem.empty() || stem.size() > 260u) {
        return false;
    }
    for (char c : stem) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20u || uc >= 0x7Fu || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

std::string EscapeJson(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8u);
    for (char c : input) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char esc[8]{};
                    std::snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned char>(c));
                    out.append(esc);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string FormatStepTarget(const std::string& dll_key,
                             const std::string& symbol,
                             uint16_t ordinal,
                             bool by_ordinal) {
    if (by_ordinal) {
        return dll_key + "!#" + std::to_string(ordinal);
    }
    return dll_key + "!" + symbol;
}

}  // namespace

const char* ForwarderResolutionKindName(ForwarderResolutionKind kind) noexcept {
    switch (kind) {
        case ForwarderResolutionKind::OpenScarlettShim: return "OpenScarlettShim";
        case ForwarderResolutionKind::WindowsSystemDll: return "WindowsSystemDll";
        case ForwarderResolutionKind::ApiSetRedirected: return "ApiSetRedirected";
        case ForwarderResolutionKind::NativeGuestDll: return "NativeGuestDll";
        case ForwarderResolutionKind::Unresolved: return "Unresolved";
        case ForwarderResolutionKind::CycleError: return "CycleError";
        case ForwarderResolutionKind::DepthLimitError: return "DepthLimitError";
        case ForwarderResolutionKind::MalformedError: return "MalformedError";
    }
    return "Unresolved";
}

std::string StripDllExtension(std::string_view dll_name) {
    std::string trimmed = TrimAscii(dll_name);
    if (trimmed.empty()) {
        return {};
    }
    const size_t slash = trimmed.find_last_of("\\/");
    if (slash != std::string::npos) {
        trimmed = trimmed.substr(slash + 1u);
    }
    if (EndsWithDllIgnoreCase(trimmed)) {
        trimmed.resize(trimmed.size() - 4u);
    }
    return trimmed;
}

std::string EnsureDllExtension(std::string_view dll_name) {
    std::string trimmed = TrimAscii(dll_name);
    if (trimmed.empty()) {
        return {};
    }
    const size_t slash = trimmed.find_last_of("\\/");
    if (slash != std::string::npos) {
        trimmed = trimmed.substr(slash + 1u);
    }
    if (trimmed.empty()) {
        return {};
    }
    if (EndsWithDllIgnoreCase(trimmed)) {
        trimmed.resize(trimmed.size() - 4u);
    }
    if (trimmed.empty()) {
        return {};
    }
    trimmed.append(".dll");
    return trimmed;
}

std::string NormalizeForwarderDllName(std::string_view raw_dll_name, bool include_dll_extension) {
    std::string stem = ToLowerAscii(StripDllExtension(raw_dll_name));
    if (stem.empty()) {
        return {};
    }
    if (include_dll_extension) {
        stem.append(".dll");
    }
    return stem;
}

bool IsApiSetContractName(std::string_view dll_name) noexcept {
    size_t b = 0u;
    while (b < dll_name.size() && std::isspace(static_cast<unsigned char>(dll_name[b])) != 0) {
        ++b;
    }
    std::string_view s = dll_name.substr(b);
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        s = s.substr(slash + 1u);
    }
    return StartsWithIgnoreCase(s, "api-ms-win-") || StartsWithIgnoreCase(s, "ext-ms-win-");
}

bool ParseMixedCaseForwarder(std::string_view forwarder, ParsedPeForwarder& out_parsed) {
    out_parsed = {};
    const std::string trimmed = TrimAscii(forwarder);
    if (trimmed.empty()) {
        return false;
    }

    const size_t last_dot = trimmed.rfind('.');
    if (last_dot == std::string::npos || last_dot == 0u || last_dot + 1u >= trimmed.size()) {
        return false;
    }

    const std::string_view raw_dll = std::string_view(trimmed).substr(0u, last_dot);
    const std::string_view raw_sym = std::string_view(trimmed).substr(last_dot + 1u);

    const std::string stem = NormalizeForwarderDllName(raw_dll, false);
    if (!IsValidDllStem(stem)) {
        return false;
    }

    out_parsed.raw_forwarder = trimmed;
    out_parsed.raw_dll_part = std::string(raw_dll);
    out_parsed.dll_stem = stem;
    out_parsed.normalized_dll = stem + ".dll";
    out_parsed.is_api_set = IsApiSetContractName(stem);

    if (raw_sym[0] == '#') {
        if (raw_sym.size() < 2u) {
            return false;
        }
        uint32_t ord = 0u;
        for (size_t i = 1u; i < raw_sym.size(); ++i) {
            if (raw_sym[i] < '0' || raw_sym[i] > '9') {
                return false;
            }
            ord = ord * 10u + static_cast<uint32_t>(raw_sym[i] - '0');
            if (ord > 0xFFFFu) {
                return false;
            }
        }
        out_parsed.by_ordinal = true;
        out_parsed.ordinal = static_cast<uint16_t>(ord);
        return true;
    }

    if (!IsValidSymbolName(raw_sym)) {
        return false;
    }
    out_parsed.by_ordinal = false;
    out_parsed.symbol_name = std::string(raw_sym);
    return true;
}

PeForwarderResolver::PeForwarderResolver() : PeForwarderResolver(DllRoutingTable()) {}

PeForwarderResolver::PeForwarderResolver(DllRoutingTable routing_table)
    : routing_table_(std::move(routing_table)) {
    // Built-in Windows 10/11 & Xbox Era/Scarlett API-set contract routing table.
    // Longer/more specific prefixes are matched before shorter prefixes.
    struct DefaultContract {
        const char* prefix;
        const char* target_dll;
    };
    static constexpr DefaultContract kDefaultContracts[] = {
        // Xbox / Gaming API-sets routed to OpenScarlett shims
        {"api-ms-win-gaming-expandedresources-", "xgameruntime.dll"},
        {"api-ms-win-gaming-tcui-", "xgameruntime.dll"},
        {"api-ms-win-gaming-", "xgameruntime.dll"},
        {"ext-ms-win-gaming-", "xgameruntime.dll"},
        {"api-ms-win-xbox-", "kernelx.dll"},
        {"ext-ms-win-xbox-", "kernelx.dll"},

        // CRT / UCRT API-sets
        {"api-ms-win-crt-", "ucrtbase.dll"},

        // Core Win32 / KernelBase / Kernel32 / Ntdll API-sets
        {"api-ms-win-core-com-", "combase.dll"},
        {"api-ms-win-core-winrt-error-", "combase.dll"},
        {"api-ms-win-core-winrt-string-", "combase.dll"},
        {"api-ms-win-core-winrt-l", "combase.dll"},
        {"api-ms-win-core-winrt-", "combase.dll"},
        {"api-ms-win-core-kernel32-legacy-", "kernel32.dll"},
        {"api-ms-win-core-kernel32-private-", "kernel32.dll"},
        {"api-ms-win-core-rtlsupport-", "ntdll.dll"},
        {"api-ms-win-core-xstate-", "kernel32.dll"},
        {"api-ms-win-core-psapi-", "kernelbase.dll"},
        {"api-ms-win-core-synch-", "kernelbase.dll"},
        {"api-ms-win-core-heap-", "kernelbase.dll"},
        {"api-ms-win-core-memory-", "kernelbase.dll"},
        {"api-ms-win-core-file-", "kernelbase.dll"},
        {"api-ms-win-core-handle-", "kernelbase.dll"},
        {"api-ms-win-core-processthreads-", "kernelbase.dll"},
        {"api-ms-win-core-libraryloader-", "kernelbase.dll"},
        {"api-ms-win-core-localization-", "kernelbase.dll"},
        {"api-ms-win-core-errorhandling-", "kernelbase.dll"},
        {"api-ms-win-core-sysinfo-", "kernelbase.dll"},
        {"api-ms-win-core-profile-", "kernelbase.dll"},
        {"api-ms-win-core-debug-", "kernelbase.dll"},
        {"api-ms-win-core-datetime-", "kernelbase.dll"},
        {"api-ms-win-core-string-", "kernelbase.dll"},
        {"api-ms-win-core-registry-", "kernelbase.dll"},
        {"api-ms-win-core-interlocked-", "kernelbase.dll"},
        {"api-ms-win-core-io-", "kernelbase.dll"},
        {"api-ms-win-core-console-", "kernelbase.dll"},
        {"api-ms-win-core-timezone-", "kernelbase.dll"},
        {"api-ms-win-core-namedpipe-", "kernelbase.dll"},
        {"api-ms-win-core-path-", "kernelbase.dll"},
        {"api-ms-win-core-util-", "kernelbase.dll"},
        {"api-ms-win-core-fibers-", "kernelbase.dll"},
        {"api-ms-win-core-threadpool-", "kernelbase.dll"},
        {"api-ms-win-core-version-", "kernelbase.dll"},
        {"api-ms-win-core-apiquery-", "ntdll.dll"},
        {"api-ms-win-core-delayload-", "kernelbase.dll"},
        {"api-ms-win-core-winsock-", "ws2_32.dll"},
        {"api-ms-win-core-", "kernelbase.dll"},

        // Security, LSA, Services & Eventing API-sets
        {"api-ms-win-security-lsalookup-", "sechost.dll"},
        {"api-ms-win-security-lsapolicy-", "sechost.dll"},
        {"api-ms-win-security-base-", "kernelbase.dll"},
        {"api-ms-win-security-sddl-", "sechost.dll"},
        {"api-ms-win-security-", "advapi32.dll"},
        {"api-ms-win-service-", "sechost.dll"},
        {"api-ms-win-eventing-", "sechost.dll"},

        // UI / Graphics / Shell / Multimedia / Downlevel API-sets
        {"api-ms-win-ntuser-", "user32.dll"},
        {"ext-ms-win-ntuser-", "user32.dll"},
        {"api-ms-win-rtcore-ntuser-", "user32.dll"},
        {"ext-ms-win-rtcore-ntuser-", "user32.dll"},
        {"api-ms-win-gdi-", "gdi32.dll"},
        {"ext-ms-win-gdi-", "gdi32.dll"},
        {"api-ms-win-rtcore-gdi-", "gdi32.dll"},
        {"ext-ms-win-rtcore-gdi-", "gdi32.dll"},
        {"api-ms-win-dx-", "dxgi.dll"},
        {"ext-ms-win-dx-", "dxgi.dll"},
        {"api-ms-win-mm-", "winmm.dll"},
        {"ext-ms-win-mm-", "winmm.dll"},
        {"api-ms-win-shcore-", "shcore.dll"},
        {"api-ms-win-shell-", "shell32.dll"},
        {"api-ms-win-networking-", "ws2_32.dll"},
        {"api-ms-win-downlevel-shlwapi-", "shlwapi.dll"},
        {"api-ms-win-downlevel-ole32-", "ole32.dll"},
        {"api-ms-win-downlevel-advapi32-", "advapi32.dll"},
        {"api-ms-win-downlevel-normaliz-", "normaliz.dll"},
        {"api-ms-win-downlevel-user32-", "user32.dll"},
        {"api-ms-win-downlevel-version-", "version.dll"},
        {"api-ms-win-downlevel-kernel32-", "kernel32.dll"},
    };

    for (const auto& entry : kDefaultContracts) {
        RegisterApiSetContract(entry.prefix, entry.target_dll);
    }
}

void PeForwarderResolver::RegisterApiSetContract(std::string_view contract_or_prefix,
                                                 std::string_view target_dll) {
    const std::string key = NormalizeForwarderDllName(contract_or_prefix, false);
    const std::string target = EnsureDllExtension(target_dll);
    if (key.empty() || target.empty()) {
        return;
    }
    if (key.back() == '-') {
        for (auto& existing : prefix_api_sets_) {
            if (existing.prefix == key) {
                existing.target_dll = target;
                return;
            }
        }
        prefix_api_sets_.push_back(PrefixContract{key, target});
        std::sort(prefix_api_sets_.begin(), prefix_api_sets_.end(),
                  [](const PrefixContract& a, const PrefixContract& b) {
                      if (a.prefix.size() != b.prefix.size()) {
                          return a.prefix.size() > b.prefix.size();
                      }
                      return a.prefix < b.prefix;
                  });
    } else {
        exact_api_sets_[key] = target;
    }
}

void PeForwarderResolver::RegisterNativeGuestDll(std::string_view dll_name) {
    const std::string norm = NormalizeForwarderDllName(dll_name, true);
    if (!norm.empty()) {
        native_guest_dlls_.insert(norm);
    }
}

bool PeForwarderResolver::ResolveApiSetContract(std::string_view contract_name,
                                                std::string& out_target_dll) const {
    out_target_dll.clear();
    const std::string stem = NormalizeForwarderDllName(contract_name, false);
    if (!IsApiSetContractName(stem)) {
        return false;
    }

    auto exact_it = exact_api_sets_.find(stem);
    if (exact_it != exact_api_sets_.end()) {
        out_target_dll = exact_it->second;
        return true;
    }

    for (const auto& entry : prefix_api_sets_) {
        if (stem.size() >= entry.prefix.size() &&
            stem.compare(0u, entry.prefix.size(), entry.prefix) == 0) {
            out_target_dll = entry.target_dll;
            return true;
        }
    }
    return false;
}

bool PeForwarderResolver::IsKnownWindowsSystemDll(std::string_view normalized_dll) const {
    if (IsWindowsKnownDll(normalized_dll)) {
        return true;
    }
    if (ClassifyImportDll(std::string(normalized_dll)) == DllCategory::WindowsNative) {
        return true;
    }
    static const std::unordered_set<std::string> kExtraWindowsSystemDlls = {
        "version.dll", "winhttp.dll", "wininet.dll", "iphlpapi.dll", "secur32.dll",
        "sspicli.dll", "netapi32.dll", "powrprof.dll", "avrt.dll", "mmdevapi.dll",
        "dxgi.dll", "d3d12.dll", "d3d11.dll", "d3dcompiler_47.dll", "xinput1_4.dll",
        "xinput9_1_0.dll", "dwmapi.dll", "uxtheme.dll", "wtsapi32.dll", "userenv.dll",
        "propsys.dll", "wintrust.dll", "ncrypt.dll", "bcryptprimitives.dll", "mswsock.dll",
        "dnsapi.dll", "dhcpcsvc.dll", "cryptsp.dll", "rsaenh.dll", "cfgmgr32.dll",
        "windowscodecs.dll", "mfreadwrite.dll", "mfuuid.dll", "evr.dll",
        "msvcp140_1.dll", "msvcp140_2.dll", "concrt140.dll", "vcomp140.dll",
        "ntoskrnl.exe",
    };
    return kExtraWindowsSystemDlls.find(std::string(normalized_dll)) != kExtraWindowsSystemDlls.end();
}

bool PeForwarderResolver::ResolveConsoleLegacyAlias(std::string_view normalized_dll,
                                                    std::string& out_canonical) const {
    out_canonical.clear();
    if (normalized_dll == "d3d12_x.dll") {
        out_canonical = "d3d12_xs.dll";
        return true;
    }
    if (normalized_dll == "xg.dll") {
        out_canonical = "xg_xs.dll";
        return true;
    }
    if (normalized_dll == "dstorage.dll") {
        out_canonical = "dstorage_xs.dll";
        return true;
    }
    if (normalized_dll == "xgamingruntimethunks.dll") {
        out_canonical = "xgameruntime.dll";
        return true;
    }
    return ResolveShimAlias(normalized_dll, out_canonical);
}

bool PeForwarderResolver::ResolveTargetDll(std::string_view raw_dll_name,
                                           std::string& out_canonical_dll,
                                           ForwarderResolutionKind& out_kind,
                                           bool& out_via_api_set,
                                           bool& out_via_shim_alias) const {
    out_canonical_dll.clear();
    out_kind = ForwarderResolutionKind::Unresolved;
    out_via_api_set = false;
    out_via_shim_alias = false;

    std::string norm = NormalizeForwarderDllName(raw_dll_name, true);
    if (norm.empty()) {
        out_kind = ForwarderResolutionKind::MalformedError;
        return false;
    }

    // 1. API-set contract redirection (api-ms-win-* / ext-ms-win-*)
    if (IsApiSetContractName(norm)) {
        std::string redirected;
        if (!ResolveApiSetContract(norm, redirected)) {
            out_canonical_dll = norm;
            out_kind = ForwarderResolutionKind::Unresolved;
            return false;
        }
        out_via_api_set = true;
        norm = NormalizeForwarderDllName(redirected, true);
        // Check whether the API-set target is an OpenScarlett shim (e.g. api-ms-win-gaming-* -> xgameruntime.dll)
        if (const DllRouteRule* rule = routing_table_.FindRule(norm)) {
            if (rule->policy == DllRoutePolicy::Shim) {
                out_canonical_dll = rule->canonical_name;
                out_kind = ForwarderResolutionKind::OpenScarlettShim;
                return true;
            }
        }
        out_canonical_dll = redirected;
        out_kind = ForwarderResolutionKind::ApiSetRedirected;
        return true;
    }

    // 2. Direct OpenScarlett 30-shim routing table match
    if (const DllRouteRule* rule = routing_table_.FindRule(norm)) {
        out_canonical_dll = rule->canonical_name;
        if (rule->policy == DllRoutePolicy::Shim) {
            out_via_shim_alias = (NormalizeForwarderDllName(rule->canonical_name, true) != norm);
            out_kind = ForwarderResolutionKind::OpenScarlettShim;
            return true;
        }
        if (rule->policy == DllRoutePolicy::NativeGuestDll) {
            out_kind = ForwarderResolutionKind::NativeGuestDll;
            return true;
        }
        out_kind = ForwarderResolutionKind::WindowsSystemDll;
        return true;
    }

    // 3. Console legacy / versioned thunk aliases (d3d12_x -> d3d12_xs, xg -> xg_xs, etc.)
    std::string alias_target;
    if (ResolveConsoleLegacyAlias(norm, alias_target)) {
        out_via_shim_alias = true;
        if (const DllRouteRule* rule = routing_table_.FindRule(alias_target)) {
            out_canonical_dll = rule->canonical_name;
            out_kind = (rule->policy == DllRoutePolicy::Shim)
                           ? ForwarderResolutionKind::OpenScarlettShim
                           : ForwarderResolutionKind::WindowsSystemDll;
            return true;
        }
        out_canonical_dll = alias_target;
        out_kind = ForwarderResolutionKind::OpenScarlettShim;
        return true;
    }

    // 4. Registered native guest DLLs (middleware / package-local DLLs)
    if (native_guest_dlls_.find(norm) != native_guest_dlls_.end()) {
        out_canonical_dll = norm;
        out_kind = ForwarderResolutionKind::NativeGuestDll;
        return true;
    }

    // 5. Known host Windows system DLLs
    if (IsKnownWindowsSystemDll(norm)) {
        out_canonical_dll = norm;
        out_kind = ForwarderResolutionKind::WindowsSystemDll;
        return true;
    }

    out_canonical_dll = norm;
    out_kind = ForwarderResolutionKind::Unresolved;
    return false;
}

PeForwarderChainResult PeForwarderResolver::ResolveChainInternal(
    const std::string& start_spec,
    const std::string& start_dll_raw,
    const std::string& start_symbol,
    uint16_t start_ordinal,
    bool start_by_ordinal,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    PeForwarderChainResult res{};
    res.initial_forwarder = start_spec;

    if (max_depth == 0u) {
        max_depth = 1u;
    }

    std::unordered_set<std::string> visited;
    std::string cur_dll_raw = start_dll_raw;
    std::string cur_symbol = start_symbol;
    uint16_t cur_ordinal = start_ordinal;
    bool cur_by_ordinal = start_by_ordinal;
    bool any_api_set = false;

    for (uint32_t depth = 0u; depth <= max_depth; ++depth) {
        const std::string cur_dll_key = NormalizeForwarderDllName(cur_dll_raw, true);
        if (cur_dll_key.empty()) {
            res.ok = false;
            res.kind = ForwarderResolutionKind::MalformedError;
            res.diagnostic = "Empty or malformed DLL name in forwarder chain";
            return res;
        }

        const std::string visit_key = FormatStepTarget(cur_dll_key, cur_symbol, cur_ordinal, cur_by_ordinal);
        if (!visited.insert(visit_key).second) {
            res.ok = false;
            res.kind = ForwarderResolutionKind::CycleError;
            res.cycle_detected = true;
            res.diagnostic = "Circular forwarder chain detected at " + visit_key;
            return res;
        }

        std::string canonical_dll;
        ForwarderResolutionKind step_kind = ForwarderResolutionKind::Unresolved;
        bool via_api_set = false;
        bool via_shim_alias = false;
        const bool resolved_target = ResolveTargetDll(
            cur_dll_raw, canonical_dll, step_kind, via_api_set, via_shim_alias);

        if (via_api_set) {
            any_api_set = true;
        }

        res.chain.push_back(PeForwarderStep{
            canonical_dll.empty() ? cur_dll_key : canonical_dll,
            cur_symbol,
            cur_ordinal,
            cur_by_ordinal,
            via_api_set,
            via_shim_alias,
        });

        // Also guard against cycles that hop through an API-set or alias back to an already-seen canonical target.
        const std::string canonical_key = NormalizeForwarderDllName(canonical_dll, true);
        if (!canonical_key.empty() && canonical_key != cur_dll_key) {
            const std::string canonical_visit =
                FormatStepTarget(canonical_key, cur_symbol, cur_ordinal, cur_by_ordinal);
            if (!visited.insert(canonical_visit).second) {
                res.ok = false;
                res.kind = ForwarderResolutionKind::CycleError;
                res.cycle_detected = true;
                res.diagnostic = "Circular forwarder chain detected after canonicalization at " +
                                 canonical_visit;
                return res;
            }
        }

        // Check if caller's export table marks this (dll, symbol/ordinal) as forwarded further.
        std::string next_forwarder;
        bool has_next = false;
        if (next_lookup) {
            has_next = next_lookup(cur_dll_key, cur_symbol, cur_ordinal, cur_by_ordinal, next_forwarder);
            if (!has_next && !canonical_key.empty() && canonical_key != cur_dll_key) {
                has_next = next_lookup(canonical_key, cur_symbol, cur_ordinal, cur_by_ordinal, next_forwarder);
            }
        }

        if (!has_next) {
            res.resolved_dll = canonical_dll.empty() ? cur_dll_key : canonical_dll;
            res.resolved_dll_stem = NormalizeForwarderDllName(res.resolved_dll, false);
            res.resolved_symbol = cur_symbol;
            res.resolved_ordinal = cur_ordinal;
            res.by_ordinal = cur_by_ordinal;
            res.api_set_resolved = any_api_set;
            res.kind = step_kind;
            res.routed_to_openscarlett_shim = (step_kind == ForwarderResolutionKind::OpenScarlettShim);
            res.routed_to_windows_system =
                (step_kind == ForwarderResolutionKind::WindowsSystemDll ||
                 step_kind == ForwarderResolutionKind::ApiSetRedirected);
            res.ok = resolved_target;
            if (res.ok) {
                res.diagnostic = "Resolved to " + res.resolved_dll + " (" +
                                 ForwarderResolutionKindName(res.kind) + ")";
            } else {
                res.diagnostic = "Unresolved DLL target: " + res.resolved_dll;
            }
            return res;
        }

        if (depth == max_depth) {
            res.ok = false;
            res.kind = ForwarderResolutionKind::DepthLimitError;
            res.depth_exceeded = true;
            res.diagnostic = "Forwarder chain exceeded max_depth=" + std::to_string(max_depth);
            return res;
        }

        ParsedPeForwarder parsed_next{};
        if (!ParseMixedCaseForwarder(next_forwarder, parsed_next)) {
            res.ok = false;
            res.kind = ForwarderResolutionKind::MalformedError;
            res.diagnostic = "Malformed chained forwarder string: " + next_forwarder;
            return res;
        }
        cur_dll_raw = parsed_next.normalized_dll;
        cur_symbol = parsed_next.symbol_name;
        cur_ordinal = parsed_next.ordinal;
        cur_by_ordinal = parsed_next.by_ordinal;
    }

    res.ok = false;
    res.kind = ForwarderResolutionKind::DepthLimitError;
    res.depth_exceeded = true;
    res.diagnostic = "Forwarder chain failed to terminate";
    return res;
}

PeForwarderChainResult PeForwarderResolver::ResolveForwarder(
    std::string_view forwarder_string,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    ParsedPeForwarder parsed{};
    if (!ParseMixedCaseForwarder(forwarder_string, parsed)) {
        PeForwarderChainResult err{};
        err.ok = false;
        err.kind = ForwarderResolutionKind::MalformedError;
        err.initial_forwarder = std::string(forwarder_string);
        err.diagnostic = "Malformed PE forwarder string: '" + std::string(forwarder_string) + "'";
        return err;
    }
    return ResolveChainInternal(parsed.raw_forwarder,
                                parsed.normalized_dll,
                                parsed.symbol_name,
                                parsed.ordinal,
                                parsed.by_ordinal,
                                next_lookup,
                                max_depth);
}

PeForwarderChainResult PeForwarderResolver::ResolveImportSymbol(
    std::string_view dll_name,
    std::string_view symbol_name,
    uint16_t ordinal,
    bool by_ordinal,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    const std::string norm_dll = NormalizeForwarderDllName(dll_name, true);
    if (norm_dll.empty() || (!by_ordinal && !IsValidSymbolName(symbol_name))) {
        PeForwarderChainResult err{};
        err.ok = false;
        err.kind = ForwarderResolutionKind::MalformedError;
        err.initial_forwarder = std::string(dll_name) + "." +
                                (by_ordinal ? ("#" + std::to_string(ordinal)) : std::string(symbol_name));
        err.diagnostic = "Invalid PE import specification";
        return err;
    }
    const std::string spec = FormatStepTarget(norm_dll, std::string(symbol_name), ordinal, by_ordinal);
    return ResolveChainInternal(spec,
                                norm_dll,
                                by_ordinal ? std::string{} : std::string(symbol_name),
                                ordinal,
                                by_ordinal,
                                next_lookup,
                                max_depth);
}

PeForwarderPreflightReport PeForwarderResolver::BuildPreflightReport(
    const PeInspectionReport& pe_report,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    PeForwarderPreflightReport out{};
    out.label = pe_report.file_path;

    auto tally = [&](const std::string& src_mod,
                     const std::string& spec,
                     PeForwarderChainResult r,
                     bool is_delay_loaded) {
        ++out.total_entries;
        if (is_delay_loaded) {
            ++out.delay_load_count;
            if (r.kind == ForwarderResolutionKind::OpenScarlettShim) {
                ++out.delay_load_shim_count;
            }
            const std::string norm_dl = NormalizeForwarderDllName(src_mod, true);
            if (!norm_dl.empty() &&
                std::find(out.delay_load_dlls.begin(), out.delay_load_dlls.end(), norm_dl) ==
                    out.delay_load_dlls.end()) {
                out.delay_load_dlls.push_back(norm_dl);
            }
        }
        switch (r.kind) {
            case ForwarderResolutionKind::OpenScarlettShim:
                ++out.openscarlett_shim_count;
                if (r.api_set_resolved) ++out.apiset_redirected_count;
                break;
            case ForwarderResolutionKind::WindowsSystemDll:
                ++out.windows_system_count;
                break;
            case ForwarderResolutionKind::ApiSetRedirected:
                ++out.apiset_redirected_count;
                ++out.windows_system_count;
                break;
            case ForwarderResolutionKind::NativeGuestDll:
                ++out.native_guest_count;
                break;
            case ForwarderResolutionKind::Unresolved:
                ++out.unresolved_count;
                out.blockers.push_back("Unresolved: " + spec + " -> " + r.resolved_dll);
                break;
            case ForwarderResolutionKind::CycleError:
            case ForwarderResolutionKind::DepthLimitError:
                ++out.cycle_error_count;
                out.blockers.push_back(r.diagnostic);
                break;
            case ForwarderResolutionKind::MalformedError:
                ++out.malformed_error_count;
                out.blockers.push_back(r.diagnostic);
                break;
        }
        out.entries.push_back(PeForwarderPreflightEntry{src_mod, spec, std::move(r), is_delay_loaded});
    };

    for (const auto& mod : pe_report.imports) {
        if (mod.symbols.empty()) {
            auto r = ResolveImportSymbol(mod.dll_name, "<module>", 0u, true, next_lookup, max_depth);
            tally(mod.dll_name, mod.dll_name, std::move(r), false);
            continue;
        }
        for (const auto& sym : mod.symbols) {
            auto r = ResolveImportSymbol(mod.dll_name, sym.name, sym.ordinal, sym.by_ordinal,
                                         next_lookup, max_depth);
            const std::string spec =
                mod.dll_name + "!" + (sym.by_ordinal ? ("#" + std::to_string(sym.ordinal)) : sym.name);
            tally(mod.dll_name, spec, std::move(r), sym.delay_loaded);
        }
    }

    for (const auto& exp : pe_report.exports) {
        if (!exp.is_forwarded) {
            continue;
        }
        auto r = ResolveForwarder(exp.forwarder, next_lookup, max_depth);
        tally("<export_forwarder>", exp.forwarder, std::move(r), false);
    }

    out.ready_to_launch = (out.unresolved_count == 0u &&
                           out.cycle_error_count == 0u &&
                           out.malformed_error_count == 0u);
    return out;
}

std::vector<PeForwarderPreflightEntry> PeForwarderResolver::InspectDelayLoadImports(
    const PeInspectionReport& pe_report,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    std::vector<PeForwarderPreflightEntry> results;
    for (const auto& mod : pe_report.imports) {
        for (const auto& sym : mod.symbols) {
            if (!sym.delay_loaded) {
                continue;
            }
            auto r = ResolveImportSymbol(mod.dll_name, sym.name, sym.ordinal, sym.by_ordinal,
                                         next_lookup, max_depth);
            const std::string spec =
                mod.dll_name + "!" + (sym.by_ordinal ? ("#" + std::to_string(sym.ordinal)) : sym.name);
            results.push_back(PeForwarderPreflightEntry{mod.dll_name, spec, std::move(r), true});
        }
    }
    return results;
}

PeForwarderPreflightReport PeForwarderResolver::BuildPreflightReportFromForwarders(
    const std::string& label,
    const std::vector<std::string>& forwarders,
    const PeForwarderLookupFn& next_lookup,
    uint32_t max_depth) const {
    PeForwarderPreflightReport out{};
    out.label = label;

    for (const auto& fwd : forwarders) {
        auto r = ResolveForwarder(fwd, next_lookup, max_depth);
        ++out.total_entries;
        switch (r.kind) {
            case ForwarderResolutionKind::OpenScarlettShim:
                ++out.openscarlett_shim_count;
                if (r.api_set_resolved) ++out.apiset_redirected_count;
                break;
            case ForwarderResolutionKind::WindowsSystemDll:
                ++out.windows_system_count;
                break;
            case ForwarderResolutionKind::ApiSetRedirected:
                ++out.apiset_redirected_count;
                ++out.windows_system_count;
                break;
            case ForwarderResolutionKind::NativeGuestDll:
                ++out.native_guest_count;
                break;
            case ForwarderResolutionKind::Unresolved:
                ++out.unresolved_count;
                out.blockers.push_back("Unresolved: " + fwd + " -> " + r.resolved_dll);
                break;
            case ForwarderResolutionKind::CycleError:
            case ForwarderResolutionKind::DepthLimitError:
                ++out.cycle_error_count;
                out.blockers.push_back(r.diagnostic);
                break;
            case ForwarderResolutionKind::MalformedError:
                ++out.malformed_error_count;
                out.blockers.push_back(r.diagnostic);
                break;
        }
        out.entries.push_back(PeForwarderPreflightEntry{"<forwarder>", fwd, std::move(r), false});
    }

    out.ready_to_launch = (out.unresolved_count == 0u &&
                           out.cycle_error_count == 0u &&
                           out.malformed_error_count == 0u);
    return out;
}

std::string FormatPeForwarderPreflightText(const PeForwarderPreflightReport& report) {
    std::ostringstream oss;
    oss << "PE Forwarder & Import Preflight: " << report.label << "\n"
        << "  ReadyToLaunch    : " << (report.ready_to_launch ? "YES" : "NO") << "\n"
        << "  TotalEntries     : " << report.total_entries << "\n"
        << "  OpenScarlettShim : " << report.openscarlett_shim_count << "\n"
        << "  WindowsSystem    : " << report.windows_system_count
        << " (ApiSetRedirected=" << report.apiset_redirected_count << ")\n"
        << "  NativeGuestDll   : " << report.native_guest_count << "\n"
        << "  DelayLoadEntries : " << report.delay_load_count
        << " (Shims=" << report.delay_load_shim_count << ")\n"
        << "  Unresolved       : " << report.unresolved_count
        << " | Cycles=" << report.cycle_error_count
        << " | Malformed=" << report.malformed_error_count << "\n";
    if (!report.blockers.empty()) {
        oss << "  Blockers:\n";
        for (const auto& b : report.blockers) {
            oss << "    - " << b << "\n";
        }
    }
    return oss.str();
}

std::string FormatPeForwarderPreflightJson(const PeForwarderPreflightReport& report) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"label\": \"" << EscapeJson(report.label) << "\",\n"
        << "  \"ready_to_launch\": " << (report.ready_to_launch ? "true" : "false") << ",\n"
        << "  \"total_entries\": " << report.total_entries << ",\n"
        << "  \"openscarlett_shim_count\": " << report.openscarlett_shim_count << ",\n"
        << "  \"windows_system_count\": " << report.windows_system_count << ",\n"
        << "  \"apiset_redirected_count\": " << report.apiset_redirected_count << ",\n"
        << "  \"native_guest_count\": " << report.native_guest_count << ",\n"
        << "  \"delay_load_count\": " << report.delay_load_count << ",\n"
        << "  \"delay_load_shim_count\": " << report.delay_load_shim_count << ",\n"
        << "  \"unresolved_count\": " << report.unresolved_count << ",\n"
        << "  \"cycle_error_count\": " << report.cycle_error_count << ",\n"
        << "  \"malformed_error_count\": " << report.malformed_error_count << ",\n"
        << "  \"delay_load_dlls\": [";
    for (size_t i = 0u; i < report.delay_load_dlls.size(); ++i) {
        if (i > 0u) oss << ", ";
        oss << "\"" << EscapeJson(report.delay_load_dlls[i]) << "\"";
    }
    oss << "],\n  \"blockers\": [";
    for (size_t i = 0u; i < report.blockers.size(); ++i) {
        if (i > 0u) oss << ", ";
        oss << "\"" << EscapeJson(report.blockers[i]) << "\"";
    }
    oss << "]\n}\n";
    return oss.str();
}

}  // namespace openscarlett
