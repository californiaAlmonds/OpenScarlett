#include "openscarlett/gpu/shader_cache.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "openscarlett/core/telemetry.h"

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <unknwn.h>
#include <windows.h>
#endif

namespace openscarlett {

namespace {

constexpr const char* kCacheTelemetryModule = "shader_cache";
constexpr char kCacheFileMagic[4] = {'O', 'S', 'S', 'C'};

struct OnDiskCacheHeader {
    char magic[4];
    uint32_t format_version;
    uint64_t key_lo;
    uint64_t key_hi;
    uint32_t stage;
    uint32_t target_len;
    uint32_t dxil_size;
    uint32_t crc32;            // IEEE 802.3 CRC32 over header[0..36) + target_profile + dxil
    uint64_t payload_checksum; // 64-bit FNV-1a + Mix64 integrity digest
};
static_assert(sizeof(OnDiskCacheHeader) == 48u, "Unexpected OnDiskCacheHeader size");

// Standard IEEE 802.3 CRC32 (polynomial 0xEDB88320)
uint32_t UpdateCrc32(uint32_t crc, const uint8_t* data, size_t size) noexcept {
    static const auto kTable = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0u; i < 256u; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = ((c & 1u) != 0u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = ~crc;
    for (size_t i = 0u; i < size; ++i) {
        c = kTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

uint32_t ComputeCacheEntryCrc32(
    const OnDiskCacheHeader& hdr,
    const std::string& target,
    const std::vector<uint8_t>& dxil) noexcept {
    uint32_t crc = 0u;
    // Hash header fields [0..36) (everything before crc32 and payload_checksum)
    crc = UpdateCrc32(crc, reinterpret_cast<const uint8_t*>(&hdr), 36u);
    if (!target.empty()) {
        crc = UpdateCrc32(crc, reinterpret_cast<const uint8_t*>(target.data()), target.size());
    }
    if (!dxil.empty()) {
        crc = UpdateCrc32(crc, dxil.data(), dxil.size());
    }
    return crc;
}

uint64_t Mix64(uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

uint64_t HashPayloadChecksum(const std::string& target, const std::vector<uint8_t>& dxil) noexcept {
    uint64_t h = 0xCBF29CE484222325ull;
    for (char c : target) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    h ^= 0xFFull;
    h *= 0x100000001B3ull;
    for (uint8_t b : dxil) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return Mix64(h ^ static_cast<uint64_t>(dxil.size()));
}

void UpdateHash128(uint64_t& h0, uint64_t& h1, const uint8_t* data, size_t size) noexcept {
    for (size_t i = 0; i < size; ++i) {
        const uint64_t b = data[i];
        h0 ^= b;
        h0 *= 0x100000001B3ull;
        h1 ^= (b + 0x9E3779B97F4A7C15ull + (h0 >> 17));
        h1 *= 0x94D049BB133111EBull;
    }
}

void UpdateHash128U32(uint64_t& h0, uint64_t& h1, uint32_t v) noexcept {
    uint8_t buf[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
    };
    UpdateHash128(h0, h1, buf, sizeof(buf));
}

bool EndsWith50(const std::string& target) noexcept {
    return target.size() >= 4 && target.compare(target.size() - 4, 4, "_5_0") == 0;
}

// ---------------------------------------------------------------------------
// Poison-Aware Shader-Translator Mutex with RAII Unwind Tracking & Recovery
// ---------------------------------------------------------------------------
// If a shader translation attempt throws an exception or unwinds without
// committing under the translator mutex, the mutex is unlocked cleanly (never
// deadlocking) while recording poison state + reason. The next caller detects
// the poison bit, logs telemetry & diagnostics, clears the poison state, and
// proceeds cleanly.
class PoisonAwareTranslatorMutex {
public:
    static PoisonAwareTranslatorMutex& Global() {
        static PoisonAwareTranslatorMutex g_instance;
        return g_instance;
    }

    class RecoverableLock {
    public:
        explicit RecoverableLock(
            PoisonAwareTranslatorMutex& owner,
            bool* out_recovered = nullptr,
            std::string* out_recovery_diag = nullptr)
            : owner_(owner), lock_(owner.mu_) {
            if (owner_.poisoned_.load(std::memory_order_acquire)) {
                owner_.poisoned_.store(false, std::memory_order_release);
                const uint64_t rec_n =
                    owner_.recovery_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                recovered_ = true;
                if (out_recovered) {
                    *out_recovered = true;
                }
                if (out_recovery_diag) {
                    std::string diag =
                        "Recovered shader-translator mutex from poisoned state (recovery #" +
                        std::to_string(rec_n) + ")";
                    if (!owner_.last_poison_reason_.empty()) {
                        diag += "; prior fault: " + owner_.last_poison_reason_;
                    }
                    *out_recovery_diag = std::move(diag);
                }
                owner_.last_poison_reason_.clear();
                CallTelemetry::Global().Record(
                    kCacheTelemetryModule, "TranslatorMutexRecoveredFromPoison", CallOutcome::Ok,
                    static_cast<int32_t>(rec_n));
            } else if (out_recovered) {
                *out_recovered = false;
            }
        }

        RecoverableLock(const RecoverableLock&) = delete;
        RecoverableLock& operator=(const RecoverableLock&) = delete;

        void Commit() noexcept { committed_ = true; }
        void MarkPoisonReason(std::string reason) { poison_reason_ = std::move(reason); }
        bool RecoveredFromPoison() const noexcept { return recovered_; }

        ~RecoverableLock() {
            if (!committed_) {
                owner_.poisoned_.store(true, std::memory_order_release);
                const uint64_t pn =
                    owner_.poison_count_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                owner_.last_poison_reason_ =
                    poison_reason_.empty()
                        ? "uncommitted unwind / exception during shader translation"
                        : poison_reason_;
                CallTelemetry::Global().Record(
                    kCacheTelemetryModule, "TranslatorMutexPoisoned", CallOutcome::Error,
                    static_cast<int32_t>(pn));
            }
        }

    private:
        PoisonAwareTranslatorMutex& owner_;
        std::unique_lock<std::mutex> lock_;
        bool committed_ = false;
        bool recovered_ = false;
        std::string poison_reason_;
    };

    bool IsPoisoned() const noexcept {
        return poisoned_.load(std::memory_order_acquire);
    }

    uint64_t RecoveryCount() const noexcept {
        return recovery_count_.load(std::memory_order_acquire);
    }

    void ResetForTest() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        poisoned_.store(false, std::memory_order_release);
        poison_count_.store(0u, std::memory_order_release);
        recovery_count_.store(0u, std::memory_order_release);
        last_poison_reason_.clear();
    }

private:
    std::mutex mu_;
    std::atomic<bool> poisoned_{false};
    std::atomic<uint64_t> poison_count_{0u};
    std::atomic<uint64_t> recovery_count_{0u};
    std::string last_poison_reason_;
};

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Self-contained minimal COM definitions for dxcompiler.dll (zero <dxcapi.h> dependency,
// compiles cleanly with 0 warnings on MSVC /W4, Clang-CL, and MinGW-w64).
// ---------------------------------------------------------------------------
struct OsDxcBuffer {
    const void* Ptr;
    SIZE_T Size;
    UINT Encoding;
};

constexpr UINT kOsDxcCpUtf8 = 65001u;
constexpr UINT32 kOsDxcOutObject = 1u;
constexpr UINT32 kOsDxcOutErrors = 2u;

struct OsIDxcBlob : public IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

struct OsIDxcBlobEncoding : public OsIDxcBlob {
    virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL* pKnown, UINT32* pCodePage) = 0;
};

struct OsIDxcBlobUtf8 : public OsIDxcBlobEncoding {
    virtual LPCSTR STDMETHODCALLTYPE GetStringPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetStringLength() = 0;
};

struct OsIDxcOperationResult : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT* pStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResult(OsIDxcBlob** ppResult) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(OsIDxcBlobEncoding** ppErrors) = 0;
};

struct OsIDxcResult : public OsIDxcOperationResult {
    virtual BOOL STDMETHODCALLTYPE HasOutput(UINT32 dxcOutKind) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOutput(
        UINT32 dxcOutKind, REFIID iid, void** ppvObject, void** ppOutputName) = 0;
    virtual UINT32 STDMETHODCALLTYPE GetNumOutputs() = 0;
    virtual UINT32 STDMETHODCALLTYPE GetOutputByIndex(UINT32 Index) = 0;
    virtual UINT32 STDMETHODCALLTYPE PrimaryOutput() = 0;
};

struct OsIDxcCompiler3 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compile(
        const OsDxcBuffer* pSource,
        LPCWSTR* pArguments,
        UINT32 argCount,
        void* pIncludeHandler,
        REFIID riid,
        LPVOID* ppResult) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disassemble(
        const OsDxcBuffer* pObject,
        REFIID riid,
        LPVOID* ppResult) = 0;
};

using PfnDxcCreateInstance = HRESULT(STDMETHODCALLTYPE*)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);

constexpr CLSID kClsidDxcCompiler = {
    0x73e22d93u, 0xe6ceu, 0x47f3u, {0xb5u, 0xbfu, 0xf0u, 0x66u, 0x4fu, 0x39u, 0xc1u, 0xb0u}};
constexpr IID kIidIDxcCompiler3 = {
    0x228b4687u, 0x5a6au, 0x4730u, {0x90u, 0x0cu, 0x97u, 0x02u, 0xb2u, 0x20u, 0x3fu, 0x54u}};
constexpr IID kIidIDxcResult = {
    0x58346cdau, 0xdde7u, 0x4497u, {0x94u, 0x61u, 0x6fu, 0x87u, 0xafu, 0x5eu, 0x06u, 0x59u}};
constexpr IID kIidIDxcBlob = {
    0x8ba5fb08u, 0x5195u, 0x40e2u, {0xacu, 0x58u, 0x0du, 0x98u, 0x9cu, 0x3au, 0x01u, 0x02u}};
constexpr IID kIidIDxcBlobUtf8 = {
    0x3da636c9u, 0xba71u, 0x4024u, {0xa3u, 0x01u, 0x30u, 0xcbu, 0xf1u, 0x25u, 0x30u, 0x5bu}};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class InProcessDxcLoader {
public:
    static InProcessDxcLoader& Instance() {
        static InProcessDxcLoader g_loader;
        return g_loader;
    }

    PfnDxcCreateInstance Resolve(const std::string& hint_dxc_path) {
        std::unique_lock<std::mutex> lock(mu_);
        if (poisoned_) {
            poisoned_ = false;
            attempted_default_ = false;
            CallTelemetry::Global().Record(
                kCacheTelemetryModule, "DxcLoaderMutexRecoveredFromPoison", CallOutcome::Ok, 1);
        }
        bool committed = false;
        struct UnwindPoisonGuard {
            bool& poisoned_ref;
            bool& committed_ref;
            ~UnwindPoisonGuard() {
                if (!committed_ref) {
                    poisoned_ref = true;
                }
            }
        } poison_guard{poisoned_, committed};

        if (create_instance_ != nullptr) {
            committed = true;
            return create_instance_;
        }
        if (attempted_default_ && hint_dxc_path.empty()) {
            committed = true;
            return nullptr;
        }
        if (hint_dxc_path.empty()) {
            attempted_default_ = true;
        }

        std::vector<std::filesystem::path> candidates;
        std::error_code ec;

        // 1. Sibling of explicit hint_dxc_path (if it points to dxc.exe or a directory)
        if (!hint_dxc_path.empty()) {
            std::filesystem::path hp(hint_dxc_path);
            if (std::filesystem::is_directory(hp, ec)) {
                candidates.push_back(hp / "dxcompiler.dll");
            } else {
                candidates.push_back(hp.parent_path() / "dxcompiler.dll");
            }
        }

        // 2. Beside running executable
        wchar_t self_buf[MAX_PATH]{};
        const DWORD self_len = GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
        if (self_len > 0u && self_len < MAX_PATH) {
            candidates.push_back(std::filesystem::path(self_buf).parent_path() / L"dxcompiler.dll");
        }

        // 3. Beside resolved portable dxc.exe (handles OPENSCARLETT_SHIM_DIR / OPENSCARLETT_DXC_PATH / PATH / WinSDK)
        const std::string resolved_exe = ResolvePortableDxcPath(hint_dxc_path);
        if (!resolved_exe.empty()) {
            candidates.push_back(std::filesystem::path(resolved_exe).parent_path() / "dxcompiler.dll");
        }

        // 4. Windows 10/11 SDK fallback
        const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
        if (std::filesystem::exists(kits_bin, ec)) {
            std::vector<std::filesystem::path> sdk_dlls;
            for (const auto& entry : std::filesystem::directory_iterator(kits_bin, ec)) {
                if (!entry.is_directory()) continue;
                const auto cand = entry.path() / "x64" / "dxcompiler.dll";
                if (std::filesystem::exists(cand, ec)) {
                    sdk_dlls.push_back(cand);
                }
            }
            std::sort(sdk_dlls.begin(), sdk_dlls.end());
            for (auto it = sdk_dlls.rbegin(); it != sdk_dlls.rend(); ++it) {
                candidates.push_back(*it);
            }
        }

        for (const auto& cand : candidates) {
            if (!std::filesystem::exists(cand, ec)) continue;
            const std::wstring wpath = cand.wstring();
            HMODULE mod = LoadLibraryExW(
                wpath.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!mod) {
                mod = LoadLibraryW(wpath.c_str());
            }
            if (!mod) continue;
            auto fn = reinterpret_cast<PfnDxcCreateInstance>(GetProcAddress(mod, "DxcCreateInstance"));
            if (fn != nullptr) {
                module_ = mod;
                create_instance_ = fn;
                committed = true;
                return create_instance_;
            }
            FreeLibrary(mod);
        }

        // 5. System search path fallback
        if (HMODULE mod = LoadLibraryW(L"dxcompiler.dll"); mod != nullptr) {
            auto fn = reinterpret_cast<PfnDxcCreateInstance>(GetProcAddress(mod, "DxcCreateInstance"));
            if (fn != nullptr) {
                module_ = mod;
                create_instance_ = fn;
                committed = true;
                return create_instance_;
            }
            FreeLibrary(mod);
        }

        committed = true;
        return nullptr;
    }

private:
    std::mutex mu_;
    HMODULE module_ = nullptr;
    PfnDxcCreateInstance create_instance_ = nullptr;
    bool attempted_default_ = false;
    bool poisoned_ = false;
};

Status CompileHlslWithFxc50(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_bytecode,
    std::string* out_diagnostics) {
    out_bytecode.clear();
    ID3DBlob* code_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    const HRESULT hr = D3DCompile(
        hlsl.data(),
        hlsl.size(),
        "openscarlett_shader.hlsl",
        nullptr,
        nullptr,
        entry_point.c_str(),
        target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        &code_blob,
        &err_blob);
    if (err_blob) {
        if (out_diagnostics && err_blob->GetBufferPointer() && err_blob->GetBufferSize() > 0) {
            out_diagnostics->assign(
                static_cast<const char*>(err_blob->GetBufferPointer()),
                err_blob->GetBufferSize());
        }
        err_blob->Release();
    }
    if (FAILED(hr) || !code_blob) {
        if (code_blob) code_blob->Release();
        return Status::Error(StatusCode::ShaderError, "D3DCompile failed for target " + target);
    }
    const auto* ptr = static_cast<const uint8_t*>(code_blob->GetBufferPointer());
    out_bytecode.assign(ptr, ptr + code_blob->GetBufferSize());
    code_blob->Release();
    return Status::Ok();
}
#endif

std::string BuildStandInHlsl(ShaderStage stage, bool mesh_has_payload) {
    switch (stage) {
        case ShaderStage::Compute:
            return "[numthreads(1, 1, 1)] void main() {}\n";
        case ShaderStage::Vertex:
            return "struct VSOut { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                   "VSOut main() { VSOut o; o.pos = float4(0.0, 0.0, 0.0, 1.0); "
                   "o.param0 = float4(0.0, 0.0, 0.0, 1.0); return o; }\n";
        case ShaderStage::Pixel:
            return "float4 main() : SV_TARGET0 { return float4(0.0, 0.0, 0.0, 1.0); }\n";
        case ShaderStage::Mesh:
            if (mesh_has_payload) {
                return "struct ASPayload { uint _data[4]; };\n"
                       "struct MeshVert { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                       "[outputtopology(\"triangle\")] [numthreads(1, 1, 1)]\n"
                       "void main(in payload ASPayload _p, out vertices MeshVert v[3], out indices uint3 pr[1]) {\n"
                       "    SetMeshOutputCounts(0, 0);\n"
                       "}\n";
            }
            return "struct MeshVert { float4 pos : SV_POSITION; float4 param0 : TEXCOORD0; };\n"
                   "[outputtopology(\"triangle\")] [numthreads(1, 1, 1)]\n"
                   "void main(out vertices MeshVert v[3], out indices uint3 pr[1]) {\n"
                   "    SetMeshOutputCounts(0, 0);\n"
                   "}\n";
        case ShaderStage::Amplification:
            return "struct ASPayload { uint _data[4]; };\n"
                   "groupshared ASPayload _p;\n"
                   "[numthreads(1, 1, 1)] void main() {\n"
                   "    _p._data[0] = 0u; _p._data[1] = 0u; _p._data[2] = 0u; _p._data[3] = 0u;\n"
                   "    DispatchMesh(0, 0, 0, _p);\n"
                   "}\n";
        case ShaderStage::Geometry:
            return "struct GSIn { float4 pos : SV_POSITION; };\n"
                   "[maxvertexcount(3)] void main(triangle GSIn v[3], inout TriangleStream<GSIn> s) {}\n";
        case ShaderStage::Hull:
            return "struct HSIn { float4 pos : SV_POSITION; };\n"
                   "struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
                   "PatchConst PatchCF(InputPatch<HSIn, 3> p) {\n"
                   "    PatchConst o; o.edges[0] = 1.0; o.edges[1] = 1.0; o.edges[2] = 1.0; o.inside = 1.0; return o;\n"
                   "}\n"
                   "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")]\n"
                   "[outputcontrolpoints(3)] [patchconstantfunc(\"PatchCF\")]\n"
                   "HSIn main(InputPatch<HSIn, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }\n";
        case ShaderStage::Domain:
            return "struct DSIn { float4 pos : SV_POSITION; };\n"
                   "struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
                   "[domain(\"tri\")] DSIn main(PatchConst pc, float3 b : SV_DomainLocation, const OutputPatch<DSIn, 3> p) {\n"
                   "    DSIn o; o.pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z; return o;\n"
                   "}\n";
        case ShaderStage::Library:
            return "[shader(\"raygeneration\")] void main() {}\n";
        default:
            return "[numthreads(1, 1, 1)] void main() {}\n";
    }
}

std::string StageStandInFxcTarget(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return "vs_5_0";
        case ShaderStage::Pixel:    return "ps_5_0";
        case ShaderStage::Geometry: return "gs_5_0";
        case ShaderStage::Hull:     return "hs_5_0";
        case ShaderStage::Domain:   return "ds_5_0";
        case ShaderStage::Compute:
        default:                    return "cs_5_0";
    }
}

Status CompileHlslPreferInProcessDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path) {
    if (IsInProcessDxcAvailable(dxc_path)) {
        Status st = CompileHlslInProcessDxc(hlsl, target, entry_point, out_dxil, out_diagnostics, dxc_path);
        if (st.ok()) {
            return st;
        }
        // Only fall back to external dxc.exe if the loader/COM creation failed (not an HLSL syntax error)
        if (st.code != StatusCode::NotFound && st.code != StatusCode::NotImplemented) {
            return st;
        }
    }
    return CompileHlslWithDxc(hlsl, target, entry_point, out_dxil, out_diagnostics, dxc_path);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public In-Process dxcompiler.dll API
// ---------------------------------------------------------------------------
bool IsInProcessDxcAvailable(const std::string& hint_dxc_path) {
#if defined(_WIN32)
    return InProcessDxcLoader::Instance().Resolve(hint_dxc_path) != nullptr;
#else
    (void)hint_dxc_path;
    return false;
#endif
}

Status CompileHlslInProcessDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& hint_dxc_path) {
    out_dxil.clear();
    out_diagnostics.clear();
    if (hlsl.empty() || target.empty() || entry_point.empty()) {
        return Status::Error(StatusCode::InvalidArgument,
                             "CompileHlslInProcessDxc requires non-empty HLSL/target/entry");
    }
#if defined(_WIN32)
    PfnDxcCreateInstance create_fn = InProcessDxcLoader::Instance().Resolve(hint_dxc_path);
    if (!create_fn) {
        return Status::Error(StatusCode::NotFound, "dxcompiler.dll is not available");
    }

    // Create a fresh per-call IDxcCompiler3 instance for 100% thread-safety across worker threads.
    OsIDxcCompiler3* compiler = nullptr;
    HRESULT hr = create_fn(kClsidDxcCompiler, kIidIDxcCompiler3, reinterpret_cast<void**>(&compiler));
    if (FAILED(hr) || !compiler) {
        return Status::Error(StatusCode::NotFound, "DxcCreateInstance(IDxcCompiler3) failed");
    }

    const std::wstring w_entry = Utf8ToWide(entry_point);
    const std::wstring w_target = Utf8ToWide(target);
    LPCWSTR args[4] = {L"-E", w_entry.c_str(), L"-T", w_target.c_str()};

    OsDxcBuffer src_buf{};
    src_buf.Ptr = hlsl.data();
    src_buf.Size = hlsl.size();
    src_buf.Encoding = kOsDxcCpUtf8;

    OsIDxcResult* result = nullptr;
    hr = compiler->Compile(&src_buf, args, 4u, nullptr, kIidIDxcResult, reinterpret_cast<void**>(&result));
    compiler->Release();
    if (FAILED(hr) || !result) {
        return Status::Error(StatusCode::ShaderError, "IDxcCompiler3::Compile returned failure HRESULT");
    }

    // Extract compiler errors / warnings if present
    if (result->HasOutput(kOsDxcOutErrors)) {
        OsIDxcBlobUtf8* err_utf8 = nullptr;
        if (SUCCEEDED(result->GetOutput(kOsDxcOutErrors, kIidIDxcBlobUtf8,
                                        reinterpret_cast<void**>(&err_utf8), nullptr)) &&
            err_utf8 != nullptr) {
            if (err_utf8->GetStringPointer() && err_utf8->GetStringLength() > 0u) {
                out_diagnostics.assign(err_utf8->GetStringPointer(), err_utf8->GetStringLength());
            }
            err_utf8->Release();
        }
    }

    HRESULT compile_status = E_FAIL;
    (void)result->GetStatus(&compile_status);
    if (FAILED(compile_status)) {
        result->Release();
        CallTelemetry::Global().Record(kCacheTelemetryModule, "InProcessDxcCompile", CallOutcome::Error, -1);
        return Status::Error(StatusCode::ShaderError, "dxcompiler.dll compile failed: " + out_diagnostics);
    }

    OsIDxcBlob* obj_blob = nullptr;
    hr = result->GetOutput(kOsDxcOutObject, kIidIDxcBlob, reinterpret_cast<void**>(&obj_blob), nullptr);
    result->Release();
    if (FAILED(hr) || !obj_blob || !obj_blob->GetBufferPointer() || obj_blob->GetBufferSize() < 32u) {
        if (obj_blob) obj_blob->Release();
        return Status::Error(StatusCode::ShaderError, "dxcompiler.dll produced empty DXIL object");
    }

    const auto* ptr = static_cast<const uint8_t*>(obj_blob->GetBufferPointer());
    out_dxil.assign(ptr, ptr + obj_blob->GetBufferSize());
    obj_blob->Release();

    if (std::memcmp(out_dxil.data(), "DXBC", 4u) != 0) {
        out_dxil.clear();
        return Status::Error(StatusCode::ShaderError, "dxcompiler.dll output is not a valid DXBC/DXIL container");
    }
    if (!VerifyDxbcContainerHash(out_dxil.data(), out_dxil.size())) {
        if (auto rs = ResignDxilContainer(out_dxil); !rs.ok()) {
            return rs;
        }
    }

    CallTelemetry::Global().Record(kCacheTelemetryModule, "InProcessDxcCompile", CallOutcome::Ok, 0);
    return Status::Ok();
#else
    (void)hint_dxc_path;
    return Status::Error(StatusCode::NotImplemented, "In-process dxcompiler.dll requires Windows");
#endif
}

std::string ShaderCacheKey::ToHex() const {
    char buf[33]{};
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
}

ShaderCacheKey ComputeShaderCacheKey(
    const void* input_blob,
    size_t input_size,
    const std::string& target_profile,
    uint32_t shader_model_flags,
    uint32_t cache_format_version) noexcept {
    uint64_t h0 = 0xCBF29CE484222325ull;
    uint64_t h1 = 0x84222325CBF29CE4ull ^ 0x9E3779B97F4A7C15ull;

    UpdateHash128U32(h0, h1, cache_format_version);
    UpdateHash128U32(h0, h1, shader_model_flags);
    UpdateHash128U32(h0, h1, static_cast<uint32_t>(target_profile.size()));
    if (!target_profile.empty()) {
        UpdateHash128(h0, h1,
                      reinterpret_cast<const uint8_t*>(target_profile.data()),
                      target_profile.size());
    }
    UpdateHash128U32(h0, h1, static_cast<uint32_t>(input_size));
    if (input_blob && input_size > 0) {
        UpdateHash128(h0, h1, static_cast<const uint8_t*>(input_blob), input_size);
    }

    ShaderCacheKey k{};
    k.lo = Mix64(h0 ^ (h1 >> 13));
    k.hi = Mix64(h1 ^ (h0 << 7));
    return k;
}

std::string SelectTargetProfileForStage(ShaderStage stage, uint32_t highest_shader_model) {
    // SM 6.6 enables HLSL ResourceDescriptorHeap[] / SamplerDescriptorHeap[] bindless
    // and 64-bit typed/buffer atomics (gated on D3D12_FEATURE_DATA_D3D12_OPTIONS9);
    // adapters supporting only SM 6.5 cleanly fall back to _6_5 (and Vulkan VK_EXT_descriptor_indexing).
    if (stage == ShaderStage::Mesh) {
        return (highest_shader_model >= 0x66u) ? "ms_6_6" : "ms_6_5";
    }
    if (stage == ShaderStage::Amplification) {
        return (highest_shader_model >= 0x66u) ? "as_6_6" : "as_6_5";
    }
    if (stage == ShaderStage::Library) {
        if (highest_shader_model >= 0x66u) return "lib_6_6";
        if (highest_shader_model >= 0x65u) return "lib_6_5";
        return "lib_6_3";
    }

    const char* prefix = "cs";
    switch (stage) {
        case ShaderStage::Vertex:   prefix = "vs"; break;
        case ShaderStage::Pixel:    prefix = "ps"; break;
        case ShaderStage::Geometry: prefix = "gs"; break;
        case ShaderStage::Hull:     prefix = "hs"; break;
        case ShaderStage::Domain:   prefix = "ds"; break;
        case ShaderStage::Compute:
        default:                    prefix = "cs"; break;
    }

    if (highest_shader_model >= 0x66u) return std::string(prefix) + "_6_6";
    if (highest_shader_model >= 0x65u) return std::string(prefix) + "_6_5";
    if (highest_shader_model >= 0x62u) return std::string(prefix) + "_6_2";
    if (highest_shader_model >= 0x60u) return std::string(prefix) + "_6_0";
    return std::string(prefix) + "_5_0";
}

ShaderCache& ShaderCache::Global() {
    static ShaderCache g_cache;
    return g_cache;
}

ShaderCache::~ShaderCache() {
    WaitForPendingCompiles();
}

void ShaderCache::SetCacheDirectory(const std::string& dir_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    explicit_cache_dir_ = dir_path;
}

std::string ShaderCache::ResolveCacheDirectory() const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!explicit_cache_dir_.empty()) {
            return explicit_cache_dir_;
        }
    }
