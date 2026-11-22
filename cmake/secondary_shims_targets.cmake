# OpenScarlett MIT Secondary Console / GDK Shim DLLs
# Owned by wave1/secondary-shims-queryapi worktree.

# 1. pixevt.dll
add_library(pixevt SHARED src/shims/pixevt_shim.cpp)
set_target_properties(pixevt PROPERTIES PREFIX "" OUTPUT_NAME "pixevt")
target_compile_definitions(pixevt PRIVATE OPENSCARLETT_BUILDING_PIXEVT=1)
target_link_libraries(pixevt PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(pixevt PRIVATE /W4)
endif()

# 2. xmem.dll (forwards Xbox memory APIs to kernelx.dll)
add_library(xmem SHARED src/shims/xmem_shim.cpp)
set_target_properties(xmem PROPERTIES PREFIX "" OUTPUT_NAME "xmem")
target_link_libraries(xmem PRIVATE openscarlett_core kernelx)
if(MSVC)
    target_compile_options(xmem PRIVATE /W4)
endif()

# 3. xcrdapi.dll
add_library(xcrdapi SHARED src/shims/xcrdapi_shim.cpp)
set_target_properties(xcrdapi PROPERTIES PREFIX "" OUTPUT_NAME "xcrdapi")
target_compile_definitions(xcrdapi PRIVATE OPENSCARLETT_BUILDING_XCRDAPI=1)
target_link_libraries(xcrdapi PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(xcrdapi PRIVATE /W4)
endif()

# 4. libHttpClient.GDK.dll
add_library(libHttpClient.GDK SHARED src/shims/libhttpclient_shim.cpp)
set_target_properties(libHttpClient.GDK PROPERTIES PREFIX "" OUTPUT_NAME "libHttpClient.GDK")
target_compile_definitions(libHttpClient.GDK PRIVATE OPENSCARLETT_BUILDING_LIBHTTPCLIENT=1)
target_link_libraries(libHttpClient.GDK PRIVATE openscarlett_core xgameruntime)
if(MSVC)
    target_compile_options(libHttpClient.GDK PRIVATE /W4)
endif()

# 5. XCurl.dll
add_library(XCurl SHARED src/shims/xcurl_shim.cpp)
set_target_properties(XCurl PROPERTIES PREFIX "" OUTPUT_NAME "XCurl")
target_compile_definitions(XCurl PRIVATE OPENSCARLETT_BUILDING_XCURL=1)
target_link_libraries(XCurl PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(XCurl PRIVATE /W4)
endif()

# 6. Bink2Scarlett.dll
add_library(Bink2Scarlett SHARED src/shims/bink2_scarlett_shim.cpp)
set_target_properties(Bink2Scarlett PROPERTIES PREFIX "" OUTPUT_NAME "Bink2Scarlett")
target_compile_definitions(Bink2Scarlett PRIVATE OPENSCARLETT_BUILDING_BINK2=1)
target_link_libraries(Bink2Scarlett PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(Bink2Scarlett PRIVATE /W4)
endif()

# 7. amd_fidelityfx_upscaler_xbox.dll
add_library(amd_fidelityfx_upscaler_xbox SHARED src/shims/ffx_upscaler_shim.cpp)
set_target_properties(amd_fidelityfx_upscaler_xbox PROPERTIES
    PREFIX ""
    OUTPUT_NAME "amd_fidelityfx_upscaler_xbox"
)
target_compile_definitions(amd_fidelityfx_upscaler_xbox PRIVATE OPENSCARLETT_BUILDING_FFX_UPSCALER=1)
target_link_libraries(amd_fidelityfx_upscaler_xbox PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(amd_fidelityfx_upscaler_xbox PRIVATE /W4)
endif()

# 8. amd_fidelityfx_loader_xbox.dll
add_library(amd_fidelityfx_loader_xbox SHARED src/shims/ffx_loader_shim.cpp)
set_target_properties(amd_fidelityfx_loader_xbox PROPERTIES
    PREFIX ""
    OUTPUT_NAME "amd_fidelityfx_loader_xbox"
)
target_compile_definitions(amd_fidelityfx_loader_xbox PRIVATE OPENSCARLETT_BUILDING_FFX_LOADER=1)
target_link_libraries(amd_fidelityfx_loader_xbox PRIVATE openscarlett_core amd_fidelityfx_upscaler_xbox)
if(MSVC)
    target_compile_options(amd_fidelityfx_loader_xbox PRIVATE /W4)
endif()

# 9. XboxBvhBuilder.dll
add_library(XboxBvhBuilder SHARED src/shims/xbox_bvh_builder_shim.cpp)
set_target_properties(XboxBvhBuilder PROPERTIES PREFIX "" OUTPUT_NAME "XboxBvhBuilder")
target_compile_definitions(XboxBvhBuilder PRIVATE OPENSCARLETT_BUILDING_XBOX_BVH=1)
target_link_libraries(XboxBvhBuilder PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(XboxBvhBuilder PRIVATE /W4)
endif()

# Comprehensive Unit + Live GPU Test Suite for QueryApiImpl & All 9 Secondary Shims
openscarlett_add_test(test_secondary_shims tests/test_secondary_shims.cpp)
target_link_libraries(test_secondary_shims PRIVATE
    xgameruntime
    pixevt
    xcrdapi
    libHttpClient.GDK
    XCurl
    Bink2Scarlett
    amd_fidelityfx_upscaler_xbox
    amd_fidelityfx_loader_xbox
    XboxBvhBuilder
)
add_dependencies(test_secondary_shims xmem kernelx)
