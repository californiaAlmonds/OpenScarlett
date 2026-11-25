#define OPENSCARLETT_BUILDING_ACPHAL 1
#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/audio_sync.h"
#include "openscarlett/audio/audio_trace.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#  include <malloc.h>
#  include <share.h>
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#endif

namespace {

using openscarlett::audio::SrwLock;
using openscarlett::audio::SrwLockGuard;

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

template <uint32_t kInitialBurst = 16u, uint32_t kLogEveryN = 256u>
inline bool ShouldTraceCall(std::atomic<uint64_t>& counter) noexcept {
    if (!openscarlett::audio::IsRetailTraceEnabled()) {
        return false;
    }
    const uint64_t n = counter.fetch_add(1ull, std::memory_order_relaxed);
    return (n < kInitialBurst) || ((n % kLogEveryN) == 0ull);
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
    void* stage_buffers[4] = {};
};

// Real retail Xbox Series X|S ACP_SHAPE_CONTEXTS (0x60 bytes, verified against fmod.dll).
struct XboxAcpShapeContexts {
    uint32_t numSrcContexts;      // +0x00 (32 bytes each -> srcContexts)
    uint32_t numEqCompContexts;   // +0x04 (160 bytes each -> eqCompContexts)
    uint32_t numFiltVolContexts;  // +0x08 (32 bytes each -> filtVolContexts)
    uint32_t numDmaContexts;      // +0x0C (16 bytes each -> dmaContexts)
    uint32_t numXmaContexts;      // +0x10 (64 bytes each -> xmaContexts)
    uint32_t numPcmContexts;      // +0x14 (32 bytes each -> pcmContexts)
    void* srcContexts;            // +0x18
    void* eqCompContexts;         // +0x20
    void* filtVolContexts;        // +0x28
    void* dmaContexts;            // +0x30
    void* xmaContexts;            // +0x38
    void* pcmContexts;            // +0x40
    uint32_t apuAddresses[6];     // +0x48..+0x5C (srcApu, eqCompApu, filtVolApu, dmaApu, xmaApu, pcmApu)
};
static_assert(sizeof(XboxAcpShapeContexts) == 0x60, "XboxAcpShapeContexts must be 96 bytes");

#pragma pack(push, 4)
struct XboxAcpMessage {
    uint32_t kind = 0;               // +0x00 (0x10 = CommandCompleted, 0x80 = Disconnected)
    uint32_t reserved = 0;           // +0x04 (0 = OK)
    uint64_t audioClockSamples = 0;  // +0x08
    uint64_t commandId = 0;          // +0x10
    uint32_t extra = 0;              // +0x18
};
#pragma pack(pop)
static_assert(sizeof(XboxAcpMessage) == 0x1Cu, "XboxAcpMessage must be 28 bytes");

std::atomic<bool> g_xbox_retail_acphal_mode{false};
SrwLock g_shape_alloc_mutex;
std::unordered_set<void*> g_synthetic_shape_allocs;
std::vector<void*> g_xbox_shape_buffers;
void* g_retail_xma_contexts_va = nullptr;
uint32_t g_retail_xma_context_count = 0u;

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
        SrwLockGuard lk(mutex_);
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
        SrwLockGuard lk(mutex_);
        return RecordHr("IAcpHal::WaitIdle", ACPHAL_S_OK, 6);
    }

    int32_t OPENSCARLETT_ACPHAL_CALL GetDecodedPacketCount(
        uint32_t contextIndex, uint64_t* packetCount) override {
        if (contextIndex >= kMaxAcpContexts || !packetCount) {
            return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_E_INVALIDARG, 7);
        }
        SrwLockGuard lk(mutex_);
        auto it = decoders_.find(contextIndex);
        *packetCount = (it != decoders_.end() && it->second)
                           ? it->second->Stats().packets_parsed
                           : 0u;
        return RecordHr("IAcpHal::GetDecodedPacketCount", ACPHAL_S_OK, 7);
    }

