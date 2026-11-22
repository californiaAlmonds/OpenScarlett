#pragma once

// Microsoft DirectStorage for Xbox (dstorage_xs.dll) COM ABI definitions.
// Self-contained (does not include <windows.h> or <dstorage.h>).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_DSTORAGE)
#    define OPENSCARLETT_DSTORAGE_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_DSTORAGE_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_DSTORAGE_CALL __stdcall
#else
#  define OPENSCARLETT_DSTORAGE_API __attribute__((visibility("default")))
#  define OPENSCARLETT_DSTORAGE_CALL
#endif

constexpr int32_t OS_DSTORAGE_S_OK = 0x00000000;
constexpr int32_t OS_DSTORAGE_E_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t OS_DSTORAGE_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t OS_DSTORAGE_E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr int32_t OS_DSTORAGE_E_PENDING = static_cast<int32_t>(0x8000000Au);
constexpr int32_t OS_DSTORAGE_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t OS_DSTORAGE_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t OS_DSTORAGE_E_FILE_NOT_FOUND = static_cast<int32_t>(0x80070002u);
constexpr int32_t OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL = static_cast<int32_t>(0x8924000Au);
constexpr int32_t OS_DSTORAGE_E_REQUEST_TOO_LARGE = static_cast<int32_t>(0x8924000Bu);
constexpr int32_t OS_DSTORAGE_E_STAGING_BUFFER_INVALID = static_cast<int32_t>(0x8924000Cu);
constexpr int32_t OS_DSTORAGE_E_DECOMPRESSION_ERROR = static_cast<int32_t>(0x89240014u);
constexpr int32_t OS_DSTORAGE_E_IO_ERROR = static_cast<int32_t>(0x89240015u);
constexpr int32_t OS_DSTORAGE_E_CANCELLED = static_cast<int32_t>(0x89240016u);

struct DStorageGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(DStorageGuid) == 16, "DStorageGuid must be 16 bytes");

constexpr DStorageGuid IID_OS_DStorage_IUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr DStorageGuid IID_IDStorageFactoryX = {
    0x6924EA0Cu, 0xC3CDu, 0x4826u, {0xB1, 0x0A, 0xF6, 0x4F, 0x4E, 0xD9, 0x27, 0xC1}};
constexpr DStorageGuid IID_IDStorageFactoryX1 = {
    0x6924EA0Cu, 0xC3CDu, 0x4826u, {0xB1, 0x0A, 0xF6, 0x4F, 0x4E, 0xD9, 0x27, 0xC2}};
constexpr DStorageGuid IID_IDStorageQueueX = {
    0xCFDBD83Fu, 0x9E06u, 0x4FDAu, {0x8E, 0xA5, 0x69, 0x04, 0x21, 0x37, 0xF4, 0x9B}};
constexpr DStorageGuid IID_IDStorageQueueX1 = {
    0xCFDBD83Fu, 0x9E06u, 0x4FDAu, {0x8E, 0xA5, 0x69, 0x04, 0x21, 0x37, 0xF4, 0x9C}};
constexpr DStorageGuid IID_IDStorageFileX = {
    0x5DE95E7Bu, 0x955Au, 0x4868u, {0xA7, 0x3C, 0x24, 0x3B, 0x29, 0xF4, 0xB8, 0xDA}};
constexpr DStorageGuid IID_IDStorageStatusArrayX = {
    0x82397587u, 0x7CD5u, 0x453Bu, {0xA0, 0x2E, 0x31, 0x37, 0x9B, 0xD6, 0x48, 0x56}};

enum DSTORAGE_DEBUG : uint32_t {
    DSTORAGE_DEBUG_NONE = 0x00u,
    DSTORAGE_DEBUG_SHOW_ERRORS = 0x01u,
    DSTORAGE_DEBUG_BREAK_ON_ERROR = 0x02u,
    DSTORAGE_DEBUG_RECORD_OBJECT_NAMES = 0x04u,
};

enum DSTORAGE_PRIORITY : int8_t {
    DSTORAGE_PRIORITY_LOW = -1,
    DSTORAGE_PRIORITY_NORMAL = 0,
    DSTORAGE_PRIORITY_HIGH = 1,
    DSTORAGE_PRIORITY_REALTIME = 2,
};

// Xbox dstorage_xs.h declares DSTORAGE_REQUEST_SOURCE_TYPE with a UINT64 underlying type, which
// makes DSTORAGE_QUEUE_DESC::SourceType 8 bytes wide (Capacity sits at +0x08, not +0x02).
// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/enums/dstorage_request_source_type
enum DSTORAGE_REQUEST_SOURCE_TYPE : uint64_t {
    DSTORAGE_REQUEST_SOURCE_FILE = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1,
};

// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/enums/dstorage_bcpack_mode
enum DSTORAGE_BCPACK_MODE : uint64_t {
    DSTORAGE_BCPACK_MODE_NONE = 0,
    DSTORAGE_BCPACK_MODE_BC1 = 1,
    DSTORAGE_BCPACK_MODE_BC2 = 2,
    DSTORAGE_BCPACK_MODE_BC3 = 3,
    DSTORAGE_BCPACK_MODE_BC4 = 4,
    DSTORAGE_BCPACK_MODE_BC5 = 5,
    DSTORAGE_BCPACK_MODE_BC6H = 6,
    DSTORAGE_BCPACK_MODE_BC7 = 7,
};

// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/enums/dstorage_swizzle_mode
enum DSTORAGE_SWIZZLE_MODE : uint64_t {
    DSTORAGE_SWIZZLE_MODE_NONE = 0,
    DSTORAGE_SWIZZLE_MODE_4K_4K_8K = 1,
    DSTORAGE_SWIZZLE_MODE_4K_4K_24K = 2,
    DSTORAGE_SWIZZLE_MODE_4K_12K_4K_4K_8K = 3,
    DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K = 4,
};

// https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/enums/dstorage_command_type
enum DSTORAGE_COMMAND_TYPE : int32_t {
    DSTORAGE_COMMAND_TYPE_NONE = -1,
    DSTORAGE_COMMAND_TYPE_REQUEST = 0,
    DSTORAGE_COMMAND_TYPE_STATUS = 1,
    DSTORAGE_COMMAND_TYPE_SIGNAL = 2,
};

// OpenScarlett host extension (PC DirectStorage concepts). Xbox dstorage_xs.h has no destination
// type or compression-format enum; these only feed OpenScarlettDStorageRequestDesc below.
enum DSTORAGE_REQUEST_DESTINATION_TYPE : uint8_t {
    DSTORAGE_REQUEST_DESTINATION_MEMORY = 0,
    DSTORAGE_REQUEST_DESTINATION_BUFFER = 1,
    DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION = 2,
    DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES = 3,
    DSTORAGE_REQUEST_DESTINATION_TILES = 4,
};

enum DSTORAGE_COMPRESSION_FORMAT : uint8_t {
    DSTORAGE_COMPRESSION_FORMAT_NONE = 0,
    DSTORAGE_COMPRESSION_FORMAT_GDEFLATE = 1,
    DSTORAGE_COMPRESSION_FORMAT_ZLIB = 2,
    DSTORAGE_COMPRESSION_FORMAT_BCPACK = 3,
};

constexpr uint16_t DSTORAGE_MIN_QUEUE_CAPACITY = 128u;
constexpr uint16_t DSTORAGE_MAX_QUEUE_CAPACITY = 0x2000u;  // 8192, per dstorage_xs.h

constexpr uint32_t DSTORAGE_MIN_STAGING_BUFFER_SIZE = 64u * 1024u;           // 64 KiB
constexpr uint32_t DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE = 32u * 1024u * 1024u;  // 32 MiB
constexpr uint32_t DSTORAGE_MAX_STAGING_BUFFER_SIZE = 256u * 1024u * 1024u;  // 256 MiB

struct IDStorageFileX;
struct IDStorageStatusArrayX;
struct IDStorageQueueX;
struct IDStorageFactoryX;

struct DSTORAGE_CONFIGURATION {
    uint32_t NumSubmitThreads;
    int32_t NumBuiltInCpuDecompressionThreads;
    int32_t ForceMappingLayer;
    int32_t DisableBypassIO;
    int32_t DisableTelemetry;
    int32_t DisableGpuDecompressionMetacommand;
    int32_t DisableGpuDecompression;
};

struct DSTORAGE_CONFIGURATION1 {
    uint32_t NumSubmitThreads;
    int32_t NumBuiltInCpuDecompressionThreads;
    int32_t ForceMappingLayer;
    int32_t DisableBypassIO;
    int32_t DisableTelemetry;
    int32_t DisableGpuDecompressionMetacommand;
    int32_t DisableGpuDecompression;
    int32_t ForceFileBuffering;
};

