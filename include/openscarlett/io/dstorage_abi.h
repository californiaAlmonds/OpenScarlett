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

enum DSTORAGE_REQUEST_SOURCE_TYPE : uint8_t {
    DSTORAGE_REQUEST_SOURCE_FILE = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1,
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

constexpr uint16_t DSTORAGE_MIN_QUEUE_CAPACITY = 128u;
constexpr uint16_t DSTORAGE_MAX_QUEUE_CAPACITY = 2000u;

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

struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    uint16_t Capacity;
    DSTORAGE_PRIORITY Priority;
    const char* Name;
    void* Device;
};

struct DSTORAGE_REQUEST_OPTIONS {
    DSTORAGE_COMPRESSION_FORMAT CompressionFormat;
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    DSTORAGE_REQUEST_DESTINATION_TYPE DestinationType;
    uint8_t Reserved[5];
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

struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;
    DSTORAGE_SOURCE Source;
    DSTORAGE_DESTINATION Destination;
    uint32_t UncompressedSize;
    uint64_t CancellationTag;
    const char* Name;
};

struct DSTORAGE_FILE_INFO {
    uint64_t FileSize;
};

struct DSTORAGE_ERROR_FIRST_FAILURE {
    int32_t HResult;
    uint32_t CommandType;
    DSTORAGE_REQUEST FailedRequest;
};

struct DSTORAGE_ERROR_RECORD {
    uint32_t FailureCount;
    DSTORAGE_ERROR_FIRST_FAILURE FirstFailure;
};

struct DSTORAGE_QUEUE_INFO {
    DSTORAGE_QUEUE_DESC Desc;
    uint16_t EmptySlotCount;
    uint16_t RequestCountUntilAutoSubmit;
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
