/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <type_traits>

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_gpr_run.h"
#include "xenia/cpu/backend/a64/a64_load_fusion.h"
#include "xenia/cpu/backend/a64/a64_seq_util.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"

DECLARE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses);
DECLARE_bool(emit_inline_mmio_checks);

DECLARE_bool(guest_spin_wfe);
DECLARE_bool(a64_gpr_save_restore_runs);
DEFINE_bool(a64_store_truncate_fusion, true,
            "Let integer stores consume the low bits of a sole adjacent "
            "truncate source directly, without a temporary move/mask.", "CPU");

DEFINE_bool(a64_load_zero_extend_fusion, true,
            "Emit an integer load directly into its sole adjacent zero-extend "
            "destination; preserve access width/byte order and skip the move.",
            "CPU");

DEFINE_bool(a64_fold_guest_address, false,
            "Use [membase, wGuest, UXTW] for plain guest loads/stores on "
            "4 KiB pages; preserves 32-bit truncation. Disabled with data tracing.",
            "CPU");
DEFINE_bool(a64_guest_address_uxtw, true,
            "Default-on successor of a64_fold_guest_address (either enables "
            "it): [membase, wGuest, UXTW] instead of a separate 32-bit move.",
            "CPU");
DEFINE_bool(a64_compact_mmio_check, false,
            "Use an equivalent upper-bit equality test for the inclusive "
            "0x7FC00000..0x7FFFFFFF inline MMIO window.",
            "CPU");
DEFINE_bool(a64_mmio_check_compact, true,
            "Default-on successor of a64_compact_mmio_check (either enables "
            "it).",
            "CPU");

DEFINE_bool(a64_native_reserved_ops, true,
            "Compile guest lwarx/stwcx. to inline native atomics (LSE CASAL) "
            "instead of the software-reservation thunk: lwarx captures the word "
            "and arms a per-thread flag; stwcx. validates with one CAS. The "
            "single atomic has no LDXR->STXR window, avoiding the monitor-loss "
            "livelock of a spanning ldaxr/stlxr (which hung Forza Horizon). "
            "Requires FEAT_LSE.",
            "CPU");

DEFINE_bool(a64_pair_cas_bump_generation, true,
            "A paired lwarx/stwcx. compare-and-swap that stored also moves the "
            "reservation generation of its granule on, like every other "
            "store-conditional.",
            "a64");
DECLARE_bool(guest_scheduler);

DEFINE_bool(a64_park_spin_backoff, true,
            "For collapsed guest spin-backoff loops, spin cheaply for the first "
            "few iterations then park the thread with a short real sleep "
            "(adaptive) instead of the fixed isb sled - reclaims CPU on long "
            "guest spin-waits while short waits (resolved during the cheap "
            "spin) stay latency-unaffected. Falls back to a cheap spin under "
            "the cooperative guest scheduler, where host-blocking a fiber could "
            "stall its producer.",
            "CPU");

DEFINE_bool(
    log_spin_wait_histogram, false,
    "Bucket the wall-clock length of every collapsed guest spin-wait episode "
    "and dump the distribution once per second. Requires a64_park_spin_backoff.",
    "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_memory = 0;

static bool IsPossibleMMIOInstruction(A64Emitter& e, const hir::Instr* i) {
  if (!cvars::emit_mmio_aware_stores_for_recorded_exception_addresses) {
    return false;
  }
  uint32_t guest_address = i->GuestAddressFor();
  if (!guest_address) {
    return false;
  }

  auto* guest_module = e.GuestModule();
  if (!guest_module) {
    return false;
  }
  auto* flags = guest_module->GetInstructionAddressFlags(guest_address);
  return flags && flags->accessed_mmio;
}

// UXTW performs the same truncation as mov w0, wGuest without a separate
// instruction. Keep constants, tracing, and large-page address fixups unchanged.
struct GuestMemoryOperand {
  A64Emitter& emitter;
  XReg address;
  bool extend;

  template <typename Fn>
  void Emit(Fn&& access) const {
    if (extend) {
      access(ptr(emitter.GetMembaseReg(), WReg(address.getIdx()), UXTW));
    } else {
      // Retain the original AdrReg encoding as well as its address fixup.
      access(ptr(emitter.GetMembaseReg(), address));
    }
  }
};

static GuestMemoryOperand ComputeGuestMemoryOperand(A64Emitter& e,
                                                    const I64Op& guest) {
  if ((cvars::a64_fold_guest_address || cvars::a64_guest_address_uxtw) &&
      !IsTracingData() && !guest.is_constant &&
      xe::memory::allocation_granularity() <= 0x1000) {
    return {e, guest.reg(), true};
  }
  return {e, ComputeMemoryAddress(e, guest), false};
}

// ARM64 LDRB/LDRH/LDR W define the full 64-bit integer result, with zero
// upper bits. The allocator already assigned the adjacent extend's register;
// writing it directly consumes address operands first and preserves any alias.
// Keep tracing unfused so every trace hook sees the legacy value/register.
static WReg IntegerLoadDestination(A64Emitter& e, const hir::Instr* instr,
                                   const WReg& original) {
  const auto* extend = FindLoadZeroExtendFusion(
      instr, cvars::a64_load_zero_extend_fusion && !IsTracingData());
  if (!extend) return original;
  hir::Instr::Op destination{};
  destination.value = extend->dest;
  I32Op op;
  op.Load(destination);
  e.fused_skip_instr_ = extend;
  return op.reg();
}

static WReg IntegerStoreSource(A64Emitter& e, const hir::Instr* store,
                                const WReg& original) {
  if (e.fused_truncate_store_instr_ != store) return original;
  const int source = e.fused_truncate_store_src_reg_;
  e.fused_truncate_store_instr_ = nullptr;
  e.fused_truncate_store_src_reg_ = -1;
  return WReg(source);
}

// w17 holds the raw, wrapped guest address. Preserve it for the helper.
static void BranchIfNotMMIO(A64Emitter& e, Xbyak_aarch64::Label& normal_access) {
  if (cvars::a64_compact_mmio_check || cvars::a64_mmio_check_compact) {
    // [0x7FC00000, 0x7FFFFFFF] is exactly the aligned 4 MiB window
    // with bits 31:22 == 0x1FF. No bounds check is removed.
    e.lsr(e.w0, e.w17, 22);
    e.cmp(e.w0, 0x1FF);
    e.b(NE, normal_access);
  } else {
    e.mov(e.w0, 0x7FC00000u);
    e.cmp(e.w17, e.w0);
    e.b(LO, normal_access);
    e.mov(e.w0, 0x7FFFFFFFu);
    e.cmp(e.w17, e.w0);
    e.b(HI, normal_access);
  }
}

// ============================================================================
// OPCODE_DELAY_EXECUTION
// ============================================================================
struct DELAY_EXECUTION
    : Sequence<DELAY_EXECUTION, I<OPCODE_DELAY_EXECUTION, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // db16cyc throttles a guest spin loop. yield is the literal translation
    // but NOPs on Cortex-X/A7xx, so the sled cost nothing; isb is the usual
    // stand-in - a pipeline flush with no guest-observable effect. Coalesce
    // consecutive barriers so a long sled stays one instruction.
    constexpr uint32_t kIsbSy = 0xD5033FDFu;
    if (e.getSize() >= sizeof(uint32_t) &&
        *reinterpret_cast<const uint32_t*>(e.getCurr() - sizeof(uint32_t)) ==
            kIsbSy) {
      return;
    }
    e.isb(Xbyak_aarch64::SY);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DELAY_EXECUTION, DELAY_EXECUTION);

