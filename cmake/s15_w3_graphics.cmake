# Sprint 15 Worker 3: D3D12X, Vulkan, XG 3D & BC6H/BCPack Parity Test Target

openscarlett_add_test(test_s15_graphics_xg_bc6h tests/test_s15_graphics_xg_bc6h.cpp)
target_link_libraries(test_s15_graphics_xg_bc6h PRIVATE openscarlett_core d3d12_xs xg_xs)
if(WIN32)
    target_link_libraries(test_s15_graphics_xg_bc6h PRIVATE d3d12 dxgi ole32 uuid)
endif()
target_include_directories(test_s15_graphics_xg_bc6h PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s15_graphics_xg_bc6h)
endif()
if(MSVC)
    target_compile_options(test_s15_graphics_xg_bc6h PRIVATE /W4)
endif()
