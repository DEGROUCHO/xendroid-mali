/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/compare_branch_sink_pass.h"

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"

DEFINE_bool(a64_sink_compare_to_branch, true,
            "Compute a compare that only a later conditional branch of the "
            "same block uses right before that branch, where the backend "
            "emits the pair as a compare and a branch instead of keeping the "
            "outcome in a register in between.",
            "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

CompareBranchSinkPass::CompareBranchSinkPass() : CompilerPass() {}

CompareBranchSinkPass::~CompareBranchSinkPass() = default;

namespace {

bool HasSingleUse(const Value* value) {
  return value->use_head && !value->use_head->next;
}

bool IsCompare(const Instr* instr) {
  const auto opcode = instr->GetOpcodeNum();
  return opcode >= OPCODE_COMPARE_EQ && opcode <= OPCODE_COMPARE_UGE;
}

// Execution may leave and come back with registers changed between a value
// and its use: a value moved across one of these would have to live through
// it.
bool IsCallLike(const Instr* instr) {
  const OpcodeInfo* opcode = instr->opcode;
  return opcode == &OPCODE_CALL_info || opcode == &OPCODE_CALL_TRUE_info ||
         opcode == &OPCODE_CALL_INDIRECT_info ||
         opcode == &OPCODE_CALL_INDIRECT_TRUE_info ||
         opcode == &OPCODE_CALL_EXTERN_info || opcode == &OPCODE_TRAP_info ||
         opcode == &OPCODE_TRAP_TRUE_info ||
         opcode == &OPCODE_DEBUG_BREAK_info ||
         opcode == &OPCODE_DEBUG_BREAK_TRUE_info;
}

}  // namespace

bool CompareBranchSinkPass::Run(HIRBuilder* builder) {
  if (!cvars::a64_sink_compare_to_branch) {
    return true;
  }
  for (auto block = builder->first_block(); block; block = block->next) {
    for (auto branch = block->instr_head; branch; branch = branch->next) {
      if (branch->opcode != &OPCODE_BRANCH_TRUE_info &&
          branch->opcode != &OPCODE_BRANCH_FALSE_info) {
        continue;
      }
      Value* condition = branch->src1.value;
      if (!condition || condition->IsConstant() || !HasSingleUse(condition)) {
        continue;
      }
      Instr* compare = condition->def;
      if (!compare || compare->dest != condition || compare->block != block ||
          !IsCompare(compare)) {
        continue;
      }
      // Already there, or only instructions that emit no code in between
      // (the backend looks past those by itself).
      bool worth_moving = false;
      bool movable = true;
      for (auto between = compare->next; between && between != branch;
           between = between->next) {
        if (IsCallLike(between)) {
          movable = false;
          break;
        }
        if (between->opcode != &OPCODE_CONTEXT_BARRIER_info &&
            between->opcode != &OPCODE_SOURCE_OFFSET_info &&
            between->opcode != &OPCODE_COMMENT_info &&
            between->opcode != &OPCODE_NOP_info) {
          worth_moving = true;
        }
      }
      if (!movable || !worth_moving) {
        continue;
      }
      // The truncation feeding only this compare goes with it, to stay the
      // free narrowing it is next to a 32-bit compare.
      Instr* narrow = nullptr;
      for (Value* operand : {compare->src1.value, compare->src2.value}) {
        if (!operand || operand->IsConstant() || !operand->def) {
          continue;
        }
        Instr* def = operand->def;
        if (def->opcode == &OPCODE_TRUNCATE_info && def->dest == operand &&
            def->block == block && HasSingleUse(operand)) {
          narrow = def;
        }
      }
      if (narrow) {
        narrow->MoveBefore(branch);
      }
      compare->MoveBefore(branch);
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
