/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/deferred_command_buffer.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_instance.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DECLARE_bool(log_gpu_frame_time_breakdown);
DECLARE_bool(log_relaxed_attachment_ops);
DECLARE_bool(vulkan_render_area_tight);
DECLARE_bool(vulkan_relax_unused_attachment_loads);

DEFINE_bool(vulkan_classic_pass_finalize, false,
            "Finalize unused attachment ops with compatible classic render "
            "pass variants at pass end.",
            "Vulkan");
DEFINE_bool(vulkan_fold_clears_to_load_op, true,
            "Fold leading full attachment clears into classic loadOp CLEAR "
            "and defer clear-only passes to the next identical framebuffer.",
            "Vulkan");
UPDATE_from_bool(vulkan_fold_clears_to_load_op, 2026, 10, 24, 12, false);
DEFINE_bool(vulkan_store_lookahead, false,
            "Discard classic attachment stores killed by a proven next full "
            "overwrite in the same submission.",
            "Vulkan");

DEFINE_bool(vulkan_deferred_cmd_size_cursor, true,
            "Track the deferred command buffer's recorded length with a size "
            "cursor over a geometrically grown buffer instead of resizing the "
            "vector per recorded command (vector::resize zero-fill was ~2% of "
            "the GPU command processor thread).",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

DeferredCommandBuffer::DeferredCommandBuffer(
    const VulkanCommandProcessor& command_processor, size_t initial_size)
    : command_processor_(command_processor),
      use_size_cursor_(cvars::vulkan_deferred_cmd_size_cursor) {
  command_stream_.reserve(initial_size / sizeof(uintmax_t));
}

void DeferredCommandBuffer::Append(const DeferredCommandBuffer& commands) {
  assert_true(this != &commands);
  assert_zero(commands.pass_head_slot_count_);
  // Sidecar positions follow the appended tail, not its earlier capture time.
  if (pass_bytes_enabled_) {
    for (const auto& [offset, notes] : commands.pass_bytes_notes_) {
      auto& destination = pass_bytes_notes_[command_stream_size_ + offset];
      destination.insert(destination.end(), notes.begin(), notes.end());
    }
  }
  const size_t end = command_stream_size_ + commands.command_stream_size_;
  const size_t storage = use_size_cursor_
                             ? std::max(end, command_stream_.size())
                             : end;
  if (storage > command_stream_.capacity() && before_reallocate_) {
    // Streaming may be replaying the prefix while this tail is appended.
    before_reallocate_();
  }
  command_stream_.resize(storage);
  std::memcpy(command_stream_.data() + command_stream_size_,
              commands.command_stream_.data(),
              commands.command_stream_size_ * sizeof(uintmax_t));
  command_stream_size_ = end;
  for (size_t i = 0; i < kCommandCountSlots; ++i) {
    command_counts_[i] += commands.command_counts_[i];
  }
}

void DeferredCommandBuffer::Reset() {
  pass_bytes_notes_.clear();
  folded_clear_count_ = 0;
  command_stream_size_ = 0;
  if (!use_size_cursor_) {
    command_stream_.clear();
  }
  // The recorded arguments are gone; a stale patch handle would write into a
  // future command's storage.
  render_area_patch_offset_ = kNoRenderAreaPatch;
  pass_drawn_width_ = 0;
  pass_drawn_height_ = 0;
  pass_recorded_draws_ = 0;
  pass_attachment_use_ = 0;
  draw_attachment_use_ = kDrawAttachmentUseAll;
  current_scissor_ = {};
  pass_head_slot_count_ = 0;
}

uint32_t DeferredCommandBuffer::CmdPassHeadSlot(
    VkPipelineStageFlags src_stage_mask) {
  uint32_t slot = pass_head_slot_count_++;
  if (slot >= pass_head_slots_.size()) {
    if (before_reallocate_ &&
        pass_head_slots_.size() == pass_head_slots_.capacity()) {
      before_reallocate_();
    }
    pass_head_slots_.push_back(
        std::make_unique<DeferredCommandBuffer>(command_processor_, 4096));
  }
  pass_head_slots_[slot]->Reset();
  pass_head_slots_[slot]->SetPassBytesEnabled(pass_bytes_enabled_);
  auto* args = reinterpret_cast<uint32_t*>(
      WriteCommand(Command::kPassHeadSlot, sizeof(uint32_t) * 2));
  args[0] = slot;
  args[1] = uint32_t(src_stage_mask);
  return slot;
}

void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {
  // True while the last guest graphics pipeline bind in the stream resolved to
  // a slot whose asynchronous creation hasn't finished (or failed) - binding
  // VK_NULL_HANDLE is undefined behavior, and drawing with the previously
  // bound pipeline would use mismatched state, so draws are dropped until the
  // next successful graphics pipeline bind.
  bool guest_graphics_pipeline_unready = false;
  ExecuteRange(command_buffer, 0, command_stream_size_,
               guest_graphics_pipeline_unready);
}

void DeferredCommandBuffer::ExecuteRange(VkCommandBuffer command_buffer,
                                         size_t begin, size_t end,
                                         bool& guest_graphics_pipeline_unready) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  assert_true(begin <= end && end <= command_stream_size_);
  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  const uintmax_t* stream = command_stream_.data() + begin;
  size_t stream_remaining = end - begin;
  while (stream_remaining) {
    const CommandHeader& header =
        *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;

    switch (header.command) {
      case Command::kNoOp:
        break;
      case Command::kVkBeginRenderPass: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
        size_t offset_bytes = sizeof(ArgsVkBeginRenderPass);
        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = args.render_pass;
        render_pass_begin_info.framebuffer = args.framebuffer;
        render_pass_begin_info.renderArea = args.render_area;
        render_pass_begin_info.clearValueCount = args.clear_value_count;
        if (render_pass_begin_info.clearValueCount) {
          offset_bytes = xe::align(offset_bytes, alignof(VkClearValue));
          render_pass_begin_info.pClearValues =
              reinterpret_cast<const VkClearValue*>(
                  reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes +=
              sizeof(VkClearValue) * render_pass_begin_info.clearValueCount;
        } else {
          render_pass_begin_info.pClearValues = nullptr;
        }
        dfn.vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info,
                                 args.contents);
      } break;

      case Command::kVkBindDescriptorSets: {
        auto& args = *reinterpret_cast<const ArgsVkBindDescriptorSets*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkBindDescriptorSets),
                                        alignof(VkDescriptorSet));
        const VkDescriptorSet* descriptor_sets =
            reinterpret_cast<const VkDescriptorSet*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes += sizeof(VkDescriptorSet) * args.descriptor_set_count;
        const uint32_t* dynamic_offsets = nullptr;
        if (args.dynamic_offset_count) {
          offset_bytes = xe::align(offset_bytes, alignof(uint32_t));
          dynamic_offsets = reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(uint32_t) * args.dynamic_offset_count;
        }
        dfn.vkCmdBindDescriptorSets(command_buffer, args.pipeline_bind_point,
                                    args.layout, args.first_set,
                                    args.descriptor_set_count, descriptor_sets,
                                    args.dynamic_offset_count, dynamic_offsets);
      } break;

      case Command::kUpdateDescriptorSets: {
        auto& args =
            *reinterpret_cast<const ArgsUpdateDescriptorSets*>(stream);
        const auto* packed = reinterpret_cast<const PackedDescriptorWrite*>(
            reinterpret_cast<const uint8_t*>(stream) +
            sizeof(ArgsUpdateDescriptorSets));
        const uint8_t* info_ptr =
            reinterpret_cast<const uint8_t*>(packed + args.write_count);
        VkWriteDescriptorSet writes[16];
        uint32_t batch = 0;
        VkDevice device = command_processor_.GetVulkanDevice()->device();
        for (uint32_t i = 0; i < args.write_count; ++i) {
          const PackedDescriptorWrite& p = packed[i];
          VkWriteDescriptorSet& write = writes[batch++];
          write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write.pNext = nullptr;
          write.dstSet = p.dst_set;
          write.dstBinding = p.dst_binding;
          write.dstArrayElement = p.dst_array_element;
          write.descriptorCount = p.descriptor_count;
          write.descriptorType = p.descriptor_type;
          write.pTexelBufferView = nullptr;
          if (p.is_image) {
            write.pImageInfo =
                reinterpret_cast<const VkDescriptorImageInfo*>(info_ptr);
            write.pBufferInfo = nullptr;
            info_ptr += sizeof(VkDescriptorImageInfo) * p.descriptor_count;
          } else {
            write.pImageInfo = nullptr;
            write.pBufferInfo =
                reinterpret_cast<const VkDescriptorBufferInfo*>(info_ptr);
            info_ptr += sizeof(VkDescriptorBufferInfo) * p.descriptor_count;
          }
          if (batch == xe::countof(writes) || i + 1 == args.write_count) {
            dfn.vkUpdateDescriptorSets(device, batch, writes, 0, nullptr);
            batch = 0;
          }
        }
      } break;

      case Command::kVkBindIndexBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkBindIndexBuffer*>(stream);
        dfn.vkCmdBindIndexBuffer(command_buffer, args.buffer, args.offset,
                                 args.index_type);
      } break;

      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point,
                              args.pipeline);
        if (args.pipeline_bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
          guest_graphics_pipeline_unready = false;
        }
      } break;

      case Command::kVkBindPipelineDeferred: {
        auto& args =
            *reinterpret_cast<const ArgsVkBindPipelineDeferred*>(stream);
        if (args.pipeline != VK_NULL_HANDLE) {
          dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point,
                                args.pipeline);
          guest_graphics_pipeline_unready = false;
        } else {
          // The asynchronous creation had not completed when the draws were
          // recorded, so their descriptor bindings cover only the interim
          // pipeline layout - drop them, even if the pipeline exists by now.
          guest_graphics_pipeline_unready = true;
        }
      } break;

      case Command::kVkBindVertexBuffers: {
        auto& args = *reinterpret_cast<const ArgsVkBindVertexBuffers*>(stream);
        size_t offset_bytes =
            xe::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
        const VkBuffer* buffers = reinterpret_cast<const VkBuffer*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes =
            xe::align(offset_bytes + sizeof(VkBuffer) * args.binding_count,
                      alignof(VkDeviceSize));
        const VkDeviceSize* offsets = reinterpret_cast<const VkDeviceSize*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdBindVertexBuffers(command_buffer, args.first_binding,
                                   args.binding_count, buffers, offsets);
      } break;
      case Command::kVkBeginQuery: {
        auto& args = *reinterpret_cast<const ArgsVkBeginQuery*>(stream);
        dfn.vkCmdBeginQuery(command_buffer, args.query_pool, args.query,
                            args.flags);
      } break;
      case Command::kVkEndQuery: {
        auto& args = *reinterpret_cast<const ArgsVkEndQuery*>(stream);
        dfn.vkCmdEndQuery(command_buffer, args.query_pool, args.query);
      } break;
      case Command::kVkResetQueryPool: {
        auto& args = *reinterpret_cast<const ArgsVkResetQueryPool*>(stream);
        dfn.vkCmdResetQueryPool(command_buffer, args.query_pool,
                                args.first_query, args.query_count);
      } break;

      case Command::kVkWriteTimestamp: {
        auto& args = *reinterpret_cast<const ArgsVkWriteTimestamp*>(stream);
        dfn.vkCmdWriteTimestamp(command_buffer, args.pipeline_stage,
                                args.query_pool, args.query);
      } break;

      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkClearAttachments),
                                        alignof(VkClearAttachment));
        const VkClearAttachment* attachments =
            reinterpret_cast<const VkClearAttachment*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes = xe::align(
            offset_bytes + sizeof(VkClearAttachment) * args.attachment_count,
            alignof(VkClearRect));
        const VkClearRect* rects = reinterpret_cast<const VkClearRect*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdClearAttachments(command_buffer, args.attachment_count,
                                  attachments, args.rect_count, rects);
      } break;

      case Command::kVkClearColorImage: {
        auto& args = *reinterpret_cast<const ArgsVkClearColorImage*>(stream);
        dfn.vkCmdClearColorImage(
            command_buffer, args.image, args.image_layout, &args.color,
            args.range_count,
            reinterpret_cast<const VkImageSubresourceRange*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkClearColorImage),
                          alignof(VkImageSubresourceRange))));
      } break;

      case Command::kVkCopyBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBuffer*>(stream);
        dfn.vkCmdCopyBuffer(
            command_buffer, args.src_buffer, args.dst_buffer, args.region_count,
            reinterpret_cast<const VkBufferCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy))));
      } break;

      case Command::kVkFillBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkFillBuffer*>(stream);
        dfn.vkCmdFillBuffer(command_buffer, args.dst_buffer, args.dst_offset,
                            args.size, args.data);
      } break;

      case Command::kVkCopyBufferToImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(stream);
        dfn.vkCmdCopyBufferToImage(
            command_buffer, args.src_buffer, args.dst_image,
            args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBufferToImage),
                          alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImage*>(stream);
        dfn.vkCmdCopyImage(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_image, args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy))));
      } break;

      case Command::kVkCopyImageToBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImageToBuffer*>(stream);
        dfn.vkCmdCopyImageToBuffer(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_buffer, args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyImageToBuffer),
                          alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkBlitImage: {
        auto& args = *reinterpret_cast<const ArgsVkBlitImage*>(stream);
        dfn.vkCmdBlitImage(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_image, args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkImageBlit*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkBlitImage), alignof(VkImageBlit))),
            args.filter);
      } break;

      case Command::kVkDispatch: {
        auto& args = *reinterpret_cast<const ArgsVkDispatch*>(stream);
        dfn.vkCmdDispatch(command_buffer, args.group_count_x,
                          args.group_count_y, args.group_count_z);
      } break;

      case Command::kVkDraw: {
        if (guest_graphics_pipeline_unready) {
          dropped_draws_.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        auto& args = *reinterpret_cast<const ArgsVkDraw*>(stream);
        dfn.vkCmdDraw(command_buffer, args.vertex_count, args.instance_count,
                      args.first_vertex, args.first_instance);
      } break;

      case Command::kVkDrawIndexed: {
        if (guest_graphics_pipeline_unready) {
          dropped_draws_.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        auto& args = *reinterpret_cast<const ArgsVkDrawIndexed*>(stream);
        dfn.vkCmdDrawIndexed(command_buffer, args.index_count,
                             args.instance_count, args.first_index,
                             args.vertex_offset, args.first_instance);
      } break;

      case Command::kVkEndRenderPass:
        dfn.vkCmdEndRenderPass(command_buffer);
        break;

      case Command::kPassHeadSlot: {
        uint32_t slot = *reinterpret_cast<const uint32_t*>(stream);
        const VkPipelineStageFlags src_stage_mask =
            VkPipelineStageFlags(reinterpret_cast<const uint32_t*>(stream)[1]);
        DeferredCommandBuffer& slot_buffer = *pass_head_slots_[slot];
        if (!slot_buffer.empty()) {
          VkMemoryBarrier barrier;
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
          barrier.pNext = nullptr;
          barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
          barrier.dstAccessMask =
              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
          dfn.vkCmdPipelineBarrier(command_buffer, src_stage_mask,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                   &barrier, 0, nullptr, 0, nullptr);
          slot_buffer.Execute(command_buffer);
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask =
              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
          dfn.vkCmdPipelineBarrier(command_buffer,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                   &barrier, 0, nullptr, 0, nullptr);
        }
      } break;

      case Command::kVkBeginRendering: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRendering*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkBeginRendering),
                                        alignof(VkRenderingAttachmentInfo));
        const VkRenderingAttachmentInfo* color_attachments =
            args.color_attachment_count
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        offset_bytes +=
            sizeof(VkRenderingAttachmentInfo) * args.color_attachment_count;
        const VkRenderingAttachmentInfo* depth_attachment =
            args.has_depth_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        if (args.has_depth_attachment) {
          offset_bytes += sizeof(VkRenderingAttachmentInfo);
        }
        const VkRenderingAttachmentInfo* stencil_attachment =
            args.has_stencil_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = args.flags;
        rendering_info.renderArea = args.render_area;
        rendering_info.layerCount = args.layer_count;
        rendering_info.viewMask = args.view_mask;
        rendering_info.colorAttachmentCount = args.color_attachment_count;
        rendering_info.pColorAttachments = color_attachments;
        rendering_info.pDepthAttachment = depth_attachment;
        rendering_info.pStencilAttachment = stencil_attachment;
        VkMultisampledRenderToSingleSampledInfoEXT msrtss_info = {};
        if (args.msrtss_samples) {
          msrtss_info.sType =
              VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT;
          msrtss_info.multisampledRenderToSingleSampledEnable = VK_TRUE;
          msrtss_info.rasterizationSamples =
              VkSampleCountFlagBits(args.msrtss_samples);
          rendering_info.pNext = &msrtss_info;
        }
        dfn.vkCmdBeginRendering(command_buffer, &rendering_info);
      } break;

      case Command::kVkEndRendering:
        dfn.vkCmdEndRendering(command_buffer);
        break;

      case Command::kVkPipelineBarrier: {
        auto& args = *reinterpret_cast<const ArgsVkPipelineBarrier*>(stream);
        size_t barrier_offset_bytes = sizeof(ArgsVkPipelineBarrier);
        const VkMemoryBarrier* memory_barriers = nullptr;
        if (args.memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkMemoryBarrier));
          memory_barriers = reinterpret_cast<const VkMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkMemoryBarrier) * args.memory_barrier_count;
        }
        const VkBufferMemoryBarrier* buffer_memory_barriers = nullptr;
        if (args.buffer_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkBufferMemoryBarrier));
          buffer_memory_barriers =
              reinterpret_cast<const VkBufferMemoryBarrier*>(
                  reinterpret_cast<const uint8_t*>(stream) +
                  barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkBufferMemoryBarrier) * args.buffer_memory_barrier_count;
        }
        const VkImageMemoryBarrier* image_memory_barriers = nullptr;
        if (args.image_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkImageMemoryBarrier));
          image_memory_barriers = reinterpret_cast<const VkImageMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkImageMemoryBarrier) * args.image_memory_barrier_count;
        }
        dfn.vkCmdPipelineBarrier(
            command_buffer, args.src_stage_mask, args.dst_stage_mask,
            args.dependency_flags, args.memory_barrier_count, memory_barriers,
            args.buffer_memory_barrier_count, buffer_memory_barriers,
            args.image_memory_barrier_count, image_memory_barriers);
      } break;

      case Command::kVkPushConstants: {
        auto& args = *reinterpret_cast<const ArgsVkPushConstants*>(stream);
        dfn.vkCmdPushConstants(command_buffer, args.layout, args.stage_flags,
                               args.offset, args.size,
                               reinterpret_cast<const uint8_t*>(stream) +
                                   sizeof(ArgsVkPushConstants));
      } break;

      case Command::kVkSetRenderingInputAttachmentIndices: {
        auto& args = *reinterpret_cast<
            const ArgsVkSetRenderingInputAttachmentIndices*>(stream);
        VkRenderingInputAttachmentIndexInfo input_attachment_index_info;
        input_attachment_index_info.sType =
            VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO;
        input_attachment_index_info.pNext = nullptr;
        input_attachment_index_info.colorAttachmentCount =
            args.color_attachment_count;
        input_attachment_index_info.pColorAttachmentInputIndices =
            args.color_attachment_input_indices;
        input_attachment_index_info.pDepthInputAttachmentIndex = nullptr;
        input_attachment_index_info.pStencilInputAttachmentIndex = nullptr;
        dfn.vkCmdSetRenderingInputAttachmentIndices(
            command_buffer, &input_attachment_index_info);
      } break;

      case Command::kVkSetBlendConstants: {
        auto& args = *reinterpret_cast<const ArgsVkSetBlendConstants*>(stream);
        dfn.vkCmdSetBlendConstants(command_buffer, args.blend_constants);
      } break;

      case Command::kVkSetDepthBias: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias*>(stream);
        dfn.vkCmdSetDepthBias(command_buffer, args.depth_bias_constant_factor,
                              args.depth_bias_clamp,
                              args.depth_bias_slope_factor);
      } break;

      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(stream);
        dfn.vkCmdSetScissor(
            command_buffer, args.first_scissor, args.scissor_count,
            reinterpret_cast<const VkRect2D*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D))));
      } break;

      case Command::kVkSetStencilCompareMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilCompareMask(command_buffer, args.face_mask,
                                       args.mask_reference);
      } break;

      case Command::kVkSetStencilReference: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilReference(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetStencilWriteMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilWriteMask(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetViewport: {
        auto& args = *reinterpret_cast<const ArgsVkSetViewport*>(stream);
        dfn.vkCmdSetViewport(
            command_buffer, args.first_viewport, args.viewport_count,
            reinterpret_cast<const VkViewport*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetViewport), alignof(VkViewport))));
      } break;

      case Command::kVkSetCullMode: {
        auto& args = *reinterpret_cast<const ArgsVkSetCullMode*>(stream);
        dfn.vkCmdSetCullMode(command_buffer, args.cull_mode);
      } break;

      case Command::kVkSetFrontFace: {
        auto& args = *reinterpret_cast<const ArgsVkSetFrontFace*>(stream);
        dfn.vkCmdSetFrontFace(command_buffer, args.front_face);
      } break;

      case Command::kVkSetPrimitiveTopology: {
        auto& args =
            *reinterpret_cast<const ArgsVkSetPrimitiveTopology*>(stream);
        dfn.vkCmdSetPrimitiveTopology(command_buffer, args.primitive_topology);
      } break;

      case Command::kVkSetPrimitiveRestartEnable: {
        auto& args = *reinterpret_cast<const ArgsVkSetBool*>(stream);
        dfn.vkCmdSetPrimitiveRestartEnable(command_buffer, args.value);
      } break;

      case Command::kVkSetDepthTestEnable: {
        auto& args = *reinterpret_cast<const ArgsVkSetBool*>(stream);
        dfn.vkCmdSetDepthTestEnable(command_buffer, args.value);
      } break;

      case Command::kVkSetDepthWriteEnable: {
        auto& args = *reinterpret_cast<const ArgsVkSetBool*>(stream);
        dfn.vkCmdSetDepthWriteEnable(command_buffer, args.value);
      } break;

      case Command::kVkSetDepthCompareOp: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthCompareOp*>(stream);
        dfn.vkCmdSetDepthCompareOp(command_buffer, args.depth_compare_op);
      } break;

      case Command::kVkSetStencilTestEnable: {
        auto& args = *reinterpret_cast<const ArgsVkSetBool*>(stream);
        dfn.vkCmdSetStencilTestEnable(command_buffer, args.value);
      } break;

      case Command::kVkSetFragmentShadingRateKHR: {
        auto& args =
            *reinterpret_cast<const ArgsVkSetFragmentShadingRateKHR*>(stream);
        const VkExtent2D fragment_size = {args.width, args.height};
        const VkFragmentShadingRateCombinerOpKHR combiner_ops[2] = {
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        dfn.vkCmdSetFragmentShadingRateKHR(command_buffer, &fragment_size,
                                           combiner_ops);
      } break;

      case Command::kVkSetStencilOp: {
        auto& args = *reinterpret_cast<const ArgsVkSetStencilOp*>(stream);
        dfn.vkCmdSetStencilOp(command_buffer, args.face_mask, args.fail_op,
                              args.pass_op, args.depth_fail_op,
                              args.compare_op);
      } break;

      case Command::kVkSetDepthClampEnableEXT: {
        auto& args = *reinterpret_cast<const ArgsVkSetBool*>(stream);
        dfn.vkCmdSetDepthClampEnableEXT(command_buffer, args.value);
      } break;

      case Command::kVkSetPolygonModeEXT: {
        auto& args = *reinterpret_cast<const ArgsVkSetPolygonModeEXT*>(stream);
        dfn.vkCmdSetPolygonModeEXT(command_buffer, args.polygon_mode);
      } break;

      case Command::kVkSetColorBlendEnableEXT: {
        auto& args =
            *reinterpret_cast<const ArgsVkSetColorBlendEnableEXT*>(stream);
        dfn.vkCmdSetColorBlendEnableEXT(
            command_buffer, args.first_attachment, args.attachment_count,
            reinterpret_cast<const VkBool32*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetColorBlendEnableEXT),
                          alignof(VkBool32))));
      } break;

      case Command::kVkSetColorBlendEquationEXT: {
        auto& args =
            *reinterpret_cast<const ArgsVkSetColorBlendEquationEXT*>(stream);
        dfn.vkCmdSetColorBlendEquationEXT(
            command_buffer, args.first_attachment, args.attachment_count,
            reinterpret_cast<const VkColorBlendEquationEXT*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetColorBlendEquationEXT),
                          alignof(VkColorBlendEquationEXT))));
      } break;

      case Command::kVkSetColorWriteMaskEXT: {
        auto& args =
            *reinterpret_cast<const ArgsVkSetColorWriteMaskEXT*>(stream);
        dfn.vkCmdSetColorWriteMaskEXT(
            command_buffer, args.first_attachment, args.attachment_count,
            reinterpret_cast<const VkColorComponentFlags*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetColorWriteMaskEXT),
                          alignof(VkColorComponentFlags))));
      } break;

      case Command::kVkBeginDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdBeginDebugUtilsLabelEXT) {
          auto& args = *reinterpret_cast<const ArgsVkDebugUtilsLabel*>(stream);
          const char* label_name = reinterpret_cast<const char*>(
              reinterpret_cast<const uint8_t*>(stream) +
              sizeof(ArgsVkDebugUtilsLabel));
          VkDebugUtilsLabelEXT label_info = {};
          label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
          label_info.pLabelName = label_name;
          ifn.vkCmdBeginDebugUtilsLabelEXT(command_buffer, &label_info);
        }
      } break;

      case Command::kVkEndDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdEndDebugUtilsLabelEXT) {
          ifn.vkCmdEndDebugUtilsLabelEXT(command_buffer);
        }
      } break;

      case Command::kVkInsertDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdInsertDebugUtilsLabelEXT) {
          auto& args = *reinterpret_cast<const ArgsVkDebugUtilsLabel*>(stream);
          const char* label_name = reinterpret_cast<const char*>(
              reinterpret_cast<const uint8_t*>(stream) +
              sizeof(ArgsVkDebugUtilsLabel));
          VkDebugUtilsLabelEXT label_info = {};
          label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
          label_info.pLabelName = label_name;
          ifn.vkCmdInsertDebugUtilsLabelEXT(command_buffer, &label_info);
        }
      } break;

      default:
        assert_unhandled_case(header.command);
        break;
    }

    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }
}

