/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include "xenia/gpu/vulkan/vulkan_primitive_processor.h"

#include <cstdint>
#include <cstring>

#include "xenia/base/clock.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DEFINE_bool(
    vulkan_avoid_geometry_shaders, true,
    "Route guest primitives that would otherwise need a geometry shader through "
    "cheaper host paths on tile-based GPUs (Adreno/Turnip), where the "
    "geometry-shader stage serializes the binning pass. Applies to point "
    "sprites (expanded in the vertex shader as a triangle strip) and quad lists "
    "(expanded on the CPU to a triangle list). Rectangle lists always stay on "
    "the geometry shader. Read once at GPU "
    "initialization, so a change takes effect on emulator restart. Disable for "
    "byte-identical pre-optimization (geometry-shader) behavior and A/B "
    "debugging.",
    "Vulkan");

DEFINE_bool(
    vulkan_rectangle_lists_vs_expansion, false,
    "Expand guest rectangle lists (mostly screen-space clears) into triangle "
    "strips in the vertex shader (kRectangleListAsTriangleStrip, the path used "
    "on hosts without geometry shaders) instead of the rectangle-list geometry "
    "shader, even when the device supports geometry shaders. For drivers whose "
    "geometry shader path doesn't render these draws correctly (Mali). Read "
    "once at GPU initialization.",
    "Vulkan");

DEFINE_bool(
    preswap_volatile_validate, true,
    "Preswapped 16-bit index buffers found stale twice in a row are validated "
    "by comparing their guest bytes with a kept copy instead of being watched "
    "with mprotect (each re-watch takes the global lock and a protect call, "
    "then the guest write faults). Same data as a copy at record time. Every "
    "64 uses the range is watched again in case it became static.",
    "GPU");
UPDATE_from_bool(preswap_volatile_validate, 2026, 10, 25, 3, false);

namespace xe {
namespace gpu {
namespace vulkan {

VulkanPrimitiveProcessor::~VulkanPrimitiveProcessor() { Shutdown(true); }

bool VulkanPrimitiveProcessor::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();
  // Tier 3 #7: avoid the geometry-shader stage for guest point sprites and quad
  // lists on tile-based GPUs (Adreno/Turnip), where the GS serializes the
  // binning pass. When vulkan_avoid_geometry_shaders is set:
  //   - point sprites take the kPointListAsTriangleStrip VS expansion (pass
  //     false for point_sprites_supported_without_vs_expansion);
  //   - quad lists take CPU index expansion to a triangle LIST (pass false for
  //     quad_lists_supported -> convert_quad_lists_to_triangle_lists_).
  // The CPU quad path splits the q0-q2 diagonal as a triangle list, while the
  // GS splits q1-q3 as a 0,1,3,2 strip; both carry an unresolved "find correct
  // order" TODO (neither is verified against real Xbox 360 hardware). For
  // planar convex quads (UI/sprites, the common case) the two are pixel- and
  // winding-identical (all triangles stay CCW), so culling is unchanged; they
  // diverge only on non-planar/concave quads. The kill-switch restores the GS
  // path if a title regresses.
  // ARM/Mali rectangle GS draws can leave fullscreen clears and post-effects
  // incomplete. Use the existing VS expansion automatically on this vendor;
  // other devices may opt into the same path with the cvar. This is a device
  // compatibility rule, independent of the guest title or its saved config.
  const bool expand_rectangle_lists_in_vs =
      cvars::vulkan_rectangle_lists_vs_expansion ||
      device_properties.vendorID == 0x13B5;
  if (!InitializeCommon(
          // full_32bit_vertex_indices_supported, triangle_fans_supported,
          device_properties.fullDrawIndexUint32, device_properties.triangleFans,
          // line_loops_supported, quad_lists_supported (gated -> CPU tri-list):
          false,
          device_properties.geometryShader &&
              !cvars::vulkan_avoid_geometry_shaders,
          // point_sprites_supported_without_vs_expansion (gated -> VS path):
          device_properties.geometryShader &&
              !cvars::vulkan_avoid_geometry_shaders,
          // rectangle_lists_supported_without_vs_expansion (GS unless the
          // VS expansion is forced):
          device_properties.geometryShader &&
              !expand_rectangle_lists_in_vs)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ =
      std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
          vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          std::max(size_t(kMinRequiredConvertedIndexBufferSize),
                   ui::GraphicsUploadBufferPool::kDefaultPageSize));
  preswap_index_buffer_pool_ =
      std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
          vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          sizeof(uint16_t) * UINT16_MAX + XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE);
  return true;
}

void VulkanPrimitiveProcessor::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  frame_index_buffers_.clear();
  ReleasePreswapEntries();
  preswap_index_buffer_pool_.reset();
  frame_index_buffer_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         builtin_index_buffer_upload_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         builtin_index_buffer_upload_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         builtin_index_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         builtin_index_buffer_memory_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      command_processor_.GetCompletedSubmission() >=
          builtin_index_buffer_upload_submission_) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
  }
}

