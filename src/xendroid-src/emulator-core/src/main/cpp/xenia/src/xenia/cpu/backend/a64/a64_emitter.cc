/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <unordered_set>
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_volatile_gpr.h"

#include <bit>
#include <cstring>

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_int64(a64_max_stackpoints);
DECLARE_bool(a64_enable_host_guest_stack_synchronization);
DECLARE_bool(a64_near_branches);
DECLARE_bool(a64_longjmp_sync_at_resolve);
DEFINE_bool(a64_call_return_address_in_register, true,
            "A guest call takes its guest return address from the register "
            "it was just put in (or makes the constant again) instead of "
            "from the frame slot it used to be written to on every call.",
            "a64");
DEFINE_bool(a64_indirection_bases_in_context, true,
            "Indirect guest calls read the two bases of the indirection table "
            "lookup from the backend context with one load pair instead of "
            "materializing them (five instructions on Android), and keep the "
            "rare external-entry form out of line.",
            "a64");
DEFINE_bool(a64_shared_cold_paths, true,
            "The rarely taken halves of the sequences every guest function "
            "has many of (the yield of a preempt check, the lookup stub of a "
            "direct guest call, the external form of an indirect one, the "
            "stackpoint overflow report) are routines shared by all functions "
            "and reached through the backend context, instead of a copy at "
            "every site: about a fifth less generated code.",
            "a64");
DEFINE_bool(a64_near_bit_branches, true,
            "Emit tbz/tbnz of guest functions direct as well (a64_near_branches "
            "leaves them in the long-range form: they reach +/-32 KiB only). "
            "A function they don't reach across is emitted again without "
            "them.",
            "a64");
DEFINE_uint32(a64_near_bit_branch_max_guest_bytes, 6 * 1024,
              "Largest guest function, in bytes of guest code, that "
              "a64_near_bit_branches tries first with direct tbz/tbnz.",
              "a64");
DECLARE_bool(a64_patchable_guest_calls);

DEFINE_bool(a64_fpcr_skip_redundant_write, true,
            "Track at run time which cached FPCR value is in effect and skip "
            "the FPCR write of a mode switch to the mode already in effect.",
            "CPU");

DEFINE_bool(a64_leaf_skip_stackpoint, true,
            "Skip the longjmp stackpoint push/pop in guest functions that "
            "make no guest or host call (only their own blr return).",
            "CPU");

DEFINE_bool(a64_reuse_x0_constant, true,
            "Materialize a guest call's return address once for the "
            "return-address slot, the LR context store and the call.",
            "CPU");

DEFINE_bool(
    log_safepoint_pc, false,
    "Record the guest address of every JIT safepoint a fiber passes, so the "
    "cooperative scheduler's no-progress report can name where a wedged "
    "fiber last checked in rather than only its link register. Costs a "
    "materialize and a store on every loop back-edge; diagnostic only.",
    "CPU");
DEFINE_bool(
    a64_safepoint_pc_cold_only, true,
    "With log_safepoint_pc (forced on by savestate_enable), write the "
    "safepoint's guest address only on the taken cold path, just before the "
    "yield handler runs, instead of on every pass. The snapshot parks a fiber "
    "only from that handler, so it reads the same address; the scheduler's "
    "no-progress report then names the last safepoint that yielded.",
    "CPU");
UPDATE_from_bool(a64_safepoint_pc_cold_only, 2026, 11, 15, 1, false);

DECLARE_bool(a64_vconst_pool);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace Xbyak_aarch64;

// Defined in a64_backend.cc.
extern uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

static uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  XELOGE("undefined extern call to {:08X} {}", function->address(),
         function->name());
  return 0;
}

// Max host code per translated guest function. 1 MiB was too small for large,
// FP-heavy guest functions (each PowerPC FP op emits NaN-handling overhead),
// which overflowed the Xbyak buffer -> uncaught Xbyak_aarch64::Error "code is
// too big" -> terminate. The buffer is a single per-emitter allocation reused
// (reset) across functions, so enlarging it is cheap.
static constexpr size_t kMaxCodeSize = 4_MiB;

// Register maps:
// GPR allocatable registers: x22, x23, x24, x25, x26, x27, x28
// (x19=backend context, x20=context, x21=membase are reserved)
const uint32_t A64Emitter::gpr_reg_map_[GPR_COUNT] = {
    22, 23, 24, 25, 26, 27, 28, 10, 11, 12, 13, 14, 15,
};

