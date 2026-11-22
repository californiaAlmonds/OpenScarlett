#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/loader/game_config.h"

namespace openscarlett {

// Xbox Console / AppContainer capability & privilege identifiers modeled in-process.
enum class ConsoleCapability : uint32_t {
    AppContainer = 0,
    XboxLive = 1,
    TitleStorage = 2,
    GameSave = 3,
    Multiplayer = 4,
    InternetClient = 5,
    PrivateNetworkClientServer = 6,
    PersistentLocalStorage = 7,
    SpatialAudio = 8,
    DirectXHardware = 9,
    GameInput = 10,
    DebugTooling = 11,
};

// Deterministic Windows-compatible SID representation (text + binary SID wire format).
struct ConsoleSid {
    std::string text;                   // e.g. "S-1-5-21-..." or "S-1-15-2-..."
    std::vector<uint8_t> binary;        // Standard Win32 SID layout (Revision, SubCount, Authority[6], SubAuth[N])
    uint8_t revision = 1u;
    uint64_t authority = 5u;            // 48-bit SID_IDENTIFIER_AUTHORITY
    std::vector<uint32_t> sub_authorities;
};

struct ConsolePackageIdentity {
    std::string package_name;           // From <Identity Name="...">
    std::string publisher;              // From <Identity Publisher="...">
    std::string publisher_id;           // 13-char AppX Crockford Base32 publisher hash
    std::string package_family_name;    // "<package_name>_<publisher_id>"
    std::string package_full_name;      // "<package_name>_<version>_x64__<publisher_id>"
    std::string app_id;                 // Primary <Executable Id="..."> (default "Game")
    std::string app_user_model_id;      // "<package_family_name>!<app_id>"
    std::string version;                // e.g. "1.0.0.0"
    std::string content_id;             // Canonical UUID string
    std::string store_id;               // 12-char alphanumeric Microsoft Store ID
    std::string sandbox_id;             // e.g. "RETAIL", "XDKS.1"
    std::string msa_app_id;             // From <MSAAppId>
    std::string display_name;           // From <ShellVisuals DefaultDisplayName="...">
    uint32_t title_id = 0u;
    std::string title_id_hex;           // Uppercase 8-digit hex
};

struct ConsoleLsaTokenIdentity {
    std::string machine_name;           // e.g. "XBOX-SCARLETT"
    std::string domain_name;            // e.g. "SCARLETT"
    std::string account_name;           // e.g. "ScarlettPlayer"
    std::string console_id;             // Deterministic Xbox console ID string
    ConsoleSid machine_sid;             // S-1-5-21-a-b-c
    ConsoleSid user_sid;                // S-1-5-21-a-b-c-1001
    ConsoleSid package_sid;             // S-1-15-2-... (AppContainer package SID)
    uint64_t authentication_luid = 0u;
    uint64_t token_id_luid = 0u;
    uint32_t integrity_rid = 0x2000u;   // SECURITY_MANDATORY_MEDIUM_RID
    bool is_app_container = true;
};

struct ConsoleCapabilityCheckResult {
    bool granted = false;
    std::string reason;
};

struct ConsoleLsaPolicySnapshot {
    ConsolePackageIdentity package;
    ConsoleLsaTokenIdentity lsa;
    std::vector<ConsoleCapability> granted_capabilities;
    bool is_dev_sandbox = false;
    uint64_t revision = 0u;
};

const char* ConsoleCapabilityName(ConsoleCapability cap) noexcept;
bool ParseConsoleCapability(std::string_view name, ConsoleCapability& out_cap) noexcept;

// Computes a deterministic 13-character AppX PublisherId using Microsoft's Base32 alphabet.
std::string ComputeAppXPublisherId(std::string_view publisher);

// Derives deterministic Windows-compatible binary + text SIDs without contacting LSASS.
ConsoleSid DeriveDeterministicMachineSid(std::string_view machine_seed);
ConsoleSid DeriveDeterministicUserSid(const ConsoleSid& machine_sid, uint32_t rid = 1001u);
ConsoleSid DeriveDeterministicAppContainerSid(std::string_view package_family_name);

// Returns true for non-RETAIL Xbox development sandboxes (e.g. "XDKS.1", "DEV.*").
bool IsDevelopmentSandboxId(std::string_view sandbox_id) noexcept;

// Thread-safe in-process Console Local Security Authority (LSA) & Package-Family Policy State.
// Answers package identity, capability, and SID/account policy queries deterministically in-process
// so titles never fail with APPMODEL_ERROR_NO_PACKAGE or stall waiting on LSASS RPCs.
class ConsoleLsaPolicy {
public:
    ConsoleLsaPolicy();

    // Process-wide singleton instance.
    static ConsoleLsaPolicy& Global();

    // Resets identity, SIDs, and capabilities to clean OpenScarlett console defaults.
    void ResetToDefaults();

    // Applies parsed MicrosoftGame.Config fields and recomputes PFN, AUMID, ContentId, StoreId, and SIDs.
    void ApplyGameConfig(const GameConfig& config,
                         std::string_view sandbox_id = {},
                         std::string_view content_id = {},
                         std::string_view store_id = {});

    // Parses MicrosoftGame.Config / .mgc XML text directly (including optional <StoreId>,
    // <ContentId>, <SandboxId>, <Capabilities>, and <DevelopmentOnly> elements).
    Status LoadFromMicrosoftGameConfigXml(std::string_view xml_text,
                                          const std::string& label = "<memory>");

    // Loads MicrosoftGame.Config / .mgc from disk.
    Status LoadFromMicrosoftGameConfigFile(const std::string& config_path);

    void SetSandboxId(std::string_view sandbox_id);
    void SetContentId(std::string_view content_id);
    void SetStoreId(std::string_view store_id);
    void SetTitleId(uint32_t title_id);
    void SetAppContainerMode(bool enabled);
    void SetMachineIdentity(std::string_view machine_name,
                            std::string_view domain_name,
                            std::string_view account_name);

    void SetCapability(ConsoleCapability cap, bool granted);
    bool HasCapability(ConsoleCapability cap) const;
    bool HasCapabilityByName(std::string_view cap_name) const;
    ConsoleCapabilityCheckResult CheckCapability(ConsoleCapability cap) const;

    ConsolePackageIdentity GetPackageIdentity() const;
    ConsoleLsaTokenIdentity GetLsaIdentity() const;
    ConsoleLsaPolicySnapshot Snapshot() const;

    // In-process LSA account/SID lookup (supports account name, DOMAIN\account, PFN, machine name).
    bool LookupSidByAccountName(std::string_view account_name, ConsoleSid& out_sid) const;
    bool LookupAccountNameBySid(std::string_view sid_text,
                                std::string& out_account_name,
                                std::string& out_domain_name) const;

    std::string FormatSummaryText() const;
    std::string FormatSummaryJson() const;

private:
    void RecomputeDerivedStateLocked();
    void ApplyDefaultCapabilitiesLocked();

    mutable std::shared_mutex mutex_;
    ConsolePackageIdentity package_;
    ConsoleLsaTokenIdentity lsa_;
    std::unordered_map<uint32_t, bool> capabilities_;
    bool explicit_content_id_ = false;
    bool explicit_store_id_ = false;
    bool explicit_debug_tooling_ = false;
    uint64_t revision_ = 0u;
};

}  // namespace openscarlett
