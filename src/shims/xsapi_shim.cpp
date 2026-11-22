#define OPENSCARLETT_BUILDING_XSAPI 1
#include "openscarlett/xsapi/xsapi_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Handle Backing Types (opaque in xsapi_abi.h)
// ---------------------------------------------------------------------------
struct XblContext {
    std::atomic<int32_t> ref_count{1};
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    std::string scid;
    uint32_t http_retry_delay_sec = 2;
    uint32_t http_timeout_window_sec = 20;
    uint32_t long_http_timeout_sec = 300;
    bool use_cross_platform_qos = false;
    uint32_t websocket_timeout_window_sec = 300;
};

struct XblAchievementsResult {
    struct EntryStorage {
        std::string id;
        std::string scid;
        std::string name;
        std::string unlocked_desc;
        std::string locked_desc;
        std::string current_progress_str;
        std::string target_progress_str;
    };

    std::atomic<int32_t> ref_count{1};
    std::vector<EntryStorage> storage;
    std::vector<XblAchievementRequirement> requirements;
    std::vector<XblAchievement> items;
    bool has_next = false;

    // Pagination continuation parameters
    std::string query_scid;
    uint64_t query_xuid = 0;
    uint32_t query_title_id = 0;
    uint32_t query_type = XblAchievementType_All;
    bool query_unlocked_only = false;
    uint32_t query_order_by = XblAchievementOrderBy_DefaultOrder;
    uint32_t next_skip = 0;
};

struct XblPresenceRecord {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0;
    XblPresenceUserState user_state = XblPresenceUserState_Offline;
    std::string title_name;
    std::string rich_presence_string;
    std::vector<XblPresenceTitleRecord> title_records;
    std::vector<XblPresenceDeviceRecord> device_records;
};

struct XblSocialRelationshipResult {
    std::atomic<int32_t> ref_count{1};
    std::vector<XblSocialRelationship> items;
    size_t total_count = 0;
    bool has_next = false;
};

struct XblTitleStorageBlobMetadataResult {
    std::atomic<int32_t> ref_count{1};
    std::vector<XblTitleStorageBlobMetadata> items;
    bool has_next = false;

    std::string query_scid;
    XblTitleStorageType query_storage_type = XblTitleStorageType_TrustedPlatformStorage;
    std::string query_blob_path;
    uint64_t query_xuid = 0;
    uint32_t next_skip = 0;
};

struct XblHttpCall {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0;
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::vector<uint8_t> request_body;
    bool retry_allowed = true;
    uint32_t retry_cache_id = 0;
    bool long_http_call = false;
    bool trace_call = true;

    uint32_t status_code = 0;
    XGameResult network_error_code = XBL_S_OK;
    uint32_t platform_network_error_code = 0;
    std::string platform_network_error_message;
    std::vector<std::pair<std::string, std::string>> response_headers;
    std::vector<uint8_t> response_body;
    std::string response_string;
};

struct XblRealTimeActivitySubscription {
    uint32_t id = 0;
    std::string kind;
    uint64_t xuid = 0;
    uint32_t title_id = 0;
    std::string scid;
    std::string stat_name;
};

struct XblSocialManagerUserGroup {
    XUserHandle local_user = nullptr;
    uint64_t local_xuid = 0;
    XblSocialUserGroupType type = XblSocialUserGroupType_FilterType;
    XblPresenceFilter presence_filter = XblPresenceFilter_All;
    XblRelationshipFilter relationship_filter = XblRelationshipFilter_Friends;
    std::vector<uint64_t> tracked_xuids;
    std::vector<XblSocialManagerUser> users_storage;
    std::vector<const XblSocialManagerUser*> user_ptrs;
};

namespace {

constexpr const char* kModName = "Microsoft.Xbox.Services.GDK.C.dll";

inline XGameResult RecordHr(const char* sym, XGameResult hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

// Canonical GDK / Xbox Live offline auth / no-token HRESULT codes.
constexpr XGameResult kHrGameUserSignedOut = static_cast<XGameResult>(0x89245101u);
constexpr XGameResult kHrGameUserResolveUserIssueRequired = static_cast<XGameResult>(0x89245102u);
constexpr XGameResult kHrGameUserDeferralNotAvailable = static_cast<XGameResult>(0x89245103u);
constexpr XGameResult kHrGameUserNoTokenRequired = static_cast<XGameResult>(0x89245105u);
constexpr XGameResult kHrXblAuthNoToken = static_cast<XGameResult>(0x89235200u);

std::string GetEnvString(const char* name) {
#if defined(_WIN32)
    if (!name || !*name) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    const DWORD written = GetEnvironmentVariableA(name, out.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    out.resize(static_cast<size_t>(written));
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

bool IsMockXstsEnabled() {
    std::string v = GetEnvString("OPENSCARLETT_MOCK_XSTS");
    if (v.empty()) {
        return false;
    }
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool IsXboxLiveAuthEndpointUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    std::string lo = url;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lo.find("xsts.auth.xboxlive.com") != std::string::npos ||
           lo.find("user.auth.xboxlive.com") != std::string::npos ||
           lo.find("device.auth.xboxlive.com") != std::string::npos ||
           lo.find("title.auth.xboxlive.com") != std::string::npos ||
           lo.find("sisu.xboxlive.com") != std::string::npos ||
           lo.find("/xsts/authorize") != std::string::npos ||
           lo.find("/user/authenticate") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Internal Persistent Data Models
// ---------------------------------------------------------------------------
struct StoredAchievement {
    std::string id;
    std::string scid;
    std::string name;
    std::string unlocked_desc;
    std::string locked_desc;
    uint32_t title_id = 0;
    uint32_t percent_complete = 0;
    int64_t time_unlocked = 0;
    uint32_t gamerscore = 25;
    XblAchievementType type = XblAchievementType_Persistent;
    bool is_secret = false;
};

struct StoredStat {
    std::string name;
    std::string type;
    std::string value;
};

struct StoredPresence {
    uint64_t xuid = 0;
    XblPresenceUserState user_state = XblPresenceUserState_Online;
    uint32_t title_id = 0;
    std::string scid;
    std::string title_name = "OpenScarlett Title";
    std::string rich_presence;
    int64_t last_modified = 0;
    bool title_active = false;
};

struct StoreKey {
    std::string scid;
    uint64_t xuid = 0;

    bool operator<(const StoreKey& o) const {
        if (scid != o.scid) return scid < o.scid;
        return xuid < o.xuid;
    }
};

struct TitleStorageKey {
    std::string scid;
    uint64_t xuid = 0;
    uint32_t storage_type = 0;
    std::string blob_path;

    bool operator<(const TitleStorageKey& o) const {
        if (scid != o.scid) return scid < o.scid;
        if (xuid != o.xuid) return xuid < o.xuid;
        if (storage_type != o.storage_type) return storage_type < o.storage_type;
        return blob_path < o.blob_path;
    }
};

struct StoredBlob {
    XblTitleStorageBlobMetadata metadata{};
    std::vector<uint8_t> data;
};

struct SocialManagerLocalUser {
    XUserHandle user = nullptr;
    uint64_t xuid = 0;
    XblSocialManagerExtraDetailLevel extra_detail = XblSocialManagerExtraDetailLevel_NoExtraDetail;
    bool rich_presence_polling = false;
};

struct XsapiState {
    std::mutex mutex;
    bool initialized = false;
    std::string scid;
    std::string override_locale;
    bool disable_throttling_asserts = false;
    XTaskQueueHandle default_queue = nullptr;
    XblMemAllocFunction mem_alloc = nullptr;
    XblMemFreeFunction mem_free = nullptr;

    XblFunctionContext next_handler_token = 1;
    uint32_t next_sub_id = 1;
    uint64_t next_etag_seq = 1;

    std::map<XblFunctionContext, std::pair<XblCallRoutedHandler, void*>> call_routed_handlers;
    std::map<XblFunctionContext, std::pair<XblSocialRelationshipChangedHandler, void*>> social_changed_handlers;
    std::map<XblFunctionContext, std::pair<XblStatisticChangedHandler, void*>> stat_changed_handlers;
    std::map<XblFunctionContext, std::pair<XblPresenceDevicePresenceChangedHandler, void*>> device_presence_handlers;
    std::map<XblFunctionContext, std::pair<XblPresenceTitlePresenceChangedHandler, void*>> title_presence_handlers;

    std::unordered_map<XblRealTimeActivitySubscription*, std::unique_ptr<XblRealTimeActivitySubscription>> live_subscriptions;

    std::unordered_map<XblContext*, std::shared_ptr<XblContext>> live_contexts;
    std::unordered_map<XblAchievementsResult*, std::shared_ptr<XblAchievementsResult>> live_ach_results;
    std::unordered_map<XblPresenceRecord*, std::shared_ptr<XblPresenceRecord>> live_presence_records;
    std::unordered_map<XblSocialRelationshipResult*, std::shared_ptr<XblSocialRelationshipResult>> live_social_results;
    std::unordered_map<XblTitleStorageBlobMetadataResult*, std::shared_ptr<XblTitleStorageBlobMetadataResult>> live_ts_results;
    std::unordered_map<XblHttpCall*, std::shared_ptr<XblHttpCall>> live_http_calls;

    std::map<StoreKey, std::map<std::string, StoredAchievement>> achievements_cache;
    std::map<StoreKey, bool> achievements_loaded;

    std::map<StoreKey, std::map<std::string, StoredStat>> stats_cache;
    std::map<StoreKey, bool> stats_loaded;

    std::map<TitleStorageKey, StoredBlob> title_storage_blobs;

    std::unordered_map<uint64_t, StoredPresence> presence_by_xuid;
    std::vector<std::string> recorded_in_game_events;

    // Social Manager state
    std::vector<SocialManagerLocalUser> sm_local_users;
    std::unordered_map<XblSocialManagerUserGroup*, std::unique_ptr<XblSocialManagerUserGroup>> sm_groups;
    std::vector<XblSocialManagerEvent> sm_pending_events;
    std::vector<XblSocialManagerEvent> sm_active_events;
};

XsapiState& GetState() {
    static XsapiState* s = new XsapiState();
    return *s;
}

int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string SanitizePathToken(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "default" : out;
}

std::string FormatXuidHex(uint64_t xuid) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(xuid));
    return std::string(buf);
}

fs::path ResolveXsapiDir(const std::string& scid) {
    const std::string env_root = GetEnvString("OPENSCARLETT_SAVE_ROOT");
    fs::path base;
    if (!env_root.empty()) {
        base = fs::path(env_root);
    } else {
        const std::string env_temp = GetEnvString("TEMP");
        if (!env_temp.empty()) {
            base = fs::path(env_temp) / "saves";
        } else {
            std::error_code ec;
            const fs::path tmp = fs::temp_directory_path(ec);
            base = (!ec && !tmp.empty()) ? (tmp / "openscarlett_saves")
                                         : fs::path("openscarlett_saves");
        }
    }
    return base / "xsapi" / SanitizePathToken(scid);
}

fs::path AchievementsFilePath(const std::string& scid, uint64_t xuid) {
    return ResolveXsapiDir(scid) / ("achievements_" + FormatXuidHex(xuid) + ".json");
}

fs::path StatsFilePath(const std::string& scid, uint64_t xuid) {
    return ResolveXsapiDir(scid) / ("stats_" + FormatXuidHex(xuid) + ".json");
}

uint32_t CurrentTitleId() {
    uint32_t tid = 0;
    if (XGameGetXboxTitleId(&tid) == XGR_S_OK && tid != 0) {
        return tid;
    }
    return 0x5CA00001u;
}

void CopyCString(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) return;
    const size_t n = std::min(src.size(), dst_size - 1);
    if (n > 0) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

struct JsonValue {
    enum class Kind { Null, Bool, Int, String, Array, Object };
    Kind kind = Kind::Null;
    bool bool_val = false;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    const JsonValue* Find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        auto it = obj_val.find(key);
        return it == obj_val.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    void SkipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out) {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') {
            out = JsonValue{};
            out.kind = JsonValue::Kind::String;
            return ParseString(out.str_val);
        }
        if (c == 't' && s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = true;
            return true;
        }
        if (c == 'f' && s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = false;
            return true;
        }
        if (c == 'n' && s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseInt(out);
        }
        return false;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                const char esc = s_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = s_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(10 + h - 'a');
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(10 + h - 'A');
                            else return false;
                        }
                        out.push_back(code < 0x80u ? static_cast<char>(code) : '?');
                        break;
                    }
                    default:
                        return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseInt(JsonValue& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
            while (pos_ < s_.size() &&
                   (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                    s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                    s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
        }
        out = JsonValue{};
        out.kind = JsonValue::Kind::Int;
        try {
            out.int_val = std::stoll(s_.substr(start, pos_ - start));
        } catch (...) {
            out.int_val = 0;
        }
        return true;
    }

    bool ParseArray(JsonValue& out) {
        ++pos_;
        out = JsonValue{};
        out.kind = JsonValue::Kind::Array;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            JsonValue elem;
            if (!ParseValue(elem)) return false;
            out.arr_val.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
        return false;
    }

    bool ParseObject(JsonValue& out) {
        ++pos_;
        out = JsonValue{};
        out.kind = JsonValue::Kind::Object;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JsonValue val;
            if (!ParseValue(val)) return false;
            out.obj_val[std::move(key)] = std::move(val);
            SkipWs();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') return false;
            ++pos_;
        }
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool WriteFileAtomically(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    fs::path tmp_path = path;
    tmp_path += ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ofs.flush();
        if (!ofs.good()) return false;
    }
    fs::rename(tmp_path, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp_path, path, ec);
        if (ec) {
            fs::remove(tmp_path, ec);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            ofs.flush();
            return ofs.good();
        }
    }
    return true;
}

bool ReadFileToString(const fs::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

void EnsureAchievementsLoadedLocked(XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    if (s.achievements_loaded[key]) return;
    s.achievements_loaded[key] = true;
    auto& map = s.achievements_cache[key];

    std::string raw;
    if (!ReadFileToString(AchievementsFilePath(scid, xuid), raw)) return;
    JsonValue root;
    JsonParser parser(raw);
    if (!parser.Parse(root)) return;
    const JsonValue* arr = root.Find("achievements");
    if (!arr || arr->kind != JsonValue::Kind::Array) return;
    for (const auto& item : arr->arr_val) {
        if (item.kind != JsonValue::Kind::Object) continue;
        const JsonValue* id_v = item.Find("id");
        if (!id_v || id_v->kind != JsonValue::Kind::String || id_v->str_val.empty()) continue;

        StoredAchievement ach{};
        ach.id = id_v->str_val;
        ach.scid = scid;
        if (const auto* v = item.Find("scid"); v && v->kind == JsonValue::Kind::String && !v->str_val.empty())
            ach.scid = v->str_val;
        if (const auto* v = item.Find("name"); v && v->kind == JsonValue::Kind::String)
            ach.name = v->str_val;
        else
            ach.name = "Achievement " + ach.id;
        if (const auto* v = item.Find("unlockedDescription"); v && v->kind == JsonValue::Kind::String)
            ach.unlocked_desc = v->str_val;
        else
            ach.unlocked_desc = "Unlocked achievement " + ach.id;
        if (const auto* v = item.Find("lockedDescription"); v && v->kind == JsonValue::Kind::String)
            ach.locked_desc = v->str_val;
        else
            ach.locked_desc = "Complete achievement " + ach.id;
        if (const auto* v = item.Find("titleId"); v && v->kind == JsonValue::Kind::Int)
            ach.title_id = static_cast<uint32_t>(v->int_val);
        if (const auto* v = item.Find("percentComplete"); v && v->kind == JsonValue::Kind::Int)
            ach.percent_complete = static_cast<uint32_t>(std::clamp<int64_t>(v->int_val, 0, 100));
        if (const auto* v = item.Find("timeUnlocked"); v && v->kind == JsonValue::Kind::Int)
            ach.time_unlocked = v->int_val;
        if (const auto* v = item.Find("gamerscore"); v && v->kind == JsonValue::Kind::Int)
            ach.gamerscore = static_cast<uint32_t>(std::max<int64_t>(0, v->int_val));
        if (const auto* v = item.Find("type"); v && v->kind == JsonValue::Kind::Int)
            ach.type = static_cast<XblAchievementType>(v->int_val);
        if (const auto* v = item.Find("isSecret"); v && v->kind == JsonValue::Kind::Bool)
            ach.is_secret = v->bool_val;
        map[ach.id] = std::move(ach);
    }
}

bool SaveAchievementsLocked(const XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    auto it = s.achievements_cache.find(key);
    std::ostringstream oss;
    oss << "{\n  \"xuid\": " << xuid << ",\n  \"scid\": " << JsonEscape(scid) << ",\n  \"achievements\": [";
    if (it != s.achievements_cache.end() && !it->second.empty()) {
        oss << "\n";
        bool first = true;
        for (const auto& [id, ach] : it->second) {
            if (!first) oss << ",\n";
            first = false;
            oss << "    {\n"
                << "      \"id\": " << JsonEscape(ach.id) << ",\n"
                << "      \"scid\": " << JsonEscape(ach.scid) << ",\n"
                << "      \"name\": " << JsonEscape(ach.name) << ",\n"
                << "      \"unlockedDescription\": " << JsonEscape(ach.unlocked_desc) << ",\n"
                << "      \"lockedDescription\": " << JsonEscape(ach.locked_desc) << ",\n"
                << "      \"titleId\": " << ach.title_id << ",\n"
                << "      \"percentComplete\": " << ach.percent_complete << ",\n"
                << "      \"timeUnlocked\": " << ach.time_unlocked << ",\n"
                << "      \"gamerscore\": " << ach.gamerscore << ",\n"
                << "      \"type\": " << static_cast<uint32_t>(ach.type) << ",\n"
                << "      \"isSecret\": " << (ach.is_secret ? "true" : "false") << "\n"
                << "    }";
        }
        oss << "\n  ";
    }
    oss << "]\n}\n";
    return WriteFileAtomically(AchievementsFilePath(scid, xuid), oss.str());
}

void EnsureStatsLoadedLocked(XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    if (s.stats_loaded[key]) return;
    s.stats_loaded[key] = true;
    auto& map = s.stats_cache[key];

    std::string raw;
    if (!ReadFileToString(StatsFilePath(scid, xuid), raw)) return;
    JsonValue root;
    JsonParser parser(raw);
    if (!parser.Parse(root)) return;
    const JsonValue* arr = root.Find("statistics");
    if (!arr || arr->kind != JsonValue::Kind::Array) return;
    for (const auto& item : arr->arr_val) {
        if (item.kind != JsonValue::Kind::Object) continue;
        const JsonValue* name_v = item.Find("name");
        if (!name_v || name_v->kind != JsonValue::Kind::String || name_v->str_val.empty()) continue;

        StoredStat st{};
        st.name = name_v->str_val;
        if (const auto* v = item.Find("type"); v && v->kind == JsonValue::Kind::String)
            st.type = v->str_val;
        else
            st.type = "String";
        if (const auto* v = item.Find("value"); v && v->kind == JsonValue::Kind::String)
            st.value = v->str_val;
        map[st.name] = std::move(st);
    }
}

bool SaveStatsLocked(const XsapiState& s, const std::string& scid, uint64_t xuid) {
    const StoreKey key{scid, xuid};
    auto it = s.stats_cache.find(key);
    std::ostringstream oss;
    oss << "{\n  \"xuid\": " << xuid << ",\n  \"scid\": " << JsonEscape(scid) << ",\n  \"statistics\": [";
    if (it != s.stats_cache.end() && !it->second.empty()) {
        oss << "\n";
        bool first = true;
        for (const auto& [name, st] : it->second) {
            if (!first) oss << ",\n";
            first = false;
            oss << "    {\n"
                << "      \"name\": " << JsonEscape(st.name) << ",\n"
                << "      \"type\": " << JsonEscape(st.type) << ",\n"
                << "      \"value\": " << JsonEscape(st.value) << "\n"
                << "    }";
        }
        oss << "\n  ";
    }
    oss << "]\n}\n";
    return WriteFileAtomically(StatsFilePath(scid, xuid), oss.str());
}

std::shared_ptr<XblContext> LookupContext(XblContextHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_contexts.find(handle);
    return it == s.live_contexts.end() ? nullptr : it->second;
}

std::shared_ptr<XblAchievementsResult> LookupAchResult(XblAchievementsResultHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_ach_results.find(handle);
    return it == s.live_ach_results.end() ? nullptr : it->second;
}

std::shared_ptr<XblPresenceRecord> LookupPresenceRecord(XblPresenceRecordHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_presence_records.find(handle);
    return it == s.live_presence_records.end() ? nullptr : it->second;
}

std::shared_ptr<XblSocialRelationshipResult> LookupSocialResult(
    XblSocialRelationshipResultHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_social_results.find(handle);
    return it == s.live_social_results.end() ? nullptr : it->second;
}

std::shared_ptr<XblTitleStorageBlobMetadataResult> LookupTsResult(
    XblTitleStorageBlobMetadataResultHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_ts_results.find(handle);
    return it == s.live_ts_results.end() ? nullptr : it->second;
}

std::shared_ptr<XblHttpCall> LookupHttpCall(XblHttpCallHandle handle) {
    if (!handle) return nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_http_calls.find(handle);
    return it == s.live_http_calls.end() ? nullptr : it->second;
}

bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

const char kIdCleanup[] = "XblCleanupAsync";
const char kIdAchGet[] = "XblAchievementsGetAchievementsForTitleIdAsync";
const char kIdAchGetSingle[] = "XblAchievementsGetAchievementAsync";
const char kIdAchGetNext[] = "XblAchievementsResultGetNextAsync";
const char kIdAchUpdate[] = "XblAchievementsUpdateAchievementAsync";
const char kIdPresenceSet[] = "XblPresenceSetPresenceAsync";
const char kIdPresenceGet[] = "XblPresenceGetPresenceAsync";
const char kIdPresenceGetMulti[] = "XblPresenceGetPresenceForMultipleUsersAsync";
const char kIdPresenceGetGroup[] = "XblPresenceGetPresenceForSocialGroupAsync";
const char kIdProfileGet[] = "XblProfileGetUserProfileAsync";
const char kIdProfileGetMulti[] = "XblProfileGetUserProfilesAsync";
const char kIdProfileGetGroup[] = "XblProfileGetUserProfilesForSocialGroupAsync";
const char kIdTsQuota[] = "XblTitleStorageGetQuotaAsync";
const char kIdTsMeta[] = "XblTitleStorageGetBlobMetadataAsync";
const char kIdTsMetaNext[] = "XblTitleStorageBlobMetadataResultGetNextAsync";
const char kIdTsUpload[] = "XblTitleStorageUploadBlobAsync";
const char kIdTsDownload[] = "XblTitleStorageDownloadBlobAsync";
const char kIdTsDelete[] = "XblTitleStorageDeleteBlobAsync";
const char kIdPrivacyCheck[] = "XblPrivacyCheckPermissionAsync";
const char kIdPrivacyCheckAnon[] = "XblPrivacyCheckPermissionForAnonymousUserAsync";
const char kIdPrivacyBatch[] = "XblPrivacyBatchCheckPermissionAsync";
const char kIdPrivacyAvoid[] = "XblPrivacyGetAvoidListAsync";
const char kIdPrivacyMute[] = "XblPrivacyGetMuteListAsync";
const char kIdSocialGet[] = "XblSocialGetSocialRelationshipsAsync";
const char kIdSocialGetNext[] = "XblSocialRelationshipResultGetNextAsync";
const char kIdSocialRep[] = "XblSocialSubmitReputationFeedbackAsync";
const char kIdSocialBatchRep[] = "XblSocialSubmitBatchReputationFeedbackAsync";
const char kIdHttpPerform[] = "XblHttpCallPerformAsync";
const char kIdStatsGet[] = "XblUserStatisticsGetSingleUserStatisticAsync";
const char kIdStatsGetSingleMulti[] = "XblUserStatisticsGetSingleUserStatisticsAsync";
const char kIdStatsGetMulti[] = "XblUserStatisticsGetMultipleUserStatisticsAsync";
const char kIdStatsGetMultiScid[] =
    "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync";
const char kIdStatsWrite[] = "XblTitleManagedStatsWriteAsync";
const char kIdStatsUpdate[] = "XblTitleManagedStatsUpdateStatsAsync";
const char kIdStatsDelete[] = "XblTitleManagedStatsDeleteStatsAsync";
const char kIdStringVerify[] = "XblStringVerifyStringAsync";
const char kIdStringVerifyMulti[] = "XblStringVerifyStringsAsync";

struct XsapiAsyncOp {
    std::function<XGameResult(XsapiAsyncOp&)> work_fn;
    std::function<XGameResult(void* buffer, size_t buffer_size)> get_result_fn;
    std::function<void()> cleanup_unclaimed_fn;
    size_t required_buffer_size = 0;
    std::atomic<bool> canceled{false};
};

XGameResult OPENSCARLETT_GDK_CALL XsapiAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<XsapiAsyncOp*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (ctx->canceled.load(std::memory_order_acquire)) {
                XAsyncComplete(data->async, XGR_E_ABORT, 0);
                return XGR_E_PENDING;
            }
            XGameResult hr = ctx->work_fn ? ctx->work_fn(*ctx) : XGR_E_FAIL;
            if (ctx->canceled.load(std::memory_order_acquire) && hr >= 0) {
                hr = XGR_E_ABORT;
            }
            XAsyncComplete(data->async, hr, hr >= 0 ? ctx->required_buffer_size : 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult:
            if (!ctx->get_result_fn) return XGR_E_NOT_SUPPORTED;
            return ctx->get_result_fn(data->buffer, data->bufferSize);
        case XAsyncOp_Cancel:
            ctx->canceled.store(true, std::memory_order_release);
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            if (ctx->cleanup_unclaimed_fn) ctx->cleanup_unclaimed_fn();
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult StartXsapiAsync(
    XAsyncBlock* async,
    const char* identity,
    std::unique_ptr<XsapiAsyncOp> op) {
    if (!async) return XGR_E_INVALIDARG;
    XsapiAsyncOp* raw = op.release();
    XGameResult hr = XAsyncBegin(async, raw, identity, identity, &XsapiAsyncProvider);
    if (hr < 0) {
        delete raw;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
        return hr;
    }
    return XGR_S_OK;
}

bool CompareAchievementIds(const std::string& a, const std::string& b) {
    auto is_digits = [](const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    if (is_digits(a) && is_digits(b)) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    }
    return a < b;
}

std::string FormatDoubleStat(double val) {
    if (std::isfinite(val)) {
        const double rounded = std::round(val);
        if (std::fabs(val - rounded) < 1e-9 &&
            rounded >= -9007199254740992.0 && rounded <= 9007199254740992.0) {
            return std::to_string(static_cast<int64_t>(rounded));
        }
    }
    std::ostringstream oss;
    oss << std::setprecision(15) << val;
    return oss.str();
}

std::shared_ptr<XblAchievementsResult> BuildAchievementsResult(
    const std::vector<StoredAchievement>& slice,
    bool has_next,
    const std::string& scid,
    uint64_t xuid,
    uint32_t title_id,
    uint32_t type,
    bool unlocked_only,
    uint32_t order_by,
    uint32_t next_skip) {
    auto res = std::make_shared<XblAchievementsResult>();
    res->has_next = has_next;
    res->query_scid = scid;
    res->query_xuid = xuid;
    res->query_title_id = title_id;
    res->query_type = type;
    res->query_unlocked_only = unlocked_only;
    res->query_order_by = order_by;
    res->next_skip = next_skip;

    const size_t count = slice.size();
    res->storage.reserve(count);
    res->requirements.reserve(count);
    res->items.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const auto& src = slice[i];
        XblAchievementsResult::EntryStorage st{};
        st.id = src.id;
        st.scid = src.scid;
        st.name = src.name;
        st.unlocked_desc = src.unlocked_desc;
        st.locked_desc = src.locked_desc;
        st.current_progress_str = std::to_string(src.percent_complete);
        st.target_progress_str = "100";
        res->storage.push_back(std::move(st));
    }

    for (size_t i = 0; i < count; ++i) {
        const auto& st = res->storage[i];
        XblAchievementRequirement req{};
        req.id = st.id.c_str();
        req.currentProgressValue = st.current_progress_str.c_str();
        req.targetProgressValue = st.target_progress_str.c_str();
        res->requirements.push_back(req);
    }

    for (size_t i = 0; i < count; ++i) {
        const auto& src = slice[i];
        const auto& st = res->storage[i];
        XblAchievement item{};
        item.id = st.id.c_str();
        item.serviceConfigurationId = st.scid.c_str();
        item.name = st.name.c_str();
        item.titleId = src.title_id;
        item.progressState = src.percent_complete >= 100u
                                 ? XblAchievementProgressState_Achieved
                                 : (src.percent_complete > 0u
                                        ? XblAchievementProgressState_InProgress
                                        : XblAchievementProgressState_NotStarted);
        item.progression.requirements = &res->requirements[i];
        item.progression.requirementsCount = 1;
        item.progression.timeUnlocked = src.time_unlocked;
        item.unlockedDescription = st.unlocked_desc.c_str();
        item.lockedDescription = st.locked_desc.c_str();
        item.type = src.type;
        item.participationType = XblAchievementParticipationType_Individual;
        item.percentComplete = src.percent_complete;
        item.gamerscore = src.gamerscore;
        item.isSecret = src.is_secret;
        item.isRevoked = false;
        res->items.push_back(item);
    }

    return res;
}

XblUserProfile BuildUserProfileForXuid(
    const std::shared_ptr<XblContext>& ctx,
    uint64_t target_xuid) {
    uint64_t total_gamerscore = 0;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        EnsureAchievementsLoadedLocked(s, ctx->scid, target_xuid);
        auto it = s.achievements_cache.find(StoreKey{ctx->scid, target_xuid});
        if (it != s.achievements_cache.end()) {
            for (const auto& [id, ach] : it->second) {
                if (ach.percent_complete >= 100u) total_gamerscore += ach.gamerscore;
            }
        }
    }

    XblUserProfile p{};
    p.xboxUserId = target_xuid;

    std::string gt_classic = "OpenScarlettUser";
    std::string gt_modern = "OpenScarlett";
    std::string gt_suffix = "1001";
    std::string gt_unique = "OpenScarlett#1001";

    if (target_xuid == ctx->xuid && ctx->user) {
        char buf[128]{};
        size_t used = 0;
        if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Classic, sizeof(buf), buf, &used) == XGR_S_OK)
            gt_classic = buf;
        if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Modern, sizeof(buf), buf, &used) == XGR_S_OK)
            gt_modern = buf;
        if (XUserGetGamertag(ctx->user, XUserGamertagComponent_ModernSuffix, sizeof(buf), buf, &used) == XGR_S_OK)
            gt_suffix = buf;
        if (XUserGetGamertag(ctx->user, XUserGamertagComponent_UniqueModern, sizeof(buf), buf, &used) == XGR_S_OK)
            gt_unique = buf;
    } else {
        const std::string tag_id = std::to_string(target_xuid % 10000ull);
        gt_classic = "OfflineUser" + tag_id;
        gt_modern = "OfflineUser";
        gt_suffix = tag_id;
        gt_unique = gt_modern + "#" + gt_suffix;
    }

    CopyCString(p.gamertag, sizeof(p.gamertag), gt_classic);
    CopyCString(p.modernGamertag, sizeof(p.modernGamertag), gt_modern);
    CopyCString(p.modernGamertagSuffix, sizeof(p.modernGamertagSuffix), gt_suffix);
    CopyCString(p.uniqueModernGamertag, sizeof(p.uniqueModernGamertag), gt_unique);
    CopyCString(p.appDisplayName, sizeof(p.appDisplayName), gt_classic);
    CopyCString(p.gameDisplayName, sizeof(p.gameDisplayName), gt_classic);
    CopyCString(p.gamerscore, sizeof(p.gamerscore), std::to_string(total_gamerscore));
    CopyCString(p.appDisplayPictureResizeUri, sizeof(p.appDisplayPictureResizeUri),
                "https://openscarlett.local/gamerpic/default.png");
    CopyCString(p.gameDisplayPictureResizeUri, sizeof(p.gameDisplayPictureResizeUri),
                "https://openscarlett.local/gamerpic/default.png");
    return p;
}

