/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/compiler/passes/loop_register_pinning_pass.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"

DEFINE_bool(a64_loop_register_pinning, true,
            "Keep frequently accessed guest registers in stable host registers "
            "throughout a function, materializing context at observations.",
            "CPU");
DEFINE_int32(a64_loop_pinned_gprs, 5,
             "Maximum integer context registers pinned per function (0-6).",
             "CPU");
UPDATE_from_int32(a64_loop_pinned_gprs, 2026, 10, 11, 17, 3);
DEFINE_int32(a64_loop_pinned_vecs, 3,
             "Maximum floating-point/vector context registers pinned per "
             "function (0-14: v31 downwards, never more than half of the "
             "allocator's vector set).",
             "CPU");
DEFINE_bool(a64_pinning_with_savestate, true,
            "Keep guest register pinning on when savestate_enable is on. Every "
            "point a snapshot can observe (safepoint yield, call, kernel "
            "export, function exit) already flushes the dirty pins to the "
            "context first, and a restored thread re-enters through a function "
            "entry that loads them again.",
            "CPU");
UPDATE_from_bool(a64_pinning_with_savestate, 2026, 11, 15, 1, false);
DECLARE_bool(savestate_enable);
DECLARE_bool(debug);
DECLARE_bool(disable_context_promotion);
DECLARE_bool(store_all_context_values);

