/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/a64/a64_volatile_gpr.h"

#include <atomic>
#include <cstddef>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif
#if XE_PLATFORM_MAC
#include <pthread.h>
#endif
#include "xenia/cpu/backend/a64/a64_assembler.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

DEFINE_bool(a64_vmx_direct_registers, true,
            "Emit VMX FP operations directly in allocated NEON registers "
            "when NaN fixups are disabled and hardware FZ handles denormals. "
            "Preserves aliased inputs and uses the original path otherwise.", "CPU");
UPDATE_from_bool(a64_vmx_direct_registers, 2026, 10, 5, 12, false);

DEFINE_bool(a64_extended_gpr_allocator, true,
            "Init-time: allocate 13 SSA integer registers instead of 7, "
            "preserving the6 additional AAPCS64 volatile GPRs at native "
            "calls. Guest call boundaries retain their context semantics.",
            "CPU");
UPDATE_from_bool(a64_extended_gpr_allocator, 2026, 10, 5, 12, false);

DEFINE_bool(a64_extern_thunk_no_vec, true,
            "Init-time: call kernel exports (CALL_EXTERN) through a thunk that "
            "skips the q4-q31 save/restore (56 memory operations per call). "
            "No float or vector value is in a register across CALL_EXTERN: it "
            "ends its block and stops context forwarding; a call with one "
            "live across still takes the full thunk.",
            "CPU");
DEFINE_int64(a64_max_stackpoints, 65536,
             "Max number of host->guest stack mappings we can record.", "a64");

DEFINE_bool(a64_enable_host_guest_stack_synchronization, true,
            "Records entries for guest/host stack mappings at function starts "
            "and checks for reentry at return sites. Has slight performance "
            "impact, but fixes crashes in games that use setjmp/longjmp.",
            "a64");

DEFINE_bool(a64_longjmp_target_frame, true,
            "After a guest longjmp resume on the host frame of the function "
            "that is jumped back into (false: the frame of the function it "
            "had called, which is what this backend used to pick, and only "
            "for jumps over three or more frames).",
            "CPU");

DEFINE_bool(a64_longjmp_sync_at_resolve, true,
            "After a guest longjmp the host stack is put back by a helper "
            "the function resolver jumps through on the way to the return "
            "site, instead of by a check emitted after every guest call (a "
            "load and a branch each).",
            "CPU");
DEFINE_bool(a64_patchable_guest_calls, true,
            "Emit direct guest calls as a BL to a lookup stub, rewritten to a "
            "BL straight to the callee once it has code (and again whenever "
            "code is placed for that address). Skips the indirection table "
            "lookup and the indirect branch on every call.",
            "a64");

DEFINE_bool(a64_near_branches, true,
            "Emit the conditional branches of guest functions (b.cond, cbz, "
            "cbnz) direct. They reach +/-1 MiB, so a function they don't "
            "reach across is emitted again with each one routed through an "
            "unconditional branch. The long-range form costs an extra "
            "instruction where the branch is taken and a taken branch where "
            "a direct one falls through (Xendroid-Plus measured 4-9% smaller "
            "code and 3-7% fewer cycles on the busiest guest thread).",
            "a64");

DEFINE_bool(a64_fpu_nan_fixup, false,
            "Emulate PPC NaN propagation on scalar fmadd/fmsub (first NaN "
            "operand by position, PPC default NaN for generated NaNs). When "
            "disabled, the hardware result is used as upstream Xenia's x64 "
            "backend does (6 fewer instructions per FMA).",
            "a64");
UPDATE_from_bool(a64_fpu_nan_fixup, 2026, 10, 5, 12, true);
DEFINE_bool(a64_fpu_nan_fixup_result_check, true,
            "Use a result-only NaN check on scalar FPU operations when PPC NaN "
            "fixup is enabled; NaN cases use a cold positional-propagation "
            "tail. Has no effect when a64_fpu_nan_fixup is disabled.",
            "a64");

DEFINE_bool(a64_vconst_pool, true,
            "Load 128-bit vector constants that have no immediate encoding "
            "with one LDR (literal) from a per-function pool instead of up to "
            "10 MOV/MOVK/FMOV/INS instructions.",
            "CPU");
UPDATE_from_bool(a64_vconst_pool, 2026, 10, 5, 12, false);
DEFINE_bool(a64_vmx_nan_fixup, false,
            "Emulate PPC NaN propagation on VMX float ops: the first NaN "
            "operand (by position) is returned, quieted; generated NaNs "
            "become the PPC default NaN (0xFFC00000). When disabled, the "
            "IEEE/NEON default NaN behavior is used instead, which matches "
            "the x64 backend and is faster; rare titles that hash or compare "
            "raw float bits may behave differently.",
            "a64");
UPDATE_from_bool(a64_vmx_nan_fixup, 2026, 10, 5, 12, true);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

// Resolve a guest function at runtime. Called by the resolve thunk when
// a guest address has not yet been compiled.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

uint32_t FindStackpointSyncDepth(const A64BackendStackpoint* stackpoints,
                                 uint32_t current_depth, uint32_t guest_sp,
                                 uint32_t target_start, uint32_t target_end) {
  if (!stackpoints || current_depth == 0) {
    return 0;
  }

  if (!cvars::a64_longjmp_target_frame) {
    uint32_t idx = current_depth - 1;
    uint32_t frames_skipped = 0;
    while (idx != 0xFFFFFFFFu && guest_sp > stackpoints[idx].guest_stack_) {
      --idx;
      ++frames_skipped;
    }
    // >1 frames skipped = real longjmp, not an early SP restore.
    if (idx == 0xFFFFFFFFu || frames_skipped <= 1) {
      return 0;
    }
    return idx + 1;
  }

  // A stackpoint holds the host SP of its function after the frame was
  // allocated - what the code of that function expects - and the guest SP the
  // function was entered with, which is its caller's. The function jumped
  // back into is therefore the first one, from the top, entered with a guest
  // SP above the restored one: every function it called since, directly or
  // not, was entered at or below it.
  uint32_t idx = current_depth - 1;
  uint32_t frames_skipped = 0;
  while (idx != 0xFFFFFFFFu && guest_sp >= stackpoints[idx].guest_stack_) {
    --idx;
    ++frames_skipped;
  }
  // Nothing above it: the host SP already is that function's (or this is not
  // a jump back into a live frame at all).
  if (idx == 0xFFFFFFFFu || !frames_skipped) {
    return 0;
  }
  // The function right above must have been called from the target: its
  // return address lies in it. Otherwise these stackpoints are not a chain
  // of calls leading here (another stack, a frame left by a tail jump).
  const uint32_t called_from = stackpoints[idx + 1].guest_return_address_;
  if (target_end > target_start &&
      (called_from < target_start || called_from >= target_end)) {
    return 0;
  }
  return idx + 1;
}

