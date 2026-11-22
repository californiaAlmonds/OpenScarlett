#pragma once

// MIT Microsoft PIX3 (<pix3.h>) public header backed by pixevt.dll.

#include <cstdint>

#if defined(_WIN32)
#  define PIX_COMPAT_API __declspec(dllimport)
#  define PIX_COMPAT_CALL __stdcall
#else
#  define PIX_COMPAT_API
#  define PIX_COMPAT_CALL
#endif

constexpr uint64_t PIX_COLOR_DEFAULT = 0xFF000000ull;

inline constexpr uint64_t PIX_COLOR(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0xFF000000ull |
           (static_cast<uint64_t>(r) << 16) |
           (static_cast<uint64_t>(g) << 8) |
           static_cast<uint64_t>(b);
}

#ifdef __cplusplus
extern "C" {
#endif

PIX_COMPAT_API void PIX_COMPAT_CALL PIXBeginEventOnCpu(uint64_t color, const char* formatString);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXEndEventOnCpu(void);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXSetMarkerOnCpu(uint64_t color, const char* formatString);

PIX_COMPAT_API void PIX_COMPAT_CALL PIXBeginEventOnCommandList(
    void* commandList,
    uint64_t color,
    const char* formatString);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXEndEventOnCommandList(void* commandList);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXSetMarkerOnCommandList(
    void* commandList,
    uint64_t color,
    const char* formatString);

PIX_COMPAT_API void PIX_COMPAT_CALL PIXBeginEventOnCommandQueue(
    void* commandQueue,
    uint64_t color,
    const char* formatString);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXEndEventOnCommandQueue(void* commandQueue);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXSetMarkerOnCommandQueue(
    void* commandQueue,
    uint64_t color,
    const char* formatString);

PIX_COMPAT_API void PIX_COMPAT_CALL PIXNotifyWakeFromFenceSignal(void* eventHandle);
PIX_COMPAT_API void PIX_COMPAT_CALL PIXReportCounter(const wchar_t* name, float value);
PIX_COMPAT_API int32_t PIX_COMPAT_CALL PIXGpuCaptureNextFrames(
    const wchar_t* fileName,
    uint32_t numFrames);
PIX_COMPAT_API uint64_t PIX_COMPAT_CALL PIXGetEventCountForTest(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
inline void PIXBeginEvent(uint64_t color, const char* label) {
    PIXBeginEventOnCpu(color, label);
}
inline void PIXEndEvent() {
    PIXEndEventOnCpu();
}
inline void PIXSetMarker(uint64_t color, const char* label) {
    PIXSetMarkerOnCpu(color, label);
}

template <typename ContextT>
inline void PIXBeginEvent(ContextT* context, uint64_t color, const char* label) {
    PIXBeginEventOnCommandList(static_cast<void*>(context), color, label);
}
template <typename ContextT>
inline void PIXEndEvent(ContextT* context) {
    PIXEndEventOnCommandList(static_cast<void*>(context));
}
template <typename ContextT>
inline void PIXSetMarker(ContextT* context, uint64_t color, const char* label) {
    PIXSetMarkerOnCommandList(static_cast<void*>(context), color, label);
}

class PIXScopedEventObject {
public:
    PIXScopedEventObject(uint64_t color, const char* label) {
        PIXBeginEventOnCpu(color, label);
    }
    ~PIXScopedEventObject() {
        PIXEndEventOnCpu();
    }
    PIXScopedEventObject(const PIXScopedEventObject&) = delete;
    PIXScopedEventObject& operator=(const PIXScopedEventObject&) = delete;
};
#endif
