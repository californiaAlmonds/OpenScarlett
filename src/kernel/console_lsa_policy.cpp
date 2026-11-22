#include "openscarlett/kernel/console_lsa_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace openscarlett {

namespace {

constexpr uint64_t kFnvOffset64 = 14695981039346656037ull;
constexpr uint64_t kFnvPrime64 = 1099511628211ull;

uint64_t Fnv1a64(std::string_view data, uint64_t seed = kFnvOffset64) noexcept {
    uint64_t h = seed;
    for (char c : data) {
        h ^= static_cast<uint8_t>(c);
        h *= kFnvPrime64;
    }
    return h;
}

uint64_t SplitMix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30u)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27u)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31u);
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string ToUpperAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
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

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0u; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string FormatTitleIdHex(uint32_t title_id) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%08X", title_id);
    return std::string(buf);
}

ConsoleSid BuildConsoleSid(uint64_t authority, const std::vector<uint32_t>& sub_auths) {
    ConsoleSid sid{};
    sid.revision = 1u;
    sid.authority = authority & 0x0000FFFFFFFFFFFFull;
    sid.sub_authorities = sub_auths;

    // Text format: S-<Rev>-<Authority>-<Sub0>-...
    std::ostringstream oss;
    oss << "S-" << static_cast<unsigned>(sid.revision) << "-" << sid.authority;
    for (uint32_t sa : sub_auths) {
        oss << "-" << sa;
    }
    sid.text = oss.str();

    // Standard Windows binary SID format:
    //   byte 0     : Revision (1)
    //   byte 1     : SubAuthorityCount
    //   bytes 2..7 : 48-bit big-endian IdentifierAuthority
    //   bytes 8..  : N little-endian 32-bit SubAuthorities
    const uint8_t sub_count = static_cast<uint8_t>(std::min<size_t>(sub_auths.size(), 15u));
    sid.binary.resize(8u + static_cast<size_t>(sub_count) * 4u, 0u);
    sid.binary[0] = sid.revision;
    sid.binary[1] = sub_count;
    for (int i = 0; i < 6; ++i) {
        const unsigned shift = static_cast<unsigned>((5 - i) * 8);
        sid.binary[static_cast<size_t>(2 + i)] = static_cast<uint8_t>((sid.authority >> shift) & 0xFFu);
    }
    for (uint8_t i = 0u; i < sub_count; ++i) {
        const uint32_t sa = sub_auths[i];
        std::memcpy(sid.binary.data() + 8u + static_cast<size_t>(i) * 4u, &sa, sizeof(sa));
    }
    return sid;
}

std::string DeriveDeterministicGuid(std::string_view seed) {
    const uint64_t h0 = SplitMix64(Fnv1a64(seed, 0x5CA0A1B2C3D4E5F6ull));
    const uint64_t h1 = SplitMix64(h0 ^ Fnv1a64(seed, 0x1029384756AFBECDull));

    uint8_t b[16]{};
    std::memcpy(b + 0, &h0, 8u);
    std::memcpy(b + 8, &h1, 8u);
    // RFC 4122 version 4 / variant 1 shape for well-formed deterministic UUID.
    b[6] = static_cast<uint8_t>((b[6] & 0x0Fu) | 0x40u);
    b[8] = static_cast<uint8_t>((b[8] & 0x3Fu) | 0x80u);

    char out[40]{};
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out);
}

std::string DeriveDeterministicStoreId(std::string_view seed) {
    // 12-char Microsoft Store ID format starting with '9' + 11 uppercase Base36 chars.
    static constexpr char kAlpha[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
    constexpr size_t kAlphaLen = sizeof(kAlpha) - 1u;
    uint64_t h = SplitMix64(Fnv1a64(seed, 0x9F8E7D6C5B4A3210ull));
    std::string out(12u, '9');
    for (size_t i = 1u; i < 12u; ++i) {
        h = SplitMix64(h ^ static_cast<uint64_t>(i * 0x517CC1B727220A95ull));
        out[i] = kAlpha[h % kAlphaLen];
    }
    return out;
}

std::string DeriveDeterministicConsoleId(std::string_view seed) {
    const uint64_t h0 = SplitMix64(Fnv1a64(seed, 0xA11CE5CA1E000001ull));
    const uint64_t h1 = SplitMix64(h0 ^ 0xDEADBEEFCAFEBABEull);
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%08X.%08X.%08X.%08X",
                  static_cast<uint32_t>(h0 >> 32u),
                  static_cast<uint32_t>(h0 & 0xFFFFFFFFu),
                  static_cast<uint32_t>(h1 >> 32u),
                  static_cast<uint32_t>(h1 & 0xFFFFFFFFu));
    return std::string(buf);
}

