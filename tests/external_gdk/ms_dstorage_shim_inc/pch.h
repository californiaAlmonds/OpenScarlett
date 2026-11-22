#pragma once

// MIT minimal pch.h for compiling untouched MIT Microsoft Xbox-GDK-Samples
// SimpleDirectStorageCombo/SampleImplementations/*.cpp headlessly against OpenScarlett dstorage_xs.dll.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "DirectStorageWin32/DirectStorageCrossPlatform.h"

namespace DX {
class com_exception : public std::exception {
public:
    explicit com_exception(HRESULT hr) noexcept : result_(hr) {}
    const char* what() const noexcept override {
        static char s_str[64]{};
        std::snprintf(s_str, sizeof(s_str), "Failure with HRESULT 0x%08X",
                      static_cast<unsigned int>(result_));
        return s_str;
    }

private:
    HRESULT result_;
};

inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        throw com_exception(hr);
    }
}
}  // namespace DX
