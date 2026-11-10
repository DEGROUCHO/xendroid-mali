/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/mutex.h"
#include "xenia/base/cp_wait_stats.h"

DEFINE_int32(global_mutex_spin_us, 40,
             "Spin up to this many microseconds on a contended global mutex "
             "before sleeping on the futex. 0: 128 quick tries only. The "
             "holds are short, while a futex sleep costs the waiter a "
             "scheduler wake-up (50-60 us at the median on Android): Black "
             "Ops' two frame-critical guest threads slept on this lock 600-800 "
             "times a second each.",
             "CPU");
UPDATE_from_int32(global_mutex_spin_us, 2026, 10, 11, 17, 0);
#if XE_PLATFORM_WIN32 == 1
#include "xenia/base/platform_win.h"
#elif XE_PLATFORM_LINUX == 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace xe {
#if XE_PLATFORM_WIN32 == 1 && XE_ENABLE_FAST_WIN32_MUTEX == 1

// xe_global_mutex: recursive mutex via SRWLOCK
void xe_global_mutex::lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    ++recursion_count_;
    return;
  }
  AcquireSRWLockExclusive(&srwlock_);
  owner_thread_ = self;
  recursion_count_ = 1;
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ == 0) {
    owner_thread_ = 0;
    ReleaseSRWLockExclusive(&srwlock_);
  }
}

bool xe_global_mutex::try_lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    ++recursion_count_;
    return true;
  }
  if (TryAcquireSRWLockExclusive(&srwlock_)) {
    owner_thread_ = self;
    recursion_count_ = 1;
    return true;
  }
  return false;
}

bool xe_global_mutex::is_held_by_current_thread() const {
  return owner_thread_ == GetCurrentThreadId();
}

// xe_fast_mutex: non-recursive mutex via SRWLOCK
void xe_fast_mutex::lock() {
  DWORD self = GetCurrentThreadId();
  if (owner_thread_ == self) {
    assert_always("xe_fast_mutex: recursive lock detected");
  }
  AcquireSRWLockExclusive(&srwlock_);
  owner_thread_ = self;
}

void xe_fast_mutex::unlock() {
  owner_thread_ = 0;
  ReleaseSRWLockExclusive(&srwlock_);
}

bool xe_fast_mutex::try_lock() {
  if (TryAcquireSRWLockExclusive(&srwlock_)) {
    owner_thread_ = GetCurrentThreadId();
    return true;
  }
  return false;
}
#elif XE_PLATFORM_LINUX == 1 && XE_ENABLE_FAST_LINUX_MUTEX == 1

namespace {

inline int futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) {
  return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr,
                 nullptr, 0);
}

inline int futex_wake(std::atomic<uint32_t>* addr, int count) {
  return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr,
                 0);
}

inline pid_t gettid() {
  // Cached per thread: this is on the xe_global_mutex lock/unlock fast path
  // (taken per kernel call and per GPU packet), and the raw syscall showed up
  // as ~2% of the GPU command processor thread in profiles.
  static thread_local const pid_t tid =
      static_cast<pid_t>(syscall(SYS_gettid));
  return tid;
}

}  // namespace

// xe_global_mutex implementation (recursive)
void xe_global_mutex::NoteAcquired(void* site) {
  if (cvars::log_cp_wait_stats) {
    hold_begin_ns_ = cp_wait_stats::Now();
    hold_site_ = site;
  }
}

void xe_global_mutex::lock() {
  void* site = __builtin_return_address(0);
  pid_t self = gettid();

  // Fast path: check if we already own it (recursive lock)
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return;
  }

  // Try to acquire with a simple CAS first (uncontended case)
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    NoteAcquired(site);
    return;
  }

  lock_slow(site);
}

void xe_global_mutex::lock_slow(void* site) {
  cp_wait_stats::MutexScope contention_wait;
  pid_t self = gettid();

  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
    SpinPause();
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      NoteAcquired(site);
      return;
    }
  }

  // Timed spin (global_mutex_spin_us): most holds are short, while a futex
  // sleep costs a scheduler wakeup per contention (hundreds of microseconds
  // on Android), which the GPU command processor pays several times a frame.
  if (const int32_t spin_us = cvars::global_mutex_spin_us; spin_us > 0) {
    const uint64_t deadline =
        cp_wait_stats::Now() + uint64_t(spin_us) * UINT64_C(1000);
    for (uint32_t n = 0;; ++n) {
      if (state_.load(std::memory_order_relaxed) == 0) {
        uint32_t expected = 0;
        if (state_.compare_exchange_strong(expected, 1,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
          owner_.store(self, std::memory_order_relaxed);
          recursion_count_ = 1;
          NoteAcquired(site);
          return;
        }
      }
#if XE_ARCH_ARM64 == 1
      // yield is a no-op on cores without SMT (phones); isb actually pauses.
      __asm__ volatile("isb sy" ::: "memory");
#endif
      if ((n & 63) == 63 && cp_wait_stats::Now() >= deadline) {
        break;
      }
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      NoteAcquired(site);
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      NoteAcquired(site);
      return;
    }
  }
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ > 0) {
    return;  // Still have recursive locks
  }

  if (hold_begin_ns_) {
    cp_wait_stats::NoteHold(hold_site_, cp_wait_stats::Now() - hold_begin_ns_);
    hold_begin_ns_ = 0;
  }
  owner_.store(0, std::memory_order_relaxed);

  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    if (cvars::log_cp_wait_stats && cp_wait_stats::is_cp) {
      cp_wait_stats::cp_wakes.fetch_add(1, std::memory_order_relaxed);
    }
    futex_wake(&state_, 1);
  }
}

bool xe_global_mutex::try_lock() {
  void* site = __builtin_return_address(0);
  pid_t self = gettid();

  // Check for recursive lock
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }

  uint32_t expected = 0;
  if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    NoteAcquired(site);
    return true;
  }
  return false;
}

bool xe_global_mutex::is_held_by_current_thread() const {
  return owner_.load(std::memory_order_relaxed) == gettid();
}

// xe_fast_mutex implementation (non-recursive)
void xe_fast_mutex::lock() {
  // Fast path: uncontended
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    return;
  }

  lock_slow();
}

void xe_fast_mutex::lock_slow() {
  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
    SpinPause();
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }
}

void xe_fast_mutex::unlock() {
  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool xe_fast_mutex::try_lock() {
  uint32_t expected = 0;
  return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed);
}

#endif
global_mutex_type& global_critical_region::mutex() {
  static global_mutex_type global_mutex;
  return global_mutex;
}

bool global_critical_region::is_held_by_current_thread() {
  return mutex().is_held_by_current_thread();
}

}  // namespace xe
