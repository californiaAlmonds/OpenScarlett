# Sprint 11 Worker 3: D3D12X Console Sampler Feedback Readback & Clear,
# Console Query Accumulator, Predicated Byte-Granular Copies, and GPU-QPC Clock Calibration.

target_sources(openscarlett_core PRIVATE
    src/gpu/d3d12_xs/sampler_feedback_x.cpp
    src/gpu/d3d12_xs/query_accumulator_x.cpp
)

openscarlett_add_test(test_d3d12_feedback_queries tests/test_d3d12_feedback_queries.cpp)
if(WIN32)
    target_link_libraries(test_d3d12_feedback_queries PRIVATE
        openscarlett_core
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
    )
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_d3d12_feedback_queries)
endif()
