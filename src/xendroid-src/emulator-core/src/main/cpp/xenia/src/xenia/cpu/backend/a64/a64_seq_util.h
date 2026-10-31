/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
#define XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_

#include "xenia/base/cvar.h"
#include "xenia/base/memory.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"

#include "xbyak_aarch64.h"

DECLARE_bool(a64_vmx_nan_fixup);
DECLARE_bool(a64_vmx_direct_registers);
DECLARE_bool(a64_fpu_nan_fixup);
DECLARE_bool(a64_fpu_nan_fixup_result_check);

#if XE_COMPILER_MSVC
#include <intrin.h>
constexpr uint32_t DCZID_EL0 = ARM64_SYSREG(0b11, 0b011, 0b0000, 0b0000, 0b111);
#define xe_cpu_mrs(reg) _ReadStatusReg(reg)
#elif XE_COMPILER_CLANG || XE_COMPILER_GNUC
#include <arm_acle.h>
#define xe_cpu_mrs(reg) __arm_rsr64(#reg)
#else
#error "No MRS wrapper available for current compiler implemented."
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using Xbyak_aarch64::QReg;
using Xbyak_aarch64::VReg;
using Xbyak_aarch64::WReg;
using Xbyak_aarch64::XReg;

template <typename Fn>
inline void EmitWithVmxFpcr(A64Emitter& e, Fn&& emit_op) {
  // Enter VMX FPCR mode using tracked lazy switching.  If the emitter
  // is already in VMX mode (e.g. consecutive VMX ops in the same basic
  // block) this is a no-op — no system register access at all.
  // FPU mode is restored at block boundaries and calls via ForgetFpcrMode,
  // or on demand by scalar FP sequences via ChangeFpcrMode(Fpu).
  e.ChangeFpcrMode(FPCRMode::Vmx);
  emit_op();
}

// True iff the 64-bit value is encodable as a MOVI Dn, #imm8 immediate.
// MOVI replicates the 8-bit immediate "a:b:c:d:e:f:g:h" into the bytes
// "aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffffgggggggghhhhhhhh", so
// every byte of the input must be 0x00 or 0xFF.
inline bool IsMovi64Imm(uint64_t value) {
  if (value == 0 || value == ~uint64_t(0)) {
    return true;
  }
  for (int shift = 0; shift < 8; ++shift) {
    const uint8_t shift_u8 = static_cast<uint8_t>(value >> (shift * 8));
    if (shift_u8 != 0 && shift_u8 != 0xFF) {
      return false;
    }
  }
  return true;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbc defgh000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov32Imm(float f32) {
  const uint32_t u32 = std::bit_cast<uint32_t, float>(f32);
  const uint32_t sign = (u32 >> 31) & 1;
  int32_t exp = ((u32 >> 23) & 0xff) - 127;
  int64_t mantissa = u32 & 0x7fffff;

  // Too many mantissa bits
  if (mantissa & 0x7ffff) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 19;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }

  return true;
}

// Try to see if the provided double value can be compressed into an 8-bit value
// for the fmov instruction. Returns false if the value cannot be represented
// abcdefgh
//    V
// aBbbbbbb bbcdefgh 00000000 00000000 00000000 00000000 00000000 00000000
// B = NOT(b)
constexpr bool IsFmov64Imm(double f64) {
  const uint64_t u64 = std::bit_cast<uint64_t, double>(f64);
  int32_t exp = ((u64 >> 52) & 0x7ff) - 1023;
  int64_t mantissa = u64 & 0xfffffffffffffULL;

  // Too many mantissa bits
  if (mantissa & 0xffffffffffffULL) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 48;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }
  return true;
}

