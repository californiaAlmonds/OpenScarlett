# Sprint 15 Worker 2: RDNA2 Shader & ISA Peer Parity Verification Suite

openscarlett_add_test(test_s15_shader_peer_parity tests/test_s15_shader_peer_parity.cpp)
target_include_directories(test_s15_shader_peer_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
if(WIN32)
    target_link_libraries(test_s15_shader_peer_parity PRIVATE d3d12 dxgi)
endif()
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s15_shader_peer_parity)
endif()
