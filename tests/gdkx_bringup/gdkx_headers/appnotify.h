#pragma once
#include <Windows.h>

// MIT appnotify.h importing real PLM exports from OpenScarlett's kernelx.dll / xgameruntime.dll.

struct _APPSTATE_REGISTRATION;
struct _APPCONSTRAIN_REGISTRATION;
typedef struct _APPSTATE_REGISTRATION* PAPPSTATE_REGISTRATION;
typedef struct _APPCONSTRAIN_REGISTRATION* PAPPCONSTRAIN_REGISTRATION;

typedef VOID(CALLBACK* PAPPSTATE_CHANGE_ROUTINE)(BOOLEAN Quiesced, PVOID Context);
typedef VOID(CALLBACK* PAPPCONSTRAIN_CHANGE_ROUTINE)(BOOLEAN Constrained, PVOID Context);

extern "C" {
__declspec(dllimport) ULONG WINAPI RegisterAppStateChangeNotification(
    PAPPSTATE_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPSTATE_REGISTRATION* Registration);

__declspec(dllimport) VOID WINAPI UnregisterAppStateChangeNotification(
    PAPPSTATE_REGISTRATION Registration);

__declspec(dllimport) ULONG WINAPI RegisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPCONSTRAIN_REGISTRATION* Registration);

__declspec(dllimport) VOID WINAPI UnregisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_REGISTRATION Registration);
}
