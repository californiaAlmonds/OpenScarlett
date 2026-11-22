#include "openscarlett/launcher/hwtest_runner.h"
#include "launcher_probe_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "openscarlett/loader/pe_inspector.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winver.h>
#endif

namespace openscarlett::launcher {

namespace {

constexpr size_t kGitHubUrlBudget = 6200;
constexpr const char* kGitHubIssuesNewUrl =
    "https://github.com/californiaAlmonds/OpenScarlett/issues/new";

std::string ToLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string TrimAscii(std::string_view input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(begin, end - begin));
}

std::string TrimTrailingWhitespace(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    return text;
}

std::string NormalizeNewlines(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

std::string StripTrailingSeparators(std::string path_str) {
    while (path_str.size() > 3 && (path_str.back() == '\\' || path_str.back() == '/')) {
        path_str.pop_back();
    }
    return path_str;
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool StartsWithIgnoreCaseAscii(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    return EqualsIgnoreCaseAscii(text.substr(0, prefix.size()), prefix);
}

bool EndsWithIgnoreCaseAscii(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    return EqualsIgnoreCaseAscii(text.substr(text.size() - suffix.size()), suffix);
}

std::string ReplaceAllIgnoreCaseAscii(const std::string& text,
                                      const std::string& needle,
                                      const std::string& replacement) {
    if (needle.empty() || text.size() < needle.size()) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        if (pos + needle.size() <= text.size() &&
            EqualsIgnoreCaseAscii(std::string_view(text.data() + pos, needle.size()), needle)) {
            out.append(replacement);
            pos += needle.size();
        } else {
            out.push_back(text[pos]);
            ++pos;
        }
    }
    return out;
}

std::string ReplaceDriveUsersUsername(const std::string& text, const std::string& username) {
    if (text.empty() || username.empty()) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        bool matched = false;
        if (i + 8 < text.size() &&
            std::isalpha(static_cast<unsigned char>(text[i])) != 0 &&
            text[i + 1] == ':') {
            size_t p = i + 2;
            const size_t sep1_start = p;
            while (p < text.size() && (text[p] == '\\' || text[p] == '/')) {
                ++p;
            }
            if (p > sep1_start && p + 5 <= text.size() &&
                EqualsIgnoreCaseAscii(std::string_view(text.data() + p, 5), "Users")) {
                p += 5;
                const size_t sep2_start = p;
                while (p < text.size() && (text[p] == '\\' || text[p] == '/')) {
                    ++p;
                }
                if (p > sep2_start && p + username.size() <= text.size() &&
                    EqualsIgnoreCaseAscii(std::string_view(text.data() + p, username.size()), username)) {
                    out.push_back(text[i]);
                    out.append(":\\Users\\<USER>");
                    i = p + username.size();
                    matched = true;
                }
            }
        }
        if (!matched) {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

std::string FormatSeconds(double seconds) {
    const double rounded = std::round(seconds * 100.0) / 100.0;
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.2f", rounded);
    return std::string(buf);
}

std::string EscapeMarkdownTableCell(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '|') {
            out.append("\\|");
        } else if (ch == '\r' || ch == '\n') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string UriEscapeRfc3986(std::string_view raw) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() + raw.size() / 4);
    for (char c : raw) {
        const unsigned char b = static_cast<unsigned char>(c);
        const bool is_unreserved =
            (b >= 'A' && b <= 'Z') ||
            (b >= 'a' && b <= 'z') ||
            (b >= '0' && b <= '9') ||
            b == '-' || b == '_' || b == '.' || b == '~';
        if (is_unreserved) {
            out.push_back(static_cast<char>(b));
        } else {
            out.push_back('%');
            out.push_back(kHex[(b >> 4) & 0x0Fu]);
            out.push_back(kHex[b & 0x0Fu]);
        }
    }
    return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string norm = NormalizeNewlines(text);
    size_t start = 0;
    while (start <= norm.size()) {
        const size_t nl = norm.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(norm.substr(start));
            break;
        }
        lines.push_back(norm.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::vector<std::string> SplitChar(std::string_view text, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(delim, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

bool ParseUint64Strict(std::string_view text, uint64_t& out_val, int base = 10) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return false;
    }
    char* end_ptr = nullptr;
    const unsigned long long parsed = std::strtoull(trimmed.c_str(), &end_ptr, base);
    if (end_ptr == trimmed.c_str()) {
        return false;
    }
    out_val = static_cast<uint64_t>(parsed);
    return true;
}

bool ExtractJsonStringField(const std::string& line, const std::string& key, std::string& out_val) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        const char ch = line[pos++];
        if (ch == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (ch == '"') {
            out_val = std::move(val);
            return true;
        } else {
            val.push_back(ch);
        }
    }
    return false;
}

bool ExtractJsonUintField(const std::string& line, const std::string& key, uint64_t& out_val) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) {
        return false;
    }
    pos += pat.size();
    char* end_ptr = nullptr;
    const unsigned long long parsed = std::strtoull(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) {
        return false;
    }
    out_val = static_cast<uint64_t>(parsed);
    return true;
}

std::string BuildSummaryTableMarkdown(const std::vector<HardwareTestRow>& rows, bool include_notes) {
    std::ostringstream oss;
    if (include_notes) {
        oss << "| Check | Result | Time | Notes |\n";
        oss << "| :--- | :---: | :---: | :--- |\n";
    } else {
        oss << "| Check | Result | Time |\n";
        oss << "| :--- | :---: | :---: |\n";
    }
    for (const auto& r : rows) {
        const std::string badge = r.passed
                                      ? "PASS"
                                      : ("FAIL (exit " + std::to_string(r.exit_code) + ")");
        if (include_notes) {
            oss << "| `" << r.name << "` | **" << badge << "** | "
                << FormatSeconds(r.seconds) << "s | "
                << EscapeMarkdownTableCell(r.highlight) << " |\n";
        } else {
            oss << "| `" << r.name << "` | **" << badge << "** | "
                << FormatSeconds(r.seconds) << "s |\n";
        }
    }
    std::string result = oss.str();
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

#if defined(_WIN32)

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::string PathToUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.wstring());
}

std::string GetEnvVarUtf8(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring buf(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return WideToUtf8(buf);
}

std::string GetCurrentUsernameUtf8() {
    std::string env_user = GetEnvVarUtf8(L"USERNAME");
    if (!env_user.empty()) {
        return env_user;
    }
    wchar_t buf[256]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (::GetUserNameW(buf, &len) != FALSE && len > 1) {
        return WideToUtf8(std::wstring_view(buf, len - 1));
    }
    return {};
}

std::wstring QuoteWin32ArgW(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildChildEnvironmentBlockW(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    struct EnvEntry {
        std::wstring key;
        std::wstring value;
    };
    std::vector<EnvEntry> entries;

    LPWCH raw_env = ::GetEnvironmentStringsW();
    if (raw_env != nullptr) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            const std::wstring_view entry(cur);
            cur += entry.size() + 1;
            if (entry.empty()) {
                continue;
            }
            // Keep special per-drive entries like "=C:=C:\..." intact (search '=' after index 0).
            const size_t eq = entry.find(L'=', 1);
            if (eq != std::wstring_view::npos) {
                entries.push_back(EnvEntry{
                    std::wstring(entry.substr(0, eq)),
                    std::wstring(entry.substr(eq + 1)),
                });
            }
        }
        ::FreeEnvironmentStringsW(raw_env);
    }

    for (const auto& [ov_key, ov_val] : overrides) {
        if (ov_key.empty()) {
            continue;
        }
        bool updated = false;
        for (auto& existing : entries) {
            if (::CompareStringOrdinal(existing.key.c_str(),
                                       static_cast<int>(existing.key.size()),
                                       ov_key.c_str(),
                                       static_cast<int>(ov_key.size()),
                                       TRUE) == CSTR_EQUAL) {
                existing.value = ov_val;
                updated = true;
                break;
            }
        }
        if (!updated) {
            entries.push_back(EnvEntry{ov_key, ov_val});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const EnvEntry& a, const EnvEntry& b) {
        return ::CompareStringOrdinal(a.key.c_str(),
                                      static_cast<int>(a.key.size()),
                                      b.key.c_str(),
                                      static_cast<int>(b.key.size()),
                                      TRUE) == CSTR_LESS_THAN;
    });

    std::wstring block;
    for (const auto& e : entries) {
        block.append(e.key);
        block.push_back(L'=');
        block.append(e.value);
        block.push_back(L'\0');
    }
    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool QueryRegStringValue(HKEY hkey, const wchar_t* value_name, std::string& out_utf8) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExW(hkey, value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return false;
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(hkey,
                           value_name,
                           nullptr,
                           &type,
                           reinterpret_cast<LPBYTE>(buf.data()),
                           &bytes) != ERROR_SUCCESS) {
        return false;
    }
    size_t chars = bytes / sizeof(wchar_t);
    while (chars > 0 && buf[chars - 1] == L'\0') {
        --chars;
    }
    out_utf8 = WideToUtf8(std::wstring_view(buf.data(), chars));
    return !out_utf8.empty();
}