void DeferredCommandBuffer::CmdVkPipelineBarrier(
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags, uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  size_t arguments_size = sizeof(ArgsVkPipelineBarrier);
  size_t memory_barriers_offset = 0;
  if (memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkMemoryBarrier));
    memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkMemoryBarrier) * memory_barrier_count;
  }
  size_t buffer_memory_barriers_offset = 0;
  if (buffer_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkBufferMemoryBarrier));
    buffer_memory_barriers_offset = arguments_size;
    arguments_size +=
        sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count;
  }
  size_t image_memory_barriers_offset = 0;
  if (image_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkImageMemoryBarrier));
    image_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkImageMemoryBarrier) * image_memory_barrier_count;
  }
  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
      WriteCommand(Command::kVkPipelineBarrier, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkPipelineBarrier*>(args_ptr);
  args.src_stage_mask = src_stage_mask;
  args.dst_stage_mask = dst_stage_mask;
  args.dependency_flags = dependency_flags;
  args.memory_barrier_count = memory_barrier_count;
  args.buffer_memory_barrier_count = buffer_memory_barrier_count;
  args.image_memory_barrier_count = image_memory_barrier_count;
  if (memory_barrier_count) {
    std::memcpy(args_ptr + memory_barriers_offset, memory_barriers,
                sizeof(VkMemoryBarrier) * memory_barrier_count);
  }
  if (buffer_memory_barrier_count) {
    std::memcpy(args_ptr + buffer_memory_barriers_offset,
                buffer_memory_barriers,
                sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count);
  }
  if (image_memory_barrier_count) {
    std::memcpy(args_ptr + image_memory_barriers_offset, image_memory_barriers,
                sizeof(VkImageMemoryBarrier) * image_memory_barrier_count);
  }
}

