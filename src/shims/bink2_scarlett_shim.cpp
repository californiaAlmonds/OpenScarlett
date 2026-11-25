#define OPENSCARLETT_BUILDING_BINK2 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/middleware/bink2_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kModName = "Bink2Scarlett.dll";

using BinkAllocFn = void* (*)(uint64_t);
using BinkFreeFn = void (*)(void*);

struct BinkPlaneLayout {
    int32_t Allocate;
    void* Buffer;
    uint32_t BufferPitch;
};

struct BinkFramePlaneSetLayout {
    BinkPlaneLayout YPlane;
    BinkPlaneLayout cRPlane;
    BinkPlaneLayout cBPlane;
    BinkPlaneLayout APlane;
};

constexpr uint32_t kBinkMaxFrameBuffers = 2u;

struct BinkFrameBuffersLayout {
    int32_t TotalFrames;
    uint32_t YABufferWidth;
    uint32_t YABufferHeight;
    uint32_t cRcBBufferWidth;
    uint32_t cRcBBufferHeight;
    uint32_t FrameNum;
    BinkFramePlaneSetLayout Frames[kBinkMaxFrameBuffers];
};

static_assert(sizeof(void*) != 8 || sizeof(BinkPlaneLayout) == 24, "Unexpected BinkPlaneLayout size");
static_assert(sizeof(void*) != 8 || sizeof(BinkFramePlaneSetLayout) == 96, "Unexpected BinkFramePlaneSetLayout size");
static_assert(sizeof(void*) != 8 || sizeof(BinkFrameBuffersLayout) == 216, "Unexpected BinkFrameBuffersLayout size");

struct AsyncThreadState {
    bool active = false;
    bool stop_requested = false;
    const void* param = nullptr;
};

struct BinkSession {
    BinkStruct pub{};  // Must be first member so HBink == &session->pub
    openscarlett::Bink2Decoder decoder;
    openscarlett::Bink2DecodedFrame cached_frame{};
    bool has_cached_frame = false;
    bool needs_seek = false;
    bool async_frame_done = false;
    bool will_loop = false;
    bool has_registered_fb = false;
    BinkFrameBuffersLayout registered_fb{};
    std::unordered_map<uint32_t, int32_t> track_volumes;
    uint32_t total_decoded_frames = 0;
};

struct BinkGlobalState {
    std::mutex mutex;
    std::string last_error;
    std::unordered_map<HBink, std::unique_ptr<BinkSession>> sessions;
    BinkAllocFn alloc_fn = nullptr;
    BinkFreeFn free_fn = nullptr;
    const void* os_file_callbacks = nullptr;
    void* sound_open_fn = nullptr;
    uint64_t sound_param = 0;
    std::unordered_map<int32_t, AsyncThreadState> async_threads;
    std::mutex gpu_mutex;
    std::unique_ptr<openscarlett::Bink2GpuColorConverter> gpu_hw;
    std::unique_ptr<openscarlett::Bink2GpuColorConverter> gpu_warp;
};

BinkGlobalState& GetBinkState() {
    static BinkGlobalState* s = new BinkGlobalState();
    return *s;
}

void SetLastErrorStr(const std::string& msg) {
    auto& st = GetBinkState();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.last_error = msg;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

inline void RecordErr(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Error, OS_SHIM_E_FAIL, -1);
}

BinkSession* LookupSessionLocked(BinkGlobalState& st, HBink bink) {
    if (!bink) return nullptr;
    auto it = st.sessions.find(bink);
    return it == st.sessions.end() ? nullptr : it->second.get();
}

bool IsSyntheticSession(HBink bink) {
    if (!bink) return false;
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    return g.sessions.find(bink) != g.sessions.end();
}

