#pragma once
#include <Windows.h>
// MIT XGameRuntimeInit.h importing from OpenScarlett xgameruntime.dll.
extern "C" {
__declspec(dllimport) HRESULT WINAPI XGameRuntimeInitialize();
__declspec(dllimport) void WINAPI XGameRuntimeUninitialize();
}