// Lightweight helper to extract a simple XML element text value (case-insensitive tag name).
bool ExtractSimpleXmlTagText(std::string_view xml, std::string_view tag_name, std::string& out_value) {
    out_value.clear();
    const std::string xml_lo = ToLowerAscii(xml);
    const std::string open_pat = "<" + ToLowerAscii(tag_name);
    const std::string close_pat = "</" + ToLowerAscii(tag_name) + ">";

    size_t pos = 0u;
    while ((pos = xml_lo.find(open_pat, pos)) != std::string::npos) {
        const size_t after = pos + open_pat.size();
        if (after >= xml_lo.size()) {
            break;
        }
        const char next_ch = xml_lo[after];
        if (next_ch != '>' && next_ch != ' ' && next_ch != '\t' && next_ch != '\r' && next_ch != '\n') {
            pos = after;
            continue;
        }
        const size_t gt = xml_lo.find('>', after);
        if (gt == std::string::npos) {
            break;
        }
        if (gt > after && xml_lo[gt - 1u] == '/') {
            pos = gt + 1u;
            continue;
        }
        const size_t close_pos = xml_lo.find(close_pat, gt + 1u);
        if (close_pos == std::string::npos) {
            break;
        }
        out_value = TrimAscii(xml.substr(gt + 1u, close_pos - (gt + 1u)));
        return true;
    }
    return false;
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

constexpr ConsoleCapability kAllCapabilities[] = {
    ConsoleCapability::AppContainer,
    ConsoleCapability::XboxLive,
    ConsoleCapability::TitleStorage,
    ConsoleCapability::GameSave,
    ConsoleCapability::Multiplayer,
    ConsoleCapability::InternetClient,
    ConsoleCapability::PrivateNetworkClientServer,
    ConsoleCapability::PersistentLocalStorage,
    ConsoleCapability::SpatialAudio,
    ConsoleCapability::DirectXHardware,
    ConsoleCapability::GameInput,
    ConsoleCapability::DebugTooling,
};

}  // namespace

const char* ConsoleCapabilityName(ConsoleCapability cap) noexcept {
    switch (cap) {
        case ConsoleCapability::AppContainer: return "AppContainer";
        case ConsoleCapability::XboxLive: return "XboxLive";
        case ConsoleCapability::TitleStorage: return "TitleStorage";
        case ConsoleCapability::GameSave: return "GameSave";
        case ConsoleCapability::Multiplayer: return "Multiplayer";
        case ConsoleCapability::InternetClient: return "InternetClient";
        case ConsoleCapability::PrivateNetworkClientServer: return "PrivateNetworkClientServer";
        case ConsoleCapability::PersistentLocalStorage: return "PersistentLocalStorage";
        case ConsoleCapability::SpatialAudio: return "SpatialAudio";
        case ConsoleCapability::DirectXHardware: return "DirectXHardware";
        case ConsoleCapability::GameInput: return "GameInput";
        case ConsoleCapability::DebugTooling: return "DebugTooling";
    }
    return "Unknown";
}