// Whether a 64-bit value is encodable as an A64 logical (bitmask) immediate:
// a power-of-two-sized element, repeated, that is a rotated run of ones.
inline bool IsLogicalImm64(uint64_t value) {
  if (!value || value == ~uint64_t(0)) {
    return false;
  }
  uint32_t size = 64;
  while (size > 2) {
    const uint32_t half = size / 2;
    const uint64_t half_mask = (uint64_t(1) << half) - 1;
    if ((value & half_mask) != ((value >> half) & half_mask)) {
      break;
    }
    size = half;
  }
  const uint64_t mask = size == 64 ? ~uint64_t(0) : (uint64_t(1) << size) - 1;
  const uint64_t element = value & mask;
  for (uint32_t rotate = 0; rotate < size; ++rotate) {
    const uint64_t rotated =
        rotate ? ((element >> rotate) | (element << (size - rotate))) & mask
               : element;
    // A run of ones starting at bit 0.
    if (rotated && !(rotated & (rotated + 1))) {
      return true;
    }
  }
  return false;
}

// A 128-bit constant from its two 64-bit halves (lanes 0-1 in low).
inline vec128_t V128FromHalves(uint64_t low, uint64_t high) {
  vec128_t value;
  value.low = low;
  value.high = high;
  return value;
}

// Load a compile-time vec128_t constant into a NEON register.
// May clobber the provided GPR scratch-register
inline void LoadV128Const(A64Emitter& e, int vreg_idx, const vec128_t& val,
                          int gpr_scratch_idx = 0) {
  // Fast common cases
  if (!val.low && !val.high) {
    // 0000...
    e.movi(VReg2D(vreg_idx), 0);
    return;
  } else if (val.low == ~uint64_t(0) && val.high == ~uint64_t(0)) {
    // 1111...
    e.movi(VReg2D(vreg_idx), ~0ULL);
    return;
  }

  // Element splats
  bool all_equal_u8 = true;
  const uint8_t splat_u8 = val.u8[0];
  for (unsigned i = 1; i < 16; ++i) {
    if (val.u8[i] != splat_u8) {
      all_equal_u8 = false;
      break;
    }
  }
  if (all_equal_u8) {
    e.movi(VReg(vreg_idx).b16, static_cast<uint8_t>(splat_u8));
    return;
  }

  bool all_equal_u16 = true;
  const uint16_t splat_u16 = val.u16[0];
  for (unsigned i = 1; i < 8; ++i) {
    if (val.u16[i] != splat_u16) {
      all_equal_u16 = false;
      break;
    }
  }
  if (all_equal_u16) {
    if ((splat_u16 & 0xFF'00) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 0), LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0) {
      e.movi(VReg(vreg_idx).h8, static_cast<uint8_t>(splat_u16 >> 8), LSL, 8);
    } else if ((splat_u16 & 0xFF'00) == 0xFF'00) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u16 & 0x00'FF) == 0x00'FF) {
      e.mvni(VReg(vreg_idx).h8, ~static_cast<uint8_t>(splat_u16 >> 8) & 0xFF,
             LSL, 8);
    } else {
      e.movz(WReg(gpr_scratch_idx), splat_u16, 0);
      e.dup(VReg(vreg_idx).h8, WReg(gpr_scratch_idx));
    }
    return;
  }

  bool all_equal_u32 = true;
  const uint32_t splat_u32 = val.u32[0];
  const float splat_f32 = val.f32[0];
  for (unsigned i = 1; i < 4; ++i) {
    if (val.u32[i] != splat_u32) {
      all_equal_u32 = false;
      break;
    }
  }
  if (all_equal_u32) {
    if ((splat_u32 & 0xFF'FF'FF'00) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 0), LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 8), LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 16), LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0) {
      e.movi(VReg(vreg_idx).s4, static_cast<uint8_t>(splat_u32 >> 24), LSL, 24);
    } else if ((splat_u32 & 0xFF'FF'FF'00) == 0xFF'FF'FF'00) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 0) & 0xFF,
             LSL, 0);
    } else if ((splat_u32 & 0xFF'FF'00'FF) == 0xFF'FF'00'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 8) & 0xFF,
             LSL, 8);
    } else if ((splat_u32 & 0xFF'00'FF'FF) == 0xFF'00'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 16) & 0xFF,
             LSL, 16);
    } else if ((splat_u32 & 0x00'FF'FF'FF) == 0x00'FF'FF'FF) {
      e.mvni(VReg(vreg_idx).s4, ~static_cast<uint8_t>(splat_u32 >> 24) & 0xFF,
             LSL, 24);
    } else if (IsFmov32Imm(splat_f32)) {
      e.fmov(VReg(vreg_idx).s4, splat_f32);
    } else {
      // A value needing two moves (plus the dup) is one literal load instead.
      const bool single_mov =
          (splat_u32 & 0xFFFF) == 0 || (splat_u32 >> 16) == 0 ||
          (splat_u32 & 0xFFFF) == 0xFFFF || (splat_u32 >> 16) == 0xFFFF;
      if (single_mov || !e.LoadV128FromPool(vreg_idx, val)) {
        e.mov(WReg(gpr_scratch_idx), splat_u32);
        e.dup(VReg(vreg_idx).s4, WReg(gpr_scratch_idx));
      }
    }
    return;
  }

  const bool all_equal_u64 = val.low == val.high;
  const uint64_t splat_u64 = val.u64[0];
  const double splat_f64 = val.f64[0];
  if (all_equal_u64) {
    if (IsMovi64Imm(val.low)) {
      // xbyak's movi(VReg2D, imm) takes the FULL 64-bit per-byte pattern and
      // performs its own compression - passing the pre-compressed imm8 makes
      // it reject the encoding (Xbyak_aarch64::Error on byte-mask constants,
      // e.g. vector select masks).
      e.movi(VReg2D(vreg_idx), val.low);
    } else if (IsFmov64Imm(splat_f64)) {
      e.fmov(VReg(vreg_idx).d2, splat_f64);
    } else if (!e.LoadV128FromPool(vreg_idx, val)) {
      e.mov(XReg(gpr_scratch_idx), splat_u64);
      e.dup(VReg(vreg_idx).d2, XReg(gpr_scratch_idx));
    }
    return;
  }

  if (e.LoadV128FromPool(vreg_idx, val)) {
    return;
  }

  // Fallback
  e.mov(XReg(gpr_scratch_idx), val.low);
  e.fmov(DReg(vreg_idx), XReg(gpr_scratch_idx));
  e.mov(XReg(gpr_scratch_idx), val.high);
  e.ins(VReg(vreg_idx).d2[1], XReg(gpr_scratch_idx));
}

