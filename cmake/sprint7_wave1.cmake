# OpenScarlett Sprint 7 Wave 1 Targets
# Integrates Worker 1 (d3d12_xs->Vulkan routing), Worker 2 (RDNA2 lockstep LDS & diff ISA),
# Worker 3 (PE/loader hardening, XMemVirtualQuery, GDK parity & 6 stub DLLs),
# and Worker 4 (3D shadowed/skinned/BC-textured GDK scene & dual-API isolated test).

if(WIN32)
    function(_openscarlett_configure_s7_target tgt)
        if(MSVC)
            target_compile_options(${tgt} PRIVATE /W4)
        elseif(MINGW)
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
        else()
            target_compile_options(${tgt} PRIVATE -Wall -Wextra)
        endif()
    endfunction()

    # -------------------------------------------------------------------------
    # Worker 1: Direct d3d12_xs -> Vulkan 1.4 Routing & Cross-Vendor Edge Cases
    # -------------------------------------------------------------------------
    openscarlett_add_test(test_d3d12_vk_routing tests/test_d3d12_vk_routing.cpp)
    target_link_libraries(test_d3d12_vk_routing PRIVATE
        d3d12_xs
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
        user32
    )
    _openscarlett_configure_s7_target(test_d3d12_vk_routing)

    # -------------------------------------------------------------------------
    # Worker 2: RDNA2 Single-Wave Lockstep LDS Barriers, No-Ops & Diff ISA
    # -------------------------------------------------------------------------
    openscarlett_add_test(test_rdna2_lockstep_isa tests/test_rdna2_lockstep_isa.cpp)
    target_link_libraries(test_rdna2_lockstep_isa PRIVATE
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
    )
    _openscarlett_configure_s7_target(test_rdna2_lockstep_isa)

    # -------------------------------------------------------------------------
    # Worker 3: 6 New Fixed-Return Stub DLLs + PE/GDK Runtime Parity
    # -------------------------------------------------------------------------
    foreach(_s7_dll_spec
        "mflnmhelp|OPENSCARLETT_S7_TARGET_MFLNMHELP|"
        "mfhevcdec|OPENSCARLETT_S7_TARGET_MFHEVCDEC|"
        "gamecoreclient|OPENSCARLETT_S7_TARGET_GAMECORECLIENT|"
        "minuserx|OPENSCARLETT_S7_TARGET_MINUSERX|user32"
        "xtfpix|OPENSCARLETT_S7_TARGET_XTFPIX|"
        "xtfdebugmonitor|OPENSCARLETT_S7_TARGET_XTFDEBUGMONITOR|"
    )
        string(REPLACE "|" ";" _s7_parts "${_s7_dll_spec}")
        list(GET _s7_parts 0 _s7_name)
        list(GET _s7_parts 1 _s7_macro)
        list(GET _s7_parts 2 _s7_extra_lib)

        add_library(${_s7_name} SHARED src/shims/sprint7_diag_shims.cpp)
        set_target_properties(${_s7_name} PROPERTIES PREFIX "" OUTPUT_NAME "${_s7_name}")
        target_compile_definitions(${_s7_name} PRIVATE ${_s7_macro}=1)
        target_link_libraries(${_s7_name} PRIVATE openscarlett_core)
        if(NOT "${_s7_extra_lib}" STREQUAL "")
            target_link_libraries(${_s7_name} PRIVATE ${_s7_extra_lib})
        endif()
        _openscarlett_configure_s7_target(${_s7_name})
    endforeach()

    openscarlett_add_test(test_s7_pe_gdk_runtime tests/test_s7_pe_gdk_runtime.cpp)
    target_link_libraries(test_s7_pe_gdk_runtime PRIVATE
        kernelx
        xgameruntime
        GameInput
        xg_xs
        d3d12
        dxgi
        d3dcompiler
        ole32
        user32
        uuid
        advapi32
    )
    add_dependencies(test_s7_pe_gdk_runtime
        openscarlett
        openscarlett_hook
        xmem
        mflnmhelp
        mfhevcdec
        gamecoreclient
        minuserx
        xtfpix
        xtfdebugmonitor
    )
    _openscarlett_configure_s7_target(test_s7_pe_gdk_runtime)

    # -------------------------------------------------------------------------
    # Worker 4: 3D Shadowed/Skinned/BC1+BC3 GDK Scene + Dual-API Test
    # -------------------------------------------------------------------------
    add_executable(gdk_3d_scene tests/gdk_3d_scene/gdk_3d_scene.cpp)
    target_include_directories(gdk_3d_scene PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/gdk_3d_scene
    )
    target_compile_definitions(gdk_3d_scene PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(gdk_3d_scene PRIVATE
        d3d12_xs
        xgameruntime
        GameInput
        XAudio2_9
        kernelx
        d3d12
        dxgi
        d3dcompiler
        user32
        kernel32
        ole32
        uuid
    )
    _openscarlett_configure_s7_target(gdk_3d_scene)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/gdk_3d_scene/MicrosoftGame.Config
        ${CMAKE_BINARY_DIR}/gdk_3d_scene_MicrosoftGame.Config
        COPYONLY
    )

    openscarlett_add_test(test_gdk_3d_scene_dual_api tests/test_gdk_3d_scene_dual_api.cpp)
    add_dependencies(test_gdk_3d_scene_dual_api
        openscarlett
        openscarlett_hook
        d3d12_xs
        xgameruntime
        GameInput
        XAudio2_9
        kernelx
        gdk_3d_scene
    )
    set_tests_properties(test_gdk_3d_scene_dual_api PROPERTIES
        ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
    )
    _openscarlett_configure_s7_target(test_gdk_3d_scene_dual_api)
endif()