bool QueryRegDwordValue(HKEY hkey, const wchar_t* value_name, DWORD& out_val) {
    DWORD type = 0;
    DWORD val = 0;
    DWORD bytes = sizeof(val);
    if (::RegQueryValueExW(hkey,
                           value_name,
                           nullptr,
                           &type,
                           reinterpret_cast<LPBYTE>(&val),
                           &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        return false;
    }
    out_val = val;
    return true;
}

bool QueryFileVersionString(const std::filesystem::path& file_path, std::string& out_ver) {
    const std::wstring wpath = file_path.wstring();
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(wpath.c_str(), &handle);
    if (size == 0) {
        return false;
    }
    std::vector<uint8_t> data(size);
    if (::GetFileVersionInfoW(wpath.c_str(), 0, size, data.data()) == FALSE) {
        return false;
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffi_len = 0;
    if (::VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffi_len) == FALSE ||
        ffi == nullptr || ffi_len < sizeof(VS_FIXEDFILEINFO)) {
        return false;
    }
    char buf[64]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
    out_ver = buf;
    return true;
}

std::string CurrentUtcTimestampString() {
    SYSTEMTIME st{};
    ::GetSystemTime(&st);
    char buf[32]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "%04u-%02u-%02u %02u:%02u:%02u",
                  static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay),
                  static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute),
                  static_cast<unsigned>(st.wSecond));
    return std::string(buf);
}

double ElapsedSecondsFromQpc(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq) noexcept {
    if (freq.QuadPart <= 0) {
        return 0.0;
    }
    return static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
}

struct ProcessRunOutput {
    bool launched = false;
    int exit_code = -1;
    bool cancelled = false;
    bool timed_out = false;
    double elapsed_seconds = 0.0;
    std::string raw_output;
    std::string error_message;
};

ProcessRunOutput RunProcessCapture(
    const std::filesystem::path& exe_path,
    const std::wstring& command_line,
    const std::filesystem::path& working_dir,
    const std::vector<std::pair<std::wstring, std::wstring>>& env_overrides,
    const std::function<void(const std::string&)>& on_line,
    const std::atomic<bool>* cancel_flag,
    uint32_t timeout_ms) {
    ProcessRunOutput res{};

    std::error_code ec;
    if (!std::filesystem::exists(exe_path, ec)) {
        res.error_message = "Executable not found: " + PathToUtf8(exe_path);
        return res;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (::CreatePipe(&read_pipe, &write_pipe, &sa, 0) == FALSE) {
        res.error_message = "CreatePipe failed (Win32 error " + std::to_string(::GetLastError()) + ")";
        return res;
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in = ::CreateFileW(L"NUL",
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nul_in != INVALID_HANDLE_VALUE) ? nul_in : ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    std::wstring mutable_cmd = command_line;
    std::wstring env_block;
    LPVOID env_ptr = nullptr;
    DWORD creation_flags = CREATE_NO_WINDOW;
    if (!env_overrides.empty()) {
        env_block = BuildChildEnvironmentBlockW(env_overrides);
        env_ptr = env_block.data();
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    const std::wstring exe_w = exe_path.wstring();
    const std::wstring work_w = working_dir.empty() ? std::wstring{} : working_dir.wstring();
    LPCWSTR work_ptr = work_w.empty() ? nullptr : work_w.c_str();

    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(exe_w.c_str(),
                                          mutable_cmd.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          creation_flags,
                                          env_ptr,
                                          work_ptr,
                                          &si,
                                          &pi);

    ::CloseHandle(write_pipe);
    if (nul_in != INVALID_HANDLE_VALUE) {
        ::CloseHandle(nul_in);
    }

    if (created == FALSE) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(read_pipe);
        res.error_message = "CreateProcessW failed for " + PathToUtf8(exe_path) +
                            " (Win32 error " + std::to_string(err) + ")";
        return res;
    }

    res.launched = true;

    // Best-effort Job Object so cancelling/timing out cleans up the full child process tree.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    bool in_job = false;
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(job,
                                      JobObjectExtendedLimitInformation,
                                      &jeli,
                                      sizeof(jeli)) != FALSE &&
            ::AssignProcessToJobObject(job, pi.hProcess) != FALSE) {
            in_job = true;
        } else {
            ::CloseHandle(job);
            job = nullptr;
        }
    }

    std::string pending_line;
    auto ingest_chunk = [&](const char* data, DWORD count) {
        res.raw_output.append(data, static_cast<size_t>(count));
        if (!on_line) {
            return;
        }
        for (DWORD i = 0; i < count; ++i) {
            const char ch = data[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                on_line(pending_line);
                pending_line.clear();
            } else {
                pending_line.push_back(ch);
            }
        }
    };

    auto drain_pipe = [&]() -> bool {
        char buf[4096];
        bool read_any = false;
        for (;;) {
            DWORD avail = 0;
            if (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) == FALSE) {
                break;
            }
            if (avail == 0) {
                break;
            }
            const DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
            DWORD got = 0;
            if (::ReadFile(read_pipe, buf, to_read, &got, nullptr) == FALSE || got == 0) {
                break;
            }
            ingest_chunk(buf, got);
            read_any = true;
        }
        return read_any;
    };

    auto terminate_child = [&]() {
        if (in_job && job != nullptr) {
            ::TerminateJobObject(job, 1);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
        }
        ::WaitForSingleObject(pi.hProcess, 2000);
    };

    for (;;) {
        drain_pipe();

        if (cancel_flag != nullptr && cancel_flag->load(std::memory_order_relaxed)) {
            res.cancelled = true;
            terminate_child();
            drain_pipe();
            break;
        }

        if (timeout_ms > 0) {
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            const double elapsed_ms = ElapsedSecondsFromQpc(qpc_start, now, qpc_freq) * 1000.0;
            if (elapsed_ms >= static_cast<double>(timeout_ms)) {
                res.timed_out = true;
                terminate_child();
                drain_pipe();
                break;
            }
        }

        const DWORD wait_rc = ::WaitForSingleObject(pi.hProcess, 15);
        if (wait_rc == WAIT_OBJECT_0) {
            drain_pipe();
            break;
        }
    }

    if (on_line && !pending_line.empty()) {
        on_line(pending_line);
        pending_line.clear();
    }

    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceCounter(&qpc_end);
    res.elapsed_seconds = ElapsedSecondsFromQpc(qpc_start, qpc_end, qpc_freq);

    DWORD exit_code_dw = 1;
    if (::GetExitCodeProcess(pi.hProcess, &exit_code_dw) != FALSE) {
        res.exit_code = static_cast<int>(exit_code_dw);
    }

    if (job != nullptr) {
        ::CloseHandle(job);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(read_pipe);

    res.raw_output = NormalizeNewlines(res.raw_output);
    return res;
}

std::filesystem::path ResolveWorkRootDirForSanitize(
    const std::filesystem::path& bundle_dir,
    std::string_view user_profile_utf8) {
    if (bundle_dir.empty()) {
        return {};
    }
    auto lower_ascii = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    std::filesystem::path dir = bundle_dir.lexically_normal();
    while (!dir.empty() && dir.filename().empty() && dir != dir.root_path()) {
        dir = dir.parent_path();
    }
    std::string leaf = lower_ascii(PathToUtf8(dir.filename()));
    if (leaf == "debug" || leaf == "release" ||
        leaf == "relwithdebinfo" || leaf == "minsizerel") {
        const std::filesystem::path parent = dir.parent_path();
        const std::string parent_leaf = lower_ascii(PathToUtf8(parent.filename()));
        if (!parent.empty() && (parent_leaf == "build" || parent_leaf == "bin")) {
            dir = parent;
            leaf = parent_leaf;
        }
    }
    if (leaf != "build" && leaf != "bin") {
        return {};
    }
    const std::filesystem::path root = dir.parent_path();
    const std::string root_clean = StripTrailingSeparators(PathToUtf8(root));
    if (root_clean.size() <= 2) {
        return {};
    }
    if (!user_profile_utf8.empty()) {
        std::string up = StripTrailingSeparators(std::string(user_profile_utf8));
        std::string rt = root_clean;
        std::replace(up.begin(), up.end(), '/', '\\');
        std::replace(rt.begin(), rt.end(), '/', '\\');
        if (!up.empty()) up.push_back('\\');
        if (!rt.empty()) rt.push_back('\\');
        if (StartsWithIgnoreCaseAscii(up, rt)) {
            return {};
        }
    }
    return root;
}

#endif  // defined(_WIN32)

}  // namespace

