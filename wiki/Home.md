<p align="center">
  <img src="https://raw.githubusercontent.com/californiaAlmonds/OpenScarlett/main/docs/banner.png" alt="OpenScarlett" width="760" />
</p>

# Welcome to the OpenScarlett Wiki

**OpenScarlett** is an MIT-licensed C++20 compatibility and translation layer for running **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 Windows PE software natively on **Windows 10 & Windows 11 PCs**.

Because the Xbox Series X|S runs an **AMD Zen 2 x86-64 CPU** with a Windows-derived Game Core kernel, OpenScarlett executes guest CPU code **natively** rather than emulating an instruction set — translating only the console-specific graphics (`d3d12_xs`, `xg_xs`), AMD RDNA2/GFX10.3 shaders, unified memory, DirectStorage/BCPack streaming, audio, input, and GDK/OS runtime calls to native PC **DirectX 12**, **Vulkan 1.4**, and Win32 APIs.

---

## Project Snapshot (Sprint 12 — Early Alpha)

| Metric | Live Measured Value |
| :--- | :--- |
| **Automated CTest Suite** | **`62 / 62` passing** (NVIDIA GTX 1650 Ti, D3D12 WARP, CI Release/Debug, plus 6 community GPUs) |
| **Shimmed Xbox / GDK DLLs** | **`30` DLLs** (`993` PE exports — 12 High, 8 Partial, 10 Stub) |
| **Shipping GDK Thunk Parity** | **`292 / 292`** `XGamingRuntimeThunks.dll` exports (`24` COM `QueryApiImpl` interfaces, `0 E_NOINTERFACE`) |
| **RDNA2 / GFX10.3 ISA Coverage** | **`774 / 779` (99.4%)** decoded, lifted (SPIR-V + DXIL) & CPU-referenced · **`615 / 779`** real-GPU verified |
| **Validated AAA Shader Corpus** | **`3,120 / 3,120`** 5-stage (`CS`/`VS`/`PS`/`MS`/`AS`) shaders across 12 workloads (`spirv-val` + `dxc` SM 6.5) |
| **Verified Community Hardware** | **`7` PCs** — NVIDIA Turing/Ampere/Blackwell (`GTX 1650 Ti`, `RTX 2060S`, `3080 Ti`, `5070`) & AMD RDNA 3/4 (`RX 7600`, `7900 XTX`, `9070 XT`) |
| **Commercial Retail Title Status** | **Early Alpha** — Playable demos (*Freedoom: Phase 1*, *ScarlettArena*), shadowed 3D dual-API GDK scene, and 4 untouched MIT `Xbox-GDK-Samples` run live; **commercial retail Xbox Series games are not playable yet**. |

---

## Wiki Directory

### Getting Started & Guides
- **[Getting Started](Getting-Started.md)** — Download the portable Win64 build, use `OpenScarlettLauncher.exe`, run the 60-second Hardware Self-Test, and launch unpacked GDK apps.
- **[Building From Source](Building-From-Source.md)** — Build with MSVC C++20, CMake, Ninja, Clang-CL, or MinGW-w64; run the 62-test CTest suite; regenerate status dashboards.

### Architecture & Subsystem Deep Dives
- **[Architecture Overview](Architecture-Overview.md)** — How native x86-64 execution, APC injection, 30-DLL routing, dual D3D12/Vulkan 1.4 rendering, and RDNA2 lifting fit together.
- **[Loader & Virtual Filesystem (VFS)](Loader-and-VFS.md)** — PE32+ inspection, suspended-process APC hook loader, `CreateProcess` child-hook propagation, `PeForwarderResolver`, `MicrosoftGame.Config`, `ConsoleLsaPolicy`, 12 KiB XVD inspection, and `NtCreateFile` / CoW `G:\`, `T:\`, `S:\` mounts.
- **[Graphics: DirectX 12 (`d3d12_xs`) & Vulkan 1.4](Graphics-D3D12-and-Vulkan.md)** — All 13 Xbox COM proxies, 6-thread multi-queue sync, GXDK extensions, Sampler Feedback, `QueryAccumulatorX`, HDR10 `PresentX`, direct `d3d12_xs` → Vulkan 1.4 routing, `xg_xs` GPU compute untiling, and GPU FSR 1.0.
- **[RDNA2 Shader Lifter & Ray Tracing](RDNA2-Shader-Lifter-and-Raytracing.md)** — GFX10.3 ISA decoders (`774/779`), Xbox DXBC/PM4/SGPR container parser, SSA IR + EXEC-mask CFG hardening, SPIR-V 1.6 (`TaskEXT`/`MeshEXT`) & DXIL SM 6.5/6.6 lifting, Wave64-on-Wave32, bindless `V#`/`T#`/`S#`, async PSO cache, and DXR 1.1 / `image_bvh64_intersect_ray` RayQuery bridge.
- **[Memory, Streaming & Storage](Memory-Streaming-and-Storage.md)** — `kernelx`/`xmem` `VirtualAlloc2` 64 KiB & 4 MiB page aliasing, `GpuTilePool`, 64-bit `TitleVAMirror` (`MEM_WRITE_WATCH`), LRU `VramResidencyManager`, and `dstorage_xs` (priority queues, GDeflate v1, BCPack BC1–BC7 texture arrays, GPU untile upload).
- **[Audio, Video & Input](Audio-Video-and-Input.md)** — Lock-free 128-voice `XAudio2_9` / WASAPI / `X3DAudio`, `AcpHal`, XMA2/Opus/ADPCM, `mfplat_xs` H.264/HEVC D3D12 video decode, `Bink2Scarlett`, and `GameInput` (XInput, Raw Input mouse, hot-plug, impulse triggers).

### References & Community
- **[System DLL Shims Reference (All 30 DLLs)](System-DLL-Shims-Reference.md)** — Export counts, coverage tiers, and capabilities for all 30 shimmed DLLs (`993` exports).
- **[CLI, Environment Variables & Diagnostics](CLI-Environment-and-Diagnostics.md)** — `openscarlett.exe` subcommands, `OpenScarlettLauncher.exe`, First-Boot Probe mode, VEH 50-call crash ring buffer, `CallTelemetry`, frame/shader capture, and all `OPENSCARLETT_*` env vars.
- **[Testing, Compatibility & Hardware](Testing-Compatibility-and-Hardware.md)** — Honest workload compatibility matrix, all 62 CTest suites, live GPU measurements, and community hardware tiers.
- **[Contributing & Roadmap](Contributing-and-Roadmap.md)** — Sprint history (`v0.1` → Sprint 12), parallel worktree workflow, and how to contribute code or hardware reports.

---

## Quick Links
- **GitHub Repository:** [californiaAlmonds/OpenScarlett](https://github.com/californiaAlmonds/OpenScarlett)
- **Interactive Coverage Dashboard:** [californiaalmonds.github.io/OpenScarlett](https://californiaalmonds.github.io/OpenScarlett/)
- **Latest Portable Win64 Release:** [GitHub Releases](https://github.com/californiaAlmonds/OpenScarlett/releases/latest)
- **Community Discord:** [Join Discord](https://discord.gg/sShMY2fjR)
