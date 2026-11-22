// Title-ABI test for IDStorageQueueX::EnqueueRequest.
//
// A title compiled against Microsoft's dstorage_xs.h passes a 0x48-byte DSTORAGE_REQUEST whose layout
// is documented at
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request_options
// This test does not trust OpenScarlett's own struct definition: it writes every field into a raw byte
// buffer at the documented offsets and hands that buffer to the shim, then checks the shim read it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "openscarlett/io/dstorage_abi.h"
#include "os_check.h"

namespace {

// Documented byte offsets (x64).
constexpr size_t kReqSize = 0x48;
constexpr size_t kOffOptions = 0x00;
constexpr size_t kOffDestination = 0x08;
constexpr size_t kOffDestinationSize = 0x18;
constexpr size_t kOffSource = 0x20;  // IDStorageFileX* File, or void* Source
constexpr size_t kOffFileOffset = 0x28;
constexpr size_t kOffSourceSize = 0x30;
constexpr size_t kOffIntermediateSize = 0x34;
constexpr size_t kOffCancellationTag = 0x38;
constexpr size_t kOffName = 0x40;

// DSTORAGE_REQUEST_OPTIONS bit positions.
constexpr uint64_t kOptZlib = 1ull << 0;
constexpr uint64_t BcpackMode(uint64_t mode) { return (mode & 0xFu) << 1; }
constexpr uint64_t SwizzleMode(uint64_t mode) { return (mode & 0xFFFFu) << 5; }
constexpr uint64_t kOptDestPhysical = 1ull << 21;
constexpr uint64_t kOptSourceMemory = 1ull << 22;
constexpr uint64_t kOptSourcePhysical = 1ull << 23;

template <typename T>
void Put(uint8_t* buf, size_t off, T value) {
    std::memcpy(buf + off, &value, sizeof(T));
}

struct RawRequest {
    alignas(8) uint8_t bytes[kReqSize]{};
    const DSTORAGE_REQUEST* get() const { return reinterpret_cast<const DSTORAGE_REQUEST*>(bytes); }
};

RawRequest MakeRequest(uint64_t options, const void* destination, uint32_t destination_size,
                       const void* source_or_file, uint64_t file_offset, uint32_t source_size,
                       uint32_t intermediate_size, uint64_t tag, const char* name) {
    RawRequest r;
    // Poison the padding so a shim reading the wrong offset sees garbage, not zeros.
    std::memset(r.bytes, 0xCD, sizeof(r.bytes));
    Put<uint64_t>(r.bytes, kOffOptions, options);
    Put<const void*>(r.bytes, kOffDestination, destination);
    Put<uint64_t>(r.bytes, kOffDestination + 8, 0);  // DestinationPageOffset slot (unused)
    Put<uint32_t>(r.bytes, kOffDestinationSize, destination_size);
    Put<const void*>(r.bytes, kOffSource, source_or_file);
    Put<uint64_t>(r.bytes, kOffFileOffset, file_offset);
    Put<uint32_t>(r.bytes, kOffSourceSize, source_size);
    Put<uint32_t>(r.bytes, kOffIntermediateSize, intermediate_size);
    Put<uint64_t>(r.bytes, kOffCancellationTag, tag);
    Put<const char*>(r.bytes, kOffName, name);
    return r;
}

void WaitComplete(IDStorageQueueX* q, IDStorageStatusArrayX* st, uint32_t index) {
    q->Submit();
    for (int i = 0; i < 2000 && !st->IsComplete(index); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    OS_REQUIRE(st->IsComplete(index));
}

IDStorageQueueX* MakeQueue(IDStorageFactoryX* factory, uint64_t source_type, DSTORAGE_PRIORITY prio,
                           const char* name) {
    // DSTORAGE_QUEUE_DESC as a title lays it out: SourceType UINT64 @0, Capacity @8, Priority @0xA, Name @0x10.
    alignas(8) uint8_t desc[0x18];
    std::memset(desc, 0xCD, sizeof(desc));
    Put<uint64_t>(desc, 0x00, source_type);
    Put<uint16_t>(desc, 0x08, DSTORAGE_MIN_QUEUE_CAPACITY);
    Put<int8_t>(desc, 0x0A, static_cast<int8_t>(prio));
    Put<const char*>(desc, 0x10, name);
    IDStorageQueueX* q = nullptr;
    OS_REQUIRE(factory->CreateQueue(reinterpret_cast<const DSTORAGE_QUEUE_DESC*>(desc), &IID_IDStorageQueueX,
                                    reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);
    DSTORAGE_QUEUE_INFO info{};
    q->Query(&info);
    OS_REQUIRE(info.Desc.SourceType == source_type);
    OS_REQUIRE(info.Desc.Capacity == DSTORAGE_MIN_QUEUE_CAPACITY);
    OS_REQUIRE(info.Desc.Priority == prio);
    return q;
}

void TestOptionBitfield() {
    std::printf("[1] DSTORAGE_REQUEST_OPTIONS bit positions\n");
    DSTORAGE_REQUEST_OPTIONS o{};
    o.AsUINT64 = kOptZlib | BcpackMode(DSTORAGE_BCPACK_MODE_BC7) | SwizzleMode(DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K) |
                 kOptDestPhysical | kOptSourceMemory | kOptSourcePhysical;
    OS_REQUIRE(o.ZlibDecompress == 1u);
    OS_REQUIRE(o.BcpackMode == DSTORAGE_BCPACK_MODE_BC7);
    OS_REQUIRE(o.SwizzleMode == DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K);
    OS_REQUIRE(o.DestinationIsPhysicalPages == 1u);
    OS_REQUIRE(o.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY);
    OS_REQUIRE(o.SourceIsPhysicalPages == 1u);
    OS_REQUIRE(o.Reserved == 0u);

    DSTORAGE_REQUEST_OPTIONS w{};
    w.BcpackMode = DSTORAGE_BCPACK_MODE_BC1;
    OS_REQUIRE(w.AsUINT64 == (1ull << 1));
    w.AsUINT64 = 0;
    w.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    OS_REQUIRE(w.AsUINT64 == kOptSourceMemory);
    w.AsUINT64 = 0;
    w.SwizzleMode = 0xFFFFu;
    OS_REQUIRE(w.AsUINT64 == (0xFFFFull << 5));
}

void TestStructOverlay() {
    std::printf("[2] DSTORAGE_REQUEST field overlay matches documented offsets\n");
    const char* name = "overlay";
    uint8_t dst[4]{};
    const uint8_t src[4]{1, 2, 3, 4};
    RawRequest raw = MakeRequest(kOptSourceMemory | BcpackMode(5), dst, 0x11223344u, src, 0x0102030405060708ull,
                                 0x55667788u, 0x99AABBCCu, 0xDEADBEEFCAFEF00Dull, name);
    const DSTORAGE_REQUEST& r = *raw.get();
    OS_REQUIRE(r.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY);
    OS_REQUIRE(r.Options.BcpackMode == 5u);
    OS_REQUIRE(r.Destination == dst);
    OS_REQUIRE(r.DestinationSize == 0x11223344u);
    OS_REQUIRE(r.Source == src);
    OS_REQUIRE(r.FileOffset == 0x0102030405060708ull);
    OS_REQUIRE(r.SourceSize == 0x55667788u);
    OS_REQUIRE(r.IntermediateSize == 0x99AABBCCu);
    OS_REQUIRE(r.CancellationTag == 0xDEADBEEFCAFEF00Dull);
    OS_REQUIRE(r.Name == name);
}

void TestMemoryRequest(IDStorageFactoryX* factory) {
    std::printf("[3] memory-sourced title request copies SourceSize bytes to Destination\n");
    IDStorageQueueX* q = MakeQueue(factory, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME, "xs_mem_q");
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(2u, "xs_mem_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    std::vector<uint8_t> src(4096);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    std::vector<uint8_t> dst(src.size() + 16u, 0xEEu);

    RawRequest raw = MakeRequest(kOptSourceMemory, dst.data(), static_cast<uint32_t>(src.size()), src.data(), 0,
                                 static_cast<uint32_t>(src.size()), 0u, 0x1234u, "xs_mem_req");
    q->EnqueueRequest(raw.get());
    q->EnqueueStatus(st, 0u);
    WaitComplete(q, st, 0u);
    OS_REQUIRE(st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(std::memcmp(dst.data(), src.data(), src.size()) == 0);
    for (size_t i = src.size(); i < dst.size(); ++i) OS_REQUIRE(dst[i] == 0xEEu);  // no overrun

    DSTORAGE_ERROR_RECORD err{};
    q->RetrieveErrorRecord(&err);
    OS_REQUIRE(err.FailureCount == 0u);

    q->Close();
    q->Release();
    st->Release();
}

void TestUnsupportedOptionsFailCleanly(IDStorageFactoryX* factory) {
    std::printf("[4] zlib / BCPack / swizzle / physical-page options are decoded, not misread\n");
    IDStorageQueueX* q = MakeQueue(factory, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME, "xs_opt_q");
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(1u, "xs_opt_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

    struct Case {
        uint64_t options;
        int32_t expected_hr;
    };
    const Case cases[] = {
        {kOptSourceMemory | kOptZlib, OS_DSTORAGE_E_DECOMPRESSION_ERROR},
        {kOptSourceMemory | BcpackMode(DSTORAGE_BCPACK_MODE_BC7), OS_DSTORAGE_E_DECOMPRESSION_ERROR},
        {kOptSourceMemory | kOptZlib | BcpackMode(DSTORAGE_BCPACK_MODE_BC1), OS_DSTORAGE_E_DECOMPRESSION_ERROR},
        {kOptSourceMemory | SwizzleMode(DSTORAGE_SWIZZLE_MODE_4K_4K_8K), OS_DSTORAGE_E_INVALIDARG},
        {kOptSourceMemory | kOptDestPhysical, OS_DSTORAGE_E_INVALIDARG},
        {kOptSourceMemory | kOptSourcePhysical, OS_DSTORAGE_E_INVALIDARG},
        {kOptSourceMemory | (1ull << 40), OS_DSTORAGE_E_INVALIDARG},  // reserved bit
    };
    std::vector<uint8_t> src(256, 0x5Au);
    std::vector<uint8_t> dst(1024, 0x00u);
    uint64_t tag = 0x7700u;
    for (const Case& c : cases) {
        ++tag;
        RawRequest raw = MakeRequest(c.options, dst.data(), static_cast<uint32_t>(dst.size()), src.data(), 0,
                                     static_cast<uint32_t>(src.size()), 128u, tag, "xs_bad_req");
        q->EnqueueRequest(raw.get());
        q->EnqueueStatus(st, 0u);
        WaitComplete(q, st, 0u);
        OS_REQUIRE(st->GetHResult(0u) == c.expected_hr);
        for (uint8_t b : dst) OS_REQUIRE(b == 0u);  // nothing written for a rejected request

        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 1u);
        OS_REQUIRE(err.FirstFailure.HResult == c.expected_hr);
        OS_REQUIRE(err.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST);
        OS_REQUIRE(err.FirstFailure.Request.Options.AsUINT64 == c.options);
        OS_REQUIRE(err.FirstFailure.Request.Destination == dst.data());
        OS_REQUIRE(err.FirstFailure.Request.DestinationSize == dst.size());
        OS_REQUIRE(err.FirstFailure.Request.Source == src.data());
        OS_REQUIRE(err.FirstFailure.Request.SourceSize == src.size());
        OS_REQUIRE(err.FirstFailure.Request.CancellationTag == tag);
        OS_REQUIRE(std::strcmp(err.FirstFailure.Request.Name, "xs_bad_req") == 0);
    }
    q->Close();
    q->Release();
    st->Release();
}

void TestCancellationTag(IDStorageFactoryX* factory) {
    std::printf("[5] CancelRequestsWithTag matches the tag at +0x38\n");
    IDStorageQueueX* q = MakeQueue(factory, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_NORMAL, "xs_cancel_q");
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(1u, "xs_cancel_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
    std::vector<uint8_t> src(64, 0x42u);
    std::vector<uint8_t> keep(64, 0u), drop(64, 0u);
    RawRequest r_keep = MakeRequest(kOptSourceMemory, keep.data(), 64u, src.data(), 0, 64u, 0u, 0xAAAA0001ull, "keep");
    RawRequest r_drop = MakeRequest(kOptSourceMemory, drop.data(), 64u, src.data(), 0, 64u, 0u, 0xBBBB0001ull, "drop");
    q->EnqueueRequest(r_keep.get());
    q->EnqueueRequest(r_drop.get());
    q->CancelRequestsWithTag(0xFFFF0000ull, 0xBBBB0000ull);
    q->EnqueueStatus(st, 0u);
    WaitComplete(q, st, 0u);
    OS_REQUIRE(st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(keep == src);
    for (uint8_t b : drop) OS_REQUIRE(b == 0u);
    q->Close();
    q->Release();
    st->Release();
}

#if defined(_WIN32)
void TestFileRequest(IDStorageFactoryX* factory) {
    std::printf("[6] file-sourced title request reads File at FileOffset\n");
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "openscarlett_xs_request_layout.bin";
    std::vector<uint8_t> payload(8192);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i * 13u + (i >> 8)) & 0xFFu);
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        OS_REQUIRE(ofs.good());
    }
    IDStorageFileX* file = nullptr;
    const std::wstring wpath = path.wstring();
    OS_REQUIRE(factory->OpenFile(wpath.c_str(), &IID_IDStorageFileX, reinterpret_cast<void**>(&file)) ==
               OS_DSTORAGE_S_OK);
    IDStorageQueueX* q = MakeQueue(factory, DSTORAGE_REQUEST_SOURCE_FILE, DSTORAGE_PRIORITY_NORMAL, "xs_file_q");
    IDStorageStatusArrayX* st = nullptr;
    OS_REQUIRE(factory->CreateStatusArray(1u, "xs_file_st", &IID_IDStorageStatusArrayX,
                                          reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);
    constexpr uint64_t kOffset = 4096u;
    constexpr uint32_t kSize = 2048u;
    std::vector<uint8_t> dst(kSize, 0u);
    RawRequest raw = MakeRequest(0u, dst.data(), kSize, file, kOffset, kSize, 0u, 1u, "xs_file_req");
    q->EnqueueRequest(raw.get());
    q->EnqueueStatus(st, 0u);
    WaitComplete(q, st, 0u);
    OS_REQUIRE(st->GetHResult(0u) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(std::memcmp(dst.data(), payload.data() + kOffset, kSize) == 0);
    q->Close();
    q->Release();
    st->Release();
    file->Close();
    file->Release();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
#endif

}  // namespace

int main() {
    TestOptionBitfield();
    TestStructOverlay();

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    TestMemoryRequest(factory);
    TestUnsupportedOptionsFailCleanly(factory);
    TestCancellationTag(factory);
#if defined(_WIN32)
    TestFileRequest(factory);
#else
    std::printf("[6] file-sourced request skipped: IDStorageFactoryX::OpenFile is Windows-only\n");
#endif
    factory->Release();
    std::printf("PASS: Xbox DSTORAGE_REQUEST layout (0x48) decoded by IDStorageQueueX::EnqueueRequest\n");
    return 0;
}