std::string SanitizeReportText(const std::string& text,
                               const std::filesystem::path& bundle_dir) {
    if (text.empty()) {
        return {};
    }

    struct ReplaceRule {
        std::string key;
        std::string val;
    };
    std::vector<ReplaceRule> rules;
    auto add_path_rule = [&](const std::string& raw_path, const char* token) {
        const std::string clean = StripTrailingSeparators(TrimAscii(raw_path));
        if (clean.empty()) {
            return;
        }
        rules.push_back(ReplaceRule{clean, token});
        std::string fwd = clean;
        std::replace(fwd.begin(), fwd.end(), '\\', '/');
        if (fwd != clean) {
            rules.push_back(ReplaceRule{std::move(fwd), token});
        }
        std::string back = clean;
        std::replace(back.begin(), back.end(), '/', '\\');
        if (back != clean) {
            rules.push_back(ReplaceRule{std::move(back), token});
        }
    };

#if defined(_WIN32)
    const std::string user_profile = GetEnvVarUtf8(L"USERPROFILE");
    if (!bundle_dir.empty()) {
        add_path_rule(PathToUtf8(bundle_dir), "<BUNDLE_DIR>");
        std::error_code ec;
        const auto norm = bundle_dir.lexically_normal();
        if (norm != bundle_dir) {
            add_path_rule(PathToUtf8(norm), "<BUNDLE_DIR>");
        }
        const auto canonical = std::filesystem::weakly_canonical(bundle_dir, ec);
        if (!ec && !canonical.empty()) {
            add_path_rule(PathToUtf8(canonical), "<BUNDLE_DIR>");
        }
        const auto work_root = ResolveWorkRootDirForSanitize(bundle_dir, user_profile);
        if (!work_root.empty()) {
            add_path_rule(PathToUtf8(work_root), "<REPO_DIR>");
            const auto work_norm = work_root.lexically_normal();
            if (work_norm != work_root) {
                add_path_rule(PathToUtf8(work_norm), "<REPO_DIR>");
            }
            const auto work_canon = std::filesystem::weakly_canonical(work_root, ec);
            if (!ec && !work_canon.empty()) {
                add_path_rule(PathToUtf8(work_canon), "<REPO_DIR>");
            }
        }
    }
    add_path_rule(user_profile, "<USER_HOME>");
    add_path_rule(GetEnvVarUtf8(L"TEMP"), "<TEMP>");
    add_path_rule(GetEnvVarUtf8(L"LOCALAPPDATA"), "<LOCALAPPDATA>");
    add_path_rule(GetEnvVarUtf8(L"APPDATA"), "<APPDATA>");
#else
    if (!bundle_dir.empty()) {
        add_path_rule(bundle_dir.string(), "<BUNDLE_DIR>");
    }
#endif

    // Longest paths first so nested paths (<TEMP>, <LOCALAPPDATA>) never partially match <USER_HOME>.
    std::sort(rules.begin(), rules.end(), [](const ReplaceRule& a, const ReplaceRule& b) {
        if (a.key.size() != b.key.size()) {
            return a.key.size() > b.key.size();
        }
        return a.key < b.key;
    });

    std::string out = text;
    for (const auto& rule : rules) {
        out = ReplaceAllIgnoreCaseAscii(out, rule.key, rule.val);
    }

#if defined(_WIN32)
    const std::string username = GetCurrentUsernameUtf8();
    if (!username.empty()) {
        out = ReplaceDriveUsersUsername(out, username);
    }
#endif

    // Also scrub any generic "<Drive>:\Users\<ProfileName>" or "<Drive>:/Users/<ProfileName>"
    // path segment (excluding system accounts like Public/Default) so profile directory names
    // that differ from %USERNAME% (e.g. Microsoft Accounts) are never leaked.
    {
        auto to_lower_ascii = [](std::string_view sv) {
            std::string r(sv);
            for (char& c : r) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return r;
        };
        std::string scrubbed;
        scrubbed.reserve(out.size());
        size_t i = 0;
        while (i < out.size()) {
            const char ch = out[i];
            const bool is_drive = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) &&
                                  (i + 2 < out.size()) && out[i + 1] == ':' &&
                                  (out[i + 2] == '\\' || out[i + 2] == '/');
            if (is_drive) {
                size_t j = i + 2;
                while (j < out.size() && (out[j] == '\\' || out[j] == '/')) {
                    ++j;
                }
                if (j + 5 < out.size() &&
                    to_lower_ascii(std::string_view(out).substr(j, 5)) == "users" &&
                    (out[j + 5] == '\\' || out[j + 5] == '/')) {
                    size_t name_start = j + 5;
                    while (name_start < out.size() &&
                           (out[name_start] == '\\' || out[name_start] == '/')) {
                        ++name_start;
                    }
                    size_t name_end = name_start;
                    while (name_end < out.size() &&
                           out[name_end] != '\\' && out[name_end] != '/' &&
                           out[name_end] != '\r' && out[name_end] != '\n' &&
                           out[name_end] != ' ' && out[name_end] != '\t' &&
                           out[name_end] != '"' && out[name_end] != '\'' &&
                           out[name_end] != ')' && out[name_end] != ']' &&
                           out[name_end] != ';') {
                        ++name_end;
                    }
                    if (name_end > name_start) {
                        const std::string seg_lower =
                            to_lower_ascii(std::string_view(out).substr(name_start, name_end - name_start));
                        if (seg_lower != "<user>" && seg_lower != "public" &&
                            seg_lower != "default" && seg_lower != "all") {
                            scrubbed.append(out, i, name_start - i);
                            scrubbed.append("<USER>");
                            i = name_end;
                            continue;
                        }
                    }
                }
            }
            scrubbed.push_back(ch);
            ++i;
        }
        out = std::move(scrubbed);
    }

    return out;
}

std::string GetFriendlyWindowsOsVersion() {
#if defined(_WIN32)
    HKEY hkey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        0,
                        KEY_READ | KEY_WOW64_64KEY,
                        &hkey) == ERROR_SUCCESS &&
        hkey != nullptr) {
        std::string build_str;
        std::string display_ver;
        std::string release_id;
        DWORD ubr = 0;

        const bool has_build = QueryRegStringValue(hkey, L"CurrentBuildNumber", build_str);
        const bool has_ubr = QueryRegDwordValue(hkey, L"UBR", ubr);
        QueryRegStringValue(hkey, L"DisplayVersion", display_ver);
        if (display_ver.empty()) {
            QueryRegStringValue(hkey, L"ReleaseId", release_id);
            display_ver = release_id;
        }
        ::RegCloseKey(hkey);

        if (has_build) {
            uint64_t build_num = 0;
            ParseUint64Strict(build_str, build_num);
            const char* os_name = (build_num >= 22000) ? "Windows 11" : "Windows 10";

            SYSTEM_INFO si{};
            ::GetNativeSystemInfo(&si);
            const char* arch = "x64";
            if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
                arch = "ARM64";
            } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
                arch = "x86";
            }

            std::string ubr_suffix = has_ubr ? ("." + std::to_string(ubr)) : std::string{};
            if (!display_ver.empty()) {
                return std::string(os_name) + " " + display_ver + " " + arch +
                       " (Build " + std::to_string(build_num) + ubr_suffix + ")";
            }
            return std::string(os_name) + " " + arch +
                   " (Build " + std::to_string(build_num) + ubr_suffix + ")";
        }
    }
    return "Windows x64";
#else
    return "Non-Windows";
#endif
}

std::string GetVcRuntimeSummary(const std::filesystem::path& bundle_dir) {
#if defined(_WIN32)
    std::string summary = "not found";
    std::error_code ec;

    wchar_t sys_dir[MAX_PATH]{};
    const UINT sys_len = ::GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (sys_len > 0 && sys_len < MAX_PATH) {
        const std::filesystem::path sys_msvcp =
            std::filesystem::path(std::wstring(sys_dir, sys_len)) / L"msvcp140.dll";
        if (std::filesystem::exists(sys_msvcp, ec)) {
            std::string ver;
            if (QueryFileVersionString(sys_msvcp, ver)) {
                summary = "System32 msvcp140.dll " + ver;
            } else {
                summary = "System32 msvcp140.dll (version unknown)";
            }
        }
    }

    if (!bundle_dir.empty()) {
        const std::filesystem::path local_msvcp = bundle_dir / L"msvcp140.dll";
        if (std::filesystem::exists(local_msvcp, ec)) {
            std::string local_ver;
            if (!QueryFileVersionString(local_msvcp, local_ver)) {
                local_ver = "unknown";
            }
            summary += " | WARNING: local bundle msvcp140.dll " + local_ver;
        }
    }
    return summary;
#else
    (void)bundle_dir;
    return "not applicable";
#endif
}

