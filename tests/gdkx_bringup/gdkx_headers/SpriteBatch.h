#pragma once
#include <d3d12_xs.h>
#include <DirectXMath.h>

#include <stdexcept>

#include "RenderTargetState.h"
#include "ResourceUploadBatch.h"

#ifdef OPENSCARLETT_SPRITEBATCH_BLIT
#include "SpriteBatchBlitVS.inc.h"
#include "SpriteBatchBlitPS.inc.h"
#endif

// MIT DirectXTK12 SpriteBatch subset for untouched Xbox-GDK-Samples.
// Supports both HUD pass-through (SpriteFont) and real GPU textured-quad Draw() blits
// routed 100% through OpenScarlett's d3d12_xs DeviceX / CommandListX.

namespace DirectX {

class SpriteBatchPipelineStateDescription {
public:
    explicit SpriteBatchPipelineStateDescription(
        const RenderTargetState& rtState,
        const D3D12_BLEND_DESC* blend = nullptr) noexcept
        : renderTargetState(rtState),
          hasBlend(blend != nullptr),
          blendDesc(blend ? *blend : D3D12_BLEND_DESC{}) {}

    RenderTargetState renderTargetState;
    bool hasBlend;
    D3D12_BLEND_DESC blendDesc;
};

class SpriteBatch {
public:
    SpriteBatch(
        ID3D12Device* device,
        ResourceUploadBatch&,
        const SpriteBatchPipelineStateDescription& psoDesc) noexcept
        : m_device(device),
          m_rtState(psoDesc.renderTargetState),
          m_hasBlend(psoDesc.hasBlend),
          m_blendDesc(psoDesc.blendDesc),
          m_rootSig(nullptr),
          m_pso(nullptr),
          m_viewport{},
          m_cmdList(nullptr) {
        if (m_device) {
            m_device->AddRef();
        }
    }

    ~SpriteBatch() noexcept {
        if (m_pso) {
            m_pso->Release();
            m_pso = nullptr;
        }
        if (m_rootSig) {
            m_rootSig->Release();
            m_rootSig = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
    }

    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    void SetViewport(const D3D12_VIEWPORT& viewport) noexcept { m_viewport = viewport; }

    void Begin(ID3D12GraphicsCommandList* commandList) noexcept { m_cmdList = commandList; }

    void Draw(
        D3D12_GPU_DESCRIPTOR_HANDLE textureDescriptor,
        XMUINT2 textureSize,
        XMFLOAT2 position) {
        if (!m_cmdList || !m_device) {
            return;
        }
#ifdef OPENSCARLETT_SPRITEBATCH_BLIT
        EnsurePipeline();
        const float vpW = (m_viewport.Width > 0.0f) ? m_viewport.Width : static_cast<float>(textureSize.x);
        const float vpH = (m_viewport.Height > 0.0f) ? m_viewport.Height : static_cast<float>(textureSize.y);
        if (vpW <= 0.0f || vpH <= 0.0f) {
            return;
        }
        const float left = (position.x / vpW) * 2.0f - 1.0f;
        const float right = ((position.x + static_cast<float>(textureSize.x)) / vpW) * 2.0f - 1.0f;
        const float top = 1.0f - (position.y / vpH) * 2.0f;
        const float bottom = 1.0f - ((position.y + static_cast<float>(textureSize.y)) / vpH) * 2.0f;
        const float ndcRect[4] = {left, top, right, bottom};

        m_cmdList->SetGraphicsRootSignature(m_rootSig);
        m_cmdList->SetPipelineState(m_pso);
        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->SetGraphicsRoot32BitConstants(0u, 4u, ndcRect, 0u);
        m_cmdList->SetGraphicsRootDescriptorTable(1u, textureDescriptor);
        m_cmdList->DrawInstanced(6u, 1u, 0u, 0u);
#else
        (void)textureDescriptor;
        (void)textureSize;
        (void)position;
#endif
    }

    void End() noexcept { m_cmdList = nullptr; }

private:
#ifdef OPENSCARLETT_SPRITEBATCH_BLIT
    void EnsurePipeline() {
        if (m_pso && m_rootSig) {
            return;
        }
        HRESULT hr = m_device->CreateRootSignature(
            0u,
            g_SpriteBatchBlitVS,
            sizeof(g_SpriteBatchBlitVS),
            IID_GRAPHICS_PPV_ARGS(&m_rootSig));
        if (FAILED(hr) || !m_rootSig) {
            throw std::runtime_error("SpriteBatch::CreateRootSignature failed");
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSig;
        desc.VS = {g_SpriteBatchBlitVS, sizeof(g_SpriteBatchBlitVS)};
        desc.PS = {g_SpriteBatchBlitPS, sizeof(g_SpriteBatchBlitPS)};

        if (m_hasBlend) {
            desc.BlendState = m_blendDesc;
        } else {
            desc.BlendState.AlphaToCoverageEnable = FALSE;
            desc.BlendState.IndependentBlendEnable = FALSE;
            for (int i = 0; i < 8; ++i) {
                desc.BlendState.RenderTarget[i] = {
                    FALSE, FALSE,
                    D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                    D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                    D3D12_LOGIC_OP_NOOP,
                    D3D12_COLOR_WRITE_ENABLE_ALL};
            }
        }

        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.FrontCounterClockwise = FALSE;
        desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.RasterizerState.MultisampleEnable = FALSE;
        desc.RasterizerState.AntialiasedLineEnable = FALSE;
        desc.RasterizerState.ForcedSampleCount = 0u;
        desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.StencilEnable = FALSE;

        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = (m_rtState.numRenderTargets > 0u) ? m_rtState.numRenderTargets : 1u;
        desc.RTVFormats[0] = (m_rtState.numRenderTargets > 0u)
                                 ? m_rtState.rtvFormats[0]
                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.DSVFormat = m_rtState.dsvFormat;
        desc.SampleDesc = m_rtState.sampleDesc;
        if (desc.SampleDesc.Count == 0u) {
            desc.SampleDesc.Count = 1u;
        }

        hr = m_device->CreateGraphicsPipelineState(&desc, IID_GRAPHICS_PPV_ARGS(&m_pso));
        if (FAILED(hr) || !m_pso) {
            throw std::runtime_error("SpriteBatch::CreateGraphicsPipelineState failed");
        }
    }
#endif

    ID3D12Device* m_device;
    RenderTargetState m_rtState;
    bool m_hasBlend;
    D3D12_BLEND_DESC m_blendDesc;
    ID3D12RootSignature* m_rootSig;
    ID3D12PipelineState* m_pso;
    D3D12_VIEWPORT m_viewport;
    ID3D12GraphicsCommandList* m_cmdList;
};

}  // namespace DirectX