bool ParseConsoleCapability(std::string_view name, ConsoleCapability& out_cap) noexcept {
    const std::string lo = ToLowerAscii(TrimAscii(name));
    if (lo == "appcontainer" || lo == "app_container") {
        out_cap = ConsoleCapability::AppContainer;
        return true;
    }
    if (lo == "xboxlive" || lo == "xbox_live" || lo == "xboxliveauthentication") {
        out_cap = ConsoleCapability::XboxLive;
        return true;
    }
    if (lo == "titlestorage" || lo == "title_storage") {
        out_cap = ConsoleCapability::TitleStorage;
        return true;
    }
    if (lo == "gamesave" || lo == "game_save" || lo == "cloudsavedgames") {
        out_cap = ConsoleCapability::GameSave;
        return true;
    }
    if (lo == "multiplayer" || lo == "xboxlivemultiplayer") {
        out_cap = ConsoleCapability::Multiplayer;
        return true;
    }
    if (lo == "internetclient" || lo == "internet_client" || lo == "internetclientserver") {
        out_cap = ConsoleCapability::InternetClient;
        return true;
    }
    if (lo == "privatenetworkclientserver" || lo == "private_network") {
        out_cap = ConsoleCapability::PrivateNetworkClientServer;
        return true;
    }
    if (lo == "persistentlocalstorage" || lo == "persistent_local_storage" || lo == "pls") {
        out_cap = ConsoleCapability::PersistentLocalStorage;
        return true;
    }
    if (lo == "spatialaudio" || lo == "spatial_audio") {
        out_cap = ConsoleCapability::SpatialAudio;
        return true;
    }
    if (lo == "directxhardware" || lo == "directx_hardware" || lo == "gpu") {
        out_cap = ConsoleCapability::DirectXHardware;
        return true;
    }
    if (lo == "gameinput" || lo == "game_input") {
        out_cap = ConsoleCapability::GameInput;
        return true;
    }
    if (lo == "debugtooling" || lo == "debug_tooling" || lo == "developmentonly") {
        out_cap = ConsoleCapability::DebugTooling;
        return true;
    }
    return false;
}

std::string ComputeAppXPublisherId(std::string_view publisher) {
    // Microsoft AppX PublisherId alphabet: 32-char Crockford-style lowercase Base32 (excludes i, l, o, u).
    static constexpr char kAppXBase32[] = "0123456789abcdefghjkmnpqrstvwxyz";
    const std::string normalized = TrimAscii(publisher);
    uint64_t h0 = SplitMix64(Fnv1a64(normalized, 0x4150505850554231ull));
    uint64_t h1 = SplitMix64(h0 ^ 0x7075626C69736865ull);

    // Encode 65 deterministic bits (13 x 5-bit groups) into a 13-char PublisherId.
    std::string out;
    out.resize(13u);
    for (size_t i = 0u; i < 13u; ++i) {
        uint32_t bits = 0u;
        if (i < 12u) {
            bits = static_cast<uint32_t>((h0 >> (i * 5u)) & 0x1Fu);
        } else {
            bits = static_cast<uint32_t>(((h0 >> 60u) & 0x0Fu) | ((h1 & 0x01u) << 4u));
        }
        out[i] = kAppXBase32[bits];
    }
    return out;
}

ConsoleSid DeriveDeterministicMachineSid(std::string_view machine_seed) {
    const std::string norm = ToUpperAscii(TrimAscii(machine_seed));
    uint64_t h0 = SplitMix64(Fnv1a64(norm, 0x534341524C455454ull));
    uint64_t h1 = SplitMix64(h0 ^ 0x4D414348494E4531ull);

    const uint32_t a1 = static_cast<uint32_t>(h0 & 0x7FFFFFFFu) | 0x10000000u;
    const uint32_t a2 = static_cast<uint32_t>((h0 >> 32u) & 0x7FFFFFFFu) | 0x10000000u;
    const uint32_t a3 = static_cast<uint32_t>(h1 & 0x7FFFFFFFu) | 0x10000000u;
    return BuildConsoleSid(5u, {21u, a1, a2, a3});
}

ConsoleSid DeriveDeterministicUserSid(const ConsoleSid& machine_sid, uint32_t rid) {
    std::vector<uint32_t> subs = machine_sid.sub_authorities;
    if (subs.empty()) {
        subs = {21u, 0x10000001u, 0x10000002u, 0x10000003u};
    }
    subs.push_back(rid);
    return BuildConsoleSid(machine_sid.authority, subs);
}

ConsoleSid DeriveDeterministicAppContainerSid(std::string_view package_family_name) {
    // Matches Windows DeriveAppContainerSidFromAppContainerName layout:
    // Authority = 15 (SECURITY_APP_PACKAGE_AUTHORITY), SubAuth[0] = 2 (SECURITY_APP_PACKAGE_BASE_RID),
    // followed by 7 deterministic 32-bit sub-authorities derived from lowercase PFN.
    const std::string pfn_lo = ToLowerAscii(TrimAscii(package_family_name));
    std::vector<uint32_t> subs;
    subs.reserve(8u);
    subs.push_back(2u);

    uint64_t state = Fnv1a64(pfn_lo, 0x50464E415050434Full);
    for (int i = 0; i < 7; ++i) {
        state = SplitMix64(state ^ static_cast<uint64_t>(i + 1));
        subs.push_back(static_cast<uint32_t>(state & 0xFFFFFFFFu));
    }
    return BuildConsoleSid(15u, subs);
}

