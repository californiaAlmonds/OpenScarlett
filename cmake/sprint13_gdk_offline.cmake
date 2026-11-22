# OpenScarlett Sprint 13 — Worker 4:
#   - Offline-first XSTS token policy (default E_GAMEUSER_NO_TOKEN_REQUIRED, opt-in OPENSCARLETT_MOCK_XSTS=1)
#   - XTaskQueue waiter re-arming & terminated-queue dispatch draining
#   - XGameUiShowMessageDialogAsync strict button bounds & XErrorOptions bitmask validation
#   - Clean-room UMS cooperative helpers (kernelx.dll + ntdll_ums_xs.dll) & WLDP policy rows (wldp_xs.dll)

# 1. wldp_xs.dll (Windows Lockdown Policy watermark & production policy query rows)
add_library(wldp_xs SHARED src/shims/console_diag_shims.cpp)
set_target_properties(wldp_xs PROPERTIES PREFIX "" OUTPUT_NAME "wldp_xs")
target_compile_definitions(wldp_xs PRIVATE OPENSCARLETT_DIAG_TARGET_WLDP_XS=1)
target_link_libraries(wldp_xs PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(wldp_xs PRIVATE /W4)
endif()

# 2. ntdll_ums_xs.dll (ntdll Rtl*Ums* + Win32 *Ums* cooperative fiber-backed shims)
add_library(ntdll_ums_xs SHARED src/shims/console_diag_shims.cpp)
set_target_properties(ntdll_ums_xs PROPERTIES PREFIX "" OUTPUT_NAME "ntdll_ums_xs")
target_compile_definitions(ntdll_ums_xs PRIVATE OPENSCARLETT_DIAG_TARGET_NTDLL_UMS_XS=1)
target_link_libraries(ntdll_ums_xs PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(ntdll_ums_xs PRIVATE /W4)
endif()

# 3. Sprint 13 GDK Offline-First, XTaskQueue/XGameUI/XError, UMS & WLDP Test Suite
openscarlett_add_test(test_s13_gdk_offline_ntdll tests/test_s13_gdk_offline_ntdll.cpp)
target_link_libraries(test_s13_gdk_offline_ntdll PRIVATE
    xgameruntime
    Microsoft.Xbox.Services.GDK.C
    kernelx
)
add_dependencies(test_s13_gdk_offline_ntdll
    xgameruntime
    Microsoft.Xbox.Services.GDK.C
    kernelx
    wldp_xs
    ntdll_ums_xs
)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(wldp_xs)
    _openscarlett_apply_portability_to_target(ntdll_ums_xs)
    _openscarlett_apply_portability_to_target(test_s13_gdk_offline_ntdll)
endif()