#if defined(_WIN32)
    char env_buf[1024]{};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len > 0 && len < sizeof(env_buf)) {
        return std::string(env_buf, len);
    }
    // Keep default shader cache off C:\ (%TEMP%): resolve to executable-local cache/shaders.
    wchar_t exe_buf[MAX_PATH]{};
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_buf, static_cast<DWORD>(MAX_PATH));
    if (exe_len > 0u && exe_len < MAX_PATH) {
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        if (!exe_dir.empty()) {
            return (exe_dir / "cache" / "shaders").string();
        }
    }
#endif
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty()) {
        return (cwd / "cache" / "shaders").string();
    }
    return (std::filesystem::path("cache") / "shaders").string();
}

std::string ShaderCache::CacheFilePathForKey(const ShaderCacheKey& key) const {
    return (std::filesystem::path(ResolveCacheDirectory()) / (key.ToHex() + ".ossc")).string();
}

void ShaderCache::EnableAsyncPsoCompilation(bool enable) {
    async_override_.store(enable ? 1 : 0, std::memory_order_release);
}

void ShaderCache::ResetAsyncPsoCompilationOverride() {
    async_override_.store(-1, std::memory_order_release);
}

bool ShaderCache::IsAsyncPsoCompilationEnabled() const {
    const int ov = async_override_.load(std::memory_order_acquire);
    if (ov >= 0) return ov != 0;
#if defined(_WIN32)
    char buf[64]{};
    const DWORD len = GetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return std::strcmp(buf, "1") == 0 ||
               std::strcmp(buf, "true") == 0 ||
               std::strcmp(buf, "TRUE") == 0;
    }
