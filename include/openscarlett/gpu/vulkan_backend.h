#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/status.h"

namespace openscarlett {

inline constexpr uint32_t kSpirvMagicNumber = 0x07230203u;

enum class VulkanDeviceType : uint32_t {
    Other = 0,
    IntegratedGpu = 1,
    DiscreteGpu = 2,
    VirtualGpu = 3,
    Cpu = 4,
};

struct VulkanDeviceInfo {
    std::string device_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t api_patch = 0;
    uint32_t driver_version = 0;
    VulkanDeviceType device_type = VulkanDeviceType::Other;
    uint32_t compute_queue_family = 0;
    bool supports_compute = false;
    bool supports_graphics = false;
    bool is_hardware = false;
    bool is_fallback = false;

    // Extended Vulkan 1.2/1.3/1.4 graphics, mesh shader & WSI capabilities
    uint32_t graphics_queue_family = 0;
    bool supports_mesh_shader = false;
    bool supports_task_shader = false;
    bool supports_win32_surface = false;
    bool supports_swapchain = false;
    bool supports_hdr10_colorspace = false;

    // Sparse / reserved resource capabilities (VK_IMAGE_CREATE_SPARSE_*)
    uint32_t sparse_queue_family = 0;
    bool supports_sparse_binding = false;
    bool supports_sparse_residency_image_2d = false;
    bool supports_sparse_residency_image_3d = false;

    // Cross-vendor limits, subgroup & BC compression capabilities
    uint32_t max_push_constants_size = 128;
    uint32_t subgroup_size = 32;
    uint32_t supported_subgroup_stages = 0;
    uint32_t supported_subgroup_operations = 0;
    bool supports_bc_texture_compression = false;

    // OPENSCARLETT_VK_GPU / OPENSCARLETT_VK_DEVICE adapter selection diagnostics
    std::string selected_device_filter;
    bool matched_device_filter = false;
    uint64_t device_local_vram_bytes = 0;
    uint64_t device_rank_score = 0;
    uint32_t enumerated_device_index = 0;

    // Extended D3D12 parity capabilities probed live via vkGetPhysicalDeviceFeatures2/Properties2
    bool supports_depth_bounds_test = false;
    bool supports_sample_locations_ext = false;
    bool supports_custom_border_color = false;
    bool supports_custom_border_color_without_format = false;
    uint32_t max_custom_border_color_samplers = 0;
};

// Clean-room portable D3D12 OPTIONS2 / OPTIONS7 tier enums matching Windows D3D12 ABI values.
// Portable across MSVC, Clang-CL, and MinGW-w64 without requiring Windows SDK d3d12.h headers.
enum class VulkanD3D12ProgrammableSamplePositionsTier : uint32_t {
    NotSupported = 0u,
    Tier1 = 1u,
    Tier2 = 2u,
};

enum class VulkanD3D12MeshShaderTier : uint32_t {
    NotSupported = 0u,
    Tier1 = 10u,  // Matches D3D12_MESH_SHADER_TIER_1
};

enum class VulkanD3D12SamplerFeedbackTier : uint32_t {
    NotSupported = 0u,
    Tier0_9 = 90u,   // Matches D3D12_SAMPLER_FEEDBACK_TIER_0_9
    Tier1_0 = 100u,  // Matches D3D12_SAMPLER_FEEDBACK_TIER_1_0
};

struct VulkanD3D12Options2 {
    bool depth_bounds_test_supported = false;
    VulkanD3D12ProgrammableSamplePositionsTier programmable_sample_positions_tier =
        VulkanD3D12ProgrammableSamplePositionsTier::NotSupported;
};

struct VulkanD3D12Options7 {
    VulkanD3D12MeshShaderTier mesh_shader_tier = VulkanD3D12MeshShaderTier::NotSupported;
    VulkanD3D12SamplerFeedbackTier sampler_feedback_tier = VulkanD3D12SamplerFeedbackTier::NotSupported;
};

// Portable template mappers onto native Windows D3D12_FEATURE_DATA_D3D12_OPTIONS2 / OPTIONS7
// (or any ABI-compatible clean-room struct on MinGW-w64).
template <typename NativeOptions2>
inline void FillNativeD3D12Options2(const VulkanD3D12Options2& src, NativeOptions2& dst) noexcept {
    dst.DepthBoundsTestSupported = src.depth_bounds_test_supported ? 1 : 0;
    dst.ProgrammableSamplePositionsTier =
        static_cast<decltype(dst.ProgrammableSamplePositionsTier)>(src.programmable_sample_positions_tier);
}

template <typename NativeOptions7>
inline void FillNativeD3D12Options7(const VulkanD3D12Options7& src, NativeOptions7& dst) noexcept {
    dst.MeshShaderTier = static_cast<decltype(dst.MeshShaderTier)>(src.mesh_shader_tier);
    dst.SamplerFeedbackTier = static_cast<decltype(dst.SamplerFeedbackTier)>(src.sampler_feedback_tier);
}

// Live physical-device capability snapshot probed via vkGetPhysicalDeviceFeatures2/Properties2.
struct VulkanFeatureProbe {
    uint32_t api_version = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t api_patch = 0;
    bool probed_from_icd = false;

