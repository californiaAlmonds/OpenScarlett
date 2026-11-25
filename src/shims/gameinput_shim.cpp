#define OPENSCARLETT_BUILDING_GAMEINPUT 1
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "GameInput.dll";

// Extended COM IID for hot-plug callbacks & UI navigation (preserves IGameInput primary vtable 100%)
constexpr GameInputGuid kIidGameInputEx = {
    0x5CA01001u, 0x4254u, 0x445Au, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x19}};

// Microsoft GameInputEnumerationKind values
constexpr uint32_t kEnumNone = 0u;
constexpr uint32_t kEnumAsync = 1u;
constexpr uint32_t kEnumBlocking = 2u;

// Microsoft GameInputUiNavigationButtons bitmask
constexpr uint32_t kUiNavNone = 0x00000000u;
constexpr uint32_t kUiNavMenu = 0x00000001u;
constexpr uint32_t kUiNavView = 0x00000002u;
constexpr uint32_t kUiNavAccept = 0x00000004u;
constexpr uint32_t kUiNavCancel = 0x00000008u;
constexpr uint32_t kUiNavUp = 0x00000010u;
constexpr uint32_t kUiNavDown = 0x00000020u;
constexpr uint32_t kUiNavLeft = 0x00000040u;
constexpr uint32_t kUiNavRight = 0x00000080u;
constexpr uint32_t kUiNavContext1 = 0x00000100u;
constexpr uint32_t kUiNavContext2 = 0x00000200u;
constexpr uint32_t kUiNavPageUp = 0x00001000u;
constexpr uint32_t kUiNavPageDown = 0x00002000u;
constexpr uint32_t kUiNavScrollUp = 0x00010000u;
constexpr uint32_t kUiNavScrollDown = 0x00020000u;
constexpr uint32_t kUiNavScrollLeft = 0x00040000u;
constexpr uint32_t kUiNavScrollRight = 0x00080000u;

struct OsGameInputUiNavigationState {
    uint32_t buttons;
};

struct OsGameInputRumbleTelemetry {
    GameInputRumbleParams lastParams;
    uint16_t xinputLeftMotor;
    uint16_t xinputRightMotor;
    uint32_t activeSlot;
    uint64_t updateCount;
    uint64_t impulseTriggerUpdateCount;
    uint64_t lastTimestampUs;
};

struct OsGameInputUserAssociationEvent {
    uint8_t deviceId[32];
    uint64_t oldUserLocalId;
    uint64_t newUserLocalId;
    uint64_t timestampUs;
    uint64_t eventSequence;
};

using OsGameInputUserAssociationCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    void* context,
    const OsGameInputUserAssociationEvent* event);

using OsGameInputDeviceCallback = GameInputDeviceCallback;

static_assert(offsetof(GameInputDeviceInfo, deviceId) == 0x20, "deviceId offset");
static_assert(offsetof(GameInputDeviceInfo, deviceRootId) == 0x40, "deviceRootId offset");
static_assert(offsetof(GameInputDeviceInfo, deviceFamily) == 0x60, "deviceFamily offset");
static_assert(offsetof(GameInputDeviceInfo, supportedInput) == 0x68, "supportedInput offset");
static_assert(sizeof(GameInputDeviceInfo) == 0x140, "GameInputDeviceInfo size");

struct IGameInputEx {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        OsGameInputDeviceCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual void OPENSCARLETT_GAMEINPUT_CALL StopCallback(uint64_t callbackToken) = 0;
    virtual bool OPENSCARLETT_GAMEINPUT_CALL UnregisterCallback(
        uint64_t callbackToken, uint64_t timeoutInMicroseconds) = 0;
};

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const GameInputGuid* a, const GameInputGuid& b) {
    return a && std::memcmp(a, &b, sizeof(GameInputGuid)) == 0;
}

uint64_t QpcTicksToMicros(uint64_t ticks, uint64_t freq) noexcept {
    if (freq == 0ull) {
        return 0ull;
    }
    // Overflow-free decomposition: avoids signed int64_t overflow in (ticks * 1000000) / freq
    // on Windows hosts with >10.67 days of QPC uptime (e.g. Fast Startup).
    return (ticks / freq) * 1000000ull + ((ticks % freq) * 1000000ull) / freq;
}

uint64_t QueryTimestampMicros() {
#if defined(_WIN32)
    static LARGE_INTEGER s_freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (s_freq.QuadPart > 0 && now.QuadPart >= 0) {
        return QpcTicksToMicros(
            static_cast<uint64_t>(now.QuadPart),
            static_cast<uint64_t>(s_freq.QuadPart));
    }
#endif
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ===========================================================================
// Dynamic XInput Loader (xinput1_4.dll -> xinput9_1_0.dll fallback)
// ===========================================================================
struct OsXInputGamepad {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
static_assert(sizeof(OsXInputGamepad) == 12, "XINPUT_GAMEPAD ABI mismatch");

struct OsXInputState {
    uint32_t dwPacketNumber;
    OsXInputGamepad Gamepad;
};
static_assert(sizeof(OsXInputState) == 16, "XINPUT_STATE ABI mismatch");

struct OsXInputVibration {
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};
static_assert(sizeof(OsXInputVibration) == 4, "XINPUT_VIBRATION ABI mismatch");

constexpr uint16_t kXiDpadUp = 0x0001u;
constexpr uint16_t kXiDpadDown = 0x0002u;
constexpr uint16_t kXiDpadLeft = 0x0004u;
constexpr uint16_t kXiDpadRight = 0x0008u;
constexpr uint16_t kXiStart = 0x0010u;
constexpr uint16_t kXiBack = 0x0020u;
constexpr uint16_t kXiLeftThumb = 0x0040u;
constexpr uint16_t kXiRightThumb = 0x0080u;
constexpr uint16_t kXiLeftShoulder = 0x0100u;
constexpr uint16_t kXiRightShoulder = 0x0200u;
constexpr uint16_t kXiA = 0x1000u;
constexpr uint16_t kXiB = 0x2000u;
constexpr uint16_t kXiX = 0x4000u;
constexpr uint16_t kXiY = 0x8000u;

struct XInputApi {
#if defined(_WIN32)
    using PfnGetState = DWORD(WINAPI*)(DWORD, OsXInputState*);
    using PfnSetState = DWORD(WINAPI*)(DWORD, OsXInputVibration*);
    using PfnEnable = void(WINAPI*)(BOOL);

    HMODULE module = nullptr;
    PfnGetState get_state = nullptr;
    PfnSetState set_state = nullptr;
    PfnEnable enable = nullptr;
#endif
};

const XInputApi& GetXInput() {
    static XInputApi api = []() {
        XInputApi a{};
#if defined(_WIN32)
        a.module = LoadLibraryA("xinput1_4.dll");
        if (!a.module) {
            a.module = LoadLibraryA("xinput9_1_0.dll");
        }
        if (a.module) {
            // Prefer XInputGetStateEx (ordinal #100 in xinput1_4.dll) which has the exact same
            // ABI as XInputGetState and also exposes the Guide button bit, falling back cleanly.
            a.get_state = reinterpret_cast<XInputApi::PfnGetState>(
                reinterpret_cast<void*>(GetProcAddress(a.module, MAKEINTRESOURCEA(100))));
            if (!a.get_state) {
                a.get_state = reinterpret_cast<XInputApi::PfnGetState>(
                    reinterpret_cast<void*>(GetProcAddress(a.module, "XInputGetState")));
            }
            a.set_state = reinterpret_cast<XInputApi::PfnSetState>(
                reinterpret_cast<void*>(GetProcAddress(a.module, "XInputSetState")));
            a.enable = reinterpret_cast<XInputApi::PfnEnable>(
                reinterpret_cast<void*>(GetProcAddress(a.module, "XInputEnable")));
            if (a.enable) {
                a.enable(TRUE);
            }
        }
#endif
        return a;
    }();
    return api;
}

float NormalizeStick(int16_t value, int16_t deadzone) {
    const int32_t v = static_cast<int32_t>(value);
    if (v > deadzone) {
        return std::clamp(
            static_cast<float>(v - deadzone) / static_cast<float>(32767 - deadzone), 0.0f, 1.0f);
    }
    if (v < -deadzone) {
        return std::clamp(
            static_cast<float>(v + deadzone) / static_cast<float>(32768 - deadzone), -1.0f, 0.0f);
    }
    return 0.0f;
}

float NormalizeTrigger(uint8_t value, uint8_t threshold) {
    if (value <= threshold) return 0.0f;
    return std::clamp(
        static_cast<float>(value - threshold) / static_cast<float>(255 - threshold), 0.0f, 1.0f);
}

uint32_t DeriveUiNavigationButtons(
    const GameInputGamepadState& gp,
    const std::vector<GameInputKeyState>& keys) {
    uint32_t nav = kUiNavNone;
    if (gp.buttons & GameInputGamepadMenu) nav |= kUiNavMenu;
    if (gp.buttons & GameInputGamepadView) nav |= kUiNavView;
    if (gp.buttons & GameInputGamepadA) nav |= kUiNavAccept;
    if (gp.buttons & GameInputGamepadB) nav |= kUiNavCancel;
    if (gp.buttons & GameInputGamepadX) nav |= kUiNavContext1;
    if (gp.buttons & GameInputGamepadY) nav |= kUiNavContext2;
    if ((gp.buttons & GameInputGamepadDPadUp) || gp.leftThumbstickY > 0.5f) nav |= kUiNavUp;
    if ((gp.buttons & GameInputGamepadDPadDown) || gp.leftThumbstickY < -0.5f) nav |= kUiNavDown;
    if ((gp.buttons & GameInputGamepadDPadLeft) || gp.leftThumbstickX < -0.5f) nav |= kUiNavLeft;
    if ((gp.buttons & GameInputGamepadDPadRight) || gp.leftThumbstickX > 0.5f) nav |= kUiNavRight;
    if (gp.buttons & GameInputGamepadLeftShoulder) nav |= kUiNavPageUp;
    if (gp.buttons & GameInputGamepadRightShoulder) nav |= kUiNavPageDown;
    if (gp.rightThumbstickY > 0.5f) nav |= kUiNavScrollUp;
    if (gp.rightThumbstickY < -0.5f) nav |= kUiNavScrollDown;
    if (gp.rightThumbstickX < -0.5f) nav |= kUiNavScrollLeft;
    if (gp.rightThumbstickX > 0.5f) nav |= kUiNavScrollRight;

    for (const auto& k : keys) {
        switch (k.virtualKey) {
            case 0x0Du:  // VK_RETURN
            case 0x20u:  // VK_SPACE
                nav |= kUiNavAccept;
                break;
            case 0x1Bu:  // VK_ESCAPE
            case 0x08u:  // VK_BACK
                nav |= kUiNavCancel;
                break;
            case 0x26u:  // VK_UP
                nav |= kUiNavUp;
                break;
            case 0x28u:  // VK_DOWN
                nav |= kUiNavDown;
                break;
            case 0x25u:  // VK_LEFT
                nav |= kUiNavLeft;
                break;
            case 0x27u:  // VK_RIGHT
                nav |= kUiNavRight;
                break;
            default:
                break;
        }
    }
    return nav;
}

void ApplyVirtualGamepadFallback(
    GameInputGamepadState& out_gamepad,
    const std::vector<GameInputKeyState>& keys,
    uint32_t mouse_buttons,
    int64_t mouse_dx,
    int64_t mouse_dy) {
    float kb_lx = 0.0f;
    float kb_ly = 0.0f;
    float kb_rx = 0.0f;
    float kb_ry = 0.0f;

    for (const auto& k : keys) {
        switch (k.virtualKey) {
            case 'W': kb_ly += 1.0f; break;
            case 'S': kb_ly -= 1.0f; break;
            case 'D': kb_lx += 1.0f; break;
            case 'A': kb_lx -= 1.0f; break;
            case 'I': kb_ry += 1.0f; break;
            case 'K': kb_ry -= 1.0f; break;
            case 'L': kb_rx += 1.0f; break;
            case 'J': kb_rx -= 1.0f; break;
            case 0x26u: out_gamepad.buttons |= GameInputGamepadDPadUp; break;     // VK_UP
            case 0x28u: out_gamepad.buttons |= GameInputGamepadDPadDown; break;   // VK_DOWN
            case 0x25u: out_gamepad.buttons |= GameInputGamepadDPadLeft; break;   // VK_LEFT
            case 0x27u: out_gamepad.buttons |= GameInputGamepadDPadRight; break;  // VK_RIGHT
            case 0x20u:                                                           // VK_SPACE
            case 0x0Du: out_gamepad.buttons |= GameInputGamepadA; break;          // VK_RETURN
            case 0x08u:                                                           // VK_BACK
            case 0xA2u:                                                           // VK_LCONTROL
            case 0xA3u: out_gamepad.buttons |= GameInputGamepadB; break;          // VK_RCONTROL
            case 'E': out_gamepad.buttons |= GameInputGamepadX; break;
            case 'Q': out_gamepad.buttons |= GameInputGamepadY; break;
            case 0xA0u: out_gamepad.buttons |= GameInputGamepadLeftShoulder; break;   // VK_LSHIFT
            case 0xA1u: out_gamepad.buttons |= GameInputGamepadRightShoulder; break;  // VK_RSHIFT
            case 0x1Bu: out_gamepad.buttons |= GameInputGamepadMenu; break;           // VK_ESCAPE
            case 0x09u: out_gamepad.buttons |= GameInputGamepadView; break;           // VK_TAB
            case 'Z': out_gamepad.leftTrigger = std::max(out_gamepad.leftTrigger, 1.0f); break;
            case 'C': out_gamepad.rightTrigger = std::max(out_gamepad.rightTrigger, 1.0f); break;
            default: break;
        }
    }

    if (kb_lx != 0.0f) {
        out_gamepad.leftThumbstickX = std::clamp(out_gamepad.leftThumbstickX + kb_lx, -1.0f, 1.0f);
    }
    if (kb_ly != 0.0f) {
        out_gamepad.leftThumbstickY = std::clamp(out_gamepad.leftThumbstickY + kb_ly, -1.0f, 1.0f);
    }
    if (kb_rx != 0.0f) {
        out_gamepad.rightThumbstickX = std::clamp(out_gamepad.rightThumbstickX + kb_rx, -1.0f, 1.0f);
    }
    if (kb_ry != 0.0f) {
        out_gamepad.rightThumbstickY = std::clamp(out_gamepad.rightThumbstickY + kb_ry, -1.0f, 1.0f);
    }

    // Mouse buttons & relative look delta -> virtual gamepad fallback
    if ((mouse_buttons & GameInputMouseLeftButton) != 0u) {
        out_gamepad.rightTrigger = std::max(out_gamepad.rightTrigger, 1.0f);
    }
    if ((mouse_buttons & GameInputMouseRightButton) != 0u) {
        out_gamepad.leftTrigger = std::max(out_gamepad.leftTrigger, 1.0f);
    }
    if ((mouse_buttons & GameInputMouseMiddleButton) != 0u) {
        out_gamepad.buttons |= GameInputGamepadRightThumbstick;
    }
    if ((mouse_buttons & GameInputMouseButton4) != 0u) {
        out_gamepad.buttons |= GameInputGamepadLeftShoulder;
    }
    if ((mouse_buttons & GameInputMouseButton5) != 0u) {
        out_gamepad.buttons |= GameInputGamepadRightShoulder;
    }
    if (mouse_dx != 0 && std::fabs(out_gamepad.rightThumbstickX) < 0.01f) {
        out_gamepad.rightThumbstickX = std::clamp(static_cast<float>(mouse_dx) / 25.0f, -1.0f, 1.0f);
    }
    if (mouse_dy != 0 && std::fabs(out_gamepad.rightThumbstickY) < 0.01f) {
        out_gamepad.rightThumbstickY = std::clamp(static_cast<float>(-mouse_dy) / 25.0f, -1.0f, 1.0f);
    }
}

struct XInputSlotTracker {
    static constexpr uint64_t kDisconnectedReprobeIntervalUs = 250000ull;  // 250 ms gentle probe

    std::mutex mutex;
    bool slot_connected[4]{false, false, false, false};
    uint32_t slot_last_pkt[4]{0u, 0u, 0u, 0u};
    bool slot_has_pkt[4]{false, false, false, false};
    uint64_t slot_last_probe_us[4]{0ull, 0ull, 0ull, 0ull};
    uint32_t active_slot = 0u;
    std::atomic<bool> force_reprobe{true};
    std::atomic<bool> enable_refresh_pending{false};
    std::atomic<bool> prefer_native_backend{false};
};

// ===========================================================================
// Optional Native Windows Host GameInput Backend (C:\Windows\System32\GameInput.dll)
// ===========================================================================
// Strictly opt-in via OPENSCARLETT_ENABLE_NATIVE_GAMEINPUT=1.
// Default physical gamepad path uses xinput1_4.dll (XInputGetStateEx / XInputGetState)
// because host GameInputRedist.dll v4 can return S_OK with all-zero readings for some
// third-party wired Xbox Series X|S controllers (e.g. PowerA) while XInput delivers live
// state and avoids loading GameInputRedist.dll into the guest process.
#if defined(_WIN32)
class NativeHostGameInputBackend {
public:
    static NativeHostGameInputBackend& Global() {
        // Intentionally leaked singleton so teardown never touches COM/loader-lock during ExitProcess.
        static NativeHostGameInputBackend* s_inst = new NativeHostGameInputBackend();
        return *s_inst;
    }

    static bool IsOptedIn() noexcept {
        static const bool s_opted_in = []() {
            char disable_buf[16]{};
            const DWORD dlen = ::GetEnvironmentVariableA(
                "OPENSCARLETT_DISABLE_NATIVE_GAMEINPUT", disable_buf, sizeof(disable_buf));
            if (dlen > 0u && dlen < sizeof(disable_buf) &&
                (disable_buf[0] == '1' || disable_buf[0] == 't' || disable_buf[0] == 'T')) {
                return false;
            }
            char enable_buf[16]{};
            const DWORD elen = ::GetEnvironmentVariableA(
                "OPENSCARLETT_ENABLE_NATIVE_GAMEINPUT", enable_buf, sizeof(enable_buf));
            return (elen > 0u && elen < sizeof(enable_buf) &&
                    (enable_buf[0] == '1' || enable_buf[0] == 't' || enable_buf[0] == 'T'));
        }();
        return s_opted_in;
    }

    static bool HasActiveState(const GameInputGamepadState& gp) noexcept {
        if (gp.buttons != GameInputGamepadNone) return true;
        if (gp.leftTrigger > 0.12f || gp.rightTrigger > 0.12f) return true;
        if (std::fabs(gp.leftThumbstickX) > 0.22f || std::fabs(gp.leftThumbstickY) > 0.22f) return true;
        if (std::fabs(gp.rightThumbstickX) > 0.22f || std::fabs(gp.rightThumbstickY) > 0.22f) return true;
        return false;
    }

    static bool GamepadStateEquals(
        const GameInputGamepadState& a, const GameInputGamepadState& b) noexcept {
        return a.buttons == b.buttons &&
               std::fabs(a.leftTrigger - b.leftTrigger) < 1e-4f &&
               std::fabs(a.rightTrigger - b.rightTrigger) < 1e-4f &&
               std::fabs(a.leftThumbstickX - b.leftThumbstickX) < 1e-4f &&
               std::fabs(a.leftThumbstickY - b.leftThumbstickY) < 1e-4f &&
               std::fabs(a.rightThumbstickX - b.rightThumbstickX) < 1e-4f &&
               std::fabs(a.rightThumbstickY - b.rightThumbstickY) < 1e-4f;
    }

    static void SanitizeGamepadState(GameInputGamepadState& gp) noexcept {
        auto clamp01 = [](float v) noexcept -> float {
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        };
        auto clamp11 = [](float v) noexcept -> float {
            return std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        };
        gp.leftTrigger = clamp01(gp.leftTrigger);
        gp.rightTrigger = clamp01(gp.rightTrigger);
        gp.leftThumbstickX = clamp11(gp.leftThumbstickX);
        gp.leftThumbstickY = clamp11(gp.leftThumbstickY);
        gp.rightThumbstickX = clamp11(gp.rightThumbstickX);
        gp.rightThumbstickY = clamp11(gp.rightThumbstickY);
    }

    bool IsActive() const noexcept {
        return host_gi_.load(std::memory_order_acquire) != nullptr;
    }

    bool IsAvailable() {
        if (!IsOptedIn()) {
            return false;
        }
        EnsureInitialized();
        return host_gi_.load(std::memory_order_acquire) != nullptr;
    }

    void RefreshFocusPolicy() {
        IGameInput* gi = host_gi_.load(std::memory_order_acquire);
        if (gi) {
            // Host Windows System32\GameInput.dll GameInputEnableBackgroundInput = 0x40.
            // OpenScarlett's own GameInputImpl::ShouldAllowInput() enforces guest focus policy.
            gi->SetFocusPolicy(0x00000040u);
        }
    }

    bool PollGamepad(
        uint32_t preferred_slot,
        GameInputGamepadState& out_gp,
        bool& out_has_fresh_activity,
        uint64_t* out_timestamp = nullptr) {
        out_gp = GameInputGamepadState{};
        out_has_fresh_activity = false;
        if (out_timestamp) *out_timestamp = 0ull;

        if (!IsOptedIn()) {
            return false;
        }
        EnsureInitialized();
        IGameInput* gi = host_gi_.load(std::memory_order_acquire);
        if (!gi) {
            return false;
        }

        std::vector<IGameInputDevice*> snapshot;
        IGameInputDevice* prev_active = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snapshot.reserve(connected_devices_.size());
            for (IGameInputDevice* d : connected_devices_) {
                if (d) {
                    d->AddRef();
                    snapshot.push_back(d);
                }
            }
            prev_active = active_device_;
        }

        bool found = false;
        GameInputGamepadState chosen_gp{};
        uint64_t chosen_ts = 0ull;
        IGameInputDevice* chosen_dev = nullptr;
        bool chosen_fresh = false;

        auto inspect_reading = [&](IGameInputDevice* dev, IGameInputReading* reading) {
            GameInputGamepadState st{};
            if (!reading->GetGamepadState(&st)) {
                return;
            }
            SanitizeGamepadState(st);
            const uint64_t ts = reading->GetTimestamp();
            bool state_changed = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = last_dev_state_.find(dev);
                if (it != last_dev_state_.end() && !GamepadStateEquals(st, it->second)) {
                    state_changed = true;
                }
                last_dev_state_[dev] = st;
            }
            const bool active_now = HasActiveState(st);
            // Never treat a timestamp-only tick on an all-zero neutral reading as fresh activity!
            const bool fresh_now = active_now || state_changed;

            if (!found || (fresh_now && !chosen_fresh) ||
                (fresh_now == chosen_fresh && dev != nullptr && dev == prev_active)) {
                found = true;
                chosen_gp = st;
                chosen_ts = ts;
                chosen_dev = dev;
                chosen_fresh = fresh_now;
            }
        };

        // Try preferred slot device first if in range so multi-pad slot assignment stays stable.
        if (preferred_slot < snapshot.size()) {
            IGameInputDevice* pref_dev = snapshot[preferred_slot];
            IGameInputReading* reading = nullptr;
            if (gi->GetCurrentReading(GameInputKindGamepad, pref_dev, &reading) >= 0 && reading) {
                inspect_reading(pref_dev, reading);
                reading->Release();
            }
        }
        for (size_t i = 0; i < snapshot.size(); ++i) {
            if (i == preferred_slot) continue;
            IGameInputDevice* dev = snapshot[i];
            IGameInputReading* reading = nullptr;
            if (gi->GetCurrentReading(GameInputKindGamepad, dev, &reading) >= 0 && reading) {
                inspect_reading(dev, reading);
                reading->Release();
            }
        }

        // Fallback: query aggregate/any gamepad if device callback list was momentarily empty.
        if (!found) {
            IGameInputReading* reading = nullptr;
            if (gi->GetCurrentReading(GameInputKindGamepad, nullptr, &reading) >= 0 && reading) {
                IGameInputDevice* rdev = nullptr;
                reading->GetDevice(&rdev);
                inspect_reading(rdev, reading);
                if (rdev) rdev->Release();
                reading->Release();
            }
        }

        if (found && chosen_dev != nullptr) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (std::find(connected_devices_.begin(), connected_devices_.end(), chosen_dev) !=
                connected_devices_.end()) {
                active_device_ = chosen_dev;
            }
        }

        for (IGameInputDevice* d : snapshot) {
            d->Release();
        }

        if (found) {
            out_gp = chosen_gp;
            out_has_fresh_activity = chosen_fresh;
            if (out_timestamp) *out_timestamp = chosen_ts;
        }
        return found;
    }

    bool SetRumble(const GameInputRumbleParams& params) {
        if (!host_gi_.load(std::memory_order_acquire)) {
            return false;
        }
        IGameInputDevice* target = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (active_device_ != nullptr &&
                std::find(connected_devices_.begin(), connected_devices_.end(), active_device_) !=
                    connected_devices_.end()) {
                target = active_device_;
            } else if (!connected_devices_.empty()) {
                target = connected_devices_.front();
            }
            if (target) {
                target->AddRef();
            }
        }
        if (!target) {
            return false;
        }
        target->SetRumbleState(&params);
        target->Release();
        return true;
    }