#endif
    return false;
}

void ShaderCache::ClearMemoryCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    memory_cache_.clear();
}

void ShaderCache::ResetStats() {
    mem_hits_.store(0, std::memory_order_relaxed);
    disk_hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    corrupt_entries_.store(0, std::memory_order_relaxed);
    async_queued_.store(0, std::memory_order_relaxed);
    async_completed_.store(0, std::memory_order_relaxed);
    async_failed_.store(0, std::memory_order_relaxed);
    sync_succeeded_.store(0, std::memory_order_relaxed);
    sync_failed_.store(0, std::memory_order_relaxed);
    standins_served_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        last_compile_error_.clear();
    }
}

ShaderCacheStats ShaderCache::Stats() const noexcept {
    ShaderCacheStats s{};
    s.memory_hits = mem_hits_.load(std::memory_order_relaxed);
    s.disk_hits = disk_hits_.load(std::memory_order_relaxed);
    s.cache_misses = misses_.load(std::memory_order_relaxed);
    s.corrupt_entries = corrupt_entries_.load(std::memory_order_relaxed);
    s.async_compiles_queued = async_queued_.load(std::memory_order_relaxed);
    s.async_compiles_completed = async_completed_.load(std::memory_order_relaxed);
    s.async_compiles_failed = async_failed_.load(std::memory_order_relaxed);
    s.sync_compiles_succeeded = sync_succeeded_.load(std::memory_order_relaxed);
    s.sync_compiles_failed = sync_failed_.load(std::memory_order_relaxed);
    s.total_compile_failures = s.sync_compiles_failed + s.async_compiles_failed;
    s.standins_served = standins_served_.load(std::memory_order_relaxed);
    return s;
}

