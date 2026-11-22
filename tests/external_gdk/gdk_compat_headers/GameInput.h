#pragma once

// MIT Microsoft GameInput (<GameInput.h>) public COM ABI header.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <unknwn.h>
#  include <windows.h>
#  define GAMEINPUT_COMPAT_API __declspec(dllimport)
#  define GAMEINPUT_COMPAT_CALL __stdcall
#else
#  define GAMEINPUT_COMPAT_API
#  define GAMEINPUT_COMPAT_CALL
#endif

constexpr HRESULT GAMEINPUT_E_DEVICE_DISCONNECTED = static_cast<HRESULT>(0x838A0001u);
constexpr HRESULT GAMEINPUT_E_DEVICE_NOT_FOUND = static_cast<HRESULT>(0x838A0002u);
constexpr HRESULT GAMEINPUT_E_READING_NOT_FOUND = static_cast<HRESULT>(0x838A0003u);

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

struct __declspec(uuid("31DD86FB-4C1B-408A-868F-439B3CD47125"))
IGameInputDevice : public IUnknown {
    virtual const GameInputDeviceInfo* GAMEINPUT_COMPAT_CALL GetDeviceInfo() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetDeviceStatus() = 0;
    virtual void GAMEINPUT_COMPAT_CALL SetRumbleState(
        const GameInputRumbleParams* params) = 0;
};

struct __declspec(uuid("2156947A-E1FA-4DE0-A30B-D812931DBD8D"))
IGameInputReading : public IUnknown {
    virtual GameInputKind GAMEINPUT_COMPAT_CALL GetInputKind() = 0;
    virtual uint64_t GAMEINPUT_COMPAT_CALL GetSequenceNumber(GameInputKind inputKind) = 0;
    virtual uint64_t GAMEINPUT_COMPAT_CALL GetTimestamp() = 0;
    virtual void GAMEINPUT_COMPAT_CALL GetDevice(IGameInputDevice** device) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetGamepadState(GameInputGamepadState* state) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetKeyCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;
};

struct __declspec(uuid("11BE2A7E-4254-445A-9C09-FFC40F006918"))
IGameInput : public IUnknown {
    virtual uint64_t GAMEINPUT_COMPAT_CALL GetCurrentTimestamp() = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual void GAMEINPUT_COMPAT_CALL SetFocusPolicy(uint32_t policy) = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

GAMEINPUT_COMPAT_API HRESULT GAMEINPUT_COMPAT_CALL GameInputCreate(
    IGameInput** gameInput);

#ifdef __cplusplus
}
#endif
