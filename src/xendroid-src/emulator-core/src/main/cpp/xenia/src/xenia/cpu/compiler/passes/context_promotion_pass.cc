/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/context_promotion_pass.h"
#include "xenia/cpu/compiler/passes/counted_loop_unroll.h"
#include "xenia/cpu/compiler/passes/control_flow_analysis_pass.h"

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/apu/apu_flags.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

DECLARE_bool(debug);
DEFINE_bool(log_counted_loop_unroll_rejects, false,
            "Log rejected vector CTR loops with the matcher source line.", "CPU");

DEFINE_bool(a64_counted_loop_unroll, true,
            "Group four iterations of bounded call-free VMX CTR loops in one "
            "HIR block, keeping the scalar remainder and guest exit state. "
            "Experimental; disabled with debug or context promotion off.", "CPU");
UPDATE_from_bool(a64_counted_loop_unroll, 2026, 10, 5, 12, false);
DEFINE_bool(a64_scalar_memory_loop_unroll, true,
            "Group four ordered iterations of small call-free scalar CTR "
            "memory loops, retaining the short-count and wrapping fallback. "
            "Requires a64_counted_loop_unroll.", "CPU");

DEFINE_bool(store_all_context_values, false,
            "Don't strip dead context stores to aid in debugging.", "CPU");

DEFINE_bool(a64_forward_context_ssa, true,
            "Forward exact PPCContext values across processed forward CFG "
            "edges in AArch64 translation. Experimental; disabled by default.",
            "CPU");

DEFINE_bool(a64_abi_volatile_cr_dse, true,
            "Whole-function context DSE treats the ABI-volatile CR fields "
            "(CR0, CR1, CR5-CR7) as dead at linked guest calls and returns. "
            "Needs a64_context_global_dse.",
            "CPU");

DEFINE_bool(a64_context_global_dse, true,
            "Eliminate PPCContext stores overwritten before observation on "
            "every CFG path, independently of cross-block SSA forwarding.", "CPU");

DEFINE_bool(full_optimization_even_with_debug, false,
            "For developer use to analyze the quality of the generated code, "
            "not intended for actual debugging of the code",
            "CPU");

DEFINE_bool(
    context_promote_vec128, true,
    "Promote VMX (VEC128) context loads/stores to SSA values and strip dead "
    "VEC128 context stores, letting the backend keep vectors in host "
    "registers. This restores upstream behavior from before 2026-01-25 "
    "(upstream excluded VEC128 to work around undiagnosed instability in "
    "some games; validity tracking is now range-keyed with overlap "
    "invalidation, which removes the offset-aliasing hazard that plausibly "
    "caused it). Disabling trades performance for current upstream's "
    "conservative behavior: every vector register access round-trips "
    "through guest context memory. Forced off by disable_context_promotion.",
    "CPU");

DEFINE_bool(context_promote_vec128_constants, true,
            "Bisect aid: with context_promote_vec128, also forward constant "
            "vectors stored to the context (makes vector operands constant: "
            "folds and the backend constant-operand paths).", "CPU");
DEFINE_bool(context_promote_vec128_loads, true,
            "Bisect aid: with context_promote_vec128, reuse vector context "
            "loads/stores as SSA values.", "CPU");
DEFINE_bool(context_vec128_dse, true,
            "Bisect aid: with context_promote_vec128, strip dead vector "
            "context stores (block-local and whole-function).", "CPU");

DECLARE_bool(disable_context_promotion);
DECLARE_bool(savestate_enable);

DEFINE_bool(a64_dse_through_preempt, true,
            "Whole-function context dead-store elimination looks through "
            "preemption safepoints (loop heads): stores every path overwrites "
            "before reading are dropped even across a loop back-edge.",
            "a64");
DEFINE_bool(context_transparent_atomics, true,
            "Atomic compare-exchanges, reserved loads/stores and memory "
            "barriers neither read nor write PPCContext: keep promoted "
            "context values and dead-store facts across them.",
            "CPU");

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Volatile for the guest memory they order or modify, never touching
// PPCContext (they lower to inline code or to helpers working on the backend
// context alone).
static bool IsContextTransparent(const hir::OpcodeInfo* opcode) {
  return cvars::context_transparent_atomics &&
         (opcode == &hir::OPCODE_ATOMIC_COMPARE_EXCHANGE_info ||
          opcode == &hir::OPCODE_RESERVED_LOAD_info ||
          opcode == &hir::OPCODE_RESERVED_STORE_info ||
          opcode == &hir::OPCODE_MEMORY_BARRIER_info ||
          opcode == &hir::OPCODE_LOAD_BARRIER_info);
}


// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Block;
using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using xe::cpu::hir::Value;

