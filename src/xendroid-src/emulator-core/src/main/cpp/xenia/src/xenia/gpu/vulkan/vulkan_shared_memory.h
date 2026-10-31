/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
#define XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_

#include <algorithm>
#include <array>
#include <deque>
#include <atomic>
#include <string>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, Memory& memory,
                     TraceWriter& trace_writer,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  // Pending records own an R32 source and a CP-only compute closure. CPU
  // faults request work and wait outside the once-held global region.
  bool CanPublishDepthSurface(uint32_t length = 0) const;
  bool has_depth_surface_records() const { return depth_provider_stats_.pending_bytes != 0; }
  uint32_t depth_surface_page_size_log2() const { return page_size_log2(); }
  // geometry identifies the guest bytes the resolve writes inside the range.
  // materialize(wait): records the compute writing the guest bytes into the
  // buffer; with wait, also completes the GPU work so the CPU can read them.
  void PublishDepthSurface(uint32_t start, uint32_t length, uint64_t geometry,
                           std::function<bool(bool wait)> materialize,
                           std::function<void()> discard);
  // A resolve rewriting exactly the bytes of a pending, unrequested record
  // makes its materialization dead: retire it without any GPU work.
  bool DiscardOverwrittenDepthSurface(uint32_t start, uint32_t length,
                                      uint64_t geometry);
  // A resolve about to rewrite every byte of [start, start + length) makes
  // the pending, unrequested records inside it dead too, whatever their
  // geometry. Returns how many were retired.
  uint32_t DiscardDepthSurfacesWithin(uint32_t start, uint32_t length);
  bool IsRangeValidInSurface(uint32_t start, uint32_t length) const override;
  // for_gpu: the bytes are needed by GPU work recorded next (resolves, draws,
  // uploads) - the compute goes in the stream with no wait and no copy to
  // guest RAM; the record stays pending for the CPU, which still waits.
  void MaterializeDepthSurfaces(uint32_t start, uint32_t length,
                                bool for_gpu = false);
  void PrepareForCpuAccess(uint32_t start, uint32_t length) override;
  void ServiceDepthRequests();
  bool depth_materializing() const { return depth_materializing_; }
  struct DepthProviderStats {
    uint64_t published = 0, published_bytes = 0;
    uint64_t materializations = 0, materialized_bytes = 0, cpu_accesses = 0;
    uint64_t discarded = 0, discarded_bytes = 0;
    uint64_t gpu_materializations = 0;
    uint64_t pending_bytes = 0;
  };
  DepthProviderStats TakeDepthProviderStats();
  // log_depth_materialize_callers: " count:pc/pc/..." per distinct stack.
  std::string TakeMaterializeCallers();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});
  Usage usage() const { return last_usage_; }
  // vulkan_pass_head_overlap: compute writes reach the read stages except the
  // vertex ones (vertex input, guest vertex shaders), so the next pass's
  // geometry needn't wait for a resolve it doesn't read. Called by every draw
  // with the ranges its vertex stages read (vertex fetch, guest DMA indices):
  // if one overlaps such a write, the vertex stages wait for it here.
  void SettleVertexReads(const std::pair<uint32_t, uint32_t>* ranges,
                         size_t range_count);
  bool vertex_read_debt() const { return vertex_debt_; }
  // Pixel shaders don't read the shared memory unless they fetch vertices
  // (memory export and in-pass resolves declare kGuestDrawReadWrite), so the
  // reads stop waiting for earlier writes in the fragment stage; a draw whose
  // pixel shader fetches vertices calls this first.
  void SettleFragmentReads();
  // The debts of the main stream across a resolve recorded while its pass is
  // open but executed at the pass end, followed by a full barrier: the tail's
  // own writes need no debt, and its barriers, running after the pass, settle
  // none of the main stream's.
  struct ReadDebt {
    bool vertex = false;
    bool fragment = false;
    bool vertex_all = false;
    std::vector<std::pair<uint32_t, uint32_t>> vertex_ranges;
    uint64_t vertex_start = UINT64_MAX;
    uint64_t vertex_end = 0;
  };
  ReadDebt SaveReadDebt() const {
    return {vertex_debt_,         fragment_debt_,     vertex_debt_all_,
            vertex_debt_ranges_, vertex_debt_start_, vertex_debt_end_};
  }
  void RestoreReadDebt(ReadDebt&& debt) {
    vertex_debt_ = debt.vertex;
    fragment_debt_ = debt.fragment;
    vertex_debt_all_ = debt.vertex_all;
    vertex_debt_ranges_ = std::move(debt.vertex_ranges);
    vertex_debt_start_ = debt.vertex_start;
    vertex_debt_end_ = debt.vertex_end;
  }

  struct BufferSlice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize buffer_offset = 0;
    uint32_t guest_offset = 0;
    uint32_t length = 0;
  };

  static constexpr uint32_t kMaximumBufferSlices = 16;
  struct BufferSliceList {
    std::array<BufferSlice, kMaximumBufferSlices> slices{};
    uint32_t count = 0;

    bool empty() const { return count == 0; }
    const BufferSlice* begin() const { return slices.data(); }
    const BufferSlice* end() const { return slices.data() + count; }
  };

  // Returns the Vulkan buffer spans covering a guest range. This creates
  // backing only, not valid guest contents: callers
  // must use RequestRange before CPU-backed reads and must call this before
  // recording any GPU write. An invalid range or allocation failure returns
  // an empty list, in which case the operation must be abandoned.
  BufferSliceList GetBufferSlices(uint32_t guest_address, uint32_t length);
  uint32_t GetShaderBufferCount() const;

  // Legacy single-buffer accessor.
  VkBuffer buffer() const { return buffer_; }
  // In-pass vertex fetch ring (vulkan_vfetch_inpass_ring_mb) in buffer_ past
  // the guest memory, 0 if unavailable.
  uint32_t vfetch_ring_size() const { return vfetch_ring_size_; }
  // Whether uploading the not yet valid pages of the byte range now would end
  // the open render pass: a command of the pass read them, so the copy can be
  // neither hoisted nor recorded at the head of the pass.
  // span_start_out and span_length_out (bytes, page-aligned) receive the span
  // from the first to the last not yet valid page.
  bool RangeUploadWouldBreakPass(uint32_t start, uint32_t length,
                                 uint32_t& span_start_out,
                                 uint32_t& span_length_out);
  // Copies the current guest bytes of the range into the ring for commands of
  // the current submission. Returns their byte offset in buffer_, or
  // UINT32_MAX if the ring has no space that the GPU no longer reads.
  uint32_t CopyRangeToVfetchRing(uint32_t start, uint32_t length);
  // Diagnostics (log_render_pass_breaks): why RangeUploadWouldBreakPass said
  // no for ranges with invalid pages, and Use() barriers pushed inside a
  // render pass by (previous usage, new usage, committing a write range).
  struct PassDiagStats {
    uint64_t window_overflow = 0;
    uint64_t window_gpu_written = 0;
    uint64_t window_hoistable = 0;
    uint64_t window_pass_head = 0;
    uint64_t window_break = 0;
    uint64_t use_in_pass[4][4][2] = {};
  };
  PassDiagStats& pass_diag_stats() { return pass_diag_stats_; }

  // Notes a range written by an in-pass draw without inserting a barrier at
  // the write point - ordering happens at the next Use().
  void MarkInPassWrite(std::pair<uint32_t, uint32_t> written_range) {
    if (last_written_range_.second) {
      uint32_t start =
          std::min(last_written_range_.first, written_range.first);
      uint32_t end =
          std::max(last_written_range_.first + last_written_range_.second,
                   written_range.first + written_range.second);
      last_written_range_ = std::make_pair(start, end - start);
    } else {
      last_written_range_ = written_range;
    }
  }

  // Whether direct readback is available.
  bool IsHostMapped() const;
  // Copies `length` bytes of guest physical memory at `guest_address` into
  // `dest`. Invalidates the CPU cache first for mapped non-coherent spans.
  // The caller is responsible for GPU/CPU ordering; this is the approximate
  // (fast-class) readback path. Returns false if a resident span is not mapped
  // or the range is out of bounds.
  // gpu_ordered: the caller orders the read after the GPU itself (resolve
  // readback of retired submissions) - pending surfaces are only recorded into
  // the GPU stream, their pages stay protected until a real CPU access.
  bool ReadHostMapped(uint32_t guest_address, uint32_t length,
                      void* dest, bool gpu_ordered = false);
  // Save states capture guest RAM. Pages whose newest data a resolve wrote
  // exist only in this buffer (without zero-copy): copy them into guest RAM
  // first. Must be called with the GPU idle. Returns false if a GPU-written
  // page could not be read.
  bool WriteBackGpuWrittenToGuest(uint32_t& pages_written);

  // A host-imported (guest RAM) copy of the buffer, or VK_NULL_HANDLE if
  // unavailable. Bound instead of buffer() for memexport-touching draws so
  // their output is coherent with the CPU (no clobber) - it aliases guest RAM.
  VkBuffer host_buffer() const { return host_buffer_; }

  // True when the buffer aliases guest RAM directly (EXT_external_memory_host).
  bool is_zero_copy() const { return zero_copy_; }

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool OnRangeRequested(uint32_t start, uint32_t length) override;
  void OnCpuRangeAccess(uint32_t start, uint32_t length) override;

  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_ranges) override;

 private:
  struct DepthSurfaceSnapshot {
    uint32_t start, length;
    uint64_t geometry;
    std::function<bool(bool wait)> materialize;
    std::function<void()> discard;
    bool pending = true;
    // The buffer holds the bytes (recorded in the GPU stream); guest RAM
    // doesn't yet.
    bool gpu_materialized = false;
    bool requested = false;
  };
  // Only the CP retires records. CPU waiters never hold a vector reference
  // while unlocked, and rescan after every wake before permitting access.
  std::vector<DepthSurfaceSnapshot> depth_surfaces_;
  void* depth_read_callback_ = nullptr;
  DepthProviderStats depth_provider_stats_;
  struct MaterializeCaller {
    std::array<uint32_t, 6> stack;
    uint32_t count;
    // First occurrence: the record materialized and the range that hit it.
    uint32_t record_start, record_length, request_start, request_length;
  };
  using MaterializeCallers = std::array<MaterializeCaller, 6>;
  MaterializeCallers materialize_callers_ = {};
  void NoteMaterializeCaller(uint32_t record_start, uint32_t record_length,
                             uint32_t request_start, uint32_t request_length);
  std::atomic<bool> depth_request_pending_{false};
  std::atomic<uint32_t> depth_cpu_waiters_{0};
  bool depth_materializing_ = false;
  // CP-only union of the records that may be pending (only the CP publishes
  // and retires them): RequestRange outside it skips the locked scan. Grows on
  // publish, shrinks when the publish drops retired records.
  uint32_t depth_pending_lo_ = UINT32_MAX;
  uint32_t depth_pending_hi_ = 0;
  void AwaitDepthSurfaces(global_unique_lock_type& lock,
                          uint32_t start, uint32_t length);
  static void DepthReadCallback(void* context, global_unique_lock_type& lock,
                                uint32_t start, uint32_t length);
  uint32_t GetDenseShaderBufferCountLog2() const;

  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Creates a non-sparse buffer bound to imported guest RAM
  // (VK_EXT_external_memory_host). Returns true and fills out_buffer/out_memory
  // on success; false (nothing to clean up) otherwise.
  bool CreateImportedGuestRamBuffer(VkBuffer& out_buffer,
                                    VkDeviceMemory& out_memory);
  // Attempts to alias guest RAM as the whole buffer (full zero-copy). Returns
  // true on success (buffer_/buffer_memory_ populated, zero_copy_ set); false
  // to fall back to the normal device-local path.
  bool TryInitializeZeroCopy();
  // Attempts to create host_buffer_ for the hybrid two-buffer path. No-op on
  // failure (host_buffer_ stays null).
  void TryInitializeHostBuffer();

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;
  // Buffer memory is imported guest RAM - no uploads or readback copies needed.
  bool zero_copy_ = false;
  uint64_t pass_head_upload_count_ = 0;

  // Second buffer aliasing guest RAM, bound for memexport-touching draws.
  VkBuffer host_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory host_buffer_memory_ = VK_NULL_HANDLE;

  // Persistent host mapping of the dense buffer for direct CPU readback
  // (readback_resolve=uma). Null unless the buffer is a single host-visible
  // allocation.
  uint8_t* host_mapped_data_ = nullptr;
  PassDiagStats pass_diag_stats_;
  uint32_t vfetch_ring_size_ = 0;
  uint32_t vfetch_ring_head_ = 0;
  VkDeviceSize vfetch_ring_memory_size_ = 0;
  struct VfetchRingAllocation {
    uint32_t start;
    uint32_t end;
    uint64_t submission;
  };
  std::deque<VfetchRingAllocation> vfetch_ring_allocations_;
  uint64_t vfetch_ring_copies_ = 0;
  uint64_t vfetch_ring_bytes_ = 0;
  uint64_t vfetch_ring_full_ = 0;
  bool host_mapped_coherent_ = false;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;
  // Compute-written [start, end) ranges not yet visible to the vertex stages
  // (see SettleVertexReads); vertex_debt_all_ when there were too many.
  bool vertex_debt_ = false;
  bool fragment_debt_ = false;
  bool vertex_debt_all_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> vertex_debt_ranges_;
  // Bounds of vertex_debt_ranges_, to skip the per-range test for most draws.
  uint64_t vertex_debt_start_ = UINT64_MAX;
  uint64_t vertex_debt_end_ = 0;
  void ClearVertexDebt() {
    vertex_debt_ = false;
    vertex_debt_all_ = false;
    vertex_debt_ranges_.clear();
    vertex_debt_start_ = UINT64_MAX;
    vertex_debt_end_ = 0;
  }

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;
  struct UploadChunk {
    uint32_t page_first;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
    uint8_t* mapping;
    BufferSliceList destination;
  };
  std::vector<UploadChunk> upload_chunks_;
  // Only copies already proven safe at submission head are deferred. CPU
  // snapshots and watches happen at RequestRange time, never at flush time.
  struct SetupUploadBatch {
    VkBuffer source;
    VkBuffer destination;
    std::vector<VkBufferCopy> regions;
  };
  std::vector<SetupUploadBatch> setup_upload_batches_;
  size_t setup_upload_batch_count_ = 0;
  void FlushSetupUploads();
  // Upload ranges left after the memexport ones are refreshed on the GPU.
  std::vector<std::pair<uint32_t, uint32_t>> cpu_upload_ranges_;

  // Created temporarily, only for downloading.
  VkBuffer trace_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory trace_download_buffer_memory_ = VK_NULL_HANDLE;
  void ResetTraceDownload();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
