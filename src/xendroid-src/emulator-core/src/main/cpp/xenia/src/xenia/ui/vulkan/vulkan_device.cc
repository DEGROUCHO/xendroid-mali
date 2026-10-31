/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_device.h"

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#include <dlfcn.h>
#include <malloc.h>

#include <atomic>
#include <map>
#include <mutex>
#include <tuple>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

DEFINE_int32(
    vulkan_clamp_storage_buffer_range, 0,
    "If > 0, clamp the reported maxStorageBufferRange to this many bytes. "
    "Drivers reporting very large limits get a single 512 MB storage buffer "
    "binding for shared memory emulation; clamping to 134217728 (128 MB) "
    "forces the split 4-binding path (what Turnip uses) for debugging "
    "driver-specific large-binding misbehavior.",
    "Vulkan");

DEFINE_bool(
    vulkan_log_capability_audit, false,
    "Diagnostic: log supported (not enabled) tile features, core features and "
    "all backend Vulkan formats with optimal-tiling masks and per-format "
    "sample counts. No extensions or features are enabled by this flag.",
    "Vulkan");

DECLARE_int32(frame_generation);
DEFINE_bool(log_vk_memory, false,
            "Diagnostic: account every vkAllocateMemory by its two calling "
            "sites and log the live MB per site (plus the native heap) when "
            "the GPU emulation thread asks, about every 10 s.",
            "Vulkan");
DEFINE_bool(
    vulkan_fragment_shading_rate_dynamic, true,
    "Enable VK_KHR_fragment_shading_rate and create the guest draw pipelines "
    "with the fragment shading rate as dynamic state, so "
    "vulkan_fragment_shading_rate can be changed while running. Latched at "
    "GPU initialization. Off: the extension isn't enabled, the rate is never "
    "touched and vulkan_fragment_shading_rate does nothing.",
    "Vulkan");
UPDATE_from_bool(vulkan_fragment_shading_rate_dynamic, 2026, 10, 11, 5, false);
DEFINE_bool(
    vulkan_msaa_single_sample_resolve, false,
    "With vulkan_msaa_single_sample_storage: enable "
    "VK_EXT_multisampled_render_to_single_sampled and keep rasterizing the "
    "game's 2x / 4x samples inside each render pass, resolving them into the "
    "single-sampled images when the pass ends. Keeps the antialiasing of the "
    "edges drawn within one pass at the same memory traffic. Latched at GPU "
    "initialization; needs dynamic rendering.",
    "Vulkan");
DEFINE_bool(log_image_compression, false,
            "Diagnostic: log whether render targets and textures are "
            "compressed by the driver (AFBC on Mali), per format and creation "
            "flags.",
            "GPU");
DEFINE_double(vulkan_emulation_queue_priority, 1.0,
              "Priority (0 to 1) of queue 0 of each queue family, used for "
              "emulation, when a second queue (frame generation painting) is "
              "enabled in the family - lower lets the paint queue go first.",
              "Vulkan");

DEFINE_bool(
    vulkan_robust_buffer_access, true,
    "Enable robustBufferAccess when the device supports it. Bounds-checking "
    "every buffer access has a cost on some GPUs (vertex fetching from the "
    "shared memory storage buffer on Mali); the shaders already mask guest "
    "addresses to the shared memory size.",
    "Vulkan");

namespace xe {
namespace ui {
namespace vulkan {

namespace {

// log_vk_memory accounting.
struct VkMemorySite {
  uint64_t bytes = 0;
  uint64_t count = 0;
};
std::mutex vk_memory_mutex;
std::unordered_map<uint64_t, std::pair<uint64_t, std::pair<uintptr_t, uintptr_t>>>
    vk_memory_live;
std::map<std::pair<uintptr_t, uintptr_t>, VkMemorySite> vk_memory_sites;
uint64_t vk_memory_total = 0;
PFN_vkAllocateMemory vk_memory_real_allocate = nullptr;
PFN_vkFreeMemory vk_memory_real_free = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL VkMemoryAccountingAllocate(
    VkDevice device, const VkMemoryAllocateInfo* allocate_info,
    const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
  const std::pair<uintptr_t, uintptr_t> site(
      uintptr_t(__builtin_return_address(0)),
      uintptr_t(__builtin_return_address(1)));
  VkResult result =
      vk_memory_real_allocate(device, allocate_info, allocator, memory);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(vk_memory_mutex);
    vk_memory_live[reinterpret_cast<uint64_t>(*memory)] = {
        allocate_info->allocationSize, site};
    VkMemorySite& entry = vk_memory_sites[site];
    entry.bytes += allocate_info->allocationSize;
    ++entry.count;
    vk_memory_total += allocate_info->allocationSize;
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL VkMemoryAccountingFree(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator) {
  if (memory != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(vk_memory_mutex);
    auto it = vk_memory_live.find(reinterpret_cast<uint64_t>(memory));
    if (it != vk_memory_live.end()) {
      VkMemorySite& entry = vk_memory_sites[it->second.second];
      entry.bytes -= it->second.first;
      --entry.count;
      vk_memory_total -= it->second.first;
      vk_memory_live.erase(it);
    }
  }
  vk_memory_real_free(device, memory, allocator);
}

uintptr_t VkMemoryModuleOffset(uintptr_t address) {
  Dl_info info = {};
  if (address && dladdr(reinterpret_cast<void*>(address), &info) &&
      info.dli_fbase) {
    return address - uintptr_t(info.dli_fbase);
  }
  return address;
}

// Query-only: the presence of an extension is not proof of its optional
// features, nor of support for a particular image usage/sample combination.
void LogCapabilityAudit(
    const VulkanInstance* instance, VkPhysicalDevice physical_device,
    const std::vector<VkExtensionProperties>& extensions) {
  const auto& ifn = instance->functions();
  auto has_extension = [&](const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties& e) {
                         return std::strcmp(e.extensionName, name) == 0;
                       });
  };
  VkPhysicalDeviceFeatures core = {};
  ifn.vkGetPhysicalDeviceFeatures(physical_device, &core);
  XELOGI("VkCapabilityAudit: SUPPORTED (not enabled) BC={} ASTC={} ETC2={} "
         "vertexStores={} fragmentStores={} clip={} cull={} sparseBinding={} "
         "sparseBuffer={} fillModeNonSolid={}",
         core.textureCompressionBC, core.textureCompressionASTC_LDR,
         core.textureCompressionETC2, core.vertexPipelineStoresAndAtomics,
         core.fragmentStoresAndAtomics, core.shaderClipDistance,
         core.shaderCullDistance, core.sparseBinding,
         core.sparseResidencyBuffer, core.fillModeNonSolid);
  if (instance->extensions().ext_1_1_KHR_get_physical_device_properties2) {
    VkPhysicalDeviceFeatures2 features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    VkPhysicalDeviceShaderTileImageFeaturesEXT tile = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT};
    bool has_order = has_extension("VK_EXT_rasterization_order_attachment_access") ||
                     has_extension("VK_ARM_rasterization_order_attachment_access");
    bool has_tile = has_extension("VK_EXT_shader_tile_image");
    if (has_order) {
      order.pNext = features.pNext;
      features.pNext = &order;
    }
    if (has_tile) {
      tile.pNext = features.pNext;
      features.pNext = &tile;
    }
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &features);
    XELOGI("VkCapabilityAudit: SUPPORTED order_ext={} orderColor={} "
           "orderDepth={} orderStencil={} tile_ext={} tileColor={} tileDepth={} "
           "tileStencil={}", has_order, order.rasterizationOrderColorAttachmentAccess,
           order.rasterizationOrderDepthAttachmentAccess,
           order.rasterizationOrderStencilAttachmentAccess, has_tile,
           tile.shaderTileImageColorReadAccess, tile.shaderTileImageDepthReadAccess,
           tile.shaderTileImageStencilReadAccess);
    if (has_tile) {
      VkPhysicalDeviceShaderTileImagePropertiesEXT tile_properties = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 properties = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &tile_properties};
      ifn.vkGetPhysicalDeviceProperties2(physical_device, &properties);
      XELOGI("VkCapabilityAudit: tileCoherentAccelerated={} "
             "tileReadHelper={} tileReadSampleFromPixelRate={}",
             tile_properties.shaderTileImageCoherentReadAccelerated,
             tile_properties.shaderTileImageReadFromHelperInvocation,
             tile_properties.shaderTileImageReadSampleFromPixelRateInvocation);
    }
  }
  struct FormatProbe { VkFormat format; const char* name; };
#define XE_AUDIT_FORMAT(f) {f, #f}
  const FormatProbe formats[] = {
      XE_AUDIT_FORMAT(VK_FORMAT_ASTC_4x4_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_A1R5G5B5_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_A2B10G10R10_SNORM_PACK32),
      XE_AUDIT_FORMAT(VK_FORMAT_A2B10G10R10_UNORM_PACK32),
      XE_AUDIT_FORMAT(VK_FORMAT_A2R10G10B10_UNORM_PACK32),
      XE_AUDIT_FORMAT(VK_FORMAT_B10G11R11_UFLOAT_PACK32),
      XE_AUDIT_FORMAT(VK_FORMAT_B4G4R4A4_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_B5G5R5A1_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_B5G6R5_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_B8G8R8A8_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_B8G8R8G8_422_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_B8G8R8G8_422_UNORM_KHR),
      XE_AUDIT_FORMAT(VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_BC2_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_BC3_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_BC4_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_BC5_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_D24_UNORM_S8_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_D32_SFLOAT_S8_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
      XE_AUDIT_FORMAT(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK),
      XE_AUDIT_FORMAT(VK_FORMAT_G8B8G8R8_422_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_G8B8G8R8_422_UNORM_KHR),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16B16A16_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16B16A16_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16B16A16_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16B16A16_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R16G16_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R16_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R16_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R16_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R32G32B32A32_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R32G32_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R32G32_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R32_SFLOAT),
      XE_AUDIT_FORMAT(VK_FORMAT_R32_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R4G4B4A4_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_R5G5B5A1_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_R5G6B5_UNORM_PACK16),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8B8A8_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8B8A8_SRGB),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8B8A8_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8B8A8_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R8G8_UNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R8_SNORM),
      XE_AUDIT_FORMAT(VK_FORMAT_R8_UINT),
      XE_AUDIT_FORMAT(VK_FORMAT_R8_UNORM),
  };
