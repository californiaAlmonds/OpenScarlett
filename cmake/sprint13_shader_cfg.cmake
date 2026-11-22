# Sprint 13 Worker 3: RDNA2 CFG/MIMG/V#/LDS Safety & 700+ Real-GPU Opcode Verification

openscarlett_add_test(test_s13_shader_cfg_safety tests/test_s13_shader_cfg_safety.cpp)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s13_shader_cfg_safety)
endif()
