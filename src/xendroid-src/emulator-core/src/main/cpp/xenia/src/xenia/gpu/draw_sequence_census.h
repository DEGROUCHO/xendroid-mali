/**
 * Copyright 2026 Xenia contributors. Released under the BSD license.
 */
#ifndef XENIA_GPU_DRAW_SEQUENCE_CENSUS_H_
#define XENIA_GPU_DRAW_SEQUENCE_CENSUS_H_

#include <algorithm>
#include <cstdint>
#include <map>

namespace xe {
namespace gpu {

// Diagnostic only. The caller establishes compatibility and closes the run at
// every ordering boundary. No guest or host rendering state is retained here.
class DrawSequenceCensus {
 public:
  void Add(bool compatible) {
    if (!compatible) Close();
    ++length_;
  }
  void Close() {
    if (!length_) return;
    ++lengths_[length_];
    draws_ += length_;
    ++sequences_;
    max_length_ = std::max(max_length_, length_);
    length_ = 0;
  }
  // Call only at a closed reporting boundary (including the first swap).
  void ResetWindow() {
    Close();
    lengths_.clear();
    draws_ = sequences_ = max_length_ = 0;
  }
  uint64_t draws() const { return draws_; }
  uint64_t sequences() const { return sequences_; }
  uint64_t max_length() const { return max_length_; }
  const std::map<uint64_t, uint64_t>& lengths() const { return lengths_; }
  // Counts removed host draw commands, rather than merely participating draws.
  uint64_t Removed(uint32_t limit) const {
    if (!limit) return 0;
    uint64_t removed = 0;
    for (const auto& entry : lengths_) {
      const uint64_t length = entry.first;
      removed +=
          (length - (length / limit + (length % limit != 0))) * entry.second;
    }
    return removed;
  }

 private:
  uint64_t length_ = 0, draws_ = 0, sequences_ = 0, max_length_ = 0;
  std::map<uint64_t, uint64_t> lengths_;
};

}  // namespace gpu
}  // namespace xe
#endif  // XENIA_GPU_DRAW_SEQUENCE_CENSUS_H_