std::shared_ptr<XblPresenceRecord> BuildPresenceRecordForXuid(
    uint64_t caller_xuid,
    uint64_t target_xuid) {
    StoredPresence stored{};
    bool found = false;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = s.presence_by_xuid.find(target_xuid);
        if (it != s.presence_by_xuid.end()) {
            stored = it->second;
            found = true;
        }
    }

    auto rec = std::make_shared<XblPresenceRecord>();
    rec->xuid = target_xuid;
    if (found) {
        rec->user_state = stored.user_state;
        rec->title_name = stored.title_name;
        rec->rich_presence_string = stored.rich_presence;

        XblPresenceTitleRecord tr{};
        tr.titleId = stored.title_id;
        tr.titleName = rec->title_name.c_str();
        tr.lastModified = stored.last_modified;
        tr.titleActive = stored.title_active;
        tr.richPresenceString = rec->rich_presence_string.c_str();
        tr.viewState = stored.title_active ? 1u : 0u;
        rec->title_records.push_back(tr);

        XblPresenceDeviceRecord dr{};
        dr.deviceType = XblPresenceDeviceType_Scarlett;
        dr.titleRecords = rec->title_records.data();
        dr.titleRecordsCount = rec->title_records.size();
        rec->device_records.push_back(dr);
    } else {
        rec->user_state = (target_xuid == caller_xuid)
                              ? XblPresenceUserState_Online
                              : XblPresenceUserState_Offline;
    }
    return rec;
}

fs::path TitleStorageBlobDiskPath(const TitleStorageKey& key) {
    return ResolveXsapiDir(key.scid) / "title_storage" /
           (std::to_string(key.storage_type) + "_" + FormatXuidHex(key.xuid) + "_" +
            SanitizePathToken(key.blob_path) + ".blob");
}

XblSocialManagerUser BuildSocialManagerUser(uint64_t xuid, bool is_local) {
    XblSocialManagerUser u{};
    u.xboxUserId = xuid;
    u.isFavorite = false;
    u.isFollowingUser = true;
    u.isFollowedByCaller = true;
    const std::string tag_id = std::to_string(xuid % 10000ull);
    const std::string gt = is_local ? "OpenScarlettUser" : ("OfflineUser" + tag_id);
    const std::string mod = is_local ? "OpenScarlett" : "OfflineUser";
    const std::string suf = is_local ? "1001" : tag_id;
    const std::string uniq = mod + "#" + suf;
    CopyCString(u.displayName, sizeof(u.displayName), gt);
    CopyCString(u.realName, sizeof(u.realName), gt);
    CopyCString(u.displayPicUrlRaw, sizeof(u.displayPicUrlRaw),
                "https://openscarlett.local/gamerpic/default.png");
    u.useAvatar = false;
    CopyCString(u.gamerscore, sizeof(u.gamerscore), "0");
    CopyCString(u.gamertag, sizeof(u.gamertag), gt);
    CopyCString(u.modernGamertag, sizeof(u.modernGamertag), mod);
    CopyCString(u.modernGamertagSuffix, sizeof(u.modernGamertagSuffix), suf);
    CopyCString(u.uniqueModernGamertag, sizeof(u.uniqueModernGamertag), uniq);
    u.presenceRecord.userState = is_local ? XblPresenceUserState_Online : XblPresenceUserState_Offline;
    u.presenceRecord.presenceTitleRecordCount = 0;
    u.titleHistory.hasUserPlayed = is_local;
    u.titleHistory.lastTimeUserPlayed = is_local ? NowUnixSeconds() : 0;
    CopyCString(u.preferredColor.primaryColor, sizeof(u.preferredColor.primaryColor), "107C10");
    CopyCString(u.preferredColor.secondaryColor, sizeof(u.preferredColor.secondaryColor), "0E6A0E");
    CopyCString(u.preferredColor.tertiaryColor, sizeof(u.preferredColor.tertiaryColor), "0B520B");
    return u;
}

void RebuildSocialGroupUsersLocked(XblSocialManagerUserGroup& grp) {
    grp.users_storage.clear();
    grp.user_ptrs.clear();
    grp.users_storage.reserve(grp.tracked_xuids.size());
    for (uint64_t x : grp.tracked_xuids) {
        grp.users_storage.push_back(BuildSocialManagerUser(x, x == grp.local_xuid));
    }
    grp.user_ptrs.reserve(grp.users_storage.size());
    for (const auto& u : grp.users_storage) {
        grp.user_ptrs.push_back(&u);
    }
}

struct MultiStatUserBundle {
    uint64_t xuid = 0;
    struct ScidGroup {
        std::string scid;
        std::vector<StoredStat> stats;
    };
    std::vector<ScidGroup> scid_groups;
};

size_t ComputeMultiStatsBufferSize(const std::vector<MultiStatUserBundle>& bundles) {
    size_t sz = bundles.size() * sizeof(XblUserStatisticsResult);
    for (const auto& ub : bundles) {
        sz += ub.scid_groups.size() * sizeof(XblServiceConfigurationStatistic);
        for (const auto& sg : ub.scid_groups) {
            sz += sg.stats.size() * sizeof(XblStatistic);
            for (const auto& st : sg.stats) {
                sz += (st.name.size() + 1u) + (st.type.size() + 1u) + (st.value.size() + 1u);
            }
        }
    }
    return sz;
}

XGameResult PackMultiStatsBuffer(
    const std::vector<MultiStatUserBundle>& bundles,
    void* buffer,
    size_t buffer_size) {
    const size_t needed = ComputeMultiStatsBufferSize(bundles);
    if (needed == 0u) return XBL_S_OK;
    if (!buffer) return XGR_E_INVALIDARG;
    if (buffer_size < needed) return XGR_E_NOT_SUFFICIENT_BUFFER;
    std::memset(buffer, 0, needed);

    auto* base = static_cast<uint8_t*>(buffer);
    size_t offset = 0;

    auto* user_results = reinterpret_cast<XblUserStatisticsResult*>(base + offset);
    offset += bundles.size() * sizeof(XblUserStatisticsResult);

    size_t total_scids = 0;
    size_t total_stats = 0;
    for (const auto& ub : bundles) {
        total_scids += ub.scid_groups.size();
        for (const auto& sg : ub.scid_groups) {
            total_stats += sg.stats.size();
        }
    }

    auto* sc_stats = reinterpret_cast<XblServiceConfigurationStatistic*>(base + offset);
    offset += total_scids * sizeof(XblServiceConfigurationStatistic);

    auto* stat_entries = reinterpret_cast<XblStatistic*>(base + offset);
    offset += total_stats * sizeof(XblStatistic);

    auto append_str = [&](const std::string& str) -> const char* {
        char* dst = reinterpret_cast<char*>(base + offset);
        std::memcpy(dst, str.c_str(), str.size() + 1u);
        offset += str.size() + 1u;
        return dst;
    };

    size_t sc_idx = 0;
    size_t st_idx = 0;
    for (size_t u = 0; u < bundles.size(); ++u) {
        const auto& ub = bundles[u];
        user_results[u].xboxUserId = ub.xuid;
        user_results[u].serviceConfigStatisticsCount = ub.scid_groups.size();
        user_results[u].serviceConfigStatistics =
            ub.scid_groups.empty() ? nullptr : &sc_stats[sc_idx];

        for (const auto& sg : ub.scid_groups) {
            auto& sc = sc_stats[sc_idx++];
            CopyCString(sc.serviceConfigurationId, sizeof(sc.serviceConfigurationId), sg.scid);
            sc.statisticsCount = sg.stats.size();
            sc.statistics = sg.stats.empty() ? nullptr : &stat_entries[st_idx];
            for (const auto& st : sg.stats) {
                auto& out_st = stat_entries[st_idx++];
                out_st.statisticName = append_str(st.name);
                out_st.statisticType = append_str(st.type);
                out_st.value = append_str(st.value);
            }
        }
    }
    return XBL_S_OK;
}

void FlushAndResetState() {
    XTaskQueueHandle old_queue = nullptr;
    std::vector<XUserHandle> sm_users_to_close;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        for (const auto& [key, _] : s.achievements_cache) {
            SaveAchievementsLocked(s, key.scid, key.xuid);
        }
        for (const auto& [key, _] : s.stats_cache) {
            SaveStatsLocked(s, key.scid, key.xuid);
        }
        s.achievements_cache.clear();
        s.achievements_loaded.clear();
        s.stats_cache.clear();
        s.stats_loaded.clear();
        s.presence_by_xuid.clear();
        s.title_storage_blobs.clear();
        s.recorded_in_game_events.clear();
        s.call_routed_handlers.clear();
        s.social_changed_handlers.clear();
        s.stat_changed_handlers.clear();
        s.device_presence_handlers.clear();
        s.title_presence_handlers.clear();
        s.live_subscriptions.clear();
        for (auto& lu : s.sm_local_users) {
            if (lu.user) sm_users_to_close.push_back(lu.user);
        }
        s.sm_local_users.clear();
        s.sm_groups.clear();
        s.sm_pending_events.clear();
        s.sm_active_events.clear();
        old_queue = s.default_queue;
        s.default_queue = nullptr;
        s.scid.clear();
        s.initialized = false;
    }
    for (XUserHandle u : sm_users_to_close) {
        XUserCloseHandle(u);
    }
    if (old_queue) {
        XTaskQueueCloseHandle(old_queue);
    }
}

}  // namespace

