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
    GameInputGamepadMenu = 0x00000001u,
    GameInputGamepadView = 0x00000002u,
    GameInputGamepadA = 0x00000004u,
    GameInputGamepadB = 0x00000008u,
    GameInputGamepadX = 0x00000010u,
    GameInputGamepadY = 0x00000020u,
    GameInputGamepadDPadUp = 0x00000040u,
    GameInputGamepadDPadDown = 0x00000080u,
    GameInputGamepadDPadLeft = 0x00000100u,
    GameInputGamepadDPadRight = 0x00000200u,
    GameInputGamepadLeftShoulder = 0x00000400u,
    GameInputGamepadRightShoulder = 0x00000800u,
    GameInputGamepadLeftThumbstick = 0x00001000u,
    GameInputGamepadRightThumbstick = 0x00002000u,
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

struct GameInputUsage {
    uint16_t page;
    uint16_t id;
};

struct GameInputVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t build;
    uint16_t revision;
};

struct GameInputString {
    uint32_t sizeInBytes;
    uint32_t codePointCount;
    const char* data;
};

struct GameInputMouseState {
    uint32_t buttons;
    int64_t positionX;
    int64_t positionY;
    int64_t wheelX;
    int64_t wheelY;
};

// Official Microsoft GDK GameInputDeviceInfo layout (size = 0x140 = 320 bytes).
struct GameInputDeviceInfo {
    uint32_t infoSize;
    uint16_t vendorId;
    uint16_t productId;
    uint16_t revisionNumber;
    uint8_t interfaceNumber;
    uint8_t collectionNumber;
    GameInputUsage usage;
    GameInputVersion hardwareVersion;
    GameInputVersion firmwareVersion;
    uint8_t deviceId[32];
    uint8_t deviceRootId[32];
    GameInputDeviceFamily deviceFamily;
    uint32_t capabilities;
    uint32_t supportedInput;
    uint32_t supportedRumbleMotors;
    uint32_t inputReportCount;
    uint32_t outputReportCount;
    uint32_t featureReportCount;
    uint32_t controllerAxisCount;
    uint32_t controllerButtonCount;
    uint32_t controllerSwitchCount;
    uint32_t touchPointCount;
    uint32_t touchSensorCount;
    uint32_t forceFeedbackMotorCount;
    uint32_t hapticFeedbackMotorCount;
    uint32_t deviceStringCount;
    uint32_t deviceDescriptorSize;
    const void* inputReportInfo;
    const void* outputReportInfo;
    const void* featureReportInfo;
    const void* controllerAxisInfo;
    const void* controllerButtonInfo;
    const void* controllerSwitchInfo;
    const void* keyboardInfo;
    const void* mouseInfo;
    const void* touchSensorInfo;
    const void* motionInfo;
    const void* arcadeStickInfo;
    const void* flightStickInfo;
    const void* gamepadInfo;
    const void* racingWheelInfo;
    const void* uiNavigationInfo;
    const void* forceFeedbackMotorInfo;
    const void* hapticFeedbackMotorInfo;
    const GameInputString* displayName;
    const GameInputString* deviceStrings;
    const void* deviceDescriptorData;
};

struct IGameInputDevice;
struct IGameInputReading;
struct IGameInput;

using GameInputReadingCallback = void(GAMEINPUT_COMPAT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputReading* reading,
    bool hasOverrunOccurred);

using GameInputDeviceCallback = void(GAMEINPUT_COMPAT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus);

using GameInputSystemButtonCallback = void(GAMEINPUT_COMPAT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentButtons,
    uint32_t previousButtons);

using GameInputKeyboardLayoutCallback = void(GAMEINPUT_COMPAT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentLayout,
    uint32_t previousLayout);