// VEC allocatable registers: v4-v15, v16-v31
// (v0-v3 are scratch)
const uint32_t A64Emitter::vec_reg_map_[VEC_COUNT] = {
    4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

namespace {
bool IsGuestCallOpcode(const hir::Instr* instr) {
  const auto opcode = instr->GetOpcodeNum();
  return opcode == hir::OPCODE_CALL || opcode == hir::OPCODE_CALL_TRUE ||
         opcode == hir::OPCODE_CALL_INDIRECT ||
         opcode == hir::OPCODE_CALL_INDIRECT_TRUE;
}

// The call a SET_RETURN_ADDRESS is for: the next one in its block, nothing
// that could come between them at run time in the way.
const hir::Instr* PairedCall(const hir::Instr* set_return_address) {
  for (const hir::Instr* next = set_return_address->next; next;
       next = next->next) {
    if (next->GetOpcodeNum() == hir::OPCODE_SET_RETURN_ADDRESS) {
      return nullptr;
    }
    if (IsGuestCallOpcode(next)) {
      return (next->flags & hir::CALL_TAIL) ? nullptr : next;
    }
  }
  return nullptr;
}
}  // namespace

A64Emitter::A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator)
    : CodeGenerator(kMaxCodeSize, Xbyak_aarch64::AutoGrow, allocator),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()),
      allocator_(allocator),
      feature_flags_(arm64::GetFeatureFlags()) {}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, hir::HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  guest_module_ = dynamic_cast<XexModule*>(function->module());

  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  trace_data_ = &function->trace_data();

  current_guest_function_ = function->address();
  log_guest_call_ = !cvars::log_guest_calls_at.empty() &&
                    GuestAddressInList(cvars::log_guest_calls_at,
                                       current_guest_function_);

  auto reset_emission_attempt = [this]() {
    static_assert(kExtendedGprGuestStashOffset ==
                  StackLayout::GUEST_STACK_SIZE);
    guest_stack_base_size_ = StackLayout::GUEST_STACK_SIZE +
        (backend()->extended_gpr_allocator() ? kExtendedGprSaveBytes : 0);
    stack_size_ = guest_stack_base_size_;
    source_map_arena_.Reset();
    tail_code_.clear();
    pinned_preempt_paths_.clear();
    emitting_tail_ = false;
    call_return_paired_call_ = nullptr;
    fpcr_mode_ = FPCRMode::Unknown;
    guest_call_sites_ = 0;
    helper_branch_sites_ = 0;
    patchable_call_sites_.clear();
    synchronize_stack_on_next_instruction_ = false;
    vconst_pool_emit_failed_ = false;
    fused_skip_instr_ = nullptr;
    fused_truncate_store_instr_ = nullptr;
    fused_truncate_store_src_reg_ = -1;
    fused_flag_branch_instr_ = nullptr;
    fused_skip_through_ = nullptr;
    ForgetX0Constant();
  };
  vconst_pool_force_disabled_ = false;
  reset_emission_attempt();

  // The prolog, epilog and helpers emit outside the per-opcode guard below, so
  // an unencodable operand needs catching here too.
  EmitFunctionInfo func_info = {};
  // Once, see LayOutLocals.
  const size_t stack_size = LayOutLocals(builder);
  auto emit_attempt = [&]() {
    try {
      return Emit(builder, stack_size, func_info);
    } catch (const Xbyak_aarch64::Error& e) {
      if (IsNearBranchOutOfRange(e)) {
        return false;
      }
      XELOGE("A64: assembler error while emitting guest function {:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
  };
  // Conditional branches direct first; a function too large for them to reach
  // across is emitted again with the long-range form.
  near_branches_ = cvars::a64_near_branches;
  // tbz/tbnz reach +/-32 KiB only: direct in functions likely to fit (the
  // guest size is all that is known here; one that doesn't fit after all is
  // emitted again without them, then without any direct branch).
  near_bit_branches_ =
      near_branches_ && cvars::a64_near_bit_branches &&
      (!function->has_end_address() ||
       function->end_address() - function->address() <=
           cvars::a64_near_bit_branch_max_guest_bytes);
  near_branch_out_of_range_ = false;
  bool emitted = emit_attempt();
  if (!emitted && near_branch_out_of_range_ && near_bit_branches_) {
    ResetPerFunctionState();
    reset_emission_attempt();
    func_info = {};
    near_bit_branches_ = false;
    near_branch_out_of_range_ = false;
    emitted = emit_attempt();
  }
  near_bit_branches_ = false;
  if (!emitted && near_branch_out_of_range_) {
    XELOGI(
        "A64: guest function {:08X} too large for direct branches, emitted "
        "again with long-range ones",
        current_guest_function_);
    ResetPerFunctionState();
    reset_emission_attempt();
    func_info = {};
    near_branches_ = false;
    near_branch_out_of_range_ = false;
    emitted = emit_attempt();
  }
  if (!emitted && vconst_pool_emit_failed_) {
    // The code buffer is still private: Emplace / PlaceGuestCode runs only
    // after a successful Emit. Rebuild this function with the existing inline
    // materializers rather than publishing a partial function or dropping a
    // constant whose literal could not reach the pool.
    ResetPerFunctionState();
    reset_emission_attempt();
    func_info = {};
    vconst_pool_force_disabled_ = true;
    emitted = emit_attempt();
    if (!emitted && near_branch_out_of_range_) {
      // Both: inline constants and long-range branches.
      ResetPerFunctionState();
      reset_emission_attempt();
      func_info = {};
      near_branches_ = false;
      near_branch_out_of_range_ = false;
      emitted = emit_attempt();
    }
    vconst_pool_force_disabled_ = false;
  }
  near_branches_ = false;
  if (!emitted) {
    // Emplace only runs on success, so a failed compile has to reset too.
    vconst_pool_force_disabled_ = false;
    ResetPerFunctionState();
    return false;
  }

  // Emplace the code into the code cache.
  *out_code_address = Emplace(func_info, function);
  *out_code_size = func_info.code_size.total;

  // Copy source map.
  source_map_arena_.CloneContents(out_source_map);

  return *out_code_address != nullptr;
}

size_t A64Emitter::LayOutLocals(hir::HIRBuilder* builder) {
  // Calculate local variable stack offsets.
  auto locals = builder->locals();
  size_t stack_offset = guest_stack_base_size_;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = hir::GetTypeSize(slot->type);
    // Align to natural size (at least 4 bytes for ARM64 alignment).
    size_t align_size = xe::round_up(type_size, static_cast<size_t>(4));
    stack_offset = xe::align(stack_offset, align_size);
    slot->set_constant(static_cast<uint32_t>(stack_offset));
    stack_offset += type_size;
  }
  // Align total stack offset to 16 bytes (ARM64 ABI requirement).
  stack_offset -= guest_stack_base_size_;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));
  return guest_stack_base_size_ + stack_offset;
}