private:
    int32_t ExecuteCommand(const AcpHalCommand& cmd) {
        SrwLockGuard lk(mutex_);
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
    SrwLock mutex_;
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
constexpr uint32_t kMaxApuAlignmentBytes = 0x10000000u;  // 256 MiB max alignment
constexpr uint64_t kMaxShapeStageBytes = 256ull * 1024ull * 1024ull;

inline bool IsPowerOfTwo(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

inline bool CheckedAlignUpU64(uint64_t value, uint64_t alignment, uint64_t& out) {
    if (alignment == 0ull || (alignment & (alignment - 1ull)) != 0ull) {
        return false;
    }
    const uint64_t mask = alignment - 1ull;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    out = (value + mask) & ~mask;
    return true;
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
    uint32_t space_base = 0;
    uint32_t space_size = 0;
    uint32_t capacity = 0;
    uint32_t bytes_allocated = 0;
    uint32_t bytes_lost = 0;
    uint32_t allocation_count = 0;
    std::vector<ApuFreeRange> free_ranges;

    void InitAddressSpace(uint32_t base, uint32_t size) {
        space_base = base;
        space_size = size;
        free_ranges.clear();
        free_ranges.push_back(ApuFreeRange{base, size});
    }

    bool IsRangeWithinSpace(uint64_t start, uint64_t span) const noexcept {
        if (span == 0ull || start + span < start) {
            return false;
        }
        const uint64_t end = start + span;
        const uint64_t space_end = static_cast<uint64_t>(space_base) + static_cast<uint64_t>(space_size);
        return start >= space_base && end <= space_end && end <= 0x100000000ull;
    }

    bool AllocateApuRange(uint32_t span, uint32_t alignment, APU_ADDRESS* out_addr) {
        if (!out_addr || span == 0u || alignment == 0u) {
            return false;
        }
        for (size_t i = 0; i < free_ranges.size(); ++i) {
            const uint64_t r_start = free_ranges[i].start;
            const uint64_t r_end = r_start + static_cast<uint64_t>(free_ranges[i].size);
            uint64_t aligned = 0ull;
            if (!CheckedAlignUpU64(r_start, alignment, aligned)) {
                continue;
            }
            const uint64_t alloc_end = aligned + static_cast<uint64_t>(span);
            if (alloc_end < aligned || alloc_end > r_end ||
                !IsRangeWithinSpace(aligned, static_cast<uint64_t>(span))) {
                continue;
            }

            const uint32_t alloc_start = static_cast<uint32_t>(aligned);
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

    bool FreeApuRange(APU_ADDRESS start, uint32_t span) {
        if (span == 0u || !IsRangeWithinSpace(start, span)) {
            return false;
        }
        const uint64_t entry_start = start;
        const uint64_t entry_end = entry_start + static_cast<uint64_t>(span);

        auto it = std::lower_bound(
            free_ranges.begin(),
            free_ranges.end(),
            start,
            [](const ApuFreeRange& r, uint32_t addr) { return r.start < addr; });

        // Reject overlapping frees to preserve free-list disjointness invariant.
        if (it != free_ranges.end() && entry_end > static_cast<uint64_t>(it->start)) {
            return false;
        }
        if (it != free_ranges.begin()) {
            const auto prev = it - 1;
            const uint64_t prev_end = static_cast<uint64_t>(prev->start) + prev->size;
            if (prev_end > entry_start) {
                return false;
            }
        }

        ApuFreeRange entry{start, span};
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
        return true;
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
        SrwLockGuard lk(mutex_);
        for (const auto& kv : by_va_) {
            AlignedFreeHost(kv.second.virtual_address);
        }
        by_va_.clear();
        by_apu_.clear();
    }

    int32_t CreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes) {
        if ((cached_bytes == 0u && non_cached_bytes == 0u) ||
            cached_bytes > kCachedApuSpaceSize ||
            non_cached_bytes > kNonCachedApuSpaceSize) {
            return ACPHAL_E_INVALIDARG;
        }
        SrwLockGuard lk(mutex_);
        const uint64_t cached_used =
            static_cast<uint64_t>(cached_.bytes_allocated) + cached_.bytes_lost;
        const uint64_t non_cached_used =
            static_cast<uint64_t>(non_cached_.bytes_allocated) + non_cached_.bytes_lost;
        cached_.capacity = static_cast<uint32_t>(std::max<uint64_t>(cached_bytes, cached_used));
        non_cached_.capacity = static_cast<uint32_t>(std::max<uint64_t>(non_cached_bytes, non_cached_used));
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
        if (alignment_in_bytes != 0u &&
            (!IsPowerOfTwo(alignment_in_bytes) || alignment_in_bytes > kMaxApuAlignmentBytes)) {
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
        const bool retail_mode = g_xbox_retail_acphal_mode.load(std::memory_order_acquire);
        // Real Xbox GDK apu.h uses flags == 1 (cached), flags == 2 (non-cached), flags == 0 (cached);
        // synthetic ABI tests use APU_ALLOC_CACHED (0) and APU_ALLOC_NONCACHED (1).
        // flags == 2 ALWAYS routes to non_cached_ even before g_xbox_retail_acphal_mode is set.
        bool is_non_cached = false;
        if ((flags & 2u) != 0u) {
            is_non_cached = true;
        } else if (retail_mode) {
            is_non_cached = false;
        } else {
            is_non_cached = ((flags & APU_ALLOC_NONCACHED) != 0u);
        }

        SrwLockGuard lk(mutex_);
        if (!initialized_) {
            cached_.capacity = kDefaultApuCachedHeapBytes;
            non_cached_.capacity = kDefaultApuNonCachedHeapBytes;
            initialized_ = true;
        }

        bool use_non_cached =
            is_non_cached && (non_cached_.capacity > 0u || cached_.capacity == 0u);
        ApuHeapPool* pool_ptr = use_non_cached ? &non_cached_ : &cached_;
        uint64_t cur_used = static_cast<uint64_t>(pool_ptr->bytes_allocated) + pool_ptr->bytes_lost;
        if (span64 > pool_ptr->capacity || cur_used > pool_ptr->capacity ||
            span64 > pool_ptr->capacity - cur_used) {
            if (retail_mode) {
                // Retail fallback: real Xbox GDK defines APU_ALLOC_CACHED = 1 (vs 0 in synthetic
                // ABI). If the requested pool cannot satisfy the allocation, fall back to the
                // complementary pool when it has sufficient capacity — while strictly honoring
                // explicit ApuCreateHeap total capacity so OOM & atomic SHAPE rollback still work.
                ApuHeapPool* alt = use_non_cached ? &cached_ : &non_cached_;
                const uint64_t alt_used = static_cast<uint64_t>(alt->bytes_allocated) + alt->bytes_lost;
                if (span64 <= alt->capacity && alt_used <= alt->capacity &&
                    span64 <= alt->capacity - alt_used) {
                    use_non_cached = !use_non_cached;
                    pool_ptr = alt;
                } else {
                    return ACPHAL_E_OUTOFMEMORY;
                }
            } else {
                return ACPHAL_E_OUTOFMEMORY;
            }
        }
        ApuHeapPool& pool = *pool_ptr;

        APU_ADDRESS apu_addr = APU_INVALID_ADDRESS;
        if (!pool.AllocateApuRange(apu_span, apu_align, &apu_addr)) {
            return ACPHAL_E_OUTOFMEMORY;
        }

        // Defensive overlap & 64-bit address-range overflow guard on active APU map.
        if (HasApuOverlapLocked(apu_addr, apu_span)) {
            pool.FreeApuRange(apu_addr, apu_span);
            return ACPHAL_E_OUTOFMEMORY;
        }

        void* ptr = AlignedAllocHost(size_in_bytes, os_align);
        if (!ptr) {
            pool.FreeApuRange(apu_addr, apu_span);
            return ACPHAL_E_OUTOFMEMORY;
        }

        const uintptr_t va_key = reinterpret_cast<uintptr_t>(ptr);
        if (va_key + static_cast<uintptr_t>(size_in_bytes) < va_key ||
            HasVaOverlapLocked(va_key, size_in_bytes)) {
            AlignedFreeHost(ptr);
            pool.FreeApuRange(apu_addr, apu_span);
            return ACPHAL_E_OUTOFMEMORY;
        }

        std::memset(ptr, 0, size_in_bytes);

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
            SrwLockGuard lk(mutex_);
            const uintptr_t va_key = reinterpret_cast<uintptr_t>(virtual_address);
            auto it = by_va_.find(va_key);
            if (it == by_va_.end()) {
                return ACPHAL_E_INVALIDARG;
            }

            const ApuAllocRecord rec = it->second;
            by_apu_.erase(rec.apu_address);
            by_va_.erase(it);

            ApuHeapPool& pool = rec.non_cached ? non_cached_ : cached_;
            (void)pool.FreeApuRange(rec.apu_address, rec.apu_span);
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

        SrwLockGuard lk(mutex_);
        if (!initialized_) {
            return ACPHAL_S_OK;
        }

        const bool retail_mode = g_xbox_retail_acphal_mode.load(std::memory_order_acquire);
        const bool is_non_cached =
            ((flags & 2u) != 0u) || (!retail_mode && ((flags & APU_ALLOC_NONCACHED) != 0u));
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
        if (va + static_cast<uintptr_t>(size_in_bytes) < va) {
            return 0;
        }
        SrwLockGuard lk(mutex_);
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
        const uint64_t end_off = offset + static_cast<uint64_t>(size_in_bytes);
        if (end_off < offset) {
            return 0;
        }
        return (end_off <= rec.requested_size) ? 1 : 0;
    }

    APU_ADDRESS MapVirtualAddress(const void* virtual_address) {
        if (!virtual_address) {
            return APU_INVALID_ADDRESS;
        }
        const uintptr_t va = reinterpret_cast<uintptr_t>(virtual_address);
        SrwLockGuard lk(mutex_);
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
        const uint64_t apu64 = static_cast<uint64_t>(rec.apu_address) + offset;
        if (apu64 > 0xFFFFFFFFull) {
            return APU_INVALID_ADDRESS;
        }
        return static_cast<APU_ADDRESS>(apu64);
    }

    void* MapApuAddress(APU_ADDRESS apu_address) {
        if (apu_address == APU_INVALID_ADDRESS) {
            return nullptr;
        }
        SrwLockGuard lk(mutex_);
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

    // Validates that a caller-supplied APU range [apu_address, apu_address + size_in_bytes)
    // does not wrap 32-bit/64-bit address space and lies strictly inside a single live allocation
    // (or does not overlap any existing allocation when checking candidate placement).
    int32_t ValidateApuAddressRange(APU_ADDRESS apu_address, uint32_t size_in_bytes) {
        if (apu_address == APU_INVALID_ADDRESS || size_in_bytes == 0u) {
            return ACPHAL_E_INVALIDARG;
        }
        const uint64_t req_start = apu_address;
        const uint64_t req_end = req_start + static_cast<uint64_t>(size_in_bytes);
        if (req_end < req_start || req_end > 0x100000000ull) {
            return ACPHAL_E_INVALIDARG;
        }
        SrwLockGuard lk(mutex_);
        auto it = by_apu_.upper_bound(apu_address);
        if (it == by_apu_.begin()) {
            return ACPHAL_E_INVALIDARG;
        }
        --it;
        auto va_it = by_va_.find(it->second);
        if (va_it == by_va_.end()) {
            return ACPHAL_E_INVALIDARG;
        }
        const ApuAllocRecord& rec = va_it->second;
        const uint64_t rec_start = rec.apu_address;
        const uint64_t rec_end = rec_start + static_cast<uint64_t>(rec.requested_size);
        if (req_start < rec_start || req_end > rec_end) {
            return ACPHAL_E_INVALIDARG;
        }
        return ACPHAL_S_OK;
    }

    // Checks whether an arbitrary APU range [apu_start, apu_start + size_in_bytes) would overflow
    // or overlap any existing active allocation. Returns ACPHAL_S_OK if disjoint & in-space,
    // ACPHAL_E_INVALIDARG on overflow/overlap.
    int32_t CheckApuRangeDisjoint(APU_ADDRESS apu_start, uint32_t size_in_bytes) {
        if (apu_start == APU_INVALID_ADDRESS || size_in_bytes == 0u) {
            return ACPHAL_E_INVALIDARG;
        }
        const uint64_t req_start = apu_start;
        const uint64_t req_end = req_start + static_cast<uint64_t>(size_in_bytes);
        if (req_end < req_start || req_end > 0x100000000ull) {
            return ACPHAL_E_INVALIDARG;
        }
        SrwLockGuard lk(mutex_);
        if (!cached_.IsRangeWithinSpace(req_start, size_in_bytes) &&
            !non_cached_.IsRangeWithinSpace(req_start, size_in_bytes)) {
            return ACPHAL_E_INVALIDARG;
        }
        return HasApuOverlapLocked(apu_start, size_in_bytes) ? ACPHAL_E_INVALIDARG : ACPHAL_S_OK;
    }

private:
    bool HasApuOverlapLocked(APU_ADDRESS start, uint32_t span) const {
        const uint64_t s = start;
        const uint64_t e = s + static_cast<uint64_t>(span);
        if (e < s || e > 0x100000000ull) return true;
        auto it = by_apu_.lower_bound(start);
        if (it != by_apu_.end()) {
            if (static_cast<uint64_t>(it->first) < e) {
                return true;
            }
        }
        if (it != by_apu_.begin()) {
            auto prev = std::prev(it);
            auto va_it = by_va_.find(prev->second);
            if (va_it != by_va_.end()) {
                const uint64_t prev_end =
                    static_cast<uint64_t>(va_it->second.apu_address) + va_it->second.apu_span;
                if (prev_end > s) {
                    return true;
                }
            }
        }
        return false;
    }

    bool HasVaOverlapLocked(uintptr_t start, uint32_t size) const {
        const uint64_t s = static_cast<uint64_t>(start);
        const uint64_t e = s + static_cast<uint64_t>(size);
        if (e < s) return true;
        auto it = by_va_.lower_bound(start);
        if (it != by_va_.end()) {
            if (static_cast<uint64_t>(it->first) < e) {
                return true;
            }
        }
        if (it != by_va_.begin()) {
            auto prev = std::prev(it);
            const uint64_t prev_end =
                static_cast<uint64_t>(prev->first) + static_cast<uint64_t>(prev->second.requested_size);
            if (prev_end > s) {
                return true;
            }
        }
        return false;
    }

    SrwLock mutex_;
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
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    if (start + bytes < start) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + bytes <= region_end;
#else
    return true;
#endif
}

static bool IsReadableAcpPtr(const void* ptr, size_t bytes) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000ull) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    if (start + bytes < start) {
        return false;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!readable) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + bytes <= region_end;
#else
    return true;
#endif
}

inline void QueryQpcAndFreq(uint64_t& out_ticks, uint64_t& out_freq) noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{}, freq{};
    if (::QueryPerformanceCounter(&now) != FALSE &&
        ::QueryPerformanceFrequency(&freq) != FALSE &&
        now.QuadPart >= 0 && freq.QuadPart > 0) {
        out_ticks = static_cast<uint64_t>(now.QuadPart);
        out_freq = static_cast<uint64_t>(freq.QuadPart);
        return;
    }
#endif
    out_ticks = 0ull;
    out_freq = 10000000ull;
}

// Full 12-slot retail Xbox Series X|S IAcpHal vtable (does NOT inherit IUnknown).
class XboxRetailAcpHalImpl {
public:
    virtual int32_t OPENSCARLETT_ACPHAL_CALL Connect(
        uint32_t commandQueueLength, uint32_t messageQueueLength) {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(),
                "IAcpHal::Connect(cmdLen=0x%X, msgLen=0x%X)",
                commandQueueLength, messageQueueLength);
        }
        SrwLockGuard lk(mutex_);
        connected_ = true;
        messages_.clear();
        max_messages_ = std::clamp<uint32_t>(
            messageQueueLength != 0u ? messageQueueLength : 4096u, 256u, 16384u);
        QueryQpcAndFreq(connect_qpc_, qpc_freq_);
        audio_clock_samples_ = 128ull;
        xma_enabled_mask_.fill(0ull);
        return RecordHr("IAcpHal::Connect", ACPHAL_S_OK, 0);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL Disconnect() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Disconnect()");
        }
        SrwLockGuard lk(mutex_);
        connected_ = false;
        const uint64_t clk = AdvanceAudioClockLocked();
        XboxAcpMessage msg{};
        msg.kind = 0x80u;  // ACP_MESSAGE_TYPE_DISCONNECTED
        msg.reserved = 0u;
        msg.audioClockSamples = clk;
        if (messages_.size() >= max_messages_) {
            messages_.pop_front();
        }
        messages_.push_back(msg);
        return RecordHr("IAcpHal::Disconnect", ACPHAL_S_OK, 1);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL SubmitCommand(
        uint32_t commandType,
        uint64_t commandId,
        uint32_t frameIndex,
        const void* pCommandData,
        uint32_t fenceApuAddr) {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(),
                "IAcpHal::SubmitCommand(type=0x%X, id=0x%llX, frame=%u, fence=0x%X)",
                commandType, static_cast<unsigned long long>(commandId),
                frameIndex, fenceApuAddr);
        }
        (void)frameIndex;

        if (fenceApuAddr != 0u) {
            void* fenceVa = ApuHeapManager::Instance().MapApuAddress(fenceApuAddr);
            if (fenceVa && IsWritableAcpPtr(fenceVa, sizeof(uint32_t))) {
                if ((reinterpret_cast<uintptr_t>(fenceVa) & 3u) == 0u) {
                    std::atomic_ref<uint32_t>(*static_cast<uint32_t*>(fenceVa))
                        .store(1u, std::memory_order_release);
                } else {
                    std::atomic_thread_fence(std::memory_order_release);
                    const uint32_t one = 1u;
                    std::memcpy(fenceVa, &one, sizeof(uint32_t));
                }
            }
        }

        SrwLockGuard lk(mutex_);
        switch (commandType) {
            case 1u:     // CMD_FLOWGRAPH
            case 0x10u:  // CMD_DMA_CONSUMED
                break;

            case 2u: {   // CMD_CONFIG
                uint32_t cfg_val = 0u;
                if (pCommandData && IsReadableAcpPtr(pCommandData, sizeof(uint32_t))) {
                    std::memcpy(&cfg_val, pCommandData, sizeof(uint32_t));
                }
                last_config_ = cfg_val;
                break;
            }

            case 5u: {   // CMD_XMA_ENABLE_ONE
                if (pCommandData && IsReadableAcpPtr(pCommandData, sizeof(uint32_t))) {
                    uint32_t xmaIdx = 0u;
                    std::memcpy(&xmaIdx, pCommandData, sizeof(uint32_t));
                    if (xmaIdx < kMaxRetailXmaContexts) {
                        SetXmaMaskBit(xmaIdx, true);
                        ProcessRetailXmaContextLocked(xmaIdx);
                    }
                }
                break;
            }

            case 6u: {   // CMD_XMA_ENABLE_MASK
                if (pCommandData && IsReadableAcpPtr(pCommandData, 16u * sizeof(uint32_t))) {
                    uint32_t words[16] = {};
                    std::memcpy(words, pCommandData, sizeof(words));
                    for (uint32_t w = 0u; w < 16u; ++w) {
                        uint32_t bits = words[w];
                        while (bits != 0u) {
                            unsigned long bit = 0u;
#if defined(_WIN32)
                            _BitScanForward(&bit, bits);
#else
                            bit = static_cast<unsigned long>(__builtin_ctz(bits));
#endif
                            const uint32_t xmaIdx = w * 32u + static_cast<uint32_t>(bit);
                            SetXmaMaskBit(xmaIdx, true);
                            ProcessRetailXmaContextLocked(xmaIdx);
                            bits &= (bits - 1u);
                        }
                    }
                }
                break;
            }

            case 7u: {   // CMD_XMA_DISABLE_ONE
                if (pCommandData && IsReadableAcpPtr(pCommandData, sizeof(uint32_t))) {
                    uint32_t xmaIdx = 0u;
                    std::memcpy(&xmaIdx, pCommandData, sizeof(uint32_t));
                    if (xmaIdx < kMaxRetailXmaContexts) {
                        SetXmaMaskBit(xmaIdx, false);
                    }
                }
                break;
            }

            case 8u: {   // CMD_XMA_DISABLE_MASK
                if (pCommandData && IsReadableAcpPtr(pCommandData, 16u * sizeof(uint32_t))) {
                    uint32_t words[16] = {};
                    std::memcpy(words, pCommandData, sizeof(words));
                    for (uint32_t w = 0u; w < 16u; ++w) {
                        uint32_t bits = words[w];
                        while (bits != 0u) {
                            unsigned long bit = 0u;
#if defined(_WIN32)
                            _BitScanForward(&bit, bits);
#else
                            bit = static_cast<unsigned long>(__builtin_ctz(bits));
#endif
                            const uint32_t xmaIdx = w * 32u + static_cast<uint32_t>(bit);
                            SetXmaMaskBit(xmaIdx, false);
                            bits &= (bits - 1u);
                        }
                    }
                }
                break;
            }

            case 0xEu:   // CMD_XMA_UPDATE
            case 0x12u:  // CMD_XMA_OUT_CONSUMED
            case 0x13u:  // CMD_XMA_SET_INPUT
            {
                if (pCommandData && IsReadableAcpPtr(pCommandData, sizeof(uint32_t))) {
                    uint32_t xmaIdx = 0u;
                    std::memcpy(&xmaIdx, pCommandData, sizeof(uint32_t));
                    if (xmaIdx < kMaxRetailXmaContexts) {
                        ProcessRetailXmaContextLocked(xmaIdx);
                    }
                }
                break;
            }

            default:
                openscarlett::audio::LogUnimplAudio(
                    "AcpHal", OPENSCARLETT_CALLER_ADDR(),
                    "SubmitCommand type=0x%X id=0x%llX",
                    commandType, static_cast<unsigned long long>(commandId));
                break;
        }

        const uint64_t clk = AdvanceAudioClockLocked();
        if (commandId != 0ull) {
            XboxAcpMessage msg{};
            msg.kind = 0x10u;  // ACP_MESSAGE_TYPE_COMMAND_COMPLETED
            msg.reserved = 0u;
            msg.audioClockSamples = clk;
            msg.commandId = commandId;
            msg.extra = 0u;
            if (messages_.size() >= max_messages_) {
                messages_.pop_front();
            }
            messages_.push_back(msg);
        }
        return RecordHr("IAcpHal::SubmitCommand", ACPHAL_S_OK, 2);
    }

    virtual bool OPENSCARLETT_ACPHAL_CALL PopMessage(void* pMessage) {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::PopMessage()");
        }
        SrwLockGuard lk(mutex_);
        if (messages_.empty()) {
            return false;
        }
        const XboxAcpMessage msg = messages_.front();
        messages_.pop_front();
        if (pMessage && IsWritableAcpPtr(pMessage, sizeof(XboxAcpMessage))) {
            std::memcpy(pMessage, &msg, sizeof(XboxAcpMessage));
        }
        return true;
    }

    virtual uint32_t OPENSCARLETT_ACPHAL_CALL GetNumMessages() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::GetNumMessages()");
        }
        SrwLockGuard lk(mutex_);
        return static_cast<uint32_t>(messages_.size());
    }

    virtual uint32_t OPENSCARLETT_ACPHAL_CALL Release() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Release()");
        }
        RecordOk("IAcpHal::Release", 5);
        delete this;
        return 0u;
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL UnsupportedSlot6() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::UnsupportedSlot6()");
        }
        openscarlett::audio::LogUnimplAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Slot6 unimplemented");
        return static_cast<int32_t>(0x8AC80F0Eu);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL UnsupportedSlot7() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::UnsupportedSlot7()");
        }
        openscarlett::audio::LogUnimplAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Slot7 unimplemented");
        return static_cast<int32_t>(0x8AC80F0Eu);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL UnsupportedSlot8() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::UnsupportedSlot8()");
        }
        openscarlett::audio::LogUnimplAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Slot8 unimplemented");
        return static_cast<int32_t>(0x8AC80F0Eu);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL UnsupportedSlot9() {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::UnsupportedSlot9()");
        }
        openscarlett::audio::LogUnimplAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Slot9 unimplemented");
        return static_cast<int32_t>(0x8AC80F0Eu);
    }

    virtual int32_t OPENSCARLETT_ACPHAL_CALL MapDriverRegion(uint64_t* outVa) {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::MapDriverRegion()");
        }
        if (outVa && IsWritableAcpPtr(outVa, sizeof(uint64_t))) {
            *outVa = 0ull;
        }
        openscarlett::audio::LogUnimplAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::MapDriverRegion unimplemented");
        return static_cast<int32_t>(0x80070032u);
    }

    virtual uint32_t OPENSCARLETT_ACPHAL_CALL Destroy(uint32_t flags) {
        static std::atomic<uint64_t> trace_cnt{0u};
        if (ShouldTraceCall(trace_cnt)) {
            openscarlett::audio::TraceRetailAudio(
                "AcpHal", OPENSCARLETT_CALLER_ADDR(), "IAcpHal::Destroy(flags=0x%X)", flags);
        }
        (void)flags;
        RecordOk("IAcpHal::Destroy", 11);
        delete this;
        return 0u;
    }

