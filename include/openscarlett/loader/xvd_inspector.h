#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

// 12 KiB (0x3000) Xbox Virtual Disk (XVD) header layout constants.
inline constexpr size_t kXvdSignatureOffset = 0x0000u;
inline constexpr size_t kXvdSignatureSize = 0x0200u;
inline constexpr size_t kXvdMagicOffset = 0x0200u;
inline constexpr size_t kXvdMagicLength = 8u;
inline constexpr size_t kXvdVolumeFlagsOffset = 0x0208u;
inline constexpr size_t kXvdFormatVersionOffset = 0x020Cu;
inline constexpr size_t kXvdFileTimeCreatedOffset = 0x0210u;
inline constexpr size_t kXvdDriveSizeOffset = 0x0218u;
inline constexpr size_t kXvdVduidOffset = 0x0220u;
inline constexpr size_t kXvdUduidOffset = 0x0230u;
inline constexpr size_t kXvdTopHashBlockHashOffset = 0x0240u;
inline constexpr size_t kXvdOriginalXvcDataHashOffset = 0x0260u;
inline constexpr size_t kXvdTypeOffset = 0x0280u;
inline constexpr size_t kXvdContentTypeOffset = 0x0284u;
inline constexpr size_t kXvdEmbeddedXvdLengthOffset = 0x0288u;
inline constexpr size_t kXvdUserDataLengthOffset = 0x028Cu;
inline constexpr size_t kXvdXvcDataLengthOffset = 0x0290u;
inline constexpr size_t kXvdDynamicHeaderLengthOffset = 0x0294u;
inline constexpr size_t kXvdBlockSizeOffset = 0x0298u;
inline constexpr size_t kXvdExtEntriesOffset = 0x029Cu;
inline constexpr size_t kXvdExtEntrySize = 0x0018u;
inline constexpr size_t kXvdExtEntryCount = 4u;
inline constexpr size_t kXvdCapabilitiesOffset = 0x02FCu;
inline constexpr size_t kXvdPeCatalogHashOffset = 0x030Cu;
inline constexpr size_t kXvdEmbeddedXvdPduidOffset = 0x032Cu;
inline constexpr size_t kXvdKeyMaterialOffset = 0x034Cu;
inline constexpr size_t kXvdKeyMaterialSize = 0x0020u;
inline constexpr size_t kXvdUserDataHashOffset = 0x036Cu;
inline constexpr size_t kXvdSandboxIdOffset = 0x038Cu;
inline constexpr size_t kXvdSandboxIdLength = 0x0010u;
inline constexpr size_t kXvdProductIdOffset = 0x039Cu;
inline constexpr size_t kXvdPduidOffset = 0x03ACu;
inline constexpr size_t kXvdPackageVersionOffset = 0x03BCu;
inline constexpr size_t kXvdWriteableExpirationOffset = 0x0464u;
inline constexpr size_t kXvdWriteablePolicyFlagsOffset = 0x0468u;
inline constexpr size_t kXvdPersistentLocalStorageSizeOffset = 0x046Cu;
inline constexpr size_t kXvdMutableDataPageCountOffset = 0x0470u;
inline constexpr size_t kXvdSequenceNumberOffset = 0x048Cu;
inline constexpr size_t kXvdRequiredSystemVersionOffset = 0x0494u;
inline constexpr size_t kXvdOdkKeyslotIdOffset = 0x049Cu;
inline constexpr size_t kXvdResilientDataOffsetField = 0x0FF4u;
inline constexpr size_t kXvdResilientDataLengthField = 0x0FFCu;
inline constexpr size_t kXvdHeaderTotalSize = 0x3000u;  // 12 KiB (0x200 sig + 0x2E00 header)

inline constexpr uint64_t kXvdPageSize = 0x1000ull;              // 4 KiB
inline constexpr uint32_t kXvdDefaultBlockSize = 0x000AA000u;    // 170 pages = 680 KiB
inline constexpr uint32_t kXvdSectorSize = 4096u;
inline constexpr uint32_t kXvdLegacySectorSize = 512u;
inline constexpr uint64_t kXvdHashEntriesPerPage = 0xAAull;      // 170 entries/page
inline constexpr size_t kXvcInfoHeaderMinSize = 0x0DA8u;
inline constexpr size_t kXvcRegionHeaderSize = 0x0080u;
inline constexpr uint16_t kXvcKeyIdNone = 0xFFFFu;

