#pragma once

// MIT Microsoft XCurl (<XCurl/curl.h>) public C ABI header backed by XCurl.dll.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define XCURL_COMPAT_API __declspec(dllimport)
#  define XCURL_COMPAT_CDECL __cdecl
#else
#  define XCURL_COMPAT_API
#  define XCURL_COMPAT_CDECL
#endif

typedef void CURL;
typedef void CURLM;

struct curl_slist {
    char* data;
    curl_slist* next;
};

enum CURLcode : int32_t {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_URL_MALFORMAT = 3,
    CURLE_OUT_OF_MEMORY = 7,
    CURLE_WRITE_ERROR = 23,
    CURLE_BAD_FUNCTION_ARGUMENT = 43,
    CURLE_UNKNOWN_OPTION = 48,
};

enum CURLMcode : int32_t {
    CURLM_OK = 0,
    CURLM_BAD_HANDLE = 1,
    CURLM_BAD_EASY_HANDLE = 2,
    CURLM_OUT_OF_MEMORY = 3,
    CURLM_ADDED_ALREADY = 7,
};

enum CURLoption : int32_t {
    CURLOPT_TIMEOUT = 13,
    CURLOPT_VERBOSE = 41,
    CURLOPT_NOBODY = 44,
    CURLOPT_POST = 47,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_POSTFIELDSIZE = 60,
    CURLOPT_SSL_VERIFYPEER = 64,
    CURLOPT_HTTPGET = 80,
    CURLOPT_SSL_VERIFYHOST = 81,
    CURLOPT_NOSIGNAL = 99,
    CURLOPT_TIMEOUT_MS = 155,
    CURLOPT_CONNECTTIMEOUT_MS = 156,
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_HEADERDATA = 10029,
    CURLOPT_CUSTOMREQUEST = 10036,
    CURLOPT_PRIVATE = 10103,
    CURLOPT_COPYPOSTFIELDS = 10165,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_HEADERFUNCTION = 20079,
};

enum CURLINFO : int32_t {
    CURLINFO_EFFECTIVE_URL = 0x100001,
    CURLINFO_CONTENT_TYPE = 0x100012,
    CURLINFO_RESPONSE_CODE = 0x200002,
    CURLINFO_HEADER_SIZE = 0x20000B,
    CURLINFO_REQUEST_SIZE = 0x20000C,
    CURLINFO_TOTAL_TIME = 0x300003,
    CURLINFO_SIZE_DOWNLOAD = 0x300008,
    CURLINFO_PRIVATE = 0x400015,
};

enum CURLMSG : int32_t {
    CURLMSG_NONE = 0,
    CURLMSG_DONE = 1,
};

struct CURLMsg {
    CURLMSG msg;
    CURL* easy_handle;
    union {
        void* whatever;
        CURLcode result;
    } data;
};

constexpr long CURL_GLOBAL_DEFAULT = 3L;

#ifdef __cplusplus
extern "C" {
#endif

XCURL_COMPAT_API CURLcode XCURL_COMPAT_CDECL curl_global_init(long flags);
XCURL_COMPAT_API void XCURL_COMPAT_CDECL curl_global_cleanup(void);
XCURL_COMPAT_API const char* XCURL_COMPAT_CDECL curl_version(void);

XCURL_COMPAT_API CURL* XCURL_COMPAT_CDECL curl_easy_init(void);
XCURL_COMPAT_API void XCURL_COMPAT_CDECL curl_easy_cleanup(CURL* curl);
XCURL_COMPAT_API void XCURL_COMPAT_CDECL curl_easy_reset(CURL* curl);
XCURL_COMPAT_API CURLcode XCURL_COMPAT_CDECL curl_easy_setopt(CURL* curl, CURLoption option, ...);
XCURL_COMPAT_API CURLcode XCURL_COMPAT_CDECL curl_easy_perform(CURL* curl);
XCURL_COMPAT_API CURLcode XCURL_COMPAT_CDECL curl_easy_getinfo(CURL* curl, CURLINFO info, ...);
XCURL_COMPAT_API const char* XCURL_COMPAT_CDECL curl_easy_strerror(CURLcode code);

XCURL_COMPAT_API curl_slist* XCURL_COMPAT_CDECL curl_slist_append(
    curl_slist* list,
    const char* string);
XCURL_COMPAT_API void XCURL_COMPAT_CDECL curl_slist_free_all(curl_slist* list);

XCURL_COMPAT_API CURLM* XCURL_COMPAT_CDECL curl_multi_init(void);
XCURL_COMPAT_API CURLMcode XCURL_COMPAT_CDECL curl_multi_cleanup(CURLM* multi_handle);
XCURL_COMPAT_API CURLMcode XCURL_COMPAT_CDECL curl_multi_add_handle(
    CURLM* multi_handle,
    CURL* curl_handle);
XCURL_COMPAT_API CURLMcode XCURL_COMPAT_CDECL curl_multi_remove_handle(
    CURLM* multi_handle,
    CURL* curl_handle);
XCURL_COMPAT_API CURLMcode XCURL_COMPAT_CDECL curl_multi_perform(
    CURLM* multi_handle,
    int32_t* running_handles);
XCURL_COMPAT_API CURLMsg* XCURL_COMPAT_CDECL curl_multi_info_read(
    CURLM* multi_handle,
    int32_t* msgs_in_queue);

#ifdef __cplusplus
}
#endif