    // Core VkPhysicalDeviceFeatures
    bool depth_bounds = false;
    bool texture_compression_bc = false;
    bool fragment_stores_and_atomics = false;
    bool shader_resource_residency = false;
    bool shader_resource_min_lod = false;
    bool sparse_binding = false;
    bool sparse_residency_image_2d = false;
    bool sparse_residency_image_3d = false;

    // Vulkan 1.2 / 1.3 core feature chains
    bool vulkan12_probed = false;
    bool sampler_filter_minmax = false;
    bool timeline_semaphore = false;
    bool buffer_device_address = false;
    bool descriptor_indexing = false;
    bool vulkan13_probed = false;
    bool synchronization2 = false;
    bool dynamic_rendering = false;
    bool maintenance4 = false;

    // VK_EXT_sample_locations (Programmable Sample Positions)
    bool has_sample_locations_ext = false;
    uint32_t sample_location_sample_counts = 0;
    uint32_t max_sample_location_grid_size_x = 0;
    uint32_t max_sample_location_grid_size_y = 0;
    uint32_t sample_location_subpixel_bits = 0;
    bool variable_sample_locations = false;

    // VK_EXT_mesh_shader (Mesh / Task Shaders)
    bool has_mesh_shader_ext = false;
    bool task_shader = false;
    bool mesh_shader = false;
    bool mesh_shader_queries = false;

    // Sampler feedback extension probe (VK_EXT_sampler_feedback / VK_KHR_sampler_feedback)
    bool has_sampler_feedback_ext = false;
    bool sampler_feedback_min_mip = false;
    bool sampler_feedback_mip_region_used = false;