bool A64Emitter::Emit(hir::HIRBuilder* builder, size_t stack_size,
                      EmitFunctionInfo& func_info) {
  // ARM64 ABI: SP must always be 16-byte aligned.
  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  func_info.lr_save_offset = StackLayout::HOST_RET_ADDR;
  stack_size_ = stack_size;

  struct {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t prolog_stack_alloc;
  } code_offsets = {};
  vconst_pool_enabled_ =
      cvars::a64_vconst_pool && !vconst_pool_force_disabled_;

  // A function that never calls out cannot have a frame resumed inside it.
  skip_stackpoint_ = false;
  if (cvars::a64_leaf_skip_stackpoint && !IsTracingFunc() &&
      !log_guest_call_) {
    bool calls_out = false;
    for (auto scan = builder->first_block(); scan && !calls_out;
         scan = scan->next) {
      for (const hir::Instr* instr = scan->instr_head; instr;
           instr = instr->next) {
        const uint16_t opcode = instr->GetOpcodeNum();
        if (opcode == hir::OPCODE_CALL || opcode == hir::OPCODE_CALL_TRUE ||
            opcode == hir::OPCODE_CALL_EXTERN ||
            opcode == hir::OPCODE_CALL_INDIRECT_TRUE ||
            (opcode == hir::OPCODE_CALL_INDIRECT &&
             (instr->flags &
              (hir::CALL_TAIL | hir::CALL_POSSIBLE_RETURN)) !=
                 (hir::CALL_TAIL | hir::CALL_POSSIBLE_RETURN))) {
          calls_out = true;
          break;
        }
      }
    }
    skip_stackpoint_ = !calls_out;
  }

  // ========================================================================
  // PROLOG
  // ========================================================================
  code_offsets.prolog = getSize();

  // sub sp, sp, #stack_size
  if (stack_size <= 4095) {
    sub(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    sub(sp, sp, x17, UXTX);
  }
  code_offsets.prolog_stack_alloc = getSize();

  // Store host return address (x30/LR) so the epilog can restore it.
  str(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  // Store guest PPC return address (passed in x0 by convention).
  str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
  // Store zero for call return address (we haven't made a call yet) - unless
  // no call of this function reads the slot
  // (a64_call_return_address_in_register).
  call_return_slot_unused_ = false;
  if (cvars::a64_call_return_address_in_register) {
    call_return_slot_unused_ = true;
    const hir::Instr* pending_set = nullptr;
    for (auto block = builder->first_block();
         block && call_return_slot_unused_; block = block->next) {
      pending_set = nullptr;
      for (const hir::Instr* instr = block->instr_head; instr;
           instr = instr->next) {
        if (instr->GetOpcodeNum() == hir::OPCODE_SET_RETURN_ADDRESS) {
          pending_set = instr;
        } else if (IsGuestCallOpcode(instr) &&
                   !(instr->flags & hir::CALL_TAIL)) {
          if (!pending_set || PairedCall(pending_set) != instr) {
            call_return_slot_unused_ = false;
            break;
          }
          pending_set = nullptr;
        }
      }
    }
  }
  if (!call_return_slot_unused_) {
    str(xzr, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
  }

  // Record stackpoint for longjmp recovery.
  PushStackpoint();

  // ========================================================================
  // BODY
  // ========================================================================
  code_offsets.body = getSize();

  // FTrace: log guest function entry when the backend was built with
  // function tracing available (gated at runtime by the trace_func flag).
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionEntry));
  }
  if (log_guest_call_) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(LogGuestCallEntry));
  }

  // Allocate the epilog label (owned by label_cache_ for cleanup).
  auto epilog_label_ptr = new Label();
  label_cache_.push_back(epilog_label_ptr);
  epilog_label_ = epilog_label_ptr;

  // Initialize before binding any block labels: backedges to the entry must
  // keep the cached state rather than reloading an older memory context.
  pinned_context_slots_ = builder->pinned_context_slots();
  uint32_t initialize_mask = 0;
  for (size_t n = 0; n < pinned_context_slots_.size(); ++n) {
    if (pinned_context_slots_[n].initialize)
      initialize_mask |= uint32_t{1} << n;
  }
  EmitPinnedContext(true, initialize_mask);

  for (auto* block = builder->first_block(); block; block = block->next) {
    for (auto* instr = block->instr_head; instr; instr = instr->next) {
      if (instr->opcode == &hir::OPCODE_CHECK_PREEMPT_info) {
        const uint64_t key = (uint64_t(uint32_t(instr->src2.offset)) << 32) |
                             uint32_t(instr->src3.offset);
        ++pinned_preempt_paths_[key].first;
      }
    }
  }

  // Walk HIR blocks and emit ARM64 instructions.
  auto block = builder->first_block();
  synchronize_stack_on_next_instruction_ = false;
  while (block) {
    // Reset FPCR tracking on each block entry (we don't know which
    // predecessor ran, so mode is unknown).
    ForgetFpcrMode();

    // Bind all labels targeting this block.
    auto label = block->label_head;
    while (label) {
      L(GetLabel(label->id));
      ForgetX0Constant();
      label = label->next;
    }

    // Process each instruction in the block.
    const hir::Instr* instr = block->instr_head;
    while (instr) {
      // After a guest call, check for longjmp on the next real instruction.
      // Skip SOURCE_OFFSET because the return address from the call would
      // point past the check, so it would never execute.
      if (synchronize_stack_on_next_instruction_) {
        if (instr->GetOpcodeNum() != hir::OPCODE_SOURCE_OFFSET) {
          synchronize_stack_on_next_instruction_ = false;
          EnsureSynchronizedGuestAndHostStack();
        }
      }
      const hir::Instr* new_tail = instr;
      bool selected = false;
      try {
        selected = SelectSequence(this, instr, &new_tail);
      } catch (const Xbyak_aarch64::Error& e) {
        if (IsNearBranchOutOfRange(e)) {
          throw;  // Emitted again with long-range branches.
        }
        // Uncaught this aborts the process with no context, so name the opcode
        // and the guest function and fail just this compile.
        XELOGE(
            "A64: assembler rejected HIR opcode {} in guest function {:08X}: "
            "{}",
            hir::GetOpcodeName(instr->GetOpcodeInfo()), current_guest_function_,
            e.what());
        return false;
      }
      if (!selected) {
        // No sequence matched — this is expected in Phase 1 before
        // sequences are implemented.
        XELOGE("A64: Unable to process HIR opcode {}",
               hir::GetOpcodeName(instr->GetOpcodeInfo()));
        return false;
      }
      instr = new_tail;
    }

    block = block->next;
  }

  // ========================================================================
  // EPILOG
  // ========================================================================
  L(*epilog_label_);
  epilog_label_ = nullptr;
  // FTrace: log the guest return value (r3) on normal return.
  if (IsTracingFunc()) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(TraceFunctionReturn));
  }
  if (log_guest_call_) {
    mov(x1, static_cast<uint64_t>(current_guest_function_));
    CallNative(reinterpret_cast<void*>(LogGuestCallReturn));
  }
  code_offsets.epilog = getSize();

  // Pop stackpoint before leaving.
  PopStackpoint();

  // Restore host return address and deallocate stack.
  ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
  if (stack_size <= 4095) {
    add(sp, sp, static_cast<uint32_t>(stack_size));
  } else {
    mov(x17, static_cast<uint64_t>(stack_size));
    add(sp, sp, x17, UXTX);
  }
  ret();

  // ========================================================================
  // TAIL CODE
  // ========================================================================
  emitting_tail_ = true;
  for (auto& tail_item : tail_code_) {
    // ARM64 instructions are always 4-byte aligned, so alignment is mostly
    // a no-op unless we want cache-line alignment for hot paths.
    L(tail_item.label);
    try {
      tail_item.func(*this, tail_item.label);
    } catch (const Xbyak_aarch64::Error& e) {
      if (IsNearBranchOutOfRange(e)) {
        throw;  // Emitted again with long-range branches.
      }
      XELOGE("A64: assembler rejected tail code in guest function {:08X}: {}",
             current_guest_function_, e.what());
      return false;
    }
  }
  // 128-bit constants loaded by LDR (literal) above (tail code included).
  vconst_pool_enabled_ = false;
  if (!vconst_pool_.empty()) {
    try {
      align(16);
      static_assert(sizeof(vec128_t) == 16);
      const size_t pool_start_offset = getSize();
      // LDR (literal) uses a signed 19-bit, word-scaled offset. Keep 16 bytes
      // of margin below the positive 1 MiB limit; the pool is after every use,
      // so the first use is the farthest reference to each entry.
      constexpr size_t kSafeLdrLiteralReach = (size_t{1} << 20) - 16;
      for (size_t i = 0; i < vconst_pool_.size(); ++i) {
        const size_t pool_offset = pool_start_offset + i * sizeof(vec128_t);
        const size_t first_use_offset = vconst_pool_[i].first_use_offset;
        if (pool_offset < first_use_offset ||
            pool_offset - first_use_offset > kSafeLdrLiteralReach) {
          vconst_pool_emit_failed_ = true;
          XELOGW("A64: V128 literal pool out of range for guest function "
                 "{:08X}; retrying with inline constants",
                 current_guest_function_);
          return false;
        }
      }
      for (auto& entry : vconst_pool_) {
        L(entry.label);
        for (uint32_t i = 0; i < 4; ++i) {
          dd(entry.value.u32[i]);
        }
      }
    } catch (const Xbyak_aarch64::Error& e) {
      vconst_pool_emit_failed_ = true;
      XELOGW("A64: assembler rejected the literal pool of guest function "
             "{:08X}; retrying with inline constants: {}",
             current_guest_function_, e.what());
      return false;
    }
  }
  code_offsets.tail = getSize();

  // Fill in EmitFunctionInfo metrics.
  assert_zero(code_offsets.prolog);
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {
  assert_true(func_info.code_size.total == getSize());

  void* new_execute_address;
  void* new_write_address;

  if (function) {
    code_cache_->PlaceGuestCode(
        function->address(),
        const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        function, new_execute_address, new_write_address);
    if (cvars::a64_patchable_guest_calls) {
      code_cache_->ResolveGuestCallSites(
          function->address(), static_cast<uint8_t*>(new_execute_address),
          patchable_call_sites_);
    }
  } else {
    code_cache_->PlaceHostCode(
        0, const_cast<void*>(static_cast<const void*>(getCode())), func_info,
        new_execute_address, new_write_address);
  }

  // In xbyak_aarch64, labels are resolved at define time (backpatching),
  // so all relative offsets are already correct. We just need to reset
  // the codegen state for the next function.
  ResetPerFunctionState();

  return new_execute_address;
}