bool IsDevelopmentSandboxId(std::string_view sandbox_id) noexcept {
    const std::string up = ToUpperAscii(TrimAscii(sandbox_id));
    if (up.empty() || up == "RETAIL") {
        return false;
    }
    return true;
}

ConsoleLsaPolicy::ConsoleLsaPolicy() {
    ResetToDefaults();
}

ConsoleLsaPolicy& ConsoleLsaPolicy::Global() {
    static ConsoleLsaPolicy s_instance;
    return s_instance;
}

void ConsoleLsaPolicy::ResetToDefaults() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    package_ = {};
    package_.package_name = "OpenScarlett.DefaultTitle";
    package_.publisher = "CN=OpenScarlett";
    package_.app_id = "Game";
    package_.version = "1.0.0.0";
    package_.sandbox_id = "RETAIL";
    package_.title_id = 0x5CA00001u;
    package_.display_name = "OpenScarlett Title";

    lsa_ = {};
    lsa_.machine_name = "XBOX-SCARLETT";
    lsa_.domain_name = "SCARLETT";
    lsa_.account_name = "ScarlettPlayer";
    lsa_.is_app_container = true;
    lsa_.integrity_rid = 0x2000u;

    explicit_content_id_ = false;
    explicit_store_id_ = false;
    explicit_debug_tooling_ = false;
    capabilities_.clear();
    ApplyDefaultCapabilitiesLocked();
    RecomputeDerivedStateLocked();
    ++revision_;
}

void ConsoleLsaPolicy::ApplyDefaultCapabilitiesLocked() {
    capabilities_[static_cast<uint32_t>(ConsoleCapability::AppContainer)] = lsa_.is_app_container;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::XboxLive)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::TitleStorage)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::GameSave)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::Multiplayer)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::InternetClient)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::PrivateNetworkClientServer)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::PersistentLocalStorage)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::SpatialAudio)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::DirectXHardware)] = true;
    capabilities_[static_cast<uint32_t>(ConsoleCapability::GameInput)] = true;
    if (!explicit_debug_tooling_) {
        capabilities_[static_cast<uint32_t>(ConsoleCapability::DebugTooling)] =
            IsDevelopmentSandboxId(package_.sandbox_id);
    }
}

void ConsoleLsaPolicy::RecomputeDerivedStateLocked() {
    if (package_.package_name.empty()) {
        package_.package_name = "OpenScarlett.DefaultTitle";
    }
    if (package_.publisher.empty()) {
        package_.publisher = "CN=OpenScarlett";
    }
    if (package_.app_id.empty()) {
        package_.app_id = "Game";
    }
    if (package_.version.empty()) {
        package_.version = "1.0.0.0";
    }
    if (package_.sandbox_id.empty()) {
        package_.sandbox_id = "RETAIL";
    }

    package_.title_id_hex = FormatTitleIdHex(package_.title_id);
    package_.publisher_id = ComputeAppXPublisherId(package_.publisher);
    package_.package_family_name = package_.package_name + "_" + package_.publisher_id;
    package_.package_full_name =
        package_.package_name + "_" + package_.version + "_x64__" + package_.publisher_id;
    package_.app_user_model_id = package_.package_family_name + "!" + package_.app_id;

    const std::string id_seed = package_.package_family_name + "|" + package_.title_id_hex;
    if (!explicit_content_id_ || package_.content_id.empty()) {
        package_.content_id = DeriveDeterministicGuid("content:" + id_seed);
    }
    if (!explicit_store_id_ || package_.store_id.empty()) {
        package_.store_id = DeriveDeterministicStoreId("store:" + id_seed);
    }

    const std::string host_seed = lsa_.domain_name + "\\" + lsa_.machine_name;
    lsa_.console_id = DeriveDeterministicConsoleId(host_seed);
    lsa_.machine_sid = DeriveDeterministicMachineSid(host_seed);
    lsa_.user_sid = DeriveDeterministicUserSid(lsa_.machine_sid, 1001u);
    lsa_.package_sid = DeriveDeterministicAppContainerSid(package_.package_family_name);
    lsa_.authentication_luid = SplitMix64(Fnv1a64(host_seed + "|" + lsa_.account_name)) | 0x10000ull;
    lsa_.token_id_luid = SplitMix64(Fnv1a64(package_.app_user_model_id + "|" + lsa_.account_name)) | 0x20000ull;

    capabilities_[static_cast<uint32_t>(ConsoleCapability::AppContainer)] = lsa_.is_app_container;
    if (!explicit_debug_tooling_) {
        capabilities_[static_cast<uint32_t>(ConsoleCapability::DebugTooling)] =
            IsDevelopmentSandboxId(package_.sandbox_id);
    }
}

