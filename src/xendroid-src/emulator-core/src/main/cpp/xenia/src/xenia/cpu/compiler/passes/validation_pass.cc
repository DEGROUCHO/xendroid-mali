/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/validation_pass.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/hir/block.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/hir/opcodes.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/processor.h"

DECLARE_bool(a64_forward_context_ssa);

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::OpcodeSignatureType;
using xe::cpu::hir::Value;

struct ValidationPass::ValidationContext {
  static constexpr size_t kMaxDominanceBlocks = 2048;
  static constexpr size_t kMaxGraphEdges = 1u << 20;

  bool allow_cross_block = false;
  bool dominators_ready = false;
  std::vector<Block*> blocks;
  std::unordered_map<Block*, size_t> block_indices;
  std::unordered_map<Instr*, size_t> instruction_order;
  std::vector<size_t> first_terminator_order;
  std::vector<std::vector<size_t>> predecessors;
  std::vector<std::vector<size_t>> successors;
  std::vector<uint8_t> reachable;
  std::vector<std::vector<uint64_t>> dominators;

  bool BuildInstructionOrder(HIRBuilder* builder) {
    size_t order = 0;
    for (Block* block = builder->first_block(); block; block = block->next) {
      const size_t index = blocks.size();
      blocks.push_back(block);
      if (!block_indices.emplace(block, index).second) {
        return false;
      }
      for (Instr* instr = block->instr_head; instr; instr = instr->next) {
        if (!instruction_order.emplace(instr, order++).second) {
          return false;
        }
      }
    }

    first_terminator_order.assign(blocks.size(),
                                  std::numeric_limits<size_t>::max());
    for (size_t index = 0; index < blocks.size(); ++index) {
      for (Instr* instr = blocks[index]->instr_head; instr;
           instr = instr->next) {
        if (instr->opcode != &OPCODE_BRANCH_info &&
            instr->opcode != &OPCODE_BRANCH_TRUE_info &&
            instr->opcode != &OPCODE_BRANCH_FALSE_info) {
          continue;
        }
        auto it = instruction_order.find(instr);
        if (it == instruction_order.end()) {
          return false;
        }
        first_terminator_order[index] =
            std::min(first_terminator_order[index], it->second);
      }
    }
    return true;
  }

  bool HasCrossBlockUse() const {
    for (Block* block : blocks) {
      for (Instr* instr = block->instr_head; instr; instr = instr->next) {
        if (instr->dest) {
          for (Value::Use* use = instr->dest->use_head; use;
               use = use->next) {
            if (use->instr && use->instr->block != block) {
              return true;
            }
          }
        }
        const uint32_t signature = instr->opcode->signature;
        if (GET_OPCODE_SIG_TYPE_SRC1(signature) == OPCODE_SIG_TYPE_V &&
            instr->src1.value && instr->src1.value->def &&
            instr->src1.value->def->block != block) {
          return true;
        }
        if (GET_OPCODE_SIG_TYPE_SRC2(signature) == OPCODE_SIG_TYPE_V &&
            instr->src2.value && instr->src2.value->def &&
            instr->src2.value->def->block != block) {
          return true;
        }
        if (GET_OPCODE_SIG_TYPE_SRC3(signature) == OPCODE_SIG_TYPE_V &&
            instr->src3.value && instr->src3.value->def &&
            instr->src3.value->def->block != block) {
          return true;
        }
      }
    }
    return false;
  }

