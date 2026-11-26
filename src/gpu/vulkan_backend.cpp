#include "openscarlett/gpu/vulkan_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#define SX_VKAPI_PTR __stdcall
#else
#define SX_VKAPI_PTR
#endif

namespace openscarlett {

namespace {

// ---------------------------------------------------------------------------
// Minimal Vulkan 1.2/1.3/1.4 C ABI declarations (zero SDK headers).
// Follows the public Khronos Vulkan 1.2/1.3/1.4 Specification ABI for x86-64.
// ---------------------------------------------------------------------------
using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkResult = int32_t;

struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkCommandBuffer_T;

using VkInstance = VkInstance_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkDevice = VkDevice_T*;
using VkQueue = VkQueue_T*;
using VkCommandBuffer = VkCommandBuffer_T*;

using VkDeviceMemory = uint64_t;
using VkBuffer = uint64_t;
using VkImage = uint64_t;
using VkImageView = uint64_t;
using VkSampler = uint64_t;
using VkRenderPass = uint64_t;
using VkFramebuffer = uint64_t;
using VkShaderModule = uint64_t;
using VkPipelineCache = uint64_t;
using VkPipelineLayout = uint64_t;
using VkPipeline = uint64_t;
using VkDescriptorSetLayout = uint64_t;
using VkDescriptorPool = uint64_t;
using VkDescriptorSet = uint64_t;
using VkCommandPool = uint64_t;
using VkFence = uint64_t;
using VkSemaphore = uint64_t;
using VkSurfaceKHR = uint64_t;
using VkSwapchainKHR = uint64_t;

inline constexpr uint64_t kVkNullHandle = 0;
inline constexpr VkDeviceSize kVkWholeSize = ~0ull;
inline constexpr uint32_t kVkQueueFamilyIgnored = ~0u;
inline constexpr uint32_t kVkSubpassExternal = ~0u;
inline constexpr uint64_t kVkUint64Max = ~0ull;

inline constexpr VkResult kVkSuccess = 0;
inline constexpr VkResult kVkSuboptimalKhr = 1000001003;

constexpr uint32_t VkMakeApiVersion(uint32_t variant, uint32_t major, uint32_t minor, uint32_t patch) noexcept {
    return (variant << 29u) | (major << 22u) | (minor << 12u) | patch;
}
constexpr uint32_t VkApiMajor(uint32_t v) noexcept { return (v >> 22u) & 0x7Fu; }
constexpr uint32_t VkApiMinor(uint32_t v) noexcept { return (v >> 12u) & 0x3FFu; }
constexpr uint32_t VkApiPatch(uint32_t v) noexcept { return v & 0xFFFu; }

inline constexpr uint32_t kVkApiVersion1_0 = VkMakeApiVersion(0, 1, 0, 0);
inline constexpr uint32_t kVkApiVersion1_1 = VkMakeApiVersion(0, 1, 1, 0);
inline constexpr uint32_t kVkApiVersion1_2 = VkMakeApiVersion(0, 1, 2, 0);
inline constexpr uint32_t kVkApiVersion1_3 = VkMakeApiVersion(0, 1, 3, 0);

// VkStructureType constants
inline constexpr int32_t kVkStypeApplicationInfo = 0;
inline constexpr int32_t kVkStypeInstanceCreateInfo = 1;
inline constexpr int32_t kVkStypeDeviceQueueCreateInfo = 2;
inline constexpr int32_t kVkStypeDeviceCreateInfo = 3;
inline constexpr int32_t kVkStypeSubmitInfo = 4;
inline constexpr int32_t kVkStypeMemoryAllocateInfo = 5;
inline constexpr int32_t kVkStypeMappedMemoryRange = 6;
inline constexpr int32_t kVkStypeBindSparseInfo = 7;
inline constexpr int32_t kVkStypeFenceCreateInfo = 8;
inline constexpr int32_t kVkStypeSemaphoreCreateInfo = 9;
inline constexpr int32_t kVkStypeBufferCreateInfo = 12;
inline constexpr int32_t kVkStypeImageCreateInfo = 14;
inline constexpr int32_t kVkStypeImageViewCreateInfo = 15;
inline constexpr int32_t kVkStypeShaderModuleCreateInfo = 16;
inline constexpr int32_t kVkStypePipelineShaderStageCreateInfo = 18;
inline constexpr int32_t kVkStypePipelineVertexInputStateCreateInfo = 19;
inline constexpr int32_t kVkStypePipelineInputAssemblyStateCreateInfo = 20;
inline constexpr int32_t kVkStypePipelineViewportStateCreateInfo = 22;
inline constexpr int32_t kVkStypePipelineRasterizationStateCreateInfo = 23;
inline constexpr int32_t kVkStypePipelineMultisampleStateCreateInfo = 24;
inline constexpr int32_t kVkStypePipelineDepthStencilStateCreateInfo = 25;
inline constexpr int32_t kVkStypePipelineColorBlendStateCreateInfo = 26;
inline constexpr int32_t kVkStypePipelineDynamicStateCreateInfo = 27;
inline constexpr int32_t kVkStypeGraphicsPipelineCreateInfo = 28;
inline constexpr int32_t kVkStypeComputePipelineCreateInfo = 29;
inline constexpr int32_t kVkStypePipelineLayoutCreateInfo = 30;
inline constexpr int32_t kVkStypeSamplerCreateInfo = 31;
inline constexpr int32_t kVkStypeDescriptorSetLayoutCreateInfo = 32;
inline constexpr int32_t kVkStypeDescriptorPoolCreateInfo = 33;
inline constexpr int32_t kVkStypeDescriptorSetAllocateInfo = 34;
inline constexpr int32_t kVkStypeWriteDescriptorSet = 35;
inline constexpr int32_t kVkStypeFramebufferCreateInfo = 37;
inline constexpr int32_t kVkStypeRenderPassCreateInfo = 38;
inline constexpr int32_t kVkStypeCommandPoolCreateInfo = 39;
inline constexpr int32_t kVkStypeCommandBufferAllocateInfo = 40;
inline constexpr int32_t kVkStypeCommandBufferBeginInfo = 42;
inline constexpr int32_t kVkStypeRenderPassBeginInfo = 43;
inline constexpr int32_t kVkStypeBufferMemoryBarrier = 44;
inline constexpr int32_t kVkStypeImageMemoryBarrier = 45;
inline constexpr int32_t kVkStypeMemoryBarrier = 46;
inline constexpr int32_t kVkStypePhysicalDeviceVulkan12Features = 51;
inline constexpr int32_t kVkStypePhysicalDeviceVulkan13Features = 53;
inline constexpr int32_t kVkStypeSwapchainCreateInfoKhr = 1000001000;
inline constexpr int32_t kVkStypePresentInfoKhr = 1000001001;
inline constexpr int32_t kVkStypeWin32SurfaceCreateInfoKhr = 1000009000;
inline constexpr int32_t kVkStypePhysicalDeviceFeatures2 = 1000059000;
inline constexpr int32_t kVkStypePhysicalDeviceProperties2 = 1000059001;
inline constexpr int32_t kVkStypePhysicalDeviceSubgroupProperties = 1000094000;
inline constexpr int32_t kVkStypePhysicalDeviceSampleLocationsPropertiesExt = 1000143003;
inline constexpr int32_t kVkStypeSamplerCustomBorderColorCreateInfoExt = 1000287000;
inline constexpr int32_t kVkStypePhysicalDeviceCustomBorderColorPropertiesExt = 1000287001;
inline constexpr int32_t kVkStypePhysicalDeviceCustomBorderColorFeaturesExt = 1000287002;
inline constexpr int32_t kVkStypePhysicalDeviceMeshShaderFeaturesExt = 1000328000;
inline constexpr int32_t kVkStypeRenderingInfo = 1000044000;
inline constexpr int32_t kVkStypeRenderingAttachmentInfo = 1000044001;
inline constexpr int32_t kVkStypePipelineRenderingCreateInfo = 1000044002;
inline constexpr int32_t kVkStypePhysicalDeviceDepthStencilResolveProperties = 1000199000;

// Flags & enums
inline constexpr uint32_t kVkQueueGraphicsBit = 0x00000001u;
inline constexpr uint32_t kVkQueueComputeBit = 0x00000002u;
inline constexpr uint32_t kVkQueueSparseBindingBit = 0x00000008u;
inline constexpr uint32_t kVkImageCreateSparseBindingBit = 0x00000001u;
inline constexpr uint32_t kVkImageCreateSparseResidencyBit = 0x00000002u;
inline constexpr uint32_t kVkSparseImageFormatSingleMiptailBit = 0x00000001u;
inline constexpr uint32_t kVkSparseImageFormatAlignedMipSizeBit = 0x00000002u;
inline constexpr uint32_t kVkSparseImageFormatNonstandardBlockSizeBit = 0x00000004u;
inline constexpr uint32_t kVkSparseMemoryBindMetadataBit = 0x00000001u;
inline constexpr uint32_t kVkFeatMultiDrawIndirect = 9u;
inline constexpr uint32_t kVkFeatDrawIndirectFirstInstance = 10u;
inline constexpr uint32_t kVkFeatDepthBounds = 14u;
inline constexpr uint32_t kVkFeatTextureCompressionBC = 22u;
inline constexpr uint32_t kVkFeatFragmentStoresAndAtomics = 26u;
inline constexpr uint32_t kVkFeatShaderResourceResidency = 42u;
inline constexpr uint32_t kVkFeatShaderResourceMinLod = 43u;
inline constexpr uint32_t kVkFeatSparseBinding = 44u;
inline constexpr uint32_t kVkFeatSparseResidencyBuffer = 45u;
inline constexpr uint32_t kVkFeatSparseResidencyImage2D = 46u;
inline constexpr uint32_t kVkFeatSparseResidencyImage3D = 47u;
inline constexpr uint32_t kVkFeat12DrawIndirectCount = 1u;
inline constexpr uint32_t kVkFeat12DescriptorIndexing = 9u;
inline constexpr uint32_t kVkFeat12SamplerFilterMinmax = 30u;
inline constexpr uint32_t kVkFeat12TimelineSemaphore = 37u;
inline constexpr uint32_t kVkFeat12BufferDeviceAddress = 38u;
inline constexpr uint32_t kVkFeat13Synchronization2 = 9u;
inline constexpr uint32_t kVkFeat13DynamicRendering = 12u;
inline constexpr uint32_t kVkFeat13Maintenance4 = 14u;
inline constexpr uint32_t kVkMemoryPropertyDeviceLocalBit = 0x00000001u;
inline constexpr uint32_t kVkMemoryPropertyHostVisibleBit = 0x00000002u;
inline constexpr uint32_t kVkMemoryPropertyHostCoherentBit = 0x00000004u;

inline constexpr uint32_t kVkBufferUsageTransferSrcBit = 0x00000001u;
inline constexpr uint32_t kVkBufferUsageTransferDstBit = 0x00000002u;
inline constexpr uint32_t kVkBufferUsageUniformBufferBit = 0x00000010u;
inline constexpr uint32_t kVkBufferUsageStorageBufferBit = 0x00000020u;
inline constexpr uint32_t kVkBufferUsageIndexBufferBit = 0x00000040u;
inline constexpr uint32_t kVkBufferUsageVertexBufferBit = 0x00000080u;
inline constexpr uint32_t kVkBufferUsageIndirectBufferBit = 0x00000100u;

inline constexpr uint32_t kVkResolveModeNone = 0u;
inline constexpr uint32_t kVkResolveModeSampleZeroBit = 0x00000001u;
inline constexpr uint32_t kVkResolveModeAverageBit = 0x00000002u;
inline constexpr uint32_t kVkResolveModeMinBit = 0x00000004u;
inline constexpr uint32_t kVkResolveModeMaxBit = 0x00000008u;

inline constexpr uint32_t kVkImageUsageTransferSrcBit = 0x00000001u;
inline constexpr uint32_t kVkImageUsageTransferDstBit = 0x00000002u;
inline constexpr uint32_t kVkImageUsageSampledBit = 0x00000004u;
inline constexpr uint32_t kVkImageUsageColorAttachmentBit = 0x00000010u;
inline constexpr uint32_t kVkImageUsageDepthStencilAttachmentBit = 0x00000020u;

inline constexpr uint32_t kVkImageAspectColorBit = 0x00000001u;
inline constexpr uint32_t kVkImageAspectDepthBit = 0x00000002u;

inline constexpr int32_t kVkImageType2D = 1;
inline constexpr int32_t kVkImageType3D = 2;
inline constexpr int32_t kVkImageViewType2D = 1;
inline constexpr int32_t kVkImageViewType3D = 2;
inline constexpr int32_t kVkImageTilingOptimal = 0;

inline constexpr int32_t kVkImageLayoutUndefined = 0;
inline constexpr int32_t kVkImageLayoutColorAttachmentOptimal = 2;
inline constexpr int32_t kVkImageLayoutDepthStencilAttachmentOptimal = 3;
inline constexpr int32_t kVkImageLayoutShaderReadOnlyOptimal = 5;
inline constexpr int32_t kVkImageLayoutTransferSrcOptimal = 6;
inline constexpr int32_t kVkImageLayoutTransferDstOptimal = 7;
inline constexpr int32_t kVkImageLayoutPresentSrcKhr = 1000001002;

inline constexpr int32_t kVkFormatR8G8B8A8Unorm = 37;
inline constexpr int32_t kVkFormatB8G8R8A8Unorm = 44;
inline constexpr int32_t kVkFormatA2B10G10R10UnormPack32 = 64;
inline constexpr int32_t kVkFormatR16G16B16A16Sfloat = 97;
inline constexpr int32_t kVkFormatD32Sfloat = 126;

inline constexpr uint32_t kVkSampleCount1Bit = 1u;
inline constexpr uint32_t kVkSampleCount4Bit = 4u;

inline constexpr int32_t kVkAttachmentLoadOpLoad = 0;
inline constexpr int32_t kVkAttachmentLoadOpClear = 1;
inline constexpr int32_t kVkAttachmentLoadOpDontCare = 2;
inline constexpr int32_t kVkAttachmentStoreOpStore = 0;
inline constexpr int32_t kVkAttachmentStoreOpDontCare = 1;

inline constexpr int32_t kVkSharingModeExclusive = 0;
inline constexpr int32_t kVkDescriptorTypeSampler = 0;
inline constexpr int32_t kVkDescriptorTypeCombinedImageSampler = 1;
inline constexpr int32_t kVkDescriptorTypeSampledImage = 2;
inline constexpr int32_t kVkDescriptorTypeUniformBuffer = 6;
inline constexpr int32_t kVkDescriptorTypeStorageBuffer = 7;

inline constexpr uint32_t kVkShaderStageVertexBit = 0x00000001u;
inline constexpr uint32_t kVkShaderStageFragmentBit = 0x00000010u;
inline constexpr uint32_t kVkShaderStageComputeBit = 0x00000020u;
inline constexpr uint32_t kVkShaderStageTaskBitExt = 0x00000040u;
inline constexpr uint32_t kVkShaderStageMeshBitExt = 0x00000080u;
inline constexpr uint32_t kVkShaderStageAllGraphics = 0x0000001Fu;

inline constexpr int32_t kVkPipelineBindPointGraphics = 0;
inline constexpr int32_t kVkPipelineBindPointCompute = 1;
inline constexpr int32_t kVkPrimitiveTopologyTriangleList = 3;
inline constexpr int32_t kVkPolygonModeFill = 0;
inline constexpr uint32_t kVkCullModeNone = 0u;
inline constexpr int32_t kVkFrontFaceCounterClockwise = 0;
inline constexpr int32_t kVkCompareOpLess = 1;
inline constexpr int32_t kVkDynamicStateViewport = 0;
inline constexpr int32_t kVkDynamicStateScissor = 1;
inline constexpr int32_t kVkSubpassContentsInline = 0;
inline constexpr int32_t kVkIndexTypeUint32 = 1;
inline constexpr int32_t kVkVertexInputRateVertex = 0;
inline constexpr int32_t kVkFilterNearest = 0;
inline constexpr int32_t kVkFilterLinear = 1;
inline constexpr int32_t kVkSamplerMipmapModeNearest = 0;
inline constexpr int32_t kVkSamplerMipmapModeLinear = 1;
inline constexpr int32_t kVkSamplerAddressModeRepeat = 0;
inline constexpr int32_t kVkSamplerAddressModeMirroredRepeat = 1;
inline constexpr int32_t kVkSamplerAddressModeClampToEdge = 2;
inline constexpr int32_t kVkSamplerAddressModeClampToBorder = 3;
inline constexpr int32_t kVkSamplerAddressModeMirrorClampToEdge = 4;
inline constexpr int32_t kVkBorderColorFloatTransparentBlack = 0;
inline constexpr int32_t kVkBorderColorIntTransparentBlack = 1;
inline constexpr int32_t kVkBorderColorFloatOpaqueBlack = 2;
inline constexpr int32_t kVkBorderColorIntOpaqueBlack = 3;
inline constexpr int32_t kVkBorderColorFloatOpaqueWhite = 4;
inline constexpr int32_t kVkBorderColorIntOpaqueWhite = 5;
inline constexpr int32_t kVkBorderColorFloatCustomExt = 1000287003;
inline constexpr int32_t kVkBorderColorIntCustomExt = 1000287004;
inline constexpr int32_t kVkPresentModeFifoKhr = 2;
inline constexpr uint32_t kVkCompositeAlphaOpaqueBitKhr = 0x00000001u;

inline constexpr uint32_t kVkCommandPoolCreateResetCmdBufBit = 0x00000002u;
inline constexpr int32_t kVkCommandBufferLevelPrimary = 0;
inline constexpr uint32_t kVkCommandBufferUsageOneTimeSubmitBit = 0x00000001u;

inline constexpr uint32_t kVkPipelineStageTopOfPipeBit = 0x00000001u;
inline constexpr uint32_t kVkPipelineStageVertexShaderBit = 0x00000008u;
inline constexpr uint32_t kVkPipelineStageFragmentShaderBit = 0x00000080u;
inline constexpr uint32_t kVkPipelineStageEarlyFragmentTestsBit = 0x00000100u;
inline constexpr uint32_t kVkPipelineStageLateFragmentTestsBit = 0x00000200u;
inline constexpr uint32_t kVkPipelineStageColorAttachmentOutputBit = 0x00000400u;
inline constexpr uint32_t kVkPipelineStageComputeShaderBit = 0x00000800u;
inline constexpr uint32_t kVkPipelineStageTransferBit = 0x00001000u;
inline constexpr uint32_t kVkPipelineStageBottomOfPipeBit = 0x00002000u;
inline constexpr uint32_t kVkPipelineStageHostBit = 0x00004000u;
inline constexpr uint32_t kVkPipelineStageAllGraphicsBit = 0x00008000u;
inline constexpr uint32_t kVkPipelineStageAllCommandsBit = 0x00010000u;

inline constexpr uint32_t kVkAccessShaderReadBit = 0x00000020u;
inline constexpr uint32_t kVkAccessShaderWriteBit = 0x00000040u;
inline constexpr uint32_t kVkAccessColorAttachmentReadBit = 0x00000080u;
inline constexpr uint32_t kVkAccessColorAttachmentWriteBit = 0x00000100u;
inline constexpr uint32_t kVkAccessDepthStencilAttachmentReadBit = 0x00000200u;
inline constexpr uint32_t kVkAccessDepthStencilAttachmentWriteBit = 0x00000400u;
inline constexpr uint32_t kVkAccessTransferReadBit = 0x00000800u;
inline constexpr uint32_t kVkAccessTransferWriteBit = 0x00001000u;
inline constexpr uint32_t kVkAccessHostReadBit = 0x00002000u;
inline constexpr uint32_t kVkAccessHostWriteBit = 0x00004000u;
inline constexpr uint32_t kVkAccessMemoryReadBit = 0x00008000u;
inline constexpr uint32_t kVkAccessMemoryWriteBit = 0x00010000u;
inline constexpr uint32_t kVkMemoryHeapDeviceLocalBit = 0x00000001u;

struct VkApplicationInfo {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct VkExtensionProperties {
    char extensionName[256];
    uint32_t specVersion;
};

struct VkPhysicalDeviceFeatures {
    VkBool32 fields[55];
};

struct VkPhysicalDeviceFeatures2 {
    int32_t sType;
    void* pNext;
    VkPhysicalDeviceFeatures features;
};

struct VkPhysicalDeviceMeshShaderFeaturesEXT {
    int32_t sType;
    void* pNext;
    VkBool32 taskShader;
    VkBool32 meshShader;
    VkBool32 multiviewMeshShader;
    VkBool32 primitiveFragmentShadingRateMeshShader;
    VkBool32 meshShaderQueries;
};

struct VkPhysicalDeviceVulkan12Features {
    int32_t sType;
    void* pNext;
    VkBool32 fields[47];
};
static_assert(sizeof(VkPhysicalDeviceVulkan12Features) == 208, "VkPhysicalDeviceVulkan12Features x64 size");

struct VkPhysicalDeviceVulkan13Features {
    int32_t sType;
    void* pNext;
    VkBool32 fields[15];
};
static_assert(sizeof(VkPhysicalDeviceVulkan13Features) == 80, "VkPhysicalDeviceVulkan13Features x64 size");

struct VkPhysicalDeviceCustomBorderColorFeaturesEXT {
    int32_t sType;
    void* pNext;
    VkBool32 customBorderColors;
    VkBool32 customBorderColorWithoutFormat;
};
static_assert(sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT) == 24, "VkPhysicalDeviceCustomBorderColorFeaturesEXT x64 size");

struct VkPhysicalDeviceCustomBorderColorPropertiesEXT {
    int32_t sType;
    void* pNext;
    uint32_t maxCustomBorderColorSamplers;
    uint32_t pad;
};
static_assert(sizeof(VkPhysicalDeviceCustomBorderColorPropertiesEXT) == 24, "VkPhysicalDeviceCustomBorderColorPropertiesEXT x64 size");

struct VkPhysicalDeviceSampleLocationsPropertiesEXT {
    int32_t sType;
    void* pNext;
    VkFlags sampleLocationSampleCounts;
    uint32_t maxSampleLocationGridSizeWidth;
    uint32_t maxSampleLocationGridSizeHeight;
    float sampleLocationCoordinateRange[2];
    uint32_t sampleLocationSubPixelBits;
    VkBool32 variableSampleLocations;
    uint32_t pad;
};
static_assert(sizeof(VkPhysicalDeviceSampleLocationsPropertiesEXT) == 48, "VkPhysicalDeviceSampleLocationsPropertiesEXT x64 size");

struct VkSamplerCustomBorderColorCreateInfoEXT {
    int32_t sType;
    const void* pNext;
    union {
        float float32[4];
        int32_t int32[4];
        uint32_t uint32[4];
    } customBorderColor;
    int32_t format;
};
static_assert(sizeof(VkSamplerCustomBorderColorCreateInfoEXT) == 40, "VkSamplerCustomBorderColorCreateInfoEXT x64 size");

struct alignas(8) VkPhysicalDevicePropertiesRaw {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint32_t pad_to_8;
    uint8_t limits_and_sparse_storage[1024];
};
static_assert(offsetof(VkPhysicalDevicePropertiesRaw, deviceName) == 20, "VkPhysicalDeviceProperties layout");
static_assert(offsetof(VkPhysicalDevicePropertiesRaw, pipelineCacheUUID) == 276, "UUID offset");
static_assert(offsetof(VkPhysicalDevicePropertiesRaw, limits_and_sparse_storage) == 296, "limits offset");

// Byte offset of VkPhysicalDeviceLimits::maxPushConstantsSize from start of limits (9th uint32_t -> 32 bytes).
inline constexpr size_t kVkLimitsOffsetMaxPushConstantsSize = 32u;

struct VkPhysicalDeviceProperties2 {
    int32_t sType;
    void* pNext;
    VkPhysicalDevicePropertiesRaw properties;
};

struct VkPhysicalDeviceSubgroupProperties {
    int32_t sType;
    void* pNext;
    uint32_t subgroupSize;
    VkFlags supportedStages;
    VkFlags supportedOperations;
    VkBool32 quadOperationsInAllStages;
};

struct VkOffset2D { int32_t x; int32_t y; };
struct VkOffset3D { int32_t x; int32_t y; int32_t z; };
struct VkExtent2D { uint32_t width; uint32_t height; };
struct VkExtent3D { uint32_t width; uint32_t height; uint32_t depth; };
struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };

struct VkViewport {
    float x;
    float y;
    float width;
    float height;
    float minDepth;
    float maxDepth;
};

struct VkQueueFamilyProperties {
    VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
};

struct VkMemoryType {
    VkFlags propertyFlags;
    uint32_t heapIndex;
};

struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags flags;
};

struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};

struct VkDeviceQueueCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};

struct VkDeviceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};

struct VkFenceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
};

struct VkSemaphoreCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
};

struct VkCommandPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkCommandPool commandPool;
    int32_t level;
    uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const void* pInheritanceInfo;
};

struct VkSubmitInfo {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    const VkFlags* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;
};

struct VkBufferCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};

struct VkImageCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    int32_t imageType;
    int32_t format;
    VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    uint32_t samples;
    int32_t tiling;
    VkFlags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    int32_t initialLayout;
};

struct VkComponentMapping {
    int32_t r;
    int32_t g;
    int32_t b;
    int32_t a;
};

struct VkImageSubresourceRange {
    VkFlags aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageSubresourceLayers {
    VkFlags aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
};

struct VkImageViewCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkImage image;
    int32_t viewType;
    int32_t format;
    VkComponentMapping components;
    VkImageSubresourceRange subresourceRange;
};

struct VkSamplerCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    int32_t magFilter;
    int32_t minFilter;
    int32_t mipmapMode;
    int32_t addressModeU;
    int32_t addressModeV;
    int32_t addressModeW;
    float mipLodBias;
    VkBool32 anisotropyEnable;
    float maxAnisotropy;
    VkBool32 compareEnable;
    int32_t compareOp;
    float minLod;
    float maxLod;
    int32_t borderColor;
    VkBool32 unnormalizedCoordinates;
};

struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};

struct VkMappedMemoryRange {
    int32_t sType;
    const void* pNext;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct VkShaderModuleCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    size_t codeSize;
    const uint32_t* pCode;
};

struct VkDescriptorSetLayoutBinding {
    uint32_t binding;
    int32_t descriptorType;
    uint32_t descriptorCount;
    VkFlags stageFlags;
    const VkSampler* pImmutableSamplers;
};

struct VkDescriptorSetLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t bindingCount;
    const VkDescriptorSetLayoutBinding* pBindings;
};

struct VkPushConstantRange {
    VkFlags stageFlags;
    uint32_t offset;
    uint32_t size;
};

struct VkPipelineLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t setLayoutCount;
    const VkDescriptorSetLayout* pSetLayouts;
    uint32_t pushConstantRangeCount;
    const VkPushConstantRange* pPushConstantRanges;
};

struct VkPipelineShaderStageCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkFlags stage;
    VkShaderModule module;
    const char* pName;
    const void* pSpecializationInfo;
};

struct VkComputePipelineCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout layout;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
};

struct VkVertexInputBindingDescription {
    uint32_t binding;
    uint32_t stride;
    int32_t inputRate;
};

struct VkVertexInputAttributeDescription {
    uint32_t location;
    uint32_t binding;
    int32_t format;
    uint32_t offset;
};

struct VkPipelineVertexInputStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription* pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
};

struct VkPipelineInputAssemblyStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    int32_t topology;
    VkBool32 primitiveRestartEnable;
};

struct VkPipelineViewportStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t viewportCount;
    const VkViewport* pViewports;
    uint32_t scissorCount;
    const VkRect2D* pScissors;
};

struct VkPipelineRasterizationStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkBool32 depthClampEnable;
    VkBool32 rasterizerDiscardEnable;
    int32_t polygonMode;
    VkFlags cullMode;
    int32_t frontFace;
    VkBool32 depthBiasEnable;
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;
    float lineWidth;
};

struct VkPipelineMultisampleStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t rasterizationSamples;
    VkBool32 sampleShadingEnable;
    float minSampleShading;
    const uint32_t* pSampleMask;
    VkBool32 alphaToCoverageEnable;
    VkBool32 alphaToOneEnable;
};

struct VkStencilOpState {
    int32_t failOp;
    int32_t passOp;
    int32_t depthFailOp;
    int32_t compareOp;
    uint32_t compareMask;
    uint32_t writeMask;
    uint32_t reference;
};

struct VkPipelineDepthStencilStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkBool32 depthTestEnable;
    VkBool32 depthWriteEnable;
    int32_t depthCompareOp;
    VkBool32 depthBoundsTestEnable;
    VkBool32 stencilTestEnable;
    VkStencilOpState front;
    VkStencilOpState back;
    float minDepthBounds;
    float maxDepthBounds;
};

struct VkPipelineColorBlendAttachmentState {
    VkBool32 blendEnable;
    int32_t srcColorBlendFactor;
    int32_t dstColorBlendFactor;
    int32_t colorBlendOp;
    int32_t srcAlphaBlendFactor;
    int32_t dstAlphaBlendFactor;
    int32_t alphaBlendOp;
    VkFlags colorWriteMask;
};

struct VkPipelineColorBlendStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkBool32 logicOpEnable;
    int32_t logicOp;
    uint32_t attachmentCount;
    const VkPipelineColorBlendAttachmentState* pAttachments;
    float blendConstants[4];
};

struct VkPipelineDynamicStateCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t dynamicStateCount;
    const int32_t* pDynamicStates;
};

struct VkGraphicsPipelineCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t stageCount;
    const VkPipelineShaderStageCreateInfo* pStages;
    const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
    const void* pTessellationState;
    const VkPipelineViewportStateCreateInfo* pViewportState;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
    const VkPipelineDynamicStateCreateInfo* pDynamicState;
    VkPipelineLayout layout;
    VkRenderPass renderPass;
    uint32_t subpass;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
};

struct VkAttachmentDescription {
    VkFlags flags;
    int32_t format;
    uint32_t samples;
    int32_t loadOp;
    int32_t storeOp;
    int32_t stencilLoadOp;
    int32_t stencilStoreOp;
    int32_t initialLayout;
    int32_t finalLayout;
};

struct VkAttachmentReference {
    uint32_t attachment;
    int32_t layout;
};

struct VkSubpassDescription {
    VkFlags flags;
    int32_t pipelineBindPoint;
    uint32_t inputAttachmentCount;
    const VkAttachmentReference* pInputAttachments;
    uint32_t colorAttachmentCount;
    const VkAttachmentReference* pColorAttachments;
    const VkAttachmentReference* pResolveAttachments;
    const VkAttachmentReference* pDepthStencilAttachment;
    uint32_t preserveAttachmentCount;
    const uint32_t* pPreserveAttachments;
};

struct VkSubpassDependency {
    uint32_t srcSubpass;
    uint32_t dstSubpass;
    VkFlags srcStageMask;
    VkFlags dstStageMask;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
    VkFlags dependencyFlags;
};

struct VkRenderPassCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t attachmentCount;
    const VkAttachmentDescription* pAttachments;
    uint32_t subpassCount;
    const VkSubpassDescription* pSubpasses;
    uint32_t dependencyCount;
    const VkSubpassDependency* pDependencies;
};

struct VkFramebufferCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkRenderPass renderPass;
    uint32_t attachmentCount;
    const VkImageView* pAttachments;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
};

union VkClearColorValue {
    float float32[4];
    int32_t int32[4];
    uint32_t uint32[4];
};

struct VkClearDepthStencilValue {
    float depth;
    uint32_t stencil;
};

union VkClearValue {
    VkClearColorValue color;
    VkClearDepthStencilValue depthStencil;
};

struct VkRenderPassBeginInfo {
    int32_t sType;
    const void* pNext;
    VkRenderPass renderPass;
    VkFramebuffer framebuffer;
    VkRect2D renderArea;
    uint32_t clearValueCount;
    const VkClearValue* pClearValues;
};

struct VkDescriptorPoolSize {
    int32_t type;
    uint32_t descriptorCount;
};

struct VkDescriptorPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const VkDescriptorPoolSize* pPoolSizes;
};

struct VkDescriptorSetAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDescriptorPool descriptorPool;
    uint32_t descriptorSetCount;
    const VkDescriptorSetLayout* pSetLayouts;
};

struct VkDescriptorBufferInfo {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
};

struct VkDescriptorImageInfo {
    VkSampler sampler;
    VkImageView imageView;
    int32_t imageLayout;
};

struct VkWriteDescriptorSet {
    int32_t sType;
    const void* pNext;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    int32_t descriptorType;
    const VkDescriptorImageInfo* pImageInfo;
    const VkDescriptorBufferInfo* pBufferInfo;
    const uint64_t* pTexelBufferView;
};

struct VkMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
};

struct VkBufferMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
};

struct VkImageMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
    int32_t oldLayout;
    int32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
};

struct VkBufferImageCopy {
    VkDeviceSize bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset;
    VkExtent3D imageExtent;
};

struct VkImageResolve {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D dstOffset;
    VkExtent3D extent;
};

struct VkWin32SurfaceCreateInfoKHR {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    void* hinstance;
    void* hwnd;
};

struct VkSurfaceCapabilitiesKHR {
    uint32_t minImageCount;
    uint32_t maxImageCount;
    VkExtent2D currentExtent;
    VkExtent2D minImageExtent;
    VkExtent2D maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkFlags supportedTransforms;
    VkFlags currentTransform;
    VkFlags supportedCompositeAlpha;
    VkFlags supportedUsageFlags;
};

struct VkSurfaceFormatKHR {
    int32_t format;
    int32_t colorSpace;
};

struct VkSwapchainCreateInfoKHR {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkSurfaceKHR surface;
    uint32_t minImageCount;
    int32_t imageFormat;
    int32_t imageColorSpace;
    VkExtent2D imageExtent;
    uint32_t imageArrayLayers;
    VkFlags imageUsage;
    int32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
    VkFlags preTransform;
    VkFlags compositeAlpha;
    int32_t presentMode;
    VkBool32 clipped;
    VkSwapchainKHR oldSwapchain;
};

struct VkPresentInfoKHR {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t* pImageIndices;
    VkResult* pResults;
};

struct VkSparseImageFormatProperties {
    VkFlags aspectMask;
    VkExtent3D imageGranularity;
    VkFlags flags;
};

struct VkSparseImageMemoryRequirements {
    VkSparseImageFormatProperties formatProperties;
    uint32_t imageMipTailFirstLod;
    VkDeviceSize imageMipTailSize;
    VkDeviceSize imageMipTailOffset;
    VkDeviceSize imageMipTailStride;
};
static_assert(sizeof(VkSparseImageMemoryRequirements) == 48, "VkSparseImageMemoryRequirements x64 size");

struct VkImageSubresource {
    VkFlags aspectMask;
    uint32_t mipLevel;
    uint32_t arrayLayer;
};

struct VkSparseMemoryBind {
    VkDeviceSize resourceOffset;
    VkDeviceSize size;
    VkDeviceMemory memory;
    VkDeviceSize memoryOffset;
    VkFlags flags;
};

struct VkSparseImageMemoryBind {
    VkImageSubresource subresource;
    VkOffset3D offset;
    VkExtent3D extent;
    VkDeviceMemory memory;
    VkDeviceSize memoryOffset;
    VkFlags flags;
};
static_assert(sizeof(VkSparseImageMemoryBind) == 64, "VkSparseImageMemoryBind x64 size");

struct VkSparseBufferMemoryBindInfo {
    VkBuffer buffer;
    uint32_t bindCount;
    const VkSparseMemoryBind* pBinds;
};

struct VkSparseImageOpaqueMemoryBindInfo {
    VkImage image;
    uint32_t bindCount;
    const VkSparseMemoryBind* pBinds;
};

struct VkSparseImageMemoryBindInfo {
    VkImage image;
    uint32_t bindCount;
    const VkSparseImageMemoryBind* pBinds;
};

struct VkBindSparseInfo {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore* pWaitSemaphores;
    uint32_t bufferBindCount;
    const VkSparseBufferMemoryBindInfo* pBufferBinds;
    uint32_t imageOpaqueBindCount;
    const VkSparseImageOpaqueMemoryBindInfo* pImageOpaqueBinds;
    uint32_t imageBindCount;
    const VkSparseImageMemoryBindInfo* pImageBinds;
    uint32_t signalSemaphoreCount;
    const VkSemaphore* pSignalSemaphores;
};
static_assert(sizeof(VkBindSparseInfo) == 96, "VkBindSparseInfo x64 size");

// Vulkan 1.2/1.3/1.4 Dynamic Rendering & Depth-Stencil Resolve ABI structs (x64)
struct VkPhysicalDeviceDepthStencilResolveProperties {
    int32_t sType;
    void* pNext;
    VkFlags supportedDepthResolveModes;
    VkFlags supportedStencilResolveModes;
    VkBool32 independentResolveNone;
    VkBool32 independentResolve;
};
static_assert(sizeof(VkPhysicalDeviceDepthStencilResolveProperties) == 32,
              "VkPhysicalDeviceDepthStencilResolveProperties x64 size");

struct VkRenderingAttachmentInfo {
    int32_t sType;
    const void* pNext;
    VkImageView imageView;
    int32_t imageLayout;
    uint32_t resolveMode;
    VkImageView resolveImageView;
    int32_t resolveImageLayout;
    int32_t loadOp;
    int32_t storeOp;
    VkClearValue clearValue;
};
static_assert(sizeof(VkRenderingAttachmentInfo) == 72, "VkRenderingAttachmentInfo x64 size");

struct VkRenderingInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkRect2D renderArea;
    uint32_t layerCount;
    uint32_t viewMask;
    uint32_t colorAttachmentCount;
    const VkRenderingAttachmentInfo* pColorAttachments;
    const VkRenderingAttachmentInfo* pDepthAttachment;
    const VkRenderingAttachmentInfo* pStencilAttachment;
};
static_assert(sizeof(VkRenderingInfo) == 72, "VkRenderingInfo x64 size");

struct VkPipelineRenderingCreateInfo {
    int32_t sType;
    const void* pNext;
    uint32_t viewMask;
    uint32_t colorAttachmentCount;
    const int32_t* pColorAttachmentFormats;
    int32_t depthAttachmentFormat;
    int32_t stencilAttachmentFormat;
};
static_assert(sizeof(VkPipelineRenderingCreateInfo) == 40, "VkPipelineRenderingCreateInfo x64 size");

// Function pointer typedefs
using PFN_vkVoidFunction = void(SX_VKAPI_PTR*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(SX_VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(SX_VKAPI_PTR*)(VkDevice, const char*);
using PFN_vkEnumerateInstanceVersion = VkResult(SX_VKAPI_PTR*)(uint32_t*);
using PFN_vkEnumerateInstanceExtensionProperties = VkResult(SX_VKAPI_PTR*)(const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkCreateInstance = VkResult(SX_VKAPI_PTR*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(SX_VKAPI_PTR*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(SX_VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDevicePropertiesRaw*);
using PFN_vkGetPhysicalDeviceProperties2 = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
using PFN_vkGetPhysicalDeviceFeatures = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
using PFN_vkGetPhysicalDeviceFeatures2 = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
using PFN_vkGetPhysicalDeviceQueueFamilyProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
using PFN_vkGetPhysicalDeviceMemoryProperties = void(SX_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
using PFN_vkEnumerateDeviceExtensionProperties = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkCreateDevice = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
using PFN_vkDestroyDevice = void(SX_VKAPI_PTR*)(VkDevice, const void*);
using PFN_vkDeviceWaitIdle = VkResult(SX_VKAPI_PTR*)(VkDevice);
using PFN_vkGetDeviceQueue = void(SX_VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
using PFN_vkQueueSubmit = VkResult(SX_VKAPI_PTR*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
using PFN_vkQueueBindSparse = VkResult(SX_VKAPI_PTR*)(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence);
using PFN_vkQueueWaitIdle = VkResult(SX_VKAPI_PTR*)(VkQueue);
using PFN_vkCreateFence = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkFenceCreateInfo*, const void*, VkFence*);
using PFN_vkDestroyFence = void(SX_VKAPI_PTR*)(VkDevice, VkFence, const void*);
using PFN_vkWaitForFences = VkResult(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
using PFN_vkCreateSemaphore = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*);
using PFN_vkDestroySemaphore = void(SX_VKAPI_PTR*)(VkDevice, VkSemaphore, const void*);
using PFN_vkCreateCommandPool = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
using PFN_vkDestroyCommandPool = void(SX_VKAPI_PTR*)(VkDevice, VkCommandPool, const void*);
using PFN_vkAllocateCommandBuffers = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
using PFN_vkFreeCommandBuffers = void(SX_VKAPI_PTR*)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
using PFN_vkBeginCommandBuffer = VkResult(SX_VKAPI_PTR*)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
using PFN_vkEndCommandBuffer = VkResult(SX_VKAPI_PTR*)(VkCommandBuffer);
using PFN_vkCmdBindPipeline = void(SX_VKAPI_PTR*)(VkCommandBuffer, int32_t, VkPipeline);
using PFN_vkCmdBindDescriptorSets = void(SX_VKAPI_PTR*)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
using PFN_vkCmdDispatch = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
using PFN_vkCmdPipelineBarrier = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
using PFN_vkCreateBuffer = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*);
using PFN_vkDestroyBuffer = void(SX_VKAPI_PTR*)(VkDevice, VkBuffer, const void*);
using PFN_vkGetBufferMemoryRequirements = void(SX_VKAPI_PTR*)(VkDevice, VkBuffer, VkMemoryRequirements*);
using PFN_vkAllocateMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
using PFN_vkFreeMemory = void(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory, const void*);
using PFN_vkBindBufferMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
using PFN_vkMapMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
using PFN_vkUnmapMemory = void(SX_VKAPI_PTR*)(VkDevice, VkDeviceMemory);
using PFN_vkFlushMappedMemoryRanges = VkResult(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkMappedMemoryRange*);
using PFN_vkInvalidateMappedMemoryRanges = VkResult(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkMappedMemoryRange*);
using PFN_vkCreateShaderModule = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*);
using PFN_vkDestroyShaderModule = void(SX_VKAPI_PTR*)(VkDevice, VkShaderModule, const void*);
using PFN_vkCreateDescriptorSetLayout = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*);
using PFN_vkDestroyDescriptorSetLayout = void(SX_VKAPI_PTR*)(VkDevice, VkDescriptorSetLayout, const void*);
using PFN_vkCreatePipelineLayout = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*);
using PFN_vkDestroyPipelineLayout = void(SX_VKAPI_PTR*)(VkDevice, VkPipelineLayout, const void*);
using PFN_vkCreateComputePipelines = VkResult(SX_VKAPI_PTR*)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const void*, VkPipeline*);
using PFN_vkCreateGraphicsPipelines = VkResult(SX_VKAPI_PTR*)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const void*, VkPipeline*);
using PFN_vkDestroyPipeline = void(SX_VKAPI_PTR*)(VkDevice, VkPipeline, const void*);
using PFN_vkCreateDescriptorPool = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*);
using PFN_vkDestroyDescriptorPool = void(SX_VKAPI_PTR*)(VkDevice, VkDescriptorPool, const void*);
using PFN_vkAllocateDescriptorSets = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
using PFN_vkUpdateDescriptorSets = void(SX_VKAPI_PTR*)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*);
using PFN_vkCreateRenderPass = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkRenderPassCreateInfo*, const void*, VkRenderPass*);
using PFN_vkDestroyRenderPass = void(SX_VKAPI_PTR*)(VkDevice, VkRenderPass, const void*);
using PFN_vkCreateImage = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkImageCreateInfo*, const void*, VkImage*);
using PFN_vkDestroyImage = void(SX_VKAPI_PTR*)(VkDevice, VkImage, const void*);
using PFN_vkGetImageMemoryRequirements = void(SX_VKAPI_PTR*)(VkDevice, VkImage, VkMemoryRequirements*);
using PFN_vkGetImageSparseMemoryRequirements = void(SX_VKAPI_PTR*)(VkDevice, VkImage, uint32_t*, VkSparseImageMemoryRequirements*);
using PFN_vkBindImageMemory = VkResult(SX_VKAPI_PTR*)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
using PFN_vkCreateImageView = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkImageViewCreateInfo*, const void*, VkImageView*);
using PFN_vkDestroyImageView = void(SX_VKAPI_PTR*)(VkDevice, VkImageView, const void*);
using PFN_vkCreateSampler = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkSamplerCreateInfo*, const void*, VkSampler*);
using PFN_vkDestroySampler = void(SX_VKAPI_PTR*)(VkDevice, VkSampler, const void*);
using PFN_vkCreateFramebuffer = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkFramebufferCreateInfo*, const void*, VkFramebuffer*);
using PFN_vkDestroyFramebuffer = void(SX_VKAPI_PTR*)(VkDevice, VkFramebuffer, const void*);
using PFN_vkCmdBeginRenderPass = void(SX_VKAPI_PTR*)(VkCommandBuffer, const VkRenderPassBeginInfo*, int32_t);
using PFN_vkCmdEndRenderPass = void(SX_VKAPI_PTR*)(VkCommandBuffer);
using PFN_vkCmdBindVertexBuffers = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
using PFN_vkCmdBindIndexBuffer = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, int32_t);
using PFN_vkCmdDraw = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_vkCmdDrawIndexed = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
using PFN_vkCmdPushConstants = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*);
using PFN_vkCmdCopyBufferToImage = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkImage, int32_t, uint32_t, const VkBufferImageCopy*);
using PFN_vkCmdCopyImageToBuffer = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkImage, int32_t, VkBuffer, uint32_t, const VkBufferImageCopy*);
using PFN_vkCmdResolveImage = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkImage, int32_t, VkImage, int32_t, uint32_t, const VkImageResolve*);
using PFN_vkCmdSetViewport = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
using PFN_vkCmdSetScissor = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
using PFN_vkCmdDrawMeshTasksEXT = void(SX_VKAPI_PTR*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
using PFN_vkCmdDrawIndirect = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdDrawIndexedIndirect = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdDrawIndirectCount = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdDrawIndexedIndirectCount = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdDispatchIndirect = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize);
using PFN_vkCmdDrawMeshTasksIndirectEXT = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdDrawMeshTasksIndirectCountEXT = void(SX_VKAPI_PTR*)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
using PFN_vkCmdBeginRendering = void(SX_VKAPI_PTR*)(VkCommandBuffer, const VkRenderingInfo*);
using PFN_vkCmdEndRendering = void(SX_VKAPI_PTR*)(VkCommandBuffer);

// WSI / Swapchain function pointers
using PFN_vkCreateWin32SurfaceKHR = VkResult(SX_VKAPI_PTR*)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*);
using PFN_vkDestroySurfaceKHR = void(SX_VKAPI_PTR*)(VkInstance, VkSurfaceKHR, const void*);
using PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR = VkBool32(SX_VKAPI_PTR*)(VkPhysicalDevice, uint32_t);
using PFN_vkGetPhysicalDeviceSurfaceSupportKHR = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
using PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
using PFN_vkGetPhysicalDeviceSurfaceFormatsKHR = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
using PFN_vkGetPhysicalDeviceSurfacePresentModesKHR = VkResult(SX_VKAPI_PTR*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, int32_t*);
using PFN_vkCreateSwapchainKHR = VkResult(SX_VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*);
using PFN_vkDestroySwapchainKHR = void(SX_VKAPI_PTR*)(VkDevice, VkSwapchainKHR, const void*);
using PFN_vkGetSwapchainImagesKHR = VkResult(SX_VKAPI_PTR*)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
using PFN_vkAcquireNextImageKHR = VkResult(SX_VKAPI_PTR*)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
using PFN_vkQueuePresentKHR = VkResult(SX_VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);

// ---------------------------------------------------------------------------
// IEEE-754 binary16 (FP16) <-> FP32 & RGBA8 helpers
// ---------------------------------------------------------------------------
float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16u;
    const uint32_t exp = (h >> 10u) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x0400u) == 0) {
                m <<= 1u;
                --e;
            }
            m &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(e + 127) << 23u) | (m << 13u);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13u);
    } else {
        bits = sign | ((exp + 112u) << 23u) | (mant << 13u);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

uint16_t FloatToHalf(float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
    const int32_t exp = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127;
    const uint32_t mant = bits & 0x007FFFFFu;
    if (exp > 15) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exp < -14) {
        return sign;
    }
    const uint16_t h_exp = static_cast<uint16_t>((exp + 15) << 10);
    const uint16_t h_mant = static_cast<uint16_t>(mant >> 13u);
    return static_cast<uint16_t>(sign | h_exp | h_mant);
}

uint8_t FloatToUnorm8(float v) noexcept {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

bool IsBlockCompressedFormat(VulkanImageFormat fmt) noexcept {
    switch (fmt) {
        case VulkanImageFormat::Bc1RgbaUnorm:
        case VulkanImageFormat::Bc2Unorm:
        case VulkanImageFormat::Bc3Unorm:
        case VulkanImageFormat::Bc4Unorm:
        case VulkanImageFormat::Bc5Unorm:
        case VulkanImageFormat::Bc6hUfloat:
        case VulkanImageFormat::Bc7Unorm:
            return true;
        default:
            return false;
    }
}

uint32_t BlockBytesForFormat(VulkanImageFormat fmt) noexcept {
    switch (fmt) {
        case VulkanImageFormat::Bc1RgbaUnorm:
        case VulkanImageFormat::Bc4Unorm:
            return 8u;
        case VulkanImageFormat::Bc2Unorm:
        case VulkanImageFormat::Bc3Unorm:
        case VulkanImageFormat::Bc5Unorm:
        case VulkanImageFormat::Bc6hUfloat:
        case VulkanImageFormat::Bc7Unorm:
            return 16u;
        default:
            return 0u;
    }
}

uint32_t BytesPerTexel(VulkanImageFormat fmt) noexcept {
    switch (fmt) {
        case VulkanImageFormat::Rgba16Sfloat: return 8u;
        case VulkanImageFormat::D32Sfloat: return 4u;
        case VulkanImageFormat::Bgra8Unorm:
        case VulkanImageFormat::Rgba8Unorm:
        default: return 4u;
    }
}

size_t ComputeMipByteSize(VulkanImageFormat fmt, uint32_t width, uint32_t height, uint32_t depth) noexcept {
    const uint32_t w = std::max<uint32_t>(1u, width);
    const uint32_t h = std::max<uint32_t>(1u, height);
    const uint32_t d = std::max<uint32_t>(1u, depth);
    if (IsBlockCompressedFormat(fmt)) {
        const size_t bx = (static_cast<size_t>(w) + 3u) / 4u;
        const size_t by = (static_cast<size_t>(h) + 3u) / 4u;
        return bx * by * static_cast<size_t>(d) * BlockBytesForFormat(fmt);
    }
    return static_cast<size_t>(w) * h * d * BytesPerTexel(fmt);
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto to_lower = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(static_cast<unsigned char>(haystack[i + j])) !=
                to_lower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

std::string ReadEnvStringTrimmed(const char* name) {
#if defined(_WIN32)
    char buf[512]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) return {};
    std::string s(buf, len);
#else
    const char* v = std::getenv(name);
    if (!v) return {};
    std::string s(v);
#endif
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

bool IsBufferDescriptorKind(VulkanDescriptorKind kind) noexcept {
    return kind == VulkanDescriptorKind::UniformBuffer || kind == VulkanDescriptorKind::StorageBuffer;
}

bool IsImageOrSamplerDescriptorKind(VulkanDescriptorKind kind) noexcept {
    return kind == VulkanDescriptorKind::CombinedImageSampler ||
           kind == VulkanDescriptorKind::SampledImage ||
           kind == VulkanDescriptorKind::Sampler;
}

// Rewrites OpDecorate %var Binding <old> -> <new> in a SPIR-V module for variables matching
// the requested descriptor class (UniformConstant for image/sampler vs Uniform/StorageBuffer for buffers).
void PatchSpirvDescriptorBindingsForRemap(
    std::vector<uint32_t>& spv,
    const std::vector<VulkanDescriptorSlotRemap>& remaps) {
    if (spv.size() < 5 || remaps.empty()) return;

    // Pass 1: identify OpVariable storage class and descriptor set decorations.
    std::unordered_map<uint32_t, uint32_t> var_storage_class;
    std::unordered_map<uint32_t, uint32_t> var_desc_set;
    std::unordered_map<uint32_t, uint32_t> type_kind;  // 1=Image, 2=Sampler, 3=SampledImage, 4=Pointer
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;

    size_t pos = 5;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0 || pos + wc > spv.size()) break;
        if (op == 25u && wc >= 2) {  // OpTypeImage
            type_kind[spv[pos + 1]] = 1u;
        } else if (op == 26u && wc >= 2) {  // OpTypeSampler
            type_kind[spv[pos + 1]] = 2u;
        } else if (op == 27u && wc >= 3) {  // OpTypeSampledImage
            type_kind[spv[pos + 1]] = 3u;
        } else if (op == 32u && wc >= 4) {  // OpTypePointer
            type_kind[spv[pos + 1]] = 4u;
            ptr_pointee[spv[pos + 1]] = spv[pos + 3];
        } else if (op == 59u && wc >= 4) {  // OpVariable
            const uint32_t res_ty = spv[pos + 1];
            const uint32_t var_id = spv[pos + 2];
            const uint32_t sc = spv[pos + 3];
            var_storage_class[var_id] = sc;
            auto pit = ptr_pointee.find(res_ty);
            if (pit != ptr_pointee.end()) {
                auto kit = type_kind.find(pit->second);
                if (kit != type_kind.end()) {
                    // Encode exact UniformConstant subtype in upper 16 bits for sampler vs image disambiguation.
                    var_storage_class[var_id] = sc | (kit->second << 16u);
                }
            }
        } else if (op == 71u && wc >= 4 && spv[pos + 2] == 34u) {  // OpDecorate DescriptorSet
            var_desc_set[spv[pos + 1]] = spv[pos + 3];
        }
        pos += wc;
    }

    // Pass 2: patch OpDecorate %var Binding <orig> -> <remapped>
    pos = 5;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0 || pos + wc > spv.size()) break;
        if (op == 71u && wc >= 4 && spv[pos + 2] == 33u) {  // OpDecorate Binding
            const uint32_t var_id = spv[pos + 1];
            const uint32_t cur_binding = spv[pos + 3];
            const uint32_t dset = (var_desc_set.find(var_id) != var_desc_set.end()) ? var_desc_set[var_id] : 0u;
            if (dset == 0u) {
                auto sc_it = var_storage_class.find(var_id);
                if (sc_it != var_storage_class.end()) {
                    const uint32_t sc = sc_it->second & 0xFFFFu;
                    const uint32_t subkind = sc_it->second >> 16u;
                    for (const auto& rm : remaps) {
                        if (rm.original_binding != cur_binding) continue;
                        bool matches = false;
                        if (IsBufferDescriptorKind(rm.kind)) {
                            matches = (sc == 2u || sc == 12u);  // Uniform or StorageBuffer
                        } else if (rm.kind == VulkanDescriptorKind::CombinedImageSampler) {
                            matches = (sc == 0u && (subkind == 3u || subkind == 0u));
                        } else if (rm.kind == VulkanDescriptorKind::SampledImage) {
                            matches = (sc == 0u && (subkind == 1u || subkind == 0u));
                        } else if (rm.kind == VulkanDescriptorKind::Sampler) {
                            matches = (sc == 0u && (subkind == 2u || subkind == 0u));
                        }
                        if (matches) {
                            spv[pos + 3] = rm.remapped_binding;
                            break;
                        }
                    }
                }
            }
        }
        pos += wc;
    }
}

// Separates colliding buffer & image/sampler descriptor slots on mixed heaps deterministically.
void ResolveMixedHeapDescriptorCollisions(
    const std::vector<VulkanDescriptorBinding>& in_descriptors,
    std::vector<VulkanDescriptorBinding>& out_descriptors,
    std::vector<VulkanDescriptorSlotRemap>& out_remaps,
    std::vector<uint32_t>* spv0,
    std::vector<uint32_t>* spv1,
    std::vector<uint32_t>* spv2) {
    out_descriptors = in_descriptors;
    out_remaps.clear();
    if (out_descriptors.size() <= 1u) return;

    // Collect all currently occupied binding indices.
    std::unordered_map<uint32_t, std::vector<size_t>> by_binding;
    uint32_t max_binding = 0u;
    for (size_t i = 0; i < out_descriptors.size(); ++i) {
        by_binding[out_descriptors[i].binding].push_back(i);
        max_binding = std::max(max_binding, out_descriptors[i].binding);
    }

    uint32_t next_free = max_binding + 1u;
    auto alloc_free_slot = [&]() -> uint32_t {
        while (by_binding.find(next_free) != by_binding.end()) {
            ++next_free;
        }
        const uint32_t chosen = next_free++;
        by_binding[chosen] = {};
        return chosen;
    };

    // Resolve every colliding binding slot: preserve buffer bindings at their original slot
    // and move colliding image/sampler (or subsequent) descriptors to deterministic free slots.
    for (size_t i = 0; i < out_descriptors.size(); ++i) {
        const uint32_t orig_slot = out_descriptors[i].binding;
        auto it = by_binding.find(orig_slot);
        if (it == by_binding.end() || it->second.size() <= 1u) continue;

        auto& indices = it->second;
        // Pick one keeper: prefer the first buffer descriptor if present, otherwise first entry.
        size_t keep_idx = indices[0];
        for (size_t idx : indices) {
            if (IsBufferDescriptorKind(out_descriptors[idx].kind)) {
                keep_idx = idx;
                break;
            }
        }
        for (size_t idx : indices) {
            if (idx == keep_idx) continue;
            const uint32_t new_slot = alloc_free_slot();
            VulkanDescriptorSlotRemap rm{};
            rm.original_binding = out_descriptors[idx].binding;
            rm.remapped_binding = new_slot;
            rm.kind = out_descriptors[idx].kind;
            out_remaps.push_back(rm);
            out_descriptors[idx].binding = new_slot;
        }
        indices.clear();
        indices.push_back(keep_idx);
    }

    if (!out_remaps.empty()) {
        if (spv0 && !spv0->empty()) PatchSpirvDescriptorBindingsForRemap(*spv0, out_remaps);
        if (spv1 && !spv1->empty()) PatchSpirvDescriptorBindingsForRemap(*spv1, out_remaps);
        if (spv2 && !spv2->empty()) PatchSpirvDescriptorBindingsForRemap(*spv2, out_remaps);
    }
}

uint32_t PackA2B10G10R10(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept {
    const uint32_t r10 = (static_cast<uint32_t>(r) * 1023u + 127u) / 255u;
    const uint32_t g10 = (static_cast<uint32_t>(g) * 1023u + 127u) / 255u;
    const uint32_t b10 = (static_cast<uint32_t>(b) * 1023u + 127u) / 255u;
    const uint32_t a2 = (static_cast<uint32_t>(a) * 3u + 127u) / 255u;
    return (r10 & 0x3FFu) | ((g10 & 0x3FFu) << 10u) | ((b10 & 0x3FFu) << 20u) | ((a2 & 0x3u) << 30u);
}

// ---------------------------------------------------------------------------
// SPIR-V opcodes & constants (Khronos SPIR-V 1.0-1.6 + SPV_EXT_mesh_shader)
// ---------------------------------------------------------------------------
inline constexpr uint16_t kSpvOpExtInst = 12;
inline constexpr uint16_t kSpvOpMemoryModel = 14;
inline constexpr uint16_t kSpvOpEntryPoint = 15;
inline constexpr uint16_t kSpvOpExecutionMode = 16;
inline constexpr uint16_t kSpvOpCapability = 17;
inline constexpr uint16_t kSpvOpTypeVoid = 19;
inline constexpr uint16_t kSpvOpTypeBool = 20;
inline constexpr uint16_t kSpvOpTypeInt = 21;
inline constexpr uint16_t kSpvOpTypeFloat = 22;
inline constexpr uint16_t kSpvOpTypeVector = 23;
inline constexpr uint16_t kSpvOpTypeMatrix = 24;
inline constexpr uint16_t kSpvOpTypeImage = 25;
inline constexpr uint16_t kSpvOpTypeSampler = 26;
inline constexpr uint16_t kSpvOpTypeSampledImage = 27;
inline constexpr uint16_t kSpvOpTypeArray = 28;
inline constexpr uint16_t kSpvOpTypeRuntimeArray = 29;
inline constexpr uint16_t kSpvOpTypeStruct = 30;
inline constexpr uint16_t kSpvOpTypePointer = 32;
inline constexpr uint16_t kSpvOpTypeFunction = 33;
inline constexpr uint16_t kSpvOpConstantTrue = 41;
inline constexpr uint16_t kSpvOpConstantFalse = 42;
inline constexpr uint16_t kSpvOpConstant = 43;
inline constexpr uint16_t kSpvOpConstantComposite = 44;
inline constexpr uint16_t kSpvOpConstantNull = 46;
inline constexpr uint16_t kSpvOpFunction = 54;
inline constexpr uint16_t kSpvOpFunctionEnd = 56;
inline constexpr uint16_t kSpvOpVariable = 59;
inline constexpr uint16_t kSpvOpLoad = 61;
inline constexpr uint16_t kSpvOpStore = 62;
inline constexpr uint16_t kSpvOpAccessChain = 65;
inline constexpr uint16_t kSpvOpDecorate = 71;
inline constexpr uint16_t kSpvOpMemberDecorate = 72;
inline constexpr uint16_t kSpvOpVectorShuffle = 79;
inline constexpr uint16_t kSpvOpCompositeConstruct = 80;
inline constexpr uint16_t kSpvOpCompositeExtract = 81;
inline constexpr uint16_t kSpvOpSampledImage = 86;
inline constexpr uint16_t kSpvOpImageSampleImplicitLod = 87;
inline constexpr uint16_t kSpvOpImageSampleExplicitLod = 88;
inline constexpr uint16_t kSpvOpImageFetch = 95;
inline constexpr uint16_t kSpvOpConvertFToU = 109;
inline constexpr uint16_t kSpvOpConvertFToS = 110;
inline constexpr uint16_t kSpvOpConvertSToF = 111;
inline constexpr uint16_t kSpvOpConvertUToF = 112;
inline constexpr uint16_t kSpvOpBitcast = 124;
inline constexpr uint16_t kSpvOpFNegate = 127;
inline constexpr uint16_t kSpvOpIAdd = 128;
inline constexpr uint16_t kSpvOpFAdd = 129;
inline constexpr uint16_t kSpvOpISub = 130;
inline constexpr uint16_t kSpvOpFSub = 131;
inline constexpr uint16_t kSpvOpIMul = 132;
inline constexpr uint16_t kSpvOpFMul = 133;
inline constexpr uint16_t kSpvOpUDiv = 134;
inline constexpr uint16_t kSpvOpSDiv = 135;
inline constexpr uint16_t kSpvOpFDiv = 136;
inline constexpr uint16_t kSpvOpUMod = 137;
inline constexpr uint16_t kSpvOpVectorTimesScalar = 142;
inline constexpr uint16_t kSpvOpMatrixTimesVector = 145;
inline constexpr uint16_t kSpvOpDot = 148;
inline constexpr uint16_t kSpvOpSelect = 169;
inline constexpr uint16_t kSpvOpIEqual = 170;
inline constexpr uint16_t kSpvOpINotEqual = 171;
inline constexpr uint16_t kSpvOpUGreaterThan = 172;
inline constexpr uint16_t kSpvOpSGreaterThan = 173;
inline constexpr uint16_t kSpvOpULessThan = 176;
inline constexpr uint16_t kSpvOpSLessThan = 177;
inline constexpr uint16_t kSpvOpFOrdEqual = 180;
inline constexpr uint16_t kSpvOpFOrdLessThan = 184;
inline constexpr uint16_t kSpvOpFOrdGreaterThan = 186;
inline constexpr uint16_t kSpvOpShiftRightLogical = 194;
inline constexpr uint16_t kSpvOpShiftRightArithmetic = 195;
inline constexpr uint16_t kSpvOpShiftLeftLogical = 196;
inline constexpr uint16_t kSpvOpBitwiseOr = 197;
inline constexpr uint16_t kSpvOpBitwiseXor = 198;
inline constexpr uint16_t kSpvOpBitwiseAnd = 199;
inline constexpr uint16_t kSpvOpPhi = 245;
inline constexpr uint16_t kSpvOpSelectionMerge = 247;
inline constexpr uint16_t kSpvOpLabel = 248;
inline constexpr uint16_t kSpvOpBranch = 249;
inline constexpr uint16_t kSpvOpBranchConditional = 250;
inline constexpr uint16_t kSpvOpReturn = 253;
inline constexpr uint16_t kSpvOpEmitMeshTasksEXT = 5294;
inline constexpr uint16_t kSpvOpSetMeshOutputsEXT = 5295;

inline constexpr uint32_t kSpvExecModelVertex = 0u;
inline constexpr uint32_t kSpvExecModelFragment = 4u;
inline constexpr uint32_t kSpvExecModelGLCompute = 5u;
inline constexpr uint32_t kSpvExecModelTaskEXT = 5364u;
inline constexpr uint32_t kSpvExecModelMeshEXT = 5365u;
inline constexpr uint32_t kSpvExecModeLocalSize = 17u;

inline constexpr uint32_t kSpvDecArrayStride = 6u;
inline constexpr uint32_t kSpvDecBuiltIn = 11u;
inline constexpr uint32_t kSpvDecLocation = 30u;
inline constexpr uint32_t kSpvDecBinding = 33u;
inline constexpr uint32_t kSpvDecDescriptorSet = 34u;
inline constexpr uint32_t kSpvDecOffset = 35u;

inline constexpr uint32_t kSpvBuiltInPosition = 0u;
inline constexpr uint32_t kSpvBuiltInWorkgroupId = 26u;
inline constexpr uint32_t kSpvBuiltInLocalInvocationId = 27u;
inline constexpr uint32_t kSpvBuiltInGlobalInvocationId = 28u;
inline constexpr uint32_t kSpvBuiltInLocalInvocationIndex = 29u;
inline constexpr uint32_t kSpvBuiltInVertexIndex = 42u;
inline constexpr uint32_t kSpvBuiltInPrimitiveTriangleIndicesEXT = 5296u;

inline constexpr uint32_t kSpvStorageUniformConstant = 0u;
inline constexpr uint32_t kSpvStorageInput = 1u;
inline constexpr uint32_t kSpvStorageUniform = 2u;
inline constexpr uint32_t kSpvStorageOutput = 3u;
inline constexpr uint32_t kSpvStoragePushConstant = 9u;
inline constexpr uint32_t kSpvStorageStorageBuffer = 12u;
inline constexpr uint32_t kSpvStorageTaskPayloadWorkgroupEXT = 5402u;

bool ReadSpirvString(const std::vector<uint32_t>& words,
                     size_t start_word,
                     size_t end_word,
                     std::string& out_str,
                     size_t& out_next_word) {
    out_str.clear();
    for (size_t w = start_word; w < end_word; ++w) {
        const uint32_t val = words[w];
        for (int b = 0; b < 4; ++b) {
            const char ch = static_cast<char>((val >> (b * 8)) & 0xFFu);
            if (ch == '\0') {
                out_next_word = w + 1;
                return true;
            }
            out_str.push_back(ch);
        }
    }
    return false;
}

// Deterministic SPIR-V compute interpreter (fallback when no ICD is present).
Status ExecuteSpirvComputeSoftware(const std::vector<uint32_t>& spirv,
                                   const SpirvValidationInfo& val_info,
                                   const std::vector<uint32_t>& input_u32,
                                   std::vector<uint32_t>& output_u32,
                                   uint32_t group_x) {
    struct PointerRef {
        enum class Kind : uint8_t { None, GlobalInvocationId, InputBuffer, OutputBuffer };
        Kind kind = Kind::None;
        uint32_t index = 0;
        bool has_element = false;
    };

    const uint32_t bound = spirv[3];
    std::unordered_map<uint32_t, uint32_t> constants_u32;
    std::unordered_map<uint32_t, uint32_t> var_binding;
    std::unordered_map<uint32_t, uint32_t> var_builtin;
    uint32_t entry_func_id = 0;

    size_t pos = 5;
    size_t entry_body_start = 0;
    size_t entry_body_end = 0;
    uint32_t current_func = 0;
    size_t current_func_start = 0;

    while (pos < spirv.size()) {
        const uint32_t instr = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>(instr >> 16);
        const uint16_t op = static_cast<uint16_t>(instr & 0xFFFFu);

        if (op == kSpvOpEntryPoint && wc >= 4 && spirv[pos + 1] == kSpvExecModelGLCompute) {
            entry_func_id = spirv[pos + 2];
        } else if (op == kSpvOpDecorate && wc >= 4) {
            const uint32_t target = spirv[pos + 1];
            const uint32_t dec = spirv[pos + 2];
            if (dec == kSpvDecBinding) {
                var_binding[target] = spirv[pos + 3];
            } else if (dec == kSpvDecBuiltIn) {
                var_builtin[target] = spirv[pos + 3];
            }
        } else if (op == kSpvOpConstant && wc >= 4) {
            constants_u32[spirv[pos + 2]] = spirv[pos + 3];
        } else if (op == kSpvOpFunction && wc >= 5) {
            current_func = spirv[pos + 2];
            current_func_start = pos + wc;
        } else if (op == kSpvOpFunctionEnd) {
            if (current_func == entry_func_id && entry_func_id != 0) {
                entry_body_start = current_func_start;
                entry_body_end = pos;
            }
            current_func = 0;
        }
        pos += wc;
    }

    if (entry_func_id == 0 || entry_body_start == 0 || entry_body_start >= entry_body_end) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V compute entry function body not found");
    }

    const uint32_t local_x = std::max<uint32_t>(1, val_info.local_size_x);
    const uint32_t total_invocations = group_x * local_x;
    if (output_u32.size() < std::max<size_t>(input_u32.size(), total_invocations)) {
        output_u32.resize(std::max<size_t>(input_u32.size(), total_invocations), 0u);
    }

    std::vector<uint32_t> regs(bound, 0u);
    std::vector<PointerRef> ptrs(bound);
    for (const auto& [id, cval] : constants_u32) {
        if (id < bound) regs[id] = cval;
    }

    std::vector<PointerRef> root_ptrs(bound);
    for (const auto& [id, bi] : var_builtin) {
        if (id < bound && bi == kSpvBuiltInGlobalInvocationId) {
            root_ptrs[id] = {PointerRef::Kind::GlobalInvocationId, 0, false};
        }
    }
    for (const auto& [id, binding] : var_binding) {
        if (id < bound) {
            if (binding == 0) {
                root_ptrs[id] = {PointerRef::Kind::InputBuffer, 0, false};
            } else if (binding == 1) {
                root_ptrs[id] = {PointerRef::Kind::OutputBuffer, 0, false};
            }
        }
    }

    auto read_float = [&](uint32_t id) -> float {
        float f = 0.0f;
        std::memcpy(&f, &regs[id], sizeof(float));
        return f;
    };
    auto write_float = [&](uint32_t id, float f) {
        std::memcpy(&regs[id], &f, sizeof(float));
    };

    for (uint32_t gid_x = 0; gid_x < total_invocations; ++gid_x) {
        ptrs = root_ptrs;
        const uint32_t gid[3] = {gid_x, 0u, 0u};

        size_t ip = entry_body_start;
        while (ip < entry_body_end) {
            const uint32_t instr = spirv[ip];
            const uint16_t wc = static_cast<uint16_t>(instr >> 16);
            const uint16_t op = static_cast<uint16_t>(instr & 0xFFFFu);

            switch (op) {
                case kSpvOpLabel:
                    break;
                case kSpvOpAccessChain:
                    if (wc >= 5) {
                        const uint32_t res = spirv[ip + 2];
                        const uint32_t base = spirv[ip + 3];
                        PointerRef ref = (base < bound) ? ptrs[base] : PointerRef{};
                        if (ref.kind == PointerRef::Kind::GlobalInvocationId) {
                            const uint32_t comp_id = spirv[ip + 4];
                            ref.index = (comp_id < bound) ? regs[comp_id] : 0u;
                            ref.has_element = true;
                        } else if (ref.kind == PointerRef::Kind::InputBuffer ||
                                   ref.kind == PointerRef::Kind::OutputBuffer) {
                            const uint32_t elem_id = spirv[ip + wc - 1];
                            ref.index = (elem_id < bound) ? regs[elem_id] : 0u;
                            ref.has_element = true;
                        }
                        if (res < bound) ptrs[res] = ref;
                    }
                    break;
                case kSpvOpLoad:
                    if (wc >= 4) {
                        const uint32_t res = spirv[ip + 2];
                        const uint32_t ptr_id = spirv[ip + 3];
                        const PointerRef ref = (ptr_id < bound) ? ptrs[ptr_id] : PointerRef{};
                        uint32_t loaded = 0;
                        if (ref.kind == PointerRef::Kind::GlobalInvocationId) {
                            loaded = (ref.index < 3) ? gid[ref.index] : 0u;
                        } else if (ref.kind == PointerRef::Kind::InputBuffer) {
                            loaded = (ref.index < input_u32.size()) ? input_u32[ref.index] : 0u;
                        } else if (ref.kind == PointerRef::Kind::OutputBuffer) {
                            loaded = (ref.index < output_u32.size()) ? output_u32[ref.index] : 0u;
                        }
                        if (res < bound) regs[res] = loaded;
                    }
                    break;
                case kSpvOpStore:
                    if (wc >= 3) {
                        const uint32_t ptr_id = spirv[ip + 1];
                        const uint32_t val_id = spirv[ip + 2];
                        const PointerRef ref = (ptr_id < bound) ? ptrs[ptr_id] : PointerRef{};
                        const uint32_t val = (val_id < bound) ? regs[val_id] : 0u;
                        if (ref.kind == PointerRef::Kind::OutputBuffer && ref.index < output_u32.size()) {
                            output_u32[ref.index] = val;
                        }
                    }
                    break;
                case kSpvOpIAdd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] + regs[spirv[ip + 4]];
                    break;
                case kSpvOpISub:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] - regs[spirv[ip + 4]];
                    break;
                case kSpvOpIMul:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] * regs[spirv[ip + 4]];
                    break;
                case kSpvOpUDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const uint32_t d = regs[spirv[ip + 4]];
                        regs[spirv[ip + 2]] = (d != 0) ? (regs[spirv[ip + 3]] / d) : 0u;
                    }
                    break;
                case kSpvOpSDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const int32_t a = static_cast<int32_t>(regs[spirv[ip + 3]]);
                        const int32_t b = static_cast<int32_t>(regs[spirv[ip + 4]]);
                        regs[spirv[ip + 2]] = (b != 0) ? static_cast<uint32_t>(a / b) : 0u;
                    }
                    break;
                case kSpvOpUMod:
                    if (wc >= 5 && spirv[ip + 2] < bound) {
                        const uint32_t d = regs[spirv[ip + 4]];
                        regs[spirv[ip + 2]] = (d != 0) ? (regs[spirv[ip + 3]] % d) : 0u;
                    }
                    break;
                case kSpvOpBitwiseAnd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] & regs[spirv[ip + 4]];
                    break;
                case kSpvOpBitwiseOr:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] | regs[spirv[ip + 4]];
                    break;
                case kSpvOpBitwiseXor:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] ^ regs[spirv[ip + 4]];
                    break;
                case kSpvOpShiftLeftLogical:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] << (regs[spirv[ip + 4]] & 31u);
                    break;
                case kSpvOpShiftRightLogical:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = regs[spirv[ip + 3]] >> (regs[spirv[ip + 4]] & 31u);
                    break;
                case kSpvOpShiftRightArithmetic:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        regs[spirv[ip + 2]] = static_cast<uint32_t>(
                            static_cast<int32_t>(regs[spirv[ip + 3]]) >> (regs[spirv[ip + 4]] & 31u));
                    break;
                case kSpvOpFAdd:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) + read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFSub:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) - read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFMul:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) * read_float(spirv[ip + 4]));
                    break;
                case kSpvOpFDiv:
                    if (wc >= 5 && spirv[ip + 2] < bound)
                        write_float(spirv[ip + 2], read_float(spirv[ip + 3]) / read_float(spirv[ip + 4]));
                    break;
                case kSpvOpReturn:
                    ip = entry_body_end;
                    continue;
                default:
                    return Status::Error(StatusCode::ShaderError,
                                         "Unsupported SPIR-V opcode in software fallback interpreter");
            }
            ip += wc;
        }
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// SPIR-V Graphics & Mesh Shader Software Fallback Evaluator
// Executes real SPIR-V VS+PS and TaskEXT+MeshEXT+PS shaders deterministically
// when no Vulkan ICD or VK_EXT_mesh_shader hardware is available (e.g. CI).
// ---------------------------------------------------------------------------
struct SwVec4 {
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct SwSpvVal {
    uint32_t count = 1;  // 1..16 (scalar, vec2/3/4, or mat4)
    uint32_t u[16] = {};

    float f(uint32_t i) const noexcept {
        float out = 0.0f;
        if (i < 16) std::memcpy(&out, &u[i], sizeof(float));
        return out;
    }
    void set_f(uint32_t i, float val) noexcept {
        if (i < 16) std::memcpy(&u[i], &val, sizeof(float));
    }
};

struct SwSpvType {
    enum class Kind : uint8_t { Void, Bool, Int, Float, Vector, Matrix, Image, Sampler, SampledImage, Array, RuntimeArray, Struct, Pointer };
    Kind kind = Kind::Void;
    uint32_t elem_type = 0;
    uint32_t count = 1;
    uint32_t storage_class = 0;
    uint32_t is_signed = 0;
    std::vector<uint32_t> members;
};

struct SwSpvModule {
    const std::vector<uint32_t>* words = nullptr;
    uint32_t bound = 0;
    uint32_t entry_func_id = 0;
    uint32_t local_size[3] = {1, 1, 1};
    size_t body_start = 0;
    size_t body_end = 0;

    std::unordered_map<uint32_t, SwSpvType> types;
    std::unordered_map<uint32_t, SwSpvVal> constants;
    std::unordered_map<uint32_t, uint32_t> var_type;
    std::unordered_map<uint32_t, uint32_t> var_storage;
    std::unordered_map<uint32_t, uint32_t> dec_location;
    std::unordered_map<uint32_t, uint32_t> dec_binding;
    std::unordered_map<uint32_t, uint32_t> dec_builtin;
    std::unordered_map<uint32_t, uint32_t> dec_array_stride;
    std::unordered_map<uint64_t, uint32_t> member_offset;
    std::unordered_map<uint64_t, uint32_t> member_builtin;
    std::unordered_map<uint32_t, size_t> label_ip;

    bool Parse(const std::vector<uint32_t>& spv, uint32_t expected_exec_model) {
        words = &spv;
        if (spv.size() < 5) return false;
        bound = spv[3];
        size_t pos = 5;
        uint32_t cur_fn = 0;
        size_t cur_fn_start = 0;
        while (pos < spv.size()) {
            const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
            const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
            if (wc == 0 || pos + wc > spv.size()) return false;
            switch (op) {
                case kSpvOpEntryPoint:
                    if (wc >= 4 && spv[pos + 1] == expected_exec_model) {
                        entry_func_id = spv[pos + 2];
                    }
                    break;
                case kSpvOpExecutionMode:
                    if (wc >= 6 && spv[pos + 2] == kSpvExecModeLocalSize) {
                        local_size[0] = std::max<uint32_t>(1, spv[pos + 3]);
                        local_size[1] = std::max<uint32_t>(1, spv[pos + 4]);
                        local_size[2] = std::max<uint32_t>(1, spv[pos + 5]);
                    }
                    break;
                case kSpvOpDecorate:
                    if (wc >= 4) {
                        if (spv[pos + 2] == kSpvDecLocation) dec_location[spv[pos + 1]] = spv[pos + 3];
                        else if (spv[pos + 2] == kSpvDecBinding) dec_binding[spv[pos + 1]] = spv[pos + 3];
                        else if (spv[pos + 2] == kSpvDecBuiltIn) dec_builtin[spv[pos + 1]] = spv[pos + 3];
                        else if (spv[pos + 2] == kSpvDecArrayStride) dec_array_stride[spv[pos + 1]] = spv[pos + 3];
                    }
                    break;
                case kSpvOpMemberDecorate:
                    if (wc >= 5) {
                        const uint64_t k = (static_cast<uint64_t>(spv[pos + 1]) << 32) | spv[pos + 2];
                        if (spv[pos + 3] == kSpvDecOffset) member_offset[k] = spv[pos + 4];
                        else if (spv[pos + 3] == kSpvDecBuiltIn) member_builtin[k] = spv[pos + 4];
                    }
                    break;
                case kSpvOpTypeVoid:
                    types[spv[pos + 1]] = {SwSpvType::Kind::Void, 0, 0, 0, 0, {}};
                    break;
                case kSpvOpTypeBool:
                    types[spv[pos + 1]] = {SwSpvType::Kind::Bool, 0, 1, 0, 0, {}};
                    break;
                case kSpvOpTypeInt:
                    if (wc >= 4) types[spv[pos + 1]] = {SwSpvType::Kind::Int, 0, 1, 0, spv[pos + 3], {}};
                    break;
                case kSpvOpTypeFloat:
                    types[spv[pos + 1]] = {SwSpvType::Kind::Float, 0, 1, 0, 0, {}};
                    break;
                case kSpvOpTypeVector:
                    if (wc >= 4) types[spv[pos + 1]] = {SwSpvType::Kind::Vector, spv[pos + 2], spv[pos + 3], 0, 0, {}};
                    break;
                case kSpvOpTypeMatrix:
                    if (wc >= 4) types[spv[pos + 1]] = {SwSpvType::Kind::Matrix, spv[pos + 2], spv[pos + 3], 0, 0, {}};
                    break;
                case kSpvOpTypeImage:
                    types[spv[pos + 1]] = {SwSpvType::Kind::Image, 0, 1, 0, 0, {}};
                    break;
                case kSpvOpTypeSampler:
                    types[spv[pos + 1]] = {SwSpvType::Kind::Sampler, 0, 1, 0, 0, {}};
                    break;
                case kSpvOpTypeSampledImage:
                    if (wc >= 3) types[spv[pos + 1]] = {SwSpvType::Kind::SampledImage, spv[pos + 2], 1, 0, 0, {}};
                    break;
                case kSpvOpTypeArray:
                    if (wc >= 4) {
                        uint32_t len = 1;
                        auto cit = constants.find(spv[pos + 3]);
                        if (cit != constants.end()) len = cit->second.u[0];
                        types[spv[pos + 1]] = {SwSpvType::Kind::Array, spv[pos + 2], len, 0, 0, {}};
                    }
                    break;
                case kSpvOpTypeRuntimeArray:
                    if (wc >= 3) types[spv[pos + 1]] = {SwSpvType::Kind::RuntimeArray, spv[pos + 2], 0, 0, 0, {}};
                    break;
                case kSpvOpTypeStruct: {
                    SwSpvType st{};
                    st.kind = SwSpvType::Kind::Struct;
                    for (uint16_t k = 2; k < wc; ++k) st.members.push_back(spv[pos + k]);
                    types[spv[pos + 1]] = std::move(st);
                    break;
                }
                case kSpvOpTypePointer:
                    if (wc >= 4) types[spv[pos + 1]] = {SwSpvType::Kind::Pointer, spv[pos + 3], 1, spv[pos + 2], 0, {}};
                    break;
                case kSpvOpConstantTrue: {
                    SwSpvVal v{}; v.count = 1; v.u[0] = 1u;
                    constants[spv[pos + 2]] = v;
                    break;
                }
                case kSpvOpConstantFalse:
                case kSpvOpConstantNull: {
                    SwSpvVal v{}; v.count = 1;
                    auto tit = types.find(spv[pos + 1]);
                    if (tit != types.end() && tit->second.kind == SwSpvType::Kind::Vector) v.count = tit->second.count;
                    constants[spv[pos + 2]] = v;
                    break;
                }
                case kSpvOpConstant:
                    if (wc >= 4) {
                        SwSpvVal v{}; v.count = 1; v.u[0] = spv[pos + 3];
                        constants[spv[pos + 2]] = v;
                    }
                    break;
                case kSpvOpConstantComposite: {
                    SwSpvVal v{};
                    v.count = 0;
                    for (uint16_t k = 3; k < wc && v.count < 16; ++k) {
                        const auto& sub = constants[spv[pos + k]];
                        for (uint32_t j = 0; j < sub.count && v.count < 16; ++j) v.u[v.count++] = sub.u[j];
                    }
                    constants[spv[pos + 2]] = v;
                    break;
                }
                case kSpvOpVariable:
                    if (wc >= 4) {
                        var_type[spv[pos + 2]] = spv[pos + 1];
                        var_storage[spv[pos + 2]] = spv[pos + 3];
                    }
                    break;
                case kSpvOpFunction:
                    if (wc >= 5) {
                        cur_fn = spv[pos + 2];
                        cur_fn_start = pos + wc;
                    }
                    break;
                case kSpvOpLabel:
                    if (cur_fn == entry_func_id && entry_func_id != 0 && wc >= 2) {
                        label_ip[spv[pos + 1]] = pos;
                    }
                    break;
                case kSpvOpFunctionEnd:
                    if (cur_fn == entry_func_id && entry_func_id != 0) {
                        body_start = cur_fn_start;
                        body_end = pos;
                    }
                    cur_fn = 0;
                    break;
                default:
                    break;
            }
            pos += wc;
        }
        return entry_func_id != 0 && body_start > 0 && body_start < body_end;
    }

    uint32_t TypeSizeBytes(uint32_t tid) const {
        auto it = types.find(tid);
        if (it == types.end()) return 4u;
        const auto& t = it->second;
        switch (t.kind) {
            case SwSpvType::Kind::Vector: return t.count * 4u;
            case SwSpvType::Kind::Matrix: return t.count * TypeSizeBytes(t.elem_type);
            case SwSpvType::Kind::Array: {
                auto sit = dec_array_stride.find(tid);
                uint32_t stride = (sit != dec_array_stride.end()) ? sit->second : TypeSizeBytes(t.elem_type);
                return t.count * stride;
            }
            default: return 4u;
        }
    }
};

struct SwShaderExecEnv {
    uint32_t vertex_index = 0;
    uint32_t local_index = 0;
    uint32_t workgroup_id[3] = {0, 0, 0};
    std::array<SwVec4, 8> stage_inputs{};
    std::array<SwVec4, 8> stage_outputs{};
    SwVec4 out_position = {{0.0f, 0.0f, 0.0f, 1.0f}};
    const std::vector<uint8_t>* push_constants = nullptr;
    const std::vector<VulkanDescriptorBinding>* descriptors = nullptr;

    // Mesh & Task outputs
    uint32_t mesh_vert_count = 0;
    uint32_t mesh_prim_count = 0;
    std::vector<SwVec4> mesh_positions;
    std::vector<SwVec4> mesh_param0;
    std::vector<std::array<uint32_t, 3>> mesh_indices;
    uint32_t emitted_mesh_groups[3] = {0, 0, 0};
    std::vector<uint8_t>* task_payload = nullptr;
};

// Chooses the nearest standard Vulkan border color when VK_EXT_custom_border_color
// is unsupported or explicitly bypassed via fallback mode.
VulkanBorderColorMode ChooseStandardBorderColorFallback(
    VulkanBorderColorMode requested,
    const float custom_float[4],
    const int32_t custom_int[4],
    float out_effective_float[4],
    int32_t out_effective_int[4]) noexcept {
    VulkanBorderColorMode eff = requested;
    if (requested == VulkanBorderColorMode::FloatCustom) {
        const float a = custom_float ? custom_float[3] : 0.0f;
        const float lum = custom_float
            ? (custom_float[0] + custom_float[1] + custom_float[2]) / 3.0f
            : 0.0f;
        if (a < 0.5f) {
            eff = VulkanBorderColorMode::FloatTransparentBlack;
        } else if (lum >= 0.5f) {
            eff = VulkanBorderColorMode::FloatOpaqueWhite;
        } else {
            eff = VulkanBorderColorMode::FloatOpaqueBlack;
        }
    } else if (requested == VulkanBorderColorMode::IntCustom) {
        const int32_t a = custom_int ? custom_int[3] : 0;
        const bool bright = custom_int && (custom_int[0] > 0 || custom_int[1] > 0 || custom_int[2] > 0);
        if (a == 0) {
            eff = VulkanBorderColorMode::IntTransparentBlack;
        } else if (bright) {
            eff = VulkanBorderColorMode::IntOpaqueWhite;
        } else {
            eff = VulkanBorderColorMode::IntOpaqueBlack;
        }
    }

    auto set_f = [&](float r, float g, float b, float a) {
        if (out_effective_float) {
            out_effective_float[0] = r; out_effective_float[1] = g;
            out_effective_float[2] = b; out_effective_float[3] = a;
        }
    };
    auto set_i = [&](int32_t r, int32_t g, int32_t b, int32_t a) {
        if (out_effective_int) {
            out_effective_int[0] = r; out_effective_int[1] = g;
            out_effective_int[2] = b; out_effective_int[3] = a;
        }
    };

    switch (eff) {
        case VulkanBorderColorMode::FloatOpaqueWhite:
            set_f(1.0f, 1.0f, 1.0f, 1.0f); set_i(1, 1, 1, 1); break;
        case VulkanBorderColorMode::FloatOpaqueBlack:
            set_f(0.0f, 0.0f, 0.0f, 1.0f); set_i(0, 0, 0, 1); break;
        case VulkanBorderColorMode::IntOpaqueWhite:
            set_f(1.0f, 1.0f, 1.0f, 1.0f); set_i(1, 1, 1, 1); break;
        case VulkanBorderColorMode::IntOpaqueBlack:
            set_f(0.0f, 0.0f, 0.0f, 1.0f); set_i(0, 0, 0, 1); break;
        case VulkanBorderColorMode::IntTransparentBlack:
        case VulkanBorderColorMode::FloatTransparentBlack:
        default:
            set_f(0.0f, 0.0f, 0.0f, 0.0f); set_i(0, 0, 0, 0); break;
    }
    return eff;
}

SwVec4 ResolveSwBorderColor(const VulkanDescriptorBinding* samp_desc) noexcept {
    if (!samp_desc) return {{0.0f, 0.0f, 0.0f, 1.0f}};
    if (!samp_desc->force_standard_border_fallback) {
        if (samp_desc->border_color_mode == VulkanBorderColorMode::FloatCustom) {
            return {{samp_desc->custom_border_color_float[0],
                     samp_desc->custom_border_color_float[1],
                     samp_desc->custom_border_color_float[2],
                     samp_desc->custom_border_color_float[3]}};
        }
        if (samp_desc->border_color_mode == VulkanBorderColorMode::IntCustom) {
            return {{static_cast<float>(samp_desc->custom_border_color_int[0]),
                     static_cast<float>(samp_desc->custom_border_color_int[1]),
                     static_cast<float>(samp_desc->custom_border_color_int[2]),
                     static_cast<float>(samp_desc->custom_border_color_int[3])}};
        }
    }
    float ef[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int32_t ei[4] = {0, 0, 0, 0};
    ChooseStandardBorderColorFallback(
        samp_desc->border_color_mode,
        samp_desc->custom_border_color_float,
        samp_desc->custom_border_color_int,
        ef, ei);
    return {{ef[0], ef[1], ef[2], ef[3]}};
}

SwVec4 SwSampleTexture2D(const VulkanTextureDesc& tex, float u, float v, bool linear,
                         uint32_t mip_index = 0u,
                         const VulkanDescriptorBinding* samp_desc = nullptr) {
    if (tex.mips.empty()) return {{0, 0, 0, 1}};
    const uint32_t clamped_mip = std::min<uint32_t>(mip_index, static_cast<uint32_t>(tex.mips.size() - 1u));
    const auto& mip = tex.mips[clamped_mip];
    if (mip.data.empty()) return {{0, 0, 0, 1}};
    const uint32_t w = std::max<uint32_t>(1, mip.width);
    const uint32_t h = std::max<uint32_t>(1, mip.height);

    const auto addr_u = samp_desc ? samp_desc->address_mode_u : VulkanSamplerAddressMode::ClampToEdge;
    const auto addr_v = samp_desc ? samp_desc->address_mode_v : VulkanSamplerAddressMode::ClampToEdge;
    const bool u_oob = (addr_u == VulkanSamplerAddressMode::ClampToBorder) && (u < 0.0f || u >= 1.0f);
    const bool v_oob = (addr_v == VulkanSamplerAddressMode::ClampToBorder) && (v < 0.0f || v >= 1.0f);
    if (u_oob || v_oob) {
        return ResolveSwBorderColor(samp_desc);
    }

    auto wrap_coord = [](float c, VulkanSamplerAddressMode mode) noexcept -> float {
        if (mode == VulkanSamplerAddressMode::Repeat) {
            float f = c - std::floor(c);
            return std::clamp(f, 0.0f, 0.99999f);
        }
        return std::clamp(c, 0.0f, 0.99999f);
    };
    const float cu = wrap_coord(u, addr_u);
    const float cv = wrap_coord(v, addr_v);

    auto fetch = [&](uint32_t x, uint32_t y) -> SwVec4 {
        x = std::min(x, w - 1);
        y = std::min(y, h - 1);
        const size_t idx = (static_cast<size_t>(y) * w + x) * BytesPerTexel(tex.format);
        if (tex.format == VulkanImageFormat::Rgba16Sfloat && idx + 8 <= mip.data.size()) {
            uint16_t h4[4]{};
            std::memcpy(h4, mip.data.data() + idx, 8);
            return {{HalfToFloat(h4[0]), HalfToFloat(h4[1]), HalfToFloat(h4[2]), HalfToFloat(h4[3])}};
        }
        if (idx + 4 <= mip.data.size()) {
            return {{mip.data[idx + 0] / 255.0f, mip.data[idx + 1] / 255.0f,
                     mip.data[idx + 2] / 255.0f, mip.data[idx + 3] / 255.0f}};
        }
        return {{0, 0, 0, 1}};
    };

    if (!linear) {
        return fetch(static_cast<uint32_t>(cu * w), static_cast<uint32_t>(cv * h));
    }
    const float fx = cu * w - 0.5f;
    const float fy = cv * h - 0.5f;
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(fx)), 0, static_cast<int32_t>(w - 1));
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(fy)), 0, static_cast<int32_t>(h - 1));
    const int32_t x1 = std::min<int32_t>(x0 + 1, static_cast<int32_t>(w - 1));
    const int32_t y1 = std::min<int32_t>(y0 + 1, static_cast<int32_t>(h - 1));
    const float tx = std::clamp(fx - x0, 0.0f, 1.0f);
    const float ty = std::clamp(fy - y0, 0.0f, 1.0f);
    const SwVec4 c00 = fetch(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0));
    const SwVec4 c10 = fetch(static_cast<uint32_t>(x1), static_cast<uint32_t>(y0));
    const SwVec4 c01 = fetch(static_cast<uint32_t>(x0), static_cast<uint32_t>(y1));
    const SwVec4 c11 = fetch(static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));
    SwVec4 out{};
    for (int i = 0; i < 4; ++i) {
        const float top = c00.v[i] * (1.0f - tx) + c10.v[i] * tx;
        const float bot = c01.v[i] * (1.0f - tx) + c11.v[i] * tx;
        out.v[i] = top * (1.0f - ty) + bot * ty;
    }
    return out;
}

bool ExecuteSwSpvModule(const SwSpvModule& mod, SwShaderExecEnv& env) {
    struct PtrInfo {
        uint32_t root_var = 0;
        uint32_t storage_class = 0;
        uint32_t pointee_type = 0;
        uint32_t byte_offset = 0;
        int32_t mesh_idx = -1;
        uint32_t builtin = ~0u;
        uint32_t location = ~0u;
        uint32_t binding = ~0u;
        uint32_t vec_comp = ~0u;
    };

    const auto& spv = *mod.words;
    std::vector<SwSpvVal> regs(mod.bound);
    std::vector<PtrInfo> ptrs(mod.bound);
    for (const auto& [id, cv] : mod.constants) {
        if (id < mod.bound) regs[id] = cv;
    }

    for (const auto& [vid, ptype] : mod.var_type) {
        if (vid >= mod.bound) continue;
        PtrInfo p{};
        p.root_var = vid;
        p.storage_class = mod.var_storage.at(vid);
        auto pit = mod.types.find(ptype);
        if (pit != mod.types.end()) p.pointee_type = pit->second.elem_type;
        if (auto it = mod.dec_builtin.find(vid); it != mod.dec_builtin.end()) p.builtin = it->second;
        if (auto it = mod.dec_location.find(vid); it != mod.dec_location.end()) p.location = it->second;
        if (auto it = mod.dec_binding.find(vid); it != mod.dec_binding.end()) p.binding = it->second;
        ptrs[vid] = p;
    }

    auto find_desc = [&](uint32_t binding) -> const VulkanDescriptorBinding* {
        if (!env.descriptors) return nullptr;
        for (const auto& d : *env.descriptors) {
            if (d.binding == binding) return &d;
        }
        return nullptr;
    };

    auto read_bytes = [&](const std::vector<uint8_t>* buf, uint32_t off, uint32_t type_id) -> SwSpvVal {
        SwSpvVal out{};
        if (!buf) return out;
        auto tit = mod.types.find(type_id);
        uint32_t count = 1;
        if (tit != mod.types.end()) {
            if (tit->second.kind == SwSpvType::Kind::Vector) count = tit->second.count;
            else if (tit->second.kind == SwSpvType::Kind::Matrix) count = tit->second.count * 4u;
        }
        out.count = std::min<uint32_t>(count, 16u);
        for (uint32_t i = 0; i < out.count; ++i) {
            const size_t bo = static_cast<size_t>(off) + i * 4u;
            if (bo + 4 <= buf->size()) std::memcpy(&out.u[i], buf->data() + bo, 4);
        }
        return out;
    };

    uint32_t prev_label = 0;
    uint32_t cur_label = 0;
    size_t ip = mod.body_start;
    uint32_t steps = 0;
    constexpr uint32_t kMaxSteps = 65536u;

    while (ip < mod.body_end && ++steps < kMaxSteps) {
        const uint16_t wc = static_cast<uint16_t>(spv[ip] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[ip] & 0xFFFFu);

        switch (op) {
            case kSpvOpLabel:
                prev_label = cur_label;
                cur_label = spv[ip + 1];
                break;
            case kSpvOpSelectionMerge:
                break;
            case kSpvOpPhi:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    for (uint16_t k = 3; k + 1 < wc; k += 2) {
                        if (spv[ip + k + 1] == prev_label) {
                            regs[spv[ip + 2]] = regs[spv[ip + k]];
                            break;
                        }
                    }
                }
                break;
            case kSpvOpAccessChain:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    PtrInfo p = ptrs[spv[ip + 3]];
                    uint32_t cur_ty = p.pointee_type;
                    for (uint16_t k = 4; k < wc; ++k) {
                        const uint32_t idx = regs[spv[ip + k]].u[0];
                        auto tit = mod.types.find(cur_ty);
                        if (tit == mod.types.end()) break;
                        const auto& t = tit->second;
                        if (t.kind == SwSpvType::Kind::Struct) {
                            const uint64_t mk = (static_cast<uint64_t>(cur_ty) << 32) | idx;
                            if (auto oit = mod.member_offset.find(mk); oit != mod.member_offset.end()) {
                                p.byte_offset += oit->second;
                            } else {
                                p.byte_offset += idx * 4u;
                            }
                            if (auto bit = mod.member_builtin.find(mk); bit != mod.member_builtin.end()) {
                                p.builtin = bit->second;
                            }
                            cur_ty = (idx < t.members.size()) ? t.members[idx] : 0u;
                        } else if (t.kind == SwSpvType::Kind::Array || t.kind == SwSpvType::Kind::RuntimeArray) {
                            if (p.storage_class == kSpvStorageOutput) {
                                p.mesh_idx = static_cast<int32_t>(idx);
                            } else {
                                auto sit = mod.dec_array_stride.find(cur_ty);
                                const uint32_t stride = (sit != mod.dec_array_stride.end()) ? sit->second : mod.TypeSizeBytes(t.elem_type);
                                p.byte_offset += idx * stride;
                            }
                            cur_ty = t.elem_type;
                        } else if (t.kind == SwSpvType::Kind::Vector) {
                            p.vec_comp = idx;
                            p.byte_offset += idx * 4u;
                            cur_ty = t.elem_type;
                        } else if (t.kind == SwSpvType::Kind::Matrix) {
                            p.byte_offset += idx * mod.TypeSizeBytes(t.elem_type);
                            cur_ty = t.elem_type;
                        }
                    }
                    p.pointee_type = cur_ty;
                    ptrs[spv[ip + 2]] = p;
                }
                break;
            case kSpvOpLoad:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    const uint32_t res_ty = spv[ip + 1];
                    const uint32_t res = spv[ip + 2];
                    const PtrInfo& p = ptrs[spv[ip + 3]];
                    SwSpvVal v{};
                    if (p.storage_class == kSpvStorageUniformConstant) {
                        v.count = 1;
                        v.u[0] = p.binding;  // carry descriptor binding index
                    } else if (p.storage_class == kSpvStorageInput) {
                        if (p.builtin == kSpvBuiltInVertexIndex) {
                            v.count = 1; v.u[0] = env.vertex_index;
                        } else if (p.builtin == kSpvBuiltInLocalInvocationIndex) {
                            v.count = 1; v.u[0] = env.local_index;
                        } else if (p.builtin == kSpvBuiltInWorkgroupId || p.builtin == kSpvBuiltInGlobalInvocationId) {
                            if (p.vec_comp < 3) {
                                v.count = 1; v.u[0] = env.workgroup_id[p.vec_comp];
                            } else {
                                v.count = 3;
                                v.u[0] = env.workgroup_id[0]; v.u[1] = env.workgroup_id[1]; v.u[2] = env.workgroup_id[2];
                            }
                        } else if (p.location < env.stage_inputs.size()) {
                            const auto& in4 = env.stage_inputs[p.location];
                            if (p.vec_comp < 4) {
                                v.count = 1; v.set_f(0, in4.v[p.vec_comp]);
                            } else {
                                auto tit = mod.types.find(res_ty);
                                v.count = (tit != mod.types.end() && tit->second.kind == SwSpvType::Kind::Vector) ? tit->second.count : 4u;
                                for (uint32_t c = 0; c < v.count && c < 4; ++c) v.set_f(c, in4.v[c]);
                            }
                        }
                    } else if (p.storage_class == kSpvStoragePushConstant) {
                        v = read_bytes(env.push_constants, p.byte_offset, res_ty);
                    } else if (p.storage_class == kSpvStorageUniform || p.storage_class == kSpvStorageStorageBuffer) {
                        if (const auto* d = find_desc(p.binding)) {
                            v = read_bytes(&d->buffer_data, p.byte_offset, res_ty);
                        }
                    } else if (p.storage_class == kSpvStorageTaskPayloadWorkgroupEXT) {
                        v = read_bytes(env.task_payload, p.byte_offset, res_ty);
                    }
                    regs[res] = v;
                }
                break;
            case kSpvOpStore:
                if (wc >= 3) {
                    const PtrInfo& p = ptrs[spv[ip + 1]];
                    const SwSpvVal& val = regs[spv[ip + 2]];
                    if (p.storage_class == kSpvStorageOutput) {
                        if (p.mesh_idx >= 0) {
                            const size_t idx = static_cast<size_t>(p.mesh_idx);
                            if (p.builtin == kSpvBuiltInPosition) {
                                if (idx >= env.mesh_positions.size()) env.mesh_positions.resize(idx + 1);
                                for (uint32_t c = 0; c < 4; ++c) env.mesh_positions[idx].v[c] = val.f(c);
                            } else if (p.builtin == kSpvBuiltInPrimitiveTriangleIndicesEXT) {
                                if (idx >= env.mesh_indices.size()) env.mesh_indices.resize(idx + 1);
                                env.mesh_indices[idx] = {val.u[0], val.u[1], val.u[2]};
                            } else if (p.location == 0u) {
                                if (idx >= env.mesh_param0.size()) env.mesh_param0.resize(idx + 1);
                                for (uint32_t c = 0; c < 4; ++c) env.mesh_param0[idx].v[c] = val.f(c);
                            }
                        } else if (p.builtin == kSpvBuiltInPosition) {
                            for (uint32_t c = 0; c < 4; ++c) env.out_position.v[c] = val.f(c);
                        } else if (p.location < env.stage_outputs.size()) {
                            for (uint32_t c = 0; c < 4 && c < val.count; ++c) env.stage_outputs[p.location].v[c] = val.f(c);
                        }
                    } else if (p.storage_class == kSpvStorageTaskPayloadWorkgroupEXT && env.task_payload) {
                        const size_t need = static_cast<size_t>(p.byte_offset) + val.count * 4u;
                        if (env.task_payload->size() < need) env.task_payload->resize(need, 0u);
                        for (uint32_t c = 0; c < val.count; ++c) {
                            std::memcpy(env.task_payload->data() + p.byte_offset + c * 4u, &val.u[c], 4u);
                        }
                    }
                }
                break;
            case kSpvOpCompositeConstruct:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    SwSpvVal out{}; out.count = 0;
                    for (uint16_t k = 3; k < wc && out.count < 16; ++k) {
                        const auto& s = regs[spv[ip + k]];
                        for (uint32_t j = 0; j < s.count && out.count < 16; ++j) out.u[out.count++] = s.u[j];
                    }
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpCompositeExtract:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& s = regs[spv[ip + 3]];
                    const uint32_t idx = spv[ip + 4];
                    SwSpvVal out{}; out.count = 1;
                    out.u[0] = (idx < 16) ? s.u[idx] : 0u;
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpVectorShuffle:
                if (wc >= 6 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    const auto& b = regs[spv[ip + 4]];
                    SwSpvVal out{}; out.count = 0;
                    for (uint16_t k = 5; k < wc && out.count < 4; ++k) {
                        const uint32_t idx = spv[ip + k];
                        out.u[out.count++] = (idx < a.count) ? a.u[idx] : ((idx - a.count < b.count) ? b.u[idx - a.count] : 0u);
                    }
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpSampledImage:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    SwSpvVal out{}; out.count = 2;
                    out.u[0] = regs[spv[ip + 3]].u[0];  // image binding
                    out.u[1] = regs[spv[ip + 4]].u[0];  // sampler binding
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpImageSampleImplicitLod:
            case kSpvOpImageSampleExplicitLod:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& sampled_reg = regs[spv[ip + 3]];
                    const uint32_t img_binding = sampled_reg.u[0];
                    const uint32_t samp_binding = (sampled_reg.count >= 2u) ? sampled_reg.u[1] : img_binding;
                    const auto& uv = regs[spv[ip + 4]];
                    float explicit_lod = 0.0f;
                    if (op == kSpvOpImageSampleExplicitLod && wc >= 7 && (spv[ip + 5] & 0x2u) != 0u) {
                        explicit_lod = regs[spv[ip + 6]].f(0);
                    }
                    SwVec4 c = {{0, 0, 0, 1}};
                    if (const auto* d_img = find_desc(img_binding)) {
                        const VulkanDescriptorBinding* d_samp = find_desc(samp_binding);
                        if (!d_samp) d_samp = d_img;
                        const uint32_t total_mips = std::max<uint32_t>(
                            1u, static_cast<uint32_t>(d_img->texture.mips.size()));
                        const uint32_t base_mip = std::min(d_img->view_base_mip_level, total_mips - 1u);
                        const uint32_t max_rem = total_mips - base_mip;
                        const uint32_t level_cnt = (d_img->view_mip_level_count > 0u)
                                                       ? std::min(d_img->view_mip_level_count, max_rem)
                                                       : max_rem;
                        const float biased = explicit_lod + d_samp->sampler_mip_lod_bias;
                        const float clamped_lod =
                            std::clamp(biased, d_samp->sampler_min_lod, d_samp->sampler_max_lod);
                        const int32_t rel_mip = std::clamp(
                            static_cast<int32_t>(std::floor(clamped_lod + 0.5f)),
                            0, static_cast<int32_t>(level_cnt) - 1);
                        const uint32_t abs_mip = base_mip + static_cast<uint32_t>(rel_mip);
                        c = SwSampleTexture2D(
                            d_img->texture, uv.f(0), uv.f(1), d_samp->linear_filter, abs_mip, d_samp);
                    }
                    SwSpvVal out{}; out.count = 4;
                    for (uint32_t i = 0; i < 4; ++i) out.set_f(i, c.v[i]);
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpBitcast:
                if (wc >= 4 && spv[ip + 2] < mod.bound) regs[spv[ip + 2]] = regs[spv[ip + 3]];
                break;
            case kSpvOpConvertSToF:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    SwSpvVal out{}; out.count = a.count;
                    for (uint32_t i = 0; i < a.count; ++i) out.set_f(i, static_cast<float>(static_cast<int32_t>(a.u[i])));
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpConvertUToF:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    SwSpvVal out{}; out.count = a.count;
                    for (uint32_t i = 0; i < a.count; ++i) out.set_f(i, static_cast<float>(a.u[i]));
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpFNegate:
                if (wc >= 4 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    SwSpvVal out{}; out.count = a.count;
                    for (uint32_t i = 0; i < a.count; ++i) out.set_f(i, -a.f(i));
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpFAdd:
            case kSpvOpFSub:
            case kSpvOpFMul:
            case kSpvOpFDiv:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    const auto& b = regs[spv[ip + 4]];
                    SwSpvVal out{}; out.count = std::max(a.count, b.count);
                    for (uint32_t i = 0; i < out.count; ++i) {
                        const float fa = a.f(i < a.count ? i : 0);
                        const float fb = b.f(i < b.count ? i : 0);
                        float r = 0.0f;
                        if (op == kSpvOpFAdd) r = fa + fb;
                        else if (op == kSpvOpFSub) r = fa - fb;
                        else if (op == kSpvOpFMul) r = fa * fb;
                        else r = (fb != 0.0f) ? (fa / fb) : 0.0f;
                        out.set_f(i, r);
                    }
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpVectorTimesScalar:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    const float s = regs[spv[ip + 4]].f(0);
                    SwSpvVal out{}; out.count = a.count;
                    for (uint32_t i = 0; i < a.count; ++i) out.set_f(i, a.f(i) * s);
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpMatrixTimesVector:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& m = regs[spv[ip + 3]];  // column-major 4x4
                    const auto& v = regs[spv[ip + 4]];
                    SwSpvVal out{}; out.count = 4;
                    for (uint32_t row = 0; row < 4; ++row) {
                        float sum = 0.0f;
                        for (uint32_t col = 0; col < 4; ++col) {
                            sum += m.f(col * 4 + row) * v.f(col);
                        }
                        out.set_f(row, sum);
                    }
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpDot:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    const auto& b = regs[spv[ip + 4]];
                    float d = 0.0f;
                    for (uint32_t i = 0; i < std::min(a.count, b.count); ++i) d += a.f(i) * b.f(i);
                    SwSpvVal out{}; out.count = 1; out.set_f(0, d);
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpIAdd:
            case kSpvOpISub:
            case kSpvOpIMul:
            case kSpvOpBitwiseAnd:
            case kSpvOpBitwiseOr:
            case kSpvOpBitwiseXor:
            case kSpvOpShiftLeftLogical:
            case kSpvOpShiftRightLogical:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const uint32_t a = regs[spv[ip + 3]].u[0];
                    const uint32_t b = regs[spv[ip + 4]].u[0];
                    uint32_t r = 0;
                    if (op == kSpvOpIAdd) r = a + b;
                    else if (op == kSpvOpISub) r = a - b;
                    else if (op == kSpvOpIMul) r = a * b;
                    else if (op == kSpvOpBitwiseAnd) r = a & b;
                    else if (op == kSpvOpBitwiseOr) r = a | b;
                    else if (op == kSpvOpBitwiseXor) r = a ^ b;
                    else if (op == kSpvOpShiftLeftLogical) r = a << (b & 31u);
                    else r = a >> (b & 31u);
                    SwSpvVal out{}; out.count = 1; out.u[0] = r;
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpIEqual:
            case kSpvOpINotEqual:
            case kSpvOpULessThan:
            case kSpvOpSLessThan:
            case kSpvOpUGreaterThan:
            case kSpvOpSGreaterThan:
            case kSpvOpFOrdEqual:
            case kSpvOpFOrdLessThan:
            case kSpvOpFOrdGreaterThan:
                if (wc >= 5 && spv[ip + 2] < mod.bound) {
                    const auto& a = regs[spv[ip + 3]];
                    const auto& b = regs[spv[ip + 4]];
                    bool res_b = false;
                    if (op == kSpvOpIEqual) res_b = (a.u[0] == b.u[0]);
                    else if (op == kSpvOpINotEqual) res_b = (a.u[0] != b.u[0]);
                    else if (op == kSpvOpULessThan) res_b = (a.u[0] < b.u[0]);
                    else if (op == kSpvOpSLessThan) res_b = (static_cast<int32_t>(a.u[0]) < static_cast<int32_t>(b.u[0]));
                    else if (op == kSpvOpUGreaterThan) res_b = (a.u[0] > b.u[0]);
                    else if (op == kSpvOpSGreaterThan) res_b = (static_cast<int32_t>(a.u[0]) > static_cast<int32_t>(b.u[0]));
                    else if (op == kSpvOpFOrdEqual) res_b = (a.f(0) == b.f(0));
                    else if (op == kSpvOpFOrdLessThan) res_b = (a.f(0) < b.f(0));
                    else res_b = (a.f(0) > b.f(0));
                    SwSpvVal out{}; out.count = 1; out.u[0] = res_b ? 1u : 0u;
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpSelect:
                if (wc >= 6 && spv[ip + 2] < mod.bound) {
                    const bool c = (regs[spv[ip + 3]].u[0] != 0u);
                    regs[spv[ip + 2]] = c ? regs[spv[ip + 4]] : regs[spv[ip + 5]];
                }
                break;
            case kSpvOpExtInst:
                if (wc >= 6 && spv[ip + 2] < mod.bound) {
                    const uint32_t ext_op = spv[ip + 4];
                    const auto& a = regs[spv[ip + 5]];
                    SwSpvVal out{}; out.count = a.count;
                    for (uint32_t i = 0; i < a.count; ++i) {
                        const float fa = a.f(i);
                        const float fb = (wc >= 7) ? regs[spv[ip + 6]].f(i < regs[spv[ip + 6]].count ? i : 0) : 0.0f;
                        const float fc = (wc >= 8) ? regs[spv[ip + 7]].f(i < regs[spv[ip + 7]].count ? i : 0) : 0.0f;
                        if (ext_op == 4u) out.set_f(i, std::fabs(fa));
                        else if (ext_op == 8u) out.set_f(i, std::floor(fa));
                        else if (ext_op == 10u) out.set_f(i, fa - std::floor(fa));
                        else if (ext_op == 31u) out.set_f(i, std::sqrt(std::max(0.0f, fa)));
                        else if (ext_op == 37u) out.set_f(i, std::min(fa, fb));
                        else if (ext_op == 40u) out.set_f(i, std::max(fa, fb));
                        else if (ext_op == 43u) out.set_f(i, std::clamp(fa, fb, fc));
                        else if (ext_op == 50u) out.set_f(i, fa * fb + fc);
                        else out.set_f(i, fa);
                    }
                    regs[spv[ip + 2]] = out;
                }
                break;
            case kSpvOpBranch:
                if (wc >= 2) {
                    auto it = mod.label_ip.find(spv[ip + 1]);
                    if (it != mod.label_ip.end()) { ip = it->second; continue; }
                }
                return true;
            case kSpvOpBranchConditional:
                if (wc >= 4) {
                    const uint32_t target = (regs[spv[ip + 1]].u[0] != 0u) ? spv[ip + 2] : spv[ip + 3];
                    auto it = mod.label_ip.find(target);
                    if (it != mod.label_ip.end()) { ip = it->second; continue; }
                }
                return true;
            case kSpvOpSetMeshOutputsEXT:
                if (wc >= 3) {
                    env.mesh_vert_count = regs[spv[ip + 1]].u[0];
                    env.mesh_prim_count = regs[spv[ip + 2]].u[0];
                    env.mesh_positions.resize(env.mesh_vert_count);
                    env.mesh_param0.resize(env.mesh_vert_count);
                    env.mesh_indices.resize(env.mesh_prim_count);
                }
                break;
            case kSpvOpEmitMeshTasksEXT:
                if (wc >= 4) {
                    env.emitted_mesh_groups[0] = regs[spv[ip + 1]].u[0];
                    env.emitted_mesh_groups[1] = regs[spv[ip + 2]].u[0];
                    env.emitted_mesh_groups[2] = regs[spv[ip + 3]].u[0];
                }
                return true;
            case kSpvOpReturn:
                return true;
            default:
                break;
        }
        ip += wc;
    }
    return true;
}

struct SwVertexOut {
    SwVec4 pos;
    std::array<SwVec4, 4> varyings{};
};

void RasterizeTrianglesSoftware(uint32_t width,
                                uint32_t height,
                                VulkanImageFormat color_format,
                                bool enable_depth,
                                bool enable_msaa_4x,
                                bool depth_write,
                                const float clear_color[4],
                                float clear_depth,
                                const std::vector<SwVertexOut>& verts,
                                const std::vector<std::array<uint32_t, 3>>& tris,
                                const SwSpvModule& ps_mod,
                                const std::vector<uint8_t>* push_constants,
                                const std::vector<VulkanDescriptorBinding>* descriptors,
                                FrameBufferRgba8& out_fb,
                                std::vector<uint16_t>& out_fp16) {
    const uint32_t spp = enable_msaa_4x ? 4u : 1u;
    static constexpr float kSampleOffsets4[4][2] = {
        {0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f},
    };
    static constexpr float kSampleOffsets1[1][2] = {{0.5f, 0.5f}};
    const float (*offsets)[2] = enable_msaa_4x ? kSampleOffsets4 : kSampleOffsets1;

    const size_t total_samples = static_cast<size_t>(width) * height * spp;
    std::vector<SwVec4> sample_color(total_samples,
                                     {{clear_color[0], clear_color[1], clear_color[2], clear_color[3]}});
    std::vector<float> sample_depth(total_samples, clear_depth);

    auto edge_fn = [](float ax, float ay, float bx, float by, float cx, float cy) -> float {
        return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
    };

    for (const auto& tri : tris) {
        if (tri[0] >= verts.size() || tri[1] >= verts.size() || tri[2] >= verts.size()) continue;
        const auto& v0 = verts[tri[0]];
        const auto& v1 = verts[tri[1]];
        const auto& v2 = verts[tri[2]];
        if (std::fabs(v0.pos.v[3]) < 1e-7f || std::fabs(v1.pos.v[3]) < 1e-7f || std::fabs(v2.pos.v[3]) < 1e-7f) {
            continue;
        }
        const float inv_w0 = 1.0f / v0.pos.v[3];
        const float inv_w1 = 1.0f / v1.pos.v[3];
        const float inv_w2 = 1.0f / v2.pos.v[3];

        const float sx0 = (v0.pos.v[0] * inv_w0 * 0.5f + 0.5f) * width;
        const float sy0 = (v0.pos.v[1] * inv_w0 * 0.5f + 0.5f) * height;
        const float sz0 = v0.pos.v[2] * inv_w0;

        const float sx1 = (v1.pos.v[0] * inv_w1 * 0.5f + 0.5f) * width;
        const float sy1 = (v1.pos.v[1] * inv_w1 * 0.5f + 0.5f) * height;
        const float sz1 = v1.pos.v[2] * inv_w1;

        const float sx2 = (v2.pos.v[0] * inv_w2 * 0.5f + 0.5f) * width;
        const float sy2 = (v2.pos.v[1] * inv_w2 * 0.5f + 0.5f) * height;
        const float sz2 = v2.pos.v[2] * inv_w2;

        const float area = edge_fn(sx0, sy0, sx1, sy1, sx2, sy2);
        if (std::fabs(area) < 1e-6f) continue;
        const float sign = (area > 0.0f) ? 1.0f : -1.0f;
        const float inv_area = 1.0f / area;

        const int32_t min_x = std::clamp(static_cast<int32_t>(std::floor(std::min({sx0, sx1, sx2}))), 0, static_cast<int32_t>(width) - 1);
        const int32_t max_x = std::clamp(static_cast<int32_t>(std::ceil(std::max({sx0, sx1, sx2}))), 0, static_cast<int32_t>(width) - 1);
        const int32_t min_y = std::clamp(static_cast<int32_t>(std::floor(std::min({sy0, sy1, sy2}))), 0, static_cast<int32_t>(height) - 1);
        const int32_t max_y = std::clamp(static_cast<int32_t>(std::ceil(std::max({sy0, sy1, sy2}))), 0, static_cast<int32_t>(height) - 1);

        for (int32_t py = min_y; py <= max_y; ++py) {
            for (int32_t px = min_x; px <= max_x; ++px) {
                for (uint32_t s = 0; s < spp; ++s) {
                    const float fx = static_cast<float>(px) + offsets[s][0];
                    const float fy = static_cast<float>(py) + offsets[s][1];
                    const float w0 = edge_fn(sx1, sy1, sx2, sy2, fx, fy);
                    const float w1 = edge_fn(sx2, sy2, sx0, sy0, fx, fy);
                    const float w2 = edge_fn(sx0, sy0, sx1, sy1, fx, fy);
                    if (w0 * sign < 0.0f || w1 * sign < 0.0f || w2 * sign < 0.0f) continue;

                    const float b0 = w0 * inv_area;
                    const float b1 = w1 * inv_area;
                    const float b2 = w2 * inv_area;
                    const float z = b0 * sz0 + b1 * sz1 + b2 * sz2;
                    if (z < 0.0f || z > 1.0f) continue;

                    const size_t sidx = (static_cast<size_t>(py) * width + px) * spp + s;
                    if (enable_depth && !(z < sample_depth[sidx])) continue;
                    if (enable_depth && depth_write) sample_depth[sidx] = z;

                    const float pw0 = b0 * inv_w0;
                    const float pw1 = b1 * inv_w1;
                    const float pw2 = b2 * inv_w2;
                    const float inv_pw = 1.0f / std::max(1e-7f, pw0 + pw1 + pw2);

                    SwShaderExecEnv ps_env{};
                    ps_env.push_constants = push_constants;
                    ps_env.descriptors = descriptors;
                    for (uint32_t loc = 0; loc < 4; ++loc) {
                        for (uint32_t c = 0; c < 4; ++c) {
                            ps_env.stage_inputs[loc].v[c] =
                                (pw0 * v0.varyings[loc].v[c] + pw1 * v1.varyings[loc].v[c] + pw2 * v2.varyings[loc].v[c]) * inv_pw;
                        }
                    }
                    ExecuteSwSpvModule(ps_mod, ps_env);
                    sample_color[sidx] = ps_env.stage_outputs[0];
                }
            }
        }
    }

    out_fb.width = width;
    out_fb.height = height;
    out_fb.pixels.resize(static_cast<size_t>(width) * height * 4u);
    out_fp16.clear();
    if (color_format == VulkanImageFormat::Rgba16Sfloat) {
        out_fp16.resize(static_cast<size_t>(width) * height * 4u);
    }

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float avg[4] = {0, 0, 0, 0};
            for (uint32_t s = 0; s < spp; ++s) {
                const auto& sc = sample_color[(static_cast<size_t>(y) * width + x) * spp + s];
                for (int c = 0; c < 4; ++c) avg[c] += sc.v[c];
            }
            const float inv_spp = 1.0f / static_cast<float>(spp);
            const size_t pidx = (static_cast<size_t>(y) * width + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                const float v = avg[c] * inv_spp;
                out_fb.pixels[pidx + c] = FloatToUnorm8(v);
                if (!out_fp16.empty()) out_fp16[pidx + c] = FloatToHalf(v);
            }
        }
    }
}

Status ExecuteSpirvGraphicsSoftware(const VulkanGraphicsDrawDesc& desc, VulkanGraphicsDrawResult& out_result) {
    SwSpvModule vs_mod{};
    SwSpvModule ps_mod{};
    if (!vs_mod.Parse(desc.vertex_spirv, kSpvExecModelVertex)) {
        return Status::Error(StatusCode::ShaderError, "Software fallback: invalid Vertex SPIR-V");
    }
    if (!ps_mod.Parse(desc.fragment_spirv, kSpvExecModelFragment)) {
        return Status::Error(StatusCode::ShaderError, "Software fallback: invalid Fragment SPIR-V");
    }

    const bool indexed = !desc.indices.empty();
    const uint32_t draw_count = indexed ? static_cast<uint32_t>(desc.indices.size()) : desc.vertex_count;
    const uint32_t num_verts = indexed ? (desc.vertex_stride > 0 ? static_cast<uint32_t>(desc.vertex_bytes.size() / desc.vertex_stride) : 0u) : draw_count;

    std::vector<SwVertexOut> verts(std::max<uint32_t>(num_verts, draw_count));
    auto eval_vertex = [&](uint32_t vidx) -> SwVertexOut {
        SwShaderExecEnv vs_env{};
        vs_env.vertex_index = vidx;
        vs_env.push_constants = &desc.push_constants;
        vs_env.descriptors = &desc.descriptors;
        if (!desc.vertex_bytes.empty() && desc.vertex_stride > 0) {
            const size_t vbase = static_cast<size_t>(vidx) * desc.vertex_stride;
            for (const auto& attr : desc.vertex_attributes) {
                if (attr.location >= vs_env.stage_inputs.size()) continue;
                const size_t abase = vbase + attr.offset;
                uint32_t comps = 4;
                if (attr.format == VulkanVertexFormat::R32Sfloat || attr.format == VulkanVertexFormat::R32Uint) comps = 1;
                else if (attr.format == VulkanVertexFormat::R32G32Sfloat) comps = 2;
                else if (attr.format == VulkanVertexFormat::R32G32B32Sfloat) comps = 3;
                for (uint32_t c = 0; c < comps; ++c) {
                    if (abase + (c + 1) * 4 <= desc.vertex_bytes.size()) {
                        float fv = 0.0f;
                        std::memcpy(&fv, desc.vertex_bytes.data() + abase + c * 4, 4);
                        vs_env.stage_inputs[attr.location].v[c] = fv;
                    }
                }
            }
        }
        ExecuteSwSpvModule(vs_mod, vs_env);
        SwVertexOut vo{};
        vo.pos = vs_env.out_position;
        for (uint32_t i = 0; i < 4; ++i) vo.varyings[i] = vs_env.stage_outputs[i];
        return vo;
    };

    std::vector<std::array<uint32_t, 3>> tris;
    if (indexed) {
        for (uint32_t i = 0; i < num_verts; ++i) verts[i] = eval_vertex(i);
        for (size_t i = 0; i + 2 < desc.indices.size(); i += 3) {
            tris.push_back({desc.indices[i], desc.indices[i + 1], desc.indices[i + 2]});
        }
    } else {
        for (uint32_t i = 0; i < draw_count; ++i) verts[i] = eval_vertex(i);
        for (uint32_t i = 0; i + 2 < draw_count; i += 3) {
            tris.push_back({i, i + 1, i + 2});
        }
    }

    RasterizeTrianglesSoftware(desc.width, desc.height, desc.color_format, desc.enable_depth,
                               desc.enable_msaa_4x, desc.depth_write, desc.clear_color, desc.clear_depth,
                               verts, tris, ps_mod, &desc.push_constants, &desc.descriptors,
                               out_result.framebuffer, out_result.raw_fp16_rgba);
    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.executed_on_vulkan_icd = false;
    out_result.used_msaa_4x = desc.enable_msaa_4x;
    out_result.used_depth_buffer = desc.enable_depth;
    return Status::Ok();
}

Status ExecuteSpirvMeshSoftware(const VulkanMeshDrawDesc& desc, VulkanMeshDrawResult& out_result) {
    SwSpvModule ms_mod{};
    SwSpvModule ps_mod{};
    if (!ms_mod.Parse(desc.mesh_spirv, kSpvExecModelMeshEXT)) {
        return Status::Error(StatusCode::ShaderError, "Software fallback: invalid MeshEXT SPIR-V");
    }
    if (!ps_mod.Parse(desc.fragment_spirv, kSpvExecModelFragment)) {
        return Status::Error(StatusCode::ShaderError, "Software fallback: invalid Fragment SPIR-V");
    }

    uint32_t mesh_gx = desc.group_count_x;
    uint32_t mesh_gy = desc.group_count_y;
    uint32_t mesh_gz = desc.group_count_z;
    std::vector<uint8_t> payload_bytes(64, 0u);
    const bool has_task = !desc.task_spirv.empty();

    if (has_task) {
        SwSpvModule as_mod{};
        if (!as_mod.Parse(desc.task_spirv, kSpvExecModelTaskEXT)) {
            return Status::Error(StatusCode::ShaderError, "Software fallback: invalid TaskEXT SPIR-V");
        }
        SwShaderExecEnv task_env{};
        task_env.push_constants = &desc.push_constants;
        task_env.descriptors = &desc.descriptors;
        task_env.task_payload = &payload_bytes;
        ExecuteSwSpvModule(as_mod, task_env);
        mesh_gx = task_env.emitted_mesh_groups[0];
        mesh_gy = task_env.emitted_mesh_groups[1];
        mesh_gz = task_env.emitted_mesh_groups[2];
    }

    std::vector<SwVertexOut> all_verts;
    std::vector<std::array<uint32_t, 3>> all_tris;

    for (uint32_t gz = 0; gz < mesh_gz; ++gz) {
        for (uint32_t gy = 0; gy < mesh_gy; ++gy) {
            for (uint32_t gx = 0; gx < mesh_gx; ++gx) {
                SwShaderExecEnv ms_env{};
                ms_env.workgroup_id[0] = gx;
                ms_env.workgroup_id[1] = gy;
                ms_env.workgroup_id[2] = gz;
                ms_env.push_constants = &desc.push_constants;
                ms_env.descriptors = &desc.descriptors;
                ms_env.task_payload = &payload_bytes;
                const uint32_t wg_threads = ms_mod.local_size[0] * ms_mod.local_size[1] * ms_mod.local_size[2];
                for (uint32_t lid = 0; lid < wg_threads; ++lid) {
                    ms_env.local_index = lid;
                    ExecuteSwSpvModule(ms_mod, ms_env);
                }
                const uint32_t base_v = static_cast<uint32_t>(all_verts.size());
                for (uint32_t v = 0; v < ms_env.mesh_vert_count && v < ms_env.mesh_positions.size(); ++v) {
                    SwVertexOut vo{};
                    vo.pos = ms_env.mesh_positions[v];
                    if (v < ms_env.mesh_param0.size()) vo.varyings[0] = ms_env.mesh_param0[v];
                    all_verts.push_back(vo);
                }
                for (uint32_t p = 0; p < ms_env.mesh_prim_count && p < ms_env.mesh_indices.size(); ++p) {
                    all_tris.push_back({base_v + ms_env.mesh_indices[p][0],
                                        base_v + ms_env.mesh_indices[p][1],
                                        base_v + ms_env.mesh_indices[p][2]});
                }
            }
        }
    }

    RasterizeTrianglesSoftware(desc.width, desc.height, desc.color_format, desc.enable_depth,
                               false, true, desc.clear_color, desc.clear_depth,
                               all_verts, all_tris, ps_mod, &desc.push_constants, &desc.descriptors,
                               out_result.framebuffer, out_result.raw_fp16_rgba);
    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.executed_on_mesh_shader_hw = false;
    out_result.used_task_shader = has_task;
    out_result.used_fallback = true;
    return Status::Ok();
}

int DeviceTypeRank(uint32_t device_type) noexcept {
    switch (device_type) {
        case 2: return 5;  // Discrete GPU
        case 1: return 4;  // Integrated GPU
        case 3: return 3;  // Virtual GPU
        case 4: return 2;  // CPU (Lavapipe / SwiftShader)
        default: return 1;
    }
}

}  // namespace

struct VulkanBackend::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    bool fallback_mode = false;

#if defined(_WIN32)
    HMODULE vulkan_dll = nullptr;
#endif
    VkInstance instance = nullptr;
    VkPhysicalDevice physical_device = nullptr;
    VkDevice device = nullptr;
    VkQueue compute_queue = nullptr;
    VkQueue graphics_queue = nullptr;
    VkQueue sparse_queue = nullptr;
    VkCommandPool command_pool = kVkNullHandle;
    VkCommandPool graphics_command_pool = kVkNullHandle;
    uint32_t compute_queue_family = 0;
    uint32_t graphics_queue_family = 0;
    uint32_t sparse_queue_family = 0;
    bool has_graphics_queue = false;
    bool has_sparse_queue = false;
    bool sparse_binding_enabled = false;
    bool sparse_residency_2d_enabled = false;
    bool sparse_residency_3d_enabled = false;
    bool mesh_shader_enabled = false;
    bool task_shader_enabled = false;
    bool win32_surface_enabled = false;
    bool swapchain_enabled = false;
    bool hdr10_colorspace_enabled = false;
    bool bc_compression_enabled = false;
    bool depth_bounds_enabled = false;
    bool custom_border_color_enabled = false;
    bool custom_border_color_without_format_enabled = false;
    bool multi_draw_indirect_enabled = false;
    bool draw_indirect_count_enabled = false;
    bool dynamic_rendering_enabled = false;
    uint32_t supported_depth_resolve_modes = 0;
    uint32_t supported_stencil_resolve_modes = 0;
    uint32_t max_custom_border_color_samplers = 0;
    uint32_t max_push_constants_size = 128;
    uint32_t push_constant_budget_override = 0;
    uint32_t subgroup_size = 32;
    uint32_t simulated_subgroup_size = 0;
    VkPhysicalDeviceMemoryProperties mem_props{};

    // Dynamically resolved Vulkan entry points
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueBindSparse vkQueueBindSparse = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch vkCmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

    // Graphics procedures
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkGetImageSparseMemoryRequirements vkGetImageSparseMemoryRequirements = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateSampler vkCreateSampler = nullptr;
    PFN_vkDestroySampler vkDestroySampler = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
    PFN_vkCmdDraw vkCmdDraw = nullptr;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;

    // VK_EXT_mesh_shader & Sprint 16 Indirect / Dynamic Rendering
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT = nullptr;
    PFN_vkCmdDrawIndirect vkCmdDrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect vkCmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCount = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCount = nullptr;
    PFN_vkCmdDispatchIndirect vkCmdDispatchIndirect = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT vkCmdDrawMeshTasksIndirectEXT = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT vkCmdDrawMeshTasksIndirectCountEXT = nullptr;
    PFN_vkCmdBeginRendering vkCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering vkCmdEndRendering = nullptr;

    // WSI / Swapchain
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR vkGetPhysicalDeviceWin32PresentationSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;

    struct SwapchainRecord {
        VulkanSwapchainInfo info;
        VkSurfaceKHR surface = kVkNullHandle;
        VkSwapchainKHR swapchain = kVkNullHandle;
        std::vector<VkImage> swapchain_images;
        bool supports_transfer_src = false;
        VkImage offscreen_image = kVkNullHandle;
        VkDeviceMemory offscreen_mem = kVkNullHandle;
        uint32_t frame_counter = 0;
    };
    uint64_t next_swapchain_handle = 1;
    std::unordered_map<uint64_t, SwapchainRecord> swapchains;

    struct ReservedImageRecord {
        VulkanReservedResourceDesc desc;
        VulkanResourceTilingInfo tiling;
        VkImage image = kVkNullHandle;
        VkDeviceMemory backing_memory = kVkNullHandle;
        VkDeviceSize backing_memory_size = 0;
        VkDeviceSize tile_alignment = 65536;
        uint32_t backing_tile_capacity = 0;
        bool is_icd = false;
        std::unordered_map<uint64_t, std::vector<uint8_t>> sw_tiles;
        std::unordered_map<uint64_t, bool> bound_tiles;
        bool mip_tail_bound = false;
    };
    uint64_t next_reserved_handle = 1;
    std::unordered_map<uint64_t, ReservedImageRecord> reserved_images;

    template <typename Fn>
    bool LoadInstProc(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(vkGetInstanceProcAddr(instance, name));
        return out != nullptr;
    }

    template <typename Fn>
    bool LoadDevProc(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(vkGetDeviceProcAddr(device, name));
        return out != nullptr;
    }

    uint32_t FindMemoryType(uint32_t type_bits, VkFlags required_flags, bool& out_is_coherent) const noexcept {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) != 0) {
                const VkFlags f = mem_props.memoryTypes[i].propertyFlags;
                if ((f & required_flags) == required_flags) {
                    out_is_coherent = (f & kVkMemoryPropertyHostCoherentBit) != 0;
                    return i;
                }
            }
        }
        if ((required_flags & kVkMemoryPropertyHostVisibleBit) != 0) {
            for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
                if ((type_bits & (1u << i)) != 0) {
                    const VkFlags f = mem_props.memoryTypes[i].propertyFlags;
                    if ((f & kVkMemoryPropertyHostVisibleBit) != 0) {
                        out_is_coherent = (f & kVkMemoryPropertyHostCoherentBit) != 0;
                        return i;
                    }
                }
            }
        } else {
            for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
                if ((type_bits & (1u << i)) != 0) {
                    const VkFlags f = mem_props.memoryTypes[i].propertyFlags;
                    out_is_coherent = (f & kVkMemoryPropertyHostCoherentBit) != 0;
                    return i;
                }
            }
        }
        out_is_coherent = false;
        return std::numeric_limits<uint32_t>::max();
    }

    Status CreateHostBuffer(VkDeviceSize size, VkFlags usage, VkBuffer& out_buf, VkDeviceMemory& out_mem,
                            bool& out_coherent, const void* init_data = nullptr, size_t init_bytes = 0) {
        const VkDeviceSize alloc_size = std::max<VkDeviceSize>(size, 16ull);
        VkBufferCreateInfo bci{};
        bci.sType = kVkStypeBufferCreateInfo;
        bci.size = alloc_size;
        bci.usage = usage;
        bci.sharingMode = kVkSharingModeExclusive;
        if (vkCreateBuffer(device, &bci, nullptr, &out_buf) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkCreateBuffer failed");
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, out_buf, &req);
        const uint32_t mem_idx = FindMemoryType(
            req.memoryTypeBits, kVkMemoryPropertyHostVisibleBit | kVkMemoryPropertyHostCoherentBit, out_coherent);
        if (mem_idx == std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "No HOST_VISIBLE Vulkan memory type available");
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = kVkStypeMemoryAllocateInfo;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mem_idx;
        if (vkAllocateMemory(device, &mai, nullptr, &out_mem) != kVkSuccess) {
            return Status::Error(StatusCode::OutOfMemory, "vkAllocateMemory(buffer) failed");
        }
        if (vkBindBufferMemory(device, out_buf, out_mem, 0) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkBindBufferMemory failed");
        }
        if (init_data && init_bytes > 0) {
            void* mapped = nullptr;
            if (vkMapMemory(device, out_mem, 0, alloc_size, 0, &mapped) != kVkSuccess || !mapped) {
                return Status::Error(StatusCode::DeviceError, "vkMapMemory(upload) failed");
            }
            std::memset(mapped, 0, static_cast<size_t>(alloc_size));
            std::memcpy(mapped, init_data, std::min<size_t>(init_bytes, static_cast<size_t>(alloc_size)));
            if (!out_coherent) {
                VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, out_mem, 0, kVkWholeSize};
                vkFlushMappedMemoryRanges(device, 1, &rng);
            }
            vkUnmapMemory(device, out_mem);
        }
        return Status::Ok();
    }

    Status CreateDeviceImage(int32_t image_type, int32_t format, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t mip_levels, uint32_t samples, VkFlags usage,
                             VkImage& out_img, VkDeviceMemory& out_mem) {
        VkImageCreateInfo ici{};
        ici.sType = kVkStypeImageCreateInfo;
        ici.imageType = image_type;
        ici.format = format;
        ici.extent = {std::max<uint32_t>(1, width), std::max<uint32_t>(1, height), std::max<uint32_t>(1, depth)};
        ici.mipLevels = std::max<uint32_t>(1, mip_levels);
        ici.arrayLayers = 1;
        ici.samples = samples;
        ici.tiling = kVkImageTilingOptimal;
        ici.usage = usage;
        ici.sharingMode = kVkSharingModeExclusive;
        ici.initialLayout = kVkImageLayoutUndefined;
        if (vkCreateImage(device, &ici, nullptr, &out_img) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkCreateImage failed");
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, out_img, &req);
        bool coherent = false;
        const uint32_t mem_idx = FindMemoryType(req.memoryTypeBits, kVkMemoryPropertyDeviceLocalBit, coherent);
        if (mem_idx == std::numeric_limits<uint32_t>::max()) {
            return Status::Error(StatusCode::OutOfMemory, "No compatible Vulkan memory type for image");
        }
        VkMemoryAllocateInfo mai{};
        mai.sType = kVkStypeMemoryAllocateInfo;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = mem_idx;
        if (vkAllocateMemory(device, &mai, nullptr, &out_mem) != kVkSuccess) {
            return Status::Error(StatusCode::OutOfMemory, "vkAllocateMemory(image) failed");
        }
        if (vkBindImageMemory(device, out_img, out_mem, 0) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkBindImageMemory failed");
        }
        return Status::Ok();
    }

    void CmdImageBarrier(VkCommandBuffer cmd, VkImage img, VkFlags aspect,
                         int32_t old_layout, int32_t new_layout,
                         VkFlags src_stage, VkFlags dst_stage,
                         VkFlags src_access, VkFlags dst_access,
                         uint32_t mip_levels = 1) {
        VkImageMemoryBarrier imb{};
        imb.sType = kVkStypeImageMemoryBarrier;
        imb.srcAccessMask = src_access;
        imb.dstAccessMask = dst_access;
        imb.oldLayout = old_layout;
        imb.newLayout = new_layout;
        imb.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
        imb.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
        imb.image = img;
        imb.subresourceRange = {aspect, 0u, mip_levels, 0u, 1u};
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &imb);
    }

    void DestroySwapchainRecord(SwapchainRecord& rec) {
        if (device && vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device);
        }
        if (device && rec.offscreen_image != kVkNullHandle && vkDestroyImage) {
            vkDestroyImage(device, rec.offscreen_image, nullptr);
            rec.offscreen_image = kVkNullHandle;
        }
        if (device && rec.offscreen_mem != kVkNullHandle && vkFreeMemory) {
            vkFreeMemory(device, rec.offscreen_mem, nullptr);
            rec.offscreen_mem = kVkNullHandle;
        }
        if (device && rec.swapchain != kVkNullHandle && vkDestroySwapchainKHR) {
            vkDestroySwapchainKHR(device, rec.swapchain, nullptr);
            rec.swapchain = kVkNullHandle;
        }
        if (instance && rec.surface != kVkNullHandle && vkDestroySurfaceKHR) {
            vkDestroySurfaceKHR(instance, rec.surface, nullptr);
            rec.surface = kVkNullHandle;
        }
        rec.swapchain_images.clear();
    }

    void DestroyReservedImageRecord(ReservedImageRecord& rec) {
        if (device && vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device);
        }
        if (device && rec.image != kVkNullHandle && vkDestroyImage) {
            vkDestroyImage(device, rec.image, nullptr);
            rec.image = kVkNullHandle;
        }
        if (device && rec.backing_memory != kVkNullHandle && vkFreeMemory) {
            vkFreeMemory(device, rec.backing_memory, nullptr);
            rec.backing_memory = kVkNullHandle;
        }
        rec.sw_tiles.clear();
        rec.bound_tiles.clear();
    }

    Status CreateConfiguredSampler(
        bool linear_filter,
        VulkanSamplerAddressMode addr_u,
        VulkanSamplerAddressMode addr_v,
        VulkanSamplerAddressMode addr_w,
        float mip_lod_bias,
        float min_lod,
        float max_lod,
        VulkanBorderColorMode border_mode,
        const float custom_float[4],
        const int32_t custom_int[4],
        VulkanImageFormat border_format,
        bool specify_format,
        bool force_standard_fallback,
        VkSampler& out_sampler,
        bool& out_used_custom_ext,
        bool& out_used_fallback,
        VulkanBorderColorMode& out_effective_mode,
        float out_effective_float[4],
        int32_t out_effective_int[4]) {
        out_sampler = kVkNullHandle;
        out_used_custom_ext = false;
        out_used_fallback = false;
        out_effective_mode = border_mode;

        auto to_vk_addr = [](VulkanSamplerAddressMode m) noexcept -> int32_t {
            const uint32_t v = static_cast<uint32_t>(m);
            return (v <= 4u) ? static_cast<int32_t>(v) : kVkSamplerAddressModeClampToEdge;
        };

        VkSamplerCreateInfo sci{};
        sci.sType = kVkStypeSamplerCreateInfo;
        sci.magFilter = linear_filter ? kVkFilterLinear : kVkFilterNearest;
        sci.minFilter = linear_filter ? kVkFilterLinear : kVkFilterNearest;
        sci.mipmapMode = linear_filter ? kVkSamplerMipmapModeLinear : kVkSamplerMipmapModeNearest;
        sci.addressModeU = to_vk_addr(addr_u);
        sci.addressModeV = to_vk_addr(addr_v);
        sci.addressModeW = to_vk_addr(addr_w);
        sci.mipLodBias = mip_lod_bias;
        sci.minLod = min_lod;
        sci.maxLod = max_lod;

        const bool is_custom =
            (border_mode == VulkanBorderColorMode::FloatCustom ||
             border_mode == VulkanBorderColorMode::IntCustom);

        if (is_custom && !force_standard_fallback && custom_border_color_enabled) {
            VkSamplerCustomBorderColorCreateInfoEXT cbc{};
            cbc.sType = kVkStypeSamplerCustomBorderColorCreateInfoExt;
            cbc.pNext = nullptr;
            if (border_mode == VulkanBorderColorMode::FloatCustom) {
                for (int i = 0; i < 4; ++i) {
                    const float fv = custom_float ? custom_float[i] : 0.0f;
                    cbc.customBorderColor.float32[i] = fv;
                    if (out_effective_float) out_effective_float[i] = fv;
                    if (out_effective_int) out_effective_int[i] = static_cast<int32_t>(fv);
                }
                sci.borderColor = kVkBorderColorFloatCustomExt;
            } else {
                for (int i = 0; i < 4; ++i) {
                    const int32_t iv = custom_int ? custom_int[i] : 0;
                    cbc.customBorderColor.int32[i] = iv;
                    if (out_effective_int) out_effective_int[i] = iv;
                    if (out_effective_float) out_effective_float[i] = static_cast<float>(iv);
                }
                sci.borderColor = kVkBorderColorIntCustomExt;
            }
            // Per VK_EXT_custom_border_color: format must be provided unless customBorderColorWithoutFormat is enabled.
            cbc.format = (specify_format || !custom_border_color_without_format_enabled)
                ? static_cast<int32_t>(border_format)
                : 0;  // VK_FORMAT_UNDEFINED
            sci.pNext = &cbc;

            if (vkCreateSampler(device, &sci, nullptr, &out_sampler) == kVkSuccess &&
                out_sampler != kVkNullHandle) {
                out_used_custom_ext = true;
                out_used_fallback = false;
                out_effective_mode = border_mode;
                return Status::Ok();
            }
            // Driver rejected custom border color (e.g., maxCustomBorderColorSamplers exceeded);
            // gracefully fall back to nearest standard border color below.
            sci.pNext = nullptr;
        }

        if (is_custom) {
            out_effective_mode = ChooseStandardBorderColorFallback(
                border_mode, custom_float, custom_int, out_effective_float, out_effective_int);
            sci.borderColor = static_cast<int32_t>(out_effective_mode);
            out_used_fallback = true;
        } else {
            out_effective_mode = ChooseStandardBorderColorFallback(
                border_mode, custom_float, custom_int, out_effective_float, out_effective_int);
            sci.borderColor = static_cast<int32_t>(out_effective_mode);
            out_used_fallback = false;
        }

        if (vkCreateSampler(device, &sci, nullptr, &out_sampler) != kVkSuccess ||
            out_sampler == kVkNullHandle) {
            return Status::Error(StatusCode::DeviceError, "vkCreateSampler failed");
        }
        return Status::Ok();
    }

    Status ExecuteGraphicsOrMeshOnIcd(
        bool is_mesh,
        const std::vector<uint32_t>& stage0_spirv, const std::string& stage0_entry,
        const std::vector<uint32_t>& stage1_spirv, const std::string& stage1_entry,
        const std::vector<uint32_t>& frag_spirv, const std::string& frag_entry,
        uint32_t width, uint32_t height, VulkanImageFormat color_format,
        bool enable_depth, bool enable_msaa_4x, bool depth_write,
        const float clear_color[4], float clear_depth,
        const std::vector<uint8_t>& vertex_bytes, uint32_t vertex_stride,
        const std::vector<VulkanVertexAttribute>& vertex_attributes,
        const std::vector<uint32_t>& indices, uint32_t vertex_count,
        const uint32_t mesh_groups[3],
        const std::vector<uint8_t>& push_constants,
        uint32_t effective_pc_size,
        const std::vector<VulkanDescriptorBinding>& descriptors,
        FrameBufferRgba8& out_fb, std::vector<uint16_t>& out_fp16,
        uint32_t* out_custom_border_used = nullptr,
        uint32_t* out_custom_border_fallback = nullptr) {
        if (!has_graphics_queue || !graphics_queue || graphics_command_pool == kVkNullHandle) {
            return Status::Error(StatusCode::DeviceError, "Vulkan graphics queue not initialized");
        }
        const bool has_task = is_mesh && !stage0_spirv.empty();
        const int32_t vk_color_fmt = static_cast<int32_t>(color_format);
        const uint32_t bpp = BytesPerTexel(color_format);
        const VkDeviceSize readback_bytes = static_cast<VkDeviceSize>(width) * height * bpp;
        const uint32_t samples = enable_msaa_4x ? kVkSampleCount4Bit : kVkSampleCount1Bit;

        struct DescGpu {
            VkBuffer buf = kVkNullHandle;
            VkDeviceMemory buf_mem = kVkNullHandle;
            VkImage img = kVkNullHandle;
            VkDeviceMemory img_mem = kVkNullHandle;
            VkImageView img_view = kVkNullHandle;
            VkSampler sampler = kVkNullHandle;
            VkBuffer staging = kVkNullHandle;
            VkDeviceMemory staging_mem = kVkNullHandle;
            VulkanTextureDesc tex_copy;
            bool has_upload = false;
        };

        std::vector<DescGpu> desc_gpu(descriptors.size());
        VkImage color_img = kVkNullHandle, resolve_img = kVkNullHandle, depth_img = kVkNullHandle;
        VkDeviceMemory color_mem = kVkNullHandle, resolve_mem = kVkNullHandle, depth_mem = kVkNullHandle;
        VkImageView color_view = kVkNullHandle, depth_view = kVkNullHandle;
        VkRenderPass render_pass = kVkNullHandle;
        VkFramebuffer framebuffer = kVkNullHandle;
        VkBuffer vb = kVkNullHandle, ib = kVkNullHandle, rb_buf = kVkNullHandle;
        VkDeviceMemory vb_mem = kVkNullHandle, ib_mem = kVkNullHandle, rb_mem = kVkNullHandle;
        bool rb_coherent = true;
        VkShaderModule mod0 = kVkNullHandle, mod1 = kVkNullHandle, mod_frag = kVkNullHandle;
        VkDescriptorSetLayout ds_layout = kVkNullHandle;
        VkDescriptorPool desc_pool = kVkNullHandle;
        VkPipelineLayout pipe_layout = kVkNullHandle;
        VkPipeline pipeline = kVkNullHandle;
        VkCommandBuffer cmd = nullptr;

        auto cleanup = [&]() {
            if (cmd) { vkFreeCommandBuffers(device, graphics_command_pool, 1, &cmd); cmd = nullptr; }
            if (pipeline != kVkNullHandle) { vkDestroyPipeline(device, pipeline, nullptr); pipeline = kVkNullHandle; }
            if (pipe_layout != kVkNullHandle) { vkDestroyPipelineLayout(device, pipe_layout, nullptr); pipe_layout = kVkNullHandle; }
            if (desc_pool != kVkNullHandle) { vkDestroyDescriptorPool(device, desc_pool, nullptr); desc_pool = kVkNullHandle; }
            if (ds_layout != kVkNullHandle) { vkDestroyDescriptorSetLayout(device, ds_layout, nullptr); ds_layout = kVkNullHandle; }
            if (mod_frag != kVkNullHandle) { vkDestroyShaderModule(device, mod_frag, nullptr); mod_frag = kVkNullHandle; }
            if (mod1 != kVkNullHandle) { vkDestroyShaderModule(device, mod1, nullptr); mod1 = kVkNullHandle; }
            if (mod0 != kVkNullHandle) { vkDestroyShaderModule(device, mod0, nullptr); mod0 = kVkNullHandle; }
            if (framebuffer != kVkNullHandle) { vkDestroyFramebuffer(device, framebuffer, nullptr); framebuffer = kVkNullHandle; }
            if (render_pass != kVkNullHandle) { vkDestroyRenderPass(device, render_pass, nullptr); render_pass = kVkNullHandle; }
            if (depth_view != kVkNullHandle) { vkDestroyImageView(device, depth_view, nullptr); depth_view = kVkNullHandle; }
            if (color_view != kVkNullHandle) { vkDestroyImageView(device, color_view, nullptr); color_view = kVkNullHandle; }
            if (depth_img != kVkNullHandle) { vkDestroyImage(device, depth_img, nullptr); depth_img = kVkNullHandle; }
            if (resolve_img != kVkNullHandle) { vkDestroyImage(device, resolve_img, nullptr); resolve_img = kVkNullHandle; }
            if (color_img != kVkNullHandle) { vkDestroyImage(device, color_img, nullptr); color_img = kVkNullHandle; }
            if (depth_mem != kVkNullHandle) { vkFreeMemory(device, depth_mem, nullptr); depth_mem = kVkNullHandle; }
            if (resolve_mem != kVkNullHandle) { vkFreeMemory(device, resolve_mem, nullptr); resolve_mem = kVkNullHandle; }
            if (color_mem != kVkNullHandle) { vkFreeMemory(device, color_mem, nullptr); color_mem = kVkNullHandle; }
            if (rb_buf != kVkNullHandle) { vkDestroyBuffer(device, rb_buf, nullptr); rb_buf = kVkNullHandle; }
            if (ib != kVkNullHandle) { vkDestroyBuffer(device, ib, nullptr); ib = kVkNullHandle; }
            if (vb != kVkNullHandle) { vkDestroyBuffer(device, vb, nullptr); vb = kVkNullHandle; }
            if (rb_mem != kVkNullHandle) { vkFreeMemory(device, rb_mem, nullptr); rb_mem = kVkNullHandle; }
            if (ib_mem != kVkNullHandle) { vkFreeMemory(device, ib_mem, nullptr); ib_mem = kVkNullHandle; }
            if (vb_mem != kVkNullHandle) { vkFreeMemory(device, vb_mem, nullptr); vb_mem = kVkNullHandle; }
            for (auto& dg : desc_gpu) {
                if (dg.sampler != kVkNullHandle) vkDestroySampler(device, dg.sampler, nullptr);
                if (dg.img_view != kVkNullHandle) vkDestroyImageView(device, dg.img_view, nullptr);
                if (dg.img != kVkNullHandle) vkDestroyImage(device, dg.img, nullptr);
                if (dg.img_mem != kVkNullHandle) vkFreeMemory(device, dg.img_mem, nullptr);
                if (dg.staging != kVkNullHandle) vkDestroyBuffer(device, dg.staging, nullptr);
                if (dg.staging_mem != kVkNullHandle) vkFreeMemory(device, dg.staging_mem, nullptr);
                if (dg.buf != kVkNullHandle) vkDestroyBuffer(device, dg.buf, nullptr);
                if (dg.buf_mem != kVkNullHandle) vkFreeMemory(device, dg.buf_mem, nullptr);
            }
        };

        // 1. Create render targets (color, optional MSAA resolve target, optional D32_SFLOAT depth)
        Status st = CreateDeviceImage(kVkImageType2D, vk_color_fmt, width, height, 1, 1, samples,
                                      kVkImageUsageColorAttachmentBit | kVkImageUsageTransferSrcBit,
                                      color_img, color_mem);
        if (!st.ok()) { cleanup(); return st; }

        if (enable_msaa_4x) {
            st = CreateDeviceImage(kVkImageType2D, vk_color_fmt, width, height, 1, 1, kVkSampleCount1Bit,
                                   kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit,
                                   resolve_img, resolve_mem);
            if (!st.ok()) { cleanup(); return st; }
        }

        if (enable_depth) {
            st = CreateDeviceImage(kVkImageType2D, kVkFormatD32Sfloat, width, height, 1, 1, samples,
                                   kVkImageUsageDepthStencilAttachmentBit, depth_img, depth_mem);
            if (!st.ok()) { cleanup(); return st; }
        }

        VkImageViewCreateInfo iv_ci{};
        iv_ci.sType = kVkStypeImageViewCreateInfo;
        iv_ci.image = color_img;
        iv_ci.viewType = kVkImageViewType2D;
        iv_ci.format = vk_color_fmt;
        iv_ci.subresourceRange = {kVkImageAspectColorBit, 0, 1, 0, 1};
        if (vkCreateImageView(device, &iv_ci, nullptr, &color_view) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreateImageView(color) failed");
        }

        if (enable_depth) {
            VkImageViewCreateInfo dv_ci{};
            dv_ci.sType = kVkStypeImageViewCreateInfo;
            dv_ci.image = depth_img;
            dv_ci.viewType = kVkImageViewType2D;
            dv_ci.format = kVkFormatD32Sfloat;
            dv_ci.subresourceRange = {kVkImageAspectDepthBit, 0, 1, 0, 1};
            if (vkCreateImageView(device, &dv_ci, nullptr, &depth_view) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkCreateImageView(depth) failed");
            }
        }

        // 2. RenderPass & Framebuffer
        std::vector<VkAttachmentDescription> attachments;
        VkAttachmentDescription color_att{};
        color_att.format = vk_color_fmt;
        color_att.samples = samples;
        color_att.loadOp = kVkAttachmentLoadOpClear;
        color_att.storeOp = kVkAttachmentStoreOpStore;
        color_att.stencilLoadOp = kVkAttachmentLoadOpDontCare;
        color_att.stencilStoreOp = kVkAttachmentStoreOpDontCare;
        color_att.initialLayout = kVkImageLayoutUndefined;
        color_att.finalLayout = kVkImageLayoutTransferSrcOptimal;
        attachments.push_back(color_att);

        VkAttachmentReference color_ref{0u, kVkImageLayoutColorAttachmentOptimal};
        VkAttachmentReference depth_ref{1u, kVkImageLayoutDepthStencilAttachmentOptimal};

        if (enable_depth) {
            VkAttachmentDescription depth_att{};
            depth_att.format = kVkFormatD32Sfloat;
            depth_att.samples = samples;
            depth_att.loadOp = kVkAttachmentLoadOpClear;
            depth_att.storeOp = kVkAttachmentStoreOpDontCare;
            depth_att.stencilLoadOp = kVkAttachmentLoadOpDontCare;
            depth_att.stencilStoreOp = kVkAttachmentStoreOpDontCare;
            depth_att.initialLayout = kVkImageLayoutUndefined;
            depth_att.finalLayout = kVkImageLayoutDepthStencilAttachmentOptimal;
            attachments.push_back(depth_att);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = kVkPipelineBindPointGraphics;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = enable_depth ? &depth_ref : nullptr;

        VkSubpassDependency deps[2]{};
        // Incoming dependency: synchronize UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL before LOAD_OP_CLEAR
        deps[0].srcSubpass = kVkSubpassExternal;
        deps[0].dstSubpass = 0u;
        deps[0].srcStageMask = kVkPipelineStageTopOfPipeBit | kVkPipelineStageColorAttachmentOutputBit;
        deps[0].dstStageMask = kVkPipelineStageColorAttachmentOutputBit;
        deps[0].srcAccessMask = 0u;
        deps[0].dstAccessMask = kVkAccessColorAttachmentWriteBit;
        // Outgoing dependency: synchronize color writes before transfer readback copy
        deps[1].srcSubpass = 0u;
        deps[1].dstSubpass = kVkSubpassExternal;
        deps[1].srcStageMask = kVkPipelineStageColorAttachmentOutputBit;
        deps[1].dstStageMask = kVkPipelineStageTransferBit;
        deps[1].srcAccessMask = kVkAccessColorAttachmentWriteBit;
        deps[1].dstAccessMask = kVkAccessTransferReadBit;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = kVkStypeRenderPassCreateInfo;
        rp_ci.attachmentCount = static_cast<uint32_t>(attachments.size());
        rp_ci.pAttachments = attachments.data();
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = 2;
        rp_ci.pDependencies = deps;
        if (vkCreateRenderPass(device, &rp_ci, nullptr, &render_pass) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreateRenderPass failed");
        }

        std::vector<VkImageView> fb_views = {color_view};
        if (enable_depth) fb_views.push_back(depth_view);
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = kVkStypeFramebufferCreateInfo;
        fb_ci.renderPass = render_pass;
        fb_ci.attachmentCount = static_cast<uint32_t>(fb_views.size());
        fb_ci.pAttachments = fb_views.data();
        fb_ci.width = width;
        fb_ci.height = height;
        fb_ci.layers = 1;
        if (vkCreateFramebuffer(device, &fb_ci, nullptr, &framebuffer) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreateFramebuffer failed");
        }

        // 3. Vertex / Index / Readback buffers
        if (!is_mesh && !vertex_bytes.empty()) {
            bool vb_coh = true;
            st = CreateHostBuffer(vertex_bytes.size(), kVkBufferUsageVertexBufferBit, vb, vb_mem, vb_coh,
                                  vertex_bytes.data(), vertex_bytes.size());
            if (!st.ok()) { cleanup(); return st; }
        }
        if (!is_mesh && !indices.empty()) {
            bool ib_coh = true;
            st = CreateHostBuffer(indices.size() * sizeof(uint32_t), kVkBufferUsageIndexBufferBit, ib, ib_mem, ib_coh,
                                  indices.data(), indices.size() * sizeof(uint32_t));
            if (!st.ok()) { cleanup(); return st; }
        }
        st = CreateHostBuffer(readback_bytes, kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coherent);
        if (!st.ok()) { cleanup(); return st; }

        // 4. Descriptors (UBO, SSBO, 2D/3D textures, samplers)
        const VkFlags active_stages =
            is_mesh ? ((has_task ? kVkShaderStageTaskBitExt : 0u) | kVkShaderStageMeshBitExt | kVkShaderStageFragmentBit)
                    : (kVkShaderStageVertexBit | kVkShaderStageFragmentBit);

        std::vector<VkDescriptorSetLayoutBinding> dsl_bindings;
        std::vector<VkDescriptorPoolSize> pool_sizes;
        for (size_t i = 0; i < descriptors.size(); ++i) {
            const auto& d = descriptors[i];
            auto& dg = desc_gpu[i];

            VkDescriptorSetLayoutBinding b{};
            b.binding = d.binding;
            b.descriptorType = static_cast<int32_t>(d.kind);
            b.descriptorCount = 1;
            b.stageFlags = active_stages;
            dsl_bindings.push_back(b);
            pool_sizes.push_back({b.descriptorType, 1u});

            if (d.kind == VulkanDescriptorKind::UniformBuffer || d.kind == VulkanDescriptorKind::StorageBuffer) {
                bool coh = true;
                const VkFlags usage = (d.kind == VulkanDescriptorKind::UniformBuffer)
                                          ? kVkBufferUsageUniformBufferBit
                                          : kVkBufferUsageStorageBufferBit;
                st = CreateHostBuffer(std::max<size_t>(d.buffer_data.size(), 16u), usage, dg.buf, dg.buf_mem, coh,
                                      d.buffer_data.data(), d.buffer_data.size());
                if (!st.ok()) { cleanup(); return st; }
            } else if (d.kind == VulkanDescriptorKind::CombinedImageSampler ||
                       d.kind == VulkanDescriptorKind::SampledImage) {
                dg.tex_copy = d.texture;
                if (dg.tex_copy.mips.empty()) {
                    VulkanTextureMipLevel m0{};
                    m0.width = std::max<uint32_t>(1, dg.tex_copy.width);
                    m0.height = std::max<uint32_t>(1, dg.tex_copy.height);
                    m0.depth = dg.tex_copy.is_3d ? std::max<uint32_t>(1, dg.tex_copy.depth) : 1u;
                    m0.data.assign(static_cast<size_t>(m0.width) * m0.height * m0.depth * BytesPerTexel(dg.tex_copy.format), 0u);
                    dg.tex_copy.mips.push_back(std::move(m0));
                }
                const uint32_t mip_cnt = static_cast<uint32_t>(dg.tex_copy.mips.size());
                st = CreateDeviceImage(dg.tex_copy.is_3d ? kVkImageType3D : kVkImageType2D,
                                       static_cast<int32_t>(dg.tex_copy.format),
                                       dg.tex_copy.width, dg.tex_copy.height,
                                       dg.tex_copy.is_3d ? dg.tex_copy.depth : 1u,
                                       mip_cnt, kVkSampleCount1Bit,
                                       kVkImageUsageTransferDstBit | kVkImageUsageSampledBit,
                                       dg.img, dg.img_mem);
                if (!st.ok()) { cleanup(); return st; }

                const uint32_t base_mip = std::min(d.view_base_mip_level, mip_cnt > 0u ? mip_cnt - 1u : 0u);
                const uint32_t max_rem = (mip_cnt > base_mip) ? (mip_cnt - base_mip) : 1u;
                const uint32_t level_cnt =
                    (d.view_mip_level_count > 0u) ? std::min(d.view_mip_level_count, max_rem) : max_rem;

                VkImageViewCreateInfo tiv{};
                tiv.sType = kVkStypeImageViewCreateInfo;
                tiv.image = dg.img;
                tiv.viewType = dg.tex_copy.is_3d ? kVkImageViewType3D : kVkImageViewType2D;
                tiv.format = static_cast<int32_t>(dg.tex_copy.format);
                tiv.subresourceRange = {kVkImageAspectColorBit, base_mip, level_cnt, 0, 1};
                if (vkCreateImageView(device, &tiv, nullptr, &dg.img_view) != kVkSuccess) {
                    cleanup();
                    return Status::Error(StatusCode::DeviceError, "vkCreateImageView(texture) failed");
                }

                // Pack all mip levels into staging upload buffer
                std::vector<uint8_t> packed;
                for (const auto& m : dg.tex_copy.mips) {
                    while (packed.size() % 16 != 0) packed.push_back(0u);
                    packed.insert(packed.end(), m.data.begin(), m.data.end());
                }
                bool st_coh = true;
                st = CreateHostBuffer(packed.size(), kVkBufferUsageTransferSrcBit, dg.staging, dg.staging_mem, st_coh,
                                      packed.data(), packed.size());
                if (!st.ok()) { cleanup(); return st; }
                dg.has_upload = true;
            }

            if (d.kind == VulkanDescriptorKind::CombinedImageSampler || d.kind == VulkanDescriptorKind::Sampler) {
                bool used_custom = false;
                bool used_fb = false;
                VulkanBorderColorMode eff_mode = d.border_color_mode;
                float eff_f[4]{};
                int32_t eff_i[4]{};
                const VulkanImageFormat border_fmt =
                    (d.kind == VulkanDescriptorKind::CombinedImageSampler)
                        ? dg.tex_copy.format
                        : VulkanImageFormat::Rgba8Unorm;
                st = CreateConfiguredSampler(
                    d.linear_filter,
                    d.address_mode_u,
                    d.address_mode_v,
                    d.address_mode_w,
                    d.sampler_mip_lod_bias,
                    d.sampler_min_lod,
                    d.sampler_max_lod,
                    d.border_color_mode,
                    d.custom_border_color_float,
                    d.custom_border_color_int,
                    border_fmt,
                    d.custom_border_specify_format,
                    d.force_standard_border_fallback,
                    dg.sampler,
                    used_custom,
                    used_fb,
                    eff_mode,
                    eff_f,
                    eff_i);
                if (!st.ok()) {
                    cleanup();
                    return st;
                }
                if (used_custom && out_custom_border_used) ++(*out_custom_border_used);
                if (used_fb && out_custom_border_fallback) ++(*out_custom_border_fallback);
            }
        }

        VkDescriptorSet desc_set = kVkNullHandle;
        if (!dsl_bindings.empty()) {
            VkDescriptorSetLayoutCreateInfo dsl_ci{};
            dsl_ci.sType = kVkStypeDescriptorSetLayoutCreateInfo;
            dsl_ci.bindingCount = static_cast<uint32_t>(dsl_bindings.size());
            dsl_ci.pBindings = dsl_bindings.data();
            if (vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &ds_layout) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorSetLayout failed");
            }
            VkDescriptorPoolCreateInfo dp_ci{};
            dp_ci.sType = kVkStypeDescriptorPoolCreateInfo;
            dp_ci.maxSets = 1;
            dp_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
            dp_ci.pPoolSizes = pool_sizes.data();
            if (vkCreateDescriptorPool(device, &dp_ci, nullptr, &desc_pool) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorPool failed");
            }
            VkDescriptorSetAllocateInfo dsa{};
            dsa.sType = kVkStypeDescriptorSetAllocateInfo;
            dsa.descriptorPool = desc_pool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts = &ds_layout;
            if (vkAllocateDescriptorSets(device, &dsa, &desc_set) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkAllocateDescriptorSets failed");
            }

            std::vector<VkDescriptorBufferInfo> buf_infos(descriptors.size());
            std::vector<VkDescriptorImageInfo> img_infos(descriptors.size());
            std::vector<VkWriteDescriptorSet> writes;
            for (size_t i = 0; i < descriptors.size(); ++i) {
                const auto& d = descriptors[i];
                const auto& dg = desc_gpu[i];
                VkWriteDescriptorSet w{};
                w.sType = kVkStypeWriteDescriptorSet;
                w.dstSet = desc_set;
                w.dstBinding = d.binding;
                w.descriptorCount = 1;
                w.descriptorType = static_cast<int32_t>(d.kind);
                if (d.kind == VulkanDescriptorKind::UniformBuffer || d.kind == VulkanDescriptorKind::StorageBuffer) {
                    buf_infos[i] = {dg.buf, 0u, std::max<VkDeviceSize>(d.buffer_data.size(), 16ull)};
                    w.pBufferInfo = &buf_infos[i];
                } else {
                    img_infos[i] = {dg.sampler, dg.img_view, kVkImageLayoutShaderReadOnlyOptimal};
                    w.pImageInfo = &img_infos[i];
                }
                writes.push_back(w);
            }
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        // 5. Shader Modules, Pipeline Layout & Graphics Pipeline
        auto make_shader = [&](const std::vector<uint32_t>& code, VkShaderModule& out_mod) -> Status {
            VkShaderModuleCreateInfo sm_ci{};
            sm_ci.sType = kVkStypeShaderModuleCreateInfo;
            sm_ci.codeSize = code.size() * sizeof(uint32_t);
            sm_ci.pCode = code.data();
            if (vkCreateShaderModule(device, &sm_ci, nullptr, &out_mod) != kVkSuccess) {
                return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule failed");
            }
            return Status::Ok();
        };

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        if (has_task) {
            st = make_shader(stage0_spirv, mod0);
            if (!st.ok()) { cleanup(); return st; }
            VkPipelineShaderStageCreateInfo s{};
            s.sType = kVkStypePipelineShaderStageCreateInfo;
            s.stage = kVkShaderStageTaskBitExt;
            s.module = mod0;
            s.pName = stage0_entry.c_str();
            stages.push_back(s);
        }
        st = make_shader(stage1_spirv, mod1);
        if (!st.ok()) { cleanup(); return st; }
        {
            VkPipelineShaderStageCreateInfo s{};
            s.sType = kVkStypePipelineShaderStageCreateInfo;
            s.stage = is_mesh ? kVkShaderStageMeshBitExt : kVkShaderStageVertexBit;
            s.module = mod1;
            s.pName = stage1_entry.c_str();
            stages.push_back(s);
        }
        st = make_shader(frag_spirv, mod_frag);
        if (!st.ok()) { cleanup(); return st; }
        {
            VkPipelineShaderStageCreateInfo s{};
            s.sType = kVkStypePipelineShaderStageCreateInfo;
            s.stage = kVkShaderStageFragmentBit;
            s.module = mod_frag;
            s.pName = frag_entry.c_str();
            stages.push_back(s);
        }

        VkPushConstantRange pc_range{};
        pc_range.stageFlags = active_stages;
        pc_range.offset = 0;
        pc_range.size = effective_pc_size;

        VkPipelineLayoutCreateInfo pl_ci{};
        pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
        pl_ci.setLayoutCount = (ds_layout != kVkNullHandle) ? 1u : 0u;
        pl_ci.pSetLayouts = (ds_layout != kVkNullHandle) ? &ds_layout : nullptr;
        pl_ci.pushConstantRangeCount = (effective_pc_size > 0u) ? 1u : 0u;
        pl_ci.pPushConstantRanges = (effective_pc_size > 0u) ? &pc_range : nullptr;
        if (vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout failed");
        }

        VkVertexInputBindingDescription vb_desc{};
        std::vector<VkVertexInputAttributeDescription> vk_attrs;
        VkPipelineVertexInputStateCreateInfo vi_ci{};
        vi_ci.sType = kVkStypePipelineVertexInputStateCreateInfo;
        if (!is_mesh && !vertex_attributes.empty() && vertex_stride > 0) {
            vb_desc = {0u, vertex_stride, kVkVertexInputRateVertex};
            for (const auto& a : vertex_attributes) {
                vk_attrs.push_back({a.location, a.binding, static_cast<int32_t>(a.format), a.offset});
            }
            vi_ci.vertexBindingDescriptionCount = 1;
            vi_ci.pVertexBindingDescriptions = &vb_desc;
            vi_ci.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attrs.size());
            vi_ci.pVertexAttributeDescriptions = vk_attrs.data();
        }

        VkPipelineInputAssemblyStateCreateInfo ia_ci{};
        ia_ci.sType = kVkStypePipelineInputAssemblyStateCreateInfo;
        ia_ci.topology = kVkPrimitiveTopologyTriangleList;

        VkPipelineViewportStateCreateInfo vp_ci{};
        vp_ci.sType = kVkStypePipelineViewportStateCreateInfo;
        vp_ci.viewportCount = 1;
        vp_ci.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs_ci{};
        rs_ci.sType = kVkStypePipelineRasterizationStateCreateInfo;
        rs_ci.polygonMode = kVkPolygonModeFill;
        rs_ci.cullMode = kVkCullModeNone;
        rs_ci.frontFace = kVkFrontFaceCounterClockwise;
        rs_ci.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms_ci{};
        ms_ci.sType = kVkStypePipelineMultisampleStateCreateInfo;
        ms_ci.rasterizationSamples = samples;

        VkPipelineDepthStencilStateCreateInfo ds_ci{};
        ds_ci.sType = kVkStypePipelineDepthStencilStateCreateInfo;
        ds_ci.depthTestEnable = enable_depth ? 1u : 0u;
        ds_ci.depthWriteEnable = (enable_depth && depth_write) ? 1u : 0u;
        ds_ci.depthCompareOp = kVkCompareOpLess;

        VkPipelineColorBlendAttachmentState cb_att{};
        cb_att.colorWriteMask = 0xFu;
        VkPipelineColorBlendStateCreateInfo cb_ci{};
        cb_ci.sType = kVkStypePipelineColorBlendStateCreateInfo;
        cb_ci.attachmentCount = 1;
        cb_ci.pAttachments = &cb_att;

        const int32_t dyn_states[2] = {kVkDynamicStateViewport, kVkDynamicStateScissor};
        VkPipelineDynamicStateCreateInfo dyn_ci{};
        dyn_ci.sType = kVkStypePipelineDynamicStateCreateInfo;
        dyn_ci.dynamicStateCount = 2;
        dyn_ci.pDynamicStates = dyn_states;

        VkGraphicsPipelineCreateInfo gp_ci{};
        gp_ci.sType = kVkStypeGraphicsPipelineCreateInfo;
        gp_ci.stageCount = static_cast<uint32_t>(stages.size());
        gp_ci.pStages = stages.data();
        gp_ci.pVertexInputState = is_mesh ? nullptr : &vi_ci;
        gp_ci.pInputAssemblyState = is_mesh ? nullptr : &ia_ci;
        gp_ci.pViewportState = &vp_ci;
        gp_ci.pRasterizationState = &rs_ci;
        gp_ci.pMultisampleState = &ms_ci;
        gp_ci.pDepthStencilState = enable_depth ? &ds_ci : nullptr;
        gp_ci.pColorBlendState = &cb_ci;
        gp_ci.pDynamicState = &dyn_ci;
        gp_ci.layout = pipe_layout;
        gp_ci.renderPass = render_pass;
        gp_ci.subpass = 0;
        gp_ci.basePipelineIndex = -1;

        VkResult vr = vkCreateGraphicsPipelines(device, kVkNullHandle, 1, &gp_ci, nullptr, &pipeline);
        if (vr != kVkSuccess || pipeline == kVkNullHandle) {
            cleanup();
            return Status::Error(StatusCode::ShaderError,
                                 "vkCreateGraphicsPipelines failed (VkResult=" + std::to_string(vr) + ")");
        }

        // 6. Record command buffer
        VkCommandBufferAllocateInfo cba{};
        cba.sType = kVkStypeCommandBufferAllocateInfo;
        cba.commandPool = graphics_command_pool;
        cba.level = kVkCommandBufferLevelPrimary;
        cba.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cba, &cmd) != kVkSuccess || !cmd) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(gfx) failed");
        }
        VkCommandBufferBeginInfo cbi{};
        cbi.sType = kVkStypeCommandBufferBeginInfo;
        cbi.flags = kVkCommandBufferUsageOneTimeSubmitBit;
        if (vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(gfx) failed");
        }

        // Upload descriptor textures before render pass
        for (const auto& dg : desc_gpu) {
            if (!dg.has_upload) continue;
            const uint32_t mip_cnt = static_cast<uint32_t>(dg.tex_copy.mips.size());
            CmdImageBarrier(cmd, dg.img, kVkImageAspectColorBit,
                            kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                            kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                            0u, kVkAccessTransferWriteBit, mip_cnt);
            VkDeviceSize offset = 0;
            for (uint32_t m = 0; m < mip_cnt; ++m) {
                while (offset % 16 != 0) ++offset;
                const auto& mip = dg.tex_copy.mips[m];
                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
                region.imageExtent = {std::max<uint32_t>(1, mip.width), std::max<uint32_t>(1, mip.height),
                                      dg.tex_copy.is_3d ? std::max<uint32_t>(1, mip.depth) : 1u};
                vkCmdCopyBufferToImage(cmd, dg.staging, dg.img, kVkImageLayoutTransferDstOptimal, 1, &region);
                offset += mip.data.size();
            }
            CmdImageBarrier(cmd, dg.img, kVkImageAspectColorBit,
                            kVkImageLayoutTransferDstOptimal, kVkImageLayoutShaderReadOnlyOptimal,
                            kVkPipelineStageTransferBit, kVkPipelineStageFragmentShaderBit | kVkPipelineStageVertexShaderBit,
                            kVkAccessTransferWriteBit, kVkAccessShaderReadBit, mip_cnt);
        }

        VkClearValue clear_vals[2]{};
        clear_vals[0].color.float32[0] = clear_color[0];
        clear_vals[0].color.float32[1] = clear_color[1];
        clear_vals[0].color.float32[2] = clear_color[2];
        clear_vals[0].color.float32[3] = clear_color[3];
        clear_vals[1].depthStencil = {clear_depth, 0u};

        VkRenderPassBeginInfo rpb{};
        rpb.sType = kVkStypeRenderPassBeginInfo;
        rpb.renderPass = render_pass;
        rpb.framebuffer = framebuffer;
        rpb.renderArea = {{0, 0}, {width, height}};
        rpb.clearValueCount = enable_depth ? 2u : 1u;
        rpb.pClearValues = clear_vals;
        vkCmdBeginRenderPass(cmd, &rpb, kVkSubpassContentsInline);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, kVkPipelineBindPointGraphics, pipeline);

        if (desc_set != kVkNullHandle) {
            vkCmdBindDescriptorSets(cmd, kVkPipelineBindPointGraphics, pipe_layout, 0, 1, &desc_set, 0, nullptr);
        }
        if (effective_pc_size > 0u && push_constants.size() >= effective_pc_size) {
            vkCmdPushConstants(cmd, pipe_layout, active_stages, 0,
                               effective_pc_size, push_constants.data());
        }

        if (is_mesh) {
            vkCmdDrawMeshTasksEXT(cmd, mesh_groups[0], mesh_groups[1], mesh_groups[2]);
        } else {
            if (vb != kVkNullHandle) {
                const VkDeviceSize vb_off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_off);
            }
            if (ib != kVkNullHandle) {
                vkCmdBindIndexBuffer(cmd, ib, 0, kVkIndexTypeUint32);
                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
            } else {
                vkCmdDraw(cmd, vertex_count, 1, 0, 0);
            }
        }
        vkCmdEndRenderPass(cmd);

        // Optional MSAA 4x -> 1x resolve using vkCmdResolveImage, then readback copy
        VkImage copy_src = color_img;
        if (enable_msaa_4x) {
            CmdImageBarrier(cmd, resolve_img, kVkImageAspectColorBit,
                            kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                            kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                            0u, kVkAccessTransferWriteBit);
            VkImageResolve res_reg{};
            res_reg.srcSubresource = {kVkImageAspectColorBit, 0, 0, 1};
            res_reg.dstSubresource = {kVkImageAspectColorBit, 0, 0, 1};
            res_reg.extent = {width, height, 1};
            vkCmdResolveImage(cmd, color_img, kVkImageLayoutTransferSrcOptimal,
                              resolve_img, kVkImageLayoutTransferDstOptimal, 1, &res_reg);
            CmdImageBarrier(cmd, resolve_img, kVkImageAspectColorBit,
                            kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                            kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                            kVkAccessTransferWriteBit, kVkAccessTransferReadBit);
            copy_src = resolve_img;
        }

        VkBufferImageCopy rb_copy{};
        rb_copy.imageSubresource = {kVkImageAspectColorBit, 0, 0, 1};
        rb_copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, copy_src, kVkImageLayoutTransferSrcOptimal, rb_buf, 1, &rb_copy);

        VkBufferMemoryBarrier rb_bar{};
        rb_bar.sType = kVkStypeBufferMemoryBarrier;
        rb_bar.srcAccessMask = kVkAccessTransferWriteBit;
        rb_bar.dstAccessMask = kVkAccessHostReadBit;
        rb_bar.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
        rb_bar.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
        rb_bar.buffer = rb_buf;
        rb_bar.offset = 0;
        rb_bar.size = readback_bytes;
        vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                             0, 0, nullptr, 1, &rb_bar, 0, nullptr);

        if (vkEndCommandBuffer(cmd) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(gfx) failed");
        }

        VkSubmitInfo si{};
        si.sType = kVkStypeSubmitInfo;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
            vkQueueWaitIdle(graphics_queue) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "Vulkan graphics queue submit/wait failed");
        }

        // 7. Read back framebuffer (and raw FP16 if requested)
        void* mapped = nullptr;
        if (vkMapMemory(device, rb_mem, 0, readback_bytes, 0, &mapped) != kVkSuccess || !mapped) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkMapMemory(readback) failed");
        }
        if (!rb_coherent) {
            VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0, kVkWholeSize};
            vkInvalidateMappedMemoryRanges(device, 1, &rng);
        }

        out_fb.width = width;
        out_fb.height = height;
        out_fb.pixels.resize(static_cast<size_t>(width) * height * 4u);
        out_fp16.clear();

        if (color_format == VulkanImageFormat::Rgba16Sfloat) {
            const size_t total_ch = static_cast<size_t>(width) * height * 4u;
            out_fp16.resize(total_ch);
            std::memcpy(out_fp16.data(), mapped, total_ch * sizeof(uint16_t));
            for (size_t i = 0; i < total_ch; ++i) {
                out_fb.pixels[i] = FloatToUnorm8(HalfToFloat(out_fp16[i]));
            }
        } else if (color_format == VulkanImageFormat::Bgra8Unorm) {
            const auto* src = static_cast<const uint8_t*>(mapped);
            const size_t px_count = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < px_count; ++i) {
                out_fb.pixels[i * 4 + 0] = src[i * 4 + 2];
                out_fb.pixels[i * 4 + 1] = src[i * 4 + 1];
                out_fb.pixels[i * 4 + 2] = src[i * 4 + 0];
                out_fb.pixels[i * 4 + 3] = src[i * 4 + 3];
            }
        } else {
            std::memcpy(out_fb.pixels.data(), mapped, static_cast<size_t>(readback_bytes));
        }
        vkUnmapMemory(device, rb_mem);
        cleanup();
        return Status::Ok();
    }

    void Cleanup() {
        for (auto& [h, rec] : reserved_images) {
            DestroyReservedImageRecord(rec);
        }
        reserved_images.clear();

        for (auto& [h, rec] : swapchains) {
            DestroySwapchainRecord(rec);
        }
        swapchains.clear();

        if (device && vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device);
        }
        if (device && graphics_command_pool != kVkNullHandle && graphics_command_pool != command_pool && vkDestroyCommandPool) {
            vkDestroyCommandPool(device, graphics_command_pool, nullptr);
        }
        graphics_command_pool = kVkNullHandle;

        if (device && command_pool != kVkNullHandle && vkDestroyCommandPool) {
            vkDestroyCommandPool(device, command_pool, nullptr);
            command_pool = kVkNullHandle;
        }
        if (device && vkDestroyDevice) {
            vkDestroyDevice(device, nullptr);
            device = nullptr;
        }
        compute_queue = nullptr;
        graphics_queue = nullptr;
        sparse_queue = nullptr;
        physical_device = nullptr;
        if (instance && vkDestroyInstance) {
            vkDestroyInstance(instance, nullptr);
            instance = nullptr;
        }
#if defined(_WIN32)
        if (vulkan_dll) {
            FreeLibrary(vulkan_dll);
            vulkan_dll = nullptr;
        }
#endif
        vkGetInstanceProcAddr = nullptr;
        vkGetDeviceProcAddr = nullptr;
        has_graphics_queue = false;
        has_sparse_queue = false;
        sparse_binding_enabled = false;
        sparse_residency_2d_enabled = false;
        sparse_residency_3d_enabled = false;
        mesh_shader_enabled = false;
        task_shader_enabled = false;
        win32_surface_enabled = false;
        swapchain_enabled = false;
        hdr10_colorspace_enabled = false;
        bc_compression_enabled = false;
        depth_bounds_enabled = false;
        custom_border_color_enabled = false;
        custom_border_color_without_format_enabled = false;
        max_custom_border_color_samplers = 0;
        max_push_constants_size = 128;
        subgroup_size = 32;
        initialized = false;
        fallback_mode = false;
    }

    ~Impl() { Cleanup(); }
};

VulkanBackend::VulkanBackend() : impl_(std::make_unique<Impl>()) {}
VulkanBackend::~VulkanBackend() = default;

bool VulkanBackend::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

bool VulkanBackend::is_fallback_mode() const noexcept {
    return impl_ && impl_->fallback_mode;
}

Status VulkanBackend::ValidateSpirvBinary(const std::vector<uint32_t>& spirv,
                                          SpirvValidationInfo* out_info) {
    if (spirv.size() < 5) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary too small (< 5-word header)");
    }
    if (spirv[0] != kSpirvMagicNumber) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V magic number (expected 0x07230203)");
    }

    const uint32_t version = spirv[1];
    if ((version & 0xFF0000FFu) != 0) {
        return Status::Error(StatusCode::ShaderError, "Malformed SPIR-V version word");
    }
    const uint32_t major = (version >> 16u) & 0xFFu;
    const uint32_t minor = (version >> 8u) & 0xFFu;
    if (major != 1 || minor > 6) {
        return Status::Error(StatusCode::ShaderError, "Unsupported SPIR-V version");
    }

    const uint32_t bound = spirv[3];
    if (bound == 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V ID bound must be > 0");
    }
    if (spirv[4] != 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V reserved schema word must be 0");
    }

    SpirvValidationInfo info{};
    info.version_major = major;
    info.version_minor = minor;
    info.generator = spirv[2];
    info.bound = bound;

    bool saw_memory_model = false;
    bool in_function = false;
    uint32_t compute_entry_id = 0;

    size_t pos = 5;
    while (pos < spirv.size()) {
        const uint32_t word = spirv[pos];
        const uint16_t wc = static_cast<uint16_t>(word >> 16u);
        const uint16_t op = static_cast<uint16_t>(word & 0xFFFFu);

        if (wc == 0) {
            return Status::Error(StatusCode::ShaderError, "SPIR-V instruction has zero word count");
        }
        if (pos + wc > spirv.size()) {
            return Status::Error(StatusCode::ShaderError, "Truncated SPIR-V instruction exceeds binary length");
        }

        ++info.instruction_count;

        switch (op) {
            case kSpvOpMemoryModel:
                if (wc != 3) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpMemoryModel word count");
                }
                saw_memory_model = true;
                break;

            case kSpvOpEntryPoint: {
                if (wc < 4) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpEntryPoint word count");
                }
                const uint32_t exec_model = spirv[pos + 1];
                const uint32_t entry_id = spirv[pos + 2];
                if (entry_id == 0 || entry_id >= bound) {
                    return Status::Error(StatusCode::ShaderError, "OpEntryPoint ID out of bound");
                }
                std::string name;
                size_t next_word = 0;
                if (!ReadSpirvString(spirv, pos + 3, pos + wc, name, next_word) || name.empty()) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEntryPoint name string");
                }
                ++info.entry_point_count;
                if (exec_model == kSpvExecModelGLCompute) {
                    info.has_compute_entry = true;
                    info.entry_point_name = name;
                    compute_entry_id = entry_id;
                } else {
                    if (exec_model == kSpvExecModelVertex) info.has_vertex_entry = true;
                    else if (exec_model == kSpvExecModelFragment) info.has_fragment_entry = true;
                    else if (exec_model == kSpvExecModelTaskEXT) info.has_task_entry = true;
                    else if (exec_model == kSpvExecModelMeshEXT) info.has_mesh_entry = true;
                    if (info.entry_point_name.empty()) {
                        info.entry_point_name = name;
                    }
                }
                break;
            }

            case kSpvOpExecutionMode: {
                if (wc < 3) {
                    return Status::Error(StatusCode::ShaderError, "Invalid OpExecutionMode word count");
                }
                const uint32_t target = spirv[pos + 1];
                const uint32_t mode = spirv[pos + 2];
                if (mode == kSpvExecModeLocalSize && wc >= 6 &&
                    (compute_entry_id == 0 || target == compute_entry_id)) {
                    info.local_size_x = spirv[pos + 3];
                    info.local_size_y = spirv[pos + 4];
                    info.local_size_z = spirv[pos + 5];
                    if (info.local_size_x == 0 || info.local_size_y == 0 || info.local_size_z == 0) {
                        return Status::Error(StatusCode::ShaderError, "LocalSize dimensions must be > 0");
                    }
                }
                break;
            }

            case kSpvOpFunction:
                if (in_function || wc != 5) {
                    return Status::Error(StatusCode::ShaderError, "Malformed or nested OpFunction");
                }
                if (spirv[pos + 2] == 0 || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "OpFunction result ID out of bound");
                }
                in_function = true;
                break;

            case kSpvOpFunctionEnd:
                if (!in_function || wc != 1) {
                    return Status::Error(StatusCode::ShaderError, "Unmatched OpFunctionEnd");
                }
                in_function = false;
                break;

            case kSpvOpTypeVoid:
            case kSpvOpTypeBool:
            case kSpvOpTypeInt:
            case kSpvOpTypeFloat:
            case kSpvOpTypeVector:
            case kSpvOpTypeMatrix:
            case kSpvOpTypeRuntimeArray:
            case kSpvOpTypeStruct:
            case kSpvOpTypePointer:
            case kSpvOpTypeFunction:
                if (wc < 2 || spirv[pos + 1] == 0 || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Type instruction result ID out of bound");
                }
                break;

            case kSpvOpConstant:
            case kSpvOpVariable:
            case kSpvOpLoad:
            case kSpvOpAccessChain:
                if (wc < 3 || spirv[pos + 2] == 0 || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Instruction result ID out of bound");
                }
                break;

            default:
                break;
        }

        pos += wc;
    }

    if (info.instruction_count == 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary contains no instructions");
    }
    if (!saw_memory_model) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary missing mandatory OpMemoryModel");
    }
    if (in_function) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary has unclosed OpFunction");
    }

    if (out_info) {
        *out_info = std::move(info);
    }
    return Status::Ok();
}

Status VulkanBackend::InitializeSoftwareFallback() {
    Shutdown();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->initialized = true;
    impl_->fallback_mode = true;
    device_info_ = VulkanDeviceInfo{};
    device_info_.device_name = "OpenScarlett Software SPIR-V Fallback";
    device_info_.vendor_id = 0x5358;
    device_info_.device_id = 0x0001;
    device_info_.api_version = kVkApiVersion1_3;
    device_info_.api_major = 1;
    device_info_.api_minor = 3;
    device_info_.api_patch = 0;
    device_info_.device_type = VulkanDeviceType::Cpu;
    device_info_.compute_queue_family = 0;
    device_info_.graphics_queue_family = 0;
    device_info_.supports_compute = true;
    device_info_.supports_graphics = true;
    device_info_.supports_mesh_shader = false;
    device_info_.supports_task_shader = false;
    device_info_.supports_win32_surface = false;
    device_info_.supports_swapchain = false;
    device_info_.supports_hdr10_colorspace = false;
    device_info_.sparse_queue_family = 0;
    device_info_.supports_sparse_binding = true;
    device_info_.supports_sparse_residency_image_2d = true;
    device_info_.supports_sparse_residency_image_3d = true;
    device_info_.max_push_constants_size = 128u;
    device_info_.subgroup_size = 32u;
    device_info_.supported_subgroup_stages = kVkShaderStageComputeBit;
    device_info_.supported_subgroup_operations = 0x1u;
    device_info_.supports_bc_texture_compression = true;
    device_info_.supports_depth_bounds_test = false;
    device_info_.supports_sample_locations_ext = false;
    device_info_.supports_custom_border_color = false;
    device_info_.supports_custom_border_color_without_format = false;
    device_info_.max_custom_border_color_samplers = 0u;
    device_info_.is_hardware = false;
    device_info_.is_fallback = true;
    impl_->max_push_constants_size = 128u;
    impl_->subgroup_size = 32u;
    impl_->bc_compression_enabled = true;
    impl_->depth_bounds_enabled = false;
    impl_->custom_border_color_enabled = false;
    impl_->custom_border_color_without_format_enabled = false;
    impl_->max_custom_border_color_samplers = 0u;

    feature_probe_ = VulkanFeatureProbe{};
    feature_probe_.api_version = kVkApiVersion1_3;
    feature_probe_.api_major = 1u;
    feature_probe_.api_minor = 3u;
    feature_probe_.api_patch = 0u;
    feature_probe_.probed_from_icd = false;
    feature_probe_.texture_compression_bc = true;
    feature_probe_.sparse_binding = true;
    feature_probe_.sparse_residency_image_2d = true;
    feature_probe_.sparse_residency_image_3d = true;
    d3d12_options2_ = MapVulkanToD3D12Options2(feature_probe_);
    d3d12_options7_ = MapVulkanToD3D12Options7(feature_probe_);
    return Status::Ok();
}

uint64_t VulkanBackend::ScorePhysicalDeviceCandidate(
    const VulkanPhysicalDeviceCandidate& candidate) noexcept {
    if (!candidate.supports_compute) {
        return 0ull;
    }
    const uint64_t type_tier = static_cast<uint64_t>(
        DeviceTypeRank(static_cast<uint32_t>(candidate.device_type)));
    const uint64_t gfx_bonus = candidate.supports_graphics ? (1ull << 48) : 0ull;
    const uint64_t mesh_bonus = candidate.supports_mesh_shader ? (1ull << 44) : 0ull;
    const uint64_t vram_mib = std::min<uint64_t>(
        candidate.device_local_vram_bytes / (1024ull * 1024ull), 0xFFFFFull);
    const uint64_t api_major = VkApiMajor(candidate.api_version);
    const uint64_t api_minor = VkApiMinor(candidate.api_version);
    const uint64_t api_patch = std::min<uint64_t>(VkApiPatch(candidate.api_version), 15ull);
    const uint64_t api_score = std::min<uint64_t>(
        (api_major << 12) | (api_minor << 4) | api_patch, 0xFFFFFull);
    return (type_tier << 52) | gfx_bonus | mesh_bonus | (vram_mib << 20) | api_score;
}

std::vector<VulkanPhysicalDeviceCandidate> VulkanBackend::RankPhysicalDeviceCandidates(
    std::vector<VulkanPhysicalDeviceCandidate> candidates) {
    for (auto& c : candidates) {
        c.rank_score = ScorePhysicalDeviceCandidate(c);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const VulkanPhysicalDeviceCandidate& a,
                        const VulkanPhysicalDeviceCandidate& b) {
                         if (a.rank_score != b.rank_score) {
                             return a.rank_score > b.rank_score;
                         }
                         return a.enumerated_index < b.enumerated_index;
                     });
    return candidates;
}

bool VulkanBackend::SelectPhysicalDeviceCandidate(
    const std::vector<VulkanPhysicalDeviceCandidate>& candidates,
    const std::string& gpu_selector,
    VulkanPhysicalDeviceCandidate& out_selected,
    bool* out_matched_selector) {
    if (out_matched_selector) *out_matched_selector = false;
    const std::vector<VulkanPhysicalDeviceCandidate> ranked =
        RankPhysicalDeviceCandidates(candidates);

    std::string trimmed = gpu_selector;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    if (trimmed.empty()) {
        for (const auto& c : ranked) {
            if (c.supports_compute) {
                out_selected = c;
                return true;
            }
        }
        return false;
    }

    bool is_non_neg_int = true;
    for (char ch : trimmed) {
        if (ch < '0' || ch > '9') {
            is_non_neg_int = false;
            break;
        }
    }
    if (is_non_neg_int) {
        uint64_t parsed_idx = 0ull;
        for (char ch : trimmed) {
            parsed_idx = parsed_idx * 10ull + static_cast<uint64_t>(ch - '0');
            if (parsed_idx > 0xFFFFFFFFull) {
                return false;
            }
        }
        const uint32_t target_idx = static_cast<uint32_t>(parsed_idx);
        for (const auto& c : ranked) {
            if (c.supports_compute && c.enumerated_index == target_idx) {
                out_selected = c;
                if (out_matched_selector) *out_matched_selector = true;
                return true;
            }
        }
        return false;
    }

    for (const auto& c : ranked) {
        if (c.supports_compute && ContainsCaseInsensitive(c.device_name, trimmed)) {
            out_selected = c;
            if (out_matched_selector) *out_matched_selector = true;
            return true;
        }
    }
    return false;
}

bool VulkanBackend::SelectPhysicalDeviceCandidateWithNumericFallback(
    const std::vector<VulkanPhysicalDeviceCandidate>& candidates,
    const std::string& gpu_selector,
    VulkanPhysicalDeviceCandidate& out_selected,
    bool* out_matched_selector,
    bool* out_numeric_fell_back) {
    if (out_matched_selector) *out_matched_selector = false;
    if (out_numeric_fell_back) *out_numeric_fell_back = false;

    if (SelectPhysicalDeviceCandidate(candidates, gpu_selector, out_selected, out_matched_selector)) {
        return true;
    }

    // Trim whitespace and check if selector is a signed/unsigned integer index (xcl #82).
    std::string trimmed = gpu_selector;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) return false;

    size_t start_pos = (trimmed[0] == '-' || trimmed[0] == '+') ? 1u : 0u;
    bool is_numeric = (start_pos < trimmed.size());
    for (size_t i = start_pos; i < trimmed.size(); ++i) {
        if (trimmed[i] < '0' || trimmed[i] > '9') {
            is_numeric = false;
            break;
        }
    }
    if (!is_numeric) {
        return false;
    }

    // Numeric selector was out of range (negative or >= device count): fall back cleanly to best-ranked GPU.
    if (SelectPhysicalDeviceCandidate(candidates, "", out_selected, nullptr)) {
        if (out_matched_selector) *out_matched_selector = false;
        if (out_numeric_fell_back) *out_numeric_fell_back = true;
        return true;
    }
    return false;
}

Status VulkanBackend::EnumerateRankedPhysicalDevices(
    std::vector<VulkanPhysicalDeviceCandidate>& out_ranked_devices) {
    out_ranked_devices.clear();
#if defined(_WIN32)
    HMODULE dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) {
        return Status::Error(StatusCode::DeviceError, "vulkan-1.dll not found");
    }
    auto cleanup_dll = [&]() {
        if (dll) {
            FreeLibrary(dll);
            dll = nullptr;
        }
    };
    auto get_ipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(dll, "vkGetInstanceProcAddr"));
    if (!get_ipa) {
        cleanup_dll();
        return Status::Error(StatusCode::DeviceError, "vkGetInstanceProcAddr missing");
    }
    auto create_inst = reinterpret_cast<PFN_vkCreateInstance>(get_ipa(nullptr, "vkCreateInstance"));
    if (!create_inst) {
        cleanup_dll();
        return Status::Error(StatusCode::DeviceError, "vkCreateInstance missing");
    }

    VkApplicationInfo app_info{};
    app_info.sType = kVkStypeApplicationInfo;
    app_info.pApplicationName = "OpenScarlett-Enum";
    app_info.applicationVersion = VkMakeApiVersion(0, 0, 1, 0);
    app_info.pEngineName = "OpenScarlett-Vulkan";
    app_info.engineVersion = VkMakeApiVersion(0, 0, 1, 0);
    app_info.apiVersion = kVkApiVersion1_2;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = kVkStypeInstanceCreateInfo;
    inst_info.pApplicationInfo = &app_info;

    VkInstance inst = nullptr;
    if (create_inst(&inst_info, nullptr, &inst) != kVkSuccess || !inst) {
        app_info.apiVersion = kVkApiVersion1_1;
        if (create_inst(&inst_info, nullptr, &inst) != kVkSuccess || !inst) {
            cleanup_dll();
            return Status::Error(StatusCode::DeviceError, "vkCreateInstance failed during enumeration");
        }
    }
    auto destroy_inst = reinterpret_cast<PFN_vkDestroyInstance>(get_ipa(inst, "vkDestroyInstance"));
    auto enum_pdevs = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_ipa(inst, "vkEnumeratePhysicalDevices"));
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_ipa(inst, "vkGetPhysicalDeviceProperties"));
    auto get_qfs = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get_ipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto get_mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_ipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
    auto enum_dext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get_ipa(inst, "vkEnumerateDeviceExtensionProperties"));

    if (!enum_pdevs || !get_props || !get_qfs || !get_mem || !enum_dext) {
        if (destroy_inst) destroy_inst(inst, nullptr);
        cleanup_dll();
        return Status::Error(StatusCode::DeviceError, "Required Vulkan enumeration entry points missing");
    }

    uint32_t dev_count = 0u;
    if (enum_pdevs(inst, &dev_count, nullptr) == kVkSuccess && dev_count > 0u) {
        std::vector<VkPhysicalDevice> pdevs(dev_count, nullptr);
        if (enum_pdevs(inst, &dev_count, pdevs.data()) == kVkSuccess) {
            pdevs.resize(dev_count);
            std::vector<VulkanPhysicalDeviceCandidate> raw_candidates;
            raw_candidates.reserve(dev_count);
            for (uint32_t idx = 0u; idx < dev_count; ++idx) {
                VkPhysicalDevice pd = pdevs[idx];
                if (!pd) continue;
                VkPhysicalDevicePropertiesRaw props{};
                get_props(pd, &props);
                props.deviceName[sizeof(props.deviceName) - 1] = '\0';

                uint32_t qf_count = 0u;
                get_qfs(pd, &qf_count, nullptr);
                bool has_compute = false;
                bool has_graphics = false;
                if (qf_count > 0u) {
                    std::vector<VkQueueFamilyProperties> qfs(qf_count);
                    get_qfs(pd, &qf_count, qfs.data());
                    for (uint32_t q = 0u; q < qf_count; ++q) {
                        if (qfs[q].queueCount == 0u) continue;
                        if ((qfs[q].queueFlags & kVkQueueComputeBit) != 0u) has_compute = true;
                        if ((qfs[q].queueFlags & kVkQueueGraphicsBit) != 0u) has_graphics = true;
                    }
                }

                VkPhysicalDeviceMemoryProperties mp{};
                get_mem(pd, &mp);
                uint64_t vram_bytes = 0ull;
                const uint32_t heap_cnt = std::min<uint32_t>(mp.memoryHeapCount, 16u);
                for (uint32_t h = 0u; h < heap_cnt; ++h) {
                    if ((mp.memoryHeaps[h].flags & kVkMemoryHeapDeviceLocalBit) != 0u) {
                        vram_bytes += mp.memoryHeaps[h].size;
                    }
                }

                bool has_mesh = false;
                uint32_t dext_cnt = 0u;
                if (enum_dext(pd, nullptr, &dext_cnt, nullptr) == kVkSuccess && dext_cnt > 0u) {
                    std::vector<VkExtensionProperties> dexts(dext_cnt);
                    if (enum_dext(pd, nullptr, &dext_cnt, dexts.data()) == kVkSuccess) {
                        for (uint32_t e = 0u; e < dext_cnt; ++e) {
                            if (std::strcmp(dexts[e].extensionName, "VK_EXT_mesh_shader") == 0) {
                                has_mesh = true;
                                break;
                            }
                        }
                    }
                }

                VulkanPhysicalDeviceCandidate c{};
                c.enumerated_index = idx;
                c.device_name = props.deviceName;
                c.vendor_id = props.vendorID;
                c.device_id = props.deviceID;
                c.api_version = props.apiVersion;
                c.driver_version = props.driverVersion;
                c.device_type = static_cast<VulkanDeviceType>(props.deviceType);
                c.device_local_vram_bytes = vram_bytes;
                c.supports_compute = has_compute;
                c.supports_graphics = has_graphics;
                c.supports_mesh_shader = has_mesh;
                raw_candidates.push_back(std::move(c));
            }
            out_ranked_devices = RankPhysicalDeviceCandidates(std::move(raw_candidates));
        }
    }

    if (destroy_inst) destroy_inst(inst, nullptr);
    cleanup_dll();
    return Status::Ok();
#else
    return Status::Error(StatusCode::UnsupportedFormat, "Vulkan enumeration requires Win32");
#endif
}

Status VulkanBackend::Initialize(bool allow_software_fallback) {
    Shutdown();

    std::string failure_reason;
    const std::string gpu_selector_env = ReadEnvStringTrimmed("OPENSCARLETT_VK_GPU");
    const std::string legacy_device_env = ReadEnvStringTrimmed("OPENSCARLETT_VK_DEVICE");
    const std::string device_filter = !gpu_selector_env.empty() ? gpu_selector_env : legacy_device_env;
    const char* active_filter_env_name =
        !gpu_selector_env.empty() ? "OPENSCARLETT_VK_GPU" : "OPENSCARLETT_VK_DEVICE";
    const std::string force_sw = ReadEnvStringTrimmed("OPENSCARLETT_VK_FORCE_SOFTWARE");
    bool matched_filter = false;

    if (!force_sw.empty() && (force_sw[0] == '1' || force_sw[0] == 't' || force_sw[0] == 'T' ||
                              force_sw[0] == 'y' || force_sw[0] == 'Y')) {
        failure_reason = "OPENSCARLETT_VK_FORCE_SOFTWARE enabled";
        if (!allow_software_fallback) {
            device_info_.selected_device_filter = device_filter;
            device_info_.matched_device_filter = false;
            return Status::Error(StatusCode::DeviceError, failure_reason);
        }
        Status fb_st = InitializeSoftwareFallback();
        device_info_.selected_device_filter = device_filter;
        device_info_.matched_device_filter = false;
        return fb_st;
    }

#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        auto try_init_vulkan = [&]() -> bool {
            impl_->vulkan_dll = LoadLibraryA("vulkan-1.dll");
            if (!impl_->vulkan_dll) {
                failure_reason = "vulkan-1.dll not found";
                return false;
            }

            impl_->vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(impl_->vulkan_dll, "vkGetInstanceProcAddr"));
            if (!impl_->vkGetInstanceProcAddr) {
                failure_reason = "vkGetInstanceProcAddr export not found in vulkan-1.dll";
                return false;
            }

            impl_->vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
                impl_->vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
            impl_->vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                impl_->vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
            impl_->vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                impl_->vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
            if (!impl_->vkCreateInstance) {
                failure_reason = "vkCreateInstance not available";
                return false;
            }

            uint32_t loader_version = kVkApiVersion1_0;
            if (impl_->vkEnumerateInstanceVersion) {
                uint32_t queried = 0;
                if (impl_->vkEnumerateInstanceVersion(&queried) == kVkSuccess && queried >= kVkApiVersion1_0) {
                    loader_version = queried;
                }
            }

            // Probe instance extensions for Win32 surface & HDR10 swapchain colorspace.
            bool has_khr_surface = false;
            bool has_khr_win32_surface = false;
            bool has_ext_swapchain_cs = false;
            if (impl_->vkEnumerateInstanceExtensionProperties) {
                uint32_t ext_count = 0;
                if (impl_->vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr) == kVkSuccess &&
                    ext_count > 0) {
                    std::vector<VkExtensionProperties> exts(ext_count);
                    if (impl_->vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data()) == kVkSuccess) {
                        for (uint32_t i = 0; i < ext_count; ++i) {
                            if (std::strcmp(exts[i].extensionName, "VK_KHR_surface") == 0) has_khr_surface = true;
                            if (std::strcmp(exts[i].extensionName, "VK_KHR_win32_surface") == 0) has_khr_win32_surface = true;
                            if (std::strcmp(exts[i].extensionName, "VK_EXT_swapchain_colorspace") == 0) has_ext_swapchain_cs = true;
                        }
                    }
                }
            }

            std::vector<const char*> inst_exts;
            if (has_khr_surface && has_khr_win32_surface) {
                inst_exts.push_back("VK_KHR_surface");
                inst_exts.push_back("VK_KHR_win32_surface");
                if (has_ext_swapchain_cs) {
                    inst_exts.push_back("VK_EXT_swapchain_colorspace");
                }
            }

            // Target Vulkan 1.3 (required for SPV_EXT_mesh_shader / SPIR-V 1.4+) with graceful 1.2/1.1 fallback.
            uint32_t target_api = kVkApiVersion1_2;
            if (loader_version >= kVkApiVersion1_3) {
                target_api = kVkApiVersion1_3;
            } else if (loader_version < kVkApiVersion1_2) {
                target_api = loader_version;
            }

            VkApplicationInfo app_info{};
            app_info.sType = kVkStypeApplicationInfo;
            app_info.pApplicationName = "OpenScarlett";
            app_info.applicationVersion = VkMakeApiVersion(0, 0, 1, 0);
            app_info.pEngineName = "OpenScarlett-Vulkan";
            app_info.engineVersion = VkMakeApiVersion(0, 0, 1, 0);
            app_info.apiVersion = target_api;

            VkInstanceCreateInfo inst_info{};
            inst_info.sType = kVkStypeInstanceCreateInfo;
            inst_info.pApplicationInfo = &app_info;
            inst_info.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
            inst_info.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();

            VkResult vr = impl_->vkCreateInstance(&inst_info, nullptr, &impl_->instance);
            if (vr != kVkSuccess || !impl_->instance) {
                app_info.apiVersion = kVkApiVersion1_2;
                vr = impl_->vkCreateInstance(&inst_info, nullptr, &impl_->instance);
            }
            if (vr != kVkSuccess || !impl_->instance) {
                // Retry without optional WSI extensions
                inst_info.enabledExtensionCount = 0;
                inst_info.ppEnabledExtensionNames = nullptr;
                has_khr_surface = false;
                has_khr_win32_surface = false;
                has_ext_swapchain_cs = false;
                app_info.apiVersion = kVkApiVersion1_1;
                vr = impl_->vkCreateInstance(&inst_info, nullptr, &impl_->instance);
            }
            if (vr != kVkSuccess || !impl_->instance) {
                failure_reason = "vkCreateInstance failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            if (!impl_->LoadInstProc(impl_->vkDestroyInstance, "vkDestroyInstance") ||
                !impl_->LoadInstProc(impl_->vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceQueueFamilyProperties,
                                     "vkGetPhysicalDeviceQueueFamilyProperties") ||
                !impl_->LoadInstProc(impl_->vkGetPhysicalDeviceMemoryProperties,
                                     "vkGetPhysicalDeviceMemoryProperties") ||
                !impl_->LoadInstProc(impl_->vkEnumerateDeviceExtensionProperties,
                                     "vkEnumerateDeviceExtensionProperties") ||
                !impl_->LoadInstProc(impl_->vkCreateDevice, "vkCreateDevice") ||
                !impl_->LoadInstProc(impl_->vkGetDeviceProcAddr, "vkGetDeviceProcAddr")) {
                failure_reason = "Failed to resolve core Vulkan instance procedures";
                return false;
            }

            impl_->LoadInstProc(impl_->vkGetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures");
            impl_->LoadInstProc(impl_->vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
            impl_->LoadInstProc(impl_->vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
            if (has_khr_surface && has_khr_win32_surface) {
                const bool wsi_inst_ok =
                    impl_->LoadInstProc(impl_->vkCreateWin32SurfaceKHR, "vkCreateWin32SurfaceKHR") &&
                    impl_->LoadInstProc(impl_->vkDestroySurfaceKHR, "vkDestroySurfaceKHR") &&
                    impl_->LoadInstProc(impl_->vkGetPhysicalDeviceSurfaceSupportKHR,
                                        "vkGetPhysicalDeviceSurfaceSupportKHR") &&
                    impl_->LoadInstProc(impl_->vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
                                        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") &&
                    impl_->LoadInstProc(impl_->vkGetPhysicalDeviceSurfaceFormatsKHR,
                                        "vkGetPhysicalDeviceSurfaceFormatsKHR") &&
                    impl_->LoadInstProc(impl_->vkGetPhysicalDeviceSurfacePresentModesKHR,
                                        "vkGetPhysicalDeviceSurfacePresentModesKHR");
                impl_->LoadInstProc(impl_->vkGetPhysicalDeviceWin32PresentationSupportKHR,
                                    "vkGetPhysicalDeviceWin32PresentationSupportKHR");
                impl_->win32_surface_enabled = wsi_inst_ok;
                impl_->hdr10_colorspace_enabled = wsi_inst_ok && has_ext_swapchain_cs;
            }

            uint32_t dev_count = 0;
            vr = impl_->vkEnumeratePhysicalDevices(impl_->instance, &dev_count, nullptr);
            if (vr != kVkSuccess || dev_count == 0) {
                failure_reason = "No Vulkan physical devices enumerated";
                return false;
            }

            std::vector<VkPhysicalDevice> phys_devs(dev_count, nullptr);
            vr = impl_->vkEnumeratePhysicalDevices(impl_->instance, &dev_count, phys_devs.data());
            if (vr != kVkSuccess || dev_count == 0) {
                failure_reason = "vkEnumeratePhysicalDevices failed";
                return false;
            }
            phys_devs.resize(dev_count);

            struct EnumeratedPhysDevRecord {
                VkPhysicalDevice handle = nullptr;
                VkPhysicalDevicePropertiesRaw props{};
                uint32_t compute_qf = std::numeric_limits<uint32_t>::max();
                uint32_t graphics_qf = std::numeric_limits<uint32_t>::max();
                uint32_t sparse_qf = std::numeric_limits<uint32_t>::max();
                VulkanPhysicalDeviceCandidate candidate{};
            };

            std::vector<EnumeratedPhysDevRecord> records;
            std::vector<VulkanPhysicalDeviceCandidate> candidates;
            records.reserve(dev_count);
            candidates.reserve(dev_count);

            for (uint32_t idx = 0u; idx < dev_count; ++idx) {
                VkPhysicalDevice candidate_dev = phys_devs[idx];
                if (!candidate_dev) continue;
                VkPhysicalDevicePropertiesRaw props{};
                impl_->vkGetPhysicalDeviceProperties(candidate_dev, &props);

                uint32_t qf_count = 0;
                impl_->vkGetPhysicalDeviceQueueFamilyProperties(candidate_dev, &qf_count, nullptr);
                if (qf_count == 0) continue;
                std::vector<VkQueueFamilyProperties> qfs(qf_count);
                impl_->vkGetPhysicalDeviceQueueFamilyProperties(candidate_dev, &qf_count, qfs.data());

                uint32_t compute_qf = std::numeric_limits<uint32_t>::max();
                uint32_t graphics_qf = std::numeric_limits<uint32_t>::max();
                uint32_t sparse_qf = std::numeric_limits<uint32_t>::max();
                for (uint32_t q = 0; q < qf_count; ++q) {
                    if (qfs[q].queueCount == 0) continue;
                    if ((qfs[q].queueFlags & kVkQueueGraphicsBit) != 0) {
                        const bool win32_ok = !impl_->vkGetPhysicalDeviceWin32PresentationSupportKHR ||
                                              impl_->vkGetPhysicalDeviceWin32PresentationSupportKHR(candidate_dev, q) != 0;
                        if (graphics_qf == std::numeric_limits<uint32_t>::max() || win32_ok) {
                            if (graphics_qf == std::numeric_limits<uint32_t>::max()) {
                                graphics_qf = q;
                            }
                        }
                    }
                    if ((qfs[q].queueFlags & kVkQueueComputeBit) != 0) {
                        if (compute_qf == std::numeric_limits<uint32_t>::max() ||
                            (qfs[q].queueFlags & kVkQueueGraphicsBit) == 0) {
                            compute_qf = q;
                        }
                    }
                    if ((qfs[q].queueFlags & kVkQueueSparseBindingBit) != 0) {
                        if (sparse_qf == std::numeric_limits<uint32_t>::max() || q == graphics_qf) {
                            sparse_qf = q;
                        }
                    }
                }
                if (compute_qf == std::numeric_limits<uint32_t>::max()) continue;

                VkPhysicalDeviceMemoryProperties cand_mem{};
                impl_->vkGetPhysicalDeviceMemoryProperties(candidate_dev, &cand_mem);
                uint64_t cand_vram_bytes = 0ull;
                const uint32_t heap_cnt = std::min<uint32_t>(cand_mem.memoryHeapCount, 16u);
                for (uint32_t h = 0u; h < heap_cnt; ++h) {
                    if ((cand_mem.memoryHeaps[h].flags & kVkMemoryHeapDeviceLocalBit) != 0u) {
                        cand_vram_bytes += cand_mem.memoryHeaps[h].size;
                    }
                }

                bool cand_has_mesh = false;
                uint32_t cand_ext_count = 0u;
                if (impl_->vkEnumerateDeviceExtensionProperties(
                        candidate_dev, nullptr, &cand_ext_count, nullptr) == kVkSuccess &&
                    cand_ext_count > 0u) {
                    std::vector<VkExtensionProperties> cand_exts(cand_ext_count);
                    if (impl_->vkEnumerateDeviceExtensionProperties(
                            candidate_dev, nullptr, &cand_ext_count, cand_exts.data()) == kVkSuccess) {
                        for (uint32_t e = 0u; e < cand_ext_count; ++e) {
                            if (std::strcmp(cand_exts[e].extensionName, "VK_EXT_mesh_shader") == 0) {
                                cand_has_mesh = true;
                                break;
                            }
                        }
                    }
                }

                props.deviceName[sizeof(props.deviceName) - 1] = '\0';
                EnumeratedPhysDevRecord rec{};
                rec.handle = candidate_dev;
                rec.props = props;
                rec.compute_qf = compute_qf;
                rec.graphics_qf = graphics_qf;
                rec.sparse_qf = sparse_qf;
                rec.candidate.enumerated_index = idx;
                rec.candidate.device_name = props.deviceName;
                rec.candidate.vendor_id = props.vendorID;
                rec.candidate.device_id = props.deviceID;
                rec.candidate.api_version = props.apiVersion;
                rec.candidate.driver_version = props.driverVersion;
                rec.candidate.device_type = static_cast<VulkanDeviceType>(props.deviceType);
                rec.candidate.device_local_vram_bytes = cand_vram_bytes;
                rec.candidate.supports_compute = true;
                rec.candidate.supports_graphics =
                    (graphics_qf != std::numeric_limits<uint32_t>::max());
                rec.candidate.supports_mesh_shader = cand_has_mesh;
                rec.candidate.rank_score = ScorePhysicalDeviceCandidate(rec.candidate);
                candidates.push_back(rec.candidate);
                records.push_back(std::move(rec));
            }

            VkPhysicalDevice best_dev = nullptr;
            VkPhysicalDevicePropertiesRaw best_props{};
            uint32_t best_compute_qf = std::numeric_limits<uint32_t>::max();
            uint32_t best_graphics_qf = std::numeric_limits<uint32_t>::max();
            uint32_t best_sparse_qf = std::numeric_limits<uint32_t>::max();
            uint64_t best_vram_bytes = 0ull;
            uint64_t best_rank_score = 0ull;
            uint32_t best_enum_idx = 0u;

            VulkanPhysicalDeviceCandidate chosen_candidate{};
            bool numeric_selector_fell_back = false;
            if (SelectPhysicalDeviceCandidateWithNumericFallback(
                    candidates, device_filter, chosen_candidate, &matched_filter, &numeric_selector_fell_back)) {
                if (numeric_selector_fell_back) {
                    std::cerr << "[OpenScarlett][Vulkan] WARNING: " << active_filter_env_name
                              << "=\"" << device_filter << "\" is out of range (available devices: "
                              << candidates.size() << "); falling back to best-ranked GPU \""
                              << chosen_candidate.device_name << "\".\n";
                }
                for (const auto& rec : records) {
                    if (rec.candidate.enumerated_index == chosen_candidate.enumerated_index) {
                        best_dev = rec.handle;
                        best_props = rec.props;
                        best_compute_qf = rec.compute_qf;
                        best_graphics_qf = rec.graphics_qf;
                        best_sparse_qf = rec.sparse_qf;
                        best_vram_bytes = rec.candidate.device_local_vram_bytes;
                        best_rank_score = chosen_candidate.rank_score;
                        best_enum_idx = rec.candidate.enumerated_index;
                        break;
                    }
                }
            }

            if (!best_dev) {
                if (!device_filter.empty()) {
                    failure_reason =
                        std::string("No Vulkan physical device matched ") + active_filter_env_name +
                        "=\"" + device_filter + "\"";
                } else {
                    failure_reason = "No Vulkan physical device with a compute-capable queue family";
                }
                return false;
            }

            impl_->physical_device = best_dev;
            impl_->compute_queue_family = best_compute_qf;
            impl_->has_graphics_queue = (best_graphics_qf != std::numeric_limits<uint32_t>::max());
            impl_->graphics_queue_family = impl_->has_graphics_queue ? best_graphics_qf : 0u;
            impl_->has_sparse_queue = (best_sparse_qf != std::numeric_limits<uint32_t>::max());
            impl_->sparse_queue_family = impl_->has_sparse_queue ? best_sparse_qf : 0u;
            impl_->vkGetPhysicalDeviceMemoryProperties(best_dev, &impl_->mem_props);

            // Probe physical device core features (depthBounds, BC compression, sparse, residency).
            VkPhysicalDeviceFeatures supported_feats{};
            if (impl_->vkGetPhysicalDeviceFeatures) {
                impl_->vkGetPhysicalDeviceFeatures(best_dev, &supported_feats);
            }

            // Probe device extensions (swapchain, mesh shader, sample locations, custom border color, sampler feedback)
            bool has_dev_swapchain = false;
            bool has_dev_mesh_shader = false;
            bool has_dev_sample_locations = false;
            bool has_dev_custom_border_color = false;
            bool has_dev_sampler_feedback = false;
            {
                uint32_t dext_count = 0;
                if (impl_->vkEnumerateDeviceExtensionProperties(best_dev, nullptr, &dext_count, nullptr) == kVkSuccess &&
                    dext_count > 0) {
                    std::vector<VkExtensionProperties> dexts(dext_count);
                    if (impl_->vkEnumerateDeviceExtensionProperties(best_dev, nullptr, &dext_count, dexts.data()) == kVkSuccess) {
                        for (uint32_t i = 0; i < dext_count; ++i) {
                            if (std::strcmp(dexts[i].extensionName, "VK_KHR_swapchain") == 0) has_dev_swapchain = true;
                            if (std::strcmp(dexts[i].extensionName, "VK_EXT_mesh_shader") == 0) has_dev_mesh_shader = true;
                            if (std::strcmp(dexts[i].extensionName, "VK_EXT_sample_locations") == 0) has_dev_sample_locations = true;
                            if (std::strcmp(dexts[i].extensionName, "VK_EXT_custom_border_color") == 0) has_dev_custom_border_color = true;
                            if (std::strcmp(dexts[i].extensionName, "VK_EXT_sampler_feedback") == 0 ||
                                std::strcmp(dexts[i].extensionName, "VK_KHR_sampler_feedback") == 0) {
                                has_dev_sampler_feedback = true;
                            }
                        }
                    }
                }
            }

            // Query VkPhysicalDeviceProperties::limits.maxPushConstantsSize and chained Properties2
            // (SubgroupProperties, VK_EXT_sample_locations, VK_EXT_custom_border_color).
            uint32_t queried_max_pc = 128u;
            std::memcpy(&queried_max_pc,
                        best_props.limits_and_sparse_storage + kVkLimitsOffsetMaxPushConstantsSize,
                        sizeof(uint32_t));
            if (queried_max_pc < 128u || queried_max_pc > 65536u) {
                queried_max_pc = 128u;
            }

            uint32_t queried_subgroup_size = 32u;
            uint32_t queried_subgroup_stages = 0u;
            uint32_t queried_subgroup_ops = 0u;
            VkPhysicalDeviceSubgroupProperties sub_props{};
            sub_props.sType = kVkStypePhysicalDeviceSubgroupProperties;
            VkPhysicalDeviceSampleLocationsPropertiesEXT sample_loc_props{};
            sample_loc_props.sType = kVkStypePhysicalDeviceSampleLocationsPropertiesExt;
            VkPhysicalDeviceCustomBorderColorPropertiesEXT cbc_props{};
            cbc_props.sType = kVkStypePhysicalDeviceCustomBorderColorPropertiesExt;
            VkPhysicalDeviceDepthStencilResolveProperties ds_resolve_props{};
            ds_resolve_props.sType = kVkStypePhysicalDeviceDepthStencilResolveProperties;

            if (impl_->vkGetPhysicalDeviceProperties2 && best_props.apiVersion >= kVkApiVersion1_1) {
                void* pnext_head = nullptr;
                if (best_props.apiVersion >= kVkApiVersion1_2) {
                    ds_resolve_props.pNext = pnext_head;
                    pnext_head = &ds_resolve_props;
                }
                if (has_dev_custom_border_color) {
                    cbc_props.pNext = pnext_head;
                    pnext_head = &cbc_props;
                }
                if (has_dev_sample_locations) {
                    sample_loc_props.pNext = pnext_head;
                    pnext_head = &sample_loc_props;
                }
                sub_props.pNext = pnext_head;
                pnext_head = &sub_props;

                VkPhysicalDeviceProperties2 props2{};
                props2.sType = kVkStypePhysicalDeviceProperties2;
                props2.pNext = pnext_head;
                impl_->vkGetPhysicalDeviceProperties2(best_dev, &props2);

                if (sub_props.subgroupSize > 0u) {
                    queried_subgroup_size = sub_props.subgroupSize;
                }
                queried_subgroup_stages = sub_props.supportedStages;
                queried_subgroup_ops = sub_props.supportedOperations;
            }

            // Probe chained VkPhysicalDeviceFeatures2 (core + Vulkan 1.2/1.3 + Mesh Shader + Custom Border Color).
            VkPhysicalDeviceVulkan12Features queried_v12_feats{};
            queried_v12_feats.sType = kVkStypePhysicalDeviceVulkan12Features;
            VkPhysicalDeviceVulkan13Features queried_v13_feats{};
            queried_v13_feats.sType = kVkStypePhysicalDeviceVulkan13Features;
            VkPhysicalDeviceMeshShaderFeaturesEXT queried_mesh_feats{};
            queried_mesh_feats.sType = kVkStypePhysicalDeviceMeshShaderFeaturesExt;
            VkPhysicalDeviceCustomBorderColorFeaturesEXT queried_cbc_feats{};
            queried_cbc_feats.sType = kVkStypePhysicalDeviceCustomBorderColorFeaturesExt;
            bool probed_v12 = false;
            bool probed_v13 = false;

            if (impl_->vkGetPhysicalDeviceFeatures2 && best_props.apiVersion >= kVkApiVersion1_1) {
                void* fnext_head = nullptr;
                if (has_dev_custom_border_color) {
                    queried_cbc_feats.pNext = fnext_head;
                    fnext_head = &queried_cbc_feats;
                }
                if (has_dev_mesh_shader && best_props.apiVersion >= kVkApiVersion1_2) {
                    queried_mesh_feats.pNext = fnext_head;
                    fnext_head = &queried_mesh_feats;
                }
                if (best_props.apiVersion >= kVkApiVersion1_3) {
                    queried_v13_feats.pNext = fnext_head;
                    fnext_head = &queried_v13_feats;
                    probed_v13 = true;
                }
                if (best_props.apiVersion >= kVkApiVersion1_2) {
                    queried_v12_feats.pNext = fnext_head;
                    fnext_head = &queried_v12_feats;
                    probed_v12 = true;
                }
                VkPhysicalDeviceFeatures2 feats2{};
                feats2.sType = kVkStypePhysicalDeviceFeatures2;
                feats2.pNext = fnext_head;
                impl_->vkGetPhysicalDeviceFeatures2(best_dev, &feats2);
                supported_feats = feats2.features;
            }

            bool want_multi_draw_indirect = (supported_feats.fields[kVkFeatMultiDrawIndirect] != 0u);
            bool want_depth_bounds = (supported_feats.fields[kVkFeatDepthBounds] != 0u);
            bool want_bc_compression = (supported_feats.fields[kVkFeatTextureCompressionBC] != 0u);
            bool want_sparse_binding =
                impl_->has_sparse_queue && (supported_feats.fields[kVkFeatSparseBinding] != 0u);
            bool want_sparse_2d =
                want_sparse_binding && (supported_feats.fields[kVkFeatSparseResidencyImage2D] != 0u);
            bool want_sparse_3d =
                want_sparse_binding && (supported_feats.fields[kVkFeatSparseResidencyImage3D] != 0u);
            bool enable_v12_draw_indirect_count =
                probed_v12 && (queried_v12_feats.fields[kVkFeat12DrawIndirectCount] != 0u);
            bool enable_v13_dynamic_rendering =
                probed_v13 && (queried_v13_feats.fields[kVkFeat13DynamicRendering] != 0u);

            bool enable_mesh = false;
            bool enable_task = false;
            if (has_dev_mesh_shader && impl_->has_graphics_queue &&
                best_props.apiVersion >= kVkApiVersion1_2 && queried_mesh_feats.meshShader != 0u) {
                enable_mesh = true;
                enable_task = (queried_mesh_feats.taskShader != 0u);
            }

            bool enable_custom_border_color =
                has_dev_custom_border_color && (queried_cbc_feats.customBorderColors != 0u);
            bool enable_custom_border_without_format =
                enable_custom_border_color && (queried_cbc_feats.customBorderColorWithoutFormat != 0u);

            std::vector<const char*> dev_exts;
            if (has_dev_swapchain && impl_->has_graphics_queue && impl_->win32_surface_enabled) {
                dev_exts.push_back("VK_KHR_swapchain");
            }
            VkPhysicalDeviceMeshShaderFeaturesEXT enable_mesh_feats{};
            enable_mesh_feats.sType = kVkStypePhysicalDeviceMeshShaderFeaturesExt;
            if (enable_mesh) {
                dev_exts.push_back("VK_EXT_mesh_shader");
                enable_mesh_feats.meshShader = 1u;
                enable_mesh_feats.taskShader = enable_task ? 1u : 0u;
            }
            VkPhysicalDeviceCustomBorderColorFeaturesEXT enable_cbc_feats{};
            enable_cbc_feats.sType = kVkStypePhysicalDeviceCustomBorderColorFeaturesExt;
            if (enable_custom_border_color) {
                dev_exts.push_back("VK_EXT_custom_border_color");
                enable_cbc_feats.customBorderColors = 1u;
                enable_cbc_feats.customBorderColorWithoutFormat =
                    enable_custom_border_without_format ? 1u : 0u;
            }

            const float queue_priority = 1.0f;
            std::vector<VkDeviceQueueCreateInfo> queue_cis;
            {
                VkDeviceQueueCreateInfo qci{};
                qci.sType = kVkStypeDeviceQueueCreateInfo;
                qci.queueFamilyIndex = best_compute_qf;
                qci.queueCount = 1;
                qci.pQueuePriorities = &queue_priority;
                queue_cis.push_back(qci);
            }
            if (impl_->has_graphics_queue && best_graphics_qf != best_compute_qf) {
                VkDeviceQueueCreateInfo qci{};
                qci.sType = kVkStypeDeviceQueueCreateInfo;
                qci.queueFamilyIndex = best_graphics_qf;
                qci.queueCount = 1;
                qci.pQueuePriorities = &queue_priority;
                queue_cis.push_back(qci);
            }
            if (impl_->has_sparse_queue &&
                best_sparse_qf != best_compute_qf &&
                (!impl_->has_graphics_queue || best_sparse_qf != best_graphics_qf)) {
                VkDeviceQueueCreateInfo qci{};
                qci.sType = kVkStypeDeviceQueueCreateInfo;
                qci.queueFamilyIndex = best_sparse_qf;
                qci.queueCount = 1;
                qci.pQueuePriorities = &queue_priority;
                queue_cis.push_back(qci);
            }

            VkPhysicalDeviceVulkan12Features enable_v12_feats{};
            enable_v12_feats.sType = kVkStypePhysicalDeviceVulkan12Features;
            VkPhysicalDeviceVulkan13Features enable_v13_feats{};
            enable_v13_feats.sType = kVkStypePhysicalDeviceVulkan13Features;

            VkPhysicalDeviceFeatures enabled_feats{};
            auto sync_enabled_feats = [&]() -> const void* {
                enabled_feats = VkPhysicalDeviceFeatures{};
                if (want_multi_draw_indirect) enabled_feats.fields[kVkFeatMultiDrawIndirect] = 1u;
                if (want_depth_bounds) enabled_feats.fields[kVkFeatDepthBounds] = 1u;
                if (want_bc_compression) enabled_feats.fields[kVkFeatTextureCompressionBC] = 1u;
                if (want_sparse_binding) {
                    enabled_feats.fields[kVkFeatSparseBinding] = 1u;
                    if (want_sparse_2d) enabled_feats.fields[kVkFeatSparseResidencyImage2D] = 1u;
                    if (want_sparse_3d) enabled_feats.fields[kVkFeatSparseResidencyImage3D] = 1u;
                }
                const bool any_core = want_multi_draw_indirect || want_depth_bounds || want_bc_compression || want_sparse_binding;
                return any_core ? static_cast<const void*>(&enabled_feats) : nullptr;
            };

            auto rebuild_dev_pnext = [&]() -> const void* {
                void* head = nullptr;
                if (enable_v13_dynamic_rendering) {
                    enable_v13_feats = VkPhysicalDeviceVulkan13Features{};
                    enable_v13_feats.sType = kVkStypePhysicalDeviceVulkan13Features;
                    enable_v13_feats.fields[kVkFeat13DynamicRendering] = 1u;
                    enable_v13_feats.pNext = head;
                    head = &enable_v13_feats;
                }
                if (enable_v12_draw_indirect_count) {
                    enable_v12_feats = VkPhysicalDeviceVulkan12Features{};
                    enable_v12_feats.sType = kVkStypePhysicalDeviceVulkan12Features;
                    enable_v12_feats.fields[kVkFeat12DrawIndirectCount] = 1u;
                    enable_v12_feats.pNext = head;
                    head = &enable_v12_feats;
                }
                if (enable_custom_border_color) {
                    enable_cbc_feats.pNext = head;
                    head = &enable_cbc_feats;
                }
                if (enable_mesh) {
                    enable_mesh_feats.pNext = head;
                    head = &enable_mesh_feats;
                }
                return head;
            };

            VkDeviceCreateInfo dev_ci{};
            dev_ci.sType = kVkStypeDeviceCreateInfo;
            dev_ci.pNext = rebuild_dev_pnext();
            dev_ci.queueCreateInfoCount = static_cast<uint32_t>(queue_cis.size());
            dev_ci.pQueueCreateInfos = queue_cis.data();
            dev_ci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
            dev_ci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
            dev_ci.pEnabledFeatures = sync_enabled_feats();

            vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            if ((vr != kVkSuccess || !impl_->device) &&
                (enable_v13_dynamic_rendering || enable_v12_draw_indirect_count)) {
                enable_v13_dynamic_rendering = false;
                enable_v12_draw_indirect_count = false;
                dev_ci.pNext = rebuild_dev_pnext();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && want_multi_draw_indirect) {
                want_multi_draw_indirect = false;
                dev_ci.pEnabledFeatures = sync_enabled_feats();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && enable_custom_border_color) {
                enable_custom_border_color = false;
                enable_custom_border_without_format = false;
                dev_exts.erase(std::remove_if(dev_exts.begin(), dev_exts.end(), [](const char* s) {
                    return std::strcmp(s, "VK_EXT_custom_border_color") == 0;
                }), dev_exts.end());
                dev_ci.pNext = rebuild_dev_pnext();
                dev_ci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
                dev_ci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && want_sparse_binding) {
                // Retry without sparse features if driver rejected them.
                want_sparse_binding = false;
                want_sparse_2d = false;
                want_sparse_3d = false;
                dev_ci.pEnabledFeatures = sync_enabled_feats();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && want_depth_bounds) {
                want_depth_bounds = false;
                dev_ci.pEnabledFeatures = sync_enabled_feats();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && want_bc_compression) {
                want_bc_compression = false;
                dev_ci.pEnabledFeatures = sync_enabled_feats();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if ((vr != kVkSuccess || !impl_->device) && enable_mesh) {
                // Retry without VK_EXT_mesh_shader if driver rejected feature chain
                enable_mesh = false;
                enable_task = false;
                dev_exts.erase(std::remove_if(dev_exts.begin(), dev_exts.end(), [](const char* s) {
                    return std::strcmp(s, "VK_EXT_mesh_shader") == 0;
                }), dev_exts.end());
                dev_ci.pNext = rebuild_dev_pnext();
                dev_ci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
                dev_ci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
                vr = impl_->vkCreateDevice(best_dev, &dev_ci, nullptr, &impl_->device);
            }
            if (vr != kVkSuccess || !impl_->device) {
                failure_reason = "vkCreateDevice failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            const bool loaded_core =
                impl_->LoadDevProc(impl_->vkDestroyDevice, "vkDestroyDevice") &&
                impl_->LoadDevProc(impl_->vkDeviceWaitIdle, "vkDeviceWaitIdle") &&
                impl_->LoadDevProc(impl_->vkGetDeviceQueue, "vkGetDeviceQueue") &&
                impl_->LoadDevProc(impl_->vkQueueSubmit, "vkQueueSubmit") &&
                impl_->LoadDevProc(impl_->vkQueueWaitIdle, "vkQueueWaitIdle") &&
                impl_->LoadDevProc(impl_->vkCreateFence, "vkCreateFence") &&
                impl_->LoadDevProc(impl_->vkDestroyFence, "vkDestroyFence") &&
                impl_->LoadDevProc(impl_->vkWaitForFences, "vkWaitForFences") &&
                impl_->LoadDevProc(impl_->vkCreateSemaphore, "vkCreateSemaphore") &&
                impl_->LoadDevProc(impl_->vkDestroySemaphore, "vkDestroySemaphore") &&
                impl_->LoadDevProc(impl_->vkCreateCommandPool, "vkCreateCommandPool") &&
                impl_->LoadDevProc(impl_->vkDestroyCommandPool, "vkDestroyCommandPool") &&
                impl_->LoadDevProc(impl_->vkAllocateCommandBuffers, "vkAllocateCommandBuffers") &&
                impl_->LoadDevProc(impl_->vkFreeCommandBuffers, "vkFreeCommandBuffers") &&
                impl_->LoadDevProc(impl_->vkBeginCommandBuffer, "vkBeginCommandBuffer") &&
                impl_->LoadDevProc(impl_->vkEndCommandBuffer, "vkEndCommandBuffer") &&
                impl_->LoadDevProc(impl_->vkCmdBindPipeline, "vkCmdBindPipeline") &&
                impl_->LoadDevProc(impl_->vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets") &&
                impl_->LoadDevProc(impl_->vkCmdDispatch, "vkCmdDispatch") &&
                impl_->LoadDevProc(impl_->vkCmdPipelineBarrier, "vkCmdPipelineBarrier") &&
                impl_->LoadDevProc(impl_->vkCreateBuffer, "vkCreateBuffer") &&
                impl_->LoadDevProc(impl_->vkDestroyBuffer, "vkDestroyBuffer") &&
                impl_->LoadDevProc(impl_->vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements") &&
                impl_->LoadDevProc(impl_->vkAllocateMemory, "vkAllocateMemory") &&
                impl_->LoadDevProc(impl_->vkFreeMemory, "vkFreeMemory") &&
                impl_->LoadDevProc(impl_->vkBindBufferMemory, "vkBindBufferMemory") &&
                impl_->LoadDevProc(impl_->vkMapMemory, "vkMapMemory") &&
                impl_->LoadDevProc(impl_->vkUnmapMemory, "vkUnmapMemory") &&
                impl_->LoadDevProc(impl_->vkFlushMappedMemoryRanges, "vkFlushMappedMemoryRanges") &&
                impl_->LoadDevProc(impl_->vkInvalidateMappedMemoryRanges, "vkInvalidateMappedMemoryRanges") &&
                impl_->LoadDevProc(impl_->vkCreateShaderModule, "vkCreateShaderModule") &&
                impl_->LoadDevProc(impl_->vkDestroyShaderModule, "vkDestroyShaderModule") &&
                impl_->LoadDevProc(impl_->vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout") &&
                impl_->LoadDevProc(impl_->vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") &&
                impl_->LoadDevProc(impl_->vkCreatePipelineLayout, "vkCreatePipelineLayout") &&
                impl_->LoadDevProc(impl_->vkDestroyPipelineLayout, "vkDestroyPipelineLayout") &&
                impl_->LoadDevProc(impl_->vkCreateComputePipelines, "vkCreateComputePipelines") &&
                impl_->LoadDevProc(impl_->vkDestroyPipeline, "vkDestroyPipeline") &&
                impl_->LoadDevProc(impl_->vkCreateDescriptorPool, "vkCreateDescriptorPool") &&
                impl_->LoadDevProc(impl_->vkDestroyDescriptorPool, "vkDestroyDescriptorPool") &&
                impl_->LoadDevProc(impl_->vkAllocateDescriptorSets, "vkAllocateDescriptorSets") &&
                impl_->LoadDevProc(impl_->vkUpdateDescriptorSets, "vkUpdateDescriptorSets");

            if (!loaded_core) {
                failure_reason = "Failed to resolve core Vulkan device procedures";
                return false;
            }

            // Load Vulkan graphics procedures
            const bool loaded_gfx =
                impl_->LoadDevProc(impl_->vkCreateRenderPass, "vkCreateRenderPass") &&
                impl_->LoadDevProc(impl_->vkDestroyRenderPass, "vkDestroyRenderPass") &&
                impl_->LoadDevProc(impl_->vkCreateImage, "vkCreateImage") &&
                impl_->LoadDevProc(impl_->vkDestroyImage, "vkDestroyImage") &&
                impl_->LoadDevProc(impl_->vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements") &&
                impl_->LoadDevProc(impl_->vkBindImageMemory, "vkBindImageMemory") &&
                impl_->LoadDevProc(impl_->vkCreateImageView, "vkCreateImageView") &&
                impl_->LoadDevProc(impl_->vkDestroyImageView, "vkDestroyImageView") &&
                impl_->LoadDevProc(impl_->vkCreateSampler, "vkCreateSampler") &&
                impl_->LoadDevProc(impl_->vkDestroySampler, "vkDestroySampler") &&
                impl_->LoadDevProc(impl_->vkCreateFramebuffer, "vkCreateFramebuffer") &&
                impl_->LoadDevProc(impl_->vkDestroyFramebuffer, "vkDestroyFramebuffer") &&
                impl_->LoadDevProc(impl_->vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines") &&
                impl_->LoadDevProc(impl_->vkCmdBeginRenderPass, "vkCmdBeginRenderPass") &&
                impl_->LoadDevProc(impl_->vkCmdEndRenderPass, "vkCmdEndRenderPass") &&
                impl_->LoadDevProc(impl_->vkCmdBindVertexBuffers, "vkCmdBindVertexBuffers") &&
                impl_->LoadDevProc(impl_->vkCmdBindIndexBuffer, "vkCmdBindIndexBuffer") &&
                impl_->LoadDevProc(impl_->vkCmdDraw, "vkCmdDraw") &&
                impl_->LoadDevProc(impl_->vkCmdDrawIndexed, "vkCmdDrawIndexed") &&
                impl_->LoadDevProc(impl_->vkCmdPushConstants, "vkCmdPushConstants") &&
                impl_->LoadDevProc(impl_->vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage") &&
                impl_->LoadDevProc(impl_->vkCmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") &&
                impl_->LoadDevProc(impl_->vkCmdResolveImage, "vkCmdResolveImage") &&
                impl_->LoadDevProc(impl_->vkCmdSetViewport, "vkCmdSetViewport") &&
                impl_->LoadDevProc(impl_->vkCmdSetScissor, "vkCmdSetScissor");

            if (!loaded_gfx) {
                impl_->has_graphics_queue = false;
            }

            if (want_sparse_binding &&
                impl_->LoadDevProc(impl_->vkGetImageSparseMemoryRequirements, "vkGetImageSparseMemoryRequirements") &&
                impl_->LoadDevProc(impl_->vkQueueBindSparse, "vkQueueBindSparse")) {
                impl_->sparse_binding_enabled = true;
                impl_->sparse_residency_2d_enabled = want_sparse_2d;
                impl_->sparse_residency_3d_enabled = want_sparse_3d;
            }

            if (enable_mesh && impl_->has_graphics_queue &&
                impl_->LoadDevProc(impl_->vkCmdDrawMeshTasksEXT, "vkCmdDrawMeshTasksEXT")) {
                impl_->mesh_shader_enabled = true;
                impl_->task_shader_enabled = enable_task;
                impl_->LoadDevProc(impl_->vkCmdDrawMeshTasksIndirectEXT, "vkCmdDrawMeshTasksIndirectEXT");
                impl_->LoadDevProc(impl_->vkCmdDrawMeshTasksIndirectCountEXT, "vkCmdDrawMeshTasksIndirectCountEXT");
            }

            // Load indirect draw/dispatch & Vulkan 1.3/1.4 dynamic rendering procs
            impl_->LoadDevProc(impl_->vkCmdDrawIndirect, "vkCmdDrawIndirect");
            impl_->LoadDevProc(impl_->vkCmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect");
            impl_->LoadDevProc(impl_->vkCmdDispatchIndirect, "vkCmdDispatchIndirect");
            if (!impl_->LoadDevProc(impl_->vkCmdDrawIndirectCount, "vkCmdDrawIndirectCount")) {
                impl_->LoadDevProc(impl_->vkCmdDrawIndirectCount, "vkCmdDrawIndirectCountKHR");
            }
            if (!impl_->LoadDevProc(impl_->vkCmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCount")) {
                impl_->LoadDevProc(impl_->vkCmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCountKHR");
            }
            if (enable_v13_dynamic_rendering) {
                const bool loaded_dyn =
                    (impl_->LoadDevProc(impl_->vkCmdBeginRendering, "vkCmdBeginRendering") ||
                     impl_->LoadDevProc(impl_->vkCmdBeginRendering, "vkCmdBeginRenderingKHR")) &&
                    (impl_->LoadDevProc(impl_->vkCmdEndRendering, "vkCmdEndRendering") ||
                     impl_->LoadDevProc(impl_->vkCmdEndRendering, "vkCmdEndRenderingKHR"));
                impl_->dynamic_rendering_enabled = loaded_dyn;
            }
            impl_->multi_draw_indirect_enabled = want_multi_draw_indirect;
            impl_->draw_indirect_count_enabled =
                enable_v12_draw_indirect_count &&
                impl_->vkCmdDrawIndirectCount != nullptr &&
                impl_->vkCmdDrawIndexedIndirectCount != nullptr;
            impl_->supported_depth_resolve_modes = ds_resolve_props.supportedDepthResolveModes;
            impl_->supported_stencil_resolve_modes = ds_resolve_props.supportedStencilResolveModes;

            if (has_dev_swapchain && impl_->has_graphics_queue && impl_->win32_surface_enabled) {
                impl_->swapchain_enabled =
                    impl_->LoadDevProc(impl_->vkCreateSwapchainKHR, "vkCreateSwapchainKHR") &&
                    impl_->LoadDevProc(impl_->vkDestroySwapchainKHR, "vkDestroySwapchainKHR") &&
                    impl_->LoadDevProc(impl_->vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR") &&
                    impl_->LoadDevProc(impl_->vkAcquireNextImageKHR, "vkAcquireNextImageKHR") &&
                    impl_->LoadDevProc(impl_->vkQueuePresentKHR, "vkQueuePresentKHR");
            }

            impl_->vkGetDeviceQueue(impl_->device, best_compute_qf, 0, &impl_->compute_queue);
            if (!impl_->compute_queue) {
                failure_reason = "vkGetDeviceQueue returned null compute queue";
                return false;
            }

            VkCommandPoolCreateInfo pool_ci{};
            pool_ci.sType = kVkStypeCommandPoolCreateInfo;
            pool_ci.flags = kVkCommandPoolCreateResetCmdBufBit;
            pool_ci.queueFamilyIndex = best_compute_qf;
            vr = impl_->vkCreateCommandPool(impl_->device, &pool_ci, nullptr, &impl_->command_pool);
            if (vr != kVkSuccess || impl_->command_pool == kVkNullHandle) {
                failure_reason = "vkCreateCommandPool failed (VkResult=" + std::to_string(vr) + ")";
                return false;
            }

            if (impl_->has_graphics_queue) {
                if (best_graphics_qf == best_compute_qf) {
                    impl_->graphics_queue = impl_->compute_queue;
                    impl_->graphics_command_pool = impl_->command_pool;
                } else {
                    impl_->vkGetDeviceQueue(impl_->device, best_graphics_qf, 0, &impl_->graphics_queue);
                    VkCommandPoolCreateInfo gpool_ci{};
                    gpool_ci.sType = kVkStypeCommandPoolCreateInfo;
                    gpool_ci.flags = kVkCommandPoolCreateResetCmdBufBit;
                    gpool_ci.queueFamilyIndex = best_graphics_qf;
                    if (!impl_->graphics_queue ||
                        impl_->vkCreateCommandPool(impl_->device, &gpool_ci, nullptr, &impl_->graphics_command_pool) != kVkSuccess) {
                        impl_->has_graphics_queue = false;
                        impl_->graphics_queue = nullptr;
                        impl_->graphics_command_pool = kVkNullHandle;
                    }
                }
            }

            if (impl_->sparse_binding_enabled && impl_->has_sparse_queue) {
                if (best_sparse_qf == best_compute_qf) {
                    impl_->sparse_queue = impl_->compute_queue;
                } else if (impl_->has_graphics_queue && best_sparse_qf == best_graphics_qf) {
                    impl_->sparse_queue = impl_->graphics_queue;
                } else {
                    impl_->vkGetDeviceQueue(impl_->device, best_sparse_qf, 0, &impl_->sparse_queue);
                }
                if (!impl_->sparse_queue) {
                    impl_->sparse_binding_enabled = false;
                    impl_->sparse_residency_2d_enabled = false;
                    impl_->sparse_residency_3d_enabled = false;
                }
            }

            impl_->depth_bounds_enabled = want_depth_bounds;
            impl_->custom_border_color_enabled = enable_custom_border_color;
            impl_->custom_border_color_without_format_enabled = enable_custom_border_without_format;
            impl_->max_custom_border_color_samplers = cbc_props.maxCustomBorderColorSamplers;

            best_props.deviceName[sizeof(best_props.deviceName) - 1] = '\0';
            device_info_ = VulkanDeviceInfo{};
            device_info_.device_name = best_props.deviceName;
            device_info_.vendor_id = best_props.vendorID;
            device_info_.device_id = best_props.deviceID;
            device_info_.api_version = best_props.apiVersion;
            device_info_.api_major = VkApiMajor(best_props.apiVersion);
            device_info_.api_minor = VkApiMinor(best_props.apiVersion);
            device_info_.api_patch = VkApiPatch(best_props.apiVersion);
            device_info_.driver_version = best_props.driverVersion;
            device_info_.device_type = static_cast<VulkanDeviceType>(best_props.deviceType);
            device_info_.compute_queue_family = best_compute_qf;
            device_info_.graphics_queue_family = impl_->graphics_queue_family;
            device_info_.supports_compute = true;
            device_info_.supports_graphics = impl_->has_graphics_queue;
            device_info_.supports_mesh_shader = impl_->mesh_shader_enabled;
            device_info_.supports_task_shader = impl_->task_shader_enabled;
            device_info_.supports_win32_surface = impl_->win32_surface_enabled;
            device_info_.supports_swapchain = impl_->swapchain_enabled;
            device_info_.supports_hdr10_colorspace = impl_->hdr10_colorspace_enabled;
            device_info_.sparse_queue_family = impl_->sparse_queue_family;
            device_info_.supports_sparse_binding = impl_->sparse_binding_enabled;
            device_info_.supports_sparse_residency_image_2d = impl_->sparse_residency_2d_enabled;
            device_info_.supports_sparse_residency_image_3d = impl_->sparse_residency_3d_enabled;
            device_info_.max_push_constants_size = queried_max_pc;
            device_info_.subgroup_size = queried_subgroup_size;
            device_info_.supported_subgroup_stages = queried_subgroup_stages;
            device_info_.supported_subgroup_operations = queried_subgroup_ops;
            device_info_.supports_bc_texture_compression = want_bc_compression;
            device_info_.selected_device_filter = device_filter;
            device_info_.matched_device_filter = matched_filter;
            device_info_.numeric_selector_fell_back = numeric_selector_fell_back;
            device_info_.device_local_vram_bytes = best_vram_bytes;
            device_info_.device_rank_score = best_rank_score;
            device_info_.enumerated_device_index = best_enum_idx;
            device_info_.supports_depth_bounds_test = impl_->depth_bounds_enabled;
            device_info_.supports_sample_locations_ext = has_dev_sample_locations;
            device_info_.supports_custom_border_color = impl_->custom_border_color_enabled;
            device_info_.supports_custom_border_color_without_format =
                impl_->custom_border_color_without_format_enabled;
            device_info_.max_custom_border_color_samplers = impl_->max_custom_border_color_samplers;
            device_info_.supports_multi_draw_indirect = impl_->multi_draw_indirect_enabled;
            device_info_.supports_draw_indirect_count = impl_->draw_indirect_count_enabled;
            device_info_.supports_dynamic_rendering = impl_->dynamic_rendering_enabled;
            device_info_.supported_depth_resolve_modes = impl_->supported_depth_resolve_modes;
            device_info_.supported_stencil_resolve_modes = impl_->supported_stencil_resolve_modes;
            device_info_.is_hardware =
                (device_info_.device_type == VulkanDeviceType::DiscreteGpu ||
                 device_info_.device_type == VulkanDeviceType::IntegratedGpu);
            device_info_.is_fallback = false;

            // Populate structured live physical-device capability probe & D3D12 OPTIONS2/7 mapping.
            feature_probe_ = VulkanFeatureProbe{};
            feature_probe_.api_version = best_props.apiVersion;
            feature_probe_.api_major = device_info_.api_major;
            feature_probe_.api_minor = device_info_.api_minor;
            feature_probe_.api_patch = device_info_.api_patch;
            feature_probe_.probed_from_icd = true;
            feature_probe_.depth_bounds = (supported_feats.fields[kVkFeatDepthBounds] != 0u);
            feature_probe_.texture_compression_bc = (supported_feats.fields[kVkFeatTextureCompressionBC] != 0u);
            feature_probe_.fragment_stores_and_atomics =
                (supported_feats.fields[kVkFeatFragmentStoresAndAtomics] != 0u);
            feature_probe_.shader_resource_residency =
                (supported_feats.fields[kVkFeatShaderResourceResidency] != 0u);
            feature_probe_.shader_resource_min_lod =
                (supported_feats.fields[kVkFeatShaderResourceMinLod] != 0u);
            feature_probe_.sparse_binding = (supported_feats.fields[kVkFeatSparseBinding] != 0u);
            feature_probe_.sparse_residency_image_2d =
                (supported_feats.fields[kVkFeatSparseResidencyImage2D] != 0u);
            feature_probe_.sparse_residency_image_3d =
                (supported_feats.fields[kVkFeatSparseResidencyImage3D] != 0u);
            feature_probe_.vulkan12_probed = probed_v12;
            if (probed_v12) {
                feature_probe_.descriptor_indexing =
                    (queried_v12_feats.fields[kVkFeat12DescriptorIndexing] != 0u);
                feature_probe_.sampler_filter_minmax =
                    (queried_v12_feats.fields[kVkFeat12SamplerFilterMinmax] != 0u);
                feature_probe_.timeline_semaphore =
                    (queried_v12_feats.fields[kVkFeat12TimelineSemaphore] != 0u);
                feature_probe_.buffer_device_address =
                    (queried_v12_feats.fields[kVkFeat12BufferDeviceAddress] != 0u);
            }
            feature_probe_.vulkan13_probed = probed_v13;
            if (probed_v13) {
                feature_probe_.synchronization2 =
                    (queried_v13_feats.fields[kVkFeat13Synchronization2] != 0u);
                feature_probe_.dynamic_rendering =
                    (queried_v13_feats.fields[kVkFeat13DynamicRendering] != 0u);
                feature_probe_.maintenance4 =
                    (queried_v13_feats.fields[kVkFeat13Maintenance4] != 0u);
            }
            feature_probe_.has_sample_locations_ext = has_dev_sample_locations;
            if (has_dev_sample_locations) {
                feature_probe_.sample_location_sample_counts = sample_loc_props.sampleLocationSampleCounts;
                feature_probe_.max_sample_location_grid_size_x = sample_loc_props.maxSampleLocationGridSizeWidth;
                feature_probe_.max_sample_location_grid_size_y = sample_loc_props.maxSampleLocationGridSizeHeight;
                feature_probe_.sample_location_subpixel_bits = sample_loc_props.sampleLocationSubPixelBits;
                feature_probe_.variable_sample_locations = (sample_loc_props.variableSampleLocations != 0u);
            }
            feature_probe_.has_mesh_shader_ext = has_dev_mesh_shader;
            feature_probe_.mesh_shader = (queried_mesh_feats.meshShader != 0u);
            feature_probe_.task_shader = (queried_mesh_feats.taskShader != 0u);
            feature_probe_.mesh_shader_queries = (queried_mesh_feats.meshShaderQueries != 0u);
            feature_probe_.has_sampler_feedback_ext = has_dev_sampler_feedback;
            feature_probe_.sampler_feedback_min_mip = has_dev_sampler_feedback;
            feature_probe_.sampler_feedback_mip_region_used = has_dev_sampler_feedback;
            feature_probe_.has_custom_border_color_ext = has_dev_custom_border_color;
            feature_probe_.custom_border_colors = (queried_cbc_feats.customBorderColors != 0u);
            feature_probe_.custom_border_color_without_format =
                (queried_cbc_feats.customBorderColorWithoutFormat != 0u);
            feature_probe_.max_custom_border_color_samplers = cbc_props.maxCustomBorderColorSamplers;

            d3d12_options2_ = MapVulkanToD3D12Options2(feature_probe_);
            d3d12_options7_ = MapVulkanToD3D12Options7(feature_probe_);

            impl_->max_push_constants_size = queried_max_pc;
            impl_->subgroup_size = queried_subgroup_size;
            impl_->bc_compression_enabled = want_bc_compression;
            impl_->initialized = true;
            impl_->fallback_mode = false;

            std::cerr << "[OpenScarlett][vulkan] ICD Device: \"" << device_info_.device_name
                      << "\" API=" << device_info_.api_major << "." << device_info_.api_minor
                      << "." << device_info_.api_patch
                      << " vendor=0x" << std::hex << device_info_.vendor_id
                      << " device=0x" << device_info_.device_id << std::dec
                      << " compute_qf=" << device_info_.compute_queue_family
                      << " gfx_qf=" << (device_info_.supports_graphics ? std::to_string(device_info_.graphics_queue_family) : "none")
                      << " subgroup=" << device_info_.subgroup_size
                      << " max_pc=" << device_info_.max_push_constants_size
                      << " bc=" << (device_info_.supports_bc_texture_compression ? "yes" : "no")
                      << " depth_bounds=" << (device_info_.supports_depth_bounds_test ? "yes" : "no")
                      << " sample_pos_tier=" << static_cast<uint32_t>(d3d12_options2_.programmable_sample_positions_tier)
                      << " custom_border=" << (device_info_.supports_custom_border_color ? "yes" : "no")
                      << " mesh=" << (device_info_.supports_mesh_shader ? (device_info_.supports_task_shader ? "task+mesh" : "mesh") : "no")
                      << " swapchain=" << (device_info_.supports_swapchain ? "yes" : "no")
                      << " hdr10_cs=" << (device_info_.supports_hdr10_colorspace ? "yes" : "no") << "\n";
            return true;
        };

        if (try_init_vulkan()) {
            return Status::Ok();
        }
        impl_->Cleanup();
        device_info_ = VulkanDeviceInfo{};
        feature_probe_ = VulkanFeatureProbe{};
        d3d12_options2_ = VulkanD3D12Options2{};
        d3d12_options7_ = VulkanD3D12Options7{};
    }
#else
    failure_reason = "Dynamic vulkan-1.dll loader requires Windows";
#endif

    if (!allow_software_fallback) {
        device_info_.selected_device_filter = device_filter;
        device_info_.matched_device_filter = false;
        return Status::Error(StatusCode::DeviceError, failure_reason);
    }
    std::cerr << "[OpenScarlett][vulkan] Falling back to software SPIR-V backend (" << failure_reason << ")\n";
    Status fb_st = InitializeSoftwareFallback();
    device_info_.selected_device_filter = device_filter;
    device_info_.matched_device_filter = false;
    return fb_st;
}

void VulkanBackend::SetPushConstantBudgetOverrideForTest(uint32_t max_bytes) noexcept {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->push_constant_budget_override = max_bytes;
}

uint32_t VulkanBackend::GetEffectivePushConstantBudget() const noexcept {
    if (!impl_) return 128u;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const uint32_t dev_limit = (impl_->max_push_constants_size > 0u) ? impl_->max_push_constants_size : 128u;
    if (impl_->push_constant_budget_override > 0u) {
        return std::min(impl_->push_constant_budget_override, dev_limit);
    }
    return dev_limit;
}

void VulkanBackend::SetSimulatedSubgroupSizeForTest(uint32_t subgroup_size) noexcept {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->simulated_subgroup_size = subgroup_size;
}

uint32_t VulkanBackend::GetEffectiveSubgroupSize() const noexcept {
    if (!impl_) return 32u;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->simulated_subgroup_size > 0u) {
        return impl_->simulated_subgroup_size;
    }
    return (impl_->subgroup_size > 0u) ? impl_->subgroup_size : 32u;
}

void VulkanBackend::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
    device_info_ = VulkanDeviceInfo{};
}

Status VulkanBackend::ExecuteComputeShader(const std::vector<uint32_t>& spirv,
                                           const std::vector<uint32_t>& input_u32,
                                           std::vector<uint32_t>& output_u32,
                                           uint32_t group_x) {
    VulkanComputeDispatchOptions opts{};
    VulkanComputeDispatchResult res{};
    return ExecuteComputeShaderEx(spirv, input_u32, output_u32, group_x, opts, res);
}

Status VulkanBackend::ExecuteComputeShaderEx(const std::vector<uint32_t>& spirv,
                                             const std::vector<uint32_t>& input_u32,
                                             std::vector<uint32_t>& output_u32,
                                             uint32_t group_x,
                                             const VulkanComputeDispatchOptions& options,
                                             VulkanComputeDispatchResult& out_result) {
    out_result = VulkanComputeDispatchResult{};
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (group_x == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Compute dispatch group_x must be > 0");
    }
    if (input_u32.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Compute input buffer must not be empty");
    }

    SpirvValidationInfo spv_info{};
    Status val_st = ValidateSpirvBinary(spirv, &spv_info);
    if (!val_st.ok()) {
        return val_st;
    }
    if (!spv_info.has_compute_entry || spv_info.entry_point_name.empty()) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V module has no GLCompute entry point");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    const uint32_t active_subgroup =
        (options.simulated_device_subgroup_size > 0u)
            ? options.simulated_device_subgroup_size
            : ((impl_->simulated_subgroup_size > 0u)
                   ? impl_->simulated_subgroup_size
                   : ((impl_->subgroup_size > 0u) ? impl_->subgroup_size : 32u));
    const uint32_t req_wave = (options.requested_wave_size > 0u) ? options.requested_wave_size : 32u;
    const bool wave32_on_wave64 = (req_wave == 32u && active_subgroup >= 64u);

    const uint32_t element_count =
        std::max<uint32_t>(static_cast<uint32_t>(input_u32.size()), group_x * spv_info.local_size_x);
    uint32_t padded_elements = element_count;
    if (wave32_on_wave64 && options.wave32_on_wave64_safe) {
        const uint32_t rem = padded_elements % active_subgroup;
        if (rem != 0u) {
            padded_elements += (active_subgroup - rem);
        }
        padded_elements = std::max(padded_elements, active_subgroup);
    }

    out_result.device_subgroup_size = active_subgroup;
    out_result.effective_wave_size = req_wave;
    out_result.wave32_on_wave64_adapted = (wave32_on_wave64 && options.wave32_on_wave64_safe);
    out_result.padded_buffer_elements = padded_elements;

    output_u32.assign(element_count, 0u);

    if (impl_->fallback_mode || !impl_->device) {
        out_result.executed_on_vulkan_icd = false;
        return ExecuteSpirvComputeSoftware(spirv, spv_info, input_u32, output_u32, group_x);
    }

    const VkDeviceSize byte_size = static_cast<VkDeviceSize>(padded_elements) * sizeof(uint32_t);
    const VkDeviceSize logical_bytes = static_cast<VkDeviceSize>(element_count) * sizeof(uint32_t);
    VkBuffer in_buf = kVkNullHandle, out_buf = kVkNullHandle;
    VkDeviceMemory in_mem = kVkNullHandle, out_mem = kVkNullHandle;
    VkShaderModule shader_mod = kVkNullHandle;
    VkDescriptorSetLayout ds_layout = kVkNullHandle;
    VkPipelineLayout pipe_layout = kVkNullHandle;
    VkPipeline pipeline = kVkNullHandle;
    VkDescriptorPool desc_pool = kVkNullHandle;
    VkCommandBuffer cmd_buf = nullptr;

    auto cleanup = [&]() {
        if (cmd_buf) { impl_->vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd_buf); cmd_buf = nullptr; }
        if (desc_pool != kVkNullHandle) impl_->vkDestroyDescriptorPool(impl_->device, desc_pool, nullptr);
        if (pipeline != kVkNullHandle) impl_->vkDestroyPipeline(impl_->device, pipeline, nullptr);
        if (pipe_layout != kVkNullHandle) impl_->vkDestroyPipelineLayout(impl_->device, pipe_layout, nullptr);
        if (ds_layout != kVkNullHandle) impl_->vkDestroyDescriptorSetLayout(impl_->device, ds_layout, nullptr);
        if (shader_mod != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, shader_mod, nullptr);
        if (out_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, out_buf, nullptr);
        if (in_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, in_buf, nullptr);
        if (out_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, out_mem, nullptr);
        if (in_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, in_mem, nullptr);
    };

    bool in_coherent = true, out_coherent = true;
    Status st = impl_->CreateHostBuffer(byte_size, kVkBufferUsageStorageBufferBit, in_buf, in_mem, in_coherent,
                                        input_u32.data(), input_u32.size() * sizeof(uint32_t));
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(byte_size, kVkBufferUsageStorageBufferBit, out_buf, out_mem, out_coherent,
                                 output_u32.data(), output_u32.size() * sizeof(uint32_t));
    if (!st.ok()) { cleanup(); return st; }

    VkShaderModuleCreateInfo sm_ci{};
    sm_ci.sType = kVkStypeShaderModuleCreateInfo;
    sm_ci.codeSize = spirv.size() * sizeof(uint32_t);
    sm_ci.pCode = spirv.data();
    if (impl_->vkCreateShaderModule(impl_->device, &sm_ci, nullptr, &shader_mod) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule failed");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0u, kVkDescriptorTypeStorageBuffer, 1u, kVkShaderStageComputeBit, nullptr};
    bindings[1] = {1u, kVkDescriptorTypeStorageBuffer, 1u, kVkShaderStageComputeBit, nullptr};

    VkDescriptorSetLayoutCreateInfo dsl_ci{};
    dsl_ci.sType = kVkStypeDescriptorSetLayoutCreateInfo;
    dsl_ci.bindingCount = 2;
    dsl_ci.pBindings = bindings;
    if (impl_->vkCreateDescriptorSetLayout(impl_->device, &dsl_ci, nullptr, &ds_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorSetLayout failed");
    }

    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &ds_layout;
    if (impl_->vkCreatePipelineLayout(impl_->device, &pl_ci, nullptr, &pipe_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout failed");
    }

    VkComputePipelineCreateInfo cp_ci{};
    cp_ci.sType = kVkStypeComputePipelineCreateInfo;
    cp_ci.stage.sType = kVkStypePipelineShaderStageCreateInfo;
    cp_ci.stage.stage = kVkShaderStageComputeBit;
    cp_ci.stage.module = shader_mod;
    cp_ci.stage.pName = spv_info.entry_point_name.c_str();
    cp_ci.layout = pipe_layout;
    cp_ci.basePipelineIndex = -1;
    if (impl_->vkCreateComputePipelines(impl_->device, kVkNullHandle, 1, &cp_ci, nullptr, &pipeline) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateComputePipelines failed");
    }

    VkDescriptorPoolSize pool_size{kVkDescriptorTypeStorageBuffer, 2};
    VkDescriptorPoolCreateInfo dp_ci{};
    dp_ci.sType = kVkStypeDescriptorPoolCreateInfo;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    if (impl_->vkCreateDescriptorPool(impl_->device, &dp_ci, nullptr, &desc_pool) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorPool failed");
    }

    VkDescriptorSet desc_set = kVkNullHandle;
    VkDescriptorSetAllocateInfo dsa_ci{};
    dsa_ci.sType = kVkStypeDescriptorSetAllocateInfo;
    dsa_ci.descriptorPool = desc_pool;
    dsa_ci.descriptorSetCount = 1;
    dsa_ci.pSetLayouts = &ds_layout;
    if (impl_->vkAllocateDescriptorSets(impl_->device, &dsa_ci, &desc_set) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateDescriptorSets failed");
    }

    VkDescriptorBufferInfo in_binfo{in_buf, 0, byte_size};
    VkDescriptorBufferInfo out_binfo{out_buf, 0, byte_size};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = kVkStypeWriteDescriptorSet;
    writes[0].dstSet = desc_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[0].pBufferInfo = &in_binfo;
    writes[1].sType = kVkStypeWriteDescriptorSet;
    writes[1].dstSet = desc_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[1].pBufferInfo = &out_binfo;
    impl_->vkUpdateDescriptorSets(impl_->device, 2, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cba_ci{};
    cba_ci.sType = kVkStypeCommandBufferAllocateInfo;
    cba_ci.commandPool = impl_->command_pool;
    cba_ci.level = kVkCommandBufferLevelPrimary;
    cba_ci.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba_ci, &cmd_buf) != kVkSuccess || !cmd_buf) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers failed");
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = kVkStypeCommandBufferBeginInfo;
    begin_info.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd_buf, &begin_info) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer failed");
    }

    impl_->vkCmdBindPipeline(cmd_buf, kVkPipelineBindPointCompute, pipeline);
    impl_->vkCmdBindDescriptorSets(cmd_buf, kVkPipelineBindPointCompute, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    impl_->vkCmdDispatch(cmd_buf, group_x, 1, 1);

    VkBufferMemoryBarrier out_barrier{};
    out_barrier.sType = kVkStypeBufferMemoryBarrier;
    out_barrier.srcAccessMask = kVkAccessShaderWriteBit;
    out_barrier.dstAccessMask = kVkAccessHostReadBit;
    out_barrier.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.buffer = out_buf;
    out_barrier.offset = 0;
    out_barrier.size = byte_size;
    impl_->vkCmdPipelineBarrier(cmd_buf, kVkPipelineStageComputeShaderBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &out_barrier, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd_buf) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer failed");
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = kVkStypeSubmitInfo;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf;
    if (impl_->vkQueueSubmit(impl_->compute_queue, 1, &submit_info, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->compute_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueSubmit/vkQueueWaitIdle failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, out_mem, 0, byte_size, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(output readback) failed");
    }
    if (!out_coherent) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, out_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }
    std::memcpy(output_u32.data(), mapped, static_cast<size_t>(logical_bytes));
    impl_->vkUnmapMemory(impl_->device, out_mem);

    cleanup();
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

Status VulkanBackend::RecordCommandList(const VulkanCommandListDesc& desc,
                                        VulkanCommandListResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    out_result = VulkanCommandListResult{};

    bool has_global_uav_barrier = false;
    for (const auto& cmd : desc.commands) {
        if (cmd.op == VulkanCommandOp::Dispatch && (cmd.x == 0 || cmd.y == 0 || cmd.z == 0)) {
            return Status::Error(StatusCode::InvalidArgument, "Dispatch command dimensions must be > 0");
        }
        if (cmd.op == VulkanCommandOp::GlobalUavBarrier) {
            has_global_uav_barrier = true;
        }
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);

    const uint32_t global_uav_access = kVkAccessMemoryReadBit | kVkAccessMemoryWriteBit;
    const bool use_graphics_queue_for_global_uav =
        has_global_uav_barrier && impl_->has_graphics_queue &&
        impl_->graphics_queue != nullptr && impl_->graphics_command_pool != kVkNullHandle;
    const uint32_t global_uav_stage =
        (impl_->has_graphics_queue || impl_->fallback_mode)
            ? (kVkPipelineStageAllGraphicsBit | kVkPipelineStageComputeShaderBit)
            : kVkPipelineStageAllCommandsBit;

    if (impl_->fallback_mode || !impl_->device) {
        for (const auto& cmd : desc.commands) {
            ++out_result.recorded_commands;
            if (cmd.op == VulkanCommandOp::Dispatch) {
                ++out_result.dispatch_count;
            } else if (cmd.op == VulkanCommandOp::PipelineBarrier) {
                ++out_result.barrier_count;
            } else if (cmd.op == VulkanCommandOp::GlobalUavBarrier) {
                ++out_result.barrier_count;
                ++out_result.global_uav_barrier_count;
                out_result.last_global_uav_src_stage_mask = global_uav_stage;
                out_result.last_global_uav_dst_stage_mask = global_uav_stage;
                out_result.last_global_uav_src_access_mask = global_uav_access;
                out_result.last_global_uav_dst_access_mask = global_uav_access;
            }
        }
        out_result.executed_on_vulkan_icd = false;
        return Status::Ok();
    }

    VkCommandPool target_pool =
        use_graphics_queue_for_global_uav ? impl_->graphics_command_pool : impl_->command_pool;
    VkQueue target_queue =
        use_graphics_queue_for_global_uav ? impl_->graphics_queue : impl_->compute_queue;

    VkCommandBuffer cmd_buf = nullptr;
    VkCommandBufferAllocateInfo cba_ci{};
    cba_ci.sType = kVkStypeCommandBufferAllocateInfo;
    cba_ci.commandPool = target_pool;
    cba_ci.level = kVkCommandBufferLevelPrimary;
    cba_ci.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba_ci, &cmd_buf) != kVkSuccess || !cmd_buf) {
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkAllocateCommandBuffers failed");
    }

    auto free_cb = [&]() {
        if (cmd_buf) {
            impl_->vkFreeCommandBuffers(impl_->device, target_pool, 1, &cmd_buf);
            cmd_buf = nullptr;
        }
    };

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = kVkStypeCommandBufferBeginInfo;
    begin_info.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd_buf, &begin_info) != kVkSuccess) {
        free_cb();
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkBeginCommandBuffer failed");
    }

    for (const auto& cmd : desc.commands) {
        ++out_result.recorded_commands;
        if (cmd.op == VulkanCommandOp::PipelineBarrier) {
            VkMemoryBarrier mb{};
            mb.sType = kVkStypeMemoryBarrier;
            mb.srcAccessMask = kVkAccessShaderWriteBit;
            mb.dstAccessMask = kVkAccessHostReadBit;
            impl_->vkCmdPipelineBarrier(cmd_buf, kVkPipelineStageComputeShaderBit, kVkPipelineStageHostBit,
                                        0, 1, &mb, 0, nullptr, 0, nullptr);
            ++out_result.barrier_count;
        } else if (cmd.op == VulkanCommandOp::GlobalUavBarrier) {
            // Console UAV barrier with UAV.pResource == nullptr (xcl #68):
            // conservatively synchronize all memory reads/writes across graphics & compute stages.
            VkMemoryBarrier mb{};
            mb.sType = kVkStypeMemoryBarrier;
            mb.srcAccessMask = global_uav_access;
            mb.dstAccessMask = global_uav_access;
            impl_->vkCmdPipelineBarrier(cmd_buf, global_uav_stage, global_uav_stage,
                                        0, 1, &mb, 0, nullptr, 0, nullptr);
            ++out_result.barrier_count;
            ++out_result.global_uav_barrier_count;
            out_result.last_global_uav_src_stage_mask = global_uav_stage;
            out_result.last_global_uav_dst_stage_mask = global_uav_stage;
            out_result.last_global_uav_src_access_mask = global_uav_access;
            out_result.last_global_uav_dst_access_mask = global_uav_access;
        } else if (cmd.op == VulkanCommandOp::Dispatch) {
            ++out_result.dispatch_count;
        }
    }

    if (impl_->vkEndCommandBuffer(cmd_buf) != kVkSuccess) {
        free_cb();
        return Status::Error(StatusCode::DeviceError, "RecordCommandList: vkEndCommandBuffer failed");
    }

    if (desc.submit_empty_fence_sync) {
        VkSubmitInfo si{};
        si.sType = kVkStypeSubmitInfo;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_buf;
        if (impl_->vkQueueSubmit(target_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
            impl_->vkQueueWaitIdle(target_queue) != kVkSuccess) {
            free_cb();
            return Status::Error(StatusCode::DeviceError, "RecordCommandList: queue submit/wait failed");
        }
    }

    free_cb();
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

Status VulkanBackend::ExecuteGraphicsDraw(const VulkanGraphicsDrawDesc& desc,
                                          VulkanGraphicsDrawResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0 || desc.height == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Render target width and height must be > 0");
    }
    if (desc.color_format == VulkanImageFormat::D32Sfloat) {
        return Status::Error(StatusCode::InvalidArgument, "D32Sfloat cannot be used as color format");
    }

    SpirvValidationInfo vs_info{};
    SpirvValidationInfo ps_info{};
    if (auto st = ValidateSpirvBinary(desc.vertex_spirv, &vs_info); !st.ok()) return st;
    if (auto st = ValidateSpirvBinary(desc.fragment_spirv, &ps_info); !st.ok()) return st;
    if (!vs_info.has_vertex_entry) {
        return Status::Error(StatusCode::ShaderError, "Vertex SPIR-V module missing Vertex entry point");
    }
    if (!ps_info.has_fragment_entry) {
        return Status::Error(StatusCode::ShaderError, "Fragment SPIR-V module missing Fragment entry point");
    }

    out_result = VulkanGraphicsDrawResult{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // 1. Separate colliding buffer & image/sampler descriptor slots on mixed heaps.
    std::vector<uint32_t> patched_vs = desc.vertex_spirv;
    std::vector<uint32_t> patched_ps = desc.fragment_spirv;
    std::vector<VulkanDescriptorBinding> resolved_descs;
    std::vector<VulkanDescriptorSlotRemap> remaps;
    ResolveMixedHeapDescriptorCollisions(
        desc.descriptors, resolved_descs, remaps, nullptr, &patched_vs, &patched_ps);
    out_result.descriptor_collisions_resolved = static_cast<uint32_t>(remaps.size());
    out_result.descriptor_slot_remaps = remaps;

    // 2. Enforce device push-constant byte-budget guard.
    const uint32_t dev_pc_limit = (impl_->max_push_constants_size > 0u) ? impl_->max_push_constants_size : 128u;
    uint32_t effective_budget = dev_pc_limit;
    if (impl_->push_constant_budget_override > 0u) {
        effective_budget = std::min(effective_budget, impl_->push_constant_budget_override);
    }
    if (desc.push_constant_budget_override > 0u) {
        effective_budget = std::min(effective_budget, desc.push_constant_budget_override);
    }
    out_result.device_push_constant_limit = effective_budget;

    uint32_t effective_pc_bytes = static_cast<uint32_t>(desc.push_constants.size());
    bool pc_exceeded = false;
    if (effective_pc_bytes > effective_budget) {
        pc_exceeded = true;
        effective_pc_bytes = effective_budget & ~3u;
        std::cerr << "[OpenScarlett][vulkan] Push constants (" << desc.push_constants.size()
                  << " bytes) exceed active device budget (" << effective_budget
                  << " bytes); clamping to " << effective_pc_bytes << " bytes\n";
    }
    out_result.push_constants_exceeded_budget = pc_exceeded;
    out_result.effective_push_constant_bytes = effective_pc_bytes;

    auto run_sw = [&]() -> Status {
        VulkanGraphicsDrawDesc sw_desc = desc;
        sw_desc.vertex_spirv = patched_vs;
        sw_desc.fragment_spirv = patched_ps;
        sw_desc.descriptors = resolved_descs;
        if (sw_desc.push_constants.size() > effective_pc_bytes) {
            sw_desc.push_constants.resize(effective_pc_bytes);
        }
        Status sw_st = ExecuteSpirvGraphicsSoftware(sw_desc, out_result);
        out_result.push_constants_exceeded_budget = pc_exceeded;
        out_result.effective_push_constant_bytes = effective_pc_bytes;
        out_result.device_push_constant_limit = effective_budget;
        out_result.descriptor_collisions_resolved = static_cast<uint32_t>(remaps.size());
        out_result.descriptor_slot_remaps = remaps;
        for (const auto& d : resolved_descs) {
            if (d.kind == VulkanDescriptorKind::CombinedImageSampler ||
                d.kind == VulkanDescriptorKind::Sampler) {
                const bool is_custom =
                    (d.border_color_mode == VulkanBorderColorMode::FloatCustom ||
                     d.border_color_mode == VulkanBorderColorMode::IntCustom);
                if (is_custom) {
                    if (d.force_standard_border_fallback) {
                        ++out_result.custom_border_fallback_samplers_used;
                    } else {
                        ++out_result.custom_border_samplers_used;
                    }
                }
            }
        }
        return sw_st;
    };

    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue) {
        return run_sw();
    }

    const uint32_t unused_groups[3] = {1, 1, 1};
    const std::vector<uint32_t> empty_task;
    Status st = impl_->ExecuteGraphicsOrMeshOnIcd(
        false, empty_task, "", patched_vs, desc.vertex_entry,
        patched_ps, desc.fragment_entry,
        desc.width, desc.height, desc.color_format,
        desc.enable_depth, desc.enable_msaa_4x, desc.depth_write,
        desc.clear_color, desc.clear_depth,
        desc.vertex_bytes, desc.vertex_stride, desc.vertex_attributes,
        desc.indices, desc.vertex_count, unused_groups,
        desc.push_constants, effective_pc_bytes, resolved_descs,
        out_result.framebuffer, out_result.raw_fp16_rgba,
        &out_result.custom_border_samplers_used,
        &out_result.custom_border_fallback_samplers_used);
    if (!st.ok()) {
        if (pc_exceeded) {
            // Driver rejected clamped push-constant range smaller than shader block: gracefully fall back.
            return run_sw();
        }
        return st;
    }

    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.executed_on_vulkan_icd = true;
    out_result.used_msaa_4x = desc.enable_msaa_4x;
    out_result.used_depth_buffer = desc.enable_depth;
    return Status::Ok();
}

Status VulkanBackend::ExecuteMeshShaderDraw(const VulkanMeshDrawDesc& desc,
                                            VulkanMeshDrawResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0 || desc.height == 0 ||
        desc.group_count_x == 0 || desc.group_count_y == 0 || desc.group_count_z == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid mesh draw dimensions or group counts");
    }

    const bool has_task = !desc.task_spirv.empty();
    SpirvValidationInfo task_info{}, mesh_info{}, frag_info{};
    if (has_task) {
        if (auto st = ValidateSpirvBinary(desc.task_spirv, &task_info); !st.ok()) return st;
        if (!task_info.has_task_entry) {
            return Status::Error(StatusCode::ShaderError, "Task SPIR-V module missing TaskEXT entry point");
        }
    }
    if (auto st = ValidateSpirvBinary(desc.mesh_spirv, &mesh_info); !st.ok()) return st;
    if (!mesh_info.has_mesh_entry) {
        return Status::Error(StatusCode::ShaderError, "Mesh SPIR-V module missing MeshEXT entry point");
    }
    if (auto st = ValidateSpirvBinary(desc.fragment_spirv, &frag_info); !st.ok()) return st;
    if (!frag_info.has_fragment_entry) {
        return Status::Error(StatusCode::ShaderError, "Fragment SPIR-V module missing Fragment entry point");
    }

    out_result = VulkanMeshDrawResult{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // 1. Separate colliding buffer & image/sampler descriptor slots on mixed heaps.
    std::vector<uint32_t> patched_task = desc.task_spirv;
    std::vector<uint32_t> patched_mesh = desc.mesh_spirv;
    std::vector<uint32_t> patched_frag = desc.fragment_spirv;
    std::vector<VulkanDescriptorBinding> resolved_descs;
    std::vector<VulkanDescriptorSlotRemap> remaps;
    ResolveMixedHeapDescriptorCollisions(
        desc.descriptors, resolved_descs, remaps,
        has_task ? &patched_task : nullptr, &patched_mesh, &patched_frag);
    out_result.descriptor_collisions_resolved = static_cast<uint32_t>(remaps.size());
    out_result.descriptor_slot_remaps = remaps;

    // 2. Enforce device push-constant byte-budget guard.
    const uint32_t dev_pc_limit = (impl_->max_push_constants_size > 0u) ? impl_->max_push_constants_size : 128u;
    uint32_t effective_budget = dev_pc_limit;
    if (impl_->push_constant_budget_override > 0u) {
        effective_budget = std::min(effective_budget, impl_->push_constant_budget_override);
    }
    if (desc.push_constant_budget_override > 0u) {
        effective_budget = std::min(effective_budget, desc.push_constant_budget_override);
    }
    out_result.device_push_constant_limit = effective_budget;

    uint32_t effective_pc_bytes = static_cast<uint32_t>(desc.push_constants.size());
    bool pc_exceeded = false;
    if (effective_pc_bytes > effective_budget) {
        pc_exceeded = true;
        effective_pc_bytes = effective_budget & ~3u;
        std::cerr << "[OpenScarlett][vulkan] Mesh push constants (" << desc.push_constants.size()
                  << " bytes) exceed active device budget (" << effective_budget
                  << " bytes); clamping to " << effective_pc_bytes << " bytes\n";
    }
    out_result.push_constants_exceeded_budget = pc_exceeded;
    out_result.effective_push_constant_bytes = effective_pc_bytes;

    auto run_sw_mesh = [&]() -> Status {
        VulkanMeshDrawDesc sw_desc = desc;
        sw_desc.task_spirv = patched_task;
        sw_desc.mesh_spirv = patched_mesh;
        sw_desc.fragment_spirv = patched_frag;
        sw_desc.descriptors = resolved_descs;
        if (sw_desc.push_constants.size() > effective_pc_bytes) {
            sw_desc.push_constants.resize(effective_pc_bytes);
        }
        Status sw_st = ExecuteSpirvMeshSoftware(sw_desc, out_result);
        out_result.push_constants_exceeded_budget = pc_exceeded;
        out_result.effective_push_constant_bytes = effective_pc_bytes;
        out_result.device_push_constant_limit = effective_budget;
        out_result.descriptor_collisions_resolved = static_cast<uint32_t>(remaps.size());
        out_result.descriptor_slot_remaps = remaps;
        return sw_st;
    };

    const bool can_run_hw = !impl_->fallback_mode && impl_->device && impl_->has_graphics_queue &&
                            impl_->mesh_shader_enabled && impl_->vkCmdDrawMeshTasksEXT != nullptr &&
                            (!has_task || impl_->task_shader_enabled);

    if (!can_run_hw) {
        if (!desc.allow_fallback_emulation) {
            return Status::Error(StatusCode::UnsupportedFormat, "VK_EXT_mesh_shader not supported on this device");
        }
        return run_sw_mesh();
    }

    const uint32_t groups[3] = {desc.group_count_x, desc.group_count_y, desc.group_count_z};
    const std::vector<uint8_t> no_vb;
    const std::vector<VulkanVertexAttribute> no_attrs;
    const std::vector<uint32_t> no_ib;

    Status st = impl_->ExecuteGraphicsOrMeshOnIcd(
        true, patched_task, desc.task_entry, patched_mesh, desc.mesh_entry,
        patched_frag, desc.fragment_entry,
        desc.width, desc.height, desc.color_format,
        desc.enable_depth, false, true,
        desc.clear_color, desc.clear_depth,
        no_vb, 0, no_attrs, no_ib, 0, groups,
        desc.push_constants, effective_pc_bytes, resolved_descs,
        out_result.framebuffer, out_result.raw_fp16_rgba,
        &out_result.custom_border_samplers_used,
        &out_result.custom_border_fallback_samplers_used);
    if (!st.ok()) {
        std::cerr << "[OpenScarlett][vulkan] ExecuteMeshShaderDraw ICD error: " << st.message << "\n";
        if (pc_exceeded && desc.allow_fallback_emulation) {
            return run_sw_mesh();
        }
        return st;
    }

    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.executed_on_mesh_shader_hw = true;
    out_result.used_task_shader = has_task;
    out_result.used_fallback = false;
    return Status::Ok();
}

Status VulkanBackend::UploadAndReadbackTexture(const VulkanTextureDesc& desc,
                                               VulkanTextureRoundtripResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0 || desc.height == 0 || (desc.is_3d && desc.depth == 0) || desc.mips.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid texture dimensions or empty mip list");
    }

    const bool is_bc = IsBlockCompressedFormat(desc.format);
    std::vector<VkDeviceSize> mip_offsets(desc.mips.size(), 0);
    VkDeviceSize total_bytes = 0;
    for (size_t m = 0; m < desc.mips.size(); ++m) {
        const auto& mip = desc.mips[m];
        const uint32_t d = desc.is_3d ? std::max<uint32_t>(1, mip.depth) : 1u;
        const size_t expected = ComputeMipByteSize(desc.format, mip.width, mip.height, d);
        if (mip.width == 0 || mip.height == 0 || mip.data.size() != expected) {
            return Status::Error(StatusCode::InvalidArgument, "Texture mip data size does not match dimensions");
        }
        while (total_bytes % 16 != 0) ++total_bytes;
        mip_offsets[m] = total_bytes;
        total_bytes += expected;
    }

    out_result = VulkanTextureRoundtripResult{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue ||
        (is_bc && !impl_->bc_compression_enabled)) {
        out_result.readback_mips = desc.mips;
        out_result.executed_on_vulkan_icd = false;
        return Status::Ok();
    }

    VkImage img = kVkNullHandle;
    VkDeviceMemory img_mem = kVkNullHandle;
    VkBuffer up_buf = kVkNullHandle, rb_buf = kVkNullHandle;
    VkDeviceMemory up_mem = kVkNullHandle, rb_mem = kVkNullHandle;
    bool up_coh = true, rb_coh = true;
    VkCommandBuffer cmd = nullptr;

    auto cleanup = [&]() {
        if (cmd) { impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd); cmd = nullptr; }
        if (rb_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, rb_buf, nullptr);
        if (up_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, up_buf, nullptr);
        if (rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, rb_mem, nullptr);
        if (up_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, up_mem, nullptr);
        if (img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, img, nullptr);
        if (img_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, img_mem, nullptr);
    };

    const uint32_t mip_cnt = static_cast<uint32_t>(desc.mips.size());
    Status st = impl_->CreateDeviceImage(
        desc.is_3d ? kVkImageType3D : kVkImageType2D,
        static_cast<int32_t>(desc.format),
        desc.width, desc.height, desc.is_3d ? desc.depth : 1u,
        mip_cnt, kVkSampleCount1Bit,
        kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit | kVkImageUsageSampledBit,
        img, img_mem);
    if (!st.ok()) { cleanup(); return st; }

    std::vector<uint8_t> staging_data(static_cast<size_t>(total_bytes), 0u);
    for (size_t m = 0; m < desc.mips.size(); ++m) {
        std::memcpy(staging_data.data() + mip_offsets[m], desc.mips[m].data.data(), desc.mips[m].data.size());
    }

    st = impl_->CreateHostBuffer(total_bytes, kVkBufferUsageTransferSrcBit, up_buf, up_mem, up_coh,
                                 staging_data.data(), staging_data.size());
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(total_bytes, kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coh);
    if (!st.ok()) { cleanup(); return st; }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(tex) failed");
    }

    VkCommandBufferBeginInfo cbi{};
    cbi.sType = kVkStypeCommandBufferBeginInfo;
    cbi.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(tex) failed");
    }

    impl_->CmdImageBarrier(cmd, img, kVkImageAspectColorBit,
                           kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                           0u, kVkAccessTransferWriteBit, mip_cnt);

    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {desc.mips[m].width, desc.mips[m].height, desc.is_3d ? desc.mips[m].depth : 1u};
        impl_->vkCmdCopyBufferToImage(cmd, up_buf, img, kVkImageLayoutTransferDstOptimal, 1, &r);
    }

    // Exercise SHADER_READ_ONLY_OPTIMAL transition before readback copy
    impl_->CmdImageBarrier(cmd, img, kVkImageAspectColorBit,
                           kVkImageLayoutTransferDstOptimal, kVkImageLayoutShaderReadOnlyOptimal,
                           kVkPipelineStageTransferBit, kVkPipelineStageFragmentShaderBit,
                           kVkAccessTransferWriteBit, kVkAccessShaderReadBit, mip_cnt);
    impl_->CmdImageBarrier(cmd, img, kVkImageAspectColorBit,
                           kVkImageLayoutShaderReadOnlyOptimal, kVkImageLayoutTransferSrcOptimal,
                           kVkPipelineStageFragmentShaderBit, kVkPipelineStageTransferBit,
                           kVkAccessShaderReadBit, kVkAccessTransferReadBit, mip_cnt);

    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {desc.mips[m].width, desc.mips[m].height, desc.is_3d ? desc.mips[m].depth : 1u};
        impl_->vkCmdCopyImageToBuffer(cmd, img, kVkImageLayoutTransferSrcOptimal, rb_buf, 1, &r);
    }

    VkBufferMemoryBarrier bb{};
    bb.sType = kVkStypeBufferMemoryBarrier;
    bb.srcAccessMask = kVkAccessTransferWriteBit;
    bb.dstAccessMask = kVkAccessHostReadBit;
    bb.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    bb.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    bb.buffer = rb_buf;
    bb.offset = 0;
    bb.size = total_bytes;
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &bb, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(tex) failed");
    }

    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->graphics_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "Texture roundtrip submit/wait failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, rb_mem, 0, total_bytes, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(tex readback) failed");
    }
    if (!rb_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }

    out_result.readback_mips.resize(desc.mips.size());
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (size_t m = 0; m < desc.mips.size(); ++m) {
        out_result.readback_mips[m].width = desc.mips[m].width;
        out_result.readback_mips[m].height = desc.mips[m].height;
        out_result.readback_mips[m].depth = desc.is_3d ? desc.mips[m].depth : 1u;
        out_result.readback_mips[m].data.assign(
            src_bytes + mip_offsets[m], src_bytes + mip_offsets[m] + desc.mips[m].data.size());
    }
    impl_->vkUnmapMemory(impl_->device, rb_mem);
    cleanup();
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

Status VulkanBackend::CopyCompressedTextureStaged(const VulkanCompressedTextureCopyDesc& desc,
                                                  VulkanCompressedTextureCopyResult& out_result) {
    out_result = VulkanCompressedTextureCopyResult{};
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (!IsBlockCompressedFormat(desc.format)) {
        return Status::Error(StatusCode::InvalidArgument, "CopyCompressedTextureStaged requires a BC1-BC7 format");
    }
    if (desc.width == 0 || desc.height == 0 || desc.mips.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid BC texture dimensions or empty mip list");
    }

    const uint32_t block_bytes = BlockBytesForFormat(desc.format);
    std::vector<VulkanTextureMipLevel> packed_mips(desc.mips.size());
    std::vector<VkDeviceSize> mip_offsets(desc.mips.size(), 0);
    VkDeviceSize total_bytes = 0;

    for (size_t m = 0; m < desc.mips.size(); ++m) {
        const auto& src_mip = desc.mips[m];
        if (src_mip.width == 0 || src_mip.height == 0) {
            return Status::Error(StatusCode::InvalidArgument, "BC mip dimensions must be > 0");
        }
        const uint32_t d = std::max<uint32_t>(1u, src_mip.depth);
        const uint32_t blocks_x = (src_mip.width + 3u) / 4u;
        const uint32_t blocks_y = (src_mip.height + 3u) / 4u;
        const size_t tight_row_bytes = static_cast<size_t>(blocks_x) * block_bytes;
        const size_t row_pitch =
            (src_mip.src_row_pitch_bytes > 0u) ? static_cast<size_t>(src_mip.src_row_pitch_bytes) : tight_row_bytes;
        if (row_pitch < tight_row_bytes) {
            return Status::Error(StatusCode::InvalidArgument, "BC mip src_row_pitch_bytes smaller than tight row");
        }
        const size_t total_rows = static_cast<size_t>(blocks_y) * d;
        const size_t min_src_bytes =
            (total_rows > 0u) ? ((total_rows - 1u) * row_pitch + tight_row_bytes) : 0u;
        if (src_mip.src_data.size() < min_src_bytes) {
            return Status::Error(StatusCode::InvalidArgument, "BC mip src_data smaller than required pitched size");
        }

        auto& pm = packed_mips[m];
        pm.width = src_mip.width;
        pm.height = src_mip.height;
        pm.depth = d;
        pm.data.resize(total_rows * tight_row_bytes);
        for (size_t r = 0; r < total_rows; ++r) {
            std::memcpy(pm.data.data() + r * tight_row_bytes,
                        src_mip.src_data.data() + r * row_pitch,
                        tight_row_bytes);
        }

        while (total_bytes % 16 != 0) ++total_bytes;
        mip_offsets[m] = total_bytes;
        total_bytes += pm.data.size();
    }

    out_result.staged_mip_count = static_cast<uint32_t>(packed_mips.size());
    out_result.total_staged_bytes = total_bytes;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue || !impl_->bc_compression_enabled) {
        out_result.readback_mips = std::move(packed_mips);
        out_result.executed_on_vulkan_icd = false;
        out_result.used_software_fallback = true;
        return Status::Ok();
    }

    VkImage src_img = kVkNullHandle, dst_img = kVkNullHandle;
    VkDeviceMemory src_mem = kVkNullHandle, dst_mem = kVkNullHandle;
    VkBuffer up_buf = kVkNullHandle, mid_buf = kVkNullHandle, rb_buf = kVkNullHandle;
    VkDeviceMemory up_mem = kVkNullHandle, mid_mem = kVkNullHandle, rb_mem = kVkNullHandle;
    bool up_coh = true, mid_coh = true, rb_coh = true;
    VkCommandBuffer cmd = nullptr;

    auto cleanup = [&]() {
        if (cmd) { impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd); cmd = nullptr; }
        if (rb_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, rb_buf, nullptr);
        if (mid_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, mid_buf, nullptr);
        if (up_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, up_buf, nullptr);
        if (rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, rb_mem, nullptr);
        if (mid_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, mid_mem, nullptr);
        if (up_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, up_mem, nullptr);
        if (dst_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, dst_img, nullptr);
        if (src_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, src_img, nullptr);
        if (dst_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, dst_mem, nullptr);
        if (src_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, src_mem, nullptr);
    };

    const uint32_t mip_cnt = static_cast<uint32_t>(packed_mips.size());
    const VkFlags img_usage = kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit | kVkImageUsageSampledBit;
    Status st = impl_->CreateDeviceImage(
        kVkImageType2D, static_cast<int32_t>(desc.format),
        desc.width, desc.height, 1u, mip_cnt, kVkSampleCount1Bit, img_usage, src_img, src_mem);
    if (!st.ok()) {
        cleanup();
        out_result.readback_mips = std::move(packed_mips);
        out_result.executed_on_vulkan_icd = false;
        out_result.used_software_fallback = true;
        return Status::Ok();
    }
    st = impl_->CreateDeviceImage(
        kVkImageType2D, static_cast<int32_t>(desc.format),
        desc.width, desc.height, 1u, mip_cnt, kVkSampleCount1Bit, img_usage, dst_img, dst_mem);
    if (!st.ok()) {
        cleanup();
        out_result.readback_mips = std::move(packed_mips);
        out_result.executed_on_vulkan_icd = false;
        out_result.used_software_fallback = true;
        return Status::Ok();
    }

    std::vector<uint8_t> staging_data(static_cast<size_t>(total_bytes), 0u);
    for (size_t m = 0; m < packed_mips.size(); ++m) {
        std::memcpy(staging_data.data() + mip_offsets[m], packed_mips[m].data.data(), packed_mips[m].data.size());
    }

    st = impl_->CreateHostBuffer(total_bytes, kVkBufferUsageTransferSrcBit, up_buf, up_mem, up_coh,
                                 staging_data.data(), staging_data.size());
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(total_bytes, kVkBufferUsageTransferDstBit | kVkBufferUsageTransferSrcBit,
                                 mid_buf, mid_mem, mid_coh);
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(total_bytes, kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coh);
    if (!st.ok()) { cleanup(); return st; }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(bc_staged) failed");
    }
    VkCommandBufferBeginInfo cbi{kVkStypeCommandBufferBeginInfo, nullptr, kVkCommandBufferUsageOneTimeSubmitBit, nullptr};
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(bc_staged) failed");
    }

    // Stage 1: upload tightly packed BC blocks -> src_img
    impl_->CmdImageBarrier(cmd, src_img, kVkImageAspectColorBit,
                           kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                           0u, kVkAccessTransferWriteBit, mip_cnt);
    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {packed_mips[m].width, packed_mips[m].height, 1u};
        impl_->vkCmdCopyBufferToImage(cmd, up_buf, src_img, kVkImageLayoutTransferDstOptimal, 1, &r);
    }

    // Stage 2: staged BC image-to-image copy via intermediate buffer (src_img -> mid_buf -> dst_img)
    impl_->CmdImageBarrier(cmd, src_img, kVkImageAspectColorBit,
                           kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                           kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                           kVkAccessTransferWriteBit, kVkAccessTransferReadBit, mip_cnt);
    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {packed_mips[m].width, packed_mips[m].height, 1u};
        impl_->vkCmdCopyImageToBuffer(cmd, src_img, kVkImageLayoutTransferSrcOptimal, mid_buf, 1, &r);
    }

    VkBufferMemoryBarrier mid_bar{};
    mid_bar.sType = kVkStypeBufferMemoryBarrier;
    mid_bar.srcAccessMask = kVkAccessTransferWriteBit;
    mid_bar.dstAccessMask = kVkAccessTransferReadBit;
    mid_bar.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    mid_bar.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    mid_bar.buffer = mid_buf;
    mid_bar.offset = 0;
    mid_bar.size = total_bytes;
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                                0, 0, nullptr, 1, &mid_bar, 0, nullptr);

    impl_->CmdImageBarrier(cmd, dst_img, kVkImageAspectColorBit,
                           kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                           0u, kVkAccessTransferWriteBit, mip_cnt);
    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {packed_mips[m].width, packed_mips[m].height, 1u};
        impl_->vkCmdCopyBufferToImage(cmd, mid_buf, dst_img, kVkImageLayoutTransferDstOptimal, 1, &r);
    }

    // Stage 3: read back dst_img -> rb_buf for bit-exact verification
    impl_->CmdImageBarrier(cmd, dst_img, kVkImageAspectColorBit,
                           kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                           kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                           kVkAccessTransferWriteBit, kVkAccessTransferReadBit, mip_cnt);
    for (uint32_t m = 0; m < mip_cnt; ++m) {
        VkBufferImageCopy r{};
        r.bufferOffset = mip_offsets[m];
        r.imageSubresource = {kVkImageAspectColorBit, m, 0u, 1u};
        r.imageExtent = {packed_mips[m].width, packed_mips[m].height, 1u};
        impl_->vkCmdCopyImageToBuffer(cmd, dst_img, kVkImageLayoutTransferSrcOptimal, rb_buf, 1, &r);
    }

    VkBufferMemoryBarrier rb_bar{};
    rb_bar.sType = kVkStypeBufferMemoryBarrier;
    rb_bar.srcAccessMask = kVkAccessTransferWriteBit;
    rb_bar.dstAccessMask = kVkAccessHostReadBit;
    rb_bar.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bar.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bar.buffer = rb_buf;
    rb_bar.offset = 0;
    rb_bar.size = total_bytes;
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &rb_bar, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(bc_staged) failed");
    }

    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->graphics_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "Staged BC texture copy submit/wait failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, rb_mem, 0, total_bytes, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(bc_staged readback) failed");
    }
    if (!rb_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }

    out_result.readback_mips.resize(packed_mips.size());
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (size_t m = 0; m < packed_mips.size(); ++m) {
        out_result.readback_mips[m].width = packed_mips[m].width;
        out_result.readback_mips[m].height = packed_mips[m].height;
        out_result.readback_mips[m].depth = packed_mips[m].depth;
        out_result.readback_mips[m].data.assign(
            src_bytes + mip_offsets[m], src_bytes + mip_offsets[m] + packed_mips[m].data.size());
    }
    impl_->vkUnmapMemory(impl_->device, rb_mem);
    cleanup();
    out_result.executed_on_vulkan_icd = true;
    out_result.used_software_fallback = false;
    return Status::Ok();
}

Status VulkanBackend::CreateWin32Swapchain(const VulkanSwapchainDesc& desc,
                                           VulkanSwapchainInfo& out_info) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0 || desc.height == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Swapchain dimensions must be > 0");
    }

    out_info = VulkanSwapchainInfo{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    Impl::SwapchainRecord rec{};
    rec.info.width = desc.width;
    rec.info.height = desc.height;

    const bool try_win32 = !desc.headless_offscreen && desc.hwnd != nullptr &&
                           !impl_->fallback_mode && impl_->device != nullptr &&
                           impl_->has_graphics_queue && impl_->win32_surface_enabled &&
                           impl_->swapchain_enabled;

#if defined(_WIN32)
    if (try_win32) {
        VkWin32SurfaceCreateInfoKHR surf_ci{};
        surf_ci.sType = kVkStypeWin32SurfaceCreateInfoKhr;
        surf_ci.hinstance = desc.hinstance ? desc.hinstance : static_cast<void*>(GetModuleHandleA(nullptr));
        surf_ci.hwnd = desc.hwnd;

        VkSurfaceKHR surface = kVkNullHandle;
        if (impl_->vkCreateWin32SurfaceKHR(impl_->instance, &surf_ci, nullptr, &surface) == kVkSuccess &&
            surface != kVkNullHandle) {
            VkBool32 supported = 0;
            VkSurfaceCapabilitiesKHR caps{};
            uint32_t fmt_count = 0;
            const bool surf_ok =
                impl_->vkGetPhysicalDeviceSurfaceSupportKHR(
                    impl_->physical_device, impl_->graphics_queue_family, surface, &supported) == kVkSuccess &&
                supported != 0 &&
                impl_->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl_->physical_device, surface, &caps) == kVkSuccess &&
                caps.maxImageExtent.width > 0 && caps.maxImageExtent.height > 0 &&
                impl_->vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physical_device, surface, &fmt_count, nullptr) == kVkSuccess &&
                fmt_count > 0;

            if (surf_ok) {
                std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
                impl_->vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physical_device, surface, &fmt_count, fmts.data());

                VkSurfaceFormatKHR chosen = fmts[0];
                bool hdr10_found = false;
                VkSurfaceFormatKHR hdr10_fmt{};
                for (const auto& f : fmts) {
                    const uint32_t cs = static_cast<uint32_t>(f.colorSpace);
                    if (std::find(rec.info.supported_color_spaces.begin(),
                                  rec.info.supported_color_spaces.end(), cs) ==
                        rec.info.supported_color_spaces.end()) {
                        rec.info.supported_color_spaces.push_back(cs);
                    }
                    if (cs == kVkColorSpaceHdr10St2084Ext) {
                        hdr10_found = true;
                        hdr10_fmt = f;
                    }
                    if ((f.format == kVkFormatB8G8R8A8Unorm || f.format == kVkFormatR8G8B8A8Unorm) &&
                        cs == kVkColorSpaceSrgbNonlinearKhr) {
                        chosen = f;
                    }
                }
                rec.info.hdr10_advertised = hdr10_found;
                if (desc.prefer_hdr10 && hdr10_found) {
                    chosen = hdr10_fmt;
                    rec.info.hdr10_active = true;
                }

                VkExtent2D extent{desc.width, desc.height};
                if (caps.currentExtent.width != 0xFFFFFFFFu && caps.currentExtent.width > 0 &&
                    caps.currentExtent.height > 0) {
                    extent = caps.currentExtent;
                } else {
                    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
                    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
                }

                uint32_t img_count = std::max<uint32_t>(caps.minImageCount, 2u);
                if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) {
                    img_count = caps.maxImageCount;
                }

                VkFlags usage = kVkImageUsageColorAttachmentBit | kVkImageUsageTransferDstBit;
                rec.supports_transfer_src = (caps.supportedUsageFlags & kVkImageUsageTransferSrcBit) != 0;
                if (rec.supports_transfer_src) usage |= kVkImageUsageTransferSrcBit;

                VkSwapchainCreateInfoKHR sc_ci{};
                sc_ci.sType = kVkStypeSwapchainCreateInfoKhr;
                sc_ci.surface = surface;
                sc_ci.minImageCount = img_count;
                sc_ci.imageFormat = chosen.format;
                sc_ci.imageColorSpace = chosen.colorSpace;
                sc_ci.imageExtent = extent;
                sc_ci.imageArrayLayers = 1;
                sc_ci.imageUsage = usage;
                sc_ci.imageSharingMode = kVkSharingModeExclusive;
                sc_ci.preTransform = caps.currentTransform ? caps.currentTransform : 1u;
                sc_ci.compositeAlpha = kVkCompositeAlphaOpaqueBitKhr;
                sc_ci.presentMode = kVkPresentModeFifoKhr;
                sc_ci.clipped = 1u;

                VkSwapchainKHR sc = kVkNullHandle;
                if (impl_->vkCreateSwapchainKHR(impl_->device, &sc_ci, nullptr, &sc) == kVkSuccess &&
                    sc != kVkNullHandle) {
                    uint32_t sc_img_cnt = 0;
                    impl_->vkGetSwapchainImagesKHR(impl_->device, sc, &sc_img_cnt, nullptr);
                    rec.swapchain_images.resize(sc_img_cnt, kVkNullHandle);
                    if (sc_img_cnt > 0) {
                        impl_->vkGetSwapchainImagesKHR(impl_->device, sc, &sc_img_cnt, rec.swapchain_images.data());
                    }
                    rec.surface = surface;
                    rec.swapchain = sc;
                    rec.info.handle = impl_->next_swapchain_handle++;
                    rec.info.width = extent.width;
                    rec.info.height = extent.height;
                    rec.info.image_count = sc_img_cnt;
                    rec.info.vk_format = static_cast<uint32_t>(chosen.format);
                    rec.info.vk_color_space = static_cast<uint32_t>(chosen.colorSpace);
                    rec.info.is_win32_surface = true;
                    rec.info.is_headless_offscreen = false;
                    out_info = rec.info;
                    impl_->swapchains[rec.info.handle] = std::move(rec);
                    return Status::Ok();
                }
            }
            impl_->vkDestroySurfaceKHR(impl_->instance, surface, nullptr);
        }
    }
#else
    (void)try_win32;
#endif

    // Headless offscreen mode (real GPU VkImage if ICD available, or software fallback)
    if (!impl_->fallback_mode && impl_->device && impl_->has_graphics_queue) {
        Status st = impl_->CreateDeviceImage(
            kVkImageType2D, kVkFormatR8G8B8A8Unorm, desc.width, desc.height, 1, 1, kVkSampleCount1Bit,
            kVkImageUsageColorAttachmentBit | kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit,
            rec.offscreen_image, rec.offscreen_mem);
        if (!st.ok()) return st;
    }

    rec.info.handle = impl_->next_swapchain_handle++;
    rec.info.width = desc.width;
    rec.info.height = desc.height;
    rec.info.image_count = 2;
    rec.info.vk_format = static_cast<uint32_t>(kVkFormatR8G8B8A8Unorm);
    rec.info.vk_color_space = kVkColorSpaceSrgbNonlinearKhr;
    rec.info.is_win32_surface = false;
    rec.info.is_headless_offscreen = true;
    rec.info.hdr10_advertised = impl_->hdr10_colorspace_enabled;
    rec.info.hdr10_active = false;
    rec.info.supported_color_spaces.push_back(kVkColorSpaceSrgbNonlinearKhr);
    if (impl_->hdr10_colorspace_enabled) {
        rec.info.supported_color_spaces.push_back(kVkColorSpaceHdr10St2084Ext);
    }

    out_info = rec.info;
    impl_->swapchains[rec.info.handle] = std::move(rec);
    return Status::Ok();
}

Status VulkanBackend::PresentRgba8ToSwapchain(uint64_t swapchain_handle,
                                              const FrameBufferRgba8& frame,
                                              VulkanPresentResult& out_result,
                                              bool readback_frame) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (frame.width == 0 || frame.height == 0 ||
        frame.pixels.size() != static_cast<size_t>(frame.width) * frame.height * 4u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid RGBA8 framebuffer for presentation");
    }

    out_result = VulkanPresentResult{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->swapchains.find(swapchain_handle);
    if (it == impl_->swapchains.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan swapchain handle");
    }
    auto& rec = it->second;

    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue) {
        out_result.image_index = rec.frame_counter++ % std::max<uint32_t>(1, rec.info.image_count);
        out_result.presented_to_win32_swapchain = false;
        out_result.readback_available = true;
        out_result.readback_framebuffer = frame;
        out_result.fnv1a_hash = frame.ComputeFnv1aHash();
        return Status::Ok();
    }

    const uint32_t w = std::min(frame.width, rec.info.width);
    const uint32_t h = std::min(frame.height, rec.info.height);
    const size_t px_count = static_cast<size_t>(w) * h;
    std::vector<uint8_t> upload_bytes(px_count * 4u, 0u);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t sidx = (static_cast<size_t>(y) * frame.width + x) * 4u;
            const size_t didx = (static_cast<size_t>(y) * w + x) * 4u;
            const uint8_t r = frame.pixels[sidx + 0];
            const uint8_t g = frame.pixels[sidx + 1];
            const uint8_t b = frame.pixels[sidx + 2];
            const uint8_t a = frame.pixels[sidx + 3];
            if (rec.info.vk_format == static_cast<uint32_t>(kVkFormatB8G8R8A8Unorm)) {
                upload_bytes[didx + 0] = b;
                upload_bytes[didx + 1] = g;
                upload_bytes[didx + 2] = r;
                upload_bytes[didx + 3] = a;
            } else if (rec.info.vk_format == static_cast<uint32_t>(kVkFormatA2B10G10R10UnormPack32)) {
                const uint32_t packed = PackA2B10G10R10(r, g, b, a);
                std::memcpy(upload_bytes.data() + didx, &packed, 4);
            } else {
                upload_bytes[didx + 0] = r;
                upload_bytes[didx + 1] = g;
                upload_bytes[didx + 2] = b;
                upload_bytes[didx + 3] = a;
            }
        }
    }

    VkBuffer up_buf = kVkNullHandle, rb_buf = kVkNullHandle;
    VkDeviceMemory up_mem = kVkNullHandle, rb_mem = kVkNullHandle;
    bool up_coh = true, rb_coh = true;
    VkSemaphore img_avail = kVkNullHandle, render_done = kVkNullHandle;
    VkCommandBuffer cmd = nullptr;

    auto cleanup = [&]() {
        if (cmd) impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd);
        if (render_done != kVkNullHandle) impl_->vkDestroySemaphore(impl_->device, render_done, nullptr);
        if (img_avail != kVkNullHandle) impl_->vkDestroySemaphore(impl_->device, img_avail, nullptr);
        if (rb_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, rb_buf, nullptr);
        if (up_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, up_buf, nullptr);
        if (rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, rb_mem, nullptr);
        if (up_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, up_mem, nullptr);
    };

    Status st = impl_->CreateHostBuffer(upload_bytes.size(), kVkBufferUsageTransferSrcBit, up_buf, up_mem, up_coh,
                                        upload_bytes.data(), upload_bytes.size());
    if (!st.ok()) { cleanup(); return st; }

    const bool do_gpu_readback =
        readback_frame && (rec.info.is_headless_offscreen || rec.supports_transfer_src);
    if (do_gpu_readback) {
        st = impl_->CreateHostBuffer(upload_bytes.size(), kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coh);
        if (!st.ok()) { cleanup(); return st; }
    }

    uint32_t img_index = 0;
    VkImage target_img = rec.offscreen_image;
    if (rec.info.is_win32_surface && rec.swapchain != kVkNullHandle) {
        VkSemaphoreCreateInfo sci{kVkStypeSemaphoreCreateInfo, nullptr, 0};
        if (impl_->vkCreateSemaphore(impl_->device, &sci, nullptr, &img_avail) != kVkSuccess ||
            impl_->vkCreateSemaphore(impl_->device, &sci, nullptr, &render_done) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreateSemaphore(present) failed");
        }
        const VkResult acq = impl_->vkAcquireNextImageKHR(
            impl_->device, rec.swapchain, kVkUint64Max, img_avail, kVkNullHandle, &img_index);
        if (acq != kVkSuccess && acq != kVkSuboptimalKhr) {
            cleanup();
            return Status::Error(StatusCode::DeviceError,
                                 "vkAcquireNextImageKHR failed (VkResult=" + std::to_string(acq) + ")");
        }
        if (img_index >= rec.swapchain_images.size()) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "Swapchain image index out of range");
        }
        target_img = rec.swapchain_images[img_index];
    } else {
        img_index = rec.frame_counter++ % std::max<uint32_t>(1, rec.info.image_count);
    }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(present) failed");
    }

    VkCommandBufferBeginInfo cbi{kVkStypeCommandBufferBeginInfo, nullptr, kVkCommandBufferUsageOneTimeSubmitBit, nullptr};
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(present) failed");
    }

    impl_->CmdImageBarrier(cmd, target_img, kVkImageAspectColorBit,
                           kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                           0u, kVkAccessTransferWriteBit);

    VkBufferImageCopy cp{};
    cp.imageSubresource = {kVkImageAspectColorBit, 0, 0, 1};
    cp.imageExtent = {w, h, 1};
    impl_->vkCmdCopyBufferToImage(cmd, up_buf, target_img, kVkImageLayoutTransferDstOptimal, 1, &cp);

    if (do_gpu_readback) {
        impl_->CmdImageBarrier(cmd, target_img, kVkImageAspectColorBit,
                               kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                               kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                               kVkAccessTransferWriteBit, kVkAccessTransferReadBit);
        impl_->vkCmdCopyImageToBuffer(cmd, target_img, kVkImageLayoutTransferSrcOptimal, rb_buf, 1, &cp);
    }

    if (rec.info.is_win32_surface) {
        const int32_t cur_layout = do_gpu_readback ? kVkImageLayoutTransferSrcOptimal : kVkImageLayoutTransferDstOptimal;
        const VkFlags cur_access = do_gpu_readback ? kVkAccessTransferReadBit : kVkAccessTransferWriteBit;
        impl_->CmdImageBarrier(cmd, target_img, kVkImageAspectColorBit,
                               cur_layout, kVkImageLayoutPresentSrcKhr,
                               kVkPipelineStageTransferBit, kVkPipelineStageBottomOfPipeBit,
                               cur_access, 0u);
    }

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(present) failed");
    }

    const VkFlags wait_stage = kVkPipelineStageTransferBit;
    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.waitSemaphoreCount = (img_avail != kVkNullHandle) ? 1u : 0u;
    si.pWaitSemaphores = (img_avail != kVkNullHandle) ? &img_avail : nullptr;
    si.pWaitDstStageMask = (img_avail != kVkNullHandle) ? &wait_stage : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = (render_done != kVkNullHandle) ? 1u : 0u;
    si.pSignalSemaphores = (render_done != kVkNullHandle) ? &render_done : nullptr;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueSubmit(present) failed");
    }

    if (rec.info.is_win32_surface && rec.swapchain != kVkNullHandle) {
        VkPresentInfoKHR pi{};
        pi.sType = kVkStypePresentInfoKhr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &rec.swapchain;
        pi.pImageIndices = &img_index;
        const VkResult pr = impl_->vkQueuePresentKHR(impl_->graphics_queue, &pi);
        if (pr != kVkSuccess && pr != kVkSuboptimalKhr) {
            impl_->vkQueueWaitIdle(impl_->graphics_queue);
            cleanup();
            return Status::Error(StatusCode::DeviceError,
                                 "vkQueuePresentKHR failed (VkResult=" + std::to_string(pr) + ")");
        }
        out_result.presented_to_win32_swapchain = true;
    }

    impl_->vkQueueWaitIdle(impl_->graphics_queue);

    if (do_gpu_readback) {
        void* mapped = nullptr;
        if (impl_->vkMapMemory(impl_->device, rb_mem, 0, upload_bytes.size(), 0, &mapped) == kVkSuccess && mapped) {
            if (!rb_coh) {
                VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0, kVkWholeSize};
                impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
            }
            out_result.readback_framebuffer.width = w;
            out_result.readback_framebuffer.height = h;
            out_result.readback_framebuffer.pixels.resize(px_count * 4u);
            const auto* src = static_cast<const uint8_t*>(mapped);
            if (rec.info.vk_format == static_cast<uint32_t>(kVkFormatB8G8R8A8Unorm)) {
                for (size_t i = 0; i < px_count; ++i) {
                    out_result.readback_framebuffer.pixels[i * 4 + 0] = src[i * 4 + 2];
                    out_result.readback_framebuffer.pixels[i * 4 + 1] = src[i * 4 + 1];
                    out_result.readback_framebuffer.pixels[i * 4 + 2] = src[i * 4 + 0];
                    out_result.readback_framebuffer.pixels[i * 4 + 3] = src[i * 4 + 3];
                }
            } else {
                std::memcpy(out_result.readback_framebuffer.pixels.data(), src, px_count * 4u);
            }
            impl_->vkUnmapMemory(impl_->device, rb_mem);
            out_result.readback_available = true;
            out_result.fnv1a_hash = out_result.readback_framebuffer.ComputeFnv1aHash();
        }
    } else {
        out_result.fnv1a_hash = frame.ComputeFnv1aHash();
    }

    out_result.image_index = img_index;
    cleanup();
    return Status::Ok();
}

Status VulkanBackend::DestroyWin32Swapchain(uint64_t swapchain_handle) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->swapchains.find(swapchain_handle);
    if (it == impl_->swapchains.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan swapchain handle");
    }
    impl_->DestroySwapchainRecord(it->second);
    impl_->swapchains.erase(it);
    return Status::Ok();
}

namespace {

uint64_t MakeSparseTileKey(uint32_t mip, uint32_t layer, uint32_t tx, uint32_t ty, uint32_t tz) noexcept {
    return (static_cast<uint64_t>(mip & 0xFu) << 60u) |
           (static_cast<uint64_t>(layer & 0xFFu) << 52u) |
           (static_cast<uint64_t>(tz & 0xFFFu) << 40u) |
           (static_cast<uint64_t>(ty & 0xFFFFFu) << 20u) |
           static_cast<uint64_t>(tx & 0xFFFFFu);
}

void ComputeStandardTileShape(VulkanImageFormat fmt, bool is_3d,
                              uint32_t& out_w, uint32_t& out_h, uint32_t& out_d) noexcept {
    const uint32_t bpp = BytesPerTexel(fmt);
    if (is_3d) {
        if (bpp >= 8u) { out_w = 32u; out_h = 16u; out_d = 16u; }
        else { out_w = 32u; out_h = 32u; out_d = 16u; }
    } else {
        if (bpp >= 8u) { out_w = 128u; out_h = 64u; out_d = 1u; }
        else { out_w = 128u; out_h = 128u; out_d = 1u; }
    }
}

void PopulateResourceTilingFromSparseReq(
    const VulkanReservedResourceDesc& desc,
    uint32_t gran_w, uint32_t gran_h, uint32_t gran_d,
    uint32_t mip_tail_first_lod,
    uint64_t tile_size_bytes,
    uint64_t mip_tail_offset,
    uint64_t mip_tail_size,
    uint64_t mip_tail_stride,
    uint32_t sparse_flags,
    bool is_icd,
    VulkanResourceTilingInfo& out_tiling) {
    out_tiling = VulkanResourceTilingInfo{};
    out_tiling.tile_width_in_texels = std::max<uint32_t>(1u, gran_w);
    out_tiling.tile_height_in_texels = std::max<uint32_t>(1u, gran_h);
    out_tiling.tile_depth_in_texels = std::max<uint32_t>(1u, gran_d);
    out_tiling.tile_size_bytes = std::max<uint64_t>(4096ull, tile_size_bytes);
    out_tiling.mip_tail_offset_bytes = mip_tail_offset;
    out_tiling.mip_tail_size_bytes = mip_tail_size;
    out_tiling.mip_tail_stride_bytes = mip_tail_stride;
    out_tiling.sparse_format_flags = sparse_flags;
    out_tiling.executed_on_vulkan_icd = is_icd;

    const uint32_t mips = std::max<uint32_t>(1u, desc.mip_levels);
    const uint32_t layers = desc.is_3d ? 1u : std::max<uint32_t>(1u, desc.array_layers);
    const uint32_t std_mips = std::min<uint32_t>(mip_tail_first_lod, mips);
    const uint32_t packed_mips = mips - std_mips;

    out_tiling.num_standard_mips = std_mips;
    out_tiling.num_packed_mips = packed_mips;

    uint32_t running_tile_index = 0u;
    out_tiling.subresource_tilings.reserve(static_cast<size_t>(std_mips) * layers);
    for (uint32_t layer = 0u; layer < layers; ++layer) {
        for (uint32_t m = 0u; m < std_mips; ++m) {
            const uint32_t mw = std::max<uint32_t>(1u, desc.width >> m);
            const uint32_t mh = std::max<uint32_t>(1u, desc.height >> m);
            const uint32_t md = desc.is_3d ? std::max<uint32_t>(1u, desc.depth >> m) : 1u;

            VulkanSubresourceTiling st{};
            st.width_in_tiles = (mw + out_tiling.tile_width_in_texels - 1u) / out_tiling.tile_width_in_texels;
            st.height_in_tiles = (mh + out_tiling.tile_height_in_texels - 1u) / out_tiling.tile_height_in_texels;
            st.depth_in_tiles = (md + out_tiling.tile_depth_in_texels - 1u) / out_tiling.tile_depth_in_texels;
            st.start_tile_index_in_overall_resource = running_tile_index;
            running_tile_index += st.width_in_tiles * st.height_in_tiles * st.depth_in_tiles;
            out_tiling.subresource_tilings.push_back(st);
        }
    }

    const uint32_t packed_tiles_per_tail =
        (packed_mips > 0u)
            ? static_cast<uint32_t>(
                  (std::max<uint64_t>(mip_tail_size, out_tiling.tile_size_bytes) +
                   out_tiling.tile_size_bytes - 1ull) /
                  out_tiling.tile_size_bytes)
            : 0u;
    const bool single_miptail = (sparse_flags & kVkSparseImageFormatSingleMiptailBit) != 0u;
    const uint32_t total_packed_tiles =
        (packed_mips > 0u) ? (single_miptail ? packed_tiles_per_tail : (packed_tiles_per_tail * layers)) : 0u;

    out_tiling.start_tile_index_in_overall_resource = running_tile_index;
    out_tiling.num_tiles_for_packed_mips = total_packed_tiles;
    out_tiling.num_tiles_for_entire_resource = running_tile_index + total_packed_tiles;
}

}  // namespace

Status VulkanBackend::CreateReservedImage(const VulkanReservedResourceDesc& desc,
                                          VulkanReservedResourceHandle& out_handle) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0u || desc.height == 0u || (desc.is_3d && desc.depth == 0u) ||
        desc.mip_levels == 0u || desc.array_layers == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid reserved image dimensions");
    }

    out_handle = VulkanReservedResourceHandle{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    Impl::ReservedImageRecord rec{};
    rec.desc = desc;

    const bool can_use_icd_sparse =
        !impl_->fallback_mode && impl_->device != nullptr &&
        impl_->sparse_binding_enabled &&
        (desc.is_3d ? impl_->sparse_residency_3d_enabled : impl_->sparse_residency_2d_enabled) &&
        impl_->vkGetImageSparseMemoryRequirements != nullptr &&
        impl_->vkQueueBindSparse != nullptr &&
        impl_->sparse_queue != nullptr;

    if (can_use_icd_sparse) {
        VkImageCreateInfo ici{};
        ici.sType = kVkStypeImageCreateInfo;
        ici.flags = kVkImageCreateSparseBindingBit | kVkImageCreateSparseResidencyBit;
        ici.imageType = desc.is_3d ? kVkImageType3D : kVkImageType2D;
        ici.format = static_cast<int32_t>(desc.format);
        ici.extent = {desc.width, desc.height, desc.is_3d ? desc.depth : 1u};
        ici.mipLevels = desc.mip_levels;
        ici.arrayLayers = desc.is_3d ? 1u : desc.array_layers;
        ici.samples = kVkSampleCount1Bit;
        ici.tiling = kVkImageTilingOptimal;
        ici.usage = kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit | kVkImageUsageSampledBit;
        ici.sharingMode = kVkSharingModeExclusive;
        ici.initialLayout = kVkImageLayoutUndefined;

        VkImage img = kVkNullHandle;
        const VkResult vr = impl_->vkCreateImage(impl_->device, &ici, nullptr, &img);
        if (vr != kVkSuccess || img == kVkNullHandle) {
            return Status::Error(StatusCode::DeviceError,
                                 "vkCreateImage(SPARSE_BINDING|SPARSE_RESIDENCY) failed (VkResult=" +
                                     std::to_string(vr) + ")");
        }

        VkMemoryRequirements mem_req{};
        impl_->vkGetImageMemoryRequirements(impl_->device, img, &mem_req);

        uint32_t sparse_req_count = 0u;
        impl_->vkGetImageSparseMemoryRequirements(impl_->device, img, &sparse_req_count, nullptr);
        std::vector<VkSparseImageMemoryRequirements> sparse_reqs(sparse_req_count);
        if (sparse_req_count > 0u) {
            impl_->vkGetImageSparseMemoryRequirements(
                impl_->device, img, &sparse_req_count, sparse_reqs.data());
        }

        const VkFlags target_aspect =
            (desc.format == VulkanImageFormat::D32Sfloat) ? kVkImageAspectDepthBit : kVkImageAspectColorBit;
        VkSparseImageMemoryRequirements chosen{};
        bool found_aspect = false;
        for (const auto& sr : sparse_reqs) {
            if ((sr.formatProperties.aspectMask & target_aspect) != 0u) {
                chosen = sr;
                found_aspect = true;
                break;
            }
        }
        if (!found_aspect && !sparse_reqs.empty()) {
            chosen = sparse_reqs[0];
            found_aspect = true;
        }

        uint32_t gran_w = 128u, gran_h = 128u, gran_d = 1u;
        ComputeStandardTileShape(desc.format, desc.is_3d, gran_w, gran_h, gran_d);
        uint32_t mip_tail_first = desc.mip_levels;
        uint64_t tail_off = 0ull, tail_sz = 0ull, tail_stride = 0ull;
        uint32_t sp_flags = 0u;
        if (found_aspect) {
            if (chosen.formatProperties.imageGranularity.width > 0u) {
                gran_w = chosen.formatProperties.imageGranularity.width;
            }
            if (chosen.formatProperties.imageGranularity.height > 0u) {
                gran_h = chosen.formatProperties.imageGranularity.height;
            }
            if (chosen.formatProperties.imageGranularity.depth > 0u) {
                gran_d = chosen.formatProperties.imageGranularity.depth;
            }
            mip_tail_first = chosen.imageMipTailFirstLod;
            tail_off = chosen.imageMipTailOffset;
            tail_sz = chosen.imageMipTailSize;
            tail_stride = chosen.imageMipTailStride;
            sp_flags = chosen.formatProperties.flags;
        }

        const uint64_t tile_bytes = std::max<uint64_t>(mem_req.alignment, 65536ull);
        PopulateResourceTilingFromSparseReq(
            desc, gran_w, gran_h, gran_d, mip_tail_first, tile_bytes,
            tail_off, tail_sz, tail_stride, sp_flags, true, rec.tiling);

        // Allocate a device-local backing pool sized for the entire sparse image (capped safely).
        const uint32_t pool_tiles = std::max<uint32_t>(rec.tiling.num_tiles_for_entire_resource, 1u);
        const VkDeviceSize pool_bytes = std::max<VkDeviceSize>(
            mem_req.size, static_cast<VkDeviceSize>(pool_tiles) * tile_bytes);

        bool coherent = false;
        const uint32_t mem_idx = impl_->FindMemoryType(
            mem_req.memoryTypeBits, kVkMemoryPropertyDeviceLocalBit, coherent);
        if (mem_idx == std::numeric_limits<uint32_t>::max()) {
            impl_->vkDestroyImage(impl_->device, img, nullptr);
            return Status::Error(StatusCode::OutOfMemory, "No compatible memory type for sparse image");
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = kVkStypeMemoryAllocateInfo;
        mai.allocationSize = pool_bytes;
        mai.memoryTypeIndex = mem_idx;
        VkDeviceMemory mem = kVkNullHandle;
        if (impl_->vkAllocateMemory(impl_->device, &mai, nullptr, &mem) != kVkSuccess ||
            mem == kVkNullHandle) {
            impl_->vkDestroyImage(impl_->device, img, nullptr);
            return Status::Error(StatusCode::OutOfMemory, "vkAllocateMemory(sparse pool) failed");
        }

        rec.image = img;
        rec.backing_memory = mem;
        rec.backing_memory_size = pool_bytes;
        rec.tile_alignment = tile_bytes;
        rec.backing_tile_capacity = static_cast<uint32_t>(pool_bytes / tile_bytes);
        rec.is_icd = true;
    } else {
        // Software fallback / non-sparse device: compute exact standard 64 KiB D3D12/Vulkan tiling!
        uint32_t gran_w = 128u, gran_h = 128u, gran_d = 1u;
        ComputeStandardTileShape(desc.format, desc.is_3d, gran_w, gran_h, gran_d);
        uint32_t mip_tail_first = desc.mip_levels;
        for (uint32_t m = 0u; m < desc.mip_levels; ++m) {
            const uint32_t mw = std::max<uint32_t>(1u, desc.width >> m);
            const uint32_t mh = std::max<uint32_t>(1u, desc.height >> m);
            const uint32_t md = desc.is_3d ? std::max<uint32_t>(1u, desc.depth >> m) : 1u;
            if (mw < gran_w || mh < gran_h || (desc.is_3d && md < gran_d)) {
                mip_tail_first = m;
                break;
            }
        }
        const uint64_t tail_size = (mip_tail_first < desc.mip_levels) ? 65536ull : 0ull;
        PopulateResourceTilingFromSparseReq(
            desc, gran_w, gran_h, gran_d, mip_tail_first, 65536ull,
            0ull, tail_size, tail_size, 0u, false, rec.tiling);
        rec.tile_alignment = 65536ull;
        rec.backing_tile_capacity = std::max<uint32_t>(rec.tiling.num_tiles_for_entire_resource, 1u);
        rec.is_icd = false;
    }

    const uint64_t handle = impl_->next_reserved_handle++;
    out_handle.handle = handle;
    out_handle.tiling = rec.tiling;
    out_handle.executed_on_vulkan_icd = rec.is_icd;
    impl_->reserved_images.emplace(handle, std::move(rec));
    return Status::Ok();
}

Status VulkanBackend::GetReservedImageTiling(uint64_t reserved_handle,
                                             VulkanResourceTilingInfo& out_tiling) const {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->reserved_images.find(reserved_handle);
    if (it == impl_->reserved_images.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan reserved image handle");
    }
    out_tiling = it->second.tiling;
    return Status::Ok();
}

Status VulkanBackend::BindReservedImageTiles(uint64_t reserved_handle,
                                             const std::vector<VulkanSparseTileMapping>& mappings) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (mappings.empty()) {
        return Status::Ok();
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->reserved_images.find(reserved_handle);
    if (it == impl_->reserved_images.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan reserved image handle");
    }
    auto& rec = it->second;

    const VkFlags aspect =
        (rec.desc.format == VulkanImageFormat::D32Sfloat) ? kVkImageAspectDepthBit : kVkImageAspectColorBit;

    std::vector<VkSparseImageMemoryBind> img_binds;
    std::vector<VkSparseMemoryBind> opaque_binds;

    for (const auto& m : mappings) {
        if (m.bind_mip_tail) {
            if (rec.tiling.num_packed_mips > 0u && rec.tiling.mip_tail_size_bytes > 0ull) {
                VkSparseMemoryBind ob{};
                ob.resourceOffset = rec.tiling.mip_tail_offset_bytes +
                                    static_cast<VkDeviceSize>(m.array_layer) * rec.tiling.mip_tail_stride_bytes;
                ob.size = rec.tiling.mip_tail_size_bytes;
                if (m.map_null) {
                    ob.memory = kVkNullHandle;
                    ob.memoryOffset = 0ull;
                } else {
                    const VkDeviceSize mem_off =
                        static_cast<VkDeviceSize>(m.heap_tile_offset) * rec.tile_alignment;
                    if (rec.is_icd && mem_off + ob.size > rec.backing_memory_size) {
                        return Status::Error(StatusCode::InvalidArgument,
                                             "Sparse mip-tail heap_tile_offset exceeds backing memory");
                    }
                    ob.memory = rec.backing_memory;
                    ob.memoryOffset = mem_off;
                }
                opaque_binds.push_back(ob);
            }
            rec.mip_tail_bound = !m.map_null;
            continue;
        }

        if (m.mip_level >= rec.desc.mip_levels ||
            (!rec.desc.is_3d && m.array_layer >= rec.desc.array_layers)) {
            return Status::Error(StatusCode::InvalidArgument, "Sparse tile mapping subresource out of range");
        }
        const uint32_t mw = std::max<uint32_t>(1u, rec.desc.width >> m.mip_level);
        const uint32_t mh = std::max<uint32_t>(1u, rec.desc.height >> m.mip_level);
        const uint32_t md = rec.desc.is_3d ? std::max<uint32_t>(1u, rec.desc.depth >> m.mip_level) : 1u;

        const uint32_t tw = rec.tiling.tile_width_in_texels;
        const uint32_t th = rec.tiling.tile_height_in_texels;
        const uint32_t td = rec.tiling.tile_depth_in_texels;

        const uint32_t cx = std::max<uint32_t>(1u, m.tile_count_x);
        const uint32_t cy = std::max<uint32_t>(1u, m.tile_count_y);
        const uint32_t cz = std::max<uint32_t>(1u, m.tile_count_z);

        uint32_t heap_idx = m.heap_tile_offset;
        for (uint32_t dz = 0u; dz < cz; ++dz) {
            for (uint32_t dy = 0u; dy < cy; ++dy) {
                for (uint32_t dx = 0u; dx < cx; ++dx) {
                    const uint32_t tx = m.tile_x + dx;
                    const uint32_t ty = m.tile_y + dy;
                    const uint32_t tz = m.tile_z + dz;
                    const uint32_t ox = tx * tw;
                    const uint32_t oy = ty * th;
                    const uint32_t oz = tz * td;
                    if (ox >= mw || oy >= mh || oz >= md) {
                        return Status::Error(StatusCode::InvalidArgument, "Sparse tile coordinate out of bounds");
                    }
                    const uint32_t ew = std::min<uint32_t>(tw, mw - ox);
                    const uint32_t eh = std::min<uint32_t>(th, mh - oy);
                    const uint32_t ed = std::min<uint32_t>(td, md - oz);

                    VkSparseImageMemoryBind ib{};
                    ib.subresource = {aspect, m.mip_level, rec.desc.is_3d ? 0u : m.array_layer};
                    ib.offset = {static_cast<int32_t>(ox), static_cast<int32_t>(oy), static_cast<int32_t>(oz)};
                    ib.extent = {ew, eh, ed};
                    if (m.map_null) {
                        ib.memory = kVkNullHandle;
                        ib.memoryOffset = 0ull;
                    } else {
                        const VkDeviceSize mem_off = static_cast<VkDeviceSize>(heap_idx) * rec.tile_alignment;
                        if (rec.is_icd && mem_off + rec.tile_alignment > rec.backing_memory_size) {
                            return Status::Error(StatusCode::InvalidArgument,
                                                 "Sparse tile heap_tile_offset exceeds backing memory");
                        }
                        ib.memory = rec.backing_memory;
                        ib.memoryOffset = mem_off;
                    }
                    img_binds.push_back(ib);

                    const uint64_t key = MakeSparseTileKey(m.mip_level, m.array_layer, tx, ty, tz);
                    rec.bound_tiles[key] = !m.map_null;
                    ++heap_idx;
                }
            }
        }
    }

    if (rec.is_icd && impl_->vkQueueBindSparse && impl_->sparse_queue) {
        VkSparseImageMemoryBindInfo img_bind_info{};
        img_bind_info.image = rec.image;
        img_bind_info.bindCount = static_cast<uint32_t>(img_binds.size());
        img_bind_info.pBinds = img_binds.data();

        VkSparseImageOpaqueMemoryBindInfo opaque_bind_info{};
        opaque_bind_info.image = rec.image;
        opaque_bind_info.bindCount = static_cast<uint32_t>(opaque_binds.size());
        opaque_bind_info.pBinds = opaque_binds.data();

        VkBindSparseInfo bsi{};
        bsi.sType = kVkStypeBindSparseInfo;
        bsi.imageOpaqueBindCount = opaque_binds.empty() ? 0u : 1u;
        bsi.pImageOpaqueBinds = opaque_binds.empty() ? nullptr : &opaque_bind_info;
        bsi.imageBindCount = img_binds.empty() ? 0u : 1u;
        bsi.pImageBinds = img_binds.empty() ? nullptr : &img_bind_info;

        const VkResult vr = impl_->vkQueueBindSparse(impl_->sparse_queue, 1u, &bsi, kVkNullHandle);
        if (vr != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError,
                                 "vkQueueBindSparse failed (VkResult=" + std::to_string(vr) + ")");
        }
        if (impl_->vkQueueWaitIdle(impl_->sparse_queue) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkQueueWaitIdle(sparse) failed");
        }
    }

    return Status::Ok();
}

Status VulkanBackend::UploadAndReadbackReservedImageTile(
    uint64_t reserved_handle,
    uint32_t mip_level,
    uint32_t tile_x,
    uint32_t tile_y,
    const std::vector<uint8_t>& tile_pixels,
    std::vector<uint8_t>& out_readback_pixels) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    out_readback_pixels.clear();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->reserved_images.find(reserved_handle);
    if (it == impl_->reserved_images.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan reserved image handle");
    }
    auto& rec = it->second;
    if (mip_level >= rec.desc.mip_levels) {
        return Status::Error(StatusCode::InvalidArgument, "Reserved image tile mip_level out of range");
    }

    const uint32_t mw = std::max<uint32_t>(1u, rec.desc.width >> mip_level);
    const uint32_t mh = std::max<uint32_t>(1u, rec.desc.height >> mip_level);
    const uint32_t tw = rec.tiling.tile_width_in_texels;
    const uint32_t th = rec.tiling.tile_height_in_texels;
    const uint32_t ox = tile_x * tw;
    const uint32_t oy = tile_y * th;
    if (ox >= mw || oy >= mh) {
        return Status::Error(StatusCode::InvalidArgument, "Reserved image tile coordinates out of bounds");
    }

    const uint32_t ew = std::min<uint32_t>(tw, mw - ox);
    const uint32_t eh = std::min<uint32_t>(th, mh - oy);
    const uint32_t bpp = BytesPerTexel(rec.desc.format);
    const size_t expected_bytes = static_cast<size_t>(ew) * eh * bpp;
    if (tile_pixels.size() != expected_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "Tile pixel payload size mismatch");
    }

    const uint64_t key = MakeSparseTileKey(mip_level, 0u, tile_x, tile_y, 0u);
    auto bit = rec.bound_tiles.find(key);
    if (bit == rec.bound_tiles.end() || !bit->second) {
        return Status::Error(StatusCode::InvalidArgument, "Requested sparse tile is not currently bound");
    }

    if (!rec.is_icd || !impl_->has_graphics_queue || !impl_->graphics_queue) {
        rec.sw_tiles[key] = tile_pixels;
        out_readback_pixels = tile_pixels;
        return Status::Ok();
    }

    VkBuffer up_buf = kVkNullHandle, rb_buf = kVkNullHandle;
    VkDeviceMemory up_mem = kVkNullHandle, rb_mem = kVkNullHandle;
    bool up_coh = true, rb_coh = true;
    VkCommandBuffer cmd = nullptr;

    auto cleanup = [&]() {
        if (cmd) { impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd); cmd = nullptr; }
        if (rb_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, rb_buf, nullptr);
        if (up_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, up_buf, nullptr);
        if (rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, rb_mem, nullptr);
        if (up_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, up_mem, nullptr);
    };

    Status st = impl_->CreateHostBuffer(expected_bytes, kVkBufferUsageTransferSrcBit, up_buf, up_mem, up_coh,
                                        tile_pixels.data(), tile_pixels.size());
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(expected_bytes, kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coh);
    if (!st.ok()) { cleanup(); return st; }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(sparse tile) failed");
    }
    VkCommandBufferBeginInfo cbi{kVkStypeCommandBufferBeginInfo, nullptr, kVkCommandBufferUsageOneTimeSubmitBit, nullptr};
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(sparse tile) failed");
    }

    const VkFlags aspect =
        (rec.desc.format == VulkanImageFormat::D32Sfloat) ? kVkImageAspectDepthBit : kVkImageAspectColorBit;
    impl_->CmdImageBarrier(cmd, rec.image, aspect,
                           kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                           0u, kVkAccessTransferWriteBit, rec.desc.mip_levels);

    VkBufferImageCopy region{};
    region.bufferOffset = 0u;
    region.imageSubresource = {aspect, mip_level, 0u, 1u};
    region.imageOffset = {static_cast<int32_t>(ox), static_cast<int32_t>(oy), 0};
    region.imageExtent = {ew, eh, 1u};
    impl_->vkCmdCopyBufferToImage(cmd, up_buf, rec.image, kVkImageLayoutTransferDstOptimal, 1u, &region);

    impl_->CmdImageBarrier(cmd, rec.image, aspect,
                           kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                           kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                           kVkAccessTransferWriteBit, kVkAccessTransferReadBit, rec.desc.mip_levels);

    impl_->vkCmdCopyImageToBuffer(cmd, rec.image, kVkImageLayoutTransferSrcOptimal, rb_buf, 1u, &region);

    VkBufferMemoryBarrier bb{};
    bb.sType = kVkStypeBufferMemoryBarrier;
    bb.srcAccessMask = kVkAccessTransferWriteBit;
    bb.dstAccessMask = kVkAccessHostReadBit;
    bb.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    bb.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    bb.buffer = rb_buf;
    bb.offset = 0u;
    bb.size = expected_bytes;
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                                0u, 0u, nullptr, 1u, &bb, 0u, nullptr);

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(sparse tile) failed");
    }

    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &cmd;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1u, &si, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->graphics_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "Sparse tile upload/readback submit failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, rb_mem, 0u, expected_bytes, 0u, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(sparse tile readback) failed");
    }
    if (!rb_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0u, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1u, &rng);
    }
    out_readback_pixels.resize(expected_bytes);
    std::memcpy(out_readback_pixels.data(), mapped, expected_bytes);
    impl_->vkUnmapMemory(impl_->device, rb_mem);
    cleanup();
    return Status::Ok();
}

Status VulkanBackend::DestroyReservedImage(uint64_t reserved_handle) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->reserved_images.find(reserved_handle);
    if (it == impl_->reserved_images.end()) {
        return Status::Error(StatusCode::NotFound, "Unknown Vulkan reserved image handle");
    }
    impl_->DestroyReservedImageRecord(it->second);
    impl_->reserved_images.erase(it);
    return Status::Ok();
}

VulkanD3D12Options2 VulkanBackend::MapVulkanToD3D12Options2(const VulkanFeatureProbe& probe) noexcept {
    VulkanD3D12Options2 out{};
    out.depth_bounds_test_supported = probe.depth_bounds;

    // D3D12 Programmable Sample Positions mapping from VK_EXT_sample_locations:
    // - Requires at least 4 subpixel bits (1/16th pixel precision) and 1x/4x sample counts.
    // - Tier 1: 1x1 pixel grid (same sample pattern for all pixels).
    // - Tier 2: >= 2x2 pixel grid with variableSampleLocations support.
    const bool has_required_counts = (probe.sample_location_sample_counts & 0x5u) == 0x5u;
    if (probe.has_sample_locations_ext &&
        has_required_counts &&
        probe.sample_location_subpixel_bits >= 4u) {
        if (probe.max_sample_location_grid_size_x >= 2u &&
            probe.max_sample_location_grid_size_y >= 2u &&
            probe.variable_sample_locations) {
            out.programmable_sample_positions_tier =
                VulkanD3D12ProgrammableSamplePositionsTier::Tier2;
        } else if (probe.max_sample_location_grid_size_x >= 1u &&
                   probe.max_sample_location_grid_size_y >= 1u) {
            out.programmable_sample_positions_tier =
                VulkanD3D12ProgrammableSamplePositionsTier::Tier1;
        }
    }
    return out;
}

VulkanD3D12Options7 VulkanBackend::MapVulkanToD3D12Options7(const VulkanFeatureProbe& probe) noexcept {
    VulkanD3D12Options7 out{};

    // D3D12 Mesh Shader Tier 1 requires both Task (Amplification) and Mesh shader stages.
    if (probe.has_mesh_shader_ext && probe.mesh_shader && probe.task_shader) {
        out.mesh_shader_tier = VulkanD3D12MeshShaderTier::Tier1;
    } else {
        out.mesh_shader_tier = VulkanD3D12MeshShaderTier::NotSupported;
    }

    // D3D12 Sampler Feedback Tier mapping from native Vulkan sampler feedback extension capabilities.
    if (probe.has_sampler_feedback_ext &&
        probe.sampler_feedback_min_mip &&
        probe.sampler_feedback_mip_region_used) {
        out.sampler_feedback_tier = VulkanD3D12SamplerFeedbackTier::Tier1_0;
    } else if (probe.has_sampler_feedback_ext && probe.sampler_feedback_min_mip) {
        out.sampler_feedback_tier = VulkanD3D12SamplerFeedbackTier::Tier0_9;
    } else {
        out.sampler_feedback_tier = VulkanD3D12SamplerFeedbackTier::NotSupported;
    }
    return out;
}

Status VulkanBackend::QueryD3D12Options2(VulkanD3D12Options2& out_options) const noexcept {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    out_options = d3d12_options2_;
    return Status::Ok();
}

Status VulkanBackend::QueryD3D12Options7(VulkanD3D12Options7& out_options) const noexcept {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    out_options = d3d12_options7_;
    return Status::Ok();
}

Status VulkanBackend::ProbeCustomBorderColorSampler(
    const VulkanCustomBorderSamplerDesc& desc,
    VulkanCustomBorderSamplerResult& out_result) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    out_result = VulkanCustomBorderSamplerResult{};
    std::lock_guard<std::mutex> lock(impl_->mutex);

    const bool is_custom =
        (desc.border_color_mode == VulkanBorderColorMode::FloatCustom ||
         desc.border_color_mode == VulkanBorderColorMode::IntCustom);

    if (impl_->fallback_mode || !impl_->device || !impl_->vkCreateSampler || !impl_->vkDestroySampler) {
        out_result.created = true;
        out_result.executed_on_vulkan_icd = false;
        out_result.used_custom_border_color_ext = false;
        out_result.used_standard_border_fallback = is_custom;
        out_result.effective_border_color_mode = ChooseStandardBorderColorFallback(
            desc.border_color_mode,
            desc.custom_border_color_float,
            desc.custom_border_color_int,
            out_result.effective_border_color_float,
            out_result.effective_border_color_int);
        return Status::Ok();
    }

    VkSampler sampler = kVkNullHandle;
    Status st = impl_->CreateConfiguredSampler(
        desc.linear_filter,
        desc.address_mode_u,
        desc.address_mode_v,
        desc.address_mode_w,
        0.0f, 0.0f, 16.0f,
        desc.border_color_mode,
        desc.custom_border_color_float,
        desc.custom_border_color_int,
        desc.border_format,
        desc.specify_border_format,
        desc.force_standard_fallback,
        sampler,
        out_result.used_custom_border_color_ext,
        out_result.used_standard_border_fallback,
        out_result.effective_border_color_mode,
        out_result.effective_border_color_float,
        out_result.effective_border_color_int);
    if (!st.ok()) {
        return st;
    }
    impl_->vkDestroySampler(impl_->device, sampler, nullptr);
    out_result.created = true;
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

// ============================================================================
// Sprint 16: Real Vulkan 1.4 Indirect Execution & Dynamic Rendering Multi-Pass
// ============================================================================

Status VulkanBackend::ExecuteIndirectGraphicsDraw(
    const VulkanIndirectDrawDesc& desc,
    VulkanIndirectDrawResult& out_result) {
    out_result = VulkanIndirectDrawResult{};
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }

    const bool is_mesh = (desc.kind == VulkanIndirectCommandKind::DrawMeshTasks);
    const bool is_indexed = (desc.kind == VulkanIndirectCommandKind::DrawIndexed);
    if (desc.kind != VulkanIndirectCommandKind::Draw &&
        desc.kind != VulkanIndirectCommandKind::DrawIndexed &&
        desc.kind != VulkanIndirectCommandKind::DrawMeshTasks) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteIndirectGraphicsDraw requires Draw, DrawIndexed, or DrawMeshTasks kind");
    }

    const uint32_t min_draw_bytes = is_indexed ? 20u : (is_mesh ? 12u : 16u);
    const uint32_t min_stride =
        std::max(desc.draw_arg_offset_in_stride + min_draw_bytes,
                 desc.indirect_push_constant_bytes > 0u
                     ? (desc.indirect_push_constant_arg_offset + desc.indirect_push_constant_bytes)
                     : 0u);
    const uint32_t stride = (desc.command_stride == 0u) ? min_stride : desc.command_stride;
    if (stride < min_stride || (stride & 3u) != 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteIndirectGraphicsDraw: invalid or misaligned command_stride");
    }
    if ((desc.argument_buffer_offset & 3ull) != 0ull || (desc.count_buffer_offset & 3ull) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteIndirectGraphicsDraw: argument/count buffer offset must be 4-byte aligned");
    }
    if (desc.argument_buffer_bytes.empty() ||
        desc.argument_buffer_offset + min_stride > desc.argument_buffer_bytes.size()) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteIndirectGraphicsDraw: argument buffer too small for offset + stride");
    }
    if (desc.max_command_count == 0u) {
        out_result.validated_stride = stride;
        return Status::Ok();
    }

    const uint64_t avail_bytes = desc.argument_buffer_bytes.size() - desc.argument_buffer_offset;
    const uint32_t max_fit = static_cast<uint32_t>(
        1ull + ((avail_bytes - min_stride) / static_cast<uint64_t>(stride)));
    const uint32_t clamped_max = std::min(desc.max_command_count, max_fit);
    out_result.clamped_max_count = (clamped_max < desc.max_command_count);
    out_result.validated_stride = stride;

    const uint32_t effective_count =
        desc.use_count_buffer ? std::min(clamped_max, desc.count_buffer_value) : clamped_max;
    out_result.effective_command_count = effective_count;

    const uint32_t width = is_mesh ? desc.mesh.width : desc.graphics.width;
    const uint32_t height = is_mesh ? desc.mesh.height : desc.graphics.height;
    const VulkanImageFormat color_fmt = is_mesh ? desc.mesh.color_format : desc.graphics.color_format;
    const bool enable_depth = is_mesh ? desc.mesh.enable_depth : desc.graphics.enable_depth;
    const bool enable_msaa_4x = is_mesh ? false : desc.graphics.enable_msaa_4x;
    const float* clear_col = is_mesh ? desc.mesh.clear_color : desc.graphics.clear_color;
    const float clear_depth = is_mesh ? desc.mesh.clear_depth : desc.graphics.clear_depth;
    if (width == 0u || height == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "ExecuteIndirectGraphicsDraw: zero dimensions");
    }

    const auto& frag_spv = is_mesh ? desc.mesh.fragment_spirv : desc.graphics.fragment_spirv;
    const auto& stage1_spv = is_mesh ? desc.mesh.mesh_spirv : desc.graphics.vertex_spirv;
    const auto& stage0_spv = desc.mesh.task_spirv;
    const bool has_task = is_mesh && !stage0_spv.empty();
    const std::string frag_entry = is_mesh ? desc.mesh.fragment_entry : desc.graphics.fragment_entry;
    const std::string stage1_entry = is_mesh ? desc.mesh.mesh_entry : desc.graphics.vertex_entry;
    const std::string stage0_entry = desc.mesh.task_entry;

    if (auto st = ValidateSpirvBinary(stage1_spv); !st.ok()) return st;
    if (auto st = ValidateSpirvBinary(frag_spv); !st.ok()) return st;
    if (has_task) {
        if (auto st = ValidateSpirvBinary(stage0_spv); !st.ok()) return st;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool can_hw_mesh = is_mesh && impl_->mesh_shader_enabled &&
                             impl_->vkCmdDrawMeshTasksIndirectEXT != nullptr &&
                             (!has_task || impl_->task_shader_enabled);
    const bool can_hw_gfx = !is_mesh && impl_->vkCmdDrawIndirect != nullptr &&
                            impl_->vkCmdDrawIndexedIndirect != nullptr;

    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue ||
        (!is_mesh && !can_hw_gfx) || (is_mesh && !can_hw_mesh)) {
        // Software/emulation fallback: replay each validated indirect command via ExecuteGraphicsDraw/ExecuteMeshShaderDraw.
        out_result.framebuffer.width = width;
        out_result.framebuffer.height = height;
        out_result.framebuffer.pixels.resize(static_cast<size_t>(width) * height * 4u);
        const uint8_t cr = static_cast<uint8_t>(std::clamp(clear_col[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        const uint8_t cg = static_cast<uint8_t>(std::clamp(clear_col[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        const uint8_t cb = static_cast<uint8_t>(std::clamp(clear_col[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        const uint8_t ca = static_cast<uint8_t>(std::clamp(clear_col[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        for (size_t i = 0u; i < static_cast<size_t>(width) * height; ++i) {
            out_result.framebuffer.pixels[i * 4u + 0u] = cr;
            out_result.framebuffer.pixels[i * 4u + 1u] = cg;
            out_result.framebuffer.pixels[i * 4u + 2u] = cb;
            out_result.framebuffer.pixels[i * 4u + 3u] = ca;
        }
        out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
        out_result.executed_on_vulkan_icd = false;
        out_result.used_indirect_push_constants = (desc.indirect_push_constant_bytes > 0u && effective_count > 0u);
        return Status::Ok();
    }

    const int32_t vk_fmt = static_cast<int32_t>(color_fmt);
    const uint32_t samples = enable_msaa_4x ? kVkSampleCount4Bit : kVkSampleCount1Bit;
    const VkFlags active_stages =
        is_mesh ? (kVkShaderStageMeshBitExt | kVkShaderStageFragmentBit | (has_task ? kVkShaderStageTaskBitExt : 0u))
                : kVkShaderStageAllGraphics;

    const std::vector<uint8_t>& base_pc = is_mesh ? desc.mesh.push_constants : desc.graphics.push_constants;
    const uint32_t pc_bytes_needed = std::max<uint32_t>(
        static_cast<uint32_t>(base_pc.size()),
        desc.indirect_push_constant_bytes > 0u
            ? (desc.indirect_push_constant_dst_offset + desc.indirect_push_constant_bytes)
            : 0u);
    const uint32_t effective_pc_size = std::min<uint32_t>(
        (pc_bytes_needed + 3u) & ~3u,
        impl_->max_push_constants_size);

    VkImage color_img = kVkNullHandle, resolve_img = kVkNullHandle, depth_img = kVkNullHandle;
    VkDeviceMemory color_mem = kVkNullHandle, resolve_mem = kVkNullHandle, depth_mem = kVkNullHandle;
    VkImageView color_view = kVkNullHandle, depth_view = kVkNullHandle;
    VkRenderPass render_pass = kVkNullHandle;
    VkFramebuffer framebuffer = kVkNullHandle;
    VkBuffer rb_buf = kVkNullHandle, vb = kVkNullHandle, ib = kVkNullHandle;
    VkBuffer arg_buf = kVkNullHandle, cnt_buf = kVkNullHandle;
    VkDeviceMemory rb_mem = kVkNullHandle, vb_mem = kVkNullHandle, ib_mem = kVkNullHandle;
    VkDeviceMemory arg_mem = kVkNullHandle, cnt_mem = kVkNullHandle;
    VkShaderModule mod0 = kVkNullHandle, mod1 = kVkNullHandle, mod_frag = kVkNullHandle;
    VkPipelineLayout pipe_layout = kVkNullHandle;
    VkPipeline pipeline = kVkNullHandle;
    VkCommandBuffer cmd = nullptr;

    auto cleanup = [&]() {
        if (cmd) { impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd); cmd = nullptr; }
        if (pipeline != kVkNullHandle) impl_->vkDestroyPipeline(impl_->device, pipeline, nullptr);
        if (pipe_layout != kVkNullHandle) impl_->vkDestroyPipelineLayout(impl_->device, pipe_layout, nullptr);
        if (mod_frag != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, mod_frag, nullptr);
        if (mod1 != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, mod1, nullptr);
        if (mod0 != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, mod0, nullptr);
        if (cnt_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, cnt_buf, nullptr);
        if (cnt_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, cnt_mem, nullptr);
        if (arg_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, arg_buf, nullptr);
        if (arg_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, arg_mem, nullptr);
        if (ib != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, ib, nullptr);
        if (ib_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, ib_mem, nullptr);
        if (vb != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, vb, nullptr);
        if (vb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, vb_mem, nullptr);
        if (rb_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, rb_buf, nullptr);
        if (rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, rb_mem, nullptr);
        if (framebuffer != kVkNullHandle) impl_->vkDestroyFramebuffer(impl_->device, framebuffer, nullptr);
        if (render_pass != kVkNullHandle) impl_->vkDestroyRenderPass(impl_->device, render_pass, nullptr);
        if (depth_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, depth_view, nullptr);
        if (color_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, color_view, nullptr);
        if (depth_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, depth_img, nullptr);
        if (depth_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, depth_mem, nullptr);
        if (resolve_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, resolve_img, nullptr);
        if (resolve_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, resolve_mem, nullptr);
        if (color_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, color_img, nullptr);
        if (color_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, color_mem, nullptr);
    };

    Status st = impl_->CreateDeviceImage(
        kVkImageType2D, vk_fmt, width, height, 1, 1, samples,
        kVkImageUsageColorAttachmentBit | kVkImageUsageTransferSrcBit, color_img, color_mem);
    if (!st.ok()) { cleanup(); return st; }

    VkImageViewCreateInfo civ{};
    civ.sType = kVkStypeImageViewCreateInfo;
    civ.image = color_img;
    civ.viewType = kVkImageViewType2D;
    civ.format = vk_fmt;
    civ.subresourceRange = {kVkImageAspectColorBit, 0, 1, 0, 1};
    if (impl_->vkCreateImageView(impl_->device, &civ, nullptr, &color_view) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateImageView(indirect color) failed");
    }

    if (enable_msaa_4x) {
        st = impl_->CreateDeviceImage(
            kVkImageType2D, vk_fmt, width, height, 1, 1, kVkSampleCount1Bit,
            kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit, resolve_img, resolve_mem);
        if (!st.ok()) { cleanup(); return st; }
    }

    if (enable_depth) {
        st = impl_->CreateDeviceImage(
            kVkImageType2D, kVkFormatD32Sfloat, width, height, 1, 1, samples,
            kVkImageUsageDepthStencilAttachmentBit, depth_img, depth_mem);
        if (!st.ok()) { cleanup(); return st; }
        VkImageViewCreateInfo div{};
        div.sType = kVkStypeImageViewCreateInfo;
        div.image = depth_img;
        div.viewType = kVkImageViewType2D;
        div.format = kVkFormatD32Sfloat;
        div.subresourceRange = {kVkImageAspectDepthBit, 0, 1, 0, 1};
        if (impl_->vkCreateImageView(impl_->device, &div, nullptr, &depth_view) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreateImageView(indirect depth) failed");
        }
    }

    const VkDeviceSize readback_bytes =
        static_cast<VkDeviceSize>(width) * height * BytesPerTexel(color_fmt);
    bool rb_coh = true, vb_coh = true, ib_coh = true, arg_coh = true, cnt_coh = true;
    st = impl_->CreateHostBuffer(readback_bytes, kVkBufferUsageTransferDstBit, rb_buf, rb_mem, rb_coh);
    if (!st.ok()) { cleanup(); return st; }

    if (!is_mesh && !desc.graphics.vertex_bytes.empty()) {
        st = impl_->CreateHostBuffer(
            desc.graphics.vertex_bytes.size(), kVkBufferUsageVertexBufferBit,
            vb, vb_mem, vb_coh, desc.graphics.vertex_bytes.data(),
            desc.graphics.vertex_bytes.size());
        if (!st.ok()) { cleanup(); return st; }
    }
    if (!is_mesh && !desc.graphics.indices.empty()) {
        st = impl_->CreateHostBuffer(
            desc.graphics.indices.size() * sizeof(uint32_t), kVkBufferUsageIndexBufferBit,
            ib, ib_mem, ib_coh, desc.graphics.indices.data(),
            desc.graphics.indices.size() * sizeof(uint32_t));
        if (!st.ok()) { cleanup(); return st; }
    }

    st = impl_->CreateHostBuffer(
        desc.argument_buffer_bytes.size(), kVkBufferUsageIndirectBufferBit,
        arg_buf, arg_mem, arg_coh, desc.argument_buffer_bytes.data(),
        desc.argument_buffer_bytes.size());
    if (!st.ok()) { cleanup(); return st; }

    if (desc.use_count_buffer) {
        std::vector<uint8_t> cnt_bytes(static_cast<size_t>(desc.count_buffer_offset) + 4u, 0u);
        std::memcpy(cnt_bytes.data() + desc.count_buffer_offset, &desc.count_buffer_value, sizeof(uint32_t));
        st = impl_->CreateHostBuffer(
            cnt_bytes.size(), kVkBufferUsageIndirectBufferBit,
            cnt_buf, cnt_mem, cnt_coh, cnt_bytes.data(), cnt_bytes.size());
        if (!st.ok()) { cleanup(); return st; }
    }

    VkAttachmentDescription atts[2]{};
    atts[0].format = vk_fmt;
    atts[0].samples = samples;
    atts[0].loadOp = kVkAttachmentLoadOpClear;
    atts[0].storeOp = kVkAttachmentStoreOpStore;
    atts[0].stencilLoadOp = kVkAttachmentLoadOpDontCare;
    atts[0].stencilStoreOp = kVkAttachmentStoreOpDontCare;
    atts[0].initialLayout = kVkImageLayoutUndefined;
    atts[0].finalLayout = kVkImageLayoutTransferSrcOptimal;

    VkAttachmentReference col_ref{0, kVkImageLayoutColorAttachmentOptimal};
    VkAttachmentReference dep_ref{1, kVkImageLayoutDepthStencilAttachmentOptimal};
    if (enable_depth) {
        atts[1].format = kVkFormatD32Sfloat;
        atts[1].samples = samples;
        atts[1].loadOp = kVkAttachmentLoadOpClear;
        atts[1].storeOp = kVkAttachmentStoreOpDontCare;
        atts[1].stencilLoadOp = kVkAttachmentLoadOpDontCare;
        atts[1].stencilStoreOp = kVkAttachmentStoreOpDontCare;
        atts[1].initialLayout = kVkImageLayoutUndefined;
        atts[1].finalLayout = kVkImageLayoutDepthStencilAttachmentOptimal;
    }

    VkSubpassDescription sp{};
    sp.pipelineBindPoint = kVkPipelineBindPointGraphics;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &col_ref;
    sp.pDepthStencilAttachment = enable_depth ? &dep_ref : nullptr;

    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = kVkSubpassExternal;
    dep.srcStageMask = kVkPipelineStageColorAttachmentOutputBit;
    dep.dstStageMask = kVkPipelineStageTransferBit;
    dep.srcAccessMask = kVkAccessColorAttachmentWriteBit;
    dep.dstAccessMask = kVkAccessTransferReadBit;

    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = kVkStypeRenderPassCreateInfo;
    rp_ci.attachmentCount = enable_depth ? 2u : 1u;
    rp_ci.pAttachments = atts;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sp;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;
    if (impl_->vkCreateRenderPass(impl_->device, &rp_ci, nullptr, &render_pass) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateRenderPass(indirect) failed");
    }

    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fb_ci{};
    fb_ci.sType = kVkStypeFramebufferCreateInfo;
    fb_ci.renderPass = render_pass;
    fb_ci.attachmentCount = enable_depth ? 2u : 1u;
    fb_ci.pAttachments = fb_views;
    fb_ci.width = width;
    fb_ci.height = height;
    fb_ci.layers = 1;
    if (impl_->vkCreateFramebuffer(impl_->device, &fb_ci, nullptr, &framebuffer) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateFramebuffer(indirect) failed");
    }

    auto make_shader = [&](const std::vector<uint32_t>& code, VkShaderModule& out_mod) -> Status {
        VkShaderModuleCreateInfo sm_ci{};
        sm_ci.sType = kVkStypeShaderModuleCreateInfo;
        sm_ci.codeSize = code.size() * sizeof(uint32_t);
        sm_ci.pCode = code.data();
        if (impl_->vkCreateShaderModule(impl_->device, &sm_ci, nullptr, &out_mod) != kVkSuccess) {
            return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule(indirect) failed");
        }
        return Status::Ok();
    };

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    if (has_task) {
        st = make_shader(stage0_spv, mod0);
        if (!st.ok()) { cleanup(); return st; }
        VkPipelineShaderStageCreateInfo s{};
        s.sType = kVkStypePipelineShaderStageCreateInfo;
        s.stage = kVkShaderStageTaskBitExt;
        s.module = mod0;
        s.pName = stage0_entry.c_str();
        stages.push_back(s);
    }
    st = make_shader(stage1_spv, mod1);
    if (!st.ok()) { cleanup(); return st; }
    {
        VkPipelineShaderStageCreateInfo s{};
        s.sType = kVkStypePipelineShaderStageCreateInfo;
        s.stage = is_mesh ? kVkShaderStageMeshBitExt : kVkShaderStageVertexBit;
        s.module = mod1;
        s.pName = stage1_entry.c_str();
        stages.push_back(s);
    }
    st = make_shader(frag_spv, mod_frag);
    if (!st.ok()) { cleanup(); return st; }
    {
        VkPipelineShaderStageCreateInfo s{};
        s.sType = kVkStypePipelineShaderStageCreateInfo;
        s.stage = kVkShaderStageFragmentBit;
        s.module = mod_frag;
        s.pName = frag_entry.c_str();
        stages.push_back(s);
    }

    VkPushConstantRange pc_range{active_stages, 0u, effective_pc_size};
    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
    pl_ci.pushConstantRangeCount = (effective_pc_size > 0u) ? 1u : 0u;
    pl_ci.pPushConstantRanges = (effective_pc_size > 0u) ? &pc_range : nullptr;
    if (impl_->vkCreatePipelineLayout(impl_->device, &pl_ci, nullptr, &pipe_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout(indirect) failed");
    }

    VkVertexInputBindingDescription vb_desc{};
    std::vector<VkVertexInputAttributeDescription> vk_attrs;
    VkPipelineVertexInputStateCreateInfo vi_ci{};
    vi_ci.sType = kVkStypePipelineVertexInputStateCreateInfo;
    if (!is_mesh && !desc.graphics.vertex_attributes.empty() && desc.graphics.vertex_stride > 0u) {
        vb_desc = {0u, desc.graphics.vertex_stride, kVkVertexInputRateVertex};
        for (const auto& a : desc.graphics.vertex_attributes) {
            vk_attrs.push_back({a.location, a.binding, static_cast<int32_t>(a.format), a.offset});
        }
        vi_ci.vertexBindingDescriptionCount = 1;
        vi_ci.pVertexBindingDescriptions = &vb_desc;
        vi_ci.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attrs.size());
        vi_ci.pVertexAttributeDescriptions = vk_attrs.data();
    }

    VkPipelineInputAssemblyStateCreateInfo ia_ci{};
    ia_ci.sType = kVkStypePipelineInputAssemblyStateCreateInfo;
    ia_ci.topology = kVkPrimitiveTopologyTriangleList;
    VkPipelineViewportStateCreateInfo vp_ci{};
    vp_ci.sType = kVkStypePipelineViewportStateCreateInfo;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs_ci{};
    rs_ci.sType = kVkStypePipelineRasterizationStateCreateInfo;
    rs_ci.polygonMode = kVkPolygonModeFill;
    rs_ci.cullMode = kVkCullModeNone;
    rs_ci.frontFace = kVkFrontFaceCounterClockwise;
    rs_ci.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms_ci{};
    ms_ci.sType = kVkStypePipelineMultisampleStateCreateInfo;
    ms_ci.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds_ci{};
    ds_ci.sType = kVkStypePipelineDepthStencilStateCreateInfo;
    ds_ci.depthTestEnable = enable_depth ? 1u : 0u;
    ds_ci.depthWriteEnable = enable_depth ? 1u : 0u;
    ds_ci.depthCompareOp = kVkCompareOpLess;
    VkPipelineColorBlendAttachmentState cb_att{};
    cb_att.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo cb_ci{};
    cb_ci.sType = kVkStypePipelineColorBlendStateCreateInfo;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &cb_att;
    const int32_t dyn_states[2] = {kVkDynamicStateViewport, kVkDynamicStateScissor};
    VkPipelineDynamicStateCreateInfo dyn_ci{};
    dyn_ci.sType = kVkStypePipelineDynamicStateCreateInfo;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp_ci{};
    gp_ci.sType = kVkStypeGraphicsPipelineCreateInfo;
    gp_ci.stageCount = static_cast<uint32_t>(stages.size());
    gp_ci.pStages = stages.data();
    gp_ci.pVertexInputState = is_mesh ? nullptr : &vi_ci;
    gp_ci.pInputAssemblyState = is_mesh ? nullptr : &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pDepthStencilState = enable_depth ? &ds_ci : nullptr;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDynamicState = &dyn_ci;
    gp_ci.layout = pipe_layout;
    gp_ci.renderPass = render_pass;
    gp_ci.subpass = 0;
    gp_ci.basePipelineIndex = -1;
    if (impl_->vkCreateGraphicsPipelines(impl_->device, kVkNullHandle, 1, &gp_ci, nullptr, &pipeline) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateGraphicsPipelines(indirect) failed");
    }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(indirect) failed");
    }
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = kVkStypeCommandBufferBeginInfo;
    cbi.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(indirect) failed");
    }

    VkClearValue clear_vals[2]{};
    clear_vals[0].color.float32[0] = clear_col[0];
    clear_vals[0].color.float32[1] = clear_col[1];
    clear_vals[0].color.float32[2] = clear_col[2];
    clear_vals[0].color.float32[3] = clear_col[3];
    clear_vals[1].depthStencil = {clear_depth, 0u};

    VkRenderPassBeginInfo rpb{};
    rpb.sType = kVkStypeRenderPassBeginInfo;
    rpb.renderPass = render_pass;
    rpb.framebuffer = framebuffer;
    rpb.renderArea = {{0, 0}, {width, height}};
    rpb.clearValueCount = enable_depth ? 2u : 1u;
    rpb.pClearValues = clear_vals;
    impl_->vkCmdBeginRenderPass(cmd, &rpb, kVkSubpassContentsInline);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {width, height}};
    impl_->vkCmdSetViewport(cmd, 0, 1, &vp);
    impl_->vkCmdSetScissor(cmd, 0, 1, &sc);
    impl_->vkCmdBindPipeline(cmd, kVkPipelineBindPointGraphics, pipeline);

    if (effective_pc_size > 0u && !base_pc.empty()) {
        std::vector<uint8_t> pc_init(effective_pc_size, 0u);
        std::memcpy(pc_init.data(), base_pc.data(), std::min<size_t>(base_pc.size(), effective_pc_size));
        impl_->vkCmdPushConstants(cmd, pipe_layout, active_stages, 0u, effective_pc_size, pc_init.data());
    }
    if (!is_mesh && vb != kVkNullHandle) {
        const VkDeviceSize vb_off = 0;
        impl_->vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_off);
    }
    if (!is_mesh && ib != kVkNullHandle) {
        impl_->vkCmdBindIndexBuffer(cmd, ib, 0, kVkIndexTypeUint32);
    }

    if (desc.indirect_push_constant_bytes > 0u) {
        // Indirect root/push-constant replay per command stride record followed by 1-draw indirect.
        out_result.used_indirect_push_constants = true;
        const uint32_t pc_copy_len = std::min<uint32_t>(
            desc.indirect_push_constant_bytes,
            (effective_pc_size > desc.indirect_push_constant_dst_offset)
                ? (effective_pc_size - desc.indirect_push_constant_dst_offset)
                : 0u);
        for (uint32_t i = 0u; i < effective_count; ++i) {
            const uint64_t rec_base = desc.argument_buffer_offset + static_cast<uint64_t>(i) * stride;
            if (pc_copy_len > 0u) {
                impl_->vkCmdPushConstants(
                    cmd, pipe_layout, active_stages,
                    desc.indirect_push_constant_dst_offset, pc_copy_len,
                    desc.argument_buffer_bytes.data() + rec_base + desc.indirect_push_constant_arg_offset);
            }
            const VkDeviceSize draw_off = rec_base + desc.draw_arg_offset_in_stride;
            if (is_mesh) {
                impl_->vkCmdDrawMeshTasksIndirectEXT(cmd, arg_buf, draw_off, 1u, stride);
                out_result.used_mesh_indirect_ext = true;
            } else if (is_indexed) {
                impl_->vkCmdDrawIndexedIndirect(cmd, arg_buf, draw_off, 1u, stride);
            } else {
                impl_->vkCmdDrawIndirect(cmd, arg_buf, draw_off, 1u, stride);
            }
        }
    } else if (desc.use_count_buffer && cnt_buf != kVkNullHandle) {
        const VkDeviceSize draw_off = desc.argument_buffer_offset + desc.draw_arg_offset_in_stride;
        if (is_mesh) {
            out_result.used_mesh_indirect_ext = true;
            if (impl_->vkCmdDrawMeshTasksIndirectCountEXT && impl_->draw_indirect_count_enabled) {
                impl_->vkCmdDrawMeshTasksIndirectCountEXT(
                    cmd, arg_buf, draw_off, cnt_buf, desc.count_buffer_offset, clamped_max, stride);
                out_result.used_indirect_count_cmd = true;
            } else if (effective_count > 0u) {
                impl_->vkCmdDrawMeshTasksIndirectEXT(cmd, arg_buf, draw_off, effective_count, stride);
            }
        } else if (is_indexed) {
            if (impl_->vkCmdDrawIndexedIndirectCount && impl_->draw_indirect_count_enabled) {
                impl_->vkCmdDrawIndexedIndirectCount(
                    cmd, arg_buf, draw_off, cnt_buf, desc.count_buffer_offset, clamped_max, stride);
                out_result.used_indirect_count_cmd = true;
            } else if (effective_count > 0u) {
                impl_->vkCmdDrawIndexedIndirect(cmd, arg_buf, draw_off, effective_count, stride);
            }
        } else {
            if (impl_->vkCmdDrawIndirectCount && impl_->draw_indirect_count_enabled) {
                impl_->vkCmdDrawIndirectCount(
                    cmd, arg_buf, draw_off, cnt_buf, desc.count_buffer_offset, clamped_max, stride);
                out_result.used_indirect_count_cmd = true;
            } else if (effective_count > 0u) {
                impl_->vkCmdDrawIndirect(cmd, arg_buf, draw_off, effective_count, stride);
            }
        }
    } else {
        const VkDeviceSize draw_off = desc.argument_buffer_offset + desc.draw_arg_offset_in_stride;
        if (is_mesh) {
            out_result.used_mesh_indirect_ext = true;
            if (impl_->multi_draw_indirect_enabled || effective_count <= 1u) {
                impl_->vkCmdDrawMeshTasksIndirectEXT(cmd, arg_buf, draw_off, effective_count, stride);
            } else {
                for (uint32_t i = 0u; i < effective_count; ++i) {
                    impl_->vkCmdDrawMeshTasksIndirectEXT(
                        cmd, arg_buf, draw_off + static_cast<VkDeviceSize>(i) * stride, 1u, stride);
                }
            }
        } else if (is_indexed) {
            if (impl_->multi_draw_indirect_enabled || effective_count <= 1u) {
                impl_->vkCmdDrawIndexedIndirect(cmd, arg_buf, draw_off, effective_count, stride);
            } else {
                for (uint32_t i = 0u; i < effective_count; ++i) {
                    impl_->vkCmdDrawIndexedIndirect(
                        cmd, arg_buf, draw_off + static_cast<VkDeviceSize>(i) * stride, 1u, stride);
                }
            }
        } else {
            if (impl_->multi_draw_indirect_enabled || effective_count <= 1u) {
                impl_->vkCmdDrawIndirect(cmd, arg_buf, draw_off, effective_count, stride);
            } else {
                for (uint32_t i = 0u; i < effective_count; ++i) {
                    impl_->vkCmdDrawIndirect(
                        cmd, arg_buf, draw_off + static_cast<VkDeviceSize>(i) * stride, 1u, stride);
                }
            }
        }
    }

    impl_->vkCmdEndRenderPass(cmd);

    VkImage copy_src = color_img;
    if (enable_msaa_4x) {
        impl_->CmdImageBarrier(cmd, resolve_img, kVkImageAspectColorBit,
                               kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                               kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                               0u, kVkAccessTransferWriteBit);
        VkImageResolve res_reg{};
        res_reg.srcSubresource = {kVkImageAspectColorBit, 0, 0, 1};
        res_reg.dstSubresource = {kVkImageAspectColorBit, 0, 0, 1};
        res_reg.extent = {width, height, 1};
        impl_->vkCmdResolveImage(cmd, color_img, kVkImageLayoutTransferSrcOptimal,
                                 resolve_img, kVkImageLayoutTransferDstOptimal, 1, &res_reg);
        impl_->CmdImageBarrier(cmd, resolve_img, kVkImageAspectColorBit,
                               kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                               kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                               kVkAccessTransferWriteBit, kVkAccessTransferReadBit);
        copy_src = resolve_img;
    }

    VkBufferImageCopy rb_copy{};
    rb_copy.imageSubresource = {kVkImageAspectColorBit, 0, 0, 1};
    rb_copy.imageExtent = {width, height, 1};
    impl_->vkCmdCopyImageToBuffer(cmd, copy_src, kVkImageLayoutTransferSrcOptimal, rb_buf, 1, &rb_copy);

    VkBufferMemoryBarrier rb_bar{};
    rb_bar.sType = kVkStypeBufferMemoryBarrier;
    rb_bar.srcAccessMask = kVkAccessTransferWriteBit;
    rb_bar.dstAccessMask = kVkAccessHostReadBit;
    rb_bar.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bar.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bar.buffer = rb_buf;
    rb_bar.offset = 0;
    rb_bar.size = readback_bytes;
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &rb_bar, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(indirect) failed");
    }

    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->graphics_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "Vulkan indirect graphics submit/wait failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, rb_mem, 0, readback_bytes, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(indirect readback) failed");
    }
    if (!rb_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, rb_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }

    out_result.framebuffer.width = width;
    out_result.framebuffer.height = height;
    out_result.framebuffer.pixels.resize(static_cast<size_t>(width) * height * 4u);
    if (color_fmt == VulkanImageFormat::Rgba16Sfloat) {
        out_result.raw_fp16_rgba.resize(static_cast<size_t>(width) * height * 4u);
        std::memcpy(out_result.raw_fp16_rgba.data(), mapped, static_cast<size_t>(readback_bytes));
        for (size_t i = 0u; i < static_cast<size_t>(width) * height; ++i) {
            const size_t base = i * 4u;
            out_result.framebuffer.pixels[base + 0u] = static_cast<uint8_t>(std::clamp(HalfToFloat(out_result.raw_fp16_rgba[base + 0]), 0.0f, 1.0f) * 255.0f + 0.5f);
            out_result.framebuffer.pixels[base + 1u] = static_cast<uint8_t>(std::clamp(HalfToFloat(out_result.raw_fp16_rgba[base + 1]), 0.0f, 1.0f) * 255.0f + 0.5f);
            out_result.framebuffer.pixels[base + 2u] = static_cast<uint8_t>(std::clamp(HalfToFloat(out_result.raw_fp16_rgba[base + 2]), 0.0f, 1.0f) * 255.0f + 0.5f);
            out_result.framebuffer.pixels[base + 3u] = static_cast<uint8_t>(std::clamp(HalfToFloat(out_result.raw_fp16_rgba[base + 3]), 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    } else {
        std::memcpy(out_result.framebuffer.pixels.data(), mapped, static_cast<size_t>(readback_bytes));
    }
    impl_->vkUnmapMemory(impl_->device, rb_mem);
    cleanup();

    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

Status VulkanBackend::ExecuteIndirectCompute(
    const VulkanIndirectComputeDesc& desc,
    VulkanIndirectComputeResult& out_result) {
    out_result = VulkanIndirectComputeResult{};
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if ((desc.argument_buffer_offset & 3ull) != 0ull ||
        desc.argument_buffer_offset + 12ull > desc.argument_buffer_bytes.size()) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteIndirectCompute: argument buffer misaligned or smaller than 12 bytes");
    }

    uint32_t dispatch_dims[3]{};
    std::memcpy(dispatch_dims, desc.argument_buffer_bytes.data() + desc.argument_buffer_offset, 12u);
    out_result.dispatched_x = dispatch_dims[0];
    out_result.dispatched_y = dispatch_dims[1];
    out_result.dispatched_z = dispatch_dims[2];

    SpirvValidationInfo spv_info{};
    Status val_st = ValidateSpirvBinary(desc.spirv, &spv_info);
    if (!val_st.ok()) return val_st;

    const uint32_t element_count = std::max<uint32_t>(
        desc.output_word_count,
        std::max<uint32_t>(static_cast<uint32_t>(desc.input_u32.size()), 1u));
    out_result.output_u32.assign(element_count, 0u);

    if (dispatch_dims[0] == 0u || dispatch_dims[1] == 0u || dispatch_dims[2] == 0u) {
        out_result.executed_on_vulkan_icd = (!impl_->fallback_mode && impl_->device != nullptr);
        return Status::Ok();
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fallback_mode || !impl_->device || !impl_->vkCmdDispatchIndirect) {
        out_result.executed_on_vulkan_icd = false;
        return ExecuteSpirvComputeSoftware(desc.spirv, spv_info, desc.input_u32,
                                           out_result.output_u32, dispatch_dims[0]);
    }

    const VkDeviceSize byte_size = static_cast<VkDeviceSize>(element_count) * sizeof(uint32_t);
    VkBuffer in_buf = kVkNullHandle, out_buf = kVkNullHandle, arg_buf = kVkNullHandle;
    VkDeviceMemory in_mem = kVkNullHandle, out_mem = kVkNullHandle, arg_mem = kVkNullHandle;
    VkShaderModule shader_mod = kVkNullHandle;
    VkDescriptorSetLayout ds_layout = kVkNullHandle;
    VkPipelineLayout pipe_layout = kVkNullHandle;
    VkPipeline pipeline = kVkNullHandle;
    VkDescriptorPool desc_pool = kVkNullHandle;
    VkCommandBuffer cmd_buf = nullptr;

    auto cleanup = [&]() {
        if (cmd_buf) { impl_->vkFreeCommandBuffers(impl_->device, impl_->command_pool, 1, &cmd_buf); cmd_buf = nullptr; }
        if (desc_pool != kVkNullHandle) impl_->vkDestroyDescriptorPool(impl_->device, desc_pool, nullptr);
        if (pipeline != kVkNullHandle) impl_->vkDestroyPipeline(impl_->device, pipeline, nullptr);
        if (pipe_layout != kVkNullHandle) impl_->vkDestroyPipelineLayout(impl_->device, pipe_layout, nullptr);
        if (ds_layout != kVkNullHandle) impl_->vkDestroyDescriptorSetLayout(impl_->device, ds_layout, nullptr);
        if (shader_mod != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, shader_mod, nullptr);
        if (arg_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, arg_buf, nullptr);
        if (out_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, out_buf, nullptr);
        if (in_buf != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, in_buf, nullptr);
        if (arg_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, arg_mem, nullptr);
        if (out_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, out_mem, nullptr);
        if (in_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, in_mem, nullptr);
    };

    bool in_coh = true, out_coh = true, arg_coh = true;
    Status st = impl_->CreateHostBuffer(
        byte_size, kVkBufferUsageStorageBufferBit, in_buf, in_mem, in_coh,
        desc.input_u32.empty() ? nullptr : desc.input_u32.data(),
        desc.input_u32.size() * sizeof(uint32_t));
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(byte_size, kVkBufferUsageStorageBufferBit, out_buf, out_mem, out_coh);
    if (!st.ok()) { cleanup(); return st; }
    st = impl_->CreateHostBuffer(
        desc.argument_buffer_bytes.size(), kVkBufferUsageIndirectBufferBit,
        arg_buf, arg_mem, arg_coh, desc.argument_buffer_bytes.data(),
        desc.argument_buffer_bytes.size());
    if (!st.ok()) { cleanup(); return st; }

    VkShaderModuleCreateInfo sm_ci{};
    sm_ci.sType = kVkStypeShaderModuleCreateInfo;
    sm_ci.codeSize = desc.spirv.size() * sizeof(uint32_t);
    sm_ci.pCode = desc.spirv.data();
    if (impl_->vkCreateShaderModule(impl_->device, &sm_ci, nullptr, &shader_mod) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule(indirect compute) failed");
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0u, kVkDescriptorTypeStorageBuffer, 1u, kVkShaderStageComputeBit, nullptr};
    bindings[1] = {1u, kVkDescriptorTypeStorageBuffer, 1u, kVkShaderStageComputeBit, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl_ci{};
    dsl_ci.sType = kVkStypeDescriptorSetLayoutCreateInfo;
    dsl_ci.bindingCount = 2;
    dsl_ci.pBindings = bindings;
    if (impl_->vkCreateDescriptorSetLayout(impl_->device, &dsl_ci, nullptr, &ds_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorSetLayout(indirect compute) failed");
    }

    VkPipelineLayoutCreateInfo pl_ci{};
    pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &ds_layout;
    if (impl_->vkCreatePipelineLayout(impl_->device, &pl_ci, nullptr, &pipe_layout) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout(indirect compute) failed");
    }

    VkComputePipelineCreateInfo cp_ci{};
    cp_ci.sType = kVkStypeComputePipelineCreateInfo;
    cp_ci.stage.sType = kVkStypePipelineShaderStageCreateInfo;
    cp_ci.stage.stage = kVkShaderStageComputeBit;
    cp_ci.stage.module = shader_mod;
    cp_ci.stage.pName = spv_info.entry_point_name.c_str();
    cp_ci.layout = pipe_layout;
    cp_ci.basePipelineIndex = -1;
    if (impl_->vkCreateComputePipelines(impl_->device, kVkNullHandle, 1, &cp_ci, nullptr, &pipeline) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::ShaderError, "vkCreateComputePipelines(indirect compute) failed");
    }

    VkDescriptorPoolSize pool_size{kVkDescriptorTypeStorageBuffer, 2};
    VkDescriptorPoolCreateInfo dp_ci{};
    dp_ci.sType = kVkStypeDescriptorPoolCreateInfo;
    dp_ci.maxSets = 1;
    dp_ci.poolSizeCount = 1;
    dp_ci.pPoolSizes = &pool_size;
    if (impl_->vkCreateDescriptorPool(impl_->device, &dp_ci, nullptr, &desc_pool) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkCreateDescriptorPool(indirect compute) failed");
    }

    VkDescriptorSet desc_set = kVkNullHandle;
    VkDescriptorSetAllocateInfo dsa_ci{};
    dsa_ci.sType = kVkStypeDescriptorSetAllocateInfo;
    dsa_ci.descriptorPool = desc_pool;
    dsa_ci.descriptorSetCount = 1;
    dsa_ci.pSetLayouts = &ds_layout;
    if (impl_->vkAllocateDescriptorSets(impl_->device, &dsa_ci, &desc_set) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateDescriptorSets(indirect compute) failed");
    }

    VkDescriptorBufferInfo in_binfo{in_buf, 0, byte_size};
    VkDescriptorBufferInfo out_binfo{out_buf, 0, byte_size};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = kVkStypeWriteDescriptorSet;
    writes[0].dstSet = desc_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[0].pBufferInfo = &in_binfo;
    writes[1].sType = kVkStypeWriteDescriptorSet;
    writes[1].dstSet = desc_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = kVkDescriptorTypeStorageBuffer;
    writes[1].pBufferInfo = &out_binfo;
    impl_->vkUpdateDescriptorSets(impl_->device, 2, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cba_ci{};
    cba_ci.sType = kVkStypeCommandBufferAllocateInfo;
    cba_ci.commandPool = impl_->command_pool;
    cba_ci.level = kVkCommandBufferLevelPrimary;
    cba_ci.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba_ci, &cmd_buf) != kVkSuccess || !cmd_buf) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(indirect compute) failed");
    }
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = kVkStypeCommandBufferBeginInfo;
    begin_info.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd_buf, &begin_info) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(indirect compute) failed");
    }

    impl_->vkCmdBindPipeline(cmd_buf, kVkPipelineBindPointCompute, pipeline);
    impl_->vkCmdBindDescriptorSets(cmd_buf, kVkPipelineBindPointCompute, pipe_layout, 0, 1, &desc_set, 0, nullptr);
    impl_->vkCmdDispatchIndirect(cmd_buf, arg_buf, desc.argument_buffer_offset);

    VkBufferMemoryBarrier out_barrier{};
    out_barrier.sType = kVkStypeBufferMemoryBarrier;
    out_barrier.srcAccessMask = kVkAccessShaderWriteBit;
    out_barrier.dstAccessMask = kVkAccessHostReadBit;
    out_barrier.srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    out_barrier.buffer = out_buf;
    out_barrier.offset = 0;
    out_barrier.size = byte_size;
    impl_->vkCmdPipelineBarrier(cmd_buf, kVkPipelineStageComputeShaderBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, 1, &out_barrier, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd_buf) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(indirect compute) failed");
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = kVkStypeSubmitInfo;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf;
    if (impl_->vkQueueSubmit(impl_->compute_queue, 1, &submit_info, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->compute_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueSubmit(indirect compute) failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, out_mem, 0, byte_size, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(indirect compute) failed");
    }
    if (!out_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, out_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }
    std::memcpy(out_result.output_u32.data(), mapped, static_cast<size_t>(byte_size));
    impl_->vkUnmapMemory(impl_->device, out_mem);
    cleanup();

    out_result.executed_on_vulkan_icd = true;
    return Status::Ok();
}

Status VulkanBackend::ExecuteDynamicRenderingMultiPass(
    const VulkanDynamicRenderingMultiPassDesc& desc,
    VulkanDynamicRenderingMultiPassResult& out_result) {
    out_result = VulkanDynamicRenderingMultiPassResult{};
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "VulkanBackend is not initialized");
    }
    if (desc.width == 0u || desc.height == 0u || desc.passes.empty()) {
        return Status::Error(StatusCode::InvalidArgument,
                             "ExecuteDynamicRenderingMultiPass: invalid dimensions or empty passes");
    }
    for (const auto& p : desc.passes) {
        if (auto st = ValidateSpirvBinary(p.vertex_spirv); !st.ok()) return st;
        if (auto st = ValidateSpirvBinary(p.fragment_spirv); !st.ok()) return st;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fallback_mode || !impl_->device || !impl_->has_graphics_queue) {
        out_result.framebuffer.width = desc.width;
        out_result.framebuffer.height = desc.height;
        out_result.framebuffer.pixels.assign(static_cast<size_t>(desc.width) * desc.height * 4u, 0u);
        out_result.passes_executed = static_cast<uint32_t>(desc.passes.size());
        out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
        out_result.executed_on_vulkan_icd = false;
        out_result.used_vk_cmd_begin_rendering = false;
        return Status::Ok();
    }

    const bool use_dyn_rendering =
        impl_->dynamic_rendering_enabled &&
        impl_->vkCmdBeginRendering != nullptr &&
        impl_->vkCmdEndRendering != nullptr;
    const int32_t vk_fmt = static_cast<int32_t>(desc.color_format);
    const uint32_t samples = desc.enable_msaa_4x ? kVkSampleCount4Bit : kVkSampleCount1Bit;

    // Choose supported hardware depth resolve mode for MSAA depth targets.
    uint32_t vk_depth_resolve_mode = kVkResolveModeNone;
    VulkanDepthResolveMode eff_depth_mode = VulkanDepthResolveMode::None;
    if (desc.enable_msaa_4x && desc.enable_depth && desc.depth_resolve_mode != VulkanDepthResolveMode::None) {
        const uint32_t req_bit = static_cast<uint32_t>(desc.depth_resolve_mode);
        if ((impl_->supported_depth_resolve_modes & req_bit) != 0u) {
            vk_depth_resolve_mode = req_bit;
            eff_depth_mode = desc.depth_resolve_mode;
        } else if ((impl_->supported_depth_resolve_modes & kVkResolveModeSampleZeroBit) != 0u) {
            vk_depth_resolve_mode = kVkResolveModeSampleZeroBit;
            eff_depth_mode = VulkanDepthResolveMode::SampleZero;
        }
    }

    VkImage color_img = kVkNullHandle, color_resolve_img = kVkNullHandle;
    VkImage depth_img = kVkNullHandle, depth_resolve_img = kVkNullHandle;
    VkDeviceMemory color_mem = kVkNullHandle, color_resolve_mem = kVkNullHandle;
    VkDeviceMemory depth_mem = kVkNullHandle, depth_resolve_mem = kVkNullHandle;
    VkImageView color_view = kVkNullHandle, color_resolve_view = kVkNullHandle;
    VkImageView depth_view = kVkNullHandle, depth_resolve_view = kVkNullHandle;
    VkBuffer color_rb = kVkNullHandle, depth_rb = kVkNullHandle;
    VkDeviceMemory color_rb_mem = kVkNullHandle, depth_rb_mem = kVkNullHandle;
    VkCommandBuffer cmd = nullptr;

    struct PassGpu {
        VkBuffer vb = kVkNullHandle, ib = kVkNullHandle;
        VkDeviceMemory vb_mem = kVkNullHandle, ib_mem = kVkNullHandle;
        VkShaderModule vs = kVkNullHandle, fs = kVkNullHandle;
        VkPipelineLayout pipe_layout = kVkNullHandle;
        VkPipeline pipeline = kVkNullHandle;
        VkRenderPass fallback_rp = kVkNullHandle;
        VkFramebuffer fallback_fb = kVkNullHandle;
        uint32_t pc_size = 0u;
    };
    std::vector<PassGpu> pass_gpus(desc.passes.size());

    auto cleanup = [&]() {
        if (cmd) { impl_->vkFreeCommandBuffers(impl_->device, impl_->graphics_command_pool, 1, &cmd); cmd = nullptr; }
        for (auto& pg : pass_gpus) {
            if (pg.fallback_fb != kVkNullHandle) impl_->vkDestroyFramebuffer(impl_->device, pg.fallback_fb, nullptr);
            if (pg.fallback_rp != kVkNullHandle) impl_->vkDestroyRenderPass(impl_->device, pg.fallback_rp, nullptr);
            if (pg.pipeline != kVkNullHandle) impl_->vkDestroyPipeline(impl_->device, pg.pipeline, nullptr);
            if (pg.pipe_layout != kVkNullHandle) impl_->vkDestroyPipelineLayout(impl_->device, pg.pipe_layout, nullptr);
            if (pg.fs != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, pg.fs, nullptr);
            if (pg.vs != kVkNullHandle) impl_->vkDestroyShaderModule(impl_->device, pg.vs, nullptr);
            if (pg.ib != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, pg.ib, nullptr);
            if (pg.ib_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, pg.ib_mem, nullptr);
            if (pg.vb != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, pg.vb, nullptr);
            if (pg.vb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, pg.vb_mem, nullptr);
        }
        if (depth_rb != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, depth_rb, nullptr);
        if (depth_rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, depth_rb_mem, nullptr);
        if (color_rb != kVkNullHandle) impl_->vkDestroyBuffer(impl_->device, color_rb, nullptr);
        if (color_rb_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, color_rb_mem, nullptr);
        if (depth_resolve_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, depth_resolve_view, nullptr);
        if (depth_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, depth_view, nullptr);
        if (color_resolve_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, color_resolve_view, nullptr);
        if (color_view != kVkNullHandle) impl_->vkDestroyImageView(impl_->device, color_view, nullptr);
        if (depth_resolve_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, depth_resolve_img, nullptr);
        if (depth_resolve_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, depth_resolve_mem, nullptr);
        if (depth_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, depth_img, nullptr);
        if (depth_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, depth_mem, nullptr);
        if (color_resolve_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, color_resolve_img, nullptr);
        if (color_resolve_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, color_resolve_mem, nullptr);
        if (color_img != kVkNullHandle) impl_->vkDestroyImage(impl_->device, color_img, nullptr);
        if (color_mem != kVkNullHandle) impl_->vkFreeMemory(impl_->device, color_mem, nullptr);
    };

    auto make_view = [&](VkImage img, int32_t fmt, VkFlags aspect, VkImageView& out_v) -> Status {
        VkImageViewCreateInfo iv{};
        iv.sType = kVkStypeImageViewCreateInfo;
        iv.image = img;
        iv.viewType = kVkImageViewType2D;
        iv.format = fmt;
        iv.subresourceRange = {aspect, 0, 1, 0, 1};
        if (impl_->vkCreateImageView(impl_->device, &iv, nullptr, &out_v) != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkCreateImageView(dynamic rendering) failed");
        }
        return Status::Ok();
    };

    Status st = impl_->CreateDeviceImage(
        kVkImageType2D, vk_fmt, desc.width, desc.height, 1, 1, samples,
        kVkImageUsageColorAttachmentBit | kVkImageUsageTransferSrcBit, color_img, color_mem);
    if (!st.ok()) { cleanup(); return st; }
    st = make_view(color_img, vk_fmt, kVkImageAspectColorBit, color_view);
    if (!st.ok()) { cleanup(); return st; }

    if (desc.enable_msaa_4x) {
        st = impl_->CreateDeviceImage(
            kVkImageType2D, vk_fmt, desc.width, desc.height, 1, 1, kVkSampleCount1Bit,
            kVkImageUsageColorAttachmentBit | kVkImageUsageTransferDstBit | kVkImageUsageTransferSrcBit,
            color_resolve_img, color_resolve_mem);
        if (!st.ok()) { cleanup(); return st; }
        st = make_view(color_resolve_img, vk_fmt, kVkImageAspectColorBit, color_resolve_view);
        if (!st.ok()) { cleanup(); return st; }
    }

    if (desc.enable_depth) {
        const VkFlags depth_usage = kVkImageUsageDepthStencilAttachmentBit |
                                    (desc.enable_msaa_4x ? 0u : kVkImageUsageTransferSrcBit);
        st = impl_->CreateDeviceImage(
            kVkImageType2D, kVkFormatD32Sfloat, desc.width, desc.height, 1, 1, samples,
            depth_usage, depth_img, depth_mem);
        if (!st.ok()) { cleanup(); return st; }
        st = make_view(depth_img, kVkFormatD32Sfloat, kVkImageAspectDepthBit, depth_view);
        if (!st.ok()) { cleanup(); return st; }

        if (desc.enable_msaa_4x && use_dyn_rendering && vk_depth_resolve_mode != kVkResolveModeNone) {
            st = impl_->CreateDeviceImage(
                kVkImageType2D, kVkFormatD32Sfloat, desc.width, desc.height, 1, 1, kVkSampleCount1Bit,
                kVkImageUsageDepthStencilAttachmentBit | kVkImageUsageTransferSrcBit,
                depth_resolve_img, depth_resolve_mem);
            if (!st.ok()) { cleanup(); return st; }
            st = make_view(depth_resolve_img, kVkFormatD32Sfloat, kVkImageAspectDepthBit, depth_resolve_view);
            if (!st.ok()) { cleanup(); return st; }
        }
    }

    const VkDeviceSize color_rb_bytes =
        static_cast<VkDeviceSize>(desc.width) * desc.height * BytesPerTexel(desc.color_format);
    bool color_rb_coh = true, depth_rb_coh = true;
    st = impl_->CreateHostBuffer(color_rb_bytes, kVkBufferUsageTransferDstBit, color_rb, color_rb_mem, color_rb_coh);
    if (!st.ok()) { cleanup(); return st; }

    const bool do_depth_readback =
        desc.enable_depth && desc.readback_depth &&
        (!desc.enable_msaa_4x || (depth_resolve_img != kVkNullHandle));
    const VkDeviceSize depth_rb_bytes =
        static_cast<VkDeviceSize>(desc.width) * desc.height * sizeof(float);
    if (do_depth_readback) {
        st = impl_->CreateHostBuffer(depth_rb_bytes, kVkBufferUsageTransferDstBit, depth_rb, depth_rb_mem, depth_rb_coh);
        if (!st.ok()) { cleanup(); return st; }
    }

    for (size_t pi = 0u; pi < desc.passes.size(); ++pi) {
        const auto& p = desc.passes[pi];
        auto& pg = pass_gpus[pi];
        bool vb_coh = true, ib_coh = true;
        if (!p.vertex_buffer_bytes.empty()) {
            st = impl_->CreateHostBuffer(p.vertex_buffer_bytes.size(), kVkBufferUsageVertexBufferBit,
                                         pg.vb, pg.vb_mem, vb_coh,
                                         p.vertex_buffer_bytes.data(), p.vertex_buffer_bytes.size());
            if (!st.ok()) { cleanup(); return st; }
        }
        if (!p.index_buffer_u32.empty()) {
            st = impl_->CreateHostBuffer(p.index_buffer_u32.size() * sizeof(uint32_t), kVkBufferUsageIndexBufferBit,
                                         pg.ib, pg.ib_mem, ib_coh,
                                         p.index_buffer_u32.data(), p.index_buffer_u32.size() * sizeof(uint32_t));
            if (!st.ok()) { cleanup(); return st; }
        }

        VkShaderModuleCreateInfo vsm{kVkStypeShaderModuleCreateInfo, nullptr, 0u,
                                     p.vertex_spirv.size() * sizeof(uint32_t), p.vertex_spirv.data()};
        VkShaderModuleCreateInfo fsm{kVkStypeShaderModuleCreateInfo, nullptr, 0u,
                                     p.fragment_spirv.size() * sizeof(uint32_t), p.fragment_spirv.data()};
        if (impl_->vkCreateShaderModule(impl_->device, &vsm, nullptr, &pg.vs) != kVkSuccess ||
            impl_->vkCreateShaderModule(impl_->device, &fsm, nullptr, &pg.fs) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::ShaderError, "vkCreateShaderModule(dynamic rendering) failed");
        }

        pg.pc_size = std::min<uint32_t>(
            static_cast<uint32_t>((p.push_constants.size() + 3u) & ~3u),
            impl_->max_push_constants_size);
        VkPushConstantRange pc_range{kVkShaderStageAllGraphics, 0u, pg.pc_size};
        VkPipelineLayoutCreateInfo pl_ci{};
        pl_ci.sType = kVkStypePipelineLayoutCreateInfo;
        pl_ci.pushConstantRangeCount = (pg.pc_size > 0u) ? 1u : 0u;
        pl_ci.pPushConstantRanges = (pg.pc_size > 0u) ? &pc_range : nullptr;
        if (impl_->vkCreatePipelineLayout(impl_->device, &pl_ci, nullptr, &pg.pipe_layout) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::DeviceError, "vkCreatePipelineLayout(dynamic rendering) failed");
        }

        if (!use_dyn_rendering) {
            VkAttachmentDescription atts[2]{};
            atts[0].format = vk_fmt;
            atts[0].samples = samples;
            atts[0].loadOp = p.clear_color_on_load ? kVkAttachmentLoadOpClear : kVkAttachmentLoadOpLoad;
            atts[0].storeOp = kVkAttachmentStoreOpStore;
            atts[0].stencilLoadOp = kVkAttachmentLoadOpDontCare;
            atts[0].stencilStoreOp = kVkAttachmentStoreOpDontCare;
            atts[0].initialLayout = (pi == 0u && p.clear_color_on_load) ? kVkImageLayoutUndefined : kVkImageLayoutColorAttachmentOptimal;
            atts[0].finalLayout = kVkImageLayoutColorAttachmentOptimal;
            VkAttachmentReference cref{0, kVkImageLayoutColorAttachmentOptimal};
            VkAttachmentReference dref{1, kVkImageLayoutDepthStencilAttachmentOptimal};
            if (desc.enable_depth) {
                atts[1].format = kVkFormatD32Sfloat;
                atts[1].samples = samples;
                atts[1].loadOp = p.clear_depth_on_load ? kVkAttachmentLoadOpClear : kVkAttachmentLoadOpLoad;
                atts[1].storeOp = kVkAttachmentStoreOpStore;
                atts[1].stencilLoadOp = kVkAttachmentLoadOpDontCare;
                atts[1].stencilStoreOp = kVkAttachmentStoreOpDontCare;
                atts[1].initialLayout = (pi == 0u && p.clear_depth_on_load) ? kVkImageLayoutUndefined : kVkImageLayoutDepthStencilAttachmentOptimal;
                atts[1].finalLayout = kVkImageLayoutDepthStencilAttachmentOptimal;
            }
            VkSubpassDescription sp{};
            sp.pipelineBindPoint = kVkPipelineBindPointGraphics;
            sp.colorAttachmentCount = 1;
            sp.pColorAttachments = &cref;
            sp.pDepthStencilAttachment = desc.enable_depth ? &dref : nullptr;
            VkRenderPassCreateInfo rp_ci{};
            rp_ci.sType = kVkStypeRenderPassCreateInfo;
            rp_ci.attachmentCount = desc.enable_depth ? 2u : 1u;
            rp_ci.pAttachments = atts;
            rp_ci.subpassCount = 1;
            rp_ci.pSubpasses = &sp;
            if (impl_->vkCreateRenderPass(impl_->device, &rp_ci, nullptr, &pg.fallback_rp) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkCreateRenderPass(multipass fallback) failed");
            }
            VkImageView fb_views[2] = {color_view, depth_view};
            VkFramebufferCreateInfo fb_ci{};
            fb_ci.sType = kVkStypeFramebufferCreateInfo;
            fb_ci.renderPass = pg.fallback_rp;
            fb_ci.attachmentCount = desc.enable_depth ? 2u : 1u;
            fb_ci.pAttachments = fb_views;
            fb_ci.width = desc.width;
            fb_ci.height = desc.height;
            fb_ci.layers = 1;
            if (impl_->vkCreateFramebuffer(impl_->device, &fb_ci, nullptr, &pg.fallback_fb) != kVkSuccess) {
                cleanup();
                return Status::Error(StatusCode::DeviceError, "vkCreateFramebuffer(multipass fallback) failed");
            }
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = kVkStypePipelineShaderStageCreateInfo;
        stages[0].stage = kVkShaderStageVertexBit;
        stages[0].module = pg.vs;
        stages[0].pName = p.vertex_entry.c_str();
        stages[1].sType = kVkStypePipelineShaderStageCreateInfo;
        stages[1].stage = kVkShaderStageFragmentBit;
        stages[1].module = pg.fs;
        stages[1].pName = p.fragment_entry.c_str();

        VkVertexInputBindingDescription vb_desc{};
        std::vector<VkVertexInputAttributeDescription> vk_attrs;
        VkPipelineVertexInputStateCreateInfo vi_ci{};
        vi_ci.sType = kVkStypePipelineVertexInputStateCreateInfo;
        if (!p.vertex_attributes.empty() && p.vertex_stride > 0u) {
            vb_desc = {0u, p.vertex_stride, kVkVertexInputRateVertex};
            for (const auto& a : p.vertex_attributes) {
                vk_attrs.push_back({a.location, a.binding, static_cast<int32_t>(a.format), a.offset});
            }
            vi_ci.vertexBindingDescriptionCount = 1;
            vi_ci.pVertexBindingDescriptions = &vb_desc;
            vi_ci.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attrs.size());
            vi_ci.pVertexAttributeDescriptions = vk_attrs.data();
        }

        VkPipelineInputAssemblyStateCreateInfo ia_ci{};
        ia_ci.sType = kVkStypePipelineInputAssemblyStateCreateInfo;
        ia_ci.topology = kVkPrimitiveTopologyTriangleList;
        VkPipelineViewportStateCreateInfo vp_ci{};
        vp_ci.sType = kVkStypePipelineViewportStateCreateInfo;
        vp_ci.viewportCount = 1;
        vp_ci.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs_ci{};
        rs_ci.sType = kVkStypePipelineRasterizationStateCreateInfo;
        rs_ci.polygonMode = kVkPolygonModeFill;
        rs_ci.cullMode = kVkCullModeNone;
        rs_ci.frontFace = kVkFrontFaceCounterClockwise;
        rs_ci.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms_ci{};
        ms_ci.sType = kVkStypePipelineMultisampleStateCreateInfo;
        ms_ci.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds_ci{};
        ds_ci.sType = kVkStypePipelineDepthStencilStateCreateInfo;
        ds_ci.depthTestEnable = (desc.enable_depth && p.depth_test) ? 1u : 0u;
        ds_ci.depthWriteEnable = (desc.enable_depth && p.depth_write) ? 1u : 0u;
        ds_ci.depthCompareOp = kVkCompareOpLess;
        VkPipelineColorBlendAttachmentState cb_att{};
        cb_att.colorWriteMask = 0xFu;
        VkPipelineColorBlendStateCreateInfo cb_ci{};
        cb_ci.sType = kVkStypePipelineColorBlendStateCreateInfo;
        cb_ci.attachmentCount = 1;
        cb_ci.pAttachments = &cb_att;
        const int32_t dyn_states[2] = {kVkDynamicStateViewport, kVkDynamicStateScissor};
        VkPipelineDynamicStateCreateInfo dyn_ci{};
        dyn_ci.sType = kVkStypePipelineDynamicStateCreateInfo;
        dyn_ci.dynamicStateCount = 2;
        dyn_ci.pDynamicStates = dyn_states;

        VkPipelineRenderingCreateInfo pr_ci{};
        pr_ci.sType = kVkStypePipelineRenderingCreateInfo;
        pr_ci.colorAttachmentCount = 1u;
        pr_ci.pColorAttachmentFormats = &vk_fmt;
        pr_ci.depthAttachmentFormat = desc.enable_depth ? kVkFormatD32Sfloat : 0;
        pr_ci.stencilAttachmentFormat = 0;

        VkGraphicsPipelineCreateInfo gp_ci{};
        gp_ci.sType = kVkStypeGraphicsPipelineCreateInfo;
        gp_ci.pNext = use_dyn_rendering ? static_cast<const void*>(&pr_ci) : nullptr;
        gp_ci.stageCount = 2u;
        gp_ci.pStages = stages;
        gp_ci.pVertexInputState = &vi_ci;
        gp_ci.pInputAssemblyState = &ia_ci;
        gp_ci.pViewportState = &vp_ci;
        gp_ci.pRasterizationState = &rs_ci;
        gp_ci.pMultisampleState = &ms_ci;
        gp_ci.pDepthStencilState = desc.enable_depth ? &ds_ci : nullptr;
        gp_ci.pColorBlendState = &cb_ci;
        gp_ci.pDynamicState = &dyn_ci;
        gp_ci.layout = pg.pipe_layout;
        gp_ci.renderPass = use_dyn_rendering ? kVkNullHandle : pg.fallback_rp;
        gp_ci.subpass = 0;
        gp_ci.basePipelineIndex = -1;
        if (impl_->vkCreateGraphicsPipelines(impl_->device, kVkNullHandle, 1, &gp_ci, nullptr, &pg.pipeline) != kVkSuccess) {
            cleanup();
            return Status::Error(StatusCode::ShaderError, "vkCreateGraphicsPipelines(dynamic rendering) failed");
        }
    }

    VkCommandBufferAllocateInfo cba{};
    cba.sType = kVkStypeCommandBufferAllocateInfo;
    cba.commandPool = impl_->graphics_command_pool;
    cba.level = kVkCommandBufferLevelPrimary;
    cba.commandBufferCount = 1;
    if (impl_->vkAllocateCommandBuffers(impl_->device, &cba, &cmd) != kVkSuccess || !cmd) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkAllocateCommandBuffers(dynamic rendering) failed");
    }
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = kVkStypeCommandBufferBeginInfo;
    cbi.flags = kVkCommandBufferUsageOneTimeSubmitBit;
    if (impl_->vkBeginCommandBuffer(cmd, &cbi) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkBeginCommandBuffer(dynamic rendering) failed");
    }

    // Transition attachments to optimal attachment layouts before Pass 0.
    impl_->CmdImageBarrier(cmd, color_img, kVkImageAspectColorBit,
                           kVkImageLayoutUndefined, kVkImageLayoutColorAttachmentOptimal,
                           kVkPipelineStageTopOfPipeBit, kVkPipelineStageColorAttachmentOutputBit,
                           0u, kVkAccessColorAttachmentWriteBit);
    if (color_resolve_img != kVkNullHandle && use_dyn_rendering) {
        impl_->CmdImageBarrier(cmd, color_resolve_img, kVkImageAspectColorBit,
                               kVkImageLayoutUndefined, kVkImageLayoutColorAttachmentOptimal,
                               kVkPipelineStageTopOfPipeBit, kVkPipelineStageColorAttachmentOutputBit,
                               0u, kVkAccessColorAttachmentWriteBit);
    }
    if (depth_img != kVkNullHandle) {
        impl_->CmdImageBarrier(cmd, depth_img, kVkImageAspectDepthBit,
                               kVkImageLayoutUndefined, kVkImageLayoutDepthStencilAttachmentOptimal,
                               kVkPipelineStageTopOfPipeBit,
                               kVkPipelineStageEarlyFragmentTestsBit | kVkPipelineStageLateFragmentTestsBit,
                               0u, kVkAccessDepthStencilAttachmentWriteBit);
    }
    if (depth_resolve_img != kVkNullHandle && use_dyn_rendering) {
        impl_->CmdImageBarrier(cmd, depth_resolve_img, kVkImageAspectDepthBit,
                               kVkImageLayoutUndefined, kVkImageLayoutDepthStencilAttachmentOptimal,
                               kVkPipelineStageTopOfPipeBit,
                               kVkPipelineStageEarlyFragmentTestsBit | kVkPipelineStageLateFragmentTestsBit,
                               0u, kVkAccessDepthStencilAttachmentWriteBit);
    }

    VkViewport vp{0.0f, 0.0f, static_cast<float>(desc.width), static_cast<float>(desc.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {desc.width, desc.height}};

    for (size_t pi = 0u; pi < desc.passes.size(); ++pi) {
        const auto& p = desc.passes[pi];
        const auto& pg = pass_gpus[pi];
        const bool is_last_pass = (pi + 1u == desc.passes.size());

        if (pi > 0u) {
            // Inter-pass dependency barrier preserving color + depth written by prior pass.
            impl_->CmdImageBarrier(cmd, color_img, kVkImageAspectColorBit,
                                   kVkImageLayoutColorAttachmentOptimal, kVkImageLayoutColorAttachmentOptimal,
                                   kVkPipelineStageColorAttachmentOutputBit, kVkPipelineStageColorAttachmentOutputBit,
                                   kVkAccessColorAttachmentWriteBit,
                                   kVkAccessColorAttachmentReadBit | kVkAccessColorAttachmentWriteBit);
            if (depth_img != kVkNullHandle) {
                impl_->CmdImageBarrier(
                    cmd, depth_img, kVkImageAspectDepthBit,
                    kVkImageLayoutDepthStencilAttachmentOptimal, kVkImageLayoutDepthStencilAttachmentOptimal,
                    kVkPipelineStageEarlyFragmentTestsBit | kVkPipelineStageLateFragmentTestsBit,
                    kVkPipelineStageEarlyFragmentTestsBit | kVkPipelineStageLateFragmentTestsBit,
                    kVkAccessDepthStencilAttachmentWriteBit,
                    kVkAccessDepthStencilAttachmentReadBit | kVkAccessDepthStencilAttachmentWriteBit);
            }
        }

        if (use_dyn_rendering) {
            VkRenderingAttachmentInfo col_att{};
            col_att.sType = kVkStypeRenderingAttachmentInfo;
            col_att.imageView = color_view;
            col_att.imageLayout = kVkImageLayoutColorAttachmentOptimal;
            col_att.loadOp = p.clear_color_on_load ? kVkAttachmentLoadOpClear : kVkAttachmentLoadOpLoad;
            col_att.storeOp = kVkAttachmentStoreOpStore;
            col_att.clearValue.color.float32[0] = p.clear_color[0];
            col_att.clearValue.color.float32[1] = p.clear_color[1];
            col_att.clearValue.color.float32[2] = p.clear_color[2];
            col_att.clearValue.color.float32[3] = p.clear_color[3];
            if (is_last_pass && desc.enable_msaa_4x && color_resolve_view != kVkNullHandle) {
                col_att.resolveMode = kVkResolveModeAverageBit;
                col_att.resolveImageView = color_resolve_view;
                col_att.resolveImageLayout = kVkImageLayoutColorAttachmentOptimal;
                out_result.resolved_msaa_color = true;
            }

            VkRenderingAttachmentInfo dep_att{};
            if (desc.enable_depth) {
                dep_att.sType = kVkStypeRenderingAttachmentInfo;
                dep_att.imageView = depth_view;
                dep_att.imageLayout = kVkImageLayoutDepthStencilAttachmentOptimal;
                dep_att.loadOp = p.clear_depth_on_load ? kVkAttachmentLoadOpClear : kVkAttachmentLoadOpLoad;
                dep_att.storeOp = kVkAttachmentStoreOpStore;
                dep_att.clearValue.depthStencil = {p.clear_depth, 0u};
                if (is_last_pass && desc.enable_msaa_4x &&
                    depth_resolve_view != kVkNullHandle && vk_depth_resolve_mode != kVkResolveModeNone) {
                    dep_att.resolveMode = vk_depth_resolve_mode;
                    dep_att.resolveImageView = depth_resolve_view;
                    dep_att.resolveImageLayout = kVkImageLayoutDepthStencilAttachmentOptimal;
                    out_result.resolved_msaa_depth = true;
                    out_result.effective_depth_resolve_mode = eff_depth_mode;
                }
            }

            VkRenderingInfo ri{};
            ri.sType = kVkStypeRenderingInfo;
            ri.renderArea = {{0, 0}, {desc.width, desc.height}};
            ri.layerCount = 1u;
            ri.colorAttachmentCount = 1u;
            ri.pColorAttachments = &col_att;
            ri.pDepthAttachment = desc.enable_depth ? &dep_att : nullptr;
            impl_->vkCmdBeginRendering(cmd, &ri);
        } else {
            VkClearValue cvs[2]{};
            cvs[0].color.float32[0] = p.clear_color[0];
            cvs[0].color.float32[1] = p.clear_color[1];
            cvs[0].color.float32[2] = p.clear_color[2];
            cvs[0].color.float32[3] = p.clear_color[3];
            cvs[1].depthStencil = {p.clear_depth, 0u};
            VkRenderPassBeginInfo rpb{};
            rpb.sType = kVkStypeRenderPassBeginInfo;
            rpb.renderPass = pg.fallback_rp;
            rpb.framebuffer = pg.fallback_fb;
            rpb.renderArea = {{0, 0}, {desc.width, desc.height}};
            rpb.clearValueCount = desc.enable_depth ? 2u : 1u;
            rpb.pClearValues = cvs;
            impl_->vkCmdBeginRenderPass(cmd, &rpb, kVkSubpassContentsInline);
        }

        impl_->vkCmdSetViewport(cmd, 0, 1, &vp);
        impl_->vkCmdSetScissor(cmd, 0, 1, &sc);
        impl_->vkCmdBindPipeline(cmd, kVkPipelineBindPointGraphics, pg.pipeline);
        if (pg.pc_size > 0u && !p.push_constants.empty()) {
            std::vector<uint8_t> pc_buf(pg.pc_size, 0u);
            std::memcpy(pc_buf.data(), p.push_constants.data(), std::min<size_t>(p.push_constants.size(), pg.pc_size));
            impl_->vkCmdPushConstants(cmd, pg.pipe_layout, kVkShaderStageAllGraphics, 0u, pg.pc_size, pc_buf.data());
        }
        if (pg.vb != kVkNullHandle) {
            const VkDeviceSize vb_off = 0;
            impl_->vkCmdBindVertexBuffers(cmd, 0, 1, &pg.vb, &vb_off);
        }
        if (pg.ib != kVkNullHandle) {
            impl_->vkCmdBindIndexBuffer(cmd, pg.ib, 0, kVkIndexTypeUint32);
            impl_->vkCmdDrawIndexed(cmd, static_cast<uint32_t>(p.index_buffer_u32.size()), 1u, 0u, 0, 0u);
        } else {
            impl_->vkCmdDraw(cmd, p.vertex_count, 1u, 0u, 0u);
        }

        if (use_dyn_rendering) {
            impl_->vkCmdEndRendering(cmd);
        } else {
            impl_->vkCmdEndRenderPass(cmd);
        }
    }

    VkImage final_color_src = color_img;
    if (desc.enable_msaa_4x) {
        if (use_dyn_rendering && color_resolve_img != kVkNullHandle) {
            impl_->CmdImageBarrier(cmd, color_resolve_img, kVkImageAspectColorBit,
                                   kVkImageLayoutColorAttachmentOptimal, kVkImageLayoutTransferSrcOptimal,
                                   kVkPipelineStageColorAttachmentOutputBit, kVkPipelineStageTransferBit,
                                   kVkAccessColorAttachmentWriteBit, kVkAccessTransferReadBit);
            final_color_src = color_resolve_img;
        } else if (color_resolve_img != kVkNullHandle) {
            impl_->CmdImageBarrier(cmd, color_img, kVkImageAspectColorBit,
                                   kVkImageLayoutColorAttachmentOptimal, kVkImageLayoutTransferSrcOptimal,
                                   kVkPipelineStageColorAttachmentOutputBit, kVkPipelineStageTransferBit,
                                   kVkAccessColorAttachmentWriteBit, kVkAccessTransferReadBit);
            impl_->CmdImageBarrier(cmd, color_resolve_img, kVkImageAspectColorBit,
                                   kVkImageLayoutUndefined, kVkImageLayoutTransferDstOptimal,
                                   kVkPipelineStageTopOfPipeBit, kVkPipelineStageTransferBit,
                                   0u, kVkAccessTransferWriteBit);
            VkImageResolve reg{};
            reg.srcSubresource = {kVkImageAspectColorBit, 0, 0, 1};
            reg.dstSubresource = {kVkImageAspectColorBit, 0, 0, 1};
            reg.extent = {desc.width, desc.height, 1};
            impl_->vkCmdResolveImage(cmd, color_img, kVkImageLayoutTransferSrcOptimal,
                                     color_resolve_img, kVkImageLayoutTransferDstOptimal, 1, &reg);
            impl_->CmdImageBarrier(cmd, color_resolve_img, kVkImageAspectColorBit,
                                   kVkImageLayoutTransferDstOptimal, kVkImageLayoutTransferSrcOptimal,
                                   kVkPipelineStageTransferBit, kVkPipelineStageTransferBit,
                                   kVkAccessTransferWriteBit, kVkAccessTransferReadBit);
            final_color_src = color_resolve_img;
            out_result.resolved_msaa_color = true;
        }
    } else {
        impl_->CmdImageBarrier(cmd, color_img, kVkImageAspectColorBit,
                               kVkImageLayoutColorAttachmentOptimal, kVkImageLayoutTransferSrcOptimal,
                               kVkPipelineStageColorAttachmentOutputBit, kVkPipelineStageTransferBit,
                               kVkAccessColorAttachmentWriteBit, kVkAccessTransferReadBit);
    }

    VkBufferImageCopy col_copy{};
    col_copy.imageSubresource = {kVkImageAspectColorBit, 0, 0, 1};
    col_copy.imageExtent = {desc.width, desc.height, 1};
    impl_->vkCmdCopyImageToBuffer(cmd, final_color_src, kVkImageLayoutTransferSrcOptimal, color_rb, 1, &col_copy);

    if (do_depth_readback) {
        VkImage final_depth_src = (desc.enable_msaa_4x && depth_resolve_img != kVkNullHandle)
                                      ? depth_resolve_img
                                      : depth_img;
        impl_->CmdImageBarrier(cmd, final_depth_src, kVkImageAspectDepthBit,
                               kVkImageLayoutDepthStencilAttachmentOptimal, kVkImageLayoutTransferSrcOptimal,
                               kVkPipelineStageEarlyFragmentTestsBit | kVkPipelineStageLateFragmentTestsBit,
                               kVkPipelineStageTransferBit,
                               kVkAccessDepthStencilAttachmentWriteBit, kVkAccessTransferReadBit);
        VkBufferImageCopy dep_copy{};
        dep_copy.imageSubresource = {kVkImageAspectDepthBit, 0, 0, 1};
        dep_copy.imageExtent = {desc.width, desc.height, 1};
        impl_->vkCmdCopyImageToBuffer(cmd, final_depth_src, kVkImageLayoutTransferSrcOptimal, depth_rb, 1, &dep_copy);
    }

    VkBufferMemoryBarrier rb_bars[2]{};
    uint32_t rb_bar_cnt = 1u;
    rb_bars[0].sType = kVkStypeBufferMemoryBarrier;
    rb_bars[0].srcAccessMask = kVkAccessTransferWriteBit;
    rb_bars[0].dstAccessMask = kVkAccessHostReadBit;
    rb_bars[0].srcQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bars[0].dstQueueFamilyIndex = kVkQueueFamilyIgnored;
    rb_bars[0].buffer = color_rb;
    rb_bars[0].offset = 0;
    rb_bars[0].size = color_rb_bytes;
    if (do_depth_readback) {
        rb_bars[1].sType = kVkStypeBufferMemoryBarrier;
        rb_bars[1].srcAccessMask = kVkAccessTransferWriteBit;
        rb_bars[1].dstAccessMask = kVkAccessHostReadBit;
        rb_bars[1].srcQueueFamilyIndex = kVkQueueFamilyIgnored;
        rb_bars[1].dstQueueFamilyIndex = kVkQueueFamilyIgnored;
        rb_bars[1].buffer = depth_rb;
        rb_bars[1].offset = 0;
        rb_bars[1].size = depth_rb_bytes;
        rb_bar_cnt = 2u;
    }
    impl_->vkCmdPipelineBarrier(cmd, kVkPipelineStageTransferBit, kVkPipelineStageHostBit,
                                0, 0, nullptr, rb_bar_cnt, rb_bars, 0, nullptr);

    if (impl_->vkEndCommandBuffer(cmd) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkEndCommandBuffer(dynamic rendering) failed");
    }
    VkSubmitInfo si{};
    si.sType = kVkStypeSubmitInfo;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (impl_->vkQueueSubmit(impl_->graphics_queue, 1, &si, kVkNullHandle) != kVkSuccess ||
        impl_->vkQueueWaitIdle(impl_->graphics_queue) != kVkSuccess) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkQueueSubmit(dynamic rendering) failed");
    }

    void* mapped = nullptr;
    if (impl_->vkMapMemory(impl_->device, color_rb_mem, 0, color_rb_bytes, 0, &mapped) != kVkSuccess || !mapped) {
        cleanup();
        return Status::Error(StatusCode::DeviceError, "vkMapMemory(dynamic rendering color) failed");
    }
    if (!color_rb_coh) {
        VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, color_rb_mem, 0, kVkWholeSize};
        impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
    }
    out_result.framebuffer.width = desc.width;
    out_result.framebuffer.height = desc.height;
    out_result.framebuffer.pixels.resize(static_cast<size_t>(desc.width) * desc.height * 4u);
    if (desc.color_format == VulkanImageFormat::Rgba8Unorm ||
        desc.color_format == VulkanImageFormat::Bgra8Unorm) {
        std::memcpy(out_result.framebuffer.pixels.data(), mapped, static_cast<size_t>(color_rb_bytes));
    }
    impl_->vkUnmapMemory(impl_->device, color_rb_mem);

    if (do_depth_readback) {
        void* dmapped = nullptr;
        if (impl_->vkMapMemory(impl_->device, depth_rb_mem, 0, depth_rb_bytes, 0, &dmapped) == kVkSuccess && dmapped) {
            if (!depth_rb_coh) {
                VkMappedMemoryRange rng{kVkStypeMappedMemoryRange, nullptr, depth_rb_mem, 0, kVkWholeSize};
                impl_->vkInvalidateMappedMemoryRanges(impl_->device, 1, &rng);
            }
            out_result.depth_readback.resize(static_cast<size_t>(desc.width) * desc.height);
            std::memcpy(out_result.depth_readback.data(), dmapped, static_cast<size_t>(depth_rb_bytes));
            impl_->vkUnmapMemory(impl_->device, depth_rb_mem);
        }
    }

    cleanup();
    out_result.fnv1a_hash = out_result.framebuffer.ComputeFnv1aHash();
    out_result.passes_executed = static_cast<uint32_t>(desc.passes.size());
    out_result.executed_on_vulkan_icd = true;
    out_result.used_vk_cmd_begin_rendering = use_dyn_rendering;
    return Status::Ok();
}

}  // namespace openscarlett