void VulkanPrimitiveProcessor::BeginSubmission() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      builtin_index_buffer_upload_submission_ == UINT64_MAX) {
    // No need to submit deferred barriers - builtin_index_buffer_ has never
    // been used yet, and builtin_index_buffer_upload_ is written before
    // submitting commands reading it.

    command_processor_.EndRenderPass();

    DeferredCommandBuffer& command_buffer =
        command_processor_.deferred_command_buffer();

    command_processor_.InsertDebugMarker(
        "Builtin Index Buffer Upload: %zu bytes", builtin_index_buffer_size_);

    VkBufferCopy* copy_region = command_buffer.CmdCopyBufferEmplace(
        builtin_index_buffer_upload_, builtin_index_buffer_, 1);
    copy_region->srcOffset = 0;
    copy_region->dstOffset = 0;
    copy_region->size = builtin_index_buffer_size_;

    command_processor_.PushBufferMemoryBarrier(
        builtin_index_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_INDEX_READ_BIT);

    builtin_index_buffer_upload_submission_ =
        command_processor_.GetCurrentSubmission();
  }
}

void VulkanPrimitiveProcessor::BeginFrame() {
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
  preswap_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
}

void VulkanPrimitiveProcessor::EndSubmission() {
  frame_index_buffer_pool_->FlushWrites();
  preswap_index_buffer_pool_->FlushWrites();
}

void VulkanPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

bool VulkanPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(builtin_index_buffer_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_memory_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_memory_ == VK_NULL_HANDLE);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  builtin_index_buffer_size_ = VkDeviceSize(size_bytes);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    XELOGE(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer GPU resource with {} bytes",
        size_bytes);
    return false;
  }
  uint32_t upload_memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload,
          builtin_index_buffer_upload_, builtin_index_buffer_upload_memory_,
          &upload_memory_type)) {
    XELOGE(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_memory_);
    return false;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0,
                      VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    XELOGE(
        "Vulkan primitive processor: Failed to map the built-in index buffer "
        "upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_memory_);
    return false;
  }
  fill_callback(mapping);
  ui::vulkan::util::FlushMappedMemoryRange(
      vulkan_device, builtin_index_buffer_memory_, upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);

  // Schedule uploading in the first submission.
  builtin_index_buffer_upload_submission_ = UINT64_MAX;
  return true;
}

void VulkanPrimitiveProcessor::ReleasePreswapEntries() {
  for (auto& entry : preswap_entries_) {
    if (preswap_index_buffer_pool_)
      preswap_index_buffer_pool_->ReleaseRetained(entry.allocation);
    entry = {};
  }
}

void VulkanPrimitiveProcessor::ClearCache() {
  ReleasePreswapEntries();
  preswap_index_buffer_pool_->ClearCache();
  frame_index_buffer_pool_->ClearCache();
}

