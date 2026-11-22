# Audio, Video & Input

OpenScarlett implements native translations for Xbox Series X|S's audio engine (`XAudio2_9`, `AcpHal`, XMA2/Opus/ADPCM), hardware video playback (`mfplat_xs`, `Bink2Scarlett`), and unified controller/keyboard/mouse API (`GameInput`).

---

## 1. Audio Stack (`XAudio2_9.dll`, `AcpHal.dll`, XMA2 / Opus / ADPCM)

Source: `src/shims/xaudio2_shim.cpp`, `src/shims/acphal_shim.cpp`, `src/audio/xma2_decoder.cpp`, `src/audio/opus_adpcm_decoder.cpp`

### Lock-Free 128-Voice `XAudio2_9.dll` Engine
- **128+ Concurrent Source Voices:** Lock-free audio mixer upgraded in Sprint 12 (`test_s12_os_loader_gameinput`) with monotonic sample clocks (`SamplesPlayed`), looping buffers, pitch/frequency ratio resampling, and volume matrix routing.
- **Submix Buses & Audio Processing Objects (APOs):** Full submix graph, Schroeder Reverb APO (`XAudio2CreateReverb`), and VolumeMeter APO (`XAudio2CreateVolumeMeter`) via `XAudio2FX`.
- **3D Spatial Audio (`X3DAudio`):** `X3DAudioInitialize` and `X3DAudioCalculate` with live Doppler pitch shift verified on `SimpleLighting` (`1.066 → 0.942`).
- **WASAPI Live Output + Null Headless Mode:** Streams real-time mixed PCM audio to Windows WASAPI on desktop PCs, and cleanly switches to a deterministic null sink when `OPENSCARLETT_AUDIO_NULL=1` is set (used in headless CI runners).

### Console Audio Codecs & `AcpHal.dll`
| Component | Capabilities | Verified In |
| :--- | :--- | :--- |
| **`AcpHal.dll`** | Console Audio Co-Processor (SHAPE) DSP context/queue shim forwarding XMA2 packets directly to OpenScarlett's software decoder | `test_codecs`, `test_audio_opus_wasapi` |
| **XMA2 Decoder (`xma2_decoder.cpp`)** | IMDCT-based XMA2 packet/frame decoder | `test_codecs` |
| **Opus & ADPCM (`opus_adpcm_decoder.cpp`)** | Integrates Windows `MSOpusDecoder` Media Foundation Transform (MFT) for low-latency Opus playback + IMA/MS-ADPCM decoding | `test_audio_opus_wasapi`, `test_dstorage_audio_diag_parity` |

---

## 2. Video Playback (`mfplat_xs.dll`, `Bink2Scarlett.dll`)

Source: `src/shims/mfplat_shim.cpp`, `src/middleware/bink2_decoder.cpp`, `src/shims/bink2_scarlett_shim.cpp`

### `mfplat_xs.dll` (Media Foundation H.264 / HEVC Decode)
- Implements `IMFDXGIDeviceManager` (`MFResetDXGIDeviceManagerX`) and `IMFMediaEngine` lifecycle for console video cutscenes.
- Translates H.264 and HEVC decode pipelines to native Windows **D3D12 Media Foundation** with `TransferVideoFrame` texture output and a deterministic headless fallback for CI environments without hardware video codecs (`test_media_video_gameinput`).
- Companion registration shims (`mfhevcdec.dll`, `mflnmhelp.dll`) ensure Media Foundation decoder discovery never fails on boot.

### `Bink2Scarlett.dll`
- **KB2i** DCT multi-frame video streaming decoder and Xbox Bink2 C ABI exports (`test_codecs`, `test_media_video_gameinput`).
- *(Note: Full commercial retail Bink2 bitstream variants are still in progress.)*

---

## 3. Input Stack (`GameInput.dll`)

Source: [`include/openscarlett/input/gameinput_abi.h`](https://github.com/californiaAlmonds/OpenScarlett/blob/main/include/openscarlett/input/gameinput_abi.h), `src/shims/gameinput_shim.cpp`

Xbox Series X|S titles use Microsoft's modern **`IGameInput`** unified reading API instead of legacy XInput-only polling. OpenScarlett's `GameInput.dll` provides:

- **Unified Device Support:**
  - **Gamepads:** Native XInput controller polling mapped to `GameInputGamepadState` (all buttons, triggers, and thumbsticks)
  - **Keyboard:** Full Win32 virtual-key / scancode mapping to `GameInputKeyState`
  - **Mouse:** Lossless **Win32 Raw Input** relative delta accumulation (`WM_INPUT`) mapped to `GameInputMouseState` (`test_s7_pe_gdk_runtime`, `test_s12_os_loader_gameinput`)
- **Hot-Plug Device Callbacks:** `RegisterDeviceCallback` / `UnregisterCallback` supporting dynamic controller connect/disconnect notifications with deterministic reading ring buffers (`test_media_video_gameinput`).
- **Haptics & Impulse Triggers:** `SetRumbleState` translates both main low/high-frequency motor rumble and Xbox One/Series **LT/RT impulse-trigger** feedback rings.
- **Verified Live Playability:** Drives interactive 60 FPS gameplay in *Freedoom: Phase 1* (`ScarlettDoom`), *ScarlettArena*, and camera-controlled `gdk_3d_scene`.

---

## Related Pages
- [System DLL Shims Reference (30 DLLs)](System-DLL-Shims-Reference.md)
- [Testing, Compatibility & Hardware](Testing-Compatibility-and-Hardware.md)
- [CLI, Environment Variables & Diagnostics](CLI-Environment-and-Diagnostics.md)