// Resolve a V128 operand to a register index, loading constants into
// scratch_idx if needed.
template <typename T>
inline int SrcVReg(A64Emitter& e, const T& op, int scratch_idx) {
  if (op.is_constant) {
    LoadV128Const(e, scratch_idx, op.constant());
    return scratch_idx;
  }
  return op.reg().getIdx();
}

// Compute a guest memory address, returning the XReg for [x21, xN] addressing.
// For constants, loads the address into x0 (scratch).
inline XReg ComputeMemoryAddress(A64Emitter& e, const I64Op& guest) {
  using namespace Xbyak_aarch64;
  if (guest.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant());
    if (address >= 0xE0000000 &&
        xe::memory::allocation_granularity() > 0x1000) {
      address += 0x1000;
    }
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  } else {
    auto src = guest.reg();
    // Guest addresses are always 32-bit. Clear any stale upper bits before
    // applying the host membase so guest pointers can't escape above 4 GB.
    e.mov(e.w0, WReg(src.getIdx()));
    if (xe::memory::allocation_granularity() > 0x1000) {
      e.mov(e.w17, 0xE0000000u);
      e.cmp(e.w0, e.w17);
      auto& skip = e.NewCachedLabel();
      e.b(LO, skip);
      e.add(e.w0, e.w0, 1, 12);  // add 0x1000 via LSL #12
      e.L(skip);
    }
    return e.x0;
  }
}