private:
    using PfnHostGameInputCreate = int32_t(OPENSCARLETT_GAMEINPUT_CALL*)(IGameInput**);

    static void OPENSCARLETT_GAMEINPUT_CALL OnHostDeviceCallback(
        uint64_t /*callbackToken*/,
        void* context,
        IGameInputDevice* device,
        uint64_t /*timestamp*/,
        uint32_t currentStatus,
        uint32_t /*previousStatus*/) {
        auto* self = static_cast<NativeHostGameInputBackend*>(context);
        if (!self || !device) {
            return;
        }
        IGameInputDevice* to_release = nullptr;
        {
            std::lock_guard<std::mutex> lk(self->mutex_);
            const bool connected = (currentStatus & GameInputDeviceConnected) != 0u;
            auto it = std::find(self->connected_devices_.begin(), self->connected_devices_.end(), device);
            if (connected) {
                if (it == self->connected_devices_.end()) {
                    device->AddRef();
                    self->connected_devices_.push_back(device);
                    if (!self->active_device_) {
                        self->active_device_ = device;
                    }
                }
            } else {
                if (it != self->connected_devices_.end()) {
                    if (self->active_device_ == device) {
                        self->active_device_ = nullptr;
                    }
                    self->last_dev_state_.erase(device);
                    self->connected_devices_.erase(it);
                    to_release = device;
                }
            }
        }
        // Release COM device strictly outside mutex_ to prevent hot-plug re-entrancy deadlocks.
        if (to_release) {
            to_release->Release();
        }
    }

    void EnsureInitialized() {
        std::call_once(init_once_, [this]() {
            if (!IsOptedIn()) {
                return;
            }

            HMODULE self_mod = nullptr;
            (void)::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&NativeHostGameInputBackend::OnHostDeviceCallback),
                &self_mod);

            wchar_t sys_dir[MAX_PATH]{};
            const UINT slen = ::GetSystemDirectoryW(sys_dir, MAX_PATH);
            if (slen == 0u || slen >= MAX_PATH) {
                return;
            }
            std::wstring host_gi_path(sys_dir, slen);
            if (!host_gi_path.empty() && host_gi_path.back() != L'\\') {
                host_gi_path.push_back(L'\\');
            }
            host_gi_path.append(L"GameInput.dll");

            HMODULE host_mod = ::LoadLibraryExW(
                host_gi_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!host_mod || host_mod == self_mod) {
                return;
            }

            auto create_fn = reinterpret_cast<PfnHostGameInputCreate>(
                reinterpret_cast<void*>(::GetProcAddress(host_mod, "GameInputCreate")));
            if (!create_fn) {
                return;
            }

            IGameInput* gi = nullptr;
            if (create_fn(&gi) < 0 || !gi) {
                return;
            }

            // Enable background input on the host GameInput instance so Alt-Tab / focus transitions
            // never freeze physical controller polling; OpenScarlett's own ShouldAllowInput() gates
            // visibility to the guest title according to the guest's SetFocusPolicy().
            gi->SetFocusPolicy(0x00000040u);

            uint64_t tok = 0ull;
            (void)gi->RegisterDeviceCallback(
                nullptr,
                GameInputKindGamepad,
                GameInputDeviceConnected,
                kEnumBlocking,
                this,
                &NativeHostGameInputBackend::OnHostDeviceCallback,
                &tok);

            host_mod_ = host_mod;
            cb_token_ = tok;
            host_gi_.store(gi, std::memory_order_release);
        });
    }

    std::once_flag init_once_;
    HMODULE host_mod_ = nullptr;
    std::atomic<IGameInput*> host_gi_{nullptr};
    uint64_t cb_token_ = 0ull;
    std::mutex mutex_;
    std::vector<IGameInputDevice*> connected_devices_;
    std::unordered_map<IGameInputDevice*, GameInputGamepadState> last_dev_state_;
    IGameInputDevice* active_device_ = nullptr;
};
#endif

bool HasActiveGamepadInput(const OsXInputGamepad& g) noexcept {
    if (g.wButtons != 0u) return true;
    if (g.bLeftTrigger > 30u || g.bRightTrigger > 30u) return true;
    if (g.sThumbLX > 7849 || g.sThumbLX < -7849) return true;
    if (g.sThumbLY > 7849 || g.sThumbLY < -7849) return true;
    if (g.sThumbRX > 8689 || g.sThumbRX < -8689) return true;
    if (g.sThumbRY > 8689 || g.sThumbRY < -8689) return true;
    return false;
}

void TranslateXInputToGameInput(const OsXInputGamepad& g, GameInputGamepadState& out) noexcept {
    const uint16_t b = g.wButtons;
    uint32_t gb = GameInputGamepadNone;
    if (b & kXiDpadUp) gb |= GameInputGamepadDPadUp;
    if (b & kXiDpadDown) gb |= GameInputGamepadDPadDown;
    if (b & kXiDpadLeft) gb |= GameInputGamepadDPadLeft;
    if (b & kXiDpadRight) gb |= GameInputGamepadDPadRight;
    if (b & kXiStart) gb |= GameInputGamepadMenu;
    if (b & kXiBack) gb |= GameInputGamepadView;
    if (b & kXiLeftThumb) gb |= GameInputGamepadLeftThumbstick;
    if (b & kXiRightThumb) gb |= GameInputGamepadRightThumbstick;
    if (b & kXiLeftShoulder) gb |= GameInputGamepadLeftShoulder;
    if (b & kXiRightShoulder) gb |= GameInputGamepadRightShoulder;
    if (b & kXiA) gb |= GameInputGamepadA;
    if (b & kXiB) gb |= GameInputGamepadB;
    if (b & kXiX) gb |= GameInputGamepadX;
    if (b & kXiY) gb |= GameInputGamepadY;
    out.buttons = gb;
    out.leftTrigger = NormalizeTrigger(g.bLeftTrigger, 30);
    out.rightTrigger = NormalizeTrigger(g.bRightTrigger, 30);
    out.leftThumbstickX = NormalizeStick(g.sThumbLX, 7849);
    out.leftThumbstickY = NormalizeStick(g.sThumbLY, 7849);
    out.rightThumbstickX = NormalizeStick(g.sThumbRX, 8689);
    out.rightThumbstickY = NormalizeStick(g.sThumbRY, 8689);
}

bool PollPhysicalXInputGamepad(
    XInputSlotTracker& tracker,
    uint32_t preferred_slot,
    GameInputGamepadState& out_gamepad,
    uint32_t& out_active_slot,
    bool* out_fresh_activity = nullptr) {
    out_gamepad = GameInputGamepadState{};
    out_active_slot = preferred_slot;
    if (out_fresh_activity) *out_fresh_activity = false;
#if defined(_WIN32)
    const XInputApi& xi = GetXInput();
    if (!xi.get_state) {
        return false;
    }
    if (tracker.enable_refresh_pending.exchange(false, std::memory_order_acq_rel) && xi.enable) {
        xi.enable(TRUE);
    }

    const uint64_t now_us = QueryTimestampMicros();
    const bool do_full_probe = tracker.force_reprobe.exchange(false, std::memory_order_acq_rel);

    OsXInputState states[4]{};
    bool polled_ok[4]{false, false, false, false};
    bool slot_pkt_changed[4]{false, false, false, false};
    DWORD last_rc = ERROR_DEVICE_NOT_CONNECTED;
    uint32_t active_input_slot = 4u;
    uint32_t first_connected_slot = 4u;
    uint32_t chosen_slot = 4u;

    {
        std::lock_guard<std::mutex> lk(tracker.mutex);
        if (preferred_slot < 4u && tracker.slot_connected[preferred_slot]) {
            tracker.active_slot = preferred_slot;
        }
        for (DWORD slot = 0u; slot < 4u; ++slot) {
            const bool should_poll =
                tracker.slot_connected[slot] ||
                do_full_probe ||
                tracker.slot_last_probe_us[slot] == 0ull ||
                (now_us >= tracker.slot_last_probe_us[slot] + XInputSlotTracker::kDisconnectedReprobeIntervalUs);
            if (!should_poll) {
                continue;
            }
            tracker.slot_last_probe_us[slot] = now_us;
            const DWORD rc = xi.get_state(slot, &states[slot]);
            if (rc == ERROR_SUCCESS) {
                polled_ok[slot] = true;
                last_rc = ERROR_SUCCESS;
                tracker.slot_connected[slot] = true;
                if (first_connected_slot >= 4u) {
                    first_connected_slot = static_cast<uint32_t>(slot);
                }
                const bool pkt_changed =
                    tracker.slot_has_pkt[slot] && (states[slot].dwPacketNumber != tracker.slot_last_pkt[slot]);
                slot_pkt_changed[slot] = pkt_changed;
                tracker.slot_last_pkt[slot] = states[slot].dwPacketNumber;
                tracker.slot_has_pkt[slot] = true;
                if (active_input_slot >= 4u && (pkt_changed || HasActiveGamepadInput(states[slot].Gamepad))) {
                    active_input_slot = static_cast<uint32_t>(slot);
                }
            } else {
                if (tracker.slot_connected[slot]) {
                    tracker.slot_connected[slot] = false;
                    tracker.slot_has_pkt[slot] = false;
                    tracker.force_reprobe.store(true, std::memory_order_release);
                }
                if (last_rc != ERROR_SUCCESS) {
                    last_rc = rc;
                }
            }
        }

        if (active_input_slot < 4u) {
            chosen_slot = active_input_slot;
            tracker.active_slot = chosen_slot;
        } else if (tracker.active_slot < 4u && polled_ok[tracker.active_slot]) {
            chosen_slot = tracker.active_slot;
        } else if (preferred_slot < 4u && polled_ok[preferred_slot]) {
            chosen_slot = preferred_slot;
            tracker.active_slot = chosen_slot;
        } else if (first_connected_slot < 4u) {
            chosen_slot = first_connected_slot;
            tracker.active_slot = chosen_slot;
        }
    }

    const bool physical_pad_found = (chosen_slot < 4u);
    if (physical_pad_found) {
        out_active_slot = chosen_slot;
        TranslateXInputToGameInput(states[chosen_slot].Gamepad, out_gamepad);
        if (out_fresh_activity) {
            *out_fresh_activity =
                slot_pkt_changed[chosen_slot] || HasActiveGamepadInput(states[chosen_slot].Gamepad);
        }
    }
    return physical_pad_found;
#else
    (void)tracker;
    (void)preferred_slot;
    return false;
#endif
}