std::string ShaderCache::LastCompileError() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return last_compile_error_;
}

std::string ShaderCache::FormatDiagnosticsSummary() const {
    const ShaderCacheStats s = Stats();
    const std::string last_err = LastCompileError();
    std::ostringstream oss;
    oss << "ShaderCache[hits=" << (s.memory_hits + s.disk_hits)
        << " (mem=" << s.memory_hits << ", disk=" << s.disk_hits << ")"
        << ", misses=" << s.cache_misses
        << ", sync_ok=" << s.sync_compiles_succeeded
        << ", sync_fail=" << s.sync_compiles_failed
        << ", async_queued=" << s.async_compiles_queued
        << ", async_ok=" << s.async_compiles_completed
        << ", async_fail=" << s.async_compiles_failed
        << ", total_fail=" << s.total_compile_failures
        << ", corrupt=" << s.corrupt_entries
        << ", standins=" << s.standins_served << "]";
    if (!last_err.empty()) {
        oss << " last_error=\"" << last_err << "\"";
    }
    return oss.str();
}

bool ShaderCache::TryLoadFromDisk(const ShaderCacheKey& key, MemoryEntry& out_entry) {
    const std::string path = CacheFilePathForKey(key);
    std::error_code ec;

    // Single-syscall open (avoids redundant exists() stat per PSO at 10,000+ scale)
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return false;
    }

    auto evict_corrupt = [&]() {
        corrupt_entries_.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::remove(path, ec);
        CallTelemetry::Global().Record(kCacheTelemetryModule, "CorruptEntryEvicted", CallOutcome::Error, -1);
    };

    const std::streamsize file_size = ifs.tellg();
    if (file_size < static_cast<std::streamsize>(sizeof(OnDiskCacheHeader))) {
        ifs.close();
        evict_corrupt();
        return false;
    }
    ifs.seekg(0, std::ios::beg);

    OnDiskCacheHeader hdr{};
    ifs.read(reinterpret_cast<char*>(&hdr), static_cast<std::streamsize>(sizeof(hdr)));
    if (!ifs ||
        std::memcmp(hdr.magic, kCacheFileMagic, 4) != 0 ||
        hdr.format_version != kShaderCacheFormatVersion ||
        hdr.key_lo != key.lo ||
        hdr.key_hi != key.hi ||
        hdr.target_len == 0u || hdr.target_len > 64u ||
        hdr.dxil_size < 32u || hdr.dxil_size > (64u * 1024u * 1024u)) {
        ifs.close();
        evict_corrupt();
        return false;
    }

    const uint64_t expected_size = sizeof(OnDiskCacheHeader) +
                                   static_cast<uint64_t>(hdr.target_len) +
                                   static_cast<uint64_t>(hdr.dxil_size);
    if (static_cast<uint64_t>(file_size) != expected_size) {
        ifs.close();
        evict_corrupt();
        return false;
    }

    std::string target(hdr.target_len, '\0');
    std::vector<uint8_t> dxil(hdr.dxil_size);
    ifs.read(target.data(), static_cast<std::streamsize>(hdr.target_len));
    ifs.read(reinterpret_cast<char*>(dxil.data()), static_cast<std::streamsize>(hdr.dxil_size));
    const bool read_ok = static_cast<bool>(ifs);
    ifs.close();

    if (!read_ok) {
        evict_corrupt();
        return false;
    }

    if (std::memcmp(dxil.data(), "DXBC", 4) != 0) {
        evict_corrupt();
        return false;
    }
    if (HashPayloadChecksum(target, dxil) != hdr.payload_checksum) {
        evict_corrupt();
        return false;
    }
    // Verify IEEE 802.3 CRC32 (self-heal any header/payload corruption or legacy zeroed CRC32 if poisoned)
    const uint32_t expected_crc = ComputeCacheEntryCrc32(hdr, target, dxil);
    if (hdr.crc32 != expected_crc) {
        evict_corrupt();
        return false;
    }

    out_entry.stage = static_cast<ShaderStage>(hdr.stage);
    out_entry.target_profile = std::move(target);
    out_entry.dxil = std::move(dxil);
    return true;
}