// Emit w0 = zext32(uint32(base) + offset) for a constant guest offset.
// Guest address arithmetic wraps at 32 bits before the host membase is
// applied; the W-register add/sub forms both provide that wrap and clear bits
// 63:32 of x0, so stale high bits in the base register can't escape into the
// final host pointer. Reads base before writing w0, so base == w0 is allowed;
// in that case the caller must already have zero-extended w0 (an offset of 0
// then emits nothing). Clobbers only w0 and, on the fallback path, w17 (base
// is never w17: it is either w0 or an allocatable GPR, w22-w28).
inline void EmitGuestOffsetAdd(A64Emitter& e, const WReg& base,
                               uint32_t offset) {
  if (offset == 0) {
    if (base.getIdx() != 0) {
      e.mov(e.w0, base);  // Zero-extending copy only.
    }
    return;
  }
  // A 'sub #imm' of the magnitude wraps mod 2^32 exactly like an 'add' of the
  // two's-complement offset, so split on sign to use the imm12 forms.
  const bool negative = (offset & 0x80000000u) != 0;
  const uint32_t magnitude = negative ? (0u - offset) : offset;
  if (magnitude < (1u << 24)) {
    const uint32_t lo12 = magnitude & 0xFFFu;
    const uint32_t hi12 = magnitude >> 12;
    // One add/sub #imm12 [LSL #12], or two for offsets like 0x2A39. The
    // two-instruction split matches mov_imm+add in length while leaving w17
    // untouched.
    if (negative) {
      if (hi12 == 0) {
        e.sub(e.w0, base, lo12);
      } else if (lo12 == 0) {
        e.sub(e.w0, base, hi12, 12);
      } else {
        e.sub(e.w0, base, lo12);
        e.sub(e.w0, e.w0, hi12, 12);
      }
    } else {
      if (hi12 == 0) {
        e.add(e.w0, base, lo12);
      } else if (lo12 == 0) {
        e.add(e.w0, base, hi12, 12);
      } else {
        e.add(e.w0, base, lo12);
        e.add(e.w0, e.w0, hi12, 12);
      }
    }
    return;
  }
  // |offset| >= 16 MiB: never produced by the PPC frontend (displacements
  // stay within +-0x10000), but HIR doesn't bound src2, so keep a
  // materializing fallback.
  e.mov(e.w17, static_cast<uint64_t>(offset));
  e.add(e.w0, base, e.w17);
}

// Compute a guest memory address plus a constant-or-register offset,
// returning the XReg for [x21, xN] addressing; the result is always
// x0 = zext32(uint32(guest + offset)).
//
// The 0xE0000000 fixup (only when allocation_granularity() > 0x1000) is
// decided from and applied to the BASE address alone, before the offset is
// added - exactly as the unfused ComputeMemoryAddress + offset-add pair
// behaved. Note: the x64 backend fixes up the base+offset SUM instead
// (x64_seq_memory.cc, ComputeMemoryAddressOffset); that latent divergence
// predates this helper and is intentionally left unchanged here.
inline XReg ComputeMemoryAddressOffset(A64Emitter& e, const I64Op& guest,
                                       const I64Op& offset) {
  using namespace Xbyak_aarch64;
  if (!offset.is_constant) {
    // Register offset (no current PPC producer, but reachable through HIR):
    // zero-extend/fix up the base as usual, then one W-register add wraps the
    // guest arithmetic at 32 bits.
    ComputeMemoryAddress(e, guest);
    e.add(e.w0, e.w0, WReg(offset.reg().getIdx()));
    return e.x0;
  }
  const uint32_t offset_const = static_cast<uint32_t>(offset.constant());
  if (guest.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant());
    if (address >= 0xE0000000 &&
        xe::memory::allocation_granularity() > 0x1000) {
      address += 0x1000;
    }
    // Guest address arithmetic wraps at 32 bits.
    address += offset_const;
    e.mov(e.x0, static_cast<uint64_t>(address));
    return e.x0;
  }
  if (xe::memory::allocation_granularity() > 0x1000) {
    // The runtime fixup must see the zero-extended base before the offset is
    // applied; ComputeMemoryAddress emits exactly that block into w0.
    ComputeMemoryAddress(e, guest);
    EmitGuestOffsetAdd(e, e.w0, offset_const);
  } else {
    // Hot path (4 KiB pages, no fixup instructions): fold the zero-extending
    // base copy into the offset add itself - typically a single
    // 'add/sub w0, wBASE, #imm'.
    EmitGuestOffsetAdd(e, WReg(guest.reg().getIdx()), offset_const);
  }
  return e.x0;
}

