#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "openscarlett/launcher/updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace openscarlett::launcher {

namespace {

constexpr wchar_t kUserAgentW[] = L"OpenScarlett-Launcher/0.4.0";
constexpr size_t kMaxApiResponseBytes = 2u * 1024u * 1024u;        // 2 MiB
constexpr size_t kMaxSha256FileBytes = 64u * 1024u;                // 64 KiB
constexpr uint64_t kMaxZipDownloadBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB
constexpr size_t kFileHashChunkBytes = 64u * 1024u;                // 64 KiB
constexpr int kMaxJsonDepth = 64;
constexpr DWORD kExpandArchiveTimeoutMs = 120000u;

inline bool NtSuccess(NTSTATUS status) noexcept {
    return status >= 0;
}

inline char AsciiLower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline bool IsAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool IsAsciiDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool IsAsciiHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string ToLowerAscii(const std::string& input) {
    std::string out = input;
    for (char& c : out) {
        c = AsciiLower(c);
    }
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && IsAsciiSpace(s[start])) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && IsAsciiSpace(s[end - 1])) {
        --end;
    }
    return s.substr(start, end - start);
}

bool EndsWithCaseInsensitive(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) {
        return false;
    }
    const size_t offset = str.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (AsciiLower(str[offset + i]) != AsciiLower(suffix[i])) {
            return false;
        }
    }
    return true;
}

