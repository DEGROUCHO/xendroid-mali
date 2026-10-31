/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
#define XENIA_CPU_BACKEND_A64_A64_EMITTER_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "xenia/base/arena.h"
#include "xenia/cpu/backend/code_cache_base.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_trace_data.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#include "xbyak_aarch64.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
using namespace arm64;
class A64Backend;
class A64CodeCache;

enum class FPCRMode : uint32_t { Unknown, Fpu, Vmx };

// Unfortunately due to the design of xbyak we have to pass this to the ctor.
class XbyakA64Allocator : public Xbyak_aarch64::Allocator {
 public:
  virtual bool useProtect() const { return false; }
};

class A64Emitter;
using TailEmitCallback =
    std::function<void(A64Emitter& e, Xbyak_aarch64::Label& lbl)>;
struct TailEmitter {
  Xbyak_aarch64::Label label;
  uint32_t alignment;
  TailEmitCallback func;
};

class A64Emitter : public Xbyak_aarch64::CodeGenerator {
 public:
  A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator);
  virtual ~A64Emitter();

  Processor* processor() const { return processor_; }
  A64Backend* backend() const { return backend_; }

  bool Emit(GuestFunction* function, hir::HIRBuilder* builder,
            uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
            void** out_code_address, size_t* out_code_size,
            std::vector<SourceMapEntry>* out_source_map);

 public:
  // Reserved: sp, x19 (backend context), x20 (context), x21 (membase)
  // Scratch: x0-x9, x16-x18, v0-v3.
  // SSA GPRs: x22-x28, optionally x10-x15 (preserved at native calls).
  static constexpr int GPR_COUNT = 13;
  static constexpr int BASE_GPR_COUNT = 7;
  // Available VEC regs: v4-v15, v16-v31
  static constexpr int VEC_COUNT = 28;
  // Pinned values (OPCODE_LOAD_PINNED): number n lives in the register with
  // allocator index PINNED_*_TOP - 1 - n. For the integers these are the
  // callee-saved x28, x27, x26, in both sizes of the allocator set.
  static constexpr int PINNED_GPR_TOP = BASE_GPR_COUNT;
  static constexpr int PINNED_VEC_TOP = VEC_COUNT;
  static constexpr int MAX_PINNED_GPR = 3;
  static constexpr int MAX_PINNED_VEC = 3;
  static uint32_t PinnedGprReg(uint32_t n) {
    return gpr_reg_map_[PINNED_GPR_TOP - 1 - n];
  }
  static uint32_t PinnedVecReg(uint32_t n) {
    return vec_reg_map_[PINNED_VEC_TOP - 1 - n];
  }
  static constexpr size_t kStashOffset = 32;

  // A value that reached codegen without a register assignment indexes the maps
  // out of bounds, which otherwise surfaces far away as an unencodable-register
  // exception. Report the culprit here, where it is still in hand.
  static uint32_t MapReg(const hir::Value* v, const uint32_t* map, int count,
                         const char* set_name);

  static void SetupReg(const hir::Value* v, Xbyak_aarch64::WReg& r) {
    r = Xbyak_aarch64::WReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::XReg& r) {
    r = Xbyak_aarch64::XReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::SReg& r) {
    r = Xbyak_aarch64::SReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::DReg& r) {
    r = Xbyak_aarch64::DReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::QReg& r) {
    r = Xbyak_aarch64::QReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::VReg& r) {
    r = Xbyak_aarch64::VReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }
  size_t guest_call_sites() const { return guest_call_sites_; }
  size_t helper_branch_sites() const { return helper_branch_sites_; }

  void MarkSourceOffset(const hir::Instr* i);

  void DebugBreak();
  void Trap(uint16_t trap_type = 0);
  void UnimplementedInstr(const hir::Instr* i);

  void Call(const hir::Instr* instr, GuestFunction* function);
  void CallIndirect(const hir::Instr* instr, int reg_index);
  void CallExtern(const hir::Instr* instr, const Function* function);
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  void CallNativeIntegerLeaf(void* fn);
  void SetReturnAddress(uint64_t value, const hir::Instr* instr = nullptr);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  static void HandleStackpointOverflowError(ppc::PPCContext* context);

  void ForgetFpcrMode() {
    if (fpcr_mode_ == FPCRMode::Vmx) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Unknown;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

  Xbyak_aarch64::Label& AddToTail(TailEmitCallback callback,
                                  uint32_t alignment = 0);
  Xbyak_aarch64::Label& NewCachedLabel();

  // Emits a cooperative-scheduler preemption safepoint: yields the fiber once
  // the context's preempt_requested flag is raised. Only valid at a block head.
  // guest_address is stamped into the context for wedge diagnosis when
  // log_safepoint_pc is on; 0 means unknown.
  // Context-cache masks use the slot order from HIRBuilder.
  void EmitPinnedContext(bool load, uint32_t mask);
  void EmitPreemptCheck(uint32_t guest_address = 0, uint32_t dirty_mask = 0,
                        uint32_t live_mask = 0);

  // ARM64 conditional branches (cbz/cbnz: ±1 MiB, tbz/tbnz: ±32 KiB,
  // b.cond: ±1 MiB) can fall short of their target in large guest functions.
  // These shadows emit the safe pattern `<inverse> skip; b target; skip:`,
  // routing the long branch through unconditional b (±128 MiB). The
  // int64_t-immediate overloads remain available via the using-declarations
  // for hand-tuned thunks that pass literal byte offsets.
  using Xbyak_aarch64::CodeGenerator::b;
  using Xbyak_aarch64::CodeGenerator::cbnz;
  using Xbyak_aarch64::CodeGenerator::cbz;
  using Xbyak_aarch64::CodeGenerator::tbnz;
  using Xbyak_aarch64::CodeGenerator::tbz;
  void b(const Xbyak_aarch64::Cond cond, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);

  // Get or create a xbyak_aarch64 label for a HIR label ID.
  Xbyak_aarch64::Label& GetLabel(uint32_t label_id);

  XexModule* GuestModule() { return guest_module_; }

 protected:
  void* Emplace(const EmitFunctionInfo& func_info,
                GuestFunction* function = nullptr);
  // Drops the code buffer, tail entries and both label pools. Both the success
  // path and a failed compile must run it, or stale labels carry over.
  void ResetPerFunctionState();
  // Gives every local of the function its stack offset (turning it into a
  // constant) and returns the frame size - once per function: another
  // emission attempt (a64_near_branches, the vconst pool fallback) must reuse
  // the layout, as the locals are constants of another type afterwards.
  size_t LayOutLocals(hir::HIRBuilder* builder);
  bool Emit(hir::HIRBuilder* builder, size_t stack_size,
            EmitFunctionInfo& func_info);

 public:
  // a64_vconst_pool: loads a 128-bit constant with one LDR (literal) from a
  // pool emitted after the function's tail code. False outside function
  // emission or with the pool disabled - the caller materializes it instead.
  bool LoadV128FromPool(int vreg_idx, const vec128_t& value);

  // a64_fuse_zero_compare_branch: a branch instruction already emitted as part
  // of the compare before it, to be skipped by SelectSequence.
  const hir::Instr* fused_skip_instr_ = nullptr;
  // Armed only for the next adjacent store; cache its allocated source so
  // later cvar changes cannot leave the skipped truncate result undefined.
  // a64_reuse_x0_constant: the 64-bit constant the instruction ending at
  // x0_constant_size_ left in x0. Only valid while nothing was emitted since
  // and within one block (reset where block labels are bound), so a guest
  // call's return address is materialized once for the return-address slot,
  // the LR store and the call itself.
  // a64_leaf_skip_stackpoint: this function makes no guest or host call at
  // all (its own `blr` return aside), so no frame is ever resumed inside it
  // and it needs no longjmp stackpoint.
  bool skip_stackpoint_ = false;
  // a64_fuse_compare_branch: the conditional branch a compare already set
  // the flags for, and the condition it is taken on.
  // Last instruction of a run already emitted as a whole by its first one.
  const hir::Instr* fused_skip_through_ = nullptr;
  const hir::Instr* fused_flag_branch_instr_ = nullptr;
  int fused_flag_branch_cond_ = 0;
  uint64_t x0_constant_ = 0;
  size_t x0_constant_size_ = ~size_t(0);
  uint64_t call_return_constant_ = 0;
  bool call_return_constant_valid_ = false;
  // a64_call_return_address_in_register: the call the last
  // SET_RETURN_ADDRESS was emitted for (the next one in its block), which
  // takes the address as a constant instead of from the frame.
  const hir::Instr* call_return_paired_call_ = nullptr;
  uint64_t call_return_paired_value_ = 0;
  // Every call of the function is paired that way: the frame slot is never
  // read.
  bool call_return_slot_unused_ = false;
  // x0 = the guest return address for the call |instr|.
  void LoadCallReturnAddress(const hir::Instr* instr);
  void NoteX0Constant(uint64_t value) {
    x0_constant_ = value;
    x0_constant_size_ = getSize();
  }
  bool X0HoldsConstant(uint64_t value) const;
  void ForgetX0Constant() {
    x0_constant_size_ = ~size_t(0);
    call_return_constant_valid_ = false;
  }
  const hir::Instr* fused_truncate_store_instr_ = nullptr;
  int fused_truncate_store_src_reg_ = -1;

 protected:
  bool vconst_pool_enabled_ = false;
  struct V128ConstantPoolEntry {
    vec128_t value;
    Xbyak_aarch64::Label label;
    // The pool is placed after all function code, so the first use is the
    // farthest literal load from its entry.
    size_t first_use_offset = 0;
  };
  // An out-of-range pool is discarded and the function is emitted again with
  // inline constant materialization.
  bool vconst_pool_force_disabled_ = false;
  bool vconst_pool_emit_failed_ = false;
  std::deque<V128ConstantPoolEntry> vconst_pool_;

  Processor* processor_ = nullptr;
  A64Backend* backend_ = nullptr;
  A64CodeCache* code_cache_ = nullptr;
  XbyakA64Allocator* allocator_ = nullptr;
  XexModule* guest_module_ = nullptr;
  uint64_t feature_flags_ = 0;
  uint32_t current_guest_function_ = 0;
  bool log_guest_call_ = false;

  Xbyak_aarch64::Label* epilog_label_ = nullptr;

  hir::Instr* current_instr_ = nullptr;

  FunctionDebugInfo* debug_info_ = nullptr;
  uint32_t debug_info_flags_ = 0;
  FunctionTraceData* trace_data_ = nullptr;
  Arena source_map_arena_;

  size_t stack_size_ = 0;
  size_t guest_stack_base_size_ = 0;

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  std::vector<hir::HIRBuilder::PinnedContextSlot> pinned_context_slots_;
  // (Dirty mask, live mask) -> number of sites and optional shared tail.
  std::unordered_map<uint64_t, std::pair<uint32_t, Xbyak_aarch64::Label*>>
      pinned_preempt_paths_;

  std::vector<TailEmitter> tail_code_;
  std::vector<Xbyak_aarch64::Label*> label_cache_;

  // Map from HIR label IDs to xbyak_aarch64 Labels.
  std::unordered_map<uint32_t, Xbyak_aarch64::Label*> label_map_;

  size_t guest_call_sites_ = 0;
  size_t helper_branch_sites_ = 0;
  // a64_patchable_guest_calls: (callee guest address, offset of its BL).
  std::vector<std::pair<uint32_t, uint32_t>> patchable_call_sites_;
  // Loads the host code of a guest address through the indirection table into
  // x9, leaving the guest address in w16 (clobbers x5, x7).
  void EmitIndirectionLookup(uint32_t guest_address);
  // The encoded-table part of an indirect call: w16 = guest address in, x9 =
  // host address out (clobbers x5, x7).
  void EmitEncodedIndirectionLoad();
  FPCRMode fpcr_mode_ = FPCRMode::Unknown;
  bool synchronize_stack_on_next_instruction_ = false;
  // a64_near_branches: set per attempt while a guest function is emitted (the
  // helper thunks keep the long-range form), and whether an attempt failed
  // because a direct branch didn't reach.
  bool near_branches_ = false;
  // The tail code list is being emitted: nothing may be added to it.
  bool emitting_tail_ = false;
  // a64_near_bit_branches: tbz/tbnz direct too in this attempt.
  bool near_bit_branches_ = false;
  bool near_branch_out_of_range_ = false;
  bool IsNearBranchOutOfRange(const Xbyak_aarch64::Error& e) {
    if (near_branches_ && int(e) == Xbyak_aarch64::ERR_LABEL_IS_TOO_FAR) {
      near_branch_out_of_range_ = true;
      return true;
    }
    return false;
  }
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
