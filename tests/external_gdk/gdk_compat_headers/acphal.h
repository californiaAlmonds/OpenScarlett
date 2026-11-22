#pragma once

// MIT Xbox Audio Co-Processor HAL (<acphal.h>) public ABI header.

#include <cstdint>
#include <unknwn.h>
#include <windows.h>

#define ACPHAL_COMPAT_API __declspec(dllimport)
#define ACPHAL_COMPAT_CALL __stdcall

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

struct __declspec(uuid("78C8F2A1-5B4C-4E3D-9A1F-623D8C449102"))
IAcpHal : public IUnknown {
    virtual HRESULT ACPHAL_COMPAT_CALL ConfigureXmaContext(
        uint32_t contextIndex, const AcpHalXmaContextDesc* desc) = 0;
    virtual HRESULT ACPHAL_COMPAT_CALL SubmitCommand(const AcpHalCommand* command) = 0;
    virtual HRESULT ACPHAL_COMPAT_CALL SubmitCommands(
        uint32_t count, const AcpHalCommand* commands) = 0;
    virtual HRESULT ACPHAL_COMPAT_CALL WaitIdle() = 0;
    virtual HRESULT ACPHAL_COMPAT_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) = 0;
};

extern "C" {

ACPHAL_COMPAT_API HRESULT ACPHAL_COMPAT_CALL AcpHalCreate(IAcpHal** ppAcpHal);
ACPHAL_COMPAT_API HRESULT ACPHAL_COMPAT_CALL AcpHalAllocateShapeContexts(
    const AcpHalShapeContexts* pRequested,
    AcpHalShapeContexts* pAllocated);
ACPHAL_COMPAT_API HRESULT ACPHAL_COMPAT_CALL AcpHalReleaseShapeContexts(
    AcpHalShapeContexts* pAllocated);

}  // extern "C"
