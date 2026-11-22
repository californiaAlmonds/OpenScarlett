#include "openscarlett/loader/xvd_inspector.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace openscarlett {

namespace {

constexpr char kXvdMagicBytes[kXvdMagicLength] = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};

// Maximum data pages representable in a 4-level 170-ary (0xAA) XVD Merkle tree: 0xAA^4.
constexpr uint64_t kHashLevel0Cap = kXvdHashEntriesPerPage;                      // 0xAA
constexpr uint64_t kHashLevel1Cap = kHashLevel0Cap * kXvdHashEntriesPerPage;     // 0x70E4
constexpr uint64_t kHashLevel2Cap = kHashLevel1Cap * kXvdHashEntriesPerPage;     // 0x4AF768
constexpr uint64_t kHashLevel3Cap = kHashLevel2Cap * kXvdHashEntriesPerPage;     // 0x31C84B10

constexpr uint32_t kMaxSupportedFormatVersion = 16u;
constexpr uint32_t kMaxParsedXvcRegions = 256u;
constexpr uint64_t kMaxXvcInfoReadBytes = 256ull * 1024ull;

template <typename T>
bool ReadLe(const std::vector<uint8_t>& buf, size_t offset, T& out) noexcept {
    if (offset > buf.size() || sizeof(T) > buf.size() - offset) {
        return false;
    }
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    return true;
}

bool CheckedAddU64(uint64_t a, uint64_t b, uint64_t& out) noexcept {
    if (a > (std::numeric_limits<uint64_t>::max)() - b) {
        return false;
    }
    out = a + b;
    return true;
}

bool CheckedMulU64(uint64_t a, uint64_t b, uint64_t& out) noexcept {
    if (a != 0u && b > (std::numeric_limits<uint64_t>::max)() / a) {
        return false;
    }
    out = a * b;
    return true;
}

bool CheckedBytesToPages(uint64_t bytes, uint64_t& out_pages) noexcept {
    if (bytes == 0u) {
        out_pages = 0u;
        return true;
    }
    constexpr uint64_t kRoundMask = kXvdPageSize - 1u;
    if (bytes > (std::numeric_limits<uint64_t>::max)() - kRoundMask) {
        return false;
    }
    out_pages = (bytes + kRoundMask) / kXvdPageSize;
    return true;
}

bool CheckedPagesToBytes(uint64_t pages, uint64_t& out_bytes) noexcept {
    return CheckedMulU64(pages, kXvdPageSize, out_bytes);
}

bool CheckedDivCeilU64(uint64_t num, uint64_t denom, uint64_t& out) noexcept {
    if (denom == 0u) {
        return false;
    }
    if (num == 0u) {
        out = 0u;
        return true;
    }
    out = 1u + ((num - 1u) / denom);
    return true;
}

std::string FormatGuidLe(const uint8_t* bytes16) {
    uint32_t d1 = 0u;
    uint16_t d2 = 0u;
    uint16_t d3 = 0u;
    std::memcpy(&d1, bytes16 + 0, sizeof(d1));
    std::memcpy(&d2, bytes16 + 4, sizeof(d2));
    std::memcpy(&d3, bytes16 + 6, sizeof(d3));

    char buf[40]{};
    std::snprintf(buf, sizeof(buf),
                  "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  d1,
                  static_cast<unsigned>(d2),
                  static_cast<unsigned>(d3),
                  static_cast<unsigned>(bytes16[8]),
                  static_cast<unsigned>(bytes16[9]),
                  static_cast<unsigned>(bytes16[10]),
                  static_cast<unsigned>(bytes16[11]),
                  static_cast<unsigned>(bytes16[12]),
                  static_cast<unsigned>(bytes16[13]),
                  static_cast<unsigned>(bytes16[14]),
                  static_cast<unsigned>(bytes16[15]));
    return std::string(buf);
}

std::string FormatHexBytes(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4u) & 0x0Fu]);
        out.push_back(kHex[data[i] & 0x0Fu]);
    }
    return out;
}

std::string FormatVersion4(const uint16_t v[4]) {
    // Public XVD headers store PackageVersion / RequiredSystemVersion in [v1, v2, v3, v4] order,
    // displayed as v4.v3.v2.v1 (matching Microsoft / xvdtool conventions).
    char buf[48]{};
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>(v[3]),
                  static_cast<unsigned>(v[2]),
                  static_cast<unsigned>(v[1]),
                  static_cast<unsigned>(v[0]));
    return std::string(buf);
}

std::string ReadTrimmedAscii(const uint8_t* data, size_t max_len) {
    size_t len = 0u;
    while (len < max_len && data[len] != 0u) {
        ++len;
    }
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t ch = data[i];
        if (ch >= 0x20u && ch <= 0x7Eu) {
            out.push_back(static_cast<char>(ch));
        }
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) {
        out.pop_back();
    }
    return out;
}