void PopulateRegisteredFrameBuffersFromRgba(BinkSession& s) {
    if (!s.has_registered_fb || !s.has_cached_frame) return;
    const uint32_t w = s.cached_frame.width;
    const uint32_t h = s.cached_frame.height;
    if (w == 0u || h == 0u || s.cached_frame.pixels.size() < static_cast<size_t>(w) * h * 4u) {
        return;
    }
    const uint32_t total_slots =
        (s.registered_fb.TotalFrames > 0)
            ? std::min<uint32_t>(static_cast<uint32_t>(s.registered_fb.TotalFrames), kBinkMaxFrameBuffers)
            : 1u;
    const uint32_t slot = (total_slots > 1u) ? (s.registered_fb.FrameNum % total_slots) : 0u;
    BinkFramePlaneSetLayout& ps = s.registered_fb.Frames[slot];
    const uint8_t* rgba = s.cached_frame.pixels.data();

    auto* y_dst = static_cast<uint8_t*>(ps.YPlane.Buffer);
    auto* a_dst = static_cast<uint8_t*>(ps.APlane.Buffer);
    const uint32_t y_pitch = ps.YPlane.BufferPitch ? ps.YPlane.BufferPitch : w;
    const uint32_t a_pitch = ps.APlane.BufferPitch ? ps.APlane.BufferPitch : w;
    if (y_dst || a_dst) {
        for (uint32_t y = 0u; y < h; ++y) {
            const uint8_t* src_row = rgba + static_cast<size_t>(y) * w * 4u;
            uint8_t* yr = y_dst ? (y_dst + static_cast<size_t>(y) * y_pitch) : nullptr;
            uint8_t* ar = a_dst ? (a_dst + static_cast<size_t>(y) * a_pitch) : nullptr;
            for (uint32_t x = 0u; x < w; ++x) {
                const int32_t r = src_row[x * 4u + 0u];
                const int32_t g = src_row[x * 4u + 1u];
                const int32_t b = src_row[x * 4u + 2u];
                if (yr) {
                    const int32_t yv = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
                    yr[x] = static_cast<uint8_t>(std::clamp(yv, 0, 255));
                }
                if (ar) {
                    ar[x] = src_row[x * 4u + 3u];
                }
            }
        }
    }

    auto* cr_dst = static_cast<uint8_t*>(ps.cRPlane.Buffer);
    auto* cb_dst = static_cast<uint8_t*>(ps.cBPlane.Buffer);
    const uint32_t cw = (w + 1u) / 2u;
    const uint32_t ch = (h + 1u) / 2u;
    const uint32_t cr_pitch = ps.cRPlane.BufferPitch ? ps.cRPlane.BufferPitch : cw;
    const uint32_t cb_pitch = ps.cBPlane.BufferPitch ? ps.cBPlane.BufferPitch : cw;
    if (cr_dst || cb_dst) {
        for (uint32_t cy = 0u; cy < ch; ++cy) {
            const uint32_t sy = std::min(cy * 2u, h - 1u);
            const uint8_t* src_row = rgba + static_cast<size_t>(sy) * w * 4u;
            uint8_t* crr = cr_dst ? (cr_dst + static_cast<size_t>(cy) * cr_pitch) : nullptr;
            uint8_t* cbr = cb_dst ? (cb_dst + static_cast<size_t>(cy) * cb_pitch) : nullptr;
            for (uint32_t cx = 0u; cx < cw; ++cx) {
                const uint32_t sx = std::min(cx * 2u, w - 1u);
                const int32_t r = src_row[sx * 4u + 0u];
                const int32_t g = src_row[sx * 4u + 1u];
                const int32_t b = src_row[sx * 4u + 2u];
                if (crr) {
                    const int32_t crv = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
                    crr[cx] = static_cast<uint8_t>(std::clamp(crv, 0, 255));
                }
                if (cbr) {
                    const int32_t cbv = ((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128;
                    cbr[cx] = static_cast<uint8_t>(std::clamp(cbv, 0, 255));
                }
            }
        }
    }
    if (total_slots > 1u) {
        s.registered_fb.FrameNum = (slot + 1u) % total_slots;
    }
}

#if defined(_WIN32)
struct NativeBink2Api {
    HMODULE mod = nullptr;
    HBink(OS_SHIM_CALL* pBinkOpen)(const char*, uint32_t) = nullptr;
    HBink(OS_SHIM_CALL* pBinkOpenWithOptions)(const char*, const void*, uint32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkClose)(HBink) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkDoFrame)(HBink) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkDoFrameAsyncMulti)(HBink, uint32_t*, int32_t) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkDoFrameAsyncWait)(HBink, int64_t) = nullptr;
    void(OS_SHIM_CALL* pBinkNextFrame)(HBink) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkWait)(HBink) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkCopyToBuffer)(HBink, void*, int32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkGoto)(HBink, uint32_t, uint32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkGetSummary)(HBink, BinkSummary*) = nullptr;
    const char*(OS_SHIM_CALL* pBinkGetError)() = nullptr;
    void(OS_SHIM_CALL* pBinkSetSoundOnOff)(HBink, int32_t) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkPause)(HBink, int32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkFreeGlobals)() = nullptr;
    void(OS_SHIM_CALL* pBinkGetFrameBuffersInfo)(HBink, void*) = nullptr;
    uint32_t(OS_SHIM_CALL* pBinkGetKeyFrame)(HBink, uint32_t, int32_t) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkGetPlatformInfo)(int32_t, void*) = nullptr;
    void(OS_SHIM_CALL* pBinkRegisterFrameBuffers)(HBink, void*) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkRequestStopAsyncThreadsMulti)(uint32_t*, int32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkSetMemory)(BinkAllocFn, BinkFreeFn) = nullptr;
    void(OS_SHIM_CALL* pBinkSetOSFileCallbacks)(const void*) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkSetSoundSystem)(void*, uint64_t) = nullptr;
    void(OS_SHIM_CALL* pBinkSetVolume)(HBink, uint32_t, int32_t) = nullptr;
    void(OS_SHIM_CALL* pBinkSetWillLoop)(HBink, int32_t) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkShouldSkip)(HBink) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkStartAsyncThread)(int32_t, const void*) = nullptr;
    int32_t(OS_SHIM_CALL* pBinkWaitStopAsyncThreadsMulti)(uint32_t*, int32_t) = nullptr;
};

