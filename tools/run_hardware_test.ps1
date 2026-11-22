# OpenScarlett — 1-Click Portable Hardware & Self-Test Runner
# Runs host GPU/OS probes + all OpenScarlett live tests, shows clear live results,
# saves a privacy-sanitized Markdown report, and optionally opens a pre-filled GitHub Issue.

[CmdletBinding()]
param(
    [Alias('no-pause')]
    [switch]$NoPause,
    [Alias('no-submit')]
    [switch]$NoSubmit,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Continue'

foreach ($arg in $ExtraArgs) {
    if ($arg -eq '--no-pause' -or $arg -eq '-NoPause') { $NoPause = $true }
    if ($arg -eq '--no-submit' -or $arg -eq '-NoSubmit') { $NoSubmit = $true }
}
if ($NoPause) {
    $NoSubmit = $true
}

$RepoOwner = 'californiaAlmonds'
$RepoName  = 'OpenScarlett'
$BundleDir = $PSScriptRoot.TrimEnd('\')
$ReportMd  = Join-Path $BundleDir 'hardware_test_report.md'

# Keep all temporary saves isolated under %TEMP%
$SaveRoot = Join-Path $env:TEMP 'openscarlett_hw_test_saves'
New-Item -ItemType Directory -Force -Path $SaveRoot | Out-Null
$env:OPENSCARLETT_SAVE_ROOT = $SaveRoot
$env:OPENSCARLETT_AUDIO_NULL = '1'

# When the tests run from a build\ or bin\ folder (optionally build\Release etc.),
# the loader puts its work dir under <parent>\.tmp\openscarlett_run
# (ResolveDefaultWorkDirW in src/loader/vfs_mounts.cpp), so that parent folder
# shows up in test output too.
function Get-WorkRootDir {
    $dir = $BundleDir
    $leaf = (Split-Path $dir -Leaf).ToLowerInvariant()
    if (@('debug', 'release', 'relwithdebinfo', 'minsizerel') -contains $leaf) {
        $parent = Split-Path $dir -Parent
        if ($parent -and @('build', 'bin') -contains (Split-Path $parent -Leaf).ToLowerInvariant()) {
            $dir = $parent
            $leaf = (Split-Path $dir -Leaf).ToLowerInvariant()
        }
    }
    if (@('build', 'bin') -notcontains $leaf) { return '' }
    $root = Split-Path $dir -Parent
    if (-not $root -or $root.TrimEnd('\').Length -le 2) { return '' }
    # Never mask a folder that contains the user profile; the user-home and
    # username rules below handle those paths.
    if ($env:USERPROFILE -and ($env:USERPROFILE.TrimEnd('\') + '\').StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { return '' }
    return $root.TrimEnd('\')
}
$WorkRootDir = Get-WorkRootDir

function Sanitize-Text {
    param([string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return '' }
    $out = $Text
    $replacements = @(
        @{ Key = $BundleDir;         Val = '<BUNDLE_DIR>' },
        @{ Key = $WorkRootDir;       Val = '<REPO_DIR>' },
        @{ Key = $env:USERPROFILE;   Val = '<USER_HOME>' },
        @{ Key = $env:TEMP;          Val = '<TEMP>' },
        @{ Key = $env:LOCALAPPDATA;  Val = '<LOCALAPPDATA>' },
        @{ Key = $env:APPDATA;       Val = '<APPDATA>' }
    )
    foreach ($r in $replacements) {
        if (-not [string]::IsNullOrWhiteSpace($r.Key)) {
            $out = [regex]::Replace($out, [regex]::Escape($r.Key), $r.Val, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            $fwd = $r.Key.Replace('\', '/')
            if ($fwd -ne $r.Key) {
                $out = [regex]::Replace($out, [regex]::Escape($fwd), $r.Val, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERNAME)) {
        $out = [regex]::Replace($out, '(?i)([A-Za-z]:[\\/]+Users[\\/]+)' + [regex]::Escape($env:USERNAME), '${1}<USER>')
    }
    return $out
}

function Get-FriendlyOsVersion {
    try {
        $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction Stop
        $build = [int]$cv.CurrentBuildNumber
        $ubr = if ($null -ne $cv.UBR) { ".$($cv.UBR)" } else { '' }
        $display = if (-not [string]::IsNullOrWhiteSpace($cv.DisplayVersion)) { $cv.DisplayVersion } else { $cv.ReleaseId }
        $osName = if ($build -ge 22000) { 'Windows 11' } else { 'Windows 10' }
        $arch = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }
        if (-not [string]::IsNullOrWhiteSpace($display)) {
            return "$osName $display $arch (Build $build$ubr)"
        }
        return "$osName $arch (Build $build$ubr)"
    } catch {
        return "Windows ($([System.Environment]::OSVersion.VersionString))"
    }
}

$TestDescriptions = @{
    'test_pe_inspect.exe'         = 'PE32+ import/export classifier & CallTelemetry JSON'
    'test_codecs.exe'             = 'Xbox codecs (GDeflate v1, BCPack, XMA2 audio, Bink2 video)'
    'test_vulkan_live.exe'        = 'Live Vulkan 1.2/1.4 GPU compute shader'
    'test_rdna2_core.exe'         = 'RDNA2 / GFX10.3 AAA decoder, DXBC parser, CPU interpreter & shader cache'
    'test_winrt_xs.exe'           = 'WinRT activation & HSTRING runtime (winrt_xs.dll)'
    'test_apc_loader.exe'         = 'Suspended-process APC hook loader & G:/T:/S: VFS'
    'test_loader_vfs_config.exe'  = 'MicrosoftGame.Config loader, CoW overlay VFS, forwarders/TLS/.pdata & 19-DLL routing'
    'test_kernelx.exe'            = 'Win10/11 VirtualAlloc2/MapViewOfFile3 VA aliasing & kernelx.dll'
    'test_d3d12_xs_live.exe'      = 'Live D3D12 COM proxy, xg_xs swizzle & GPU FSR 1.0'
    'test_gdk_shims.exe'          = 'GDK runtime shims (xgameruntime, GameInput, XAudio2, dstorage, XSAPI)'
    'test_audio_opus_wasapi.exe'  = 'Opus MFT (0x704F), IMA-ADPCM & live host WASAPI playback'
    'test_xg_dstorage.exe'        = 'Full GFX10 2D/3D/mip-tail xg_xs swizzle & hardened dstorage_xs queues'
    'test_rdna2_lifter_live.exe'  = 'RDNA2 -> SPIR-V & SM 6.5 DXIL lifting + CPU wave32/64 interpreter parity'
    'test_d3d12_xs_autolift.exe'  = 'Live d3d12_xs XS2/Xbox-DXBC auto-lifting, disk shader cache & async PSO'
    'test_dxr_live.exe'           = 'DXR Tier 1.1 hardware raytracing / SAH BVH fallback'
    'test_secondary_shims.exe'    = 'xgameruntime QueryApiImpl dispatch & 9 secondary console/GDK shim DLLs'
    'test_presentx_windowing.exe'   = 'Interactive Win32 PresentX swapchains (60/120 Hz, waitable, Alt+Enter) & GameInput'
    'test_gdk_live.exe'             = 'End-to-end live Xbox GDK samples (SimpleTriangle, SimpleMeshShader, SimpleCompute, SystemCombo)'
    'test_gdk_simple_pbr.exe'       = 'Multi-frame 3D Cook-Torrance SimplePBR (4x MSAA, D32 depth, PBR textures, CBs)'
    'test_gdk_meshlet_cull.exe'     = 'AS+MS+PS GPU frustum & normal-cone MeshletCull (MeshTier=10 + WARP fallback)'
    'test_gdk_dxr_triangle.exe'     = 'End-to-end DXRTriangle (native lib_6_3 HW RT cores / SAH BVH fallback)'
    'test_external_gdk_bringup.exe' = 'External GDK-header binaries, untouched MIT DirectStorageCombo & 19-DLL full stack'
    'test_vulkan_graphics_live.exe' = 'Full Vulkan 1.2/1.4 graphics, 4x MSAA, 3D textures, VK_EXT_mesh_shader & Win32 swapchain'
    'test_hdr10_d3d12.exe'          = 'HDR10 ST.2084 & scRGB PresentX, SDR tone-map, default Texture2DArray SRV & VA remap'
    'test_gdk_extended_loader.exe'  = 'XDisplay HDR, XGameUI/Invite/Speech, X3DAudio/XAudio2FX & topological DLL init order'
    'test_gdk_simple_lighting.exe'  = 'DirectXTK12-style 3D Blinn-Phong SimpleLighting (D3D12 HW/WARP & Vulkan 1.4 parity)'
    'test_gdk_hdr_cubemap.exe'         = 'HDRCubeMap (FP16 TextureCube + default Array SRV + HDR10 ST.2084 + SDR tone-map)'
    'test_vulkan_mesh_showcase.exe'    = 'VulkanMeshShowcase (RDNA2 XS2 AS+MS+PS + 3D LUT: Vulkan 1.4 vs D3D12 MeshTier=10)'
    'test_rdna2_corpus_parity.exe'     = 'RDNA2/GFX10.3 corpus parity (v_cmpx, f16/d16, NaN/denorm, PSV0 wave, mesh, DXIL MD5 re-sign)'
    'test_gpu_d3d12x_vk_parity.exe'    = 'd3d12_xs R9G9B9E5 UAV fallback, CommandListX/QueueX, sret ABI, Vulkan sparse/mip & xg_xs metadata'
    'test_xgr_conformance_parity.exe'  = 'xgameruntime 292/292 parity, 3-arg COM QueryApiImpl, headless XStore/PLS & XTaskQueue/XAsync stress'
    'test_dstorage_audio_diag_parity.exe' = 'dstorage_xs cancel/phys-page, mfplat_xs & 4 console diag DLLs, XMem/XThread & WASAPI MMDevice'
    'test_loader_child_hook_parity.exe' = '24-DLL routing, hardened nested CoW G: VFS & Win32 parent->child APC hook propagation'
    'test_bcpack_bvh4.exe'             = 'Hardened BCPack BC1-BC7 entropy codecs & RDNA2 BVH4 header/node-pointer decoder + SAH equivalence'
    'test_real_gdk_thunks_live.exe'    = 'Live 292/292 xgameruntime + 16/16 XCurl + shipping commercial GDK thunks (zero E_NOINTERFACE)'
    'test_scarlett_game_live.exe'      = 'Full playable Scarlett game stack via openscarlett run (d3d12_xs+PresentX+GameInput+XAudio2+S:)'
    'test_d3d12_vk_routing.exe'        = 'Direct d3d12_xs->Vulkan 1.4 routing, BC1-BC7 staged copies, mixed-heap isolation & PresentX timing'
    'test_rdna2_lockstep_isa.exe'      = 'Single-wave lockstep LDS barriers, 5 RDNA2 no-ops & CPU-vs-GPU differential ISA checks'
    'test_s7_pe_gdk_runtime.exe'       = 'PE/KnownDLL/.xdata hardening, XMemVirtualQuery, Minecraft-GDK surface, 6 stubs, RawInput & DCC fuzz'
    'test_gdk_3d_scene_dual_api.exe'   = '3D shadowed/skinned/BC1+BC3 GDK scene launched via openscarlett run on D3D12 & Vulkan 1.4'
    'test_launcher_updater.exe'        = 'Native Win32/D2D OpenScarlettLauncher GUI smoke, icon, privacy sanitizer & BCrypt SHA-256 updater'
    'test_rdna2_isa_scalar_vector.exe' = 'GFX10.3 scalar & vector ISA (SOP/SMEM/VOP1-3P/SDWA/DPP16/DPP8) CPU-vs-live-GPU parity'
    'test_rdna2_isa_mem_img.exe'       = 'GFX10.3 MUBUF/MTBUF/MIMG/DS/FLAT/GLOBAL/SCRATCH/EXP ISA CPU-vs-live-GPU parity'
    'test_rdna2_corpus_bench.exe'      = 'GFX10.3 779-opcode ISA coverage auditor & 5-stage AAA shader corpus benchmark'
    'test_d3d12_xs_proxies_multiqueue.exe' = 'd3d12_xs 10 COM object proxies, Xbox extensions & 6-thread multi-queue FenceX sync'
    'test_unified_mem_bindless.exe'    = '64-bit TitleVAMirror (MEM_WRITE_WATCH dirty-page flush/readback) & RDNA2 V#/T#/S# bindless bridge'
    'test_xg_gpu_untile_and_stages.exe' = 'GPU compute GFX10 untiler/retiler, HS/DS/GS, DXR SBT, Wave64-on-32 & async PSO pool'
    'test_os_winrt_offline_auth.exe'   = 'Xbox 7-core CPU affinity remap, WinRT lifecycle & non-blocking offline auth/XSTS/XStore/XCurl'
    'test_media_video_gameinput.exe'   = 'MFMediaEngine video decode, 64-voice XAudio2 effects, Bink2 streaming & GameInput hot-plug'
    'test_probe_veh_capture.exe'       = 'First-Boot Probe E_NOTIMPL trampolines, Win32 VEH crash dumper & per-draw PNG/JSON capture'
    'test_gdkx_abi.exe'                = 'GDKX COM zero-escape (Device..Device12), IDXGIDevice1 virtual output, 5-arg WaitFrameEventX, 3-arg PresentX & appnotify PLM'
    'test_gdkx_untouched_samples.exe'  = '6 untouched Microsoft _GAMING_XBOX_SCARLETT samples (Triangle/Lighting/Texture/Instancing/Bezier/Compute) APC boot + 4K PNG check'
    'test_rdna2_gpu_accuracy.exe'      = 'GFX10.3 HW accuracy rules (v_sin/cos turns, s_bfe, fused fp16 FMA, D16/snorm) & 764/779 real-GPU opcode audit'
    'test_rdna2_as_task_spirv.exe'     = 'Amplification/Task SPIR-V 1.6 TaskEXT (130/130 spirv-val), Wave64-on-32 LDS & VS/GS/MS CullDistance'
    'test_d3d12_feedback_queries.exe'  = 'Console Sampler Feedback readback/clear, QueryAccumulatorX, predicated byte copies & GPU-QPC clock calibration'
    'test_xvd_loader_lsa.exe'          = '12 KiB XVD header geometry inspector, mixed-case PE forwarders/API-sets & Console LSA/PFN policy state'
    'test_s12_d3d12_aaa_wiring.exe'    = 'Sprint 12 live D3D12X island wiring (SamplerFeedback, QueryAccumulator, TitleVA, SGPR, Untile), GXDK extensions & PresentX pacing'
    'test_s12_xbox_shader_container.exe' = 'Sprint 12 Xbox DXBC/PM4/SGPR shader container parser, irreducible CFG/EXEC-stack lifter, VS->PS linkage & in-proc DXC CRC32 cache'
    'test_s12_rayquery_live.exe'       = 'Sprint 12 RDNA2 image_bvh*_intersect_ray -> DXR 1.1 RayQuery / SPV_KHR_ray_query, 64-bit multi-window BVH4 & SBT remapping'
    'test_s12_streaming_vram.exe'      = 'Sprint 12 DirectStorage priority/auto-submit/cancel/BCPack->untile, BC1-BC7 fuzz, HTILE/CMASK/DCC, page aliasing & LRU VRAM eviction'
    'test_s12_os_loader_gameinput.exe' = 'Sprint 12 live PeForwarder/LSA/probe CLI, TLS slot 0, NtCreateFile nested VFS, GameInput user-before-connect & 128-voice XAudio2'
    'test_s13_live_xbox_shader_e2e.exe' = 'Sprint 13 live Xbox DXBC/PM4/SGPR + RayQuery E2E through DeviceX (VS/PS/CS/Mesh no-RTS0), unified texture sync & title HWND PresentX'
    'test_s13_vk_xg_tiled.exe'         = 'Sprint 13 Vulkan OPTIONS2/7 & VK_EXT_custom_border_color, placed tiled textures in CommandListX/XG untiler & stencil/HTILE parity'
    'test_s13_shader_cfg_safety.exe'   = 'Sprint 13 RDNA2 MIMG R128/TFE/LWE refusal, poisoned mutex recovery, CFG canonicalization, V# slot-0/LDS & 764/779 GPU opcodes'
    'test_s13_gdk_offline_ntdll.exe'   = 'Sprint 13 offline-first XSTS default (OPENSCARLETT_MOCK_XSTS=1 opt-in), XTaskQueue waiter re-arm/drain, dialog/XError & UMS/wldp shims'
}

Write-Host ''
Write-Host '======================================================================' -ForegroundColor Cyan
Write-Host '  OpenScarlett - Automatic Hardware Compatibility & Self-Test Suite'    -ForegroundColor Cyan
Write-Host '======================================================================' -ForegroundColor Cyan
Write-Host '  Testing D3D12, Vulkan 1.4 Graphics/Mesh, HDR10, DXR 1.1 & GDK Shims' -ForegroundColor Gray
Write-Host '  Privacy: No personal paths or usernames are saved. Nothing is sent'   -ForegroundColor Gray
Write-Host '           until you explicitly confirm at the end.'                    -ForegroundColor Gray
Write-Host ''

$OsVersion = Get-FriendlyOsVersion
Write-Host "  OS Detected : $OsVersion" -ForegroundColor White

$VcRuntimeInfo = 'not found'
$sysMsvcp = Join-Path $env:SystemRoot 'System32\msvcp140.dll'
if (Test-Path $sysMsvcp) {
    $sysMsvcpVer = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($sysMsvcp).FileVersion
    $VcRuntimeInfo = "System32 msvcp140.dll $sysMsvcpVer"
    Write-Host "  VC++ Runtime: $VcRuntimeInfo" -ForegroundColor Gray
} else {
    Write-Host '  VC++ Runtime: System32\msvcp140.dll not found (install Microsoft VC++ 2015-2022 x64 Redistributable)' -ForegroundColor Yellow
}

$localMsvcp = Join-Path $BundleDir 'msvcp140.dll'
if (Test-Path $localMsvcp) {
    $localMsvcpVer = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($localMsvcp).FileVersion
    $VcRuntimeInfo += " | WARNING: local bundle msvcp140.dll $localMsvcpVer"
    Write-Host "  [WARN] Local msvcp140.dll ($localMsvcpVer) found in bundle directory!" -ForegroundColor Red
    Write-Host '         Stale CRT DLLs next to OpenScarlett EXEs can shadow System32 and cause crashes.' -ForegroundColor Yellow
    Write-Host '         Please extract OpenScarlett into a clean, empty folder.' -ForegroundColor Yellow
}

Push-Location $BundleDir
try {
    # ------------------------------------------------------------------
    # Step 1: Hardware & Toolchain Probe
    # ------------------------------------------------------------------
    Write-Host ''
    Write-Host '[Step 1/2] Probing Host GPUs (D3D12, DXR, MeshTier, Wave, Vulkan, DXC)...' -ForegroundColor Yellow

    $probeExe = Join-Path $BundleDir 'openscarlett.exe'
    $swProbe = [System.Diagnostics.Stopwatch]::StartNew()
    $probeLines = @(& $probeExe probe-gpu 2>&1 | ForEach-Object { $_.ToString() })
    $probeRc = $LASTEXITCODE
    $swProbe.Stop()
    $probeRaw = ($probeLines -join "`n").TrimEnd()
    $probeClean = Sanitize-Text $probeRaw

    # Parse primary hardware GPU + Vulkan info for clean display & GitHub issue title
    $hwAdapters = @()
    $vulkanLine = ''
    foreach ($line in $probeLines) {
        if ($line -match '^D3D12\s+\[\d+\]\s*:\s*(.+?)\s+\(Hardware\)\s*\|\s*(.+)$') {
            $hwAdapters += [PSCustomObject]@{
                Name    = $Matches[1].Trim()
                Details = $Matches[2].Trim()
                Full    = "$($Matches[1].Trim()) ($($Matches[2].Trim()))"
            }
        } elseif ($line -match '^Vulkan\s*:\s*(.+)$') {
            $vulkanLine = $Matches[1].Trim()
        }
    }

    # Prefer discrete GPU (highest VRAM) if multiple hardware adapters exist (e.g. Intel iGPU + NVIDIA/AMD dGPU)
    $primaryGpu = $null
    $bestVram = -1L
    foreach ($a in $hwAdapters) {
        $vram = 0L
        if ($a.Details -match 'VRAM=(\d+)\s*MB') { $vram = [int64]$Matches[1] }
        if ($vram -gt $bestVram) {
            $bestVram = $vram
            $primaryGpu = $a
        }
    }
    $primaryGpuShort = if ($primaryGpu) { $primaryGpu.Name } else { 'D3D12 WARP / No Discrete GPU' }
    $gpuSummaryText  = if ($primaryGpu) {
        $s = $primaryGpu.Full
        if ($vulkanLine) { $s += " | Vulkan: $vulkanLine" }
        $s
    } elseif ($vulkanLine) {
        "WARP | Vulkan: $vulkanLine"
    } else {
        'Unknown GPU'
    }

    foreach ($line in $probeLines) {
        if ($line -match '^(D3D12|Vulkan|DXC)') {
            Write-Host "    $line" -ForegroundColor Gray
        }
    }
    if ($probeRc -eq 0) {
        Write-Host ("  -> Hardware probe OK ({0:F2}s)" -f $swProbe.Elapsed.TotalSeconds) -ForegroundColor Green
    } else {
        Write-Host ("  -> Hardware probe FAILED (exit {0})" -f $probeRc) -ForegroundColor Red
    }

    # ------------------------------------------------------------------
    # Step 2: Run All Automated Test Binaries
    # ------------------------------------------------------------------
    $testFiles = @(Get-ChildItem -Path $BundleDir -Filter 'test_*.exe' |
        Where-Object { $_.Name -ine 'test_guest_dummy.exe' } |
        Sort-Object Name)

    $results = New-Object System.Collections.Generic.List[object]
    $results.Add([PSCustomObject]@{
        Name        = 'openscarlett.exe probe-gpu'
        Description = 'Host D3D12 / DXR / MeshTier / Vulkan / DXC capability probe'
        Passed      = ($probeRc -eq 0)
        ExitCode    = $probeRc
        Seconds     = [math]::Round($swProbe.Elapsed.TotalSeconds, 2)
        Highlight   = $primaryGpuShort
        Output      = $probeClean
    })

    Write-Host ''
    Write-Host ("[Step 2/2] Running {0} live hardware & runtime tests..." -f $testFiles.Count) -ForegroundColor Yellow
    Write-Host ''

    $idx = 0
    foreach ($tf in $testFiles) {
        $idx++
        $tname = $tf.Name
        $desc = if ($TestDescriptions.ContainsKey($tname)) { $TestDescriptions[$tname] } else { 'OpenScarlett hardware/runtime test' }

        Write-Host ("  [{0,2}/{1,2}] {2} - {3}" -f $idx, $testFiles.Count, $tname, $desc) -ForegroundColor White

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $outLines = @(& $tf.FullName 2>&1 | ForEach-Object { $_.ToString() })
        $rc = $LASTEXITCODE
        $sw.Stop()

        $cleanOut = Sanitize-Text (($outLines -join "`n").TrimEnd())

        # Pick the most informative last [PASS] or [FAIL] line to show inline
        $highlight = ''
        $passFailLines = @($outLines | Where-Object { $_ -match '\[(PASS|FAIL)\]' })
        if ($passFailLines.Count -gt 0) {
            $highlight = Sanitize-Text $passFailLines[-1].Trim()
        } elseif ($outLines.Count -gt 0) {
            $highlight = Sanitize-Text $outLines[-1].Trim()
        }
        if ($highlight.Length -gt 110) {
            $highlight = $highlight.Substring(0, 107) + '...'
        }

        if ($rc -eq 0) {
            Write-Host ("          PASS ({0:F2}s)  {1}" -f $sw.Elapsed.TotalSeconds, $highlight) -ForegroundColor Green
        } else {
            Write-Host ("          FAIL (exit {0}, {1:F2}s)  {2}" -f $rc, $sw.Elapsed.TotalSeconds, $highlight) -ForegroundColor Red
        }

        $results.Add([PSCustomObject]@{
            Name        = $tname
            Description = $desc
            Passed      = ($rc -eq 0)
            ExitCode    = $rc
            Seconds     = [math]::Round($sw.Elapsed.TotalSeconds, 2)
            Highlight   = $highlight
            Output      = $cleanOut
        })
    }

    $totalCount  = $results.Count
    $passedCount = @($results | Where-Object { $_.Passed }).Count
    $failedCount = $totalCount - $passedCount
    $statusLabel = if ($failedCount -eq 0) { 'PASS' } else { 'FAIL' }

    # ------------------------------------------------------------------
    # Build Clean GitHub-Ready Markdown Report
    # ------------------------------------------------------------------
    $tableLines = New-Object System.Collections.Generic.List[string]
    $tableLines.Add('| Check | Result | Time | Notes |')
    $tableLines.Add('| :--- | :---: | :---: | :--- |')
    foreach ($r in $results) {
        $badge = if ($r.Passed) { 'PASS' } else { "FAIL (exit $($r.ExitCode))" }
        $note = ($r.Highlight -replace '\|', '\|')
        $tableLines.Add("| ``$($r.Name)`` | **$badge** | $($r.Seconds)s | $note |")
    }
    $summaryTableMd = ($tableLines -join "`n")

    $fullLogSections = New-Object System.Collections.Generic.List[string]
    foreach ($r in $results) {
        $badge = if ($r.Passed) { 'PASS' } else { "FAIL (exit $($r.ExitCode))" }
        $fullLogSections.Add("=== $($r.Name) [$badge, $($r.Seconds)s] ===")
        if (-not [string]::IsNullOrWhiteSpace($r.Output)) {
            $fullLogSections.Add($r.Output)
        }
        $fullLogSections.Add('')
    }
    $fullLogText = ($fullLogSections -join "`n").TrimEnd()

    $reportDoc = @"
# OpenScarlett Hardware Test Report

- **Result:** **$passedCount/$totalCount passed** ($failedCount failed)
- **OS:** $OsVersion
- **VC++ Runtime:** $VcRuntimeInfo
- **Primary GPU:** $gpuSummaryText
- **UTC Timestamp:** $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC

## Test Summary

$summaryTableMd

## Host GPU Probe

``````text
$probeClean
``````

<details>
<summary>Full Automated Test Output</summary>

``````text
$fullLogText
``````

</details>
"@

    [System.IO.File]::WriteAllText($ReportMd, $reportDoc, (New-Object System.Text.UTF8Encoding($false)))

    # ------------------------------------------------------------------
    # Final Console Summary
    # ------------------------------------------------------------------
    Write-Host ''
    Write-Host '======================================================================' -ForegroundColor Cyan
    if ($failedCount -eq 0) {
        Write-Host ("  ALL TESTS PASSED: {0}/{1} checks passed on {2}" -f $passedCount, $totalCount, $primaryGpuShort) -ForegroundColor Green
    } else {
        Write-Host ("  COMPLETED WITH FAILURES: {0}/{1} passed, {2} failed on {3}" -f $passedCount, $totalCount, $failedCount, $primaryGpuShort) -ForegroundColor Red
        Write-Host '  (Failures on different GPUs / Windows 11 are super helpful bug reports!)' -ForegroundColor Yellow
    }
    Write-Host '======================================================================' -ForegroundColor Cyan
    Write-Host "  OS         : $OsVersion"
    Write-Host "  GPU        : $gpuSummaryText"
    Write-Host "  Report File: $ReportMd"
    Write-Host ''

    # ------------------------------------------------------------------
    # Optional 1-Click GitHub Issue Submission (Interactive Mode Only)
    # ------------------------------------------------------------------
    if (-not $NoSubmit) {
        $copied = $false
        try {
            Set-Clipboard -Value $reportDoc
            $copied = $true
            Write-Host '  [OK] Full privacy-safe report copied to your clipboard!' -ForegroundColor Green
        } catch {
            Write-Host '  [INFO] Report saved to hardware_test_report.md.' -ForegroundColor Gray
        }

        $issueTitle = "[HW Report] $primaryGpuShort | $OsVersion | $passedCount/$totalCount $statusLabel"
        $baseUrl = "https://github.com/$RepoOwner/$RepoName/issues/new"
        $paramsBase = @(
            "template=hardware-report.yml",
            "labels=hardware-report",
            "title=$([Uri]::EscapeDataString($issueTitle))",
            "os_version=$([Uri]::EscapeDataString($OsVersion))",
            "gpu_summary=$([Uri]::EscapeDataString($gpuSummaryText))",
            "test_summary=$([Uri]::EscapeDataString("**$passedCount/$totalCount passed** ($failedCount failed)`n`n$summaryTableMd"))"
        )

        # Keep URL safely under ~6200 chars so browsers & GitHub accept it with zero truncation errors.
        $compactLog = "=== Host Probe ===`n$probeClean`n`n=== Failed / Key Test Lines ===`n"
        foreach ($r in $results) {
            if ($r.Name -eq 'openscarlett.exe probe-gpu') { continue }
            $badge = if ($r.Passed) { 'PASS' } else { "FAIL (exit $($r.ExitCode))" }
            if (-not $r.Passed) {
                $compactLog += "[$badge] $($r.Name)`n$($r.Output)`n`n"
            } else {
                $compactLog += "[$badge] $($r.Name): $($r.Highlight)`n"
            }
        }
        $compactLog = $compactLog.TrimEnd()

        $slimTableLines = New-Object System.Collections.Generic.List[string]
        $slimTableLines.Add('| Check | Result | Time |')
        $slimTableLines.Add('| :--- | :---: | :---: |')
        foreach ($r in $results) {
            $badge = if ($r.Passed) { 'PASS' } else { "FAIL (exit $($r.ExitCode))" }
            $slimTableLines.Add("| ``$($r.Name)`` | **$badge** | $($r.Seconds)s |")
        }
        $slimTableMd = ($slimTableLines -join "`n")

        $paramsSlim = @(
            "template=hardware-report.yml",
            "labels=hardware-report",
            "title=$([Uri]::EscapeDataString($issueTitle))",
            "os_version=$([Uri]::EscapeDataString($OsVersion))",
            "gpu_summary=$([Uri]::EscapeDataString($gpuSummaryText))",
            "test_summary=$([Uri]::EscapeDataString("**$passedCount/$totalCount passed** ($failedCount failed)`n`n$slimTableMd"))"
        )
        $paramsMin = @(
            "template=hardware-report.yml",
            "labels=hardware-report",
            "title=$([Uri]::EscapeDataString($issueTitle))",
            "os_version=$([Uri]::EscapeDataString($OsVersion))",
            "gpu_summary=$([Uri]::EscapeDataString($gpuSummaryText))",
            "test_summary=$([Uri]::EscapeDataString("**$passedCount/$totalCount passed** ($failedCount failed)`n`n(Full summary table is in your clipboard.)"))"
        )

        $fullUrlCandidate = "$baseUrl?" + (($paramsBase + @("full_report=$([Uri]::EscapeDataString($fullLogText))")) -join '&')
        $compactUrlCandidate = "$baseUrl?" + (($paramsBase + @("full_report=$([Uri]::EscapeDataString($compactLog))")) -join '&')
        $slimCompactCandidate = "$baseUrl?" + (($paramsSlim + @("full_report=$([Uri]::EscapeDataString($compactLog))")) -join '&')

        $issueUrl = $null
        $urlMode = 'clipboard'

        if ($fullUrlCandidate.Length -le 6200) {
            $issueUrl = $fullUrlCandidate
            $urlMode = 'complete'
        } elseif ($compactUrlCandidate.Length -le 6200) {
            $issueUrl = $compactUrlCandidate
            $urlMode = 'compact'
        } elseif ($failedCount -gt 0) {
            # Embed failing test output directly in the URL (with slim summary table) so failures
            # are never lost even if the tester skips pressing Ctrl+V.
            $failLog = "=== Failed Tests ===`n"
            foreach ($r in $results) {
                if ($r.Passed) { continue }
                $failLog += "[FAIL (exit $($r.ExitCode))] $($r.Name)`n"
                if (-not [string]::IsNullOrWhiteSpace($r.Output)) {
                    $failLog += "$($r.Output)`n`n"
                } elseif (-not [string]::IsNullOrWhiteSpace($r.Highlight)) {
                    $failLog += "$($r.Highlight)`n`n"
                } else {
                    $failLog += "(no output captured)`n`n"
                }
            }
            $failLog += "(Full raw test log is also in your clipboard - press Ctrl+V to append passing logs.)"

            $truncSuffix = "`n... (truncated; full raw log is in your clipboard - press Ctrl+V to paste)"
            foreach ($pSet in @($paramsSlim, $paramsMin)) {
                $prefixUrl = "$baseUrl?" + ($pSet -join '&') + '&full_report='
                $exactFailUrl = $prefixUrl + [Uri]::EscapeDataString($failLog)
                if ($exactFailUrl.Length -le 6200) {
                    $issueUrl = $exactFailUrl
                    $urlMode = 'compact'
                    break
                }
                $encSuffix = [Uri]::EscapeDataString($truncSuffix)
                if (($prefixUrl.Length + $encSuffix.Length + 120) -le 6200) {
                    $lo = 0
                    $hi = $failLog.Length
                    while ($lo -lt $hi) {
                        $mid = [int][math]::Floor(($lo + $hi + 1) / 2)
                        $cand = $prefixUrl + [Uri]::EscapeDataString($failLog.Substring(0, $mid)) + $encSuffix
                        if ($cand.Length -le 6200) { $lo = $mid } else { $hi = $mid - 1 }
                    }
                    if ($lo -ge 40) {
                        $cut = $lo
                        $lastNl = $failLog.LastIndexOf("`n", $lo - 1)
                        if ($lastNl -ge 0 -and ($lastNl + 80) -ge $lo) { $cut = $lastNl }
                        $issueUrl = $prefixUrl + [Uri]::EscapeDataString($failLog.Substring(0, $cut)) + $encSuffix
                        $urlMode = 'compact'
                        break
                    }
                }
            }
        } elseif ($slimCompactCandidate.Length -le 6200) {
            $issueUrl = $slimCompactCandidate
            $urlMode = 'compact'
        }

        if ($null -eq $issueUrl) {
            $fallbackNote = "(Full raw test log is in your clipboard - press Ctrl+V here to paste it.)"
            $encFallback = "full_report=$([Uri]::EscapeDataString($fallbackNote))"
            $issueUrl = "$baseUrl?" + (($paramsBase + @($encFallback)) -join '&')
            if ($issueUrl.Length -gt 6200) {
                $issueUrl = "$baseUrl?" + (($paramsSlim + @($encFallback)) -join '&')
            }
            if ($issueUrl.Length -gt 6200) {
                $issueUrl = "$baseUrl?" + (($paramsMin + @($encFallback)) -join '&')
            }
            $urlMode = 'clipboard'
        }

        Write-Host ''
        $ans = Read-Host '  Open GitHub in your browser with this report pre-filled to submit? [Y/n]'
        if ([string]::IsNullOrWhiteSpace($ans) -or $ans.Trim().ToLowerInvariant().StartsWith('y')) {
            if ($urlMode -ne 'complete') {
                # Summary table is already pre-filled in the Test Summary field; place ONLY the raw
                # log on the clipboard so pressing Ctrl+V in Probe & Test Output avoids duplication.
                try { Set-Clipboard -Value $fullLogText } catch {}
            }
            try {
                Start-Process $issueUrl
                Write-Host ''
                Write-Host '  -> Opened pre-filled GitHub Issue in your browser!' -ForegroundColor Green
                if ($urlMode -eq 'complete') {
                    Write-Host '     Everything is already filled in - just review and click "Create".' -ForegroundColor Cyan
                } else {
                    Write-Host '     Summary is already pre-filled; optionally press Ctrl+V in "Probe & Test Output" to paste the full raw log.' -ForegroundColor Cyan
                }
            } catch {
                Write-Host "  Could not open browser automatically. Please submit hardware_test_report.md at:" -ForegroundColor Yellow
                Write-Host "  https://github.com/$RepoOwner/$RepoName/issues/new/choose" -ForegroundColor Cyan
            }
        } else {
            Write-Host ''
            Write-Host "  No problem! Whenever you want to share it, open:" -ForegroundColor Gray
            Write-Host "  https://github.com/$RepoOwner/$RepoName/issues/new?template=hardware-report.yml" -ForegroundColor Cyan
            if ($copied) {
                Write-Host '  (The full Markdown report is already on your clipboard and saved in hardware_test_report.md.)' -ForegroundColor Gray
            }
        }
    }

    if (-not $NoPause) {
        Write-Host ''
        Write-Host 'Press Enter to close this window...' -ForegroundColor Gray
        [void](Read-Host)
    }

    if ($failedCount -gt 0) { exit 1 }
    exit 0
} finally {
    Pop-Location
}
