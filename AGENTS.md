# OpenScarlett — Workspace Engineering Rules (`AGENTS.md`)

## 1. Mission & Engineering Philosophy
**OpenScarlett** is an MIT-licensed C++20 compatibility & translation layer for running **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 Windows PE software natively on **Windows 10 & Windows 11 PCs**.

- **Real Binaries & Live GPU Over Guesswork:** Every subsystem must be validated against real compiled workloads — Microsoft's official MIT `Xbox-GDK-Samples`, real D3D12/Mesh Shader binaries via our suspended-process APC hook loader + live COM vtable/import telemetry, and real RDNA2 shader lifting validated by `spirv-val` + `dxc.exe` SM 6.5/6.6 on live D3D12 & Vulkan GPUs.
- **Radical Honesty in Docs:** `README.md` and `docs/status.svg` must always distinguish between:
  - **Live-Tested** (verified on real GPU / real GDK binaries)
  - **Unit-Tested** (verified on synthetic tests)
  - **In Progress / Planned**

## 2. Host Environment & Disk Constraints
- **Host OS:** Windows 10 x64 laptop
- **Host GPU:** NVIDIA GeForce GTX 1650 Ti (D3D12 + Vulkan 1.4.329; Turing TU117 — wave32 native, no HW RT cores; always test both hardware GPU and D3D12 WARP!)
- **Toolchain (ALL builds & temp files strictly on `E:\` — NEVER write project/build/temp files to `C:\`!):**
  - Compiler: Visual Studio 2022 Community MSVC (`vcvars64.bat`, C++20)
  - CMake: `E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe`
  - Ninja: `E:\ScarlettX-dev\tools\ninja\ninja.exe`
  - DXIL Shader Compiler: `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`
  - Rust (optional helper tools): `rustc 1.95.0`
  - Build command: `cmd /c E:\OpenScarlett\build.cmd`
  - Worktree builds for parallel Neon subagents: `cmd /c E:\OpenScarlett\build_wt.cmd <worktree_dir>`

## 3. Commit-Often Visibility & Parallel Neon Workflow
- **Commit & push frequently** in small, atomic, well-titled conventional commits (`feat(...)`, `fix(...)`, `test(...)`, `docs(...)`) so GitHub shows clear, transparent real-time progress.
- **Regenerate status badges** (`python tools/generate_status.py --build-dir build`) after every milestone so `README.md` stays accurate and readable for humans.
- **Parallel Neon Subagents:** When work splits across independent subsystems, create isolated git worktrees under `E:\OpenScarlett\.worktrees\<name>` (never `C:\`), assign non-overlapping files to parallel `neon` subagents, require 0 warnings + 100% CTest pass per worktree, then merge cleanly into `main`, re-run full CTest + `generate_status.py`, and push.