std::string ReadUtf16LeAsciiSafe(const uint8_t* data, size_t byte_len) {
    const size_t code_units = byte_len / 2u;
    std::string out;
    out.reserve(code_units);
    for (size_t i = 0; i < code_units; ++i) {
        uint16_t cu = 0u;
        std::memcpy(&cu, data + i * 2u, sizeof(cu));
        if (cu == 0u) {
            break;
        }
        if (cu >= 0x20u && cu <= 0x7Eu) {
            out.push_back(static_cast<char>(cu));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

bool HasNonZeroBytes(const uint8_t* data, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0u) {
            return true;
        }
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
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
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

bool ParseHexUint32(std::string_view text, uint32_t& out_val) noexcept {
    if (text.size() >= 2u && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2u);
    }
    if (text.empty() || text.size() > 8u) {
        return false;
    }
    uint32_t acc = 0u;
    for (char c : text) {
        uint32_t digit = 0u;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(10 + (c - 'A'));
        } else {
            return false;
        }
        acc = (acc << 4u) | digit;
    }
    out_val = acc;
    return true;
}

void ParseXvcInfoFromBuffer(const std::vector<uint8_t>& buf,
                            size_t base_offset,
                            XvdInspectionReport& report) {
    if (base_offset > buf.size() || buf.size() - base_offset < kXvcInfoHeaderMinSize) {
        return;
    }

    XvcMetadataSummary& xvc = report.xvc_metadata;
    xvc.parsed = true;
    xvc.content_id_guid = FormatGuidLe(buf.data() + base_offset + 0x000u);
    xvc.description = ReadUtf16LeAsciiSafe(buf.data() + base_offset + 0xC10u, 0x100u);

    ReadLe(buf, base_offset + 0xD10u, xvc.version);
    ReadLe(buf, base_offset + 0xD14u, xvc.region_count);
    ReadLe(buf, base_offset + 0xD18u, xvc.flags);
    ReadLe(buf, base_offset + 0xD1Eu, xvc.key_count);
    ReadLe(buf, base_offset + 0xD24u, xvc.initial_play_region_id);
    ReadLe(buf, base_offset + 0xD28u, xvc.initial_play_offset);
    ReadLe(buf, base_offset + 0xD38u, xvc.preview_region_id);
    ReadLe(buf, base_offset + 0xD3Cu, xvc.update_segment_count);
    ReadLe(buf, base_offset + 0xD40u, xvc.preview_offset);
    ReadLe(buf, base_offset + 0xD48u, xvc.unused_space);
    ReadLe(buf, base_offset + 0xD50u, xvc.region_specifier_count);

    // Optional region table starts immediately after the 0xDA8 XvcInfo fixed header.
    const size_t regions_base = base_offset + kXvcInfoHeaderMinSize;
    const uint32_t capped_regions = std::min<uint32_t>(xvc.region_count, kMaxParsedXvcRegions);
    if (xvc.region_count > kMaxParsedXvcRegions) {
        report.warnings.push_back("XVC region count capped during inspection");
    }

    for (uint32_t i = 0u; i < capped_regions; ++i) {
        const size_t reg_off = regions_base + static_cast<size_t>(i) * kXvcRegionHeaderSize;
        if (reg_off > buf.size() || buf.size() - reg_off < kXvcRegionHeaderSize) {
            break;
        }
        XvcRegionSummary reg{};
        ReadLe(buf, reg_off + 0x00u, reg.region_id);
        ReadLe(buf, reg_off + 0x04u, reg.key_id);
        ReadLe(buf, reg_off + 0x08u, reg.flags);
        ReadLe(buf, reg_off + 0x0Cu, reg.first_segment_index);
        reg.description = ReadUtf16LeAsciiSafe(buf.data() + reg_off + 0x10u, 0x40u);
        ReadLe(buf, reg_off + 0x50u, reg.offset);
        ReadLe(buf, reg_off + 0x58u, reg.length);
        ReadLe(buf, reg_off + 0x60u, reg.hash);
        reg.is_encrypted = (reg.key_id != kXvcKeyIdNone);
        xvc.regions.push_back(std::move(reg));
    }

    // Attempt optional TitleId detection from XVC description tag ("TitleId=XXXXXXXX") if not yet set.
    if (!report.has_title_id && !xvc.description.empty()) {
        const size_t pos = xvc.description.find("TitleId=");
        if (pos != std::string::npos) {
            std::string_view tail = std::string_view(xvc.description).substr(pos + 8u);
            const size_t sep = tail.find_first_of(" ;,|");
            if (sep != std::string_view::npos) {
                tail = tail.substr(0u, sep);
            }
            uint32_t parsed_tid = 0u;
            if (ParseHexUint32(tail, parsed_tid)) {
                report.title_id = parsed_tid;
                report.has_title_id = true;
            }
        }
    }
}

}  // namespace

const char* XvdDiskTypeName(XvdDiskType type) noexcept {
    switch (type) {
        case XvdDiskType::Fixed: return "Fixed";
        case XvdDiskType::Dynamic: return "Dynamic";
    }
    return "Unknown";
}

