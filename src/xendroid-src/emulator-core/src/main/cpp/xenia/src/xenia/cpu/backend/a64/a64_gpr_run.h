#ifndef XENIA_CPU_BACKEND_A64_A64_GPR_RUN_H_
#define XENIA_CPU_BACKEND_A64_A64_GPR_RUN_H_

#include <cstdint>

#include "xenia/cpu/hir/instr.h"

namespace xe::cpu::backend::a64 {

// a64_gpr_save_restore_runs: __savegprlr / __restgprlr style runs.
//
// Save:     vN = load_context +ctx          Restore:  vN = load_offset.swap base, disp
//           store_offset.swap base, disp, vN          store_context +ctx, vN
// repeated with ctx and disp both advancing by 8, every vN read by its pair
// alone. Two (or four) registers then move as one byte-reversed NEON vector
// instead of a load, a rev, an address add and a store each.
struct GprRun {
  uint32_t pairs = 0;           // number of (load, store) pairs, even
  uint32_t context_offset = 0;  // of the first register
  const hir::Instr* first_memory = nullptr;  // carries base and displacement
  const hir::Instr* last = nullptr;          // last instruction of the run
};

inline bool MatchGprRun(const hir::Instr* load, bool save, GprRun* run) {
  constexpr uint32_t kMaxPairs = 32;
  const hir::Value* base = nullptr;
  int64_t displacement = 0;
  uint64_t context_offset = 0;
  uint32_t pairs = 0;
  const hir::Instr* last_even = nullptr;
  const hir::Instr* first_memory = nullptr;
  for (const hir::Instr* cur = load; cur && pairs < kMaxPairs;) {
    const hir::Instr* store = cur->next;
    if (!store || store->block != load->block || !cur->dest ||
        cur->dest->type != hir::INT64_TYPE || !cur->dest->use_head ||
        cur->dest->use_head->next || cur->dest->use_head->instr != store) {
      break;
    }
    const hir::Instr* memory = save ? store : cur;
    const hir::Instr* context = save ? cur : store;
    if (save) {
      if (cur->opcode != &hir::OPCODE_LOAD_CONTEXT_info ||
          store->opcode != &hir::OPCODE_STORE_OFFSET_info ||
          store->src3.value != cur->dest) {
        break;
      }
    } else {
      if (cur->opcode != &hir::OPCODE_LOAD_OFFSET_info ||
          store->opcode != &hir::OPCODE_STORE_CONTEXT_info ||
          store->src2.value != cur->dest) {
        break;
      }
    }
    const hir::Value* memory_base = memory->src1.value;
    const hir::Value* memory_disp = memory->src2.value;
    if (memory->flags != hir::LOAD_STORE_BYTE_SWAP || !memory_base ||
        memory_base->IsConstant() || memory_base->type != hir::INT64_TYPE ||
        memory_base == cur->dest || !memory_disp ||
        !memory_disp->IsConstant() || memory_disp->type != hir::INT64_TYPE) {
      break;
    }
    const uint64_t this_context = static_cast<uint64_t>(context->src1.offset);
    const int64_t this_disp = memory_disp->constant.i64;
    if (!pairs) {
      base = memory_base;
      displacement = this_disp;
      context_offset = this_context;
      first_memory = memory;
      // add xN, ctx, #imm12 and the scaled Q offsets below.
      if (context_offset > 0xFFF) {
        break;
      }
    } else if (memory_base != base ||
               this_disp != displacement + int64_t(pairs) * 8 ||
               this_context != context_offset + uint64_t(pairs) * 8) {
      break;
    }
    ++pairs;
    if (!(pairs & 1)) {
      last_even = store;
    }
    cur = store->next;
  }
  pairs &= ~1u;
  if (pairs < 2) {
    return false;
  }
  run->pairs = pairs;
  run->context_offset = static_cast<uint32_t>(context_offset);
  run->first_memory = first_memory;
  run->last = last_even;
  return true;
}

// x1 = host address of the first guest slot, x2 = address of the first
// context register. Clobbers v0, v1.
template <typename Emitter>
void EmitGprRunCopy(Emitter& e, uint32_t pairs, bool save) {
  using namespace Xbyak_aarch64;
  const XReg from = save ? e.x2 : e.x1;
  const XReg to = save ? e.x1 : e.x2;
  uint32_t offset = 0;
  for (uint32_t vectors = pairs / 2; vectors;) {
    if (vectors >= 2) {
      e.ldp(QReg(0), QReg(1), ptr(from, static_cast<int32_t>(offset)));
      e.rev64(VReg(0).b16, VReg(0).b16);
      e.rev64(VReg(1).b16, VReg(1).b16);
      e.stp(QReg(0), QReg(1), ptr(to, static_cast<int32_t>(offset)));
      offset += 32;
      vectors -= 2;
    } else {
      e.ldr(QReg(0), ptr(from, static_cast<int32_t>(offset)));
      e.rev64(VReg(0).b16, VReg(0).b16);
      e.str(QReg(0), ptr(to, static_cast<int32_t>(offset)));
      offset += 16;
      vectors -= 1;
    }
  }
}

}  // namespace xe::cpu::backend::a64

#endif  // XENIA_CPU_BACKEND_A64_A64_GPR_RUN_H_
