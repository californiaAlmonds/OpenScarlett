# Sprint 12 Worker 2: Xbox dxc_xs DXBC/RDNA2 Shader Container, CFG Hardening, & Scalable Shader Cache

target_sources(openscarlett_core PRIVATE
    src/gpu/rdna2_xbox_container.cpp
)

openscarlett_add_test(test_s12_xbox_shader_container tests/test_s12_xbox_shader_container.cpp)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s12_xbox_shader_container)
endif()