// ---------------------------------------------------------------------------
// Title-facing Xbox structures (dstorage_xs.h). Layouts follow the public GDK reference:
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_queue_desc
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request_options
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_request
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_queue_info
//   https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/structs/dstorage_error_record
// Microsoft's MIT Xbox-GDK-Samples ship the same layout in
// Samples/System/SimpleDirectStorageCombo/DirectStorageWin32/dstorage_win32.h.
// A title hands these to the shim by pointer, so every size and offset is asserted below.
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201)  // nameless struct/union, as in dstorage_xs.h
#endif

struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;  // +0x00 (UINT64)
    uint16_t Capacity;                        // +0x08
    DSTORAGE_PRIORITY Priority;               // +0x0A (INT8)
    const char* Name;                         // +0x10
};

// 64-bit option word. Bit positions: ZlibDecompress 0, BcpackMode 1-4, SwizzleMode 5-20,
// DestinationIsPhysicalPages 21, SourceType 22, SourceIsPhysicalPages 23, Reserved 24-63.
union DSTORAGE_REQUEST_OPTIONS {
    struct {
        uint64_t ZlibDecompress : 1;
        uint64_t BcpackMode : 4;   // DSTORAGE_BCPACK_MODE
        uint64_t SwizzleMode : 16; // DSTORAGE_SWIZZLE_MODE
        uint64_t DestinationIsPhysicalPages : 1;
        uint64_t SourceType : 1;   // DSTORAGE_REQUEST_SOURCE_TYPE
        uint64_t SourceIsPhysicalPages : 1;
        uint64_t Reserved : 40;
    };
    uint64_t AsUINT64;
};

constexpr uint64_t DSTORAGE_REQUEST_OPTION_ZLIB_MASK = 1ull << 0;
constexpr uint32_t DSTORAGE_REQUEST_OPTION_BCPACK_SHIFT = 1u;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_BCPACK_MASK = 0xFull << 1;
constexpr uint32_t DSTORAGE_REQUEST_OPTION_SWIZZLE_SHIFT = 5u;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_SWIZZLE_MASK = 0xFFFFull << 5;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_DEST_PHYSICAL_MASK = 1ull << 21;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_SOURCE_TYPE_MASK = 1ull << 22;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_SOURCE_PHYSICAL_MASK = 1ull << 23;
constexpr uint64_t DSTORAGE_REQUEST_OPTION_RESERVED_MASK = ~((1ull << 24) - 1ull);

struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;  // +0x00
    union {                            // +0x08
        void* Destination;
        struct {
            uintptr_t* DestinationPageArray;  // PULONG_PTR, 64 KiB page numbers
            uint16_t DestinationPageOffset;   // +0x10
        };
    };
    uint32_t DestinationSize;  // +0x18
    union {                    // +0x20
        struct {
            IDStorageFileX* File;
            uint64_t FileOffset;  // +0x28
        };
        void* Source;
        struct {
            uintptr_t* SourcePageArray;
            uint16_t SourcePageOffset;  // +0x28
        };
    };
    uint32_t SourceSize;        // +0x30
    uint32_t IntermediateSize;  // +0x34
    uint64_t CancellationTag;   // +0x38
    const char* Name;           // +0x40
};

struct DSTORAGE_QUEUE_INFO {
    DSTORAGE_QUEUE_DESC Desc;              // +0x00
    uint16_t EmptySlotCount;               // +0x18
    uint16_t RequestCountUntilAutoSubmit;  // +0x1A
};

constexpr uint32_t DSTORAGE_REQUEST_MAX_NAME = 64u;
constexpr uint32_t DSTORAGE_ERROR_RECORD_MAX_PATH = 260u;  // MAX_PATH

