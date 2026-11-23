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

uint64_t QueryTimestampMicros() {
#if defined(_WIN32)
    static LARGE_INTEGER s_freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (s_freq.QuadPart > 0) {
        return static_cast<uint64_t>((now.QuadPart * 1000000LL) / s_freq.QuadPart);
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

    HMODULE module = nullptr;
    PfnGetState get_state = nullptr;
    PfnSetState set_state = nullptr;
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
            a.get_state = reinterpret_cast<XInputApi::PfnGetState>(
                reinterpret_cast<void*>(GetProcAddress(a.module, "XInputGetState")));
            a.set_state = reinterpret_cast<XInputApi::PfnSetState>(
                reinterpret_cast<void*>(GetProcAddress(a.module, "XInputSetState")));
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

void PollGamepadAndKeyboard(
    uint32_t preferred_slot,
    bool virtual_gamepad_fallback_enabled,
    uint32_t raw_mouse_buttons,
    int64_t raw_mouse_dx,
    int64_t raw_mouse_dy,
    GameInputGamepadState& out_gamepad,
    std::vector<GameInputKeyState>& out_keys,
    uint32_t& out_active_slot) {
    out_gamepad = GameInputGamepadState{};
    out_keys.clear();
    out_active_slot = preferred_slot;

#if defined(_WIN32)
    bool physical_pad_found = false;
    const XInputApi& xi = GetXInput();
    if (xi.get_state) {
        OsXInputState st{};
        DWORD last_rc = xi.get_state(preferred_slot, &st);
        if (last_rc == ERROR_SUCCESS) {
            physical_pad_found = true;
            out_active_slot = preferred_slot;
        } else {
            for (DWORD slot = 0; slot < 4; ++slot) {
                DWORD rc = xi.get_state(slot, &st);
                if (rc == ERROR_SUCCESS) {
                    physical_pad_found = true;
                    out_active_slot = static_cast<uint32_t>(slot);
                    last_rc = rc;
                    break;
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
            const uint32_t packed = (physical_pad_found ? 0x80000000u : 0u) |
                                    ((out_active_slot & 0xFu) << 16) |
                                    (physical_pad_found ? static_cast<uint32_t>(st.Gamepad.wButtons) : (last_rc & 0xFFFFu));
            if (s_prev_state.exchange(packed, std::memory_order_relaxed) != packed) {
                char msg[220];
                std::snprintf(msg, sizeof(msg),
                              "[GameInput] XInput found=%d slot=%u rc=%lu pkt=%u btn=0x%04X lx=%d ly=%d\n",
                              physical_pad_found ? 1 : 0, out_active_slot, static_cast<unsigned long>(last_rc),
                              st.dwPacketNumber, physical_pad_found ? st.Gamepad.wButtons : 0u,
                              physical_pad_found ? static_cast<int>(st.Gamepad.sThumbLX) : 0,
                              physical_pad_found ? static_cast<int>(st.Gamepad.sThumbLY) : 0);
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
        if (physical_pad_found) {
            const uint16_t b = st.Gamepad.wButtons;
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
            out_gamepad.buttons = gb;
            out_gamepad.leftTrigger = NormalizeTrigger(st.Gamepad.bLeftTrigger, 30);
            out_gamepad.rightTrigger = NormalizeTrigger(st.Gamepad.bRightTrigger, 30);
            out_gamepad.leftThumbstickX = NormalizeStick(st.Gamepad.sThumbLX, 7849);
            out_gamepad.leftThumbstickY = NormalizeStick(st.Gamepad.sThumbLY, 7849);
            out_gamepad.rightThumbstickX = NormalizeStick(st.Gamepad.sThumbRX, 8689);
            out_gamepad.rightThumbstickY = NormalizeStick(st.Gamepad.sThumbRY, 8689);
        }
    }

    auto is_down = [](int vk) -> bool {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    static constexpr uint8_t kTrackedVks[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
        VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_RETURN, VK_SPACE, VK_ESCAPE, VK_TAB, VK_BACK
    };
    for (uint8_t vk : kTrackedVks) {
        if (is_down(vk)) {
            GameInputKeyState ks{};
            ks.virtualKey = vk;
            ks.scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            ks.codePoint = (vk >= 0x20 && vk <= 0x7E) ? vk : 0u;
            ks.isDeadKey = false;
            out_keys.push_back(ks);
        }
    }

    if (!physical_pad_found && virtual_gamepad_fallback_enabled) {
        uint32_t eff_mouse_buttons = raw_mouse_buttons;
        if (is_down(VK_LBUTTON)) eff_mouse_buttons |= GameInputMouseLeftButton;
        if (is_down(VK_RBUTTON)) eff_mouse_buttons |= GameInputMouseRightButton;
        if (is_down(VK_MBUTTON)) eff_mouse_buttons |= GameInputMouseMiddleButton;
        if (is_down(VK_XBUTTON1)) eff_mouse_buttons |= GameInputMouseButton4;
        if (is_down(VK_XBUTTON2)) eff_mouse_buttons |= GameInputMouseButton5;
        ApplyVirtualGamepadFallback(out_gamepad, out_keys, eff_mouse_buttons, raw_mouse_dx, raw_mouse_dy);
    }
#else
    (void)preferred_slot;
    if (virtual_gamepad_fallback_enabled) {
        ApplyVirtualGamepadFallback(out_gamepad, out_keys, raw_mouse_buttons, raw_mouse_dx, raw_mouse_dy);
    }
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
            const XInputApi& xi = GetXInput();
            if (xi.set_state) {
                OsXInputVibration vib{};
                vib.wLeftMotorSpeed = xi_left;
                vib.wRightMotorSpeed = xi_right;
                (void)xi.set_state(slot, &vib);  // Safely no-ops if no physical XInput pad is attached
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
};

LiveGameInputRegistry& GetLiveGameInputRegistry() {
    static LiveGameInputRegistry* reg = new LiveGameInputRegistry();
    return *reg;
}

class GameInputImpl final : public IGameInput, public IGameInputEx {
public:
    GameInputImpl() {
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
            GameInputKindKeyboard | GameInputKindUiNavigation,
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
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.instances.insert(this);
    }

    ~GameInputImpl() {
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
        GameInputMouseState mouse_snapshot{};
        int64_t raw_dx = 0;
        int64_t raw_dy = 0;
        {
            std::lock_guard<std::mutex> mlk(raw_mouse_mutex_);
            if (input_allowed) {
                mouse_snapshot.buttons = mouse_buttons_;
                mouse_snapshot.positionX = mouse_cumulative_x_;
                mouse_snapshot.positionY = mouse_cumulative_y_;
                mouse_snapshot.wheelX = mouse_cumulative_wheel_x_;
                mouse_snapshot.wheelY = mouse_cumulative_wheel_y_;
                raw_dx = pending_mouse_dx_;
                raw_dy = pending_mouse_dy_;
            }
        }

        GameInputGamepadState gp{};
        std::vector<GameInputKeyState> keys;
        const bool vpad_fallback = virtual_gamepad_fallback_.load(std::memory_order_acquire);

        bool use_injected = false;
        bool injected_gp_explicit = false;
        if (input_allowed) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (test_inject_enabled_.load(std::memory_order_acquire)) {
                gp = injected_gamepad_;
                keys = injected_keys_;
                use_injected = true;
                injected_gp_explicit = injected_gamepad_explicit_;
            }
        }

        if (input_allowed && use_injected && !injected_gp_explicit && vpad_fallback) {
            // Synthesize virtual gamepad state from injected keyboard + raw mouse state.
            ApplyVirtualGamepadFallback(gp, keys, mouse_snapshot.buttons, raw_dx, raw_dy);
        }

        if (input_allowed && !use_injected) {
            uint32_t active_slot = target_dev->ActiveSlot();
            PollGamepadAndKeyboard(
                active_slot,
                vpad_fallback,
                mouse_snapshot.buttons,
                raw_dx,
                raw_dy,
                gp,
                keys,
                active_slot);
            if (target_dev == default_device_) {
                target_dev->SetActiveSlot(active_slot);
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
        const uint64_t ts = QueryTimestampMicros();
        for (auto* d : initial_devices) {
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
        std::vector<uint8_t> buf(data_size);
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(h_raw_input),
                              RID_INPUT,
                              buf.data(),
                              &data_size,
                              sizeof(RAWINPUTHEADER)) != data_size) {
            return GAMEINPUT_E_INVALIDARG;
        }
        const auto* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
        if (raw->header.dwType != RIM_TYPEMOUSE) {
            return GAMEINPUT_S_OK;
        }
        const RAWMOUSE& m = raw->data.mouse;
        int32_t dx = 0;
        int32_t dy = 0;
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0u) {
            dx = static_cast<int32_t>(m.lLastX);
            dy = static_cast<int32_t>(m.lLastY);
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
        std::lock_guard<std::mutex> lk(raw_mouse_mutex_);
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
        return GAMEINPUT_S_OK;
#else
        (void)h_raw_input;
        return GAMEINPUT_E_INVALIDARG;
#endif
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

#if defined(_WIN32)
    static bool ProcessOwnsVisibleGuiWindow() noexcept {
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
        return ctx.found;
    }
#endif

    bool ShouldAllowInput() const noexcept {
        const uint32_t policy = focus_policy_.load(std::memory_order_acquire);
        if ((policy & GameInputEnableBackgroundInput) != 0u) {
            return true;
        }
#if defined(_WIN32)
        HWND fg = GetForegroundWindow();
        HWND target = reinterpret_cast<HWND>(target_hwnd_.load(std::memory_order_acquire));
        if (target && IsWindow(target) != FALSE) {
            if (!fg) {
                return false;
            }
            return (fg == target) ||
                   (IsChild(target, fg) != FALSE) ||
                   (GetAncestor(fg, GA_ROOTOWNER) == target);
        }
        const DWORD self_pid = GetCurrentProcessId();
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
        return !ProcessOwnsVisibleGuiWindow();
#else
        return true;
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> focus_policy_{GameInputDefaultFocusPolicy};
    std::atomic<void*> target_hwnd_{nullptr};
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

}  // extern "C"
