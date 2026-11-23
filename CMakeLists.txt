cmake_minimum_required(VERSION 3.20)

# Enable embedded debug info (/Z7) on MSVC when supported so sccache/ccache can
# cache Debug and RelWithDebInfo objects without mspdbsrv.exe PDB contention.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

# Automatic compiler cache (sccache / ccache) for fast cross-worktree builds.
option(OPENSCARLETT_USE_COMPILER_CACHE
    "Use sccache or ccache automatically when available"
    ON)
if(OPENSCARLETT_USE_COMPILER_CACHE
   AND NOT DEFINED ENV{OPENSCARLETT_NO_CACHE}
   AND NOT CMAKE_C_COMPILER_LAUNCHER
   AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(_os_compiler_cache
        NAMES sccache ccache
        HINTS "E:/ScarlettX-dev/tools/sccache"
    )
    if(_os_compiler_cache)
        set(CMAKE_C_COMPILER_LAUNCHER "${_os_compiler_cache}" CACHE STRING "C compiler cache launcher")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_os_compiler_cache}" CACHE STRING "CXX compiler cache launcher")
        message(STATUS "OpenScarlett compiler cache enabled: ${_os_compiler_cache}")
    endif()
endif()

project(OpenScarlett VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(MSVC)
    # Restore runtime std::mutex initialization so binaries built with VS 2022 17.10+
    # remain ABI-compatible with all msvcp140.dll versions (< 14.40) and never crash
    # with 0xC0000005 inside _Mtx_lock when an older VC++ runtime is loaded.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

add_library(openscarlett_core STATIC
    src/core/status.cpp
    src/core/telemetry.cpp
    src/loader/pe_inspector.cpp
    src/io/gdeflate.cpp
    src/io/bcpack.cpp
    src/audio/xma2_decoder.cpp
    src/middleware/bink2_decoder.cpp
    src/gpu/vulkan_backend.cpp
    src/gpu/rdna2_disasm.cpp
    src/gpu/rdna2_ir.cpp
)

target_include_directories(openscarlett_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(openscarlett_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        OPENSCARLETT_PLATFORM_WINDOWS=1
    )
    target_link_libraries(openscarlett_core PUBLIC d3d12 dxgi d3dcompiler ole32 uuid)
    if(MSVC)
        target_compile_options(openscarlett_core PRIVATE /W4)
    endif()
endif()

# Shims
add_library(winrt_xs SHARED src/shims/winrt_shim.cpp)
set_target_properties(winrt_xs PROPERTIES PREFIX "" OUTPUT_NAME "winrt_xs")
target_link_libraries(winrt_xs PRIVATE openscarlett_core)
target_compile_definitions(winrt_xs PRIVATE OPENSCARLETT_BUILDING_WINRT=1)

# CLI runner & hardware/binary inspector
add_executable(openscarlett src/main.cpp)
target_link_libraries(openscarlett PRIVATE openscarlett_core)
add_dependencies(openscarlett winrt_xs)

# Unit & Live-GPU Test Suite
function(openscarlett_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE openscarlett_core)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    endif()
    target_compile_definitions(${name} PRIVATE
        OPENSCARLETT_BINARY_DIR="${CMAKE_BINARY_DIR}"
        OPENSCARLETT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

openscarlett_add_test(test_pe_inspect tests/test_pe_inspect.cpp)
add_dependencies(test_pe_inspect openscarlett)

openscarlett_add_test(test_codecs tests/test_codecs.cpp)
openscarlett_add_test(test_vulkan_live tests/test_vulkan_live.cpp)
openscarlett_add_test(test_rdna2_core tests/test_rdna2_core.cpp)

openscarlett_add_test(test_winrt_xs tests/test_winrt_xs.cpp)
target_link_libraries(test_winrt_xs PRIVATE winrt_xs)

add_test(NAME test_cli_probe_gpu COMMAND $<TARGET_FILE:openscarlett> probe-gpu)

# Modular subsystem build files (keeps parallel worktrees conflict-free)
foreach(_os_module_cmake
    cmake/loader_targets.cmake
    cmake/kernel_targets.cmake
    cmake/graphics_targets.cmake
    cmake/gdk_targets.cmake
    cmake/audio_xg_targets.cmake
    cmake/shader_targets.cmake
    cmake/dxr_targets.cmake
    cmake/secondary_shims_targets.cmake
    cmake/presentx_targets.cmake
    cmake/gdk_live_targets.cmake
    cmake/wave2_gdk_targets.cmake
    cmake/wave3_vulkan_graphics.cmake
    cmake/wave3_hdr10_d3d12.cmake
    cmake/wave3_gdk_extended_loader.cmake
    cmake/wave4_simple_lighting.cmake
    cmake/wave4_hdr_cubemap.cmake
    cmake/wave4_vulkan_mesh_showcase.cmake
    cmake/worker_rdna2_corpus.cmake
    cmake/worker_gpu_d3d12x_vk.cmake
    cmake/worker_xgr_conformance.cmake
    cmake/worker_dstorage_audio_diag.cmake
    cmake/worker_loader_child_hook.cmake
    cmake/worker_bcpack_bvh4.cmake
    cmake/worker_playable_game.cmake
    cmake/wave3_portability.cmake
    cmake/sprint7_wave1.cmake
    cmake/sprint7_launcher.cmake
    cmake/sprint8_rdna2.cmake
    cmake/sprint9_10_compat.cmake
    cmake/gdkx_bringup_targets.cmake
    cmake/sprint11_as_spirv.cmake
    cmake/sprint11_d3d12_feedback_queries.cmake
    cmake/sprint11_xvd_loader_lsa.cmake
    cmake/sprint11_rdna2_gpu_verify.cmake
    cmake/sprint12_d3d12_aaa.cmake
    cmake/sprint12_xbox_shader.cmake
    cmake/sprint12_rayquery.cmake
    cmake/sprint12_streaming_vram.cmake
    cmake/sprint12_os_loader.cmake
    cmake/sprint13_live_xbox_shader.cmake
    cmake/sprint13_vk_xg_tiled.cmake
    cmake/sprint13_shader_cfg.cmake
    cmake/sprint13_gdk_offline.cmake
    cmake/sprint14_p0_shims.cmake
    cmake/sprint14_d3d12_residency.cmake
    cmake/sprint14_bvh_builder.cmake
    cmake/sprint14_dstorage_x2.cmake
    cmake/sprint14_shader_sm66.cmake
    cmake/mine_xapu_acphal.cmake
    cmake/mine_d3d12_xmem.cmake
    cmake/mine_routing_apisets.cmake
    cmake/s15_w1_rdna2_bench.cmake
    cmake/s15_w2_shaders.cmake
    cmake/s15_w3_graphics.cmake
    cmake/s15_w4_shims.cmake
    cmake/ue5_dxc_xcrdapi_xg.cmake
    cmake/ue5_d3d12_exports.cmake
    cmake/ue5_acphal_xmem_mfplat.cmake
    cmake/ue5_loader_probe.cmake
    cmake/ue5_matrix_integration.cmake
    cmake/build_speedups.cmake
)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_os_module_cmake}")
        include("${CMAKE_CURRENT_SOURCE_DIR}/${_os_module_cmake}")
    endif()
endforeach()