struct DSTORAGE_ERROR_RECORD {
    uint32_t FailureCount;  // +0x00
    struct {
        int32_t HResult;                    // +0x08
        DSTORAGE_COMMAND_TYPE CommandType;  // +0x0C
        union {                             // +0x10
            struct {
                DSTORAGE_REQUEST_OPTIONS Options;                // +0x10
                void* Destination;                               // +0x18
                uint32_t DestinationSize;                        // +0x20
                char16_t Filename[DSTORAGE_ERROR_RECORD_MAX_PATH];  // +0x24 (WCHAR)
                uint64_t FileOffset;                             // +0x230
                void* Source;                                    // +0x238
                uint32_t SourceSize;                             // +0x240
                uint64_t CancellationTag;                        // +0x248
                char Name[DSTORAGE_REQUEST_MAX_NAME];            // +0x250
            } Request;
            struct {
                IDStorageStatusArrayX* StatusArray;
                uint32_t Index;
            } Status;
            struct {
                void* Fence;  // ID3D12Fence*
                uint64_t Value;
            } Signal;
        };
    } FirstFailure;
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

static_assert(sizeof(void*) == 8, "dstorage_xs.h layouts are x64-only");
static_assert(sizeof(DSTORAGE_QUEUE_DESC) == 0x18, "DSTORAGE_QUEUE_DESC must be 0x18 bytes");
static_assert(offsetof(DSTORAGE_QUEUE_DESC, Capacity) == 0x08, "DSTORAGE_QUEUE_DESC::Capacity");
static_assert(offsetof(DSTORAGE_QUEUE_DESC, Priority) == 0x0A, "DSTORAGE_QUEUE_DESC::Priority");
static_assert(offsetof(DSTORAGE_QUEUE_DESC, Name) == 0x10, "DSTORAGE_QUEUE_DESC::Name");
static_assert(sizeof(DSTORAGE_REQUEST_OPTIONS) == 8, "DSTORAGE_REQUEST_OPTIONS must be 8 bytes");
static_assert(sizeof(DSTORAGE_REQUEST) == 0x48, "DSTORAGE_REQUEST must be 0x48 bytes");
static_assert(offsetof(DSTORAGE_REQUEST, Options) == 0x00, "DSTORAGE_REQUEST::Options");
static_assert(offsetof(DSTORAGE_REQUEST, Destination) == 0x08, "DSTORAGE_REQUEST::Destination");
static_assert(offsetof(DSTORAGE_REQUEST, DestinationPageArray) == 0x08, "DSTORAGE_REQUEST::DestinationPageArray");
static_assert(offsetof(DSTORAGE_REQUEST, DestinationPageOffset) == 0x10, "DSTORAGE_REQUEST::DestinationPageOffset");
static_assert(offsetof(DSTORAGE_REQUEST, DestinationSize) == 0x18, "DSTORAGE_REQUEST::DestinationSize");
static_assert(offsetof(DSTORAGE_REQUEST, File) == 0x20, "DSTORAGE_REQUEST::File");
static_assert(offsetof(DSTORAGE_REQUEST, Source) == 0x20, "DSTORAGE_REQUEST::Source");
static_assert(offsetof(DSTORAGE_REQUEST, SourcePageArray) == 0x20, "DSTORAGE_REQUEST::SourcePageArray");
static_assert(offsetof(DSTORAGE_REQUEST, FileOffset) == 0x28, "DSTORAGE_REQUEST::FileOffset");
static_assert(offsetof(DSTORAGE_REQUEST, SourcePageOffset) == 0x28, "DSTORAGE_REQUEST::SourcePageOffset");
static_assert(offsetof(DSTORAGE_REQUEST, SourceSize) == 0x30, "DSTORAGE_REQUEST::SourceSize");
static_assert(offsetof(DSTORAGE_REQUEST, IntermediateSize) == 0x34, "DSTORAGE_REQUEST::IntermediateSize");
static_assert(offsetof(DSTORAGE_REQUEST, CancellationTag) == 0x38, "DSTORAGE_REQUEST::CancellationTag");
static_assert(offsetof(DSTORAGE_REQUEST, Name) == 0x40, "DSTORAGE_REQUEST::Name");
static_assert(sizeof(DSTORAGE_QUEUE_INFO) == 0x20, "DSTORAGE_QUEUE_INFO must be 0x20 bytes");
static_assert(offsetof(DSTORAGE_QUEUE_INFO, EmptySlotCount) == 0x18, "DSTORAGE_QUEUE_INFO::EmptySlotCount");
static_assert(offsetof(DSTORAGE_QUEUE_INFO, RequestCountUntilAutoSubmit) == 0x1A,
              "DSTORAGE_QUEUE_INFO::RequestCountUntilAutoSubmit");
static_assert(sizeof(DSTORAGE_ERROR_RECORD) == 0x290, "DSTORAGE_ERROR_RECORD must be 0x290 bytes");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure) == 0x08, "DSTORAGE_ERROR_RECORD::FirstFailure");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.CommandType) == 0x0C, "FirstFailure.CommandType");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.Request.Options) == 0x10, "FirstFailure.Request.Options");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.Request.Filename) == 0x24, "FirstFailure.Request.Filename");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.Request.FileOffset) == 0x230, "FirstFailure.Request.FileOffset");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.Request.CancellationTag) == 0x248,
              "FirstFailure.Request.CancellationTag");
static_assert(offsetof(DSTORAGE_ERROR_RECORD, FirstFailure.Request.Name) == 0x250, "FirstFailure.Request.Name");

