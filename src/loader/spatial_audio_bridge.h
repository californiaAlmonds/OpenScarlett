#pragma once

#include <windows.h>

namespace openscarlett::loader {

// Wraps a host IMMDeviceEnumerator* (when rclsid == CLSID_MMDeviceEnumerator) so that
// IMMDevice::Activate(IID_ISpatialAudioClient) succeeds on any Windows 10/11 PC even when
// Windows Sonic / Spatial Sound is disabled on the default audio endpoint, downmixing
// 7.1.4 static bed + dynamic 3D spatial objects to shared-mode stereo WASAPI and
// OPENSCARLETT_RECORD_WAV.
void WrapMmDeviceEnumeratorIfNeeded(REFCLSID rclsid, REFIID riid, void** ppv) noexcept;

void FlushSpatialAudioWavRecorder() noexcept;

}  // namespace openscarlett::loader