bool AreSameFileOnDiskW(const wchar_t* path_a, const wchar_t* path_b) noexcept {
    if (!path_a || !path_b) return false;
    HANDLE ha = ::CreateFileW(path_a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = ::CreateFileW(path_b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hb == INVALID_HANDLE_VALUE) {
        ::CloseHandle(ha);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION ia{}, ib{};
    const BOOL oka = ::GetFileInformationByHandle(ha, &ia);
    const BOOL okb = ::GetFileInformationByHandle(hb, &ib);
    ::CloseHandle(ha);
    ::CloseHandle(hb);
    if (!oka || !okb) return false;
    return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh &&
           ia.nFileIndexLow == ib.nFileIndexLow;
}

const NativeBink2Api* GetNativeGuestBink2() noexcept {
    static std::once_flag s_once;
    static NativeBink2Api s_api{};
    std::call_once(s_once, []() {
        wchar_t force_shim[16]{};
        if (::GetEnvironmentVariableW(L"OPENSCARLETT_BINK2_FORCE_SHIM", force_shim, 16u) > 0u &&
            force_shim[0] == L'1') {
            return;
        }
        HMODULE self_hm = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetBinkState), &self_hm);
        wchar_t self_path[MAX_PATH]{};
        if (self_hm) {
            ::GetModuleFileNameW(self_hm, self_path, MAX_PATH);
        }

        std::vector<std::wstring> cand_dirs;
        const wchar_t* env_vars[] = {L"OPENSCARLETT_GUEST_DIR", L"OPENSCARLETT_VFS_GAME_ROOT"};
        for (const wchar_t* ev : env_vars) {
            wchar_t dir_buf[MAX_PATH]{};
            const DWORD n = ::GetEnvironmentVariableW(ev, dir_buf, MAX_PATH);
            if (n > 0u && n < MAX_PATH) {
                cand_dirs.emplace_back(dir_buf, n);
            }
        }
        wchar_t exe_path[MAX_PATH]{};
        const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (exe_len > 0u && exe_len < MAX_PATH) {
            std::wstring exe_dir(exe_path, exe_len);
            const size_t slash = exe_dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                exe_dir.resize(slash);
                cand_dirs.push_back(std::move(exe_dir));
            }
        }

        // Pre-load OpenScarlett's xmem.dll & XAudio2_9.dll shims so native Bink2Scarlett.dll
        // binds Ordinal 1 (XAudio2Create) and XMemAlloc/XMemFree to OpenScarlett's shims.
        wchar_t shim_dir[MAX_PATH]{};
        const DWORD shim_len = ::GetEnvironmentVariableW(L"OPENSCARLETT_SHIM_DIR", shim_dir, MAX_PATH);
        if (shim_len > 0u && shim_len < MAX_PATH) {
            std::wstring sdir(shim_dir, shim_len);
            while (!sdir.empty() && (sdir.back() == L'\\' || sdir.back() == L'/')) {
                sdir.pop_back();
            }
            if (!::GetModuleHandleW(L"xmem.dll")) {
                (void)::LoadLibraryExW((sdir + L"\\xmem.dll").c_str(), nullptr, 0u);
            }
            if (!::GetModuleHandleW(L"XAudio2_9.dll")) {
                (void)::LoadLibraryExW((sdir + L"\\XAudio2_9.dll").c_str(), nullptr, 0u);
            }
        }

        HMODULE loaded = nullptr;
        std::wstring loaded_path;
        for (std::wstring cand : cand_dirs) {
            while (!cand.empty() && (cand.back() == L'\\' || cand.back() == L'/')) {
                cand.pop_back();
            }
            if (cand.empty()) continue;
            cand += L"\\Bink2Scarlett.dll";
            const DWORD attr = ::GetFileAttributesW(cand.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                continue;
            }
            if (self_path[0] != L'\0' && AreSameFileOnDiskW(self_path, cand.c_str())) {
                continue;
            }
            HMODULE hm = ::LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (hm && hm != self_hm) {
                loaded = hm;
                loaded_path = cand;
                break;
            }
            if (hm && hm == self_hm) {
                ::FreeLibrary(hm);
            }
        }
        if (!loaded) return;

        if (HMODULE hook_hm = ::GetModuleHandleW(L"openscarlett_hook.dll")) {
            using PfnRegPdata = int(__cdecl*)(HMODULE);
            using PfnPatchMod = int(__cdecl*)(HMODULE);
            if (auto reg_fn = reinterpret_cast<PfnRegPdata>(
                    ::GetProcAddress(hook_hm, "OpenScarlettHook_RegisterModuleExceptionTable"))) {
                (void)reg_fn(loaded);
            }
            if (auto patch_fn = reinterpret_cast<PfnPatchMod>(
                    ::GetProcAddress(hook_hm, "OpenScarlettHook_PatchModuleImports"))) {
                (void)patch_fn(loaded);
            }
        }
        std::fprintf(stderr, "[OpenScarlett][Bink2] Loaded native guest Bink2Scarlett.dll (%ls) -> %p\n",
                     loaded_path.c_str(), static_cast<void*>(loaded));
        std::fflush(stderr);

        s_api.mod = loaded;
        #define OS_BIND_BINK(fn) \
            s_api.p##fn = reinterpret_cast<decltype(s_api.p##fn)>(::GetProcAddress(loaded, #fn))
        OS_BIND_BINK(BinkOpen);
        OS_BIND_BINK(BinkOpenWithOptions);
        OS_BIND_BINK(BinkClose);
        OS_BIND_BINK(BinkDoFrame);
        OS_BIND_BINK(BinkDoFrameAsyncMulti);
        OS_BIND_BINK(BinkDoFrameAsyncWait);
        OS_BIND_BINK(BinkNextFrame);
        OS_BIND_BINK(BinkWait);
        OS_BIND_BINK(BinkCopyToBuffer);
        OS_BIND_BINK(BinkGoto);
        OS_BIND_BINK(BinkGetSummary);
        OS_BIND_BINK(BinkGetError);
        OS_BIND_BINK(BinkSetSoundOnOff);
        OS_BIND_BINK(BinkPause);
        OS_BIND_BINK(BinkFreeGlobals);
        OS_BIND_BINK(BinkGetFrameBuffersInfo);
        OS_BIND_BINK(BinkGetKeyFrame);
        OS_BIND_BINK(BinkGetPlatformInfo);
        OS_BIND_BINK(BinkRegisterFrameBuffers);
        OS_BIND_BINK(BinkRequestStopAsyncThreadsMulti);
        OS_BIND_BINK(BinkSetMemory);
        OS_BIND_BINK(BinkSetOSFileCallbacks);
        OS_BIND_BINK(BinkSetSoundSystem);
        OS_BIND_BINK(BinkSetVolume);
        OS_BIND_BINK(BinkSetWillLoop);
        OS_BIND_BINK(BinkShouldSkip);
        OS_BIND_BINK(BinkStartAsyncThread);
        OS_BIND_BINK(BinkWaitStopAsyncThreadsMulti);
        #undef OS_BIND_BINK

        // Replay any global callbacks registered prior to lazy load.
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        if (s_api.pBinkSetMemory && (g.alloc_fn || g.free_fn)) {
            s_api.pBinkSetMemory(g.alloc_fn, g.free_fn);
        }
        if (s_api.pBinkSetOSFileCallbacks && g.os_file_callbacks) {
            s_api.pBinkSetOSFileCallbacks(g.os_file_callbacks);
        }
        if (s_api.pBinkSetSoundSystem && g.sound_open_fn) {
            s_api.pBinkSetSoundSystem(g.sound_open_fn, g.sound_param);
        }
    });
    return s_api.mod ? &s_api : nullptr;
}
#endif

