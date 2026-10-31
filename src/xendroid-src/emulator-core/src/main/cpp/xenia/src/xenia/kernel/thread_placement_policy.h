/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_THREAD_PLACEMENT_POLICY_H_
#define XENIA_KERNEL_THREAD_PLACEMENT_POLICY_H_

#include <algorithm>
#include <cstdint>

namespace xe {
namespace kernel {

// Which guest CPU dispatch threads to keep on the host's lower-clocked cores
// (guest_threads_placement), decided about once a second from what each thread
// used of a core and from how the paced frames went. Pure logic so it can be
// exercised on the host; the scheduler measures and applies.
struct ThreadPlacementPolicy {
  static constexpr int kThreads = 6;

  struct Inputs {
    uint64_t now_ms = 0;
    // Since the previous decision.
    uint64_t elapsed_ms = 1000;
    // Per dispatch thread: it exists; share of a core it used (as measured,
    // wherever it ran); the part of that which was work rather than spinning
    // in place.
    bool present[kThreads] = {};
    uint32_t util_percent[kThreads] = {};
    uint32_t work_percent[kThreads] = {};
    // Dispatch thread of the guest thread that presents frames, -1 if unknown.
    int presenter = -1;
    // Paced frames since the previous decision: arrived with spare time, and
    // arrived short of it or late.
    uint32_t comfortable_frames = 0;
    uint32_t tight_frames = 0;
    // The thermal governor reports a hot device.
    bool hot = false;
    // The frames are not gameplay (loading screen, menu, film): what the
    // threads wait for is not bound to them.
    bool loading = false;
    // The frame limiter is measuring the game at full speed.
    bool probe = false;
    // Settings.
    uint32_t enter_percent = 38;
    uint32_t leave_percent = 65;
    uint32_t budget_percent = 60;
    uint32_t slow_core_count = 4;
    // Speed of a slow core relative to the fastest, in percent.
    uint32_t slow_speed_percent = 65;
    bool clamp_spinners = false;
  };

  struct State {
    // Bit per dispatch thread on the slow cores, and with its clock capped.
    uint32_t restricted_mask = 0;
    uint32_t clamped_mask = 0;
    // The restricted threads that are there for doing little work, not for
    // spinning in place: the ones a frame may depend on.
    uint32_t light_mask = 0;
    uint64_t light_since_ms = 0;
    uint32_t calm_seconds = 0;
    uint64_t holdoff_until_ms = 0;
    uint32_t holdoff_ms = 0;
    uint64_t last_apply_ms = 0;
  };

  struct Decision {
    // The masks below are to be (re)applied.
    bool apply = false;
    uint32_t restricted_mask = 0;
    uint32_t clamped_mask = 0;
    const char* reason = "";
  };

