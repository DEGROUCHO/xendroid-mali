/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/shared_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/platform.h"

#if defined(__ANDROID__)
#include <android/sharedmem.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif  // __ANDROID__

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/bit_range.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shared_memory_working_set.h"

DECLARE_bool(shm_fault_batch_unprotect);

DEFINE_uint32(shm_log_gpu_invalidations, 0,
              "Diagnostic: log the first N CPU invalidations of GPU-owned "
              "pages; 0 disables.",
              "GPU");

DEFINE_bool(shared_memory_log_working_set, false,
            "Init-time diagnostic: log declared requested 4 KiB pages per "
            "frame, 120-frame window and process lifetime. Excludes pending "
            "and GPU-authoritative residency; does not evict or cap memory.",
            "GPU");

DEFINE_bool(shared_memory_snapshot_invalidation, false,
            "Snapshot submission invalidation bit blocks under the global "
            "mutex, then scan the snapshot outside it for upload hoisting. "
            "Does not change hashing, copies or Vulkan command ordering.",
            "GPU");
DEFINE_bool(shared_memory_short_frame_lock, false,
            "At swap, consume dirty page bits under a short global lock, "
            "then update CP-owned rewrite scores and report statistics "
            "outside it. Rewatch transactions retain the global lock.",
            "GPU");

DEFINE_bool(shm_hash_only_unwatched_pages, true,
            "Hash only the uploaded pages left unwatched (verified by hash "
            "instead of a write watch).",
            "GPU");

DEFINE_int32(shm_fault_invalidation_block_pages, 16,
             "Pages (aligned block) invalidated around a CPU write fault on "
             "watched shared memory. 64 (256 KB) is the upstream behavior; "
             "16 re-protects ~30% fewer pages per frame with fewer slow "
             "frames (Black Ops, Dante); 4 faults too often.",
             "GPU");
UPDATE_from_int32(shm_fault_invalidation_block_pages, 2026, 10, 24, 20, 64);
DEFINE_bool(shm_log_vma_count, false,
            "Diagnostics: with the ShmUnwatch statistics, also log the number "
            "of memory mappings of the process (lines of /proc/self/maps) - "
            "protection changes split mappings, and many of them make "
            "mprotect and page faults slower.",
            "GPU");
DEFINE_int32(log_syscall_sampler_us, 0,
             "Diagnostics (Android): every N microseconds read "
             "/proc/self/task/*/syscall and log every 5 s the share of time "
             "each thread spent blocked in memory map syscalls (mprotect, "
             "mmap, munmap, madvise) or page faults - contention on the "
             "kernel mmap lock, which makes the write-watch mprotect slow. 0 "
             "disables.",
             "GPU");
DEFINE_int32(shm_unwatch_score, 2,
             "Frames with a CPU rewrite (score +1 each, -1 per frame without) "
             "after which an uploaded shared memory page is left unwatched and "
             "hash-verified instead of re-protected with mprotect.",
             "GPU");
DEFINE_int32(shm_unwatch_score_max, 8,
             "Ceiling of a page's rewrite score: a page rewritten for many "
             "frames stays unwatched for up to this many frames after its last "
             "rewrite before it is re-protected (a back-off for pages the CPU "
             "keeps rewriting, each re-protect costing an mprotect).",
             "GPU");
DEFINE_bool(shm_unwatch_release_exclusion, true,
            "Pages read by a CPU-side cache (index conversion) or a texture "
            "stop being excluded from dynamic unwatching once a CPU write "
            "fired their watches, and CPU-read caches no longer watch pages "
            "the CPU rewrites frame after frame (they convert them on every "
            "use instead): their write watch and the guest fault before every "
            "rewrite both go away.",
            "GPU");
// Default since q587: Black Ops write-watch arms 90 -> 30 per frame, guest
// faults 48 -> 26, mprotect time -45%, +5-8% fps over three pairs, same image.
UPDATE_from_bool(shm_unwatch_release_exclusion, 2026, 10, 25, 22, false);
DEFINE_int32(shm_unwatch_decay_frames, 1,
             "Frames between decrements of a shared memory page's rewrite "
             "score. Larger values keep pages that the CPU rewrites "
             "periodically (ring buffers wrapping every few frames) unwatched "
             "and hash-verified instead of re-protected on every rewrite.",
             "GPU");

namespace xe {
namespace memory {
// memory_posix.cc: totals over every Protect call so far (all threads).
void GetProtectStats(uint64_t& calls, uint64_t& bytes, uint64_t& nanoseconds);
void GetProtectSplitStats(uint64_t calls[2], uint64_t nanoseconds[2]);
void ProbeProtectCost(uint64_t& anonymous_ns, uint64_t& shared_ns);
// memory_posix.cc: totals over every QueryProtect (/proc/self/maps parse).
void GetQueryProtectStats(uint64_t& calls, uint64_t& nanoseconds);
// memory.cc: lock-free totals for the optional batched fault unprotection.
void GetFaultUnprotectStats(uint64_t& calls, uint64_t& pages,
                           uint64_t& fallbacks);
}  // namespace memory
namespace gpu {

#if defined(__ANDROID__)
// Diagnostics: userfaultfd synchronous write protection as a replacement for
// mprotect write watches. UFFDIO_WRITEPROTECT takes the mmap lock only for
// reading and does not split mappings; logs whether the app may use it on
// shared memory and what a protect + unprotect pair costs against mprotect.
static void ProbeUffdWriteProtect(const char* kind, int fd, bool truncate) {
  constexpr size_t kSize = 64 * 4096, kRange = 4 * 4096;
  constexpr int kIterations = 2000;
  // ASharedMemory regions are created with their size; memfd needs one.
  if (fd < 0 || (truncate && ftruncate(fd, kSize))) {
    XELOGI("WriteTracking: {} probe: no shared memory", kind);
    if (fd >= 0) close(fd);
    return;
  }
  void* mapping =
      mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    XELOGI("WriteTracking: {} probe: mmap failed", kind);
    return;
  }
  uint8_t* bytes = static_cast<uint8_t*>(mapping);
  for (size_t i = 0; i < kSize; i += 4096) bytes[i] = 1;
  auto elapsed_us = [](auto start) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - start)
        .count();
  };
  auto mprotect_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kIterations; ++i) {
    uint8_t* range = bytes + (size_t(i) * 7 % 60) * 4096;
    mprotect(range, kRange, PROT_READ);
    mprotect(range, kRange, PROT_READ | PROT_WRITE);
  }
  double mprotect_us = elapsed_us(mprotect_start) / (2.0 * kIterations);
  int uffd = int(syscall(__NR_userfaultfd,
                         O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (uffd < 0) {
    XELOGI("WriteTracking: {} probe: mprotect {:.2f} us, userfaultfd: {}",
           kind, mprotect_us, strerror(errno));
    munmap(mapping, kSize);
    return;
  }
  uffdio_api api = {};
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
  uffdio_register registration = {};
  registration.range.start = uint64_t(uintptr_t(bytes));
  registration.range.len = kSize;
  registration.mode = UFFDIO_REGISTER_MODE_WP;
  const char* failure = nullptr;
  int error = 0;
  if (ioctl(uffd, UFFDIO_API, &api)) {
    failure = "UFFDIO_API";
  } else if (ioctl(uffd, UFFDIO_REGISTER, &registration)) {
    failure = "UFFDIO_REGISTER(WP)";
  }
  if (failure) error = errno;
  double uffd_us = 0.0;
  if (!failure) {
    auto uffd_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations && !failure; ++i) {
      uffdio_writeprotect writeprotect = {};
      writeprotect.range.start =
          uint64_t(uintptr_t(bytes + (size_t(i) * 7 % 60) * 4096));
      writeprotect.range.len = kRange;
      writeprotect.mode = UFFDIO_WRITEPROTECT_MODE_WP;
      if (ioctl(uffd, UFFDIO_WRITEPROTECT, &writeprotect)) {
        failure = "UFFDIO_WRITEPROTECT";
        error = errno;
        break;
      }
      writeprotect.mode = 0;
      if (ioctl(uffd, UFFDIO_WRITEPROTECT, &writeprotect)) {
        failure = "UFFDIO_WRITEPROTECT(0)";
        error = errno;
      }
    }
    uffd_us = elapsed_us(uffd_start) / (2.0 * kIterations);
  }
  if (failure) {
    XELOGI("WriteTracking: {} probe: mprotect {:.2f} us, {} failed: {} ({})",
           kind, mprotect_us, failure, strerror(error), error);
  } else {
    XELOGI(
        "WriteTracking: {} probe: mprotect {:.2f} us, UFFDIO_WRITEPROTECT "
        "{:.2f} us per call (4 pages, register ioctls 0x{:X})",
        kind, mprotect_us, uffd_us, uint64_t(registration.ioctls));
  }
  close(uffd);
  munmap(mapping, kSize);
}

// Diagnostics: can the kernel track guest writes without mprotect and signals
// (userfaultfd asynchronous write protection + PAGEMAP_SCAN, Linux 6.7+)?
static void LogWriteTrackingSupport() {
  int uffd = int(syscall(__NR_userfaultfd,
                         O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (uffd < 0) {
    XELOGI("WriteTracking: userfaultfd unavailable: {} ({})", strerror(errno),
           errno);
  } else {
    uffdio_api api = {};
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &api)) {
      XELOGI("WriteTracking: UFFDIO_API failed: {} ({})", strerror(errno),
             errno);
    } else {
      XELOGI(
          "WriteTracking: userfaultfd features 0x{:X}: wp_async={} "
          "wp_shmem={} wp_unpopulated={}",
          uint64_t(api.features),
          (api.features & UFFD_FEATURE_WP_ASYNC) != 0,
          (api.features & UFFD_FEATURE_WP_HUGETLBFS_SHMEM) != 0,
          (api.features & UFFD_FEATURE_WP_UNPOPULATED) != 0);
    }
    close(uffd);
  }
  ProbeUffdWriteProtect("memfd",
                        int(syscall(__NR_memfd_create, "xe-uffd-probe", 0)),
                        true);
  ProbeUffdWriteProtect("ASharedMemory",
                        ASharedMemory_create("xe-uffd-probe", 64 * 4096),
                        false);
  int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  if (pagemap < 0) {
    XELOGI("WriteTracking: /proc/self/pagemap: {} ({})", strerror(errno),
           errno);
    return;
  }
  alignas(4096) static uint8_t probe_page[4096];
  probe_page[0] = 1;
  page_region region = {};
  pm_scan_arg arg = {};
  arg.size = sizeof(arg);
  arg.start = uint64_t(uintptr_t(probe_page));
  arg.end = arg.start + sizeof(probe_page);
  arg.vec = uint64_t(uintptr_t(&region));
  arg.vec_len = 1;
  arg.category_mask = PAGE_IS_PRESENT;
  arg.return_mask = PAGE_IS_PRESENT | PAGE_IS_WRITTEN;
  long result = ioctl(pagemap, PAGEMAP_SCAN, &arg);
  XELOGI("WriteTracking: PAGEMAP_SCAN -> {} ({}), region categories 0x{:X}",
         result, result < 0 ? strerror(errno) : "ok",
         uint64_t(region.categories));
  close(pagemap);
}