HBink CreateSessionFromBytes(std::vector<uint8_t> bytes, uint32_t flags, const char* sym) {
    auto session = std::make_unique<BinkSession>();
    openscarlett::Status st = session->decoder.OpenFromMemory(std::move(bytes));
    if (!st.ok()) {
        SetLastErrorStr(st.message.empty() ? "Failed to open Bink2 container" : st.message);
        RecordErr(sym);
        return nullptr;
    }

    const auto& hdr = session->decoder.Header();
    session->pub.Width = hdr.width;
    session->pub.Height = hdr.height;
    session->pub.Frames = hdr.frame_count;
    session->pub.FrameNum = 1u;
    session->pub.LastFrameNum = 0u;
    session->pub.FrameRate = hdr.fps_num;
    session->pub.FrameRateDiv = hdr.fps_den;
    session->pub.OpenFlags = flags;
    session->pub.Paused = 0u;
    session->pub.SoundOn = 1u;

    HBink handle = &session->pub;
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.last_error.clear();
        g.sessions[handle] = std::move(session);
    }
    RecordOk(sym);
    return handle;
}

}  // namespace

extern "C" {

HBink OS_SHIM_CALL BinkOpen(const char* name, uint32_t flags) {
    if (!name || !*name) {
        SetLastErrorStr("BinkOpen: null or empty file path");
        RecordErr("BinkOpen");
        return nullptr;
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkOpen) {
        HBink h = nat->pBinkOpen(name, flags);
        if (h) {
            RecordOk("BinkOpen");
            return h;
        }
    }
#endif
    std::ifstream ifs(name, std::ios::binary | std::ios::ate);
    if (!ifs) {
        SetLastErrorStr(std::string("BinkOpen: unable to open file: ") + name);
        RecordErr("BinkOpen");
        return nullptr;
    }
    const std::streamoff sz = ifs.tellg();
    if (sz <= 0) {
        SetLastErrorStr("BinkOpen: empty file");
        RecordErr("BinkOpen");
        return nullptr;
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!ifs) {
        SetLastErrorStr("BinkOpen: read failed");
        RecordErr("BinkOpen");
        return nullptr;
    }
    return CreateSessionFromBytes(std::move(bytes), flags, "BinkOpen");
}

HBink OS_SHIM_CALL BinkOpenBuffer(const void* buffer, size_t bufferSize, uint32_t flags) {
    if (!buffer || bufferSize == 0u) {
        SetLastErrorStr("BinkOpenBuffer: invalid memory buffer");
        RecordErr("BinkOpenBuffer");
        return nullptr;
    }
    const auto* src = static_cast<const uint8_t*>(buffer);
    std::vector<uint8_t> bytes(src, src + bufferSize);
    return CreateSessionFromBytes(std::move(bytes), flags, "BinkOpenBuffer");
}

void OS_SHIM_CALL BinkClose(HBink bink) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkClose) {
            nat->pBinkClose(bink);
            RecordOk("BinkClose");
            return;
        }
    }
#endif
    if (bink) {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        auto it = g.sessions.find(bink);
        if (it != g.sessions.end()) {
            it->second->decoder.Close();
            g.sessions.erase(it);
        }
    }
    RecordOk("BinkClose");
}

int32_t OS_SHIM_CALL BinkDoFrame(HBink bink) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkDoFrame) {
            const int32_t rc = nat->pBinkDoFrame(bink);
            RecordOk("BinkDoFrame");
            return rc;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        g.last_error = "BinkDoFrame: invalid Bink handle";
        RecordErr("BinkDoFrame");
        return -1;
    }

    const uint32_t zero_idx = (s->pub.FrameNum > 0u) ? (s->pub.FrameNum - 1u) : 0u;
    openscarlett::Status st = s->needs_seek
        ? s->decoder.SeekToFrame(zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame)
        : s->decoder.DecodeFrame(zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);

    if (!st.ok()) {
        g.last_error = st.message.empty() ? "BinkDoFrame: decode failed" : st.message;
        RecordErr("BinkDoFrame");
        return -1;
    }

    s->needs_seek = false;
    s->has_cached_frame = true;
    s->pub.LastFrameNum = s->pub.FrameNum;
    ++s->total_decoded_frames;
    PopulateRegisteredFrameBuffersFromRgba(*s);
    g.last_error.clear();
    RecordOk("BinkDoFrame");
    return 0;
}

void OS_SHIM_CALL BinkNextFrame(HBink bink) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkNextFrame) {
            nat->pBinkNextFrame(bink);
            RecordOk("BinkNextFrame");
            return;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        RecordErr("BinkNextFrame");
        return;
    }
    if (s->pub.FrameNum >= s->pub.Frames) {
        s->pub.FrameNum = 1u;
        s->needs_seek = true;
    } else {
        ++s->pub.FrameNum;
    }
    RecordOk("BinkNextFrame");
}

int32_t OS_SHIM_CALL BinkWait(HBink bink) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkWait) {
            const int32_t rc = nat->pBinkWait(bink);
            RecordOk("BinkWait");
            return rc;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkWait");
        return 1;
    }
    RecordOk("BinkWait");
    return s->pub.Paused != 0u ? 1 : 0;
}

