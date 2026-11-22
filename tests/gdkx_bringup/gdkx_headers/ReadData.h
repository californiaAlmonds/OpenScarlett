#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>
// MIT DX::ReadData helper for untouched Xbox-GDK-Samples.
namespace DX {
inline std::vector<uint8_t> ReadData(const wchar_t* name) {
    std::ifstream f(name, std::ios::binary | std::ios::ate);
    if (!f) {
        throw std::runtime_error("DX::ReadData failed to open file");
    }
    const auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) {
        f.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}
}  // namespace DX
