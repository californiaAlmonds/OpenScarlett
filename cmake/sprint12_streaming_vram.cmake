# Sprint 12 Worker 4: DirectStorage NVMe Streaming Hardening, BCPack BC1-BC7 Fuzz/Array Safety,
# HTILE/CMASK/DCC Fast-Clear Metadata Coherency, Physical Page Double-Mapping (64KB/2MB/4MB),
# and LRU VRAM Residency Manager.

target_sources(openscarlett_core PRIVATE
    src/kernel/vram_residency_manager.cpp
)

openscarlett_add_test(test_s12_streaming_vram tests/test_s12_streaming_vram.cpp)
target_link_libraries(test_s12_streaming_vram PRIVATE
    openscarlett_core
    dstorage_xs
    xg_xs
)
if(WIN32)
    target_link_libraries(test_s12_streaming_vram PRIVATE
        d3d12
        dxgi
        d3dcompiler
        ole32
        uuid
    )
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s12_streaming_vram)
endif()
