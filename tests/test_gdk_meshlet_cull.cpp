// OpenScarlett Wave 2 — Portable End-to-End Live Test Harness for MeshletCull:
//   - Stages a zero-DLL Xbox package folder under %TEMP%\openscarlett_wave2_meshlet_cull\
//     with genuine MicrosoftGame.Config (TitleId = 0x5CA01002) and standalone gdk_meshlet_cull.exe.
//   - Compiles live HLSL shaders via dxc.exe:
//       * HW Mesh Shader path: as_6_5 (6-plane frustum + normal-cone culling + Wave compaction + UAV stats),
//                              ms_6_5 (9 verts / 6 tris per surviving meshlet), ps_6_5 (directional shading)
//       * WARP Fallback path:  cs_6_2 (identical frustum + normal-cone culling + UAV stats),
//                              vs_6_2 + ps_6_2 (identical 3D pyramid geometry + D32_FLOAT depth test)
//   - Launches `openscarlett.exe run <package_dir>` on Hardware GPU (asserting mode=hw_as_ms,
//     ID3D12DeviceX::CreatePipelineState slot 47, ID3D12GraphicsCommandListX::DispatchMesh slot 79
//     when host has MeshTier >= 10) AND on D3D12 WARP (`-- --warp`, asserting mode=warp_fallback
//     with identical culling counts), verifying not_impl == 0 and 100% byte-identical <package_dir>.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/loader/game_config.h"
#include "openscarlett/loader/pe_inspector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kMeshletCullAsHlsl = R"HLSL(
#define MESHLET_CULL_RS \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "SRV(t0, visibility=SHADER_VISIBILITY_ALL)," \
    "UAV(u0, visibility=SHADER_VISIBILITY_ALL)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct MeshletDesc
{
    float3 Center;
    float  Radius;
    float3 ConeAxis;
    float  ConeCutoff;
    float3 Color;
    float  HalfExtent;
};

StructuredBuffer<MeshletDesc> Meshlets  : register(t0);
RWStructuredBuffer<uint>      CullStats : register(u0);

struct Payload
{
    uint MeshletIndices[32];
};

groupshared Payload s_Payload;
groupshared uint    s_VisibleCount;

bool IsInsideFrustum(float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        if (dot(FrustumPlanes[i].xyz, center) + FrustumPlanes[i].w < -radius)
        {
            return false;
        }
    }
    return true;
}

bool IsConeBackfacing(float3 center, float3 coneAxis, float coneCutoff)
{
    float3 viewDir = normalize(center - CameraPos.xyz);
    return dot(coneAxis, viewDir) >= coneCutoff;
}

[RootSignature(MESHLET_CULL_RS)]
[numthreads(32, 1, 1)]
void main(uint gtid : SV_GroupThreadID)
{
    if (gtid == 0)
    {
        s_VisibleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    bool visible = false;
    if (gtid < MeshletCount)
    {
        InterlockedAdd(CullStats[3], 1);
        MeshletDesc m = Meshlets[gtid];
        if (!IsInsideFrustum(m.Center, m.Radius))
        {
            InterlockedAdd(CullStats[1], 1);
        }
        else if (IsConeBackfacing(m.Center, m.ConeAxis, m.ConeCutoff))
        {
            InterlockedAdd(CullStats[2], 1);
        }
        else
        {
            visible = true;
            InterlockedAdd(CullStats[0], 1);
        }
    }

    uint laneOffset = WavePrefixCountBits(visible);
    uint waveCount  = WaveActiveCountBits(visible);
    uint waveBase   = 0;
    if (WaveIsFirstLane() && waveCount > 0)
    {
        InterlockedAdd(s_VisibleCount, waveCount, waveBase);
    }
    waveBase = WaveReadLaneFirst(waveBase);
    if (visible)
    {
        s_Payload.MeshletIndices[waveBase + laneOffset] = gtid;
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_VisibleCount, 1, 1, s_Payload);
}
)HLSL";

constexpr const char* kMeshletCullMsHlsl = R"HLSL(
#define MESHLET_CULL_RS \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "SRV(t0, visibility=SHADER_VISIBILITY_ALL)," \
    "UAV(u0, visibility=SHADER_VISIBILITY_ALL)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct MeshletDesc
{
    float3 Center;
    float  Radius;
    float3 ConeAxis;
    float  ConeCutoff;
    float3 Color;
    float  HalfExtent;
};

StructuredBuffer<MeshletDesc> Meshlets : register(t0);

struct Payload
{
    uint MeshletIndices[32];
};

struct VertexOut
{
    float4 Position : SV_Position;
    float3 Color    : COLOR0;
    float3 Normal   : NORMAL0;
};

float4 MulViewProj(float3 worldPos)
{
    float4 p = float4(worldPos, 1.0f);
    return float4(
        dot(ViewProj[0], p),
        dot(ViewProj[1], p),
        dot(ViewProj[2], p),
        dot(ViewProj[3], p));
}

