/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_POLL_SPIN_DETECTOR_H_
#define XENIA_KERNEL_POLL_SPIN_DETECTOR_H_

#include <algorithm>
#include <cstdint>

namespace xe {
namespace kernel {

// Tells a guest thread that only waits - polling something in a tight loop
// with nothing changing - from one that polls between pieces of work
// (guest_poll_spin_park_us). One per dispatch thread, fed every "found nothing
// to do" poll of the fiber running on it: a yield that had nobody to yield
// to, a zero-timeout wait that timed out, an event pulse that released
// nobody. Pure logic (no clock, no sleeping) so it can be exercised on the
// host; the scheduler computes the state hash and does the sleeping.
class PollSpinDetector {
 public:
  struct Params {
    uint64_t ticks_per_us = 1;
    // Longest time between two polls for them to belong to one loop.
    uint32_t gap_us = 60;
    // How long an unchanged loop must have gone on before it is put to sleep
    // (the detector raises this by itself for waits that turn out short).
    uint32_t base_after_us = 300;
    // Sleeping is configured at all.
    bool park_enabled = true;
    // The frame has spare time: sleeps are taken from idle time.
    bool slack = false;
    // A sleep may last at most 1/wait_share of how long the wait has already
    // gone on, what the host adds to every short sleep (timer slack and
    // wake-up) included: whatever the wait is for, noticing it late then
    // costs at most that share of its length. A chain of waits (loading) is
    // slowed by no more than that either.
    uint32_t wait_share = 8;
    uint32_t sleep_overhead_us = 50;
    // Shorter sleeps than this are not worth a system call.
    uint32_t min_park_us = 20;
    // Polls this close together belong to a wait whatever the state looks
    // like (0 = off): a loop that counts its turns or reads the time changes
    // its registers at every poll without doing anything, and there is no
    // room for work worth the name in so short a gap.
    uint32_t tight_gap_us = 0;
  };

  // What the last poll looked like (for the statistics).
  enum class Kind : uint8_t {
    // First of a loop: another fiber, or one that ran in between.
    kFresh,
    // Same fiber, nothing else ran, but longer than gap_us since the last.
    kGapTooLong,
    // In time, with a state not seen before in this run (and not within
    // tight_gap_us).
    kNewState,
    // In time with a known state.
    kSameState,
    // New state within tight_gap_us: counted as the same wait.
    kTight,
  };

  enum class Action {
    kNone,
    // Sleep one park of at most park_limit_us() (then call NoteParked).
    kPark,
    // A loop that only pulses an event nobody waits on: hold it to the pace
    // the host pulse's own sleep used to give it (then call NoteSlept).
    kThrottlePulse,
  };

  // The spin before the sleeps never grows past this.
  static constexpr uint32_t kMaxAfterUs = 5000;
  // A job kick is a short burst of pulses; a wait loop that pulses goes on.
  static constexpr uint32_t kPulseRunThrottle = 48;

  // |fiber|: who polls (any stable identity). |hash|: fingerprint of where
  // the loop keeps its state - unchanged between two polls means nothing was
  // done in between. |other_ran|: another fiber ran on this dispatch thread
  // since the previous poll. |pulse| + |key|: the poll is a pulse of that
  // event. |now|: host ticks.
  Action OnPoll(const void* fiber, uint64_t hash, bool other_ran, bool pulse,
                uint32_t key, uint64_t now, const Params& params) {
    // The same loop: at most gap_us since the previous poll of this fiber,
    // with nothing else having run on this dispatch thread in between.
    const bool same_loop = owner_ == fiber && !other_ran;
    const bool continuing =
        same_loop && now - last_tick_ <=
                         uint64_t(std::max(params.gap_us, uint32_t(1))) *
                             params.ticks_per_us;
    bool repeated = false;
    last_kind_ = same_loop ? Kind::kGapTooLong : Kind::kFresh;
    if (continuing) {
      last_kind_ = Kind::kNewState;
      const uint32_t known = std::min(hash_count_, uint32_t(4));
      for (uint32_t i = 0; i < known; ++i) {
        if (hashes_[i] == hash) {
          repeated = true;
          last_kind_ = Kind::kSameState;
          break;
        }
      }
      if (!repeated && hash_count_ &&
          now - last_tick_ <=
              uint64_t(params.tight_gap_us) * params.ticks_per_us) {
        repeated = true;
        last_kind_ = Kind::kTight;
      }
    }
    if (pulse) {
      pulse_run_ = continuing && pulse_key_ == key ? pulse_run_ + 1 : 1;
      pulse_key_ = key;
    }
    const bool long_pulse_run = pulse && pulse_run_ >= kPulseRunThrottle;
    if (!repeated) {
      // The run of unchanged polls, if there was one, is over: what was
      // waited for happened, or the loop moved on. Adapt the spin to how the
      // wait went, like an adaptive lock: a wait that ended within a sleep or
      // two of the sleeps starting only paid their wake-up latency (a chain
      // of short waits, as in loading, would pay it at every link), so spin
      // longer before sleeping next time; a wait that slept many times can
      // start sooner.
      if (parks_) {
        const uint32_t current = std::max(after_us_, params.base_after_us);
        if (parks_ <= 2) {
          after_us_ = std::min(current * 2, kMaxAfterUs);
        } else if (parks_ >= 8) {
          after_us_ = std::max(current / 2, params.base_after_us);
        }
        parks_ = 0;
      }
      if (!continuing) {
        owner_ = fiber;
        hash_count_ = 0;
      }
      // A poll not seen in this run (a loop alternates between a few: its
      // wait predicate, the yield of its job search): the spin, if it is
      // one, is counted from here.
      hashes_[hash_count_++ & 3] = hash;
      start_tick_ = now;
      last_tick_ = now;
      return long_pulse_run ? Action::kThrottlePulse : Action::kNone;
    }
    last_tick_ = now;
    if (now - start_tick_ <
        uint64_t(std::max(after_us_, params.base_after_us)) *
            params.ticks_per_us) {
      return long_pulse_run ? Action::kThrottlePulse : Action::kNone;
    }
    if (!params.park_enabled || !params.slack) {
      // No slack in the frame: the loop keeps its full speed, except one
      // that only pulses.
      return pulse ? Action::kThrottlePulse : Action::kNone;
    }
    const uint64_t waited_us = (now - start_tick_) / params.ticks_per_us;
    const uint64_t share_us =
        waited_us / std::max(params.wait_share, uint32_t(1));
    if (share_us < uint64_t(params.sleep_overhead_us) + params.min_park_us) {
      // Too young a wait for even the shortest sleep.
      return long_pulse_run ? Action::kThrottlePulse : Action::kNone;
    }
    park_limit_us_ =
        uint32_t(std::min(share_us - params.sleep_overhead_us,
                          uint64_t(UINT32_MAX)));
    return Action::kPark;
  }

