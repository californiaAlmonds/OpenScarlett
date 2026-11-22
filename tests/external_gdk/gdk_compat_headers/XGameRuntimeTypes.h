#pragma once

// MIT Microsoft GDK common runtime types & HRESULT definitions.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define XGR_COMPAT_API __declspec(dllimport)
#  define XGR_COMPAT_CALL __stdcall
#else
typedef int32_t HRESULT;
#  define XGR_COMPAT_API
#  define XGR_COMPAT_CALL
#endif

#ifndef E_GAMERUNTIME_NOT_INITIALIZED
#  define E_GAMERUNTIME_NOT_INITIALIZED static_cast<HRESULT>(0x89240100u)
#endif
#ifndef E_GS_UPDATE_TOO_BIG
#  define E_GS_UPDATE_TOO_BIG static_cast<HRESULT>(0x80830002u)
#endif
#ifndef E_GS_PROVIDED_BUFFER_TOO_SMALL
#  define E_GS_PROVIDED_BUFFER_TOO_SMALL static_cast<HRESULT>(0x80830007u)
#endif
#ifndef E_GS_BLOB_NOT_FOUND
#  define E_GS_BLOB_NOT_FOUND static_cast<HRESULT>(0x80830008u)
#endif
#ifndef E_GS_CONTAINER_NOT_IN_SYNC
#  define E_GS_CONTAINER_NOT_IN_SYNC static_cast<HRESULT>(0x8083000Bu)
#endif

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201)
#endif
struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
static_assert(sizeof(XVersion) == 8, "XVersion must be 8 bytes");