bool PollPhysicalGamepad(
    XInputSlotTracker& tracker,
    uint32_t preferred_slot,
    GameInputGamepadState& out_gamepad,
    uint32_t& out_active_slot) {
    out_gamepad = GameInputGamepadState{};
    out_active_slot = preferred_slot;
#if defined(_WIN32)
    // Poll XInput first (xinput1_4.dll XInputGetStateEx / XInputGetState -> xinput9_1_0.dll).
    // Only poll opt-in System32\GameInput.dll when explicitly enabled via env.
    GameInputGamepadState xinput_gp{};
    uint32_t xinput_slot = preferred_slot;
    bool xinput_fresh = false;
    const bool xinput_found = PollPhysicalXInputGamepad(
        tracker, preferred_slot, xinput_gp, xinput_slot, &xinput_fresh);

    GameInputGamepadState native_gp{};
    bool native_fresh = false;
    uint64_t native_ts = 0ull;
    const bool native_found = NativeHostGameInputBackend::IsOptedIn()
                                  ? NativeHostGameInputBackend::Global().PollGamepad(
                                        preferred_slot, native_gp, native_fresh, &native_ts)
                                  : false;

    bool use_native = false;
    bool found = false;
    if (xinput_found && native_found) {
        found = true;
        const bool xinput_active = NativeHostGameInputBackend::HasActiveState(xinput_gp);
        const bool native_active = NativeHostGameInputBackend::HasActiveState(native_gp);
        if (xinput_fresh && !native_fresh) {
            tracker.prefer_native_backend.store(false, std::memory_order_release);
            use_native = false;
        } else if (native_fresh && !xinput_fresh) {
            tracker.prefer_native_backend.store(true, std::memory_order_release);
            use_native = true;
        } else if (xinput_active && !native_active) {
            tracker.prefer_native_backend.store(false, std::memory_order_release);
            use_native = false;
        } else if (native_active && !xinput_active) {
            tracker.prefer_native_backend.store(true, std::memory_order_release);
            use_native = true;
        } else {
            use_native = tracker.prefer_native_backend.load(std::memory_order_acquire);
        }
    } else if (xinput_found) {
        found = true;
        use_native = false;
    } else if (native_found) {
        found = true;
        use_native = true;
    }

    if (found) {
        if (use_native) {
            out_gamepad = native_gp;
            out_active_slot = preferred_slot;
        } else {
            out_gamepad = xinput_gp;
            out_active_slot = xinput_slot;
        }
    }

    static const uint64_t s_autopress_start_ms = []() -> uint64_t {
        char buf[32]{};
        const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_AUTOPRESS_A_MS", buf, sizeof(buf));
        if (n > 0u && n < sizeof(buf)) {
            return std::strtoull(buf, nullptr, 10);
        }
        return 0ull;
    }();
    if (s_autopress_start_ms > 0ull) {
        static const uint64_t s_t0_ms = ::GetTickCount64();
        const uint64_t elapsed = ::GetTickCount64() - s_t0_ms;
        if (elapsed >= s_autopress_start_ms) {
            const uint64_t phase = (elapsed - s_autopress_start_ms) % 5000ull;
            if (phase < 300ull) {
                found = true;
                out_gamepad.buttons |= GameInputGamepadA;
            }
        }
    }
    struct AutoNavEvent {
        uint64_t time_ms = 0ull;
        uint64_t dur_ms = 300ull;
        uint32_t mask = 0u;
        float lx = 0.0f;
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        float lt = 0.0f;
        float rt = 0.0f;
    };
    static const std::vector<AutoNavEvent> s_autonav = []() {
        std::vector<AutoNavEvent> evs;
        char buf[1024]{};
        const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_AUTONAV", buf, sizeof(buf));
        if (n == 0u || n >= sizeof(buf)) return evs;
        const char* p = buf;
        while (*p) {
            while (*p == ',' || *p == ' ') ++p;
            if (!*p) break;
            char* end = nullptr;
            const uint64_t t = std::strtoull(p, &end, 10);
            if (!end || *end != ':') break;
            p = end + 1;
            AutoNavEvent ev{};
            ev.time_ms = t;
            ev.dur_ms = 300ull;
            if (std::strncmp(p, "LSU", 3) == 0) { ev.ly = 1.0f; }
            else if (std::strncmp(p, "LSD", 3) == 0) { ev.ly = -1.0f; }
            else if (std::strncmp(p, "LSL", 3) == 0) { ev.lx = -1.0f; }
            else if (std::strncmp(p, "LSR", 3) == 0) { ev.lx = 1.0f; }
            else if (std::strncmp(p, "RSU", 3) == 0) { ev.ry = 1.0f; }
            else if (std::strncmp(p, "RSD", 3) == 0) { ev.ry = -1.0f; }
            else if (std::strncmp(p, "RSL", 3) == 0) { ev.rx = -1.0f; }
            else if (std::strncmp(p, "RSR", 3) == 0) { ev.rx = 1.0f; }
            else if (std::strncmp(p, "RT", 2) == 0) { ev.rt = 1.0f; }
            else if (std::strncmp(p, "LT", 2) == 0) { ev.lt = 1.0f; }
            else {
                switch (*p) {
                    case 'A': case 'a': ev.mask = GameInputGamepadA; break;
                    case 'B': case 'b': ev.mask = GameInputGamepadB; break;
                    case 'X': case 'x': ev.mask = GameInputGamepadX; break;
                    case 'Y': case 'y': ev.mask = GameInputGamepadY; break;
                    case 'U': case 'u': ev.mask = GameInputGamepadDPadUp; break;
                    case 'D': case 'd': ev.mask = GameInputGamepadDPadDown; break;
                    case 'L': case 'l': ev.mask = GameInputGamepadDPadLeft; break;
                    case 'R': case 'r': ev.mask = GameInputGamepadDPadRight; break;
                    case 'S': case 's': ev.mask = GameInputGamepadMenu; break;
                    case 'V': case 'v': ev.mask = GameInputGamepadView; break;
                    default: break;
                }
            }
            while (*p && *p != ',' && *p != ':') ++p;
            if (*p == ':') {
                ++p;
                const uint64_t d = std::strtoull(p, nullptr, 10);
                if (d > 0ull && d <= 30000ull) ev.dur_ms = d;
            }
            if (ev.mask != 0u || ev.lx != 0.0f || ev.ly != 0.0f || ev.rx != 0.0f || ev.ry != 0.0f ||
                ev.lt != 0.0f || ev.rt != 0.0f) {
                evs.push_back(ev);
            }
            while (*p && *p != ',') ++p;
        }
        return evs;
    }();
    if (!s_autonav.empty()) {
        static const uint64_t s_nav_t0_ms = ::GetTickCount64();
        const uint64_t elapsed = ::GetTickCount64() - s_nav_t0_ms;
        for (const auto& ev : s_autonav) {
            if (elapsed >= ev.time_ms && elapsed < ev.time_ms + ev.dur_ms) {
                found = true;
                out_gamepad.buttons |= ev.mask;
                if (ev.lx != 0.0f) out_gamepad.leftThumbstickX = ev.lx;
                if (ev.ly != 0.0f) out_gamepad.leftThumbstickY = ev.ly;
                if (ev.rx != 0.0f) out_gamepad.rightThumbstickX = ev.rx;
                if (ev.ry != 0.0f) out_gamepad.rightThumbstickY = ev.ry;
                if (ev.lt != 0.0f) out_gamepad.leftTrigger = ev.lt;
                if (ev.rt != 0.0f) out_gamepad.rightTrigger = ev.rt;
            }
        }
    }
    // Live interactive input file (OPENSCARLETT_LIVE_INPUT_FILE):
    // Supports either single "<seq>:<CMD>[:<dur_ms>]" or multi-line timed batches
    // "<seq>:<delay_ms>:<CMD>:<dur_ms>" (one event per line sharing <seq>).
    static const std::string s_live_input_path = []() -> std::string {
        char buf[MAX_PATH]{};
        const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_LIVE_INPUT_FILE", buf, sizeof(buf));
        return (n > 0u && n < sizeof(buf)) ? std::string(buf, n) : std::string();
    }();
    if (!s_live_input_path.empty()) {
        struct LiveStep {
            uint64_t start_ms = 0ull;
            uint64_t end_ms = 0ull;
            uint32_t btn = 0u;
            float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f, lt = 0.0f, rt = 0.0f;
        };
        static std::mutex s_live_mu;
        static uint64_t s_last_poll_ms = 0ull;
        static uint64_t s_last_seq = 0ull;
        static std::vector<LiveStep> s_steps;
        auto parse_cmd = [](const char* cmd, LiveStep& st, uint64_t& out_dur) {
            out_dur = 250ull;
            if (std::strncmp(cmd, "RSD", 3) == 0) { st.ry = -1.0f; out_dur = 3000ull; }
            else if (std::strncmp(cmd, "RSU", 3) == 0) { st.ry = 1.0f; out_dur = 1500ull; }
            else if (std::strncmp(cmd, "RSL", 3) == 0) { st.rx = -1.0f; out_dur = 600ull; }
            else if (std::strncmp(cmd, "RSR", 3) == 0) { st.rx = 1.0f; out_dur = 600ull; }
            else if (std::strncmp(cmd, "LSD", 3) == 0) { st.ly = -1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "LSU", 3) == 0) { st.ly = 1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "LSL", 3) == 0) { st.lx = -1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "LSR", 3) == 0) { st.lx = 1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "LT", 2) == 0) { st.lt = 1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "RT", 2) == 0) { st.rt = 1.0f; out_dur = 400ull; }
            else if (std::strncmp(cmd, "LB", 2) == 0) { st.btn = GameInputGamepadLeftShoulder; }
            else if (std::strncmp(cmd, "RB", 2) == 0) { st.btn = GameInputGamepadRightShoulder; }
            else if (cmd[0] == 'A') { st.btn = GameInputGamepadA; }
            else if (cmd[0] == 'B') { st.btn = GameInputGamepadB; }
            else if (cmd[0] == 'X') { st.btn = GameInputGamepadX; }
            else if (cmd[0] == 'Y') { st.btn = GameInputGamepadY; }
            else if (cmd[0] == 'U') { st.btn = GameInputGamepadDPadUp; }
            else if (cmd[0] == 'D') { st.btn = GameInputGamepadDPadDown; }
            else if (cmd[0] == 'L') { st.btn = GameInputGamepadDPadLeft; }
            else if (cmd[0] == 'R') { st.btn = GameInputGamepadDPadRight; }
            else if (cmd[0] == 'S') { st.btn = GameInputGamepadMenu; }
            else if (cmd[0] == 'V') { st.btn = GameInputGamepadView; }
        };
        const uint64_t now_ms = ::GetTickCount64();
        std::lock_guard<std::mutex> lk(s_live_mu);
        if (now_ms - s_last_poll_ms >= 50ull) {
            s_last_poll_ms = now_ms;
            FILE* f = nullptr;
            if (::fopen_s(&f, s_live_input_path.c_str(), "rb") == 0 && f) {
                char line[160]{};
                uint64_t batch_seq = 0ull;
                while (std::fgets(line, static_cast<int>(sizeof(line)), f)) {
                    if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
                    char* end1 = nullptr;
                    const uint64_t seq = std::strtoull(line, &end1, 10);
                    if (!end1 || *end1 != ':') continue;
                    if (batch_seq == 0ull) {
                        if (seq <= s_last_seq) break;
                        batch_seq = seq;
                        s_last_seq = seq;
                        s_steps.clear();
                    } else if (seq != batch_seq) {
                        continue;
                    }
                    const char* p1 = end1 + 1;
                    LiveStep st{};
                    uint64_t delay_ms = 0ull;
                    uint64_t dur = 250ull;
                    if (p1[0] >= '0' && p1[0] <= '9') {
                        char* end2 = nullptr;
                        delay_ms = std::strtoull(p1, &end2, 10);
                        if (end2 && *end2 == ':') {
                            const char* cmd = end2 + 1;
                            parse_cmd(cmd, st, dur);
                            if (const char* col3 = std::strchr(cmd, ':')) {
                                const uint64_t pd = std::strtoull(col3 + 1, nullptr, 10);
                                if (pd > 0ull && pd <= 30000ull) dur = pd;
                            }
                        }
                    } else {
                        parse_cmd(p1, st, dur);
                        if (const char* col2 = std::strchr(p1, ':')) {
                            const uint64_t pd = std::strtoull(col2 + 1, nullptr, 10);
                            if (pd > 0ull && pd <= 30000ull) dur = pd;
                        }
                    }
                    st.start_ms = now_ms + delay_ms;
                    st.end_ms = st.start_ms + dur;
                    s_steps.push_back(st);
                }
                std::fclose(f);
            }
        }
        for (const auto& st : s_steps) {
            if (now_ms >= st.start_ms && now_ms < st.end_ms) {
                found = true;
                out_gamepad.buttons |= st.btn;
                if (st.lx != 0.0f) out_gamepad.leftThumbstickX = st.lx;
                if (st.ly != 0.0f) out_gamepad.leftThumbstickY = st.ly;
                if (st.rx != 0.0f) out_gamepad.rightThumbstickX = st.rx;
                if (st.ry != 0.0f) out_gamepad.rightThumbstickY = st.ry;
                if (st.lt != 0.0f) out_gamepad.leftTrigger = st.lt;
                if (st.rt != 0.0f) out_gamepad.rightTrigger = st.rt;
            }
        }
    }

    static const std::string s_trace_gi_path = []() {
        char buf[512]{};
        DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_GAMEINPUT", buf, sizeof(buf));
        return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string{};
    }();
    if (!s_trace_gi_path.empty()) {
        static std::atomic<uint32_t> s_prev_state{0xFFFFFFFFu};
        const uint32_t packed = (found ? 0x80000000u : 0u) |
                                (use_native ? 0x40000000u : 0u) |
                                (native_found ? 0x20000000u : 0u) |
                                (xinput_found ? 0x10000000u : 0u) |
                                ((out_active_slot & 0xFu) << 20) |
                                (out_gamepad.buttons & 0xFFFFFu);
        if (s_prev_state.exchange(packed, std::memory_order_relaxed) != packed) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[GameInput] Pad found=%d src=%s (native=%d xinput=%d) slot=%u btn=0x%04X "
                          "lt=%.2f rt=%.2f lx=%.2f ly=%.2f rx=%.2f ry=%.2f\n",
                          found ? 1 : 0,
                          !found ? "none" : (use_native ? "native_sys32" : "xinput"),
                          native_found ? 1 : 0, xinput_found ? 1 : 0,
                          out_active_slot, out_gamepad.buttons,
                          out_gamepad.leftTrigger, out_gamepad.rightTrigger,
                          out_gamepad.leftThumbstickX, out_gamepad.leftThumbstickY,
                          out_gamepad.rightThumbstickX, out_gamepad.rightThumbstickY);
            ::OutputDebugStringA(msg);
            if (s_trace_gi_path != "1") {
                FILE* f = nullptr;
                if (::fopen_s(&f, s_trace_gi_path.c_str(), "a") == 0 && f) {
                    std::fputs(msg, f);
                    std::fclose(f);
                }
            }
        }
    }
    return found;
