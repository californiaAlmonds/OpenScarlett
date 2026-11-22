// OpenScarlett MIT Standalone Playable Xbox Series X|S (Scarlett) Game:
// "Scarlett Arena" — a crisp 640x400 Wolf-style textured 3D raycaster running on the
// exact same Scarlett full-stack platform host (d3d12_xs.dll + PresentX + kernelx.dll +
// xgameruntime.dll + GameInput.dll + XAudio2_9.dll + G:\ VFS + S:\ XGameSave).
//
// Zero external dependencies — builds and runs identically on GitHub Actions CI and local hosts!

#include "scarlett_platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWidth = 640u;
constexpr uint32_t kHeight = 400u;
constexpr int kMapSize = 16;

constexpr const char* kDefaultArenaMap[kMapSize] = {
    "1111111111111111",
    "1000002000000001",
    "1033002004440001",
    "1030000004000001",
    "1030002004000221",
    "1000002000000001",
    "1002202000330001",
    "1000000000300001",
    "1004000000300001",
    "1004400000000441",
    "1000000220000001",
    "1020000200030001",
    "1020000000030001",
    "1022004400033001",
    "1000000000000001",
    "1111111111111111",
};

struct ArenaWorld {
    uint8_t cells[kMapSize][kMapSize]{};
    float px = 2.5f;
    float py = 1.5f;
    float angle = 0.35f;
    float fire_flash = 0.0f;
    float bob_phase = 0.0f;
    uint32_t shots_fired = 0;
    uint32_t ticks = 0;
    std::string map_source = "builtin";
};