    // VK_EXT_custom_border_color
    bool has_custom_border_color_ext = false;
    bool custom_border_colors = false;
    bool custom_border_color_without_format = false;
    uint32_t max_custom_border_color_samplers = 0;
};

struct SpirvValidationInfo {
    uint32_t version_major = 0;
    uint32_t version_minor = 0;
    uint32_t generator = 0;
    uint32_t bound = 0;
    uint32_t instruction_count = 0;
    uint32_t entry_point_count = 0;
    bool has_compute_entry = false;
    std::string entry_point_name;
    uint32_t local_size_x = 1;
    uint32_t local_size_y = 1;
    uint32_t local_size_z = 1;
    bool has_vertex_entry = false;
    bool has_fragment_entry = false;
    bool has_task_entry = false;
    bool has_mesh_entry = false;
};

enum class VulkanCommandOp : uint32_t {
    Nop = 0,
    PipelineBarrier = 1,
    BindComputePipeline = 2,
    BindDescriptorSets = 3,
    Dispatch = 4,
    GlobalUavBarrier = 5,
};

struct VulkanCommandItem {
    VulkanCommandOp op = VulkanCommandOp::Nop;
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
    uint64_t resource_id = 0;
};

struct VulkanCommandListDesc {
    std::vector<VulkanCommandItem> commands;
    bool submit_empty_fence_sync = true;
};

struct VulkanCommandListResult {
    uint32_t recorded_commands = 0;
    uint32_t dispatch_count = 0;
    uint32_t barrier_count = 0;
    uint32_t global_uav_barrier_count = 0;
    uint32_t last_global_uav_src_stage_mask = 0;
    uint32_t last_global_uav_dst_stage_mask = 0;
    uint32_t last_global_uav_src_access_mask = 0;
    uint32_t last_global_uav_dst_access_mask = 0;
    bool executed_on_vulkan_icd = false;
};

// Candidate physical device metadata used by deterministic GPU ranking & OPENSCARLETT_VK_GPU selector (AnyBox #68).
struct VulkanPhysicalDeviceCandidate {
    uint32_t enumerated_index = 0;
    std::string device_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    VulkanDeviceType device_type = VulkanDeviceType::Other;
    uint64_t device_local_vram_bytes = 0;
    bool supports_compute = false;
    bool supports_graphics = false;
    bool supports_mesh_shader = false;
    uint64_t rank_score = 0;
};

// Supported Vulkan image formats (values match Khronos VkFormat constants).
enum class VulkanImageFormat : uint32_t {
    Rgba8Unorm = 37,    // VK_FORMAT_R8G8B8A8_UNORM
    Bgra8Unorm = 44,    // VK_FORMAT_B8G8R8A8_UNORM
    Rgba16Sfloat = 97,  // VK_FORMAT_R16G16B16A16_SFLOAT
    D32Sfloat = 126,    // VK_FORMAT_D32_SFLOAT
    Bc1RgbaUnorm = 133, // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
    Bc2Unorm = 135,     // VK_FORMAT_BC2_UNORM_BLOCK
    Bc3Unorm = 137,     // VK_FORMAT_BC3_UNORM_BLOCK
    Bc4Unorm = 139,     // VK_FORMAT_BC4_UNORM_BLOCK
    Bc5Unorm = 141,     // VK_FORMAT_BC5_UNORM_BLOCK
    Bc6hUfloat = 143,   // VK_FORMAT_BC6H_UFLOAT_BLOCK
    Bc7Unorm = 145,     // VK_FORMAT_BC7_UNORM_BLOCK
};

// Sampler addressing modes (values match Khronos VkSamplerAddressMode).
enum class VulkanSamplerAddressMode : uint32_t {
    Repeat = 0u,
    MirroredRepeat = 1u,
    ClampToEdge = 2u,
    ClampToBorder = 3u,
    MirrorClampToEdge = 4u,
};

// Sampler border color modes (values match Khronos VkBorderColor + VK_EXT_custom_border_color).
enum class VulkanBorderColorMode : uint32_t {
    FloatTransparentBlack = 0u,
    IntTransparentBlack = 1u,
    FloatOpaqueBlack = 2u,
    IntOpaqueBlack = 3u,
    FloatOpaqueWhite = 4u,
    IntOpaqueWhite = 5u,
    FloatCustom = 1000287003u,  // VK_BORDER_COLOR_FLOAT_CUSTOM_EXT
    IntCustom = 1000287004u,    // VK_BORDER_COLOR_INT_CUSTOM_EXT
};

struct VulkanCustomBorderSamplerDesc {
    VulkanSamplerAddressMode address_mode_u = VulkanSamplerAddressMode::ClampToBorder;
    VulkanSamplerAddressMode address_mode_v = VulkanSamplerAddressMode::ClampToBorder;
    VulkanSamplerAddressMode address_mode_w = VulkanSamplerAddressMode::ClampToBorder;
    bool linear_filter = false;
    VulkanBorderColorMode border_color_mode = VulkanBorderColorMode::FloatCustom;
    float custom_border_color_float[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int32_t custom_border_color_int[4] = {0, 0, 0, 1};
    VulkanImageFormat border_format = VulkanImageFormat::Rgba8Unorm;
    bool specify_border_format = false;
    bool force_standard_fallback = false;
};

struct VulkanCustomBorderSamplerResult {
    bool created = false;
    bool executed_on_vulkan_icd = false;
    bool used_custom_border_color_ext = false;
    bool used_standard_border_fallback = false;
    VulkanBorderColorMode effective_border_color_mode = VulkanBorderColorMode::FloatTransparentBlack;
    float effective_border_color_float[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int32_t effective_border_color_int[4] = {0, 0, 0, 0};
};

// Supported vertex attribute formats (values match Khronos VkFormat constants).
enum class VulkanVertexFormat : uint32_t {
    R32Uint = 98,
    R32Sfloat = 100,
    R32G32Sfloat = 103,
    R32G32B32Sfloat = 106,
    R32G32B32A32Uint = 107,
    R32G32B32A32Sfloat = 109,
};

struct VulkanVertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    VulkanVertexFormat format = VulkanVertexFormat::R32G32B32A32Sfloat;
    uint32_t offset = 0;
};

// Supported descriptor kinds (values match Khronos VkDescriptorType constants).
enum class VulkanDescriptorKind : uint32_t {
    Sampler = 0,
    CombinedImageSampler = 1,
    SampledImage = 2,
    UniformBuffer = 6,
    StorageBuffer = 7,
};

// Single mip level of a 2D or 3D texture.
struct VulkanTextureMipLevel {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    std::vector<uint8_t> data;  // Tightly packed texels (4 B/texel RGBA8, 8 B/texel RGBA16F)
};

// Descriptor for a mipmapped 2D texture or 3D volume texture.
struct VulkanTextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    bool is_3d = false;
    VulkanImageFormat format = VulkanImageFormat::Rgba8Unorm;
    std::vector<VulkanTextureMipLevel> mips;
};

struct VulkanTextureRoundtripResult {
    std::vector<VulkanTextureMipLevel> readback_mips;
    bool executed_on_vulkan_icd = false;
};

// Staged block-compressed (BC1-BC7) mip copy descriptor (supports custom / non-aligned row pitch).
struct VulkanCompressedMipCopy {
    uint32_t width = 4;
    uint32_t height = 4;
    uint32_t depth = 1;
    uint32_t src_row_pitch_bytes = 0;  // 0 = tightly packed; may be D3D12 256B-aligned or custom pitch
    std::vector<uint8_t> src_data;
};

struct VulkanCompressedTextureCopyDesc {
    uint32_t width = 4;
    uint32_t height = 4;
    uint32_t depth = 1;
    VulkanImageFormat format = VulkanImageFormat::Bc7Unorm;
    std::vector<VulkanCompressedMipCopy> mips;
};

struct VulkanCompressedTextureCopyResult {
    std::vector<VulkanTextureMipLevel> readback_mips;
    uint32_t staged_mip_count = 0;
    uint64_t total_staged_bytes = 0;
    bool executed_on_vulkan_icd = false;
    bool used_software_fallback = false;
};

// Wave32-on-Wave64 compute dispatch controls & diagnostics.
struct VulkanComputeDispatchOptions {
    uint32_t requested_wave_size = 32;            // 32 (Scarlett native Wave32) or 64
    bool wave32_on_wave64_safe = true;            // Pad buffers & guard lane budget on Wave64 devices
    uint32_t simulated_device_subgroup_size = 0;  // 0 = use active/queried device subgroup_size
};

struct VulkanComputeDispatchResult {
    bool executed_on_vulkan_icd = false;
    uint32_t device_subgroup_size = 32;
    uint32_t effective_wave_size = 32;
    bool wave32_on_wave64_adapted = false;
    uint32_t padded_buffer_elements = 0;
};

// Records automatic mixed-heap buffer/image descriptor slot separation remappings.
struct VulkanDescriptorSlotRemap {
    uint32_t original_binding = 0;
    uint32_t remapped_binding = 0;
    VulkanDescriptorKind kind = VulkanDescriptorKind::UniformBuffer;
};

// Binding in descriptor set 0 for graphics or mesh pipelines.
struct VulkanDescriptorBinding {
    uint32_t binding = 0;
    VulkanDescriptorKind kind = VulkanDescriptorKind::UniformBuffer;
    std::vector<uint8_t> buffer_data;  // UniformBuffer / StorageBuffer payload
    VulkanTextureDesc texture;         // CombinedImageSampler / SampledImage payload
    bool linear_filter = false;        // false = nearest, true = linear