void BuildMeshletVertex(
    MeshletDesc m,
    uint vIdx,
    out float3 outPos,
    out float3 outNormal,
    out float3 outColor)
{
    float h = m.HalfExtent;
    float apexSign = (m.ConeAxis.z < 0.0f) ? -1.0f : 1.0f;
    float3 c = m.Center;

    if (vIdx < 5)
    {
        outColor = m.Color;
        if (vIdx == 0)
        {
            outPos    = c + float3(-h, -h, 0.0f);
            outNormal = normalize(float3(-0.35f, -0.35f, apexSign));
        }
        else if (vIdx == 1)
        {
            outPos    = c + float3( h, -h, 0.0f);
            outNormal = normalize(float3( 0.35f, -0.35f, apexSign));
        }
        else if (vIdx == 2)
        {
            outPos    = c + float3( h,  h, 0.0f);
            outNormal = normalize(float3( 0.35f,  0.35f, apexSign));
        }
        else if (vIdx == 3)
        {
            outPos    = c + float3(-h,  h, 0.0f);
            outNormal = normalize(float3(-0.35f,  0.35f, apexSign));
        }
        else
        {
            outPos    = c + float3(0.0f, 0.0f, apexSign * 0.22f);
            outNormal = float3(0.0f, 0.0f, apexSign);
        }
    }
    else
    {
        // Occluded dark back-plate at z = c.z + 0.30 aligned along camera ray through center.
        // Verifies D32_FLOAT depth buffer rejects geometry behind the front pyramid.
        float dz = 0.30f;
        float vz = max(c.z - CameraPos.z, 0.5f);
        float scale = (vz + dz) / vz;
        float3 backCenter = float3(
            CameraPos.x + (c.x - CameraPos.x) * scale,
            CameraPos.y + (c.y - CameraPos.y) * scale,
            c.z + dz);
        float bh = 0.20f;
        uint bIdx = vIdx - 5;
        float sx = (bIdx == 1 || bIdx == 2) ? bh : -bh;
        float sy = (bIdx == 2 || bIdx == 3) ? bh : -bh;
        outPos    = backCenter + float3(sx, sy, 0.0f);
        outNormal = float3(0.0f, 0.0f, -1.0f);
        outColor  = float3(0.05f, 0.05f, 0.05f);
    }
}

static const uint3 kMeshletTris[6] = {
    uint3(0, 4, 1),
    uint3(1, 4, 2),
    uint3(2, 4, 3),
    uint3(3, 4, 0),
    uint3(5, 8, 6),
    uint3(6, 8, 7)
};

[RootSignature(MESHLET_CULL_RS)]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in payload Payload payload,
    out indices uint3 tris[6],
    out vertices VertexOut verts[9])
{
    SetMeshOutputCounts(9, 6);

    uint meshletIdx = payload.MeshletIndices[gid];
    MeshletDesc m = Meshlets[meshletIdx];

    if (gtid < 9)
    {
        float3 pos, nrm, col;
        BuildMeshletVertex(m, gtid, pos, nrm, col);
        verts[gtid].Position = MulViewProj(pos);
        verts[gtid].Color    = col;
        verts[gtid].Normal   = nrm;
    }

    if (gtid < 6)
    {
        tris[gtid] = kMeshletTris[gtid];
    }
}
)HLSL";

constexpr const char* kMeshletCullPsHlsl = R"HLSL(
#define MESHLET_CULL_RS \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "SRV(t0, visibility=SHADER_VISIBILITY_ALL)," \
    "UAV(u0, visibility=SHADER_VISIBILITY_ALL)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct VertexOut
{
    float4 Position : SV_Position;
    float3 Color    : COLOR0;
    float3 Normal   : NORMAL0;
};

[RootSignature(MESHLET_CULL_RS)]
float4 main(VertexOut input) : SV_TARGET0
{
    float3 n = normalize(input.Normal);
    float ndotl = saturate(dot(n, normalize(LightDir.xyz)));
    float shade = 0.35f + 0.65f * ndotl;
    return float4(saturate(input.Color * shade), 1.0f);
}
)HLSL";

constexpr const char* kFallbackCullCsHlsl = R"HLSL(
#define FALLBACK_CS_RS "RootFlags(0),CBV(b0),SRV(t0),UAV(u0),UAV(u1)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct MeshletDesc
{
    float3 Center;
    float  Radius;
    float3 ConeAxis;
    float  ConeCutoff;
    float3 Color;
    float  HalfExtent;
};

StructuredBuffer<MeshletDesc> Meshlets       : register(t0);
RWStructuredBuffer<uint>      CullStats      : register(u0);
RWStructuredBuffer<uint>      VisibleIndices : register(u1);

bool IsInsideFrustum(float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        if (dot(FrustumPlanes[i].xyz, center) + FrustumPlanes[i].w < -radius)
        {
            return false;
        }
    }
    return true;
}

bool IsConeBackfacing(float3 center, float3 coneAxis, float coneCutoff)
{
    float3 viewDir = normalize(center - CameraPos.xyz);
    return dot(coneAxis, viewDir) >= coneCutoff;
}

[RootSignature(FALLBACK_CS_RS)]
[numthreads(32, 1, 1)]
void main(uint gtid : SV_GroupThreadID)
{
    if (gtid >= MeshletCount)
    {
        return;
    }

    InterlockedAdd(CullStats[3], 1);
    MeshletDesc m = Meshlets[gtid];
    if (!IsInsideFrustum(m.Center, m.Radius))
    {
        InterlockedAdd(CullStats[1], 1);
    }
    else if (IsConeBackfacing(m.Center, m.ConeAxis, m.ConeCutoff))
    {
        InterlockedAdd(CullStats[2], 1);
    }
    else
    {
        uint slot = 0;
        InterlockedAdd(CullStats[0], 1, slot);
        VisibleIndices[slot] = gtid;
    }
}
)HLSL";

