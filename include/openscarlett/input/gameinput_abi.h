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
constexpr int32_t GAMEINPUT_E_NOTIMPL = static_cast<int32_t>(0x80004001u);
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

// Official Microsoft GDK GameInputDeviceInfo layout (size = 0x140 = 320 bytes).
struct GameInputDeviceInfo {
    uint32_t infoSize;                   // +0x00
    uint16_t vendorId;                   // +0x04
    uint16_t productId;                  // +0x06
    uint16_t revisionNumber;             // +0x08
    uint8_t interfaceNumber;             // +0x0A
    uint8_t collectionNumber;            // +0x0B
    GameInputUsage usage;                // +0x0C
    GameInputVersion hardwareVersion;    // +0x10
    GameInputVersion firmwareVersion;    // +0x18
    uint8_t deviceId[32];                // +0x20
    uint8_t deviceRootId[32];            // +0x40
    GameInputDeviceFamily deviceFamily;  // +0x60
    uint32_t capabilities;               // +0x64
    uint32_t supportedInput;             // +0x68
    uint32_t supportedRumbleMotors;      // +0x6C
    uint32_t inputReportCount;           // +0x70
    uint32_t outputReportCount;          // +0x74
    uint32_t featureReportCount;         // +0x78
    uint32_t controllerAxisCount;        // +0x7C
    uint32_t controllerButtonCount;      // +0x80
    uint32_t controllerSwitchCount;      // +0x84
    uint32_t touchPointCount;            // +0x88
    uint32_t touchSensorCount;           // +0x8C
    uint32_t forceFeedbackMotorCount;    // +0x90
    uint32_t hapticFeedbackMotorCount;   // +0x94
    uint32_t deviceStringCount;          // +0x98
    uint32_t deviceDescriptorSize;       // +0x9C
    const void* inputReportInfo;         // +0xA0
    const void* outputReportInfo;        // +0xA8
    const void* featureReportInfo;       // +0xB0
    const void* controllerAxisInfo;      // +0xB8
    const void* controllerButtonInfo;    // +0xC0
    const void* controllerSwitchInfo;    // +0xC8
    const void* keyboardInfo;            // +0xD0
    const void* mouseInfo;               // +0xD8
    const void* touchSensorInfo;         // +0xE0
    const void* motionInfo;              // +0xE8
    const void* arcadeStickInfo;         // +0xF0
    const void* flightStickInfo;         // +0xF8
    const void* gamepadInfo;             // +0x100
    const void* racingWheelInfo;         // +0x108
    const void* uiNavigationInfo;        // +0x110
    const void* forceFeedbackMotorInfo;  // +0x118
    const void* hapticFeedbackMotorInfo; // +0x120
    const GameInputString* displayName;  // +0x128
    const GameInputString* deviceStrings;// +0x130
    const void* deviceDescriptorData;    // +0x138
};

// Forward declarations for COM interfaces.
struct IGameInputDevice;
struct IGameInputReading;
struct IGameInput;

using GameInputReadingCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputReading* reading,
    bool hasOverrunOccurred);

using GameInputDeviceCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus);

using GameInputSystemButtonCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentButtons,
    uint32_t previousButtons);

using GameInputKeyboardLayoutCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentLayout,
    uint32_t previousLayout);

// Official Microsoft GDK IGameInputDevice vtable (slots 0..21).
struct IGameInputDevice {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;                          // 0 (+0x00)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;                     // 1 (+0x08)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;                    // 2 (+0x10)
    virtual const GameInputDeviceInfo* OPENSCARLETT_GAMEINPUT_CALL GetDeviceInfo() = 0;  // 3 (+0x18)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetDeviceStatus() = 0;            // 4 (+0x20)
    virtual void OPENSCARLETT_GAMEINPUT_CALL GetBatteryState(void* state) = 0;     // 5 (+0x28)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL CreateForceFeedbackEffect(
        uint32_t motorIndex, const void* params, void** effect) = 0;               // 6 (+0x30)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL IsForceFeedbackMotorPoweredOn(
        uint32_t motorIndex) = 0;                                                  // 7 (+0x38)
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetForceFeedbackMotorGain(
        uint32_t motorIndex, float masterGain) = 0;                                // 8 (+0x40)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL SetHapticMotorState(
        uint32_t motorIndex, const void* params) = 0;                              // 9 (+0x48)
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetRumbleState(
        const GameInputRumbleParams* params) = 0;                                  // 10 (+0x50)
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetInputSynchronizationState(
        bool enabled) = 0;                                                         // 11 (+0x58)
    virtual void OPENSCARLETT_GAMEINPUT_CALL SendInputSynchronizationHint() = 0;   // 12 (+0x60)
    virtual void OPENSCARLETT_GAMEINPUT_CALL PowerOff() = 0;                       // 13 (+0x68)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL CreateRawDeviceReport(
        uint32_t reportId, uint32_t reportKind, void** report) = 0;                // 14 (+0x70)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetRawDeviceFeature(
        uint32_t reportId, void** report) = 0;                                     // 15 (+0x78)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL SetRawDeviceFeature(
        void* report) = 0;                                                         // 16 (+0x80)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL SendRawDeviceOutput(
        void* report) = 0;                                                         // 17 (+0x88)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL SendRawDeviceOutputWithResponse(
        void* requestReport, void** responseReport) = 0;                           // 18 (+0x90)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL ExecuteRawDeviceIoControl(
        uint32_t controlCode, size_t inputBufferSize, const void* inputBuffer,
        size_t outputBufferSize, void* outputBuffer, size_t* outputSize) = 0;      // 19 (+0x98)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL AcquireExclusiveRawDeviceAccess(
        uint64_t timeoutInMicroseconds) = 0;                                       // 20 (+0xA0)
    virtual void OPENSCARLETT_GAMEINPUT_CALL ReleaseExclusiveRawDeviceAccess() = 0;// 21 (+0xA8)
};