// Spin-wait-length histogram (log_spin_wait_histogram): eight wall-clock
// duration buckets, dumping both the ~1s interval delta and cumulative totals.
namespace {
constexpr size_t kSpinHistBuckets = 8;
// Upper edges (ns) for buckets 0..6; bucket 7 (>10ms) is everything larger.
constexpr int64_t kSpinHistEdgesNs[kSpinHistBuckets - 1] = {
    1000, 3000, 10000, 30000, 100000, 1000000, 10000000};
const char* const kSpinHistLabels[kSpinHistBuckets] = {
    "<1us",     "1-3us", "3-10us", "10-30us",
    "30-100us", "100us-1ms",  "1-10ms",  ">10ms"};
std::atomic<uint64_t> g_spin_hist_count[kSpinHistBuckets];
std::atomic<uint64_t> g_spin_hist_time_ns[kSpinHistBuckets];
// Snapshot at the previous dump; written only by the dump-winning thread.
uint64_t g_spin_hist_prev_count[kSpinHistBuckets];
uint64_t g_spin_hist_prev_time_ns[kSpinHistBuckets];
std::atomic<int64_t> g_spin_hist_last_dump_ns{0};

void FormatSpinHistLine(const char* tag, const uint64_t* cc, const uint64_t* tt,
                        uint64_t ctot, uint64_t ttot) {
  const double td = ttot ? static_cast<double>(ttot) : 1.0;
  char line[512];
  int off = 0;
  for (size_t i = 0; i < kSpinHistBuckets; ++i) {
    off += std::snprintf(line + off, sizeof(line) - off, "%s:%llu(%.0f%%t) ",
                         kSpinHistLabels[i],
                         static_cast<unsigned long long>(cc[i]),
                         100.0 * static_cast<double>(tt[i]) / td);
    if (off < 0 || off >= static_cast<int>(sizeof(line))) {
      break;
    }
  }
  XELOGI("SpinWaitHist[{}]: episodes={} spin={:.1f}ms | {}", tag, ctot,
         static_cast<double>(ttot) / 1.0e6, line);
}

void RecordSpinEpisode(int64_t dur_ns, int64_t now_ns) {
  if (dur_ns < 0) {
    dur_ns = 0;
  }
  size_t b = kSpinHistBuckets - 1;
  for (size_t i = 0; i < kSpinHistBuckets - 1; ++i) {
    if (dur_ns < kSpinHistEdgesNs[i]) {
      b = i;
      break;
    }
  }
  g_spin_hist_count[b].fetch_add(1, std::memory_order_relaxed);
  g_spin_hist_time_ns[b].fetch_add(static_cast<uint64_t>(dur_ns),
                                   std::memory_order_relaxed);

  int64_t last = g_spin_hist_last_dump_ns.load(std::memory_order_relaxed);
  if (now_ns - last <= 1000000000LL) {
    return;  // <1s since last dump
  }
  if (!g_spin_hist_last_dump_ns.compare_exchange_strong(
          last, now_ns, std::memory_order_relaxed)) {
    return;  // another thread is dumping this second
  }
  uint64_t c[kSpinHistBuckets], t[kSpinHistBuckets], ctot = 0, ttot = 0;
  uint64_t ic[kSpinHistBuckets], it[kSpinHistBuckets], ictot = 0, ittot = 0;
  for (size_t i = 0; i < kSpinHistBuckets; ++i) {
    c[i] = g_spin_hist_count[i].load(std::memory_order_relaxed);
    t[i] = g_spin_hist_time_ns[i].load(std::memory_order_relaxed);
    ic[i] = c[i] - g_spin_hist_prev_count[i];
    it[i] = t[i] - g_spin_hist_prev_time_ns[i];
    g_spin_hist_prev_count[i] = c[i];
    g_spin_hist_prev_time_ns[i] = t[i];
    ctot += c[i];
    ttot += t[i];
    ictot += ic[i];
    ittot += it[i];
  }
  FormatSpinHistLine("interval~1s", ic, it, ictot, ittot);
  FormatSpinHistLine("cumulative", c, t, ctot, ttot);
}
}  // namespace

// Adaptive park helper for OPCODE_SPIN_BACKOFF (a64_park_spin_backoff), called
// once per outer poll iteration of a collapsed guest spin-wait. A young wait
// spins cheap (a few isb); once it proves long it sleeps briefly so the core
// stops burning cycles. A gap since the previous call starts a fresh episode so
// an unrelated later wait spins cheap again. Never host-blocks under the
// cooperative scheduler - blocking a dispatch thread there can stall the
// sibling fiber that releases the polled word (raw-jit-spin-path.md Blocker 2).
// The sleep timeout guarantees forward progress, so no wake plumbing is needed.
static void SpinBackoffParkThunk(void* /*ppc_context*/) {
  static constexpr uint32_t kSpinIters = 24;
  static constexpr int64_t kParkNs = 30000;  // 30us bounded park
  static constexpr int64_t kGapNs = 200000;  // >200us idle -> new episode
  thread_local uint32_t consec = 0;
  thread_local int64_t last_ns = 0;
  thread_local int64_t ep_start_ns = 0;
  if (cvars::guest_scheduler) {
    // Cooperative path: host-parking would stall co-resident fibers (the NFS
    // Most Wanted deadlock class) and host-spinning never runs the producer,
    // which may be a fiber queued behind this one. Yield instead - ready-tail
    // requeue guarantees co-resident progress.
    if (auto* yield_handler = xe::cpu::backend::spin_backoff_yield_handler) {
      yield_handler(nullptr);
      return;
    }
    // Scheduler enabled but not started yet (early init), or a non-fiber
    // caller: fall back to the cheap spin.
    for (uint32_t n = 0; n < 8; ++n) {
      __asm__ __volatile__("isb sy" ::: "memory");
    }
    return;
  }
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  if (now_ns - last_ns > kGapNs) {
    // A gap since the last poll ends the previous episode; record its duration.
    if (cvars::log_spin_wait_histogram && last_ns != 0) {
      RecordSpinEpisode(last_ns - ep_start_ns, now_ns);
    }
    consec = 0;
    ep_start_ns = now_ns;
  }
  last_ns = now_ns;
  if (++consec < kSpinIters) {
    for (uint32_t n = 0; n < 8; ++n) {
      __asm__ __volatile__("isb sy" ::: "memory");
    }
    return;
  }
  xe::threading::NanoSleep(kParkNs);
}

// ============================================================================
// OPCODE_SPIN_BACKOFF
// ============================================================================
// Bounded host-side wait: a counted loop of `isb sy` (~tens of cycles each on
// modern cores), emitted in place of proven constant-trip-count guest
// spin-backoff loops. src1.offset is the iteration count, already clamped by
// the pass that emits this op. The loop is held entirely in w16, an
// emitter-scratch register (the register allocator only hands out x22-x28;
// w16/w17 are already used as per-sequence scratch elsewhere). It uses
// sub+cbnz rather than subs+b.ne so NZCV is never written - no host state
// that surrounding sequences could observe is disturbed, and there is no
// guest context or memory traffic at all.
struct SPIN_BACKOFF
    : Sequence<SPIN_BACKOFF, I<OPCODE_SPIN_BACKOFF, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t count = static_cast<uint32_t>(i.src1.value);
    if (!count) {
      return;
    }
    if (cvars::a64_park_spin_backoff) {
      // Adaptive spin-then-park (see SpinBackoffParkThunk): cheap for short
      // waits, a real short sleep for long ones. CallNativeSafe preserves guest
      // context across the (possibly sleeping) helper.
      //
      // The helper only yields the fiber, a no-op unless something else is
      // runnable, but the call is a full guest->host thunk (28 Q-regs, 464 B
      // frame) plus a thread_local lookup. Gate it on the scheduler's own
      // give-way flag so the common case is two instructions.
      if (cvars::guest_scheduler) {
        static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
        auto& skip = e.NewCachedLabel();
        e.ldrb(e.w16, Xbyak_aarch64::ptr(
                          e.GetContextReg(),
                          static_cast<uint32_t>(offsetof(
                              ppc::PPCContext, preempt_requested))));
        e.cbz(e.w16, skip);
        e.EmitPinnedContext(false, uint32_t(i.instr->src2.offset));
        e.CallNativeSafe(reinterpret_cast<void*>(&SpinBackoffParkThunk));
        e.EmitPinnedContext(true, uint32_t(i.instr->src3.offset));
        e.L(skip);
        return;
      }
      e.EmitPinnedContext(false, uint32_t(i.instr->src2.offset));
      e.CallNativeSafe(reinterpret_cast<void*>(&SpinBackoffParkThunk));
      e.EmitPinnedContext(true, uint32_t(i.instr->src3.offset));
      return;
    }
    auto& loop = e.NewCachedLabel();
    e.mov(e.w16, count);
    e.L(loop);
    e.isb(Xbyak_aarch64::SY);
    e.sub(e.w16, e.w16, 1);
    e.cbnz(e.w16, loop);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SPIN_BACKOFF, SPIN_BACKOFF);