#else
    (void)tracker;
    (void)preferred_slot;
    return false;
#endif
}

uint32_t ResolveAsciiCodePointForVk(uint8_t vk, bool shift, bool caps) noexcept {
    if (vk >= 'A' && vk <= 'Z') {
        const bool upper = (shift != caps);
        return upper ? static_cast<uint32_t>(vk) : static_cast<uint32_t>(vk + ('a' - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        static constexpr char kShiftedDigits[10] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
        return shift ? static_cast<uint32_t>(static_cast<unsigned char>(kShiftedDigits[vk - '0']))
                     : static_cast<uint32_t>(vk);
    }
    switch (vk) {
        case 0x20u: return ' ';
        case 0xBAu: return shift ? ':' : ';';   // VK_OEM_1
        case 0xBBu: return shift ? '+' : '=';   // VK_OEM_PLUS
        case 0xBCu: return shift ? '<' : ',';   // VK_OEM_COMMA
        case 0xBDu: return shift ? '_' : '-';   // VK_OEM_MINUS
        case 0xBEu: return shift ? '>' : '.';   // VK_OEM_PERIOD
        case 0xBFu: return shift ? '?' : '/';   // VK_OEM_2
        case 0xC0u: return shift ? '~' : '`';   // VK_OEM_3
        case 0xDBu: return shift ? '{' : '[';   // VK_OEM_4
        case 0xDCu: return shift ? '|' : '\\';  // VK_OEM_5
        case 0xDDu: return shift ? '}' : ']';   // VK_OEM_6
        case 0xDEu: return shift ? '"' : '\'';  // VK_OEM_7
        case 0x60u: case 0x61u: case 0x62u: case 0x63u: case 0x64u:
        case 0x65u: case 0x66u: case 0x67u: case 0x68u: case 0x69u:
            return static_cast<uint32_t>('0' + (vk - 0x60u));
        case 0x6Au: return '*';
        case 0x6Bu: return '+';
        case 0x6Du: return '-';
        case 0x6Eu: return '.';
        case 0x6Fu: return '/';
        default: return 0u;
    }
}

void PollHostKeyboard(std::vector<GameInputKeyState>& out_keys) {
    out_keys.clear();
#if defined(_WIN32)
    auto is_down = [](int vk) -> bool {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    const bool shift_down = is_down(VK_LSHIFT) || is_down(VK_RSHIFT) || is_down(VK_SHIFT);
    const bool caps_on = (GetKeyState(VK_CAPITAL) & 1) != 0;

    static constexpr uint8_t kTrackedVks[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
        VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_RETURN, VK_SPACE, VK_ESCAPE, VK_TAB, VK_BACK,
        VK_DELETE, VK_INSERT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_CAPITAL,
        VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
        VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
        VK_MULTIPLY, VK_ADD, VK_SUBTRACT, VK_DECIMAL, VK_DIVIDE,
        VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
        VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_OEM_102
    };
    for (uint8_t vk : kTrackedVks) {
        if (is_down(vk)) {
            GameInputKeyState ks{};
            ks.virtualKey = vk;
            ks.scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            ks.codePoint = ResolveAsciiCodePointForVk(vk, shift_down, caps_on);
            ks.isDeadKey = false;
            out_keys.push_back(ks);
        }
    }
#endif
}

uint32_t PollHostMouseButtonsMask() noexcept {
#if defined(_WIN32)
    uint32_t btns = GameInputMouseNone;
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) btns |= GameInputMouseLeftButton;
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) btns |= GameInputMouseRightButton;
    if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) btns |= GameInputMouseMiddleButton;
    if ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0) btns |= GameInputMouseButton4;
    if ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0) btns |= GameInputMouseButton5;
    return btns;
#else
    return GameInputMouseNone;
#endif
}

// ===========================================================================
// COM Implementations: IGameInputDevice, IGameInputReading, IGameInput
// ===========================================================================
class GameInputReadingImpl;

class GameInputDeviceImpl final : public IGameInputDevice {
public:
    GameInputDeviceImpl(
        uint32_t slot,
        uint32_t supported_input,
        uint16_t vendor_id,
        uint16_t product_id,
        GameInputDeviceFamily family,
        uint32_t rumble_motors,
        const char* display_name)
        : xinput_slot_(slot) {
        std::memset(&info_, 0, sizeof(info_));
        info_.infoSize = sizeof(GameInputDeviceInfo);
        info_.vendorId = vendor_id;
        info_.productId = product_id;
        info_.revisionNumber = 0x0500u;
        info_.deviceFamily = family;
        info_.supportedInput = supported_input;
        info_.supportedRumbleMotors = rumble_motors;
        info_.deviceId[0] = 0x4Fu;  // 'O'
        info_.deviceId[1] = 0x53u;  // 'S'
        info_.deviceId[2] = static_cast<uint8_t>(slot & 0xFFu);
        info_.deviceId[3] = static_cast<uint8_t>(family & 0xFF);
        std::memcpy(info_.deviceRootId, info_.deviceId, sizeof(info_.deviceId));
        display_name_str_.data = display_name ? display_name : "";
        display_name_str_.sizeInBytes = static_cast<uint32_t>(std::strlen(display_name_str_.data));
        display_name_str_.codePointCount = display_name_str_.sizeInBytes;
        info_.displayName = &display_name_str_;
    }