Status ShaderCache::SaveToDiskAtomic(const ShaderCacheKey& key, const MemoryEntry& entry) {
    static std::atomic<uint64_t> g_tmp_seq{1};
    const std::string dir = ResolveCacheDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status::Error(StatusCode::IoError, "Failed to create shader cache directory: " + dir);
    }

    const std::filesystem::path final_path = std::filesystem::path(dir) / (key.ToHex() + ".ossc");
    uint32_t pid = 0;
#if defined(_WIN32)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#endif
    const uint64_t seq = g_tmp_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream tmp_name;
    tmp_name << key.ToHex() << "." << pid << "." << seq << ".tmp";
    const std::filesystem::path tmp_path = std::filesystem::path(dir) / tmp_name.str();

    OnDiskCacheHeader hdr{};
    std::memcpy(hdr.magic, kCacheFileMagic, 4);
    hdr.format_version = kShaderCacheFormatVersion;
    hdr.key_lo = key.lo;
    hdr.key_hi = key.hi;
    hdr.stage = static_cast<uint32_t>(entry.stage);
    hdr.target_len = static_cast<uint32_t>(entry.target_profile.size());
    hdr.dxil_size = static_cast<uint32_t>(entry.dxil.size());
    hdr.crc32 = ComputeCacheEntryCrc32(hdr, entry.target_profile, entry.dxil);
    hdr.payload_checksum = HashPayloadChecksum(entry.target_profile, entry.dxil);

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return Status::Error(StatusCode::IoError, "Failed to open shader cache temp file");
        }
        ofs.write(reinterpret_cast<const char*>(&hdr), static_cast<std::streamsize>(sizeof(hdr)));
        ofs.write(entry.target_profile.data(), static_cast<std::streamsize>(entry.target_profile.size()));
        ofs.write(reinterpret_cast<const char*>(entry.dxil.data()), static_cast<std::streamsize>(entry.dxil.size()));
        ofs.flush();
        if (!ofs) {
            ofs.close();
            std::filesystem::remove(tmp_path, ec);
            return Status::Error(StatusCode::IoError, "Failed to write shader cache temp file");
        }
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path.string().c_str(), final_path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp_path, ec);
        return Status::Error(StatusCode::IoError, "Atomic rename failed for shader cache entry");
    }