void A64Emitter::ResetPerFunctionState() {
  reset();
  tail_code_.clear();
  pinned_preempt_paths_.clear();
  patchable_call_sites_.clear();
  vconst_pool_enabled_ = false;
  vconst_pool_emit_failed_ = false;
  vconst_pool_.clear();
  fused_skip_instr_ = nullptr;

  // Clean up cached labels.
  epilog_label_ = nullptr;
  for (auto* cached_label : label_cache_) {
    delete cached_label;
  }
  label_cache_.clear();

  // Clean up HIR->xbyak label map. HIR label ids restart at each function, so
  // stale entries would hand the next function this one's labels.
  for (auto& pair : label_map_) {
    delete pair.second;
  }
  label_map_.clear();
}

void A64Emitter::MarkSourceOffset(const hir::Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->code_offset = static_cast<uint32_t>(getSize());
}

bool A64Emitter::LoadV128FromPool(int vreg_idx, const vec128_t& value) {
  if (!vconst_pool_enabled_) {
    return false;
  }
  for (auto& entry : vconst_pool_) {
    if (entry.value.low == value.low && entry.value.high == value.high) {
      const size_t use_offset = getSize();
      if (use_offset < entry.first_use_offset) {
        entry.first_use_offset = use_offset;
      }
      ldr(Xbyak_aarch64::QReg(vreg_idx), entry.label);
      return true;
    }
  }
  // Bounds the linear search; exact literal reach is checked once the final
  // tail-code and pool offsets are known.
  if (vconst_pool_.size() >= 512) {
    return false;
  }
  vconst_pool_.emplace_back();
  auto& entry = vconst_pool_.back();
  entry.value = value;
  entry.first_use_offset = getSize();
  ldr(Xbyak_aarch64::QReg(vreg_idx), entry.label);
  return true;
}

void A64Emitter::DebugBreak() { brk(0xF000); }

void A64Emitter::Trap(uint16_t trap_type) { brk(trap_type); }