void DeferredCommandBuffer::CmdVkBeginRendering(
    const VkRenderingInfo* rendering_info, uint32_t msrtss_samples) {
  assert_null(rendering_info->pNext);

  size_t arguments_size = xe::align(sizeof(ArgsVkBeginRendering),
                                    alignof(VkRenderingAttachmentInfo));
  size_t color_attachments_offset = arguments_size;
  arguments_size +=
      sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount;
  size_t depth_attachment_offset = arguments_size;
  if (rendering_info->pDepthAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }
  size_t stencil_attachment_offset = arguments_size;
  if (rendering_info->pStencilAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }

  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
      WriteCommand(Command::kVkBeginRendering, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkBeginRendering*>(args_ptr);
  args.flags = rendering_info->flags;
  args.render_area = rendering_info->renderArea;
  args.layer_count = rendering_info->layerCount;
  args.view_mask = rendering_info->viewMask;
  args.color_attachment_count = rendering_info->colorAttachmentCount;
  args.has_depth_attachment = rendering_info->pDepthAttachment != nullptr;
  args.has_stencil_attachment = rendering_info->pStencilAttachment != nullptr;
  args.msrtss_samples = msrtss_samples;
  BeginRenderAreaTracking(arguments_size, /*dynamic_rendering=*/true);

  if (rendering_info->colorAttachmentCount) {
    std::memcpy(args_ptr + color_attachments_offset,
                rendering_info->pColorAttachments,
                sizeof(VkRenderingAttachmentInfo) *
                    rendering_info->colorAttachmentCount);
  }
  if (rendering_info->pDepthAttachment) {
    std::memcpy(args_ptr + depth_attachment_offset,
                rendering_info->pDepthAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
  if (rendering_info->pStencilAttachment) {
    std::memcpy(args_ptr + stencil_attachment_offset,
                rendering_info->pStencilAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
}

uint32_t DeferredCommandBuffer::RelaxUnusedAttachmentOps(bool load_op_none) {
  if (render_area_patch_offset_ == kNoRenderAreaPatch ||
      !render_area_patch_dynamic_) {
    return 0;
  }
  assert_true(render_area_patch_offset_ < command_stream_size_);
  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(command_stream_.data() +
                                                 render_area_patch_offset_);
  const auto& args = *reinterpret_cast<const ArgsVkBeginRendering*>(args_ptr);
  // Same layout as CmdVkBeginRendering writes.
  auto* attachment = reinterpret_cast<VkRenderingAttachmentInfo*>(
      args_ptr + xe::align(sizeof(ArgsVkBeginRendering),
                           alignof(VkRenderingAttachmentInfo)));
  const uint32_t use = pass_attachment_use_;
  uint32_t relaxed = 0;
  auto relax = [&](VkRenderingAttachmentInfo& info, bool written, bool read) {
    // An attachment the pass was entitled to leave undefined (loadOp
    // DONT_CARE) keeps its operations: only preserved contents stay preserved.
    if (info.imageView == VK_NULL_HANDLE || written ||
        info.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) {
      return;
    }
    if (info.storeOp == VK_ATTACHMENT_STORE_OP_STORE) {
      info.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
      ++relaxed;
    }
    if (load_op_none && !read && info.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
      info.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_EXT;
      ++relaxed;
    }
  };
  for (uint32_t i = 0; i < args.color_attachment_count; ++i, ++attachment) {
    // Blending reads a color attachment only where it also writes it.
    const bool written = i >= 4 || (use & (uint32_t(2) << i)) != 0;
    relax(*attachment, written, written);
  }
  const bool depth_written = (use & 0x1) != 0;
  const bool depth_read = (use & 0x100) != 0;
  if (args.has_depth_attachment) {
    relax(*attachment++, depth_written, depth_read);
  }
  if (args.has_stencil_attachment) {
    relax(*attachment++, depth_written, depth_read);
  }
  return relaxed;
}

uint32_t DeferredCommandBuffer::FinalizeClassicRenderPass(
    VulkanRenderTargetCache& cache, bool load_store_none) {
  if (render_area_patch_offset_ == kNoRenderAreaPatch ||
      render_area_patch_dynamic_ || !load_store_none) {
    return 0;
  }
  auto& args = *reinterpret_cast<ArgsVkBeginRenderPass*>(
      command_stream_.data() + render_area_patch_offset_);
  VulkanRenderTargetCache::RenderPassKey key;
  if (!cache.GetHostRenderPassKey(args.render_pass, key)) return 0;
  VulkanRenderTargetCache::RenderPassOps ops{args.classic_ops};
  uint32_t changed = 0;
  for (uint32_t aspect = 0; aspect < 6; ++aspect) {
    const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
    const uint32_t bit = 1u << slot;
    if (!(key.depth_and_color_used & bit) ||
        (key.depth_and_color_load_dont_care & bit) ||
        (pass_attachment_use_ & bit) ||
        ops.load(aspect) !=
            VulkanRenderTargetCache::RenderPassOps::kLoadDefault) {
      continue;
    }
    // DONT_CARE would discard preserved EDRAM even if no draw wrote it.
    // Without NONE support leave the original LOAD/STORE pair intact.
    ops.SetStore(aspect, VulkanRenderTargetCache::RenderPassOps::kStoreNone);
    ++changed;
    if (cvars::vulkan_relax_unused_attachment_loads &&
        (slot || !(pass_attachment_use_ & 0x100))) {
      ops.SetLoad(aspect, VulkanRenderTargetCache::RenderPassOps::kLoadNone);
      ++changed;
    }
  }
  if (!changed) return 0;
  VkRenderPass variant = cache.GetHostRenderTargetsRenderPass(key, ops);
  if (variant == VK_NULL_HANDLE) return 0;
  // Only ops change: framebuffer, pipeline compatibility, layouts and external
  // dependencies are identical. Patch before this pass is published to replay.
  args.render_pass = variant;
  args.classic_ops = ops.key;
  args.changed_ops = ops.ChangedOpCount();
  if (cvars::log_relaxed_attachment_ops) {
    XELOGI("VkClassicPass: begin={} changed_ops={}", render_area_patch_offset_,
           args.changed_ops);
  }
  return changed;
}

uint32_t DeferredCommandBuffer::ClipLazyClears(LazyClearResult* results) {
  if (!lazy_clear_count_ || render_area_patch_offset_ == kNoRenderAreaPatch) {
    return 0;
  }
  const VkRect2D area =
      render_area_patch_dynamic_
          ? reinterpret_cast<const ArgsVkBeginRendering*>(
                command_stream_.data() + render_area_patch_offset_)
                ->render_area
          : reinterpret_cast<const ArgsVkBeginRenderPass*>(
                command_stream_.data() + render_area_patch_offset_)
                ->render_area;
  for (uint32_t i = 0; i < lazy_clear_count_; ++i) {
    const LazyClear& lazy = lazy_clears_[i];
    auto& header = *reinterpret_cast<CommandHeader*>(command_stream_.data() +
                                                     lazy.command_offset);
    assert_true(header.command == Command::kVkClearAttachments);
    auto* ptr = reinterpret_cast<uint8_t*>(command_stream_.data() +
                                           lazy.command_offset +
                                           kCommandHeaderSizeElements);
    auto& rect = *reinterpret_cast<VkClearRect*>(
        ptr + xe::align(xe::align(sizeof(ArgsVkClearAttachments),
                                  alignof(VkClearAttachment)) +
                            sizeof(VkClearAttachment),
                        alignof(VkClearRect)));
    const int32_t x0 = std::max(lazy.rect.offset.x, area.offset.x);
    const int32_t y0 = std::max(lazy.rect.offset.y, area.offset.y);
    const int32_t x1 =
        std::min(lazy.rect.offset.x + int32_t(lazy.rect.extent.width),
                 area.offset.x + int32_t(area.extent.width));
    const int32_t y1 =
        std::min(lazy.rect.offset.y + int32_t(lazy.rect.extent.height),
                 area.offset.y + int32_t(area.extent.height));
    LazyClearResult& result = results[i];
    result.slot = lazy.slot;
    result.original = lazy.rect;
    result.applied = {};
    if (x1 > x0 && y1 > y0) {
      rect.rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
      result.applied = rect.rect;
    } else {
      // Nothing of it is in the area: the rows stay pending.
      header.command = Command::kNoOp;
      --command_counts_[size_t(Command::kVkClearAttachments)];
    }
  }
  return lazy_clear_count_;
}

uint32_t DeferredCommandBuffer::FoldClassicClears(
    VulkanRenderTargetCache& cache) {
  using Ops = VulkanRenderTargetCache::RenderPassOps;
  if (render_area_patch_offset_ == kNoRenderAreaPatch ||
      render_area_patch_dynamic_)
    return 0;
  auto& begin = *reinterpret_cast<ArgsVkBeginRenderPass*>(
      command_stream_.data() + render_area_patch_offset_);
  if (begin.clear_value_capacity < 5 || begin.clear_value_count) return 0;
  const auto& begin_header = *reinterpret_cast<const CommandHeader*>(
      command_stream_.data() + render_area_patch_offset_ -
      kCommandHeaderSizeElements);
  const size_t clear_offset =
      render_area_patch_offset_ + begin_header.arguments_size_elements;
  if (clear_offset == command_stream_size_) return 0;
  auto& header =
      *reinterpret_cast<CommandHeader*>(command_stream_.data() + clear_offset);
  if (header.command != Command::kVkClearAttachments) return 0;
  const auto* ptr = reinterpret_cast<const uint8_t*>(
      command_stream_.data() + clear_offset + kCommandHeaderSizeElements);
  const auto& clear = *reinterpret_cast<const ArgsVkClearAttachments*>(ptr);
  if (!clear.attachment_count || clear.rect_count != 1) return 0;
  const size_t attachments_offset =
      xe::align(sizeof(clear), alignof(VkClearAttachment));
  const auto* attachments =
      reinterpret_cast<const VkClearAttachment*>(ptr + attachments_offset);
  const auto& rect = *reinterpret_cast<const VkClearRect*>(
      ptr + xe::align(attachments_offset +
                          sizeof(VkClearAttachment) * clear.attachment_count,
                      alignof(VkClearRect)));
  if (rect.baseArrayLayer || rect.layerCount != 1 || rect.rect.offset.x ||
      rect.rect.offset.y || begin.render_area.offset.x ||
      begin.render_area.offset.y ||
      rect.rect.extent.width != begin.render_area.extent.width ||
      rect.rect.extent.height != begin.render_area.extent.height)
    return 0;
  VulkanRenderTargetCache::RenderPassKey key;
  if (!cache.GetHostRenderPassKey(begin.render_pass, key)) return 0;
  Ops ops{begin.classic_ops};
  VkClearValue values[5] = {};
  uint32_t changed = 0;
  for (uint32_t i = 0; i < clear.attachment_count; ++i) {
    const auto& attachment = attachments[i];
    const bool color = attachment.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!color && (!attachment.aspectMask ||
                   (attachment.aspectMask & ~(VK_IMAGE_ASPECT_DEPTH_BIT |
                                              VK_IMAGE_ASPECT_STENCIL_BIT))))
      return 0;
    const uint32_t slot = color ? attachment.colorAttachment + 1 : 0;
    if (slot >= 5 || !(key.depth_and_color_used & (1u << slot))) return 0;
    // The framebuffer may be smaller than its images. Require the actual
    // image extent, not just the render area, to preserve all EDRAM outside it.
    const VkExtent2D extent = begin.attachment_extents[slot];
    // A lazy clear was clipped to the render area; the cache keeps the rows
    // outside as pending constant rows, so they needn't be cleared here.
    if (!IsLazyClearCommand(clear_offset) &&
        (rect.rect.extent.width != extent.width ||
         rect.rect.extent.height != extent.height))
      return 0;
    const uint32_t index =
        xe::bit_count(key.depth_and_color_used & ((1u << slot) - 1));
    auto set_clear = [&](uint32_t aspect) {
      if (ops.load(aspect) != Ops::kLoadDefault) return false;
      ops.SetLoad(aspect, Ops::kLoadClear);
      ++changed;
      return true;
    };
    if (color) {
      if (!set_clear(slot + 1)) return 0;
      values[index].color = attachment.clearValue.color;
    } else {
      if (attachment.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
        if (!set_clear(0)) return 0;
        values[index].depthStencil.depth =
            attachment.clearValue.depthStencil.depth;
      }
      if (attachment.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
        if (!set_clear(1)) return 0;
        values[index].depthStencil.stencil =
            attachment.clearValue.depthStencil.stencil;
      }
    }
  }
  VkRenderPass variant = cache.GetHostRenderTargetsRenderPass(key, ops);
  if (variant == VK_NULL_HANDLE) return 0;
  auto* clear_values = reinterpret_cast<VkClearValue*>(
      reinterpret_cast<uint8_t*>(&begin) +
      xe::align(sizeof(begin), alignof(VkClearValue)));
  std::memcpy(clear_values, values, sizeof(values));
  begin.render_pass = variant;
  begin.classic_ops = ops.key;
  begin.clear_value_count = xe::bit_count(key.depth_and_color_used);
  begin.changed_ops = ops.ChangedOpCount();
  ++begin.folded_clears;
  // Folding runs at the pass end: a pass without draws may be clear-only.
  if (!pass_recorded_draws_) ++folded_clear_count_;
  header.command = Command::kNoOp;
  --command_counts_[size_t(Command::kVkClearAttachments)];
  // loadOp CLEAR must retain its full area even when subsequent draws touch a
  // smaller rectangle. The clear itself is now absent from the command stream.
  pass_unaccounted_clear_ = true;
  if (cvars::log_relaxed_attachment_ops) {
    XELOGI("VkFoldClear: begin={} changed_ops={} clear_commands=1",
           render_area_patch_offset_, changed);
  }
  return changed;
}

bool DeferredCommandBuffer::IsPassBoundaryCommand(size_t offset) const {
  const auto& header =
      *reinterpret_cast<const CommandHeader*>(command_stream_.data() + offset);
  const auto* args =
      command_stream_.data() + offset + kCommandHeaderSizeElements;
  switch (header.command) {
    case Command::kNoOp:
    case Command::kVkWriteTimestamp:
    case Command::kVkBeginDebugUtilsLabelEXT:
    case Command::kVkEndDebugUtilsLabelEXT:
    case Command::kVkInsertDebugUtilsLabelEXT:
      return true;
    case Command::kPassHeadSlot:
      return pass_head_slots_[*reinterpret_cast<const uint32_t*>(args)]
          ->empty();
    case Command::kVkPipelineBarrier:
      // Preserve every barrier. Moving a clear across an image layout
      // transition would need additional proof about access to the old data.
      return !reinterpret_cast<const ArgsVkPipelineBarrier*>(args)
                  ->image_memory_barrier_count;
    default:
      return false;
  }
}

size_t DeferredCommandBuffer::MergeClassicClearPasses(
    VulkanRenderTargetCache& cache, size_t begin) {
  // A clear-only pass is a pass without draws whose clears were folded.
  if (!folded_clear_count_) {
    return SIZE_MAX;
  }
  using Ops = VulkanRenderTargetCache::RenderPassOps;
  size_t pending_begin = SIZE_MAX, pending_end = SIZE_MAX;
  size_t current_begin = SIZE_MAX;
  bool clear_only = false;
  for (size_t offset = begin; offset < command_stream_size_;) {
    auto& header =
        *reinterpret_cast<CommandHeader*>(command_stream_.data() + offset);
    const size_t args_offset = offset + kCommandHeaderSizeElements;
    const size_t next = args_offset + header.arguments_size_elements;
    if (header.command == Command::kVkBeginRenderPass) {
      auto& dst = *reinterpret_cast<ArgsVkBeginRenderPass*>(
          command_stream_.data() + args_offset);
      if (pending_begin != SIZE_MAX) {
        auto& src = *reinterpret_cast<ArgsVkBeginRenderPass*>(
            command_stream_.data() + pending_begin +
            kCommandHeaderSizeElements);
        VulkanRenderTargetCache::RenderPassKey src_key, dst_key;
        if (src.framebuffer == dst.framebuffer &&
            dst.clear_value_capacity >= 5 && !src.render_area.offset.x &&
            !src.render_area.offset.y && !dst.render_area.offset.x &&
            !dst.render_area.offset.y &&
            src.render_area.extent.width >= dst.render_area.extent.width &&
            src.render_area.extent.height >= dst.render_area.extent.height &&
            cache.GetHostRenderPassKey(src.render_pass, src_key) &&
            cache.GetHostRenderPassKey(dst.render_pass, dst_key) &&
            src_key.depth_and_color_used == dst_key.depth_and_color_used) {
          Ops src_ops{src.classic_ops}, dst_ops{dst.classic_ops};
          bool merge = true;
          uint32_t changed = 0;
          // Restoring the full clear area may expand the successor's shrunken
          // render area. Never expand a discard of its other attachments.
          for (uint32_t aspect = 0; aspect < 6; ++aspect) {
            const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
            if (!(dst_key.depth_and_color_used & (1u << slot))) continue;
            const auto original =
                (dst_key.depth_and_color_load_dont_care & (1u << slot))
                    ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                    : VK_ATTACHMENT_LOAD_OP_LOAD;
            const auto load = dst_ops.LoadOp(aspect, original);
            if (load == VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
                (load == VK_ATTACHMENT_LOAD_OP_NONE_EXT &&
                 dst_ops.store(aspect) != Ops::kStoreNone))
              merge = false;
          }
          for (uint32_t aspect = 0; aspect < 6; ++aspect) {
            if (src_ops.load(aspect) != Ops::kLoadClear) continue;
            const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
            const auto original =
                (dst_key.depth_and_color_load_dont_care & (1u << slot))
                    ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                    : VK_ATTACHMENT_LOAD_OP_LOAD;
            if (dst_ops.LoadOp(aspect, original) !=
                    VK_ATTACHMENT_LOAD_OP_LOAD ||
                src_ops.store(aspect) != Ops::kStoreDefault) {
              merge = false;
              break;
            }
            dst_ops.SetLoad(aspect, Ops::kLoadClear);
            ++changed;
            if (dst_ops.store(aspect) != Ops::kStoreDefault) {
              dst_ops.SetStore(aspect, Ops::kStoreDefault);
              ++changed;
            }
          }
          VkRenderPass variant =
              merge && changed
                  ? cache.GetHostRenderTargetsRenderPass(dst_key, dst_ops)
                  : VK_NULL_HANDLE;
          if (variant != VK_NULL_HANDLE) {
            const auto* src_values = reinterpret_cast<const VkClearValue*>(
                reinterpret_cast<const uint8_t*>(&src) +
                xe::align(sizeof(src), alignof(VkClearValue)));
            auto* dst_values = reinterpret_cast<VkClearValue*>(
                reinterpret_cast<uint8_t*>(&dst) +
                xe::align(sizeof(dst), alignof(VkClearValue)));
            for (uint32_t aspect = 0; aspect < 6; ++aspect) {
              if (src_ops.load(aspect) != Ops::kLoadClear) continue;
              const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
              const uint32_t index = xe::bit_count(
                  src_key.depth_and_color_used & ((1u << slot) - 1));
              if (!aspect)
                dst_values[index].depthStencil.depth =
                    src_values[index].depthStencil.depth;
              else if (aspect == 1)
                dst_values[index].depthStencil.stencil =
                    src_values[index].depthStencil.stencil;
              else
                dst_values[index].color = src_values[index].color;
            }
            dst.render_pass = variant;
            dst.render_area = src.render_area;
            dst.classic_ops = dst_ops.key;
            dst.changed_ops = dst_ops.ChangedOpCount();
            dst.folded_clears += src.folded_clears;
            dst.clear_value_count = xe::bit_count(dst_key.depth_and_color_used);
            reinterpret_cast<CommandHeader*>(command_stream_.data() +
                                             pending_begin)
                ->command = Command::kNoOp;
            reinterpret_cast<CommandHeader*>(command_stream_.data() +
                                             pending_end)
                ->command = Command::kNoOp;
            --command_counts_[size_t(Command::kVkBeginRenderPass)];
            --command_counts_[size_t(Command::kVkEndRenderPass)];
            if (cvars::log_relaxed_attachment_ops) {
              XELOGI("VkMergeClear: source={} destination={} changed_ops={}",
                     pending_begin, offset, changed);
            }
          }
        }
      }
      pending_begin = pending_end = SIZE_MAX;
      current_begin = offset;
      clear_only = dst.folded_clears != 0;
    } else if (header.command == Command::kVkEndRenderPass) {
      if (current_begin != SIZE_MAX && clear_only) {
        pending_begin = current_begin;
        pending_end = offset;
      }
      current_begin = SIZE_MAX;
    } else if (current_begin != SIZE_MAX) {
      switch (header.command) {
        case Command::kNoOp:
        case Command::kVkWriteTimestamp:
        case Command::kVkBeginDebugUtilsLabelEXT:
        case Command::kVkEndDebugUtilsLabelEXT:
        case Command::kVkInsertDebugUtilsLabelEXT:
          break;
        default:
          clear_only = false;
          break;
      }
    } else if (!IsPassBoundaryCommand(offset)) {
      pending_begin = pending_end = SIZE_MAX;
    }
    offset = next;
  }
  return pending_begin;
}

void DeferredCommandBuffer::ApplyClassicStoreLookahead(
    VulkanRenderTargetCache& cache) {
  using Ops = VulkanRenderTargetCache::RenderPassOps;
  struct PendingStore {
    VkImage image;
    VkImageAspectFlags aspect_mask;
    uint32_t aspect;
    size_t begin;
  };
  std::vector<PendingStore> pending;
  auto access = [&](VkImage image, VkImageAspectFlags mask, bool overwrite) {
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->image != image || !(it->aspect_mask & mask)) {
        ++it;
        continue;
      }
      if (overwrite) {
        auto& begin = *reinterpret_cast<ArgsVkBeginRenderPass*>(
            command_stream_.data() + it->begin);
        VulkanRenderTargetCache::RenderPassKey key;
        if (cache.GetHostRenderPassKey(begin.render_pass, key)) {
          Ops ops{begin.classic_ops};
          ops.SetStore(it->aspect, Ops::kStoreDontCare);
          VkRenderPass variant = cache.GetHostRenderTargetsRenderPass(key, ops);
          if (variant != VK_NULL_HANDLE) {
            begin.render_pass = variant;
            begin.classic_ops = ops.key;
            begin.changed_ops = ops.ChangedOpCount();
            ++begin.discarded_stores;
          }
        }
      }
      it = pending.erase(it);
    }
  };
  auto aspect_mask = [](uint32_t aspect) -> VkImageAspectFlags {
    return aspect == 0   ? VK_IMAGE_ASPECT_DEPTH_BIT
           : aspect == 1 ? VK_IMAGE_ASPECT_STENCIL_BIT
                         : VK_IMAGE_ASPECT_COLOR_BIT;
  };
  constexpr VkImageAspectFlags kAllAspects = VK_IMAGE_ASPECT_COLOR_BIT |
                                             VK_IMAGE_ASPECT_DEPTH_BIT |
                                             VK_IMAGE_ASPECT_STENCIL_BIT;
  size_t current_begin = SIZE_MAX;
  for (size_t offset = 0; offset < command_stream_size_;) {
    const auto& header = *reinterpret_cast<const CommandHeader*>(
        command_stream_.data() + offset);
    const size_t args_offset = offset + kCommandHeaderSizeElements;
    const auto* ptr =
        reinterpret_cast<const uint8_t*>(command_stream_.data() + args_offset);
    switch (header.command) {
      case Command::kVkBeginRenderPass: {
        const auto& begin =
            *reinterpret_cast<const ArgsVkBeginRenderPass*>(ptr);
        VulkanRenderTargetCache::RenderPassKey key;
        if (!cache.GetHostRenderPassKey(begin.render_pass, key)) {
          pending.clear();
          current_begin = SIZE_MAX;
          break;
        }
        current_begin = args_offset;
        const Ops ops{begin.classic_ops};
        for (uint32_t aspect = 0; aspect < 6; ++aspect) {
          const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
          if (!(key.depth_and_color_used & (1u << slot))) continue;
          const auto original =
              (key.depth_and_color_load_dont_care & (1u << slot))
                  ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                  : VK_ATTACHMENT_LOAD_OP_LOAD;
          const auto load = ops.LoadOp(aspect, original);
          // NONE preserves memory; it is not evidence of an overwrite.
          if (load == VK_ATTACHMENT_LOAD_OP_NONE_EXT) continue;
          const VkExtent2D extent = begin.attachment_extents[slot];
          const bool full = !begin.render_area.offset.x &&
                            !begin.render_area.offset.y && extent.width &&
                            extent.height &&
                            begin.render_area.extent.width == extent.width &&
                            begin.render_area.extent.height == extent.height;
          access(begin.attachment_images[slot], aspect_mask(aspect),
                 full && (load == VK_ATTACHMENT_LOAD_OP_CLEAR ||
                          load == VK_ATTACHMENT_LOAD_OP_DONT_CARE));
        }
      } break;
      case Command::kVkEndRenderPass: {
        if (current_begin == SIZE_MAX) break;
        const auto& begin = *reinterpret_cast<const ArgsVkBeginRenderPass*>(
            command_stream_.data() + current_begin);
        VulkanRenderTargetCache::RenderPassKey key;
        if (cache.GetHostRenderPassKey(begin.render_pass, key)) {
          const Ops ops{begin.classic_ops};
          for (uint32_t aspect = 0; aspect < 6; ++aspect) {
            const uint32_t slot = aspect < 2 ? 0 : aspect - 1;
            if (!(key.depth_and_color_used & (1u << slot)) ||
                ops.store(aspect) != Ops::kStoreDefault ||
                begin.attachment_images[slot] == VK_NULL_HANDLE)
              continue;
            access(begin.attachment_images[slot], aspect_mask(aspect), false);
            pending.push_back({begin.attachment_images[slot],
                               aspect_mask(aspect), aspect, current_begin});
          }
        }
        current_begin = SIZE_MAX;
      } break;
      case Command::kVkClearColorImage: {
        const auto& clear =
            *reinterpret_cast<const ArgsVkClearColorImage*>(ptr);
        const auto* ranges = reinterpret_cast<const VkImageSubresourceRange*>(
            ptr + xe::align(sizeof(clear), alignof(VkImageSubresourceRange)));
        bool full = false;
        for (uint32_t i = 0; i < clear.range_count; ++i) {
          const auto& range = ranges[i];
          // RTC images have exactly one mip and layer. No inference for
          // partial copies or arbitrary shader writes is made here.
          full |= range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
                  !range.baseMipLevel && !range.baseArrayLayer &&
                  (range.levelCount == 1 ||
                   range.levelCount == VK_REMAINING_MIP_LEVELS) &&
                  (range.layerCount == 1 ||
                   range.layerCount == VK_REMAINING_ARRAY_LAYERS);
        }
        access(clear.image, VK_IMAGE_ASPECT_COLOR_BIT, full);
      } break;
      case Command::kVkCopyBufferToImage:
        access(reinterpret_cast<const ArgsVkCopyBufferToImage*>(ptr)->dst_image,
               kAllAspects, false);
        break;
      case Command::kVkCopyImage: {
        const auto& copy = *reinterpret_cast<const ArgsVkCopyImage*>(ptr);
        access(copy.src_image, kAllAspects, false);
        access(copy.dst_image, kAllAspects, false);
      } break;
      case Command::kVkCopyImageToBuffer:
        access(reinterpret_cast<const ArgsVkCopyImageToBuffer*>(ptr)->src_image,
               kAllAspects, false);
        break;
      case Command::kVkBlitImage: {
        const auto& blit = *reinterpret_cast<const ArgsVkBlitImage*>(ptr);
        access(blit.src_image, kAllAspects, false);
        access(blit.dst_image, kAllAspects, false);
      } break;
      case Command::kVkPipelineBarrier: {
        const auto& barrier =
            *reinterpret_cast<const ArgsVkPipelineBarrier*>(ptr);
        size_t bytes = sizeof(barrier);
        if (barrier.memory_barrier_count)
          bytes = xe::align(bytes, alignof(VkMemoryBarrier)) +
                  sizeof(VkMemoryBarrier) * barrier.memory_barrier_count;
        if (barrier.buffer_memory_barrier_count)
          bytes = xe::align(bytes, alignof(VkBufferMemoryBarrier)) +
                  sizeof(VkBufferMemoryBarrier) *
                      barrier.buffer_memory_barrier_count;
        if (barrier.image_memory_barrier_count) {
          const auto* images = reinterpret_cast<const VkImageMemoryBarrier*>(
              ptr + xe::align(bytes, alignof(VkImageMemoryBarrier)));
          for (uint32_t i = 0; i < barrier.image_memory_barrier_count; ++i) {
            if (images[i].oldLayout != images[i].newLayout ||
                images[i].srcQueueFamilyIndex !=
                    images[i].dstQueueFamilyIndex) {
              access(images[i].image, images[i].subresourceRange.aspectMask,
                     false);
            }
          }
        }
      } break;
      case Command::kPassHeadSlot:
        if (!pass_head_slots_[*reinterpret_cast<const uint32_t*>(ptr)]->empty())
          pending.clear();
        break;
      case Command::kNoOp:
      case Command::kUpdateDescriptorSets:
      case Command::kVkBindDescriptorSets:
      case Command::kVkBindIndexBuffer:
      case Command::kVkBindPipeline:
      case Command::kVkBindPipelineDeferred:
      case Command::kVkBindVertexBuffers:
      case Command::kVkCopyBuffer:
      case Command::kVkFillBuffer:
      case Command::kVkPushConstants:
      case Command::kVkBeginQuery:
      case Command::kVkEndQuery:
      case Command::kVkResetQueryPool:
      case Command::kVkWriteTimestamp:
      case Command::kVkBeginDebugUtilsLabelEXT:
      case Command::kVkEndDebugUtilsLabelEXT:
      case Command::kVkInsertDebugUtilsLabelEXT:
        break;
      default:
        // Draws and dispatches can sample any RTC image through descriptors.
        // An unclassified command is also a fence: never guess an image's next
        // access, recurse into late pass-head work, or carry state across
        // submits.
        pending.clear();
        break;
    }
    offset = args_offset + header.arguments_size_elements;
  }
}

void DeferredCommandBuffer::LogClassicPassOps() const {
  if (!cvars::log_relaxed_attachment_ops) return;
  for (size_t offset = 0; offset < command_stream_size_;) {
    const auto& header = *reinterpret_cast<const CommandHeader*>(
        command_stream_.data() + offset);
    const size_t args_offset = offset + kCommandHeaderSizeElements;
    if (header.command == Command::kVkBeginRenderPass) {
      const auto& begin = *reinterpret_cast<const ArgsVkBeginRenderPass*>(
          command_stream_.data() + args_offset);
      XELOGI(
          "VkClassicPassOps: begin={} changed_ops={} folded_clears={} "
          "discarded_stores={}",
          args_offset, begin.changed_ops, begin.folded_clears,
          begin.discarded_stores);
    }
    offset = args_offset + header.arguments_size_elements;
  }
}

static uint32_t xe_shrink_logged = 0;

void DeferredCommandBuffer::ShrinkRenderAreaToDrawn(
    uint32_t granularity_width, uint32_t granularity_height) {
  if (render_area_patch_offset_ == kNoRenderAreaPatch) {
    return;
  }
  const size_t patch_offset = render_area_patch_offset_;
  // A pass without draws whose clear rectangles are all accounted touches
  // exactly those rectangles (vulkan_render_area_tight); a pass with neither is
  // only load/store work and keeps the full extent.
  if ((!pass_recorded_draws_ && !cvars::vulkan_render_area_tight) ||
      !pass_drawn_width_ || !pass_drawn_height_ || pass_unaccounted_clear_) {
    // Nothing rasterized into the pass (or only load/store work) - the full
    // extent is the only safe answer.
    if (cvars::log_gpu_frame_time_breakdown && xe_shrink_logged < 200) {
      ++xe_shrink_logged;
      XELOGI("VkShrink: kept full extent (draws={} w={} h={} unaccounted={} "
             "h_by={})",
             pass_recorded_draws_, pass_drawn_width_, pass_drawn_height_,
             pass_unaccounted_clear_ ? 1 : 0, pass_height_source_);
    }
    return;
  }
  assert_true(patch_offset < command_stream_size_);
  VkRect2D* render_area;
  if (render_area_patch_dynamic_) {
    render_area = &reinterpret_cast<ArgsVkBeginRendering*>(
                       command_stream_.data() + patch_offset)
                       ->render_area;
  } else {
    render_area = &reinterpret_cast<ArgsVkBeginRenderPass*>(
                       command_stream_.data() + patch_offset)
                       ->render_area;
  }
  uint32_t width = pass_drawn_width_;
  uint32_t height = pass_drawn_height_;
  if (granularity_width > 1) {
    width = xe::round_up(width, granularity_width);
  }
  if (granularity_height > 1) {
    height = xe::round_up(height, granularity_height);
  }
  // Offsets stay at the recorded origin; only ever shrink.
  const uint32_t was_w = render_area->extent.width;
  const uint32_t was_h = render_area->extent.height;
  render_area->extent.width = std::min(width, render_area->extent.width);
  render_area->extent.height = std::min(height, render_area->extent.height);
  // Only the large passes matter here; startup floods the log otherwise.
  if (cvars::log_gpu_frame_time_breakdown && was_h >= 2000 &&
      xe_shrink_logged < 200) {
    ++xe_shrink_logged;
    XELOGI("VkShrink: {}x{} -> {}x{} ({} draws, dynamic={}, h_by={})", was_w,
           was_h, render_area->extent.width, render_area->extent.height,
           pass_recorded_draws_, render_area_patch_dynamic_ ? 1 : 0,
           pass_height_source_);
  }
}

void DeferredCommandBuffer::CollectPassBytes(const PassByteCatalog& catalog,
                                            PassByteLedger& ledger) const {
  using L = PassByteLedger;
  using E = L::Event;
  std::vector<E> attachments;
  uint32_t pass = 0;
  L::Category copy_category = L::kCopy;
  bool annotated_work = false;
  bool drew = false;
  auto add = [&](E e) { e.pass = pass; ledger.Add(e); };
  auto unknown = [&]() {
    E e; e.kind = L::kUnknown; add(e);
  };
  auto region = [&](uint64_t image, VkImageAspectFlags aspects, uint32_t mip,
                    uint32_t layer, L::Rect r, L::Kind kind, L::Category category) {
    E e; e.image = image; e.aspect = aspects; e.mip = mip; e.layer = layer;
    e.rect = r; e.kind = kind; e.category = category;
    auto it = catalog.images.find(image);
    if (it == catalog.images.end()) { e.kind = L::kUnknown; add(e); return; }
    const auto& info = it->second;
    e.format = info.format; e.bpp = info.bpp; e.samples = info.samples;
    e.image_w = info.width; e.image_h = info.height;
    if (!e.bpp) { e.kind = L::kUnknown; add(e); return; }
    if (info.block_w > 1 || info.block_h > 1) {
      e.rect = {r.x / int32_t(info.block_w), r.y / int32_t(info.block_h),
                (r.w + info.block_w - 1) / info.block_w,
                (r.h + info.block_h - 1) / info.block_h};
    }
    // A packed depth-stencil image has one physical byte budget. Partition
    // its storage (including padding) between aspects, never count it twice.
    if (info.format == VK_FORMAT_D24_UNORM_S8_UINT ||
        info.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
      if (aspects == VK_IMAGE_ASPECT_STENCIL_BIT) e.bpp = 1;
      else if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) --e.bpp;
    }
    if (kind == L::kRead || kind == L::kLoad) e.read = e.bytes(e.rect);
    if (kind == L::kWrite || kind == L::kClear || kind == L::kStore) e.write = e.bytes(e.rect);
    add(e);
  };
  auto attachment = [&](uint64_t view, VkImageAspectFlags aspect, L::Rect r,
                        uint32_t load, uint32_t store, uint32_t slot) {
    auto it = catalog.views.find(view);
    if (it == catalog.views.end()) { unknown(); return; }
    const auto& info = it->second;
    E e; e.kind = L::kAttachment; e.image = info.image; e.aspect = aspect;
    e.rect = r; e.format = info.format; e.bpp = info.bpp; e.samples = info.samples;
    e.image_w = info.width; e.image_h = info.height;
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) e.bpp = 1;
    else if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
             (info.format == VK_FORMAT_D24_UNORM_S8_UINT ||
              info.format == VK_FORMAT_D32_SFLOAT_S8_UINT)) --e.bpp;
    e.load_op = load; e.store_op = store; e.slot = slot;
    attachments.push_back(e); add(e);
    if (load == VK_ATTACHMENT_LOAD_OP_LOAD) {
      e.kind = L::kLoad; e.category = L::kPassLoad; e.read = e.bytes(r); add(e);
    } else if (load == VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
               load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      e.kind = load == VK_ATTACHMENT_LOAD_OP_CLEAR ? L::kClear : L::kWrite;
      // Discard is a lifetime boundary, not a memory transaction. A loadOp
      // clear happens on tile memory, so it too has no raw external write.
      add(e);
    }
  };
  for (size_t offset = 0; offset < command_stream_size_;) {
    auto notes = pass_bytes_notes_.find(offset);
    if (notes != pass_bytes_notes_.end()) {
      for (E e : notes->second) {
        if (e.kind == L::kTag) {
          copy_category = e.category;
          annotated_work = true;
        } else { add(e); drew = false; }
      }
    }
    const auto& header = *reinterpret_cast<const CommandHeader*>(command_stream_.data() + offset);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(command_stream_.data() + offset + kCommandHeaderSizeElements);
    auto array = [&](size_t size, size_t alignment) { return data + xe::align(size, alignment); };
    switch (header.command) {
      case Command::kPassHeadSlot: {
        uint32_t slot = *reinterpret_cast<const uint32_t*>(data);
        pass_head_slots_[slot]->CollectPassBytes(catalog, ledger);
      } break;
      case Command::kVkBeginRenderPass: {
        const auto& a = *reinterpret_cast<const ArgsVkBeginRenderPass*>(data);
        pass = ledger.BeginPass(); attachments.clear(); drew = false;
        auto fb = catalog.framebuffers.find(PassByteCatalog::Id(a.framebuffer));
        auto rp = catalog.render_passes.find(PassByteCatalog::Id(a.render_pass));
        if (fb == catalog.framebuffers.end() || rp == catalog.render_passes.end() ||
            fb->second.size() != rp->second.size()) { unknown(); break; }
        for (size_t i = 0; i < fb->second.size(); ++i) {
          auto view = catalog.views.find(fb->second[i]);
          if (view == catalog.views.end()) { unknown(); continue; }
          uint32_t fmt = view->second.format;
          bool depth = fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
          L::Rect r{a.render_area.offset.x, a.render_area.offset.y, a.render_area.extent.width, a.render_area.extent.height};
          const auto& ops = rp->second[i];
          attachment(fb->second[i], depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                     r, ops.load, ops.store, ops.slot);
          if (depth) attachment(fb->second[i], VK_IMAGE_ASPECT_STENCIL_BIT, r, ops.stencil_load, ops.stencil_store, ops.slot);
        }
      } break;
      case Command::kVkBeginRendering: {
        const auto& a = *reinterpret_cast<const ArgsVkBeginRendering*>(data);
        pass = ledger.BeginPass(); attachments.clear(); drew = false;
        const auto* infos = reinterpret_cast<const VkRenderingAttachmentInfo*>(array(sizeof(a), alignof(VkRenderingAttachmentInfo)));
        uint32_t count = a.color_attachment_count + a.has_depth_attachment + a.has_stencil_attachment;
        for (uint32_t i = 0; i < count; ++i) {
          if (infos[i].imageView == VK_NULL_HANDLE) continue;
          uint32_t aspect = i < a.color_attachment_count ? VK_IMAGE_ASPECT_COLOR_BIT :
              (a.has_depth_attachment && i == a.color_attachment_count ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT);
          attachment(PassByteCatalog::Id(infos[i].imageView), aspect,
                     {a.render_area.offset.x, a.render_area.offset.y, a.render_area.extent.width, a.render_area.extent.height},
                     infos[i].loadOp, infos[i].storeOp, i);
          if (infos[i].resolveImageView != VK_NULL_HANDLE) unknown();
        }
      } break;
      case Command::kVkEndRendering:
      case Command::kVkEndRenderPass:
        for (E e : attachments) {
          if (e.store_op == VK_ATTACHMENT_STORE_OP_STORE) {
            e.kind = L::kStore; e.category = L::kPassStore; e.write = e.bytes(e.rect); add(e);
          }
        }
        attachments.clear(); pass = 0;
        break;
      case Command::kVkDraw:
      case Command::kVkDrawIndexed:
        // A draw may read blending/depth data and only partially write it.
        if (!drew) for (E e : attachments) { e.kind = L::kDraw; add(e); }
        drew = true;
        break;
      case Command::kVkDispatch:
        if (!annotated_work) unknown();
        break;
      case Command::kVkCopyBuffer: {
        const auto& a = *reinterpret_cast<const ArgsVkCopyBuffer*>(data);
        const auto* rs = reinterpret_cast<const VkBufferCopy*>(array(sizeof(a), alignof(VkBufferCopy)));
        E e; e.kind = L::kRead; e.category = copy_category;
        for (uint32_t i = 0; i < a.region_count; ++i) e.read += rs[i].size;
        e.write = e.read; add(e);
      } break;
      case Command::kVkFillBuffer: {
        const auto& a = *reinterpret_cast<const ArgsVkFillBuffer*>(data);
        if (a.size == VK_WHOLE_SIZE) { unknown(); break; }
        E e; e.kind = L::kClear; e.category = L::kClearBytes; e.write = a.size; add(e);
      } break;
      case Command::kVkCopyBufferToImage:
      case Command::kVkCopyImageToBuffer: {
        bool to_image = header.command == Command::kVkCopyBufferToImage;
        uint64_t image; uint32_t count; size_t size;
        if (to_image) {
          const auto& a = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(data);
          image = PassByteCatalog::Id(a.dst_image); count = a.region_count; size = sizeof(a);
        } else {
          const auto& a = *reinterpret_cast<const ArgsVkCopyImageToBuffer*>(data);
          image = PassByteCatalog::Id(a.src_image); count = a.region_count; size = sizeof(a);
        }
        const auto* rs = reinterpret_cast<const VkBufferImageCopy*>(array(size, alignof(VkBufferImageCopy)));
        for (uint32_t i = 0; i < count; ++i) {
          const auto& r = rs[i];
          for (uint32_t z = 0; z < r.imageExtent.depth; ++z)
            for (uint32_t l = 0; l < r.imageSubresource.layerCount; ++l) {
              size_t before = ledger.events.size();
              region(image, r.imageSubresource.aspectMask, r.imageSubresource.mipLevel,
                     r.imageSubresource.baseArrayLayer + l + r.imageOffset.z + z,
                     {r.imageOffset.x, r.imageOffset.y, r.imageExtent.width, r.imageExtent.height},
                     to_image ? L::kWrite : L::kRead, L::kCopy);
              if (ledger.events.size() > before) {
                E& e = ledger.events.back();
                // The buffer side is a separate transaction, charged here
                // rather than emitting a duplicate image lifetime event.
                if (to_image) e.read = e.write; else e.write = e.read;
              }
            }
        }
      } break;
      case Command::kVkCopyImage: {
        const auto& a = *reinterpret_cast<const ArgsVkCopyImage*>(data);
        const auto* rs = reinterpret_cast<const VkImageCopy*>(array(sizeof(a), alignof(VkImageCopy)));
        for (uint32_t i = 0; i < a.region_count; ++i) {
          const auto& r = rs[i];
          for (uint32_t z = 0; z < r.extent.depth; ++z)
            for (uint32_t l = 0; l < r.srcSubresource.layerCount; ++l) {
              region(PassByteCatalog::Id(a.src_image), r.srcSubresource.aspectMask, r.srcSubresource.mipLevel,
                     r.srcSubresource.baseArrayLayer + l + r.srcOffset.z + z,
                     {r.srcOffset.x, r.srcOffset.y, r.extent.width, r.extent.height}, L::kRead, L::kCopy);
              region(PassByteCatalog::Id(a.dst_image), r.dstSubresource.aspectMask, r.dstSubresource.mipLevel,
                     r.dstSubresource.baseArrayLayer + l + r.dstOffset.z + z,
                     {r.dstOffset.x, r.dstOffset.y, r.extent.width, r.extent.height}, L::kWrite, L::kCopy);
            }
        }
      } break;
      case Command::kVkClearAttachments: {
        drew = false;
        const auto& a = *reinterpret_cast<const ArgsVkClearAttachments*>(data);
        const auto* cs = reinterpret_cast<const VkClearAttachment*>(array(sizeof(a), alignof(VkClearAttachment)));
        const auto* rs = reinterpret_cast<const VkClearRect*>(reinterpret_cast<const uint8_t*>(cs) +
            xe::align(size_t(a.attachment_count) * sizeof(*cs), alignof(VkClearRect)));
        for (uint32_t c = 0; c < a.attachment_count; ++c) {
          for (E e : attachments) {
            if (e.aspect == VK_IMAGE_ASPECT_COLOR_BIT && e.slot != cs[c].colorAttachment) continue;
            if (!(e.aspect & cs[c].aspectMask)) continue;
            for (uint32_t r = 0; r < a.rect_count; ++r) {
              E clear = e; clear.kind = L::kClear; clear.category = L::kClearBytes;
              clear.rect = L::Intersection(e.rect, {rs[r].rect.offset.x, rs[r].rect.offset.y,
                  rs[r].rect.extent.width, rs[r].rect.extent.height});
              // Attachment clears run in tile memory; payload is visible in
              // the region, the external traffic is already in pass stores.
              add(clear);
            }
          }
        }
      } break;
      case Command::kVkClearColorImage: {
        const auto& a = *reinterpret_cast<const ArgsVkClearColorImage*>(data);
        auto image = catalog.images.find(PassByteCatalog::Id(a.image));
        if (image == catalog.images.end()) { unknown(); break; }
        const auto* rs = reinterpret_cast<const VkImageSubresourceRange*>(array(sizeof(a), alignof(VkImageSubresourceRange)));
        for (uint32_t i = 0; i < a.range_count; ++i) {
          uint32_t levels = rs[i].levelCount == VK_REMAINING_MIP_LEVELS ?
              image->second.mip_levels - rs[i].baseMipLevel : rs[i].levelCount;
          uint32_t layers = rs[i].layerCount == VK_REMAINING_ARRAY_LAYERS ?
              image->second.layers - rs[i].baseArrayLayer : rs[i].layerCount;
          for (uint32_t m = 0; m < levels; ++m)
            for (uint32_t l = 0; l < layers; ++l) {
              uint32_t mip = rs[i].baseMipLevel + m;
              region(image->first, rs[i].aspectMask, mip, rs[i].baseArrayLayer + l,
                     {0, 0, std::max(1u, image->second.width >> mip), std::max(1u, image->second.height >> mip)},
                     L::kClear, L::kClearBytes);
            }
        }
      } break;
      case Command::kVkPipelineBarrier: {
        const auto& a = *reinterpret_cast<const ArgsVkPipelineBarrier*>(data);
        size_t image_offset = xe::align(sizeof(a), alignof(VkMemoryBarrier)) +
            size_t(a.memory_barrier_count) * sizeof(VkMemoryBarrier);
        image_offset = xe::align(image_offset, alignof(VkBufferMemoryBarrier)) +
            size_t(a.buffer_memory_barrier_count) * sizeof(VkBufferMemoryBarrier);
        image_offset = xe::align(image_offset, alignof(VkImageMemoryBarrier));
        const auto* barriers = reinterpret_cast<const VkImageMemoryBarrier*>(data + image_offset);
        for (uint32_t i = 0; i < a.image_memory_barrier_count; ++i) {
          const auto& b = barriers[i];
          if (b.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED) continue;
          auto image = catalog.images.find(PassByteCatalog::Id(b.image));
          if (image == catalog.images.end()) continue;
          const auto& r = b.subresourceRange;
          uint32_t levels = r.levelCount == VK_REMAINING_MIP_LEVELS ?
              image->second.mip_levels - r.baseMipLevel : r.levelCount;
          uint32_t layers = r.layerCount == VK_REMAINING_ARRAY_LAYERS ?
              image->second.layers - r.baseArrayLayer : r.layerCount;
          for (uint32_t m = 0; m < levels; ++m)
            for (uint32_t l = 0; l < layers; ++l) {
              uint32_t mip = r.baseMipLevel + m;
              region(image->first, r.aspectMask, mip, r.baseArrayLayer + l,
                     {0, 0, std::max(1u, image->second.width >> mip), std::max(1u, image->second.height >> mip)},
                     L::kWrite, L::kCopy);
              ledger.events.back().write = 0;
            }
        }
      } break;
      case Command::kVkBlitImage: {
        const auto& a = *reinterpret_cast<const ArgsVkBlitImage*>(data);
        const auto* rs = reinterpret_cast<const VkImageBlit*>(array(sizeof(a), alignof(VkImageBlit)));
        for (uint32_t i = 0; i < a.region_count; ++i) {
          for (uint32_t l = 0; l < rs[i].srcSubresource.layerCount; ++l) {
            const auto& r = rs[i];
            auto rect = [](const VkOffset3D* offsets) -> L::Rect {
              return {std::min(offsets[0].x, offsets[1].x), std::min(offsets[0].y, offsets[1].y),
                      uint32_t(std::abs(offsets[1].x - offsets[0].x)),
                      uint32_t(std::abs(offsets[1].y - offsets[0].y))};
            };
            for (int32_t z = std::min(r.srcOffsets[0].z, r.srcOffsets[1].z);
                 z < std::max(r.srcOffsets[0].z, r.srcOffsets[1].z); ++z)
              region(PassByteCatalog::Id(a.src_image), r.srcSubresource.aspectMask,
                     r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer + l + z,
                     rect(r.srcOffsets), L::kRead, L::kCopy);
            for (int32_t z = std::min(r.dstOffsets[0].z, r.dstOffsets[1].z);
                 z < std::max(r.dstOffsets[0].z, r.dstOffsets[1].z); ++z)
              region(PassByteCatalog::Id(a.dst_image), r.dstSubresource.aspectMask,
                     r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer + l + z,
                     rect(r.dstOffsets), L::kWrite, L::kCopy);
          }
        }
      } break;
      default: break;
    }
    // A tag describes exactly the next dispatch/copy; state-setting commands
    // in between do not consume it. In particular, captures remain ordered.
    if (header.command == Command::kVkDispatch || header.command == Command::kVkCopyBuffer ||
        header.command == Command::kVkDraw || header.command == Command::kVkDrawIndexed) {
      copy_category = L::kCopy; annotated_work = false;
    }
    offset += kCommandHeaderSizeElements + header.arguments_size_elements;
  }
}