#else
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return Status::Error(StatusCode::IoError, "Rename failed for shader cache entry");
    }
#endif
    return Status::Ok();
}

bool ShaderCache::TryGet(const ShaderCacheKey& key, CachedShaderResult& out_result) {
    out_result = CachedShaderResult{};
    out_result.key = key;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = memory_cache_.find(key);
        if (it != memory_cache_.end()) {
            out_result.stage = it->second.stage;
            out_result.target_profile = it->second.target_profile;
            out_result.dxil = it->second.dxil;
            out_result.from_memory_cache = true;
            out_result.diagnostics = "ShaderCache HIT (memory) key=" + key.ToHex() + " target=" + out_result.target_profile;
            mem_hits_.fetch_add(1, std::memory_order_relaxed);
            CallTelemetry::Global().Record(kCacheTelemetryModule, "MemoryHit", CallOutcome::Ok, 0);
            return true;
        }
    }

    MemoryEntry disk_entry{};
    if (TryLoadFromDisk(key, disk_entry)) {
        out_result.stage = disk_entry.stage;
        out_result.target_profile = disk_entry.target_profile;
        out_result.dxil = disk_entry.dxil;
        out_result.from_disk_cache = true;
        out_result.diagnostics = "ShaderCache HIT (disk) key=" + key.ToHex() + " target=" + out_result.target_profile;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            memory_cache_[key] = std::move(disk_entry);
        }
        disk_hits_.fetch_add(1, std::memory_order_relaxed);
        CallTelemetry::Global().Record(kCacheTelemetryModule, "DiskHit", CallOutcome::Ok, 0);
        return true;
    }

    return false;
}

Status ShaderCache::Put(
    const ShaderCacheKey& key,
    ShaderStage stage,
    const std::string& target_profile,
    const std::vector<uint8_t>& dxil) {
    if (dxil.size() < 32u || std::memcmp(dxil.data(), "DXBC", 4) != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot cache invalid DXBC/DXIL bytecode");
    }
    MemoryEntry entry{};
    entry.stage = stage;
    entry.target_profile = target_profile;
    entry.dxil = dxil;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        memory_cache_[key] = entry;
    }
    return SaveToDiskAtomic(key, entry);
}

Status ShaderCache::CompileStandInShader(
    ShaderStage stage,
    const std::string& target_profile,
    bool mesh_has_payload,
    bool prefer_dxil,
    const std::string& dxc_path,
    std::vector<uint8_t>& out_bytecode,
    std::string* out_diagnostics) {
    const bool needs_dxc = (stage == ShaderStage::Mesh || stage == ShaderStage::Amplification ||
                            stage == ShaderStage::Library || prefer_dxil);
    const std::string standin_target = needs_dxc ? target_profile : StageStandInFxcTarget(stage);
    const std::string cache_id = std::to_string(static_cast<uint32_t>(stage)) + ":" +
                                 standin_target + ":" + (mesh_has_payload ? "1" : "0");

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = standin_cache_.find(cache_id);
        if (it != standin_cache_.end()) {
            out_bytecode = it->second;
            return Status::Ok();
        }
    }

    const std::string hlsl = BuildStandInHlsl(stage, mesh_has_payload);
    Status st = Status::Ok();
    std::string diag;
#if defined(_WIN32)
    if (!needs_dxc || EndsWith50(standin_target)) {
        st = CompileHlslWithFxc50(hlsl, standin_target, "main", out_bytecode, &diag);
    } else {
        st = CompileHlslPreferInProcessDxc(hlsl, standin_target, "main", out_bytecode, diag, dxc_path);
    }
