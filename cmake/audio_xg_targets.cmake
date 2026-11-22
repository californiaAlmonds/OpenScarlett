# OpenScarlett MIT Wave 1 Audio (Opus/IMA-ADPCM/WASAPI),
# Full GFX10 Texture Tiling (xg_xs), and DirectStorage Hardening Targets.
# Owned by wave1/audio-textures-dstorage worktree.

target_sources(openscarlett_core PRIVATE
    src/audio/opus_adpcm_decoder.cpp
)

# 1. Opus MFT + IMA-ADPCM + Deterministic & Live Host WASAPI Test
# Note: Intentionally does NOT force OPENSCARLETT_AUDIO_NULL=1 so the live
# WASAPI sub-test exercises host WASAPI (while still handling headless CI safely).
openscarlett_add_test(test_audio_opus_wasapi tests/test_audio_opus_wasapi.cpp)
target_link_libraries(test_audio_opus_wasapi PRIVATE XAudio2_9)

# 2. Full GFX10 2D/3D/MipTail/Formats (xg_xs) + DirectStorage Hardening Test
openscarlett_add_test(test_xg_dstorage tests/test_xg_dstorage.cpp)
target_link_libraries(test_xg_dstorage PRIVATE xg_xs dstorage_xs)
