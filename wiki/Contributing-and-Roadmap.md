# Contributing & Roadmap

OpenScarlett welcomes both **community hardware testers** (no coding required!) and **C++20 developers**.

---

## 1. How to Contribute

### Option A: 60-Second GPU Hardware Report (No Coding!)
1. Download the latest portable `OpenScarlett-*-win64.zip` from [GitHub Releases](https://github.com/californiaAlmonds/OpenScarlett/releases/latest).
2. Run **`OpenScarlettLauncher.exe`** → **60s Hardware Self-Test** (or `run_hardware_test.ps1`).
3. Click **Open Pre-Filled GitHub Hardware Report** or share your sanitized summary in [Discord](https://discord.gg/sShMY2fjR).
   *(Failing tests on new GPUs like RTX 40, RX 6000, Intel Arc, or handheld APUs are just as valuable as passing ones!)*

### Option B: Test Unpacked GDK Builds with First-Boot Probe
Run unencrypted GDK samples or homebrew packages through:
```powershell
openscarlett.exe run --probe-unknown-imports path\to\package --telemetry probe.json
```
Share only the sanitized telemetry (`missing exports`, `not_impl COM slots`, `shader opcodes`) on Discord or GitHub Issues — diagnostic reports and launcher captures automatically strip Windows usernames and local directory paths.

### Option C: Code Pull Requests
1. Fork & clone the repository and build locally ([Building From Source](Building-From-Source.md)).
2. Keep new subsystem build rules in modular `cmake/*_targets.cmake` files to minimize merge conflicts.
3. Follow the PR checklist:
   - Compiles with **0 warnings** at `/W4` under MSVC C++20.
   - All **62+ CTest suites** pass (`ctest --test-dir build --output-on-failure`).
   - Uses conventional commits (`feat(...)`, `fix(...)`, `test(...)`, `ci(...)`, `docs(...)`).
   - Regenerates status files via `python tools/generate_status.py --build-dir build` when adding new tests, DLL exports, or ISA coverage.

---

## 2. Milestone History (`v0.1` → Sprint 12)

- [x] **Bootstrap (`v0.1`):** MIT C++20 foundation, PE32+ inspector, `CallTelemetry`, GDeflate v1, BCPack, XMA2 IMDCT, Bink2, WinRT `HSTRING`, RDNA2 decoder + SSA IR, live Vulkan 1.4 compute.
- [x] **Sprint 1 — Live Loader & Core Shims:** Suspended-process APC hook loader (`openscarlett_hook.dll`), `G:\`/`T:\`/`S:\` mounts, `kernelx` `VirtualAlloc2` 64 KiB/4 MiB aliasing + `GpuTilePool`, `d3d12_xs` COM proxies + `xg_xs` + GPU FSR 1.0.
- [x] **Sprint 2 — RDNA2 Shader Lifter & DXR 1.1:** `MIMG` textures, `DS` LDS + atomics, `wave64`→`wave32`, Mesh/Amplification shaders (`spirv-val` + `dxc` SM 6.5), `ID3D12Device5` DXR Tier 1.1 + SAH BVH fallback.
- [x] **Sprint 3 — Live `Xbox-GDK-Samples`:** `SimpleTriangle`, `SimpleMeshShader`, `SimpleCompute`, 10-DLL `SystemCombo` running live through `openscarlett run` (`not_impl = 0`).
- [x] **Sprint 4 — Shader Cache, 19-DLL Stack & Multi-Pass GDK Bring-Up:** CPU wave32/64 interpreter, disk cache + `d3d12_xs` auto-lift, Opus MFT + WASAPI, full GFX10 `xg_xs`, `MicrosoftGame.Config` + CoW VFS, interactive `PresentX`, `SimplePBR`, `MeshletCull`, `DXRTriangle`, untouched MIT `SimpleDirectStorageCombo` (`23/23` CTest).
- [x] **Sprint 5 (`v0.3.0-alpha`) — Full Vulkan 1.4 Graphics, Mesh Shaders & HDR10:** Zero-SDK Vulkan 1.4 rasterization + `VK_EXT_mesh_shader` + Win32 swapchain, HDR10 ST.2084/scRGB `PresentX`, `XDisplay`/`XGameUI`/`X3DAudio`/`XAudio2FX`, Kahn topo DLL init, Clang-CL & MinGW-w64 CI (`29/29` CTest).
- [x] **Sprint 6 (`v0.3.1-alpha`) — 292/292 GDK Thunks, Child Hooks & First Playable Game:** `292/292` `XGamingRuntimeThunks.dll` parity (`0 E_NOINTERFACE`), `CreateProcess` child-hook propagation, BCPack BC1–BC7 + RDNA2 BVH4, `PSV0` wave bounds, DXIL MD5 re-signing, and 60 FPS live **Freedoom: Phase 1** & **ScarlettArena** (`37/37` CTest).
- [x] **Sprint 7 (`v0.4.0-alpha`) — Direct Vulkan 1.4 Routing, 3D Dual-API Scene & Native GUI:** Direct `d3d12_xs` → Vulkan 1.4 routing (`0.0000` MAE), lockstep LDS barriers, 30 routed DLLs, Raw Input mouse, `gdk_3d_scene` over D3D12 + Vulkan 1.4, and native Win32/Direct2D `OpenScarlettLauncher.exe` with SHA-256 updater (`42/42` CTest).
- [x] **Sprint 8 (`v0.4.1-alpha`) — GFX10.3 ISA Expansion (`774/779`) & 3,120-Shader Corpus:** Modular scalar/vector & memory/image ISA decoders, `openscarlett rdna2-bench --full` (`3,120/3,120` 5-stage AAA shaders across 12 workloads), embedded scarlet launcher icon (`45/45` CTest).
- [x] **Sprint 9 + Sprint 10 (`v0.5.0-alpha`) — Full Compatibility Engine:** All 13 `d3d12_xs` COM proxies + 6-thread multi-queue sync, 64-bit `TitleVAMirror` (`MEM_WRITE_WATCH`), raw `V#`/`T#`/`S#` bindless bridge, GPU compute untiling (`cs_6_5` + SPIR-V), HS/DS/GS stages, exact cross-half `Wave64-on-Wave32`, 7-core affinity remap, WinRT lifecycle, offline XSTS/XStore, `mfplat_xs` H.264/HEVC decode, First-Boot Probe + VEH 50-call ring + `OPENSCARLETT_CAPTURE_DIR` (`51/51` CTest).
- [x] **Sprint 11 — Hardware GPU Accuracy, AS `TaskEXT`, Sampler Feedback & XVD/LSA Preflight:** Real-GPU differential verification expanded to **`615/779`** opcodes + 4 GFX10.3 HW accuracy rules, SPIR-V 1.6 `TaskEXT` (`130/130` `spirv-val`) + `CullDistance`, console Sampler Feedback + `QueryAccumulatorX` + GPU-QPC clock calibration, 12 KiB XVD inspector + mixed-case `PeForwarder` + `ConsoleLsaPolicy`, and real `_GAMING_XBOX_SCARLETT` ABI parity (`57/57` CTest).
- [x] **Sprint 12 — AAA Bring-Up Wall Integration:** Live `d3d12_xs` island wiring + GXDK extensions, Xbox `dxc_xs` DXBC/PM4/SGPR shader container parser + EXEC64 CFG hardening + in-proc DXC cache, live RDNA2 `image_bvh(64)_intersect_ray` DXR/SPIR-V RayQuery bridge + 64-bit multi-window BVH4 + SBT remapping, hardened DirectStorage priority/custom-decomp/GPU-untile streaming + BCPack texture arrays + LRU `VramResidencyManager`, live `PeForwarder`/LSA/probe + TLS slot 0 + `NtCreateFile` VFS + lock-free 128-voice `XAudio2_9`, and 4 untouched Microsoft `Xbox-GDK-Samples` (`62/62` CTest).

---

## 3. What's Next (Active Roadmap)

- [ ] **First Commercial Xbox-ABI / GDK Title Bring-Up:** Boot → main menu → playable in-game driven by community First-Boot Probe, `CallTelemetry`, and VEH crash reports.
- [ ] **Final 5 GFX10.3 Edge Opcodes (`774 → 779`):** Close the remaining 4 `SMEM` and 1 `VOP2` edge opcodes and expand live-GPU verification from `615/779` toward `779/779`.
- [ ] **FSR 2 / 3 Temporal Upscaling & Retail Bink2 Variants:** Extend `amd_fidelityfx_upscaler_xbox.dll` beyond FSR 1.0 and broaden `Bink2Scarlett.dll` bitstream support.
- [ ] **Hardware Validation on Missing Tiers:** Verify NVIDIA RTX 40 (Ada), AMD RX 6000 (RDNA2) & Windows handheld APUs (ROG Ally / Legion Go), and Intel Arc (Alchemist / Battlemage).

---

## Publishing Updates to the GitHub Wiki

The canonical Markdown sources for this Wiki live in `wiki/` inside the main repository. Once the GitHub Wiki tab has been initialized (clicking **Create the first page** on `https://github.com/californiaAlmonds/OpenScarlett/wiki`), maintainers can sync `wiki/*.md` directly to GitHub's native Wiki repository with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_wiki.ps1
```