constexpr const char* kFallbackDrawVsHlsl = R"HLSL(
#define FALLBACK_GFX_RS \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "SRV(t0, visibility=SHADER_VISIBILITY_VERTEX)," \
    "SRV(t1, visibility=SHADER_VISIBILITY_VERTEX)," \
    "SRV(t2, visibility=SHADER_VISIBILITY_VERTEX)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct MeshletDesc
{
    float3 Center;
    float  Radius;
    float3 ConeAxis;
    float  ConeCutoff;
    float3 Color;
    float  HalfExtent;
};

StructuredBuffer<MeshletDesc> Meshlets       : register(t0);
StructuredBuffer<uint>        VisibleIndices : register(t1);
StructuredBuffer<uint>        CullStats      : register(t2);

struct VertexOut
{
    float4 Position : SV_Position;
    float3 Color    : COLOR0;
    float3 Normal   : NORMAL0;
};

float4 MulViewProj(float3 worldPos)
{
    float4 p = float4(worldPos, 1.0f);
    return float4(
        dot(ViewProj[0], p),
        dot(ViewProj[1], p),
        dot(ViewProj[2], p),
        dot(ViewProj[3], p));
}

void BuildMeshletVertex(
    MeshletDesc m,
    uint vIdx,
    out float3 outPos,
    out float3 outNormal,
    out float3 outColor)
{
    float h = m.HalfExtent;
    float apexSign = (m.ConeAxis.z < 0.0f) ? -1.0f : 1.0f;
    float3 c = m.Center;

    if (vIdx < 5)
    {
        outColor = m.Color;
        if (vIdx == 0)
        {
            outPos    = c + float3(-h, -h, 0.0f);
            outNormal = normalize(float3(-0.35f, -0.35f, apexSign));
        }
        else if (vIdx == 1)
        {
            outPos    = c + float3( h, -h, 0.0f);
            outNormal = normalize(float3( 0.35f, -0.35f, apexSign));
        }
        else if (vIdx == 2)
        {
            outPos    = c + float3( h,  h, 0.0f);
            outNormal = normalize(float3( 0.35f,  0.35f, apexSign));
        }
        else if (vIdx == 3)
        {
            outPos    = c + float3(-h,  h, 0.0f);
            outNormal = normalize(float3(-0.35f,  0.35f, apexSign));
        }
        else
        {
            outPos    = c + float3(0.0f, 0.0f, apexSign * 0.22f);
            outNormal = float3(0.0f, 0.0f, apexSign);
        }
    }
    else
    {
        float dz = 0.30f;
        float vz = max(c.z - CameraPos.z, 0.5f);
        float scale = (vz + dz) / vz;
        float3 backCenter = float3(
            CameraPos.x + (c.x - CameraPos.x) * scale,
            CameraPos.y + (c.y - CameraPos.y) * scale,
            c.z + dz);
        float bh = 0.20f;
        uint bIdx = vIdx - 5;
        float sx = (bIdx == 1 || bIdx == 2) ? bh : -bh;
        float sy = (bIdx == 2 || bIdx == 3) ? bh : -bh;
        outPos    = backCenter + float3(sx, sy, 0.0f);
        outNormal = float3(0.0f, 0.0f, -1.0f);
        outColor  = float3(0.05f, 0.05f, 0.05f);
    }
}

uint MapFallbackVertexId(uint vid)
{
    if (vid < 12)
    {
        uint tri = vid / 3;
        uint corner = vid - tri * 3;
        if (corner == 1) return 4;
        return (corner == 0) ? tri : ((tri + 1) & 3);
    }
    if (vid == 12) return 5;
    if (vid == 13 || vid == 16) return 8;
    if (vid == 14 || vid == 15) return 6;
    return 7;
}

[RootSignature(FALLBACK_GFX_RS)]
VertexOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VertexOut o;
    uint visibleCount = CullStats[0];
    if (iid >= visibleCount || vid >= 18)
    {
        o.Position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        o.Color    = float3(0.0f, 0.0f, 0.0f);
        o.Normal   = float3(0.0f, 0.0f, -1.0f);
        return o;
    }

    uint meshletIdx = VisibleIndices[iid];
    MeshletDesc m = Meshlets[meshletIdx];

    float3 pos, nrm, col;
    BuildMeshletVertex(m, MapFallbackVertexId(vid), pos, nrm, col);
    o.Position = MulViewProj(pos);
    o.Color    = col;
    o.Normal   = nrm;
    return o;
}
)HLSL";

constexpr const char* kFallbackDrawPsHlsl = R"HLSL(
#define FALLBACK_GFX_RS \
    "RootFlags(0)," \
    "CBV(b0, visibility=SHADER_VISIBILITY_ALL)," \
    "SRV(t0, visibility=SHADER_VISIBILITY_VERTEX)," \
    "SRV(t1, visibility=SHADER_VISIBILITY_VERTEX)," \
    "SRV(t2, visibility=SHADER_VISIBILITY_VERTEX)"

cbuffer FrameConstants : register(b0)
{
    float4 ViewProj[4];
    float4 FrustumPlanes[6];
    float4 CameraPos;
    float4 LightDir;
    uint   MeshletCount;
    uint3  _Pad0;
};

struct VertexOut
{
    float4 Position : SV_Position;
    float3 Color    : COLOR0;
    float3 Normal   : NORMAL0;
};

[RootSignature(FALLBACK_GFX_RS)]
float4 main(VertexOut input) : SV_TARGET0
{
    float3 n = normalize(input.Normal);
    float ndotl = saturate(dot(n, normalize(LightDir.xyz)));
    float shade = 0.35f + 0.65f * ndotl;
    return float4(saturate(input.Color * shade), 1.0f);
}
)HLSL";

constexpr const char* kMicrosoftGameConfigXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Game configVersion=\"1\">\n"
    "  <Identity Name=\"OpenScarlett.Wave2.MeshletCull\"\n"
    "            Publisher=\"CN=OpenScarlett\"\n"
    "            Version=\"1.0.0.0\" />\n"
    "  <TitleId>0x5CA01002</TitleId>\n"
    "  <ShellVisuals DefaultDisplayName=\"OpenScarlett MeshletCull (AS+MS+PS)\" />\n"
    "  <ExecutableList>\n"
    "    <Executable Name=\"gdk_meshlet_cull.exe\"\n"
    "                Id=\"Game\"\n"
    "                TargetDeviceFamily=\"Scarlett\" />\n"
    "  </ExecutableList>\n"
    "</Game>\n";

struct TelemetryRow {
    std::string module;
    std::string symbol;
    int32_t slot = -1;
    uint64_t total = 0;
    uint64_t ok = 0;
    uint64_t stub = 0;
    uint64_t not_impl = 0;
    uint64_t errors = 0;
};

struct TelemetrySummary {
    std::vector<TelemetryRow> rows;
    std::set<std::string> modules;
    uint64_t total_calls = 0;
    uint64_t total_not_impl = 0;
    uint64_t total_errors = 0;

    bool HasSymbol(const std::string& sym, int32_t expected_slot = -999) const {
        for (const auto& r : rows) {
            if (r.symbol == sym) {
                if (expected_slot == -999 || r.slot == expected_slot) {
                    return r.total >= 1u;
                }
            }
        }
        return false;
    }

    bool HasModule(const std::string& mod) const {
        return modules.find(mod) != modules.end();
    }
};

struct FileDigest {
    std::string rel_path;
    uint64_t byte_size = 0;
    uint64_t fnv1a_hash = 0;

    bool operator==(const FileDigest& o) const noexcept {
        return rel_path == o.rel_path && byte_size == o.byte_size && fnv1a_hash == o.fnv1a_hash;
    }
};

uint64_t Fnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    OS_REQUIRE(static_cast<bool>(ifs));
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::vector<uint8_t> ReadBinaryFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    OS_REQUIRE(static_cast<bool>(ifs));
    const std::streamoff sz = ifs.tellg();
    OS_REQUIRE(sz >= 0);
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        OS_REQUIRE(static_cast<bool>(ifs));
    }
    return buf;
}

void WriteBinaryFile(const fs::path& p, const void* data, size_t size) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    OS_REQUIRE(static_cast<bool>(ofs));
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    OS_REQUIRE(static_cast<bool>(ofs));
}

std::map<std::string, std::string> ParseKeyValueFile(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::istringstream iss(ReadTextFile(p));
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

std::vector<FileDigest> SnapshotDirectoryBytes(const fs::path& dir) {
    std::vector<FileDigest> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto bytes = ReadBinaryFile(entry.path());
        FileDigest d{};
        d.rel_path = fs::relative(entry.path(), dir).generic_string();
        d.byte_size = static_cast<uint64_t>(bytes.size());
        d.fnv1a_hash = Fnv1a64(bytes.data(), bytes.size());
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const FileDigest& a, const FileDigest& b) {
        return a.rel_path < b.rel_path;
    });
    return out;
}

void AssertNoDllsInDir(const fs::path& dir) {
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(ext != ".dll");
    }
}

fs::path ResolveRuntimeBinDir() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const fs::path exe_dir = fs::path(self_buf).parent_path();
        if (fs::exists(exe_dir / L"openscarlett.exe", ec) &&
            fs::exists(exe_dir / L"openscarlett_hook.dll", ec) &&
            fs::exists(exe_dir / L"gdk_meshlet_cull.exe", ec)) {
            return exe_dir;
        }
    }
    return fs::path(OPENSCARLETT_BINARY_DIR);
}

fs::path ResolveScratchDir(const wchar_t* subdir_name) {
    std::error_code ec;
    wchar_t temp_buf[1024]{};
    const DWORD tlen = ::GetEnvironmentVariableW(L"TEMP", temp_buf, static_cast<DWORD>(1024));
    if (tlen > 0 && tlen < 1024) {
        return fs::path(temp_buf) / subdir_name;
    }
    const fs::path sys_tmp = fs::temp_directory_path(ec);
    if (!ec && !sys_tmp.empty()) {
        return sys_tmp / subdir_name;
    }
    return ResolveRuntimeBinDir() / subdir_name;
}

fs::path FindDxcExe() {
    std::error_code ec;
    wchar_t self_buf[MAX_PATH]{};
    const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
    if (self_len > 0 && self_len < MAX_PATH) {
        const auto beside = fs::path(self_buf).parent_path() / L"dxc.exe";
        if (fs::exists(beside, ec)) return beside;
    }
    wchar_t env_dxc[1024]{};
    const DWORD env_len = ::GetEnvironmentVariableW(L"OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(1024));
    if (env_len > 0 && env_len < 1024 && fs::exists(env_dxc, ec)) {
        return fs::path(env_dxc);
    }
    wchar_t path_buf[MAX_PATH]{};
    const DWORD path_len = ::SearchPathW(nullptr, L"dxc.exe", nullptr, static_cast<DWORD>(MAX_PATH), path_buf, nullptr);
    if (path_len > 0 && path_len < MAX_PATH && fs::exists(path_buf, ec)) {
        return fs::path(path_buf);
    }
    const wchar_t* candidates[] = {
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
        LR"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
    };
    for (const wchar_t* p : candidates) {
        if (fs::exists(p, ec)) return fs::path(p);
    }
    const fs::path kits_bin(LR"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (fs::exists(kits_bin, ec)) {
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            const auto cand = entry.path() / L"x64" / L"dxc.exe";
            if (fs::exists(cand, ec)) found.push_back(cand);
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.back();
    }
    return {};
}

DWORD RunProcessW(const fs::path& exe_path, const std::wstring& cmd_line, DWORD timeout_ms = 45000) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        exe_path.c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);
    OS_REQUIRE(ok != 0);
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, timeout_ms);
    OS_REQUIRE(wr == WAIT_OBJECT_0);
    DWORD exit_code = 999;
    OS_REQUIRE(::GetExitCodeProcess(pi.hProcess, &exit_code) != 0);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return exit_code;
}

