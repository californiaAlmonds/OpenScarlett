// OpenScarlett MIT Standalone 3D Multi-Mesh Xbox-ABI GDK Title (`gdk_3d_scene.exe`).
//
// Pure Xbox Series X|S guest binary (NEVER links openscarlett_core):
//   - d3d12_xs.dll    : D3D12XboxCreateDevice, two-pass shadow map (D32_FLOAT) + Blinn-Phong PBR,
//                       BC1 + BC3 block-compressed textures via CopyTextureRegion + SRV heap,
//                       hierarchical transforms + 3-bone GPU vertex skinning, PresentX!
//   - xgameruntime.dll: XGameRuntimeInitialize, XSystemGetDeviceType, XGameGetXboxTitleId,
//                       XDisplayTryAcquireTimeoutDeferral
//   - GameInput.dll   : Live keyboard & Xbox gamepad orbital camera control
//   - XAudio2_9.dll   : Procedural harmonic synth cues (honors OPENSCARLETT_AUDIO_NULL=1)
//   - kernelx.dll     : GetConsoleType, XGetProcessorCount, TitleMemoryStatus
//   - VFS S:\         : Writes and verifies S:\scene_checkpoint.txt via Win32 CreateFileW!

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/kernel/kernelx_abi.h"

#include "bc_textures.h"
#include "scene_math.h"
#include "scene_meshes.h"

namespace {

constexpr uint32_t kShadowMapSize = 1024u;
constexpr uint32_t kMaxDrawObjects = 16u;
constexpr uint32_t kMaxBones = 8u;
constexpr uint32_t kAudioSampleRate = 48000u;

// HLSL SM 5.0 shaders compiled at runtime via D3DCompile.
// Supports both Pass 1 (directional light depth-only shadow pass) and Pass 2
// (main lit PBR/Blinn-Phong pass with 3x3 PCF shadow sampling, BC1/BC3 textures,
// rim/fill lighting, ambient occlusion gradient, and 3-bone linear-blend skinning).
constexpr const char* kSceneShadersHlsl = R"HLSL(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 g_ViewProj;
    row_major float4x4 g_LightViewProj;
    float4 g_CameraPos;
    float4 g_SunDir;      // xyz = unit dir TO sun, w = sun intensity
    float4 g_SunColor;    // rgb = warm sun color, a = ambient intensity
    float4 g_RimDir;      // xyz = unit dir TO rim light, w = rim intensity
    float4 g_RimColor;    // rgb = cool rim color, a = shadow texel size
    float4 g_Params;      // x = time, y = shadow bias, z/w = reserved
};

cbuffer ObjectConstants : register(b1) {
    row_major float4x4 g_Bones[8];
    float4 g_Material;    // x = texIndex (0=BC1 floor, 1=BC3 metal), y = specPower, z = specStrength, w = emissiveScale
    float4 g_Tint;        // rgb = tint multiplier, a = metallic factor
};

Texture2D<float4> g_FloorBc1   : register(t0);
Texture2D<float4> g_MetalBc3   : register(t1);
Texture2D<float>  g_ShadowMap  : register(t2);

SamplerState g_LinearWrap : register(s0);
SamplerState g_ShadowSamp : register(s1);

struct VsIn {
    float3 Position    : POSITION;
    float3 Normal      : NORMAL;
    float2 TexCoord    : TEXCOORD0;
    float4 Color       : COLOR0;
    uint4  BoneIndices : BLENDINDICES0;
    float4 BoneWeights : BLENDWEIGHT0;
};

struct ShadowVsOut {
    float4 PositionCS : SV_Position;
};

struct MainVsOut {
    float4 PositionCS : SV_Position;
    float3 WorldPos   : POSITION0;
    float3 WorldNorm  : NORMAL0;
    float2 TexCoord   : TEXCOORD0;
    float4 Color      : COLOR0;
    float4 LightPosCS : TEXCOORD1;
};

float4x4 ComputeSkinMatrix(uint4 idx, float4 w) {
    float4x4 m = g_Bones[idx.x] * w.x;
    if (w.y > 0.0f) m += g_Bones[idx.y] * w.y;
    if (w.z > 0.0f) m += g_Bones[idx.z] * w.z;
    if (w.w > 0.0f) m += g_Bones[idx.w] * w.w;
    return m;
}

ShadowVsOut ShadowVsMain(VsIn input) {
    ShadowVsOut o;
    float4x4 skinMat = ComputeSkinMatrix(input.BoneIndices, input.BoneWeights);
    float4 worldPos = mul(float4(input.Position, 1.0f), skinMat);
    o.PositionCS = mul(worldPos, g_LightViewProj);
    return o;
}

MainVsOut MainVsMain(VsIn input) {
    MainVsOut o;
    float4x4 skinMat = ComputeSkinMatrix(input.BoneIndices, input.BoneWeights);
    float4 worldPos = mul(float4(input.Position, 1.0f), skinMat);
    float3 worldNorm = mul(input.Normal, (float3x3)skinMat);
    float nLen = max(length(worldNorm), 1e-5f);

    o.PositionCS = mul(worldPos, g_ViewProj);
    o.WorldPos = worldPos.xyz;
    o.WorldNorm = worldNorm / nLen;
    o.TexCoord = input.TexCoord;
    o.Color = input.Color;
    o.LightPosCS = mul(worldPos, g_LightViewProj);
    return o;
}

float SampleShadowPcf(float4 lightPosCS, float NdotL) {
    float3 proj = lightPosCS.xyz / max(lightPosCS.w, 1e-5f);
    float2 uv = float2(proj.x * 0.5f + 0.5f, 0.5f - proj.y * 0.5f);
    if (uv.x < 0.002f || uv.x > 0.998f || uv.y < 0.002f || uv.y > 0.998f ||
        proj.z < 0.0f || proj.z > 1.0f) {
        return 1.0f;
    }

    float bias = g_Params.y + (1.0f - clamp(NdotL, 0.0f, 1.0f)) * 0.0022f;
    float texel = g_RimColor.a;
    float lit = 0.0f;
    [unroll]
    for (int sy = -1; sy <= 1; ++sy) {
        [unroll]
        for (int sx = -1; sx <= 1; ++sx) {
            float sampleDepth = g_ShadowMap.SampleLevel(
                g_ShadowSamp, uv + float2((float)sx, (float)sy) * texel, 0.0f).r;
            lit += ((proj.z - bias) <= sampleDepth) ? 1.0f : 0.0f;
        }
    }
    return lerp(0.06f, 1.0f, lit / 9.0f);
}

float4 MainPsMain(MainVsOut input) : SV_Target0 {
    float4 texSample = (g_Material.x < 0.5f)
        ? g_FloorBc1.Sample(g_LinearWrap, input.TexCoord)
        : g_MetalBc3.Sample(g_LinearWrap, input.TexCoord);

    float3 albedo = texSample.rgb * input.Color.rgb * g_Tint.rgb;
    float emissiveMask = (g_Material.x >= 0.5f) ? texSample.a : 0.0f;

    float3 N = normalize(input.WorldNorm);
    float3 V = normalize(g_CameraPos.xyz - input.WorldPos);

    // 1. Directional Sun Light + 3x3 PCF Shadow Map
    float3 L = normalize(g_SunDir.xyz);
    float NdotL = max(dot(N, L), 0.0f);
    float shadow = SampleShadowPcf(input.LightPosCS, NdotL);
    float3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0f);
    float spec = pow(NdotH, max(g_Material.y, 2.0f)) * g_Material.z * (0.55f + 0.45f * emissiveMask);

    float3 sunDiffuse = albedo * g_SunColor.rgb * (NdotL * g_SunDir.w * shadow);
    float3 sunSpec = lerp(float3(1.0f, 0.98f, 0.94f), albedo, g_Tint.a) *
                     g_SunColor.rgb * (spec * g_SunDir.w * shadow);

    // 2. Cool Rim / Fill Light + Fresnel Accent
    float3 Lr = normalize(g_RimDir.xyz);
    float NdotLr = max(dot(N, Lr), 0.0f);
    float fresnel = pow(1.0f - max(dot(N, V), 0.0f), 3.0f);
    float3 rimLight = albedo * g_RimColor.rgb * (NdotLr * g_RimDir.w) +
                      g_RimColor.rgb * (fresnel * 0.28f * g_RimDir.w);

    // 3. Hemisphere Ambient + Height Ambient Occlusion Gradient
    float hemi = clamp(N.y * 0.5f + 0.5f, 0.0f, 1.0f);
    float heightAo = clamp(0.62f + input.WorldPos.y * 0.22f, 0.62f, 1.0f);
    float3 ambientTint = lerp(float3(0.24f, 0.28f, 0.38f), float3(0.58f, 0.72f, 0.96f), hemi);
    float3 ambient = albedo * ambientTint * (g_SunColor.a * heightAo);

    // 4. BC3 Alpha-Driven Emissive Accent
    float3 emissive = texSample.rgb * (emissiveMask * emissiveMask * g_Material.w);

    // Distance atmospheric blend toward twilight horizon
    float camDist = length(g_CameraPos.xyz - input.WorldPos);
    float fog = clamp((camDist - 12.0f) / 20.0f, 0.0f, 0.35f);
    float3 linearColor = ambient + sunDiffuse + sunSpec + rimLight + emissive;
    linearColor = lerp(linearColor, float3(0.06f, 0.09f, 0.16f), fog);

    // Filmic ACES-like tone map + gamma 2.0 encoding
    float3 mapped = (linearColor * (2.51f * linearColor + 0.03f)) /
                    (linearColor * (2.43f * linearColor + 0.59f) + 0.14f);
    float3 srgb = sqrt(clamp(mapped, 0.0f, 1.0f));
    return float4(srgb, 1.0f);
}
)HLSL";

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