// ==========================================================================
// A64HelperEmitter — generates thunks using xbyak_aarch64.
// ==========================================================================
class A64HelperEmitter : public A64Emitter {
 public:
  A64HelperEmitter(A64Backend* backend, XbyakA64Allocator* allocator);

  HostToGuestThunk EmitHostToGuestThunk();
  GuestToHostThunk EmitGuestToHostThunk();
  GuestToHostThunk EmitGuestToHostThunkNoVec();
  ResolveFunctionThunk EmitResolveFunctionThunk();
  void* EmitGuestAndHostSynchronizeStackHelper(bool resume_from_context);
  enum class ColdPath { kPreempt, kStackpointOverflow, kCallLookup,
                        kExternalLookup };
  void* EmitColdPathThunk(ColdPath path);
};

A64HelperEmitter::A64HelperEmitter(A64Backend* backend,
                                   XbyakA64Allocator* allocator)
    : A64Emitter(backend, allocator) {}

// --------------------------------------------------------------------------
// HostToGuestThunk
// --------------------------------------------------------------------------
// Called from host C++ code to enter JIT'd guest code.
//   x0 = target machine code address
//   x1 = PPCContext* (arg0)
//   x2 = return address value (arg1)
//
// ARM64 AAPCS64 calling convention:
//   Caller-saved: x0-x18, v0-v7, v16-v31
//   Callee-saved: x19-x28, x29(FP), x30(LR), d8-d15
//
// We save all callee-saved regs, set up context (x20) and membase (x21),
// then call the target. On return, restore and return to host.
HostToGuestThunk A64HelperEmitter::EmitHostToGuestThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // Allocate thunk stack frame.
  // Save x29(FP) and x30(LR) first, then callee-saved GPRs and NEON regs.
  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;

  // sub sp, sp, #thunk_stack
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save callee-saved GPRs: x19-x28, x29, x30
  stp(x19, x20, ptr(sp, 0x00));
  stp(x21, x22, ptr(sp, 0x10));
  stp(x23, x24, ptr(sp, 0x20));
  stp(x25, x26, ptr(sp, 0x30));
  stp(x27, x28, ptr(sp, 0x40));
  stp(x29, x30, ptr(sp, 0x50));

  // Save callee-saved NEON regs: full q8-q15 (JIT uses all 128 bits).
  stp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x60));
  stp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x80));
  stp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0xA0));
  stp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0xC0));

  code_offsets.body = getSize();

  // Set up guest execution state.
  // x20 = context (PPCContext*)
  mov(x20, x1);
  // x19 = backend context (immediately before PPCContext in memory)
  sub(x19, x20, static_cast<uint32_t>(sizeof(A64BackendContext)));
  // x21 = virtual_membase (loaded from context)
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
  // Restore the guest scalar FPCR on every host->guest entry so host-side
  // work done before the call can't leak a stale rounding / non-IEEE mode.
  ldr(w5,
      ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  msr(3, 3, 4, 4, 0, x5);
  mov(w5, 1);
  strb(w5, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, fpcr_live_mode))));
  // x0 still holds target, x2 holds return address.
  // The guest function's prolog stores x0 to GUEST_RET_ADDR on its stack
  // frame. Move the target to a scratch reg and put the guest return
  // address into x0.
  mov(x9, x0);  // x9 = target (scratch reg)
  // Pass guest return address in x0 (convention for guest function entry).
  mov(x0, x2);  // x0 = guest return address

  // Call the guest function.
  blr(x9);

  code_offsets.epilog = getSize();

  // Restore callee-saved NEON regs (full q8-q15).
  ldp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0xC0));
  ldp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0xA0));
  ldp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x80));
  ldp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x60));

  // Restore callee-saved GPRs.
  ldp(x29, x30, ptr(sp, 0x50));
  ldp(x27, x28, ptr(sp, 0x40));
  ldp(x25, x26, ptr(sp, 0x30));
  ldp(x23, x24, ptr(sp, 0x20));
  ldp(x21, x22, ptr(sp, 0x10));
  ldp(x19, x20, ptr(sp, 0x00));

  // Deallocate stack.
  add(sp, sp, static_cast<uint32_t>(thunk_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;
  func_info.lr_save_offset = 0x058;  // stp x29, x30, [sp, #0x50]

  void* fn = Emplace(func_info);
  return reinterpret_cast<HostToGuestThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestToHostThunk
// --------------------------------------------------------------------------
// Called from guest JIT code to transition into a host (C++) function.
//   x0 = target host function
//   x1 = arg0
//   x2 = arg1
//
// We save volatile guest registers that we need to preserve across the
// host call, then call the host function with context as the first arg.
GuestToHostThunk A64HelperEmitter::EmitGuestToHostThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // The guest JIT uses v4-v15, v16-v31 as allocatable VEC regs.
  // v0-v7, v16-v31 are caller-saved in AAPCS64 (fully clobbered by C).
  // v8-v15 lower 64 bits are callee-saved, but upper 64 bits are not.
  // We must save all guest-allocated VEC regs (full 128-bit Q regs).
  // GPRs x19-x28 are callee-saved in AAPCS64, so the C function preserves them.
  //
  // Stack layout:
  //   q4, q5       sp + 0x000  (32 bytes)
  //   q6, q7       sp + 0x020
  //   q8, q9       sp + 0x040
  //   q10, q11     sp + 0x060
  //   q12, q13     sp + 0x080
  //   q14, q15     sp + 0x0A0
  //   q16, q17     sp + 0x0C0
  //   q18, q19     sp + 0x0E0
  //   q20, q21     sp + 0x100
  //   q22, q23     sp + 0x120
  //   q24, q25     sp + 0x140
  //   q26, q27     sp + 0x160
  //   q28, q29     sp + 0x180
  //   q30, q31     sp + 0x1A0
  //   x29, x30     sp + 0x1C0
  //   Total: 0x1D0 = 464 bytes (16-byte aligned)
  const bool extended_gprs = backend()->extended_gpr_allocator();
  const size_t g2h_stack = 464 +
      (extended_gprs ? kExtendedGprSaveBytes : 0);
  sub(sp, sp, static_cast<uint32_t>(g2h_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save guest-allocated VEC regs (full Q = 128-bit).
  stp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));
  stp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  stp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x040));
  stp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x060));
  stp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0x080));
  stp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0x0A0));
  stp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x0C0));
  stp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x0E0));
  stp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x100));
  stp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x120));
  stp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x140));
  stp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x160));
  stp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x180));
  stp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x1A0));
  // Save x29/x30 (FP/LR).
  stp(x29, x30, ptr(sp, 0x1C0));
  if (extended_gprs) SaveExtendedGprs(*this, 0x1D0);

  code_offsets.body = getSize();

  // Host C code runs with the AAPCS64 default FPCR (round to nearest, no
  // flush-to-zero), not the guest's scalar mode: mtfsf can put rounding
  // controls or FPSCR.NI (FZ) in fpcr_fpu, which host code must not inherit.
  Xbyak_aarch64::Label host_fpcr_ready;
  mrs(x11, 3, 3, 4, 4, 0);  // mrs x11, FPCR
  cbz(x11, host_fpcr_ready);
  msr(3, 3, 4, 4, 0, xzr);
  L(host_fpcr_ready);

  // Call host function.
  // AAPCS64: x0=first arg. We set x0=context (from x20).
  mov(x9, x0);   // x9 = target function (scratch)
  mov(x0, x20);  // x0 = PPCContext* (our context reg)
  // x1, x2, x3 already hold args from the caller.
  blr(x9);

  // Host callbacks may change FPCR. Restore the guest scalar FPCR before
  // resuming the JIT so later guest ops observe the cached PPC mode.
  // x19 (backend context) is callee-saved, so it survives the host call.
  ldr(w5,
      ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  msr(3, 3, 4, 4, 0, x5);
  mov(w5, 1);
  strb(w5, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, fpcr_live_mode))));

  code_offsets.epilog = getSize();

  // Restore the extra SSA GPRs after the native call and FPCR repair.
  if (extended_gprs) RestoreExtendedGprs(*this, 0x1D0);
  // Restore.
  ldp(x29, x30, ptr(sp, 0x1C0));
  ldp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x1A0));
  ldp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x180));
  ldp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x160));
  ldp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x140));
  ldp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x120));
  ldp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x100));
  ldp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x0E0));
  ldp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x0C0));
  ldp(Xbyak_aarch64::QReg(14), Xbyak_aarch64::QReg(15), ptr(sp, 0x0A0));
  ldp(Xbyak_aarch64::QReg(12), Xbyak_aarch64::QReg(13), ptr(sp, 0x080));
  ldp(Xbyak_aarch64::QReg(10), Xbyak_aarch64::QReg(11), ptr(sp, 0x060));
  ldp(Xbyak_aarch64::QReg(8), Xbyak_aarch64::QReg(9), ptr(sp, 0x040));
  ldp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  ldp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));

  add(sp, sp, static_cast<uint32_t>(g2h_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = g2h_stack;
  func_info.lr_save_offset = 0x1C8;  // stp x29, x30, [sp, #0x1C0]

  void* fn = Emplace(func_info);
  return reinterpret_cast<GuestToHostThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestToHostThunkNoVec
// --------------------------------------------------------------------------
// GuestToHostThunk without the q4-q31 save/restore, for CallExtern (from
// Xendroid-Plus fa341ce0, adapted): HIRBuilder::CallExtern ends its block and
// ContextPromotionPass treats CALL_EXTERN as a forwarding barrier, so no value
// is in a vector register across it (CallExtern still checks). The extra SSA
// GPRs are kept, as in the full thunk.
GuestToHostThunk A64HelperEmitter::EmitGuestToHostThunkNoVec() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  //   x29, x30     sp + 0x00
  //   extra GPRs   sp + 0x10 (with the extended allocator)
  const bool extended_gprs = backend()->extended_gpr_allocator();
  const size_t g2h_stack =
      (size_t(16) + (extended_gprs ? kExtendedGprSaveBytes : 0) + 15) &
      ~size_t(15);
  sub(sp, sp, static_cast<uint32_t>(g2h_stack));
  code_offsets.prolog_stack_alloc = getSize();
  stp(x29, x30, ptr(sp, 0x00));
  if (extended_gprs) SaveExtendedGprs(*this, 0x10);

  code_offsets.body = getSize();

  // The FPCR handling of GuestToHostThunk.
  Xbyak_aarch64::Label host_fpcr_ready;
  mrs(x11, 3, 3, 4, 4, 0);  // mrs x11, FPCR
  cbz(x11, host_fpcr_ready);
  msr(3, 3, 4, 4, 0, xzr);
  L(host_fpcr_ready);

  mov(x9, x0);   // x9 = target function (scratch)
  mov(x0, x20);  // x0 = PPCContext* (our context reg)
  blr(x9);

  ldr(w5,
      ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  msr(3, 3, 4, 4, 0, x5);
  mov(w5, 1);
  strb(w5, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, fpcr_live_mode))));

  code_offsets.epilog = getSize();

  if (extended_gprs) RestoreExtendedGprs(*this, 0x10);
  ldp(x29, x30, ptr(sp, 0x00));
  add(sp, sp, static_cast<uint32_t>(g2h_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = g2h_stack;
  func_info.lr_save_offset = 0x08;  // stp x29, x30, [sp, #0x00]

  void* fn = Emplace(func_info);
  return reinterpret_cast<GuestToHostThunk>(fn);
}

// --------------------------------------------------------------------------
// ResolveFunctionThunk
// --------------------------------------------------------------------------
// Called when guest code calls an unresolved function address.
// The indirection table initially points all entries here.
// We call ResolveFunction to compile/lookup the target, then jump to it.
//
// On entry from the indirection table:
//   w16 = guest PPC address (loaded by the call sequence)
//   x20 = context
//   x30 = return address (from the BLR that got us here)
ResolveFunctionThunk A64HelperEmitter::EmitResolveFunctionThunk() {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save x29/x30 and x0 (guest return address, needed by the resolved
  // function's prolog). x19 is callee-saved so it survives the C call.
  stp(x29, x30, ptr(sp, 0x50));
  stp(x0, x19, ptr(sp, 0x00));  // save x0 (guest ret addr) and x19

  code_offsets.body = getSize();

  // Call ResolveFunction(context, target_address).
  mov(x0, x20);  // x0 = PPCContext*
  mov(x1, x16);  // x1 = guest address (32-bit in w16)
  // Load address of ResolveFunction.
  mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
  blr(x9);
  // x0 now holds the resolved host machine code address.
  mov(x9, x0);

  code_offsets.epilog = getSize();

  // Restore x0 (guest return address) and saved regs.
  ldp(x0, x19, ptr(sp, 0x00));
  ldp(x29, x30, ptr(sp, 0x50));
  add(sp, sp, static_cast<uint32_t>(thunk_stack));

  cbz(x9, 8);   // skip br x9 if null, fall through to brk
  br(x9);       // Jump to the resolved function (tail call — preserves LR).
  brk(0xF000);  // Resolution failed — trap for debugging.

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;
  func_info.lr_save_offset = 0x058;  // stp x29, x30, [sp, #0x50]

  void* fn = Emplace(func_info);
  return reinterpret_cast<ResolveFunctionThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestAndHostSynchronizeStackHelper
// --------------------------------------------------------------------------
// Called when ResolveFunction detected a longjmp return-site reentry. Restores
// the host SP for the existing frame and jumps back to the caller.
//
// On entry (set by the tail-emitted sync check in the guest function):
//   x8  = return address (where to jump after fixup)
//   x19 = A64BackendContext*
void* A64HelperEmitter::EmitGuestAndHostSynchronizeStackHelper(
    bool resume_from_context) {
  using namespace Xbyak_aarch64;
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // x19 = backend context pointer (already set up by HostToGuestThunk)

  if (resume_from_context) {
    // Entered straight from the resolve thunk (a64_longjmp_sync_at_resolve):
    // the return site to resume at was left in the context.
    ldr(x8, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                    pending_sync_target))));
  }

  // x4 = stackpoints array pointer
  ldr(x4, ptr(x19, static_cast<uint32_t>(
                        offsetof(A64BackendContext, stackpoints))));
  // w5 = current_stackpoint_depth
  ldr(w5, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));

  // w6 = target depth computed by ResolveFunction.
  ldr(w6, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  auto& underflow = NewCachedLabel();

  cbz(x4, underflow);
  // A zero target means this helper was called without a pending repair.
  cbz(w6, underflow);
  // The pending target must not be deeper than the current live depth.
  cmp(w6, w5);
  b(HI, underflow);

  // x7 = &stackpoints[target_depth - 1]
  sub(w6, w6, 1);

  mov(w7, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
  umull(x7, w6, w7);
  add(x7, x4, x7);

  // Restore host SP from stackpoints[index].host_stack_. A64 stackpoints are
  // recorded after the function frame allocation, so this is already the SP
  // expected by the return-site code.
  ldr(x16, ptr(x7, static_cast<uint32_t>(
                        offsetof(A64BackendStackpoint, host_stack_))));
  mov(sp, x16);

  // Update current_stackpoint_depth = index + 1
  // (the entry we restored to has been consumed)
  add(w6, w6, 1);
  str(w6, ptr(x19, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));
  mov(w3, 0);
  str(w3, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));

  // Jump back to the caller.
  br(x8);

  L(underflow);
  // Should be impossible — stackpoint array underflowed.
  brk(0xF001);  // assertion failure

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  return Emplace(func_info);
}

