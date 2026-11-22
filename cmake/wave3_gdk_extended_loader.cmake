# OpenScarlett MIT Wave 3 — Extended GDK (XDisplay/UI/Invite/Accessibility/Speech),
# X3DAudio/XAudio2FX, and Topological DLL Dependency Initialization Ordering.
# Owned by wave1/gdk-display-audio-loader worktree.

set(_os_dep_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/wave3_dep_dlls")
file(MAKE_DIRECTORY "${_os_dep_gen_dir}")

# Generate 3 tiny helper DLLs (strictly inside build dir on E:\)
# that append their DLL name to %OPENSCARLETT_DEP_INIT_LOG% inside DllMain(DLL_PROCESS_ATTACH).
file(WRITE "${_os_dep_gen_dir}/test_dep_base.cpp" [=[
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace {
void AppendInitLogLine(const char* line) {
    wchar_t log_path[1024]{};
    const DWORD len = ::GetEnvironmentVariableW(L"OPENSCARLETT_DEP_INIT_LOG", log_path, 1024);
    if (len == 0 || len >= 1024) {
        return;
    }
    HANDLE h = ::CreateFileW(
        log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        ::CloseHandle(h);
    }
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl TestDepBase_GetValue() {
    return 10;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hinstDLL);
        AppendInitLogLine("test_dep_base.dll\n");
    }
    return TRUE;
}
]=])

file(WRITE "${_os_dep_gen_dir}/test_dep_mid.cpp" [=[
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

extern "C" __declspec(dllimport) int __cdecl TestDepBase_GetValue();

namespace {
void AppendInitLogLine(const char* line) {
    wchar_t log_path[1024]{};
    const DWORD len = ::GetEnvironmentVariableW(L"OPENSCARLETT_DEP_INIT_LOG", log_path, 1024);
    if (len == 0 || len >= 1024) {
        return;
    }
    HANDLE h = ::CreateFileW(
        log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        ::CloseHandle(h);
    }
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl TestDepMid_GetValue() {
    return TestDepBase_GetValue() + 20;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hinstDLL);
        AppendInitLogLine("test_dep_mid.dll\n");
    }
    return TRUE;
}
]=])

file(WRITE "${_os_dep_gen_dir}/test_dep_top.cpp" [=[
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

extern "C" __declspec(dllimport) int __cdecl TestDepBase_GetValue();
extern "C" __declspec(dllimport) int __cdecl TestDepMid_GetValue();

namespace {
void AppendInitLogLine(const char* line) {
    wchar_t log_path[1024]{};
    const DWORD len = ::GetEnvironmentVariableW(L"OPENSCARLETT_DEP_INIT_LOG", log_path, 1024);
    if (len == 0 || len >= 1024) {
        return;
    }
    HANDLE h = ::CreateFileW(
        log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        ::CloseHandle(h);
    }
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl TestDepTop_GetValue() {
    return TestDepBase_GetValue() + TestDepMid_GetValue() + 30;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hinstDLL);
        AppendInitLogLine("test_dep_top.dll\n");
    }
    return TRUE;
}
]=])

add_library(test_dep_base SHARED "${_os_dep_gen_dir}/test_dep_base.cpp")
set_target_properties(test_dep_base PROPERTIES PREFIX "" OUTPUT_NAME "test_dep_base")
if(MSVC)
    target_compile_options(test_dep_base PRIVATE /W4)
endif()

add_library(test_dep_mid SHARED "${_os_dep_gen_dir}/test_dep_mid.cpp")
set_target_properties(test_dep_mid PROPERTIES PREFIX "" OUTPUT_NAME "test_dep_mid")
target_link_libraries(test_dep_mid PRIVATE test_dep_base)
if(MSVC)
    target_compile_options(test_dep_mid PRIVATE /W4)
endif()

add_library(test_dep_top SHARED "${_os_dep_gen_dir}/test_dep_top.cpp")
set_target_properties(test_dep_top PROPERTIES PREFIX "" OUTPUT_NAME "test_dep_top")
target_link_libraries(test_dep_top PRIVATE test_dep_mid test_dep_base)
if(MSVC)
    target_compile_options(test_dep_top PRIVATE /W4)
endif()

openscarlett_add_test(test_gdk_extended_loader tests/test_gdk_extended_loader.cpp)
target_link_libraries(test_gdk_extended_loader PRIVATE xgameruntime XAudio2_9)
add_dependencies(test_gdk_extended_loader
    test_dep_base
    test_dep_mid
    test_dep_top
    openscarlett_hook
)
set_tests_properties(test_gdk_extended_loader PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