private:
    static constexpr uint32_t kMaxRetailXmaContexts = 512u;

    struct RetailXmaCtxState {
        std::unique_ptr<openscarlett::Xma2Decoder> decoder;
        uint32_t sample_rate = 0u;
        uint16_t channels = 0u;
        std::deque<int16_t> pending_pcm;
    };

    void SetXmaMaskBit(uint32_t idx, bool enable) noexcept {
        const uint32_t word = idx >> 6u;
        const uint64_t bit = 1ull << (idx & 63u);
        if (enable) {
            xma_enabled_mask_[word] |= bit;
        } else {
            xma_enabled_mask_[word] &= ~bit;
        }
    }

    bool IsXmaMaskEnabled(uint32_t idx) const noexcept {
        const uint32_t word = idx >> 6u;
        const uint64_t bit = 1ull << (idx & 63u);
        return (xma_enabled_mask_[word] & bit) != 0ull;
    }

    void ProcessRetailXmaContextLocked(uint32_t xmaIdx) {
        if (xmaIdx >= kMaxRetailXmaContexts) {
            return;
        }
        SrwLockGuard shape_lk(g_shape_alloc_mutex);
        if (!g_retail_xma_contexts_va || xmaIdx >= g_retail_xma_context_count) {
            return;
        }
        uint8_t* ctx = static_cast<uint8_t*>(g_retail_xma_contexts_va) +
                       static_cast<size_t>(xmaIdx) * 64u;
        if (!IsWritableAcpPtr(ctx, 64u)) {
            return;
        }

        uint32_t dw0 = 0u;
        uint32_t dw1 = 0u;
        uint32_t dw4 = 0u;
        uint32_t dw9 = 0u;
        uint32_t buf0Apu = 0u;
        uint32_t buf1Apu = 0u;
        uint32_t outRingApu = 0u;
        std::memcpy(&dw0, ctx + 0x00, 4u);
        std::memcpy(&dw1, ctx + 0x04, 4u);
        std::memcpy(&dw4, ctx + 0x10, 4u);
        std::memcpy(&buf0Apu, ctx + 0x14, 4u);
        std::memcpy(&buf1Apu, ctx + 0x18, 4u);
        std::memcpy(&outRingApu, ctx + 0x1C, 4u);
        std::memcpy(&dw9, ctx + 0x24, 4u);

        const uint32_t buf0Pkts = dw0 & 0xFFFu;
        bool buf0Valid = ((dw0 >> 20u) & 1u) != 0u;
        bool buf1Valid = ((dw0 >> 21u) & 1u) != 0u;
        const uint32_t rawOutBlocks = (dw0 >> 22u) & 0x1Fu;
        const uint32_t outRingBlocks = (rawOutBlocks == 0u) ? 32u : rawOutBlocks;
        uint32_t writeIdx = ((dw0 >> 27u) & 0x1Fu) % outRingBlocks;

        const uint32_t buf1Pkts = dw1 & 0xFFFu;
        const uint32_t rateCode = (dw1 >> 27u) & 3u;
        const uint32_t chMinus1 = (dw1 >> 29u) & 1u;
        const bool ctxEnabled = ((dw1 >> 31u) & 1u) != 0u;
        uint32_t curBuf = (dw4 >> 31u) & 1u;
        const uint32_t readIdx = (dw9 & 0x1Fu) % outRingBlocks;

        if (!ctxEnabled && !IsXmaMaskEnabled(xmaIdx)) {
            return;
        }
        if (outRingApu == 0u || outRingBlocks <= 1u) {
            return;
        }
        const uint32_t outRingBytes = outRingBlocks * 256u;
        void* outRingVa = ApuHeapManager::Instance().MapApuAddress(outRingApu);
        if (!outRingVa ||
            ApuHeapManager::Instance().ValidateApuAddressRange(outRingApu, outRingBytes) != ACPHAL_S_OK ||
            !IsWritableAcpPtr(outRingVa, outRingBytes)) {
            return;
        }

        uint32_t freeBlocks = (readIdx <= writeIdx)
                                  ? (outRingBlocks - 1u - (writeIdx - readIdx))
                                  : (readIdx - writeIdx - 1u);
        auto& state = xma_states_[xmaIdx];

        auto writePendingToRing = [&]() {
            while (freeBlocks > 0u && !state.pending_pcm.empty()) {
                int16_t block[128] = {};
                const size_t n = std::min<size_t>(128u, state.pending_pcm.size());
                for (size_t i = 0u; i < n; ++i) {
                    block[i] = state.pending_pcm.front();
                    state.pending_pcm.pop_front();
                }
                uint8_t* dst = static_cast<uint8_t*>(outRingVa) +
                               static_cast<size_t>(writeIdx) * 256u;
                std::memcpy(dst, block, 256u);
                writeIdx = (writeIdx + 1u) % outRingBlocks;
                --freeBlocks;
            }
        };

        writePendingToRing();

        static constexpr uint32_t kRates[4] = {24000u, 32000u, 44100u, 48000u};
        const uint32_t targetRate = kRates[rateCode];
        const uint16_t targetCh = static_cast<uint16_t>(chMinus1 + 1u);

        while (freeBlocks > 0u && state.pending_pcm.empty()) {
            uint32_t activeBuf = 0u;
            if (curBuf == 0u) {
                if (buf0Valid) {
                    activeBuf = 0u;
                } else if (buf1Valid) {
                    activeBuf = 1u;
                } else {
                    break;
                }
            } else {
                if (buf1Valid) {
                    activeBuf = 1u;
                } else if (buf0Valid) {
                    activeBuf = 0u;
                } else {
                    break;
                }
            }

            const uint32_t inApu = (activeBuf == 0u) ? buf0Apu : buf1Apu;
            const uint32_t inPkts = (activeBuf == 0u) ? buf0Pkts : buf1Pkts;
            if (inApu == 0u || inPkts == 0u) {
                break;
            }
            const uint64_t inBytes64 =
                static_cast<uint64_t>(inPkts) * openscarlett::kXma2PacketSize;
            if (inBytes64 > 0xFFFFFFFFull) {
                break;
            }
            const uint32_t inBytes = static_cast<uint32_t>(inBytes64);
            void* inVa = ApuHeapManager::Instance().MapApuAddress(inApu);
            if (!inVa ||
                ApuHeapManager::Instance().ValidateApuAddressRange(inApu, inBytes) != ACPHAL_S_OK ||
                !IsReadableAcpPtr(inVa, inBytes)) {
                break;
            }

            if (!state.decoder || state.sample_rate != targetRate || state.channels != targetCh) {
                state.decoder = std::make_unique<openscarlett::Xma2Decoder>();
                openscarlett::Xma2Config cfg{};
                cfg.sample_rate = targetRate;
                cfg.channels = targetCh;
                cfg.block_size = openscarlett::kXma2DefaultBlockSize;
                cfg.subframes_per_frame = 4u;
                cfg.quant_bits = 12u;
                if (!state.decoder->Initialize(cfg).ok()) {
                    state.decoder.reset();
                    break;
                }
                state.sample_rate = targetRate;
                state.channels = targetCh;
            }

            std::vector<int16_t> pcm;
            const auto st = state.decoder->DecodePacketsInt16(
                static_cast<const uint8_t*>(inVa), inBytes, pcm);
            if (st.ok() && !pcm.empty()) {
                state.pending_pcm.insert(state.pending_pcm.end(), pcm.begin(), pcm.end());
            }

            // Mark buffer consumed and advance curBuf.
            if (activeBuf == 0u) {
                buf0Valid = false;
                dw0 &= ~(1u << 20u);
            } else {
                buf1Valid = false;
                dw0 &= ~(1u << 21u);
            }
            curBuf = activeBuf ^ 1u;
            dw4 = (dw4 & ~(1u << 31u)) | (curBuf << 31u);

            writePendingToRing();
        }

        dw0 = (dw0 & ~(0x1Fu << 27u)) | ((writeIdx & 0x1Fu) << 27u);
        std::memcpy(ctx + 0x00, &dw0, sizeof(uint32_t));
        std::memcpy(ctx + 0x10, &dw4, sizeof(uint32_t));
    }

    uint64_t AdvanceAudioClockLocked() noexcept {
        uint64_t now_qpc = 0ull;
        uint64_t freq = qpc_freq_;
        QueryQpcAndFreq(now_qpc, freq);
        if (connect_qpc_ == 0ull && now_qpc != 0ull) {
            connect_qpc_ = now_qpc;
            qpc_freq_ = freq;
        }
        uint64_t target = 128ull;
        if (now_qpc >= connect_qpc_ && qpc_freq_ > 0ull) {
            const uint64_t delta = now_qpc - connect_qpc_;
            const uint64_t whole_sec = delta / qpc_freq_;
            const uint64_t rem_ticks = delta % qpc_freq_;
            const uint64_t elapsed_48k =
                whole_sec * 48000ull + (rem_ticks * 48000ull) / qpc_freq_;
            target = 128ull + elapsed_48k;
        }
        if (target > audio_clock_samples_) {
            audio_clock_samples_ = target;
        }
        return audio_clock_samples_;
    }

    SrwLock mutex_;
    bool connected_ = false;
    uint32_t last_config_ = 0u;
    uint32_t max_messages_ = 4096u;
    uint64_t connect_qpc_ = 0ull;
    uint64_t qpc_freq_ = 10000000ull;
    uint64_t audio_clock_samples_ = 128ull;
    std::array<uint64_t, 8> xma_enabled_mask_{};
    std::unordered_map<uint32_t, RetailXmaCtxState> xma_states_;
    std::deque<XboxAcpMessage> messages_;
};

}  // namespace