// ============================================================================
// OPCODE_MEMORY_BARRIER
// ============================================================================
struct MEMORY_BARRIER
    : Sequence<MEMORY_BARRIER, I<OPCODE_MEMORY_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.dmb(Xbyak_aarch64::ISH);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMORY_BARRIER, MEMORY_BARRIER);

// ============================================================================
// OPCODE_LOAD_BARRIER
// ============================================================================
struct LOAD_BARRIER : Sequence<LOAD_BARRIER, I<OPCODE_LOAD_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.dmb(Xbyak_aarch64::ISHLD);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_BARRIER, LOAD_BARRIER);

// ============================================================================
// OPCODE_CACHE_CONTROL
// ============================================================================
struct CACHE_CONTROL
    : Sequence<CACHE_CONTROL,
               I<OPCODE_CACHE_CONTROL, VoidOp, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    bool is_clflush = false, is_prefetch = false, is_prefetchw = false;
    switch (CacheControlType(i.instr->flags)) {
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH:
        is_prefetch = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
        is_prefetchw = true;
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE:
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
        is_clflush = true;
        break;
      default:
        return;
    }
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    size_t cache_line_size = i.src2.value;
    if (is_clflush) {
      // dc civac, x0
      e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
    }
    if (is_prefetch) {
      e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
    } else if (is_prefetchw) {
      e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
    }
    if (cache_line_size >= 128) {
      e.eor(e.x0, e.x0, 64);
      if (is_clflush) {
        // dc civac, x0
        e.sys(0b011, 0b0111, 0b1110, 0b001, e.x0);
      }
      if (is_prefetch) {
        e.prfm(Xbyak_aarch64::PLDL1KEEP, ptr(e.x0));
      } else if (is_prefetchw) {
        e.prfm(Xbyak_aarch64::PSTL1KEEP, ptr(e.x0));
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CACHE_CONTROL, CACHE_CONTROL);

template <typename T, bool swap>
static void MMIOAwareStore(void* _ctx, unsigned int guestaddr, T value) {
  if (swap) {
    value = xe::byte_swap(value);
  }
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr) = value;
  } else {
    value = xe::byte_swap(value);
    gaddr->write(nullptr, gaddr->callback_context, guestaddr, value);
  }
}

template <typename T, bool swap>
static T MMIOAwareLoad(void* _ctx, unsigned int guestaddr) {
  T value;
  if (guestaddr >= 0xE0000000) {
    guestaddr += 0x1000;
  }
  auto ctx = reinterpret_cast<ppc::PPCContext*>(_ctx);
  auto gaddr = ctx->processor->memory()->LookupVirtualMappedRange(guestaddr);
  if (!gaddr) {
    value = *reinterpret_cast<T*>(ctx->virtual_membase + guestaddr);
    if (swap) {
      value = xe::byte_swap(value);
    }
  } else {
    value = gaddr->read(nullptr, gaddr->callback_context, guestaddr);
  }
  return value;
}

// ============================================================================
// OPCODE_LOAD
// ============================================================================
struct LOAD_I8 : Sequence<LOAD_I8, I<OPCODE_LOAD, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    mem.Emit([&](const auto& operand) { e.ldrb(load_dest, operand); });
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w2, load_dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI8));
    }
  }
};
struct LOAD_I16 : Sequence<LOAD_I16, I<OPCODE_LOAD, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    mem.Emit([&](const auto& operand) { e.ldrh(load_dest, operand); });
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(load_dest, load_dest);
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w2, load_dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI16));
    }
  }
};
struct LOAD_I32 : Sequence<LOAD_I32, I<OPCODE_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      e.CallNativeSafe(mmio_fn);
      e.mov(load_dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      BranchIfNotMMIO(e, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(load_dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        auto mem = ComputeGuestMemoryOperand(e, i.src1);
        mem.Emit([&](const auto& operand) { e.ldr(load_dest, operand); });
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(load_dest, load_dest);
        }
      }
      e.L(done);
    } else {
      auto mem = ComputeGuestMemoryOperand(e, i.src1);
      mem.Emit([&](const auto& operand) { e.ldr(load_dest, operand); });
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(load_dest, load_dest);
      }
      if (IsTracingData()) {
        auto addr = ComputeMemoryAddress(e, i.src1);
        e.mov(e.w2, load_dest);
        e.mov(e.w1, WReg(addr.getIdx()));
        e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI32));
      }
    }
  }
};
struct LOAD_I64 : Sequence<LOAD_I64, I<OPCODE_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.x2, i.dest);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI64));
    }
  }
};
struct LOAD_F32 : Sequence<LOAD_F32, I<OPCODE_LOAD, F32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reversed where it is going to be used: no general register, no move
      // across.
      mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
      const auto dest_index = i.dest.reg().getIdx();
      e.rev32(VReg8B(dest_index), VReg8B(dest_index));
    } else {
      mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.mov(VReg(0).b16, VReg(i.dest.reg().getIdx()).b16);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF32));
    }
  }
};
struct LOAD_F64 : Sequence<LOAD_F64, I<OPCODE_LOAD, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
      const auto dest_index = i.dest.reg().getIdx();
      e.rev64(VReg8B(dest_index), VReg8B(dest_index));
    } else {
      mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.mov(VReg(0).b16, VReg(i.dest.reg().getIdx()).b16);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF64));
    }
  }
};
struct LOAD_V128 : Sequence<LOAD_V128, I<OPCODE_LOAD, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    mem.Emit([&](const auto& operand) { e.ldr(i.dest, operand); });
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word (PPC BE -> ARM64 LE).
      auto idx = i.dest.reg().getIdx();
      e.rev32(VReg16B(idx), VReg16B(idx));
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x2, e.GetMembaseReg(), addr);
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD, LOAD_I8, LOAD_I16, LOAD_I32, LOAD_I64,
                     LOAD_F32, LOAD_F64, LOAD_V128);

