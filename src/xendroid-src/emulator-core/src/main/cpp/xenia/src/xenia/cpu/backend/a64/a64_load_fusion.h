#ifndef XENIA_CPU_BACKEND_A64_A64_LOAD_FUSION_H_
#define XENIA_CPU_BACKEND_A64_A64_LOAD_FUSION_H_
#include "xenia/cpu/hir/instr.h"
namespace xe::cpu::backend::a64 {
// Match after allocation. No intervening instruction means the future dest
// can only alias operands already consumed by the load. Extra uses/spills,
// source markers, blocks, tracing, and unrecognized flags fail closed.
inline const hir::Instr* FindLoadZeroExtendFusion(const hir::Instr* load,
                                                 bool enabled) {
  if (!enabled || !load || !load->block || !load->dest ||
      (load->dest->type != hir::INT8_TYPE &&
       load->dest->type != hir::INT16_TYPE &&
       load->dest->type != hir::INT32_TYPE) ||
      (load->opcode != &hir::OPCODE_LOAD_info &&
       load->opcode != &hir::OPCODE_LOAD_OFFSET_info) ||
      (load->flags & ~hir::LOAD_STORE_BYTE_SWAP)) return nullptr;
  const auto* extend = load->next;
  if (!extend || extend->block != load->block ||
      extend->opcode != &hir::OPCODE_ZERO_EXTEND_info || extend->flags ||
      extend->src1.value != load->dest || !extend->dest ||
      (extend->dest->type != hir::INT16_TYPE &&
       extend->dest->type != hir::INT32_TYPE &&
       extend->dest->type != hir::INT64_TYPE) ||
      hir::GetTypeSize(extend->dest->type) <= hir::GetTypeSize(load->dest->type) ||
      !extend->dest->reg.set || extend->dest->reg.index < 0 ||
      extend->dest->reg.index >= extend->dest->reg.set->count ||
      !load->dest->use_head || load->dest->use_head->next ||
      load->dest->use_head->instr != extend) return nullptr;
  return extend;
}
inline const hir::Instr* FindTruncateStoreFusion(const hir::Instr* truncate,
                                                 bool enabled) {
  if (!enabled || !truncate || !truncate->block ||
      truncate->opcode != &hir::OPCODE_TRUNCATE_info || truncate->flags ||
      !truncate->dest || !truncate->src1.value ||
      truncate->src1.value->type != hir::INT64_TYPE ||
      truncate->src1.value->IsConstant() ||
      (truncate->dest->type != hir::INT8_TYPE &&
       truncate->dest->type != hir::INT16_TYPE &&
       truncate->dest->type != hir::INT32_TYPE)) return nullptr;
  const auto* store = truncate->next;
  if (!store || store->block != truncate->block ||
      (store->opcode != &hir::OPCODE_STORE_info &&
       store->opcode != &hir::OPCODE_STORE_OFFSET_info) ||
      (store->flags & ~hir::LOAD_STORE_BYTE_SWAP)) return nullptr;
  const auto* stored_value = store->opcode == &hir::OPCODE_STORE_info
                                ? store->src2.value : store->src3.value;
  const auto* source = truncate->src1.value;
  if (stored_value != truncate->dest ||
      !truncate->dest->use_head || truncate->dest->use_head->next ||
      truncate->dest->use_head->instr != store ||
      !source->reg.set || source->reg.index < 0 ||
      source->reg.index >= source->reg.set->count) return nullptr;
  return store;
}
}
#endif
