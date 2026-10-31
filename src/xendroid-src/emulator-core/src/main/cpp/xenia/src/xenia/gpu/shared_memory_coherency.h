// Copyright 2026 Xenia contributors. Released under the BSD license.
#ifndef XENIA_GPU_SHARED_MEMORY_COHERENCY_H_
#define XENIA_GPU_SHARED_MEMORY_COHERENCY_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace xe {
namespace gpu {
namespace coherency {

// Inputs are nonzero. Keep this header independently testable and portable
// to the MSVC backend as well as Clang/NDK.
inline uint32_t TrailingZeros(uint64_t bits) {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long index;
  _BitScanForward64(&index, bits);
  return uint32_t(index);
#else
  return uint32_t(__builtin_ctzll(bits));
#endif
}
inline uint32_t LeadingZeros(uint64_t bits) {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long index;
  _BitScanReverse64(&index, bits);
  return 63 - uint32_t(index);
#else
  return uint32_t(__builtin_clzll(bits));
#endif
}

// Inclusive bit bounds within one 64-bit word. No shift by 64.
inline uint64_t Mask(uint32_t first, uint32_t last) {
  return (UINT64_MAX << (first & 63)) &
         (UINT64_MAX >> (63 - (last & 63)));
}

// Enumerates maximal set-bit spans, merging across word boundaries. The
// reader supplies a bitmap word; emit receives (first bit, bit count).
// Storage-free, suitable for use under the memory/fault critical region.
template <typename Reader, typename Emit>
void ForEachRange(uint32_t first, uint32_t last, Reader read, Emit emit) {
  if (first > last) return;
  uint32_t run_first = UINT32_MAX;
  uint32_t run_end = 0;
  for (uint32_t word = first >> 6; word <= last >> 6; ++word) {
    uint64_t bits = read(word);
    if (word == first >> 6) bits &= UINT64_MAX << (first & 63);
    if (word == last >> 6) bits &= UINT64_MAX >> (63 - (last & 63));
    while (bits) {
      uint32_t bit = TrailingZeros(bits);
      uint64_t shifted = bits >> bit;
      uint32_t count = shifted == UINT64_MAX
                           ? 64 : TrailingZeros(~shifted);
      uint32_t begin = (word << 6) + bit;
      uint32_t end = begin + count;
      if (run_first != UINT32_MAX && begin != run_end) {
        emit(run_first, run_end - run_first);
        run_first = UINT32_MAX;
      }
      if (run_first == UINT32_MAX) run_first = begin;
      run_end = end;
      bits &= ~(Mask(bit, bit + count - 1));
    }
  }
  if (run_first != UINT32_MAX) emit(run_first, run_end - run_first);
}

// Expand a write fault to its configured block, stopping at the nearest
// GPU-owned page on either side. The actual fault pages are always included:
// their CPU stores intentionally replace GPU data. No allocation or callbacks.
template <typename Reader>
std::pair<uint32_t, uint32_t> FaultRange(uint32_t first, uint32_t last,
                                      uint32_t page_count, uint32_t block_pages,
                                      Reader gpu_written) {
  if (!block_pages) block_pages = 1;
  if (block_pages > 64) block_pages = 64;
  uint32_t begin = first - first % block_pages;
  uint32_t end = last - last % block_pages + block_pages - 1;
  if (end >= page_count) end = page_count - 1;
  if (begin < first) {
    const uint32_t limit = first - 1;
    for (uint32_t b = begin >> 6; b <= limit >> 6; ++b) {
      uint64_t bits = gpu_written(b) & Mask(b == begin >> 6 ? begin : 0,
                                          b == limit >> 6 ? limit : 63);
      if (bits) begin = (b << 6) + 64 - LeadingZeros(bits);
    }
  }
  if (end > last) {
    const uint32_t limit = last + 1;
    for (uint32_t b = limit >> 6; b <= end >> 6; ++b) {
      uint64_t bits = gpu_written(b) & Mask(b == limit >> 6 ? limit : 0,
                                          b == end >> 6 ? end : 63);
      if (bits) {
        end = (b << 6) + TrailingZeros(bits) - 1;
        break;
      }
    }
  }
  return {begin, end};
}

// Independent epochs for 64 KiB regions, so a CPU write invalidates only the
// cached copies near it (with 4 MiB regions a static index buffer expired with
// any dynamic data in the same 4 MiB). A range over a few regions uses the sum
// of their epochs (each only grows, so the sum changes iff one of them does);
// larger ranges fall back to the global epoch. Region size is CPU metadata
// only; it does not change the 512 MiB Vulkan buffer or watches.
class RangeEpochs {
 public:
  RangeEpochs() {
    for (auto& region : regions_) region.store(0, std::memory_order_relaxed);
  }
  static constexpr uint32_t kRegionShift = 16;
  static constexpr uint32_t kRegionCount = uint32_t(1) << (29 - kRegionShift);
  static constexpr uint32_t kMaxSummedRegions = 16;
  uint64_t Load(uint32_t start, uint32_t length) const {
    uint32_t first = start >> kRegionShift;
    uint32_t last = (start + length - 1) >> kRegionShift;
    if (last - first >= kMaxSummedRegions) {
      return global_.load(std::memory_order_acquire);
    }
    uint64_t sum = 0;
    for (uint32_t i = first; i <= last; ++i) {
      sum += regions_[i].load(std::memory_order_acquire);
    }
    return sum;
  }
  // Called AFTER clearing validity, serialized by the global memory lock.
  void Invalidate(uint32_t start, uint32_t length) {
    uint32_t first = start >> kRegionShift;
    uint32_t last = (start + length - 1) >> kRegionShift;
    for (uint32_t i = first; i <= last; ++i) {
      regions_[i].fetch_add(1, std::memory_order_release);
    }
    global_.fetch_add(1, std::memory_order_release);
  }
  void InvalidateAll() { Invalidate(0, uint32_t(1) << 29); }
  // Advanced by every invalidation, of any range.
  uint64_t Global() const { return global_.load(std::memory_order_acquire); }

 private:
  std::array<std::atomic<uint64_t>, kRegionCount> regions_{};
  std::atomic<uint64_t> global_{0};
};

}  // namespace coherency
}  // namespace gpu
}  // namespace xe
#endif  // XENIA_GPU_SHARED_MEMORY_COHERENCY_H_