// ---------------------------------------------------------------------------
// OpenScarlett host extension request (not part of dstorage_xs.h).
// Before this split the shim treated this PC-DirectStorage-style struct as the title ABI, which made
// IDStorageQueueX::EnqueueRequest misread every real Xbox DSTORAGE_REQUEST. It is kept for host-side
// features titles cannot reach (GDeflate, D3D12 texture/buffer destinations, custom codecs) and is
// submitted through OpenScarlettDStorageEnqueueRequestDesc(). Titles' requests are decoded into it.
// ---------------------------------------------------------------------------

struct OpenScarlettDStorageRequestDescOptions {
    DSTORAGE_COMPRESSION_FORMAT CompressionFormat;
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    DSTORAGE_REQUEST_DESTINATION_TYPE DestinationType;
};

struct DSTORAGE_SOURCE_MEMORY {
    const void* Source;
    uint32_t Size;
};

struct DSTORAGE_SOURCE_FILE {
    IDStorageFileX* Source;
    uint64_t Offset;
    uint32_t Size;
};

union DSTORAGE_SOURCE {
    DSTORAGE_SOURCE_MEMORY Memory;
    DSTORAGE_SOURCE_FILE File;
};

struct DSTORAGE_DESTINATION_MEMORY {
    void* Buffer;
    uint32_t Size;
};

union DSTORAGE_DESTINATION {
    DSTORAGE_DESTINATION_MEMORY Memory;
};

struct OpenScarlettDStorageRequestDesc {
    OpenScarlettDStorageRequestDescOptions Options;
    DSTORAGE_SOURCE Source;
    DSTORAGE_DESTINATION Destination;
    uint32_t UncompressedSize;
    uint64_t CancellationTag;
    const char* Name;
};

struct DSTORAGE_FILE_INFO {
    uint64_t FileSize;
};

struct DSTORAGE_QUEUE_STATS {
    uint64_t StagedCanceled;
    uint64_t SubmittedCanceled;
    uint64_t InFlightCanceled;
    uint64_t CompletedOk;
    uint64_t CompletedFailed;
    uint64_t CompletedCanceled;
    uint64_t PhysicalPageWrites;
    uint64_t XMemDestinationWrites;
    uint32_t StagingBufferSize;
    uint32_t LastIntermediateSize;
};

struct IDStorageFileX {
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL Release() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Close() = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL GetFileInformation(
        DSTORAGE_FILE_INFO* info) = 0;
};

struct IDStorageStatusArrayX {
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL Release() = 0;
    virtual bool OPENSCARLETT_DSTORAGE_CALL IsComplete(uint32_t index) = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL GetHResult(uint32_t index) = 0;
};

struct IDStorageQueueX {
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL Release() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Submit() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Close() = 0;
    virtual void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(
        DSTORAGE_ERROR_RECORD* record) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) = 0;
};

struct IDStorageFactoryX {
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_DSTORAGE_CALL Release() = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, const DStorageGuid* riid, void** ppv) = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL OpenFile(
        const wchar_t* path, const DStorageGuid* riid, void** ppv) = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL CreateStatusArray(
        uint32_t capacity, const char* name, const DStorageGuid* riid, void** ppv) = 0;
    virtual void OPENSCARLETT_DSTORAGE_CALL SetDebugFlags(uint32_t flags) = 0;
    virtual int32_t OPENSCARLETT_DSTORAGE_CALL SetStagingBufferSize(uint32_t size) = 0;
};

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL DStorageGetFactory(
    const DStorageGuid* riid, void** ppv);
OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration(
    const DSTORAGE_CONFIGURATION* configuration);
OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration1(
    const DSTORAGE_CONFIGURATION1* configuration);

// Submits a host-extension request (GDeflate, D3D12 destinations, custom codecs) to a queue.
OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageEnqueueRequestDesc(
    IDStorageQueueX* queue, const OpenScarlettDStorageRequestDesc* request);

// Attaches the host ID3D12Device used by D3D12 destinations. Xbox DSTORAGE_QUEUE_DESC has no
// device field, so the shim cannot take it from the title's queue description.
OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageSetQueueDevice(
    IDStorageQueueX* queue, void* d3d12_device);

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetQueueStats(
    IDStorageQueueX* queue, DSTORAGE_QUEUE_STATS* outStats);

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetStagingBufferSize(
    IDStorageFactoryX* factory, uint32_t* outSize);

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageDecompressWithIntermediate(
    DSTORAGE_COMPRESSION_FORMAT format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t intermediate_size,
    size_t* out_decompressed_size);

#ifdef __cplusplus
}
#endif