const char* XvdContentTypeName(XvdContentType type) noexcept {
    switch (type) {
        case XvdContentType::Data: return "Data";
        case XvdContentType::Title: return "Title";
        case XvdContentType::SystemOS: return "SystemOS";
        case XvdContentType::EraOS: return "EraOS";
        case XvdContentType::Scratch: return "Scratch";
        case XvdContentType::ResetData: return "ResetData";
        case XvdContentType::Application: return "Application";
        case XvdContentType::HostOS: return "HostOS";
        case XvdContentType::X360STFS: return "X360STFS";
        case XvdContentType::X360FATX: return "X360FATX";
        case XvdContentType::X360GDFX: return "X360GDFX";
        case XvdContentType::Updater: return "Updater";
        case XvdContentType::OfflineUpdater: return "OfflineUpdater";
        case XvdContentType::Template: return "Template";
        case XvdContentType::MteHost: return "MteHost";
        case XvdContentType::MteApp: return "MteApp";
        case XvdContentType::MteTitle: return "MteTitle";
        case XvdContentType::MteEraOS: return "MteEraOS";
        case XvdContentType::EraTools: return "EraTools";
        case XvdContentType::SystemTools: return "SystemTools";
        case XvdContentType::SystemAux: return "SystemAux";
        case XvdContentType::AcousticModel: return "AcousticModel";
        case XvdContentType::SystemCodecsVolume: return "SystemCodecsVolume";
        case XvdContentType::QasltPackage: return "QasltPackage";
        case XvdContentType::AppDlc: return "AppDlc";
        case XvdContentType::TitleDlc: return "TitleDlc";
        case XvdContentType::UniversalDlc: return "UniversalDlc";
        case XvdContentType::SystemDataVolume: return "SystemDataVolume";
        case XvdContentType::TestVolume: return "TestVolume";
        case XvdContentType::HardwareTestVolume: return "HardwareTestVolume";
        case XvdContentType::KioskContent: return "KioskContent";
        case XvdContentType::DevPackage: return "DevPackage";
        case XvdContentType::HostProfiler: return "HostProfiler";
        case XvdContentType::Roamable: return "Roamable";
        case XvdContentType::ThinProvisioned: return "ThinProvisioned";
        case XvdContentType::StreamingOnlySRA: return "StreamingOnlySRA";
        case XvdContentType::StreamingOnlyERA: return "StreamingOnlyERA";
        case XvdContentType::StreamingOnlyHost: return "StreamingOnlyHost";
        case XvdContentType::QuickResume: return "QuickResume";
        case XvdContentType::HostData: return "HostData";
    }
    return "Unknown";
}

bool IsXvcContentType(XvdContentType type) noexcept {
    // Public Xbox bitmask 0x07018042: bits for Title(1), Application(6), MteApp(15),
    // MteTitle(16), AppDlc(24), TitleDlc(25), UniversalDlc(26).
    const uint32_t raw = static_cast<uint32_t>(type);
    if (raw >= 32u) {
        return false;
    }
    constexpr uint32_t kXvcMask = 0x07018042u;
    return ((kXvcMask >> raw) & 1u) != 0u;
}

bool ComputeXvdHashTreeGeometry(uint64_t hashed_page_count,
                                bool resiliency_enabled,
                                uint64_t& out_levels,
                                uint64_t& out_hash_page_count) noexcept {
    out_levels = 0u;
    out_hash_page_count = 0u;
    if (hashed_page_count == 0u) {
        return true;
    }
    if (hashed_page_count > kHashLevel3Cap) {
        return false;
    }

    uint64_t level0 = 0u;
    if (!CheckedDivCeilU64(hashed_page_count, kHashLevel0Cap, level0)) {
        return false;
    }
    uint64_t total_pages = level0;
    uint64_t levels = 1u;

    if (level0 > 1u) {
        constexpr uint64_t kLevelCaps[3] = {kHashLevel1Cap, kHashLevel2Cap, kHashLevel3Cap};
        for (size_t idx = 0; idx < 3u; ++idx) {
            uint64_t lvl_blocks = 0u;
            if (!CheckedDivCeilU64(hashed_page_count, kLevelCaps[idx], lvl_blocks)) {
                return false;
            }
            if (!CheckedAddU64(total_pages, lvl_blocks, total_pages)) {
                return false;
            }
            ++levels;
            if (lvl_blocks <= 1u) {
                break;
            }
        }
    }

    if (resiliency_enabled) {
        if (!CheckedMulU64(total_pages, 2u, total_pages)) {
            return false;
        }
    }

    out_levels = levels;
    out_hash_page_count = total_pages;
    return true;
}

