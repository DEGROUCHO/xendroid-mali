/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_SINGLE_PRECISION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_SINGLE_PRECISION_PASS_H_

#include <unordered_map>
#include <unordered_set>

#include "xenia/cpu/compiler/compiler_pass.h"
#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Computes single-precision guest arithmetic in single precision
// (a64_single_precision_narrowing).
//
// Guest floating-point registers hold doubles, so the frontend translates
// `fmuls` as a double multiply rounded to single and widened back: three
// instructions in a row on the dependency chain of every single-precision
// operation, where the guest had one. But the operands are nearly always
// singles themselves - loaded by lfs or produced by another single-precision
// instruction - and then the same result comes from one single-precision
// instruction: for + - * / and sqrt, rounding the double result to single
// gives exactly the single-precision result (the double has more than twice
// the bits), and for the fused multiply-adds the single-precision instruction
// is what the guest does (one rounding), which the double detour only
// approximates.
//
// The pass tracks, for double values that are provably singles, a "twin"
// holding the same number as a FLOAT32: the bits lfs loaded, the result of a
// narrowed operation, the middle of a round-to-single. Operations whose
// operands all have twins are computed on the twins; stfs stores the twin's
// bits. The double is still produced for the context store, off the chain.
// Operands of unknown origin (arguments, values from other paths) are left to
// the double-precision path: nothing is assumed about them.
class SinglePrecisionNarrowingPass : public CompilerPass {
 public:
  SinglePrecisionNarrowingPass();
  ~SinglePrecisionNarrowingPass() override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  hir::Value* TwinOf(hir::HIRBuilder* builder, hir::Value* value);
  // Moves the instruction the builder just appended (if it appended one since
  // |before_tail|) to right before / right after |anchor|.
  static void PlaceBefore(hir::HIRBuilder* builder, hir::Instr* before_tail,
                          hir::Instr* anchor);
  static void PlaceAfter(hir::HIRBuilder* builder, hir::Instr* before_tail,
                         hir::Instr* anchor);

  // a64_single_precision_across_blocks: which guest floating-point registers
  // are known to hold singles at every load_context of one (a forward
  // dataflow over the function's blocks), so that values coming from another
  // block or around a loop have twins too.
  void FindSingleContextLoads(hir::HIRBuilder* builder);
  bool IsKnownSingle(const hir::Value* value, int depth = 0) const;
  // load_context results that are singles.
  std::unordered_set<const hir::Value*> single_loads_;
  // Twins made by narrowing such a load: the same number, but a signaling
  // NaN among them was quieted on the way, so they stand in for the double
  // as operands only (not for its bits).
  std::unordered_set<const hir::Value*> narrowed_twins_;

  std::unordered_map<hir::Value*, hir::Value*> twins_;
  // Twins that are bits as loaded and may therefore be signaling NaNs: fine
  // as operands and for storing back, but a rounding of the double (frsp)
  // quiets them, so it can't be replaced by the twin.
  std::unordered_map<hir::Value*, bool> may_signal_;
  bool MaySignal(hir::Value* twin) const {
    auto found = may_signal_.find(twin);
    return found != may_signal_.end() && found->second;
  }
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_SINGLE_PRECISION_PASS_H_