int32_t OS_SHIM_CALL BinkCopyToBuffer(
    HBink bink,
    void* dest,
    int32_t destPitch,
    uint32_t destHeight,
    uint32_t destX,
    uint32_t destY,
    uint32_t flags) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkCopyToBuffer) {
            const int32_t rc =
                nat->pBinkCopyToBuffer(bink, dest, destPitch, destHeight, destX, destY, flags);
            RecordOk("BinkCopyToBuffer");
            return rc;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || !dest || destPitch <= 0 || destHeight == 0u) {
        g.last_error = "BinkCopyToBuffer: invalid arguments";
        RecordErr("BinkCopyToBuffer");
        return -1;
    }

    if (!s->has_cached_frame) {
        const uint32_t zero_idx = (s->pub.FrameNum > 0u) ? (s->pub.FrameNum - 1u) : 0u;
        openscarlett::Status st = s->decoder.SeekToFrame(
            zero_idx, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);
        if (!st.ok()) {
            g.last_error = st.message.empty() ? "BinkCopyToBuffer: decode failed" : st.message;
            RecordErr("BinkCopyToBuffer");
            return -1;
        }
        s->has_cached_frame = true;
    }

    const uint32_t src_w = s->cached_frame.width;
    const uint32_t src_h = s->cached_frame.height;
    if (destY >= destHeight || static_cast<uint32_t>(destPitch) < (destX + src_w) * 4u) {
        g.last_error = "BinkCopyToBuffer: destination buffer too small for pitch/offset";
        RecordErr("BinkCopyToBuffer");
        return -1;
    }

    const uint32_t copy_rows = std::min(src_h, destHeight - destY);
    const bool want_bgra = ((flags & 0xFFu) == OS_BINKSURFACE32);
    auto* dst_base = static_cast<uint8_t*>(dest);
    const uint8_t* src_base = s->cached_frame.pixels.data();

    for (uint32_t y = 0; y < copy_rows; ++y) {
        uint8_t* dst_row =
            dst_base + static_cast<size_t>(destY + y) * static_cast<size_t>(destPitch) +
            static_cast<size_t>(destX) * 4u;
        const uint8_t* src_row = src_base + static_cast<size_t>(y) * static_cast<size_t>(src_w) * 4u;
        if (!want_bgra) {
            std::memcpy(dst_row, src_row, static_cast<size_t>(src_w) * 4u);
        } else {
            for (uint32_t x = 0; x < src_w; ++x) {
                dst_row[x * 4u + 0u] = src_row[x * 4u + 2u];
                dst_row[x * 4u + 1u] = src_row[x * 4u + 1u];
                dst_row[x * 4u + 2u] = src_row[x * 4u + 0u];
                dst_row[x * 4u + 3u] = src_row[x * 4u + 3u];
            }
        }
    }

    g.last_error.clear();
    RecordOk("BinkCopyToBuffer");
    return 0;
}

void OS_SHIM_CALL BinkGoto(HBink bink, uint32_t frame, uint32_t flags) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGoto) {
            nat->pBinkGoto(bink, frame, flags);
            RecordOk("BinkGoto");
            return;
        }
    }
#else
    (void)flags;
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s || s->pub.Frames == 0u) {
        g.last_error = "BinkGoto: invalid handle";
        RecordErr("BinkGoto");
        return;
    }
    const uint32_t clamped = std::clamp(frame == 0u ? 1u : frame, 1u, s->pub.Frames);
    s->pub.FrameNum = clamped;
    openscarlett::Status st = s->decoder.SeekToFrame(
        clamped - 1u, openscarlett::Bink2PixelFormat::Rgba8, s->cached_frame);
    if (!st.ok()) {
        s->needs_seek = true;
        g.last_error = st.message.empty() ? "BinkGoto: seek failed" : st.message;
        RecordErr("BinkGoto");
        return;
    }
    s->needs_seek = false;
    s->has_cached_frame = true;
    s->pub.LastFrameNum = clamped;
    g.last_error.clear();
    RecordOk("BinkGoto");
}

void OS_SHIM_CALL BinkGetSummary(HBink bink, BinkSummary* summary) {
    if (!summary) {
        RecordErr("BinkGetSummary");
        return;
    }
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGetSummary) {
            nat->pBinkGetSummary(bink, summary);
            RecordOk("BinkGetSummary");
            return;
        }
    }
#endif
    std::memset(summary, 0, sizeof(BinkSummary));
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkGetSummary");
        return;
    }
    const auto& hdr = s->decoder.Header();
    summary->Width = hdr.width;
    summary->Height = hdr.height;
    summary->TotalFrames = hdr.frame_count;
    summary->CurrentFrame = s->pub.FrameNum;
    summary->FrameRate = hdr.fps_num;
    summary->FrameRateDiv = hdr.fps_den;
    summary->TotalDecodedFrames = s->total_decoded_frames;
    summary->AudioTrackCount = hdr.audio_track_count;
    summary->IsBink2 = hdr.is_bink2 ? 1u : 0u;
    std::snprintf(summary->FourCC, sizeof(summary->FourCC), "%s", hdr.fourcc);
    RecordOk("BinkGetSummary");
}

const char* OS_SHIM_CALL BinkGetError(void) {
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGetError) {
        const char* ne = nat->pBinkGetError();
        if (ne && *ne) {
            RecordOk("BinkGetError");
            return ne;
        }
    }
#endif
    RecordOk("BinkGetError");
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    static thread_local std::string tls_err;
    tls_err = g.last_error;
    return tls_err.c_str();
}

void OS_SHIM_CALL BinkSetSoundOnOff(HBink bink, int32_t onOff) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetSoundOnOff) {
            nat->pBinkSetSoundOnOff(bink, onOff);
            RecordOk("BinkSetSoundOnOff");
            return;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (s) {
        s->pub.SoundOn = onOff != 0 ? 1u : 0u;
        RecordOk("BinkSetSoundOnOff");
    } else {
        RecordErr("BinkSetSoundOnOff");
    }
}

