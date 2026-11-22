# OpenScarlett GDKX Untouched Sample API Audit (`SimpleTexture` + `SimpleInstancing`)

This directory stages **2 additional 100% untouched MIT-licensed Microsoft `Xbox-GDK-Samples`** (from `https://github.com/microsoft/Xbox-GDK-Samples`, `Samples/IntroGraphics/`) alongside the upstream `LICENSE` file:

1. `SimpleTexture/` — Textured quad rendered via WIC image decode, upload heap + `UpdateSubresources`, shader-visible SRV descriptor table, static sampler, and indexed draw.
2. `SimpleInstancing/` — 5,000–20,000 hardware-instanced 3D cubes using 3 vertex streams (per-vertex + 2 per-instance streams), quaternion rotation + multi-point lighting shaders, per-frame ring-buffered instance uploads with `ID3D12Fence` pacing, and DirectXTK12 `SpriteBatch`/`SpriteFont` HUD overlay.

Together with `SimpleTriangle` and `SimpleLighting`, these expand OpenScarlett's `_GAMING_XBOX_SCARLETT` bring-up corpus to **4 untouched official Microsoft GDKX samples**.

---

## 1. Shared Baseline Already Required by `SimpleTriangle` + `SimpleLighting`

Both `SimpleTexture` and `SimpleInstancing` share the exact same Scarlett GDKX core surface already audited for `SimpleTriangle` and `SimpleLighting`:

| Category | Headers / APIs |
|---|---|
| **GDK Core & Version** | `<gxdk.h>` (`_GXDK_VER >= 0x55F00C58`, March 2022 GDK or later), `<XGame.h>`, `<XSystem.h>` (`XSystemGetDeviceType`), `<XTaskQueue.h>`, `<XGameRuntimeInit.h>` (`XGameRuntimeInitialize`, `XGameRuntimeUninitialize`) |
| **Xbox D3D12.XS** | `<d3d12_xs.h>`, `<d3dx12_xs.h>`: `D3D12XboxCreateDevice`, `D3D12XBOX_CREATE_DEVICE_PARAMETERS` (`Version`, `ProcessDebugFlags`, `GraphicsCommandQueueRingSizeBytes`, `DisableGeometryShaderAllocations`, `DisableTessellationShaderAllocations`, `DisableDXR`, `CreateDeviceFlags`), `IID_GRAPHICS_PPV_ARGS`, `CommandListCast`, `D3D12_HEAP_FLAG_ALLOW_DISPLAY`, `PresentX`, `SuspendX`, `ResumeX`, `SetFrameIntervalX`, `ScheduleFrameEventX`, `WaitFrameEventX`, `D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL` |
| **DXGI QI on Device** | `DeviceResources::RegisterFrameEvents()` calls `m_d3dDevice.As(&dxgiDevice)` (`IDXGIDevice1`) -> `GetAdapter()` -> `EnumOutputs(0)` (requires `ID3D12DeviceX` proxy to answer `QueryInterface(IID_IDXGIDevice1)` cleanly!) |
| **PLM Lifecycle** | `<appnotify.h>`: `RegisterAppStateChangeNotification`, `UnregisterAppStateChangeNotification`, `RegisterAppConstrainedChangeNotification`, `UnregisterAppConstrainedChangeNotification`, `PAPPSTATE_REGISTRATION`, `PAPPCONSTRAIN_REGISTRATION` |
| **PIX & Math** | `<pix3.h>` (`PIXBeginEvent`, `PIXEndEvent`, `PIXScopedEvent`, `PIX_COLOR_DEFAULT`), `<DirectXMath.h>`, `<DirectXColors.h>`, `<wrl/client.h>`, `<wrl/event.h>` |
| **Shared ATG/DirectXTK12** | `GamePad.h`, `GraphicsMemory.h`, `RenderTargetState.h`, `SimpleMath.h` (already in `SimpleLighting`), `ATGColors.h`, `ATGTelemetry.h`, `ReadData.h`, `StepTimer.h` |
| **Win32 Runtime** | `assert(GetACP() == CP_UTF8)` in `Main.cpp` (requires UTF-8 manifest or active code page shim in `Debug` builds), `SetThreadAffinityMask`, `CreateEventEx`, `WNDCLASSEXA` / `CreateWindowExA` |

---

## 2. Extra Headers & APIs Required by `SimpleTexture` (Delta Beyond `SimpleTriangle` + `SimpleLighting`)