bool CountedLoopUnrollPass::Run(HIRBuilder* builder) {
  if (!cvars::a64_counted_loop_unroll || cvars::debug ||
      cvars::store_all_context_values || cvars::disable_context_promotion ||
      !cvars::context_promote_vec128 || !builder->last_block()) {
    return true;
  }
  // At most one expanded loop per function bounds code-cache growth and makes
  // this first version independent of stale CFG edges after a transformation.
  // Preserve the established large VMX-loop priority. A short scalar alignment
  // prefix must not consume the one-expansion budget before a vector body.
  const unsigned searches = cvars::a64_scalar_memory_loop_unroll ? 2 : 1;
  for (unsigned search = 0; search < searches; ++search) {
  for (Block* block = builder->first_block(); block; block = block->next) {
    uint32_t rejected_at = 0;
    if (!counted_loop_detail::Unroll(
            builder, block, uint32_t(offsetof(ppc::PPCContext, ctr)),
            &rejected_at, search != 0)) {
      if (cvars::log_counted_loop_unroll_rejects) {
        unsigned vectors = 0;
        uint32_t pc = 0;
        bool self_edge = false;
        for (Instr* i = block->instr_head; i; i = i->next) {
          if (i->opcode == &OPCODE_SOURCE_OFFSET_info && !pc)
            pc = uint32_t(i->src1.offset);
          if (i->dest && i->dest->type == VEC128_TYPE) ++vectors;
          if (i->opcode == &OPCODE_BRANCH_TRUE_info &&
              i->src2.label->block == block) self_edge = true;
        }
        if (self_edge && vectors >= 8)
          XELOGI("CountedLoopUnrollReject: guest={:08X} matcher_line={}",
                 pc, rejected_at);
      }
      continue;
    }
    uint32_t guest_pc = 0;
    for (Instr* i = block->instr_head; i; i = i->next) {
      if (i->opcode == &OPCODE_SOURCE_OFFSET_info) {
        guest_pc = uint32_t(i->src1.offset);
        break;
      }
    }
    XELOGI("CountedLoopUnroll: guest={:08X} factor=4", guest_pc);
    ControlFlowAnalysisPass cfg;
    return cfg.Run(builder);
  }
  }
  return true;
}

ContextPromotionPass::ContextPromotionPass() : CompilerPass() {}

ContextPromotionPass::~ContextPromotionPass() {}

bool ContextPromotionPass::Initialize(Compiler* compiler) {
  if (!CompilerPass::Initialize(compiler)) {
    return false;
  }

  // This is a terrible implementation.
  context_values_.resize(sizeof(ppc::PPCContext));
  context_value_size_.resize(sizeof(ppc::PPCContext));
  context_value_base_.resize(sizeof(ppc::PPCContext));
  context_validity_.resize(static_cast<uint32_t>(sizeof(ppc::PPCContext)));

  return true;
}

