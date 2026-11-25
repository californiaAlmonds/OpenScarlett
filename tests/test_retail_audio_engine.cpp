#include "openscarlett/audio/acphal_abi.h"
#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xapu_abi.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "os_check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265358979323846f;

fs::path GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0u && n < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
    return fs::current_path();
}

template <typename Fn>
Fn LoadExport(HMODULE mod, const char* name) {
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(mod, name)));
    OS_REQUIRE(fn != nullptr);
    return fn;
}

// ===========================================================================
// 1. xapu.dll Retail Engine Tests
// ===========================================================================
#pragma pack(push, 1)
struct XApuRetailStartCommand {
    XApuRetailCommandHeader header;
    uint32_t channels;
    uint32_t reserved[3];
    float startPitch;
};
static_assert(sizeof(XApuRetailStartCommand) == 0x2C, "XApuRetailStartCommand must be 44 bytes");
#pragma pack(pop)

void TestXApuRetailEngine() {
    std::printf("[1/5] Testing xapu.dll retail engine (DecodeOpus, bad packet, ConvertPCM, Stop/Reset/Util)...\n");

    const fs::path dll_path = GetExeDir() / L"xapu.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnConnect3 = int32_t(__stdcall*)(void*, void*, void*);
    using PfnEnqueue = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE, const void*, uint32_t);
    using PfnDequeue = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE, void*, uint32_t, uint32_t*);
    using PfnGetUtil = int32_t(__stdcall*)(uint32_t, uint32_t, uint32_t*);
    using PfnDisconnect = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE);

    auto p_connect = LoadExport<PfnConnect3>(mod, "XApuConnect");
    auto p_enqueue = LoadExport<PfnEnqueue>(mod, "XApuEnqueueCommand");
    auto p_dequeue = LoadExport<PfnDequeue>(mod, "XApuDequeueResult");
    auto p_get_util = LoadExport<PfnGetUtil>(mod, "XApuGetUtilization");
    auto p_disconnect = LoadExport<PfnDisconnect>(mod, "XApuDisconnect");

    // 1A. 3-arg XApuConnect(DecodeOpus, EnableResultReadyEvent) + Start + Data + Bad Packet + Utilization + Stop/Reset
    {
        uint32_t util[2]{};
        OS_REQUIRE(p_get_util(1u, 1u, util) == XAPU_S_OK);
        OS_REQUIRE(util[0] == 500u && util[1] == 500u);

        XApuConnectInput in{};
        in.processType = XApuProcessType_DecodeOpus;
        in.streamCount = 4u;
        in.commandQueueDepth = 64u;
        in.sharedMemoryBytes = 0x20000u;
        in.maxFrameCountPerOutput = 1024u;
        in.options = XApuConnectOption_EnableResultReadyEvent;

        XApuConnectOutput out{};
        XAPU_CONNECTION_HANDLE conn = nullptr;
        OS_REQUIRE(p_connect(&in, &out, &conn) == XAPU_S_OK);
        OS_REQUIRE(conn != nullptr);
        OS_REQUIRE(out.baseAddress != nullptr);
        OS_REQUIRE((reinterpret_cast<uintptr_t>(out.baseAddress) % 0x10000u) == 0u);

        HANDLE ready_event = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(out.sizeOrReadyEvent));
        OS_REQUIRE(ready_event != nullptr);

        // Start stream 0 (2ch)
        XApuRetailStartCommand start_cmd{};
        start_cmd.header.type = XApuRetailCommand_Start;
        start_cmd.header.streamIndex = 0u;
        start_cmd.header.sequence = 1u;
        start_cmd.header.context = 0x1111222233334444ull;
        start_cmd.channels = 2u;
        start_cmd.startPitch = 1.0f;

        OS_REQUIRE(p_enqueue(conn, &start_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(::WaitForSingleObject(ready_event, 1000u) == WAIT_OBJECT_0);

        XApuRetailResult res{};
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.header.type == XApuRetailCommand_Start);
        OS_REQUIRE(res.header.context == 0x1111222233334444ull);

        // Active stream count tracked out of 500
        OS_REQUIRE(p_get_util(1u, 1u, util) == XAPU_S_OK);
        OS_REQUIRE(util[0] == 499u && util[1] == 500u);

        // Place valid Opus packet and output buffer inside 64 KiB shared memory
        auto* shm = static_cast<uint8_t*>(out.baseAddress);
        const std::vector<uint8_t> opus_pkt = openscarlett::BuildOpusTestPacket(2u, 0u, 64u);
        uint8_t* pkt_in = shm + 0x1000u;
        auto* pcm_out = reinterpret_cast<float*>(shm + 0x4000u);
        constexpr uint32_t kMaxOutBytes = 4096u * 2u * sizeof(float);
        std::memcpy(pkt_in, opus_pkt.data(), opus_pkt.size());
        std::memset(pcm_out, 0, kMaxOutBytes);

        XApuRetailDataCommand data_cmd{};
        data_cmd.header.type = XApuRetailCommand_Data;
        data_cmd.header.streamIndex = 0u;
        data_cmd.header.sequence = 2u;
        data_cmd.header.options = 2u;  // flush/drain
        data_cmd.header.context = 0xCAFEBABE12345678ull;
        data_cmd.firstFrame = 0u;
        data_cmd.frameCount = 0xFFFFFFFFu;
        data_cmd.inputDataLength = static_cast<uint32_t>(opus_pkt.size());
        data_cmd.inputDataPtr = reinterpret_cast<uint64_t>(pkt_in);
        data_cmd.maxOutputDataLength = kMaxOutBytes;
        data_cmd.outputDataPtr = reinterpret_cast<uint64_t>(pcm_out);

        OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(::WaitForSingleObject(ready_event, 1000u) == WAIT_OBJECT_0);

        std::memset(&res, 0, sizeof(res));
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.header.type == XApuRetailCommand_Data);
        OS_REQUIRE(res.header.streamIndex == 0u);
        OS_REQUIRE(res.header.sequence == 2u);
        OS_REQUIRE(res.header.context == 0xCAFEBABE12345678ull);
        OS_REQUIRE(res.outputFrameCount > 0u);
        OS_REQUIRE(res.outputDataLength == res.outputFrameCount * 2u * sizeof(float));
        OS_REQUIRE(res.outputDataPtr == reinterpret_cast<uint64_t>(pcm_out));

        float max_abs = 0.0f;
        for (uint32_t i = 0u; i < res.outputFrameCount * 2u; ++i) {
            OS_REQUIRE(std::isfinite(pcm_out[i]));
            max_abs = (std::max)(max_abs, std::fabs(pcm_out[i]));
        }
        OS_REQUIRE(max_abs > 1e-5f);

        // Bad Opus packet -> XAPU_E_BAD_OPUS_PACKET (0x8AC81008)
        const uint8_t bad_opus[2] = {0x03u, 0x00u};  // frame_code=3 with frame_count=0 is invalid
        std::memcpy(pkt_in, bad_opus, sizeof(bad_opus));
        data_cmd.header.sequence = 3u;
        data_cmd.header.options = 0u;
        data_cmd.header.context = 0xBAD0000000000001ull;
        data_cmd.inputDataLength = sizeof(bad_opus);

        OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(::WaitForSingleObject(ready_event, 1000u) == WAIT_OBJECT_0);

        std::memset(&res, 0, sizeof(res));
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_E_BAD_OPUS_PACKET);
        OS_REQUIRE(res.header.context == 0xBAD0000000000001ull);

        // Reset (type=5, options=4)
        XApuRetailCommandHeader reset_cmd{};
        reset_cmd.type = XApuRetailCommand_Reset;
        reset_cmd.streamIndex = 0u;
        reset_cmd.sequence = 4u;
        reset_cmd.options = 4u;
        reset_cmd.context = 0x5555666677778888ull;

        OS_REQUIRE(p_enqueue(conn, &reset_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(::WaitForSingleObject(ready_event, 1000u) == WAIT_OBJECT_0);
        std::memset(&res, 0, sizeof(res));
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.header.type == XApuRetailCommand_Reset);

        // Stop (type=2) deferred deactivation on dequeue
        XApuRetailCommandHeader stop_cmd{};
        stop_cmd.type = XApuRetailCommand_Stop;
        stop_cmd.streamIndex = 0u;
        stop_cmd.sequence = 5u;
        stop_cmd.context = 0x9999AAAABBBBCCCCull;

        OS_REQUIRE(p_enqueue(conn, &stop_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(::WaitForSingleObject(ready_event, 1000u) == WAIT_OBJECT_0);

        // Before dequeue: stream is still active in utilization
        OS_REQUIRE(p_get_util(1u, 1u, util) == XAPU_S_OK);
        OS_REQUIRE(util[0] == 499u);

        std::memset(&res, 0, sizeof(res));
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.header.type == XApuRetailCommand_Stop);

        // After dequeue: stream deactivated, free returns to 500
        OS_REQUIRE(p_get_util(1u, 1u, util) == XAPU_S_OK);
        OS_REQUIRE(util[0] == 500u && util[1] == 500u);

        OS_REQUIRE(p_disconnect(conn) == XAPU_S_OK);
    }

    // 1B. ConvertPCM (processType=3) with DeinterleavedInput (2) | DeinterleavedOutput (4)
    {
        constexpr uint32_t kMaxOutFrames = 256u;
        XApuConnectInput in{};
        in.processType = XApuProcessType_ConvertPCM;
        in.streamCount = 2u;
        in.commandQueueDepth = 32u;
        in.sharedMemoryBytes = 0x10000u;
        in.maxFrameCountPerOutput = kMaxOutFrames;
        in.options = XApuConnectOption_DeinterleavedInput | XApuConnectOption_DeinterleavedOutput;

        XApuConnectOutput out{};
        XAPU_CONNECTION_HANDLE conn = nullptr;
        OS_REQUIRE(p_connect(&in, &out, &conn) == XAPU_S_OK);

        XApuRetailStartCommand start_cmd{};
        start_cmd.header.type = XApuRetailCommand_Start;
        start_cmd.header.streamIndex = 0u;
        start_cmd.header.sequence = 1u;
        start_cmd.channels = 2u;
        start_cmd.startPitch = 1.0f;
        OS_REQUIRE(p_enqueue(conn, &start_cmd, 0u) == XAPU_S_OK);

        XApuRetailResult res{};
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);

        // Planar stereo input with >1.0f amplitude to verify saturation flag (state & 8)
        constexpr uint32_t kInFrames = 128u;
        std::vector<float> planar_in(kInFrames * 2u, 0.0f);
        for (uint32_t i = 0u; i < kInFrames; ++i) {
            planar_in[i] = 1.5f * std::sin(2.0f * kPi * 3.0f * static_cast<float>(i) / 128.0f);
            planar_in[kInFrames + i] = 1.25f * std::cos(2.0f * kPi * 5.0f * static_cast<float>(i) / 128.0f);
        }
        std::vector<float> planar_out(kMaxOutFrames * 2u, 0.0f);

        XApuRetailDataCommand data_cmd{};
        data_cmd.header.type = XApuRetailCommand_Data;
        data_cmd.header.streamIndex = 0u;
        data_cmd.header.sequence = 2u;
        data_cmd.header.options = 2u;  // flush_end so last fractional sample emits cleanly
        data_cmd.header.context = 0x00000000DEADBEEFull;
        data_cmd.pitchRamp = 0.02f;
        data_cmd.targetPitch = 2.0f;
        data_cmd.firstFrame = 0u;
        data_cmd.frameCount = 0xFFFFFFFFu;
        data_cmd.inputDataLength = static_cast<uint32_t>(planar_in.size() * sizeof(float));
        data_cmd.inputDataPtr = reinterpret_cast<uint64_t>(planar_in.data());
        data_cmd.maxOutputDataLength = static_cast<uint32_t>(planar_out.size() * sizeof(float));
        data_cmd.outputDataPtr = reinterpret_cast<uint64_t>(planar_out.data());

        OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.outputFrameCount > 0u && res.outputFrameCount < kInFrames);
        OS_REQUIRE(res.outputDataLength == res.outputFrameCount * 2u * sizeof(float));
        OS_REQUIRE(res.currentPitch > 1.0f && res.currentPitch <= 2.0f);
        OS_REQUIRE((res.state & 8u) != 0u);  // saturation flag

        float ch0_max = 0.0f;
        float ch1_max = 0.0f;
        for (uint32_t i = 0u; i < res.outputFrameCount; ++i) {
            const float s0 = planar_out[i];
            const float s1 = planar_out[kMaxOutFrames + i];
            OS_REQUIRE(std::isfinite(s0) && std::isfinite(s1));
            ch0_max = (std::max)(ch0_max, std::fabs(s0));
            ch1_max = (std::max)(ch1_max, std::fabs(s1));
        }
        OS_REQUIRE(ch0_max > 0.5f && ch1_max > 0.5f);

        OS_REQUIRE(p_disconnect(conn) == XAPU_S_OK);
    }

    // 1C. Retail GTA VI XApu sequence: proc=1, shm=0x104000, opt=1, zeroed Start pitch=0.0f,
    //     1-arg XApuGetResultCount(conn, nullptr) returning pending count in EAX, and mono
    //     Opus decode with firstFrame=312 pre-skip & frameCount=0x3C0 (960).
    {
        using PfnGetCount = int32_t(__stdcall*)(XAPU_CONNECTION_HANDLE, uint32_t*);
        auto p_get_count = LoadExport<PfnGetCount>(mod, "XApuGetResultCount");

        XApuConnectInput in{};
        in.processType = XApuProcessType_DecodeOpus;
        in.streamCount = 256u;
        in.commandQueueDepth = 2u;
        in.sharedMemoryBytes = 0x104000u;
        in.maxFrameCountPerOutput = 0u;
        in.options = 1u;

        XApuConnectOutput out{};
        XAPU_CONNECTION_HANDLE conn = nullptr;
        OS_REQUIRE(p_connect(&in, &out, &conn) == XAPU_S_OK);
        OS_REQUIRE(conn != nullptr && out.baseAddress != nullptr);
        OS_REQUIRE(p_get_count(conn, nullptr) == 0);

        XApuRetailStartCommand start_cmd{};
        start_cmd.header.type = XApuRetailCommand_Start;
        start_cmd.header.streamIndex = 3u;
        start_cmd.channels = 1u;
        start_cmd.startPitch = 0.0f;  // Zeroed struct must default to 1.0f, NOT 0.125f!
        OS_REQUIRE(p_enqueue(conn, &start_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(p_get_count(conn, nullptr) == 1);

        XApuRetailResult res{};
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.currentPitch == 1.0f);
        OS_REQUIRE(p_get_count(conn, nullptr) == 0);

        const auto pkt = openscarlett::BuildOpusTestPacket(1u, 11u, 64u);
        std::vector<float> out_pcm(960u, 0.0f);
        XApuRetailDataCommand data_cmd{};
        data_cmd.header.type = XApuRetailCommand_Data;
        data_cmd.header.streamIndex = 3u;
        data_cmd.pitchRamp = 0.0f;
        data_cmd.targetPitch = 0.0f;
        data_cmd.firstFrame = 312u;
        data_cmd.frameCount = 0x3C0u;
        data_cmd.inputDataLength = static_cast<uint32_t>(pkt.size());
        data_cmd.inputDataPtr = reinterpret_cast<uint64_t>(pkt.data());
        data_cmd.maxOutputDataLength = static_cast<uint32_t>(out_pcm.size() * sizeof(float));
        data_cmd.outputDataPtr = reinterpret_cast<uint64_t>(out_pcm.data());

        OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(p_get_count(conn, nullptr) == 1);
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.currentPitch == 1.0f);
        OS_REQUIRE(res.outputFrameCount == 960u - 312u);
        OS_REQUIRE(res.outputDataLength == (960u - 312u) * sizeof(float));

        float peak = 0.0f;
        for (uint32_t i = 0u; i < res.outputFrameCount; ++i) {
            OS_REQUIRE(std::isfinite(out_pcm[i]));
            peak = (std::max)(peak, std::fabs(out_pcm[i]));
        }
        OS_REQUIRE(peak > 0.001f);

        // 1D. Verify 100 consecutive empty XApuDequeueResult polls complete in < 5 ms total
        //     (proving the 15.625 ms Windows Sleep(1) mixer thread stall is completely gone).
        LARGE_INTEGER qpc_freq{}, qpc_t0{}, qpc_t1{};
        OS_REQUIRE(::QueryPerformanceFrequency(&qpc_freq) != FALSE && qpc_freq.QuadPart > 0);
        OS_REQUIRE(::QueryPerformanceCounter(&qpc_t0) != FALSE);
        for (uint32_t poll = 0u; poll < 100u; ++poll) {
            OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_E_QUEUE_EMPTY);
        }
        OS_REQUIRE(::QueryPerformanceCounter(&qpc_t1) != FALSE);
        const double empty_poll_ms =
            static_cast<double>(qpc_t1.QuadPart - qpc_t0.QuadPart) * 1000.0 /
            static_cast<double>(qpc_freq.QuadPart);
        std::printf("  XApuDequeueResult 100 consecutive empty polls: %.3f ms (< 5.0 ms)\n", empty_poll_ms);
        OS_REQUIRE(empty_poll_ms < 5.0);

        // 1E. Verify multi-packet Opus continuity & post-flush_end recovery on the same stream
        for (uint32_t pkt_idx = 0u; pkt_idx < 3u; ++pkt_idx) {
            const auto next_pkt = openscarlett::BuildOpusTestPacket(1u, 20u + pkt_idx, 64u);
            std::fill(out_pcm.begin(), out_pcm.end(), 0.0f);
            data_cmd.header.sequence = 10u + pkt_idx;
            data_cmd.header.options = (pkt_idx == 1u) ? 2u : 0u;  // drain on pkt 1, then continue on pkt 2!
            data_cmd.firstFrame = 0u;
            data_cmd.frameCount = 0x3C0u;
            data_cmd.inputDataLength = static_cast<uint32_t>(next_pkt.size());
            data_cmd.inputDataPtr = reinterpret_cast<uint64_t>(next_pkt.data());
            OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
            OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
            OS_REQUIRE(res.hresult == XAPU_S_OK);
            OS_REQUIRE(res.outputFrameCount == 960u);
            OS_REQUIRE((res.state & 2u) != 0u && (res.state & 4u) == 0u);
        }

        OS_REQUIRE(p_disconnect(conn) == XAPU_S_OK);
    }

    // 1F. Pitched DecodeConvertOpus mid-stream state check (1-sample interpolation lookahead
    //     must report state & 2 [need input], NOT state & 4 [stuck pending output]).
    {
        XApuConnectInput in{};
        in.processType = XApuProcessType_DecodeConvertOpus;
        in.streamCount = 2u;
        in.commandQueueDepth = 8u;
        in.sharedMemoryBytes = 0x10000u;
        in.options = 0u;

        XApuConnectOutput out{};
        XAPU_CONNECTION_HANDLE conn = nullptr;
        OS_REQUIRE(p_connect(&in, &out, &conn) == XAPU_S_OK);

        XApuRetailStartCommand start_cmd{};
        start_cmd.header.type = XApuRetailCommand_Start;
        start_cmd.header.streamIndex = 0u;
        start_cmd.channels = 2u;
        start_cmd.startPitch = 1.05f;
        OS_REQUIRE(p_enqueue(conn, &start_cmd, 0u) == XAPU_S_OK);
        XApuRetailResult res{};
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);

        const auto pkt = openscarlett::BuildOpusTestPacket(2u, 33u, 64u);
        std::vector<float> out_pcm(1024u * 2u, 0.0f);
        XApuRetailDataCommand data_cmd{};
        data_cmd.header.type = XApuRetailCommand_Data;
        data_cmd.header.streamIndex = 0u;
        data_cmd.header.options = 0u;  // mid-stream (not flush_end)
        data_cmd.targetPitch = 1.05f;
        data_cmd.firstFrame = 0u;
        data_cmd.frameCount = 0xFFFFFFFFu;
        data_cmd.inputDataLength = static_cast<uint32_t>(pkt.size());
        data_cmd.inputDataPtr = reinterpret_cast<uint64_t>(pkt.data());
        data_cmd.maxOutputDataLength = static_cast<uint32_t>(out_pcm.size() * sizeof(float));
        data_cmd.outputDataPtr = reinterpret_cast<uint64_t>(out_pcm.data());

        OS_REQUIRE(p_enqueue(conn, &data_cmd, 0u) == XAPU_S_OK);
        OS_REQUIRE(p_dequeue(conn, &res, sizeof(res), nullptr) == XAPU_S_OK);
        OS_REQUIRE(res.hresult == XAPU_S_OK);
        OS_REQUIRE(res.outputFrameCount > 900u && res.outputFrameCount < 960u);
        OS_REQUIRE((res.state & 2u) != 0u);
        OS_REQUIRE((res.state & 4u) == 0u);
        OS_REQUIRE(p_disconnect(conn) == XAPU_S_OK);
    }

    ::FreeLibrary(mod);
    std::printf("  PASS: xapu.dll retail engine.\n");
}