int32_t OS_SHIM_CALL BinkPause(HBink bink, int32_t pause) {
#if defined(_WIN32)
    if (bink && !IsSyntheticSession(bink)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkPause) {
            const int32_t rc = nat->pBinkPause(bink, pause);
            RecordOk("BinkPause");
            return rc;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bink);
    if (!s) {
        RecordErr("BinkPause");
        return 0;
    }
    s->pub.Paused = pause != 0 ? 1u : 0u;
    RecordOk("BinkPause");
    return static_cast<int32_t>(s->pub.Paused);
}

// Live GPU YUV420p(+A) -> RGBA8/BGRA8 compute-shader conversion export (BT.709 / BT.601;
// supports HW GPU when preferWarp==0 and D3D12 WARP when preferWarp!=0, with CPU fallback).
OS_BINK2_API int32_t OS_SHIM_CALL BinkConvertYuvToRgbaGpu(
    uint32_t width,
    uint32_t height,
    const uint8_t* planeY,
    uint32_t pitchY,
    const uint8_t* planeU,
    uint32_t pitchU,
    const uint8_t* planeV,
    uint32_t pitchV,
    const uint8_t* planeA,
    uint32_t pitchA,
    uint32_t matrixMode,
    uint32_t pixelFormat,
    uint32_t preferWarp,
    uint8_t* outPixels,
    size_t outPixelsCapacity,
    uint32_t* outUsedGpu) {
    if (outUsedGpu) *outUsedGpu = 0u;
    if (!outPixels || width == 0u || height == 0u) {
        SetLastErrorStr("BinkConvertYuvToRgbaGpu: invalid arguments");
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }
    const uint64_t required = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
    if (required > outPixelsCapacity) {
        SetLastErrorStr("BinkConvertYuvToRgbaGpu: output buffer too small");
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }

    auto& g = GetBinkState();
    std::lock_guard<std::mutex> glk(g.gpu_mutex);
    auto& conv_ptr = (preferWarp != 0u) ? g.gpu_warp : g.gpu_hw;
    if (!conv_ptr) {
        conv_ptr = std::make_unique<openscarlett::Bink2GpuColorConverter>();
        (void)conv_ptr->Initialize(preferWarp != 0u, /*allow_cpu_fallback=*/true);
    }

    std::vector<uint8_t> rgba;
    bool used_gpu = false;
    const auto mat = (matrixMode != 0u)
                         ? openscarlett::Bink2ColorMatrix::Bt601
                         : openscarlett::Bink2ColorMatrix::Bt709;
    const auto pfmt = (pixelFormat != 0u)
                          ? openscarlett::Bink2PixelFormat::Bgra8
                          : openscarlett::Bink2PixelFormat::Rgba8;
    const openscarlett::Status st = conv_ptr->ConvertYuv420pToRgba(
        width, height, planeY, pitchY, planeU, pitchU, planeV, pitchV, planeA, pitchA,
        mat, pfmt, rgba, &used_gpu);
    if (!st.ok() || rgba.size() != static_cast<size_t>(required)) {
        SetLastErrorStr(st.message.empty() ? "BinkConvertYuvToRgbaGpu failed" : st.message);
        RecordErr("BinkConvertYuvToRgbaGpu");
        return -1;
    }
    std::memcpy(outPixels, rgba.data(), rgba.size());
    if (outUsedGpu) *outUsedGpu = used_gpu ? 1u : 0u;
    RecordOk("BinkConvertYuvToRgbaGpu");
    return 0;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkGetGpuConverterStats(
    uint32_t preferWarp,
    uint32_t* outUsingGpu,
    uint32_t* outIsWarp,
    uint64_t* outGpuConversions,
    uint64_t* outCpuFallbacks) {
    if (outUsingGpu) *outUsingGpu = 0u;
    if (outIsWarp) *outIsWarp = 0u;
    if (outGpuConversions) *outGpuConversions = 0ull;
    if (outCpuFallbacks) *outCpuFallbacks = 0ull;
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> glk(g.gpu_mutex);
    auto& conv_ptr = (preferWarp != 0u) ? g.gpu_warp : g.gpu_hw;
    if (!conv_ptr) {
        RecordOk("BinkGetGpuConverterStats");
        return 0;
    }
    const auto st = conv_ptr->Stats();
    if (outUsingGpu) *outUsingGpu = st.using_gpu ? 1u : 0u;
    if (outIsWarp) *outIsWarp = st.is_warp ? 1u : 0u;
    if (outGpuConversions) *outGpuConversions = st.gpu_conversions;
    if (outCpuFallbacks) *outCpuFallbacks = st.cpu_fallback_conversions;
    RecordOk("BinkGetGpuConverterStats");
    return 0;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkDoFrameAsyncMulti(
    HBink bnk,
    uint32_t* threads,
    int32_t thread_count) {
    if (!bnk) {
        SetLastErrorStr("BinkDoFrameAsyncMulti: invalid Bink handle");
        RecordErr("BinkDoFrameAsyncMulti");
        return 0;
    }
#if defined(_WIN32)
    if (!IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkDoFrameAsyncMulti) {
            const int32_t rc = nat->pBinkDoFrameAsyncMulti(bnk, threads, thread_count);
            if (rc != 0) {
                RecordOk("BinkDoFrameAsyncMulti");
            } else {
                RecordErr("BinkDoFrameAsyncMulti");
            }
            return rc;
        }
    }
#endif
    (void)threads;
    (void)thread_count;
    const int32_t rc = BinkDoFrame(bnk);
    if (rc != 0) {
        RecordErr("BinkDoFrameAsyncMulti");
        return 0;
    }
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        if (BinkSession* s = LookupSessionLocked(g, bnk)) {
            s->async_frame_done = true;
        }
    }
    RecordOk("BinkDoFrameAsyncMulti");
    return 1;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkDoFrameAsyncWait(HBink bnk, int64_t us) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkDoFrameAsyncWait) {
            const int32_t rc = nat->pBinkDoFrameAsyncWait(bnk, us);
            RecordOk("BinkDoFrameAsyncWait");
            return rc;
        }
    }
