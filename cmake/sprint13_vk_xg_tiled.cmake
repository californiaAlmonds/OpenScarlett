# OpenScarlett Sprint 13 Worker 2 Targets
# Vulkan D3D12 OPTIONS2/7 & Custom Border Color, Tiled Placed Textures, and XG Stencil/HTILE Parity.

if(WIN32)
    openscarlett_add_test(test_s13_vk_xg_tiled tests/test_s13_vk_xg_tiled.cpp)
    target_include_directories(test_s13_vk_xg_tiled PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/d3d12_xs
    )
    target_link_libraries(test_s13_vk_xg_tiled PRIVATE
        d3d12_xs
        xg_xs
        openscarlett_core
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
        user32
    )
    add_dependencies(test_s13_vk_xg_tiled d3d12_xs xg_xs)
    if(COMMAND _openscarlett_apply_portability_to_target)
        _openscarlett_apply_portability_to_target(test_s13_vk_xg_tiled)
    endif()
endif()
