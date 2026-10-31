/** Diagnostic declared-range accounting; does not allocate or evict GPU pages. */
#ifndef XENIA_GPU_SHARED_MEMORY_WORKING_SET_H_
#define XENIA_GPU_SHARED_MEMORY_WORKING_SET_H_
#include <algorithm>
#include <array>
#include <cstdint>
#include "xenia/base/math.h"
namespace xe::gpu {
class SharedMemoryWorkingSet {
 public:
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint32_t kSize = uint32_t(512) << 20;
  static constexpr uint32_t kWordCount = kSize / kPageSize / 64;
  struct Stats {
    uint64_t frames;
    uint32_t last_frame_pages, peak_frame_pages, window_pages, all_seen_pages;
    // Distinct 2 MiB / 32 MiB blocks with any requested page.
    uint32_t window_blocks_2m, all_seen_blocks_2m, window_blocks_32m,
        all_seen_blocks_32m;
  };
  // Command processor thread only. Include validated attempted ranges, even
  // if a later upload fails. This overestimates actual accesses, and cannot
  // determine pending or GPU-authoritative residency needed for safe eviction.
  void Request(uint32_t start, uint32_t length) {
    if (!length || start >= kSize) return;
    uint32_t first = start / kPageSize;
    uint32_t last = uint32_t((std::min(uint64_t(start) + length,
                                     uint64_t(kSize)) - 1) / kPageSize);
    for (uint32_t word = first / 64; word <= last / 64; ++word) {
      uint64_t mask = UINT64_MAX;
      if (word == first / 64) mask &= UINT64_MAX << (first % 64);
      if (word == last / 64) mask &= UINT64_MAX >> (63 - last % 64);
      frame_[word] |= mask;
      window_[word] |= mask;
      all_seen_[word] |= mask;
    }
  }
  bool EndFrame(Stats& stats) {
    uint32_t count = Count(frame_);
    peak_ = std::max(peak_, count);
    frame_.fill(0);
    if (++frames_ % 120) return false;
    stats = {frames_, count, peak_, Count(window_), Count(all_seen_),
             CountBlocks(window_, 8), CountBlocks(all_seen_, 8),
             CountBlocks(window_, 128), CountBlocks(all_seen_, 128)};
    window_.fill(0);
    peak_ = 0;
    return true;
  }
 private:
  using Bits = std::array<uint64_t, kWordCount>;
  static uint32_t Count(const Bits& bits) {
    uint32_t count = 0;
    for (uint64_t word : bits) count += xe::bit_count(word);
    return count;
  }
  // Blocks of |words_per_block| 64-page words with any bit set.
  static uint32_t CountBlocks(const Bits& bits, uint32_t words_per_block) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < kWordCount; i += words_per_block) {
      uint64_t any = 0;
      for (uint32_t j = 0; j < words_per_block; ++j) any |= bits[i + j];
      count += any != 0;
    }
    return count;
  }
  Bits frame_{}, window_{}, all_seen_{};
  uint64_t frames_ = 0;
  uint32_t peak_ = 0;
};
}  // namespace xe::gpu
#endif  // XENIA_GPU_SHARED_MEMORY_WORKING_SET_H_