// Flush denormal float32 lanes to zero in a NEON register (in-place).
// A float32 is denormal when 0 < abs(val) < 0x00800000.
// vreg must not equal sa or sb.
// This is needed because FPCR.FZ may not flush denormal inputs on all ARM64
// implementations (the ARM spec says input flushing is implementation-defined).
inline void FlushDenormals_V128(A64Emitter& e, int vreg, int sa = 2,
                                int sb = 3) {
  // val<<1 removes the sign bit and doubles the value.
  // Denormals become [0x00000002, 0x00FFFFFE]; zeros become 0x00000000.
  // (val<<1) - 1: wraps 0→0xFFFFFFFF (excluded),
  // denorms→[0x00000001,0x00FFFFFD]. Denormal iff ((val<<1) - 1) < 0x00FFFFFF
  // (unsigned).
  e.shl(VReg(sa).s4, VReg(vreg).s4, 1);
  e.movi(VReg(sb).s4, 1u);
  e.sub(VReg(sa).s4, VReg(sa).s4, VReg(sb).s4);
  e.mvni(VReg(sb).s4, 0xFFu, LSL, 24);  // 0x00FFFFFF
  e.cmhi(VReg(sb).s4, VReg(sb).s4,
         VReg(sa).s4);  // mask: all-1s for denormal lanes
  // Clear only bits 30:0 (preserve sign bit 31) so -denormal → -0, +denormal →
  // +0.
  e.ushr(VReg(sa).s4, VReg(sb).s4, 1);  // sa = mask with bit 31 cleared
  e.bic(VReg(vreg).b16, VReg(vreg).b16, VReg(sa).b16);
}

// Fixup for vmaxfp/vminfp when BOTH inputs are NaN.
// ARM64 fmax/fmin with DN=0 correctly propagates NaN when only one input is
// NaN, but when both are NaN it may quiet an SNaN differently than x64.
// x64 uses maxps(a,b)|maxps(b,a) which effectively gives src1|src2 for NaN
// lanes. We replicate that: use src1|src2 only for lanes where BOTH are NaN.
// Expects: v0=flushed src1, v1=flushed src2, v2=hardware fmax/fmin result.
// Modifies v2 in place. Clobbers v0, v1, v3.
inline void FixupVmxMaxMinNan(A64Emitter& e) {
  // Compute OR fallback first (before clobbering v0/v1).
  e.orr(VReg(3).b16, VReg(0).b16, VReg(1).b16);  // v3 = src1 | src2
  // Build "at least one not NaN" mask.
  e.fcmeq(VReg(0).s4, VReg(0).s4, VReg(0).s4);   // v0 = non-NaN mask for src1
  e.fcmeq(VReg(1).s4, VReg(1).s4, VReg(1).s4);   // v1 = non-NaN mask for src2
  e.orr(VReg(0).b16, VReg(0).b16, VReg(1).b16);  // v0 = 1 where at least one ok
  // BSL: mask=1 → v2 (fmax result), mask=0 → v3 (src1|src2 for both-NaN)
  e.bsl(VReg(0).b16, VReg(2).b16, VReg(3).b16);
  e.mov(VReg(2).b16, VReg(0).b16);
}

// Prepare two V128 operands for a VMX FP operation: copy to scratch v0/v1
// and flush denormals. Returns the flushed register indices (always 0 and 1).
template <typename T1, typename T2>
inline void PrepareVmxFpSources(A64Emitter& e, const T1& op1, const T2& op2,
                                int& out_s1, int& out_s2) {
  int s1 = SrcVReg(e, op1, 0);
  int s2 = SrcVReg(e, op2, 1);
  // Copy to scratch v0/v1 so we don't modify live allocated registers.
  if (s1 != 0) {
    e.mov(VReg(0).b16, VReg(s1).b16);
  }
  if (s2 != 1) {
    e.mov(VReg(1).b16, VReg(s2).b16);
  }
  // Flush denormal inputs in software only if FPCR.FZ doesn't handle it.
  if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    FlushDenormals_V128(e, 0);
    FlushDenormals_V128(e, 1);
  }
  out_s1 = 0;
  out_s2 = 1;
}

