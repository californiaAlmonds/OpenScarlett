#define OPENSCARLETT_BUILDING_LIBHTTPCLIENT 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr uint32_t kMagicHttpCall = 0x48434341u;    // 'HCCA'
constexpr uint32_t kMagicWebSocket = 0x48435753u;   // 'HCWS'

struct HCCallObject {
    uint32_t magic = kMagicHttpCall;
    std::atomic<int32_t> ref_count{1};
    std::mutex mutex;
    uint64_t call_id = 0;

    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::vector<uint8_t> request_body;
    uint32_t timeout_seconds = 30u;
    uint32_t retry_delay_seconds = 2u;
    uint32_t timeout_window_seconds = 20u;
    uint32_t retry_cache_id = 0u;
    bool retry_allowed = true;
    bool ssl_validation = true;
    bool trace_call = true;
    void* user_context = nullptr;

    bool completed = false;
    uint32_t response_status = 0;
    int32_t network_error_code = 0;
    uint32_t platform_network_error_code = 0;
    std::string platform_network_error_message;
    std::vector<std::pair<std::string, std::string>> response_headers;
    std::vector<uint8_t> response_body;
    std::string response_string;
};

struct HCWebSocketObject;
typedef HCWebSocketObject* HCWebsocketHandle;

enum HCWebSocketCloseStatus : uint32_t {
    HCWebSocketCloseStatus_Normal = 1000u,
    HCWebSocketCloseStatus_GoingAway = 1001u,
    HCWebSocketCloseStatus_ProtocolError = 1002u,
    HCWebSocketCloseStatus_Unsupported = 1003u,
    HCWebSocketCloseStatus_Abnormal = 1006u,
};

struct WebSocketCompletionResult {
    HCWebsocketHandle websocket;
    int32_t errorCode;
    uint32_t platformErrorCode;
};

typedef void(OS_SHIM_CALL* HCWebSocketMessageFunction)(
    HCWebsocketHandle websocket,
    const char* incomingBodyString,
    void* functionContext);
typedef void(OS_SHIM_CALL* HCWebSocketBinaryMessageFunction)(
    HCWebsocketHandle websocket,
    const uint8_t* payloadBytes,
    uint32_t payloadSize,
    void* functionContext);
typedef void(OS_SHIM_CALL* HCWebSocketCloseEventFunction)(
    HCWebsocketHandle websocket,
    HCWebSocketCloseStatus closeStatus,
    void* functionContext);

typedef int32_t(OS_SHIM_CALL* HCWebSocketConnectFunction)(
    const char* uri,
    const char* subProtocol,
    HCWebsocketHandle websocket,
    XAsyncBlock* asyncBlock,
    void* context);
typedef int32_t(OS_SHIM_CALL* HCWebSocketSendMessageFunction)(
    HCWebsocketHandle websocket,
    const char* message,
    XAsyncBlock* asyncBlock,
    void* context);
typedef int32_t(OS_SHIM_CALL* HCWebSocketSendBinaryMessageFunction)(
    HCWebsocketHandle websocket,
    const uint8_t* payloadBytes,
    uint32_t payloadSize,
    XAsyncBlock* asyncBlock,
    void* context);
typedef int32_t(OS_SHIM_CALL* HCWebSocketDisconnectFunction)(
    HCWebsocketHandle websocket,
    HCWebSocketCloseStatus closeStatus,
    void* context);

struct HCWebSocketObject {
    uint32_t magic = kMagicWebSocket;
    std::atomic<int32_t> ref_count{1};
    std::mutex mutex;
    std::string uri;
    std::string sub_protocol;
    std::string proxy_uri;
    std::vector<std::pair<std::string, std::string>> headers;
    size_t max_receive_buffer_size = 64u * 1024u;
    bool connected = false;
    HCWebSocketMessageFunction message_fn = nullptr;
    HCWebSocketBinaryMessageFunction binary_message_fn = nullptr;
    HCWebSocketCloseEventFunction close_fn = nullptr;
    void* fn_context = nullptr;
    std::vector<std::string> sent_text_messages;
    std::vector<std::vector<uint8_t>> sent_binary_messages;
};

namespace {

constexpr const char* kModName = "libHttpClient.GDK.dll";
constexpr const char* kLibVersion = "1.0.0-OpenScarlett";

struct MockRule {
    std::string method;       // empty = match any method
    std::string url_prefix;   // exact or prefix match
    uint32_t status_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
};

struct HcState {
    std::mutex mutex;
    uint32_t init_count = 0;
    uint64_t next_call_id = 1ull;
    bool trace_to_debugger = false;
    HCTraceLevel trace_level = HCTraceLevel_Off;
    std::unordered_map<HCCallObject*, std::shared_ptr<HCCallObject>> live_calls;
    std::unordered_map<HCWebSocketObject*, std::shared_ptr<HCWebSocketObject>> live_websockets;
    std::vector<MockRule> mocks;

