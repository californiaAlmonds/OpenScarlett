# OpenScarlett MIT Core GDK Runtime Shims
# Owned by sprint1-gdk worktree.

# 1. xgameruntime.dll
add_library(xgameruntime SHARED src/shims/xgameruntime_shim.cpp)
set_target_properties(xgameruntime PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xgameruntime"
)
target_compile_definitions(xgameruntime PRIVATE OPENSCARLETT_BUILDING_XGAMERUNTIME=1)
target_link_libraries(xgameruntime PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(xgameruntime PRIVATE /W4)
endif()

# 2. GameInput.dll
add_library(GameInput SHARED src/shims/gameinput_shim.cpp)
set_target_properties(GameInput PROPERTIES
    PREFIX ""
    OUTPUT_NAME "GameInput"
)
target_compile_definitions(GameInput PRIVATE OPENSCARLETT_BUILDING_GAMEINPUT=1)
target_link_libraries(GameInput PRIVATE openscarlett_core)
if(WIN32)
    target_link_libraries(GameInput PRIVATE user32)
endif()
if(MSVC)
    target_compile_options(GameInput PRIVATE /W4)
endif()

# 3. XAudio2_9.dll
add_library(XAudio2_9 SHARED src/shims/xaudio2_shim.cpp)
set_target_properties(XAudio2_9 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "XAudio2_9"
)
target_compile_definitions(XAudio2_9 PRIVATE OPENSCARLETT_BUILDING_XAUDIO2=1)
target_link_libraries(XAudio2_9 PRIVATE openscarlett_core)
if(WIN32)
    target_link_libraries(XAudio2_9 PRIVATE ole32 uuid)
endif()
if(MSVC)
    target_compile_options(XAudio2_9 PRIVATE /W4)
endif()

# 4. AcpHal.dll
add_library(AcpHal SHARED src/shims/acphal_shim.cpp)
set_target_properties(AcpHal PROPERTIES
    PREFIX ""
    OUTPUT_NAME "AcpHal"
)
target_compile_definitions(AcpHal PRIVATE OPENSCARLETT_BUILDING_ACPHAL=1)
target_link_libraries(AcpHal PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(AcpHal PRIVATE /W4)
endif()

# 5. dstorage_xs.dll
add_library(dstorage_xs SHARED src/shims/dstorage_shim.cpp)
set_target_properties(dstorage_xs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dstorage_xs"
)
target_compile_definitions(dstorage_xs PRIVATE OPENSCARLETT_BUILDING_DSTORAGE=1)
target_link_libraries(dstorage_xs PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(dstorage_xs PRIVATE /W4)
endif()

# 6. Microsoft.Xbox.Services.GDK.C.dll (XSAPI)
add_library(Microsoft.Xbox.Services.GDK.C SHARED src/shims/xsapi_shim.cpp)
set_target_properties(Microsoft.Xbox.Services.GDK.C PROPERTIES
    PREFIX ""
    OUTPUT_NAME "Microsoft.Xbox.Services.GDK.C"
)
target_compile_definitions(Microsoft.Xbox.Services.GDK.C PRIVATE OPENSCARLETT_BUILDING_XSAPI=1)
target_link_libraries(Microsoft.Xbox.Services.GDK.C PRIVATE openscarlett_core xgameruntime)
if(MSVC)
    target_compile_options(Microsoft.Xbox.Services.GDK.C PRIVATE /W4)
endif()

# End-to-End GDK Shims Test Suite
openscarlett_add_test(test_gdk_shims tests/test_gdk_shims.cpp)
target_link_libraries(test_gdk_shims PRIVATE
    xgameruntime
    GameInput
    XAudio2_9
    AcpHal
    dstorage_xs
    Microsoft.Xbox.Services.GDK.C
)
set_tests_properties(test_gdk_shims PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)

# Title-ABI test: raw Xbox dstorage_xs.h DSTORAGE_REQUEST (0x48 bytes) through IDStorageQueueX.
openscarlett_add_test(test_dstorage_xbox_request_layout tests/test_dstorage_xbox_request_layout.cpp)
target_link_libraries(test_dstorage_xbox_request_layout PRIVATE dstorage_xs)