void A64Emitter::b(const Xbyak_aarch64::Cond cond,
                   const Xbyak_aarch64::Label& label) {
  if (near_branches_) {
    CodeGenerator::b(cond, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::b(static_cast<Xbyak_aarch64::Cond>(cond ^ 1), skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::WReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (near_branches_) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbz(const Xbyak_aarch64::XReg& rt,
                     const Xbyak_aarch64::Label& label) {
  if (near_branches_) {
    CodeGenerator::cbz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbnz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::WReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (near_branches_) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::cbnz(const Xbyak_aarch64::XReg& rt,
                      const Xbyak_aarch64::Label& label) {
  if (near_branches_) {
    CodeGenerator::cbnz(rt, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::cbz(rt, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (near_bit_branches_) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                     const Xbyak_aarch64::Label& label) {
  if (near_bit_branches_) {
    CodeGenerator::tbz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbnz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (near_bit_branches_) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
                      const Xbyak_aarch64::Label& label) {
  if (near_bit_branches_) {
    CodeGenerator::tbnz(rt, imm, label);
    return;
  }
  Xbyak_aarch64::Label skip;
  CodeGenerator::tbz(rt, imm, skip);
  CodeGenerator::b(label);
  L(skip);
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  XELOGE("A64: Unimplemented HIR instruction: {}",
         hir::GetOpcodeName(i->GetOpcodeInfo()));
  DebugBreak();
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  EmitPinnedContext(false, uint32_t(instr->src3.offset));
  ForgetFpcrMode();
  auto fn = static_cast<A64Function*>(function);

  // Guest trampolines (re-allocated host callbacks) are never bound.
  constexpr uint32_t kPatchableCallMinAddress = 0x80040000;
  if (cvars::a64_patchable_guest_calls &&
      code_cache_->has_indirection_table() &&
      !(instr->flags & hir::CALL_TAIL) &&
      function->address() >= kPatchableCallMinAddress) {
    // BL to a per-site stub doing the regular lookup; the code cache rewrites
    // the BL to the callee itself once it has code. x30 from the BL is the
    // return address either way.
    if (!call_return_constant_valid_ ||
        !X0HoldsConstant(call_return_constant_)) {
      LoadCallReturnAddress(instr);
    }
    call_return_constant_valid_ = false;
    const uint32_t target_address = function->address();
    auto& stub = AddToTail([target_address](A64Emitter& e, Label& lbl) {
      if (cvars::a64_shared_cold_paths) {
        e.mov(e.w16, target_address);
        e.ldr(e.x9, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                   A64BackendContext, cold_call_lookup_thunk))));
        e.br(e.x9);
        return;
      }
      e.EmitIndirectionLookup(target_address);
      e.br(e.x9);
    });
    patchable_call_sites_.emplace_back(target_address,
                                       static_cast<uint32_t>(getSize()));
    bl(stub);
    ++guest_call_sites_;
    synchronize_stack_on_next_instruction_ = true;
    return;
  }

  if (fn->machine_code()) {
    // Direct call — function is already compiled.
    mov(x9, reinterpret_cast<uint64_t>(fn->machine_code()));
    if (!(instr->flags & hir::CALL_TAIL)) {
      // Pass the next call's guest return address in x0.
      LoadCallReturnAddress(instr);
      ++guest_call_sites_;
      blr(x9);
      synchronize_stack_on_next_instruction_ = true;
    } else {
      // Tail call: pass our return address to the callee.
      PopStackpoint();
      ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
      ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
      if (stack_size() <= 4095) {
        add(sp, sp, static_cast<uint32_t>(stack_size()));
      } else {
        mov(x17, static_cast<uint64_t>(stack_size()));
        add(sp, sp, x17, UXTX);
      }
      br(x9);
    }
    return;
  }

  if (code_cache_->has_indirection_table()) {
    EmitIndirectionLookup(function->address());
  } else {
    // No indirection table: resolve at runtime.
    mov(x0, x20);  // context
    mov(x1, static_cast<uint64_t>(function->address()));
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address in x9
  }

  if (instr->flags & hir::CALL_TAIL) {
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    LoadCallReturnAddress(instr);
    ++guest_call_sites_;
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::EmitIndirectionLookup(uint32_t guest_address) {
  // Must leave the guest address in w16 for the resolve thunk to read.
  mov(w16, guest_address);
  if (!code_cache_->encoded_indirection()) {
    // Fast path: table mapped at host VA == guest addr; slot holds raw
    // 32-bit host target.
    ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
  } else {
    EmitEncodedIndirectionLoad();
  }
}

void A64Emitter::EmitEncodedIndirectionLoad() {
  // Encoded path: see A64CodeCache for the entry format. w16 = guest address
  // on entry, x9 = host address on exit; clobbers x5, x7.
  if (cvars::a64_indirection_bases_in_context) {
    // Both bases with one load from the backend context instead of five
    // instructions of immediates, and the rare external form out of line: an
    // indirect guest call runs straight through.
    static_assert(offsetof(A64BackendContext, execute_base) ==
                  offsetof(A64BackendContext, indirection_table_bias) + 8);
    static_assert(offsetof(A64BackendContext, indirection_table_bias) % 8 ==
                      0 &&
                  offsetof(A64BackendContext, indirection_table_bias) <= 504);
    Label& ready = NewCachedLabel();
    ldp(x7, x5,
        ptr(x19, static_cast<int32_t>(
                     offsetof(A64BackendContext, indirection_table_bias))));
    add(x7, x7, w16, UXTW);
    ldr(w9, ptr(x7, static_cast<uint32_t>(0)));
    if (emitting_tail_) {
      // Already out of line (the stub of a patchable call): the tail list is
      // being walked and can't grow.
      Label external_inline;
      tbnz(w9, 31, external_inline);
      add(x9, x5, w9, UXTW);
      b(ready);
      L(external_inline);
      and_(w5, w9, A64CodeCache::kIndirectionExternalIndexMask);
      mov(x7, code_cache_->external_indirection_table_base_address());
      lsl(x5, x5, 3);
      add(x7, x7, x5);
      ldr(x9, ptr(x7, static_cast<uint32_t>(0)));
      L(ready);
      return;
    }
    Label& external = AddToTail([&ready](A64Emitter& e, Label& lbl) {
      if (cvars::a64_shared_cold_paths) {
        // x30 is free here: a call follows, and a tail call reloads it.
        e.ldr(e.x7, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                   A64BackendContext,
                                   cold_external_lookup_thunk))));
        e.blr(e.x7);
        e.b(ready);
        return;
      }
      // External: tagged index into the side table.
      e.and_(e.w5, e.w9, A64CodeCache::kIndirectionExternalIndexMask);
      e.mov(e.x7, e.code_cache_->external_indirection_table_base_address());
      e.lsl(e.x5, e.x5, 3);
      e.add(e.x7, e.x7, e.x5);
      e.ldr(e.x9, ptr(e.x7, static_cast<uint32_t>(0)));
      e.b(ready);
    });
    tbnz(w9, 31, external);
    // Internal: rel32 from code cache base.
    add(x9, x5, w9, UXTW);
    L(ready);
    return;
  }

  Label external_target;
  Label indirection_ready;

  mov(x7, code_cache_->indirection_table_base_bias());
  add(x7, x7, w16, UXTW);
  ldr(w9, ptr(x7, static_cast<uint32_t>(0)));
  tbnz(w9, 31, external_target);

  // Internal: rel32 from code cache base.
  mov(x7, code_cache_->execute_base_address());
  add(x9, x7, w9, UXTW);
  b(indirection_ready);

  // External: tagged index into the side table.
  L(external_target);
  and_(w5, w9, A64CodeCache::kIndirectionExternalIndexMask);
  mov(x7, code_cache_->external_indirection_table_base_address());
  lsl(x5, x5, 3);
  add(x7, x7, x5);
  ldr(x9, ptr(x7, static_cast<uint32_t>(0)));

  L(indirection_ready);
}

void A64Emitter::CallIndirect(const hir::Instr* instr, int reg_index) {
  EmitPinnedContext(false, uint32_t(instr->src3.offset));
  ForgetFpcrMode();
  auto target_w = WReg(reg_index);

  // Check if this is a possible return (e.g., PPC blr).
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    // Compare target guest address with our function's return address.
    ldr(w0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    cmp(target_w, w0);
    b(EQ, epilog_label());
  }

  // Load host code address from indirection table.
  if (code_cache_->has_indirection_table()) {
    // Must leave the guest address in w16 for the resolve thunk to read.
    if (target_w.getIdx() != w16.getIdx()) {
      mov(w16, target_w);
    }
    if (!code_cache_->encoded_indirection()) {
      // Fast path: table mapped at host VA == guest addr; slot holds raw
      // 32-bit host target.
      ldr(w9, ptr(x16, static_cast<uint32_t>(0)));
    } else {
      EmitEncodedIndirectionLoad();
    }
  } else {
    // No indirection table: resolve at runtime.
    mov(w16, target_w);
    mov(x0, x20);  // context
    mov(x1, x16);  // guest address
    mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
    blr(x9);
    mov(x9, x0);  // resolved address
  }

  if (instr->flags & hir::CALL_TAIL) {
    // Tail call: pass our return address to the callee.
    PopStackpoint();
    ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_RET_ADDR)));
    ldr(x30, ptr(sp, static_cast<uint32_t>(StackLayout::HOST_RET_ADDR)));
    if (stack_size() <= 4095) {
      add(sp, sp, static_cast<uint32_t>(stack_size()));
    } else {
      mov(x17, static_cast<uint64_t>(stack_size()));
      add(sp, sp, x17, UXTX);
    }
    br(x9);
  } else {
    // Regular call: pass the next call's return address.
    LoadCallReturnAddress(instr);
    ++guest_call_sites_;
    blr(x9);
    synchronize_stack_on_next_instruction_ = true;
  }
}

// Whether a float or vector value defined before `call` is read after it,
// in this block or a later one (context forwarding may cross blocks). False
// for CALL_EXTERN as built: it ends its block and stops forwarding.
static bool FpValueLiveAcross(const hir::Instr* call) {
  const auto is_fp = [](const hir::Value* v) {
    return v->type == hir::FLOAT32_TYPE || v->type == hir::FLOAT64_TYPE ||
           v->type == hir::VEC128_TYPE;
  };
  std::unordered_set<const hir::Instr*> after;
  for (const hir::Instr* i = call->next; i; i = i->next) after.insert(i);
  for (const hir::Block* b = call->block->next; b; b = b->next) {
    for (const hir::Instr* i = b->instr_head; i; i = i->next) after.insert(i);
  }
  const auto live_after = [&](const hir::Instr* i) {
    if (!i->dest || !is_fp(i->dest)) return false;
    for (const hir::Value::Use* use = i->dest->use_head; use; use = use->next) {
      if (after.count(use->instr)) return true;
    }
    return false;
  };
  for (const hir::Block* b = call->block->prev; b; b = b->prev) {
    for (const hir::Instr* i = b->instr_head; i; i = i->next) {
      if (live_after(i)) return true;
    }
  }
  for (const hir::Instr* i = call->block->instr_head; i != call; i = i->next) {
    if (live_after(i)) return true;
  }
  return false;
}

void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  ForgetFpcrMode();
  // The no-vec thunk skips the q4-q31 save (a64_extern_thunk_no_vec).
  const GuestToHostThunk extern_thunk =
      FpValueLiveAcross(instr) ? backend()->guest_to_host_thunk()
                               : backend()->guest_to_host_thunk_no_vec();
  bool undefined = true;
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0, x2=arg1
      // Thunk rearranges to: x0=context, x1=arg0, x2=arg1, calls target
      mov(x0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      mov(x1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      mov(x2, reinterpret_cast<uint64_t>(builtin_function->arg1()));
      mov(x9, reinterpret_cast<uint64_t>(extern_thunk));
      blr(x9);
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // GuestToHostThunk: x0=target, x1=arg0
      mov(x0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      ldr(x1, ptr(GetContextReg(), static_cast<int32_t>(offsetof(
                                       ppc::PPCContext, kernel_state))));
      mov(x9, reinterpret_cast<uint64_t>(extern_thunk));
      blr(x9);
    }
  }
  if (undefined) {
    // Set arg0 = function pointer, then call UndefinedCallExtern via thunk.
    mov(x1, reinterpret_cast<uint64_t>(function));
    CallNativeSafe(reinterpret_cast<void*>(&UndefinedCallExtern));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNativeSafe(void* fn) {
  ForgetFpcrMode();
  // GuestToHostThunk: x0=target function, x1/x2=args (set by caller).
  // The thunk rearranges: saves x0 in x9, sets x0=context, calls x9.
  mov(x0, reinterpret_cast<uint64_t>(fn));
  mov(x9, reinterpret_cast<uint64_t>(backend()->guest_to_host_thunk()));
  blr(x9);
}

void A64Emitter::CallNativeIntegerLeaf(void* fn) {
  // Integer leaf helpers take/return x0 rather than PPCContext*. They don't
  // pass through GuestToHostThunk, and may clobber every AAPCS volatile GPR.
  // Use reserved space in the guest frame, outside VMX scratch and locals.
  // Keep SP constant through the body for unwind, stackpoint and preemption.
  if (backend()->extended_gpr_allocator()) {
    SaveExtendedGprs(*this, kExtendedGprGuestStashOffset);
  }
  mov(x9, reinterpret_cast<uint64_t>(fn));
  blr(x9);
  if (backend()->extended_gpr_allocator()) {
    RestoreExtendedGprs(*this, kExtendedGprGuestStashOffset);
  }
}

bool A64Emitter::X0HoldsConstant(uint64_t value) const {
  return cvars::a64_reuse_x0_constant && x0_constant_size_ == getSize() &&
         x0_constant_ == value;
}

void A64Emitter::SetReturnAddress(uint64_t value, const hir::Instr* instr) {
  if (!X0HoldsConstant(value)) {
    mov(x0, value);
  }
  const hir::Instr* paired_call =
      cvars::a64_call_return_address_in_register && instr ? PairedCall(instr)
                                                          : nullptr;
  if (paired_call) {
    // The call takes it from x0, or makes it again: nothing reads the slot.
    call_return_paired_call_ = paired_call;
    call_return_paired_value_ = value;
  } else {
    str(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
  }
  NoteX0Constant(value);
  call_return_constant_ = value;
  call_return_constant_valid_ = true;
}

void A64Emitter::LoadCallReturnAddress(const hir::Instr* instr) {
  if (instr && instr == call_return_paired_call_) {
    if (!X0HoldsConstant(call_return_paired_value_)) {
      mov(x0, call_return_paired_value_);
    }
    return;
  }
  ldr(x0, ptr(sp, static_cast<uint32_t>(StackLayout::GUEST_CALL_RET_ADDR)));
}

void A64Emitter::ReloadMembase() {
  // Reload x21 from context->virtual_membase.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
}

bool A64Emitter::ChangeFpcrMode(FPCRMode new_mode, bool already_set) {
  if (fpcr_mode_ == new_mode) {
    return false;
  }
  fpcr_mode_ = new_mode;
  if (!already_set) {
    // Load the pre-computed FPCR value from the backend context.
    // This avoids an expensive MRS + read-modify-write cycle.
    auto bctx = GetBackendCtxReg();
    const uint32_t live_offset = static_cast<uint32_t>(
        offsetof(A64BackendContext, fpcr_live_mode));
    const uint32_t live_mode = new_mode == FPCRMode::Vmx ? 2 : 1;
    Label* already_live = nullptr;
    if (cvars::a64_fpcr_skip_redundant_write) {
      // Flag-free test of the runtime mode: the usual case is a block whose
      // predecessors all left this very mode, and writing FPCR is far more
      // expensive than three plain instructions.
      already_live = &NewCachedLabel();
      ldrb(w0, Xbyak_aarch64::ptr(bctx, live_offset));
      sub(w0, w0, live_mode);
      cbz(w0, *already_live);
    }
    if (new_mode == FPCRMode::Vmx) {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, fpcr_vmx))));
    } else {
      ldr(w0, Xbyak_aarch64::ptr(bctx, static_cast<uint32_t>(offsetof(
                                           A64BackendContext, fpcr_fpu))));
    }
    msr(3, 3, 4, 4, 0, x0);  // msr FPCR, x0
    mov(w0, live_mode);
    strb(w0, Xbyak_aarch64::ptr(bctx, live_offset));
    if (already_live) {
      L(*already_live);
    }
  }
  return true;
}

Label& A64Emitter::AddToTail(TailEmitCallback callback, uint32_t alignment) {
  TailEmitter tail;
  tail.alignment = alignment;
  tail.func = std::move(callback);
  tail_code_.push_back(std::move(tail));
  return tail_code_.back().label;
}

Label& A64Emitter::NewCachedLabel() {
  auto* label = new Label();
  label_cache_.push_back(label);
  return *label;
}

uint32_t A64Emitter::MapReg(const hir::Value* v, const uint32_t* map, int count,
                            const char* set_name) {
  // reg.index is a signed int32 and is -1 while unassigned, so the unsigned
  // compare catches both "never allocated" and "past the end of the set".
  const uint32_t index = static_cast<uint32_t>(v->reg.index);
  if (index >= static_cast<uint32_t>(count)) {
    XELOGE(
        "A64: value v{} (type {}, def opcode {}) has no {} register assignment "
        "(index {} of {}); codegen would emit a bogus register",
        v->ordinal, static_cast<uint32_t>(v->type),
        v->def ? hir::GetOpcodeName(v->def->GetOpcodeInfo()) : "<none>",
        set_name, index, count);
    assert_always("register allocation missed a value");
    // Clamp so the assembler still produces a decodable instruction.
    return map[0];
  }
  return map[index];
}

void A64Emitter::EmitPinnedContext(bool load, uint32_t mask) {
  // Emit in context order so adjacent slots can share an LDP/STP, regardless
  // of which physical pin the access-frequency ranking assigned them.
  while (mask) {
    size_t first = pinned_context_slots_.size();
    for (size_t n = 0; n < pinned_context_slots_.size(); ++n) {
      if ((mask & (uint32_t{1} << n)) &&
          (first == pinned_context_slots_.size() ||
           pinned_context_slots_[n].offset <
               pinned_context_slots_[first].offset)) {
        first = n;
      }
    }
    const auto& s = pinned_context_slots_[first];
    mask &= ~(uint32_t{1} << first);
    const uint32_t size = uint32_t(hir::GetTypeSize(s.type));
    size_t second = pinned_context_slots_.size();
    if (s.offset % size == 0 && s.offset / size <= 63) {
      for (size_t n = 0; n < pinned_context_slots_.size(); ++n) {
        if ((mask & (uint32_t{1} << n)) &&
            pinned_context_slots_[n].type == s.type &&
            pinned_context_slots_[n].offset == s.offset + size) {
          second = n;
          mask &= ~(uint32_t{1} << n);
          break;
        }
      }
    }
    const auto address = ptr(x20, s.offset);
    auto emit = [&](auto reg, auto other) {
      if (second != pinned_context_slots_.size()) {
        if (load) {
          ldp(reg, other, ptr(x20, static_cast<int32_t>(s.offset)));
        } else {
          stp(reg, other, ptr(x20, static_cast<int32_t>(s.offset)));
        }
      } else {
        if (load) {
          ldr(reg, address);
        } else {
          str(reg, address);
        }
      }
    };
    const uint32_t next_index = second == pinned_context_slots_.size()
                                    ? s.index
                                    : pinned_context_slots_[second].index;
    if (s.type == hir::INT64_TYPE) {
      emit(XReg(PinnedGprReg(s.index)), XReg(PinnedGprReg(next_index)));
    } else if (s.type == hir::FLOAT64_TYPE) {
      emit(DReg(PinnedVecReg(s.index)), DReg(PinnedVecReg(next_index)));
    } else {
      emit(QReg(PinnedVecReg(s.index)), QReg(PinnedVecReg(next_index)));
    }
  }
}

void A64Emitter::EmitPreemptCheck(uint32_t guest_address, uint32_t dirty_mask,
                                  uint32_t live_mask) {
  // Only safe at a block head, where the per-block register allocator leaves no
  // guest value live and ForgetFpcrMode has already run, so the unannounced
  // guest->host call cannot lose a register or desync the mode tracking.
  // Tests the preempt flag other threads raise. The cold path clears it, a
  // deferred yield re-sets it.
  Label& after = NewCachedLabel();
  // ldrb/strb unsigned-offset encoding caps at 4095.
  static_assert(offsetof(ppc::PPCContext, preempt_requested) < 4096);
  const uint32_t flag_offset =
      static_cast<uint32_t>(offsetof(ppc::PPCContext, preempt_requested));
  Label* shared_sync = nullptr;
  const uint64_t key = (uint64_t(dirty_mask) << 32) | live_mask;
  auto found = pinned_preempt_paths_.find(key);
  if (cvars::a64_shared_cold_paths && key &&
      found != pinned_preempt_paths_.end()) {
    const uint32_t sites = found->second.first;
    // Pairing can at most halve the synchronization instructions. Share only
    // when even that lower bound pays for the extra cold call and LR save.
    const uint32_t minimum_sync = (std::popcount(dirty_mask) + 1) / 2 +
                                  (std::popcount(live_mask) + 1) / 2;
    if ((sites - 1) * minimum_sync + sites > 5) {
      if (!found->second.second) {
        // TailEmitter entries move as the tail vector grows. Keep the branch
        // target in the label cache, whose labels have stable addresses.
        found->second.second = &NewCachedLabel();
        AddToTail([dirty_mask, live_mask, target = found->second.second](
                      A64Emitter& e, Label&) {
          e.L(*target);
          // Each guest frame owns this padding word, including callbacks
          // reentering guest code. HOST_RET_ADDR remains the frame's LR.
          e.str(e.x30, ptr(e.sp, uint32_t(StackLayout::GUEST_RESERVED)));
          e.EmitPinnedContext(false, dirty_mask);
          e.ldr(e.x9, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                     A64BackendContext, cold_preempt_thunk))));
          e.blr(e.x9);
          e.EmitPinnedContext(true, live_mask);
          e.ldr(e.x30, ptr(e.sp, uint32_t(StackLayout::GUEST_RESERVED)));
          e.ret();
        });
      }
      shared_sync = found->second.second;
    }
  }
  const bool cold_pc = cvars::log_safepoint_pc &&
                       cvars::a64_safepoint_pc_cold_only && guest_address;
  Label& do_yield = AddToTail([&after, flag_offset, dirty_mask, live_mask,
                               shared_sync, cold_pc,
                               guest_address](A64Emitter& e, Label&) {
    if (cold_pc) {
      // Per-site tail code: the shared paths below never see the address.
      e.mov(e.w9, guest_address);
      e.str(e.w9, ptr(e.x20, static_cast<uint32_t>(offsetof(
                                 ppc::PPCContext, last_safepoint_pc))));
    }
    if (shared_sync) {
      e.bl(*shared_sync);
      e.b(after);
      return;
    }
    // Only the taken cold path materializes the cache. Yield handlers see
    // current guest registers, and their changes are reloaded before resuming.
    e.EmitPinnedContext(false, dirty_mask);
    if (cvars::a64_shared_cold_paths) {
      e.ldr(e.x9, ptr(e.x19, static_cast<uint32_t>(offsetof(
                                 A64BackendContext, cold_preempt_thunk))));
      e.blr(e.x9);
      e.EmitPinnedContext(true, live_mask);
      e.b(after);
      return;
    }
    e.strb(e.wzr, ptr(e.x20, flag_offset));
    // Null until the scheduler starts, and a stale flag can reach here after
    // it shuts down, so check before calling.
    e.mov(e.x0,
          reinterpret_cast<uint64_t>(&xe::cpu::backend::preempt_yield_handler));
    e.ldr(e.x0, ptr(e.x0));
    e.cbz(e.x0, after);
    e.mov(e.x9, reinterpret_cast<uint64_t>(e.backend()->guest_to_host_thunk()));
    e.blr(e.x9);
    e.EmitPinnedContext(true, live_mask);
    e.b(after);
  });
  if (cvars::log_safepoint_pc && guest_address && !cold_pc) {
    // Diagnostic only: costs a materialize + store on every loop back-edge, so
    // it stays off unless a wedge is being chased.
    static_assert(offsetof(ppc::PPCContext, last_safepoint_pc) < 16384);
    mov(w9, guest_address);
    str(w9, ptr(x20, static_cast<uint32_t>(
                         offsetof(ppc::PPCContext, last_safepoint_pc))));
  }
  ldrb(w8, ptr(x20, flag_offset));
  cbnz(w8, do_yield);
  L(after);
}

