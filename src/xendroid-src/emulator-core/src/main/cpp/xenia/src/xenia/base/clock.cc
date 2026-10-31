/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"

#if defined(_WIN32)

#include "xenia/base/platform_win.h"

#endif

DEFINE_bool(clock_no_scaling, false,
            "Disable scaling code. Time management and locking is bypassed. "
            "Guest system time is directly pulled from host.",
            "CPU");
DEFINE_bool(clock_source_raw, false,
            "On x64, Use the RDTSC instruction as the time source. Requires "
            "invariant TSC. "
            "On a64, Use the CNTVCT_EL0 register as the time source",
            "CPU");
DEFINE_bool(clock_fast_guest_ticks, true,
            "Guest tick count (mftb, KeQueryPerformanceCounter and the guest "
            "system time derived from it) straight from the hardware counter "
            "(CNTVCT_EL0 on a64) with a fixed-point ratio, without the mutex "
            "and the clock syscall of the default path. Continuous across "
            "guest tick frequency and time scalar changes. Ignored with "
            "clock_no_scaling.",
            "CPU");

namespace xe {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks.
uint64_t last_guest_tick_count_ = 0;
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();

using tick_mutex_type = std::mutex;

// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
// std::mutex tick_mutex_;
static tick_mutex_type tick_mutex_;

#if XE_CLOCK_RAW_AVAILABLE
// clock_fast_guest_ticks: guest ticks = tick_base + (raw - raw_base) * ratio,
// the ratio in 32.32 fixed point. The state is immutable, swapped (and the old
// one intentionally leaked) only when the guest frequency or time scalar
// changes, so the hot path is one load and a multiply.
struct FastGuestClockState {
  uint64_t raw_base;
  uint64_t tick_base;
  uint64_t ratio_32_32;
};
std::atomic<const FastGuestClockState*> fast_guest_clock_state_{nullptr};
std::mutex fast_guest_clock_rebase_mutex_;

inline uint64_t FastGuestTicks(const FastGuestClockState* state, uint64_t raw) {
  // Another thread may have rebased with a later counter value than this one.
  const uint64_t delta = raw > state->raw_base ? raw - state->raw_base : 0;
  return state->tick_base +
         uint64_t((unsigned __int128)delta * state->ratio_32_32 >> 32);
}

void RebaseFastGuestClock() {
  std::lock_guard<std::mutex> lock(fast_guest_clock_rebase_mutex_);
  const uint64_t raw = Clock::host_tick_count_raw();
  const FastGuestClockState* old_state =
      fast_guest_clock_state_.load(std::memory_order_acquire);
  auto* state = new FastGuestClockState;
  state->raw_base = raw;
  state->tick_base = old_state ? FastGuestTicks(old_state, raw) : 0;
  const double ratio = double(guest_tick_frequency_) * guest_time_scalar_ /
                       double(Clock::host_tick_frequency_raw());
  state->ratio_32_32 = uint64_t(ratio * 4294967296.0);
  fast_guest_clock_state_.store(state, std::memory_order_release);
}
#endif

void RecomputeGuestTickScalar() {
#if XE_CLOCK_RAW_AVAILABLE
  RebaseFastGuestClock();
#endif
  // Create a rational number with numerator (first) and denominator (second)
  auto frac =
      std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_fast_guest_ticks && !cvars::clock_no_scaling) {
    const FastGuestClockState* state =
        fast_guest_clock_state_.load(std::memory_order_acquire);
    if (!state) {
      RebaseFastGuestClock();
      state = fast_guest_clock_state_.load(std::memory_order_acquire);
    }
    return FastGuestTicks(state, Clock::host_tick_count_raw());
  }
#endif
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (cvars::clock_no_scaling) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  std::unique_lock<tick_mutex_type> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta = host_tick_count > last_host_tick_count_
                                   ? host_tick_count - last_host_tick_count_
                                   : 0;
    last_host_tick_count_ = host_tick_count;
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    last_guest_tick_count_ += guest_tick_delta;
    return last_guest_tick_count_;
  } else {
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_;
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}
uint64_t Clock::QueryHostTickFrequency() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if XE_CLOCK_RAW_AVAILABLE
  if (cvars::clock_source_raw) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() { return guest_time_scalar_; }

void Clock::set_guest_time_scalar(double scalar) {
  if (cvars::clock_no_scaling) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<tick_mutex_type> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() { return guest_tick_frequency_; }

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() { return guest_system_time_base_; }

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t* Clock::GetGuestTickCountPointer() { return &last_guest_tick_count_; }
uint64_t Clock::QueryGuestSystemTime() {
  if (cvars::clock_no_scaling) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint64_t Clock::QueryGuestInterruptTime() {
  return Clock::QueryHostInterruptTime();
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(
      std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                         std::numeric_limits<uint32_t>::max()));
}

// Candidate additive Clock API. No QueryGuestTickCount hot-path change.
// Call only with all guest readers and timestamp writers quiesced.
bool Clock::CaptureRuntimeSnapshot(uint64_t& ticks, uint64_t& system_time) {
  if (cvars::clock_no_scaling || !guest_tick_frequency_) return false;
  ticks = QueryGuestTickCount();
  uint64_t numerator=10000000, denominator=guest_tick_frequency_;
  reduce_fraction(numerator,denominator);
  system_time=guest_system_time_base_+uint64_t(
      (static_cast<unsigned __int128>(ticks)*numerator)/denominator);
  return true;
}

bool Clock::RestoreRuntimeSnapshot(uint64_t ticks, uint64_t system_time) {
  if (cvars::clock_no_scaling || !guest_tick_frequency_) return false;
  // Mutating these bases is forbidden while a guest reader runs. Rebuilding
  // immutable fast-clock state preserves its existing ratio and hot path.
#if XE_CLOCK_RAW_AVAILABLE
  std::lock_guard<std::mutex> rebase(fast_guest_clock_rebase_mutex_);
  auto* prior=fast_guest_clock_state_.load(std::memory_order_acquire);
  if (!prior) return false;
  auto* state=new FastGuestClockState{host_tick_count_raw(),ticks,
                                     prior->ratio_32_32};
  fast_guest_clock_state_.store(state,std::memory_order_release);
#endif
  {
    std::lock_guard<tick_mutex_type> lock(tick_mutex_);
    last_guest_tick_count_=ticks;
    last_host_tick_count_=QueryHostTickCount();
  }
  uint64_t numerator=10000000,denominator=guest_tick_frequency_;
  reduce_fraction(numerator,denominator);
  const uint64_t offset=uint64_t(
      (static_cast<unsigned __int128>(ticks)*numerator)/denominator);
  // FILETIME arithmetic is modulo uint64_t, matching the existing base.
  guest_system_time_base_=system_time-offset;
  return true;
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (cvars::clock_no_scaling) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (cvars::clock_no_scaling) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms = static_cast<uint64_t>(
      (static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (cvars::clock_no_scaling) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time =
        static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time = static_cast<uint64_t>(
        (static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (cvars::clock_no_scaling) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) *
                                              guest_time_scalar_);
  uint64_t scaled_usec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) *
                                               guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

uint64_t Clock::QueryRawNanoseconds() {
#if XE_CLOCK_RAW_AVAILABLE
  static const uint64_t ns_per_tick_32_32 =
      uint64_t((static_cast<unsigned __int128>(1000000000u) << 32) /
               host_tick_frequency_raw());
  return uint64_t(
      (static_cast<unsigned __int128>(host_tick_count_raw()) *
       ns_per_tick_32_32) >>
      32);
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}

}  // namespace xe
