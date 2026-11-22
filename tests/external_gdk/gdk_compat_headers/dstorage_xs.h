#pragma once

// MIT Microsoft DirectStorage for Xbox (<dstorage_xs.h>) public COM ABI header.
// Binary-compatible with dstorage_xs.dll and Microsoft::WRL::ComPtr / __uuidof().

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <unknwn.h>
#  include <windows.h>
#  define DSTORAGE_COMPAT_API __declspec(dllimport)
#  define DSTORAGE_COMPAT_CALL __stdcall
#else
#  define DSTORAGE_COMPAT_API
#  define DSTORAGE_COMPAT_CALL
#endif

constexpr HRESULT E_DSTORAGE_DECOMPRESSION_ERROR = static_cast<HRESULT>(0x89240014u);
constexpr HRESULT E_DSTORAGE_REQUEST_TOO_LARGE = static_cast<HRESULT>(0x8924000Bu);
constexpr HRESULT E_DSTORAGE_IO_ERROR = static_cast<HRESULT>(0x89240015u);

enum DSTORAGE_PRIORITY : int8_t {
    DSTORAGE_PRIORITY_LOW = -1,
    DSTORAGE_PRIORITY_NORMAL = 0,
    DSTORAGE_PRIORITY_HIGH = 1,
    DSTORAGE_PRIORITY_REALTIME = 2,
};

enum DSTORAGE_REQUEST_SOURCE_TYPE : uint64_t {
    DSTORAGE_REQUEST_SOURCE_FILE = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1,
};

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

enum DSTORAGE_SWIZZLE_MODE : uint64_t {
    DSTORAGE_SWIZZLE_MODE_NONE = 0,
};

enum DSTORAGE_COMMAND_TYPE : int32_t {
    DSTORAGE_COMMAND_TYPE_NONE = -1,
    DSTORAGE_COMMAND_TYPE_REQUEST = 0,
    DSTORAGE_COMMAND_TYPE_STATUS = 1,
    DSTORAGE_COMMAND_TYPE_SIGNAL = 2,
};

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

enum DSTORAGE_DEBUG : uint32_t {
    DSTORAGE_DEBUG_NONE = 0x00u,
    DSTORAGE_DEBUG_SHOW_ERRORS = 0x01u,
    DSTORAGE_DEBUG_BREAK_ON_ERROR = 0x02u,
    DSTORAGE_DEBUG_RECORD_OBJECT_NAMES = 0x04u,
};

constexpr uint16_t DSTORAGE_MIN_QUEUE_CAPACITY = 128u;
constexpr uint16_t DSTORAGE_MAX_QUEUE_CAPACITY = 0x2000u;

struct IDStorageFileX;
struct IDStorageStatusArrayX;
struct IDStorageQueueX;
struct IDStorageQueueX1;
struct IDStorageFactoryX;
struct IDStorageFactoryX1;

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

// Title-facing structures, laid out as in Microsoft's dstorage_xs.h (see
// include/openscarlett/io/dstorage_abi.h for the per-field offsets and doc links).
#pragma warning(push)
#pragma warning(disable : 4201)

struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    uint16_t Capacity;
    DSTORAGE_PRIORITY Priority;
    const char* Name;
};

union DSTORAGE_REQUEST_OPTIONS {
    struct {
        UINT64 ZlibDecompress : 1;
        UINT64 BcpackMode : 4;
        UINT64 SwizzleMode : 16;
        UINT64 DestinationIsPhysicalPages : 1;
        UINT64 SourceType : 1;
        UINT64 SourceIsPhysicalPages : 1;
        UINT64 Reserved : 40;
    };
    UINT64 AsUINT64;
};

struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;
    union {
        void* Destination;
        struct {
            PULONG_PTR DestinationPageArray;
            UINT16 DestinationPageOffset;
        };
    };
    UINT32 DestinationSize;
    union {
        struct {
            IDStorageFileX* File;
            UINT64 FileOffset;
        };
        void* Source;
        struct {
            PULONG_PTR SourcePageArray;
            UINT16 SourcePageOffset;
        };
    };
    UINT32 SourceSize;
    UINT32 IntermediateSize;
    UINT64 CancellationTag;
    const CHAR* Name;
};

struct DSTORAGE_FILE_INFO {
    uint64_t FileSize;
};

#define DSTORAGE_REQUEST_MAX_NAME 64

struct DSTORAGE_ERROR_RECORD {
    UINT32 FailureCount;
    struct {
        HRESULT HResult;
        DSTORAGE_COMMAND_TYPE CommandType;
        union {
            struct {
                DSTORAGE_REQUEST_OPTIONS Options;
                void* Destination;
                UINT32 DestinationSize;
                WCHAR Filename[MAX_PATH];
                UINT64 FileOffset;
                void* Source;
                UINT32 SourceSize;
                UINT64 CancellationTag;
                CHAR Name[DSTORAGE_REQUEST_MAX_NAME];
            } Request;
            struct {
                IDStorageStatusArrayX* StatusArray;
                UINT32 Index;
            } Status;
            struct {
                struct ID3D12Fence* Fence;
                UINT64 Value;
            } Signal;
        };
    } FirstFailure;
};

