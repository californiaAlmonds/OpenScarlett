#pragma once

// OpenScarlett MIT Playable Xbox Series X|S (Scarlett) Game Platform Host.
// Pure standalone Scarlett ABI integration:
//   - d3d12_xs.dll    : D3D12CreateDeviceX + GPU upload/SRV/RTV textured fullscreen quad + PresentX
//   - kernelx.dll     : XMemAlloc / XMemFree unified staging framebuffer + GetConsoleType + TitleMemoryStatus
//   - xgameruntime.dll: XGameRuntimeInitialize + XUserAddAsync + XTaskQueue + XDisplay + XGameSave (S:\)
//   - GameInput.dll   : Live keyboard & Xbox gamepad polling + deterministic test input injection
//   - XAudio2_9.dll   : Live WASAPI streaming SFX/tone playback via IXAudio2SourceVoice

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScarlettPlatformConfig {
    uint32_t fb_width;           // e.g. 640
    uint32_t fb_height;          // e.g. 400
    uint32_t window_width;       // e.g. 960 (interactive window client width)
    uint32_t window_height;      // e.g. 600 (interactive window client height)
    int prefer_warp;             // 1 = D3D12 WARP, 0 = Hardware GPU
    int headless;                // 1 = hidden window + uncapped frame pacing for automated tests
    int verify_ticks;            // > 0 enables automated gameplay verification mode
    const char* window_title;    // UTF-8 initial window title
    const char* save_container;  // XGameSave container name (e.g. "scarlett_doom")
} ScarlettPlatformConfig;

typedef struct ScarlettPlatformStats {
    uint64_t frames_presented;
    uint64_t unique_frame_hashes;
    uint64_t last_frame_hash;
    uint32_t last_non_black_pixels;
    uint32_t last_unique_colors;
    uint64_t audio_buffers_submitted;
    uint64_t input_events_queued;
    uint32_t console_type;
    uint32_t title_id;
    int save_roundtrip_ok;
    char adapter_name[128];
    char gamertag[64];
} ScarlettPlatformStats;

// Abstract key codes compatible with DOOM (doomkeys.h) and ScarlettArena.
enum ScarlettKeyCode {
    SCARLETT_KEY_NONE = 0,
    SCARLETT_KEY_TAB = 9,
    SCARLETT_KEY_ENTER = 13,
    SCARLETT_KEY_ESCAPE = 27,
    SCARLETT_KEY_STRAFE_L = 0xA0,
    SCARLETT_KEY_STRAFE_R = 0xA1,
    SCARLETT_KEY_USE = 0xA2,
    SCARLETT_KEY_FIRE = 0xA3,
    SCARLETT_KEY_LEFT = 0xAC,
    SCARLETT_KEY_UP = 0xAD,
    SCARLETT_KEY_RIGHT = 0xAE,
    SCARLETT_KEY_DOWN = 0xAF,
    SCARLETT_KEY_RSHIFT = 0x80 + 0x36,
    SCARLETT_KEY_RCTRL = 0x80 + 0x1D,
    SCARLETT_KEY_RALT = 0x80 + 0x38,
    SCARLETT_KEY_F1 = 0x80 + 0x3B,
    SCARLETT_KEY_F2 = 0x80 + 0x3C,
    SCARLETT_KEY_F3 = 0x80 + 0x3D,
    SCARLETT_KEY_F4 = 0x80 + 0x3E,
    SCARLETT_KEY_F5 = 0x80 + 0x3F,
    SCARLETT_KEY_F6 = 0x80 + 0x40,
};

// Initializes the full Xbox Series X|S (Scarlett) platform stack.
// Returns 1 on success, 0 on failure.
int ScarlettPlatformInit(const ScarlettPlatformConfig* config);

// Shuts down all Scarlett subsystems cleanly.
void ScarlettPlatformShutdown(void);

// Returns 1 if the user requested window close (WM_CLOSE / WM_QUIT).
int ScarlettPlatformShouldQuit(void);

// Updates the interactive Win32 window title.
void ScarlettPlatformSetWindowTitle(const char* title_utf8);

// Returns monotonic milliseconds. In verify_ticks mode, uses deterministic virtual clock.
uint32_t ScarlettPlatformGetTicksMs(void);

// Sleeps cleanly (interactive) or advances minimally (verify_ticks mode).
void ScarlettPlatformSleepMs(uint32_t ms);

// Pumps Win32 messages, polls GameInput (keyboard + gamepad), and dispatches XTaskQueue.
void ScarlettPlatformPumpInput(void);

// Pops the next key event (pressed=1 down, 0 up; key=ScarlettKeyCode or ASCII).
// Returns 1 if an event was popped, 0 if queue is empty.
int ScarlettPlatformPopKeyEvent(int* out_pressed, unsigned char* out_key);

// Queries current live held state of a ScarlettKeyCode / ASCII key (1 = held, 0 = up).
int ScarlettPlatformIsKeyHeld(unsigned char key);

// Injects deterministic simulated gameplay input via GameInput for automated verification ticks.
void ScarlettPlatformInjectVerifyInput(uint32_t tick_index);

// Uploads a 32-bit XRGB8888 (0x00RRGGBB, e.g. doomgeneric DG_ScreenBuffer) or RGBA8888
// framebuffer to the GPU texture, runs the D3D12 fullscreen quad shader pass, and calls PresentX.
// Set is_xrgb_00rrggbb = 1 for doomgeneric (0x00RRGGBB), or 0 for native RGBA8888 bytes.
int ScarlettPlatformPresentFrame(const uint32_t* pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 int is_xrgb_00rrggbb);

// Submits a synthesized PCM16 tone/SFX burst to XAudio2_9.dll (WASAPI / null-audio safe).
void ScarlettPlatformPlaySfx(float freq_hz, uint32_t duration_ms, float gain);

// Writes a persistent save blob via xgameruntime.dll XGameSave to S:\ and verifies round-trip read.
// Returns 1 on success, 0 on failure.
int ScarlettPlatformSaveAndVerify(const char* blob_name, const void* data, size_t byte_count);

// Writes the last presented frame as a P6 PPM image (e.g. to T:\frame.ppm). Returns 1 on success.
int ScarlettPlatformWriteLastFramePpm(const char* path_utf8);

// Writes key=value verification metrics to disk (e.g. T:\metrics.txt). Returns 1 on success.
int ScarlettPlatformWriteMetricsFile(const char* path_utf8, const char* extra_kv_lines);

// Copies current runtime statistics snapshot.
void ScarlettPlatformGetStats(ScarlettPlatformStats* out_stats);

// Verifies a read-only asset via hooked Win32 CreateFileW on G:\<rel_name> and writes
// a UCRT-accessible path into out_path. Returns 1 if found, 0 otherwise.
int ScarlettPlatformResolveGameAsset(const char* rel_name, char* out_path, size_t out_size);

// Resolves a scratch file path inside T:\ (OPENSCARLETT_VFS_T) for UCRT fopen().
int ScarlettPlatformResolveScratchPath(const char* rel_name, char* out_path, size_t out_size);

// Switches process working directory to persistent S:\ (OPENSCARLETT_VFS_S) so legacy C-stdio
// engines (e.g. doomgeneric) never create save/config folders inside read-only G:\!
int ScarlettPlatformChdirToSaveDir(void);

#ifdef __cplusplus
}
#endif
