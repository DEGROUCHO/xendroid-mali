/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XNET_STARTUP_PARAMS_H_
#define XENIA_KERNEL_XAM_XNET_STARTUP_PARAMS_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xe {
namespace kernel {
namespace xam {

// XNetStartupParams as the guest lays them out.
inline constexpr size_t kStartupParamsSize = 13;
using StartupParams = std::array<uint8_t, kStartupParamsSize>;

// What XNet runs with for the parameters a title passed, none when empty. Zero
// fields take xam's defaults and the others are clamped to its limits.
inline StartupParams NormalizeStartupParams(std::span<const uint8_t> params) {
  constexpr StartupParams kDefaults{0, 0, 8, 32, 16, 16, 8, 32, 64, 2, 3, 8, 2};
  constexpr StartupParams kMinimums{0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  constexpr StartupParams kMaximums{0,   255, 232, 232, 63,  63, 255,
                                    255, 255, 255, 255, 255, 255};
  StartupParams result = kDefaults;
  // The extended struct starts with the same fields, any other size is
  // ignored.
  if (params.size() >= kStartupParamsSize &&
      (params[0] == kStartupParamsSize || params[0] == 0x4C)) {
    for (size_t i = 1; i < kStartupParamsSize; ++i) {
      if (params[i]) {
        result[i] = std::clamp(params[i], kMinimums[i], kMaximums[i]);
      }
    }
  }
  // Retail xam clears the flags that turn security off.
  result[1] &= 0xF6;
  result[0] = kStartupParamsSize;
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XNET_STARTUP_PARAMS_H_