namespace {

#if defined(_WIN32)
void StartRetailLoopbackWavRecorderOnce() noexcept {
    static std::atomic<bool> s_started{false};
    if (s_started.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    char null_buf[16]{};
    if (::GetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", null_buf, sizeof(null_buf)) > 0 &&
        null_buf[0] != '0') {
        return;
    }
    wchar_t wav_buf[2048]{};
    const DWORD len = ::GetEnvironmentVariableW(
        L"OPENSCARLETT_RECORD_WAV", wav_buf, static_cast<DWORD>(std::size(wav_buf)));
    if (len == 0u || len >= std::size(wav_buf)) {
        return;
    }
    std::wstring raw_path(wav_buf, len);
    if (raw_path.size() >= 2u && raw_path.front() == L'"' && raw_path.back() == L'"') {
        raw_path = raw_path.substr(1u, raw_path.size() - 2u);
    }
    if (raw_path.empty()) {
        return;
    }
    std::thread([wav_path = std::filesystem::path(raw_path)]() {
        const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool need_uninit = SUCCEEDED(hr_co);

        // Give XAudio2_9 / SpatialAudioBridge a brief moment; if claimed or file already exists, skip.
        ::Sleep(250u);
        char claimed_buf[8]{};
        if (::GetEnvironmentVariableA("OPENSCARLETT_SPATIAL_WAV_CLAIMED", claimed_buf, sizeof(claimed_buf)) > 0 &&
            claimed_buf[0] == '1') {
            if (need_uninit) ::CoUninitialize();
            return;
        }
        std::error_code ec;
        if (std::filesystem::exists(wav_path, ec) && std::filesystem::file_size(wav_path, ec) > 44u) {
            if (need_uninit) ::CoUninitialize();
            return;
        }
        if (wav_path.has_parent_path()) {
            std::filesystem::create_directories(wav_path.parent_path(), ec);
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* endpoint = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* mix_fmt = nullptr;

        auto cleanup = [&]() {
            if (mix_fmt) ::CoTaskMemFree(mix_fmt);
            if (capture) capture->Release();
            if (client) {
                (void)client->Stop();
                client->Release();
            }
            if (endpoint) endpoint->Release();
            if (enumerator) enumerator->Release();
            if (need_uninit) ::CoUninitialize();
        };

        if (FAILED(::CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) ||
            !enumerator) {
            cleanup();
            return;
        }
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) || !endpoint) {
            cleanup();
            return;
        }
        if (FAILED(endpoint->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
            !client) {
            cleanup();
            return;
        }
        if (FAILED(client->GetMixFormat(&mix_fmt)) || !mix_fmt || mix_fmt->nSamplesPerSec == 0u ||
            mix_fmt->nChannels == 0u) {
            cleanup();
            return;
        }
        // 100 ms shared-mode loopback capture buffer
        if (FAILED(client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                1000000LL,
                0,
                mix_fmt,
                nullptr))) {
            cleanup();
            return;
        }
        if (FAILED(client->GetService(
                __uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
            !capture) {
            cleanup();
            return;
        }

        const uint32_t sr = mix_fmt->nSamplesPerSec;
        const uint32_t src_ch = mix_fmt->nChannels;
        const uint32_t bytes_per_frame = mix_fmt->nBlockAlign;
        bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix_fmt->wBitsPerSample == 32u);
        if (!is_float && mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22u) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
            if (ext->SubFormat.Data1 == 0x00000003u && mix_fmt->wBitsPerSample == 32u) {
                is_float = true;
            }
        }
        const bool is_pcm16 = (!is_float && mix_fmt->wBitsPerSample == 16u);
        if (!is_float && !is_pcm16) {
            cleanup();
            return;
        }

        FILE* fp = ::_wfsopen(wav_path.c_str(), L"wb", _SH_DENYNO);
        if (!fp) {
            cleanup();
            return;
        }

        auto write_wav_header = [&](uint64_t total_frames) {
            const uint64_t data_bytes64 = std::min<uint64_t>(total_frames * 4ull, 0xFFFFFF00ull);
            const uint32_t data_bytes = static_cast<uint32_t>(data_bytes64);
            const uint32_t riff_bytes = 36u + data_bytes;
            const uint32_t byte_rate = sr * 4u;
            const uint16_t block_align = 4u;
            const uint16_t bits = 16u;
            const uint16_t ch = 2u;
            const uint16_t pcm_tag = 1u;
            const uint32_t fmt_sz = 16u;
            uint8_t hdr[44]{};
            std::memcpy(hdr + 0, "RIFF", 4);
            std::memcpy(hdr + 4, &riff_bytes, 4);
            std::memcpy(hdr + 8, "WAVEfmt ", 8);
            std::memcpy(hdr + 16, &fmt_sz, 4);
            std::memcpy(hdr + 20, &pcm_tag, 2);
            std::memcpy(hdr + 22, &ch, 2);
            std::memcpy(hdr + 24, &sr, 4);
            std::memcpy(hdr + 28, &byte_rate, 4);
            std::memcpy(hdr + 32, &block_align, 2);
            std::memcpy(hdr + 34, &bits, 2);
            std::memcpy(hdr + 36, "data", 4);
            std::memcpy(hdr + 40, &data_bytes, 4);
            ::_fseeki64(fp, 0, SEEK_SET);
            (void)std::fwrite(hdr, 1u, sizeof(hdr), fp);
            ::_fseeki64(fp, 0, SEEK_END);
            std::fflush(fp);
        };

        write_wav_header(0ull);
        if (FAILED(client->Start())) {
            std::fclose(fp);
            cleanup();
            return;
        }

        const std::filesystem::path json_path(wav_path.wstring() + L".json");
        const uint32_t win_target = std::max<uint32_t>(1u, sr / 100u);  // 10 ms window
        uint32_t cur_win_frames = 0u;
        float cur_win_peak = 0.0f;
        bool seen_active = false;
        uint32_t silent_run = 0u;
        uint64_t active_10ms_windows = 0ull;
        uint64_t silent_holes_in_bursts = 0ull;
        uint64_t total_frames = 0ull;
        float peak_abs = 0.0f;
        double sum_sq = 0.0;
        std::vector<int16_t> scratch;
        uint64_t last_flush_ms = ::GetTickCount64();

        auto flush_all = [&]() {
            write_wav_header(total_frames);
            FILE* jfp = ::_wfsopen(json_path.c_str(), L"wb", _SH_DENYNO);
            if (jfp) {
                const double rms = (total_frames > 0ull)
                                       ? std::sqrt(sum_sq / static_cast<double>(total_frames * 2ull))
                                       : 0.0;
                char jbuf[640]{};
                const int n = std::snprintf(
                    jbuf, sizeof(jbuf),
                    "{\n"
                    "  \"source\": \"wasapi_loopback\",\n"
                    "  \"sample_rate\": %u,\n"
                    "  \"channels\": 2,\n"
                    "  \"total_frames\": %llu,\n"
                    "  \"duration_sec\": %.3f,\n"
                    "  \"rms\": %.6f,\n"
                    "  \"peak_abs\": %.6f,\n"
                    "  \"active_10ms_windows\": %llu,\n"
                    "  \"silent_holes_in_bursts\": %llu\n"
                    "}\n",
                    sr,
                    static_cast<unsigned long long>(total_frames),
                    static_cast<double>(total_frames) / static_cast<double>(sr),
                    rms,
                    static_cast<double>(peak_abs),
                    static_cast<unsigned long long>(active_10ms_windows),
                    static_cast<unsigned long long>(silent_holes_in_bursts));
                if (n > 0) {
                    (void)std::fwrite(jbuf, 1u, static_cast<size_t>(std::min<int>(n, sizeof(jbuf) - 1)), jfp);
                }
                std::fflush(jfp);
                std::fclose(jfp);
            }
        };

        for (;;) {
            ::Sleep(5u);
            char cl_buf[8]{};
            if (::GetEnvironmentVariableA("OPENSCARLETT_SPATIAL_WAV_CLAIMED", cl_buf, sizeof(cl_buf)) > 0 &&
                cl_buf[0] == '1') {
                std::fclose(fp);
                cleanup();
                return;
            }
            UINT32 pkt_frames = 0u;
            while (SUCCEEDED(capture->GetNextPacketSize(&pkt_frames)) && pkt_frames > 0u) {
                BYTE* data = nullptr;
                UINT32 num_frames = 0u;
                DWORD flags = 0u;
                if (FAILED(capture->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr))) {
                    break;
                }
                const bool silent_pkt = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0u || !data;
                scratch.resize(static_cast<size_t>(num_frames) * 2u);
                for (UINT32 f = 0u; f < num_frames; ++f) {
                    float l = 0.0f;
                    float r = 0.0f;
                    if (!silent_pkt) {
                        const BYTE* row = data + static_cast<size_t>(f) * bytes_per_frame;
                        if (is_float) {
                            const auto* flt = reinterpret_cast<const float*>(row);
                            l = flt[0];
                            r = (src_ch >= 2u) ? flt[1] : flt[0];
                        } else {
                            const auto* s16 = reinterpret_cast<const int16_t*>(row);
                            l = static_cast<float>(s16[0]) / 32768.0f;
                            r = static_cast<float>((src_ch >= 2u) ? s16[1] : s16[0]) / 32768.0f;
                        }
                    }
                    l = std::clamp(l, -1.0f, 1.0f);
                    r = std::clamp(r, -1.0f, 1.0f);
                    const float p = std::max(std::fabs(l), std::fabs(r));
                    if (p > peak_abs) peak_abs = p;
                    if (p > cur_win_peak) cur_win_peak = p;
                    sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
                    if (++cur_win_frames >= win_target) {
                        if (cur_win_peak > 1e-4f) {
                            if (seen_active && silent_run > 0u && silent_run <= 20u) {
                                silent_holes_in_bursts += silent_run;
                            }
                            seen_active = true;
                            silent_run = 0u;
                            ++active_10ms_windows;
                        } else if (seen_active) {
                            ++silent_run;
                        }
                        cur_win_frames = 0u;
                        cur_win_peak = 0.0f;
                    }
                    scratch[static_cast<size_t>(f) * 2u + 0u] =
                        static_cast<int16_t>(std::lround(l * 32767.0f));
                    scratch[static_cast<size_t>(f) * 2u + 1u] =
                        static_cast<int16_t>(std::lround(r * 32767.0f));
                }
                (void)capture->ReleaseBuffer(num_frames);
                const size_t wr = std::fwrite(scratch.data(), sizeof(int16_t) * 2u, num_frames, fp);
                total_frames += static_cast<uint64_t>(wr);
            }
            const uint64_t now_ms = ::GetTickCount64();
            if (now_ms - last_flush_ms >= 1000ull) {
                last_flush_ms = now_ms;
                flush_all();
            }
        }
    }).detach();
}
#endif

}  // namespace