std::string GetHardwareTestDescription(const std::string& exe_name) {
    std::string key = exe_name;
    const size_t slash = key.find_last_of("\\/");
    if (slash != std::string::npos) {
        key = key.substr(slash + 1);
    }
    key = ToLowerAscii(TrimAscii(key));

    static const std::unordered_map<std::string, const char*> kDescriptions = {
        {"openscarlett.exe probe-gpu", "Host D3D12 / DXR / MeshTier / Vulkan / DXC capability probe"},
        {"test_pe_inspect.exe", "PE32+ import/export classifier & CallTelemetry JSON"},
        {"test_codecs.exe", "Xbox codecs (GDeflate v1, BCPack, XMA2 audio, Bink2 video)"},
        {"test_vulkan_live.exe", "Live Vulkan 1.2/1.4 GPU compute shader"},
        {"test_rdna2_core.exe", "RDNA2 / GFX10.3 AAA decoder, DXBC parser, CPU interpreter & shader cache"},
        {"test_winrt_xs.exe", "WinRT activation & HSTRING runtime (winrt_xs.dll)"},
        {"test_apc_loader.exe", "Suspended-process APC hook loader & G:/T:/S: VFS"},
        {"test_loader_vfs_config.exe", "MicrosoftGame.Config loader, CoW overlay VFS, forwarders/TLS/.pdata & 19-DLL routing"},
        {"test_kernelx.exe", "Win10/11 VirtualAlloc2/MapViewOfFile3 VA aliasing & kernelx.dll"},
        {"test_d3d12_xs_live.exe", "Live D3D12 COM proxy, xg_xs swizzle & GPU FSR 1.0"},
        {"test_gdk_shims.exe", "GDK runtime shims (xgameruntime, GameInput, XAudio2, dstorage, XSAPI)"},
        {"test_audio_opus_wasapi.exe", "Opus MFT (0x704F), IMA-ADPCM & live host WASAPI playback"},
        {"test_xg_dstorage.exe", "Full GFX10 2D/3D/mip-tail xg_xs swizzle & hardened dstorage_xs queues"},
        {"test_rdna2_lifter_live.exe", "RDNA2 -> SPIR-V & SM 6.5 DXIL lifting + CPU wave32/64 interpreter parity"},
        {"test_d3d12_xs_autolift.exe", "Live d3d12_xs XS2/Xbox-DXBC auto-lifting, disk shader cache & async PSO"},
        {"test_dxr_live.exe", "DXR Tier 1.1 hardware raytracing / SAH BVH fallback"},
        {"test_secondary_shims.exe", "xgameruntime QueryApiImpl dispatch & 9 secondary console/GDK shim DLLs"},
        {"test_presentx_windowing.exe", "Interactive Win32 PresentX swapchains (60/120 Hz, waitable, Alt+Enter) & GameInput"},
        {"test_gdk_live.exe", "End-to-end live Xbox GDK samples (SimpleTriangle, SimpleMeshShader, SimpleCompute, SystemCombo)"},
        {"test_gdk_simple_pbr.exe", "Multi-frame 3D Cook-Torrance SimplePBR (4x MSAA, D32 depth, PBR textures, CBs)"},
        {"test_gdk_meshlet_cull.exe", "AS+MS+PS GPU frustum & normal-cone MeshletCull (MeshTier=10 + WARP fallback)"},
        {"test_gdk_dxr_triangle.exe", "End-to-end DXRTriangle (native lib_6_3 HW RT cores / SAH BVH fallback)"},
        {"test_external_gdk_bringup.exe", "External GDK-header binaries, untouched MIT DirectStorageCombo & 19-DLL full stack"},
        {"test_vulkan_graphics_live.exe", "Full Vulkan 1.2/1.4 graphics, 4x MSAA, 3D textures, VK_EXT_mesh_shader & Win32 swapchain"},
        {"test_hdr10_d3d12.exe", "HDR10 ST.2084 & scRGB PresentX, SDR tone-map, default Texture2DArray SRV & VA remap"},
        {"test_gdk_extended_loader.exe", "XDisplay HDR, XGameUI/Invite/Speech, X3DAudio/XAudio2FX & topological DLL init order"},
        {"test_gdk_simple_lighting.exe", "DirectXTK12-style 3D Blinn-Phong SimpleLighting (D3D12 HW/WARP & Vulkan 1.4 parity)"},
        {"test_gdk_hdr_cubemap.exe", "HDRCubeMap (FP16 TextureCube + default Array SRV + HDR10 ST.2084 + SDR tone-map)"},
        {"test_vulkan_mesh_showcase.exe", "VulkanMeshShowcase (RDNA2 XS2 AS+MS+PS + 3D LUT: Vulkan 1.4 vs D3D12 MeshTier=10)"},
        {"test_rdna2_corpus_parity.exe", "RDNA2/GFX10.3 corpus parity (v_cmpx, f16/d16, NaN/denorm, PSV0 wave, mesh, DXIL MD5 re-sign)"},
        {"test_gpu_d3d12x_vk_parity.exe", "d3d12_xs R9G9B9E5 UAV fallback, CommandListX/QueueX, sret ABI, Vulkan sparse/mip & xg_xs metadata"},
        {"test_xgr_conformance_parity.exe", "xgameruntime 292/292 parity, 3-arg COM QueryApiImpl, headless XStore/PLS & XTaskQueue/XAsync stress"},
        {"test_dstorage_audio_diag_parity.exe", "dstorage_xs cancel/phys-page, mfplat_xs & 4 console diag DLLs, XMem/XThread & WASAPI MMDevice"},
        {"test_loader_child_hook_parity.exe", "24-DLL routing, hardened nested CoW G: VFS & Win32 parent->child APC hook propagation"},
        {"test_bcpack_bvh4.exe", "Hardened BCPack BC1-BC7 entropy codecs & RDNA2 BVH4 header/node-pointer decoder + SAH equivalence"},
        {"test_real_gdk_thunks_live.exe", "Live 292/292 xgameruntime + 16/16 XCurl + shipping commercial GDK thunks (zero E_NOINTERFACE)"},
        {"test_scarlett_game_live.exe", "Full playable Scarlett game stack via openscarlett run (d3d12_xs+PresentX+GameInput+XAudio2+S:)"},
        {"test_d3d12_vk_routing.exe", "Direct d3d12_xs->Vulkan 1.4 routing, BC1-BC7 staged copies, mixed-heap isolation & PresentX timing"},
        {"test_rdna2_lockstep_isa.exe", "Single-wave lockstep LDS barriers, 5 RDNA2 no-ops & CPU-vs-GPU differential ISA checks"},
        {"test_s7_pe_gdk_runtime.exe", "PE/KnownDLL/.xdata hardening, XMemVirtualQuery, Minecraft-GDK surface, 6 stubs, RawInput & DCC fuzz"},
        {"test_gdk_3d_scene_dual_api.exe", "3D shadowed/skinned/BC1+BC3 GDK scene launched via openscarlett run on D3D12 & Vulkan 1.4"},
        {"test_launcher_updater.exe", "Native Win32 launcher self-updater, SHA-256 verification, release parser & hwtest bridge"},
    };

    const auto it = kDescriptions.find(key);
    if (it != kDescriptions.end()) {
        return it->second;
    }
    return "OpenScarlett hardware/runtime test";
}

GpuTelemetrySnapshot ProbeHostGpuTelemetry(const std::filesystem::path& bundle_dir) {
    GpuTelemetrySnapshot snap{};
    snap.probed = true;
    snap.os_version = GetFriendlyWindowsOsVersion();
    snap.vc_runtime_info = GetVcRuntimeSummary(bundle_dir);

#if defined(_WIN32)
    const std::filesystem::path probe_exe = bundle_dir / L"openscarlett.exe";
    const std::wstring cmd = QuoteWin32ArgW(probe_exe.wstring()) + L" probe-gpu";
    const ProcessRunOutput proc = RunProcessCapture(
        probe_exe, cmd, bundle_dir, {}, {}, nullptr, 30000);

    snap.probe_ok = (proc.launched && proc.exit_code == 0 && !proc.timed_out);
    snap.raw_probe_output = SanitizeReportText(TrimTrailingWhitespace(proc.raw_output), bundle_dir);

    const auto lines = SplitLines(snap.raw_probe_output);
    for (const auto& raw_line : lines) {
        const std::string line = TrimAscii(raw_line);
        if (line.empty()) {
            continue;
        }

        // Parse: D3D12 [0]    : <Name> (Hardware|WARP) | VRAM=<N> MB | Wave=<min>..<max> | SM=0x<XX> | DXR=<N> | MeshTier=<N>
        if (StartsWithIgnoreCaseAscii(line, "D3D12")) {
            const size_t lb = line.find('[');
            const size_t rb = (lb != std::string::npos) ? line.find(']', lb + 1) : std::string::npos;
            const size_t colon = (rb != std::string::npos) ? line.find(':', rb + 1) : std::string::npos;
            if (lb != std::string::npos && rb != std::string::npos && colon != std::string::npos) {
                GpuAdapterEntry entry{};
                uint64_t idx_val = 0;
                if (ParseUint64Strict(std::string_view(line).substr(lb + 1, rb - lb - 1), idx_val)) {
                    entry.index = static_cast<uint32_t>(idx_val);
                }

                const std::string rhs = TrimAscii(std::string_view(line).substr(colon + 1));
                const size_t pipe = rhs.find('|');
                std::string name_part = (pipe == std::string::npos) ? rhs : TrimAscii(std::string_view(rhs).substr(0, pipe));
                entry.details = (pipe == std::string::npos) ? std::string{} : TrimAscii(std::string_view(rhs).substr(pipe + 1));

                if (EndsWithIgnoreCaseAscii(name_part, "(WARP)")) {
                    entry.is_warp = true;
                    name_part = TrimAscii(std::string_view(name_part).substr(0, name_part.size() - 6));
                } else if (EndsWithIgnoreCaseAscii(name_part, "(Hardware)")) {
                    entry.is_warp = false;
                    name_part = TrimAscii(std::string_view(name_part).substr(0, name_part.size() - 10));
                }
                entry.name = std::move(name_part);

                const auto fields = SplitChar(entry.details, '|');
                for (const auto& f : fields) {
                    const std::string tok = TrimAscii(f);
                    if (StartsWithIgnoreCaseAscii(tok, "VRAM=")) {
                        uint64_t v = 0;
                        if (ParseUint64Strict(std::string_view(tok).substr(5), v)) {
                            entry.vram_mb = v;
                        }
                    } else if (StartsWithIgnoreCaseAscii(tok, "Wave=")) {
                        const std::string wave_str = TrimAscii(std::string_view(tok).substr(5));
                        const size_t dots = wave_str.find("..");
                        if (dots != std::string::npos) {
                            uint64_t wmin = 0;
                            uint64_t wmax = 0;
                            if (ParseUint64Strict(std::string_view(wave_str).substr(0, dots), wmin)) {
                                entry.wave_min = static_cast<uint32_t>(wmin);
                            }
                            if (ParseUint64Strict(std::string_view(wave_str).substr(dots + 2), wmax)) {
                                entry.wave_max = static_cast<uint32_t>(wmax);
                            }
                        }
                    } else if (StartsWithIgnoreCaseAscii(tok, "SM=")) {
                        uint64_t sm = 0;
                        if (ParseUint64Strict(std::string_view(tok).substr(3), sm, 0)) {
                            entry.shader_model = static_cast<uint32_t>(sm);
                        }
                    } else if (StartsWithIgnoreCaseAscii(tok, "DXR=")) {
                        uint64_t dxr = 0;
                        if (ParseUint64Strict(std::string_view(tok).substr(4), dxr)) {
                            entry.dxr_tier = static_cast<uint32_t>(dxr);
                        }
                    } else if (StartsWithIgnoreCaseAscii(tok, "MeshTier=")) {
                        uint64_t mesh = 0;
                        if (ParseUint64Strict(std::string_view(tok).substr(9), mesh)) {
                            entry.mesh_tier = static_cast<uint32_t>(mesh);
                        }
                    }
                }
                snap.adapters.push_back(std::move(entry));
            }
            continue;
        }

        // Parse: Vulkan : <Name> | API <M.m.p> | ... | VK_EXT_mesh_shader=<val> | Win32Swapchain=<yes|no> | HDR10_CS=<yes|no>
        if (StartsWithIgnoreCaseAscii(line, "Vulkan")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string rhs = TrimAscii(std::string_view(line).substr(colon + 1));
                snap.vulkan_summary_line = rhs;
                if (!rhs.empty() && !StartsWithIgnoreCaseAscii(rhs, "unavailable")) {
                    snap.vulkan_available = true;
                    const auto parts = SplitChar(rhs, '|');
                    if (!parts.empty()) {
                        snap.vulkan_device_name = TrimAscii(parts[0]);
                    }
                    for (size_t pi = 1; pi < parts.size(); ++pi) {
                        const std::string seg = TrimAscii(parts[pi]);
                        if (StartsWithIgnoreCaseAscii(seg, "API ")) {
                            snap.vulkan_api_version = TrimAscii(std::string_view(seg).substr(4));
                        } else if (StartsWithIgnoreCaseAscii(seg, "VK_EXT_mesh_shader=")) {
                            snap.vulkan_mesh_shader = TrimAscii(std::string_view(seg).substr(19));
                        } else if (StartsWithIgnoreCaseAscii(seg, "Win32Swapchain=")) {
                            snap.vulkan_swapchain = EqualsIgnoreCaseAscii(
                                TrimAscii(std::string_view(seg).substr(15)), "yes");
                        } else if (StartsWithIgnoreCaseAscii(seg, "HDR10_CS=")) {
                            snap.vulkan_hdr10 = EqualsIgnoreCaseAscii(
                                TrimAscii(std::string_view(seg).substr(9)), "yes");
                        }
                    }
                }
            }
            continue;
        }

        // Parse: DXC (DXIL) : <path>
        if (StartsWithIgnoreCaseAscii(line, "DXC (DXIL)")) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                snap.dxc_path = TrimAscii(std::string_view(line).substr(colon + 1));
            }
            continue;
        }
    }

    // Select primary discrete/hardware GPU (highest VRAM among non-WARP adapters, falling back to WARP).
    const GpuAdapterEntry* primary = nullptr;
    for (const auto& a : snap.adapters) {
        if (a.is_warp) {
            continue;
        }
        if (primary == nullptr || a.vram_mb > primary->vram_mb) {
            primary = &a;
        }
    }
    if (primary == nullptr && !snap.adapters.empty()) {
        primary = &snap.adapters.front();
    }

    if (primary != nullptr) {
        snap.primary_gpu_name = primary->name;
        snap.primary_vram_mb = primary->vram_mb;
        snap.primary_shader_model = primary->shader_model;
        snap.primary_wave_min = primary->wave_min;
        snap.primary_wave_max = primary->wave_max;
        snap.primary_mesh_tier = primary->mesh_tier;
        snap.primary_dxr_tier = primary->dxr_tier;

        if (!primary->details.empty()) {
            snap.primary_gpu_summary = primary->name + " (" + primary->details + ")";
        } else {
            snap.primary_gpu_summary = primary->name;
        }
        if (!snap.vulkan_summary_line.empty()) {
            snap.primary_gpu_summary += " | Vulkan: " + snap.vulkan_summary_line;
        }
    } else {
        snap.primary_gpu_name = "D3D12 WARP / No Discrete GPU";
        if (!snap.vulkan_summary_line.empty()) {
            snap.primary_gpu_summary = "WARP | Vulkan: " + snap.vulkan_summary_line;
        } else {
            snap.primary_gpu_summary = "Unknown GPU";
        }
    }