Label& A64Emitter::GetLabel(uint32_t label_id) {
  auto it = label_map_.find(label_id);
  if (it != label_map_.end()) {
    return *it->second;
  }
  auto* label = new Label();
  label_map_[label_id] = label;
  return *label;
}

void A64Emitter::HandleStackpointOverflowError(ppc::PPCContext* context) {
  if (debugging::IsDebuggerAttached()) {
    debugging::Break();
  }
  xe::FatalError(
      "Overflowed stackpoints! Please report this error for this title to "
      "Xenia developers.");
}

void A64Emitter::PushStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization || skip_stackpoint_) {
    return;
  }
  // x8 = &stackpoints[depth], w9 = current depth.
  ldr(x8, ptr(x19,
              static_cast<uint32_t>(offsetof(A64BackendContext, stackpoints))));
  ldr(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
  static_assert(sizeof(A64BackendStackpoint) == 16);
  add(x8, x8, w9, UXTW, 4);

  // Host SP, then guest r1 and LR (adjacent 32-bit fields) as one pair.
  static_assert(offsetof(A64BackendStackpoint, guest_return_address_) ==
                offsetof(A64BackendStackpoint, guest_stack_) + 4);
  mov(x6, sp);
  str(x6, ptr(x8, static_cast<uint32_t>(
                       offsetof(A64BackendStackpoint, host_stack_))));
  ldr(w6, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));
  ldr(w7, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, lr))));
  stp(w6, w7, ptr(x8, static_cast<int32_t>(
                          offsetof(A64BackendStackpoint, guest_stack_))));

  // Increment depth.
  add(w9, w9, 1);
  str(w9, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));

  // Check for overflow. b(cond) takes the far-safe form by itself when the
  // function is emitted with long-range branches.
  auto& overflow_label = AddToTail([](A64Emitter& e, Label& lbl) {
    if (cvars::a64_shared_cold_paths) {
      e.ldr(e.x9,
            ptr(e.x19, static_cast<uint32_t>(offsetof(
                           A64BackendContext, cold_stackpoint_overflow_thunk))));
      e.br(e.x9);
      return;
    }
    e.CallNativeSafe(
        reinterpret_cast<void*>(A64Emitter::HandleStackpointOverflowError));
  });
  const uint32_t max_stackpoints =
      static_cast<uint32_t>(cvars::a64_max_stackpoints);
  if (max_stackpoints && !(max_stackpoints & (max_stackpoints - 1))) {
    // A power of two (the default): the depth grows by one at a time, so it
    // reaches the limit exactly when that one bit turns on.
    uint32_t bit = 0;
    while ((uint32_t(1) << bit) != max_stackpoints) {
      ++bit;
    }
    tbnz(w9, bit, overflow_label);
  } else {
    mov(w6, max_stackpoints);
    cmp(w9, w6);
    b(GE, overflow_label);
  }
}

