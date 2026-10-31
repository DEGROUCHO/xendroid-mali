/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_LOOP_REGISTER_PINNING_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_LOOP_REGISTER_PINNING_PASS_H_

#include "xenia/cpu/backend/machine_info.h"
#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Assigns frequently accessed architectural context slots stable host registers
// across the function CFG. Observations materialize them; preempt checks do so
// only on their cold path. Disabled for savestate and debugger execution.
class LoopRegisterPinningPass : public CompilerPass {
 public:
  explicit LoopRegisterPinningPass(const backend::MachineInfo* machine_info);
  ~LoopRegisterPinningPass() override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  // Pinned registers the backend offers per function, per class.
  uint32_t available_int_ = 0;
  uint32_t available_vec_ = 0;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_LOOP_REGISTER_PINNING_PASS_H_
