# OpenScarlett MIT xgameruntime 100% Thunks Export Parity,
# 3-arg QueryApiImpl COM ABI, Headless XStore/PLS, and XTaskQueue/XAsync Conformance Suite.
# Owned by wt-xgr-100pct-queryapi worktree.

openscarlett_add_test(test_xgr_conformance_parity tests/test_xgr_conformance_parity.cpp)
target_link_libraries(test_xgr_conformance_parity PRIVATE xgameruntime)
set_tests_properties(test_xgr_conformance_parity PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