namespace xe::cpu::compiler::passes {
using namespace xe::cpu::hir;

namespace {
constexpr size_t kMaxBlocks = 2048;

bool IsVec(TypeName type) {
  return type == FLOAT64_TYPE || type == VEC128_TYPE;
}

// Stack recovery reads r1 and LR directly. Only architectural data registers
// and CTR can be cached; backend/scheduler fields must always stay in memory.
bool IsDataRegister(uint64_t offset, TypeName type) {
  using ppc::PPCContext;
  if (type == INT64_TYPE) {
    return offset == offsetof(PPCContext, ctr) ||
           (offset >= offsetof(PPCContext, r) &&
            offset < offsetof(PPCContext, r) + sizeof(PPCContext::r) &&
            (offset - offsetof(PPCContext, r)) % 8 == 0 &&
            offset != offsetof(PPCContext, r[1]));
  }
  const size_t start =
      type == FLOAT64_TYPE ? offsetof(PPCContext, f) : offsetof(PPCContext, v);
  const size_t size =
      type == FLOAT64_TYPE ? sizeof(PPCContext::f) : sizeof(PPCContext::v);
  return IsVec(type) && offset >= start && offset < start + size &&
         (offset - start) % GetTypeSize(type) == 0;
}

bool IsInternalBranch(const Instr* i) {
  return i->opcode == &OPCODE_BRANCH_info ||
         i->opcode == &OPCODE_BRANCH_TRUE_info ||
         i->opcode == &OPCODE_BRANCH_FALSE_info;
}

// CONTEXT_BARRIER is a frontend marker, emitted as no code on A64. The
// actual following operation (call/trap/MMIO) determines observation.
bool ObservesContext(const Instr* i) {
  const auto* op = i->opcode;
  if (IsInternalBranch(i) || op == &OPCODE_CHECK_PREEMPT_info ||
      op == &OPCODE_ATOMIC_COMPARE_EXCHANGE_info ||
      op == &OPCODE_RESERVED_LOAD_info || op == &OPCODE_RESERVED_STORE_info ||
      op == &OPCODE_MEMORY_BARRIER_info || op == &OPCODE_LOAD_BARRIER_info) {
    return false;
  }
  return (op->flags & OPCODE_FLAG_VOLATILE) || op == &OPCODE_LOAD_MMIO_info ||
         op == &OPCODE_STORE_MMIO_info;
}

bool LeavesFunction(const Instr* i) {
  return i->opcode == &OPCODE_RETURN_info ||
         ((i->opcode == &OPCODE_CALL_info ||
           i->opcode == &OPCODE_CALL_INDIRECT_info) &&
          (i->flags & CALL_TAIL));
}

// These exits do not observe context on the continuing path. src3.offset
// holds their dirty-slot mask; the backend flushes only after the condition.
bool IsConditionalExit(const Instr* i) {
  return i->opcode == &OPCODE_RETURN_TRUE_info ||
         ((i->opcode == &OPCODE_CALL_TRUE_info ||
           i->opcode == &OPCODE_CALL_INDIRECT_TRUE_info) &&
          (i->flags & CALL_TAIL));
}

bool IsColdObservation(const Instr* i) {
  return i->opcode == &OPCODE_CHECK_PREEMPT_info ||
         i->opcode == &OPCODE_SPIN_BACKOFF_info;
}

void MoveAfter(Instr* i, Instr* other) {
  if (i->prev == other) {
    return;
  }
  Block* old = i->block;
  if (i->prev) {
    i->prev->next = i->next;
  } else {
    old->instr_head = i->next;
  }
  if (i->next) {
    i->next->prev = i->prev;
  } else {
    old->instr_tail = i->prev;
  }
  i->block = other->block;
  i->prev = other;
  i->next = other->next;
  if (i->next) {
    i->next->prev = i;
  } else {
    i->block->instr_tail = i;
  }
  other->next = i;
}

struct Slot {
  uint64_t offset;
  TypeName type;
  int64_t benefit = -1;  // Entry initialization.
  bool written = false;
  bool loaded = false;
  uint8_t loop_depth = 0;
  bool usable = true;
};
}  // namespace

LoopRegisterPinningPass::LoopRegisterPinningPass(
    const backend::MachineInfo* machine_info) {
  for (const auto& set : machine_info->register_sets) {
    const uint32_t offered = std::min(set.pinned_top, set.count / 2);
    if (set.types & backend::MachineInfo::RegisterSet::INT_TYPES) {
      available_int_ = offered;
    } else if (set.types & (backend::MachineInfo::RegisterSet::FLOAT_TYPES |
                            backend::MachineInfo::RegisterSet::VEC_TYPES)) {
      available_vec_ = offered;
    }
  }
}
LoopRegisterPinningPass::~LoopRegisterPinningPass() = default;

bool LoopRegisterPinningPass::Run(HIRBuilder* builder) {
  // Restored frames have context, but no pinned host state. Debuggers also
  // require every architectural update to be visible immediately.
  if (!cvars::a64_loop_register_pinning ||
      (cvars::savestate_enable && !cvars::a64_pinning_with_savestate) ||
      cvars::debug || cvars::disable_context_promotion ||
      cvars::store_all_context_values) {
    return true;
  }

  std::vector<Block*> blocks;
  std::unordered_map<Block*, size_t> indices;
  for (Block* b = builder->first_block(); b; b = b->next) {
    if (blocks.size() == kMaxBlocks) {
      return true;
    }
    indices.emplace(b, blocks.size());
    blocks.push_back(b);
  }
  if (blocks.empty()) {
    return true;
  }

  // Backward branch spans estimate hotness; correctness never depends on it.
  std::vector<uint8_t> depth(blocks.size(), 0);
  for (size_t n = 0; n < blocks.size(); ++n) {
    for (Instr* i = blocks[n]->instr_head; i; i = i->next) {
      if (!IsInternalBranch(i)) {
        continue;
      }
      Label* target =
          i->opcode == &OPCODE_BRANCH_info ? i->src1.label : i->src2.label;
      auto found = indices.find(target->block);
      if (found == indices.end()) {
        return true;
      }
      if (found->second <= n) {
        for (size_t k = found->second; k <= n; ++k) {
          depth[k] = std::min(unsigned(depth[k]) + 1, 3u);
        }
      }
    }
  }
  std::vector<Slot> slots;
  int64_t reload_cost = 0, flush_cost = 0;
  uint8_t call_depth = 0;
  for (size_t n = 0; n < blocks.size(); ++n) {
    int64_t weight = int64_t{1} << (3 * depth[n]);
    for (Instr* i = blocks[n]->instr_head; i; i = i->next) {
      if (ObservesContext(i) && !IsConditionalExit(i)) {
        flush_cost += weight;
        if (!LeavesFunction(i)) {
          reload_cost += weight;
          call_depth = std::max(call_depth, depth[n]);
        }
      }
      if (IsInternalBranch(i)) {
        Label* target =
            i->opcode == &OPCODE_BRANCH_info ? i->src1.label : i->src2.label;
        if (indices.at(target->block) <= n) {
          weight = std::max(int64_t{1}, weight / 8);
        }
      }
      const bool load = i->opcode == &OPCODE_LOAD_CONTEXT_info;
      if (!load && i->opcode != &OPCODE_STORE_CONTEXT_info) {
        continue;
      }
      const TypeName type = load ? i->dest->type : i->src2.value->type;
      auto found = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) {
        return s.offset == i->src1.offset && s.type == type;
      });
      if (found == slots.end()) {
        slots.push_back({i->src1.offset, type});
        found = slots.end() - 1;
        found->usable = IsDataRegister(found->offset, type);
      }
      found->benefit += weight;
      found->written |= !load;
      found->loaded |= load;
      found->loop_depth = std::max(found->loop_depth, depth[n]);
    }
  }
  // No differently sized/type-punned access may bypass the cached value.
  for (Slot& a : slots) {
    for (const Slot& b : slots) {
      if (&a != &b && a.offset < b.offset + GetTypeSize(b.type) &&
          b.offset < a.offset + GetTypeSize(a.type)) {
        a.usable = false;
      }
    }
    a.benefit -= reload_cost + (a.written ? flush_cost : 0);
  }
  std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
    return a.benefit != b.benefit ? a.benefit > b.benefit : a.offset < b.offset;
  });
  const uint32_t max_int = std::min(
      available_int_, uint32_t(std::clamp(cvars::a64_loop_pinned_gprs, 0, 6)));
  const uint32_t max_vec = std::min(
      available_vec_, uint32_t(std::clamp(cvars::a64_loop_pinned_vecs, 0, 14)));
  auto& pinned = builder->pinned_context_slots();
  uint32_t ints = 0, vecs = 0;
  for (const Slot& s : slots) {
    uint32_t& count = IsVec(s.type) ? vecs : ints;
    if (!s.usable || !s.loaded || s.benefit <= 0 ||
        count == (IsVec(s.type) ? max_vec : max_int)) {
      continue;
    }
    pinned.push_back({uint32_t(s.offset), s.type, count++, false});
  }
  if (pinned.empty()) {
    return true;
  }
  builder->set_pinned_counts(ints, vecs);

  using Mask = uint32_t;
  auto slot_mask = [&](const Instr* i) -> Mask {
    if (i->opcode != &OPCODE_LOAD_CONTEXT_info &&
        i->opcode != &OPCODE_STORE_CONTEXT_info) {
      return 0;
    }
    for (size_t n = 0; n < pinned.size(); ++n) {
      if (pinned[n].offset == i->src1.offset) {
        return Mask{1} << n;
      }
    }
    return 0;
  };
  auto target_index = [&](const Instr* i) {
    return indices.at(
        (i->opcode == &OPCODE_BRANCH_info ? i->src1.label : i->src2.label)
            ->block);
  };
  auto falls_through = [&](size_t n) {
    Instr* tail = blocks[n]->instr_tail;
    return n + 1 < blocks.size() &&
           (!tail ||
            (!LeavesFunction(tail) && tail->opcode != &OPCODE_BRANCH_info));
  };
  std::vector<std::vector<size_t>> predecessors(blocks.size());
  for (size_t n = 0; n < blocks.size(); ++n) {
    if (falls_through(n)) {
      predecessors[n + 1].push_back(n);
    }
    for (Instr* i = blocks[n]->instr_head; i; i = i->next) {
      if (IsInternalBranch(i)) {
        predecessors[target_index(i)].push_back(n);
      }
    }
  }

  // A bit means that some path has changed this slot since its last context
  // observation. Snapshot each branch at its instruction, not at block end:
  // merged blocks may contain stores after a conditional transfer.
  std::vector<Mask> dirty_in(blocks.size(), 0);
  std::unordered_map<const Instr*, Mask> dirty_before, live_after;
  std::deque<size_t> work;
  std::vector<bool> queued(blocks.size(), true);
  for (size_t n = 0; n < blocks.size(); ++n) {
    work.push_back(n);
  }
  while (!work.empty()) {
    const size_t n = work.front();
    work.pop_front();
    queued[n] = false;
    Mask dirty = dirty_in[n];
    auto propagate = [&](size_t to) {
      if ((dirty_in[to] | dirty) == dirty_in[to]) {
        return;
      }
      dirty_in[to] |= dirty;
      if (!queued[to]) {
        work.push_back(to);
        queued[to] = true;
      }
    };
    for (Instr* i = blocks[n]->instr_head; i; i = i->next) {
      if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
        dirty |= slot_mask(i);
      }
      if (ObservesContext(i) || IsColdObservation(i)) {
        dirty_before[i] = dirty;
      }
      // A conditional observer is flushed on both paths, so the fallthrough
      // state is clean too. Preempt is cold-only and cannot clear hot facts.
      if (ObservesContext(i) && !IsConditionalExit(i)) {
        dirty = 0;
      }
      if (IsInternalBranch(i)) {
        propagate(target_index(i));
      }
    }
    if (falls_through(n)) {
      propagate(n + 1);
    }
  }

  // Liveness includes the pending writes that observations must publish.
  // Observers kill incoming values: only future reads require a reload.
  // These finite bitset analyses are monotone, including loops/irreducible
  // CFGs.
  std::vector<Mask> live_in(blocks.size(), 0);
  auto scan_live = [&](size_t n, bool record) {
    Mask live = falls_through(n) ? live_in[n + 1] : 0;
    for (Instr* i = blocks[n]->instr_tail; i; i = i->prev) {
      if (IsInternalBranch(i)) {
        if (i->opcode == &OPCODE_BRANCH_info) {
          live = 0;
        }
        live |= live_in[target_index(i)];
      }
      if (ObservesContext(i) && !IsConditionalExit(i)) {
        if (LeavesFunction(i)) {
          live = 0;
        }
        if (record) {
          live_after[i] = live;
        }
        live = dirty_before.at(i);
      } else if (IsColdObservation(i) || IsConditionalExit(i)) {
        if (record) {
          live_after[i] = live;
        }
        live |= dirty_before.at(i);
      }
      if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
        live &= ~slot_mask(i);
      } else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
        live |= slot_mask(i);
      }
    }
    return live;
  };
  std::fill(queued.begin(), queued.end(), true);
  for (size_t n = blocks.size(); n; --n) {
    work.push_back(n - 1);
  }
  while (!work.empty()) {
    const size_t n = work.front();
    work.pop_front();
    queued[n] = false;
    const Mask live = scan_live(n, false);
    if (live == live_in[n]) {
      continue;
    }
    live_in[n] = live;
    for (size_t from : predecessors[n]) {
      if (!queued[from]) {
        work.push_back(from);
        queued[from] = true;
      }
    }
  }
  for (size_t n = 0; n < blocks.size(); ++n) {
    scan_live(n, true);
  }
  for (size_t n = 0; n < pinned.size(); ++n) {
    pinned[n].initialize = (live_in[0] & (Mask{1} << n)) != 0;
  }

  // A pin must also pay for its static synchronization sites. A frequency
  // score alone hides duplicated cold-path reloads in functions with calls.
  std::vector<uint32_t> accesses(pinned.size(), 0), syncs(pinned.size(), 0);
  for (size_t n = 0; n < pinned.size(); ++n) {
    const Mask bit = Mask{1} << n;
    syncs[n] = pinned[n].initialize;
    for (Block* b : blocks) {
      for (Instr* i = b->instr_head; i; i = i->next) {
        accesses[n] += (slot_mask(i) & bit) != 0;
        if (ObservesContext(i) || IsColdObservation(i)) {
          syncs[n] += (dirty_before.at(i) & bit) != 0;
          if (IsColdObservation(i) ||
              (!LeavesFunction(i) && !IsConditionalExit(i))) {
            syncs[n] += (live_after.at(i) & bit) != 0;
          }
        }
      }
    }
  }
  Mask kept = 0;
  for (size_t n = 0; n < pinned.size(); ++n) {
    const auto slot =
        std::find_if(slots.begin(), slots.end(), [&](const Slot& s) {
          return s.offset == pinned[n].offset && s.type == pinned[n].type;
        });
    // Repeated updates in nested loops amortize their cold safepoints even
    // when the cold copies outnumber the static body accesses.
    const bool loop_carried =
        slot->written && slot->loop_depth >= 2 &&
        (IsVec(slot->type) || slot->loop_depth > call_depth);
    if (accesses[n] >= syncs[n] || loop_carried) kept |= Mask{1} << n;
  }
  auto compact = [&](Mask mask) {
    Mask result = 0;
    uint32_t to = 0;
    for (size_t n = 0; n < pinned.size(); ++n) {
      if (kept & (Mask{1} << n)) {
        if (mask & (Mask{1} << n)) result |= Mask{1} << to;
        ++to;
      }
    }
    return result;
  };
  for (auto& entry : dirty_before) entry.second = compact(entry.second);
  for (auto& entry : live_after) entry.second = compact(entry.second);
  ints = vecs = 0;
  size_t to = 0;
  for (size_t n = 0; n < pinned.size(); ++n) {
    if (kept & (Mask{1} << n)) {
      auto s = pinned[n];
      s.index = IsVec(s.type) ? vecs++ : ints++;
      pinned[to++] = s;
    }
  }
  pinned.resize(to);
  builder->set_pinned_counts(ints, vecs);
  if (pinned.empty()) return true;

  for (Block* b : blocks) {
    for (Instr* i = b->instr_head; i;) {
      Instr* next = i->next;
      const bool load = i->opcode == &OPCODE_LOAD_CONTEXT_info;
      if (load || i->opcode == &OPCODE_STORE_CONTEXT_info) {
        for (const auto& s : pinned) {
          if (s.offset != i->src1.offset) {
            continue;
          }
          i->opcode =
              load ? &OPCODE_LOAD_PINNED_info : &OPCODE_STORE_PINNED_info;
          i->src1.offset = s.index;
          break;
        }
      } else if (IsColdObservation(i)) {
        i->src2.offset = dirty_before.at(i);
        i->src3.offset = live_after.at(i);
      } else if (IsConditionalExit(i)) {
        i->src3.offset = dirty_before.at(i);
      } else if (ObservesContext(i)) {
        // Keep the call-return constant adjacent to its call. A flush before
        // SET_RETURN_ADDRESS sees the same pins only if no intervening store
        // or observation could have changed them.
        Instr* before = i;
        for (Instr* scan = i->prev; scan; scan = scan->prev) {
          if (scan->opcode == &OPCODE_SET_RETURN_ADDRESS_info) {
            before = scan;
            break;
          }
          if (scan->opcode == &OPCODE_STORE_PINNED_info ||
              ObservesContext(scan) || IsColdObservation(scan) ||
              IsInternalBranch(scan)) {
            break;
          }
        }
        // The may-dirty meet plus backward liveness ensures that a pin is
        // initialized even on clean incoming paths to an unconditional flush.
        for (size_t n = 0; n < pinned.size(); ++n) {
          const auto& s = pinned[n];
          if (!(dirty_before.at(i) & (Mask{1} << n))) {
            continue;
          }
          Value* value = builder->LoadPinned(s.index, s.type);
          value->def->MoveBefore(before);
          builder->StoreContext(s.offset, value);
          value->use_head->instr->MoveBefore(before);
        }
        if (!LeavesFunction(i)) {
          Instr* after = i;
          for (size_t n = 0; n < pinned.size(); ++n) {
            const auto& s = pinned[n];
            if (!(live_after.at(i) & (Mask{1} << n))) {
              continue;
            }
            Value* value = builder->LoadContext(s.offset, s.type);
            MoveAfter(value->def, after);
            Instr* store = builder->StorePinned(s.index, value);
            MoveAfter(store, value->def);
            after = store;
          }
        }
      }
      i = next;
    }
  }
  return true;
}
}  // namespace xe::cpu::compiler::passes
