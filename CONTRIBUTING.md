# Contributing to OpenScarlett

Thank you for your interest in **OpenScarlett**! You do **not** need to be a C++ developer to make a huge impact — **60-second hardware test reports** on different GPUs and Windows 11 builds are one of the most valuable contributions right now.

---

## 1. Hardware Testing (No Coding or Visual Studio Needed!)

### Why We Need Testers
OpenScarlett is currently developed and verified primarily on a single laptop (**Windows 10 x64 + NVIDIA GeForce GTX 1650 Ti**). That hardware has specific blind spots we cannot verify locally:
- **NVIDIA RTX 20 / 30 / 40 / 50 GPUs:** Real **DXR Tier 1.1 hardware RT-core** validation (`GTX 1650 Ti` has `DXR=0`, so it can only test our software BVH fallback).
- **AMD Radeon RX 6000 / 7000 / 9000 & Handheld APUs (ROG Ally / Legion Go / Steam Deck Windows):** Native **Wave64** execution (matching Xbox Series X|S RDNA2), plus AMD D3D12/Vulkan driver behavior.
- **Intel Arc (Alchemist / Battlemage) & Integrated GPUs (Xe / Vega / RDNA iGPU):** Subgroup/wave-size differences (`Wave8`/`Wave16`/`Wave32`), UMA memory heaps, and strict driver validation.
- **Windows 11 (23H2 / 24H2 / 25H2):** `VirtualAlloc2` / `MapViewOfFile3` placeholder memory aliasing, newer D3D12 Agility runtimes, and WinRT differences vs. Windows 10.

### How to Run & Submit in 60 Seconds
1. Download the latest **`OpenScarlett-*-win64.zip`** from [GitHub Releases](https://github.com/californiaAlmonds/OpenScarlett/releases/latest) and extract it anywhere.
2. Right-click **`run_hardware_test.ps1`** → **Run with PowerShell** *(or run `powershell -ExecutionPolicy Bypass -File .\run_hardware_test.ps1`)*.
   - It shows live progress as it probes your GPU(s) and runs all automated tests (Vulkan compute, D3D12 COM proxies, Mesh Shaders, GPU FSR 1.0, `VirtualAlloc2` memory aliasing, RDNA2 shader lifting, DXR 1.1, and live Xbox GDK samples).
   - It saves a privacy-sanitized `hardware_test_report.md` (automatically stripping your Windows username and local folder paths) and copies it to your clipboard.
3. Press **Enter** (`Y`) when prompted — it opens a **pre-filled GitHub Hardware Report issue** in your browser. Review and click **Create**!

> **Note:** Failing tests on a new GPU or Windows 11 build are *just as valuable* as passing ones — please submit them!

---

## 2. Code Contributions

All contributions are submitted under the [MIT License](LICENSE).

### Building Locally (Windows 10 / 11 x64)
Requirements: Visual Studio 2022 (MSVC C++20) + Windows 10/11 SDK (`dxc.exe`) + CMake 3.20+ + Ninja.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Pull Request Checklist
- Compiles cleanly with `/W4` (0 warnings) on MSVC C++20.
- All CTest suites pass (`ctest --test-dir build --output-on-failure`).
- Follow conventional commits (`feat(...)`, `fix(...)`, `test(...)`, `ci(...)`, `docs(...)`).
- See [`AGENTS.md`](AGENTS.md) for architecture conventions and modular `cmake/*_targets.cmake` hooks.
