// MIT headless runner driving untouched MIT Microsoft Xbox-GDK-Samples
// SimpleDirectStorageCombo/SampleImplementations/{SimpleLoad,StatusBatch,CompletionEvent,MultipleQueues}.cpp
// directly against OpenScarlett's dstorage_xs.dll via `openscarlett run <package_dir>`.

#include "pch.h"

#include "SampleImplementations/CompletionEvent.h"
#include "SampleImplementations/ImplementationBase.h"
#include "SampleImplementations/MultipleQueues.h"
#include "SampleImplementations/SimpleLoad.h"
#include "SampleImplementations/StatusBatch.h"

#include <cstdio>
#include <exception>
#include <string>

int main() {
    try {
        const std::wstring data_file = L"G:\\datafile.dat";

        HANDLE hf = ::CreateFileW(data_file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: open G:\\datafile.dat (err=%lu)\n",
                         static_cast<unsigned long>(::GetLastError()));
            return 1;
        }
        LARGE_INTEGER sz{};
        const BOOL ok_sz = ::GetFileSizeEx(hf, &sz);
        ::CloseHandle(hf);
        if (!ok_sz || sz.QuadPart < 4 * 1024 * 1024) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: invalid datafile size\n");
            return 1;
        }
        const uint64_t file_size = static_cast<uint64_t>(sz.QuadPart);

        SimpleLoad simple_load;
        if (!simple_load.RunSample(data_file)) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: SimpleLoad\n");
            return 1;
        }

        StatusBatch status_batch;
        if (!status_batch.RunSample(data_file, file_size)) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: StatusBatch\n");
            return 1;
        }

        CompletionEvent completion_event;
        if (!completion_event.RunSample(data_file, file_size)) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: CompletionEvent\n");
            return 1;
        }

        MultipleQueues multi_queues;
        if (!multi_queues.RunSample(data_file, file_size)) {
            std::fprintf(stderr, "[gdk_ms_directstorage_real] FAIL: MultipleQueues\n");
            return 1;
        }

        ImplementationBase::ShutdownDirectStorageObjects();
        std::printf("[gdk_ms_directstorage_real] PASS: Untouched MS SimpleLoad + StatusBatch + CompletionEvent + MultipleQueues\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[gdk_ms_directstorage_real] EXCEPTION: %s\n", ex.what());
        ImplementationBase::ShutdownDirectStorageObjects();
        return 1;
    }
}
