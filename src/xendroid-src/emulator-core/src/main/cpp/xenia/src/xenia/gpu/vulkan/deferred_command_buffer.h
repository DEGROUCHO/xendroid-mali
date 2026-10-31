/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_H_
#define XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_H_

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <map>
#include <unordered_map>

#include "xenia/gpu/vulkan/pass_byte_ledger.h"
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;
class VulkanRenderTargetCache;

// Built only for sampled submissions, from existing cache objects. Handles are
// copied as values: diagnostics must not acquire leases or touch usage/LRU state.
struct PassByteCatalog {
  template <typename T> static uint64_t Id(T handle) {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    uint64_t id = 0; std::memcpy(&id, &handle, sizeof(T)); return id;
  }
  static uint32_t FormatBytes(VkFormat f) {
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) return 1;
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) return 4;
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) return 2;
    if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) return 4;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) return 4;
    if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) return 16;
    switch (f) {
      case VK_FORMAT_R4G4_UNORM_PACK8: return 1;
      case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
      case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
      case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_D16_UNORM: return 2;
      case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 4;
      case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
      case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: return 16;
      default: return 0;
    }
  }
  struct Image {
    uint64_t image = 0;
    uint32_t format = 0, bpp = 0, samples = 1, width = 0, height = 0;
    uint32_t block_w = 1, block_h = 1;
    uint32_t mip_levels = 1, layers = 1;
  };
  struct Ops {
    uint32_t load = 0, store = 0, stencil_load = 0, stencil_store = 0, slot = 0;
  };
  std::unordered_map<uint64_t, Image> images, views;
  std::unordered_map<uint64_t, std::vector<uint64_t>> framebuffers;
  std::unordered_map<uint64_t, std::vector<Ops>> render_passes;
};

// A lazy clear (vulkan_lazy_constant_rows) after clipping to the render area.
struct DeferredLazyClearResult {
  uint32_t slot;
  VkRect2D original;
  // Zero extent if the clear was dropped (nothing of it in the area).
  VkRect2D applied;
};

class DeferredCommandBuffer {
 public:
  DeferredCommandBuffer(const VulkanCommandProcessor& command_processor,
                        size_t initial_size_bytes = 1024 * 1024);

  void Reset();
  void SetPassBytesEnabled(bool enabled) { pass_bytes_enabled_ = enabled; }
  bool pass_bytes_enabled() const { return pass_bytes_enabled_; }
  void AnnotatePassBytes(PassByteLedger::Event event) {
    if (pass_bytes_enabled_) pass_bytes_notes_[command_stream_size_].push_back(event);
  }
  void CollectPassBytes(const PassByteCatalog& catalog, PassByteLedger& ledger) const;
  // Append a self-contained, out-of-pass stream. It must not contain slot
  // indices, whose namespace belongs to the recording command buffer.
  void Append(const DeferredCommandBuffer& commands);
  void Execute(VkCommandBuffer command_buffer);
  // Replays the commands in [begin, end) (stream elements, both on command
  // boundaries). guest_graphics_pipeline_unready carries the dropped-draws
  // state from the previous range of the same stream.
  void ExecuteRange(VkCommandBuffer command_buffer, size_t begin, size_t end,
                    bool& guest_graphics_pipeline_unready);
  bool empty() const { return command_stream_size_ == 0; }
  // Guest draws dropped at replay because their pipeline was still being
  // created asynchronously (vulkan_async_skip_draws) - returns and resets.
  uint64_t TakeDroppedDraws() {
    return dropped_draws_.exchange(0, std::memory_order_relaxed);
  }
  // Recorded length in stream elements - a command boundary.
  size_t size_elements() const { return command_stream_size_; }
  // Called before the stream storage (or the pass-head slot table) is
  // reallocated, so a thread replaying already recorded commands concurrently
  // can be drained first.
  void SetBeforeReallocate(std::function<void()> before_reallocate) {
    before_reallocate_ = std::move(before_reallocate);
  }

