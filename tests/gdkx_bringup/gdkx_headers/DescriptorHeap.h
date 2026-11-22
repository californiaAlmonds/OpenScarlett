#pragma once
#include <d3d12_xs.h>

#include <cstddef>
#include <stdexcept>

// MIT DirectXTK12 DescriptorHeap subset for untouched Xbox-GDK-Samples.
// Creates a real D3D12 descriptor heap so SetDescriptorHeaps routes through DescriptorHeapX.

namespace DirectX {

class DescriptorHeap {
public:
    DescriptorHeap(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags,
        size_t count)
        : m_heap(nullptr),
          m_increment(0u),
          m_count(count),
          m_cpuStart{},
          m_gpuStart{} {
        if (!device || count == 0u) {
            throw std::invalid_argument("DescriptorHeap requires valid device and non-zero count");
        }
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = type;
        desc.NumDescriptors = static_cast<UINT>(count);
        desc.Flags = flags;
        desc.NodeMask = 0u;
        const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_GRAPHICS_PPV_ARGS(&m_heap));
        if (FAILED(hr) || !m_heap) {
            throw std::runtime_error("DescriptorHeap::CreateDescriptorHeap failed");
        }
        m_increment = device->GetDescriptorHandleIncrementSize(type);
        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        if ((flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
            m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
        }
    }

    DescriptorHeap(ID3D12Device* device, size_t count)
        : DescriptorHeap(
              device,
              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
              count) {}

    ~DescriptorHeap() noexcept {
        if (m_heap) {
            m_heap->Release();
            m_heap = nullptr;
        }
    }

    DescriptorHeap(const DescriptorHeap&) = delete;
    DescriptorHeap& operator=(const DescriptorHeap&) = delete;

    ID3D12DescriptorHeap* Heap() const noexcept { return m_heap; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(size_t index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_cpuStart;
        h.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(m_increment);
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(size_t index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuStart;
        h.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(m_increment);
        return h;
    }

    size_t Count() const noexcept { return m_count; }

private:
    ID3D12DescriptorHeap* m_heap;
    UINT m_increment;
    size_t m_count;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart;
};

}  // namespace DirectX