#else
    (void)dxc_path;
    st = Status::Error(StatusCode::NotImplemented, "Stand-in shader compilation requires Windows");
#endif
    if (out_diagnostics && !diag.empty()) {
        *out_diagnostics = diag;
    }
    if (!st.ok()) {
        return st;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        standin_cache_[cache_id] = out_bytecode;
    }
    return Status::Ok();
}

Status ExecuteGuardedShaderTranslation(
    const std::function<Status()>& work,
    std::string* out_diagnostics,
    bool* out_recovered_from_poison) {
    if (!work) {
        if (out_recovered_from_poison) *out_recovered_from_poison = false;
        return Status::Error(StatusCode::InvalidArgument, "Null shader translation callback");
    }

    bool recovered = false;
    std::string recovery_diag;
    PoisonAwareTranslatorMutex::RecoverableLock guard(
        PoisonAwareTranslatorMutex::Global(), &recovered, &recovery_diag);
    if (out_recovered_from_poison) {
        *out_recovered_from_poison = recovered;
    }

    try {
        Status st = work();
        guard.Commit();
        if (out_diagnostics && !recovery_diag.empty()) {
            if (out_diagnostics->empty()) {
                *out_diagnostics = recovery_diag;
            } else {
                *out_diagnostics = recovery_diag + " | " + *out_diagnostics;
            }
        }
        return st;
    } catch (const std::exception& ex) {
        const std::string reason = std::string("Shader translation threw std::exception: ") + ex.what();
        guard.MarkPoisonReason(reason);
        if (out_diagnostics) {
            *out_diagnostics = recovery_diag.empty() ? reason : (recovery_diag + " | " + reason);
        }
        return Status::Error(StatusCode::ShaderError, reason);
    } catch (...) {
        const std::string reason = "Shader translation threw unknown exception (translator mutex poisoned)";
        guard.MarkPoisonReason(reason);
        if (out_diagnostics) {
            *out_diagnostics = recovery_diag.empty() ? reason : (recovery_diag + " | " + reason);
        }
        return Status::Error(StatusCode::ShaderError, reason);
    }
}

bool IsShaderTranslatorMutexPoisoned() noexcept {
    return PoisonAwareTranslatorMutex::Global().IsPoisoned();
}

uint64_t GetShaderTranslatorMutexRecoveryCount() noexcept {
    return PoisonAwareTranslatorMutex::Global().RecoveryCount();
}

void ResetShaderTranslatorMutexForTest() noexcept {
    PoisonAwareTranslatorMutex::Global().ResetForTest();
}

Status ShaderCache::CompileRealXboxShaderSync(
    const std::vector<uint8_t>& blob_bytes,
    ShaderStage stage,
    const std::string& target_profile,
    const ShaderCacheLiftRequest& request,
    std::vector<uint8_t>& out_dxil,
    std::string* out_diagnostics) {
    out_dxil.clear();

    const bool use_fxc_50 = EndsWith50(target_profile);
    const bool can_use_inproc_dxc = (!use_fxc_50 && IsInProcessDxcAvailable(request.dxc_path));

    Rdna2LiftOptions opts{};
    opts.use_sm65 = !use_fxc_50;
    opts.emit_spirv = false;
    opts.emit_hlsl = true;
    opts.validate_spirv_external = false;
    opts.compile_dxil_with_dxc = (!use_fxc_50 && !can_use_inproc_dxc);
    opts.mesh_has_payload = request.mesh_has_payload;
    opts.target_profile_override = target_profile;
    opts.dxc_path = request.dxc_path;

    Rdna2LiftResult lifted{};
    std::string guard_diag;
    bool recovered_from_poison = false;
    Status st = ExecuteGuardedShaderTranslation(
        [&]() -> Status { return LiftRdna2Xs2Shader(blob_bytes, lifted, opts); },
        &guard_diag,
        &recovered_from_poison);
    if (!st.ok()) {
        if (out_diagnostics) {
            std::string detail = lifted.dxc_diagnostics.empty() ? st.message : lifted.dxc_diagnostics;
            *out_diagnostics = guard_diag.empty() ? detail : (guard_diag + " | " + detail);
        }
        return st;
    }

    try {
        if (use_fxc_50) {
#if defined(_WIN32)
            std::string fxc_diag;
            st = CompileHlslWithFxc50(lifted.hlsl_source, target_profile, "main", out_dxil, &fxc_diag);
            if (!st.ok()) {
                if (out_diagnostics) *out_diagnostics = fxc_diag.empty() ? st.message : fxc_diag;
                return st;
            }
#else
            (void)stage;
            return Status::Error(StatusCode::NotImplemented, "SM 5.0 fallback requires Windows");
#endif
        } else if (can_use_inproc_dxc) {
            std::string dxc_diag;
            st = CompileHlslPreferInProcessDxc(
                lifted.hlsl_source, target_profile, "main", out_dxil, dxc_diag, request.dxc_path);
            if (!st.ok()) {
                if (out_diagnostics) *out_diagnostics = dxc_diag.empty() ? st.message : dxc_diag;
                return st;
            }
        } else {
            out_dxil = std::move(lifted.pc_dxil);
        }
    } catch (const std::exception& ex) {
        const std::string err = std::string("Backend DXIL/FXC compilation threw exception: ") + ex.what();
        if (out_diagnostics) *out_diagnostics = err;
        return Status::Error(StatusCode::ShaderError, err);
    } catch (...) {
        const std::string err = "Backend DXIL/FXC compilation threw unknown exception";
        if (out_diagnostics) *out_diagnostics = err;
        return Status::Error(StatusCode::ShaderError, err);
    }

    if (out_diagnostics && recovered_from_poison && !guard_diag.empty()) {
        *out_diagnostics = guard_diag;
    }
    (void)stage;
    return Status::Ok();
}

void ShaderCache::EnqueueAsyncJob(AsyncJob job) {
    {
        std::lock_guard<std::mutex> lock(async_mutex_);
        if (!inflight_keys_.insert(job.key).second) {
            return;  // Already queued or compiling
        }
        job_queue_.push_back(std::move(job));
        async_queued_.fetch_add(1, std::memory_order_relaxed);
        CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileQueued", CallOutcome::Ok, 0);
        const uint32_t max_workers = IsInProcessDxcAvailable() ? 4u : 2u;
        if (active_workers_ < max_workers) {
            ++active_workers_;
            workers_.emplace_back(&ShaderCache::WorkerLoop, this);
        }
    }
    async_cv_.notify_one();
}

