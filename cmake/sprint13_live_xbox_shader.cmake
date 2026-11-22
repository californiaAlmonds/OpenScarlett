# Sprint 13 Worker 1: Live Xbox DXBC/PM4/RDNA2 + RayQuery E2E in DeviceX & Unified Texture Sync
if(WIN32)
    openscarlett_add_test(test_s13_live_xbox_shader_e2e tests/test_s13_live_xbox_shader_e2e.cpp)
    target_link_libraries(test_s13_live_xbox_shader_e2e PRIVATE
        d3d12_xs
        openscarlett_core
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
        user32
    )
    add_dependencies(test_s13_live_xbox_shader_e2e d3d12_xs)
    if(COMMAND _openscarlett_apply_portability_to_target)
        _openscarlett_apply_portability_to_target(test_s13_live_xbox_shader_e2e)
    endif()
endif()