    // Optional T# view-range mip selection (VkImageViewCreateInfo::subresourceRange)
    uint32_t view_base_mip_level = 0;
    uint32_t view_mip_level_count = 0;  // 0 = all remaining mips from view_base_mip_level

    // Optional S# sampler LOD controls (VkSamplerCreateInfo)
    float sampler_min_lod = 0.0f;
    float sampler_max_lod = 16.0f;
    float sampler_mip_lod_bias = 0.0f;

    // Optional sampler addressing & custom border color (VK_EXT_custom_border_color)
    VulkanSamplerAddressMode address_mode_u = VulkanSamplerAddressMode::ClampToEdge;
    VulkanSamplerAddressMode address_mode_v = VulkanSamplerAddressMode::ClampToEdge;
    VulkanSamplerAddressMode address_mode_w = VulkanSamplerAddressMode::ClampToEdge;
    VulkanBorderColorMode border_color_mode = VulkanBorderColorMode::FloatTransparentBlack;
    float custom_border_color_float[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int32_t custom_border_color_int[4] = {0, 0, 0, 1};
    bool custom_border_specify_format = false;
    bool force_standard_border_fallback = false;
};

// Full VS+PS graphics draw configuration (VB, IB, UBO/SSBO/2D/3D textures, push constants, depth, MSAA 4x).
struct VulkanGraphicsDrawDesc {
    std::vector<uint32_t> vertex_spirv;
    std::vector<uint32_t> fragment_spirv;
    std::string vertex_entry = "main";
    std::string fragment_entry = "main";

