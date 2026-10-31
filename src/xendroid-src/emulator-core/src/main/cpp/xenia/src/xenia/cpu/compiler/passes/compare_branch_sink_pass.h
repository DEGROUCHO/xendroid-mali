/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_COMPARE_BRANCH_SINK_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_COMPARE_BRANCH_SINK_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Moves a compare whose only use is a conditional branch later in the same
// block down to right before that branch (a64_sink_compare_to_branch).
//
// Guest code compares (cmpwi, the count test of a loop) and branches some
// instructions later. Translated in that order, the compare's outcome has to
// be turned into a value and kept in a register until the branch tests it
// again. Next to its branch, the backend emits the two as one compare and
// one conditional branch - or a single compare-and-branch against zero.
// Values in the HIR are assigned once, so computing the compare later gives
// the same result.
class CompareBranchSinkPass : public CompilerPass {
 public:
  CompareBranchSinkPass();
  ~CompareBranchSinkPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_COMPARE_BRANCH_SINK_PASS_H_