DeferredCommandBuffer::CommandStats DeferredCommandBuffer::GetCommandStats()
    const {
  static_assert(size_t(Command::kVkInsertDebugUtilsLabelEXT) <
                kCommandCountSlots);
  auto count = [this](Command command) {
    return command_counts_[size_t(command)];
  };
  CommandStats stats;
  stats.draws = count(Command::kVkDraw) + count(Command::kVkDrawIndexed);
  stats.pipeline_binds =
      count(Command::kVkBindPipeline) + count(Command::kVkBindPipelineDeferred);
  stats.descriptor_set_binds = count(Command::kVkBindDescriptorSets);
  stats.push_constants = count(Command::kVkPushConstants);
  stats.viewports_scissors =
      count(Command::kVkSetViewport) + count(Command::kVkSetScissor);
  for (Command command :
       {Command::kVkSetBlendConstants, Command::kVkSetDepthBias,
        Command::kVkSetStencilCompareMask, Command::kVkSetStencilReference,
        Command::kVkSetStencilWriteMask, Command::kVkSetCullMode,
        Command::kVkSetFrontFace, Command::kVkSetPrimitiveTopology,
        Command::kVkSetPrimitiveRestartEnable, Command::kVkSetDepthTestEnable,
        Command::kVkSetDepthWriteEnable, Command::kVkSetDepthCompareOp,
        Command::kVkSetStencilTestEnable, Command::kVkSetStencilOp,
        Command::kVkSetFragmentShadingRateKHR,
        Command::kVkSetDepthClampEnableEXT, Command::kVkSetPolygonModeEXT,
        Command::kVkSetColorBlendEnableEXT,
        Command::kVkSetColorBlendEquationEXT,
        Command::kVkSetColorWriteMaskEXT}) {
    stats.other_dynamic_state += count(command);
  }
  stats.barriers = count(Command::kVkPipelineBarrier);
  stats.index_vertex_buffer_binds =
      count(Command::kVkBindIndexBuffer) + count(Command::kVkBindVertexBuffers);
  return stats;
}

