/**
 * Copyright 2026 Xenia contributors. Released under the BSD license.
 */
#ifndef XENIA_GPU_DRAW_FLOAT_CONSTANTS_H_
#define XENIA_GPU_DRAW_FLOAT_CONSTANTS_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include "xenia/gpu/shared_memory_coherency.h"

namespace xe {
namespace gpu {

// Packed CPU image and copy plan. GPU slices are immutable: an interval edit
// updates this image, then the uploader copies it into a fresh slice. Never
// patch the slice used by an already recorded draw.
class DrawFloatConstants {
 public:
  bool Update(const void* shader, const uint64_t* bitmap, bool full,
              const uint32_t* source, uint32_t first, uint32_t end) {
    bool layout_changed = false;
    if (shader_ != shader || full_ != full || !valid_) {
      uint64_t bits[4];
      for (uint32_t i = 0; i < 4; ++i)
        bits[i] = full ? UINT64_MAX : (bitmap ? bitmap[i] : 0);
      layout_changed = !valid_ || std::memcmp(bits, bitmap_, sizeof(bits));
      shader_ = shader;
      full_ = full;
      valid_ = true;
      if (layout_changed) {
        std::memcpy(bitmap_, bits, sizeof(bits));
        count_ = span_count_ = 0;
        coherency::ForEachRange(
            0, 255, [&](uint32_t word) { return bits[word]; },
            [&](uint32_t begin, uint32_t length) {
              spans_[span_count_++] = {begin, begin + length, count_};
              count_ += length;
            });
        first = 0;
        end = 256;
      }
    }
    bool changed = layout_changed;
    if (first < end) {
      for (uint32_t i = 0; i < span_count_; ++i) {
        const Span& span = spans_[i];
        if (span.first >= end) break;
        uint32_t begin = std::max(first, span.first);
        uint32_t limit = std::min(end, span.end);
        if (begin >= limit) continue;
        std::memcpy(values_ + 4 * (span.packed + begin - span.first),
                    source + 4 * begin, 16 * (limit - begin));
        changed = true;
      }
    }
    return changed;
  }
  const uint32_t* data() const { return values_; }
  uint32_t count() const { return count_; }

 private:
  struct Span { uint32_t first, end, packed; };
  Span spans_[128];
  uint64_t bitmap_[4] = {};
  alignas(16) uint32_t values_[1024] = {};
  const void* shader_ = nullptr;
  uint32_t count_ = 0, span_count_ = 0;
  bool valid_ = false, full_ = false;
};

}  // namespace gpu
}  // namespace xe
#endif
