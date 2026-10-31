/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/float_memory_domain_pass.h"

#include <unordered_set>

#include "xenia/base/cvar.h"
#include "xenia/cpu/hir/hir_builder.h"

DEFINE_bool(a64_float_memory_domain, true,
            "Load and store floating-point values straight in the vector "
            "registers (byte reverse included) when their bits are never "
            "used as an integer, instead of through a general register and "
            "a move across.",
            "CPU");
DEFINE_bool(a64_float_memory_domain_doubles, false,
            "a64_float_memory_domain for 64-bit values as well. Off: on the "
            "Cortex-A725 a double goes through memory faster by way of a "
            "general register (measured: a double-heavy loop 7% slower with "
            "the vector byte reverse), a single does not.",
            "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

FloatMemoryDomainPass::FloatMemoryDomainPass() : CompilerPass() {}

FloatMemoryDomainPass::~FloatMemoryDomainPass() = default;

namespace {

// The float type with the bits of |type|, or |type| itself if there is none.
TypeName FloatOfSameSize(TypeName type) {
  return type == INT64_TYPE ? FLOAT64_TYPE
                            : (type == INT32_TYPE ? FLOAT32_TYPE : type);
}

// A leftover of an earlier pass that dead code elimination is about to
// remove (it runs after this pass): the byte swap a load or store absorbed,
// an assignment everyone was renamed past.
bool IsDeadLeftover(const Instr* instr) {
  return (instr->opcode == &OPCODE_ASSIGN_info ||
          instr->opcode == &OPCODE_BYTE_SWAP_info ||
          instr->opcode == &OPCODE_CAST_info) &&
         instr->dest && !instr->dest->use_head;
}

bool HasSingleLiveUse(const Value* value) {
  uint32_t live = 0;
  for (auto use = value->use_head; use; use = use->next) {
    if (!IsDeadLeftover(use->instr) && ++live > 1) {
      return false;
    }
  }
  return live == 1;
}

}  // namespace

bool FloatMemoryDomainPass::Run(HIRBuilder* builder) {
  if (!cvars::a64_float_memory_domain) {
    return true;
  }
  // Casts that became moves of a value onto itself: their users take the
  // loaded value directly (below), and dead code elimination removes them.
  std::unordered_set<const Instr*> moves;
  for (auto block = builder->first_block(); block; block = block->next) {
    for (auto i = block->instr_head; i; i = i->next) {
      if (i->opcode == &OPCODE_LOAD_info) {
        Value* loaded = i->dest;
        const TypeName float_type = FloatOfSameSize(loaded->type);
        if (float_type == loaded->type || !loaded->use_head) {
          continue;
        }
        if (float_type == FLOAT64_TYPE &&
            !cvars::a64_float_memory_domain_doubles) {
          continue;
        }
        // Every use takes the bits as a float of the same size.
        bool only_float = true;
        bool any_float = false;
        for (auto use = loaded->use_head; use; use = use->next) {
          const Instr* user = use->instr;
          if (IsDeadLeftover(user)) {
            continue;
          }
          any_float = true;
          const bool is_cast = user->opcode == &OPCODE_CAST_info &&
                               user->dest->type == float_type;
          const bool is_single_to_double =
              float_type == FLOAT32_TYPE &&
              user->opcode == &OPCODE_SINGLE_BITS_TO_DOUBLE_info;
          if (!(is_cast || is_single_to_double) ||
              user->src1.value != loaded) {
            only_float = false;
            break;
          }
        }
        if (!only_float || !any_float) {
          continue;
        }
        loaded->type = float_type;
        for (auto use = loaded->use_head; use;) {
          auto next_use = use->next;
          Instr* user = use->instr;
          if (IsDeadLeftover(user)) {
            // Off the value before its type changes under it (it would be
            // an integer operation on a float until it is removed).
            user->UnlinkAndNOP();
          } else if (user->opcode == &OPCODE_CAST_info) {
            // Now a move between two values of one type.
            user->opcode = &OPCODE_ASSIGN_info;
            user->flags = 0;
            moves.insert(user);
          }
          use = next_use;
        }
      } else if (i->opcode == &OPCODE_STORE_info) {
        Value* stored = i->src2.value;
        if (stored->IsConstant() || !stored->def || !HasSingleLiveUse(stored)) {
          continue;
        }
        Instr* def = stored->def;
        if (def->opcode == &OPCODE_CAST_info) {
          // store(cast float -> bits): store the float.
          Value* source = def->src1.value;
          if (!source->IsConstant() &&
              FloatOfSameSize(stored->type) == source->type &&
              source->type != stored->type &&
              (source->type != FLOAT64_TYPE ||
               cvars::a64_float_memory_domain_doubles)) {
            i->set_src2(source);
          }
        } else if (def->opcode == &OPCODE_DOUBLE_TO_SINGLE_BITS_info &&
                   stored->type == INT32_TYPE) {
          // The single is only stored: have it produced where it is stored
          // from.
          for (auto use = stored->use_head; use;) {
            auto next_use = use->next;
            if (IsDeadLeftover(use->instr)) {
              use->instr->UnlinkAndNOP();
            }
            use = next_use;
          }
          stored->type = FLOAT32_TYPE;
        }
      }
    }
  }
  if (!moves.empty()) {
    for (auto block = builder->first_block(); block; block = block->next) {
      for (auto i = block->instr_head; i; i = i->next) {
        i->VisitValueOperands([&moves, i](Value* value, uint32_t idx) {
          if (value->def && moves.count(value->def)) {
            i->set_srcN(value->def->src1.value, idx);
          }
        });
      }
    }
  }
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