// ===========================================================================
// 2. chad.dll Real DSP Engine Tests
// ===========================================================================
#pragma pack(push, 1)
struct ChadActivateDesc {
    uint32_t kind;
    uint32_t flags;
    uint32_t blockFrames;
    uint32_t channels;
    uint32_t irLenFloats;
    uint64_t impulseResponsePtr;
};
static_assert(sizeof(ChadActivateDesc) == 28, "ChadActivateDesc must be 28 bytes");

struct ChadDspCommand {
    float gainIn;
    float gainOut;
    uint64_t inputPtr;
    uint64_t outputPtr;
    uint64_t multPtr;
};
static_assert(sizeof(ChadDspCommand) == 32, "ChadDspCommand must be 32 bytes");
#pragma pack(pop)

void TestChadDspEngine() {
    std::printf("[2/5] Testing chad.dll DSP engine (Cartesian/Polar FFT<->IFFT, Partitioned Convolution)...\n");

    const fs::path dll_path = GetExeDir() / L"chad.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnConnect = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, void**);
    using PfnConnectLimit = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
    using PfnDisconnect = HRESULT(__stdcall*)(HANDLE);
    using PfnActivate = HRESULT(__stdcall*)(HANDLE, const void*, uint32_t**, void**);
    using PfnDeactivate = HRESULT(__stdcall*)(HANDLE);
    using PfnSubmit = HRESULT(__stdcall*)(HANDLE, const void*, uint32_t*);
    using PfnSubmitEnv = HRESULT(__stdcall*)(HANDLE, const void*, const float*, uint32_t*);

    auto p_connect = LoadExport<PfnConnect>(mod, "XDspConnect");
    auto p_connect_lim = LoadExport<PfnConnectLimit>(mod, "XDspConnectWithMaximumStreamLimit");
    auto p_disconnect = LoadExport<PfnDisconnect>(mod, "XDspDisconnect");
    auto p_activate = LoadExport<PfnActivate>(mod, "XDspActivate");
    auto p_deactivate = LoadExport<PfnDeactivate>(mod, "XDspDeactivate");
    auto p_submit = LoadExport<PfnSubmit>(mod, "XDspSubmitCommand");
    auto p_submit_env = LoadExport<PfnSubmitEnv>(mod, "XDspSubmitCommandWithEnvelope");

    // 2A. Connect + ConnectWithMaximumStreamLimit
    std::vector<uint8_t> dsp_mem(4096u, 0u);
    void* client0 = nullptr;
    OS_REQUIRE(p_connect(dsp_mem.data(), static_cast<uint32_t>(dsp_mem.size()), 16u, &client0) == S_OK);
    OS_REQUIRE(client0 != nullptr);
    OS_REQUIRE(p_disconnect(client0) == S_OK);

    void* client = nullptr;
    OS_REQUIRE(p_connect_lim(dsp_mem.data(), static_cast<uint32_t>(dsp_mem.size()), 16u, 32u, 1u, &client) == S_OK);
    OS_REQUIRE(client != nullptr);

    // 2B. Cartesian & Polar FFT (kind=2) -> IFFT (kind=3) round-trip with multPtr window
    constexpr uint32_t kN = 512u;
    std::vector<float> signal(kN, 0.0f);
    std::vector<float> window(kN, 0.0f);
    for (uint32_t i = 0u; i < kN; ++i) {
        signal[i] = 0.6f * std::sin(2.0f * kPi * 7.0f * static_cast<float>(i) / static_cast<float>(kN)) +
                    (i == 23u ? 0.5f : 0.0f);
        window[i] = 0.54f - 0.46f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kN - 1u));
    }

    for (uint32_t flags : {0u, 4u}) {  // 0 = Cartesian, 4 = Polar
        ChadActivateDesc fft_desc{};
        fft_desc.kind = 2u;
        fft_desc.flags = flags;
        fft_desc.blockFrames = kN;
        fft_desc.channels = 1u;

        ChadActivateDesc ifft_desc{};
        ifft_desc.kind = 3u;
        ifft_desc.flags = flags;
        ifft_desc.blockFrames = kN;
        ifft_desc.channels = 1u;

        uint32_t* fft_status = nullptr;
        void* fft_stream = nullptr;
        uint32_t* ifft_status = nullptr;
        void* ifft_stream = nullptr;
        OS_REQUIRE(p_activate(client, &fft_desc, &fft_status, &fft_stream) == S_OK);
        OS_REQUIRE(fft_status != nullptr && fft_stream != nullptr);
        OS_REQUIRE(p_activate(client, &ifft_desc, &ifft_status, &ifft_stream) == S_OK);
        OS_REQUIRE(ifft_status != nullptr && ifft_stream != nullptr);

        std::vector<float> spec(kN, 0.0f);
        std::vector<float> recon(kN, 0.0f);

        ChadDspCommand fft_cmd{};
        fft_cmd.gainIn = 1.0f;
        fft_cmd.gainOut = 1.0f;
        fft_cmd.inputPtr = reinterpret_cast<uint64_t>(signal.data());
        fft_cmd.outputPtr = reinterpret_cast<uint64_t>(spec.data());
        fft_cmd.multPtr = reinterpret_cast<uint64_t>(window.data());

        uint32_t tok_fft = 0u;
        OS_REQUIRE(p_submit(fft_stream, &fft_cmd, &tok_fft) == S_OK);
        OS_REQUIRE(tok_fft != 0u);
        OS_REQUIRE(fft_status[0] == tok_fft && fft_status[1] == tok_fft);
        OS_REQUIRE(static_cast<int32_t>(fft_status[3]) == S_OK);

        ChadDspCommand ifft_cmd{};
        ifft_cmd.gainIn = 1.0f;
        ifft_cmd.gainOut = 1.0f;
        ifft_cmd.inputPtr = reinterpret_cast<uint64_t>(spec.data());
        ifft_cmd.outputPtr = reinterpret_cast<uint64_t>(recon.data());

        uint32_t tok_ifft = 0u;
        OS_REQUIRE(p_submit(ifft_stream, &ifft_cmd, &tok_ifft) == S_OK);
        OS_REQUIRE(tok_ifft != 0u);
        OS_REQUIRE(ifft_status[0] == tok_ifft && ifft_status[1] == tok_ifft);
        OS_REQUIRE(static_cast<int32_t>(ifft_status[3]) == S_OK);

        float max_err = 0.0f;
        for (uint32_t i = 0u; i < kN; ++i) {
            const float expected = signal[i] * window[i];
            max_err = (std::max)(max_err, std::fabs(recon[i] - expected));
        }
        OS_REQUIRE(max_err < 1e-4f);

        OS_REQUIRE(p_deactivate(fft_stream) == S_OK);
        OS_REQUIRE(p_deactivate(ifft_stream) == S_OK);
    }

    // 2C. Partitioned overlap-save Convolution (stereo interleaved & deinterleaved,
    //     2+ active partitions + 0xFFFFACE1 metadata sentinel partition skipped)
    constexpr uint32_t kDelay = 37u;
    constexpr uint32_t kRawPartitions = 3u;  // partition 0 = sentinel (skipped), partitions 1 & 2 = active
    constexpr uint32_t kIrPerCh = kRawPartitions * kN;
    float sentinel_f = 0.0f;
    const uint32_t sentinel_u32 = 0xFFFFACE1u;
    std::memcpy(&sentinel_f, &sentinel_u32, sizeof(float));

    for (bool deinterleaved : {false, true}) {
        std::vector<float> ir(kIrPerCh * 2u, 0.0f);
        if (deinterleaved) {
            ir[0u] = sentinel_f;
            ir[kIrPerCh] = sentinel_f;
            // Active partition 0 (raw partition 1) delta at kDelay for both channels
            ir[kN + kDelay] = 1.0f;
            ir[kIrPerCh + kN + kDelay] = 1.0f;
        } else {
            ir[0u] = sentinel_f;
            ir[1u] = sentinel_f;
            ir[(kN + kDelay) * 2u + 0u] = 1.0f;
            ir[(kN + kDelay) * 2u + 1u] = 1.0f;
        }

        ChadActivateDesc conv_desc{};
        conv_desc.kind = 1u;
        conv_desc.flags = 0x8u | (deinterleaved ? 0x2u : 0x0u);  // kFlagStereoIr | optional kFlagDeinterleaved
        conv_desc.blockFrames = kN;
        conv_desc.channels = 2u;
        conv_desc.irLenFloats = static_cast<uint32_t>(ir.size());
        conv_desc.impulseResponsePtr = reinterpret_cast<uint64_t>(ir.data());

        uint32_t* conv_status = nullptr;
        void* conv_stream = nullptr;
        OS_REQUIRE(p_activate(client, &conv_desc, &conv_status, &conv_stream) == S_OK);
        OS_REQUIRE(conv_status != nullptr && conv_stream != nullptr);

        std::vector<float> in_blk(kN * 2u, 0.0f);
        std::vector<float> out_blk(kN * 2u, 0.0f);
        for (uint32_t i = 0u; i < kN; ++i) {
            const float l = std::sin(2.0f * kPi * 5.0f * static_cast<float>(i) / static_cast<float>(kN));
            const float r = std::cos(2.0f * kPi * 3.0f * static_cast<float>(i) / static_cast<float>(kN));
            if (deinterleaved) {
                in_blk[i] = l;
                in_blk[kN + i] = r;
            } else {
                in_blk[i * 2u + 0u] = l;
                in_blk[i * 2u + 1u] = r;
            }
        }

        ChadDspCommand cmd{};
        cmd.gainIn = 1.0f;
        cmd.gainOut = 1.0f;
        cmd.inputPtr = reinterpret_cast<uint64_t>(in_blk.data());
        cmd.outputPtr = reinterpret_cast<uint64_t>(out_blk.data());

        uint32_t tok = 0u;
        OS_REQUIRE(p_submit(conv_stream, &cmd, &tok) == S_OK);
        OS_REQUIRE(conv_status[0] == tok && conv_status[1] == tok && static_cast<int32_t>(conv_status[3]) == S_OK);

        float max_delay_err = 0.0f;
        for (uint32_t i = kDelay; i < kN; ++i) {
            const float exp_l = deinterleaved ? in_blk[i - kDelay] : in_blk[(i - kDelay) * 2u + 0u];
            const float exp_r = deinterleaved ? in_blk[kN + (i - kDelay)] : in_blk[(i - kDelay) * 2u + 1u];
            const float got_l = deinterleaved ? out_blk[i] : out_blk[i * 2u + 0u];
            const float got_r = deinterleaved ? out_blk[kN + i] : out_blk[i * 2u + 1u];
            OS_REQUIRE(std::isfinite(got_l) && std::isfinite(got_r));
            max_delay_err = (std::max)(max_delay_err, std::fabs(got_l - exp_l));
            max_delay_err = (std::max)(max_delay_err, std::fabs(got_r - exp_r));
        }
        OS_REQUIRE(max_delay_err < 1e-3f);

        // Verify XDspSubmitCommandWithEnvelope scales output by envelope
        std::vector<float> env(kN, 0.25f);
        std::vector<float> out_env(kN * 2u, 0.0f);
        cmd.outputPtr = reinterpret_cast<uint64_t>(out_env.data());
        OS_REQUIRE(p_submit_env(conv_stream, &cmd, env.data(), &tok) == S_OK);
        for (uint32_t i = kDelay; i < kN; ++i) {
            const float exp_l = 0.25f * (deinterleaved ? in_blk[i - kDelay] : in_blk[(i - kDelay) * 2u + 0u]);
            const float got_l = deinterleaved ? out_env[i] : out_env[i * 2u + 0u];
            OS_REQUIRE(std::fabs(got_l - exp_l) < 1e-3f);
        }

        OS_REQUIRE(p_deactivate(conv_stream) == S_OK);
    }

    // 2D. Empty-IR passthrough scales by gainIn * gainOut
    {
        ChadActivateDesc pass_desc{};
        pass_desc.kind = 1u;
        pass_desc.flags = 0u;
        pass_desc.blockFrames = kN;
        pass_desc.channels = 1u;
        pass_desc.irLenFloats = 0u;
        pass_desc.impulseResponsePtr = 0ull;

        uint32_t* status = nullptr;
        void* stream = nullptr;
        OS_REQUIRE(p_activate(client, &pass_desc, &status, &stream) == S_OK);

        std::vector<float> in_p(kN, 0.8f);
        std::vector<float> out_p(kN, 0.0f);
        ChadDspCommand cmd{};
        cmd.gainIn = 0.5f;
        cmd.gainOut = 0.25f;
        cmd.inputPtr = reinterpret_cast<uint64_t>(in_p.data());
        cmd.outputPtr = reinterpret_cast<uint64_t>(out_p.data());

        uint32_t tok = 0u;
        OS_REQUIRE(p_submit(stream, &cmd, &tok) == S_OK);
        for (uint32_t i = 0u; i < kN; ++i) {
            OS_REQUIRE(std::fabs(out_p[i] - 0.1f) < 1e-6f);
        }
        OS_REQUIRE(p_deactivate(stream) == S_OK);
    }

    OS_REQUIRE(p_disconnect(client) == S_OK);
    ::FreeLibrary(mod);
    std::printf("  PASS: chad.dll DSP engine.\n");
}

