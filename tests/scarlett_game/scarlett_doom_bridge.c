/*
 * OpenScarlett MIT DOOM (doomgeneric) -> Xbox Series X|S (Scarlett) Platform Bridge.
 *
 * Implements the 6 doomgeneric platform callbacks (DG_Init, DG_DrawFrame, DG_SleepMs,
 * DG_GetTicksMs, DG_GetKey, DG_SetWindowTitle) on top of scarlett_platform.h
 * (d3d12_xs.dll + PresentX + kernelx.dll + xgameruntime.dll + GameInput.dll + XAudio2_9.dll).
 *
 * Supports:
 *   - Interactive 60 FPS live play with keyboard (WASD/Arrows/Ctrl/Space/E/Shift/Esc/Enter)
 *     and Xbox Gamepad (GameInput.dll) + Alt+Enter borderless fullscreen.
 *   - Automated verification mode:
 *       --verify-ticks N    Run N DOOM ticks with simulated gameplay input, write S:\ save + T:\ metrics
 *       --warp              Use D3D12 WARP adapter (single-dash -warp E M still passes to DOOM!)
 *       --headless          Hide Win32 window for non-intrusive automated verification
 */

#include "doomgeneric.h"
#include "scarlett_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_use_warp = 0;
static int g_headless = 0;
static int g_verify_ticks = 0;
static uint32_t g_frame_counter = 0;
static const char* g_metrics_out = "T:\\scarlett_doom_metrics.txt";
static const char* g_ppm_out = "T:\\scarlett_doom_frame.ppm";

void DG_Init(void) {
    ScarlettPlatformConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fb_width = DOOMGENERIC_RESX;
    cfg.fb_height = DOOMGENERIC_RESY;
    cfg.window_width = 960u;
    cfg.window_height = 600u;
    cfg.prefer_warp = g_use_warp;
    cfg.headless = g_headless || (g_verify_ticks > 0);
    cfg.verify_ticks = g_verify_ticks;
    cfg.window_title = "OpenScarlett - Freedoom (Xbox Series X|S)";
    cfg.save_container = "scarlett_doom";

    if (!ScarlettPlatformInit(&cfg)) {
        fprintf(stderr, "[ScarlettDoom] FATAL: ScarlettPlatformInit failed!\n");
        exit(2);
    }
}

void DG_DrawFrame(void) {
    ScarlettPlatformPumpInput();
    if (!ScarlettPlatformPresentFrame(
            (const uint32_t*)DG_ScreenBuffer,
            DOOMGENERIC_RESX,
            DOOMGENERIC_RESY,
            1 /* XRGB 0x00RRGGBB */)) {
        fprintf(stderr, "[ScarlettDoom] WARN: ScarlettPlatformPresentFrame failed\n");
    }
    g_frame_counter += 1u;
    if (g_verify_ticks > 0 && (g_frame_counter % 15u) == 1u) {
        ScarlettPlatformPlaySfx(220.0f + (float)(g_frame_counter % 5u) * 55.0f, 35u, 0.12f);
    }
}

void DG_SleepMs(uint32_t ms) {
    ScarlettPlatformSleepMs(ms);
}

uint32_t DG_GetTicksMs(void) {
    return ScarlettPlatformGetTicksMs();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    return ScarlettPlatformPopKeyEvent(pressed, key);
}

void DG_SetWindowTitle(const char* title) {
    if (title && title[0] != '\0') {
        char buf[256];
        snprintf(buf, sizeof(buf), "OpenScarlett [Xbox Series X|S] - %s", title);
        ScarlettPlatformSetWindowTitle(buf);
    }
}

