# System DLL Shims Reference (All 30 DLLs)

OpenScarlett ships **30 Xbox / GDK Windows PE shim DLLs** totaling **993 live PE exports** (measured directly from `build/*.dll` PE export directories via `tools/generate_status.py`).

Each DLL is honestly classified into one of three implementation tiers:
- **High (12 DLLs):** Substantial functional implementation verified on real binaries and/or live GPUs.
- **Partial (8 DLLs):** Core paths implemented and tested; some commercial-game edge cases remain.
- **Stub (10 DLLs):** Clean telemetry-logged / fixed-return shims for boot & diagnostic compatibility.

---

## Complete 30-DLL Catalog

| DLL Name | Tier | Live PE Exports | Implemented Scope & Notes |
| :--- | :---: | :-: | :--- |
| **`xgameruntime.dll`** | 🟢 High | **345** | **292/292** shipping `XGamingRuntimeThunks.dll` parity, **24 COM `QueryApiImpl` interfaces** (`0 E_NOINTERFACE` on live *Alexandria IV DEMO* thunks), `XUser`, `XStore` offline licensing, `XGameSave` (`S:\`), exact `XTaskQueue` port delivery, `XSystem`, `XDisplay`, `XGameUI`, `XGameStreaming`, `XPackage` |
| **`Microsoft.Xbox.Services.GDK.C.dll`** | 🟢 High | **186** | XSAPI offline user profile, achievements, stats, social, matchmaking, and telemetry shims |
| **`kernelx.dll`** | 🟢 High | **73** | Win10/11 `VirtualAlloc2` / `MapViewOfFile3` 64 KiB & 4 MiB physical page aliasing, `GpuTilePool`, `XMem`/`XThread`, 7-core / 14-thread console CPU affinity remapping |
| **`d3d12_xs.dll`** | 🟢 High | **48** | All **13 Xbox COM proxies**, 6-thread multi-queue (`Direct`/`Compute`/`Copy`) sync, GXDK extensions, Sampler Feedback, `QueryAccumulatorX`, HDR10 `PresentX`, auto-lift PSOs, and direct Vulkan 1.4 routing |
| **`XCurl.dll`** | 🟢 High | **46** | **16/16** real commercial thunk imports, WinHTTP handle lifecycle, and non-blocking offline XSTS/JWT HTTP 200 synthesis |
| **`xg_xs.dll`** | 🟢 High | **25** | 15 2D + 6 3D GFX10 tile modes, `HTILE`/`DCC`/`CMASK` metadata, `XGEncodeDCC` (1,000-case fuzzed), and real-time GPU compute untiling (`cs_6_5` + SPIR-V) |
| **`winrt_xs.dll`** | 🟢 High | **21** | `RoInitialize`, `HSTRING`, `CoreApplication` / `CoreWindow` / `ApplicationView` / `DisplayInformation` lifecycle, and `IReferenceTracker` `E_NOINTERFACE` refusal |
| **`XAudio2_9.dll`** | 🟢 High | **19** | Lock-free **128-voice** mixer, live WASAPI + null CI sink, submix buses, Schroeder Reverb/VolumeMeter APOs, XMA2/Opus/ADPCM, and `X3DAudio` Doppler |
| **`GameInput.dll`** | 🟢 High | **18** | XInput gamepads, keyboard, lossless Win32 Raw Input mouse deltas, hot-plug device callbacks, and LT/RT impulse-trigger rumble |
| **`mfplat_xs.dll`** | 🟢 High | **17** | `IMFDXGIDeviceManager` + `IMFMediaEngine` H.264/HEVC D3D12 Media Foundation video decode (`TransferVideoFrame` + headless fallback) |
| **`xmem.dll`** | 🟢 High | **15** | `XMemAlloc`/`XMemFree`, `XMemVirtualQuery`, and ERA allocation attribute preservation |
| **`dstorage_xs.dll`** | 🟢 High | **9** | Priority queues (`REALTIME`→`LOW`), request cancellation, custom decompression callbacks, GDeflate v1 + BCPack BC1–BC7, and automatic GPU untile texture upload |
| **`libHttpClient.GDK.dll`** | 🟡 Partial | **21** | Offline/HTTP request queue and `XAsync` lifecycle |
| **`advapi32legacy.dll`** | 🟡 Partial | **14** | UE4/UE5 console startup, login, and registry routing shims |
| **`xcrdapi.dll`** | 🟡 Partial | **13** | Console DirectStorage / container staging and cancellation |
| **`Bink2Scarlett.dll`** | 🟡 Partial | **12** | KB2i DCT multi-frame decode and C ABI exports *(full retail bitstream variants pending)* |
| **`XboxBvhBuilder.dll`** | 🟡 Partial | **9** | SAH BVH2 → BVH4 builder and 64-bit RDNA2 BVH4 decoder |
| **`amd_fidelityfx_upscaler_xbox.dll`** | 🟡 Partial | **6** | Live GPU **FSR 1.0** (`EASU` + `RCAS`, `0 LSB` parity) *(FSR 2/3 temporal pending)* |
| **`amd_fidelityfx_loader_xbox.dll`** | 🟡 Partial | **5** | FidelityFX versioned context loader and dispatch |
| **`AcpHal.dll`** | 🟡 Partial | **3** | SHAPE audio DSP queue forwarding to software XMA2 decoder |
| **`pixevt.dll`** | ⚪ Stub | **13** | PIX begin/end event & marker telemetry stubs |
| **`hix_xs.dll`** | ⚪ Stub | **11** | Hang-dump & debug telemetry forward stubs |
| **`KernelTraceControl.dll`** | ⚪ Stub | **9** | ETW kernel trace control fixed-return stubs |
| **`xtfpix.dll`** | ⚪ Stub | **9** | XTF PIX capture diagnostic stubs |
| **`xbtp.dll`** | ⚪ Stub | **8** | Debug transport session stubs |
| **`mflnmhelp.dll`** | ⚪ Stub | **8** | Media Foundation helper fixed-return stubs |
| **`mfhevcdec.dll`** | ⚪ Stub | **8** | HEVC decoder registration stubs |
| **`gamecoreclient.dll`** | ⚪ Stub | **8** | GameCore client broker stubs |
| **`minuserx.dll`** | ⚪ Stub | **7** | Console minimal `user32` forward stubs |
| **`xtfdebugmonitor.dll`** | ⚪ Stub | **7** | XTF debug monitor stubs |
| **TOTAL (30 DLLs)** | **12H / 8P / 10S** | **993** | *Measured from `build/*.dll` via `tools/generate_status.py`* |

---

## Key Runtime Highlights

### `xgameruntime.dll` & Shipping `XGamingRuntimeThunks.dll` Parity
Commercial GDK games typically link against Microsoft's static **`XGamingRuntimeThunks.dll`**, which dynamically resolves 24 internal COM interfaces via 3-argument `QueryApiImpl` calls into `xgameruntime.dll`.
- OpenScarlett implements **`292 / 292`** shipping thunk exports and all **24 COM `QueryApiImpl` interfaces**, verified live against the real shipping *Alexandria IV DEMO* thunks (`test_real_gdk_thunks_live`: **110 calls, `0 E_NOINTERFACE`**).
- **Non-Blocking Offline Auth & Licensing:** Synthesizes valid offline XSTS/JWT tokens, local gamertag/XUID profiles (`OPENSCARLETT_GAMERTAG`, `OPENSCARLETT_XUID`), and `XStore` entitlement checks (`OPENSCARLETT_STORE_OWNED_PACKAGES`, `OPENSCARLETT_STORE_ADDON_IDS`) so titles never hang waiting on Xbox Live sign-in prompts.
- **Exact `XTaskQueue` Port Delivery:** Supports manual, thread-pool, serialized thread-pool, and immediate work/completion port dispatch modes (`test_xgr_conformance_parity`, `test_s7_pe_gdk_runtime`).

### `winrt_xs.dll` (Console WinRT Lifecycle)
- Provides `RoInitialize`, `HSTRING` allocation/refcounting, and `RoGetActivationFactory` for `CoreApplication`, `CoreWindow`, `ApplicationView`, and `DisplayInformation`.
- Intentionally returns `E_NOINTERFACE` for `IReferenceTracker` on host WinRT objects to match console GC/interop expectations (`test_os_winrt_offline_auth`).

---

## Related Pages
- [Loader & Virtual Filesystem](Loader-and-VFS.md)
- [Graphics: DirectX 12 (`d3d12_xs`) & Vulkan 1.4](Graphics-D3D12-and-Vulkan.md)
- [CLI, Environment Variables & Diagnostics](CLI-Environment-and-Diagnostics.md)
