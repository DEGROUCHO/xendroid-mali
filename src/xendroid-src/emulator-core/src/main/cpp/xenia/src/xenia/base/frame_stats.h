/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 */

#ifndef XENIA_BASE_FRAME_STATS_H_
#define XENIA_BASE_FRAME_STATS_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace xe {

// Guest-frame present timing for the debug overlay.
//
// RecordGuestPresent() is called exactly once per presented GUEST frame (from
// the GPU command processor's IssueSwap), so the reported FPS / frame time
// reflect the actual game frame rate -- NOT the host UI repaint cadence (which
// keeps running at panel refresh even when the guest stalls). The producer is
// single-threaded (the command-processor thread); the published values are read
// from the UI thread via GetFrameStats(). A torn read across the three values
// is harmless for a debug readout.
namespace internal {
inline std::atomic<float>& frame_instant_ms() {
  static std::atomic<float> v{0.0f};
  return v;
}
inline std::atomic<float>& frame_avg_ms() {
  static std::atomic<float> v{0.0f};
  return v;
}
inline std::atomic<float>& frame_fps() {
  static std::atomic<float> v{0.0f};
  return v;
}
inline std::atomic<float>& frame_presented_fps() {
  static std::atomic<float> v{0.0f};
  return v;
}
inline std::atomic<float>& requested_present_rate() {
  static std::atomic<float> v{-1.0f};
  return v;
}
}  // namespace internal

// Frames per second actually shown with frame generation (interpolated ones
// included), published by the presenter once per second; 0 when it is off.
inline void SetPresentedFps(float fps) {
  internal::frame_presented_fps().store(fps, std::memory_order_relaxed);
}
inline float GetPresentedFps() {
  return internal::frame_presented_fps().load(std::memory_order_relaxed);
}

// Display rate the frame limiter wants while it paces the game at a rate the
// default display mode would not show evenly; negative = no preference.
inline void SetRequestedPresentRate(float rate) {
  internal::requested_present_rate().store(rate, std::memory_order_relaxed);
}
inline float GetRequestedPresentRate() {
  return internal::requested_present_rate().load(std::memory_order_relaxed);
}

// Call once per presented guest frame (single producer thread).
// FPS is a RenderDoc-style average over a ~1s sliding time window (framerate
// independent); instant_ms stays the raw present-to-present delta.
inline void RecordGuestPresent() {
  using clock = std::chrono::steady_clock;
  static constexpr double kWindowMs = 1000.0;
  static constexpr size_t kCap = 1024;  // covers >1000 fps within the window
  static clock::time_point ts[kCap];
  static size_t head = 0;   // oldest sample
  static size_t count = 0;  // samples in the window
  static clock::time_point last{};

  clock::time_point now = clock::now();
  const bool have_last = last != clock::time_point{};
  double instant_ms =
      have_last ? std::chrono::duration<double, std::milli>(now - last).count()
                : 0.0;
  last = now;

  // A long gap (first frame / pause / load) restarts the window so the average
  // recovers within a second instead of being dragged by a stale outlier.
  if (!have_last || instant_ms > kWindowMs) {
    head = 0;
    count = 1;
    ts[0] = now;
    internal::frame_instant_ms().store(0.0f, std::memory_order_relaxed);
    internal::frame_avg_ms().store(0.0f, std::memory_order_relaxed);
    internal::frame_fps().store(0.0f, std::memory_order_relaxed);
    return;
  }

  const size_t tail = (head + count) % kCap;
  ts[tail] = now;
  if (count < kCap) {
    ++count;
  } else {
    head = (head + 1) % kCap;  // ring full: drop oldest
  }

  // Evict samples older than the window.
  while (count > 1 &&
         std::chrono::duration<double, std::milli>(now - ts[head]).count() >
             kWindowMs) {
    head = (head + 1) % kCap;
    --count;
  }

  const double span_ms =
      std::chrono::duration<double, std::milli>(now - ts[head]).count();
  const double fps =
      (count > 1 && span_ms > 0.0) ? double(count - 1) * 1000.0 / span_ms : 0.0;
  const double avg_ms = fps > 0.0 ? 1000.0 / fps : 0.0;

  internal::frame_instant_ms().store(float(instant_ms),
                                     std::memory_order_relaxed);
  internal::frame_avg_ms().store(float(avg_ms), std::memory_order_relaxed);
  internal::frame_fps().store(float(fps), std::memory_order_relaxed);
}

// Read the latest published stats (any thread).
inline void GetFrameStats(float& instant_ms, float& avg_ms, float& fps) {
  instant_ms = internal::frame_instant_ms().load(std::memory_order_relaxed);
  avg_ms = internal::frame_avg_ms().load(std::memory_order_relaxed);
  fps = internal::frame_fps().load(std::memory_order_relaxed);
}

}  // namespace xe

#endif  // XENIA_BASE_FRAME_STATS_H_