void ShaderCache::WorkerLoop() {
    for (;;) {
        AsyncJob job{};
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            if (job_queue_.empty()) {
                --active_workers_;
                async_cv_.notify_all();
                return;
            }
            job = std::move(job_queue_.front());
            job_queue_.pop_front();
            ++active_jobs_;
        }

        std::vector<uint8_t> dxil;
        std::string diag;
        Status st = CompileRealXboxShaderSync(
            job.blob_bytes, job.stage, job.target_profile, job.request, dxil, &diag);
        if (st.ok()) {
            (void)Put(job.key, job.stage, job.target_profile, dxil);
            async_completed_.fetch_add(1, std::memory_order_relaxed);
            CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileCompleted", CallOutcome::Ok, 0);
        } else {
            async_failed_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                last_compile_error_ = "Async DXIL compile failed (key=" + job.key.ToHex() +
                                      ", target=" + job.target_profile + "): " +
                                      (diag.empty() ? st.message : diag);
            }
            CallTelemetry::Global().Record(kCacheTelemetryModule, "AsyncCompileCompleted", CallOutcome::Error, -1);
        }

        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            inflight_keys_.erase(job.key);
            --active_jobs_;
        }
        async_cv_.notify_all();
    }
}

bool ShaderCache::IsCompileReady(const ShaderCacheKey& key) const {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (memory_cache_.find(key) != memory_cache_.end()) {
            return true;
        }
    }
    std::error_code ec;
    return std::filesystem::exists(CacheFilePathForKey(key), ec);
}

void ShaderCache::WaitForPendingCompiles() {
    std::vector<std::thread> to_join;
    {
        std::unique_lock<std::mutex> lock(async_mutex_);
        async_cv_.wait(lock, [&]() {
            return job_queue_.empty() && active_jobs_ == 0u && active_workers_ == 0u;
        });
        to_join.swap(workers_);
    }
    for (auto& t : to_join) {
        if (t.joinable()) {
            t.join();
        }
    }
}

Status ShaderCache::GetOrLiftXboxShader(
    const void* blob,
    size_t blob_size,
    const ShaderCacheLiftRequest& request,
    CachedShaderResult& out_result,
    std::string* out_diagnostics) {
    out_result = CachedShaderResult{};
    if (!blob || blob_size < 4u) {
        const std::string err = "Empty Xbox shader bytecode";
        sync_failed_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            last_compile_error_ = err;
        }
        out_result.diagnostics = "ShaderCache FAIL: " + err;
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return Status::Error(StatusCode::InvalidArgument, err);
    }

    XboxParsedShaderContainer full_container{};
    if (auto st = ParseFullXboxShaderContainer(blob, blob_size, full_container); !st.ok()) {
        sync_failed_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            last_compile_error_ = "Xbox shader container parse failed: " + st.message;
        }
        out_result.diagnostics = "ShaderCache FAIL: " + st.message;
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return st;
    }
    if (!full_container.has_xs2) {
        const std::string err = "Input blob does not carry Xbox XS2/RDNA2 microcode";
        sync_failed_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            last_compile_error_ = err;
        }
        out_result.diagnostics = "ShaderCache FAIL: " + err;
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return Status::Error(StatusCode::UnsupportedFormat, err);
    }

    const ShaderStage stage = full_container.xs2.stage;
    const std::string target_profile = SelectTargetProfileForStage(stage, request.highest_shader_model);
    const uint32_t sm_flags =
        ((stage == ShaderStage::Mesh && request.mesh_has_payload) ? 1u : 0u) |
        ((full_container.xs2.wave_size == 64u) ? 2u : 0u) |
        ((request.highest_shader_model & 0xFFu) << 8) |
        ((static_cast<uint32_t>(stage) & 0xFFu) << 16);

    const ShaderCacheKey key = ComputeShaderCacheKey(
        blob, blob_size, target_profile, sm_flags, kShaderCacheFormatVersion);

    // 1. Check memory + disk cache
    if (TryGet(key, out_result)) {
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return Status::Ok();
    }

    // 2. Cache miss
    misses_.fetch_add(1, std::memory_order_relaxed);
    CallTelemetry::Global().Record(kCacheTelemetryModule, "CacheMiss", CallOutcome::Ok, 0);

    const auto* raw_ptr = static_cast<const uint8_t*>(blob);
    std::vector<uint8_t> blob_copy(raw_ptr, raw_ptr + blob_size);

    // 3. Optional async background compilation mode
    if (IsAsyncPsoCompilationEnabled()) {
        std::vector<uint8_t> standin_bc;
        std::string standin_diag;
        Status st = CompileStandInShader(
            stage, target_profile, request.mesh_has_payload,
            request.prefer_dxil_standin, request.dxc_path, standin_bc, &standin_diag);
        if (!st.ok()) {
            sync_failed_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                last_compile_error_ = "Stand-in shader compile failed (target=" + target_profile + "): " +
                                      (standin_diag.empty() ? st.message : standin_diag);
            }
            out_result.diagnostics = "ShaderCache MISS -> stand-in FAIL: " +
                                     (standin_diag.empty() ? st.message : standin_diag);
            if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
            return st;
        }

        AsyncJob job{};
        job.key = key;
        job.blob_bytes = std::move(blob_copy);
        job.stage = stage;
        job.target_profile = target_profile;
        job.request = request;
        EnqueueAsyncJob(std::move(job));
        standins_served_.fetch_add(1, std::memory_order_relaxed);

        out_result.key = key;
        out_result.stage = stage;
        out_result.target_profile = target_profile;
        out_result.dxil = std::move(standin_bc);
        out_result.is_standin_async = true;
        out_result.diagnostics = "ShaderCache MISS (async queued, stand-in served) key=" + key.ToHex() +
                                 " target=" + target_profile;
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return Status::Ok();
    }

    // 4. Synchronous lift + compile (in-process dxcompiler.dll preferred) + atomic disk cache store
    std::vector<uint8_t> dxil;
    std::string sync_diag;
    Status st = CompileRealXboxShaderSync(blob_copy, stage, target_profile, request, dxil, &sync_diag);
    if (!st.ok()) {
        sync_failed_.fetch_add(1, std::memory_order_relaxed);
        const std::string detail = sync_diag.empty() ? st.message : sync_diag;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            last_compile_error_ = "Sync DXIL compile failed (key=" + key.ToHex() +
                                  ", target=" + target_profile + "): " + detail;
        }
        out_result.diagnostics = "ShaderCache MISS -> compile FAIL (key=" + key.ToHex() +
                                 ", target=" + target_profile + "): " + detail;
        if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
        return st;
    }

    sync_succeeded_.fetch_add(1, std::memory_order_relaxed);
    (void)Put(key, stage, target_profile, dxil);

    out_result.key = key;
    out_result.stage = stage;
    out_result.target_profile = target_profile;
    out_result.dxil = std::move(dxil);
    out_result.diagnostics = "ShaderCache MISS -> compiled & cached (key=" + key.ToHex() +
                             ", target=" + target_profile + ", bytes=" + std::to_string(out_result.dxil.size()) + ")";
    if (!sync_diag.empty()) {
        out_result.diagnostics += " | " + sync_diag;
    }
    if (out_diagnostics) *out_diagnostics = out_result.diagnostics;
    return Status::Ok();
}

}  // namespace openscarlett