Status InspectXvdBytes(const std::vector<uint8_t>& bytes,
                       const std::string& label,
                       XvdInspectionReport& out_report,
                       uint64_t logical_file_size) {
    out_report = {};
    out_report.source_label = label;
    out_report.file_size_bytes = (logical_file_size != 0u)
                                     ? logical_file_size
                                     : static_cast<uint64_t>(bytes.size());

    if (bytes.size() < kXvdHeaderTotalSize) {
        return Status::Error(StatusCode::ParseError,
                             "Truncated XVD container: expected at least 12 KiB (0x3000) header, got " +
                                 std::to_string(bytes.size()) + " bytes");
    }

    // 1. Validate "msft-xvd" magic at offset 0x200.
    if (std::memcmp(bytes.data() + kXvdMagicOffset, kXvdMagicBytes, kXvdMagicLength) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Invalid XVD header magic at offset 0x200 (expected 'msft-xvd')");
    }

    // 2. Signature presence (bytes [0x20..0x1FF] non-zero). Strictly read-only detection.
    out_report.is_signed = HasNonZeroBytes(bytes.data() + 0x20u, kXvdSignatureSize - 0x20u);

    // 3. Parse primary 12 KiB header metadata fields.
    uint32_t raw_type = 0u;
    uint32_t raw_content_type = 0u;
    uint32_t embedded_xvd_len = 0u;
    uint32_t user_data_len = 0u;
    uint32_t xvc_data_len = 0u;
    uint32_t dynamic_hdr_len = 0u;
    uint32_t block_size = 0u;
    uint8_t mutable_data_pages = 0u;
    uint64_t drive_size = 0u;

    ReadLe(bytes, kXvdVolumeFlagsOffset, out_report.volume_flags);
    ReadLe(bytes, kXvdFormatVersionOffset, out_report.format_version);
    ReadLe(bytes, kXvdFileTimeCreatedOffset, out_report.file_time_created);
    ReadLe(bytes, kXvdDriveSizeOffset, drive_size);
    ReadLe(bytes, kXvdTypeOffset, raw_type);
    ReadLe(bytes, kXvdContentTypeOffset, raw_content_type);
    ReadLe(bytes, kXvdEmbeddedXvdLengthOffset, embedded_xvd_len);
    ReadLe(bytes, kXvdUserDataLengthOffset, user_data_len);
    ReadLe(bytes, kXvdXvcDataLengthOffset, xvc_data_len);
    ReadLe(bytes, kXvdDynamicHeaderLengthOffset, dynamic_hdr_len);
    ReadLe(bytes, kXvdBlockSizeOffset, block_size);
    ReadLe(bytes, kXvdWriteableExpirationOffset, out_report.writeable_expiration_date);
    ReadLe(bytes, kXvdWriteablePolicyFlagsOffset, out_report.writeable_policy_flags);
    ReadLe(bytes, kXvdPersistentLocalStorageSizeOffset, out_report.persistent_local_storage_size);
    ReadLe(bytes, kXvdMutableDataPageCountOffset, mutable_data_pages);
    ReadLe(bytes, kXvdSequenceNumberOffset, out_report.sequence_number);
    ReadLe(bytes, kXvdOdkKeyslotIdOffset, out_report.odk_keyslot_id);
    ReadLe(bytes, kXvdResilientDataOffsetField, out_report.resilient_data_offset);
    ReadLe(bytes, kXvdResilientDataLengthField, out_report.resilient_data_length);

    if (out_report.format_version == 0u || out_report.format_version > kMaxSupportedFormatVersion) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported XVD format version: " +
                                 std::to_string(out_report.format_version));
    }
    if (raw_type > static_cast<uint32_t>(XvdDiskType::Dynamic)) {
        return Status::Error(StatusCode::ParseError,
                             "Invalid XVD disk type at offset 0x280: " + std::to_string(raw_type));
    }
    if (drive_size == 0u) {
        return Status::Error(StatusCode::ParseError,
                             "Invalid XVD header: DriveSize at offset 0x218 is zero");
    }

    out_report.disk_type = static_cast<XvdDiskType>(raw_type);
    out_report.content_type = static_cast<XvdContentType>(raw_content_type);
    out_report.is_xvc = IsXvcContentType(out_report.content_type);

    // 4. Decode VolumeFlags (offset 0x208).
    const uint32_t vf = out_report.volume_flags;
    out_report.is_read_only = (vf & XvdVolumeFlags::kReadOnly) != 0u;
    out_report.is_encrypted = (vf & XvdVolumeFlags::kEncryptionDisabled) == 0u;
    out_report.is_data_integrity_enabled = (vf & XvdVolumeFlags::kDataIntegrityDisabled) == 0u;
    out_report.is_legacy_sector_size = (vf & XvdVolumeFlags::kLegacySectorSize) != 0u;
    out_report.is_resiliency_enabled = (vf & XvdVolumeFlags::kResiliencyEnabled) != 0u;
    out_report.is_sra_read_only = (vf & XvdVolumeFlags::kSraReadOnly) != 0u;
    out_report.is_region_id_in_xts = (vf & XvdVolumeFlags::kRegionIdInXts) != 0u;
    out_report.is_era_specific = (vf & XvdVolumeFlags::kEraSpecific) != 0u;

    out_report.has_embedded_xvd = (embedded_xvd_len != 0u);
    out_report.has_user_data = (user_data_len != 0u);
    out_report.has_key_material_present =
        HasNonZeroBytes(bytes.data() + kXvdKeyMaterialOffset, kXvdKeyMaterialSize);

    // 5. Identifiers, versions, hashes, and extension entries.
    out_report.vduid_guid = FormatGuidLe(bytes.data() + kXvdVduidOffset);
    out_report.uduid_guid = FormatGuidLe(bytes.data() + kXvdUduidOffset);
    out_report.product_id_guid = FormatGuidLe(bytes.data() + kXvdProductIdOffset);
    out_report.pduid_guid = FormatGuidLe(bytes.data() + kXvdPduidOffset);
    out_report.embedded_xvd_pduid_guid = FormatGuidLe(bytes.data() + kXvdEmbeddedXvdPduidOffset);
    out_report.sandbox_id = ReadTrimmedAscii(bytes.data() + kXvdSandboxIdOffset, kXvdSandboxIdLength);
    out_report.top_hash_block_sha256_hex =
        FormatHexBytes(bytes.data() + kXvdTopHashBlockHashOffset, 32u);
    out_report.original_xvc_data_sha256_hex =
        FormatHexBytes(bytes.data() + kXvdOriginalXvcDataHashOffset, 32u);

    uint16_t pkg_ver[4]{};
    uint16_t sys_ver[4]{};
    for (size_t i = 0; i < 4u; ++i) {
        ReadLe(bytes, kXvdPackageVersionOffset + i * sizeof(uint16_t), pkg_ver[i]);
        ReadLe(bytes, kXvdRequiredSystemVersionOffset + i * sizeof(uint16_t), sys_ver[i]);
    }
    out_report.package_version = FormatVersion4(pkg_ver);
    out_report.required_system_version = FormatVersion4(sys_ver);

    for (size_t i = 0; i < kXvdExtEntryCount; ++i) {
        const size_t ext_off = kXvdExtEntriesOffset + i * kXvdExtEntrySize;
        XvdExtEntryInfo ext{};
        ReadLe(bytes, ext_off + 0x00u, ext.code);
        ReadLe(bytes, ext_off + 0x04u, ext.length);
        ReadLe(bytes, ext_off + 0x08u, ext.offset);
        ReadLe(bytes, ext_off + 0x10u, ext.data_length);
        if (ext.code != 0u || ext.length != 0u || ext.offset != 0u || ext.data_length != 0u) {
            out_report.ext_entries.push_back(ext);
            if (!out_report.has_title_id && ext.code == kXvdExtCodeTitleId && ext.data_length != 0u) {
                out_report.title_id = ext.data_length;
                out_report.has_title_id = true;
            }
        }
    }

    // 6. Overflow-safe sector, page, hash-tree, and segment geometry calculation.
    XvdGeometry& g = out_report.geometry;
    g.header_size = kXvdHeaderTotalSize;
    g.page_size = static_cast<uint32_t>(kXvdPageSize);
    g.sector_size = out_report.is_legacy_sector_size ? kXvdLegacySectorSize : kXvdSectorSize;
    g.block_size = (block_size != 0u) ? block_size : kXvdDefaultBlockSize;
    if (block_size != 0u && (block_size % kXvdPageSize) != 0u) {
        out_report.warnings.push_back("Header BlockSize at 0x298 is not 4 KiB page-aligned");
    }
    if ((drive_size % g.sector_size) != 0u) {
        return Status::Error(StatusCode::ParseError,
                             "DriveSize at 0x218 is not aligned to volume sector size (" +
                                 std::to_string(g.sector_size) + " bytes)");
    }

    g.drive_size_bytes = drive_size;
    g.drive_sector_count = drive_size / g.sector_size;
    g.embedded_xvd_length = embedded_xvd_len;
    g.user_data_length = user_data_len;
    g.xvc_data_length = xvc_data_len;
    g.dynamic_header_length = dynamic_hdr_len;
    g.mutable_data_page_count = mutable_data_pages;

    if (!CheckedBytesToPages(g.drive_size_bytes, g.drive_page_count) ||
        !CheckedBytesToPages(g.embedded_xvd_length, g.embedded_xvd_page_count) ||
        !CheckedBytesToPages(g.user_data_length, g.user_data_page_count) ||
        !CheckedBytesToPages(g.xvc_data_length, g.xvc_info_page_count) ||
        !CheckedBytesToPages(g.dynamic_header_length, g.dynamic_header_page_count) ||
        !CheckedPagesToBytes(g.mutable_data_page_count, g.mutable_data_length)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD page counts");
    }

    uint64_t tmp_meta = 0u;
    if (!CheckedAddU64(g.user_data_page_count, g.xvc_info_page_count, tmp_meta) ||
        !CheckedAddU64(tmp_meta, g.dynamic_header_page_count, g.number_of_metadata_pages) ||
        !CheckedAddU64(g.drive_page_count, g.number_of_metadata_pages, g.number_of_hashed_pages)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD hashed page count");
    }

    if (!ComputeXvdHashTreeGeometry(g.number_of_hashed_pages,
                                    out_report.is_resiliency_enabled,
                                    g.hash_tree_levels,
                                    g.hash_tree_page_count)) {
        return Status::Error(StatusCode::ParseError,
                             "XVD DriveSize/metadata pages exceed 4-level Merkle hash tree capacity");
    }
    if (!CheckedPagesToBytes(g.hash_tree_page_count, g.hash_tree_bytes)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD hash tree byte size");
    }

    uint64_t embedded_bytes = 0u;
    uint64_t user_bytes = 0u;
    uint64_t xvc_bytes = 0u;
    uint64_t dyn_bytes = 0u;
    uint64_t drive_padded_bytes = 0u;
    if (!CheckedPagesToBytes(g.embedded_xvd_page_count, embedded_bytes) ||
        !CheckedPagesToBytes(g.user_data_page_count, user_bytes) ||
        !CheckedPagesToBytes(g.xvc_info_page_count, xvc_bytes) ||
        !CheckedPagesToBytes(g.dynamic_header_page_count, dyn_bytes) ||
        !CheckedPagesToBytes(g.drive_page_count, drive_padded_bytes)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD region sizes");
    }

    g.embedded_xvd_offset = kXvdHeaderTotalSize;
    if (!CheckedAddU64(g.embedded_xvd_offset, embedded_bytes, g.mutable_data_offset) ||
        !CheckedAddU64(g.mutable_data_offset, g.mutable_data_length, g.hash_tree_offset)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD hash tree offset");
    }

    const uint64_t effective_hash_bytes =
        out_report.is_data_integrity_enabled ? g.hash_tree_bytes : 0u;
    if (!CheckedAddU64(g.hash_tree_offset, effective_hash_bytes, g.user_data_offset) ||
        !CheckedAddU64(g.user_data_offset, user_bytes, g.xvc_info_offset) ||
        !CheckedAddU64(g.xvc_info_offset, xvc_bytes, g.dynamic_header_offset) ||
        !CheckedAddU64(g.dynamic_header_offset, dyn_bytes, g.drive_data_offset)) {
        return Status::Error(StatusCode::ParseError, "Overflow computing XVD segment offsets");
    }

    if (out_report.disk_type == XvdDiskType::Fixed) {
        if (!CheckedAddU64(g.drive_data_offset, drive_padded_bytes, g.expected_minimum_file_size)) {
            return Status::Error(StatusCode::ParseError,
                                 "Overflow computing Fixed XVD expected file size");
        }
    } else {
        g.expected_minimum_file_size = g.drive_data_offset;
    }

    if (out_report.file_size_bytes < g.expected_minimum_file_size) {
        out_report.is_payload_truncated = true;
        out_report.warnings.push_back(
            "Container size (" + std::to_string(out_report.file_size_bytes) +
            " bytes) is smaller than expected minimum layout (" +
            std::to_string(g.expected_minimum_file_size) + " bytes)");
    }

    // 7. Embedded XVC metadata region detection (unencrypted XvcInfo descriptor).
    out_report.xvc_metadata.present = (out_report.is_xvc && g.xvc_data_length > 0u);
    if (out_report.xvc_metadata.present &&
        g.xvc_info_offset <= static_cast<uint64_t>(bytes.size()) &&
        static_cast<uint64_t>(bytes.size()) - g.xvc_info_offset >= kXvcInfoHeaderMinSize) {
        ParseXvcInfoFromBuffer(bytes, static_cast<size_t>(g.xvc_info_offset), out_report);
    }

    // 8. Container accessibility diagnostic.
    out_report.can_inspect_filesystem_directly =
        (!out_report.is_encrypted && !out_report.is_payload_truncated);

    if (out_report.is_encrypted) {
        out_report.container_status_summary =
            "ENCRYPTED XVD/XVC container (EncryptionDisabled flag not set). "
            "Only the unencrypted header can be inspected; "
            "supply an unpacked package directory with loose files to run.";
    } else if (out_report.is_payload_truncated) {
        out_report.container_status_summary =
            "UNENCRYPTED XVD/XVC header (EncryptionDisabled set), but container payload is truncated "
            "relative to header geometry.";
    } else {
        out_report.container_status_summary =
            "UNENCRYPTED / UNPACKED XVD/XVC container (EncryptionDisabled set). "
            "Filesystem payload begins at DriveDataOffset and can be inspected directly.";
    }

    return Status::Ok();
}