bool ContextPromotionPass::Run(HIRBuilder* builder) {
  // Like mem2reg, but because context memory is unaliasable it's easier to
  // check and convert LoadContext/StoreContext into value operations.
  // Example of load->value promotion:
  //   v0 = load_context +100
  //   store_context +200, v0
  //   v1 = load_context +100  <-- replace with v1 = v0
  //   store_context +200, v1
  //
  // It'd be possible in this stage to also remove redundant context stores:
  // Example of dead store elimination:
  //   store_context +100, v0  <-- removed due to following store
  //   store_context +100, v1
  // This is more generally done by DSE, however if it could be done here
  // instead as it may be faster (at least on the block-level).

  // The default path remains block-local. The opt-in path carries only exact
  // range/type/value facts over forward CFG edges whose predecessors have all
  // already been processed. It never guesses across a backedge or unknown
  // predecessor, and it does not synthesize PHIs. Cross-block store elimination is
  // independently guarded by a64_context_global_dse after local promotion.
#if XE_ARCH_ARM64
  const bool forward_context_ssa =
      cvars::a64_forward_context_ssa && !cvars::debug &&
      !cvars::store_all_context_values && !cvars::disable_context_promotion;
#else
  // The guarded cross-block register allocator path is AArch64-only.
  const bool forward_context_ssa = false;
#endif
  // Store elimination does not introduce cross-block SSA values or extend
  // register live ranges. Keep it independent of the experimental forwarding
  // allocator so its all-path proof can run with the stable local allocator.
#if XE_ARCH_ARM64
  const bool global_context_dse =
      cvars::a64_context_global_dse && !cvars::debug &&
      !cvars::store_all_context_values && !cvars::disable_context_promotion;
#else
  const bool global_context_dse = false;
#endif
  uint32_t cross_block_reused_loads = 0;
  auto block = builder->first_block();
  if (!forward_context_ssa) {
    while (block) {
      PromoteBlock(block);
      block = block->next;
    }
  } else {
    constexpr size_t kMaxForwardSSABlocks = 2048;
    constexpr size_t kMaxForwardSSAGraphEdges = 1u << 20;
    size_t block_count = 0;
    for (Block* current = block; current; current = current->next) {
      ++block_count;
    }
    bool cfg_supported = block_count <= kMaxForwardSSABlocks;
    std::vector<Block*> blocks;
    std::unordered_map<const Block*, size_t> block_indices;
    if (cfg_supported) {
      blocks.reserve(block_count);
      block_indices.reserve(block_count);
      for (Block* current = block; current; current = current->next) {
        block_indices.emplace(current, blocks.size());
        blocks.push_back(current);
      }
    }

    // Reconstruct the effective CFG for the forwarding proof. Finalize normally
    // materializes fallthroughs as branches; the local graph mirrors the
    // validator's successor rules and is fully bounded before rewriting loads.
    std::vector<std::vector<size_t>> predecessors;
    std::vector<std::vector<size_t>> successors;
    if (cfg_supported) {
      predecessors.resize(blocks.size());
      successors.resize(blocks.size());
      size_t graph_edges = 0;
      auto add_edge = [&predecessors, &successors, &graph_edges,
                       kMaxForwardSSAGraphEdges](size_t source, size_t dest) {
        // Count attempted edges like ValidationPass, including duplicates.
        if (++graph_edges > kMaxForwardSSAGraphEdges) {
          return false;
        }
        auto& outgoing = successors[source];
        if (std::find(outgoing.begin(), outgoing.end(), dest) !=
            outgoing.end()) {
          return true;
        }
        outgoing.push_back(dest);
        predecessors[dest].push_back(source);
        return true;
      };
      for (size_t source_index = 0; source_index < blocks.size();
           ++source_index) {
        Block* source = blocks[source_index];
        for (Instr* instr = source->instr_head; instr; instr = instr->next) {
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
            // Unknown control flow cannot establish a safe cross-block fact.
            cfg_supported = false;
            break;
          }
          if (target) {
            if (!target->block) {
              cfg_supported = false;
              break;
            }
            auto dest = block_indices.find(target->block);
            if (dest == block_indices.end() ||
                !add_edge(source_index, dest->second)) {
              cfg_supported = false;
              break;
            }
          }
        }
        if (!cfg_supported) {
          break;
        }

        Instr* tail = source->instr_tail;
        const bool unconditional_transfer =
            tail && (tail->opcode == &OPCODE_BRANCH_info ||
                     tail->opcode == &OPCODE_RETURN_info ||
                     ((tail->opcode == &OPCODE_CALL_info ||
                       tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                      (tail->flags & CALL_TAIL)));
        if (!unconditional_transfer && source_index + 1 < blocks.size() &&
            !add_edge(source_index, source_index + 1)) {
          cfg_supported = false;
          break;
        }
      }
    }

    if (!cfg_supported || blocks.empty()) {
      // Preserve the original block-local behavior for CFG forms this proof
      // does not understand or exceeds its analysis bounds.
      for (Block* current = builder->first_block(); current;
           current = current->next) {
        PromoteBlock(current);
      }
    } else {
      // Cross-block reuse is limited to code reachable from the function entry;
      // disconnected blocks remain independent roots with empty incoming facts.
      std::vector<uint8_t> reachable(blocks.size(), 0);
      std::vector<size_t> pending{0};
      reachable[0] = 1;
      while (!pending.empty()) {
        const size_t source_index = pending.back();
        pending.pop_back();
        for (size_t dest : successors[source_index]) {
          if (!reachable[dest]) {
            reachable[dest] = 1;
            pending.push_back(dest);
          }
        }
      }

      std::vector<ContextState> exit_states(blocks.size());
      std::vector<uint8_t> processed(blocks.size(), 0);
      for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        Block* current = blocks[block_index];
        const auto& predecessors_for_block = predecessors[block_index];
        std::vector<size_t> predecessor_indices;
        bool all_predecessors_forward =
            reachable[block_index] && !predecessors_for_block.empty();
        for (size_t source_index : predecessors_for_block) {
          if (source_index >= block_index || !processed[source_index]) {
            all_predecessors_forward = false;
            break;
          }
          predecessor_indices.push_back(source_index);
        }

        ContextState entry_state;
        if (all_predecessors_forward && !predecessor_indices.empty()) {
          const ContextState& first_state =
              exit_states[predecessor_indices.front()];
          entry_state.reserve(first_state.size());
          for (const auto& candidate : first_state) {
            if (!candidate.value) {
              continue;
            }
            bool agrees_on_every_predecessor = true;
            for (size_t predecessor_index : predecessor_indices) {
              const auto& predecessor_state = exit_states[predecessor_index];
              auto found = std::find_if(
                  predecessor_state.begin(), predecessor_state.end(),
                  [&candidate](const TrackedContextValue& incoming) {
                    return incoming.offset == candidate.offset &&
                           incoming.size == candidate.size &&
                           incoming.value == candidate.value &&
                           incoming.value &&
                           incoming.value->type == candidate.value->type;
                  });
              if (found == predecessor_state.end()) {
                agrees_on_every_predecessor = false;
                break;
              }
            }
            if (agrees_on_every_predecessor) {
              entry_state.push_back(candidate);
            }
          }
        }

        ContextState exit_state;
        const Instr* tail = current->instr_tail;
        const bool unconditional_transfer =
            tail && (tail->opcode == &OPCODE_BRANCH_info ||
                     tail->opcode == &OPCODE_RETURN_info ||
                     ((tail->opcode == &OPCODE_CALL_info ||
                       tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                      (tail->flags & CALL_TAIL)));
        const bool has_fallthrough_successor =
            block_index + 1 < blocks.size() && !unconditional_transfer;
        PromoteBlock(current, &entry_state, &exit_state, true,
                     has_fallthrough_successor, &cross_block_reused_loads);
        exit_states[block_index] = std::move(exit_state);
        processed[block_index] = 1;
      }

      // Keep diagnostics bounded: one summary for at most the first 64 functions
      // with an actual forwarded load, never per block or per instruction.
      if (cross_block_reused_loads) {
        static std::atomic<uint32_t> logged_functions{0};
        uint32_t log_slot =
            logged_functions.fetch_add(1, std::memory_order_relaxed);
        if (log_slot < 64) {
          uint32_t guest_address = 0;
          bool have_guest_address = false;
          for (Block* current : blocks) {
            for (Instr* i = current->instr_head; i; i = i->next) {
              if (i->opcode == &OPCODE_SOURCE_OFFSET_info) {
                guest_address = static_cast<uint32_t>(i->src1.offset);
                have_guest_address = true;
                break;
              }
            }
            if (have_guest_address) {
              break;
            }
          }
          XELOGI("ContextPromotionSSA: guest={:08X} reused_loads={}",
                 guest_address, cross_block_reused_loads);
        }
      }
    }
  }

  // Remove all dead stores.
  // This will break debugging as we can't recover this information when
  // trying to extract stack traces/register values, so we don't do that.
  if (cvars::full_optimization_even_with_debug ||
      (!cvars::debug && !cvars::store_all_context_values)) {
    block = builder->first_block();
    while (block) {
      RemoveDeadStoresBlock(block);
      block = block->next;
    }

    if (global_context_dse) {
      // Whole-function DSE. A byte is set only when every path
      // from this point overwrites it before a context observation. All input
      // states start empty (least fixed point), and no instruction is changed
      // until the bounded analysis has converged.
      constexpr size_t kMaxGlobalDSEBlocks = 2048;
      constexpr size_t kMaxGlobalDSEEdges = 1u << 20;
      constexpr size_t kMaxGlobalDSEWorklistPops = 1u << 14;
      using OverwrittenWords = std::vector<uint64_t>;
      const size_t context_size = sizeof(ppc::PPCContext);
      const size_t context_words = (context_size + 63) / 64;
      auto for_range_chunks = [](size_t offset, size_t size, auto callback) {
        const size_t end = offset + size;
        while (offset < end) {
          const size_t word = offset / 64;
          const unsigned bit = static_cast<unsigned>(offset % 64);
          const size_t chunk = std::min(end - offset, size_t(64 - bit));
          const uint64_t mask =
              chunk == 64
                  ? ~uint64_t{0}
                  : ((uint64_t{1} << static_cast<unsigned>(chunk)) - 1)
                        << bit;
          callback(word, mask);
          offset += chunk;
        }
      };
      auto range_set = [&for_range_chunks](OverwrittenWords& state,
                                           size_t offset, size_t size) {
        for_range_chunks(offset, size, [&state](size_t word, uint64_t mask) {
          state[word] |= mask;
        });
      };
      auto range_clear = [&for_range_chunks](OverwrittenWords& state,
                                             size_t offset, size_t size) {
        for_range_chunks(offset, size, [&state](size_t word, uint64_t mask) {
          state[word] &= ~mask;
        });
      };
      auto range_all_set = [&for_range_chunks](const OverwrittenWords& state,
                                               size_t offset, size_t size) {
        bool all_set = true;
        for_range_chunks(offset, size,
                         [&state, &all_set](size_t word, uint64_t mask) {
                           all_set = all_set && (state[word] & mask) == mask;
                         });
        return all_set;
      };
      auto run_global_dse = [&]() -> size_t {
        std::vector<Block*> blocks;
        std::unordered_map<const Block*, size_t> block_indices;
        for (Block* current = builder->first_block(); current;
             current = current->next) {
          if (blocks.size() == kMaxGlobalDSEBlocks) {
            return 0;
          }
          block_indices.emplace(current, blocks.size());
          blocks.push_back(current);
        }
        if (blocks.empty()) {
          return 0;
        }

        auto is_call = [](const auto* opcode) {
          return opcode == &OPCODE_CALL_info ||
                 opcode == &OPCODE_CALL_TRUE_info ||
                 opcode == &OPCODE_CALL_INDIRECT_info ||
                 opcode == &OPCODE_CALL_INDIRECT_TRUE_info ||
                 opcode == &OPCODE_CALL_EXTERN_info;
        };
        auto is_return = [](const auto* opcode) {
          return opcode == &OPCODE_RETURN_info ||
                 opcode == &OPCODE_RETURN_TRUE_info;
        };
        auto is_conditional_branch = [](const auto* opcode) {
          return opcode == &OPCODE_BRANCH_TRUE_info ||
                 opcode == &OPCODE_BRANCH_FALSE_info;
        };
        auto branch_target = [&block_indices](Instr* instr,
                                              size_t* target_index) {
          Label* target = nullptr;
          if (instr->opcode == &OPCODE_BRANCH_info) {
            target = instr->src1.label;
          } else if (instr->opcode == &OPCODE_BRANCH_TRUE_info ||
                     instr->opcode == &OPCODE_BRANCH_FALSE_info) {
            target = instr->src2.label;
          } else {
            return false;
          }
          if (!target || !target->block) {
            return false;
          }
          auto found = block_indices.find(target->block);
          if (found == block_indices.end()) {
            return false;
          }
          *target_index = found->second;
          return true;
        };
        auto valid_context_range = [context_size](uint64_t offset,
                                                   size_t size) {
          return size && offset <= context_size &&
                 size <= context_size - static_cast<size_t>(offset);
        };

        std::vector<std::vector<size_t>> predecessors(blocks.size());
        std::vector<std::vector<size_t>> successors(blocks.size());
        size_t graph_edges = 0;
        auto add_edge = [&](size_t source, size_t destination) {
          if (++graph_edges > kMaxGlobalDSEEdges) {
            return false;
          }
          auto& outgoing = successors[source];
          if (std::find(outgoing.begin(), outgoing.end(), destination) !=
              outgoing.end()) {
            return true;
          }
          outgoing.push_back(destination);
          predecessors[destination].push_back(source);
          return true;
        };

        // Reconstruct every branch target, including transfers in the middle
        // of a block. Any unknown branch or malformed context range disables
        // the global pass for this function.
        for (size_t source = 0; source < blocks.size(); ++source) {
          Block* current = blocks[source];
          for (Instr* instr = current->instr_head; instr;
               instr = instr->next) {
            const auto* opcode = instr->opcode;
            if ((opcode->flags & OPCODE_FLAG_BRANCH) &&
                opcode != &OPCODE_BRANCH_info &&
                opcode != &OPCODE_BRANCH_TRUE_info &&
                opcode != &OPCODE_BRANCH_FALSE_info && !is_call(opcode) &&
                !is_return(opcode)) {
              return 0;
            }
            size_t destination = 0;
            if ((opcode == &OPCODE_BRANCH_info ||
                 is_conditional_branch(opcode)) &&
                (!branch_target(instr, &destination) ||
                 !add_edge(source, destination))) {
              return 0;
            }
            if (opcode == &OPCODE_LOAD_CONTEXT_info) {
              const size_t size = GetTypeSize(instr->dest->type);
              if (!valid_context_range(instr->src1.offset, size)) {
                return 0;
              }
            } else if (opcode == &OPCODE_STORE_CONTEXT_info) {
              if (!instr->src2.value ||
                  !valid_context_range(instr->src1.offset,
                                       GetTypeSize(instr->src2.value->type))) {
                return 0;
              }
            }
          }
          Instr* tail = current->instr_tail;
          const bool unconditional_transfer =
              tail && (tail->opcode == &OPCODE_BRANCH_info ||
                       tail->opcode == &OPCODE_RETURN_info ||
                       ((tail->opcode == &OPCODE_CALL_info ||
                         tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                        (tail->flags & CALL_TAIL)));
          if (!unconditional_transfer && source + 1 < blocks.size() &&
              !add_edge(source, source + 1)) {
            return 0;
          }
        }

        const bool promote_vec128 =
            cvars::context_promote_vec128 && cvars::context_vec128_dse &&
            !cvars::disable_context_promotion;
        // a64_abi_volatile_cr_dse: CR0, CR1 and CR5-CR7 are volatile in the
        // Xbox 360 ABI. Only `bl`-style linked calls and returns count as ABI
        // boundaries; plain tail jumps may be the analyzer splitting one
        // function, where the flags are still read on the other side.
        const bool abi_volatile_cr = cvars::a64_abi_volatile_cr_dse;
        OverwrittenWords volatile_cr(context_words, 0);
        range_set(volatile_cr, offsetof(ppc::PPCContext, cr0), 4);
        range_set(volatile_cr, offsetof(ppc::PPCContext, cr1), 4);
        range_set(volatile_cr, offsetof(ppc::PPCContext, cr5), 4);
        range_set(volatile_cr, offsetof(ppc::PPCContext, cr6), 4);
        range_set(volatile_cr, offsetof(ppc::PPCContext, cr7), 4);
        auto is_abi_boundary = [&](const Instr* instr) {
          const auto* opcode = instr->opcode;
          if (is_return(opcode)) {
            return true;
          }
          if (opcode == &OPCODE_CALL_info || opcode == &OPCODE_CALL_TRUE_info) {
            return !(instr->flags & CALL_TAIL);
          }
          if (opcode == &OPCODE_CALL_INDIRECT_info ||
              opcode == &OPCODE_CALL_INDIRECT_TRUE_info) {
            return !(instr->flags & CALL_TAIL) ||
                   (instr->flags & CALL_POSSIBLE_RETURN);
          }
          return false;
        };
        auto evaluate_block = [&](size_t block_index,
                                  const std::vector<OverwrittenWords>& in,
                                  OverwrittenWords* state,
                                  std::vector<Instr*>* dead_stores) {
          state->resize(context_words);
          std::fill(state->begin(), state->end(), 0);
          Block* current = blocks[block_index];
          Instr* tail = current->instr_tail;
          const bool unconditional_transfer =
              tail && (tail->opcode == &OPCODE_BRANCH_info ||
                       tail->opcode == &OPCODE_RETURN_info ||
                       ((tail->opcode == &OPCODE_CALL_info ||
                         tail->opcode == &OPCODE_CALL_INDIRECT_info) &&
                        (tail->flags & CALL_TAIL)));
          if (!unconditional_transfer && block_index + 1 < blocks.size()) {
            *state = in[block_index + 1];
          }

          for (Instr* instr = current->instr_tail; instr;
               instr = instr->prev) {
            const auto* opcode = instr->opcode;
            if (opcode == &OPCODE_CONTEXT_BARRIER_info) {
              // This opcode lowers to a no-op and does not expose context.
              continue;
            }
            if (opcode == &OPCODE_BRANCH_info) {
              size_t target = 0;
              if (!branch_target(instr, &target)) {
                std::fill(state->begin(), state->end(), 0);
              } else {
                *state = in[target];
              }
            } else if (is_conditional_branch(opcode)) {
              size_t target = 0;
              if (!branch_target(instr, &target)) {
                std::fill(state->begin(), state->end(), 0);
              } else {
                const OverwrittenWords& target_state = in[target];
                for (size_t word = 0; word < context_words; ++word) {
                  (*state)[word] &= target_state[word];
                }
              }
            } else if (abi_volatile_cr && is_abi_boundary(instr)) {
              // The callee of a linked call never reads the caller's volatile
              // CR fields, and nothing reads them from a returning function:
              // every earlier store to them is dead on this path, and
              // everything else is observed.
              const bool conditional =
                  opcode == &OPCODE_CALL_TRUE_info ||
                  opcode == &OPCODE_CALL_INDIRECT_TRUE_info ||
                  opcode == &OPCODE_RETURN_TRUE_info;
              for (size_t word = 0; word < context_words; ++word) {
                (*state)[word] = conditional
                                     ? ((*state)[word] & volatile_cr[word])
                                     : volatile_cr[word];
              }
            } else if (opcode == &OPCODE_CHECK_PREEMPT_info &&
                       cvars::a64_dse_through_preempt &&
                       !cvars::savestate_enable) {
              // A fiber switch resumes this very point with this very
              // context, so a slot overwritten before its next read on every
              // path is as unobservable to the guest thread as without the
              // safepoint. (The safepoint still ends value forwarding: its
              // cold path is a host call.)
              // Snapshot mode observes canonical context at this point, so
              // it must retain the stores preceding the safepoint. The mode
              // is fixed before JIT compilation, never toggled on live code.
            } else if (is_call(opcode) || is_return(opcode) ||
                       opcode == &OPCODE_CHECK_PREEMPT_info ||
                       opcode == &OPCODE_LOAD_MMIO_info ||
                       opcode == &OPCODE_STORE_MMIO_info ||
                       ((opcode->flags & OPCODE_FLAG_VOLATILE) &&
                        !IsContextTransparent(opcode))) {
              std::fill(state->begin(), state->end(), 0);
            } else if (opcode == &OPCODE_LOAD_CONTEXT_info) {
              const size_t offset = static_cast<size_t>(instr->src1.offset);
              const size_t size = GetTypeSize(instr->dest->type);
              range_clear(*state, offset, size);
            } else if (opcode == &OPCODE_STORE_CONTEXT_info) {
              const Value* value = instr->src2.value;
              const size_t offset = static_cast<size_t>(instr->src1.offset);
              const size_t size = GetTypeSize(value->type);
              if (!promote_vec128 && value->type == VEC128_TYPE) {
                continue;
              }
              if (range_all_set(*state, offset, size) && dead_stores) {
                dead_stores->push_back(instr);
              }
              range_set(*state, offset, size);
            }
          }
        };

        // Least-fixed-point iteration over must-overwritten byte facts.
        std::vector<OverwrittenWords> in_states(
            blocks.size(), OverwrittenWords(context_words, 0));
        OverwrittenWords scratch;
        scratch.reserve(context_words);
        std::vector<size_t> worklist;
        worklist.reserve(blocks.size());
        std::vector<uint8_t> queued(blocks.size(), 0);
        // Backward facts converge in one sweep on a forward acyclic CFG.
        for (size_t remaining = blocks.size(); remaining; --remaining) {
          const size_t index = remaining - 1;
          worklist.push_back(index);
          queued[index] = 1;
        }
        const size_t worklist_pop_budget = std::min(
            kMaxGlobalDSEWorklistPops,
            std::max<size_t>(128, blocks.size() * 8));
        size_t worklist_head = 0;
        size_t worklist_pops = 0;
        while (worklist_head < worklist.size()) {
          if (++worklist_pops > worklist_pop_budget) {
            return 0;
          }
          const size_t index = worklist[worklist_head++];
          queued[index] = 0;
          evaluate_block(index, in_states, &scratch, nullptr);
          if (scratch == in_states[index]) {
            continue;
          }
          std::copy(scratch.begin(), scratch.end(), in_states[index].begin());
          for (size_t predecessor : predecessors[index]) {
            if (!queued[predecessor]) {
              queued[predecessor] = 1;
              worklist.push_back(predecessor);
            }
          }
        }

        // Collect first, then mutate. The converged IN states are immutable
        // throughout this pass, including for merged blocks with early exits.
        std::vector<Instr*> dead_stores;
        for (size_t index = 0; index < blocks.size(); ++index) {
          evaluate_block(index, in_states, &scratch, &dead_stores);
        }
        for (Instr* dead_store : dead_stores) {
          dead_store->UnlinkAndNOP();
        }
        return dead_stores.size();
      };
      run_global_dse();
    }
  }

  return true;
}

// Range-keyed validity invariants (shared by the helpers below):
// - context_validity_ bit b is set iff some tracked value's range covers
//   byte b;
// - context_value_base_[b] is that value's base offset (meaningful only
//   while bit b is set);
// - context_value_size_[base] is its size in bytes, and whenever the bit at
//   `base` is set, every bit of [base, base + size) is set - ranges are
//   tracked and invalidated only as whole units.
// Stale Value*/size/base entries are gated solely by the validity bits, so
// the wholesale validity.reset() on volatile instructions remains correct.

void ContextPromotionPass::InvalidateTrackedRange(uint32_t offset,
                                                  uint32_t size) {
  auto& validity = context_validity_;
  const uint32_t end = offset + size;
  uint32_t b = offset;
  while (b < end) {
    if (validity.test(b)) {
      // Drop the whole tracked value covering this byte, whether it sits
      // narrower inside the new range or is wider and merely overlaps it.
      const uint32_t base = context_value_base_[b];
      const uint32_t base_size = context_value_size_[base];
      validity.reset(base, base + base_size);
      b = base + base_size;
    } else {
      ++b;
    }
  }
}

void ContextPromotionPass::TrackValue(uint32_t offset, uint32_t size,
                                      Value* value) {
  InvalidateTrackedRange(offset, size);
  context_values_[offset] = value;
  context_value_size_[offset] = static_cast<uint8_t>(size);
  for (uint32_t b = offset; b < offset + size; ++b) {
    context_value_base_[b] = offset;
  }
  context_validity_.set(offset, offset + size);
}

Value* ContextPromotionPass::LookupTrackedValue(uint32_t offset, uint32_t size,
                                                TypeName type) {
  // Fold only on an exact range and type match: same base offset, same size
  // (full range coverage is implied by the whole-unit invariant above), and
  // same type. The type check is insurance against same-base-offset unions
  // in PPCContext (e.g. cr0.value as INT32 vs cr0.cr0_lt as INT8).
  if (!context_validity_.test(offset) ||
      context_value_base_[offset] != offset ||
      context_value_size_[offset] != size) {
    return nullptr;
  }
  Value* value = context_values_[offset];
  if (!value || value->type != type) {
    return nullptr;
  }
  return value;
}

void ContextPromotionPass::PromoteBlock(
    Block* block, const ContextState* entry_state, ContextState* exit_state,
    bool forward_context_ssa, bool has_fallthrough_successor,
    uint32_t* cross_block_reused_loads) {
  auto& validity = context_validity_;
  validity.reset();
  ContextState remaining_entry_facts;
  if (entry_state) {
    remaining_entry_facts = *entry_state;
    for (const auto& tracked : *entry_state) {
      if (tracked.value) {
        TrackValue(tracked.offset, tracked.size, tracked.value);
      }
    }
  }
  auto forget_incoming_range = [&remaining_entry_facts](uint32_t offset,
                                                        uint32_t size) {
    const uint64_t end = uint64_t(offset) + size;
    remaining_entry_facts.erase(
        std::remove_if(remaining_entry_facts.begin(),
                       remaining_entry_facts.end(),
                       [offset, end](const TrackedContextValue& tracked) {
                         const uint64_t tracked_end =
                             uint64_t(tracked.offset) + tracked.size;
                         return uint64_t(tracked.offset) < end &&
                                uint64_t(offset) < tracked_end;
                       }),
        remaining_entry_facts.end());
  };
  const bool promote_vec128 =
      cvars::context_promote_vec128 && cvars::context_promote_vec128_loads &&
      !cvars::disable_context_promotion;

  bool has_transfer_snapshot = false;
  auto capture_transfer_snapshot = [&]() {
    if (!exit_state) {
      return;
    }
    ContextState snapshot;
    for (uint32_t offset = 0; offset < context_validity_.size(); ++offset) {
      if (!validity.test(offset) || context_value_base_[offset] != offset) {
        continue;
      }
      const uint32_t size = context_value_size_[offset];
      Value* value = context_values_[offset];
      if (size && value) {
        snapshot.push_back({offset, size, value});
        offset += size - 1;
      }
    }
    if (!has_transfer_snapshot) {
      *exit_state = std::move(snapshot);
      has_transfer_snapshot = true;
      return;
    }
    exit_state->erase(
        std::remove_if(exit_state->begin(), exit_state->end(),
                       [&snapshot](const TrackedContextValue& candidate) {
                         return std::find_if(
                                    snapshot.begin(), snapshot.end(),
                                    [&candidate](
                                        const TrackedContextValue& transfer) {
                                      return transfer.offset == candidate.offset &&
                                             transfer.size == candidate.size &&
                                             transfer.value == candidate.value &&
                                             transfer.value && candidate.value &&
                                             transfer.value->type ==
                                                 candidate.value->type;
                                    }) == snapshot.end();
                       }),
        exit_state->end());
  };

  Instr* i = block->instr_head;
  while (i) {
    auto next = i->next;
    const auto* opcode = i->opcode;
    const bool forward_branch =
        forward_context_ssa &&
        (opcode == &OPCODE_BRANCH_info ||
         opcode == &OPCODE_BRANCH_TRUE_info ||
         opcode == &OPCODE_BRANCH_FALSE_info);
    const bool forward_context_barrier =
        forward_context_ssa &&
        (opcode == &OPCODE_CALL_info ||
         opcode == &OPCODE_CALL_TRUE_info ||
         opcode == &OPCODE_CALL_INDIRECT_info ||
         opcode == &OPCODE_CALL_INDIRECT_TRUE_info ||
         opcode == &OPCODE_CALL_EXTERN_info ||
         opcode == &OPCODE_CHECK_PREEMPT_info ||
         opcode == &OPCODE_LOAD_MMIO_info ||
         opcode == &OPCODE_STORE_MMIO_info);
    if (((opcode->flags & OPCODE_FLAG_VOLATILE) && !forward_branch &&
         !IsContextTransparent(opcode)) ||
        forward_context_barrier) {
      // Calls, safepoints, MMIO, and volatile operations may expose or mutate
      // PPCContext; do not carry a forwarded fact across them.
      validity.reset();
      remaining_entry_facts.clear();
    } else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const TypeName type = i->dest->type;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(type));
      Value* previous_value = LookupTrackedValue(offset, size, type);
      if (previous_value) {
        if (forward_context_ssa && cross_block_reused_loads) {
          auto forwarded = std::find_if(
              remaining_entry_facts.begin(), remaining_entry_facts.end(),
              [offset, size, type, previous_value](
                  const TrackedContextValue& tracked) {
                return tracked.offset == offset && tracked.size == size &&
                       tracked.value == previous_value && tracked.value &&
                       tracked.value->type == type;
              });
          if (forwarded != remaining_entry_facts.end()) {
            ++*cross_block_reused_loads;
            // Count only the first load made redundant by this incoming fact;
            // later loads are already covered by block-local promotion.
            forget_incoming_range(offset, size);
          }
        }
        // Legit previous value, reuse.
        i->opcode = &hir::OPCODE_ASSIGN_info;
        i->set_src1(previous_value);
      } else if (promote_vec128 || type != TypeName::VEC128_TYPE) {
        // Track the loaded value so later loads of this range reuse it.
        // (Loads don't modify memory, but TrackValue evicts any tracked
        // value overlapping a range we now know with a different shape.)
        if (!remaining_entry_facts.empty()) {
          forget_incoming_range(offset, size);
        }
        TrackValue(offset, size, i->dest);
      } else if (!remaining_entry_facts.empty()) {
        forget_incoming_range(offset, size);
      }
    } else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      Value* value = i->src2.value;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(value->type));
      if (!remaining_entry_facts.empty()) {
        forget_incoming_range(offset, size);
      }
      if ((promote_vec128 || value->type != TypeName::VEC128_TYPE) &&
          (cvars::context_promote_vec128_constants ||
           value->type != TypeName::VEC128_TYPE || !value->IsConstant())) {
        // Track the stored value for later loads. TrackValue first drops
        // every tracked value overlapping the bytes this store clobbers.
        TrackValue(offset, size, value);
      } else {
        // VEC128 promotion disabled: the value isn't tracked, but the store
        // still clobbers these bytes, so overlapping tracked values die.
        InvalidateTrackedRange(offset, size);
      }
    }
    if (forward_context_ssa && forward_branch) {
      // A merged block may contain several internal branches. Only facts
      // present at every transfer site can be used by any successor.
      capture_transfer_snapshot();
    }
    i = next;
  }

  if (exit_state) {
    if (forward_context_ssa) {
      if (has_fallthrough_successor) {
        // The final linear path contributes an edge only when the CFG has an
        // implicit successor; include it in the same conservative meet.
        capture_transfer_snapshot();
      }
      if (!has_transfer_snapshot) {
        exit_state->clear();
      }
    } else {
      exit_state->clear();
      for (uint32_t offset = 0; offset < context_validity_.size(); ++offset) {
        if (!validity.test(offset) || context_value_base_[offset] != offset) {
          continue;
        }
        uint32_t size = context_value_size_[offset];
        Value* value = context_values_[offset];
        if (size && value) {
          exit_state->push_back({offset, size, value});
          offset += size - 1;
        }
      }
    }
  }
}

