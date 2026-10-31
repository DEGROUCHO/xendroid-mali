/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_SPIN_PARK_CONTROL_H_
#define XENIA_KERNEL_SPIN_PARK_CONTROL_H_

#include <algorithm>
#include <cstdint>

namespace xe {
namespace kernel {

// Feedback from the frame pacer to the guest spin parks, once per paced frame
// (guest_spin_park_adaptive). Pure logic so it can be exercised on the host.
struct SpinParkControl {
  struct Step {
    // The frame arrived with enough spare time (for the placement governor's
    // frame tallies).
    bool comfortable;
    // How long from now the sleeps are allowed, in ms; 0 = no new allowance
    // (the one granted by the previous frame is left to run out).
    uint64_t allow_ms;
    // Length of the sleeps from now on, in 1/256 of the configured one.
    uint32_t scale_q8;
  };

  // Spare time a frame must keep for the sleeps to go on: the configured one,
  // 15% of the frame period at most (and at least 1 ms).
  static uint64_t KeepMs(uint64_t configured_min_slack_ms, uint64_t period_ms) {
    return std::min(configured_min_slack_ms,
                    std::max(period_ms * 15 / 100, uint64_t(1)));
  }

  // |early|: the frame reached the swap before its presentation slot,
  // |slack_ms| ahead of it. |period_ms|: the pace's frame period.
  // |scale_q8|: the current scale.
  static Step Update(bool adaptive, bool early, uint64_t slack_ms,
                     uint64_t period_ms, uint64_t configured_min_slack_ms,
                     int32_t max_scale_percent, uint32_t scale_q8) {
    if (!adaptive) {
      // Fixed gate: 60 ms of allowance after each frame with enough spare
      // time, sleeps as configured.
      const bool comfortable = early && slack_ms >= configured_min_slack_ms;
      return {comfortable, comfortable ? uint64_t(60) : uint64_t(0), 256};
    }
    const uint64_t keep_ms = KeepMs(configured_min_slack_ms, period_ms);
    // Spare time above which the sleeps may grow.
    const uint64_t grow_ms = keep_ms + std::max(period_ms / 10, uint64_t(2));
    const uint32_t max_scale = std::max(
        uint32_t(std::clamp(max_scale_percent, 25, 3200)) * 256 / 100,
        uint32_t(64));
    if (early && slack_ms >= keep_ms) {
      // Allowed through the wait for this frame's slot and while the next
      // frame is within its budget: one that runs longer finishes at full
      // speed.
      if (slack_ms >= grow_ms) {
        scale_q8 = std::min(max_scale, scale_q8 + 16);
      }
      scale_q8 = std::min(scale_q8, max_scale);
      return {true, slack_ms + (period_ms > keep_ms ? period_ms - keep_ms : 0),
              scale_q8};
    }
    // Short of spare time, or behind: the next frame gets no sleeps (no new
    // allowance), and the ones after it shorter sleeps.
    return {false, 0, std::max(uint32_t(64), scale_q8 / 2)};
  }
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_SPIN_PARK_CONTROL_H_
