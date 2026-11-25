# OpenScarlett Loader Subsystem (VFS, Suspended-Process APC Injector, In-Process Hook DLL)

target_sources(openscarlett_core PRIVATE
    src/loader/game_config.cpp
    src/loader/vfs_mounts.cpp
    src/loader/apc_injector.cpp
)

add_library(openscarlett_hook SHARED
    src/loader/hook_dll.cpp
    src/loader/spatial_audio_bridge.cpp
)
set_target_properties(openscarlett_hook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "openscarlett_hook"
)
target_link_libraries(openscarlett_hook PRIVATE openscarlett_core)
if(WIN32)
    target_compile_definitions(openscarlett_hook PRIVATE
        PSAPI_VERSION=2
    )
    if(MSVC)
        target_compile_options(openscarlett_hook PRIVATE /W4)
    endif()
endif()

add_dependencies(openscarlett openscarlett_hook)
if(MSVC)
    target_compile_options(openscarlett PRIVATE /W4)
endif()

# Standalone Win32 x64 guest executable used to validate APC injection & VFS end-to-end.
# Deliberately does NOT link openscarlett_core or winrt_xs.
add_executable(test_guest_dummy tests/test_guest_dummy.cpp)
if(WIN32)
    target_compile_definitions(test_guest_dummy PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(test_guest_dummy PRIVATE /W4)
    endif()
endif()

openscarlett_add_test(test_apc_loader tests/test_apc_loader.cpp)
add_dependencies(test_apc_loader openscarlett openscarlett_hook winrt_xs test_guest_dummy)

openscarlett_add_test(test_loader_vfs_config tests/test_loader_vfs_config.cpp)
add_dependencies(test_loader_vfs_config openscarlett openscarlett_hook winrt_xs test_guest_dummy)