#undef XE_AUDIT_FORMAT
  const auto get_image_format_properties =
      PFN_vkGetPhysicalDeviceImageFormatProperties(ifn.vkGetInstanceProcAddr(
          instance->instance(), "vkGetPhysicalDeviceImageFormatProperties"));
  if (!get_image_format_properties) {
    XELOGW("VkCapabilityAudit: image format query entry point unavailable");
    return;
  }
  for (const auto& probe : formats) {
    VkFormatProperties properties;
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device, probe.format,
                                           &properties);
    VkImageFormatProperties sampled = {}, attachment = {};
    VkResult sampled_result = get_image_format_properties(
        physical_device, probe.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &sampled);
    bool is_depth = (properties.optimalTilingFeatures &
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    VkImageUsageFlags attachment_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        (is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    VkResult attachment_result = get_image_format_properties(
        physical_device, probe.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        attachment_usage, 0, &attachment);
    XELOGI("VkFormatAudit: {} optimal=0x{:X} linear=0x{:X} buffer=0x{:X} "
           "sampledTransferResult={} sampledSamples=0x{:X} "
           "sampledAttachmentInputResult={} attachmentSamples=0x{:X}",
           probe.name, properties.optimalTilingFeatures,
           properties.linearTilingFeatures, properties.bufferFeatures,
           int32_t(sampled_result), sampled.sampleCounts,
           int32_t(attachment_result), attachment.sampleCounts);
  }
}

}  // namespace

template <typename Structure, VkStructureType StructureType>
struct VulkanFeatures {
  Structure supported = {StructureType};
  Structure enabled = {StructureType};

  void Link(VkPhysicalDeviceFeatures2& supported_features_2,
            VkDeviceCreateInfo& device_create_info) {
    supported.pNext = supported_features_2.pNext;
    supported_features_2.pNext = &supported;
    enabled.pNext = const_cast<void*>(device_create_info.pNext);
    device_create_info.pNext = &enabled;
  }
};

std::unique_ptr<VulkanDevice> VulkanDevice::CreateIfSupported(
    const VulkanInstance* const vulkan_instance,
    const VkPhysicalDevice physical_device, const bool with_gpu_emulation,
    const bool with_swapchain) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);

  const VulkanInstance::Functions& ifn = vulkan_instance->functions();

  // Get supported Vulkan 1.0 properties and features.

  VkPhysicalDeviceProperties properties = {};
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);

  // From the VkApplicationInfo specification:
  //
  // "The Khronos validation layers will treat apiVersion as the highest API
  // version the application targets, and will validate API usage against the
  // minimum of that version and the implementation version (instance or device,
  // depending on context). If an application tries to use functionality from a
  // greater version than this, a validation error will be triggered."
  //
  // "Vulkan 1.0 implementations were required to return
  // VK_ERROR_INCOMPATIBLE_DRIVER if apiVersion was larger than 1.0."
  //
  // Make sure that all usages of the API version in Xenia receive the highest
  // minor version that Xenia has been tested on.
  // Libraries such as the Vulkan Memory Allocator also may expect a minor
  // version that is known to them.
  const uint32_t unclamped_api_version = properties.apiVersion;
  const uint32_t clamped_api_minor_version = std::min(
      VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(unclamped_api_version),
                          VK_API_VERSION_MAJOR(unclamped_api_version),
                          VK_API_VERSION_MINOR(unclamped_api_version), 0),
      vulkan_instance->api_version() >= VK_MAKE_API_VERSION(0, 1, 1, 0)
          ? kHighestUsedApiMinorVersion
          : VK_MAKE_API_VERSION(0, 1, 0, 0));
  properties.apiVersion =
      VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(clamped_api_minor_version),
                          VK_API_VERSION_MAJOR(clamped_api_minor_version),
                          VK_API_VERSION_MINOR(clamped_api_minor_version),
                          VK_API_VERSION_PATCH(unclamped_api_version));

  VkPhysicalDeviceFeatures supported_features = {};
  ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);

  if (with_gpu_emulation) {
    if (!supported_features.independentBlend) {
      // Not trivial to work around:
      // - Affects not only the blend equation, but also the color write mask.
      // - Can't reuse the blend state of the first attachment for all because
      //   some attachments may have a format that doesn't support blending.
      // - Not possible to split the draw into per-attachment draws because of
      //   depth / stencil.
      // Not supported only on the proprietary driver for the Qualcomm
      // Adreno 4xx, where the driver is largely experimental and doesn't expose
      // a lot of the functionality available in the hardware.
      XELOGW(
          "Vulkan device '{}' doesn't support the independentBlend feature "
          "required for GPU emulation",
          properties.deviceName);
      return nullptr;
    }
  }

  // Enable needed extensions.

  std::unique_ptr<VulkanDevice> device(
      new VulkanDevice(vulkan_instance, physical_device));

  const bool get_physical_device_properties2_supported =
      vulkan_instance->extensions().ext_1_1_KHR_get_physical_device_properties2;

  // Name pointers from `requested_extensions` will be used in the enabled
  // extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;

  const auto request_promoted_extension =
      [&](const char* const name, uint32_t const major, uint32_t const minor,
          bool* const supported_ptr) {
        assert_not_null(supported_ptr);
        if (properties.apiVersion >= VK_MAKE_API_VERSION(0, major, minor, 0)) {
          *supported_ptr = true;
        } else {
          requested_extensions.emplace(name, supported_ptr);
        }
      };

#define XE_UI_VULKAN_STRUCT_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &device->extensions_.ext_##name);
#define XE_UI_VULKAN_LOCAL_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &ext_##name);
#define XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension(                                      \
      "VK_" #name, major, minor,                                   \
      &device->extensions_.ext_##major##_##minor##_##name);
#define XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor,           \
                             &ext_##major##_##minor##_##name);

  bool ext_KHR_portability_subset = false;
  bool ext_1_2_KHR_driver_properties = false;
  if (get_physical_device_properties2_supported) {
    // #164. Must be enabled according to the specification if the physical
    // device is a portability subset one.
    XE_UI_VULKAN_LOCAL_EXTENSION(KHR_portability_subset)
    // #197
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_driver_properties, 1, 2)
  }

  // Used by the Vulkan Memory Allocator and potentially by Xenia.
  // #128.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dedicated_allocation, 1, 1)
  // #147. Also must be enabled for VK_KHR_dedicated_allocation and
  // VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_get_memory_requirements2, 1, 1)
  // #158. Also must be enabled for VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_bind_memory2, 1, 1)
  if (get_physical_device_properties2_supported) {
    // #238.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_memory_budget)
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    // #414.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_maintenance4, 1, 3)
  }
  // #401. No features, only the NONE attachment load operation.
  XE_UI_VULKAN_STRUCT_EXTENSION(EXT_load_store_op_none)
  // #527. The same, promoted to KHR.
  XE_UI_VULKAN_STRUCT_EXTENSION(KHR_load_store_op_none)
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    // #55. Dynamic rendering removes render pass/framebuffer overhead.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dynamic_rendering, 1, 3)
  }

  if (with_swapchain) {
    // #2.
    XE_UI_VULKAN_STRUCT_EXTENSION(KHR_swapchain)
#if XE_PLATFORM_WIN32
    // #256. Windows-only extension to control fullscreen exclusive behavior.
    // Used to prevent HDR state corruption during fullscreen transitions.
    // Requires the VK_KHR_get_surface_capabilities2 instance extension.
    if (vulkan_instance->extensions().ext_KHR_get_surface_capabilities2) {
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_full_screen_exclusive)
    }
#endif
  }

  bool ext_1_2_KHR_sampler_mirror_clamp_to_edge = false;
  bool ext_1_2_EXT_host_query_reset = false;
  bool ext_1_2_KHR_shader_float16_int8 = false;
  bool ext_1_1_KHR_maintenance1 = false;
  bool ext_1_2_KHR_shader_float_controls = false;
  bool ext_EXT_fragment_shader_interlock = false;
  bool ext_1_3_EXT_shader_demote_to_helper_invocation = false;
  bool ext_1_3_KHR_dynamic_rendering = false;
  bool ext_EXT_non_seamless_cube_map = false;
  bool ext_EXT_custom_border_color = false;
  bool ext_1_3_EXT_subgroup_size_control = false;
  bool ext_KHR_fragment_shader_barycentric = false;
  bool ext_NV_fragment_shader_barycentric = false;
  if (with_gpu_emulation) {
    // #15.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_sampler_mirror_clamp_to_edge, 1,
                                          2)
    // #70. Must be enabled for VK_KHR_sampler_ycbcr_conversion.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_maintenance1, 1, 1)
    // #141.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_shader_stencil_export)
    // #148.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_image_format_list, 1, 2)
    if (get_physical_device_properties2_supported) {
      // #157.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_sampler_ycbcr_conversion, 1, 1)
      // #198. Also must be enabled for VK_KHR_spirv_1_4.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_shader_float_controls, 1, 2)
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(EXT_host_query_reset, 1, 2)
      // #83. Float16 and Int16 capabilities are declared by system shaders.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_shader_float16_int8, 1, 2)
      // #252.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_fragment_shader_interlock)
      // #55.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_dynamic_rendering, 1, 3)
      // #233. In-pass reads of current attachments for tiler-native resolves.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dynamic_rendering_local_read,
                                             1, 4)
      // #277.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(
          EXT_shader_demote_to_helper_invocation, 1, 3)
      // #423.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_non_seamless_cube_map)
      // #288. Custom sampler border colors (for YCbCr border colors).
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_custom_border_color)
      // #226.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(EXT_subgroup_size_control, 1, 3)
      // #322 (KHR) / #203 (NV). Barycentric coordinates for manual
      // interpolation.
      XE_UI_VULKAN_LOCAL_EXTENSION(KHR_fragment_shader_barycentric)
      XE_UI_VULKAN_LOCAL_EXTENSION(NV_fragment_shader_barycentric)
      // #456. Per-sub-feature dynamic state used to collapse pipeline key
      // permutations. EDS1/EDS2 are core in 1.3; only EDS3 needs an extension.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_extended_dynamic_state3)
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      // #237.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_spirv_1_4, 1, 2)
    }
    // #342. Driver-side fault description after VK_ERROR_DEVICE_LOST.
    if (get_physical_device_properties2_supported) {
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_device_fault)
    }
    // #227. Requires VK_KHR_create_renderpass2, core since 1.2.
    if (cvars::vulkan_fragment_shading_rate_dynamic &&
        get_physical_device_properties2_supported &&
        properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      XE_UI_VULKAN_STRUCT_EXTENSION(KHR_fragment_shading_rate)
    }
    // #377. Requires VK_KHR_create_renderpass2 and VK_KHR_depth_stencil_resolve,
    // both core since 1.2.
    if (cvars::vulkan_msaa_single_sample_resolve &&
        get_physical_device_properties2_supported &&
        properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_multisampled_render_to_single_sampled)
    }
    if (get_physical_device_properties2_supported) {
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_image_compression_control)
    }
    // #179. Import guest RAM as device memory for a zero-copy shared-memory
    // buffer that aliases guest RAM.
    if (get_physical_device_properties2_supported) {
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_external_memory_host)
    }
  }