bool StartsWithCaseInsensitive(const std::string& str, const std::string& prefix) {
    if (prefix.size() > str.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (AsciiLower(str[i]) != AsciiLower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool EqualsCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::string FormatWin32Error(const std::string& context, DWORD err) {
    std::ostringstream oss;
    oss << context << " (Win32 error " << err << ")";
    return oss.str();
}

// RAII wrapper for Win32 HANDLEs.
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

    HANDLE Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// RAII wrapper for WinHTTP HINTERNET handles.
class UniqueWinHttp {
public:
    UniqueWinHttp() = default;
    explicit UniqueWinHttp(HINTERNET h) noexcept : handle_(h) {}
    ~UniqueWinHttp() { Reset(); }

    UniqueWinHttp(const UniqueWinHttp&) = delete;
    UniqueWinHttp& operator=(const UniqueWinHttp&) = delete;

    UniqueWinHttp(UniqueWinHttp&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueWinHttp& operator=(UniqueWinHttp&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    void Reset(HINTERNET h = nullptr) noexcept {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
        handle_ = h;
    }

    HINTERNET Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

// RAII wrapper for Windows BCrypt SHA-256 streaming hash.
class BCryptSha256Hasher {
public:
    BCryptSha256Hasher() = default;
    ~BCryptSha256Hasher() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
        }
        if (alg_ != nullptr) {
            BCryptCloseAlgorithmProvider(alg_, 0);
            alg_ = nullptr;
        }
    }

    BCryptSha256Hasher(const BCryptSha256Hasher&) = delete;
    BCryptSha256Hasher& operator=(const BCryptSha256Hasher&) = delete;

    Status Init() {
        NTSTATUS st = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!NtSuccess(st)) {
            return Status::Error(StatusCode::IoError, "BCryptOpenAlgorithmProvider(SHA256) failed");
        }

        DWORD object_length = 0;
        DWORD cb_data = 0;
        st = BCryptGetProperty(
            alg_,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_length),
            sizeof(object_length),
            &cb_data,
            0);
        if (!NtSuccess(st) || object_length == 0) {
            return Status::Error(StatusCode::IoError, "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");
        }

        st = BCryptGetProperty(
            alg_,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_length_),
            sizeof(hash_length_),
            &cb_data,
            0);
        if (!NtSuccess(st) || hash_length_ != 32u) {
            return Status::Error(StatusCode::IoError, "BCryptGetProperty(BCRYPT_HASH_LENGTH) failed");
        }

        hash_object_.resize(object_length);
        st = BCryptCreateHash(
            alg_,
            &hash_,
            hash_object_.data(),
            static_cast<ULONG>(hash_object_.size()),
            nullptr,
            0,
            0);
        if (!NtSuccess(st)) {
            return Status::Error(StatusCode::IoError, "BCryptCreateHash failed");
        }

        return Status::Ok();
    }

    Status Update(const uint8_t* data, size_t size) {
        if (size == 0) {
            return Status::Ok();
        }
        if (data == nullptr) {
            return Status::Error(StatusCode::InvalidArgument, "Null input buffer with non-zero size");
        }

        constexpr size_t kMaxChunk = 0x40000000u;  // 1 GiB per ULONG call
        size_t offset = 0;
        while (offset < size) {
            const size_t chunk = std::min(size - offset, kMaxChunk);
            NTSTATUS st = BCryptHashData(
                hash_,
                const_cast<PUCHAR>(data + offset),
                static_cast<ULONG>(chunk),
                0);
            if (!NtSuccess(st)) {
                return Status::Error(StatusCode::IoError, "BCryptHashData failed");
            }
            offset += chunk;
        }
        return Status::Ok();
    }

    Status Finalize(std::string& out_hex_lower) {
        std::array<uint8_t, 32> digest{};
        NTSTATUS st = BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (!NtSuccess(st)) {
            return Status::Error(StatusCode::IoError, "BCryptFinishHash failed");
        }

        static constexpr char kHexDigits[] = "0123456789abcdef";
        out_hex_lower.resize(64);
        for (size_t i = 0; i < digest.size(); ++i) {
            out_hex_lower[i * 2] = kHexDigits[(digest[i] >> 4) & 0x0Fu];
            out_hex_lower[i * 2 + 1] = kHexDigits[digest[i] & 0x0Fu];
        }
        return Status::Ok();
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> hash_object_;
    DWORD hash_length_ = 0;
};

// SemVer helpers
bool ParseNonNegativeInt(const std::string& token, int& out_value) {
    if (token.empty()) {
        return false;
    }
    int64_t accum = 0;
    for (char c : token) {
        if (!IsAsciiDigit(c)) {
            return false;
        }
        accum = accum * 10 + (c - '0');
        if (accum > INT_MAX) {
            return false;
        }
    }
    out_value = static_cast<int>(accum);
    return true;
}

bool IsAllDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!IsAsciiDigit(c)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> SplitByDot(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t dot = s.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

int CompareNumericStrings(const std::string& a, const std::string& b) {
    size_t ia = 0;
    while (ia + 1 < a.size() && a[ia] == '0') {
        ++ia;
    }
    size_t ib = 0;
    while (ib + 1 < b.size() && b[ib] == '0') {
        ++ib;
    }
    const size_t len_a = a.size() - ia;
    const size_t len_b = b.size() - ib;
    if (len_a != len_b) {
        return (len_a < len_b) ? -1 : 1;
    }
    for (size_t i = 0; i < len_a; ++i) {
        if (a[ia + i] != b[ib + i]) {
            return (a[ia + i] < b[ib + i]) ? -1 : 1;
        }
    }
    return 0;
}

// Minimal JSON parser tailored for GitHub /releases/latest response.
class MinimalJsonParser {
public:
    explicit MinimalJsonParser(const std::string& text) : src_(text) {}

    struct AssetEntry {
        std::string name;
        std::string download_url;
    };

    Status ParseGitHubRelease(UpdateReleaseInfo& out_info) {
        out_info = UpdateReleaseInfo{};
        std::vector<AssetEntry> assets;

        SkipWs();
        if (!Consume('{')) {
            return Status::Error(StatusCode::ParseError, "Expected JSON object at root");
        }

        SkipWs();
        if (!Consume('}')) {
            while (true) {
                SkipWs();
                std::string key;
                Status st = ParseString(key);
                if (!st.ok()) {
                    return st;
                }
                SkipWs();
                if (!Consume(':')) {
                    return Status::Error(StatusCode::ParseError, "Expected ':' after object key");
                }
                SkipWs();

                if (key == "tag_name") {
                    st = ParseStringOrNull(out_info.tag_name);
                } else if (key == "name") {
                    st = ParseStringOrNull(out_info.release_name);
                } else if (key == "body") {
                    st = ParseStringOrNull(out_info.body_markdown);
                } else if (key == "html_url") {
                    st = ParseStringOrNull(out_info.html_url);
                } else if (key == "published_at") {
                    st = ParseStringOrNull(out_info.published_at);
                } else if (key == "assets") {
                    st = ParseAssetsArray(assets);
                } else {
                    st = SkipValue(1);
                }

                if (!st.ok()) {
                    return st;
                }

                SkipWs();
                if (Consume(',')) {
                    continue;
                }
                if (Consume('}')) {
                    break;
                }
                return Status::Error(StatusCode::ParseError, "Expected ',' or '}' in root JSON object");
            }
        }

        SkipWs();
        if (pos_ != src_.size()) {
            return Status::Error(StatusCode::ParseError, "Unexpected trailing characters after JSON object");
        }

        if (out_info.tag_name.empty()) {
            return Status::Error(StatusCode::ParseError, "GitHub release JSON missing non-empty tag_name");
        }

        SelectWindowsAssets(out_info.tag_name, assets, out_info);
        return Status::Ok();
    }

private:
    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    static void SelectWindowsAssets(const std::string& tag,
                                    const std::vector<AssetEntry>& assets,
                                    UpdateReleaseInfo& out_info) {
        const std::string exact_zip = "OpenScarlett-" + tag + "-win64.zip";
        const std::string exact_sha = exact_zip + ".sha256";

        // Pass 1: exact tag-matched zip asset.
        for (const auto& a : assets) {
            if (EqualsCaseInsensitive(a.name, exact_zip)) {
                out_info.zip_asset_name = a.name;
                out_info.zip_download_url = a.download_url;
                break;
            }
        }

        // Pass 2: fallback to any OpenScarlett-*-win64.zip, then any *-win64.zip.
        if (out_info.zip_asset_name.empty()) {
            for (const auto& a : assets) {
                if (StartsWithCaseInsensitive(a.name, "OpenScarlett-") &&
                    EndsWithCaseInsensitive(a.name, "-win64.zip")) {
                    out_info.zip_asset_name = a.name;
                    out_info.zip_download_url = a.download_url;
                    break;
                }
            }
        }
        if (out_info.zip_asset_name.empty()) {
            for (const auto& a : assets) {
                if (EndsWithCaseInsensitive(a.name, "-win64.zip")) {
                    out_info.zip_asset_name = a.name;
                    out_info.zip_download_url = a.download_url;
                    break;
                }
            }
        }

        // Pass 1 for SHA-256: exact match for selected zip (or exact_sha).
        const std::string target_sha =
            out_info.zip_asset_name.empty() ? exact_sha : (out_info.zip_asset_name + ".sha256");
        for (const auto& a : assets) {
            if (EqualsCaseInsensitive(a.name, target_sha) ||
                EqualsCaseInsensitive(a.name, exact_sha)) {
                out_info.sha256_asset_name = a.name;
                out_info.sha256_download_url = a.download_url;
                break;
            }
        }

        // Pass 2 for SHA-256: fallback to any OpenScarlett-*-win64.zip.sha256 or *-win64.zip.sha256.
        if (out_info.sha256_asset_name.empty()) {
            for (const auto& a : assets) {
                if (StartsWithCaseInsensitive(a.name, "OpenScarlett-") &&
                    EndsWithCaseInsensitive(a.name, "-win64.zip.sha256")) {
                    out_info.sha256_asset_name = a.name;
                    out_info.sha256_download_url = a.download_url;
                    break;
                }
            }
        }
        if (out_info.sha256_asset_name.empty()) {
            for (const auto& a : assets) {
                if (EndsWithCaseInsensitive(a.name, "-win64.zip.sha256")) {
                    out_info.sha256_asset_name = a.name;
                    out_info.sha256_download_url = a.download_url;
                    break;
                }
            }
        }
    }

    void SkipWs() noexcept {
        while (pos_ < src_.size() && IsAsciiSpace(src_[pos_])) {
            ++pos_;
        }
    }

    bool Consume(char expected) noexcept {
        if (pos_ < src_.size() && src_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ConsumeLiteral(const char* lit) noexcept {
        const size_t len = std::strlen(lit);
        if (pos_ + len <= src_.size() && src_.compare(pos_, len, lit) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    Status ParseHex4(uint16_t& out_code_unit) {
        if (pos_ + 4 > src_.size()) {
            return Status::Error(StatusCode::ParseError, "Truncated \\uXXXX escape in JSON string");
        }
        uint16_t val = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            val = static_cast<uint16_t>(val << 4);
            if (c >= '0' && c <= '9') {
                val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - '0'));
            } else if (c >= 'a' && c <= 'f') {
                val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - 'a' + 10));
            } else if (c >= 'A' && c <= 'F') {
                val = static_cast<uint16_t>(val | static_cast<uint16_t>(c - 'A' + 10));
            } else {
                return Status::Error(StatusCode::ParseError, "Invalid hex digit in \\uXXXX escape");
            }
        }
        out_code_unit = val;
        return Status::Ok();
    }

    Status ParseString(std::string& out) {
        out.clear();
        if (!Consume('"')) {
            return Status::Error(StatusCode::ParseError, "Expected '\"' at start of JSON string");
        }
        while (pos_ < src_.size()) {
            const unsigned char ch = static_cast<unsigned char>(src_[pos_++]);
            if (ch == '"') {
                return Status::Ok();
            }
            if (ch < 0x20u) {
                return Status::Error(StatusCode::ParseError, "Unescaped control character in JSON string");
            }
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (pos_ >= src_.size()) {
                return Status::Error(StatusCode::ParseError, "Truncated escape sequence in JSON string");
            }
            const char esc = src_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint16_t u1 = 0;
                    Status st = ParseHex4(u1);
                    if (!st.ok()) {
                        return st;
                    }
                    uint32_t cp = u1;
                    if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                        if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint16_t u2 = 0;
                            st = ParseHex4(u2);
                            if (!st.ok()) {
                                return st;
                            }
                            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                                cp = 0x10000u +
                                     ((static_cast<uint32_t>(u1 - 0xD800u) << 10) |
                                      static_cast<uint32_t>(u2 - 0xDC00u));
                            } else {
                                cp = 0xFFFDu;
                            }
                        } else {
                            cp = 0xFFFDu;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Status::Error(StatusCode::ParseError, "Unknown escape sequence in JSON string");
            }
        }
        return Status::Error(StatusCode::ParseError, "Unterminated JSON string");
    }

    Status ParseStringOrNull(std::string& out) {
        SkipWs();
        if (ConsumeLiteral("null")) {
            out.clear();
            return Status::Ok();
        }
        return ParseString(out);
    }

    Status ParseAssetObject(AssetEntry& out_asset) {
        out_asset = AssetEntry{};
        if (!Consume('{')) {
            return Status::Error(StatusCode::ParseError, "Expected '{' for asset object");
        }
        SkipWs();
        if (Consume('}')) {
            return Status::Ok();
        }
        while (true) {
            SkipWs();
            std::string key;
            Status st = ParseString(key);
            if (!st.ok()) {
                return st;
            }
            SkipWs();
            if (!Consume(':')) {
                return Status::Error(StatusCode::ParseError, "Expected ':' in asset object");
            }
            SkipWs();
            if (key == "name") {
                st = ParseStringOrNull(out_asset.name);
            } else if (key == "browser_download_url") {
                st = ParseStringOrNull(out_asset.download_url);
            } else {
                st = SkipValue(3);
            }
            if (!st.ok()) {
                return st;
            }
            SkipWs();
            if (Consume(',')) {
                continue;
            }
            if (Consume('}')) {
                return Status::Ok();
            }
            return Status::Error(StatusCode::ParseError, "Expected ',' or '}' in asset object");
        }
    }

    Status ParseAssetsArray(std::vector<AssetEntry>& out_assets) {
        SkipWs();
        if (ConsumeLiteral("null")) {
            return Status::Ok();
        }
        if (!Consume('[')) {
            return Status::Error(StatusCode::ParseError, "Expected '[' for assets array");
        }
        SkipWs();
        if (Consume(']')) {
            return Status::Ok();
        }
        while (true) {
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == '{') {
                AssetEntry entry;
                Status st = ParseAssetObject(entry);
                if (!st.ok()) {
                    return st;
                }
                if (!entry.name.empty()) {
                    out_assets.push_back(std::move(entry));
                }
            } else {
                Status st = SkipValue(2);
                if (!st.ok()) {
                    return st;
                }
            }
            SkipWs();
            if (Consume(',')) {
                continue;
            }
            if (Consume(']')) {
                return Status::Ok();
            }
            return Status::Error(StatusCode::ParseError, "Expected ',' or ']' in assets array");
        }
    }

    Status SkipNumber() {
        const size_t start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return Status::Error(StatusCode::ParseError, "Truncated JSON number");
        }
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && IsAsciiDigit(src_[pos_])) {
                ++pos_;
            }
        } else {
            return Status::Error(StatusCode::ParseError, "Invalid JSON number");
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || !IsAsciiDigit(src_[pos_])) {
                return Status::Error(StatusCode::ParseError, "Invalid fractional JSON number");
            }
            while (pos_ < src_.size() && IsAsciiDigit(src_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || !IsAsciiDigit(src_[pos_])) {
                return Status::Error(StatusCode::ParseError, "Invalid exponent in JSON number");
            }
            while (pos_ < src_.size() && IsAsciiDigit(src_[pos_])) {
                ++pos_;
            }
        }
        if (pos_ == start) {
            return Status::Error(StatusCode::ParseError, "Empty JSON number");
        }
        return Status::Ok();
    }

    Status SkipValue(int depth) {
        if (depth > kMaxJsonDepth) {
            return Status::Error(StatusCode::ParseError, "JSON nesting depth exceeded limit");
        }
        SkipWs();
        if (pos_ >= src_.size()) {
            return Status::Error(StatusCode::ParseError, "Unexpected end of JSON input");
        }
        const char c = src_[pos_];
        if (c == '"') {
            std::string ignored;
            return ParseString(ignored);
        }
        if (c == '{') {
            ++pos_;
            SkipWs();
            if (Consume('}')) {
                return Status::Ok();
            }
            while (true) {
                SkipWs();
                std::string ignored_key;
                Status st = ParseString(ignored_key);
                if (!st.ok()) {
                    return st;
                }
                SkipWs();
                if (!Consume(':')) {
                    return Status::Error(StatusCode::ParseError, "Expected ':' in nested JSON object");
                }
                st = SkipValue(depth + 1);
                if (!st.ok()) {
                    return st;
                }
                SkipWs();
                if (Consume(',')) {
                    continue;
                }
                if (Consume('}')) {
                    return Status::Ok();
                }
                return Status::Error(StatusCode::ParseError, "Expected ',' or '}' in nested JSON object");
            }
        }
        if (c == '[') {
            ++pos_;
            SkipWs();
            if (Consume(']')) {
                return Status::Ok();
            }
            while (true) {
                Status st = SkipValue(depth + 1);
                if (!st.ok()) {
                    return st;
                }
                SkipWs();
                if (Consume(',')) {
                    continue;
                }
                if (Consume(']')) {
                    return Status::Ok();
                }
                return Status::Error(StatusCode::ParseError, "Expected ',' or ']' in nested JSON array");
            }
        }
        if (c == 't') {
            return ConsumeLiteral("true")
                       ? Status::Ok()
                       : Status::Error(StatusCode::ParseError, "Invalid JSON literal");
        }
        if (c == 'f') {
            return ConsumeLiteral("false")
                       ? Status::Ok()
                       : Status::Error(StatusCode::ParseError, "Invalid JSON literal");
        }
        if (c == 'n') {
            return ConsumeLiteral("null")
                       ? Status::Ok()
                       : Status::Error(StatusCode::ParseError, "Invalid JSON literal");
        }
        if (c == '-' || IsAsciiDigit(c)) {
            return SkipNumber();
        }
        return Status::Error(StatusCode::ParseError, "Unexpected token in JSON");
    }

    const std::string& src_;
    size_t pos_ = 0;
};