// Official Microsoft GDK IGameInputDevice vtable (slots 0..21).
struct __declspec(uuid("31DD86FB-4C1B-408A-868F-439B3CD47125"))
IGameInputDevice : public IUnknown {
    virtual const GameInputDeviceInfo* GAMEINPUT_COMPAT_CALL GetDeviceInfo() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetDeviceStatus() = 0;
    virtual void GAMEINPUT_COMPAT_CALL GetBatteryState(void* state) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL CreateForceFeedbackEffect(
        uint32_t motorIndex, const void* params, void** effect) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL IsForceFeedbackMotorPoweredOn(uint32_t motorIndex) = 0;
    virtual void GAMEINPUT_COMPAT_CALL SetForceFeedbackMotorGain(
        uint32_t motorIndex, float masterGain) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL SetHapticMotorState(
        uint32_t motorIndex, const void* params) = 0;
    virtual void GAMEINPUT_COMPAT_CALL SetRumbleState(
        const GameInputRumbleParams* params) = 0;
    virtual void GAMEINPUT_COMPAT_CALL SetInputSynchronizationState(bool enabled) = 0;
    virtual void GAMEINPUT_COMPAT_CALL SendInputSynchronizationHint() = 0;
    virtual void GAMEINPUT_COMPAT_CALL PowerOff() = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL CreateRawDeviceReport(
        uint32_t reportId, uint32_t reportKind, void** report) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL GetRawDeviceFeature(
        uint32_t reportId, void** report) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL SetRawDeviceFeature(void* report) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL SendRawDeviceOutput(void* report) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL SendRawDeviceOutputWithResponse(
        void* requestReport, void** responseReport) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL ExecuteRawDeviceIoControl(
        uint32_t controlCode, size_t inputBufferSize, const void* inputBuffer,
        size_t outputBufferSize, void* outputBuffer, size_t* outputSize) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL AcquireExclusiveRawDeviceAccess(
        uint64_t timeoutInMicroseconds) = 0;
    virtual void GAMEINPUT_COMPAT_CALL ReleaseExclusiveRawDeviceAccess() = 0;
};

// Official Microsoft GDK IGameInputReading vtable (slots 0..24).
struct __declspec(uuid("2156947A-E1FA-4DE0-A30B-D812931DBD8D"))
IGameInputReading : public IUnknown {
    virtual GameInputKind GAMEINPUT_COMPAT_CALL GetInputKind() = 0;
    virtual uint64_t GAMEINPUT_COMPAT_CALL GetSequenceNumber(GameInputKind inputKind) = 0;
    virtual uint64_t GAMEINPUT_COMPAT_CALL GetTimestamp() = 0;
    virtual void GAMEINPUT_COMPAT_CALL GetDevice(IGameInputDevice** device) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetRawReport(void** report) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerAxisCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerAxisState(
        uint32_t stateArrayCount, float* stateArray) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerButtonCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerButtonState(
        uint32_t stateArrayCount, bool* stateArray) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerSwitchCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetControllerSwitchState(
        uint32_t stateArrayCount, uint32_t* stateArray) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetKeyCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetMouseState(GameInputMouseState* state) = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetTouchCount() = 0;
    virtual uint32_t GAMEINPUT_COMPAT_CALL GetTouchState(
        uint32_t stateArrayCount, void* stateArray) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetMotionState(void* state) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetArcadeStickState(void* state) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetFlightStickState(void* state) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetGamepadState(GameInputGamepadState* state) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetRacingWheelState(void* state) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL GetUiNavigationState(void* state) = 0;
};

// Official Microsoft GDK IGameInput vtable (slots 0..21).
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
    virtual HRESULT GAMEINPUT_COMPAT_CALL GetTemporalReading(
        uint64_t timestamp,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL RegisterReadingCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        float analogThreshold,
        void* context,
        GameInputReadingCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        GameInputDeviceCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL RegisterSystemButtonCallback(
        IGameInputDevice* device,
        uint32_t buttonFilter,
        void* context,
        GameInputSystemButtonCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL RegisterKeyboardLayoutCallback(
        IGameInputDevice* device,
        void* context,
        GameInputKeyboardLayoutCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual void GAMEINPUT_COMPAT_CALL StopCallback(uint64_t callbackToken) = 0;
    virtual bool GAMEINPUT_COMPAT_CALL UnregisterCallback(
        uint64_t callbackToken, uint64_t timeoutInMicroseconds) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL CreateDispatcher(void** dispatcher) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL CreateAggregateDevice(
        GameInputKind inputKind, IGameInputDevice** device) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL FindDeviceFromId(
        const void* value, IGameInputDevice** device) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL FindDeviceFromObject(
        void* value, IGameInputDevice** device) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL FindDeviceFromPlatformHandle(
        void* value, IGameInputDevice** device) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL FindDeviceFromPlatformString(
        const wchar_t* value, IGameInputDevice** device) = 0;
    virtual HRESULT GAMEINPUT_COMPAT_CALL EnableOemDeviceSupport(
        uint16_t vendorId, uint16_t productId,
        uint8_t interfaceNumber, uint8_t collectionNumber) = 0;
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
