# OpenScarlett Worker 6 — BCPack BC1-BC7 Hardening & RDNA2 BVH4 Decoder

target_sources(openscarlett_core PRIVATE
    src/gpu/bvh4_decoder.cpp
)

openscarlett_add_test(test_bcpack_bvh4 tests/test_bcpack_bvh4.cpp)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_bcpack_bvh4)
endif()