extern "C" {

// ===========================================================================
// Memory Hooks, Lifecycle & Context
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblMemSetFunctions(
    XblMemAllocFunction memAllocFunc,
    XblMemFreeFunction memFreeFunc) {
    if ((memAllocFunc != nullptr) != (memFreeFunc != nullptr)) {
        return RecordHr("XblMemSetFunctions", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.mem_alloc = memAllocFunc;
    s.mem_free = memFreeFunc;
    return RecordHr("XblMemSetFunctions", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblInitialize(const XblInitArgs* args) {
    if (!args) {
        return RecordHr("XblInitialize", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    XTaskQueueHandle dup_queue = nullptr;
    if (args->queue) {
        XGameResult hr = XTaskQueueDuplicateHandle(args->queue, &dup_queue);
        if (hr < 0) {
            return RecordHr("XblInitialize", hr);
        }
    }

    XTaskQueueHandle old_queue = nullptr;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        old_queue = s.default_queue;
        s.default_queue = dup_queue;
        s.scid = (args->scid && *args->scid)
                     ? std::string(args->scid)
                     : std::string("00000000-0000-0000-0000-00005ca00001");
        s.initialized = true;
    }
    if (old_queue) {
        XTaskQueueCloseHandle(old_queue);
    }
    return RecordHr("XblInitialize", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblCleanup(void) {
    FlushAndResetState();
    RecordOk("XblCleanup");
}

XGameResult OPENSCARLETT_XSAPI_CALL XblCleanupAsync(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XblCleanupAsync", XGR_E_INVALIDARG);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult {
        FlushAndResetState();
        return XBL_S_OK;
    };
    return RecordHr("XblCleanupAsync", StartXsapiAsync(async, kIdCleanup, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextCreateHandle(
    XUserHandle user, XblContextHandle* context) {
    if (!user || !context) {
        return RecordHr("XblContextCreateHandle", XGR_E_INVALIDARG);
    }
    *context = nullptr;

    std::string active_scid;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) {
            return RecordHr("XblContextCreateHandle", XBL_E_NOT_INITIALIZED);
        }
        active_scid = s.scid;
    }

    uint64_t xuid = 0;
    XGameResult hr = XUserGetId(user, &xuid);
    if (hr < 0) return RecordHr("XblContextCreateHandle", hr);

    XUserHandle dup_user = nullptr;
    hr = XUserDuplicateHandle(user, &dup_user);
    if (hr < 0) return RecordHr("XblContextCreateHandle", hr);

    auto ctx = std::make_shared<XblContext>();
    ctx->user = dup_user;
    ctx->xuid = xuid;
    ctx->scid = std::move(active_scid);

    XblContextHandle raw = ctx.get();
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        s.live_contexts[raw] = std::move(ctx);
    }
    *context = raw;
    return RecordHr("XblContextCreateHandle", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextDuplicateHandle(
    XblContextHandle handle, XblContextHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XblContextDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto ctx = LookupContext(handle);
    if (!ctx) return RecordHr("XblContextDuplicateHandle", XGR_E_HANDLE);
    ctx->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XblContextDuplicateHandle", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblContextCloseHandle(XblContextHandle handle) {
    RecordOk("XblContextCloseHandle");
    if (!handle) return;
    std::shared_ptr<XblContext> dropped;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        auto it = s.live_contexts.find(handle);
        if (it == s.live_contexts.end()) return;
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            s.live_contexts.erase(it);
        }
    }
    if (dropped && dropped->user) {
        XUserCloseHandle(dropped->user);
        dropped->user = nullptr;
    }
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetUser(
    XblContextHandle context, XUserHandle* user) {
    if (!context || !user) return RecordHr("XblContextGetUser", XGR_E_INVALIDARG);
    *user = nullptr;
    auto ctx = LookupContext(context);
    if (!ctx || !ctx->user) return RecordHr("XblContextGetUser", XGR_E_HANDLE);
    return RecordHr("XblContextGetUser", XUserDuplicateHandle(ctx->user, user));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextGetXboxUserId(
    XblContextHandle context, uint64_t* xboxUserId) {
    if (!context || !xboxUserId) return RecordHr("XblContextGetXboxUserId", XGR_E_INVALIDARG);
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextGetXboxUserId", XGR_E_HANDLE);
    *xboxUserId = ctx->xuid;
    return RecordHr("XblContextGetXboxUserId", XBL_S_OK);
}

// ===========================================================================
// Achievements
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsUpdateAchievementForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    const char* serviceConfigurationId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !achievementId || !*achievementId || !async || percentComplete > 100u) {
        return RecordHr("XblAchievementsUpdateAchievementForTitleIdAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblAchievementsUpdateAchievementForTitleIdAsync", XGR_E_HANDLE);

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const uint32_t target_tid = titleId != 0 ? titleId : CurrentTitleId();
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string ach_id = achievementId;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, target_xuid, target_tid, target_scid, ach_id, percentComplete](
                      XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        EnsureAchievementsLoadedLocked(s, target_scid, target_xuid);
        auto& map = s.achievements_cache[StoreKey{target_scid, target_xuid}];
        auto& ach = map[ach_id];
        if (ach.id.empty()) {
            ach.id = ach_id;
            ach.scid = target_scid;
            ach.title_id = target_tid;
            ach.name = "Achievement " + ach_id;
            ach.unlocked_desc = "Unlocked achievement " + ach_id;
            ach.locked_desc = "Complete achievement " + ach_id;
            ach.gamerscore = 25;
            ach.type = XblAchievementType_Persistent;
            ach.is_secret = false;
        } else if (ach.title_id == 0) {
            ach.title_id = target_tid;
        }
        if (percentComplete > ach.percent_complete) {
            ach.percent_complete = percentComplete;
        }
        if (ach.percent_complete >= 100u) {
            ach.percent_complete = 100u;
            if (ach.time_unlocked == 0) ach.time_unlocked = NowUnixSeconds();
        }
        if (!SaveAchievementsLocked(s, target_scid, target_xuid)) return XGR_E_FAIL;
        return XBL_S_OK;
    };
    return RecordHr("XblAchievementsUpdateAchievementForTitleIdAsync",
                    StartXsapiAsync(async, kIdAchUpdate, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async) {
    const XGameResult hr = XblAchievementsUpdateAchievementForTitleIdAsync(
        xboxLiveContext, xboxUserId, CurrentTitleId(), nullptr, achievementId, percentComplete, async);
    return RecordHr("XblAchievementsUpdateAchievementAsync", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    uint32_t type,
    bool unlockedOnly,
    uint32_t orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblAchievementsGetAchievementsForTitleIdAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblAchievementsGetAchievementsForTitleIdAsync", XGR_E_HANDLE);

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto result_holder = std::make_shared<XblAchievementsResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, target_scid, titleId, type, unlockedOnly, orderBy,
                   skipItems, maxItems, result_holder](XsapiAsyncOp& self) -> XGameResult {
        std::vector<StoredAchievement> filtered;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureAchievementsLoadedLocked(s, target_scid, target_xuid);
            const auto it = s.achievements_cache.find(StoreKey{target_scid, target_xuid});
            if (it != s.achievements_cache.end()) {
                for (const auto& [id, ach] : it->second) {
                    if (titleId != 0 && ach.title_id != titleId) continue;
                    if (type == XblAchievementType_Persistent &&
                        ach.type != XblAchievementType_Persistent) continue;
                    if (type == XblAchievementType_Challenge &&
                        ach.type != XblAchievementType_Challenge) continue;
                    if (unlockedOnly && ach.percent_complete < 100u) continue;
                    filtered.push_back(ach);
                }
            }
        }

        if (orderBy == XblAchievementOrderBy_TitleId) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.title_id != b.title_id) return a.title_id < b.title_id;
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else if (orderBy == XblAchievementOrderBy_UnlockTime) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.time_unlocked != b.time_unlocked) return a.time_unlocked > b.time_unlocked;
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          return CompareAchievementIds(a.id, b.id);
                      });
        }

        const size_t total = filtered.size();
        const size_t start = std::min<size_t>(skipItems, total);
        size_t count = total - start;
        if (maxItems > 0 && count > maxItems) count = maxItems;
        const bool has_next = (start + count) < total;

        std::vector<StoredAchievement> slice(
            filtered.begin() + static_cast<std::ptrdiff_t>(start),
            filtered.begin() + static_cast<std::ptrdiff_t>(start + count));

        auto res = BuildAchievementsResult(
            slice,
            has_next,
            target_scid,
            target_xuid,
            titleId,
            type,
            unlockedOnly,
            orderBy,
            static_cast<uint32_t>(start + count));

        XblAchievementsResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ach_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblAchievementsResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblAchievementsResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblAchievementsResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblAchievementsResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblAchievementsGetAchievementsForTitleIdAsync",
                    StartXsapiAsync(async, kIdAchGet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async, XblAchievementsResultHandle* result) {
    if (!async || !result) {
        return RecordHr("XblAchievementsGetAchievementsForTitleIdResult", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblAchievementsGetAchievementsForTitleIdResult", hr);
    hr = XAsyncGetResult(async, kIdAchGet, sizeof(XblAchievementsResultHandle), result, nullptr);
    return RecordHr("XblAchievementsGetAchievementsForTitleIdResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount) {
    if (!resultHandle || !achievements || !achievementsCount) {
        return RecordHr("XblAchievementsResultGetAchievements", XGR_E_INVALIDARG);
    }
    auto res = LookupAchResult(resultHandle);
    if (!res) return RecordHr("XblAchievementsResultGetAchievements", XGR_E_HANDLE);
    *achievements = res->items.empty() ? nullptr : res->items.data();
    *achievementsCount = res->items.size();
    return RecordHr("XblAchievementsResultGetAchievements", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle, bool* hasNext) {
    if (!resultHandle || !hasNext) {
        return RecordHr("XblAchievementsResultHasNext", XGR_E_INVALIDARG);
    }
    auto res = LookupAchResult(resultHandle);
    if (!res) return RecordHr("XblAchievementsResultHasNext", XGR_E_HANDLE);
    *hasNext = res->has_next;
    return RecordHr("XblAchievementsResultHasNext", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultDuplicateHandle(
    XblAchievementsResultHandle handle, XblAchievementsResultHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XblAchievementsResultDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto res = LookupAchResult(handle);
    if (!res) return RecordHr("XblAchievementsResultDuplicateHandle", XGR_E_HANDLE);
    res->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XblAchievementsResultDuplicateHandle", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle) {
    RecordOk("XblAchievementsResultCloseHandle");
    if (!resultHandle) return;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_ach_results.find(resultHandle);
    if (it == s.live_ach_results.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        s.live_ach_results.erase(it);
    }
}

// ===========================================================================
// Presence
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPresenceSetPresenceAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPresenceSetPresenceAsync", XGR_E_HANDLE);

    std::string scid = ctx->scid;
    std::string rich_str;
    if (richPresenceIds) {
        if (richPresenceIds->scid[0] != '\0') scid = richPresenceIds->scid;
        if (richPresenceIds->presenceId && *richPresenceIds->presenceId) {
            rich_str = richPresenceIds->presenceId;
            if (richPresenceIds->presenceTokenIds && richPresenceIds->presenceTokenIdsCount > 0) {
                rich_str += "(";
                for (size_t i = 0; i < richPresenceIds->presenceTokenIdsCount; ++i) {
                    if (i > 0) rich_str += ", ";
                    rich_str += richPresenceIds->presenceTokenIds[i]
                                    ? richPresenceIds->presenceTokenIds[i]
                                    : "";
                }
                rich_str += ")";
            }
        }
    }
    const uint32_t tid = CurrentTitleId();

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, isUserActiveInTitle, scid, rich_str, tid](XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        StoredPresence p{};
        p.xuid = ctx->xuid;
        p.user_state = isUserActiveInTitle ? XblPresenceUserState_Online : XblPresenceUserState_Away;
        p.title_id = tid;
        p.scid = scid;
        p.title_name = "OpenScarlett Title";
        p.rich_presence = rich_str;
        p.last_modified = NowUnixSeconds();
        p.title_active = isUserActiveInTitle;
        s.presence_by_xuid[ctx->xuid] = std::move(p);
        return XBL_S_OK;
    };
    return RecordHr("XblPresenceSetPresenceAsync",
                    StartXsapiAsync(async, kIdPresenceSet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext, uint64_t xuid, XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPresenceGetPresenceAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPresenceGetPresenceAsync", XGR_E_HANDLE);
    const uint64_t target_xuid = xuid != 0 ? xuid : ctx->xuid;

    auto result_holder = std::make_shared<XblPresenceRecordHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, result_holder](XsapiAsyncOp& self) -> XGameResult {
        StoredPresence stored{};
        bool found = false;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            auto it = s.presence_by_xuid.find(target_xuid);
            if (it != s.presence_by_xuid.end()) {
                stored = it->second;
                found = true;
            }
        }

        auto rec = std::make_shared<XblPresenceRecord>();
        rec->xuid = target_xuid;
        if (found) {
            rec->user_state = stored.user_state;
            rec->title_name = stored.title_name;
            rec->rich_presence_string = stored.rich_presence;

            XblPresenceTitleRecord tr{};
            tr.titleId = stored.title_id;
            tr.titleName = rec->title_name.c_str();
            tr.lastModified = stored.last_modified;
            tr.titleActive = stored.title_active;
            tr.richPresenceString = rec->rich_presence_string.c_str();
            tr.viewState = stored.title_active ? 1u : 0u;
            rec->title_records.push_back(tr);

            XblPresenceDeviceRecord dr{};
            dr.deviceType = XblPresenceDeviceType_Scarlett;
            dr.titleRecords = rec->title_records.data();
            dr.titleRecordsCount = rec->title_records.size();
            rec->device_records.push_back(dr);
        } else {
            rec->user_state = (target_xuid == ctx->xuid)
                                  ? XblPresenceUserState_Online
                                  : XblPresenceUserState_Offline;
        }

        XblPresenceRecordHandle raw = rec.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_presence_records[raw] = std::move(rec);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblPresenceRecordHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblPresenceRecordHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblPresenceRecordHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblPresenceRecordCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblPresenceGetPresenceAsync",
                    StartXsapiAsync(async, kIdPresenceGet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceResult(
    XAsyncBlock* async, XblPresenceRecordHandle* presenceRecordHandle) {
    if (!async || !presenceRecordHandle) {
        return RecordHr("XblPresenceGetPresenceResult", XGR_E_INVALIDARG);
    }
    *presenceRecordHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceResult", hr);
    hr = XAsyncGetResult(
        async, kIdPresenceGet, sizeof(XblPresenceRecordHandle), presenceRecordHandle, nullptr);
    return RecordHr("XblPresenceGetPresenceResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetXuid(
    XblPresenceRecordHandle handle, uint64_t* xuid) {
    if (!handle || !xuid) return RecordHr("XblPresenceRecordGetXuid", XGR_E_INVALIDARG);
    auto rec = LookupPresenceRecord(handle);
    if (!rec) return RecordHr("XblPresenceRecordGetXuid", XGR_E_HANDLE);
    *xuid = rec->xuid;
    return RecordHr("XblPresenceRecordGetXuid", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetUserState(
    XblPresenceRecordHandle handle, XblPresenceUserState* userState) {
    if (!handle || !userState) return RecordHr("XblPresenceRecordGetUserState", XGR_E_INVALIDARG);
    auto rec = LookupPresenceRecord(handle);
    if (!rec) return RecordHr("XblPresenceRecordGetUserState", XGR_E_HANDLE);
    *userState = rec->user_state;
    return RecordHr("XblPresenceRecordGetUserState", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordGetDeviceRecords(
    XblPresenceRecordHandle handle,
    const XblPresenceDeviceRecord** deviceRecords,
    size_t* deviceRecordsCount) {
    if (!handle || !deviceRecords || !deviceRecordsCount) {
        return RecordHr("XblPresenceRecordGetDeviceRecords", XGR_E_INVALIDARG);
    }
    auto rec = LookupPresenceRecord(handle);
    if (!rec) return RecordHr("XblPresenceRecordGetDeviceRecords", XGR_E_HANDLE);
    *deviceRecords = rec->device_records.empty() ? nullptr : rec->device_records.data();
    *deviceRecordsCount = rec->device_records.size();
    return RecordHr("XblPresenceRecordGetDeviceRecords", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRecordDuplicateHandle(
    XblPresenceRecordHandle handle, XblPresenceRecordHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XblPresenceRecordDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto rec = LookupPresenceRecord(handle);
    if (!rec) return RecordHr("XblPresenceRecordDuplicateHandle", XGR_E_HANDLE);
    rec->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XblPresenceRecordDuplicateHandle", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblPresenceRecordCloseHandle(XblPresenceRecordHandle handle) {
    RecordOk("XblPresenceRecordCloseHandle");
    if (!handle) return;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_presence_records.find(handle);
    if (it == s.live_presence_records.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        s.live_presence_records.erase(it);
    }
}

// ===========================================================================
// Profile
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext, uint64_t xboxUserId, XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblProfileGetUserProfileAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblProfileGetUserProfileAsync", XGR_E_HANDLE);
    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;

    auto profile_holder = std::make_shared<XblUserProfile>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, profile_holder](XsapiAsyncOp& self) -> XGameResult {
        uint64_t total_gamerscore = 0;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureAchievementsLoadedLocked(s, ctx->scid, target_xuid);
            auto it = s.achievements_cache.find(StoreKey{ctx->scid, target_xuid});
            if (it != s.achievements_cache.end()) {
                for (const auto& [id, ach] : it->second) {
                    if (ach.percent_complete >= 100u) total_gamerscore += ach.gamerscore;
                }
            }
        }

        XblUserProfile p{};
        p.xboxUserId = target_xuid;

        std::string gt_classic = "OpenScarlettUser";
        std::string gt_modern = "OpenScarlett";
        std::string gt_suffix = "1001";
        std::string gt_unique = "OpenScarlett#1001";

        if (target_xuid == ctx->xuid && ctx->user) {
            char buf[128]{};
            size_t used = 0;
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Classic, sizeof(buf), buf, &used) == XGR_S_OK)
                gt_classic = buf;
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_Modern, sizeof(buf), buf, &used) == XGR_S_OK)
                gt_modern = buf;
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_ModernSuffix, sizeof(buf), buf, &used) == XGR_S_OK)
                gt_suffix = buf;
            if (XUserGetGamertag(ctx->user, XUserGamertagComponent_UniqueModern, sizeof(buf), buf, &used) == XGR_S_OK)
                gt_unique = buf;
        } else {
            const std::string tag_id = std::to_string(target_xuid % 10000ull);
            gt_classic = "OfflineUser" + tag_id;
            gt_modern = "OfflineUser";
            gt_suffix = tag_id;
            gt_unique = gt_modern + "#" + gt_suffix;
        }

        CopyCString(p.gamertag, sizeof(p.gamertag), gt_classic);
        CopyCString(p.modernGamertag, sizeof(p.modernGamertag), gt_modern);
        CopyCString(p.modernGamertagSuffix, sizeof(p.modernGamertagSuffix), gt_suffix);
        CopyCString(p.uniqueModernGamertag, sizeof(p.uniqueModernGamertag), gt_unique);
        CopyCString(p.appDisplayName, sizeof(p.appDisplayName), gt_classic);
        CopyCString(p.gameDisplayName, sizeof(p.gameDisplayName), gt_classic);
        CopyCString(p.gamerscore, sizeof(p.gamerscore), std::to_string(total_gamerscore));
        CopyCString(p.appDisplayPictureResizeUri, sizeof(p.appDisplayPictureResizeUri),
                    "https://openscarlett.local/gamerpic/default.png");
        CopyCString(p.gameDisplayPictureResizeUri, sizeof(p.gameDisplayPictureResizeUri),
                    "https://openscarlett.local/gamerpic/default.png");

        *profile_holder = p;
        self.required_buffer_size = sizeof(XblUserProfile);
        return XBL_S_OK;
    };

    op->get_result_fn = [profile_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblUserProfile)) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(buffer, profile_holder.get(), sizeof(XblUserProfile));
        return XBL_S_OK;
    };

    return RecordHr("XblProfileGetUserProfileAsync",
                    StartXsapiAsync(async, kIdProfileGet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfileResult(
    XAsyncBlock* async, XblUserProfile* profile) {
    if (!async || !profile) return RecordHr("XblProfileGetUserProfileResult", XGR_E_INVALIDARG);
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblProfileGetUserProfileResult", hr);
    hr = XAsyncGetResult(async, kIdProfileGet, sizeof(XblUserProfile), profile, nullptr);
    return RecordHr("XblProfileGetUserProfileResult", hr);
}

// ===========================================================================
// Social Relationships (offline-safe empty list)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblSocialGetSocialRelationshipsAsync(
    XblContextHandle xboxLiveContext,
    uint64_t /*xboxUserId*/,
    XblSocialRelationshipFilter /*socialRelationshipFilter*/,
    size_t /*startIndex*/,
    size_t /*maxItems*/,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblSocialGetSocialRelationshipsAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblSocialGetSocialRelationshipsAsync", XGR_E_HANDLE);

    auto result_holder = std::make_shared<XblSocialRelationshipResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, result_holder](XsapiAsyncOp& self) -> XGameResult {
        auto res = std::make_shared<XblSocialRelationshipResult>();
        res->has_next = false;
        XblSocialRelationshipResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            s.live_social_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblSocialRelationshipResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblSocialRelationshipResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblSocialRelationshipResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblSocialRelationshipResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblSocialGetSocialRelationshipsAsync",
                    StartXsapiAsync(async, kIdSocialGet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialGetSocialRelationshipsResult(
    XAsyncBlock* async, XblSocialRelationshipResultHandle* handle) {
    if (!async || !handle) {
        return RecordHr("XblSocialGetSocialRelationshipsResult", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblSocialGetSocialRelationshipsResult", hr);
    hr = XAsyncGetResult(
        async, kIdSocialGet, sizeof(XblSocialRelationshipResultHandle), handle, nullptr);
    return RecordHr("XblSocialGetSocialRelationshipsResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultGetRelationships(
    XblSocialRelationshipResultHandle resultHandle,
    const XblSocialRelationship** relationships,
    size_t* relationshipsCount) {
    if (!resultHandle || !relationships || !relationshipsCount) {
        return RecordHr("XblSocialRelationshipResultGetRelationships", XGR_E_INVALIDARG);
    }
    auto res = LookupSocialResult(resultHandle);
    if (!res) return RecordHr("XblSocialRelationshipResultGetRelationships", XGR_E_HANDLE);
    *relationships = res->items.empty() ? nullptr : res->items.data();
    *relationshipsCount = res->items.size();
    return RecordHr("XblSocialRelationshipResultGetRelationships", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultHasNext(
    XblSocialRelationshipResultHandle resultHandle, bool* hasNext) {
    if (!resultHandle || !hasNext) {
        return RecordHr("XblSocialRelationshipResultHasNext", XGR_E_INVALIDARG);
    }
    auto res = LookupSocialResult(resultHandle);
    if (!res) return RecordHr("XblSocialRelationshipResultHasNext", XGR_E_HANDLE);
    *hasNext = res->has_next;
    return RecordHr("XblSocialRelationshipResultHasNext", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultDuplicateHandle(
    XblSocialRelationshipResultHandle handle,
    XblSocialRelationshipResultHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XblSocialRelationshipResultDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto res = LookupSocialResult(handle);
    if (!res) return RecordHr("XblSocialRelationshipResultDuplicateHandle", XGR_E_HANDLE);
    res->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XblSocialRelationshipResultDuplicateHandle", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultCloseHandle(
    XblSocialRelationshipResultHandle resultHandle) {
    RecordOk("XblSocialRelationshipResultCloseHandle");
    if (!resultHandle) return;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_social_results.find(resultHandle);
    if (it == s.live_social_results.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        s.live_social_results.erase(it);
    }
}

// ===========================================================================
// User Statistics & Title-Managed Statistics
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || (!statistics && statisticsCount > 0)) {
        return RecordHr("XblTitleManagedStatsWriteAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleManagedStatsWriteAsync", XGR_E_HANDLE);

    std::vector<StoredStat> entries;
    entries.reserve(statisticsCount);
    for (size_t i = 0; i < statisticsCount; ++i) {
        if (!statistics[i].statisticName || !*statistics[i].statisticName) {
            return RecordHr("XblTitleManagedStatsWriteAsync", XGR_E_INVALIDARG);
        }
        StoredStat st{};
        st.name = statistics[i].statisticName;
        if (statistics[i].statisticType == XblTitleManagedStatType_Number) {
            st.type = "Double";
            st.value = FormatDoubleStat(statistics[i].numberValue);
        } else if (statistics[i].statisticType == XblTitleManagedStatType_String) {
            st.type = "String";
            st.value = statistics[i].stringValue ? statistics[i].stringValue : "";
        } else {
            return RecordHr("XblTitleManagedStatsWriteAsync", XGR_E_INVALIDARG);
        }
        entries.push_back(std::move(st));
    }

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, target_xuid, target_scid, entries = std::move(entries)](
                      XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        EnsureStatsLoadedLocked(s, target_scid, target_xuid);
        auto& map = s.stats_cache[StoreKey{target_scid, target_xuid}];
        for (const auto& entry : entries) map[entry.name] = entry;
        if (!SaveStatsLocked(s, target_scid, target_xuid)) return XGR_E_FAIL;
        return XBL_S_OK;
    };
    return RecordHr("XblTitleManagedStatsWriteAsync",
                    StartXsapiAsync(async, kIdStatsWrite, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !statisticName || !*statisticName || !async) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblUserStatisticsGetSingleUserStatisticAsync", XGR_E_HANDLE);

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string stat_name = statisticName;

    struct StatPayload {
        uint64_t xuid = 0;
        std::string scid;
        StoredStat stat;
        size_t packed_size = 0;
    };
    auto payload = std::make_shared<StatPayload>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, target_scid, stat_name, payload](
                      XsapiAsyncOp& self) -> XGameResult {
        StoredStat result_stat{};
        result_stat.name = stat_name;
        result_stat.type = "Int64";
        result_stat.value = "0";
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureStatsLoadedLocked(s, target_scid, target_xuid);
            auto it = s.stats_cache.find(StoreKey{target_scid, target_xuid});
            if (it != s.stats_cache.end()) {
                auto sit = it->second.find(stat_name);
                if (sit != it->second.end()) result_stat = sit->second;
            }
        }

        payload->xuid = target_xuid;
        payload->scid = target_scid;
        payload->stat = std::move(result_stat);
        payload->packed_size = sizeof(XblUserStatisticsResult) +
                               sizeof(XblServiceConfigurationStatistic) +
                               sizeof(XblStatistic) +
                               (payload->stat.name.size() + 1) +
                               (payload->stat.type.size() + 1) +
                               (payload->stat.value.size() + 1);
        self.required_buffer_size = payload->packed_size;
        return XBL_S_OK;
    };

    op->get_result_fn = [payload](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer) return XGR_E_INVALIDARG;
        if (buffer_size < payload->packed_size) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memset(buffer, 0, payload->packed_size);

        auto* base = static_cast<uint8_t*>(buffer);
        size_t offset = 0;

        auto* res = reinterpret_cast<XblUserStatisticsResult*>(base + offset);
        offset += sizeof(XblUserStatisticsResult);

        auto* sc_stat = reinterpret_cast<XblServiceConfigurationStatistic*>(base + offset);
        offset += sizeof(XblServiceConfigurationStatistic);

        auto* stat = reinterpret_cast<XblStatistic*>(base + offset);
        offset += sizeof(XblStatistic);

        auto append_str = [&](const std::string& str) -> const char* {
            char* dst = reinterpret_cast<char*>(base + offset);
            std::memcpy(dst, str.c_str(), str.size() + 1);
            offset += str.size() + 1;
            return dst;
        };

        stat->statisticName = append_str(payload->stat.name);
        stat->statisticType = append_str(payload->stat.type);
        stat->value = append_str(payload->stat.value);

        CopyCString(sc_stat->serviceConfigurationId, sizeof(sc_stat->serviceConfigurationId),
                    payload->scid);
        sc_stat->statistics = stat;
        sc_stat->statisticsCount = 1;

        res->xboxUserId = payload->xuid;
        res->serviceConfigStatistics = sc_stat;
        res->serviceConfigStatisticsCount = 1;
        return XBL_S_OK;
    };

    return RecordHr("XblUserStatisticsGetSingleUserStatisticAsync",
                    StartXsapiAsync(async, kIdStatsGet, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblUserStatisticsGetSingleUserStatisticResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdStatsGet, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticResult", hr);
    *ptrToBuffer = static_cast<XblUserStatisticsResult*>(buffer);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblUserStatisticsGetSingleUserStatisticResult", XBL_S_OK);
}

// ===========================================================================
// Expanded Core / Lifecycle / Context Settings / Events
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblMemGetFunctions(
    XblMemAllocFunction* memAllocFunc,
    XblMemFreeFunction* memFreeFunc) {
    if (!memAllocFunc || !memFreeFunc) {
        return RecordHr("XblMemGetFunctions", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *memAllocFunc = s.mem_alloc;
    *memFreeFunc = s.mem_free;
    return RecordHr("XblMemGetFunctions", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblWrapper_XblInitialize(
    const char* scid,
    XTaskQueueHandle internalWorkQueue) {
    XblInitArgs args{};
    args.queue = internalWorkQueue;
    args.scid = scid;
    const XGameResult hr = XblInitialize(&args);
    return RecordHr("XblWrapper_XblInitialize", hr);
}

XTaskQueueHandle OPENSCARLETT_XSAPI_CALL XblGetAsyncQueue(void) {
    RecordOk("XblGetAsyncQueue");
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.default_queue;
}

XGameResult OPENSCARLETT_XSAPI_CALL XblGetScid(const char** scid) {
    if (!scid) {
        return RecordHr("XblGetScid", XGR_E_INVALIDARG);
    }
    *scid = nullptr;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblGetScid", XBL_E_NOT_INITIALIZED);
    }
    *scid = s.scid.c_str();
    return RecordHr("XblGetScid", XBL_S_OK);
}

XblErrorCondition OPENSCARLETT_XSAPI_CALL XblGetErrorCondition(XGameResult hr) {
    RecordOk("XblGetErrorCondition");
    if (hr >= 0) return XblErrorCondition_NoError;
    if (hr == XBL_E_HTTP_STATUS_404_NOT_FOUND || hr == XGR_E_NOT_FOUND) {
        return XblErrorCondition_Http404NotFound;
    }
    if (hr == XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED) {
        return XblErrorCondition_Http412PreconditionFailed;
    }
    if (hr == XBL_E_HTTP_STATUS_429_TOO_MANY_REQUESTS) {
        return XblErrorCondition_Http429TooManyRequests;
    }
    if (hr == XGR_E_INVALIDARG || hr == XGR_E_NOT_SUFFICIENT_BUFFER) {
        return XblErrorCondition_GenericOutOfRange;
    }
    if (hr == XGR_E_ACCESSDENIED ||
        hr == kHrGameUserSignedOut ||
        hr == kHrGameUserResolveUserIssueRequired ||
        hr == kHrGameUserDeferralNotAvailable ||
        hr == kHrGameUserNoTokenRequired ||
        hr == kHrXblAuthNoToken) {
        return XblErrorCondition_Auth;
    }
    return XblErrorCondition_GenericError;
}

XblFunctionContext OPENSCARLETT_XSAPI_CALL XblAddServiceCallRoutedHandler(
    XblCallRoutedHandler handler,
    void* context) {
    if (!handler) {
        RecordHr("XblAddServiceCallRoutedHandler", XGR_E_INVALIDARG);
        return 0;
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    const XblFunctionContext token = s.next_handler_token++;
    s.call_routed_handlers[token] = {handler, context};
    RecordOk("XblAddServiceCallRoutedHandler");
    return token;
}

void OPENSCARLETT_XSAPI_CALL XblRemoveServiceCallRoutedHandler(XblFunctionContext token) {
    RecordOk("XblRemoveServiceCallRoutedHandler");
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.call_routed_handlers.erase(token);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSetOverrideConfiguration(const char* overrideLocale) {
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.override_locale = overrideLocale ? overrideLocale : "";
    return RecordHr("XblSetOverrideConfiguration", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblDisableAssertsForXboxLiveThrottlingInDevSandboxes(
    XblConfigSetting /*setting*/) {
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.disable_throttling_asserts = true;
    RecordOk("XblDisableAssertsForXboxLiveThrottlingInDevSandboxes");
}

XGameResult OPENSCARLETT_XSAPI_CALL XblEventsWriteInGameEvent(
    XblContextHandle xboxLiveContext,
    const char* eventName,
    const char* dimensionsJson,
    const char* measurementsJson) {
    if (!xboxLiveContext || !eventName || !*eventName) {
        return RecordHr("XblEventsWriteInGameEvent", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblEventsWriteInGameEvent", XGR_E_HANDLE);

    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) return RecordHr("XblEventsWriteInGameEvent", XBL_E_NOT_INITIALIZED);
    std::string record = std::string(eventName) + "|" +
                         (dimensionsJson ? dimensionsJson : "{}") + "|" +
                         (measurementsJson ? measurementsJson : "{}");
    if (s.recorded_in_game_events.size() < 1024u) {
        s.recorded_in_game_events.push_back(std::move(record));
    }
    return RecordHr("XblEventsWriteInGameEvent", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetHttpRetryDelay(
    XblContextHandle context, uint32_t* delayInSeconds) {
    if (!context || !delayInSeconds) {
        return RecordHr("XblContextSettingsGetHttpRetryDelay", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsGetHttpRetryDelay", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *delayInSeconds = ctx->http_retry_delay_sec;
    return RecordHr("XblContextSettingsGetHttpRetryDelay", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetHttpRetryDelay(
    XblContextHandle context, uint32_t delayInSeconds) {
    if (!context) {
        return RecordHr("XblContextSettingsSetHttpRetryDelay", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsSetHttpRetryDelay", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    ctx->http_retry_delay_sec = delayInSeconds;
    return RecordHr("XblContextSettingsSetHttpRetryDelay", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetHttpTimeoutWindow(
    XblContextHandle context, uint32_t* timeoutWindowInSeconds) {
    if (!context || !timeoutWindowInSeconds) {
        return RecordHr("XblContextSettingsGetHttpTimeoutWindow", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsGetHttpTimeoutWindow", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *timeoutWindowInSeconds = ctx->http_timeout_window_sec;
    return RecordHr("XblContextSettingsGetHttpTimeoutWindow", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetHttpTimeoutWindow(
    XblContextHandle context, uint32_t timeoutWindowInSeconds) {
    if (!context) {
        return RecordHr("XblContextSettingsSetHttpTimeoutWindow", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsSetHttpTimeoutWindow", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    ctx->http_timeout_window_sec = timeoutWindowInSeconds;
    return RecordHr("XblContextSettingsSetHttpTimeoutWindow", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetLongHttpTimeout(
    XblContextHandle context, uint32_t* timeoutInSeconds) {
    if (!context || !timeoutInSeconds) {
        return RecordHr("XblContextSettingsGetLongHttpTimeout", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsGetLongHttpTimeout", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *timeoutInSeconds = ctx->long_http_timeout_sec;
    return RecordHr("XblContextSettingsGetLongHttpTimeout", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetLongHttpTimeout(
    XblContextHandle context, uint32_t timeoutInSeconds) {
    if (!context) {
        return RecordHr("XblContextSettingsSetLongHttpTimeout", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsSetLongHttpTimeout", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    ctx->long_http_timeout_sec = timeoutInSeconds;
    return RecordHr("XblContextSettingsSetLongHttpTimeout", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetUseCrossPlatformQosServers(
    XblContextHandle context, bool* value) {
    if (!context || !value) {
        return RecordHr("XblContextSettingsGetUseCrossPlatformQosServers", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsGetUseCrossPlatformQosServers", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *value = ctx->use_cross_platform_qos;
    return RecordHr("XblContextSettingsGetUseCrossPlatformQosServers", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetUseCrossPlatformQosServers(
    XblContextHandle context, bool value) {
    if (!context) {
        return RecordHr("XblContextSettingsSetUseCrossPlatformQosServers", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsSetUseCrossPlatformQosServers", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    ctx->use_cross_platform_qos = value;
    return RecordHr("XblContextSettingsSetUseCrossPlatformQosServers", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsGetWebsocketTimeoutWindow(
    XblContextHandle context, uint32_t* timeoutWindowInSeconds) {
    if (!context || !timeoutWindowInSeconds) {
        return RecordHr("XblContextSettingsGetWebsocketTimeoutWindow", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsGetWebsocketTimeoutWindow", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *timeoutWindowInSeconds = ctx->websocket_timeout_window_sec;
    return RecordHr("XblContextSettingsGetWebsocketTimeoutWindow", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblContextSettingsSetWebsocketTimeoutWindow(
    XblContextHandle context, uint32_t timeoutWindowInSeconds) {
    if (!context) {
        return RecordHr("XblContextSettingsSetWebsocketTimeoutWindow", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(context);
    if (!ctx) return RecordHr("XblContextSettingsSetWebsocketTimeoutWindow", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    ctx->websocket_timeout_window_sec = timeoutWindowInSeconds;
    return RecordHr("XblContextSettingsSetWebsocketTimeoutWindow", XBL_S_OK);
}

// ===========================================================================
// Expanded Achievements (Single + Pagination)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* achievementId,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !achievementId || !*achievementId || !async) {
        return RecordHr("XblAchievementsGetAchievementAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblAchievementsGetAchievementAsync", XGR_E_HANDLE);

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string ach_id = achievementId;

    auto result_holder = std::make_shared<XblAchievementsResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuid, target_scid, ach_id, result_holder](
                      XsapiAsyncOp& self) -> XGameResult {
        std::vector<StoredAchievement> items;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureAchievementsLoadedLocked(s, target_scid, target_xuid);
            const auto it = s.achievements_cache.find(StoreKey{target_scid, target_xuid});
            if (it != s.achievements_cache.end()) {
                auto ait = it->second.find(ach_id);
                if (ait != it->second.end()) {
                    items.push_back(ait->second);
                }
            }
        }
        if (items.empty()) {
            StoredAchievement def{};
            def.id = ach_id;
            def.scid = target_scid;
            def.title_id = CurrentTitleId();
            def.name = "Achievement " + ach_id;
            def.unlocked_desc = "Unlocked achievement " + ach_id;
            def.locked_desc = "Complete achievement " + ach_id;
            def.percent_complete = 0u;
            def.gamerscore = 25u;
            def.type = XblAchievementType_Persistent;
            items.push_back(std::move(def));
        }

        auto res = BuildAchievementsResult(
            items, false, target_scid, target_xuid, CurrentTitleId(),
            XblAchievementType_All, false, XblAchievementOrderBy_DefaultOrder, 1u);
        XblAchievementsResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ach_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblAchievementsResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblAchievementsResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblAchievementsResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblAchievementsResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblAchievementsGetAchievementAsync",
                    StartXsapiAsync(async, kIdAchGetSingle, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsGetAchievementResult(
    XAsyncBlock* async, XblAchievementsResultHandle* result) {
    if (!async || !result) {
        return RecordHr("XblAchievementsGetAchievementResult", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblAchievementsGetAchievementResult", hr);
    hr = XAsyncGetResult(async, kIdAchGetSingle, sizeof(XblAchievementsResultHandle), result, nullptr);
    return RecordHr("XblAchievementsGetAchievementResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultGetNextAsync(
    XblAchievementsResultHandle resultHandle, uint32_t maxItems, XAsyncBlock* async) {
    if (!resultHandle || !async) {
        return RecordHr("XblAchievementsResultGetNextAsync", XGR_E_INVALIDARG);
    }
    auto prev = LookupAchResult(resultHandle);
    if (!prev) return RecordHr("XblAchievementsResultGetNextAsync", XGR_E_HANDLE);

    auto result_holder = std::make_shared<XblAchievementsResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [prev, maxItems, result_holder](XsapiAsyncOp& self) -> XGameResult {
        std::vector<StoredAchievement> filtered;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureAchievementsLoadedLocked(s, prev->query_scid, prev->query_xuid);
            const auto it = s.achievements_cache.find(StoreKey{prev->query_scid, prev->query_xuid});
            if (it != s.achievements_cache.end()) {
                for (const auto& [id, ach] : it->second) {
                    if (prev->query_title_id != 0 && ach.title_id != prev->query_title_id) continue;
                    if (prev->query_type == XblAchievementType_Persistent &&
                        ach.type != XblAchievementType_Persistent) continue;
                    if (prev->query_type == XblAchievementType_Challenge &&
                        ach.type != XblAchievementType_Challenge) continue;
                    if (prev->query_unlocked_only && ach.percent_complete < 100u) continue;
                    filtered.push_back(ach);
                }
            }
        }

        if (prev->query_order_by == XblAchievementOrderBy_TitleId) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.title_id != b.title_id) return a.title_id < b.title_id;
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else if (prev->query_order_by == XblAchievementOrderBy_UnlockTime) {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          if (a.time_unlocked != b.time_unlocked) return a.time_unlocked > b.time_unlocked;
                          return CompareAchievementIds(a.id, b.id);
                      });
        } else {
            std::sort(filtered.begin(), filtered.end(),
                      [](const StoredAchievement& a, const StoredAchievement& b) {
                          return CompareAchievementIds(a.id, b.id);
                      });
        }

        const size_t total = filtered.size();
        const size_t start = std::min<size_t>(prev->next_skip, total);
        size_t count = total - start;
        if (maxItems > 0 && count > maxItems) count = maxItems;
        const bool has_next = (start + count) < total;

        std::vector<StoredAchievement> slice(
            filtered.begin() + static_cast<std::ptrdiff_t>(start),
            filtered.begin() + static_cast<std::ptrdiff_t>(start + count));

        auto res = BuildAchievementsResult(
            slice, has_next, prev->query_scid, prev->query_xuid, prev->query_title_id,
            prev->query_type, prev->query_unlocked_only, prev->query_order_by,
            static_cast<uint32_t>(start + count));

        XblAchievementsResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ach_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblAchievementsResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblAchievementsResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblAchievementsResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblAchievementsResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblAchievementsResultGetNextAsync",
                    StartXsapiAsync(async, kIdAchGetNext, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblAchievementsResultGetNextResult(
    XAsyncBlock* async, XblAchievementsResultHandle* result) {
    if (!async || !result) {
        return RecordHr("XblAchievementsResultGetNextResult", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblAchievementsResultGetNextResult", hr);
    hr = XAsyncGetResult(async, kIdAchGetNext, sizeof(XblAchievementsResultHandle), result, nullptr);
    return RecordHr("XblAchievementsResultGetNextResult", hr);
}

// ===========================================================================
// Expanded Profile (Multiple Users & Social Group)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesAsync(
    XblContextHandle xboxLiveContext,
    const uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || (!xboxUserIds && xboxUserIdsCount > 0)) {
        return RecordHr("XblProfileGetUserProfilesAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblProfileGetUserProfilesAsync", XGR_E_HANDLE);

    std::vector<uint64_t> xuids;
    if (xboxUserIds && xboxUserIdsCount > 0) {
        xuids.assign(xboxUserIds, xboxUserIds + xboxUserIdsCount);
    }
    auto profiles_holder = std::make_shared<std::vector<XblUserProfile>>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, xuids = std::move(xuids), profiles_holder](
                      XsapiAsyncOp& self) -> XGameResult {
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        }
        profiles_holder->reserve(xuids.size());
        for (uint64_t x : xuids) {
            profiles_holder->push_back(BuildUserProfileForXuid(ctx, x != 0 ? x : ctx->xuid));
        }
        self.required_buffer_size = profiles_holder->size() * sizeof(XblUserProfile);
        return XBL_S_OK;
    };

    op->get_result_fn = [profiles_holder](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t needed = profiles_holder->size() * sizeof(XblUserProfile);
        if (needed == 0u) return XBL_S_OK;
        if (!buffer || buffer_size < needed) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(buffer, profiles_holder->data(), needed);
        return XBL_S_OK;
    };

    return RecordHr("XblProfileGetUserProfilesAsync",
                    StartXsapiAsync(async, kIdProfileGetMulti, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesResultCount(
    XAsyncBlock* async, size_t* profileCount) {
    if (!async || !profileCount) {
        return RecordHr("XblProfileGetUserProfilesResultCount", XGR_E_INVALIDARG);
    }
    *profileCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesResultCount", hr);
    *profileCount = sz / sizeof(XblUserProfile);
    return RecordHr("XblProfileGetUserProfilesResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesResult(
    XAsyncBlock* async, size_t profilesCount, XblUserProfile* profiles) {
    if (!async || (!profiles && profilesCount > 0)) {
        return RecordHr("XblProfileGetUserProfilesResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesResult", hr);
    if (sz == 0u) return RecordHr("XblProfileGetUserProfilesResult", XBL_S_OK);
    hr = XAsyncGetResult(
        async, kIdProfileGetMulti, profilesCount * sizeof(XblUserProfile), profiles, nullptr);
    return RecordHr("XblProfileGetUserProfilesResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesForSocialGroupAsync(
    XblContextHandle xboxLiveContext, const char* socialGroup, XAsyncBlock* async) {
    if (!xboxLiveContext || !socialGroup || !*socialGroup || !async) {
        return RecordHr("XblProfileGetUserProfilesForSocialGroupAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblProfileGetUserProfilesForSocialGroupAsync", XGR_E_HANDLE);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = 0;
        return XBL_S_OK;
    };
    op->get_result_fn = [](void*, size_t) -> XGameResult { return XBL_S_OK; };

    return RecordHr("XblProfileGetUserProfilesForSocialGroupAsync",
                    StartXsapiAsync(async, kIdProfileGetGroup, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesForSocialGroupResultCount(
    XAsyncBlock* async, size_t* profileCount) {
    if (!async || !profileCount) {
        return RecordHr("XblProfileGetUserProfilesForSocialGroupResultCount", XGR_E_INVALIDARG);
    }
    *profileCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesForSocialGroupResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesForSocialGroupResultCount", hr);
    *profileCount = sz / sizeof(XblUserProfile);
    return RecordHr("XblProfileGetUserProfilesForSocialGroupResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblProfileGetUserProfilesForSocialGroupResult(
    XAsyncBlock* async, size_t profilesCount, XblUserProfile* profiles) {
    if (!async || (!profiles && profilesCount > 0)) {
        return RecordHr("XblProfileGetUserProfilesForSocialGroupResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesForSocialGroupResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblProfileGetUserProfilesForSocialGroupResult", hr);
    if (sz == 0u) return RecordHr("XblProfileGetUserProfilesForSocialGroupResult", XBL_S_OK);
    hr = XAsyncGetResult(
        async, kIdProfileGetGroup, profilesCount * sizeof(XblUserProfile), profiles, nullptr);
    return RecordHr("XblProfileGetUserProfilesForSocialGroupResult", hr);
}

// ===========================================================================
// Title Storage (Local + Persistent Blob Store)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetQuotaAsync(
    XblContextHandle xboxLiveContext,
    const char* serviceConfigurationId,
    XblTitleStorageType storageType,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblTitleStorageGetQuotaAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleStorageGetQuotaAsync", XGR_E_HANDLE);

    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    struct QuotaResultData {
        size_t used = 0;
        size_t quota = 256u * 1024u * 1024u;
    };
    auto holder = std::make_shared<QuotaResultData>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_scid, storageType, holder](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        size_t used = 0;
        for (const auto& [k, blob] : s.title_storage_blobs) {
            if (k.scid == target_scid &&
                k.storage_type == static_cast<uint32_t>(storageType) &&
                (storageType == XblTitleStorageType_GlobalStorage || k.xuid == ctx->xuid)) {
                used += blob.data.size();
            }
        }
        holder->used = used;
        holder->quota = 256u * 1024u * 1024u;
        self.required_buffer_size = sizeof(QuotaResultData);
        return XBL_S_OK;
    };

    op->get_result_fn = [holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(QuotaResultData)) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(buffer, holder.get(), sizeof(QuotaResultData));
        return XBL_S_OK;
    };

    return RecordHr("XblTitleStorageGetQuotaAsync",
                    StartXsapiAsync(async, kIdTsQuota, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetQuotaResult(
    XAsyncBlock* async, size_t* usedBytes, size_t* quotaBytes) {
    if (!async || !usedBytes || !quotaBytes) {
        return RecordHr("XblTitleStorageGetQuotaResult", XGR_E_INVALIDARG);
    }
    struct QuotaResultData {
        size_t used;
        size_t quota;
    } data{};
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblTitleStorageGetQuotaResult", hr);
    hr = XAsyncGetResult(async, kIdTsQuota, sizeof(data), &data, nullptr);
    if (hr < 0) return RecordHr("XblTitleStorageGetQuotaResult", hr);
    *usedBytes = data.used;
    *quotaBytes = data.quota;
    return RecordHr("XblTitleStorageGetQuotaResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetBlobMetadataAsync(
    XblContextHandle xboxLiveContext,
    const char* serviceConfigurationId,
    XblTitleStorageType storageType,
    const char* blobPath,
    uint64_t xboxUserId,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblTitleStorageGetBlobMetadataAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleStorageGetBlobMetadataAsync", XGR_E_HANDLE);

    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    const std::string prefix = blobPath ? std::string(blobPath) : std::string{};
    const uint64_t target_xuid = (storageType == XblTitleStorageType_GlobalStorage)
                                     ? 0ull
                                     : (xboxUserId != 0 ? xboxUserId : ctx->xuid);

    auto result_holder = std::make_shared<XblTitleStorageBlobMetadataResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [target_scid, storageType, prefix, target_xuid, skipItems, maxItems,
                   result_holder](XsapiAsyncOp& self) -> XGameResult {
        std::vector<XblTitleStorageBlobMetadata> matched;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            for (const auto& [k, blob] : s.title_storage_blobs) {
                if (k.scid != target_scid) continue;
                if (k.storage_type != static_cast<uint32_t>(storageType)) continue;
                if (storageType != XblTitleStorageType_GlobalStorage && k.xuid != target_xuid) {
                    continue;
                }
                if (!prefix.empty() && k.blob_path.rfind(prefix, 0) != 0) continue;
                matched.push_back(blob.metadata);
            }
        }

        const size_t total = matched.size();
        const size_t start = std::min<size_t>(skipItems, total);
        size_t count = total - start;
        if (maxItems > 0 && count > maxItems) count = maxItems;
        const bool has_next = (start + count) < total;

        auto res = std::make_shared<XblTitleStorageBlobMetadataResult>();
        res->has_next = has_next;
        res->query_scid = target_scid;
        res->query_storage_type = storageType;
        res->query_blob_path = prefix;
        res->query_xuid = target_xuid;
        res->next_skip = static_cast<uint32_t>(start + count);
        res->items.assign(
            matched.begin() + static_cast<std::ptrdiff_t>(start),
            matched.begin() + static_cast<std::ptrdiff_t>(start + count));

        XblTitleStorageBlobMetadataResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ts_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblTitleStorageBlobMetadataResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblTitleStorageBlobMetadataResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblTitleStorageBlobMetadataResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblTitleStorageBlobMetadataResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblTitleStorageGetBlobMetadataAsync",
                    StartXsapiAsync(async, kIdTsMeta, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageGetBlobMetadataResult(
    XAsyncBlock* async, XblTitleStorageBlobMetadataResultHandle* result) {
    if (!async || !result) {
        return RecordHr("XblTitleStorageGetBlobMetadataResult", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblTitleStorageGetBlobMetadataResult", hr);
    hr = XAsyncGetResult(
        async, kIdTsMeta, sizeof(XblTitleStorageBlobMetadataResultHandle), result, nullptr);
    return RecordHr("XblTitleStorageGetBlobMetadataResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultGetItems(
    XblTitleStorageBlobMetadataResultHandle resultHandle,
    const XblTitleStorageBlobMetadata** items,
    size_t* itemsCount) {
    if (!resultHandle || !items || !itemsCount) {
        return RecordHr("XblTitleStorageBlobMetadataResultGetItems", XGR_E_INVALIDARG);
    }
    auto res = LookupTsResult(resultHandle);
    if (!res) return RecordHr("XblTitleStorageBlobMetadataResultGetItems", XGR_E_HANDLE);
    *items = res->items.empty() ? nullptr : res->items.data();
    *itemsCount = res->items.size();
    return RecordHr("XblTitleStorageBlobMetadataResultGetItems", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultHasNext(
    XblTitleStorageBlobMetadataResultHandle resultHandle, bool* hasNext) {
    if (!resultHandle || !hasNext) {
        return RecordHr("XblTitleStorageBlobMetadataResultHasNext", XGR_E_INVALIDARG);
    }
    auto res = LookupTsResult(resultHandle);
    if (!res) return RecordHr("XblTitleStorageBlobMetadataResultHasNext", XGR_E_HANDLE);
    *hasNext = res->has_next;
    return RecordHr("XblTitleStorageBlobMetadataResultHasNext", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultGetNextAsync(
    XblTitleStorageBlobMetadataResultHandle resultHandle,
    uint32_t maxItems,
    XAsyncBlock* async) {
    if (!resultHandle || !async) {
        return RecordHr("XblTitleStorageBlobMetadataResultGetNextAsync", XGR_E_INVALIDARG);
    }
    auto prev = LookupTsResult(resultHandle);
    if (!prev) return RecordHr("XblTitleStorageBlobMetadataResultGetNextAsync", XGR_E_HANDLE);

    auto result_holder = std::make_shared<XblTitleStorageBlobMetadataResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [prev, maxItems, result_holder](XsapiAsyncOp& self) -> XGameResult {
        std::vector<XblTitleStorageBlobMetadata> matched;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            for (const auto& [k, blob] : s.title_storage_blobs) {
                if (k.scid != prev->query_scid) continue;
                if (k.storage_type != static_cast<uint32_t>(prev->query_storage_type)) continue;
                if (prev->query_storage_type != XblTitleStorageType_GlobalStorage &&
                    k.xuid != prev->query_xuid) {
                    continue;
                }
                if (!prev->query_blob_path.empty() &&
                    k.blob_path.rfind(prev->query_blob_path, 0) != 0) {
                    continue;
                }
                matched.push_back(blob.metadata);
            }
        }

        const size_t total = matched.size();
        const size_t start = std::min<size_t>(prev->next_skip, total);
        size_t count = total - start;
        if (maxItems > 0 && count > maxItems) count = maxItems;
        const bool has_next = (start + count) < total;

        auto res = std::make_shared<XblTitleStorageBlobMetadataResult>();
        res->has_next = has_next;
        res->query_scid = prev->query_scid;
        res->query_storage_type = prev->query_storage_type;
        res->query_blob_path = prev->query_blob_path;
        res->query_xuid = prev->query_xuid;
        res->next_skip = static_cast<uint32_t>(start + count);
        res->items.assign(
            matched.begin() + static_cast<std::ptrdiff_t>(start),
            matched.begin() + static_cast<std::ptrdiff_t>(start + count));

        XblTitleStorageBlobMetadataResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            s.live_ts_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblTitleStorageBlobMetadataResultHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblTitleStorageBlobMetadataResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblTitleStorageBlobMetadataResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblTitleStorageBlobMetadataResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };

    return RecordHr("XblTitleStorageBlobMetadataResultGetNextAsync",
                    StartXsapiAsync(async, kIdTsMetaNext, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultGetNextResult(
    XAsyncBlock* async, XblTitleStorageBlobMetadataResultHandle* result) {
    if (!async || !result) {
        return RecordHr("XblTitleStorageBlobMetadataResultGetNextResult", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblTitleStorageBlobMetadataResultGetNextResult", hr);
    hr = XAsyncGetResult(
        async, kIdTsMetaNext, sizeof(XblTitleStorageBlobMetadataResultHandle), result, nullptr);
    return RecordHr("XblTitleStorageBlobMetadataResultGetNextResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultDuplicateHandle(
    XblTitleStorageBlobMetadataResultHandle handle,
    XblTitleStorageBlobMetadataResultHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XblTitleStorageBlobMetadataResultDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto res = LookupTsResult(handle);
    if (!res) return RecordHr("XblTitleStorageBlobMetadataResultDuplicateHandle", XGR_E_HANDLE);
    res->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XblTitleStorageBlobMetadataResultDuplicateHandle", XBL_S_OK);
}

void OPENSCARLETT_XSAPI_CALL XblTitleStorageBlobMetadataResultCloseHandle(
    XblTitleStorageBlobMetadataResultHandle handle) {
    RecordOk("XblTitleStorageBlobMetadataResultCloseHandle");
    if (!handle) return;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_ts_results.find(handle);
    if (it == s.live_ts_results.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        s.live_ts_results.erase(it);
    }
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageUploadBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    const uint8_t* blobBuffer,
    size_t blobBufferCount,
    XblTitleStorageETagMatchCondition etagMatchCondition,
    size_t /*preferredUploadBlockSize*/,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || blobMetadata.blobPath[0] == '\0' ||
        (!blobBuffer && blobBufferCount > 0)) {
        return RecordHr("XblTitleStorageUploadBlobAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleStorageUploadBlobAsync", XGR_E_HANDLE);

    std::vector<uint8_t> bytes;
    if (blobBuffer && blobBufferCount > 0) {
        bytes.assign(blobBuffer, blobBuffer + blobBufferCount);
    }
    auto meta_out = std::make_shared<XblTitleStorageBlobMetadata>(blobMetadata);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, bytes = std::move(bytes), etagMatchCondition, meta_out](
                      XsapiAsyncOp& self) -> XGameResult {
        const std::string target_scid = (meta_out->serviceConfigurationId[0] != '\0')
                                            ? std::string(meta_out->serviceConfigurationId)
                                            : ctx->scid;
        const uint64_t target_xuid =
            (meta_out->storageType == XblTitleStorageType_GlobalStorage)
                ? 0ull
                : (meta_out->xboxUserId != 0 ? meta_out->xboxUserId : ctx->xuid);

        TitleStorageKey key{
            target_scid,
            target_xuid,
            static_cast<uint32_t>(meta_out->storageType),
            std::string(meta_out->blobPath)};

        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;

        auto existing = s.title_storage_blobs.find(key);
        if (etagMatchCondition == XblTitleStorageETagMatchCondition_IfMatch) {
            if (existing == s.title_storage_blobs.end() ||
                std::strcmp(existing->second.metadata.eTag, meta_out->eTag) != 0) {
                return XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED;
            }
        } else if (etagMatchCondition == XblTitleStorageETagMatchCondition_IfNotMatch) {
            if (existing != s.title_storage_blobs.end() &&
                (meta_out->eTag[0] == '\0' ||
                 std::strcmp(existing->second.metadata.eTag, meta_out->eTag) == 0)) {
                return XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED;
            }
        }

        CopyCString(meta_out->serviceConfigurationId, sizeof(meta_out->serviceConfigurationId),
                    target_scid);
        meta_out->xboxUserId = target_xuid;
        meta_out->length = bytes.size();
        meta_out->clientTimestamp = NowUnixSeconds();
        const uint64_t seq = s.next_etag_seq++;
        const std::string new_etag = "\"os-etag-" + std::to_string(seq) + "-" +
                                     std::to_string(bytes.size()) + "\"";
        CopyCString(meta_out->eTag, sizeof(meta_out->eTag), new_etag);
        if (meta_out->blobType == XblTitleStorageBlobType_Unknown) {
            meta_out->blobType = XblTitleStorageBlobType_Binary;
        }

        StoredBlob sb{};
        sb.metadata = *meta_out;
        sb.data = bytes;
        s.title_storage_blobs[key] = std::move(sb);

        std::string disk_str(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        (void)WriteFileAtomically(TitleStorageBlobDiskPath(key), disk_str);

        self.required_buffer_size = sizeof(XblTitleStorageBlobMetadata);
        return XBL_S_OK;
    };

    op->get_result_fn = [meta_out](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblTitleStorageBlobMetadata)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memcpy(buffer, meta_out.get(), sizeof(XblTitleStorageBlobMetadata));
        return XBL_S_OK;
    };

    return RecordHr("XblTitleStorageUploadBlobAsync",
                    StartXsapiAsync(async, kIdTsUpload, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageUploadBlobResult(
    XAsyncBlock* async, XblTitleStorageBlobMetadata* blobMetadata) {
    if (!async || !blobMetadata) {
        return RecordHr("XblTitleStorageUploadBlobResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblTitleStorageUploadBlobResult", hr);
    hr = XAsyncGetResult(
        async, kIdTsUpload, sizeof(XblTitleStorageBlobMetadata), blobMetadata, nullptr);
    return RecordHr("XblTitleStorageUploadBlobResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDownloadBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    uint8_t* blobBuffer,
    size_t blobBufferCount,
    XblTitleStorageETagMatchCondition etagMatchCondition,
    const char* /*selectQuery*/,
    size_t /*preferredDownloadBlockSize*/,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || blobMetadata.blobPath[0] == '\0' ||
        (!blobBuffer && blobBufferCount > 0)) {
        return RecordHr("XblTitleStorageDownloadBlobAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleStorageDownloadBlobAsync", XGR_E_HANDLE);

    auto meta_out = std::make_shared<XblTitleStorageBlobMetadata>(blobMetadata);
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, blobBuffer, blobBufferCount, etagMatchCondition, meta_out](
                      XsapiAsyncOp& self) -> XGameResult {
        const std::string target_scid = (meta_out->serviceConfigurationId[0] != '\0')
                                            ? std::string(meta_out->serviceConfigurationId)
                                            : ctx->scid;
        const uint64_t target_xuid =
            (meta_out->storageType == XblTitleStorageType_GlobalStorage)
                ? 0ull
                : (meta_out->xboxUserId != 0 ? meta_out->xboxUserId : ctx->xuid);

        TitleStorageKey key{
            target_scid,
            target_xuid,
            static_cast<uint32_t>(meta_out->storageType),
            std::string(meta_out->blobPath)};

        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;

        auto it = s.title_storage_blobs.find(key);
        if (it == s.title_storage_blobs.end()) {
            return XBL_E_HTTP_STATUS_404_NOT_FOUND;
        }
        const StoredBlob& sb = it->second;
        if (etagMatchCondition == XblTitleStorageETagMatchCondition_IfMatch &&
            std::strcmp(sb.metadata.eTag, meta_out->eTag) != 0) {
            return XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED;
        }
        if (etagMatchCondition == XblTitleStorageETagMatchCondition_IfNotMatch &&
            std::strcmp(sb.metadata.eTag, meta_out->eTag) == 0) {
            return XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED;
        }
        if (blobBufferCount < sb.data.size()) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        if (!sb.data.empty() && blobBuffer) {
            std::memcpy(blobBuffer, sb.data.data(), sb.data.size());
        }
        *meta_out = sb.metadata;
        self.required_buffer_size = sizeof(XblTitleStorageBlobMetadata);
        return XBL_S_OK;
    };

    op->get_result_fn = [meta_out](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblTitleStorageBlobMetadata)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memcpy(buffer, meta_out.get(), sizeof(XblTitleStorageBlobMetadata));
        return XBL_S_OK;
    };

    return RecordHr("XblTitleStorageDownloadBlobAsync",
                    StartXsapiAsync(async, kIdTsDownload, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDownloadBlobResult(
    XAsyncBlock* async, XblTitleStorageBlobMetadata* blobMetadata) {
    if (!async || !blobMetadata) {
        return RecordHr("XblTitleStorageDownloadBlobResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblTitleStorageDownloadBlobResult", hr);
    hr = XAsyncGetResult(
        async, kIdTsDownload, sizeof(XblTitleStorageBlobMetadata), blobMetadata, nullptr);
    return RecordHr("XblTitleStorageDownloadBlobResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleStorageDeleteBlobAsync(
    XblContextHandle xboxLiveContext,
    XblTitleStorageBlobMetadata blobMetadata,
    bool deleteOnlyIfEtagMatches,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || blobMetadata.blobPath[0] == '\0') {
        return RecordHr("XblTitleStorageDeleteBlobAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblTitleStorageDeleteBlobAsync", XGR_E_HANDLE);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [ctx, blobMetadata, deleteOnlyIfEtagMatches](XsapiAsyncOp&) -> XGameResult {
        const std::string target_scid = (blobMetadata.serviceConfigurationId[0] != '\0')
                                            ? std::string(blobMetadata.serviceConfigurationId)
                                            : ctx->scid;
        const uint64_t target_xuid =
            (blobMetadata.storageType == XblTitleStorageType_GlobalStorage)
                ? 0ull
                : (blobMetadata.xboxUserId != 0 ? blobMetadata.xboxUserId : ctx->xuid);

        TitleStorageKey key{
            target_scid,
            target_xuid,
            static_cast<uint32_t>(blobMetadata.storageType),
            std::string(blobMetadata.blobPath)};

        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;

        auto it = s.title_storage_blobs.find(key);
        if (it == s.title_storage_blobs.end()) {
            return XBL_E_HTTP_STATUS_404_NOT_FOUND;
        }
        if (deleteOnlyIfEtagMatches &&
            std::strcmp(it->second.metadata.eTag, blobMetadata.eTag) != 0) {
            return XBL_E_HTTP_STATUS_412_PRECONDITION_FAILED;
        }
        s.title_storage_blobs.erase(it);
        std::error_code ec;
        fs::remove(TitleStorageBlobDiskPath(key), ec);
        return XBL_S_OK;
    };

    return RecordHr("XblTitleStorageDeleteBlobAsync",
                    StartXsapiAsync(async, kIdTsDelete, std::move(op)));
}

// ===========================================================================
// Privacy (Offline Permissive Defaults)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListAsync(
    XblContextHandle xboxLiveContext, XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPrivacyGetAvoidListAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPrivacyGetAvoidListAsync", XGR_E_HANDLE);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = 0;
        return XBL_S_OK;
    };
    op->get_result_fn = [](void*, size_t) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblPrivacyGetAvoidListAsync",
                    StartXsapiAsync(async, kIdPrivacyAvoid, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListResultCount(
    XAsyncBlock* async, size_t* xuidCount) {
    if (!async || !xuidCount) {
        return RecordHr("XblPrivacyGetAvoidListResultCount", XGR_E_INVALIDARG);
    }
    *xuidCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyGetAvoidListResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPrivacyGetAvoidListResultCount", hr);
    *xuidCount = sz / sizeof(uint64_t);
    return RecordHr("XblPrivacyGetAvoidListResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetAvoidListResult(
    XAsyncBlock* async, size_t xuidCount, uint64_t* xuids) {
    if (!async || (!xuids && xuidCount > 0)) {
        return RecordHr("XblPrivacyGetAvoidListResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyGetAvoidListResult", hr);
    return RecordHr("XblPrivacyGetAvoidListResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListAsync(
    XblContextHandle xboxLiveContext, XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPrivacyGetMuteListAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPrivacyGetMuteListAsync", XGR_E_HANDLE);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = 0;
        return XBL_S_OK;
    };
    op->get_result_fn = [](void*, size_t) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblPrivacyGetMuteListAsync",
                    StartXsapiAsync(async, kIdPrivacyMute, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListResultCount(
    XAsyncBlock* async, size_t* xuidCount) {
    if (!async || !xuidCount) {
        return RecordHr("XblPrivacyGetMuteListResultCount", XGR_E_INVALIDARG);
    }
    *xuidCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyGetMuteListResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPrivacyGetMuteListResultCount", hr);
    *xuidCount = sz / sizeof(uint64_t);
    return RecordHr("XblPrivacyGetMuteListResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyGetMuteListResult(
    XAsyncBlock* async, size_t xuidCount, uint64_t* xuids) {
    if (!async || (!xuids && xuidCount > 0)) {
        return RecordHr("XblPrivacyGetMuteListResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyGetMuteListResult", hr);
    return RecordHr("XblPrivacyGetMuteListResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    uint64_t targetXuid,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPrivacyCheckPermissionAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPrivacyCheckPermissionAsync", XGR_E_HANDLE);

    auto res_holder = std::make_shared<XblPermissionCheckResult>();
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [permissionToCheck, targetXuid, res_holder](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        res_holder->isAllowed = true;
        res_holder->targetXuid = targetXuid;
        res_holder->targetUserType = XblAnonymousUserType_Unknown;
        res_holder->permissionRequested = permissionToCheck;
        res_holder->reasons = nullptr;
        res_holder->reasonsCount = 0;
        self.required_buffer_size = sizeof(XblPermissionCheckResult);
        return XBL_S_OK;
    };
    op->get_result_fn = [res_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblPermissionCheckResult)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memcpy(buffer, res_holder.get(), sizeof(XblPermissionCheckResult));
        return XBL_S_OK;
    };
    return RecordHr("XblPrivacyCheckPermissionAsync",
                    StartXsapiAsync(async, kIdPrivacyCheck, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblPrivacyCheckPermissionResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblPrivacyCheckPermissionResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XblPrivacyCheckPermissionResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdPrivacyCheck, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionResult", hr);
    *ptrToBuffer = static_cast<XblPermissionCheckResult*>(buffer);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblPrivacyCheckPermissionResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionForAnonymousUserAsync(
    XblContextHandle xboxLiveContext,
    XblPermission permissionToCheck,
    XblAnonymousUserType userType,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblPrivacyCheckPermissionForAnonymousUserAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPrivacyCheckPermissionForAnonymousUserAsync", XGR_E_HANDLE);

    auto res_holder = std::make_shared<XblPermissionCheckResult>();
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [permissionToCheck, userType, res_holder](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        res_holder->isAllowed = true;
        res_holder->targetXuid = 0;
        res_holder->targetUserType = userType;
        res_holder->permissionRequested = permissionToCheck;
        res_holder->reasons = nullptr;
        res_holder->reasonsCount = 0;
        self.required_buffer_size = sizeof(XblPermissionCheckResult);
        return XBL_S_OK;
    };
    op->get_result_fn = [res_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblPermissionCheckResult)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        std::memcpy(buffer, res_holder.get(), sizeof(XblPermissionCheckResult));
        return XBL_S_OK;
    };
    return RecordHr("XblPrivacyCheckPermissionForAnonymousUserAsync",
                    StartXsapiAsync(async, kIdPrivacyCheckAnon, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionForAnonymousUserResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyCheckPermissionForAnonymousUserResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdPrivacyCheckAnon, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResult", hr);
    *ptrToBuffer = static_cast<XblPermissionCheckResult*>(buffer);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblPrivacyCheckPermissionForAnonymousUserResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyBatchCheckPermissionAsync(
    XblContextHandle xboxLiveContext,
    XblPermission* permissionsToCheck,
    size_t permissionsCount,
    uint64_t* targetXuids,
    size_t xuidsCount,
    XblAnonymousUserType* targetAnonymousUserTypes,
    size_t targetAnonymousUserTypesCount,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || !permissionsToCheck || permissionsCount == 0 ||
        (!targetXuids && xuidsCount > 0) ||
        (!targetAnonymousUserTypes && targetAnonymousUserTypesCount > 0)) {
        return RecordHr("XblPrivacyBatchCheckPermissionAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblPrivacyBatchCheckPermissionAsync", XGR_E_HANDLE);

    std::vector<XblPermission> perms(permissionsToCheck, permissionsToCheck + permissionsCount);
    std::vector<uint64_t> xuids;
    if (targetXuids && xuidsCount > 0) xuids.assign(targetXuids, targetXuids + xuidsCount);
    std::vector<XblAnonymousUserType> anons;
    if (targetAnonymousUserTypes && targetAnonymousUserTypesCount > 0) {
        anons.assign(targetAnonymousUserTypes,
                     targetAnonymousUserTypes + targetAnonymousUserTypesCount);
    }

    auto results_holder = std::make_shared<std::vector<XblPermissionCheckResult>>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [perms = std::move(perms), xuids = std::move(xuids), anons = std::move(anons),
                   results_holder](XsapiAsyncOp& self) -> XGameResult {
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        }
        for (uint64_t x : xuids) {
            for (XblPermission p : perms) {
                XblPermissionCheckResult r{};
                r.isAllowed = true;
                r.targetXuid = x;
                r.targetUserType = XblAnonymousUserType_Unknown;
                r.permissionRequested = p;
                results_holder->push_back(r);
            }
        }
        for (XblAnonymousUserType a : anons) {
            for (XblPermission p : perms) {
                XblPermissionCheckResult r{};
                r.isAllowed = true;
                r.targetXuid = 0;
                r.targetUserType = a;
                r.permissionRequested = p;
                results_holder->push_back(r);
            }
        }
        self.required_buffer_size = results_holder->size() * sizeof(XblPermissionCheckResult);
        return XBL_S_OK;
    };

    op->get_result_fn = [results_holder](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t needed = results_holder->size() * sizeof(XblPermissionCheckResult);
        if (needed == 0u) return XBL_S_OK;
        if (!buffer || buffer_size < needed) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(buffer, results_holder->data(), needed);
        return XBL_S_OK;
    };

    return RecordHr("XblPrivacyBatchCheckPermissionAsync",
                    StartXsapiAsync(async, kIdPrivacyBatch, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyBatchCheckPermissionResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblPrivacyBatchCheckPermissionResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyBatchCheckPermissionResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblPrivacyBatchCheckPermissionResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPrivacyBatchCheckPermissionResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblPermissionCheckResult** results,
    size_t* resultsCount,
    size_t* bufferUsed) {
    if (!async || !results || !resultsCount || (!buffer && bufferSize > 0)) {
        return RecordHr("XblPrivacyBatchCheckPermissionResult", XGR_E_INVALIDARG);
    }
    *results = nullptr;
    *resultsCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPrivacyBatchCheckPermissionResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPrivacyBatchCheckPermissionResult", hr);
    if (sz == 0u) {
        if (bufferUsed) *bufferUsed = 0;
        return RecordHr("XblPrivacyBatchCheckPermissionResult", XBL_S_OK);
    }
    if (!buffer || bufferSize < sz) {
        return RecordHr("XblPrivacyBatchCheckPermissionResult", XGR_E_NOT_SUFFICIENT_BUFFER);
    }
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdPrivacyBatch, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblPrivacyBatchCheckPermissionResult", hr);
    *results = static_cast<XblPermissionCheckResult*>(buffer);
    *resultsCount = used / sizeof(XblPermissionCheckResult);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblPrivacyBatchCheckPermissionResult", XBL_S_OK);
}

// ===========================================================================
// Expanded Social Relationships & Social Manager
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultGetTotalCount(
    XblSocialRelationshipResultHandle resultHandle, size_t* totalCount) {
    if (!resultHandle || !totalCount) {
        return RecordHr("XblSocialRelationshipResultGetTotalCount", XGR_E_INVALIDARG);
    }
    auto res = LookupSocialResult(resultHandle);
    if (!res) return RecordHr("XblSocialRelationshipResultGetTotalCount", XGR_E_HANDLE);
    *totalCount = res->total_count;
    return RecordHr("XblSocialRelationshipResultGetTotalCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultGetNextAsync(
    XblContextHandle xboxLiveContext,
    XblSocialRelationshipResultHandle resultHandle,
    size_t /*maxItems*/,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !resultHandle || !async) {
        return RecordHr("XblSocialRelationshipResultGetNextAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xboxLiveContext);
    if (!ctx) return RecordHr("XblSocialRelationshipResultGetNextAsync", XGR_E_HANDLE);
    auto prev = LookupSocialResult(resultHandle);
    if (!prev) return RecordHr("XblSocialRelationshipResultGetNextAsync", XGR_E_HANDLE);

    auto result_holder = std::make_shared<XblSocialRelationshipResultHandle>(nullptr);
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [result_holder](XsapiAsyncOp& self) -> XGameResult {
        auto res = std::make_shared<XblSocialRelationshipResult>();
        res->has_next = false;
        res->total_count = 0;
        XblSocialRelationshipResultHandle raw = res.get();
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            s.live_social_results[raw] = std::move(res);
        }
        *result_holder = raw;
        self.required_buffer_size = sizeof(XblSocialRelationshipResultHandle);
        return XBL_S_OK;
    };
    op->get_result_fn = [result_holder](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblSocialRelationshipResultHandle)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblSocialRelationshipResultHandle h = *result_holder;
        *result_holder = nullptr;
        std::memcpy(buffer, &h, sizeof(h));
        return XBL_S_OK;
    };
    op->cleanup_unclaimed_fn = [result_holder]() {
        if (*result_holder) {
            XblSocialRelationshipResultCloseHandle(*result_holder);
            *result_holder = nullptr;
        }
    };
    return RecordHr("XblSocialRelationshipResultGetNextAsync",
                    StartXsapiAsync(async, kIdSocialGetNext, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRelationshipResultGetNextResult(
    XAsyncBlock* async, XblSocialRelationshipResultHandle* handle) {
    if (!async || !handle) {
        return RecordHr("XblSocialRelationshipResultGetNextResult", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblSocialRelationshipResultGetNextResult", hr);
    hr = XAsyncGetResult(
        async, kIdSocialGetNext, sizeof(XblSocialRelationshipResultHandle), handle, nullptr);
    return RecordHr("XblSocialRelationshipResultGetNextResult", hr);
}

XblFunctionContext OPENSCARLETT_XSAPI_CALL XblSocialAddSocialRelationshipChangedHandler(
    XblContextHandle xboxLiveContext,
    XblSocialRelationshipChangedHandler handler,
    void* handlerContext) {
    if (!xboxLiveContext || !handler || !LookupContext(xboxLiveContext)) {
        RecordHr("XblSocialAddSocialRelationshipChangedHandler", XGR_E_INVALIDARG);
        return 0;
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    const XblFunctionContext token = s.next_handler_token++;
    s.social_changed_handlers[token] = {handler, handlerContext};
    RecordOk("XblSocialAddSocialRelationshipChangedHandler");
    return token;
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialRemoveSocialRelationshipChangedHandler(
    XblContextHandle xboxLiveContext, XblFunctionContext handlerContext) {
    if (!xboxLiveContext) {
        return RecordHr("XblSocialRemoveSocialRelationshipChangedHandler", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblSocialRemoveSocialRelationshipChangedHandler", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.social_changed_handlers.erase(handlerContext);
    return RecordHr("XblSocialRemoveSocialRelationshipChangedHandler", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialSubscribeToSocialRelationshipChange(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle) {
    if (!xboxLiveContext || !subscriptionHandle) {
        return RecordHr("XblSocialSubscribeToSocialRelationshipChange", XGR_E_INVALIDARG);
    }
    *subscriptionHandle = nullptr;
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblSocialSubscribeToSocialRelationshipChange", XGR_E_HANDLE);
    }
    auto sub = std::make_unique<XblRealTimeActivitySubscription>();
    sub->kind = "social";
    sub->xuid = xboxUserId;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblSocialSubscribeToSocialRelationshipChange", XBL_E_NOT_INITIALIZED);
    }
    sub->id = s.next_sub_id++;
    XblRealTimeActivitySubscriptionHandle raw = sub.get();
    s.live_subscriptions[raw] = std::move(sub);
    *subscriptionHandle = raw;
    return RecordHr("XblSocialSubscribeToSocialRelationshipChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialUnsubscribeFromSocialRelationshipChange(
    XblContextHandle xboxLiveContext, XblRealTimeActivitySubscriptionHandle subscriptionHandle) {
    if (!xboxLiveContext || !subscriptionHandle) {
        return RecordHr("XblSocialUnsubscribeFromSocialRelationshipChange", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblSocialUnsubscribeFromSocialRelationshipChange", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.live_subscriptions.erase(subscriptionHandle);
    return RecordHr("XblSocialUnsubscribeFromSocialRelationshipChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialSubmitReputationFeedbackAsync(
    XblContextHandle xboxLiveContext,
    uint64_t /*xboxUserId*/,
    XblReputationFeedbackType /*reputationFeedbackType*/,
    const char* /*sessionName*/,
    const char* /*reasonMessage*/,
    const char* /*evidenceResourceId*/,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async) {
        return RecordHr("XblSocialSubmitReputationFeedbackAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblSocialSubmitReputationFeedbackAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        return s.initialized ? XBL_S_OK : XBL_E_NOT_INITIALIZED;
    };
    return RecordHr("XblSocialSubmitReputationFeedbackAsync",
                    StartXsapiAsync(async, kIdSocialRep, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialSubmitBatchReputationFeedbackAsync(
    XblContextHandle xboxLiveContext,
    const XblReputationFeedbackItem* feedbackItems,
    size_t feedbackItemsCount,
    XAsyncBlock* async) {
    if (!xboxLiveContext || !async || (!feedbackItems && feedbackItemsCount > 0)) {
        return RecordHr("XblSocialSubmitBatchReputationFeedbackAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblSocialSubmitBatchReputationFeedbackAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        return s.initialized ? XBL_S_OK : XBL_E_NOT_INITIALIZED;
    };
    return RecordHr("XblSocialSubmitBatchReputationFeedbackAsync",
                    StartXsapiAsync(async, kIdSocialBatchRep, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerAddLocalUser(
    XUserHandle user,
    XblSocialManagerExtraDetailLevel extraLevelDetail,
    XTaskQueueHandle /*queue*/) {
    if (!user) {
        return RecordHr("XblSocialManagerAddLocalUser", XGR_E_INVALIDARG);
    }
    uint64_t xuid = 0;
    XGameResult hr = XUserGetId(user, &xuid);
    if (hr < 0) return RecordHr("XblSocialManagerAddLocalUser", hr);

    XUserHandle dup = nullptr;
    hr = XUserDuplicateHandle(user, &dup);
    if (hr < 0) return RecordHr("XblSocialManagerAddLocalUser", hr);

    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        XUserCloseHandle(dup);
        return RecordHr("XblSocialManagerAddLocalUser", XBL_E_NOT_INITIALIZED);
    }
    for (const auto& existing : s.sm_local_users) {
        if (existing.xuid == xuid) {
            XUserCloseHandle(dup);
            return RecordHr("XblSocialManagerAddLocalUser", XBL_S_OK);
        }
    }
    SocialManagerLocalUser entry{};
    entry.user = dup;
    entry.xuid = xuid;
    entry.extra_detail = extraLevelDetail;
    s.sm_local_users.push_back(entry);

    XblSocialManagerEvent ev{};
    ev.user = dup;
    ev.eventType = XblSocialManagerEventType_LocalUserAdded;
    ev.hr = XBL_S_OK;
    s.sm_pending_events.push_back(ev);
    return RecordHr("XblSocialManagerAddLocalUser", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerRemoveLocalUser(XUserHandle user) {
    if (!user) {
        return RecordHr("XblSocialManagerRemoveLocalUser", XGR_E_INVALIDARG);
    }
    uint64_t xuid = 0;
    (void)XUserGetId(user, &xuid);
    XUserHandle to_close = nullptr;
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        for (auto it = s.sm_local_users.begin(); it != s.sm_local_users.end(); ++it) {
            if (it->user == user || (xuid != 0 && it->xuid == xuid)) {
                to_close = it->user;
                s.sm_local_users.erase(it);
                break;
            }
        }
    }
    if (to_close) {
        XUserCloseHandle(to_close);
    }
    return RecordHr("XblSocialManagerRemoveLocalUser", XBL_S_OK);
}

size_t OPENSCARLETT_XSAPI_CALL XblSocialManagerGetLocalUserCount(void) {
    RecordOk("XblSocialManagerGetLocalUserCount");
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.sm_local_users.size();
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerGetLocalUsers(
    size_t usersCount, XUserHandle* users) {
    if (!users && usersCount > 0) {
        return RecordHr("XblSocialManagerGetLocalUsers", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (usersCount < s.sm_local_users.size()) {
        return RecordHr("XblSocialManagerGetLocalUsers", XGR_E_NOT_SUFFICIENT_BUFFER);
    }
    for (size_t i = 0; i < s.sm_local_users.size(); ++i) {
        users[i] = s.sm_local_users[i].user;
    }
    return RecordHr("XblSocialManagerGetLocalUsers", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerCreateSocialUserGroupFromFilters(
    XUserHandle user,
    XblPresenceFilter presenceFilter,
    XblRelationshipFilter relationshipFilter,
    XblSocialManagerUserGroupHandle* group) {
    if (!user || !group) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromFilters", XGR_E_INVALIDARG);
    }
    *group = nullptr;
    uint64_t xuid = 0;
    XGameResult hr = XUserGetId(user, &xuid);
    if (hr < 0) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromFilters", hr);
    }

    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromFilters", XBL_E_NOT_INITIALIZED);
    }
    auto g = std::make_unique<XblSocialManagerUserGroup>();
    g->local_user = user;
    g->local_xuid = xuid;
    g->type = XblSocialUserGroupType_FilterType;
    g->presence_filter = presenceFilter;
    g->relationship_filter = relationshipFilter;
    XblSocialManagerUserGroupHandle raw = g.get();
    s.sm_groups[raw] = std::move(g);

    XblSocialManagerEvent ev{};
    ev.user = user;
    ev.eventType = XblSocialManagerEventType_SocialUserGroupLoaded;
    ev.hr = XBL_S_OK;
    ev.groupAffected = raw;
    s.sm_pending_events.push_back(ev);

    *group = raw;
    return RecordHr("XblSocialManagerCreateSocialUserGroupFromFilters", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerCreateSocialUserGroupFromList(
    XUserHandle user,
    uint64_t* xboxUserIdList,
    size_t xboxUserIdListCount,
    XblSocialManagerUserGroupHandle* group) {
    if (!user || !group || (!xboxUserIdList && xboxUserIdListCount > 0)) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromList", XGR_E_INVALIDARG);
    }
    *group = nullptr;
    uint64_t xuid = 0;
    XGameResult hr = XUserGetId(user, &xuid);
    if (hr < 0) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromList", hr);
    }

    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblSocialManagerCreateSocialUserGroupFromList", XBL_E_NOT_INITIALIZED);
    }
    auto g = std::make_unique<XblSocialManagerUserGroup>();
    g->local_user = user;
    g->local_xuid = xuid;
    g->type = XblSocialUserGroupType_UserListType;
    if (xboxUserIdList && xboxUserIdListCount > 0) {
        g->tracked_xuids.assign(xboxUserIdList, xboxUserIdList + xboxUserIdListCount);
    }
    RebuildSocialGroupUsersLocked(*g);

    XblSocialManagerUserGroupHandle raw = g.get();
    s.sm_groups[raw] = std::move(g);

    XblSocialManagerEvent ev{};
    ev.user = user;
    ev.eventType = XblSocialManagerEventType_SocialUserGroupLoaded;
    ev.hr = XBL_S_OK;
    ev.groupAffected = raw;
    s.sm_pending_events.push_back(ev);

    *group = raw;
    return RecordHr("XblSocialManagerCreateSocialUserGroupFromList", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerDestroySocialUserGroup(
    XblSocialManagerUserGroupHandle group) {
    if (!group) {
        return RecordHr("XblSocialManagerDestroySocialUserGroup", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerDestroySocialUserGroup", XGR_E_HANDLE);
    }
    s.sm_groups.erase(it);
    return RecordHr("XblSocialManagerDestroySocialUserGroup", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerDoWork(
    const XblSocialManagerEvent** socialEvents, size_t* socialEventsCount) {
    if (!socialEvents || !socialEventsCount) {
        return RecordHr("XblSocialManagerDoWork", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.sm_active_events.swap(s.sm_pending_events);
    s.sm_pending_events.clear();
    *socialEvents = s.sm_active_events.empty() ? nullptr : s.sm_active_events.data();
    *socialEventsCount = s.sm_active_events.size();
    return RecordHr("XblSocialManagerDoWork", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetUsers(
    XblSocialManagerUserGroupHandle group,
    const XblSocialManagerUser* const** xboxSocialUsers,
    size_t* xboxSocialUsersCount) {
    if (!group || !xboxSocialUsers || !xboxSocialUsersCount) {
        return RecordHr("XblSocialManagerUserGroupGetUsers", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUserGroupGetUsers", XGR_E_HANDLE);
    }
    auto& g = *it->second;
    *xboxSocialUsers = g.user_ptrs.empty() ? nullptr : g.user_ptrs.data();
    *xboxSocialUsersCount = g.user_ptrs.size();
    return RecordHr("XblSocialManagerUserGroupGetUsers", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetLocalUser(
    XblSocialManagerUserGroupHandle group, XUserHandle* localUser) {
    if (!group || !localUser) {
        return RecordHr("XblSocialManagerUserGroupGetLocalUser", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUserGroupGetLocalUser", XGR_E_HANDLE);
    }
    *localUser = it->second->local_user;
    return RecordHr("XblSocialManagerUserGroupGetLocalUser", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetType(
    XblSocialManagerUserGroupHandle group, XblSocialUserGroupType* type) {
    if (!group || !type) {
        return RecordHr("XblSocialManagerUserGroupGetType", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUserGroupGetType", XGR_E_HANDLE);
    }
    *type = it->second->type;
    return RecordHr("XblSocialManagerUserGroupGetType", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetFilters(
    XblSocialManagerUserGroupHandle group,
    XblPresenceFilter* presenceFilter,
    XblRelationshipFilter* relationshipFilter) {
    if (!group || (!presenceFilter && !relationshipFilter)) {
        return RecordHr("XblSocialManagerUserGroupGetFilters", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUserGroupGetFilters", XGR_E_HANDLE);
    }
    if (presenceFilter) *presenceFilter = it->second->presence_filter;
    if (relationshipFilter) *relationshipFilter = it->second->relationship_filter;
    return RecordHr("XblSocialManagerUserGroupGetFilters", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUserGroupGetUsersTrackedByGroup(
    XblSocialManagerUserGroupHandle group,
    const uint64_t** trackedUsers,
    size_t* trackedUsersCount) {
    if (!group || !trackedUsers || !trackedUsersCount) {
        return RecordHr("XblSocialManagerUserGroupGetUsersTrackedByGroup", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUserGroupGetUsersTrackedByGroup", XGR_E_HANDLE);
    }
    const auto& vec = it->second->tracked_xuids;
    *trackedUsers = vec.empty() ? nullptr : vec.data();
    *trackedUsersCount = vec.size();
    return RecordHr("XblSocialManagerUserGroupGetUsersTrackedByGroup", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerUpdateSocialUserGroup(
    XblSocialManagerUserGroupHandle group, uint64_t* users, size_t usersCount) {
    if (!group || (!users && usersCount > 0)) {
        return RecordHr("XblSocialManagerUpdateSocialUserGroup", XGR_E_INVALIDARG);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.sm_groups.find(group);
    if (it == s.sm_groups.end()) {
        return RecordHr("XblSocialManagerUpdateSocialUserGroup", XGR_E_HANDLE);
    }
    auto& g = *it->second;
    g.tracked_xuids.clear();
    if (users && usersCount > 0) {
        g.tracked_xuids.assign(users, users + usersCount);
    }
    RebuildSocialGroupUsersLocked(g);

    XblSocialManagerEvent ev{};
    ev.user = g.local_user;
    ev.eventType = XblSocialManagerEventType_SocialUserGroupUpdated;
    ev.hr = XBL_S_OK;
    ev.groupAffected = group;
    s.sm_pending_events.push_back(ev);
    return RecordHr("XblSocialManagerUpdateSocialUserGroup", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblSocialManagerSetRichPresencePollingStatus(
    XUserHandle user, bool shouldEnablePolling) {
    if (!user) {
        return RecordHr("XblSocialManagerSetRichPresencePollingStatus", XGR_E_INVALIDARG);
    }
    uint64_t xuid = 0;
    (void)XUserGetId(user, &xuid);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    for (auto& lu : s.sm_local_users) {
        if (lu.user == user || (xuid != 0 && lu.xuid == xuid)) {
            lu.rich_presence_polling = shouldEnablePolling;
            break;
        }
    }
    return RecordHr("XblSocialManagerSetRichPresencePollingStatus", XBL_S_OK);
}

bool OPENSCARLETT_XSAPI_CALL XblSocialManagerPresenceRecordIsUserPlayingTitle(
    const XblSocialManagerPresenceRecord* presenceRecord, uint32_t titleId) {
    RecordOk("XblSocialManagerPresenceRecordIsUserPlayingTitle");
    if (!presenceRecord || titleId == 0u) return false;
    const uint32_t count = std::min<uint32_t>(
        presenceRecord->presenceTitleRecordCount,
        static_cast<uint32_t>(XBL_NUM_PRESENCE_RECORDS));
    for (uint32_t i = 0; i < count; ++i) {
        if (presenceRecord->presenceTitleRecords[i].titleId == titleId &&
            presenceRecord->presenceTitleRecords[i].isTitleActive) {
            return true;
        }
    }
    return false;
}

// ===========================================================================
// HTTP Calls (XblHttpCall*)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallCreate(
    XblContextHandle xblContext,
    const char* method,
    const char* url,
    XblHttpCallHandle* call) {
    if (!xblContext || !method || !*method || !url || !*url || !call) {
        return RecordHr("XblHttpCallCreate", XGR_E_INVALIDARG);
    }
    *call = nullptr;
    auto ctx = LookupContext(xblContext);
    if (!ctx) return RecordHr("XblHttpCallCreate", XGR_E_HANDLE);

    auto obj = std::make_shared<XblHttpCall>();
    obj->xuid = ctx->xuid;
    obj->method = method;
    obj->url = url;
    XblHttpCallHandle raw = obj.get();
    {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return RecordHr("XblHttpCallCreate", XBL_E_NOT_INITIALIZED);
        s.live_http_calls[raw] = std::move(obj);
    }
    *call = raw;
    return RecordHr("XblHttpCallCreate", XBL_S_OK);
}

XblHttpCallHandle OPENSCARLETT_XSAPI_CALL XblHttpCallDuplicateHandle(XblHttpCallHandle call) {
    if (!call) {
        RecordHr("XblHttpCallDuplicateHandle", XGR_E_INVALIDARG);
        return nullptr;
    }
    auto obj = LookupHttpCall(call);
    if (!obj) {
        RecordHr("XblHttpCallDuplicateHandle", XGR_E_HANDLE);
        return nullptr;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    RecordOk("XblHttpCallDuplicateHandle");
    return call;
}

void OPENSCARLETT_XSAPI_CALL XblHttpCallCloseHandle(XblHttpCallHandle call) {
    RecordOk("XblHttpCallCloseHandle");
    if (!call) return;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto it = s.live_http_calls.find(call);
    if (it == s.live_http_calls.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        s.live_http_calls.erase(it);
    }
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallPerformAsync(
    XblHttpCallHandle call,
    XblHttpCallResponseBodyType /*type*/,
    XAsyncBlock* asyncBlock) {
    if (!call || !asyncBlock) {
        return RecordHr("XblHttpCallPerformAsync", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallPerformAsync", XGR_E_HANDLE);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [obj](XsapiAsyncOp&) -> XGameResult {
        std::vector<std::pair<XblCallRoutedHandler, void*>> routed_handlers;
        std::string method_copy;
        std::string url_copy;
        std::string body_str;
        std::string auth_token;
        std::string auth_sig;
        uint64_t xuid_copy = 0;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            method_copy = obj->method;
            url_copy = obj->url;
            xuid_copy = obj->xuid;
            for (const auto& [tok, cb] : s.call_routed_handlers) {
                (void)tok;
                routed_handlers.push_back(cb);
            }
        }

        const bool is_auth_endpoint = IsXboxLiveAuthEndpointUrl(url_copy);
        if (is_auth_endpoint && !IsMockXstsEnabled()) {
            // Offline-first policy: fail Xbox Live auth/XSTS token endpoints immediately with
            // canonical E_GAMEUSER_NO_TOKEN_REQUIRED so real titles take their offline codepath
            // without stalling or hanging on live authentication handshakes.
            {
                XsapiState& s = GetState();
                std::lock_guard<std::mutex> lk(s.mutex);
                obj->status_code = 0u;
                obj->network_error_code = kHrGameUserNoTokenRequired;
                obj->platform_network_error_code = static_cast<uint32_t>(kHrGameUserNoTokenRequired);
                obj->platform_network_error_message =
                    "Offline mode: XSTS auth token disabled (set OPENSCARLETT_MOCK_XSTS=1 to opt in)";
                obj->response_headers.clear();
                obj->response_body.clear();
                obj->response_string.clear();
            }
            if (!routed_handlers.empty()) {
                const int64_t now = NowUnixSeconds();
                XblServiceCallRoutedArgs args{};
                args.xboxUserId = xuid_copy;
                args.httpMethod = method_copy.c_str();
                args.uri = url_copy.c_str();
                args.requestHeaders = "";
                args.requestBody = "";
                args.responseCount = 0u;
                args.responseHeaders = "";
                args.responseBody = "";
                args.etag = "";
                args.token = "";
                args.signature = "";
                args.httpStatus = 0u;
                args.fullResponseFormatted = "";
                args.requestTime = now;
                args.responseTime = now;
                args.elapsedCallTime = 0;
                for (const auto& [fn, user_ctx] : routed_handlers) {
                    if (fn) fn(args, user_ctx);
                }
            }
            return kHrGameUserNoTokenRequired;
        }

        if (is_auth_endpoint) {
            // Opt-in OPENSCARLETT_MOCK_XSTS=1: request synthetic XBL3.0 token from xgameruntime.
            XUserHandle u = nullptr;
            if (XUserFindUserById(xuid_copy, &u) == XGR_S_OK && u != nullptr) {
                XAsyncBlock tab{};
                if (XUserGetTokenAndSignatureAsync(
                        u, XUserGetTokenAndSignatureOptions_None,
                        method_copy.c_str(), url_copy.c_str(),
                        0, nullptr, 0, nullptr, &tab) == XGR_S_OK) {
                    size_t tsz = 0;
                    if (XUserGetTokenAndSignatureResultSize(&tab, &tsz) == XGR_S_OK &&
                        tsz >= sizeof(XUserGetTokenAndSignatureData)) {
                        std::vector<uint8_t> tbuf(tsz, 0);
                        XUserGetTokenAndSignatureData* tdata = nullptr;
                        size_t tused = 0;
                        if (XUserGetTokenAndSignatureResult(
                                &tab, tbuf.size(), tbuf.data(), &tdata, &tused) == XGR_S_OK &&
                            tdata != nullptr) {
                            if (tdata->token) auth_token = tdata->token;
                            if (tdata->signature) auth_sig = tdata->signature;
                        }
                    }
                }
                XUserCloseHandle(u);
            }
        }

        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;

            std::ostringstream oss;
            if (is_auth_endpoint) {
                oss << "{\"service\":\"openscarlett-xsapi-http\",\"status\":200,\"Token\":"
                    << JsonEscape(auth_token) << ",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"0000000000000001\"}]}}";
            } else {
                oss << "{\"service\":\"openscarlett-xsapi-http\",\"status\":200,\"method\":"
                    << JsonEscape(obj->method) << ",\"url\":" << JsonEscape(obj->url)
                    << ",\"requestBytes\":" << obj->request_body.size() << "}";
            }
            body_str = oss.str();

            obj->status_code = 200u;
            obj->network_error_code = XBL_S_OK;
            obj->platform_network_error_code = 0u;
            obj->platform_network_error_message.clear();
            obj->response_string = body_str;
            obj->response_body.assign(body_str.begin(), body_str.end());
            obj->response_headers = {
                {"Content-Type", "application/json"},
                {"Server", "OpenScarlett-XSAPI/1.0"},
                {"Content-Length", std::to_string(body_str.size())},
            };
        }

        if (!routed_handlers.empty()) {
            const int64_t now = NowUnixSeconds();
            XblServiceCallRoutedArgs args{};
            args.xboxUserId = xuid_copy;
            args.httpMethod = method_copy.c_str();
            args.uri = url_copy.c_str();
            args.requestHeaders = "";
            args.requestBody = "";
            args.responseCount = 1u;
            args.responseHeaders = "Content-Type: application/json\r\n";
            args.responseBody = body_str.c_str();
            args.etag = "";
            args.token = auth_token.c_str();
            args.signature = auth_sig.c_str();
            args.httpStatus = 200u;
            args.fullResponseFormatted = body_str.c_str();
            args.requestTime = now;
            args.responseTime = now;
            args.elapsedCallTime = 0;
            for (const auto& [fn, user_ctx] : routed_handlers) {
                if (fn) fn(args, user_ctx);
            }
        }
        return XBL_S_OK;
    };

    return RecordHr("XblHttpCallPerformAsync",
                    StartXsapiAsync(asyncBlock, kIdHttpPerform, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetHeader(
    XblHttpCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool /*allowTracing*/) {
    if (!call || !headerName || !*headerName || !headerValue) {
        return RecordHr("XblHttpCallRequestSetHeader", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallRequestSetHeader", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    for (auto& kv : obj->request_headers) {
        if (EqualsIgnoreCaseAscii(kv.first, headerName)) {
            kv.second = headerValue;
            return RecordHr("XblHttpCallRequestSetHeader", XBL_S_OK);
        }
    }
    obj->request_headers.emplace_back(headerName, headerValue);
    return RecordHr("XblHttpCallRequestSetHeader", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRequestBodyBytes(
    XblHttpCallHandle call, const uint8_t* requestBodyBytes, uint32_t requestBodySize) {
    if (!call || (!requestBodyBytes && requestBodySize > 0u)) {
        return RecordHr("XblHttpCallRequestSetRequestBodyBytes", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallRequestSetRequestBodyBytes", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (requestBodyBytes && requestBodySize > 0u) {
        obj->request_body.assign(requestBodyBytes, requestBodyBytes + requestBodySize);
    } else {
        obj->request_body.clear();
    }
    return RecordHr("XblHttpCallRequestSetRequestBodyBytes", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRequestBodyString(
    XblHttpCallHandle call, const char* requestBodyString) {
    if (!call || !requestBodyString) {
        return RecordHr("XblHttpCallRequestSetRequestBodyString", XGR_E_INVALIDARG);
    }
    return XblHttpCallRequestSetRequestBodyBytes(
        call,
        reinterpret_cast<const uint8_t*>(requestBodyString),
        static_cast<uint32_t>(std::strlen(requestBodyString)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRetryAllowed(
    XblHttpCallHandle call, bool retryAllowed) {
    if (!call) return RecordHr("XblHttpCallRequestSetRetryAllowed", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallRequestSetRetryAllowed", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    obj->retry_allowed = retryAllowed;
    return RecordHr("XblHttpCallRequestSetRetryAllowed", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetRetryCacheId(
    XblHttpCallHandle call, uint32_t retryAfterCacheId) {
    if (!call) return RecordHr("XblHttpCallRequestSetRetryCacheId", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallRequestSetRetryCacheId", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    obj->retry_cache_id = retryAfterCacheId;
    return RecordHr("XblHttpCallRequestSetRetryCacheId", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallRequestSetLongHttpCall(
    XblHttpCallHandle call, bool longHttpCall) {
    if (!call) return RecordHr("XblHttpCallRequestSetLongHttpCall", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallRequestSetLongHttpCall", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    obj->long_http_call = longHttpCall;
    return RecordHr("XblHttpCallRequestSetLongHttpCall", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallSetTracing(
    XblHttpCallHandle call, bool traceCall) {
    if (!call) return RecordHr("XblHttpCallSetTracing", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallSetTracing", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    obj->trace_call = traceCall;
    return RecordHr("XblHttpCallSetTracing", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetRequestUrl(
    XblHttpCallHandle call, const char** url) {
    if (!call || !url) return RecordHr("XblHttpCallGetRequestUrl", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetRequestUrl", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *url = obj->url.c_str();
    return RecordHr("XblHttpCallGetRequestUrl", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetStatusCode(
    XblHttpCallHandle call, uint32_t* statusCode) {
    if (!call || !statusCode) return RecordHr("XblHttpCallGetStatusCode", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetStatusCode", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *statusCode = obj->status_code;
    return RecordHr("XblHttpCallGetStatusCode", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetNetworkErrorCode(
    XblHttpCallHandle call,
    XGameResult* networkErrorCode,
    uint32_t* platformNetworkErrorCode) {
    if (!call || !networkErrorCode || !platformNetworkErrorCode) {
        return RecordHr("XblHttpCallGetNetworkErrorCode", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetNetworkErrorCode", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *networkErrorCode = obj->network_error_code;
    *platformNetworkErrorCode = obj->platform_network_error_code;
    return RecordHr("XblHttpCallGetNetworkErrorCode", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetPlatformNetworkErrorMessage(
    XblHttpCallHandle call, const char** platformNetworkErrorMessage) {
    if (!call || !platformNetworkErrorMessage) {
        return RecordHr("XblHttpCallGetPlatformNetworkErrorMessage", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetPlatformNetworkErrorMessage", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *platformNetworkErrorMessage = obj->platform_network_error_message.c_str();
    return RecordHr("XblHttpCallGetPlatformNetworkErrorMessage", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetNumHeaders(
    XblHttpCallHandle call, uint32_t* numHeaders) {
    if (!call || !numHeaders) return RecordHr("XblHttpCallGetNumHeaders", XGR_E_INVALIDARG);
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetNumHeaders", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *numHeaders = static_cast<uint32_t>(obj->response_headers.size());
    return RecordHr("XblHttpCallGetNumHeaders", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetHeaderAtIndex(
    XblHttpCallHandle call,
    uint32_t headerIndex,
    const char** headerName,
    const char** headerValue) {
    if (!call || !headerName || !headerValue) {
        return RecordHr("XblHttpCallGetHeaderAtIndex", XGR_E_INVALIDARG);
    }
    *headerName = nullptr;
    *headerValue = nullptr;
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetHeaderAtIndex", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (headerIndex >= obj->response_headers.size()) {
        return RecordHr("XblHttpCallGetHeaderAtIndex", XGR_E_INVALIDARG);
    }
    *headerName = obj->response_headers[headerIndex].first.c_str();
    *headerValue = obj->response_headers[headerIndex].second.c_str();
    return RecordHr("XblHttpCallGetHeaderAtIndex", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetHeader(
    XblHttpCallHandle call, const char* headerName, const char** headerValue) {
    if (!call || !headerName || !headerValue) {
        return RecordHr("XblHttpCallGetHeader", XGR_E_INVALIDARG);
    }
    *headerValue = nullptr;
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetHeader", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    for (const auto& kv : obj->response_headers) {
        if (EqualsIgnoreCaseAscii(kv.first, headerName)) {
            *headerValue = kv.second.c_str();
            return RecordHr("XblHttpCallGetHeader", XBL_S_OK);
        }
    }
    return RecordHr("XblHttpCallGetHeader", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseBodyBytesSize(
    XblHttpCallHandle call, size_t* bufferSize) {
    if (!call || !bufferSize) {
        return RecordHr("XblHttpCallGetResponseBodyBytesSize", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetResponseBodyBytesSize", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *bufferSize = obj->response_body.size();
    return RecordHr("XblHttpCallGetResponseBodyBytesSize", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseBodyBytes(
    XblHttpCallHandle call, size_t bufferSize, uint8_t* buffer, size_t* bufferUsed) {
    if (!call || (!buffer && bufferSize > 0)) {
        return RecordHr("XblHttpCallGetResponseBodyBytes", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetResponseBodyBytes", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (bufferSize < obj->response_body.size()) {
        return RecordHr("XblHttpCallGetResponseBodyBytes", XGR_E_NOT_SUFFICIENT_BUFFER);
    }
    if (!obj->response_body.empty() && buffer) {
        std::memcpy(buffer, obj->response_body.data(), obj->response_body.size());
    }
    if (bufferUsed) *bufferUsed = obj->response_body.size();
    return RecordHr("XblHttpCallGetResponseBodyBytes", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblHttpCallGetResponseString(
    XblHttpCallHandle call, const char** responseString) {
    if (!call || !responseString) {
        return RecordHr("XblHttpCallGetResponseString", XGR_E_INVALIDARG);
    }
    auto obj = LookupHttpCall(call);
    if (!obj) return RecordHr("XblHttpCallGetResponseString", XGR_E_HANDLE);
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    *responseString = obj->response_string.c_str();
    return RecordHr("XblHttpCallGetResponseString", XBL_S_OK);
}

// ===========================================================================
// Expanded User Statistics & Title-Managed Statistics
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticsAsync(
    XblContextHandle xblContextHandle,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!statisticNames && statisticNamesCount > 0)) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticsAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) return RecordHr("XblUserStatisticsGetSingleUserStatisticsAsync", XGR_E_HANDLE);

    const uint64_t target_xuid = xboxUserId != 0 ? xboxUserId : ctx->xuid;
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    std::vector<std::string> names;
    names.reserve(statisticNamesCount);
    for (size_t i = 0; i < statisticNamesCount; ++i) {
        if (statisticNames[i] && *statisticNames[i]) names.emplace_back(statisticNames[i]);
    }

    auto bundles = std::make_shared<std::vector<MultiStatUserBundle>>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [target_xuid, target_scid, names = std::move(names), bundles](
                      XsapiAsyncOp& self) -> XGameResult {
        MultiStatUserBundle ub{};
        ub.xuid = target_xuid;
        MultiStatUserBundle::ScidGroup sg{};
        sg.scid = target_scid;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureStatsLoadedLocked(s, target_scid, target_xuid);
            auto it = s.stats_cache.find(StoreKey{target_scid, target_xuid});
            for (const auto& name : names) {
                StoredStat st{name, "Int64", "0"};
                if (it != s.stats_cache.end()) {
                    auto sit = it->second.find(name);
                    if (sit != it->second.end()) st = sit->second;
                }
                sg.stats.push_back(std::move(st));
            }
        }
        ub.scid_groups.push_back(std::move(sg));
        bundles->push_back(std::move(ub));
        self.required_buffer_size = ComputeMultiStatsBufferSize(*bundles);
        return XBL_S_OK;
    };

    op->get_result_fn = [bundles](void* buffer, size_t buffer_size) -> XGameResult {
        return PackMultiStatsBuffer(*bundles, buffer, buffer_size);
    };

    return RecordHr("XblUserStatisticsGetSingleUserStatisticsAsync",
                    StartXsapiAsync(async, kIdStatsGetSingleMulti, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticsResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticsResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticsResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblUserStatisticsGetSingleUserStatisticsResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetSingleUserStatisticsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XblUserStatisticsGetSingleUserStatisticsResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticsResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdStatsGetSingleMulti, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblUserStatisticsGetSingleUserStatisticsResult", hr);
    *ptrToBuffer = static_cast<XblUserStatisticsResult*>(buffer);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblUserStatisticsGetSingleUserStatisticsResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetMultipleUserStatisticsAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xboxUserIds,
    size_t xboxUserIdsCount,
    const char* serviceConfigurationId,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!xboxUserIds && xboxUserIdsCount > 0) ||
        (!statisticNames && statisticNamesCount > 0)) {
        return RecordHr("XblUserStatisticsGetMultipleUserStatisticsAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) return RecordHr("XblUserStatisticsGetMultipleUserStatisticsAsync", XGR_E_HANDLE);

    std::vector<uint64_t> xuids;
    if (xboxUserIds && xboxUserIdsCount > 0) {
        xuids.assign(xboxUserIds, xboxUserIds + xboxUserIdsCount);
    }
    const std::string target_scid = (serviceConfigurationId && *serviceConfigurationId)
                                        ? std::string(serviceConfigurationId)
                                        : ctx->scid;
    std::vector<std::string> names;
    names.reserve(statisticNamesCount);
    for (size_t i = 0; i < statisticNamesCount; ++i) {
        if (statisticNames[i] && *statisticNames[i]) names.emplace_back(statisticNames[i]);
    }

    struct MultiResultHolder {
        std::vector<MultiStatUserBundle> bundles;
    };
    auto holder = std::make_shared<MultiResultHolder>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, xuids = std::move(xuids), target_scid, names = std::move(names), holder](
                      XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        for (uint64_t x : xuids) {
            const uint64_t target_xuid = x != 0 ? x : ctx->xuid;
            EnsureStatsLoadedLocked(s, target_scid, target_xuid);
            MultiStatUserBundle ub{};
            ub.xuid = target_xuid;
            MultiStatUserBundle::ScidGroup sg{};
            sg.scid = target_scid;
            auto it = s.stats_cache.find(StoreKey{target_scid, target_xuid});
            for (const auto& name : names) {
                StoredStat st{name, "Int64", "0"};
                if (it != s.stats_cache.end()) {
                    auto sit = it->second.find(name);
                    if (sit != it->second.end()) st = sit->second;
                }
                sg.stats.push_back(std::move(st));
            }
            ub.scid_groups.push_back(std::move(sg));
            holder->bundles.push_back(std::move(ub));
        }
        // Prefix payload with size_t count? No, PackMultiStatsBuffer writes XblUserStatisticsResult[]
        // at offset 0. We store count in a trailing size_t? Better: expose count via get_result_fn
        // or store count at buffer end? Wait: caller calls ResultSize then Result(bufferSize, buffer,
        // &results, &resultsCount, &bufferUsed). Let's pack count in a small header? No! *results
        // points into buffer, so placing results at offset 0 and returning count from holder is easiest.
        // Wait: XAsyncGetResult only passes buffer/bufferSize, so we pack size_t count at offset 0
        // and place XblUserStatisticsResult[] right after it (aligned to 8 bytes)!
        self.required_buffer_size = sizeof(size_t) + ComputeMultiStatsBufferSize(holder->bundles);
        return XBL_S_OK;
    };

    op->get_result_fn = [holder](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t data_sz = ComputeMultiStatsBufferSize(holder->bundles);
        const size_t total_sz = sizeof(size_t) + data_sz;
        if (!buffer || buffer_size < total_sz) return XGR_E_NOT_SUFFICIENT_BUFFER;
        const size_t cnt = holder->bundles.size();
        std::memcpy(buffer, &cnt, sizeof(size_t));
        auto* payload = static_cast<uint8_t*>(buffer) + sizeof(size_t);
        return PackMultiStatsBuffer(holder->bundles, payload, data_sz);
    };

    return RecordHr("XblUserStatisticsGetMultipleUserStatisticsAsync",
                    StartXsapiAsync(async, kIdStatsGetMulti, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetMultipleUserStatisticsResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResultSize", hr);
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsGetMultipleUserStatisticsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** results,
    size_t* resultsCount,
    size_t* bufferUsed) {
    if (!async || !buffer || !results || !resultsCount) {
        return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResult", XGR_E_INVALIDARG);
    }
    *results = nullptr;
    *resultsCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdStatsGetMulti, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResult", hr);
    size_t cnt = 0;
    std::memcpy(&cnt, buffer, sizeof(size_t));
    *resultsCount = cnt;
    *results = (cnt > 0u)
                   ? reinterpret_cast<XblUserStatisticsResult*>(
                         static_cast<uint8_t*>(buffer) + sizeof(size_t))
                   : nullptr;
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblUserStatisticsGetMultipleUserStatisticsResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xboxUserIds,
    uint32_t xboxUserIdsCount,
    const XblRequestedStatistics* requestedServiceConfigurationStatisticsCollection,
    uint32_t requestedServiceConfigurationStatisticsCollectionCount,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!xboxUserIds && xboxUserIdsCount > 0u) ||
        (!requestedServiceConfigurationStatisticsCollection &&
         requestedServiceConfigurationStatisticsCollectionCount > 0u)) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync",
            XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync",
            XGR_E_HANDLE);
    }

    std::vector<uint64_t> xuids;
    if (xboxUserIds && xboxUserIdsCount > 0u) {
        xuids.assign(xboxUserIds, xboxUserIds + xboxUserIdsCount);
    }
    struct ReqCopy {
        std::string scid;
        std::vector<std::string> names;
    };
    std::vector<ReqCopy> reqs;
    reqs.reserve(requestedServiceConfigurationStatisticsCollectionCount);
    for (uint32_t i = 0; i < requestedServiceConfigurationStatisticsCollectionCount; ++i) {
        const auto& r = requestedServiceConfigurationStatisticsCollection[i];
        ReqCopy rc{};
        rc.scid = r.serviceConfigurationId[0] != '\0' ? std::string(r.serviceConfigurationId) : ctx->scid;
        for (uint32_t j = 0; j < r.statisticsCount; ++j) {
            if (r.statistics && r.statistics[j] && *r.statistics[j]) {
                rc.names.emplace_back(r.statistics[j]);
            }
        }
        reqs.push_back(std::move(rc));
    }

    auto bundles = std::make_shared<std::vector<MultiStatUserBundle>>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, xuids = std::move(xuids), reqs = std::move(reqs), bundles](
                      XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        for (uint64_t x : xuids) {
            const uint64_t target_xuid = x != 0 ? x : ctx->xuid;
            MultiStatUserBundle ub{};
            ub.xuid = target_xuid;
            for (const auto& rc : reqs) {
                EnsureStatsLoadedLocked(s, rc.scid, target_xuid);
                MultiStatUserBundle::ScidGroup sg{};
                sg.scid = rc.scid;
                auto it = s.stats_cache.find(StoreKey{rc.scid, target_xuid});
                for (const auto& name : rc.names) {
                    StoredStat st{name, "Int64", "0"};
                    if (it != s.stats_cache.end()) {
                        auto sit = it->second.find(name);
                        if (sit != it->second.end()) st = sit->second;
                    }
                    sg.stats.push_back(std::move(st));
                }
                ub.scid_groups.push_back(std::move(sg));
            }
            bundles->push_back(std::move(ub));
        }
        self.required_buffer_size = sizeof(size_t) + ComputeMultiStatsBufferSize(*bundles);
        return XBL_S_OK;
    };

    op->get_result_fn = [bundles](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t data_sz = ComputeMultiStatsBufferSize(*bundles);
        const size_t total_sz = sizeof(size_t) + data_sz;
        if (!buffer || buffer_size < total_sz) return XGR_E_NOT_SUFFICIENT_BUFFER;
        const size_t cnt = bundles->size();
        std::memcpy(buffer, &cnt, sizeof(size_t));
        auto* payload = static_cast<uint8_t*>(buffer) + sizeof(size_t);
        return PackMultiStatsBuffer(*bundles, payload, data_sz);
    };

    return RecordHr(
        "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsAsync",
        StartXsapiAsync(async, kIdStatsGetMultiScid, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize(
    XAsyncBlock* async, size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize",
            XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize",
            hr);
    }
    hr = XAsyncGetResultSize(async, resultSizeInBytes);
    return RecordHr(
        "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL
XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** results,
    size_t* resultsCount,
    size_t* bufferUsed) {
    if (!async || !buffer || !results || !resultsCount) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult",
            XGR_E_INVALIDARG);
    }
    *results = nullptr;
    *resultsCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult", hr);
    }
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdStatsGetMultiScid, bufferSize, buffer, &used);
    if (hr < 0) {
        return RecordHr(
            "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult", hr);
    }
    size_t cnt = 0;
    std::memcpy(&cnt, buffer, sizeof(size_t));
    *resultsCount = cnt;
    *results = (cnt > 0u)
                   ? reinterpret_cast<XblUserStatisticsResult*>(
                         static_cast<uint8_t*>(buffer) + sizeof(size_t))
                   : nullptr;
    if (bufferUsed) *bufferUsed = used;
    return RecordHr(
        "XblUserStatisticsGetMultipleUserStatisticsForMultipleServiceConfigurationsResult",
        XBL_S_OK);
}

XblFunctionContext OPENSCARLETT_XSAPI_CALL XblUserStatisticsAddStatisticChangedHandler(
    XblContextHandle xblContextHandle,
    XblStatisticChangedHandler handler,
    void* handlerContext) {
    if (!xblContextHandle || !handler || !LookupContext(xblContextHandle)) {
        RecordHr("XblUserStatisticsAddStatisticChangedHandler", XGR_E_INVALIDARG);
        return 0;
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    const XblFunctionContext token = s.next_handler_token++;
    s.stat_changed_handlers[token] = {handler, handlerContext};
    RecordOk("XblUserStatisticsAddStatisticChangedHandler");
    return token;
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsRemoveStatisticChangedHandler(
    XblContextHandle xblContextHandle, XblFunctionContext handlerContext) {
    if (!xblContextHandle) {
        return RecordHr("XblUserStatisticsRemoveStatisticChangedHandler", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblUserStatisticsRemoveStatisticChangedHandler", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.stat_changed_handlers.erase(handlerContext);
    return RecordHr("XblUserStatisticsRemoveStatisticChangedHandler", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsSubscribeToStatisticChange(
    XblContextHandle xblContextHandle,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle) {
    if (!xblContextHandle || !statisticName || !*statisticName || !subscriptionHandle) {
        return RecordHr("XblUserStatisticsSubscribeToStatisticChange", XGR_E_INVALIDARG);
    }
    *subscriptionHandle = nullptr;
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) {
        return RecordHr("XblUserStatisticsSubscribeToStatisticChange", XGR_E_HANDLE);
    }
    auto sub = std::make_unique<XblRealTimeActivitySubscription>();
    sub->kind = "stat";
    sub->xuid = xboxUserId;
    sub->scid = (serviceConfigurationId && *serviceConfigurationId)
                    ? std::string(serviceConfigurationId)
                    : ctx->scid;
    sub->stat_name = statisticName;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblUserStatisticsSubscribeToStatisticChange", XBL_E_NOT_INITIALIZED);
    }
    sub->id = s.next_sub_id++;
    XblRealTimeActivitySubscriptionHandle raw = sub.get();
    s.live_subscriptions[raw] = std::move(sub);
    *subscriptionHandle = raw;
    return RecordHr("XblUserStatisticsSubscribeToStatisticChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblUserStatisticsUnsubscribeFromStatisticChange(
    XblContextHandle xblContextHandle, XblRealTimeActivitySubscriptionHandle subscriptionHandle) {
    if (!xblContextHandle || !subscriptionHandle) {
        return RecordHr("XblUserStatisticsUnsubscribeFromStatisticChange", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblUserStatisticsUnsubscribeFromStatisticChange", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.live_subscriptions.erase(subscriptionHandle);
    return RecordHr("XblUserStatisticsUnsubscribeFromStatisticChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsUpdateStatsAsync(
    XblContextHandle xblContextHandle,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!statistics && statisticsCount > 0)) {
        return RecordHr("XblTitleManagedStatsUpdateStatsAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) return RecordHr("XblTitleManagedStatsUpdateStatsAsync", XGR_E_HANDLE);

    std::vector<StoredStat> entries;
    entries.reserve(statisticsCount);
    for (size_t i = 0; i < statisticsCount; ++i) {
        if (!statistics[i].statisticName || !*statistics[i].statisticName) {
            return RecordHr("XblTitleManagedStatsUpdateStatsAsync", XGR_E_INVALIDARG);
        }
        StoredStat st{};
        st.name = statistics[i].statisticName;
        if (statistics[i].statisticType == XblTitleManagedStatType_Number) {
            st.type = "Double";
            st.value = FormatDoubleStat(statistics[i].numberValue);
        } else if (statistics[i].statisticType == XblTitleManagedStatType_String) {
            st.type = "String";
            st.value = statistics[i].stringValue ? statistics[i].stringValue : "";
        } else {
            return RecordHr("XblTitleManagedStatsUpdateStatsAsync", XGR_E_INVALIDARG);
        }
        entries.push_back(std::move(st));
    }

    const uint64_t target_xuid = ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [target_xuid, target_scid, entries = std::move(entries)](
                      XsapiAsyncOp&) -> XGameResult {
        std::vector<std::pair<XblStatisticChangedHandler, void*>> handlers;
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
            EnsureStatsLoadedLocked(s, target_scid, target_xuid);
            auto& map = s.stats_cache[StoreKey{target_scid, target_xuid}];
            for (const auto& entry : entries) map[entry.name] = entry;
            if (!SaveStatsLocked(s, target_scid, target_xuid)) return XGR_E_FAIL;
            for (const auto& [tok, cb] : s.stat_changed_handlers) {
                (void)tok;
                handlers.push_back(cb);
            }
        }
        for (const auto& entry : entries) {
            XblStatisticChangeEventArgs args{};
            args.xboxUserId = target_xuid;
            CopyCString(args.serviceConfigurationId, sizeof(args.serviceConfigurationId),
                        target_scid);
            args.latestStatistic.statisticName = entry.name.c_str();
            args.latestStatistic.statisticType = entry.type.c_str();
            args.latestStatistic.value = entry.value.c_str();
            for (const auto& [fn, user_ctx] : handlers) {
                if (fn) fn(args, user_ctx);
            }
        }
        return XBL_S_OK;
    };

    return RecordHr("XblTitleManagedStatsUpdateStatsAsync",
                    StartXsapiAsync(async, kIdStatsUpdate, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblTitleManagedStatsDeleteStatsAsync(
    XblContextHandle xblContextHandle,
    const char** statisticNames,
    size_t statisticNamesCount,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!statisticNames && statisticNamesCount > 0)) {
        return RecordHr("XblTitleManagedStatsDeleteStatsAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) return RecordHr("XblTitleManagedStatsDeleteStatsAsync", XGR_E_HANDLE);

    std::vector<std::string> names;
    names.reserve(statisticNamesCount);
    for (size_t i = 0; i < statisticNamesCount; ++i) {
        if (statisticNames[i] && *statisticNames[i]) names.emplace_back(statisticNames[i]);
    }
    const uint64_t target_xuid = ctx->xuid;
    const std::string target_scid = ctx->scid;

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [target_xuid, target_scid, names = std::move(names)](
                      XsapiAsyncOp&) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        EnsureStatsLoadedLocked(s, target_scid, target_xuid);
        auto it = s.stats_cache.find(StoreKey{target_scid, target_xuid});
        if (it != s.stats_cache.end()) {
            for (const auto& n : names) {
                it->second.erase(n);
            }
            (void)SaveStatsLocked(s, target_scid, target_xuid);
        }
        return XBL_S_OK;
    };

    return RecordHr("XblTitleManagedStatsDeleteStatsAsync",
                    StartXsapiAsync(async, kIdStatsDelete, std::move(op)));
}

// ===========================================================================
// String Verification (XblString*)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringAsync(
    XblContextHandle xboxLiveContext,
    const char* stringToVerify,
    XAsyncBlock* asyncBlock) {
    if (!xboxLiveContext || !stringToVerify || !asyncBlock) {
        return RecordHr("XblStringVerifyStringAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblStringVerifyStringAsync", XGR_E_HANDLE);
    }

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = sizeof(XblVerifyStringResult);
        return XBL_S_OK;
    };
    op->get_result_fn = [](void* buffer, size_t buffer_size) -> XGameResult {
        if (!buffer || buffer_size < sizeof(XblVerifyStringResult)) {
            return XGR_E_NOT_SUFFICIENT_BUFFER;
        }
        XblVerifyStringResult res{};
        res.resultCode = XblVerifyStringResultCode_Success;
        res.firstOffendingSubstring = nullptr;
        std::memcpy(buffer, &res, sizeof(res));
        return XBL_S_OK;
    };
    return RecordHr("XblStringVerifyStringAsync",
                    StartXsapiAsync(asyncBlock, kIdStringVerify, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringResultSize(
    XAsyncBlock* asyncBlock, size_t* resultSizeInBytes) {
    if (!asyncBlock || !resultSizeInBytes) {
        return RecordHr("XblStringVerifyStringResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(asyncBlock, true);
    if (hr < 0) return RecordHr("XblStringVerifyStringResultSize", hr);
    hr = XAsyncGetResultSize(asyncBlock, resultSizeInBytes);
    return RecordHr("XblStringVerifyStringResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringResult(
    XAsyncBlock* asyncBlock,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBuffer,
    size_t* bufferUsed) {
    if (!asyncBlock || !buffer || !ptrToBuffer) {
        return RecordHr("XblStringVerifyStringResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(asyncBlock, true);
    if (hr < 0) return RecordHr("XblStringVerifyStringResult", hr);
    size_t used = 0;
    hr = XAsyncGetResult(asyncBlock, kIdStringVerify, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblStringVerifyStringResult", hr);
    *ptrToBuffer = static_cast<XblVerifyStringResult*>(buffer);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblStringVerifyStringResult", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsAsync(
    XblContextHandle xboxLiveContext,
    const char** stringsToVerify,
    uint64_t stringsCount,
    XAsyncBlock* asyncBlock) {
    if (!xboxLiveContext || !asyncBlock || (!stringsToVerify && stringsCount > 0ull)) {
        return RecordHr("XblStringVerifyStringsAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xboxLiveContext)) {
        return RecordHr("XblStringVerifyStringsAsync", XGR_E_HANDLE);
    }
    const size_t count = static_cast<size_t>(stringsCount);

    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [count](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = count * sizeof(XblVerifyStringResult);
        return XBL_S_OK;
    };
    op->get_result_fn = [count](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t needed = count * sizeof(XblVerifyStringResult);
        if (needed == 0u) return XBL_S_OK;
        if (!buffer || buffer_size < needed) return XGR_E_NOT_SUFFICIENT_BUFFER;
        auto* arr = static_cast<XblVerifyStringResult*>(buffer);
        for (size_t i = 0; i < count; ++i) {
            arr[i].resultCode = XblVerifyStringResultCode_Success;
            arr[i].firstOffendingSubstring = nullptr;
        }
        return XBL_S_OK;
    };
    return RecordHr("XblStringVerifyStringsAsync",
                    StartXsapiAsync(asyncBlock, kIdStringVerifyMulti, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsResultSize(
    XAsyncBlock* asyncBlock, size_t* resultSizeInBytes) {
    if (!asyncBlock || !resultSizeInBytes) {
        return RecordHr("XblStringVerifyStringsResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(asyncBlock, true);
    if (hr < 0) return RecordHr("XblStringVerifyStringsResultSize", hr);
    hr = XAsyncGetResultSize(asyncBlock, resultSizeInBytes);
    return RecordHr("XblStringVerifyStringsResultSize", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblStringVerifyStringsResult(
    XAsyncBlock* asyncBlock,
    size_t bufferSize,
    void* buffer,
    XblVerifyStringResult** ptrToBufferStrings,
    size_t* stringsCount,
    size_t* bufferUsed) {
    if (!asyncBlock || !ptrToBufferStrings || !stringsCount || (!buffer && bufferSize > 0)) {
        return RecordHr("XblStringVerifyStringsResult", XGR_E_INVALIDARG);
    }
    *ptrToBufferStrings = nullptr;
    *stringsCount = 0;
    XGameResult hr = XAsyncGetStatus(asyncBlock, true);
    if (hr < 0) return RecordHr("XblStringVerifyStringsResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(asyncBlock, &sz);
    if (hr < 0) return RecordHr("XblStringVerifyStringsResult", hr);
    if (sz == 0u) {
        if (bufferUsed) *bufferUsed = 0;
        return RecordHr("XblStringVerifyStringsResult", XBL_S_OK);
    }
    if (!buffer || bufferSize < sz) {
        return RecordHr("XblStringVerifyStringsResult", XGR_E_NOT_SUFFICIENT_BUFFER);
    }
    size_t used = 0;
    hr = XAsyncGetResult(asyncBlock, kIdStringVerifyMulti, bufferSize, buffer, &used);
    if (hr < 0) return RecordHr("XblStringVerifyStringsResult", hr);
    *ptrToBufferStrings = static_cast<XblVerifyStringResult*>(buffer);
    *stringsCount = used / sizeof(XblVerifyStringResult);
    if (bufferUsed) *bufferUsed = used;
    return RecordHr("XblStringVerifyStringsResult", XBL_S_OK);
}

// ===========================================================================
// Expanded Presence (Multi-User, Social Group, Handlers & Subscriptions)
// ===========================================================================
XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForMultipleUsersAsync(
    XblContextHandle xblContextHandle,
    uint64_t* xuids,
    size_t xuidsCount,
    const XblPresenceQueryFilters* filters,
    XAsyncBlock* async) {
    if (!xblContextHandle || !async || (!xuids && xuidsCount > 0)) {
        return RecordHr("XblPresenceGetPresenceForMultipleUsersAsync", XGR_E_INVALIDARG);
    }
    auto ctx = LookupContext(xblContextHandle);
    if (!ctx) return RecordHr("XblPresenceGetPresenceForMultipleUsersAsync", XGR_E_HANDLE);

    std::vector<uint64_t> target_xuids;
    if (xuids && xuidsCount > 0) target_xuids.assign(xuids, xuids + xuidsCount);
    const bool online_only = filters ? filters->onlineOnly : false;

    auto handles_holder = std::make_shared<std::vector<XblPresenceRecordHandle>>();
    auto op = std::make_unique<XsapiAsyncOp>();

    op->work_fn = [ctx, target_xuids = std::move(target_xuids), online_only, handles_holder](
                      XsapiAsyncOp& self) -> XGameResult {
        {
            XsapiState& s = GetState();
            std::lock_guard<std::mutex> lk(s.mutex);
            if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        }
        for (uint64_t x : target_xuids) {
            auto rec = BuildPresenceRecordForXuid(ctx->xuid, x != 0 ? x : ctx->xuid);
            if (online_only && rec->user_state != XblPresenceUserState_Online) continue;
            XblPresenceRecordHandle raw = rec.get();
            {
                XsapiState& s = GetState();
                std::lock_guard<std::mutex> lk(s.mutex);
                s.live_presence_records[raw] = std::move(rec);
            }
            handles_holder->push_back(raw);
        }
        self.required_buffer_size = handles_holder->size() * sizeof(XblPresenceRecordHandle);
        return XBL_S_OK;
    };

    op->get_result_fn = [handles_holder](void* buffer, size_t buffer_size) -> XGameResult {
        const size_t needed = handles_holder->size() * sizeof(XblPresenceRecordHandle);
        if (needed == 0u) return XBL_S_OK;
        if (!buffer || buffer_size < needed) return XGR_E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(buffer, handles_holder->data(), needed);
        handles_holder->clear();
        return XBL_S_OK;
    };

    op->cleanup_unclaimed_fn = [handles_holder]() {
        for (XblPresenceRecordHandle h : *handles_holder) {
            XblPresenceRecordCloseHandle(h);
        }
        handles_holder->clear();
    };

    return RecordHr("XblPresenceGetPresenceForMultipleUsersAsync",
                    StartXsapiAsync(async, kIdPresenceGetMulti, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForMultipleUsersResultCount(
    XAsyncBlock* async, size_t* resultCount) {
    if (!async || !resultCount) {
        return RecordHr("XblPresenceGetPresenceForMultipleUsersResultCount", XGR_E_INVALIDARG);
    }
    *resultCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForMultipleUsersResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForMultipleUsersResultCount", hr);
    *resultCount = sz / sizeof(XblPresenceRecordHandle);
    return RecordHr("XblPresenceGetPresenceForMultipleUsersResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForMultipleUsersResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandles,
    size_t presenceRecordHandlesCount) {
    if (!async || (!presenceRecordHandles && presenceRecordHandlesCount > 0)) {
        return RecordHr("XblPresenceGetPresenceForMultipleUsersResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForMultipleUsersResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForMultipleUsersResult", hr);
    if (sz == 0u) return RecordHr("XblPresenceGetPresenceForMultipleUsersResult", XBL_S_OK);
    hr = XAsyncGetResult(
        async,
        kIdPresenceGetMulti,
        presenceRecordHandlesCount * sizeof(XblPresenceRecordHandle),
        presenceRecordHandles,
        nullptr);
    return RecordHr("XblPresenceGetPresenceForMultipleUsersResult", hr);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForSocialGroupAsync(
    XblContextHandle xblContextHandle,
    const char* socialGroupName,
    uint64_t* /*socialGroupOwnerXuid*/,
    const XblPresenceQueryFilters* /*filters*/,
    XAsyncBlock* async) {
    if (!xblContextHandle || !socialGroupName || !*socialGroupName || !async) {
        return RecordHr("XblPresenceGetPresenceForSocialGroupAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceGetPresenceForSocialGroupAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        XsapiState& s = GetState();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.initialized) return XBL_E_NOT_INITIALIZED;
        self.required_buffer_size = 0;
        return XBL_S_OK;
    };
    op->get_result_fn = [](void*, size_t) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblPresenceGetPresenceForSocialGroupAsync",
                    StartXsapiAsync(async, kIdPresenceGetGroup, std::move(op)));
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForSocialGroupResultCount(
    XAsyncBlock* async, size_t* resultCount) {
    if (!async || !resultCount) {
        return RecordHr("XblPresenceGetPresenceForSocialGroupResultCount", XGR_E_INVALIDARG);
    }
    *resultCount = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForSocialGroupResultCount", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForSocialGroupResultCount", hr);
    *resultCount = sz / sizeof(XblPresenceRecordHandle);
    return RecordHr("XblPresenceGetPresenceForSocialGroupResultCount", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceGetPresenceForSocialGroupResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandles,
    size_t presenceRecordHandlesCount) {
    if (!async || (!presenceRecordHandles && presenceRecordHandlesCount > 0)) {
        return RecordHr("XblPresenceGetPresenceForSocialGroupResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForSocialGroupResult", hr);
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr < 0) return RecordHr("XblPresenceGetPresenceForSocialGroupResult", hr);
    if (sz == 0u) return RecordHr("XblPresenceGetPresenceForSocialGroupResult", XBL_S_OK);
    hr = XAsyncGetResult(
        async,
        kIdPresenceGetGroup,
        presenceRecordHandlesCount * sizeof(XblPresenceRecordHandle),
        presenceRecordHandles,
        nullptr);
    return RecordHr("XblPresenceGetPresenceForSocialGroupResult", hr);
}

XblFunctionContext OPENSCARLETT_XSAPI_CALL XblPresenceAddDevicePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblPresenceDevicePresenceChangedHandler handler,
    void* context) {
    if (!xblContextHandle || !handler || !LookupContext(xblContextHandle)) {
        RecordHr("XblPresenceAddDevicePresenceChangedHandler", XGR_E_INVALIDARG);
        return 0;
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    const XblFunctionContext token = s.next_handler_token++;
    s.device_presence_handlers[token] = {handler, context};
    RecordOk("XblPresenceAddDevicePresenceChangedHandler");
    return token;
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRemoveDevicePresenceChangedHandler(
    XblContextHandle xblContextHandle, XblFunctionContext token) {
    if (!xblContextHandle) {
        return RecordHr("XblPresenceRemoveDevicePresenceChangedHandler", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceRemoveDevicePresenceChangedHandler", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.device_presence_handlers.erase(token);
    return RecordHr("XblPresenceRemoveDevicePresenceChangedHandler", XBL_S_OK);
}

XblFunctionContext OPENSCARLETT_XSAPI_CALL XblPresenceAddTitlePresenceChangedHandler(
    XblContextHandle xblContextHandle,
    XblPresenceTitlePresenceChangedHandler handler,
    void* context) {
    if (!xblContextHandle || !handler || !LookupContext(xblContextHandle)) {
        RecordHr("XblPresenceAddTitlePresenceChangedHandler", XGR_E_INVALIDARG);
        return 0;
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    const XblFunctionContext token = s.next_handler_token++;
    s.title_presence_handlers[token] = {handler, context};
    RecordOk("XblPresenceAddTitlePresenceChangedHandler");
    return token;
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceRemoveTitlePresenceChangedHandler(
    XblContextHandle xblContextHandle, XblFunctionContext token) {
    if (!xblContextHandle) {
        return RecordHr("XblPresenceRemoveTitlePresenceChangedHandler", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceRemoveTitlePresenceChangedHandler", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.title_presence_handlers.erase(token);
    return RecordHr("XblPresenceRemoveTitlePresenceChangedHandler", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceSubscribeToDevicePresenceChange(
    XblContextHandle xblContextHandle,
    uint64_t xuid,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle) {
    if (!xblContextHandle || !subscriptionHandle) {
        return RecordHr("XblPresenceSubscribeToDevicePresenceChange", XGR_E_INVALIDARG);
    }
    *subscriptionHandle = nullptr;
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceSubscribeToDevicePresenceChange", XGR_E_HANDLE);
    }
    auto sub = std::make_unique<XblRealTimeActivitySubscription>();
    sub->kind = "device_presence";
    sub->xuid = xuid;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblPresenceSubscribeToDevicePresenceChange", XBL_E_NOT_INITIALIZED);
    }
    sub->id = s.next_sub_id++;
    XblRealTimeActivitySubscriptionHandle raw = sub.get();
    s.live_subscriptions[raw] = std::move(sub);
    *subscriptionHandle = raw;
    return RecordHr("XblPresenceSubscribeToDevicePresenceChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceUnsubscribeFromDevicePresenceChange(
    XblContextHandle xblContextHandle,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle) {
    if (!xblContextHandle || !subscriptionHandle) {
        return RecordHr("XblPresenceUnsubscribeFromDevicePresenceChange", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceUnsubscribeFromDevicePresenceChange", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.live_subscriptions.erase(subscriptionHandle);
    return RecordHr("XblPresenceUnsubscribeFromDevicePresenceChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceSubscribeToTitlePresenceChange(
    XblContextHandle xblContextHandle,
    uint64_t xuid,
    uint32_t titleId,
    XblRealTimeActivitySubscriptionHandle* subscriptionHandle) {
    if (!xblContextHandle || !subscriptionHandle) {
        return RecordHr("XblPresenceSubscribeToTitlePresenceChange", XGR_E_INVALIDARG);
    }
    *subscriptionHandle = nullptr;
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceSubscribeToTitlePresenceChange", XGR_E_HANDLE);
    }
    auto sub = std::make_unique<XblRealTimeActivitySubscription>();
    sub->kind = "title_presence";
    sub->xuid = xuid;
    sub->title_id = titleId;
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!s.initialized) {
        return RecordHr("XblPresenceSubscribeToTitlePresenceChange", XBL_E_NOT_INITIALIZED);
    }
    sub->id = s.next_sub_id++;
    XblRealTimeActivitySubscriptionHandle raw = sub.get();
    s.live_subscriptions[raw] = std::move(sub);
    *subscriptionHandle = raw;
    return RecordHr("XblPresenceSubscribeToTitlePresenceChange", XBL_S_OK);
}

XGameResult OPENSCARLETT_XSAPI_CALL XblPresenceUnsubscribeFromTitlePresenceChange(
    XblContextHandle xblContextHandle,
    XblRealTimeActivitySubscriptionHandle subscriptionHandle) {
    if (!xblContextHandle || !subscriptionHandle) {
        return RecordHr("XblPresenceUnsubscribeFromTitlePresenceChange", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContextHandle)) {
        return RecordHr("XblPresenceUnsubscribeFromTitlePresenceChange", XGR_E_HANDLE);
    }
    XsapiState& s = GetState();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.live_subscriptions.erase(subscriptionHandle);
    return RecordHr("XblPresenceUnsubscribeFromTitlePresenceChange", XBL_S_OK);
}

// ===========================================================================
// Multiplayer Activity (Non-Blocking Offline Simulation)
// ===========================================================================
static const char kIdMpaFlushRecent[] = "XblMultiplayerActivityFlushRecentPlayersAsync";
static const char kIdMpaSetActivity[] = "XblMultiplayerActivitySetActivityAsync";
static const char kIdMpaGetActivity[] = "XblMultiplayerActivityGetActivityAsync";
static const char kIdMpaDeleteActivity[] = "XblMultiplayerActivityDeleteActivityAsync";
static const char kIdMpaSendInvites[] = "XblMultiplayerActivitySendInvitesAsync";

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityUpdateRecentPlayers(
    XblContextHandle xblContext,
    const void* updates,
    size_t updatesCount) {
    if (!xblContext || (!updates && updatesCount > 0)) {
        return RecordHr("XblMultiplayerActivityUpdateRecentPlayers", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivityUpdateRecentPlayers", XGR_E_HANDLE);
    }
    return RecordHr("XblMultiplayerActivityUpdateRecentPlayers", XBL_S_OK);
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityFlushRecentPlayersAsync(
    XblContextHandle xblContext,
    XAsyncBlock* async) {
    if (!xblContext || !async) {
        return RecordHr("XblMultiplayerActivityFlushRecentPlayersAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivityFlushRecentPlayersAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblMultiplayerActivityFlushRecentPlayersAsync",
                    StartXsapiAsync(async, kIdMpaFlushRecent, std::move(op)));
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivitySetActivityAsync(
    XblContextHandle xblContext,
    const void* activityInfo,
    bool /*allowCrossPlatformJoin*/,
    XAsyncBlock* async) {
    if (!xblContext || !activityInfo || !async) {
        return RecordHr("XblMultiplayerActivitySetActivityAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivitySetActivityAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblMultiplayerActivitySetActivityAsync",
                    StartXsapiAsync(async, kIdMpaSetActivity, std::move(op)));
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityGetActivityAsync(
    XblContextHandle xblContext,
    const uint64_t* xuids,
    size_t xuidsCount,
    XAsyncBlock* async) {
    if (!xblContext || !async || (!xuids && xuidsCount > 0)) {
        return RecordHr("XblMultiplayerActivityGetActivityAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivityGetActivityAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp& self) -> XGameResult {
        self.required_buffer_size = 0;
        return XBL_S_OK;
    };
    op->get_result_fn = [](void*, size_t) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblMultiplayerActivityGetActivityAsync",
                    StartXsapiAsync(async, kIdMpaGetActivity, std::move(op)));
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityGetActivityResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes) {
    if (!async || !resultSizeInBytes) {
        return RecordHr("XblMultiplayerActivityGetActivityResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblMultiplayerActivityGetActivityResultSize", hr);
    *resultSizeInBytes = 0;
    return RecordHr("XblMultiplayerActivityGetActivityResultSize", XBL_S_OK);
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityGetActivityResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    void** ptrToBufferResults,
    size_t* resultCount,
    size_t* bufferUsed) {
    if (!async || !ptrToBufferResults || !resultCount) {
        return RecordHr("XblMultiplayerActivityGetActivityResult", XGR_E_INVALIDARG);
    }
    (void)bufferSize;
    (void)buffer;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) return RecordHr("XblMultiplayerActivityGetActivityResult", hr);
    *ptrToBufferResults = nullptr;
    *resultCount = 0;
    if (bufferUsed) *bufferUsed = 0;
    return RecordHr("XblMultiplayerActivityGetActivityResult", XBL_S_OK);
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivityDeleteActivityAsync(
    XblContextHandle xblContext,
    XAsyncBlock* async) {
    if (!xblContext || !async) {
        return RecordHr("XblMultiplayerActivityDeleteActivityAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivityDeleteActivityAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblMultiplayerActivityDeleteActivityAsync",
                    StartXsapiAsync(async, kIdMpaDeleteActivity, std::move(op)));
}

OPENSCARLETT_XSAPI_API XGameResult OPENSCARLETT_XSAPI_CALL
XblMultiplayerActivitySendInvitesAsync(
    XblContextHandle xblContext,
    const uint64_t* xuids,
    size_t xuidsCount,
    bool /*allowCrossPlatformJoin*/,
    const char* /*connectionString*/,
    XAsyncBlock* async) {
    if (!xblContext || !async || (!xuids && xuidsCount > 0)) {
        return RecordHr("XblMultiplayerActivitySendInvitesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupContext(xblContext)) {
        return RecordHr("XblMultiplayerActivitySendInvitesAsync", XGR_E_HANDLE);
    }
    auto op = std::make_unique<XsapiAsyncOp>();
    op->work_fn = [](XsapiAsyncOp&) -> XGameResult { return XBL_S_OK; };
    return RecordHr("XblMultiplayerActivitySendInvitesAsync",
                    StartXsapiAsync(async, kIdMpaSendInvites, std::move(op)));
}

}  // extern "C"
