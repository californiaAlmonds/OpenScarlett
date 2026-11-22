#pragma once

// Microsoft GameInput (GameInput.dll) COM ABI definitions for OpenScarlett.
// Self-contained (does not include <windows.h> or <GameInput.h>).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_GAMEINPUT)
#    define OPENSCARLETT_GAMEINPUT_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_GAMEINPUT_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_GAMEINPUT_CALL __stdcall
#else
#  define OPENSCARLETT_GAMEINPUT_API __attribute__((visibility("default")))
#  define OPENSCARLETT_GAMEINPUT_CALL
#endif

// Standard GameInput HRESULT codes.
constexpr int32_t GAMEINPUT_S_OK = 0x00000000;
constexpr int32_t GAMEINPUT_E_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t GAMEINPUT_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t GAMEINPUT_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t GAMEINPUT_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t GAMEINPUT_E_DEVICE_DISCONNECTED = static_cast<int32_t>(0x838A0001u);
constexpr int32_t GAMEINPUT_E_DEVICE_NOT_FOUND = static_cast<int32_t>(0x838A0002u);
constexpr int32_t GAMEINPUT_E_READING_NOT_FOUND = static_cast<int32_t>(0x838A0003u);

struct GameInputGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(GameInputGuid) == 16, "GameInputGuid must be 16 bytes");

constexpr GameInputGuid IID_OS_GameInput_IUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GameInputGuid IID_IGameInput = {
    0x11BE2A7Eu, 0x4254u, 0x445Au, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x18}};
constexpr GameInputGuid IID_IGameInputReading = {
    0x2156947Au, 0xE1FAu, 0x4DE0u, {0xA3, 0x0B, 0xD8, 0x12, 0x93, 0x1D, 0xBD, 0x8D}};
constexpr GameInputGuid IID_IGameInputDevice = {
    0x31DD86FBu, 0x4C1Bu, 0x408Au, {0x86, 0x8F, 0x43, 0x9B, 0x3C, 0xD4, 0x71, 0x25}};

enum GameInputKind : uint32_t {
    GameInputKindUnknown = 0x00000000u,
    GameInputKindRawDeviceReport = 0x00000001u,
    GameInputKindControllerAxis = 0x00000002u,
    GameInputKindControllerButton = 0x00000004u,
    GameInputKindControllerSwitch = 0x00000008u,
    GameInputKindController = 0x0000000Eu,
    GameInputKindKeyboard = 0x00000010u,
    GameInputKindMouse = 0x00000020u,
    GameInputKindTouch = 0x00000100u,
    GameInputKindMotion = 0x00001000u,
    GameInputKindArcadeStick = 0x00010000u,
    GameInputKindFlightStick = 0x00020000u,
    GameInputKindGamepad = 0x00040000u,
    GameInputKindRacingWheel = 0x00080000u,
    GameInputKindUiNavigation = 0x01000000u,
};

enum GameInputGamepadButtons : uint32_t {
    GameInputGamepadNone = 0x00000000u,
    GameInputGamepadMenu = 0x00000004u,
    GameInputGamepadView = 0x00000008u,
    GameInputGamepadA = 0x00000010u,
    GameInputGamepadB = 0x00000020u,
    GameInputGamepadX = 0x00000040u,
    GameInputGamepadY = 0x00000080u,
    GameInputGamepadDPadUp = 0x00000100u,
    GameInputGamepadDPadDown = 0x00000200u,
    GameInputGamepadDPadLeft = 0x00000400u,
    GameInputGamepadDPadRight = 0x00000800u,
    GameInputGamepadLeftShoulder = 0x00001000u,
    GameInputGamepadRightShoulder = 0x00002000u,
    GameInputGamepadLeftThumbstick = 0x00004000u,
    GameInputGamepadRightThumbstick = 0x00008000u,
};

enum GameInputDeviceStatus : uint32_t {
    GameInputDeviceNoStatus = 0x00000000u,
    GameInputDeviceConnected = 0x00000001u,
    GameInputDeviceInputEnabled = 0x00000002u,
    GameInputDeviceOutputEnabled = 0x00000004u,
    GameInputDeviceAnyStatus = 0xFFFFFFFFu,
};

enum GameInputDeviceFamily : int32_t {
    GameInputFamilyVirtual = -1,
    GameInputFamilyAggregate = 0,
    GameInputFamilyXboxOne = 1,
    GameInputFamilyXbox360 = 2,
    GameInputFamilyHid = 3,
    GameInputFamilyI8042 = 4,
};

enum GameInputRumbleMotors : uint32_t {
    GameInputRumbleNone = 0x00000000u,
    GameInputRumbleLowFrequency = 0x00000001u,
    GameInputRumbleHighFrequency = 0x00000002u,
    GameInputRumbleLeftTrigger = 0x00000004u,
    GameInputRumbleRightTrigger = 0x00000008u,
};