    uint32_t width = 128;
    uint32_t height = 128;
    VulkanImageFormat color_format = VulkanImageFormat::Rgba8Unorm;
    bool enable_depth = false;
    bool enable_msaa_4x = false;
    bool depth_write = true;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;

    // Optional vertex buffer + attributes (if empty, VS may generate geometry from VertexIndex).
    std::vector<uint8_t> vertex_bytes;
    uint32_t vertex_stride = 0;
    std::vector<VulkanVertexAttribute> vertex_attributes;

    // Optional 32-bit index buffer (if empty, uses vkCmdDraw with vertex_count).
    std::vector<uint32_t> indices;
    uint32_t vertex_count = 3;

    // Optional push constants (bound to VERTEX | FRAGMENT).
    std::vector<uint8_t> push_constants;
    // Optional push-constant byte-budget override (0 = use device maxPushConstantsSize).
    uint32_t push_constant_budget_override = 0;

    // Optional descriptor set 0 bindings.
    std::vector<VulkanDescriptorBinding> descriptors;
};

struct VulkanGraphicsDrawResult {
    FrameBufferRgba8 framebuffer;         // Always populated (FP16 clamped/quantized to RGBA8)
    std::vector<uint16_t> raw_fp16_rgba;  // Populated when color_format == Rgba16Sfloat
    uint64_t fnv1a_hash = 0;
    bool executed_on_vulkan_icd = false;
    bool used_msaa_4x = false;
    bool used_depth_buffer = false;
    bool push_constants_exceeded_budget = false;
    uint32_t effective_push_constant_bytes = 0;
    uint32_t device_push_constant_limit = 128;
    uint32_t descriptor_collisions_resolved = 0;
    std::vector<VulkanDescriptorSlotRemap> descriptor_slot_remaps;
    uint32_t custom_border_samplers_used = 0;
    uint32_t custom_border_fallback_samplers_used = 0;
};

// VK_EXT_mesh_shader draw configuration (optional Task/AS + Mesh/MS + Fragment/PS).
struct VulkanMeshDrawDesc {
    std::vector<uint32_t> task_spirv;      // Optional TaskEXT (Amplification) SPIR-V 1.4+
    std::vector<uint32_t> mesh_spirv;      // Required MeshEXT SPIR-V 1.4+
    std::vector<uint32_t> fragment_spirv;  // Required Fragment SPIR-V
    std::string task_entry = "main";
    std::string mesh_entry = "main";
    std::string fragment_entry = "main";

    uint32_t group_count_x = 1;
    uint32_t group_count_y = 1;
    uint32_t group_count_z = 1;

    uint32_t width = 128;
    uint32_t height = 128;
    VulkanImageFormat color_format = VulkanImageFormat::Rgba8Unorm;
    bool enable_depth = false;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;

    std::vector<uint8_t> push_constants;
    uint32_t push_constant_budget_override = 0;
    std::vector<VulkanDescriptorBinding> descriptors;