// The out-of-line halves of sequences every guest function has many of
// (a64_shared_cold_paths), once here instead of once per site. Reached with a
// load from the backend context and a branch; none of them needs a frame.
void* A64HelperEmitter::EmitColdPathThunk(ColdPath path) {
  using namespace Xbyak_aarch64;
  const size_t start = getSize();
  switch (path) {
    case ColdPath::kPreempt: {
      // Called (blr) from a preempt check that found the flag raised; x20 =
      // guest context. Returns to the caller, through the yield if there is
      // a scheduler to yield to.
      Label no_handler;
      strb(wzr, ptr(x20, static_cast<uint32_t>(
                             offsetof(ppc::PPCContext, preempt_requested))));
      mov(x0, reinterpret_cast<uint64_t>(
                  &xe::cpu::backend::preempt_yield_handler));
      ldr(x0, ptr(x0));
      cbz(x0, no_handler);
      mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
      br(x9);
      L(no_handler);
      ret();
      break;
    }
    case ColdPath::kStackpointOverflow:
      // Jumped to; the handler does not return.
      mov(x0, reinterpret_cast<uint64_t>(
                  &A64Emitter::HandleStackpointOverflowError));
      mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
      br(x9);
      break;
    case ColdPath::kCallLookup: {
      // Jumped to from the stub of a direct guest call not bound to its
      // callee (yet): w16 = guest address, x0 and x30 as the callee wants
      // them. Clobbers x5, x7, x9.
      if (!code_cache_->encoded_indirection()) {
        ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
        br(x9);
        break;
      }
      Label external;
      ldp(x7, x5,
          ptr(x19, static_cast<int32_t>(
                       offsetof(A64BackendContext, indirection_table_bias))));
      add(x7, x7, w16, UXTW);
      ldr(w9, ptr(x7, static_cast<uint32_t>(0)));
      tbnz(w9, 31, external);
      add(x9, x5, w9, UXTW);
      br(x9);
      L(external);
      and_(w5, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x7, code_cache_->external_indirection_table_base_address());
      lsl(x5, x5, 3);
      add(x7, x7, x5);
      ldr(x9, ptr(x7, static_cast<uint32_t>(0)));
      br(x9);
      break;
    }
    case ColdPath::kExternalLookup:
      // Called (blr) with the tagged indirection entry in w9: the host
      // address it stands for, in x9. Clobbers x5, x7.
      and_(w5, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x7, code_cache_->external_indirection_table_base_address());
      lsl(x5, x5, 3);
      add(x7, x7, x5);
      ldr(x9, ptr(x7, static_cast<uint32_t>(0)));
      ret();
      break;
  }
  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize() - start;
  func_info.code_size.body = getSize() - start;
  func_info.stack_size = 0;
  return Emplace(func_info);
}

