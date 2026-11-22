# OpenScarlett Sprint 11 Targets
# RDNA2 / GFX10.3 Hardware Accuracy Rules & Expanded Real-GPU Opcode Verification Suite.

add_executable(rdna2_bench src/tools/rdna2_bench_main.cpp)
target_link_libraries(rdna2_bench PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(rdna2_bench PRIVATE /W4)
endif()

openscarlett_add_test(test_rdna2_gpu_accuracy tests/test_rdna2_gpu_accuracy.cpp)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(rdna2_bench)
    _openscarlett_apply_portability_to_target(test_rdna2_gpu_accuracy)
endif()