void ConsoleLsaPolicy::ApplyGameConfig(const GameConfig& config,
                                       std::string_view sandbox_id,
                                       std::string_view content_id,
                                       std::string_view store_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!config.identity.name.empty()) {
        package_.package_name = config.identity.name;
    }
    if (!config.identity.publisher.empty()) {
        package_.publisher = config.identity.publisher;
    }
    if (!config.identity.version.empty()) {
        package_.version = config.identity.version;
    }
    if (config.has_title_id) {
        package_.title_id = config.title_id_value;
    }
    if (!config.msa_app_id.empty()) {
        package_.msa_app_id = config.msa_app_id;
    }
    if (!config.display_name.empty()) {
        package_.display_name = config.display_name;
    }
    if (const GameExecutableEntry* primary = config.SelectPrimaryExecutable()) {
        if (!primary->id.empty()) {
            package_.app_id = primary->id;
        }
    }
    if (!sandbox_id.empty()) {
        package_.sandbox_id = ToUpperAscii(TrimAscii(sandbox_id));
    }
    if (!content_id.empty()) {
        package_.content_id = TrimAscii(content_id);
        explicit_content_id_ = true;
    } else {
        explicit_content_id_ = false;
    }
    if (!store_id.empty()) {
        package_.store_id = ToUpperAscii(TrimAscii(store_id));
        explicit_store_id_ = true;
    } else {
        explicit_store_id_ = false;
    }

    RecomputeDerivedStateLocked();
    ++revision_;
}

Status ConsoleLsaPolicy::LoadFromMicrosoftGameConfigXml(std::string_view xml_text,
                                                        const std::string& label) {
    GameConfig cfg{};
    Status st = ParseGameConfigXml(xml_text, label, cfg);
    if (!st.ok()) {
        return st;
    }

    std::string store_id;
    std::string content_id;
    std::string sandbox_id;
    ExtractSimpleXmlTagText(xml_text, "StoreId", store_id);
    ExtractSimpleXmlTagText(xml_text, "ContentId", content_id);
    ExtractSimpleXmlTagText(xml_text, "SandboxId", sandbox_id);
    if (sandbox_id.empty()) {
        ExtractSimpleXmlTagText(xml_text, "XboxLiveSandboxId", sandbox_id);
    }

    ApplyGameConfig(cfg, sandbox_id, content_id, store_id);
    return Status::Ok();
}

Status ConsoleLsaPolicy::LoadFromMicrosoftGameConfigFile(const std::string& config_path) {
    std::ifstream ifs(config_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound,
                             "Cannot open MicrosoftGame.Config: " + config_path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return LoadFromMicrosoftGameConfigXml(oss.str(), config_path);
}

void ConsoleLsaPolicy::SetSandboxId(std::string_view sandbox_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const std::string clean = ToUpperAscii(TrimAscii(sandbox_id));
    if (!clean.empty()) {
        package_.sandbox_id = clean;
        RecomputeDerivedStateLocked();
        ++revision_;
    }
}

void ConsoleLsaPolicy::SetContentId(std::string_view content_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const std::string clean = TrimAscii(content_id);
    if (!clean.empty()) {
        package_.content_id = clean;
        explicit_content_id_ = true;
        ++revision_;
    }
}

void ConsoleLsaPolicy::SetStoreId(std::string_view store_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const std::string clean = ToUpperAscii(TrimAscii(store_id));
    if (!clean.empty()) {
        package_.store_id = clean;
        explicit_store_id_ = true;
        ++revision_;
    }
}

void ConsoleLsaPolicy::SetTitleId(uint32_t title_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    package_.title_id = title_id;
    RecomputeDerivedStateLocked();
    ++revision_;
}

void ConsoleLsaPolicy::SetAppContainerMode(bool enabled) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    lsa_.is_app_container = enabled;
    lsa_.integrity_rid = enabled ? 0x2000u : 0x3000u;
    RecomputeDerivedStateLocked();
    ++revision_;
}

