/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/preempt_check_injection_pass.h"

#include <unordered_set>

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DECLARE_bool(guest_scheduler);
DEFINE_bool(preempt_check_skip_leaf_entry, true,
            "No preemption safepoint at the entry of a guest function that "
            "calls nothing and returns where it was called from: no cycle of "
            "calls can run through it without passing a function that has "
            "one. Its loops, if any, keep theirs.",
            "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

PreemptCheckInjectionPass::PreemptCheckInjectionPass() : CompilerPass() {}

PreemptCheckInjectionPass::~PreemptCheckInjectionPass() {}

bool PreemptCheckInjectionPass::Run(HIRBuilder* builder) {
  // The bool return is pass success, not whether anything changed, and Compile
  // aborts the whole function on false.
  //
  // Read the cvar here, not in the ctor, so a per-title override applies.
  if (!cvars::guest_scheduler || !builder->first_block()) {
    return true;
  }
  // Blocks are laid out in guest address order, so every intra-function cycle
  // branches to an already-seen block. Calls, recursion and indirect branches
  // (bcctr lowers to CallIndirect) re-enter at the entry block.
  std::unordered_set<Block*> seen;
  std::unordered_set<Block*> check_blocks;
  // The entry check is what breaks cycles made of calls. A function that
  // makes none - its only way out the return to its caller, with the link
  // register it was given - is in no such cycle by itself: whoever calls it
  // in a loop or recursively has a check of its own.
  bool entry_check = true;
  if (cvars::preempt_check_skip_leaf_entry) {
    entry_check = false;
    for (auto block = builder->first_block(); block && !entry_check;
         block = block->next) {
      for (auto instr = block->instr_head; instr; instr = instr->next) {
        const auto opcode = instr->GetOpcodeNum();
        const bool plain_return =
            opcode == OPCODE_CALL_INDIRECT &&
            (instr->flags & (CALL_TAIL | CALL_POSSIBLE_RETURN)) ==
                (CALL_TAIL | CALL_POSSIBLE_RETURN);
        const bool calls_out =
            opcode == OPCODE_CALL || opcode == OPCODE_CALL_TRUE ||
            opcode == OPCODE_CALL_EXTERN ||
            opcode == OPCODE_CALL_INDIRECT_TRUE ||
            (opcode == OPCODE_CALL_INDIRECT && !plain_return);
        // A return to somewhere else than the caller is a jump.
        const bool writes_lr =
            opcode == OPCODE_STORE_CONTEXT &&
            instr->src1.offset == offsetof(ppc::PPCContext, lr);
        if (calls_out || writes_lr) {
          entry_check = true;
          break;
        }
      }
    }
  }
  if (entry_check) {
    check_blocks.insert(builder->first_block());
  }
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    seen.insert(block);
    // Scan every instruction: a back-edge is not always in the trailing
    // branch run. A loop ending its block with a call hides the loop branch
    // from a tail-backward walk, leaving it to spin with no safepoint.
    for (auto instr = block->instr_head; instr != nullptr;
         instr = instr->next) {
      Label* label = nullptr;
      if (instr->opcode == &OPCODE_BRANCH_info) {
        label = instr->src1.label;
      } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                 instr->opcode == &OPCODE_BRANCH_FALSE_info) {
        label = instr->src2.label;
      }
      if (label && label->block && seen.count(label->block)) {
        check_blocks.insert(label->block);
      }
    }
  }
  for (auto block : check_blocks) {
    // A block holding only fake instructions falls through, so the check
    // lands in the first real successor, still on every cycle through it.
    for (auto b = block; b != nullptr; b = b->next) {
      auto first = b->instr_head;
      for (; first && first->IsFake(); first = first->next) {
      }
      if (first) {
        if (first->GetOpcodeNum() != OPCODE_CHECK_PREEMPT) {
          // Carry the guest address of the safepoint so the backend can record
          // where a fiber last checked in. A fiber that stops yielding is
          // otherwise only locatable by its link register, which points at the
          // last call it made rather than the loop it is stuck in.
          uint32_t guest_address = 0;
          for (auto s = b->instr_head; s; s = s->next) {
            if (s->opcode == &OPCODE_SOURCE_OFFSET_info) {
              guest_address = uint32_t(s->src1.offset);
              break;
            }
          }
          Instr* check = builder->CheckPreempt();
          check->src1.offset = guest_address;
          check->MoveBefore(first);
        }
        break;
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