#else
    (void)bundle_dir;
#endif

    return snap;
}

std::vector<BundledDemoEntry> DiscoverBundledDemos(const std::filesystem::path& bundle_dir) {
    struct DemoSpec {
        const char* id;
        const char* exe_name;
        const char* display_name;
        const char* subtitle;
        const char* named_config;
    };
    static constexpr std::array<DemoSpec, 3> kSpecs = {{
        {
            "scarlett_doom",
            "ScarlettDoom.exe",
            "ScarlettDoom (Freedoom: Phase 1)",
            "Playable Scarlett GDK Doom port (d3d12_xs + PresentX + GameInput + XAudio2)",
            "ScarlettDoom_MicrosoftGame.Config",
        },
        {
            "scarlett_arena",
            "ScarlettArena.exe",
            "ScarlettArena (MIT 3D Raycaster)",
            "MIT 3D raycaster running natively through OpenScarlett shims",
            "ScarlettArena_MicrosoftGame.Config",
        },
        {
            "gdk_3d_scene",
            "gdk_3d_scene.exe",
            "Scarlett 3D Scene (Shadowed/Skinned BC1+BC3)",
            "Dual-API D3D12 & Vulkan 1.4 shadowed/skinned 3D GDK scene with BC1/BC3 textures",
            "gdk_3d_scene_MicrosoftGame.Config",
        },
    }};

    std::vector<BundledDemoEntry> demos;
    demos.reserve(kSpecs.size());

    std::error_code ec;
    const std::filesystem::path demos_dir = bundle_dir / "demos";

    for (const auto& spec : kSpecs) {
        BundledDemoEntry entry{};
        entry.id = spec.id;
        entry.display_name = spec.display_name;
        entry.subtitle = spec.subtitle;

        const std::array<std::filesystem::path, 2> exe_candidates = {
            bundle_dir / spec.exe_name,
            demos_dir / spec.exe_name,
        };
        entry.exe_path = exe_candidates[0];
        for (const auto& cand : exe_candidates) {
            if (std::filesystem::is_regular_file(cand, ec)) {
                entry.exe_path = cand;
                entry.available = true;
                break;
            }
        }

        const std::filesystem::path exe_parent = entry.exe_path.parent_path();
        const std::array<std::filesystem::path, 4> cfg_candidates = {
            exe_parent / spec.named_config,
            bundle_dir / spec.named_config,
            demos_dir / spec.named_config,
            (exe_parent != bundle_dir) ? (exe_parent / "MicrosoftGame.Config")
                                       : std::filesystem::path{},
        };
        for (const auto& cfg : cfg_candidates) {
            if (!cfg.empty() && std::filesystem::is_regular_file(cfg, ec)) {
                entry.config_path = cfg;
                break;
            }
        }

        demos.push_back(std::move(entry));
    }

    return demos;
}