bool IsSafeRepoToken(const std::string& s) {
    if (s.empty() || s.size() > 128) {
        return false;
    }
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return s != "." && s != "..";
}

bool IsHttpsUrl(const std::string& url) {
    return StartsWithCaseInsensitive(url, "https://") && url.size() > 8;
}

UniqueWinHttp OpenWinHttpSession(DWORD resolve_ms, DWORD connect_ms, DWORD send_ms, DWORD recv_ms) {
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    HINTERNET h = WinHttpOpen(
        kUserAgentW,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (h == nullptr && GetLastError() == ERROR_INVALID_PARAMETER) {
        h = WinHttpOpen(
            kUserAgentW,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }
#else
    HINTERNET h = WinHttpOpen(
        kUserAgentW,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
#endif
    UniqueWinHttp session(h);
    if (session.Valid()) {
        WinHttpSetTimeouts(
            session.Get(),
            static_cast<int>(resolve_ms),
            static_cast<int>(connect_ms),
            static_cast<int>(send_ms),
            static_cast<int>(recv_ms));
    }
    return session;
}

struct ParsedHttpsUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path_and_query;
};

Status CrackHttpsUrl(const std::string& url, ParsedHttpsUrl& out) {
    if (!IsHttpsUrl(url)) {
        return Status::Error(StatusCode::InvalidArgument, "Only HTTPS URLs are permitted");
    }
    const std::wstring url_w = Utf8ToWide(url);
    if (url_w.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid UTF-8 URL");
    }

    URL_COMPONENTS comps{};
    comps.dwStructSize = sizeof(comps);
    comps.dwHostNameLength = static_cast<DWORD>(-1);
    comps.dwUrlPathLength = static_cast<DWORD>(-1);
    comps.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url_w.c_str(), static_cast<DWORD>(url_w.size()), 0, &comps)) {
        return Status::Error(
            StatusCode::InvalidArgument,
            FormatWin32Error("WinHttpCrackUrl failed", GetLastError()));
    }
    if (comps.nScheme != INTERNET_SCHEME_HTTPS || comps.lpszHostName == nullptr ||
        comps.dwHostNameLength == 0) {
        return Status::Error(StatusCode::InvalidArgument, "URL must use https:// with a valid host");
    }

    out.host.assign(comps.lpszHostName, comps.dwHostNameLength);
    out.port = (comps.nPort != 0) ? comps.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    out.path_and_query.clear();
    if (comps.lpszUrlPath != nullptr && comps.dwUrlPathLength > 0) {
        out.path_and_query.append(comps.lpszUrlPath, comps.dwUrlPathLength);
    } else {
        out.path_and_query = L"/";
    }
    if (comps.lpszExtraInfo != nullptr && comps.dwExtraInfoLength > 0) {
        out.path_and_query.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);
    }
    return Status::Ok();
}

void ConfigureSecureRequestOptions(HINTERNET request) {
#ifdef WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(
        request,
        WINHTTP_OPTION_REDIRECT_POLICY,
        &redirect_policy,
        sizeof(redirect_policy));
#endif
}

Status DownloadHttpsFile(
    const std::string& url,
    const std::filesystem::path& dest_path,
    uint64_t max_bytes,
    const std::string& stage_label,
    const std::function<void(const UpdateDownloadProgress&)>& on_progress,
    const std::atomic<bool>* cancel_flag) {
    if (cancel_flag != nullptr && cancel_flag->load()) {
        return Status::Error(StatusCode::IoError, "Download cancelled by user");
    }

    ParsedHttpsUrl parsed;
    Status st = CrackHttpsUrl(url, parsed);
    if (!st.ok()) {
        return st;
    }

    UniqueWinHttp session = OpenWinHttpSession(5000, 10000, 15000, 30000);
    if (!session.Valid()) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpOpen failed", GetLastError()));
    }

    UniqueWinHttp connect(WinHttpConnect(session.Get(), parsed.host.c_str(), parsed.port, 0));
    if (!connect.Valid()) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpConnect failed", GetLastError()));
    }

    UniqueWinHttp request(WinHttpOpenRequest(
        connect.Get(),
        L"GET",
        parsed.path_and_query.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.Valid()) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpOpenRequest failed", GetLastError()));
    }

    ConfigureSecureRequestOptions(request.Get());

    const wchar_t* headers = L"User-Agent: OpenScarlett-Launcher/0.4.0\r\nAccept: application/octet-stream, */*\r\n";
    if (!WinHttpSendRequest(
            request.Get(),
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpSendRequest failed", GetLastError()));
    }

    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpReceiveResponse failed", GetLastError()));
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpQueryHeaders(status) failed", GetLastError()));
    }
    if (status_code != 200u) {
        std::ostringstream oss;
        oss << "HTTPS download failed with HTTP " << status_code;
        return Status::Error(StatusCode::IoError, oss.str());
    }

    uint64_t total_bytes = 0;
    wchar_t content_len_buf[64] = {};
    DWORD content_len_size = sizeof(content_len_buf);
    if (WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX,
            content_len_buf,
            &content_len_size,
            WINHTTP_NO_HEADER_INDEX)) {
        try {
            total_bytes = std::stoull(std::wstring(content_len_buf));
        } catch (...) {
            total_bytes = 0;
        }
    }
    if (total_bytes > max_bytes) {
        return Status::Error(StatusCode::IoError, "Download Content-Length exceeds safety limit");
    }

    UniqueHandle file(CreateFileW(
        dest_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.Valid()) {
        return Status::Error(StatusCode::IoError, FormatWin32Error("Failed to create download destination file", GetLastError()));
    }

    std::vector<uint8_t> buffer(kFileHashChunkBytes);
    uint64_t bytes_downloaded = 0;

    if (on_progress) {
        UpdateDownloadProgress p;
        p.stage = stage_label;
        p.bytes_downloaded = 0;
        p.total_bytes = total_bytes;
        p.percent = 0;
        on_progress(p);
    }

    auto cleanup_partial = [&]() {
        file.Reset();
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
    };

    while (true) {
        if (cancel_flag != nullptr && cancel_flag->load()) {
            cleanup_partial();
            return Status::Error(StatusCode::IoError, "Download cancelled by user");
        }

        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
            const DWORD err = GetLastError();
            cleanup_partial();
            return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpQueryDataAvailable failed", err));
        }
        if (avail == 0) {
            break;
        }

        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(avail, buffer.size()));
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request.Get(), buffer.data(), to_read, &bytes_read)) {
            const DWORD err = GetLastError();
            cleanup_partial();
            return Status::Error(StatusCode::IoError, FormatWin32Error("WinHttpReadData failed", err));
        }
        if (bytes_read == 0) {
            break;
        }

        bytes_downloaded += bytes_read;
        if (bytes_downloaded > max_bytes) {
            cleanup_partial();
            return Status::Error(StatusCode::IoError, "Downloaded file exceeded safety size limit");
        }

        DWORD written_total = 0;
        while (written_total < bytes_read) {
            DWORD written = 0;
            if (!WriteFile(
                    file.Get(),
                    buffer.data() + written_total,
                    bytes_read - written_total,
                    &written,
                    nullptr)) {
                const DWORD err = GetLastError();
                cleanup_partial();
                return Status::Error(StatusCode::IoError, FormatWin32Error("WriteFile failed during download", err));
            }
            written_total += written;
        }

        if (on_progress) {
            UpdateDownloadProgress p;
            p.stage = stage_label;
            p.bytes_downloaded = bytes_downloaded;
            p.total_bytes = total_bytes;
            if (total_bytes > 0) {
                const uint64_t pct = (bytes_downloaded * 100ull) / total_bytes;
                p.percent = static_cast<int>(std::min<uint64_t>(pct, 100ull));
            } else {
                p.percent = 0;
            }
            on_progress(p);
        }
    }

    file.Reset();
    return Status::Ok();
}

