# Sprint 15 Worker 1: RDNA2 Bench Speedup & 100% Honest GPU Coverage + Privileged Trap/Halt CPU Harness

openscarlett_add_test(test_s15_rdna2_bench_trap_harness tests/test_s15_rdna2_bench_trap_harness.cpp)
target_include_directories(test_s15_rdna2_bench_trap_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s15_rdna2_bench_trap_harness)
endif()
