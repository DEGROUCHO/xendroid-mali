/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_MACHINE_INFO_H_
#define XENIA_CPU_BACKEND_MACHINE_INFO_H_

#include <cstdint>

namespace xe {
namespace cpu {
namespace backend {

struct MachineInfo {
  bool supports_extended_load_store;
  bool supports_word_rotate_mask = false;

  struct RegisterSet {
    enum Types {
      INT_TYPES = (1 << 1),
      FLOAT_TYPES = (1 << 2),
      VEC_TYPES = (1 << 3),
    };
    uint8_t id;
    char name[4];
    uint32_t types;
    uint32_t count;
    // Registers a function may keep out of allocation for its pinned values
    // (OPCODE_LOAD_PINNED): pinned value n lives in the register with index
    // pinned_top - 1 - n. 0 = this set has none.
    uint32_t pinned_top;
  } register_sets[8];
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_MACHINE_INFO_H_