struct DSTORAGE_QUEUE_INFO {
    DSTORAGE_QUEUE_DESC Desc;
    uint16_t EmptySlotCount;
    uint16_t RequestCountUntilAutoSubmit;
};

#pragma warning(pop)

static_assert(sizeof(DSTORAGE_QUEUE_DESC) == 0x18, "DSTORAGE_QUEUE_DESC");
static_assert(sizeof(DSTORAGE_REQUEST) == 0x48, "DSTORAGE_REQUEST");
static_assert(offsetof(DSTORAGE_REQUEST, DestinationSize) == 0x18 && offsetof(DSTORAGE_REQUEST, File) == 0x20 &&
                  offsetof(DSTORAGE_REQUEST, FileOffset) == 0x28 && offsetof(DSTORAGE_REQUEST, SourceSize) == 0x30 &&
                  offsetof(DSTORAGE_REQUEST, IntermediateSize) == 0x34 &&
                  offsetof(DSTORAGE_REQUEST, CancellationTag) == 0x38 && offsetof(DSTORAGE_REQUEST, Name) == 0x40,
              "DSTORAGE_REQUEST layout");
static_assert(sizeof(DSTORAGE_ERROR_RECORD) == 0x290, "DSTORAGE_ERROR_RECORD");

// OpenScarlett host extension (not in dstorage_xs.h): GDeflate and other PC-only request kinds.
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

struct __declspec(uuid("5DE95E7B-955A-4868-A73C-243B29F4B8DA"))
IDStorageFileX : public IUnknown {
    virtual void DSTORAGE_COMPAT_CALL Close() = 0;
    virtual HRESULT DSTORAGE_COMPAT_CALL GetFileInformation(
        DSTORAGE_FILE_INFO* info) = 0;
};

struct __declspec(uuid("82397587-7CD5-453B-A02E-31379BD64856"))
IDStorageStatusArrayX : public IUnknown {
    virtual bool DSTORAGE_COMPAT_CALL IsComplete(uint32_t index) = 0;
    virtual HRESULT DSTORAGE_COMPAT_CALL GetHResult(uint32_t index) = 0;
};

struct __declspec(uuid("CFDBD83F-9E06-4FDA-8EA5-69042137F49B"))
IDStorageQueueX : public IUnknown {
    virtual void DSTORAGE_COMPAT_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) = 0;
    virtual void DSTORAGE_COMPAT_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) = 0;
    virtual void DSTORAGE_COMPAT_CALL EnqueueSetEvent(HANDLE handle) = 0;
    virtual void DSTORAGE_COMPAT_CALL Submit() = 0;
    virtual void DSTORAGE_COMPAT_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) = 0;
    virtual void DSTORAGE_COMPAT_CALL Close() = 0;
    virtual HANDLE DSTORAGE_COMPAT_CALL GetErrorEvent() = 0;
    virtual void DSTORAGE_COMPAT_CALL RetrieveErrorRecord(
        DSTORAGE_ERROR_RECORD* record) = 0;
    virtual void DSTORAGE_COMPAT_CALL Query(DSTORAGE_QUEUE_INFO* info) = 0;
};

struct __declspec(uuid("CFDBD83F-9E06-4FDA-8EA5-69042137F49C"))
IDStorageQueueX1 : public IDStorageQueueX {};

struct __declspec(uuid("6924EA0C-C3CD-4826-B10A-F64F4ED927C1"))
IDStorageFactoryX : public IUnknown {
    virtual HRESULT DSTORAGE_COMPAT_CALL CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) = 0;
    virtual HRESULT DSTORAGE_COMPAT_CALL OpenFile(
        const wchar_t* path, REFIID riid, void** ppv) = 0;
    virtual HRESULT DSTORAGE_COMPAT_CALL CreateStatusArray(
        uint32_t capacity, const char* name, REFIID riid, void** ppv) = 0;
    virtual void DSTORAGE_COMPAT_CALL SetDebugFlags(uint32_t flags) = 0;
    virtual HRESULT DSTORAGE_COMPAT_CALL SetStagingBufferSize(uint32_t size) = 0;
};

struct __declspec(uuid("6924EA0C-C3CD-4826-B10A-F64F4ED927C2"))
IDStorageFactoryX1 : public IDStorageFactoryX {};

#ifdef __cplusplus
extern "C" {
#endif

DSTORAGE_COMPAT_API HRESULT DSTORAGE_COMPAT_CALL DStorageGetFactory(
    REFIID riid, void** ppv);
DSTORAGE_COMPAT_API HRESULT DSTORAGE_COMPAT_CALL DStorageSetConfiguration(
    const DSTORAGE_CONFIGURATION* configuration);
DSTORAGE_COMPAT_API HRESULT DSTORAGE_COMPAT_CALL DStorageSetConfiguration1(
    const DSTORAGE_CONFIGURATION1* configuration);
DSTORAGE_COMPAT_API HRESULT DSTORAGE_COMPAT_CALL OpenScarlettDStorageEnqueueRequestDesc(
    IDStorageQueueX* queue, const OpenScarlettDStorageRequestDesc* request);

#ifdef __cplusplus
}
#endif