void CleanDirectoryBestEffort(const std::filesystem::path& dir) noexcept {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        return;
    }
    std::filesystem::remove_all(dir, ec);
    if (!ec) {
        return;
    }

    // Retry after stripping read-only / system / hidden attributes on Windows.
    std::error_code walk_ec;
    for (std::filesystem::recursive_directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied, walk_ec),
         end;
         it != end && !walk_ec;
         it.increment(walk_ec)) {
        SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    ec.clear();
    std::filesystem::remove_all(dir, ec);
}

std::wstring StripTrailingSeparators(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        // Preserve Windows drive roots such as "C:\" or "E:\".
        if (path.size() == 3 && path[1] == L':') {
            break;
        }
        path.pop_back();
    }
    return path;
}

bool IsPathInsideDirectory(const std::filesystem::path& candidate,
                           const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::path norm_cand = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return false;
    }
    std::filesystem::path norm_dir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) {
        return false;
    }

    // Collect non-empty components so trailing slashes never break prefix matching.
    std::vector<std::wstring> dir_parts;
    for (const auto& part : norm_dir) {
        const std::wstring w = part.wstring();
        if (!w.empty()) {
            dir_parts.push_back(w);
        }
    }
    std::vector<std::wstring> cand_parts;
    for (const auto& part : norm_cand) {
        const std::wstring w = part.wstring();
        if (!w.empty()) {
            cand_parts.push_back(w);
        }
    }
    if (dir_parts.empty() || cand_parts.size() < dir_parts.size()) {
        return false;
    }
    for (size_t i = 0; i < dir_parts.size(); ++i) {
        if (!EqualsCaseInsensitive(WideToUtf8(dir_parts[i]), WideToUtf8(cand_parts[i]))) {
            return false;
        }
    }
    return true;
}

std::string SanitizeAssetFilename(const std::string& raw_name, const std::string& fallback) {
    std::filesystem::path p(raw_name);
    std::string base = p.filename().string();
    if (base.empty() || base == "." || base == "..") {
        base = fallback;
    }
    for (char& c : base) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20u) {
            c = '_';
        }
    }
    return base;
}

std::wstring EscapePowerShellSingleQuoted(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        if (c == L'\'') {
            out.push_back(L'\'');
            out.push_back(L'\'');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string EscapeBatchLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '%') {
            out.push_back('%');
            out.push_back('%');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring GetWindowsSystemExecutable(const wchar_t* relative_subpath) {
    wchar_t sys_dir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::wstring full(sys_dir, len);
        if (!full.empty() && full.back() != L'\\') {
            full.push_back(L'\\');
        }
        full.append(relative_subpath);
        return full;
    }
    return std::wstring(relative_subpath);
}

std::wstring QuoteWin32Arg(const std::wstring& arg) {
    std::wstring out;
    out.reserve(arg.size() + 4);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes > 0) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out.push_back(c);
        }
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

std::wstring Base64EncodeUtf16Le(const std::wstring& text) {
    static constexpr wchar_t kTable[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * 2u);
    for (wchar_t ch : text) {
        const uint16_t u = static_cast<uint16_t>(ch);
        bytes.push_back(static_cast<uint8_t>(u & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((u >> 8u) & 0xFFu));
    }

    std::wstring out;
    out.reserve(((bytes.size() + 2u) / 3u) * 4u);
    size_t i = 0;
    while (i + 3u <= bytes.size()) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16u) |
                                (static_cast<uint32_t>(bytes[i + 1u]) << 8u) |
                                static_cast<uint32_t>(bytes[i + 2u]);
        out.push_back(kTable[(triple >> 18u) & 0x3Fu]);
        out.push_back(kTable[(triple >> 12u) & 0x3Fu]);
        out.push_back(kTable[(triple >> 6u) & 0x3Fu]);
        out.push_back(kTable[triple & 0x3Fu]);
        i += 3u;
    }
    const size_t rem = bytes.size() - i;
    if (rem == 1u) {
        const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16u;
        out.push_back(kTable[(triple >> 18u) & 0x3Fu]);
        out.push_back(kTable[(triple >> 12u) & 0x3Fu]);
        out.push_back(L'=');
        out.push_back(L'=');
    } else if (rem == 2u) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16u) |
                                (static_cast<uint32_t>(bytes[i + 1u]) << 8u);
        out.push_back(kTable[(triple >> 18u) & 0x3Fu]);
        out.push_back(kTable[(triple >> 12u) & 0x3Fu]);
        out.push_back(kTable[(triple >> 6u) & 0x3Fu]);
        out.push_back(L'=');
    }
    return out;
}

uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8u));
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint32_t ComputeCrc32Ieee(const uint8_t* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool DirectoryHasAnyEntries(const std::filesystem::path& dir) noexcept {
    std::error_code ec;
    const std::filesystem::directory_iterator it(dir, ec);
    return !ec && (it != std::filesystem::directory_iterator());
}

// Pure C++ PKZIP STORE (compression method 0) fallback extractor.
// Provides zero-subprocess extraction with strict Zip-Slip path validation and CRC-32 checks.
Status ExtractStoredZipNative(const std::filesystem::path& zip_path,
                              const std::filesystem::path& extract_dir) {
    std::ifstream ifs(zip_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return Status::Error(StatusCode::IoError, "Unable to open zip archive for native extraction");
    }
    const std::streamoff end_pos = ifs.tellg();
    if (end_pos < 22) {
        return Status::Error(StatusCode::ParseError, "Zip archive is smaller than minimum EOCD size");
    }
    const size_t file_size = static_cast<size_t>(end_pos);
    constexpr size_t kMaxNativeFallbackZipBytes = 256u * 1024u * 1024u;
    if (file_size > kMaxNativeFallbackZipBytes) {
        return Status::Error(StatusCode::IoError, "Zip archive exceeds native STORE fallback memory cap");
    }

    std::vector<uint8_t> buf(file_size);
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size))) {
        return Status::Error(StatusCode::IoError, "Failed to read zip archive bytes");
    }

    // Locate End of Central Directory (0x06054B50) scanning backwards (up to 64 KiB comment).
    const size_t max_back = std::min<size_t>(file_size, 65535u + 22u);
    const size_t min_eocd = file_size - max_back;
    size_t eocd_pos = std::string::npos;
    for (size_t pos = file_size - 22u;; --pos) {
        if (ReadLe32(buf.data() + pos) == 0x06054B50u) {
            const uint16_t comment_len = ReadLe16(buf.data() + pos + 20u);
            if (pos + 22u + comment_len == file_size) {
                eocd_pos = pos;
                break;
            }
        }
        if (pos == min_eocd) {
            break;
        }
    }
    if (eocd_pos == std::string::npos) {
        return Status::Error(StatusCode::ParseError, "Zip EOCD record not found");
    }

    const uint16_t total_entries = ReadLe16(buf.data() + eocd_pos + 10u);
    const uint32_t cd_size = ReadLe32(buf.data() + eocd_pos + 12u);
    const uint32_t cd_offset = ReadLe32(buf.data() + eocd_pos + 16u);
    if (total_entries == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        return Status::Error(StatusCode::ParseError, "Zip64 archives are not supported by native STORE fallback");
    }
    if (static_cast<uint64_t>(cd_offset) + static_cast<uint64_t>(cd_size) > file_size) {
        return Status::Error(StatusCode::ParseError, "Zip central directory extends past end of file");
    }

    size_t cd_pos = cd_offset;
    const size_t cd_end = static_cast<size_t>(cd_offset) + static_cast<size_t>(cd_size);
    size_t extracted_count = 0;

    for (uint16_t i = 0; i < total_entries; ++i) {
        if (cd_pos + 46u > cd_end) {
            return Status::Error(StatusCode::ParseError, "Truncated zip central directory entry");
        }
        const uint8_t* cd = buf.data() + cd_pos;
        if (ReadLe32(cd) != 0x02014B50u) {
            return Status::Error(StatusCode::ParseError, "Invalid zip central directory signature");
        }

        const uint16_t flags = ReadLe16(cd + 8u);
        const uint16_t method = ReadLe16(cd + 10u);
        const uint32_t expected_crc = ReadLe32(cd + 16u);
        const uint32_t comp_size = ReadLe32(cd + 20u);
        const uint32_t uncomp_size = ReadLe32(cd + 24u);
        const uint16_t name_len = ReadLe16(cd + 28u);
        const uint16_t extra_len = ReadLe16(cd + 30u);
        const uint16_t comment_len = ReadLe16(cd + 32u);
        const uint32_t local_hdr_off = ReadLe32(cd + 42u);

        const size_t entry_total =
            46u + static_cast<size_t>(name_len) + static_cast<size_t>(extra_len) + static_cast<size_t>(comment_len);
        if (cd_pos + entry_total > cd_end) {
            return Status::Error(StatusCode::ParseError, "Zip central directory entry overflows table");
        }

        if ((flags & 0x0001u) != 0u) {
            return Status::Error(StatusCode::ParseError, "Encrypted zip entries are not supported");
        }
        if (method != 0u) {
            std::ostringstream oss;
            oss << "Native fallback supports STORE (method 0) only; entry uses compression method " << method;
            return Status::Error(StatusCode::IoError, oss.str());
        }
        if (comp_size != uncomp_size) {
            return Status::Error(StatusCode::ParseError, "STORE zip entry compressed/uncompressed size mismatch");
        }

        const std::string raw_name(reinterpret_cast<const char*>(cd + 46u), name_len);
        cd_pos += entry_total;

        if (raw_name.empty() || raw_name.find('\0') != std::string::npos ||
            raw_name.find(':') != std::string::npos || raw_name[0] == '/' || raw_name[0] == '\\') {
            return Status::Error(StatusCode::ParseError, "Rejected unsafe zip entry path");
        }

        const bool is_dir = (raw_name.back() == '/' || raw_name.back() == '\\');
        std::filesystem::path rel_path;
        size_t seg_start = 0;
        while (seg_start < raw_name.size()) {
            size_t seg_end = raw_name.find_first_of("/\\", seg_start);
            if (seg_end == std::string::npos) {
                seg_end = raw_name.size();
            }
            const std::string seg = raw_name.substr(seg_start, seg_end - seg_start);
            seg_start = seg_end + 1u;
            if (seg.empty() || seg == ".") {
                continue;
            }
            if (seg == "..") {
                return Status::Error(StatusCode::ParseError, "Rejected zip-slip '..' path traversal entry");
            }
            rel_path /= Utf8ToWide(seg);
        }
        if (rel_path.empty()) {
            continue;
        }

        const std::filesystem::path out_path = extract_dir / rel_path;
        if (!IsPathInsideDirectory(out_path, extract_dir)) {
            return Status::Error(StatusCode::ParseError, "Rejected zip entry escaping extraction directory");
        }

        std::error_code ec;
        if (is_dir) {
            std::filesystem::create_directories(out_path, ec);
            if (ec) {
                return Status::Error(StatusCode::IoError, "Failed to create directory from zip: " + ec.message());
            }
            ++extracted_count;
            continue;
        }

        if (static_cast<uint64_t>(local_hdr_off) + 30u > file_size) {
            return Status::Error(StatusCode::ParseError, "Zip local file header offset out of range");
        }
        const uint8_t* lh = buf.data() + local_hdr_off;
        if (ReadLe32(lh) != 0x04034B50u) {
            return Status::Error(StatusCode::ParseError, "Invalid zip local file header signature");
        }
        const uint16_t lh_name_len = ReadLe16(lh + 26u);
        const uint16_t lh_extra_len = ReadLe16(lh + 28u);
        const uint64_t data_off = static_cast<uint64_t>(local_hdr_off) + 30u +
                                  static_cast<uint64_t>(lh_name_len) +
                                  static_cast<uint64_t>(lh_extra_len);
        if (data_off + static_cast<uint64_t>(comp_size) > file_size) {
            return Status::Error(StatusCode::ParseError, "Zip entry payload extends past end of file");
        }

        const uint8_t* payload = buf.data() + static_cast<size_t>(data_off);
        const uint32_t actual_crc = ComputeCrc32Ieee(payload, comp_size);
        if (actual_crc != expected_crc) {
            return Status::Error(StatusCode::ParseError, "Zip entry CRC-32 mismatch during native extraction");
        }

        if (out_path.has_parent_path()) {
            std::filesystem::create_directories(out_path.parent_path(), ec);
            if (ec) {
                return Status::Error(StatusCode::IoError, "Failed to create parent directory: " + ec.message());
            }
        }
        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return Status::Error(StatusCode::IoError, "Failed to create extracted file");
        }
        if (comp_size > 0) {
            ofs.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(comp_size));
        }
        ofs.flush();
        if (!ofs.good()) {
            return Status::Error(StatusCode::IoError, "Failed to write extracted file bytes");
        }
        ++extracted_count;
    }

    if (extracted_count == 0) {
        return Status::Error(StatusCode::ParseError, "Zip archive contained no extractable entries");
    }
    return Status::Ok();
}

Status ExtractZipArchive(const std::filesystem::path& zip_path,
                         const std::filesystem::path& extract_dir) {
    std::error_code norm_ec;
    std::filesystem::path abs_zip = std::filesystem::absolute(zip_path, norm_ec);
    if (norm_ec) {
        abs_zip = zip_path;
    }
    abs_zip = std::filesystem::path(StripTrailingSeparators(abs_zip.lexically_normal().wstring()));

    norm_ec.clear();
    std::filesystem::path abs_dest = std::filesystem::absolute(extract_dir, norm_ec);
    if (norm_ec) {
        abs_dest = extract_dir;
    }
    abs_dest = std::filesystem::path(StripTrailingSeparators(abs_dest.lexically_normal().wstring()));

    auto prepare_clean_dest = [&]() -> Status {
        CleanDirectoryBestEffort(abs_dest);
        std::error_code dir_ec;
        std::filesystem::create_directories(abs_dest, dir_ec);
        if (dir_ec) {
            return Status::Error(
                StatusCode::IoError,
                "Failed to prepare clean extraction directory: " + dir_ec.message());
        }
        return Status::Ok();
    };

    auto run_hidden = [](const std::wstring& exe, const std::wstring& cmdline, DWORD& out_exit) -> Status {
        std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
        cmd_buf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(
                exe.c_str(),
                cmd_buf.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi)) {
            return Status::Error(
                StatusCode::IoError,
                FormatWin32Error("Failed to launch zip extractor process", GetLastError()));
        }

        UniqueHandle proc(pi.hProcess);
        UniqueHandle thread(pi.hThread);

        const DWORD wait_rc = WaitForSingleObject(proc.Get(), kExpandArchiveTimeoutMs);
        if (wait_rc != WAIT_OBJECT_0) {
            TerminateProcess(proc.Get(), 1);
            return Status::Error(StatusCode::IoError, "Zip extraction timed out or failed to wait");
        }

        out_exit = 1;
        if (!GetExitCodeProcess(proc.Get(), &out_exit)) {
            return Status::Error(StatusCode::IoError, "Failed to query zip extractor exit code");
        }
        return Status::Ok();
    };

    std::ostringstream diag;

    // Fast path 1: Windows 10 1803+ / Windows 11 / Server 2019+ built-in System32\tar.exe
    // (handles ZIP STORE & DEFLATE natively without PowerShell or PSModulePath issues).
    const std::wstring tar_exe = GetWindowsSystemExecutable(L"tar.exe");
    std::error_code ec;
    if (std::filesystem::exists(tar_exe, ec) && !ec) {
        Status prep_st = prepare_clean_dest();
        if (!prep_st.ok()) {
            return prep_st;
        }
        const std::wstring tar_cmd =
            QuoteWin32Arg(tar_exe) + L" -xf " + QuoteWin32Arg(abs_zip.wstring()) +
            L" -C " + QuoteWin32Arg(abs_dest.wstring());
        DWORD tar_exit = 1;
        const Status tar_st = run_hidden(tar_exe, tar_cmd, tar_exit);
        if (tar_st.ok() && tar_exit == 0 && DirectoryHasAnyEntries(abs_dest)) {
            return Status::Ok();
        }
        diag << "tar.exe(";
        if (!tar_st.ok()) {
            diag << tar_st.message;
        } else if (tar_exit != 0) {
            diag << "exit=" << tar_exit;
        } else {
            diag << "empty output";
        }
        diag << ")";
    } else {
        diag << "tar.exe(not found)";
    }

    // Fallback 2: .NET System.IO.Compression.ZipFile via WindowsPowerShell with -EncodedCommand
    // and direct Assembly::Load (zero PowerShell module imports, 100% immune to pwsh7 PSModulePath
    // pollution AND path special characters like spaces, parentheses, $, &, or quotes).
    const std::wstring ps_exe =
        GetWindowsSystemExecutable(L"WindowsPowerShell\\v1.0\\powershell.exe");
    ec.clear();
    if (std::filesystem::exists(ps_exe, ec) && !ec) {
        Status prep_st = prepare_clean_dest();
        if (!prep_st.ok()) {
            return prep_st;
        }

        const std::wstring escaped_zip = EscapePowerShellSingleQuoted(abs_zip.wstring());
        const std::wstring escaped_dest = EscapePowerShellSingleQuoted(abs_dest.wstring());
        const std::wstring ps_script =
            L"$ErrorActionPreference = 'Stop'; "
            L"[void][System.Reflection.Assembly]::Load("
            L"'System.IO.Compression.FileSystem, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089'); "
            L"[System.IO.Compression.ZipFile]::ExtractToDirectory('" + escaped_zip +
            L"', '" + escaped_dest + L"')";
        const std::wstring ps_encoded = Base64EncodeUtf16Le(ps_script);

        const std::wstring ps_cmd =
            QuoteWin32Arg(ps_exe) +
            L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + ps_encoded;

        DWORD ps_exit = 1;
        const Status ps_st = run_hidden(ps_exe, ps_cmd, ps_exit);
        if (ps_st.ok() && ps_exit == 0 && DirectoryHasAnyEntries(abs_dest)) {
            return Status::Ok();
        }
        diag << "; powershell-zipfile(";
        if (!ps_st.ok()) {
            diag << ps_st.message;
        } else if (ps_exit != 0) {
            diag << "exit=" << ps_exit;
        } else {
            diag << "empty output";
        }
        diag << ")";
    } else {
        diag << "; powershell-zipfile(not found)";
    }

    // Fallback 3: Built-in pure C++ PKZIP STORE (method 0) extractor
    // (guarantees zero-subprocess extraction even on locked-down environments).
    {
        Status prep_st = prepare_clean_dest();
        if (!prep_st.ok()) {
            return prep_st;
        }
        const Status native_st = ExtractStoredZipNative(abs_zip, abs_dest);
        if (native_st.ok() && DirectoryHasAnyEntries(abs_dest)) {
            return Status::Ok();
        }
        diag << "; native-store(" << native_st.message << ")";
    }

    return Status::Error(StatusCode::IoError, "All zip extractors failed: " + diag.str());
}