  // After kPark: the longest sleep to take now, in microseconds.
  uint32_t park_limit_us() const { return park_limit_us_; }

  // A park was taken; |now| after it.
  void NoteParked(uint64_t now) {
    ++parks_;
    last_tick_ = now;
  }

  // The polling fiber slept |ticks| for a reason that is not its loop doing
  // something else (a throttle, a sleep inside the kernel call it polls
  // with): doesn't break the loop.
  void NoteSlept(uint64_t ticks) { last_tick_ += ticks; }

  const void* owner() const { return owner_; }
  uint32_t parks() const { return parks_; }
  // 0 until a wait has taught the detector otherwise.
  uint32_t learned_after_us() const { return after_us_; }
  uint32_t pulse_run() const { return pulse_run_; }
  Kind last_kind() const { return last_kind_; }

 private:
  const void* owner_ = nullptr;
  uint64_t hashes_[4] = {};
  uint32_t hash_count_ = 0;
  uint64_t start_tick_ = 0;
  uint64_t last_tick_ = 0;
  // Sleeps taken in the current unchanged run of polls.
  uint32_t parks_ = 0;
  // Learned spin length before the sleeps start (0 = the configured one).
  uint32_t after_us_ = 0;
  // Pulses that released nobody, back to back on one event.
  uint32_t pulse_key_ = 0;
  uint32_t pulse_run_ = 0;
  uint32_t park_limit_us_ = 0;
  Kind last_kind_ = Kind::kFresh;
};

// Tells a guest thread that spins in place - whatever it spins on, kernel
// call or not - by sampling where it is at each of its quantum ends (about
// once a millisecond while it keeps running): working code never shows the
// same full state twice, a wait loop shows a few over and over
// (guest_spin_sample_park_us). One per guest thread. Pure logic.
class SpinSampleDetector {
 public:
  struct Result {
    // Found in place now and at three or more of its last 16 quantum ends.
    bool spinning;
    // The longest sleep to take now, in microseconds; 0 = none. Set once the
    // thread has been in place for two quantum ends in a row, and a share of
    // how long that has been going on.
    uint32_t park_limit_us;
  };

  // |fingerprint|: hash of the thread's whole state at the quantum end.
  // |quantum_us|: time between two quantum ends of a running thread.
  // |wait_share|, |sleep_overhead_us|, |min_park_us|: as in
  // PollSpinDetector::Params.
  Result OnQuantumEnd(uint64_t fingerprint, uint32_t quantum_us,
                      uint32_t wait_share, uint32_t sleep_overhead_us,
                      uint32_t min_park_us) {
    bool seen = false;
    for (uint64_t state : states_) {
      if (state == fingerprint) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      // Only new states are stored, so a loop's few states stay in the ring
      // however long it spins, while working code just keeps overwriting it.
      states_[next_++ & 15] = fingerprint;
    }
    history_ = uint16_t((history_ << 1) | (seen ? 1 : 0));
    uint32_t hits = 0;
    for (uint32_t bits = history_; bits; bits &= bits - 1) {
      ++hits;
    }
    // Quantum ends in a row that found it in place: how long this wait has
    // lasted, at least.
    uint32_t run = 0;
    for (uint32_t bits = history_; bits & 1; bits >>= 1) {
      ++run;
    }
    Result result = {seen && hits >= 3, 0};
    if (run >= 2) {
      const uint64_t share_us = uint64_t(run) * quantum_us /
                                std::max(wait_share, uint32_t(1));
      if (share_us >= uint64_t(sleep_overhead_us) + min_park_us) {
        result.park_limit_us = uint32_t(share_us - sleep_overhead_us);
      }
    }
    return result;
  }

 private:
  uint64_t states_[16] = {};
  uint16_t history_ = 0;
  uint8_t next_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_POLL_SPIN_DETECTOR_H_