// ==========================================================================
// Reservation helpers. A global per-granule generation counter so
// cross-thread stores invalidate other threads' reservations. A CAS on the
// value alone would be ABA-vulnerable.
// ==========================================================================
namespace {

A64BackendContext* BackendContextFromRawContext(void* raw_context) {
  return reinterpret_cast<A64BackendContext*>(
      reinterpret_cast<uint8_t*>(raw_context) - sizeof(A64BackendContext));
}

std::atomic<uint32_t>& ReserveGranule(ReserveHelper* reserve_helper,
                                      uint32_t guest_address) {
  const uint32_t granule = guest_address >> A64_RESERVE_GRANULE_SHIFT;
  return reserve_helper->generations[granule & A64_RESERVE_ENTRY_MASK];
}

extern "C" uint64_t TryAcquireReservationHelper(void* raw_context,
                                                uint64_t guest_address) {
  auto* bctx = BackendContextFromRawContext(raw_context);
  auto& granule =
      ReserveGranule(bctx->reserve_helper_, uint32_t(guest_address));
  // snapshot the generation first, the acquire pins the value read after
  bctx->reserve_generation = granule.load(std::memory_order_acquire);
  bctx->reserve_address = uint32_t(guest_address);
  // lwarx replaces any reservation this thread already held
  bctx->flags |= 1u << kA64BackendHasReserveBit;
  return 1;
}

template <typename T>
T ReservedLoadImpl(ppc::PPCContext* context, uint32_t address) {
  T* host_address = context->TranslateVirtual<T*>(address);
  TryAcquireReservationHelper(context, address);
  auto* bctx = BackendContextFromRawContext(context);
  const T raw = *host_address;
  bctx->cached_reserve_value_ = static_cast<uint64_t>(raw);
  return xe::byte_swap(raw);
}

template <typename T>
uint64_t ReservedStoreImpl(void* raw_context, uint64_t guest_address,
                           uint64_t host_address, uint64_t value) {
  auto* bctx = BackendContextFromRawContext(raw_context);
  const uint32_t reserve_flag = 1u << kA64BackendHasReserveBit;
  const bool had_reservation = (bctx->flags & reserve_flag) != 0;
  // stwcx. always clears the reservation, stored or not
  bctx->flags &= ~reserve_flag;
  // the reservation must be for the address we're storing to
  if (!had_reservation || bctx->reserve_address != uint32_t(guest_address)) {
    return 0;
  }

  auto& granule =
      ReserveGranule(bctx->reserve_helper_, uint32_t(guest_address));
  // a store to this granule since our lwarx kills the reservation
  if (granule.load(std::memory_order_acquire) != bctx->reserve_generation) {
    return 0;
  }

  bool exchange_ok;
  if constexpr (sizeof(T) == sizeof(uint64_t)) {
    exchange_ok = xe::atomic_cas(
        bctx->cached_reserve_value_, uint64_t(value),
        reinterpret_cast<volatile uint64_t*>(uintptr_t(host_address)));
  } else {
    exchange_ok = xe::atomic_cas(
        uint32_t(bctx->cached_reserve_value_), uint32_t(value),
        reinterpret_cast<volatile uint32_t*>(uintptr_t(host_address)));
  }

  if (exchange_ok) {
    // the store landed, so kill other reservations on this granule
    granule.fetch_add(1, std::memory_order_release);
    if (xe::threading::guest_wake_generation_enabled) {
      xe::threading::BumpGuestWakeGeneration();
      // Match the native SC path: this wake came from the current guest thread,
      // so its own lock traffic must not prevent NoteIdleSpin from parking.
      ++static_cast<ppc::PPCContext*>(raw_context)->wake_self_bumps;
    }
  }
  return exchange_ok ? 1 : 0;
}

extern "C" uint64_t ReservedStore32Helper(void* raw_context,
                                          uint64_t guest_address,
                                          uint64_t host_address,
                                          uint64_t value) {
  return ReservedStoreImpl<uint32_t>(raw_context, guest_address, host_address,
                                     value);
}

extern "C" uint64_t ReservedStore64Helper(void* raw_context,
                                          uint64_t guest_address,
                                          uint64_t host_address,
                                          uint64_t value) {
  return ReservedStoreImpl<uint64_t>(raw_context, guest_address, host_address,
                                     value);
}

}  // namespace