// ===========================================================================
// 3. AcpHal.dll Retail SHAPE & 12-Slot IAcpHal Engine Tests
// ===========================================================================
struct XboxAcpShapeContexts {
    uint32_t numSrcContexts;      // +0x00
    uint32_t numEqCompContexts;   // +0x04
    uint32_t numFiltVolContexts;  // +0x08
    uint32_t numDmaContexts;      // +0x0C
    uint32_t numXmaContexts;      // +0x10
    uint32_t numPcmContexts;      // +0x14
    void* srcContexts;            // +0x18
    void* eqCompContexts;         // +0x20
    void* filtVolContexts;        // +0x28
    void* dmaContexts;            // +0x30
    void* xmaContexts;            // +0x38
    void* pcmContexts;            // +0x40
    uint32_t apuAddresses[6];     // +0x48..+0x5C
};
static_assert(sizeof(XboxAcpShapeContexts) == 0x60, "XboxAcpShapeContexts must be 96 bytes");

#pragma pack(push, 4)
struct XboxAcpMessage {
    uint32_t kind;
    uint32_t reserved;
    uint64_t audioClockSamples;
    uint64_t commandId;
    uint32_t extra;
};
#pragma pack(pop)
static_assert(sizeof(XboxAcpMessage) == 0x1Cu, "XboxAcpMessage must be 28 bytes");

