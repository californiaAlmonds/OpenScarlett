#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

struct GameIdentity {
    std::string name;
    std::string publisher;
    std::string version;
};

struct GameExecutableEntry {
    std::string name;
    std::string id;
    std::string target_device_family;
    std::string override_display_name;
    bool is_dev_only = false;
};

struct GameConfig {
    std::string source_path;
    std::string config_version;
    GameIdentity identity;
    std::string title_id;          // Trimmed raw string from <TitleId>
    uint32_t title_id_value = 0;   // Parsed 32-bit hex value (supports optional 0x prefix)
    bool has_title_id = false;
    std::string msa_app_id;
    std::string display_name;      // From <ShellVisuals DefaultDisplayName="...">
    std::vector<GameExecutableEntry> executables;

    // Selects the primary executable, preferring Scarlett -> Xbox -> first non-dev-only -> first entry.
    const GameExecutableEntry* SelectPrimaryExecutable() const noexcept;
};

struct ResolvedPackageTarget {
    std::string exe_path;
    std::string package_root;
    std::string config_path;
    bool has_config = false;
    GameConfig config;
};

// Parser for MicrosoftGame.Config / .mgc XML text in memory.
Status ParseGameConfigXml(std::string_view xml_text,
                          const std::string& label,
                          GameConfig& out_config);

// Loads and parses a MicrosoftGame.Config (or .mgc) file from disk.
Status LoadGameConfigFile(const std::string& config_path, GameConfig& out_config);

// Resolves a launch target from:
//   1. A directory containing MicrosoftGame.Config (case-insensitive) or MicrosoftGameConfig.mgc
//   2. A direct path to MicrosoftGame.Config / .mgc
//   3. A direct path to a .exe (auto-detecting a sibling MicrosoftGame.Config if present)
Status ResolvePackageLaunchTarget(const std::string& input_path,
                                  ResolvedPackageTarget& out_target);

// Formats a safe per-title directory name (uppercase 8-digit hex TitleId when available,
// falling back to sanitized Identity.Name).
std::string FormatTitleIdFolderName(const GameConfig& config);

}  // namespace openscarlett
