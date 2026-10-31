/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHARED_MEMORY_H_
#define XENIA_GPU_SHARED_MEMORY_H_

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/shared_memory_coherency.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
class SharedMemoryWorkingSet;

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;
  // CPU-side vertex interpretation is only safe for pages never written by
  // the GPU. Keep this history at fixed 4 KiB granularity, independent of the
  // host OS page size, so it stays small and conservatively catches subpage
  // writes on platforms with larger system pages.
  static constexpr uint32_t kGpuWrittenHistoryPageSizeLog2 = 12;
  static constexpr uint32_t kGpuWrittenHistoryPageCount =
      kBufferSize >> kGpuWrittenHistoryPageSizeLog2;
  static constexpr uint32_t kGpuWrittenHistoryWordCount =
      (kGpuWrittenHistoryPageCount + 63) / 64;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  virtual void SetSystemPageBlocksValidWithGpuDataWritten();

  void InvalidateAllPages();

  // Guest memory access for diagnostics outside the shared memory classes.
  Memory& guest_memory() const { return memory_; }

  // Call when a new GPU submission starts recording; resets the tracking of
  // pages invalidated while the submission was open.
  void OnGpuSubmissionOpened();

  typedef void (*GlobalWatchCallback)(
      const global_unique_lock_type& global_lock, void* context,
      uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback,
                                        void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const global_unique_lock_type& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length,
                               WatchCallback callback, void* callback_context,
                               void* callback_data, uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable.
  // Read-only integration contract for draw input caches. Epoch is advanced
  // after CPU invalidation; batch covers pages intentionally left unwatched;
  // submission covers usage resets. Pass/pending usage is declared on hits too.
  struct DrawInputGeneration {
    uint64_t epoch;
    uint32_t batch, submission;
    bool reusable;
  };
  DrawInputGeneration draw_input_generation() const {
    return {request_epochs_.Global(), unwatch_verify_serial_,
            page_reads_generation_, declared_working_set_ == nullptr};
  }
  // For immutable host copies of CPU-read guest data. Establish a write watch
  // before reading, then publish only if the epoch is unchanged after copying.
  // UINT64_MAX means watches are unavailable and the copy cannot be cached.
  uint64_t WatchCPUReadRange(uint32_t start, uint32_t length);
  uint64_t CPUReadRangeEpoch(uint32_t start, uint32_t length) const {
    return memory_invalidation_callback_handle_
               ? request_epochs_.Load(start, length)
               : UINT64_MAX;
  }
  bool RequestRange(uint32_t start, uint32_t length);
  bool RequestRangeImpl(uint32_t start, uint32_t length);
  // CP thread: snapshot all inputs of one operation before recording it.
  // Byte ranges may overlap or be unsorted; invalid pages are uploaded once,
  // in address order. Does not defer CPU reads to submission end. Callers must
  // abandon the operation on false, just as with RequestRange.
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* byte_ranges,
                     uint32_t count);
  void LogDeclaredWorkingSetFrame();
  // shared_memory_draw_input_range_epochs: per-draw input caches
  // (DrawMemoryRange) key on the epochs of their own 64 KiB regions instead
  // of the global epoch, which any invalidation anywhere advances.
  bool draw_input_range_epochs_ = false;
  uint64_t DrawInputEpoch(uint32_t start, uint32_t length) const {
    return draw_input_range_epochs_ ? request_epochs_.Load(start, length)
                                    : request_epochs_.Global();
  }
  // Dynamic page unwatching (shared_memory_unwatch_dynamic_pages): pages the
  // CPU rewrites frame after frame stay writable - no access violation watch,
  // so no mprotect after every upload and no guest page fault before every
  // rewrite - and are checked against their upload hash instead when
  // requested, once per primary buffer batch (everything a batch's commands
  // read was written before the guest submitted it).
  bool unwatch_enabled_ = false;
  void BeginCommandBatch() {
    if (++unwatch_verify_serial_ == 0) {
      std::fill(unwatch_verified_serial_.begin(),
                unwatch_verified_serial_.end(), 0);
      unwatch_verify_serial_ = 1;
    }
  }
  // Accesses requested while a draw/resolve is being prepared must survive
  // every pass begin/reopen until that operation has been recorded. CP thread.
  class PendingAccessScope {
   public:
    explicit PendingAccessScope(SharedMemory* shared_memory);
    ~PendingAccessScope();
    PendingAccessScope(const PendingAccessScope&) = delete;
    PendingAccessScope& operator=(const PendingAccessScope&) = delete;

   private:
    SharedMemory* shared_memory_;
  };
  // Starts a new pass, retaining accesses of the operation being prepared.
  void OnRenderPassBegin();
  // Whether a command of the current render pass used (read or wrote) any page
  // of the byte ranges (start, length) - work reading them may then not be
  // moved before the pass.
  bool AnyByteRangeUsedInCurrentPass(
      const std::pair<uint32_t, uint32_t>* byte_ranges, uint32_t count) const {
    std::pair<uint32_t, uint32_t> page_ranges[4];
    assert_true(count <= 4);
    uint32_t page_range_count = 0;
    for (uint32_t i = 0; i < count && i < 4; ++i) {
      if (!byte_ranges[i].second) {
        continue;
      }
      uint32_t page_first = byte_ranges[i].first >> page_size_log2_;
      uint32_t page_last =
          (byte_ranges[i].first + byte_ranges[i].second - 1) >> page_size_log2_;
      page_ranges[page_range_count++] =
          std::make_pair(page_first, page_last - page_first + 1);
    }
    return page_range_count &&
           AnyPageUsedInCurrentPass(page_ranges, page_range_count);
  }
  // Whether the buffer contents of any page of the byte ranges changed in the
  // current render pass (unlike AnyByteRangeUsedInCurrentPass, reads don't
  // count) - a load reading them sees the same data at the head of the pass.
  bool AnyByteRangeWrittenInCurrentPass(
      const std::pair<uint32_t, uint32_t>* byte_ranges, uint32_t count) const;
  // CP thread, once per frame: updates per-page rewrite scores and re-watches
  // the unwatched pages that stopped changing (all of them if disabled).
  void OnFrameEndUnwatch();
  uint64_t stat_unwatch_hashes_ = 0;
  uint64_t stat_unwatch_invalidations_ = 0;
  uint64_t stat_unwatch_rewatches_ = 0;
  uint32_t unwatched_page_count_ = 0;
  void VerifyUnwatchedPages(uint32_t page_first, uint32_t page_last);
  void RewatchPages(uint32_t block, uint64_t mask);
  bool UnwatchedPageMatchesUpload(uint32_t page);
  void EnableWatchesExceptDynamic(uint32_t page_first, uint32_t page_last);
  std::vector<uint64_t> unwatched_pages_;
  std::vector<uint64_t> texture_watched_pages_;
  std::vector<uint64_t> dirty_this_frame_;
  // Zero-copy backends: unwatched pages need no upload-hash verification (the
  // GPU reads guest RAM itself; the backend keeps it coherent).
  bool unwatch_skip_verify_ = false;
  // Zero-copy backends: pages the CPU may have written since the backend last
  // cache-cleaned them (set on every CPU write invalidation; empty if unused).
  std::vector<uint64_t> cpu_written_since_clean_;
  std::vector<uint64_t> dirty_score_nonzero_;
  std::vector<uint64_t> unwatch_candidates_;
  uint64_t stat_cpu_read_uncached_ = 0;
  std::vector<uint8_t> dirty_score_;
  std::vector<uint32_t> unwatch_verified_serial_;
  uint32_t unwatch_verify_serial_ = 1;
  uint32_t unwatch_report_frames_ = 0;
  uint32_t unwatch_decay_counter_ = 0;
  uint64_t stat_protect_calls_ = 0;
  uint64_t stat_protect_pages_ = 0;
  uint64_t stat_fault_invalidations_ = 0;
  // CP-owned diagnostics for the optional short critical sections.
  mutable uint64_t stat_invalidation_snapshots_ = 0;
  mutable uint64_t stat_invalidation_snapshot_words_ = 0;
  uint64_t stat_frame_dirty_pages_ = 0;
  // Diagnostic counters (log_cp_frame_breakdown), requesting thread only.
  uint64_t stat_requests_ = 0;
  uint64_t stat_request_slow_ = 0;
  uint64_t stat_request_uploads_ = 0;
  uint64_t stat_request_ns_ = 0;
  uint64_t stat_mark_hashes_ = 0;
  uint64_t stat_mark_hash_bytes_ = 0;
  // Records that a GPU command recorded in the current submission accesses the
  // range (done by RequestRange; needed where a range is read without it).
  void MarkRangeUsedInSubmission(uint32_t start, uint32_t length);
  // Returns whether every page in the range is currently valid in the host GPU
  // memory copy. Hold the global critical region if relying on this for state
  // transitions such as watch installation.
  bool IsRangeValid(uint32_t start, uint32_t length) const;
  // A texture filled independently may be current while its buffer pages are
  // deliberately stale. General buffer consumers must still RequestRange.
  virtual bool IsRangeValidInSurface(uint32_t start, uint32_t length) const {
    return false;
  }
  // Host physical aliases bypass page faults (trace/hash and explicit reads).
  virtual void PrepareForCpuAccess(uint32_t start, uint32_t length) {
    OnCpuRangeAccess(start, length);
  }
  // Returns whether every page in the range is valid AND was last written by
  // the GPU (not the CPU). Necessary, not sufficient, for treating a resolve
  // destination as still GPU-authoritative: page-granular, so it over-claims
  // on sub-page tails, and sticky across GPU writes so it cannot order one
  // resolve against another.
  bool IsRangeGpuWritten(uint32_t start, uint32_t length) const;
  // Of the GPU-written host page holding address, the byte ranges the GPU
  // wrote, as absolute [first, end) pairs (the whole page unless GPU writes
  // covered only parts of it); returns their count, at most
  // kGpuWrittenPartialRanges. The rest of a partly written page belongs to the
  // CPU, which may have written it after the pre-write upload, so a copy to
  // guest RAM takes only these bytes.
  static constexpr uint32_t kGpuWrittenPartialRanges = 4;
  uint32_t GpuWrittenPageRanges(
      uint32_t address, std::pair<uint32_t, uint32_t>* ranges_out) const;
  // Whether any 4 KiB page intersecting the range has ever been written by
  // the GPU during this SharedMemory lifetime. The history is monotonic and
  // is not cleared when CPU writes invalidate or upload a page. Returns true
  // (fail closed) for an invalid range or if the small history allocation was
  // unavailable. If provided, history_available distinguishes the latter.
  bool IsRangeEverGpuWritten(uint32_t start, uint32_t length,
                             bool* history_available = nullptr) const;

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(
      uint32_t physical_address_start, uint32_t length, bool exact_range);

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  // written_to_buffer false means the GPU wrote guest RAM directly (memexport
  // routed to the host-imported buffer), leaving the shared memory buffer stale
  // for this range rather than up to date.
  virtual void RangeWrittenByGpu(uint32_t start, uint32_t length,
                         bool written_to_buffer = true);

 protected:
  SharedMemory(Memory& memory);
  // Byte ranges whose newest contents the GPU wrote (valid_and_gpu_written).
  std::vector<std::pair<uint32_t, uint32_t>> GetGpuWrittenRanges() const;
  virtual bool OnRangeRequested(uint32_t start, uint32_t length) { return true; }
  virtual void OnCpuRangeAccess(uint32_t start, uint32_t length) {}
  // After normal GPU-write publication, keep ownership/watches but withdraw
  // buffer validity. Only a synchronous provider can make it valid again.
  void MakeRangeValidInSurfaceOnly(uint32_t start, uint32_t length);
  // Invalidates a page-aligned range so it is re-uploaded from guest RAM on the
  // next request, unless it holds GPU-written or unreadable pages whose only
  // copy is on the GPU. Returns whether the range was invalidated.
  bool InvalidateRangeIfReuploadable(uint32_t start, uint32_t length);
  // Under the global critical region: use the existing mprotect invalidation
  // state, including initial never-uploaded pages, for selective AHB CVAC.
  bool IsPageCpuDirty(uint32_t page) const;
  // Valid-page bits of one 64-page block.
  uint64_t ValidPagesBlock(uint32_t block) const {
    return system_page_flags_valid_[block];
  }
  // Call in implementation-specific initialization.
  bool InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Whether any page in the ranges lost validity since the current GPU
  // submission opened. Pages that were never invalidated while the submission
  // was recording can't have been legitimately read by an already-recorded
  // command, so their upload may be reordered to the submission start.
  bool AnyPageInvalidatedSinceSubmissionOpen(
      const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const;
  // Whether the GPU may access any page in the ranges in the current
  // submission: requested by a recorded command, or written by the GPU. An
  // upload of pages that are not can be moved to the submission start even if
  // the CPU changed them while it was recording - nothing recorded reads them.
  bool AnyPageUsedSinceSubmissionOpen(
      const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const;
  // Pages used (as by AnyPageUsedSinceSubmissionOpen) since the current render
  // pass began - an upload of pages that aren't may be recorded at the head of
  // the pass instead of ending it.
  bool AnyPageUsedInCurrentPass(
      const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const;
  // The (first page, page count) ranges of the not yet valid pages of the byte
  // range: their number, or UINT32_MAX if there are more than max_ranges.
  // cpu_sourced_out is false if any page of the range is valid with contents
  // the guest memory doesn't hold (GPU-written or unreadable).
  uint32_t GetInvalidPageRanges(uint32_t start, uint32_t length,
                                std::pair<uint32_t, uint32_t>* ranges_out,
                                uint32_t max_ranges, bool& cpu_sourced_out);
  // Uploads recorded in place (not hoisted): later uploads of these pages in
  // the submission must stay after them.
  // in_pass_body: recorded in the pass body (contents change mid-pass), not
  // at its head before everything the pass records.
  void MarkPagesUsedHard(const std::pair<uint32_t, uint32_t>* page_ranges,
                         uint32_t count, bool in_pass_body = true);
  // Valid pages that can't be read by the CPU (decommitted, upload skipped).
  void MarkPagesValidUnreadable(uint32_t page_first, uint32_t page_end);
  // Records the hashes of pages just uploaded from `source` (guest memory).
  void SetPageUploadHashes(uint32_t page_first, uint32_t page_count,
                           const uint8_t* source);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::pair<uint32_t, uint32_t>* upload_page_ranges,
      uint32_t num_upload_ranges) = 0;

  const std::vector<std::pair<uint32_t, uint32_t>>& trace_download_ranges() {
    return trace_download_ranges_;
  }
  uint32_t trace_download_page_count() const {
    return trace_download_page_count_;
  }
  // Fills trace_download_ranges() and trace_download_page_count() with
  // GPU-written ranges that need to be downloaded, and also invalidates
  // non-GPU-written ranges so only the needed data - not the all the collected
  // data - will be written in the trace. trace_download_page_count() will be 0
  // if nothing to download.
  void PrepareForTraceDownload();
  // Release memory used for trace download ranges, to be called after
  // downloading or in cases when download is dropped.
  void ReleaseTraceDownloadRanges();

 private:
  void MarkRangeEverGpuWritten(uint32_t start, uint32_t length);

  Memory& memory_;
  std::unique_ptr<SharedMemoryWorkingSet> declared_working_set_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;
  // CP-owned scratch; reserved once, never used by a fault callback.
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  static constexpr xe::global_critical_region global_critical_region_{};

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  struct SystemPageFlagsBlock {
    // Whether each page is up to date in the GPU buffer.
    uint64_t valid;
    // Subset of valid pages - whether each page in the GPU buffer contains data
    // that was written on the GPU, thus should not be invalidated spuriously.
    uint64_t valid_and_gpu_written;
  };

  // chrispy: todo, systempageflagsblock should be 3 different arrays
  //  Flags for each 64 system pages, interleaved as blocks, so bit scan can be
  //  used to quickly extract ranges.
  // std::vector<SystemPageFlagsBlock> system_page_flags_;

  uint64_t* system_page_flags_valid_ = nullptr;
  uint64_t* system_page_flags_valid_and_gpu_written_ = nullptr;
  // GPU-written pages the GPU wrote only part of: page -> disjoint byte
  // ranges [first, end) within the page. Read only while the page's
  // valid_and_gpu_written bit is set.
  struct GpuWrittenPartialPage {
    uint32_t count = 0;
    std::pair<uint32_t, uint32_t> ranges[kGpuWrittenPartialRanges];
  };
  std::unordered_map<uint32_t, GpuWrittenPartialPage>
      gpu_written_partial_pages_;
  // Pages invalidated since the current GPU submission opened - cleared by
  // OnGpuSubmissionOpened.
  uint64_t* system_page_flags_invalidated_in_submission_ = nullptr;
  // Monotonic 4 KiB-page history used only to validate CPU vertex reads. It is
  // deliberately separate from current GPU-authority bits, which CPU faults
  // clear before the faulting store is retried.
  uint64_t* gpu_written_history_ = nullptr;
  // Preallocated on initialization, only used by the requesting CP thread.
  // Never allocate scratch storage in a memory invalidation callback.
  mutable std::vector<uint64_t> submission_invalidation_snapshot_;
  std::vector<uint64_t> frame_dirty_snapshot_;
  // Pages requested or GPU-written since the current GPU submission opened.
  // Only accessed by the GPU thread.
  std::vector<uint64_t> system_page_flags_used_in_submission_;
  // Subset used since the current render pass began; blocks to clear listed
  // in used_in_pass_blocks_ (all of them when used_in_pass_all_).
  std::vector<uint64_t> system_page_flags_used_in_pass_;
  std::vector<uint32_t> used_in_pass_blocks_;
  bool used_in_pass_all_ = true;
  // Separate from the pass union: clearing pending accesses on scope exit must
  // never erase accesses of draws already recorded in the open pass.
  std::vector<uint64_t> system_page_flags_pending_access_;
  std::vector<uint32_t> pending_access_blocks_;
  bool pending_access_scope_active_ = false;
  // Pages whose buffer contents changed inside the current pass (uploads
  // recorded in place, GPU writes): work reading them can't move to the head.
  std::vector<uint64_t> system_page_flags_written_in_pass_;
  std::vector<uint32_t> written_in_pass_blocks_;
  bool written_in_pass_all_ = true;
  void MarkWrittenInPass(uint32_t block, uint64_t mask) {
    if (!system_page_flags_written_in_pass_[block]) {
      written_in_pass_blocks_.push_back(block);
    }
    system_page_flags_written_in_pass_[block] |= mask;
  }
  void MarkUsedInPass(uint32_t block, uint64_t mask) {
    if (pending_access_scope_active_) {
      if (!system_page_flags_pending_access_[block]) {
        pending_access_blocks_.push_back(block);
      }
      system_page_flags_pending_access_[block] |= mask;
    }
    if (!system_page_flags_used_in_pass_[block]) {
      used_in_pass_blocks_.push_back(block);
    }
    system_page_flags_used_in_pass_[block] |= mask;
  }
  // Used pages whose reads aren't tracked by byte range (not dynamic,
  // GPU-written, or too many ranges) - never hoisted over.
  std::vector<uint64_t> system_page_flags_used_hard_in_submission_;
  // Pages the CPU has invalidated at least once - worth tracking reads of by
  // byte range. Set under the global lock, read by the GPU thread (relaxed).
  std::vector<uint64_t> system_page_flags_dynamic_;
  // Valid pages that were skipped as decommitted and can't be read.
  std::vector<uint64_t> system_page_flags_valid_unreadable_;
  struct PageReads {
    static constexpr uint32_t kMaxRanges = 4;
    // Valid in the submission with this generation only.
    uint32_t generation = 0;
    uint32_t count = 0;
    uint16_t begin[kMaxRanges];
    uint16_t end[kMaxRanges];
    uint64_t hash[kMaxRanges];
  };
  // Byte ranges read from partially read dynamic pages, by page.
  std::vector<PageReads> page_reads_;
  // Bumped (release) after any validity flag is cleared.
  coherency::RangeEpochs request_epochs_;
  // Lazy cached accesses already included in this pass, retained in O(1)
  // while preparing an operation. Materialized only if a pass is reopened.
  uint64_t pass_serial_ = 1;
  uint64_t pending_access_serial_ = 1;
  std::vector<uint64_t> requested_pages_;
  std::vector<uint32_t> requested_page_blocks_;
  std::vector<std::pair<uint32_t, uint32_t>> batch_upload_ranges_;
  // Fully read dynamic pages whose content is known from their upload hash.
  std::vector<uint64_t> system_page_flags_used_full_in_submission_;
  // Hash of each page's last CPU upload, valid while the bit is set.
  std::vector<uint64_t> page_upload_hash_;
  std::vector<uint64_t> page_upload_hash_valid_;
  uint32_t page_reads_generation_ = 1;

 public:
  // Diagnostic (log_render_pass_breaks): return address of the current
  // RequestRange caller, for attributing uploads that end render passes.
  void* debug_request_caller_ = nullptr;

 protected:

  // Set when GPU-written flags change, so an unchanged frame skips the copy.
  bool gpu_written_data_dirty_ = false;

  // Bitmap of dirty 64-entry chunks of valid_and_gpu_written, so localized GPU
  // writes only copy the chunks that changed.
  uint32_t dirty_blocks_ = 0;

  unsigned num_system_page_flags_ = 0;

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length,
      bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount =
      1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;

  // GPU-written memory downloading for traces. <Start address, length>.
  std::vector<std::pair<uint32_t, uint32_t>> trace_download_ranges_;
  uint32_t trace_download_page_count_ = 0;

  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last,
                   bool invalidated_by_gpu);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHARED_MEMORY_H_
