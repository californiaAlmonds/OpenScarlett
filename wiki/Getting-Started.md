# Getting Started

This guide walks through downloading and running **OpenScarlett** on Windows 10 or Windows 11 (x64) with zero coding or Visual Studio installation required.

---

## System Requirements

| Requirement | Minimum | Recommended |
| :--- | :--- | :--- |
| **OS** | Windows 10 (1903+) x64 | Windows 10 22H2 / Windows 11 24H2–26H2 x64 |
| **CPU** | 64-bit x86-64 processor (AVX2 recommended) | 8-core / 16-thread x86-64 (matches Xbox Series X\|S topology) |
| **GPU** | DirectX 12 Feature Level 12_0 or Vulkan 1.2+ (or D3D12 WARP fallback) | DirectX 12 Ultimate (SM 6.5+, Mesh Shaders, DXR 1.1) + Vulkan 1.4 |
| **Toolchain** | None needed for portable releases (`dxc.exe` is bundled) | Only needed if [Building From Source](Building-From-Source.md) |

---

## 1. Download the Portable Release

1. Download the latest **`OpenScarlett-*-win64.zip`** archive from [GitHub Releases](https://github.com/californiaAlmonds/OpenScarlett/releases/latest).
2. Extract it to any writable folder (for example `D:\OpenScarlett` or `E:\OpenScarlett`).
3. Verify the folder contains:
   - `OpenScarlettLauncher.exe` — Native dark Win32/Direct2D GUI launcher
   - `openscarlett.exe` — Command-line runner, inspector, and RDNA2 benchmark tool
   - `openscarlett_hook.dll` + all 30 shimmed Xbox/GDK DLLs (`d3d12_xs.dll`, `kernelx.dll`, `xgameruntime.dll`, etc.)
   - Bundled open-source Microsoft `dxc.exe`, `dxcompiler.dll`, and `dxil.dll`
   - `run_hardware_test.ps1` / `run_hardware_test.bat` — 60-second hardware self-test scripts

> **Portable Storage:** OpenScarlett never writes saves, VFS scratch files, or telemetry to `C:\` when launched from a writable folder. By default, writable runtime data is placed under `.openscarlett_tmp` alongside the executable (or `OPENSCARLETT_WORK_DIR` if set).

---

## 2. Using the Native Windows Launcher (`OpenScarlettLauncher.exe`)

Double-click **`OpenScarlettLauncher.exe`** to open the zero-dependency native Win32/Direct2D GUI. It includes:

1. **Live GPU Telemetry Strip** (top bar) — Automatically probes your host D3D12 adapter(s), Shader Model, Wave lane size, Mesh Shader tier, DXR tier, Vulkan API version, and detected `dxc.exe`.
2. **Play & Launch Tab**
   - Pick an unpacked Xbox GDK package directory (containing `MicrosoftGame.Config`), a `.mgc` config file, or a standalone `.exe`.
   - Choose the GPU backend (**DirectX 12** default or **Vulkan 1.4**).
   - Toggle **First-Boot Probe Mode** (`--probe-unknown-imports`) when testing a brand-new binary for the first time — automatically synthesizes `E_NOTIMPL` trampolines for missing imports and attaches the Win32 VEH 50-call API ring buffer crash logger.
   - Click **Copy Sanitized Telemetry** to copy a privacy-stripped summary (usernames and local paths removed) to share on [Discord](https://discord.gg/sShMY2fjR) or GitHub Issues.
3. **60s Hardware Self-Test Tab**
   - Click **Run 60s Hardware Test** to validate your GPU and Windows build across the bundled tests.
   - Click **Open Pre-Filled GitHub Hardware Report** to submit a sanitized hardware compatibility report in one click.
4. **PE32+ Binary Inspector Tab**
   - Inspect any x64 `.exe` or `.dll` to list its sections, TLS callbacks, `.pdata` SEH entries, exports, and console/GDK/Windows imports.
5. **Opt-In SHA-256 Verified Updater**
   - Checks GitHub Releases via WinHTTP and verifies downloaded archives with Windows BCrypt **SHA-256** against the release `.sha256` checksum before staging an update. Disable checks with `OPENSCARLETT_NO_UPDATE_CHECK=1`.

---

## 3. Running the 60-Second Hardware Test (CLI Alternative)

If you prefer the command line or headless execution:

```powershell
# Interactive script (prompts to open pre-filled GitHub report)
powershell -ExecutionPolicy Bypass -File .\run_hardware_test.ps1

# Non-interactive / CI mode
powershell -ExecutionPolicy Bypass -File .\run_hardware_test.ps1 -NoPause
```

The script outputs `hardware_test_report.md` with your Windows username and local directory paths automatically redacted.

---

## 4. Launching an Unpacked GDK Package via CLI

```powershell
# 1. Run a preflight compatibility check before launching
.\openscarlett.exe probe path\to\UnpackedPackage

# 2. Launch using the default DirectX 12 backend with telemetry
.\openscarlett.exe run path\to\UnpackedPackage --telemetry report.json

# 3. Launch routing d3d12_xs directly to Vulkan 1.4
.\openscarlett.exe run --backend=vulkan path\to\UnpackedPackage

# 4. Launch an unknown binary with First-Boot Probe + VEH diagnostics
.\openscarlett.exe run --probe-unknown-imports path\to\game.exe --telemetry probe.json
```

> **`.xvd` / `.xvc` Containers:** If you pass an `.xvd` or `.xvc` container to `openscarlett probe`, OpenScarlett inspects its 12 KiB geometry header and reports container metadata and encryption status (`ready_to_launch: false` when encrypted).

---

## Next Steps
- Explore [CLI, Environment Variables & Diagnostics](CLI-Environment-and-Diagnostics.md) for advanced launch flags and frame/shader dumping.
- Check [Testing, Compatibility & Hardware](Testing-Compatibility-and-Hardware.md) to see what workloads and GPUs are verified today.
- Want to compile and hack on OpenScarlett? Head to [Building From Source](Building-From-Source.md).
