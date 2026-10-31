#ifndef XENIA_GPU_USER_CLIP_PLANES_H_
#define XENIA_GPU_USER_CLIP_PLANES_H_
#include <cstdint>
namespace xe::gpu {
// Only exactly zero planes are no-ops for the ordered-negative software test.
// Keep subnormals, infinities and NaNs; +/-0 are equivalent here.
inline uint32_t RemoveZeroUserClipPlanes(uint32_t mask,
                                         const uint32_t* coefficients) {
  for (uint32_t i = 0; i < 6; ++i) {
    if (!(mask & (1u << i))) continue;
    uint32_t magnitude = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      magnitude |= coefficients[i * 4 + c] & 0x7FFFFFFFu;
    }
    if (!magnitude) mask &= ~(1u << i);
  }
  return mask;
}
}  // namespace xe::gpu
#endif