int main(int argc, char** argv) {
    char* doom_argv[128];
    int doom_argc = 0;
    int has_iwad = 0;
    int has_config = 0;
    int has_extraconfig = 0;
    int has_doom_warp = 0;
    int i;

    doom_argv[doom_argc++] = (argc > 0 && argv[0]) ? argv[0] : "ScarlettDoom.exe";

    for (i = 1; i < argc && doom_argc < 110; ++i) {
        if (strcmp(argv[i], "--warp") == 0) {
            g_use_warp = 1;
        } else if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
        } else if (strcmp(argv[i], "--verify-ticks") == 0 && i + 1 < argc) {
            g_verify_ticks = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--metrics-out") == 0 && i + 1 < argc) {
            g_metrics_out = argv[++i];
        } else if (strcmp(argv[i], "--ppm-out") == 0 && i + 1 < argc) {
            g_ppm_out = argv[++i];
        } else {
            if (strcmp(argv[i], "-iwad") == 0) has_iwad = 1;
            if (strcmp(argv[i], "-config") == 0) has_config = 1;
            if (strcmp(argv[i], "-extraconfig") == 0) has_extraconfig = 1;
            if (strcmp(argv[i], "-warp") == 0) has_doom_warp = 1;
            doom_argv[doom_argc++] = argv[i];
        }
    }

    /* Resolve IWAD strictly through read-only G:\ VFS (never mutates G:\)! */
    static char s_iwad_path[512];
    static char s_cfg_path[512];
    static char s_extra_cfg_path[512];
    s_iwad_path[0] = '\0';
    s_cfg_path[0] = '\0';
    s_extra_cfg_path[0] = '\0';

    if (!has_iwad) {
        if (ScarlettPlatformResolveGameAsset("freedoom1.wad", s_iwad_path, sizeof(s_iwad_path))) {
            doom_argv[doom_argc++] = "-iwad";
            doom_argv[doom_argc++] = s_iwad_path;
        }
    }

    /* Switch CWD to persistent S:\ BEFORE DOOM initializes so .savegame/ lives in S:\ and
       NEVER touches the read-only G:\ game package directory! */
    (void)ScarlettPlatformChdirToSaveDir();

    /* Keep DOOM config files strictly in scratch T:\ so read-only G:\ is NEVER written! */
    if (!has_config &&
        ScarlettPlatformResolveScratchPath("doom_default.cfg", s_cfg_path, sizeof(s_cfg_path))) {
        doom_argv[doom_argc++] = "-config";
        doom_argv[doom_argc++] = s_cfg_path;
    }
    if (!has_extraconfig &&
        ScarlettPlatformResolveScratchPath("doom_extra.cfg", s_extra_cfg_path, sizeof(s_extra_cfg_path))) {
        doom_argv[doom_argc++] = "-extraconfig";
        doom_argv[doom_argc++] = s_extra_cfg_path;
    }

    /* In automated verification mode, warp directly into E1M1 live 3D gameplay! */
    if (g_verify_ticks > 0 && !has_doom_warp) {
        doom_argv[doom_argc++] = "-warp";
        doom_argv[doom_argc++] = "1";
        doom_argv[doom_argc++] = "1";
    }

    doom_argv[doom_argc] = NULL;

    doomgeneric_Create(doom_argc, doom_argv);

    if (g_verify_ticks > 0) {
        int tick;
        for (tick = 0; tick < g_verify_ticks && !ScarlettPlatformShouldQuit(); ++tick) {
            ScarlettPlatformInjectVerifyInput((uint32_t)tick);
            ScarlettPlatformPumpInput();
            doomgeneric_Tick();
        }

        /* Persist a Scarlett XGameSave checkpoint blob to S:\ and verify round-trip read! */
        {
            char save_payload[160];
            ScarlettPlatformStats st;
            ScarlettPlatformGetStats(&st);
            snprintf(save_payload, sizeof(save_payload),
                     "SCARLETT_DOOM_E1M1|frames=%llu|hashes=%llu|hash=0x%016llX",
                     (unsigned long long)st.frames_presented,
                     (unsigned long long)st.unique_frame_hashes,
                     (unsigned long long)st.last_frame_hash);
            if (!ScarlettPlatformSaveAndVerify("doom_checkpoint", save_payload, strlen(save_payload))) {
                fprintf(stderr, "[ScarlettDoom] FAIL: XGameSave S:\\ round-trip failed!\n");
                ScarlettPlatformShutdown();
                return 3;
            }
        }

        (void)ScarlettPlatformWriteLastFramePpm(g_ppm_out);
        {
            char extra[640];
            snprintf(extra, sizeof(extra), "game=Freedoom1\niwad=%s\n", s_iwad_path);
            (void)ScarlettPlatformWriteMetricsFile(g_metrics_out, extra);
        }

        ScarlettPlatformShutdown();
        return 0;
    }

    /* Interactive 60 FPS playable loop */
    while (!ScarlettPlatformShouldQuit()) {
        ScarlettPlatformPumpInput();
        doomgeneric_Tick();
    }

    /* Save session checkpoint to S:\ on clean interactive exit */
    {
        const char* session_blob = "SCARLETT_DOOM_INTERACTIVE_EXIT_OK";
        (void)ScarlettPlatformSaveAndVerify("doom_session", session_blob, strlen(session_blob));
    }
    ScarlettPlatformShutdown();
    return 0;
}
