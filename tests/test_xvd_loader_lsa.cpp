#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "openscarlett/kernel/console_lsa_policy.h"
#include "openscarlett/loader/pe_forwarder.h"
#include "openscarlett/loader/pe_inspector.h"
#include "openscarlett/loader/xvd_inspector.h"
#include "os_check.h"

namespace {

using namespace openscarlett;

template <typename T>
void WriteLe(std::vector<uint8_t>& buf, size_t offset, T value) {
    OS_REQUIRE(offset + sizeof(T) <= buf.size());
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

void WriteUtf16Ascii(std::vector<uint8_t>& buf, size_t offset, const char* text, size_t max_bytes) {
    const size_t len = std::strlen(text);
    const size_t max_chars = max_bytes / 2u;
    for (size_t i = 0u; i < len && i + 1u < max_chars; ++i) {
        const uint16_t ch = static_cast<uint16_t>(static_cast<unsigned char>(text[i]));
        WriteLe<uint16_t>(buf, offset + i * 2u, ch);
    }
}

std::vector<uint8_t> BuildSyntheticXvdHeader(uint32_t volume_flags,
                                             uint32_t format_version,
                                             uint64_t drive_size,
                                             XvdDiskType disk_type,
                                             XvdContentType content_type,
                                             uint32_t embedded_xvd_len,
                                             uint32_t user_data_len,
                                             uint32_t xvc_data_len,
                                             uint32_t dynamic_hdr_len,
                                             uint8_t mutable_data_pages,
                                             const char* sandbox_id,
                                             uint32_t ext_title_id = 0u) {
    std::vector<uint8_t> hdr(kXvdHeaderTotalSize, 0u);

    // Magic "msft-xvd" at 0x200
    std::memcpy(hdr.data() + kXvdMagicOffset, "msft-xvd", 8u);

    // Core geometry & metadata offsets
    WriteLe<uint32_t>(hdr, kXvdVolumeFlagsOffset, volume_flags);
    WriteLe<uint32_t>(hdr, kXvdFormatVersionOffset, format_version);
    WriteLe<uint64_t>(hdr, kXvdFileTimeCreatedOffset, 133500000000000000ull);
    WriteLe<uint64_t>(hdr, kXvdDriveSizeOffset, drive_size);

    // Deterministic VDUID (0x220) and UDUID (0x230)
    const uint8_t vduid[16] = {
        0x78, 0x56, 0x34, 0x12, 0xBC, 0x9A, 0xF0, 0xDE,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const uint8_t uduid[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    std::memcpy(hdr.data() + kXvdVduidOffset, vduid, 16u);
    std::memcpy(hdr.data() + kXvdUduidOffset, uduid, 16u);

    // Geometry / segment fields at 0x280, 0x284, 0x288, 0x28C, 0x290, 0x294, 0x298
    WriteLe<uint32_t>(hdr, kXvdTypeOffset, static_cast<uint32_t>(disk_type));
    WriteLe<uint32_t>(hdr, kXvdContentTypeOffset, static_cast<uint32_t>(content_type));
    WriteLe<uint32_t>(hdr, kXvdEmbeddedXvdLengthOffset, embedded_xvd_len);
    WriteLe<uint32_t>(hdr, kXvdUserDataLengthOffset, user_data_len);
    WriteLe<uint32_t>(hdr, kXvdXvcDataLengthOffset, xvc_data_len);
    WriteLe<uint32_t>(hdr, kXvdDynamicHeaderLengthOffset, dynamic_hdr_len);
    WriteLe<uint32_t>(hdr, kXvdBlockSizeOffset, kXvdDefaultBlockSize);

    // Optional TitleId in ExtEntry[0] at 0x29C
    if (ext_title_id != 0u) {
        WriteLe<uint32_t>(hdr, kXvdExtEntriesOffset + 0x00u, kXvdExtCodeTitleId);
        WriteLe<uint32_t>(hdr, kXvdExtEntriesOffset + 0x04u, 4u);
        WriteLe<uint64_t>(hdr, kXvdExtEntriesOffset + 0x08u, 0u);
        WriteLe<uint32_t>(hdr, kXvdExtEntriesOffset + 0x10u, ext_title_id);
    }

    // SandboxId at 0x38C
    if (sandbox_id != nullptr) {
        const size_t n = std::min<size_t>(std::strlen(sandbox_id), kXvdSandboxIdLength);
        std::memcpy(hdr.data() + kXvdSandboxIdOffset, sandbox_id, n);
    }

    // PackageVersion (v1=5, v2=4, v3=3, v4=2 -> "2.3.4.5") and RequiredSystemVersion (10.0.22621.1)
    WriteLe<uint16_t>(hdr, kXvdPackageVersionOffset + 0u, 5u);
    WriteLe<uint16_t>(hdr, kXvdPackageVersionOffset + 2u, 4u);
    WriteLe<uint16_t>(hdr, kXvdPackageVersionOffset + 4u, 3u);
    WriteLe<uint16_t>(hdr, kXvdPackageVersionOffset + 6u, 2u);

    WriteLe<uint16_t>(hdr, kXvdRequiredSystemVersionOffset + 0u, 1u);
    WriteLe<uint16_t>(hdr, kXvdRequiredSystemVersionOffset + 2u, 22621u);
    WriteLe<uint16_t>(hdr, kXvdRequiredSystemVersionOffset + 4u, 0u);
    WriteLe<uint16_t>(hdr, kXvdRequiredSystemVersionOffset + 6u, 10u);

    // MutableDataPageCount at 0x470
    hdr[kXvdMutableDataPageCountOffset] = mutable_data_pages;
    WriteLe<uint64_t>(hdr, kXvdSequenceNumberOffset, 42ull);
    WriteLe<uint32_t>(hdr, kXvdPersistentLocalStorageSizeOffset, 0x00100000u);

    return hdr;
}

void TestXvdInspector() {
    // 1. Verify exact 12 KiB XVD geometry with all key offsets (0x200, 0x284, 0x288, 0x28C, 0x470)
    //    and embedded unencrypted XvcInfo region.
    const uint32_t flags = XvdVolumeFlags::kReadOnly |
                           XvdVolumeFlags::kEncryptionDisabled |
                           XvdVolumeFlags::kEraSpecific;
    const uint64_t drive_size = 64ull * kXvdPageSize;  // 256 KiB (64 pages)
    const uint32_t embedded_len = 0x4000u;             // 4 pages
    const uint32_t user_data_len = 0x2000u;            // 2 pages
    const uint32_t xvc_data_len = 0x2000u;             // 2 pages
    const uint8_t mutable_pages = 3u;                  // 3 pages at offset 0x470

    auto buf = BuildSyntheticXvdHeader(flags, 3u, drive_size, XvdDiskType::Fixed,
                                       XvdContentType::Title, embedded_len, user_data_len,
                                       xvc_data_len, 0u, mutable_pages, "XDKS.1", 0x5CA01234u);

    // Compute expected offsets:
    //   Header:       0x0000 .. 0x3000 (12 KiB)
    //   EmbeddedXVD:  0x3000 .. 0x7000 (4 pages)
    //   MutableData:  0x7000 .. 0xA000 (3 pages from offset 0x470)
    //   Hashed pages: drive(64) + user(2) + xvc(2) + dyn(0) = 68 pages <= 170 => 1 hash tree level, 1 page!
    //   HashTree:     0xA000 .. 0xB000 (1 page)
    //   UserData:     0xB000 .. 0xD000 (2 pages)
    //   XvcInfo:      0xD000 .. 0xF000 (2 pages)
    //   DriveData:    0xF000 .. 0x4F000 (64 pages)
    const size_t expected_xvc_off = 0xD000u;
    const size_t full_size = 0xF000u + static_cast<size_t>(drive_size);
    buf.resize(full_size, 0u);

    // Populate unencrypted XvcInfo descriptor at 0xD000 with 2 region headers.
    const uint8_t xvc_cid[16] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0x99, 0x00, 0xAB, 0xCD};
    std::memcpy(buf.data() + expected_xvc_off + 0x000u, xvc_cid, 16u);
    WriteUtf16Ascii(buf, expected_xvc_off + 0xC10u, "OpenScarlett Test XVC", 0x100u);
    WriteLe<uint32_t>(buf, expected_xvc_off + 0xD10u, 1u);   // Version
    WriteLe<uint32_t>(buf, expected_xvc_off + 0xD14u, 2u);   // RegionCount
    WriteLe<uint16_t>(buf, expected_xvc_off + 0xD1Eu, 0u);   // KeyCount = 0 (unencrypted)
    WriteLe<uint32_t>(buf, expected_xvc_off + 0xD24u, 1u);   // InitialPlayRegionId
    WriteLe<uint64_t>(buf, expected_xvc_off + 0xD28u, 0xF000ull);
    WriteLe<uint32_t>(buf, expected_xvc_off + 0xD3Cu, 12u);  // UpdateSegmentCount

    const size_t reg0_off = expected_xvc_off + kXvcInfoHeaderMinSize;
    WriteLe<uint32_t>(buf, reg0_off + 0x00u, 1u);               // RegionId
    WriteLe<uint16_t>(buf, reg0_off + 0x04u, kXvcKeyIdNone);    // Unencrypted
    WriteLe<uint32_t>(buf, reg0_off + 0x08u, 0x11u);            // Resident | Present
    WriteUtf16Ascii(buf, reg0_off + 0x10u, "XVC-HD", 0x40u);
    WriteLe<uint64_t>(buf, reg0_off + 0x50u, 0x0000ull);
    WriteLe<uint64_t>(buf, reg0_off + 0x58u, 0x3000ull);

    const size_t reg1_off = reg0_off + kXvcRegionHeaderSize;
    WriteLe<uint32_t>(buf, reg1_off + 0x00u, 2u);               // RegionId
    WriteLe<uint16_t>(buf, reg1_off + 0x04u, 0u);               // KeyId 0 (encrypted marker)
    WriteLe<uint32_t>(buf, reg1_off + 0x08u, 0x12u);            // InitialPlay | Present
    WriteUtf16Ascii(buf, reg1_off + 0x10u, "FS-MD", 0x40u);
    WriteLe<uint64_t>(buf, reg1_off + 0x50u, 0xF000ull);
    WriteLe<uint64_t>(buf, reg1_off + 0x58u, drive_size);

    XvdInspectionReport rep{};
    Status st = InspectXvdBytes(buf, "synthetic_unencrypted.xvc", rep);
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.format_version == 3u);
    OS_REQUIRE(rep.disk_type == XvdDiskType::Fixed);
    OS_REQUIRE(rep.content_type == XvdContentType::Title);
    OS_REQUIRE(rep.is_xvc);
    OS_REQUIRE(!rep.is_encrypted);
    OS_REQUIRE(rep.is_read_only);
    OS_REQUIRE(rep.is_data_integrity_enabled);
    OS_REQUIRE(rep.is_era_specific);
    OS_REQUIRE(!rep.is_signed);
    OS_REQUIRE(!rep.is_payload_truncated);
    OS_REQUIRE(rep.can_inspect_filesystem_directly);
    OS_REQUIRE(rep.sandbox_id == "XDKS.1");
    OS_REQUIRE(rep.has_title_id && rep.title_id == 0x5CA01234u);
    OS_REQUIRE(rep.package_version == "2.3.4.5");
    OS_REQUIRE(rep.required_system_version == "10.0.22621.1");
    OS_REQUIRE(rep.vduid_guid == "12345678-9ABC-DEF0-1122-334455667788");
    OS_REQUIRE(rep.geometry.header_size == 0x3000u);
    OS_REQUIRE(rep.geometry.embedded_xvd_offset == 0x3000u);
    OS_REQUIRE(rep.geometry.embedded_xvd_length == 0x4000u);
    OS_REQUIRE(rep.geometry.mutable_data_offset == 0x7000u);
    OS_REQUIRE(rep.geometry.mutable_data_page_count == 3u);
    OS_REQUIRE(rep.geometry.mutable_data_length == 0x3000u);
    OS_REQUIRE(rep.geometry.hash_tree_offset == 0xA000u);
    OS_REQUIRE(rep.geometry.hash_tree_levels == 1u);
    OS_REQUIRE(rep.geometry.hash_tree_page_count == 1u);
    OS_REQUIRE(rep.geometry.user_data_offset == 0xB000u);
    OS_REQUIRE(rep.geometry.user_data_length == 0x2000u);
    OS_REQUIRE(rep.geometry.xvc_info_offset == 0xD000u);
    OS_REQUIRE(rep.geometry.drive_data_offset == 0xF000u);
    OS_REQUIRE(rep.geometry.expected_minimum_file_size == full_size);
    OS_REQUIRE(rep.xvc_metadata.present);
    OS_REQUIRE(rep.xvc_metadata.parsed);
    OS_REQUIRE(rep.xvc_metadata.description == "OpenScarlett Test XVC");
    OS_REQUIRE(rep.xvc_metadata.region_count == 2u);
    OS_REQUIRE(rep.xvc_metadata.regions.size() == 2u);
    OS_REQUIRE(rep.xvc_metadata.regions[0].description == "XVC-HD");
    OS_REQUIRE(!rep.xvc_metadata.regions[0].is_encrypted);
    OS_REQUIRE(rep.xvc_metadata.regions[1].description == "FS-MD");
    OS_REQUIRE(rep.xvc_metadata.regions[1].is_encrypted);

    const std::string text = FormatXvdInspectionText(rep);
    const std::string json = FormatXvdInspectionJson(rep);
    OS_REQUIRE(text.find("UNENCRYPTED / UNPACKED") != std::string::npos);
    OS_REQUIRE(json.find("\"is_encrypted\": false") != std::string::npos);
    OS_REQUIRE(json.find("\"sandbox_id\": \"XDKS.1\"") != std::string::npos);

    // 2. Encrypted Dynamic SystemOS XVD with ResiliencyEnabled + LegacySectorSize (512 B)
    const uint32_t enc_flags = XvdVolumeFlags::kReadOnly |
                               XvdVolumeFlags::kLegacySectorSize |
                               XvdVolumeFlags::kResiliencyEnabled |
                               XvdVolumeFlags::kRegionIdInXts;
    auto enc_hdr = BuildSyntheticXvdHeader(enc_flags, 3u, 500ull * kXvdPageSize,
                                           XvdDiskType::Dynamic, XvdContentType::SystemOS,
                                           0u, 0u, 0u, 0x1000u, 0u, "RETAIL");
    enc_hdr[0x40u] = 0xA5u;                   // Non-zero RSA signature area => signed
    enc_hdr[kXvdKeyMaterialOffset] = 0x5Au;   // Non-zero encrypted key slot presence
    // 500 + 1 dynamic hdr page = 501 hashed pages > 170 => 2 levels: ceil(501/170)=3 + ceil(501/28900)=1 => 4 pages, x2 resiliency = 8 pages
    XvdInspectionReport enc_rep{};
    st = InspectXvdBytes(enc_hdr, "encrypted_sysos.xvd", enc_rep, 0x20000ull);
    OS_REQUIRE(st.ok());
    OS_REQUIRE(enc_rep.is_encrypted);
    OS_REQUIRE(enc_rep.is_signed);
    OS_REQUIRE(enc_rep.has_key_material_present);
    OS_REQUIRE(!enc_rep.is_xvc);
    OS_REQUIRE(!enc_rep.can_inspect_filesystem_directly);
    OS_REQUIRE(enc_rep.disk_type == XvdDiskType::Dynamic);
    OS_REQUIRE(enc_rep.geometry.sector_size == 512u);
    OS_REQUIRE(enc_rep.geometry.hash_tree_levels == 2u);
    OS_REQUIRE(enc_rep.geometry.hash_tree_page_count == 8u);
    OS_REQUIRE(enc_rep.geometry.dynamic_header_offset == 0x3000u + 8u * 0x1000u);
    OS_REQUIRE(enc_rep.geometry.drive_data_offset == 0x3000u + 9u * 0x1000u);
    const std::string enc_text = FormatXvdInspectionText(enc_rep);
    OS_REQUIRE(enc_text.find("ENCRYPTED XVD/XVC container") != std::string::npos);

    // 3. Multi-level Merkle hash tree geometry math checks (1, 2, 3 levels + overflow rejection)
    uint64_t lvls = 0u, hpages = 0u;
    OS_REQUIRE(ComputeXvdHashTreeGeometry(170u, false, lvls, hpages) && lvls == 1u && hpages == 1u);
    OS_REQUIRE(ComputeXvdHashTreeGeometry(171u, false, lvls, hpages) && lvls == 2u && hpages == 3u);
    OS_REQUIRE(ComputeXvdHashTreeGeometry(28901u, false, lvls, hpages) && lvls == 3u && hpages == 174u);
    OS_REQUIRE(!ComputeXvdHashTreeGeometry(0x31C84B11ull, false, lvls, hpages));

    // 4. Streaming InspectXvdFile on disk (portable across both CTest build dir & standalone zip bundle)
    std::error_code ec;
    auto tmp_dir = std::filesystem::temp_directory_path(ec);
    if (ec || tmp_dir.empty()) {
        tmp_dir = std::filesystem::current_path(ec);
    }
    const auto tmp_path = tmp_dir / "openscarlett_test_synthetic_xvd.xvc";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        OS_REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    XvdInspectionReport file_rep{};
    OS_REQUIRE(InspectXvdFile(tmp_path.string(), file_rep).ok());
    OS_REQUIRE(file_rep.xvc_metadata.parsed);
    OS_REQUIRE(file_rep.xvc_metadata.regions.size() == 2u);
    OS_REQUIRE(file_rep.geometry.drive_data_offset == 0xF000u);
    std::filesystem::remove(tmp_path, ec);

    // 5. Negative controls: truncated, bad magic, bad version, bad type, zero/unaligned/overflow DriveSize
    XvdInspectionReport bad{};
    OS_REQUIRE(!InspectXvdBytes(std::vector<uint8_t>(0x1000u, 0u), "short.xvd", bad).ok());

    auto bad_magic = BuildSyntheticXvdHeader(flags, 3u, drive_size, XvdDiskType::Fixed,
                                             XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    bad_magic[kXvdMagicOffset] = 'X';
    OS_REQUIRE(InspectXvdBytes(bad_magic, "bad_magic.xvd", bad).code == StatusCode::UnsupportedFormat);

    auto bad_ver = BuildSyntheticXvdHeader(flags, 99u, drive_size, XvdDiskType::Fixed,
                                           XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    OS_REQUIRE(InspectXvdBytes(bad_ver, "bad_ver.xvd", bad).code == StatusCode::UnsupportedFormat);

    auto bad_type = BuildSyntheticXvdHeader(flags, 3u, drive_size, XvdDiskType::Fixed,
                                            XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    WriteLe<uint32_t>(bad_type, kXvdTypeOffset, 7u);
    OS_REQUIRE(InspectXvdBytes(bad_type, "bad_type.xvd", bad).code == StatusCode::ParseError);

    auto zero_drive = BuildSyntheticXvdHeader(flags, 3u, 0u, XvdDiskType::Fixed,
                                              XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    OS_REQUIRE(InspectXvdBytes(zero_drive, "zero_drive.xvd", bad).code == StatusCode::ParseError);

    auto unaligned_drive = BuildSyntheticXvdHeader(flags, 3u, 4097u, XvdDiskType::Fixed,
                                                   XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    OS_REQUIRE(InspectXvdBytes(unaligned_drive, "unaligned.xvd", bad).code == StatusCode::ParseError);

    auto overflow_drive = BuildSyntheticXvdHeader(flags, 3u, 0xFFFFFFFFFFFFF000ull, XvdDiskType::Fixed,
                                                  XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    OS_REQUIRE(InspectXvdBytes(overflow_drive, "overflow.xvd", bad).code == StatusCode::ParseError);

    // Header-only Fixed buffer detects truncated payload safely
    auto hdr_only = BuildSyntheticXvdHeader(flags, 3u, drive_size, XvdDiskType::Fixed,
                                            XvdContentType::Data, 0u, 0u, 0u, 0u, 0u, "RETAIL");
    XvdInspectionReport trunc_rep{};
    OS_REQUIRE(InspectXvdBytes(hdr_only, "trunc_payload.xvd", trunc_rep).ok());
    OS_REQUIRE(trunc_rep.is_payload_truncated);
    OS_REQUIRE(!trunc_rep.can_inspect_filesystem_directly);
}

void TestPeForwarderResolver(const char* argv0) {
    // 1. Mixed-case DLL name normalization & .dll stripping/preservation
    OS_REQUIRE(StripDllExtension("KernelBase.DLL") == "KernelBase");
    OS_REQUIRE(StripDllExtension("Microsoft.Xbox.Services.141.GDK.C.Thunks.Dll") ==
               "Microsoft.Xbox.Services.141.GDK.C.Thunks");
    OS_REQUIRE(EnsureDllExtension("KERNELX") == "KERNELX.dll");
    OS_REQUIRE(EnsureDllExtension("C:\\Windows\\System32\\NtDll.DLL") == "NtDll.dll");
    OS_REQUIRE(NormalizeForwarderDllName("API-MS-WIN-CORE-SYNCH-L1-2-0", true) ==
               "api-ms-win-core-synch-l1-2-0.dll");
    OS_REQUIRE(NormalizeForwarderDllName("KERNELX.DLL", false) == "kernelx");
    OS_REQUIRE(IsApiSetContractName("API-MS-WIN-CORE-SYNCH-L1-2-0.dll"));
    OS_REQUIRE(IsApiSetContractName("ext-ms-win-ntuser-window-l1-1-0"));
    OS_REQUIRE(!IsApiSetContractName("KernelBase.dll"));

    // 2. Parsing mixed-case forwarders, explicit .DLL before symbol, multi-dot DLLs, and ordinals
    ParsedPeForwarder p{};
    OS_REQUIRE(ParseMixedCaseForwarder("API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress", p));
    OS_REQUIRE(p.dll_stem == "api-ms-win-core-synch-l1-2-0");
    OS_REQUIRE(p.normalized_dll == "api-ms-win-core-synch-l1-2-0.dll");
    OS_REQUIRE(p.symbol_name == "WaitOnAddress");
    OS_REQUIRE(!p.by_ordinal && p.is_api_set);

    OS_REQUIRE(ParseMixedCaseForwarder("KernelBase.SleepConditionVariableSRW", p));
    OS_REQUIRE(p.normalized_dll == "kernelbase.dll");
    OS_REQUIRE(p.symbol_name == "SleepConditionVariableSRW");

    OS_REQUIRE(ParseMixedCaseForwarder("KERNEL32.DLL.CreateFileW", p));
    OS_REQUIRE(p.normalized_dll == "kernel32.dll");
    OS_REQUIRE(p.symbol_name == "CreateFileW");

    OS_REQUIRE(ParseMixedCaseForwarder("NTDLL.#123", p));
    OS_REQUIRE(p.normalized_dll == "ntdll.dll");
    OS_REQUIRE(p.by_ordinal && p.ordinal == 123u && p.symbol_name.empty());

    OS_REQUIRE(ParseMixedCaseForwarder("Microsoft.Xbox.Services.141.GDK.C.Thunks.XblInitialize", p));
    OS_REQUIRE(p.normalized_dll == "microsoft.xbox.services.141.gdk.c.thunks.dll");
    OS_REQUIRE(p.symbol_name == "XblInitialize");

    // Malformed forwarder rejections
    OS_REQUIRE(!ParseMixedCaseForwarder("", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("NODOT", p));
    OS_REQUIRE(!ParseMixedCaseForwarder(".CreateFileW", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("KERNEL32.", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("NTDLL.#", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("NTDLL.#70000", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("NTDLL.#12a", p));
    OS_REQUIRE(!ParseMixedCaseForwarder("KERNEL32.Bad Symbol", p));

    // 3. Resolution against OpenScarlett's 30 shims, aliases, and Windows API-sets
    PeForwarderResolver resolver;
    resolver.RegisterNativeGuestDll("CustomMiddleware.DLL");

    auto r1 = resolver.ResolveForwarder("API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress");
    OS_REQUIRE(r1.ok);
    OS_REQUIRE(r1.kind == ForwarderResolutionKind::ApiSetRedirected);
    OS_REQUIRE(r1.api_set_resolved);
    OS_REQUIRE(r1.routed_to_windows_system);
    OS_REQUIRE(r1.resolved_dll == "kernelbase.dll");
    OS_REQUIRE(r1.resolved_symbol == "WaitOnAddress");

    auto r2 = resolver.ResolveForwarder("KERNELX.XMemAlloc");
    OS_REQUIRE(r2.ok);
    OS_REQUIRE(r2.kind == ForwarderResolutionKind::OpenScarlettShim);
    OS_REQUIRE(r2.routed_to_openscarlett_shim);
    OS_REQUIRE(r2.resolved_dll == "kernelx.dll");

    auto r3 = resolver.ResolveForwarder("D3D12_X.D3D12XboxCreateDevice");
    OS_REQUIRE(r3.ok);
    OS_REQUIRE(r3.kind == ForwarderResolutionKind::OpenScarlettShim);
    OS_REQUIRE(r3.resolved_dll == "d3d12_xs.dll");

    auto r4 = resolver.ResolveForwarder("Microsoft.Xbox.Services.141.GDK.C.Thunks.XblInitialize");
    OS_REQUIRE(r4.ok);
    OS_REQUIRE(r4.kind == ForwarderResolutionKind::OpenScarlettShim);
    OS_REQUIRE(r4.resolved_dll == "Microsoft.Xbox.Services.GDK.C.dll");

    auto r5 = resolver.ResolveForwarder("API-MS-WIN-GAMING-TCUI-L1-1-0.ShowGameInviteUI");
    OS_REQUIRE(r5.ok);
    OS_REQUIRE(r5.kind == ForwarderResolutionKind::OpenScarlettShim);
    OS_REQUIRE(r5.api_set_resolved);
    OS_REQUIRE(r5.resolved_dll == "xgameruntime.dll");

    auto r6 = resolver.ResolveForwarder("NTDLL.#123");
    OS_REQUIRE(r6.ok);
    OS_REQUIRE(r6.kind == ForwarderResolutionKind::WindowsSystemDll);
    OS_REQUIRE(r6.by_ordinal && r6.resolved_ordinal == 123u);

    auto r7 = resolver.ResolveForwarder("custommiddleware.InitEngine");
    OS_REQUIRE(r7.ok);
    OS_REQUIRE(r7.kind == ForwarderResolutionKind::NativeGuestDll);
    OS_REQUIRE(r7.resolved_dll == "custommiddleware.dll");

    // 4. Multi-hop recursive forwarder chain + mixed-case cycle protection + depth limit
    const std::unordered_map<std::string, std::string> fwd_table = {
        {"kernel32.dll!WaitOnAddress", "API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress"},
        {"kernelbase.dll!WaitOnAddress", "NtDll.#123"},
        // Mixed-case 3-hop cycle: alpha -> BETA.DLL -> Alpha.Dll -> alpha!
        {"alpha.dll!LoopFn", "BETA.DLL.LoopMid"},
        {"beta.dll!LoopMid", "Alpha.Dll.LoopFn"},
        // Deep chain exceeding max_depth=3
        {"hop0.dll!Go", "HOP1.Go"},
        {"hop1.dll!Go", "Hop2.dll.Go"},
        {"hop2.dll!Go", "hop3.Go"},
        {"hop3.dll!Go", "hop4.Go"},
        {"hop4.dll!Go", "KERNEL32.CreateFileW"},
    };
    PeForwarderLookupFn lookup = [&](const std::string& norm_dll,
                                     const std::string& sym,
                                     uint16_t ord,
                                     bool by_ord,
                                     std::string& out_next) -> bool {
        const std::string key = norm_dll + "!" + (by_ord ? ("#" + std::to_string(ord)) : sym);
        auto it = fwd_table.find(key);
        if (it == fwd_table.end()) {
            return false;
        }
        out_next = it->second;
        return true;
    };

    auto chain_ok = resolver.ResolveForwarder("KERNEL32.WaitOnAddress", lookup);
    OS_REQUIRE(chain_ok.ok);
    OS_REQUIRE(chain_ok.api_set_resolved);
    OS_REQUIRE(chain_ok.resolved_dll == "ntdll.dll");
    OS_REQUIRE(chain_ok.by_ordinal && chain_ok.resolved_ordinal == 123u);
    OS_REQUIRE(chain_ok.chain.size() == 3u);

    auto cycle_res = resolver.ResolveForwarder("ALPHA.LoopFn", lookup);
    OS_REQUIRE(!cycle_res.ok);
    OS_REQUIRE(cycle_res.cycle_detected);
    OS_REQUIRE(cycle_res.kind == ForwarderResolutionKind::CycleError);

    auto depth_res = resolver.ResolveForwarder("HOP0.Go", lookup, 3u);
    OS_REQUIRE(!depth_res.ok);
    OS_REQUIRE(depth_res.depth_exceeded);
    OS_REQUIRE(depth_res.kind == ForwarderResolutionKind::DepthLimitError);

    // 5. Preflight readiness report (both clean list and real openscarlett.exe PE report)
    auto clean_pf = resolver.BuildPreflightReportFromForwarders(
        "clean_forwarders",
        {
            "API-MS-WIN-CORE-SYNCH-L1-2-0.WaitOnAddress",
            "KernelBase.SleepConditionVariableSRW",
            "KERNELX.XMemAlloc",
            "NTDLL.#123",
            "XGamingRuntimeThunks.XUserGetId",
        });
    OS_REQUIRE(clean_pf.ready_to_launch);
    OS_REQUIRE(clean_pf.total_entries == 5u);
    OS_REQUIRE(clean_pf.openscarlett_shim_count == 2u);
    OS_REQUIRE(clean_pf.apiset_redirected_count == 1u);
    OS_REQUIRE(clean_pf.unresolved_count == 0u);
    OS_REQUIRE(FormatPeForwarderPreflightText(clean_pf).find("ReadyToLaunch    : YES") != std::string::npos);
    OS_REQUIRE(FormatPeForwarderPreflightJson(clean_pf).find("\"ready_to_launch\": true") != std::string::npos);

    auto blocked_pf = resolver.BuildPreflightReportFromForwarders(
        "blocked_forwarders",
        {"ALPHA.LoopFn", "UnknownVendor.MissingFn", "BADFORMAT"},
        lookup);
    OS_REQUIRE(!blocked_pf.ready_to_launch);
    OS_REQUIRE(blocked_pf.cycle_error_count == 1u);
    OS_REQUIRE(blocked_pf.unresolved_count == 1u);
    OS_REQUIRE(blocked_pf.malformed_error_count == 1u);

    // Inspect compiled openscarlett.exe imports through preflight analyzer
    std::filesystem::path exe_path;
    if (argv0 != nullptr) {
        auto beside = std::filesystem::path(argv0).parent_path() / "openscarlett.exe";
        if (std::filesystem::exists(beside)) {
            exe_path = beside;
        }
    }
    if (exe_path.empty()) {
        exe_path = std::filesystem::path(OPENSCARLETT_BINARY_DIR) / "openscarlett.exe";
    }
    if (std::filesystem::exists(exe_path)) {
        PeInspectionReport pe_rep{};
        OS_REQUIRE(InspectPeFile(exe_path.string(), pe_rep).ok());
        auto exe_pf = resolver.BuildPreflightReport(pe_rep);
        OS_REQUIRE(exe_pf.total_entries > 10u);
        OS_REQUIRE(exe_pf.cycle_error_count == 0u);
        OS_REQUIRE(exe_pf.malformed_error_count == 0u);
    }
}

void TestConsoleLsaPolicy() {
    // 1. Default policy state & SID binary wire format validation
    ConsoleLsaPolicy policy;
    const auto def_snap = policy.Snapshot();
    OS_REQUIRE(!def_snap.package.package_family_name.empty());
    OS_REQUIRE(def_snap.package.publisher_id.size() == 13u);
    OS_REQUIRE(def_snap.package.app_user_model_id ==
               def_snap.package.package_family_name + "!Game");
    OS_REQUIRE(def_snap.package.sandbox_id == "RETAIL");
    OS_REQUIRE(!def_snap.is_dev_sandbox);
    OS_REQUIRE(def_snap.package.store_id.size() == 12u && def_snap.package.store_id[0] == '9');
    OS_REQUIRE(def_snap.package.content_id.size() == 36u);
    OS_REQUIRE(def_snap.lsa.machine_sid.text.rfind("S-1-5-21-", 0) == 0u);
    OS_REQUIRE(def_snap.lsa.user_sid.text == def_snap.lsa.machine_sid.text + "-1001");
    OS_REQUIRE(def_snap.lsa.package_sid.text.rfind("S-1-15-2-", 0) == 0u);

    // Binary Win32 SID layout check: [Rev=1, SubCount, Auth(6B BE), SubAuths(4B LE each)]
    const auto& psid = def_snap.lsa.package_sid;
    OS_REQUIRE(psid.binary.size() == 8u + psid.sub_authorities.size() * 4u);
    OS_REQUIRE(psid.binary[0] == 1u);
    OS_REQUIRE(psid.binary[1] == psid.sub_authorities.size());
    OS_REQUIRE(psid.binary[7] == 15u);
    uint32_t sub0 = 0u;
    std::memcpy(&sub0, psid.binary.data() + 8u, sizeof(sub0));
    OS_REQUIRE(sub0 == 2u);

    // Default capabilities (RETAIL: core game caps granted, DebugTooling denied)
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::AppContainer));
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::XboxLive));
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::TitleStorage));
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::GameSave));
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::Multiplayer));
    OS_REQUIRE(policy.HasCapabilityByName("XboxLive"));
    OS_REQUIRE(policy.HasCapabilityByName("gamesave"));
    OS_REQUIRE(!policy.HasCapability(ConsoleCapability::DebugTooling));

    // 2. Load from MicrosoftGame.Config XML (with StoreId, ContentId, SandboxId)
    const char* kSampleXml = R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="Contoso.ScarlettDemo"
            Publisher="CN=Contoso Studios, O=Contoso"
            Version="1.2.3.4"/>
  <TitleId>5CA0ABCD</TitleId>
  <MSAAppId>0000000040123456</MSAAppId>
  <StoreId>9NBLGGH4DEMO</StoreId>
  <ContentId>11112222-3333-4444-8555-666677778888</ContentId>
  <SandboxId>XDKS.1</SandboxId>
  <ExecutableList>
    <Executable Name="ScarlettDemo.exe" Id="ScarlettApp" TargetDeviceFamily="Scarlett"/>
  </ExecutableList>
  <ShellVisuals DefaultDisplayName="Contoso Scarlett Demo"/>
</Game>)";

    OS_REQUIRE(policy.LoadFromMicrosoftGameConfigXml(kSampleXml, "MicrosoftGame.Config").ok());
    const auto cfg_pkg = policy.GetPackageIdentity();
    const auto cfg_lsa = policy.GetLsaIdentity();
    OS_REQUIRE(cfg_pkg.package_name == "Contoso.ScarlettDemo");
    OS_REQUIRE(cfg_pkg.title_id == 0x5CA0ABCDu);
    OS_REQUIRE(cfg_pkg.title_id_hex == "5CA0ABCD");
    OS_REQUIRE(cfg_pkg.app_id == "ScarlettApp");
    OS_REQUIRE(cfg_pkg.app_user_model_id == cfg_pkg.package_family_name + "!ScarlettApp");
    OS_REQUIRE(cfg_pkg.store_id == "9NBLGGH4DEMO");
    OS_REQUIRE(cfg_pkg.content_id == "11112222-3333-4444-8555-666677778888");
    OS_REQUIRE(cfg_pkg.sandbox_id == "XDKS.1");
    OS_REQUIRE(cfg_pkg.msa_app_id == "0000000040123456");
    OS_REQUIRE(policy.HasCapability(ConsoleCapability::DebugTooling));  // Auto-enabled in XDKS.1!

    // Determinism check across two independent instances
    ConsoleLsaPolicy policy2;
    OS_REQUIRE(policy2.LoadFromMicrosoftGameConfigXml(kSampleXml, "MicrosoftGame.Config").ok());
    OS_REQUIRE(policy2.GetPackageIdentity().package_family_name == cfg_pkg.package_family_name);
    OS_REQUIRE(policy2.GetLsaIdentity().machine_sid.binary == cfg_lsa.machine_sid.binary);
    OS_REQUIRE(policy2.GetLsaIdentity().user_sid.binary == cfg_lsa.user_sid.binary);
    OS_REQUIRE(policy2.GetLsaIdentity().package_sid.binary == cfg_lsa.package_sid.binary);
    OS_REQUIRE(policy2.GetLsaIdentity().console_id == cfg_lsa.console_id);

    // Bidirectional in-process LSA account <-> SID lookup
    ConsoleSid found_sid{};
    std::string acct, dom;
    OS_REQUIRE(policy.LookupSidByAccountName("SCARLETT\\ScarlettPlayer", found_sid));
    OS_REQUIRE(found_sid.text == cfg_lsa.user_sid.text);
    OS_REQUIRE(policy.LookupAccountNameBySid(cfg_lsa.user_sid.text, acct, dom));
    OS_REQUIRE(acct == "ScarlettPlayer" && dom == "SCARLETT");

    OS_REQUIRE(policy.LookupSidByAccountName(cfg_pkg.package_family_name, found_sid));
    OS_REQUIRE(found_sid.text == cfg_lsa.package_sid.text);
    OS_REQUIRE(policy.LookupAccountNameBySid(cfg_lsa.package_sid.text, acct, dom));
    OS_REQUIRE(acct == cfg_pkg.package_family_name && dom == "APPLICATION PACKAGE AUTHORITY");

    // Load staged untouched Microsoft Xbox-GDK-Samples MicrosoftGameConfig.mgc files
    // when running inside the source checkout (skipped gracefully in standalone portable zip bundles).
    std::error_code mgc_ec;
    const auto extra_dir = std::filesystem::path(OPENSCARLETT_SOURCE_DIR) / "tests" / "gdkx_bringup_extra";
    const auto tex_mgc = extra_dir / "SimpleTexture" / "MicrosoftGameConfig.mgc";
    const auto inst_mgc = extra_dir / "SimpleInstancing" / "MicrosoftGameConfig.mgc";
    if (std::filesystem::exists(extra_dir, mgc_ec)) {
        OS_REQUIRE(std::filesystem::exists(tex_mgc, mgc_ec));
        OS_REQUIRE(std::filesystem::exists(inst_mgc, mgc_ec));
        ConsoleLsaPolicy sample_policy;
        OS_REQUIRE(sample_policy.LoadFromMicrosoftGameConfigFile(tex_mgc.string()).ok());
        OS_REQUIRE(sample_policy.GetPackageIdentity().display_name == "SimpleTexture");
        OS_REQUIRE(sample_policy.GetPackageIdentity().app_id == "Game");
        OS_REQUIRE(sample_policy.LoadFromMicrosoftGameConfigFile(inst_mgc.string()).ok());
        OS_REQUIRE(sample_policy.GetPackageIdentity().display_name == "SimpleInstancing");
    }

    // Formatting diagnostics
    OS_REQUIRE(policy.FormatSummaryText().find("Contoso.ScarlettDemo_") != std::string::npos);
    OS_REQUIRE(policy.FormatSummaryJson().find("\"sandbox_id\": \"XDKS.1\"") != std::string::npos);

    // 3. Multi-threaded concurrent reads + policy mutations (race-free consistency)
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reader_checks{0u};
    std::vector<std::thread> threads;

    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snap = policy.Snapshot();
                OS_REQUIRE(!snap.package.package_family_name.empty());
                OS_REQUIRE(snap.package.app_user_model_id.rfind(snap.package.package_family_name + "!", 0) == 0u);
                OS_REQUIRE(snap.lsa.package_sid.text.rfind("S-1-15-2-", 0) == 0u);
                (void)policy.CheckCapability(ConsoleCapability::Multiplayer);
                reader_checks.fetch_add(1u, std::memory_order_relaxed);
            }
        });
    }

    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&, w]() {
            for (int iter = 0; iter < 250; ++iter) {
                policy.SetSandboxId(((iter + w) % 2 == 0) ? "RETAIL" : "XDKS.1");
                policy.SetCapability(ConsoleCapability::Multiplayer, ((iter + w) % 3) != 0);
                policy.SetTitleId(0x5CA00000u + static_cast<uint32_t>(iter + w * 1000));
            }
        });
    }

    threads[4].join();
    threads[5].join();
    stop.store(true, std::memory_order_relaxed);
    for (int r = 0; r < 4; ++r) {
        threads[static_cast<size_t>(r)].join();
    }
    OS_REQUIRE(reader_checks.load() > 0u);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    TestXvdInspector();
    TestPeForwarderResolver((argc > 0 && argv != nullptr) ? argv[0] : nullptr);
    TestConsoleLsaPolicy();
    std::printf("[PASS] test_xvd_loader_lsa (XVD 12 KiB geometry + mixed-case PE forwarders + Console LSA/PFN policy)\n");
    return 0;
}