enum GameInputFocusPolicy : uint32_t {
    GameInputDefaultFocusPolicy = 0x00000000u,
    GameInputDisableBackgroundInput = 0x00000001u,
    GameInputExclusiveForegroundInput = 0x00000002u,
    GameInputEnableBackgroundInput = 0x00000004u,
};

struct GameInputGamepadState {
    uint32_t buttons;
    float leftTrigger;
    float rightTrigger;
    float leftThumbstickX;
    float leftThumbstickY;
    float rightThumbstickX;
    float rightThumbstickY;
};

struct GameInputKeyState {
    uint32_t scanCode;
    uint32_t codePoint;
    uint8_t virtualKey;
    bool isDeadKey;
};

enum GameInputMouseButtons : uint32_t {
    GameInputMouseNone = 0x00000000u,
    GameInputMouseLeftButton = 0x00000001u,
    GameInputMouseRightButton = 0x00000002u,
    GameInputMouseMiddleButton = 0x00000004u,
    GameInputMouseButton4 = 0x00000008u,
    GameInputMouseButton5 = 0x00000010u,
    GameInputMouseWheelTiltLeft = 0x00000020u,
    GameInputMouseWheelTiltRight = 0x00000040u,
};

struct GameInputMouseState {
    uint32_t buttons;
    int64_t positionX;
    int64_t positionY;
    int64_t wheelX;
    int64_t wheelY;
};

struct GameInputRawMouseFrameDelta {
    int64_t deltaX;
    int64_t deltaY;
    int64_t wheelX;
    int64_t wheelY;
    uint32_t buttons;
    uint32_t packetCount;
    uint64_t totalPacketsReceived;
    uint32_t ringBufferedPackets;
    uint32_t reserved;
};

struct GameInputRumbleParams {
    float lowFrequency;
    float highFrequency;
    float leftTrigger;
    float rightTrigger;
};

struct GameInputDeviceInfo {
    uint32_t infoSize;
    uint16_t vendorId;
    uint16_t productId;
    uint16_t revisionNumber;
    uint8_t interfaceNumber;
    uint8_t collectionNumber;
    uint8_t deviceId[32];
    GameInputDeviceFamily deviceFamily;
    uint32_t supportedInput;
    uint32_t supportedRumbleMotors;
    const char* displayName;
};

// Forward declarations for COM interfaces.
struct IGameInputDevice;
struct IGameInputReading;
struct IGameInput;

struct IGameInputDevice {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;
    virtual const GameInputDeviceInfo* OPENSCARLETT_GAMEINPUT_CALL GetDeviceInfo() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetDeviceStatus() = 0;
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetRumbleState(
        const GameInputRumbleParams* params) = 0;
};

struct IGameInputReading {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;
    virtual GameInputKind OPENSCARLETT_GAMEINPUT_CALL GetInputKind() = 0;
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetSequenceNumber(GameInputKind inputKind) = 0;
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetTimestamp() = 0;
    virtual void OPENSCARLETT_GAMEINPUT_CALL GetDevice(IGameInputDevice** device) = 0;
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetGamepadState(GameInputGamepadState* state) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyCount() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetMouseState(GameInputMouseState* state) = 0;
};

struct IGameInput {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentTimestamp() = 0;
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetFocusPolicy(uint32_t policy) = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL GameInputCreate(
    IGameInput** gameInput);

// OpenScarlett optional window-focus & deterministic test injection helpers.
// `hwnd` accepts a Win32 HWND (implicitly convertible to void*) or nullptr.
OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputSetTargetWindow(IGameInput* input, void* hwnd);

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputInjectTestState(
    IGameInput* input,
    const GameInputGamepadState* gp,
    const GameInputKeyState* keys,
    uint32_t key_count,
    int enable);

// Native Win32 Raw Input mouse registration, WM_INPUT ingestion, deterministic test injection,
// and lossless per-frame mouse delta consumption (never drops deltas on ring-buffer overflow).
OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputRegisterRawMouse(
    IGameInput* input,
    void* hwnd,
    uint32_t rawInputFlags);

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputProcessRawInput(
    IGameInput* input,
    void* hRawInput);

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputInjectRawMouseDelta(
    IGameInput* input,
    int32_t deltaX,
    int32_t deltaY,
    int32_t wheelX,
    int32_t wheelY,
    uint32_t buttons);

OPENSCARLETT_GAMEINPUT_API int32_t OPENSCARLETT_GAMEINPUT_CALL
OpenScarlettGameInputConsumeFrameMouseDelta(
    IGameInput* input,
    GameInputRawMouseFrameDelta* outDelta);

#ifdef __cplusplus
}
#endif