struct IXboxRetailAcpHal {
    virtual int32_t __stdcall Connect(uint32_t cmdQueueLen, uint32_t msgQueueLen) = 0;
    virtual int32_t __stdcall Disconnect() = 0;
    virtual int32_t __stdcall SubmitCommand(
        uint32_t commandType, uint64_t commandId, uint32_t frameIndex,
        const void* pCommandData, uint32_t fenceApuAddr) = 0;
    virtual bool __stdcall PopMessage(void* pMessage) = 0;
    virtual uint32_t __stdcall GetNumMessages() = 0;
    virtual uint32_t __stdcall Release() = 0;
    virtual int32_t __stdcall UnsupportedSlot6() = 0;
    virtual int32_t __stdcall UnsupportedSlot7() = 0;
    virtual int32_t __stdcall UnsupportedSlot8() = 0;
    virtual int32_t __stdcall UnsupportedSlot9() = 0;
    virtual int32_t __stdcall MapDriverRegion(uint64_t* outVa) = 0;
    virtual uint32_t __stdcall Destroy(uint32_t flags) = 0;
};

void TestAcpHalRetailEngine() {
    std::printf("[3/5] Testing AcpHal.dll retail SHAPE & 12-slot IAcpHal XMA2 decode engine...\n");

    const fs::path dll_path = GetExeDir() / L"AcpHal.dll";
    HMODULE mod = ::LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnCreateHeap = int32_t(__stdcall*)(uint32_t, uint32_t);
    using PfnAlloc = int32_t(__stdcall*)(void**, APU_ADDRESS*, uint32_t, uint32_t, uint32_t);
    using PfnFree = int32_t(__stdcall*)(void*);
    using PfnGetState = int32_t(__stdcall*)(APU_HEAP_STATE*, uint32_t);
    using PfnAllocShape = int32_t(__stdcall*)(const void*, void*);
    using PfnReleaseShape = int32_t(__stdcall*)(void*);
    using PfnAcpCreate = int32_t(__stdcall*)(IXboxRetailAcpHal**);

    auto p_create_heap = LoadExport<PfnCreateHeap>(mod, "ApuCreateHeap");
    auto p_alloc = LoadExport<PfnAlloc>(mod, "ApuAlloc");
    auto p_free = LoadExport<PfnFree>(mod, "ApuFree");
    auto p_get_state = LoadExport<PfnGetState>(mod, "ApuHeapGetState");
    auto p_alloc_shape = LoadExport<PfnAllocShape>(mod, "AcpHalAllocateShapeContexts");
    auto p_release_shape = LoadExport<PfnReleaseShape>(mod, "AcpHalReleaseShapeContexts");
    auto p_acp_create = LoadExport<PfnAcpCreate>(mod, "AcpHalCreate");

    // 3A. ApuCreateHeap + ApuAlloc with retail flags=2 routes to non-cached pool even before retail mode
    OS_REQUIRE(p_create_heap(4u * 1024u * 1024u, 2u * 1024u * 1024u) == ACPHAL_S_OK);
    {
        void* nc_va = nullptr;
        APU_ADDRESS nc_apu = APU_INVALID_ADDRESS;
        OS_REQUIRE(p_alloc(&nc_va, &nc_apu, 1024u, 64u, 2u) == ACPHAL_S_OK);
        OS_REQUIRE(nc_va != nullptr && nc_apu != APU_INVALID_ADDRESS);

        APU_HEAP_STATE st_cached{};
        APU_HEAP_STATE st_noncached{};
        OS_REQUIRE(p_get_state(&st_cached, APU_ALLOC_CACHED) == ACPHAL_S_OK);
        OS_REQUIRE(p_get_state(&st_noncached, APU_ALLOC_NONCACHED) == ACPHAL_S_OK);
        OS_REQUIRE(st_cached.allocationCount == 0u);
        OS_REQUIRE(st_noncached.allocationCount == 1u && st_noncached.bytesAllocated == 1024u);
        OS_REQUIRE(p_free(nc_va) == ACPHAL_S_OK);
    }

    // 3B. Retail 1-arg AcpHalAllocateShapeContexts populates VA (+0x18..+0x40) AND APU (+0x48..+0x5C)
    XboxAcpShapeContexts shape{};
    shape.numSrcContexts = 2u;
    shape.numDmaContexts = 1u;
    shape.numXmaContexts = 4u;
    OS_REQUIRE(p_alloc_shape(&shape, nullptr) == ACPHAL_S_OK);
    OS_REQUIRE(shape.srcContexts != nullptr && shape.apuAddresses[0] != APU_INVALID_ADDRESS);
    OS_REQUIRE(shape.eqCompContexts == nullptr && shape.apuAddresses[1] == APU_INVALID_ADDRESS);
    OS_REQUIRE(shape.filtVolContexts == nullptr && shape.apuAddresses[2] == APU_INVALID_ADDRESS);
    OS_REQUIRE(shape.dmaContexts != nullptr && shape.apuAddresses[3] != APU_INVALID_ADDRESS);
    OS_REQUIRE(shape.xmaContexts != nullptr && shape.apuAddresses[4] != APU_INVALID_ADDRESS);
    OS_REQUIRE(shape.pcmContexts == nullptr && shape.apuAddresses[5] == APU_INVALID_ADDRESS);

    // 3C. AcpHalCreate in retail mode returns 12-slot non-IUnknown IAcpHal
    IXboxRetailAcpHal* hal = nullptr;
    OS_REQUIRE(p_acp_create(&hal) == ACPHAL_S_OK && hal != nullptr);
    OS_REQUIRE(hal->Connect(0x1000u, 0x1000u) == ACPHAL_S_OK);

    // Prepare real XMA2 packet + output ring + fence in APU memory
    constexpr uint32_t kToneFrames = 1024u;
    std::vector<float> src_pcm(kToneFrames * 2u);
    for (uint32_t i = 0u; i < kToneFrames; ++i) {
        const float s = 0.6f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        src_pcm[i * 2u + 0u] = s;
        src_pcm[i * 2u + 1u] = s;
    }
    openscarlett::Xma2Config xma_cfg{};
    xma_cfg.sample_rate = 48000u;
    xma_cfg.channels = 2u;
    xma_cfg.block_size = openscarlett::kXma2DefaultBlockSize;
    xma_cfg.subframes_per_frame = 4u;
    xma_cfg.quant_bits = 12u;
    std::vector<uint8_t> xma_pkts;
    OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(src_pcm.data(), kToneFrames, xma_cfg, xma_pkts).ok());
    OS_REQUIRE(xma_pkts.size() >= openscarlett::kXma2PacketSize);

    void* in_va = nullptr;
    APU_ADDRESS in_apu = APU_INVALID_ADDRESS;
    OS_REQUIRE(p_alloc(&in_va, &in_apu, static_cast<uint32_t>(xma_pkts.size()), 2048u, 1u) == ACPHAL_S_OK);
    std::memcpy(in_va, xma_pkts.data(), xma_pkts.size());

    constexpr uint32_t kOutRingBlocks = 31u;
    void* ring_va = nullptr;
    APU_ADDRESS ring_apu = APU_INVALID_ADDRESS;
    OS_REQUIRE(p_alloc(&ring_va, &ring_apu, kOutRingBlocks * 256u, 256u, 1u) == ACPHAL_S_OK);

    void* fence_va = nullptr;
    APU_ADDRESS fence_apu = APU_INVALID_ADDRESS;
    OS_REQUIRE(p_alloc(&fence_va, &fence_apu, 64u, 64u, 2u) == ACPHAL_S_OK);
    *static_cast<uint32_t*>(fence_va) = 0u;

    // Populate retail XMA context 0 (64 bytes)
    auto* xma_ctx = static_cast<uint8_t*>(shape.xmaContexts);
    const uint32_t num_pkts = static_cast<uint32_t>(xma_pkts.size() / openscarlett::kXma2PacketSize);
    const uint32_t dw0 = (num_pkts & 0xFFFu) | (1u << 20u) | (kOutRingBlocks << 22u);
    const uint32_t dw1 = (3u << 27u) | (1u << 29u) | (1u << 31u);  // 48kHz, stereo, enabled
    const uint32_t dw4 = 0u;                                        // curBuf = 0
    const uint32_t dw9 = 0u;                                        // readIdx = 0
    std::memcpy(xma_ctx + 0x00, &dw0, 4u);
    std::memcpy(xma_ctx + 0x04, &dw1, 4u);
    std::memcpy(xma_ctx + 0x10, &dw4, 4u);
    std::memcpy(xma_ctx + 0x14, &in_apu, 4u);
    std::memcpy(xma_ctx + 0x1C, &ring_apu, 4u);
    std::memcpy(xma_ctx + 0x24, &dw9, 4u);

    const uint32_t xma_idx = 0u;
    constexpr uint64_t kCmdId = 0x12345678ABCDEF01ull;
    OS_REQUIRE(hal->SubmitCommand(5u, kCmdId, 0u, &xma_idx, fence_apu) == ACPHAL_S_OK);

    // Fence atomically written to 1
    OS_REQUIRE(*static_cast<const uint32_t*>(fence_va) == 1u);

    // Write block index advanced & non-silent int16 PCM decoded into output ring
    uint32_t dw0_after = 0u;
    std::memcpy(&dw0_after, xma_ctx + 0x00, 4u);
    const uint32_t write_idx = (dw0_after >> 27u) & 0x1Fu;
    OS_REQUIRE(write_idx > 0u);

    const auto* ring_i16 = static_cast<const int16_t*>(ring_va);
    int16_t peak_i16 = 0;
    for (uint32_t i = 0u; i < write_idx * 128u; ++i) {
        const int16_t v = ring_i16[i];
        const int16_t av = (v < 0 && v != INT16_MIN) ? static_cast<int16_t>(-v) : v;
        if (av > peak_i16) peak_i16 = av;
    }
    OS_REQUIRE(peak_i16 > 50);

    // Message queue & smooth 48 kHz QPC audioClockSamples verification
    OS_REQUIRE(hal->GetNumMessages() == 1u);
    XboxAcpMessage msg{};
    OS_REQUIRE(hal->PopMessage(&msg));
    OS_REQUIRE(msg.kind == 0x10u);
    OS_REQUIRE(msg.commandId == kCmdId);
    OS_REQUIRE(msg.audioClockSamples > 0ull);
    OS_REQUIRE(hal->GetNumMessages() == 0u);

    const uint64_t clk_before_burst = msg.audioClockSamples;
    for (uint32_t b = 0u; b < 10u; ++b) {
        OS_REQUIRE(hal->SubmitCommand(1u, 0x2000ull + b, b, nullptr, 0u) == ACPHAL_S_OK);
    }
    OS_REQUIRE(hal->GetNumMessages() == 10u);
    XboxAcpMessage burst_msg{};
    for (uint32_t b = 0u; b < 10u; ++b) {
        OS_REQUIRE(hal->PopMessage(&burst_msg));
    }
    // Rapid burst must NOT artificially jump by 10*128 samples independent of QPC time
    OS_REQUIRE(burst_msg.audioClockSamples >= clk_before_burst);
    ::Sleep(25u);
    OS_REQUIRE(hal->SubmitCommand(1u, 0x3000ull, 11u, nullptr, 0u) == ACPHAL_S_OK);
    XboxAcpMessage delayed_msg{};
    OS_REQUIRE(hal->PopMessage(&delayed_msg));
    OS_REQUIRE(delayed_msg.audioClockSamples >= burst_msg.audioClockSamples + 800ull);

    // Unsupported slots 6..10
    OS_REQUIRE(hal->UnsupportedSlot6() == static_cast<int32_t>(0x8AC80F0Eu));
    OS_REQUIRE(hal->UnsupportedSlot7() == static_cast<int32_t>(0x8AC80F0Eu));
    OS_REQUIRE(hal->UnsupportedSlot8() == static_cast<int32_t>(0x8AC80F0Eu));
    OS_REQUIRE(hal->UnsupportedSlot9() == static_cast<int32_t>(0x8AC80F0Eu));
    uint64_t mapped_va = 1234ull;
    OS_REQUIRE(hal->MapDriverRegion(&mapped_va) == static_cast<int32_t>(0x80070032u));
    OS_REQUIRE(mapped_va == 0ull);

    // Disconnect pushes kind==0x80, Release frees cleanly
    OS_REQUIRE(hal->Disconnect() == ACPHAL_S_OK);
    OS_REQUIRE(hal->GetNumMessages() == 1u);
    std::memset(&msg, 0, sizeof(msg));
    OS_REQUIRE(hal->PopMessage(&msg));
    OS_REQUIRE(msg.kind == 0x80u);
    OS_REQUIRE(hal->Release() == 0u);

    OS_REQUIRE(p_free(fence_va) == ACPHAL_S_OK);
    OS_REQUIRE(p_free(ring_va) == ACPHAL_S_OK);
    OS_REQUIRE(p_free(in_va) == ACPHAL_S_OK);
    OS_REQUIRE(p_release_shape(&shape) == ACPHAL_S_OK);
    ::FreeLibrary(mod);
    std::printf("  PASS: AcpHal.dll retail SHAPE & 12-slot IAcpHal.\n");
}