std::string BuildGitHubHardwareReportUrl(const GpuTelemetrySnapshot& probe,
                                         const HardwareTestRunResult& run,
                                         std::string* out_mode) {
    const std::string primary_gpu_short =
        probe.primary_gpu_name.empty() ? "Unknown GPU" : probe.primary_gpu_name;
    const std::string os_version =
        probe.os_version.empty() ? GetFriendlyWindowsOsVersion() : probe.os_version;
    const std::string gpu_summary =
        probe.primary_gpu_summary.empty() ? primary_gpu_short : probe.primary_gpu_summary;
    const std::string status_label = (run.failed_count == 0) ? "PASS" : "FAIL";

    const std::string issue_title =
        "[HW Report] " + primary_gpu_short + " | " + os_version + " | " +
        std::to_string(run.passed_count) + "/" + std::to_string(run.total_count) + " " + status_label;

    const std::string headline =
        "**" + std::to_string(run.passed_count) + "/" + std::to_string(run.total_count) +
        " passed** (" + std::to_string(run.failed_count) + " failed)";

    auto make_base_params = [&](const std::string& summary_body) {
        std::ostringstream oss;
        oss << "template=hardware-report.yml"
            << "&labels=hardware-report"
            << "&title=" << UriEscapeRfc3986(issue_title)
            << "&os_version=" << UriEscapeRfc3986(os_version)
            << "&gpu_summary=" << UriEscapeRfc3986(gpu_summary)
            << "&test_summary=" << UriEscapeRfc3986(summary_body);
        return oss.str();
    };

    const std::string full_table = BuildSummaryTableMarkdown(run.rows, true);
    const std::string params_base = make_base_params(headline + "\n\n" + full_table);

    // Build compact log matching tools/run_hardware_test.ps1
    std::ostringstream compact_oss;
    compact_oss << "=== Host Probe ===\n"
                << probe.raw_probe_output
                << "\n\n=== Failed / Key Test Lines ===\n";
    for (const auto& r : run.rows) {
        if (EqualsIgnoreCaseAscii(r.name, "openscarlett.exe probe-gpu")) {
            continue;
        }
        const std::string badge = r.passed
                                      ? "PASS"
                                      : ("FAIL (exit " + std::to_string(r.exit_code) + ")");
        if (!r.passed) {
            compact_oss << "[" << badge << "] " << r.name << "\n" << r.output << "\n\n";
        } else {
            compact_oss << "[" << badge << "] " << r.name << ": " << r.highlight << "\n";
        }
    }
    const std::string compact_log = TrimTrailingWhitespace(compact_oss.str());

    const std::string full_url =
        std::string(kGitHubIssuesNewUrl) + "?" + params_base +
        "&full_report=" + UriEscapeRfc3986(run.full_log_text);
    if (full_url.size() <= kGitHubUrlBudget) {
        if (out_mode != nullptr) *out_mode = "complete";
        return full_url;
    }

    const std::string compact_url =
        std::string(kGitHubIssuesNewUrl) + "?" + params_base +
        "&full_report=" + UriEscapeRfc3986(compact_log);
    if (compact_url.size() <= kGitHubUrlBudget) {
        if (out_mode != nullptr) *out_mode = "compact";
        return compact_url;
    }

    const std::string slim_table = BuildSummaryTableMarkdown(run.rows, false);
    const std::string slim_url_prefix =
        std::string(kGitHubIssuesNewUrl) + "?" + make_base_params(headline + "\n\n" + slim_table);
    const std::string min_url_prefix =
        std::string(kGitHubIssuesNewUrl) + "?" +
        make_base_params(headline + "\n\n(Full summary table is in your clipboard.)");

    // When tests failed and the full 42-test summary exceeds the URL budget, prioritize
    // embedding the failing tests' exact output directly in full_report so failures are
    // always visible in the GitHub issue even if the user forgets to press Ctrl+V.
    if (run.failed_count > 0) {
        std::ostringstream fail_oss;
        fail_oss << "=== Failed Tests ===\n";
        for (const auto& r : run.rows) {
            if (r.passed) {
                continue;
            }
            fail_oss << "[FAIL (exit " << r.exit_code << ")] " << r.name << "\n";
            const std::string body = TrimTrailingWhitespace(r.output);
            if (!body.empty()) {
                fail_oss << body << "\n\n";
            } else if (!r.highlight.empty()) {
                fail_oss << r.highlight << "\n\n";
            } else {
                fail_oss << "(no output captured)\n\n";
            }
        }
        fail_oss << "(Full raw test log is also in your clipboard - press Ctrl+V to append passing logs.)";
        const std::string failures_log = fail_oss.str();

        auto try_fit_failures = [&](const std::string& prefix, std::string& out_url) -> bool {
            const std::string exact = prefix + "&full_report=" + UriEscapeRfc3986(failures_log);
            if (exact.size() <= kGitHubUrlBudget) {
                out_url = exact;
                return true;
            }
            const std::string trunc_suffix =
                "\n... (truncated; full raw log is in your clipboard - press Ctrl+V to paste)";
            const std::string base_param = prefix + "&full_report=";
            const std::string encoded_suffix = UriEscapeRfc3986(trunc_suffix);
            if (base_param.size() + encoded_suffix.size() + 120u > kGitHubUrlBudget) {
                return false;
            }
            size_t lo = 0;
            size_t hi = failures_log.size();
            while (lo < hi) {
                const size_t mid = lo + (hi - lo + 1u) / 2u;
                const std::string cand =
                    base_param + UriEscapeRfc3986(std::string_view(failures_log.data(), mid)) + encoded_suffix;
                if (cand.size() <= kGitHubUrlBudget) {
                    lo = mid;
                } else {
                    hi = mid - 1u;
                }
            }
            if (lo < 40u) {
                return false;
            }
            // Prefer trimming cleanly at a newline boundary when close to the cap.
            size_t cut = lo;
            const size_t last_nl = failures_log.rfind('\n', lo - 1u);
            if (last_nl != std::string::npos && last_nl + 80u >= lo) {
                cut = last_nl;
            }
            out_url = base_param + UriEscapeRfc3986(std::string_view(failures_log.data(), cut)) + encoded_suffix;
            return out_url.size() <= kGitHubUrlBudget;
        };

        std::string fitted_url;
        if (try_fit_failures(slim_url_prefix, fitted_url) ||
            try_fit_failures(min_url_prefix, fitted_url)) {
            if (out_mode != nullptr) *out_mode = "compact";
            return fitted_url;
        }
    } else {
        const std::string slim_compact =
            slim_url_prefix + "&full_report=" + UriEscapeRfc3986(compact_log);
        if (slim_compact.size() <= kGitHubUrlBudget) {
            if (out_mode != nullptr) *out_mode = "compact";
            return slim_compact;
        }
    }

    const std::string fallback_note =
        "(Full raw test log is in your clipboard - press Ctrl+V here to paste it.)";
    const std::string encoded_fallback = "&full_report=" + UriEscapeRfc3986(fallback_note);

    std::string clipboard_url =
        std::string(kGitHubIssuesNewUrl) + "?" + params_base + encoded_fallback;
    if (clipboard_url.size() > kGitHubUrlBudget) {
        // Trim table notes if a very large suite pushes the summary table itself over the URL budget.
        clipboard_url = slim_url_prefix + encoded_fallback;
    }
    if (clipboard_url.size() > kGitHubUrlBudget) {
        clipboard_url = min_url_prefix + encoded_fallback;
    }

    if (out_mode != nullptr) *out_mode = "clipboard";
    return clipboard_url;
}

HardwareTestRunResult RunHardwareSelfTestSuite(
    const std::filesystem::path& bundle_dir,
    const GpuTelemetrySnapshot* cached_probe,
    const std::function<void(const HardwareTestProgress&)>& on_progress,
    const std::atomic<bool>* cancel_flag) {
    HardwareTestRunResult result{};

#if defined(_WIN32)
    GpuTelemetrySnapshot probe{};
    double probe_seconds = 0.0;
    if (cached_probe != nullptr && cached_probe->probed) {
        probe = *cached_probe;
    } else {
        LARGE_INTEGER freq{};
        LARGE_INTEGER t0{};
        LARGE_INTEGER t1{};
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&t0);
        probe = ProbeHostGpuTelemetry(bundle_dir);
        ::QueryPerformanceCounter(&t1);
        probe_seconds = ElapsedSecondsFromQpc(t0, t1, freq);
    }

    // Enumerate all test_*.exe in bundle_dir (case-insensitive, excluding test_guest_dummy.exe).
    std::vector<std::filesystem::path> test_files;
    std::error_code ec;
    if (std::filesystem::is_directory(bundle_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(bundle_dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string fname = PathToUtf8(entry.path().filename());
            if (!StartsWithIgnoreCaseAscii(fname, "test_") ||
                !EndsWithIgnoreCaseAscii(fname, ".exe") ||
                EqualsIgnoreCaseAscii(fname, "test_guest_dummy.exe")) {
                continue;
            }
            test_files.push_back(entry.path());
        }
    }
    std::sort(test_files.begin(), test_files.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return ToLowerAscii(PathToUtf8(a.filename())) <
                         ToLowerAscii(PathToUtf8(b.filename()));
              });

    const int total_checks = 1 + static_cast<int>(test_files.size());

    // Row 0: openscarlett.exe probe-gpu
    HardwareTestRow probe_row{};
    probe_row.name = "openscarlett.exe probe-gpu";
    probe_row.description = GetHardwareTestDescription(probe_row.name);
    probe_row.passed = probe.probe_ok;
    probe_row.exit_code = probe.probe_ok ? 0 : 1;
    probe_row.seconds = std::round(probe_seconds * 100.0) / 100.0;
    probe_row.highlight = probe.primary_gpu_name;
    probe_row.output = probe.raw_probe_output;
    result.rows.push_back(probe_row);

    if (on_progress) {
        HardwareTestProgress prog{};
        prog.completed = 1;
        prog.total = total_checks;
        prog.current_test_name = probe_row.name;
        prog.has_completed_row = true;
        prog.last_row = probe_row;
        on_progress(prog);
    }

    // Isolated save root under %TEMP%\openscarlett_hw_test_saves + null audio for child test processes only.
    std::filesystem::path temp_dir;
    const std::string temp_env = GetEnvVarUtf8(L"TEMP");
    if (!temp_env.empty()) {
        temp_dir = std::filesystem::path(Utf8ToWide(temp_env));
    } else {
        temp_dir = std::filesystem::temp_directory_path(ec);
    }
    const std::filesystem::path save_root = temp_dir / L"openscarlett_hw_test_saves";
    std::filesystem::create_directories(save_root, ec);

    const std::vector<std::pair<std::wstring, std::wstring>> child_env = {
        {L"OPENSCARLETT_SAVE_ROOT", save_root.wstring()},
        {L"OPENSCARLETT_AUDIO_NULL", L"1"},
    };

    int completed_count = 1;
    for (const auto& tf : test_files) {
        if (cancel_flag != nullptr && cancel_flag->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            break;
        }

        const std::string tname = PathToUtf8(tf.filename());
        const std::string desc = GetHardwareTestDescription(tname);

        if (on_progress) {
            HardwareTestProgress start_prog{};
            start_prog.completed = completed_count;
            start_prog.total = total_checks;
            start_prog.current_test_name = tname;
            start_prog.has_completed_row = false;
            on_progress(start_prog);
        }

        const std::wstring cmd = QuoteWin32ArgW(tf.wstring());
        const auto line_cb = [&](const std::string& raw_line) {
            if (!on_progress) {
                return;
            }
            HardwareTestProgress lp{};
            lp.completed = completed_count;
            lp.total = total_checks;
            lp.current_test_name = tname;
            lp.has_completed_row = false;
            lp.log_line = SanitizeReportText(raw_line, bundle_dir);
            on_progress(lp);
        };

        const ProcessRunOutput proc = RunProcessCapture(
            tf, cmd, bundle_dir, child_env, line_cb, cancel_flag, 0);

        if (proc.cancelled) {
            result.cancelled = true;
            break;
        }

        const std::string clean_out =
            SanitizeReportText(TrimTrailingWhitespace(proc.raw_output), bundle_dir);

        // Extract the last [PASS] or [FAIL] highlight line (clamped to 110 chars).
        std::string highlight;
        const auto lines = SplitLines(proc.raw_output);
        std::string last_non_empty;
        for (const auto& l : lines) {
            const std::string trimmed = TrimAscii(l);
            if (!trimmed.empty()) {
                last_non_empty = trimmed;
            }
            if (l.find("[PASS]") != std::string::npos || l.find("[FAIL]") != std::string::npos) {
                highlight = trimmed;
            }
        }
        if (highlight.empty()) {
            highlight = last_non_empty;
        }
        highlight = SanitizeReportText(highlight, bundle_dir);
        if (highlight.size() > 110) {
            highlight = highlight.substr(0, 107) + "...";
        }

        HardwareTestRow row{};
        row.name = tname;
        row.description = desc;
        row.passed = (proc.launched && proc.exit_code == 0 && !proc.timed_out);
        row.exit_code = proc.exit_code;
        row.seconds = std::round(proc.elapsed_seconds * 100.0) / 100.0;
        row.highlight = std::move(highlight);
        row.output = clean_out;
        result.rows.push_back(row);
        ++completed_count;

        if (on_progress) {
            HardwareTestProgress done_prog{};
            done_prog.completed = completed_count;
            done_prog.total = total_checks;
            done_prog.current_test_name = tname;
            done_prog.has_completed_row = true;
            done_prog.last_row = row;
            on_progress(done_prog);
        }
    }

    result.total_count = static_cast<int>(result.rows.size());
    for (const auto& r : result.rows) {
        if (r.passed) {
            ++result.passed_count;
        } else {
            ++result.failed_count;
        }
    }
    result.completed = !result.cancelled;

    // Build exact GitHub-ready Markdown report matching tools/run_hardware_test.ps1
    const std::string summary_table_md = BuildSummaryTableMarkdown(result.rows, true);

    std::ostringstream full_log_oss;
    for (const auto& r : result.rows) {
        const std::string badge = r.passed
                                      ? "PASS"
                                      : ("FAIL (exit " + std::to_string(r.exit_code) + ")");
        full_log_oss << "=== " << r.name << " [" << badge << ", " << FormatSeconds(r.seconds) << "s] ===\n";
        if (!TrimAscii(r.output).empty()) {
            full_log_oss << r.output << "\n";
        }
        full_log_oss << "\n";
    }
    result.full_log_text = TrimTrailingWhitespace(full_log_oss.str());

    std::ostringstream md;
    md << "# OpenScarlett Hardware Test Report\n\n"
       << "- **Result:** **" << result.passed_count << "/" << result.total_count << " passed** ("
       << result.failed_count << " failed)\n"
       << "- **OS:** " << probe.os_version << "\n"
       << "- **VC++ Runtime:** " << probe.vc_runtime_info << "\n"
       << "- **Primary GPU:** " << probe.primary_gpu_summary << "\n"
       << "- **UTC Timestamp:** " << CurrentUtcTimestampString() << " UTC\n\n"
       << "## Test Summary\n\n"
       << summary_table_md << "\n\n"
       << "## Host GPU Probe\n\n"
       << "```text\n"
       << probe.raw_probe_output << "\n"
       << "```\n\n"
       << "<details>\n"
       << "<summary>Full Automated Test Output</summary>\n\n"
       << "```text\n"
       << result.full_log_text << "\n"
       << "```\n\n"
       << "</details>\n";

    result.report_markdown = md.str();
    result.report_path = bundle_dir / L"hardware_test_report.md";
    {
        std::ofstream ofs(result.report_path, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(result.report_markdown.data(),
                      static_cast<std::streamsize>(result.report_markdown.size()));
        }
    }

    result.github_issue_url = BuildGitHubHardwareReportUrl(probe, result, &result.github_url_mode);