void ContextPromotionPass::RemoveDeadStoresBlock(Block* block) {
  // In this walk a validity bit means "this byte is fully overwritten by a
  // later store in this block, with no barrier or load in between".
  auto& validity = context_validity_;
  validity.reset();
  const bool promote_vec128 =
      cvars::context_promote_vec128 && cvars::context_vec128_dse &&
      !cvars::disable_context_promotion;

  // Walk backwards and mark byte ranges that are written to.
  // If a store's whole range was already written to later, it is dead.
  Instr* i = block->instr_tail;
  while (i) {
    Instr* prev = i->prev;
    if ((i->opcode->flags & (OPCODE_FLAG_VOLATILE | OPCODE_FLAG_BRANCH)) &&
        !IsContextTransparent(i->opcode)) {
      // Volatile instruction - requires all context values be flushed.
      validity.reset();
    } else if (i->opcode == &OPCODE_LOAD_CONTEXT_info) {
      // A load that survived PromoteBlock is a live use of these bytes:
      // earlier stores overlapping it must be kept. (PromoteBlock folds
      // exact-match loads to values, but a load whose range/type mismatches
      // the tracked value - or that follows a volatile reset - survives.)
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(i->dest->type));
      validity.reset(offset, offset + size);
    } else if (i->opcode == &OPCODE_STORE_CONTEXT_info) {
      const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
      const Value* value = i->src2.value;
      const uint32_t size = static_cast<uint32_t>(GetTypeSize(value->type));
      if (promote_vec128 || value->type != TypeName::VEC128_TYPE) {
        bool fully_overwritten = true;
        for (uint32_t b = offset; b < offset + size; ++b) {
          if (!validity.test(b)) {
            fully_overwritten = false;
            break;
          }
        }
        if (!fully_overwritten) {
          // Some byte still escapes - keep the store; all bytes it writes
          // are now dead for earlier stores.
          validity.set(offset, offset + size);
        } else {
          // Every byte is overwritten later. Remove this store.
          i->UnlinkAndNOP();
        }
      }
    }
    i = prev;
  }
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