// ============================================================================
// OPCODE_STORE
// ============================================================================
struct STORE_I8 : Sequence<STORE_I8, I<OPCODE_STORE, VoidOp, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src2.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src2.reg());
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.src2.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFF));
      mem.Emit([&](const auto& operand) { e.strb(e.w17, operand); });
    } else {
      mem.Emit([&](const auto& operand) { e.strb(store_source, operand); });
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldrb(e.w2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI8));
    }
  }
};
struct STORE_I16 : Sequence<STORE_I16, I<OPCODE_STORE, VoidOp, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src2.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src2.reg());
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src2.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, store_source);
      }
      mem.Emit([&](const auto& operand) { e.strh(e.w17, operand); });
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.constant() & 0xFFFF));
        mem.Emit([&](const auto& operand) { e.strh(e.w17, operand); });
      } else {
        mem.Emit([&](const auto& operand) { e.strh(store_source, operand); });
      }
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldrh(e.w2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI16));
    }
  }
};
struct STORE_I32 : Sequence<STORE_I32, I<OPCODE_STORE, VoidOp, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src2.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src2.reg());
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, store_source);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      BranchIfNotMMIO(e, normal_access);
      // MMIO path — copy value to w2 before w1 in case src2 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src2.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w2, store_source);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        auto mem = ComputeGuestMemoryOperand(e, i.src1);
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src2.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, store_source);
          }
          mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
        } else {
          if (i.src2.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src2.constant())));
            mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
          } else {
            mem.Emit([&](const auto& operand) { e.str(store_source, operand); });
          }
        }
      }
      e.L(done);
    } else {
      auto mem = ComputeGuestMemoryOperand(e, i.src1);
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src2.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src2.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, store_source);
        }
        mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
      } else {
        if (i.src2.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src2.constant())));
          mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
        } else {
          mem.Emit([&](const auto& operand) { e.str(store_source, operand); });
        }
      }
      if (IsTracingData()) {
        auto addr = ComputeMemoryAddress(e, i.src1);
        e.ldr(e.w2, ptr(e.GetMembaseReg(), addr));
        e.mov(e.w1, WReg(addr.getIdx()));
        e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI32));
      }
    }
  }
};
struct STORE_I64 : Sequence<STORE_I64, I<OPCODE_STORE, VoidOp, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src2.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src2);
      }
      mem.Emit([&](const auto& operand) { e.str(e.x17, operand); });
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.constant()));
        mem.Emit([&](const auto& operand) { e.str(e.x17, operand); });
      } else {
        mem.Emit([&](const auto& operand) { e.str(i.src2, operand); });
      }
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.x2, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI64));
    }
  }
};
struct STORE_F32 : Sequence<STORE_F32, I<OPCODE_STORE, VoidOp, I64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint32_t val =
            xe::byte_swap(static_cast<uint32_t>(i.src2.value->constant.i32));
        e.mov(e.w17, static_cast<uint64_t>(val));
        mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
      } else {
        // Reversed in the scratch vector register and stored from there.
        e.rev32(VReg8B(0), VReg8B(i.src2.reg().getIdx()));
        mem.Emit([&](const auto& operand) { e.str(e.s0, operand); });
      }
    } else {
      if (i.src2.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src2.value->constant.i32));
        mem.Emit([&](const auto& operand) { e.str(e.w17, operand); });
      } else {
        mem.Emit([&](const auto& operand) { e.str(i.src2, operand); });
      }
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.s0, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF32));
    }
  }
};
struct STORE_F64 : Sequence<STORE_F64, I<OPCODE_STORE, VoidOp, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mem = ComputeGuestMemoryOperand(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src2.is_constant) {
        uint64_t val =
            xe::byte_swap(static_cast<uint64_t>(i.src2.value->constant.i64));
        e.mov(e.x17, val);
        mem.Emit([&](const auto& operand) { e.str(e.x17, operand); });
      } else {
        e.rev64(VReg8B(0), VReg8B(i.src2.reg().getIdx()));
        mem.Emit([&](const auto& operand) { e.str(e.d0, operand); });
      }
    } else {
      if (i.src2.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src2.value->constant.i64));
        mem.Emit([&](const auto& operand) { e.str(e.x17, operand); });
      } else {
        mem.Emit([&](const auto& operand) { e.str(i.src2, operand); });
      }
    }
    if (IsTracingData()) {
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.ldr(e.d0, ptr(e.GetMembaseReg(), addr));
      e.mov(e.w1, WReg(addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF64));
    }
  }
};
struct STORE_V128
    : Sequence<STORE_V128, I<OPCODE_STORE, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // ComputeMemoryAddress may return x0, and LoadV128Const/SrcVReg clobber
    // x0, so save the address to x17 when we need to load a constant source.
    bool need_src_load =
        i.src2.is_constant ||
        (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP);
    auto addr = ComputeMemoryAddress(e, i.src1);
    if (need_src_load) {
      e.mov(e.x17, addr);
      addr = e.x17;
    }
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse bytes within each 32-bit word, store via scratch v0.
      int idx = SrcVReg(e, i.src2, 0);
      e.rev32(VReg16B(0), VReg16B(idx));
      e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
    } else {
      if (i.src2.is_constant) {
        LoadV128Const(e, 0, i.src2.constant());
        e.str(QReg(0), ptr(e.GetMembaseReg(), addr));
      } else {
        e.str(i.src2, ptr(e.GetMembaseReg(), addr));
      }
    }
    if (IsTracingData()) {
      auto trace_addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x2, e.GetMembaseReg(), trace_addr);
      e.mov(e.w1, WReg(trace_addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE, STORE_I8, STORE_I16, STORE_I32, STORE_I64,
                     STORE_F32, STORE_F64, STORE_V128);

// ============================================================================
// OPCODE_LOAD_CLOCK
// ============================================================================
struct LOAD_CLOCK : Sequence<LOAD_CLOCK, I<OPCODE_LOAD_CLOCK, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Call QueryGuestTickCount which updates the clock from host ticks.
    // Reading the cached pointer directly would return stale values for
    // consecutive mftb instructions.
    e.CallNative(reinterpret_cast<void*>(LoadClock));
    e.mov(i.dest, e.x0);
  }
  static uint64_t LoadClock(void* raw_context) {
    // A burst of time base reads on one thread (24 within 200 us) is a timed
    // poll loop spinning: hand it to the scheduler (see
    // guest_timebase_spin_park_us).
    if (auto* spin_handler = xe::cpu::backend::timebase_spin_handler) {
      static thread_local uint64_t burst_start_ticks = 0;
      static thread_local uint32_t burst_reads = 0;
      static const uint64_t burst_window_ticks =
          Clock::host_tick_frequency_raw() / 5000;
      const uint64_t now_ticks = Clock::host_tick_count_raw();
      if (now_ticks - burst_start_ticks > burst_window_ticks) {
        burst_start_ticks = now_ticks;
        burst_reads = 0;
      } else if (++burst_reads >= 24) {
        // Reset before the call: the handler may switch fibers, and this one
        // can resume on another dispatch thread, whose counters are not the
        // ones addressed here.
        burst_reads = 0;
        burst_start_ticks = now_ticks;
        spin_handler(raw_context);
      }
    }
    return Clock::QueryGuestTickCount();
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CLOCK, LOAD_CLOCK);

