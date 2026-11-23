#define OPENSCARLETT_BUILDING_ACPHAL 1
#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <malloc.h>
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "AcpHal.dll";
constexpr uint32_t kMaxAcpContexts = 256u;

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const AcpHalGuid* a, const AcpHalGuid& b) {
    return a && std::memcmp(a, &b, sizeof(AcpHalGuid)) == 0;
}

openscarlett::Xma2Config ToXma2Config(const AcpHalXmaContextDesc& desc) {
    openscarlett::Xma2Config cfg{};
    cfg.sample_rate = desc.sampleRate != 0 ? desc.sampleRate : 48000u;
    cfg.channels = desc.channels != 0 ? desc.channels : 2u;
    cfg.block_size = desc.blockSize != 0 ? desc.blockSize : openscarlett::kXma2DefaultBlockSize;
    cfg.subframes_per_frame = desc.subframesPerFrame != 0 ? desc.subframesPerFrame : 4u;
    cfg.quant_bits = desc.quantBits != 0 ? desc.quantBits : 12u;
    return cfg;
}

struct AcpShapeAllocation {
    uint32_t num_xma = 0;
    uint32_t num_src = 0;
    uint32_t num_eq = 0;
    uint32_t num_dma = 0;
};

// Real retail Xbox Series X|S ACP_SHAPE_CONTEXTS (0x60 bytes, verified against fmod.dll).
struct XboxAcpShapeContexts {
    uint32_t numSrcContexts;      // +0x00 (32 bytes each -> srcContexts)
    uint32_t numEqCompContexts;   // +0x04 (32 bytes each -> eqCompContexts)
    uint32_t numFiltVolContexts;  // +0x08 (32 bytes each -> filtVolContexts)
    uint32_t numDmaContexts;      // +0x0C (16 bytes each -> dmaContexts)
    uint32_t numXmaContexts;      // +0x10 (64 bytes each -> xmaContexts)
    uint32_t numPcmContexts;      // +0x14 (64 bytes each -> pcmContexts)
    void* srcContexts;            // +0x18
    void* eqCompContexts;         // +0x20
    void* filtVolContexts;        // +0x28
    void* dmaContexts;            // +0x30
    void* xmaContexts;            // +0x38
    void* pcmContexts;            // +0x40
    uint64_t reserved[3];         // +0x48..0x5F
};
static_assert(sizeof(XboxAcpShapeContexts) == 0x60, "XboxAcpShapeContexts must be 96 bytes");

struct XboxAcpMessage {
    uint32_t type = 0;       // +0x00 (0x10 = CommandCompleted, 0x80 = Disconnected)
    uint32_t error = 0;      // +0x04 (0 = OK)
    uint64_t reserved = 0;   // +0x08
    uint64_t commandId = 0;  // +0x10
    uint32_t extra = 0;      // +0x18
};
static_assert(sizeof(XboxAcpMessage) == 0x20 || sizeof(XboxAcpMessage) == 0x1C, "XboxAcpMessage size");