// ===========================================================================
// 4. XAudio2_9.dll Console Compatibility, Implicit COM MTA & WASAPI/NULL Parity
// ===========================================================================
struct XAudio2WorkerArgs {
    bool force_null = true;
};

DWORD WINAPI XAudio2ConsoleThreadProc(LPVOID param) {
    // Intentionally NEVER calls CoInitializeEx to verify implicit COM MTA setup.
    const auto* args = static_cast<const XAudio2WorkerArgs*>(param);
    ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", args->force_null ? "1" : nullptr);

    IXAudio2* xa = nullptr;
    constexpr uint32_t kConsoleFlags = 0x400u | 0x41u;
    constexpr uint32_t kConsoleProcMask = 0x20u;
    OS_REQUIRE(XAudio2Create(&xa, kConsoleFlags, kConsoleProcMask) == OS_XAUDIO2_S_OK && xa != nullptr);

    // Malformed endpoint ID must fail with OS_XAUDIO2_E_INVALIDARG
    IXAudio2MasteringVoice* bad_master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&bad_master, 2u, 48000u, 0u, L"malformed-id") ==
               OS_XAUDIO2_E_INVALIDARG);
    OS_REQUIRE(bad_master == nullptr);

    // Console audio endpoint must succeed
    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, 48000u, 0u, L"console-audio-endpoint-0") ==
               OS_XAUDIO2_S_OK && master != nullptr);
    OS_REQUIRE(master->SetVolume(0.01f) == OS_XAUDIO2_S_OK);

    OS_XAUDIO2_BACKEND_INFO binfo{};
    OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo) == OS_XAUDIO2_S_OK);
    if (args->force_null) {
        OS_REQUIRE(binfo.null_audio_forced == 1u);
        OS_REQUIRE(binfo.wasapi_active == 0u);
    } else {
        OS_REQUIRE(binfo.null_audio_forced == 0u);
    }

    // CreateSourceVoice with XAUDIO2_VOICE_NOSHAPE (0x400)
    OS_WAVEFORMATEX fmt{};
    fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2u;
    fmt.nSamplesPerSec = 48000u;
    fmt.wBitsPerSample = 32u;
    fmt.nBlockAlign = 8u;
    fmt.nAvgBytesPerSec = 48000u * 8u;

    constexpr uint32_t kVoiceNoShape = 0x400u;
    IXAudio2SourceVoice* src = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&src, &fmt, kVoiceNoShape) == OS_XAUDIO2_S_OK && src != nullptr);

    constexpr uint32_t kFrames = 1920u;
    std::vector<float> pcm(kFrames * 2u, 0.0f);
    for (uint32_t i = 0u; i < kFrames; ++i) {
        const float s = 0.25f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        pcm[i * 2u + 0u] = s;
        pcm[i * 2u + 1u] = s;
    }

    OS_XAUDIO2_BUFFER buf{};
    buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = static_cast<uint32_t>(pcm.size() * sizeof(float));
    buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm.data());
    OS_REQUIRE(src->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(src->Start() == OS_XAUDIO2_S_OK);

    if (args->force_null) {
        std::vector<float> mix(512u * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), 512u) == OS_XAUDIO2_S_OK);
        float peak = 0.0f;
        for (float s : mix) {
            peak = (std::max)(peak, std::fabs(s));
        }
        OS_REQUIRE(peak > 1e-4f);
    } else {
        ::Sleep(60u);
        OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(binfo.null_audio_forced == 0u);
        if (binfo.wasapi_active != 0u) {
            OS_REQUIRE(binfo.wasapi_sample_rate >= 8000u);
            OS_REQUIRE(binfo.wasapi_channels >= 1u);
            OS_REQUIRE(binfo.wasapi_buffer_frames >= (binfo.wasapi_sample_rate * 50u) / 1000u);
            OS_REQUIRE(binfo.wasapi_frames_rendered > 0ull);
        } else {
            // Graceful fallback when host has no active WASAPI render endpoint
            std::vector<float> mix(512u * 2u, 0.0f);
            OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), 512u) == OS_XAUDIO2_S_OK);
        }
    }

    src->DestroyVoice();
    master->DestroyVoice();
    xa->Release();
    return 0u;
}