Status InspectXvdFile(const std::string& file_path, XvdInspectionReport& out_report) {
    out_report = {};
    if (file_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "XVD file path is empty");
    }

    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound, "Cannot open XVD file: " + file_path);
    }
    const std::streamoff raw_sz = ifs.tellg();
    if (raw_sz < 0) {
        return Status::Error(StatusCode::IoError, "Failed to query XVD file size: " + file_path);
    }
    const uint64_t file_size = static_cast<uint64_t>(raw_sz);
    if (file_size < kXvdHeaderTotalSize) {
        return Status::Error(StatusCode::ParseError,
                             "Truncated XVD file '" + file_path + "': " +
                                 std::to_string(file_size) + " bytes (< 12 KiB header)");
    }

    std::vector<uint8_t> header_buf(kXvdHeaderTotalSize, 0u);
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(header_buf.data()),
                  static_cast<std::streamsize>(kXvdHeaderTotalSize))) {
        return Status::Error(StatusCode::IoError, "Failed reading 12 KiB XVD header: " + file_path);
    }

    Status st = InspectXvdBytes(header_buf, file_path, out_report, file_size);
    if (!st.ok()) {
        return st;
    }

    // Stream optional unencrypted XvcInfo descriptor if present on disk and not yet parsed.
    if (out_report.xvc_metadata.present && !out_report.xvc_metadata.parsed &&
        out_report.geometry.xvc_info_offset < file_size) {
        const uint64_t avail = file_size - out_report.geometry.xvc_info_offset;
        const uint64_t want = std::min<uint64_t>(
            std::min<uint64_t>(avail, std::max<uint64_t>(out_report.geometry.xvc_data_length,
                                                         kXvcInfoHeaderMinSize)),
            kMaxXvcInfoReadBytes);
        if (want >= kXvcInfoHeaderMinSize) {
            std::vector<uint8_t> xvc_buf(static_cast<size_t>(want), 0u);
            ifs.clear();
            ifs.seekg(static_cast<std::streamoff>(out_report.geometry.xvc_info_offset), std::ios::beg);
            if (ifs.read(reinterpret_cast<char*>(xvc_buf.data()),
                         static_cast<std::streamsize>(want))) {
                ParseXvcInfoFromBuffer(xvc_buf, 0u, out_report);
            }
        }
    }

    return Status::Ok();
}

