// Bounded strip-mining of call-free CTR loops. The scalar loop is retained
// verbatim for short counts and low32 CTR == 0 (the wrapping bdnz case).
#ifndef XENIA_CPU_COMPILER_PASSES_COUNTED_LOOP_UNROLL_H_
#define XENIA_CPU_COMPILER_PASSES_COUNTED_LOOP_UNROLL_H_
#include <unordered_map>
#include <vector>
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/label.h"
namespace xe::cpu::compiler::passes {
namespace counted_loop_detail {
using namespace xe::cpu::hir;
inline void Append(HIRBuilder* builder, Block* block, Instr* instr) {
  instr->block = block;
  instr->prev = block->instr_tail;
  instr->next = nullptr;
  if (instr->prev) instr->prev->next = instr;
  else block->instr_head = instr;
  block->instr_tail = instr;
}
inline bool Reject(uint32_t* rejected_at, uint32_t line) {
  if (rejected_at) *rejected_at = line;
  return false;
}
inline bool Unroll(HIRBuilder* builder, Block* block, uint32_t ctr_offset,
                   uint32_t* rejected_at = nullptr,
                   bool allow_scalar_memory = false) {
  // The trailing branch run must contain exactly bdnz-to-self + an explicit
  // scalar exit. Requiring explicit edges avoids guessing fallthroughs.
  Instr* branch = nullptr;
  Instr* exit = nullptr;
  for (Instr* i = block->instr_tail;
       i && (i->opcode->flags & OPCODE_FLAG_BRANCH); i = i->prev) {
    if (i->opcode == &OPCODE_BRANCH_TRUE_info &&
        i->src2.label->block == block && !branch) branch = i;
    else if (i->opcode == &OPCODE_BRANCH_info &&
             i->src1.label->block != block && !exit) exit = i;
    else return Reject(rejected_at, __LINE__);
  }
  if (!branch || !exit || branch->next != exit || exit != block->instr_tail)
    return Reject(rejected_at, __LINE__);
  Instr* cmp = branch->src1.value->def;
  if (!cmp || cmp->opcode != &OPCODE_COMPARE_NE_info ||
      !cmp->src2.value->IsConstant() || cmp->src2.value->constant.u32 != 0)
    return Reject(rejected_at, __LINE__);
  Instr* trunc = cmp->src1.value->def;
  if (!trunc || trunc->opcode != &OPCODE_TRUNCATE_info ||
      trunc->dest->type != INT32_TYPE) return Reject(rejected_at, __LINE__);
  Instr* sub = trunc->src1.value->def;
  if (!sub || sub->opcode != &OPCODE_SUB_info || sub->flags ||
      sub->dest->type != INT64_TYPE || !sub->src2.value->IsConstant() ||
      sub->src2.value->constant.u64 != 1) return Reject(rejected_at, __LINE__);
  Instr* load_ctr = sub->src1.value->def;
  if (!load_ctr || load_ctr->opcode != &OPCODE_LOAD_CONTEXT_info ||
      load_ctr->src1.offset != ctr_offset ||
      load_ctr->dest->type != INT64_TYPE) return Reject(rejected_at, __LINE__);
  auto last = builder->last_block()->instr_tail;
  if (!last || !(last->opcode == &OPCODE_RETURN_info ||
                last->opcode == &OPCODE_BRANCH_info ||
                ((last->opcode == &OPCODE_CALL_info ||
                  last->opcode == &OPCODE_CALL_INDIRECT_info) &&
                 (last->flags & CALL_TAIL)))) return Reject(rejected_at, __LINE__);

  std::vector<Instr*> body, entries;
  Instr* store_ctr = nullptr;
  Instr* counter_barrier = nullptr;
  uint32_t guest_pc = 0;
  unsigned source_count = 0, vector_count = 0, memory_stores = 0;
  for (Instr* i = block->instr_head; i != branch; i = i->next) {
    if (!i || body.size() >= 1024 ||
        (i->opcode->flags & (OPCODE_FLAG_VOLATILE | OPCODE_FLAG_BRANCH)))
      return Reject(rejected_at, __LINE__);
    // Data prefetches are nonfaulting ARM hints and keep their original order.
    // Explicit MMIO, cache invalidation and reservation helpers remain excluded.
    const bool prefetch = i->opcode == &OPCODE_CACHE_CONTROL_info &&
        (i->flags == CACHE_CONTROL_TYPE_DATA_TOUCH ||
         i->flags == CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE);
    if (((i->opcode->flags & OPCODE_FLAG_MEMORY) && !prefetch &&
         i->opcode != &OPCODE_LOAD_info && i->opcode != &OPCODE_STORE_info &&
         i->opcode != &OPCODE_LOAD_OFFSET_info &&
         i->opcode != &OPCODE_STORE_OFFSET_info &&
         i->opcode != &OPCODE_LVL_info && i->opcode != &OPCODE_LVR_info) ||
        i->opcode == &OPCODE_LOAD_LOCAL_info ||
        i->opcode == &OPCODE_STORE_LOCAL_info ||
        i->opcode == &OPCODE_SET_NJM_info ||
        i->opcode == &OPCODE_LOAD_CLOCK_info) return Reject(rejected_at, __LINE__);
    if (i->opcode == &OPCODE_SOURCE_OFFSET_info) {
      ++source_count;
      if (!guest_pc) guest_pc = uint32_t(i->src1.offset);
    }
    if (i->opcode == &OPCODE_CONTEXT_BARRIER_info) counter_barrier = i;
    if (i->opcode == &OPCODE_STORE_info ||
        i->opcode == &OPCODE_STORE_OFFSET_info) ++memory_stores;
    if (i->dest && i->dest->type == VEC128_TYPE) ++vector_count;
    if (i->opcode == &OPCODE_STORE_CONTEXT_info ||
        i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      Value* value = i->opcode == &OPCODE_LOAD_CONTEXT_info ? i->dest
                                                         : i->src2.value;
      auto offset = i->src1.offset;
      auto size = GetTypeSize(value->type);
      if (offset < ctr_offset + 8 && offset + size > ctr_offset) {
        if (i == load_ctr) {}
        else if (i->opcode == &OPCODE_STORE_CONTEXT_info &&
                 offset == ctr_offset && i->src2.value == sub->dest &&
                 !store_ctr) store_ctr = i;
        else return Reject(rejected_at, __LINE__);
      }
    }
    // No SSA values may enter from or escape to another block. The four
    // copies communicate through context, promoted only after construction.
    if (i->dest) {
      for (auto use = i->dest->use_head; use; use = use->next)
        if (use->instr->block != block) return Reject(rejected_at, __LINE__);
    }
    for (unsigned n = 0; n != 3; ++n) {
      auto sig = (i->opcode->signature >> (3 + 3 * n)) & 7;
      if (sig != OPCODE_SIG_TYPE_V) continue;
      Value* value = i->srcs[n].value;
      if (!value->IsConstant() && (!value->def || value->def->block != block))
        return Reject(rejected_at, __LINE__);
    }
    body.push_back(i);
  }
  // Small scalar memory loops benefit from the same grouping: their context
  // loads/stores and CTR update become one SSA chain over four ordered copies.
  // Bound their size separately to avoid expanding large arithmetic bodies.
  const bool scalar_memory = allow_scalar_memory && !vector_count &&
                             source_count <= 24 && body.size() <= 128;
  if (!store_ctr || source_count > 96 ||
      (vector_count < 8 && !scalar_memory) || !memory_stores)
    return Reject(rejected_at, __LINE__);
  // The only removable context boundary must belong to the final CTR chain.
  if (counter_barrier && counter_barrier->next != load_ctr) return Reject(rejected_at, __LINE__);
  for (Instr* i : body)
    if (i->opcode == &OPCODE_CONTEXT_BARRIER_info && i != counter_barrier)
      return Reject(rejected_at, __LINE__);
  // Check dominance/order before mutation: each nonconstant input must have
  // already been defined within the body, so cloning cannot fail halfway.
  std::unordered_map<Value*, Value*> available;
  for (Instr* i : body) {
    for (unsigned n = 0; n != 3; ++n) {
      auto sig = (i->opcode->signature >> (3 + 3 * n)) & 7;
      if (sig == OPCODE_SIG_TYPE_V && !i->srcs[n].value->IsConstant() &&
          !available.count(i->srcs[n].value)) return Reject(rejected_at, __LINE__);
    }
    if (i->dest) available.emplace(i->dest, i->dest);
  }
  if (!available.count(branch->src1.value)) return Reject(rejected_at, __LINE__);
  // All external entries must be explicit branch operands. This includes
  // aliases of the block's label, but excludes the original self-edge.
  for (auto b = builder->first_block(); b; b = b->next) {
    if (b == block) continue;
    for (auto i = b->instr_head; i; i = i->next) {
      if ((i->opcode == &OPCODE_BRANCH_info && i->src1.label->block == block) ||
          ((i->opcode == &OPCODE_BRANCH_TRUE_info ||
            i->opcode == &OPCODE_BRANCH_FALSE_info) &&
           i->src2.label->block == block)) entries.push_back(i);
    }
  }
  unsigned incoming = 0;
  for (auto e = block->incoming_edge_head; e; e = e->incoming_next)
    if (e->src != block) ++incoming;
  if (entries.empty() || entries.size() != incoming) return Reject(rejected_at, __LINE__);

  auto guard = builder->NewLabel();
  auto fast = builder->NewLabel();
  auto slow = branch->src2.label;
  auto done = exit->src1.label;
  builder->MarkLabel(guard);
  builder->SourceOffset(guest_pc);
  Value* ctr = builder->Truncate(builder->LoadContext(ctr_offset, INT64_TYPE),
                                 INT32_TYPE);
  builder->BranchTrue(builder->CompareUGE(ctr, builder->LoadConstantUint32(4)),
                      fast);
  builder->Branch(slow);
  builder->MarkLabel(fast);
  Block* fast_block = builder->current_block();
  Value* keep_looping = nullptr;
  for (unsigned iteration = 0; iteration != 4; ++iteration) {
    std::unordered_map<Value*, Value*> values;
    for (Instr* original : body) {
      if (original == counter_barrier && iteration != 3) continue;
      Instr* clone = builder->AllocateInstruction();
      clone->block = fast_block;
      clone->opcode = original->opcode;
      clone->flags = original->flags;
      clone->backend_flags = 0;
      clone->ordinal = UINT32_MAX;
      clone->dest = original->dest ? builder->AllocValue(original->dest->type)
                                   : nullptr;
      if (clone->dest) {
        clone->dest->def = clone;
        values.emplace(original->dest, clone->dest);
      }
      for (unsigned n = 0; n != 3; ++n) {
        clone->srcs[n].value = nullptr;
        clone->srcs_use[n] = nullptr;
        auto sig = (original->opcode->signature >> (3 + 3 * n)) & 7;
        if (sig == OPCODE_SIG_TYPE_V) {
          Value* value = original->srcs[n].value;
          clone->set_srcN(value->IsConstant() ? value : values.at(value), n);
        } else clone->srcs[n] = original->srcs[n];
      }
      Append(builder, fast_block, clone);
    }
    keep_looping = values.at(branch->src1.value);
  }
  builder->BranchTrue(keep_looping, guard, branch->flags);
  builder->Branch(done, exit->flags);
  for (Instr* entry : entries) {
    if (entry->opcode == &OPCODE_BRANCH_info) entry->src1.label = guard;
    else entry->src2.label = guard;
  }
  // Newly appended conditional blocks need explicit fallthrough edges before
  // ControlFlowAnalysis and safepoint injection see them.
  builder->Finalize();
  return true;
}
}  // namespace counted_loop_detail
}  // namespace xe::cpu::compiler::passes
#endif
