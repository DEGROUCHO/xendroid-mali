/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_HIR_BUILDER_H_
#define XENIA_CPU_PPC_PPC_HIR_BUILDER_H_

#include "xenia/base/string_buffer.h"
#include "xenia/cpu/function.h"
#include <unordered_map>

#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace ppc {

struct PPCBuiltins;
class PPCFrontend;

class PPCHIRBuilder : public hir::HIRBuilder {
  using Instr = xe::cpu::hir::Instr;
  using Label = xe::cpu::hir::Label;
  using Value = xe::cpu::hir::Value;

 public:
  explicit PPCHIRBuilder(PPCFrontend* frontend);
  ~PPCHIRBuilder() override;

  PPCBuiltins* builtins() const;
  bool SupportsWordRotateMask() const;

 private:
  // The FPSCR store of the last UpdateFPSCR in this function, if any.
  hir::Instr* last_fpscr_clear_ = nullptr;
  // Store-conditionals statically paired with a load-and-reserve, by guest
  // address (see AnalyzeReservedPair).
  std::unordered_map<uint32_t, uint8_t> reserved_pair_stores_;

 public:
  enum ReservedPairKind : uint8_t {
    kReservedPairNone = 0,
    // Becomes a compare-and-swap against the word the paired load read.
    kReservedPairCas = 1,
    // Writes back the word the paired load read: nothing to do.
    kReservedPairRelease = 2,
  };
  // Called at a lwarx/ldarx: true when every store-conditional it can reach
  // was paired, so the load needs no reservation.
  bool AnalyzeReservedPair(uint32_t address, uint32_t code);
  ReservedPairKind reserved_pair_kind(uint32_t address) const {
    auto it = reserved_pair_stores_.find(address);
    return it == reserved_pair_stores_.end() ? kReservedPairNone
                                             : ReservedPairKind(it->second);
  }

 private:

 public:

  void Reset() override;

  enum EmitFlags {
    // Emit comment nodes.
    EMIT_DEBUG_COMMENTS = 1 << 0,
  };
  bool Emit(GuestFunction* function, uint32_t flags);

  GuestFunction* function() const { return function_; }
  Function* LookupFunction(uint32_t address);

  // Expands a call to a small leaf function inline. Returns false (having
  // emitted nothing) when the target does not qualify, in which case the
  // caller emits a normal call.
  bool TryInlineLeafCall(uint32_t target_address, uint64_t cia);
  Label* LookupLabel(uint32_t address);
  // While a leaf with internal forward branches is being expanded inline
  // (inline_leaf_branches): the label its returns (blr, conditional bclr)
  // jump to, right after the inlined body. Null otherwise.
  Label* inline_exit_label() const { return inline_exit_; }

  Value* LoadLR();
  void StoreLR(Value* value);
  Value* LoadCTR();
  void StoreCTR(Value* value);
  Value* LoadCR();
  Value* LoadCR(uint32_t n);
  Value* LoadCRField(uint32_t n, uint32_t bit);
  void StoreCR(Value* value);
  void StoreCR(uint32_t n, Value* value);
  void StoreCRField(uint32_t n, uint32_t bit, Value* value);
  void UpdateCR(uint32_t n, Value* lhs, bool is_signed = true);
  void UpdateCR(uint32_t n, Value* lhs, Value* rhs, bool is_signed = true);
  void UpdateCR6(Value* src_value);
  Value* LoadFPSCR();
  void StoreFPSCR(Value* value);
  void UpdateFPSCR(Value* result, bool update_cr1);
  void CopyFPSCRToCR1();
  Value* LoadXER();
  void StoreXER(Value* value);
  // void UpdateXERWithOverflow();
  // void UpdateXERWithOverflowAndCarry();
  // void StoreOV(Value* value);
  Value* LoadCA();
  void StoreCA(Value* value);
  Value* LoadSAT();
  void StoreSAT(Value* value);

  Value* LoadGPR(uint32_t reg);
  void StoreGPR(uint32_t reg, Value* value);
  Value* LoadFPR(uint32_t reg);
  void StoreFPR(uint32_t reg, Value* value);
  Value* LoadVR(uint32_t reg);
  void StoreVR(uint32_t reg, Value* value);

  // calls original impl in hirbuilder, but also records the is_return_site bit
  // into flags in the guestmodule
  void SetReturnAddress(Value* value);

 private:
  void MaybeBreakOnInstruction(uint32_t address);
  void AnnotateLabel(uint32_t address, Label* label);

  PPCFrontend* frontend_;

  // Reset whenever needed:
  StringBuffer comment_buffer_;

  // Reset each Emit:
  bool with_debug_info_;
  GuestFunction* function_;
  uint64_t start_address_;
  uint64_t instr_count_;
  Instr** instr_offset_list_;
  Label** label_list_;

  // Set only while TryInlineLeafCall expands a leaf with internal branches:
  // its branch targets resolve through inline_labels_ ([inline_start_,
  // inline_end_), one slot per instruction) instead of the enclosing
  // function's tables, and its returns branch to inline_exit_.
  uint32_t inline_start_ = 0;
  uint32_t inline_end_ = 0;
  Label** inline_labels_ = nullptr;
  Label* inline_exit_ = nullptr;

  // Reset each instruction. Sized for what a single PowerPC instruction can
  // write, so anything emitting a longer run of register stores from one
  // instruction (an inlined helper, say) must not be allowed to run the
  // counter past the end - see the guard in the Store*R helpers.
  static constexpr uint32_t kMaxTraceDests = 4;
  struct {
    uint32_t dest_count;
    struct {
      uint8_t reg;
      Value* value;
    } dests[kMaxTraceDests];
  } trace_info_;
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_HIR_BUILDER_H_
