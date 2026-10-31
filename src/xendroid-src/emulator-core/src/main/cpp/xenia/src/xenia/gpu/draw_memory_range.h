/** Copyright 2026 Xenia contributors. Released under the BSD license. */
#ifndef XENIA_GPU_DRAW_MEMORY_RANGE_H_
#define XENIA_GPU_DRAW_MEMORY_RANGE_H_
#include <cstdint>
#include "xenia/gpu/shared_memory.h"
namespace xe {
namespace gpu {
// One draw input, no global hash table. Capture invalidation BEFORE requesting:
// an invalidation racing with upload must never be acknowledged by the cache.
struct DrawMemoryRange {
  uint32_t address = 0, length = 0;
  uint64_t epoch = UINT64_MAX;
  uint32_t batch = 0, submission = 0;
  bool Request(SharedMemory& memory) {
    const auto generation = memory.draw_input_generation();
    const uint64_t range_epoch = memory.DrawInputEpoch(address, length);
    if (generation.reusable && epoch == range_epoch &&
        batch == generation.batch && submission == generation.submission) {
      // Still declare every access in the pending operation. Internal transfers
      // may open a new pass later in this draw, even on a residency hit.
      memory.MarkRangeUsedInSubmission(address, length);
      return true;
    }
    epoch = UINT64_MAX;
    if (!memory.RequestRange(address, length)) return false;
    epoch = range_epoch;
    batch = generation.batch;
    submission = generation.submission;
    return true;
  }
  void Set(uint32_t start, uint32_t size) {
    if (address != start || length != size) {
      address = start;
      length = size;
      epoch = UINT64_MAX;
    }
  }
};
}  // namespace gpu
}  // namespace xe
#endif