void CompileShaderWithDxc(
    const fs::path& dxc_exe,
    const fs::path& hlsl_path,
    const fs::path& cso_out_path,
    const std::wstring& profile,
    const std::wstring& entry_point) {
    const std::wstring cmd = L"\"" + dxc_exe.wstring() + L"\" -T " + profile +
                             L" -E " + entry_point + L" -O3" +
                             L" -Fo \"" + cso_out_path.wstring() + L"\" \"" + hlsl_path.wstring() + L"\"";
    const DWORD rc = RunProcessW(dxc_exe, cmd, 30000);
    OS_REQUIRE(rc == 0);
    OS_REQUIRE(fs::exists(cso_out_path));
    OS_REQUIRE(fs::file_size(cso_out_path) > 64u);
}

bool ExtractJsonString(const std::string& line, const std::string& key, std::string& out) {
    const std::string pat = "\"" + key + "\": \"";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    std::string val;
    while (pos < line.size()) {
        char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            val.push_back(line[pos++]);
        } else if (c == '"') {
            out = std::move(val);
            return true;
        } else {
            val.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonInt(const std::string& line, const std::string& key, int64_t& out) {
    const std::string pat = "\"" + key + "\": ";
    size_t pos = line.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    char* end_ptr = nullptr;
    const long long v = std::strtoll(line.c_str() + pos, &end_ptr, 10);
    if (end_ptr == line.c_str() + pos) return false;
    out = static_cast<int64_t>(v);
    return true;
}

TelemetrySummary ParseTelemetryJson(const fs::path& json_path) {
    OS_REQUIRE(fs::exists(json_path));
    TelemetrySummary sum;
    std::istringstream iss(ReadTextFile(json_path));
    std::string line;
    while (std::getline(iss, line)) {
        TelemetryRow row{};
        if (!ExtractJsonString(line, "module", row.module) ||
            !ExtractJsonString(line, "symbol", row.symbol)) {
            continue;
        }
        int64_t v = 0;
        if (ExtractJsonInt(line, "slot", v)) row.slot = static_cast<int32_t>(v);
        if (ExtractJsonInt(line, "total", v)) row.total = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "ok", v)) row.ok = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "stub", v)) row.stub = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "not_impl", v)) row.not_impl = static_cast<uint64_t>(v);
        if (ExtractJsonInt(line, "errors", v)) row.errors = static_cast<uint64_t>(v);

        sum.modules.insert(row.module);
        sum.total_calls += row.total;
        sum.total_not_impl += row.not_impl;
        sum.total_errors += row.errors;
        sum.rows.push_back(std::move(row));
    }
    return sum;
}

DWORD LaunchPackageViaOpenScarlettCli(
    const fs::path& cli_exe,
    const fs::path& package_dir,
    const fs::path& shim_dir,
    const fs::path& vfs_t,
    const fs::path& vfs_s,
    const fs::path& telemetry_json,
    const std::wstring& guest_args = {}) {
    // Deliberately pass <package_dir> directly (NO --vfs-g) so openscarlett.exe parses
    // MicrosoftGame.Config, resolves gdk_meshlet_cull.exe + TitleId 0x5CA01002, and mounts
    // <package_dir> as read-only G:\ with per-TitleId CoW overlay!
    std::wstring cmd =
        L"\"" + cli_exe.wstring() + L"\" run \"" + package_dir.wstring() +
        L"\" --shim-dir \"" + shim_dir.wstring() +
        L"\" --vfs-t \"" + vfs_t.wstring() +
        L"\" --vfs-s \"" + vfs_s.wstring() +
        L"\" --telemetry \"" + telemetry_json.wstring() +
        L"\" --timeout-ms 45000";
    if (!guest_args.empty()) {
        cmd += L" -- " + guest_args;
    }
    return RunProcessW(cli_exe, cmd, 50000);
}

std::vector<uint8_t> VerifyAndReadPpmRgb(const fs::path& ppm_path, uint32_t width, uint32_t height) {
    OS_REQUIRE(fs::exists(ppm_path));
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const auto raw = ReadBinaryFile(ppm_path);
    const size_t expected_size = header.size() + static_cast<size_t>(width) * height * 3u;
    OS_REQUIRE(raw.size() == expected_size);
    OS_REQUIRE(std::memcmp(raw.data(), header.data(), header.size()) == 0);
    return std::vector<uint8_t>(raw.begin() + static_cast<std::ptrdiff_t>(header.size()), raw.end());
}

