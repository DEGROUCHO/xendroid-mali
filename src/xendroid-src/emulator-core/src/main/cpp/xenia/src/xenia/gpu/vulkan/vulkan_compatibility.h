/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_COMPATIBILITY_H_
#define XENIA_GPU_VULKAN_VULKAN_COMPATIBILITY_H_

#include "xenia/base/cvar.h"

DECLARE_bool(vulkan_compatibility_rendering);

namespace xe {
namespace gpu {
namespace vulkan {

// The render target -> texture shortcuts (textures sampled straight from render
// targets, direct host resolves, resolve copies, color surface resolves,
// in-pass transfers) are on only while vulkan_compatibility_rendering is off.
// Some drivers (Mali-G615 r44p1) output a black image with them; with the
// switch on, resolves take the plain compute path.
inline bool RenderTargetShortcut(bool cvar_value) {
  return cvar_value && !cvars::vulkan_compatibility_rendering;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_COMPATIBILITY_H_