    HCWebSocketConnectFunction ws_connect_fn = nullptr;
    HCWebSocketSendMessageFunction ws_send_text_fn = nullptr;
    HCWebSocketSendBinaryMessageFunction ws_send_bin_fn = nullptr;
    HCWebSocketDisconnectFunction ws_disconnect_fn = nullptr;
    void* ws_fn_context = nullptr;
};

HcState& GetHc() {
    static HcState* s = new HcState();
    return *s;
}

inline int32_t RecordHr(const char* sym, int32_t hr) {
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

bool EqualsAsciiCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool IsValidHttpToken(const char* s) {
    if (!s || !*s) {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
        const unsigned char c = *p;
        const bool is_alnum =
            (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool is_tchar_sym =
            c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
            c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
            c == '`' || c == '|' || c == '~';
        if (!is_alnum && !is_tchar_sym) {
            return false;
        }
    }
    return true;
}

bool IsValidHttpHeaderName(const char* name) {
    return IsValidHttpToken(name);
}

bool IsValidHttpHeaderValue(const char* value) {
    if (!value) {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p != '\0'; ++p) {
        const unsigned char c = *p;
        if (c == '\r' || c == '\n' || c == 0x7Fu || (c < 0x20u && c != '\t')) {
            return false;
        }
    }
    return true;
}

bool HasSchemeCaseInsensitive(const char* url, const char* scheme) {
    if (!url || !scheme) {
        return false;
    }
    while (*scheme != '\0') {
        if (*url == '\0') {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(*url)) !=
            std::tolower(static_cast<unsigned char>(*scheme))) {
            return false;
        }
        ++url;
        ++scheme;
    }
    return true;
}

bool IsValidSchemeUrl(const char* url, const char* scheme1, const char* scheme2) {
    if (!url || !*url) {
        return false;
    }
    size_t scheme_len = 0u;
    if (HasSchemeCaseInsensitive(url, scheme1)) {
        scheme_len = std::strlen(scheme1);
    } else if (HasSchemeCaseInsensitive(url, scheme2)) {
        scheme_len = std::strlen(scheme2);
    } else {
        return false;
    }
    const char* host_start = url + scheme_len;
    if (*host_start == '\0' || *host_start == '/' || *host_start == '?' || *host_start == '#') {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(url); *p != '\0'; ++p) {
        const unsigned char c = *p;
        if (c <= 0x20u || c == 0x7Fu) {
            return false;
        }
    }
    return true;
}

bool IsValidHttpUrl(const char* url) {
    return IsValidSchemeUrl(url, "http://", "https://");
}

bool IsValidWebSocketUrl(const char* url) {
    return IsValidSchemeUrl(url, "ws://", "wss://");
}

std::shared_ptr<HCCallObject> LookupCall(HCCallHandle handle) {
    if (!handle) return nullptr;
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_calls.find(handle);
    if (it == st.live_calls.end() || !it->second || it->second->magic != kMagicHttpCall) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<HCWebSocketObject> LookupWebSocket(HCWebsocketHandle handle) {
    if (!handle) return nullptr;
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_websockets.find(handle);
    if (it == st.live_websockets.end() || !it->second || it->second->magic != kMagicWebSocket) {
        return nullptr;
    }
    return it->second;
}

const char kIdHcPerform[] = "HCHttpCallPerformAsync";
const char kIdHcCleanup[] = "HCCleanupAsync";
const char kIdWsConnect[] = "HCWebSocketConnectAsync";
const char kIdWsSend[] = "HCWebSocketSendMessageAsync";
const char kIdWsSendBin[] = "HCWebSocketSendBinaryMessageAsync";

struct WsAsyncCtx {
    std::shared_ptr<HCWebSocketObject> ws;
    std::string uri;
    std::string sub_protocol;
    std::string text_payload;
    std::vector<uint8_t> bin_payload;
    uint32_t op_kind = 0u;  // 0=connect, 1=send_text, 2=send_binary
    WebSocketCompletionResult result{};
};

XGameResult OPENSCARLETT_GDK_CALL HcWebSocketAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<WsAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (!ctx || !ctx->ws) {
                XAsyncComplete(data->async, XGR_E_INVALIDARG, 0);
                return XGR_E_PENDING;
            }
            {
                std::lock_guard<std::mutex> lk(ctx->ws->mutex);
                if (ctx->op_kind == 0u) {
                    ctx->ws->uri = ctx->uri;
                    ctx->ws->sub_protocol = ctx->sub_protocol;
                    ctx->ws->connected = true;
                } else if (ctx->op_kind == 1u) {
                    ctx->ws->sent_text_messages.push_back(ctx->text_payload);
                } else if (ctx->op_kind == 2u) {
                    ctx->ws->sent_binary_messages.push_back(ctx->bin_payload);
                }
            }
            ctx->result.websocket = ctx->ws.get();
            ctx->result.errorCode = OS_SHIM_S_OK;
            ctx->result.platformErrorCode = 0u;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(WebSocketCompletionResult));
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!ctx || !data->buffer || data->bufferSize < sizeof(WebSocketCompletionResult)) {
                return XGR_E_INVALIDARG;
            }
            std::memcpy(data->buffer, &ctx->result, sizeof(WebSocketCompletionResult));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct HcPerformAsyncCtx {
    std::shared_ptr<HCCallObject> call;
};

XGameResult OPENSCARLETT_GDK_CALL HcPerformProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<HcPerformAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (!ctx || !ctx->call) {
                XAsyncComplete(data->async, XGR_E_INVALIDARG, 0);
                return XGR_E_PENDING;
            }
            std::string method;
            std::string url;
            size_t req_body_size = 0;
            {
                std::lock_guard<std::mutex> lk(ctx->call->mutex);
                method = ctx->call->method;
                url = ctx->call->url;
                req_body_size = ctx->call->request_body.size();
            }

