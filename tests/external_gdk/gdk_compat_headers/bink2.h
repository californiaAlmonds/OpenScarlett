#pragma once

// MIT Bink2 for Scarlett (<bink2.h>) public C ABI header
// backed by Bink2Scarlett.dll.

#include <cstddef>
#include <cstdint>

#define BINK2_COMPAT_API __declspec(dllimport)
#define BINK2_COMPAT_CALL __stdcall

constexpr uint32_t BINKSURFACE32 = 3u;
constexpr uint32_t BINKSURFACE32RGBA = 6u;
constexpr uint32_t BINKCOPYALL = 0x80000000u;

struct BinkStruct {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t OpenFlags;
    uint32_t Paused;
    uint32_t SoundOn;
};
typedef BinkStruct* HBink;

struct BinkSummary {
    uint32_t Width;
    uint32_t Height;
    uint32_t TotalFrames;
    uint32_t CurrentFrame;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t TotalDecodedFrames;
    uint32_t AudioTrackCount;
    uint32_t IsBink2;
    char FourCC[8];
};

extern "C" {

BINK2_COMPAT_API HBink BINK2_COMPAT_CALL BinkOpen(const char* name, uint32_t flags);
BINK2_COMPAT_API HBink BINK2_COMPAT_CALL BinkOpenBuffer(
    const void* buffer,
    size_t bufferSize,
    uint32_t flags);
BINK2_COMPAT_API void BINK2_COMPAT_CALL BinkClose(HBink bink);
BINK2_COMPAT_API int32_t BINK2_COMPAT_CALL BinkDoFrame(HBink bink);
BINK2_COMPAT_API void BINK2_COMPAT_CALL BinkNextFrame(HBink bink);
BINK2_COMPAT_API int32_t BINK2_COMPAT_CALL BinkWait(HBink bink);
BINK2_COMPAT_API int32_t BINK2_COMPAT_CALL BinkCopyToBuffer(
    HBink bink,
    void* dest,
    int32_t destPitch,
    uint32_t destHeight,
    uint32_t destX,
    uint32_t destY,
    uint32_t flags);
BINK2_COMPAT_API void BINK2_COMPAT_CALL BinkGoto(HBink bink, uint32_t frame, uint32_t flags);
BINK2_COMPAT_API void BINK2_COMPAT_CALL BinkGetSummary(HBink bink, BinkSummary* summary);
BINK2_COMPAT_API const char* BINK2_COMPAT_CALL BinkGetError(void);
BINK2_COMPAT_API void BINK2_COMPAT_CALL BinkSetSoundOnOff(HBink bink, int32_t onOff);
BINK2_COMPAT_API int32_t BINK2_COMPAT_CALL BinkPause(HBink bink, int32_t pause);

}  // extern "C"
