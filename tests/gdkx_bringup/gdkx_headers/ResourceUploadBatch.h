#pragma once
#include <d3d12_xs.h>

#include <future>

// MIT DirectXTK12 ResourceUploadBatch subset for untouched Xbox-GDK-Samples.

namespace DirectX {

class ResourceUploadBatch {
public:
    explicit ResourceUploadBatch(ID3D12Device* device) noexcept
        : m_device(device), m_inBeginEnd(false) {}

    ResourceUploadBatch(const ResourceUploadBatch&) = delete;
    ResourceUploadBatch& operator=(const ResourceUploadBatch&) = delete;

    void Begin(D3D12_COMMAND_LIST_TYPE = D3D12_COMMAND_LIST_TYPE_DIRECT) noexcept {
        m_inBeginEnd = true;
    }

    std::future<void> End(ID3D12CommandQueue*) {
        m_inBeginEnd = false;
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    ID3D12Device* GetDevice() const noexcept { return m_device; }

private:
    ID3D12Device* m_device;
    bool m_inBeginEnd;
};

}  // namespace DirectX