std::filesystem::path ResolveExtractedPayloadDir(const std::filesystem::path& extract_dir) {
    std::error_code ec;
    size_t dir_count = 0;
    size_t file_count = 0;
    std::filesystem::path single_subdir;

    for (const auto& entry : std::filesystem::directory_iterator(extract_dir, ec)) {
        if (ec) {
            return extract_dir;
        }
        if (entry.is_directory(ec)) {
            ++dir_count;
            single_subdir = entry.path();
        } else {
            ++file_count;
        }
    }

    if (dir_count == 1 && file_count == 0 && !single_subdir.empty()) {
        return single_subdir;
    }
    return extract_dir;
}

Status WriteRestartApplyScript(const std::filesystem::path& script_path,
                               const std::filesystem::path& app_dir,
                               const std::filesystem::path& staging_dir,
                               const std::filesystem::path& payload_dir,
                               const std::string& tag_name) {
    const std::string app_str = EscapeBatchLiteral(
        WideToUtf8(StripTrailingSeparators(app_dir.wstring())));
    const std::string staging_str = EscapeBatchLiteral(
        WideToUtf8(StripTrailingSeparators(staging_dir.wstring())));
    const std::string payload_str = EscapeBatchLiteral(
        WideToUtf8(StripTrailingSeparators(payload_dir.wstring())));
    const std::string tag_str = EscapeBatchLiteral(tag_name);

    std::ostringstream script;
    script << "@echo off\r\n";
    script << "setlocal EnableExtensions\r\n";
    script << "rem OpenScarlett transparent update helper (target: " << tag_str << ")\r\n";
    script << "rem Preserves saves\\, S\\, T\\, G_overlay\\, openscarlett_config.json, launcher_config.json, user_config.json\r\n";
    script << "\r\n";
    script << "set \"PARENT_PID=%~1\"\r\n";
    script << "set \"APP_DIR=" << app_str << "\"\r\n";
    script << "set \"STAGING_DIR=" << staging_str << "\"\r\n";
    script << "set \"SRC_DIR=" << payload_str << "\"\r\n";
    script << "\r\n";
    script << "rem 1. Wait for parent OpenScarlettLauncher.exe PID to exit cleanly (bounded timeout)\r\n";
    script << "if \"%PARENT_PID%\"==\"\" goto :wait_done\r\n";
    script << "set /a WAIT_COUNT=0\r\n";
    script << ":wait_loop\r\n";
    script << "tasklist /FI \"PID eq %PARENT_PID%\" /FO CSV /NH 2>nul | findstr /C:\",\\\"%PARENT_PID%\\\",\" >nul\r\n";
    script << "if errorlevel 1 goto :wait_done\r\n";
    script << "set /a WAIT_COUNT+=1\r\n";
    script << "if %WAIT_COUNT% GEQ 60 goto :wait_done\r\n";
    script << "ping -n 2 127.0.0.1 >nul\r\n";
    script << "goto :wait_loop\r\n";
    script << ":wait_done\r\n";
    script << "\r\n";
    script << "rem 2. Copy verified update payload into APP_DIR while preserving user saves and config\r\n";
    script << "if not exist \"%SRC_DIR%\" goto :cleanup\r\n";
    script << "robocopy \"%SRC_DIR%\" \"%APP_DIR%\" /E /R:5 /W:1 /NP /NFL /NDL /NJH /NJS "
           << "/XD \"saves\" \"S\" \"T\" \"G_overlay\" \"_openscarlett_update_staging\" "
           << "/XF \"openscarlett_config.json\" \"launcher_config.json\" \"user_config.json\" >nul\r\n";
    script << "\r\n";
    script << "rem 3. Relaunch OpenScarlettLauncher.exe\r\n";
    script << "if exist \"%APP_DIR%\\OpenScarlettLauncher.exe\" (\r\n";
    script << "    start \"\" \"%APP_DIR%\\OpenScarlettLauncher.exe\"\r\n";
    script << ")\r\n";
    script << "\r\n";
    script << ":cleanup\r\n";
    script << "rem 4. Remove staging directory after exiting this script\r\n";
    script << "cd /d \"%APP_DIR%\"\r\n";
    script << "start \"\" /b cmd /c \"ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%STAGING_DIR%\"\"\r\n";
    script << "endlocal\r\n";
    script << "exit /b 0\r\n";

    std::ofstream ofs(script_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::Error(StatusCode::IoError, "Failed to create restart helper script");
    }
    const std::string content = script.str();
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed to write restart helper script");
    }
    return Status::Ok();
}

Status ReadSmallTextFile(const std::filesystem::path& path, size_t max_bytes, std::string& out_text) {
    out_text.clear();
    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.Valid()) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return Status::Error(StatusCode::NotFound, "File not found: " + WideToUtf8(path.wstring()));
        }
        return Status::Error(StatusCode::IoError, FormatWin32Error("Failed to open file", err));
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.Get(), &file_size) || file_size.QuadPart < 0) {
        return Status::Error(StatusCode::IoError, "Failed to query file size");
    }
    if (static_cast<uint64_t>(file_size.QuadPart) > max_bytes) {
        return Status::Error(StatusCode::ParseError, "Checksum file exceeds maximum allowed size");
    }

    const size_t size = static_cast<size_t>(file_size.QuadPart);
    out_text.resize(size);
    size_t total_read = 0;
    while (total_read < size) {
        DWORD chunk = 0;
        const DWORD to_read = static_cast<DWORD>(size - total_read);
        if (!ReadFile(file.Get(), out_text.data() + total_read, to_read, &chunk, nullptr)) {
            return Status::Error(StatusCode::IoError, FormatWin32Error("Failed to read file", GetLastError()));
        }
        if (chunk == 0) {
            break;
        }
        total_read += chunk;
    }
    out_text.resize(total_read);
    return Status::Ok();
}

}  // namespace

