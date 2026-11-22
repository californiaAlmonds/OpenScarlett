#pragma once
#include <d3d12_xs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

// MIT DirectXTK12 GraphicsMemory subset for untouched Xbox-GDK-Samples.
// Lazily allocates a 256-byte-aligned GPU upload ring on first Allocate/AllocateConstant call.

namespace DirectX {

class GraphicsResource {
public:
    GraphicsResource() noexcept : m_gpuAddress(0u), m_memory(nullptr), m_size(0u) {}
    GraphicsResource(D3D12_GPU_VIRTUAL_ADDRESS gpuAddr, void* mem, size_t sz) noexcept
        : m_gpuAddress(gpuAddr), m_memory(mem), m_size(sz) {}

    D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const noexcept { return m_gpuAddress; }
    void* Memory() const noexcept { return m_memory; }
    size_t Size() const noexcept { return m_size; }

private:
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuAddress;
    void* m_memory;
    size_t m_size;
};

class SharedGraphicsResource {
public:
    SharedGraphicsResource() noexcept : m_gpuAddress(0u), m_memory(nullptr), m_size(0u) {}
    SharedGraphicsResource(const GraphicsResource& r) noexcept
        : m_gpuAddress(r.GpuAddress()), m_memory(r.Memory()), m_size(r.Size()) {}
    SharedGraphicsResource(GraphicsResource&& r) noexcept
        : m_gpuAddress(r.GpuAddress()), m_memory(r.Memory()), m_size(r.Size()) {}

    SharedGraphicsResource& operator=(const GraphicsResource& r) noexcept {
        m_gpuAddress = r.GpuAddress();
        m_memory = r.Memory();
        m_size = r.Size();
        return *this;
    }
    SharedGraphicsResource& operator=(GraphicsResource&& r) noexcept {
        m_gpuAddress = r.GpuAddress();
        m_memory = r.Memory();
        m_size = r.Size();
        return *this;
    }

    D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const noexcept { return m_gpuAddress; }
    void* Memory() const noexcept { return m_memory; }
    size_t Size() const noexcept { return m_size; }

private:
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuAddress;
    void* m_memory;
    size_t m_size;
};

class GraphicsMemory {
public:
    enum MemoryTag : uint32_t {
        TAG_GRAPHICS = 0u,
        TAG_COMPUTE = 1u,
    };

    explicit GraphicsMemory(ID3D12Device* device) noexcept
        : m_device(device),
          m_uploadBuffer(nullptr),
          m_mapped(nullptr),
          m_gpuBase(0u),
          m_offset(0u) {
        if (m_device) {
            m_device->AddRef();
        }
        s_instance = this;
    }

    ~GraphicsMemory() noexcept {
        if (s_instance == this) {
            s_instance = nullptr;
        }
        if (m_uploadBuffer) {
            if (m_mapped) {
                m_uploadBuffer->Unmap(0u, nullptr);
                m_mapped = nullptr;
            }
            m_uploadBuffer->Release();
            m_uploadBuffer = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
    }

    GraphicsMemory(const GraphicsMemory&) = delete;
    GraphicsMemory& operator=(const GraphicsMemory&) = delete;

    static GraphicsMemory& Get(ID3D12Device* = nullptr) {
        if (!s_instance) {
            throw std::logic_error("GraphicsMemory singleton not initialized");
        }
        return *s_instance;
    }

    GraphicsResource Allocate(size_t size, size_t alignment = 16u, uint32_t = TAG_GRAPHICS) {
        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureRing();
        // Desktop D3D12 root CBVs require 256-byte alignment (strictly satisfies <=256 alignment).
        const size_t effAlign = std::max<size_t>(alignment, 256u);
        const size_t mask = effAlign - 1u;
        size_t alignedOffset = (m_offset + mask) & ~mask;
        const size_t alignedSize = (size + 255u) & ~size_t(255u);
        if (alignedSize > kRingCapacityBytes) {
            throw std::bad_alloc();
        }
        if (alignedOffset + alignedSize > kRingCapacityBytes) {
            alignedOffset = 0u;
        }
        uint8_t* dst = m_mapped + alignedOffset;
        const D3D12_GPU_VIRTUAL_ADDRESS gpuAddr =
            m_gpuBase + static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(alignedOffset);
        m_offset = alignedOffset + alignedSize;
        return GraphicsResource(gpuAddr, dst, size);
    }

    template <typename T>
    GraphicsResource AllocateConstant(const T& data) {
        GraphicsResource res = Allocate(sizeof(T), 256u, TAG_GRAPHICS);
        std::memcpy(res.Memory(), &data, sizeof(T));
        return res;
    }

    void Commit(ID3D12CommandQueue*) noexcept {}

private:
    static constexpr size_t kRingCapacityBytes = 2u * 1024u * 1024u;
    inline static GraphicsMemory* s_instance = nullptr;

    void EnsureRing() {
        if (m_uploadBuffer) return;
        if (!m_device) {
            throw std::runtime_error("GraphicsMemory has null device");
        }
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1u;
        heapProps.VisibleNodeMask = 1u;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0u;
        desc.Width = kRingCapacityBytes;
        desc.Height = 1u;
        desc.DepthOrArraySize = 1u;
        desc.MipLevels = 1u;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1u;
        desc.SampleDesc.Quality = 0u;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        const HRESULT hr = m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(&m_uploadBuffer));
        if (FAILED(hr) || !m_uploadBuffer) {
            throw std::runtime_error("GraphicsMemory failed to allocate upload ring");
        }
        D3D12_RANGE readRange = {0u, 0u};
        const HRESULT mapHr = m_uploadBuffer->Map(0u, &readRange, reinterpret_cast<void**>(&m_mapped));
        if (FAILED(mapHr) || !m_mapped) {
            m_uploadBuffer->Release();
            m_uploadBuffer = nullptr;
            throw std::runtime_error("GraphicsMemory failed to map upload ring");
        }
        m_gpuBase = m_uploadBuffer->GetGPUVirtualAddress();
        m_offset = 0u;
    }

    std::mutex m_mutex;
    ID3D12Device* m_device;
    ID3D12Resource* m_uploadBuffer;
    uint8_t* m_mapped;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase;
    size_t m_offset;
};

}  // namespace DirectX