    ~GameInputDeviceImpl();

    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInputDevice)) {
            AddRef();
            *ppvObject = static_cast<IGameInputDevice*>(this);
            return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInputDevice::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInputDevice::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    const GameInputDeviceInfo* OPENSCARLETT_GAMEINPUT_CALL GetDeviceInfo() override {
        RecordOk("IGameInputDevice::GetDeviceInfo", 3);
        return &info_;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetDeviceStatus() override {
        RecordOk("IGameInputDevice::GetDeviceStatus", 4);
        return status_.load(std::memory_order_acquire);
    }

    void OPENSCARLETT_GAMEINPUT_CALL GetBatteryState(void* state) override {
        RecordOk("IGameInputDevice::GetBatteryState", 5);
        if (state) {
            // GameInputBatteryState is 20 bytes (4 floats + 1 enum); zero-init safely.
            std::memset(state, 0, 20);
        }
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL CreateForceFeedbackEffect(
        uint32_t /*motorIndex*/, const void* /*params*/, void** effect) override {
        if (effect) *effect = nullptr;
        return RecordHr("IGameInputDevice::CreateForceFeedbackEffect", GAMEINPUT_E_NOTIMPL, 6);
    }

    bool OPENSCARLETT_GAMEINPUT_CALL IsForceFeedbackMotorPoweredOn(uint32_t /*motorIndex*/) override {
        RecordOk("IGameInputDevice::IsForceFeedbackMotorPoweredOn", 7);
        return false;
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetForceFeedbackMotorGain(
        uint32_t /*motorIndex*/, float /*masterGain*/) override {
        RecordOk("IGameInputDevice::SetForceFeedbackMotorGain", 8);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL SetHapticMotorState(
        uint32_t /*motorIndex*/, const void* /*params*/) override {
        return RecordHr("IGameInputDevice::SetHapticMotorState", GAMEINPUT_S_OK, 9);
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetRumbleState(const GameInputRumbleParams* params) override {
        RecordOk("IGameInputDevice::SetRumbleState", 10);
        auto sanitize_unit = [](float v) -> float {
            return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        };
        GameInputRumbleParams clamped{};
        if (params) {
            clamped.lowFrequency = sanitize_unit(params->lowFrequency);
            clamped.highFrequency = sanitize_unit(params->highFrequency);
            clamped.leftTrigger = sanitize_unit(params->leftTrigger);
            clamped.rightTrigger = sanitize_unit(params->rightTrigger);
        }

        // Mix Xbox impulse triggers (LT -> low/left motor, RT -> high/right motor) cleanly
        // into XInput 16-bit motor speeds while preserving exact 4-channel telemetry.
        const float low_mixed = std::clamp(
            std::max(clamped.lowFrequency, clamped.leftTrigger * 0.6f) +
                std::min(clamped.lowFrequency, clamped.leftTrigger) * 0.25f,
            0.0f, 1.0f);
        const float high_mixed = std::clamp(
            std::max(clamped.highFrequency, clamped.rightTrigger * 0.6f) +
                std::min(clamped.highFrequency, clamped.rightTrigger) * 0.25f,
            0.0f, 1.0f);
        const uint16_t xi_left = static_cast<uint16_t>(std::lround(low_mixed * 65535.0f));
        const uint16_t xi_right = static_cast<uint16_t>(std::lround(high_mixed * 65535.0f));
        const uint32_t slot = xinput_slot_.load(std::memory_order_relaxed);
        const uint64_t ts = QueryTimestampMicros();

        {
            std::lock_guard<std::mutex> lk(mutex_);
            last_rumble_ = clamped;
            last_xinput_left_ = xi_left;
            last_xinput_right_ = xi_right;
            ++rumble_updates_;
            if (clamped.leftTrigger > 0.0f || clamped.rightTrigger > 0.0f) {
                ++impulse_trigger_updates_;
            }
            last_rumble_ts_ = ts;
        }
#if defined(_WIN32)
        if ((info_.supportedRumbleMotors != GameInputRumbleNone) &&
            (status_.load(std::memory_order_acquire) & GameInputDeviceConnected) != 0u) {
            // Send true 4-motor Xbox rumble (including impulse triggers) via native host
            // System32\GameInput.dll when available; fall back to XInput otherwise.
            if (!NativeHostGameInputBackend::Global().SetRumble(clamped)) {
                const XInputApi& xi = GetXInput();
                if (xi.set_state) {
                    OsXInputVibration vib{};
                    vib.wLeftMotorSpeed = xi_left;
                    vib.wRightMotorSpeed = xi_right;
                    (void)xi.set_state(slot, &vib);  // Safely no-ops if no physical XInput pad is attached
                }
            }
        }
#endif
    }

    void GetRumbleTelemetry(OsGameInputRumbleTelemetry& out) {
        std::lock_guard<std::mutex> lk(mutex_);
        out.lastParams = last_rumble_;
        out.xinputLeftMotor = last_xinput_left_;
        out.xinputRightMotor = last_xinput_right_;
        out.activeSlot = xinput_slot_.load(std::memory_order_relaxed);
        out.updateCount = rumble_updates_;
        out.impulseTriggerUpdateCount = impulse_trigger_updates_;
        out.lastTimestampUs = last_rumble_ts_;
    }

    void SetActiveSlot(uint32_t slot) {
        xinput_slot_.store(slot, std::memory_order_relaxed);
    }

    uint32_t ActiveSlot() const {
        return xinput_slot_.load(std::memory_order_relaxed);
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetInputSynchronizationState(bool /*enabled*/) override {}
    void OPENSCARLETT_GAMEINPUT_CALL SendInputSynchronizationHint() override {}
    void OPENSCARLETT_GAMEINPUT_CALL PowerOff() override {}
    int32_t OPENSCARLETT_GAMEINPUT_CALL CreateRawDeviceReport(
        uint32_t /*reportId*/, uint32_t /*reportKind*/, void** report) override {
        if (report) *report = nullptr;
        return GAMEINPUT_E_NOTIMPL;
    }
    int32_t OPENSCARLETT_GAMEINPUT_CALL GetRawDeviceFeature(
        uint32_t /*reportId*/, void** report) override {
        if (report) *report = nullptr;
        return GAMEINPUT_E_NOTIMPL;
    }
    int32_t OPENSCARLETT_GAMEINPUT_CALL SetRawDeviceFeature(void* /*report*/) override {
        return GAMEINPUT_E_NOTIMPL;
    }
    int32_t OPENSCARLETT_GAMEINPUT_CALL SendRawDeviceOutput(void* /*report*/) override {
        return GAMEINPUT_E_NOTIMPL;
    }
    int32_t OPENSCARLETT_GAMEINPUT_CALL SendRawDeviceOutputWithResponse(
        void* /*requestReport*/, void** responseReport) override {
        if (responseReport) *responseReport = nullptr;
        return GAMEINPUT_E_NOTIMPL;
    }
    int32_t OPENSCARLETT_GAMEINPUT_CALL ExecuteRawDeviceIoControl(
        uint32_t /*controlCode*/, size_t /*inputBufferSize*/, const void* /*inputBuffer*/,
        size_t /*outputBufferSize*/, void* /*outputBuffer*/, size_t* outputSize) override {
        if (outputSize) *outputSize = 0u;
        return GAMEINPUT_E_NOTIMPL;
    }
    bool OPENSCARLETT_GAMEINPUT_CALL AcquireExclusiveRawDeviceAccess(
        uint64_t /*timeoutInMicroseconds*/) override {
        return false;
    }
    void OPENSCARLETT_GAMEINPUT_CALL ReleaseExclusiveRawDeviceAccess() override {}

    uint32_t SetStatus(uint32_t new_status) {
        return status_.exchange(new_status, std::memory_order_acq_rel);
    }

    uint64_t SetUserAssociation(uint64_t user_local_id, uint64_t assoc_seq) {
        const uint64_t old_id = associated_user_local_id_.exchange(user_local_id, std::memory_order_acq_rel);
        user_assoc_seq_.store(assoc_seq, std::memory_order_release);
        return old_id;
    }

    void RecordConnectSequence(uint64_t seq) {
        connect_seq_.store(seq, std::memory_order_release);
    }

    uint64_t AssociatedUserLocalId() const {
        return associated_user_local_id_.load(std::memory_order_acquire);
    }

    uint64_t UserAssociationSequence() const {
        return user_assoc_seq_.load(std::memory_order_acquire);
    }

    uint64_t ConnectSequence() const {
        return connect_seq_.load(std::memory_order_acquire);
    }

    uint32_t SupportedInput() const { return info_.supportedInput; }

    // Per-device timestamped reading history ring buffer (bounded to kPerDeviceRingCap)
    void PushDeviceReading(GameInputReadingImpl* reading);
    GameInputReadingImpl* FindNextDeviceReading(uint64_t ref_seq, GameInputKind filter);
    GameInputReadingImpl* FindPrevDeviceReading(uint64_t ref_seq, GameInputKind filter);
    void ClearDeviceHistory();

private:
    static constexpr size_t kPerDeviceRingCap = 128u;

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> xinput_slot_{0};
    std::atomic<uint32_t> status_{GameInputDeviceNoStatus};
    std::atomic<uint64_t> associated_user_local_id_{0ull};
    std::atomic<uint64_t> user_assoc_seq_{0ull};
    std::atomic<uint64_t> connect_seq_{0ull};
    GameInputDeviceInfo info_{};
    GameInputString display_name_str_{};
    std::mutex mutex_;
    GameInputRumbleParams last_rumble_{};
    uint16_t last_xinput_left_ = 0u;
    uint16_t last_xinput_right_ = 0u;
    uint64_t rumble_updates_ = 0ull;
    uint64_t impulse_trigger_updates_ = 0ull;
    uint64_t last_rumble_ts_ = 0ull;
    std::deque<GameInputReadingImpl*> device_ring_;
};

class GameInputReadingImpl final : public IGameInputReading {
public:
    GameInputReadingImpl(
        GameInputKind kind,
        uint64_t seq,
        uint64_t timestamp,
        GameInputDeviceImpl* device,
        const GameInputGamepadState& gamepad,
        std::vector<GameInputKeyState> keys,
        const GameInputMouseState& mouse,
        uint32_t ui_nav_buttons)
        : kind_(kind),
          seq_(seq),
          timestamp_(timestamp),
          device_(device),
          gamepad_(gamepad),
          keys_(std::move(keys)),
          mouse_(mouse),
          ui_nav_buttons_(ui_nav_buttons) {
        if (device_) {
            device_->AddRef();
        }
    }

    ~GameInputReadingImpl() {
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInputReading)) {
            AddRef();
            *ppvObject = static_cast<IGameInputReading*>(this);
            return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInputReading::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInputReading::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    GameInputKind OPENSCARLETT_GAMEINPUT_CALL GetInputKind() override {
        RecordOk("IGameInputReading::GetInputKind", 3);
        return kind_;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetSequenceNumber(GameInputKind /*inputKind*/) override {
        RecordOk("IGameInputReading::GetSequenceNumber", 4);
        return seq_;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetTimestamp() override {
        RecordOk("IGameInputReading::GetTimestamp", 5);
        return timestamp_;
    }

    void OPENSCARLETT_GAMEINPUT_CALL GetDevice(IGameInputDevice** device) override {
        RecordOk("IGameInputReading::GetDevice", 6);
        if (!device) return;
        *device = device_;
        if (device_) {
            device_->AddRef();
        }
    }

    bool OPENSCARLETT_GAMEINPUT_CALL GetRawReport(void** report) override {
        if (report) *report = nullptr;
        return false;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerAxisCount() override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerAxisState(uint32_t, float*) override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerButtonCount() override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerButtonState(uint32_t, bool*) override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerSwitchCount() override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerSwitchState(uint32_t, uint32_t*) override { return 0u; }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyCount() override {
        RecordOk("IGameInputReading::GetKeyCount", 14);
        return static_cast<uint32_t>(keys_.size());
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) override {
        RecordOk("IGameInputReading::GetKeyState", 15);
        if (!stateArray || stateArrayCount == 0) return 0;
        const uint32_t count = std::min<uint32_t>(
            stateArrayCount, static_cast<uint32_t>(keys_.size()));
        if (count > 0) {
            std::memcpy(stateArray, keys_.data(), count * sizeof(GameInputKeyState));
        }
        return count;
    }

    bool OPENSCARLETT_GAMEINPUT_CALL GetMouseState(GameInputMouseState* state) override {
        RecordOk("IGameInputReading::GetMouseState", 16);
        if (!state) return false;
        *state = mouse_;
        return (kind_ & GameInputKindMouse) != 0;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetTouchCount() override { return 0u; }
    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetTouchState(uint32_t, void*) override { return 0u; }
    bool OPENSCARLETT_GAMEINPUT_CALL GetMotionState(void*) override { return false; }
    bool OPENSCARLETT_GAMEINPUT_CALL GetArcadeStickState(void*) override { return false; }
    bool OPENSCARLETT_GAMEINPUT_CALL GetFlightStickState(void*) override { return false; }

    bool OPENSCARLETT_GAMEINPUT_CALL GetGamepadState(GameInputGamepadState* state) override {
        RecordOk("IGameInputReading::GetGamepadState", 22);
        if (!state) return false;
        *state = gamepad_;
        return (kind_ & GameInputKindGamepad) != 0;
    }

    bool OPENSCARLETT_GAMEINPUT_CALL GetRacingWheelState(void*) override { return false; }

    bool OPENSCARLETT_GAMEINPUT_CALL GetUiNavigationState(void* state) override {
        return GetUiNavigationStateInternal(static_cast<OsGameInputUiNavigationState*>(state));
    }

    bool GetUiNavigationStateInternal(OsGameInputUiNavigationState* state) const {
        if (!state) return false;
        state->buttons = ui_nav_buttons_;
        return (kind_ & (GameInputKindUiNavigation | GameInputKindGamepad | GameInputKindKeyboard)) != 0u;
    }

    uint64_t Sequence() const { return seq_; }
    GameInputKind KindRaw() const { return kind_; }
    IGameInputDevice* DevicePtr() const { return device_; }

private:
    std::atomic<uint32_t> ref_count_{1};
    GameInputKind kind_;
    uint64_t seq_;
    uint64_t timestamp_;
    GameInputDeviceImpl* device_;
    GameInputGamepadState gamepad_;
    std::vector<GameInputKeyState> keys_;
    GameInputMouseState mouse_;
    uint32_t ui_nav_buttons_ = kUiNavNone;
};

GameInputDeviceImpl::~GameInputDeviceImpl() {
    ClearDeviceHistory();
}

void GameInputDeviceImpl::ClearDeviceHistory() {
    std::deque<GameInputReadingImpl*> to_release;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        to_release.swap(device_ring_);
    }
    for (auto* r : to_release) {
        if (r) r->Release();
    }
}

void GameInputDeviceImpl::PushDeviceReading(GameInputReadingImpl* reading) {
    if (!reading) return;
    reading->AddRef();
    GameInputReadingImpl* evicted = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        device_ring_.push_back(reading);
        if (device_ring_.size() > kPerDeviceRingCap) {
            evicted = device_ring_.front();
            device_ring_.pop_front();
        }
    }
    if (evicted) evicted->Release();
}

GameInputReadingImpl* GameInputDeviceImpl::FindNextDeviceReading(
    uint64_t ref_seq, GameInputKind filter) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto* r : device_ring_) {
        if (r->Sequence() <= ref_seq) continue;
        if (filter != GameInputKindUnknown && (r->KindRaw() & filter) == 0u) continue;
        r->AddRef();
        return r;
    }
    return nullptr;
}

GameInputReadingImpl* GameInputDeviceImpl::FindPrevDeviceReading(
    uint64_t ref_seq, GameInputKind filter) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = device_ring_.rbegin(); it != device_ring_.rend(); ++it) {
        auto* r = *it;
        if (r->Sequence() >= ref_seq) continue;
        if (filter != GameInputKindUnknown && (r->KindRaw() & filter) == 0u) continue;
        r->AddRef();
        return r;
    }
    return nullptr;
}

class GameInputImpl;

struct LiveGameInputRegistry {
    std::mutex mutex;
    std::unordered_set<GameInputImpl*> instances;
    std::unordered_set<GameInputDeviceImpl*> devices;
    std::unordered_set<GameInputReadingImpl*> readings;
    std::atomic<void*> global_target_hwnd{nullptr};
};

LiveGameInputRegistry& GetLiveGameInputRegistry() {
    static LiveGameInputRegistry* reg = new LiveGameInputRegistry();
    return *reg;
}

std::atomic<int> g_openscarlett_active_cursor_locks{0};

class GameInputImpl final : public IGameInput, public IGameInputEx {
public:
    GameInputImpl() {
#if defined(_WIN32)
        char env_lock[16]{};
        const DWORD env_len = ::GetEnvironmentVariableA(
            "OPENSCARLETT_CURSOR_LOCK", env_lock, static_cast<DWORD>(sizeof(env_lock)));
        if (env_len == 1u && env_lock[0] == '1') {
            SetCursorLocked(true);
        }
#endif
        default_device_ = CreateTrackedDevice(
            0u,
            GameInputKindGamepad | GameInputKindController | GameInputKindUiNavigation,
            0x045Eu, 0x0B12u,
            GameInputFamilyXboxOne,
            GameInputRumbleLowFrequency | GameInputRumbleHighFrequency |
                GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger,
            "OpenScarlett Xbox Wireless Controller (XInput + Keyboard)");
        keyboard_device_ = CreateTrackedDevice(
            1u,
            GameInputKindKeyboard,
            0x045Eu, 0x00DBu,
            GameInputFamilyI8042,
            GameInputRumbleNone,
            "OpenScarlett System Keyboard");
        mouse_device_ = CreateTrackedDevice(
            2u,
            GameInputKindMouse,
            0x045Eu, 0x00CBu,
            GameInputFamilyHid,
            GameInputRumbleNone,
            "OpenScarlett Raw Input Mouse");

        // Report gamepad device connected strictly AFTER establishing user association (local user 1),
        // matching real Xbox Series X|S title binding expectations.
        constexpr uint32_t kConnectedFlags =
            GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled;
        const uint64_t assoc_seq = event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull;
        default_device_->SetUserAssociation(1ull, assoc_seq);
        const uint64_t connect_seq = event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull;
        default_device_->SetStatus(kConnectedFlags);
        default_device_->RecordConnectSequence(connect_seq);

        keyboard_device_->SetStatus(kConnectedFlags);
        keyboard_device_->RecordConnectSequence(event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull);
        mouse_device_->SetStatus(kConnectedFlags);
        mouse_device_->RecordConnectSequence(event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull);

        slot_devices_[0u] = default_device_;
        slot_devices_[1u] = keyboard_device_;
        slot_devices_[2u] = mouse_device_;

        auto& reg = GetLiveGameInputRegistry();
        void* known_hwnd = reg.global_target_hwnd.load(std::memory_order_acquire);
        if (known_hwnd) {
            target_hwnd_.store(known_hwnd, std::memory_order_release);
        }
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.instances.insert(this);
    }

    void SetCursorLocked(bool locked) noexcept {
        const bool prev = cursor_lock_enabled_.exchange(locked, std::memory_order_acq_rel);
        if (prev != locked) {
            g_openscarlett_active_cursor_locks.fetch_add(locked ? 1 : -1, std::memory_order_acq_rel);
        }
    }

    bool IsCursorLocked() const noexcept {
        return cursor_lock_enabled_.load(std::memory_order_acquire);
    }

    ~GameInputImpl() {
        SetCursorLocked(false);
        {
            auto& reg = GetLiveGameInputRegistry();
            std::lock_guard<std::mutex> lk(reg.mutex);
            reg.instances.erase(this);
            for (auto& kv : slot_devices_) {
                reg.devices.erase(kv.second);
            }
        }
        // Break device -> reading -> device reference cycles before releasing devices
        for (auto& kv : slot_devices_) {
            if (kv.second) {
                kv.second->ClearDeviceHistory();
            }
        }
        for (auto* r : history_) {
            if (r) {
                UnregisterReading(r);
                r->Release();
            }
        }
        history_.clear();
        for (auto& kv : slot_devices_) {
            if (kv.second) {
                kv.second->Release();
            }
        }
        slot_devices_.clear();
        default_device_ = nullptr;
        keyboard_device_ = nullptr;
        mouse_device_ = nullptr;
    }

    // --- IGameInput (primary vtable: slots 0..7 unchanged!) ---
    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInput)) {
            AddRef();
            *ppvObject = static_cast<IGameInput*>(this);
            return RecordHr("IGameInput::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        if (GuidEquals(riid, kIidGameInputEx)) {
            AddRef();
            *ppvObject = static_cast<IGameInputEx*>(this);
            return RecordHr("IGameInput::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInput::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInput::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentTimestamp() override {
        RecordOk("IGameInput::GetCurrentTimestamp", 3);
        return QueryTimestampMicros();
    }

    static const std::string& GetTraceGameInputPath() {
        static const std::string s_path = []() {
#if defined(_WIN32)
            char buf[512]{};
            DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_GAMEINPUT", buf, sizeof(buf));
            return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string{};
#else
            return std::string{};
#endif
        }();
        return s_path;
    }

    static void TraceGameInputLine(const char* line) {
        const auto& p = GetTraceGameInputPath();
        if (p.empty() || !line) return;
#if defined(_WIN32)
        ::OutputDebugStringA(line);
        if (p != "1") {
            FILE* f = nullptr;
            if (::fopen_s(&f, p.c_str(), "a") == 0 && f) {
                std::fputs(line, f);
                std::fclose(f);
            }
        }
#endif
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!reading) {
            return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_E_POINTER, 4);
        }
        *reading = nullptr;

        GameInputDeviceImpl* target_dev = nullptr;
        if (device) {
            target_dev = static_cast<GameInputDeviceImpl*>(device);
            if ((target_dev->GetDeviceStatus() & GameInputDeviceConnected) == 0u) {
                return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_E_DEVICE_DISCONNECTED, 4);
            }
        } else {
            if (inputKind == GameInputKindKeyboard) {
                target_dev = keyboard_device_;
            } else if (inputKind == GameInputKindMouse) {
                target_dev = mouse_device_;
            } else {
                target_dev = default_device_;
            }
        }

        const bool input_allowed = ShouldAllowInput();
        const bool was_allowed = prev_input_allowed_.exchange(input_allowed, std::memory_order_acq_rel);
        if (input_allowed && !was_allowed) {
            OnFocusGained();
        } else if (!input_allowed && was_allowed) {
            OnFocusLost();
        }

        if (!GetTraceGameInputPath().empty()) {
            static std::atomic<uint32_t> s_gcr_logs{0u};
            const uint32_t n = s_gcr_logs.fetch_add(1u, std::memory_order_relaxed);
            if (n < 25u || input_allowed != was_allowed) {
                char msg[256];
                std::snprintf(
                    msg, sizeof(msg),
                    "[GameInput] GCR#%u kind=0x%X dev=%p devSup=0x%X allowed=%d policy=0x%X target=%p fg=%p\n",
                    n, static_cast<unsigned>(inputKind), static_cast<void*>(target_dev),
                    target_dev ? target_dev->SupportedInput() : 0u,
                    input_allowed ? 1 : 0,
                    focus_policy_.load(std::memory_order_relaxed),
                    target_hwnd_.load(std::memory_order_relaxed),
#if defined(_WIN32)
                    static_cast<void*>(::GetForegroundWindow())
#else
                    nullptr
#endif
                );
                TraceGameInputLine(msg);
            }
        }

        const bool want_all = (inputKind == GameInputKindUnknown);
        const bool want_gamepad =
            want_all ||
            ((inputKind & (GameInputKindGamepad | GameInputKindController | GameInputKindUiNavigation)) != 0u);
        const bool want_keyboard =
            want_all ||
            ((inputKind & (GameInputKindKeyboard | GameInputKindUiNavigation)) != 0u);
        const bool want_mouse =
            want_all || ((inputKind & GameInputKindMouse) != 0u);
        const bool vpad_fallback = virtual_gamepad_fallback_.load(std::memory_order_acquire);

        bool use_injected = false;
        bool injected_gp_explicit = false;
        GameInputGamepadState gp{};
        std::vector<GameInputKeyState> keys;
        if (input_allowed) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (test_inject_enabled_.load(std::memory_order_acquire)) {
                gp = injected_gamepad_;
                keys = injected_keys_;
                use_injected = true;
                injected_gp_explicit = injected_gamepad_explicit_;
            }
        }

        GameInputMouseState mouse_snapshot{};
        int64_t raw_dx = 0;
        int64_t raw_dy = 0;
        if (want_mouse || (want_gamepad && vpad_fallback)) {
            std::lock_guard<std::mutex> mlk(raw_mouse_mutex_);
            if (input_allowed) {
                if (!use_injected) {
                    PollCursorLockToggleLocked();
                }
                if (!use_injected && total_mouse_packets_ == 0ull &&
                    !raw_mouse_registered_.load(std::memory_order_acquire)) {
                    PollFallbackCursorMouseLocked();
                    mouse_snapshot.buttons = fallback_mouse_buttons_;
                    mouse_snapshot.positionX = fallback_mouse_x_;
                    mouse_snapshot.positionY = fallback_mouse_y_;
                    mouse_snapshot.wheelX = fallback_wheel_x_;
                    mouse_snapshot.wheelY = fallback_wheel_y_;
                    raw_dx = fallback_last_dx_;
                    raw_dy = fallback_last_dy_;
                } else {
                    mouse_snapshot.buttons = mouse_buttons_;
                    mouse_snapshot.positionX = mouse_cumulative_x_;
                    mouse_snapshot.positionY = mouse_cumulative_y_;
                    mouse_snapshot.wheelX = mouse_cumulative_wheel_x_;
                    mouse_snapshot.wheelY = mouse_cumulative_wheel_y_;
                    raw_dx = pending_mouse_dx_;
                    raw_dy = pending_mouse_dy_;
                }
            } else {
                f8_prev_down_ = false;
                fallback_was_cursor_locked_ = false;
            }
        }

        if (input_allowed && use_injected && !injected_gp_explicit && vpad_fallback && want_gamepad) {
            // Synthesize virtual gamepad state from injected keyboard + raw mouse state.
            ApplyVirtualGamepadFallback(gp, keys, mouse_snapshot.buttons, raw_dx, raw_dy);
        }

        if (input_allowed && !use_injected) {
            bool physical_pad_found = false;
            if (want_gamepad && (target_dev->SupportedInput() & GameInputKindGamepad) != 0u) {
                uint32_t active_slot = target_dev->ActiveSlot();
                physical_pad_found = PollPhysicalGamepad(
                    xinput_tracker_, active_slot, gp, active_slot);
                if (target_dev == default_device_) {
                    target_dev->SetActiveSlot(active_slot);
                }
            }
            const bool need_keys_for_vpad =
                want_gamepad && !physical_pad_found && vpad_fallback &&
                (target_dev->SupportedInput() & GameInputKindGamepad) != 0u;
            if (want_keyboard || need_keys_for_vpad) {
                PollHostKeyboard(keys);
            }
            if (need_keys_for_vpad) {
                const uint32_t eff_mouse_buttons = mouse_snapshot.buttons | PollHostMouseButtonsMask();
                ApplyVirtualGamepadFallback(gp, keys, eff_mouse_buttons, raw_dx, raw_dy);
            }
        }

        const GameInputKind resolved_kind =
            (inputKind == GameInputKindUnknown)
                ? static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard |
                                             GameInputKindMouse | GameInputKindUiNavigation)
                : inputKind;

        const uint32_t ui_nav = input_allowed ? DeriveUiNavigationButtons(gp, keys) : kUiNavNone;

        GameInputReadingImpl* r = nullptr;
        GameInputReadingImpl* evicted_global = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const uint64_t seq = next_seq_++;
            const uint64_t raw_ts = QueryTimestampMicros();
            const uint64_t ts = (raw_ts >= last_ts_) ? raw_ts : last_ts_;
            last_ts_ = ts;

            r = new (std::nothrow) GameInputReadingImpl(
                resolved_kind, seq, ts, target_dev, gp, std::move(keys), mouse_snapshot, ui_nav);
            if (!r) {
                return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_E_OUTOFMEMORY, 4);
            }

            // Global history copy (bounded to 256 readings across all devices).
            r->AddRef();
            history_.push_back(r);
            if (history_.size() > 256u) {
                evicted_global = history_.front();
                history_.pop_front();
            }
        }
        RegisterReading(r);
        target_dev->PushDeviceReading(r);
        if (evicted_global) {
            evicted_global->Release();
        }

        *reading = r;
        return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_S_OK, 4);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) {
            return RecordHr("IGameInput::GetNextReading", GAMEINPUT_E_INVALIDARG, 5);
        }
        *reading = nullptr;
        const uint64_t ref_seq = static_cast<GameInputReadingImpl*>(referenceReading)->Sequence();

        // Fast per-device ring-buffer lookup when a specific device is queried
        if (device) {
            auto* dev_impl = static_cast<GameInputDeviceImpl*>(device);
            if (auto* found = dev_impl->FindNextDeviceReading(ref_seq, inputKind)) {
                *reading = found;
                return RecordHr("IGameInput::GetNextReading", GAMEINPUT_S_OK, 5);
            }
            return RecordHr("IGameInput::GetNextReading", GAMEINPUT_E_READING_NOT_FOUND, 5);
        }

        std::lock_guard<std::mutex> lk(mutex_);
        for (auto* r : history_) {
            if (r->Sequence() <= ref_seq) continue;
            if (inputKind != GameInputKindUnknown && (r->KindRaw() & inputKind) == 0u) continue;
            r->AddRef();
            *reading = r;
            return RecordHr("IGameInput::GetNextReading", GAMEINPUT_S_OK, 5);
        }
        return RecordHr("IGameInput::GetNextReading", GAMEINPUT_E_READING_NOT_FOUND, 5);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) {
            return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_E_INVALIDARG, 6);
        }
        *reading = nullptr;
        const uint64_t ref_seq = static_cast<GameInputReadingImpl*>(referenceReading)->Sequence();

        if (device) {
            auto* dev_impl = static_cast<GameInputDeviceImpl*>(device);
            if (auto* found = dev_impl->FindPrevDeviceReading(ref_seq, inputKind)) {
                *reading = found;
                return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_S_OK, 6);
            }
            return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_E_READING_NOT_FOUND, 6);
        }

        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            auto* r = *it;
            if (r->Sequence() >= ref_seq) continue;
            if (inputKind != GameInputKindUnknown && (r->KindRaw() & inputKind) == 0u) continue;
            r->AddRef();
            *reading = r;
            return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_S_OK, 6);
        }
        return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_E_READING_NOT_FOUND, 6);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetTemporalReading(
        uint64_t /*timestamp*/,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        return GetCurrentReading(GameInputKindUnknown, device, reading);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterReadingCallback(
        IGameInputDevice* /*device*/,
        GameInputKind /*inputKind*/,
        float /*analogThreshold*/,
        void* /*context*/,
        GameInputReadingCallback /*callbackFunc*/,
        uint64_t* callbackToken) override {
        const uint64_t tok = next_cb_token_++;
        if (callbackToken) *callbackToken = tok;
        return RecordHr("IGameInput::RegisterReadingCallback", GAMEINPUT_S_OK, 8);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterSystemButtonCallback(
        IGameInputDevice* /*device*/,
        uint32_t /*buttonFilter*/,
        void* /*context*/,
        GameInputSystemButtonCallback /*callbackFunc*/,
        uint64_t* callbackToken) override {
        const uint64_t tok = next_cb_token_++;
        if (callbackToken) *callbackToken = tok;
        return RecordHr("IGameInput::RegisterSystemButtonCallback", GAMEINPUT_S_OK, 10);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterKeyboardLayoutCallback(
        IGameInputDevice* /*device*/,
        void* /*context*/,
        GameInputKeyboardLayoutCallback /*callbackFunc*/,
        uint64_t* callbackToken) override {
        const uint64_t tok = next_cb_token_++;
        if (callbackToken) *callbackToken = tok;
        return RecordHr("IGameInput::RegisterKeyboardLayoutCallback", GAMEINPUT_S_OK, 11);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL CreateDispatcher(void** dispatcher) override {
        if (dispatcher) *dispatcher = nullptr;
        return RecordHr("IGameInput::CreateDispatcher", GAMEINPUT_E_NOTIMPL, 14);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL CreateAggregateDevice(
        GameInputKind /*inputKind*/, IGameInputDevice** device) override {
        if (!device) return RecordHr("IGameInput::CreateAggregateDevice", GAMEINPUT_E_POINTER, 15);
        *device = default_device_;
        if (default_device_) default_device_->AddRef();
        return RecordHr("IGameInput::CreateAggregateDevice", GAMEINPUT_S_OK, 15);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromId(
        const void* value, IGameInputDevice** device) override {
        if (!device) return RecordHr("IGameInput::FindDeviceFromId", GAMEINPUT_E_POINTER, 16);
        *device = nullptr;
        if (!value) return RecordHr("IGameInput::FindDeviceFromId", GAMEINPUT_E_INVALIDARG, 16);
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : slot_devices_) {
            if (kv.second && std::memcmp(kv.second->GetDeviceInfo()->deviceId, value, 32) == 0) {
                kv.second->AddRef();
                *device = kv.second;
                return RecordHr("IGameInput::FindDeviceFromId", GAMEINPUT_S_OK, 16);
            }
        }
        return RecordHr("IGameInput::FindDeviceFromId", GAMEINPUT_E_DEVICE_DISCONNECTED, 16);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromObject(
        void* /*value*/, IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return RecordHr("IGameInput::FindDeviceFromObject", GAMEINPUT_E_NOTIMPL, 17);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromPlatformHandle(
        void* /*value*/, IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return RecordHr("IGameInput::FindDeviceFromPlatformHandle", GAMEINPUT_E_NOTIMPL, 18);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromPlatformString(
        const wchar_t* /*value*/, IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return RecordHr("IGameInput::FindDeviceFromPlatformString", GAMEINPUT_E_NOTIMPL, 19);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL EnableOemDeviceSupport(
        uint16_t /*vendorId*/, uint16_t /*productId*/,
        uint8_t /*interfaceNumber*/, uint8_t /*collectionNumber*/) override {
        return RecordHr("IGameInput::EnableOemDeviceSupport", GAMEINPUT_S_OK, 20);
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetFocusPolicy(uint32_t policy) override {
        RecordOk("IGameInput::SetFocusPolicy", 21);
        focus_policy_.store(policy, std::memory_order_release);
    }

    // --- IGameInputEx: Hot-Plug Device Callbacks ---
    int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        OsGameInputDeviceCallback callbackFunc,
        uint64_t* callbackToken) override {
        if (!callbackFunc) {
            return RecordHr("IGameInput::RegisterDeviceCallback", GAMEINPUT_E_INVALIDARG);
        }
        DeviceCallbackEntry entry{};
        std::vector<GameInputDeviceImpl*> initial_devices;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            entry.token = next_cb_token_++;
            entry.device_filter = device;
            entry.kind_filter = inputKind;
            entry.status_filter = statusFilter;
            entry.context = context;
            entry.func = callbackFunc;
            entry.stopped = false;
            device_callbacks_.push_back(entry);

            if (enumerationKind == kEnumAsync || enumerationKind == kEnumBlocking) {
                for (const auto& kv : slot_devices_) {
                    auto* d = kv.second;
                    if (!d) continue;
                    const uint32_t cur_st = d->GetDeviceStatus();
                    if ((cur_st & GameInputDeviceConnected) == 0u) continue;
                    if (device && static_cast<IGameInputDevice*>(d) != device) continue;
                    if (inputKind != GameInputKindUnknown &&
                        (d->SupportedInput() & static_cast<uint32_t>(inputKind)) == 0u) {
                        continue;
                    }
                    if (statusFilter != GameInputDeviceAnyStatus && (cur_st & statusFilter) == 0u) {
                        continue;
                    }
                    d->AddRef();
                    initial_devices.push_back(d);
                }
            }
        }
        if (callbackToken) {
            *callbackToken = entry.token;
        }

        // Deliver initial enumeration callbacks outside internal locks
        if (!GetTraceGameInputPath().empty()) {
            char msg[220];
            std::snprintf(
                msg, sizeof(msg),
                "[GameInput] RegisterDeviceCallback kind=0x%X status=0x%X enum=%u initial_count=%zu\n",
                static_cast<unsigned>(inputKind), statusFilter, enumerationKind, initial_devices.size());
            TraceGameInputLine(msg);
        }
        const uint64_t ts = QueryTimestampMicros();
        for (auto* d : initial_devices) {
            if (!GetTraceGameInputPath().empty()) {
                char msg[220];
                std::snprintf(
                    msg, sizeof(msg),
                    "[GameInput]   EnumDev dev=%p slot=%u sup=0x%X fam=%d status=0x%X\n",
                    static_cast<void*>(d), d->ActiveSlot(), d->SupportedInput(),
                    static_cast<int>(d->GetDeviceInfo()->deviceFamily), d->GetDeviceStatus());
                TraceGameInputLine(msg);
            }
            bool still_active = false;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (const auto& cb : device_callbacks_) {
                    if (cb.token == entry.token && !cb.stopped) {
                        still_active = true;
                        break;
                    }
                }
            }
            if (still_active) {
                entry.func(
                    entry.token,
                    entry.context,
                    d,
                    ts,
                    d->GetDeviceStatus(),
                    GameInputDeviceNoStatus);
            }
            d->Release();
        }
        return RecordHr("IGameInput::RegisterDeviceCallback", GAMEINPUT_S_OK);
    }

    void OPENSCARLETT_GAMEINPUT_CALL StopCallback(uint64_t callbackToken) override {
        RecordOk("IGameInput::StopCallback");
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& cb : device_callbacks_) {
            if (cb.token == callbackToken) {
                cb.stopped = true;
                break;
            }
        }
    }

    bool OPENSCARLETT_GAMEINPUT_CALL UnregisterCallback(
        uint64_t callbackToken, uint64_t /*timeoutInMicroseconds*/) override {
        RecordOk("IGameInput::UnregisterCallback");
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t before = device_callbacks_.size();
        device_callbacks_.erase(
            std::remove_if(device_callbacks_.begin(), device_callbacks_.end(),
                           [&](const DeviceCallbackEntry& e) { return e.token == callbackToken; }),
            device_callbacks_.end());
        return device_callbacks_.size() < before;
    }

    int32_t RegisterUserAssociationCallback(
        void* context,
        OsGameInputUserAssociationCallback callback,
        uint64_t* out_token) {
        if (!callback) {
            return GAMEINPUT_E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t tok = next_user_assoc_token_++;
        user_assoc_callbacks_.push_back(UserAssocCallbackEntry{tok, context, callback});
        if (out_token) {
            *out_token = tok;
        }
        return GAMEINPUT_S_OK;
    }

    bool UnregisterUserAssociationCallback(uint64_t token) {
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t before = user_assoc_callbacks_.size();
        user_assoc_callbacks_.erase(
            std::remove_if(user_assoc_callbacks_.begin(), user_assoc_callbacks_.end(),
                           [&](const UserAssocCallbackEntry& e) { return e.token == token; }),
            user_assoc_callbacks_.end());
        return user_assoc_callbacks_.size() < before;
    }

    void NotifyUserAssociationForDevice(GameInputDeviceImpl* dev, uint64_t new_user_local_id) {
        if (!dev) return;
        const uint64_t seq = event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull;
        const uint64_t old_user = dev->SetUserAssociation(new_user_local_id, seq);
        OsGameInputUserAssociationEvent ev{};
        const GameInputDeviceInfo* info = dev->GetDeviceInfo();
        if (info) {
            std::memcpy(ev.deviceId, info->deviceId, sizeof(ev.deviceId));
        }
        ev.oldUserLocalId = old_user;
        ev.newUserLocalId = new_user_local_id;
        ev.timestampUs = QueryTimestampMicros();
        ev.eventSequence = seq;

        std::vector<UserAssocCallbackEntry> cbs;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cbs = user_assoc_callbacks_;
        }
        for (const auto& cb : cbs) {
            if (cb.func) {
                cb.func(cb.context, &ev);
            }
        }
    }

    int32_t SimulateDeviceHotPlug(
        uint32_t device_slot,
        GameInputKind supported_input,
        bool connected,
        IGameInputDevice** out_device) {
        GameInputDeviceImpl* target = nullptr;
        uint32_t prev_status = GameInputDeviceNoStatus;
        uint32_t curr_status = connected
            ? (GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled)
            : GameInputDeviceNoStatus;
        std::vector<DeviceCallbackEntry> matching_cbs;
        bool need_user_assoc_before_connect = false;
        uint64_t assoc_user_id = 0ull;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = slot_devices_.find(device_slot);
            if (it == slot_devices_.end()) {
                const uint32_t kinds = (supported_input != GameInputKindUnknown)
                                           ? static_cast<uint32_t>(supported_input)
                                           : static_cast<uint32_t>(GameInputKindGamepad | GameInputKindUiNavigation);
                const uint32_t rumble = (kinds & GameInputKindGamepad)
                    ? (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency |
                       GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger)
                    : GameInputRumbleNone;
                target = CreateTrackedDevice(
                    device_slot,
                    kinds,
                    0x045Eu,
                    static_cast<uint16_t>(0x0B12u + (device_slot & 0x0Fu)),
                    (kinds & GameInputKindGamepad) ? GameInputFamilyXboxOne : GameInputFamilyHid,
                    rumble,
                    "OpenScarlett Hot-Plug Device");
                target->SetStatus(GameInputDeviceNoStatus);
                slot_devices_[device_slot] = target;
            } else {
                target = it->second;
            }
            target->AddRef();

            if (connected && (target->SupportedInput() & GameInputKindGamepad) != 0u) {
                need_user_assoc_before_connect = true;
                assoc_user_id = (target->AssociatedUserLocalId() != 0ull)
                                    ? target->AssociatedUserLocalId()
                                    : (1ull + static_cast<uint64_t>(device_slot));
            }
        }

        // CRITICAL GDK ORDERING: Establish and notify user association STRICTLY BEFORE
        // transitioning the gamepad device to GameInputDeviceConnected and firing device callbacks!
        if (need_user_assoc_before_connect) {
            NotifyUserAssociationForDevice(target, assoc_user_id);
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            prev_status = target->SetStatus(curr_status);
            if (connected) {
                target->RecordConnectSequence(event_seq_.fetch_add(1ull, std::memory_order_acq_rel) + 1ull);
            }

            const uint32_t changed = prev_status ^ curr_status;
            for (const auto& cb : device_callbacks_) {
                if (cb.stopped) continue;
                if (cb.device_filter && cb.device_filter != static_cast<IGameInputDevice*>(target)) {
                    continue;
                }
                if (cb.kind_filter != GameInputKindUnknown &&
                    (target->SupportedInput() & static_cast<uint32_t>(cb.kind_filter)) == 0u) {
                    continue;
                }
                if (cb.status_filter != GameInputDeviceAnyStatus &&
                    (changed & cb.status_filter) == 0u &&
                    (curr_status & cb.status_filter) == 0u) {
                    continue;
                }
                matching_cbs.push_back(cb);
            }
        }

        const uint64_t ts = QueryTimestampMicros();
        for (const auto& cb : matching_cbs) {
            cb.func(cb.token, cb.context, target, ts, curr_status, prev_status);
        }

        // On disconnect, disassociate user AFTER device disconnected callback fires.
        if (!connected && (target->SupportedInput() & GameInputKindGamepad) != 0u &&
            target->AssociatedUserLocalId() != 0ull) {
            NotifyUserAssociationForDevice(target, 0ull);
        }

        if (out_device) {
            *out_device = target;  // transfers AddRef to caller
        } else {
            target->Release();
        }
        return GAMEINPUT_S_OK;
    }

    int32_t GetDeviceForKind(GameInputKind kind, IGameInputDevice** out_device) {
        if (!out_device) return GAMEINPUT_E_POINTER;
        *out_device = nullptr;
        std::lock_guard<std::mutex> lk(mutex_);
        GameInputDeviceImpl* chosen = nullptr;
        if (kind == GameInputKindKeyboard) {
            chosen = keyboard_device_;
        } else if (kind == GameInputKindMouse) {
            chosen = mouse_device_;
        } else {
            chosen = default_device_;
        }
        if (!chosen) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        chosen->AddRef();
        *out_device = chosen;
        return GAMEINPUT_S_OK;
    }

    void SetTargetWindow(void* hwnd) {
        target_hwnd_.store(hwnd, std::memory_order_release);
    }

    void SetVirtualGamepadFallback(bool enable) {
        virtual_gamepad_fallback_.store(enable, std::memory_order_release);
    }

    void SetInjectedTestState(
        const GameInputGamepadState* gp,
        const GameInputKeyState* keys,
        uint32_t key_count,
        bool enable) {
        std::lock_guard<std::mutex> lk(mutex_);
        test_inject_enabled_.store(enable, std::memory_order_release);
        if (!enable) {
            injected_gamepad_explicit_ = false;
            injected_gamepad_ = GameInputGamepadState{};
            injected_keys_.clear();
            return;
        }
        injected_gamepad_explicit_ = (gp != nullptr);
        injected_gamepad_ = gp ? *gp : GameInputGamepadState{};
        injected_keys_.clear();
        if (keys && key_count > 0u) {
            injected_keys_.assign(keys, keys + key_count);
        }
    }

    int32_t RegisterRawMouse(void* hwnd, uint32_t raw_flags) {
#if defined(_WIN32)
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01u;
        rid.usUsage = 0x02u;
        rid.dwFlags = raw_flags;
        rid.hwndTarget = reinterpret_cast<HWND>(hwnd);
        if (!::RegisterRawInputDevices(&rid, 1u, static_cast<UINT>(sizeof(rid)))) {
            return GAMEINPUT_E_INVALIDARG;
        }
        raw_mouse_registered_.store(true, std::memory_order_release);
        if (hwnd && ::IsWindowVisible(reinterpret_cast<HWND>(hwnd)) != FALSE) {
            target_hwnd_.store(hwnd, std::memory_order_release);
        }
        return GAMEINPUT_S_OK;
#else
        (void)hwnd;
        (void)raw_flags;
        raw_mouse_registered_.store(true, std::memory_order_release);
        return GAMEINPUT_S_OK;
#endif
    }

    int32_t ProcessWin32RawInput(void* h_raw_input) {
#if defined(_WIN32)
        if (!h_raw_input) {
            return GAMEINPUT_E_INVALIDARG;
        }
        UINT data_size = 0u;
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(h_raw_input),
                              RID_INPUT,
                              nullptr,
                              &data_size,
                              sizeof(RAWINPUTHEADER)) != 0u ||
            data_size < sizeof(RAWINPUTHEADER)) {
            return GAMEINPUT_E_INVALIDARG;
        }
        uint8_t stack_buf[128]{};
        std::vector<uint8_t> heap_buf;
        uint8_t* ptr = stack_buf;
        if (data_size > sizeof(stack_buf)) {
            heap_buf.resize(data_size);
            ptr = heap_buf.data();
        }
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(h_raw_input),
                              RID_INPUT,
                              ptr,
                              &data_size,
                              sizeof(RAWINPUTHEADER)) != data_size) {
            return GAMEINPUT_E_INVALIDARG;
        }
        const auto* raw = reinterpret_cast<const RAWINPUT*>(ptr);
        if (raw->header.dwType != RIM_TYPEMOUSE) {
            return GAMEINPUT_S_OK;
        }
        ApplyDecodedRawMouse(raw->data.mouse);
        return GAMEINPUT_S_OK;
#else
        (void)h_raw_input;
        return GAMEINPUT_E_INVALIDARG;
#endif
    }

#if defined(_WIN32)
    void ApplyDecodedRawMouse(const RAWMOUSE& m) {
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        // Seed cumulative position from cursor fallback on the very first Raw Input packet
        // so the title never sees a sudden coordinate jump on transition.
        if (total_mouse_packets_ == 0ull && (fallback_mouse_x_ != 0 || fallback_mouse_y_ != 0 ||
                                             fallback_wheel_x_ != 0 || fallback_wheel_y_ != 0)) {
            mouse_cumulative_x_ = fallback_mouse_x_;
            mouse_cumulative_y_ = fallback_mouse_y_;
            mouse_cumulative_wheel_x_ = fallback_wheel_x_;
            mouse_cumulative_wheel_y_ = fallback_wheel_y_;
            mouse_buttons_ = fallback_mouse_buttons_;
        }

        int32_t dx = 0;
        int32_t dy = 0;
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0u) {
            dx = static_cast<int32_t>(m.lLastX);
            dy = static_cast<int32_t>(m.lLastY);
        } else {
            const bool vdesk = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0u;
            const int32_t w = ::GetSystemMetrics(vdesk ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int32_t h = ::GetSystemMetrics(vdesk ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            const int32_t px = (w > 0) ? static_cast<int32_t>((static_cast<int64_t>(m.lLastX) * w) / 65535LL) : 0;
            const int32_t py = (h > 0) ? static_cast<int32_t>((static_cast<int64_t>(m.lLastY) * h) / 65535LL) : 0;
            if (abs_mouse_valid_) {
                dx = px - abs_mouse_last_x_;
                dy = py - abs_mouse_last_y_;
            }
            abs_mouse_last_x_ = px;
            abs_mouse_last_y_ = py;
            abs_mouse_valid_ = true;
        }
        int32_t wx = 0;
        int32_t wy = 0;
        if ((m.usButtonFlags & RI_MOUSE_WHEEL) != 0u) {
            wy = static_cast<int16_t>(m.usButtonData);
        }
#if defined(RI_MOUSE_HWHEEL)
        if ((m.usButtonFlags & RI_MOUSE_HWHEEL) != 0u) {
            wx = static_cast<int16_t>(m.usButtonData);
        }
#endif
        uint32_t btns = mouse_buttons_;
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) btns |= GameInputMouseLeftButton;
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) btns &= ~GameInputMouseLeftButton;
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) btns |= GameInputMouseRightButton;
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) btns &= ~GameInputMouseRightButton;
        if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) btns |= GameInputMouseMiddleButton;
        if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) btns &= ~GameInputMouseMiddleButton;
        if (m.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) btns |= GameInputMouseButton4;
        if (m.usButtonFlags & RI_MOUSE_BUTTON_4_UP) btns &= ~GameInputMouseButton4;
        if (m.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) btns |= GameInputMouseButton5;
        if (m.usButtonFlags & RI_MOUSE_BUTTON_5_UP) btns &= ~GameInputMouseButton5;
        AccumulateRawMousePacketLocked(dx, dy, wx, wy, btns);
    }
