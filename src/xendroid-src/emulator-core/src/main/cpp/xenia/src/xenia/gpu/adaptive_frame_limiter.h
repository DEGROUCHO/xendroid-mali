/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_ADAPTIVE_FRAME_LIMITER_H_
#define XENIA_GPU_ADAPTIVE_FRAME_LIMITER_H_

#include <algorithm>
#include <cstdint>

namespace xe {
namespace gpu {

// Decides the rate the frame limiter paces a game at (framerate_limit_adaptive).
//
// A game that can't hold the limit runs every core and the GPU flat out for an
// uneven frame rate in between. The limit is stepped down while the game
// doesn't deliver it - to an intermediate rate if one is configured and the
// game holds it with margin, else to half - and back up when the work of a
// frame fits the faster period with margin: an even pace, and idle time the
// governors turn into lower clocks.
//
// Below the full limit the power-saving mechanisms that live on a frame's spare
// time (guest thread sleeps, threads moved to slow cores) stretch the work of
// a frame towards its period, so the measured work no longer says whether a
// faster rate would hold. Nor does it for a game that waits for the vertical
// sync by itself: its wait counts as work, and fills whatever period it is
// given. So when the work doesn't say yes by itself, the limiter asks for a
// probe now and then: a window of about a second during which those
// mechanisms must stand down (Result::probe) and the game is paced at the
// full limit again, to see what it delivers. Two probes in a row must say
// yes; probes that say no are spaced further and further apart.
//
// Pure logic (no clock, no logging) so it can be exercised on the host.
class AdaptiveFrameLimiter {
 public:
  enum class Change { kNone, kLowered, kRaised };
  struct Result {
    // Rate to pace at from now on.
    uint32_t limit;
    Change change;
    // The rate before |change|.
    uint32_t previous_limit;
    // From now until the next call that says otherwise, everything that
    // trades frame time for power must stand down: the limiter is measuring
    // what the game can do.
    bool probe;
  };

  // 0 = full limit, 1 = intermediate, 2 = half.
  uint32_t level() const { return level_; }

  void Reset() { *this = AdaptiveFrameLimiter(); }

