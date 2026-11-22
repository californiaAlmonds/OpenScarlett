#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <windows.h>
#endif

namespace openscarlett::d3d12_xs {

enum class QueryKind : uint32_t {
    Occlusion = 0u,
    BinaryOcclusion = 1u,
    Timestamp = 2u,
    PipelineStatistics = 3u,
};

enum class QueryAccumulationOp : uint32_t {
    Overwrite = 0u,
    Add = 1u,
    Max = 2u,
};

struct QueryPipelineStatsResult {
    uint64_t ia_vertices = 0ull;
    uint64_t ia_primitives = 0ull;
    uint64_t vs_invocations = 0ull;
    uint64_t gs_invocations = 0ull;
    uint64_t gs_primitives = 0ull;
    uint64_t c_invocations = 0ull;
    uint64_t c_primitives = 0ull;
    uint64_t ps_invocations = 0ull;
    uint64_t hs_invocations = 0ull;
    uint64_t ds_invocations = 0ull;
    uint64_t cs_invocations = 0ull;

    void AccumulateAdd(const QueryPipelineStatsResult& rhs) noexcept {
        ia_vertices += rhs.ia_vertices;
        ia_primitives += rhs.ia_primitives;
        vs_invocations += rhs.vs_invocations;
        gs_invocations += rhs.gs_invocations;
        gs_primitives += rhs.gs_primitives;
        c_invocations += rhs.c_invocations;
        c_primitives += rhs.c_primitives;
        ps_invocations += rhs.ps_invocations;
        hs_invocations += rhs.hs_invocations;
        ds_invocations += rhs.ds_invocations;
        cs_invocations += rhs.cs_invocations;
    }
};

struct QueryRangeSpec {
    uint32_t start_index = 0u;
    uint32_t count = 1u;
};

struct QueryAccumulatorConfig {
    uint32_t occlusion_capacity = 256u;
    uint32_t timestamp_capacity = 256u;
    uint32_t pipeline_stats_capacity = 64u;
};

struct QueryAccumulatorStats {
    uint32_t occlusion_capacity = 0u;
    uint32_t timestamp_capacity = 0u;
    uint32_t pipeline_stats_capacity = 0u;
    uint64_t begin_count = 0ull;
    uint64_t end_count = 0ull;
    uint64_t resolve_count = 0ull;
    uint64_t gpu_accumulate_dispatches = 0ull;
    uint64_t active_frame_index = 0ull;
};

// Console Query Accumulator managing OCCLUSION, BINARY_OCCLUSION, TIMESTAMP, and
// PIPELINE_STATISTICS query heaps across multiple command lists and frames.
// Supports native D3D12 ResolveQueryData readback AND GPU compute / CPU multi-range
// accumulation into destination GPU buffers with optional D3D12_PREDICATION_OP support.
class QueryAccumulatorX {
public:
    QueryAccumulatorX();
    ~QueryAccumulatorX();

    QueryAccumulatorX(const QueryAccumulatorX&) = delete;
    QueryAccumulatorX& operator=(const QueryAccumulatorX&) = delete;

#if defined(_WIN32)
    Status Initialize(
        ID3D12Device* host_device,
        const QueryAccumulatorConfig& config = {});
#endif
    void Shutdown();
    bool IsInitialized() const noexcept;
    QueryAccumulatorStats GetStats() const;

    void BeginFrame(uint64_t frame_index);
    uint64_t CurrentFrame() const noexcept;

#if defined(_WIN32)
    ID3D12QueryHeap* GetQueryHeap(QueryKind kind) const noexcept;
    static D3D12_QUERY_HEAP_TYPE ToD3D12HeapType(QueryKind kind) noexcept;
    static D3D12_QUERY_TYPE ToD3D12QueryType(QueryKind kind) noexcept;
    static uint32_t QueryElementByteSize(QueryKind kind) noexcept;

    // Records BeginQuery (no-op for Timestamp, which only uses EndQuery in D3D12).
    Status BeginQuery(
        ID3D12GraphicsCommandList* cmd_list,
        QueryKind kind,
        uint32_t index);