inline uint32_t PackXrgb(uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

void LoadArenaMapFromVfs(ArenaWorld& w) {
    for (int y = 0; y < kMapSize; ++y) {
        for (int x = 0; x < kMapSize; ++x) {
            w.cells[y][x] = static_cast<uint8_t>(kDefaultArenaMap[y][x] - '0');
        }
    }

    char resolved_map[512]{};
    const char* candidates[3] = {nullptr, "G:\\arena_map.txt", "arena_map.txt"};
    if (ScarlettPlatformResolveGameAsset("arena_map.txt", resolved_map, sizeof(resolved_map))) {
        candidates[0] = resolved_map;
    }
    for (const char* path : candidates) {
        if (!path || path[0] == '\0') continue;
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        char line[64]{};
        int row = 0;
        while (row < kMapSize && std::fgets(line, static_cast<int>(sizeof(line)), f)) {
            if (std::strlen(line) < static_cast<size_t>(kMapSize)) continue;
            for (int col = 0; col < kMapSize; ++col) {
                const char c = line[col];
                if (c >= '0' && c <= '4') {
                    w.cells[row][col] = static_cast<uint8_t>(c - '0');
                }
            }
            ++row;
        }
        std::fclose(f);
        if (row == kMapSize) {
            w.map_source = path;
            return;
        }
    }
}

bool IsWall(const ArenaWorld& w, float x, float y) {
    const int mx = static_cast<int>(x);
    const int my = static_cast<int>(y);
    if (mx < 0 || my < 0 || mx >= kMapSize || my >= kMapSize) return true;
    return w.cells[my][mx] != 0u;
}

uint32_t SampleWallTexel(uint8_t wall_type, int tx, int ty, bool side_shade) {
    const int u = tx & 63;
    const int v = ty & 63;
    uint8_t r = 128, g = 128, b = 128;

    if (wall_type == 1u) {
        // Tech-steel paneled wall with amber trim
        const bool border = (u < 3 || v < 3 || u > 60 || v > 60);
        const bool stripe = (v >= 28 && v <= 34);
        const int checker = ((u >> 3) ^ (v >> 3)) & 1;
        r = border ? 60u : (stripe ? 210u : static_cast<uint8_t>(84 + checker * 24));
        g = border ? 68u : (stripe ? 140u : static_cast<uint8_t>(96 + checker * 24));
        b = border ? 82u : (stripe ? 40u : static_cast<uint8_t>(116 + checker * 26));
    } else if (wall_type == 2u) {
        // Brick fortress wall
        const int row = v >> 4;
        const int bx = (u + ((row & 1) ? 16 : 0)) & 31;
        const bool mortar = (v & 15) < 2 || bx < 2;
        const int grain = ((u * 13 + v * 7) & 31) - 16;
        r = mortar ? 62u : static_cast<uint8_t>(std::clamp(172 + grain, 40, 255));
        g = mortar ? 58u : static_cast<uint8_t>(std::clamp(74 + grain / 2, 30, 220));
        b = mortar ? 54u : static_cast<uint8_t>(std::clamp(58 + grain / 2, 24, 200));
    } else if (wall_type == 3u) {
        // Scarlett emerald circuit pillar
        const bool grid = (u % 16 == 0) || (v % 16 == 0);
        const bool core = (u > 22 && u < 42 && v > 22 && v < 42);
        r = grid ? 40u : (core ? 44u : 36u);
        g = grid ? 220u : (core ? 245u : 128u);
        b = grid ? 150u : (core ? 185u : 94u);
    } else {
        // Magma / reactor core wall
        const int wave = static_cast<int>(std::sin(static_cast<float>(u + v) * 0.25f) * 40.0f);
        r = static_cast<uint8_t>(std::clamp(210 + wave, 120, 255));
        g = static_cast<uint8_t>(std::clamp(88 + wave / 2, 30, 210));
        b = static_cast<uint8_t>(std::clamp(32 + (u & 15), 16, 120));
    }

    if (side_shade) {
        r = static_cast<uint8_t>((static_cast<uint32_t>(r) * 180u) >> 8);
        g = static_cast<uint8_t>((static_cast<uint32_t>(g) * 180u) >> 8);
        b = static_cast<uint8_t>((static_cast<uint32_t>(b) * 180u) >> 8);
    }
    return PackXrgb(r, g, b);
}

uint32_t ApplyFog(uint32_t xrgb, float dist) {
    const float atten = std::clamp(1.0f / (1.0f + dist * dist * 0.045f), 0.16f, 1.0f);
    const uint8_t r = static_cast<uint8_t>(static_cast<float>((xrgb >> 16) & 0xFFu) * atten);
    const uint8_t g = static_cast<uint8_t>(static_cast<float>((xrgb >> 8) & 0xFFu) * atten);
    const uint8_t b = static_cast<uint8_t>(static_cast<float>(xrgb & 0xFFu) * atten);
    return PackXrgb(r, g, b);
}

void UpdateWorld(ArenaWorld& w, float dt) {
    const float rot_speed = 2.4f * dt;
    const float move_speed =
        (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_RSHIFT)) ? 4.8f : 3.0f) * dt;

    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_LEFT))) {
        w.angle -= rot_speed;
    }
    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_RIGHT))) {
        w.angle += rot_speed;
    }

    const float dir_x = std::cos(w.angle);
    const float dir_y = std::sin(w.angle);
    float dx = 0.0f;
    float dy = 0.0f;
    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_UP))) {
        dx += dir_x * move_speed;
        dy += dir_y * move_speed;
    }
    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_DOWN))) {
        dx -= dir_x * move_speed;
        dy -= dir_y * move_speed;
    }
    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_STRAFE_L))) {
        dx += dir_y * move_speed;
        dy -= dir_x * move_speed;
    }
    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_STRAFE_R))) {
        dx -= dir_y * move_speed;
        dy += dir_x * move_speed;
    }

    if (dx != 0.0f || dy != 0.0f) {
        w.bob_phase += dt * 10.0f;
        const float margin = 0.22f;
        if (!IsWall(w, w.px + dx + (dx > 0.0f ? margin : -margin), w.py)) {
            w.px += dx;
        }
        if (!IsWall(w, w.px, w.py + dy + (dy > 0.0f ? margin : -margin))) {
            w.py += dy;
        }
    }

    if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_FIRE))) {
        if (w.fire_flash <= 0.05f) {
            w.fire_flash = 1.0f;
            w.shots_fired += 1u;
            ScarlettPlatformPlaySfx(185.0f, 50u, 0.16f);
        }
    }
    w.fire_flash = std::max(0.0f, w.fire_flash - dt * 5.5f);
    w.ticks += 1u;
}

