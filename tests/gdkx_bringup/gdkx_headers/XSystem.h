#pragma once
#include <cstdint>
#include <Windows.h>
// MIT XSystem.h importing from OpenScarlett xgameruntime.dll.
enum class XSystemDeviceType : uint32_t {
    Unknown = 0,
    Pc = 1,
    XboxOne = 2,
    XboxOneS = 3,
    XboxOneX = 4,
    XboxOneXDevkit = 5,
    XboxScarlettLockhart = 6,
    XboxScarlettAnaconda = 7,
    XboxScarlettDevkit = 8,
};
extern "C" __declspec(dllimport) XSystemDeviceType WINAPI XSystemGetDeviceType();