#undef XE_UI_VULKAN_STRUCT_EXTENSION
#undef XE_UI_VULKAN_LOCAL_EXTENSION
#undef XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION
#undef XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION

  std::vector<const char*> enabled_extensions;
  {
    uint32_t supported_extension_count = 0;
    const VkResult get_supported_extension_count_result =
        ifn.vkEnumerateDeviceExtensionProperties(
            physical_device, nullptr, &supported_extension_count, nullptr);
    if (get_supported_extension_count_result != VK_SUCCESS &&
        get_supported_extension_count_result != VK_INCOMPLETE) {
      XELOGW("Failed to get the Vulkan device '{}' extension count",
             properties.deviceName);
      return nullptr;
    }
    if (supported_extension_count) {
      std::vector<VkExtensionProperties> supported_extensions(
          supported_extension_count);
      if (ifn.vkEnumerateDeviceExtensionProperties(
              physical_device, nullptr, &supported_extension_count,
              supported_extensions.data()) != VK_SUCCESS) {
        XELOGW("Failed to get the Vulkan device '{}' extensions",
               properties.deviceName);
        return nullptr;
      }
      assert_true(supported_extension_count == supported_extensions.size());
      {
        // One-line list of everything the driver offers (driver comparisons).
        std::string all_names;
        for (const VkExtensionProperties& e : supported_extensions) {
          all_names += ' ';
          all_names += e.extensionName;
        }
        XELOGI("Vulkan device extensions ({}):{}", supported_extensions.size(),
               all_names);
      }
      if (cvars::vulkan_log_capability_audit && with_gpu_emulation) {
        LogCapabilityAudit(vulkan_instance, physical_device, supported_extensions);
      }
      for (const VkExtensionProperties& supported_extension :
           supported_extensions) {
        const auto requested_extension_it =
            requested_extensions.find(supported_extension.extensionName);
        if (requested_extension_it == requested_extensions.cend()) {
          continue;
        }
        assert_not_null(requested_extension_it->second);
        if (!*requested_extension_it->second) {
          enabled_extensions.emplace_back(
              requested_extension_it->first.c_str());
          *requested_extension_it->second = true;
        }
      }
    }
  }

  if (with_swapchain && !device->extensions_.ext_KHR_swapchain) {
    XELOGW("Vulkan device '{}' doesn't support swapchains",
           properties.deviceName);
    return nullptr;
  }

  VkDeviceCreateInfo device_create_info = {
      VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

  device_create_info.enabledExtensionCount =
      uint32_t(enabled_extensions.size());
  device_create_info.ppEnabledExtensionNames = enabled_extensions.data();

  // Get supported Vulkan 1.1+ and extension properties and features.
  //
  // The property and feature structures are initialized to zero or to the
  // minimum / maximum requirements for the simplicity of handling unavailable
  // VK_KHR_get_physical_device_properties2.

  VkPhysicalDeviceProperties2 properties_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

  VkPhysicalDeviceFeatures2 supported_features_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

  VulkanFeatures<VkPhysicalDeviceVulkan11Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES>
      features_1_1;
  VulkanFeatures<VkPhysicalDeviceVulkan12Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES>
      features_1_2;
  VulkanFeatures<VkPhysicalDeviceHostQueryResetFeatures,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES>
      features_EXT_host_query_reset;
  VulkanFeatures<VkPhysicalDeviceShaderFloat16Int8Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES>
      features_KHR_shader_float16_int8;
  VulkanFeatures<VkPhysicalDeviceVulkan13Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES>
      features_1_3;
  VulkanFeatures<
      VkPhysicalDevicePortabilitySubsetFeaturesKHR,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR>
      features_KHR_portability_subset;
  VkPhysicalDeviceDriverPropertiesKHR properties_1_2_KHR_driver_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceFloatControlsProperties
      properties_1_2_KHR_shader_float_controls = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
  VulkanFeatures<
      VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT>
      features_EXT_fragment_shader_interlock;
  VulkanFeatures<
      VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT>
      features_1_3_EXT_shader_demote_to_helper_invocation;
  VulkanFeatures<
      VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT>
      features_EXT_non_seamless_cube_map;
  VulkanFeatures<
      VkPhysicalDeviceCustomBorderColorFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT>
      features_EXT_custom_border_color;
  VulkanFeatures<VkPhysicalDeviceDynamicRenderingFeatures,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES>
      features_1_3_KHR_dynamic_rendering;
  VulkanFeatures<
      VkPhysicalDeviceDynamicRenderingLocalReadFeatures,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES>
      features_1_4_KHR_dynamic_rendering_local_read;
  // Vulkan 1.1 core subgroup properties.
  VkPhysicalDeviceSubgroupProperties properties_subgroup = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  // VK_EXT_subgroup_size_control (#226, promoted to 1.3).
  VkPhysicalDeviceSubgroupSizeControlProperties
      properties_1_3_EXT_subgroup_size_control = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
  VulkanFeatures<
      VkPhysicalDeviceSubgroupSizeControlFeatures,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES>
      features_1_3_EXT_subgroup_size_control;
  // VK_KHR_fragment_shader_barycentric (#322) /
  // VK_NV_fragment_shader_barycentric (#203). KHR and NV share the same feature
  // structure type.
  VulkanFeatures<
      VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR>
      features_KHR_fragment_shader_barycentric;
  VulkanFeatures<VkPhysicalDeviceFaultFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT>
      features_EXT_device_fault;
  VulkanFeatures<
      VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT>
      features_EXT_multisampled_render_to_single_sampled;
  VulkanFeatures<
      VkPhysicalDeviceFragmentShadingRateFeaturesKHR,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR>
      features_KHR_fragment_shading_rate;
  VulkanFeatures<
      VkPhysicalDeviceImageCompressionControlFeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT>
      features_EXT_image_compression_control;
  // VK_EXT_extended_dynamic_state3 (#456). Per-sub-feature dynamic state.
  VulkanFeatures<
      VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT>
      features_EXT_extended_dynamic_state3;
  // dynamicPrimitiveTopologyUnrestricted is a property, not a feature.
  VkPhysicalDeviceExtendedDynamicState3PropertiesEXT
      properties_EXT_extended_dynamic_state3 = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT};
  // VK_EXT_external_memory_host (#179) properties (host pointer import
  // alignment).
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT
      properties_EXT_external_memory_host = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};

  if (get_physical_device_properties2_supported) {
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      features_1_1.Link(supported_features_2, device_create_info);
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      features_1_2.Link(supported_features_2, device_create_info);
    } else {
      if (ext_1_2_EXT_host_query_reset) {
        features_EXT_host_query_reset.Link(supported_features_2,
                                           device_create_info);
      }
      if (ext_1_2_KHR_shader_float16_int8) {
        features_KHR_shader_float16_int8.Link(supported_features_2,
                                              device_create_info);
      }
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      features_1_3.Link(supported_features_2, device_create_info);
    } else {
      if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
        features_1_3_EXT_shader_demote_to_helper_invocation.Link(
            supported_features_2, device_create_info);
      }
      if (ext_1_3_KHR_dynamic_rendering) {
        features_1_3_KHR_dynamic_rendering.Link(supported_features_2,
                                                device_create_info);
      }
    }
    if (device->extensions_.ext_1_4_KHR_dynamic_rendering_local_read) {
      features_1_4_KHR_dynamic_rendering_local_read.Link(supported_features_2,
                                                         device_create_info);
    }
    if (ext_KHR_portability_subset) {
      features_KHR_portability_subset.Link(supported_features_2,
                                           device_create_info);
    }
    if (ext_1_2_KHR_driver_properties) {
      properties_1_2_KHR_driver_properties.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_driver_properties;
    }
    if (ext_1_2_KHR_shader_float_controls) {
      properties_1_2_KHR_shader_float_controls.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_shader_float_controls;
    }
    if (ext_EXT_fragment_shader_interlock) {
      features_EXT_fragment_shader_interlock.Link(supported_features_2,
                                                  device_create_info);
    }
    if (ext_EXT_non_seamless_cube_map) {
      features_EXT_non_seamless_cube_map.Link(supported_features_2,
                                              device_create_info);
    }
    if (ext_EXT_custom_border_color) {
      features_EXT_custom_border_color.Link(supported_features_2,
                                            device_create_info);
    }
    // Subgroup properties are Vulkan 1.1 core.
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      properties_subgroup.pNext = properties_2.pNext;
      properties_2.pNext = &properties_subgroup;
    }
    // VK_EXT_subgroup_size_control properties and features.
    if (ext_1_3_EXT_subgroup_size_control) {
      properties_1_3_EXT_subgroup_size_control.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_3_EXT_subgroup_size_control;
      // On Vulkan 1.3 these features come from VkPhysicalDeviceVulkan13Features
      // (linked above). The standalone structure must not also be in the chain.
      if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
        features_1_3_EXT_subgroup_size_control.Link(supported_features_2,
                                                    device_create_info);
      }
    }
    // VK_KHR_fragment_shader_barycentric / VK_NV_fragment_shader_barycentric.
    if (ext_KHR_fragment_shader_barycentric ||
        ext_NV_fragment_shader_barycentric) {
      features_KHR_fragment_shader_barycentric.Link(supported_features_2,
                                                    device_create_info);
    }
    if (device->extensions_.ext_EXT_device_fault) {
      features_EXT_device_fault.Link(supported_features_2, device_create_info);
    }
    if (device->extensions_.ext_EXT_image_compression_control) {
      features_EXT_image_compression_control.Link(supported_features_2,
                                                  device_create_info);
    }
    if (device->extensions_.ext_KHR_fragment_shading_rate) {
      features_KHR_fragment_shading_rate.Link(supported_features_2,
                                              device_create_info);
    }
    if (device->extensions_.ext_EXT_multisampled_render_to_single_sampled) {
      features_EXT_multisampled_render_to_single_sampled.Link(
          supported_features_2, device_create_info);
    }
    if (device->extensions_.ext_EXT_extended_dynamic_state3) {
      features_EXT_extended_dynamic_state3.Link(supported_features_2,
                                                device_create_info);
      properties_EXT_extended_dynamic_state3.pNext = properties_2.pNext;
      properties_2.pNext = &properties_EXT_extended_dynamic_state3;
    }
    if (device->extensions_.ext_EXT_external_memory_host) {
      properties_EXT_external_memory_host.pNext = properties_2.pNext;
      properties_2.pNext = &properties_EXT_external_memory_host;
    }
    ifn.vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported_features_2);
    // Mirror supported deviceFault into the enabled struct so the driver
    // collects fault info during normal execution. Disable the extension flag
    // if the feature wasn't actually supported - vkGetDeviceFaultInfoEXT is
    // only valid to call when deviceFault was enabled at device creation.
    if (device->extensions_.ext_EXT_device_fault &&
        !features_EXT_device_fault.supported.deviceFault) {
      device->extensions_.ext_EXT_device_fault = false;
    }
    features_EXT_device_fault.enabled.deviceFault =
        features_EXT_device_fault.supported.deviceFault;
    if (device->extensions_.ext_EXT_image_compression_control &&
        !features_EXT_image_compression_control.supported
             .imageCompressionControl) {
      device->extensions_.ext_EXT_image_compression_control = false;
    }
    if (device->extensions_.ext_EXT_multisampled_render_to_single_sampled &&
        !features_EXT_multisampled_render_to_single_sampled.supported
             .multisampledRenderToSingleSampled) {
      device->extensions_.ext_EXT_multisampled_render_to_single_sampled = false;
    }
    features_EXT_multisampled_render_to_single_sampled.enabled
        .multisampledRenderToSingleSampled =
        features_EXT_multisampled_render_to_single_sampled.supported
            .multisampledRenderToSingleSampled;
    // Only the per-draw (pipeline) rate is used.
    if (device->extensions_.ext_KHR_fragment_shading_rate &&
        !features_KHR_fragment_shading_rate.supported
             .pipelineFragmentShadingRate) {
      device->extensions_.ext_KHR_fragment_shading_rate = false;
    }
    features_KHR_fragment_shading_rate.enabled.pipelineFragmentShadingRate =
        features_KHR_fragment_shading_rate.supported
            .pipelineFragmentShadingRate;
    features_EXT_image_compression_control.enabled.imageCompressionControl =
        features_EXT_image_compression_control.supported
            .imageCompressionControl;
  }

  uint32_t queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device,
                                               &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(
      physical_device, &queue_family_count, queue_families.data());

  device->queue_families_.resize(queue_family_count);

  uint32_t first_queue_family_graphics_compute_sparse_binding = UINT32_MAX;
  uint32_t first_queue_family_graphics_compute = UINT32_MAX;
  uint32_t first_queue_family_sparse_binding = UINT32_MAX;
  uint32_t first_queue_family_transfer = UINT32_MAX;
  bool has_presentation_queue_family = false;

  for (uint32_t queue_family_index = 0; queue_family_index < queue_family_count;
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    const VkQueueFamilyProperties& queue_family_properties =
        queue_families[queue_family_index];

    const VkQueueFlags queue_unsupported_flags =
        ~queue_family_properties.queueFlags;

    if (!(queue_unsupported_flags &
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      first_queue_family_graphics_compute =
          std::min(queue_family_index, first_queue_family_graphics_compute);
    }

    if (with_gpu_emulation && supported_features.sparseBinding &&
        !(queue_unsupported_flags & VK_QUEUE_SPARSE_BINDING_BIT)) {
      first_queue_family_sparse_binding =
          std::min(queue_family_index, first_queue_family_sparse_binding);
      if (!(queue_unsupported_flags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        first_queue_family_graphics_compute_sparse_binding =
            std::min(queue_family_index,
                     first_queue_family_graphics_compute_sparse_binding);
      }
    }

    // A transfer-only family (transfer supported, graphics and compute not) is
    // the dedicated DMA / copy engine, used for resolve readback copies.
    if (with_gpu_emulation &&
        !(queue_unsupported_flags & VK_QUEUE_TRANSFER_BIT) &&
        (queue_unsupported_flags & VK_QUEUE_GRAPHICS_BIT) &&
        (queue_unsupported_flags & VK_QUEUE_COMPUTE_BIT)) {
      first_queue_family_transfer =
          std::min(queue_family_index, first_queue_family_transfer);
    }

    if (with_swapchain) {
#if XE_PLATFORM_WIN32
      queue_family.may_support_presentation =
          vulkan_instance->extensions().ext_KHR_win32_surface &&
          ifn.vkGetPhysicalDeviceWin32PresentationSupportKHR(
              physical_device, queue_family_index);
#else
      queue_family.may_support_presentation = true;
#endif
      if (queue_family.may_support_presentation) {
        queue_family.queues.resize(
            std::max(size_t(1), queue_family.queues.size()));
        has_presentation_queue_family = true;
      }
    }
  }

  if (first_queue_family_graphics_compute == UINT32_MAX) {
    // Not valid according to the Vulkan specification, but for safety.
    XELOGW(
        "Vulkan device '{}' doesn't provide a graphics and compute queue "
        "family",
        properties.deviceName);
    return nullptr;
  }

  if (with_swapchain && !has_presentation_queue_family) {
    XELOGW(
        "Vulkan device '{}' doesn't provide a queue family that supports "
        "presentation",
        properties.deviceName);
    return nullptr;
  }

  // Get the queues to create.

  if (first_queue_family_sparse_binding == UINT32_MAX) {
    // Not valid not to provide a sparse binding queue if the sparseBinding
    // feature is supported according to the Vulkan specification, but for
    // safety and simplicity.
    supported_features.sparseBinding = VK_FALSE;
  }
  if (!supported_features.sparseBinding) {
    supported_features.sparseResidencyBuffer = VK_FALSE;
    supported_features.sparseResidencyImage2D = VK_FALSE;
    supported_features.sparseResidencyImage3D = VK_FALSE;
    supported_features.sparseResidency2Samples = VK_FALSE;
    supported_features.sparseResidency4Samples = VK_FALSE;
    supported_features.sparseResidency8Samples = VK_FALSE;
    supported_features.sparseResidency16Samples = VK_FALSE;
    supported_features.sparseResidencyAliased = VK_FALSE;
  }

  // Prefer using one queue for everything whenever possible for simplicity.
  // TODO(Triang3l): Research if separate queues for purposes like composition,
  // swapchain image presentation, and sparse binding, may be beneficial.

  if (first_queue_family_graphics_compute_sparse_binding != UINT32_MAX) {
    device->queue_family_graphics_compute_ =
        first_queue_family_graphics_compute_sparse_binding;
    device->queue_family_sparse_binding_ =
        first_queue_family_graphics_compute_sparse_binding;
  } else {
    device->queue_family_graphics_compute_ =
        first_queue_family_graphics_compute;
    device->queue_family_sparse_binding_ = first_queue_family_sparse_binding;
  }

  // A second graphics queue lets frame generation present its frames on time
  // while the emulation keeps the first one busy with long submissions.
  size_t graphics_compute_queue_count = 1;
  if (cvars::frame_generation >= 2 &&
      queue_families[device->queue_family_graphics_compute_].queueCount >= 2) {
    graphics_compute_queue_count = 2;
  }
  device->queue_families_[device->queue_family_graphics_compute_].queues.resize(
      std::max(graphics_compute_queue_count,
               device->queue_families_[device->queue_family_graphics_compute_]
                   .queues.size()));
  if (device->queue_family_sparse_binding_ != UINT32_MAX) {
    device->queue_families_[device->queue_family_sparse_binding_].queues.resize(
        std::max(size_t(1),
                 device->queue_families_[device->queue_family_sparse_binding_]
                     .queues.size()));
  }
  device->queue_family_transfer_ = first_queue_family_transfer;
  if (device->queue_family_transfer_ != UINT32_MAX) {
    device->queue_families_[device->queue_family_transfer_].queues.resize(
        std::max(size_t(1),
                 device->queue_families_[device->queue_family_transfer_]
                     .queues.size()));
    XELOGI(
        "VulkanDevice: using dedicated transfer queue family {} for readback",
        device->queue_family_transfer_);
  } else {
    XELOGI(
        "VulkanDevice: no dedicated transfer queue family; readback copies "
        "stay "
        "on the graphics queue");
  }

  size_t max_enabled_queues_per_family = 0;
  for (const QueueFamily& queue_family : device->queue_families_) {
    max_enabled_queues_per_family =
        std::max(queue_family.queues.size(), max_enabled_queues_per_family);
  }
  // Queue 0 of a family is the emulation queue; frame generation paints on
  // queue 1 of the graphics family and should be scheduled ahead of it.
  std::vector<float> queue_priorities(max_enabled_queues_per_family, 1.0f);
  if (max_enabled_queues_per_family > 1) {
    queue_priorities[0] = float(std::min(
        std::max(cvars::vulkan_emulation_queue_priority, 0.0), 1.0));
  }
  std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
  for (size_t queue_family_index = 0;
       queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    const QueueFamily& queue_family =
        device->queue_families_[queue_family_index];
    if (queue_family.queues.empty()) {
      continue;
    }
    VkDeviceQueueCreateInfo& queue_create_info =
        queue_create_infos.emplace_back();
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.pNext = nullptr;
    queue_create_info.flags = 0;
    queue_create_info.queueFamilyIndex = uint32_t(queue_family_index);
    queue_create_info.queueCount = uint32_t(queue_family.queues.size());
    queue_create_info.pQueuePriorities = queue_priorities.data();
  }
  device_create_info.queueCreateInfoCount = uint32_t(queue_create_infos.size());
  device_create_info.pQueueCreateInfos = queue_create_infos.data();

  // Enable needed features and copy the properties.
  //
  // Enabling only actually used features because drivers may take more optimal
  // paths when certain features are disabled. Also, in VK_EXT_shader_object,
  // the state that the application must set for the draw depends on which
  // features are enabled.

  device->properties_.apiVersion = properties.apiVersion;
  device->properties_.driverVersion = properties.driverVersion;
  device->properties_.vendorID = properties.vendorID;
  device->properties_.deviceID = properties.deviceID;
  std::strcpy(device->properties_.deviceName, properties.deviceName);

  XELOGI(
      "Vulkan device '{}': API {}.{}.{} ({}.{} used), vendor 0x{:04X}, device "
      "0x{:04X}, driver version 0x{:X}",
      properties.deviceName, VK_VERSION_MAJOR(unclamped_api_version),
      VK_VERSION_MINOR(unclamped_api_version),
      VK_VERSION_PATCH(properties.apiVersion),
      VK_VERSION_MAJOR(properties.apiVersion),
      VK_VERSION_MINOR(properties.apiVersion), properties.vendorID,
      properties.deviceID, properties.driverVersion);

  XELOGI("Enabled Vulkan device extensions:");
  for (uint32_t enabled_extension_index = 0;
       enabled_extension_index < device_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    XELOGI("* {}",
           device_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  XELOGI("Vulkan device properties and enabled features:");

  VkPhysicalDeviceFeatures enabled_features = {};
  device_create_info.pEnabledFeatures = &enabled_features;

#define XE_UI_VULKAN_LIMIT(name)                     \
  device->properties_.name = properties.limits.name; \
  XELOGI("* " #name ": {}", properties.limits.name);
#define XE_UI_VULKAN_ENUM_LIMIT(name, type)          \
  device->properties_.name = properties.limits.name; \
  XELOGI("* " #name ": {}", vk::to_string(vk::type(properties.limits.name)));
#define XE_UI_VULKAN_FEATURE(name)                    \
  enabled_features.name = supported_features.name;    \
  device->properties_.name = supported_features.name; \
  if (supported_features.name) {                      \
    XELOGI("* " #name);                               \
  }
#define XE_UI_VULKAN_PROPERTY_2(structure, name) \
  device->properties_.name = structure.name;     \
  XELOGI("* " #name ": {}", structure.name);
#define XE_UI_VULKAN_ENUM_PROPERTY_2(structure, name, type) \
  device->properties_.name = structure.name;                \
  XELOGI("* " #name ": {}", vk::to_string(vk::type(structure.name)));
#define XE_UI_VULKAN_FEATURE_2(structure, name)        \
  structure.enabled.name = structure.supported.name;   \
  device->properties_.name = structure.supported.name; \
  if (structure.supported.name) {                      \
    XELOGI("* " #name);                                \
  }
#define XE_UI_VULKAN_FEATURE_IMPLIED(name) \
  device->properties_.name = true;         \
  XELOGI("* " #name);

  bool mali_dynamic_rendering_erratum = false;
  if (ext_1_2_KHR_driver_properties) {
    XE_UI_VULKAN_ENUM_PROPERTY_2(properties_1_2_KHR_driver_properties, driverID,
                                 DriverId);
    XELOGI("* driverName: {}", properties_1_2_KHR_driver_properties.driverName);
    if (properties_1_2_KHR_driver_properties.driverInfo[0]) {
      XELOGI("* driverInfo: {}",
             properties_1_2_KHR_driver_properties.driverInfo);
    }
    if (properties_1_2_KHR_driver_properties.driverID ==
        VK_DRIVER_ID_ARM_PROPRIETARY) {
      // Mali drivers before r52 have dynamic rendering bugs (ANGLE disables it
      // there too), e.g. erratum 3786926 (r49p1-r51p0): vkCmdBeginRendering
      // reuses stale internal resources (black tiles). Arm's workaround is
      // static render passes. driverInfo looks like "v1.r49p1-03bet0.<hash>".
      unsigned r = 0, p = 0;
      for (const char* s = properties_1_2_KHR_driver_properties.driverInfo;
           *s; ++s) {
        if (*s == 'r' && std::sscanf(s, "r%up%u", &r, &p) == 2) {
          mali_dynamic_rendering_erratum = r < 52;
          break;
        }
      }
      if (mali_dynamic_rendering_erratum) {
        XELOGI("Mali r{}p{}: using render passes instead of dynamic rendering",
               r, p);
      }
    }
    XELOGI("* conformanceVersion: {}.{}.{}.{}",
           properties_1_2_KHR_driver_properties.conformanceVersion.major,
           properties_1_2_KHR_driver_properties.conformanceVersion.minor,
           properties_1_2_KHR_driver_properties.conformanceVersion.subminor,
           properties_1_2_KHR_driver_properties.conformanceVersion.patch);
  }

  XE_UI_VULKAN_LIMIT(maxImageDimension2D)
  XE_UI_VULKAN_LIMIT(maxImageDimension3D)
  XE_UI_VULKAN_LIMIT(maxImageDimensionCube)
  XE_UI_VULKAN_LIMIT(maxImageArrayLayers)
  XE_UI_VULKAN_LIMIT(maxStorageBufferRange)
  if (cvars::vulkan_clamp_storage_buffer_range > 0) {
    device->properties_.maxStorageBufferRange =
        std::min(device->properties_.maxStorageBufferRange,
                 uint32_t(cvars::vulkan_clamp_storage_buffer_range));
    XELOGI("* maxStorageBufferRange clamped to {} by configuration",
           device->properties_.maxStorageBufferRange);
  }
  XE_UI_VULKAN_LIMIT(maxSamplerAllocationCount)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSamplers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorStorageBuffers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSampledImages)
  XE_UI_VULKAN_LIMIT(maxPerStageResources)
  XE_UI_VULKAN_LIMIT(maxVertexOutputComponents)
  XE_UI_VULKAN_LIMIT(maxTessellationEvaluationOutputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryInputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryOutputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentInputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentCombinedOutputResources)
  XE_UI_VULKAN_LIMIT(maxSamplerAnisotropy)
  XE_UI_VULKAN_LIMIT(timestampPeriod)
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[0])
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[1])
  XE_UI_VULKAN_LIMIT(minUniformBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(maxUniformBufferRange)
  XE_UI_VULKAN_LIMIT(maxDrawIndirectCount)
  XE_UI_VULKAN_LIMIT(maxDescriptorSetUniformBuffersDynamic)
  XE_UI_VULKAN_LIMIT(minStorageBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(maxFramebufferWidth)
  XE_UI_VULKAN_LIMIT(maxFramebufferHeight)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferNoAttachmentsSampleCounts,
                          SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageIntegerSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_LIMIT(standardSampleLocations)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyOffsetAlignment)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyRowPitchAlignment)
  XE_UI_VULKAN_LIMIT(nonCoherentAtomSize)

  if (with_gpu_emulation) {
    XE_UI_VULKAN_FEATURE(robustBufferAccess)
    XE_UI_VULKAN_FEATURE(fullDrawIndexUint32)
    XE_UI_VULKAN_FEATURE(multiDrawIndirect)
    XE_UI_VULKAN_FEATURE(drawIndirectFirstInstance)
    XELOGI(
        "DrawBatchFeatures: enabled multiDrawIndirect={} "
        "drawIndirectFirstInstance={} maxDrawIndirectCount={}",
        device->properties_.multiDrawIndirect,
        device->properties_.drawIndirectFirstInstance,
        device->properties_.maxDrawIndirectCount);
    XE_UI_VULKAN_FEATURE(independentBlend)
    XE_UI_VULKAN_FEATURE(geometryShader)
    XE_UI_VULKAN_FEATURE(tessellationShader)
    XE_UI_VULKAN_FEATURE(sampleRateShading)
    XE_UI_VULKAN_FEATURE(depthClamp)
    XE_UI_VULKAN_FEATURE(fillModeNonSolid)
    XE_UI_VULKAN_FEATURE(samplerAnisotropy)
    XE_UI_VULKAN_FEATURE(occlusionQueryPrecise)
    XE_UI_VULKAN_FEATURE(vertexPipelineStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(fragmentStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(shaderClipDistance)
    XE_UI_VULKAN_FEATURE(shaderCullDistance)
    XE_UI_VULKAN_FEATURE(shaderInt16)
    XE_UI_VULKAN_FEATURE(shaderSampledImageArrayDynamicIndexing)
    XE_UI_VULKAN_FEATURE(sparseBinding)
    XE_UI_VULKAN_FEATURE(sparseResidencyBuffer)
    if (!cvars::vulkan_robust_buffer_access &&
        enabled_features.robustBufferAccess) {
      enabled_features.robustBufferAccess = VK_FALSE;
      device->properties_.robustBufferAccess = false;
      XELOGI("* robustBufferAccess disabled by vulkan_robust_buffer_access");
    }
  }
  // Storage writes to the 10:10:10:2 guest output (gamma application, frame
  // generation).
  XE_UI_VULKAN_FEATURE(shaderStorageImageExtendedFormats)
  // ASTC images (DXT1 transcoding on devices without BC) require the feature to
  // be enabled, not only format support.
  enabled_features.textureCompressionASTC_LDR =
      supported_features.textureCompressionASTC_LDR;
  if (supported_features.textureCompressionASTC_LDR) {
    XELOGI("* textureCompressionASTC_LDR");
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_2, samplerMirrorClampToEdge);
      XE_UI_VULKAN_FEATURE_2(features_1_2, uniformBufferStandardLayout);
      XE_UI_VULKAN_FEATURE_2(features_1_2, scalarBlockLayout);
      XE_UI_VULKAN_FEATURE_2(features_1_2,
                             descriptorBindingSampledImageUpdateAfterBind);
      XE_UI_VULKAN_FEATURE_2(features_1_2, descriptorBindingPartiallyBound);
      XE_UI_VULKAN_FEATURE_2(features_1_2,
                             shaderSampledImageArrayNonUniformIndexing);
      XE_UI_VULKAN_FEATURE_2(features_1_2,
                             descriptorBindingUpdateUnusedWhilePending);
      XE_UI_VULKAN_FEATURE_2(features_1_2, hostQueryReset);
      XE_UI_VULKAN_FEATURE_2(features_1_2, shaderFloat16);
      // Frame generation synchronization between the emulation and the
      // presentation queues.
      if (cvars::frame_generation >= 2) {
        XE_UI_VULKAN_FEATURE_2(features_1_2, timelineSemaphore);
      }
    }
  } else {
    if (ext_1_2_KHR_sampler_mirror_clamp_to_edge) {
      XE_UI_VULKAN_FEATURE_IMPLIED(samplerMirrorClampToEdge)
    }
    if (ext_1_2_EXT_host_query_reset && with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_host_query_reset, hostQueryReset);
    }
    if (ext_1_2_KHR_shader_float16_int8 && with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_KHR_shader_float16_int8, shaderFloat16);
    }
  }
  device->extensions_.ext_1_2_EXT_host_query_reset =
      ext_1_2_EXT_host_query_reset;

  // shaderDrawParameters (Vulkan 1.1). Needed by shaders reading SV_VertexID
  // with Direct3D semantics, which are compiled to VertexIndex minus BaseVertex
  // (declaring the DrawParameters SPIR-V capability). The guest output triangle
  // strip vertex shader used by the presenter is one such shader.
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    features_1_1.enabled.shaderDrawParameters =
        features_1_1.supported.shaderDrawParameters;
    device->properties_.shaderDrawParameters =
        features_1_1.enabled.shaderDrawParameters != VK_FALSE;
    if (features_1_1.supported.shaderDrawParameters) {
      XELOGI("* shaderDrawParameters");
    }
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_3, shaderDemoteToHelperInvocation);
      if (!mali_dynamic_rendering_erratum) {
        XE_UI_VULKAN_FEATURE_2(features_1_3, dynamicRendering);
      }
    }
  } else {
    if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
      if (with_gpu_emulation) {
        XE_UI_VULKAN_FEATURE_2(
            features_1_3_EXT_shader_demote_to_helper_invocation,
            shaderDemoteToHelperInvocation);
      }
    }
    if (ext_1_3_KHR_dynamic_rendering && !mali_dynamic_rendering_erratum) {
      if (with_gpu_emulation) {
        XE_UI_VULKAN_FEATURE_2(features_1_3_KHR_dynamic_rendering,
                               dynamicRendering);
      }
    }
  }

  if (device->extensions_.ext_1_4_KHR_dynamic_rendering_local_read &&
      with_gpu_emulation && !mali_dynamic_rendering_erratum) {
    XE_UI_VULKAN_FEATURE_2(features_1_4_KHR_dynamic_rendering_local_read,
                           dynamicRenderingLocalRead);
  }

  if (ext_KHR_portability_subset) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             constantAlphaColorBlendFactors)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             imageViewFormatReinterpretation)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             imageViewFormatSwizzle)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, pointPolygons)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             separateStencilMaskRef)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             shaderSampleRateInterpolationFunctions)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, triangleFans)
    }
  } else {
    // Not a portability subset device.
    XE_UI_VULKAN_FEATURE_IMPLIED(constantAlphaColorBlendFactors)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatReinterpretation)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatSwizzle)
    XE_UI_VULKAN_FEATURE_IMPLIED(pointPolygons)
    XE_UI_VULKAN_FEATURE_IMPLIED(separateStencilMaskRef)
    XE_UI_VULKAN_FEATURE_IMPLIED(shaderSampleRateInterpolationFunctions)
    XE_UI_VULKAN_FEATURE_IMPLIED(triangleFans)
  }

  if (ext_1_2_KHR_shader_float_controls) {
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderSignedZeroInfNanPreserveFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderDenormFlushToZeroFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderRoundingModeRTEFloat32);
  }

  if (ext_EXT_fragment_shader_interlock) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock,
                             fragmentShaderSampleInterlock)
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock,
                             fragmentShaderPixelInterlock)
    }
  }

  if (ext_EXT_non_seamless_cube_map) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_non_seamless_cube_map,
                             nonSeamlessCubeMap)
    }
  }

  if (ext_EXT_custom_border_color) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color,
                             customBorderColors)
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color,
                             customBorderColorWithoutFormat)
    }
  }

  // Vulkan 1.1 core subgroup properties.
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    XE_UI_VULKAN_PROPERTY_2(properties_subgroup, subgroupSize);
    device->properties_.subgroupSupportedStages =
        properties_subgroup.supportedStages;
    XELOGI("* subgroupSupportedStages: {}",
           vk::to_string(
               vk::ShaderStageFlags(properties_subgroup.supportedStages)));
    device->properties_.subgroupSupportedOperations =
        properties_subgroup.supportedOperations;
    XELOGI("* subgroupSupportedOperations: {}",
           vk::to_string(vk::SubgroupFeatureFlags(
               properties_subgroup.supportedOperations)));
  }

  // VK_EXT_subgroup_size_control (#226, promoted to 1.3).
  if (ext_1_3_EXT_subgroup_size_control) {
    XE_UI_VULKAN_PROPERTY_2(properties_1_3_EXT_subgroup_size_control,
                            minSubgroupSize);
    XE_UI_VULKAN_PROPERTY_2(properties_1_3_EXT_subgroup_size_control,
                            maxSubgroupSize);
    if (with_gpu_emulation) {
      // On Vulkan 1.3 these are enabled through
      // VkPhysicalDeviceVulkan13Features.
      if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
        XE_UI_VULKAN_FEATURE_2(features_1_3, subgroupSizeControl);
        XE_UI_VULKAN_FEATURE_2(features_1_3, computeFullSubgroups);
      } else {
        XE_UI_VULKAN_FEATURE_2(features_1_3_EXT_subgroup_size_control,
                               subgroupSizeControl);
        XE_UI_VULKAN_FEATURE_2(features_1_3_EXT_subgroup_size_control,
                               computeFullSubgroups);
      }
    }
  }
  device->extensions_.ext_1_3_EXT_subgroup_size_control =
      ext_1_3_EXT_subgroup_size_control;

  // VK_EXT_external_memory_host (#179).
  if (device->extensions_.ext_EXT_external_memory_host) {
    XE_UI_VULKAN_PROPERTY_2(properties_EXT_external_memory_host,
                            minImportedHostPointerAlignment);
  }

  // VK_KHR_fragment_shader_barycentric (#322) /
  // VK_NV_fragment_shader_barycentric (#203).
  // MoltenVK advertises this, but SPIRV-Cross can't translate Xenia's
  // PerVertexKHR usage to MSL, so leave the shader path on its fallback.
  const bool driver_is_moltenvk =
      device->properties_.driverID == VK_DRIVER_ID_MOLTENVK;
  if ((ext_KHR_fragment_shader_barycentric ||
       ext_NV_fragment_shader_barycentric) &&
      !driver_is_moltenvk) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_KHR_fragment_shader_barycentric,
                             fragmentShaderBarycentric);
    }
  }
  device->extensions_.ext_KHR_fragment_shader_barycentric =
      (ext_KHR_fragment_shader_barycentric ||
       ext_NV_fragment_shader_barycentric) &&
      !driver_is_moltenvk;

  // Extended dynamic state. EDS1 (#268) and EDS2 (#378) are promoted to Vulkan
  // 1.3 core - always available on a 1.3 device. EDS3 (#456) is a standalone
  // extension with per-sub-feature bools; mirror only the sub-features Xenia
  // actually makes dynamic into the enabled struct (leaving the rest disabled
  // avoids requesting unsupported sub-features at device creation).
  if (with_gpu_emulation) {
    device->properties_.extendedDynamicState =
        properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0);
    if (device->properties_.extendedDynamicState) {
      XELOGI("* extendedDynamicState (core 1.3)");
    }
    if (device->extensions_.ext_EXT_extended_dynamic_state3 &&
        device->properties_.extendedDynamicState) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_extended_dynamic_state3,
                             extendedDynamicState3DepthClampEnable);
      XE_UI_VULKAN_FEATURE_2(features_EXT_extended_dynamic_state3,
                             extendedDynamicState3PolygonMode);
      XE_UI_VULKAN_FEATURE_2(features_EXT_extended_dynamic_state3,
                             extendedDynamicState3ColorBlendEnable);
      XE_UI_VULKAN_FEATURE_2(features_EXT_extended_dynamic_state3,
                             extendedDynamicState3ColorBlendEquation);
      XE_UI_VULKAN_FEATURE_2(features_EXT_extended_dynamic_state3,
                             extendedDynamicState3ColorWriteMask);
    }
  }
  // dynamicPrimitiveTopologyUnrestricted is a property of the extension, not a
  // feature to enable - read it from the properties structure (valid only when
  // the extension is present and queried above). When false, dynamic topology
  // must stay within its class, so the class is kept in the pipeline key.
  if (device->extensions_.ext_EXT_extended_dynamic_state3) {
    device->properties_.extendedDynamicState3PrimitiveTopologyUnrestricted =
        properties_EXT_extended_dynamic_state3
            .dynamicPrimitiveTopologyUnrestricted;
  }