// Real retail Xbox Series X|S IAcpHal vtable (does NOT inherit IUnknown;
// verified against Xbox fmod.dll:
//   slot 0 (+0x00): Connect(cmdQueueLen, msgQueueLen)
//   slot 1 (+0x08): Disconnect()
//   slot 2 (+0x10): SubmitCommand(cmdType, cmdId, flags, pData, dataFlags)
//   slot 3 (+0x18): PopMessage(ACP_MESSAGE* outMsg) -> bool
//   slot 4 (+0x20): RegisterEvent / GetQueueSpace
//   slot 5 (+0x28): Release()
class XboxRetailAcpHalImpl {
public:
    virtual int32_t OPENSCARLETT_ACPHAL_CALL Connect(
        uint32_t commandQueueLength, uint32_t messageQueueLength) {
        (void)commandQueueLength;
        (void)messageQueueLength;
        std::lock_guard<std::mutex> lk(mutex_);
        connected_ = true;
        messages_.clear();
        return RecordHr("IAcpHal::Connect", ACPHAL_S_OK, 0);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL Disconnect() {
        std::lock_guard<std::mutex> lk(mutex_);
        connected_ = false;
        XboxAcpMessage msg{};
        msg.type = 0x80u;  // ACP_MESSAGE_TYPE_DISCONNECTED
        msg.error = 0u;
        messages_.push_back(msg);
        return RecordHr("IAcpHal::Disconnect", ACPHAL_S_OK, 1);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(
        uint32_t commandType,
        uint64_t commandId,
        uint32_t waitFlags,
        const void* pCommandData,
        uint32_t extraFlags) {
        (void)commandType;
        (void)waitFlags;
        (void)pCommandData;
        (void)extraFlags;
        if (commandId != 0ull) {
            std::lock_guard<std::mutex> lk(mutex_);
            XboxAcpMessage msg{};
            msg.type = 0x10u;  // ACP_MESSAGE_TYPE_COMMAND_COMPLETED
            msg.error = 0u;
            msg.commandId = commandId;
            messages_.push_back(msg);
        }
        return RecordHr("IAcpHal::SubmitCommand", ACPHAL_S_OK, 2);
    }

    virtual bool OPENSCARLETT_ACPHAL_CALL PopMessage(void* pMessage) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (messages_.empty()) {
            return false;
        }
        const XboxAcpMessage msg = messages_.front();
        messages_.pop_front();
        if (pMessage) {
            std::memcpy(pMessage, &msg, 0x1Cu);
        }
        return true;
    }

    virtual uint32_t OPENSCARLETT_ACPHAL_CALL GetCommandQueueSpace() {
        return 0x800u;
    }

    virtual uint32_t OPENSCARLETT_ACPHAL_CALL Release() {
        RecordOk("IAcpHal::Release", 5);
        delete this;
        return 0u;
    }

private:
    std::mutex mutex_;
    bool connected_ = false;
    std::deque<XboxAcpMessage> messages_;
};

std::atomic<bool> g_xbox_retail_acphal_mode{false};
std::mutex g_shape_alloc_mutex;
std::unordered_set<void*> g_synthetic_shape_allocs;
std::vector<void*> g_xbox_shape_buffers;

class AcpHalImpl final : public IAcpHal {
public:
    int32_t OPENSCARLETT_ACPHAL_CALL QueryInterface(
        const AcpHalGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_AcpHal_IUnknown) || GuidEquals(riid, IID_IAcpHal)) {
            AddRef();
            *ppvObject = static_cast<IAcpHal*>(this);
            return RecordHr("IAcpHal::QueryInterface", ACPHAL_S_OK, 0);
        }
        return RecordHr("IAcpHal::QueryInterface", ACPHAL_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL AddRef() override {
        RecordOk("IAcpHal::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_ACPHAL_CALL Release() override {
        RecordOk("IAcpHal::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_ACPHAL_CALL ConfigureXmaContext(
        uint32_t contextIndex, const AcpHalXmaContextDesc* desc) override {
        if (contextIndex >= kMaxAcpContexts || !desc) {
            return RecordHr("IAcpHal::ConfigureXmaContext", ACPHAL_E_INVALIDARG, 3);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto& dec = decoders_[contextIndex];
        if (!dec) {
            dec = std::make_unique<openscarlett::Xma2Decoder>();
        }
        const auto st = dec->Initialize(ToXma2Config(*desc));
        return RecordHr("IAcpHal::ConfigureXmaContext", st.ok() ? ACPHAL_S_OK : ACPHAL_E_INVALIDARG, 3);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(const AcpHalCommand* command) override {
        if (!command) {
            return RecordHr("IAcpHal::SubmitCommand", ACPHAL_E_INVALIDARG, 4);
        }
        const int32_t hr = ExecuteCommand(*command);
        return RecordHr("IAcpHal::SubmitCommand", hr, 4);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommands(
        uint32_t count, const AcpHalCommand* commands) override {
        if (count > 0 && !commands) {
            return RecordHr("IAcpHal::SubmitCommands", ACPHAL_E_INVALIDARG, 5);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t hr = ExecuteCommand(commands[i]);
            if (hr < 0) {
                return RecordHr("IAcpHal::SubmitCommands", hr, 5);
            }
        }
        return RecordHr("IAcpHal::SubmitCommands", ACPHAL_S_OK, 5);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL WaitIdle() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return RecordHr("IAcpHal::WaitIdle", ACPHAL_S_OK, 6);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) override {
        if (contextIndex >= kMaxAcpContexts || !packetCount) {
            return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_E_INVALIDARG, 7);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = decoders_.find(contextIndex);
        *packetCount = (it != decoders_.end() && it->second)
                           ? it->second->Stats().packets_parsed
                           : 0u;
        return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_S_OK, 7);
    }

private:
    int32_t ExecuteCommand(const AcpHalCommand& cmd) {
        std::lock_guard<std::mutex> lk(mutex_);
        switch (cmd.type) {
            case AcpHalCommand_Nop:
            case AcpHalCommand_Flush:
                return ACPHAL_S_OK;

            case AcpHalCommand_ResetContext: {
                if (cmd.contextIndex >= kMaxAcpContexts) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto it = decoders_.find(cmd.contextIndex);
                if (it != decoders_.end() && it->second) {
                    it->second->Reset();
                }
                return ACPHAL_S_OK;
            }

            case AcpHalCommand_XmaDecode: {
                if (cmd.contextIndex >= kMaxAcpContexts ||
                    !cmd.inputData || cmd.inputBytes == 0) {
                    return ACPHAL_E_INVALIDARG;
                }
                auto& dec = decoders_[cmd.contextIndex];
                if (!dec) {
                    dec = std::make_unique<openscarlett::Xma2Decoder>();
                }
                if (!dec->IsInitialized()) {
                    const auto init_st = dec->Initialize(ToXma2Config(cmd.xmaConfig));
                    if (!init_st.ok()) {
                        return ACPHAL_E_INVALIDARG;
                    }
                }

                if (cmd.outputFloatPcm) {
                    std::vector<float> pcm;
                    const auto st = dec->DecodePackets(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputFloatPcm, pcm.data(), to_copy * sizeof(float));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                if (cmd.outputInt16Pcm) {
                    std::vector<int16_t> pcm;
                    const auto st = dec->DecodePacketsInt16(cmd.inputData, cmd.inputBytes, pcm);
                    if (!st.ok()) {
                        return ACPHAL_E_FAIL;
                    }
                    const uint32_t to_copy = std::min<uint32_t>(
                        cmd.outputCapacitySamples, static_cast<uint32_t>(pcm.size()));
                    if (to_copy > 0) {
                        std::memcpy(cmd.outputInt16Pcm, pcm.data(), to_copy * sizeof(int16_t));
                    }
                    if (cmd.outSamplesWritten) {
                        *cmd.outSamplesWritten = to_copy;
                    }
                    return ACPHAL_S_OK;
                }

                return ACPHAL_E_INVALIDARG;
            }

            default:
                return ACPHAL_E_INVALIDARG;
        }
    }

    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<openscarlett::Xma2Decoder>> decoders_;
};

// ---------------------------------------------------------------------------
// Thread-safe Xbox APU Audio Heap Manager (ApuCreateHeap / ApuAlloc / ApuFree /
// ApuHeapGetState / ApuIsVirtualAddressValid / ApuMapVirtualAddress / ApuMapApuAddress)
// ---------------------------------------------------------------------------

constexpr uint32_t kDefaultApuCachedHeapBytes = 64u * 1024u * 1024u;
constexpr uint32_t kDefaultApuNonCachedHeapBytes = 64u * 1024u * 1024u;
constexpr uint32_t kCachedApuSpaceBase = 0x10000000u;
constexpr uint32_t kCachedApuSpaceSize = 0x70000000u;
constexpr uint32_t kNonCachedApuSpaceBase = 0x80000000u;
constexpr uint32_t kNonCachedApuSpaceSize = 0x70000000u;
constexpr uint32_t kDefaultAlignmentBytes = 64u;

inline bool IsPowerOfTwo(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

inline uint64_t AlignUpU64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

inline void* AlignedAllocHost(size_t size, size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, std::max<size_t>(alignment, sizeof(void*)), size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

inline void AlignedFreeHost(void* ptr) {
    if (!ptr) return;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

struct ApuFreeRange {
    uint32_t start = 0;
    uint32_t size = 0;
};

struct ApuHeapPool {
    uint32_t capacity = 0;
    uint32_t bytes_allocated = 0;
    uint32_t bytes_lost = 0;
    uint32_t allocation_count = 0;
    std::vector<ApuFreeRange> free_ranges;

    void InitAddressSpace(uint32_t base, uint32_t size) {
        free_ranges.clear();
        free_ranges.push_back(ApuFreeRange{base, size});
    }

    bool AllocateApuRange(uint32_t span, uint32_t alignment, APU_ADDRESS* out_addr) {
        for (size_t i = 0; i < free_ranges.size(); ++i) {
            const uint64_t r_start = free_ranges[i].start;
            const uint64_t r_end = r_start + free_ranges[i].size;
            const uint64_t aligned = AlignUpU64(r_start, alignment);
            if (aligned < r_start || aligned + span > r_end || aligned > 0xFFFFFFFFull) {
                continue;
            }

            const uint32_t alloc_start = static_cast<uint32_t>(aligned);
            const uint64_t alloc_end = aligned + span;
            const uint32_t prefix_size = static_cast<uint32_t>(aligned - r_start);
            const uint32_t suffix_size = static_cast<uint32_t>(r_end - alloc_end);

            if (prefix_size > 0u && suffix_size > 0u) {
                free_ranges[i].size = prefix_size;
                free_ranges.insert(
                    free_ranges.begin() + static_cast<std::ptrdiff_t>(i + 1u),
                    ApuFreeRange{static_cast<uint32_t>(alloc_end), suffix_size});
            } else if (prefix_size > 0u) {
                free_ranges[i].size = prefix_size;
            } else if (suffix_size > 0u) {
                free_ranges[i].start = static_cast<uint32_t>(alloc_end);
                free_ranges[i].size = suffix_size;
            } else {
                free_ranges.erase(free_ranges.begin() + static_cast<std::ptrdiff_t>(i));
            }

            *out_addr = alloc_start;
            return true;
        }
        return false;
    }

    void FreeApuRange(APU_ADDRESS start, uint32_t span) {
        if (span == 0u) return;
        ApuFreeRange entry{start, span};
        auto it = std::lower_bound(
            free_ranges.begin(),
            free_ranges.end(),
            entry.start,
            [](const ApuFreeRange& r, uint32_t addr) { return r.start < addr; });
        it = free_ranges.insert(it, entry);

        // Coalesce with next range if contiguous.
        if (it + 1 != free_ranges.end()) {
            const uint64_t cur_end = static_cast<uint64_t>(it->start) + it->size;
            if (cur_end == (it + 1)->start) {
                it->size += (it + 1)->size;
                free_ranges.erase(it + 1);
            }
        }
        // Coalesce with previous range if contiguous.
        if (it != free_ranges.begin()) {
            auto prev = it - 1;
            const uint64_t prev_end = static_cast<uint64_t>(prev->start) + prev->size;
            if (prev_end == it->start) {
                prev->size += it->size;
                free_ranges.erase(it);
            }
        }
    }
};

struct ApuAllocRecord {
    void* virtual_address = nullptr;
    APU_ADDRESS apu_address = APU_INVALID_ADDRESS;
    uint32_t requested_size = 0;
    uint32_t pad_bytes = 0;
    uint32_t apu_span = 0;
    bool non_cached = false;
};

class ApuHeapManager {
public:
    static ApuHeapManager& Instance() {
        static ApuHeapManager mgr;
        return mgr;
    }

    ApuHeapManager() {
        cached_.InitAddressSpace(kCachedApuSpaceBase, kCachedApuSpaceSize);
        non_cached_.InitAddressSpace(kNonCachedApuSpaceBase, kNonCachedApuSpaceSize);
    }

    ~ApuHeapManager() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : by_va_) {
            AlignedFreeHost(kv.second.virtual_address);
        }
        by_va_.clear();
        by_apu_.clear();
    }

    int32_t CreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes) {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t cached_used = cached_.bytes_allocated + cached_.bytes_lost;
        const uint32_t non_cached_used = non_cached_.bytes_allocated + non_cached_.bytes_lost;
        cached_.capacity = std::max(cached_bytes, cached_used);
        non_cached_.capacity = std::max(non_cached_bytes, non_cached_used);
        initialized_ = true;
        return ACPHAL_S_OK;
    }

    int32_t Alloc(
        void** virtual_address,
        APU_ADDRESS* physical_address,
        uint32_t size_in_bytes,
        uint32_t alignment_in_bytes,
        uint32_t flags) {
        if (physical_address) {
            *physical_address = APU_INVALID_ADDRESS;
        }
        if (!virtual_address) {
            return ACPHAL_E_POINTER;
        }
        *virtual_address = nullptr;

        if (size_in_bytes == 0u) {
            return ACPHAL_E_INVALIDARG;
        }
        if (alignment_in_bytes != 0u && !IsPowerOfTwo(alignment_in_bytes)) {
            return ACPHAL_E_INVALIDARG;
        }

        const uint32_t req_align =
            (alignment_in_bytes == 0u) ? kDefaultAlignmentBytes : alignment_in_bytes;
        const size_t os_align = std::max<size_t>(req_align, alignof(void*));
        const uint32_t apu_align = std::max<uint32_t>(req_align, 4u);
        const uint32_t pad_bytes =
            (size_in_bytes % 4u == 0u) ? 0u : (4u - (size_in_bytes % 4u));
        const uint64_t span64 = static_cast<uint64_t>(size_in_bytes) + pad_bytes;
        if (span64 > 0xFFFFFFFFull) {
            return ACPHAL_E_OUTOFMEMORY;
        }
        const uint32_t apu_span = static_cast<uint32_t>(span64);
        const bool is_non_cached = (flags & APU_ALLOC_NONCACHED) != 0u;

        std::lock_guard<std::mutex> lk(mutex_);
        if (!initialized_) {
            cached_.capacity = kDefaultApuCachedHeapBytes;
            non_cached_.capacity = kDefaultApuNonCachedHeapBytes;
            initialized_ = true;
        }

        const bool use_non_cached =
            is_non_cached && (non_cached_.capacity > 0u || cached_.capacity == 0u);
        ApuHeapPool& pool = use_non_cached ? non_cached_ : cached_;
        const uint64_t used_after =
            static_cast<uint64_t>(pool.bytes_allocated) + pool.bytes_lost + span64;
        if (used_after > pool.capacity) {
            return ACPHAL_E_OUTOFMEMORY;
        }

        APU_ADDRESS apu_addr = APU_INVALID_ADDRESS;
        if (!pool.AllocateApuRange(apu_span, apu_align, &apu_addr)) {
            return ACPHAL_E_OUTOFMEMORY;
        }

        void* ptr = AlignedAllocHost(size_in_bytes, os_align);
        if (!ptr) {
            pool.FreeApuRange(apu_addr, apu_span);
            return ACPHAL_E_OUTOFMEMORY;
        }
        std::memset(ptr, 0, size_in_bytes);

        const uintptr_t va_key = reinterpret_cast<uintptr_t>(ptr);
        ApuAllocRecord rec{};
        rec.virtual_address = ptr;
        rec.apu_address = apu_addr;
        rec.requested_size = size_in_bytes;
        rec.pad_bytes = pad_bytes;
        rec.apu_span = apu_span;
        rec.non_cached = use_non_cached;

        by_va_[va_key] = rec;
        by_apu_[apu_addr] = va_key;

        pool.bytes_allocated += size_in_bytes;
        pool.bytes_lost += pad_bytes;
        pool.allocation_count += 1u;

        *virtual_address = ptr;
        if (physical_address) {
            *physical_address = apu_addr;
        }
        return ACPHAL_S_OK;
    }

    int32_t Free(void* virtual_address) {
        if (!virtual_address) {
            return ACPHAL_S_OK;
        }

        void* to_free = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const uintptr_t va_key = reinterpret_cast<uintptr_t>(virtual_address);
            auto it = by_va_.find(va_key);
            if (it == by_va_.end()) {
                return ACPHAL_E_INVALIDARG;
            }

            const ApuAllocRecord rec = it->second;
            by_apu_.erase(rec.apu_address);
            by_va_.erase(it);

            ApuHeapPool& pool = rec.non_cached ? non_cached_ : cached_;
            pool.FreeApuRange(rec.apu_address, rec.apu_span);
            pool.bytes_allocated -= rec.requested_size;
            pool.bytes_lost -= rec.pad_bytes;
            pool.allocation_count -= 1u;
            to_free = rec.virtual_address;
        }

        AlignedFreeHost(to_free);
        return ACPHAL_S_OK;
    }

    int32_t GetState(APU_HEAP_STATE* state, uint32_t flags) {
        if (!state) {
            return ACPHAL_E_POINTER;
        }
        std::memset(state, 0, sizeof(*state));

        std::lock_guard<std::mutex> lk(mutex_);
        if (!initialized_) {
            return ACPHAL_S_OK;
        }

        const bool is_non_cached = (flags & APU_ALLOC_NONCACHED) != 0u;
        const ApuHeapPool& pool = is_non_cached ? non_cached_ : cached_;
        const uint64_t used = static_cast<uint64_t>(pool.bytes_allocated) + pool.bytes_lost;
        const uint32_t free_bytes =
            (used < pool.capacity) ? static_cast<uint32_t>(pool.capacity - used) : 0u;

        state->bytesAllocated = pool.bytes_allocated;
        state->bytesFree = free_bytes;
        state->bytesLost = pool.bytes_lost;
        state->maximumBlockSizeAvailable = free_bytes;
        state->allocationCount = pool.allocation_count;
        return ACPHAL_S_OK;
    }

    int32_t IsVirtualAddressValid(const void* virtual_address, uint32_t size_in_bytes) {
        if (!virtual_address) {
            return 0;
        }
        const uintptr_t va = reinterpret_cast<uintptr_t>(virtual_address);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = by_va_.upper_bound(va);
        if (it == by_va_.begin()) {
            return 0;
        }
        --it;
        const uintptr_t base = it->first;
        const ApuAllocRecord& rec = it->second;
        if (va < base) {
            return 0;
        }
        const uint64_t offset = static_cast<uint64_t>(va - base);
        if (offset >= rec.requested_size) {
            return 0;
        }
        if (size_in_bytes == 0u) {
            return 1;
        }
        return (offset + static_cast<uint64_t>(size_in_bytes) <= rec.requested_size) ? 1 : 0;
    }

    APU_ADDRESS MapVirtualAddress(const void* virtual_address) {
        if (!virtual_address) {
            return APU_INVALID_ADDRESS;
        }
        const uintptr_t va = reinterpret_cast<uintptr_t>(virtual_address);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = by_va_.upper_bound(va);
        if (it == by_va_.begin()) {
            return APU_INVALID_ADDRESS;
        }
        --it;
        const uintptr_t base = it->first;
        const ApuAllocRecord& rec = it->second;
        if (va < base) {
            return APU_INVALID_ADDRESS;
        }
        const uint64_t offset = static_cast<uint64_t>(va - base);
        if (offset >= rec.requested_size) {
            return APU_INVALID_ADDRESS;
        }
        return rec.apu_address + static_cast<uint32_t>(offset);
    }

    void* MapApuAddress(APU_ADDRESS apu_address) {
        if (apu_address == APU_INVALID_ADDRESS) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = by_apu_.upper_bound(apu_address);
        if (it == by_apu_.begin()) {
            return nullptr;
        }
        --it;
        const APU_ADDRESS apu_base = it->first;
        if (apu_address < apu_base) {
            return nullptr;
        }
        auto va_it = by_va_.find(it->second);
        if (va_it == by_va_.end()) {
            return nullptr;
        }
        const ApuAllocRecord& rec = va_it->second;
        const uint32_t offset = apu_address - apu_base;
        if (offset >= rec.requested_size) {
            return nullptr;
        }
        return static_cast<uint8_t*>(rec.virtual_address) + offset;
    }

private:
    std::mutex mutex_;
    bool initialized_ = false;
    ApuHeapPool cached_{};
    ApuHeapPool non_cached_{};
    std::map<uintptr_t, ApuAllocRecord> by_va_;
    std::map<APU_ADDRESS, uintptr_t> by_apu_;
};

static bool IsWritableAcpPtr(void* ptr, size_t bytes) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(ptr) + bytes <= region_end;
#else
    (void)bytes;
    return true;
#endif
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalCreate(IAcpHal** ppAcpHal) {
    if (!ppAcpHal) {
        return RecordHr("AcpHalCreate", ACPHAL_E_POINTER);
    }
    *ppAcpHal = nullptr;
    if (g_xbox_retail_acphal_mode.load(std::memory_order_acquire)) {
        auto* obj = new (std::nothrow) XboxRetailAcpHalImpl();
        if (!obj) {
            return RecordHr("AcpHalCreate", ACPHAL_E_OUTOFMEMORY);
        }
        *ppAcpHal = reinterpret_cast<IAcpHal*>(obj);
        return RecordHr("AcpHalCreate", ACPHAL_S_OK);
    }
    auto* obj = new (std::nothrow) AcpHalImpl();
    if (!obj) {
        return RecordHr("AcpHalCreate", ACPHAL_E_OUTOFMEMORY);
    }
    *ppAcpHal = obj;
    return RecordHr("AcpHalCreate", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalAllocateShapeContexts(
    const AcpHalShapeContexts* pRequested,
    AcpHalShapeContexts* pAllocated) {
    if (!pRequested) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_INVALIDARG);
    }

    // Detect real retail Xbox 1-arg AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXTS* pShapeContexts)
    // vs OpenScarlett's synthetic 2-arg (pRequested, pAllocated):
    // In synthetic tests, pAllocated is a valid writable pointer AND pRequested->internalAllocation == nullptr.
    // In retail Xbox (fmod.dll), only 1 arg is passed (pAllocated is null or non-writable, or
    // offset +0x10 holds numXmaContexts != 0).
    const bool is_synthetic_2arg =
        IsWritableAcpPtr(pAllocated, sizeof(AcpHalShapeContexts)) &&
        (pRequested->internalAllocation == nullptr);

    if (!is_synthetic_2arg) {
        g_xbox_retail_acphal_mode.store(true, std::memory_order_release);
        auto* xbox_ctx = reinterpret_cast<XboxAcpShapeContexts*>(
            const_cast<AcpHalShapeContexts*>(pRequested));

        std::lock_guard<std::mutex> lk(g_shape_alloc_mutex);
        auto alloc_array = [](uint32_t count, uint32_t elem_bytes) -> void* {
            if (count == 0u) return nullptr;
            const uint32_t total = count * elem_bytes;
            void* va = nullptr;
            APU_ADDRESS pa = APU_INVALID_ADDRESS;
            if (ApuHeapManager::Instance().Alloc(&va, &pa, total, 64u, APU_ALLOC_CACHED) == ACPHAL_S_OK && va) {
                g_xbox_shape_buffers.push_back(va);
                return va;
            }
            // Fallback to host aligned memory if APU heap is full.
            va = AlignedAllocHost(total, 64u);
            if (va) {
                std::memset(va, 0, total);
                g_xbox_shape_buffers.push_back(va);
            }
            return va;
        };

        xbox_ctx->srcContexts = alloc_array(xbox_ctx->numSrcContexts, 32u);
        xbox_ctx->eqCompContexts = alloc_array(xbox_ctx->numEqCompContexts, 32u);
        xbox_ctx->filtVolContexts = alloc_array(xbox_ctx->numFiltVolContexts, 32u);
        xbox_ctx->dmaContexts = alloc_array(xbox_ctx->numDmaContexts, 16u);
        xbox_ctx->xmaContexts = alloc_array(xbox_ctx->numXmaContexts, 64u);
        xbox_ctx->pcmContexts = alloc_array(xbox_ctx->numPcmContexts, 64u);
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
    }

    g_xbox_retail_acphal_mode.store(false, std::memory_order_release);
    auto* alloc = new (std::nothrow) AcpShapeAllocation();
    if (!alloc) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_OUTOFMEMORY);
    }
    alloc->num_xma = std::min<uint32_t>(pRequested->numXmaContexts, kMaxAcpContexts);
    alloc->num_src = pRequested->numSrcContexts;
    alloc->num_eq = pRequested->numEqContexts;
    alloc->num_dma = pRequested->numDmaContexts;

    pAllocated->numXmaContexts = alloc->num_xma;
    pAllocated->numSrcContexts = alloc->num_src;
    pAllocated->numEqContexts = alloc->num_eq;
    pAllocated->numDmaContexts = alloc->num_dma;
    pAllocated->internalAllocation = alloc;
    {
        std::lock_guard<std::mutex> lk(g_shape_alloc_mutex);
        g_synthetic_shape_allocs.insert(alloc);
    }
    return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalReleaseShapeContexts(AcpHalShapeContexts* pAllocated) {
    {
        std::lock_guard<std::mutex> lk(g_shape_alloc_mutex);
        if (IsWritableAcpPtr(pAllocated, sizeof(AcpHalShapeContexts)) &&
            pAllocated->internalAllocation != nullptr &&
            g_synthetic_shape_allocs.erase(pAllocated->internalAllocation) > 0) {
            delete static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
            std::memset(pAllocated, 0, sizeof(*pAllocated));
            return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
        }
        if (g_xbox_retail_acphal_mode.load(std::memory_order_acquire) || !g_xbox_shape_buffers.empty()) {
            for (void* ptr : g_xbox_shape_buffers) {
                if (ApuHeapManager::Instance().Free(ptr) != ACPHAL_S_OK) {
                    AlignedFreeHost(ptr);
                }
            }
            g_xbox_shape_buffers.clear();
            return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
        }
    }
    if (!pAllocated) {
        return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_E_INVALIDARG);
    }
    delete static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
    std::memset(pAllocated, 0, sizeof(*pAllocated));
    return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuCreateHeap(
    uint32_t cachedSizeInBytes,
    uint32_t nonCachedSizeInBytes) {
    return RecordHr(
        "ApuCreateHeap",
        ApuHeapManager::Instance().CreateHeap(cachedSizeInBytes, nonCachedSizeInBytes));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuAlloc(
    void** virtualAddress,
    APU_ADDRESS* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    return RecordHr(
        "ApuAlloc",
        ApuHeapManager::Instance().Alloc(
            virtualAddress, physicalAddress, sizeInBytes, alignmentInBytes, flags));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuFree(void* virtualAddress) {
    return RecordHr("ApuFree", ApuHeapManager::Instance().Free(virtualAddress));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuHeapGetState(
    APU_HEAP_STATE* state,
    uint32_t flags) {
    return RecordHr("ApuHeapGetState", ApuHeapManager::Instance().GetState(state, flags));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuIsVirtualAddressValid(
    const void* virtualAddress,
    uint32_t sizeInBytes) {
    const int32_t valid =
        ApuHeapManager::Instance().IsVirtualAddressValid(virtualAddress, sizeInBytes);
    RecordOk("ApuIsVirtualAddressValid");
    return valid;
}

APU_ADDRESS OPENSCARLETT_ACPHAL_CALL ApuMapVirtualAddress(const void* virtualAddress) {
    const APU_ADDRESS addr = ApuHeapManager::Instance().MapVirtualAddress(virtualAddress);
    RecordOk("ApuMapVirtualAddress");
    return addr;
}

void* OPENSCARLETT_ACPHAL_CALL ApuMapApuAddress(APU_ADDRESS apuAddress) {
    void* va = ApuHeapManager::Instance().MapApuAddress(apuAddress);
    RecordOk("ApuMapApuAddress");
    return va;
}

}  // extern "C"