// Fix PPC NaN propagation for V128 float32 lanes after a NEON FP operation.
// If sources_saved, expects v0/v1 to hold src1/src2; otherwise, copies the
// source registers there only after confirming that the result contains NaN.
// Modifies result_reg in place. Clobbers v0, v1, v3, w0, w16, w17.
// PPC rule: first NaN by operand position wins; SNaN is quieted (bit 22 set).
// If neither input was NaN but the op generated NaN (e.g., inf-inf),
// use the PPC default NaN (0xFFC00000).
inline void FixupVmxNan_V128(A64Emitter& e, int result_reg = 2,
                             int src1_reg = 0, int src2_reg = 1,
                             bool sources_saved = true) {
  using namespace Xbyak_aarch64;
  auto& done = e.NewCachedLabel();

  // Fast path: if no result lane is NaN, skip entirely.
  e.fcmeq(VReg(3).s4, VReg(result_reg).s4,
          VReg(result_reg).s4);                  // all-1s for non-NaN
  e.uminv(SReg(3), VReg(3).s4);                 // min across lanes
  e.fmov(e.w0, SReg(3));
  e.cbnz(e.w0, done);  // all non-NaN → skip

  // A direct-register binary op can defer these copies until a NaN result is
  // found when its destination did not alias either source.
  if (!sources_saved) {
    if (src1_reg != 0) {
      e.mov(VReg(0).b16, VReg(src1_reg).b16);
    }
    if (src2_reg != 1) {
      e.mov(VReg(1).b16, VReg(src2_reg).b16);
    }
  }

  // Save s1/s2 to stack for scalar lane extraction.
  e.str(QReg(0), ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH)));
  e.str(QReg(1),
        ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) + 16));

  // NaN threshold: (val<<1) > 0xFF000000 means val is NaN.
  e.mov(e.w16, 0xFF000000u);

  for (int lane = 0; lane < 4; lane++) {
    auto& lane_ok = e.NewCachedLabel();
    auto& s1_not_nan = e.NewCachedLabel();
    auto& use_default = e.NewCachedLabel();

    // Check if result[lane] is NaN.
    e.umov(e.w0, VReg(result_reg).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, lane_ok);

    // Result is NaN. Check s1[lane].
    e.ldr(e.w0, ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) +
                              lane * 4));
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, s1_not_nan);

    // s1 is NaN: quiet it and insert.
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(s1_not_nan);
    // Check s2[lane].
    e.ldr(e.w0, ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) +
                              16 + lane * 4));
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, use_default);

    // s2 is NaN: quiet it and insert.
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(use_default);
    // Generated NaN (neither input was NaN): use PPC default NaN.
    e.mov(e.w0, 0xFFC00000u);
    e.ins(VReg(result_reg).s4[lane], e.w0);

    e.L(lane_ok);
  }

  e.L(done);
}

// Fix PPC NaN propagation for V128 FMA result (3 source operands).
// Expects: result in v2, flushed sources saved on stack at:
//   GUEST_SCRATCH + 0  = src1 (16 bytes)
//   GUEST_SCRATCH + 16 = src2 (16 bytes)
//   GUEST_SCRATCH + 32 = src3 (16 bytes)
// PPC rule: first NaN by operand position (src1 > src2 > src3) wins.
// Clobbers v0, v1, v3, w0, w16, w17.
inline void FixupVmxNan_V128_Fma(A64Emitter& e) {
  using namespace Xbyak_aarch64;
  auto& done = e.NewCachedLabel();

  // Fast path: if no result lane is NaN, skip entirely.
  e.fcmeq(VReg(3).s4, VReg(2).s4, VReg(2).s4);
  e.uminv(SReg(3), VReg(3).s4);
  e.fmov(e.w0, SReg(3));
  e.cbnz(e.w0, done);

  // NaN threshold constant.
  e.mov(e.w16, 0xFF000000u);

  for (int lane = 0; lane < 4; lane++) {
    auto& lane_ok = e.NewCachedLabel();
    auto& s1_not_nan = e.NewCachedLabel();
    auto& s2_not_nan = e.NewCachedLabel();
    auto& use_default = e.NewCachedLabel();

    // Check if result[lane] is NaN.
    e.umov(e.w0, VReg(2).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, lane_ok);

    // Result is NaN. Check src1[lane].
    e.ldr(e.w0, ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) +
                              lane * 4));
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, s1_not_nan);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(2).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(s1_not_nan);
    // Check src2[lane].
    e.ldr(e.w0, ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) +
                              16 + lane * 4));
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, s2_not_nan);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(2).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(s2_not_nan);
    // Check src3[lane].
    e.ldr(e.w0, ptr(e.sp, static_cast<int32_t>(StackLayout::GUEST_SCRATCH) +
                              32 + lane * 4));
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, use_default);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(2).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(use_default);
    e.mov(e.w0, 0xFFC00000u);
    e.ins(VReg(2).s4[lane], e.w0);

    e.L(lane_ok);
  }

  e.L(done);
}