#endif

    void OnFocusGained() noexcept {
        xinput_tracker_.force_reprobe.store(true, std::memory_order_release);
        xinput_tracker_.enable_refresh_pending.store(true, std::memory_order_release);
#if defined(_WIN32)
        NativeHostGameInputBackend::Global().RefreshFocusPolicy();
#endif
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        cursor_fallback_valid_ = false;
        abs_mouse_valid_ = false;
        fallback_last_dx_ = 0;
        fallback_last_dy_ = 0;
        if (!test_inject_enabled_.load(std::memory_order_acquire)) {
            const uint32_t live_btns = PollHostMouseButtonsMask();
            mouse_buttons_ = live_btns;
            fallback_mouse_buttons_ = live_btns;
        }
    }

    void OnFocusLost() noexcept {
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        mouse_buttons_ = GameInputMouseNone;
        fallback_mouse_buttons_ = GameInputMouseNone;
        cursor_fallback_valid_ = false;
        abs_mouse_valid_ = false;
        fallback_last_dx_ = 0;
        fallback_last_dy_ = 0;
    }

    void OnDeviceChange() noexcept {
        xinput_tracker_.force_reprobe.store(true, std::memory_order_release);
        xinput_tracker_.enable_refresh_pending.store(true, std::memory_order_release);
    }

    void AccumulateFallbackWheel(int32_t wx, int32_t wy) noexcept {
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        if (total_mouse_packets_ == 0ull) {
            fallback_wheel_x_ += static_cast<int64_t>(wx);
            fallback_wheel_y_ += static_cast<int64_t>(wy);
        }
    }

    void InjectRawMouseDelta(int32_t dx, int32_t dy, int32_t wx, int32_t wy, uint32_t buttons) {
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        AccumulateRawMousePacketLocked(dx, dy, wx, wy, buttons);
    }

    void ConsumeFrameMouseDelta(GameInputRawMouseFrameDelta& out_delta) {
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
        out_delta.deltaX = pending_mouse_dx_;
        out_delta.deltaY = pending_mouse_dy_;
        out_delta.wheelX = pending_wheel_x_;
        out_delta.wheelY = pending_wheel_y_;
        out_delta.buttons = mouse_buttons_;
        out_delta.packetCount = pending_packet_count_;
        out_delta.totalPacketsReceived = total_mouse_packets_;
        out_delta.ringBufferedPackets = static_cast<uint32_t>(raw_mouse_ring_.size());
        out_delta.reserved = 0u;
        pending_mouse_dx_ = 0;
        pending_mouse_dy_ = 0;
        pending_wheel_x_ = 0;
        pending_wheel_y_ = 0;
        pending_packet_count_ = 0u;
    }