uint32_t A64Backend::ReservedLoad32(ppc::PPCContext* context,
                                    uint32_t address) {
  return ReservedLoadImpl<uint32_t>(context, address);
}

uint64_t A64Backend::ReservedLoad64(ppc::PPCContext* context,
                                    uint32_t address) {
  return ReservedLoadImpl<uint64_t>(context, address);
}

bool A64Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                                 uint32_t value) {
  return ReservedStoreImpl<uint32_t>(
             context, address,
             reinterpret_cast<uint64_t>(
                 context->TranslateVirtual<uint32_t*>(address)),
             xe::byte_swap(value)) != 0;
}

bool A64Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                                 uint64_t value) {
  return ReservedStoreImpl<uint64_t>(
             context, address,
             reinterpret_cast<uint64_t>(
                 context->TranslateVirtual<uint64_t*>(address)),
             xe::byte_swap(value)) != 0;
}

// ==========================================================================
// ResolveFunction — runtime function resolution.
// ==========================================================================
uint64_t ResolveFunction(void* raw_context, uint64_t target_address) {
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  auto thread_state = guest_context->thread_state;
  assert_not_zero(target_address);

  // Longjmp re-entry: resume inside an existing function frame instead of
  // re-running its prolog. Mirrors x64_emitter.cc::ResolveFunction.
  auto* processor = thread_state->processor();
  if (cvars::a64_enable_host_guest_stack_synchronization &&
      target_address <= 0xFFFFFFFFu) {
    auto* module_for_address =
        processor->LookupModule(static_cast<uint32_t>(target_address));
    auto* xexmod = dynamic_cast<XexModule*>(module_for_address);
    if (xexmod) {
      InfoCacheFlags* flags = xexmod->GetInstructionAddressFlags(
          static_cast<uint32_t>(target_address));
      if (flags && flags->is_return_site) {
        uintptr_t host_address = 0;
        for (auto* entry : processor->FindFunctionsWithAddress(
                 static_cast<uint32_t>(target_address))) {
          auto* afunc = static_cast<A64Function*>(entry);
          host_address = afunc->MapGuestAddressToMachineCode(
              static_cast<uint32_t>(target_address));
          if (host_address &&
              afunc->machine_code() !=
                  reinterpret_cast<const uint8_t*>(host_address)) {
            auto* backend = static_cast<A64Backend*>(processor->backend());
            auto* backend_context =
                backend->BackendContextForGuestContext(guest_context);
            const uint32_t sync_depth = FindStackpointSyncDepth(
                backend_context->stackpoints,
                backend_context->current_stackpoint_depth,
                static_cast<uint32_t>(guest_context->r[1]), afunc->address(),
                afunc->end_address());
            if (sync_depth != 0) {
              // How often games do this is not known: the first few are
              // worth a line each.
              static std::atomic<uint32_t> logged{0};
              if (logged.load(std::memory_order_relaxed) < 16 &&
                  logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                XELOGI(
                    "A64 longjmp: into {:08X} (function {:08X}), guest sp "
                    "{:08X}, stackpoint depth {} -> {}",
                    static_cast<uint32_t>(target_address), afunc->address(),
                    static_cast<uint32_t>(guest_context->r[1]),
                    backend_context->current_stackpoint_depth, sync_depth);
              }
              backend_context->pending_stackpoint_sync_depth = sync_depth;
              if (cvars::a64_longjmp_sync_at_resolve &&
                  backend->synchronize_stack_and_resume_helper()) {
                // Through the helper first: the code after a call doesn't
                // check for this by itself.
                backend_context->pending_sync_target = host_address;
                return reinterpret_cast<uint64_t>(
                    backend->synchronize_stack_and_resume_helper());
              }
              return host_address;
            }
            break;
          }
        }
      }
    }
  }

  auto fn = thread_state->processor()->ResolveFunction(
      static_cast<uint32_t>(target_address));
  if (!fn) {
    // Unresolvable — return 0 which will fault.
    return 0;
  }

  auto guest_fn = static_cast<GuestFunction*>(fn);
  auto code = guest_fn->machine_code();
  if (!code) {
    return 0;
  }
  return reinterpret_cast<uint64_t>(code);
}

// ==========================================================================
// A64Backend
// ==========================================================================