#else
    (void)bundle_dir;
    (void)cached_probe;
    (void)on_progress;
    (void)cancel_flag;
#endif

    return result;
}

bool CopyTextToClipboard(const std::string& utf8_text) {
#if defined(_WIN32)
    const std::wstring wide = Utf8ToWide(utf8_text);

    bool opened = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (::OpenClipboard(nullptr) != FALSE) {
            opened = true;
            break;
        }
        ::Sleep(10);
    }
    if (!opened) {
        return false;
    }

    if (::EmptyClipboard() == FALSE) {
        ::CloseClipboard();
        return false;
    }

    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hmem == nullptr) {
        ::CloseClipboard();
        return false;
    }

    void* locked = ::GlobalLock(hmem);
    if (locked == nullptr) {
        ::GlobalFree(hmem);
        ::CloseClipboard();
        return false;
    }
    std::memcpy(locked, wide.c_str(), bytes);
    ::GlobalUnlock(hmem);

    if (::SetClipboardData(CF_UNICODETEXT, hmem) == nullptr) {
        ::GlobalFree(hmem);
        ::CloseClipboard();
        return false;
    }

    ::CloseClipboard();
    return true;
#else
    (void)utf8_text;
    return false;
#endif
}

bool OpenUrlInDefaultBrowser(const std::string& url) {
#if defined(_WIN32)
    if (url.empty()) {
        return false;
    }
    const std::wstring wide_url = Utf8ToWide(url);
    const HINSTANCE rc = ::ShellExecuteW(
        nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
#else
    (void)url;
    return false;
#endif
}

LauncherRunResult LaunchTargetWithProbeOption(
    const std::filesystem::path& bundle_dir,
    const LauncherRunRequest& req,
    bool probe_unknown_imports,
    const std::function<void(const std::string&)>& on_log_line,
    const std::atomic<bool>* cancel_flag) {
    LauncherRunResult result{};

#if defined(_WIN32)
    if (req.target_path.empty()) {
        result.error_message = "No target executable, package directory, or MicrosoftGame.Config specified.";
        return result;
    }

    const std::filesystem::path cli_exe = bundle_dir / L"openscarlett.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(cli_exe, ec)) {
        result.error_message = "openscarlett.exe not found in " + PathToUtf8(bundle_dir);
        return result;
    }

    const std::filesystem::path shim_dir = req.shim_dir.empty() ? bundle_dir : req.shim_dir;

    // Build an isolated telemetry output path under %TEMP% (never hardcoded to C:\).
    std::filesystem::path temp_dir;
    const std::string temp_env = GetEnvVarUtf8(L"TEMP");
    if (!temp_env.empty()) {
        temp_dir = std::filesystem::path(Utf8ToWide(temp_env));
    } else {
        temp_dir = std::filesystem::temp_directory_path(ec);
    }
    const std::filesystem::path tel_dir = temp_dir / L"openscarlett_launcher_telemetry";
    std::filesystem::create_directories(tel_dir, ec);

    const std::filesystem::path target_fs(Utf8ToWide(req.target_path));
    std::string stem = PathToUtf8(target_fs.stem());
    if (stem.empty()) {
        stem = "guest";
    }
    const std::string tel_filename =
        stem + "_" + std::to_string(::GetCurrentProcessId()) + "_" +
        std::to_string(static_cast<unsigned long long>(::GetTickCount64())) + "_telemetry.json";
    result.telemetry_path = tel_dir / Utf8ToWide(tel_filename);

    // Assemble command line: openscarlett.exe run <target> --shim-dir <dir> --telemetry <json> [--backend=...] [--probe-unknown-imports] [--timeout-ms N] [-- <extra_args>]
    std::wstring cmd = QuoteWin32ArgW(cli_exe.wstring());
    cmd += L" run ";
    cmd += QuoteWin32ArgW(Utf8ToWide(req.target_path));
    cmd += L" --shim-dir ";
    cmd += QuoteWin32ArgW(shim_dir.wstring());
    cmd += L" --telemetry ";
    cmd += QuoteWin32ArgW(result.telemetry_path.wstring());

    if (req.backend == LaunchBackendChoice::D3D12) {
        cmd += L" --backend=d3d12";
    } else if (req.backend == LaunchBackendChoice::Vulkan) {
        cmd += L" --backend=vulkan";
    }

    if (probe_unknown_imports) {
        cmd += L" --probe-unknown-imports";
    }

    if (req.timeout_ms > 0) {
        cmd += L" --timeout-ms ";
        cmd += std::to_wstring(req.timeout_ms);
    }

    const std::string trimmed_extra = TrimAscii(req.extra_args);
    if (!trimmed_extra.empty()) {
        cmd += L" -- ";
        cmd += Utf8ToWide(trimmed_extra);
    }

    // Per-launch child environment overrides (parent process env stays completely untouched).
    std::vector<std::pair<std::wstring, std::wstring>> env_overrides;
    if (!req.adapter_substring.empty()) {
        env_overrides.emplace_back(L"OPENSCARLETT_VK_DEVICE", Utf8ToWide(req.adapter_substring));
    }
    if (req.prefer_warp) {
        env_overrides.emplace_back(L"OPENSCARLETT_PREFER_WARP", L"1");
    }
    if (req.null_audio) {
        env_overrides.emplace_back(L"OPENSCARLETT_AUDIO_NULL", L"1");
    }
    if (probe_unknown_imports) {
        env_overrides.emplace_back(L"OPENSCARLETT_PROBE_UNKNOWN_IMPORTS", L"1");
    }

    // Allow openscarlett.exe run a brief grace period to flush telemetry if it hits --timeout-ms itself.
    const uint32_t outer_timeout_ms =
        (req.timeout_ms > 0) ? (req.timeout_ms + 3000u) : 0u;

    const ProcessRunOutput proc = RunProcessCapture(
        cli_exe, cmd, bundle_dir, env_overrides, on_log_line, cancel_flag, outer_timeout_ms);

    result.launched = proc.launched;
    result.exit_code = proc.exit_code;
    result.timed_out = proc.timed_out || (proc.exit_code == 124) ||
                       (proc.raw_output.find("(TIMED OUT)") != std::string::npos);
    result.log_output = proc.raw_output;
    result.error_message = proc.error_message;

    // Parse unified CallTelemetry JSON emitted by openscarlett.exe run.
    if (std::filesystem::is_regular_file(result.telemetry_path, ec)) {
        std::ifstream ifs(result.telemetry_path);
        std::string line;
        while (std::getline(ifs, line)) {
            std::string mod;
            std::string sym;
            if (!ExtractJsonStringField(line, "module", mod) ||
                !ExtractJsonStringField(line, "symbol", sym)) {
                continue;
            }
            ++result.tracked_symbols;
            uint64_t total = 0;
            uint64_t not_impl = 0;
            if (ExtractJsonUintField(line, "total", total)) {
                result.total_calls += total;
            }
            if (ExtractJsonUintField(line, "not_impl", not_impl)) {
                result.not_impl_calls += not_impl;
            }
        }
    }
#else
    (void)bundle_dir;
    (void)req;
    (void)probe_unknown_imports;
    (void)on_log_line;
    (void)cancel_flag;
    result.error_message = "LaunchTargetWithOpenScarlett is only supported on Windows.";
#endif

    return result;
}

