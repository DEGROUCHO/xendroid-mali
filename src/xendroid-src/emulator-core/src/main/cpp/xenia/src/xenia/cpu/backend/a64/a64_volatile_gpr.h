// The additional SSA registers are volatile under AAPCS64. Preserve them only
// across native calls; guest HIR calls terminate blocks and clobber SSA state.
#ifndef XENIA_CPU_BACKEND_A64_A64_VOLATILE_GPR_H_
#define XENIA_CPU_BACKEND_A64_A64_VOLATILE_GPR_H_
#include <cstdint>
#include "xbyak_aarch64.h"
namespace xe::cpu::backend::a64 {
constexpr uint32_t kExtendedGprSaveBytes = 48;
constexpr int32_t kExtendedGprGuestStashOffset = 80;
template <typename E> void SaveExtendedGprs(E& e, int32_t offset) {
  e.stp(e.x10, e.x11, Xbyak_aarch64::ptr(e.sp, offset));
  e.stp(e.x12, e.x13, Xbyak_aarch64::ptr(e.sp, offset + 16));
  e.stp(e.x14, e.x15, Xbyak_aarch64::ptr(e.sp, offset + 32));
}
template <typename E> void RestoreExtendedGprs(E& e, int32_t offset) {
  e.ldp(e.x14, e.x15, Xbyak_aarch64::ptr(e.sp, offset + 32));
  e.ldp(e.x12, e.x13, Xbyak_aarch64::ptr(e.sp, offset + 16));
  e.ldp(e.x10, e.x11, Xbyak_aarch64::ptr(e.sp, offset));
}
}  // namespace xe::cpu::backend::a64
#endif  // XENIA_CPU_BACKEND_A64_A64_VOLATILE_GPR_H_