SemVer ParseSemVer(const std::string& version_str) {
    SemVer result;
    std::string s = TrimAscii(version_str);
    if (s.empty()) {
        return result;
    }

    if (s[0] == 'v' || s[0] == 'V') {
        s.erase(0, 1);
    }
    if (s.empty()) {
        return result;
    }

    // Ignore +build metadata if present.
    const size_t plus_pos = s.find('+');
    if (plus_pos != std::string::npos) {
        if (plus_pos == 0) {
            return result;
        }
        s = s.substr(0, plus_pos);
    }

    // Split optional -prerelease.
    std::string core = s;
    std::string prerelease;
    const size_t dash_pos = s.find('-');
    if (dash_pos != std::string::npos) {
        core = s.substr(0, dash_pos);
        prerelease = s.substr(dash_pos + 1);
        if (prerelease.empty()) {
            return result;
        }
        const auto pre_parts = SplitByDot(prerelease);
        for (const auto& part : pre_parts) {
            if (part.empty()) {
                return result;
            }
            for (char c : part) {
                const bool ok = (c >= '0' && c <= '9') ||
                                (c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                c == '-';
                if (!ok) {
                    return result;
                }
            }
        }
    }

    // Parse major.minor[.patch].
    const auto core_parts = SplitByDot(core);
    if (core_parts.size() != 2 && core_parts.size() != 3) {
        return result;
    }

    int maj = 0;
    int min = 0;
    int pat = 0;
    if (!ParseNonNegativeInt(core_parts[0], maj) || !ParseNonNegativeInt(core_parts[1], min)) {
        return result;
    }
    if (core_parts.size() == 3 && !ParseNonNegativeInt(core_parts[2], pat)) {
        return result;
    }

    result.major = maj;
    result.minor = min;
    result.patch = pat;
    result.prerelease = std::move(prerelease);
    result.valid = true;
    return result;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (!a.valid || !b.valid) {
        if (!a.valid && !b.valid) {
            return 0;
        }
        return a.valid ? 1 : -1;
    }

    if (a.major != b.major) {
        return (a.major < b.major) ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return (a.minor < b.minor) ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return (a.patch < b.patch) ? -1 : 1;
    }

    const bool a_stable = a.prerelease.empty();
    const bool b_stable = b.prerelease.empty();
    if (a_stable && b_stable) {
        return 0;
    }
    if (a_stable != b_stable) {
        return a_stable ? 1 : -1;
    }

    const auto a_ids = SplitByDot(a.prerelease);
    const auto b_ids = SplitByDot(b.prerelease);
    const size_t shared = std::min(a_ids.size(), b_ids.size());

    for (size_t i = 0; i < shared; ++i) {
        const std::string& ia = a_ids[i];
        const std::string& ib = b_ids[i];
        if (ia == ib) {
            continue;
        }
        const bool num_a = IsAllDigits(ia);
        const bool num_b = IsAllDigits(ib);
        if (num_a && num_b) {
            const int cmp = CompareNumericStrings(ia, ib);
            if (cmp != 0) {
                return cmp;
            }
        } else if (num_a != num_b) {
            return num_a ? -1 : 1;
        } else {
            return (ia < ib) ? -1 : 1;
        }
    }

    if (a_ids.size() != b_ids.size()) {
        return (a_ids.size() < b_ids.size()) ? -1 : 1;
    }
    return 0;
}

bool IsNewerVersion(const std::string& current_version, const std::string& candidate_version) {
    const SemVer cur = ParseSemVer(current_version);
    const SemVer cand = ParseSemVer(candidate_version);
    if (!cur.valid || !cand.valid) {
        return false;
    }
    return CompareSemVer(cur, cand) < 0;
}

bool IsUpdateCheckDisabledByEnv() {
    char buf[64] = {};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_NO_UPDATE_CHECK", buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    const std::string val = ToLowerAscii(TrimAscii(std::string(buf, len)));
    return val == "1" || val == "true" || val == "yes";
}

Status ParseGitHubReleaseJson(const std::string& json_text, UpdateReleaseInfo& out_info) {
    MinimalJsonParser parser(json_text);
    return parser.ParseGitHubRelease(out_info);
}

Status ParseSha256ChecksumFile(const std::string& checksum_text,
                               std::string& out_hex_lower,
                               std::string* out_filename) {
    out_hex_lower.clear();
    if (out_filename != nullptr) {
        out_filename->clear();
    }

    std::string text = checksum_text;
    // Strip UTF-8 BOM if present.
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEFu &&
        static_cast<unsigned char>(text[1]) == 0xBBu &&
        static_cast<unsigned char>(text[2]) == 0xBFu) {
        text.erase(0, 3);
    }

    // Trim leading whitespace/blank lines to reach the first non-empty line.
    size_t start = 0;
    while (start < text.size() && IsAsciiSpace(text[start])) {
        ++start;
    }
    if (start >= text.size()) {
        return Status::Error(StatusCode::ParseError, "SHA-256 checksum text is empty");
    }

    size_t line_end = text.find_first_of("\r\n", start);
    std::string line = (line_end == std::string::npos)
                           ? text.substr(start)
                           : text.substr(start, line_end - start);
    line = TrimAscii(line);

    if (line.size() < 64) {
        return Status::Error(StatusCode::ParseError, "SHA-256 checksum is shorter than 64 hex characters");
    }

    std::string hex;
    hex.reserve(64);
    for (size_t i = 0; i < 64; ++i) {
        if (!IsAsciiHex(line[i])) {
            return Status::Error(StatusCode::ParseError, "SHA-256 checksum contains non-hex characters");
        }
        hex.push_back(AsciiLower(line[i]));
    }

    std::string filename;
    if (line.size() > 64) {
        if (line[64] != ' ' && line[64] != '\t') {
            return Status::Error(
                StatusCode::ParseError,
                "SHA-256 checksum must be exactly 64 hex characters followed by whitespace or end-of-line");
        }
        size_t fn_pos = 64;
        while (fn_pos < line.size() && (line[fn_pos] == ' ' || line[fn_pos] == '\t')) {
            ++fn_pos;
        }
        if (fn_pos < line.size() && line[fn_pos] == '*') {
            ++fn_pos;
        }
        if (fn_pos < line.size()) {
            filename = TrimAscii(line.substr(fn_pos));
        }
    }

    out_hex_lower = std::move(hex);
    if (out_filename != nullptr) {
        *out_filename = std::move(filename);
    }
    return Status::Ok();
}

Status ComputeBufferSha256Hex(const uint8_t* data, size_t size, std::string& out_hex_lower) {
    out_hex_lower.clear();
    if (size > 0 && data == nullptr) {
        return Status::Error(StatusCode::InvalidArgument, "Null buffer with non-zero size");
    }

    BCryptSha256Hasher hasher;
    Status st = hasher.Init();
    if (!st.ok()) {
        return st;
    }
    st = hasher.Update(data, size);
    if (!st.ok()) {
        return st;
    }
    return hasher.Finalize(out_hex_lower);
}

Status ComputeFileSha256Hex(const std::filesystem::path& file_path, std::string& out_hex_lower) {
    out_hex_lower.clear();
    if (file_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty file path for SHA-256");
    }

    UniqueHandle file(CreateFileW(
        file_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.Valid()) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return Status::Error(StatusCode::NotFound, "File not found for SHA-256");
        }
        return Status::Error(StatusCode::IoError, FormatWin32Error("Failed to open file for SHA-256", err));
    }

    BCryptSha256Hasher hasher;
    Status st = hasher.Init();
    if (!st.ok()) {
        return st;
    }

    std::vector<uint8_t> chunk(kFileHashChunkBytes);
    while (true) {
        DWORD bytes_read = 0;
        if (!ReadFile(file.Get(), chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read, nullptr)) {
            return Status::Error(StatusCode::IoError, FormatWin32Error("ReadFile failed during SHA-256", GetLastError()));
        }
        if (bytes_read == 0) {
            break;
        }
        st = hasher.Update(chunk.data(), bytes_read);
        if (!st.ok()) {
            return st;
        }
    }

    return hasher.Finalize(out_hex_lower);
}

Sha256VerifyResult VerifyFileSha256(const std::filesystem::path& file_path,
                                    const std::string& expected_checksum_text) {
    Sha256VerifyResult result;

    Status st = ParseSha256ChecksumFile(expected_checksum_text, result.expected_hex);
    if (!st.ok()) {
        result.error_message = "Malformed expected SHA-256 checksum: " + st.message;
        return result;
    }

    st = ComputeFileSha256Hex(file_path, result.computed_hex);
    if (!st.ok()) {
        result.error_message = "Failed to compute file SHA-256: " + st.message;
        return result;
    }

    uint8_t diff = 0;
    if (result.computed_hex.size() == result.expected_hex.size()) {
        for (size_t i = 0; i < result.computed_hex.size(); ++i) {
            diff |= static_cast<uint8_t>(result.computed_hex[i] ^ result.expected_hex[i]);
        }
    } else {
        diff = 1;
    }

    if (diff != 0) {
        result.error_message =
            "SHA-256 verification failed: expected " + result.expected_hex +
            ", computed " + result.computed_hex;
        return result;
    }

    result.ok = true;
    return result;
}

