# CLI, Environment Variables & Diagnostics

OpenScarlett provides both a native Windows GUI (**`OpenScarlettLauncher.exe`**) and a full-featured CLI (**`openscarlett.exe`**) along with deep telemetry, crash diagnostics, and frame/shader dumping tools.

---

## 1. `openscarlett.exe` Command-Line Reference

### `probe-gpu` — Inspect Host Hardware & Toolchain
```powershell
openscarlett.exe probe-gpu
```
Lists every D3D12 adapter (Hardware & WARP), VRAM, Wave lane range, Shader Model, DXR tier, Mesh Shader tier, Vulkan 1.4 ICD capabilities (`VK_EXT_mesh_shader`, Win32 swapchain, HDR10), and detected `dxc.exe`.

### `probe` — Preflight Compatibility Check (EXE, Package Dir, or XVD/XVC)
```powershell
openscarlett.exe probe <exe|dir|xvd> [--json]
```
- **For an EXE or unpacked package folder:** Parses `MicrosoftGame.Config` / `.mgc`, computes `ConsoleLsaPolicy` identity (TitleId, PFN, SandboxId), resolves every PE import & forwarder chain via `PeForwarderResolver`, counts shimmed/Windows/API-set/native/missing imports, and outputs `ready_to_launch`.
- **For `.xvd` / `.xvc` files:** Performs read-only 12 KiB header geometry inspection and reports disk type, layout geometry, and encryption status (`ready_to_launch: false` when encrypted).

### `inspect` — PE32+ x64 Binary Inspector
```powershell
openscarlett.exe inspect <path\to\binary.exe_or_dll>
```
Prints ImageBase, image size, section count, relocations, TLS callbacks, `.pdata` SEH entry count, exports (including forwarded exports), and all imported DLLs categorized as `console-only`, `GDK`, `delay-loaded`, or `Windows`.

### `rdna2-bench` — GFX10.3 ISA & 3,120-Shader Corpus Benchmark
```powershell
openscarlett.exe rdna2-bench [--full | --fast] [--json <path>] [--corpus-count <N>] [--no-gpu] [--no-external]
```
- `--full`: Runs all **3,120** 5-stage (`CS`/`VS`/`PS`/`MS`/`AS`) shaders across 12 AAA workloads and audits all 779 GFX10.3 opcodes.
- `--fast`: Runs a fast **240-shader** sweep.
- `--json <path>`: Writes deterministic JSON coverage report (used to generate `docs/rdna2_coverage.json`).
- `--no-gpu` / `--no-external`: Skips live GPU dispatch or external `spirv-val`/`dxc.exe` invocation.

### `run` — Launch Guest Package or Executable via APC Hook
```powershell
openscarlett.exe run <package_dir_or_config_or_exe> [options] [-- <guest-args...>]
```

| Option | Description |
| :--- | :--- |
| `--shim-dir <dir>` | Directory containing OpenScarlett's 30 shim DLLs and `openscarlett_hook.dll` |
| `--vfs-g <dir>` | Read-only backing directory for `G:\` / `GAME:\` |
| `--vfs-g-overlay <dir>` | Copy-on-Write overlay directory for writes targeting `G:\` / `GAME:\` |
| `--vfs-t <dir>` | Backing directory for `T:\` (title scratch) |
| `--vfs-s <dir>` | Backing directory for `S:\` (persistent saves) |
| `--telemetry <json>` | Output file path for unified `CallTelemetry` JSON report |
| `--backend <d3d12\|vulkan>` | Select GPU backend (`d3d12` default, or `vulkan` for direct `d3d12_xs` → Vulkan 1.4) |
| `--probe-unknown-imports` | Enable **First-Boot Probe Mode** (`E_NOTIMPL` trampoline pool + Win32 VEH crash monitor) |
| `--timeout-ms <ms>` | Terminate guest after `<ms>` milliseconds (`0` = infinite; exits with code `124` on timeout) |
| `-- <guest-args...>` | Pass remaining arguments verbatim to the guest executable |

---

## 2. `OpenScarlettLauncher.exe` Flags

```powershell
# Launch the native Win32/Direct2D GUI
OpenScarlettLauncher.exe

