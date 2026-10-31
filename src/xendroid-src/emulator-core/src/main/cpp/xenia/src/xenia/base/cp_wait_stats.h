/**
 * Diagnostic: where the GPU command processor (CP) thread blocks, and global
 * mutex contention, logged per guest frame with log_cp_wait_stats.
 */
#ifndef XENIA_BASE_CP_WAIT_STATS_H_
#define XENIA_BASE_CP_WAIT_STATS_H_

#include <time.h>

#include <atomic>
#include <cstdint>

#include "xenia/base/cvar.h"

DECLARE_bool(log_cp_wait_stats);

namespace xe::cp_wait_stats {

enum Category {
  kRing,       // Ring buffer empty - waiting for the guest to submit.
  kFence,      // GPU submission fences (frames in flight).
  kPresenter,  // Presenter mutex, swapchain acquire, presenter fences.
  kPipeline,   // Waiting for pipeline creation threads.
  kEncode,     // Waiting for the stream encode thread to go idle.
  kPause,
  kThrottle,   // Frame limiter sleep.
  kPending,    // pending_fns_mutex_.
  kRefreshTotal,  // RefreshGuestOutput inclusive (not a wait).
  kCount
};

// Who asked for a kFence wait (log_cp_wait_stats breakdown).
enum FenceSite {
  kFenceOther,
  kFenceRegMem,   // WAIT_REG_MEM PrepareForWait.
  kFenceIdle,     // Ring empty PrepareForWait.
  kFenceCpFault,  // CP access to GPU-leased zero-copy memory.
  kFenceFrame,    // Frame-end frames-in-flight throttle.
  kFenceSiteCount
};
inline thread_local unsigned fence_site = kFenceOther;
inline thread_local uint64_t fence_site_ns[kFenceSiteCount] = {};
class FenceSiteScope {
 public:
  explicit FenceSiteScope(FenceSite site) : previous_(fence_site) {
    fence_site = site;
  }
  ~FenceSiteScope() { fence_site = previous_; }

 private:
  unsigned previous_;
};

inline thread_local bool is_cp = false;
inline thread_local unsigned presenter_depth = 0;
inline thread_local uint64_t ns[kCount] = {};
// Per-frame event counts for log_frame_spikes_ms (any thread may add).
inline std::atomic<uint64_t> textures_created{0};
inline std::atomic<uint64_t> texture_bytes_created{0};
inline std::atomic<uint64_t> pipelines_created{0};
inline std::atomic<uint64_t> upload_bytes{0};
// [0] = CP thread, [1] = all other threads.
inline std::atomic<uint64_t> mutex_count[2] = {};
inline std::atomic<uint64_t> mutex_ns[2] = {};
inline std::atomic<uint64_t> cp_wakes{0};
// What the command processor thread is blocked on right now, for the GPU
// heartbeat: Category + 1, kMutexWait for the global mutex, 0 when running.
constexpr int kMutexWait = 100;
inline std::atomic<int> cp_current_wait{0};

// Global mutex holds of >= 50 us by acquiring call site (bit 0 = CP thread).
struct HoldSite {
  std::atomic<uintptr_t> site{0};
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> ns{0};
};
constexpr uint32_t kHoldSiteCount = 128;
inline HoldSite hold_sites[kHoldSiteCount];
inline void NoteHold(void* site, uint64_t held_ns) {
  if (held_ns < 50000) {
    return;
  }
  const uintptr_t key = uintptr_t(site) | (is_cp ? 1 : 0);
  const uint32_t hash = uint32_t((key >> 2) * 2654435761u);
  for (uint32_t i = 0; i < kHoldSiteCount; ++i) {
    HoldSite& entry = hold_sites[(hash + i) & (kHoldSiteCount - 1)];
    uintptr_t current = entry.site.load(std::memory_order_relaxed);
    if (current == 0 &&
        !entry.site.compare_exchange_strong(current, key,
                                            std::memory_order_relaxed)) {
      // Lost the race - current now holds the winner's key.
    } else if (current == 0) {
      current = key;
    }
    if (current != key) {
      continue;
    }
    entry.count.fetch_add(1, std::memory_order_relaxed);
    entry.ns.fetch_add(held_ns, std::memory_order_relaxed);
    return;
  }
}

inline uint64_t Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

class Scope {
 public:
  explicit Scope(Category category) : category_(category) {
    if (is_cp) {
      cp_current_wait.store(int(category) + 1, std::memory_order_relaxed);
    }
    if (cvars::log_cp_wait_stats && is_cp) {
      // Presenter fences must not also appear as CP submission fences.
      if (category_ == kFence && presenter_depth) {
        category_ = kPresenter;
      }
      begin_ = Now();
    }
  }
  ~Scope() { Stop(); }
  void Stop() {
    if (is_cp) {
      cp_current_wait.store(0, std::memory_order_relaxed);
    }
    if (begin_) {
      const uint64_t elapsed = Now() - begin_;
      ns[category_] += elapsed;
      if (category_ == kFence) {
        fence_site_ns[fence_site] += elapsed;
      }
      begin_ = 0;
    }
  }

 private:
  Category category_;
  uint64_t begin_ = 0;
};

class MutexScope {
 public:
  MutexScope() {
    if (is_cp) {
      cp_current_wait.store(kMutexWait, std::memory_order_relaxed);
    }
    if (cvars::log_cp_wait_stats) {
      bucket_ = is_cp ? 0 : 1;
      begin_ = Now();
    }
  }
  ~MutexScope() {
    if (is_cp) {
      cp_current_wait.store(0, std::memory_order_relaxed);
    }
    if (begin_) {
      mutex_count[bucket_].fetch_add(1, std::memory_order_relaxed);
      mutex_ns[bucket_].fetch_add(Now() - begin_, std::memory_order_relaxed);
    }
  }

 private:
  unsigned bucket_ = 0;
  uint64_t begin_ = 0;
};

class PresenterScope {
 public:
  PresenterScope() {
    if (cvars::log_cp_wait_stats && is_cp) {
      ++presenter_depth;
      active_ = true;
    }
  }
  ~PresenterScope() {
    if (active_) {
      --presenter_depth;
    }
  }

 private:
  bool active_ = false;
};

// Both called by the CP thread once per guest frame (at swap).
void Frame();
void UpdateNice();
void ApplyThreadPlacement();

}  // namespace xe::cp_wait_stats

#endif  // XENIA_BASE_CP_WAIT_STATS_H_
