#ifndef XENIA_GPU_GPU_FRAME_TIME_H_
#define XENIA_GPU_GPU_FRAME_TIME_H_
#include <atomic>
#include <chrono>
#include <cstdint>

namespace xe::gpu {
inline std::atomic<bool> gpu_timing_enabled{false};
inline std::atomic<uint64_t> gpu_timing_generation{0};
inline std::atomic<double> gpu_frame_ms{-1.0};
inline std::atomic<uint64_t> gpu_frame_updated_ns{0};
inline uint64_t GpuTimingNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void SetGpuTimingEnabled(bool enabled) {
  if (gpu_timing_enabled.exchange(enabled) != enabled) {
    gpu_frame_updated_ns.store(0);
    gpu_frame_ms.store(-1.0);
    gpu_timing_generation.fetch_add(1);
  }
}
inline double GetGpuFrameTimeMsAt(uint64_t now) {
  const uint64_t updated = gpu_frame_updated_ns.load();
  if (!gpu_timing_enabled.load() || !updated || now < updated ||
      now - updated > 3000000000ull) return -1.0;
  return gpu_frame_ms.load();
}
inline double GetGpuFrameTimeMs() {
  return GetGpuFrameTimeMsAt(GpuTimingNowNs());
}

// Extend a wrapping device clock and avoid counting overlapping submissions twice.
class GpuTimestampSpanUnion {
 public:
  uint64_t Add(uint64_t top, uint64_t bottom, uint32_t bits) {
    if (!bits || bits > 64) return 0;
    const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    top &= mask; bottom &= mask;
    const uint64_t start = seen_ ? extended_top_ + ((top - raw_top_) & mask) : top;
    const uint64_t end = start + ((bottom - top) & mask);
    const uint64_t begin = seen_ && end_ > start ? end_ : start;
    const uint64_t fresh = end > begin ? end - begin : 0;
    raw_top_ = top; extended_top_ = start;
    if (!seen_ || end > end_) end_ = end;
    seen_ = true;
    return fresh;
  }
 private:
  bool seen_ = false;
  uint64_t raw_top_ = 0, extended_top_ = 0, end_ = 0;
};

// Single CP-thread writer. Completed GPU timestamp spans / guest frames in a
// one-second window: an execution-time average, never a device utilization %.
class GpuFrameTimer {
 public:
  void AddTimestampSpan(uint64_t top, uint64_t bottom, uint32_t bits, double period_ns) {
    if (!gpu_timing_enabled.load()) return;
    SyncGeneration();
    AddExecution(uint64_t(span_union_.Add(top, bottom, bits) * period_ns));
  }
  void AddExecution(uint64_t ns) {
    if (!gpu_timing_enabled.load()) return;
    SyncGeneration();
    if (ns <= 10000000000ull) { execution_ns_ += ns; ++samples_; }
    else incomplete_ = true;
  }
  void MarkUnavailable() {
    if (!gpu_timing_enabled.load()) return;
    SyncGeneration(); incomplete_ = true;
  }
  void OnFrame(uint64_t now) {
    if (!gpu_timing_enabled.load()) return;
    SyncGeneration();
    ++frames_;
    if (!window_start_) { window_start_ = now; return; }
    if (now - window_start_ < 1000000000ull) return;
    const double ms = samples_ && frames_ && !incomplete_ ?
        double(execution_ns_) / double(frames_) / 1e6 : -1.0;
    gpu_frame_ms.store(ms);
    gpu_frame_updated_ns.store(now);
    window_start_ = now; execution_ns_ = 0; samples_ = 0; frames_ = 0; incomplete_ = false;
  }
 private:
  void SyncGeneration() {
    const uint64_t generation = gpu_timing_generation.load();
    if (generation != generation_) {
      generation_ = generation;
      window_start_ = 0; execution_ns_ = 0; samples_ = 0; frames_ = 0; incomplete_ = false;
      span_union_ = GpuTimestampSpanUnion();
    }
  }
  uint64_t generation_ = 0, window_start_ = 0, execution_ns_ = 0;
  uint64_t samples_ = 0, frames_ = 0;
  bool incomplete_ = false;
  GpuTimestampSpanUnion span_union_;
};
}
#endif