void ConsoleLsaPolicy::SetMachineIdentity(std::string_view machine_name,
                                          std::string_view domain_name,
                                          std::string_view account_name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!machine_name.empty()) {
        lsa_.machine_name = TrimAscii(machine_name);
    }
    if (!domain_name.empty()) {
        lsa_.domain_name = TrimAscii(domain_name);
    }
    if (!account_name.empty()) {
        lsa_.account_name = TrimAscii(account_name);
    }
    RecomputeDerivedStateLocked();
    ++revision_;
}

void ConsoleLsaPolicy::SetCapability(ConsoleCapability cap, bool granted) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (cap == ConsoleCapability::AppContainer) {
        lsa_.is_app_container = granted;
    }
    if (cap == ConsoleCapability::DebugTooling) {
        explicit_debug_tooling_ = true;
    }
    capabilities_[static_cast<uint32_t>(cap)] = granted;
    ++revision_;
}

bool ConsoleLsaPolicy::HasCapability(ConsoleCapability cap) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = capabilities_.find(static_cast<uint32_t>(cap));
    return it != capabilities_.end() && it->second;
}

bool ConsoleLsaPolicy::HasCapabilityByName(std::string_view cap_name) const {
    ConsoleCapability cap{};
    if (!ParseConsoleCapability(cap_name, cap)) {
        return false;
    }
    return HasCapability(cap);
}

ConsoleCapabilityCheckResult ConsoleLsaPolicy::CheckCapability(ConsoleCapability cap) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = capabilities_.find(static_cast<uint32_t>(cap));
    const bool granted = (it != capabilities_.end() && it->second);
    if (granted) {
        return {true, std::string("Granted by ConsoleLsaPolicy (sandbox=") + package_.sandbox_id + ")"};
    }
    return {false, std::string("Denied by ConsoleLsaPolicy (sandbox=") + package_.sandbox_id + ")"};
}

ConsolePackageIdentity ConsoleLsaPolicy::GetPackageIdentity() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return package_;
}

ConsoleLsaTokenIdentity ConsoleLsaPolicy::GetLsaIdentity() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return lsa_;
}

ConsoleLsaPolicySnapshot ConsoleLsaPolicy::Snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ConsoleLsaPolicySnapshot snap{};
    snap.package = package_;
    snap.lsa = lsa_;
    snap.is_dev_sandbox = IsDevelopmentSandboxId(package_.sandbox_id);
    snap.revision = revision_;
    for (ConsoleCapability cap : kAllCapabilities) {
        auto it = capabilities_.find(static_cast<uint32_t>(cap));
        if (it != capabilities_.end() && it->second) {
            snap.granted_capabilities.push_back(cap);
        }
    }
    return snap;
}