// Direct-register variant for a NaN-fixup FMA. The direct caller guarantees
// that result and all three sources are allocated registers outside v0-v3 and
// that result doesn't alias a source, so the NaN slow path can read the
// original operands without first spilling them to GUEST_SCRATCH.
inline void FixupVmxNan_V128_FmaDirect(A64Emitter& e, int result_reg,
                                       int src1_reg, int src2_reg,
                                       int src3_reg) {
  using namespace Xbyak_aarch64;
  auto& done = e.NewCachedLabel();

  e.fcmeq(VReg(3).s4, VReg(result_reg).s4, VReg(result_reg).s4);
  e.uminv(SReg(3), VReg(3).s4);
  e.fmov(e.w0, SReg(3));
  e.cbnz(e.w0, done);
  e.mov(e.w16, 0xFF000000u);

  for (int lane = 0; lane < 4; lane++) {
    auto& lane_ok = e.NewCachedLabel();
    auto& s1_not_nan = e.NewCachedLabel();
    auto& s2_not_nan = e.NewCachedLabel();
    auto& use_default = e.NewCachedLabel();

    e.umov(e.w0, VReg(result_reg).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, lane_ok);

    e.umov(e.w0, VReg(src1_reg).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, s1_not_nan);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(s1_not_nan);
    e.umov(e.w0, VReg(src2_reg).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, s2_not_nan);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(s2_not_nan);
    e.umov(e.w0, VReg(src3_reg).s4[lane]);
    e.lsl(e.w17, e.w0, 1);
    e.cmp(e.w17, e.w16);
    e.b(LS, use_default);
    e.orr(e.w0, e.w0, static_cast<uint64_t>(1u << 22));
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.b(lane_ok);

    e.L(use_default);
    e.mov(e.w0, 0xFFC00000u);
    e.ins(VReg(result_reg).s4[lane], e.w0);
    e.L(lane_ok);
  }

  e.L(done);
}

// VMX float32x4 binary operations with full PPC semantics.
enum class VmxFpBinOp { Add, Sub, Mul, Div };

// ARM three-register binary operations read both sources before writing the
// destination, including when it aliases either input. The caller gates on
// hardware FZ; when PPC NaN fixups are enabled, preserve aliased sources and
// apply the same lane fixup to the direct result.
inline bool TryEmitVmxBinaryDirect(A64Emitter& e, int dest, int src1, int src2,
                                   VmxFpBinOp op) {
  const bool nan_fixup = cvars::a64_vmx_nan_fixup;
  if (!cvars::a64_vmx_direct_registers ||
      !e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs) ||
      (nan_fixup && dest < 4)) {
    return false;
  }

  const bool sources_alias_dest = dest == src1 || dest == src2;
  if (nan_fixup && sources_alias_dest) {
    if (src1 != 0) e.mov(VReg(0).b16, VReg(src1).b16);
    if (src2 != 1) e.mov(VReg(1).b16, VReg(src2).b16);
  }

  switch (op) {
    case VmxFpBinOp::Add: e.fadd(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4); break;
    case VmxFpBinOp::Sub: e.fsub(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4); break;
    case VmxFpBinOp::Mul: e.fmul(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4); break;
    case VmxFpBinOp::Div: e.fdiv(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4); break;
  }

  if (nan_fixup) {
    FixupVmxNan_V128(e, dest, src1, src2, sources_alias_dest);
  }
  return true;
}