  bool ComputeDominators() {
    const size_t block_count = blocks.size();
    if (!block_count || block_count > kMaxDominanceBlocks) {
      return false;
    }

    predecessors.assign(block_count, {});
    successors.assign(block_count, {});
    size_t edge_count = 0;
    auto add_edge = [&](size_t src, size_t dest) {
      if (++edge_count > kMaxGraphEdges) {
        return false;
      }
      predecessors[dest].push_back(src);
      successors[src].push_back(dest);
      return true;
    };
    for (size_t src_index = 0; src_index < block_count; ++src_index) {
      Block* block = blocks[src_index];
      for (Instr* instr = block->instr_head; instr; instr = instr->next) {
        if (!(instr->opcode->flags & OPCODE_FLAG_BRANCH)) {
          continue;
        }
        Label* target = nullptr;
        if (instr->opcode == &OPCODE_BRANCH_info) {
          target = instr->src1.label;
        } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                   instr->opcode == &OPCODE_BRANCH_FALSE_info) {
          target = instr->src2.label;
        } else if (instr->opcode != &OPCODE_CALL_info &&
                   instr->opcode != &OPCODE_CALL_TRUE_info &&
                   instr->opcode != &OPCODE_CALL_INDIRECT_info &&
                   instr->opcode != &OPCODE_CALL_INDIRECT_TRUE_info &&
                   instr->opcode != &OPCODE_CALL_EXTERN_info &&
                   instr->opcode != &OPCODE_RETURN_info &&
                   instr->opcode != &OPCODE_RETURN_TRUE_info) {
          // Do not claim cross-block dominance for a new control-flow opcode
          // until its intrafunction successors are modeled here.
          return false;
        }
        if (target) {
          if (!target->block) {
            return false;
          }
          auto dest_it = block_indices.find(target->block);
          if (dest_it == block_indices.end() ||
              !add_edge(src_index, dest_it->second)) {
            return false;
          }
        }
      }

      Instr* tail = block->instr_tail;
      const bool unconditional_transfer =
          tail && (tail->opcode == &OPCODE_BRANCH_info ||
                   tail->opcode == &OPCODE_RETURN_info ||
                   ((tail->opcode == &OPCODE_CALL_info ||
                     tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                    (tail->flags & CALL_TAIL)));
      if (!unconditional_transfer && src_index + 1 < block_count &&
          !add_edge(src_index, src_index + 1)) {
        return false;
      }
    }

    // Reachability starts at the function entry. Edges include explicit branch
    // targets and sequential fallthrough, which ControlFlowAnalysisPass does
    // not record for conditional branches and returning calls. Disconnected
    // code is treated conservatively as independent roots below.
    reachable.assign(block_count, 0);
    std::vector<size_t> pending{0};
    reachable[0] = 1;
    while (!pending.empty()) {
      const size_t src = pending.back();
      pending.pop_back();
      for (size_t dest : successors[src]) {
        if (!reachable[dest]) {
          reachable[dest] = 1;
          pending.push_back(dest);
        }
      }
    }

    const size_t word_count = (block_count + 63) / 64;
    std::vector<uint64_t> all_reachable(word_count, 0);
    dominators.assign(block_count, std::vector<uint64_t>(word_count, 0));
    for (size_t i = 0; i < block_count; ++i) {
      if (reachable[i]) {
        all_reachable[i / 64] |= uint64_t(1) << (i % 64);
      } else {
        // Do not invent a root for a disconnected component or cycle.
        dominators[i][i / 64] |= uint64_t(1) << (i % 64);
      }
    }
    dominators[0][0] |= 1;
    for (size_t i = 1; i < block_count; ++i) {
      if (reachable[i]) {
        dominators[i] = all_reachable;
      }
    }

    bool converged = false;
    // A descending dominator-set iteration can need one sweep per block when
    // the block list is opposite to the CFG order. Bound it by the maximum
    // accepted graph size instead of relying on typical translator ordering.
    for (size_t iteration = 0; iteration <= block_count; ++iteration) {
      bool changed = false;
      for (size_t block_index = 1; block_index < block_count;
           ++block_index) {
        if (!reachable[block_index]) {
          continue;
        }
        std::vector<uint64_t> next = all_reachable;
        bool has_reachable_predecessor = false;
        for (size_t pred : predecessors[block_index]) {
          if (!reachable[pred]) {
            continue;
          }
          if (!has_reachable_predecessor) {
            next = dominators[pred];
            has_reachable_predecessor = true;
          } else {
            for (size_t word = 0; word < word_count; ++word) {
              next[word] &= dominators[pred][word];
            }
          }
        }
        if (!has_reachable_predecessor) {
          std::fill(next.begin(), next.end(), 0);
        }
        next[block_index / 64] |= uint64_t(1) << (block_index % 64);
        if (next != dominators[block_index]) {
          dominators[block_index].swap(next);
          changed = true;
        }
      }
      if (!changed) {
        converged = true;
        break;
      }
    }
    dominators_ready = converged;
    return dominators_ready;
  }