enum class XvdDiskType : uint32_t {
    Fixed = 0,
    Dynamic = 1,
};

enum class XvdContentType : uint32_t {
    Data = 0x00,
    Title = 0x01,
    SystemOS = 0x02,
    EraOS = 0x03,
    Scratch = 0x04,
    ResetData = 0x05,
    Application = 0x06,
    HostOS = 0x07,
    X360STFS = 0x08,
    X360FATX = 0x09,
    X360GDFX = 0x0A,
    Updater = 0x0B,
    OfflineUpdater = 0x0C,
    Template = 0x0D,
    MteHost = 0x0E,
    MteApp = 0x0F,
    MteTitle = 0x10,
    MteEraOS = 0x11,
    EraTools = 0x12,
    SystemTools = 0x13,
    SystemAux = 0x14,
    AcousticModel = 0x15,
    SystemCodecsVolume = 0x16,
    QasltPackage = 0x17,
    AppDlc = 0x18,
    TitleDlc = 0x19,
    UniversalDlc = 0x1A,
    SystemDataVolume = 0x1B,
    TestVolume = 0x1C,
    HardwareTestVolume = 0x1D,
    KioskContent = 0x1E,
    DevPackage = 0x1F,
    HostProfiler = 0x20,
    Roamable = 0x21,
    ThinProvisioned = 0x22,
    StreamingOnlySRA = 0x23,
    StreamingOnlyERA = 0x24,
    StreamingOnlyHost = 0x25,
    QuickResume = 0x26,
    HostData = 0x27,
};

// XvdHeader VolumeFlags bitmask values (offset 0x208).
namespace XvdVolumeFlags {
inline constexpr uint32_t kReadOnly = 0x00000001u;
inline constexpr uint32_t kEncryptionDisabled = 0x00000002u;
inline constexpr uint32_t kDataIntegrityDisabled = 0x00000004u;
inline constexpr uint32_t kLegacySectorSize = 0x00000008u;
inline constexpr uint32_t kResiliencyEnabled = 0x00000010u;
inline constexpr uint32_t kSraReadOnly = 0x00000020u;
inline constexpr uint32_t kRegionIdInXts = 0x00000040u;
inline constexpr uint32_t kEraSpecific = 0x00000080u;
}  // namespace XvdVolumeFlags

// XvdExtEntry code tags (metadata extensions).
inline constexpr uint32_t kXvdExtCodeTitleId = 0x00000001u;

struct XvdExtEntryInfo {
    uint32_t code = 0;
    uint32_t length = 0;
    uint64_t offset = 0;
    uint32_t data_length = 0;
};

struct XvcRegionSummary {
    uint32_t region_id = 0;
    uint16_t key_id = kXvcKeyIdNone;
    uint32_t flags = 0;
    uint32_t first_segment_index = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t hash = 0;
    std::string description;
    bool is_encrypted = false;
};

struct XvcMetadataSummary {
    bool present = false;  // Header declares XVC content type with non-zero XvcDataLength
    bool parsed = false;   // Unencrypted XvcInfo descriptor was present in input and parsed
    std::string content_id_guid;
    std::string description;
    uint32_t version = 0;
    uint32_t region_count = 0;
    uint32_t flags = 0;
    uint16_t key_count = 0;
    uint32_t initial_play_region_id = 0;
    uint64_t initial_play_offset = 0;
    uint32_t preview_region_id = 0;
    uint64_t preview_offset = 0;
    uint32_t update_segment_count = 0;
    uint32_t region_specifier_count = 0;
    uint64_t unused_space = 0;
    std::vector<XvcRegionSummary> regions;
};

struct XvdGeometry {
    uint64_t header_size = kXvdHeaderTotalSize;
    uint32_t page_size = static_cast<uint32_t>(kXvdPageSize);
    uint32_t sector_size = kXvdSectorSize;
    uint32_t block_size = kXvdDefaultBlockSize;
    uint64_t drive_size_bytes = 0;
    uint64_t drive_page_count = 0;
    uint64_t drive_sector_count = 0;

    uint64_t embedded_xvd_offset = kXvdHeaderTotalSize;
    uint64_t embedded_xvd_length = 0;
    uint64_t embedded_xvd_page_count = 0;

    uint64_t mutable_data_offset = kXvdHeaderTotalSize;
    uint64_t mutable_data_length = 0;
    uint64_t mutable_data_page_count = 0;