# Headless / CI self-check (verifies Direct2D/DWrite, GPU probe, sanitizer & updater offline)
OpenScarlettLauncher.exe --headless-check
```

---

## 3. Runtime Environment Variables Reference

### GPU, Shaders & Capture
| Variable | Values / Format | Purpose |
| :--- | :--- | :--- |
| `OPENSCARLETT_GPU_BACKEND` / `OPENSCARLETT_BACKEND` | `d3d12` \| `vulkan` | Override GPU translation backend |
| `OPENSCARLETT_PREFER_WARP` | `1` | Force D3D12 WARP software adapter |
| `OPENSCARLETT_VK_DEVICE` | Substring or index | Select a specific Vulkan physical device |
| `OPENSCARLETT_VK_FORCE_SOFTWARE` | `1` | Force Vulkan software ICD |
| `OPENSCARLETT_FORCE_HDR` | `1` | Force HDR10 swapchain output path |
| `OPENSCARLETT_DXC_PATH` | Path to `dxc.exe` | Override DXIL compiler path |
| `OPENSCARLETT_SHADER_CACHE` | Directory path | Override content-addressed disk shader cache directory |
| `OPENSCARLETT_ASYNC_PSO` | `0` \| `1` | Enable/disable async background PSO pre-compilation pool |
| `OPENSCARLETT_CAPTURE_DIR` | Directory path | Enable automatic frame, draw-call, and shader bytecode capture |
| `OPENSCARLETT_CAPTURE_MAX_DRAWS` | Integer | Limit captured draw calls per frame |

### Loader, VFS & Diagnostics
| Variable | Values / Format | Purpose |
| :--- | :--- | :--- |
| `OPENSCARLETT_SHIM_DIR` | Directory path | Override shim DLL directory |
| `OPENSCARLETT_HOOK_DLL` | Path to DLL | Override `openscarlett_hook.dll` path |
| `OPENSCARLETT_DLL_ROUTING` | Routing override string | Customize per-DLL routing rules |
| `OPENSCARLETT_WORK_DIR` | Directory path | Base portable scratch directory (prevents `C:\` writes) |
| `OPENSCARLETT_VFS_G` / `OPENSCARLETT_VFS_G_OVERLAY` | Directory path | Override `G:\` read-only root and CoW overlay |
| `OPENSCARLETT_VFS_G_SUBMOUNTS` | Submount spec | Register nested read-only sub-mounts under `G:\` (e.g. DLC packs) |
| `OPENSCARLETT_VFS_T` / `OPENSCARLETT_VFS_S` | Directory path | Override `T:\` scratch and `S:\` save directories |
| `OPENSCARLETT_SAVE_ROOT` | Directory path | Override persistent `XGameSave` root |
| `OPENSCARLETT_FORBID_SYSTEM_DRIVE_WRITES` | `1` | Actively reject mutating VFS operations targeting `C:\` |
| `OPENSCARLETT_PROBE_UNKNOWN_IMPORTS` | `1` | Enable First-Boot Probe `E_NOTIMPL` trampolines + VEH crash monitor |
| `OPENSCARLETT_TELEMETRY_PATH` / `OPENSCARLETT_TELEMETRY_BASE` | File / dir path | Override `CallTelemetry` JSON output location |
| `OPENSCARLETT_CHILD_TELEMETRY_PER_PID` | `1` | Emit separate telemetry JSON files for spawned child processes |
| `OPENSCARLETT_NO_UPDATE_CHECK` | `1` | Disable Launcher GitHub Releases update check |

### Console Identity, Offline Auth, Audio & UI
| Variable | Values / Format | Purpose |
| :--- | :--- | :--- |
| `OPENSCARLETT_OFFLINE` | `1` | Force offline auth/XSTS synthesis mode |
| `OPENSCARLETT_CONSOLE_TYPE` | Console type ID | Override reported Scarlett / Anaconda / Lockhart console model |
| `OPENSCARLETT_TITLE_ID` / `OPENSCARLETT_PACKAGE_ID` / `OPENSCARLETT_SANDBOX_ID` / `OPENSCARLETT_STORE_ID` | String / hex | Override package identity metadata |
| `OPENSCARLETT_GAMERTAG` / `OPENSCARLETT_XUID` | String / uint64 | Customize offline Xbox profile gamertag and XUID |
| `OPENSCARLETT_LOCALE` / `OPENSCARLETT_INSTALLED_LANGUAGES` | e.g. `en-US` | Override console locale & installed language list |
| `OPENSCARLETT_STORE_OWNED_PACKAGES` / `OPENSCARLETT_STORE_ADDON_IDS` | Comma-separated IDs | Configure offline `XStore` owned license/DLC responses |
| `OPENSCARLETT_AUDIO_NULL` | `1` | Use deterministic silent WASAPI sink (ideal for CI/headless runs) |
| `OPENSCARLETT_UI_DIALOG_BUTTON` / `OPENSCARLETT_UI_TEXT_ENTRY` | Button index / text | Automated response for headless `XGameUI` dialog prompts |

---

## 4. Telemetry, Crash Logs & Frame Capture

### `CallTelemetry` JSON Reports
Every run via `openscarlett run --telemetry report.json` tracks per-symbol call counts across all 30 shim DLLs and highlights any missing blockers (`not_impl > 0`). On exit, the CLI prints the top 10 `[BLOCKER]` entries so contributors immediately know which APIs a new title requires next.

### Win32 VEH 50-Call Ring Buffer (`--probe-unknown-imports`)
When a guest hits an unhandled exception under First-Boot Probe mode, `probe_veh.cpp` dumps:
- Exception code, faulting address, and relative module+offset
- CPU register snapshot (`RIP`, `RSP`, `RAX`–`R15`)
- The **50 most recent shimmed API calls** prior to the crash
- Automatic redaction of `%USERPROFILE%`, usernames, and local drive prefixes

### Frame & Shader Capture (`OPENSCARLETT_CAPTURE_DIR`)
Setting `OPENSCARLETT_CAPTURE_DIR=E:\captures\my_title` dumps rendered frames, draw metadata, and intercepted RDNA2/DXIL shader blobs (`src/gpu/frame_capture.cpp`), making shader and rendering bug reproduction straightforward without needing the original game files.