std::string FormatXvdInspectionText(const XvdInspectionReport& report) {
    std::ostringstream oss;
    oss << "XVD Container Inspection: " << report.source_label << "\n"
        << "  Status           : " << report.container_status_summary << "\n"
        << "  FormatVersion    : " << report.format_version
        << " | DiskType=" << XvdDiskTypeName(report.disk_type)
        << " | ContentType=" << XvdContentTypeName(report.content_type)
        << " (0x" << std::hex << static_cast<uint32_t>(report.content_type) << std::dec << ")"
        << (report.is_xvc ? " [XVC]" : " [XVD]") << "\n"
        << "  VolumeFlags      : 0x" << std::hex << report.volume_flags << std::dec
        << " [encrypted=" << (report.is_encrypted ? "yes" : "no")
        << ", read_only=" << (report.is_read_only ? "yes" : "no")
        << ", integrity=" << (report.is_data_integrity_enabled ? "yes" : "no")
        << ", resiliency=" << (report.is_resiliency_enabled ? "yes" : "no")
        << ", sector_bytes=" << report.geometry.sector_size << "]\n"
        << "  Signed           : " << (report.is_signed ? "yes" : "no")
        << " | KeyMaterialPresent=" << (report.has_key_material_present ? "yes" : "no") << "\n"
        << "  SandboxId        : " << (report.sandbox_id.empty() ? "<empty>" : report.sandbox_id)
        << " | PackageVersion=" << report.package_version
        << " | ReqOS=" << report.required_system_version << "\n";
    if (report.has_title_id) {
        char tid_buf[16]{};
        std::snprintf(tid_buf, sizeof(tid_buf), "0x%08X", report.title_id);
        oss << "  TitleId          : " << tid_buf << "\n";
    }
    oss << "  VDUID            : " << report.vduid_guid << "\n"
        << "  UDUID            : " << report.uduid_guid << "\n"
        << "  DriveSize        : " << report.geometry.drive_size_bytes << " bytes ("
        << report.geometry.drive_page_count << " pages, "
        << report.geometry.drive_sector_count << " sectors)\n"
        << "  Geometry Offsets : Header=0x0..0x3000"
        << ", EmbeddedXVD=0x" << std::hex << report.geometry.embedded_xvd_offset
        << " (len=0x" << report.geometry.embedded_xvd_length << ")"
        << ", MutableData=0x" << report.geometry.mutable_data_offset
        << " (pages=" << std::dec << report.geometry.mutable_data_page_count << ")"
        << ", HashTree=0x" << std::hex << report.geometry.hash_tree_offset
        << " (levels=" << std::dec << report.geometry.hash_tree_levels
        << ", pages=" << report.geometry.hash_tree_page_count << ")"
        << ", UserData=0x" << std::hex << report.geometry.user_data_offset
        << ", XvcInfo=0x" << report.geometry.xvc_info_offset
        << ", DynamicHdr=0x" << report.geometry.dynamic_header_offset
        << ", DriveData=0x" << report.geometry.drive_data_offset << std::dec << "\n";
    if (report.xvc_metadata.present) {
        oss << "  XVC Metadata     : present=yes, parsed="
            << (report.xvc_metadata.parsed ? "yes" : "no");
        if (report.xvc_metadata.parsed) {
            oss << ", version=" << report.xvc_metadata.version
                << ", regions=" << report.xvc_metadata.region_count
                << ", key_count=" << report.xvc_metadata.key_count
                << ", content_id=" << report.xvc_metadata.content_id_guid;
        }
        oss << "\n";
    }
    if (!report.warnings.empty()) {
        oss << "  Warnings (" << report.warnings.size() << "):\n";
        for (const auto& w : report.warnings) {
            oss << "    - " << w << "\n";
        }
    }
    return oss.str();
}