// ============================================================================
// OPCODE_LOAD_OFFSET / OPCODE_STORE_OFFSET
// ============================================================================
struct LOAD_OFFSET_I8
    : Sequence<LOAD_OFFSET_I8, I<OPCODE_LOAD_OFFSET, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    e.ldrb(load_dest, ptr(e.GetMembaseReg(), e.x0));
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.mov(e.w2, load_dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI8));
    }
  }
};
struct LOAD_OFFSET_I16
    : Sequence<LOAD_OFFSET_I16, I<OPCODE_LOAD_OFFSET, I16Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    e.ldrh(load_dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev16(load_dest, load_dest);
    }
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.mov(e.w2, load_dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI16));
    }
  }
};
struct LOAD_OFFSET_I32
    : Sequence<LOAD_OFFSET_I32, I<OPCODE_LOAD_OFFSET, I32Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg load_dest = IntegerLoadDestination(e, i.instr, i.dest.reg());
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(load_dest, e.w0);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      BranchIfNotMMIO(e, normal_access);
      // MMIO path
      void* mmio_fn = (void*)&MMIOAwareLoad<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareLoad<uint32_t, true>;
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.mov(load_dest, e.w0);
      e.b(done);
      e.L(normal_access);
      {
        ComputeMemoryAddressOffset(e, i.src1, i.src2);
        e.ldr(load_dest, ptr(e.GetMembaseReg(), e.x0));
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          e.rev(load_dest, load_dest);
        }
      }
      e.L(done);
    } else {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.ldr(load_dest, ptr(e.GetMembaseReg(), e.x0));
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        e.rev(load_dest, load_dest);
      }
      if (IsTracingData()) {
        ComputeMemoryAddressOffset(e, i.src1, i.src2);
        e.mov(e.w2, load_dest);
        e.mov(e.w1, e.w0);
        e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI32));
      }
    }
  }
};
struct LOAD_OFFSET_I64
    : Sequence<LOAD_OFFSET_I64, I<OPCODE_LOAD_OFFSET, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    GprRun run;
    if (cvars::a64_gpr_save_restore_runs &&
        !IsTracingData() && MatchGprRun(i.instr, false, &run)) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.add(e.x1, e.GetMembaseReg(), e.x0);
      e.add(e.x2, e.GetContextReg(), run.context_offset);
      EmitGprRunCopy(e, run.pairs, false);
      e.fused_skip_through_ = run.last;
      return;
    }
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), e.x0));
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.rev(i.dest, i.dest);
    }
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.mov(e.x2, i.dest);
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI64));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_OFFSET, LOAD_OFFSET_I8, LOAD_OFFSET_I16,
                     LOAD_OFFSET_I32, LOAD_OFFSET_I64);

struct STORE_OFFSET_I8
    : Sequence<STORE_OFFSET_I8,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src3.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src3.reg());
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.src3.is_constant) {
      e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFF));
      e.strb(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      e.strb(store_source, ptr(e.GetMembaseReg(), e.x0));
    }
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.ldrb(e.w2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI8));
    }
  }
};
struct STORE_OFFSET_I16
    : Sequence<STORE_OFFSET_I16,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src3.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src3.reg());
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint16_t val = xe::byte_swap(static_cast<uint16_t>(i.src3.constant()));
        e.mov(e.w17, static_cast<uint64_t>(val));
      } else {
        e.rev16(e.w17, store_source);
      }
      e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.w17, static_cast<uint64_t>(i.src3.constant() & 0xFFFF));
        e.strh(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.strh(store_source, ptr(e.GetMembaseReg(), e.x0));
      }
    }
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.ldrh(e.w2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI16));
    }
  }
};
struct STORE_OFFSET_I32
    : Sequence<STORE_OFFSET_I32,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const WReg store_source = i.src3.is_constant ? e.wzr
        : IntegerStoreSource(e, i.instr, i.src3.reg());
    if (IsPossibleMMIOInstruction(e, i.instr)) {
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src1.is_constant) {
        e.mov(e.w1,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w1, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src2.constant())));
      } else {
        e.mov(e.w17, WReg(i.src2.reg().getIdx()));
      }
      e.add(e.w1, e.w1, e.w17);
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, store_source);
      }
      e.CallNativeSafe(mmio_fn);
      return;
    }
    if (cvars::emit_inline_mmio_checks && !IsTracingData()) {
      // Compute raw guest address (src1 + src2) in w17 for range check.
      if (i.src1.is_constant) {
        e.mov(e.w17,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src1.constant())));
      } else {
        e.mov(e.w17, WReg(i.src1.reg().getIdx()));
      }
      if (i.src2.is_constant) {
        uint32_t offset = static_cast<uint32_t>(i.src2.constant());
        if (offset != 0) {
          e.mov(e.w0, static_cast<uint64_t>(offset));
          e.add(e.w17, e.w17, e.w0);
        }
      } else {
        e.add(e.w17, e.w17, WReg(i.src2.reg().getIdx()));
      }
      auto& normal_access = e.NewCachedLabel();
      auto& done = e.NewCachedLabel();
      BranchIfNotMMIO(e, normal_access);
      // MMIO path — copy value to w2 before w1 in case src3 is in w1
      void* mmio_fn = (void*)&MMIOAwareStore<uint32_t, false>;
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        mmio_fn = (void*)&MMIOAwareStore<uint32_t, true>;
      }
      if (i.src3.is_constant) {
        e.mov(e.w2,
              static_cast<uint64_t>(static_cast<uint32_t>(i.src3.constant())));
      } else {
        e.mov(e.w2, store_source);
      }
      e.mov(e.w1, e.w17);
      e.CallNativeSafe(mmio_fn);
      e.b(done);
      e.L(normal_access);
      {
        ComputeMemoryAddressOffset(e, i.src1, i.src2);
        if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
          if (i.src3.is_constant) {
            uint32_t val =
                xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
            e.mov(e.w17, static_cast<uint64_t>(val));
          } else {
            e.rev(e.w17, store_source);
          }
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          if (i.src3.is_constant) {
            e.mov(e.w17, static_cast<uint64_t>(
                             static_cast<uint32_t>(i.src3.constant())));
            e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
          } else {
            e.str(store_source, ptr(e.GetMembaseReg(), e.x0));
          }
        }
      }
      e.L(done);
    } else {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
        if (i.src3.is_constant) {
          uint32_t val =
              xe::byte_swap(static_cast<uint32_t>(i.src3.constant()));
          e.mov(e.w17, static_cast<uint64_t>(val));
        } else {
          e.rev(e.w17, store_source);
        }
        e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        if (i.src3.is_constant) {
          e.mov(e.w17, static_cast<uint64_t>(
                           static_cast<uint32_t>(i.src3.constant())));
          e.str(e.w17, ptr(e.GetMembaseReg(), e.x0));
        } else {
          e.str(store_source, ptr(e.GetMembaseReg(), e.x0));
        }
      }
      if (IsTracingData()) {
        ComputeMemoryAddressOffset(e, i.src1, i.src2);
        e.ldr(e.w2, ptr(e.GetMembaseReg(), e.x0));
        e.mov(e.w1, e.w0);
        e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI32));
      }
    }
  }
};
struct STORE_OFFSET_I64
    : Sequence<STORE_OFFSET_I64,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      if (i.src3.is_constant) {
        uint64_t val = xe::byte_swap(static_cast<uint64_t>(i.src3.constant()));
        e.mov(e.x17, val);
      } else {
        e.rev(e.x17, i.src3);
      }
      e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
    } else {
      if (i.src3.is_constant) {
        e.mov(e.x17, static_cast<uint64_t>(i.src3.constant()));
        e.str(e.x17, ptr(e.GetMembaseReg(), e.x0));
      } else {
        e.str(i.src3, ptr(e.GetMembaseReg(), e.x0));
      }
    }
    if (IsTracingData()) {
      ComputeMemoryAddressOffset(e, i.src1, i.src2);
      e.ldr(e.x2, ptr(e.GetMembaseReg(), e.x0));
      e.mov(e.w1, e.w0);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI64));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_OFFSET, STORE_OFFSET_I8, STORE_OFFSET_I16,
                     STORE_OFFSET_I32, STORE_OFFSET_I64);

