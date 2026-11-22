# OpenScarlett Sprint 11 — Worker 4: 12 KiB XVD Header Inspector, Mixed-Case PE Forwarder
# Resolver, and In-Process Console LSA & Package-Family Policy State.

target_sources(openscarlett_core PRIVATE
    src/loader/xvd_inspector.cpp
    src/loader/pe_forwarder.cpp
    src/kernel/console_lsa_policy.cpp
)

openscarlett_add_test(test_xvd_loader_lsa tests/test_xvd_loader_lsa.cpp)
add_dependencies(test_xvd_loader_lsa openscarlett)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_xvd_loader_lsa)
endif()