// Official Microsoft GDK IGameInputReading vtable (slots 0..24).
struct IGameInputReading {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;                          // 0 (+0x00)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;                     // 1 (+0x08)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;                    // 2 (+0x10)
    virtual GameInputKind OPENSCARLETT_GAMEINPUT_CALL GetInputKind() = 0;          // 3 (+0x18)
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetSequenceNumber(
        GameInputKind inputKind) = 0;                                              // 4 (+0x20)
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetTimestamp() = 0;               // 5 (+0x28)
    virtual void OPENSCARLETT_GAMEINPUT_CALL GetDevice(
        IGameInputDevice** device) = 0;                                            // 6 (+0x30)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetRawReport(void** report) = 0;      // 7 (+0x38)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerAxisCount() = 0;     // 8 (+0x40)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerAxisState(
        uint32_t stateArrayCount, float* stateArray) = 0;                          // 9 (+0x48)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerButtonCount() = 0;   // 10 (+0x50)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerButtonState(
        uint32_t stateArrayCount, bool* stateArray) = 0;                           // 11 (+0x58)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerSwitchCount() = 0;   // 12 (+0x60)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetControllerSwitchState(
        uint32_t stateArrayCount, uint32_t* stateArray) = 0;                       // 13 (+0x68)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyCount() = 0;                // 14 (+0x70)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) = 0;              // 15 (+0x78)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetMouseState(
        GameInputMouseState* state) = 0;                                           // 16 (+0x80)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetTouchCount() = 0;              // 17 (+0x88)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL GetTouchState(
        uint32_t stateArrayCount, void* stateArray) = 0;                           // 18 (+0x90)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetMotionState(void* state) = 0;      // 19 (+0x98)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetArcadeStickState(void* state) = 0; // 20 (+0xA0)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetFlightStickState(void* state) = 0; // 21 (+0xA8)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetGamepadState(
        GameInputGamepadState* state) = 0;                                         // 22 (+0xB0)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetRacingWheelState(void* state) = 0; // 23 (+0xB8)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL GetUiNavigationState(void* state) = 0;// 24 (+0xC0)
};

// Official Microsoft GDK IGameInput vtable (slots 0..21).
struct IGameInput {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;                          // 0 (+0x00)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;                     // 1 (+0x08)
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;                    // 2 (+0x10)
    virtual uint64_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentTimestamp() = 0;        // 3 (+0x18)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;                                          // 4 (+0x20)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;                                          // 5 (+0x28)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;                                          // 6 (+0x30)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL GetTemporalReading(
        uint64_t timestamp,
        IGameInputDevice* device,
        IGameInputReading** reading) = 0;                                          // 7 (+0x38)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterReadingCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        float analogThreshold,
        void* context,
        GameInputReadingCallback callbackFunc,
        uint64_t* callbackToken) = 0;                                              // 8 (+0x40)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        GameInputDeviceCallback callbackFunc,
        uint64_t* callbackToken) = 0;                                              // 9 (+0x48)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterSystemButtonCallback(
        IGameInputDevice* device,
        uint32_t buttonFilter,
        void* context,
        GameInputSystemButtonCallback callbackFunc,
        uint64_t* callbackToken) = 0;                                              // 10 (+0x50)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterKeyboardLayoutCallback(
        IGameInputDevice* device,
        void* context,
        GameInputKeyboardLayoutCallback callbackFunc,
        uint64_t* callbackToken) = 0;                                              // 11 (+0x58)
    virtual void OPENSCARLETT_GAMEINPUT_CALL StopCallback(
        uint64_t callbackToken) = 0;                                               // 12 (+0x60)
    virtual bool OPENSCARLETT_GAMEINPUT_CALL UnregisterCallback(
        uint64_t callbackToken,
        uint64_t timeoutInMicroseconds) = 0;                                       // 13 (+0x68)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL CreateDispatcher(
        void** dispatcher) = 0;                                                    // 14 (+0x70)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL CreateAggregateDevice(
        GameInputKind inputKind,
        IGameInputDevice** device) = 0;                                            // 15 (+0x78)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromId(
        const void* value,
        IGameInputDevice** device) = 0;                                            // 16 (+0x80)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromObject(
        void* value,
        IGameInputDevice** device) = 0;                                            // 17 (+0x88)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromPlatformHandle(
        void* value,
        IGameInputDevice** device) = 0;                                            // 18 (+0x90)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL FindDeviceFromPlatformString(
        const wchar_t* value,
        IGameInputDevice** device) = 0;                                            // 19 (+0x98)
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL EnableOemDeviceSupport(
        uint16_t vendorId,
        uint16_t productId,
        uint8_t interfaceNumber,
        uint8_t collectionNumber) = 0;                                             // 20 (+0xA0)
    virtual void OPENSCARLETT_GAMEINPUT_CALL SetFocusPolicy(uint32_t policy) = 0;  // 21 (+0xA8)
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