private:
    struct UserAssocCallbackEntry {
        uint64_t token = 0ull;
        void* context = nullptr;
        OsGameInputUserAssociationCallback func = nullptr;
    };

    struct DeviceCallbackEntry {
        uint64_t token = 0ull;
        IGameInputDevice* device_filter = nullptr;
        GameInputKind kind_filter = GameInputKindUnknown;
        uint32_t status_filter = GameInputDeviceAnyStatus;
        void* context = nullptr;
        OsGameInputDeviceCallback func = nullptr;
        bool stopped = false;
    };

    struct RawMousePacket {
        int32_t dx = 0;
        int32_t dy = 0;
        int32_t wheel_x = 0;
        int32_t wheel_y = 0;
        uint32_t buttons = 0u;
        uint64_t timestamp = 0u;
    };

    static constexpr size_t kRawMouseRingCapacity = 256u;

    static GameInputDeviceImpl* CreateTrackedDevice(
        uint32_t slot,
        uint32_t supported_input,
        uint16_t vid,
        uint16_t pid,
        GameInputDeviceFamily family,
        uint32_t rumble,
        const char* name) {
        auto* d = new GameInputDeviceImpl(slot, supported_input, vid, pid, family, rumble, name);
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.devices.insert(d);
        return d;
    }

    static void RegisterReading(GameInputReadingImpl* r) {
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.readings.insert(r);
    }

    static void UnregisterReading(GameInputReadingImpl* r) {
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.readings.erase(r);
    }

    void AccumulateRawMousePacketLocked(
        int32_t dx, int32_t dy, int32_t wx, int32_t wy, uint32_t buttons) {
        mouse_cumulative_x_ += static_cast<int64_t>(dx);
        mouse_cumulative_y_ += static_cast<int64_t>(dy);
        mouse_cumulative_wheel_x_ += static_cast<int64_t>(wx);
        mouse_cumulative_wheel_y_ += static_cast<int64_t>(wy);
        pending_mouse_dx_ += static_cast<int64_t>(dx);
        pending_mouse_dy_ += static_cast<int64_t>(dy);
        pending_wheel_x_ += static_cast<int64_t>(wx);
        pending_wheel_y_ += static_cast<int64_t>(wy);
        mouse_buttons_ = buttons;
        ++pending_packet_count_;
        ++total_mouse_packets_;

        RawMousePacket pkt{};
        pkt.dx = dx;
        pkt.dy = dy;
        pkt.wheel_x = wx;
        pkt.wheel_y = wy;
        pkt.buttons = buttons;
        pkt.timestamp = QueryTimestampMicros();
        if (raw_mouse_ring_.size() >= kRawMouseRingCapacity) {
            raw_mouse_ring_.pop_front();
        }
        raw_mouse_ring_.push_back(pkt);
    }

    void PollCursorLockToggleLocked() noexcept {
#if defined(_WIN32)
        HWND fg = ::GetForegroundWindow();
        DWORD fg_pid = 0;
        const bool fg_owned = (fg != nullptr &&
                               ::GetWindowThreadProcessId(fg, &fg_pid) != 0 &&
                               fg_pid == ::GetCurrentProcessId());
        if (fg_owned) {
            const bool f8_down = (::GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8_down && !f8_prev_down_) {
                SetCursorLocked(!IsCursorLocked());
            }
            f8_prev_down_ = f8_down;
        } else {
            f8_prev_down_ = false;
        }
#endif
    }

    void PollFallbackCursorMouseLocked() noexcept {
#if defined(_WIN32)
        HWND fg = ::GetForegroundWindow();
        DWORD fg_pid = 0;
        const bool fg_owned = (fg != nullptr &&
                               ::GetWindowThreadProcessId(fg, &fg_pid) != 0 &&
                               fg_pid == ::GetCurrentProcessId());
        const bool locked_now = fg_owned && IsCursorLocked();
        const bool just_locked = locked_now && !fallback_was_cursor_locked_;
        fallback_was_cursor_locked_ = locked_now;

        POINT pt{};
        if (::GetCursorPos(&pt) != FALSE) {
            if (cursor_fallback_valid_ && !just_locked) {
                const int64_t dx = static_cast<int64_t>(pt.x - cursor_fallback_last_.x);
                const int64_t dy = static_cast<int64_t>(pt.y - cursor_fallback_last_.y);
                fallback_mouse_x_ += dx;
                fallback_mouse_y_ += dy;
                fallback_last_dx_ = dx;
                fallback_last_dy_ = dy;
            } else {
                fallback_last_dx_ = 0;
                fallback_last_dy_ = 0;
            }
            cursor_fallback_last_ = pt;
            cursor_fallback_valid_ = true;
            if (locked_now) {
                RECT cr{};
                if (::GetClientRect(fg, &cr) != FALSE && cr.right > cr.left && cr.bottom > cr.top) {
                    POINT center{(cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2};
                    ::ClientToScreen(fg, &center);
                    ::SetCursorPos(center.x, center.y);
                    cursor_fallback_last_ = center;
                }
            }
        }
        fallback_mouse_buttons_ = PollHostMouseButtonsMask();
#endif
    }

#if defined(_WIN32)
    static bool ProcessOwnsVisibleGuiWindowCached() noexcept {
        static std::atomic<uint64_t> s_last_check_us{0ull};
        static std::atomic<bool> s_cached_found{false};
        const uint64_t now_us = QueryTimestampMicros();
        const uint64_t prev_us = s_last_check_us.load(std::memory_order_acquire);
        if (prev_us != 0ull && now_us < prev_us + 100000ull) {
            return s_cached_found.load(std::memory_order_acquire);
        }
        struct EnumCtx {
            DWORD self_pid;
            HWND console_hwnd;
            bool found;
        };
        EnumCtx ctx{GetCurrentProcessId(), GetConsoleWindow(), false};
        EnumWindows(
            [](HWND hwnd, LPARAM lparam) -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lparam);
                if (!hwnd || hwnd == c->console_hwnd || !IsWindowVisible(hwnd)) {
                    return TRUE;
                }
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != c->self_pid) {
                    return TRUE;
                }
                RECT rc{};
                if (GetWindowRect(hwnd, &rc) && (rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0) {
                    c->found = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        s_cached_found.store(ctx.found, std::memory_order_release);
        s_last_check_us.store(now_us, std::memory_order_release);
        return ctx.found;
    }
#endif

    bool ShouldAllowInput() const noexcept {
#if defined(_WIN32)
        static const bool s_force_bg_allow = []() noexcept {
            char buf[64]{};
            if (::GetEnvironmentVariableA("OPENSCARLETT_GAMEINPUT_BACKGROUND", buf, sizeof(buf)) > 0 && buf[0] != '0') {
                return true;
            }
            if (::GetEnvironmentVariableA("OPENSCARLETT_AUTONAV", buf, sizeof(buf)) > 0) {
                return true;
            }
            if (::GetEnvironmentVariableA("OPENSCARLETT_LIVE_INPUT_FILE", buf, sizeof(buf)) > 0) {
                return true;
            }
            if (::GetEnvironmentVariableA("OPENSCARLETT_AUTOPRESS_A_MS", buf, sizeof(buf)) > 0) {
                return true;
            }
            return false;
        }();
        if (s_force_bg_allow) {
            return true;
        }
#endif
        const uint32_t policy = focus_policy_.load(std::memory_order_acquire);
        if ((policy & (GameInputEnableBackgroundInput | 0x00000040u)) != 0u) {
            return true;
        }
#if defined(_WIN32)
        HWND fg = GetForegroundWindow();
        const DWORD self_pid = GetCurrentProcessId();
        HWND target = reinterpret_cast<HWND>(target_hwnd_.load(std::memory_order_acquire));
        if (target && IsWindow(target) != FALSE) {
            if (!fg) {
                return false;
            }
            if ((fg == target) ||
                (IsChild(target, fg) != FALSE) ||
                (GetAncestor(fg, GA_ROOTOWNER) == target) ||
                (GetAncestor(fg, GA_ROOTOWNER) == GetAncestor(target, GA_ROOTOWNER))) {
                return true;
            }
            // If an explicit target window was set (e.g., focus gating test or multi-window title),
            // allow only if fg belongs to this process AND isn't a different unrelated window when
            // explicit focus policy is set.
            if ((policy & (GameInputDisableBackgroundInput | GameInputExclusiveForegroundInput)) != 0u) {
                return false;
            }
            DWORD fg_pid = 0;
            GetWindowThreadProcessId(fg, &fg_pid);
            return (fg_pid == self_pid);
        }
        if (fg) {
            DWORD fg_pid = 0;
            GetWindowThreadProcessId(fg, &fg_pid);
            if (fg_pid == self_pid) {
                return true;
            }
        }
        const uint32_t explicit_fg_mask =
            GameInputDisableBackgroundInput | GameInputExclusiveForegroundInput;
        if ((policy & explicit_fg_mask) != 0u) {
            return false;
        }
        // GameInputDefaultFocusPolicy (0): block background input whenever this process
        // owns a visible GUI window (e.g. a game swapchain window) that is not focused,
        // while keeping headless console unit tests & explicit test injection functional.
        if (test_inject_enabled_.load(std::memory_order_acquire)) {
            return true;
        }
        return !ProcessOwnsVisibleGuiWindowCached();
#else
        return true;
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> focus_policy_{GameInputDefaultFocusPolicy};
    std::atomic<void*> target_hwnd_{nullptr};
    std::atomic<bool> prev_input_allowed_{true};
    XInputSlotTracker xinput_tracker_;
    GameInputDeviceImpl* default_device_ = nullptr;
    GameInputDeviceImpl* keyboard_device_ = nullptr;
    GameInputDeviceImpl* mouse_device_ = nullptr;
    std::map<uint32_t, GameInputDeviceImpl*> slot_devices_;
    std::mutex mutex_;
    std::atomic<uint64_t> event_seq_{0ull};
    std::atomic<bool> virtual_gamepad_fallback_{true};
    uint64_t next_seq_ = 1u;
    uint64_t last_ts_ = 0u;
    uint64_t next_cb_token_ = 1u;
    uint64_t next_user_assoc_token_ = 1u;
    std::deque<GameInputReadingImpl*> history_;
    std::vector<DeviceCallbackEntry> device_callbacks_;
    std::vector<UserAssocCallbackEntry> user_assoc_callbacks_;
    std::atomic<bool> test_inject_enabled_{false};
    bool injected_gamepad_explicit_ = false;
    GameInputGamepadState injected_gamepad_{};
    std::vector<GameInputKeyState> injected_keys_;

    std::atomic<bool> raw_mouse_registered_{false};
    std::atomic<bool> cursor_lock_enabled_{false};
    bool f8_prev_down_{false};
    bool fallback_was_cursor_locked_{false};
    std::mutex raw_mouse_mutex_;
    int64_t mouse_cumulative_x_ = 0;
    int64_t mouse_cumulative_y_ = 0;
    int64_t mouse_cumulative_wheel_x_ = 0;
    int64_t mouse_cumulative_wheel_y_ = 0;
    int64_t pending_mouse_dx_ = 0;
    int64_t pending_mouse_dy_ = 0;
    int64_t pending_wheel_x_ = 0;
    int64_t pending_wheel_y_ = 0;
    uint32_t mouse_buttons_ = GameInputMouseNone;
    uint32_t pending_packet_count_ = 0u;
    uint64_t total_mouse_packets_ = 0ull;
    std::deque<RawMousePacket> raw_mouse_ring_;
    bool abs_mouse_valid_ = false;
    int32_t abs_mouse_last_x_ = 0;
    int32_t abs_mouse_last_y_ = 0;
    bool cursor_fallback_valid_ = false;
#if defined(_WIN32)
    POINT cursor_fallback_last_{};
#endif
    int64_t fallback_mouse_x_ = 0;
    int64_t fallback_mouse_y_ = 0;
    int64_t fallback_wheel_x_ = 0;
    int64_t fallback_wheel_y_ = 0;
    int64_t fallback_last_dx_ = 0;
    int64_t fallback_last_dy_ = 0;
    uint32_t fallback_mouse_buttons_ = GameInputMouseNone;
};

GameInputImpl* AcquireLiveGameInput(IGameInput* input) {
    if (!input) return nullptr;
    auto* candidate = static_cast<GameInputImpl*>(input);
    auto& reg = GetLiveGameInputRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    if (reg.instances.find(candidate) == reg.instances.end()) {
        return nullptr;
    }
    candidate->AddRef();
    return candidate;
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_GAMEINPUT_CALL GameInputCreate(IGameInput** gameInput) {
    if (!gameInput) {
        return RecordHr("GameInputCreate", GAMEINPUT_E_POINTER);
    }
    *gameInput = nullptr;
    auto* obj = new (std::nothrow) GameInputImpl();
    if (!obj) {
        return RecordHr("GameInputCreate", GAMEINPUT_E_OUTOFMEMORY);
    }
    *gameInput = static_cast<IGameInput*>(obj);
    return RecordHr("GameInputCreate", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputSetTargetWindow(
    IGameInput* input, void* hwnd) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputSetTargetWindow", GAMEINPUT_E_INVALIDARG);
    }
    impl->SetTargetWindow(hwnd);
    impl->Release();
    return RecordHr("OpenScarlettGameInputSetTargetWindow", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputInjectTestState(
    IGameInput* input,
    const GameInputGamepadState* gp,
    const GameInputKeyState* keys,
    uint32_t key_count,
    int enable) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_E_INVALIDARG);
    }
    if (enable != 0 && key_count > 0u && !keys) {
        impl->Release();
        return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_E_INVALIDARG);
    }
    impl->SetInjectedTestState(gp, keys, key_count, enable != 0);
    impl->Release();
    return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputRegisterRawMouse(
    IGameInput* input,
    void* hwnd,
    uint32_t rawInputFlags) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputRegisterRawMouse", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->RegisterRawMouse(hwnd, rawInputFlags);
    impl->Release();
    return RecordHr("OpenScarlettGameInputRegisterRawMouse", hr);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputProcessRawInput(
    IGameInput* input,
    void* hRawInput) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputProcessRawInput", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->ProcessWin32RawInput(hRawInput);
    impl->Release();
    return RecordHr("OpenScarlettGameInputProcessRawInput", hr);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputInjectRawMouseDelta(
    IGameInput* input,
    int32_t deltaX,
    int32_t deltaY,
    int32_t wheelX,
    int32_t wheelY,
    uint32_t buttons) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputInjectRawMouseDelta", GAMEINPUT_E_INVALIDARG);
    }
    impl->InjectRawMouseDelta(deltaX, deltaY, wheelX, wheelY, buttons);
    impl->Release();
    return RecordHr("OpenScarlettGameInputInjectRawMouseDelta", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputConsumeFrameMouseDelta(
    IGameInput* input,
    GameInputRawMouseFrameDelta* outDelta) {
    if (!outDelta) {
        return RecordHr("OpenScarlettGameInputConsumeFrameMouseDelta", GAMEINPUT_E_POINTER);
    }
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputConsumeFrameMouseDelta", GAMEINPUT_E_INVALIDARG);
    }
    impl->ConsumeFrameMouseDelta(*outDelta);
    impl->Release();
    return RecordHr("OpenScarlettGameInputConsumeFrameMouseDelta", GAMEINPUT_S_OK);
}

// --- Sprint 9/10 Hot-Plug Device Enumeration, UI Navigation & Impulse-Trigger Telemetry ---

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputRegisterDeviceCallback(
    IGameInput* input,
    IGameInputDevice* device,
    GameInputKind inputKind,
    uint32_t statusFilter,
    uint32_t enumerationKind,
    void* context,
    OsGameInputDeviceCallback callbackFunc,
    uint64_t* callbackToken) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputRegisterDeviceCallback", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->RegisterDeviceCallback(
        device, inputKind, statusFilter, enumerationKind, context, callbackFunc, callbackToken);
    impl->Release();
    return hr;
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputStopCallback(
    IGameInput* input,
    uint64_t callbackToken) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputStopCallback", GAMEINPUT_E_INVALIDARG);
    }
    impl->StopCallback(callbackToken);
    impl->Release();
    return GAMEINPUT_S_OK;
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputUnregisterCallback(
    IGameInput* input,
    uint64_t callbackToken,
    uint64_t timeoutInMicroseconds) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputUnregisterCallback", GAMEINPUT_E_INVALIDARG);
    }
    const bool removed = impl->UnregisterCallback(callbackToken, timeoutInMicroseconds);
    impl->Release();
    return removed ? GAMEINPUT_S_OK : GAMEINPUT_E_INVALIDARG;
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputSimulateDeviceHotPlug(
    IGameInput* input,
    uint32_t deviceSlot,
    GameInputKind supportedInput,
    int32_t connected,
    IGameInputDevice** outDevice) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputSimulateDeviceHotPlug", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->SimulateDeviceHotPlug(
        deviceSlot, supportedInput, connected != 0, outDevice);
    impl->Release();
    return RecordHr("OpenScarlettGameInputSimulateDeviceHotPlug", hr);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputGetDeviceForKind(
    IGameInput* input,
    GameInputKind inputKind,
    IGameInputDevice** outDevice) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputGetDeviceForKind", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->GetDeviceForKind(inputKind, outDevice);
    impl->Release();
    return RecordHr("OpenScarlettGameInputGetDeviceForKind", hr);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputGetDeviceRumbleTelemetry(
    IGameInputDevice* device,
    void* outTelemetry) {
    if (!device || !outTelemetry) {
        return RecordHr("OpenScarlettGameInputGetDeviceRumbleTelemetry", GAMEINPUT_E_INVALIDARG);
    }
    auto* dev = static_cast<GameInputDeviceImpl*>(device);
    {
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        if (reg.devices.find(dev) == reg.devices.end()) {
            return RecordHr("OpenScarlettGameInputGetDeviceRumbleTelemetry", GAMEINPUT_E_INVALIDARG);
        }
    }
    dev->GetRumbleTelemetry(*static_cast<OsGameInputRumbleTelemetry*>(outTelemetry));
    return RecordHr("OpenScarlettGameInputGetDeviceRumbleTelemetry", GAMEINPUT_S_OK);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputGetUiNavigationState(
    IGameInputReading* reading,
    void* outState) {
    if (!reading || !outState) {
        return RecordHr("OpenScarlettGameInputGetUiNavigationState", GAMEINPUT_E_INVALIDARG);
    }
    auto* r = static_cast<GameInputReadingImpl*>(reading);
    const bool ok = r->GetUiNavigationStateInternal(static_cast<OsGameInputUiNavigationState*>(outState));
    return RecordHr("OpenScarlettGameInputGetUiNavigationState",
                    ok ? GAMEINPUT_S_OK : GAMEINPUT_E_INVALIDARG);
}

// --- Sprint 12 User-Before-Connect Association & Virtual Gamepad Fallback Controls ---

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputRegisterUserAssociationCallback(
    IGameInput* input,
    void* context,
    OsGameInputUserAssociationCallback callbackFunc,
    uint64_t* callbackToken) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputRegisterUserAssociationCallback", GAMEINPUT_E_INVALIDARG);
    }
    const int32_t hr = impl->RegisterUserAssociationCallback(context, callbackFunc, callbackToken);
    impl->Release();
    return RecordHr("OpenScarlettGameInputRegisterUserAssociationCallback", hr);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputUnregisterUserAssociationCallback(
    IGameInput* input,
    uint64_t callbackToken) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputUnregisterUserAssociationCallback", GAMEINPUT_E_INVALIDARG);
    }
    const bool removed = impl->UnregisterUserAssociationCallback(callbackToken);
    impl->Release();
    return RecordHr("OpenScarlettGameInputUnregisterUserAssociationCallback",
                    removed ? GAMEINPUT_S_OK : GAMEINPUT_E_INVALIDARG);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputGetDeviceUserAssociation(
    IGameInputDevice* device,
    uint64_t* outUserLocalId,
    uint64_t* outAssocSeq,
    uint64_t* outConnectSeq) {
    if (!device) {
        return RecordHr("OpenScarlettGameInputGetDeviceUserAssociation", GAMEINPUT_E_INVALIDARG);
    }
    auto* dev = static_cast<GameInputDeviceImpl*>(device);
    {
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        if (reg.devices.find(dev) == reg.devices.end()) {
            return RecordHr("OpenScarlettGameInputGetDeviceUserAssociation", GAMEINPUT_E_INVALIDARG);
        }
    }
    if (outUserLocalId) *outUserLocalId = dev->AssociatedUserLocalId();
    if (outAssocSeq) *outAssocSeq = dev->UserAssociationSequence();
    if (outConnectSeq) *outConnectSeq = dev->ConnectSequence();
    return RecordHr("OpenScarlettGameInputGetDeviceUserAssociation", GAMEINPUT_S_OK);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputSetVirtualGamepadFallback(
    IGameInput* input,
    int enable) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputSetVirtualGamepadFallback", GAMEINPUT_E_INVALIDARG);
    }
    impl->SetVirtualGamepadFallback(enable != 0);
    impl->Release();
    return RecordHr("OpenScarlettGameInputSetVirtualGamepadFallback", GAMEINPUT_S_OK);
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputBroadcastRawInput(void* hRawInput) {
#if defined(_WIN32)
    if (!hRawInput) {
        return GAMEINPUT_E_INVALIDARG;
    }
    UINT data_size = 0u;
    if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(hRawInput),
                          RID_INPUT,
                          nullptr,
                          &data_size,
                          sizeof(RAWINPUTHEADER)) != 0u ||
        data_size < sizeof(RAWINPUTHEADER)) {
        return GAMEINPUT_E_INVALIDARG;
    }
    uint8_t stack_buf[128]{};
    std::vector<uint8_t> heap_buf;
    uint8_t* ptr = stack_buf;
    if (data_size > sizeof(stack_buf)) {
        heap_buf.resize(data_size);
        ptr = heap_buf.data();
    }
    if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(hRawInput),
                          RID_INPUT,
                          ptr,
                          &data_size,
                          sizeof(RAWINPUTHEADER)) != data_size) {
        return GAMEINPUT_E_INVALIDARG;
    }
    const auto* raw = reinterpret_cast<const RAWINPUT*>(ptr);
    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return GAMEINPUT_S_OK;
    }
    std::vector<GameInputImpl*> live;
    {
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        live.reserve(reg.instances.size());
        for (auto* inst : reg.instances) {
            if (inst) {
                inst->AddRef();
                live.push_back(inst);
            }
        }
    }
    for (auto* inst : live) {
        inst->ApplyDecodedRawMouse(raw->data.mouse);
        inst->Release();
    }
    return GAMEINPUT_S_OK;
