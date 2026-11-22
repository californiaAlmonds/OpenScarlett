# OpenScarlett Compiler Portability Profile (MSVC / Clang-CL / MinGW-w64)
# Owned exclusively by wave1/mingw-ci-portability.
#
# Guarantees:
# - Pure MSVC (/W4, 0 warnings) receives zero flag perturbations.
# - Clang-CL (MSVC frontend on Windows SDK) keeps /W4 while suppressing
#   known Windows SDK / MSVC-extension false-positive noise.
# - MinGW-w64 (GCC/Clang targeting x86_64-w64-mingw32) receives portable
#   -Wall -Wextra flags, _WIN32_WINNT=0x0A00, explicit import libraries, and
#   .def export alias files matching MSVC #pragma comment(linker, "/export:...").

set(OPENSCARLETT_IS_CLANG_CL FALSE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(OPENSCARLETT_IS_CLANG_CL TRUE)
endif()

set(OPENSCARLETT_IS_MINGW FALSE)
if(MINGW)
    set(OPENSCARLETT_IS_MINGW TRUE)
endif()

if(OPENSCARLETT_IS_CLANG_CL)
    message(STATUS "OpenScarlett portability profile: Clang-CL (${CMAKE_CXX_COMPILER_VERSION}, MSVC frontend)")
    # Allow Clang-CL (e.g., LLVM 20 bundled with VS) to build against newer MSVC STL headers (e.g., VS 18 / 14.52+).
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-language-extension-token>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-microsoft-enum-value>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-gnu-anonymous-struct>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-nested-anon-types>
        $<$<COMPILE_LANGUAGE:C>:-Wno-unknown-pragmas>
    )
elseif(OPENSCARLETT_IS_MINGW)
    message(STATUS "OpenScarlett portability profile: MinGW-w64 (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
elseif(MSVC)
    message(STATUS "OpenScarlett portability profile: MSVC (${CMAKE_CXX_COMPILER_VERSION})")
else()
    message(STATUS "OpenScarlett portability profile: ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER_VERSION})")
endif()

function(_openscarlett_apply_portability_to_target tgt)
    if(NOT TARGET ${tgt})
        return()
    endif()

    if(OPENSCARLETT_IS_CLANG_CL)
        # Clang-CL already inherits /W4 from existing if(MSVC) rules.
        # Keep diagnostics strict while avoiding Windows SDK / MSVC-extension noise,
        # and allow Clang-CL (e.g. LLVM 20) against newer MSVC STL headers (e.g. 14.52+).
        target_compile_definitions(${tgt} PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
        target_compile_options(${tgt} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-language-extension-token>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-microsoft-enum-value>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-gnu-anonymous-struct>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-nested-anon-types>
            $<$<COMPILE_LANGUAGE:C>:-Wno-unknown-pragmas>
        )
    elseif(OPENSCARLETT_IS_MINGW)
        target_compile_definitions(${tgt} PRIVATE
            OPENSCARLETT_MINGW=1
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        target_compile_options(${tgt} PRIVATE
            -Wall
            -Wextra
            -Wno-unknown-pragmas
            -Wno-missing-field-initializers
            -Wno-unused-parameter
        )
    endif()
endfunction()

# Apply portability flags across all core, shim, tool, and test targets that exist
# in the current CMake configuration.
set(_openscarlett_known_targets
    openscarlett_core
    openscarlett
    openscarlett_hook
    winrt_xs
    d3d12_xs
    xg_xs
    kernelx
    xgameruntime
    GameInput
    XAudio2_9
    AcpHal
    dstorage_xs
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
    test_pe_inspect
    test_codecs
    test_vulkan_live
    test_rdna2_core
    test_winrt_xs
    test_d3d12_xs_live
    test_gdk_shims
    test_dstorage_xbox_request_layout
    test_kernelx
    test_guest_dummy
    test_apc_loader
    test_loader_vfs_config
    test_audio_opus_wasapi
    test_xg_dstorage
    test_dxr_live
    test_secondary_shims
    test_rdna2_lifter_live
    test_d3d12_xs_autolift
    test_presentx_windowing
    test_gdk_live
    test_gdk_simple_pbr
    test_gdk_meshlet_cull
    test_gdk_dxr_triangle
    test_external_gdk_bringup
    gdk_simple_triangle
    gdk_simple_mesh_shader
    gdk_simple_compute
    gdk_system_combo
    gdk_simple_pbr
    gdk_meshlet_cull
    gdk_dxr_triangle
    gdk_external_dstorage_async
    gdk_external_full_stack
    gdk_ms_directstorage_real
    test_vulkan_graphics_live
    test_hdr10_d3d12
    test_gdk_extended_loader
    test_dep_base
    test_dep_mid
    test_dep_top
    test_gdk_simple_lighting
    gdk_simple_lighting
    test_gdk_hdr_cubemap
    gdk_hdr_cubemap
    test_vulkan_mesh_showcase
    gdk_vulkan_mesh_showcase
    test_rdna2_corpus_parity
    test_gpu_d3d12x_vk_parity
    test_xgr_conformance_parity
    mfplat_xs
    advapi32legacy
    KernelTraceControl
    hix_xs
    xbtp
    test_dstorage_audio_diag_parity
    test_loader_child_hook_parity
    test_bcpack_bvh4
    test_real_gdk_thunks_live
    scarlett_platform_host
    ScarlettArena
    ScarlettDoom
    test_scarlett_game_live
)

foreach(_tgt IN LISTS _openscarlett_known_targets)
    _openscarlett_apply_portability_to_target(${_tgt})
endforeach()

# MinGW-w64 specific link libraries and .def export alias files.
# MSVC resolves these implicitly via #pragma comment(linker, ...) or default SDK libs.
if(OPENSCARLETT_IS_MINGW)
    if(TARGET openscarlett_core)
        target_link_libraries(openscarlett_core PUBLIC user32 gdi32)
    endif()
    if(TARGET openscarlett_hook)
        target_link_libraries(openscarlett_hook PRIVATE psapi)
    endif()
    if(TARGET d3d12_xs)
        target_link_libraries(d3d12_xs PRIVATE user32 gdi32)
    endif()
    if(TARGET winrt_xs)
        target_link_libraries(winrt_xs PRIVATE ole32 uuid)
    endif()
    if(TARGET xgameruntime)
        target_link_libraries(xgameruntime PRIVATE ole32 uuid user32)
    endif()
    if(TARGET kernelx AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shims/kernelx_mingw.def")
        target_sources(kernelx PRIVATE src/shims/kernelx_mingw.def)
    endif()
    if(TARGET xmem AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/shims/xmem_mingw.def")
        target_sources(xmem PRIVATE src/shims/xmem_mingw.def)
    endif()
endif()