std::string FormatXvdInspectionJson(const XvdInspectionReport& report) {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"source\": \"" << EscapeJson(report.source_label) << "\",\n"
        << "  \"file_size_bytes\": " << report.file_size_bytes << ",\n"
        << "  \"format_version\": " << report.format_version << ",\n"
        << "  \"disk_type\": \"" << XvdDiskTypeName(report.disk_type) << "\",\n"
        << "  \"content_type\": \"" << XvdContentTypeName(report.content_type) << "\",\n"
        << "  \"content_type_id\": " << static_cast<uint32_t>(report.content_type) << ",\n"
        << "  \"is_xvc\": " << (report.is_xvc ? "true" : "false") << ",\n"
        << "  \"is_signed\": " << (report.is_signed ? "true" : "false") << ",\n"
        << "  \"volume_flags\": " << report.volume_flags << ",\n"
        << "  \"is_encrypted\": " << (report.is_encrypted ? "true" : "false") << ",\n"
        << "  \"is_read_only\": " << (report.is_read_only ? "true" : "false") << ",\n"
        << "  \"is_data_integrity_enabled\": " << (report.is_data_integrity_enabled ? "true" : "false") << ",\n"
        << "  \"is_resiliency_enabled\": " << (report.is_resiliency_enabled ? "true" : "false") << ",\n"
        << "  \"is_payload_truncated\": " << (report.is_payload_truncated ? "true" : "false") << ",\n"
        << "  \"can_inspect_filesystem_directly\": " << (report.can_inspect_filesystem_directly ? "true" : "false") << ",\n"
        << "  \"sandbox_id\": \"" << EscapeJson(report.sandbox_id) << "\",\n"
        << "  \"has_title_id\": " << (report.has_title_id ? "true" : "false") << ",\n"
        << "  \"title_id\": " << report.title_id << ",\n"
        << "  \"vduid\": \"" << EscapeJson(report.vduid_guid) << "\",\n"
        << "  \"uduid\": \"" << EscapeJson(report.uduid_guid) << "\",\n"
        << "  \"product_id\": \"" << EscapeJson(report.product_id_guid) << "\",\n"
        << "  \"pduid\": \"" << EscapeJson(report.pduid_guid) << "\",\n"
        << "  \"package_version\": \"" << EscapeJson(report.package_version) << "\",\n"
        << "  \"required_system_version\": \"" << EscapeJson(report.required_system_version) << "\",\n"
        << "  \"status_summary\": \"" << EscapeJson(report.container_status_summary) << "\",\n"
        << "  \"geometry\": {\n"
        << "    \"header_size\": " << report.geometry.header_size << ",\n"
        << "    \"page_size\": " << report.geometry.page_size << ",\n"
        << "    \"sector_size\": " << report.geometry.sector_size << ",\n"
        << "    \"block_size\": " << report.geometry.block_size << ",\n"
        << "    \"drive_size_bytes\": " << report.geometry.drive_size_bytes << ",\n"
        << "    \"drive_page_count\": " << report.geometry.drive_page_count << ",\n"
        << "    \"drive_sector_count\": " << report.geometry.drive_sector_count << ",\n"
        << "    \"embedded_xvd_offset\": " << report.geometry.embedded_xvd_offset << ",\n"
        << "    \"embedded_xvd_length\": " << report.geometry.embedded_xvd_length << ",\n"
        << "    \"mutable_data_offset\": " << report.geometry.mutable_data_offset << ",\n"
        << "    \"mutable_data_page_count\": " << report.geometry.mutable_data_page_count << ",\n"
        << "    \"hash_tree_offset\": " << report.geometry.hash_tree_offset << ",\n"
        << "    \"hash_tree_levels\": " << report.geometry.hash_tree_levels << ",\n"
        << "    \"hash_tree_page_count\": " << report.geometry.hash_tree_page_count << ",\n"
        << "    \"user_data_offset\": " << report.geometry.user_data_offset << ",\n"
        << "    \"user_data_length\": " << report.geometry.user_data_length << ",\n"
        << "    \"xvc_info_offset\": " << report.geometry.xvc_info_offset << ",\n"
        << "    \"xvc_data_length\": " << report.geometry.xvc_data_length << ",\n"
        << "    \"dynamic_header_offset\": " << report.geometry.dynamic_header_offset << ",\n"
        << "    \"dynamic_header_length\": " << report.geometry.dynamic_header_length << ",\n"
        << "    \"drive_data_offset\": " << report.geometry.drive_data_offset << ",\n"
        << "    \"expected_minimum_file_size\": " << report.geometry.expected_minimum_file_size << "\n"
        << "  },\n"
        << "  \"xvc_metadata\": {\n"
        << "    \"present\": " << (report.xvc_metadata.present ? "true" : "false") << ",\n"
        << "    \"parsed\": " << (report.xvc_metadata.parsed ? "true" : "false") << ",\n"
        << "    \"version\": " << report.xvc_metadata.version << ",\n"
        << "    \"region_count\": " << report.xvc_metadata.region_count << ",\n"
        << "    \"key_count\": " << report.xvc_metadata.key_count << ",\n"
        << "    \"content_id\": \"" << EscapeJson(report.xvc_metadata.content_id_guid) << "\"\n"
        << "  },\n"
        << "  \"warnings\": [";
    for (size_t i = 0; i < report.warnings.size(); ++i) {
        if (i > 0u) oss << ", ";
        oss << "\"" << EscapeJson(report.warnings[i]) << "\"";
    }
    oss << "]\n}\n";
    return oss.str();
}

}  // namespace openscarlett