### New System Headers & COM/WIC APIs
- **`<wincodec.h>` (`windowscodecs.dll`)**:
  - `Main.cpp`: `CoInitializeEx(nullptr, COINITBASE_MULTITHREADED)` and `CoUninitialize()` (already present on host Windows `ole32.dll`).
  - `SimpleTexture.cpp::LoadBGRAImage()`:
    - `CoCreateInstance(CLSID_WICImagingFactory2, ..., IID_PPV_ARGS(&wicFactory))`
    - `IWICImagingFactory::CreateDecoderFromFilename(L"sunset.jpg", ...)`
    - `IWICBitmapDecoder::GetFrame(0, ...)`
    - `IWICBitmapFrameDecode::GetSize`, `GetPixelFormat`, `CopyPixels`
    - `IWICImagingFactory::CreateFormatConverter` -> `IWICFormatConverter::CanConvert`, `Initialize(..., GUID_WICPixelFormat32bppBGRA, ...)`, `CopyPixels`
- **Header-Inclusion Guard Compatibility (`SimpleTexture/pch.h` lines 81–88)**:
  - Before including `<wincodec.h>`, untouched `pch.h` defines:
    ```cpp
    #define DCOMMON_H_INCLUDED
    #define __dxgicommon_h__
    #define __dxgiformat_h__
    #define __dxgitype_h__
    struct DXGI_JPEG_AC_HUFFMAN_TABLE;
    struct DXGI_JPEG_QUANTIZATION_TABLE;
    struct DXGI_JPEG_DC_HUFFMAN_TABLE;
    ```
  - OpenScarlett's compat `<d3d12_xs.h>` / `<d3dx12_xs.h>` must ensure `DXGI_FORMAT`, `DXGI_SAMPLE_DESC`, etc. are already defined before `<wincodec.h>` is reached so these suppression macros do not hide required DXGI types.

### New D3D12 / D3DX12 Inline Helpers & Pipeline Features
- **`<d3dx12_xs.h>` texture upload helpers**:
  - `GetRequiredIntermediateSize(ID3D12Resource*, UINT, UINT)`
  - `UpdateSubresources(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, UINT64, UINT, UINT, const D3D12_SUBRESOURCE_DATA*)` (internally calls `ID3D12Device::GetCopyableFootprints` + `CopyTextureRegion`)
- **SRV Descriptor Heap & Root Table**:
  - `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` with `D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`
  - `ID3D12Device::CreateShaderResourceView` (`DXGI_FORMAT_B8G8R8A8_UNORM_SRGB`, `D3D12_SRV_DIMENSION_TEXTURE2D`, `D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING`)
  - `ID3D12GraphicsCommandList::SetDescriptorHeaps`, `SetGraphicsRootDescriptorTable`
- **Indexed Draw**:
  - `ID3D12GraphicsCommandList::IASetIndexBuffer` (`DXGI_FORMAT_R16_UINT`)
  - `ID3D12GraphicsCommandList::DrawIndexedInstanced(6, 1, 0, 0, 0)`
- **HLSL Root Signature (`VertexShader.hlsl` / `PixelShader.hlsl`)**:
  - `DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL)` + `StaticSampler(s0, visibility=SHADER_VISIBILITY_PIXEL)`
  - Compile both to `VertexShader.cso` and `PixelShader.cso` with `dxc.exe` (`vs_6_0`/`ps_6_0` or `vs_6_5`/`ps_6_5`).