extern "C" {

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalCreate(IAcpHal** ppAcpHal) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "AcpHalCreate(ppAcpHal=%p, retail=%d)",
            static_cast<const void*>(ppAcpHal),
            g_xbox_retail_acphal_mode.load(std::memory_order_acquire) ? 1 : 0);
    }
    if (!ppAcpHal) {
        return RecordHr("AcpHalCreate", ACPHAL_E_POINTER);
    }
    *ppAcpHal = nullptr;
    if (g_xbox_retail_acphal_mode.load(std::memory_order_acquire)) {
#if defined(_WIN32)
        StartRetailLoopbackWavRecorderOnce();
#endif
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
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "AcpHalAllocateShapeContexts(pReq=%p, pAlloc=%p)",
            static_cast<const void*>(pRequested), static_cast<void*>(pAllocated));
    }
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
#if defined(_WIN32)
        StartRetailLoopbackWavRecorderOnce();
#endif
        auto* xbox_ctx = reinterpret_cast<XboxAcpShapeContexts*>(
            const_cast<AcpHalShapeContexts*>(pRequested));

        // 6 SHAPE pipeline stages in retail XboxAcpShapeContexts.
        // Validate ALL stage byte counts in 64-bit FIRST before committing any allocations,
        // and atomically roll back stages 0..N-1 on partial OOM leaving zero leaked APU memory!
        struct StageSpec {
            uint32_t count;
            uint32_t elem_bytes;
            void** out_slot;
        };
        StageSpec stages[6] = {
            {xbox_ctx->numSrcContexts,     32u,  &xbox_ctx->srcContexts},
            {xbox_ctx->numEqCompContexts,  160u, &xbox_ctx->eqCompContexts},
            {xbox_ctx->numFiltVolContexts, 32u,  &xbox_ctx->filtVolContexts},
            {xbox_ctx->numDmaContexts,     16u,  &xbox_ctx->dmaContexts},
            {xbox_ctx->numXmaContexts,     64u,  &xbox_ctx->xmaContexts},
            {xbox_ctx->numPcmContexts,     32u,  &xbox_ctx->pcmContexts},
        };

        uint32_t stage_bytes[6] = {};
        for (size_t i = 0u; i < 6u; ++i) {
            const uint64_t total64 =
                static_cast<uint64_t>(stages[i].count) * static_cast<uint64_t>(stages[i].elem_bytes);
            if (total64 > kMaxShapeStageBytes || total64 > 0xFFFFFFFFull) {
                return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_OUTOFMEMORY);
            }
            stage_bytes[i] = static_cast<uint32_t>(total64);
        }

        SrwLockGuard lk(g_shape_alloc_mutex);
        void* allocated_ptrs[6] = {};
        APU_ADDRESS allocated_pas[6] = {};
        int32_t fail_hr = ACPHAL_S_OK;
        constexpr uint32_t kXboxRetailApuAllocCached = 1u;
        for (size_t i = 0u; i < 6u; ++i) {
            if (stage_bytes[i] == 0u) {
                continue;
            }
            void* va = nullptr;
            APU_ADDRESS pa = APU_INVALID_ADDRESS;
            const int32_t hr = ApuHeapManager::Instance().Alloc(
                &va, &pa, stage_bytes[i], 64u, kXboxRetailApuAllocCached);
            if (hr != ACPHAL_S_OK || !va) {
                fail_hr = (hr < 0) ? hr : ACPHAL_E_OUTOFMEMORY;
                // Atomic rollback of stages 0..(i-1) without clobbering caller slots.
                for (size_t r = i; r > 0u; --r) {
                    if (allocated_ptrs[r - 1u]) {
                        (void)ApuHeapManager::Instance().Free(allocated_ptrs[r - 1u]);
                        allocated_ptrs[r - 1u] = nullptr;
                    }
                }
                return RecordHr("AcpHalAllocateShapeContexts", fail_hr);
            }
            allocated_ptrs[i] = va;
            allocated_pas[i] = pa;
        }

        // All 6 stages succeeded — publish pointers/APU addresses and track for release.
        for (size_t i = 0u; i < 6u; ++i) {
            if (stage_bytes[i] == 0u || !allocated_ptrs[i]) {
                continue;
            }
            if (IsWritableAcpPtr(stages[i].out_slot, sizeof(void*))) {
                *stages[i].out_slot = allocated_ptrs[i];
            }
            if (IsWritableAcpPtr(&xbox_ctx->apuAddresses[i], sizeof(uint32_t))) {
                xbox_ctx->apuAddresses[i] = allocated_pas[i];
            }
            g_xbox_shape_buffers.push_back(allocated_ptrs[i]);
        }
        g_retail_xma_contexts_va = allocated_ptrs[4];
        g_retail_xma_context_count = allocated_ptrs[4] ? xbox_ctx->numXmaContexts : 0u;
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
    }

    // Synthetic 2-arg path: validate counts, allocate per-stage APU backing buffers with
    // atomic rollback on stage N failure, zero leaked APU allocations or dangling handles.
    g_xbox_retail_acphal_mode.store(false, std::memory_order_release);
    std::memset(pAllocated, 0, sizeof(*pAllocated));

    const uint32_t req_counts[4] = {
        pRequested->numXmaContexts,
        pRequested->numSrcContexts,
        pRequested->numEqContexts,
        pRequested->numDmaContexts,
    };
    const uint32_t stage_elem_bytes[4] = {64u, 32u, 32u, 16u};
    uint32_t stage_bytes[4] = {};
    for (size_t i = 0u; i < 4u; ++i) {
        const uint64_t total64 =
            static_cast<uint64_t>(req_counts[i]) * static_cast<uint64_t>(stage_elem_bytes[i]);
        if (total64 > kMaxShapeStageBytes || total64 > 0xFFFFFFFFull) {
            return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_INVALIDARG);
        }
        stage_bytes[i] = static_cast<uint32_t>(total64);
    }

    auto* alloc = new (std::nothrow) AcpShapeAllocation();
    if (!alloc) {
        return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_E_OUTOFMEMORY);
    }

    for (size_t i = 0u; i < 4u; ++i) {
        if (stage_bytes[i] == 0u) continue;
        void* va = nullptr;
        APU_ADDRESS pa = APU_INVALID_ADDRESS;
        const int32_t hr =
            ApuHeapManager::Instance().Alloc(&va, &pa, stage_bytes[i], 64u, APU_ALLOC_CACHED);
        if (hr != ACPHAL_S_OK || !va) {
            // Atomic rollback of stages 0..(i-1)!
            for (size_t r = i; r > 0u; --r) {
                if (alloc->stage_buffers[r - 1u]) {
                    (void)ApuHeapManager::Instance().Free(alloc->stage_buffers[r - 1u]);
                    alloc->stage_buffers[r - 1u] = nullptr;
                }
            }
            delete alloc;
            return RecordHr("AcpHalAllocateShapeContexts", (hr < 0) ? hr : ACPHAL_E_OUTOFMEMORY);
        }
        alloc->stage_buffers[i] = va;
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
        SrwLockGuard lk(g_shape_alloc_mutex);
        g_synthetic_shape_allocs.insert(alloc);
    }
    return RecordHr("AcpHalAllocateShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL AcpHalReleaseShapeContexts(AcpHalShapeContexts* pAllocated) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "AcpHalReleaseShapeContexts(pAlloc=%p)", static_cast<void*>(pAllocated));
    }
    {
        SrwLockGuard lk(g_shape_alloc_mutex);
        if (IsWritableAcpPtr(pAllocated, sizeof(AcpHalShapeContexts)) &&
            pAllocated->internalAllocation != nullptr &&
            g_synthetic_shape_allocs.erase(pAllocated->internalAllocation) > 0) {
            auto* alloc = static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
            for (void* buf : alloc->stage_buffers) {
                if (buf) {
                    (void)ApuHeapManager::Instance().Free(buf);
                }
            }
            delete alloc;
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
            g_retail_xma_contexts_va = nullptr;
            g_retail_xma_context_count = 0u;
            return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
        }
    }
    if (!pAllocated) {
        return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_E_INVALIDARG);
    }
    if (pAllocated->internalAllocation) {
        auto* alloc = static_cast<AcpShapeAllocation*>(pAllocated->internalAllocation);
        for (void* buf : alloc->stage_buffers) {
            if (buf) {
                (void)ApuHeapManager::Instance().Free(buf);
            }
        }
        delete alloc;
    }
    std::memset(pAllocated, 0, sizeof(*pAllocated));
    return RecordHr("AcpHalReleaseShapeContexts", ACPHAL_S_OK);
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuValidateAddressRange(
    APU_ADDRESS apuAddress,
    uint32_t sizeInBytes) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuValidateAddressRange(apu=0x%X, size=0x%X)", apuAddress, sizeInBytes);
    }
    return RecordHr(
        "ApuValidateAddressRange",
        ApuHeapManager::Instance().ValidateApuAddressRange(apuAddress, sizeInBytes));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuCheckAddressRangeDisjoint(
    APU_ADDRESS apuAddress,
    uint32_t sizeInBytes) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuCheckAddressRangeDisjoint(apu=0x%X, size=0x%X)", apuAddress, sizeInBytes);
    }
    return RecordHr(
        "ApuCheckAddressRangeDisjoint",
        ApuHeapManager::Instance().CheckApuRangeDisjoint(apuAddress, sizeInBytes));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuCreateHeap(
    uint32_t cachedSizeInBytes,
    uint32_t nonCachedSizeInBytes) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuCreateHeap(cached=0x%X, nonCached=0x%X)",
            cachedSizeInBytes, nonCachedSizeInBytes);
    }
    const int32_t hr = ApuHeapManager::Instance().CreateHeap(cachedSizeInBytes, nonCachedSizeInBytes);
    if (hr < 0) {
        std::fprintf(stderr, "[OpenScarlett][AcpHal] ApuCreateHeap(cached=0x%X, nonCached=0x%X) -> 0x%08X\n",
                     cachedSizeInBytes, nonCachedSizeInBytes, static_cast<unsigned>(hr));
        std::fflush(stderr);
    }
    return RecordHr("ApuCreateHeap", hr);
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuAlloc(
    void** virtualAddress,
    APU_ADDRESS* physicalAddress,
    uint32_t sizeInBytes,
    uint32_t alignmentInBytes,
    uint32_t flags) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuAlloc(size=0x%X, align=0x%X, flags=0x%X)",
            sizeInBytes, alignmentInBytes, flags);
    }
    return RecordHr(
        "ApuAlloc",
        ApuHeapManager::Instance().Alloc(
            virtualAddress, physicalAddress, sizeInBytes, alignmentInBytes, flags));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuFree(void* virtualAddress) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuFree(va=%p)", virtualAddress);
    }
    return RecordHr("ApuFree", ApuHeapManager::Instance().Free(virtualAddress));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuHeapGetState(
    APU_HEAP_STATE* state,
    uint32_t flags) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuHeapGetState(flags=0x%X)", flags);
    }
    return RecordHr("ApuHeapGetState", ApuHeapManager::Instance().GetState(state, flags));
}

