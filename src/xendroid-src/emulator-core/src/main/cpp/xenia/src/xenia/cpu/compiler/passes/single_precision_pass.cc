/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/single_precision_pass.h"

#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(a64_single_precision_narrowing, true,
            "Compute single-precision guest arithmetic (fadds, fmuls, "
            "fmadds...) with single-precision host instructions when the "
            "operands are provably singles, instead of in double precision "
            "followed by a round to single and a widening.",
            "CPU");

DEFINE_bool(a64_single_precision_across_blocks, true,
            "a64_single_precision_narrowing also for operands that reach an "
            "instruction through a guest floating-point register from "
            "another block or around a loop, when every path there left a "
            "single in the register.",
            "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

namespace {

// The guest floating-point register a context offset is, or -1.
int FprIndex(uint64_t offset) {
  const uint64_t base = offsetof(ppc::PPCContext, f);
  if (offset < base || offset >= base + 32 * 8 || ((offset - base) & 7)) {
    return -1;
  }
  return int((offset - base) / 8);
}

// Execution continues past these with any guest register changed behind the
// translated code's back.
bool ClobbersContext(const Instr* instr) {
  const OpcodeInfo* opcode = instr->opcode;
  if (opcode == &OPCODE_CALL_info || opcode == &OPCODE_CALL_TRUE_info ||
      opcode == &OPCODE_CALL_INDIRECT_info ||
      opcode == &OPCODE_CALL_INDIRECT_TRUE_info) {
    return true;
  }
  return opcode == &OPCODE_CALL_EXTERN_info || opcode == &OPCODE_TRAP_info ||
         opcode == &OPCODE_TRAP_TRUE_info ||
         opcode == &OPCODE_DEBUG_BREAK_info ||
         opcode == &OPCODE_DEBUG_BREAK_TRUE_info;
}

bool LeavesFunction(const Instr* tail) {
  return tail && (tail->opcode == &OPCODE_BRANCH_info ||
                  tail->opcode == &OPCODE_RETURN_info ||
                  ((tail->opcode == &OPCODE_CALL_info ||
                    tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                   (tail->flags & CALL_TAIL)));
}

void MoveAfter(Instr* instr, Instr* other) {
  if (other->next) {
    instr->MoveBefore(other->next);
    return;
  }
  // |other| is the last of its block.
  Block* old_block = instr->block;
  if (old_block == other->block && instr->prev == other) {
    return;
  }
  if (instr->prev) {
    instr->prev->next = instr->next;
  } else {
    old_block->instr_head = instr->next;
  }
  if (instr->next) {
    instr->next->prev = instr->prev;
  } else {
    old_block->instr_tail = instr->prev;
  }
  Block* block = other->block;
  instr->block = block;
  instr->prev = other;
  instr->next = nullptr;
  other->next = instr;
  block->instr_tail = instr;
}

bool IsFloat64(const Value* value) { return value->type == FLOAT64_TYPE; }

}  // namespace

SinglePrecisionNarrowingPass::SinglePrecisionNarrowingPass() : CompilerPass() {}

SinglePrecisionNarrowingPass::~SinglePrecisionNarrowingPass() = default;

void SinglePrecisionNarrowingPass::PlaceBefore(HIRBuilder* builder,
                                               Instr* before_tail,
                                               Instr* anchor) {
  Instr* created = builder->last_instr();
  if (created && created != before_tail && created != anchor) {
    created->MoveBefore(anchor);
  }
}

void SinglePrecisionNarrowingPass::PlaceAfter(HIRBuilder* builder,
                                              Instr* before_tail,
                                              Instr* anchor) {
  Instr* created = builder->last_instr();
  if (created && created != before_tail && created != anchor) {
    MoveAfter(created, anchor);
  }
}

// The FLOAT32 value holding the same number as the FLOAT64 |value|, or null
// when |value| isn't known to be a single. Created next to the definition of
// |value| when it only exists implicitly, so it is available wherever |value|
// is.
Value* SinglePrecisionNarrowingPass::TwinOf(HIRBuilder* builder, Value* value) {
  if (!value || value->IsConstant() || !IsFloat64(value)) {
    return nullptr;
  }
  auto found = twins_.find(value);
  if (found != twins_.end()) {
    return found->second;
  }
  Instr* def = value->def;
  if (!def || def->dest != value) {
    return nullptr;
  }
  Value* twin = nullptr;
  if (def->opcode == &OPCODE_SINGLE_BITS_TO_DOUBLE_info) {
    // lfs: the loaded bits are the single.
    Value* bits = def->src1.value;
    if (bits && !bits->IsConstant() && bits->type == INT32_TYPE) {
      Instr* tail = builder->last_instr();
      twin = builder->Cast(bits, FLOAT32_TYPE);
      PlaceAfter(builder, tail, def);
      may_signal_[twin] = true;
    }
  } else if (def->opcode == &OPCODE_CONVERT_info) {
    // A widened single.
    Value* source = def->src1.value;
    if (source && !source->IsConstant() && source->type == FLOAT32_TYPE) {
      twin = source;
      // (The widening quieted the double, but the single itself may be raw
      // bits when lfs is translated without the signaling NaN handling.)
      if (source->def && source->def->opcode == &OPCODE_CAST_info) {
        may_signal_[twin] = true;
      }
    }
  } else if (def->opcode == &OPCODE_NEG_info ||
             def->opcode == &OPCODE_ABS_info) {
    // Sign changes of a single are singles.
    Value* source_twin = TwinOf(builder, def->src1.value);
    if (source_twin) {
      Instr* tail = builder->last_instr();
      twin = def->opcode == &OPCODE_NEG_info ? builder->Neg(source_twin)
                                             : builder->Abs(source_twin);
      PlaceAfter(builder, tail, def);
      may_signal_[twin] = MaySignal(source_twin);
    }
  } else if (def->opcode == &OPCODE_ASSIGN_info) {
    twin = TwinOf(builder, def->src1.value);
  } else if (def->opcode == &OPCODE_LOAD_CONTEXT_info &&
             single_loads_.count(value)) {
    // A guest register every path left a single in: narrowing it is exact.
    Instr* tail = builder->last_instr();
    twin = builder->Convert(value, FLOAT32_TYPE, ROUND_DYNAMIC);
    PlaceAfter(builder, tail, def);
    may_signal_[twin] = true;
    narrowed_twins_.insert(twin);
  }
  if (twin) {
    twins_[value] = twin;
  }
  return twin;
}

// Whether the FLOAT64 |value| is a single by construction, given which
// load_context results are (single_loads_).
bool SinglePrecisionNarrowingPass::IsKnownSingle(const Value* value,
                                                 int depth) const {
  if (!value || value->IsConstant() || value->type != FLOAT64_TYPE ||
      depth > 8) {
    return false;
  }
  const Instr* def = value->def;
  if (!def || def->dest != value) {
    return false;
  }
  const OpcodeInfo* op = def->opcode;
  if (op == &OPCODE_SINGLE_BITS_TO_DOUBLE_info ||
      op == &OPCODE_TO_SINGLE_info) {
    return true;
  }
  if (op == &OPCODE_CONVERT_info) {
    return def->src1.value && def->src1.value->type == FLOAT32_TYPE;
  }
  if (op == &OPCODE_NEG_info || op == &OPCODE_ABS_info ||
      op == &OPCODE_ASSIGN_info) {
    return IsKnownSingle(def->src1.value, depth + 1);
  }
  if (op == &OPCODE_LOAD_CONTEXT_info) {
    return single_loads_.count(value) != 0;
  }
  return false;
}

void SinglePrecisionNarrowingPass::FindSingleContextLoads(HIRBuilder* builder) {
  single_loads_.clear();
  // Blocks by ordinal.
  std::vector<Block*> blocks;
  std::unordered_map<const Block*, uint32_t> ordinal;
  for (Block* block = builder->first_block(); block; block = block->next) {
    ordinal[block] = uint32_t(blocks.size());
    blocks.push_back(block);
  }
  const uint32_t count = uint32_t(blocks.size());
  if (!count || count > 4096) {
    return;
  }
  // One bit per guest floating-point register known to hold a single on
  // entry to each block: the meet (and) over every way in. Optimistic start,
  // lowered until nothing changes; a block nothing is seen to lead to gets
  // nothing.
  std::vector<uint32_t> in(count, ~uint32_t(0));
  std::vector<uint8_t> reached(count, 1);
  in[0] = 0;
  const auto flow = [&](bool record) {
    bool changed = false;
    std::vector<uint32_t> next_in(count, ~uint32_t(0));
    std::vector<uint8_t> next_reached(count, 0);
    next_in[0] = 0;
    next_reached[0] = 1;
    const auto edge = [&](const Block* to, uint32_t state) {
      auto found = ordinal.find(to);
      if (found == ordinal.end()) {
        return;
      }
      next_in[found->second] &= state;
      next_reached[found->second] = 1;
    };
    for (uint32_t n = 0; n < count; ++n) {
      uint32_t state = reached[n] ? in[n] : 0;
      const Block* block = blocks[n];
      for (const Instr* instr = block->instr_head; instr;
           instr = instr->next) {
        const OpcodeInfo* op = instr->opcode;
        if (op == &OPCODE_LOAD_CONTEXT_info) {
          const int fpr = FprIndex(instr->src1.offset);
          if (fpr >= 0 && instr->dest && instr->dest->type == FLOAT64_TYPE) {
            if ((state >> fpr) & 1) {
              single_loads_.insert(instr->dest);
            } else {
              single_loads_.erase(instr->dest);
            }
          }
        } else if (op == &OPCODE_STORE_CONTEXT_info) {
          const int fpr = FprIndex(instr->src1.offset);
          if (fpr >= 0) {
            if (IsKnownSingle(instr->src2.value)) {
              state |= uint32_t(1) << fpr;
            } else {
              state &= ~(uint32_t(1) << fpr);
            }
          }
        } else if (ClobbersContext(instr)) {
          state = 0;
        } else if (op == &OPCODE_BRANCH_info) {
          if (instr->src1.label && instr->src1.label->block) {
            edge(instr->src1.label->block, state);
          }
        } else if (op == &OPCODE_BRANCH_TRUE_info ||
                   op == &OPCODE_BRANCH_FALSE_info) {
          if (instr->src2.label && instr->src2.label->block) {
            edge(instr->src2.label->block, state);
          }
        }
      }
      if (!LeavesFunction(block->instr_tail) && n + 1 < count) {
        edge(blocks[n + 1], state);
      }
    }
    for (uint32_t n = 1; n < count; ++n) {
      const uint32_t value = next_reached[n] ? next_in[n] : 0;
      // Only ever lowered: the first round starts from everything.
      const uint32_t lowered = reached[n] ? (in[n] & value) : value;
      if (lowered != in[n] || next_reached[n] != reached[n]) {
        changed = true;
      }
      in[n] = lowered;
      reached[n] = next_reached[n];
    }
    (void)record;
    return changed;
  };
  // (Loads are classified with the state of the round they are seen in, so
  // the last round - the one that changes nothing - leaves them right.)
  for (int round = 0; round < 64; ++round) {
    if (!flow(false)) {
      return;
    }
  }
  // Did not settle (it always should): assume nothing.
  single_loads_.clear();
}

bool SinglePrecisionNarrowingPass::Run(HIRBuilder* builder) {
  // The bool return is pass success, not whether anything changed.
  if (!cvars::a64_single_precision_narrowing) {
    return true;
  }
  twins_.clear();
  may_signal_.clear();
  narrowed_twins_.clear();
  single_loads_.clear();
  if (cvars::a64_single_precision_across_blocks) {
    FindSingleContextLoads(builder);
  }

  for (Block* block = builder->first_block(); block; block = block->next) {
    for (Instr* instr = block->instr_head; instr;) {
      Instr* next = instr->next;
      const OpcodeInfo* opcode = instr->opcode;

      if (opcode == &OPCODE_TO_SINGLE_info && instr->dest &&
          instr->src1.value && !instr->src1.value->IsConstant()) {
        Value* source = instr->src1.value;
        Instr* def = source->def;
        Value* result = nullptr;
        if (Value* twin = TwinOf(builder, source)) {
          if (!MaySignal(twin)) {
            // Rounding a single to single: the value itself.
            instr->Replace(&OPCODE_ASSIGN_info, 0);
            instr->set_src1(source);
            twins_[instr->dest] = twin;
            instr = next;
            continue;
          }
        }
        if (def && def->dest == source && def->block == block) {
          Instr* tail = builder->last_instr();
          const OpcodeInfo* op = def->opcode;
          if (op == &OPCODE_ADD_info || op == &OPCODE_SUB_info ||
              op == &OPCODE_MUL_info || op == &OPCODE_DIV_info) {
            Value* a = TwinOf(builder, def->src1.value);
            Value* b = a ? TwinOf(builder, def->src2.value) : nullptr;
            if (a && b) {
              tail = builder->last_instr();
              if (op == &OPCODE_ADD_info) {
                result = builder->Add(a, b);
              } else if (op == &OPCODE_SUB_info) {
                result = builder->Sub(a, b);
              } else if (op == &OPCODE_MUL_info) {
                result = builder->Mul(a, b);
              } else {
                result = builder->Div(a, b);
              }
              PlaceBefore(builder, tail, instr);
            }
          } else if (op == &OPCODE_MUL_ADD_info || op == &OPCODE_MUL_SUB_info) {
            Value* a = TwinOf(builder, def->src1.value);
            Value* b = a ? TwinOf(builder, def->src2.value) : nullptr;
            Value* c = b ? TwinOf(builder, def->src3.value) : nullptr;
            if (a && b && c) {
              tail = builder->last_instr();
              result = op == &OPCODE_MUL_ADD_info ? builder->MulAdd(a, b, c)
                                                  : builder->MulSub(a, b, c);
              PlaceBefore(builder, tail, instr);
            }
          } else if (op == &OPCODE_NEG_info && def->src1.value &&
                     !def->src1.value->IsConstant()) {
            // fnmadds / fnmsubs: the negated fused operation.
            Instr* inner = def->src1.value->def;
            if (inner && inner->dest == def->src1.value &&
                inner->block == block &&
                (inner->opcode == &OPCODE_MUL_ADD_info ||
                 inner->opcode == &OPCODE_MUL_SUB_info)) {
              Value* a = TwinOf(builder, inner->src1.value);
              Value* b = a ? TwinOf(builder, inner->src2.value) : nullptr;
              Value* c = b ? TwinOf(builder, inner->src3.value) : nullptr;
              if (a && b && c) {
                tail = builder->last_instr();
                Value* fused = inner->opcode == &OPCODE_MUL_ADD_info
                                   ? builder->MulAdd(a, b, c)
                                   : builder->MulSub(a, b, c);
                PlaceBefore(builder, tail, instr);
                tail = builder->last_instr();
                result = builder->Neg(fused);
                PlaceBefore(builder, tail, instr);
              }
            }
          } else if (op == &OPCODE_SQRT_info) {
            if (Value* a = TwinOf(builder, def->src1.value)) {
              tail = builder->last_instr();
              result = builder->Sqrt(a);
              PlaceBefore(builder, tail, instr);
            }
          }
        }
        if (!result) {
          // Operands of unknown origin: the double result rounded to single,
          // as before - but keeping the single around as the twin.
          Instr* tail = builder->last_instr();
          result = builder->Convert(source, FLOAT32_TYPE, ROUND_DYNAMIC);
          PlaceBefore(builder, tail, instr);
        }
        if (result && result->type == FLOAT32_TYPE && !result->IsConstant()) {
          instr->Replace(&OPCODE_CONVERT_info, ROUND_DYNAMIC);
          instr->set_src1(result);
          twins_[instr->dest] = result;
        }
      } else if (opcode == &OPCODE_DOUBLE_TO_SINGLE_BITS_info &&
                 instr->src1.value) {
        // stfs of a single: its bits (of the double itself when the twin
        // was narrowed from it - see narrowed_twins_).
        Value* twin = TwinOf(builder, instr->src1.value);
        if (twin && !narrowed_twins_.count(twin)) {
          instr->Replace(&OPCODE_CAST_info, 0);
          instr->set_src1(twin);
        }
      } else if (opcode == &OPCODE_CONVERT_info && instr->dest &&
                 instr->dest->type == FLOAT32_TYPE && instr->src1.value &&
                 IsFloat64(instr->src1.value)) {
        // Rounding a single to single.
        Value* twin = TwinOf(builder, instr->src1.value);
        if (twin && !MaySignal(twin)) {
          instr->Replace(&OPCODE_ASSIGN_info, 0);
          instr->set_src1(twin);
        }
      }
      instr = next;
    }
  }
  twins_.clear();
  may_signal_.clear();
  narrowed_twins_.clear();
  single_loads_.clear();
  return true;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
