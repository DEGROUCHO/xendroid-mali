#ifndef XENIA_BASE_FRAME_INTERVAL_HISTOGRAM_H_
#define XENIA_BASE_FRAME_INTERVAL_HISTOGRAM_H_
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "xenia/base/logging.h"

namespace xe {
// Intervals between consecutive events of one producer thread, in 0.25 ms
// buckets, logged as percentiles every 5 seconds (stalls included). Fixed
// storage, no allocation.
class FrameIntervalHistogram {
 public:
  void Record(const char* stage) {
    const auto now = std::chrono::steady_clock::now();
    if (last_ == std::chrono::steady_clock::time_point{}) {
      last_ = start_ = now;
      return;
    }
    const auto us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - last_)
            .count());
    last_ = now;
    ++bins_[std::min<uint64_t>(us / 250, bins_.size() - 1)];
    ++count_;
    over25_ += us > 25000;
    over50_ += us > 50000;
    max_us_ = std::max(max_us_, us);
    if (now - start_ < std::chrono::seconds(5)) return;
    const auto percentile = [&](uint32_t p) {
      uint32_t sum = 0;
      for (size_t i = 0; i < bins_.size(); ++i) {
        sum += bins_[i];
        if (uint64_t(sum) * 100 >= uint64_t(count_) * p) {
          return i + 1 == bins_.size() ? double(max_us_) / 1000.0
                                       : double(i + 1) * 0.25;
        }
      }
      return 0.0;
    };
    XELOGI(
        "FramePace {} n={} p50<={:.2f} p90<={:.2f} p99<={:.2f} max={:.2f} ms "
        "over25={} over50={}",
        stage, count_, percentile(50), percentile(90), percentile(99),
        double(max_us_) / 1000.0, over25_, over50_);
    bins_.fill(0);
    count_ = over25_ = over50_ = 0;
    max_us_ = 0;
    start_ = now;
  }
  void Reset() { *this = FrameIntervalHistogram(); }

 private:
  std::array<uint32_t, 401> bins_{};
  uint32_t count_ = 0, over25_ = 0, over50_ = 0;
  uint64_t max_us_ = 0;
  std::chrono::steady_clock::time_point last_{}, start_{};
};
}  // namespace xe
#endif  // XENIA_BASE_FRAME_INTERVAL_HISTOGRAM_H_
