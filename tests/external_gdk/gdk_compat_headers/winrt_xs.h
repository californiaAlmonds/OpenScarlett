#pragma once

// MIT Xbox WinRT Activation & HSTRING (<winrt_xs.h>) public ABI header
// backed by winrt_xs.dll. Reuses Windows SDK <roapi.h> / <winstring.h> declarations
// so callers can also include <wrl/client.h> without extern "C" overload conflicts.

#include <roapi.h>
#include <winstring.h>
#include <cstdint>

constexpr GUID XS_IID_IAnalyticsInfoStatics = {
    0x1D5EE066u, 0x188Du, 0x5BA9u, {0x43, 0x87, 0xAC, 0xAE, 0xB0, 0xE7, 0xE3, 0x05}};

struct XS_IAnalyticsVersionInfo;
struct XS_IAnalyticsInfoStatics;
struct XS_IAnalyticsInfoStaticsVtbl {
    HRESULT(__stdcall* QueryInterface)(XS_IAnalyticsInfoStatics*, const GUID*, void**);
    ULONG(__stdcall* AddRef)(XS_IAnalyticsInfoStatics*);
    ULONG(__stdcall* Release)(XS_IAnalyticsInfoStatics*);
    HRESULT(__stdcall* GetIids)(XS_IAnalyticsInfoStatics*, ULONG*, GUID**);
    HRESULT(__stdcall* GetRuntimeClassName)(XS_IAnalyticsInfoStatics*, HSTRING*);
    HRESULT(__stdcall* GetTrustLevel)(XS_IAnalyticsInfoStatics*, int32_t*);
    HRESULT(__stdcall* get_VersionInfo)(XS_IAnalyticsInfoStatics*, XS_IAnalyticsVersionInfo**);
    HRESULT(__stdcall* get_DeviceForm)(XS_IAnalyticsInfoStatics*, HSTRING*);
};
struct XS_IAnalyticsInfoStatics {
    const XS_IAnalyticsInfoStaticsVtbl* lpVtbl;
};