  bool IsBefore(Instr* def, Instr* use) const {
    auto def_it = instruction_order.find(def);
    auto use_it = instruction_order.find(use);
    return def_it != instruction_order.end() &&
           use_it != instruction_order.end() && def_it->second < use_it->second;
  }

  bool IsBeforeTerminator(Instr* def) const {
    auto block_it = block_indices.find(def->block);
    auto instr_it = instruction_order.find(def);
    if (block_it == block_indices.end() || instr_it == instruction_order.end()) {
      return false;
    }
    const size_t terminator = first_terminator_order[block_it->second];
    return terminator == std::numeric_limits<size_t>::max() ||
           instr_it->second < terminator;
  }

  bool Dominates(Block* def, Block* use) const {
    auto def_it = block_indices.find(def);
    auto use_it = block_indices.find(use);
    if (!dominators_ready || def_it == block_indices.end() ||
        use_it == block_indices.end()) {
      return false;
    }
    const size_t def_index = def_it->second;
    const size_t use_index = use_it->second;
    return (dominators[use_index][def_index / 64] &
            (uint64_t(1) << (def_index % 64))) != 0;
  }
};

ValidationPass::ValidationPass() : CompilerPass() {}

ValidationPass::~ValidationPass() {}

bool ValidationPass::Run(HIRBuilder* builder) {
#if 0
  StringBuffer str;
  builder->Dump(&str);
  printf("%s", str.GetString());
  fflush(stdout);
  str.Reset();
#endif  // 0

  ValidationContext context;
  context.allow_cross_block = cvars::a64_forward_context_ssa;
  if (!context.BuildInstructionOrder(builder)) {
    return false;
  }
  if (context.allow_cross_block && context.HasCrossBlockUse() &&
      !context.ComputeDominators()) {
    return false;
  }

  auto block = builder->first_block();
  while (block) {
    auto label = block->label_head;
    while (label) {
      assert_true(label->block == block);
      if (label->block != block) {
        return false;
      }
      label = label->next;
    }

    auto instr = block->instr_head;
    while (instr) {
      if (!ValidateInstruction(block, instr, context)) {
        return false;
      }
      instr = instr->next;
    }

    block = block->next;
  }

  return true;
}

bool ValidationPass::ValidateInstruction(Block* block, Instr* instr,
                                         const ValidationContext& context) {
  assert_true(instr->block == block);
  if (instr->block != block) {
    return false;
  }

  if (instr->dest) {
    assert_true(instr->dest->def == instr);
    auto use = instr->dest->use_head;
    while (use) {
      if (!use->instr || !use->instr->block ||
          !ValidateValue(use->instr->block, use->instr, instr->dest, context)) {
        return false;
      }
      use = use->next;
    }
  }

  uint32_t signature = instr->opcode->signature;
  if (GET_OPCODE_SIG_TYPE_SRC1(signature) == OPCODE_SIG_TYPE_V) {
    if (!ValidateValue(block, instr, instr->src1.value, context)) {
      return false;
    }
  }
  if (GET_OPCODE_SIG_TYPE_SRC2(signature) == OPCODE_SIG_TYPE_V) {
    if (!ValidateValue(block, instr, instr->src2.value, context)) {
      return false;
    }
  }
  if (GET_OPCODE_SIG_TYPE_SRC3(signature) == OPCODE_SIG_TYPE_V) {
    if (!ValidateValue(block, instr, instr->src3.value, context)) {
      return false;
    }
  }

  return true;
}

bool ValidationPass::ValidateValue(Block* block, Instr* instr, Value* value,
                                   const ValidationContext& context) {
  if (!value || value->IsConstant() || !value->def) {
    return true;
  }
  Instr* def = value->def;
  if (!def->block || !instr || !instr->block || instr->block != block) {
    return false;
  }
  if (def->block == block) {
    return !context.allow_cross_block || context.IsBefore(def, instr);
  }
  return context.allow_cross_block && context.Dominates(def->block, block) &&
         context.IsBeforeTerminator(def);
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