            // Look up most recently registered matching mock rule.
            bool matched_mock = false;
            MockRule chosen{};
            {
                auto& st = GetHc();
                std::lock_guard<std::mutex> lk(st.mutex);
                for (size_t i = st.mocks.size(); i > 0; --i) {
                    const auto& rule = st.mocks[i - 1u];
                    const bool method_ok =
                        rule.method.empty() || EqualsAsciiCaseInsensitive(rule.method, method);
                    const bool url_ok =
                        rule.url_prefix.empty() || url.rfind(rule.url_prefix, 0) == 0;
                    if (method_ok && url_ok) {
                        chosen = rule;
                        matched_mock = true;
                        break;
                    }
                }
            }

            uint32_t resp_status = 200u;
            std::string content_type = "application/json";
            std::vector<uint8_t> resp_body;

            if (matched_mock) {
                resp_status = chosen.status_code;
                content_type = chosen.content_type.empty()
                                   ? "application/octet-stream"
                                   : chosen.content_type;
                resp_body = std::move(chosen.body);
            } else {
                std::ostringstream oss;
                oss << "{\"status\":\"ok\",\"offline\":true,\"method\":\""
                    << EscapeJsonString(method) << "\",\"url\":\""
                    << EscapeJsonString(url) << "\",\"requestBytes\":"
                    << req_body_size << "}";
                const std::string json = oss.str();
                resp_body.assign(json.begin(), json.end());
            }

            {
                std::lock_guard<std::mutex> lk(ctx->call->mutex);
                ctx->call->response_status = resp_status;
                ctx->call->response_body = std::move(resp_body);
                ctx->call->response_string.assign(
                    reinterpret_cast<const char*>(ctx->call->response_body.data()),
                    ctx->call->response_body.size());
                ctx->call->response_headers.clear();
                ctx->call->response_headers.emplace_back("Content-Type", content_type);
                ctx->call->response_headers.emplace_back(
                    "Content-Length", std::to_string(ctx->call->response_body.size()));
                ctx->call->response_headers.emplace_back("Server", "OpenScarlett-Offline-HTTP/1.0");
                ctx->call->response_headers.emplace_back(
                    "X-OpenScarlett-Mock", matched_mock ? "registered" : "loopback");
                ctx->call->completed = true;
            }

            XAsyncComplete(data->async, XGR_S_OK, 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult OPENSCARLETT_GDK_CALL HcCleanupProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            auto& st = GetHc();
            std::lock_guard<std::mutex> lk(st.mutex);
            if (st.init_count > 0u) {
                --st.init_count;
            }
            if (st.init_count == 0u) {
                st.mocks.clear();
            }
            XAsyncComplete(data->async, XGR_S_OK, 0);
            return XGR_E_PENDING;
        }
        default:
            return XGR_S_OK;
    }
}

}  // namespace

extern "C" {

int32_t OS_SHIM_CALL HCInitialize(const HCInitArgs* /*args*/) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordHr("HCInitialize", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCCleanup(void) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    if (st.init_count == 0u) {
        st.mocks.clear();
    }
    RecordOk("HCCleanup");
}

int32_t OS_SHIM_CALL HCCleanupAsync(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("HCCleanupAsync", OS_SHIM_E_INVALIDARG);
    }
    XGameResult hr = XAsyncBegin(async, nullptr, kIdHcCleanup, kIdHcCleanup, &HcCleanupProvider);
    if (hr < 0) {
        return RecordHr("HCCleanupAsync", hr);
    }
    hr = XAsyncSchedule(async, 0u);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("HCCleanupAsync", hr);
}