void A64Emitter::PopStackpoint() {
  if (!cvars::a64_enable_host_guest_stack_synchronization || skip_stackpoint_) {
    return;
  }
  // Decrement current_stackpoint_depth.
  ldr(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
  sub(w8, w8, 1);
  str(w8, ptr(x19, static_cast<uint32_t>(
                       offsetof(A64BackendContext, current_stackpoint_depth))));
}

void A64Emitter::EnsureSynchronizedGuestAndHostStack() {
  if (!cvars::a64_enable_host_guest_stack_synchronization ||
      cvars::a64_longjmp_sync_at_resolve) {
    // (With a64_longjmp_sync_at_resolve the resolver goes through the helper
    // before it gets here.)
    return;
  }
  // ResolveFunction marks this when it returns a return-site address inside an
  // existing frame. The marker lives in backend context because native SP can
  // still point at a skipped frame here.
  auto& return_from_sync = NewCachedLabel();

  ldr(w16, ptr(x19, static_cast<uint32_t>(offsetof(
                        A64BackendContext, pending_stackpoint_sync_depth))));
  cbz(w16, return_from_sync);

  auto& sync_label = AddToTail([](A64Emitter& e, Label& lbl) {
    // x8 was set up in the body to point at return_from_sync; do that there
    // instead of here because adr's ±1 MiB range can't span body+tail in
    // large functions.
    //   x8 = return address (where to resume after fixup)
    e.mov(e.x6, reinterpret_cast<uint64_t>(
                     e.backend()->synchronize_guest_and_host_stack_helper()));
    ++e.helper_branch_sites_;
    e.br(e.x6);
  });
  adr(x8, return_from_sync);
  b(sync_label);

  L(return_from_sync);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