  // Pass-head slots: a placeholder recorded right before a render pass begins,
  // filled later with transfer commands that must precede the pass but were
  // requested while it was already open (so the pass doesn't have to be ended
  // for them). Executed wrapped in barriers ordering them after the earlier
  // commands of src_stage_mask and before all later ones.
  uint32_t CmdPassHeadSlot(
      VkPipelineStageFlags src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  DeferredCommandBuffer& GetPassHeadSlot(uint32_t slot) {
    return *pass_head_slots_[slot];
  }

  // render_pass_begin->pNext of all barriers must be null.
  void CmdVkBeginRenderPass(const VkRenderPassBeginInfo* render_pass_begin,
                            VkSubpassContents contents,
                            bool reserve_clear_values = false) {
    assert_null(render_pass_begin->pNext);
    size_t arguments_size = sizeof(ArgsVkBeginRenderPass);
    uint32_t clear_value_count = render_pass_begin->clearValueCount;
    const uint32_t clear_value_capacity = reserve_clear_values
                                              ? std::max(clear_value_count, 5u)
                                              : clear_value_count;
    size_t clear_values_offset = 0;
    if (clear_value_capacity) {
      arguments_size = xe::align(arguments_size, alignof(VkClearValue));
      clear_values_offset = arguments_size;
      arguments_size += sizeof(VkClearValue) * clear_value_capacity;
    }
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBeginRenderPass, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBeginRenderPass*>(args_ptr);
    args.render_pass = render_pass_begin->renderPass;
    args.framebuffer = render_pass_begin->framebuffer;
    args.render_area = render_pass_begin->renderArea;
    args.clear_value_count = clear_value_count;
    args.contents = contents;
    args.classic_ops = 0;
    args.changed_ops = 0;
    args.folded_clears = 0;
    args.discarded_stores = 0;
    args.clear_value_capacity = clear_value_capacity;
    std::memset(args.attachment_images, 0, sizeof(args.attachment_images));
    std::memset(args.attachment_extents, 0, sizeof(args.attachment_extents));
    BeginRenderAreaTracking(arguments_size, /*dynamic_rendering=*/false);
    if (clear_value_capacity) {
      std::memset(args_ptr + clear_values_offset, 0,
                  sizeof(VkClearValue) * clear_value_capacity);
    }
    if (clear_value_count) {
      std::memcpy(args_ptr + clear_values_offset,
                  render_pass_begin->pClearValues,
                  sizeof(VkClearValue) * clear_value_count);
    }
  }

  void SetClassicRenderPassAttachments(const VkImage* images,
                                       const VkExtent2D* extents) {
    assert_false(render_area_patch_dynamic_);
    auto& args = *reinterpret_cast<ArgsVkBeginRenderPass*>(
        command_stream_.data() + render_area_patch_offset_);
    std::memcpy(args.attachment_images, images, sizeof(args.attachment_images));
    std::memcpy(args.attachment_extents, extents,
                sizeof(args.attachment_extents));
  }

  void CmdVkBindDescriptorSets(VkPipelineBindPoint pipeline_bind_point,
                               VkPipelineLayout layout, uint32_t first_set,
                               uint32_t descriptor_set_count,
                               const VkDescriptorSet* descriptor_sets,
                               uint32_t dynamic_offset_count,
                               const uint32_t* dynamic_offsets) {
    size_t arguments_size =
        xe::align(sizeof(ArgsVkBindDescriptorSets), alignof(VkDescriptorSet));
    size_t descriptor_sets_offset = arguments_size;
    arguments_size += sizeof(VkDescriptorSet) * descriptor_set_count;
    size_t dynamic_offsets_offset = 0;
    if (dynamic_offset_count) {
      arguments_size = xe::align(arguments_size, alignof(uint32_t));
      dynamic_offsets_offset = arguments_size;
      arguments_size += sizeof(uint32_t) * dynamic_offset_count;
    }
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBindDescriptorSets, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindDescriptorSets*>(args_ptr);
    args.pipeline_bind_point = pipeline_bind_point;
    args.layout = layout;
    args.first_set = first_set;
    args.descriptor_set_count = descriptor_set_count;
    args.dynamic_offset_count = dynamic_offset_count;
    std::memcpy(args_ptr + descriptor_sets_offset, descriptor_sets,
                sizeof(VkDescriptorSet) * descriptor_set_count);
    if (dynamic_offset_count) {
      std::memcpy(args_ptr + dynamic_offsets_offset, dynamic_offsets,
                  sizeof(uint32_t) * dynamic_offset_count);
    }
  }

  // Not a command buffer command: vkUpdateDescriptorSets replayed at Execute
  // time (on the stream encoder thread when streaming), right before the binds
  // recorded after it, taking the Mali descriptor write cost off the CP thread.
  // Only pImageInfo / pBufferInfo writes are supported.
  void CmdUpdateDescriptorSets(uint32_t write_count,
                               const VkWriteDescriptorSet* writes) {
    size_t arguments_size = sizeof(ArgsUpdateDescriptorSets) +
                            sizeof(PackedDescriptorWrite) * write_count;
    for (uint32_t i = 0; i < write_count; ++i) {
      arguments_size += writes[i].pImageInfo
                            ? sizeof(VkDescriptorImageInfo) *
                                  writes[i].descriptorCount
                            : sizeof(VkDescriptorBufferInfo) *
                                  writes[i].descriptorCount;
    }
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kUpdateDescriptorSets, arguments_size));
    reinterpret_cast<ArgsUpdateDescriptorSets*>(args_ptr)->write_count =
        write_count;
    auto* packed = reinterpret_cast<PackedDescriptorWrite*>(
        args_ptr + sizeof(ArgsUpdateDescriptorSets));
    uint8_t* info_ptr =
        reinterpret_cast<uint8_t*>(packed + write_count);
    for (uint32_t i = 0; i < write_count; ++i) {
      const VkWriteDescriptorSet& write = writes[i];
      assert_null(write.pTexelBufferView);
      PackedDescriptorWrite& p = packed[i];
      p.dst_set = write.dstSet;
      p.dst_binding = write.dstBinding;
      p.dst_array_element = write.dstArrayElement;
      p.descriptor_count = write.descriptorCount;
      p.descriptor_type = write.descriptorType;
      p.is_image = write.pImageInfo ? 1 : 0;
      size_t info_size = write.pImageInfo
                             ? sizeof(VkDescriptorImageInfo) *
                                   write.descriptorCount
                             : sizeof(VkDescriptorBufferInfo) *
                                   write.descriptorCount;
      std::memcpy(info_ptr,
                  write.pImageInfo
                      ? static_cast<const void*>(write.pImageInfo)
                      : static_cast<const void*>(write.pBufferInfo),
                  info_size);
      info_ptr += info_size;
    }
  }

  void CmdVkBindIndexBuffer(VkBuffer buffer, VkDeviceSize offset,
                            VkIndexType index_type) {
    auto& args = *reinterpret_cast<ArgsVkBindIndexBuffer*>(WriteCommand(
        Command::kVkBindIndexBuffer, sizeof(ArgsVkBindIndexBuffer)));
    args.buffer = buffer;
    args.offset = offset;
    args.index_type = index_type;
  }

  void CmdVkBindPipeline(VkPipelineBindPoint pipeline_bind_point,
                         VkPipeline pipeline) {
    auto& args = *reinterpret_cast<ArgsVkBindPipeline*>(
        WriteCommand(Command::kVkBindPipeline, sizeof(ArgsVkBindPipeline)));
    args.pipeline_bind_point = pipeline_bind_point;
    args.pipeline = pipeline;
  }

  // Deferred-creation variant: records a STABLE pointer to a pipeline cache
  // slot (a pipelines_ map node's Pipeline::pipeline field) instead of a
  // handle value, dereferenced at Execute() time. The slot may still be
  // VK_NULL_HANDLE at replay if asynchronous creation hasn't finished (or
  // failed); in that case the bind is skipped and all graphics draws until the
  // next successful graphics pipeline bind are dropped. Used only for guest
  // graphics pipelines; the external / compute bind paths keep using the
  // by-value CmdVkBindPipeline.
  void CmdVkBindPipelineDeferred(VkPipelineBindPoint pipeline_bind_point,
                                 VkPipeline pipeline_at_record) {
    auto& args = *reinterpret_cast<ArgsVkBindPipelineDeferred*>(WriteCommand(
        Command::kVkBindPipelineDeferred, sizeof(ArgsVkBindPipelineDeferred)));
    args.pipeline_bind_point = pipeline_bind_point;
    args.pipeline = pipeline_at_record;
  }

  void CmdVkBindVertexBuffers(uint32_t first_binding, uint32_t binding_count,
                              const VkBuffer* buffers,
                              const VkDeviceSize* offsets) {
    size_t arguments_size =
        xe::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
    size_t buffers_offset = arguments_size;
    arguments_size =
        xe::align(arguments_size + sizeof(VkBuffer) * binding_count,
                  alignof(VkDeviceSize));
    size_t offsets_offset = arguments_size;
    arguments_size += sizeof(VkDeviceSize) * binding_count;
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBindVertexBuffers, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindVertexBuffers*>(args_ptr);
    args.first_binding = first_binding;
    args.binding_count = binding_count;
    std::memcpy(args_ptr + buffers_offset, buffers,
                sizeof(VkBuffer) * binding_count);
    std::memcpy(args_ptr + offsets_offset, offsets,
                sizeof(VkDeviceSize) * binding_count);
  }

  void CmdVkBeginQuery(VkQueryPool query_pool, uint32_t query,
                       VkQueryControlFlags flags) {
    auto& args = *reinterpret_cast<ArgsVkBeginQuery*>(
        WriteCommand(Command::kVkBeginQuery, sizeof(ArgsVkBeginQuery)));
    args.query_pool = query_pool;
    args.query = query;
    args.flags = flags;
  }

  void CmdVkEndQuery(VkQueryPool query_pool, uint32_t query) {
    auto& args = *reinterpret_cast<ArgsVkEndQuery*>(
        WriteCommand(Command::kVkEndQuery, sizeof(ArgsVkEndQuery)));
    args.query_pool = query_pool;
    args.query = query;
  }

  void CmdVkResetQueryPool(VkQueryPool query_pool, uint32_t first_query,
                           uint32_t query_count) {
    auto& args = *reinterpret_cast<ArgsVkResetQueryPool*>(
        WriteCommand(Command::kVkResetQueryPool, sizeof(ArgsVkResetQueryPool)));
    args.query_pool = query_pool;
    args.first_query = first_query;
    args.query_count = query_count;
  }

  void CmdVkWriteTimestamp(VkPipelineStageFlagBits pipeline_stage,
                           VkQueryPool query_pool, uint32_t query) {
    auto& args = *reinterpret_cast<ArgsVkWriteTimestamp*>(WriteCommand(
        Command::kVkWriteTimestamp, sizeof(ArgsVkWriteTimestamp)));
    args.pipeline_stage = pipeline_stage;
    args.query_pool = query_pool;
    args.query = query;
  }

  void CmdClearAttachmentsEmplace(uint32_t attachment_count,
                                  VkClearAttachment*& attachments_out,
                                  uint32_t rect_count,
                                  VkClearRect*& rects_out) {
    size_t arguments_size =
        xe::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment));
    size_t attachments_offset = arguments_size;
    arguments_size =
        xe::align(arguments_size + sizeof(VkClearAttachment) * attachment_count,
                  alignof(VkClearRect));
    size_t rects_offset = arguments_size;
    arguments_size += sizeof(VkClearRect) * rect_count;
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkClearAttachments, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkClearAttachments*>(args_ptr);
    args.attachment_count = attachment_count;
    args.rect_count = rect_count;
    attachments_out =
        reinterpret_cast<VkClearAttachment*>(args_ptr + attachments_offset);
    rects_out = reinterpret_cast<VkClearRect*>(args_ptr + rects_offset);
    // The caller fills the rectangles afterwards - their extent is unknown
    // here, so the pass keeps its whole render area unless the caller accounts
    // them (CmdVkClearAttachments, AccountEmplacedClearRects).
    unaccounted_before_emplace_ = pass_unaccounted_clear_;
    pass_unaccounted_clear_ = true;
    pass_attachment_use_ = kDrawAttachmentUseAll;
  }
  // vulkan_lazy_constant_rows: a clear of one attachment, at the head of the
  // pass, that doesn't widen the drawn extent. ClipLazyClears trims it to the
  // final render area; the render target cache keeps the rows outside as
  // pending constant rows instead of storing them.
  void CmdVkClearAttachmentsLazy(uint32_t slot,
                                 const VkClearAttachment& attachment,
                                 const VkClearRect& rect) {
    if (lazy_clear_count_ >= kMaxLazyClears ||
        render_area_patch_offset_ == kNoRenderAreaPatch) {
      CmdVkClearAttachments(1, &attachment, 1, &rect);
      return;
    }
    const size_t offset = command_stream_size_;
    const bool unaccounted_clear = pass_unaccounted_clear_;
    VkClearAttachment* attachment_arg;
    VkClearRect* rect_arg;
    CmdClearAttachmentsEmplace(1, attachment_arg, 1, rect_arg);
    *attachment_arg = attachment;
    *rect_arg = rect;
    pass_unaccounted_clear_ = unaccounted_clear;
    lazy_clears_[lazy_clear_count_++] = {offset, slot, rect.rect};
  }
  using LazyClearResult = DeferredLazyClearResult;
  // After ShrinkRenderAreaToDrawn and before folding: clips the pass's lazy
  // clears to its render area, returns how many results were written.
  uint32_t ClipLazyClears(LazyClearResult* results);
  static constexpr uint32_t kMaxLazyClears = 5;

  // The rectangles of the last CmdClearAttachmentsEmplace, once filled: the
  // pass's render area may then shrink around them.
  void AccountEmplacedClearRects(const VkClearRect* rects, uint32_t count) {
    pass_unaccounted_clear_ = unaccounted_before_emplace_;
    for (uint32_t i = 0; i < count; ++i) {
      AccumulateDrawnRect(rects[i].rect);
    }
  }

  void CmdVkClearAttachments(uint32_t attachment_count,
                             const VkClearAttachment* attachments,
                             uint32_t rect_count, const VkClearRect* rects) {
    VkClearAttachment* attachments_arg;
    VkClearRect* rects_arg;
    // These rectangles are accounted below - keep whether an earlier clear of
    // the pass wasn't.
    const bool unaccounted_clear = pass_unaccounted_clear_;
    CmdClearAttachmentsEmplace(attachment_count, attachments_arg, rect_count,
                               rects_arg);
    std::memcpy(attachments_arg, attachments,
                sizeof(VkClearAttachment) * attachment_count);
    std::memcpy(rects_arg, rects, sizeof(VkClearRect) * rect_count);
    pass_unaccounted_clear_ = unaccounted_clear;
    for (uint32_t i = 0; i < rect_count; ++i) {
      AccumulateDrawnRect(rects[i].rect);
    }
  }

  VkImageSubresourceRange* CmdClearColorImageEmplace(
      VkImage image, VkImageLayout image_layout, const VkClearColorValue* color,
      uint32_t range_count) {
    const size_t header_size = xe::align(sizeof(ArgsVkClearColorImage),
                                         alignof(VkImageSubresourceRange));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkClearColorImage,
        header_size + sizeof(VkImageSubresourceRange) * range_count));
    auto& args = *reinterpret_cast<ArgsVkClearColorImage*>(args_ptr);
    args.image = image;
    args.image_layout = image_layout;
    args.color = *color;
    args.range_count = range_count;
    return reinterpret_cast<VkImageSubresourceRange*>(args_ptr + header_size);
  }
  void CmdVkClearColorImage(VkImage image, VkImageLayout image_layout,
                            const VkClearColorValue* color,
                            uint32_t range_count,
                            const VkImageSubresourceRange* ranges) {
    std::memcpy(
        CmdClearColorImageEmplace(image, image_layout, color, range_count),
        ranges, sizeof(VkImageSubresourceRange) * range_count);
  }

  VkBufferCopy* CmdCopyBufferEmplace(VkBuffer src_buffer, VkBuffer dst_buffer,
                                     uint32_t region_count) {
    const size_t header_size =
        xe::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyBuffer,
                     header_size + sizeof(VkBufferCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBuffer*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_buffer = dst_buffer;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBuffer(VkBuffer src_buffer, VkBuffer dst_buffer,
                       uint32_t region_count, const VkBufferCopy* regions) {
    std::memcpy(CmdCopyBufferEmplace(src_buffer, dst_buffer, region_count),
                regions, sizeof(VkBufferCopy) * region_count);
  }

  VkBufferImageCopy* CmdCopyBufferToImageEmplace(VkBuffer src_buffer,
                                                 VkImage dst_image,
                                                 VkImageLayout dst_image_layout,
                                                 uint32_t region_count) {
    const size_t header_size =
        xe::align(sizeof(ArgsVkCopyBufferToImage), alignof(VkBufferImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyBufferToImage,
                     header_size + sizeof(VkBufferImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBufferToImage*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferImageCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBufferToImage(VkBuffer src_buffer, VkImage dst_image,
                              VkImageLayout dst_image_layout,
                              uint32_t region_count,
                              const VkBufferImageCopy* regions) {
    std::memcpy(CmdCopyBufferToImageEmplace(src_buffer, dst_image,
                                            dst_image_layout, region_count),
                regions, sizeof(VkBufferImageCopy) * region_count);
  }

  VkImageCopy* CmdCopyImageEmplace(VkImage src_image,
                                   VkImageLayout src_image_layout,
                                   VkImage dst_image,
                                   VkImageLayout dst_image_layout,
                                   uint32_t region_count) {
    const size_t header_size =
        xe::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyImage,
                     header_size + sizeof(VkImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    return reinterpret_cast<VkImageCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyImage(VkImage src_image, VkImageLayout src_image_layout,
                      VkImage dst_image, VkImageLayout dst_image_layout,
                      uint32_t region_count, const VkImageCopy* regions) {
    std::memcpy(CmdCopyImageEmplace(src_image, src_image_layout, dst_image,
                                    dst_image_layout, region_count),
                regions, sizeof(VkImageCopy) * region_count);
  }

  void CmdVkCopyImageToBuffer(VkImage src_image,
                              VkImageLayout src_image_layout,
                              VkBuffer dst_buffer, uint32_t region_count,
                              const VkBufferImageCopy* regions) {
    const size_t header_size =
        xe::align(sizeof(ArgsVkCopyImageToBuffer), alignof(VkBufferImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyImageToBuffer,
                     header_size + sizeof(VkBufferImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyImageToBuffer*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_buffer = dst_buffer;
    args.region_count = region_count;
    std::memcpy(args_ptr + header_size, regions,
                sizeof(VkBufferImageCopy) * region_count);
  }

  void CmdVkFillBuffer(VkBuffer dst_buffer, VkDeviceSize dst_offset,
                       VkDeviceSize size, uint32_t data) {
    auto& args = *reinterpret_cast<ArgsVkFillBuffer*>(
        WriteCommand(Command::kVkFillBuffer, sizeof(ArgsVkFillBuffer)));
    args.dst_buffer = dst_buffer;
    args.dst_offset = dst_offset;
    args.size = size;
    args.data = data;
  }

  VkImageBlit* CmdBlitImageEmplace(VkImage src_image,
                                   VkImageLayout src_image_layout,
                                   VkImage dst_image,
                                   VkImageLayout dst_image_layout,
                                   uint32_t region_count, VkFilter filter) {
    const size_t header_size =
        xe::align(sizeof(ArgsVkBlitImage), alignof(VkImageBlit));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBlitImage,
                     header_size + sizeof(VkImageBlit) * region_count));
    auto& args = *reinterpret_cast<ArgsVkBlitImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    args.filter = filter;
    return reinterpret_cast<VkImageBlit*>(args_ptr + header_size);
  }
  void CmdVkBlitImage(VkImage src_image, VkImageLayout src_image_layout,
                      VkImage dst_image, VkImageLayout dst_image_layout,
                      uint32_t region_count, const VkImageBlit* regions,
                      VkFilter filter) {
    std::memcpy(CmdBlitImageEmplace(src_image, src_image_layout, dst_image,
                                    dst_image_layout, region_count, filter),
                regions, sizeof(VkImageBlit) * region_count);
  }

  // Who records compute dispatches (GuestFps line with log_guest_fps).
  enum class DispatchSite : uint32_t {
    kOther,
    kSwap,
    kMemExport,
    kCopy,
    kResolveDirect,
    kMaterializeDepth,
    kResolve,
    kResolveClear,
    kHostDepthStore,
    kTextureLoad,
    kDump,
    kCount,
  };
  struct DispatchStats {
    uint64_t dispatches[size_t(DispatchSite::kCount)];
    uint64_t groups[size_t(DispatchSite::kCount)];
  };
  // Shared by every buffer (setup, deferred resolve), all on the CP thread.
  static DispatchStats& dispatch_stats() { return dispatch_stats_; }
  // Bit per DispatchSite recorded since the caller last cleared it (the GPU
  // time breakdown labels the time between two render passes with it).
  static uint32_t& dispatch_site_mask() { return dispatch_site_mask_; }

  void CmdVkDispatch(uint32_t group_count_x, uint32_t group_count_y,
                     uint32_t group_count_z,
                     DispatchSite site = DispatchSite::kOther) {
    ++dispatch_stats_.dispatches[size_t(site)];
    dispatch_site_mask_ |= uint32_t(1) << uint32_t(site);
    dispatch_stats_.groups[size_t(site)] +=
        uint64_t(group_count_x) * group_count_y * group_count_z;
    auto& args = *reinterpret_cast<ArgsVkDispatch*>(
        WriteCommand(Command::kVkDispatch, sizeof(ArgsVkDispatch)));
    args.group_count_x = group_count_x;
    args.group_count_y = group_count_y;
    args.group_count_z = group_count_z;
  }

  void CmdVkDraw(uint32_t vertex_count, uint32_t instance_count,
                 uint32_t first_vertex, uint32_t first_instance) {
    AccumulateDrawnScissor();
    auto& args = *reinterpret_cast<ArgsVkDraw*>(
        WriteCommand(Command::kVkDraw, sizeof(ArgsVkDraw)));
    args.vertex_count = vertex_count;
    args.instance_count = instance_count;
    args.first_vertex = first_vertex;
    args.first_instance = first_instance;
  }

  void CmdVkDrawIndexed(uint32_t index_count, uint32_t instance_count,
                        uint32_t first_index, int32_t vertex_offset,
                        uint32_t first_instance) {
    AccumulateDrawnScissor();
    auto& args = *reinterpret_cast<ArgsVkDrawIndexed*>(
        WriteCommand(Command::kVkDrawIndexed, sizeof(ArgsVkDrawIndexed)));
    args.index_count = index_count;
    args.instance_count = instance_count;
    args.first_index = first_index;
    args.vertex_offset = vertex_offset;
    args.first_instance = first_instance;
  }

  void CmdVkEndRenderPass() { WriteCommand(Command::kVkEndRenderPass, 0); }

  // Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3)
  // Simpler than CmdVkBeginRenderPass - doesn't need
  // VkRenderPass/VkFramebuffer.
  // |msrtss_samples| not 0: the pass rasterizes that many samples into its
  // single-sampled attachments (VK_EXT_multisampled_render_to_single_sampled),
  // resolved as their resolveMode says when it ends.
  void CmdVkBeginRendering(const VkRenderingInfo* rendering_info,
                           uint32_t msrtss_samples = 0);
  void CmdVkEndRendering() { WriteCommand(Command::kVkEndRendering, 0); }

  // pNext of all barriers must be null.
  void CmdVkPipelineBarrier(VkPipelineStageFlags src_stage_mask,
                            VkPipelineStageFlags dst_stage_mask,
                            VkDependencyFlags dependency_flags,
                            uint32_t memory_barrier_count,
                            const VkMemoryBarrier* memory_barriers,
                            uint32_t buffer_memory_barrier_count,
                            const VkBufferMemoryBarrier* buffer_memory_barriers,
                            uint32_t image_memory_barrier_count,
                            const VkImageMemoryBarrier* image_memory_barriers);

  void CmdVkPushConstants(VkPipelineLayout layout,
                          VkShaderStageFlags stage_flags, uint32_t offset,
                          uint32_t size, const void* values) {
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkPushConstants, sizeof(ArgsVkPushConstants) + size));
    auto& args = *reinterpret_cast<ArgsVkPushConstants*>(args_ptr);
    args.layout = layout;
    args.stage_flags = stage_flags;
    args.offset = offset;
    args.size = size;
    std::memcpy(args_ptr + sizeof(ArgsVkPushConstants), values, size);
  }

  void CmdVkSetRenderingInputAttachmentIndices(
      uint32_t color_attachment_count, const uint32_t* color_attachment_input_indices) {
    auto& args = *reinterpret_cast<ArgsVkSetRenderingInputAttachmentIndices*>(
        WriteCommand(Command::kVkSetRenderingInputAttachmentIndices,
                     sizeof(ArgsVkSetRenderingInputAttachmentIndices)));
    args.color_attachment_count = color_attachment_count;
    for (uint32_t i = 0; i < 4; ++i) {
      args.color_attachment_input_indices[i] =
          i < color_attachment_count ? color_attachment_input_indices[i]
                                     : VK_ATTACHMENT_UNUSED;
    }
  }

  void CmdVkSetBlendConstants(const float* blend_constants) {
    auto& args = *reinterpret_cast<ArgsVkSetBlendConstants*>(WriteCommand(
        Command::kVkSetBlendConstants, sizeof(ArgsVkSetBlendConstants)));
    std::memcpy(args.blend_constants, blend_constants, sizeof(float) * 4);
  }

  void CmdVkSetDepthBias(float depth_bias_constant_factor,
                         float depth_bias_clamp,
                         float depth_bias_slope_factor) {
    auto& args = *reinterpret_cast<ArgsVkSetDepthBias*>(
        WriteCommand(Command::kVkSetDepthBias, sizeof(ArgsVkSetDepthBias)));
    args.depth_bias_constant_factor = depth_bias_constant_factor;
    args.depth_bias_clamp = depth_bias_clamp;
    args.depth_bias_slope_factor = depth_bias_slope_factor;
  }

  void CmdVkSetScissor(uint32_t first_scissor, uint32_t scissor_count,
                       const VkRect2D* scissors) {
    constexpr size_t header_size =
        xe::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetScissor,
                     header_size + sizeof(VkRect2D) * scissor_count));
    auto& args = *reinterpret_cast<ArgsVkSetScissor*>(args_ptr);
    args.first_scissor = first_scissor;
    args.scissor_count = scissor_count;
    std::memcpy(args_ptr + header_size, scissors,
                sizeof(VkRect2D) * scissor_count);
    // Dynamic state persists across render pass boundaries, so this is kept
    // outside the per-pass tracking.
    if (first_scissor == 0 && scissor_count) {
      current_scissor_ = scissors[0];
    }
  }

  void CmdVkSetStencilCompareMask(VkStencilFaceFlags face_mask,
                                  uint32_t compare_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilCompareMask,
                     sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = compare_mask;
  }

  void CmdVkSetStencilReference(VkStencilFaceFlags face_mask,
                                uint32_t reference) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(WriteCommand(
        Command::kVkSetStencilReference, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = reference;
  }

  void CmdVkSetStencilWriteMask(VkStencilFaceFlags face_mask,
                                uint32_t write_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(WriteCommand(
        Command::kVkSetStencilWriteMask, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = write_mask;
  }

  void CmdVkSetViewport(uint32_t first_viewport, uint32_t viewport_count,
                        const VkViewport* viewports) {
    constexpr size_t header_size =
        xe::align(sizeof(ArgsVkSetViewport), alignof(VkViewport));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetViewport,
                     header_size + sizeof(VkViewport) * viewport_count));
    auto& args = *reinterpret_cast<ArgsVkSetViewport*>(args_ptr);
    args.first_viewport = first_viewport;
    args.viewport_count = viewport_count;
    std::memcpy(args_ptr + header_size, viewports,
                sizeof(VkViewport) * viewport_count);
  }

  // Extended dynamic state (EDS1/EDS2, core in Vulkan 1.3). Each setter mirrors
  // the value that the static pipeline would otherwise bake.
  void CmdVkSetCullMode(VkCullModeFlags cull_mode) {
    auto& args = *reinterpret_cast<ArgsVkSetCullMode*>(
        WriteCommand(Command::kVkSetCullMode, sizeof(ArgsVkSetCullMode)));
    args.cull_mode = cull_mode;
  }

  void CmdVkSetFrontFace(VkFrontFace front_face) {
    auto& args = *reinterpret_cast<ArgsVkSetFrontFace*>(
        WriteCommand(Command::kVkSetFrontFace, sizeof(ArgsVkSetFrontFace)));
    args.front_face = front_face;
  }

  void CmdVkSetPrimitiveTopology(VkPrimitiveTopology primitive_topology) {
    auto& args = *reinterpret_cast<ArgsVkSetPrimitiveTopology*>(WriteCommand(
        Command::kVkSetPrimitiveTopology, sizeof(ArgsVkSetPrimitiveTopology)));
    args.primitive_topology = primitive_topology;
  }

  void CmdVkSetPrimitiveRestartEnable(VkBool32 primitive_restart_enable) {
    auto& args = *reinterpret_cast<ArgsVkSetBool*>(
        WriteCommand(Command::kVkSetPrimitiveRestartEnable, sizeof(ArgsVkSetBool)));
    args.value = primitive_restart_enable;
  }

  void CmdVkSetDepthTestEnable(VkBool32 depth_test_enable) {
    auto& args = *reinterpret_cast<ArgsVkSetBool*>(
        WriteCommand(Command::kVkSetDepthTestEnable, sizeof(ArgsVkSetBool)));
    args.value = depth_test_enable;
  }

  void CmdVkSetDepthWriteEnable(VkBool32 depth_write_enable) {
    auto& args = *reinterpret_cast<ArgsVkSetBool*>(
        WriteCommand(Command::kVkSetDepthWriteEnable, sizeof(ArgsVkSetBool)));
    args.value = depth_write_enable;
  }

  void CmdVkSetDepthCompareOp(VkCompareOp depth_compare_op) {
    auto& args = *reinterpret_cast<ArgsVkSetDepthCompareOp*>(WriteCommand(
        Command::kVkSetDepthCompareOp, sizeof(ArgsVkSetDepthCompareOp)));
    args.depth_compare_op = depth_compare_op;
  }

  void CmdVkSetStencilTestEnable(VkBool32 stencil_test_enable) {
    auto& args = *reinterpret_cast<ArgsVkSetBool*>(
        WriteCommand(Command::kVkSetStencilTestEnable, sizeof(ArgsVkSetBool)));
    args.value = stencil_test_enable;
  }

  // VK_KHR_fragment_shading_rate, pipeline rate only (both combiners KEEP).
  void CmdVkSetFragmentShadingRateKHR(uint32_t width, uint32_t height) {
    auto& args = *reinterpret_cast<ArgsVkSetFragmentShadingRateKHR*>(
        WriteCommand(Command::kVkSetFragmentShadingRateKHR,
                     sizeof(ArgsVkSetFragmentShadingRateKHR)));
    args.width = width;
    args.height = height;
  }

  void CmdVkSetStencilOp(VkStencilFaceFlags face_mask, VkStencilOp fail_op,
                         VkStencilOp pass_op, VkStencilOp depth_fail_op,
                         VkCompareOp compare_op) {
    auto& args = *reinterpret_cast<ArgsVkSetStencilOp*>(
        WriteCommand(Command::kVkSetStencilOp, sizeof(ArgsVkSetStencilOp)));
    args.face_mask = face_mask;
    args.fail_op = fail_op;
    args.pass_op = pass_op;
    args.depth_fail_op = depth_fail_op;
    args.compare_op = compare_op;
  }

  // Extended dynamic state 3 (EDS3, VK_EXT_extended_dynamic_state3). Only
  // recorded when the corresponding sub-feature is supported.
  void CmdVkSetDepthClampEnableEXT(VkBool32 depth_clamp_enable) {
    auto& args = *reinterpret_cast<ArgsVkSetBool*>(WriteCommand(
        Command::kVkSetDepthClampEnableEXT, sizeof(ArgsVkSetBool)));
    args.value = depth_clamp_enable;
  }

  void CmdVkSetPolygonModeEXT(VkPolygonMode polygon_mode) {
    auto& args = *reinterpret_cast<ArgsVkSetPolygonModeEXT*>(WriteCommand(
        Command::kVkSetPolygonModeEXT, sizeof(ArgsVkSetPolygonModeEXT)));
    args.polygon_mode = polygon_mode;
  }

  void CmdVkSetColorBlendEnableEXT(uint32_t first_attachment,
                                   uint32_t attachment_count,
                                   const VkBool32* color_blend_enables) {
    constexpr size_t header_size =
        xe::align(sizeof(ArgsVkSetColorBlendEnableEXT), alignof(VkBool32));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkSetColorBlendEnableEXT,
        header_size + sizeof(VkBool32) * attachment_count));
    auto& args = *reinterpret_cast<ArgsVkSetColorBlendEnableEXT*>(args_ptr);
    args.first_attachment = first_attachment;
    args.attachment_count = attachment_count;
    std::memcpy(args_ptr + header_size, color_blend_enables,
                sizeof(VkBool32) * attachment_count);
  }

  void CmdVkSetColorBlendEquationEXT(
      uint32_t first_attachment, uint32_t attachment_count,
      const VkColorBlendEquationEXT* color_blend_equations) {
    constexpr size_t header_size = xe::align(
        sizeof(ArgsVkSetColorBlendEquationEXT), alignof(VkColorBlendEquationEXT));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkSetColorBlendEquationEXT,
        header_size + sizeof(VkColorBlendEquationEXT) * attachment_count));
    auto& args = *reinterpret_cast<ArgsVkSetColorBlendEquationEXT*>(args_ptr);
    args.first_attachment = first_attachment;
    args.attachment_count = attachment_count;
    std::memcpy(args_ptr + header_size, color_blend_equations,
                sizeof(VkColorBlendEquationEXT) * attachment_count);
  }

  void CmdVkSetColorWriteMaskEXT(uint32_t first_attachment,
                                 uint32_t attachment_count,
                                 const VkColorComponentFlags* color_write_masks) {
    constexpr size_t header_size = xe::align(
        sizeof(ArgsVkSetColorWriteMaskEXT), alignof(VkColorComponentFlags));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkSetColorWriteMaskEXT,
        header_size + sizeof(VkColorComponentFlags) * attachment_count));
    auto& args = *reinterpret_cast<ArgsVkSetColorWriteMaskEXT*>(args_ptr);
    args.first_attachment = first_attachment;
    args.attachment_count = attachment_count;
    std::memcpy(args_ptr + header_size, color_write_masks,
                sizeof(VkColorComponentFlags) * attachment_count);
  }

  // Debug marker support for RenderDoc/debug tools annotation.
  void CmdVkBeginDebugUtilsLabelEXT(const char* label_name) {
    size_t label_len = std::strlen(label_name);
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkBeginDebugUtilsLabelEXT,
                     sizeof(ArgsVkDebugUtilsLabel) + label_len + 1));
    auto& args = *reinterpret_cast<ArgsVkDebugUtilsLabel*>(args_ptr);
    args.label_length = static_cast<uint32_t>(label_len);
    std::memcpy(args_ptr + sizeof(ArgsVkDebugUtilsLabel), label_name,
                label_len + 1);
  }

  void CmdVkEndDebugUtilsLabelEXT() {
    WriteCommand(Command::kVkEndDebugUtilsLabelEXT, 0);
  }

  void CmdVkInsertDebugUtilsLabelEXT(const char* label_name) {
    size_t label_len = std::strlen(label_name);
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkInsertDebugUtilsLabelEXT,
                     sizeof(ArgsVkDebugUtilsLabel) + label_len + 1));
    auto& args = *reinterpret_cast<ArgsVkDebugUtilsLabel*>(args_ptr);
    args.label_length = static_cast<uint32_t>(label_len);
    std::memcpy(args_ptr + sizeof(ArgsVkDebugUtilsLabel), label_name,
                label_len + 1);
  }

 private:
  bool pass_bytes_enabled_ = false;
  std::map<size_t, std::vector<PassByteLedger::Event>> pass_bytes_notes_;
  // Clears folded into the load op of a pass without draws since Reset
  // (MergeClassicClearPasses needs at least one).
  uint32_t folded_clear_count_ = 0;

  enum class Command {
    kVkBeginRenderPass,
    kVkBindDescriptorSets,
    kVkBindIndexBuffer,
    kVkBindPipeline,
    kVkBindPipelineDeferred,
    kVkBindVertexBuffers,
    kVkBeginQuery,
    kVkEndQuery,
    kVkResetQueryPool,
    kVkWriteTimestamp,
    kVkClearAttachments,
    kVkClearColorImage,
    kVkCopyBuffer,
    kVkCopyBufferToImage,
    kVkCopyImage,
    kVkCopyImageToBuffer,
    kVkFillBuffer,
    kVkBlitImage,
    kVkDispatch,
    kVkDraw,
    kVkDrawIndexed,
    kVkEndRenderPass,
    kVkBeginRendering,
    kVkEndRendering,
    kVkPipelineBarrier,
    kVkPushConstants,
    kVkSetBlendConstants,
    kVkSetRenderingInputAttachmentIndices,
    kVkSetDepthBias,
    kVkSetScissor,
    kVkSetStencilCompareMask,
    kVkSetStencilReference,
    kVkSetStencilWriteMask,
    kVkSetViewport,
    kVkSetCullMode,
    kVkSetFrontFace,
    kVkSetPrimitiveTopology,
    kVkSetPrimitiveRestartEnable,
    kVkSetDepthTestEnable,
    kVkSetDepthWriteEnable,
    kVkSetDepthCompareOp,
    kVkSetStencilTestEnable,
    kVkSetStencilOp,
    kVkSetFragmentShadingRateKHR,
    kVkSetDepthClampEnableEXT,
    kVkSetPolygonModeEXT,
    kVkSetColorBlendEnableEXT,
    kVkSetColorBlendEquationEXT,
    kVkSetColorWriteMaskEXT,
    kVkBeginDebugUtilsLabelEXT,
    kVkEndDebugUtilsLabelEXT,
    kVkInsertDebugUtilsLabelEXT,
    kPassHeadSlot,
    kUpdateDescriptorSets,
    kNoOp,
  };

  struct CommandHeader {
    Command command;
    uint32_t arguments_size_elements;
  };
  static constexpr size_t kCommandHeaderSizeElements =
      (sizeof(CommandHeader) + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);

  struct ArgsVkBeginRenderPass {
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D render_area;
    uint32_t clear_value_count;
    VkSubpassContents contents;
    uint32_t classic_ops;
    uint32_t changed_ops;
    uint32_t folded_clears;
    uint32_t discarded_stores;
    uint32_t clear_value_capacity;
    VkImage attachment_images[5];
    VkExtent2D attachment_extents[5];
    // Followed by aligned optional VkClearValue[].
    static_assert(alignof(VkClearValue) <= alignof(uintmax_t));
  };

  // For VK_KHR_dynamic_rendering / Vulkan 1.3.
  struct ArgsVkBeginRendering {
    VkRenderingFlags flags;
    VkRect2D render_area;
    uint32_t layer_count;
    uint32_t view_mask;
    uint32_t color_attachment_count;
    bool has_depth_attachment;
    bool has_stencil_attachment;
    uint32_t msrtss_samples;
    // Followed by:
    // - VkRenderingAttachmentInfo[color_attachment_count] for color attachments
    // - VkRenderingAttachmentInfo for depth (if has_depth_attachment)
    // - VkRenderingAttachmentInfo for stencil (if has_stencil_attachment)
    static_assert(alignof(VkRenderingAttachmentInfo) <= alignof(uintmax_t));
  };

  struct ArgsVkBindDescriptorSets {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipelineLayout layout;
    uint32_t first_set;
    uint32_t descriptor_set_count;
    uint32_t dynamic_offset_count;
    // Followed by aligned VkDescriptorSet[], optional uint32_t[].
    static_assert(alignof(VkDescriptorSet) <= alignof(uintmax_t));
  };

  struct ArgsUpdateDescriptorSets {
    uint32_t write_count;
    uint32_t padding;
    // Followed by PackedDescriptorWrite[write_count], then the image / buffer
    // infos of each write in order.
  };
  struct PackedDescriptorWrite {
    VkDescriptorSet dst_set;
    uint32_t dst_binding;
    uint32_t dst_array_element;
    uint32_t descriptor_count;
    VkDescriptorType descriptor_type;
    uint32_t is_image;
    uint32_t padding;
  };
  static_assert(sizeof(ArgsUpdateDescriptorSets) % 8 == 0);
  static_assert(sizeof(PackedDescriptorWrite) % 8 == 0);
  static_assert(sizeof(VkDescriptorImageInfo) % 8 == 0);
  static_assert(sizeof(VkDescriptorBufferInfo) % 8 == 0);

  struct ArgsVkBindIndexBuffer {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType index_type;
  };

  struct ArgsVkBindPipeline {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipeline pipeline;
  };

  struct ArgsVkBindPipelineDeferred {
    VkPipelineBindPoint pipeline_bind_point;
    // The slot's handle as observed at record time; the recorded draws'
    // descriptor bindings only cover this pipeline's layout. Null when the
    // async creation had not finished by then - a slot that fills in between
    // record and Execute() must still drop its draws.
    VkPipeline pipeline;
  };

  struct ArgsVkBindVertexBuffers {
    uint32_t first_binding;
    uint32_t binding_count;
    // Followed by aligned VkBuffer[], VkDeviceSize[].
    static_assert(alignof(VkBuffer) <= alignof(uintmax_t));
    static_assert(alignof(VkDeviceSize) <= alignof(uintmax_t));
  };

  struct ArgsVkBeginQuery {
    VkQueryPool query_pool;
    uint32_t query;
    VkQueryControlFlags flags;
  };

  struct ArgsVkEndQuery {
    VkQueryPool query_pool;
    uint32_t query;
  };

  struct ArgsVkResetQueryPool {
    VkQueryPool query_pool;
    uint32_t first_query;
    uint32_t query_count;
  };

  struct ArgsVkWriteTimestamp {
    VkPipelineStageFlagBits pipeline_stage;
    VkQueryPool query_pool;
    uint32_t query;
  };

  struct ArgsVkClearAttachments {
    uint32_t attachment_count;
    uint32_t rect_count;
    // Followed by aligned VkClearAttachment[], VkClearRect[].
    static_assert(alignof(VkClearAttachment) <= alignof(uintmax_t));
    static_assert(alignof(VkClearRect) <= alignof(uintmax_t));
  };

  struct ArgsVkClearColorImage {
    VkImage image;
    VkImageLayout image_layout;
    VkClearColorValue color;
    uint32_t range_count;
    // Followed by aligned VkImageSubresourceRange[].
    static_assert(alignof(VkImageSubresourceRange) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBuffer {
    VkBuffer src_buffer;
    VkBuffer dst_buffer;
    uint32_t region_count;
    // Followed by aligned VkBufferCopy[].
    static_assert(alignof(VkBufferCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBufferToImage {
    VkBuffer src_buffer;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkBufferImageCopy[].
    static_assert(alignof(VkBufferImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyImage {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkImageCopy[].
    static_assert(alignof(VkImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyImageToBuffer {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkBuffer dst_buffer;
    uint32_t region_count;
    // Followed by aligned VkBufferImageCopy[].
    static_assert(alignof(VkBufferImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkFillBuffer {
    VkBuffer dst_buffer;
    VkDeviceSize dst_offset;
    VkDeviceSize size;
    uint32_t data;
  };

  struct ArgsVkBlitImage {
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    VkFilter filter;
    // Followed by aligned VkImageBlit[].
    static_assert(alignof(VkImageBlit) <= alignof(uintmax_t));
  };

  struct ArgsVkDispatch {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
  };

  struct ArgsVkDraw {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
  };

  struct ArgsVkDrawIndexed {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
  };

  struct ArgsVkPipelineBarrier {
    VkPipelineStageFlags src_stage_mask;
    VkPipelineStageFlags dst_stage_mask;
    VkDependencyFlags dependency_flags;
    uint32_t memory_barrier_count;
    uint32_t buffer_memory_barrier_count;
    uint32_t image_memory_barrier_count;
    // Followed by aligned optional VkMemoryBarrier[],
    // optional VkBufferMemoryBarrier[], optional VkImageMemoryBarrier[].
    static_assert(alignof(VkMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkBufferMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkImageMemoryBarrier) <= alignof(uintmax_t));
  };

  struct ArgsVkPushConstants {
    VkPipelineLayout layout;
    VkShaderStageFlags stage_flags;
    uint32_t offset;
    uint32_t size;
    // Followed by `size` bytes of values.
  };

  struct ArgsVkSetBlendConstants {
    float blend_constants[4];
  };

  struct ArgsVkSetRenderingInputAttachmentIndices {
    uint32_t color_attachment_count;
    uint32_t color_attachment_input_indices[4];
  };

  struct ArgsVkSetDepthBias {
    float depth_bias_constant_factor;
    float depth_bias_clamp;
    float depth_bias_slope_factor;
  };

  struct ArgsVkSetScissor {
    uint32_t first_scissor;
    uint32_t scissor_count;
    // Followed by aligned VkRect2D[].
    static_assert(alignof(VkRect2D) <= alignof(uintmax_t));
  };

  struct ArgsSetStencilMaskReference {
    VkStencilFaceFlags face_mask;
    uint32_t mask_reference;
  };

  struct ArgsVkSetViewport {
    uint32_t first_viewport;
    uint32_t viewport_count;
    // Followed by aligned VkViewport[].
    static_assert(alignof(VkViewport) <= alignof(uintmax_t));
  };

  // Extended dynamic state argument structures.
  struct ArgsVkSetBool {
    VkBool32 value;
  };

  struct ArgsVkSetCullMode {
    VkCullModeFlags cull_mode;
  };

  struct ArgsVkSetFrontFace {
    VkFrontFace front_face;
  };

  struct ArgsVkSetPrimitiveTopology {
    VkPrimitiveTopology primitive_topology;
  };

  struct ArgsVkSetDepthCompareOp {
    VkCompareOp depth_compare_op;
  };

  struct ArgsVkSetFragmentShadingRateKHR {
    uint32_t width;
    uint32_t height;
  };

  struct ArgsVkSetStencilOp {
    VkStencilFaceFlags face_mask;
    VkStencilOp fail_op;
    VkStencilOp pass_op;
    VkStencilOp depth_fail_op;
    VkCompareOp compare_op;
  };

  struct ArgsVkSetPolygonModeEXT {
    VkPolygonMode polygon_mode;
  };

  struct ArgsVkSetColorBlendEnableEXT {
    uint32_t first_attachment;
    uint32_t attachment_count;
    // Followed by aligned VkBool32[].
    static_assert(alignof(VkBool32) <= alignof(uintmax_t));
  };

  struct ArgsVkSetColorBlendEquationEXT {
    uint32_t first_attachment;
    uint32_t attachment_count;
    // Followed by aligned VkColorBlendEquationEXT[].
    static_assert(alignof(VkColorBlendEquationEXT) <= alignof(uintmax_t));
  };

  struct ArgsVkSetColorWriteMaskEXT {
    uint32_t first_attachment;
    uint32_t attachment_count;
    // Followed by aligned VkColorComponentFlags[].
    static_assert(alignof(VkColorComponentFlags) <= alignof(uintmax_t));
  };

  struct ArgsVkDebugUtilsLabel {
    uint32_t label_length;
    // Followed by null-terminated label string.
  };

  void* WriteCommand(Command command, size_t arguments_size_bytes);

  static constexpr size_t kCommandCountSlots = 64;
  uint64_t command_counts_[kCommandCountSlots] = {};

 public:
  // Shrinks the last recorded begin-pass render area to the union of the
  // pass's draw scissors and clear rects, rounded up to the granularity. Only
  // ever shrinks; a pass with no recorded draws keeps the full extent. Must be
  // called before the stream is executed, while the args are still patchable.
  void ShrinkRenderAreaToDrawn(uint32_t granularity_width,
                               uint32_t granularity_height);
  // The pass is ended: the render area handle must not survive into the next
  // pass or across a Reset().
  void EndRenderAreaTracking() {
    render_area_patch_offset_ = kNoRenderAreaPatch;
    lazy_clear_count_ = 0;
  }

  // Diagnostic: totals of recorded commands by category since creation.
  struct CommandStats {
    uint64_t draws = 0;
    uint64_t pipeline_binds = 0;
    uint64_t descriptor_set_binds = 0;
    uint64_t push_constants = 0;
    uint64_t viewports_scissors = 0;
    uint64_t other_dynamic_state = 0;
    uint64_t barriers = 0;
    uint64_t index_vertex_buffer_binds = 0;
  };
  CommandStats GetCommandStats() const;

 private:
  // Offset of the last recorded begin-pass argument struct, in stream elements
  // (not a pointer - the stream vector reallocates as it grows).
  static constexpr size_t kNoRenderAreaPatch = SIZE_MAX;
  size_t render_area_patch_offset_ = kNoRenderAreaPatch;
  bool render_area_patch_dynamic_ = false;
  VkRect2D current_scissor_ = {};
  uint32_t pass_drawn_width_ = 0;
  uint32_t pass_drawn_height_ = 0;
  uint32_t pass_height_source_ = 0;
  uint32_t pass_recorded_draws_ = 0;
  // A clear whose rectangles weren't accounted in the pass's drawn extent.
  bool pass_unaccounted_clear_ = false;
  bool unaccounted_before_emplace_ = false;
  struct LazyClear {
    size_t command_offset;
    uint32_t slot;
    VkRect2D rect;
  };
  std::array<LazyClear, kMaxLazyClears> lazy_clears_ = {};
  uint32_t lazy_clear_count_ = 0;
  bool IsLazyClearCommand(size_t command_offset) const {
    for (uint32_t i = 0; i < lazy_clear_count_; ++i) {
      if (lazy_clears_[i].command_offset == command_offset) return true;
    }
    return false;
  }
  // Bottom row (host pixels) the next recorded draw stays above, or UINT32_MAX
  // when unknown - set around guest draws only.
  uint32_t draw_height_bound_ = UINT32_MAX;

 public:
  void SetDrawHeightBound(uint32_t bound) { draw_height_bound_ = bound; }

  // Which attachments the draws recorded next touch: bit 0 depth / stencil
  // written, bits 1-4 color attachment 0-3 written, bit 8 depth / stencil
  // tested. Everything unless set around a draw whose state is known.
  static constexpr uint32_t kDrawAttachmentUseAll = 0x11F;
  void SetDrawAttachmentUse(uint32_t use) { draw_attachment_use_ = use; }
  // With dynamic rendering, relaxes the load / store operations of the last
  // recorded begin-pass for attachments no draw or clear of the pass wrote
  // (storeOp NONE: a tiler writes none of their tiles back) or, when
  // |load_op_none| is supported, neither wrote nor tested (loadOp NONE: nor
  // loads them). Must be called before ShrinkRenderAreaToDrawn, which drops
  // the patch handle. Returns the number of operations relaxed.
  uint32_t RelaxUnusedAttachmentOps(bool load_op_none);
  uint32_t FinalizeClassicRenderPass(VulkanRenderTargetCache& cache,
                                     bool load_store_none);
  uint32_t FoldClassicClears(VulkanRenderTargetCache& cache);
  // Merges clear-only passes into the next pass from begin on. Returns the
  // offset of a trailing clear-only pass that a pass recorded later may still
  // absorb (to keep unreplayed), or SIZE_MAX.
  size_t MergeClassicClearPasses(VulkanRenderTargetCache& cache,
                                 size_t begin = 0);
  void ApplyClassicStoreLookahead(VulkanRenderTargetCache& cache);
  void LogClassicPassOps() const;

 private:
  bool IsPassBoundaryCommand(size_t offset) const;
  uint32_t draw_attachment_use_ = kDrawAttachmentUseAll;
  uint32_t pass_attachment_use_ = 0;

 private:

  void BeginRenderAreaTracking(size_t arguments_size_bytes,
                               bool dynamic_rendering) {
    const size_t arguments_size_elements =
        (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
    render_area_patch_offset_ = command_stream_size_ - arguments_size_elements;
    render_area_patch_dynamic_ = dynamic_rendering;
    pass_drawn_width_ = 0;
    pass_drawn_height_ = 0;
    pass_height_source_ = 0;
    pass_recorded_draws_ = 0;
    pass_unaccounted_clear_ = false;
    pass_attachment_use_ = 0;
    lazy_clear_count_ = 0;
  }

  // source (diagnostic, VkShrink): 1 bounded guest draw, 2 draw without a
  // row bound (transfers, unbounded guest draws), 3 clear rectangle.
  void AccumulateDrawnRect(const VkRect2D& rect, uint32_t source = 3) {
    if (render_area_patch_offset_ == kNoRenderAreaPatch) {
      return;
    }
    pass_drawn_width_ =
        std::max(pass_drawn_width_,
                 uint32_t(std::max(0, rect.offset.x)) + rect.extent.width);
    const uint32_t bottom =
        uint32_t(std::max(0, rect.offset.y)) + rect.extent.height;
    if (bottom > pass_drawn_height_) {
      pass_drawn_height_ = bottom;
      pass_height_source_ = source;
    }
  }

  void AccumulateDrawnScissor() {
    if (render_area_patch_offset_ == kNoRenderAreaPatch) {
      return;
    }
    ++pass_recorded_draws_;
    pass_attachment_use_ |= draw_attachment_use_;
    if (draw_height_bound_ != UINT32_MAX) {
      // The draw's own conservative row bound (vulkan_render_area_draw_bound).
      VkRect2D rect = current_scissor_;
      const int64_t top = std::max(int32_t(0), rect.offset.y);
      const int64_t bottom = std::min(top + int64_t(rect.extent.height),
                                      int64_t(draw_height_bound_));
      rect.extent.height = uint32_t(std::max(int64_t(0), bottom - top));
      AccumulateDrawnRect(rect, 1);
      return;
    }
    AccumulateDrawnRect(current_scissor_, 2);
  }

  const VulkanCommandProcessor& command_processor_;

  // Reused across submissions; the first pass_head_slot_count_ are in use.
  std::vector<std::unique_ptr<DeferredCommandBuffer>> pass_head_slots_;
  uint32_t pass_head_slot_count_ = 0;

  std::function<void()> before_reallocate_;

  // uintmax_t to ensure uint64_t and pointer alignment of all structures.
  std::vector<uintmax_t> command_stream_;
  // Logical end of the recorded stream. With the size-cursor optimization the
  // vector is grown geometrically and never shrunk, so its size() is the
  // capacity of the arena, not the recorded length — vector::resize()'s
  // zero-fill on every WriteCommand was ~2% of the GPU command processor
  // thread. Maintained correctly in both modes.
  size_t command_stream_size_ = 0;
  std::atomic<uint64_t> dropped_draws_{0};
  // Snapshot of cvars::vulkan_deferred_cmd_size_cursor at construction.
  bool use_size_cursor_;
  static inline DispatchStats dispatch_stats_;
  static inline uint32_t dispatch_site_mask_ = 0;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_H_