  // Call once per presented frame.
  // |now_ticks|: any monotonic time in |tick_freq| ticks per second.
  // |work_ticks|: what producing this frame took, waits for its slot excluded.
  // |full_limit|: the configured limit. |mid_limit|: the intermediate rate, 0
  // or out of (half, full) for none.
  Result OnFrame(uint64_t now_ticks, uint64_t work_ticks, uint64_t tick_freq,
                 uint32_t full_limit, uint32_t mid_limit) {
    const uint32_t half_limit = full_limit / 2;
    if (mid_limit <= half_limit || mid_limit >= full_limit) {
      mid_limit = 0;
    }
    if (level_ == 1 && !mid_limit) {
      // The intermediate step was switched off while in use.
      level_ = 2;
    }
    const auto level_limit = [&](uint32_t level) {
      return level == 0 ? full_limit : (level == 1 ? mid_limit : half_limit);
    };
    // The window in progress is a probe: paced at the full limit.
    const bool probing = probe_phase_ != 0;
    Result result = {probing ? full_limit : level_limit(level_), Change::kNone,
                     level_limit(level_), probing};
    const uint64_t full_period_ticks = tick_freq / full_limit;
    ++window_frames_;
    if (probe_settle_frames_) {
      // Threads are still waking up and moving back as a probe starts.
      --probe_settle_frames_;
    } else if (work_ticks < full_period_ticks * 8) {
      // (A frame that took many periods is a hitch or a load, not the pace.)
      work_ticks_sum_ += work_ticks;
      ++work_frames_;
    }
    if (!window_start_ticks_) {
      window_start_ticks_ = now_ticks;
    }
    if (now_ticks - window_start_ticks_ < tick_freq || !work_frames_) {
      return result;
    }
    // A window of at least a second has ended.
    const uint64_t work_average = work_ticks_sum_ / work_frames_;
    const uint64_t window_ticks = now_ticks - window_start_ticks_;
    // Frames delivered in the window against a share of a rate. Counted in
    // frames, not in work time: a game locked to the limit by its own vertical
    // sync wait fills the whole period without being late.
    const auto delivers = [&](uint32_t limit, uint32_t percent) {
      return uint64_t(window_frames_) * tick_freq * 100 >=
             uint64_t(limit) * window_ticks * percent;
    };
    // The full limit tolerates frames down to 87% of it (an uneven 52-60 still
    // beats an even 30). The intermediate rate is only worth keeping while it
    // is nearly even: under 94% of it the half rate looks better.
    const uint32_t keep_percent = level_ == 1 ? 94 : 87;
    // Down: the current rate not delivered for two seconds in a row, or one
    // right after the limit was raised - a game that falls back under it at
    // once (leaving a menu or a film) must not run uneven for long.
    // (A probe window ran at another rate: it says nothing about this one.)
    if (probing) {
      down_votes_ = 0;
    } else if (level_ < 2 && !delivers(result.limit, keep_percent)) {
      if (++down_votes_ >= (recently_raised_ ? 1u : 2u)) {
        const bool raise_failed = recently_raised_;
        if (raise_failed) {
          // The raise didn't hold: ask for a longer calm before the next.
          up_votes_needed_ = std::min(up_votes_needed_ * 2, uint32_t(48));
        }
        // To the intermediate rate only if the game runs clearly faster than
        // it already.
        level_ = level_ == 0 && mid_limit && delivers(mid_limit, 105) ? 1 : 2;
        recently_raised_ = false;
        down_votes_ = 0;
        up_votes_ = 0;
        const uint32_t interval = probe_interval_;
        ResetProbing();
        if (raise_failed) {
          probe_interval_ = std::min(interval * 2, kProbeIntervalMax);
          windows_until_probe_ = probe_interval_;
        }
        result.limit = level_limit(level_);
        result.change = Change::kLowered;
      }
    } else {
      down_votes_ = 0;
    }
    // Up: a frame's work takes less than 80% of a faster rate's period for
    // six seconds in a row (more after a raise that didn't hold) - or, when
    // the stretched work doesn't show it, less than 90% of it in two probes
    // in a row. Straight to the full limit when it fits that.
    if (level_ > 0 && result.change == Change::kNone) {
      const bool probe_measured = probe_phase_ == 2;
      const uint32_t fit_percent = probe_measured ? 90 : 80;
      uint32_t up_level = level_;
      if (work_average * 100 < full_period_ticks * fit_percent) {
        up_level = 0;
      } else if (level_ == 2 && mid_limit &&
                 work_average * 100 < (tick_freq / mid_limit) * fit_percent) {
        up_level = 1;
      }
      if (probe_measured) {
        // What it delivered with the limit out of the way: enough of the
        // full rate to keep it, or clearly more than the intermediate one
        // (the same bars as on the way down).
        if (delivers(full_limit, 90)) {
          up_level = 0;
        } else if (level_ == 2 && mid_limit && delivers(mid_limit, 105)) {
          up_level = 1;
        }
      }
      bool raise = false;
      if (probe_measured) {
        probe_phase_ = 0;
        if (up_level < level_) {
          // Confirmed by a second probe soon after.
          raise = ++probe_votes_ >= 2;
          probe_interval_ = kProbeInterval;
          windows_until_probe_ = kProbeConfirmAfter;
        } else {
          probe_votes_ = 0;
          probe_interval_ = std::min(probe_interval_ * 2, kProbeIntervalMax);
          windows_until_probe_ = probe_interval_;
        }
      } else if (up_level < level_) {
        raise = ++up_votes_ >= up_votes_needed_;
      } else {
        up_votes_ = 0;
        if (windows_until_probe_) {
          --windows_until_probe_;
        }
        if (!windows_until_probe_) {
          probe_phase_ = 2;
          probe_settle_frames_ = kProbeSettleFrames;
        }
      }
      if (raise) {
        level_ = up_level;
        recently_raised_ = true;
        raised_windows_ = 0;
        down_votes_ = 0;
        up_votes_ = 0;
        const uint32_t interval = probe_interval_;
        ResetProbing();
        // Remembered across the raise: if it doesn't hold, the next probes
        // come later.
        probe_interval_ = interval;
        windows_until_probe_ = interval;
        result.limit = level_limit(level_);
        result.change = Change::kRaised;
      }
    }
    if (recently_raised_ && result.change == Change::kNone &&
        ++raised_windows_ > 10) {
      // The raise held: the usual calm is enough for the next one.
      recently_raised_ = false;
      up_votes_needed_ = 6;
      probe_interval_ = kProbeInterval;
    }
    if (level_ == 0) {
      probe_phase_ = 0;
      probe_settle_frames_ = 0;
    }
    result.probe = probe_phase_ != 0;
    if (result.change == Change::kNone) {
      result.limit = result.probe ? full_limit : level_limit(level_);
    }
    window_start_ticks_ = now_ticks;
    work_ticks_sum_ = 0;
    work_frames_ = 0;
    window_frames_ = 0;
    return result;
  }

 private:
  // Windows (seconds) before a probe, the most that backs off to after
  // probes that said no, and the wait for the confirming probe after one that
  // said yes.
  static constexpr uint32_t kProbeInterval = 6;
  static constexpr uint32_t kProbeIntervalMax = 24;
  static constexpr uint32_t kProbeConfirmAfter = 2;
  // Frames at the start of a probe window left out of its measure.
  static constexpr uint32_t kProbeSettleFrames = 6;

  void ResetProbing() {
    probe_phase_ = 0;
    probe_votes_ = 0;
    probe_settle_frames_ = 0;
    probe_interval_ = kProbeInterval;
    windows_until_probe_ = kProbeInterval;
  }

  // 0 = none, 2 = the window in progress is a probe.
  uint32_t probe_phase_ = 0;
  uint32_t probe_votes_ = 0;
  uint32_t probe_settle_frames_ = 0;
  uint32_t probe_interval_ = kProbeInterval;
  uint32_t windows_until_probe_ = kProbeInterval;
  uint32_t level_ = 0;
  uint32_t down_votes_ = 0;
  uint32_t up_votes_ = 0;
  uint32_t up_votes_needed_ = 6;
  bool recently_raised_ = false;
  uint32_t raised_windows_ = 0;
  uint32_t work_frames_ = 0;
  uint32_t window_frames_ = 0;
  uint64_t work_ticks_sum_ = 0;
  uint64_t window_start_ticks_ = 0;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_ADAPTIVE_FRAME_LIMITER_H_