#undef XE_UI_VULKAN_LIMIT
#undef XE_UI_VULKAN_ENUM_LIMIT
#undef XE_UI_VULKAN_FEATURE
#undef XE_UI_VULKAN_PROPERTY_2
#undef XE_UI_VULKAN_ENUM_PROPERTY_2
#undef XE_UI_VULKAN_FEATURE_2

  // Create the device.

  const VkResult device_create_result = ifn.vkCreateDevice(
      physical_device, &device_create_info, nullptr, &device->device_);
  if (device_create_result != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan logical device from the physical device "
        "'{}': {}",
        properties.deviceName, vk::to_string(vk::Result(device_create_result)));
    return nullptr;
  }

  // Load device functions.

  bool functions_loaded = true;

  Functions& dfn = device->functions_;

#define XE_UI_VULKAN_FUNCTION(name)                                   \
  functions_loaded &= (dfn.name = PFN_##name(ifn.vkGetDeviceProcAddr( \
                           device->device_, #name))) != nullptr;

  // Vulkan 1.0.
#include "xenia/ui/vulkan/functions/device_1_0.inc"

  // Extensions promoted to a Vulkan version supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &=                                             \
      (dfn.core_name = PFN_##core_name(                           \
           ifn.vkGetDeviceProcAddr(device->device_, #core_name))) != nullptr;
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc"
#include "xenia/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
#include "xenia/ui/vulkan/functions/device_1_2_ext_host_query_reset.inc"
#include "xenia/ui/vulkan/functions/device_1_2_khr_timeline_semaphore.inc"
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
#include "xenia/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc"
#include "xenia/ui/vulkan/functions/device_1_3_khr_maintenance4.inc"
    // VK_EXT_extended_dynamic_state (#268) + VK_EXT_extended_dynamic_state2
    // (#378) setters and the core topology/restart setters, all promoted to
    // 1.3 core and loaded by their core name.
    if (with_gpu_emulation) {
#include "xenia/ui/vulkan/functions/device_ext_extended_dynamic_state.inc"
#include "xenia/ui/vulkan/functions/device_1_3_core_dynamic_topology.inc"
    }
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0)) {
#include "xenia/ui/vulkan/functions/device_1_4_khr_dynamic_rendering_local_read.inc"
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

  // Non-promoted extensions, and extensions promoted to a Vulkan version not
  // supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &=                                             \
      (dfn.core_name = PFN_##core_name(ifn.vkGetDeviceProcAddr(   \
           device->device_, #extension_name))) != nullptr;
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (device->extensions_.ext_1_1_KHR_get_memory_requirements2) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
    }
    if (device->extensions_.ext_1_1_KHR_bind_memory2) {
#include "xenia/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (device->extensions_.ext_1_2_EXT_host_query_reset) {
#include "xenia/ui/vulkan/functions/device_1_2_ext_host_query_reset.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (device->extensions_.ext_1_3_KHR_maintenance4) {
#include "xenia/ui/vulkan/functions/device_1_3_khr_maintenance4.inc"
    }
    if (device->extensions_.ext_1_3_KHR_dynamic_rendering) {
#include "xenia/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 4, 0)) {
    if (device->extensions_.ext_1_4_KHR_dynamic_rendering_local_read) {
#include "xenia/ui/vulkan/functions/device_1_4_khr_dynamic_rendering_local_read.inc"
    }
  }
  if (device->extensions_.ext_KHR_swapchain) {
#include "xenia/ui/vulkan/functions/device_khr_swapchain.inc"
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

#undef XE_UI_VULKAN_FUNCTION

  if (cvars::log_vk_memory && functions_loaded) {
    vk_memory_real_allocate = dfn.vkAllocateMemory;
    vk_memory_real_free = dfn.vkFreeMemory;
    dfn.vkAllocateMemory = VkMemoryAccountingAllocate;
    dfn.vkFreeMemory = VkMemoryAccountingFree;
    XELOGI("VkMemory: accounting enabled");
  }

  if (device->extensions_.ext_KHR_fragment_shading_rate) {
    dfn.vkCmdSetFragmentShadingRateKHR =
        PFN_vkCmdSetFragmentShadingRateKHR(ifn.vkGetDeviceProcAddr(
            device->device_, "vkCmdSetFragmentShadingRateKHR"));
    if (!dfn.vkCmdSetFragmentShadingRateKHR) {
      device->extensions_.ext_KHR_fragment_shading_rate = false;
    }
  }
  // VK_EXT_extended_dynamic_state3 (#456) is optional and never promoted - load
  // its EXT-named setters non-fatally (like vkGetDeviceFaultInfoEXT). If any are
  // missing, disable the extension so the GPU backend bakes those fields into
  // the pipeline key instead of trying to call null function pointers.
  if (device->extensions_.ext_EXT_extended_dynamic_state3) {
    bool eds3_functions_loaded = true;
#define XE_UI_VULKAN_FUNCTION(name)                                   \
  eds3_functions_loaded &= (dfn.name = PFN_##name(ifn.vkGetDeviceProcAddr( \
                               device->device_, #name))) != nullptr;
#include "xenia/ui/vulkan/functions/device_ext_extended_dynamic_state3.inc"
#undef XE_UI_VULKAN_FUNCTION
    if (!eds3_functions_loaded) {
      device->extensions_.ext_EXT_extended_dynamic_state3 = false;
      device->properties_.extendedDynamicState3DepthClampEnable = false;
      device->properties_.extendedDynamicState3PolygonMode = false;
      device->properties_.extendedDynamicState3ColorBlendEnable = false;
      device->properties_.extendedDynamicState3ColorBlendEquation = false;
      device->properties_.extendedDynamicState3ColorWriteMask = false;
      device->properties_
          .extendedDynamicState3PrimitiveTopologyUnrestricted = false;
    }
  }

  if (device->extensions_.ext_EXT_image_compression_control) {
    device->vkGetImageSubresourceLayout2EXT_ =
        PFN_vkGetImageSubresourceLayout2EXT(ifn.vkGetDeviceProcAddr(
            device->device_, "vkGetImageSubresourceLayout2EXT"));
  }
  if (cvars::log_image_compression) {
    XELOGI("ImgCompression: extension={} query={}",
           device->extensions_.ext_EXT_image_compression_control,
           device->vkGetImageSubresourceLayout2EXT_ != nullptr);
  }

  // Optional fault-info function pointer - failing to load is not fatal.
  if (device->extensions_.ext_EXT_device_fault) {
    device->vkGetDeviceFaultInfoEXT_ = PFN_vkGetDeviceFaultInfoEXT(
        ifn.vkGetDeviceProcAddr(device->device_, "vkGetDeviceFaultInfoEXT"));
    if (!device->vkGetDeviceFaultInfoEXT_) {
      device->extensions_.ext_EXT_device_fault = false;
    }
  }

  // Optional host-pointer import function - failing to load disables zero-copy.
  if (device->extensions_.ext_EXT_external_memory_host) {
    device->vkGetMemoryHostPointerPropertiesEXT_ =
        PFN_vkGetMemoryHostPointerPropertiesEXT(ifn.vkGetDeviceProcAddr(
            device->device_, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!device->vkGetMemoryHostPointerPropertiesEXT_) {
      device->extensions_.ext_EXT_external_memory_host = false;
    }
  }

  if (!functions_loaded) {
    XELOGE("Failed to get all Vulkan device function pointers for '{}'",
           properties.deviceName);
    return nullptr;
  }

  // Get the queues.

  for (size_t queue_family_index = 0;
       queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    for (size_t queue_index = 0; queue_index < queue_family.queues.size();
         ++queue_index) {
      VkQueue queue;
      dfn.vkGetDeviceQueue(device->device_, uint32_t(queue_family_index),
                           uint32_t(queue_index), &queue);
      queue_family.queues[queue_index] = std::make_unique<Queue>(queue);
    }
  }

  // Get the memory types.

  VkPhysicalDeviceMemoryProperties memory_properties;
  ifn.vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  for (uint32_t memory_type_index = 0;
       memory_type_index < memory_properties.memoryTypeCount;
       ++memory_type_index) {
    const uint32_t memory_type_bit = uint32_t(1) << memory_type_index;
    const VkMemoryPropertyFlags memory_type_flags =
        memory_properties.memoryTypes[memory_type_index].propertyFlags;
    if (memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      device->memory_types_.device_local |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      device->memory_types_.host_visible |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      device->memory_types_.host_coherent |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
      device->memory_types_.host_cached |= memory_type_bit;
    }
    XELOGI(
        "Vulkan memory type {}: {}{}{}{}{}heap {}", memory_type_index,
        (memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            ? "DEVICE_LOCAL "
            : "",
        (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            ? "HOST_VISIBLE "
            : "",
        (memory_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            ? "HOST_COHERENT "
            : "",
        (memory_type_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            ? "HOST_CACHED "
            : "",
        (memory_type_flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            ? "LAZILY_ALLOCATED "
            : "",
        memory_properties.memoryTypes[memory_type_index].heapIndex);

    // Detect ReBAR/SAM memory (both device-local and host-visible)
    if ((memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      uint32_t heap_index =
          memory_properties.memoryTypes[memory_type_index].heapIndex;
      VkDeviceSize heap_size = memory_properties.memoryHeaps[heap_index].size;
      // Require at least 256MB to consider this usable ReBAR memory.
      // Smaller heaps aren't worth using for staging buffers.
      constexpr VkDeviceSize kMinRebarHeapSize = 256 * 1024 * 1024;
      if (heap_size >= kMinRebarHeapSize) {
        device->memory_types_.device_local_host_visible |= memory_type_bit;
        XELOGI(
            "Vulkan memory type {}: HOST_VISIBLE | DEVICE_LOCAL (ReBAR/SAM), "
            "heap {} ({} MB)",
            memory_type_index, heap_index, heap_size >> 20);
      } else {
        XELOGI(
            "Vulkan memory type {}: HOST_VISIBLE | DEVICE_LOCAL but heap {} "
            "too small ({} MB < 256 MB), not using as ReBAR",
            memory_type_index, heap_index, heap_size >> 20);
      }
    }
  }

  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vulkan_instance_->functions().vkDestroyDevice(device_, nullptr);
  }
}

VulkanDevice::VulkanDevice(const VulkanInstance* const vulkan_instance,
                           const VkPhysicalDevice physical_device)
    : vulkan_instance_(vulkan_instance), physical_device_(physical_device) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);
}

void VulkanDevice::LogFaultInfo() {
  if (!extensions_.ext_EXT_device_fault || !vkGetDeviceFaultInfoEXT_) {
    return;
  }
  // Log only once, even if many observers call us.
  if (fault_info_logged_.test_and_set(std::memory_order_acq_rel)) {
    return;
  }

  // First pass: counts only.
  VkDeviceFaultCountsEXT counts = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  if (vkGetDeviceFaultInfoEXT_(device_, &counts, nullptr) != VK_SUCCESS) {
    XELOGE("VK_EXT_device_fault: failed to query fault info counts");
    return;
  }

  std::vector<VkDeviceFaultAddressInfoEXT> address_infos(
      counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT> vendor_infos(counts.vendorInfoCount);
  VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos = address_infos.empty() ? nullptr : address_infos.data();
  info.pVendorInfos = vendor_infos.empty() ? nullptr : vendor_infos.data();
  // Skip vendor binary - we don't have a place to dump it anyway.
  counts.vendorBinarySize = 0;
  if (vkGetDeviceFaultInfoEXT_(device_, &counts, &info) != VK_SUCCESS) {
    XELOGE("VK_EXT_device_fault: failed to query fault info");
    return;
  }

  XELOGE("VK_EXT_device_fault: \"{}\" - {} address(es), {} vendor code(s)",
         info.description, counts.addressInfoCount, counts.vendorInfoCount);
  for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
    const VkDeviceFaultAddressInfoEXT& a = address_infos[i];
    const char* type_name = "unknown";
    switch (a.addressType) {
      case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        type_name = "none";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        type_name = "read-invalid";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        type_name = "write-invalid";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        type_name = "execute-invalid";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        type_name = "instruction-pointer-unknown";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        type_name = "instruction-pointer-invalid";
        break;
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        type_name = "instruction-pointer-fault";
        break;
      default:
        break;
    }
    XELOGE("  fault addr: 0x{:016X} (precision ±0x{:X}) type={}",
           a.reportedAddress, a.addressPrecision, type_name);
  }
  for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
    const VkDeviceFaultVendorInfoEXT& v = vendor_infos[i];
    XELOGE("  vendor code: {} fault=0x{:016X} \"{}\"", v.vendorFaultCode,
           v.vendorFaultData, v.description);
  }
}

void VulkanDevice::LogImageCompression(uint32_t kind, VkImage image,
                                       const VkImageCreateInfo& info,
                                       VkImageAspectFlags aspect) const {
  if (!vkGetImageSubresourceLayout2EXT_) {
    return;
  }
  VkImageCompressionPropertiesEXT compression = {};
  compression.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT;
  VkSubresourceLayout2EXT layout = {};
  layout.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_EXT;
  layout.pNext = &compression;
  VkImageSubresource2EXT subresource = {};
  subresource.sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_EXT;
  subresource.imageSubresource.aspectMask = aspect;
  vkGetImageSubresourceLayout2EXT_(device_, image, &subresource, &layout);
  static std::mutex mutex;
  static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t>,
                  uint64_t>
      counts;
  std::lock_guard<std::mutex> lock(mutex);
  uint64_t& count = counts[std::make_tuple(
      kind, uint32_t(info.format), uint32_t(info.flags), uint32_t(info.usage),
      uint32_t(info.samples), uint32_t(compression.imageCompressionFlags))];
  ++count;
  // First of each combination, then at powers of two.
  if (count & (count - 1)) {
    return;
  }
  static const char* const kKinds[] = {"RT color", "RT depth", "texture"};
  XELOGI(
      "ImgCompression: {} format={} compression=0x{:X} ({}) fixed_rate=0x{:X} "
      "create_flags=0x{:X} usage=0x{:X} samples={} count={}",
      kKinds[kind < 3 ? kind : 2], uint32_t(info.format),
      uint32_t(compression.imageCompressionFlags),
      compression.imageCompressionFlags == VK_IMAGE_COMPRESSION_DISABLED_EXT
          ? "NOT compressed"
          : "driver default",
      uint32_t(compression.imageCompressionFixedRateFlags),
      uint32_t(info.flags), uint32_t(info.usage), uint32_t(info.samples),
      count);
}

namespace {
struct HostAllocationCounter {
  std::atomic<int64_t> bytes{0};
  std::atomic<int64_t> allocations{0};
  std::atomic<int64_t> internal_bytes{0};
  VkAllocationCallbacks callbacks;
};
HostAllocationCounter host_allocation_counters[size_t(
    VulkanDevice::HostAllocationCategory::kCount)];

void* VKAPI_PTR HostAllocationCount(void* user_data, size_t size,
                                    size_t alignment,
                                    VkSystemAllocationScope) {
  void* p = nullptr;
  if (posix_memalign(&p, std::max(alignment, sizeof(void*)), size)) {
    return nullptr;
  }
  auto& counter = *static_cast<HostAllocationCounter*>(user_data);
  counter.bytes.fetch_add(int64_t(malloc_usable_size(p)),
                          std::memory_order_relaxed);
  counter.allocations.fetch_add(1, std::memory_order_relaxed);
  return p;
}

void VKAPI_PTR HostAllocationFree(void* user_data, void* memory) {
  if (!memory) {
    return;
  }
  auto& counter = *static_cast<HostAllocationCounter*>(user_data);
  counter.bytes.fetch_sub(int64_t(malloc_usable_size(memory)),
                          std::memory_order_relaxed);
  counter.allocations.fetch_sub(1, std::memory_order_relaxed);
  free(memory);
}

void* VKAPI_PTR HostAllocationReallocate(void* user_data, void* original,
                                         size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
  if (!original) {
    return HostAllocationCount(user_data, size, alignment, scope);
  }
  if (!size) {
    HostAllocationFree(user_data, original);
    return nullptr;
  }
  void* p = HostAllocationCount(user_data, size, alignment, scope);
  if (!p) {
    return nullptr;
  }
  std::memcpy(p, original, std::min(malloc_usable_size(original), size));
  HostAllocationFree(user_data, original);
  return p;
}

void VKAPI_PTR HostInternalAllocation(void* user_data, size_t size,
                                      VkInternalAllocationType,
                                      VkSystemAllocationScope) {
  static_cast<HostAllocationCounter*>(user_data)->internal_bytes.fetch_add(
      int64_t(size), std::memory_order_relaxed);
}

void VKAPI_PTR HostInternalFree(void* user_data, size_t size,
                                VkInternalAllocationType,
                                VkSystemAllocationScope) {
  static_cast<HostAllocationCounter*>(user_data)->internal_bytes.fetch_sub(
      int64_t(size), std::memory_order_relaxed);
}
}  // namespace

const VkAllocationCallbacks* VulkanDevice::host_allocation_callbacks(
    HostAllocationCategory category) {
  // Fixed at the first call: objects must be destroyed with the callbacks
  // they were created with.
  static const bool counting = cvars::log_vk_memory;
  if (!counting || category >= HostAllocationCategory::kCount) {
    return nullptr;
  }
  HostAllocationCounter& counter = host_allocation_counters[size_t(category)];
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    for (HostAllocationCounter& c : host_allocation_counters) {
      c.callbacks.pUserData = &c;
      c.callbacks.pfnAllocation = HostAllocationCount;
      c.callbacks.pfnReallocation = HostAllocationReallocate;
      c.callbacks.pfnFree = HostAllocationFree;
      c.callbacks.pfnInternalAllocation = HostInternalAllocation;
      c.callbacks.pfnInternalFree = HostInternalFree;
    }
  });
  return &counter.callbacks;
}

void VulkanDevice::LogMemoryAccounting() {
  if (!cvars::log_vk_memory) {
    return;
  }
  std::string host_text;
  {
    static const char* const kCategoryNames[] = {"pipelines", "pipeline cache",
                                                  "shader modules"};
    for (size_t i = 0; i < size_t(HostAllocationCategory::kCount); ++i) {
      const HostAllocationCounter& c = host_allocation_counters[i];
      host_text += fmt::format(
          " {} {:.1f}MB in {} blocks (+{:.1f}MB internal);", kCategoryNames[i],
          double(c.bytes.load(std::memory_order_relaxed)) / 1048576.0,
          c.allocations.load(std::memory_order_relaxed),
          double(c.internal_bytes.load(std::memory_order_relaxed)) /
              1048576.0);
    }
  }
  XELOGI("VkHostMemory: driver CPU memory by object kind:{}", host_text);
  std::vector<std::pair<std::pair<uintptr_t, uintptr_t>, VkMemorySite>> sites;
  uint64_t total;
  {
    std::lock_guard<std::mutex> lock(vk_memory_mutex);
    sites.assign(vk_memory_sites.begin(), vk_memory_sites.end());
    total = vk_memory_total;
  }
  std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) {
    return a.second.bytes > b.second.bytes;
  });
  struct mallinfo heap = mallinfo();
  std::string text;
  for (size_t i = 0; i < std::min(sites.size(), size_t(16)); ++i) {
    if (!sites[i].second.bytes) {
      break;
    }
    text += fmt::format(" 0x{:X}/0x{:X} n={} {:.1f}MB;",
                        VkMemoryModuleOffset(sites[i].first.first),
                        VkMemoryModuleOffset(sites[i].first.second),
                        sites[i].second.count,
                        double(sites[i].second.bytes) / 1048576.0);
  }
  XELOGI("VkMemory: total {:.1f} MB, native heap allocated {:.1f} MB | by "
         "site (caller/caller's caller offsets):{}",
         double(total) / 1048576.0, double(heap.uordblks) / 1048576.0, text);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe
