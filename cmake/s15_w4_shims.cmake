# OpenScarlett Sprint 15 Worker 4: Loader, VFS, GDK/XAsync, KernelX/XMem & ETW Controller Tests
openscarlett_add_test(test_s15_loader_vfs_gdk_shims tests/test_s15_loader_vfs_gdk_shims.cpp)
target_link_libraries(test_s15_loader_vfs_gdk_shims PRIVATE
    openscarlett_core
    xgameruntime
)
add_dependencies(test_s15_loader_vfs_gdk_shims
    openscarlett_hook
    kernelx
    libHttpClient.GDK
    xmem
    pixevt
    advapi32legacy
    KernelTraceControl
)