void TestXAudio2ConsoleAndWasapiParity() {
    std::printf("[4/5] Testing XAudio2_9.dll console flags, implicit COM MTA, and WASAPI/NULL parity...\n");

    char prev_env[64]{};
    const DWORD prev_len = ::GetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", prev_env, sizeof(prev_env));
    const bool had_prev = (prev_len > 0u && prev_len < sizeof(prev_env));

    for (bool force_null : {true, false}) {
        XAudio2WorkerArgs args{force_null};
        HANDLE th = ::CreateThread(nullptr, 0u, XAudio2ConsoleThreadProc, &args, 0u, nullptr);
        OS_REQUIRE(th != nullptr);
        OS_REQUIRE(::WaitForSingleObject(th, 10000u) == WAIT_OBJECT_0);
        DWORD exit_code = 1u;
        OS_REQUIRE(::GetExitCodeThread(th, &exit_code) && exit_code == 0u);
        ::CloseHandle(th);
    }

    ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", had_prev ? prev_env : nullptr);
    std::printf("  PASS: XAudio2_9.dll console compatibility & WASAPI/NULL parity.\n");
}

// ===========================================================================
// 5. Direct WAV Recorder (OPENSCARLETT_RECORD_WAV + .wav.json Dropout Audit)
// ===========================================================================
void TestDirectWavRecorder() {
    std::printf("[5/5] Testing OPENSCARLETT_RECORD_WAV crash-safe RIFF WAVE + .wav.json dropout metrics...\n");

    wchar_t prev_wav[2048]{};
    const DWORD prev_wav_len =
        ::GetEnvironmentVariableW(L"OPENSCARLETT_RECORD_WAV", prev_wav, 2048u);
    const bool had_prev_wav = (prev_wav_len > 0u && prev_wav_len < 2048u);

    char prev_null[64]{};
    const DWORD prev_null_len =
        ::GetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", prev_null, sizeof(prev_null));
    const bool had_prev_null = (prev_null_len > 0u && prev_null_len < sizeof(prev_null));

    const fs::path rec_dir = GetExeDir() / L"test_wav_recorder_out";
    const fs::path wav_path = rec_dir / L"sub" / L"retail_mix_capture.wav";
    fs::path json_path = wav_path;
    json_path += L".json";

    std::error_code ec;
    fs::remove_all(rec_dir, ec);

    ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", "1");
    ::SetEnvironmentVariableW(L"OPENSCARLETT_RECORD_WAV", wav_path.c_str());

    constexpr uint32_t kSampleRate = 48000u;
    constexpr uint32_t kQuantumFrames = 480u;  // 10 ms
    constexpr uint32_t kNumQuanta = 60u;       // 600 ms (> 0.5s periodic flush threshold!)
    constexpr uint32_t kTotalFrames = kQuantumFrames * kNumQuanta;

    {
        IXAudio2* xa = nullptr;
        OS_REQUIRE(XAudio2Create(&xa, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

        IXAudio2MasteringVoice* master = nullptr;
        OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, kSampleRate) == OS_XAUDIO2_S_OK && master != nullptr);

        OS_WAVEFORMATEX fmt{};
        fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2u;
        fmt.nSamplesPerSec = kSampleRate;
        fmt.wBitsPerSample = 32u;
        fmt.nBlockAlign = 8u;
        fmt.nAvgBytesPerSec = kSampleRate * 8u;

        IXAudio2SourceVoice* src = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&src, &fmt) == OS_XAUDIO2_S_OK && src != nullptr);

        std::vector<float> tone(kTotalFrames * 2u, 0.0f);
        for (uint32_t i = 0u; i < kTotalFrames; ++i) {
            tone[i * 2u + 0u] = 0.5f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
            tone[i * 2u + 1u] = 0.4f * std::cos(2.0f * kPi * 660.0f * static_cast<float>(i) / 48000.0f);
        }

        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(tone.size() * sizeof(float));
        buf.pAudioData = reinterpret_cast<const uint8_t*>(tone.data());
        OS_REQUIRE(src->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(src->Start() == OS_XAUDIO2_S_OK);

        std::vector<float> mix_q(kQuantumFrames * 2u, 0.0f);
        for (uint32_t q = 0u; q < kNumQuanta; ++q) {
            OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix_q.data(), kQuantumFrames) ==
                       OS_XAUDIO2_S_OK);
            if (q == 52u) {
                // Verify periodic ~0.5s crash-safe header + sidecar flush BEFORE engine teardown!
                OS_REQUIRE(fs::exists(wav_path) && fs::exists(json_path));
                std::ifstream live_in(wav_path, std::ios::binary);
                OS_REQUIRE(live_in.good());
                uint8_t live_hdr[44]{};
                live_in.read(reinterpret_cast<char*>(live_hdr), 44);
                OS_REQUIRE(live_in.gcount() == 44);
                uint32_t mid_data_sz = 0u;
                std::memcpy(&mid_data_sz, live_hdr + 40, 4u);
                OS_REQUIRE(mid_data_sz >= 24000u * 4u);
            }
        }

        src->DestroyVoice();
        master->DestroyVoice();
        xa->Release();
    }

    ::SetEnvironmentVariableW(L"OPENSCARLETT_RECORD_WAV", had_prev_wav ? prev_wav : nullptr);
    ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", had_prev_null ? prev_null : nullptr);

    OS_REQUIRE(fs::exists(wav_path));
    OS_REQUIRE(fs::exists(json_path));

    // Validate 44-byte RIFF WAVE PCM16 stereo header + audio payload
    {
        std::ifstream wf(wav_path, std::ios::binary | std::ios::ate);
        OS_REQUIRE(wf.good());
        const auto file_bytes = static_cast<uint64_t>(wf.tellg());
        const uint32_t expected_data_bytes = kTotalFrames * 4u;
        OS_REQUIRE(file_bytes == 44ull + expected_data_bytes);

        wf.seekg(0, std::ios::beg);
        uint8_t hdr[44]{};
        wf.read(reinterpret_cast<char*>(hdr), 44);
        OS_REQUIRE(wf.gcount() == 44);
        OS_REQUIRE(std::memcmp(hdr + 0, "RIFF", 4) == 0);
        OS_REQUIRE(std::memcmp(hdr + 8, "WAVE", 4) == 0);
        OS_REQUIRE(std::memcmp(hdr + 12, "fmt ", 4) == 0);
        OS_REQUIRE(std::memcmp(hdr + 36, "data", 4) == 0);

        uint32_t riff_sz = 0u, fmt_sz = 0u, sr = 0u, byte_rate = 0u, data_sz = 0u;
        uint16_t audio_fmt = 0u, num_ch = 0u, block_align = 0u, bps = 0u;
        std::memcpy(&riff_sz, hdr + 4, 4u);
        std::memcpy(&fmt_sz, hdr + 16, 4u);
        std::memcpy(&audio_fmt, hdr + 20, 2u);
        std::memcpy(&num_ch, hdr + 22, 2u);
        std::memcpy(&sr, hdr + 24, 4u);
        std::memcpy(&byte_rate, hdr + 28, 4u);
        std::memcpy(&block_align, hdr + 32, 2u);
        std::memcpy(&bps, hdr + 34, 2u);
        std::memcpy(&data_sz, hdr + 40, 4u);

        OS_REQUIRE(riff_sz == 36u + expected_data_bytes);
        OS_REQUIRE(fmt_sz == 16u && audio_fmt == 1u && num_ch == 2u);
        OS_REQUIRE(sr == kSampleRate && byte_rate == kSampleRate * 4u);
        OS_REQUIRE(block_align == 4u && bps == 16u);
        OS_REQUIRE(data_sz == expected_data_bytes);

        std::vector<int16_t> samples(kTotalFrames * 2u, 0);
        wf.read(reinterpret_cast<char*>(samples.data()), expected_data_bytes);
        OS_REQUIRE(static_cast<uint32_t>(wf.gcount()) == expected_data_bytes);

        int32_t max_pcm = 0;
        for (int16_t s : samples) {
            max_pcm = (std::max)(max_pcm, std::abs(static_cast<int32_t>(s)));
        }
        OS_REQUIRE(max_pcm > 15000 && max_pcm < 17000);
    }

    // Validate .wav.json sidecar metrics
    {
        std::ifstream jf(json_path);
        OS_REQUIRE(jf.good());
        const std::string json_txt(
            (std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
        OS_REQUIRE(json_txt.find("\"sample_rate\": 48000") != std::string::npos);
        OS_REQUIRE(json_txt.find("\"channels\": 2") != std::string::npos);
        OS_REQUIRE(json_txt.find("\"total_frames\": 28800") != std::string::npos);
        OS_REQUIRE(json_txt.find("\"active_10ms_windows\": 60") != std::string::npos);
        OS_REQUIRE(json_txt.find("\"silent_holes_in_bursts\": 0") != std::string::npos);
    }

    // Also verify live WasapiRenderLoop recording when a host audio endpoint is available
    {
        const fs::path live_wav = rec_dir / L"live_wasapi_capture.wav";
        fs::path live_json = live_wav;
        live_json += L".json";
        ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", nullptr);
        ::SetEnvironmentVariableW(L"OPENSCARLETT_RECORD_WAV", live_wav.c_str());

        IXAudio2* xa = nullptr;
        OS_REQUIRE(XAudio2Create(&xa, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);
        IXAudio2MasteringVoice* master = nullptr;
        OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, kSampleRate) == OS_XAUDIO2_S_OK && master != nullptr);
        OS_REQUIRE(master->SetVolume(0.02f) == OS_XAUDIO2_S_OK);

        OS_WAVEFORMATEX fmt{};
        fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2u;
        fmt.nSamplesPerSec = kSampleRate;
        fmt.wBitsPerSample = 32u;
        fmt.nBlockAlign = 8u;
        fmt.nAvgBytesPerSec = kSampleRate * 8u;

        IXAudio2SourceVoice* src = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&src, &fmt) == OS_XAUDIO2_S_OK && src != nullptr);

        constexpr uint32_t kLiveFrames = 4800u;  // 100 ms continuous tone
        std::vector<float> live_tone(kLiveFrames * 2u, 0.0f);
        for (uint32_t i = 0u; i < kLiveFrames; ++i) {
            const float s = 0.5f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
            live_tone[i * 2u + 0u] = s;
            live_tone[i * 2u + 1u] = s;
        }

        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(live_tone.size() * sizeof(float));
        buf.pAudioData = reinterpret_cast<const uint8_t*>(live_tone.data());
        OS_REQUIRE(src->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(src->Start() == OS_XAUDIO2_S_OK);

        ::Sleep(80u);

        OS_XAUDIO2_BACKEND_INFO binfo{};
        OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo) == OS_XAUDIO2_S_OK);
        const bool wasapi_ok = (binfo.wasapi_active != 0u);

        src->DestroyVoice();
        master->DestroyVoice();
        xa->Release();

        ::SetEnvironmentVariableW(L"OPENSCARLETT_RECORD_WAV", had_prev_wav ? prev_wav : nullptr);
        ::SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", had_prev_null ? prev_null : nullptr);

        if (wasapi_ok) {
            OS_REQUIRE(fs::exists(live_wav) && fs::exists(live_json));
            std::ifstream jf(live_json);
            OS_REQUIRE(jf.good());
            const std::string jtxt(
                (std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
            OS_REQUIRE(jtxt.find("\"silent_holes_in_bursts\": 0") != std::string::npos);
        }
    }

    fs::remove_all(rec_dir, ec);
    std::printf("  PASS: OPENSCARLETT_RECORD_WAV RIFF WAVE + .wav.json verified (0 dropouts).\n");
}

}  // namespace

int main() {
    TestXApuRetailEngine();
    TestChadDspEngine();
    TestAcpHalRetailEngine();
    TestXAudio2ConsoleAndWasapiParity();
    TestDirectWavRecorder();
    std::printf("ALL retail audio engine tests PASSED.\n");
    return 0;
}