    // Records EndQuery (and records GPU timestamp for QueryKind::Timestamp).
    Status EndQuery(
        ID3D12GraphicsCommandList* cmd_list,
        QueryKind kind,
        uint32_t index);

    // Records native D3D12 ResolveQueryData for a contiguous query range.
    Status ResolveQueryData(
        ID3D12GraphicsCommandList* cmd_list,
        QueryKind kind,
        uint32_t start_index,
        uint32_t num_queries,
        ID3D12Resource* dst_buffer,
        uint64_t aligned_dst_offset);

    // Resolves multiple query ranges (potentially recorded across multiple command lists)
    // and accumulates the 64-bit results on the GPU via HLSL compute into dst_buffer
    // (uint64_t element at dst_offset_bytes). Optionally predicates the accumulation using
    // predicate_buffer (64-bit value at predicate_offset_bytes) and predicate_op.
    Status ResolveAndAccumulateU64Gpu(
        ID3D12GraphicsCommandList* cmd_list,
        QueryKind kind,
        const std::vector<QueryRangeSpec>& ranges,
        ID3D12Resource* dst_buffer,
        uint64_t dst_offset_bytes,
        QueryAccumulationOp op = QueryAccumulationOp::Add,
        ID3D12Resource* predicate_buffer = nullptr,
        uint64_t predicate_offset_bytes = 0ull,
        D3D12_PREDICATION_OP predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO,
        D3D12_RESOURCE_STATES dst_state_before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATES dst_state_after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Synchronous readback of resolved uint64 queries (Occlusion / BinaryOcclusion / Timestamp)
    // directly from the managed query heap on queue.
    Status ReadbackU64Range(
        ID3D12CommandQueue* queue,
        QueryKind kind,
        uint32_t start_index,
        uint32_t num_queries,
        std::vector<uint64_t>& out_values);

    // Synchronous multi-range CPU accumulation across multiple ranges (matching Xbox multi-CL resolve).
    Status ReadbackAndAccumulateU64(
        ID3D12CommandQueue* queue,
        QueryKind kind,
        const std::vector<QueryRangeSpec>& ranges,
        uint64_t& out_accumulated_value,
        std::vector<uint64_t>* out_individual_values = nullptr);

    // Synchronous readback + accumulation of PIPELINE_STATISTICS queries.
    Status ReadbackPipelineStatistics(
        ID3D12CommandQueue* queue,
        const std::vector<QueryRangeSpec>& ranges,
        QueryPipelineStatsResult& out_accumulated,
        std::vector<QueryPipelineStatsResult>* out_individual = nullptr);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Predicated Byte-Granular GPU Buffer Copy (Leonida #1277 / #1278)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
struct PredicatedByteCopyDesc {
    ID3D12Resource* dst_buffer = nullptr;
    uint64_t dst_offset_bytes = 0ull;
    ID3D12Resource* src_buffer = nullptr;
    uint64_t src_offset_bytes = 0ull;
    uint64_t num_bytes = 0ull;

    // 64-bit predicate buffer (aligned to 8 bytes per D3D12 SetPredication rules).
    // When predicate_buffer == nullptr, copy executes unconditionally.
    ID3D12Resource* predicate_buffer = nullptr;
    uint64_t predicate_offset_bytes = 0ull;
    // D3D12_PREDICATION_OP_EQUAL_ZERO: skip copy if *predicate == 0 (execute when != 0).
    // D3D12_PREDICATION_OP_NOT_EQUAL_ZERO: skip copy if *predicate != 0 (execute when == 0).
    D3D12_PREDICATION_OP predicate_op = D3D12_PREDICATION_OP_EQUAL_ZERO;

    D3D12_RESOURCE_STATES dst_state_before = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES dst_state_after = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES src_state_before = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_STATES src_state_after = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_STATES predicate_state_before = D3D12_RESOURCE_STATE_PREDICATION;
    D3D12_RESOURCE_STATES predicate_state_after = D3D12_RESOURCE_STATE_PREDICATION;
};

struct PredicatedByteCopyMetrics {
    bool used_native_predication = false;
    bool sub_4byte_unaligned = false;
    uint64_t dst_offset_bytes = 0ull;
    uint64_t src_offset_bytes = 0ull;
    uint64_t num_bytes = 0ull;
};

// Records a byte-granular CopyBufferRegion predicated by a 64-bit GPU predicate buffer value.
// Handles native ID3D12GraphicsCommandList::SetPredication, automatic resource state transitions,
// exact sub-4-byte offsets & lengths, and clears predication state after the copy.
Status PredicatedByteBufferCopy(
    ID3D12GraphicsCommandList* cmd_list,
    const PredicatedByteCopyDesc& desc,
    PredicatedByteCopyMetrics* out_metrics = nullptr);
#endif

// ---------------------------------------------------------------------------
// GPU-to-Win32 QPC Clock Calibration (D3D12 + Vulkan VK_EXT_calibrated_timestamps)
// ---------------------------------------------------------------------------
struct GpuClockCalibrationSample {
    uint64_t gpu_timestamp_ticks = 0ull;
    uint64_t cpu_qpc_ticks = 0ull;
    uint64_t qpc_before_ticks = 0ull;
    uint64_t qpc_after_ticks = 0ull;
    uint64_t max_deviation_ns = 0ull;  // From Vulkan pMaxDeviation or QPC bracket width
    double gpu_time_seconds = 0.0;
    double cpu_time_seconds = 0.0;
    double offset_seconds = 0.0;       // cpu_time_seconds - gpu_time_seconds
};

struct GpuClockCalibrationResult {
    std::string backend_name;          // "D3D12" or "Vulkan"
    std::string device_name;
    bool calibrated_with_vulkan_ext = false;
    bool gpu_timestamp_frequency_valid = false;
    uint64_t gpu_timestamp_frequency_hz = 0ull;
    uint64_t cpu_qpc_frequency_hz = 0ull;
    double gpu_tick_period_ns = 0.0;
    double cpu_qpc_period_ns = 0.0;

    // Best (lowest-bracket-error) anchor sample and linear drift estimate across samples.
    GpuClockCalibrationSample anchor{};
    std::vector<GpuClockCalibrationSample> samples;
    double estimated_offset_seconds = 0.0;
    double max_bracket_error_us = 0.0;
    double min_bracket_error_us = 0.0;
    double max_offset_spread_us = 0.0;
    double estimated_drift_ppm = 0.0;

    // Converts a raw GPU timestamp counter value into estimated Win32 QPC ticks.
    uint64_t GpuTicksToEstimatedQpc(uint64_t gpu_ticks) const noexcept;
    // Converts a GPU tick delta into microseconds.
    double GpuDeltaTicksToMicroseconds(uint64_t delta_gpu_ticks) const noexcept;
};

#if defined(_WIN32)
// Calibrates a D3D12 command queue's GPU timestamp clock against Win32 QueryPerformanceCounter
// using GetTimestampFrequency + GetClockCalibration across num_samples bracketed pairs.
Status CalibrateGpuClockWithQpc(
    ID3D12CommandQueue* queue,
    GpuClockCalibrationResult& out_result,
    uint32_t num_samples = 5u,
    uint32_t inter_sample_delay_ms = 2u);
#endif

// Probes dynamically loaded vulkan-1.dll for VK_EXT_calibrated_timestamps (or
// VK_KHR_calibrated_timestamps) with VK_TIME_DOMAIN_DEVICE_EXT +
// VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT, calibrating the Vulkan GPU clock
// against Win32 QPC with zero Vulkan SDK header dependencies.
Status CalibrateVulkanGpuClockWithQpc(
    GpuClockCalibrationResult& out_result,
    uint32_t num_samples = 5u,
    uint32_t inter_sample_delay_ms = 2u);

}  // namespace openscarlett::d3d12_xs