// ============================================================================
// OPCODE_MEMSET
// ============================================================================
static const bool zva_enable = (xe_cpu_mrs(DCZID_EL0) & 0b1'0000) == 0;
static const uint64_t zva_length = (4ULL << (xe_cpu_mrs(DCZID_EL0) & 0b0'1111));

struct MEMSET_I64
    : Sequence<MEMSET_I64, I<OPCODE_MEMSET, VoidOp, I64Op, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    assert_true(i.src3.is_constant);
    assert_true(i.src2.constant() == 0);
    // memset(membase + guest_addr, 0, length)
    // Only used by dcbz/dcbz128: constant zero value, constant aligned size.
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x0, e.GetMembaseReg(), addr);
    const uint64_t len = i.src3.constant();
    uint64_t off = 0;

    // Use `dc zva` if it writes more bytes at a time than STP
    if (zva_enable && len >= zva_length && zva_length > 16) {
      for (; off + zva_length <= len; off += zva_length) {
        // dc zva, x0
        e.sys(0b011, 0b0111, 0b0100, 0b001, e.x0);
        if (off + zva_length < len) {
          e.add(e.x0, e.x0, zva_length);
        }
      }
    }

    // Inline with STP xzr, xzr pairs (16 bytes each)
    for (; off + 16 <= len; off += 16) {
      e.stp(e.xzr, e.xzr, AdrPostImm(e.x0, 16));
    }
    // Handle remaining bytes (0-15)
    if (off + 8 <= len) {
      e.str(e.xzr, AdrPostImm(e.x0, 8));
      off += 8;
    }
    if (off + 4 <= len) {
      e.str(e.wzr, AdrPostImm(e.x0, 4));
      off += 4;
    }
    // Byte loop for any remaining 0-3 bytes
    for (; off + 1 <= len; off += 1) {
      e.strb(e.wzr, AdrPostImm(e.x0, 1));
    }

    if (IsTracingData()) {
      auto trace_addr = ComputeMemoryAddress(e, i.src1);
      e.mov(e.w3, static_cast<uint64_t>(i.src3.constant()));
      e.mov(e.w2, static_cast<uint64_t>(i.src2.constant()));
      e.mov(e.w1, WReg(trace_addr.getIdx()));
      e.CallNative(reinterpret_cast<void*>(TraceMemset));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMSET, MEMSET_I64);

// ============================================================================
// ============================================================================
// OPCODE_LOAD_MMIO / OPCODE_STORE_MMIO
// ============================================================================
struct LOAD_MMIO_I32
    : Sequence<LOAD_MMIO_I32, I<OPCODE_LOAD_MMIO, I32Op, OffsetOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto read_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOReadCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(read_address));
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->read));
    e.rev(e.w0, e.w0);
    e.mov(i.dest, e.w0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_MMIO, LOAD_MMIO_I32);

struct STORE_MMIO_I32
    : Sequence<STORE_MMIO_I32,
               I<OPCODE_STORE_MMIO, VoidOp, OffsetOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    auto write_address = uint32_t(i.src2.value);
    // CallNativeSafe: thunk sets x0=PPCContext*, x1/x2/x3 pass through.
    // MMIOWriteCallback(void* ppc_ctx, void* callback_ctx, uint32_t addr,
    //                   uint32_t value).
    e.mov(e.x1, uint64_t(mmio_range->callback_context));
    e.mov(e.w2, static_cast<uint64_t>(write_address));
    if (i.src3.is_constant) {
      e.mov(e.w3, static_cast<uint64_t>(
                      xe::byte_swap(static_cast<uint32_t>(i.src3.constant()))));
    } else {
      e.mov(e.w3, i.src3);
      e.rev(e.w3, e.w3);
    }
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->write));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_MMIO, STORE_MMIO_I32);

// ============================================================================
// OPCODE_RESERVED_LOAD / OPCODE_RESERVED_STORE
// ============================================================================
// Helper: get pointer to A64BackendContext.
// x19 is the dedicated backend context register, so this is a no-op
// accessor for readability. The returned register is x19.
static const Xbyak_aarch64::XReg& LoadBackendCtxPtr(A64Emitter& e) {
  return e.GetBackendCtxReg();
}

// Two paths, selected by a64_native_reserved_ops:
//  - Software (cvar off): RESERVED_LOAD/STORE call host helpers that share a
//    per-granule generation counter, so a stwcx. on one thread invalidates
//    concurrent lwarx reservations on others.
//  - Native (cvar on, default): inline, no thunk. lwarx plain-loads the word,
//    stashes it in cached_reserve_value_, and arms a per-thread reserve flag;
//    stwcx. validates with one LSE CASAL. An EARLIER native design armed the
//    hardware exclusive monitor at lwarx and consumed it with a bare stlxr at
//    stwcx., but that monitor only survives a window with no intervening memory
//    access - a register spill or context/guest load-store between the (far
//    apart) guest lwarx and stwcx. clears it every iteration, so stlxr never
//    succeeds and the guest retry loop livelocks (hung Forza Horizon). The CAS
//    is a single atomic with no such window. The guest byte-swap is a separate
//    HIR op on both paths, so the captured/compared value is the raw word.
//    Upstream's caveat applies to the native path: comparing the cached
//    value cannot see an ABA where another thread writes and restores the
//    word between the guest's lwarx and stwcx.; the software path's
//    generation counter does catch that.
// The width of a generation counter's index, the granule number's low bits.
constexpr uint32_t kReserveEntryBits = 20;
static_assert((1u << kReserveEntryBits) == A64_RESERVE_NUM_ENTRIES);
static_assert(offsetof(ReserveHelper, generations) == 0);

// w1 = the guest address of a reserved access as the software helpers key
// reservations, before ComputeMemoryAddress offsets it.
static void LoadReserveAddress(A64Emitter& e, const I64Op& guest) {
  if (guest.is_constant) {
    e.mov(e.w1,
          static_cast<uint64_t>(static_cast<uint32_t>(guest.constant())));
  } else {
    e.mov(e.w1, WReg(guest.reg().getIdx()));
  }
}

// x2 = the generation counter of the granule of the guest address in w1 (the
// software helpers' ReserveGranule). Clobbers |scratch|.
static void LoadReserveGranule(A64Emitter& e, const XReg& scratch) {
  e.ldr(e.x2, ptr(LoadBackendCtxPtr(e),
                  static_cast<uint32_t>(
                      offsetof(A64BackendContext, reserve_helper_))));
  e.ubfx(WReg(scratch.getIdx()), e.w1, A64_RESERVE_GRANULE_SHIFT,
         kReserveEntryBits);
  e.add(e.x2, e.x2, scratch, Xbyak_aarch64::LSL, 2);
}

// Native lwarx before the word: records the guest address and the generation
// of its granule. The acquire keeps the word, read next, from being older
// than the generation - a store landing in between moved it on, so stwcx.
// fails. Clobbers x1-x3.
static void EmitNativeReserve(A64Emitter& e, const I64Op& guest) {
  auto bctx = LoadBackendCtxPtr(e);
  LoadReserveAddress(e, guest);
  LoadReserveGranule(e, e.x3);
  e.ldar(e.w3, ptr(e.x2));
  e.str(e.w3, ptr(bctx, static_cast<uint32_t>(offsetof(
                            A64BackendContext, reserve_generation))));
  e.str(e.w1, ptr(bctx, static_cast<uint32_t>(
                            offsetof(A64BackendContext, reserve_address))));
}

// After a store-conditional stored, with w0 = 1: wakes guest threads parked in
// GuestScheduler::NoteIdleSpin (the store clears their exclusive monitor on
// this line). Clobbers x2.
static void EmitStoreConditionalWake(A64Emitter& e) {
  if (!cvars::guest_spin_wfe) {
    return;
  }
  e.mov(e.x2,
        reinterpret_cast<uint64_t>(&xe::threading::guest_wake_generation));
  e.staddl(e.w0, ptr(e.x2));
  static_assert(offsetof(ppc::PPCContext, wake_self_bumps) < 16384 &&
                offsetof(ppc::PPCContext, wake_self_bumps) % 4 == 0);
  const auto self_bumps_ptr =
      ptr(e.GetContextReg(),
          static_cast<uint32_t>(offsetof(ppc::PPCContext, wake_self_bumps)));
  e.ldr(e.w2, self_bumps_ptr);
  e.add(e.w2, e.w2, 1);
  e.str(e.w2, self_bumps_ptr);
}

