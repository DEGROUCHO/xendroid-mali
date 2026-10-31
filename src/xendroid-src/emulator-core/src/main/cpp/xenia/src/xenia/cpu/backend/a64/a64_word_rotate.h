#ifndef XENIA_CPU_BACKEND_A64_A64_WORD_ROTATE_H_
#define XENIA_CPU_BACKEND_A64_A64_WORD_ROTATE_H_
#include <bit>
#include <cstdint>
namespace xe::cpu::backend::a64 {
// Low-word rotation AND a low-word mask. W writes define zero upper 32 bits.
// The PPC frontend uses this only when MB<=ME: wrapping 64-bit masks retain
// their legacy duplicated-word semantics. No condition flags are changed.
template <typename Emitter>
void EmitWordRotateMask(Emitter& e, const Xbyak_aarch64::WReg& dest,
                        const Xbyak_aarch64::WReg& source, unsigned rotation,
                        uint32_t mask) {
  rotation &= 31;
  if (!mask) { e.mov(dest, 0); return; }
  if (mask == UINT32_MAX) {
    if (rotation) e.ror(dest, source, (32-rotation)&31);
    else e.mov(dest, source);
    return;
  }
  const unsigned low = std::countr_zero(mask);
  const uint32_t field = mask >> low;
  // field+1 wraps only for all-ones (handled above).
  if (!(field & (field+1))) {
    const unsigned width = std::bit_width(field);
    const unsigned source_low = (low+32-rotation)&31;
    if (!low && source_low+width<=32) {
      e.ubfx(dest, source, source_low, width);
      return;
    }
    if (!source_low && low+width<=32) {
      e.ubfiz(dest, source, low, width);
      return;
    }
    if (rotation) e.ror(dest, source, (32-rotation)&31);
    // Any proper contiguous 32-bit mask is a logical immediate.
    e.and_(dest, rotation ? dest : source, uint64_t(mask));
    return;
  }
  // The public HIR helper permits any mask. Keep a total fallback even though
  // the PPC contiguous-mask path never produces this case. w17 is reserved.
  if (rotation) e.ror(dest, source, (32-rotation)&31);
  e.mov(e.w17, uint64_t(mask));
  e.and_(dest, rotation ? dest : source, e.w17);
}
}
#endif