UpdateCheckResult CheckLatestGitHubRelease(const std::string& current_version,
                                           bool offline_mode,
                                           const std::string& repo_owner,
                                           const std::string& repo_name) {
    UpdateCheckResult result;
    result.current_version = current_version;

    if (offline_mode || IsUpdateCheckDisabledByEnv()) {
        result.ok = true;
        result.skipped_offline = true;
        return result;
    }

    if (!IsSafeRepoToken(repo_owner) || !IsSafeRepoToken(repo_name)) {
        result.error_message = "Invalid GitHub repository owner or name";
        return result;
    }

    UniqueWinHttp session = OpenWinHttpSession(5000, 5000, 10000, 10000);
    if (!session.Valid()) {
        result.error_message = FormatWin32Error("WinHttpOpen failed", GetLastError());
        return result;
    }

    UniqueWinHttp connect(
        WinHttpConnect(session.Get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect.Valid()) {
        result.error_message = FormatWin32Error("WinHttpConnect(api.github.com) failed", GetLastError());
        return result;
    }

    const std::wstring path_w =
        Utf8ToWide("/repos/" + repo_owner + "/" + repo_name + "/releases/latest");
    UniqueWinHttp request(WinHttpOpenRequest(
        connect.Get(),
        L"GET",
        path_w.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.Valid()) {
        result.error_message = FormatWin32Error("WinHttpOpenRequest failed", GetLastError());
        return result;
    }

    ConfigureSecureRequestOptions(request.Get());

    const wchar_t* headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n"
        L"User-Agent: OpenScarlett-Launcher/0.4.0\r\n";

    if (!WinHttpSendRequest(
            request.Get(),
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        result.error_message = FormatWin32Error("WinHttpSendRequest failed", GetLastError());
        return result;
    }

    if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
        result.error_message = FormatWin32Error("WinHttpReceiveResponse failed", GetLastError());
        return result;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        result.error_message = FormatWin32Error("WinHttpQueryHeaders failed", GetLastError());
        return result;
    }

    if (status_code != 200u) {
        std::ostringstream oss;
        oss << "GitHub Releases API returned HTTP " << status_code;
        result.error_message = oss.str();
        return result;
    }

    std::string body;
    body.reserve(16384);
    std::array<char, 8192> buf{};

    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
            result.error_message = FormatWin32Error("WinHttpQueryDataAvailable failed", GetLastError());
            return result;
        }
        if (avail == 0) {
            break;
        }
        const DWORD to_read = static_cast<DWORD>(std::min<size_t>(avail, buf.size()));
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request.Get(), buf.data(), to_read, &bytes_read)) {
            result.error_message = FormatWin32Error("WinHttpReadData failed", GetLastError());
            return result;
        }
        if (bytes_read == 0) {
            break;
        }
        if (body.size() + bytes_read > kMaxApiResponseBytes) {
            result.error_message = "GitHub Releases API response exceeded 2 MiB safety cap";
            return result;
        }
        body.append(buf.data(), bytes_read);
    }

    const Status parse_st = ParseGitHubReleaseJson(body, result.release);
    if (!parse_st.ok()) {
        result.error_message = "Failed to parse GitHub release JSON: " + parse_st.message;
        return result;
    }

    result.release.update_available = IsNewerVersion(current_version, result.release.tag_name);
    result.ok = true;
    return result;
}

UpdateApplyResult VerifyAndStageLocalUpdateFiles(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& sha256_path,
    const std::filesystem::path& app_dir,
    const std::string& tag_name,
    const std::function<void(const UpdateDownloadProgress&)>& on_progress) {
    UpdateApplyResult result;

    if (app_dir.empty()) {
        result.error_message = "Application directory path is empty";
        return result;
    }

    const std::filesystem::path staging_dir = app_dir / "_openscarlett_update_staging";
    const bool inputs_in_staging =
        IsPathInsideDirectory(zip_path, staging_dir) ||
        IsPathInsideDirectory(sha256_path, staging_dir);

    auto fail_and_purge = [&](const std::string& msg) -> UpdateApplyResult {
        CleanDirectoryBestEffort(staging_dir);
        result.ok = false;
        result.staging_dir.clear();
        result.restart_script_path.clear();
        result.verified_sha256.clear();
        result.error_message = msg;
        return result;
    };

    if (!inputs_in_staging) {
        CleanDirectoryBestEffort(staging_dir);
    }

    if (on_progress) {
        UpdateDownloadProgress p;
        p.stage = "Verifying SHA-256...";
        p.percent = 50;
        on_progress(p);
    }

    // 1. Read .sha256 file and verify zip SHA-256 strictly BEFORE any extraction.
    std::string sha256_text;
    Status st = ReadSmallTextFile(sha256_path, kMaxSha256FileBytes, sha256_text);
    if (!st.ok()) {
        return fail_and_purge("Failed to read .sha256 checksum file: " + st.message);
    }

    const Sha256VerifyResult verify = VerifyFileSha256(zip_path, sha256_text);
    if (!verify.ok) {
        return fail_and_purge(verify.error_message);
    }

    // 2. Prepare clean extraction directory inside staging_dir.
    const std::filesystem::path extract_dir = staging_dir / "extracted";
    CleanDirectoryBestEffort(extract_dir);
    std::error_code ec;
    std::filesystem::create_directories(extract_dir, ec);
    if (ec) {
        return fail_and_purge("Failed to create staging extraction directory: " + ec.message());
    }

    if (on_progress) {
        UpdateDownloadProgress p;
        p.stage = "Extracting...";
        p.percent = 75;
        on_progress(p);
    }

    // 3. Extract verified zip archive (tar.exe -> .NET ZipFile -> native STORE fallback).
    st = ExtractZipArchive(zip_path, extract_dir);
    if (!st.ok()) {
        return fail_and_purge(st.message);
    }

    const std::filesystem::directory_iterator dir_it(extract_dir, ec);
    const bool has_entries = !ec && (dir_it != std::filesystem::directory_iterator());
    if (ec || !has_entries) {
        return fail_and_purge("Extracted update archive is empty");
    }

    const std::filesystem::path payload_dir = ResolveExtractedPayloadDir(extract_dir);

    // 4. Write transparent restart-and-swap helper script.
    const std::filesystem::path script_path = staging_dir / "_openscarlett_apply_update.cmd";
    st = WriteRestartApplyScript(script_path, app_dir, staging_dir, payload_dir, tag_name);
    if (!st.ok()) {
        return fail_and_purge(st.message);
    }

    if (on_progress) {
        UpdateDownloadProgress p;
        p.stage = "Ready to apply update";
        p.percent = 100;
        on_progress(p);
    }

    result.ok = true;
    result.staging_dir = staging_dir;
    result.restart_script_path = script_path;
    result.verified_sha256 = verify.computed_hex;
    return result;
}

UpdateApplyResult DownloadVerifyAndStageUpdate(
    const UpdateReleaseInfo& release,
    const std::filesystem::path& app_dir,
    const std::function<void(const UpdateDownloadProgress&)>& on_progress,
    const std::atomic<bool>* cancel_flag) {
    UpdateApplyResult result;

    if (app_dir.empty()) {
        result.error_message = "Application directory path is empty";
        return result;
    }
    if (!IsHttpsUrl(release.zip_download_url)) {
        result.error_message = "Release zip_download_url must be a valid HTTPS URL";
        return result;
    }
    if (!IsHttpsUrl(release.sha256_download_url)) {
        result.error_message = "Release sha256_download_url must be a valid HTTPS URL";
        return result;
    }

    const std::filesystem::path staging_dir = app_dir / "_openscarlett_update_staging";
    CleanDirectoryBestEffort(staging_dir);

    std::error_code ec;
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        result.error_message = "Failed to create update staging directory: " + ec.message();
        return result;
    }

    const std::string safe_tag = SanitizeAssetFilename(
        release.tag_name.empty() ? "update" : release.tag_name, "update");
    const std::string zip_name = SanitizeAssetFilename(
        release.zip_asset_name, "OpenScarlett-" + safe_tag + "-win64.zip");
    const std::string sha_name = SanitizeAssetFilename(
        release.sha256_asset_name, zip_name + ".sha256");

    const std::filesystem::path zip_dest = staging_dir / zip_name;
    const std::filesystem::path sha_dest = staging_dir / sha_name;

    Status st = DownloadHttpsFile(
        release.sha256_download_url,
        sha_dest,
        kMaxSha256FileBytes,
        "Downloading SHA-256 checksum...",
        on_progress,
        cancel_flag);
    if (!st.ok()) {
        CleanDirectoryBestEffort(staging_dir);
        result.error_message = "Failed to download .sha256 file: " + st.message;
        return result;
    }

    st = DownloadHttpsFile(
        release.zip_download_url,
        zip_dest,
        kMaxZipDownloadBytes,
        "Downloading zip...",
        on_progress,
        cancel_flag);
    if (!st.ok()) {
        CleanDirectoryBestEffort(staging_dir);
        result.error_message = "Failed to download update zip: " + st.message;
        return result;
    }

    return VerifyAndStageLocalUpdateFiles(zip_dest, sha_dest, app_dir, release.tag_name, on_progress);
}

Status LaunchUpdateRestartHelper(const std::filesystem::path& restart_script_path,
                                 uint32_t parent_pid) {
    if (restart_script_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Restart helper script path is empty");
    }

    std::error_code ec;
    if (!std::filesystem::exists(restart_script_path, ec) || ec) {
        return Status::Error(StatusCode::NotFound, "Restart helper script does not exist");
    }

    const std::wstring cmd_exe = GetWindowsSystemExecutable(L"cmd.exe");
    std::wstring cmdline = L"\"" + cmd_exe + L"\" /c \"\"" +
                           restart_script_path.wstring() + L"\" " +
                           std::to_wstring(parent_pid) + L"\"";

    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    // Run with working directory outside staging_dir so the script can rmdir staging_dir cleanly.
    std::filesystem::path work_dir = restart_script_path.parent_path().parent_path();
    const wchar_t* work_dir_ptr = nullptr;
    std::wstring work_dir_w;
    if (!work_dir.empty() && std::filesystem::exists(work_dir, ec)) {
        work_dir_w = work_dir.wstring();
        work_dir_ptr = work_dir_w.c_str();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            cmd_exe.c_str(),
            cmd_buf.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
            nullptr,
            work_dir_ptr,
            &si,
            &pi)) {
        return Status::Error(
            StatusCode::IoError,
            FormatWin32Error("Failed to launch update restart helper", GetLastError()));
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Status::Ok();
}

}  // namespace openscarlett::launcher