int32_t OPENSCARLETT_ACPHAL_CALL ApuIsVirtualAddressValid(
    const void* virtualAddress,
    uint32_t sizeInBytes) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuIsVirtualAddressValid(va=%p, size=0x%X)", virtualAddress, sizeInBytes);
    }
    const int32_t valid =
        ApuHeapManager::Instance().IsVirtualAddressValid(virtualAddress, sizeInBytes);
    RecordOk("ApuIsVirtualAddressValid");
    return valid;
}

APU_ADDRESS OPENSCARLETT_ACPHAL_CALL ApuMapVirtualAddress(const void* virtualAddress) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuMapVirtualAddress(va=%p)", virtualAddress);
    }
    const APU_ADDRESS addr = ApuHeapManager::Instance().MapVirtualAddress(virtualAddress);
    RecordOk("ApuMapVirtualAddress");
    return addr;
}

void* OPENSCARLETT_ACPHAL_CALL ApuMapApuAddress(APU_ADDRESS apuAddress) {
    static std::atomic<uint64_t> trace_cnt{0u};
    if (ShouldTraceCall(trace_cnt)) {
        openscarlett::audio::TraceRetailAudio(
            "AcpHal", OPENSCARLETT_CALLER_ADDR(),
            "ApuMapApuAddress(apu=0x%X)", apuAddress);
    }
    void* va = ApuHeapManager::Instance().MapApuAddress(apuAddress);
    RecordOk("ApuMapApuAddress");
    return va;
}

}  // extern "C"