// ARM64 guest trampoline template.
// Loads proc, userdata1, userdata2 into x0-x2, then jumps to guest_to_host
// thunk via x9.  Each 64-bit immediate uses movz + 3x movk (16 bytes).
// Total: 4 registers × 16 bytes + 4 bytes (br x9) = 68 bytes.
//
// Template layout (offsets where 64-bit immediates are patched):
//   +0x00: movz x0, #imm16; movk x0, ..., lsl 16/32/48  -> proc
//   +0x10: movz x1, #imm16; movk x1, ..., lsl 16/32/48  -> userdata1
//   +0x20: movz x2, #imm16; movk x2, ..., lsl 16/32/48  -> userdata2
//   +0x30: movz x9, #imm16; movk x9, ..., lsl 16/32/48  -> g2h thunk
//   +0x40: br x9
//
// ARM64 encoding helpers:
//   movz xN, #imm16          = 0xD2800000 | (imm16 << 5) | N
//   movk xN, #imm16, lsl #S  = 0xF2800000 | (hw << 21) | (imm16 << 5) | N
//     where hw = S/16 (0,1,2,3)
static void EncodeMovImm64(uint32_t* out, uint32_t reg, uint64_t imm) {
  out[0] = 0xD2800000 | (static_cast<uint32_t>(imm & 0xFFFF) << 5) | reg;
  out[1] =
      0xF2A00000 | (static_cast<uint32_t>((imm >> 16) & 0xFFFF) << 5) | reg;
  out[2] =
      0xF2C00000 | (static_cast<uint32_t>((imm >> 32) & 0xFFFF) << 5) | reg;
  out[3] =
      0xF2E00000 | (static_cast<uint32_t>((imm >> 48) & 0xFFFF) << 5) | reg;
}

static constexpr size_t kGuestTrampolineSize = 68;  // 17 instructions × 4
static constexpr uint32_t kTrampolineOffsetProc = 0x00;
static constexpr uint32_t kTrampolineOffsetArg1 = 0x10;
static constexpr uint32_t kTrampolineOffsetArg2 = 0x20;
static constexpr uint32_t kTrampolineOffsetThunk = 0x30;

static void BuildGuestTrampoline(uint8_t* buf, void* proc, void* userdata1,
                                 void* userdata2, void* g2h_thunk) {
  auto* code = reinterpret_cast<uint32_t*>(buf);
  // x0 = proc (target function for guest-to-host thunk)
  EncodeMovImm64(&code[0], 0, reinterpret_cast<uint64_t>(proc));
  // x1 = userdata1
  EncodeMovImm64(&code[4], 1, reinterpret_cast<uint64_t>(userdata1));
  // x2 = userdata2
  EncodeMovImm64(&code[8], 2, reinterpret_cast<uint64_t>(userdata2));
  // x9 = guest_to_host_thunk
  EncodeMovImm64(&code[12], 9, reinterpret_cast<uint64_t>(g2h_thunk));
  // br x9
  code[16] = 0xD61F0120;  // br x9
}

A64Backend::A64Backend() {
  code_cache_ = A64CodeCache::Create();

  // Prefer a sub-2GB slot so fast indirection (rel32) is usable; fall back
  // to an OS-chosen address if none is available. macOS rejects fixed
  // PROT_EXEC mappings in this range, so skip the scan entirely there.
  void* buf = nullptr;
#if !XE_PLATFORM_MAC
  for (uint32_t base_address = 0x10000; base_address < 0x80000000;
       base_address += 65536) {
    buf = memory::AllocFixed(
        reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)),
        kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
        xe::memory::AllocationType::kReserveCommit,
        xe::memory::PageAccess::kExecuteReadWrite);
    if (buf) {
      break;
    }
  }
#endif
  if (!buf) {
    buf = memory::AllocFixed(nullptr,
                             kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                             xe::memory::AllocationType::kReserveCommit,
                             xe::memory::PageAccess::kExecuteReadWrite);
  }
  xenia_assert(buf);
  guest_trampoline_memory_ = reinterpret_cast<uint8_t*>(buf);
  guest_trampolines_sub4gb_ = reinterpret_cast<uintptr_t>(buf) < 0x100000000ull;
  guest_trampoline_address_bitmap_.Resize(MAX_GUEST_TRAMPOLINES);
}

A64Backend::~A64Backend() {
  ExceptionHandler::Uninstall(&ExceptionCallbackThunk, this);
  if (guest_trampoline_memory_) {
    memory::DeallocFixed(guest_trampoline_memory_,
                         kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                         memory::DeallocationType::kRelease);
    guest_trampoline_memory_ = nullptr;
  }
}

bool A64Backend::Initialize(Processor* processor) {
  extended_gpr_allocator_ = cvars::a64_extended_gpr_allocator;
  if (!Backend::Initialize(processor)) {
    return false;
  }

  // Fast indirection is only viable if trampolines made it under 4GB.
  code_cache_->set_allow_fast_indirection(guest_trampolines_sub4gb_);
  if (!code_cache_->Initialize()) {
    XELOGE("A64Backend: Failed to initialize code cache");
    return false;
  }

  // Expose the code cache to the base Backend class.
  Backend::code_cache_ = code_cache_.get();

  // Set up machine info for the register allocator.
  machine_info_.supports_extended_load_store = true;
  machine_info_.supports_word_rotate_mask = true;
  // GPR set: x22-x28 (7 registers; x19=backend ctx, x20=context, x21=membase)
  auto& gpr_set = machine_info_.register_sets[0];
  gpr_set.id = 0;
  std::strcpy(gpr_set.name, "gpr");
  gpr_set.types = MachineInfo::RegisterSet::INT_TYPES;
  gpr_set.count = extended_gpr_allocator_
                      ? A64Emitter::GPR_COUNT : A64Emitter::BASE_GPR_COUNT;
  gpr_set.pinned_top = A64Emitter::PINNED_GPR_TOP;
  XELOGI("A64 SSA GPR allocator: {} registers (extended={})",
         gpr_set.count, extended_gpr_allocator_);
  // VEC set: v4-v15, v16-v31 (28 registers, v0-v3 scratch)
  auto& vec_set = machine_info_.register_sets[1];
  vec_set.id = 1;
  std::strcpy(vec_set.name, "vec");
  vec_set.types = MachineInfo::RegisterSet::FLOAT_TYPES |
                  MachineInfo::RegisterSet::VEC_TYPES;
  vec_set.count = A64Emitter::VEC_COUNT;
  vec_set.pinned_top = A64Emitter::PINNED_VEC_TOP;

  // Generate thunks using ARM64 assembler.
  XbyakA64Allocator allocator;
  A64HelperEmitter thunk_emitter(this, &allocator);

  host_to_guest_thunk_ = thunk_emitter.EmitHostToGuestThunk();
  guest_to_host_thunk_ = thunk_emitter.EmitGuestToHostThunk();
  guest_to_host_thunk_no_vec_ = cvars::a64_extern_thunk_no_vec
                                    ? thunk_emitter.EmitGuestToHostThunkNoVec()
                                    : guest_to_host_thunk_;
  resolve_function_thunk_ = thunk_emitter.EmitResolveFunctionThunk();

  if (!host_to_guest_thunk_ || !guest_to_host_thunk_ ||
      !guest_to_host_thunk_no_vec_ || !resolve_function_thunk_) {
    XELOGE("A64Backend: Failed to generate thunks");
    return false;
  }

  if (cvars::a64_enable_host_guest_stack_synchronization) {
    synchronize_guest_and_host_stack_helper_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackHelper(false);
    synchronize_stack_and_resume_helper_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackHelper(true);
  }

  cold_preempt_thunk_ =
      thunk_emitter.EmitColdPathThunk(A64HelperEmitter::ColdPath::kPreempt);
  cold_stackpoint_overflow_thunk_ = thunk_emitter.EmitColdPathThunk(
      A64HelperEmitter::ColdPath::kStackpointOverflow);
  if (code_cache_->has_indirection_table()) {
    cold_call_lookup_thunk_ = thunk_emitter.EmitColdPathThunk(
        A64HelperEmitter::ColdPath::kCallLookup);
    if (code_cache_->encoded_indirection()) {
      cold_external_lookup_thunk_ = thunk_emitter.EmitColdPathThunk(
          A64HelperEmitter::ColdPath::kExternalLookup);
    }
  }

  // Wire up reservation helpers used by RESERVED_LOAD/STORE codegen.
  try_acquire_reservation_helper_ =
      reinterpret_cast<void*>(&TryAcquireReservationHelper);
  reserved_store_32_helper = reinterpret_cast<void*>(&ReservedStore32Helper);
  reserved_store_64_helper = reinterpret_cast<void*>(&ReservedStore64Helper);

  // Set the indirection table default to point at the resolve thunk.
  // Use 64-bit encoding: the resolve thunk address is encoded as a rel32
  // offset if it lands inside the code cache, or as a tagged external-table
  // index otherwise.
  static_cast<A64CodeCache*>(code_cache_.get())
      ->set_indirection_default_64(
          reinterpret_cast<uint64_t>(resolve_function_thunk_));

  // Commit the indirection table range used by guest trampolines so that
  // CreateGuestTrampoline can call AddIndirection without faulting.
  code_cache_->CommitExecutableRange(GUEST_TRAMPOLINE_BASE,
                                     GUEST_TRAMPOLINE_END);

  // Commit special indirection ranges (force return address, etc.).
  code_cache_->CommitExecutableRange(0x9FFF0000, 0x9FFFFFFF);

  // Register exception handler for MMIO access from JIT code.
  ExceptionHandler::Install(ExceptionCallbackThunk, this);

  return true;
}

