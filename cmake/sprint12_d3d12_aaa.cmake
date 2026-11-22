# OpenScarlett Sprint 12 Targets
# D3D12X AAA Live GPU Island Wiring, GXDK Extensions & PresentX Pacing.

if(WIN32)
    openscarlett_add_test(test_s12_d3d12_aaa_wiring tests/test_s12_d3d12_aaa_wiring.cpp)
    target_link_libraries(test_s12_d3d12_aaa_wiring PRIVATE
        d3d12_xs
        openscarlett_core
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
        user32
    )
    add_dependencies(test_s12_d3d12_aaa_wiring d3d12_xs)
    if(COMMAND _openscarlett_apply_portability_to_target)
        _openscarlett_apply_portability_to_target(test_s12_d3d12_aaa_wiring)
    endif()
endif()