- **Runtime Asset**:
  - `sunset.jpg` in the sample working / `G:\` package directory (any valid JPEG/PNG image decoded via host WIC works cleanly).

---

## 3. Extra Headers & APIs Required by `SimpleInstancing` (Delta Beyond `SimpleTriangle` + `SimpleLighting`)

### New DirectXTK12 & ATG Headers
| Header | Symbols Referenced in `SimpleInstancing.h` / `.cpp` |
|---|---|
| `"DescriptorHeap.h"` | `DirectX::DescriptorHeap(device, count)`, `Heap()`, `GetCpuHandle(index)`, `GetGpuHandle(index)` |
| `"ResourceUploadBatch.h"` | `DirectX::ResourceUploadBatch(device)`, `Begin()`, `End(ID3D12CommandQueue*)` (returns `std::future<void>`) |
| `"SpriteBatch.h"` | `DirectX::SpriteBatch(device, resourceUpload, pd)`, `SpriteBatchPipelineStateDescription(rtState)`, `Begin(cmdList)`, `End()`, `SetViewport(vp)` |
| `"SpriteFont.h"` | `DirectX::SpriteFont(device, resourceUpload, filename, cpuHandle, gpuHandle)`, `DrawString(batch, text, pos, color)`, `GetLineSpacing()` |
| `"ControllerFont.h"` | `DX::DrawControllerString(SpriteBatch*, SpriteFont*, SpriteFont*, const wchar_t*, XMFLOAT2, XMVECTOR)` |

### Additional DirectXTK12 / DirectXMath Methods
- `DirectX::GraphicsMemory::AllocateConstant<T>(const T&)` returning a handle with `.GpuAddress()`
- `DirectX::GamePad::ButtonStateTracker` (`Update(pad)`, `Reset()`, `.a`, `.leftShoulder`, `.rightShoulder`, `ButtonState::PRESSED`)
- `DirectX::SimpleMath::Viewport::ComputeTitleSafeArea(UINT, UINT)`
- `DirectX::PackedVector::XMCOLOR` (constructed from `XMVECTORF32` and 4 floats)
- `<random>` (`std::default_random_engine`, `std::uniform_real_distribution<float>`) and `_aligned_malloc` / `_aligned_free`

### New D3D12 Multi-Stream Hardware Instancing & Fence APIs
- **3-Stream Input Assembly**:
  - `IASetVertexBuffers(0, 3, m_vertexBufferView)` with:
    - Slot 0 (`PER_VERTEX_DATA`): `POSITION` (`R32G32B32_FLOAT`), `NORMAL` (`R32G32B32_FLOAT`, `D3D12_APPEND_ALIGNED_ELEMENT`)
    - Slot 1 (`PER_INSTANCE_DATA`, step rate 1): `I_ROTATION` (`R32G32B32A32_FLOAT`), `I_POSSCALE` (`R32G32B32A32_FLOAT`, `D3D12_APPEND_ALIGNED_ELEMENT`)
    - Slot 2 (`PER_INSTANCE_DATA`, step rate 1): `I_COLOR` (`R8G8B8A8_UNORM`)
  - `DrawIndexedInstanced(36, 5000..20000, 0, 0, 0)`
- **Dual Root CBVs (`SimpleInstancing.hlsli`)**:
  - Root parameter 0: `CBV(b0, space=0, visibility=SHADER_VISIBILITY_VERTEX)` (`SetGraphicsRootConstantBufferView(0, ...)`)
  - Root parameter 1: `CBV(b0, space=0, visibility=SHADER_VISIBILITY_PIXEL)` (`SetGraphicsRootConstantBufferView(1, ...)`)
- **Persistent Mapped Upload Ring + Explicit Fence Pacing**:
  - `m_instanceData->Map(0, nullptr, ...)` kept persistently mapped across frames
  - `ID3D12Device::CreateFence`, `ID3D12Fence::GetCompletedValue`, `SetEventOnCompletion`, `ID3D12CommandQueue::Signal`, `Microsoft::WRL::Wrappers::Event`, `WaitForSingleObjectEx`
- **Runtime Assets**:
  - `VertexShader.cso`, `PixelShader.cso` (compiled from `VertexShader.hlsl` + `PixelShader.hlsl` with `SimpleInstancing.hlsli` & `Shared.h`)
  - `SegoeUI_18.spritefont` / `SegoeUI_36.spritefont` and `XboxOneControllerLegendSmall.spritefont` / `XboxOneControllerLegend.spritefont` (note: when `XSystemGetDeviceType()` returns `XboxScarlettAnaconda`, `DeviceResources` picks `3840x2160` so `size.bottom > 1080` selects the `_36` and non-`Small` font filenames).

---

## 4. Integration Pass Readiness Summary

| Requirement | `SimpleTexture` | `SimpleInstancing` | Current OpenScarlett Status |
|---|---|---|---|
| `d3d12_xs` core + `PresentX` + frame events | Required | Required | Implemented (`d3d12_xs.dll` + 10 COM proxies) |
| `XGameRuntime` + `XSystemGetDeviceType` | Required | Required | Implemented (`xgameruntime.dll`) |
| `pixevt` (`PIXBeginEvent`/`PIXScopedEvent`) | Required | Required | Implemented (`pixevt.dll`) |
| `MicrosoftGameConfig.mgc` -> PFN/LSA policy | Verified in `test_xvd_loader_lsa` | Verified in `test_xvd_loader_lsa` | Implemented (`game_config` + `console_lsa_policy`) |
| `d3dx12_xs.h` (`GetRequiredIntermediateSize`, `UpdateSubresources`) | **Needed for bring-up** | Basic `CD3DX12_*` only | Add inline helpers to GDKX compat header |
| `<appnotify.h>` PLM callbacks | **Needed for bring-up** | **Needed for bring-up** | Add compat header + no-op/event shim |
| `<gxdk.h>` + `<XGame.h>` | **Needed for bring-up** | **Needed for bring-up** | Add thin compat header forwarding to existing `XGameRuntime*` headers |
| Host WIC (`windowscodecs.dll`) | **Needed (`sunset.jpg`)** | Not used | Native Windows DLL (ensure `d3d12_xs.h` compat with `pch.h` DXGI guard macros) |
| DirectXTK12 (`DescriptorHeap`, `ResourceUploadBatch`, `SpriteBatch`, `SpriteFont`, `ControllerFont`) | Not used | **Needed for bring-up** | Link MIT DirectXTK12 or provide compat headers in integration harness |