void A64Backend::CommitExecutableRange(uint32_t guest_low,
                                       uint32_t guest_high) {
  code_cache_->CommitExecutableRange(guest_low, guest_high);
}

std::unique_ptr<Assembler> A64Backend::CreateAssembler() {
  return std::make_unique<A64Assembler>(this);
}

std::unique_ptr<GuestFunction> A64Backend::CreateGuestFunction(
    Module* module, uint32_t address) {
  return std::make_unique<A64Function>(module, address);
}

uint64_t A64Backend::CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                                  uint64_t current_pc) {
  // ARM64 instructions are fixed 4 bytes.
  return current_pc + 4;
}

// ARM64 BRK #0 encoding (4 bytes, fixed-width instruction).
static constexpr uint32_t kArm64Brk0 = 0xD4200000;

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint) {
  breakpoint->ForEachHostAddress([breakpoint](uint64_t host_address) {
    auto ptr = reinterpret_cast<void*>(host_address);
    auto original_bytes = xe::load<uint32_t>(ptr);
    assert_true(original_bytes != kArm64Brk0);
    xe::store<uint32_t>(ptr, kArm64Brk0);
    breakpoint->backend_data().emplace_back(host_address, original_bytes);
  });
}

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint, Function* fn) {
  assert_true(breakpoint->address_type() == Breakpoint::AddressType::kGuest);
  assert_true(fn->is_guest());
  auto guest_function = reinterpret_cast<cpu::GuestFunction*>(fn);
  auto host_address =
      guest_function->MapGuestAddressToMachineCode(breakpoint->guest_address());
  if (!host_address) {
    assert_always();
    return;
  }

  auto ptr = reinterpret_cast<void*>(host_address);
  auto original_bytes = xe::load<uint32_t>(ptr);
  assert_true(original_bytes != kArm64Brk0);
  xe::store<uint32_t>(ptr, kArm64Brk0);
  breakpoint->backend_data().emplace_back(host_address, original_bytes);
}

void A64Backend::UninstallBreakpoint(Breakpoint* breakpoint) {
  for (auto& pair : breakpoint->backend_data()) {
    auto ptr = reinterpret_cast<uint8_t*>(pair.first);
    auto instruction_bytes = xe::load<uint32_t>(ptr);
    assert_true(instruction_bytes == kArm64Brk0);
    xe::store<uint32_t>(ptr, static_cast<uint32_t>(pair.second));
  }
  breakpoint->backend_data().clear();
}

void A64Backend::InitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  std::memset(a64_ctx, 0, sizeof(A64BackendContext));
  a64_ctx->reserve_helper_ = &reserve_helper_;
  a64_ctx->Ox1000 = 0x1000;
  a64_ctx->fpcr_fpu = DEFAULT_FPU_FPCR;
  a64_ctx->fpcr_vmx = DEFAULT_VMX_FPCR;
  a64_ctx->flags = (1U << kA64BackendNJMOn);  // NJM on by default
  a64_ctx->guest_tick_count = Clock::GetGuestTickCountPointer();
  a64_ctx->indirection_table_bias = code_cache()->indirection_table_base_bias();
  a64_ctx->execute_base = code_cache()->execute_base_address();
  a64_ctx->cold_preempt_thunk = uint64_t(cold_preempt_thunk_);
  a64_ctx->cold_stackpoint_overflow_thunk =
      uint64_t(cold_stackpoint_overflow_thunk_);
  a64_ctx->cold_call_lookup_thunk = uint64_t(cold_call_lookup_thunk_);
  a64_ctx->cold_external_lookup_thunk = uint64_t(cold_external_lookup_thunk_);

  // Allocate stackpoints for longjmp detection.
  if (cvars::a64_enable_host_guest_stack_synchronization) {
    uint64_t max_stackpoints = cvars::a64_max_stackpoints;
    if (max_stackpoints > 0) {
      a64_ctx->stackpoints = new A64BackendStackpoint[max_stackpoints]();
    }
  }

  // Reset the live host FPCR for a fresh PPC context so one test's rounding
  // state does not leak into the next on the shared PPC test runner thread.
  SetGuestRoundingMode(ctx, 0);
}

void A64Backend::DeinitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  if (a64_ctx->stackpoints) {
    delete[] a64_ctx->stackpoints;
    a64_ctx->stackpoints = nullptr;
  }
}

void A64Backend::PrepareForReentry(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  a64_ctx->current_stackpoint_depth = 0;
  a64_ctx->pending_stackpoint_sync_depth = 0;
}