// FMLA reads the destination as its accumulator. Copying there first is safe
// only if it does not overwrite a multiplicand, or already is the accumulator.
// MUL_SUB preserves the original FNEG + FMLA ordering (including signed zero).
inline bool TryEmitVmxFmaDirect(A64Emitter& e, int dest, int src1, int src2,
                                int src3, bool subtract) {
  if (!cvars::a64_vmx_direct_registers || cvars::a64_vmx_nan_fixup ||
      !e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
    return false;
  }
  if (subtract) {
    if (dest == src1 || dest == src2) return false;
    e.fneg(VReg(dest).s4, VReg(src3).s4);
  } else {
    if (dest != src3 && (dest == src1 || dest == src2)) return false;
    if (dest != src3) e.mov(VReg(dest).b16, VReg(src3).b16);
  }
  e.fmla(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4);
  return true;
}

inline bool TryEmitVmxFmaDirectWithNanFixup(A64Emitter& e, int dest,
                                            int src1, int src2, int src3,
                                            bool subtract) {
  if (!cvars::a64_vmx_direct_registers || !cvars::a64_vmx_nan_fixup ||
      !e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs) || dest < 4 ||
      src1 < 4 || src2 < 4 || src3 < 4 || dest == src1 || dest == src2 ||
      dest == src3) {
    return false;
  }

  if (subtract) {
    e.fneg(VReg(dest).s4, VReg(src3).s4);
  } else {
    e.mov(VReg(dest).b16, VReg(src3).b16);
  }
  e.fmla(VReg(dest).s4, VReg(src1).s4, VReg(src2).s4);
  FixupVmxNan_V128_FmaDirect(e, dest, src1, src2, src3);
  return true;
}

// Execute a VMX float32x4 binary operation with denormal flushing and PPC NaN
// propagation.  Result goes into dest_idx.
// Clobbers v0-v3, w0, w16, w17.
template <typename T1, typename T2>
inline void EmitVmxFpBinOp_V128(A64Emitter& e, int dest_idx, const T1& src1,
                                const T2& src2, VmxFpBinOp op) {
  EmitWithVmxFpcr(e, [&] {
    if (cvars::a64_vmx_direct_registers &&
        e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
      const int src1_reg = SrcVReg(e, src1, 0);
      const int src2_reg = SrcVReg(e, src2, 1);
      if (TryEmitVmxBinaryDirect(e, dest_idx, src1_reg, src2_reg, op)) return;
    }
    // Flush input denormals → v0=s1, v1=s2.
    int s1, s2;
    PrepareVmxFpSources(e, src1, src2, s1, s2);

    // Hardware FP op → v2.
    switch (op) {
      case VmxFpBinOp::Add:
        e.fadd(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Sub:
        e.fsub(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Mul:
        e.fmul(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
      case VmxFpBinOp::Div:
        e.fdiv(VReg(2).s4, VReg(s1).s4, VReg(s2).s4);
        break;
    }

    // PPC NaN propagation fixup (fast-path skip when no NaN).
    // When disabled, NEON's IEEE default NaN propagation is kept as-is,
    // matching the x64 backend (which emits no fixup on these ops).
    if (cvars::a64_vmx_nan_fixup) {
      FixupVmxNan_V128(e);
    }

    // Flush output denormals. FPCR.FZ guarantees output flushing per the
    // ARM spec, so skip when FZ is known to also handle inputs (implying
    // the core fully supports FZ denormal handling).
    if (!e.IsFeatureEnabled(xe::arm64::kA64FZFlushesInputs)) {
      FlushDenormals_V128(e, 2, 0, 1);
    }

    // Move to dest.
    e.mov(VReg(dest_idx).b16, VReg(2).b16);
  });
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_SEQ_UTIL_H_
