/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_FLOAT_MEMORY_DOMAIN_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_FLOAT_MEMORY_DOMAIN_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

// Keeps floating-point values that only travel between guest memory and the
// floating-point registers out of the integer registers
// (a64_float_memory_domain).
//
// The frontend reads a double as an integer load, a byte swap and a cast of
// the bits (and writes one the other way around); a single goes through
// single_bits_to_double / double_to_single_bits on integer bits. On the host
// that is a load into a general register, a byte reverse there and a move
// across to the vector registers - the move alone costs about as much as the
// load. Loads whose value is only ever taken as a float become float loads,
// and stores of bits that only come from a float become float stores: the
// backend reverses the bytes in the vector register.
class FloatMemoryDomainPass : public CompilerPass {
 public:
  FloatMemoryDomainPass();
  ~FloatMemoryDomainPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_FLOAT_MEMORY_DOMAIN_PASS_H_