uint32_t A64Backend::CreateGuestTrampoline(GuestTrampolineProc proc,
                                           void* userdata1, void* userdata2,
                                           bool long_term) {
  size_t new_index;
  if (long_term) {
    new_index = guest_trampoline_address_bitmap_.AcquireFromBack();
  } else {
    new_index = guest_trampoline_address_bitmap_.Acquire();
  }
  xenia_assert(new_index != static_cast<size_t>(-1));

  uint8_t* write_pos =
      &guest_trampoline_memory_[kGuestTrampolineSize * new_index];

#if XE_PLATFORM_MAC
  pthread_jit_write_protect_np(0);
#endif
  BuildGuestTrampoline(write_pos, reinterpret_cast<void*>(proc), userdata1,
                       userdata2,
                       reinterpret_cast<void*>(guest_to_host_thunk_));
#if XE_PLATFORM_MAC
  pthread_jit_write_protect_np(1);
#endif

  // Flush instruction cache for the new trampoline code.
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), write_pos, kGuestTrampolineSize);
#else
  __builtin___clear_cache(
      reinterpret_cast<char*>(write_pos),
      reinterpret_cast<char*>(write_pos + kGuestTrampolineSize));
#endif

  uint32_t indirection_guest_addr =
      GUEST_TRAMPOLINE_BASE +
      (static_cast<uint32_t>(new_index) * GUEST_TRAMPOLINE_MIN_LEN);

  code_cache()->AddIndirection64(indirection_guest_addr,
                                 reinterpret_cast<uint64_t>(write_pos));

  return indirection_guest_addr;
}

void A64Backend::FreeGuestTrampoline(uint32_t trampoline_addr) {
  xenia_assert(trampoline_addr >= GUEST_TRAMPOLINE_BASE &&
               trampoline_addr < GUEST_TRAMPOLINE_END);
  size_t index =
      (trampoline_addr - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;
  guest_trampoline_address_bitmap_.Release(index);
}

bool A64Backend::trace_instr_available() const { return IsTracingInstr(); }
bool A64Backend::trace_data_available() const { return IsTracingData(); }
bool A64Backend::trace_func_available() const { return IsTracingFunc(); }
bool A64Backend::trace_instr_enabled() const { return GetTraceInstrEnabled(); }
void A64Backend::set_trace_instr_enabled(bool value) {
  SetTraceInstrEnabled(value);
}
bool A64Backend::trace_data_enabled() const { return GetTraceDataEnabled(); }
void A64Backend::set_trace_data_enabled(bool value) {
  SetTraceDataEnabled(value);
}
bool A64Backend::trace_func_enabled() const { return GetTraceFuncEnabled(); }
void A64Backend::set_trace_func_enabled(bool value) {
  SetTraceFuncEnabled(value);
}

// PPC rounding mode (3-bit) to ARM64 FPCR value.
// Same table as in a64_sequences.cc SET_ROUNDING_MODE.
static constexpr uint32_t fpcr_table[8] = {
    (0b00 << 22),              // PPC 0: nearest, IEEE
    (0b11 << 22),              // PPC 1: toward zero, IEEE
    (0b01 << 22),              // PPC 2: toward +inf, IEEE
    (0b10 << 22),              // PPC 3: toward -inf, IEEE
    (0b00 << 22) | (1 << 24),  // PPC 4: nearest, flush-to-zero
    (0b11 << 22) | (1 << 24),  // PPC 5: toward zero, flush-to-zero
    (0b01 << 22) | (1 << 24),  // PPC 6: toward +inf, flush-to-zero
    (0b10 << 22) | (1 << 24),  // PPC 7: toward -inf, flush-to-zero
};

void A64Backend::SetGuestRoundingMode(void* ctx, unsigned int mode) {
  A64BackendContext* bctx = BackendContextForGuestContext(ctx);
  uint32_t control = mode & 7;
  uint32_t fpcr_val = fpcr_table[control];
#if XE_COMPILER_MSVC
  // MSVC ARM64 intrinsic: ARM64_FPCR = register ID 0x5A20.
  _WriteStatusReg(0x5A20, static_cast<uint64_t>(fpcr_val));
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(static_cast<uint64_t>(fpcr_val)));
#endif
  bctx->fpcr_fpu = fpcr_val;
  bctx->fpcr_live_mode = 1;
  if (control & 0b100) {
    bctx->flags |= (1u << kA64BackendNonIEEEMode);
  } else {
    bctx->flags &= ~(1u << kA64BackendNonIEEEMode);
  }
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(ctx);
  ppc_context->fpscr.bits.rn = control;
  ppc_context->fpscr.bits.ni = control >> 2;
}

bool A64Backend::PopulatePseudoStacktrace(GuestPseudoStackTrace* st) {
  if (!cvars::a64_enable_host_guest_stack_synchronization) {
    return false;
  }

  ThreadState* thrd_state = ThreadState::Get();
  if (!thrd_state) {
    return false;
  }
  ppc::PPCContext* ctx = thrd_state->context();
  A64BackendContext* backend_ctx = BackendContextForGuestContext(ctx);

  if (!backend_ctx->stackpoints || backend_ctx->current_stackpoint_depth < 2) {
    return false;
  }
  uint32_t depth = backend_ctx->current_stackpoint_depth - 1;
  uint32_t num_entries_to_populate =
      std::min(MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES, depth);

  st->count = num_entries_to_populate;
  st->truncated_flag = num_entries_to_populate < depth ? 1 : 0;

  A64BackendStackpoint* current_stackpoint =
      &backend_ctx->stackpoints[backend_ctx->current_stackpoint_depth - 1];

  for (uint32_t stp_index = 0; stp_index < num_entries_to_populate;
       ++stp_index) {
    st->return_addrs[stp_index] = current_stackpoint->guest_return_address_;
    current_stackpoint--;
  }
  return true;
}

void A64Backend::RecordMMIOExceptionForGuestInstruction(void* host_address) {
  uint64_t host_addr_u64 = reinterpret_cast<uint64_t>(host_address);
  auto fnfor = code_cache()->LookupFunction(host_addr_u64);
  if (fnfor) {
    uint32_t guestaddr = fnfor->MapMachineCodeToGuestAddress(host_addr_u64);
    Module* guest_module = fnfor->module();
    if (guest_module) {
      XexModule* xex_guest_module = dynamic_cast<XexModule*>(guest_module);
      if (xex_guest_module) {
        cpu::InfoCacheFlags* icf =
            xex_guest_module->GetInstructionAddressFlags(guestaddr);
        if (icf) {
          cpu::InfoCacheFlags bits{};
          bits.accessed_mmio = true;
          cpu::AtomicSetInfoCacheFlags(icf, bits);
        }
      }
    }
  }
}

bool A64Backend::ExceptionCallbackThunk(Exception* ex, void* data) {
  auto* backend = reinterpret_cast<A64Backend*>(data);
  return backend->ExceptionCallback(ex);
}

bool A64Backend::ExceptionCallback(Exception* ex) {
  if (ex->code() != Exception::Code::kIllegalInstruction) {
    return false;
  }

  // Verify it's our BRK #0 instruction.
  auto instruction_bytes =
      xe::load<uint32_t>(reinterpret_cast<void*>(ex->pc()));
  if (instruction_bytes != kArm64Brk0) {
    return false;
  }

  return processor()->OnThreadBreakpointHit(ex);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