#endif
    (void)us;
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (s) {
        s->async_frame_done = true;
        RecordOk("BinkDoFrameAsyncWait");
    } else {
        RecordErr("BinkDoFrameAsyncWait");
    }
    // Always return 1 (complete) so callers never spinloop waiting on async decode.
    return 1;
}

OS_BINK2_API void OS_SHIM_CALL BinkFreeGlobals(void) {
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkFreeGlobals) {
        nat->pBinkFreeGlobals();
    }
#endif
    {
        auto& g = GetBinkState();
        std::scoped_lock lk(g.mutex, g.gpu_mutex);
        for (auto& kv : g.sessions) {
            if (kv.second) {
                kv.second->decoder.Close();
            }
        }
        g.sessions.clear();
        g.last_error.clear();
        g.alloc_fn = nullptr;
        g.free_fn = nullptr;
        g.os_file_callbacks = nullptr;
        g.sound_open_fn = nullptr;
        g.sound_param = 0;
        g.async_threads.clear();
        if (g.gpu_hw) {
            g.gpu_hw->Shutdown();
            g.gpu_hw.reset();
        }
        if (g.gpu_warp) {
            g.gpu_warp->Shutdown();
            g.gpu_warp.reset();
        }
    }
    RecordOk("BinkFreeGlobals");
}

OS_BINK2_API void OS_SHIM_CALL BinkGetFrameBuffersInfo(HBink bnk, void* fbset) {
    if (!fbset) {
        RecordErr("BinkGetFrameBuffersInfo");
        return;
    }
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGetFrameBuffersInfo) {
            nat->pBinkGetFrameBuffersInfo(bnk, fbset);
            RecordOk("BinkGetFrameBuffersInfo");
            return;
        }
    }
#endif
    std::memset(fbset, 0, sizeof(BinkFrameBuffersLayout));
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (!s) {
        RecordErr("BinkGetFrameBuffersInfo");
        return;
    }

    const uint32_t width = s->pub.Width;
    const uint32_t height = s->pub.Height;
    const uint32_t chroma_w = (width > 0u) ? ((width + 1u) / 2u) : 0u;
    const uint32_t chroma_h = (height > 0u) ? ((height + 1u) / 2u) : 0u;
    const uint32_t y_pitch = (width + 15u) & ~15u;
    const uint32_t c_pitch = (chroma_w + 15u) & ~15u;

    auto* out = static_cast<BinkFrameBuffersLayout*>(fbset);
    out->TotalFrames = 1;
    out->YABufferWidth = width;
    out->YABufferHeight = height;
    out->cRcBBufferWidth = chroma_w;
    out->cRcBBufferHeight = chroma_h;
    out->FrameNum = 0u;

    out->Frames[0].YPlane.Allocate = 1;
    out->Frames[0].YPlane.Buffer = nullptr;
    out->Frames[0].YPlane.BufferPitch = y_pitch;

    out->Frames[0].cRPlane.Allocate = 1;
    out->Frames[0].cRPlane.Buffer = nullptr;
    out->Frames[0].cRPlane.BufferPitch = c_pitch;

    out->Frames[0].cBPlane.Allocate = 1;
    out->Frames[0].cBPlane.Buffer = nullptr;
    out->Frames[0].cBPlane.BufferPitch = c_pitch;

    out->Frames[0].APlane.Allocate = 0;
    out->Frames[0].APlane.Buffer = nullptr;
    out->Frames[0].APlane.BufferPitch = 0u;

    RecordOk("BinkGetFrameBuffersInfo");
}

OS_BINK2_API uint32_t OS_SHIM_CALL BinkGetKeyFrame(
    HBink bnk,
    uint32_t frame,
    int32_t flags) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGetKeyFrame) {
            const uint32_t kf = nat->pBinkGetKeyFrame(bnk, frame, flags);
            RecordOk("BinkGetKeyFrame");
            return kf;
        }
    }
#endif
    (void)flags;
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    uint32_t keyframe = (frame > 0u) ? frame : 1u;
    if (s) {
        if (s->pub.Frames > 0u && keyframe > s->pub.Frames) {
            keyframe = s->pub.Frames;
        }
        RecordOk("BinkGetKeyFrame");
    } else {
        RecordErr("BinkGetKeyFrame");
    }
    return keyframe;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkGetPlatformInfo(int32_t bink_enum, void* output) {
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkGetPlatformInfo) {
        const int32_t rc = nat->pBinkGetPlatformInfo(bink_enum, output);
        RecordOk("BinkGetPlatformInfo");
        return rc;
    }
#endif
    (void)bink_enum;
    if (output) {
        *static_cast<uint32_t*>(output) = 0u;
    }
    RecordOk("BinkGetPlatformInfo");
    return 1;
}

OS_BINK2_API HBink OS_SHIM_CALL BinkOpenWithOptions(
    const char* name,
    const void* open_options,
    uint32_t flags) {
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkOpenWithOptions) {
        HBink h = nat->pBinkOpenWithOptions(name, open_options, flags);
        if (h) {
            RecordOk("BinkOpenWithOptions");
        } else {
            RecordErr("BinkOpenWithOptions");
        }
        return h;
    }
#endif
    (void)open_options;
    HBink h = BinkOpen(name, flags);
    if (h) {
        RecordOk("BinkOpenWithOptions");
    } else {
        RecordErr("BinkOpenWithOptions");
    }
    return h;
}