void VerifyStandaloneXboxPeImports(const fs::path& exe_path, const std::vector<std::string>& required_dlls) {
    openscarlett::PeInspectionReport rep;
    OS_REQUIRE(openscarlett::InspectPeFile(exe_path.string(), rep).ok());
    OS_REQUIRE(!rep.is_dll);
    OS_REQUIRE(rep.console_imports + rep.gdk_imports > 0u);

    std::set<std::string> imported_lower;
    for (const auto& mod : rep.imports) {
        std::string lo = mod.dll_name;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(lo != "openscarlett_hook.dll");
        imported_lower.insert(lo);
    }
    for (const auto& req : required_dlls) {
        std::string lo = req;
        std::transform(lo.begin(), lo.end(), lo.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        OS_REQUIRE(imported_lower.find(lo) != imported_lower.end());
    }
}

}  // namespace

int main() {
    // Clear any inherited VFS/TitleId overrides so `openscarlett run <package_dir>` strictly
    // derives G:\, per-TitleId CoW overlay, and TitleId=0x5CA01002 from MicrosoftGame.Config!
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_VFS_G_OVERLAY", nullptr);
    ::SetEnvironmentVariableW(L"OPENSCARLETT_TITLE_ID", nullptr);

    const fs::path bin_dir = ResolveRuntimeBinDir();
    const fs::path cli_exe = bin_dir / "openscarlett.exe";
    const fs::path dxc_exe = FindDxcExe();
    OS_REQUIRE(fs::exists(cli_exe));
    OS_REQUIRE(fs::exists(bin_dir / "openscarlett_hook.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "d3d12_xs.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "xgameruntime.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "pixevt.dll"));
    OS_REQUIRE(fs::exists(bin_dir / "gdk_meshlet_cull.exe"));
    OS_REQUIRE(!dxc_exe.empty());

    // Probe host hardware D3D12 adapters & Mesh Shader Tier (GTX 1650 Ti = MeshTier 10; CI = WARP)
    bool has_hw_gpu = false;
    bool has_hw_mesh = false;
    {
        std::vector<openscarlett::D3D12AdapterInfo> adapters;
        if (openscarlett::EnumerateD3D12Adapters(adapters).ok()) {
            for (const auto& a : adapters) {
                if (!a.is_warp) {
                    has_hw_gpu = true;
                    if (a.mesh_shader_tier >= 10u) {
                        has_hw_mesh = true;
                    }
                }
            }
        }
    }

    const fs::path stage_root = ResolveScratchDir(L"openscarlett_wave2_meshlet_cull");
    std::error_code ec;
    fs::remove_all(stage_root, ec);
    fs::create_directories(stage_root, ec);
    OS_REQUIRE(fs::exists(stage_root));

    const fs::path hlsl_dir = stage_root / "_hlsl";
    const fs::path pkg_dir = stage_root / "MeshletCull_Pkg";
    fs::create_directories(hlsl_dir, ec);
    fs::create_directories(pkg_dir, ec);

    // 1. Stage standalone Xbox-ABI binary + MicrosoftGame.Config into zero-DLL <package_dir>
    const fs::path staged_exe = pkg_dir / "gdk_meshlet_cull.exe";
    fs::copy_file(bin_dir / "gdk_meshlet_cull.exe", staged_exe, fs::copy_options::overwrite_existing);
    VerifyStandaloneXboxPeImports(staged_exe, {"d3d12_xs.dll", "xgameruntime.dll", "pixevt.dll"});

    WriteBinaryFile(pkg_dir / "MicrosoftGame.Config",
                    kMicrosoftGameConfigXml, std::strlen(kMicrosoftGameConfigXml));

    // Verify MicrosoftGame.Config parses and resolves to gdk_meshlet_cull.exe + TitleId 0x5CA01002
    {
        openscarlett::ResolvedPackageTarget target{};
        OS_REQUIRE(openscarlett::ResolvePackageLaunchTarget(pkg_dir.string(), target).ok());
        OS_REQUIRE(target.has_config);
        OS_REQUIRE(target.config.has_title_id);
        OS_REQUIRE(target.config.title_id_value == 0x5CA01002u);
        OS_REQUIRE(fs::path(target.exe_path).filename() == "gdk_meshlet_cull.exe");
    }

    // 2. Compile ALL 6 HLSL shaders (HW as_6_5/ms_6_5/ps_6_5 + WARP cs_6_2/vs_6_2/ps_6_2) into <package_dir>
    WriteBinaryFile(hlsl_dir / "MeshletCullAS.hlsl", kMeshletCullAsHlsl, std::strlen(kMeshletCullAsHlsl));
    WriteBinaryFile(hlsl_dir / "MeshletCullMS.hlsl", kMeshletCullMsHlsl, std::strlen(kMeshletCullMsHlsl));
    WriteBinaryFile(hlsl_dir / "MeshletCullPS.hlsl", kMeshletCullPsHlsl, std::strlen(kMeshletCullPsHlsl));
    WriteBinaryFile(hlsl_dir / "MeshletCullFallbackCS.hlsl", kFallbackCullCsHlsl, std::strlen(kFallbackCullCsHlsl));
    WriteBinaryFile(hlsl_dir / "MeshletCullFallbackVS.hlsl", kFallbackDrawVsHlsl, std::strlen(kFallbackDrawVsHlsl));
    WriteBinaryFile(hlsl_dir / "MeshletCullFallbackPS.hlsl", kFallbackDrawPsHlsl, std::strlen(kFallbackDrawPsHlsl));

    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullAS.hlsl",
                         pkg_dir / "MeshletCullAS.cso", L"as_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullMS.hlsl",
                         pkg_dir / "MeshletCullMS.cso", L"ms_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullPS.hlsl",
                         pkg_dir / "MeshletCullPS.cso", L"ps_6_5", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullFallbackCS.hlsl",
                         pkg_dir / "MeshletCullFallbackCS.cso", L"cs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullFallbackVS.hlsl",
                         pkg_dir / "MeshletCullFallbackVS.cso", L"vs_6_2", L"main");
    CompileShaderWithDxc(dxc_exe, hlsl_dir / "MeshletCullFallbackPS.hlsl",
                         pkg_dir / "MeshletCullFallbackPS.cso", L"ps_6_2", L"main");

    AssertNoDllsInDir(pkg_dir);
    const auto pkg_snapshot_before = SnapshotDirectoryBytes(pkg_dir);
    OS_REQUIRE(pkg_snapshot_before.size() == 8u);  // exe + config + 6 cso files

    // 3. Run 1: Default Launch via `openscarlett.exe run <package_dir>` (HW AS+MS+PS when MeshTier>=10!)
    const fs::path hw_t = stage_root / "run_hw_T";
    const fs::path hw_s = stage_root / "run_hw_S";
    const fs::path hw_tel = stage_root / "telemetry_hw.json";
    const auto t0_hw = std::chrono::steady_clock::now();
    const DWORD hw_rc = LaunchPackageViaOpenScarlettCli(cli_exe, pkg_dir, bin_dir, hw_t, hw_s, hw_tel);
    const double hw_elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_hw).count();
    OS_REQUIRE(hw_rc == 0);

    // Strictly verify <package_dir> is 100% byte-identical and still contains zero DLLs!
    AssertNoDllsInDir(pkg_dir);
    OS_REQUIRE(SnapshotDirectoryBytes(pkg_dir) == pkg_snapshot_before);

    const auto hw_ppm_rgb = VerifyAndReadPpmRgb(hw_t / "meshlet_cull.ppm", 256u, 256u);
    const auto hw_metrics = ParseKeyValueFile(hw_t / "metrics.txt");
    const auto hw_tel_sum = ParseTelemetryJson(hw_tel);

    OS_REQUIRE(hw_tel_sum.total_not_impl == 0u);
    OS_REQUIRE(hw_tel_sum.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(hw_tel_sum.HasModule("xgameruntime.dll"));
    OS_REQUIRE(hw_tel_sum.HasModule("pixevt.dll"));
    OS_REQUIRE(hw_tel_sum.HasModule("vfs"));
    OS_REQUIRE(hw_tel_sum.HasSymbol("XGameGetXboxTitleId"));
    OS_REQUIRE(hw_tel_sum.HasSymbol("PIXBeginEventOnCommandList"));
    OS_REQUIRE(hw_tel_sum.HasSymbol("PIXReportCounter"));
    OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12DeviceX::CreateDepthStencilView", 21));
    OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47));
    OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

    OS_REQUIRE(hw_metrics.at("title_id") == "0x5CA01002");
    OS_REQUIRE(hw_metrics.at("total_meshlets") == "16");
    OS_REQUIRE(hw_metrics.at("surviving") == "6");
    OS_REQUIRE(hw_metrics.at("frustum_culled") == "6");
    OS_REQUIRE(hw_metrics.at("cone_culled") == "4");

    if (has_hw_mesh) {
        OS_REQUIRE(hw_metrics.at("mode") == "hw_as_ms");
        OS_REQUIRE(hw_metrics.at("is_warp") == "0");
        OS_REQUIRE(std::stoul(hw_metrics.at("mesh_tier")) >= 10u);
        OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12DeviceX::CreatePipelineState", 47));
        OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DispatchMesh", 79));
    } else {
        // WARP-only CI runner or non-MeshTier GPU gracefully executes fallback path
        OS_REQUIRE(hw_metrics.at("mode") == "warp_fallback");
        OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::Dispatch", 14));
        OS_REQUIRE(hw_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));
    }

    // 4. Run 2: Explicit D3D12 WARP (`openscarlett.exe run <package_dir> ... -- --warp`)
    const fs::path wp_t = stage_root / "run_warp_T";
    const fs::path wp_s = stage_root / "run_warp_S";
    const fs::path wp_tel = stage_root / "telemetry_warp.json";
    const auto t0_wp = std::chrono::steady_clock::now();
    const DWORD wp_rc = LaunchPackageViaOpenScarlettCli(cli_exe, pkg_dir, bin_dir, wp_t, wp_s, wp_tel, L"--warp");
    const double wp_elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_wp).count();
    OS_REQUIRE(wp_rc == 0);

    // Strictly verify <package_dir> is STILL 100% byte-identical and zero-DLL!
    AssertNoDllsInDir(pkg_dir);
    OS_REQUIRE(SnapshotDirectoryBytes(pkg_dir) == pkg_snapshot_before);

    const auto wp_ppm_rgb = VerifyAndReadPpmRgb(wp_t / "meshlet_cull.ppm", 256u, 256u);
    const auto wp_metrics = ParseKeyValueFile(wp_t / "metrics.txt");
    const auto wp_tel_sum = ParseTelemetryJson(wp_tel);

    OS_REQUIRE(wp_tel_sum.total_not_impl == 0u);
    OS_REQUIRE(wp_tel_sum.HasModule("d3d12_xs.dll"));
    OS_REQUIRE(wp_tel_sum.HasModule("xgameruntime.dll"));
    OS_REQUIRE(wp_tel_sum.HasModule("pixevt.dll"));
    OS_REQUIRE(wp_tel_sum.HasModule("vfs"));
    OS_REQUIRE(wp_tel_sum.HasSymbol("XGameGetXboxTitleId"));
    OS_REQUIRE(wp_tel_sum.HasSymbol("PIXBeginEventOnCommandList"));
    OS_REQUIRE(wp_tel_sum.HasSymbol("PIXReportCounter"));
    OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12DeviceX::CreateDepthStencilView", 21));
    OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47));
    OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::Dispatch", 14));
    OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12GraphicsCommandListX::DrawInstanced", 12));
    OS_REQUIRE(wp_tel_sum.HasSymbol("ID3D12CommandQueueX::PresentX", 19));

    OS_REQUIRE(wp_metrics.at("mode") == "warp_fallback");
    OS_REQUIRE(wp_metrics.at("is_warp") == "1");
    OS_REQUIRE(wp_metrics.at("title_id") == "0x5CA01002");

    // Verify 100% identical GPU culling statistics between HW AS+MS+PS and WARP CS+VS+PS!
    OS_REQUIRE(hw_metrics.at("total_meshlets") == wp_metrics.at("total_meshlets"));
    OS_REQUIRE(hw_metrics.at("surviving") == wp_metrics.at("surviving"));
    OS_REQUIRE(hw_metrics.at("frustum_culled") == wp_metrics.at("frustum_culled"));
    OS_REQUIRE(hw_metrics.at("cone_culled") == wp_metrics.at("cone_culled"));

    // Cross-compare rendered 256x256 PPM images between Run 1 (HW AS+MS+PS) and Run 2 (WARP CS+VS+PS)
    OS_REQUIRE(hw_ppm_rgb.size() == wp_ppm_rgb.size());
    const int hw_non_bg = std::stoi(hw_metrics.at("non_bg_pixels"));
    const int wp_non_bg = std::stoi(wp_metrics.at("non_bg_pixels"));
    OS_REQUIRE(std::abs(hw_non_bg - wp_non_bg) <= 100);

    uint32_t diff_pixels = 0;
    int max_channel_diff = 0;
    for (size_t i = 0; i < hw_ppm_rgb.size(); i += 3u) {
        const int dr = std::abs(static_cast<int>(hw_ppm_rgb[i + 0u]) - static_cast<int>(wp_ppm_rgb[i + 0u]));
        const int dg = std::abs(static_cast<int>(hw_ppm_rgb[i + 1u]) - static_cast<int>(wp_ppm_rgb[i + 1u]));
        const int db = std::abs(static_cast<int>(hw_ppm_rgb[i + 2u]) - static_cast<int>(wp_ppm_rgb[i + 2u]));
        const int m = std::max({dr, dg, db});
        if (m > 0) {
            ++diff_pixels;
            max_channel_diff = std::max(max_channel_diff, m);
        }
    }
    // Allow at most minor rasterization edge/rounding differences between HW GPU and WARP (< 2% of pixels)
    OS_REQUIRE(diff_pixels < 1300u);
    if (hw_non_bg == wp_non_bg) {
        OS_REQUIRE(max_channel_diff <= 4);
    }

    std::printf(
        "\n=== OpenScarlett Wave 2 MeshletCull Summary ===\n"
        "Host Probe    : has_hw_gpu=%d, has_hw_mesh=%d\n"
        "Package Dir   : %s (0 DLLs, 8 files, 100%% byte-identical before & after)\n"
        "Run 1 (%s)    : Adapter=%s | MeshTier=%s | Cull=%s/%s (%s frustum, %s cone) | "
        "NonBG=%s | FNV1a=%s | Tel=%llu calls (%zu syms, 0 not_impl) | %.1f ms (GPU %s ms)\n"
        "Run 2 (%s)    : Adapter=%s | MeshTier=%s | Cull=%s/%s (%s frustum, %s cone) | "
        "NonBG=%s | FNV1a=%s | Tel=%llu calls (%zu syms, 0 not_impl) | %.1f ms (GPU %s ms)\n"
        "HW vs WARP    : diff_pixels=%u / 65536 (max_channel_diff=%d)\n",
        has_hw_gpu ? 1 : 0,
        has_hw_mesh ? 1 : 0,
        pkg_dir.string().c_str(),
        hw_metrics.at("mode").c_str(),
        hw_metrics.at("adapter").c_str(),
        hw_metrics.at("mesh_tier").c_str(),
        hw_metrics.at("surviving").c_str(),
        hw_metrics.at("total_meshlets").c_str(),
        hw_metrics.at("frustum_culled").c_str(),
        hw_metrics.at("cone_culled").c_str(),
        hw_metrics.at("non_bg_pixels").c_str(),
        hw_metrics.at("fnv1a").c_str(),
        static_cast<unsigned long long>(hw_tel_sum.total_calls),
        hw_tel_sum.rows.size(),
        hw_elapsed_ms,
        hw_metrics.at("frame_ms").c_str(),
        wp_metrics.at("mode").c_str(),
        wp_metrics.at("adapter").c_str(),
        wp_metrics.at("mesh_tier").c_str(),
        wp_metrics.at("surviving").c_str(),
        wp_metrics.at("total_meshlets").c_str(),
        wp_metrics.at("frustum_culled").c_str(),
        wp_metrics.at("cone_culled").c_str(),
        wp_metrics.at("non_bg_pixels").c_str(),
        wp_metrics.at("fnv1a").c_str(),
        static_cast<unsigned long long>(wp_tel_sum.total_calls),
        wp_tel_sum.rows.size(),
        wp_elapsed_ms,
        wp_metrics.at("frame_ms").c_str(),
        diff_pixels,
        max_channel_diff);
    std::fflush(stdout);

    return 0;
}
