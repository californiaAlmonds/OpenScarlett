# OpenScarlett Sprint 11 (Worker 2):
# Amplification/Task (AS) Stage SPIR-V 1.6 (SPV_EXT_mesh_shader / TaskEXT),
# Wave64-on-Wave32 LDS Emulation Helpers, and Paired Barycentric + CullDistance Stage Lowering.

openscarlett_add_test(test_rdna2_as_task_spirv tests/test_rdna2_as_task_spirv.cpp)
if(WIN32)
    target_link_libraries(test_rdna2_as_task_spirv PRIVATE openscarlett_core d3d12 dxgi d3dcompiler ole32 uuid)
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_rdna2_as_task_spirv)
endif()
