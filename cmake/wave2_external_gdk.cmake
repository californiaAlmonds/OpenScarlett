# OpenScarlett Wave 2 Agent I — External GDK-Header Binaries & 19-DLL Live Telemetry Bring-Up
# Owned exclusively by wave2/external-gdk-bringup worktree.

set(_os_gdk_compat_inc "${CMAKE_CURRENT_SOURCE_DIR}/tests/external_gdk/gdk_compat_headers")

function(openscarlett_add_external_gdk_exe name src)
    add_executable(${name} ${src})
    # Pure standalone external Xbox-ABI Win32 x64 guest executable:
    # Deliberately does NOT link openscarlett_core AND does NOT include ${CMAKE_CURRENT_SOURCE_DIR}/include!
    # Compiles strictly against MIT Microsoft-GDK-compatible headers!
    target_include_directories(${name} PRIVATE
        ${_os_gdk_compat_inc}
    )
    if(WIN32)
        target_compile_definitions(${name} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        target_link_libraries(${name} PRIVATE
            kernel32
            user32
            ole32
            uuid
            dxgi
        )
        if(MSVC)
            target_compile_options(${name} PRIVATE /W4)
        endif()
    endif()
endfunction()

# 1. External GDK-Header Binary #1: DirectStorage + AsynchronousProgramming + GameSaveCombo + GameInput + PIX3
openscarlett_add_external_gdk_exe(
    gdk_external_dstorage_async
    tests/external_gdk/gdk_external_dstorage_async.cpp
)
target_link_libraries(gdk_external_dstorage_async PRIVATE
    xgameruntime
    dstorage_xs
    GameInput
    pixevt
)

# 2. External GDK-Header Binary #2: Full 19-DLL Live Stack + XS2\0 Auto-Lift + CoW VFS Protection
# Statically imports 17 Xbox/GDK DLLs via PE IAT and dynamically loads kernelx.dll + xmem.dll via LoadLibraryA.
openscarlett_add_external_gdk_exe(
    gdk_external_full_stack
    tests/external_gdk/gdk_external_full_stack.cpp
)
target_link_libraries(gdk_external_full_stack PRIVATE
    d3d12_xs
    xg_xs
    xgameruntime
    GameInput
    XAudio2_9
    AcpHal
    dstorage_xs
    winrt_xs
    Microsoft.Xbox.Services.GDK.C
    pixevt
    xcrdapi
    libHttpClient.GDK
    XCurl
    Bink2Scarlett
    amd_fidelityfx_upscaler_xbox
    amd_fidelityfx_loader_xbox
    XboxBvhBuilder
    d3d12
)

# 3. Optional Real Untouched MIT Microsoft Xbox-GDK-Samples SimpleDirectStorageCombo Runner
# Discovered dynamically via OPENSCARLETT_GDK_SAMPLES_DIR or relative packages/Xbox-GDK-Samples
# (supports main checkout and git worktrees); skipped cleanly on CI runners without it.
set(_os_ms_dstorage_root "")
set(_os_ms_sample_candidates "")
if(DEFINED ENV{OPENSCARLETT_GDK_SAMPLES_DIR} AND NOT "$ENV{OPENSCARLETT_GDK_SAMPLES_DIR}" STREQUAL "")
    list(APPEND _os_ms_sample_candidates "$ENV{OPENSCARLETT_GDK_SAMPLES_DIR}")
endif()
list(APPEND _os_ms_sample_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/packages/Xbox-GDK-Samples"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/Xbox-GDK-Samples"
)
foreach(_cand IN LISTS _os_ms_sample_candidates)
    if(EXISTS "${_cand}/Samples/System/SimpleDirectStorageCombo/SampleImplementations/SimpleLoad.cpp")
        set(_os_ms_dstorage_root "${_cand}/Samples/System/SimpleDirectStorageCombo")
        break()
    endif()
endforeach()
if(WIN32 AND NOT "${_os_ms_dstorage_root}" STREQUAL "")
    set(_os_ms_dstorage_sources
        "${_os_ms_dstorage_root}/SampleImplementations/ImplementationBase.cpp"
        "${_os_ms_dstorage_root}/SampleImplementations/SimpleLoad.cpp"
        "${_os_ms_dstorage_root}/SampleImplementations/StatusBatch.cpp"
        "${_os_ms_dstorage_root}/SampleImplementations/CompletionEvent.cpp"
        "${_os_ms_dstorage_root}/SampleImplementations/MultipleQueues.cpp"
    )
    add_executable(gdk_ms_directstorage_real
        tests/external_gdk/ms_dstorage_runner.cpp
        ${_os_ms_dstorage_sources}
    )
    target_include_directories(gdk_ms_directstorage_real PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/external_gdk/ms_dstorage_shim_inc"
        ${_os_gdk_compat_inc}
        "${_os_ms_dstorage_root}"
    )
    # Build the samples' Xbox Series path: DirectStorageCrossPlatform.h then includes <dstorage_xs.h>
    # (the compat header) and fills the Xbox DSTORAGE_REQUEST fields (File/FileOffset/SourceSize/...),
    # which is what dstorage_xs.dll receives from a title. _GAMING_DESKTOP would select PC dstorage.h.
    target_compile_definitions(gdk_ms_directstorage_real PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _GAMING_XBOX=1
        _GAMING_XBOX_SCARLETT=1
        _GRDK_VER=0x585D073C
    )
    target_link_libraries(gdk_ms_directstorage_real PRIVATE
        dstorage_xs
        kernel32
        ole32
        uuid
    )
    if(MSVC)
        target_compile_options(gdk_ms_directstorage_real PRIVATE /W4 /Zc:char8_t-)
        # Keep third-party untouched Microsoft sample sources warning-free at /W4
        set_source_files_properties(${_os_ms_dstorage_sources} PROPERTIES
            COMPILE_OPTIONS "/wd4100;/wd4189;/wd4244;/wd4267;/wd4389;/wd4458;/wd4505"
        )
    endif()
    set(_os_has_ms_dstorage_real TRUE)
else()
    set(_os_has_ms_dstorage_real FALSE)
endif()

# 4. Portable End-to-End Test Harness (runs all external GDK binaries via `openscarlett.exe run <package_dir>`)
openscarlett_add_test(test_external_gdk_bringup tests/test_external_gdk_bringup.cpp)
add_dependencies(test_external_gdk_bringup
    openscarlett
    openscarlett_hook
    d3d12_xs
    xg_xs
    kernelx
    xgameruntime
    GameInput
    XAudio2_9
    AcpHal
    dstorage_xs
    winrt_xs
    Microsoft.Xbox.Services.GDK.C
    pixevt
    xmem
    xcrdapi
    libHttpClient.GDK
    XCurl
    Bink2Scarlett
    amd_fidelityfx_upscaler_xbox
    amd_fidelityfx_loader_xbox
    XboxBvhBuilder
    gdk_external_dstorage_async
    gdk_external_full_stack
)
if(_os_has_ms_dstorage_real)
    add_dependencies(test_external_gdk_bringup gdk_ms_directstorage_real)
endif()
set_tests_properties(test_external_gdk_bringup PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