int32_t OS_SHIM_CALL HCGetLibVersion(const char** version) {
    if (!version) {
        return RecordHr("HCGetLibVersion", OS_SHIM_E_INVALIDARG);
    }
    *version = kLibVersion;
    return RecordHr("HCGetLibVersion", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCTraceSetTraceToDebugger(bool traceToDebugger) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.trace_to_debugger = traceToDebugger;
    RecordOk("HCTraceSetTraceToDebugger");
}

int32_t OS_SHIM_CALL HCSettingsSetTraceLevel(HCTraceLevel traceLevel) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.trace_level = traceLevel;
    return RecordHr("HCSettingsSetTraceLevel", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallCreate(HCCallHandle* call) {
    if (!call) {
        return RecordHr("HCHttpCallCreate", OS_SHIM_E_INVALIDARG);
    }
    *call = nullptr;
    auto obj = std::make_shared<HCCallObject>();
    HCCallHandle raw = obj.get();
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        obj->call_id = st.next_call_id++;
        st.live_calls[raw] = std::move(obj);
    }
    *call = raw;
    return RecordHr("HCHttpCallCreate", OS_SHIM_S_OK);
}

HCCallHandle OS_SHIM_CALL HCHttpCallDuplicateHandle(HCCallHandle call) {
    auto obj = LookupCall(call);
    if (!obj) {
        RecordHr("HCHttpCallDuplicateHandle", OS_SHIM_E_HANDLE);
        return nullptr;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    RecordOk("HCHttpCallDuplicateHandle");
    return call;
}

int32_t OS_SHIM_CALL HCHttpCallCloseHandle(HCCallHandle call) {
    if (!call) {
        return RecordHr("HCHttpCallCloseHandle", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_calls.find(call);
    if (it == st.live_calls.end()) {
        return RecordHr("HCHttpCallCloseHandle", OS_SHIM_E_HANDLE);
    }
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        st.live_calls.erase(it);
    }
    return RecordHr("HCHttpCallCloseHandle", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetUrl(
    HCCallHandle call,
    const char* method,
    const char* url) {
    if (!call || !IsValidHttpToken(method) || !IsValidHttpUrl(url)) {
        return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->method = method;
    obj->url = url;
    return RecordHr("HCHttpCallRequestSetUrl", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* headerName,
    const char* headerValue,
    bool /*allowTracing*/) {
    if (!call || !IsValidHttpHeaderName(headerName) || !IsValidHttpHeaderValue(headerValue)) {
        return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (auto& kv : obj->request_headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            kv.second = headerValue;
            return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_S_OK);
        }
    }
    obj->request_headers.emplace_back(headerName, headerValue);
    return RecordHr("HCHttpCallRequestSetHeader", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* requestBodyBytes,
    uint32_t requestBodySize) {
    if (!call || (requestBodySize > 0u && !requestBodyBytes)) {
        return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    if (requestBodySize > 0u) {
        obj->request_body.assign(requestBodyBytes, requestBodyBytes + requestBodySize);
    } else {
        obj->request_body.clear();
    }
    return RecordHr("HCHttpCallRequestSetRequestBodyBytes", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call,
    const char* requestBodyString) {
    if (!call || !requestBodyString) {
        return RecordHr("HCHttpCallRequestSetRequestBodyString", OS_SHIM_E_INVALIDARG);
    }
    const size_t len = std::strlen(requestBodyString);
    const int32_t hr = HCHttpCallRequestSetRequestBodyBytes(
        call,
        reinterpret_cast<const uint8_t*>(requestBodyString),
        static_cast<uint32_t>(len));
    return RecordHr("HCHttpCallRequestSetRequestBodyString", hr);
}

int32_t OS_SHIM_CALL HCHttpCallPerformAsync(HCCallHandle call, XAsyncBlock* asyncBlock) {
    if (!call || !asyncBlock) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_HANDLE);
    }
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        if (obj->url.empty()) {
            return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_INVALIDARG);
        }
    }

    auto* ctx = new (std::nothrow) HcPerformAsyncCtx{std::move(obj)};
    if (!ctx) {
        return RecordHr("HCHttpCallPerformAsync", OS_SHIM_E_OUTOFMEMORY);
    }

    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kIdHcPerform, kIdHcPerform, &HcPerformProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("HCHttpCallPerformAsync", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0u);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("HCHttpCallPerformAsync", hr);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetStatusCode(
    HCCallHandle call,
    uint32_t* statusCode) {
    if (!call || !statusCode) {
        return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *statusCode = obj->response_status;
    return RecordHr("HCHttpCallResponseGetStatusCode", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseString(
    HCCallHandle call,
    const char** responseString) {
    if (!call || !responseString) {
        return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *responseString = obj->response_string.c_str();
    return RecordHr("HCHttpCallResponseGetResponseString", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytesSize(
    HCCallHandle call,
    size_t* bufferSize) {
    if (!call || !bufferSize) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *bufferSize = obj->response_body.size();
    return RecordHr("HCHttpCallResponseGetResponseBodyBytesSize", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed) {
    if (!call || (!buffer && (bufferSize > 0u || !bufferUsed))) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    const size_t n = obj->response_body.size();
    if (bufferUsed) {
        *bufferUsed = n;
    }
    if (!buffer) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes",
                        n == 0u ? OS_SHIM_S_OK : OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    }
    if (bufferSize < n) {
        return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_E_NOT_SUFFICIENT_BUFFER);
    }
    if (n > 0u) {
        std::memcpy(buffer, obj->response_body.data(), n);
    }
    return RecordHr("HCHttpCallResponseGetResponseBodyBytes", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* headerName,
    const char** headerValue) {
    if (!call || !IsValidHttpHeaderName(headerName) || !headerValue) {
        return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_E_INVALIDARG);
    }
    *headerValue = nullptr;
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (const auto& kv : obj->response_headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            *headerValue = kv.second.c_str();
            return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_S_OK);
        }
    }
    return RecordHr("HCHttpCallResponseGetHeader", OS_SHIM_S_OK);
}

int32_t OS_SHIM_CALL HCAddMockResponseForTest(
    const char* method,
    const char* urlPrefix,
    uint32_t statusCode,
    const char* contentType,
    const uint8_t* body,
    size_t bodySize) {
    if (bodySize > 0u && !body) {
        return RecordHr("HCAddMockResponseForTest", OS_SHIM_E_INVALIDARG);
    }
    MockRule rule{};
    rule.method = method ? method : "";
    rule.url_prefix = urlPrefix ? urlPrefix : "";
    rule.status_code = statusCode == 0u ? 200u : statusCode;
    rule.content_type = (contentType && *contentType) ? contentType : "application/json";
    if (bodySize > 0u) {
        rule.body.assign(body, body + bodySize);
    }
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.mocks.push_back(std::move(rule));
    return RecordHr("HCAddMockResponseForTest", OS_SHIM_S_OK);
}

void OS_SHIM_CALL HCClearMockResponsesForTest(void) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.mocks.clear();
    RecordOk("HCClearMockResponsesForTest");
}

// ===========================================================================
// Extended HCHttpCall Request/Response Getters & Setters (Offline Mode)
// ===========================================================================
OS_HC_API uint64_t OS_SHIM_CALL HCHttpCallGetId(HCCallHandle call) {
    auto obj = LookupCall(call);
    if (!obj) {
        RecordHr("HCHttpCallGetId", OS_SHIM_E_HANDLE);
        return 0ull;
    }
    RecordOk("HCHttpCallGetId");
    return obj->call_id;
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallSetTracing(HCCallHandle call, bool traceCall) {
    if (!call) return RecordHr("HCHttpCallSetTracing", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallSetTracing", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->trace_call = traceCall;
    return RecordHr("HCHttpCallSetTracing", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallSetContext(HCCallHandle call, void* context) {
    if (!call) return RecordHr("HCHttpCallSetContext", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallSetContext", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->user_context = context;
    return RecordHr("HCHttpCallSetContext", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallGetContext(HCCallHandle call, void** context) {
    if (!call || !context) return RecordHr("HCHttpCallGetContext", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallGetContext", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *context = obj->user_context;
    return RecordHr("HCHttpCallGetContext", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetTimeout(
    HCCallHandle call,
    uint32_t timeoutInSeconds) {
    if (!call) return RecordHr("HCHttpCallRequestSetTimeout", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetTimeout", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->timeout_seconds = timeoutInSeconds;
    return RecordHr("HCHttpCallRequestSetTimeout", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetTimeout(
    HCCallHandle call,
    uint32_t* timeoutInSeconds) {
    if (!call || !timeoutInSeconds) return RecordHr("HCHttpCallRequestGetTimeout", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetTimeout", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *timeoutInSeconds = obj->timeout_seconds;
    return RecordHr("HCHttpCallRequestGetTimeout", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetRetryAllowed(
    HCCallHandle call,
    bool retryAllowed) {
    if (!call) return RecordHr("HCHttpCallRequestSetRetryAllowed", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetRetryAllowed", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->retry_allowed = retryAllowed;
    return RecordHr("HCHttpCallRequestSetRetryAllowed", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetRetryAllowed(
    HCCallHandle call,
    bool* retryAllowed) {
    if (!call || !retryAllowed) return RecordHr("HCHttpCallRequestGetRetryAllowed", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetRetryAllowed", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *retryAllowed = obj->retry_allowed;
    return RecordHr("HCHttpCallRequestGetRetryAllowed", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetRetryDelay(
    HCCallHandle call,
    uint32_t retryDelayInSeconds) {
    if (!call) return RecordHr("HCHttpCallRequestSetRetryDelay", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetRetryDelay", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->retry_delay_seconds = retryDelayInSeconds;
    return RecordHr("HCHttpCallRequestSetRetryDelay", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetRetryDelay(
    HCCallHandle call,
    uint32_t* retryDelayInSeconds) {
    if (!call || !retryDelayInSeconds) return RecordHr("HCHttpCallRequestGetRetryDelay", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetRetryDelay", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *retryDelayInSeconds = obj->retry_delay_seconds;
    return RecordHr("HCHttpCallRequestGetRetryDelay", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetTimeoutWindow(
    HCCallHandle call,
    uint32_t timeoutWindowInSeconds) {
    if (!call) return RecordHr("HCHttpCallRequestSetTimeoutWindow", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetTimeoutWindow", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->timeout_window_seconds = timeoutWindowInSeconds;
    return RecordHr("HCHttpCallRequestSetTimeoutWindow", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetTimeoutWindow(
    HCCallHandle call,
    uint32_t* timeoutWindowInSeconds) {
    if (!call || !timeoutWindowInSeconds) {
        return RecordHr("HCHttpCallRequestGetTimeoutWindow", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetTimeoutWindow", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *timeoutWindowInSeconds = obj->timeout_window_seconds;
    return RecordHr("HCHttpCallRequestGetTimeoutWindow", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetRetryCacheId(
    HCCallHandle call,
    uint32_t retryAfterCacheId) {
    if (!call) return RecordHr("HCHttpCallRequestSetRetryCacheId", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetRetryCacheId", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->retry_cache_id = retryAfterCacheId;
    return RecordHr("HCHttpCallRequestSetRetryCacheId", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestSetSSLValidation(
    HCCallHandle call,
    bool sslValidation) {
    if (!call) return RecordHr("HCHttpCallRequestSetSSLValidation", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestSetSSLValidation", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->ssl_validation = sslValidation;
    return RecordHr("HCHttpCallRequestSetSSLValidation", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetUrl(
    HCCallHandle call,
    const char** method,
    const char** url) {
    if (!call || !method || !url) return RecordHr("HCHttpCallRequestGetUrl", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetUrl", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *method = obj->method.c_str();
    *url = obj->url.c_str();
    return RecordHr("HCHttpCallRequestGetUrl", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetNumHeaders(
    HCCallHandle call,
    uint32_t* numHeaders) {
    if (!call || !numHeaders) return RecordHr("HCHttpCallRequestGetNumHeaders", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetNumHeaders", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *numHeaders = static_cast<uint32_t>(obj->request_headers.size());
    return RecordHr("HCHttpCallRequestGetNumHeaders", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetHeaderAtIndex(
    HCCallHandle call,
    uint32_t headerIndex,
    const char** headerName,
    const char** headerValue) {
    if (!call || !headerName || !headerValue) {
        return RecordHr("HCHttpCallRequestGetHeaderAtIndex", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetHeaderAtIndex", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    if (headerIndex >= obj->request_headers.size()) {
        return RecordHr("HCHttpCallRequestGetHeaderAtIndex", OS_SHIM_E_INVALIDARG);
    }
    *headerName = obj->request_headers[headerIndex].first.c_str();
    *headerValue = obj->request_headers[headerIndex].second.c_str();
    return RecordHr("HCHttpCallRequestGetHeaderAtIndex", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetHeader(
    HCCallHandle call,
    const char* headerName,
    const char** headerValue) {
    if (!call || !IsValidHttpHeaderName(headerName) || !headerValue) {
        return RecordHr("HCHttpCallRequestGetHeader", OS_SHIM_E_INVALIDARG);
    }
    *headerValue = nullptr;
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetHeader", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (const auto& kv : obj->request_headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            *headerValue = kv.second.c_str();
            break;
        }
    }
    return RecordHr("HCHttpCallRequestGetHeader", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallRequestGetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t** requestBodyBytes,
    uint32_t* requestBodySize) {
    if (!call || !requestBodyBytes || !requestBodySize) {
        return RecordHr("HCHttpCallRequestGetRequestBodyBytes", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallRequestGetRequestBodyBytes", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *requestBodyBytes = obj->request_body.empty() ? nullptr : obj->request_body.data();
    *requestBodySize = static_cast<uint32_t>(obj->request_body.size());
    return RecordHr("HCHttpCallRequestGetRequestBodyBytes", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call,
    int32_t* networkErrorCode,
    uint32_t* platformNetworkErrorCode) {
    if (!call || !networkErrorCode || !platformNetworkErrorCode) {
        return RecordHr("HCHttpCallResponseGetNetworkErrorCode", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallResponseGetNetworkErrorCode", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *networkErrorCode = obj->network_error_code;
    *platformNetworkErrorCode = obj->platform_network_error_code;
    return RecordHr("HCHttpCallResponseGetNetworkErrorCode", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetPlatformNetworkErrorMessage(
    HCCallHandle call,
    const char** platformNetworkErrorMessage) {
    if (!call || !platformNetworkErrorMessage) {
        return RecordHr("HCHttpCallResponseGetPlatformNetworkErrorMessage", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) {
        return RecordHr("HCHttpCallResponseGetPlatformNetworkErrorMessage", OS_SHIM_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->mutex);
    *platformNetworkErrorMessage = obj->platform_network_error_message.c_str();
    return RecordHr("HCHttpCallResponseGetPlatformNetworkErrorMessage", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetNumHeaders(
    HCCallHandle call,
    uint32_t* numHeaders) {
    if (!call || !numHeaders) return RecordHr("HCHttpCallResponseGetNumHeaders", OS_SHIM_E_INVALIDARG);
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallResponseGetNumHeaders", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    *numHeaders = static_cast<uint32_t>(obj->response_headers.size());
    return RecordHr("HCHttpCallResponseGetNumHeaders", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCHttpCallResponseGetHeaderAtIndex(
    HCCallHandle call,
    uint32_t headerIndex,
    const char** headerName,
    const char** headerValue) {
    if (!call || !headerName || !headerValue) {
        return RecordHr("HCHttpCallResponseGetHeaderAtIndex", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupCall(call);
    if (!obj) return RecordHr("HCHttpCallResponseGetHeaderAtIndex", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    if (headerIndex >= obj->response_headers.size()) {
        return RecordHr("HCHttpCallResponseGetHeaderAtIndex", OS_SHIM_E_INVALIDARG);
    }
    *headerName = obj->response_headers[headerIndex].first.c_str();
    *headerValue = obj->response_headers[headerIndex].second.c_str();
    return RecordHr("HCHttpCallResponseGetHeaderAtIndex", OS_SHIM_S_OK);
}

// ===========================================================================
// WebSocket C ABI Exports (Offline-Safe / XAsync Backed)
// ===========================================================================
OS_HC_API int32_t OS_SHIM_CALL HCWebSocketCreate(
    HCWebsocketHandle* websocket,
    HCWebSocketMessageFunction messageFunc,
    HCWebSocketBinaryMessageFunction binaryMessageFunc,
    HCWebSocketCloseEventFunction closeFunc,
    void* functionContext) {
    if (!websocket) {
        return RecordHr("HCWebSocketCreate", OS_SHIM_E_INVALIDARG);
    }
    *websocket = nullptr;
    auto obj = std::make_shared<HCWebSocketObject>();
    obj->message_fn = messageFunc;
    obj->binary_message_fn = binaryMessageFunc;
    obj->close_fn = closeFunc;
    obj->fn_context = functionContext;
    HCWebsocketHandle raw = obj.get();
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.live_websockets[raw] = std::move(obj);
    }
    *websocket = raw;
    return RecordHr("HCWebSocketCreate", OS_SHIM_S_OK);
}

OS_HC_API HCWebsocketHandle OS_SHIM_CALL HCWebSocketDuplicateHandle(HCWebsocketHandle websocket) {
    auto obj = LookupWebSocket(websocket);
    if (!obj) {
        RecordHr("HCWebSocketDuplicateHandle", OS_SHIM_E_HANDLE);
        return nullptr;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    RecordOk("HCWebSocketDuplicateHandle");
    return websocket;
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketCloseHandle(HCWebsocketHandle websocket) {
    if (!websocket) {
        return RecordHr("HCWebSocketCloseHandle", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.live_websockets.find(websocket);
    if (it == st.live_websockets.end()) {
        return RecordHr("HCWebSocketCloseHandle", OS_SHIM_E_HANDLE);
    }
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        st.live_websockets.erase(it);
    }
    return RecordHr("HCWebSocketCloseHandle", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSetProxyUri(
    HCWebsocketHandle websocket,
    const char* proxyUri) {
    if (!websocket || !proxyUri) {
        return RecordHr("HCWebSocketSetProxyUri", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketSetProxyUri", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->proxy_uri = proxyUri;
    return RecordHr("HCWebSocketSetProxyUri", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSetHeader(
    HCWebsocketHandle websocket,
    const char* headerName,
    const char* headerValue) {
    if (!websocket || !IsValidHttpHeaderName(headerName) || !IsValidHttpHeaderValue(headerValue)) {
        return RecordHr("HCWebSocketSetHeader", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketSetHeader", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    for (auto& kv : obj->headers) {
        if (EqualsAsciiCaseInsensitive(kv.first, headerName)) {
            kv.second = headerValue;
            return RecordHr("HCWebSocketSetHeader", OS_SHIM_S_OK);
        }
    }
    obj->headers.emplace_back(headerName, headerValue);
    return RecordHr("HCWebSocketSetHeader", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSetFunctions(
    HCWebSocketConnectFunction websocketConnectFunc,
    HCWebSocketSendMessageFunction websocketSendMessageFunc,
    HCWebSocketSendBinaryMessageFunction websocketSendBinaryMessageFunc,
    HCWebSocketDisconnectFunction websocketDisconnectFunc,
    void* context) {
    auto& st = GetHc();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.ws_connect_fn = websocketConnectFunc;
    st.ws_send_text_fn = websocketSendMessageFunc;
    st.ws_send_bin_fn = websocketSendBinaryMessageFunc;
    st.ws_disconnect_fn = websocketDisconnectFunc;
    st.ws_fn_context = context;
    return RecordHr("HCWebSocketSetFunctions", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketGetEventFunctions(
    HCWebsocketHandle websocket,
    HCWebSocketMessageFunction* messageFunc,
    HCWebSocketBinaryMessageFunction* binaryMessageFunc,
    HCWebSocketCloseEventFunction* closeFunc,
    void** functionContext) {
    if (!websocket) {
        return RecordHr("HCWebSocketGetEventFunctions", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketGetEventFunctions", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    if (messageFunc) *messageFunc = obj->message_fn;
    if (binaryMessageFunc) *binaryMessageFunc = obj->binary_message_fn;
    if (closeFunc) *closeFunc = obj->close_fn;
    if (functionContext) *functionContext = obj->fn_context;
    return RecordHr("HCWebSocketGetEventFunctions", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSetMaxReceiveBufferSize(
    HCWebsocketHandle websocket,
    size_t bufferSizeInBytes) {
    if (!websocket || bufferSizeInBytes == 0u) {
        return RecordHr("HCWebSocketSetMaxReceiveBufferSize", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketSetMaxReceiveBufferSize", OS_SHIM_E_HANDLE);
    std::lock_guard<std::mutex> lk(obj->mutex);
    obj->max_receive_buffer_size = bufferSizeInBytes;
    return RecordHr("HCWebSocketSetMaxReceiveBufferSize", OS_SHIM_S_OK);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketConnectAsync(
    const char* uri,
    const char* subProtocol,
    HCWebsocketHandle websocket,
    XAsyncBlock* asyncBlock) {
    if (!IsValidWebSocketUrl(uri) || !websocket || !asyncBlock) {
        return RecordHr("HCWebSocketConnectAsync", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketConnectAsync", OS_SHIM_E_HANDLE);

    HCWebSocketConnectFunction custom_fn = nullptr;
    void* custom_ctx = nullptr;
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        custom_fn = st.ws_connect_fn;
        custom_ctx = st.ws_fn_context;
    }
    if (custom_fn) {
        return RecordHr("HCWebSocketConnectAsync",
                        custom_fn(uri, subProtocol ? subProtocol : "", websocket, asyncBlock, custom_ctx));
    }

    auto* ctx = new (std::nothrow) WsAsyncCtx();
    if (!ctx) return RecordHr("HCWebSocketConnectAsync", OS_SHIM_E_OUTOFMEMORY);
    ctx->ws = std::move(obj);
    ctx->uri = uri;
    ctx->sub_protocol = subProtocol ? subProtocol : "";
    ctx->op_kind = 0u;

    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kIdWsConnect, kIdWsConnect, &HcWebSocketAsyncProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("HCWebSocketConnectAsync", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0u);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("HCWebSocketConnectAsync", hr);
}

OS_HC_API int32_t OS_SHIM_CALL HCGetWebSocketConnectResult(
    XAsyncBlock* asyncBlock,
    WebSocketCompletionResult* result) {
    if (!asyncBlock || !result) {
        return RecordHr("HCGetWebSocketConnectResult", OS_SHIM_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetResult(
        asyncBlock, kIdWsConnect, sizeof(WebSocketCompletionResult), result, nullptr);
    return RecordHr("HCGetWebSocketConnectResult", hr);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSendMessageAsync(
    HCWebsocketHandle websocket,
    const char* message,
    XAsyncBlock* asyncBlock) {
    if (!websocket || !message || !asyncBlock) {
        return RecordHr("HCWebSocketSendMessageAsync", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketSendMessageAsync", OS_SHIM_E_HANDLE);

    HCWebSocketSendMessageFunction custom_fn = nullptr;
    void* custom_ctx = nullptr;
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        custom_fn = st.ws_send_text_fn;
        custom_ctx = st.ws_fn_context;
    }
    if (custom_fn) {
        return RecordHr("HCWebSocketSendMessageAsync",
                        custom_fn(websocket, message, asyncBlock, custom_ctx));
    }

    auto* ctx = new (std::nothrow) WsAsyncCtx();
    if (!ctx) return RecordHr("HCWebSocketSendMessageAsync", OS_SHIM_E_OUTOFMEMORY);
    ctx->ws = std::move(obj);
    ctx->text_payload = message;
    ctx->op_kind = 1u;

    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kIdWsSend, kIdWsSend, &HcWebSocketAsyncProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("HCWebSocketSendMessageAsync", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0u);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("HCWebSocketSendMessageAsync", hr);
}

OS_HC_API int32_t OS_SHIM_CALL HCGetWebSocketSendMessageResult(
    XAsyncBlock* asyncBlock,
    WebSocketCompletionResult* result) {
    if (!asyncBlock || !result) {
        return RecordHr("HCGetWebSocketSendMessageResult", OS_SHIM_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetResult(
        asyncBlock, kIdWsSend, sizeof(WebSocketCompletionResult), result, nullptr);
    return RecordHr("HCGetWebSocketSendMessageResult", hr);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketSendBinaryMessageAsync(
    HCWebsocketHandle websocket,
    const uint8_t* payloadBytes,
    uint32_t payloadSize,
    XAsyncBlock* asyncBlock) {
    if (!websocket || !asyncBlock || (payloadSize > 0u && !payloadBytes)) {
        return RecordHr("HCWebSocketSendBinaryMessageAsync", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketSendBinaryMessageAsync", OS_SHIM_E_HANDLE);

    HCWebSocketSendBinaryMessageFunction custom_fn = nullptr;
    void* custom_ctx = nullptr;
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        custom_fn = st.ws_send_bin_fn;
        custom_ctx = st.ws_fn_context;
    }
    if (custom_fn) {
        return RecordHr("HCWebSocketSendBinaryMessageAsync",
                        custom_fn(websocket, payloadBytes, payloadSize, asyncBlock, custom_ctx));
    }

    auto* ctx = new (std::nothrow) WsAsyncCtx();
    if (!ctx) return RecordHr("HCWebSocketSendBinaryMessageAsync", OS_SHIM_E_OUTOFMEMORY);
    ctx->ws = std::move(obj);
    if (payloadSize > 0u) {
        ctx->bin_payload.assign(payloadBytes, payloadBytes + payloadSize);
    }
    ctx->op_kind = 2u;

    XGameResult hr = XAsyncBegin(
        asyncBlock, ctx, kIdWsSendBin, kIdWsSendBin, &HcWebSocketAsyncProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("HCWebSocketSendBinaryMessageAsync", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0u);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("HCWebSocketSendBinaryMessageAsync", hr);
}

OS_HC_API int32_t OS_SHIM_CALL HCWebSocketDisconnect(HCWebsocketHandle websocket) {
    if (!websocket) {
        return RecordHr("HCWebSocketDisconnect", OS_SHIM_E_INVALIDARG);
    }
    auto obj = LookupWebSocket(websocket);
    if (!obj) return RecordHr("HCWebSocketDisconnect", OS_SHIM_E_HANDLE);

    HCWebSocketDisconnectFunction custom_fn = nullptr;
    void* custom_ctx = nullptr;
    {
        auto& st = GetHc();
        std::lock_guard<std::mutex> lk(st.mutex);
        custom_fn = st.ws_disconnect_fn;
        custom_ctx = st.ws_fn_context;
    }
    if (custom_fn) {
        return RecordHr("HCWebSocketDisconnect",
                        custom_fn(websocket, HCWebSocketCloseStatus_Normal, custom_ctx));
    }

    HCWebSocketCloseEventFunction close_fn = nullptr;
    void* fn_ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(obj->mutex);
        obj->connected = false;
        close_fn = obj->close_fn;
        fn_ctx = obj->fn_context;
    }
    if (close_fn) {
        close_fn(websocket, HCWebSocketCloseStatus_Normal, fn_ctx);
    }
    return RecordHr("HCWebSocketDisconnect", OS_SHIM_S_OK);
}

}  // extern "C"