LauncherRunResult LaunchTargetWithOpenScarlett(
    const std::filesystem::path& bundle_dir,
    const LauncherRunRequest& req,
    const std::function<void(const std::string&)>& on_log_line,
    const std::atomic<bool>* cancel_flag) {
    return LaunchTargetWithProbeOption(bundle_dir, req, false, on_log_line, cancel_flag);
}

bool BuildSanitizedLatestTelemetryForClipboard(
    const std::filesystem::path& bundle_dir,
    const std::filesystem::path& preferred_telemetry_path,
    const std::string& fallback_launch_log,
    std::string& out_sanitized_text,
    std::string& out_source_label) {
    out_sanitized_text.clear();
    out_source_label.clear();

    std::error_code ec;
    std::filesystem::path chosen_path;

    if (!preferred_telemetry_path.empty() &&
        std::filesystem::is_regular_file(preferred_telemetry_path, ec)) {
        chosen_path = preferred_telemetry_path;
    }

    if (chosen_path.empty()) {
        std::vector<std::filesystem::path> search_dirs;
#if defined(_WIN32)
        const std::string temp_env = GetEnvVarUtf8(L"TEMP");
        std::filesystem::path temp_dir =
            !temp_env.empty()
                ? std::filesystem::path(Utf8ToWide(temp_env))
                : std::filesystem::temp_directory_path(ec);
        search_dirs.push_back(temp_dir / L"openscarlett_launcher_telemetry");
        search_dirs.push_back(temp_dir / L"openscarlett_telemetry");
#else
        search_dirs.push_back(std::filesystem::temp_directory_path(ec) / "openscarlett_launcher_telemetry");
#endif
        if (!bundle_dir.empty()) {
            search_dirs.push_back(bundle_dir);
            search_dirs.push_back(bundle_dir / "telemetry");
        }

        std::filesystem::file_time_type best_time{};
        bool found_any = false;
        for (const auto& dir : search_dirs) {
            if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) {
                    continue;
                }
#if defined(_WIN32)
                const std::string fname = PathToUtf8(entry.path().filename());
#else
                const std::string fname = entry.path().filename().string();
#endif
                const std::string lower = ToLowerAscii(fname);
                if (!EndsWithIgnoreCaseAscii(lower, ".json")) {
                    continue;
                }
                if (lower.find("telemetry") == std::string::npos &&
                    lower.find("bringup") == std::string::npos &&
                    lower.find("veh") == std::string::npos) {
                    continue;
                }
                const auto wt = entry.last_write_time(ec);
                if (!ec && (!found_any || wt > best_time)) {
                    best_time = wt;
                    chosen_path = entry.path();
                    found_any = true;
                }
            }
        }
    }

    if (!chosen_path.empty()) {
        std::ifstream ifs(chosen_path, std::ios::binary);
        if (ifs) {
            std::ostringstream buf;
            buf << ifs.rdbuf();
            const std::string raw = buf.str();
            if (!raw.empty()) {
                out_sanitized_text = SanitizeReportText(raw, bundle_dir);
#if defined(_WIN32)
                out_source_label = PathToUtf8(chosen_path.filename());
#else
                out_source_label = chosen_path.filename().string();
#endif
                return true;
            }
        }
    }

    // Fallback: emit sanitized launcher log wrapped in JSON so clipboard always has valid sanitized data.
    const std::string clean_log = SanitizeReportText(fallback_launch_log, bundle_dir);
    std::string escaped;
    escaped.reserve(clean_log.size() + 32);
    for (char ch : clean_log) {
        switch (ch) {
            case '"':  escaped.append("\\\""); break;
            case '\\': escaped.append("\\\\"); break;
            case '\n': escaped.append("\\n"); break;
            case '\r': break;
            case '\t': escaped.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(ch) >= 0x20u) {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    out_sanitized_text =
        std::string("{\n  \"schema\": \"openscarlett.launcher_log_telemetry.v1\",\n  \"os\": \"") +
        GetFriendlyWindowsOsVersion() + "\",\n  \"log\": \"" + escaped + "\"\n}\n";
    out_source_label = "launcher_log";
    return true;
}

PeInspectorSummary InspectBinaryForLauncher(const std::filesystem::path& pe_path,
                                            const std::filesystem::path& shim_dir) {
    PeInspectorSummary summary{};
#if defined(_WIN32)
    summary.file_path = PathToUtf8(pe_path);
#else
    summary.file_path = pe_path.string();
#endif

    openscarlett::PeInspectionReport rep{};
    const openscarlett::Status st = openscarlett::InspectPeFile(summary.file_path, rep);
    if (!st.ok()) {
        summary.ok = false;
        summary.error_message = st.message;
        return summary;
    }

    summary.ok = true;
    summary.is_dll = rep.is_dll;
    summary.image_base = rep.image_base;
    summary.image_size = rep.image_size;
    summary.section_count = rep.section_count;
    summary.has_relocations = rep.has_relocations;
    summary.has_tls = rep.has_tls;
    summary.tls_callbacks = rep.tls_callback_rvas.size();
    summary.pdata_entries = rep.exception_entry_count;
    summary.total_exports = rep.exports.size();
    for (const auto& exp : rep.exports) {
        if (exp.is_forwarded) {
            ++summary.forwarded_exports;
        }
    }
    summary.total_imports = rep.total_imports;
    summary.console_imports = rep.console_imports;
    summary.gdk_imports = rep.gdk_imports;
    summary.delay_load_imports = rep.delay_load_imports;

    const openscarlett::DllRoutingTable routing =
        openscarlett::DllRoutingTable::CreateWithEnvOverrides();
    const bool xbox_context = (rep.console_imports > 0 || rep.gdk_imports > 0);
    const std::wstring shim_dir_w = shim_dir.wstring();
    const std::wstring guest_dir_w = pe_path.parent_path().wstring();

    struct CachedShimExports {
        bool inspected_ok = false;
        std::unordered_set<std::string> names;
    };
    std::unordered_map<std::string, CachedShimExports> shim_cache;

    auto get_shim_exports = [&](const std::wstring& resolved_w) -> const CachedShimExports& {
#if defined(_WIN32)
        const std::string resolved_u8 = WideToUtf8(resolved_w);
#else
        const std::string resolved_u8(resolved_w.begin(), resolved_w.end());
#endif
        const std::string key = ToLowerAscii(resolved_u8);
        auto it = shim_cache.find(key);
        if (it != shim_cache.end()) {
            return it->second;
        }
        CachedShimExports cached{};
        openscarlett::PeInspectionReport shim_rep{};
        if (openscarlett::InspectPeFile(resolved_u8, shim_rep).ok()) {
            cached.inspected_ok = true;
            cached.names.reserve(shim_rep.exports.size());
            for (const auto& exp : shim_rep.exports) {
                if (!exp.name.empty()) {
                    cached.names.insert(exp.name);
                }
            }
        }
        auto [inserted, _] = shim_cache.emplace(key, std::move(cached));
        return inserted->second;
    };

    summary.modules.reserve(rep.imports.size());
    for (const auto& mod : rep.imports) {
        PeInspectorModuleRow row{};
        row.dll_name = mod.dll_name;
        row.category = openscarlett::DllCategoryName(mod.category);
        row.import_count = mod.symbols.size();
        row.symbols.reserve(mod.symbols.size());
        for (const auto& sym : mod.symbols) {
            if (sym.by_ordinal || sym.name.empty()) {
                row.symbols.push_back("#" + std::to_string(sym.ordinal));
            } else {
                row.symbols.push_back(sym.name);
            }
        }

        const openscarlett::DllRouteDecision decision =
            routing.ResolveRoute(mod.dll_name, shim_dir_w, guest_dir_w, xbox_context);
        const bool is_known = openscarlett::IsWindowsKnownDll(mod.dll_name);

        if (decision.policy == openscarlett::DllRoutePolicy::Shim) {
            row.routing_status = "OpenScarlett Shim (" + decision.canonical_name + ")";
            row.shim_present = !decision.resolved_path.empty();
            if (row.shim_present) {
                const auto& shim_exports = get_shim_exports(decision.resolved_path);
                if (shim_exports.inspected_ok) {
                    for (const auto& sym : mod.symbols) {
                        if (sym.by_ordinal || sym.name.empty()) {
                            continue;
                        }
                        if (shim_exports.names.find(sym.name) == shim_exports.names.end()) {
                            row.missing_exports.push_back(sym.name);
                            ++summary.missing_export_count;
                        }
                    }
                }
            }
        } else if (is_known) {
            row.routing_status = "System32 KnownDLL";
            row.shim_present = true;
        } else if (decision.policy == openscarlett::DllRoutePolicy::NativeGuestDll) {
            row.routing_status = "Native Guest DLL";
            row.shim_present = !decision.resolved_path.empty();
        } else if (mod.category == openscarlett::DllCategory::ConsoleExclusive ||
                   mod.category == openscarlett::DllCategory::GdkRuntime) {
            row.routing_status = "Unmapped Console/GDK DLL";
            row.shim_present = false;
        } else {
            row.routing_status = "Host Windows DLL";
            row.shim_present = true;
        }

        summary.modules.push_back(std::move(row));
    }

    return summary;
}

}  // namespace openscarlett::launcher