bool ConsoleLsaPolicy::LookupSidByAccountName(std::string_view account_name,
                                              ConsoleSid& out_sid) const {
    out_sid = {};
    const std::string query = TrimAscii(account_name);
    if (query.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    const std::string qualified_user = lsa_.domain_name + "\\" + lsa_.account_name;
    if (EqualsIgnoreCase(query, lsa_.account_name) || EqualsIgnoreCase(query, qualified_user)) {
        out_sid = lsa_.user_sid;
        return true;
    }
    if (EqualsIgnoreCase(query, lsa_.machine_name) || EqualsIgnoreCase(query, lsa_.domain_name)) {
        out_sid = lsa_.machine_sid;
        return true;
    }
    if (EqualsIgnoreCase(query, package_.package_family_name) ||
        EqualsIgnoreCase(query, package_.app_user_model_id)) {
        out_sid = lsa_.package_sid;
        return true;
    }
    return false;
}

bool ConsoleLsaPolicy::LookupAccountNameBySid(std::string_view sid_text,
                                              std::string& out_account_name,
                                              std::string& out_domain_name) const {
    out_account_name.clear();
    out_domain_name.clear();
    const std::string query = TrimAscii(sid_text);
    if (query.empty()) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (EqualsIgnoreCase(query, lsa_.user_sid.text)) {
        out_account_name = lsa_.account_name;
        out_domain_name = lsa_.domain_name;
        return true;
    }
    if (EqualsIgnoreCase(query, lsa_.machine_sid.text)) {
        out_account_name = lsa_.machine_name;
        out_domain_name = lsa_.domain_name;
        return true;
    }
    if (EqualsIgnoreCase(query, lsa_.package_sid.text)) {
        out_account_name = package_.package_family_name;
        out_domain_name = "APPLICATION PACKAGE AUTHORITY";
        return true;
    }
    return false;
}

std::string ConsoleLsaPolicy::FormatSummaryText() const {
    const ConsoleLsaPolicySnapshot snap = Snapshot();
    std::ostringstream oss;
    oss << "Console LSA & Package-Family Policy:\n"
        << "  PackageFamilyName : " << snap.package.package_family_name << "\n"
        << "  AppUserModelId    : " << snap.package.app_user_model_id << "\n"
        << "  TitleId           : 0x" << snap.package.title_id_hex
        << " | SandboxId=" << snap.package.sandbox_id
        << (snap.is_dev_sandbox ? " [DEV]" : " [RETAIL]") << "\n"
        << "  ContentId         : " << snap.package.content_id
        << " | StoreId=" << snap.package.store_id << "\n"
        << "  Machine/Domain    : " << snap.lsa.domain_name << "\\" << snap.lsa.machine_name
        << " (ConsoleId=" << snap.lsa.console_id << ")\n"
        << "  MachineSid        : " << snap.lsa.machine_sid.text << "\n"
        << "  UserSid           : " << snap.lsa.user_sid.text << "\n"
        << "  PackageSid        : " << snap.lsa.package_sid.text << "\n"
        << "  Capabilities (" << snap.granted_capabilities.size() << "): ";
    for (size_t i = 0u; i < snap.granted_capabilities.size(); ++i) {
        if (i > 0u) oss << ", ";
        oss << ConsoleCapabilityName(snap.granted_capabilities[i]);
    }
    oss << "\n";
    return oss.str();
}

std::string ConsoleLsaPolicy::FormatSummaryJson() const {
    const ConsoleLsaPolicySnapshot snap = Snapshot();
    std::ostringstream oss;
    oss << "{\n"
        << "  \"package_family_name\": \"" << EscapeJson(snap.package.package_family_name) << "\",\n"
        << "  \"package_full_name\": \"" << EscapeJson(snap.package.package_full_name) << "\",\n"
        << "  \"app_user_model_id\": \"" << EscapeJson(snap.package.app_user_model_id) << "\",\n"
        << "  \"title_id\": " << snap.package.title_id << ",\n"
        << "  \"title_id_hex\": \"" << EscapeJson(snap.package.title_id_hex) << "\",\n"
        << "  \"content_id\": \"" << EscapeJson(snap.package.content_id) << "\",\n"
        << "  \"store_id\": \"" << EscapeJson(snap.package.store_id) << "\",\n"
        << "  \"sandbox_id\": \"" << EscapeJson(snap.package.sandbox_id) << "\",\n"
        << "  \"is_dev_sandbox\": " << (snap.is_dev_sandbox ? "true" : "false") << ",\n"
        << "  \"console_id\": \"" << EscapeJson(snap.lsa.console_id) << "\",\n"
        << "  \"machine_sid\": \"" << EscapeJson(snap.lsa.machine_sid.text) << "\",\n"
        << "  \"user_sid\": \"" << EscapeJson(snap.lsa.user_sid.text) << "\",\n"
        << "  \"package_sid\": \"" << EscapeJson(snap.lsa.package_sid.text) << "\",\n"
        << "  \"is_app_container\": " << (snap.lsa.is_app_container ? "true" : "false") << ",\n"
        << "  \"granted_capabilities\": [";
    for (size_t i = 0u; i < snap.granted_capabilities.size(); ++i) {
        if (i > 0u) oss << ", ";
        oss << "\"" << ConsoleCapabilityName(snap.granted_capabilities[i]) << "\"";
    }
    oss << "]\n}\n";
    return oss.str();
}

}  // namespace openscarlett
