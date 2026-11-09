/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"

#include <dlfcn.h>
#include "xenia/gpu/vulkan/spirv_single_sample_patch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_set>


DECLARE_bool(savestate_enable);
DECLARE_bool(log_depth_materialize_callers);
DECLARE_bool(rt_cache_transfer_known_constants);

#include "third_party/glslang/SPIRV/GLSL.std.450.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/render_target_retirement.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_compatibility.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(accurate_resolve_number_formats);
DECLARE_bool(log_image_compression);
DECLARE_bool(rt_cache_ownership_claim_memo);
DECLARE_bool(rt_cache_skip_unused_stencil_transfers);
DECLARE_bool(rt_cache_color_draws_dirty_stencil);
DECLARE_bool(vulkan_dynamic_rendering);

DEFINE_bool(
    vulkan_texture_from_render_target, true,
    "Experimental R32 depth snapshots with compute only on demand. "
    "Only provably zero stencil, native 1x, full 2D extents are eligible. "
    "Requires coherent mapped shared memory. "
    "Unsupported cases use the old resolve. No STORAGE texture usage.",
    "Vulkan");
UPDATE_from_bool(vulkan_texture_from_render_target, 2026, 10, 24, 18, false);

DEFINE_bool(
    vulkan_rt_retirement, false,
    "Experimental fail-closed retirement of unowned Vulkan render targets. "
    "Requires tracked GPU uses, zero CPU/alias leases, and completed "
    "submissions; default off.",
    "Vulkan");

DEFINE_bool(
    vulkan_defer_new_depth_owner_transfers, true,
    "Defer ownership transfers for a proven depth-only clear until the next "
    "non-clear operation. Consecutive clears of the same target may remove "
    "more overwritten pixels from the transfer. Requires "
    "rt_cache_clear_new_depth_owners; fake occlusion queries only.",
    "Vulkan");
UPDATE_from_bool(vulkan_defer_new_depth_owner_transfers, 2026, 10, 5, 12, false);

DEFINE_path(
    vulkan_source_rt_capture_request, "",
    "One-shot file request for a raw pre-resolve color render-target capture. "
    "Enables transfer-source image usage for supported single-sample color "
    "targets when configured.",
    "Vulkan");

DEFINE_bool(
    vulkan_source_rt_precapture_at_depth_resolve, false,
    "MW2 diagnostic: while the source render-target capture request is armed, "
    "also capture (once) the color render target owning EDRAM tiles "
    "1060..1554 (pitch 13) at the 1024x600 depth resolve, i.e. right after "
    "the main scene pass, as source-rt-pre-frame-N.bin/.json.",
    "Vulkan");

DEFINE_int32(
    vulkan_source_rt_precapture_format, -1,
    "MW2 diagnostic: -1 captures the current owner in the pre-capture; a guest "
    "color format index (0 = k_8_8_8_8) captures the sibling host render target "
    "of the same EDRAM surface with that format instead.",
    "Vulkan");

DEFINE_bool(
    vulkan_in_pass_resolve_debug_read_usage, false,
    "DEBUG probe: declare shared memory as kRead rather than "
    "kGuestDrawReadWrite for guest passes even when local-read attachments "
    "are enabled. kRead covers COMPUTE/TRANSFER stages that "
    "kGuestDrawReadWrite does not.",
    "Vulkan");
DEFINE_int32(
    vulkan_in_pass_resolve_debug_reject, 0,
    "DEBUG probe bitmask: 1 = reject multisampled sources, 2 = reject "
    "single-sampled sources. For bisecting which resolve population "
    "carries an artifact.",
    "Vulkan");
DEFINE_int32(
    vulkan_in_pass_resolve_debug_exp_bias, 0,
    "DEBUG probe: add this to the destination exponent bias of in-pass "
    "resolves only. Nonzero must visibly change the image if their output "
    "is consumed; identical output means the data is not the conduit.",
    "Vulkan");
DEFINE_bool(
    vulkan_in_pass_resolve_debug_dump, false,
    "DEBUG probe: still dump the owning render targets to the EDRAM buffer "
    "when a resolve was performed in-pass, to test whether skipping the "
    "dump is what a title misses.",
    "Vulkan");
DEFINE_bool(
    vulkan_resolve_texture_copy, true,
    "Mirror complete identity texture resolves to a linear buffer, then copy "
    "their exact loader bits to the sampled image. Includes loader-exact depth "
    "resolves and valid partial depth updates. No texture STORAGE/alias usage. "
    "Experimental, disabled until device validation.",
    "Vulkan");
UPDATE_from_bool(vulkan_resolve_texture_copy, 2026, 10, 23, 19, false);

DEFINE_bool(
    vulkan_in_pass_transfers, true,
    "Queue compatible render-target ownership transfers for execution inside "
    "the guest draw render pass instead of breaking the pass to perform them. "
    "On tile-based GPUs (Adreno/Turnip) this avoids GMEM store/restore cycles, "
    "and for provably full-overwrite targets it also discards (DONT_CARE) the "
    "old attachment contents. Disable to restore the always-break behavior.",
    "Vulkan");

DEFINE_bool(
    vulkan_normalize_dontcare_keys, true,
    "Keep the loadOp discard bits out of framebuffer and pipeline cache keys "
    "under dynamic rendering, so toggling them cannot force a render pass "
    "break. Disable for A/B measurement of the break count.",
    "Vulkan");

DEFINE_bool(
    vulkan_consume_dontcare, true,
    "Treat a loadOp DONT_CARE as one-shot: clear it from the last update key "
    "once a pass has begun with it (or a draw was encoded into the open pass) "
    "and when queued transfers are materialized, so a pass reopened without a "
    "new Update loads instead of discarding what earlier draws wrote.",
    "Vulkan");
DEFINE_bool(
    vulkan_in_pass_resolve, false,
    "Prepare color attachments for in-pass EDRAM resolves with "
    "VK_KHR_dynamic_rendering_local_read: RENDERING_LOCAL_READ image layout "
    "and input-attachment usage on color render targets. Requires dynamic "
    "rendering and driver support; no effect otherwise.",
    "Vulkan");

DEFINE_bool(
    vulkan_in_pass_resolve_r32_float, false,
    "Deprecated compatibility option for configurations using the former "
    "typed-SFLOAT R32_FLOAT in-pass resolve. Ignored; bit-exact R32_FLOAT "
    "resolves require the experimental UINT draw path.",
    "Vulkan");
DEFINE_bool(
    vulkan_in_pass_resolve_r32_uint, false,
    "EXPERIMENT: draw a single-sample R32_FLOAT target through its R32_UINT "
    "alias and use a uint subpass input for exact k_32_FLOAT resolves. "
    "Requires no color blending, no resolve conversion, zero exponent bias, "
    "no destination swap, and vulkan_in_pass_resolve. Disabled by default.",
    "Vulkan");

DECLARE_int32(log_draw_overwrite_cutouts);
DEFINE_bool(
    vulkan_color_surface_resolve, true,
    "With vulkan_texture_from_render_target, also serve raw 8_8_8_8 resolves "
    "of 1x 8_8_8_8 render targets by drawing straight into the texture that "
    "reads them, writing guest memory only if something else reads it.",
    "GPU");
UPDATE_from_bool(vulkan_color_surface_resolve, 2026, 10, 24, 18, false);
DEFINE_bool(vulkan_resolve_full_overwrite_discard, true,
            "A resolve rewriting every byte of its destination range drops the "
            "older pending (not yet written back) resolves inside that range "
            "instead of first writing them back to guest memory.",
            "GPU");
DEFINE_bool(
    vulkan_lazy_constant_rows, true,
    "With rt_cache_defer_retarget_clears: clip a depth clear recorded in the "
    "guest pass to the rows the pass draws and keep the rest as pending "
    "constant rows, cleared for real only when something reads them, instead "
    "of storing the whole (EDRAM-range tall) render target every frame.",
    "GPU");
UPDATE_from_bool(vulkan_lazy_constant_rows, 2026, 10, 24, 12, false);
DEFINE_bool(
    vulkan_transfer_scissor_bounds, true,
    "Scissor ownership transfer draws to the bounds of their rectangles "
    "instead of the whole framebuffer, so the render pass area can shrink "
    "around them (on tile-based GPUs it bounds the tiles loaded and stored).",
    "GPU");
UPDATE_from_bool(vulkan_transfer_scissor_bounds, 2026, 10, 24, 12, false);
DEFINE_bool(
    rt_cache_defer_retarget_clears, true,
    "Record a retargeted depth clear (rt_cache_retarget_full_depth_clears) and "
    "the transfers around its rectangle inside the guest render pass of the "
    "next draw on that render target, instead of a pass of their own which on "
    "tile-based GPUs stores the cleared target and loads it right back.",
    "GPU");
UPDATE_from_bool(rt_cache_defer_retarget_clears, 2026, 10, 24, 12, false);
DEFINE_bool(
    rt_cache_clear_chain_in_pass, true,
    "When a clear chain (rt_cache_draw_clear_chains) ends while the guest "
    "render pass of its render target is still open, do its ownership "
    "transfers inside that pass instead of a pass of their own, which on "
    "tile-based GPUs stores the target and loads it back right away.",
    "GPU");
UPDATE_from_bool(rt_cache_clear_chain_in_pass, 2026, 10, 24, 12, false);
DEFINE_bool(
    rt_cache_draw_clear_chains, true,
    "Defer the ownership transfers into render targets that a clear draw (an "
    "exact screen-aligned rectangle writing every bit, see "
    "rt_cache_draw_overwrite_cutouts) overwrites, while the following draws "
    "keep clearing the same render targets, then transfer only the pixels "
    "around the union of the rectangles. The Xbox 360 clears surfaces in "
    "several rectangles (a strip, then the rest), so a single-draw cutout "
    "misses most of the area.",
    "GPU");
DEFINE_bool(
    rt_cache_stencil_bit_transfer_mask, true,
    "Without stencil export, write only the stencil bits that may be 1 in the "
    "source of an ownership transfer (tracked from stencil-writing draws, "
    "clears and earlier transfers) instead of always drawing all 8 bit passes "
    "over the transferred area.",
    "GPU");
// Defined with the device extension it needs (ui/vulkan/vulkan_device.cc).
DECLARE_bool(vulkan_msaa_single_sample_resolve);
DEFINE_bool(
    vulkan_msaa_single_sample_storage, true,
    "Host render target path: keep the images of 2x / 4x multisampled guest "
    "render targets single-sampled and rasterize one sample per pixel into "
    "them. Halves or quarters what every render pass loads and stores, and "
    "lets Mali compress them (AFBC is not applied to multisampled images). "
    "The guest-visible layout is unchanged: all samples of a pixel read the "
    "same value, so resolves return it as is. Loses the antialiasing of "
    "geometry edges. Latched at GPU initialization. On by default since "
    "2026-10-10: measured on a Mali-G720 in Call of Duty: Black Ops gameplay "
    "at 22 M GPU cycles and 11 M shaded quads per frame against 31 M and 21 M "
    "(32.6 against 25.1 FPS), and the GPU work per frame is what sets the "
    "frame rate once the device throttles.",
    "Vulkan");
UPDATE_from_bool(vulkan_msaa_single_sample_storage, 2026, 10, 10, 23, false);

DEFINE_bool(
    vulkan_same_depth_stencil_ops, false,
    "Give the depth and the stencil aspect of a guest render pass the same "
    "load and store operations (the one keeping more of the contents), the "
    "workaround of PPSSPP, ARMSX2 and hakuX for Mali drivers corrupting "
    "depth / stencil attachments whose two aspects have different load "
    "operations. Off: it didn't remove the Black Ops squares (BO-DS1, q612).",
    "Vulkan");
UPDATE_from_bool(vulkan_same_depth_stencil_ops, 2026, 10, 26, 18, true);
DEFINE_bool(
    vulkan_direct_host_resolve, true,
    "Resolve eligible (non-format-converting and packable format-converting) "
    "host render targets directly to guest memory with compute shaders instead "
    "of first dumping the host render target back through EDRAM. Avoids a "
    "render-pass break and a DRAM round-trip on the resolve path. Ineligible "
    "cases (gamma, unsupported formats, multi-sample selects) fall back to the "
    "EDRAM dump path. Disable to always use the EDRAM dump path.",
    "Vulkan");
DEFINE_bool(
    vulkan_compatibility_rendering, false,
    "Compatibility rendering: turns off the render target -> texture shortcuts "
    "(vulkan_texture_from_render_target, vulkan_direct_host_resolve, "
    "vulkan_resolve_texture_copy, vulkan_color_surface_resolve, "
    "vulkan_in_pass_transfers), so resolves take the plain compute path. For "
    "drivers that show a black image with them (Mali-G615 r44p1). Costs GPU "
    "time (Halo Reach on Mali-G720: about 6 ms per frame).",
    "Vulkan");

DEFINE_bool(
    vulkan_direct_host_resolve_2x2, true,
    "With vulkan_direct_host_resolve: resolve 4x surfaces kept as 1x images of "
    "twice the size (vulkan_msaa_4x_as_2x2) directly from those images, "
    "reading their texels as the EDRAM samples, instead of dumping them "
    "through the EDRAM buffer first.",
    "Vulkan");
// Default since q587: the EDRAM dump of 2x2-kept 4x surfaces is skipped (GTA IV
// 11 -> 5 dumps per frame, Halo Reach 8 -> 6), same bytes, same image.
UPDATE_from_bool(vulkan_direct_host_resolve_2x2, 2026, 10, 25, 22, false);

DEFINE_bool(
    vulkan_direct_host_resolve_foreign_owner, true,
    "With vulkan_direct_host_resolve: also resolve color directly when the "
    "EDRAM is owned by a 32bpp color render target of another format, or by a "
    "depth buffer (bit-exact copies), packing the owner's texels as the EDRAM "
    "dump would, instead of dumping them through the EDRAM buffer first.",
    "Vulkan");
// Default since q640: Halo Reach 4 -> 0 EDRAM dumps per frame (depth, gamma and
// other-format owners), same bytes, same image.
UPDATE_from_bool(vulkan_direct_host_resolve_foreign_owner, 2026, 11, 1, 11, false);

DEFINE_bool(
    vulkan_depth_unorm24, true,
    "Use the native D24_UNORM_S8_UINT format for guest 24-bit depth when the "
    "driver supports it. Disable to force the float32 depth emulation path "
    "(what drivers without unorm24 support, like Adreno proprietary, always "
    "use) for debugging.",
    "Vulkan");

DEFINE_int32(
    log_in_pass_transfer_rejects, 0,
    "Every N frames, log how many ownership transfers ran inside the guest "
    "render pass and why the others needed a pass of their own (0 = off).",
    "GPU");

DEFINE_bool(
    log_resolve_details, false,
    "Log a once-per-second histogram of resolve (EDRAM copy) operations: "
    "count and bytes per frame grouped by size, color/depth, MSAA, source and "
    "destination formats and direct-host vs staged path, plus destination "
    "address reuse within and across frames.",
    "GPU");

DECLARE_uint32(log_texture_reloads);
DECLARE_bool(rt_cache_legacy_clear_paths);
DEFINE_bool(vulkan_stencil_transfer_copy, true,
            "Without VK_EXT_shader_stencil_export, write the stencil of a "
            "k_8_8_8_8 color range taken over by a 1x depth render target at "
            "the same base and pitch with a compute pass and a buffer to image "
            "copy instead of one full transfer draw per stencil bit (Grand "
            "Theft Auto IV rebuilds its depth buffer this way every frame). "
            "Read at render target creation.",
            "GPU");
DEFINE_bool(debug_skip_stencil_bit_transfers, false,
            "Diagnostic (breaks the stencil): skip the per-bit stencil draws of "
            "ownership transfers into depth render targets, to measure their "
            "cost.",
            "GPU");
DEFINE_uint32(log_transfer_summary, 0,
              "Diagnostic: every this many render target ownership transfer "
              "invocations, log them grouped by source, destination and "
              "aspect. 0 disables.",
              "GPU");
DECLARE_uint32(log_inpass_texture_loads);

namespace xe {
namespace gpu {
namespace vulkan {
namespace {
// A compute pipeline of a precompiled shader that may fetch the samples of
// render target images: with vulkan_msaa_single_sample_storage the images are
// single-sampled, and the shader is rewritten to read every sample it asks
// for from the pixel's one texel (a no-op for shaders reading nothing
// multisampled).
VkPipeline CreateSourceReadingComputePipeline(
    bool single_sample_storage, const ui::vulkan::VulkanDevice* vulkan_device,
    VkPipelineLayout layout, const uint32_t* shader_code,
    size_t shader_code_size_bytes,
    const VkSpecializationInfo* specialization_info = nullptr,
    const char* entry_point = "main", uint32_t required_subgroup_size = 0) {
  if (!single_sample_storage) {
    return ui::vulkan::util::CreateComputePipeline(
        vulkan_device, layout, shader_code, shader_code_size_bytes,
        specialization_info, entry_point, required_subgroup_size);
  }
  std::vector<uint32_t> patched;
  if (!PatchSpirvMultisampledFetchesToSingleSample(
          shader_code, shader_code_size_bytes, patched)) {
    XELOGE("VulkanRenderTargetCache: Failed to rewrite a shader for "
           "single-sample storage");
    return VK_NULL_HANDLE;
  }
  return ui::vulkan::util::CreateComputePipeline(
      vulkan_device, layout, patched.data(), patched.size() * sizeof(uint32_t),
      specialization_info, entry_point, required_subgroup_size);
}
}  // namespace
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/passthrough_position_xy_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_fast_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_fast_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_fast_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_fast_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_fast_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_fast_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_fast_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_fast_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_fast_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_fast_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_inpass_32bpp_ms_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_inpass_32bpp_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_inpass_32bpp_r32_uint_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_inpass_64bpp_ms_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_host_color_inpass_64bpp_ps.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_inpass_vs.h"
#include "xenia/gpu/shaders/vulkan_direct_host_resolve_bytecode.h"
}  // namespace shaders

std::string VulkanRenderTargetCache::GetLastUpdateRenderTargetsDebugName()
    const {
  RenderTarget* const* rts = last_update_accumulated_render_targets();
  std::string names;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!rts[i]) {
      continue;
    }
    if (!names.empty()) {
      names += " | ";
    }
    names += (i == 0 ? "depth " : fmt::format("color{} ", i - 1));
    names += rts[i]->key().GetDebugName();
  }
  return names.empty() ? std::string("(none)") : names;
}

const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs,
         sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs,
         sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs,
         sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs,
         sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs,
         sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs,
         sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

// Same order as kResolveCopyShaders. The fast copies are raw, so only the full
// entries differ; the fast entries alias the shared ones.
const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShadersFastFormats[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_fast_cs,
         sizeof(shaders::resolve_full_8bpp_fast_cs),
         shaders::resolve_full_8bpp_fast_scaled_cs,
         sizeof(shaders::resolve_full_8bpp_fast_scaled_cs)},
        {shaders::resolve_full_16bpp_fast_cs,
         sizeof(shaders::resolve_full_16bpp_fast_cs),
         shaders::resolve_full_16bpp_fast_scaled_cs,
         sizeof(shaders::resolve_full_16bpp_fast_scaled_cs)},
        {shaders::resolve_full_32bpp_fast_cs,
         sizeof(shaders::resolve_full_32bpp_fast_cs),
         shaders::resolve_full_32bpp_fast_scaled_cs,
         sizeof(shaders::resolve_full_32bpp_fast_scaled_cs)},
        {shaders::resolve_full_64bpp_fast_cs,
         sizeof(shaders::resolve_full_64bpp_fast_cs),
         shaders::resolve_full_64bpp_fast_scaled_cs,
         sizeof(shaders::resolve_full_64bpp_fast_scaled_cs)},
        {shaders::resolve_full_128bpp_fast_cs,
         sizeof(shaders::resolve_full_128bpp_fast_cs),
         shaders::resolve_full_128bpp_fast_scaled_cs,
         sizeof(shaders::resolve_full_128bpp_fast_scaled_cs)},
};

// Each entry expands to {code, sizeof(code), "code"} for one generated SPIR-V
// resolve variant, keeping the variant name spelled out only once.
#define XE_DHR_SHADER(name) {shaders::name, sizeof(shaders::name), #name}

const VulkanRenderTargetCache::DirectHostResolveShaderCode
    VulkanRenderTargetCache::kDirectHostResolveColorShaders
        [VulkanRenderTargetCache::kDirectHostResolveBppCount]
        [VulkanRenderTargetCache::kDirectHostResolveMsaaCount]
        [VulkanRenderTargetCache::kDirectHostResolveScaledCount]
        [VulkanRenderTargetCache::kDirectHostResolveSourceUintCount] = {
            {
                // 32bpp
                {{XE_DHR_SHADER(resolve_host_color_32bpp_1xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_32bpp_1xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_32bpp_1xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_32bpp_1xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_32bpp_2xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_32bpp_2xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_32bpp_2xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_32bpp_2xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_32bpp_4xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_32bpp_4xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_32bpp_4xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_32bpp_4xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_32bpp_4xmsaa_2x2_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_32bpp_4xmsaa_2x2_cs)},
                 {XE_DHR_SHADER(resolve_host_color_32bpp_4xmsaa_scaled_2x2_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_32bpp_4xmsaa_scaled_2x2_cs)}},
            },
            {
                // 64bpp
                {{XE_DHR_SHADER(resolve_host_color_64bpp_1xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_64bpp_1xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_64bpp_1xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_64bpp_1xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_64bpp_2xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_64bpp_2xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_64bpp_2xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_64bpp_2xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_64bpp_4xmsaa_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_64bpp_4xmsaa_cs)},
                 {XE_DHR_SHADER(resolve_host_color_64bpp_4xmsaa_scaled_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_64bpp_4xmsaa_scaled_cs)}},
                {{XE_DHR_SHADER(resolve_host_color_64bpp_4xmsaa_2x2_cs),
                  XE_DHR_SHADER(resolve_host_color_uint_64bpp_4xmsaa_2x2_cs)},
                 {XE_DHR_SHADER(resolve_host_color_64bpp_4xmsaa_scaled_2x2_cs),
                  XE_DHR_SHADER(
                      resolve_host_color_uint_64bpp_4xmsaa_scaled_2x2_cs)}},
            },
};

#define XE_DHR_FULL_DESTS_1X(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_1xmsaa_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_1xmsaa_cs), \
   XE_DHR_SHADER(prefix##_32bpp_1xmsaa_cs), \
   XE_DHR_SHADER(prefix##_64bpp_1xmsaa_cs), \
   XE_DHR_SHADER(prefix##_128bpp_1xmsaa_cs)}
#define XE_DHR_FULL_DESTS_1X_SCALED(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_1xmsaa_scaled_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_1xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_32bpp_1xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_64bpp_1xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_128bpp_1xmsaa_scaled_cs)}
#define XE_DHR_FULL_DESTS_2X(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_2xmsaa_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_2xmsaa_cs), \
   XE_DHR_SHADER(prefix##_32bpp_2xmsaa_cs), \
   XE_DHR_SHADER(prefix##_64bpp_2xmsaa_cs), \
   XE_DHR_SHADER(prefix##_128bpp_2xmsaa_cs)}
#define XE_DHR_FULL_DESTS_2X_SCALED(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_2xmsaa_scaled_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_2xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_32bpp_2xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_64bpp_2xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_128bpp_2xmsaa_scaled_cs)}
#define XE_DHR_FULL_DESTS_4X(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_4xmsaa_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_4xmsaa_cs), \
   XE_DHR_SHADER(prefix##_32bpp_4xmsaa_cs), \
   XE_DHR_SHADER(prefix##_64bpp_4xmsaa_cs), \
   XE_DHR_SHADER(prefix##_128bpp_4xmsaa_cs)}
#define XE_DHR_FULL_DESTS_4X_SCALED(prefix)        \
  {XE_DHR_SHADER(prefix##_8bpp_4xmsaa_scaled_cs),  \
   XE_DHR_SHADER(prefix##_16bpp_4xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_32bpp_4xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_64bpp_4xmsaa_scaled_cs), \
   XE_DHR_SHADER(prefix##_128bpp_4xmsaa_scaled_cs)}
#define XE_DHR_FULL_DESTS_2X2(prefix)            \
  {XE_DHR_SHADER(prefix##_8bpp_4xmsaa_2x2_cs),   \
   XE_DHR_SHADER(prefix##_16bpp_4xmsaa_2x2_cs),  \
   XE_DHR_SHADER(prefix##_32bpp_4xmsaa_2x2_cs),  \
   XE_DHR_SHADER(prefix##_64bpp_4xmsaa_2x2_cs),  \
   XE_DHR_SHADER(prefix##_128bpp_4xmsaa_2x2_cs)}
#define XE_DHR_FULL_DESTS_2X2_SCALED(prefix)            \
  {XE_DHR_SHADER(prefix##_8bpp_4xmsaa_scaled_2x2_cs),   \
   XE_DHR_SHADER(prefix##_16bpp_4xmsaa_scaled_2x2_cs),  \
   XE_DHR_SHADER(prefix##_32bpp_4xmsaa_scaled_2x2_cs),  \
   XE_DHR_SHADER(prefix##_64bpp_4xmsaa_scaled_2x2_cs),  \
   XE_DHR_SHADER(prefix##_128bpp_4xmsaa_scaled_2x2_cs)}

const VulkanRenderTargetCache::DirectHostResolveShaderCode
    VulkanRenderTargetCache::kDirectHostResolveColorFullShaders
        [VulkanRenderTargetCache::kDirectHostResolveMsaaCount]
        [VulkanRenderTargetCache::kDirectHostResolveScaledCount]
        [VulkanRenderTargetCache::kDirectHostResolveSourceUintCount]
        [VulkanRenderTargetCache::kDirectHostResolveFullDestCount] = {
            {{XE_DHR_FULL_DESTS_1X(resolve_host_color_full),
              XE_DHR_FULL_DESTS_1X(resolve_host_color_full_uint)},
             {XE_DHR_FULL_DESTS_1X_SCALED(resolve_host_color_full),
              XE_DHR_FULL_DESTS_1X_SCALED(resolve_host_color_full_uint)}},
            {{XE_DHR_FULL_DESTS_2X(resolve_host_color_full),
              XE_DHR_FULL_DESTS_2X(resolve_host_color_full_uint)},
             {XE_DHR_FULL_DESTS_2X_SCALED(resolve_host_color_full),
              XE_DHR_FULL_DESTS_2X_SCALED(resolve_host_color_full_uint)}},
            {{XE_DHR_FULL_DESTS_4X(resolve_host_color_full),
              XE_DHR_FULL_DESTS_4X(resolve_host_color_full_uint)},
             {XE_DHR_FULL_DESTS_4X_SCALED(resolve_host_color_full),
              XE_DHR_FULL_DESTS_4X_SCALED(resolve_host_color_full_uint)}},
            {{XE_DHR_FULL_DESTS_2X2(resolve_host_color_full),
              XE_DHR_FULL_DESTS_2X2(resolve_host_color_full_uint)},
             {XE_DHR_FULL_DESTS_2X2_SCALED(resolve_host_color_full),
              XE_DHR_FULL_DESTS_2X2_SCALED(resolve_host_color_full_uint)}},
};

#undef XE_DHR_FULL_DESTS_2X2_SCALED
#undef XE_DHR_FULL_DESTS_2X2
#undef XE_DHR_FULL_DESTS_4X_SCALED
#undef XE_DHR_FULL_DESTS_4X
#undef XE_DHR_FULL_DESTS_2X_SCALED
#undef XE_DHR_FULL_DESTS_2X
#undef XE_DHR_FULL_DESTS_1X_SCALED
#undef XE_DHR_FULL_DESTS_1X

const VulkanRenderTargetCache::DirectHostResolveShaderCode
    VulkanRenderTargetCache::kDirectHostResolveDepthShaders
        [VulkanRenderTargetCache::kDirectHostResolveMsaaCount]
        [VulkanRenderTargetCache::kDirectHostResolveScaledCount] = {
            {XE_DHR_SHADER(resolve_host_depth_32bpp_1xmsaa_cs),
             XE_DHR_SHADER(resolve_host_depth_32bpp_1xmsaa_scaled_cs)},
            {XE_DHR_SHADER(resolve_host_depth_32bpp_2xmsaa_cs),
             XE_DHR_SHADER(resolve_host_depth_32bpp_2xmsaa_scaled_cs)},
            {XE_DHR_SHADER(resolve_host_depth_32bpp_4xmsaa_cs),
             XE_DHR_SHADER(resolve_host_depth_32bpp_4xmsaa_scaled_cs)},
            // No 2x2 depth variant: depth stays on the EDRAM dump path.
            {{nullptr, 0, "none"}, {nullptr, 0, "none"}},
};

#undef XE_DHR_SHADER

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[size_t(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit |
             kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit |
             kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const VulkanRenderTargetCache::TransferModeInfo
    VulkanRenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit,
         TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit,
         TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

VulkanRenderTargetCache::VulkanRenderTargetCache(
    const RegisterFile& register_file, const Memory& memory,
    TraceWriter& trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, &trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {
  // This must be on before any RT handle can escape into a command buffer.
  // A runtime enable after untracked uses cannot establish their fences.
  retirement_tracking_enabled_ = cvars::vulkan_rt_retirement;
  SetDrawExtentInputReadValidator(
      &VulkanRenderTargetCache::ValidateDrawExtentInputRead, this);
}

bool VulkanRenderTargetCache::ValidateDrawExtentInputRead(
    void* context, uint32_t address, uint32_t length,
    DrawExtentEstimator::InputReadKind kind) {
  auto& render_target_cache =
      *static_cast<VulkanRenderTargetCache*>(context);
  const VulkanSharedMemory* shared_memory =
      render_target_cache.command_processor_.shared_memory_.get();
  const char* read_kind =
      kind == DrawExtentEstimator::InputReadKind::kIndexBuffer ? "index"
                                                                : "vfetch";
  if (!shared_memory) {
    if (render_target_cache.draw_extent_input_rejections_logged_ < 32) {
      ++render_target_cache.draw_extent_input_rejections_logged_;
      XELOGI(
          "RTCLEAR: CPU vertex estimate rejected {} input at 0x{:08X} "
          "({} bytes): shared-memory tracking unavailable",
          read_kind, address, length);
    }
    return false;
  }
  // The estimator validates one DMA index or one fetched dword at a time. The
  // history query rejects if any fixed 4 KiB page intersecting that read has
  // ever been GPU-written, including after a CPU fault cleared current GPU
  // authority and before the retried CPU store actually updates guest RAM.
  if (!length || address >= SharedMemory::kBufferSize ||
      length > SharedMemory::kBufferSize - address) {
    if (render_target_cache.draw_extent_input_rejections_logged_ < 32) {
      ++render_target_cache.draw_extent_input_rejections_logged_;
      XELOGI(
          "RTCLEAR: CPU vertex estimate rejected {} input at 0x{:08X} "
          "({} bytes): outside shared-memory address range",
          read_kind, address, length);
    }
    return false;
  }
  bool history_available = false;
  if (!shared_memory->IsRangeEverGpuWritten(address, length,
                                            &history_available)) {
    return true;
  }
  if (render_target_cache.draw_extent_input_rejections_logged_ < 32) {
    ++render_target_cache.draw_extent_input_rejections_logged_;
    constexpr uint32_t page_log2 =
        SharedMemory::kGpuWrittenHistoryPageSizeLog2;
    uint32_t page = (address >> page_log2) << page_log2;
    XELOGI(
        "RTCLEAR: CPU vertex estimate rejected {} input at 0x{:08X} "
        "({} bytes, page 0x{:08X}, 4 KiB history page): {}",
        read_kind, address, length, page,
        history_available ? "page was GPU-written during this lifetime"
                          : "GPU-write history unavailable; failing closed");
  }
  return false;
}

VulkanRenderTargetCache::~VulkanRenderTargetCache() { Shutdown(true); }

bool VulkanRenderTargetCache::Initialize(uint32_t shared_memory_binding_count) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();

  // Cache the SPIR-V version for utility shader creation.
  spirv_version_ = SpirvShaderTranslator::Features(vulkan_device).spirv_version;

  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  use_dynamic_rendering_ =
      cvars::vulkan_dynamic_rendering && device_properties.dynamicRendering;
  local_read_attachments_ =
      cvars::vulkan_in_pass_resolve && cvars::vulkan_dynamic_rendering &&
      device_properties.dynamicRendering &&
      device_properties.dynamicRenderingLocalRead;
  if (local_read_attachments_ && cvars::vulkan_in_pass_resolve &&
      cvars::vulkan_in_pass_resolve_r32_uint) {
    VkFormatProperties r32_uint_format_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_R32_UINT, &r32_uint_format_properties);
    constexpr VkFormatFeatureFlags kR32UintColorAttachmentFeatures =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    r32_uint_input_attachment_supported_ =
        (r32_uint_format_properties.optimalTilingFeatures &
         kR32UintColorAttachmentFeatures) ==
        kR32UintColorAttachmentFeatures;
  }
  if (local_read_attachments_) {
    color_draw_stage_mask_ |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    color_draw_access_mask_ |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    color_draw_layout_ = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
    XELOGI("VulkanRenderTargetCache: local-read color attachment mode on");
  }
  // Setup below can fail after the masks were raised; RENDERING_LOCAL_READ is
  // only legal while the feature is actually in use, so undo them together.
  auto disable_local_read = [this]() {
    local_read_attachments_ = false;
    r32_uint_input_attachment_supported_ = false;
    color_draw_stage_mask_ = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_draw_access_mask_ = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    color_draw_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  };
  if (local_read_attachments_) {
    VkDescriptorSetLayoutBinding input_attachment_binding;
    input_attachment_binding.binding = 0;
    input_attachment_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    input_attachment_binding.descriptorCount = 1;
    input_attachment_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    input_attachment_binding.pImmutableSamplers = nullptr;
    VkDescriptorSetLayoutCreateInfo input_attachment_layout_create_info;
    input_attachment_layout_create_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    input_attachment_layout_create_info.pNext = nullptr;
    input_attachment_layout_create_info.flags = 0;
    input_attachment_layout_create_info.bindingCount = 1;
    input_attachment_layout_create_info.pBindings = &input_attachment_binding;
    VkPushConstantRange resolve_inpass_push_constant_range;
    resolve_inpass_push_constant_range.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    resolve_inpass_push_constant_range.offset = 0;
    resolve_inpass_push_constant_range.size =
        sizeof(draw_util::ResolveCopyShaderConstants) + 3 * sizeof(int32_t);
    if (dfn.vkCreateDescriptorSetLayout(
            device, &input_attachment_layout_create_info, nullptr,
            &descriptor_set_layout_input_attachment_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the input attachment "
          "descriptor set layout, disabling in-pass resolves");
      disable_local_read();
    } else {
      VkDescriptorSetLayout resolve_inpass_set_layouts[] = {
          command_processor_.GetSingleTransientDescriptorLayout(
              VulkanCommandProcessor::SingleTransientDescriptorLayout ::
                  kStorageBufferFragment),
          descriptor_set_layout_input_attachment_};
      VkPipelineLayoutCreateInfo resolve_inpass_pipeline_layout_create_info;
      resolve_inpass_pipeline_layout_create_info.sType =
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      resolve_inpass_pipeline_layout_create_info.pNext = nullptr;
      resolve_inpass_pipeline_layout_create_info.flags = 0;
      resolve_inpass_pipeline_layout_create_info.setLayoutCount =
          uint32_t(xe::countof(resolve_inpass_set_layouts));
      resolve_inpass_pipeline_layout_create_info.pSetLayouts =
          resolve_inpass_set_layouts;
      resolve_inpass_pipeline_layout_create_info.pushConstantRangeCount = 1;
      resolve_inpass_pipeline_layout_create_info.pPushConstantRanges =
          &resolve_inpass_push_constant_range;
      resolve_inpass_vertex_shader_ = ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::resolve_inpass_vs,
          sizeof(shaders::resolve_inpass_vs));
      resolve_inpass_shaders_[0] = ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::resolve_host_color_inpass_32bpp_ps,
          sizeof(shaders::resolve_host_color_inpass_32bpp_ps));
      resolve_inpass_shaders_[1] = ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::resolve_host_color_inpass_64bpp_ps,
          sizeof(shaders::resolve_host_color_inpass_64bpp_ps));
      resolve_inpass_shaders_[2] = ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::resolve_host_color_inpass_32bpp_ms_ps,
          sizeof(shaders::resolve_host_color_inpass_32bpp_ms_ps));
      resolve_inpass_shaders_[3] = ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::resolve_host_color_inpass_64bpp_ms_ps,
          sizeof(shaders::resolve_host_color_inpass_64bpp_ms_ps));
      if (dfn.vkCreatePipelineLayout(
              device, &resolve_inpass_pipeline_layout_create_info, nullptr,
              &resolve_inpass_pipeline_layout_) != VK_SUCCESS ||
          resolve_inpass_vertex_shader_ == VK_NULL_HANDLE ||
          resolve_inpass_shaders_[0] == VK_NULL_HANDLE ||
          resolve_inpass_shaders_[1] == VK_NULL_HANDLE ||
          resolve_inpass_shaders_[2] == VK_NULL_HANDLE ||
          resolve_inpass_shaders_[3] == VK_NULL_HANDLE) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the in-pass resolve "
            "pipeline layout or shaders, disabling in-pass resolves");
        disable_local_read();
      } else {
        if (r32_uint_input_attachment_supported_ &&
            cvars::vulkan_in_pass_resolve &&
            cvars::vulkan_in_pass_resolve_r32_uint) {
          resolve_inpass_r32_uint_shaders_[0] =
              ui::vulkan::util::CreateShaderModule(
                  vulkan_device,
                  shaders::resolve_host_color_inpass_32bpp_r32_uint_ps,
                  sizeof(shaders::resolve_host_color_inpass_32bpp_r32_uint_ps));
          if (resolve_inpass_r32_uint_shaders_[0] == VK_NULL_HANDLE) {
            for (auto& shader : resolve_inpass_r32_uint_shaders_) {
              ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule,
                                                     device, shader);
            }
            r32_uint_input_attachment_supported_ = false;
            XELOGW(
                "VulkanRenderTargetCache: Failed to create R32_UINT in-pass "
                "resolve shaders; keeping the base in-pass resolver enabled");
          }
        }
        VkDescriptorPoolSize input_attachment_pool_size;
        input_attachment_pool_size.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        input_attachment_pool_size.descriptorCount = 1;
        const bool allocate_r32_uint_descriptors =
            r32_uint_input_attachment_supported_ &&
            cvars::vulkan_in_pass_resolve &&
            cvars::vulkan_in_pass_resolve_r32_uint;
        descriptor_set_pool_input_attachment_ =
            std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
                vulkan_device, allocate_r32_uint_descriptors ? 512 : 256, 1,
                &input_attachment_pool_size,
                descriptor_set_layout_input_attachment_);
      }
    }
  }

  if (cvars::render_target_path == "accuracy") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
  }
  // Fragment shader interlock is a feature implemented by pretty advanced GPUs,
  // closer to Direct3D 11 / OpenGL ES 3.2 level mainly, not Direct3D 10 /
  // OpenGL ES 3.1. Thus, it's fine to demand a wide range of other optional
  // features for the fragment shader interlock backend to work.
  if (path_ == Path::kPixelShaderInterlock) {
    // Interlocking between fragments with common sample coverage is enough, but
    // interlocking more is acceptable too (fragmentShaderShadingRateInterlock
    // would be okay too, but it's unlikely that an implementation would
    // advertise only it and not any other ones, as it's a very specific feature
    // interacting with another optional feature that is variable shading rate,
    // so there's no need to overcomplicate the checks and the shader execution
    // mode setting).
    // Sample-rate shading is required by certain SPIR-V revisions to access the
    // sample mask fragment shader input.
    // Stanard sample locations are needed for calculating the depth at the
    // samples.
    // It's unlikely that a device exposing fragment shader interlock won't have
    // a large enough storage buffer range and a sufficient SSBO slot count for
    // all the shared memory buffers and the EDRAM buffer - an in a conflict
    // between, for instance, the ability to vfetch and memexport in fragment
    // shaders, and the usage of fragment shader interlock, prefer the former
    // for simplicity.
    if (!(device_properties.fragmentShaderSampleInterlock ||
          device_properties.fragmentShaderPixelInterlock) ||
        !device_properties.fragmentStoresAndAtomics ||
        !device_properties.sampleRateShading ||
        !device_properties.standardSampleLocations ||
        shared_memory_binding_count >=
            device_properties.maxPerStageDescriptorStorageBuffers) {
      path_ = Path::kHostRenderTargets;
    }
  }

  // Format support.
  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VkFormatProperties depth_unorm24_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &depth_unorm24_properties);
  // Cache only the device-support half; the vulkan_depth_unorm24 cvar is
  // applied live in the accessor so it can be overridden per-game.
  depth_unorm24_vulkan_format_supported_ =
      (depth_unorm24_properties.optimalTilingFeatures &
       kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
  XELOGGPU(
      "VulkanRenderTargetCache: D24_UNORM_S8_UINT {} - guest 24-bit depth "
      "will use the {} path",
      depth_unorm24_vulkan_format_supported_ ? "supported" : "unavailable",
      depth_unorm24_vulkan_format_supported_ ? "native unorm24"
                                             : "float32 emulation");

  // 2x MSAA support.
  // TODO(Triang3l): Handle sampledImageIntegerSampleCounts 4 not supported in
  // transfers.
  if (cvars::native_2x_msaa) {
    // Multisampled integer sampled images are optional in Vulkan and in Xenia.
    msaa_2x_attachments_supported_ =
        (device_properties.framebufferColorSampleCounts &
         device_properties.framebufferDepthSampleCounts &
         device_properties.framebufferStencilSampleCounts &
         device_properties.sampledImageColorSampleCounts &
         device_properties.sampledImageDepthSampleCounts &
         device_properties.sampledImageStencilSampleCounts &
         VK_SAMPLE_COUNT_2_BIT) &&
        (device_properties.sampledImageIntegerSampleCounts &
         (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT)) !=
            VK_SAMPLE_COUNT_4_BIT;
    msaa_2x_no_attachments_supported_ =
        (device_properties.framebufferNoAttachmentsSampleCounts &
         VK_SAMPLE_COUNT_2_BIT) != 0;
  } else {
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;
  }
  msaa_single_sample_storage_ = cvars::vulkan_msaa_single_sample_storage &&
                                GetPath() == Path::kHostRenderTargets;
  msaa_single_sample_resolve_ = false;
  if (msaa_single_sample_storage_ && cvars::vulkan_msaa_single_sample_resolve) {
    // Every format a multisampled render target may have must accept the
    // guest sample counts when created for multisampled rendering into a
    // single-sampled image, or the whole mode stays off (the pipelines, the
    // passes and the images must all agree).
    const char* refusal = nullptr;
    if (!vulkan_device->extensions()
             .ext_EXT_multisampled_render_to_single_sampled) {
      refusal = "extension not available";
    } else if (!cvars::vulkan_dynamic_rendering ||
               !device_properties.dynamicRendering) {
      refusal = "dynamic rendering not in use";
    } else if (!msaa_2x_attachments_supported_) {
      // 2x would be two of four samples, and the resolve averages all four.
      refusal = "2x multisampling not native";
    } else {
      const VkSampleCountFlags needed_samples =
          (msaa_2x_attachments_supported_ ? VK_SAMPLE_COUNT_2_BIT : 0) |
          VK_SAMPLE_COUNT_4_BIT;
      auto format_accepts = [&](VkFormat format, bool is_depth) {
        if (format == VK_FORMAT_UNDEFINED) {
          return true;
        }
        VkImageFormatProperties format_properties = {};
        return vulkan_device->vulkan_instance()
                       ->functions()
                       .vkGetPhysicalDeviceImageFormatProperties(
                           vulkan_device->physical_device(), format,
                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                           // As CreateRenderTarget may ask for.
                           VK_IMAGE_USAGE_SAMPLED_BIT |
                               (is_depth
                                    ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                    : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       (local_read_attachments_
                                            ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                                            : 0))),
                           VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT |
                               (is_depth ? 0
                                         : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT),
                           &format_properties) == VK_SUCCESS &&
               (format_properties.sampleCounts & needed_samples) ==
                   needed_samples;
      };
      for (xenos::DepthRenderTargetFormat depth_format :
           {xenos::DepthRenderTargetFormat::kD24S8,
            xenos::DepthRenderTargetFormat::kD24FS8}) {
        if (!format_accepts(GetDepthVulkanFormat(depth_format), true)) {
          refusal = "a depth format doesn't support it";
        }
      }
      for (xenos::ColorRenderTargetFormat color_format :
           {xenos::ColorRenderTargetFormat::k_8_8_8_8,
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
            xenos::ColorRenderTargetFormat::k_2_10_10_10,
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
            xenos::ColorRenderTargetFormat::k_16_16,
            xenos::ColorRenderTargetFormat::k_16_16_16_16,
            xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16,
            xenos::ColorRenderTargetFormat::k_32_FLOAT,
            xenos::ColorRenderTargetFormat::k_32_32_FLOAT}) {
        if (!format_accepts(GetColorVulkanFormat(color_format), false) ||
            !format_accepts(
                GetColorOwnershipTransferVulkanFormat(color_format), false)) {
          refusal = "a color format doesn't support it";
        }
      }
      // What k_8_8_8_8_GAMMA may be given later in the initialization.
      if (!format_accepts(VK_FORMAT_R16G16B16A16_UNORM, false)) {
        refusal = "a color format doesn't support it";
      }
    }
    msaa_single_sample_resolve_ = refusal == nullptr;
    if (refusal) {
      XELOGW("VulkanRenderTargetCache: vulkan_msaa_single_sample_resolve not "
             "used ({}); render targets rasterize one sample",
             refusal);
    }
  }
  if (msaa_single_sample_storage_) {
    XELOGI("VulkanRenderTargetCache: multisampled guest render targets are "
           "stored single-sampled{}",
           msaa_single_sample_resolve_
               ? ", multisampled within each pass"
               : "");
  }

  // Descriptor set layouts.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_storage_buffer_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one sampled image");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags =
      descriptor_set_layout_bindings[0].stageFlags;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 2;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_sampled_image_x2_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with two sampled images");
    Shutdown();
    return false;
  }

  // Descriptor set pools.
  // The pool sizes were chosen without a specific reason.
  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // EDRAM contents reinterpretation buffer.
  // 90 MB with 9x resolution scaling - within the minimum
  // maxStorageBufferRange.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize(xenos::kEdramSizeBytes *
                       (draw_resolution_scale_x() * draw_resolution_scale_y())),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, edram_buffer_,
          edram_buffer_memory_)) {
    XELOGE("VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    // The first operation will likely be drawing.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentReadWrite;
  } else {
    // The first operation will likely be depth self-comparison.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPoolSize edram_storage_buffer_descriptor_pool_size;
  edram_storage_buffer_descriptor_pool_size.type =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo edram_storage_buffer_descriptor_pool_create_info;
  edram_storage_buffer_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  edram_storage_buffer_descriptor_pool_create_info.pNext = nullptr;
  edram_storage_buffer_descriptor_pool_create_info.flags = 0;
  edram_storage_buffer_descriptor_pool_create_info.maxSets = 1;
  edram_storage_buffer_descriptor_pool_create_info.poolSizeCount = 1;
  edram_storage_buffer_descriptor_pool_create_info.pPoolSizes =
      &edram_storage_buffer_descriptor_pool_size;
  if (dfn.vkCreateDescriptorPool(
          device, &edram_storage_buffer_descriptor_pool_create_info, nullptr,
          &edram_storage_buffer_descriptor_pool_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the EDRAM buffer storage "
        "buffer descriptor pool");
    Shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo edram_storage_buffer_descriptor_set_allocate_info;
  edram_storage_buffer_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  edram_storage_buffer_descriptor_set_allocate_info.pNext = nullptr;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorPool =
      edram_storage_buffer_descriptor_pool_;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorSetCount = 1;
  edram_storage_buffer_descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_storage_buffer_;
  if (dfn.vkAllocateDescriptorSets(
          device, &edram_storage_buffer_descriptor_set_allocate_info,
          &edram_storage_buffer_descriptor_set_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to allocate the EDRAM buffer storage "
        "buffer descriptor set");
    Shutdown();
    return false;
  }
  VkDescriptorBufferInfo edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_buffer_info.buffer = edram_buffer_;
  edram_storage_buffer_descriptor_buffer_info.offset = 0;
  edram_storage_buffer_descriptor_buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet edram_storage_buffer_descriptor_write;
  edram_storage_buffer_descriptor_write.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  edram_storage_buffer_descriptor_write.pNext = nullptr;
  edram_storage_buffer_descriptor_write.dstSet =
      edram_storage_buffer_descriptor_set_;
  edram_storage_buffer_descriptor_write.dstBinding = 0;
  edram_storage_buffer_descriptor_write.dstArrayElement = 0;
  edram_storage_buffer_descriptor_write.descriptorCount = 1;
  edram_storage_buffer_descriptor_write.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_write.pImageInfo = nullptr;
  edram_storage_buffer_descriptor_write.pBufferInfo =
      &edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &edram_storage_buffer_descriptor_write,
                             0, nullptr);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Resolve copy pipeline layout.
  VkDescriptorSetLayout
      resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEdram] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::
              kStorageBufferCompute);
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetUnused] =
      resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest];
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetLinear] =
      resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest];
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Potentially binding all of the shared memory at 1x resolution, but only
  // portions with scaled resolution.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled
          ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
          : sizeof(draw_util::ResolveCopyShaderConstants);
  VkPipelineLayoutCreateInfo resolve_copy_pipeline_layout_create_info;
  resolve_copy_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  resolve_copy_pipeline_layout_create_info.pNext = nullptr;
  resolve_copy_pipeline_layout_create_info.flags = 0;
  resolve_copy_pipeline_layout_create_info.setLayoutCount =
      kResolveCopyDescriptorSetCount;
  resolve_copy_pipeline_layout_create_info.pSetLayouts =
      resolve_copy_descriptor_set_layouts;
  resolve_copy_pipeline_layout_create_info.pushConstantRangeCount = 1;
  resolve_copy_pipeline_layout_create_info.pPushConstantRanges =
      &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &resolve_copy_pipeline_layout_create_info, nullptr,
          &resolve_copy_pipeline_layout_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
        "layout");
    Shutdown();
    return false;
  }
  if (draw_resolution_scaled) {
    // Second layout for fully native resolve copies.
    resolve_copy_push_constant_range.size =
        sizeof(draw_util::ResolveCopyShaderConstants);
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_copy_pipeline_layout_create_info, nullptr,
            &resolve_copy_native_pipeline_layout_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the native resolve copy "
          "pipeline layout");
      Shutdown();
      return false;
    }
  }

  // Resolve copy pipelines.
  XELOGI("Resolve copy shaders: {} number-format variants",
         cvars::accurate_resolve_number_formats ? "ACCURATE" : "FAST");
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount);
       ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code =
        cvars::accurate_resolve_number_formats ? kResolveCopyShaders[i]
                                               : kResolveCopyShadersFastFormats[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled &&
                resolve_copy_shader_code.unscaled_size_bytes &&
                resolve_copy_shader_code.scaled &&
                resolve_copy_shader_code.scaled_size_bytes);
    // Resolve copy shaders use 8x8 = 64 threads per group. Request wave64 mode
    // on RDNA GPUs to ensure one full wave per group.
    VkPipeline resolve_copy_pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes,
        nullptr, "main", 64);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the resolve copy "
          "pipeline {}",
          resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, resolve_copy_pipeline,
                                 resolve_copy_shader_info.debug_name);
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
    if (draw_resolution_scaled) {
      // Unscaled variant for fully native resolves.
      VkPipeline resolve_copy_native_pipeline =
          ui::vulkan::util::CreateComputePipeline(
              vulkan_device, resolve_copy_native_pipeline_layout_,
              resolve_copy_shader_code.unscaled,
              resolve_copy_shader_code.unscaled_size_bytes);
      if (resolve_copy_native_pipeline == VK_NULL_HANDLE) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the native resolve "
            "copy pipeline {}",
            resolve_copy_shader_info.debug_name);
        Shutdown();
        return false;
      }
      vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE,
                                   resolve_copy_native_pipeline,
                                   resolve_copy_shader_info.debug_name);
      resolve_copy_native_pipelines_[i] = resolve_copy_native_pipeline;
    }
  }

  VkDescriptorSetLayout direct_host_resolve_color_descriptor_set_layouts[] = {
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::
              kUniformBufferComputeB1),
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::
              kStorageBufferCompute),
      descriptor_set_layout_sampled_image_,
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::
              kStorageBufferCompute),
  };
  VkDescriptorSetLayout direct_host_resolve_depth_descriptor_set_layouts[] = {
      direct_host_resolve_color_descriptor_set_layouts[0],
      direct_host_resolve_color_descriptor_set_layouts[1],
      descriptor_set_layout_sampled_image_x2_,
      direct_host_resolve_color_descriptor_set_layouts[3],
  };
  VkPipelineLayoutCreateInfo direct_host_resolve_pipeline_layout_create_info;
  direct_host_resolve_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  direct_host_resolve_pipeline_layout_create_info.pNext = nullptr;
  direct_host_resolve_pipeline_layout_create_info.flags = 0;
  direct_host_resolve_pipeline_layout_create_info.setLayoutCount =
      uint32_t(xe::countof(direct_host_resolve_color_descriptor_set_layouts));
  direct_host_resolve_pipeline_layout_create_info.pSetLayouts =
      direct_host_resolve_color_descriptor_set_layouts;
  direct_host_resolve_pipeline_layout_create_info.pushConstantRangeCount = 1;
  direct_host_resolve_pipeline_layout_create_info.pPushConstantRanges =
      &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &direct_host_resolve_pipeline_layout_create_info, nullptr,
          &direct_host_resolve_pipeline_layout_color_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the direct host color "
        "resolve pipeline layout");
    Shutdown();
    return false;
  }
  direct_host_resolve_pipeline_layout_create_info.setLayoutCount = 4;
  direct_host_resolve_pipeline_layout_create_info.pSetLayouts =
      direct_host_resolve_depth_descriptor_set_layouts;
  if (dfn.vkCreatePipelineLayout(
          device, &direct_host_resolve_pipeline_layout_create_info, nullptr,
          &direct_host_resolve_pipeline_layout_depth_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the direct host depth "
        "resolve pipeline layout");
    Shutdown();
    return false;
  }
  direct_host_resolve_constants_pool_ =
      std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
          vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

  // TODO(Triang3l): All paths (FSI).

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    // Store k_8_8_8_8_GAMMA as linear in R16G16B16A16_UNORM for conceptually
    // correct blending in linear color space, with the linear <-> gamma color
    // space conversion done in the pixel shader output, ownership transfer,
    // resolve dump and clear paths. Requires the format to be usable as a
    // blendable color attachment and as a sampled image (for transfers/dumps).
    constexpr VkFormatFeatureFlags kGammaUnorm16Features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    VkFormatProperties gamma_unorm16_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                            VK_FORMAT_R16G16B16A16_UNORM,
                                            &gamma_unorm16_properties);
    gamma_render_target_as_unorm16_ =
        cvars::gamma_render_target_as_unorm16 &&
        (gamma_unorm16_properties.optimalTilingFeatures &
         kGammaUnorm16Features) == kGammaUnorm16Features;

    depth_float24_round_ = cvars::depth_float24_round;
    // In-PS conversion requires per-sample shading under MSAA for intersections
    // to antialias; without sampleRateShading, fall back to transfer-time
    // conversion so the host/PS encoding stays consistent across all draws.
    depth_float24_convert_in_pixel_shader_ =
        cvars::depth_float24_convert_in_pixel_shader &&
        device_properties.sampleRateShading;

    // Host depth storing pipeline layout.
    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        // Destination EDRAM storage buffer.
        descriptor_set_layout_storage_buffer_,
        // Source depth / stencil texture (only depth is used).
        descriptor_set_layout_sampled_image_x2_,
    };
    VkPushConstantRange host_depth_store_push_constant_range;
    host_depth_store_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    host_depth_store_push_constant_range.offset = 0;
    host_depth_store_push_constant_range.size = sizeof(HostDepthStoreConstants);
    VkPipelineLayoutCreateInfo host_depth_store_pipeline_layout_create_info;
    host_depth_store_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    host_depth_store_pipeline_layout_create_info.pNext = nullptr;
    host_depth_store_pipeline_layout_create_info.flags = 0;
    host_depth_store_pipeline_layout_create_info.setLayoutCount =
        uint32_t(xe::countof(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &host_depth_store_pipeline_layout_create_info, nullptr,
            &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the host depth storing "
          "pipeline layout");
      Shutdown();
      return false;
    }
    constexpr std::pair<const uint32_t*, size_t> host_depth_store_shaders[] = {
        {shaders::host_depth_store_1xmsaa_cs,
         sizeof(shaders::host_depth_store_1xmsaa_cs)},
        {shaders::host_depth_store_2xmsaa_cs,
         sizeof(shaders::host_depth_store_2xmsaa_cs)},
        {shaders::host_depth_store_4xmsaa_cs,
         sizeof(shaders::host_depth_store_4xmsaa_cs)},
    };
    for (size_t i = 0; i < xe::countof(host_depth_store_shaders); ++i) {
      const std::pair<const uint32_t*, size_t> host_depth_store_shader =
          host_depth_store_shaders[i];
      // Host depth store shaders use 8x8 = 64 threads per group. Request wave64
      // mode on RDNA GPUs to ensure one full wave per group.
      VkPipeline host_depth_store_pipeline =
          vulkan::CreateSourceReadingComputePipeline(
              msaa_single_sample_storage_,
              vulkan_device, host_depth_store_pipeline_layout_,
              host_depth_store_shader.first, host_depth_store_shader.second,
              nullptr, "main", 64);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the {}-sample host "
            "depth storing pipeline",
            uint32_t(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ =
        std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
            vulkan_device, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            std::max(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                     sizeof(float) * 2 * 6 *
                         Transfer::kMaxCutoutBorderRectangles *
                         xenos::kEdramTileCount));

    // Transfer vertex shader.
    transfer_passthrough_vertex_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::passthrough_position_xy_vs,
        sizeof(shaders::passthrough_position_xy_vs));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the render target "
          "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    // Transfer pipeline layouts.
    VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layouts
        [kTransferUsedDescriptorSetCount];
    VkPushConstantRange transfer_pipeline_layout_push_constant_range;
    transfer_pipeline_layout_push_constant_range.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    transfer_pipeline_layout_push_constant_range.offset = 0;
    VkPipelineLayoutCreateInfo transfer_pipeline_layout_create_info;
    transfer_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    transfer_pipeline_layout_create_info.pNext = nullptr;
    transfer_pipeline_layout_create_info.flags = 0;
    transfer_pipeline_layout_create_info.pSetLayouts =
        transfer_pipeline_layout_descriptor_set_layouts;
    transfer_pipeline_layout_create_info.pPushConstantRanges =
        &transfer_pipeline_layout_push_constant_range;
    for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      uint32_t transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      uint32_t transfer_pipeline_layout_descriptor_set_index;
      while (xe::bit_scan_forward(
          transfer_pipeline_layout_descriptor_sets_remaining,
          &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(uint32_t(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout =
            VK_NULL_HANDLE;
        switch (TransferUsedDescriptorSet(
            transfer_pipeline_layout_descriptor_set_index)) {
          case kTransferUsedDescriptorSetHostDepthBuffer:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_storage_buffer_;
            break;
          case kTransferUsedDescriptorSetHostDepthStencilTextures:
          case kTransferUsedDescriptorSetDepthStencilTextures:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_x2_;
            break;
          case kTransferUsedDescriptorSetColorTexture:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_;
            break;
          default:
            assert_unhandled_case(TransferUsedDescriptorSet(
                transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts
            [transfer_pipeline_layout_create_info.setLayoutCount++] =
                transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size = uint32_t(
          sizeof(uint32_t) *
          xe::bit_count(
              transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(
              device, &transfer_pipeline_layout_create_info, nullptr,
              &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline layout {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dump pipeline layouts.
    VkDescriptorSetLayout
        dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size =
        sizeof(uint32_t) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        uint32_t(xe::countof(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts =
        dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info,
                                   nullptr, &dump_pipeline_layout_color_) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the color render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info,
                                   nullptr, &dump_pipeline_layout_depth_) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the depth render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel (fragment) shader interlock.

    // Piecewise linear gamma is 8-bit with programmable blending.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even, converted in the
    // shader (FSI ignores depth_float24_convert_in_pixel_shader, but set it for
    // parity with the host render target path).
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // The pipeline layout and the pipelines for clearing the EDRAM buffer in
    // resolves.
    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size =
        sizeof(draw_util::ResolveClearShaderConstants);
    VkPipelineLayoutCreateInfo resolve_fsi_clear_pipeline_layout_create_info;
    resolve_fsi_clear_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_fsi_clear_pipeline_layout_create_info.pNext = nullptr;
    resolve_fsi_clear_pipeline_layout_create_info.flags = 0;
    resolve_fsi_clear_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pSetLayouts =
        &descriptor_set_layout_storage_buffer_;
    resolve_fsi_clear_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_fsi_clear_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_fsi_clear_pipeline_layout_create_info, nullptr,
            &resolve_fsi_clear_pipeline_layout_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear pipeline layout");
      Shutdown();
      return false;
    }
    // Resolve clear shaders use 8x8 = 64 threads per group. Request wave64
    // mode on RDNA GPUs to ensure one full wave per group.
    resolve_fsi_clear_32bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs),
        nullptr, "main", 64);
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs),
        nullptr, "main", 64);
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }

    // Common render pass.
    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Fragment shader interlock provides synchronization and ordering within a
    // subpass, create an external by-region dependency to maintain interlocking
    // between passes. Framebuffer-global dependencies will be made with
    // explicit barriers when the addressing of the EDRAM buffer relatively to
    // the fragment coordinates is changed.
    VkSubpassDependency fsi_subpass_dependencies[2];
    fsi_subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    fsi_subpass_dependencies[0].dstSubpass = 0;
    fsi_subpass_dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    fsi_subpass_dependencies[1] = fsi_subpass_dependencies[0];
    std::swap(fsi_subpass_dependencies[1].srcSubpass,
              fsi_subpass_dependencies[1].dstSubpass);
    VkRenderPassCreateInfo fsi_render_pass_create_info;
    fsi_render_pass_create_info.sType =
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fsi_render_pass_create_info.pNext = nullptr;
    fsi_render_pass_create_info.flags = 0;
    fsi_render_pass_create_info.attachmentCount = 0;
    fsi_render_pass_create_info.pAttachments = nullptr;
    fsi_render_pass_create_info.subpassCount = 1;
    fsi_render_pass_create_info.pSubpasses = &fsi_subpass;
    fsi_render_pass_create_info.dependencyCount =
        uint32_t(xe::countof(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr,
                               &fsi_render_pass_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend render pass");
      Shutdown();
      return false;
    }

    // Common framebuffer.
    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType =
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width = std::min(
        xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_x(),
        device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height = std::min(
        xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
        device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  // Reset the last update structures, to keep the defaults consistent between
  // paths regardless of whether the update for the path actually modifies them.
  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::memset(last_update_framebuffer_attachments_, 0,
              sizeof(last_update_framebuffer_attachments_));
  last_update_framebuffer_ = VK_NULL_HANDLE;

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!snapshot_quarantine_.empty()) {
    const auto wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        vulkan_device->vulkan_instance()->functions().vkGetDeviceProcAddr(
            device, "vkDeviceWaitIdle"));
    const VkResult idle = wait_idle ? wait_idle(device) : VK_ERROR_INITIALIZATION_FAILED;
    if (idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST) {
      for (const auto& allocation : snapshot_quarantine_) {
        dfn.vkDestroyBuffer(device, allocation.buffer, nullptr);
        dfn.vkFreeMemory(device, allocation.memory, nullptr);
      }
      snapshot_quarantine_.clear();
    }
  }


  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         stencil_copy_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         stencil_copy_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         depth_materialize_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         depth_materialize_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         depth_surface_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         depth_surface_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         depth_surface_render_pass_);
  depth_surface_pipeline_attempted_ = false;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         color_materialize_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         color_surface_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         color_surface_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         color_surface_render_pass_);
  color_surface_pipeline_attempted_ = false;
  pending_clear_render_target_ = nullptr;
  pending_clear_transfers_.clear();
  deferred_clear_render_target_ = nullptr;
  deferred_clear_transfers_.clear();
  deferred_clear_in_pass_ = false;
  pending_constant_rows_.clear();
  lazy_clear_owners_ = {};
  // Destroy all render targets before the descriptor set pool is destroyed -
  // may happen if shutting down the VulkanRenderTargetCache by destroying it,
  // so ShutdownCommon is called by the RenderTargetCache destructor, when it's
  // already too late.
  DestroyAllRenderTargets(true);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_64bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_32bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_fsi_clear_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         fsi_framebuffer_.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    // May be null to prevent recreation attempts.
    if (dump_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, dump_pipeline_pair.second, nullptr);
    }
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_color_);

  for (const auto& resolve_inpass_pipeline_pair : resolve_inpass_pipelines_) {
    if (resolve_inpass_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, resolve_inpass_pipeline_pair.second,
                            nullptr);
    }
  }
  resolve_inpass_pipelines_.clear();
  for (size_t i = 0; i < xe::countof(resolve_inpass_shaders_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                           resolve_inpass_shaders_[i]);
  }
  for (auto& shader : resolve_inpass_r32_uint_shaders_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                           shader);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         resolve_inpass_vertex_shader_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_inpass_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_input_attachment_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      // May be null to prevent recreation attempts.
      if (transfer_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, transfer_pipeline, nullptr);
      }
    }
  }
  transfer_pipelines_.clear();
  for (const auto& transfer_shader_pair : transfer_shaders_) {
    if (transfer_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, transfer_shader_pair.second, nullptr);
    }
  }
  transfer_shaders_.clear();
  for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           transfer_pipeline_layouts_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           host_depth_store_pipelines_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         host_depth_store_pipeline_layout_);

  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    command_processor_.ForgetRenderTargetFramebuffer(
        &framebuffer_pair.second);
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer,
                             nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    if (render_pass_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
    }
  }
  render_passes_.clear();
  render_pass_keys_.clear();

  for (VkPipeline& resolve_copy_native_pipeline :
       resolve_copy_native_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           resolve_copy_native_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_native_pipeline_layout_);
  for (VkPipeline& resolve_copy_pipeline : resolve_copy_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           resolve_copy_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_pipeline_layout_);

  for (auto& bpp_pipelines : direct_host_resolve_pipelines_) {
    for (auto& msaa_pipelines : bpp_pipelines) {
      for (auto& scaled_pipelines : msaa_pipelines) {
        for (VkPipeline& pipeline : scaled_pipelines) {
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                                 pipeline);
        }
      }
    }
  }
  for (auto& msaa_pipelines : direct_host_color_full_resolve_pipelines_) {
    for (auto& scaled_pipelines : msaa_pipelines) {
      for (auto& source_pipelines : scaled_pipelines) {
        for (VkPipeline& pipeline : source_pipelines) {
          ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                                 pipeline);
        }
      }
    }
  }
  for (auto& msaa_pipelines : direct_host_depth_resolve_pipelines_) {
    for (VkPipeline& pipeline : msaa_pipelines) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                             pipeline);
    }
  }
  direct_host_resolve_constants_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipelineLayout, device,
      direct_host_resolve_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipelineLayout, device,
      direct_host_resolve_pipeline_layout_color_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         edram_storage_buffer_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         edram_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_input_attachment_.reset();
  descriptor_set_pool_sampled_image_.reset();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_sampled_image_x2_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_sampled_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

// Candidate methods for VulkanRenderTargetCache; called ONLY on CP worker
// while guest writers are frozen. Initial support is interlock at 1x.
bool VulkanRenderTargetCache::CaptureRuntimeEdram(std::vector<uint8_t>& bytes) {
  if (GetPath() != Path::kPixelShaderInterlock ||
      draw_resolution_scale_x() != 1 || draw_resolution_scale_y() != 1) {
    return false; // Host RT ownership/image reconstruction is not implemented.
  }
  command_processor_.EndRenderPass();
  if (!command_processor_.BeginSubmission(false)) return false;
  auto* device = command_processor_.GetVulkanDevice();
  const auto& fn = device->functions();
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory allocation = VK_NULL_HANDLE;
  uint32_t memory_type = UINT32_MAX;
  VkDeviceSize allocation_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, xenos::kEdramSizeBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, staging, allocation,
          &memory_type, &allocation_size)) return false;
  UseEdramBuffer(EdramBufferUsage::kTransferRead);
  command_processor_.SubmitBarriers(true);
  VkBufferCopy region{0,0,xenos::kEdramSizeBytes};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      edram_buffer_, staging, 1, &region);
  command_processor_.PushBufferMemoryBarrier(
      staging,0,VK_WHOLE_SIZE,VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);
  if (!command_processor_.EndSubmission(false) ||
      !command_processor_.AwaitAllQueueOperationsCompletion()) {
    // Submission may still reference staging. Quarantine until Shutdown.
    snapshot_quarantine_.push_back({staging,allocation});
    return false;
  }
  void* mapped = nullptr;
  bool ok = fn.vkMapMemory(device->device(),allocation,0,VK_WHOLE_SIZE,0,
                           &mapped) == VK_SUCCESS;
  if (ok) {
    ui::vulkan::util::InvalidateMappedMemoryRange(
        device,allocation,memory_type,0,allocation_size,xenos::kEdramSizeBytes);
    bytes.resize(xenos::kEdramSizeBytes);
    std::memcpy(bytes.data(),mapped,bytes.size());
    fn.vkUnmapMemory(device->device(),allocation);
  }
  fn.vkDestroyBuffer(device->device(),staging,nullptr);
  fn.vkFreeMemory(device->device(),allocation,nullptr);
  return command_processor_.BeginSubmission(false) && ok;
}

bool VulkanRenderTargetCache::RestoreRuntimeEdram(std::span<const uint8_t> bytes) {
  if (GetPath()!=Path::kPixelShaderInterlock || draw_resolution_scale_x()!=1 ||
      draw_resolution_scale_y()!=1 || bytes.size()!=xenos::kEdramSizeBytes) return false;
  command_processor_.EndRenderPass();
  if (!command_processor_.EndSubmission(false) ||
      !command_processor_.AwaitAllQueueOperationsCompletion()) return false;
  auto* device=command_processor_.GetVulkanDevice();
  const auto& fn=device->functions();
  VkBuffer staging=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;
  uint32_t memory_type=UINT32_MAX;VkDeviceSize allocation_size=0;
  if(!ui::vulkan::util::CreateDedicatedAllocationBuffer(
      device,bytes.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      ui::vulkan::util::MemoryPurpose::kUpload,staging,allocation,
      &memory_type,&allocation_size))return false;
  void* mapped=nullptr;
  if(fn.vkMapMemory(device->device(),allocation,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS){
    fn.vkDestroyBuffer(device->device(),staging,nullptr);
    fn.vkFreeMemory(device->device(),allocation,nullptr);return false;
  }
  std::memcpy(mapped,bytes.data(),bytes.size());
  ui::vulkan::util::FlushMappedMemoryRange(
      device,allocation,memory_type,0,allocation_size,bytes.size());
  fn.vkUnmapMemory(device->device(),allocation);
  if(!command_processor_.BeginSubmission(false)){
    fn.vkDestroyBuffer(device->device(),staging,nullptr);
    fn.vkFreeMemory(device->device(),allocation,nullptr);return false;
  }
  UseEdramBuffer(EdramBufferUsage::kTransferWrite);
  command_processor_.SubmitBarriers(true);
  VkBufferCopy region{0,0,bytes.size()};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      staging,edram_buffer_,1,&region);
  // Force a transfer-write -> future fragment/compute dependency even if the
  // immediately following operation is another write of the same usage.
  edram_buffer_modification_status_=EdramBufferModificationStatus::kViaUnordered;
  if(!command_processor_.EndSubmission(false)||
     !command_processor_.AwaitAllQueueOperationsCompletion()){
    snapshot_quarantine_.push_back({staging,allocation});return false;
  }
  fn.vkDestroyBuffer(device->device(),staging,nullptr);
  fn.vkFreeMemory(device->device(),allocation,nullptr);
  return command_processor_.BeginSubmission(false);
}

bool VulkanRenderTargetCache::CaptureRuntimeHostTargets(
    std::vector<uint8_t>& bytes) {
  return TransferRuntimeHostTargets(bytes, false);
}

bool VulkanRenderTargetCache::RestoreRuntimeHostTargets(
    std::span<const uint8_t> bytes) {
  if (bytes.size() != xenos::kEdramSizeBytes) return false;
  std::vector<uint8_t> upload(bytes.begin(), bytes.end());
  return TransferRuntimeHostTargets(upload, true);
}

bool VulkanRenderTargetCache::TransferRuntimeHostTargets(
    std::vector<uint8_t>& bytes, bool restore) {
  if (!cvars::savestate_enable || GetPath() != Path::kHostRenderTargets ||
      draw_resolution_scale_x() != 1 || draw_resolution_scale_y() != 1 ||
      (restore && bytes.size() != xenos::kEdramSizeBytes)) return false;
  if (restore) {
    // Arbitrary words replace the whole EDRAM.
    SetEdramStencilMaybeSetBits(0, xenos::kEdramTileCount, 0xFF);
    ForgetAllRenderTargetConstants();
  }
  command_processor_.EndRenderPass();
  if (!command_processor_.BeginSubmission(false)) return false;
  FlushDrawClearChain();
  FlushPendingClearTransfers();
  if (!FlushPendingDrawPassTransfers()) return false;
  command_processor_.EndRenderPass();

  RenderTargetKey key;
  key.pitch_tiles_at_32bpp = 16;
  key.resource_format = uint32_t(GetColorResourceFormat(
      xenos::ColorRenderTargetFormat::k_8_8_8_8));
  auto* canonical = static_cast<VulkanRenderTarget*>(GetOrCreateRenderTarget(key));
  if (!canonical || !canonical->transfer_src_enabled() ||
      key.GetWidth() != 1280 || GetRenderTargetHeight(key) != 2048) return false;
  auto* device = command_processor_.GetVulkanDevice();
  const auto& fn = device->functions();
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory allocation = VK_NULL_HANDLE;
  uint32_t memory_type = UINT32_MAX;
  VkDeviceSize allocation_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, xenos::kEdramSizeBytes,
          restore ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          restore ? ui::vulkan::util::MemoryPurpose::kUpload
                  : ui::vulkan::util::MemoryPurpose::kReadback,
          staging, allocation, &memory_type, &allocation_size)) return false;
  auto release = [&] {
    fn.vkDestroyBuffer(device->device(), staging, nullptr);
    fn.vkFreeMemory(device->device(), allocation, nullptr);
  };
  if (restore) {
    // The whole EDRAM is rewritten: no lazy constant rows survive it.
    pending_constant_rows_.clear();
    void* mapped = nullptr;
    if (fn.vkMapMemory(device->device(), allocation, 0, VK_WHOLE_SIZE, 0,
                       &mapped) != VK_SUCCESS) { release(); return false; }
    std::memcpy(mapped, bytes.data(), bytes.size());
    ui::vulkan::util::FlushMappedMemoryRange(
        device, allocation, memory_type, 0, allocation_size, bytes.size());
    fn.vkUnmapMemory(device->device(), allocation);
  }
  // Capture keeps the previous ownership map until all transfers succeeded.
  // On failure the source images still own valid data, and must remain owners.
  auto previous_ownership = ownership_ranges_;
  if (!restore) {
    std::vector<Transfer> transfers;
    ChangeOwnership(key, 0, xenos::kEdramTileCount, &transfers);
    RenderTarget* target = canonical;
    if (!PerformTransfersAndResolveClears(1, &target, &transfers)) {
      ownership_ranges_ = std::move(previous_ownership);
      ++ownership_ranges_version_;
      release();
      return false;
    }
    command_processor_.EndRenderPass();
  }
  auto range = ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageLayout layout = restore ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                 : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAccessFlags access = restore ? VK_ACCESS_TRANSFER_WRITE_BIT
                                : VK_ACCESS_TRANSFER_READ_BIT;
  command_processor_.PushImageMemoryBarrier(
      canonical->image(), range, canonical->current_stage_mask(),
      VK_PIPELINE_STAGE_TRANSFER_BIT, canonical->current_access_mask(), access,
      canonical->current_layout(), layout);
  canonical->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, access, layout);
  command_processor_.SubmitBarriers(true);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {1280, 2048, 1};
  auto& commands = command_processor_.deferred_command_buffer();
  if (restore) {
    commands.CmdVkCopyBufferToImage(staging, canonical->image(), layout, 1, &region);
  } else {
    commands.CmdVkCopyImageToBuffer(canonical->image(), layout, staging, 1, &region);
    command_processor_.PushBufferMemoryBarrier(
        staging, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  }
  if (!command_processor_.EndSubmission(false) ||
      !command_processor_.AwaitAllQueueOperationsCompletion()) {
    snapshot_quarantine_.push_back({staging, allocation});
    // Device completion failure is fatal to the enclosing transaction; guest
    // threads must remain stopped, including if rollback cannot complete.
    return false;
  }
  bool ok = true;
  if (!restore) {
    void* mapped = nullptr;
    ok = fn.vkMapMemory(device->device(), allocation, 0, VK_WHOLE_SIZE, 0,
                        &mapped) == VK_SUCCESS;
    if (ok) {
      ui::vulkan::util::InvalidateMappedMemoryRange(
          device, allocation, memory_type, 0, allocation_size, xenos::kEdramSizeBytes);
      bytes.resize(xenos::kEdramSizeBytes);
      std::memcpy(bytes.data(), mapped, bytes.size());
      fn.vkUnmapMemory(device->device(), allocation);
    }
  } else {
    // Establish canonical ownership only after the GPU upload completed.
    ownership_ranges_.clear();
    ownership_ranges_.emplace(0, OwnershipRange(xenos::kEdramTileCount, key,
                                               RenderTargetKey(), RenderTargetKey()));
    ++ownership_ranges_version_;
    // The old attachment selection must be recomputed by the next draw.
    std::fill(std::begin(last_update_used_render_targets_),
              std::end(last_update_used_render_targets_), nullptr);
  }
  if (!ok) {
    ownership_ranges_ = std::move(previous_ownership);
    ++ownership_ranges_version_;
  }
  release();
  return command_processor_.BeginSubmission(false) && ok;
}

void VulkanRenderTargetCache::ClearCache() {
  // The processor closes and waits for submissions before clearing the cache.
  assert_null(pending_clear_render_target_);
  assert_null(deferred_clear_render_target_);
  pending_clear_render_target_ = nullptr;
  pending_clear_transfers_.clear();
  deferred_clear_render_target_ = nullptr;
  deferred_clear_transfers_.clear();
  deferred_clear_in_pass_ = false;
  pending_constant_rows_.clear();
  lazy_clear_owners_ = {};
  draw_clear_chain_ = DrawClearChain();
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Framebuffer objects must be destroyed because they reference views of
  // attachment images, which may be removed by the common ClearCache.
  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    command_processor_.ForgetRenderTargetFramebuffer(
        &framebuffer_pair.second);
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer,
                             nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
  }
  render_passes_.clear();
  render_pass_keys_.clear();

  RenderTargetCache::ClearCache();
}

void VulkanRenderTargetCache::CompletedSubmissionUpdated() {
  if (!materialize_descriptors_in_flight_.empty()) {
    const uint64_t completed = command_processor_.GetCompletedSubmission();
    auto& in_flight = materialize_descriptors_in_flight_;
    in_flight.erase(
        std::remove_if(in_flight.begin(), in_flight.end(),
                       [&](const std::pair<uint64_t, size_t>& entry) {
                         if (entry.first > completed) return false;
                         descriptor_set_pool_sampled_image_x2_->Free(
                             entry.second);
                         return true;
                       }),
        in_flight.end());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(
        command_processor_.GetCompletedSubmission());
  }
  if (direct_host_resolve_constants_pool_) {
    direct_host_resolve_constants_pool_->Reclaim(
        command_processor_.GetCompletedSubmission());
  }
  RetireUnownedRenderTargets();
}

bool VulkanRenderTargetCache::HasPendingRenderTargetReference(
    const VulkanRenderTarget* render_target) const {
  if (!render_target ||
      RenderTargetCache::HasRetainedRenderTargetReference(render_target)) {
    return true;
  }
  auto transfer_list_references = [render_target](
                                    const std::vector<Transfer>& transfers) {
    for (const Transfer& transfer : transfers) {
      if (transfer.source == render_target ||
          transfer.host_depth_source == render_target) {
        return true;
      }
    }
    return false;
  };
  if (pending_clear_render_target_ == render_target ||
      transfer_list_references(pending_clear_transfers_) ||
      deferred_clear_render_target_ == render_target ||
      transfer_list_references(deferred_clear_transfers_) ||
      HasPendingConstantRows(render_target)) {
    return true;
  }
  for (const RenderTarget* attachment : last_update_framebuffer_attachments_) {
    if (attachment == render_target) {
      return true;
    }
  }
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (pending_draw_pass_render_targets_[i] == render_target ||
        pending_out_of_pass_render_targets_[i] == render_target ||
        draw_clear_chain_.render_targets[i] == render_target ||
        transfer_list_references(pending_draw_pass_transfers_[i]) ||
        transfer_list_references(pending_out_of_pass_transfers_[i]) ||
        transfer_list_references(draw_clear_chain_.transfers[i])) {
      return true;
    }
  }
  for (const TransferInvocation& invocation : current_transfer_invocations_) {
    if (invocation.transfer.source == render_target ||
        invocation.transfer.host_depth_source == render_target) {
      return true;
    }
  }
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    if (rectangle.render_target == render_target) {
      return true;
    }
  }
  for (const DumpInvocation& invocation : dump_invocations_) {
    if (invocation.rectangle.render_target == render_target) {
      return true;
    }
  }
  return false;
}

bool VulkanRenderTargetCache::HasExternalRenderTargetReferences(
    const RenderTarget* render_target) const {
  if (!render_target) {
    return true;
  }
  for (const auto& source : deferred_resolve_sources_) {
    if (source.target == render_target) return true;
  }
  const auto* vulkan_render_target =
      static_cast<const VulkanRenderTarget*>(render_target);
  return !vulkan_render_target->external_alias_leases_known() ||
         vulkan_render_target->external_alias_lease_count() != 0;
}

bool VulkanRenderTargetCache::FramebufferReferencesRenderTarget(
    const Framebuffer& framebuffer,
    const RenderTargetKey& render_target_key) const {
  return xe::gpu::vulkan::FramebufferReferencesRenderTarget(
      framebuffer.attachment_keys, framebuffer.attachment_used_mask,
      uint32_t(xe::countof(framebuffer.attachment_keys)), render_target_key);
}

bool VulkanRenderTargetCache::EvictFramebuffersForRenderTarget(
    const VulkanRenderTarget& render_target,
    uint64_t completed_submission, uint64_t current_submission,
    bool submission_open) {
  const RenderTargetKey& render_target_key = render_target.key();
  for (const auto& framebuffer_pair : framebuffers_) {
    const Framebuffer& framebuffer = framebuffer_pair.second;
    if (!FramebufferReferencesRenderTarget(framebuffer, render_target_key)) {
      continue;
    }
    if (!xe::gpu::vulkan::SubmissionReferenceComplete(
            framebuffer.last_gpu_reference_submission_known,
            framebuffer.last_gpu_reference_submission,
            completed_submission) ||
        (submission_open &&
         framebuffer.last_gpu_reference_submission >= current_submission) ||
        &framebuffer == last_update_framebuffer_ ||
        command_processor_.IsRenderTargetFramebufferInUse(&framebuffer)) {
      return false;
    }
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  auto it = framebuffers_.begin();
  while (it != framebuffers_.end()) {
    if (!FramebufferReferencesRenderTarget(it->second, render_target_key)) {
      ++it;
      continue;
    }
    command_processor_.ForgetRenderTargetFramebuffer(&it->second);
    dfn.vkDestroyFramebuffer(device, it->second.framebuffer, nullptr);
    it = framebuffers_.erase(it);
  }
  return true;
}

void VulkanRenderTargetCache::RetireUnownedRenderTargets() {
  if (!retirement_tracking_enabled_ || !cvars::vulkan_rt_retirement ||
      command_processor_.in_render_pass()) {
    return;
  }
  const uint64_t completed_submission =
      command_processor_.GetCompletedSubmission();
  const uint64_t current_submission =
      command_processor_.GetCurrentSubmission();
  const uint64_t current_frame = command_processor_.GetCurrentFrame();
  if (last_retirement_scan_submission_ == completed_submission) {
    return;
  }
  last_retirement_scan_submission_ = completed_submission;

  uint64_t retired_count = 0;
  uint64_t retired_bytes = 0;
  auto& render_targets = GetRenderTargetsForRetirement();
  auto it = render_targets.begin();
  while (it != render_targets.end()) {
    if (!it->second) {
      ++it;
      continue;
    }
    auto* render_target =
        static_cast<VulkanRenderTarget*>(it->second);
    RenderTargetRetirementEvidence evidence;
    bool ownership_known = false;
    evidence.has_edram_owner =
        IsRenderTargetOwned(render_target->key(), &ownership_known);
    evidence.ownership_known = ownership_known;
    evidence.cpu_references_known = true;
    evidence.has_cpu_references =
        HasPendingRenderTargetReference(render_target);
    evidence.alias_leases_known =
        render_target->external_alias_leases_known();
    evidence.external_alias_leases =
        render_target->external_alias_lease_count();
    evidence.submission_fence_known =
        render_target->gpu_reference_submission_known();
    evidence.last_gpu_reference_submission =
        render_target->last_gpu_reference_submission();
    evidence.completed_submission = completed_submission;
    evidence.idle_grace_known =
        render_target->gpu_reference_frame_known();
    evidence.last_gpu_reference_frame =
        render_target->last_gpu_reference_frame();
    evidence.current_frame = current_frame;
    evidence.current_recording_known = true;
    evidence.has_current_recording_reference =
        command_processor_.submission_open() &&
        evidence.last_gpu_reference_submission >= current_submission;

    if (!evidence.CanRetireBeforeFramebufferInvalidation()) {
      ++it;
      continue;
    }
    if (!EvictFramebuffersForRenderTarget(
            *render_target, completed_submission, current_submission,
            command_processor_.submission_open())) {
      ++it;
      continue;
    }
    evidence.framebuffer_dependencies_known = true;
    evidence.has_framebuffer_dependencies = false;
    if (!evidence.CanRetire()) {
      ++it;
      continue;
    }
    retired_bytes += render_target->allocation_size();
    ++retired_count;
    delete render_target;
    it = render_targets.erase(it);
  }
  if (retired_count) {
    XELOGI("RTRETIRE completed_submission={} retired={} dedicated_bytes={}",
           completed_submission, retired_count, retired_bytes);
  }
}

void VulkanRenderTargetCache::NotePassBytesRead(
    DeferredCommandBuffer& commands, const RenderTarget* source,
    uint64_t pixels, bool resolve, uint64_t buffer_write) {
  if (!commands.pass_bytes_enabled()) return;
  using L = PassByteLedger;
  L::Event e; e.kind = L::kRead;
  e.category = resolve ? L::kResolve : L::kOwnership;
  if (source) {
    const auto& rt = *static_cast<const VulkanRenderTarget*>(source);
    auto key = rt.key();
    e.image = PassByteCatalog::Id(rt.image_);
    e.aspect = key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    e.format = key.is_depth ? GetDepthVulkanFormat(key.GetDepthFormat())
                           : GetColorVulkanFormat(key.GetColorFormat());
    e.bpp = PassByteCatalog::FormatBytes(VkFormat(e.format));
    e.samples = msaa_single_sample_storage_ ? 1 :
        (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_
             ? 4 : 1u << uint32_t(key.msaa_samples));
    e.image_w = key.GetWidth() * GetKeyScaleX(key);
    e.image_h = GetRenderTargetHeight(key) * GetKeyScaleY(key);
    e.rect = {0, 0, e.image_w, e.image_h};
    e.read = pixels * e.bpp * e.samples;
    e.conservative = true;
  }
  e.write = buffer_write; commands.AnnotatePassBytes(e);
  L::Event tag; tag.kind = L::kTag; commands.AnnotatePassBytes(tag);
}

void VulkanRenderTargetCache::DescribePassBytes(PassByteCatalog& c) const {
  for (const auto& [key, base] : render_targets_) {
    const auto& rt = *static_cast<const VulkanRenderTarget*>(base);
    VkFormat format = key.is_depth ? GetDepthVulkanFormat(key.GetDepthFormat())
                                  : GetColorVulkanFormat(key.GetColorFormat());
    PassByteCatalog::Image info;
    info.image = PassByteCatalog::Id(rt.image_);
    info.format = format; info.bpp = PassByteCatalog::FormatBytes(format);
    info.samples = msaa_single_sample_storage_ ? 1 :
        (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_attachments_supported_
             ? 4 : uint32_t(1) << uint32_t(key.msaa_samples));
    info.width = key.GetWidth() * GetKeyScaleX(key);
    info.height = GetRenderTargetHeight(key) * GetKeyScaleY(key);
    c.images[info.image] = info;
    for (VkImageView view : {rt.view_depth_color_, rt.view_depth_stencil_,
                             rt.view_stencil_, rt.view_color_transfer_separate_})
      if (view != VK_NULL_HANDLE) c.views[PassByteCatalog::Id(view)] = info;
  }
  for (const auto& [key, fb] : framebuffers_) {
    auto& views = c.framebuffers[PassByteCatalog::Id(fb.framebuffer)];
    for (uint32_t i = 0; i < 5; ++i) {
      if (!(fb.attachment_used_mask & (1u << i))) continue;
      auto rt = render_targets_.find(fb.attachment_keys[i]);
      if (rt == render_targets_.end()) { views.push_back(0); continue; }
      const auto& target = *static_cast<const VulkanRenderTarget*>(rt->second);
      // Match GetHostRenderTargetsFramebuffer without view getters, which
      // would mark GPU references when retirement tracking is enabled.
      const uint32_t mode = key.render_pass_key.color_rts_view_mode;
      VkImageView view = !i ? target.view_depth_stencil_ :
          ((mode == kColorRTViewModeTransferAll ||
            mode == kColorRTViewModeR32UintFirst + (i - 1)) &&
           target.view_color_transfer_separate_ != VK_NULL_HANDLE)
              ? target.view_color_transfer_separate_ : target.view_depth_color_;
      views.push_back(PassByteCatalog::Id(view));
    }
  }
  for (const auto& [cache_key, rp] : render_passes_) {
    // Low 32 bits: the compatibility key; high 32 bits: the classic pass op
    // overrides (RenderPassOps, aspects depth, stencil, color 0..3).
    RenderPassKey key;
    key.key = uint32_t(cache_key);
    RenderPassOps pass_ops;
    pass_ops.key = uint32_t(cache_key >> 32);
    auto& ops = c.render_passes[PassByteCatalog::Id(rp)];
    for (uint32_t i = 0; i < 5; ++i) {
      if (!(key.depth_and_color_used & (1u << i))) continue;
      uint32_t base_load = (key.depth_and_color_load_dont_care & (1u << i))
          ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
      auto load_of = [&](uint32_t aspect) -> uint32_t {
        switch (pass_ops.load(aspect)) {
          case RenderPassOps::kLoadNone: return VK_ATTACHMENT_LOAD_OP_NONE_EXT;
          case RenderPassOps::kLoadClear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
          case RenderPassOps::kLoadDontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          default: return base_load;
        }
      };
      auto store_of = [&](uint32_t aspect) -> uint32_t {
        switch (pass_ops.store(aspect)) {
          case RenderPassOps::kStoreNone: return VK_ATTACHMENT_STORE_OP_NONE;
          case RenderPassOps::kStoreDontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
          default: return VK_ATTACHMENT_STORE_OP_STORE;
        }
      };
      const uint32_t aspect = i ? i + 1 : 0;
      ops.push_back({load_of(aspect), store_of(aspect), load_of(i ? aspect : 1),
                     store_of(i ? aspect : 1), i ? i - 1 : 0});
    }
  }
  c.framebuffers[PassByteCatalog::Id(fsi_framebuffer_.framebuffer)] = {};
  c.render_passes[PassByteCatalog::Id(fsi_render_pass_)] = {};
}

void VulkanRenderTargetCache::EndSubmission() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->FlushWrites();
  }
  if (direct_host_resolve_constants_pool_) {
    direct_host_resolve_constants_pool_->FlushWrites();
  }
}

size_t DirectHostResolveMsaaIndex(xenos::MsaaSamples msaa_samples) {
  switch (msaa_samples) {
    case xenos::MsaaSamples::k1X:
      return 0;
    case xenos::MsaaSamples::k2X:
      return 1;
    case xenos::MsaaSamples::k4X:
      return 2;
    default:
      return SIZE_MAX;
  }
}

bool IsResolveDirectHostRTFastCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
    case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
      return true;
    default:
      return false;
  }
}

size_t DirectHostResolveFullDestIndex(
    draw_util::ResolveCopyShaderIndex shader) {
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return 0;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
      return 1;
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
      return 2;
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 3;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 4;
    default:
      return 5;
  }
}

bool IsResolveDirectHostRTFullColorCandidate(
    draw_util::ResolveCopyShaderIndex shader) {
  return DirectHostResolveFullDestIndex(shader) < 5;
}

bool IsResolveDirectHostRTFullColorSourcePackable(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

uint32_t DirectHostResolvePixelsPerThread(
    draw_util::ResolveCopyShaderIndex shader, bool source_is_64bpp,
    bool resolve_is_depth, xenos::MsaaSamples msaa_samples) {
  if (resolve_is_depth) {
    return 8u;
  }
  switch (shader) {
    case draw_util::ResolveCopyShaderIndex::kFull8bpp:
      return msaa_samples >= xenos::MsaaSamples::k4X ? 4u : 8u;
    case draw_util::ResolveCopyShaderIndex::kFull128bpp:
      return 2u;
    case draw_util::ResolveCopyShaderIndex::kFull16bpp:
    case draw_util::ResolveCopyShaderIndex::kFull32bpp:
    case draw_util::ResolveCopyShaderIndex::kFull64bpp:
      return 4u;
    default:
      return source_is_64bpp ? 4u : 8u;
  }
}

VkPipeline VulkanRenderTargetCache::GetDirectHostResolvePipeline(
    bool is_64bpp, xenos::MsaaSamples msaa_samples, bool scaled,
    bool source_is_uint, bool source_2x2) {
  size_t msaa_index = source_2x2 ? kDirectHostResolveMsaa2x2Index
                                 : DirectHostResolveMsaaIndex(msaa_samples);
  if (msaa_index >= kDirectHostResolveMsaaCount) {
    return VK_NULL_HANDLE;
  }
  VkPipeline& pipeline =
      direct_host_resolve_pipelines_[is_64bpp ? 1u : 0u][msaa_index]
                                    [scaled ? 1u : 0u]
                                    [source_is_uint ? 1u : 0u];
  if (pipeline != VK_NULL_HANDLE) {
    return pipeline;
  }
  const DirectHostResolveShaderCode& shader =
      kDirectHostResolveColorShaders[is_64bpp ? 1u : 0u][msaa_index]
                                          [scaled ? 1u : 0u]
                                          [source_is_uint ? 1u : 0u];
  pipeline = vulkan::CreateSourceReadingComputePipeline(
          msaa_single_sample_storage_,
      command_processor_.GetVulkanDevice(),
      direct_host_resolve_pipeline_layout_color_,
      shader.code,
      shader.size_bytes, nullptr, "main", 64);
  if (pipeline == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanRenderTargetCache: Failed to create optional direct host "
        "resolve pipeline {}",
        shader.debug_name);
    return VK_NULL_HANDLE;
  }
  command_processor_.GetVulkanDevice()->SetObjectName(
      VK_OBJECT_TYPE_PIPELINE, pipeline, shader.debug_name);
  return pipeline;
}

VkPipeline VulkanRenderTargetCache::GetDirectHostColorFullResolvePipeline(
    xenos::MsaaSamples msaa_samples, bool scaled, bool source_is_uint,
    draw_util::ResolveCopyShaderIndex copy_shader, bool source_2x2) {
  size_t msaa_index = source_2x2 ? kDirectHostResolveMsaa2x2Index
                                 : DirectHostResolveMsaaIndex(msaa_samples);
  size_t dest_index = DirectHostResolveFullDestIndex(copy_shader);
  if (msaa_index >= kDirectHostResolveMsaaCount ||
      dest_index >= kDirectHostResolveFullDestCount) {
    return VK_NULL_HANDLE;
  }
  VkPipeline& pipeline =
      direct_host_color_full_resolve_pipelines_[msaa_index]
                                               [scaled ? 1u : 0u]
                                               [source_is_uint ? 1u : 0u]
                                               [dest_index];
  if (pipeline != VK_NULL_HANDLE) {
    return pipeline;
  }
  const DirectHostResolveShaderCode& shader =
      kDirectHostResolveColorFullShaders[msaa_index][scaled ? 1u : 0u]
                                              [source_is_uint ? 1u : 0u]
                                              [dest_index];
  pipeline = vulkan::CreateSourceReadingComputePipeline(
          msaa_single_sample_storage_,
      command_processor_.GetVulkanDevice(),
      direct_host_resolve_pipeline_layout_color_,
      shader.code,
      shader.size_bytes, nullptr, "main", 64);
  if (pipeline == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanRenderTargetCache: Failed to create optional direct host full "
        "color resolve pipeline {}",
        shader.debug_name);
    return VK_NULL_HANDLE;
  }
  command_processor_.GetVulkanDevice()->SetObjectName(
      VK_OBJECT_TYPE_PIPELINE, pipeline, shader.debug_name);
  return pipeline;
}

VkPipeline VulkanRenderTargetCache::GetDirectHostDepthResolvePipeline(
    xenos::MsaaSamples msaa_samples, bool scaled) {
  size_t msaa_index = DirectHostResolveMsaaIndex(msaa_samples);
  if (msaa_index >= kDirectHostResolveMsaaCount) {
    return VK_NULL_HANDLE;
  }
  VkPipeline& pipeline =
      direct_host_depth_resolve_pipelines_[msaa_index][scaled ? 1u : 0u];
  if (pipeline != VK_NULL_HANDLE) {
    return pipeline;
  }
  const DirectHostResolveShaderCode& shader =
      kDirectHostResolveDepthShaders[msaa_index][scaled ? 1u : 0u];
  pipeline = vulkan::CreateSourceReadingComputePipeline(
          msaa_single_sample_storage_,
      command_processor_.GetVulkanDevice(),
      direct_host_resolve_pipeline_layout_depth_,
      shader.code,
      shader.size_bytes, nullptr, "main", 64);
  if (pipeline == VK_NULL_HANDLE) {
    XELOGW(
        "VulkanRenderTargetCache: Failed to create optional direct host depth "
        "resolve pipeline {}",
        shader.debug_name);
    return VK_NULL_HANDLE;
  }
  command_processor_.GetVulkanDevice()->SetObjectName(
      VK_OBJECT_TYPE_PIPELINE, pipeline, shader.debug_name);
  return pipeline;
}

bool VulkanRenderTargetCache::TryInPassResolveCopy(
    const draw_util::ResolveInfo& resolve_info,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
    VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
    uint32_t& written_address_out, uint32_t& written_length_out) {
  auto reject = [&](const char* reason) {
    for (uint32_t i = 0; i < inpass_reject_reason_count_; ++i) {
      if (inpass_reject_reasons_[i] == reason) {
        ++inpass_reject_counts_[i];
        return false;
      }
    }
    if (inpass_reject_reason_count_ < kInPassRejectReasons) {
      inpass_reject_reasons_[inpass_reject_reason_count_] = reason;
      inpass_reject_counts_[inpass_reject_reason_count_] = 1;
      ++inpass_reject_reason_count_;
    }
    return false;
  };
  if (!local_read_attachments_ || GetPath() != Path::kHostRenderTargets ||
      IsDrawResolutionScaled() || resolve_info.IsCopyingDepth()) {
    return false;
  }
  if (!command_processor_.in_render_pass()) {
    return reject("no pass open");
  }
  // Multisampled sources resolve dark through this path, cause not yet known.
  if (xenos::MsaaSamples(uint32_t(
          resolve_info.color_edram_info.msaa_samples)) !=
      xenos::MsaaSamples::k1X) {
    return reject("multisampled source");
  }
  // Bitwise-equivalent color copies: single-sample selects, and at 2x also
  // the two-sample average. copy_dest_exp_bias is applied in-shader.
  bool is_single_sample = xenos::IsSingleCopySampleSelected(
      resolve_info.copy_dest_coordinate_info.copy_sample_select);
  bool is_2x_average = resolve_info.copy_dest_coordinate_info
                               .copy_sample_select ==
                           xenos::CopySampleSelect::k01 &&
                       resolve_info.color_edram_info.msaa_samples ==
                           xenos::MsaaSamples::k2X;
  if (!is_single_sample && !is_2x_average) {
    return reject("unsupported sample select");
  }
  // "Full" classifications are admitted too: exp bias / dest swap are applied
  // in-shader, and real format conversions still fail the checks below.
  if (!IsResolveDirectHostRTFastCandidate(copy_shader) &&
      !IsResolveDirectHostRTFullColorCandidate(copy_shader)) {
    return reject("not a direct-host candidate");
  }
  xenos::ColorRenderTargetFormat resolve_color_format =
      xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format);
  const bool convert_format = !xenos::IsColorResolveFormatBitwiseEquivalent(
      resolve_color_format,
      xenos::ColorFormat(resolve_info.copy_dest_info.copy_dest_format));
  if (resolve_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    return reject("gamma source");
  }
  if (convert_format) {
    return reject("format not bitwise-equivalent");
  }
  xenos::MsaaSamples resolve_msaa = resolve_info.color_edram_info.msaa_samples;
  if (resolve_msaa > xenos::MsaaSamples::k2X) {
    return reject("4x source");
  }
  uint32_t sample_select = uint32_t(
      resolve_info.copy_dest_coordinate_info.copy_sample_select);
  uint32_t first_sample_index =
      sample_select <= uint32_t(xenos::CopySampleSelect::k3)
          ? sample_select
          : (sample_select == uint32_t(xenos::CopySampleSelect::k23) ? 2u : 0u);
  if (resolve_msaa == xenos::MsaaSamples::k2X && !is_2x_average &&
      first_sample_index > 1) {
    return reject("sample index > 1");
  }
  if (is_2x_average) {
    first_sample_index = 0;
  }

  // A single source render target that fully covers the resolved span and is
  // one of the current pass's color attachments.
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(dump_base, dump_pitch, dump_rectangles_);
  if (dump_rectangles_.size() != 1) {
    return reject("multiple source rectangles");
  }
  const ResolveCopyDumpRectangle& rect = dump_rectangles_.front();
  if (rect.row_first != 0 || rect.rows != dump_rows ||
      rect.row_first_start != 0 || rect.row_last_end != dump_row_length_used) {
    return reject("partial coverage");
  }
  auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rect.render_target);
  RenderTargetKey rt_key = vulkan_rt.key();
  if (rt_key.is_depth || rt_key.msaa_samples != resolve_msaa ||
      !vulkan_rt.HasDescriptorSetInputAttachment()) {
    return reject("rt kind/msaa/descriptor mismatch");
  }
  // Match the compute path: the source's own format must be the one the resolve
  // declares, or the packing below reinterprets the tile wrongly (this also
  // keeps the source's bpp class in sync with the copy shader's).
  if (rt_key.GetColorFormat() != resolve_color_format) {
    static uint32_t mismatch_logged = 0;
    if (cvars::log_texture_reloads && mismatch_logged < 40) {
      ++mismatch_logged;
      XELOGI(
          "VkInPassMismatch #{}: source rt format {} resolve format {} "
          "{}x{} pitch {} dest 0x{:08X}",
          mismatch_logged, uint32_t(rt_key.GetColorFormat()),
          uint32_t(resolve_color_format), dump_row_length_used, dump_rows,
          dump_pitch, written_address_out);
    }
    return reject("source format mismatch");
  }
  // R32_FLOAT is admitted only when its active guest draw used the UINT alias
  // from the beginning of the pass. The UINT subpass shader reads the raw
  // word, avoiding any floating-point conversion of NaNs or subnormals.
  const bool allow_r32_uint_inpass =
      cvars::vulkan_in_pass_resolve_r32_uint &&
      r32_uint_input_attachment_supported_ && !convert_format &&
      copy_shader == draw_util::ResolveCopyShaderIndex::kFull32bpp &&
      resolve_color_format ==
          xenos::ColorRenderTargetFormat::k_32_FLOAT &&
      xenos::ColorFormat(resolve_info.copy_dest_info.copy_dest_format) ==
          xenos::ColorFormat::k_32_FLOAT &&
      resolve_info.color_edram_info.msaa_samples ==
          xenos::MsaaSamples::k1X &&
      resolve_info.copy_dest_coordinate_info.copy_sample_select ==
          xenos::CopySampleSelect::k0 &&
      resolve_info.copy_dest_info.copy_dest_exp_bias == 0 &&
      !resolve_info.copy_dest_info.copy_dest_swap &&
      !cvars::vulkan_in_pass_resolve_debug_exp_bias &&
      !cvars::vulkan_in_pass_resolve_debug_reject;
  if ((resolve_color_format ==
           xenos::ColorRenderTargetFormat::k_16_16_FLOAT ||
       resolve_color_format ==
           xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
       resolve_color_format == xenos::ColorRenderTargetFormat::k_32_FLOAT ||
       resolve_color_format ==
           xenos::ColorRenderTargetFormat::k_32_32_FLOAT) &&
      !allow_r32_uint_inpass) {
    return reject("float source: specials not preserved");
  }
  if (cvars::vulkan_in_pass_resolve_debug_reject) {
    const bool is_msaa = xenos::MsaaSamples(uint32_t(
        resolve_info.color_edram_info.msaa_samples)) !=
        xenos::MsaaSamples::k1X;
    if ((cvars::vulkan_in_pass_resolve_debug_reject & (is_msaa ? 1 : 2))) {
      return reject("debug reject mask");
    }
  }
  inpass_format_hist_[uint32_t(resolve_color_format) & 15]++;
  {
    const uint32_t combo =
        (uint32_t(resolve_color_format) & 15) |
        ((uint32_t(resolve_info.copy_dest_info.copy_dest_format) & 63) << 4) |
        ((uint32_t(resolve_info.copy_dest_info.copy_dest_exp_bias) & 63)
         << 10) |
        (uint32_t(resolve_info.copy_dest_info.copy_dest_swap) << 16) |
        (uint32_t(resolve_info.color_edram_info.msaa_samples) << 17) |
        (uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select)
         << 19);
    bool seen = false;
    for (uint32_t i = 0; i < inpass_combo_count_; ++i) {
      if (inpass_combos_[i] == combo) {
        seen = true;
        break;
      }
    }
    if (!seen && inpass_combo_count_ < 16) {
      inpass_combos_[inpass_combo_count_++] = combo;
      XELOGI(
          "VkInPassCombo: src_fmt={} dest_fmt={} exp_bias={} swap={} "
          "src64={} rect={}x{} msaa={} sel={}",
          uint32_t(resolve_color_format),
          uint32_t(resolve_info.copy_dest_info.copy_dest_format),
          int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
          uint32_t(resolve_info.copy_dest_info.copy_dest_swap),
          uint32_t(rt_key.Is64bpp()),
          resolve_info.coordinate_info.width_div_8 << 3,
          resolve_info.height_div_8 << 3,
          uint32_t(resolve_info.color_edram_info.msaa_samples),
          uint32_t(
              resolve_info.copy_dest_coordinate_info.copy_sample_select));
    }
  }
  RenderPassKey render_pass_key = last_update_render_pass_key();
  RenderTarget* const* accumulated = last_update_accumulated_render_targets();
  uint32_t color_slot = UINT32_MAX;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if ((render_pass_key.depth_and_color_used & (uint32_t(1) << (1 + i))) &&
        accumulated[1 + i] == rect.render_target) {
      color_slot = i;
      break;
    }
  }
  if (color_slot == UINT32_MAX) {
    return reject("source not a bound attachment");
  }
  const bool r32_uint_source =
      render_pass_key.color_rts_view_mode ==
      kColorRTViewModeR32UintFirst + color_slot;
  if (resolve_color_format ==
      xenos::ColorRenderTargetFormat::k_32_FLOAT) {
    if (!allow_r32_uint_inpass || !r32_uint_source ||
        !vulkan_rt.HasDescriptorSetInputAttachment(true)) {
      return reject("R32_UINT draw/pass was not selected before the draw");
    }
  }

  // The fragment shader converts its framebuffer position to the source
  // sample with one constant offset - reject mappings the EDRAM tile walk
  // makes non-affine over the resolved rectangle (checking the corners is
  // sufficient: wrapping is monotonic in both tile axes).
  bool is_64bpp = rt_key.Is64bpp();
  uint32_t tile_size_x = is_64bpp ? 40u : 80u;
  constexpr uint32_t kTileSizeY = 16u;
  uint32_t pixel_x0 = resolve_info.coordinate_info.edram_offset_x_div_8 << 3;
  uint32_t pixel_y0 = resolve_info.coordinate_info.edram_offset_y_div_8 << 3;
  uint32_t width = resolve_info.coordinate_info.width_div_8 << 3;
  uint32_t height = resolve_info.height_div_8 << 3;
  if (!width || !height) {
    return reject("empty rect");
  }
  // EDRAM sample space: at 2x the sample rows are the pixel rows doubled plus
  // the selected sample; the host image is pixel-dimensioned with the sample
  // as the texel fetch index.
  uint32_t sample_y_log2 = resolve_msaa == xenos::MsaaSamples::k2X ? 1 : 0;
  uint32_t sample_y_offset = first_sample_index & 1;
  uint32_t rt_base_tiles = rt_key.base_tiles;
  uint32_t rt_pitch_tiles = rt_key.GetPitchTiles();
  int64_t offset_x = INT64_MAX, offset_y = INT64_MAX;
  // The tile walk is linear only within a tile, so corners agreeing says
  // nothing about the interior. Check every tile the rect covers.
  const uint32_t tiles_x = (width + tile_size_x - 1) / tile_size_x;
  const uint32_t tiles_y = (height + kTileSizeY - 1) / kTileSizeY;
  for (uint32_t ty = 0; ty <= tiles_y; ++ty) {
    uint32_t py = std::min(ty * kTileSizeY, height - 1);
    for (uint32_t tx = 0; tx <= tiles_x; ++tx) {
      uint32_t px = std::min(tx * tile_size_x, width - 1);
      uint32_t sx = px + pixel_x0;
      uint32_t sy = ((py + pixel_y0) << sample_y_log2) + sample_y_offset;
      uint32_t tile_x = sx / tile_size_x, tile_y = sy / kTileSizeY;
      uint32_t nonwrapped = dump_base + tile_y * dump_pitch + tile_x;
      if (nonwrapped < rt_base_tiles) {
        return reject("tile before rt base");
      }
      uint32_t linear = nonwrapped - rt_base_tiles;
      uint32_t rt_tile_y = linear / rt_pitch_tiles;
      uint32_t rt_tile_x = linear - rt_tile_y * rt_pitch_tiles;
      int64_t texel_x =
          int64_t(rt_tile_x) * tile_size_x + (sx - tile_x * tile_size_x);
      int64_t texel_y =
          int64_t(rt_tile_y) * kTileSizeY + (sy - tile_y * kTileSizeY);
      int64_t host_x = texel_x;
      int64_t host_y = texel_y >> sample_y_log2;
      int64_t corner_offset_x = int64_t(px) - host_x;
      int64_t corner_offset_y = int64_t(py) - host_y;
      if (offset_x == INT64_MAX) {
        offset_x = corner_offset_x;
        offset_y = corner_offset_y;
      } else if (corner_offset_x != offset_x || corner_offset_y != offset_y) {
        return reject("mapping not affine");
      }
    }
  }

  // The resolve draw's fragments must stay inside the render area - a mapped
  // rectangle beyond the framebuffer extent is undefined behavior (observed as
  // VK_ERROR_DEVICE_LOST on Adreno).
  const Framebuffer* framebuffer = last_update_framebuffer();
  if (!framebuffer) {
    return reject("no framebuffer");
  }
  if (offset_x > 0 || offset_y > 0 ||
      uint64_t(-offset_x) + width > framebuffer->host_extent.width ||
      uint64_t(-offset_y) + height > framebuffer->host_extent.height) {
    return reject("mapped rect outside the render area");
  }

  // Committing the destination may trigger a shared-memory upload that ends
  // the pass (inline, non-hoistable) - re-check afterwards.
  if (!shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length)) {
    return reject("dest range not committed");
  }
  if (!command_processor_.in_render_pass()) {
    return reject("pass ended by commit");
  }

  const bool dest_is_64bpp = is_64bpp;
  VkPipeline pipeline =
      GetResolveInPassPipeline(render_pass_key, color_slot, dest_is_64bpp);
  if (pipeline == VK_NULL_HANDLE) {
    return reject("no pipeline");
  }

  {
    static std::atomic<uint32_t> dbg_inpass_count{0};
    static std::atomic<uint32_t> dbg_gameplay_count{0};
    uint32_t dbg_n = dbg_inpass_count.fetch_add(1, std::memory_order_relaxed);
    const uint32_t frame = command_processor_.GetCurrentFrame();
    const bool gameplay_sample =
        frame >= 4500 &&
        dbg_gameplay_count.fetch_add(1, std::memory_order_relaxed) < 160;
    if (dbg_n < 80 || rt_key.GetPitchTiles() != dump_pitch ||
        gameplay_sample) {
      XELOGI(
          "RTINPASS#{} frame={} dump_base={} dump_pitch={} src_pitch={} "
          "store={} tiles={}x{} rect={}x{} msaa={} 64bpp={}",
          dbg_n, frame, dump_base, dump_pitch, rt_key.GetPitchTiles(), 0u,
          tiles_x, tiles_y, width, height, uint32_t(resolve_msaa),
          uint32_t(is_64bpp));
    }
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkDescriptorSet descriptor_set_dest =
      command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout::
              kStorageBufferFragment);
  if (descriptor_set_dest == VK_NULL_HANDLE) {
    return reject("no dest descriptor");
  }
  VkDescriptorBufferInfo dest_buffer_info;
  dest_buffer_info.buffer = shared_memory.buffer();
  dest_buffer_info.offset = resolve_info.copy_dest_base;
  dest_buffer_info.range = resolve_info.copy_dest_extent_start -
                           resolve_info.copy_dest_base +
                           resolve_info.copy_dest_extent_length;
  VkWriteDescriptorSet write_descriptor_set_dest = {};
  write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_dest.dstSet = descriptor_set_dest;
  write_descriptor_set_dest.dstBinding = 0;
  write_descriptor_set_dest.descriptorCount = 1;
  write_descriptor_set_dest.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_dest.pBufferInfo = &dest_buffer_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0,
                             nullptr);

  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  // In-pass self-dependency, legal under dynamic rendering local read: order
  // previous draws' color output before the resolve's input attachment reads.
  VkMemoryBarrier local_read_barrier;
  local_read_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  local_read_barrier.pNext = nullptr;
  local_read_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  local_read_barrier.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  command_buffer.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1,
      &local_read_barrier, 0, nullptr, 0, nullptr);

  command_processor_.BindExternalGraphicsPipeline(pipeline);
  VkDescriptorSet descriptor_sets[] = {
      descriptor_set_dest,
      vulkan_rt.GetDescriptorSetInputAttachment(r32_uint_source)};
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         resolve_inpass_pipeline_layout_, 0, 2u,
                                         descriptor_sets, 0, nullptr);

  struct {
    draw_util::ResolveCopyShaderConstants base;
    int32_t frag_to_pixel_offset[2];
    int32_t host_sample;
  } push_constants;
  push_constants.base = copy_shader_constants;
  if (cvars::vulkan_in_pass_resolve_debug_exp_bias) {
    push_constants.base.dest_relative.dest_info.copy_dest_exp_bias =
        push_constants.base.dest_relative.dest_info.copy_dest_exp_bias +
        cvars::vulkan_in_pass_resolve_debug_exp_bias;
  }
  push_constants.base.dest_base -= uint32_t(dest_buffer_info.offset);
  push_constants.frag_to_pixel_offset[0] = int32_t(offset_x);
  push_constants.frag_to_pixel_offset[1] = int32_t(offset_y);
  if (resolve_msaa == xenos::MsaaSamples::k2X) {
    uint32_t host_sample_0 = draw_util::GetD3D10SampleIndexForGuest2xMSAA(
        first_sample_index, msaa_2x_attachments_supported_);
    uint32_t sample_info = host_sample_0;
    if (is_2x_average) {
      sample_info |= draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                         1, msaa_2x_attachments_supported_)
                         << 8 |
                     uint32_t(1) << 16;
    }
    push_constants.host_sample = int32_t(sample_info);
  } else {
    push_constants.host_sample = 0;
  }
  command_buffer.CmdVkPushConstants(resolve_inpass_pipeline_layout_,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(push_constants), &push_constants);

  VkViewport viewport;
  viewport.x = float(-offset_x);
  viewport.y = float(-offset_y);
  viewport.width = float(width);
  viewport.height = float(height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  command_processor_.SetViewport(viewport);
  VkRect2D scissor;
  scissor.offset.x = int32_t(-offset_x);
  scissor.offset.y = int32_t(-offset_y);
  scissor.extent.width = width;
  scissor.extent.height = height;
  command_processor_.SetScissor(scissor);

  // Render-pass state, and the pass stays open, so restore identity after.
  const uint32_t color_attachment_count =
      32 - xe::lzcnt(render_pass_key.depth_and_color_used >> 1);
  uint32_t input_attachment_indices[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    input_attachment_indices[i] = i == color_slot ? 0 : VK_ATTACHMENT_UNUSED;
  }
  command_buffer.CmdVkSetRenderingInputAttachmentIndices(
      color_attachment_count, input_attachment_indices);

  NotePassBytesRead(command_buffer, &vulkan_rt, 0, true,
                    uint64_t(width) * height *
                        (1u << draw_util::GetResolveDownscalePixelSizeLog2(resolve_info.copy_dest_info)));
  command_buffer.CmdVkDraw(3, 1, 0, 0);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    input_attachment_indices[i] = i;
  }
  command_buffer.CmdVkSetRenderingInputAttachmentIndices(
      color_attachment_count, input_attachment_indices);
  shared_memory.MarkInPassWrite(
      std::make_pair(resolve_info.copy_dest_extent_start,
                     resolve_info.copy_dest_extent_length));
  // In-pass resolves refuse scaling outright.
  texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                    resolve_info.copy_dest_extent_length,
                                    false);
  written_address_out = resolve_info.copy_dest_extent_start;
  written_length_out = resolve_info.copy_dest_extent_length;
  return true;
}

namespace {
// log_inpass_texture_loads diagnostic: every resolve destination once per
// outcome (path taken, or why the direct / dual-output path refused it).
void LogResolveSiteOnce(const draw_util::ResolveInfo& resolve_info,
                        const char* outcome, const char* detail = "") {
  if (!cvars::log_inpass_texture_loads) {
    return;
  }
  static std::unordered_set<uint64_t> logged;
  if (logged.size() >= 128) {
    return;
  }
  uint32_t dest_base = resolve_info.copy_dest_base_unadjusted & 0x1FFFFFFF;
  uint64_t site = (uint64_t(dest_base) << 32) ^
                  uint64_t(reinterpret_cast<uintptr_t>(outcome)) ^
                  (uint64_t(resolve_info.coordinate_info.width_div_8) << 20);
  if (!logged.insert(site).second) {
    return;
  }
  bool is_depth = resolve_info.IsCopyingDepth();
  XELOGI(
      "RESOLVESITE dest={:08X} {}x{} at {},{} pitch32={} src={}{} "
      "dest_fmt={} endian={} array={} exp_bias={} sample={} -> {} {}",
      dest_base, resolve_info.coordinate_info.width_div_8 << 3,
      resolve_info.height_div_8 << 3, resolve_info.copy_dest_x0,
      resolve_info.copy_dest_y0,
      resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32,
      is_depth ? "depth" : "color",
      is_depth ? uint32_t(resolve_info.depth_edram_info.format)
               : uint32_t(resolve_info.color_edram_info.format),
      uint32_t(resolve_info.copy_dest_info.copy_dest_format),
      uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
      uint32_t(resolve_info.copy_dest_info.copy_dest_array),
      int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
      uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
      outcome, detail);
}
}  // namespace

bool VulkanRenderTargetCache::TryDirectHostResolveCopy(
    const draw_util::ResolveInfo& resolve_info,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
    VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
    uint32_t& written_address_out, uint32_t& written_length_out) {
  static std::atomic<uint32_t> dbg_small_resolves{0};
  const bool dbg_small =
      command_processor_.GetCurrentFrame() >= 4500 &&
      (resolve_info.coordinate_info.width_div_8 << 3) == 256 &&
      (resolve_info.height_div_8 << 3) == 152 &&
      dbg_small_resolves.fetch_add(1, std::memory_order_relaxed) < 24;
  auto debug_fail = [&](const char* reason) {
    LogResolveSiteOnce(resolve_info, reason);
    if (dbg_small) {
      XELOGI("RTSMALL frame={} reject={} base={} pitch={} rows={} rowlen={} "
             "shader={} sample={} bias={}",
             command_processor_.GetCurrentFrame(), reason, dump_base,
             dump_pitch, dump_rows, dump_row_length_used,
             uint32_t(copy_shader),
             uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
             resolve_info.copy_dest_info.copy_dest_exp_bias);
    }
    return false;
  };
  if (GetPath() != Path::kHostRenderTargets ||
      !direct_host_resolve_constants_pool_) {
    return debug_fail("path-or-pool");
  }

  bool resolve_is_depth = resolve_info.IsCopyingDepth();
  bool copy_shader_is_fast = IsResolveDirectHostRTFastCandidate(copy_shader);
  bool copy_shader_is_full_color =
      !resolve_is_depth && IsResolveDirectHostRTFullColorCandidate(copy_shader);

  xenos::ColorRenderTargetFormat resolve_color_format =
      xenos::ColorRenderTargetFormat::k_8_8_8_8;
  xenos::DepthRenderTargetFormat resolve_depth_format =
      xenos::DepthRenderTargetFormat::kD24S8;
  if (resolve_is_depth) {
    if (!xenos::IsSingleCopySampleSelected(
            resolve_info.copy_dest_coordinate_info.copy_sample_select) ||
        !copy_shader_is_fast) {
      return debug_fail("depth-sample-or-shader");
    }
    resolve_depth_format =
        xenos::DepthRenderTargetFormat(resolve_info.depth_edram_info.format);
  } else {
    resolve_color_format =
        xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format);
    if (resolve_color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      // TODO (xenios-jp): Add the missing gamma render target format.
      return debug_fail("gamma-source");
    }
    if (copy_shader_is_fast) {
      if (!xenos::IsSingleCopySampleSelected(
              resolve_info.copy_dest_coordinate_info.copy_sample_select) ||
          resolve_info.copy_dest_info.copy_dest_exp_bias ||
          !xenos::IsColorResolveFormatBitwiseEquivalent(
              resolve_color_format,
              xenos::ColorFormat(
                  resolve_info.copy_dest_info.copy_dest_format))) {
        return debug_fail("color-sample-bias-or-format");
      }
    } else if (!copy_shader_is_full_color) {
      return debug_fail("color-shader");
    }
  }

  struct DirectHostResolveConstants {
    uint32_t dispatch_offset;
    uint32_t dump_base;
    uint32_t dump_pitch_tiles;
    uint32_t source_base_tiles;
    uint32_t source_pitch_tiles;
    uint32_t thread_count_x;
    uint32_t thread_count_y;
    uint32_t height_scaled;
    uint32_t msaa_2x_sample_0;
    uint32_t msaa_2x_sample_1;
    uint32_t flags;
  };
  constexpr uint32_t kDirectHostResolveDepthFlagHasStencil = 1u << 0;
  constexpr uint32_t kDirectHostResolveDepthFlagRoundDepth = 1u << 1;
  constexpr uint32_t kDirectHostResolveDepthFlagColorView = 1u << 2;
  constexpr uint32_t kDirectHostResolveDepthFlagFloat24 = 1u << 3;
  constexpr uint32_t kDirectHostResolveColorFlagOwnerFormat = 1u << 2;

  struct DirectHostResolveSource {
    RenderTargetKey key;
    VulkanRenderTarget* render_target = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = 0;
    uint32_t flags = 0;
    uint32_t pixels_per_thread = 0;
    // The guest's MSAA of the surface: 4x for a 2x2-kept 1x key.
    xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
    bool is_64bpp = false;
    bool is_depth = false;
    bool source_is_uint = false;
  };

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(dump_base, dump_pitch, dump_rectangles_);
  if (dbg_small) {
    XELOGI("RTSMALL frame={} rects={} base={} pitch={} rows={} rowlen={}",
           command_processor_.GetCurrentFrame(), dump_rectangles_.size(),
           dump_base, dump_pitch, dump_rows, dump_row_length_used);
  }
  if (dump_rectangles_.empty()) {
    return debug_fail("no-owned-rectangles");
  }

  uint64_t covered_tiles = 0;
  std::vector<DirectHostResolveSource> sources;
  sources.reserve(dump_rectangles_.size());
  for (const ResolveCopyDumpRectangle& rect : dump_rectangles_) {
    if (!rect.rows || rect.row_last_end <= rect.row_first_start) {
      return debug_fail("bad-rectangle");
    }
    if (rect.rows == 1) {
      covered_tiles += rect.row_last_end - rect.row_first_start;
    } else {
      covered_tiles += dump_row_length_used - rect.row_first_start;
      covered_tiles += uint64_t(rect.rows - 2) * dump_row_length_used;
      covered_tiles += rect.row_last_end;
    }

    auto* rt = static_cast<VulkanRenderTarget*>(rect.render_target);
    if (!rt) {
      return debug_fail("null-render-target");
    }
    RenderTargetKey key = rt->key();
    if (dbg_small) {
      XELOGI("RTSMALL frame={} owner_base={} owner_pitch={} owner_depth={} "
             "owner_msaa={} owner_format={} rect_rows={} rfs={} rle={}",
             command_processor_.GetCurrentFrame(), key.base_tiles,
             key.GetPitchTiles(), uint32_t(key.is_depth),
             uint32_t(key.msaa_samples), key.resource_format, rect.rows,
             rect.row_first_start, rect.row_last_end);
    }
    auto owner_fail = [&](const char* reason) {
      if (cvars::log_inpass_texture_loads) {
        std::string detail = fmt::format(
            "owner={}{} msaa={} pitch={}", key.is_depth ? "depth" : "color",
            uint32_t(key.resource_format), uint32_t(key.msaa_samples),
            key.GetPitchTiles());
        LogResolveSiteOnce(resolve_info, reason, detail.c_str());
      }
      return false;
    };
    // A color resolve of EDRAM owned by a depth buffer (bit-exact copies only)
    // or by a 32bpp color target of another format: the source texels are
    // packed as the owner's EDRAM bits, like the dump writes them, and the
    // resolve then reads those bits as its own format.
    bool owner_depth_as_color = false;
    bool owner_foreign_color = false;
    if (key.is_depth != resolve_is_depth) {
      if (!cvars::vulkan_direct_host_resolve_foreign_owner || resolve_is_depth ||
          !copy_shader_is_fast ||
          xenos::IsColorRenderTargetFormat64bpp(resolve_color_format) ||
          key.msaa_samples != resolve_info.color_edram_info.msaa_samples) {
        return owner_fail("owner-kind");
      }
      owner_depth_as_color = true;
    }

    {
      static std::atomic<uint32_t> dbg_copy_count{0};
      uint32_t dbg_n = dbg_copy_count.fetch_add(1, std::memory_order_relaxed);
      // The first ones only: a pitch mismatch can repeat every frame.
      if (dbg_n < 80) {
        XELOGI(
            "RTCOPY#{} dump_base={} dump_pitch={} src_pitch={} msaa={} "
            "64bpp={} depth={} rect_rows={} rfs={} rle={} rowlen={} rows={}",
            dbg_n, dump_base, dump_pitch, key.GetPitchTiles(),
            uint32_t(key.msaa_samples), uint32_t(key.Is64bpp()),
            uint32_t(resolve_is_depth), rect.rows, rect.row_first_start,
            rect.row_last_end, dump_row_length_used, dump_rows);
      }
    }

    bool source_is_uint = false;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t source_flags = 0;
    bool is_64bpp = false;
    xenos::MsaaSamples source_msaa = key.msaa_samples;
    if (owner_depth_as_color) {
      if (!depth_float24_convert_in_pixel_shader_ && depth_float24_round_) {
        source_flags |= kDirectHostResolveDepthFlagRoundDepth;
      }
      if (key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24FS8) {
        source_flags |= kDirectHostResolveDepthFlagFloat24;
      }
      source_flags |= kDirectHostResolveDepthFlagHasStencil |
                      kDirectHostResolveDepthFlagColorView;
      pipeline = GetDirectHostDepthResolvePipeline(key.msaa_samples,
                                                   IsDrawResolutionScaled());
    } else if (resolve_is_depth) {
      if (key.GetDepthFormat() != resolve_depth_format ||
          key.msaa_samples != resolve_info.depth_edram_info.msaa_samples) {
        return debug_fail("depth-format-or-msaa");
      }
      if (!depth_float24_convert_in_pixel_shader_ && depth_float24_round_) {
        source_flags |= kDirectHostResolveDepthFlagRoundDepth;
      }
      source_flags |= kDirectHostResolveDepthFlagHasStencil;
      pipeline = GetDirectHostDepthResolvePipeline(key.msaa_samples,
                                                   IsDrawResolutionScaled());
    } else {
      // A 1x owner of a 4x resolve - a 4x surface kept as a 1x image of
      // twice the size (vulkan_msaa_4x_as_2x2): same pitch in tiles, its
      // texels are the EDRAM samples, exactly what the dump would write.
      const bool source_2x2 =
          cvars::vulkan_direct_host_resolve_2x2 &&
          key.msaa_samples == xenos::MsaaSamples::k1X &&
          resolve_info.color_edram_info.msaa_samples ==
              xenos::MsaaSamples::k4X;
      // Keys hold the storage format: the _AS_ variants have the bits of their
      // base format (the EDRAM dump writes them alike).
      if (key.GetColorFormat() != (cvars::vulkan_direct_host_resolve_2x2
                                       ? GetColorResourceFormat(
                                             resolve_color_format)
                                       : resolve_color_format)) {
        // A k_8_8_8_8_GAMMA owner is kept linear on the host: the shader
        // encodes it back to the PWL curve like the dump.
        if (!cvars::vulkan_direct_host_resolve_foreign_owner ||
            key.Is64bpp() ||
            xenos::IsColorRenderTargetFormat64bpp(resolve_color_format) ||
            (key.GetColorFormat() !=
                 xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
             !IsResolveDirectHostRTFullColorSourcePackable(
                 key.GetColorFormat()))) {
          return owner_fail("color-format");
        }
        owner_foreign_color = true;
        source_flags |= kDirectHostResolveColorFlagOwnerFormat |
                        (uint32_t(key.GetColorFormat()) << 8);
      }
      if (key.msaa_samples != resolve_info.color_edram_info.msaa_samples &&
          !source_2x2) {
        return owner_fail("color-msaa");
      }
      if (source_2x2) {
        source_msaa = xenos::MsaaSamples::k4X;
      }
      if (copy_shader_is_full_color &&
          !IsResolveDirectHostRTFullColorSourcePackable(resolve_color_format)) {
        return debug_fail("color-not-packable");
      }
      GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(),
                                            &source_is_uint);
      is_64bpp = key.Is64bpp();
      const bool source_is_2x2 = source_msaa != key.msaa_samples;
      pipeline = copy_shader_is_full_color
                     ? GetDirectHostColorFullResolvePipeline(
                           source_msaa, IsDrawResolutionScaled(),
                           source_is_uint, copy_shader, source_is_2x2)
                     : GetDirectHostResolvePipeline(is_64bpp, source_msaa,
                                                    IsDrawResolutionScaled(),
                                                    source_is_uint,
                                                    source_is_2x2);
    }
    if (pipeline == VK_NULL_HANDLE) {
      return debug_fail("no-pipeline");
    }

    DirectHostResolveSource source;
    source.key = key;
    source.render_target = rt;
    source.pipeline = pipeline;
    source.dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, source.dispatches);
    source.flags = source_flags;
    source.pixels_per_thread = DirectHostResolvePixelsPerThread(
        copy_shader, is_64bpp, key.is_depth, source_msaa);
    source.msaa_samples = source_msaa;
    source.is_64bpp = is_64bpp;
    source.is_depth = key.is_depth;
    source.source_is_uint = source_is_uint;
    if (!source.dispatch_count) {
      return debug_fail("no-dispatch");
    }
    const uint32_t tile_size_x =
        (source.is_64bpp ? 40u : 80u) * draw_resolution_scale_x();
    const uint32_t tile_pixel_size_x =
        tile_size_x >>
        uint32_t(source.msaa_samples >= xenos::MsaaSamples::k4X);
    for (uint32_t i = 0; i < source.dispatch_count; ++i) {
      uint32_t dispatch_pixel_width =
          source.dispatches[i].width_tiles * tile_pixel_size_x;
      if (dispatch_pixel_width % source.pixels_per_thread) {
        return debug_fail("dispatch-width");
      }
    }
    // One pipeline layout (depth or color sources) per resolve.
    if (!sources.empty() && sources.front().is_depth != source.is_depth) {
      return debug_fail("mixed-owner-kind");
    }
    if (owner_depth_as_color || owner_foreign_color) {
      LogResolveSiteOnce(resolve_info, "direct-foreign-owner",
                         owner_depth_as_color ? "depth" : "color");
    }
    sources.push_back(source);
  }

  if (covered_tiles != uint64_t(dump_row_length_used) * uint64_t(dump_rows)) {
    if (dbg_small) {
      XELOGI("RTSMALL frame={} covered={} expected={}",
             command_processor_.GetCurrentFrame(), covered_tiles,
             uint64_t(dump_row_length_used) * uint64_t(dump_rows));
    }
    return debug_fail("incomplete-ownership");
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info = {};
  bool scaled_buffer_ready = false;
  if (IsDrawResolutionScaled()) {
    uint32_t dest_address = resolve_info.copy_dest_base;
    uint32_t dest_length = resolve_info.copy_dest_extent_start -
                           resolve_info.copy_dest_base +
                           resolve_info.copy_dest_extent_length;
    scaled_buffer_ready =
        texture_cache.EnsureScaledResolveMemoryCommittedPublic(dest_address,
                                                               dest_length) &&
        texture_cache.MakeScaledResolveRangeCurrent(dest_address, dest_length);
    VkBuffer scaled_buffer = scaled_buffer_ready
                                 ? texture_cache.GetCurrentScaledResolveBuffer()
                                 : VK_NULL_HANDLE;
    if (scaled_buffer_ready && scaled_buffer != VK_NULL_HANDLE) {
      uint32_t draw_resolution_scale_area =
          draw_resolution_scale_x() * draw_resolution_scale_y();
      uint64_t scaled_offset =
          uint64_t(dest_address) * draw_resolution_scale_area;
      write_descriptor_set_dest_buffer_info.buffer = scaled_buffer;
      write_descriptor_set_dest_buffer_info.offset =
          scaled_offset -
          texture_cache.GetCurrentScaledResolveBufferBaseOffset();
      write_descriptor_set_dest_buffer_info.range =
          dest_length * draw_resolution_scale_area;
    } else {
      scaled_buffer_ready = false;
    }
  }

  // A scaled shader cannot populate a native destination. Let the staged
  // path handle allocation failure; never publish a texture with mixed scale.
  if (IsDrawResolutionScaled() && !scaled_buffer_ready) {
    return false;
  }
  if (!scaled_buffer_ready) {
    if (!shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                    resolve_info.copy_dest_extent_length)) {
      return false;
    }
    write_descriptor_set_dest_buffer_info.buffer = shared_memory.buffer();
    write_descriptor_set_dest_buffer_info.offset =
        resolve_info.copy_dest_base;
    write_descriptor_set_dest_buffer_info.range =
        resolve_info.copy_dest_extent_start - resolve_info.copy_dest_base +
        resolve_info.copy_dest_extent_length;
  }

  VkDescriptorSet descriptor_set_dest = VK_NULL_HANDLE;
  descriptor_set_dest = command_processor_.AllocateSingleTransientDescriptor(
      VulkanCommandProcessor::SingleTransientDescriptorLayout::
          kStorageBufferCompute);
  if (descriptor_set_dest == VK_NULL_HANDLE) {
    return false;
  }
  VkWriteDescriptorSet write_descriptor_set_dest = {};
  write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_dest.dstSet = descriptor_set_dest;
  write_descriptor_set_dest.dstBinding = 0;
  write_descriptor_set_dest.descriptorCount = 1;
  write_descriptor_set_dest.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_dest.pBufferInfo =
      &write_descriptor_set_dest_buffer_info;
  dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0,
                             nullptr);

  // The scaled resolve buffer is bound for both reading (as the source render
  // target backing) and writing, so it needs shader-read/shader-write barriers
  // around the dispatches; shared memory uses the usage tracker instead.
  auto emit_scaled_dest_buffer_barrier = [&](VkAccessFlags src_access,
                                             VkAccessFlags dst_access) {
    VkBufferMemoryBarrier buffer_barrier = {};
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_barrier.srcAccessMask = src_access;
    buffer_barrier.dstAccessMask = dst_access;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = write_descriptor_set_dest_buffer_info.buffer;
    buffer_barrier.offset = 0;
    buffer_barrier.size = VK_WHOLE_SIZE;
    command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                        0, nullptr, 1, &buffer_barrier, 0,
                                        nullptr);
  };

  if (scaled_buffer_ready) {
    emit_scaled_dest_buffer_barrier(VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_SHADER_WRITE_BIT);
  } else {
    shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                      std::make_pair(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length));
  }

  auto resolve_texture_lock = xe::global_critical_region::AcquireDirect();
  VulkanTextureCache::ResolveTextureCopy texture_copy;
  texture_copy = texture_cache.PrepareResolveTextureCopy(
      resolve_info, IsDrawResolutionScaled(), resolve_texture_lock);
  VulkanCommandProcessor::ScratchBufferAcquisition texture_scratch;
  VkDescriptorSet linear_descriptor = descriptor_set_dest;
  if (texture_copy.size) {
    texture_scratch = command_processor_.AcquireScratchGpuBuffer(
        texture_copy.size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);
    // An allocation/descriptor failure after an earlier dispatch can leave a
    // compute write as the final use. Keep the conservative write mask until
    // completion so the next scratch acquisition cannot elide a WAW barrier.
    texture_scratch.SetAccessMask(VK_ACCESS_MEMORY_WRITE_BIT);
    VkDescriptorSet descriptor =
        command_processor_.AllocateSingleTransientDescriptor(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::
                kStorageBufferCompute);
    if (texture_scratch.buffer() != VK_NULL_HANDLE &&
        descriptor != VK_NULL_HANDLE) {
      VkDescriptorBufferInfo buffer_info = {texture_scratch.buffer(), 0,
                                            texture_copy.size};
      VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = descriptor;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &buffer_info;
      dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
      linear_descriptor = descriptor;
    } else {
      texture_copy = {};
    }
  }
  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  for (const DirectHostResolveSource& source : sources) {
    VulkanRenderTarget& rt = *source.render_target;
    command_processor_.PushImageMemoryBarrier(
        rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            source.is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        rt.current_stage_mask(), kSourceStageMask, rt.current_access_mask(),
        kSourceAccessMask, rt.current_layout(), kSourceLayout);
    rt.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
  }

  draw_util::ResolveCopyShaderConstants push_constants = copy_shader_constants;
  if (!IsDrawResolutionScaled()) {
    push_constants.dest_base -=
        uint32_t(write_descriptor_set_dest_buffer_info.offset);
  }
  // Bits 29:31 are host-only, explicitly cleared even if the guest set padding.
  push_constants.dest_relative.dest_info.value &= ~UINT32_C(0xE0000000);
  if (texture_copy.size)
    push_constants.dest_relative.dest_info.value |=
        UINT32_C(0x80000000) | texture_copy.linear_dest_flags;
  VkPipelineLayout pipeline_layout =
      sources.front().is_depth ? direct_host_resolve_pipeline_layout_depth_
                               : direct_host_resolve_pipeline_layout_color_;
  if (IsDrawResolutionScaled()) {
    command_buffer.CmdVkPushConstants(
        pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(push_constants.dest_relative), &push_constants.dest_relative);
  } else {
    command_buffer.CmdVkPushConstants(pipeline_layout,
                                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(push_constants), &push_constants);
  }

  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  size_t constants_alignment = std::max<size_t>(
      1, size_t(vulkan_device->properties().minUniformBufferOffsetAlignment));
  const uint32_t scale_x = draw_resolution_scale_x();
  const uint32_t scale_y = draw_resolution_scale_y();
  const uint32_t height_scaled =
      (resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2) *
      scale_y;
  const uint32_t msaa_2x_sample_0 =
      draw_util::GetD3D10SampleIndexForGuest2xMSAA(
          0, msaa_2x_attachments_supported_);
  const uint32_t msaa_2x_sample_1 =
      draw_util::GetD3D10SampleIndexForGuest2xMSAA(
          1, msaa_2x_attachments_supported_);

  for (const DirectHostResolveSource& source : sources) {
    command_processor_.BindExternalComputePipeline(source.pipeline);
    VkDescriptorSet source_descriptor_set =
        source.render_target->GetDescriptorSetTransferSource();
    for (uint32_t i = 0; i < source.dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = source.dispatches[i];
      DirectHostResolveConstants constants = {};
      constants.dispatch_offset = dispatch.offset;
      constants.dump_base = dump_base;
      constants.dump_pitch_tiles = dump_pitch;
      constants.source_base_tiles = source.key.base_tiles;
      constants.source_pitch_tiles = source.key.GetPitchTiles();
      const uint32_t tile_size_x = (source.is_64bpp ? 40u : 80u) * scale_x;
      const uint32_t tile_size_y = 16u * scale_y;
      const uint32_t tile_pixel_size_x =
          tile_size_x >>
          uint32_t(source.msaa_samples >= xenos::MsaaSamples::k4X);
      const uint32_t tile_pixel_size_y =
          tile_size_y >>
          uint32_t(source.msaa_samples >= xenos::MsaaSamples::k2X);
      constants.thread_count_x =
          dispatch.width_tiles * tile_pixel_size_x / source.pixels_per_thread;
      constants.thread_count_y = dispatch.height_tiles * tile_pixel_size_y;
      constants.height_scaled = height_scaled;
      constants.msaa_2x_sample_0 = msaa_2x_sample_0;
      constants.msaa_2x_sample_1 = msaa_2x_sample_1;
      constants.flags = source.flags;

      VkBuffer constants_buffer;
      VkDeviceSize constants_offset;
      auto* constants_mapping = direct_host_resolve_constants_pool_->Request(
          current_submission, sizeof(constants), constants_alignment,
          constants_buffer, constants_offset);
      if (!constants_mapping) {
        return false;
      }
      std::memcpy(constants_mapping, &constants, sizeof(constants));

      VkDescriptorSet descriptor_set_constants =
          command_processor_.AllocateSingleTransientDescriptor(
              VulkanCommandProcessor::SingleTransientDescriptorLayout::
                  kUniformBufferComputeB1);
      if (descriptor_set_constants == VK_NULL_HANDLE) {
        return false;
      }
      VkDescriptorBufferInfo constants_buffer_info = {};
      constants_buffer_info.buffer = constants_buffer;
      constants_buffer_info.offset = constants_offset;
      constants_buffer_info.range = sizeof(constants);
      VkWriteDescriptorSet write_descriptor_set_constants = {};
      write_descriptor_set_constants.sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_descriptor_set_constants.dstSet = descriptor_set_constants;
      write_descriptor_set_constants.dstBinding = 1;
      write_descriptor_set_constants.descriptorCount = 1;
      write_descriptor_set_constants.descriptorType =
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_descriptor_set_constants.pBufferInfo = &constants_buffer_info;
      dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_constants, 0,
                                 nullptr);

      VkDescriptorSet descriptor_sets[] = {
          descriptor_set_constants,
          descriptor_set_dest,
          source_descriptor_set,
          linear_descriptor,
      };
      command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline_layout, 0, 4u,
                                             descriptor_sets, 0, nullptr);
      command_processor_.SubmitBarriers(true);
      NotePassBytesRead(command_buffer, source.render_target,
                        uint64_t(dispatch.width_tiles) * dispatch.height_tiles *
                            (xenos::kEdramTileWidthSamples >> uint32_t(source.key.Is64bpp())) *
                            xenos::kEdramTileHeightSamples * draw_resolution_scale_x() *
                            draw_resolution_scale_y() /
                            (1u << uint32_t(source.key.msaa_samples)),
                        true, uint64_t(constants.thread_count_x) * constants.thread_count_y *
                            source.pixels_per_thread *
                            (1u << draw_util::GetResolveDownscalePixelSizeLog2(resolve_info.copy_dest_info)) *
                            (texture_copy.size ? 2 : 1));
      command_buffer.CmdVkDispatch((constants.thread_count_x + 7u) >> 3,
                                   (constants.thread_count_y + 7u) >> 3, 1,
                                   DeferredCommandBuffer::DispatchSite::kResolveDirect);
    }
  }

  if (scaled_buffer_ready) {
    emit_scaled_dest_buffer_barrier(VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT);
  }

  texture_cache.CompleteResolveTextureCopy(
      texture_copy, texture_scratch.buffer(), resolve_info,
      IsDrawResolutionScaled(), resolve_texture_lock);
  if (texture_copy.size) {
    texture_scratch.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT);
    texture_scratch.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT);
  }
  written_address_out = resolve_info.copy_dest_extent_start;
  written_length_out = resolve_info.copy_dest_extent_length;
  return true;
}

bool VulkanRenderTargetCache::CaptureSourceRenderTarget(
    const draw_util::ResolveInfo& resolve_info, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch) {
  static bool capture_request_consumed = false;
  const std::filesystem::path& request_path =
      cvars::vulkan_source_rt_capture_request;
  if (capture_request_consumed || request_path.empty()) {
    return true;
  }

  static uint64_t last_marker_check_frame = UINT64_MAX;
  static bool marker_present_this_frame = false;
  const uint64_t frame = command_processor_.GetCurrentFrame();
  if (last_marker_check_frame != frame) {
    last_marker_check_frame = frame;
    std::error_code request_error;
    marker_present_this_frame =
        std::filesystem::exists(request_path, request_error);
  }
  if (!marker_present_this_frame) {
    return true;
  }

  const uint32_t resolve_width =
      resolve_info.coordinate_info.width_div_8 << 3;
  const uint32_t resolve_height = resolve_info.height_div_8 << 3;
  // MW2 diagnostic pre-capture: at the 1024x600 depth resolve (right after the
  // main scene pass) capture the color target owning the scene color range
  // (EDRAM base 1060, pitch 13, 38 tile rows) without consuming the request.
  static bool precapture_done = false;
  const bool pre_mode = cvars::vulkan_source_rt_precapture_at_depth_resolve &&
                        !precapture_done &&
                        GetPath() == Path::kHostRenderTargets &&
                        resolve_info.IsCopyingDepth() &&
                        resolve_width == 1024 && resolve_height == 600;
  if (pre_mode) {
    dump_base = 1060;
    dump_row_length_used = 13;
    dump_rows = 38;
    dump_pitch = 13;
  } else if (GetPath() != Path::kHostRenderTargets ||
             resolve_info.IsCopyingDepth() || resolve_width != 1024 ||
             resolve_height != 600 ||
             resolve_info.color_edram_info.format_is_64bpp ||
             resolve_info.color_edram_info.msaa_samples !=
                 xenos::MsaaSamples::k1X) {
    return true;
  }
  const char* const capture_tag =
      pre_mode ? "[SOURCE-RT-PRECAPTURE]" : "[SOURCE-RT-CAPTURE]";
  const char* const capture_prefix =
      pre_mode ? "source-rt-pre-frame" : "source-rt-frame";

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(dump_base, dump_pitch, dump_rectangles_);
  if (pre_mode) {
    XELOGI("{} owners={} frame={}", capture_tag, dump_rectangles_.size(),
           frame);
  }
  if (pre_mode ? dump_rectangles_.empty() : dump_rectangles_.size() != 1) {
    return true;
  }
  RenderTarget* capture_rt = dump_rectangles_.front().render_target;
  if (pre_mode && cvars::vulkan_source_rt_precapture_format >= 0) {
    // Capture a sibling host render target of the same EDRAM surface (same
    // base/pitch/msaa, different guest format) instead of the current owner -
    // its image still holds whatever was last drawn or transferred into it.
    RenderTargetKey sibling_key = capture_rt->key();
    sibling_key.resource_format =
        uint32_t(cvars::vulkan_source_rt_precapture_format);
    RenderTarget* sibling_rt = FindRenderTargetForTrace(sibling_key);
    XELOGI("{} sibling_format={} found={}", capture_tag,
           cvars::vulkan_source_rt_precapture_format,
           uint32_t(sibling_rt != nullptr));
    if (!sibling_rt) {
      return true;
    }
    capture_rt = sibling_rt;
  }
  auto& render_target = *static_cast<VulkanRenderTarget*>(capture_rt);
  class ScopedRenderTargetAliasLease {
   public:
    ScopedRenderTargetAliasLease(VulkanRenderTargetCache& cache,
                                 RenderTarget* render_target,
                                 bool tracking_enabled)
        : cache_(cache) {
      if (!tracking_enabled) {
        return;
      }
      if (cache_.AcquireExternalRenderTargetAliasLease(render_target)) {
        render_target_ = render_target;
      } else {
        acquired_ = false;
      }
    }
    ~ScopedRenderTargetAliasLease() {
      if (render_target_) {
        cache_.ReleaseExternalRenderTargetAliasLease(render_target_);
      }
    }
    bool acquired() const { return acquired_; }

   private:
    VulkanRenderTargetCache& cache_;
    RenderTarget* render_target_ = nullptr;
    bool acquired_ = true;
  } render_target_alias_lease(*this, &render_target,
                              retirement_tracking_enabled_);
  if (!render_target_alias_lease.acquired()) {
    return true;
  }
  const RenderTargetKey key = render_target.key();
  if (key.is_depth || key.Is64bpp() ||
      key.msaa_samples != xenos::MsaaSamples::k1X) {
    return true;
  }

  const VkFormat format = GetColorVulkanFormat(key.GetColorFormat());
  const uint32_t bytes_per_pixel =
      format == VK_FORMAT_R8G8B8A8_UNORM
          ? 4
          : (format == VK_FORMAT_R16G16B16A16_UNORM ? 8 : 0);
  if (!bytes_per_pixel) {
    return true;
  }

  // Consume only on the requested owner/resolve, so earlier unrelated resolves
  // cannot eat a marker armed for the 1024x600 scene surface. The pre-capture
  // never consumes the request (the main capture follows in the same frame).
  std::error_code remove_error;
  if (pre_mode) {
    precapture_done = true;
  } else {
    capture_request_consumed = true;
    std::filesystem::remove(request_path, remove_error);
  }

  auto log_error = [&](const char* phase) {
    XELOGE(
        "{} status=error frame={} phase={} request_path={} remove_error={}",
        capture_tag, frame, phase, request_path.string(),
        remove_error.value());
  };
  if (remove_error) {
    log_error("consume");
    return true;
  }
  if (!render_target.transfer_src_enabled()) {
    log_error("transfer-src-not-enabled-at-create");
    return true;
  }

  const uint32_t width = key.GetWidth() * GetKeyScaleX(key);
  const uint32_t height =
      GetRenderTargetHeight(key) *
      GetKeyScaleY(key);
  const VkDeviceSize buffer_size =
      VkDeviceSize(width) * VkDeviceSize(height) * bytes_per_pixel;
  if (!width || !height || buffer_size > SIZE_MAX) {
    log_error("invalid-extent");
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint32_t readback_memory_type = UINT32_MAX;
  VkDeviceSize readback_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer,
          readback_memory, &readback_memory_type, &readback_memory_size)) {
    log_error("allocate-readback");
    return true;
  }

  const VkPipelineStageFlags previous_stage =
      render_target.current_stage_mask();
  const VkAccessFlags previous_access = render_target.current_access_mask();
  const VkImageLayout previous_layout = render_target.current_layout();
  const VkImageSubresourceRange color_range =
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  command_processor_.PushImageMemoryBarrier(
      render_target.image(), color_range, previous_stage,
      VK_PIPELINE_STAGE_TRANSFER_BIT, previous_access,
      VK_ACCESS_TRANSFER_READ_BIT, previous_layout,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  render_target.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  command_processor_.SubmitBarriers(true);

  VkBufferImageCopy copy_region = {};
  copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_region.imageSubresource.layerCount = 1;
  copy_region.imageExtent = {width, height, 1};
  command_processor_.deferred_command_buffer().CmdVkCopyImageToBuffer(
      render_target.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      readback_buffer, 1, &copy_region);

  command_processor_.PushImageMemoryBarrier(
      render_target.image(), color_range, VK_PIPELINE_STAGE_TRANSFER_BIT,
      previous_stage, VK_ACCESS_TRANSFER_READ_BIT, previous_access,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, previous_layout);
  render_target.SetUsage(previous_stage, previous_access, previous_layout);
  command_processor_.PushBufferMemoryBarrier(
      readback_buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);

  if (!command_processor_.EndSubmission(false)) {
    log_error("submit");
    // The deferred command stream may still reference the readback buffer.
    // Keep it alive on this fatal path rather than destroy an in-use resource.
    return false;
  }
  if (!command_processor_.AwaitAllQueueOperationsCompletion()) {
    log_error("wait");
    // GPU completion was not established, so the readback allocation must stay
    // alive until device teardown.
    return false;
  }
  if (!command_processor_.BeginSubmission(false)) {
    log_error("reopen-submission");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return false;
  }

  void* mapped_data = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                      &mapped_data) != VK_SUCCESS) {
    log_error("map-readback");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }
  ui::vulkan::util::InvalidateMappedMemoryRange(
      vulkan_device, readback_memory, readback_memory_type, 0,
      readback_memory_size, buffer_size);

  std::filesystem::path raw_path = request_path.parent_path();
  raw_path /= fmt::format("{}-{}.bin", capture_prefix, frame);
  std::filesystem::path metadata_path = request_path.parent_path();
  metadata_path /= fmt::format("{}-{}.json", capture_prefix, frame);
  bool raw_written = false;
  if (FILE* raw_file = std::fopen(raw_path.string().c_str(), "wb")) {
    raw_written =
        std::fwrite(mapped_data, 1, size_t(buffer_size), raw_file) ==
            size_t(buffer_size) &&
        std::ferror(raw_file) == 0;
    raw_written = std::fclose(raw_file) == 0 && raw_written;
  }
  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  if (!raw_written) {
    log_error("write-raw");
    return true;
  }

  bool metadata_written = false;
  if (FILE* metadata_file =
          std::fopen(metadata_path.string().c_str(), "wb")) {
    const ResolveCopyDumpRectangle& source_rect = dump_rectangles_.front();
    metadata_written =
        std::fprintf(
            metadata_file,
            "{\n  \"frame\": %llu,\n  \"width\": %u,\n  \"height\": %u,\n"
            "  \"vk_format\": %u,\n  \"vk_format_name\": \"%s\",\n"
            "  \"bytes_per_pixel\": %u,\n  \"source_base_tiles\": %u,\n"
            "  \"source_pitch_tiles\": %u,\n  \"source_msaa\": 1,\n"
            "  \"resource_format\": %u,\n  \"resolve_width\": %u,\n"
            "  \"resolve_height\": %u,\n  \"resolve_dest_base\": %u,\n"
            "  \"resolve_extent_start\": %u,\n"
            "  \"resolve_extent_length\": %u,\n  \"edram_dump_base\": %u,\n"
            "  \"edram_dump_pitch\": %u,\n  \"edram_dump_rows\": %u,\n"
            "  \"edram_row_length\": %u,\n  \"owner_rect\": {\n"
            "    \"row_first\": %u,\n    \"rows\": %u,\n"
            "    \"row_first_start\": %u,\n    \"row_last_end\": %u\n"
            "  },\n  \"data_file\": \"%s\"\n}\n",
            static_cast<unsigned long long>(frame), width, height,
            uint32_t(format),
            format == VK_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8_UNORM"
                                               : "R16G16B16A16_UNORM",
            bytes_per_pixel, key.base_tiles, key.GetPitchTiles(),
            key.resource_format, resolve_width, resolve_height,
            resolve_info.copy_dest_base_unadjusted,
            resolve_info.copy_dest_extent_start,
            resolve_info.copy_dest_extent_length, dump_base, dump_pitch,
            dump_rows, dump_row_length_used, source_rect.row_first,
            source_rect.rows, source_rect.row_first_start,
            source_rect.row_last_end, raw_path.filename().string().c_str()) >= 0;
    metadata_written = std::fclose(metadata_file) == 0 && metadata_written;
  }
  if (!metadata_written) {
    log_error("write-metadata");
    return true;
  }

  XELOGI(
      "{} status=ok frame={} width={} height={} vk_format={} raw_path={} "
      "metadata_path={}",
      capture_tag, frame, width, height, uint32_t(format), raw_path.string(),
      metadata_path.string());
  return true;
}

std::string VulkanRenderTargetCache::FormatRtTraceKey(const void* rt,
                                                       bool msaa_and_pointer) {
  if (!rt) {
    return "-";
  }
  RenderTargetKey key = static_cast<const RenderTarget*>(rt)->key();
  // uint32_t(...) around the bit-field members: fmt::format's forwarding-
  // reference parameters can't bind directly to a bit-field lvalue.
  if (msaa_and_pointer) {
    return fmt::format("{}/{}/{}/{}@{:04X}", uint32_t(key.base_tiles),
                       key.GetPitchTiles(), uint32_t(key.resource_format),
                       uint32_t(key.msaa_samples),
                       uint32_t(reinterpret_cast<uintptr_t>(rt)) & 0xFFFFu);
  }
  return fmt::format("{}/{}/{}/{}", uint32_t(key.base_tiles),
                     key.GetPitchTiles(), uint32_t(key.resource_format),
                     uint32_t(key.is_depth));
}

bool VulkanRenderTargetCache::GetResolveInfoForDefer(
    const Memory& memory, draw_util::ResolveInfo& info) {
  return draw_util::GetResolveInfo(
      register_file(), memory, trace_writer_, draw_resolution_scale_x(),
      draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
      IsFixedRGBA16TruncatedToMinus1To1(), info);
}

bool VulkanRenderTargetCache::BeginDeferredResolve(
    const draw_util::ResolveInfo& info) {
  assert_true(deferred_resolve_sources_.empty());
  // Clears and transfers may still need their original ordering or pass.
  // In-pass resolves already avoid a pass break and must keep their path.
  if (cvars::vulkan_in_pass_resolve ||
      !cvars::vulkan_source_rt_capture_request.empty() ||
      pending_clear_render_target_ || deferred_clear_render_target_ ||
      draw_clear_chain_.mask || HasPendingDrawPassTransfers() ||
      HasPendingOutOfPassTransfers()) {
    return false;
  }
  uint32_t base, width, rows, pitch;
  info.GetCopyEdramTileSpan(base, width, rows, pitch);
  GetResolveCopyRectanglesToDump(base, width, rows, pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(base, pitch, dump_rectangles_);
  if (dump_rectangles_.empty()) return false;
  for (const auto& rectangle : dump_rectangles_) {
    const auto* target =
        static_cast<const VulkanRenderTarget*>(rectangle.render_target);
    // UNDEFINED/PREINITIALIZED are not legal restore destinations. Such an
    // owner has no defined saved contents to retain across the open pass.
    if (target->current_layout() == VK_IMAGE_LAYOUT_UNDEFINED ||
        target->current_layout() == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      return false;
    }
  }
  for (const auto& rectangle : dump_rectangles_) {
    auto* target = static_cast<VulkanRenderTarget*>(rectangle.render_target);
    deferred_resolve_sources_.push_back(
        {target, target->current_stage_mask(), target->current_access_mask(),
         target->current_layout()});
    // Commands capture the original owners, not a later ownership-map lookup.
    // Keep these images alive through this submission even if draws alias them.
    target->MarkGpuReference(command_processor_.GetCurrentSubmission(),
                             command_processor_.GetCurrentFrame());
  }
  deferred_resolve_edram_usage_ = edram_buffer_usage_;
  return true;
}

void VulkanRenderTargetCache::RestoreDeferredResolveSourceUsage() {
  for (const auto& source : deferred_resolve_sources_) {
    auto& target = *source.target;
    command_processor_.PushImageMemoryBarrier(
        target.image(), ui::vulkan::util::InitializeSubresourceRange(
            target.key().is_depth
                ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                : VK_IMAGE_ASPECT_COLOR_BIT),
        target.current_stage_mask(), source.stages,
        target.current_access_mask(), source.access,
        target.current_layout(), source.layout);
    target.SetUsage(source.stages, source.access, source.layout);
  }
  // The tail returns to the main stream's tracked layouts. Thus a barrier
  // already prepared by the caller of EndRenderPass still has the right old
  // layout, and unrelated draws can keep using the open framebuffer meanwhile.
  UseEdramBuffer(deferred_resolve_edram_usage_);
}

bool VulkanRenderTargetCache::EnsureDepthSurfacePipeline() {
  if (depth_surface_pipeline_ != VK_NULL_HANDLE) return true;
  if (depth_surface_pipeline_attempted_) return false;
  depth_surface_pipeline_attempted_ = true;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  const VkDevice device = vk->device();
  if (depth_surface_render_pass_ == VK_NULL_HANDLE) {
    // 0: decoded depth (sampled by the guest), 1: stencil (for the provider).
    VkAttachmentDescription attachments[2] = {};
    for (auto& attachment : attachments) {
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    attachments[0].format = VK_FORMAT_R32_SFLOAT;
    attachments[1].format = VK_FORMAT_R8_UINT;
    VkAttachmentReference references[2] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments = references;
    VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 2;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    if (dfn.vkCreateRenderPass(device, &pass, nullptr,
                              &depth_surface_render_pass_) != VK_SUCCESS)
      return false;
  }
  if (depth_surface_pipeline_layout_ == VK_NULL_HANDLE) {
    // Same depth/stencil sampled-image pair as the existing resolve. The
    // fragment shader reads depth only; stencil remains in guest memory/RT.
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12 * 4};
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptor_set_layout_sampled_image_x2_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (dfn.vkCreatePipelineLayout(device, &layout, nullptr,
                                   &depth_surface_pipeline_layout_) != VK_SUCCESS)
      return false;
  }
  // Exact native-1x counterpart of resolve_host_depth.xesli followed by
  // texture_load_depth_float: texelFetch -> 20e4 -> float32, BEFORE filtering.
  SpirvBuilder b(spirv_version_,
      (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  const auto glsl = b.import("GLSL.std.450");
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.setSource(spv::SourceLanguageUnknown, 0);
  const auto u = b.makeUintType(32), f = b.makeFloatType(32);
  const auto f4 = b.makeVectorType(f, 4), u2 = b.makeVectorType(u, 2);
  const auto boolean = b.makeBoolType();
  auto image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(f, spv::Dim2D, false, false, false, 1,
                       spv::ImageFormatUnknown), "depth_source");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  const auto u4 = b.makeVectorType(u, 4);
  auto stencil_image = b.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(u, spv::Dim2D, false, false, false, 1,
                      spv::ImageFormatUnknown),
      "stencil_source");
  b.addDecoration(stencil_image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(stencil_image, spv::DecorationBinding, 1);
  std::vector<spv::Id> members(12, u);
  auto pc_type = b.makeStructType(members, "DepthSurfaceConstants");
  b.addDecoration(pc_type, spv::DecorationBlock);
  for (uint32_t i = 0; i < 12; ++i)
    b.addMemberDecoration(pc_type, i, spv::DecorationOffset, i * 4);
  auto pc = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                             pc_type, "constants");
  auto frag = b.createVariable(spv::NoPrecision, spv::StorageClassInput, f4,
                               "gl_FragCoord");
  b.addDecoration(frag, spv::DecorationBuiltIn, int(spv::BuiltIn::FragCoord));
  auto out = b.createVariable(spv::NoPrecision, spv::StorageClassOutput, f,
                              "guest_depth");
  b.addDecoration(out, spv::DecorationLocation, 0);
  auto stencil_out = b.createVariable(spv::NoPrecision,
                                      spv::StorageClassOutput, u,
                                      "guest_stencil");
  b.addDecoration(stencil_out, spv::DecorationLocation, 1);
  std::vector<spv::Id> params;
  std::vector<std::vector<spv::Decoration>> precision;
  spv::Block* entry;
  auto main = b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "main",
                                  params, precision, &entry);
  auto constant = [&](uint32_t index) {
    std::vector<spv::Id> indices = {b.makeIntConstant(int32_t(index))};
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, pc,
                                            indices), spv::NoPrecision);
  };
  auto add = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpIAdd, u, a, c); };
  auto mul = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpIMul, u, a, c); };
  auto div = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpUDiv, u, a, c); };
  auto mod = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpUMod, u, a, c); };
  auto coord = b.createLoad(frag, spv::NoPrecision);
  auto x = add(b.createUnaryOp(spv::OpConvertFToU, u,
      b.createCompositeExtract(coord, f, 0)), constant(4));
  auto y = add(b.createUnaryOp(spv::OpConvertFToU, u,
      b.createCompositeExtract(coord, f, 1)), constant(5));
  auto tx = div(x, b.makeUintConstant(80));
  auto ty = div(y, b.makeUintConstant(16));
  // Disjoint ownership rectangles in resolve-tile coordinates. Padding is
  // never published: preflight proves exactly one owner for every tile.
  auto row_last = b.createBinOp(spv::OpISub, u,
      add(constant(6), constant(7)), b.makeUintConstant(1));
  auto first_x = b.createTriOp(spv::OpSelect, u,
      b.createBinOp(spv::OpIEqual, boolean, ty, constant(6)), constant(8),
      b.makeUintConstant(0));
  auto last_x = b.createTriOp(spv::OpSelect, u,
      b.createBinOp(spv::OpIEqual, boolean, ty, row_last), constant(9), constant(10));
  auto valid = b.createBinOp(spv::OpLogicalAnd, boolean,
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, ty, constant(6)),
      b.createBinOp(spv::OpULessThanEqual, boolean, ty, row_last));
  valid = b.createBinOp(spv::OpLogicalAnd, boolean, valid,
      b.createBinOp(spv::OpLogicalAnd, boolean,
          b.createBinOp(spv::OpUGreaterThanEqual, boolean, tx, first_x),
          b.createBinOp(spv::OpULessThan, boolean, tx, last_x)));
  {
    SpirvBuilder::IfBuilder kill(b.createUnaryOp(spv::OpLogicalNot, boolean, valid),
                                 spv::SelectionControlMaskNone, b);
    b.createNoResultOp(spv::OpKill);
    kill.makeEndIf(false);
  }
  auto local_tile = b.createBinOp(spv::OpISub, u,
      add(constant(0), add(mul(ty, constant(1)), tx)), constant(2));
  auto sx = add(mul(mod(local_tile, constant(3)), b.makeUintConstant(80)),
                 mod(x, b.makeUintConstant(80)));
  auto sy = add(mul(div(local_tile, constant(3)), b.makeUintConstant(16)),
                 mod(y, b.makeUintConstant(16)));
  spv::Builder::TextureParameters sample = {};
  sample.sampler = b.createLoad(image, spv::NoPrecision);
  sample.coords = b.createCompositeConstruct(u2, std::vector<spv::Id>{sx, sy});
  sample.lod = b.makeIntConstant(0);
  auto depth = b.createCompositeExtract(
      b.createTextureCall(spv::NoPrecision, f4, false, true, false, false,
                          false, sample, spv::ImageOperandsMaskNone), f, 0);
  auto packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
      b, depth, !depth_float24_convert_in_pixel_shader_ && depth_float24_round_,
      true, glsl);
  auto decoded = SpirvShaderTranslator::Depth20e4To32(
      b, packed, 0, false, false, glsl);
  b.createStore(decoded, out);
  spv::Builder::TextureParameters stencil_sample = {};
  stencil_sample.sampler = b.createLoad(stencil_image, spv::NoPrecision);
  stencil_sample.coords = sample.coords;
  stencil_sample.lod = b.makeIntConstant(0);
  b.createStore(
      b.createCompositeExtract(
          b.createTextureCall(spv::NoPrecision, u4, false, true, false, false,
                              false, stencil_sample,
                              spv::ImageOperandsMaskNone),
          u, 0),
      stencil_out);
  b.leaveFunction();
  b.addExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
  auto ep = b.addEntryPoint(spv::ExecutionModelFragment, main, "main");
  ep->addIdOperand(frag);
  ep->addIdOperand(out);
  ep->addIdOperand(stencil_out);
  if (spirv_version_ >= spv::Spv_1_4) {
    ep->addIdOperand(image);
    ep->addIdOperand(stencil_image);
    ep->addIdOperand(pc);
  }
  std::vector<unsigned int> code;
  b.dump(code);
  VkShaderModule fragment = ui::vulkan::util::CreateShaderModule(
      vk, code.data(), code.size() * sizeof(uint32_t));
  if (fragment == VK_NULL_HANDLE) return false;
  VkPipelineShaderStageCreateInfo shaders[2] = {};
  for (auto& s : shaders) {
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.pName = "main";
  }
  shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shaders[0].module = transfer_passthrough_vertex_shader_;
  shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shaders[1].module = fragment;
  VkVertexInputBindingDescription binding = {0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1;
  vertex.pVertexBindingDescriptions = &binding;
  vertex.pVertexAttributeDescriptions = &attribute;
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo samples = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend_attachments[2] = {};
  for (auto& blend_attachment : blend_attachments) {
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  }
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 2;
  blend.pAttachments = blend_attachments;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline.stageCount = 2;
  pipeline.pStages = shaders;
  pipeline.pVertexInputState = &vertex;
  pipeline.pInputAssemblyState = &assembly;
  pipeline.pViewportState = &viewport;
  pipeline.pRasterizationState = &raster;
  pipeline.pMultisampleState = &samples;
  pipeline.pDepthStencilState = &depth_stencil;
  pipeline.pColorBlendState = &blend;
  pipeline.pDynamicState = &dynamic;
  pipeline.layout = depth_surface_pipeline_layout_;
  pipeline.renderPass = depth_surface_render_pass_;
  pipeline.basePipelineIndex = -1;
  const auto result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
      &pipeline, nullptr, &depth_surface_pipeline_);
  dfn.vkDestroyShaderModule(device, fragment, nullptr);
  return result == VK_SUCCESS;
}

bool VulkanRenderTargetCache::EnsureColorSurfacePipeline() {
  if (color_surface_pipeline_ != VK_NULL_HANDLE) return true;
  if (color_surface_pipeline_attempted_) return false;
  color_surface_pipeline_attempted_ = true;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  const VkDevice device = vk->device();
  if (color_surface_render_pass_ == VK_NULL_HANDLE) {
    // The k_8_8_8_8 texture the guest samples, written whole.
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference = {0,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    if (dfn.vkCreateRenderPass(device, &pass, nullptr,
                              &color_surface_render_pass_) != VK_SUCCESS)
      return false;
  }
  if (color_surface_pipeline_layout_ == VK_NULL_HANDLE) {
    // The color render target's own transfer source descriptor.
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12 * 4};
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptor_set_layout_sampled_image_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (dfn.vkCreatePipelineLayout(device, &layout, nullptr,
                                   &color_surface_pipeline_layout_) != VK_SUCCESS)
      return false;
  }
  // Raw 8_8_8_8 resolve of a 1x 8_8_8_8 render target into a texture with
  // the same endianness: the texels are the render target's (red and blue
  // exchanged with copy_dest_swap), unorm8 -> float -> unorm8 is exact.
  SpirvBuilder b(spirv_version_,
      (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  const auto glsl = b.import("GLSL.std.450");
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.setSource(spv::SourceLanguageUnknown, 0);
  const auto u = b.makeUintType(32), f = b.makeFloatType(32);
  const auto f4 = b.makeVectorType(f, 4), u2 = b.makeVectorType(u, 2);
  const auto boolean = b.makeBoolType();
  auto image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(f, spv::Dim2D, false, false, false, 1,
                       spv::ImageFormatUnknown), "color_source");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  const auto u4 = b.makeVectorType(u, 4);
  std::vector<spv::Id> members(12, u);
  auto pc_type = b.makeStructType(members, "DepthSurfaceConstants");
  b.addDecoration(pc_type, spv::DecorationBlock);
  for (uint32_t i = 0; i < 12; ++i)
    b.addMemberDecoration(pc_type, i, spv::DecorationOffset, i * 4);
  auto pc = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                             pc_type, "constants");
  auto frag = b.createVariable(spv::NoPrecision, spv::StorageClassInput, f4,
                               "gl_FragCoord");
  b.addDecoration(frag, spv::DecorationBuiltIn, int(spv::BuiltIn::FragCoord));
  auto out = b.createVariable(spv::NoPrecision, spv::StorageClassOutput, f4,
                              "guest_color");
  b.addDecoration(out, spv::DecorationLocation, 0);
  std::vector<spv::Id> params;
  std::vector<std::vector<spv::Decoration>> precision;
  spv::Block* entry;
  auto main = b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "main",
                                  params, precision, &entry);
  auto constant = [&](uint32_t index) {
    std::vector<spv::Id> indices = {b.makeIntConstant(int32_t(index))};
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, pc,
                                            indices), spv::NoPrecision);
  };
  auto add = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpIAdd, u, a, c); };
  auto mul = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpIMul, u, a, c); };
  auto div = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpUDiv, u, a, c); };
  auto mod = [&](spv::Id a, spv::Id c) { return b.createBinOp(spv::OpUMod, u, a, c); };
  auto coord = b.createLoad(frag, spv::NoPrecision);
  auto x = add(b.createUnaryOp(spv::OpConvertFToU, u,
      b.createCompositeExtract(coord, f, 0)), constant(4));
  auto y = add(b.createUnaryOp(spv::OpConvertFToU, u,
      b.createCompositeExtract(coord, f, 1)), constant(5));
  auto tx = div(x, b.makeUintConstant(80));
  auto ty = div(y, b.makeUintConstant(16));
  // Disjoint ownership rectangles in resolve-tile coordinates. Padding is
  // never published: preflight proves exactly one owner for every tile.
  auto row_last = b.createBinOp(spv::OpISub, u,
      add(constant(6), constant(7)), b.makeUintConstant(1));
  auto first_x = b.createTriOp(spv::OpSelect, u,
      b.createBinOp(spv::OpIEqual, boolean, ty, constant(6)), constant(8),
      b.makeUintConstant(0));
  auto last_x = b.createTriOp(spv::OpSelect, u,
      b.createBinOp(spv::OpIEqual, boolean, ty, row_last), constant(9), constant(10));
  auto valid = b.createBinOp(spv::OpLogicalAnd, boolean,
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, ty, constant(6)),
      b.createBinOp(spv::OpULessThanEqual, boolean, ty, row_last));
  valid = b.createBinOp(spv::OpLogicalAnd, boolean, valid,
      b.createBinOp(spv::OpLogicalAnd, boolean,
          b.createBinOp(spv::OpUGreaterThanEqual, boolean, tx, first_x),
          b.createBinOp(spv::OpULessThan, boolean, tx, last_x)));
  {
    SpirvBuilder::IfBuilder kill(b.createUnaryOp(spv::OpLogicalNot, boolean, valid),
                                 spv::SelectionControlMaskNone, b);
    b.createNoResultOp(spv::OpKill);
    kill.makeEndIf(false);
  }
  auto local_tile = b.createBinOp(spv::OpISub, u,
      add(constant(0), add(mul(ty, constant(1)), tx)), constant(2));
  auto sx = add(mul(mod(local_tile, constant(3)), b.makeUintConstant(80)),
                 mod(x, b.makeUintConstant(80)));
  auto sy = add(mul(div(local_tile, constant(3)), b.makeUintConstant(16)),
                 mod(y, b.makeUintConstant(16)));
  spv::Builder::TextureParameters sample = {};
  sample.sampler = b.createLoad(image, spv::NoPrecision);
  sample.coords = b.createCompositeConstruct(u2, std::vector<spv::Id>{sx, sy});
  sample.lod = b.makeIntConstant(0);
  auto texel = b.createTextureCall(spv::NoPrecision, f4, false, true, false,
                                   false, false, sample,
                                   spv::ImageOperandsMaskNone);
  auto swapped = b.createRvalueSwizzle(spv::NoPrecision, f4, texel,
                                       std::vector<unsigned>{2, 1, 0, 3});
  auto swap = b.createBinOp(spv::OpINotEqual, boolean, constant(11),
                            b.makeUintConstant(0));
  b.createStore(b.createTriOp(spv::OpSelect, f4,
                              b.smearScalar(spv::NoPrecision, swap,
                                            b.makeVectorType(boolean, 4)),
                              swapped, texel),
                out);
  b.leaveFunction();
  b.addExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
  auto ep = b.addEntryPoint(spv::ExecutionModelFragment, main, "main");
  ep->addIdOperand(frag);
  ep->addIdOperand(out);
  if (spirv_version_ >= spv::Spv_1_4) {
    ep->addIdOperand(image);
    ep->addIdOperand(pc);
  }
  std::vector<unsigned int> code;
  b.dump(code);
  VkShaderModule fragment = ui::vulkan::util::CreateShaderModule(
      vk, code.data(), code.size() * sizeof(uint32_t));
  if (fragment == VK_NULL_HANDLE) return false;
  VkPipelineShaderStageCreateInfo shaders[2] = {};
  for (auto& s : shaders) {
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.pName = "main";
  }
  shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shaders[0].module = transfer_passthrough_vertex_shader_;
  shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shaders[1].module = fragment;
  VkVertexInputBindingDescription binding = {0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attribute = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex.vertexBindingDescriptionCount = vertex.vertexAttributeDescriptionCount = 1;
  vertex.pVertexBindingDescriptions = &binding;
  vertex.pVertexAttributeDescriptions = &attribute;
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo samples = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline.stageCount = 2;
  pipeline.pStages = shaders;
  pipeline.pVertexInputState = &vertex;
  pipeline.pInputAssemblyState = &assembly;
  pipeline.pViewportState = &viewport;
  pipeline.pRasterizationState = &raster;
  pipeline.pMultisampleState = &samples;
  pipeline.pDepthStencilState = &depth_stencil;
  pipeline.pColorBlendState = &blend;
  pipeline.pDynamicState = &dynamic;
  pipeline.layout = color_surface_pipeline_layout_;
  pipeline.renderPass = color_surface_render_pass_;
  pipeline.basePipelineIndex = -1;
  const auto result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
      &pipeline, nullptr, &color_surface_pipeline_);
  dfn.vkDestroyShaderModule(device, fragment, nullptr);
  return result == VK_SUCCESS;
}

bool VulkanRenderTargetCache::DrawDepthSurfaceResolve(
    const draw_util::ResolveInfo& info, VulkanTextureCache& texture_cache,
    const global_unique_lock_type& lock,
    std::vector<VulkanTextureCache::DepthSurfaceTarget>& targets) {
  assert_true(lock.owns_lock());
  auto& stats = depth_surface_stats_;
  if (GetPath() != Path::kHostRenderTargets || !info.IsCopyingDepth() ||
      info.depth_edram_info.format != uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) ||
      uint32_t(info.copy_dest_info.copy_dest_format) != uint32_t(xenos::TextureFormat::k_24_8_FLOAT) ||
      IsDrawResolutionScaled() || info.copy_dest_info.copy_dest_array ||
      uint32_t(info.copy_dest_info.copy_dest_endian) >= 4 ||
      info.copy_dest_x0 || info.copy_dest_y0 ||
      info.copy_dest_info.copy_dest_exp_bias ||
      info.rb_copy_control.copy_command != xenos::CopyCommand::kRaw) {
    ++stats.fallback_schema;
    static uint32_t schema_logged = 0;
    if (info.IsCopyingDepth() && schema_logged < 32) {
      ++schema_logged;
      XELOGI(
          "VkRTTexture: depth resolve not drawn: depth_format={} dest_format={} "
          "scaled={} array={} endian={} x0={} y0={} exp_bias={} command={}",
          uint32_t(info.depth_edram_info.format),
          uint32_t(info.copy_dest_info.copy_dest_format),
          IsDrawResolutionScaled(), uint32_t(info.copy_dest_info.copy_dest_array),
          uint32_t(info.copy_dest_info.copy_dest_endian), info.copy_dest_x0,
          info.copy_dest_y0, uint32_t(info.copy_dest_info.copy_dest_exp_bias),
          uint32_t(info.rb_copy_control.copy_command));
    }
    return false;
  }
  if (info.depth_edram_info.msaa_samples != xenos::MsaaSamples::k1X ||
      !xenos::IsSingleCopySampleSelected(info.copy_dest_coordinate_info.copy_sample_select)) {
    ++stats.fallback_msaa;
    return false;
  }
  uint32_t base, width_tiles, rows, pitch;
  info.GetCopyEdramTileSpan(base, width_tiles, rows, pitch);
  GetResolveCopyRectanglesToDump(base, width_tiles, rows, pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(base, pitch, dump_rectangles_);
  if (!width_tiles || !rows || !pitch || dump_rectangles_.empty() ||
      width_tiles > pitch || base + uint64_t(rows - 1) * pitch + width_tiles > xenos::kEdramTileCount) {
    ++stats.fallback_owner;
    return false;
  }
  std::vector<uint8_t> covered(size_t(width_tiles) * rows, 0);
  for (const auto& rect : dump_rectangles_) {
    const auto* rt = static_cast<const VulkanRenderTarget*>(rect.render_target);
    if (!rt || !rect.rows || rect.row_first >= rows || rect.rows > rows - rect.row_first) {
      ++stats.fallback_owner;
      return false;
    }
    const auto key = rt->key();
    if (!key.is_depth || key.GetDepthFormat() != xenos::DepthRenderTargetFormat::kD24FS8 ||
        key.msaa_samples != xenos::MsaaSamples::k1X || GetKeyScaleX(key) != 1 ||
        GetKeyScaleY(key) != 1 || !key.GetPitchTiles() ||
        rt->current_layout() == VK_IMAGE_LAYOUT_UNDEFINED ||
        rt->current_layout() == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      ++stats.fallback_owner;
      return false;
    }
    for (uint32_t y = rect.row_first; y < rect.row_first + rect.rows; ++y) {
      const uint32_t x0 = y == rect.row_first ? rect.row_first_start : 0;
      const uint32_t x1 = y + 1 == rect.row_first + rect.rows ? rect.row_last_end : width_tiles;
      if (x0 >= x1 || x1 > width_tiles) {
        ++stats.fallback_owner;
        return false;
      }
      for (uint32_t x = x0; x < x1; ++x) {
        const uint32_t tile = base + y * pitch + x;
        if (tile < key.base_tiles || covered[size_t(y) * width_tiles + x]++ ||
            ((tile - key.base_tiles) / key.GetPitchTiles() + 1) * 16 > GetRenderTargetHeight(key)) {
          ++stats.fallback_owner;
          return false;
        }
      }
    }
  }
  if (std::find(covered.begin(), covered.end(), uint8_t(0)) != covered.end()) {
    ++stats.fallback_owner;
    return false;
  }
  if (!EnsureDepthSurfacePipeline()) {
    ++stats.fallback_resource;
    return false;
  }
  targets = texture_cache.PrepareDepthSurfaceTargets(info, depth_surface_render_pass_, lock);
  if (targets.empty()) {
    ++stats.fallback_texture;
    return false;
  }
  // One immutable full-extent source avoids duplicating snapshots/traffic.
  targets.resize(1);
  VkBuffer vertex_buffer;
  VkDeviceSize vertex_offset;
  auto vertices = reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
      command_processor_.GetCurrentSubmission(), 6 * sizeof(float), sizeof(float),
      vertex_buffer, vertex_offset));
  if (!vertices) {
    ++stats.fallback_resource;
    return false;
  }
  constexpr float triangle[] = {-1, -1, -1, 3, 3, -1};
  std::memcpy(vertices, triangle, sizeof(triangle));
  command_processor_.EndRenderPass("depth_surface");
  auto& commands = command_processor_.deferred_command_buffer();
  // Source depth/stencil move together. Sampling does not modify either plane.
  for (const auto& rect : dump_rectangles_) {
    auto& rt = *static_cast<VulkanRenderTarget*>(rect.render_target);
    command_processor_.PushImageMemoryBarrier(
        rt.image(), ui::vulkan::util::InitializeSubresourceRange(
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
        rt.current_stage_mask(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rt.SetUsage(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rt.MarkGpuReference(command_processor_.GetCurrentSubmission(),
                         command_processor_.GetCurrentFrame());
  }
  command_processor_.SubmitBarriers(true);
  for (const auto& target : targets) {
    texture_cache.BeginDepthSurfaceTarget(target);
    command_processor_.SubmitBarriers(true);
    VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = depth_surface_render_pass_;
    begin.framebuffer = target.framebuffer;
    begin.renderArea.extent = {target.width, target.height};
    commands.CmdVkBeginRenderPass(&begin, VK_SUBPASS_CONTENTS_INLINE);
    command_processor_.BindExternalGraphicsPipeline(depth_surface_pipeline_);
    VkViewport viewport = {0, 0, float(target.width), float(target.height), 0, 1};
    command_processor_.SetViewport(viewport);
    VkRect2D scissor = {{0, 0}, {target.width, target.height}};
    command_processor_.SetScissor(scissor);
    commands.CmdVkBindVertexBuffers(0, 1, &vertex_buffer, &vertex_offset);
    for (const auto& rect : dump_rectangles_) {
      auto& rt = *static_cast<VulkanRenderTarget*>(rect.render_target);
      const auto descriptor = rt.GetDescriptorSetTransferSource();
      commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
          depth_surface_pipeline_layout_, 0, 1, &descriptor, 0, nullptr);
      uint32_t constants[] = {base, pitch, uint32_t(rt.key().base_tiles),
          rt.key().GetPitchTiles(), uint32_t(info.coordinate_info.edram_offset_x_div_8) << 3,
          uint32_t(info.coordinate_info.edram_offset_y_div_8) << 3, rect.row_first, rect.rows,
          rect.row_first_start, rect.row_last_end, width_tiles, 0};
      commands.CmdVkPushConstants(depth_surface_pipeline_layout_,
          VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
      commands.CmdVkDraw(3, 1, 0, 0);
    }
    commands.CmdVkEndRenderPass();
    texture_cache.CompleteDepthSurfaceTarget(target);
    command_processor_.SubmitBarriers(true);
    ++stats.snapshots;
    // Logical payload only; AFBC and physical bus transactions are not inferred.
    stats.surface_read_bytes += uint64_t(target.width) * target.height * 4;
    stats.surface_write_bytes += uint64_t(target.width) * target.height * 4;
  }
  return true;
}

bool VulkanRenderTargetCache::DrawColorSurfaceResolve(
    const draw_util::ResolveInfo& info, VulkanTextureCache& texture_cache,
    const global_unique_lock_type& lock,
    std::vector<VulkanTextureCache::DepthSurfaceTarget>& targets) {
  assert_true(lock.owns_lock());
  auto& stats = depth_surface_stats_;
  if (GetPath() != Path::kHostRenderTargets || info.IsCopyingDepth() ||
      info.color_edram_info.format !=
          uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) ||
      uint32_t(info.copy_dest_info.copy_dest_format) !=
          uint32_t(xenos::TextureFormat::k_8_8_8_8) ||
      IsDrawResolutionScaled() || info.copy_dest_info.copy_dest_array ||
      uint32_t(info.copy_dest_info.copy_dest_endian) >= 4 ||
      info.copy_dest_x0 || info.copy_dest_y0 ||
      info.copy_dest_info.copy_dest_exp_bias ||
      (info.rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
       info.rb_copy_control.copy_command != xenos::CopyCommand::kConvert)) {
    ++stats.fallback_schema;
    static uint32_t color_schema_logged = 0;
    if (color_schema_logged < 32) {
      ++color_schema_logged;
      XELOGI(
          "VkRTTexture: color resolve not drawn: color_format={} "
          "dest_format={} {}x{} scaled={} array={} endian={} x0={} y0={} "
          "exp_bias={} command={} msaa={}",
          uint32_t(info.color_edram_info.format),
          uint32_t(info.copy_dest_info.copy_dest_format),
          info.coordinate_info.width_div_8 << 3, info.height_div_8 << 3,
          IsDrawResolutionScaled(), uint32_t(info.copy_dest_info.copy_dest_array),
          uint32_t(info.copy_dest_info.copy_dest_endian), info.copy_dest_x0,
          info.copy_dest_y0, uint32_t(info.copy_dest_info.copy_dest_exp_bias),
          uint32_t(info.rb_copy_control.copy_command),
          uint32_t(info.color_edram_info.msaa_samples));
    }
    return false;
  }
  if (info.color_edram_info.msaa_samples != xenos::MsaaSamples::k1X ||
      !xenos::IsSingleCopySampleSelected(info.copy_dest_coordinate_info.copy_sample_select)) {
    ++stats.fallback_msaa;
    return false;
  }
  uint32_t base, width_tiles, rows, pitch;
  info.GetCopyEdramTileSpan(base, width_tiles, rows, pitch);
  GetResolveCopyRectanglesToDump(base, width_tiles, rows, pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(base, pitch, dump_rectangles_);
  if (!width_tiles || !rows || !pitch || dump_rectangles_.empty() ||
      width_tiles > pitch || base + uint64_t(rows - 1) * pitch + width_tiles > xenos::kEdramTileCount) {
    ++stats.fallback_owner;
    return false;
  }
  std::vector<uint8_t> covered(size_t(width_tiles) * rows, 0);
  for (const auto& rect : dump_rectangles_) {
    const auto* rt = static_cast<const VulkanRenderTarget*>(rect.render_target);
    if (!rt || !rect.rows || rect.row_first >= rows || rect.rows > rows - rect.row_first) {
      ++stats.fallback_owner;
      return false;
    }
    const auto key = rt->key();
    if (key.is_depth ||
        key.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        key.msaa_samples != xenos::MsaaSamples::k1X || GetKeyScaleX(key) != 1 ||
        GetKeyScaleY(key) != 1 || !key.GetPitchTiles() ||
        rt->current_layout() == VK_IMAGE_LAYOUT_UNDEFINED ||
        rt->current_layout() == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      ++stats.fallback_owner;
      return false;
    }
    for (uint32_t y = rect.row_first; y < rect.row_first + rect.rows; ++y) {
      const uint32_t x0 = y == rect.row_first ? rect.row_first_start : 0;
      const uint32_t x1 = y + 1 == rect.row_first + rect.rows ? rect.row_last_end : width_tiles;
      if (x0 >= x1 || x1 > width_tiles) {
        ++stats.fallback_owner;
        return false;
      }
      for (uint32_t x = x0; x < x1; ++x) {
        const uint32_t tile = base + y * pitch + x;
        if (tile < key.base_tiles || covered[size_t(y) * width_tiles + x]++ ||
            ((tile - key.base_tiles) / key.GetPitchTiles() + 1) * 16 > GetRenderTargetHeight(key)) {
          ++stats.fallback_owner;
          return false;
        }
      }
    }
  }
  if (std::find(covered.begin(), covered.end(), uint8_t(0)) != covered.end()) {
    ++stats.fallback_owner;
    return false;
  }
  if (!EnsureColorSurfacePipeline()) {
    ++stats.fallback_resource;
    return false;
  }
  targets = texture_cache.PrepareDepthSurfaceTargets(info, color_surface_render_pass_, lock);
  if (targets.empty()) {
    ++stats.fallback_texture;
    return false;
  }
  // One immutable full-extent source avoids duplicating snapshots/traffic.
  targets.resize(1);
  VkBuffer vertex_buffer;
  VkDeviceSize vertex_offset;
  auto vertices = reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
      command_processor_.GetCurrentSubmission(), 6 * sizeof(float), sizeof(float),
      vertex_buffer, vertex_offset));
  if (!vertices) {
    ++stats.fallback_resource;
    return false;
  }
  constexpr float triangle[] = {-1, -1, -1, 3, 3, -1};
  std::memcpy(vertices, triangle, sizeof(triangle));
  command_processor_.EndRenderPass("color_surface");
  auto& commands = command_processor_.deferred_command_buffer();
  // The source is only sampled.
  for (const auto& rect : dump_rectangles_) {
    auto& rt = *static_cast<VulkanRenderTarget*>(rect.render_target);
    command_processor_.PushImageMemoryBarrier(
        rt.image(), ui::vulkan::util::InitializeSubresourceRange(),
        rt.current_stage_mask(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT, rt.current_layout(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rt.SetUsage(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rt.MarkGpuReference(command_processor_.GetCurrentSubmission(),
                         command_processor_.GetCurrentFrame());
  }
  command_processor_.SubmitBarriers(true);
  for (const auto& target : targets) {
    texture_cache.BeginDepthSurfaceTarget(target);
    command_processor_.SubmitBarriers(true);
    VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = color_surface_render_pass_;
    begin.framebuffer = target.framebuffer;
    begin.renderArea.extent = {target.width, target.height};
    commands.CmdVkBeginRenderPass(&begin, VK_SUBPASS_CONTENTS_INLINE);
    command_processor_.BindExternalGraphicsPipeline(color_surface_pipeline_);
    VkViewport viewport = {0, 0, float(target.width), float(target.height), 0, 1};
    command_processor_.SetViewport(viewport);
    VkRect2D scissor = {{0, 0}, {target.width, target.height}};
    command_processor_.SetScissor(scissor);
    commands.CmdVkBindVertexBuffers(0, 1, &vertex_buffer, &vertex_offset);
    for (const auto& rect : dump_rectangles_) {
      auto& rt = *static_cast<VulkanRenderTarget*>(rect.render_target);
      const auto descriptor = rt.GetDescriptorSetTransferSource();
      commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
          color_surface_pipeline_layout_, 0, 1, &descriptor, 0, nullptr);
      uint32_t constants[] = {base, pitch, uint32_t(rt.key().base_tiles),
          rt.key().GetPitchTiles(), uint32_t(info.coordinate_info.edram_offset_x_div_8) << 3,
          uint32_t(info.coordinate_info.edram_offset_y_div_8) << 3, rect.row_first, rect.rows,
          rect.row_first_start, rect.row_last_end, width_tiles,
          uint32_t(info.copy_dest_info.copy_dest_swap)};
      commands.CmdVkPushConstants(color_surface_pipeline_layout_,
          VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
      commands.CmdVkDraw(3, 1, 0, 0);
    }
    commands.CmdVkEndRenderPass();
    texture_cache.CompleteDepthSurfaceTarget(target);
    command_processor_.SubmitBarriers(true);
    ++stats.snapshots;
    // Logical payload only; AFBC and physical bus transactions are not inferred.
    stats.surface_read_bytes += uint64_t(target.width) * target.height * 4;
    stats.surface_write_bytes += uint64_t(target.width) * target.height * 4;
  }
  return true;
}

bool VulkanRenderTargetCache::EnsureStencilCopyPipeline() {
  if (stencil_copy_pipeline_ != VK_NULL_HANDLE) return true;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  if (!stencil_copy_pipeline_layout_) {
    VkDescriptorSetLayout sets[] = {descriptor_set_layout_sampled_image_,
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute)};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 5 * 4};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2; ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
    if (dfn.vkCreatePipelineLayout(vk->device(), &ci, nullptr,
        &stencil_copy_pipeline_layout_) != VK_SUCCESS) return false;
  }
  // k_8_8_8_8 color texels -> the stencil byte of the depth / stencil word at
  // the same EDRAM sample (the low byte, red - with the 40-sample halves of the
  // tile swapped between color and depth), four pixels per output word.
  // Constants: width in words, height, rectangle x, y, first output word.
  SpirvBuilder b(spirv_version_,
      (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto u = b.makeUintType(32), f = b.makeFloatType(32);
  const auto u2 = b.makeVectorType(u, 2), u3 = b.makeVectorType(u, 3);
  const auto f4 = b.makeVectorType(f, 4), boolean = b.makeBoolType();
  auto image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(f, spv::Dim2D, false, false, false, 1,
                      spv::ImageFormatUnknown), "color_source");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  auto array = b.makeRuntimeArray(u);
  b.addDecoration(array, spv::DecorationArrayStride, 4);
  auto buffer_type = b.makeStructType(std::vector<spv::Id>{array}, "Destination");
  const auto storage = spirv_version_ >= spv::Spv_1_3
      ? spv::StorageClassStorageBuffer : spv::StorageClassUniform;
  b.addDecoration(buffer_type, spirv_version_ >= spv::Spv_1_3
      ? spv::DecorationBlock : spv::DecorationBufferBlock);
  b.addMemberDecoration(buffer_type, 0, spv::DecorationOffset, 0);
  auto dest = b.createVariable(spv::NoPrecision, storage, buffer_type, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 1);
  b.addDecoration(dest, spv::DecorationBinding, 0);
  auto pc_type = b.makeStructType(std::vector<spv::Id>(5, u), "Constants");
  b.addDecoration(pc_type, spv::DecorationBlock);
  for (uint32_t i = 0; i < 5; ++i)
    b.addMemberDecoration(pc_type, i, spv::DecorationOffset, i * 4);
  auto pc = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, pc_type, "constants");
  auto invocation = b.createVariable(spv::NoPrecision, spv::StorageClassInput, u3, "gl_GlobalInvocationID");
  b.addDecoration(invocation, spv::DecorationBuiltIn, int(spv::BuiltIn::GlobalInvocationId));
  std::vector<spv::Id> params;
  std::vector<std::vector<spv::Decoration>> precision;
  spv::Block* entry;
  auto main = b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "main", params, precision, &entry);
  auto constant = [&](uint32_t i) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, pc,
        std::vector<spv::Id>{b.makeIntConstant(int32_t(i))}), spv::NoPrecision);
  };
  auto op = [&](spv::Op code, spv::Id a, spv::Id c) { return b.createBinOp(code, u, a, c); };
  auto id = b.createLoad(invocation, spv::NoPrecision);
  auto word_x = b.createCompositeExtract(id, u, 0);
  auto row = b.createCompositeExtract(id, u, 1);
  auto outside = b.createBinOp(spv::OpLogicalOr, boolean,
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, word_x, constant(0)),
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, row, constant(1)));
  {
    SpirvBuilder::IfBuilder skip(outside, spv::SelectionControlMaskNone, b);
    b.makeReturn(false);
    skip.makeEndIf(false);
  }
  auto source_image = b.createLoad(image, spv::NoPrecision);
  auto y = op(spv::OpIAdd, constant(3), row);
  auto x_first = op(spv::OpIAdd, constant(2),
                    op(spv::OpShiftLeftLogical, word_x, b.makeUintConstant(2)));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples;
  const uint32_t tile_half = tile_width >> 1;
  spv::Id word = b.makeUintConstant(0);
  for (uint32_t k = 0; k < 4; ++k) {
    auto x = op(spv::OpIAdd, x_first, b.makeUintConstant(k));
    auto in_tile = op(spv::OpUMod, x, b.makeUintConstant(tile_width));
    auto source_x = op(spv::OpIAdd, op(spv::OpISub, x, in_tile),
        b.createTriOp(spv::OpSelect, u,
            b.createBinOp(spv::OpULessThan, boolean, in_tile,
                          b.makeUintConstant(tile_half)),
            op(spv::OpIAdd, in_tile, b.makeUintConstant(tile_half)),
            op(spv::OpISub, in_tile, b.makeUintConstant(tile_half))));
    spv::Builder::TextureParameters fetch = {};
    fetch.sampler = source_image;
    fetch.coords = b.createCompositeConstruct(u2, std::vector<spv::Id>{source_x, y});
    fetch.lod = b.makeIntConstant(0);
    auto red = b.createCompositeExtract(b.createTextureCall(spv::NoPrecision,
        f4, false, true, false, false, false, fetch, spv::ImageOperandsMaskNone), f, 0);
    // As the transfer shaders pack k_8_8_8_8: round(red * 255).
    auto stencil = op(spv::OpBitwiseAnd,
        b.createUnaryOp(spv::OpConvertFToU, u,
            b.createBinOp(spv::OpFAdd, f,
                b.createBinOp(spv::OpFMul, f, red, b.makeFloatConstant(255.0f)),
                b.makeFloatConstant(0.5f))),
        b.makeUintConstant(0xFF));
    word = op(spv::OpBitwiseOr, word,
              op(spv::OpShiftLeftLogical, stencil, b.makeUintConstant(8 * k)));
  }
  auto address = op(spv::OpIAdd, constant(4),
                    op(spv::OpIAdd, op(spv::OpIMul, row, constant(0)), word_x));
  b.createStore(word, b.createAccessChain(storage, dest,
      std::vector<spv::Id>{b.makeIntConstant(0), address}));
  b.leaveFunction();
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);
  auto ep = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  ep->addIdOperand(invocation);
  if (spirv_version_ >= spv::Spv_1_4) {
    ep->addIdOperand(image);
    ep->addIdOperand(dest);
    ep->addIdOperand(pc);
  }
  std::vector<unsigned int> code;
  b.dump(code);
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(vk, code.data(), code.size() * 4);
  if (!module) return false;
  VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
  ci.layout = stencil_copy_pipeline_layout_;
  const auto result = dfn.vkCreateComputePipelines(vk->device(), VK_NULL_HANDLE, 1, &ci,
      nullptr, &stencil_copy_pipeline_);
  dfn.vkDestroyShaderModule(vk->device(), module, nullptr);
  return result == VK_SUCCESS;
}

bool VulkanRenderTargetCache::EnsureDepthMaterializePipeline() {
  if (depth_materialize_pipeline_ != VK_NULL_HANDLE) return true;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  if (!depth_materialize_pipeline_layout_) {
    VkDescriptorSetLayout sets[] = {descriptor_set_layout_sampled_image_x2_,
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute)};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 5 * 4};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2; ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
    if (dfn.vkCreatePipelineLayout(vk->device(), &ci, nullptr,
        &depth_materialize_pipeline_layout_) != VK_SUCCESS) return false;
  }
  // Already quantized R32 + captured stencil -> exact 24.8 -> guest
  // tiling/endian.
  // Keep this generator extractable for host spirv-val/numerical checks.
  SpirvBuilder b(spirv_version_,
      (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  const auto glsl = b.import("GLSL.std.450");
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto u = b.makeUintType(32), f = b.makeFloatType(32);
  const auto u2 = b.makeVectorType(u, 2), u3 = b.makeVectorType(u, 3);
  const auto f4 = b.makeVectorType(f, 4), boolean = b.makeBoolType();
  auto image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(f, spv::Dim2D, false, false, false, 1,
                      spv::ImageFormatUnknown), "depth_source");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  auto stencil_image = b.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(u, spv::Dim2D, false, false, false, 1,
                      spv::ImageFormatUnknown),
      "stencil_source");
  b.addDecoration(stencil_image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(stencil_image, spv::DecorationBinding, 1);
  auto array = b.makeRuntimeArray(u);
  b.addDecoration(array, spv::DecorationArrayStride, 4);
  auto buffer_type = b.makeStructType(std::vector<spv::Id>{array}, "Destination");
  const auto storage = spirv_version_ >= spv::Spv_1_3
      ? spv::StorageClassStorageBuffer : spv::StorageClassUniform;
  b.addDecoration(buffer_type, spirv_version_ >= spv::Spv_1_3
      ? spv::DecorationBlock : spv::DecorationBufferBlock);
  b.addMemberDecoration(buffer_type, 0, spv::DecorationOffset, 0);
  auto dest = b.createVariable(spv::NoPrecision, storage, buffer_type, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 1);
  b.addDecoration(dest, spv::DecorationBinding, 0);
  auto pc_type = b.makeStructType(std::vector<spv::Id>(5, u), "Constants");
  b.addDecoration(pc_type, spv::DecorationBlock);
  for (uint32_t i = 0; i < 5; ++i)
    b.addMemberDecoration(pc_type, i, spv::DecorationOffset, i * 4);
  auto pc = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, pc_type, "constants");
  auto invocation = b.createVariable(spv::NoPrecision, spv::StorageClassInput, u3, "gl_GlobalInvocationID");
  b.addDecoration(invocation, spv::DecorationBuiltIn, int(spv::BuiltIn::GlobalInvocationId));
  std::vector<spv::Id> params;
  std::vector<std::vector<spv::Decoration>> precision;
  spv::Block* entry;
  auto main = b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "main", params, precision, &entry);
  auto constant = [&](uint32_t i) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, pc,
        std::vector<spv::Id>{b.makeIntConstant(int32_t(i))}), spv::NoPrecision);
  };
  auto op = [&](spv::Op code, spv::Id a, spv::Id c) { return b.createBinOp(code, u, a, c); };
  auto shl = [&](spv::Id a, uint32_t n) { return op(spv::OpShiftLeftLogical, a, b.makeUintConstant(n)); };
  auto shr = [&](spv::Id a, uint32_t n) { return op(spv::OpShiftRightLogical, a, b.makeUintConstant(n)); };
  auto mask = [&](spv::Id a, uint32_t m) { return op(spv::OpBitwiseAnd, a, b.makeUintConstant(m)); };
  auto join = [&](spv::Id a, spv::Id c) { return op(spv::OpBitwiseOr, a, c); };
  auto id = b.createLoad(invocation, spv::NoPrecision);
  auto x = b.createCompositeExtract(id, u, 0), y = b.createCompositeExtract(id, u, 1);
  auto outside = b.createBinOp(spv::OpLogicalOr, boolean,
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, x, constant(0)),
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, y, constant(1)));
  {
    SpirvBuilder::IfBuilder skip(outside, spv::SelectionControlMaskNone, b);
    b.makeReturn(false);
    skip.makeEndIf(false);
  }
  spv::Builder::TextureParameters sample = {};
  sample.sampler = b.createLoad(image, spv::NoPrecision);
  sample.coords = b.createCompositeConstruct(u2, std::vector<spv::Id>{x, y});
  sample.lod = b.makeIntConstant(0);
  auto depth = b.createCompositeExtract(b.createTextureCall(spv::NoPrecision,
      f4, false, true, false, false, false, sample, spv::ImageOperandsMaskNone), f, 0);
  // remap=false: the R32 snapshot already decoded the guest [0,2) value.
  spv::Builder::TextureParameters stencil_sample = {};
  stencil_sample.sampler = b.createLoad(stencil_image, spv::NoPrecision);
  stencil_sample.coords = sample.coords;
  stencil_sample.lod = b.makeIntConstant(0);
  auto stencil = mask(b.createCompositeExtract(
      b.createTextureCall(spv::NoPrecision, b.makeVectorType(u, 4), false,
                          true, false, false, false, stencil_sample,
                          spv::ImageOperandsMaskNone),
      u, 0), 0xFF);
  auto value = join(
      shl(SpirvShaderTranslator::PreClampedDepthTo20e4(b, depth, false, false, glsl), 8),
      stencil);
  auto endian = constant(4);
  auto eq = [&](uint32_t n) { return b.createBinOp(spv::OpIEqual, boolean, endian, b.makeUintConstant(n)); };
  value = b.createTriOp(spv::OpSelect, u, b.createBinOp(spv::OpLogicalOr, boolean, eq(1), eq(2)),
      join(shl(mask(value, 0x00FF00FF), 8), shr(mask(value, 0xFF00FF00), 8)), value);
  value = b.createTriOp(spv::OpSelect, u, b.createBinOp(spv::OpLogicalOr, boolean, eq(2), eq(3)),
      join(shl(value, 16), shr(value, 16)), value);
  // XenosTextureTiledAddress2D, fixed 32bpp. No offset/array/mip variants.
  auto outer = shl(op(spv::OpIAdd, op(spv::OpIMul, shr(y, 5), constant(2)), shr(x, 5)), 6);
  auto inner = join(shl(mask(shr(y, 1), 7), 3), mask(x, 7));
  auto oi = shl(join(outer, inner), 2);
  auto pipe = op(spv::OpBitwiseXor, mask(shr(x, 3), 3), shl(mask(shr(y, 3), 1), 1));
  auto offset = join(join(shl(mask(y, 1), 4), shl(pipe, 6)), shl(mask(shr(y, 4), 1), 11));
  offset = join(offset, join(mask(oi, 15), shl(mask(shr(oi, 4), 1), 5)));
  offset = join(offset, join(shl(mask(shr(oi, 5), 7), 8), shl(shr(oi, 8), 12)));
  auto address = shr(op(spv::OpIAdd, constant(3), offset), 2);
  b.createStore(value, b.createAccessChain(storage, dest,
      std::vector<spv::Id>{b.makeIntConstant(0), address}));
  b.leaveFunction();
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);
  auto ep = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  ep->addIdOperand(invocation);
  if (spirv_version_ >= spv::Spv_1_4) {
    ep->addIdOperand(image);
    ep->addIdOperand(stencil_image);
    ep->addIdOperand(dest);
    ep->addIdOperand(pc);
  }
  std::vector<unsigned int> code;
  b.dump(code);
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(vk, code.data(), code.size() * 4);
  if (!module) return false;
  VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
  ci.layout = depth_materialize_pipeline_layout_;
  const auto result = dfn.vkCreateComputePipelines(vk->device(), VK_NULL_HANDLE, 1, &ci,
      nullptr, &depth_materialize_pipeline_);
  dfn.vkDestroyShaderModule(vk->device(), module, nullptr);
  return result == VK_SUCCESS;
}

// The guest bytes a resolve writes, whatever their values: two resolves with
// the same footprint overwrite exactly the same bytes (format and endianness
// only change the values), so the older one's pending surface is dead.
static uint64_t ResolveDestFootprint(const draw_util::ResolveInfo& info) {
  const FormatInfo* format_info = FormatInfo::Get(
      xenos::TextureFormat(uint32_t(info.copy_dest_info.copy_dest_format)));
  const uint64_t bits = format_info ? format_info->bits_per_pixel : 0;
  draw_util::ResolveCopyDestCoordinateInfo coordinates =
      info.copy_dest_coordinate_info;
  // Only the sample selection doesn't move bytes.
  coordinates.copy_sample_select = xenos::CopySampleSelect(0);
  return ((uint64_t(coordinates.packed) << 24 ^ bits << 16 ^
           uint64_t(info.copy_dest_info.copy_dest_array) << 8 ^
           uint64_t(info.copy_dest_info.copy_dest_slice)) *
              UINT64_C(0x9E3779B97F4A7C15)) ^
         (uint64_t(info.coordinate_info.width_div_8) << 32 ^ info.height_div_8) ^
         uint64_t(info.copy_dest_base) << 17;
}

void VulkanRenderTargetCache::DiscardSurfaceOverwrittenByResolve(
    const draw_util::ResolveInfo& info, VulkanSharedMemory& shared_memory,
    bool recording) {
  if (!info.copy_dest_extent_length) return;
  const uint32_t page_mask =
      (1u << shared_memory.depth_surface_page_size_log2()) - 1;
  const uint32_t start = info.copy_dest_extent_start & ~page_mask;
  const uint64_t end = (uint64_t(info.copy_dest_extent_start) +
                        info.copy_dest_extent_length + page_mask) &
                       ~uint64_t(page_mask);
  if (end > SharedMemory::kBufferSize || end <= start) return;
  if ((!recording &&
       shared_memory.DiscardOverwrittenDepthSurface(
           start, uint32_t(end - start), ResolveDestFootprint(info))) ||
      !cvars::vulkan_resolve_full_overwrite_discard) {
    return;
  }
  // A resolve from the origin across the whole pitch over whole 32-row tile
  // rows writes every byte of its extent: when that extent is exactly the
  // page-aligned range, older pending surfaces inside it are dead whatever
  // their geometry, and the range needs no upload before the write either.
  const FormatInfo* format_info = FormatInfo::Get(
      xenos::TextureFormat(uint32_t(info.copy_dest_info.copy_dest_format)));
  const uint32_t pitch =
      uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5;
  const uint32_t rows = info.height_div_8 << 3;
  if (cvars::log_depth_materialize_callers &&
      info.copy_dest_extent_length >= (UINT32_C(4) << 20)) {
    static uint32_t logged = 0;
    if (logged < 40) {
      ++logged;
      XELOGI("ResolveFullOverwrite: extent {:08X}+{:X} range {:08X}+{:X} "
             "dest {:08X} at {},{} {}x{} pitch {} fmt {} bpp {} array {}",
             info.copy_dest_extent_start, info.copy_dest_extent_length, start,
             uint32_t(end - start), info.copy_dest_base, info.copy_dest_x0,
             info.copy_dest_y0, info.coordinate_info.width_div_8 << 3, rows,
             pitch, uint32_t(info.copy_dest_info.copy_dest_format),
             format_info ? format_info->bits_per_pixel : 0,
             uint32_t(info.copy_dest_info.copy_dest_array));
      XELOGI("ResolveFullOverwrite: recording {}", uint32_t(recording));
    }
  }
  if (!format_info || (format_info->bits_per_pixel & 7) ||
      info.copy_dest_info.copy_dest_array || info.copy_dest_x0 ||
      info.copy_dest_y0 || (info.coordinate_info.width_div_8 << 3) < pitch ||
      (rows & 31) || start != info.copy_dest_extent_start ||
      end - start != info.copy_dest_extent_length ||
      uint64_t(pitch) * rows * (format_info->bits_per_pixel >> 3) !=
          info.copy_dest_extent_length) {
    return;
  }
  if (shared_memory.DiscardDepthSurfacesWithin(start, uint32_t(end - start))) {
    shared_memory.RangeWrittenByGpu(start, uint32_t(end - start));
  }
}

bool VulkanRenderTargetCache::EnsureColorMaterializePipeline() {
  if (color_materialize_pipeline_ != VK_NULL_HANDLE) return true;
  // Same layout as the depth provider (binding 1 unused here).
  if (!EnsureDepthMaterializePipeline()) return false;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  // The k_8_8_8_8 texels -> the guest words (red in the low byte, as the
  // texture loader read them) -> guest tiling/endian.
  // Keep this generator extractable for host spirv-val/numerical checks.
  SpirvBuilder b(spirv_version_,
      (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  const auto glsl = b.import("GLSL.std.450");
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto u = b.makeUintType(32), f = b.makeFloatType(32);
  const auto u2 = b.makeVectorType(u, 2), u3 = b.makeVectorType(u, 3);
  const auto f4 = b.makeVectorType(f, 4), boolean = b.makeBoolType();
  auto image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
      b.makeImageType(f, spv::Dim2D, false, false, false, 1,
                      spv::ImageFormatUnknown), "color_source");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  auto array = b.makeRuntimeArray(u);
  b.addDecoration(array, spv::DecorationArrayStride, 4);
  auto buffer_type = b.makeStructType(std::vector<spv::Id>{array}, "Destination");
  const auto storage = spirv_version_ >= spv::Spv_1_3
      ? spv::StorageClassStorageBuffer : spv::StorageClassUniform;
  b.addDecoration(buffer_type, spirv_version_ >= spv::Spv_1_3
      ? spv::DecorationBlock : spv::DecorationBufferBlock);
  b.addMemberDecoration(buffer_type, 0, spv::DecorationOffset, 0);
  auto dest = b.createVariable(spv::NoPrecision, storage, buffer_type, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 1);
  b.addDecoration(dest, spv::DecorationBinding, 0);
  auto pc_type = b.makeStructType(std::vector<spv::Id>(5, u), "Constants");
  b.addDecoration(pc_type, spv::DecorationBlock);
  for (uint32_t i = 0; i < 5; ++i)
    b.addMemberDecoration(pc_type, i, spv::DecorationOffset, i * 4);
  auto pc = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, pc_type, "constants");
  auto invocation = b.createVariable(spv::NoPrecision, spv::StorageClassInput, u3, "gl_GlobalInvocationID");
  b.addDecoration(invocation, spv::DecorationBuiltIn, int(spv::BuiltIn::GlobalInvocationId));
  std::vector<spv::Id> params;
  std::vector<std::vector<spv::Decoration>> precision;
  spv::Block* entry;
  auto main = b.makeFunctionEntry(spv::NoPrecision, b.makeVoidType(), "main", params, precision, &entry);
  auto constant = [&](uint32_t i) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, pc,
        std::vector<spv::Id>{b.makeIntConstant(int32_t(i))}), spv::NoPrecision);
  };
  auto op = [&](spv::Op code, spv::Id a, spv::Id c) { return b.createBinOp(code, u, a, c); };
  auto shl = [&](spv::Id a, uint32_t n) { return op(spv::OpShiftLeftLogical, a, b.makeUintConstant(n)); };
  auto shr = [&](spv::Id a, uint32_t n) { return op(spv::OpShiftRightLogical, a, b.makeUintConstant(n)); };
  auto mask = [&](spv::Id a, uint32_t m) { return op(spv::OpBitwiseAnd, a, b.makeUintConstant(m)); };
  auto join = [&](spv::Id a, spv::Id c) { return op(spv::OpBitwiseOr, a, c); };
  auto id = b.createLoad(invocation, spv::NoPrecision);
  auto x = b.createCompositeExtract(id, u, 0), y = b.createCompositeExtract(id, u, 1);
  auto outside = b.createBinOp(spv::OpLogicalOr, boolean,
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, x, constant(0)),
      b.createBinOp(spv::OpUGreaterThanEqual, boolean, y, constant(1)));
  {
    SpirvBuilder::IfBuilder skip(outside, spv::SelectionControlMaskNone, b);
    b.makeReturn(false);
    skip.makeEndIf(false);
  }
  spv::Builder::TextureParameters sample = {};
  sample.sampler = b.createLoad(image, spv::NoPrecision);
  sample.coords = b.createCompositeConstruct(u2, std::vector<spv::Id>{x, y});
  sample.lod = b.makeIntConstant(0);
  auto texel = b.createTextureCall(spv::NoPrecision, f4, false, true, false,
                                   false, false, sample,
                                   spv::ImageOperandsMaskNone);
  auto value = b.createBuiltinCall(u, glsl, GLSLstd450PackUnorm4x8,
                                   std::vector<spv::Id>{texel});
  auto endian = constant(4);
  auto eq = [&](uint32_t n) { return b.createBinOp(spv::OpIEqual, boolean, endian, b.makeUintConstant(n)); };
  value = b.createTriOp(spv::OpSelect, u, b.createBinOp(spv::OpLogicalOr, boolean, eq(1), eq(2)),
      join(shl(mask(value, 0x00FF00FF), 8), shr(mask(value, 0xFF00FF00), 8)), value);
  value = b.createTriOp(spv::OpSelect, u, b.createBinOp(spv::OpLogicalOr, boolean, eq(2), eq(3)),
      join(shl(value, 16), shr(value, 16)), value);
  // XenosTextureTiledAddress2D, fixed 32bpp. No offset/array/mip variants.
  auto outer = shl(op(spv::OpIAdd, op(spv::OpIMul, shr(y, 5), constant(2)), shr(x, 5)), 6);
  auto inner = join(shl(mask(shr(y, 1), 7), 3), mask(x, 7));
  auto oi = shl(join(outer, inner), 2);
  auto pipe = op(spv::OpBitwiseXor, mask(shr(x, 3), 3), shl(mask(shr(y, 3), 1), 1));
  auto offset = join(join(shl(mask(y, 1), 4), shl(pipe, 6)), shl(mask(shr(y, 4), 1), 11));
  offset = join(offset, join(mask(oi, 15), shl(mask(shr(oi, 4), 1), 5)));
  offset = join(offset, join(shl(mask(shr(oi, 5), 7), 8), shl(shr(oi, 8), 12)));
  auto address = shr(op(spv::OpIAdd, constant(3), offset), 2);
  b.createStore(value, b.createAccessChain(storage, dest,
      std::vector<spv::Id>{b.makeIntConstant(0), address}));
  b.leaveFunction();
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);
  auto ep = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  ep->addIdOperand(invocation);
  if (spirv_version_ >= spv::Spv_1_4) {
    ep->addIdOperand(image);
    ep->addIdOperand(dest);
    ep->addIdOperand(pc);
  }
  std::vector<unsigned int> code;
  b.dump(code);
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(vk, code.data(), code.size() * 4);
  if (!module) return false;
  VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
  ci.layout = depth_materialize_pipeline_layout_;
  const auto result = dfn.vkCreateComputePipelines(vk->device(), VK_NULL_HANDLE, 1, &ci,
      nullptr, &color_materialize_pipeline_);
  dfn.vkDestroyShaderModule(vk->device(), module, nullptr);
  return result == VK_SUCCESS;
}

bool VulkanRenderTargetCache::TryDeferredDepthSurfaceResolve(
    const draw_util::ResolveInfo& info, VulkanSharedMemory& shared_memory,
    VulkanTextureCache& texture_cache) {
  const bool depth = info.IsCopyingDepth();
  if (!shared_memory.CanPublishDepthSurface() ||
      (!depth && !RenderTargetShortcut(cvars::vulkan_color_surface_resolve)) ||
      IsDrawResolutionScaled() ||
      GetPath() != Path::kHostRenderTargets || !info.copy_dest_extent_length) {
    ++depth_surface_stats_.fallback_schema;
    static uint32_t publish_logged = 0;
    if (info.IsCopyingDepth() && publish_logged < 8) {
      ++publish_logged;
      XELOGI("VkRTTexture: depth resolve not deferred: can_publish={} "
             "scaled={} length={}",
             shared_memory.CanPublishDepthSurface(), IsDrawResolutionScaled(),
             info.copy_dest_extent_length);
    }
    return false;
  }
  const uint32_t page_mask = (1u << shared_memory.depth_surface_page_size_log2()) - 1;
  const uint32_t start = info.copy_dest_extent_start & ~page_mask;
  const uint64_t end = (uint64_t(info.copy_dest_extent_start) + info.copy_dest_extent_length + page_mask) & ~uint64_t(page_mask);
  if (end > SharedMemory::kBufferSize || end <= start || end - start > (8u << 20) ||
      command_processor_.memory_->GetPhysicalHeap()->QueryRangeAccess(start, uint32_t(end - 1)) !=
          xe::memory::PageAccess::kReadWrite ||
      !(depth ? EnsureDepthMaterializePipeline()
              : EnsureColorMaterializePipeline())) {
    ++depth_surface_stats_.fallback_schema;
    static uint32_t range_logged = 0;
    if (range_logged < 8) {
      ++range_logged;
      XELOGI("VkRTTexture: depth resolve range rejected: start={:08X} "
             "end={:08X}", start, uint32_t(end));
    }
    return false;
  }
  // Shadow maps are resolved to the same place every frame: the new resolve
  // rewrites exactly the bytes of the previous pending one, so that one is
  // dropped instead of materialized (a GPU-idle compute + guest write back).
  const uint64_t geometry = ResolveDestFootprint(info);
  const bool overwrote = shared_memory.DiscardOverwrittenDepthSurface(
      start, uint32_t(end - start), geometry);
  if (!shared_memory.CanPublishDepthSurface(uint32_t(end - start))) {
    ++depth_surface_stats_.fallback_resource;
    return false;
  }
  // Preserve only existing shared padding. This materializes an older overlap
  // if necessary, but issues NO compute for the new resolve. After an exact
  // overwrite the padding is already as the previous publish left it.
  if (!overwrote && !shared_memory.RequestRange(start, uint32_t(end - start))) return false;
  const auto slices = shared_memory.GetBufferSlices(info.copy_dest_extent_start, info.copy_dest_extent_length);
  const auto& limits = command_processor_.GetVulkanDevice()->properties();
  if (slices.count != 1 || slices.slices[0].length != info.copy_dest_extent_length ||
      slices.slices[0].length + slices.slices[0].buffer_offset %
          limits.minStorageBufferOffsetAlignment > limits.maxStorageBufferRange) {
    ++depth_surface_stats_.fallback_resource;
    return false;
  }
  auto lock = xe::global_critical_region::AcquireDirect();
  if (command_processor_.memory_->GetPhysicalHeap()->QueryRangeAccess(
          start, uint32_t(end - 1)) != xe::memory::PageAccess::kReadWrite ||
      (!overwrote && !shared_memory.RequestRange(start, uint32_t(end - start)))) return false;
  std::vector<VulkanTextureCache::DepthSurfaceTarget> targets;
  if (!(depth ? DrawDepthSurfaceResolve(info, texture_cache, lock, targets)
             : DrawColorSurfaceResolve(info, texture_cache, lock, targets))) {
    return false;
  }
  const auto source = targets.front();
  ++source.texture->surface_provider_refs;
  texture_cache.MarkRangeAsResolved(info.copy_dest_extent_start, info.copy_dest_extent_length, false);
  shared_memory.PublishDepthSurface(start, uint32_t(end - start), geometry,
      [this, info, source, &shared_memory, &texture_cache](bool wait) {
        return MaterializeDepthSurface(info, source, shared_memory,
                                       texture_cache, wait);
      },
      [source]() { --source.texture->surface_provider_refs; });
  for (const auto& target : targets) texture_cache.PublishDepthSurfaceTarget(target, lock);
  ++depth_surface_stats_.surface_resolves;
  ++depth_surface_stats_.resolves_without_compute;
  // A skipped resolve (2P) pays for its R32 draw (2P). Loader savings are
  // counted only if a guest binding actually uses that snapshot.
  return true;
}

bool VulkanRenderTargetCache::MaterializeDepthSurface(
    const draw_util::ResolveInfo& info, VulkanTextureCache::DepthSurfaceTarget target,
    VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
    bool wait) {
  if (!command_processor_.BeginSubmission(false)) return false;
  const auto* vk = command_processor_.GetVulkanDevice();
  const auto& dfn = vk->functions();
  const auto descriptor_index = descriptor_set_pool_sampled_image_x2_->Allocate();
  const auto dest = command_processor_.AllocateSingleTransientDescriptor(
      VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
  if (descriptor_index == SIZE_MAX || !dest) return false;
  const auto source = descriptor_set_pool_sampled_image_x2_->Get(descriptor_index);
  const auto slices = shared_memory.GetBufferSlices(info.copy_dest_extent_start, info.copy_dest_extent_length);
  if (slices.count != 1 || slices.slices[0].guest_offset != info.copy_dest_extent_start ||
      slices.slices[0].length != info.copy_dest_extent_length) {
    descriptor_set_pool_sampled_image_x2_->Free(descriptor_index);
    return false;
  }
  ++command_processor_.depth_materialization_epoch_;
  command_processor_.EndRenderPass("depth_materialize");
  VkDescriptorImageInfo images[2] = {
      {VK_NULL_HANDLE, texture_cache.PrepareDepthSurfaceSource(target),
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, texture_cache.GetDepthSurfaceStencilSource(target),
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  // Align the descriptor independently of the guest tiled base, including
  // the small aligned prefix in the shader's destination offset.
  const auto& slice = slices.slices[0];
  const auto& limits = vk->properties();
  const VkDeviceSize aligned_offset = slice.buffer_offset &
      ~(VkDeviceSize(limits.minStorageBufferOffsetAlignment) - 1);
  const VkDeviceSize descriptor_size = slice.buffer_offset + slice.length - aligned_offset;
  if (descriptor_size > limits.maxStorageBufferRange) {
    descriptor_set_pool_sampled_image_x2_->Free(descriptor_index);
    return false;
  }
  VkDescriptorBufferInfo buffer = {slice.buffer, aligned_offset, descriptor_size};
  VkWriteDescriptorSet writes[2] = {};
  for (auto& w : writes) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.descriptorCount = 1; }
  writes[0].dstSet = source; writes[0].descriptorCount = 2;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = images;
  writes[1].dstSet = dest; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &buffer;
  dfn.vkUpdateDescriptorSets(vk->device(), 2, writes, 0, nullptr);
  shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                    {info.copy_dest_extent_start, info.copy_dest_extent_length});
  auto& commands = command_processor_.deferred_command_buffer();
  command_processor_.BindExternalComputePipeline(
      info.IsCopyingDepth() ? depth_materialize_pipeline_
                            : color_materialize_pipeline_);
  VkDescriptorSet sets[] = {source, dest};
  commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
      depth_materialize_pipeline_layout_, 0, 2, sets, 0, nullptr);
  uint32_t constants[] = {target.width, target.height,
      uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32),
      uint32_t(slice.buffer_offset - aligned_offset + info.copy_dest_base - slice.guest_offset),
      uint32_t(info.copy_dest_info.copy_dest_endian)};
  commands.CmdVkPushConstants(depth_materialize_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(constants), constants);
  command_processor_.SubmitBarriers(true);
  if (commands.pass_bytes_enabled()) {
    // Byte ledger: reads the resolved texture, writes guest memory; no render
    // target is touched.
    PassByteLedger::Event tag;
    tag.kind = PassByteLedger::kTag;
    tag.category = PassByteLedger::kResolve;
    commands.AnnotatePassBytes(tag);
  }
  commands.CmdVkDispatch((target.width + 7) / 8, (target.height + 7) / 8, 1,
                         DeferredCommandBuffer::DispatchSite::kMaterializeDepth);
  if (!wait) {
    // The GPU work that needs the bytes follows in the stream (Use() orders
    // it after this compute write). The source image may change after this
    // point in stream order; the descriptor set lives until completion.
    materialize_descriptors_in_flight_.emplace_back(
        command_processor_.GetCurrentSubmission(), descriptor_index);
    --target.texture->surface_provider_refs;
    ++depth_surface_stats_.demand_computes;
    depth_surface_stats_.estimated_bytes_saved -=
        int64_t(target.width) * target.height * 8;
    return true;
  }
  command_processor_.PushBufferMemoryBarrier(slice.buffer, slice.buffer_offset, slice.length,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  command_processor_.SubmitBarriers(true);
  const bool completed = command_processor_.EndSubmission(false) &&
                         command_processor_.AwaitAllQueueOperationsCompletion();
  if (completed) {
    descriptor_set_pool_sampled_image_x2_->Free(descriptor_index);
    --target.texture->surface_provider_refs;
  }
  ++depth_surface_stats_.demand_computes;
  // Charge R32 read + guest write. Guest texture upload savings were counted
  // only for the first binding below, not for merely creating the snapshot.
  depth_surface_stats_.estimated_bytes_saved -= int64_t(target.width) * target.height * 8;
  return completed && command_processor_.BeginSubmission(false);
}

bool VulkanRenderTargetCache::Resolve(
    const Memory& memory, VulkanSharedMemory& shared_memory,
    VulkanTextureCache& texture_cache, uint32_t& written_address_out,
    uint32_t& written_length_out, reg::RB_COPY_DEST_INFO* copy_dest_info_out,
    bool* written_scaled_out, const draw_util::ResolveInfo* captured_info) {
  SCOPE_profile_cpu_f("gpu");
  // Transfers queued for a draw that was never encoded must be done before
  // the resolve reads the render targets.
  FlushPendingDrawPassTransfers();
  // A resolve reads its source before clearing it, so even a copy-and-clear
  // must materialize the previous contents first.
  FlushPendingClearTransfers();
  written_address_out = 0;
  written_length_out = 0;
  if (written_scaled_out) {
    *written_scaled_out = false;
  }

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  if (captured_info) {
    resolve_info = *captured_info;
  } else if (!GetResolveInfoForDefer(memory, resolve_info)) {
    XELOGE("Resolve: GetResolveInfo failed");
    return false;
  }

  if (command_processor_.resolve_defer_census_.active) {
    command_processor_.ResolveDeferDescribe(
        resolve_info, command_processor_.deferred_resolve_recording_);
  }
  // RTTRACE: log every resolve (including clear-only ones) before any early
  // return below.
  if (command_processor_.IsRtTraceActive()) {
    const draw_util::ResolveEdramInfo& rt_trace_src =
        resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                      : resolve_info.color_edram_info;
    XELOGI(
        "RTTRACE resolve src={}/{}/{}/msaa{}/depth{} rect=[{},{},{},{}] "
        "copy_dest=0x{:08X} copy_cmd={} clear_color={} clear_depth={} "
        "clear_values=0x{:08X}/0x{:08X} zsrc={}/{}",
        rt_trace_src.base_tiles, rt_trace_src.pitch_tiles, rt_trace_src.format,
        uint32_t(rt_trace_src.msaa_samples),
        uint32_t(resolve_info.IsCopyingDepth()),
        resolve_info.coordinate_info.edram_offset_x_div_8 << 3,
        resolve_info.coordinate_info.edram_offset_y_div_8 << 3,
        resolve_info.coordinate_info.width_div_8 << 3,
        resolve_info.height_div_8 << 3, resolve_info.copy_dest_base,
        uint32_t(resolve_info.rb_copy_control.copy_command),
        uint32_t(resolve_info.IsClearingColor()),
        uint32_t(resolve_info.IsClearingDepth()), resolve_info.rb_depth_clear,
        resolve_info.rb_color_clear, resolve_info.depth_edram_info.base_tiles,
        resolve_info.depth_edram_info.pitch_tiles);
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    if (command_processor_.IsRtTraceActive()) {
      XELOGI("RTTRACE resolve early_return reason={}", "zero-size-region");
    }
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  // Copying.
  bool copied = false;
  bool surface_resolve_taken = false;
  bool direct_host_used = false;
  if (resolve_info.copy_dest_extent_length) {
    if (command_processor_.debug_markers_enabled()) {
      char label[draw_util::kDebugMarkerLabelMaxLength];
      draw_util::FormatResolveCopyDebugMarker(label, sizeof(label),
                                              resolve_info);
      command_processor_.PushDebugMarker("%s", label);
    }
    // If everything owning the source is native, copy at 1x1 into shared
    // memory.
    bool copy_native = false;
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x = 0, copy_group_count_y = 0;
    draw_util::ResolveCopyShaderIndex copy_shader =
        draw_util::ResolveCopyShaderIndex::kUnknown;
    if (GetPath() == Path::kHostRenderTargets) {
      uint32_t dump_base;
      uint32_t dump_row_length_used;
      uint32_t dump_rows;
      uint32_t dump_pitch;
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
      copy_native = IsResolveSourceNativeOnly(dump_base, dump_row_length_used,
                                              dump_rows, dump_pitch);
      if (copy_native) {
        // Redo the resolve info at 1x1 so the scale-dependent fields match
        // what the unscaled copy shaders expect.
        if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_,
                                       1, 1, IsFixedRG16TruncatedToMinus1To1(),
                                       IsFixedRGBA16TruncatedToMinus1To1(),
                                       resolve_info)) {
          return false;
        }
      }
      // The copy shader depends on the scale the copy will run at, which is
      // only known once copy_native is decided, so derive it here - the fork's
      // in-pass and direct-host paths below consume it.
      copy_shader = resolve_info.GetCopyShader(
          copy_native ? 1 : draw_resolution_scale_x(),
          copy_native ? 1 : draw_resolution_scale_y(), copy_shader_constants,
          copy_group_count_x, copy_group_count_y);
      assert_true(copy_group_count_x && copy_group_count_y);
      {
        static std::atomic<uint32_t> dbg_gameplay_resolves{0};
        const uint32_t frame = command_processor_.GetCurrentFrame();
        if (frame >= 4500 &&
            dbg_gameplay_resolves.fetch_add(1, std::memory_order_relaxed) <
                200) {
          const draw_util::ResolveEdramInfo& source =
              resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                            : resolve_info.color_edram_info;
          XELOGI(
              "RTFLOW frame={} src_base={} src_pitch={} src_msaa={} "
              "src_format={} src64={} depth={} rect={}x{} dest_format={} "
              "dest_pitch={} shader={} native={}",
              frame, source.base_tiles, source.pitch_tiles,
              uint32_t(source.msaa_samples), source.format,
              source.format_is_64bpp, uint32_t(resolve_info.IsCopyingDepth()),
              resolve_info.coordinate_info.width_div_8 << 3,
              resolve_info.height_div_8 << 3,
              uint32_t(resolve_info.copy_dest_info.copy_dest_format),
              resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32,
              uint32_t(copy_shader), uint32_t(copy_native));
        }
      }
      if (!CaptureSourceRenderTarget(resolve_info, dump_base,
                                     dump_row_length_used, dump_rows,
                                     dump_pitch)) {
        return false;
      }
      if (RenderTargetShortcut(cvars::vulkan_texture_from_render_target) &&
          !command_processor_.deferred_resolve_recording_ &&
          TryDeferredDepthSurfaceResolve(resolve_info, shared_memory,
                                        texture_cache)) {
        copied = surface_resolve_taken = true;
        written_address_out = resolve_info.copy_dest_extent_start;
        written_length_out = resolve_info.copy_dest_extent_length;
        if (copy_dest_info_out) *copy_dest_info_out = resolve_info.copy_dest_info;
      }
      // A resolve on another path rewriting a pending surface's exact bytes
      // must not materialize it first (with a GPU wait) for nothing.
      // Publication happens at record time for a resolve recorded for the
      // end of the pass too, so a complete overwrite may retire records then.
      if (!copied &&
          RenderTargetShortcut(cvars::vulkan_texture_from_render_target) &&
          copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
        DiscardSurfaceOverwrittenByResolve(
            resolve_info, shared_memory,
            command_processor_.deferred_resolve_recording_);
      }
      // Try the on-tile resolve before dumping the owning render targets.
      if (!copied && cvars::vulkan_in_pass_resolve &&
          copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
        copied = TryInPassResolveCopy(
            resolve_info, copy_shader_constants, copy_shader, dump_base,
            dump_row_length_used, dump_rows, dump_pitch, shared_memory,
            texture_cache, written_address_out, written_length_out);
        direct_host_used = copied;
        ++inpass_attempts_;
        inpass_taken_ += uint32_t(copied);
        if ((inpass_attempts_ % 512) == 0) {
          XELOGI("VkInPass: {}/{} in-pass", inpass_taken_, inpass_attempts_);
          XELOGI("VkInPassConvert: taken={} destination_bytes={}",
                 inpass_conversions_, inpass_conversion_bytes_);
          {
            char fmt_line[256];
            int fmt_len = 0;
            for (uint32_t fi = 0; fi < 16; ++fi) {
              if (!inpass_format_hist_[fi]) {
                continue;
              }
              fmt_len += std::snprintf(fmt_line + fmt_len,
                                       sizeof(fmt_line) - fmt_len, "%u=%llu ",
                                       fi,
                                       (unsigned long long)inpass_format_hist_[fi]);
            }
            XELOGI("VkInPassFmt: {}", fmt_line);
          }
          {
            char rej_line[512];
            int rej_len = 0;
            for (uint32_t ri = 0; ri < inpass_reject_reason_count_; ++ri) {
              rej_len += std::snprintf(
                  rej_line + rej_len, sizeof(rej_line) - rej_len,
                  "%s=%llu ", inpass_reject_reasons_[ri],
                  (unsigned long long)inpass_reject_counts_[ri]);
            }
            XELOGI("VkInPassReject: {}", rej_line);
          }
        }
      }
      if (!copied && RenderTargetShortcut(cvars::vulkan_direct_host_resolve) &&
          copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
        copied = TryDirectHostResolveCopy(
            resolve_info, copy_shader_constants, copy_shader, dump_base,
            dump_row_length_used, dump_rows, dump_pitch, shared_memory,
            texture_cache, written_address_out, written_length_out);
        direct_host_used = copied;
      }
      if (!copied || (cvars::vulkan_in_pass_resolve_debug_dump &&
                      !surface_resolve_taken)) {
        DumpRenderTargets(dump_base, dump_row_length_used, dump_rows,
                          dump_pitch, copy_native);
      }
    }
    bool copy_dest_scaled = draw_resolution_scaled && !copy_native;

    if (!copied && copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      // TODO(Triang3l): Resolution-scaled buffer committing.
      copy_dest_committed =
          shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length);
      if (!copy_dest_committed) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      } else {
        // TODO(Triang3l): Switching between descriptors if exceeding
        // maxStorageBufferRange.
        // Bind the whole shared memory buffer persistently when possible
        // (passing the destination byte offset via dest_base) instead of
        // allocating and writing a per-resolve descriptor. Scaled resolves
        // write to separate scaled buffers, so they use transient descriptors.
        const bool use_persistent_dest =
            texture_cache.shared_memory_persistent_descriptor_set() !=
                VK_NULL_HANDLE &&
            !draw_resolution_scaled;
        VkDescriptorSet descriptor_set_dest =
            use_persistent_dest
                ? texture_cache.shared_memory_persistent_descriptor_set()
                : command_processor_.AllocateSingleTransientDescriptor(
                      VulkanCommandProcessor::SingleTransientDescriptorLayout ::
                          kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          // Write the destination descriptor.
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;

          bool scaled_buffer_ready = false;
          if (copy_dest_scaled) {
            // For scaled resolve, ensure the scaled buffer exists and bind to
            // it
            uint32_t dest_address = resolve_info.copy_dest_base;
            uint32_t dest_length = resolve_info.copy_dest_extent_start -
                                   resolve_info.copy_dest_base +
                                   resolve_info.copy_dest_extent_length;

            // Ensure scaled resolve memory is committed
            scaled_buffer_ready = true;
            if (!texture_cache.EnsureScaledResolveMemoryCommittedPublic(
                    dest_address, dest_length)) {
              XELOGE(
                  "Failed to commit scaled resolve memory for resolve dest at "
                  "0x{:08X}",
                  dest_address);
              scaled_buffer_ready = false;
            }

            // Make the range current to get the buffer
            if (scaled_buffer_ready &&
                !texture_cache.MakeScaledResolveRangeCurrent(dest_address,
                                                             dest_length)) {
              XELOGE(
                  "Failed to make scaled resolve range current for resolve "
                  "dest at 0x{:08X}",
                  dest_address);
              scaled_buffer_ready = false;
            }

            // Get the current scaled buffer
            VkBuffer scaled_buffer = VK_NULL_HANDLE;
            if (scaled_buffer_ready) {
              scaled_buffer = texture_cache.GetCurrentScaledResolveBuffer();
              if (scaled_buffer == VK_NULL_HANDLE) {
                XELOGE(
                    "No current scaled resolve buffer for resolve dest at "
                    "0x{:08X}",
                    dest_address);
                scaled_buffer_ready = false;
              }
            }

            if (scaled_buffer_ready) {
              // Calculate offset within the scaled buffer
              uint32_t draw_resolution_scale_area =
                  draw_resolution_scale_x() * draw_resolution_scale_y();
              uint64_t scaled_offset =
                  uint64_t(dest_address) * draw_resolution_scale_area;
              uint64_t buffer_relative_offset =
                  scaled_offset -
                  texture_cache.GetCurrentScaledResolveBufferBaseOffset();

              write_descriptor_set_dest_buffer_info.buffer = scaled_buffer;
              write_descriptor_set_dest_buffer_info.offset =
                  buffer_relative_offset;
              write_descriptor_set_dest_buffer_info.range =
                  dest_length * draw_resolution_scale_area;
            }
          }

          if (!scaled_buffer_ready && !use_persistent_dest) {
            // Write unscaled or native resolves to shared memory.
            if (copy_dest_scaled) {
              XELOGW(
                  "Falling back to unscaled resolve at 0x{:08X} - scaled "
                  "buffer not available",
                  resolve_info.copy_dest_base);
            }
            write_descriptor_set_dest_buffer_info.buffer =
                shared_memory.buffer();
            write_descriptor_set_dest_buffer_info.offset =
                resolve_info.copy_dest_base;
            write_descriptor_set_dest_buffer_info.range =
                resolve_info.copy_dest_extent_start -
                resolve_info.copy_dest_base +
                resolve_info.copy_dest_extent_length;
          }
          if (!use_persistent_dest) {
            VkWriteDescriptorSet write_descriptor_set_dest;
            write_descriptor_set_dest.sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_descriptor_set_dest.pNext = nullptr;
            write_descriptor_set_dest.dstSet = descriptor_set_dest;
            write_descriptor_set_dest.dstBinding = 0;
            write_descriptor_set_dest.dstArrayElement = 0;
            write_descriptor_set_dest.descriptorCount = 1;
            write_descriptor_set_dest.descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write_descriptor_set_dest.pImageInfo = nullptr;
            write_descriptor_set_dest.pBufferInfo =
                &write_descriptor_set_dest_buffer_info;
            write_descriptor_set_dest.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0,
                                       nullptr);
          }

          // Submit the resolve.
          if (!scaled_buffer_ready) {
            // Regular unscaled - transition shared memory for write
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<uint32_t, uint32_t>(
                                  resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length));
          } else {
            // Scaled - add barrier for the scaled resolve buffer
            // The buffer transitions from compute shader read (texture loading)
            // to compute shader write
            VkBuffer scaled_buffer =
                texture_cache.GetCurrentScaledResolveBuffer();
            if (scaled_buffer != VK_NULL_HANDLE) {
              VkBufferMemoryBarrier buffer_barrier = {};
              buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
              // More specific: previous compute shader reads to compute shader
              // write
              buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
              buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
              buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.buffer = scaled_buffer;
              buffer_barrier.offset = 0;
              buffer_barrier.size = VK_WHOLE_SIZE;

              command_buffer.CmdVkPipelineBarrier(
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,  // From compute shader
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,  // To compute shader
                  0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
            }
          }
          UseEdramBuffer(EdramBufferUsage::kComputeRead);
          auto resolve_texture_lock =
              xe::global_critical_region::AcquireDirect();
          VulkanTextureCache::ResolveTextureCopy texture_copy;
          if (copy_dest_scaled == scaled_buffer_ready)
            texture_copy = texture_cache.PrepareResolveTextureCopy(
                resolve_info, scaled_buffer_ready, resolve_texture_lock);
          VulkanCommandProcessor::ScratchBufferAcquisition texture_scratch;
          VkDescriptorSet linear_descriptor = descriptor_set_dest;
          if (texture_copy.size) {
            texture_scratch = command_processor_.AcquireScratchGpuBuffer(
                texture_copy.size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
            texture_scratch.SetAccessMask(VK_ACCESS_MEMORY_WRITE_BIT);
            VkDescriptorSet descriptor =
                command_processor_.AllocateSingleTransientDescriptor(
                    VulkanCommandProcessor::SingleTransientDescriptorLayout::
                        kStorageBufferCompute);
            if (texture_scratch.buffer() != VK_NULL_HANDLE &&
                descriptor != VK_NULL_HANDLE) {
              VkDescriptorBufferInfo buffer_info = {texture_scratch.buffer(), 0,
                                                    texture_copy.size};
              VkWriteDescriptorSet write = {
                  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
              write.dstSet = descriptor;
              write.descriptorCount = 1;
              write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
              write.pBufferInfo = &buffer_info;
              dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
              linear_descriptor = descriptor;
            } else {
              texture_copy = {};
            }
          }
          copy_shader_constants.dest_relative.dest_info.value &=
              ~UINT32_C(0xE0000000);
          if (texture_copy.size)
            copy_shader_constants.dest_relative.dest_info.value |=
                UINT32_C(0x80000000) | texture_copy.linear_dest_flags;
          // Fully native resolves use the unscaled shader variant with the
          // full push constant layout.
          VkPipelineLayout copy_pipeline_layout =
              copy_native ? resolve_copy_native_pipeline_layout_
                          : resolve_copy_pipeline_layout_;
          command_processor_.BindExternalComputePipeline(
              copy_native
                  ? resolve_copy_native_pipelines_[size_t(copy_shader)]
                  : resolve_copy_pipelines_[size_t(copy_shader)]);
          VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
          descriptor_sets[kResolveCopyDescriptorSetEdram] =
              edram_storage_buffer_descriptor_set_;
          descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
          descriptor_sets[kResolveCopyDescriptorSetUnused] =
              descriptor_set_dest;
          descriptor_sets[kResolveCopyDescriptorSetLinear] = linear_descriptor;
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, copy_pipeline_layout, 0,
              uint32_t(xe::countof(descriptor_sets)), descriptor_sets, 0,
              nullptr);
          if (copy_dest_scaled) {
            command_buffer.CmdVkPushConstants(
                copy_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants.dest_relative),
                &copy_shader_constants.dest_relative);
          } else {
            // TODO(Triang3l): Multiple shared memory bindings in case of
            // splitting due to maxStorageBufferRange overflow.
            if (!use_persistent_dest) {
              // The descriptor is offset to the destination, so make dest_base
              // relative to it. With the whole buffer bound persistently,
              // dest_base stays the absolute byte offset.
              copy_shader_constants.dest_base -=
                  uint32_t(write_descriptor_set_dest_buffer_info.offset);
            }
            command_buffer.CmdVkPushConstants(
                copy_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants), &copy_shader_constants);
          }
          command_processor_.SubmitBarriers(true);
          if (command_buffer.pass_bytes_enabled()) {
            using L = PassByteLedger;
            const auto& src = resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                                          : resolve_info.color_edram_info;
            const uint64_t pixels = uint64_t(resolve_info.coordinate_info.width_div_8 << 3) *
                (resolve_info.height_div_8 << 3) *
                (copy_native ? 1 : draw_resolution_scale_x() * draw_resolution_scale_y());
            L::Event e; e.kind = L::kRead; e.category = L::kResolve; e.conservative = true;
            e.read = pixels * (src.format_is_64bpp ? 8 : 4) * (1u << uint32_t(src.msaa_samples));
            e.write = pixels * (1u << draw_util::GetResolveDownscalePixelSizeLog2(resolve_info.copy_dest_info));
            if (texture_copy.size) e.write *= 2;
            command_buffer.AnnotatePassBytes(e);
            L::Event tag; tag.kind = L::kTag; command_buffer.AnnotatePassBytes(tag);
          }
          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y,
                                       1, DeferredCommandBuffer::DispatchSite::kResolve);

          // Add barrier after writing to scaled resolve buffer
          if (scaled_buffer_ready) {
            VkBuffer scaled_buffer =
                texture_cache.GetCurrentScaledResolveBuffer();
            if (scaled_buffer != VK_NULL_HANDLE) {
              VkBufferMemoryBarrier buffer_barrier = {};
              buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
              buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
              buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
              buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.buffer = scaled_buffer;
              buffer_barrier.offset = 0;
              buffer_barrier.size = VK_WHOLE_SIZE;

              command_buffer.CmdVkPipelineBarrier(
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                  &buffer_barrier, 0, nullptr);
            }
          }

          texture_cache.CompleteResolveTextureCopy(
              texture_copy, texture_scratch.buffer(), resolve_info,
              scaled_buffer_ready, resolve_texture_lock);
          if (texture_copy.size) {
            texture_scratch.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT);
            texture_scratch.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT);
          }
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          if (copy_dest_info_out) {
            // Normalized copy format (depth format for depth resolves) - the
            // texel size the readback downscale expects for the extent.
            *copy_dest_info_out = resolve_info.copy_dest_info;
          }
          if (written_scaled_out) {
            *written_scaled_out = scaled_buffer_ready;
          }
          copied = true;
        }
      }
    }
    command_processor_.PopDebugMarker();
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    if (command_processor_.debug_markers_enabled()) {
      char label[draw_util::kDebugMarkerLabelMaxLength];
      draw_util::FormatResolveClearDebugMarker(
          label, sizeof(label), resolve_info, clear_depth, clear_color);
      command_processor_.PushDebugMarker("%s", label);
    }
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(
                resolve_info, clear_rectangle, clear_render_targets[0],
                clear_transfers_[0], clear_render_targets[1],
                clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          // For 64bpp formats, RB_COLOR_CLEAR_LO is the lower 32 bits of the
          // packed clear value. RB_COLOR_CLEAR is the upper 32 bits and, for
          // 32bpp formats, the whole value.
          clear_values[1] =
              resolve_info.color_edram_info.format_is_64bpp
                  ? resolve_info.rb_color_clear_lo |
                        (uint64_t(resolve_info.rb_color_clear) << 32)
                  : resolve_info.rb_color_clear;
          PerformTransfersAndResolveClears(2, clear_render_targets,
                                           clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Should be safe to only commit once (if was accessed as unordered or
        // with fragment shader interlock previously - if there was nothing to
        // copy, only to clear, for some reason, for instance), overlap of the
        // depth and the color ranges is highly unlikely.
        CommitEdramBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, resolve_fsi_clear_pipeline_layout_,
            0, 1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<uint32_t, uint32_t> clear_group_count =
            resolve_info.GetClearShaderGroupCount(draw_resolution_scale_x(),
                                                  draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(
              resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_buffer.CmdVkPushConstants(
              resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
              0, sizeof(depth_clear_constants), &depth_clear_constants);
          command_processor_.SubmitBarriers(true);
          if (command_buffer.pass_bytes_enabled()) {
            using L = PassByteLedger;
            L::Event event; event.kind = L::kClear; event.category = L::kClearBytes;
            event.conservative = true;
            event.write = uint64_t(resolve_info.coordinate_info.width_div_8 << 3) *
                (resolve_info.height_div_8 << 3) * draw_resolution_scale_x() *
                draw_resolution_scale_y() * 4 *
                (1u << uint32_t(resolve_info.depth_edram_info.msaa_samples));
            command_buffer.AnnotatePassBytes(event);
            L::Event tag; tag.kind = L::kTag; command_buffer.AnnotatePassBytes(tag);
          }
          command_buffer.CmdVkDispatch(clear_group_count.first,
                                       clear_group_count.second, 1,
                                       DeferredCommandBuffer::DispatchSite::kResolveClear);
        }
        if (clear_color) {
          command_processor_.BindExternalComputePipeline(
              resolve_info.color_edram_info.format_is_64bpp
                  ? resolve_fsi_clear_64bpp_pipeline_
                  : resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                uint32_t(offsetof(draw_util::ResolveClearShaderConstants,
                                  rt_specific)),
                sizeof(color_clear_constants.rt_specific),
                &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          if (command_buffer.pass_bytes_enabled()) {
            using L = PassByteLedger;
            L::Event event; event.kind = L::kClear; event.category = L::kClearBytes;
            event.conservative = true;
            event.write = uint64_t(resolve_info.coordinate_info.width_div_8 << 3) *
                (resolve_info.height_div_8 << 3) * draw_resolution_scale_x() *
                draw_resolution_scale_y() * (resolve_info.color_edram_info.format_is_64bpp ? 8 : 4) *
                (1u << uint32_t(resolve_info.color_edram_info.msaa_samples));
            command_buffer.AnnotatePassBytes(event);
            L::Event tag; tag.kind = L::kTag; command_buffer.AnnotatePassBytes(tag);
          }
          command_buffer.CmdVkDispatch(clear_group_count.first,
                                       clear_group_count.second, 1,
                                       DeferredCommandBuffer::DispatchSite::kResolveClear);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
    command_processor_.PopDebugMarker();
  } else {
    cleared = true;
  }

  if (cvars::log_resolve_details) {
    RecordResolveDetails(resolve_info, copied, direct_host_used);
  }

  return copied && cleared;
}

void VulkanRenderTargetCache::RecordResolveDetails(
    const draw_util::ResolveInfo& resolve_info, bool copied, bool direct_host) {
  ResolveDetailsStats& s = resolve_details_stats_;
  if (!resolve_info.copy_dest_extent_length) {
    ++s.clears_only;
    return;
  }
  ++s.copies;
  const draw_util::ResolveEdramInfo edram_info =
      resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                    : resolve_info.color_edram_info;
  const uint32_t path = direct_host ? 0 : (copied ? 1 : 2);
  const uint64_t key =
      uint64_t(path) | (uint64_t(edram_info.is_depth) << 2) |
      (uint64_t(edram_info.msaa_samples) << 3) |
      (uint64_t(edram_info.format) << 5) |
      (uint64_t(edram_info.format_is_64bpp) << 9) |
      (uint64_t(resolve_info.copy_dest_info.copy_dest_format) << 10) |
      (uint64_t(resolve_info.coordinate_info.width_div_8) << 16) |
      (uint64_t(resolve_info.height_div_8) << 27);
  const uint64_t bytes = copied ? resolve_info.copy_dest_extent_length : 0;
  s.total_bytes += bytes;
  size_t i = 0;
  for (; i < s.bucket_count; ++i) {
    if (s.buckets[i].key == key) {
      break;
    }
  }
  if (i == s.bucket_count &&
      s.bucket_count < ResolveDetailsStats::kMaxBuckets) {
    s.buckets[s.bucket_count].key = key;
    s.buckets[s.bucket_count].count = 0;
    s.buckets[s.bucket_count].bytes = 0;
    ++s.bucket_count;
  }
  if (i < s.bucket_count) {
    ++s.buckets[i].count;
    s.buckets[i].bytes += bytes;
  }
  // Destination address reuse. A destination re-resolved every frame is a live
  // per-frame surface (consumed as a texture); repeats within one frame
  // indicate tiled/multi-rect resolves or redundant re-resolves of the same
  // buffer.
  const uint32_t dest = resolve_info.copy_dest_extent_start;
  if (std::find(resolve_dests_current_frame_.begin(),
                resolve_dests_current_frame_.end(), dest) !=
      resolve_dests_current_frame_.end()) {
    ++s.dest_repeats_in_frame;
  } else {
    ++s.dest_distinct;
    if (resolve_dests_current_frame_.size() < 256) {
      resolve_dests_current_frame_.push_back(dest);
    }
    if (std::find(resolve_dests_previous_frame_.begin(),
                  resolve_dests_previous_frame_.end(), dest) !=
        resolve_dests_previous_frame_.end()) {
      ++s.dest_repeats_cross_frame;
    }
  }
}

void VulkanRenderTargetCache::LogResolveDetailsOnFrameEnd() {
  if (cvars::vulkan_lazy_constant_rows && cvars::log_in_pass_transfer_rejects > 0) {
    static uint32_t lazy_frames = 0;
    if (++lazy_frames >= uint32_t(cvars::log_in_pass_transfer_rejects)) {
      lazy_frames = 0;
      auto& l = lazy_rows_stats_;
      uint64_t pending_pixels = 0;
      for (const auto& entry : pending_constant_rows_) {
        for (const VkRect2D& rect : entry.rects) {
          pending_pixels += uint64_t(rect.extent.width) * rect.extent.height;
        }
      }
      XELOGI("[LAZYROWS] lazy_clears={} kept_rects={} kept_pixels={} "
             "pending_targets={} pending_pixels={} materialized: "
             "transfer_dest={} transfer_source={} resolve={} external={}",
             l.lazy_clears, l.kept_rows_rects, l.kept_pixels,
             pending_constant_rows_.size(), pending_pixels, l.materialized[0],
             l.materialized[1], l.materialized[2], l.materialized[3]);
      l = {};
    }
  }
  if (cvars::log_in_pass_transfer_rejects > 0 &&
      ++in_pass_transfer_frames_ >=
          uint32_t(cvars::log_in_pass_transfer_rejects)) {
    std::string reasons;
    for (size_t i = 1; i < in_pass_transfer_verdicts_.size(); ++i) {
      if (in_pass_transfer_verdicts_[i]) {
        reasons += fmt::format(" r{}={}", i, in_pass_transfer_verdicts_[i]);
      }
    }
    std::string callers;
    for (const auto& slot : own_pass_transfer_callers_) {
      if (slot.second) {
        callers += fmt::format(" {:X}={}", slot.first, slot.second);
      }
    }
    XELOGI("[XFERPASS] frames={} in_pass={} rejected:{} own_pass_callers:{}",
           in_pass_transfer_frames_, in_pass_transfer_verdicts_[0], reasons,
           callers);
    own_pass_transfer_callers_ = {};
    in_pass_transfer_verdicts_ = {};
    in_pass_transfer_frames_ = 0;
  }
  if (RenderTargetShortcut(cvars::vulkan_texture_from_render_target)) {
    auto& d = depth_surface_stats_;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!d.last_report_ns) d.last_report_ns = now;
    if (now - d.last_report_ns >= 1000000000ull) {
      const auto p = command_processor_.shared_memory_->TakeDepthProviderStats();
      const double seconds = double(now - d.last_report_ns) / 1.0e9;
      XELOGI("VkRTTexture lazy_compute=1 eager_private=0 interval_s={:.3f} "
             "surface_resolves={} resolve_without_compute={} snapshots={} surface_texture_hits={} "
             "materializations_on_demand={} materializations={} gpu_materializations={} materialized_extent_bytes={} "
             "cpu_provider={} discarded={} discarded_bytes={} pending_bytes={} estimated_bytes_saved={} "
             "logical_surface_read_bytes={} logical_surface_write_bytes={} "
             "fallback_schema={} fallback_msaa={} fallback_owner={} fallback_texture={} "
             "fallback_resource={} fallback_stencil={} callers:{}",
             seconds, d.surface_resolves, d.resolves_without_compute, d.snapshots, d.texture_hits,
             d.demand_computes, p.materializations, p.gpu_materializations,
             p.materialized_bytes, p.cpu_accesses,
             p.discarded, p.discarded_bytes, p.pending_bytes, d.estimated_bytes_saved - int64_t(p.materialized_bytes) * 2,
             d.surface_read_bytes,
             d.surface_write_bytes, d.fallback_schema, d.fallback_msaa, d.fallback_owner,
             d.fallback_texture, d.fallback_resource, d.fallback_stencil,
             command_processor_.shared_memory_->TakeMaterializeCallers());
      d = {};
      d.last_report_ns = now;
    }
  }
  if (!cvars::log_resolve_details) {
    return;
  }
  ResolveDetailsStats& s = resolve_details_stats_;
  ++s.frames;
  resolve_dests_previous_frame_.swap(resolve_dests_current_frame_);
  resolve_dests_current_frame_.clear();
  const uint64_t now =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
  if (!s.last_report_ns) {
    s.last_report_ns = now;
    return;
  }
  if (now - s.last_report_ns < 1000000000ull) {
    return;
  }
  const double f = double(std::max(s.frames, 1u));
  XELOGI(
      "ResolveDetails: {} frames | per frame: copies={:.1f} "
      "clears-only={:.1f} MB={:.2f} | dests per frame: distinct={:.1f} "
      "in-frame-repeats={:.1f} recurring-vs-prev-frame={:.1f}",
      s.frames, s.copies / f, s.clears_only / f,
      s.total_bytes / f / (1024.0 * 1024.0), s.dest_distinct / f,
      s.dest_repeats_in_frame / f, s.dest_repeats_cross_frame / f);
  std::sort(s.buckets, s.buckets + s.bucket_count,
            [](const ResolveDetailsBucket& a, const ResolveDetailsBucket& b) {
              return a.count > b.count;
            });
  for (size_t i = 0; i < s.bucket_count; ++i) {
    const ResolveDetailsBucket& b = s.buckets[i];
    const uint32_t path = uint32_t(b.key & 0b11);
    const bool is_depth = ((b.key >> 2) & 0b1) != 0;
    const uint32_t msaa_log2 = uint32_t((b.key >> 3) & 0b11);
    const uint32_t src_format = uint32_t((b.key >> 5) & 0b1111);
    const uint32_t dest_format = uint32_t((b.key >> 10) & 0b111111);
    const uint32_t width = uint32_t((b.key >> 16) & 0b11111111111) * 8;
    const uint32_t height = uint32_t((b.key >> 27) & 0b11111111111) * 8;
    XELOGI(
        "ResolveDetails:   {:>5.2f}/fr {:>4}x{:<4} {} {}xAA src={} dst={} {} "
        "{:.2f} MB/fr",
        b.count / f, width, height, is_depth ? "depth" : "color",
        1u << msaa_log2,
        is_depth ? xenos::GetDepthRenderTargetFormatName(
                       xenos::DepthRenderTargetFormat(src_format))
                 : xenos::GetColorRenderTargetFormatName(
                       xenos::ColorRenderTargetFormat(src_format)),
        FormatInfo::GetName(dest_format),
        path == 0 ? "direct" : (path == 1 ? "staged" : "dropped"),
        b.bytes / f / (1024.0 * 1024.0));
  }
  s = ResolveDetailsStats();
  s.last_report_ns = now;
}

bool VulkanRenderTargetCache::AcquireExternalRenderTargetAliasLease(
    RenderTarget* render_target) {
  if (!render_target ||
      FindRenderTargetForTrace(render_target->key()) != render_target) {
    return false;
  }
  // External readers (trace, savestate) see the image itself.
  MaterializePendingConstantRows(render_target, 3);
  return static_cast<VulkanRenderTarget*>(render_target)
      ->AcquireExternalAliasLease();
}

void VulkanRenderTargetCache::ReleaseExternalRenderTargetAliasLease(
    RenderTarget* render_target) {
  assert_not_null(render_target);
  if (!render_target) {
    return;
  }
  assert_true(FindRenderTargetForTrace(render_target->key()) == render_target);
  if (FindRenderTargetForTrace(render_target->key()) != render_target) {
    return;
  }
  static_cast<VulkanRenderTarget*>(render_target)->ReleaseExternalAliasLease();
}

bool VulkanRenderTargetCache::SupportsNewOwnerClearTransfers() const {
  // Clearing attachments doesn't contribute the guest draw's occlusion
  // samples. Until accounting is implemented, only the fake-query mode can
  // replace a draw this way (even if a real query happens to be closed).
  return command_processor_.GetZPDMode() == ZPDMode::kFake;
}

bool VulkanRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");
  // Any transfers queued by a previous Update() but never encoded (e.g. the
  // draw was skipped) must be performed before this update reconfigures the
  // render targets, otherwise they'd be lost.
  if (!FlushPendingDrawPassTransfers()) {
    return false;
  }
  // Likewise a deferred clear kept for a pass that was never entered.
  if (deferred_clear_in_pass_) {
    FlushDeferredClear();
  }

  if (!RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    FlushPendingClearTransfers();
    return false;
  }

  if (retirement_tracking_enabled_ &&
      GetPath() == Path::kHostRenderTargets) {
    const uint64_t use_submission =
        command_processor_.GetCurrentSubmission();
    const uint64_t use_frame = command_processor_.GetCurrentFrame();
    auto mark_reference = [use_submission, use_frame](RenderTarget* target) {
      if (target) {
        static_cast<VulkanRenderTarget*>(target)->MarkGpuReference(
            use_submission, use_frame);
      }
    };
    RenderTarget* const* used = last_update_used_render_targets();
    RenderTarget* const* accumulated =
        last_update_accumulated_render_targets();
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      mark_reference(used[i]);
      mark_reference(accumulated[i]);
      for (const Transfer& transfer : last_update_transfers()[i]) {
        mark_reference(transfer.source);
        mark_reference(transfer.host_depth_source);
      }
    }
    mark_reference(last_update_clear_retarget_render_target());
  }

  // rt_cache_draw_clear_chains: per binding slot and aspect, a chain
  // continues while nothing but more exact clears of its render target (drawn,
  // or retargeted clears of it) touch it; anything reading or otherwise
  // writing it ends it before any host command. Common Update emits no
  // host-target commands, so the chains are still intact here.
  if (draw_clear_chain_.mask) {
    constexpr uint32_t kSlots = DrawClearChain::kSlots;
    DrawClearChain& chain = draw_clear_chain_;
    // A draw not confirmed by now wasn't recorded - nothing was written.
    chain.awaiting_mask = 0;
    const bool draw_replaced = last_update_draw_replaced_by_clear();
    RenderTarget* const* used_rts = last_update_used_render_targets();
    RenderTarget* retarget_rt = last_update_clear_retarget_render_target();
    uint32_t retarget_aspects = 0;
    if (retarget_rt) {
      const uint64_t retarget_value = last_update_clear_retarget_value();
      retarget_aspects = (retarget_value & kResolveClearDepthOnlyBit)
                             ? 0b01
                             : ((retarget_value & kResolveClearStencilOnlyBit)
                                    ? 0b10
                                    : 0b11);
    }
    const bool draw_exact = last_update_draw_cutout_exact();
    uint32_t flush_mask = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
      const uint32_t bit = uint32_t(1) << i;
      if (!(chain.mask & bit)) {
        continue;
      }
      RenderTarget* rt = chain.render_targets[i];
      // Read by a transfer of this update - must be complete first.
      bool source = false;
      for (uint32_t j = 0; j < kSlots && !source; ++j) {
        for (const Transfer& transfer : last_update_transfers()[j]) {
          // Its own retained host depth for a range it gets back isn't a
          // read by another render target.
          if (transfer.source == rt ||
              (transfer.host_depth_source == rt &&
               (used_rts[j] != rt || cvars::rt_cache_legacy_clear_paths))) {
            source = true;
            break;
          }
        }
      }
      if (source) {
        flush_mask |= bit;
        continue;
      }
      // The deferred transfers copy from their sources later, so the sources
      // must stay as they are. A draw into a source may write pixels outside
      // the range it claims (the claim comes from row estimates - for
      // unclipped draws, vertices run on the CPU), including ones the chain
      // still has to copy - transfer before anything draws into a source.
      bool source_bound = false;
      for (const Transfer& transfer : chain.transfers[i]) {
        for (uint32_t j = 0; j < kSlots && !source_bound; ++j) {
          // The chain's own render target as the host depth source (its
          // retained host depth for a range it gets back) is written by the
          // chain's clears only inside the cutout the transfers skip.
          if (used_rts[j] &&
              (used_rts[j] == transfer.source ||
               (used_rts[j] == transfer.host_depth_source &&
                (transfer.host_depth_source != rt ||
                 cvars::rt_cache_legacy_clear_paths)))) {
            source_bound = true;
          }
        }
        if (source_bound) {
          break;
        }
      }
      if (source_bound) {
        flush_mask |= bit;
        continue;
      }
      bool bound = false;
      if (!draw_replaced) {
        for (uint32_t j = 0; j < kSlots; ++j) {
          if (used_rts[j] == rt) {
            bound = true;
            break;
          }
        }
      }
      // The aspects this update writes entirely, reading none of them.
      uint32_t written_aspects = 0;
      bool candidate = false;
      if (bound) {
        if (used_rts[i] == rt && draw_exact &&
            last_update_transfers()[i].empty()) {
          written_aspects =
              i ? ((last_update_draw_cutout_mask() >> i) & 1)
                : last_update_draw_cutout_depth_aspects();
        }
        if (!written_aspects) {
          // Drawn with otherwise - the contents may be read.
          flush_mask |= bit;
          continue;
        }
        candidate = true;
      } else if (retarget_rt == rt) {
        // New transfers into the target (pixels it gets back from another
        // owner) land outside the clear; the chain's older transfers would
        // later overwrite them, as in the bound case above.
        if (!last_update_transfers()[0].empty()) {
          flush_mask |= bit;
          continue;
        }
        // The retargeted clear is performed right below in this update.
        written_aspects = retarget_aspects;
      } else {
        // Untouched by this update.
        continue;
      }
      const Transfer::Rectangle& rect =
          candidate ? last_update_draw_cutout()
                    : last_update_clear_retarget_rectangle();
      std::array<Transfer::Rectangle, 2> merged;
      bool merge_ok = true;
      for (uint32_t a = 0; a < 2; ++a) {
        if ((written_aspects & (uint32_t(1) << a)) &&
            !MergeClearRectangle(chain.has_cutout[i][a], chain.cutout[i][a],
                                 rect, merged[a])) {
          merge_ok = false;
        }
      }
      if (!merge_ok) {
        flush_mask |= bit;
        continue;
      }
      for (uint32_t a = 0; a < 2; ++a) {
        if (!(written_aspects & (uint32_t(1) << a))) {
          continue;
        }
        if (candidate) {
          // Kept only if this draw is confirmed to execute.
          chain.candidate[i][a] = merged[a];
          chain.awaiting_mask |= uint32_t(1) << (2 * i + a);
        } else {
          chain.cutout[i][a] = merged[a];
          chain.has_cutout[i][a] = true;
        }
      }
      ++chain.draws[i];
      if (command_processor_.IsRtTraceActive()) {
        XELOGI("RTTRACE draw_chain {} slot={} aspects={} rect=[{},{},{},{}]",
               candidate ? "extend" : "retarget", i, written_aspects,
               rect.x_pixels, rect.y_pixels, rect.width_pixels,
               rect.height_pixels);
      }
    }
    if (flush_mask) {
      FlushDrawClearChainSlots(flush_mask);
    }
  }

  // A deferred clear waits only for a plain draw on its render target.
  if (deferred_clear_render_target_ && !CanKeepDeferredClear()) {
    FlushDeferredClear();
  }

  // Common Update only plans ownership and allocates targets. It emits no
  // host-target commands; materialize pending pixels before any new commands,
  // unless this update proves another clear of the same target. Only an exact
  // rectangular union may be excluded, never the bounding box of an L shape.
  if (pending_clear_render_target_) {
    Transfer::Rectangle merged;
    if (SupportsNewOwnerClearTransfers() &&
        last_update_draw_replaced_by_clear() &&
        last_update_clear_retarget_render_target() == pending_clear_render_target_ &&
        last_update_transfers()[0].empty() &&
        // The flush excludes the cutout from both aspects' transfers, so only
        // a clear of both may grow it.
        !(last_update_clear_retarget_value() &
          (kResolveClearDepthOnlyBit | kResolveClearStencilOnlyBit)) &&
        TryMergeClearRectangles(pending_clear_cutout_,
                                last_update_clear_retarget_rectangle(), merged)) {
      pending_clear_cutout_ = merged;
      if (command_processor_.IsRtTraceActive()) {
        XELOGI("RTTRACE clear_chain merged=[{},{},{},{}]",
               merged.x_pixels, merged.y_pixels,
               merged.width_pixels, merged.height_pixels);
      }
    } else {
      FlushPendingClearTransfers();
    }
  }

  // The draw is a depth clear performed as a clear of the render target
  // holding the range (rt_cache_retarget_full_depth_clears) - the caller skips
  // the draw itself.
  if (RenderTarget* clear_retarget_render_target =
          last_update_clear_retarget_render_target()) {
    uint64_t clear_retarget_value = last_update_clear_retarget_value();
    const auto& cutout = last_update_clear_retarget_rectangle();
    const auto key = clear_retarget_render_target->key();
    clear_transfers_[0].clear();
    if (cvars::vulkan_defer_new_depth_owner_transfers &&
        last_update_draw_replaced_by_clear() &&
        !last_update_transfers()[0].empty() &&
        // The deferred transfers later skip the cutout in both aspects.
        !(clear_retarget_value &
          (kResolveClearDepthOnlyBit | kResolveClearStencilOnlyBit))) {
      assert_null(pending_clear_render_target_);
      pending_clear_render_target_ = clear_retarget_render_target;
      pending_clear_cutout_ = cutout;
      pending_clear_transfers_ = last_update_transfers()[0];
      if (command_processor_.IsRtTraceActive()) {
        XELOGI("RTTRACE clear_chain defer sources={}", pending_clear_transfers_.size());
      }
    }
    // Completely covered sources must not reach transfer invocations or host
    // depth stores, which expect at least one surviving rectangle.
    for (const Transfer& transfer : last_update_transfers()[0]) {
      if (pending_clear_render_target_) {
        break;
      }
      if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                 key.msaa_samples, false, nullptr, &cutout)) {
        clear_transfers_[0].push_back(transfer);
      }
    }
    if (command_processor_.IsRtTraceActive() && !last_update_transfers()[0].empty()) {
      XELOGI("RTTRACE clear_new_owner kept_sources={} original_sources={}",
             clear_transfers_[0].size(), last_update_transfers()[0].size());
    }
    // A clear of one aspect leaves the other one inside the rectangle to the
    // old owners, but the transfers skip the cutout in both aspects (the
    // stencil bit draws use the same cutout as the depth). Transfer them whole
    // first, then clear.
    if ((clear_retarget_value &
         (kResolveClearDepthOnlyBit | kResolveClearStencilOnlyBit)) &&
        !pending_clear_render_target_ && !last_update_transfers()[0].empty()) {
      PerformTransfersAndResolveClears(1, &clear_retarget_render_target,
                                       &last_update_transfers()[0]);
      clear_transfers_[0].clear();
    }
    if (cvars::rt_cache_defer_retarget_clears && !use_dynamic_rendering_ &&
        key.is_depth && last_update_draw_replaced_by_clear() &&
        !command_processor_.deferred_resolve_pending_) {
      if (deferred_clear_render_target_ == clear_retarget_render_target) {
        // Kept by CanKeepDeferredClear: same value, exactly mergeable.
        Transfer::Rectangle merged;
        if (TryMergeClearRectangles(deferred_clear_rectangle_, cutout,
                                    merged)) {
          deferred_clear_rectangle_ = merged;
          // Sources entirely inside the grown cutout no longer transfer.
          std::vector<Transfer> kept;
          for (const Transfer& transfer : deferred_clear_transfers_) {
            if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                       key.msaa_samples, false, nullptr,
                                       &merged)) {
              kept.push_back(transfer);
            }
          }
          deferred_clear_transfers_ = std::move(kept);
          deferred_clear_in_pass_ = false;
          return true;
        }
        FlushDeferredClear();
      }
      // CanKeepDeferredClear flushed any other earlier one at the top of Update.
      assert_null(deferred_clear_render_target_);
      deferred_clear_render_target_ = clear_retarget_render_target;
      deferred_clear_value_ = clear_retarget_value;
      deferred_clear_rectangle_ = last_update_clear_retarget_rectangle();
      deferred_clear_transfers_ = clear_transfers_[0];
      // The transfers vulkan_defer_new_depth_owner_transfers kept for exactly
      // this rectangle go with the clear (same filter as their flush).
      if (pending_clear_render_target_ == clear_retarget_render_target &&
          pending_clear_cutout_.x_pixels == cutout.x_pixels &&
          pending_clear_cutout_.y_pixels == cutout.y_pixels &&
          pending_clear_cutout_.width_pixels == cutout.width_pixels &&
          pending_clear_cutout_.height_pixels == cutout.height_pixels) {
        for (const Transfer& transfer : pending_clear_transfers_) {
          if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                     key.msaa_samples, false, nullptr,
                                     &cutout)) {
            deferred_clear_transfers_.push_back(transfer);
          }
        }
        pending_clear_render_target_ = nullptr;
        pending_clear_transfers_.clear();
      }
      deferred_clear_in_pass_ = false;
      return true;
    }
    PerformTransfersAndResolveClears(1, &clear_retarget_render_target,
                                     clear_transfers_, &clear_retarget_value,
                                     &last_update_clear_retarget_rectangle());
    if (last_update_draw_replaced_by_clear()) {
      return true;
    }
    // Only depth / stencil was cleared this way - continue setting up the
    // render targets for the color part of the draw.
  }

  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed even with the fragment shader interlock render backend for passing
  // the sample count to the pipeline cache.
  render_pass_key.msaa_samples = rb_surface_info.msaa_samples;

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format =
            depth_and_color_render_targets[0]->key().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->key().GetColorFormat();
      }

      // Determine whether there's any transfer work at all (the common
      // steady-state case has none, in which case nothing below runs).
      const std::vector<Transfer>* update_transfers = last_update_transfers();
      bool any_transfers = false;
      if (cvars::rt_cache_ownership_claim_memo) {
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          if (!update_transfers[i].empty()) {
            any_transfers = true;
            break;
          }
        }
      } else {
        any_transfers = true;
      }
      if (!any_transfers && cvars::vulkan_in_pass_resolve_r32_uint &&
          cvars::vulkan_in_pass_resolve &&
          !cvars::vulkan_in_pass_resolve_debug_exp_bias &&
          !cvars::vulkan_in_pass_resolve_debug_reject &&
          local_read_attachments_ && r32_uint_input_attachment_supported_ &&
          render_pass_key.msaa_samples == xenos::MsaaSamples::k1X &&
          !last_update_draw_replaced_by_clear() &&
          !last_update_clear_retarget_render_target() &&
          !HasPendingDrawPassTransfers()) {
        uint32_t r32_uint_slot = UINT32_MAX;
        bool no_blending = true;
        for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
          const uint32_t attachment_bit = uint32_t(1) << (1 + i);
          if (!(render_pass_key.depth_and_color_used & attachment_bit)) {
            continue;
          }
          const xenos::ColorRenderTargetFormat format =
              i == 0   ? render_pass_key.color_0_view_format
              : i == 1 ? render_pass_key.color_1_view_format
              : i == 2 ? render_pass_key.color_2_view_format
                       : render_pass_key.color_3_view_format;
          const reg::RB_BLENDCONTROL blend_control =
              register_file().Get<reg::RB_BLENDCONTROL>(
                  reg::RB_BLENDCONTROL::rt_register_indices[i]);
          no_blending &=
              blend_control.color_srcblend == xenos::BlendFactor::kOne &&
              blend_control.color_destblend == xenos::BlendFactor::kZero &&
              blend_control.color_comb_fcn == xenos::BlendOp::kAdd &&
              blend_control.alpha_srcblend == xenos::BlendFactor::kOne &&
              blend_control.alpha_destblend == xenos::BlendFactor::kZero &&
              blend_control.alpha_comb_fcn == xenos::BlendOp::kAdd;
          if (format == xenos::ColorRenderTargetFormat::k_32_FLOAT) {
            const uint32_t write_mask =
                (normalized_color_mask >> (i * 4)) & 0b1111;
            if (r32_uint_slot != UINT32_MAX || write_mask != 0b0001) {
              r32_uint_slot = UINT32_MAX;
              no_blending = false;
              break;
            }
            r32_uint_slot = i;
          }
        }
        if (no_blending && r32_uint_slot != UINT32_MAX) {
          const auto* r32_uint_rt = static_cast<const VulkanRenderTarget*>(
              depth_and_color_render_targets[1 + r32_uint_slot]);
          if (r32_uint_rt &&
              r32_uint_rt->HasDescriptorSetInputAttachment(true)) {
            render_pass_key.color_rts_view_mode =
                kColorRTViewModeR32UintFirst + r32_uint_slot;
          }
        }
      }
      // Clear chains this draw starts (their sources get readable below).
      uint32_t clear_chains_started = 0;
      if (any_transfers) {
        if (RenderTargetShortcut(cvars::vulkan_in_pass_transfers)) {
          // Try to queue each render target's transfers for in-pass execution;
          // transfers that can't be merged into the active pass fall back to
          // the immediate (pass-breaking) path.
          std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
              fallback_transfers;
          bool fallback_transfer_work = false;
          // rt_cache_draw_overwrite_cutouts - the draw's rectangle, skipped by
          // the transfers if the draw executes (decided once its pipeline is
          // known, see EncodePendingDrawPassTransfers and
          // PerformPendingOutOfPassTransfers).
          const uint32_t draw_cutout_mask = last_update_draw_cutout_mask();
          if (draw_cutout_mask) {
            pending_draw_cutout_ = last_update_draw_cutout();
          }
          // rt_cache_draw_clear_chains: the transfers into render targets this
          // clear overwrites exactly are deferred until the clears end.
          const bool start_clear_chain = cvars::rt_cache_draw_clear_chains &&
                                         last_update_draw_cutout_exact();
          uint32_t clear_chain_mask = 0;
          for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
            const std::vector<Transfer>& transfers = update_transfers[i];
            if (transfers.empty()) {
              continue;
            }
            const bool draw_cutout = (draw_cutout_mask >> i) & 1;
            // The aspects of this render target the draw writes exactly.
            const uint32_t chain_aspects =
                i ? uint32_t(draw_cutout)
                  : last_update_draw_cutout_depth_aspects();
            // The deferred transfers copy their sources later - not when this
            // draw writes one of them.
            bool source_bound = false;
            for (const Transfer& transfer : transfers) {
              for (uint32_t j = 0; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
                RenderTarget* bound_rt = depth_and_color_render_targets[j];
                if (bound_rt &&
                    (bound_rt == transfer.source ||
                     (bound_rt == transfer.host_depth_source &&
                      (bound_rt != depth_and_color_render_targets[i] ||
                       cvars::rt_cache_legacy_clear_paths)))) {
                  source_bound = true;
                }
              }
            }
            if (command_processor_.IsRtTraceActive()) {
              XELOGI(
                  "RTTRACE draw_chain consider slot={} aspects={} exact={} "
                  "source_bound={} active={}",
                  i, chain_aspects, uint32_t(start_clear_chain),
                  uint32_t(source_bound),
                  (draw_clear_chain_.mask >> i) & 1);
            }
            if (chain_aspects && start_clear_chain && !source_bound &&
                !(draw_clear_chain_.mask & (uint32_t(1) << i))) {
              draw_clear_chain_.has_cutout[i] = {false, false};
              for (uint32_t a = 0; a < 2; ++a) {
                if (chain_aspects & (uint32_t(1) << a)) {
                  draw_clear_chain_.candidate[i][a] = last_update_draw_cutout();
                  draw_clear_chain_.awaiting_mask |= uint32_t(1) << (2 * i + a);
                }
              }
              draw_clear_chain_.draws[i] = 1;
              draw_clear_chain_.render_targets[i] =
                  depth_and_color_render_targets[i];
              draw_clear_chain_.transfers[i] = transfers;
              clear_chain_mask |= uint32_t(1) << i;
              if (command_processor_.IsRtTraceActive()) {
                LogRtTraceTransfers(i, depth_and_color_render_targets[i],
                                    transfers, false, false);
              }
              continue;
            }
            if (draw_cutout &&
                draw_cutout_logged_ <
                    uint32_t(std::max(0, cvars::log_draw_overwrite_cutouts))) {
              ++draw_cutout_logged_;
              RenderTargetKey cutout_key =
                  depth_and_color_render_targets[i]->key();
              XELOGI(
                  "RTCLEAR: draw cutout [{},{},{},{}] for {} rt at {} (pitch "
                  "{}, {}x MSAA), {} sources",
                  pending_draw_cutout_.x_pixels, pending_draw_cutout_.y_pixels,
                  pending_draw_cutout_.width_pixels,
                  pending_draw_cutout_.height_pixels,
                  cutout_key.is_depth ? "depth" : "color",
                  cutout_key.base_tiles, cutout_key.GetPitchTiles(),
                  uint32_t(1) << uint32_t(cutout_key.msaa_samples),
                  transfers.size());
            }
            if (CanQueueDrawPassTransfers(i, depth_and_color_render_targets,
                                          transfers)) {
              pending_draw_pass_render_targets_[i] =
                  depth_and_color_render_targets[i];
              pending_draw_pass_transfers_[i] = transfers;
              pending_draw_pass_transfer_mask_ |= uint32_t(1) << i;
              if (draw_cutout) {
                pending_draw_cutout_mask_ |= uint32_t(1) << i;
              }
              bool rt_trace_full_overwrite =
                  PendingDrawPassTransfersFullyOverwriteTarget(
                      i, depth_and_color_render_targets[i], transfers);
              if (rt_trace_full_overwrite) {
                pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
              }
              if (command_processor_.IsRtTraceActive()) {
                LogRtTraceTransfers(i, depth_and_color_render_targets[i],
                                    transfers, true, rt_trace_full_overwrite);
              }
            } else {
              if (command_processor_.IsRtTraceActive()) {
                LogRtTraceTransfers(i, depth_and_color_render_targets[i],
                                    transfers, false, false);
              }
              if (draw_cutout) {
                if (cvars::log_in_pass_transfer_rejects > 0) {
                  ++in_pass_transfer_verdicts_[17];
                }
                // Outside the guest pass, but only once the draw is known to
                // execute - before entering the pass.
                pending_out_of_pass_render_targets_[i] =
                    depth_and_color_render_targets[i];
                pending_out_of_pass_transfers_[i] = transfers;
                pending_out_of_pass_mask_ |= uint32_t(1) << i;
              } else {
                fallback_transfers[i] = transfers;
                fallback_transfer_work = true;
              }
            }
          }
          if (clear_chain_mask) {
            clear_chains_started = clear_chain_mask;
            draw_clear_chain_.mask |= clear_chain_mask;
            if (command_processor_.IsRtTraceActive()) {
              const Transfer::Rectangle& r = last_update_draw_cutout();
              XELOGI("RTTRACE draw_chain start mask=0x{:X} rect=[{},{},{},{}]",
                     clear_chain_mask, r.x_pixels, r.y_pixels, r.width_pixels,
                     r.height_pixels);
            }
          }
          if (fallback_transfer_work) {
            PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                             depth_and_color_render_targets,
                                             fallback_transfers.data());
          }
          // Only discard prior contents of targets that are both fully
          // overwritten by the queued transfers AND used by this render pass.
          render_pass_key.depth_and_color_load_dont_care =
              pending_draw_pass_full_overwrite_mask_ &
              render_pass_key.depth_and_color_used;
          // If the queued transfers can't actually build their pipelines for
          // this render pass, fall back to performing them immediately and
          // don't discard any contents.
          if (HasPendingDrawPassTransfers() &&
              !PreflightPendingDrawPassTransfers(render_pass_key)) {
            if (command_processor_.IsRtTraceActive()) {
              XELOGI("RTTRACE xfer_preflight_fallback mask=0x{:X}",
                     pending_draw_pass_transfer_mask_);
            }
            if (cvars::log_in_pass_transfer_rejects > 0) {
              ++in_pass_transfer_verdicts_[16];
            }
            if (!FlushPendingDrawPassTransfers()) {
              return false;
            }
            render_pass_key.depth_and_color_load_dont_care = 0;
          }
        } else {
          if (command_processor_.IsRtTraceActive()) {
            for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
              LogRtTraceTransfers(i, depth_and_color_render_targets[i],
                                 update_transfers[i], false, false);
            }
          }
          PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                           depth_and_color_render_targets,
                                           update_transfers);
        }
      }

      // Clears covering the whole host image don't need the previous
      // contents.
      render_pass_key.depth_and_color_load_dont_care |=
          last_update_full_host_overwrite() &
          render_pass_key.depth_and_color_used;

      const Framebuffer* framebuffer = last_update_framebuffer_;
      VkRenderPass render_pass = last_update_render_pass_key_ == render_pass_key
                                     ? last_update_render_pass_
                                     : VK_NULL_HANDLE;
      if (render_pass == VK_NULL_HANDLE) {
        render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
        if (render_pass == VK_NULL_HANDLE) {
          return false;
        }
        // Framebuffer for a different render pass needed now.
        framebuffer = nullptr;
      }

      uint32_t pitch_tiles_at_32bpp =
          ((rb_surface_info.surface_pitch << uint32_t(
                rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
           (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ !=
                pitch_tiles_at_32bpp ||
            std::memcmp(last_update_framebuffer_attachments_,
                        depth_and_color_render_targets,
                        sizeof(last_update_framebuffer_attachments_))) {
          framebuffer = nullptr;
        }
      }
      if (!framebuffer) {
        framebuffer = GetHostRenderTargetsFramebuffer(
            render_pass_key, pitch_tiles_at_32bpp,
            depth_and_color_render_targets);
        if (!framebuffer) {
          return false;
        }
      }

      // Successful update - write the new configuration.
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::memcpy(last_update_framebuffer_attachments_,
                  depth_and_color_render_targets,
                  sizeof(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      // Transition the used render targets.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        if (!rt) {
          continue;
        }
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rt);
        VkPipelineStageFlags rt_dst_stage_mask;
        VkAccessFlags rt_dst_access_mask;
        VkImageLayout rt_new_layout;
        vulkan_rt.GetDrawUsage(&rt_dst_stage_mask, &rt_dst_access_mask,
                               &rt_new_layout);
        command_processor_.PushImageMemoryBarrier(
            vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.current_stage_mask(), rt_dst_stage_mask,
            vulkan_rt.current_access_mask(), rt_dst_access_mask,
            vulkan_rt.current_layout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask,
                           rt_new_layout);
      }

      // Move the queued transfer sources to the shader-read layout now, while
      // the render pass is not yet active, so they can be sampled in-pass.
      PreparePendingDrawPassTransferBarriers();
      // The deferred clear survived CanKeepDeferredClear, so this draw uses
      // its render target as the depth attachment: run it in this pass if the
      // transfers around it can, with sources made readable before the pass.
      if (deferred_clear_render_target_) {
        // vkCmdClearAttachments must stay inside the pass's render area.
        const RenderTargetKey clear_key = deferred_clear_render_target_->key();
        const uint64_t clear_right =
            uint64_t(deferred_clear_rectangle_.x_pixels +
                     deferred_clear_rectangle_.width_pixels) *
            GetKeyScaleX(clear_key);
        const uint64_t clear_bottom =
            uint64_t(deferred_clear_rectangle_.y_pixels +
                     deferred_clear_rectangle_.height_pixels) *
            GetKeyScaleY(clear_key);
        deferred_clear_in_pass_ =
            depth_and_color_render_targets[0] == deferred_clear_render_target_ &&
            clear_right <= framebuffer->host_extent.width &&
            clear_bottom <= framebuffer->host_extent.height &&
            (deferred_clear_transfers_.empty() ||
             (CanRunTransfersInGuestPass(
                  0, deferred_clear_render_target_, deferred_clear_transfers_,
                  [depth_and_color_render_targets](const RenderTarget* rt) {
                    return std::find(depth_and_color_render_targets,
                                     depth_and_color_render_targets +
                                         1 + xenos::kMaxColorRenderTargets,
                                     rt) != depth_and_color_render_targets +
                                                1 + xenos::kMaxColorRenderTargets;
                  }) &&
              PreflightTransferPipelines(render_pass_key, 0,
                                         deferred_clear_render_target_,
                                         deferred_clear_transfers_)));
        if (!deferred_clear_in_pass_) {
          FlushDeferredClear();
        } else if (!deferred_clear_transfers_.empty()) {
          PrepareTransferSourceBarriers(deferred_clear_transfers_,
                                        depth_and_color_render_targets);
        }
      }
      // rt_cache_clear_chain_in_pass: no barrier can be recorded inside the
      // pass when the chain ends, so make its sources readable now - only if
      // this draw begins a new pass anyway, never breaking an open one.
      if (clear_chains_started && cvars::rt_cache_clear_chain_in_pass &&
          (!command_processor_.in_render_pass() ||
           command_processor_.current_framebuffer_ != framebuffer ||
           command_processor_.current_render_pass_ != render_pass)) {
        PrepareClearChainSourceBarriers(clear_chains_started,
                                        depth_and_color_render_targets);
      }
    } break;

    case Path::kPixelShaderInterlock: {
      // For FSI, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for FSI drawing now.
      UseEdramBuffer(EdramBufferUsage::kFragmentReadWrite);
      // Commit preceding unordered (but not FSI) writes like clears as they
      // aren't synchronized with FSI accesses.
      CommitEdramBufferShaderWrites(
          EdramBufferModificationStatus::kViaUnordered);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      assert_unhandled_case(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::LogRtTraceTransfers(
    uint32_t slot, RenderTarget* dest, const std::vector<Transfer>& transfers,
    bool in_pass, bool full_overwrite) {
  if (transfers.empty()) {
    return;
  }
  std::string dest_str = FormatRtTraceKey(dest, true);
  for (const Transfer& transfer : transfers) {
    XELOGI(
        "RTTRACE xfer slot={} dest={} src={} tiles=[{},{}) path={} "
        "full_overwrite={}",
        slot, dest_str, FormatRtTraceKey(transfer.source, true),
        transfer.start_tiles, transfer.end_tiles,
        in_pass ? "inpass" : "immediate", uint32_t(full_overwrite));
  }
}

void VulkanRenderTargetCache::DropLastUpdateLoadDontCare(uint32_t mask) {
  RenderPassKey key = last_update_render_pass_key_;
  key.depth_and_color_load_dont_care &= ~mask;
  if (key == last_update_render_pass_key_) {
    return;
  }
  if (!use_dynamic_rendering_ && GetPath() == Path::kHostRenderTargets) {
    VkRenderPass render_pass = GetHostRenderTargetsRenderPass(key);
    if (render_pass == VK_NULL_HANDLE) {
      return;
    }
    last_update_render_pass_ = render_pass;
  }
  last_update_render_pass_key_ = key;
}

void VulkanRenderTargetCache::ClearPendingDrawPassTransfers() {
  // Materialized transfers no longer justify discarding on a later begin.
  if (cvars::vulkan_consume_dontcare) {
    DropLastUpdateLoadDontCare(pending_draw_pass_full_overwrite_mask_);
  }
  for (auto& transfers : pending_draw_pass_transfers_) {
    transfers.clear();
  }
  pending_draw_pass_render_targets_.fill(nullptr);
  pending_draw_pass_transfer_mask_ = 0;
  pending_draw_pass_full_overwrite_mask_ = 0;
  pending_draw_cutout_mask_ = 0;
}

void VulkanRenderTargetCache::PerformTransfersWithCutout(
    RenderTarget* const* render_targets,
    const std::vector<Transfer>* transfers, uint32_t cutout_mask,
    const Transfer::Rectangle& cutout, bool in_current_render_pass) {
  constexpr uint32_t kCount = 1 + xenos::kMaxColorRenderTargets;
  if (!cutout_mask) {
    PerformTransfersAndResolveClears(kCount, render_targets, transfers,
                                     nullptr, nullptr, in_current_render_pass);
    return;
  }
  std::array<RenderTarget*, kCount> whole_rts = {};
  std::array<RenderTarget*, kCount> cut_rts = {};
  std::array<std::vector<Transfer>, kCount> cut_transfers;
  bool any_whole = false, any_cut = false;
  for (uint32_t i = 0; i < kCount; ++i) {
    RenderTarget* rt = render_targets[i];
    if (!rt || transfers[i].empty()) {
      continue;
    }
    if (!(cutout_mask & (uint32_t(1) << i))) {
      whole_rts[i] = rt;
      any_whole = true;
      continue;
    }
    // Sources entirely inside the cutout must not reach the transfer
    // invocations or host depth stores, which expect at least one rectangle.
    RenderTargetKey key = rt->key();
    for (const Transfer& transfer : transfers[i]) {
      if (transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                 key.msaa_samples, key.Is64bpp(), nullptr,
                                 &cutout)) {
        cut_transfers[i].push_back(transfer);
      }
    }
    if (!cut_transfers[i].empty()) {
      cut_rts[i] = rt;
      any_cut = true;
    }
  }
  if (any_whole) {
    PerformTransfersAndResolveClears(kCount, whole_rts.data(), transfers,
                                     nullptr, nullptr, in_current_render_pass);
  }
  if (command_processor_.IsRtTraceActive()) {
    // Samples moved by the cut render targets' transfers, with and without
    // the cutout (times 9 passes for depth without stencil export).
    uint64_t kept_samples = 0, original_samples = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
      if (!(cutout_mask & (uint32_t(1) << i)) || !render_targets[i]) {
        continue;
      }
      RenderTargetKey key = render_targets[i]->key();
      uint32_t samples = uint32_t(1) << uint32_t(key.msaa_samples);
      for (const Transfer& transfer : transfers[i]) {
        Transfer::Rectangle rects[Transfer::kMaxRectanglesWithCutout];
        uint32_t n = transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                            key.msaa_samples, key.Is64bpp(),
                                            rects, &cutout);
        for (uint32_t j = 0; j < n; ++j) {
          kept_samples +=
              uint64_t(rects[j].width_pixels) * rects[j].height_pixels * samples;
        }
        n = transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                   key.msaa_samples, key.Is64bpp(), rects);
        for (uint32_t j = 0; j < n; ++j) {
          original_samples +=
              uint64_t(rects[j].width_pixels) * rects[j].height_pixels * samples;
        }
      }
    }
    XELOGI(
        "RTTRACE cutout mask=0x{:X} rect=[{},{},{},{}] inpass={} samples={}/{}",
        cutout_mask, cutout.x_pixels, cutout.y_pixels, cutout.width_pixels,
        cutout.height_pixels, uint32_t(in_current_render_pass), kept_samples,
        original_samples);
  }
  if (any_cut) {
    PerformTransfersAndResolveClears(kCount, cut_rts.data(),
                                     cut_transfers.data(), nullptr, &cutout,
                                     in_current_render_pass);
  }
}

bool VulkanRenderTargetCache::MergeClearRectangle(
    bool has_a, const Transfer::Rectangle& a, const Transfer::Rectangle& b,
    Transfer::Rectangle& merged) {
  if (!has_a) {
    merged = b;
    return true;
  }
  if (b.x_pixels >= a.x_pixels && b.y_pixels >= a.y_pixels &&
      b.x_pixels + b.width_pixels <= a.x_pixels + a.width_pixels &&
      b.y_pixels + b.height_pixels <= a.y_pixels + a.height_pixels) {
    merged = a;
    return true;
  }
  if (a.x_pixels >= b.x_pixels && a.y_pixels >= b.y_pixels &&
      a.x_pixels + a.width_pixels <= b.x_pixels + b.width_pixels &&
      a.y_pixels + a.height_pixels <= b.y_pixels + b.height_pixels) {
    merged = b;
    return true;
  }
  return TryMergeClearRectangles(a, b, merged);
}

void VulkanRenderTargetCache::ConfirmDrawClearChain(bool draw_will_execute) {
  DrawClearChain& chain = draw_clear_chain_;
  if (!chain.awaiting_mask) {
    return;
  }
  if (draw_will_execute) {
    for (uint32_t i = 0; i < DrawClearChain::kSlots; ++i) {
      for (uint32_t a = 0; a < 2; ++a) {
        if (chain.awaiting_mask & (uint32_t(1) << (2 * i + a))) {
          chain.cutout[i][a] = chain.candidate[i][a];
          chain.has_cutout[i][a] = true;
        }
      }
    }
  }
  // Not executed: the rectangle isn't written, the chains go on without it.
  chain.awaiting_mask = 0;
}

void VulkanRenderTargetCache::FlushDrawClearChain() {
  FlushDrawClearChainSlots(draw_clear_chain_.mask);
}

void VulkanRenderTargetCache::FlushDrawClearChainSlots(uint32_t slot_mask) {
  constexpr uint32_t kSlots = DrawClearChain::kSlots;
  slot_mask &= draw_clear_chain_.mask;
  if (!slot_mask) {
    return;
  }
  // Take the slots out before encoding: command-buffer helpers may end a
  // submission, which flushes the remaining chains.
  std::array<RenderTarget*, kSlots> rts = {};
  std::array<std::vector<Transfer>, kSlots> transfers;
  std::array<std::array<bool, 2>, kSlots> has_cutout = {};
  std::array<std::array<Transfer::Rectangle, 2>, kSlots> cutouts = {};
  std::array<uint32_t, kSlots> draws = {};
  DrawClearChain& chain = draw_clear_chain_;
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    if (!(slot_mask & bit)) {
      continue;
    }
    rts[i] = chain.render_targets[i];
    transfers[i] = std::move(chain.transfers[i]);
    has_cutout[i] = chain.has_cutout[i];
    cutouts[i] = chain.cutout[i];
    draws[i] = chain.draws[i];
    chain.render_targets[i] = nullptr;
    chain.transfers[i].clear();
    chain.has_cutout[i] = {false, false};
    chain.draws[i] = 0;
    chain.mask &= ~bit;
    chain.awaiting_mask &= ~(uint32_t(0b11) << (2 * i));
  }
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (!rts[i]) {
      continue;
    }
    const Transfer::Rectangle* main_cutout =
        has_cutout[i][0] ? &cutouts[i][0] : nullptr;
    const Transfer::Rectangle* stencil_cutout =
        has_cutout[i][1] ? &cutouts[i][1] : nullptr;
    RenderTargetKey key = rts[i]->key();
    // Sources entirely inside the cutouts of every aspect are dropped - the
    // transfer invocations and host depth stores expect some rectangle.
    std::array<std::vector<Transfer>, kSlots> slot_transfers;
    uint64_t kept_samples = 0, original_samples = 0;
    const uint32_t samples = uint32_t(1) << uint32_t(key.msaa_samples);
    for (const Transfer& transfer : transfers[i]) {
      Transfer::Rectangle rects[Transfer::kMaxRectanglesWithCutout];
      uint32_t main_count =
          transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                 key.msaa_samples, key.Is64bpp(), rects,
                                 main_cutout);
      uint32_t stencil_count = 0;
      if (key.is_depth) {
        stencil_count = transfer.GetRectangles(
            key.base_tiles, key.GetPitchTiles(), key.msaa_samples, false,
            nullptr, stencil_cutout);
      }
      if (command_processor_.IsRtTraceActive()) {
        for (uint32_t j = 0; j < main_count; ++j) {
          kept_samples +=
              uint64_t(rects[j].width_pixels) * rects[j].height_pixels * samples;
        }
        uint32_t n = transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(),
                                            key.msaa_samples, key.Is64bpp(),
                                            rects);
        for (uint32_t j = 0; j < n; ++j) {
          original_samples +=
              uint64_t(rects[j].width_pixels) * rects[j].height_pixels * samples;
        }
      }
      if (main_count || stencil_count) {
        slot_transfers[i].push_back(transfer);
      }
    }
    if (command_processor_.IsRtTraceActive()) {
      XELOGI(
          "RTTRACE draw_chain flush slot={} draws={} cutout={} [{},{},{},{}] "
          "stencil_cutout={} [{},{},{},{}] main_samples={}/{}",
          i, draws[i], uint32_t(has_cutout[i][0]), cutouts[i][0].x_pixels,
          cutouts[i][0].y_pixels, cutouts[i][0].width_pixels,
          cutouts[i][0].height_pixels, uint32_t(has_cutout[i][1]),
          cutouts[i][1].x_pixels, cutouts[i][1].y_pixels,
          cutouts[i][1].width_pixels, cutouts[i][1].height_pixels,
          kept_samples, original_samples);
    }
    if (slot_transfers[i].empty()) {
      continue;
    }
    std::array<RenderTarget*, kSlots> slot_rts = {};
    slot_rts[i] = rts[i];
    // The transfers only write outside the cleared rectangles, so they may
    // follow the clears inside the same pass.
    const bool in_pass =
        CanFlushClearChainInCurrentPass(i, rts[i], slot_transfers[i]);
    if (in_pass && cvars::log_in_pass_transfer_rejects > 0) {
      ++in_pass_transfer_verdicts_[19];
    }
    PerformTransfersAndResolveClears(kSlots, slot_rts.data(),
                                     slot_transfers.data(), nullptr,
                                     main_cutout, in_pass, key.is_depth,
                                     stencil_cutout);
  }
}

void VulkanRenderTargetCache::PerformPendingOutOfPassTransfers(
    bool draw_will_execute) {
  if (!pending_out_of_pass_mask_) {
    return;
  }
  PerformTransfersWithCutout(
      pending_out_of_pass_render_targets_.data(),
      pending_out_of_pass_transfers_.data(),
      draw_will_execute ? pending_out_of_pass_mask_ : 0, pending_draw_cutout_,
      false);
  for (auto& transfers : pending_out_of_pass_transfers_) {
    transfers.clear();
  }
  pending_out_of_pass_render_targets_.fill(nullptr);
  pending_out_of_pass_mask_ = 0;
}

bool VulkanRenderTargetCache::BuildTransferRectanglePlans(
    RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
    std::vector<TransferRectanglePlan>& transfer_rectangles_out) const {
  transfer_rectangles_out.clear();
  transfer_rectangles_out.reserve(transfers.size());
  for (const Transfer& transfer : transfers) {
    TransferRectanglePlan plan;
    plan.rectangle_count = transfer.GetRectangles(
        dest_key.base_tiles, dest_key.GetPitchTiles(), dest_key.msaa_samples,
        dest_key.Is64bpp(), plan.rectangles.data(), nullptr);
    if (!plan.rectangle_count) {
      transfer_rectangles_out.clear();
      return false;
    }
    transfer_rectangles_out.push_back(plan);
  }
  return true;
}

bool VulkanRenderTargetCache::CanQueueDrawPassTransfers(
    uint32_t render_target_index, RenderTarget* const* render_targets,
    const std::vector<Transfer>& transfers) const {
  if (!render_targets || render_target_index > xenos::kMaxColorRenderTargets) {
    return CanRunTransfersInGuestPass(render_target_index, nullptr, transfers,
                                      [](const RenderTarget*) { return false; });
  }
  // A source can't be one of the render pass's own attachments (it would be
  // both read and written in the same pass).
  return CanRunTransfersInGuestPass(
      render_target_index, render_targets[render_target_index], transfers,
      [render_targets](const RenderTarget* rt) {
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          if (render_targets[i] == rt) {
            return true;
          }
        }
        return false;
      });
}

bool VulkanRenderTargetCache::CanRunTransfersInGuestPass(
    uint32_t render_target_index, RenderTarget* dest_rt,
    const std::vector<Transfer>& transfers,
    const std::function<bool(const RenderTarget*)>& is_attachment) const {
  // Reasons as numbered below; 16 = pipeline preflight failed, 17 = draw
  // cutout (transfers just before the guest pass), 18 = clear chain source
  // not readable inside a pass, 19 = clear chain flushed inside the pass,
  // 14 = a source with pending constant rows.
  auto Verdict = [this](uint32_t reason) {
    if (cvars::log_in_pass_transfer_rejects > 0) {
      ++in_pass_transfer_verdicts_[reason];
    }
    return !reason;
  };
  if (transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return Verdict(1);
  }
  auto* dest_vulkan_rt = static_cast<VulkanRenderTarget*>(dest_rt);
  if (!dest_vulkan_rt) {
    return Verdict(2);
  }
  RenderTargetKey dest_key = dest_vulkan_rt->key();
  if (dest_key.is_depth != (render_target_index == 0)) {
    return Verdict(3);
  }

  if (dest_key.is_depth) {
    if (!dest_vulkan_rt->view_depth_stencil()) {
      return Verdict(4);
    }
  } else {
    // The color target's draw-view format must be usable for the transfer too
    // (no integer reinterpretation, no separate transfer view) - otherwise the
    // guest pass's framebuffer can't be reused for the transfer.
    bool dest_is_integer = false;
    VkFormat transfer_format = GetColorOwnershipTransferVulkanFormat(
        dest_key.GetColorFormat(), &dest_is_integer);
    if (dest_is_integer ||
        transfer_format != GetColorVulkanFormat(dest_key.GetColorFormat()) ||
        dest_vulkan_rt->view_color_transfer() !=
            dest_vulkan_rt->view_depth_color()) {
      return Verdict(5);
    }
  }

  for (const Transfer& transfer : transfers) {
    if (!transfer.source) {
      return Verdict(6);
    }
    if (transfer.source == dest_vulkan_rt) {
      return Verdict(7);
    }
    if (is_attachment(transfer.source)) {
      return Verdict(8);
    }
    auto* source_vulkan_rt = static_cast<VulkanRenderTarget*>(transfer.source);
    if (source_vulkan_rt->key().is_depth &&
        !source_vulkan_rt->view_depth_stencil()) {
      return Verdict(9);
    }
    // Pending constant rows can't be materialized inside the pass.
    if (HasPendingConstantRows(transfer.source) ||
        (transfer.host_depth_source &&
         HasPendingConstantRows(transfer.host_depth_source))) {
      return Verdict(14);
    }

    if (transfer.host_depth_source) {
      if (!dest_key.is_depth) {
        return Verdict(10);
      }
      if (transfer.host_depth_source == dest_vulkan_rt) {
        return Verdict(12);
      }
      if (is_attachment(transfer.host_depth_source)) {
        return Verdict(13);
      }
      auto* host_depth_vulkan_rt =
          static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
      if (!host_depth_vulkan_rt->key().is_depth ||
          !host_depth_vulkan_rt->view_depth_stencil()) {
        return Verdict(11);
      }
    }
  }

  std::vector<TransferRectanglePlan> transfer_rectangle_plans;
  return BuildTransferRectanglePlans(dest_key, transfers,
                                     transfer_rectangle_plans)
             ? Verdict(0)
             : Verdict(15);
}

bool VulkanRenderTargetCache::PendingDrawPassTransfersFullyOverwriteTarget(
    uint32_t render_target_index, RenderTarget* render_target,
    const std::vector<Transfer>& transfers) const {
  if (!render_target || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }

  auto* dest_vulkan_rt = static_cast<VulkanRenderTarget*>(render_target);
  RenderTargetKey dest_key = dest_vulkan_rt->key();
  if (dest_key.is_depth != (render_target_index == 0) ||
      !last_update_framebuffer_) {
    return false;
  }

  // The whole destination image, not the previous framebuffer: this check
  // runs before the pass's framebuffer is chosen, and a larger new render
  // area would DONT_CARE (on tilers: fill with garbage) rows the transfers
  // never wrote.
  const VkExtent2D dest_extent = {
      dest_key.GetWidth() * GetKeyScaleX(dest_key),
      GetRenderTargetHeight(dest_key) * GetKeyScaleY(dest_key)};
  if (!dest_extent.width || !dest_extent.height) {
    return false;
  }

  // Full overwrite requires every transfer to be a single rectangle covering
  // the whole target - any partial transfer means prior contents survive and
  // must be loaded.
  auto is_full_target_rectangle = [&](const Transfer::Rectangle& rect) {
    uint32_t scaled_x = rect.x_pixels * draw_resolution_scale_x();
    uint32_t scaled_y = rect.y_pixels * draw_resolution_scale_y();
    uint32_t scaled_width = rect.width_pixels * draw_resolution_scale_x();
    uint32_t scaled_height = rect.height_pixels * draw_resolution_scale_y();
    return !scaled_x && !scaled_y && scaled_width == dest_extent.width &&
           scaled_height == dest_extent.height;
  };

  std::vector<TransferRectanglePlan> transfer_rectangle_plans;
  if (!BuildTransferRectanglePlans(dest_key, transfers,
                                   transfer_rectangle_plans)) {
    return false;
  }
  for (const TransferRectanglePlan& plan : transfer_rectangle_plans) {
    if (plan.rectangle_count != 1 ||
        !is_full_target_rectangle(plan.rectangles[0])) {
      return false;
    }
  }
  return true;
}

VulkanRenderTargetCache::TransferMode VulkanRenderTargetCache::GetTransferMode(
    bool is_stencil_bit_pass, bool dest_is_depth, bool source_is_depth,
    bool has_host_depth_source, bool host_depth_source_is_copy) {
  if (is_stencil_bit_pass) {
    return source_is_depth ? TransferMode::kDepthToStencilBit
                           : TransferMode::kColorToStencilBit;
  }
  if (dest_is_depth) {
    if (has_host_depth_source) {
      return source_is_depth
                 ? (host_depth_source_is_copy
                        ? TransferMode::kDepthAndHostDepthCopyToDepth
                        : TransferMode::kDepthAndHostDepthToDepth)
                 : (host_depth_source_is_copy
                        ? TransferMode::kColorAndHostDepthCopyToDepth
                        : TransferMode::kColorAndHostDepthToDepth);
    }
    return source_is_depth ? TransferMode::kDepthToDepth
                           : TransferMode::kColorToDepth;
  }
  return source_is_depth ? TransferMode::kDepthToColor
                         : TransferMode::kColorToColor;
}

bool VulkanRenderTargetCache::PreflightPendingDrawPassTransfers(
    RenderPassKey render_pass_key) {
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if ((pending_draw_pass_transfer_mask_ & (uint32_t(1) << i)) &&
        !PreflightTransferPipelines(render_pass_key, i,
                                    pending_draw_pass_render_targets_[i],
                                    pending_draw_pass_transfers_[i])) {
      return false;
    }
  }
  return true;
}

bool VulkanRenderTargetCache::PreflightTransferPipelines(
    RenderPassKey render_pass_key, uint32_t i, RenderTarget* dest_rt,
    const std::vector<Transfer>& transfers) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  std::vector<TransferRectanglePlan> transfer_rectangle_plans;
  auto* dest_vulkan_rt = static_cast<VulkanRenderTarget*>(dest_rt);
  if (!dest_vulkan_rt || transfers.empty()) {
    return false;
  }
  RenderTargetKey dest_key = dest_vulkan_rt->key();
  if (dest_key.is_depth != (i == 0) ||
      !BuildTransferRectanglePlans(dest_key, transfers,
                                   transfer_rectangle_plans)) {
    return false;
  }

  bool need_stencil_bit_draws =
      dest_key.is_depth &&
      !vulkan_device->extensions().ext_EXT_shader_stencil_export &&
      (stencil_test_ever_enabled_ ||
       !cvars::rt_cache_skip_unused_stencil_transfers);
  for (uint32_t stencil_pass = 0;
       stencil_pass <= uint32_t(need_stencil_bit_draws); ++stencil_pass) {
    for (const Transfer& transfer : transfers) {
      auto* source_vulkan_rt =
          static_cast<VulkanRenderTarget*>(transfer.source);
      if (!source_vulkan_rt) {
        return false;
      }
      auto* host_depth_source_vulkan_rt =
          stencil_pass
              ? nullptr
              : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
      TransferShaderKey shader_key;
      shader_key.dest_msaa_samples = dest_key.msaa_samples;
      shader_key.dest_color_rt_index = i ? i - 1 : 0;
      shader_key.dest_resource_format = dest_key.resource_format;
      shader_key.source_msaa_samples = source_vulkan_rt->key().msaa_samples;
      shader_key.source_resource_format =
          source_vulkan_rt->key().resource_format;
      bool host_depth_source_is_copy =
          host_depth_source_vulkan_rt == dest_vulkan_rt;
      shader_key.host_depth_source_msaa_samples =
          (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
              ? host_depth_source_vulkan_rt->key().msaa_samples
              : xenos::MsaaSamples::k1X;
      shader_key.mode = GetTransferMode(
          stencil_pass != 0, dest_key.is_depth,
          source_vulkan_rt->key().is_depth,
          host_depth_source_vulkan_rt != nullptr, host_depth_source_is_copy);
      if (!GetTransferPipelines(
              TransferPipelineKey(render_pass_key, shader_key))) {
        return false;
      }
    }
  }
  return true;
}

bool VulkanRenderTargetCache::CanFlushClearChainInCurrentPass(
    uint32_t render_target_index, RenderTarget* dest_rt,
    const std::vector<Transfer>& transfers) {
  if (!cvars::rt_cache_clear_chain_in_pass || use_dynamic_rendering_ ||
      GetPath() != Path::kHostRenderTargets || !dest_rt) {
    return false;
  }
  // The open pass must be the last update's guest pass (Update has not
  // replaced it yet when it flushes chains), with dest_rt at this slot.
  const Framebuffer* framebuffer = last_update_framebuffer_;
  const VulkanCommandProcessor& cp = command_processor_;
  if (!framebuffer || !cp.in_render_pass() || cp.deferred_resolve_pending_ ||
      cp.current_framebuffer_ != framebuffer ||
      cp.current_render_pass_ != last_update_render_pass_ ||
      !(framebuffer->attachment_used_mask &
        (uint32_t(1) << render_target_index)) ||
      !(framebuffer->attachment_keys[render_target_index] == dest_rt->key())) {
    return false;
  }
  if (!CanRunTransfersInGuestPass(
          render_target_index, dest_rt, transfers,
          [this, framebuffer](const RenderTarget* rt) {
            return rt && FramebufferReferencesRenderTarget(*framebuffer,
                                                           rt->key());
          })) {
    return false;
  }
  // No barrier can be recorded inside the pass: the sources must already be
  // readable by fragment shaders, as PreparePendingDrawPassTransferBarriers
  // leaves them before a guest pass begins.
  for (const Transfer& transfer : transfers) {
    for (RenderTarget* rt : {transfer.source, transfer.host_depth_source}) {
      if (!rt) {
        continue;
      }
      const auto& source = *static_cast<const VulkanRenderTarget*>(rt);
      if (source.current_layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
          !(source.current_stage_mask() & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) ||
          !(source.current_access_mask() & VK_ACCESS_SHADER_READ_BIT)) {
        if (cvars::log_in_pass_transfer_rejects > 0) {
          ++in_pass_transfer_verdicts_[18];
        }
        return false;
      }
    }
  }
  if (!PreflightTransferPipelines(last_update_render_pass_key_,
                                  render_target_index, dest_rt, transfers)) {
    if (cvars::log_in_pass_transfer_rejects > 0) {
      ++in_pass_transfer_verdicts_[16];
    }
    return false;
  }
  return true;
}

void VulkanRenderTargetCache::PreparePendingDrawPassTransferBarriers() {
  if (!HasPendingDrawPassTransfers()) {
    return;
  }

  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  std::vector<VulkanRenderTarget*> source_rts;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }
    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto add_source = [&](RenderTarget* rt) {
        if (!rt) {
          return;
        }
        auto* vulkan_rt = static_cast<VulkanRenderTarget*>(rt);
        if (std::find(source_rts.begin(), source_rts.end(), vulkan_rt) ==
            source_rts.end()) {
          source_rts.push_back(vulkan_rt);
        }
      };
      add_source(transfer.source);
      add_source(transfer.host_depth_source);
    }
  }

  for (VulkanRenderTarget* source_vulkan_rt : source_rts) {
    command_processor_.PushImageMemoryBarrier(
        source_vulkan_rt->image(),
        ui::vulkan::util::InitializeSubresourceRange(
            source_vulkan_rt->key().is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        source_vulkan_rt->current_stage_mask(), kSourceStageMask,
        source_vulkan_rt->current_access_mask(), kSourceAccessMask,
        source_vulkan_rt->current_layout(), kSourceLayout);
    source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                               kSourceLayout);
  }
}

void VulkanRenderTargetCache::PrepareClearChainSourceBarriers(
    uint32_t slot_mask, RenderTarget* const* bound_render_targets) {
  for (uint32_t i = 0; i < DrawClearChain::kSlots; ++i) {
    if (slot_mask & (uint32_t(1) << i)) {
      PrepareTransferSourceBarriers(draw_clear_chain_.transfers[i],
                                    bound_render_targets);
    }
  }
}

void VulkanRenderTargetCache::PrepareTransferSourceBarriers(
    const std::vector<Transfer>& transfers,
    RenderTarget* const* bound_render_targets) {
  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  constexpr uint32_t kBound = 1 + xenos::kMaxColorRenderTargets;
  for (const Transfer& transfer : transfers) {
    for (RenderTarget* rt : {transfer.source, transfer.host_depth_source}) {
      // Attachments of this pass stay in their draw state; transfers reading
      // them are refused in the pass and run outside as before.
      if (!rt || std::find(bound_render_targets, bound_render_targets + kBound,
                           rt) != bound_render_targets + kBound) {
        continue;
      }
      auto& source = *static_cast<VulkanRenderTarget*>(rt);
      if (source.current_layout() == kSourceLayout &&
          (source.current_stage_mask() & kSourceStageMask) &&
          (source.current_access_mask() & kSourceAccessMask)) {
        continue;
      }
      command_processor_.PushImageMemoryBarrier(
          source.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              source.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          source.current_stage_mask(), kSourceStageMask,
          source.current_access_mask(), kSourceAccessMask,
          source.current_layout(), kSourceLayout);
      source.SetUsage(kSourceStageMask, kSourceAccessMask, kSourceLayout);
    }
  }
}

bool VulkanRenderTargetCache::CanKeepDeferredClear() const {
  // The Xbox 360 clears in several rectangles (a strip, then the rest): another
  // clear of the same target and value whose rectangle merges exactly joins
  // the deferred one, so the transfers skip the union (Update merges it).
  Transfer::Rectangle merged;
  if (last_update_draw_replaced_by_clear() &&
      last_update_clear_retarget_render_target() ==
          deferred_clear_render_target_ &&
      last_update_clear_retarget_value() == deferred_clear_value_ &&
      !(deferred_clear_value_ &
        (kResolveClearDepthOnlyBit | kResolveClearStencilOnlyBit)) &&
      last_update_transfers()[0].empty() &&
      TryMergeClearRectangles(deferred_clear_rectangle_,
                              last_update_clear_retarget_rectangle(), merged)) {
    return true;
  }
  // Otherwise only a plain draw (not another clear) with the render target as
  // its depth, no new transfers into it and none reading it (they'd see it
  // uncleared).
  if (last_update_draw_replaced_by_clear() ||
      last_update_clear_retarget_render_target() ||
      last_update_accumulated_render_targets()[0] !=
          deferred_clear_render_target_ ||
      !last_update_transfers()[0].empty()) {
    return false;
  }
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    for (const Transfer& transfer : last_update_transfers()[i]) {
      if (transfer.source == deferred_clear_render_target_ ||
          transfer.host_depth_source == deferred_clear_render_target_) {
        return false;
      }
    }
  }
  return true;
}

bool VulkanRenderTargetCache::LazyConstantRowsEligible(
    const RenderTarget* render_target) const {
  if (!cvars::vulkan_lazy_constant_rows || !render_target ||
      use_dynamic_rendering_ || GetPath() != Path::kHostRenderTargets ||
      IsDrawResolutionScaled()) {
    return false;
  }
  const RenderTargetKey key = render_target->key();
  return key.is_depth && key.msaa_samples == xenos::MsaaSamples::k1X;
}

VulkanRenderTargetCache::PendingConstantRows*
VulkanRenderTargetCache::FindPendingConstantRows(
    const RenderTarget* render_target) {
  for (auto& entry : pending_constant_rows_) {
    if (entry.render_target == render_target) return &entry;
  }
  return nullptr;
}

bool VulkanRenderTargetCache::HasPendingConstantRows(
    const RenderTarget* render_target) const {
  for (const auto& entry : pending_constant_rows_) {
    if (entry.render_target == render_target) return true;
  }
  return false;
}

void VulkanRenderTargetCache::EmitLazyClear(RenderTarget* render_target,
                                            uint64_t clear_value,
                                            const VkRect2D& rect) {
  const VkClearAttachment attachment = MakeResolveClearAttachment(
      *static_cast<VulkanRenderTarget*>(render_target), clear_value);
  const VkClearRect clear_rect = {rect, 0, 1};
  command_processor_.deferred_command_buffer().CmdVkClearAttachmentsLazy(
      0, attachment, clear_rect);
  lazy_clear_owners_[0] = {render_target, clear_value};
  ++lazy_rows_stats_.lazy_clears;
}

void VulkanRenderTargetCache::MaterializePendingConstantRows(
    RenderTarget* render_target, uint32_t reason) {
  auto it = std::find_if(
      pending_constant_rows_.begin(), pending_constant_rows_.end(),
      [render_target](const PendingConstantRows& entry) {
        return entry.render_target == render_target;
      });
  if (it == pending_constant_rows_.end()) return;
  // Out of the list first: the clears below re-enter the transfer path.
  PendingConstantRows entry = std::move(*it);
  pending_constant_rows_.erase(it);
  ++lazy_rows_stats_.materialized[std::min(reason, uint32_t(3))];
  const std::vector<Transfer> no_transfers;
  for (const VkRect2D& rect : entry.rects) {
    // Eligible targets are unscaled: host pixels are guest pixels.
    const Transfer::Rectangle clear_rect = {
        uint32_t(rect.offset.x), uint32_t(rect.offset.y), rect.extent.width,
        rect.extent.height};
    PerformTransfersAndResolveClears(1, &entry.render_target, &no_transfers,
                                     &entry.clear_value, &clear_rect);
  }
}

void VulkanRenderTargetCache::MaterializePendingConstantRowsForDump(
    uint32_t base, uint32_t pitch,
    const std::vector<ResolveCopyDumpRectangle>& rectangles) {
  if (pending_constant_rows_.empty()) return;
  for (const ResolveCopyDumpRectangle& rectangle : rectangles) {
    RenderTarget* render_target = rectangle.render_target;
    PendingConstantRows* entry = FindPendingConstantRows(render_target);
    if (!entry || !rectangle.rows) continue;
    // Rows (1x: 16 pixels per tile row) of the target the dump reads. Columns
    // aren't checked - only ever materializing more than needed.
    const RenderTargetKey key = render_target->key();
    const uint32_t target_pitch = key.GetPitchTiles();
    const uint32_t first_tile =
        base + rectangle.row_first * pitch + rectangle.row_first_start;
    const uint32_t last_tile = base +
                               (rectangle.row_first + rectangle.rows - 1) * pitch +
                               rectangle.row_last_end - 1;
    bool read = !target_pitch || first_tile < key.base_tiles;
    if (!read) {
      const uint32_t y0 = (first_tile - key.base_tiles) / target_pitch * 16;
      const uint32_t y1 = ((last_tile - key.base_tiles) / target_pitch + 1) * 16;
      for (const VkRect2D& rect : entry->rects) {
        if (uint32_t(rect.offset.y) < y1 &&
            uint32_t(rect.offset.y) + rect.extent.height > y0) {
          read = true;
          break;
        }
      }
    }
    if (read) MaterializePendingConstantRows(render_target, 2);
  }
}

void VulkanRenderTargetCache::OnLazyClearsClipped(
    const DeferredLazyClearResult* results, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const DeferredLazyClearResult& result = results[i];
    const LazyClearOwner owner = lazy_clear_owners_[result.slot];
    if (!owner.render_target) continue;
    // What the area left out of the clear: right of it, then below it (the
    // area starts at the origin, so nothing is above or to the left).
    const VkRect2D& r = result.original;
    const VkRect2D& a = result.applied;
    VkRect2D kept[2];
    uint32_t kept_count = 0;
    if (!a.extent.width || !a.extent.height) {
      kept[kept_count++] = r;
    } else {
      const int32_t r_x1 = r.offset.x + int32_t(r.extent.width);
      const int32_t r_y1 = r.offset.y + int32_t(r.extent.height);
      const int32_t a_x1 = a.offset.x + int32_t(a.extent.width);
      const int32_t a_y1 = a.offset.y + int32_t(a.extent.height);
      if (a_x1 < r_x1) {
        kept[kept_count++] = {{a_x1, r.offset.y},
                              {uint32_t(r_x1 - a_x1), uint32_t(a_y1 - r.offset.y)}};
      }
      if (a_y1 < r_y1) {
        kept[kept_count++] = {{r.offset.x, a_y1},
                              {r.extent.width, uint32_t(r_y1 - a_y1)}};
      }
    }
    if (!kept_count) continue;
    PendingConstantRows* entry = FindPendingConstantRows(owner.render_target);
    if (!entry) {
      pending_constant_rows_.push_back(
          {owner.render_target, owner.clear_value, {}});
      entry = &pending_constant_rows_.back();
    }
    // The pass recorded every pending rectangle of its targets, so an entry
    // here only comes from this loop, with the same value.
    assert_true(entry->clear_value == owner.clear_value);
    for (uint32_t j = 0; j < kept_count; ++j) {
      entry->rects.push_back(kept[j]);
      ++lazy_rows_stats_.kept_rows_rects;
      lazy_rows_stats_.kept_pixels +=
          uint64_t(kept[j].extent.width) * kept[j].extent.height;
    }
  }
  lazy_clear_owners_ = {};
}

void VulkanRenderTargetCache::OnGuestRenderPassBegun() {
  // A deferred clear with nothing around it goes first, so that it can fold
  // into loadOp CLEAR (anything recorded before it, like a query, prevents
  // that). The rest of EncodeDeferredClear's cases stay with the draw path.
  if (HasDeferredClearForPass() && deferred_clear_transfers_.empty() &&
      LazyConstantRowsEligible(deferred_clear_render_target_) &&
      !command_processor_.deferred_resolve_pending_) {
    EncodeDeferredClear();
  }
  if (pending_constant_rows_.empty() || !last_update_framebuffer_ ||
      command_processor_.current_framebuffer_ != last_update_framebuffer_) {
    return;
  }
  const RenderTarget* depth = last_update_framebuffer_attachments_[0];
  if (!depth || (HasDeferredClearForPass() &&
                 deferred_clear_render_target_ == depth)) {
    // EncodeDeferredClear orders the target's pending rows with its clear.
    return;
  }
  auto it = std::find_if(pending_constant_rows_.begin(),
                         pending_constant_rows_.end(),
                         [depth](const PendingConstantRows& entry) {
                           return entry.render_target == depth;
                         });
  if (it == pending_constant_rows_.end()) return;
  PendingConstantRows entry = std::move(*it);
  pending_constant_rows_.erase(it);
  for (const VkRect2D& rect : entry.rects) {
    EmitLazyClear(entry.render_target, entry.clear_value, rect);
  }
}

void VulkanRenderTargetCache::EncodeDeferredClear() {
  assert_true(HasDeferredClearForPass());
  RenderTarget* target = deferred_clear_render_target_;
  const uint64_t value = deferred_clear_value_;
  const Transfer::Rectangle rectangle = deferred_clear_rectangle_;
  std::vector<Transfer> transfers = std::move(deferred_clear_transfers_);
  // Clear the state before encoding: helpers may end a submission.
  deferred_clear_render_target_ = nullptr;
  deferred_clear_transfers_.clear();
  deferred_clear_in_pass_ = false;
  // vulkan_lazy_constant_rows: unscaled, so host pixels are guest pixels.
  const bool lazy = transfers.empty() && LazyConstantRowsEligible(target);
  const VkRect2D clear_rect = {
      {int32_t(rectangle.x_pixels), int32_t(rectangle.y_pixels)},
      {rectangle.width_pixels, rectangle.height_pixels}};
  if (PendingConstantRows* pending = FindPendingConstantRows(target)) {
    bool covered = lazy;
    for (const VkRect2D& rect : pending->rects) {
      covered = covered && rect.offset.x >= clear_rect.offset.x &&
                rect.offset.y >= clear_rect.offset.y &&
                rect.offset.x + rect.extent.width <=
                    clear_rect.offset.x + clear_rect.extent.width &&
                rect.offset.y + rect.extent.height <=
                    clear_rect.offset.y + clear_rect.extent.height;
    }
    PendingConstantRows entry = std::move(*pending);
    pending_constant_rows_.erase(pending_constant_rows_.begin() +
                                 (pending - pending_constant_rows_.data()));
    // Covered by the new clear: superseded. Otherwise the older rows go
    // first, then the new clear overwrites its part - not lazily, so a slot
    // has a single lazy value per pass.
    if (!covered) {
      for (const VkRect2D& rect : entry.rects) {
        EmitLazyClear(target, entry.clear_value, rect);
      }
      PerformTransfersAndResolveClears(1, &target, &transfers, &value,
                                       &rectangle, true);
      return;
    }
  }
  if (lazy) {
    EmitLazyClear(target, value, clear_rect);
    return;
  }
  PerformTransfersAndResolveClears(1, &target, &transfers, &value, &rectangle,
                                   true);
}

void VulkanRenderTargetCache::FlushDeferredClear() {
  if (!deferred_clear_render_target_) {
    return;
  }
  RenderTarget* target = deferred_clear_render_target_;
  const uint64_t value = deferred_clear_value_;
  const Transfer::Rectangle rectangle = deferred_clear_rectangle_;
  std::vector<Transfer> transfers = std::move(deferred_clear_transfers_);
  deferred_clear_render_target_ = nullptr;
  deferred_clear_transfers_.clear();
  deferred_clear_in_pass_ = false;
  PerformTransfersAndResolveClears(1, &target, &transfers, &value, &rectangle);
}

bool VulkanRenderTargetCache::EncodePendingDrawPassTransfers(
    bool draw_will_execute) {
  if (HasPendingDrawPassTransfers() &&
      command_processor_.deferred_resolve_pending_) {
    command_processor_.FlushDeferredResolve(
        VulkanCommandProcessor::ResolveDeferStop::kResource);
    // The flush ended the pass. Let the caller materialize transfers through
    // its ordinary out-of-pass fallback, then restore the guest draw state.
    return false;
  }
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!PreflightPendingDrawPassTransfers(last_update_render_pass_key_)) {
    return false;
  }
  PerformTransfersWithCutout(pending_draw_pass_render_targets_.data(),
                             pending_draw_pass_transfers_.data(),
                             draw_will_execute ? pending_draw_cutout_mask_ : 0,
                             pending_draw_cutout_, true);
  ClearPendingDrawPassTransfers();
  return true;
}

bool VulkanRenderTargetCache::TryMergeClearRectangles(
    const Transfer::Rectangle& a, const Transfer::Rectangle& b,
    Transfer::Rectangle& merged) {
  const uint32_t ax1 = a.x_pixels + a.width_pixels;
  const uint32_t ay1 = a.y_pixels + a.height_pixels;
  const uint32_t bx1 = b.x_pixels + b.width_pixels;
  const uint32_t by1 = b.y_pixels + b.height_pixels;
  if (a.y_pixels == b.y_pixels && a.height_pixels == b.height_pixels &&
      a.x_pixels <= bx1 && b.x_pixels <= ax1) {
    const uint32_t x0 = std::min(a.x_pixels, b.x_pixels);
    merged = {x0, a.y_pixels, std::max(ax1, bx1) - x0, a.height_pixels};
    return true;
  }
  if (a.x_pixels == b.x_pixels && a.width_pixels == b.width_pixels &&
      a.y_pixels <= by1 && b.y_pixels <= ay1) {
    const uint32_t y0 = std::min(a.y_pixels, b.y_pixels);
    merged = {a.x_pixels, y0, a.width_pixels, std::max(ay1, by1) - y0};
    return true;
  }
  return false;
}

void VulkanRenderTargetCache::FlushPendingClearTransfers() {
  // Resolves, submission ends and failed updates end clear draw chains too.
  FlushDeferredClear();
  FlushDrawClearChain();
  if (!pending_clear_render_target_) {
    return;
  }
  RenderTarget* target = pending_clear_render_target_;
  const Transfer::Rectangle cutout = pending_clear_cutout_;
  const auto key = target->key();
  std::vector<Transfer> transfers;
  uint64_t original_pixels = 0, surviving_pixels = 0;
  for (const Transfer& transfer : pending_clear_transfers_) {
    Transfer::Rectangle rects[Transfer::kMaxRectanglesWithCutout];
    const uint32_t count = transfer.GetRectangles(
        key.base_tiles, key.GetPitchTiles(), key.msaa_samples,
        false, rects, &cutout);
    if (count) {
      transfers.push_back(transfer);
    }
    if (command_processor_.IsRtTraceActive()) {
      for (uint32_t i = 0; i < count; ++i) {
        surviving_pixels += uint64_t(rects[i].width_pixels) * rects[i].height_pixels;
      }
      const uint32_t original_count = transfer.GetRectangles(
          key.base_tiles, key.GetPitchTiles(), key.msaa_samples, false, rects);
      for (uint32_t i = 0; i < original_count; ++i) {
        original_pixels += uint64_t(rects[i].width_pixels) * rects[i].height_pixels;
      }
    }
  }
  // Clear the slot before encoding: command-buffer helpers may end a submission.
  pending_clear_render_target_ = nullptr;
  pending_clear_transfers_.clear();
  if (command_processor_.IsRtTraceActive()) {
    XELOGI("RTTRACE clear_chain flush cutout=[{},{},{},{}] sources={} pixels={}/{}",
           cutout.x_pixels, cutout.y_pixels, cutout.width_pixels,
           cutout.height_pixels, transfers.size(), surviving_pixels, original_pixels);
  }
  // The rectangle is only a transfer cutout here. The clears were already
  // encoded in guest order and may have different depth or stencil values.
  PerformTransfersAndResolveClears(1, &target, &transfers, nullptr, &cutout);
}

bool VulkanRenderTargetCache::FlushPendingDrawPassTransfers() {
  PerformPendingOutOfPassTransfers(false);
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                   pending_draw_pass_render_targets_.data(),
                                   pending_draw_pass_transfers_.data());
  ClearPendingDrawPassTransfers();
  return true;
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments,
    uint32_t* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  const RenderTarget* const* rts = last_update_accumulated_render_targets();

  // Initialize depth/stencil attachments. Must match what pipeline creation
  // declared (depthAttachmentFormat from key.depth_and_color_used bit 0); null
  // RT still consumes the slot with imageView=VK_NULL_HANDLE.
  std::memset(depth_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  std::memset(stencil_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  if (key.depth_and_color_used & 0b1) {
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    stencil_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (rts[0]) {
      const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
      depth_attachment->imageView = vulkan_rt->view_depth_stencil();
      depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
      depth_attachment->loadOp = (key.depth_and_color_load_dont_care & 0b1)
                                     ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (GetMsrtssSampleCount(key.msaa_samples)) {
        // The value of one sample is the only resolve every implementation
        // has for depth and stencil.
        depth_attachment->resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      }
      // Stencil uses the same view for depth-stencil formats.
      *stencil_attachment = *depth_attachment;
    }
  }

  // Set up color attachments. The slot count must match what pipeline creation
  // declared (colorAttachmentCount from key.depth_and_color_used bits 1-4),
  // otherwise the pipeline's FS may write a Location that has no destination
  // and the result is undefined per the Vulkan spec - RADV hangs on this.
  // Null RT entries get imageView=VK_NULL_HANDLE; writes to them are silently
  // dropped per spec.
  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    std::memset(&color_attachment, 0, sizeof(VkRenderingAttachmentInfo));
    if (!(key.depth_and_color_used & (1 << (1 + i)))) {
      continue;
    }
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment_count = i + 1;
    if (!rts[1 + i]) {
      continue;
    }
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[1 + i]);
    const bool use_transfer_view =
        key.color_rts_view_mode == kColorRTViewModeTransferAll ||
        key.color_rts_view_mode == kColorRTViewModeR32UintFirst + i;
    color_attachment.imageView =
        use_transfer_view ? vulkan_rt->view_color_transfer()
                          : vulkan_rt->view_depth_color();
    color_attachment.imageLayout = color_draw_layout_;
    color_attachment.loadOp =
        (key.depth_and_color_load_dont_care & (uint32_t(1) << (1 + i)))
            ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
            : VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (GetMsrtssSampleCount(key.msaa_samples)) {
      // Integer views (raw ownership transfer formats) can't be averaged.
      bool view_is_integer = false;
      if (use_transfer_view) {
        GetColorOwnershipTransferVulkanFormat(
            vulkan_rt->key().GetColorFormat(), &view_is_integer);
      }
      color_attachment.resolveMode = view_is_integer
                                         ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                         : VK_RESOLVE_MODE_AVERAGE_BIT;
    }
  }
  *color_attachment_count_out = color_attachment_count;
}

// vulkan_same_depth_stencil_ops: the op keeping more of the contents for
// both aspects (load / store over none over clear / don't care).
static void UnifyDepthStencilOps(VkAttachmentDescription& attachment) {
  auto preserves = [](VkAttachmentLoadOp op) {
    return op == VK_ATTACHMENT_LOAD_OP_LOAD ||
           op == VK_ATTACHMENT_LOAD_OP_NONE_EXT;
  };
  VkAttachmentLoadOp& depth_load = attachment.loadOp;
  VkAttachmentLoadOp& stencil_load = attachment.stencilLoadOp;
  if (depth_load != stencil_load) {
    if (depth_load == VK_ATTACHMENT_LOAD_OP_CLEAR ||
        stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
      // A clear can only absorb an aspect whose contents don't matter.
      if (depth_load == VK_ATTACHMENT_LOAD_OP_DONT_CARE ||
          stencil_load == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
        depth_load = stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
      }
    } else if (preserves(depth_load) || preserves(stencil_load)) {
      depth_load = stencil_load =
          (depth_load == VK_ATTACHMENT_LOAD_OP_LOAD ||
           stencil_load == VK_ATTACHMENT_LOAD_OP_LOAD)
              ? VK_ATTACHMENT_LOAD_OP_LOAD
              : VK_ATTACHMENT_LOAD_OP_NONE_EXT;
    }
  }
  VkAttachmentStoreOp& depth_store = attachment.storeOp;
  VkAttachmentStoreOp& stencil_store = attachment.stencilStoreOp;
  if (depth_store != stencil_store) {
    depth_store = stencil_store =
        (depth_store == VK_ATTACHMENT_STORE_OP_STORE ||
         stencil_store == VK_ATTACHMENT_STORE_OP_STORE)
            ? VK_ATTACHMENT_STORE_OP_STORE
        : (depth_store == VK_ATTACHMENT_STORE_OP_NONE ||
           stencil_store == VK_ATTACHMENT_STORE_OP_NONE)
            ? VK_ATTACHMENT_STORE_OP_NONE
            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }
}

VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(
    RenderPassKey key, RenderPassOps ops) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const uint64_t cache_key = uint64_t(key.key) | (uint64_t(ops.key) << 32);
  auto it = render_passes_.find(cache_key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MsaaSamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MsaaSamples::k2X:
      samples = IsMsaa2xSupported(key.depth_and_color_used != 0)
                    ? VK_SAMPLE_COUNT_2_BIT
                    : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MsaaSamples::k4X:
      samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      return VK_NULL_HANDLE;
  }
  if (msaa_single_sample_storage_) {
    // The images are single-sampled (multisampled rendering into them is for
    // dynamic rendering only).
    samples = VK_SAMPLE_COUNT_1_BIT;
  }

  VkAttachmentDescription attachments[1 + xenos::kMaxColorRenderTargets];
  if (key.depth_and_color_used & 0b1) {
    VkAttachmentDescription& attachment = attachments[0];
    attachment.flags = 0;
    attachment.format = GetDepthVulkanFormat(key.depth_format);
    attachment.samples = samples;
    attachment.loadOp = (key.depth_and_color_load_dont_care & 0b1)
                            ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                            : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilLoadOp = ops.LoadOp(1, attachment.loadOp);
    attachment.loadOp = ops.LoadOp(0, attachment.loadOp);
    attachment.storeOp = ops.StoreOp(0);
    attachment.stencilStoreOp = ops.StoreOp(1);
    if (cvars::vulkan_same_depth_stencil_ops) {
      UnifyDepthStencilOps(attachment);
    }
    attachment.initialLayout = VulkanRenderTarget::kDepthDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kDepthDrawLayout;
  }
  VkAttachmentReference color_attachments[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    uint32_t attachment_bit = uint32_t(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    uint32_t attachment_index =
        xe::bit_count(key.depth_and_color_used & (attachment_bit - 1));
    color_attachment.attachment = attachment_index;
    VkAttachmentDescription& attachment = attachments[attachment_index];
    attachment.flags = 0;
    attachment.format = GetColorRenderPassVulkanFormat(key, i);
    attachment.samples = samples;
    attachment.loadOp = (key.depth_and_color_load_dont_care & attachment_bit)
                            ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                            : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.loadOp = ops.LoadOp(i + 2, attachment.loadOp);
    attachment.storeOp = ops.StoreOp(i + 2);
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VulkanRenderTarget::kColorDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kColorDrawLayout;
  }

  VkAttachmentReference depth_stencil_attachment;
  depth_stencil_attachment.attachment =
      (key.depth_and_color_used & 0b1) ? 0 : VK_ATTACHMENT_UNUSED;
  depth_stencil_attachment.layout = VulkanRenderTarget::kDepthDrawLayout;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount =
      32 - xe::lzcnt(uint32_t(key.depth_and_color_used >> 1));
  subpass.pColorAttachments = color_attachments;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment =
      (key.depth_and_color_used & 0b1) ? &depth_stencil_attachment : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkPipelineStageFlags dependency_stage_mask = 0;
  VkAccessFlags dependency_access_mask = 0;
  if (key.depth_and_color_used & 0b1) {
    dependency_stage_mask |= VulkanRenderTarget::kDepthDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kDepthDrawAccessMask;
  }
  if (key.depth_and_color_used >> 1) {
    dependency_stage_mask |= VulkanRenderTarget::kColorDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kColorDrawAccessMask;
  }
  VkSubpassDependency subpass_dependencies[2];
  subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[0].dstSubpass = 0;
  subpass_dependencies[0].srcStageMask = dependency_stage_mask;
  subpass_dependencies[0].dstStageMask = dependency_stage_mask;
  subpass_dependencies[0].srcAccessMask = dependency_access_mask;
  subpass_dependencies[0].dstAccessMask = dependency_access_mask;
  subpass_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  subpass_dependencies[1].srcSubpass = 0;
  subpass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[1].srcStageMask = dependency_stage_mask;
  subpass_dependencies[1].dstStageMask = dependency_stage_mask;
  subpass_dependencies[1].srcAccessMask = dependency_access_mask;
  subpass_dependencies[1].dstAccessMask = dependency_access_mask;
  subpass_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  VkRenderPassCreateInfo render_pass_create_info;
  render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;
  render_pass_create_info.attachmentCount =
      xe::bit_count(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? uint32_t(xe::countof(subpass_dependencies))
                               : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr,
                             &render_pass) != VK_SUCCESS) {
    XELOGE("VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(cache_key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(cache_key, render_pass);
  render_pass_keys_.emplace(render_pass, key);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 &&
      depth_unorm24_vulkan_format_supported()) {
    return VK_FORMAT_D24_UNORM_S8_UINT;
  }
  return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

VkFormat VulkanRenderTargetCache::GetColorVulkanFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      // TODO(Triang3l): Fallback to float16 (disregarding clearing correctness
      // likely) - possibly on render target gathering, treating them entirely
      // as float16.
      return VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      // TODO(Triang3l): Fallback to float16 (disregarding clearing correctness
      // likely) - possibly on render target gathering, treating them entirely
      // as float16.
      return VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      assert_unhandled_case(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  // Floating-point numbers have NaNs that need to be propagated without
  // modifications to the bit representation, and SNORM has two representations
  // of -1.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
  }
}

VkFormat VulkanRenderTargetCache::GetColorRenderPassVulkanFormat(
    RenderPassKey key, uint32_t color_slot) const {
  assert_true(color_slot < xenos::kMaxColorRenderTargets);
  const xenos::ColorRenderTargetFormat color_formats[] = {
      key.color_0_view_format, key.color_1_view_format,
      key.color_2_view_format, key.color_3_view_format};
  const xenos::ColorRenderTargetFormat color_format =
      color_formats[color_slot];
  switch (key.color_rts_view_mode) {
    case kColorRTViewModeGuest:
      return GetColorVulkanFormat(color_format);
    case kColorRTViewModeTransferAll:
      return GetColorOwnershipTransferVulkanFormat(color_format);
    default:
      if (key.color_rts_view_mode >= kColorRTViewModeR32UintFirst &&
          key.color_rts_view_mode <
              kColorRTViewModeR32UintFirst +
                  xenos::kMaxColorRenderTargets) {
        const uint32_t uint_slot =
            key.color_rts_view_mode - kColorRTViewModeR32UintFirst;
        if (color_formats[uint_slot] !=
            xenos::ColorRenderTargetFormat::k_32_FLOAT) {
          return VK_FORMAT_UNDEFINED;
        }
        return color_slot == uint_slot
                   ? VK_FORMAT_R32_UINT
                   : GetColorVulkanFormat(color_format);
      }
      assert_unhandled_case(key.color_rts_view_mode);
      return VK_FORMAT_UNDEFINED;
  }
}

void VulkanRenderTargetCache::VulkanRenderTarget::MarkGpuReference() const {
  if (!render_target_cache_.retirement_tracking_enabled()) {
    return;
  }
  MarkGpuReference(
      render_target_cache_.command_processor_.GetCurrentSubmission());
}

void VulkanRenderTargetCache::VulkanRenderTarget::MarkGpuReference(
    uint64_t submission) const {
  if (!render_target_cache_.retirement_tracking_enabled()) {
    return;
  }
  MarkGpuReference(submission,
                   render_target_cache_.command_processor_.GetCurrentFrame());
}

void VulkanRenderTargetCache::VulkanRenderTarget::MarkGpuReference(
    uint64_t submission, uint64_t frame) const {
  if (!render_target_cache_.retirement_tracking_enabled()) {
    return;
  }
  RecordSubmissionReference(gpu_reference_submission_known_,
                            last_gpu_reference_submission_, submission);
  if (!last_gpu_reference_frame_ || frame > last_gpu_reference_frame_) {
    last_gpu_reference_frame_ = frame;
  }
}

void VulkanRenderTargetCache::MarkLastUpdateFramebufferReference() const {
  if (!retirement_tracking_enabled_ || !last_update_framebuffer_) {
    return;
  }
  const uint64_t submission = command_processor_.GetCurrentSubmission();
  const uint64_t frame = command_processor_.GetCurrentFrame();
  auto* framebuffer = const_cast<Framebuffer*>(last_update_framebuffer_);
  framebuffer->MarkGpuReference(submission);
  if (last_update_framebuffer_ == &fsi_framebuffer_) {
    return;
  }
  for (const RenderTarget* attachment : last_update_framebuffer_attachments_) {
    if (attachment) {
      static_cast<const VulkanRenderTarget*>(attachment)->MarkGpuReference(
          submission, frame);
    }
  }
}

VulkanRenderTargetCache::VulkanRenderTarget::~VulkanRenderTarget() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key().is_depth
          ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
          : *render_target_cache_.descriptor_set_pool_sampled_image_;
  if (descriptor_set_index_transfer_source_ != SIZE_MAX) {
    render_target_cache_.command_processor_
        .InvalidateCachedRenderTargetDescriptorSet(
            descriptor_set_pool.Get(descriptor_set_index_transfer_source_));
  }
  descriptor_set_pool.Free(descriptor_set_index_transfer_source_);
  if (descriptor_set_index_input_attachment_ != SIZE_MAX &&
      render_target_cache_.descriptor_set_pool_input_attachment_) {
    render_target_cache_.command_processor_
        .InvalidateCachedRenderTargetDescriptorSet(
            render_target_cache_.descriptor_set_pool_input_attachment_->Get(
                descriptor_set_index_input_attachment_));
    render_target_cache_.descriptor_set_pool_input_attachment_->Free(
        descriptor_set_index_input_attachment_);
  }
  if (descriptor_set_index_input_attachment_uint_ != SIZE_MAX &&
      render_target_cache_.descriptor_set_pool_input_attachment_) {
    render_target_cache_.command_processor_
        .InvalidateCachedRenderTargetDescriptorSet(
            render_target_cache_.descriptor_set_pool_input_attachment_->Get(
                descriptor_set_index_input_attachment_uint_));
    render_target_cache_.descriptor_set_pool_input_attachment_->Free(
        descriptor_set_index_input_attachment_uint_);
  }
  if (view_color_transfer_separate_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_color_transfer_separate_, nullptr);
  }
  if (view_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_stencil_, nullptr);
  }
  if (view_depth_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_depth_stencil_, nullptr);
  }
  dfn.vkDestroyImageView(device, view_depth_color_, nullptr);
  dfn.vkDestroyImage(device, image_, nullptr);
  dfn.vkFreeMemory(device, memory_, nullptr);
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferWidth,
                  device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferHeight,
                  device_properties.maxImageDimension2D);
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->vulkan_instance()->functions();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image.

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = key.GetWidth() * GetKeyScaleX(key);
  image_create_info.extent.height =
      GetRenderTargetHeight(key) *
      GetKeyScaleY(key);
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (msaa_single_sample_storage_) {
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  } else if (key.msaa_samples == xenos::MsaaSamples::k2X &&
             !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples =
        VkSampleCountFlagBits(uint32_t(1) << uint32_t(key.msaa_samples));
  }
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat transfer_format;
  if (key.is_depth) {
    image_create_info.format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = image_create_info.format;
    image_create_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (cvars::vulkan_stencil_transfer_copy &&
        key.msaa_samples == xenos::MsaaSamples::k1X &&
        GetKeyScaleX(key) == 1 && GetKeyScaleY(key) == 1) {
      image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
  } else {
    xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
    image_create_info.format = GetColorVulkanFormat(color_format);
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format);
    if (image_create_info.format != transfer_format) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    image_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (local_read_attachments_) {
      image_create_info.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
  }
  if (GetMsrtssSampleCount(key.msaa_samples)) {
    image_create_info.flags |=
        VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    XELOGE("VulkanRenderTargetCache: Unknown {} render target format {}",
           key.is_depth ? "depth" : "color", key.resource_format);
    return nullptr;
  }
  bool transfer_src_enabled = false;
  const bool snapshot_canonical = cvars::savestate_enable && !key.is_depth &&
      !key.base_tiles && key.pitch_tiles_at_32bpp == 16 &&
      key.msaa_samples == xenos::MsaaSamples::k1X && !key.height_class &&
      image_create_info.format == VK_FORMAT_R8G8B8A8_UNORM;
  if (snapshot_canonical) {
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    transfer_src_enabled = true;
  }
  const bool capture_configured =
      !cvars::vulkan_source_rt_capture_request.empty();
  if (capture_configured && !key.is_depth &&
      key.msaa_samples == xenos::MsaaSamples::k1X &&
      (image_create_info.format == VK_FORMAT_R8G8B8A8_UNORM ||
       image_create_info.format == VK_FORMAT_R16G16B16A16_UNORM)) {
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    transfer_src_enabled = true;
  }
  // Declare the full set of view formats for mutable-format images (the
  // UNORM/SFLOAT attachment format is aliased with a UINT view for ownership
  // transfers and EDRAM dumps). Without this, drivers must assume any
  // compatible format may alias the image and can't make safe compression
  // decisions - on UBWC-compressing mobile drivers, sampling the aliased view
  // of a compressed render target without the list returns zeros or garbage
  // (the texture cache already does this for its reinterpreted formats).
  VkFormat image_view_formats[] = {image_create_info.format, transfer_format};
  VkImageFormatListCreateInfo image_format_list_create_info;
  if ((image_create_info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
      vulkan_device->extensions().ext_1_2_KHR_image_format_list) {
    image_format_list_create_info.sType =
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    image_format_list_create_info.pNext = nullptr;
    image_format_list_create_info.viewFormatCount =
        uint32_t(xe::countof(image_view_formats));
    image_format_list_create_info.pViewFormats = image_view_formats;
    image_create_info.pNext = &image_format_list_create_info;
  }
  VkImage image;
  VkDeviceMemory memory;
  VkDeviceSize allocation_size = 0;
  bool image_created = ui::vulkan::util::CreateDedicatedAllocationImage(
      vulkan_device, image_create_info,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal, image, memory, nullptr,
      &allocation_size);
  if (!image_created && transfer_src_enabled) {
    // Format feature bits do not guarantee every usage combination. Fall back
    // to normal render-target usage if an optional transfer usage cannot be
    // created, keeping both diagnostics/optimizations safe on that driver.
    image_create_info.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    transfer_src_enabled = false;
    image_created = ui::vulkan::util::CreateDedicatedAllocationImage(
        vulkan_device, image_create_info,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, image, memory, nullptr,
        &allocation_size);
  }
  if (!image_created) {
    XELOGE(
        "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
        "image",
        image_create_info.extent.width, image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return nullptr;
  }

  if (cvars::log_image_compression) {
    vulkan_device->LogImageCompression(
        key.is_depth ? 1 : 0, image, image_create_info,
        key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  }
  // Set debug name for the image.
  std::string debug_name = key.GetDebugName();
  vulkan_device->SetObjectName(VK_OBJECT_TYPE_IMAGE, image, debug_name.c_str());

  // Create the image views.

  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = image_create_info.format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange =
      ui::vulkan::util::InitializeSubresourceRange(
          key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                            &view_depth_color) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
        "render target",
        key.is_depth ? "depth" : "color", image_create_info.extent.width,
        image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkImageView view_depth_stencil = VK_NULL_HANDLE;
  VkImageView view_stencil = VK_NULL_HANDLE;
  VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
  if (key.is_depth) {
    view_create_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                              &view_depth_stencil) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTarget: Failed to create a depth / stencil view for a "
          "{}x{} {}xMSAA {} render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                              &view_stencil) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
          "{}xMSAA render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_stencil, nullptr);
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
  } else {
    if (transfer_format != image_create_info.format) {
      view_create_info.format = transfer_format;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                                &view_color_transfer_separate) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
            "{}xMSAA {} render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
  }

  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_
                   : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to allocate sampled image descriptors "
        "for a {} render target",
        key.is_depth ? "depth/stencil" : "color");
    if (view_color_transfer_separate != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_color_transfer_separate, nullptr);
    }
    dfn.vkDestroyImageView(device, view_depth_color, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkDescriptorSet descriptor_set_transfer_source =
      descriptor_set_pool.Get(descriptor_set_index_transfer_source);
  VkWriteDescriptorSet descriptor_set_write[2];
  VkDescriptorImageInfo descriptor_set_write_depth_color;
  descriptor_set_write_depth_color.sampler = VK_NULL_HANDLE;
  descriptor_set_write_depth_color.imageView =
      view_color_transfer_separate != VK_NULL_HANDLE
          ? view_color_transfer_separate
          : view_depth_color;
  descriptor_set_write_depth_color.imageLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  descriptor_set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_set_write[0].pNext = nullptr;
  descriptor_set_write[0].dstSet = descriptor_set_transfer_source;
  descriptor_set_write[0].dstBinding = 0;
  descriptor_set_write[0].dstArrayElement = 0;
  descriptor_set_write[0].descriptorCount = 1;
  descriptor_set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_write[0].pImageInfo = &descriptor_set_write_depth_color;
  descriptor_set_write[0].pBufferInfo = nullptr;
  descriptor_set_write[0].pTexelBufferView = nullptr;
  VkDescriptorImageInfo descriptor_set_write_stencil;
  if (key.is_depth) {
    descriptor_set_write_stencil.sampler = VK_NULL_HANDLE;
    descriptor_set_write_stencil.imageView = view_stencil;
    descriptor_set_write_stencil.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write[1].pNext = nullptr;
    descriptor_set_write[1].dstSet = descriptor_set_transfer_source;
    descriptor_set_write[1].dstBinding = 1;
    descriptor_set_write[1].dstArrayElement = 0;
    descriptor_set_write[1].descriptorCount = 1;
    descriptor_set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write[1].pImageInfo = &descriptor_set_write_stencil;
    descriptor_set_write[1].pBufferInfo = nullptr;
    descriptor_set_write[1].pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, key.is_depth ? 2 : 1, descriptor_set_write,
                             0, nullptr);

  size_t descriptor_set_index_input_attachment = SIZE_MAX;
  size_t descriptor_set_index_input_attachment_uint = SIZE_MAX;
  if (!key.is_depth && local_read_attachments_ &&
      descriptor_set_pool_input_attachment_) {
    descriptor_set_index_input_attachment =
        descriptor_set_pool_input_attachment_->Allocate();
    if (descriptor_set_index_input_attachment != SIZE_MAX) {
      VkDescriptorImageInfo input_attachment_image_info;
      input_attachment_image_info.sampler = VK_NULL_HANDLE;
      input_attachment_image_info.imageView = view_depth_color;
      input_attachment_image_info.imageLayout =
          VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
      VkWriteDescriptorSet input_attachment_write;
      input_attachment_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      input_attachment_write.pNext = nullptr;
      input_attachment_write.dstSet =
          descriptor_set_pool_input_attachment_->Get(
              descriptor_set_index_input_attachment);
      input_attachment_write.dstBinding = 0;
      input_attachment_write.dstArrayElement = 0;
      input_attachment_write.descriptorCount = 1;
      input_attachment_write.descriptorType =
          VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
      input_attachment_write.pImageInfo = &input_attachment_image_info;
      input_attachment_write.pBufferInfo = nullptr;
      input_attachment_write.pTexelBufferView = nullptr;
      dfn.vkUpdateDescriptorSets(device, 1, &input_attachment_write, 0,
                                 nullptr);
    }
    if (key.GetColorFormat() ==
            xenos::ColorRenderTargetFormat::k_32_FLOAT &&
        view_color_transfer_separate != VK_NULL_HANDLE &&
        cvars::vulkan_in_pass_resolve &&
        cvars::vulkan_in_pass_resolve_r32_uint &&
        r32_uint_input_attachment_supported_) {
      descriptor_set_index_input_attachment_uint =
          descriptor_set_pool_input_attachment_->Allocate();
      if (descriptor_set_index_input_attachment_uint != SIZE_MAX) {
        VkDescriptorImageInfo input_attachment_image_info;
        input_attachment_image_info.sampler = VK_NULL_HANDLE;
        input_attachment_image_info.imageView = view_color_transfer_separate;
        input_attachment_image_info.imageLayout =
            VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
        VkWriteDescriptorSet input_attachment_write;
        input_attachment_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        input_attachment_write.pNext = nullptr;
        input_attachment_write.dstSet =
            descriptor_set_pool_input_attachment_->Get(
                descriptor_set_index_input_attachment_uint);
        input_attachment_write.dstBinding = 0;
        input_attachment_write.dstArrayElement = 0;
        input_attachment_write.descriptorCount = 1;
        input_attachment_write.descriptorType =
            VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        input_attachment_write.pImageInfo = &input_attachment_image_info;
        input_attachment_write.pBufferInfo = nullptr;
        input_attachment_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &input_attachment_write, 0,
                                   nullptr);
      }
    }
  }

  VulkanRenderTarget* render_target = new VulkanRenderTarget(
      key, *this, image, memory, allocation_size, view_depth_color,
      view_depth_stencil,
      view_stencil, view_color_transfer_separate,
      descriptor_set_index_transfer_source, transfer_src_enabled);
  if (retirement_tracking_enabled_) {
    render_target->MarkGpuReference();
  }
  render_target->stencil_copy_destination =
      (image_create_info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 &&
      key.is_depth;
  render_target->SetDescriptorSetIndexInputAttachment(
      descriptor_set_index_input_attachment);
  render_target->SetDescriptorSetIndexInputAttachmentUint(
      descriptor_set_index_input_attachment_uint);
  return render_target;
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !depth_unorm24_vulkan_format_supported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // When converting in the pixel shader, the host float32 depth already
      // holds float24-grid values, so it's the canonical encoding and the
      // separate host depth tracking isn't needed.
      return !depth_float24_convert_in_pixel_shader();
  }
  return false;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  if (edram_buffer_usage_ == EdramBufferUsage::kFragmentReadWrite) {
    CommitEdramBufferShaderWrites();
  }
}

void VulkanRenderTargetCache::GetEdramBufferUsageMasks(
    EdramBufferUsage usage, VkPipelineStageFlags& stage_mask_out,
    VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EdramBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EdramBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanRenderTargetCache::UseEdramBuffer(EdramBufferUsage new_usage) {
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, src_stage_mask,
                           src_access_mask);
  GetEdramBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(
          edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask, dst_stage_mask,
          src_access_mask, dst_access_mask)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted.
    edram_buffer_modification_status_ =
        EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status !=
              EdramBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EdramBufferUsage::kFragmentReadWrite:
      // max because being modified via unordered access requires stricter
      // synchronization than via fragment shader interlocks.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EdramBufferUsage::kComputeWrite:
      assert_true(modification_status ==
                  EdramBufferModificationStatus::kViaUnordered);
      modification_status = EdramBufferModificationStatus::kViaUnordered;
      break;
    default:
      assert_always(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEdramBufferShaderWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  assert_not_zero(access_mask & VK_ACCESS_SHADER_WRITE_BIT);
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask,
      access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = NormalizeKeyForCacheLookup(render_pass_key);
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles =
        depth_and_color_render_targets[1]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles =
        depth_and_color_render_targets[2]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles =
        depth_and_color_render_targets[3]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles =
        depth_and_color_render_targets[4]->key().base_tiles;
  }
  // All the render targets of a draw must share the exact height class.
  uint32_t height_class = UINT32_MAX;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (render_pass_key.depth_and_color_used & (uint32_t(1) << i)) {
      const uint32_t attachment_height_class =
          depth_and_color_render_targets[i]->key().height_class;
      if (height_class == UINT32_MAX) {
        height_class = attachment_height_class;
      } else {
        assert_true(height_class == attachment_height_class);
      }
    }
  }
  key.height_class = height_class == UINT32_MAX
                         ? kRenderTargetHeightClassFull
                         : height_class;
  const uint32_t attachment_used_mask =
      render_pass_key.depth_and_color_used;
  RenderTargetKey attachment_keys[1 + xenos::kMaxColorRenderTargets] = {};
  uint64_t use_submission = 0;
  uint64_t use_frame = 0;
  if (retirement_tracking_enabled_) {
    use_submission = command_processor_.GetCurrentSubmission();
    use_frame = command_processor_.GetCurrentFrame();
    uint32_t used_attachments = render_pass_key.depth_and_color_used;
    uint32_t used_attachment_index;
    while (xe::bit_scan_forward(used_attachments, &used_attachment_index)) {
      used_attachments &= ~(uint32_t(1) << used_attachment_index);
      const auto* render_target = static_cast<const VulkanRenderTarget*>(
          depth_and_color_render_targets[used_attachment_index]);
      attachment_keys[used_attachment_index] = render_target->key();
      render_target->MarkGpuReference(use_submission, use_frame);
    }
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    if (retirement_tracking_enabled_) {
      it->second.MarkGpuReference(use_submission);
    }
    return &it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  uint32_t rt_index;
  while (xe::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(uint32_t(1) << rt_index);
    const auto& vulkan_rt = *static_cast<const VulkanRenderTarget*>(
        depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      const bool use_transfer_view =
          render_pass_key.color_rts_view_mode ==
              kColorRTViewModeTransferAll ||
          render_pass_key.color_rts_view_mode ==
              kColorRTViewModeR32UintFirst + (rt_index - 1);
      attachment = use_transfer_view ? vulkan_rt.view_color_transfer()
                                     : vulkan_rt.view_depth_color();
    } else {
      attachment = vulkan_rt.view_depth_stencil();
    }
    attachments[attachment_count++] = attachment;
  }

  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = render_pass;
  framebuffer_create_info.attachmentCount = attachment_count;
  framebuffer_create_info.pAttachments = attachments;
  VkExtent2D host_extent;
  // The scale class is a function of the pitch and MSAA mode, so a
  // framebuffer can never mix classes and the key needs no scale bit.
  uint32_t framebuffer_scale_x = draw_resolution_scale_x();
  uint32_t framebuffer_scale_y = draw_resolution_scale_y();
  if (pitch_tiles_at_32bpp) {
    host_extent.width = RenderTargetKey::GetWidth(pitch_tiles_at_32bpp,
                                                  render_pass_key.msaa_samples);
    host_extent.height = GetRenderTargetHeight(pitch_tiles_at_32bpp,
                                               render_pass_key.msaa_samples);
    // The smallest attachment bounds the framebuffer.
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (render_pass_key.depth_and_color_used & (uint32_t(1) << i)) {
        host_extent.height = std::min(
            host_extent.height,
            GetRenderTargetHeight(depth_and_color_render_targets[i]->key()));
      }
    }
    if (IsScaleNativeForPitch(pitch_tiles_at_32bpp,
                              render_pass_key.msaa_samples)) {
      framebuffer_scale_x = 1;
      framebuffer_scale_y = 1;
    }
  } else {
    assert_zero(render_pass_key.depth_and_color_used);
    // Still needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Limiting to the device limit for the case of no attachments, for which
  // there's no limit imposed by the sizes of the attachments that have been
  // created successfully.
  host_extent.width = std::min(host_extent.width * framebuffer_scale_x,
                               device_properties.maxFramebufferWidth);
  host_extent.height = std::min(host_extent.height * framebuffer_scale_y,
                                device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr,
                              &framebuffer) != VK_SUCCESS) {
    return nullptr;
  }
  // Creates at a persistent location - safe to use pointers.
  Framebuffer& framebuffer_object =
      framebuffers_
          .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                   std::forward_as_tuple(
                       framebuffer, host_extent, attachment_keys,
                       attachment_used_mask))
          .first->second;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(attachment_used_mask & (1u << i))) continue;
    const auto& target = *static_cast<const VulkanRenderTarget*>(
        depth_and_color_render_targets[i]);
    // Diagnostic descriptions need these keys even with retirement disabled.
    framebuffer_object.attachment_keys[i] = target.key();
    framebuffer_object.attachment_images[i] = target.image();
    framebuffer_object.attachment_extents[i] = {
        target.key().GetWidth() * GetKeyScaleX(target.key()),
        GetRenderTargetHeight(target.key()) * GetKeyScaleY(target.key())};
  }
  if (retirement_tracking_enabled_) {
    framebuffer_object.MarkGpuReference(use_submission);
  }
  return &framebuffer_object;
}

// Converts a raw 8-bit gamma-encoded byte (a uint scalar) to the midpoint of
// its linear range. Used when a non-gamma EDRAM value is reinterpreted as
// k_8_8_8_8_GAMMA for a linear unorm16 host render target. The midpoint, rather
// than the exact lower edge produced by PWLGammaToLinear, keeps the value
// safely inside the byte's range across the unorm16 quantization round-trip, so
// a later linear -> gamma re-encode reproduces the original byte. Mirrors the
// piecewise constants in the D3D12 render target cache.
static spv::Id GammaByteToLinearMidpoint(SpirvBuilder& builder,
                                         spv::Id byte_uint) {
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_bool = builder.makeBoolType();
  // F = float(byte) * recip + offset, with the piece selected by the byte.
  spv::Id recip = builder.makeFloatConstant(1.0f / 1023.0f);
  spv::Id offset = builder.makeFloatConstant(0.5f / 1023.0f);
  struct Piece {
    uint32_t threshold;
    float recip;
    float offset;
  };
  static const Piece kPieces[] = {
      {64, 1.0f / 511.5f, -31.5f / 511.5f},
      {96, 1.0f / 255.75f, -63.5f / 255.75f},
      {192, 1.0f / 127.875f, -127.5f / 127.875f},
  };
  for (const Piece& piece : kPieces) {
    spv::Id in_piece =
        builder.createBinOp(spv::OpUGreaterThanEqual, type_bool, byte_uint,
                            builder.makeUintConstant(piece.threshold));
    recip = builder.createTriOp(spv::OpSelect, type_float, in_piece,
                                builder.makeFloatConstant(piece.recip), recip);
    offset =
        builder.createTriOp(spv::OpSelect, type_float, in_piece,
                            builder.makeFloatConstant(piece.offset), offset);
  }
  return builder.createBinOp(
      spv::OpFAdd, type_float,
      builder.createBinOp(
          spv::OpFMul, type_float,
          builder.createUnaryOp(spv::OpConvertUToF, type_float, byte_uint),
          recip),
      offset);
}

VkShaderModule VulkanRenderTargetCache::GetTransferShader(
    TransferShaderKey key) {
  auto shader_it = transfer_shaders_.find(key);
  if (shader_it != transfer_shaders_.end()) {
    return shader_it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;
  SpirvBuilder builder(spirv_version_,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp =
      dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color = (pipeline_layout_info.used_descriptor_sets &
                          kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferVulkanFormat(source_color_format,
                                          &source_color_is_uint);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->extensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      GetColorOwnershipTransferVulkanFormat(dest_color_format,
                                            &dest_color_is_uint);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component =
          dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component,
                                       dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data = builder.createVariable(
          spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
          "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation,
                            key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                 type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::FragDepth));
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref =
            builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                   type_int, "gl_FragStencilRefARB");
        builder.addDecoration(
            output_fragment_stencil_ref, spv::DecorationBuiltIn,
            static_cast<int>(spv::BuiltIn::FragStencilRefEXT));
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  // Whether the source image itself is multisampled (it isn't with
  // single-sample storage, whatever the guest sample count the coordinate math
  // below works with).
  bool source_is_multisampled =
      key.source_msaa_samples != xenos::MsaaSamples::k1X &&
      !msaa_single_sample_storage_;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false,
                              false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(
        source_color_texture, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth ||
        shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture,
                            spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                          sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer,
                          spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniform,
        type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(
        host_depth_source_buffer, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(
            type_float, spv::Dim2D, false, false,
            key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X &&
                !msaa_single_sample_storage_,
            1, spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        xe::bit_count(
            pipeline_layout_info.used_descriptor_sets &
            (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants =
        builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address,
                            "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(pipeline_layout_info.used_push_constant_dwords &
                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_stencil_mask, "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(
        spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
        "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::FragCoord));
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id = builder.createVariable(
          spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::SampleId));
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  // The two sides of the transfer may be in different scale classes. The host
  // depth source always has the destination's scale since native render
  // targets don't track host depth.
  uint32_t dest_scale_x = key.dest_scale_native ? 1 : draw_resolution_scale_x();
  uint32_t dest_scale_y = key.dest_scale_native ? 1 : draw_resolution_scale_y();
  uint32_t source_scale_x =
      key.source_scale_native ? 1 : draw_resolution_scale_x();
  uint32_t source_scale_y =
      key.source_scale_native ? 1 : draw_resolution_scale_y();
  uint32_t dest_tile_width_samples =
      xenos::kEdramTileWidthSamples * dest_scale_x;
  uint32_t dest_tile_height_samples =
      xenos::kEdramTileHeightSamples * dest_scale_y;
  uint32_t source_tile_width_samples =
      xenos::kEdramTileWidthSamples * source_scale_x;
  uint32_t source_tile_height_samples =
      xenos::kEdramTileHeightSamples * source_scale_y;

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(
          spv::NoPrecision, type_float2,
          builder.createLoad(input_fragment_coord, spv::NoPrecision),
          uint_vector_temp));
  spv::Id dest_pixel_x =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      dest_tile_width_samples >>
      (uint32_t(dest_is_64bpp) +
       uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  spv::Id dest_tile_index_x = builder.createBinOp(
      spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x = builder.createBinOp(
      spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      dest_tile_height_samples >>
      uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y = builder.createBinOp(
      spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, address_constant,
              builder.makeUintConstant(0),
              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  // If the scale classes differ, convert the tile-local pixel coordinates to
  // the source scale space - the remappings below transform between two
  // layouts of one scale. The destination-space coordinates are saved for the
  // host depth source. Sample indices don't change.
  spv::Id dest_space_tile_pixel_x = dest_tile_pixel_x;
  spv::Id dest_space_tile_pixel_y = dest_tile_pixel_y;
  if (key.source_scale_native != key.dest_scale_native) {
    if (key.dest_scale_native) {
      // Native destination reading a scaled source - take the center host
      // pixel of each guest pixel, like memexport and the resolve downscale
      // do.
      dest_tile_pixel_x = builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(source_scale_x)),
          builder.makeUintConstant(source_scale_x >> 1));
      dest_tile_pixel_y = builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(spv::OpIMul, type_uint, dest_tile_pixel_y,
                              builder.makeUintConstant(source_scale_y)),
          builder.makeUintConstant(source_scale_y >> 1));
    } else {
      // Scaled destination reading a native source - duplicate guest pixels.
      dest_tile_pixel_x =
          builder.createBinOp(spv::OpUDiv, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(dest_scale_x));
      dest_tile_pixel_y =
          builder.createBinOp(spv::OpUDiv, type_uint, dest_tile_pixel_y,
                              builder.makeUintConstant(dest_scale_y));
    }
  }

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id =
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
            builder.makeUintConstant(1), builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(
              spv::OpShiftLeftLogical, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              builder.makeUintConstant(1));
        } else {
          source_sample_id =
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1 << 1));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0),
            dest_tile_pixel_y, builder.makeUintConstant(1),
            builder.makeUintConstant(1));
        source_tile_pixel_y =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1),
            builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
            builder.makeUintConstant(0), builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half =
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id = builder.createTriOp(
            spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                    dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id,
                source_sample_id, builder.makeUintConstant(0),
                builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
              builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half =
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                    dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id,
                dest_tile_pixel_x, builder.makeUintConstant(0),
                builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
              builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint,
                                  dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
              builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
              builder.makeUintConstant(1));
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id,
              dest_tile_pixel_x, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_sample_id, builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                                 source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_sample_id, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_sample_id, builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1),
            builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id =
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                                 source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_sample_id, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
        }
        source_tile_pixel_y =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                  dest_sample_id, builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        }
      }
    }
  }

  // Source coordinates are derived - the host depth source path below needs
  // the destination-space coordinates back.
  dest_tile_pixel_x = dest_space_tile_pixel_x;
  dest_tile_pixel_y = dest_space_tile_pixel_y;

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) +
      uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        source_tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(
                    spv::OpULessThan, builder.makeBoolType(),
                    source_tile_pixel_x,
                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(
                    -int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(
                  spv::OpBitFieldSExtract, type_int,
                  builder.createUnaryOp(spv::OpBitcast, type_int,
                                        address_constant),
                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles = builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint, address_constant,
      builder.makeUintConstant(xenos::kEdramPitchTilesBits),
      builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x = builder.createBinOp(
      spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(source_tile_width_samples >>
                                       source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(
                  source_tile_height_samples >>
                  uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  }
  if (!source_is_multisampled) {
    // Also every sample of a pixel with single-sample storage.
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0],
          builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int,
                                                   source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] =
          builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type =
        builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample =
          source_is_multisampled ? source_sample_ids_int[i] : spv::NoResult;
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false,
          false, source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining =
          source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (xe::bit_scan_forward(source_color_components_remaining,
                                  &source_color_component_index)) {
        source_color_components_remaining &=
            ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] =
            builder.createCompositeExtract(source_color_vec4,
                                           source_color_component_type,
                                           source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample =
          source_is_multisampled ? source_sample_ids_int[i] : spv::NoResult;
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_float4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample =
          source_is_multisampled ? source_sample_ids_int[i] : spv::NoResult;
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_uint4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count =
        source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half =
        builder.createBinOp(spv::OpINotEqual, type_bool, source_color_half,
                            builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type,
          source_color_is_second_half,
          source_color[0][source_color_half_component_count],
          source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type,
            source_color_is_second_half,
            source_color[0][source_color_half_component_count + i],
            source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult &&
      source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
    builder.createStore(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
        output_fragment_stencil_ref);
  }

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (source_color_format ==
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
            // Gamma source stores linear; encode RGB of both 32bpp samples to
            // gamma bytes before packing into the 64bpp destination. Only
            // loaded components are converted.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                if (source_color[i][j] == spv::NoResult) {
                  continue;
                }
                source_color[i][j] = SpirvShaderTranslator::LinearToPWLGamma(
                    &builder, source_color[i][j], true, ext_inst_glsl_std_450);
              }
            }
          }
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              source_color[i][j], unorm_scale),
                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale_rgb),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(
                              spv::OpFMul, type_float, source_color[i][j],
                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j),
                  j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(
                builder, source_color[i][0], ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  SpirvShaderTranslator::UnclampedFloat32To7e3(
                      builder, source_color[i][j], ext_inst_glsl_std_450),
                  builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated = builder.createTriBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(
                        spv::OpFAdd, type_float,
                        builder.createBinOp(spv::OpFMul, type_float,
                                            alpha_saturated, unorm_scale_a),
                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[i][0],
                  source_color[i][1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[i >> 1][i & 1]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[0][i << 1],
                  source_color[0][(i << 1) + 1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[i],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i],
                !depth_float24_convert_in_pixel_shader() &&
                    depth_float24_round(),
                true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                         source_stencil[i], depth24,
                                         depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // Multisampled sampled images are optional in Vulkan, and image views
        // of different formats can't be created separately for sampled image
        // and color attachment usages, so no multisampled integer sampled image
        // support implies no multisampled integer framebuffer attachment
        // support in Xenia.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 =
                builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      } else {
        spv::Id const_uint_0 = builder.makeUintConstant(0);
        spv::Id const_uint_16 = builder.makeUintConstant(16);
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, packed[i >> 1],
              (i & 1) ? const_uint_16 : const_uint_0, const_uint_16));
        }
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            // Same byte layout - only a gamma color space conversion on RGB may
            // be needed. The gamma resource format is stored as linear in the
            // unorm16 host render target, so convert when exactly one side is
            // gamma (alpha is never gamma-encoded).
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              spv::Id component = source_color[0][i];
              if (i < 3 && dest_color_format != source_color_format) {
                if (dest_color_format ==
                    xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                  // Gamma source (linear storage) -> plain dest (gamma bytes).
                  component = SpirvShaderTranslator::LinearToPWLGamma(
                      &builder, component, true, ext_inst_glsl_std_450);
                } else {
                  // Plain source (gamma bytes) -> gamma dest (linear storage).
                  component = SpirvShaderTranslator::PWLGammaToLinear(
                      &builder, component, true, ext_inst_glsl_std_450);
                }
              }
              id_vector_temp.push_back(component);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            if (source_color_format ==
                xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              // Gamma source stores linear; encode RGB to gamma bytes before
              // packing into the raw 32bpp value reinterpreted by the
              // differently-formatted destination. Only loaded components are
              // converted (stencil bit output loads red only).
              for (uint32_t j = 0; j < 3; ++j) {
                if (source_color[0][j] == spv::NoResult) {
                  continue;
                }
                source_color[0][j] = SpirvShaderTranslator::LinearToPWLGamma(
                    &builder, source_color[0][j], true, ext_inst_glsl_std_450);
              }
            }
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
            uint32_t packed_component_offset = 0;
            if (mode.output == TransferOutput::kDepth) {
              // When need only depth, not stencil, skip the red component, and
              // put the depth from GBA directly in the lower bits.
              packed_component_offset = 1;
              packed_only_depth = true;
              if (output_fragment_stencil_ref != spv::NoResult) {
                builder.createStore(
                    builder.createUnaryOp(
                        spv::OpBitcast, type_int,
                        builder.createUnaryOp(
                            spv::OpConvertFToU, type_uint,
                            builder.createBinOp(
                                spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float,
                                                    source_color[0][0],
                                                    unorm_scale),
                                unorm_round_offset))),
                    output_fragment_stencil_ref);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(
                        spv::OpFMul, type_float,
                        source_color[0][packed_component_offset], unorm_scale),
                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id component_width = builder.makeUintConstant(8);
              for (uint32_t i = 1; i < 4 - packed_component_offset; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float,
                                source_color[0][packed_component_offset + i],
                                unorm_scale),
                            unorm_round_offset)),
                    builder.makeUintConstant(8 * i), component_width);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[0][0], unorm_scale_rgb),
                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float, source_color[0][i],
                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i),
                    i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(
                builder, source_color[0][0], ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    SpirvShaderTranslator::UnclampedFloat32To7e3(
                        builder, source_color[0][i], ext_inst_glsl_std_450),
                    builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                  source_color[0][3], builder.makeFloatConstant(0.0f),
                  builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // All 64bpp formats, and all 16 bits per component formats, are
          // represented as integers in ownership transfer for safe handling of
          // NaN encodings and -32768 / -32767.
          // TODO(Triang3l): Handle the case when that's not true (no
          // multisampled sampled images, no 16-bit UNORM, no cross-packing
          // 32bpp aliasing on a portability subset device or a 64bpp format
          // where that wouldn't help anyway).
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 2; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            packed = source_color[0][0];
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id component_offset_width = builder.makeUintConstant(16);
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed, source_color[0][1],
                  component_offset_width, component_offset_width);
            }
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth &&
          dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[0],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0],
                !depth_float24_convert_in_pixel_shader() &&
                    depth_float24_round(),
                true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
              builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    // For stencil bit output, use stencil directly for the discard check.
    if (packed == spv::NoResult && mode.output == TransferOutput::kStencilBit) {
      packed = source_stencil[0];
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              // Same-base 7e3 source -> decode the 7e3 floats to [0, 1] instead
              // of bit-reinterpreting. See IsTransferValueConverted7e3And8888
              // and the D3D12 TransferConvert7e3To8888.
              if (key.value_convert && source_is_color &&
                  (source_color_format ==
                       xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
                   source_color_format ==
                       xenos::ColorRenderTargetFormat::
                           k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
                id_vector_temp.clear();
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp.push_back(builder.createTriBuiltinCall(
                      type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                      SpirvShaderTranslator::Float7e3To32(
                          builder, packed, 10 * i, false,
                          ext_inst_glsl_std_450),
                      builder.makeFloatConstant(0.0f),
                      builder.makeFloatConstant(1.0f)));
                }
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                            packed,
                                            builder.makeUintConstant(30),
                                            builder.makeUintConstant(2))),
                    builder.makeFloatConstant(1.0f / 3.0f)));
                builder.createStore(builder.createCompositeConstruct(
                                        type_fragment_data, id_vector_temp),
                                    output_fragment_data);
                break;
              }
              // For a gamma destination stored as linear in unorm16, the raw
              // EDRAM bytes are reinterpreted as gamma and decoded to the
              // midpoint of each byte's linear range (alpha stays linear). The
              // midpoint survives the unorm16 round-trip so a later re-encode
              // reproduces the byte. Reaching the gamma format implies
              // gamma_render_target_as_unorm16.
              bool is_gamma = dest_color_format ==
                              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                spv::Id byte_uint = builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, packed,
                    builder.makeUintConstant(8 * i), component_width);
                if (is_gamma && i < 3) {
                  id_vector_temp.push_back(
                      GammaByteToLinearMidpoint(builder, byte_uint));
                } else {
                  id_vector_temp.push_back(builder.createBinOp(
                      spv::OpFMul, type_float,
                      builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                            byte_uint),
                      unorm_scale));
                }
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb =
                  builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                            packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::
                k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              if (key.value_convert && source_is_color &&
                  source_color_format ==
                      xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                // Reverse: 8_8_8_8 source -> unpack the unorm bytes to [0, 1].
                // Matches the D3D12 TransferConvert8888To7e3.
                spv::Id component_width = builder.makeUintConstant(8);
                spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
                id_vector_temp.clear();
                for (uint32_t i = 0; i < 4; ++i) {
                  id_vector_temp.push_back(builder.createBinOp(
                      spv::OpFMul, type_float,
                      builder.createUnaryOp(
                          spv::OpConvertUToF, type_float,
                          builder.createTriOp(spv::OpBitFieldUExtract,
                                              type_uint, packed,
                                              builder.makeUintConstant(8 * i),
                                              component_width)),
                      unorm_scale));
                }
                builder.createStore(builder.createCompositeConstruct(
                                        type_fragment_data, id_vector_temp),
                                    output_fragment_data);
                break;
              }
              id_vector_temp.clear();
              // Color.
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                          packed, builder.makeUintConstant(30),
                                          builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              // All 16 bits per component formats are represented as integers
              // in ownership transfer for safe handling of NaN encodings and
              // -32768 / -32767.
              // TODO(Triang3l): Handle the case when that's not true (no
              // multisampled sampled images, no 16-bit UNORM, no cross-packing
              // 32bpp aliasing on a portability subset device or a 64bpp format
              // where that wouldn't help anyway).
              spv::Id component_offset_width = builder.makeUintConstant(16);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, packed,
                    i ? component_offset_width : builder.makeUintConstant(0),
                    component_offset_width));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // However, multisampled sampled images are optional in Vulkan,
              // and image views of different formats can't be created
              // separately for sampled image and color attachment usages, so no
              // multisampled integer sampled image support implies no
              // multisampled integer framebuffer attachment support in Xenia.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 =
                    builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 =
                builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                    guest_depth24, builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >=
                  xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                            dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id,
                        dest_tile_pixel_x, builder.makeUintConstant(0),
                        builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                      builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint,
                                          dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1),
                      builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                      builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                      builder.makeUintConstant(1));
                }
              } else {
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id,
                      dest_tile_pixel_x, builder.makeUintConstant(1),
                      builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples <
                  xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples ==
                      xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id = builder.createBinOp(
                        spv::OpShiftRightLogical, type_uint, dest_sample_id,
                        builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id =
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              host_depth_source_sample_id,
                                              builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id =
                          builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                               host_depth_source_sample_id,
                                               host_depth_source_sample_id,
                                               builder.makeUintConstant(1),
                                               builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                            dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples ==
                      xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id = builder.createBinOp(
                        spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                        builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id =
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              host_depth_source_sample_id,
                                              builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id =
                          builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                               host_depth_source_sample_id,
                                               host_depth_source_sample_id,
                                               builder.makeUintConstant(1),
                                               builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y = builder.createBinOp(
                        spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                        builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical,
                                              type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(builder.makeIntConstant(
                int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant = builder.createLoad(
                builder.createAccessChain(spv::StorageClassPushConstant,
                                          push_constants, id_vector_temp),
                spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int,
                                              dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(
                                xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(
                                xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles = builder.createTriOp(
                spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y = builder.createBinOp(
                spv::OpUDiv, type_uint, host_depth_source_tile_index,
                host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x = builder.createBinOp(
                spv::OpUMod, type_uint, host_depth_source_tile_index,
                host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(
                                            dest_tile_width_samples >>
                                            uint32_t(key.source_msaa_samples >=
                                                     xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(
                                            dest_tile_height_samples >>
                                            uint32_t(key.source_msaa_samples >=
                                                     xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters
                host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X &&
                !msaa_single_sample_storage_) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int,
                                        host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod =
                  builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false,
                                          true, false, false, false,
                                          host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples ==
                        xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id,
                    dest_tile_pixel_x, builder.makeUintConstant(1),
                    builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp(
                      (key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                       msaa_2x_attachments_supported_)
                          ? spv::OpBitwiseXor
                          : spv::OpShiftRightLogical,
                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1),
                  builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(dest_tile_width_samples *
                                             dest_tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(dest_tile_width_samples),
                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(builder.createUnaryOp(
                spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform,
                                              host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(
                            spv::OpFMul, type_float, host_depth32,
                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32,
                    !depth_float24_convert_in_pixel_shader() &&
                        depth_float24_round(),
                    true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated = builder.createBinOp(
                spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry =
                builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block =
                  builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge,
                                         spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated,
                                            &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical,
                                              type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(
                  builder, guest_depth24, 0, true, false,
                  ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
            spv::Id depth24_to_depth32_result_block_id =
                builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 =
                builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth &&
              output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(
                    spv::OpBitcast, type_int,
                    builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                        builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (packed && !cvars::no_discard_stencil_in_transfer_pipelines) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(
              int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant = builder.createLoad(
              builder.createAccessChain(spv::StorageClassPushConstant,
                                        push_constants, id_vector_temp),
              spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                      stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function,
                             spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer shader 0x{:08X}",
        key.key);
  }
  transfer_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkPipeline VulkanRenderTargetCache::GetResolveInPassPipeline(
    RenderPassKey render_pass_key, uint32_t color_slot, bool is_64bpp) {
  uint64_t key = uint64_t(render_pass_key.key) | (uint64_t(color_slot) << 32) |
                 (uint64_t(is_64bpp) << 34);
  auto it = resolve_inpass_pipelines_.find(key);
  if (it != resolve_inpass_pipelines_.end()) {
    return it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = resolve_inpass_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  bool source_is_multisampled =
      render_pass_key.msaa_samples != xenos::MsaaSamples::k1X;
  shader_stages[1] = shader_stages[0];
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  if (msaa_single_sample_storage_ && source_is_multisampled) {
    // The precompiled shaders read a multisampled input attachment.
    return VK_NULL_HANDLE;
  }
  const bool source_is_r32_uint =
      render_pass_key.color_rts_view_mode ==
      kColorRTViewModeR32UintFirst + color_slot;
  if (source_is_r32_uint) {
    if (is_64bpp || source_is_multisampled) {
      return VK_NULL_HANDLE;
    }
    shader_stages[1].module = resolve_inpass_r32_uint_shaders_[0];
  } else {
    shader_stages[1].module =
        resolve_inpass_shaders_[uint32_t(is_64bpp) |
                                (uint32_t(source_is_multisampled) << 1)];
  }
  if (shader_stages[1].module == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 0;
  vertex_input_state.pVertexBindingDescriptions = nullptr;
  vertex_input_state.vertexAttributeDescriptionCount = 0;
  vertex_input_state.pVertexAttributeDescriptions = nullptr;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  uint32_t pass_sample_count =
      msaa_single_sample_rasterization()
          ? 1
          : uint32_t(1) << uint32_t(render_pass_key.msaa_samples);
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (pass_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(pass_sample_count);

  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  // No attachment output - the resolved data leaves through the fragment
  // shader's storage buffer writes.
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - xe::lzcnt(render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;

  std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                  VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = uint32_t(dynamic_states.size());
  dynamic_state.pDynamicStates = dynamic_states.data();

  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
  if (render_pass_key.depth_and_color_used & 0b1) {
    depth_attachment_format = GetDepthVulkanFormat(render_pass_key.depth_format);
  }
  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(render_pass_key.depth_and_color_used & (1 << (1 + i)))) {
      continue;
    }
    color_attachment_formats[i] =
        GetColorRenderPassVulkanFormat(render_pass_key, i);
    color_attachment_count = i + 1;
  }
  uint32_t input_attachment_indices[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    input_attachment_indices[i] =
        i == color_slot ? 0 : VK_ATTACHMENT_UNUSED;
  }
  VkRenderingInputAttachmentIndexInfo input_attachment_index_info;
  input_attachment_index_info.sType =
      VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO;
  input_attachment_index_info.pNext = nullptr;
  input_attachment_index_info.colorAttachmentCount = color_attachment_count;
  input_attachment_index_info.pColorAttachmentInputIndices =
      input_attachment_indices;
  input_attachment_index_info.pDepthInputAttachmentIndex = nullptr;
  input_attachment_index_info.pStencilInputAttachmentIndex = nullptr;
  VkPipelineRenderingCreateInfo pipeline_rendering_create_info;
  pipeline_rendering_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  pipeline_rendering_create_info.pNext = &input_attachment_index_info;
  pipeline_rendering_create_info.viewMask = 0;
  pipeline_rendering_create_info.colorAttachmentCount = color_attachment_count;
  pipeline_rendering_create_info.pColorAttachmentFormats =
      color_attachment_count ? color_attachment_formats : nullptr;
  pipeline_rendering_create_info.depthAttachmentFormat =
      depth_attachment_format;
  pipeline_rendering_create_info.stencilAttachmentFormat =
      depth_attachment_format;

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = &pipeline_rendering_create_info;
  pipeline_create_info.flags = 0;
  pipeline_create_info.stageCount = uint32_t(xe::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = resolve_inpass_pipeline_layout_;
  pipeline_create_info.renderPass = VK_NULL_HANDLE;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                    &pipeline_create_info, nullptr,
                                    &pipeline) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the in-pass resolve "
        "pipeline for render pass key 0x{:08X} slot {}",
        render_pass_key.key, color_slot);
    pipeline = VK_NULL_HANDLE;
  }
  resolve_inpass_pipelines_.emplace(key, pipeline);
  return pipeline;
}

VkPipeline const* VulkanRenderTargetCache::GetTransferPipelines(
    TransferPipelineKey key) {
  auto pipeline_it = transfer_pipelines_.find(key);
  if (pipeline_it != transfer_pipelines_.end()) {
    return pipeline_it->second[0] != VK_NULL_HANDLE ? pipeline_it->second.data()
                                                    : nullptr;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  bool use_dynamic_rendering =
      cvars::vulkan_dynamic_rendering && device_properties.dynamicRendering;

  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!use_dynamic_rendering) {
    render_pass = GetHostRenderTargetsRenderPass(key.render_pass_key);
    if (render_pass == VK_NULL_HANDLE) {
      transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
      return nullptr;
    }
  }

  VkShaderModule fragment_shader_module = GetTransferShader(key.shader_key);
  if (fragment_shader_module == VK_NULL_HANDLE) {
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }

  const TransferModeInfo& mode = kTransferModes[size_t(key.shader_key.mode)];

  // One rasterized sample per pixel with single-sample storage: the shader's
  // destination sample is then always sample 0.
  uint32_t dest_sample_count =
      msaa_single_sample_rasterization()
          ? 1
          : uint32_t(1) << uint32_t(key.shader_key.dest_msaa_samples);
  bool dest_is_masked_sample =
      dest_sample_count > 1 && !device_properties.sampleRateShading;

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = transfer_passthrough_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].pNext = nullptr;
  shader_stages[1].flags = 0;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = fragment_shader_module;
  shader_stages[1].pName = "main";
  shader_stages[1].pSpecializationInfo = nullptr;
  VkSpecializationMapEntry sample_id_specialization_map_entry;
  uint32_t sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(uint32_t);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries =
        &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize =
        sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(float) * 2;
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attribute;
  vertex_input_attribute.location = 0;
  vertex_input_attribute.binding = 0;
  vertex_input_attribute.format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attribute.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount = 1;
  vertex_input_state.pVertexAttributeDescriptions = &vertex_input_attribute;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  // Dynamic, to stay within maxViewportDimensions while preferring a
  // power-of-two factor for converting from pixel coordinates to NDC for exact
  // precision.
  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  // For samples other than the first, will be changed for the pipelines for
  // other samples.
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (dest_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(dest_sample_count);
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
        // supported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Whether the depth / stencil state is used depends on the presence of a
  // depth attachment in the render pass - but not making assumptions about
  // whether the render pass contains any specific attachments, so setting up
  // valid depth / stencil state unconditionally.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp = cvars::depth_transfer_not_equal_test
                                             ? VK_COMPARE_OP_NOT_EQUAL
                                             : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->extensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by stencil
    // being different.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    // Will be dynamic for stencil bit output.
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Whether the color blend state is used depends on the presence of color
  // attachments in the render pass - but not making assumptions about whether
  // the render pass contains any specific attachments, so setting up valid
  // color blend state unconditionally.
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - xe::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    assert_true(device_properties.independentBlend);
    color_blend_attachments[key.shader_key.dest_color_rt_index].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }

  std::array<VkDynamicState, 3> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (mode.output == TransferOutput::kStencilBit) {
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }

  // For VK_KHR_dynamic_rendering: set up VkPipelineRenderingCreateInfo.
  VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {};
  VkFormat color_attachment_formats[xenos::kMaxColorRenderTargets] = {};
  VkFormat depth_attachment_format = VK_FORMAT_UNDEFINED;
  VkFormat stencil_attachment_format = VK_FORMAT_UNDEFINED;
  if (use_dynamic_rendering) {
    pipeline_rendering_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipeline_rendering_create_info.pNext = nullptr;
    pipeline_rendering_create_info.viewMask = 0;

    if (key.render_pass_key.depth_and_color_used & 0b1) {
      depth_attachment_format =
          GetDepthVulkanFormat(key.render_pass_key.depth_format);
      stencil_attachment_format = depth_attachment_format;
    }
    // When reusing the guest render pass for in-pass transfers, the pass may
    // have multiple color attachments; build a format for every used color RT.
    uint32_t color_attachment_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(key.render_pass_key.depth_and_color_used & (1 << (1 + i)))) {
        continue;
      }
      color_attachment_formats[i] = GetColorRenderPassVulkanFormat(
          key.render_pass_key, i);
      color_attachment_count = i + 1;
    }
    pipeline_rendering_create_info.colorAttachmentCount = color_attachment_count;
    pipeline_rendering_create_info.pColorAttachmentFormats =
        color_attachment_count ? color_attachment_formats : nullptr;
    pipeline_rendering_create_info.depthAttachmentFormat =
        depth_attachment_format;
    pipeline_rendering_create_info.stencilAttachmentFormat =
        stencil_attachment_format;
  }

  std::array<VkPipeline, 4> pipelines{};
  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext =
      use_dynamic_rendering ? &pipeline_rendering_create_info : nullptr;
  pipeline_create_info.flags = 0;
  if (dest_is_masked_sample) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  }
  pipeline_create_info.stageCount = uint32_t(xe::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      transfer_pipeline_layouts_[size_t(mode.pipeline_layout)];
  pipeline_create_info.renderPass = render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                    &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
        key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    assert_true(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags = (pipeline_create_info.flags &
                                  ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
                                 VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (uint32_t i = 1; i < dest_sample_count; ++i) {
      // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
      // supported.
      uint32_t host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1)
              ? 3
              : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = uint32_t(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline for render pass 0x{:08X}, shader "
            "0x{:08X}, sample {}",
            key.render_pass_key.key, key.shader_key.key, i);
        for (uint32_t j = 0; j < i; ++j) {
          dfn.vkDestroyPipeline(device, pipelines[j], nullptr);
        }
        transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
        return nullptr;
      }
    }
  }
  return transfer_pipelines_.emplace(key, pipelines).first->second.data();
}

VkClearAttachment VulkanRenderTargetCache::MakeResolveClearAttachment(
    VulkanRenderTarget& dest_vulkan_rt, uint64_t clear_value) {
  const RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
  VkClearAttachment resolve_clear_attachment;
  resolve_clear_attachment.colorAttachment = 0;
  std::memset(&resolve_clear_attachment.clearValue, 0,
              sizeof(resolve_clear_attachment.clearValue));
  const bool clear_depth_only =
      dest_rt_key.is_depth && (clear_value & kResolveClearDepthOnlyBit);
  const bool clear_stencil_only =
      dest_rt_key.is_depth && (clear_value & kResolveClearStencilOnlyBit);
  if (dest_rt_key.is_depth) {
    resolve_clear_attachment.aspectMask =
        clear_depth_only
            ? VK_IMAGE_ASPECT_DEPTH_BIT
            : (clear_stencil_only ? VK_IMAGE_ASPECT_STENCIL_BIT
                                  : VK_IMAGE_ASPECT_DEPTH_BIT |
                                        VK_IMAGE_ASPECT_STENCIL_BIT);
    uint32_t depth_guest_clear_value =
        (uint32_t(clear_value) >> 8) & 0xFFFFFF;
    switch (dest_rt_key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8:
        resolve_clear_attachment.clearValue.depthStencil.depth =
            xenos::UNorm24To32(depth_guest_clear_value);
        break;
      case xenos::DepthRenderTargetFormat::kD24FS8:
        // Taking [0, 2) -> [0, 1) remapping into account.
        resolve_clear_attachment.clearValue.depthStencil.depth =
            xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
        break;
    }
    resolve_clear_attachment.clearValue.depthStencil.stencil =
        uint32_t(clear_value) & 0xFF;
    if (!clear_depth_only && (uint32_t(clear_value) & 0xFF) &
        ~dest_vulkan_rt.stencil_maybe_set_bits()) {
      static uint32_t stencil_bits_clear_logs = 0;
      if (stencil_bits_clear_logs < 24) {
        ++stencil_bits_clear_logs;
        XELOGI("StencilBits: clear adds 0x{:02X} to depth RT @{}",
               uint32_t(clear_value) & 0xFF, uint32_t(dest_rt_key.base_tiles));
      }
    }
    if (!clear_depth_only) {
      dest_vulkan_rt.AddStencilMaybeSetBits(uint32_t(clear_value) & 0xFF);
    }
  } else {
    resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (dest_rt_key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        for (uint32_t j = 0; j < 4; ++j) {
          resolve_clear_attachment.clearValue.color.float32[j] =
              ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
        }
        if (dest_rt_key.GetColorFormat() ==
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
          // Stored as linear in the unorm16 host render target.
          for (uint32_t j = 0; j < 3; ++j) {
            resolve_clear_attachment.clearValue.color.float32[j] =
                xenos::PWLGammaToLinear(
                    resolve_clear_attachment.clearValue.color.float32[j]);
          }
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        for (uint32_t j = 0; j < 3; ++j) {
          resolve_clear_attachment.clearValue.color.float32[j] =
              ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
        }
        resolve_clear_attachment.clearValue.color.float32[3] =
            ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::
          k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        for (uint32_t j = 0; j < 3; ++j) {
          resolve_clear_attachment.clearValue.color.float32[j] =
              xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
        }
        resolve_clear_attachment.clearValue.color.float32[3] =
            ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
        // Using uint for transfers and clears of both. Disregarding the
        // current -32...32 vs. -1...1 settings for consistency with color
        // clear via depth aliasing.
        // TODO(Triang3l): Handle cases of unsupported multisampled 16_UINT
        // and completely unsupported 16_UNORM.
        for (uint32_t j = 0; j < 2; ++j) {
          resolve_clear_attachment.clearValue.color.uint32[j] =
              uint32_t(clear_value >> (j * 16)) & 0xFFFF;
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        // Using uint for transfers and clears of both. Disregarding the
        // current -32...32 vs. -1...1 settings for consistency with color
        // clear via depth aliasing.
        // TODO(Triang3l): Handle cases of unsupported multisampled 16_UINT
        // and completely unsupported 16_UNORM.
        for (uint32_t j = 0; j < 4; ++j) {
          resolve_clear_attachment.clearValue.color.uint32[j] =
              uint32_t(clear_value >> (j * 16)) & 0xFFFF;
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
        // Using uint for proper denormal and NaN handling.
        resolve_clear_attachment.clearValue.color.uint32[0] =
            uint32_t(clear_value);
      } break;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        // Using uint for proper denormal and NaN handling.
        resolve_clear_attachment.clearValue.color.uint32[0] =
            uint32_t(clear_value);
        resolve_clear_attachment.clearValue.color.uint32[1] =
            uint32_t(clear_value >> 32);
      } break;
    }
  }
  return resolve_clear_attachment;
}

bool VulkanRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    bool in_current_render_pass, bool stencil_cutout_separate,
    const Transfer::Rectangle* stencil_cutout) {
  bool complete = true;
  SCOPE_profile_cpu_f("gpu");
  assert_true(GetPath() == Path::kHostRenderTargets);
  // The cutout of the stencil bit draws and of the stencil clear before them.
  const Transfer::Rectangle* const stencil_rectangle_cutout =
      stencil_cutout_separate ? stencil_cutout : resolve_clear_rectangle;

  // vulkan_lazy_constant_rows: what these transfers and clears read or write
  // must hold its real values first (in a guest pass, the pass already
  // recorded its targets' pending rows and in-pass transfers refuse sources
  // with any).
  if (!pending_constant_rows_.empty() && !in_current_render_pass) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (!render_targets[i]) continue;
      if (HasPendingConstantRows(render_targets[i])) {
        MaterializePendingConstantRows(render_targets[i], 0);
      }
      if (!render_target_transfers) continue;
      for (const Transfer& transfer : render_target_transfers[i]) {
        for (RenderTarget* source :
             {transfer.source, transfer.host_depth_source}) {
          if (source && HasPendingConstantRows(source)) {
            MaterializePendingConstantRows(source, 1);
          }
        }
      }
    }
  }

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  // In the guest pass only the deferred depth clear (slot 0) is cleared.
  assert_false(in_current_render_pass && resolve_clear_needed &&
               (render_target_count != 1 || !render_targets[0] ||
                !render_targets[0]->key().is_depth));

  // Check if there's any actual work to do before pushing debug marker.
  bool has_transfers = false;
  for (uint32_t i = 0; i < render_target_count && !has_transfers; ++i) {
    if (render_targets[i] &&
        (!render_target_transfers[i].empty() || resolve_clear_needed)) {
      has_transfers = true;
    }
  }
  if (!has_transfers) {
    return true;
  }

  if (command_processor_.deferred_resolve_pending_) {
    command_processor_.FlushDeferredResolve(
        resolve_clear_needed ? VulkanCommandProcessor::ResolveDeferStop::kClear
                             : VulkanCommandProcessor::ResolveDeferStop::kResource);
    in_current_render_pass = false;
  }

  if (cvars::log_in_pass_transfer_rejects > 0 && !in_current_render_pass) {
    Dl_info info;
    const uintptr_t base =
        dladdr(reinterpret_cast<void*>(&cvars::log_in_pass_transfer_rejects),
               &info)
            ? reinterpret_cast<uintptr_t>(info.dli_fbase)
            : 0;
    const uint32_t caller = uint32_t(
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - base);
    for (auto& slot : own_pass_transfer_callers_) {
      if (slot.first == caller || !slot.second) {
        slot.first = caller;
        ++slot.second;
        break;
      }
    }
  }
  command_processor_.PushDebugMarker("PerformTransfersAndResolveClears");

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  VkClearRect resolve_clear_rect;
  if (resolve_clear_needed) {
    // All render targets of one resolve clear share the pitch and thus the
    // scale class - take the scale from whichever is there.
    uint32_t resolve_clear_scale_x = draw_resolution_scale_x();
    uint32_t resolve_clear_scale_y = draw_resolution_scale_y();
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (render_targets[i]) {
        resolve_clear_scale_x = GetKeyScaleX(render_targets[i]->key());
        resolve_clear_scale_y = GetKeyScaleY(render_targets[i]->key());
        break;
      }
    }
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    resolve_clear_rect.rect.offset.x =
        int32_t(resolve_clear_rectangle->x_pixels * resolve_clear_scale_x);
    resolve_clear_rect.rect.offset.y =
        int32_t(resolve_clear_rectangle->y_pixels * resolve_clear_scale_y);
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * resolve_clear_scale_x;
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * resolve_clear_scale_y;
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  // Skipped for in-pass transfers - the barriers and host-depth store run
  // outside the active render pass, before it was entered.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; !in_current_render_pass && i < render_target_count;
       ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      assert_false(dest_rt_key.scale_native);
      if (!host_depth_store_set_up) {
        // Pipeline.
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        // Descriptor set bindings.
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_,
            0, uint32_t(xe::countof(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        // Render target constant.
        HostDepthStoreRenderTargetConstant
            host_depth_store_render_target_constant =
                GetHostDepthStoreRenderTargetConstant(
                    dest_rt_key.pitch_tiles_at_32bpp,
                    msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, SHADER_READ -> DEPTH_STENCIL_ATTACHMENT_WRITE will be
        // done anyway even in the best case, so it's not possible to have all
        // the barriers in one place here.
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Always transitioning both depth and stencil, not storing separate
        // usage flags for depth and stencil.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.current_layout(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle
          transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp,
          dest_rt_key.msaa_samples, false, transfer_rectangles,
          resolve_clear_rectangle);
      // With separate aspect cutouts, the depth part may be covered entirely.
      assert_true(transfer_rectangle_count || stencil_cutout_separate);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(
            transfer_rectangles[j], dest_rt_key.msaa_samples,
            host_depth_store_rectangle_constant, group_count_x, group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant),
            &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        const auto& byte_rect = transfer_rectangles[j];
        const uint64_t byte_pixels = uint64_t(byte_rect.width_pixels) * byte_rect.height_pixels *
            GetKeyScaleX(dest_rt_key) * GetKeyScaleY(dest_rt_key);
        NotePassBytesRead(command_buffer, &dest_vulkan_rt, byte_pixels, false,
                          byte_pixels * 4 * (1u << uint32_t(dest_rt_key.msaa_samples)));
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1,
                                     DeferredCommandBuffer::DispatchSite::kHostDepthStore);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  // For in-pass transfers the destination and source barriers were already
  // issued before entering the pass (PreparePendingDrawPassTransferBarriers).
  for (uint32_t i = 0; !in_current_render_pass && i < render_target_count;
       ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt ||
            previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask,
                                  &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_vulkan_rt.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask,
                              dest_new_layout);
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : dest_transfers) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask,
                                  kSourceLayout);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            host_depth_source_vulkan_rt.current_access_mask(),
            kSourceAccessMask, host_depth_source_vulkan_rt.current_layout(),
            kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask,
                                             kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    UseEdramBuffer(EdramBufferUsage::kFragmentRead);
  }

  // Perform the transfers and clears.

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  uint32_t transfer_descriptor_sets_bound = 0;
  uint32_t transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures =
      VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();

    // Late barriers in case there was cross-copying that prevented merging of
    // barriers. For in-pass transfers the destination is already in its draw
    // state inside the active pass, so no barrier is issued.
    if (!in_current_render_pass) {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask,
                                  &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_rt_key.is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask,
                              dest_new_layout);
    }

    // Get the objects needed for transfers to the destination. When merging
    // into the active guest pass, reuse it (and its framebuffer / dest view)
    // directly. Otherwise build a transfer-specific single-attachment pass.
    RenderPassKey transfer_render_pass_key;
    VkRenderPass transfer_render_pass = VK_NULL_HANDLE;
    const Framebuffer* transfer_framebuffer = nullptr;
    VkImageView transfer_dest_view = VK_NULL_HANDLE;
    bool transfer_dest_view_is_integer = false;
    if (in_current_render_pass) {
      transfer_render_pass_key = last_update_render_pass_key_;
      transfer_render_pass = last_update_render_pass_;
      transfer_framebuffer = last_update_framebuffer_;
    } else {
      transfer_render_pass_key.msaa_samples = dest_rt_key.msaa_samples;
      if (dest_rt_key.is_depth) {
        transfer_render_pass_key.depth_and_color_used = 0b1;
        transfer_render_pass_key.depth_format = dest_rt_key.GetDepthFormat();
      } else {
        transfer_render_pass_key.depth_and_color_used = 0b1 << 1;
        transfer_render_pass_key.color_0_view_format =
            dest_rt_key.GetColorFormat();
        transfer_render_pass_key.color_rts_view_mode =
            kColorRTViewModeTransferAll;
      }
      transfer_render_pass =
          GetHostRenderTargetsRenderPass(transfer_render_pass_key);
      if (transfer_render_pass == VK_NULL_HANDLE) {
        complete = false;
        continue;
      }
      const RenderTarget* transfer_framebuffer_render_targets
          [1 + xenos::kMaxColorRenderTargets] = {};
      transfer_framebuffer_render_targets[dest_rt_key.is_depth ? 0 : 1] =
          dest_rt;
      transfer_framebuffer = GetHostRenderTargetsFramebuffer(
          transfer_render_pass_key, dest_rt_key.pitch_tiles_at_32bpp,
          transfer_framebuffer_render_targets);
      if (!transfer_framebuffer) {
        complete = false;
        continue;
      }
      // Get the view for dynamic rendering (used for both transfers and
      // clears).
      transfer_dest_view = dest_rt_key.is_depth
                               ? dest_vulkan_rt.view_depth_stencil()
                               : dest_vulkan_rt.view_color_transfer();
      if (!dest_rt_key.is_depth) {
        GetColorOwnershipTransferVulkanFormat(dest_rt_key.GetColorFormat(),
                                              &transfer_dest_view_is_integer);
      }
      // Don't enter the render pass immediately - may still insert source
      // barriers later.
    }

    if (!current_transfers.empty()) {
      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // The destination may now hold any stencil bit its sources may hold -
      // all of them when a color render target is reinterpreted as stencil.
      if (dest_rt_key.is_depth) {
        for (const Transfer& transfer : current_transfers) {
          uint32_t source_bits =
              (transfer.source->key().is_depth
                   ? transfer.source->stencil_maybe_set_bits()
                   : 0xFF) &
              GetEdramStencilMaybeSetBits(transfer.start_tiles,
                                          transfer.end_tiles);
          if (source_bits & ~dest_vulkan_rt.stencil_maybe_set_bits()) {
            static uint32_t stencil_bits_transfer_logs = 0;
            if (stencil_bits_transfer_logs < 48) {
              ++stencil_bits_transfer_logs;
              XELOGI(
                  "StencilBits: transfer from {} RT @{} adds 0x{:02X} to "
                  "depth RT @{} (had 0x{:02X})",
                  transfer.source->key().is_depth ? "depth" : "color",
                  uint32_t(transfer.source->key().base_tiles),
                  source_bits & ~dest_vulkan_rt.stencil_maybe_set_bits(),
                  uint32_t(dest_rt_key.base_tiles),
                  dest_vulkan_rt.stencil_maybe_set_bits());
            }
          }
          dest_vulkan_rt.AddStencilMaybeSetBits(source_bits);
        }
      }

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws =
          dest_rt_key.is_depth &&
          !vulkan_device->extensions().ext_EXT_shader_stencil_export &&
        (stencil_test_ever_enabled_ ||
         !cvars::rt_cache_skip_unused_stencil_transfers);
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(
          current_transfers.size() << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      // In the reused guest pass the color output location is the RT's own
      // index; the standalone transfer pass always has a single attachment.
      new_transfer_shader_key.dest_color_rt_index =
          dest_rt_key.is_depth || !in_current_render_pass ? 0 : i - 1;
      new_transfer_shader_key.dest_resource_format =
          dest_rt_key.resource_format;
      new_transfer_shader_key.dest_scale_native = dest_rt_key.scale_native;
      uint32_t stencil_clear_rectangle_count = 0;
      // vulkan_stencil_transfer_copy: the stencil of k_8_8_8_8 sources at the
      // same base and pitch is computed and copied after the pass instead.
      stencil_copy_transfers_.clear();
      VulkanCommandProcessor::ScratchBufferAcquisition stencil_copy_scratch;
      VkDeviceSize stencil_copy_size = 0;
      if (need_stencil_bit_draws && cvars::vulkan_stencil_transfer_copy &&
          !in_current_render_pass && dest_vulkan_rt.stencil_copy_destination &&
          dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X) {
        for (const Transfer& transfer : current_transfers) {
          const RenderTargetKey source_key = transfer.source->key();
          if (source_key.is_depth ||
              source_key.GetColorFormat() !=
                  xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
              source_key.msaa_samples != xenos::MsaaSamples::k1X ||
              source_key.base_tiles != dest_rt_key.base_tiles ||
              source_key.pitch_tiles_at_32bpp !=
                  dest_rt_key.pitch_tiles_at_32bpp ||
              GetKeyScaleX(source_key) != 1 || GetKeyScaleY(source_key) != 1 ||
              (transfer.constant_mask & 0xFF) == 0xFF) {
            continue;
          }
          Transfer::Rectangle copy_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t copy_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles,
              dest_rt_key.msaa_samples, false, copy_rectangles,
              stencil_rectangle_cutout);
          bool aligned = copy_rectangle_count != 0;
          VkDeviceSize transfer_size = 0;
          for (uint32_t k = 0; k < copy_rectangle_count; ++k) {
            aligned = aligned &&
                      !((copy_rectangles[k].x_pixels |
                         copy_rectangles[k].width_pixels) & 3);
            transfer_size += VkDeviceSize(copy_rectangles[k].width_pixels) *
                             copy_rectangles[k].height_pixels;
          }
          if (!aligned) {
            continue;
          }
          stencil_copy_transfers_.push_back(transfer);
          stencil_copy_size += transfer_size;
        }
        if (!stencil_copy_transfers_.empty()) {
          if (EnsureStencilCopyPipeline()) {
            stencil_copy_scratch = command_processor_.AcquireScratchGpuBuffer(
                stencil_copy_size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
          }
          if (stencil_copy_scratch.buffer() == VK_NULL_HANDLE) {
            stencil_copy_transfers_.clear();
          }
        }
      }
      auto is_stencil_copied = [this](const Transfer& transfer) {
        for (const Transfer& copied : stencil_copy_transfers_) {
          if (copied.source == transfer.source &&
              copied.start_tiles == transfer.start_tiles &&
              copied.end_tiles == transfer.end_tiles) {
            return true;
          }
        }
        return false;
      };
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (uint32_t transfer_index = 0;
             transfer_index < current_transfers.size(); ++transfer_index) {
          const Transfer& transfer = current_transfers[transfer_index];
          assert_not_null(transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr
                : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->temporary_sort_index() ==
                  UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.temporary_sort_index() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          new_transfer_shader_key.source_msaa_samples =
              source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format =
              source_rt_key.resource_format;
          new_transfer_shader_key.value_convert =
              IsTransferValueConverted7e3And8888(source_rt_key, dest_rt_key);
          new_transfer_shader_key.source_scale_native =
              source_rt_key.scale_native;
          assert_true(!host_depth_source_vulkan_rt ||
                      host_depth_source_vulkan_rt->key().scale_native ==
                          dest_rt_key.scale_native);
          bool host_depth_source_is_copy =
              host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          new_transfer_shader_key.mode = GetTransferMode(
              j != 0, dest_rt_key.is_depth, source_rt_key.is_depth,
              host_depth_source_vulkan_rt != nullptr, host_depth_source_is_copy);
          if (j && is_stencil_copied(transfer)) {
            continue;
          }
          // The part of this aspect outside its cutout - none left with
          // separate aspect cutouts: nothing to draw for this aspect.
          uint32_t aspect_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles,
              dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
              j ? stencil_rectangle_cutout : resolve_clear_rectangle);
          if (!aspect_rectangle_count) {
            assert_true(stencil_cutout_separate);
            continue;
          }
          if (j) {
            stencil_clear_rectangle_count += aspect_rectangle_count;
          }
          current_transfer_invocations_.emplace_back(transfer,
                                                     new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source =
                nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(),
                current_transfer_invocations_.end());

      // For in-pass transfers, the source barriers were already issued before
      // the active render pass was entered (PreparePendingDrawPassTransferBarriers),
      // and the pass can't be (re)entered here.
      if (!in_current_render_pass) {
        for (auto it = current_transfer_invocations_.cbegin();
             it != current_transfer_invocations_.cend(); ++it) {
          assert_not_null(it->transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(it->transfer.source);
          command_processor_.PushImageMemoryBarrier(
              source_vulkan_rt.image(),
              ui::vulkan::util::InitializeSubresourceRange(
                  source_vulkan_rt.key().is_depth
                      ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                         VK_IMAGE_ASPECT_STENCIL_BIT)
                      : VK_IMAGE_ASPECT_COLOR_BIT),
              source_vulkan_rt.current_stage_mask(), kSourceStageMask,
              source_vulkan_rt.current_access_mask(), kSourceAccessMask,
              source_vulkan_rt.current_layout(), kSourceLayout);
          source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask,
                                    kSourceLayout);
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            TransferShaderKey transfer_shader_key = it->shader_key;
            if (transfer_shader_key.mode ==
                    TransferMode::kDepthAndHostDepthCopyToDepth ||
                transfer_shader_key.mode ==
                    TransferMode::kColorAndHostDepthCopyToDepth) {
              // Reading copied host depth from the EDRAM buffer.
              UseEdramBuffer(EdramBufferUsage::kFragmentRead);
            } else {
              // Reading host depth from the texture.
              command_processor_.PushImageMemoryBarrier(
                  host_depth_source_vulkan_rt->image(),
                  ui::vulkan::util::InitializeSubresourceRange(
                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
                  host_depth_source_vulkan_rt->current_stage_mask(),
                  kSourceStageMask,
                  host_depth_source_vulkan_rt->current_access_mask(),
                  kSourceAccessMask,
                  host_depth_source_vulkan_rt->current_layout(), kSourceLayout);
              host_depth_source_vulkan_rt->SetUsage(
                  kSourceStageMask, kSourceAccessMask, kSourceLayout);
            }
          }
        }
      }

      // Perform the transfers for the render target.

      if (!in_current_render_pass) {
        command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
            transfer_render_pass, transfer_framebuffer, transfer_dest_view,
            dest_rt_key.is_depth,
            GetMsrtssSampleCount(dest_rt_key.msaa_samples),
            transfer_dest_view_is_integer);
      }

      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                  stencil_clear_rectangle_count,
                                                  stencil_clear_rect_write_ptr);
        VkClearRect* const stencil_clear_rects = stencil_clear_rect_write_ptr;
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (uint32_t transfer_index = 0;
             transfer_index < current_transfers.size(); ++transfer_index) {
          const Transfer& transfer = current_transfers[transfer_index];
          Transfer::Rectangle transfer_stencil_clear_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count =
              transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles,
                                     dest_rt_key.msaa_samples, dest_is_64bpp,
                                     transfer_stencil_clear_rectangles,
                                     stencil_rectangle_cutout);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count;
               ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x = int32_t(
                stencil_clear_rectangle.x_pixels * GetKeyScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->rect.offset.y = int32_t(
                stencil_clear_rectangle.y_pixels * GetKeyScaleY(dest_rt_key));
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels *
                GetKeyScaleX(dest_rt_key);
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels *
                GetKeyScaleY(dest_rt_key);
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
        if (cvars::vulkan_transfer_scissor_bounds) {
          command_buffer.AccountEmplacedClearRects(stencil_clear_rects,
                                                   stencil_clear_rectangle_count);
        }
      }

      // Prefer power of two viewports for exact division by simply biasing the
      // exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          float(std::min(xe::next_pow2(transfer_framebuffer->host_extent.width),
                         vulkan_device->properties().maxViewportDimensions[0]));
      transfer_viewport.height = float(
          std::min(xe::next_pow2(transfer_framebuffer->host_extent.height),
                   vulkan_device->properties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      // GetRectangles returns guest pixels - scale to the destination's host
      // pixels.
      float pixels_to_ndc_x =
          2.0f / transfer_viewport.width * GetKeyScaleX(dest_rt_key);
      float pixels_to_ndc_y =
          2.0f / transfer_viewport.height * GetKeyScaleY(dest_rt_key);
      // Only the transferred rectangles are drawn: scissoring to their bounds
      // changes no pixel, but lets the pass's render area shrink around them
      // instead of covering the whole (up to 8192-row) render target.
      uint32_t bound_x0 = UINT32_MAX, bound_y0 = UINT32_MAX;
      uint32_t bound_x1 = 0, bound_y1 = 0;
      for (const Transfer& transfer : current_transfers) {
        for (const Transfer::Rectangle* cutout :
             {resolve_clear_rectangle, stencil_rectangle_cutout}) {
          Transfer::Rectangle rects[Transfer::kMaxRectanglesWithCutout];
          const uint32_t count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples,
              dest_is_64bpp, rects, cutout);
          for (uint32_t j = 0; j < count; ++j) {
            bound_x0 = std::min(bound_x0, rects[j].x_pixels);
            bound_y0 = std::min(bound_y0, rects[j].y_pixels);
            bound_x1 = std::max(bound_x1, rects[j].x_pixels + rects[j].width_pixels);
            bound_y1 = std::max(bound_y1, rects[j].y_pixels + rects[j].height_pixels);
          }
        }
      }
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      if (cvars::vulkan_transfer_scissor_bounds && bound_x1 > bound_x0 &&
          bound_y1 > bound_y0) {
        const uint32_t x0 = std::min(bound_x0 * GetKeyScaleX(dest_rt_key),
                                     transfer_framebuffer->host_extent.width);
        const uint32_t y0 = std::min(bound_y0 * GetKeyScaleY(dest_rt_key),
                                     transfer_framebuffer->host_extent.height);
        const uint32_t x1 = std::min(bound_x1 * GetKeyScaleX(dest_rt_key),
                                     transfer_framebuffer->host_extent.width);
        const uint32_t y1 = std::min(bound_y1 * GetKeyScaleY(dest_rt_key),
                                     transfer_framebuffer->host_extent.height);
        transfer_scissor.offset.x = int32_t(x0);
        transfer_scissor.offset.y = int32_t(y0);
        transfer_scissor.extent.width = x1 - x0;
        transfer_scissor.extent.height = y1 - y0;
      }
      command_processor_.SetScissor(transfer_scissor);

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // The cutout of this aspect (merged invocations share the mode).
        const Transfer::Rectangle* const invocation_cutout =
            kTransferModes[size_t(transfer_invocation_first.shader_key.mode)]
                        .output == TransferOutput::kStencilBit
                ? stencil_rectangle_cutout
                : resolve_clear_rectangle;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count =
            transfer_invocation_first.transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles,
                dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
                invocation_cutout);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles,
              dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
              invocation_cutout);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[size_t(
                transfer_pipeline_layout_index)];
        uint32_t transfer_sample_pipeline_count =
            vulkan_device->properties().sampleRateShading
                ? 1
                : uint32_t(1) << uint32_t(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit =
            (transfer_pipeline_layout_info.used_push_constant_dwords &
             kTransferUsedPushConstantDwordStencilMaskBit) != 0;
        // The destination stencil of the transferred area was cleared to 0, so
        // bits that are 0 everywhere in the source need no draw.
        uint32_t transfer_stencil_bits = 0xFF;
        if (transfer_is_stencil_bit && cvars::rt_cache_stencil_bit_transfer_mask) {
          if (source_vulkan_rt.key().is_depth) {
            transfer_stencil_bits = source_vulkan_rt.stencil_maybe_set_bits();
          }
          // Narrowed to what the EDRAM tiles of the merged transfers may hold
          // (also known for a color source reinterpreted as stencil).
          uint32_t transfer_tile_bits = 0;
          for (auto it_bits = it_merged_first;; ++it_bits) {
            transfer_tile_bits |= GetEdramStencilMaybeSetBits(
                it_bits->transfer.start_tiles, it_bits->transfer.end_tiles);
            if (it_bits == it_merged_last) {
              break;
            }
          }
          transfer_stencil_bits &= transfer_tile_bits;
        }

        // rt_cache_transfer_known_constants: the source range holds one EDRAM
        // word a clear left there - write the destination as a clear of that
        // word (the same bits in the destination's format) instead of copying.
        if (cvars::rt_cache_transfer_known_constants && !dest_is_64bpp &&
            !source_vulkan_rt.key().Is64bpp()) {
          const uint32_t needed_mask =
              dest_rt_key.is_depth
                  ? (transfer_is_stencil_bit ? UINT32_C(0xFF)
                                             : UINT32_C(0xFFFFFF00))
                  : UINT32_MAX;
          const uint32_t word = it_merged_first->transfer.constant_word;
          bool constant = true;
          for (auto it_constant = it_merged_first;; ++it_constant) {
            if ((it_constant->transfer.constant_mask & needed_mask) !=
                    needed_mask ||
                ((it_constant->transfer.constant_word ^ word) & needed_mask)) {
              constant = false;
              break;
            }
            if (it_constant == it_merged_last) {
              break;
            }
          }
          // 7e3 <-> 8_8_8_8 transfers decode the values: only zero stays zero.
          if (constant && transfer_shader_key.value_convert &&
              (word & needed_mask)) {
            constant = false;
          }
          if (constant) {
            VkClearAttachment constant_attachment = MakeResolveClearAttachment(
                dest_vulkan_rt,
                (word & needed_mask) |
                    (dest_rt_key.is_depth
                         ? (transfer_is_stencil_bit ? kResolveClearStencilOnlyBit
                                                    : kResolveClearDepthOnlyBit)
                         : 0));
            if (!dest_rt_key.is_depth && in_current_render_pass) {
              constant_attachment.colorAttachment = i - 1;
            }
            VkClearRect constant_rects[Transfer::kMaxRectanglesWithCutout];
            for (auto it_constant = it_merged_first;; ++it_constant) {
              Transfer::Rectangle constant_rectangles
                  [Transfer::kMaxRectanglesWithCutout];
              uint32_t constant_rectangle_count =
                  it_constant->transfer.GetRectangles(
                      dest_rt_key.base_tiles, dest_pitch_tiles,
                      dest_rt_key.msaa_samples, dest_is_64bpp,
                      constant_rectangles, invocation_cutout);
              for (uint32_t j = 0; j < constant_rectangle_count; ++j) {
                const Transfer::Rectangle& rectangle = constant_rectangles[j];
                VkClearRect& clear_rect = constant_rects[j];
                clear_rect.rect.offset.x =
                    int32_t(rectangle.x_pixels * GetKeyScaleX(dest_rt_key));
                clear_rect.rect.offset.y =
                    int32_t(rectangle.y_pixels * GetKeyScaleY(dest_rt_key));
                clear_rect.rect.extent.width =
                    rectangle.width_pixels * GetKeyScaleX(dest_rt_key);
                clear_rect.rect.extent.height =
                    rectangle.height_pixels * GetKeyScaleY(dest_rt_key);
                clear_rect.baseArrayLayer = 0;
                clear_rect.layerCount = 1;
              }
              if (constant_rectangle_count) {
                command_buffer.CmdVkClearAttachments(
                    1, &constant_attachment, constant_rectangle_count,
                    constant_rects);
              }
              if (it_constant == it_merged_last) {
                break;
              }
            }
            ++constant_transfer_clears_;
            if (cvars::log_transfer_summary &&
                !(constant_transfer_clears_ % cvars::log_transfer_summary)) {
              XELOGI("TransferConstantClears: {}", constant_transfer_clears_);
            }
            continue;
          }
        }

        if (cvars::log_transfer_summary) {
          // Diagnostic: which ownership transfers the scene keeps paying for.
          static std::map<std::array<uint32_t, 3>, std::array<uint64_t, 2>>
              transfer_summary;
          static uint32_t transfer_summary_count = 0;
          uint64_t summary_tiles = 0;
          for (auto it_sum = it_merged_first;; ++it_sum) {
            summary_tiles +=
                it_sum->transfer.end_tiles - it_sum->transfer.start_tiles;
            if (it_sum == it_merged_last) {
              break;
            }
          }
          auto& summary_entry = transfer_summary[{
              source_vulkan_rt.key().key, dest_rt_key.key,
              (transfer_is_stencil_bit ? 0x100 | transfer_stencil_bits : 0) |
                  (invocation_cutout ? 0x200 : 0) |
                  (transfer_rectangle_count << 12)}];
          ++summary_entry[0];
          summary_entry[1] += summary_tiles;
          if (++transfer_summary_count >= cvars::log_transfer_summary) {
            XELOGI("TransferSummary: {} invocations, {} distinct",
                   transfer_summary_count, transfer_summary.size());
            for (const auto& e : transfer_summary) {
              RenderTargetKey sk, dk;
              sk.key = e.first[0];
              dk.key = e.first[1];
              XELOGI(
                  "TransferSummary: n={} tiles={} src {}@{} p{} m{} f{} h{} -> "
                  "dst {}@{} p{} m{} f{} h{} {} bits=0x{:02X} cutout={} "
                  "rects={}",
                  e.second[0], e.second[1], sk.is_depth ? "D" : "C",
                  uint32_t(sk.base_tiles), uint32_t(sk.pitch_tiles_at_32bpp),
                  uint32_t(sk.msaa_samples), uint32_t(sk.resource_format),
                  uint32_t(sk.height_class), dk.is_depth ? "D" : "C",
                  uint32_t(dk.base_tiles), uint32_t(dk.pitch_tiles_at_32bpp),
                  uint32_t(dk.msaa_samples), uint32_t(dk.resource_format),
                  uint32_t(dk.height_class),
                  (e.first[2] & 0x100) ? "stencil" : "data",
                  e.first[2] & 0xFF, (e.first[2] >> 9) & 1, e.first[2] >> 12);
            }
            transfer_summary.clear();
            transfer_summary_count = 0;
          }
        }

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(float) * 2 * transfer_vertex_count,
                sizeof(float), transfer_vertex_buffer,
                transfer_vertex_buffer_offset));
        if (!transfer_rectangle_write_ptr) {
          complete = false;
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last;
             ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count =
              it_merged->transfer.GetRectangles(
                  dest_rt_key.base_tiles, dest_pitch_tiles,
                  dest_rt_key.msaa_samples, dest_is_64bpp,
                  transfer_invocation_rectangles, invocation_cutout);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle =
                transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 =
                -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 =
                -1.0f + transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 +
                transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 +
                transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_buffer.CmdVkBindVertexBuffers(0, 1, &transfer_vertex_buffer,
                                              &transfer_vertex_buffer_offset);

        const VkPipeline* transfer_pipelines = GetTransferPipelines(
            TransferPipelineKey(transfer_render_pass_key, transfer_shader_key));
        if (!transfer_pipelines) {
          complete = false;
          continue;
        }
        command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[0]);
        if (last_transfer_pipeline_layout_index !=
            transfer_pipeline_layout_index) {
          last_transfer_pipeline_layout_index = transfer_pipeline_layout_index;
          transfer_descriptor_sets_bound = 0;
          transfer_push_constants_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          VkDescriptorSet descriptor_set_host_depth_stencil_textures =
              host_depth_source_vulkan_rt->GetDescriptorSetTransferSource();
          if (last_descriptor_set_host_depth_stencil_textures !=
              descriptor_set_host_depth_stencil_textures) {
            last_descriptor_set_host_depth_stencil_textures =
                descriptor_set_host_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          VkDescriptorSet descriptor_set_depth_stencil_textures =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_depth_stencil_textures !=
              descriptor_set_depth_stencil_textures) {
            last_descriptor_set_depth_stencil_textures =
                descriptor_set_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetColorTextureBit) {
          VkDescriptorSet descriptor_set_color_texture =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_color_texture !=
              descriptor_set_color_texture) {
            last_descriptor_set_color_texture = descriptor_set_color_texture;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetColorTextureBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          RenderTargetKey host_depth_source_rt_key =
              host_depth_source_vulkan_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch =
              host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) -
              int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &=
                ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest = int32_t(dest_rt_key.base_tiles) -
                                            int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &=
                ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        // Apply the new bindings.
        // TODO(Triang3l): Merge binding updates into spans.
        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets &
            ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(
                  transfer_pipeline_layout_info.used_descriptor_sets &
                  (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(
                  transfer_pipeline_layout_info.used_descriptor_sets &
                  (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetColorTextureBit;
        }
        uint32_t transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords &
            ~transfer_push_constants_set;
        if (transfer_push_constants_unset &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  xe::bit_count(
                      transfer_pipeline_layout_info.used_push_constant_dwords &
                      (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(uint32_t), &last_host_depth_address_constant);
          transfer_push_constants_set |=
              kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset &
            kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  xe::bit_count(
                      transfer_pipeline_layout_info.used_push_constant_dwords &
                      (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(uint32_t), &last_address_constant);
          transfer_push_constants_set |=
              kTransferUsedPushConstantDwordAddressBit;
        }

        for (uint32_t j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(
                transfer_pipelines[j]);
          }
          for (uint32_t k = 0; k < uint32_t(transfer_is_stencil_bit ? 8 : 1);
               ++k) {
            if (transfer_is_stencil_bit) {
              uint32_t transfer_stencil_bit = uint32_t(1) << k;
              if (!(transfer_stencil_bits & transfer_stencil_bit) ||
                  cvars::debug_skip_stencil_bit_transfers) {
                ++stencil_bit_draws_skipped_;
                continue;
              }
              ++stencil_bit_draws_done_;
              if (stencil_bit_draws_done_ + stencil_bit_draws_skipped_ >=
                  stencil_bit_draws_log_next_) {
                stencil_bit_draws_log_next_ += 8192;
                XELOGI("StencilBitTransfers: {} bit draws, {} skipped",
                       stencil_bit_draws_done_, stencil_bit_draws_skipped_);
              }
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(uint32_t) *
                      xe::bit_count(
                          transfer_pipeline_layout_info
                              .used_push_constant_dwords &
                          (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(uint32_t), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(
                  VK_STENCIL_FACE_FRONT_AND_BACK, transfer_stencil_bit);
            }
            if (command_buffer.pass_bytes_enabled()) {
              for (auto byte_it = it_merged_first; byte_it <= it_merged_last; ++byte_it) {
                Transfer::Rectangle byte_rects[Transfer::kMaxRectanglesWithCutout];
                uint32_t byte_count = byte_it->transfer.GetRectangles(
                    dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples,
                    dest_is_64bpp, byte_rects, invocation_cutout);
                uint64_t byte_pixels = 0;
                for (uint32_t b = 0; b < byte_count; ++b)
                  byte_pixels += uint64_t(byte_rects[b].width_pixels) * byte_rects[b].height_pixels;
                NotePassBytesRead(command_buffer, byte_it->transfer.source,
                                  byte_pixels * GetKeyScaleX(dest_rt_key) * GetKeyScaleY(dest_rt_key), false);
                if (byte_it->transfer.host_depth_source == dest_rt) {
                  PassByteLedger::Event buffer_read;
                  buffer_read.kind = PassByteLedger::kRead;
                  buffer_read.category = PassByteLedger::kOwnership;
                  buffer_read.conservative = true;
                  buffer_read.read = byte_pixels * GetKeyScaleX(dest_rt_key) * GetKeyScaleY(dest_rt_key) *
                      4 * (1u << uint32_t(dest_rt_key.msaa_samples));
                  command_buffer.AnnotatePassBytes(buffer_read);
                } else if (byte_it->transfer.host_depth_source) {
                  NotePassBytesRead(command_buffer, byte_it->transfer.host_depth_source,
                                    byte_pixels * GetKeyScaleX(dest_rt_key) * GetKeyScaleY(dest_rt_key), false);
                }
              }
            }
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }

      if (!stencil_copy_transfers_.empty()) {
        // The depth went through the pass above; the stencil is written
        // outside of it, as bytes computed from the color sources.
        command_processor_.EndRenderPass("stencil_copy");
        const VkBuffer stencil_buffer = stencil_copy_scratch.buffer();
        for (const Transfer& transfer : stencil_copy_transfers_) {
          auto& source_rt = *static_cast<VulkanRenderTarget*>(transfer.source);
          command_processor_.PushImageMemoryBarrier(
              source_rt.image(),
              ui::vulkan::util::InitializeSubresourceRange(
                  VK_IMAGE_ASPECT_COLOR_BIT),
              source_rt.current_stage_mask(),
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              source_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
              source_rt.current_layout(),
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          source_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        command_processor_.SubmitBarriers(true);
        const VkDescriptorSet stencil_buffer_set =
            command_processor_.AllocateSingleTransientDescriptor(
                VulkanCommandProcessor::SingleTransientDescriptorLayout::
                    kStorageBufferCompute);
        if (stencil_buffer_set != VK_NULL_HANDLE) {
          VkDescriptorBufferInfo stencil_buffer_info = {stencil_buffer, 0,
                                                        stencil_copy_size};
          VkWriteDescriptorSet stencil_buffer_write = {
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          stencil_buffer_write.dstSet = stencil_buffer_set;
          stencil_buffer_write.descriptorCount = 1;
          stencil_buffer_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          stencil_buffer_write.pBufferInfo = &stencil_buffer_info;
          vulkan_device->functions().vkUpdateDescriptorSets(
              vulkan_device->device(), 1, &stencil_buffer_write, 0, nullptr);
        }
        uint32_t copy_region_count = 0;
        VkBufferImageCopy copy_regions[Transfer::kMaxRectanglesWithCutout * 8];
        if (stencil_buffer_set != VK_NULL_HANDLE) {
          command_processor_.BindExternalComputePipeline(stencil_copy_pipeline_);
          uint32_t word_offset = 0;
          for (const Transfer& transfer : stencil_copy_transfers_) {
            auto& source_rt =
                *static_cast<VulkanRenderTarget*>(transfer.source);
            VkDescriptorSet sets[] = {source_rt.GetDescriptorSetTransferSource(),
                                      stencil_buffer_set};
            command_buffer.CmdVkBindDescriptorSets(
                VK_PIPELINE_BIND_POINT_COMPUTE, stencil_copy_pipeline_layout_,
                0, 2, sets, 0, nullptr);
            Transfer::Rectangle copy_rectangles
                [Transfer::kMaxRectanglesWithCutout];
            uint32_t copy_rectangle_count = transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles,
                dest_rt_key.msaa_samples, false, copy_rectangles,
                stencil_rectangle_cutout);
            for (uint32_t k = 0; k < copy_rectangle_count &&
                                 copy_region_count < xe::countof(copy_regions);
                 ++k) {
              const Transfer::Rectangle& rectangle = copy_rectangles[k];
              uint32_t constants[] = {rectangle.width_pixels >> 2,
                                      rectangle.height_pixels,
                                      rectangle.x_pixels, rectangle.y_pixels,
                                      word_offset};
              command_buffer.CmdVkPushConstants(
                  stencil_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                  0, sizeof(constants), constants);
              command_buffer.CmdVkDispatch(
                  ((rectangle.width_pixels >> 2) + 7) >> 3,
                  (rectangle.height_pixels + 7) >> 3, 1);
              VkBufferImageCopy& region = copy_regions[copy_region_count++];
              region.bufferOffset = VkDeviceSize(word_offset) * 4;
              region.bufferRowLength = rectangle.width_pixels;
              region.bufferImageHeight = rectangle.height_pixels;
              region.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
              region.imageOffset = {int32_t(rectangle.x_pixels),
                                    int32_t(rectangle.y_pixels), 0};
              region.imageExtent = {rectangle.width_pixels,
                                    rectangle.height_pixels, 1};
              word_offset +=
                  (rectangle.width_pixels >> 2) * rectangle.height_pixels;
            }
          }
        }
        command_processor_.PushBufferMemoryBarrier(
            stencil_buffer, 0, VK_WHOLE_SIZE,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
            dest_vulkan_rt.current_layout(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        command_processor_.SubmitBarriers(true);
        if (copy_region_count) {
          std::memcpy(command_buffer.CmdCopyBufferToImageEmplace(
                          stencil_buffer, dest_vulkan_rt.image(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          copy_region_count),
                      copy_regions, sizeof(VkBufferImageCopy) * copy_region_count);
        }
        stencil_copy_scratch.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT);
        stencil_copy_scratch.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT);
        VkPipelineStageFlags dest_draw_stage_mask;
        VkAccessFlags dest_draw_access_mask;
        VkImageLayout dest_draw_layout;
        dest_vulkan_rt.GetDrawUsage(&dest_draw_stage_mask,
                                    &dest_draw_access_mask, &dest_draw_layout);
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            VK_PIPELINE_STAGE_TRANSFER_BIT, dest_draw_stage_mask,
            VK_ACCESS_TRANSFER_WRITE_BIT, dest_draw_access_mask,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dest_draw_layout);
        dest_vulkan_rt.SetUsage(dest_draw_stage_mask, dest_draw_access_mask,
                                dest_draw_layout);
        stencil_copy_transfers_.clear();
        ++stencil_copies_;
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      if (!in_current_render_pass) {
        command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
            transfer_render_pass, transfer_framebuffer, transfer_dest_view,
            dest_rt_key.is_depth,
            GetMsrtssSampleCount(dest_rt_key.msaa_samples),
            transfer_dest_view_is_integer);
      }
      VkClearAttachment resolve_clear_attachment = MakeResolveClearAttachment(
          dest_vulkan_rt, render_target_resolve_clear_values[i]);
      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, 1,
                                           &resolve_clear_rect);
      if (command_processor_.IsRtTraceActive()) {
        XELOGI(
            "RTTRACE rclear dest={} rect=[{},{},{},{}]",
            FormatRtTraceKey(dest_rt, true), resolve_clear_rect.rect.offset.x,
            resolve_clear_rect.rect.offset.y,
            resolve_clear_rect.rect.extent.width,
            resolve_clear_rect.rect.extent.height);
      }
    }
  }

  command_processor_.PopDebugMarker();
  return complete;
}

VkPipeline VulkanRenderTargetCache::GetDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(spirv_version_,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);

  // Bindings.
  // EDRAM buffer.
  bool format_is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                              key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t) << uint32_t(format_is_64bpp));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id edram_buffer = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet,
                        kDumpDescriptorSetEdram);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  // The image is not multisampled with single-sample storage, though the
  // EDRAM samples are addressed as usual.
  bool source_texture_is_multisampled =
      source_is_multisampled && !msaa_single_sample_storage_;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(),
                                          &source_is_uint);
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false,
                            source_texture_is_multisampled, 1,
                            spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet,
                        kDumpDescriptorSetSource);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false,
                              source_texture_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          kDumpDescriptorSetSource);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants.
  id_vector_temp.clear();
  id_vector_temp.reserve(kDumpPushConstantCount);
  for (uint32_t i = 0; i < kDumpPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants =
      builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants, kDumpPushConstantPitches,
                        "pitches");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantPitches,
                              spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantPitches));
  builder.addMemberName(type_push_constants, kDumpPushConstantOffsets,
                        "offsets");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantOffsets,
                              spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants = builder.createVariable(
      spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
      "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::GlobalInvocationId));

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id =
      builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x =
      builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  // Dumps for fully native resolves address the EDRAM buffer with the plain
  // 1x1 tile layout.
  uint32_t layout_scale_x = key.native_layout ? 1 : draw_resolution_scale_x();
  uint32_t layout_scale_y = key.native_layout ? 1 : draw_resolution_scale_y();
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
      layout_scale_x;
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y =
      builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * layout_scale_y;
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits =
      builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                              pitches_constant, const_uint_0,
                              const_edram_pitch_tiles_bits),
          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_uint_0, const_edram_base_tiles_bits_plus_1),
      rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index.
  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(
              spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createBinOp(spv::OpIMul, type_uint,
                                              const_tile_width, tile_sample_y),
                          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(),
                                    tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }

  // Get the linear tile index within the source texture.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(
          spv::OpBitFieldUExtract, type_uint, offsets_constant,
          const_edram_base_tiles_bits_plus_1,
          builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles = builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint, pitches_constant,
      const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x = builder.createBinOp(
      spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                          source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height,
                          source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                         source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                           source_sample_x, const_uint_1);
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      source_sample_id = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x,
                              const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // 2x MSAA source texture sample index - convert from the guest to
      // the Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y,
                                  const_uint_1),
              const_uint_0),
          builder.makeUintConstant(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
              1, msaa_2x_attachments_supported_)),
          builder.makeUintConstant(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
              0, msaa_2x_attachments_supported_)));
    }
  }
  if (key.source_scale_native && !key.native_layout &&
      IsDrawResolutionScaled()) {
    // Native source dumped to the scaled EDRAM layout. Duplicate each pixel
    // into all the scaled sample slots covering it. Done after the sample index
    // is extracted since MSAA isn't affected by scale.
    source_pixel_x = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_x,
        builder.makeUintConstant(draw_resolution_scale_x()));
    source_pixel_y = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_y,
        builder.makeUintConstant(draw_resolution_scale_y()));
  }

  // Load the source, and pack the value into one or two 32-bit integers.
  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  source_texture_parameters.sampler =
      builder.createLoad(source_texture, spv::NoPrecision);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
  source_texture_parameters.coords =
      builder.createCompositeConstruct(type_int2, id_vector_temp);
  if (source_texture_is_multisampled) {
    source_texture_parameters.sample =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  spv::Id source_vec4 = builder.createTextureCall(
      spv::NoPrecision, builder.makeVectorType(source_component_type, 4), false,
      true, false, false, false, source_texture_parameters,
      spv::ImageOperandsMaskNone);
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(
            spv::NoPrecision, builder.makeVectorType(type_uint, 4), false, true,
            false, false, false, source_texture_parameters,
            spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 =
        builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(
                    spv::OpFMul, type_float, source_depth32,
                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32,
            !depth_float24_convert_in_pixel_shader() && depth_float24_round(),
            true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
        builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        // k_8_8_8_8_GAMMA is stored as linear in the unorm16 host render
        // target, so encode RGB linear -> gamma before packing (alpha stays
        // linear). Reaching the gamma resource format implies
        // gamma_render_target_as_unorm16.
        bool is_gamma = key.GetColorFormat() ==
                        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        spv::Id source_red =
            builder.createCompositeExtract(source_vec4, type_float, 0);
        if (is_gamma) {
          source_red = SpirvShaderTranslator::LinearToPWLGamma(
              &builder, source_red, true, ext_inst_glsl_std_450);
        }
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float,
                                                    source_red, unorm_scale),
                                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (uint32_t i = 1; i < 4; ++i) {
          spv::Id source_component =
              builder.createCompositeExtract(source_vec4, type_float, i);
          if (is_gamma && i < 3) {
            source_component = SpirvShaderTranslator::LinearToPWLGamma(
                &builder, source_component, true, ext_inst_glsl_std_450);
          }
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(spv::OpFMul, type_float,
                                          source_component, unorm_scale),
                      unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createCompositeExtract(source_vec4, type_float, 0),
                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(
                          spv::OpFMul, type_float,
                          builder.createCompositeExtract(source_vec4,
                                                         type_float, i),
                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SpirvShaderTranslator::UnclampedFloat32To7e3(
                  builder,
                  builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        alpha_saturated,
                                        builder.makeFloatConstant(3.0f)),
                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        spv::Id component_offset_width = builder.makeUintConstant(16);
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          packed[i] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createCompositeExtract(source_vec4, type_uint, 2 * i),
              builder.createCompositeExtract(source_vec4, type_uint, 2 * i + 1),
              component_offset_width, component_offset_width);
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4,
                                                      source_component_type, i);
          if (!source_is_uint) {
            packed_ref =
                builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  // Write the packed value to the EDRAM buffer.
  spv::Id store_value = packed[0];
  if (format_is_64bpp) {
    id_vector_temp.clear();
    id_vector_temp.push_back(packed[0]);
    id_vector_temp.push_back(packed[1]);
    store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
  }
  id_vector_temp.clear();
  // The only SSBO structure member.
  id_vector_temp.push_back(builder.makeIntConstant(0));
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  builder.createStore(store_value,
                      builder.createAccessChain(spv::StorageClassUniform,
                                                edram_buffer, id_vector_temp));

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize,
                           kDumpSamplesPerGroupX, kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point = builder.addEntryPoint(
      spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the pipeline, and store the handle even if creation fails not to try
  // to create it again later.
  VkPipeline pipeline = ui::vulkan::util::CreateComputePipeline(
      command_processor_.GetVulkanDevice(),
      key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_,
      reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        UINT32_C(1) << uint32_t(key.msaa_samples),
        key.is_depth
            ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
            : xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

void VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base,
                                                uint32_t dump_row_length_used,
                                                uint32_t dump_rows,
                                                uint32_t dump_pitch,
                                                bool native_layout) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  MaterializePendingConstantRowsForDump(dump_base, dump_pitch, dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return;
  }

  command_processor_.PushDebugMarker(
      "DumpRenderTargets (EDRAM Write): base tile %u", dump_base);

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)
        ->SetTemporarySortIndex(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to sort the invocations.
  UseEdramBuffer(EdramBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  constexpr VkPipelineStageFlags kRenderTargetDstStageMask =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  constexpr VkAccessFlags kRenderTargetDstAccessMask =
      VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kRenderTargetNewLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt =
        *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            rt_key.is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
        vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.temporary_sort_index() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    // Native layout is only for resolves with ALL native sources.
    assert_true(!native_layout || rt_key.scale_native);
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    pipeline_key.source_scale_native = rt_key.scale_native;
    pipeline_key.native_layout = uint32_t(native_layout);
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt =
        *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout = rt_key.is_depth
                                           ? dump_pipeline_layout_depth_
                                           : dump_pipeline_layout_color_;

    // Only need to bind the EDRAM buffer once (relying on pipeline layout
    // compatibility).
    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
          kDumpDescriptorSetEdram, 1, &edram_storage_buffer_descriptor_set_, 0,
          nullptr);
    }

    VkDescriptorSet source_descriptor_set =
        vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
          kDumpDescriptorSetSource, 1, &source_descriptor_set, 0, nullptr);
    }

    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_bound = false;
    }
    if (!pitches_bound) {
      pitches_bound = true;
      command_buffer.CmdVkPushConstants(
          pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
          sizeof(uint32_t) * kDumpPushConstantPitches, sizeof(last_pitches),
          &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(
            pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            sizeof(uint32_t) * kDumpPushConstantOffsets, sizeof(last_offsets),
            &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      const uint64_t byte_pixels = uint64_t(dispatch.width_tiles) * dispatch.height_tiles *
          (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
          xenos::kEdramTileHeightSamples * (native_layout ? 1 : draw_resolution_scale_x()) *
          (native_layout ? 1 : draw_resolution_scale_y());
      NotePassBytesRead(command_buffer, &vulkan_rt,
                        byte_pixels / (1u << uint32_t(rt_key.msaa_samples)),
                        true, byte_pixels * (rt_key.Is64bpp() ? 8 : 4));
      // The native layout has a 1x1 footprint.
      command_buffer.CmdVkDispatch(
          ((native_layout ? 1 : draw_resolution_scale_x()) *
               (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          ((native_layout ? 1 : draw_resolution_scale_y()) *
               xenos::kEdramTileHeightSamples * dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1,
          DeferredCommandBuffer::DispatchSite::kDump);
    }
    MarkEdramBufferModified();
  }

  command_processor_.PopDebugMarker();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
