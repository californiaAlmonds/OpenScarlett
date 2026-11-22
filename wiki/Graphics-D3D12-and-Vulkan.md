# Graphics: DirectX 12 (`d3d12_xs`) & Vulkan 1.4

Xbox Series X|S graphics software links against `d3d12_xs.dll` (Xbox-specific Direct3D 12 COM ABI) and `xg_xs.dll` (GFX10 texture addressing/tiling). OpenScarlett implements a full **`d3d12_xs.dll`** proxy layer that translates console D3D12 calls onto either native PC **DirectX 12** (default) or **Vulkan 1.4** (`--backend=vulkan`).

---

## 1. All 13 Xbox `d3d12_xs` COM Object Proxies

Source: `src/gpu/d3d12_xs/` (`device_x.cpp`, `command_queue_x.cpp`, `command_list_x.cpp`, `proxies_x.cpp`), `src/shims/d3d12_xs_shim.cpp`

Console D3D12 (`_GAMING_XBOX_SCARLETT`) differs from desktop PC D3D12 in COM vtable slot layouts, `PresentX` presentation semantics, direct placement memory, and GXDK extensions. OpenScarlett wraps all **13 core Xbox COM objects**:

| # | Xbox COM Proxy | Role & Key Capabilities |
| :-: | :--- | :--- |
| 1 | **`DeviceX`** | Wraps host `ID3D12Device` / `ID3D12Device10`, handles Xbox heap/placement creation, auto-lifts embedded `XS2\0` / `dxc_xs` RDNA2 shaders, wires Sampler Feedback, `TitleVAMirror`, `DescriptorSgprBridge`, and `VramResidencyManager` |
| 2 | **`CommandQueueX`** | Multi-queue (`Direct`, `Async Compute`, `Copy`) submission, `FenceX` cross-queue sync, `PresentX` 60/120 Hz frame pacing, GPU-QPC clock calibration |
| 3 | **`CommandListX`** | Graphics/Compute/Copy/Mesh (`DispatchMesh` slot #79) recording, GXDK extensions, predicated byte copies, Sampler Feedback resolve/clear, optional live Vulkan 1.4 command routing, and `OPENSCARLETT_CAPTURE_DIR` frame capture |
| 4 | **`ResourceX`** | Tracks console resource descriptors, `TitleVAMirror` backing, `xg_xs` tiling/metadata (`HTILE`/`DCC`/`CMASK`), and `GetCopyableFootprints` parity |
| 5 | **`HeapX`** | Translates Xbox physical/direct heaps and `GpuTilePool` / `VramResidencyManager` pages to PC D3D12 heaps |
| 6 | **`DescriptorHeapX`** | Isolates mixed CBV/SRV/UAV/Sampler descriptor heaps and bridges raw RDNA2 `V#`/`T#`/`S#` descriptors |
| 7 | **`PipelineStateX`** | Caches Graphics, Compute, Mesh, and Amplification PSOs with async background pre-compilation (`OPENSCARLETT_ASYNC_PSO`) and disk shader cache |
| 8 | **`RootSignatureX`** | v1.1 → v1.0 root signature round-tripping, root-SGPR `s0..s15` mapping, and `dxc_xs` `RTS0` extraction |
| 9 | **`FenceX`** | Cross-queue (`Direct` / `Async Compute` / `Copy`) and CPU-waitable synchronization verified under 6-thread multi-queue recording |
| 10 | **`CommandAllocatorX`** | Multi-threaded per-worker command list allocator tracking |
| 11 | **`QueryHeapX`** | Timestamp, occlusion, pipeline-statistics queries integrated with `QueryAccumulatorX` |
| 12 | **`CommandSignatureX`** | `ExecuteIndirect` indirect draw/dispatch/mesh argument translation |
| 13 | **`StateObjectX`** | DXR Tier 1.1 raytracing pipeline state object (`ID3D12StateObject`) and SBT shader-table mapping |

---

## 2. GXDK Extensions, Multi-Queue Sync, Sampler Feedback & Queries

Source: `src/gpu/d3d12_xs/sampler_feedback_x.cpp`, `src/gpu/d3d12_xs/query_accumulator_x.cpp`, `src/gpu/d3d12_xs/command_list_x.cpp`

- **6-Thread Multi-Queue Synchronization:** Verified in `test_d3d12_xs_proxies_multiqueue` — 6 concurrent worker threads recording Direct, Async Compute, and Copy command lists synchronized via `FenceX` barriers.
- **Xbox-Specific Command List Extensions:**
  - `WriteBufferImmediate`
  - `SetViewInstanceMask`
  - `OMSetDepthBounds`
  - `SetSamplePositions`
  - Predicated byte-level copies & fast-clear metadata updates
  - `GetCopyableFootprints` parity with Xbox layout rules
- **Console Sampler Feedback (`SamplerFeedbackX`):** Mip-region residency tracking, readback decode (`MinMip` / `MipRegionUsed`), and fast clear (`test_d3d12_feedback_queries`).
- **`QueryAccumulatorX` & GPU-QPC Clock Calibration:** Deterministic occlusion, binary occlusion, pipeline statistics, and calibrated GPU-to-Win32 `QueryPerformanceCounter` timestamp correlation (`test_d3d12_feedback_queries`).
- **Untouched `_GAMING_XBOX_SCARLETT` ABI Parity:** Verified against 4 untouched Microsoft MIT `Xbox-GDK-Samples` (`SimpleTriangle`, `SimpleLighting`, `SimpleTexture`, `SimpleInstancing`) compiled directly against Scarlett headers (`test_gdkx_abi`, `test_gdkx_untouched_samples`).

---

## 3. HDR10 / scRGB `PresentX` & Frame Pacing

Source: `src/gpu/d3d12_xs/command_queue_x.cpp`, `tests/test_presentx_windowing.cpp`, `tests/test_hdr10_d3d12.cpp`

Xbox Series X|S replaces DXGI `IDXGISwapChain::Present` with **`PresentX`** + frame-pipeline tokens (`WaitFrameEventX` / `RegisterFrameEventX` / `SetFrameIntervalX`):
- **60 Hz / 120 Hz Frame Pacing:** Decoupled `PresentX` fence completion vs. DXGI presentation timings with waitable swapchain latency.
- **HDR10 ST.2084 & scRGB Output:** Full `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` / `R10G10B10A2_UNORM` and `FP16` scRGB support (verified on `HDRCubeMap` at `666.7 nits` peak).
- **Automatic SDR Fallback:** When running on a standard SDR monitor (unless `OPENSCARLETT_FORCE_HDR=1`), automatically applies ACES / Reinhard tonemapping to sRGB.

---

## 4. Direct `d3d12_xs` → Vulkan 1.4 Routing

Source: [`include/openscarlett/gpu/vulkan_backend.h`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/include/openscarlett/gpu/vulkan_backend.h), [`src/gpu/vulkan_backend.cpp`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/src/gpu/vulkan_backend.cpp)

Passing `--backend=vulkan` (or `OPENSCARLETT_GPU_BACKEND=vulkan`) routes `d3d12_xs` graphics, compute, copy, mesh shader, and `PresentX` calls directly onto OpenScarlett's **zero-SDK Vulkan 1.4 backend** (dynamically loaded from `vulkan-1.dll`, requiring no Vulkan SDK at build time):

- **Full Rasterization & Compute:** MSAA, depth/stencil (`D32_FLOAT`), 2D/3D/Cube textures, staged BC1–BC7 texture copies, mixed-heap descriptor slot isolation, and push-constant budget guards.
- **`VK_EXT_mesh_shader` (`TaskEXT` + `MeshEXT`):** Live hardware Amplification + Mesh shader execution verified with **`0.0000` MAE** bit-exact parity against D3D12 `MeshTier=10` (`VulkanMeshShowcase` on GTX 1650 Ti Vulkan 1.4.329).
- **Sparse Resources (`vkQueueBindSparse`):** Translates D3D12 reserved resources and `GetResourceTiling` onto Vulkan sparse memory bindings.
- **Wave32-on-64 Compute & Bindless (`VK_EXT_descriptor_indexing`):** Executes lifted RDNA2 SPIR-V 1.6 shaders natively.
- **Adapter Override:** Select a specific Vulkan physical device via `OPENSCARLETT_VK_DEVICE` or force software ICD with `OPENSCARLETT_VK_FORCE_SOFTWARE=1`.

---

## 5. `xg_xs.dll` Texture Tiling, Metadata & GPU Compute Untiler

Source: [`src/gpu/xg_layout.cpp`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/src/gpu/xg_layout.cpp), [`src/gpu/xg_gpu_untile.cpp`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/src/gpu/xg_gpu_untile.cpp), [`src/shims/xg_xs_shim.cpp`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/src/shims/xg_xs_shim.cpp)

Xbox Series X|S stores textures in AMD GFX10/RDNA2 tiled memory layouts with auxiliary compression metadata surfaces. OpenScarlett provides both CPU layout calculators and a **real-time GPU compute untiler/retiler**:

- **21 GFX10 Tile Modes:** 15 2D tile modes (`64KB_S`, `64KB_D`, `64KB_R_X`, `4KB_*`, linear, etc.) + 6 3D thick tile modes, mip-tails, texture arrays, and BC1–BC7 block-compressed formats.
- **Real-Time GPU Compute Untiler/Retiler (`XgGpuUntiler`):** Runs compiled **`cs_6_5` DXIL** and **SPIR-V** compute shaders on the host GPU to untile/retile surfaces at memory-bandwidth speed (`test_xg_gpu_untile_and_stages`), also wired directly into `dstorage_xs` GPU texture uploads.
- **`HTILE` / `DCC` / `CMASK` Metadata Coherency:** Full `XGEncodeDCC` (verified with a 1,000-case fuzz test), fast-clear register propagation, and coherency across upload/untile/resolve transitions (`test_s12_streaming_vram`).

---

## 6. FidelityFX Super Resolution (`amd_fidelityfx_*_xbox.dll`)

Source: `src/shims/ffx_upscaler_shim.cpp`, `src/shims/ffx_loader_shim.cpp`

- Implements live GPU **FSR 1.0** (**EASU** Edge-Adaptive Spatial Upsampling + **RCAS** Robust Contrast-Adaptive Sharpening) compute passes verified to **`0 LSB`** difference between AMD RDNA 3/4, NVIDIA Turing, and D3D12 WARP (`test_d3d12_xs_live`).
- Context loader (`amd_fidelityfx_loader_xbox.dll`) handles versioned FidelityFX context dispatch *(FSR 2/3 temporal upscaling is planned)*.

---

## Related Pages
- [RDNA2 Shader Lifter & Ray Tracing](RDNA2-Shader-Lifter-and-Raytracing.md)
- [Memory, Streaming & Storage](Memory-Streaming-and-Storage.md)
- [Testing, Compatibility & Hardware](Testing-Compatibility-and-Hardware.md)