// Samples /proc/self/task/<tid>/syscall. The kernel reports "running" for a
// thread on a CPU, "-1 ..." for one blocked outside a syscall (a page fault)
// and the syscall number for one sleeping inside it, so the counts are the
// time threads wait in each syscall, such as on the mmap lock.
static void SyscallSamplerThread(uint32_t interval_us) {
  static constexpr int kCallNumbers[] = {226, 222, 215, 233, 216, 29, 98};
  static constexpr const char* kCallNames[] = {
      "mprotect", "mmap", "munmap", "madvise", "mremap", "ioctl", "futex"};
  static constexpr size_t kCallCount = std::size(kCallNumbers);
  static constexpr size_t kMemoryCallCount = 5;
  struct ThreadSamples {
    int fd = -1;
    std::string name;
    uint32_t running = 0;
    uint32_t fault = 0;
    uint32_t calls[kCallCount] = {};
    bool alive = false;
  };
  std::unordered_map<int, ThreadSamples> threads;
  const int self_tid = int(gettid());
  auto refresh = [&]() {
    for (auto& [tid, thread] : threads) thread.alive = false;
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
      int tid = atoi(entry->d_name);
      if (tid <= 0 || tid == self_tid) continue;
      ThreadSamples& thread = threads[tid];
      thread.alive = true;
      if (thread.fd >= 0) continue;
      char path[64];
      snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
      if (FILE* comm = fopen(path, "r")) {
        char name[32] = {};
        if (fgets(name, sizeof(name), comm)) {
          name[strcspn(name, "\n")] = 0;
          thread.name = name;
        }
        fclose(comm);
      }
      snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", tid);
      thread.fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    closedir(dir);
    for (auto it = threads.begin(); it != threads.end();) {
      if (!it->second.alive) {
        if (it->second.fd >= 0) close(it->second.fd);
        it = threads.erase(it);
      } else {
        ++it;
      }
    }
  };
  using clock = std::chrono::steady_clock;
  auto next_refresh = clock::now();
  auto next_report = next_refresh + std::chrono::seconds(5);
  uint32_t ticks = 0;
  char buffer[256];
  for (;;) {
    auto now = clock::now();
    if (now >= next_refresh) {
      refresh();
      next_refresh = now + std::chrono::seconds(1);
    }
    for (auto& [tid, thread] : threads) {
      if (thread.fd < 0) continue;
      ssize_t length = pread(thread.fd, buffer, sizeof(buffer) - 1, 0);
      if (length <= 0) continue;
      buffer[length] = 0;
      if (buffer[0] == 'r') {
        ++thread.running;
        continue;
      }
      long number = strtol(buffer, nullptr, 10);
      if (number < 0) {
        ++thread.fault;
        continue;
      }
      for (size_t i = 0; i < kCallCount; ++i) {
        if (number == kCallNumbers[i]) {
          ++thread.calls[i];
          break;
        }
      }
    }
    ++ticks;
    if (now >= next_report && ticks) {
      // Percent of the window each thread spent in each state.
      std::vector<std::pair<uint32_t, ThreadSamples*>> order;
      uint32_t totals[kCallCount + 1] = {};
      for (auto& [tid, thread] : threads) {
        uint32_t memory = thread.fault;
        for (size_t i = 0; i < kMemoryCallCount; ++i) {
          memory += thread.calls[i];
        }
        for (size_t i = 0; i < kCallCount; ++i) totals[i] += thread.calls[i];
        totals[kCallCount] += thread.fault;
        order.emplace_back(memory, &thread);
      }
      std::sort(order.begin(), order.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      const float scale = 100.0f / float(ticks);
      XELOGI(
          "SyscallSampler: {} ticks, threads x 100% blocked: mprotect={:.1f} "
          "mmap={:.1f} munmap={:.1f} madvise={:.1f} mremap={:.1f} "
          "fault={:.1f} ioctl={:.1f}",
          ticks, totals[0] * scale, totals[1] * scale, totals[2] * scale,
          totals[3] * scale, totals[4] * scale, totals[kCallCount] * scale,
          totals[5] * scale);
      for (size_t i = 0; i < std::min(order.size(), size_t(12)); ++i) {
        const ThreadSamples& thread = *order[i].second;
        std::string line;
        for (size_t c = 0; c < kCallCount; ++c) {
          if (thread.calls[c]) {
            line += fmt::format(" {}={:.1f}", kCallNames[c],
                                thread.calls[c] * scale);
          }
        }
        XELOGI("SyscallSampler:   {:<16} run={:.1f} fault={:.1f}{}",
               thread.name, thread.running * scale, thread.fault * scale,
               line);
      }
      for (auto& [tid, thread] : threads) {
        thread.running = thread.fault = 0;
        std::fill(std::begin(thread.calls), std::end(thread.calls), 0);
      }
      ticks = 0;
      next_report = now + std::chrono::seconds(5);
    }
    usleep(interval_us);
  }
}
#endif  // __ANDROID__

SharedMemory::SharedMemory(Memory& memory) : memory_(memory) {
  page_size_log2_ = xe::log2_ceil(uint32_t(xe::memory::page_size()));
}

SharedMemory::~SharedMemory() { ShutdownCommon(); }

bool SharedMemory::InitializeCommon() {
#if defined(__ANDROID__)
  if (cvars::log_syscall_sampler_us > 0) {
    std::thread(SyscallSamplerThread,
                uint32_t(cvars::log_syscall_sampler_us))
        .detach();
  }
#endif  // __ANDROID__
  if (cvars::shared_memory_log_working_set) {
    declared_working_set_ = std::make_unique<SharedMemoryWorkingSet>();
  }
  size_t num_system_page_flags_entries =
      ((kBufferSize >> page_size_log2_) + 63) / 64;
  num_system_page_flags_ = static_cast<uint32_t>(num_system_page_flags_entries);

  // Three flag arrays (valid, valid_and_gpu_written,
  // invalidated_in_submission), 8 bytes per 64-page entry.
  uint64_t* system_page_flags_base = (uint64_t*)memory::AllocFixed(
      nullptr, num_system_page_flags_ * 3 * sizeof(uint64_t),
      memory::AllocationType::kReserveCommit, memory::PageAccess::kReadWrite);

  if (!system_page_flags_base) {
    XELOGE("SharedMemory: Failed to allocate system page flags");
    return false;
  }

  system_page_flags_valid_ = system_page_flags_base;
  system_page_flags_valid_and_gpu_written_ =
      system_page_flags_base + num_system_page_flags_;
  system_page_flags_invalidated_in_submission_ =
      system_page_flags_base + 2 * num_system_page_flags_;

  memset(system_page_flags_base, 0, 3 * 8 * num_system_page_flags_entries);

  gpu_written_history_ = static_cast<uint64_t*>(memory::AllocFixed(
      nullptr, kGpuWrittenHistoryWordCount * sizeof(uint64_t),
      memory::AllocationType::kReserveCommit, memory::PageAccess::kReadWrite));
  if (gpu_written_history_) {
    memset(gpu_written_history_, 0,
           kGpuWrittenHistoryWordCount * sizeof(uint64_t));
  } else {
    // Shared memory remains usable, but CPU-derived vertex bounds must reject
    // all reads when their provenance cannot be checked.
    XELOGW("SharedMemory: GPU-written input history unavailable; CPU vertex "
           "validation will fail closed");
  }

  system_page_flags_used_in_submission_.assign(num_system_page_flags_,
                                               UINT64_MAX);
  system_page_flags_used_hard_in_submission_.assign(num_system_page_flags_,
                                                    UINT64_MAX);
  system_page_flags_used_in_pass_.assign(num_system_page_flags_, UINT64_MAX);
  system_page_flags_written_in_pass_.assign(num_system_page_flags_, UINT64_MAX);
  written_in_pass_blocks_.clear();
  written_in_pass_all_ = true;
  system_page_flags_pending_access_.assign(num_system_page_flags_, 0);
  pending_access_blocks_.clear();
  pending_access_scope_active_ = false;
  used_in_pass_all_ = true;
  system_page_flags_dynamic_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_unreadable_.assign(num_system_page_flags_, 0);
  page_upload_hash_.assign(size_t(num_system_page_flags_) * 64, 0);
  page_upload_hash_valid_.assign(num_system_page_flags_, 0);
  unwatched_pages_.assign(num_system_page_flags_, 0);
  texture_watched_pages_.assign(num_system_page_flags_, 0);
  dirty_this_frame_.assign(num_system_page_flags_, 0);
  submission_invalidation_snapshot_.assign(num_system_page_flags_, 0);
  frame_dirty_snapshot_.assign(num_system_page_flags_, 0);
  dirty_score_nonzero_.assign(num_system_page_flags_, 0);
  unwatch_candidates_.assign(num_system_page_flags_, 0);
  requested_pages_.assign(num_system_page_flags_, 0);
  requested_page_blocks_.reserve(num_system_page_flags_);
  upload_ranges_.reserve((kBufferSize >> page_size_log2_) / 2);
  batch_upload_ranges_.reserve((kBufferSize >> page_size_log2_) / 2);
  dirty_score_.assign(size_t(num_system_page_flags_) * 64, 0);
  unwatch_verified_serial_.assign(size_t(num_system_page_flags_) * 64, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(
          MemoryInvalidationCallbackThunk, this);
  return true;
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) -
                    6));
}

void SharedMemory::ShutdownCommon() {
  ReleaseTraceDownloadRanges();

  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(
        memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  // One AllocFixed backs all flag arrays; valid_ is its base pointer.
  if (system_page_flags_valid_) {
    memory::DeallocFixed(system_page_flags_valid_, 0,
                         memory::DeallocationType::kRelease);
    system_page_flags_valid_ = nullptr;
  }

  if (gpu_written_history_) {
    memory::DeallocFixed(gpu_written_history_, 0,
                         memory::DeallocationType::kRelease);
    gpu_written_history_ = nullptr;
  }

  system_page_flags_valid_and_gpu_written_ = nullptr;
  system_page_flags_invalidated_in_submission_ = nullptr;
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  OnCpuRangeAccess(0, kBufferSize);
  auto global_lock = global_critical_region_.Acquire();

  for (uint32_t b = 0; b < num_system_page_flags_; ++b) {
    __atomic_store_n(&system_page_flags_valid_[b], uint64_t(0), __ATOMIC_RELEASE);
  }
  std::memset(system_page_flags_valid_and_gpu_written_, 0,
              num_system_page_flags_ * sizeof(uint64_t));
  gpu_written_partial_pages_.clear();
  std::memset(system_page_flags_invalidated_in_submission_, 0xFF,
              num_system_page_flags_ * sizeof(uint64_t));
  std::fill(system_page_flags_used_in_submission_.begin(),
            system_page_flags_used_in_submission_.end(), UINT64_MAX);
  std::fill(system_page_flags_used_hard_in_submission_.begin(),
            system_page_flags_used_hard_in_submission_.end(), UINT64_MAX);
  std::fill(system_page_flags_used_in_pass_.begin(),
            system_page_flags_used_in_pass_.end(), UINT64_MAX);
  used_in_pass_all_ = true;
  std::fill(system_page_flags_written_in_pass_.begin(),
            system_page_flags_written_in_pass_.end(), UINT64_MAX);
  written_in_pass_all_ = true;
  request_epochs_.InvalidateAll();
  // Mark all chunks dirty so the next copy refreshes everything.
  dirty_blocks_ = 0xFFFFFFFF;
  gpu_written_data_dirty_ = true;
}

void SharedMemory::OnGpuSubmissionOpened() {
  if (!system_page_flags_invalidated_in_submission_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  std::memset(system_page_flags_invalidated_in_submission_, 0,
              num_system_page_flags_ * sizeof(uint64_t));
  std::fill(system_page_flags_used_in_submission_.begin(),
            system_page_flags_used_in_submission_.end(), 0);
  std::fill(system_page_flags_used_hard_in_submission_.begin(),
            system_page_flags_used_hard_in_submission_.end(), 0);
  if (++page_reads_generation_ == 0) {
    page_reads_generation_ = 1;
  }
}

void SharedMemory::MarkPagesUsedHard(
    const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count,
    bool in_pass_body) {
  if (system_page_flags_used_in_submission_.empty()) {
    return;
  }
  // A 64-page block at a time.
  for (uint32_t i = 0; i < count; ++i) {
    if (!page_ranges[i].second) {
      continue;
    }
    const uint32_t first = page_ranges[i].first;
    const uint32_t last = first + page_ranges[i].second - 1;
    for (uint32_t block = first >> 6; block <= last >> 6; ++block) {
      uint64_t mask = UINT64_MAX;
      if (block == first >> 6) {
        mask &= UINT64_MAX << (first & 63);
      }
      if (block == last >> 6) {
        mask &= UINT64_MAX >> (63 - (last & 63));
      }
      system_page_flags_used_in_submission_[block] |= mask;
      system_page_flags_used_hard_in_submission_[block] |= mask;
      MarkUsedInPass(block, mask);
      if (in_pass_body) {
        MarkWrittenInPass(block, mask);
      }
    }
  }
}

void SharedMemory::SetPageUploadHashes(uint32_t page_first,
                                       uint32_t page_count,
                                       const uint8_t* source) {
  if (page_upload_hash_.empty() || !page_count) return;
  const uint32_t last = page_first + page_count - 1;
  const size_t page_size = size_t(1) << page_size_log2_;
  const bool only_unwatched = cvars::shm_hash_only_unwatched_pages;
  for (uint32_t b = page_first >> 6; b <= last >> 6; ++b) {
    uint64_t bits = coherency::Mask(b == page_first >> 6 ? page_first : 0,
                                  b == last >> 6 ? last : 63);
    if (only_unwatched) bits &= unwatched_pages_[b];
    while (bits) {
      const uint32_t bit = xe::tzcnt(bits);
      bits &= bits - 1;
      const uint32_t page = (b << 6) + bit;
      page_upload_hash_[page] = XXH3_64bits(
          source + size_t(page - page_first) * page_size, page_size);
      __atomic_fetch_or(&page_upload_hash_valid_[b], uint64_t(1) << bit,
                        __ATOMIC_RELAXED);
    }
  }
}

void SharedMemory::MarkPagesValidUnreadable(uint32_t page_first,
                                            uint32_t page_end) {
  if (system_page_flags_valid_unreadable_.empty()) {
    return;
  }
  if (page_first == page_end) return;
  const uint32_t last = page_end - 1;
  for (uint32_t b = page_first >> 6; b <= last >> 6; ++b) {
    system_page_flags_valid_unreadable_[b] |= coherency::Mask(
        b == page_first >> 6 ? page_first : 0, b == last >> 6 ? last : 63);
  }
}

SharedMemory::PendingAccessScope::PendingAccessScope(
    SharedMemory* shared_memory)
    : shared_memory_(shared_memory) {
  if (!shared_memory_) {
    return;
  }
  assert_false(shared_memory_->pending_access_scope_active_);
  assert_true(shared_memory_->pending_access_blocks_.empty());
  shared_memory_->pending_access_scope_active_ = true;
  ++shared_memory_->pending_access_serial_;
}

SharedMemory::PendingAccessScope::~PendingAccessScope() {
  if (!shared_memory_) {
    return;
  }
  for (uint32_t block : shared_memory_->pending_access_blocks_) {
    shared_memory_->system_page_flags_pending_access_[block] = 0;
  }
  shared_memory_->pending_access_blocks_.clear();
  shared_memory_->pending_access_scope_active_ = false;
}

void SharedMemory::OnRenderPassBegin() {
  ++pass_serial_;
  // Writes recorded before the pass began are ordered before its head.
  if (!system_page_flags_written_in_pass_.empty()) {
    if (written_in_pass_all_) {
      std::fill(system_page_flags_written_in_pass_.begin(),
                system_page_flags_written_in_pass_.end(), 0);
      written_in_pass_all_ = false;
    } else {
      for (uint32_t block : written_in_pass_blocks_) {
        system_page_flags_written_in_pass_[block] = 0;
      }
    }
  }
  written_in_pass_blocks_.clear();
  if (system_page_flags_used_in_pass_.empty()) {
    return;
  }
  if (used_in_pass_all_) {
    std::fill(system_page_flags_used_in_pass_.begin(),
              system_page_flags_used_in_pass_.end(), 0);
    used_in_pass_all_ = false;
  } else {
    for (uint32_t block : used_in_pass_blocks_) {
      system_page_flags_used_in_pass_[block] = 0;
    }
  }
  used_in_pass_blocks_.clear();
  // RequestRange (including cache hits), ring-backed reads and GPU writes all
  // feed this set. Do not call MarkUsedInPass while iterating pending blocks.
  for (uint32_t block : pending_access_blocks_) {
    system_page_flags_used_in_pass_[block] =
        system_page_flags_pending_access_[block];
    used_in_pass_blocks_.push_back(block);
  }
}

bool SharedMemory::AnyPageUsedInCurrentPass(
    const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const {
  if (system_page_flags_used_in_pass_.empty() || used_in_pass_all_) {
    return true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t page_first = page_ranges[i].first;
    uint32_t page_last = page_first + page_ranges[i].second - 1;
    uint32_t block_first = page_first >> 6;
    uint32_t block_last = page_last >> 6;
    for (uint32_t b = block_first; b <= block_last; ++b) {
      uint64_t mask = UINT64_MAX;
      if (b == block_first) {
        mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (b == block_last && (page_last & 63) != 63) {
        mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      if (system_page_flags_used_in_pass_[b] & mask) {
        return true;
      }
    }
  }
  return false;
}

bool SharedMemory::AnyByteRangeWrittenInCurrentPass(
    const std::pair<uint32_t, uint32_t>* byte_ranges, uint32_t count) const {
  if (system_page_flags_written_in_pass_.empty() || written_in_pass_all_) {
    return true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!byte_ranges[i].second) {
      continue;
    }
    const uint32_t page_first = byte_ranges[i].first >> page_size_log2_;
    const uint32_t page_last =
        (byte_ranges[i].first + byte_ranges[i].second - 1) >> page_size_log2_;
    for (uint32_t b = page_first >> 6; b <= page_last >> 6; ++b) {
      uint64_t mask = UINT64_MAX;
      if (b == page_first >> 6) {
        mask &= UINT64_MAX << (page_first & 63);
      }
      if (b == page_last >> 6) {
        mask &= UINT64_MAX >> (63 - (page_last & 63));
      }
      if (system_page_flags_written_in_pass_[b] & mask) {
        return true;
      }
    }
  }
  return false;
}

bool SharedMemory::AnyPageUsedSinceSubmissionOpen(
    const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const {
  if (system_page_flags_used_in_submission_.empty()) {
    return true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t page_first = page_ranges[i].first;
    uint32_t page_last = page_first + page_ranges[i].second - 1;
    uint32_t block_first = page_first >> 6;
    uint32_t block_last = page_last >> 6;
    for (uint32_t b = block_first; b <= block_last; ++b) {
      uint64_t mask = UINT64_MAX;
      if (b == block_first) {
        mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (b == block_last && (page_last & 63) != 63) {
        mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      if (system_page_flags_used_in_submission_[b] & mask) {
        return true;
      }
    }
  }
  return false;
}

void SharedMemory::MarkRangeUsedInSubmission(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize ||
      system_page_flags_used_in_submission_.empty()) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;
  for (uint32_t b = block_first; b <= block_last; ++b) {
    uint64_t mask = UINT64_MAX;
    if (b == block_first) {
      mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (b == block_last && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    system_page_flags_used_in_submission_[b] |= mask;
    MarkUsedInPass(b, mask);
  }
}

bool SharedMemory::AnyPageInvalidatedSinceSubmissionOpen(
    const std::pair<uint32_t, uint32_t>* page_ranges, uint32_t count) const {
  auto global_lock = global_critical_region_.Acquire();
  const uint64_t* invalidated = system_page_flags_invalidated_in_submission_;
  if (cvars::shared_memory_snapshot_invalidation) {
    // This is the same decision point as the legacy scan: invalidations after
    // it cannot retroactively affect the decision. Do not clear any bits here.
    // All writers and submission-open resets remain serialized by the global
    // lock, so this is one coherent snapshot even across multiple ranges.
    ++stat_invalidation_snapshots_;
    for (uint32_t i = 0; i < count; ++i) {
      if (!page_ranges[i].second) {
        continue;
      }
      uint32_t first = page_ranges[i].first >> 6;
      uint32_t last =
          (page_ranges[i].first + page_ranges[i].second - 1) >> 6;
      uint32_t words = last + 1 - first;
      std::memcpy(submission_invalidation_snapshot_.data() + first,
                  invalidated + first, size_t(words) * sizeof(uint64_t));
      stat_invalidation_snapshot_words_ += words;
    }
    invalidated = submission_invalidation_snapshot_.data();
    global_lock.unlock();
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!page_ranges[i].second) {
      continue;
    }
    uint32_t page_first = page_ranges[i].first;
    uint32_t page_last = page_first + page_ranges[i].second - 1;
    uint32_t block_first = page_first >> 6;
    uint32_t block_last = page_last >> 6;
    for (uint32_t b = block_first; b <= block_last; ++b) {
      uint64_t mask = UINT64_MAX;
      if (b == block_first) {
        mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (b == block_last && (page_last & 63) != 63) {
        mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      if (invalidated[b] & mask) {
        return true;
      }
    }
  }
  return false;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  if (gpu_written_data_dirty_) {
    // Only copy the chunks that changed. Read and reset the dirty bitmap.
    uint32_t dirty_mask = dirty_blocks_;
    dirty_blocks_ = 0;
    gpu_written_data_dirty_ = false;

    uint32_t dirty_count = xe::bit_count(dirty_mask);

    // Past ~50% dirty the per-chunk bookkeeping costs more than a straight
    // copy.
    if (dirty_count == 0 || dirty_count > 16) {
      for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
        uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
        system_page_flags_invalidated_in_submission_[i] |=
            system_page_flags_valid_[i] & ~gpu_written_block;
        __atomic_store_n(&system_page_flags_valid_[i], gpu_written_block,
                         __ATOMIC_RELEASE);
      }
      request_epochs_.InvalidateAll();
    } else {
      while (dirty_mask) {
        uint32_t block_index;
        xe::bit_scan_forward(dirty_mask, &block_index);
        dirty_mask &= ~(1u << block_index);

        // Each chunk is 64 entries of 8 bytes.
        uint32_t entry_offset = block_index * 64;
        for (uint32_t i = entry_offset; i < entry_offset + 64; ++i) {
          uint64_t gpu_written_block =
              system_page_flags_valid_and_gpu_written_[i];
          system_page_flags_invalidated_in_submission_[i] |=
              system_page_flags_valid_[i] & ~gpu_written_block;
          __atomic_store_n(&system_page_flags_valid_[i], gpu_written_block,
                         __ATOMIC_RELEASE);
        }
      }
      request_epochs_.InvalidateAll();
    }
  }
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(
    GlobalWatchCallback callback, void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(
    uint32_t start, uint32_t length, WatchCallback callback,
    void* callback_context, void* callback_data, uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first =
      watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last =
      watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;
  if (!texture_watched_pages_.empty()) {
    // Pages with range watches (textures) are never unwatched - their users
    // rely on the invalidation notifications.
    for (uint32_t b = watch_page_first >> 6; b <= watch_page_last >> 6; ++b) {
      uint64_t mask = UINT64_MAX;
      if (b == watch_page_first >> 6) {
        mask &= ~((uint64_t(1) << (watch_page_first & 63)) - 1);
      }
      if (b == watch_page_last >> 6 && (watch_page_last & 63) != 63) {
        mask &= (uint64_t(1) << ((watch_page_last & 63) + 1)) - 1;
      }
      texture_watched_pages_[b] |= mask;
      if (unwatched_pages_[b] & mask) {
        RewatchPages(b, unwatched_pages_[b] & mask);
      }
    }
  }
  // chrispy: Not required the global lock is always held by the caller
  // auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() ||
        watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() ||
          watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last,
                               bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last =
      (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();
  // CPU-read copies also depend on GPU writes / RAM mirrors. Advance the epoch
  // under the same memory lock as the watch notifications.
  if (invalidated_by_gpu) {
    request_epochs_.Invalidate(address_first, address_last - address_first + 1);
  }

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context,
                           address_first, address_last, invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    if (i + 1 <= bucket_last) {
      WatchNode* nextnode = watch_buckets_[i + 1];
      if (nextnode) {
        swcache::PrefetchL1(nextnode->range);
      }
    }
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (node) {
        swcache::PrefetchL1(node);
      }
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context,
                        range->callback_data, range->callback_argument,
                        invalidated_by_gpu);
        if (node && node->range) {
          swcache::PrefetchL1(node->range);
        }
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length,
                                     bool written_to_buffer) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  MarkRangeEverGpuWritten(start, length);
  if (declared_working_set_) declared_working_set_->Request(start, length);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // A GPU access remains an ordering dependency even when its output lives
  // in an imported host buffer rather than in the device-memory copy.
  const std::pair<uint32_t, uint32_t> written_pages(
      page_first, page_last - page_first + 1);
  MarkPagesUsedHard(&written_pages, 1);

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  // written_to_buffer also marks it GPU-authoritative, which must not happen
  // when the write went to guest RAM instead - that would keep the buffer's
  // stale copy across cache clears.
  MakeRangeValid(start, length, written_to_buffer);
}

void SharedMemory::MakeRangeValidInSurfaceOnly(uint32_t start, uint32_t length) {
  assert_true(length && start < kBufferSize && length <= kBufferSize - start);
  auto lock = global_critical_region_.Acquire();
  const uint32_t first = start >> page_size_log2_;
  const uint32_t last = (start + length - 1) >> page_size_log2_;
  for (uint32_t b = first >> 6; b <= last >> 6; ++b) {
    const uint64_t mask = coherency::Mask(b == first >> 6 ? first : 0,
                                         b == last >> 6 ? last : 63);
    __atomic_fetch_and(&system_page_flags_valid_[b], ~mask, __ATOMIC_RELEASE);
  }
  request_epochs_.Invalidate(start, length);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length,
                                  bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();

    if (written_by_gpu) {
      // Bytes outside a partial GPU write stay CPU-owned: the CPU may write
      // them between the pre-write upload and this call (Halo Reach: an
      // emitter table sharing its page with the end of a particle export).
      const uint32_t page_mask = (uint32_t(1) << page_size_log2_) - 1;
      auto record = [&](uint32_t page, uint32_t first, uint32_t end) {
        const bool was_gpu_written =
            (system_page_flags_valid_and_gpu_written_[page >> 6] >>
             (page & 63)) & 1;
        auto it = gpu_written_partial_pages_.find(page);
        if (was_gpu_written && it == gpu_written_partial_pages_.end()) {
          return;  // the GPU already wrote all of it
        }
        if (first == 0 && end == page_mask + 1) {
          if (it != gpu_written_partial_pages_.end()) {
            gpu_written_partial_pages_.erase(it);
          }
          return;
        }
        GpuWrittenPartialPage& partial = gpu_written_partial_pages_[page];
        if (!was_gpu_written) partial.count = 0;
        // Merge every range overlapping or touching the new one.
        uint32_t kept = 0;
        for (uint32_t i = 0; i < partial.count; ++i) {
          auto range = partial.ranges[i];
          if (range.first <= end && first <= range.second) {
            first = std::min(first, range.first);
            end = std::max(end, range.second);
          } else {
            partial.ranges[kept++] = range;
          }
        }
        if (kept == kGpuWrittenPartialRanges) {
          // Out of slots (never seen): one range over all of them.
          for (uint32_t i = 0; i < kept; ++i) {
            first = std::min(first, partial.ranges[i].first);
            end = std::max(end, partial.ranges[i].second);
          }
          kept = 0;
        }
        partial.ranges[kept++] = {first, end};
        partial.count = kept;
        if (first == 0 && end == page_mask + 1 && kept == 1) {
          gpu_written_partial_pages_.erase(page);
        }
      };
      const uint32_t first_end = valid_page_first == valid_page_last
                                     ? (last & page_mask) + 1
                                     : page_mask + 1;
      record(valid_page_first, start & page_mask, first_end);
      if (valid_page_last != valid_page_first) {
        record(valid_page_last, 0, (last & page_mask) + 1);
      }
      if (valid_page_last > valid_page_first + 1 &&
          !gpu_written_partial_pages_.empty()) {
        std::erase_if(gpu_written_partial_pages_, [&](const auto& entry) {
          return entry.first > valid_page_first && entry.first < valid_page_last;
        });
      }
    }

    if (written_by_gpu && memory_invalidation_callback_handle_) {
      // Protect before publishing the GPU-written bit: a CPU store after the
      // bit but before the protection would not fault, leaving the bit set,
      // and the buffer's stale copy would later be written back over it
      // (savestate, memexport RAM mirror). A store faulting from here on waits
      // for this lock and then clears the bit.
      ++stat_protect_calls_;
      stat_protect_pages_ += valid_page_last - valid_page_first + 1;
      memory().EnablePhysicalMemoryAccessCallbacks(
          valid_page_first << page_size_log2_,
          (valid_page_last - valid_page_first + 1) << page_size_log2_, true,
          false);
    }

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      // SystemPageFlagsBlock& block = system_page_flags_[i];
      __atomic_fetch_or(&system_page_flags_valid_[i], valid_bits, __ATOMIC_RELEASE);
      if (!page_upload_hash_valid_.empty()) {
        __atomic_fetch_and(&page_upload_hash_valid_[i], ~valid_bits,
                           __ATOMIC_RELAXED);
      }
      if (written_by_gpu) {
        // GPU-owned pages have no CPU upload hash to verify. Remove any
        // dynamic-unwatch state here without comparing against that stale hash.
        uint64_t unwatched_bits = unwatched_pages_[i] & valid_bits;
        if (unwatched_bits) {
          unwatched_pages_[i] &= ~unwatched_bits;
          unwatched_page_count_ -= uint32_t(xe::bit_count(unwatched_bits));
        }
        if (!system_page_flags_used_in_submission_.empty()) {
          system_page_flags_used_in_submission_[i] |= valid_bits;
          MarkUsedInPass(i, valid_bits);
          MarkWrittenInPass(i, valid_bits);
          system_page_flags_used_hard_in_submission_[i] |= valid_bits;
        }
        system_page_flags_valid_and_gpu_written_[i] |= valid_bits;
        // Mark the touched chunk dirty so the next copy picks it up.
        gpu_written_data_dirty_ = true;
        dirty_blocks_ |= 1u << (i >> 6);
      } else {
        system_page_flags_valid_and_gpu_written_[i] &= ~valid_bits;
        if (!system_page_flags_valid_unreadable_.empty()) {
          system_page_flags_valid_unreadable_[i] &= ~valid_bits;
        }
      }
    }

  }

  if (memory_invalidation_callback_handle_ && !written_by_gpu) {
    // A page that isn't writable here gets no watch. A later guest
    // protect-to-writable invalidates it unconditionally, so its writes are
    // still caught.
    if (unwatch_enabled_ && !dirty_score_.empty()) {
      EnableWatchesExceptDynamic(valid_page_first, valid_page_last);
    } else {
      ++stat_protect_calls_;
      stat_protect_pages_ += valid_page_last - valid_page_first + 1;
      memory().EnablePhysicalMemoryAccessCallbacks(
          valid_page_first << page_size_log2_,
          (valid_page_last - valid_page_first + 1) << page_size_log2_, true,
          false);
    }
  }
}

// Pages rewritten in at least shm_unwatch_score recent frames (score +1 per
// frame with a rewrite, -1 per frame without, up to shm_unwatch_score_max) are
// left unwatched; they are watched again once the score decays to 0.

void SharedMemory::EnableWatchesExceptDynamic(uint32_t page_first,
                                              uint32_t page_last) {
  auto global_lock = global_critical_region_.Acquire();
  coherency::ForEachRange(
      page_first, page_last,
      [&](uint32_t b) {
        uint64_t mask = coherency::Mask(b == page_first >> 6 ? page_first : 0,
                                      b == page_last >> 6 ? page_last : 63);
        uint64_t dynamic = unwatch_candidates_[b] & mask &
                           ~texture_watched_pages_[b] &
                           ~system_page_flags_valid_and_gpu_written_[b];
        uint64_t added = dynamic & ~unwatched_pages_[b];
        unwatched_page_count_ += uint32_t(xe::bit_count(added));
        unwatched_pages_[b] |= dynamic;
        uint64_t uploaded_unwatched = unwatched_pages_[b] & mask;
        while (uploaded_unwatched) {
          uint32_t page = (b << 6) + xe::tzcnt(uploaded_unwatched);
          uploaded_unwatched &= uploaded_unwatched - 1;
          unwatch_verified_serial_[page] = unwatch_verify_serial_;
        }
        // A score of 1 is the hysteresis interval: remain writable until
        // OnFrameEndUnwatch explicitly re-arms and verifies at score 0.
        return mask & ~unwatched_pages_[b];
      },
      [&](uint32_t first, uint32_t count) {
        ++stat_protect_calls_;
        stat_protect_pages_ += count;
        memory().EnablePhysicalMemoryAccessCallbacks(
            first << page_size_log2_, count << page_size_log2_, true, false);
      });
}

bool SharedMemory::UnwatchedPageMatchesUpload(uint32_t page) {
  uint32_t address = page << page_size_log2_;
  if (!memory().GetPhysicalHeap()->IsPageReadableRelaxed(address)) {
    return false;
  }
  if (!((__atomic_load_n(&page_upload_hash_valid_[page >> 6],
                         __ATOMIC_RELAXED) >>
         (page & 63)) &
        1)) {
    return false;
  }
  ++stat_unwatch_hashes_;
  return XXH3_64bits(memory().TranslatePhysical<const uint8_t*>(address),
                     size_t(1) << page_size_log2_) == page_upload_hash_[page];
}

void SharedMemory::VerifyUnwatchedPages(uint32_t page_first,
                                        uint32_t page_last) {
  for (uint32_t b = page_first >> 6; b <= page_last >> 6; ++b) {
    uint64_t mask = UINT64_MAX;
    if (b == page_first >> 6) {
      mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (b == page_last >> 6 && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    uint64_t bits = unwatched_pages_[b] & mask &
                    __atomic_load_n(&system_page_flags_valid_[b],
                                    __ATOMIC_ACQUIRE);
    while (bits) {
      uint32_t page = (b << 6) + xe::tzcnt(bits);
      bits &= bits - 1;
      if (unwatch_verified_serial_[page] == unwatch_verify_serial_) {
        continue;
      }
      unwatch_verified_serial_[page] = unwatch_verify_serial_;
      if (!UnwatchedPageMatchesUpload(page)) {
        ++stat_unwatch_invalidations_;
        MemoryInvalidationCallback(page << page_size_log2_,
                                   uint32_t(1) << page_size_log2_, true);
      }
    }
  }
}

void SharedMemory::RewatchPages(uint32_t block, uint64_t mask) {
  auto global_lock = global_critical_region_.Acquire();
  mask &= unwatched_pages_[block];
  if (!mask) {
    return;
  }
  unwatched_pages_[block] &= ~mask;
  unwatched_page_count_ -= uint32_t(xe::bit_count(mask));
  // Protect first, so a write racing the verification below still faults.
  uint64_t valid = mask & system_page_flags_valid_[block];
  uint64_t runs = valid;
  while (runs) {
    uint32_t first = xe::tzcnt(runs);
    uint64_t shifted = runs >> first;
    uint32_t count = shifted == UINT64_MAX ? 64 - first : xe::tzcnt(~shifted);
    memory().EnablePhysicalMemoryAccessCallbacks(
        ((block << 6) + first) << page_size_log2_, count << page_size_log2_,
        true, false);
    runs &= ~(count >= 64 ? UINT64_MAX
                          : (((uint64_t(1) << count) - 1) << first));
  }
  while (valid) {
    uint32_t page = (block << 6) + xe::tzcnt(valid);
    valid &= valid - 1;
    ++stat_unwatch_rewatches_;
    if (!UnwatchedPageMatchesUpload(page)) {
      ++stat_unwatch_invalidations_;
      MemoryInvalidationCallback(page << page_size_log2_,
                                 uint32_t(1) << page_size_log2_, true);
    }
  }
}

void SharedMemory::OnFrameEndUnwatch() {
  if (dirty_score_.empty()) {
    return;
  }
  const bool short_lock = cvars::shared_memory_short_frame_lock;
  const bool report = ++unwatch_report_frames_ >= 60;
  uint64_t report_fault_invalidations = 0;
  auto global_lock = global_critical_region_.Acquire();
  if (short_lock) {
    // Guest callbacks write only dirty_this_frame_, not the rewrite scores.
    // Consume every word atomically with respect to those callbacks: a fault
    // after this snapshot remains in dirty_this_frame_ for the next frame.
    std::memcpy(frame_dirty_snapshot_.data(), dirty_this_frame_.data(),
                size_t(num_system_page_flags_) * sizeof(uint64_t));
    std::memset(dirty_this_frame_.data(), 0,
                size_t(num_system_page_flags_) * sizeof(uint64_t));
  }
  if (report) {
    // This counter is also written by the guest signal handler.
    report_fault_invalidations = stat_fault_invalidations_;
    stat_fault_invalidations_ = 0;
  }
  if (short_lock) {
    global_lock.unlock();
  }
  const int32_t decay_frames = std::max(cvars::shm_unwatch_decay_frames, 1);
  const uint8_t unwatch_score_max =
      uint8_t(std::clamp(cvars::shm_unwatch_score_max, 1, 255));
  const uint8_t unwatch_score =
      uint8_t(std::clamp(cvars::shm_unwatch_score, 1, int32_t(unwatch_score_max)));
  const bool decay_frame = ++unwatch_decay_counter_ >= uint32_t(decay_frames);
  if (decay_frame) {
    unwatch_decay_counter_ = 0;
  }
  for (uint32_t b = 0; b < num_system_page_flags_; ++b) {
    uint64_t dirty = short_lock ? frame_dirty_snapshot_[b] : dirty_this_frame_[b];
    uint64_t live = dirty | dirty_score_nonzero_[b];
    uint64_t rewatch = unwatch_enabled_ ? 0 : unwatched_pages_[b];
    if (!live && !rewatch) {
      continue;
    }
    if (!short_lock) {
      dirty_this_frame_[b] = 0;
    }
    stat_frame_dirty_pages_ += uint32_t(xe::bit_count(dirty));
    while (live) {
      uint32_t i = xe::tzcnt(live);
      live &= live - 1;
      uint64_t bit = uint64_t(1) << i;
      uint8_t& score = dirty_score_[(b << 6) + i];
      if (dirty & bit) {
        score = std::min(uint8_t(score + 1), unwatch_score_max);
      } else if (score && decay_frame) {
        --score;
      }
      if (score >= unwatch_score) {
        unwatch_candidates_[b] |= bit;
      } else {
        unwatch_candidates_[b] &= ~bit;
      }
      if (score) {
        dirty_score_nonzero_[b] |= bit;
      } else {
        dirty_score_nonzero_[b] &= ~bit;
        rewatch |= unwatched_pages_[b] & bit;
      }
    }
    if (rewatch) {
      RewatchPages(b, rewatch);
    }
  }
  if (report) {
    const double f = double(unwatch_report_frames_);
    XELOGI(
        "ShmUnwatch: {} unwatched pages, per frame: hashes={:.0f} "
        "invalidations={:.1f} rewatches={:.1f} protect_calls={:.1f} "
        "protect_pages={:.1f} fault_invalidations={:.1f} "
        "cpu_read_uncached={:.1f}",
        unwatched_page_count_, stat_unwatch_hashes_ / f,
        stat_unwatch_invalidations_ / f, stat_unwatch_rewatches_ / f,
        stat_protect_calls_ / f, stat_protect_pages_ / f,
        report_fault_invalidations / f, stat_cpu_read_uncached_ / f);
    if (short_lock || cvars::shared_memory_snapshot_invalidation ||
        cvars::shm_fault_batch_unprotect) {
      static uint64_t batch_prev[3] = {};
      uint64_t batch_calls, batch_pages, batch_fallbacks;
      memory::GetFaultUnprotectStats(batch_calls, batch_pages, batch_fallbacks);
      XELOGI("ShmShortLock: frame_snapshot={} upload_snapshot={} "
             "per frame: dirty_pages={:.1f} upload_snapshots={:.1f} "
             "snapshot_words={:.1f} fault_batches={:.1f} "
             "fault_batch_pages={:.1f} fault_fallbacks={:.1f}",
             short_lock, cvars::shared_memory_snapshot_invalidation,
             stat_frame_dirty_pages_ / f, stat_invalidation_snapshots_ / f,
             stat_invalidation_snapshot_words_ / f,
             (batch_calls - batch_prev[0]) / f,
             (batch_pages - batch_prev[1]) / f,
             (batch_fallbacks - batch_prev[2]) / f);
      batch_prev[0] = batch_calls;
      batch_prev[1] = batch_pages;
      batch_prev[2] = batch_fallbacks;
    }
    {
      // All mprotect calls of the process (CP watches and guest write faults).
      // xe::memory::GetProtectStats totals at the previous report.
      static uint64_t protect_stats_prev_[3] = {};
      static uint64_t query_stats_prev_[2] = {};
      static uint64_t uffd_stats_prev_[5] = {};
      uint64_t uffd_stats[5];
      xe::memory::GetUffdWatchStats(uffd_stats[0], uffd_stats[1],
                                    uffd_stats[2], uffd_stats[3],
                                    uffd_stats[4]);
      uint64_t calls, bytes, ns, query_calls, query_ns;
      xe::memory::GetProtectStats(calls, bytes, ns);
      xe::memory::GetQueryProtectStats(query_calls, query_ns);
      uint32_t vmas = 0;
      if (cvars::shm_log_vma_count) {
#if defined(__ANDROID__)
        static bool write_tracking_logged = false;
        if (!write_tracking_logged) {
          write_tracking_logged = true;
          LogWriteTrackingSupport();
        }
#endif  // __ANDROID__
        if (FILE* maps = fopen("/proc/self/maps", "r")) {
          char buffer[16384];
          size_t read;
          while ((read = fread(buffer, 1, sizeof(buffer), maps))) {
            vmas += uint32_t(std::count(buffer, buffer + read, '\n'));
          }
          fclose(maps);
        }
      }
      XELOGI(
          "ShmProtect: per frame: mprotect calls={:.1f} pages={:.0f} "
          "time={:.3f} ms (avg {:.1f} us) | process mappings={} | maps "
          "queries={:.2f} time={:.3f} ms | uffd wp={:.1f} unwp={:.1f} "
          "time={:.3f} ms faults={:.1f} deferred={:.2f}",
          (calls - protect_stats_prev_[0]) / f,
          (bytes - protect_stats_prev_[1]) / 4096.0 / f,
          (ns - protect_stats_prev_[2]) / 1e6 / f,
          calls != protect_stats_prev_[0]
              ? (ns - protect_stats_prev_[2]) / 1e3 /
                    double(calls - protect_stats_prev_[0])
              : 0.0,
          vmas, (query_calls - query_stats_prev_[0]) / f,
          (query_ns - query_stats_prev_[1]) / 1e6 / f,
          (uffd_stats[0] - uffd_stats_prev_[0]) / f,
          (uffd_stats[1] - uffd_stats_prev_[1]) / f,
          (uffd_stats[2] - uffd_stats_prev_[2]) / 1e6 / f,
          (uffd_stats[3] - uffd_stats_prev_[3]) / f,
          (uffd_stats[4] - uffd_stats_prev_[4]) / f);
      {
        static uint64_t split_prev[4] = {};
        uint64_t split_calls[2], split_ns[2];
        xe::memory::GetProtectSplitStats(split_calls, split_ns);
        uint64_t probe_anonymous_ns, probe_shared_ns;
        xe::memory::ProbeProtectCost(probe_anonymous_ns, probe_shared_ns);
        auto avg_us = [](uint64_t ns, uint64_t calls) {
          return calls ? ns / 1e3 / double(calls) : 0.0;
        };
        XELOGI(
            "ShmProtectSplit: per frame: arm calls={:.1f} time={:.3f} ms (avg "
            "{:.1f} us) | drop calls={:.1f} time={:.3f} ms (avg {:.1f} us) | "
            "probe 1 page: anonymous {:.1f} us, shared memfd {:.1f} us",
            (split_calls[0] - split_prev[0]) / f,
            (split_ns[0] - split_prev[2]) / 1e6 / f,
            avg_us(split_ns[0] - split_prev[2], split_calls[0] - split_prev[0]),
            (split_calls[1] - split_prev[1]) / f,
            (split_ns[1] - split_prev[3]) / 1e6 / f,
            avg_us(split_ns[1] - split_prev[3], split_calls[1] - split_prev[1]),
            probe_anonymous_ns / 1e3, probe_shared_ns / 1e3);
        split_prev[0] = split_calls[0];
        split_prev[1] = split_calls[1];
        split_prev[2] = split_ns[0];
        split_prev[3] = split_ns[1];
      }
      std::copy(std::begin(uffd_stats), std::end(uffd_stats),
                std::begin(uffd_stats_prev_));
      query_stats_prev_[0] = query_calls;
      query_stats_prev_[1] = query_ns;
      protect_stats_prev_[0] = calls;
      protect_stats_prev_[1] = bytes;
      protect_stats_prev_[2] = ns;
    }
    stat_protect_calls_ = 0;
    stat_protect_pages_ = 0;
    stat_cpu_read_uncached_ = 0;
    stat_frame_dirty_pages_ = 0;
    stat_invalidation_snapshots_ = 0;
    stat_invalidation_snapshot_words_ = 0;
    unwatch_report_frames_ = 0;
    stat_unwatch_hashes_ = 0;
    stat_unwatch_invalidations_ = 0;
    stat_unwatch_rewatches_ = 0;
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket =
      range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

uint64_t SharedMemory::WatchCPUReadRange(uint32_t start, uint32_t length) {
  if (!memory_invalidation_callback_handle_ || !length ||
      start >= kBufferSize || length > kBufferSize - start)
    return UINT64_MAX;
  const uint32_t first = start >> page_size_log2_;
  const uint32_t last = (start + length - 1) >> page_size_log2_;
  OnCpuRangeAccess(start, length);
  auto lock = global_critical_region_.Acquire();
  if (cvars::shm_unwatch_release_exclusion && unwatch_enabled_ &&
      !unwatch_candidates_.empty()) {
    // A page the CPU rewrites every frame would be re-protected and fault
    // again every frame just to keep this cache: the caller converts it again
    // instead (no epoch, nothing published).
    for (uint32_t b = first >> 6; b <= last >> 6; ++b) {
      if (unwatch_candidates_[b] &
          coherency::Mask(b == first >> 6 ? first : 0,
                          b == last >> 6 ? last : 63)) {
        ++stat_cpu_read_uncached_;
        return UINT64_MAX;
      }
    }
  }
  // The watch-page bitmap also excludes CPU-read cached data from dynamic
  // unwatching. Do not mark shared GPU memory valid: no upload happened here.
  if (!texture_watched_pages_.empty()) {
    for (uint32_t b = first >> 6; b <= last >> 6; ++b) {
      uint64_t mask = coherency::Mask(b == first >> 6 ? first : 0,
                                      b == last >> 6 ? last : 63);
      texture_watched_pages_[b] |= mask;
      if (unwatched_pages_[b] & mask)
        RewatchPages(b, unwatched_pages_[b] & mask);
    }
  }
  ++stat_protect_calls_;
  stat_protect_pages_ += last - first + 1;
  memory().EnablePhysicalMemoryAccessCallbacks(
      first << page_size_log2_, (last - first + 1) << page_size_log2_, true,
      false);
  return request_epochs_.Load(start, length);
}

// todo: optimize, an enormous amount of cpu time (1.34%) is spent here.
bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  if (cvars::log_cp_frame_breakdown) {
    const uint64_t t0 = Clock::QueryRawNanoseconds();
    const bool result = RequestRangeImpl(start, length);
    ++stat_requests_;
    stat_request_ns_ += Clock::QueryRawNanoseconds() - t0;
    return result;
  }
  return RequestRangeImpl(start, length);
}

void SharedMemory::LogDeclaredWorkingSetFrame() {
  if (!declared_working_set_) return;
  SharedMemoryWorkingSet::Stats stats;
  if (!declared_working_set_->EndFrame(stats)) return;
  constexpr double page_mib = double(SharedMemoryWorkingSet::kPageSize) / 1048576;
  XELOGI("SharedWorkingSet: declared_requested page=4096 frames={} "
         "last={:.2f} MiB peak120={:.2f} MiB union120={:.2f} MiB "
         "all_seen={:.2f} MiB blocks2m window={} all={} blocks32m window={} "
         "all={} (incl. GPU writes)",
         stats.frames, stats.last_frame_pages * page_mib,
         stats.peak_frame_pages * page_mib, stats.window_pages * page_mib,
         stats.all_seen_pages * page_mib, stats.window_blocks_2m,
         stats.all_seen_blocks_2m, stats.window_blocks_32m,
         stats.all_seen_blocks_32m);
}

bool SharedMemory::IsPageCpuDirty(uint32_t page) const {
  return !(__atomic_load_n(&system_page_flags_valid_[page >> 6],
                           __ATOMIC_ACQUIRE) & (uint64_t(1) << (page & 63)));
}

bool SharedMemory::RequestRangeImpl(uint32_t start, uint32_t length) {
  if (!length) return true;
  if (start >= kBufferSize || length > kBufferSize - start) return false;
  if (declared_working_set_) declared_working_set_->Request(start, length);
  if (!OnRangeRequested(start, length)) return false;

  const uint32_t first = start >> page_size_log2_;
  const uint32_t last = (start + length - 1) >> page_size_log2_;
  if (unwatch_enabled_ && unwatched_page_count_ && !unwatch_skip_verify_) {
    VerifyUnwatchedPages(first, last);
  }

  SCOPE_profile_cpu_f("gpu");
  if (!EnsureHostGpuMemoryAllocated(start, length)) return false;
  bool valid = true;
  for (uint32_t b = first >> 6; b <= last >> 6; ++b) {
    uint64_t mask = coherency::Mask(b == first >> 6 ? first : 0,
                                  b == last >> 6 ? last : 63);
    if ((__atomic_load_n(&system_page_flags_valid_[b], __ATOMIC_ACQUIRE) & mask)
        != mask) {
      valid = false;
      break;
    }
  }
  bool uploaded = true;
  if (!valid) {
    ++stat_request_slow_;
    upload_ranges_.clear();
    {
      auto lock = global_critical_region_.Acquire();
      coherency::ForEachRange(first, last,
          [&](uint32_t b) { return ~system_page_flags_valid_[b]; },
          [&](uint32_t page, uint32_t count) {
            upload_ranges_.emplace_back(page, count);
          });
    }
    if (!upload_ranges_.empty()) {
      ++stat_request_uploads_;
      debug_request_caller_ = __builtin_return_address(0);
      uploaded = UploadRanges(upload_ranges_.data(),
                              uint32_t(upload_ranges_.size()));
    }
  }
  MarkRangeUsedInSubmission(start, length);
  return uploaded;
}

bool SharedMemory::RequestRanges(
    const std::pair<uint32_t, uint32_t>* byte_ranges, uint32_t count) {
  if (!count) return true;
  if (!byte_ranges) return false;
  if (count == 1) return RequestRange(byte_ranges[0].first, byte_ranges[0].second);
  // Validate before making any state changes. Union page coverage, rather
  // than merging byte bounds, so valid GPU-owned gaps are never uploaded.
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t start = byte_ranges[i].first, length = byte_ranges[i].second;
    if (length && (start >= kBufferSize || length > kBufferSize - start)) {
      return false;
    }
  }
  for (uint32_t b : requested_page_blocks_) requested_pages_[b] = 0;
  requested_page_blocks_.clear();
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t start = byte_ranges[i].first, length = byte_ranges[i].second;
    if (!length) continue;
    if (declared_working_set_) declared_working_set_->Request(start, length);
    if (!OnRangeRequested(start, length) ||
        !EnsureHostGpuMemoryAllocated(start, length)) return false;
    uint32_t a = start >> page_size_log2_;
    uint32_t z = (start + length - 1) >> page_size_log2_;
    if (unwatch_enabled_ && unwatched_page_count_ && !unwatch_skip_verify_) {
      VerifyUnwatchedPages(a, z);
    }
    for (uint32_t b = a >> 6; b <= z >> 6; ++b) {
      if (!requested_pages_[b]) requested_page_blocks_.push_back(b);
      requested_pages_[b] |= coherency::Mask(b == a >> 6 ? a : 0,
                                           b == z >> 6 ? z : 63);
    }
  }
  if (requested_page_blocks_.empty()) return true;
  if (!std::is_sorted(requested_page_blocks_.begin(), requested_page_blocks_.end())) {
    std::sort(requested_page_blocks_.begin(), requested_page_blocks_.end());
  }
  batch_upload_ranges_.clear();
  {
    auto lock = global_critical_region_.Acquire();
    for (uint32_t b : requested_page_blocks_) {
      coherency::ForEachRange(b << 6, (b << 6) + 63,
          [&](uint32_t word) {
            return requested_pages_[word] & ~system_page_flags_valid_[word];
          },
          [&](uint32_t page, uint32_t pages) {
            if (!batch_upload_ranges_.empty() &&
                batch_upload_ranges_.back().first +
                    batch_upload_ranges_.back().second == page) {
              batch_upload_ranges_.back().second += pages;
            } else {
              batch_upload_ranges_.emplace_back(page, pages);
            }
          });
    }
  }
  bool uploaded = true;
  if (!batch_upload_ranges_.empty()) {
    debug_request_caller_ = __builtin_return_address(0);
    uploaded = UploadRanges(batch_upload_ranges_.data(),
                            uint32_t(batch_upload_ranges_.size()));
  }
  // Publish access after choosing upload placement, like RequestRange.
  for (uint32_t i = 0; i < count; ++i) {
    MarkRangeUsedInSubmission(byte_ranges[i].first, byte_ranges[i].second);
  }
  return uploaded;
}

uint32_t SharedMemory::GetInvalidPageRanges(
    uint32_t start, uint32_t length, std::pair<uint32_t, uint32_t>* ranges_out,
    uint32_t max_ranges, bool& cpu_sourced_out) {
  cpu_sourced_out = false;
  if (!length || start > kBufferSize || (kBufferSize - start) < length ||
      !system_page_flags_valid_) {
    return 0;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  bool cpu_sourced = true;
  uint32_t count = 0;
  uint32_t range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint32_t block = page >> 6;
    uint64_t bit = uint64_t(1) << (page & 63);
    if ((system_page_flags_valid_and_gpu_written_ &&
         (system_page_flags_valid_and_gpu_written_[block] & bit)) ||
        (block < system_page_flags_valid_unreadable_.size() &&
         (system_page_flags_valid_unreadable_[block] & bit))) {
      cpu_sourced = false;
    }
    if (!(system_page_flags_valid_[block] & bit)) {
      if (range_start == UINT32_MAX) {
        range_start = page;
      }
    } else if (range_start != UINT32_MAX) {
      if (count >= max_ranges) {
        return UINT32_MAX;
      }
      ranges_out[count++] = std::make_pair(range_start, page - range_start);
      range_start = UINT32_MAX;
    }
  }
  if (range_start != UINT32_MAX) {
    if (count >= max_ranges) {
      return UINT32_MAX;
    }
    ranges_out[count++] = std::make_pair(range_start, page_last + 1 - range_start);
  }
  cpu_sourced_out = cpu_sourced;
  return count;
}

bool SharedMemory::IsRangeValid(uint32_t start, uint32_t length) const {
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }

  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  const uint32_t block_first = page_first >> 6;
  const uint32_t block_last = page_last >> 6;

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t valid_mask = UINT64_MAX;
    if (i == block_first) {
      valid_mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      valid_mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_[i] & valid_mask) != valid_mask) {
      return false;
    }
  }
  return true;
}

std::vector<std::pair<uint32_t, uint32_t>> SharedMemory::GetGpuWrittenRanges()
    const {
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  auto global_lock = global_critical_region_.Acquire();
  const uint32_t page_count = kBufferSize >> page_size_log2_;
  uint32_t run_first = UINT32_MAX;
  for (uint32_t page = 0; page <= page_count; ++page) {
    const bool written =
        page < page_count &&
        ((system_page_flags_valid_and_gpu_written_[page >> 6] >> (page & 63)) &
         1);
    if (written && run_first == UINT32_MAX) run_first = page;
    if (!written && run_first != UINT32_MAX) {
      runs.emplace_back(run_first << page_size_log2_,
                        (page - run_first) << page_size_log2_);
      run_first = UINT32_MAX;
    }
  }
  return runs;
}

uint32_t SharedMemory::GpuWrittenPageRanges(
    uint32_t address, std::pair<uint32_t, uint32_t>* ranges_out) const {
  const uint32_t page = address >> page_size_log2_;
  const uint32_t page_start = page << page_size_log2_;
  auto global_lock = global_critical_region_.Acquire();
  auto it = gpu_written_partial_pages_.find(page);
  if (it == gpu_written_partial_pages_.end()) {
    ranges_out[0] = {page_start, page_start + (uint32_t(1) << page_size_log2_)};
    return 1;
  }
  for (uint32_t i = 0; i < it->second.count; ++i) {
    ranges_out[i] = {page_start + it->second.ranges[i].first,
                     page_start + it->second.ranges[i].second};
  }
  return it->second.count;
}

bool SharedMemory::IsRangeGpuWritten(uint32_t start, uint32_t length) const {
  if (!length) {
    return false;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }

  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  const uint32_t block_first = page_first >> 6;
  const uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t valid_mask = UINT64_MAX;
    if (i == block_first) {
      valid_mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      valid_mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_and_gpu_written_[i] & valid_mask) != valid_mask) {
      return false;
    }
  }
  return true;
}

void SharedMemory::MarkRangeEverGpuWritten(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> kGpuWrittenHistoryPageSizeLog2;
  const uint32_t page_last =
      (start + length - 1) >> kGpuWrittenHistoryPageSizeLog2;

  auto global_lock = global_critical_region_.Acquire();
  if (!gpu_written_history_) {
    return;
  }
  const uint32_t word_first = page_first >> 6;
  const uint32_t word_last = page_last >> 6;
  for (uint32_t word = word_first; word <= word_last; ++word) {
    uint64_t mask = UINT64_MAX;
    if (word == word_first) {
      mask &= UINT64_MAX << (page_first & 63);
    }
    if (word == word_last && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    gpu_written_history_[word] |= mask;
  }
}

bool SharedMemory::IsRangeEverGpuWritten(uint32_t start, uint32_t length,
                                         bool* history_available) const {
  auto global_lock = global_critical_region_.Acquire();
  const bool history_exists = gpu_written_history_ != nullptr;
  if (history_available) {
    *history_available = history_exists;
  }
  if (!length || start >= kBufferSize || length > kBufferSize - start ||
      !history_exists) {
    return true;
  }

  const uint32_t page_first = start >> kGpuWrittenHistoryPageSizeLog2;
  const uint32_t page_last =
      (start + length - 1) >> kGpuWrittenHistoryPageSizeLog2;
  const uint32_t word_first = page_first >> 6;
  const uint32_t word_last = page_last >> 6;
  for (uint32_t word = word_first; word <= word_last; ++word) {
    uint64_t mask = UINT64_MAX;
    if (word == word_first) {
      mask &= UINT64_MAX << (page_first & 63);
    }
    if (word == word_last && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (gpu_written_history_[word] & mask) {
      return true;
    }
  }
  return false;
}

bool SharedMemory::InvalidateRangeIfReuploadable(uint32_t start,
                                                 uint32_t length) {
  if (!length || start >= kBufferSize || length > kBufferSize - start) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = page_first >> 6; i <= page_last >> 6; ++i) {
    uint64_t mask = UINT64_MAX;
    if (i == page_first >> 6) {
      mask &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == page_last >> 6 && (page_last & 63) != 63) {
      mask &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_and_gpu_written_[i] & mask) ||
        (!system_page_flags_valid_unreadable_.empty() &&
         (system_page_flags_valid_unreadable_[i] & mask))) {
      return false;
    }
  }
  MemoryInvalidationCallback(start, length, true);
  return true;
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length,
    bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();
  // Preserve the untouched bytes BEFORE a partial CPU overwrite invalidates
  // its page. The provider may only copy already completed immutable data.
  OnCpuRangeAccess(physical_address_start, length);

  if (!exact_range) {
    ++stat_fault_invalidations_;
    // One bitmap kernel for every configured block size. In particular, a
    // GPU-owned page inside a smaller/non-power-of-two block must stop excess
    // invalidation even when both endpoints are CPU-backed.
    const auto range = coherency::FaultRange(
        page_first, page_last, kBufferSize >> page_size_log2_,
        uint32_t(std::max(cvars::shm_fault_invalidation_block_pages, int32_t(1))),
        [&](uint32_t b) { return system_page_flags_valid_and_gpu_written_[b]; });
    page_first = range.first;
    page_last = range.second;
    block_first = page_first >> 6;
    block_last = page_last >> 6;
  }

  uint32_t dirty_blocks_mask = 0;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (cvars::shm_log_gpu_invalidations) {
      static uint32_t logged = 0;
      const uint64_t gpu_bits =
          system_page_flags_valid_and_gpu_written_[i] & invalidate_bits;
      if (gpu_bits && logged < cvars::shm_log_gpu_invalidations) {
        ++logged;
        XELOGW("[GPUINVAL] addr={:08X}+{} exact={} block={:08X} gpu={:016X}",
               physical_address_start, length, exact_range,
               i << (page_size_log2_ + 6), gpu_bits);
      }
    }
    __atomic_fetch_and(&system_page_flags_valid_[i], ~invalidate_bits,
                       __ATOMIC_RELEASE);
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
    system_page_flags_invalidated_in_submission_[i] |= invalidate_bits;
    if (!dirty_this_frame_.empty()) {
      dirty_this_frame_[i] |= invalidate_bits;
    }
    if (!cpu_written_since_clean_.empty()) {
      cpu_written_since_clean_[i] |= invalidate_bits;
    }
    if (cvars::shm_unwatch_release_exclusion && !texture_watched_pages_.empty()) {
      // FireWatches below removes every range watch on these pages and the
      // CPU-read epochs advance: nothing relies on their notifications any
      // more until a new watch sets the bit again.
      texture_watched_pages_[i] &= ~invalidate_bits;
    }
    if (!system_page_flags_dynamic_.empty()) {
      __atomic_fetch_or(&system_page_flags_dynamic_[i], invalidate_bits,
                        __ATOMIC_RELAXED);
      __atomic_fetch_and(&page_upload_hash_valid_[i], ~invalidate_bits,
                         __ATOMIC_RELAXED);
    }
    // Track which chunks changed for partial copying.
    dirty_blocks_mask |= (1u << (i >> 6));
  }

  request_epochs_.Invalidate(page_first << page_size_log2_,
                             (page_last - page_first + 1) << page_size_log2_);

  // GPU-written flags changed due to CPU invalidation.
  gpu_written_data_dirty_ = true;
  dirty_blocks_ |= dirty_blocks_mask;

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_,
                        (page_last - page_first + 1) << page_size_log2_);
}

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();
  assert_true(trace_download_ranges_.empty());
  assert_zero(trace_download_page_count_);

  // Invalidate the entire memory CPU->GPU memory copy so all the history
  // doesn't have to be written into every frame trace, and collect the list of
  // ranges with data modified on the GPU.

  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    // SystemPageFlagsBlock& page_flags_block = system_page_flags_[i];
    uint64_t previously_valid_block = system_page_flags_valid_[i];
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    __atomic_store_n(&system_page_flags_valid_[i], gpu_written_block,
                     __ATOMIC_RELEASE);
    // Callbacks below may reenter a consumer. Publish this block's epoch
    // before invoking any of them, rather than waiting for the whole trace.
    request_epochs_.Invalidate(i << (page_size_log2_ + 6),
                               uint32_t(64) << page_size_log2_);

    // Fire watches on the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    system_page_flags_invalidated_in_submission_[i] |= fire_watches_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t fire_watches_block_page;
      if (!xe::bit_scan_forward(fire_watches_range_start == UINT32_MAX
                                    ? fire_watches_block
                                    : fire_watches_break_block,
                                &fire_watches_block_page)) {
        break;
      }
      uint32_t fire_watches_page = (i << 6) + fire_watches_block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = fire_watches_page;
      } else {
        FireWatches(fire_watches_range_start, fire_watches_page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t fire_watches_block_mask =
          ~((uint64_t(1) << fire_watches_block_page) - 1);
      fire_watches_block &= fire_watches_block_mask;
      fire_watches_break_block &= fire_watches_block_mask;
    }

    // Add to the GPU-written ranges.
    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t gpu_written_block_page;
      if (!xe::bit_scan_forward(gpu_written_range_start == UINT32_MAX
                                    ? gpu_written_block
                                    : gpu_written_break_block,
                                &gpu_written_block_page)) {
        break;
      }
      uint32_t gpu_written_page = (i << 6) + gpu_written_block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = gpu_written_page;
      } else {
        uint32_t gpu_written_range_length =
            gpu_written_page - gpu_written_range_start;
        // Call EnsureHostGpuMemoryAllocated in case the page was marked as
        // GPU-written not as a result to an actual write to the shared memory
        // buffer, but, for instance, by resolving with resolution scaling (to a
        // separate buffer).
        if (EnsureHostGpuMemoryAllocated(
                gpu_written_range_start << page_size_log2_,
                gpu_written_range_length << page_size_log2_)) {
          trace_download_ranges_.push_back(
              std::make_pair(gpu_written_range_start << page_size_log2_,
                             gpu_written_range_length << page_size_log2_));
          trace_download_page_count_ += gpu_written_range_length;
        }
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t gpu_written_block_mask =
          ~((uint64_t(1) << gpu_written_block_page) - 1);
      gpu_written_block &= gpu_written_block_mask;
      gpu_written_break_block &= gpu_written_block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    uint32_t gpu_written_range_length = page_count - gpu_written_range_start;
    if (EnsureHostGpuMemoryAllocated(
            gpu_written_range_start << page_size_log2_,
            gpu_written_range_length << page_size_log2_)) {
      trace_download_ranges_.push_back(
          std::make_pair(gpu_written_range_start << page_size_log2_,
                         gpu_written_range_length << page_size_log2_));
      trace_download_page_count_ += gpu_written_range_length;
    }
  }
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start,
                                                uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ != UINT32_MAX && length) {
    if (start <= kBufferSize && (kBufferSize - start) >= length) {
      uint32_t page_first = start >> page_size_log2_;
      uint32_t page_last = (start + length - 1) >> page_size_log2_;
      uint32_t allocation_first = page_first << page_size_log2_ >>
                                  host_gpu_memory_sparse_granularity_log2_;
      uint32_t allocation_last = page_last << page_size_log2_ >>
                                 host_gpu_memory_sparse_granularity_log2_;
      while (true) {
        std::pair<size_t, size_t> allocation_range =
            xe::bit_range::NextUnsetRange(
                host_gpu_memory_sparse_allocated_.data(), allocation_first,
                allocation_last - allocation_first + 1);
        if (!allocation_range.second) {
          break;
        }
        if (!AllocateSparseHostGpuMemoryRange(
                uint32_t(allocation_range.first),
                uint32_t(allocation_range.second))) {
          return false;
        }
        xe::bit_range::SetRange(host_gpu_memory_sparse_allocated_.data(),
                                allocation_range.first,
                                allocation_range.second);
        ++host_gpu_memory_sparse_allocations_;
        COUNT_profile_set(
            "gpu/shared_memory/host_gpu_memory_sparse_allocations",
            host_gpu_memory_sparse_allocations_);
        host_gpu_memory_sparse_used_bytes_ +=
            uint32_t(allocation_range.second)
            << host_gpu_memory_sparse_granularity_log2_;
        COUNT_profile_set(
            "gpu/shared_memory/host_gpu_memory_sparse_used_mb",
            (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
        allocation_first =
            uint32_t(allocation_range.first + allocation_range.second);
      }
    } else {
      return false;
    }
  } else {
    return true;
  }
  return true;
}

}  // namespace gpu
}  // namespace xe