// Exclusive stores a stwcx. retries before falling back (see below).
constexpr uint32_t kStoreConditionalExclusiveTries = 16;

// Native stwcx.: the value to store in x3 (w3 for a word). Consumes the
// reservation (stwcx. always does, stored or not); fails without one (no
// matching lwarx, exactly like PPC), with one on another address (like the
// software helpers), or when the granule's generation or the word moved since
// the lwarx. Otherwise stores and moves the generation on, failing every
// other reservation on the granule. i.dest (CR0.eq) = stored. Clobbers x0-x2,
// x4, x5 and x16.
//
// Both checks sit between the ldaxr and the stlxr, so a context switch or a
// store to the word anywhere before the store fails it and the loop checks
// again - checking the generation before the ldaxr would leave a window for
// a thread descheduled there. The generation load inside the exclusive pair
// is outside the architecture's forward-progress guarantee, so after a few
// failed exclusive stores (on a core whose monitor that load clears) it falls
// back to checking the generation, then one CAS on the word: never a
// livelock, at worst the old window.
template <typename Reg>
static void EmitNativeStoreConditional(A64Emitter& e, const I64Op& guest,
                                       const Reg& value,
                                       const Xbyak_aarch64::WReg& dest) {
  static_assert(std::is_same_v<Reg, XReg> || std::is_same_v<Reg, WReg>);
  auto bctx = LoadBackendCtxPtr(e);
  LoadReserveAddress(e, guest);
  auto addr = ComputeMemoryAddress(e, guest);
  e.add(e.x16, e.GetMembaseReg(), addr);
  auto& sc_retry = e.NewCachedLabel();
  auto& sc_stored = e.NewCachedLabel();
  auto& sc_fail_exclusive = e.NewCachedLabel();
  auto& sc_fail = e.NewCachedLabel();
  auto& sc_done = e.NewCachedLabel();
  const uint32_t kFlagsOff =
      static_cast<uint32_t>(offsetof(A64BackendContext, flags));
  e.ldr(e.w0, ptr(bctx, kFlagsOff));
  e.and_(e.w2, e.w0, uint64_t(1) << kA64BackendHasReserveBit);
  e.eor(e.w0, e.w0, e.w2);  // clear reserve flag (w2 = mask if set, else 0)
  e.str(e.w0, ptr(bctx, kFlagsOff));
  e.cbz(e.w2, sc_fail);
  e.ldr(e.w2, ptr(bctx, static_cast<uint32_t>(
                            offsetof(A64BackendContext, reserve_address))));
  e.cmp(e.w2, e.w1);
  e.bne(sc_fail);
  LoadReserveGranule(e, e.x0);
  // w4 = the generation at the lwarx, x1/w1 = the word then.
  e.ldr(e.w4, ptr(bctx, static_cast<uint32_t>(offsetof(
                            A64BackendContext, reserve_generation))));
  const Reg expected(1);
  const Reg current(0);
  e.ldr(expected, ptr(bctx, static_cast<uint32_t>(offsetof(
                                A64BackendContext, cached_reserve_value_))));
  e.mov(e.w5, kStoreConditionalExclusiveTries);
  e.L(sc_retry);
  e.ldaxr(current, ptr(e.x16));
  e.cmp(current, expected);
  e.bne(sc_fail_exclusive);
  // The acquire of the ldaxr keeps this from being older than the word.
  e.ldr(e.w0, ptr(e.x2));
  e.cmp(e.w0, e.w4);
  e.bne(sc_fail_exclusive);
  e.stlxr(e.w0, value, ptr(e.x16));
  e.cbz(e.w0, sc_stored);
  e.subs(e.w5, e.w5, 1);
  e.bne(sc_retry);
  // Fallback: the generation, then one CAS on the word.
  e.ldar(e.w0, ptr(e.x2));
  e.cmp(e.w0, e.w4);
  e.bne(sc_fail);
  e.mov(current, expected);
  e.casal(current, value, ptr(e.x16));
  e.cmp(current, expected);
  e.bne(sc_fail);
  e.L(sc_stored);
  e.mov(e.w0, 1);
  e.staddl(e.w0, ptr(e.x2));
  EmitStoreConditionalWake(e);
  e.mov(dest, 1);
  e.b(sc_done);
  e.L(sc_fail_exclusive);
  e.clrex(15);
  e.L(sc_fail);
  e.mov(dest, 0);
  e.L(sc_done);
}

// ============================================================================
// OPCODE_ATOMIC_COMPARE_EXCHANGE
// ============================================================================
// The store-conditional of a statically paired lwarx/stwcx. (see
// PPCHIRBuilder::AnalyzeReservedPair): one compare-and-swap on the word. A
// successful one moves the granule's generation on like any other
// store-conditional, so reservations taken by the unpaired sequences above
// still see it. Clobbers x0-x6 and x16.
template <typename Reg, typename Op>
static void EmitAtomicCompareExchange(A64Emitter& e, const I64Op& guest,
                                      const Op& expected_op,
                                      const Op& desired_op,
                                      const Xbyak_aarch64::WReg& dest) {
  static_assert(std::is_same_v<Reg, XReg> || std::is_same_v<Reg, WReg>);
  const bool bump = cvars::a64_pair_cas_bump_generation;
  if (bump) {
    // Before dest, which may share a register with the address.
    LoadReserveAddress(e, guest);
  }
  auto addr = ComputeMemoryAddress(e, guest);
  e.add(e.x16, e.GetMembaseReg(), addr);
  const uint64_t mask =
      std::is_same_v<Reg, XReg> ? ~uint64_t(0) : uint64_t(0xFFFFFFFF);
  Reg expected(5);
  Reg desired(6);
  if (expected_op.is_constant) {
    e.mov(expected, static_cast<uint64_t>(expected_op.constant()) & mask);
  } else {
    expected = Reg(expected_op.reg().getIdx());
  }
  if (desired_op.is_constant) {
    e.mov(desired, static_cast<uint64_t>(desired_op.constant()) & mask);
  } else {
    desired = Reg(desired_op.reg().getIdx());
  }
  const Reg current(0);
  if (e.IsFeatureEnabled(kA64EmitLSE)) {
    e.mov(current, expected);
    e.casal(current, desired, ptr(e.x16));
    e.cmp(current, expected);
  } else {
    auto& retry = e.NewCachedLabel();
    auto& out = e.NewCachedLabel();
    e.L(retry);
    e.ldaxr(current, ptr(e.x16));
    e.cmp(current, expected);
    e.bne(out);
    e.stlxr(e.w4, desired, ptr(e.x16));
    e.cbnz(e.w4, retry);
    e.cmp(e.w4, e.w4);  // eq
    e.L(out);
  }
  e.cset(dest, Xbyak_aarch64::EQ);
  if (bump) {
    auto& skip = e.NewCachedLabel();
    e.bne(skip);
    LoadReserveGranule(e, e.x3);
    e.mov(e.w0, 1);
    e.staddl(e.w0, ptr(e.x2));
    EmitStoreConditionalWake(e);
    e.L(skip);
  }
}
struct ATOMIC_COMPARE_EXCHANGE_I32
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I32,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicCompareExchange<WReg>(e, i.src1, i.src2, i.src3, i.dest);
  }
};
struct ATOMIC_COMPARE_EXCHANGE_I64
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I64,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicCompareExchange<XReg>(e, i.src1, i.src2, i.src3, i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ATOMIC_COMPARE_EXCHANGE,
                     ATOMIC_COMPARE_EXCHANGE_I32, ATOMIC_COMPARE_EXCHANGE_I64);

