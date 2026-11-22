# Building From Source

OpenScarlett is written in **C++20** with a modular CMake + Ninja build system. Primary development targets **Visual Studio 2022 MSVC**, with continuous portability checks on **Clang-CL** and **MinGW-w64**.

---

## Prerequisites (Windows 10 / 11 x64)

- **Windows 10 (1903+)** or **Windows 11 (x64)**
- **Visual Studio 2022 Community/Professional/Enterprise** with the *Desktop development with C++* workload (MSVC C++20)
- **Windows 10/11 SDK** (provides `d3d12.h`, `dxgi1_6.h`, and `dxc.exe` for SM 6.5/6.6 DXIL validation)
- **CMake 3.20+**
- **Ninja** build tool
- **Python 3.10+** *(optional — used to regenerate status SVGs, dashboard HTML, and README tables via `tools/generate_status.py`)*

---

## Standard Build (MSVC + CMake + Ninja)

Open an **x64 Native Tools Command Prompt for VS 2022** (or initialize `vcvars64.bat`) and run:

```powershell
git clone https://github.com/californiaAlmonds/OpenScarlett.git
cd OpenScarlett

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Workspace Convenience Scripts
In the repository root:
- `build.cmd` — Configures and builds the primary `build/` directory with MSVC + Ninja.
- `build_wt.cmd <worktree_dir>` — Configures and builds an isolated git worktree (used for parallel feature development without polluting the main build tree).

> **ABI Compatibility Note:** `CMakeLists.txt` defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` under MSVC so binaries built with VS 2022 17.10+ remain ABI-compatible with older `msvcp140.dll` (`< 14.40`) redistributables and avoid `0xC0000005` crashes inside `_Mtx_lock`.

---

## Compiler Portability Builds

OpenScarlett's GitHub Actions CI ([`.github/workflows/ci.yml`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/.github/workflows/ci.yml)) continuously builds and tests across three Windows toolchains:

### 1. MSVC (Primary — Blocking CI)
Both `Release` and `Debug` builds run all **62 CTest suites**, compile at `/W4` warning level, and smoke-test a portable release bundle with `build/` hidden so hardcoded build paths fail CI immediately.

### 2. Clang-CL (`build-clangcl`)
```powershell
cmake -S . -B build-clangcl -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
cmake --build build-clangcl --parallel
ctest --test-dir build-clangcl --output-on-failure
```
*(Untouched `_GAMING_XBOX_SCARLETT` DirectXMath samples automatically receive `/arch:AVX2` under Clang-CL.)*

### 3. MinGW-w64 (MSYS2 `MINGW64`)
```bash
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build-mingw --target openscarlett_core d3d12_xs --parallel
```
MinGW-w64 builds validate clean `.def` export tables (`d3d12_xs_mingw.def`, `kernelx_mingw.def`, `xmem_mingw.def`) and COM aggregate-return ABI compatibility.

---

## Modular CMake Architecture

To keep parallel worktrees and feature branches merge-conflict-free, `CMakeLists.txt` builds `openscarlett_core`, `winrt_xs`, `openscarlett`, and the base unit tests, then dynamically includes modular subsystem files under `cmake/`:

| CMake Module Group | Purpose |
| :--- | :--- |
| `cmake/loader_targets.cmake`, `cmake/worker_loader_child_hook.cmake`, `cmake/sprint11_xvd_loader_lsa.cmake`, `cmake/sprint12_os_loader.cmake` | `openscarlett_hook.dll`, APC injector, CoW VFS, child hooks, XVD inspector, `PeForwarder`, `ConsoleLsaPolicy`, NT VFS & GameInput/XAudio2 |
| `cmake/kernel_targets.cmake`, `cmake/sprint12_streaming_vram.cmake` | `kernelx.dll`, `xmem.dll`, `GpuTilePool`, `TitleVAMirror`, `VramResidencyManager`, DirectStorage/BCPack coherency |
| `cmake/graphics_targets.cmake`, `cmake/presentx_targets.cmake`, `cmake/wave3_vulkan_graphics.cmake`, `cmake/wave3_hdr10_d3d12.cmake`, `cmake/sprint11_d3d12_feedback_queries.cmake`, `cmake/sprint12_d3d12_aaa.cmake` | `d3d12_xs.dll` 13 COM proxies, HDR10 `PresentX`, Vulkan 1.4 graphics routing, Sampler Feedback, `QueryAccumulatorX`, GXDK extensions |
| `cmake/shader_targets.cmake`, `cmake/dxr_targets.cmake`, `cmake/worker_rdna2_corpus.cmake`, `cmake/sprint8_rdna2.cmake`, `cmake/sprint11_as_spirv.cmake`, `cmake/sprint11_rdna2_gpu_verify.cmake`, `cmake/sprint12_xbox_shader.cmake`, `cmake/sprint12_rayquery.cmake` | RDNA2/GFX10.3 ISA decoders, SSA IR, Xbox DXBC/PM4/SGPR container parser, SPIR-V/DXIL lifters, Wave64-on-32, DXR 1.1 & RayQuery bridge |
| `cmake/gdk_targets.cmake`, `cmake/audio_xg_targets.cmake`, `cmake/secondary_shims_targets.cmake`, `cmake/worker_xgr_conformance.cmake`, `cmake/worker_bcpack_bvh4.cmake` | `xgameruntime`, `XAudio2_9`, `xg_xs`, `dstorage_xs`, `GameInput`, `XCurl`, `XSAPI`, `Bink2Scarlett`, `XboxBvhBuilder`, FSR 1.0, diagnostic shims |
| `cmake/gdk_live_targets.cmake`, `cmake/gdkx_bringup_targets.cmake`, `cmake/worker_playable_game.cmake`, `cmake/sprint7_wave1.cmake` | Live GDK samples, untouched Microsoft `Xbox-GDK-Samples`, *Freedoom* / *ScarlettArena*, and dual-API `gdk_3d_scene` |
| `cmake/sprint7_launcher.cmake` | Native Win32/Direct2D `OpenScarlettLauncher.exe` with embedded icon resource (`launcher.rc`) and updater |

---

## Running Benchmarks & Updating Status Dashboards

### Audit RDNA2 ISA & 3,120-Shader Corpus
```powershell
# Full 3,120-shader validation across 12 AAA workloads + JSON report
.\build\openscarlett.exe rdna2-bench --full --json docs\rdna2_coverage.json

# Fast 240-shader sweep for quick local iteration
.\build\openscarlett.exe rdna2-bench --fast
```

### Regenerate Status SVGs, Dashboard & README Table
After building and running CTest, regenerate `docs/status.svg`, `docs/matrix.svg`, `docs/index.html`, and the auto-generated table inside `README.md`:

```powershell
python tools\generate_status.py --build-dir build
```

This script inspects actual PE export tables from `build/*.dll`, reads `build/Testing/Temporary/LastTest.log`, and parses `docs/rdna2_coverage.json` so all published numbers stay grounded in live measurements.