struct alignas(256) SceneConstants {
    gdk3d::Mat4 view_proj{};
    gdk3d::Mat4 light_view_proj{};
    float camera_pos[4]{};
    float sun_dir[4]{};
    float sun_color[4]{};
    float rim_dir[4]{};
    float rim_color[4]{};
    float params[4]{};
    float pad[8]{};
};
static_assert(sizeof(SceneConstants) == 256, "SceneConstants must be 256 bytes");

struct alignas(256) ObjectConstants {
    gdk3d::Mat4 bones[kMaxBones]{};
    float material[4]{};
    float tint[4]{};
    float pad[56]{};
};
static_assert(sizeof(ObjectConstants) == 768, "ObjectConstants must be 768 bytes (3x256)");

struct LaunchConfig {
    bool headless = false;
    bool prefer_warp = false;
    uint32_t width = 1280u;
    uint32_t height = 720u;
    bool width_specified = false;
    bool height_specified = false;
    int max_frames = 0;  // 0 = unlimited in windowed, 12 in headless
    std::string stats_out;
    std::string ppm_out;
};

struct WindowState {
    HWND hwnd = nullptr;
    bool should_quit = false;
    std::array<uint8_t, 256> keys{};
};

WindowState g_win;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

bool WriteWholeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t written_total = 0;
    while (written_total < size) {
        DWORD chunk = 0;
        const DWORD req = static_cast<DWORD>(std::min<size_t>(size - written_total, 16u * 1024u * 1024u));
        if (!::WriteFile(h, p + written_total, req, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        written_total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool ReadWholeFileW(const wchar_t* path, std::string& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    ::CloseHandle(h);
    return ok != FALSE && got == static_cast<DWORD>(out.size());
}

bool WritePpmUtf8(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba8) {
    if (path.empty() || !rgba8 || w == 0u || h == 0u) return false;
    char hdr[64]{};
    const int hdr_len = std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", w, h);
    if (hdr_len <= 0) return false;
    const size_t n_px = static_cast<size_t>(w) * h;
    std::vector<uint8_t> ppm;
    ppm.reserve(static_cast<size_t>(hdr_len) + n_px * 3u);
    ppm.insert(ppm.end(), hdr, hdr + hdr_len);
    for (size_t i = 0; i < n_px; ++i) {
        ppm.push_back(rgba8[i * 4u + 0u]);
        ppm.push_back(rgba8[i * 4u + 1u]);
        ppm.push_back(rgba8[i * 4u + 2u]);
    }
    const std::wstring wpath = Utf8ToWide(path);
    return WriteWholeFileW(wpath.c_str(), ppm.data(), ppm.size());
}

LRESULT CALLBACK SceneWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_win.should_quit = true;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN) {
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            if (wParam < 256u) {
                g_win.keys[static_cast<size_t>(wParam)] = 1u;
            }
            if (wParam == VK_ESCAPE) {
                g_win.should_quit = true;
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam < 256u) {
                g_win.keys[static_cast<size_t>(wParam)] = 0u;
            }
            return 0;
        case WM_KILLFOCUS:
            g_win.keys.fill(0u);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool CreateSceneWindow(uint32_t width, uint32_t height, bool headless) {
    static constexpr const wchar_t* kClassName = L"OpenScarlettGdk3DSceneWndClass";
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SceneWndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kClassName;
    (void)::RegisterClassExW(&wc);

    RECT wr{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    ::AdjustWindowRectEx(&wr, style, FALSE, 0);

    g_win.hwnd = ::CreateWindowExW(
        0,
        kClassName,
        L"OpenScarlett - 3D Shadowed Skinned GDK Scene (Xbox Series X|S)",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);
    if (!g_win.hwnd) return false;

    if (!headless) {
        ::ShowWindow(g_win.hwnd, SW_SHOW);
        ::UpdateWindow(g_win.hwnd);
    }
    return true;
}

D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                      D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_DESC MakeBufferDesc(uint64_t bytes) noexcept {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1u;
    d.DepthOrArraySize = 1u;
    d.MipLevels = 1u;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1u;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

bool EnvFlagTrue(const char* name) {
    char buf[64]{};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return false;
    return (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T');
}

// Synthesizes a clean stereo harmonic triad chime for XAudio2_9.dll.
std::vector<int16_t> BuildHarmonicChimePcm(float base_freq_hz, uint32_t duration_ms) {
    const size_t frames = (static_cast<size_t>(kAudioSampleRate) * duration_ms) / 1000u;
    std::vector<int16_t> pcm(frames * 2u, 0);
    const float f1 = base_freq_hz;
    const float f2 = base_freq_hz * 1.25f;  // Major third
    const float f3 = base_freq_hz * 1.50f;  // Perfect fifth
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kAudioSampleRate);
        const float progress = static_cast<float>(i) / static_cast<float>(std::max<size_t>(frames, 1u));
        const float attack = std::min(1.0f, progress * 18.0f);
        const float decay = (1.0f - progress) * (1.0f - progress);
        const float env = attack * decay * 0.14f;

        const float sig = (std::sin(2.0f * gdk3d::kPi * f1 * t) * 0.50f +
                           std::sin(2.0f * gdk3d::kPi * f2 * t) * 0.30f +
                           std::sin(2.0f * gdk3d::kPi * f3 * t) * 0.20f) * env;
        const int16_t s = static_cast<int16_t>(std::clamp(sig * 32767.0f, -32767.0f, 32767.0f));
        pcm[i * 2u + 0u] = s;
        pcm[i * 2u + 1u] = s;
    }
    return pcm;
}

struct AudioContext {
    IXAudio2* xaudio2 = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    std::array<std::vector<int16_t>, 4> ring{};
    uint32_t next_slot = 0u;
    uint64_t buffers_submitted = 0ull;

    void Init() {
        if (XAudio2Create(&xaudio2, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) != OS_XAUDIO2_S_OK || !xaudio2) {
            return;
        }
        if (xaudio2->CreateMasteringVoice(&master, 2u, kAudioSampleRate) != OS_XAUDIO2_S_OK || !master) {
            return;
        }
        OS_WAVEFORMATEX wfx{};
        wfx.wFormatTag = OS_WAVE_FORMAT_PCM;
        wfx.nChannels = 2u;
        wfx.nSamplesPerSec = kAudioSampleRate;
        wfx.wBitsPerSample = 16u;
        wfx.nBlockAlign = 4u;
        wfx.nAvgBytesPerSec = kAudioSampleRate * 4u;
        wfx.cbSize = 0u;
        if (xaudio2->CreateSourceVoice(&voice, &wfx) == OS_XAUDIO2_S_OK && voice) {
            (void)voice->Start(0u, OS_XAUDIO2_COMMIT_NOW);
        }
    }

    void PlayChime(float base_freq_hz, uint32_t duration_ms) {
        if (!voice) return;
        OS_XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st, 0u);
        if (st.BuffersQueued >= 3u) return;

        auto& buf = ring[next_slot];
        next_slot = (next_slot + 1u) % static_cast<uint32_t>(ring.size());
        buf = BuildHarmonicChimePcm(base_freq_hz, duration_ms);

        OS_XAUDIO2_BUFFER xbuf{};
        xbuf.AudioBytes = static_cast<uint32_t>(buf.size() * sizeof(int16_t));
        xbuf.pAudioData = reinterpret_cast<const uint8_t*>(buf.data());
        if (voice->SubmitSourceBuffer(&xbuf, nullptr) == OS_XAUDIO2_S_OK) {
            ++buffers_submitted;
        }
    }

    void Shutdown() {
        if (voice) {
            (void)voice->Stop(0u, OS_XAUDIO2_COMMIT_NOW);
            voice->DestroyVoice();
            voice = nullptr;
        }
        if (master) {
            master->DestroyVoice();
            master = nullptr;
        }
        if (xaudio2) {
            xaudio2->StopEngine();
            xaudio2->Release();
            xaudio2 = nullptr;
        }
    }
};

struct FrameVisualMetrics {
    uint32_t non_black_pixels = 0u;
    uint32_t unique_colors = 0u;
    uint32_t lit_pixels = 0u;
    uint32_t shadow_pixels = 0u;
    uint32_t bc_textured_pixels = 0u;
    double lum_mean = 0.0;
    double lum_stddev = 0.0;
};

FrameVisualMetrics AnalyzeFrameRgba(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    FrameVisualMetrics m{};
    const size_t n_px = static_cast<size_t>(w) * h;
    if (n_px == 0u || rgba.size() < n_px * 4u) return m;

    // Sky clear color in sRGB bytes is approximately (15, 23, 41).
    std::set<uint32_t> uniq;
    double sum_lum = 0.0;
    double sum_lum_sq = 0.0;

    for (size_t i = 0; i < n_px; ++i) {
        const uint8_t r = rgba[i * 4u + 0u];
        const uint8_t g = rgba[i * 4u + 1u];
        const uint8_t b = rgba[i * 4u + 2u];
        if (r > 6u || g > 6u || b > 6u) {
            ++m.non_black_pixels;
        }
        if (uniq.size() < 8192u) {
            uniq.insert((static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b);
        }

        const double lum = 0.2126 * static_cast<double>(r) +
                           0.7152 * static_cast<double>(g) +
                           0.0722 * static_cast<double>(b);
        sum_lum += lum;
        sum_lum_sq += lum * lum;

        // Classify non-sky scene geometry pixels (sky is dark blue-dominant ~ (15,23,41))
        const bool is_sky = (std::abs(static_cast<int>(r) - 15) <= 6 &&
                             std::abs(static_cast<int>(g) - 23) <= 6 &&
                             std::abs(static_cast<int>(b) - 41) <= 8);
        if (!is_sky) {
            ++m.bc_textured_pixels;
            if (lum >= 115.0) {
                ++m.lit_pixels;
            } else if (lum >= 26.0 && lum < 96.0) {
                ++m.shadow_pixels;
            }
        }
    }

    m.unique_colors = static_cast<uint32_t>(uniq.size());
    m.lum_mean = sum_lum / static_cast<double>(n_px);
    const double var = std::max(0.0, (sum_lum_sq / static_cast<double>(n_px)) - m.lum_mean * m.lum_mean);
    m.lum_stddev = std::sqrt(var);
    return m;
}

int FailStep(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_3d_scene] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    LaunchConfig cfg{};
    cfg.prefer_warp = EnvFlagTrue("OPENSCARLETT_PREFER_WARP");

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            cfg.headless = true;
        } else if (std::strcmp(argv[i], "--windowed") == 0) {
            cfg.headless = false;
        } else if (std::strcmp(argv[i], "--warp") == 0) {
            cfg.prefer_warp = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            cfg.max_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            cfg.width = static_cast<uint32_t>(std::max(128, std::atoi(argv[++i])));
            cfg.width_specified = true;
        } else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            cfg.height = static_cast<uint32_t>(std::max(128, std::atoi(argv[++i])));
            cfg.height_specified = true;
        } else if (std::strcmp(argv[i], "--stats-out") == 0 && i + 1 < argc) {
            cfg.stats_out = argv[++i];
        } else if (std::strcmp(argv[i], "--ppm-out") == 0 && i + 1 < argc) {
            cfg.ppm_out = argv[++i];
        }
    }

    if (cfg.headless) {
        if (!cfg.width_specified) cfg.width = 640u;
        if (!cfg.height_specified) cfg.height = 360u;
        if (cfg.max_frames <= 0) cfg.max_frames = 12;
    }

    std::string gpu_backend_env = "d3d12";
    {
        char buf[128]{};
        const DWORD len = ::GetEnvironmentVariableA("OPENSCARLETT_GPU_BACKEND", buf,
                                                    static_cast<DWORD>(sizeof(buf)));
        if (len > 0u && len < sizeof(buf)) {
            gpu_backend_env = buf;
        }
    }

    // 1. Query KernelX console & unified memory status (statically imports kernelx.dll).
    const uint32_t console_type = GetConsoleType();
    const uint32_t proc_count = XGetProcessorCount();
    TITLEMEMORYSTATUS tms{};
    tms.dwLength = static_cast<uint32_t>(sizeof(TITLEMEMORYSTATUS));
    if (TitleMemoryStatus(&tms) == 0) {
        return FailStep("kernelx TitleMemoryStatus");
    }

    // 2. Initialize GDK Runtime (xgameruntime.dll).
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        return FailStep("XGameRuntimeInitialize");
    }
    const XSystemDeviceType device_type = XSystemGetDeviceType();
    uint32_t title_id = 0u;
    (void)XGameGetXboxTitleId(&title_id);
    XDisplayTimeoutDeferralHandle display_deferral = nullptr;
    (void)XDisplayTryAcquireTimeoutDeferral(&display_deferral);

    // 3. Create Win32 window (hidden when --headless, visible 1280x720 when windowed).
    if (!CreateSceneWindow(cfg.width, cfg.height, cfg.headless)) {
        if (display_deferral) XDisplayCloseTimeoutDeferralHandle(display_deferral);
        XGameRuntimeUninitialize();
        return FailStep("CreateSceneWindow");
    }

    // 4. Initialize GameInput (GameInput.dll).
    IGameInput* game_input = nullptr;
    if (GameInputCreate(&game_input) == GAMEINPUT_S_OK && game_input) {
        (void)OpenScarlettGameInputSetTargetWindow(game_input, g_win.hwnd);
        game_input->SetFocusPolicy(cfg.headless ? GameInputEnableBackgroundInput
                                                : GameInputDefaultFocusPolicy);
    }

    // 5. Initialize XAudio2_9 (XAudio2_9.dll) and play startup harmonic chime.
    AudioContext audio{};
    audio.Init();
    audio.PlayChime(440.0f, 80u);

    auto cleanup_platform = [&]() {
        audio.Shutdown();
        if (game_input) {
            game_input->Release();
            game_input = nullptr;
        }
        if (g_win.hwnd) {
            ::DestroyWindow(g_win.hwnd);
            g_win.hwnd = nullptr;
        }
        if (display_deferral) {
            XDisplayCloseTimeoutDeferralHandle(display_deferral);
            display_deferral = nullptr;
        }
        XGameRuntimeUninitialize();
    };

    // 6. Initialize Xbox Series X|S D3D12X device via D3D12XboxCreateDevice (d3d12_xs.dll).
    OpenScarlettD3D12SetPreferWarp(cfg.prefer_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1u;
    dev_params.DisableGeometryShaderAllocations = 1u;
    dev_params.DisableTessellationShaderAllocations = 1u;

    ComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        cleanup_platform();
        return FailStep("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    (void)device->GetGpuHardwareInfoX(&gpu_info);
    (void)device->SetFrameIntervalX(nullptr, cfg.headless ? 0u : 16667u, 2u, 0u);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(&qd, IID_ID3D12CommandQueueX,
                                    reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        cleanup_platform();
        return FailStep("CreateCommandQueue", hr);
    }

    D3D12XBOX_PRESENTX_CONFIG pcfg{};
    pcfg.OutputWindow = g_win.hwnd;
    pcfg.PreferredFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    pcfg.ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    (void)OpenScarlettD3D12SetPresentConfig(queue.Get(), &pcfg);

    if (cfg.headless || !cfg.stats_out.empty() || !cfg.ppm_out.empty()) {
        ComPtr<IOpenScarlettPresentCapture> cap;
        if (SUCCEEDED(queue->QueryInterface(IID_IOpenScarlettPresentCapture,
                                            reinterpret_cast<void**>(cap.Put()))) && cap) {
            (void)cap->SetReadbackCaptureEnabled(TRUE);
        }
    }

    ComPtr<ID3D12CommandAllocator> cmd_alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(cmd_alloc.Put()));
    if (FAILED(hr)) {
        cleanup_platform();
        return FailStep("CreateCommandAllocator", hr);
    }

    ComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc.Get(), nullptr,
                                   IID_ID3D12GraphicsCommandListX,
                                   reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr)) {
        cleanup_platform();
        return FailStep("CreateCommandList", hr);
    }

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr)) {
        cleanup_platform();
        return FailStep("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        cleanup_platform();
        return FailStep("CreateEventW");
    }
    uint64_t fence_val = 0ull;

    auto wait_gpu_idle = [&]() {
        fence_val += 1ull;
        if (SUCCEEDED(queue->Signal(fence.Get(), fence_val))) {
            if (fence->GetCompletedValue() < fence_val) {
                (void)fence->SetEventOnCompletion(fence_val, fence_event);
                ::WaitForSingleObject(fence_event, 15000u);
            }
        }
    };

    // 7. Compile embedded SM 5.0 shaders (ShadowVS, MainVS, MainPS).
    ComPtr<ID3DBlob> shadow_vs;
    ComPtr<ID3DBlob> main_vs;
    ComPtr<ID3DBlob> main_ps;
    ComPtr<ID3DBlob> err_blob;
    const size_t hlsl_len = std::strlen(kSceneShadersHlsl);

    auto compile_stage = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out_blob) -> bool {
        err_blob.Reset();
        const HRESULT chr = ::D3DCompile(
            kSceneShadersHlsl, hlsl_len, "gdk_3d_scene.hlsl", nullptr, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u,
            out_blob.Put(), err_blob.Put());
        if (FAILED(chr) || !out_blob) {
            if (err_blob && err_blob->GetBufferPointer()) {
                std::fprintf(stderr, "[gdk_3d_scene] HLSL %s error:\n%s\n",
                             entry, static_cast<const char*>(err_blob->GetBufferPointer()));
            }
            return false;
        }
        return true;
    };

    if (!compile_stage("ShadowVsMain", "vs_5_0", shadow_vs) ||
        !compile_stage("MainVsMain", "vs_5_0", main_vs) ||
        !compile_stage("MainPsMain", "ps_5_0", main_ps)) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("D3DCompile");
    }

    // 8. Root Signature: CBV b0 (Scene), CBV b1 (Object+Bones), SRV Table t0..t2 (BC1, BC3, Shadow), s0, s1.
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 3u;
    srv_range.BaseShaderRegister = 0u;
    srv_range.RegisterSpace = 0u;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_params[3]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[0].Descriptor.ShaderRegister = 0u;
    root_params[0].Descriptor.RegisterSpace = 0u;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[1].Descriptor.ShaderRegister = 1u;
    root_params[1].Descriptor.RegisterSpace = 0u;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[2].DescriptorTable.NumDescriptorRanges = 1u;
    root_params[2].DescriptorTable.pDescriptorRanges = &srv_range;
    root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    // s0: linear wrap sampler for BC1/BC3 textures
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0u;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: point clamp sampler for R32_FLOAT shadow map
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1u;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3u;
    rs_desc.pParameters = root_params;
    rs_desc.NumStaticSamplers = 2u;
    rs_desc.pStaticSamplers = samplers;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rs_blob;
    err_blob.Reset();
    hr = ::D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                       rs_blob.Put(), err_blob.Put());
    if (FAILED(hr) || !rs_blob) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("D3D12SerializeRootSignature", hr);
    }

    ComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                     IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("CreateRootSignature", hr);
    }

    // 9. Input Layout & Pipeline States (Pass 1 Shadow PSO + Pass 2 Main Lit PSO).
    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow_pso_desc{};
    shadow_pso_desc.pRootSignature = root_sig.Get();
    shadow_pso_desc.VS = {shadow_vs->GetBufferPointer(), shadow_vs->GetBufferSize()};
    shadow_pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    shadow_pso_desc.BlendState.IndependentBlendEnable = FALSE;
    shadow_pso_desc.SampleMask = 0xFFFFFFFFu;
    shadow_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    shadow_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    shadow_pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    shadow_pso_desc.RasterizerState.DepthBias = 1000;
    shadow_pso_desc.RasterizerState.DepthBiasClamp = 0.0f;
    shadow_pso_desc.RasterizerState.SlopeScaledDepthBias = 1.6f;
    shadow_pso_desc.RasterizerState.DepthClipEnable = TRUE;
    shadow_pso_desc.DepthStencilState.DepthEnable = TRUE;
    shadow_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    shadow_pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    shadow_pso_desc.DepthStencilState.StencilEnable = FALSE;
    shadow_pso_desc.InputLayout = {input_layout, static_cast<UINT>(sizeof(input_layout) / sizeof(input_layout[0]))};
    shadow_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadow_pso_desc.NumRenderTargets = 0u;
    shadow_pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadow_pso_desc.SampleDesc.Count = 1u;

    ComPtr<ID3D12PipelineState> shadow_pso;
    hr = device->CreateGraphicsPipelineState(&shadow_pso_desc, IID_PPV_ARGS(shadow_pso.Put()));
    if (FAILED(hr) || !shadow_pso) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("CreateGraphicsPipelineState(Shadow)", hr);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC main_pso_desc = shadow_pso_desc;
    main_pso_desc.VS = {main_vs->GetBufferPointer(), main_vs->GetBufferSize()};
    main_pso_desc.PS = {main_ps->GetBufferPointer(), main_ps->GetBufferSize()};
    main_pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    main_pso_desc.RasterizerState.DepthBias = 0;
    main_pso_desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    main_pso_desc.NumRenderTargets = 1u;
    main_pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    main_pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    ComPtr<ID3D12PipelineState> main_pso;
    hr = device->CreateGraphicsPipelineState(&main_pso_desc, IID_PPV_ARGS(main_pso.Put()));
    if (FAILED(hr) || !main_pso) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("CreateGraphicsPipelineState(Main)", hr);
    }

    // 10. Descriptor Heaps (1 RTV, 2 DSV [main + shadow], 3 SRV [BC1 floor, BC3 metal, shadow map]).
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 1u;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap.Put())))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateDescriptorHeap(RTV)");
        }
        hd.NumDescriptors = 2u;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsv_heap.Put())))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateDescriptorHeap(DSV)");
        }
        hd.NumDescriptors = 3u;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srv_heap.Put())))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateDescriptorHeap(SRV)");
        }
    }
    const UINT dsv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const UINT srv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 11. Render Target, Main Depth Buffer (D32_FLOAT), and Shadow Map (R32_TYPELESS -> D32_FLOAT / R32_FLOAT).
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> color_rt;
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = cfg.width;
        d.Height = cfg.height;
        d.DepthOrArraySize = 1u;
        d.MipLevels = 1u;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1u;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cv.Color[0] = 0.06f;
        cv.Color[1] = 0.09f;
        cv.Color[2] = 0.16f;
        cv.Color[3] = 1.0f;

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &d,
            D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(color_rt.Put()));
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateCommittedResource(ColorRT)", hr);
        }
        device->CreateRenderTargetView(color_rt.Get(), nullptr,
                                       rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    ComPtr<ID3D12Resource> main_depth;
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = cfg.width;
        d.Height = cfg.height;
        d.DepthOrArraySize = 1u;
        d.MipLevels = 1u;
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1u;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        cv.DepthStencil.Stencil = 0u;

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(main_depth.Put()));
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateCommittedResource(MainDepth)", hr);
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(main_depth.Get(), &dsv_desc,
                                       dsv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    ComPtr<ID3D12Resource> shadow_tex;
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kShadowMapSize;
        d.Height = kShadowMapSize;
        d.DepthOrArraySize = 1u;
        d.MipLevels = 1u;
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.SampleDesc.Count = 1u;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        cv.DepthStencil.Stencil = 0u;

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(shadow_tex.Put()));
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateCommittedResource(ShadowMap)", hr);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        shadow_dsv.ptr += dsv_inc;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(shadow_tex.Get(), &dsv_desc, shadow_dsv);

        D3D12_CPU_DESCRIPTOR_HANDLE shadow_srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
        shadow_srv.ptr += static_cast<SIZE_T>(srv_inc) * 2u;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1u;
        device->CreateShaderResourceView(shadow_tex.Get(), &srv_desc, shadow_srv);
    }

    // 12. Procedural BC1 (floor) & BC3 (metallic/emissive) block-compressed textures + GPU upload.
    const gdk3d::BcTextureData bc1_floor = gdk3d::GenerateStoneFloorBc1Texture(256u);
    const gdk3d::BcTextureData bc3_metal = gdk3d::GenerateTechMetalBc3Texture(256u);

    ComPtr<ID3D12Resource> floor_tex;
    ComPtr<ID3D12Resource> floor_upload;
    ComPtr<ID3D12Resource> metal_tex;
    ComPtr<ID3D12Resource> metal_upload;

    auto create_bc_texture = [&](const gdk3d::BcTextureData& bc,
                                 DXGI_FORMAT fmt,
                                 UINT srv_slot,
                                 ComPtr<ID3D12Resource>& gpu_tex,
                                 ComPtr<ID3D12Resource>& up_buf) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = bc.width;
        td.Height = bc.height;
        td.DepthOrArraySize = 1u;
        td.MipLevels = 1u;
        td.Format = fmt;
        td.SampleDesc.Count = 1u;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        if (FAILED(device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(gpu_tex.Put())))) {
            return false;
        }

        const uint32_t aligned_pitch = (bc.row_pitch + 255u) & ~255u;
        const uint32_t block_rows = bc.height / 4u;
        const uint64_t up_bytes = static_cast<uint64_t>(aligned_pitch) * block_rows;
        D3D12_RESOURCE_DESC ud = MakeBufferDesc(up_bytes);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up_buf.Put())))) {
            return false;
        }

        void* mapped = nullptr;
        D3D12_RANGE rr{0, 0};
        if (FAILED(up_buf->Map(0u, &rr, &mapped)) || !mapped) return false;
        auto* dst = static_cast<uint8_t*>(mapped);
        for (uint32_t row = 0; row < block_rows; ++row) {
            std::memcpy(dst + static_cast<size_t>(row) * aligned_pitch,
                        bc.bytes.data() + static_cast<size_t>(row) * bc.row_pitch,
                        bc.row_pitch);
        }
        up_buf->Unmap(0u, nullptr);

        D3D12_TEXTURE_COPY_LOCATION copy_dst{};
        copy_dst.pResource = gpu_tex.Get();
        copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_dst.SubresourceIndex = 0u;

        D3D12_TEXTURE_COPY_LOCATION copy_src{};
        copy_src.pResource = up_buf.Get();
        copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        copy_src.PlacedFootprint.Offset = 0ull;
        copy_src.PlacedFootprint.Footprint.Format = fmt;
        copy_src.PlacedFootprint.Footprint.Width = bc.width;
        copy_src.PlacedFootprint.Footprint.Height = bc.height;
        copy_src.PlacedFootprint.Footprint.Depth = 1u;
        copy_src.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

        cmd->CopyTextureRegion(&copy_dst, 0u, 0u, 0u, &copy_src, nullptr);
        D3D12_RESOURCE_BARRIER bar = MakeTransition(
            gpu_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1u, &bar);

        D3D12_CPU_DESCRIPTOR_HANDLE srv_h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv_h.ptr += static_cast<SIZE_T>(srv_inc) * srv_slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_d{};
        srv_d.Format = fmt;
        srv_d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_d.Texture2D.MipLevels = 1u;
        device->CreateShaderResourceView(gpu_tex.Get(), &srv_d, srv_h);
        return true;
    };

    if (!create_bc_texture(bc1_floor, DXGI_FORMAT_BC1_UNORM, 0u, floor_tex, floor_upload) ||
        !create_bc_texture(bc3_metal, DXGI_FORMAT_BC3_UNORM, 1u, metal_tex, metal_upload)) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("UploadBcTextures");
    }

    // 13. Build 3D multi-mesh geometry (floor, monument, 3-bone skinned banner, satellites, pillars).
    std::vector<gdk3d::SceneVertex> vertices;
    std::vector<uint32_t> indices;

    constexpr float kColNeutral[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr float kColBanner[4]  = {1.0f, 0.88f, 0.78f, 1.0f};
    constexpr float kColSatA[4]    = {0.82f, 0.94f, 1.0f, 1.0f};
    constexpr float kColSatB[4]    = {1.0f, 0.84f, 0.68f, 1.0f};
    constexpr float kColMoon[4]    = {0.95f, 1.0f, 0.75f, 1.0f};

    const gdk3d::MeshRange mesh_floor = gdk3d::AppendFloorPlane(vertices, indices, 6.5f, 4.0f);
    const gdk3d::MeshRange mesh_monument = gdk3d::AppendMonument(vertices, indices, kColNeutral);
    const gdk3d::MeshRange mesh_banner =
        gdk3d::AppendSkinnedBanner(vertices, indices, 2.35f, 0.42f, 0.045f, 12u, kColBanner);
    const gdk3d::MeshRange mesh_sphere = gdk3d::AppendUvSphere(vertices, indices, 0.46f, 24u, 16u, kColSatA);
    const gdk3d::MeshRange mesh_cube = gdk3d::AppendUnitBox(vertices, indices, 0.42f, 0.42f, 0.42f, 0.0f, kColSatB);
    const gdk3d::MeshRange mesh_moon = gdk3d::AppendUvSphere(vertices, indices, 0.22f, 16u, 12u, kColMoon);
    const gdk3d::MeshRange mesh_pillar = gdk3d::AppendUnitBox(vertices, indices, 0.36f, 1.15f, 0.36f, 1.15f, kColNeutral);

    const uint64_t vb_bytes = vertices.size() * sizeof(gdk3d::SceneVertex);
    const uint64_t ib_bytes = indices.size() * sizeof(uint32_t);

    ComPtr<ID3D12Resource> vb_res;
    ComPtr<ID3D12Resource> ib_res;
    {
        D3D12_RESOURCE_DESC vbd = MakeBufferDesc(vb_bytes);
        D3D12_RESOURCE_DESC ibd = MakeBufferDesc(ib_bytes);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &vbd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vb_res.Put()))) ||
            FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &ibd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(ib_res.Put())))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateVertexIndexBuffers");
        }
        void* p = nullptr;
        D3D12_RANGE rr{0, 0};
        if (FAILED(vb_res->Map(0u, &rr, &p)) || !p) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("MapVB");
        }
        std::memcpy(p, vertices.data(), static_cast<size_t>(vb_bytes));
        vb_res->Unmap(0u, nullptr);

        p = nullptr;
        if (FAILED(ib_res->Map(0u, &rr, &p)) || !p) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("MapIB");
        }
        std::memcpy(p, indices.data(), static_cast<size_t>(ib_bytes));
        ib_res->Unmap(0u, nullptr);
    }

    // Execute initial BC1/BC3 texture upload command list before first frame.
    if (FAILED(cmd->Close())) {
        ::CloseHandle(fence_event);
        cleanup_platform();
        return FailStep("CloseUploadCmdList");
    }
    {
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);
        wait_gpu_idle();
    }

    // 14. Constant Buffers (SceneConstants + 16 ObjectConstants slots).
    ComPtr<ID3D12Resource> scene_cb_res;
    ComPtr<ID3D12Resource> obj_cb_res;
    uint8_t* scene_cb_mapped = nullptr;
    uint8_t* obj_cb_mapped = nullptr;
    {
        D3D12_RESOURCE_DESC scd = MakeBufferDesc(sizeof(SceneConstants));
        D3D12_RESOURCE_DESC ocd = MakeBufferDesc(sizeof(ObjectConstants) * kMaxDrawObjects);
        if (FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &scd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(scene_cb_res.Put()))) ||
            FAILED(device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &ocd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(obj_cb_res.Put())))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CreateConstantBuffers");
        }
        D3D12_RANGE rr{0, 0};
        void* sp = nullptr;
        void* op = nullptr;
        if (FAILED(scene_cb_res->Map(0u, &rr, &sp)) || FAILED(obj_cb_res->Map(0u, &rr, &op))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("MapConstantBuffers");
        }
        scene_cb_mapped = static_cast<uint8_t*>(sp);
        obj_cb_mapped = static_cast<uint8_t*>(op);
    }

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb_res->GetGPUVirtualAddress();
    vbv.SizeInBytes = static_cast<UINT>(vb_bytes);
    vbv.StrideInBytes = sizeof(gdk3d::SceneVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = ib_res->GetGPUVirtualAddress();
    ibv.SizeInBytes = static_cast<UINT>(ib_bytes);
    ibv.Format = DXGI_FORMAT_R32_UINT;

    // 15. Main Animation / Interactive Render Loop.
    float cam_yaw = 0.52f;
    float cam_pitch = 0.44f;
    float cam_dist = 6.4f;
    float sim_time = 0.0f;
    bool auto_orbit = true;
    D3D12_RESOURCE_STATES color_rt_state = D3D12_RESOURCE_STATE_COMMON;

    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_prev{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_prev);

    uint32_t frames_rendered = 0u;
    uint64_t first_frame_hash = 0ull;
    uint64_t last_frame_hash = 0ull;
    std::set<uint64_t> unique_hashes;
    std::vector<uint8_t> last_rgba;
    FrameVisualMetrics last_visual{};

    struct DrawItem {
        gdk3d::MeshRange mesh;
        bool casts_shadow = true;
        uint32_t cb_slot = 0u;
    };

    while (!g_win.should_quit) {
        if (cfg.max_frames > 0 && static_cast<int>(frames_rendered) >= cfg.max_frames) {
            break;
        }

        // Pump Win32 messages
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0u, 0u, PM_REMOVE) != FALSE) {
            if (msg.message == WM_QUIT) {
                g_win.should_quit = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (g_win.should_quit) break;

        // Compute timestep (fixed deterministic dt in headless/counted mode, live QPC in windowed mode)
        float dt = 1.0f / 30.0f;
        if (!cfg.headless && cfg.max_frames <= 0) {
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            if (qpc_freq.QuadPart > 0) {
                const double sec = static_cast<double>(now.QuadPart - qpc_prev.QuadPart) /
                                   static_cast<double>(qpc_freq.QuadPart);
                dt = std::clamp(static_cast<float>(sec), 0.002f, 0.05f);
            }
            qpc_prev = now;
        }

        // Poll GameInput.dll for keyboard & Xbox gamepad orbital camera control
        float yaw_input = 0.0f;
        float pitch_input = 0.0f;
        float zoom_input = 0.0f;
        if (g_win.keys['A'] || g_win.keys[VK_LEFT]) yaw_input -= 1.0f;
        if (g_win.keys['D'] || g_win.keys[VK_RIGHT]) yaw_input += 1.0f;
        if (g_win.keys['W'] || g_win.keys[VK_UP]) pitch_input += 1.0f;
        if (g_win.keys['S'] || g_win.keys[VK_DOWN]) pitch_input -= 1.0f;
        if (g_win.keys['Q']) zoom_input += 1.0f;
        if (g_win.keys['E']) zoom_input -= 1.0f;

        if (game_input) {
            IGameInputReading* reading = nullptr;
            const GameInputKind kind =
                static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard);
            if (game_input->GetCurrentReading(kind, nullptr, &reading) == GAMEINPUT_S_OK && reading) {
                GameInputGamepadState gp{};
                if (reading->GetGamepadState(&gp)) {
                    if (std::fabs(gp.leftThumbstickX) > 0.2f) yaw_input += gp.leftThumbstickX;
                    if (std::fabs(gp.rightThumbstickX) > 0.2f) yaw_input += gp.rightThumbstickX;
                    if (std::fabs(gp.leftThumbstickY) > 0.2f) pitch_input += gp.leftThumbstickY;
                    if (gp.buttons & GameInputGamepadDPadLeft) yaw_input -= 1.0f;
                    if (gp.buttons & GameInputGamepadDPadRight) yaw_input += 1.0f;
                    if (gp.buttons & GameInputGamepadDPadUp) pitch_input += 1.0f;
                    if (gp.buttons & GameInputGamepadDPadDown) pitch_input -= 1.0f;
                    if (gp.leftTrigger > 0.2f) zoom_input += gp.leftTrigger;
                    if (gp.rightTrigger > 0.2f) zoom_input -= gp.rightTrigger;
                    if (gp.buttons & GameInputGamepadMenu) g_win.should_quit = true;
                }
                reading->Release();
            }
        }

        if (yaw_input != 0.0f || pitch_input != 0.0f) {
            auto_orbit = false;
        }
        cam_yaw += (auto_orbit ? 0.55f : yaw_input * 1.8f) * dt;
        cam_pitch = std::clamp(cam_pitch + pitch_input * 1.1f * dt, 0.14f, 1.18f);
        cam_dist = std::clamp(cam_dist + zoom_input * 2.5f * dt, 3.4f, 11.5f);
        sim_time += dt;

        // Play milestone chime on frame 6 so audio exercises multi-buffer submission cleanly.
        if (frames_rendered == 6u) {
            audio.PlayChime(587.33f, 65u);
        }

        // Camera & Directional Sun Light matrices
        const gdk3d::Vec3 cam_target{0.0f, 1.05f, 0.0f};
        const gdk3d::Vec3 cam_eye{
            cam_dist * std::cos(cam_pitch) * std::sin(cam_yaw),
            cam_target.y + cam_dist * std::sin(cam_pitch),
            cam_dist * std::cos(cam_pitch) * std::cos(cam_yaw),
        };
        const gdk3d::Mat4 view = gdk3d::Mat4LookAtRH(cam_eye, cam_target, {0.0f, 1.0f, 0.0f});
        const float aspect = static_cast<float>(cfg.width) / static_cast<float>(cfg.height);
        const gdk3d::Mat4 proj = gdk3d::Mat4PerspectiveFovRH(48.0f * gdk3d::kPi / 180.0f, aspect, 0.15f, 45.0f);
        const gdk3d::Mat4 view_proj = gdk3d::Mat4Multiply(view, proj);

        // Gently sweeping sun light so dramatic shadows project across the BC1 floor
        const float sun_angle = 0.72f + 0.18f * std::sin(sim_time * 0.7f);
        const gdk3d::Vec3 sun_dir = gdk3d::Vec3Normalize({std::cos(sun_angle) * 1.15f, 0.82f, std::sin(sun_angle) * 1.05f});
        const gdk3d::Vec3 light_eye = gdk3d::Vec3Add(cam_target, gdk3d::Vec3Scale(sun_dir, 11.5f));
        const gdk3d::Mat4 light_view = gdk3d::Mat4LookAtRH(light_eye, cam_target, {0.0f, 1.0f, 0.0f});
        const gdk3d::Mat4 light_proj = gdk3d::Mat4OrthoOffCenterRH(-6.8f, 6.8f, -6.8f, 6.8f, 1.0f, 24.0f);
        const gdk3d::Mat4 light_vp = gdk3d::Mat4Multiply(light_view, light_proj);

        SceneConstants sc{};
        sc.view_proj = view_proj;
        sc.light_view_proj = light_vp;
        sc.camera_pos[0] = cam_eye.x;
        sc.camera_pos[1] = cam_eye.y;
        sc.camera_pos[2] = cam_eye.z;
        sc.camera_pos[3] = 1.0f;
        sc.sun_dir[0] = sun_dir.x;
        sc.sun_dir[1] = sun_dir.y;
        sc.sun_dir[2] = sun_dir.z;
        sc.sun_dir[3] = 1.65f;
        sc.sun_color[0] = 1.08f;
        sc.sun_color[1] = 0.96f;
        sc.sun_color[2] = 0.82f;
        sc.sun_color[3] = 0.22f;
        const gdk3d::Vec3 rim_dir = gdk3d::Vec3Normalize({-sun_dir.x, 0.28f, -sun_dir.z});
        sc.rim_dir[0] = rim_dir.x;
        sc.rim_dir[1] = rim_dir.y;
        sc.rim_dir[2] = rim_dir.z;
        sc.rim_dir[3] = 0.36f;
        sc.rim_color[0] = 0.34f;
        sc.rim_color[1] = 0.62f;
        sc.rim_color[2] = 1.00f;
        sc.rim_color[3] = 1.0f / static_cast<float>(kShadowMapSize);
        sc.params[0] = sim_time;
        sc.params[1] = 0.0014f;
        std::memcpy(scene_cb_mapped, &sc, sizeof(sc));

        // Build hierarchical scene transforms & 3-bone skinning palette!
        std::vector<DrawItem> draws;
        draws.reserve(8u);

        auto push_rigid_obj = [&](const gdk3d::MeshRange& mesh,
                                  const gdk3d::Mat4& world,
                                  float tex_idx,
                                  float spec_pow,
                                  float spec_str,
                                  float emissive,
                                  const float tint[4],
                                  bool casts_shadow) {
            const uint32_t slot = static_cast<uint32_t>(draws.size());
            ObjectConstants oc{};
            oc.bones[0] = world;
            for (uint32_t b = 1u; b < kMaxBones; ++b) oc.bones[b] = gdk3d::Mat4Identity();
            oc.material[0] = tex_idx;
            oc.material[1] = spec_pow;
            oc.material[2] = spec_str;
            oc.material[3] = emissive;
            oc.tint[0] = tint[0];
            oc.tint[1] = tint[1];
            oc.tint[2] = tint[2];
            oc.tint[3] = tint[3];
            std::memcpy(obj_cb_mapped + static_cast<size_t>(slot) * sizeof(ObjectConstants),
                        &oc, sizeof(oc));
            draws.push_back(DrawItem{mesh, casts_shadow, slot});
        };

        // Node 0: BC1 Stone Tile Floor Plane (receives shadows, does not self-shadow-cast)
        constexpr float kFloorTint[4] = {1.0f, 1.0f, 1.0f, 0.08f};
        push_rigid_obj(mesh_floor, gdk3d::Mat4Identity(), 0.0f, 18.0f, 0.25f, 0.0f, kFloorTint, false);

        // Node 1: Rotating Central Monument / Pedestal (BC3 metallic/emissive)
        const gdk3d::Mat4 m_monument = gdk3d::Mat4RotationY(sim_time * 0.75f);
        constexpr float kMonTint[4] = {0.95f, 0.98f, 1.0f, 0.55f};
        push_rigid_obj(mesh_monument, m_monument, 1.0f, 48.0f, 0.85f, 0.55f, kMonTint, true);

        // Node 2: 3-Bone Skinned Articulated Banner parented to the Rotating Central Monument!
        {
            const uint32_t slot = static_cast<uint32_t>(draws.size());
            ObjectConstants oc{};
            // Bone 0: mounted near top of monument at (0.42, 1.72, 0.0)
            const gdk3d::Mat4 b0_local = gdk3d::Mat4Translation(0.42f, 1.72f, 0.0f);
            const gdk3d::Mat4 b0_world = gdk3d::Mat4Multiply(b0_local, m_monument);

            // Bone 1: mid-banner joint at local x = 1.15, flexing with wave angle1
            const float wave1 = 0.34f * std::sin(sim_time * 3.4f);
            const float pitch1 = 0.14f * std::cos(sim_time * 2.2f);
            const gdk3d::Mat4 b1_pivot_inv = gdk3d::Mat4Translation(-1.15f, 0.0f, 0.0f);
            const gdk3d::Mat4 b1_rot = gdk3d::Mat4Multiply(gdk3d::Mat4RotationY(wave1),
                                                           gdk3d::Mat4RotationZ(pitch1));
            const gdk3d::Mat4 b1_pivot = gdk3d::Mat4Translation(1.15f, 0.0f, 0.0f);
            const gdk3d::Mat4 b1_rel = gdk3d::Mat4Multiply(gdk3d::Mat4Multiply(b1_pivot_inv, b1_rot), b1_pivot);
            const gdk3d::Mat4 b1_world = gdk3d::Mat4Multiply(b1_rel, b0_world);

            // Bone 2: tip joint at local x = 2.05, cascading child wave off Bone 1!
            const float wave2 = 0.42f * std::sin(sim_time * 3.4f - 1.15f);
            const gdk3d::Mat4 b2_pivot_inv = gdk3d::Mat4Translation(-2.05f, 0.0f, 0.0f);
            const gdk3d::Mat4 b2_rot = gdk3d::Mat4RotationY(wave2);
            const gdk3d::Mat4 b2_pivot = gdk3d::Mat4Translation(2.05f, 0.0f, 0.0f);
            const gdk3d::Mat4 b2_rel = gdk3d::Mat4Multiply(gdk3d::Mat4Multiply(b2_pivot_inv, b2_rot), b2_pivot);
            const gdk3d::Mat4 b2_world = gdk3d::Mat4Multiply(b2_rel, b1_world);

            oc.bones[0] = b0_world;
            oc.bones[1] = b1_world;
            oc.bones[2] = b2_world;
            for (uint32_t b = 3u; b < kMaxBones; ++b) oc.bones[b] = gdk3d::Mat4Identity();

            oc.material[0] = 1.0f;   // BC3 tech-metal/emissive texture
            oc.material[1] = 36.0f;
            oc.material[2] = 0.75f;
            oc.material[3] = 0.75f;
            oc.tint[0] = 1.05f;
            oc.tint[1] = 0.62f;
            oc.tint[2] = 0.45f;
            oc.tint[3] = 0.40f;
            std::memcpy(obj_cb_mapped + static_cast<size_t>(slot) * sizeof(ObjectConstants),
                        &oc, sizeof(oc));
            draws.push_back(DrawItem{mesh_banner, true, slot});
        }

        // Node 3: Orbiting Satellite Sphere A (parent = Monument)
        const float sat_a_ang = sim_time * 1.15f;
        const gdk3d::Mat4 m_sat_a_local = gdk3d::Mat4Multiply(
            gdk3d::Mat4RotationY(sim_time * 1.8f),
            gdk3d::Mat4Translation(2.55f * std::cos(sat_a_ang),
                                   1.45f + 0.28f * std::sin(sim_time * 2.1f),
                                   2.55f * std::sin(sat_a_ang)));
        const gdk3d::Mat4 m_sat_a = gdk3d::Mat4Multiply(m_sat_a_local, m_monument);
        constexpr float kSatATint[4] = {0.42f, 0.85f, 1.05f, 0.70f};
        push_rigid_obj(mesh_sphere, m_sat_a, 1.0f, 64.0f, 1.05f, 0.60f, kSatATint, true);

        // Node 4: 3-Level Child Sub-Moon orbiting Satellite A (Root -> Monument -> SatA -> Moon)
        const float moon_ang = sim_time * 2.9f;
        const gdk3d::Mat4 m_moon_local = gdk3d::Mat4Translation(
            0.95f * std::cos(moon_ang), 0.22f * std::sin(moon_ang * 1.5f), 0.95f * std::sin(moon_ang));
        const gdk3d::Mat4 m_moon = gdk3d::Mat4Multiply(m_moon_local, m_sat_a);
        constexpr float kMoonTint[4] = {1.05f, 0.92f, 0.45f, 0.65f};
        push_rigid_obj(mesh_moon, m_moon, 1.0f, 52.0f, 0.90f, 0.80f, kMoonTint, true);

        // Node 5: Orbiting Satellite Cube B (opposite side, spinning around X & Y)
        const float sat_b_ang = sat_a_ang + gdk3d::kPi;
        const gdk3d::Mat4 m_sat_b_spin = gdk3d::Mat4Multiply(
            gdk3d::Mat4RotationX(sim_time * 1.4f),
            gdk3d::Mat4RotationY(sim_time * 1.1f));
        const gdk3d::Mat4 m_sat_b_local = gdk3d::Mat4Multiply(
            m_sat_b_spin,
            gdk3d::Mat4Translation(2.25f * std::cos(sat_b_ang),
                                   1.10f + 0.20f * std::cos(sim_time * 1.7f),
                                   2.25f * std::sin(sat_b_ang)));
        const gdk3d::Mat4 m_sat_b = gdk3d::Mat4Multiply(m_sat_b_local, m_monument);
        constexpr float kSatBTint[4] = {1.05f, 0.54f, 0.36f, 0.50f};
        push_rigid_obj(mesh_cube, m_sat_b, 1.0f, 40.0f, 0.80f, 0.45f, kSatBTint, true);

        // Nodes 6 & 7: Two Architectural Corner Pillars casting long diagonal shadows across the BC1 floor
        constexpr float kPillarTint[4] = {0.82f, 0.88f, 0.94f, 0.35f};
        push_rigid_obj(mesh_pillar, gdk3d::Mat4Translation(-3.1f, 0.0f, -2.6f),
                       1.0f, 32.0f, 0.55f, 0.25f, kPillarTint, true);
        push_rigid_obj(mesh_pillar, gdk3d::Mat4Translation(3.1f, 0.0f, -2.2f),
                       1.0f, 32.0f, 0.55f, 0.25f, kPillarTint, true);

        // Acquire Xbox frame pipeline token
        D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0ull;
        (void)device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);

        if (FAILED(cmd_alloc->Reset()) || FAILED(cmd->Reset(cmd_alloc.Get(), shadow_pso.Get()))) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("ResetCommandList");
        }

        cmd->SetGraphicsRootSignature(root_sig.Get());
        cmd->SetGraphicsRootConstantBufferView(0u, scene_cb_res->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0u, 1u, &vbv);
        cmd->IASetIndexBuffer(&ibv);

        // =====================================================================
        // PASS 1: Directional Light Shadow Map Pass (1024x1024 D32_FLOAT)
        // =====================================================================
        {
            D3D12_RESOURCE_BARRIER b = MakeTransition(
                shadow_tex.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ResourceBarrier(1u, &b);

            D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            shadow_dsv.ptr += dsv_inc;
            cmd->OMSetRenderTargets(0u, nullptr, FALSE, &shadow_dsv);
            cmd->ClearDepthStencilView(shadow_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0u, 0u, nullptr);

            D3D12_VIEWPORT svp{0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                               static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
            D3D12_RECT ssc{0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
            cmd->RSSetViewports(1u, &svp);
            cmd->RSSetScissorRects(1u, &ssc);

            const D3D12_GPU_VIRTUAL_ADDRESS obj_base = obj_cb_res->GetGPUVirtualAddress();
            for (const DrawItem& d : draws) {
                if (!d.casts_shadow) continue;
                cmd->SetGraphicsRootConstantBufferView(
                    1u, obj_base + static_cast<uint64_t>(d.cb_slot) * sizeof(ObjectConstants));
                cmd->DrawIndexedInstanced(d.mesh.index_count, 1u, d.mesh.index_start, d.mesh.base_vertex, 0u);
            }

            b = MakeTransition(
                shadow_tex.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1u, &b);
        }

        // =====================================================================
        // PASS 2: Main Lit Camera Pass (PBR/Blinn-Phong + Shadow PCF + BC1/BC3)
        // =====================================================================
        {
            D3D12_RESOURCE_BARRIER b = MakeTransition(
                color_rt.Get(), color_rt_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1u, &b);
            color_rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

            cmd->SetPipelineState(main_pso.Get());
            ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
            cmd->SetDescriptorHeaps(1u, heaps);
            cmd->SetGraphicsRootDescriptorTable(2u, srv_heap->GetGPUDescriptorHandleForHeapStart());

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const D3D12_CPU_DESCRIPTOR_HANDLE main_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
            cmd->OMSetRenderTargets(1u, &rtv, FALSE, &main_dsv);

            const float clear_col[4] = {0.06f, 0.09f, 0.16f, 1.0f};
            cmd->ClearRenderTargetView(rtv, clear_col, 0u, nullptr);
            cmd->ClearDepthStencilView(main_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0u, 0u, nullptr);

            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(cfg.width),
                              static_cast<float>(cfg.height), 0.0f, 1.0f};
            D3D12_RECT sc_rect{0, 0, static_cast<LONG>(cfg.width), static_cast<LONG>(cfg.height)};
            cmd->RSSetViewports(1u, &vp);
            cmd->RSSetScissorRects(1u, &sc_rect);

            const D3D12_GPU_VIRTUAL_ADDRESS obj_base = obj_cb_res->GetGPUVirtualAddress();
            for (const DrawItem& d : draws) {
                cmd->SetGraphicsRootConstantBufferView(
                    1u, obj_base + static_cast<uint64_t>(d.cb_slot) * sizeof(ObjectConstants));
                cmd->DrawIndexedInstanced(d.mesh.index_count, 1u, d.mesh.index_start, d.mesh.base_vertex, 0u);
            }

            b = MakeTransition(
                color_rt.Get(), color_rt_state, D3D12_RESOURCE_STATE_PRESENT);
            cmd->ResourceBarrier(1u, &b);
            color_rt_state = D3D12_RESOURCE_STATE_PRESENT;
        }

        if (FAILED(cmd->Close())) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("CloseFrameCmdList");
        }
        ID3D12CommandList* exec_lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, exec_lists);

        // Present via Xbox Series X|S ID3D12CommandQueueX::PresentX!
        ID3D12Resource* plane_res[] = {color_rt.Get()};
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = frame_token;
        plane.ResourceCount = 1u;
        plane.ppResources = plane_res;
        plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

        D3D12XBOX_PRESENT_PARAMETERS pres{};
        pres.Flags = cfg.headless ? D3D12XBOX_PRESENT_FLAG_IMMEDIATE : D3D12XBOX_PRESENT_FLAG_NONE;
        pres.SyncInterval = cfg.headless ? 0u : 1u;
        pres.OutputWindow = g_win.hwnd;

        hr = queue->PresentX(1u, &plane, &pres);
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            cleanup_platform();
            return FailStep("PresentX", hr);
        }

        wait_gpu_idle();
        ++frames_rendered;

        // Capture PresentX readback hash & pixels in headless / verification modes
        if (cfg.headless || !cfg.stats_out.empty() || !cfg.ppm_out.empty()) {
            uint64_t hash = 0ull;
            uint32_t rw = 0u;
            uint32_t rh = 0u;
            if (SUCCEEDED(OpenScarlettD3D12GetLastPresentHash(queue.Get(), &hash, &rw, &rh)) &&
                hash != 0ull) {
                if (first_frame_hash == 0ull) first_frame_hash = hash;
                last_frame_hash = hash;
                unique_hashes.insert(hash);

                const bool need_pixels = (cfg.max_frames > 0 && static_cast<int>(frames_rendered) == cfg.max_frames) ||
                                         (frames_rendered == 1u);
                if (need_pixels && rw > 0u && rh > 0u) {
                    const uint32_t need_bytes = rw * rh * 4u;
                    last_rgba.resize(need_bytes);
                    uint32_t copied = 0u;
                    if (SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
                            queue.Get(), last_rgba.data(), need_bytes, &copied)) &&
                        copied == need_bytes) {
                        last_visual = AnalyzeFrameRgba(last_rgba, rw, rh);
                    }
                }
            }
        }
    }

    // Flush blank present before teardown (standard Xbox GDK pattern)
    (void)queue->PresentX(0u, nullptr, nullptr);
    wait_gpu_idle();

    // 16. Write & verify S:\scene_checkpoint.txt via Win32 CreateFileW (routed through OpenScarlett VFS!).
    int checkpoint_written = 0;
    {
        char ckpt[1024]{};
        const int n = std::snprintf(
            ckpt, sizeof(ckpt),
            "title=gdk_3d_scene\n"
            "title_id=0x%08X\n"
            "gpu_backend_env=%s\n"
            "adapter=%s\n"
            "is_warp=%u\n"
            "frames_rendered=%u\n"
            "unique_frame_hashes=%zu\n"
            "last_frame_hash=0x%016llX\n"
            "cam_yaw=%.4f\n"
            "cam_pitch=%.4f\n",
            title_id,
            gpu_backend_env.c_str(),
            gpu_info.AdapterName,
            gpu_info.IsWarp,
            frames_rendered,
            unique_hashes.size(),
            static_cast<unsigned long long>(last_frame_hash),
            static_cast<double>(cam_yaw),
            static_cast<double>(cam_pitch));
        if (n > 0 && WriteWholeFileW(L"S:\\scene_checkpoint.txt", ckpt, static_cast<size_t>(n))) {
            std::string verify_back;
            if (ReadWholeFileW(L"S:\\scene_checkpoint.txt", verify_back) &&
                verify_back.find("title=gdk_3d_scene") != std::string::npos) {
                checkpoint_written = 1;
            }
        }
    }

    // Optional P6 PPM snapshot
    if (!cfg.ppm_out.empty() && !last_rgba.empty()) {
        (void)WritePpmUtf8(cfg.ppm_out, cfg.width, cfg.height, last_rgba.data());
    }

    // 17. Write stats file if --stats-out was supplied.
    if (!cfg.stats_out.empty()) {
        char stats_buf[2048]{};
        const int n = std::snprintf(
            stats_buf, sizeof(stats_buf),
            "title=gdk_3d_scene\n"
            "title_id=0x%08X\n"
            "gpu_backend_env=%s\n"
            "adapter=%s\n"
            "is_warp=%u\n"
            "console_type=%u\n"
            "processor_count=%u\n"
            "device_type=%u\n"
            "width=%u\n"
            "height=%u\n"
            "frames_rendered=%u\n"
            "unique_frame_hashes=%zu\n"
            "first_frame_hash=0x%016llX\n"
            "last_frame_hash=0x%016llX\n"
            "non_black_pixels=%u\n"
            "unique_colors=%u\n"
            "lit_pixels=%u\n"
            "shadow_pixels=%u\n"
            "bc_textured_pixels=%u\n"
            "luminance_mean=%.3f\n"
            "luminance_stddev=%.3f\n"
            "bc1_bytes=%zu\n"
            "bc3_bytes=%zu\n"
            "audio_buffers_submitted=%llu\n"
            "checkpoint_written=%d\n"
            "checkpoint_path=S:\\scene_checkpoint.txt\n",
            title_id,
            gpu_backend_env.c_str(),
            gpu_info.AdapterName,
            gpu_info.IsWarp,
            console_type,
            proc_count,
            static_cast<unsigned>(device_type),
            cfg.width,
            cfg.height,
            frames_rendered,
            unique_hashes.size(),
            static_cast<unsigned long long>(first_frame_hash),
            static_cast<unsigned long long>(last_frame_hash),
            last_visual.non_black_pixels,
            last_visual.unique_colors,
            last_visual.lit_pixels,
            last_visual.shadow_pixels,
            last_visual.bc_textured_pixels,
            last_visual.lum_mean,
            last_visual.lum_stddev,
            bc1_floor.bytes.size(),
            bc3_metal.bytes.size(),
            static_cast<unsigned long long>(audio.buffers_submitted),
            checkpoint_written);
        if (n > 0) {
            const std::wstring wstats = Utf8ToWide(cfg.stats_out);
            if (!WriteWholeFileW(wstats.c_str(), stats_buf, static_cast<size_t>(n))) {
                ::CloseHandle(fence_event);
                cleanup_platform();
                return FailStep("WriteStatsOut");
            }
        }
    }

    // 18. Clean shutdown of all GPU & GDK resources.
    scene_cb_res->Unmap(0u, nullptr);
    obj_cb_res->Unmap(0u, nullptr);
    obj_cb_res.Reset();
    scene_cb_res.Reset();
    ib_res.Reset();
    vb_res.Reset();
    metal_upload.Reset();
    metal_tex.Reset();
    floor_upload.Reset();
    floor_tex.Reset();
    shadow_tex.Reset();
    main_depth.Reset();
    color_rt.Reset();
    srv_heap.Reset();
    dsv_heap.Reset();
    rtv_heap.Reset();
    main_pso.Reset();
    shadow_pso.Reset();
    root_sig.Reset();
    ::CloseHandle(fence_event);
    fence.Reset();
    cmd.Reset();
    cmd_alloc.Reset();
    queue.Reset();
    device.Reset();
    OpenScarlettD3D12SetPreferWarp(0);
    cleanup_platform();
    return 0;
}