    uint64_t hash_tree_offset = kXvdHeaderTotalSize;
    uint64_t hash_tree_levels = 0;
    uint64_t hash_tree_page_count = 0;
    uint64_t hash_tree_bytes = 0;
    uint64_t number_of_metadata_pages = 0;
    uint64_t number_of_hashed_pages = 0;

    uint64_t user_data_offset = kXvdHeaderTotalSize;
    uint64_t user_data_length = 0;
    uint64_t user_data_page_count = 0;

    uint64_t xvc_info_offset = kXvdHeaderTotalSize;
    uint64_t xvc_data_length = 0;
    uint64_t xvc_info_page_count = 0;

    uint64_t dynamic_header_offset = kXvdHeaderTotalSize;
    uint64_t dynamic_header_length = 0;
    uint64_t dynamic_header_page_count = 0;

    uint64_t drive_data_offset = kXvdHeaderTotalSize;
    uint64_t expected_minimum_file_size = kXvdHeaderTotalSize;
};

struct XvdInspectionReport {
    std::string source_label;
    uint64_t file_size_bytes = 0;

    bool is_signed = false;
    uint32_t volume_flags = 0;
    uint32_t format_version = 0;
    uint64_t file_time_created = 0;
    XvdDiskType disk_type = XvdDiskType::Fixed;
    XvdContentType content_type = XvdContentType::Data;

    bool is_xvc = false;
    bool is_encrypted = true;
    bool is_read_only = false;
    bool is_data_integrity_enabled = true;
    bool is_legacy_sector_size = false;
    bool is_resiliency_enabled = false;
    bool is_sra_read_only = false;
    bool is_region_id_in_xts = false;
    bool is_era_specific = false;
    bool has_embedded_xvd = false;
    bool has_user_data = false;
    bool has_key_material_present = false;  // Header presence indicator for the key material field
    bool is_payload_truncated = false;
    bool can_inspect_filesystem_directly = false;

    std::string vduid_guid;
    std::string uduid_guid;
    std::string product_id_guid;
    std::string pduid_guid;
    std::string embedded_xvd_pduid_guid;
    std::string sandbox_id;
    uint32_t title_id = 0;
    bool has_title_id = false;
    std::string package_version;
    std::string required_system_version;

    uint64_t sequence_number = 0;
    uint32_t persistent_local_storage_size = 0;
    uint32_t writeable_expiration_date = 0;
    uint32_t writeable_policy_flags = 0;
    uint32_t odk_keyslot_id = 0;
    uint64_t resilient_data_offset = 0;
    uint32_t resilient_data_length = 0;

    std::string top_hash_block_sha256_hex;
    std::string original_xvc_data_sha256_hex;

    std::vector<XvdExtEntryInfo> ext_entries;
    XvdGeometry geometry;
    XvcMetadataSummary xvc_metadata;
    std::vector<std::string> warnings;
    std::string container_status_summary;
};

const char* XvdDiskTypeName(XvdDiskType type) noexcept;
const char* XvdContentTypeName(XvdContentType type) noexcept;
bool IsXvcContentType(XvdContentType type) noexcept;

// Overflow-safe 170-ary (0xAA) Merkle hash tree level and page count calculator.
bool ComputeXvdHashTreeGeometry(uint64_t hashed_page_count,
                                bool resiliency_enabled,
                                uint64_t& out_levels,
                                uint64_t& out_hash_page_count) noexcept;

// Read-only parser for a 12 KiB (0x3000) XVD header buffer (and optional embedded XvcInfo).
// Pass logical_file_size == 0 to use bytes.size(), or pass the actual on-disk file size when
// inspecting only the header prefix of a large XVD container.
Status InspectXvdBytes(const std::vector<uint8_t>& bytes,
                       const std::string& label,
                       XvdInspectionReport& out_report,
                       uint64_t logical_file_size = 0);

// Read-only file inspector: streams only the 12 KiB header (and unencrypted XvcInfo descriptor
// when present) without loading gigabyte container payloads into memory.
Status InspectXvdFile(const std::string& file_path, XvdInspectionReport& out_report);

// Formats human-readable and deterministic JSON diagnostic reports.
std::string FormatXvdInspectionText(const XvdInspectionReport& report);
std::string FormatXvdInspectionJson(const XvdInspectionReport& report);

}  // namespace openscarlett