bool VulkanPrimitiveProcessor::GetPreswappedIndexBuffer(uint32_t guest_base,
                                                        uint32_t count,
                                                        size_t& handle_out) {
  const uint32_t length = count * sizeof(uint16_t);
  const uint32_t set_first = (((guest_base >> 1) * UINT32_C(0x9E3779B1) ^
                               count * UINT32_C(0x85EBCA6B)) >>
                              kPreswapSetShift) *
                             kPreswapWays;
  uint32_t slot = set_first;
  for (uint32_t way = 0; way < kPreswapWays; ++way) {
    const PreswapEntry& candidate = preswap_entries_[set_first + way];
    if (candidate.base == guest_base && candidate.count == count) {
      slot = set_first + way;
      break;
    }
    if (candidate.last_use < preswap_entries_[slot].last_use) {
      slot = set_first + way;
    }
  }
  auto& entry = preswap_entries_[slot];
  entry.last_use = ++preswap_use_counter_;
  const bool same_range = entry.base == guest_base && entry.count == count &&
                          entry.allocation.buffer != VK_NULL_HANDLE;
  if (!same_range) {
    entry.stale_streak = 0;
    entry.validate_by_compare = false;
  }
  const uint8_t* guest_indices = memory().TranslatePhysical(guest_base);
  bool compare_mode = same_range && entry.validate_by_compare &&
                      entry.guest_copy.size() == length;
  if (compare_mode && ++entry.compare_uses >= 64) {
    // Watch it again now and then: a range that became static is cached
    // through the epoch, which costs nothing per use.
    entry.validate_by_compare = false;
    entry.stale_streak = 0;
    compare_mode = false;
  }
  const uint64_t epoch =
      compare_mode ? UINT64_MAX
                   : shared_memory().CPUReadRangeEpoch(guest_base, length);
  const bool hit =
      compare_mode
          ? !std::memcmp(entry.guest_copy.data(), guest_indices, length)
          : same_range && entry.epoch == epoch && epoch != UINT64_MAX;
  if (hit && !compare_mode) entry.stale_streak = 0;
  if (compare_mode && hit && preswap_stats_) ++stat_preswap_compare_hits_;
  if (!hit) {
    if (same_range && entry.stale_streak < UINT8_MAX) ++entry.stale_streak;
    if (cvars::preswap_volatile_validate && !compare_mode &&
        entry.stale_streak >= 2) {
      entry.validate_by_compare = true;
      entry.compare_uses = 0;
      compare_mode = true;
    }
    // Release only the cache's pin. The page remembers its last GPU use and
    // cannot be recycled until that frame completed, including eviction here.
    preswap_index_buffer_pool_->ReleaseRetained(entry.allocation);
    const uint64_t watched_epoch =
        compare_mode ? UINT64_MAX
                     : shared_memory().WatchCPUReadRange(guest_base, length);
    uint8_t* destination = preswap_index_buffer_pool_->Request(
        command_processor_.GetCurrentFrame(),
        length + XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE, sizeof(uint16_t),
        entry.allocation.buffer, entry.allocation.offset, &entry.allocation);
    if (!destination) {
      entry = {};
      return false;
    }
    entry.last_use = preswap_use_counter_;
    ptrdiff_t alignment = GetSimdCoalignmentOffset(destination, guest_base);
    entry.allocation.offset += alignment;
    const uint64_t copy_start =
        preswap_stats_ ? Clock::QueryRawNanoseconds() : 0;
    xe::copy_and_swap_16_unaligned(destination + alignment, guest_indices,
                                   count);
    if (compare_mode) {
      entry.guest_copy.assign(guest_indices, guest_indices + length);
    } else {
      entry.guest_copy.clear();
    }
    if (preswap_stats_) {
      ++stat_preswap_misses_;
      stat_preswap_stale_ += entry.base == guest_base && entry.count == count;
      stat_preswap_compare_copies_ += compare_mode;
      stat_preswap_indices_ += count;
      stat_preswap_ns_ += Clock::QueryRawNanoseconds() - copy_start;
      --stat_preswap_hits_;  // Counted below for every request.
    }
    entry.base = guest_base;
    entry.count = count;
    // Never publish a copy across a racing invalidation.
    entry.epoch =
        watched_epoch == shared_memory().CPUReadRangeEpoch(guest_base, length)
            ? watched_epoch
            : UINT64_MAX;
  }
  if (preswap_stats_) ++stat_preswap_hits_;
  preswap_index_buffer_pool_->UseRetained(entry.allocation,
                                          command_processor_.GetCurrentFrame());
  // Consumed before Process is called again; deferred draws store VkBuffer and
  // offset, never this cache slot. The pinned page covers GPU execution too.
  handle_out = kPreswapHandleBit | slot;
  return true;
}

void* VulkanPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                           : sizeof(uint32_t);
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count +
          (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset =
        GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = VkDeviceSize(offset + coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(buffer, offset);
  return mapping;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