    // If true, transparently falls back to deterministic SPIR-V mesh emulation
    // when the host GPU / CI runner does not expose VK_EXT_mesh_shader.
    bool allow_fallback_emulation = true;
};

struct VulkanMeshDrawResult {
    FrameBufferRgba8 framebuffer;
    std::vector<uint16_t> raw_fp16_rgba;
    uint64_t fnv1a_hash = 0;
    bool executed_on_mesh_shader_hw = false;
    bool used_task_shader = false;
    bool used_fallback = false;
    bool push_constants_exceeded_budget = false;
    uint32_t effective_push_constant_bytes = 0;
    uint32_t device_push_constant_limit = 128;
    uint32_t descriptor_collisions_resolved = 0;
    std::vector<VulkanDescriptorSlotRemap> descriptor_slot_remaps;
    uint32_t custom_border_samplers_used = 0;
    uint32_t custom_border_fallback_samplers_used = 0;
};

// Win32 surface / swapchain / headless offscreen presentation descriptor.
inline constexpr uint32_t kVkColorSpaceSrgbNonlinearKhr = 0u;
inline constexpr uint32_t kVkColorSpaceHdr10St2084Ext = 1000104008u;

struct VulkanSwapchainDesc {
    void* hwnd = nullptr;       // Win32 HWND; nullptr or headless_offscreen=true selects offscreen mode
    void* hinstance = nullptr;  // Optional HINSTANCE (defaults to GetModuleHandleA(nullptr))
    uint32_t width = 128;
    uint32_t height = 128;
    bool prefer_hdr10 = false;
    bool headless_offscreen = false;
};

struct VulkanSwapchainInfo {
    uint64_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t image_count = 0;
    uint32_t vk_format = 0;
    uint32_t vk_color_space = 0;
    bool is_win32_surface = false;
    bool is_headless_offscreen = false;
    bool hdr10_advertised = false;
    bool hdr10_active = false;
    std::vector<uint32_t> supported_color_spaces;
};

struct VulkanPresentResult {
    uint32_t image_index = 0;
    bool presented_to_win32_swapchain = false;
    bool readback_available = false;
    FrameBufferRgba8 readback_framebuffer;
    uint64_t fnv1a_hash = 0;
};

// Sparse / reserved image resource structs (D3D12 CreateReservedResource + GetResourceTiling parity).
struct VulkanReservedResourceDesc {
    uint32_t width = 256;
    uint32_t height = 256;
    uint32_t depth = 1;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    bool is_3d = false;
    VulkanImageFormat format = VulkanImageFormat::Rgba8Unorm;
};

struct VulkanSubresourceTiling {
    uint32_t width_in_tiles = 0;
    uint32_t height_in_tiles = 0;
    uint32_t depth_in_tiles = 0;
    uint32_t start_tile_index_in_overall_resource = 0;
};

struct VulkanResourceTilingInfo {
    uint32_t num_tiles_for_entire_resource = 0;
    uint32_t num_standard_mips = 0;
    uint32_t num_packed_mips = 0;
    uint32_t num_tiles_for_packed_mips = 0;
    uint32_t start_tile_index_in_overall_resource = 0;
    uint32_t tile_width_in_texels = 0;
    uint32_t tile_height_in_texels = 0;
    uint32_t tile_depth_in_texels = 1;
    uint64_t tile_size_bytes = 65536;
    uint64_t mip_tail_offset_bytes = 0;
    uint64_t mip_tail_size_bytes = 0;
    uint64_t mip_tail_stride_bytes = 0;
    uint32_t sparse_format_flags = 0;
    std::vector<VulkanSubresourceTiling> subresource_tilings;
    bool executed_on_vulkan_icd = false;
};

struct VulkanSparseTileMapping {
    uint32_t mip_level = 0;
    uint32_t array_layer = 0;
    uint32_t tile_x = 0;
    uint32_t tile_y = 0;
    uint32_t tile_z = 0;
    uint32_t tile_count_x = 1;
    uint32_t tile_count_y = 1;
    uint32_t tile_count_z = 1;
    uint32_t heap_tile_offset = 0;
    bool map_null = false;
    bool bind_mip_tail = false;
};

struct VulkanReservedResourceHandle {
    uint64_t handle = 0;
    VulkanResourceTilingInfo tiling;
    bool executed_on_vulkan_icd = false;
};

// Dynamic Vulkan 1.2/1.3/1.4 backend with zero Vulkan SDK header dependencies.
// Dynamically loads vulkan-1.dll at runtime via LoadLibraryA + vkGetInstanceProcAddr,
// executes real hardware/ICD compute, graphics rasterization, VK_EXT_mesh_shader,
// 2D/3D mipmapped texture copies, and Win32 swapchain presentation when available,
// and provides deterministic SPIR-V software fallbacks when no ICD is present.
class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Attempts to initialize a real Vulkan 1.2/1.3/1.4 ICD via vulkan-1.dll.
    // If no ICD is available and allow_software_fallback is true, enters deterministic
    // software SPIR-V compute/graphics/mesh fallback mode.
    Status Initialize(bool allow_software_fallback = true);

    // Explicitly initializes the deterministic software SPIR-V fallback mode
    // (useful for testing fallback parity even on hosts with hardware Vulkan GPUs).
    Status InitializeSoftwareFallback();

    void Shutdown();
    bool IsInitialized() const noexcept;
    bool is_fallback_mode() const noexcept;

    const VulkanDeviceInfo& GetDeviceInfo() const noexcept { return device_info_; }

    // Deterministic physical-device ranking & OPENSCARLETT_VK_GPU selector helpers (AnyBox #68).
    // Ranks Discrete > Integrated > Virtual > CPU > Other, weighted by graphics+compute,
    // VK_EXT_mesh_shader, device-local VRAM size, and Vulkan API version.
    static uint64_t ScorePhysicalDeviceCandidate(const VulkanPhysicalDeviceCandidate& candidate) noexcept;
    static std::vector<VulkanPhysicalDeviceCandidate> RankPhysicalDeviceCandidates(
        std::vector<VulkanPhysicalDeviceCandidate> candidates);
    // Selects a physical device using OPENSCARLETT_VK_GPU syntax:
    //   - empty string: highest-ranked eligible candidate
    //   - non-negative integer (e.g. "0", "1"): selects candidate by enumerated_index
    //   - non-numeric string (e.g. "Intel", "RTX"): highest-ranked case-insensitive name substring match
    static bool SelectPhysicalDeviceCandidate(
        const std::vector<VulkanPhysicalDeviceCandidate>& candidates,
        const std::string& gpu_selector,
        VulkanPhysicalDeviceCandidate& out_selected,
        bool* out_matched_selector = nullptr);
    // Enumerates and ranks all compute-capable physical devices on the live Vulkan ICD.
    static Status EnumerateRankedPhysicalDevices(
        std::vector<VulkanPhysicalDeviceCandidate>& out_ranked_devices);

    // Live Vulkan physical-device capability probe & D3D12 OPTIONS2 / OPTIONS7 mapping helpers.
    const VulkanFeatureProbe& GetFeatureProbe() const noexcept { return feature_probe_; }
    static VulkanD3D12Options2 MapVulkanToD3D12Options2(const VulkanFeatureProbe& probe) noexcept;
    static VulkanD3D12Options7 MapVulkanToD3D12Options7(const VulkanFeatureProbe& probe) noexcept;
    Status QueryD3D12Options2(VulkanD3D12Options2& out_options) const noexcept;
    Status QueryD3D12Options7(VulkanD3D12Options7& out_options) const noexcept;

    // Creates and validates a Vulkan sampler using VK_EXT_custom_border_color when supported,
    // or falls back cleanly to standard Vulkan border colors when unsupported or forced.
    Status ProbeCustomBorderColorSampler(const VulkanCustomBorderSamplerDesc& desc,
                                         VulkanCustomBorderSamplerResult& out_result);

    // Validates SPIR-V binary magic (0x07230203), version, header bounds, instruction
    // word lengths, OpMemoryModel presence, and balanced OpFunction/OpFunctionEnd blocks.
    static Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv,
                                      SpirvValidationInfo* out_info = nullptr);

    // Validates SPIR-V and executes a 1D compute shader with storage buffer binding 0 (input)
    // and binding 1 (output) across group_x workgroups on real Vulkan GPU or software fallback.
    Status ExecuteComputeShader(const std::vector<uint32_t>& spirv,
                                const std::vector<uint32_t>& input_u32,
                                std::vector<uint32_t>& output_u32,
                                uint32_t group_x);

    // Extended compute dispatch with Wave32-on-Wave64 subgroup safety and detailed diagnostics.
    Status ExecuteComputeShaderEx(const std::vector<uint32_t>& spirv,
                                  const std::vector<uint32_t>& input_u32,
                                  std::vector<uint32_t>& output_u32,
                                  uint32_t group_x,
                                  const VulkanComputeDispatchOptions& options,
                                  VulkanComputeDispatchResult& out_result);

    // Staged block-compressed (BC1-BC7) texture copy handling 4x4 block-rounded extents
    // and non-aligned / D3D12 256B-padded row pitches via intermediate staging buffers.
    Status CopyCompressedTextureStaged(const VulkanCompressedTextureCopyDesc& desc,
                                       VulkanCompressedTextureCopyResult& out_result);

    // Test/diagnostic overrides for cross-vendor push-constant and subgroup-size verification.
    void SetPushConstantBudgetOverrideForTest(uint32_t max_bytes) noexcept;
    uint32_t GetEffectivePushConstantBudget() const noexcept;
    void SetSimulatedSubgroupSizeForTest(uint32_t subgroup_size) noexcept;
    uint32_t GetEffectiveSubgroupSize() const noexcept;

    // Records a command list (real VkCommandBuffer begin/barrier/end/submit on Vulkan ICD,
    // or validated recording in software fallback mode).
    Status RecordCommandList(const VulkanCommandListDesc& desc,
                             VulkanCommandListResult& out_result);

    // Executes a full VS+PS rasterization draw with optional vertex/index buffers, UBO/SSBO,
    // 2D/3D sampled textures, push constants, D32_SFLOAT depth buffer, and 4x MSAA resolve.
    Status ExecuteGraphicsDraw(const VulkanGraphicsDrawDesc& desc,
                               VulkanGraphicsDrawResult& out_result);

    // Executes a VK_EXT_mesh_shader pipeline (optional TaskEXT + MeshEXT + Fragment) via
    // vkCmdDrawMeshTasksEXT with graceful deterministic fallback when unavailable.
    Status ExecuteMeshShaderDraw(const VulkanMeshDrawDesc& desc,
                                 VulkanMeshDrawResult& out_result);

    // Uploads all mip levels of a 2D or 3D volume texture via vkCmdCopyBufferToImage and
    // reads back every mip level via vkCmdCopyImageToBuffer to verify exact GPU layout/copies.
    Status UploadAndReadbackTexture(const VulkanTextureDesc& desc,
                                    VulkanTextureRoundtripResult& out_result);

    // Creates either an interactive Win32 VkSwapchainKHR (with HDR10 colorspace probe)
    // or a headless offscreen render-target swapchain (for CI/headless environments).
    Status CreateWin32Swapchain(const VulkanSwapchainDesc& desc,
                                VulkanSwapchainInfo& out_info);

    // Presents an RGBA8 framebuffer to a Win32 swapchain or offscreen headless target.
    Status PresentRgba8ToSwapchain(uint64_t swapchain_handle,
                                   const FrameBufferRgba8& frame,
                                   VulkanPresentResult& out_result,
                                   bool readback_frame = false);

    // Destroys a previously created swapchain/surface handle.
    Status DestroyWin32Swapchain(uint64_t swapchain_handle);

    // Creates a sparse/reserved Vulkan image (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
    // VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) and queries vkGetImageSparseMemoryRequirements
    // to populate D3D12 GetResourceTiling-compatible tiling metadata.
    Status CreateReservedImage(const VulkanReservedResourceDesc& desc,
                               VulkanReservedResourceHandle& out_handle);

    // Queries GetResourceTiling-compatible tiling layout for an existing reserved image.
    Status GetReservedImageTiling(uint64_t reserved_handle,
                                  VulkanResourceTilingInfo& out_tiling) const;

    // Binds or unbinds sparse tiles (and optional opaque mip tail) via vkQueueBindSparse.
    Status BindReservedImageTiles(uint64_t reserved_handle,
                                  const std::vector<VulkanSparseTileMapping>& mappings);

    // Uploads texel data to a bound tile of a reserved image and reads it back from the GPU.
    Status UploadAndReadbackReservedImageTile(uint64_t reserved_handle,
                                              uint32_t mip_level,
                                              uint32_t tile_x,
                                              uint32_t tile_y,
                                              const std::vector<uint8_t>& tile_pixels,
                                              std::vector<uint8_t>& out_readback_pixels);

    // Destroys a previously created reserved/sparse image and releases its backing memory.
    Status DestroyReservedImage(uint64_t reserved_handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VulkanDeviceInfo device_info_{};
    VulkanFeatureProbe feature_probe_{};
    VulkanD3D12Options2 d3d12_options2_{};
    VulkanD3D12Options7 d3d12_options7_{};
};

}  // namespace openscarlett
