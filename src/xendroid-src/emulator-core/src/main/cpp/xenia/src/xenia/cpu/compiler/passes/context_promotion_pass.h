/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_CONTEXT_PROMOTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_CONTEXT_PROMOTION_PASS_H_

#include <cmath>
#include <cstdint>
#include <vector>

#include "xenia/base/platform.h"
#include "xenia/cpu/compiler/compiler_pass.h"

#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#include <llvm/ADT/BitVector.h>
#pragma warning(pop)
#else
#include <llvm/ADT/BitVector.h>
#endif  // XE_COMPILER_MSVC

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Builds bounded four-iteration blocks before context promotion and safepoint
// injection. No cross-block SSA or changes to the scalar remainder path.
class CountedLoopUnrollPass : public CompilerPass {
 public:
  bool Run(hir::HIRBuilder* builder) override;
};

class ContextPromotionPass : public CompilerPass {
 public:
  ContextPromotionPass();
  virtual ~ContextPromotionPass() override;

  bool Initialize(Compiler* compiler) override;

  bool Run(hir::HIRBuilder* builder) override;

 private:
  struct TrackedContextValue {
    uint32_t offset;
    uint32_t size;
    hir::Value* value;
  };
  using ContextState = std::vector<TrackedContextValue>;

  void PromoteBlock(hir::Block* block,
                    const ContextState* entry_state = nullptr,
                    ContextState* exit_state = nullptr,
                    bool forward_context_ssa = false,
                    bool has_fallthrough_successor = false,
                    uint32_t* cross_block_reused_loads = nullptr);
  void RemoveDeadStoresBlock(hir::Block* block);

  // Range-keyed value tracking: a tracked value recorded at `offset` covers
  // every byte of [offset, offset + size).
  hir::Value* LookupTrackedValue(uint32_t offset, uint32_t size,
                                 hir::TypeName type);
  void TrackValue(uint32_t offset, uint32_t size, hir::Value* value);
  void InvalidateTrackedRange(uint32_t offset, uint32_t size);

 private:
  // Indexed by base byte offset into the context: the tracked SSA value
  // whose range starts there, and that range's size in bytes.
  std::vector<hir::Value*> context_values_;
  std::vector<uint8_t> context_value_size_;
  // Indexed by byte offset: the base offset of the tracked value covering
  // this byte. Only meaningful while the corresponding validity bit is set.
  std::vector<uint32_t> context_value_base_;
  // Byte-granular: bit b is set iff some tracked value's range covers b.
  llvm::BitVector context_validity_;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_CONTEXT_PROMOTION_PASS_H_
