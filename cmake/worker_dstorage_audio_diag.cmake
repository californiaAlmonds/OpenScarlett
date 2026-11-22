# OpenScarlett MIT DirectStorage, Media Foundation, Console Diagnostic
# Shims & Audio/XMem/XThread Conformance Targets.
# Owned by Worker 4 (wt-dstorage-audio-diag / feat/dstorage-mfplat-audio-shims).

function(_openscarlett_configure_diag_dll tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4)
    elseif(MINGW)
        target_compile_definitions(${tgt} PRIVATE
            OPENSCARLETT_MINGW=1
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        target_compile_options(${tgt} PRIVATE
            -Wall
            -Wextra
            -Wno-unknown-pragmas
            -Wno-missing-field-initializers
            -Wno-unused-parameter
        )
    endif()
endfunction()

# 1. mfplat_xs.dll (MFResetDXGIDeviceManagerX + host mfplat.dll forwarding)
add_library(mfplat_xs SHARED src/shims/mfplat_shim.cpp)
set_target_properties(mfplat_xs PROPERTIES PREFIX "" OUTPUT_NAME "mfplat_xs")
target_link_libraries(mfplat_xs PRIVATE openscarlett_core)
_openscarlett_configure_diag_dll(mfplat_xs)

# 2. advapi32legacy.dll (UE4 startup/login/crypto/ETW forwarding + fallbacks)
add_library(advapi32legacy SHARED src/shims/console_diag_shims.cpp)
set_target_properties(advapi32legacy PROPERTIES PREFIX "" OUTPUT_NAME "advapi32legacy")
target_compile_definitions(advapi32legacy PRIVATE OPENSCARLETT_DIAG_TARGET_ADVAPI32LEGACY=1)
target_link_libraries(advapi32legacy PRIVATE openscarlett_core)
_openscarlett_configure_diag_dll(advapi32legacy)

# 3. KernelTraceControl.dll (ETW / kernel trace control clean stubs)
add_library(KernelTraceControl SHARED src/shims/console_diag_shims.cpp)
set_target_properties(KernelTraceControl PROPERTIES PREFIX "" OUTPUT_NAME "KernelTraceControl")
target_compile_definitions(KernelTraceControl PRIVATE OPENSCARLETT_DIAG_TARGET_KERNELTRACECONTROL=1)
target_link_libraries(KernelTraceControl PRIVATE openscarlett_core)
_openscarlett_configure_diag_dll(KernelTraceControl)

# 4. hix_xs.dll (hang-dump / debug telemetry forwards)
add_library(hix_xs SHARED src/shims/console_diag_shims.cpp)
set_target_properties(hix_xs PROPERTIES PREFIX "" OUTPUT_NAME "hix_xs")
target_compile_definitions(hix_xs PRIVATE OPENSCARLETT_DIAG_TARGET_HIX_XS=1)
target_link_libraries(hix_xs PRIVATE openscarlett_core)
_openscarlett_configure_diag_dll(hix_xs)

# 5. xbtp.dll (Xbox Binary Telemetry / profiling session stubs)
add_library(xbtp SHARED src/shims/console_diag_shims.cpp)
set_target_properties(xbtp PROPERTIES PREFIX "" OUTPUT_NAME "xbtp")
target_compile_definitions(xbtp PRIVATE OPENSCARLETT_DIAG_TARGET_XBTP=1)
target_link_libraries(xbtp PRIVATE openscarlett_core)
_openscarlett_configure_diag_dll(xbtp)

# Comprehensive Parity Test Suite
openscarlett_add_test(test_dstorage_audio_diag_parity tests/test_dstorage_audio_diag_parity.cpp)
target_link_libraries(test_dstorage_audio_diag_parity PRIVATE
    dstorage_xs
    kernelx
    XAudio2_9
    xcrdapi
)
add_dependencies(test_dstorage_audio_diag_parity
    xmem
    mfplat_xs
    advapi32legacy
    KernelTraceControl
    hix_xs
    xbtp
)
if(MINGW)
    target_compile_definitions(test_dstorage_audio_diag_parity PRIVATE
        OPENSCARLETT_MINGW=1
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    target_compile_options(test_dstorage_audio_diag_parity PRIVATE
        -Wall
        -Wextra
        -Wno-unknown-pragmas
        -Wno-missing-field-initializers
        -Wno-unused-parameter
    )
endif()