OS_BINK2_API void OS_SHIM_CALL BinkRegisterFrameBuffers(HBink bnk, void* fbset) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkRegisterFrameBuffers) {
            nat->pBinkRegisterFrameBuffers(bnk, fbset);
            RecordOk("BinkRegisterFrameBuffers");
            return;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (!s) {
        RecordErr("BinkRegisterFrameBuffers");
        return;
    }
    if (fbset) {
        std::memcpy(&s->registered_fb, fbset, sizeof(BinkFrameBuffersLayout));
        s->has_registered_fb = true;
    } else {
        std::memset(&s->registered_fb, 0, sizeof(BinkFrameBuffersLayout));
        s->has_registered_fb = false;
    }
    RecordOk("BinkRegisterFrameBuffers");
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkRequestStopAsyncThreadsMulti(
    uint32_t* threads,
    int32_t thread_count) {
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkRequestStopAsyncThreadsMulti) {
        const int32_t rc = nat->pBinkRequestStopAsyncThreadsMulti(threads, thread_count);
        RecordOk("BinkRequestStopAsyncThreadsMulti");
        return rc;
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    if (threads && thread_count > 0) {
        for (int32_t i = 0; i < thread_count; ++i) {
            g.async_threads[static_cast<int32_t>(threads[i])].stop_requested = true;
        }
    } else if (!threads) {
        for (auto& kv : g.async_threads) {
            kv.second.stop_requested = true;
        }
    }
    RecordOk("BinkRequestStopAsyncThreadsMulti");
    return 1;
}

OS_BINK2_API void OS_SHIM_CALL BinkSetMemory(
    void* (*alloc_fn)(uint64_t),
    void (*free_fn)(void*)) {
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.alloc_fn = alloc_fn;
        g.free_fn = free_fn;
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetMemory) {
        nat->pBinkSetMemory(alloc_fn, free_fn);
    }
#endif
    RecordOk("BinkSetMemory");
}

OS_BINK2_API void OS_SHIM_CALL BinkSetOSFileCallbacks(const void* callbacks) {
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.os_file_callbacks = callbacks;
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetOSFileCallbacks) {
        nat->pBinkSetOSFileCallbacks(callbacks);
    }
#endif
    RecordOk("BinkSetOSFileCallbacks");
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkSetSoundSystem(void* open_fn, uint64_t param) {
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        g.sound_open_fn = open_fn;
        g.sound_param = param;
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetSoundSystem) {
        const int32_t rc = nat->pBinkSetSoundSystem(open_fn, param);
        RecordOk("BinkSetSoundSystem");
        return rc;
    }
#endif
    RecordOk("BinkSetSoundSystem");
    return 1;
}

OS_BINK2_API void OS_SHIM_CALL BinkSetVolume(
    HBink bnk,
    uint32_t trackid,
    int32_t volume) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetVolume) {
            nat->pBinkSetVolume(bnk, trackid, volume);
            RecordOk("BinkSetVolume");
            return;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (!s) {
        RecordErr("BinkSetVolume");
        return;
    }
    s->track_volumes[trackid] = volume;
    RecordOk("BinkSetVolume");
}

OS_BINK2_API void OS_SHIM_CALL BinkSetWillLoop(HBink bnk, int32_t onoff) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkSetWillLoop) {
            nat->pBinkSetWillLoop(bnk, onoff);
            RecordOk("BinkSetWillLoop");
            return;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (!s) {
        RecordErr("BinkSetWillLoop");
        return;
    }
    s->will_loop = (onoff != 0);
    RecordOk("BinkSetWillLoop");
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkShouldSkip(HBink bnk) {
#if defined(_WIN32)
    if (bnk && !IsSyntheticSession(bnk)) {
        if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkShouldSkip) {
            const int32_t rc = nat->pBinkShouldSkip(bnk);
            RecordOk("BinkShouldSkip");
            return rc;
        }
    }
#endif
    auto& g = GetBinkState();
    std::lock_guard<std::mutex> lk(g.mutex);
    BinkSession* s = LookupSessionLocked(g, bnk);
    if (s) {
        RecordOk("BinkShouldSkip");
    } else {
        RecordErr("BinkShouldSkip");
    }
    return 0;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkStartAsyncThread(
    int32_t thread_num,
    const void* thread_param) {
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        AsyncThreadState& slot = g.async_threads[thread_num];
        slot.active = true;
        slot.stop_requested = false;
        slot.param = thread_param;
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkStartAsyncThread) {
        const int32_t rc = nat->pBinkStartAsyncThread(thread_num, thread_param);
        RecordOk("BinkStartAsyncThread");
        return rc;
    }
#endif
    RecordOk("BinkStartAsyncThread");
    return 1;
}

OS_BINK2_API int32_t OS_SHIM_CALL BinkWaitStopAsyncThreadsMulti(
    uint32_t* threads,
    int32_t thread_count) {
    {
        auto& g = GetBinkState();
        std::lock_guard<std::mutex> lk(g.mutex);
        if (threads && thread_count > 0) {
            for (int32_t i = 0; i < thread_count; ++i) {
                AsyncThreadState& slot = g.async_threads[static_cast<int32_t>(threads[i])];
                slot.active = false;
                slot.stop_requested = false;
            }
        } else if (!threads) {
            for (auto& kv : g.async_threads) {
                kv.second.active = false;
                kv.second.stop_requested = false;
            }
        }
    }
#if defined(_WIN32)
    if (const auto* nat = GetNativeGuestBink2(); nat && nat->pBinkWaitStopAsyncThreadsMulti) {
        const int32_t rc = nat->pBinkWaitStopAsyncThreadsMulti(threads, thread_count);
        RecordOk("BinkWaitStopAsyncThreadsMulti");
        return rc;
    }
#endif
    RecordOk("BinkWaitStopAsyncThreadsMulti");
    return 1;
}

}  // extern "C"
