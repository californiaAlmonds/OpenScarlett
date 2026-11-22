#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

// Deterministic 64-bit FNV-1a hash used for frame verification and surface digests.
inline uint64_t ComputeFnv1a64(const void* data, size_t size_bytes) noexcept {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffsetBasis;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size_bytes; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kPrime;
    }
    return hash;
}

struct FrameBufferRgba8 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, row-major, 4 bytes per pixel

    uint64_t ComputeFnv1aHash() const noexcept;
    Status WritePpm(const std::string& file_path) const;
    Status WritePng(const std::string& file_path) const;
};

}  // namespace openscarlett
