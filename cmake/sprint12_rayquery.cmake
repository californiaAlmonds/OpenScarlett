# OpenScarlett Sprint 12 (Worker 3):
# Live RDNA2 image_bvh_intersect_ray / image_bvh64_intersect_ray -> DXR 1.1 RayQuery /
# Vulkan SPV_KHR_ray_query / Live Compute BVH4 Fallback, Multi-Window 64-bit BVH Traversal,
# XboxBvhBuilder Conversion, and Live CreateStateObject / DispatchRays SBT Remapping.

target_sources(openscarlett_core PRIVATE
    src/gpu/rdna2_rayquery_bridge.cpp
)

openscarlett_add_test(test_s12_rayquery_live tests/test_s12_rayquery_live.cpp)
target_link_libraries(test_s12_rayquery_live PRIVATE XboxBvhBuilder)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s12_rayquery_live)
endif()
