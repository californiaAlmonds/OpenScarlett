#pragma once

// Xbox Audio Co-Processor HAL (AcpHal.dll) ABI definitions for OpenScarlett.
// Routes hardware ACP XMA2 decode commands directly to openscarlett::Xma2Decoder.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_ACPHAL)
#    define OPENSCARLETT_ACPHAL_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_ACPHAL_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_ACPHAL_CALL __stdcall
#else
#  define OPENSCARLETT_ACPHAL_API __attribute__((visibility("default")))
#  define OPENSCARLETT_ACPHAL_CALL
#endif

constexpr int32_t ACPHAL_S_OK = 0x00000000;
constexpr int32_t ACPHAL_E_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t ACPHAL_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t ACPHAL_E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t ACPHAL_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t ACPHAL_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t ACPHAL_E_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8007007Au);

struct AcpHalGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(AcpHalGuid) == 16, "AcpHalGuid must be 16 bytes");

constexpr AcpHalGuid IID_OS_AcpHal_IUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr AcpHalGuid IID_IAcpHal = {
    0x78C8F2A1u, 0x5B4Cu, 0x4E3Du, {0x9A, 0x1F, 0x62, 0x3D, 0x8C, 0x44, 0x91, 0x02}};

enum AcpHalCommandType : uint32_t {
    AcpHalCommand_Nop = 0,
    AcpHalCommand_XmaDecode = 1,
    AcpHalCommand_ResetContext = 2,
    AcpHalCommand_Flush = 3,
};

struct AcpHalXmaContextDesc {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t blockSize;
    uint8_t subframesPerFrame;
    uint8_t quantBits;
    uint16_t reserved;
};

struct AcpHalCommand {
    AcpHalCommandType type;
    uint32_t contextIndex;
    const uint8_t* inputData;
    uint32_t inputBytes;
    float* outputFloatPcm;
    int16_t* outputInt16Pcm;
    uint32_t outputCapacitySamples;
    uint32_t* outSamplesWritten;
    AcpHalXmaContextDesc xmaConfig;
};

struct AcpHalShapeContexts {
    uint32_t numXmaContexts;
    uint32_t numSrcContexts;
    uint32_t numEqContexts;
    uint32_t numDmaContexts;
    void* internalAllocation;
};

struct IAcpHal {
    virtual int32_t OPENSCARLETT_ACPHAL_CALL QueryInterface(
        const AcpHalGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_ACPHAL_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_ACPHAL_CALL Release() = 0;
    virtual int32_t OPENSCARLETT_ACPHAL_CALL ConfigureXmaContext(
        uint32_t contextIndex, const AcpHalXmaContextDesc* desc) = 0;
    virtual int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(const AcpHalCommand* command) = 0;
    virtual int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommands(
        uint32_t count, const AcpHalCommand* commands) = 0;
    virtual int32_t OPENSCARLETT_ACPHAL_CALL WaitIdle() = 0;
    virtual int32_t OPENSCARLETT_ACPHAL_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL AcpHalCreate(IAcpHal** ppAcpHal);
OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL AcpHalAllocateShapeContexts(
    const AcpHalShapeContexts* pRequested,
    AcpHalShapeContexts* pAllocated);
OPENSCARLETT_ACPHAL_API int32_t OPENSCARLETT_ACPHAL_CALL AcpHalReleaseShapeContexts(
    AcpHalShapeContexts* pAllocated);

#ifdef __cplusplus
}
#endif
