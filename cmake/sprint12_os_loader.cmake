# OpenScarlett Sprint 12 — Worker 5: OS/Loader, TLS Slot 0, NT VFS,
# Live PeForwarder/XvdInspector/ConsoleLsaPolicy Wiring, GameInput,
# xgameruntime, and Lock-Free 128-Voice XAudio2 + Spatial Audio Stability.

openscarlett_add_test(test_s12_os_loader_gameinput tests/test_s12_os_loader_gameinput.cpp)
target_link_libraries(test_s12_os_loader_gameinput PRIVATE
    GameInput
    XAudio2_9
    xgameruntime
)
add_dependencies(test_s12_os_loader_gameinput
    openscarlett
    openscarlett_hook
    GameInput
    XAudio2_9
    xgameruntime
)
set_tests_properties(test_s12_os_loader_gameinput PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s12_os_loader_gameinput)
endif()