void* DeferredCommandBuffer::WriteCommand(Command command,
                                          size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t offset = command_stream_size_;
  size_t new_size = offset + kCommandHeaderSizeElements + arguments_size_elements;
  if (before_reallocate_) {
    size_t storage_size = new_size;
    if (use_size_cursor_) {
      storage_size = new_size > command_stream_.size()
                         ? std::max(new_size, command_stream_.size() * 2)
                         : command_stream_.size();
    }
    if (storage_size > command_stream_.capacity()) {
      // Growth moves the recorded commands - drain any concurrent replay
      // first.
      before_reallocate_();
    }
  }
  if (use_size_cursor_) {
    // Grow geometrically and keep the arena allocated across Reset() — the
    // bytes past the cursor are dead, so resize()'s zero-fill per command is
    // pure waste. std::max ordering matters: size() * 2 is 0 right after
    // construction (the constructor only reserves).
    if (new_size > command_stream_.size()) {
      command_stream_.resize(std::max(new_size, command_stream_.size() * 2));
    }
  } else {
    command_stream_.resize(new_size);
  }
  command_stream_size_ = new_size;
  CommandHeader& header =
      *reinterpret_cast<CommandHeader*>(command_stream_.data() + offset);
  header.command = command;
  ++command_counts_[size_t(command) % kCommandCountSlots];
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  return command_stream_.data() + (offset + kCommandHeaderSizeElements);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
