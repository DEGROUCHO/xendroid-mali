/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_PRIMITIVE_PROCESSOR_H_
#define XENIA_GPU_VULKAN_VULKAN_PRIMITIVE_PROCESSOR_H_

#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  VulkanPrimitiveProcessor(const RegisterFile& register_file, Memory& memory,
                           TraceWriter& trace_writer,
                           SharedMemory& shared_memory,
                           VulkanCommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory),
        command_processor_(command_processor) {}
  ~VulkanPrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache();

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();
  void EndSubmission();
  void EndFrame();

  std::pair<VkBuffer, VkDeviceSize> GetBuiltinIndexBuffer(size_t handle) const {
    assert_not_null(builtin_index_buffer_);
    return std::make_pair(
        builtin_index_buffer_,
        VkDeviceSize(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<VkBuffer, VkDeviceSize> GetConvertedIndexBuffer(
      size_t handle) const {
    if (handle & kPreswapHandleBit) {
      const auto& allocation =
          preswap_entries_[handle & ~kPreswapHandleBit].allocation;
      return {allocation.buffer, allocation.offset};
    }
    return frame_index_buffers_[handle];
  }

 protected:
  bool InitializeBuiltinIndexBuffer(
      size_t size_bytes, std::function<void(void*)> fill_callback) override;

  bool GetPreswappedIndexBuffer(uint32_t guest_base, uint32_t count,
                                size_t& handle_out) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(
      xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
      uint32_t coalignment_original_address,
      size_t& backend_handle_out) override;

 private:
  VulkanCommandProcessor& command_processor_;

  VkDeviceSize builtin_index_buffer_size_ = 0;
  VkBuffer builtin_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_memory_ = VK_NULL_HANDLE;
  // Temporary buffer copied in the beginning of the first submission for
  // uploading to builtin_index_buffer_, destroyed when the submission when it
  // was uploaded is completed.
  VkBuffer builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  // UINT64_MAX means not uploaded yet and needs uploading in the first
  // submission (if the upload buffer exists at all).
  uint64_t builtin_index_buffer_upload_submission_ = UINT64_MAX;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> frame_index_buffer_pool_;
  static constexpr size_t kPreswapHandleBit = size_t(1)
                                              << (sizeof(size_t) * 8 - 1);
  // 4-way set associative, least recently used way replaced: a game can use
  // several thousand index buffers per frame, and a direct-mapped table of
  // 2048 lost ~90% of its misses to collisions (Black Ops: 1560 per frame).
  static constexpr uint32_t kPreswapSetShift = 21;  // 2048 sets.
  static constexpr uint32_t kPreswapWays = 4;
  static constexpr uint32_t kPreswapEntryCount =
      (uint32_t(1) << (32 - kPreswapSetShift)) * kPreswapWays;
  struct PreswapEntry {
    uint32_t base = 0, count = 0;
    uint64_t epoch = UINT64_MAX;
    uint64_t last_use = 0;
    ui::vulkan::VulkanUploadBufferPool::RetainedAllocation allocation;
    // preswap_volatile_validate: a range found stale again and again (rewritten
    // by the CPU, or only sharing a 64 KB epoch region with written data) is
    // validated by comparing its guest bytes with this copy instead of being
    // watched: no lock, no mprotect, no guest write fault.
    uint8_t stale_streak = 0;
    bool validate_by_compare = false;
    uint8_t compare_uses = 0;
    std::vector<uint8_t> guest_copy;
  };
  uint64_t preswap_use_counter_ = 0;
  // Address/count identify bytes only; reset enable doesn't change conversion.
  std::array<PreswapEntry, kPreswapEntryCount> preswap_entries_{};
  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool>
      preswap_index_buffer_pool_;
  void ReleasePreswapEntries();
  // Indexed by the backend handles.
  std::deque<std::pair<VkBuffer, VkDeviceSize>> frame_index_buffers_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_PRIMITIVE_PROCESSOR_H_
