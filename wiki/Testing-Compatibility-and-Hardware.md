# Testing, Compatibility & Hardware

OpenScarlett tracks every capability by verification tier: **Live-Verified**, **Unit-Tested**, and **In Progress**.

---

## 1. Honest Compatibility Status (Early Alpha)

| Category | Current Status | Details |
| :--- | :---: | :--- |
| **Playable Homebrew / Open-Source Games** | 🟢 **Playable (60 FPS)** | **Freedoom: Phase 1** (`ScarlettDoom` + `freedoom1.wad`, E1M1 131 frames / 129 unique hashes, `GameInput` + `XAudio2_9` + `S:\` `XGameSave`) & **`ScarlettArena`** |
| **Untouched Microsoft `Xbox-GDK-Samples`** | 🟢 **Live Pass** | **4 untouched MIT samples** compiled for `_GAMING_XBOX_SCARLETT` (`SimpleTriangle`, `SimpleLighting`, `SimpleTexture`, `SimpleInstancing`) + **`SimpleDirectStorageCombo`** (`0x6824D2954256492C` byte-identical, 769 live calls) |
| **Custom Dual-API 3D GDK Workloads** | 🟢 **Live Pass** | Shadowed/skinned BC1+BC3 **`gdk_3d_scene`**, **`SimplePBR`** (4× MSAA), **`MeshletCull`** (`as_6_5`+`ms_6_5`+`ps_6_5`), **`DXRTriangle`**, **`HDRCubeMap`** (666.7 nits ST.2084), **`VulkanMeshShowcase`** (`VK_EXT_mesh_shader`, `0.0000` MAE vs D3D12) |
| **Commercial Shipping GDK Thunks & Launchers** | 🟢 **Live Pass** | Shipping `XGamingRuntimeThunks.dll` (*Alexandria IV DEMO*: 292/292 parity, 24 COM `QueryApiImpl` interfaces, `0 E_NOINTERFACE`); multi-process child-hook propagation (*Pizza House Simulator*, 3-gen *House Flipper 2*) |
| **Commercial Retail Xbox Series X\|S Games** | 🟣 **Not Playable Yet** | **0 retail AAA games playable so far.** Active bring-up target — join [Discord](https://discord.gg/sShMY2fjR) to share First-Boot Probe telemetry on unpacked GDK builds! |

---

## 2. Verified Community Hardware Matrix

| Hardware Tier | Tested GPUs | Windows Build | Probed Capabilities | Result |
| :--- | :--- | :--- | :--- | :---: |
| **NVIDIA Turing** | GTX 1650 Ti (4 GB), RTX 2060 SUPER (8 GB) | Win10 22H2, Win11 26H2 | `SM 6.5–6.6` · `Wave32` · `Mesh` · `DXR=0/12` · `Vulkan 1.4` | 🟢 **Verified** (`62/62` on GTX 1650 Ti) |
| **NVIDIA Ampere** | RTX 3080 Ti (12 GB) | Win11 25H2 | `SM 6.6` · `Wave32` · `Mesh` · `DXR=12` · `Vulkan 1.4.357` | 🟢 **Verified** |
| **NVIDIA Blackwell** | RTX 5070 (12 GB) | Win11 26H2 | `SM 6.6` · `Wave32` · `Mesh` · `DXR=12` · `Vulkan 1.4.357` | 🟢 **Verified** |
| **AMD RDNA 3 & 4** | RX 7600 (8 GB), RX 7900 XTX (24 GB), RX 9070 XT (16 GB) | Win11 26H2 | `SM 6.6` · `Wave=32..64` · `Mesh` · `DXR=11` · `Vulkan 1.4` | 🟢 **Verified** |
| **Software Fallback** | D3D12 WARP | Win10 / Win11 / CI | `SM 6.2` · Software rasterizer & SW BVH | 🟢 **Verified** (`62/62`) |
| **NVIDIA Ada** | RTX 40 Series | — | Ada SM 6.7 & DXR 1.1 | 🟡 **Tester Needed** |
| **AMD RDNA 2 & Handhelds** | RX 6000 / ROG Ally / Legion Go / Steam Deck Win | — | Native RDNA2 (Series X\|S sibling) & APU UMA | 🟡 **Tester Needed** |
| **Intel Arc & iGPUs** | Arc A-Series / B-Series / Xe | — | `Wave8`/`Wave16`/`Wave32` & UMA heaps | 🟡 **Tester Needed** |

Want to help test a needed GPU tier? See [Getting Started — 60s Hardware Test](Getting-Started.md#3-running-the-60-second-hardware-test-cli-alternative).

---

## 3. Complete 62-Test Automated CTest Suite

All **62 CTest targets** run on both MSVC Release/Debug CI and local GPU hardware:

### Core, Loader, VFS & OS Shims (15 tests)
1. `test_pe_inspect` — PE32+ x64 headers, imports, exports, TLS & `.pdata`
2. `test_winrt_xs` — `winrt_xs.dll` `RoInitialize` & `HSTRING`
3. `test_cli_probe_gpu` — `openscarlett probe-gpu` end-to-end probe
4. `test_apc_loader` — Suspended-process APC injection (`openscarlett_hook.dll`)
5. `test_loader_vfs_config` — `MicrosoftGame.Config` XML & `G:\`/`T:\`/`S:\` CoW VFS
6. `test_gdk_shims` — Core `xgameruntime` & `GameInput` shims
7. `test_secondary_shims` — Secondary GDK & console diagnostic DLLs
8. `test_gdk_extended_loader` — Kahn topological DLL initialization & extended shims
9. `test_xgr_conformance_parity` — `XGamingRuntime` `XTaskQueue` & API conformance
10. `test_loader_child_hook_parity` — `CreateProcessW/A` child-process APC propagation
11. `test_real_gdk_thunks_live` — Real shipping `XGamingRuntimeThunks.dll` (292/292, 24 COM)
12. `test_s7_pe_gdk_runtime` — PE hardening, `XMemVirtualQuery`, Minecraft-GDK & Raw Input
13. `test_os_winrt_offline_auth` — 7-core affinity remap, WinRT lifecycle & offline XSTS/XStore
14. `test_probe_veh_capture` — `--probe-unknown-imports` trampolines, VEH 50-call ring & capture
15. `test_xvd_loader_lsa` — 12 KiB XVD inspector, mixed-case `PeForwarder` & `ConsoleLsaPolicy`
16. `test_s12_os_loader_gameinput` — Live `PeForwarder`/LSA/probe, TLS slot 0, `NtCreateFile` VFS, GameInput & 128-voice `XAudio2_9`
17. `test_launcher_updater` — Native Win32/Direct2D `OpenScarlettLauncher.exe` & BCrypt SHA-256 updater

### Graphics (`d3d12_xs`, Vulkan 1.4, HDR10, `xg_xs`) (13 tests)
18. `test_vulkan_live` — Zero-SDK Vulkan 1.4 compute pipeline
19. `test_d3d12_xs_live` — `d3d12_xs` COM proxies & live GPU FSR 1.0
20. `test_d3d12_xs_autolift` — Automatic `XS2\0` RDNA2 shader lifting & disk cache hit
21. `test_presentx_windowing` — Interactive `PresentX` 60/120 Hz pacing & waitable swapchain
22. `test_vulkan_graphics_live` — Vulkan 1.4 rasterization, MSAA & 3D textures
23. `test_hdr10_d3d12` — HDR10 ST.2084 / scRGB & ACES/Reinhard fallback
24. `test_gpu_d3d12x_vk_parity` — D3D12X vs. Vulkan sparse/tiling/format parity
25. `test_d3d12_vk_routing` — Direct `d3d12_xs` → Vulkan 1.4 routing & edge cases
26. `test_d3d12_xs_proxies_multiqueue` — All 13 Xbox COM proxies & 6-thread multi-queue sync
27. `test_xg_gpu_untile_and_stages` — GPU compute untiling (`cs_6_5`+SPIR-V), HS/DS/GS & Wave64-on-32
28. `test_gdkx_abi` — Real `_GAMING_XBOX_SCARLETT` `d3d12_xs` COM vtable ABI parity
29. `test_d3d12_feedback_queries` — Sampler Feedback readback/clear, `QueryAccumulatorX` & GPU-QPC clock
30. `test_s12_d3d12_aaa_wiring` — Sprint 12 AAA GPU island wiring, GXDK extensions & `PresentX` pacing

### RDNA2 Shader Lifter & Ray Tracing (12 tests)
31. `test_rdna2_core` — RDNA2 disassembler & SSA IR foundation
32. `test_rdna2_lifter_live` — Live GPU execution of lifted RDNA2 SPIR-V & DXIL shaders
33. `test_dxr_live` — Hardware DXR Tier 1.1 & SAH BVH4 fallback (`0x86443514BF44B806`)
34. `test_rdna2_corpus_parity` — Clamped shifts, `v_cmpx`, `MTBUF d16`, `PSV0` & MD5 re-signing
35. `test_rdna2_lockstep_isa` — Single-wave lockstep LDS barriers & differential ISA parity
36. `test_rdna2_isa_scalar_vector` — GFX10.3 scalar/vector ISA expansion (`SOP*`, `SMEM`, `VOP*`, `SDWA`/`DPP`)
37. `test_rdna2_isa_mem_img` — GFX10.3 memory/image/LDS/flat/exp ISA expansion
38. `test_rdna2_corpus_bench` — 779-opcode auditor & 3,120-shader 12-workload AAA corpus
39. `test_rdna2_as_task_spirv` — Amplification Shader SPIR-V 1.6 `TaskEXT` (`130/130` `spirv-val`) & `CullDistance`
40. `test_rdna2_gpu_accuracy` — 615/779 real-GPU verified opcodes & 4 GFX10.3 hardware accuracy rules
41. `test_s12_xbox_shader_container` — Xbox `dxc_xs` DXBC/PM4/SGPR parser, EXEC64 CFG hardening & in-proc DXC cache
42. `test_s12_rayquery_live` — Live RDNA2 `image_bvh(64)_intersect_ray` DXR/SPIR-V bridge, 64-bit multi-window BVH4 & SBT remapping

### Memory, Streaming, Codecs & Media (8 tests)
43. `test_codecs` — GDeflate v1, BCPack, XMA2 IMDCT & Bink2
44. `test_kernelx` — `VirtualAlloc2` 64 KiB / 4 MiB aliasing & `GpuTilePool`
45. `test_audio_opus_wasapi` — `MSOpusDecoder` MFT, ADPCM & live/null WASAPI
46. `test_xg_dstorage` — `xg_xs` tiling + `dstorage_xs` queues & cancellation
47. `test_dstorage_audio_diag_parity` — DirectStorage + audio + diagnostic shim parity
48. `test_bcpack_bvh4` — Full BCPack BC1–BC7 & RDNA2 BVH4 decoding
49. `test_unified_mem_bindless` — 64-bit `TitleVAMirror` (`MEM_WRITE_WATCH`) & raw `V#`/`T#`/`S#` bindless bridge
50. `test_media_video_gameinput` — `mfplat_xs` H.264/HEVC decode, 64+ `XAudio2_9` voices, Bink2 & `GameInput` hot-plug
51. `test_s12_streaming_vram` — DirectStorage priority/custom decomp/GPU untile upload, BCPack arrays, page aliasing & LRU `VramResidencyManager`

### Live GDK Samples & Playable Workloads (11 tests)
52. `test_gdk_live` — `SimpleTriangle`, `SimpleMeshShader`, `SimpleCompute` & 10-DLL `SystemCombo`
53. `test_gdk_simple_pbr` — Multi-frame 3D `SimplePBR` (4× MSAA, `D32_FLOAT`)
54. `test_gdk_meshlet_cull` — `MeshletCull` (`as_6_5`+`ms_6_5`+`ps_6_5` frustum + cone culling)
55. `test_gdk_dxr_triangle` — `DXRTriangle` (`lib_6_3` HW RT / SAH BVH)
56. `test_external_gdk_bringup` — Untouched MIT Microsoft `SimpleDirectStorageCombo`
57. `test_gdk_simple_lighting` — 3D Blinn-Phong + `XDisplay` + `X3DAudio` Doppler (D3D12 == Vulkan `0.0000` MAE)
58. `test_gdk_hdr_cubemap` — `HDRCubeMap` (`FP16 TextureCube` + ST.2084 `666.7 nits`)
59. `test_vulkan_mesh_showcase` — Raw RDNA2 `AS`+`MS`+`PS` over `VK_EXT_mesh_shader` (`0.0000` MAE vs D3D12)
60. `test_scarlett_game_live` — 60 FPS live playable **Freedoom: Phase 1** & **ScarlettArena**
61. `test_gdk_3d_scene_dual_api` — Shadowed/skinned BC1+BC3 3D dual-API GDK scene (`D3D12` + `Vulkan 1.4`)
62. `test_gdkx_untouched_samples` — 4 untouched MIT Microsoft `Xbox-GDK-Samples` (`SimpleTriangle`, `SimpleLighting`, `SimpleTexture`, `SimpleInstancing`)