void RenderArenaFrame(const ArenaWorld& w, std::vector<uint32_t>& fb) {
    const float dir_x = std::cos(w.angle);
    const float dir_y = std::sin(w.angle);
    const float plane_x = -dir_y * 0.66f;
    const float plane_y = dir_x * 0.66f;
    const uint32_t hud_h = 48u;
    const uint32_t view_h = kHeight - hud_h;

    for (uint32_t x = 0; x < kWidth; ++x) {
        const float cam_x = 2.0f * static_cast<float>(x) / static_cast<float>(kWidth) - 1.0f;
        const float ray_dx = dir_x + plane_x * cam_x;
        const float ray_dy = dir_y + plane_y * cam_x;

        int map_x = static_cast<int>(w.px);
        int map_y = static_cast<int>(w.py);
        const float delta_x = (std::fabs(ray_dx) < 1e-6f) ? 1e30f : std::fabs(1.0f / ray_dx);
        const float delta_y = (std::fabs(ray_dy) < 1e-6f) ? 1e30f : std::fabs(1.0f / ray_dy);

        int step_x = 0;
        int step_y = 0;
        float side_x = 0.0f;
        float side_y = 0.0f;

        if (ray_dx < 0.0f) {
            step_x = -1;
            side_x = (w.px - static_cast<float>(map_x)) * delta_x;
        } else {
            step_x = 1;
            side_x = (static_cast<float>(map_x) + 1.0f - w.px) * delta_x;
        }
        if (ray_dy < 0.0f) {
            step_y = -1;
            side_y = (w.py - static_cast<float>(map_y)) * delta_y;
        } else {
            step_y = 1;
            side_y = (static_cast<float>(map_y) + 1.0f - w.py) * delta_y;
        }

        uint8_t hit_wall = 1u;
        bool hit_side = false;
        for (int step = 0; step < 64; ++step) {
            if (side_x < side_y) {
                side_x += delta_x;
                map_x += step_x;
                hit_side = false;
            } else {
                side_y += delta_y;
                map_y += step_y;
                hit_side = true;
            }
            if (map_x < 0 || map_y < 0 || map_x >= kMapSize || map_y >= kMapSize) {
                hit_wall = 1u;
                break;
            }
            if (w.cells[map_y][map_x] != 0u) {
                hit_wall = w.cells[map_y][map_x];
                break;
            }
        }

        const float perp_dist = std::max(0.05f, hit_side ? (side_y - delta_y) : (side_x - delta_x));
        const int line_h = static_cast<int>(static_cast<float>(view_h) / perp_dist);
        const int draw_start = std::max(0, -line_h / 2 + static_cast<int>(view_h) / 2);
        const int draw_end = std::min(static_cast<int>(view_h) - 1, line_h / 2 + static_cast<int>(view_h) / 2);

        float wall_x = hit_side ? (w.px + perp_dist * ray_dx) : (w.py + perp_dist * ray_dy);
        wall_x -= std::floor(wall_x);
        const int tex_x = std::clamp(static_cast<int>(wall_x * 64.0f), 0, 63);

        for (int y = 0; y < draw_start; ++y) {
            const uint8_t sky = static_cast<uint8_t>(18 + (y * 36) / static_cast<int>(view_h));
            fb[static_cast<size_t>(y) * kWidth + x] = PackXrgb(sky, static_cast<uint8_t>(sky + 8), static_cast<uint8_t>(sky + 26));
        }

        for (int y = draw_start; y <= draw_end; ++y) {
            const int d = y * 256 - static_cast<int>(view_h) * 128 + line_h * 128;
            const int tex_y = std::clamp(((d * 64) / std::max(1, line_h)) / 256, 0, 63);
            const uint32_t col = ApplyFog(SampleWallTexel(hit_wall, tex_x, tex_y, hit_side), perp_dist);
            fb[static_cast<size_t>(y) * kWidth + x] = col;
        }

        for (int y = draw_end + 1; y < static_cast<int>(view_h); ++y) {
            const float row_ratio = static_cast<float>(y - static_cast<int>(view_h) / 2) /
                                    static_cast<float>(view_h / 2);
            const uint8_t f = static_cast<uint8_t>(std::clamp(22.0f + row_ratio * 64.0f, 16.0f, 96.0f));
            fb[static_cast<size_t>(y) * kWidth + x] = PackXrgb(f, static_cast<uint8_t>(f * 8 / 10), static_cast<uint8_t>(f * 7 / 10));
        }
    }

    // Center plasma blaster sprite + muzzle flash
    const int bob_y = static_cast<int>(std::sin(w.bob_phase) * 5.0f);
    const int gun_cx = static_cast<int>(kWidth / 2u);
    const int gun_top = static_cast<int>(view_h) - 82 + bob_y;
    for (int y = std::max(0, gun_top); y < static_cast<int>(view_h); ++y) {
        const int dy = y - gun_top;
        const int half_w = 14 + dy / 2;
        for (int x = std::max(0, gun_cx - half_w); x < std::min(static_cast<int>(kWidth), gun_cx + half_w); ++x) {
            const bool barrel = std::abs(x - gun_cx) < 8 && dy < 32;
            const uint8_t glow = static_cast<uint8_t>(w.fire_flash * 220.0f);
            fb[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)] =
                barrel ? PackXrgb(static_cast<uint8_t>(std::min(255, 64 + glow)),
                                  static_cast<uint8_t>(std::min(255, 190 + glow / 3)),
                                  static_cast<uint8_t>(std::min(255, 220 + glow / 4)))
                       : PackXrgb(58u, 64u, 74u);
        }
    }
    if (w.fire_flash > 0.1f) {
        const int r = static_cast<int>(w.fire_flash * 26.0f);
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy <= r * r) {
                    const int px = gun_cx + dx;
                    const int py = gun_top - 4 + dy;
                    if (px >= 0 && px < static_cast<int>(kWidth) && py >= 0 && py < static_cast<int>(view_h)) {
                        fb[static_cast<size_t>(py) * kWidth + static_cast<size_t>(px)] =
                            PackXrgb(255u, 230u, 110u);
                    }
                }
            }
        }
    }

    // Bottom Scarlett status bar + minimap
    for (uint32_t y = view_h; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const bool top_border = (y == view_h || y == view_h + 1u);
            fb[static_cast<size_t>(y) * kWidth + x] =
                top_border ? PackXrgb(40u, 200u, 110u) : PackXrgb(24u, 28u, 36u);
        }
    }

    // Mini-radar in top-left corner
    const int tile_px = 5;
    for (int my = 0; my < kMapSize; ++my) {
        for (int mx = 0; mx < kMapSize; ++mx) {
            const uint32_t col = w.cells[my][mx] ? PackXrgb(56u, 190u, 120u) : PackXrgb(16u, 22u, 28u);
            for (int dy = 0; dy < tile_px; ++dy) {
                for (int dx = 0; dx < tile_px; ++dx) {
                    const uint32_t sx = static_cast<uint32_t>(10 + mx * tile_px + dx);
                    const uint32_t sy = static_cast<uint32_t>(10 + my * tile_px + dy);
                    fb[static_cast<size_t>(sy) * kWidth + sx] = col;
                }
            }
        }
    }
    const uint32_t p_sx = static_cast<uint32_t>(10 + static_cast<int>(w.px * static_cast<float>(tile_px)));
    const uint32_t p_sy = static_cast<uint32_t>(10 + static_cast<int>(w.py * static_cast<float>(tile_px)));
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int x = static_cast<int>(p_sx) + dx;
            const int y = static_cast<int>(p_sy) + dy;
            if (x >= 0 && x < static_cast<int>(kWidth) && y >= 0 && y < static_cast<int>(kHeight)) {
                fb[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)] = PackXrgb(255u, 70u, 70u);
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    bool headless = false;
    int verify_ticks = 0;
    const char* metrics_out = "T:\\scarlett_arena_metrics.txt";
    const char* ppm_out = "T:\\scarlett_arena_frame.ppm";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--verify-ticks") == 0 && i + 1 < argc) {
            verify_ticks = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--metrics-out") == 0 && i + 1 < argc) {
            metrics_out = argv[++i];
        } else if (std::strcmp(argv[i], "--ppm-out") == 0 && i + 1 < argc) {
            ppm_out = argv[++i];
        }
    }

    ScarlettPlatformConfig cfg{};
    cfg.fb_width = kWidth;
    cfg.fb_height = kHeight;
    cfg.window_width = 960u;
    cfg.window_height = 600u;
    cfg.prefer_warp = use_warp ? 1 : 0;
    cfg.headless = (headless || verify_ticks > 0) ? 1 : 0;
    cfg.verify_ticks = verify_ticks;
    cfg.window_title = "OpenScarlett - Scarlett Arena (Xbox Series X|S)";
    cfg.save_container = "scarlett_arena";

    if (!ScarlettPlatformInit(&cfg)) {
        std::fprintf(stderr, "[ScarlettArena] FAIL: ScarlettPlatformInit failed\n");
        return 1;
    }

    ArenaWorld world{};
    LoadArenaMapFromVfs(world);
    std::vector<uint32_t> fb(static_cast<size_t>(kWidth) * kHeight, 0u);

    if (verify_ticks > 0) {
        for (int t = 0; t < verify_ticks && !ScarlettPlatformShouldQuit(); ++t) {
            ScarlettPlatformInjectVerifyInput(static_cast<uint32_t>(t));
            ScarlettPlatformPumpInput();
            int pr = 0;
            unsigned char key = 0;
            while (ScarlettPlatformPopKeyEvent(&pr, &key)) {
            }
            UpdateWorld(world, 1.0f / 30.0f);
            RenderArenaFrame(world, fb);
            if (!ScarlettPlatformPresentFrame(fb.data(), kWidth, kHeight, 1)) {
                std::fprintf(stderr, "[ScarlettArena] FAIL: PresentFrame failed at tick %d\n", t);
                ScarlettPlatformShutdown();
                return 2;
            }
        }

        char save_blob[160]{};
        std::snprintf(save_blob, sizeof(save_blob),
                      "SCARLETT_ARENA|px=%.2f|py=%.2f|ang=%.2f|shots=%u|ticks=%u",
                      world.px, world.py, world.angle, world.shots_fired, world.ticks);
        if (!ScarlettPlatformSaveAndVerify("arena_checkpoint", save_blob, std::strlen(save_blob))) {
            std::fprintf(stderr, "[ScarlettArena] FAIL: XGameSave S:\\ round-trip failed\n");
            ScarlettPlatformShutdown();
            return 3;
        }

        (void)ScarlettPlatformWriteLastFramePpm(ppm_out);
        char extra[256]{};
        std::snprintf(extra, sizeof(extra), "game=ScarlettArena\nmap_source=%s\nshots_fired=%u\n",
                      world.map_source.c_str(), world.shots_fired);
        (void)ScarlettPlatformWriteMetricsFile(metrics_out, extra);

        ScarlettPlatformShutdown();
        return 0;
    }

    // Interactive 60 FPS playable loop
    uint32_t last_ms = ScarlettPlatformGetTicksMs();
    while (!ScarlettPlatformShouldQuit()) {
        ScarlettPlatformPumpInput();
        if (ScarlettPlatformIsKeyHeld(static_cast<unsigned char>(SCARLETT_KEY_ESCAPE))) {
            break;
        }
        int pr = 0;
        unsigned char key = 0;
        while (ScarlettPlatformPopKeyEvent(&pr, &key)) {
        }
        const uint32_t now_ms = ScarlettPlatformGetTicksMs();
        const float dt = std::clamp(static_cast<float>(now_ms - last_ms) / 1000.0f, 0.004f, 0.05f);
        last_ms = now_ms;

        UpdateWorld(world, dt);
        RenderArenaFrame(world, fb);
        (void)ScarlettPlatformPresentFrame(fb.data(), kWidth, kHeight, 1);
    }

    const char* exit_blob = "SCARLETT_ARENA_INTERACTIVE_OK";
    (void)ScarlettPlatformSaveAndVerify("arena_session", exit_blob, std::strlen(exit_blob));
    ScarlettPlatformShutdown();
    return 0;
}