  // Updates |state| (except restricted_mask, clamped_mask and last_apply_ms:
  // the caller stores what it managed to apply with NoteApplied).
  static Decision Decide(const Inputs& in, State& state) {
    const uint32_t slow_capacity = in.budget_percent * in.slow_core_count;
    const uint32_t slow_speed = std::clamp(in.slow_speed_percent, 10u, 100u);

    // Threads burning a good part of a core while almost always found
    // spinning in place: a lower clock costs them next to nothing.
    uint32_t spinner_mask = 0;
    uint32_t spinner_budget = slow_capacity;
    for (int i = 0; i < kThreads; ++i) {
      if (i == in.presenter || !in.present[i] || in.util_percent[i] < 30 ||
          in.work_percent[i] * 8 > in.util_percent[i] ||
          in.util_percent[i] > spinner_budget) {
        continue;
      }
      spinner_budget -= in.util_percent[i];
      spinner_mask |= uint32_t(1) << i;
    }

    const uint32_t frames = in.comfortable_frames + in.tight_frames;
    // At least 5 paced frames a second, whatever the period turned out to be.
    const bool paced = uint64_t(frames) * 1000 >= in.elapsed_ms * 5;
    const bool stressed =
        in.tight_frames >= std::max(uint32_t(3), frames / 10);
    const bool calm =
        paced && in.tight_frames <= std::max(uint32_t(1), frames / 30);

    Decision decision;
    const auto finish = [&](uint32_t restricted, uint32_t clamped,
                            const char* reason) {
      clamped &= restricted;
      decision.restricted_mask = restricted;
      decision.clamped_mask = clamped;
      decision.reason = reason;
      decision.apply =
          restricted != state.restricted_mask ||
          clamped != state.clamped_mask ||
          (restricted && in.now_ms - state.last_apply_ms >= 10000);
      const uint32_t light = restricted & ~spinner_mask;
      if (light && !state.light_mask) {
        state.light_since_ms = in.now_ms;
      }
      state.light_mask = light;
      return decision;
    };
    const uint32_t spinner_clamped = in.clamp_spinners ? spinner_mask : 0;

    if (in.probe) {
      // Everything back to where the host scheduler puts it while the probe
      // lasts; what was learned about the frames is kept for afterwards.
      return finish(0, 0, "limiter probe");
    }

    if (!paced || stressed || in.loading) {
      if (state.light_mask && !in.loading) {
        // Frames running short right after a restriction most likely ran
        // short because of it: wait longer before the next try.
        state.holdoff_ms =
            in.now_ms - state.light_since_ms < 5000
                ? std::min(std::max(state.holdoff_ms, uint32_t(5000)) * 2,
                           uint32_t(120000))
                : uint32_t(5000);
        state.holdoff_until_ms = in.now_ms + state.holdoff_ms;
      }
      state.calm_seconds = 0;
      // With frames short of time everything stays where the host scheduler
      // puts it - a worker spinning between two short jobs is what makes the
      // next one start at once - unless the device is hot, when the lower
      // clock of the spinners matters more.
      return finish(in.hot ? spinner_mask : 0, in.hot ? spinner_clamped : 0,
                    in.loading ? "not gameplay"
                               : (paced ? "frames short of spare time"
                                        : "no pacing"));
    }

    state.calm_seconds = calm ? state.calm_seconds + 1 : 0;
    if (!state.light_mask &&
        (state.calm_seconds < 3 || in.now_ms < state.holdoff_until_ms)) {
      // Not calm for long enough yet, or a restriction just failed: only the
      // spinners.
      return finish(spinner_mask, spinner_clamped, "spinning in place");
    }

    // The lightest threads first, while the slow cores have room for them.
    uint32_t budget = slow_capacity;
    int order[kThreads];
    uint32_t cost[kThreads];
    for (int i = 0; i < kThreads; ++i) {
      order[i] = i;
      // Share of a slow core the thread takes there: as measured if it
      // already is on one, scaled by the speed difference otherwise.
      cost[i] = ((state.restricted_mask >> i) & 1)
                    ? in.util_percent[i]
                    : in.util_percent[i] * 100 / slow_speed;
    }
    std::sort(order, order + kThreads, [&](int a, int b) {
      return cost[a] != cost[b] ? cost[a] < cost[b] : a < b;
    });
    uint32_t restricted_mask = 0;
    uint32_t clamped_mask = 0;
    for (int i : order) {
      if (i == in.presenter || !in.present[i]) {
        continue;
      }
      const bool restricted = (state.restricted_mask >> i) & 1;
      const bool spinner = (spinner_mask >> i) & 1;
      if ((!spinner && in.work_percent[i] >=
                           (restricted ? in.leave_percent : in.enter_percent)) ||
          cost[i] > budget) {
        continue;
      }
      budget -= cost[i];
      restricted_mask |= uint32_t(1) << i;
      if (spinner) {
        clamped_mask |= spinner_clamped & (uint32_t(1) << i);
      }
    }
    return finish(restricted_mask, clamped_mask, "frames have spare time");
  }

  // What the caller actually applied after a Decision with |apply| set.
  static void NoteApplied(State& state, uint32_t restricted_mask,
                          uint32_t clamped_mask, uint64_t now_ms) {
    state.restricted_mask = restricted_mask;
    state.clamped_mask = clamped_mask & restricted_mask;
    state.light_mask &= restricted_mask;
    state.last_apply_ms = now_ms;
  }
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_THREAD_PLACEMENT_POLICY_H_