#else
    (void)hRawInput;
    return GAMEINPUT_E_INVALIDARG;
#endif
}

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputBroadcastWindowEvent(
    void* hwnd,
    uint32_t eventKind,
    int32_t param1,
    int32_t param2) {
    auto& reg = GetLiveGameInputRegistry();
    if (eventKind == GameInputWinEventTargetHwnd && hwnd) {
#if defined(_WIN32)
        if (::IsWindow(reinterpret_cast<HWND>(hwnd)) != FALSE &&
            ::IsWindowVisible(reinterpret_cast<HWND>(hwnd)) != FALSE) {
            reg.global_target_hwnd.store(hwnd, std::memory_order_release);
        }
#else
        reg.global_target_hwnd.store(hwnd, std::memory_order_release);
#endif
    }
    std::vector<GameInputImpl*> live;
    {
        std::lock_guard<std::mutex> lk(reg.mutex);
        live.reserve(reg.instances.size());
        for (auto* inst : reg.instances) {
            if (inst) {
                inst->AddRef();
                live.push_back(inst);
            }
        }
    }
    for (auto* inst : live) {
        switch (eventKind) {
            case GameInputWinEventTargetHwnd:
                if (hwnd) {
#if defined(_WIN32)
                    if (::IsWindow(reinterpret_cast<HWND>(hwnd)) != FALSE &&
                        ::IsWindowVisible(reinterpret_cast<HWND>(hwnd)) != FALSE) {
                        inst->SetTargetWindow(hwnd);
                    }
#else
                    inst->SetTargetWindow(hwnd);
#endif
                }
                break;
            case GameInputWinEventFocus:
                if (param1 != 0) {
                    inst->OnFocusGained();
                } else {
                    inst->OnFocusLost();
                }
                break;
            case GameInputWinEventDeviceChange:
                inst->OnDeviceChange();
                break;
            case GameInputWinEventMouseWheel:
                inst->AccumulateFallbackWheel(param1, param2);
                break;
            default:
                break;
        }
        inst->Release();
    }
    return GAMEINPUT_S_OK;
}

OPENSCARLETT_GAMEINPUT_API bool OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputIsCursorLocked(IGameInput* input) {
    if (!input) {
        return false;
    }
    return static_cast<GameInputImpl*>(input)->IsCursorLocked();
}

OPENSCARLETT_GAMEINPUT_API void OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputSetCursorLocked(
    IGameInput* input,
    bool locked) {
    if (!input) {
        return;
    }
    static_cast<GameInputImpl*>(input)->SetCursorLocked(locked);
    RecordOk("OpenScarlettGameInputSetCursorLocked");
}

OPENSCARLETT_GAMEINPUT_API bool OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputIsAnyCursorLocked(void) {
    return g_openscarlett_active_cursor_locks.load(std::memory_order_acquire) > 0;
}

}  // extern "C"