// A new guest reserve-load always replaces the previous reservation. The
// paired path implements the new one by CAS, so retire the older backend bit
// now (also on exits taking the no-op release), rather than leaving it armed.
template <typename Reg>
static void EmitPairedReservedLoad(A64Emitter& e, const I64Op& guest,
                                   const Reg& dest) {
  const uint32_t flags_offset =
      static_cast<uint32_t>(offsetof(A64BackendContext, flags));
  auto bctx = LoadBackendCtxPtr(e);
  e.ldr(e.w0, ptr(bctx, flags_offset));
  e.and_(e.w0, e.w0, uint64_t(uint32_t(~(1u << kA64BackendHasReserveBit))));
  e.str(e.w0, ptr(bctx, flags_offset));
  auto addr = ComputeMemoryAddress(e, guest);
  e.ldr(dest, ptr(e.GetMembaseReg(), addr));
}

struct RESERVED_LOAD_I32
    : Sequence<RESERVED_LOAD_I32, I<OPCODE_RESERVED_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & RESERVED_LOAD_PAIRED_CAS) {
      EmitPairedReservedLoad(e, i.src1, i.dest);
      return;
    }
    if (cvars::a64_native_reserved_ops && !IsPossibleMMIOInstruction(e, i.instr)) {
      // The generation, then plain load + capture (value, reserve flag); the
      // matching stwcx. validates both in one exclusive window.
      EmitNativeReserve(e, i.src1);
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x16, e.GetMembaseReg(), addr);
      e.ldr(i.dest, ptr(e.x16));
      auto bctx = LoadBackendCtxPtr(e);
      e.str(i.dest, ptr(bctx, static_cast<uint32_t>(offsetof(
                                A64BackendContext, cached_reserve_value_))));
      const uint32_t kFlagsOff =
          static_cast<uint32_t>(offsetof(A64BackendContext, flags));
      e.ldr(e.w0, ptr(bctx, kFlagsOff));
      e.orr(e.w0, e.w0, uint64_t(1) << kA64BackendHasReserveBit);
      e.str(e.w0, ptr(bctx, kFlagsOff));
      return;
    }
    if (i.src1.is_constant) {
      e.mov(e.w1, static_cast<uint32_t>(i.src1.constant()));
    } else {
      e.mov(e.w1, WReg(i.src1.reg().getIdx()));
    }
    e.CallNativeSafe(e.backend()->try_acquire_reservation_helper_);
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    auto bctx = LoadBackendCtxPtr(e);
    e.mov(e.w0, i.dest);
    e.str(e.x0, ptr(bctx, static_cast<uint32_t>(offsetof(
                              A64BackendContext, cached_reserve_value_))));
  }
};
struct RESERVED_LOAD_I64
    : Sequence<RESERVED_LOAD_I64, I<OPCODE_RESERVED_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & RESERVED_LOAD_PAIRED_CAS) {
      EmitPairedReservedLoad(e, i.src1, i.dest);
      return;
    }
    if (cvars::a64_native_reserved_ops && !IsPossibleMMIOInstruction(e, i.instr)) {
      // The generation, then plain load + capture (value, reserve flag); the
      // matching stwcx. validates both in one exclusive window.
      EmitNativeReserve(e, i.src1);
      auto addr = ComputeMemoryAddress(e, i.src1);
      e.add(e.x16, e.GetMembaseReg(), addr);
      e.ldr(i.dest, ptr(e.x16));
      auto bctx = LoadBackendCtxPtr(e);
      e.str(i.dest, ptr(bctx, static_cast<uint32_t>(offsetof(
                                A64BackendContext, cached_reserve_value_))));
      const uint32_t kFlagsOff =
          static_cast<uint32_t>(offsetof(A64BackendContext, flags));
      e.ldr(e.w0, ptr(bctx, kFlagsOff));
      e.orr(e.w0, e.w0, uint64_t(1) << kA64BackendHasReserveBit);
      e.str(e.w0, ptr(bctx, kFlagsOff));
      return;
    }
    if (i.src1.is_constant) {
      e.mov(e.w1, static_cast<uint32_t>(i.src1.constant()));
    } else {
      e.mov(e.w1, WReg(i.src1.reg().getIdx()));
    }
    e.CallNativeSafe(e.backend()->try_acquire_reservation_helper_);
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.ldr(i.dest, ptr(e.GetMembaseReg(), addr));
    auto bctx = LoadBackendCtxPtr(e);
    e.str(i.dest, ptr(bctx, static_cast<uint32_t>(offsetof(
                                A64BackendContext, cached_reserve_value_))));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_LOAD, RESERVED_LOAD_I32,
                     RESERVED_LOAD_I64);

// Native SC: fail if no matching lwarx armed the per-thread flag (and always
// clear it - PPC stwcx. unconditionally releases). Otherwise one LSE CASAL
// against the value captured at lwarx: if memory still holds it, swap in the
// new (already HIR-byte-swapped) value and report success; otherwise a store
// landed since lwarx, so fail. CR0.eq (i.dest) = compare matched. The CAS is a
// single atomic instruction, so unlike a spanning ldaxr/stlxr there is no
// window for a spill/context access to clear and no possibility of livelock.
// Value-CAS carries the same (rare, benign) ABA characteristic as the software
// path; it does not need that path's contended global bitmap.
struct RESERVED_STORE_I32
    : Sequence<RESERVED_STORE_I32,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (cvars::a64_native_reserved_ops && !IsPossibleMMIOInstruction(e, i.instr)) {
      // Stage the value before clobbering w0 via ComputeMemoryAddress.
      if (i.src2.is_constant) {
        e.mov(e.w3, static_cast<uint32_t>(i.src2.constant()));
      } else {
        e.mov(e.w3, WReg(i.src2.reg().getIdx()));
      }
      EmitNativeStoreConditional(e, i.src1, e.w3, i.dest);
      return;
    }
    // Compute host address into x2 first; ComputeMemoryAddress writes w0
    // and CallNativeSafe will clobber x0-x18 anyway, so x2 must be set up
    // before populating other arg regs (and before the call).
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x2, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.w3, static_cast<uint32_t>(i.src2.constant()));
    } else {
      e.mov(e.w3, WReg(i.src2.reg().getIdx()));
    }
    if (i.src1.is_constant) {
      e.mov(e.w1, static_cast<uint32_t>(i.src1.constant()));
    } else {
      e.mov(e.w1, WReg(i.src1.reg().getIdx()));
    }
    e.CallNativeSafe(e.backend()->reserved_store_32_helper);
    e.mov(i.dest, e.w0);
  }
};
struct RESERVED_STORE_I64
    : Sequence<RESERVED_STORE_I64,
               I<OPCODE_RESERVED_STORE, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (cvars::a64_native_reserved_ops && !IsPossibleMMIOInstruction(e, i.instr)) {
      if (i.src2.is_constant) {
        e.mov(e.x3, static_cast<uint64_t>(i.src2.constant()));
      } else {
        e.mov(e.x3, XReg(i.src2.reg().getIdx()));
      }
      EmitNativeStoreConditional(e, i.src1, e.x3, i.dest);
      return;
    }
    auto addr = ComputeMemoryAddress(e, i.src1);
    e.add(e.x2, e.GetMembaseReg(), addr);
    if (i.src2.is_constant) {
      e.mov(e.x3, static_cast<uint64_t>(i.src2.constant()));
    } else {
      e.mov(e.x3, XReg(i.src2.reg().getIdx()));
    }
    if (i.src1.is_constant) {
      e.mov(e.w1, static_cast<uint32_t>(i.src1.constant()));
    } else {
      e.mov(e.w1, WReg(i.src1.reg().getIdx()));
    }
    e.CallNativeSafe(e.backend()->reserved_store_64_helper);
    e.mov(i.dest, e.w0);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RESERVED_STORE, RESERVED_STORE_I32,
                     RESERVED_STORE_I64);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
