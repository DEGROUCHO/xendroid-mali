/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/render_target_cache.h"

#include <cmath>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(
    rt_cache_skip_unused_stencil_transfers, true,
    "Without VK_EXT_shader_stencil_export, depth render target ownership "
    "transfers draw each of the 8 stencil bits separately. Skip those draws "
    "while the guest has never enabled the stencil test (stencil values are "
    "then never read).",
    "GPU");

DEFINE_bool(
    rt_cache_edram_stencil_bit_map, true,
    "Without VK_EXT_shader_stencil_export: track which stencil bits may be "
    "set per EDRAM tile rather than only per render target, so a depth "
    "render target that once received any stencil bit from a color render "
    "target isn't transferred with all 8 bit passes forever. Viewing depth "
    "as color and back, areas no draw wrote, and areas since cleared with a "
    "known stencil keep or regain their exact bits.",
    "GPU");

DEFINE_bool(
    rt_cache_color_draws_dirty_stencil, true,
    "With rt_cache_edram_stencil_bit_map: assume that draws to a color render "
    "target may leave any value in the stencil byte of the EDRAM words they "
    "claim (accurate - the low byte of a color pixel is the stencil when the "
    "same EDRAM area is later viewed as depth). When false, only stencil "
    "writes, clears and snapshot restores make stencil bits known as possibly "
    "set, so depth buffers that take their EDRAM area back from a color "
    "target every frame are not transferred with one draw per stencil bit - "
    "stencil values a game produces by drawing color are lost.",
    "GPU");
DEFINE_bool(
    rt_cache_ownership_claim_memo, true,
    "Skip EDRAM ownership-map walks for render target claims that provably "
    "change nothing (the same render target re-claiming an extent it already "
    "fully owns, with no ownership mutation since), and skip the transfer/"
    "resolve-clear pass entirely when no draw produced any ownership "
    "transfers. Disable for debugging render target transfer issues.",
    "GPU");

DEFINE_bool(
    rt_cache_skip_transfers_for_full_clears, true,
    "Don't transfer the previous EDRAM contents into render targets that the "
    "draw overwrites entirely - a single screen-aligned rectangle (evaluated "
    "on the CPU) covering the whole claimed range, writing all depth and "
    "stencil bits (depth always + write, stencil always + replace, full "
    "write mask) and / or all color components without blending, with no "
    "culling, clipping, alpha test or pixel kill that could leave samples "
    "unwritten. Games clearing the EDRAM with such rectangles (Modern Warfare "
    "2 clears the whole EDRAM through a 4x MSAA view) otherwise pay for a "
    "full-EDRAM transfer before every clear.",
    "GPU");
UPDATE_from_bool(rt_cache_skip_transfers_for_full_clears, 2026, 10, 5, 12, false);
DEFINE_bool(
    rt_cache_full_overwrite_quads, true,
    "With rt_cache_skip_transfers_for_full_clears: also recognize a "
    "screen-aligned quad drawn as two triangles (a strip or a fan of 4 "
    "vertices, a quad, a list of 6) as overwriting its rectangle, under the "
    "same conditions as for a rectangle. Full-screen post-processing and "
    "composition passes are drawn this way into render targets that reuse "
    "the EDRAM area of the scene, and otherwise pay for a transfer of what "
    "they are about to replace.",
    "GPU");
DEFINE_bool(
    rt_cache_legacy_clear_paths, false,
    "Diagnostic, for measuring only: restore the behavior before the clear "
    "path fixes of 2026-10-10 (the nested implementation update for the "
    "color part of a retargeted depth clear, which performed the color "
    "transfers twice, and no draw clear chain for a depth render target that "
    "is its own host depth source).",
    "GPU");
DEFINE_bool(
    rt_cache_dont_care_load_for_full_clears, true,
    "With rt_cache_skip_transfers_for_full_clears, also begin the render pass "
    "without loading the previous contents (loadOp DONT_CARE) when the clear "
    "rectangle covers the whole host render target, tile padding and all "
    "EDRAM rows included (the 4x MSAA whole-EDRAM clears of Modern Warfare "
    "2).",
    "GPU");
UPDATE_from_bool(rt_cache_dont_care_load_for_full_clears, 2026, 10, 5, 12, false);
DEFINE_bool(
    rt_cache_transfer_known_constants, true,
    "Remember the EDRAM word a clear (a resolve clear, or a rectangle writing "
    "one exact depth and stencil over a whole render target range) left in a "
    "render target, until anything else writes it, and do the ownership "
    "transfers out of such a range as clears of the destination with that "
    "word instead of drawing the copy (and 8 stencil bit draws without "
    "stencil export).",
    "GPU");
DEFINE_bool(
    rt_cache_full_overwrite_alpha_without_pixel_shader, true,
    "With rt_cache_skip_transfers_for_full_clears, accept depth / stencil "
    "clear rectangles with alpha test or alpha to coverage enabled when the "
    "draw has no effective pixel shader, as nothing can then drop pixels "
    "(GTA IV clears its depth buffers this way).",
    "GPU");
DEFINE_int32(log_full_clear_transfer_skips, 0,
             "Diagnostic: log this many draws whose render target transfers "
             "were skipped by rt_cache_skip_transfers_for_full_clears.",
             "GPU");
DEFINE_bool(
    rt_cache_retarget_full_depth_clears, true,
    "With rt_cache_skip_transfers_for_full_clears, perform a depth / stencil "
    "clear rectangle covering the whole claimed range with a known constant "
    "value as a clear of the depth render target currently holding that range "
    "when it only differs by the MSAA sample count, instead of drawing it into "
    "its own render target. The constant is the same in either sample layout, "
    "so the transfers into the clear's render target and back out of it are "
    "not needed (Modern Warfare 2 clears its 1x depth buffer through a 4x MSAA "
    "view of the same EDRAM, costing two full depth transfers per clear, each "
    "9 passes without stencil export).",
    "GPU");
UPDATE_from_bool(rt_cache_retarget_full_depth_clears, 2026, 10, 5, 12, false);
DEFINE_bool(
    rt_cache_retarget_depth_only_clears, true,
    "With rt_cache_retarget_full_depth_clears, also retarget depth clear "
    "rectangles that don't touch the stencil (stencil test disabled) - as a "
    "clear of only the depth of the render target holding the range, keeping "
    "its stencil. Depth-only clears through a 4x MSAA view of a 1x depth "
    "buffer (shadow maps) otherwise cost two full depth transfers, 9 passes "
    "each without stencil export, for a single rectangle. Likewise stencil "
    "clear rectangles that don't touch the depth, and clears also writing "
    "color (the color part is still drawn).",
    "GPU");

DEFINE_bool(
    rt_cache_clear_new_depth_owners, true,
    "For a proven constant depth and stencil clear, transfer only the pixels "
    "outside its rectangle when changing EDRAM ownership, then clear that "
    "rectangle. Requires rt_cache_skip_transfers_for_full_clears. Applies only "
    "to depth-only draws with pending ownership transfers.",
    "GPU");
UPDATE_from_bool(rt_cache_clear_new_depth_owners, 2026, 10, 5, 12, false);

DEFINE_bool(log_render_target_height_cap, false,
            "Log, every 600 frames, how many draws per frame got capped or "
            "larger-height render targets and why, plus the first draws that "
            "needed more than the active capped height.",
            "GPU");
DEFINE_uint32(
    render_target_height_cap, 768,
    "Host render target images normally span the EDRAM down to the next "
    "addressing period (2528 rows for a 1040-pixel pitch) though games draw "
    "720. When a draw needs at most this many rows, its render targets are "
    "created only this tall - unless a full-height render target already holds "
    "the same base, which keeps being used. A range "
    "needing more rows uses a full-height render target (the ownership transfer "
    "copies the data). 0 "
    "keeps full height. Must be a multiple of 16. Read once at startup.",
    "GPU");

DEFINE_bool(
    rt_cache_draw_overwrite_cutouts, true,
    "When a draw provably overwrites every bit of a render target inside its "
    "screen-aligned rectangle (the conditions of "
    "rt_cache_skip_transfers_for_full_clears) but doesn't cover the whole "
    "range claimed for the render target (clears of the visible area of a "
    "surface whose pitch is wider, clears in strips), transfer only the "
    "pixels outside that rectangle when changing EDRAM ownership.",
    "GPU");
DEFINE_int32(log_draw_overwrite_cutouts, 0,
             "Diagnostic: log this many ownership changes whose transfers "
             "skip the rectangle overwritten by the draw "
             "(rt_cache_draw_overwrite_cutouts).",
             "GPU");

DEFINE_bool(
    depth_transfer_not_equal_test, true,
    "When transferring data between depth render targets, use the \"not "
    "equal\" test to avoid writing rewriting depth via shader depth output if "
    "it's the same as the one currently in the depth buffer in case of round "
    "trips of the data.\n"
    "Settings this to true may make transfer round trips more friendly to "
    "depth compression depending on how the GPU implements it (as arbitrary "
    "depth output may result in it being disabled completely), which is "
    "beneficial to subsequent rendering, while setting this to false may "
    "reduce bandwidth usage during transfers as the previous depth won't need "
    "to be read.",
    "GPU");
// Lossless round trip: 545407F2.
// Lossy round trip with the "greater or equal" test afterwards: 4D530919.
// Lossy round trip with the "equal" test afterwards: 535107F5, 565507EF.
DEFINE_bool(
    depth_float24_round, false,
    "Whether to round to the nearest even, rather than truncating (rounding "
    "towards zero), the depth when converting it to 24-bit floating-point "
    "(20e4) from the host precision (32-bit floating point) when using a host "
    "depth buffer.\n"
    "false:\n"
    " Recommended.\n"
    " The conversion may move the depth values farther away from the camera.\n"
    " Without depth_float24_convert_in_pixel_shader:\n"
    "  The \"greater or equal\" depth test function continues to work fine if "
    "the full host precision depth data is lost, it's still possible to draw "
    "another pass of the same geometry with it.\n"
    "  (See the description of depth_float24_convert_in_pixel_shader for more "
    "information about full precision depth data loss.)\n"
    " With depth_float24_convert_in_pixel_shader:\n"
    "  Faster - the pixel shader for hidden surfaces may still be skipped "
    "(using conservative depth output).\n"
    "true:\n"
    " Only for special cases of issues caused by minor 32-bit floating-point "
    "rounding errors, for instance, when the game tries to draw something at "
    "the camera plane by setting Z of the vertex position to W.\n"
    " The conversion may move the depth values closer or farther.\n"
    " Using the same rounding mode as in the Direct3D 9 reference rasterizer.\n"
    " Without depth_float24_convert_in_pixel_shader:\n"
    "  Not possible to recover from a full host precision depth data loss - in "
    "subsequent passes of rendering the same geometry, half of the samples "
    "will be failing the depth test with the \"greater or equal\" depth test "
    "function.\n"
    " With depth_float24_convert_in_pixel_shader:\n"
    "  Slower - depth rejection before the pixel shader is not possible.\n"
    "When the depth buffer is emulated in software (via the fragment shader "
    "interlock / rasterizer-ordered view), this is ignored, and rounding to "
    "the nearest even is always done.",
    "GPU");
// With MSAA, when converting the depth in pixel shaders, they must run at
// sample frequency - otherwise, if the depth is the same for the entire pixel,
// intersections of polygons cannot be antialiased.
//
// Important usage note: When using this mode, bounds of the fixed-function
// viewport must be converted to and back from float24 too (preferably using
// rounding to the nearest even regardless of whether truncation was requested
// for the values, to reduce the error already caused by truncation rather than
// to amplify it). This ensures that clamping to the viewport bounds, which
// happens after the pixel shader even if it overwrites the resulting depth, is
// never done to a value not representable as float24 (for example, if the
// minimum Z is a number too small to be represented as float24, but not zero,
// it won't be possible to write what should become 0x000000 to the depth
// buffer). Note that this may add some error to the depth values from the
// rasterizer; however, modifying Z in the vertex shader to make interpolated
// depth values would cause clipping to be done to different bounds, which may
// be more undesirable, especially in cases when Z is explicitly set to a value
// like 0 or W (in such cases, the adjusted polygon may go outside 0...W in clip
// space and disappear).
//
// If false, doing the depth test at the host precision, converting to 20e4 to
// support reinterpretation, but keeping track of both the last color (or
// non-20e4 depth) value (let's call it stored_f24) and the last host depth
// value (stored_host) for each EDRAM pixel, reloading the last host depth value
// if stored_f24 == to_f24(stored_host) (otherwise it was overwritten by
// something else, like clearing, or an actually used color buffer; this is
// inexact though, and will incorrectly load pixels that were overwritten by
// something else in the EDRAM, but turned out to have the same value on the
// guest as before - an outdated host-precision value will be loaded in these
// cases instead).
DEFINE_bool(
    depth_float24_convert_in_pixel_shader, false,
    "Whether to convert the depth values to 24-bit floating-point (20e4) from "
    "the host precision (32-bit floating point) directly in the pixel shaders "
    "of guest draws when using a host depth buffer.\n"
    "This prevents visual artifacts (interleaved stripes of parts of surfaces "
    "rendered and parts not rendered, having either the same width in case of "
    "the \"greater or equal\" depth test function, or the former being much "
    "thinner than the latter with the \"equal\" function) if the full host "
    "precision depth data is lost.\n"
    "This issue may happen if the game reloads the depth data previously "
    "evicted from the EDRAM to the RAM back to the EDRAM, but the EDRAM region "
    "that previously contained that depth buffer was overwritten by another "
    "depth buffer, or the game loads it to a different location in the EDRAM "
    "than it was previously placed at, thus Xenia is unable to restore the "
    "depth data with the original precision, and instead falls back to "
    "converting the lower-precision values, so in subsequent rendering passes "
    "for the same geometry, the actual depth values of the surfaces don't "
    "match those stored in the depth buffer anymore.\n"
    "This is a costly option because it makes the GPU unable to use depth "
    "buffer compression, and also with MSAA, forces the pixel shader to run "
    "for every subpixel sample rather than for the entire pixel, making pixel "
    "shading 2 or 4 times heavier depending on the MSAA sample count.\n"
    "The rounding direction is controlled by the depth_float24_round "
    "configuration variable.\n"
    "Note that with depth_float24_round = true, this becomes even more costly "
    "because pixel shaders must be executed regardless of whether the surface "
    "is behind the previously drawn surfaces. With depth_float24_round = "
    "false, conservative depth output is used, however, so depth rejection "
    "before the pixel shader may still work.\n"
    "If sample-rate shading is not supported by the host GPU, the conversion "
    "in the pixel shader is done only when MSAA is not used.\n"
    "When the depth buffer is emulated in software (via the fragment shader "
    "interlock / rasterizer-ordered view), this is ignored because 24-bit "
    "depth is always used directly.",
    "GPU");
DEFINE_bool(
    draw_resolution_scaled_texture_offsets, true,
    "Apply offsets from texture fetch instructions taking resolution scale "
    "into account for render-to-texture, for more correct shadow filtering, "
    "bloom, etc., in some cases.",
    "GPU");
DEFINE_uint32(
    draw_resolution_scale_threshold, 0,
    "Surface pitch in pixels at or below render targets skip being upscaled "
    "by draw_resolution_scale_x/y. 0 disables it.\n"
    "Small offscreen surfaces like bloom or depth of field buffers often "
    "break when upscaled and keeping them native avoids that. The pitch "
    "is compared after alignment to 80 pixel EDRAM tiles, so prefer "
    "conservative values, only as high as the broken effects need.\n"
    "Host render targets only.",
    "GPU");
DEFINE_bool(
    gamma_render_target_as_unorm16, true,
    "When the host can't write 8 bits per component pixels with piecewise "
    "linear gamma encoding directly with correct blending, use the 16-bit "
    "unsigned normalized format, if supported, for conceptually correct "
    "8_8_8_8_GAMMA render target format blending in linear color space. "
    "Greatly increases accuracy for this format, but may result in render "
    "target copying costs if the game switches between 8_8_8_8_GAMMA and "
    "8_8_8_8 views for the same EDRAM render target.",
    "GPU");
DEFINE_bool(
    mrt_edram_used_range_clamp_to_min, true,
    "With host render targets, if multiple render targets are bound, estimate "
    "the EDRAM range modified in any of them to be not bigger than the "
    "distance between any two render targets in the EDRAM, rather than "
    "allowing the last one claim the rest of the EDRAM.\n"
    "Has effect primarily on draws without viewport clipping.\n"
    "Setting this to false results in higher accuracy in rare cases, but may "
    "increase the amount of copying that needs to be done sometimes.",
    "GPU");
DEFINE_bool(
    native_2x_msaa, true,
    "Use host 2x MSAA when available. Can be disabled for scalability testing "
    "on host GPU APIs where 2x is not mandatory, in this case, 2 samples of 4x "
    "MSAA will be used instead (with similar or worse quality and higher "
    "memory usage).",
    "GPU");
DEFINE_bool(
    native_stencil_value_output, true,
    "Use pixel shader stencil reference output where available for purposes "
    "like copying between render targets. Can be disabled for scalability "
    "testing, in this case, much more expensive drawing of 8 quads will be "
    "done.",
    "GPU");
DEFINE_bool(
    snorm16_render_target_full_range, true,
    "When the host can only support 16_16 and 16_16_16_16 render targets as "
    "-1...1, remap -32...32 to -1...1 to use the full possible range of "
    "values, at the expense of multiplicative blending correctness.",
    "GPU");
DEFINE_bool(
    value_convert_7e3_8888_reuse, true,
    "Decode (HDR float to LDR unorm) instead of bit-reinterpreting when a 7e3 "
    "(2_10_10_10_FLOAT) EDRAM tile is reused in place as 8_8_8_8 and the "
    "reusing "
    "draw blends over it, so the background is not colored garbage (e.g. "
    "Deadly "
    "Premonition foliage).\n"
    "On by default. Set to false to force bit-exact reinterpretation if a "
    "title "
    "regresses.",
    "GPU");
UPDATE_from_bool(value_convert_7e3_8888_reuse, 2026, 6, 28, 12, false);
// Enabled by default as the GPU is overall usually the bottleneck when the
// pixel shader interlock render backend implementation is used, anything that
// may improve GPU performance is favorable.
DEFINE_bool(
    execute_unclipped_draw_vs_on_cpu_for_psi_render_backend, true,
    "If execute_unclipped_draw_vs_on_cpu is enabled, execute the vertex shader "
    "for unclipped draws on the CPU even when using the pixel shader interlock "
    "(rasterizer-ordered view) implementation of the render backend on the "
    "host, for which no expensive copying between host render targets is "
    "needed when the ownership of a EDRAM range is changed.\n"
    "If this is enabled, excessive barriers may be eliminated when switching "
    "between different render targets in separate EDRAM locations.",
    "GPU");

namespace xe {
namespace gpu {

void RenderTargetCache::GetPSIColorFormatInfo(
    xenos::ColorRenderTargetFormat format, uint32_t write_mask,
    float& clamp_rgb_low, float& clamp_alpha_low, float& clamp_rgb_high,
    float& clamp_alpha_high, uint32_t& keep_mask_low,
    uint32_t& keep_mask_high) {
  keep_mask_low = keep_mask_high = 0;
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0xFF) << (i * 8);
        }
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = 31.875f;
      clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      // Alpha clamping affects blending source, so it's non-zero for alpha for
      // k_16_16 (the render target is fixed-point). There's one deviation from
      // how Direct3D 11.3 functional specification defines SNorm conversion
      // (NaN should be 0, not the lowest negative number), and that needs to be
      // handled separately.
      clamp_rgb_low = clamp_alpha_low = -32.0f;
      clamp_rgb_high = clamp_alpha_high = 32.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      // No NaNs on the Xbox 360 GPU, though can't use the extended range with
      // Direct3D and Vulkan conversions.
      // TODO(Triang3l): Use the extended-range encoding in all implementations.
      clamp_rgb_low = clamp_alpha_low = -65504.0f;
      clamp_rgb_high = clamp_alpha_high = 65504.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high =
          std::nanf("");
      write_mask &= 0b0001;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high =
          std::nanf("");
      write_mask &= 0b0011;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_high = ~uint32_t(0);
      }
      break;
    default:
      assert_unhandled_case(format);
      // Disable invalid render targets.
      write_mask = 0;
      break;
  }
  // Special case handled in the shaders for empty write mask to completely skip
  // a disabled render target: all keep bits are set.
  if (!write_mask) {
    keep_mask_low = keep_mask_high = ~uint32_t(0);
  }
}

uint32_t RenderTargetCache::Transfer::GetRangeRectangles(
    uint32_t start_tiles, uint32_t end_tiles, uint32_t base_tiles,
    uint32_t pitch_tiles, xenos::MsaaSamples msaa_samples, bool is_64bpp,
    Rectangle* rectangles_out, const Rectangle* cutout) {
  // EDRAM addressing wrapping must be handled by doing GetRangeRectangles for
  // two clamped ranges, in this case start_tiles == end_tiles will also
  // unambiguously mean an empty range rather than the entire EDRAM.
  assert_true(start_tiles < xenos::kEdramTileCount);
  assert_true(end_tiles <= xenos::kEdramTileCount);
  assert_true(start_tiles <= end_tiles);
  // If start_tiles < base_tiles, this is the tail after EDRAM addressing
  // wrapping.
  assert_true(start_tiles >= base_tiles || end_tiles <= base_tiles);
  assert_not_zero(pitch_tiles);
  if (start_tiles == end_tiles) {
    return 0;
  }
  uint32_t tile_width =
      xenos::kEdramTileWidthSamples >>
      (uint32_t(msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(is_64bpp));
  uint32_t tile_height = xenos::kEdramTileHeightSamples >>
                         uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  // If the first and / or the last rows have the same X spans as the middle
  // part, merge them with it.
  uint32_t rectangle_count = 0;
  // If start_tiles < base_tiles, this is the tail after EDRAM addressing
  // wrapping.
  uint32_t local_offset = start_tiles < base_tiles ? xenos::kEdramTileCount : 0;
  uint32_t local_start = local_offset + start_tiles - base_tiles;
  uint32_t local_end = local_offset + end_tiles - base_tiles;
  // Inclusive.
  uint32_t rows_start = local_start / pitch_tiles;
  // Exclusive.
  uint32_t rows_end = (local_end + (pitch_tiles - 1)) / pitch_tiles;
  uint32_t row_first_start = local_start - rows_start * pitch_tiles;
  uint32_t row_last_end = pitch_tiles - (rows_end * pitch_tiles - local_end);
  uint32_t rows = rows_end - rows_start;
  if (rows == 1 || row_first_start) {
    Rectangle rectangle_first;
    rectangle_first.x_pixels = row_first_start * tile_width;
    rectangle_first.y_pixels = rows_start * tile_height;
    rectangle_first.width_pixels =
        ((rows == 1 ? row_last_end : pitch_tiles) - row_first_start) *
        tile_width;
    rectangle_first.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_first,
        rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
    if (rows == 1) {
      return rectangle_count;
    }
  }
  uint32_t mid_rows_start = rows_start + 1;
  uint32_t mid_rows = rows - 2;
  if (!row_first_start) {
    --mid_rows_start;
    ++mid_rows;
  }
  if (row_last_end == pitch_tiles) {
    ++mid_rows;
  }
  if (mid_rows) {
    Rectangle rectangle_mid;
    rectangle_mid.x_pixels = 0;
    rectangle_mid.y_pixels = mid_rows_start * tile_height;
    rectangle_mid.width_pixels = pitch_tiles * tile_width;
    rectangle_mid.height_pixels = mid_rows * tile_height;
    rectangle_count += AddRectangle(
        rectangle_mid,
        rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  if (row_last_end != pitch_tiles) {
    Rectangle rectangle_last;
    rectangle_last.x_pixels = 0;
    rectangle_last.y_pixels = (rows_end - 1) * tile_height;
    rectangle_last.width_pixels = row_last_end * tile_width;
    rectangle_last.height_pixels = tile_height;
    rectangle_count += AddRectangle(
        rectangle_last,
        rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
  }
  assert_true(rectangle_count <= (cutout ? kMaxRectanglesWithCutout
                                         : kMaxRectanglesWithoutCutout));
  return rectangle_count;
}

uint32_t RenderTargetCache::Transfer::AddRectangle(const Rectangle& rectangle,
                                                   Rectangle* rectangles_out,
                                                   const Rectangle* cutout) {
  uint32_t rectangle_right = rectangle.x_pixels + rectangle.width_pixels;
  uint32_t rectangle_bottom = rectangle.y_pixels + rectangle.height_pixels;
  // If nothing to cut out (no region specified, or no intersection - if the
  // cutout region is in the middle on Y, but completely to the left / right on
  // X, don't split), add the whole rectangle.
  if (!cutout || !cutout->width_pixels || !cutout->height_pixels ||
      cutout->x_pixels >= rectangle_right ||
      cutout->x_pixels + cutout->width_pixels <= rectangle.x_pixels ||
      cutout->y_pixels >= rectangle_bottom ||
      cutout->y_pixels + cutout->height_pixels <= rectangle.y_pixels) {
    if (rectangles_out) {
      rectangles_out[0] = rectangle;
    }
    return 1;
  }
  uint32_t rectangle_count = 0;
  uint32_t cutout_right = cutout->x_pixels + cutout->width_pixels;
  uint32_t cutout_bottom = cutout->y_pixels + cutout->height_pixels;
  // Upper part after cutout.
  if (cutout->y_pixels > rectangle.y_pixels) {
    // The completely outside case has already been checked.
    assert_true(cutout->y_pixels < rectangle_bottom);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = rectangle.y_pixels;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      // cutout->y_pixels is already known to be < rectangle_bottom, no need for
      // min(cutout->y_pixels - rectangle.y_pixels, rectangle.height_pixels).
      rectangle_upper.height_pixels = cutout->y_pixels - rectangle.y_pixels;
    }
    ++rectangle_count;
  }
  // Middle part after cutout.
  uint32_t middle_top = std::max(cutout->y_pixels, rectangle.y_pixels);
  uint32_t middle_height =
      std::min(cutout_bottom, rectangle_bottom) - middle_top;
  // Middle left.
  if (cutout->x_pixels > rectangle.x_pixels) {
    assert_true(cutout->x_pixels < rectangle_right);
    if (rectangles_out) {
      Rectangle& rectangle_middle_left = rectangles_out[rectangle_count];
      rectangle_middle_left.x_pixels = rectangle.x_pixels;
      rectangle_middle_left.y_pixels = middle_top;
      rectangle_middle_left.width_pixels =
          cutout->x_pixels - rectangle.x_pixels;
      rectangle_middle_left.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  // Middle right.
  if (cutout_right < rectangle_right) {
    assert_true(cutout_right > rectangle.x_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_middle_right = rectangles_out[rectangle_count];
      rectangle_middle_right.x_pixels = cutout_right;
      rectangle_middle_right.y_pixels = middle_top;
      rectangle_middle_right.width_pixels = rectangle_right - cutout_right;
      rectangle_middle_right.height_pixels = middle_height;
    }
    ++rectangle_count;
  }
  // Lower part after cutout.
  if (cutout_bottom < rectangle_bottom) {
    assert_true(cutout_bottom > rectangle.y_pixels);
    if (rectangles_out) {
      Rectangle& rectangle_upper = rectangles_out[rectangle_count];
      rectangle_upper.x_pixels = rectangle.x_pixels;
      rectangle_upper.y_pixels = cutout_bottom;
      rectangle_upper.width_pixels = rectangle.width_pixels;
      rectangle_upper.height_pixels = rectangle_bottom - cutout_bottom;
    }
    ++rectangle_count;
  }
  assert_true(rectangle_count <= kMaxCutoutBorderRectangles);
  return rectangle_count;
}

RenderTargetCache::~RenderTargetCache() { ShutdownCommon(); }

void RenderTargetCache::InitializeCommon() {
  assert_true(ownership_ranges_.empty());
  ownership_ranges_.emplace(
      std::piecewise_construct, std::forward_as_tuple(uint32_t(0)),
      std::forward_as_tuple(xenos::kEdramTileCount, RenderTargetKey(),
                            RenderTargetKey(), RenderTargetKey()));

  if (cvars::draw_resolution_scale_threshold) {
    if (GetPath() != Path::kHostRenderTargets) {
      XELOGW(
          "draw_resolution_scale_threshold is only supported by the host "
          "render target path - ignoring");
    } else if (!IsDrawResolutionScaled()) {
      XELOGW(
          "draw_resolution_scale_threshold has no effect without "
          "draw_resolution_scale_x/y above 1 - ignoring");
    }
  }
}

void RenderTargetCache::DestroyAllRenderTargets(bool shutting_down) {
  ++ownership_ranges_version_;
  // The render targets and their per-target stencil bits start over as well.
  SetEdramStencilMaybeSetBits(0, xenos::kEdramTileCount, 0);
  ownership_ranges_.clear();
  if (!shutting_down) {
    ownership_ranges_.emplace(
        std::piecewise_construct, std::forward_as_tuple(uint32_t(0)),
        std::forward_as_tuple(xenos::kEdramTileCount, RenderTargetKey(),
                              RenderTargetKey(), RenderTargetKey()));
  }

  render_target_lookup_ = {};
  for (const auto& render_target_pair : render_targets_) {
    if (render_target_pair.second) {
      delete render_target_pair.second;
    }
  }
  render_targets_.clear();
}

void RenderTargetCache::ShutdownCommon() { DestroyAllRenderTargets(true); }

void RenderTargetCache::ClearCache() {
  // Keep only render targets currently owning any EDRAM data.
  if (!render_targets_.empty()) {
    std::unordered_set<RenderTargetKey, RenderTargetKey::Hasher>
        used_render_targets;
    for (const auto& ownership_range_pair : ownership_ranges_) {
      const OwnershipRange& ownership_range = ownership_range_pair.second;
      if (!ownership_range.render_target.IsEmpty()) {
        used_render_targets.emplace(ownership_range.render_target);
      }
      if (!ownership_range.host_depth_render_target_unorm24.IsEmpty()) {
        used_render_targets.emplace(
            ownership_range.host_depth_render_target_unorm24);
      }
      if (!ownership_range.host_depth_render_target_float24.IsEmpty()) {
        used_render_targets.emplace(
            ownership_range.host_depth_render_target_float24);
      }
    }
    if (render_targets_.size() != used_render_targets.size()) {
      // Clear before deleting any object referenced by a direct lookup.
      render_target_lookup_ = {};
      typename decltype(render_targets_)::iterator it_next;
      for (auto it = render_targets_.begin(); it != render_targets_.end();
           it = it_next) {
        it_next = std::next(it);
        if (!it->second) {
          render_targets_.erase(it);
          continue;
        }
        if (used_render_targets.find(it->second->key()) ==
                used_render_targets.end() &&
            !HasExternalRenderTargetReferences(it->second)) {
          delete it->second;
          render_targets_.erase(it);
        }
      }
    }
  }
}

bool RenderTargetCache::IsRenderTargetOwned(RenderTargetKey key,
                                            bool* known_out) const {
  const bool known = !ownership_ranges_.empty();
  if (known_out) {
    *known_out = known;
  }
  // An uninitialized or shut-down map is unknown, never evidence of no owner.
  if (!known) {
    return true;
  }
  for (const auto& ownership_range_pair : ownership_ranges_) {
    const OwnershipRange& ownership_range = ownership_range_pair.second;
    if (ownership_range.render_target == key ||
        ownership_range.host_depth_render_target_unorm24 == key ||
        ownership_range.host_depth_render_target_float24 == key) {
      return true;
    }
  }
  return false;
}

bool RenderTargetCache::HasRetainedRenderTargetReference(
    const RenderTarget* render_target) const {
  if (!render_target) {
    return true;
  }
  for (RenderTarget* used : last_update_used_render_targets_) {
    if (used == render_target) {
      return true;
    }
  }
  for (RenderTarget* accumulated : last_update_accumulated_render_targets_) {
    if (accumulated == render_target) {
      return true;
    }
  }
  if (last_update_clear_retarget_render_target_ == render_target) {
    return true;
  }
  for (const auto& transfers : last_update_transfers_) {
    for (const Transfer& transfer : transfers) {
      if (transfer.source == render_target ||
          transfer.host_depth_source == render_target) {
        return true;
      }
    }
  }
  return false;
}

void RenderTargetCache::BeginFrame() {
  ResetAccumulatedRenderTargets();
  ++height_cap_frame_;
  if (cvars::log_render_target_height_cap && !(height_cap_frame_ % 600)) {
    XELOGI("RtHeightCap: per frame {:.1f} draws capped, {:.1f} needing full "
           "height, {:.1f} kept at a larger class by recent need",
           height_cap_capped_draws_ / 600.0, height_cap_too_tall_draws_ / 600.0,
           height_cap_sticky_draws_ / 600.0);
    height_cap_capped_draws_ = 0;
    height_cap_too_tall_draws_ = 0;
    height_cap_sticky_draws_ = 0;
  }
}

bool RenderTargetCache::IsScaleNativeForPitch(
    uint32_t pitch_tiles_at_32bpp, xenos::MsaaSamples msaa_samples) const {
  uint32_t threshold = cvars::draw_resolution_scale_threshold;
  if (!threshold || !IsDrawResolutionScaled() ||
      GetPath() != Path::kHostRenderTargets) {
    return false;
  }
  // Pitch is the only guest surface dimension that's reliably known since host
  // render target heights are overestimated to cover all EDRAM, and draw height
  // estimates would flip the same surface between classes and churn transfers.
  // Pitch and MSAA are also shared by every surface of a draw so depth and
  // color always land in the same class.
  uint32_t pitch_pixels_tile_aligned =
      RenderTargetKey::GetWidth(pitch_tiles_at_32bpp, msaa_samples);
  return pitch_pixels_tile_aligned != 0 &&
         pitch_pixels_tile_aligned <= threshold;
}

bool RenderTargetCache::IsDrawScaleNative() const {
  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();
  // Same pitch normalization as in Update.
  uint32_t msaa_samples_x_log2 =
      uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t pitch_tiles_at_32bpp =
      ((rb_surface_info.surface_pitch << msaa_samples_x_log2) +
       (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  return IsScaleNativeForPitch(pitch_tiles_at_32bpp,
                               rb_surface_info.msaa_samples);
}

// Stencil bits a draw may set to 1: REPLACE writes the reference, the
// arithmetic and invert operations may set any bit of the write mask, KEEP and
// ZERO set none. Back faces use the front state unless backface_enable.
uint32_t RenderTargetCache::GetDrawStencilMaybeSetBits(
    const RegisterFile& regs, reg::RB_DEPTHCONTROL normalized_depth_control) {
  auto face_bits = [](reg::RB_STENCILREFMASK ref_mask, xenos::StencilOp fail,
                      xenos::StencilOp zfail, xenos::StencilOp zpass) {
    uint32_t bits = 0;
    for (xenos::StencilOp op : {fail, zfail, zpass}) {
      switch (op) {
        case xenos::StencilOp::kKeep:
        case xenos::StencilOp::kZero:
          break;
        case xenos::StencilOp::kReplace:
          bits |= ref_mask.stencilref;
          break;
        default:
          bits |= 0xFF;
          break;
      }
    }
    return bits & ref_mask.stencilwritemask;
  };
  reg::RB_DEPTHCONTROL dc = normalized_depth_control;
  uint32_t bits = face_bits(regs.Get<reg::RB_STENCILREFMASK>(), dc.stencilfail,
                            dc.stencilzfail, dc.stencilzpass);
  if (dc.backface_enable) {
    reg::RB_STENCILREFMASK ref_mask_bf;
    ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    bits |= face_bits(ref_mask_bf, dc.stencilfail_bf, dc.stencilzfail_bf,
                      dc.stencilzpass_bf);
  }
  return bits;
}

bool RenderTargetCache::Update(bool is_rasterization_done,
                               reg::RB_DEPTHCONTROL normalized_depth_control,
                               uint32_t normalized_color_mask,
                               const Shader& vertex_shader) {
  const RegisterFile& regs = register_file();
  bool interlock_barrier_only = GetPath() == Path::kPixelShaderInterlock;
  last_update_full_host_overwrite_ = 0;
  last_update_fully_overwritten_mask_ = 0;
  last_update_draw_cutout_mask_ = 0;
  last_update_draw_cutout_exact_ = false;
  last_update_draw_cutout_depth_aspects_ = 0;
  last_update_clear_retarget_render_target_ = nullptr;
  last_update_clear_retarget_color_draw_ = false;

  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  xenos::MsaaSamples msaa_samples = rb_surface_info.msaa_samples;
  assert_true(msaa_samples <= xenos::MsaaSamples::k4X);
  if (msaa_samples > xenos::MsaaSamples::k4X) {
    // Safety check because a lot of code assumes up to 4x.
    assert_always();
    XELOGE("{}x MSAA requested by the guest, Xenos only supports up to 4x",
           uint32_t(1) << uint32_t(msaa_samples));
    return false;
  }
  uint32_t msaa_samples_x_log2 =
      uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t pitch_pixels = rb_surface_info.surface_pitch;
  // surface_pitch 0 should be handled in disabling rasterization (hopefully
  // it's safe to assume that).
  assert_true(pitch_pixels || !is_rasterization_done);
  if (!pitch_pixels) {
    is_rasterization_done = false;
  } else if (pitch_pixels > xenos::kTexture2DCubeMaxWidthHeight) {
    XELOGE(
        "Surface pitch {} larger than the maximum texture width {} specified "
        "by the guest",
        pitch_pixels, xenos::kTexture2DCubeMaxWidthHeight);
    return false;
  }
  uint32_t pitch_tiles_at_32bpp = ((pitch_pixels << msaa_samples_x_log2) +
                                   (xenos::kEdramTileWidthSamples - 1)) /
                                  xenos::kEdramTileWidthSamples;
  // Scale class of all the surfaces of this draw.
  bool scale_native = IsScaleNativeForPitch(pitch_tiles_at_32bpp, msaa_samples);
  if (!interlock_barrier_only) {
    uint32_t pitch_pixels_tile_aligned_scaled =
        pitch_tiles_at_32bpp *
        (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2) *
        (scale_native ? 1 : draw_resolution_scale_x());
    uint32_t max_render_target_width = GetMaxRenderTargetWidth();
    if (pitch_pixels_tile_aligned_scaled > max_render_target_width) {
      // TODO(Triang3l): If really needed for some game on some device, clamp
      // the pitch and generate multiple ranges (each for every row of tiles)
      // with gaps for padding. Very few PowerVR GPUs have 4096, not 8192, as
      // the limit, though with 8192 (on Mali) the actual limit for Xenia is
      // 8160 because tile padding is stored - but 8192 should be extremely rare
      // anyway.
      XELOGE(
          "Surface pitch aligned to EDRAM tiles and resolution-scaled {} "
          "larger than the maximum host render target width {}",
          pitch_pixels_tile_aligned_scaled, max_render_target_width);
      return false;
    }
  }

  // Get used render targets.
  // [0] is depth / stencil where relevant, [1...4] is color.
  // Depth / stencil testing / writing is before color in the pipeline.
  uint32_t depth_and_color_rts_used_bits = 0;
  // depth_and_color_rts_used_bits -> EDRAM base.
  uint32_t edram_bases[1 + xenos::kMaxColorRenderTargets];
  uint32_t resource_formats[1 + xenos::kMaxColorRenderTargets];
  uint32_t rts_are_64bpp = 0;
  if (is_rasterization_done) {
    if (normalized_depth_control.stencil_enable &&
        !stencil_test_ever_enabled_) {
      stencil_test_ever_enabled_ = true;
      XELOGI(
          "RenderTargetCache: guest stencil test enabled for the first time; "
          "stencil is now transferred between render targets");
    }
    if (normalized_depth_control.z_enable ||
        normalized_depth_control.stencil_enable) {
      depth_and_color_rts_used_bits |= 1;
      auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      edram_bases[0] = rb_depth_info.depth_base;
      // With pixel shader interlock, always the same addressing disregarding
      // the format.
      resource_formats[0] =
          interlock_barrier_only ? 0 : uint32_t(rb_depth_info.depth_format);
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(normalized_color_mask & (uint32_t(0b1111) << (4 * i)))) {
        continue;
      }
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[i]);
      uint32_t rt_bit_index = 1 + i;
      depth_and_color_rts_used_bits |= uint32_t(1) << rt_bit_index;
      edram_bases[rt_bit_index] = color_info.color_base;
      xenos::ColorRenderTargetFormat color_format =
          regs.Get<reg::RB_COLOR_INFO>(
                  reg::RB_COLOR_INFO::rt_register_indices[i])
              .color_format;
      bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_format);
      if (is_64bpp) {
        rts_are_64bpp |= uint32_t(1) << rt_bit_index;
      }
      xenos::ColorRenderTargetFormat color_resource_format;
      if (interlock_barrier_only) {
        // Only changes in mapping between coordinates and addresses are
        // interesting (along with access overlap between draw calls), thus only
        // pixel size is relevant.
        color_resource_format =
            is_64bpp ? xenos::ColorRenderTargetFormat::k_16_16_16_16
                     : xenos::ColorRenderTargetFormat::k_8_8_8_8;
      } else {
        color_resource_format =
            GetColorResourceFormat(xenos::GetStorageColorFormat(color_format));
      }
      resource_formats[rt_bit_index] = uint32_t(color_resource_format);
    }
  }

  uint32_t rts_remaining;
  uint32_t rt_index;

  // Eliminate other bound render targets if their EDRAM base conflicts with
  // another render target - it's an error in most host implementations to bind
  // the same render target into multiple slots, also the behavior would be
  // unpredictable if that happens.
  // Depth is considered the least important as it's earlier in the pipeline
  // (issues caused by color and depth render target collisions haven't been
  // found yet), but render targets with smaller index are considered more
  // important - specifically, because of the usage in the lighting pass of
  // 4D5307E6, which can be checked in the vertical look calibration sequence in
  // the beginning of the game: if render target 0 is removed in favor of 1, the
  // characters and the world will be too dark, like fully in shadow -
  // especially prominent on the helmet. This happens because the shader picks
  // between two render targets to write dynamically (though with a static, bool
  // constant condition), but all other state is set up in a way that implies
  // the same render target being bound twice. On Direct3D 9, if you don't write
  // to a color pixel shader output on the control flow that was taken, the
  // render target will not be written to. However, this has been relaxed in
  // Direct3D 10, where if the shader declares an output, it's assumed to be
  // always written (or with an undefined value otherwise).
  rts_remaining = depth_and_color_rts_used_bits & ~(uint32_t(1));
  while (xe::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(uint32_t(1) << rt_index);
    uint32_t edram_base = edram_bases[rt_index];
    uint32_t rts_other_remaining =
        depth_and_color_rts_used_bits &
        (~((uint32_t(1) << (rt_index + 1)) - 1) | uint32_t(1));
    uint32_t rt_other_index;
    while (xe::bit_scan_forward(rts_other_remaining, &rt_other_index)) {
      rts_other_remaining &= ~(uint32_t(1) << rt_other_index);
      if (edram_bases[rt_other_index] == edram_base) {
        depth_and_color_rts_used_bits &= ~(uint32_t(1) << rt_other_index);
      }
    }
  }

  // Clear ownership transfers before adding any.
  if (!interlock_barrier_only) {
    for (size_t i = 0; i < xe::countof(last_update_transfers_); ++i) {
      last_update_transfers_[i].clear();
    }
  }

  if (!depth_and_color_rts_used_bits) {
    // Nothing to bind, don't waste time on things like memexport-only draws -
    // just check if old bindings can still be used.
    std::memset(last_update_used_render_targets_, 0,
                sizeof(last_update_used_render_targets_));
    if (are_accumulated_render_targets_valid_) {
      for (size_t i = 0;
           i < xe::countof(last_update_accumulated_render_targets_); ++i) {
        const RenderTarget* render_target =
            last_update_accumulated_render_targets_[i];
        if (!render_target) {
          continue;
        }
        RenderTargetKey rt_key = render_target->key();
        if (rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            rt_key.msaa_samples != msaa_samples) {
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    if (!are_accumulated_render_targets_valid_) {
      std::memset(last_update_accumulated_render_targets_, 0,
                  sizeof(last_update_accumulated_render_targets_));
    }
    return true;
  }

  // Estimate height used by render targets (for color for writes, for depth /
  // stencil for both reads and writes) from various sources.
  uint32_t height_used = std::min(
      GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples),
      draw_extent_estimator_.EstimateMaxY(
          interlock_barrier_only
              ? cvars::execute_unclipped_draw_vs_on_cpu_for_psi_render_backend
              : true,
          vertex_shader, draw_samples_as_pixels_ ? 2 : 1));
  last_update_height_used_ = height_used;

  // Sorted by EDRAM base and then by index in the pipeline - for simplicity,
  // treat render targets placed closer to the end of the EDRAM as truncating
  // the previous one (and in case multiple render targets are placed at the
  // same EDRAM base, though normally this shouldn't happen, treat the color
  // ones as more important than the depth one, which may be not needed and just
  // a leftover if the draw, for instance, has depth / stencil happening to be
  // always passing and never writing with the current state, and also because
  // depth testing has to happen before the color is written). Overall it's
  // normal for estimated EDRAM ranges of render targets to intersect if drawing
  // without a viewport (as there's nothing to clamp the estimated height) and
  // multiple render targets are bound.
  std::pair<uint32_t, uint32_t>
      edram_bases_sorted[1 + xenos::kMaxColorRenderTargets];
  uint32_t edram_bases_sorted_count = 0;
  rts_remaining = depth_and_color_rts_used_bits;
  while (xe::bit_scan_forward(rts_remaining, &rt_index)) {
    rts_remaining &= ~(uint32_t(1) << rt_index);
    edram_bases_sorted[edram_bases_sorted_count++] =
        std::make_pair(edram_bases[rt_index], rt_index);
  }
  std::sort(edram_bases_sorted, edram_bases_sorted + edram_bases_sorted_count);
  // "As if it was 64bpp" (contribution of 32bpp render targets multiplied by 2,
  // and clamping for 32bpp render targets divides this by 2) because 32bpp
  // render targets can be combined with twice as long 64bpp render targets. An
  // example is the 4541099D menu background (1-sample 1152x720, or 1200x720
  // after rounding to tiles, with a 32bpp depth buffer at 0 requiring 675
  // tiles, and a 64bpp color buffer at 675 requiring 1350 tiles, but the
  // smallest distance between two render target bases is 675 tiles).
  uint32_t rt_max_distance_tiles_at_64bpp = xenos::kEdramTileCount * 2;
  if (cvars::mrt_edram_used_range_clamp_to_min &&
      edram_bases_sorted_count >= 2) {
    for (uint32_t i = 1; i < edram_bases_sorted_count; ++i) {
      const std::pair<uint32_t, uint32_t>& rt_base_prev =
          edram_bases_sorted[i - 1];
      rt_max_distance_tiles_at_64bpp =
          std::min(rt_max_distance_tiles_at_64bpp,
                   (edram_bases_sorted[i].first - rt_base_prev.first)
                       << (((rts_are_64bpp >> rt_base_prev.second) & 1) ^ 1));
    }
    // Clamp to the distance from the last render target to the first with
    // EDRAM addressing wrapping.
    const std::pair<uint32_t, uint32_t>& rt_base_last =
        edram_bases_sorted[edram_bases_sorted_count - 1];
    rt_max_distance_tiles_at_64bpp =
        std::min(rt_max_distance_tiles_at_64bpp,
                 (xenos::kEdramTileCount + edram_bases_sorted[0].first -
                  rt_base_last.first)
                     << (((rts_are_64bpp >> rt_base_last.second) & 1) ^ 1));
  }

  // Make sure all the needed render targets are created, and gather lengths of
  // ranges used by each render target.
  RenderTargetKey rt_keys[1 + xenos::kMaxColorRenderTargets];
  RenderTarget* rts[1 + xenos::kMaxColorRenderTargets];
  uint32_t rt_lengths_tiles[1 + xenos::kMaxColorRenderTargets];
  uint32_t length_used_tiles_at_32bpp =
      ((height_used << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X)) +
       (xenos::kEdramTileHeightSamples - 1)) /
      xenos::kEdramTileHeightSamples * pitch_tiles_at_32bpp;
  // Immutable host image height class for this draw: full or base cap.
  uint32_t height_class = kRenderTargetHeightClassFull;
  // Whether this draw itself needs full height (then the full-height render
  // targets are marked as recently needed).
  bool height_cap_too_tall = false;
  {
    static const uint32_t cap = cvars::render_target_height_cap & ~uint32_t(15);
    const uint32_t full_height =
        GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples);
    const uint32_t msaa_samples_y_log2 =
        uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t rows_claimed =
        ((height_used << msaa_samples_y_log2) +
         (xenos::kEdramTileHeightSamples - 1)) /
        xenos::kEdramTileHeightSamples *
        (xenos::kEdramTileHeightSamples >> msaa_samples_y_log2);
    if (cap && !interlock_barrier_only &&
        GetPath() == Path::kHostRenderTargets && full_height > cap) {
      const uint32_t actual_height_class =
          rows_claimed > cap ? kRenderTargetHeightClassFull
                             : kRenderTargetHeightClassBaseCap;
      height_cap_too_tall = rows_claimed > cap;

      if (height_cap_too_tall) {
        ++height_cap_too_tall_draws_;
        if (cvars::log_render_target_height_cap &&
            height_cap_too_tall_logged_ < 64) {
          ++height_cap_too_tall_logged_;
          const RegisterFile& regs = register_file();
          XELOGI(
              "RtHeightCap: draw needs {} rows (pitch {} tiles, msaa {}, base "
              "{}): window scissor br_y {} screen scissor br_y {} clip_disable "
              "{} vte 0x{:X} vport y scale {} offset {}",
              rows_claimed, pitch_tiles_at_32bpp, uint32_t(msaa_samples),
              edram_bases_sorted_count ? edram_bases_sorted[0].first : 0,
              regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y,
              regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>().br_y,
              regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable,
              regs.Get<reg::PA_CL_VTE_CNTL>().value,
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET));
        }
      }

      if (actual_height_class != kRenderTargetHeightClassFull) {
        height_class = kRenderTargetHeightClassBaseCap;
        // A full-height target at any used base stays selected while it was
        // actually needed recently, avoiding repeated ownership transfers.
        constexpr uint64_t kFullHeightRecentFrames = 60;
        for (uint32_t i = 0; i < edram_bases_sorted_count &&
                             height_class != kRenderTargetHeightClassFull;
             ++i) {
          RenderTargetKey full_key;
          full_key.base_tiles = edram_bases_sorted[i].first;
          full_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
          full_key.msaa_samples = msaa_samples;
          full_key.is_depth = edram_bases_sorted[i].second == 0;
          full_key.resource_format =
              resource_formats[edram_bases_sorted[i].second];
          full_key.scale_native = uint32_t(scale_native);
          auto it_full = render_targets_.find(full_key);
          if (it_full != render_targets_.end() && it_full->second &&
              height_cap_frame_ - it_full->second->last_full_height_need_frame <
                  kFullHeightRecentFrames) {
            height_class = kRenderTargetHeightClassFull;
          }
        }
        if (height_class == kRenderTargetHeightClassBaseCap) {
          ++height_cap_capped_draws_;
        } else {
          ++height_cap_sticky_draws_;
        }
      } else {
        height_class = kRenderTargetHeightClassFull;
      }
    }
  }
  for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];
    uint32_t rt_base = rt_base_index.first;
    uint32_t rt_bit_index = rt_base_index.second;
    RenderTargetKey& rt_key = rt_keys[rt_bit_index];
    rt_key.height_class = height_class;
    rt_key.base_tiles = rt_base;
    rt_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    rt_key.msaa_samples = msaa_samples;
    rt_key.is_depth = rt_bit_index == 0;
    rt_key.resource_format = resource_formats[rt_bit_index];
    rt_key.scale_native = uint32_t(scale_native);
    if (!interlock_barrier_only) {
      RenderTarget* render_target = GetOrCreateRenderTarget(rt_key);
      if (!render_target) {
        return false;
      }
      rts[rt_bit_index] = render_target;
      if (height_cap_too_tall) {
        render_target->last_full_height_need_frame = height_cap_frame_;
      }
    }
    uint32_t rt_is_64bpp = (rts_are_64bpp >> rt_bit_index) & 1;
    // The last render target can occupy the EDRAM until the base of the first
    // render target (itself in case of 1 render target) with EDRAM addressing
    // wrapping.
    rt_lengths_tiles[i] = std::min(
        std::min(length_used_tiles_at_32bpp << rt_is_64bpp,
                 rt_max_distance_tiles_at_64bpp >> (rt_is_64bpp ^ 1)),
        ((i + 1 < edram_bases_sorted_count)
             ? edram_bases_sorted[i + 1].first
             : (xenos::kEdramTileCount + edram_bases_sorted[0].first)) -
            rt_base);
  }

  if (!interlock_barrier_only) {
    // Before anything may skip, retarget or split the draw: whatever it ends
    // up as, its claimed ranges may now hold what it can write. Any color
    // write makes the low bytes unknown; depth writes leave the stencil alone.
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      uint32_t written_bits =
          edram_bases_sorted[i].second
              ? (cvars::rt_cache_color_draws_dirty_stencil ? 0xFF : 0)
              : (normalized_depth_control.stencil_enable
                     ? GetDrawStencilMaybeSetBits(regs, normalized_depth_control)
                     : 0);
      EdramStencilBitsMemo& bits_memo =
          edram_stencil_bits_memos_[edram_bases_sorted[i].second];
      if (bits_memo.version == edram_stencil_bits_version_ &&
          bits_memo.start_tiles == edram_bases_sorted[i].first &&
          bits_memo.length_tiles >= rt_lengths_tiles[i] &&
          !(written_bits & ~bits_memo.bits)) {
        continue;
      }
      AddEdramStencilMaybeSetBits(edram_bases_sorted[i].first,
                                  rt_lengths_tiles[i], written_bits);
      bits_memo.start_tiles = edram_bases_sorted[i].first;
      bits_memo.length_tiles = rt_lengths_tiles[i];
      bits_memo.bits = written_bits & 0xFF;
      bits_memo.version = edram_stencil_bits_version_;
    }
  }

  if (interlock_barrier_only) {
    // Because a full pixel shader interlock barrier may clear the ownership map
    // (since it flushes all previous writes, and there's no need for another
    // barrier if an overlap is encountered later between pre-barrier and
    // post-barrier usages), check if any overlap requiring a barrier happens,
    // and then insert the barrier if needed.
    bool interlock_barrier_needed = false;
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      const std::pair<uint32_t, uint32_t>& rt_base_index =
          edram_bases_sorted[i];
      if (WouldOwnershipChangeRequireTransfers(rt_keys[rt_base_index.second], 0,
                                               rt_lengths_tiles[i])) {
        interlock_barrier_needed = true;
        break;
      }
    }
    if (interlock_barrier_needed) {
      RequestPixelShaderInterlockBarrier();
    }
  }

  // Render targets whose claimed range this draw overwrites entirely don't
  // need the previous contents transferred into them.
  uint32_t rts_fully_overwritten =
      interlock_barrier_only
          ? 0
          : GetFullyOverwrittenRenderTargets(
                depth_and_color_rts_used_bits, normalized_depth_control,
                normalized_color_mask, vertex_shader, pitch_pixels,
                height_used);
  if (update_draw_may_be_skipped_) {
    if (rts_fully_overwritten && ++skipped_full_overwrite_logged_ <= 32) {
      XELOGI("RTCLEAR: preserving full-overwrite contents for a skipped draw "
             "mask={:X}", rts_fully_overwritten);
    }
    // Ownership changes still retain their transfers. The Vulkan backend
    // flushes unconfirmed draws without cutouts before the next update/resolve.
    rts_fully_overwritten = 0;
    last_update_full_host_overwrite_ = 0;
  }
  last_update_fully_overwritten_mask_ = rts_fully_overwritten;
  if ((rts_fully_overwritten & 1) && last_full_clear_depth_value_valid_ &&
      !last_full_clear_depth_only_) {
    // Every sample of the claimed depth range gets this one stencil value (the
    // same proof the skipped transfers rely on).
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      if (!edram_bases_sorted[i].second) {
        SetEdramStencilMaybeSetBits(edram_bases_sorted[i].first,
                                    rt_lengths_tiles[i],
                                    last_full_clear_depth_value_ & 0xFF);
      }
    }
  }

  // A depth-only clear of a known value covering the whole claimed range, while
  // the range is held by a depth render target differing only in the sample
  // count: clear that render target instead, keeping the ownership, rather
  // than transferring the range into the clear's render target and back.
  // With color also written, only the depth / stencil part is retargeted, and
  // the draw is done for color only.
  uint32_t depth_sorted_index = edram_bases_sorted_count;
  for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
    if (edram_bases_sorted[i].second == 0) {
      depth_sorted_index = i;
      break;
    }
  }
  // The retargeted draw is skipped, so it must have no effect but the clear
  // (no memory export).
  if (cvars::rt_cache_retarget_full_depth_clears && !interlock_barrier_only &&
      !vertex_shader.memexport_eM_written() &&
      !(update_pixel_shader_ && update_pixel_shader_->memexport_eM_written()) &&
      (depth_and_color_rts_used_bits & 1) &&
      depth_sorted_index < edram_bases_sorted_count &&
      last_full_clear_depth_value_valid_) {
    const RenderTargetKey& clear_key = rt_keys[0];
    uint32_t clear_start_tiles = clear_key.base_tiles;
    uint32_t clear_length_tiles = rt_lengths_tiles[depth_sorted_index];
    uint32_t clear_pitch_tiles = clear_key.pitch_tiles_at_32bpp;
    const char* retarget_reject = nullptr;
    if (!clear_length_tiles || !clear_pitch_tiles ||
        (clear_length_tiles % clear_pitch_tiles) ||
        clear_start_tiles + clear_length_tiles > xenos::kEdramTileCount) {
      retarget_reject = "range not whole rows";
    } else {
      auto owner_it = ownership_ranges_.upper_bound(clear_start_tiles);
      if (owner_it == ownership_ranges_.begin()) {
        retarget_reject = "no owner";
      } else {
        --owner_it;
        RenderTargetKey owner_key = owner_it->second.render_target;
        auto owner_rt_it = render_targets_.end();
        if (owner_it->second.end_tiles <
            clear_start_tiles + clear_length_tiles) {
          retarget_reject = "multiple owners";
        } else if (owner_key.IsEmpty() || owner_key == clear_key) {
          retarget_reject = "owned by the clear's render target";
        } else if (!owner_key.is_depth ||
                   owner_key.base_tiles != clear_key.base_tiles ||
                   owner_key.pitch_tiles_at_32bpp != clear_pitch_tiles ||
                   owner_key.resource_format != clear_key.resource_format ||
                   owner_key.scale_native != clear_key.scale_native) {
          retarget_reject = "owner layout / format differs";
        } else if (IsHostDepthEncodingDifferent(owner_key.GetDepthFormat()) &&
                   owner_it->second.GetHostDepthRenderTarget(
                       owner_key.GetDepthFormat()) != owner_key) {
          // The precise host depth of the range is in another render target -
          // a clear only in the owner would desynchronize them.
          retarget_reject = "host depth held by another render target";
        } else if ((owner_rt_it = render_targets_.find(owner_key)) ==
                       render_targets_.end() ||
                   !owner_rt_it->second) {
          retarget_reject = "owner render target missing";
        } else {
          // The same EDRAM samples in the owner's pixels - the constant is
          // the same for every sample, so only the covered sample set matters,
          // which must consist of whole owner pixels.
          uint32_t clear_msaa_x_log2 =
              uint32_t(clear_key.msaa_samples >= xenos::MsaaSamples::k4X);
          uint32_t clear_msaa_y_log2 =
              uint32_t(clear_key.msaa_samples >= xenos::MsaaSamples::k2X);
          uint32_t owner_msaa_x_log2 =
              uint32_t(owner_key.msaa_samples >= xenos::MsaaSamples::k4X);
          uint32_t owner_msaa_y_log2 =
              uint32_t(owner_key.msaa_samples >= xenos::MsaaSamples::k2X);
          uint32_t samples_x0 = last_clear_rect_[0] << clear_msaa_x_log2;
          uint32_t samples_y0 = last_clear_rect_[1] << clear_msaa_y_log2;
          uint32_t samples_x1 = last_clear_rect_[2] << clear_msaa_x_log2;
          uint32_t samples_y1 = last_clear_rect_[3] << clear_msaa_y_log2;
          uint32_t owner_x_mask = (uint32_t(1) << owner_msaa_x_log2) - 1;
          uint32_t owner_y_mask = (uint32_t(1) << owner_msaa_y_log2) - 1;
          uint32_t owner_width_pixels =
              clear_pitch_tiles *
              (xenos::kEdramTileWidthSamples >> owner_msaa_x_log2);
          uint32_t owner_height_pixels = std::min(
              clear_length_tiles / clear_pitch_tiles *
                  (xenos::kEdramTileHeightSamples >> owner_msaa_y_log2),
              GetRenderTargetHeight(owner_key));
          uint32_t owner_x0 = samples_x0 >> owner_msaa_x_log2;
          uint32_t owner_y0 = samples_y0 >> owner_msaa_y_log2;
          uint32_t owner_x1 =
              std::min(samples_x1 >> owner_msaa_x_log2, owner_width_pixels);
          uint32_t owner_y1 =
              std::min(samples_y1 >> owner_msaa_y_log2, owner_height_pixels);
          if (((samples_x0 | samples_x1) & owner_x_mask) ||
              ((samples_y0 | samples_y1) & owner_y_mask)) {
            retarget_reject = "partial owner pixels";
          } else if (owner_x0 >= owner_x1 || owner_y0 >= owner_y1) {
            retarget_reject = "empty";
          } else {
            Transfer::Rectangle& rect = last_update_clear_retarget_rectangle_;
            rect.x_pixels = owner_x0;
            rect.y_pixels = owner_y0;
            rect.width_pixels = owner_x1 - owner_x0;
            rect.height_pixels = owner_y1 - owner_y0;
            last_update_clear_retarget_render_target_ = owner_rt_it->second;
            last_update_clear_retarget_value_ =
                last_full_clear_depth_value_ |
                (last_full_clear_depth_only_ ? kResolveClearDepthOnlyBit : 0) |
                (last_full_clear_stencil_only_ ? kResolveClearStencilOnlyBit
                                               : 0);
          }
        }
      }
    }
    if (clear_retarget_logged_ <
        uint32_t(std::max(0, cvars::log_full_clear_transfer_skips))) {
      ++clear_retarget_logged_;
      if (last_update_clear_retarget_render_target_) {
        RenderTargetKey owner_key =
            last_update_clear_retarget_render_target_->key();
        XELOGI(
            "RTCLEAR: retargeting clear of {}x MSAA depth at {} (pitch {}, "
            "{} tiles) to its {}x MSAA owner, value 0x{:08X}, rect {}x{} at "
            "{},{}",
            uint32_t(1) << uint32_t(clear_key.msaa_samples), clear_start_tiles,
            clear_pitch_tiles, clear_length_tiles,
            uint32_t(1) << uint32_t(owner_key.msaa_samples),
            last_full_clear_depth_value_,
            last_update_clear_retarget_rectangle_.width_pixels,
            last_update_clear_retarget_rectangle_.height_pixels,
            last_update_clear_retarget_rectangle_.x_pixels,
            last_update_clear_retarget_rectangle_.y_pixels);
      } else {
        XELOGI("RTCLEAR: not retargeting clear at {} (pitch {}, {} tiles): {}",
               clear_start_tiles, clear_pitch_tiles, clear_length_tiles,
               retarget_reject ? retarget_reject : "?");
      }
    }
    if (last_update_clear_retarget_render_target_) {
      last_update_clear_retarget_render_target_->ForgetConstant();
      if (depth_and_color_rts_used_bits == 1) {
        // The implementation performs the clear, and the draw is skipped.
        return true;
      }
      // Set up the render targets for the color part of the draw, with depth /
      // stencil disabled - the clear draw's depth and stencil tests always
      // pass, so the color written doesn't depend on them.
      RenderTarget* retarget_render_target =
          last_update_clear_retarget_render_target_;
      Transfer::Rectangle retarget_rectangle =
          last_update_clear_retarget_rectangle_;
      uint64_t retarget_value = last_update_clear_retarget_value_;
      reg::RB_DEPTHCONTROL color_only_depth_control = normalized_depth_control;
      color_only_depth_control.z_enable = 0;
      color_only_depth_control.stencil_enable = 0;
      // Not through the virtual call: the implementation is in its own
      // Update already and processes the result once this returns - a nested
      // implementation update would perform the color transfers, and then the
      // outer one would perform them again (and flush the draw clear chain
      // the nested one had just started, transferring everything).
      if (!(cvars::rt_cache_legacy_clear_paths
                ? Update(is_rasterization_done, color_only_depth_control,
                         normalized_color_mask, vertex_shader)
                : RenderTargetCache::Update(is_rasterization_done,
                                            color_only_depth_control,
                                            normalized_color_mask,
                                            vertex_shader))) {
        return false;
      }
      last_update_clear_retarget_render_target_ = retarget_render_target;
      last_update_clear_retarget_rectangle_ = retarget_rectangle;
      last_update_clear_retarget_value_ = retarget_value;
      last_update_clear_retarget_color_draw_ = true;
      return true;
    }
  }

  // From now on ownership transfers should succeed for simplicity and
  // consistency, even if they fail in the implementation (just ignore that and
  // draw with whatever contents currently are in the render target in this
  // case).

  for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
    const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];
    uint32_t rt_bit_index = rt_base_index.second;
    RenderTargetKey rt_key = rt_keys[rt_bit_index];
    uint32_t rt_length_tiles = rt_lengths_tiles[i];
    if (rt_trace_claims_ && !interlock_barrier_only) {
      const OwnershipClaimMemo& rt_trace_memo =
          ownership_claim_memos_[rt_bit_index];
      const bool rt_trace_memo_hit =
          cvars::rt_cache_ownership_claim_memo &&
          rt_trace_memo.version == ownership_ranges_version_ &&
          rt_trace_memo.dest == rt_key &&
          rt_length_tiles <= rt_trace_memo.length_tiles;
      if (!rt_trace_memo_hit) {
        XELOGI(
            "RTTRACE claim slot={} key={}/{}/{}/msaa{}/depth{} tiles=[{},{}) "
            "height_used={}",
            rt_bit_index, rt_key.base_tiles, rt_key.GetPitchTiles(),
            rt_key.resource_format, uint32_t(rt_key.msaa_samples),
            uint32_t(rt_key.is_depth), rt_key.base_tiles,
            rt_key.base_tiles + rt_length_tiles, height_used);
      }
    }
    if (cvars::rt_cache_ownership_claim_memo) {
      OwnershipClaimMemo& memo = ownership_claim_memos_[rt_bit_index];
      if (memo.version == ownership_ranges_version_ && memo.dest == rt_key &&
          rt_length_tiles <= memo.length_tiles) {
        // This render target already owns [base, base + memo.length_tiles)
        // and the ownership map hasn't changed since - the walk would change
        // nothing and append no transfers (the slot's last_update_transfers_
        // was cleared above, matching the no-op outcome).
        continue;
      }
      ChangeOwnership(rt_key, 0, rt_length_tiles,
                      (interlock_barrier_only ||
                       (rts_fully_overwritten & (uint32_t(1) << rt_bit_index)))
                          ? nullptr
                          : &last_update_transfers_[rt_bit_index]);
      // Record with the post-claim version: if the claim itself mutated the
      // map, ownership now reflects this claim at the bumped version, while
      // any later mutation (including another slot's claim) bumps again and
      // naturally invalidates this memo.
      memo.dest = rt_key;
      memo.length_tiles = rt_length_tiles;
      memo.version = ownership_ranges_version_;
    } else {
      ChangeOwnership(rt_key, 0, rt_length_tiles,
                      (interlock_barrier_only ||
                       (rts_fully_overwritten & (uint32_t(1) << rt_bit_index)))
                          ? nullptr
                          : &last_update_transfers_[rt_bit_index]);
    }
  }

  // rt_cache_transfer_known_constants: a draw writing one exact depth and / or
  // stencil value over the whole claimed range leaves that EDRAM word (or the
  // written part of it) in its depth render target.
  constant_set_by_update_ = nullptr;
  if (cvars::rt_cache_transfer_known_constants && !interlock_barrier_only &&
      (depth_and_color_rts_used_bits & 1) && rts[0] &&
      !update_draw_may_be_skipped_ && last_draw_covers_claimed_range_ &&
      last_full_clear_depth_value_valid_) {
    uint32_t depth_length_tiles = 0;
    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {
      if (!edram_bases_sorted[i].second) {
        depth_length_tiles = rt_lengths_tiles[i];
      }
    }
    uint32_t mask = last_full_clear_depth_only_
                        ? UINT32_C(0xFFFFFF00)
                        : (last_full_clear_stencil_only_ ? UINT32_C(0xFF)
                                                         : UINT32_MAX);
    uint32_t word = last_full_clear_depth_value_ & mask;
    uint32_t old_word, old_mask;
    if (mask != UINT32_MAX &&
        rts[0]->GetConstant(0, depth_length_tiles, old_word, old_mask)) {
      word |= old_word & old_mask & ~mask;
      mask |= old_mask;
    }
    rts[0]->SetConstant(0, depth_length_tiles, word, mask);
    constant_set_by_update_ = rts[0];
  }

  // GetFullyOverwrittenRenderTargets has already proven that the rectangle
  // writes a constant depth AND every stencil bit, with no rejected samples.
  // The whole claimed tile range may include untouched pitch/row padding, so
  // keep ownership transfers, but let the backend cut out only the clear's
  // pixel rectangle. This also works when the range has multiple old owners.
  if (cvars::rt_cache_clear_new_depth_owners &&
      SupportsNewOwnerClearTransfers() && !interlock_barrier_only &&
      depth_and_color_rts_used_bits == 1 && rts[0] &&
      !vertex_shader.memexport_eM_written() &&
      !(update_pixel_shader_ && update_pixel_shader_->memexport_eM_written()) &&
      last_full_clear_depth_value_valid_ && !last_full_clear_depth_only_ &&
      !last_full_clear_stencil_only_ &&
      !last_update_transfers_[0].empty()) {
    const RenderTargetKey& clear_key = rt_keys[0];
    const uint32_t width = clear_key.GetPitchTiles() *
        (xenos::kEdramTileWidthSamples >>
         uint32_t(clear_key.msaa_samples >= xenos::MsaaSamples::k4X));
    const uint32_t height = GetRenderTargetHeight(clear_key);
    const uint32_t x0 = std::min(last_clear_rect_[0], width);
    const uint32_t y0 = std::min(last_clear_rect_[1], height);
    const uint32_t x1 = std::min(last_clear_rect_[2], width);
    const uint32_t y1 = std::min(last_clear_rect_[3], height);
    if (x0 < x1 && y0 < y1) {
      last_update_clear_retarget_render_target_ = rts[0];
      rts[0]->ForgetConstant();
      constant_set_by_update_ = nullptr;
      last_update_clear_retarget_rectangle_ = {x0, y0, x1 - x0, y1 - y0};
      last_update_clear_retarget_value_ = last_full_clear_depth_value_;
      if (rt_trace_claims_) {
        XELOGI("RTTRACE clear_new_owner cutout=[{},{},{},{}] transfers={}",
               x0, y0, x1 - x0, y1 - y0, last_update_transfers_[0].size());
      }
      return true;
    }
  }

  // The draw's rectangle is overwritten anyway in the render targets it writes
  // completely - their transfers (if any) may skip it.
  if (!interlock_barrier_only && cvars::rt_cache_draw_overwrite_cutouts) {
    last_update_draw_cutout_mask_ = last_draw_overwrite_mask_ &
                                    depth_and_color_rts_used_bits &
                                    ~rts_fully_overwritten;
    last_update_draw_cutout_ = last_draw_overwrite_rect_;
    last_update_draw_cutout_exact_ = last_draw_overwrite_exact_;
    last_update_draw_cutout_depth_aspects_ =
        (depth_and_color_rts_used_bits & 1) ? last_draw_overwrite_depth_aspects_
                                            : 0;
  }

  if (interlock_barrier_only) {
    // No copying transfers or render target bindings - only needed the barrier.
    return true;
  }

  // If everything succeeded, update the used render targets.
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    last_update_used_render_targets_[i] =
        (depth_and_color_rts_used_bits & (uint32_t(1) << i)) ? rts[i] : nullptr;
  }
  if ((depth_and_color_rts_used_bits & 1) && rts[0] &&
      normalized_depth_control.stencil_enable) {
    uint32_t stencil_bits =
        GetDrawStencilMaybeSetBits(regs, normalized_depth_control);
    if (stencil_bits & ~rts[0]->stencil_maybe_set_bits()) {
      static uint32_t stencil_bits_draw_logs = 0;
      if (stencil_bits_draw_logs < 48) {
        ++stencil_bits_draw_logs;
        reg::RB_DEPTHCONTROL dc = normalized_depth_control;
        auto ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
        XELOGI(
            "StencilBits: draw adds 0x{:02X} to depth RT @{} (had 0x{:02X}) "
            "ops f/zf/zp {}/{}/{} ref 0x{:02X} wm 0x{:02X} bf {}",
            stencil_bits & ~rts[0]->stencil_maybe_set_bits(),
            uint32_t(rts[0]->key().base_tiles), rts[0]->stencil_maybe_set_bits(),
            uint32_t(dc.stencilfail), uint32_t(dc.stencilzfail),
            uint32_t(dc.stencilzpass), uint32_t(ref_mask.stencilref),
            uint32_t(ref_mask.stencilwritemask), uint32_t(dc.backface_enable));
      }
    }
    rts[0]->AddStencilMaybeSetBits(stencil_bits);
  }
  if (are_accumulated_render_targets_valid_) {
    // Check if the only re-enabling a previously bound render target.
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      RenderTarget* current_rt =
          (depth_and_color_rts_used_bits & (uint32_t(1) << i)) ? rts[i]
                                                               : nullptr;
      const RenderTarget* accumulated_rt =
          last_update_accumulated_render_targets_[i];
      if (!accumulated_rt) {
        if (current_rt) {
          // Binding a totally new render target - won't keep the existing
          // render pass anyway, no much need to try to re-enable previously
          // disabled render targets in other slots as well, even though that
          // would be valid.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
        // Append the new render target.
        last_update_accumulated_render_targets_[i] = current_rt;
        continue;
      }
      if (current_rt) {
        if (current_rt != accumulated_rt) {
          // Changing a render target in a slot.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      } else {
        RenderTargetKey accumulated_rt_key = accumulated_rt->key();
        if (accumulated_rt_key.pitch_tiles_at_32bpp != pitch_tiles_at_32bpp ||
            accumulated_rt_key.msaa_samples != msaa_samples) {
          // The previously bound render target is incompatible with the
          // current surface info.
          are_accumulated_render_targets_valid_ = false;
          break;
        }
      }
    }
    // Make sure the same render target isn't bound into two different slots
    // over time.
    // chrispy: this needs optimization!
    if (are_accumulated_render_targets_valid_) {
      for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        const RenderTarget* render_target =
            last_update_accumulated_render_targets_[i];
        if (!render_target) {
          continue;
        }
        for (uint32_t j = 0; j < i; ++j) {
          if (last_update_accumulated_render_targets_[j] == render_target) {
            are_accumulated_render_targets_valid_ = false;
            goto exit_slot_check_loop;
          }
        }
      }
    exit_slot_check_loop:;
    }
  }
  if (!are_accumulated_render_targets_valid_) {
    std::memcpy(last_update_accumulated_render_targets_,
                last_update_used_render_targets_,
                sizeof(last_update_accumulated_render_targets_));
    are_accumulated_render_targets_valid_ = true;
  }

  return true;
}

uint32_t RenderTargetCache::GetFullyOverwrittenRenderTargets(
    uint32_t rts_used_bits, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader,
    uint32_t pitch_pixels, uint32_t height_used) {
  last_full_clear_depth_value_valid_ = false;
  last_full_clear_depth_only_ = false;
  last_full_clear_stencil_only_ = false;
  last_draw_covers_claimed_range_ = false;
  last_draw_overwrite_mask_ = 0;
  last_draw_overwrite_depth_aspects_ = 0;
  last_draw_overwrite_exact_ = false;
  if (!cvars::rt_cache_skip_transfers_for_full_clears || !rts_used_bits) {
    return 0;
  }
  const RegisterFile& regs = register_file();

  // A single rectangle - its three vertices bound it exactly if it's
  // screen-aligned, which is checked below - or a screen-aligned quad of two
  // triangles (full-screen post-processing and composition passes).
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  // Vertex positions (in drawing order) of the two corners that only one of
  // the two triangles uses - UINT32_MAX for the rectangle list.
  uint32_t quad_lone_corner_a = UINT32_MAX, quad_lone_corner_b = UINT32_MAX;
  uint32_t overwrite_vertex_count = 3;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kRectangleList &&
      vgt_draw_initiator.num_indices == 3) {
  } else if (cvars::rt_cache_full_overwrite_quads &&
             vgt_draw_initiator.prim_type ==
                 xenos::PrimitiveType::kTriangleStrip &&
             vgt_draw_initiator.num_indices == 4) {
    // 012, 123.
    overwrite_vertex_count = 4;
    quad_lone_corner_a = 0;
    quad_lone_corner_b = 3;
  } else if (cvars::rt_cache_full_overwrite_quads &&
             (vgt_draw_initiator.prim_type ==
                  xenos::PrimitiveType::kTriangleFan ||
              vgt_draw_initiator.prim_type ==
                  xenos::PrimitiveType::kQuadList) &&
             vgt_draw_initiator.num_indices == 4) {
    // 012, 023.
    overwrite_vertex_count = 4;
    quad_lone_corner_a = 1;
    quad_lone_corner_b = 3;
  } else if (cvars::rt_cache_full_overwrite_quads &&
             vgt_draw_initiator.prim_type ==
                 xenos::PrimitiveType::kTriangleList &&
             vgt_draw_initiator.num_indices == 6) {
    // The corners are found from the positions below.
    overwrite_vertex_count = 6;
  } else {
    return 0;
  }
  // Rectangles that could be clears - log why they aren't used.
  auto reject = [&](const char* reason, uint32_t detail = 0) -> uint32_t {
    if (rt_trace_claims_ ||
        full_overwrite_logged_ <
            uint32_t(std::max(0, cvars::log_full_clear_transfer_skips))) {
      ++full_overwrite_logged_;
      XELOGI("RTCLEAR: rectangle not a full clear: {} (0x{:X}), zctl=0x{:X}",
             reason, detail, normalized_depth_control.value);
    }
    return 0;
  };
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth &&
      edram_mode != xenos::EdramMode::kDepthOnly) {
    return reject("edram mode", uint32_t(edram_mode));
  }
  // Nothing may drop the rectangle or some of its samples.
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  // For the two-triangle quads, the facing of both is checked once their
  // vertices are known.
  if (((pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back) &&
       overwrite_vertex_count == 3) ||
      pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode) {
    return reject("culling / polygon mode", pa_su_sc_mode_cntl.value);
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  // Alpha test and alpha to coverage act on the pixel shader's color output:
  // a draw without an effective pixel shader (none bound, or one writing
  // nothing that matters, see IsPixelShaderNeededWithRasterization) can't drop
  // anything through them.
  if (((rb_colorcontrol.alpha_test_enable &&
        rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
       rb_colorcontrol.alpha_to_mask_enable) &&
      (update_pixel_shader_ ||
       !cvars::rt_cache_full_overwrite_alpha_without_pixel_shader)) {
    return reject("alpha test / to mask", rb_colorcontrol.value);
  }
  if (update_pixel_shader_ && update_pixel_shader_->kills_pixels()) {
    return reject("pixel shader kills");
  }

  uint32_t result = 0;
  bool depth_only = false;
  bool stencil_only = false;
  // Only one aspect written entirely, the other neither read nor written
  // (rt_cache_retarget_depth_only_clears, and the per-aspect clear chains).
  bool depth_only_written = false;
  bool stencil_only_written = false;
  if (rts_used_bits & 1) {
    reg::RB_DEPTHCONTROL dc = normalized_depth_control;
    bool depth_written = dc.z_enable && dc.z_write_enable &&
                         dc.zfunc == xenos::CompareFunction::kAlways;
    auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
    bool stencil_written = dc.stencil_enable &&
                           dc.stencilfunc == xenos::CompareFunction::kAlways &&
                           dc.stencilzpass == xenos::StencilOp::kReplace &&
                           stencil_ref_mask.stencilwritemask == 0xFF;
    if (stencil_written && dc.backface_enable) {
      reg::RB_STENCILREFMASK stencil_ref_mask_bf;
      stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
      stencil_written =
          dc.stencilfunc_bf == xenos::CompareFunction::kAlways &&
          dc.stencilzpass_bf == xenos::StencilOp::kReplace &&
          stencil_ref_mask_bf.stencilwritemask == 0xFF;
    }
    if (depth_written && stencil_written) {
      result |= 1;
    } else {
      // Not a full overwrite (one aspect must be kept), but the written aspect
      // may be retargeted as a clear of only that aspect, or chained.
      if (depth_written && !dc.stencil_enable) {
        depth_only_written = true;
      } else if (stencil_written && !(dc.z_enable && dc.z_write_enable) &&
                 (!dc.z_enable ||
                  dc.zfunc == xenos::CompareFunction::kAlways)) {
        // The depth test always passes, so the stencil pass op applies.
        stencil_only_written = true;
      }
      if (cvars::rt_cache_retarget_full_depth_clears &&
          cvars::rt_cache_retarget_depth_only_clears) {
        depth_only = depth_only_written;
        stencil_only = stencil_only_written;
      }
    }
  }
  // Color is written to every covered pixel only if no depth or stencil test
  // can fail there - a full-screen rectangle drawn with a depth test (a sky at
  // the far plane, a light volume) is not a color clear.
  bool depth_stencil_tests_always_pass = true;
  if (rts_used_bits & 1) {
    reg::RB_DEPTHCONTROL dc = normalized_depth_control;
    if (dc.z_enable && dc.zfunc != xenos::CompareFunction::kAlways) {
      depth_stencil_tests_always_pass = false;
    }
    if (dc.stencil_enable &&
        (dc.stencilfunc != xenos::CompareFunction::kAlways ||
         (dc.backface_enable &&
          dc.stencilfunc_bf != xenos::CompareFunction::kAlways))) {
      depth_stencil_tests_always_pass = false;
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!depth_stencil_tests_always_pass) {
      break;
    }
    if (!(rts_used_bits & (uint32_t(1) << (1 + i))) ||
        ((normalized_color_mask >> (4 * i)) & 0b1111) != 0b1111) {
      continue;
    }
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    if (blend_control.color_srcblend != xenos::BlendFactor::kOne ||
        blend_control.color_destblend != xenos::BlendFactor::kZero ||
        blend_control.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend_control.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend_control.alpha_destblend != xenos::BlendFactor::kZero ||
        blend_control.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      continue;
    }
    result |= uint32_t(1) << (1 + i);
  }
  if (!result && !depth_only_written && !stencil_only_written) {
    return reject("depth/stencil or color not fully written",
                  regs.Get<reg::RB_STENCILREFMASK>().value);
  }

  // Where the rectangle lands.
  float xs[6], ys[6];
  float min_x, min_y, max_x, max_y, min_z, max_z;
  uint32_t bounds_failure = 0;
  if (!draw_extent_estimator_.EstimateVertexBounds(
          vertex_shader, overwrite_vertex_count, min_x, min_y, max_x, max_y,
          min_z, max_z, xs, ys, &bounds_failure)) {
    return reject(
        bounds_failure ==
                DrawExtentEstimator::kInputReadRejectedFailureReason
            ? "vertex input validation"
            : "vertex evaluation",
        bounds_failure);
  }
  // Screen-aligned: three corners of an axis-aligned rectangle use exactly
  // two distinct X and two distinct Y values.
  auto two_distinct = [](const float* v) {
    return (v[0] == v[1]) != (v[0] == v[2]) || (v[0] != v[1] && v[1] == v[2]);
  };
  // Two equal corners make a zero-area rectangle that writes nothing, though
  // its bounds span the third corner.
  if (overwrite_vertex_count == 3) {
    if (!two_distinct(xs) || !two_distinct(ys) ||
        (xs[0] == xs[1] && ys[0] == ys[1]) ||
        (xs[0] == xs[2] && ys[0] == ys[2]) ||
        (xs[1] == xs[2] && ys[1] == ys[2])) {
      return reject("not screen-aligned");
    }
  } else {
    // Every vertex must be a corner of the bounding rectangle, each triangle
    // must use three different corners, and the corners the triangles don't
    // share must be opposite - then the triangles meet along a diagonal and
    // cover the rectangle exactly once (otherwise they overlap and leave a
    // half of it unwritten).
    if (!(min_x < max_x) || !(min_y < max_y)) {
      return reject("quad without area");
    }
    uint32_t corners[6];
    for (uint32_t i = 0; i < overwrite_vertex_count; ++i) {
      if ((xs[i] != min_x && xs[i] != max_x) ||
          (ys[i] != min_y && ys[i] != max_y)) {
        return reject("quad not screen-aligned");
      }
      corners[i] = uint32_t(xs[i] == max_x) | (uint32_t(ys[i] == max_y) << 1);
    }
    uint32_t lone_a, lone_b;
    if (overwrite_vertex_count == 6) {
      uint32_t triangle_corner_bits[2];
      for (uint32_t i = 0; i < 2; ++i) {
        triangle_corner_bits[i] = (uint32_t(1) << corners[3 * i]) |
                                  (uint32_t(1) << corners[3 * i + 1]) |
                                  (uint32_t(1) << corners[3 * i + 2]);
        if (xe::bit_count(triangle_corner_bits[i]) != 3) {
          return reject("quad with a degenerate triangle");
        }
      }
      if (triangle_corner_bits[0] == triangle_corner_bits[1]) {
        return reject("quad of two identical triangles");
      }
      // The corner missing from a triangle is the lone corner of the other.
      uint32_t lone_bits_a = 0b1111 & ~triangle_corner_bits[1];
      uint32_t lone_bits_b = 0b1111 & ~triangle_corner_bits[0];
      lone_a = lone_bits_a == 1 ? 0 : lone_bits_a == 2 ? 1 : lone_bits_a == 4 ? 2 : 3;
      lone_b = lone_bits_b == 1 ? 0 : lone_bits_b == 2 ? 1 : lone_bits_b == 4 ? 2 : 3;
    } else {
      uint32_t all_corner_bits = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        all_corner_bits |= uint32_t(1) << corners[i];
      }
      if (all_corner_bits != 0b1111) {
        return reject("quad not using all four corners");
      }
      lone_a = corners[quad_lone_corner_a];
      lone_b = corners[quad_lone_corner_b];
    }
    // Opposite corners differ in both coordinates.
    if ((lone_a ^ lone_b) != 0b11) {
      return reject("quad triangles don't meet along a diagonal");
    }
    if (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back) {
      // Neither triangle may be culled. In the screen space the positions are
      // in (Y down, like the host framebuffer), a positive doubled area is a
      // clockwise triangle, and the front face is clockwise when face is set.
      static const uint8_t kTriangleVertices[3][2][3] = {
          // Strip (the second triangle with the winding of the first).
          {{0, 1, 2}, {2, 1, 3}},
          // Fan, quad.
          {{0, 1, 2}, {0, 2, 3}},
          // List.
          {{0, 1, 2}, {3, 4, 5}},
      };
      uint32_t topology =
          overwrite_vertex_count == 6
              ? 2
              : (vgt_draw_initiator.prim_type ==
                         xenos::PrimitiveType::kTriangleStrip
                     ? 0
                     : 1);
      for (uint32_t i = 0; i < 2; ++i) {
        const uint8_t* v = kTriangleVertices[topology][i];
        float doubled_area = (xs[v[1]] - xs[v[0]]) * (ys[v[2]] - ys[v[0]]) -
                             (xs[v[2]] - xs[v[0]]) * (ys[v[1]] - ys[v[0]]);
        if (doubled_area == 0.0f) {
          return reject("quad with a degenerate triangle");
        }
        bool front = (doubled_area > 0.0f) == (pa_su_sc_mode_cntl.face != 0);
        if (front ? pa_su_sc_mode_cntl.cull_front
                  : pa_su_sc_mode_cntl.cull_back) {
          return reject("quad triangle culled", pa_su_sc_mode_cntl.value);
        }
      }
    }
  }
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (pa_cl_clip_cntl.ucp_ena) {
    return reject("user clip planes");
  }
  if (!pa_cl_clip_cntl.clip_disable) {
    // Clipped to the viewport and the depth range.
    float z_min_allowed = pa_cl_clip_cntl.dx_clip_space_def ? 0.0f : -1.0f;
    if (!(min_z >= z_min_allowed && max_z <= 1.0f)) {
      return reject("depth outside the clip range",
                    xe::memory::Reinterpret<uint32_t>(max_z));
    }
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float vp_scale_x = pa_cl_vte_cntl.vport_x_scale_ena
                           ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)
                           : 1.0f;
    float vp_offset_x = pa_cl_vte_cntl.vport_x_offset_ena
                            ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET)
                            : 0.0f;
    float vp_scale_y = pa_cl_vte_cntl.vport_y_scale_ena
                           ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
                           : 1.0f;
    float vp_offset_y = pa_cl_vte_cntl.vport_y_offset_ena
                            ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
                            : 0.0f;
    float screen_add_x = 0.0f, screen_add_y = 0.0f;
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
        xenos::PixelCenter::kD3DZero) {
      screen_add_x += 0.5f;
      screen_add_y += 0.5f;
    }
    if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
      auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
      screen_add_x += float(pa_sc_window_offset.window_x_offset);
      screen_add_y += float(pa_sc_window_offset.window_y_offset);
    }
    min_x = std::max(min_x,
                     vp_offset_x - std::abs(vp_scale_x) + screen_add_x);
    max_x = std::min(max_x,
                     vp_offset_x + std::abs(vp_scale_x) + screen_add_x);
    min_y = std::max(min_y,
                     vp_offset_y - std::abs(vp_scale_y) + screen_add_y);
    max_y = std::min(max_y,
                     vp_offset_y + std::abs(vp_scale_y) + screen_add_y);
  }
  // Before scissoring, for the whole host image check.
  float unscissored_min_x = min_x, unscissored_min_y = min_y;
  float unscissored_max_x = max_x, unscissored_max_y = max_y;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true);
  min_x = std::max(min_x, float(scissor.offset[0]));
  min_y = std::max(min_y, float(scissor.offset[1]));
  max_x = std::min(max_x, float(scissor.offset[0] + scissor.extent[0]));
  max_y = std::min(max_y, float(scissor.offset[1] + scissor.extent[1]));
  if (draw_samples_as_pixels_) {
    // The rewritten 1x target stores the four guest 4x samples at a 2x2
    // grid of host pixels; bounds and transfer cutouts use host coordinates.
    min_x *= 2.0f;
    min_y *= 2.0f;
    max_x *= 2.0f;
    max_y *= 2.0f;
    unscissored_min_x *= 2.0f;
    unscissored_min_y *= 2.0f;
    unscissored_max_x *= 2.0f;
    unscissored_max_y *= 2.0f;
  }
  // The depth / stencil value written, if known exactly - a constant depth,
  // converted to 24-bit unorm away from a rounding boundary - and the pixels
  // written, for rt_cache_retarget_full_depth_clears, whether or not the
  // rectangle covers the whole range claimed for the render target.
  if (((result & 1) || depth_only || stencil_only) &&
      (stencil_only ||
       !(update_pixel_shader_ && update_pixel_shader_->writes_depth()))) {
    auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    reg::RB_DEPTHCONTROL dc = normalized_depth_control;
    auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
    reg::RB_STENCILREFMASK stencil_ref_mask_bf;
    stencil_ref_mask_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    float z = min_z;
    if (pa_cl_vte_cntl.vport_z_scale_ena) {
      z *= regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
    }
    if (pa_cl_vte_cntl.vport_z_offset_ena) {
      z += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
    }
    // The guest depth value, only if the host value written by the draw is
    // exactly the one a clear of the guest value produces.
    uint32_t z_guest = 0;
    bool z_exact = false;
    if (min_z == max_z && z >= 0.0f && z <= 1.0f) {
      switch (rb_depth_info.depth_format) {
        case xenos::DepthRenderTargetFormat::kD24S8: {
          double z_unorm24 = double(z) * double(0xFFFFFF);
          double z_unorm24_rounded = std::nearbyint(z_unorm24);
          z_exact = std::abs(z_unorm24 - z_unorm24_rounded) < 0.25;
          z_guest = uint32_t(z_unorm24_rounded);
        } break;
        case xenos::DepthRenderTargetFormat::kD24FS8:
          z_guest = xenos::Float32To20e4(z, true);
          z_exact = xenos::Float20e4To32(z_guest) == z;
          break;
      }
    }
    if (stencil_only) {
      // Only the stencil reference is written.
      z_guest = 0;
    }
    if ((z_exact || stencil_only) &&
        (stencil_only || (!pa_su_sc_mode_cntl.poly_offset_front_enable &&
                          !pa_su_sc_mode_cntl.poly_offset_back_enable &&
                          !pa_su_sc_mode_cntl.poly_offset_para_enable)) &&
        (depth_only || !dc.backface_enable ||
         stencil_ref_mask_bf.stencilref == stencil_ref_mask.stencilref)) {
      // The pixels covered - only with the edges on pixel boundaries, so
      // exactly the pixels between them are covered (their centers inside).
      float clear_x0 = std::max(min_x, 0.0f), clear_y0 = std::max(min_y, 0.0f);
      if (clear_x0 == std::floor(clear_x0) && clear_y0 == std::floor(clear_y0) &&
          max_x == std::floor(max_x) && max_y == std::floor(max_y) &&
          clear_x0 < max_x && clear_y0 < max_y) {
        last_full_clear_depth_value_valid_ = true;
        last_full_clear_depth_only_ = depth_only;
        last_full_clear_stencil_only_ = stencil_only;
        last_full_clear_depth_value_ =
            (z_guest << 8) | stencil_ref_mask.stencilref;
        last_clear_rect_[0] = uint32_t(clear_x0);
        last_clear_rect_[1] = uint32_t(clear_y0);
        last_clear_rect_[2] = uint32_t(max_x);
        last_clear_rect_[3] = uint32_t(max_y);
      }
    }
    if (!last_full_clear_depth_value_valid_ &&
        clear_retarget_logged_ <
            uint32_t(std::max(0, cvars::log_full_clear_transfer_skips))) {
      ++clear_retarget_logged_;
      XELOGI(
          "RTCLEAR: constant depth clear value unknown: z=[{},{}] -> {}, "
          "format {}, poly offset {}{}{}, stencil ref 0x{:X}/0x{:X} (bf {}), "
          "rect [{},{},{},{}]",
          min_z, max_z, z, uint32_t(rb_depth_info.depth_format),
          uint32_t(pa_su_sc_mode_cntl.poly_offset_front_enable),
          uint32_t(pa_su_sc_mode_cntl.poly_offset_back_enable),
          uint32_t(pa_su_sc_mode_cntl.poly_offset_para_enable),
          uint32_t(stencil_ref_mask.stencilref),
          uint32_t(stencil_ref_mask_bf.stencilref),
          uint32_t(dc.backface_enable), min_x, min_y, max_x, max_y);
    }
  }
  // Whole pixels (all samples of them) inside the rectangle, for transferring
  // only the pixels around it when it doesn't cover the claimed range.
  if (cvars::rt_cache_draw_overwrite_cutouts) {
    float cut_x0 = std::ceil(std::max(min_x, 0.0f));
    float cut_y0 = std::ceil(std::max(min_y, 0.0f));
    float cut_x1 = std::floor(std::min(max_x, float(xenos::kTexture2DCubeMaxWidthHeight)));
    float cut_y1 = std::floor(std::min(max_y, float(xenos::kTexture2DCubeMaxWidthHeight)));
    if (cut_x0 < cut_x1 && cut_y0 < cut_y1) {
      last_draw_overwrite_mask_ = result;
      // Bit 0 - depth, bit 1 - stencil.
      last_draw_overwrite_depth_aspects_ =
          (result & 1) ? 0b11
                       : (depth_only_written ? 0b01
                                             : (stencil_only_written ? 0b10
                                                                     : 0));
      last_draw_overwrite_rect_.x_pixels = uint32_t(cut_x0);
      last_draw_overwrite_rect_.y_pixels = uint32_t(cut_y0);
      last_draw_overwrite_rect_.width_pixels = uint32_t(cut_x1 - cut_x0);
      last_draw_overwrite_rect_.height_pixels = uint32_t(cut_y1 - cut_y0);
      last_draw_overwrite_exact_ =
          cut_x0 == std::max(min_x, 0.0f) && cut_y0 == std::max(min_y, 0.0f) &&
          cut_x1 == max_x && cut_y1 == max_y;
    }
  }
  // The whole addressable surface area of the claimed range - which ends at a
  // tile row boundary, so the last partially used row must be covered too.
  // Columns past the surface pitch are only tile padding.
  uint32_t tile_height_pixels =
      xenos::kEdramTileHeightSamples >>
      uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples >=
               xenos::MsaaSamples::k2X);
  if (!(min_x <= 0.0f && min_y <= 0.0f && max_x >= float(pitch_pixels) &&
        max_y >= float(xe::round_up(height_used, tile_height_pixels)))) {
    if (rt_trace_claims_ ||
        full_overwrite_logged_ <
            uint32_t(std::max(0, cvars::log_full_clear_transfer_skips))) {
      ++full_overwrite_logged_;
      XELOGI(
          "RTCLEAR: rectangle not a full clear: coverage [{},{},{},{}] vs "
          "{}x{} (clip {}, vte 0x{:X})",
          min_x, min_y, max_x, max_y, pitch_pixels, height_used,
          uint32_t(!pa_cl_clip_cntl.clip_disable),
          regs.Get<reg::PA_CL_VTE_CNTL>().value);
    }
    return 0;
  }
  last_draw_covers_claimed_range_ = true;
  if (cvars::rt_cache_dont_care_load_for_full_clears) {
    draw_util::Scissor scissor_unclamped;
    draw_util::GetScissor(regs, scissor_unclamped, false);
    xenos::MsaaSamples msaa_samples =
        regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
    uint32_t msaa_samples_x_log2 =
        uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
    uint32_t pitch_tiles_at_32bpp =
        ((pitch_pixels << msaa_samples_x_log2) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples;
    float host_width = float(pitch_tiles_at_32bpp *
                             (xenos::kEdramTileWidthSamples >>
                              msaa_samples_x_log2));
    float host_height =
        float(GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples));
    float scissor_min_x = float(scissor_unclamped.offset[0]);
    float scissor_min_y = float(scissor_unclamped.offset[1]);
    float scissor_max_x =
        float(scissor_unclamped.offset[0] + scissor_unclamped.extent[0]);
    float scissor_max_y =
        float(scissor_unclamped.offset[1] + scissor_unclamped.extent[1]);
    if (draw_samples_as_pixels_) {
      scissor_min_x *= 2.0f;
      scissor_min_y *= 2.0f;
      scissor_max_x *= 2.0f;
      scissor_max_y *= 2.0f;
    }
    float covered_min_x = std::max(unscissored_min_x, scissor_min_x);
    float covered_min_y = std::max(unscissored_min_y, scissor_min_y);
    float covered_max_x = std::min(unscissored_max_x, scissor_max_x);
    float covered_max_y = std::min(unscissored_max_y, scissor_max_y);
    if (covered_min_x <= 0.0f && covered_min_y <= 0.0f &&
        covered_max_x >= host_width && covered_max_y >= host_height) {
      last_update_full_host_overwrite_ = result;
    }
  }
  if (full_overwrite_logged_ <
      uint32_t(std::max(0, cvars::log_full_clear_transfer_skips))) {
    ++full_overwrite_logged_;
    XELOGI(
        "RTCLEAR: skipping transfers into rts=0x{:X} (used 0x{:X}, whole "
        "host 0x{:X}) pitch={} height={} msaa={} rect=[{},{},{},{}] "
        "z=[{},{}]",
        result, rts_used_bits, last_update_full_host_overwrite_, pitch_pixels,
        height_used,
        uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples), min_x, min_y,
        max_x, max_y, min_z, max_z);
  }
  return result;
}

uint32_t RenderTargetCache::GetLastUpdateBoundRenderTargets(
    uint32_t* depth_and_color_formats_out) const {
  if (GetPath() != Path::kHostRenderTargets) {
    if (depth_and_color_formats_out) {
      std::memset(depth_and_color_formats_out, 0,
                  sizeof(uint32_t) * (1 + xenos::kMaxColorRenderTargets));
    }
    return 0;
  }
  uint32_t rts_used = 0;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    const RenderTarget* render_target =
        last_update_accumulated_render_targets_[i];
    if (!render_target) {
      if (depth_and_color_formats_out) {
        depth_and_color_formats_out[i] = 0;
      }
      continue;
    }
    rts_used |= uint32_t(1) << i;
    if (depth_and_color_formats_out) {
      depth_and_color_formats_out[i] = render_target->key().resource_format;
    }
  }
  return rts_used;
}

uint32_t RenderTargetCache::GetRenderTargetHeight(RenderTargetKey key) const {
  uint32_t height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);
  if (key.height_class == kRenderTargetHeightClassBaseCap) {
    // The base cap is a multiple of the tile height at every sample count.
    static const uint32_t cap = cvars::render_target_height_cap & ~uint32_t(15);
    height = std::min(height, cap);
  } else {
    assert_true(key.height_class == kRenderTargetHeightClassFull);
  }
  return height;
}

uint32_t RenderTargetCache::GetRenderTargetHeight(
    uint32_t pitch_tiles_at_32bpp, xenos::MsaaSamples msaa_samples) const {
  if (!pitch_tiles_at_32bpp) {
    return 0;
  }
  // Down to the beginning of the render target in the next 11-bit EDRAM
  // addressing period.
  uint32_t tile_rows = (xenos::kEdramTileCount + (pitch_tiles_at_32bpp - 1)) /
                       pitch_tiles_at_32bpp;
  // Clamp to the guest limit (tile padding should exceed it) and to the host
  // limit (tile padding mustn't exceed it).
  static_assert(
      !(xenos::kTexture2DCubeMaxWidthHeight % xenos::kEdramTileHeightSamples),
      "Maximum guest render target height is assumed to always be a multiple "
      "of an EDRAM tile height");
  uint32_t max_height_scaled =
      std::min(xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
               GetMaxRenderTargetHeight());
  uint32_t msaa_samples_y_log2 =
      uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t tile_height_samples_scaled =
      xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  tile_rows = std::min(tile_rows, (max_height_scaled << msaa_samples_y_log2) /
                                      tile_height_samples_scaled);
  assert_not_zero(tile_rows);
  return tile_rows * (xenos::kEdramTileHeightSamples >> msaa_samples_y_log2);
}

void RenderTargetCache::GetHostDepthStoreRectangleInfo(
    const Transfer::Rectangle& transfer_rectangle,
    xenos::MsaaSamples msaa_samples,
    HostDepthStoreRectangleConstant& rectangle_constant_out,
    uint32_t& group_count_x_out, uint32_t& group_count_y_out) const {
  // Initialize to all bits zeroed.
  HostDepthStoreRectangleConstant rectangle_constant;
  // 8 pixels is the resolve granularity, both clearing and tile size are
  // aligned to 8.
  assert_zero(transfer_rectangle.x_pixels & 7);
  assert_zero(transfer_rectangle.y_pixels & 7);
  assert_zero(transfer_rectangle.width_pixels & 7);
  assert_zero(transfer_rectangle.height_pixels & 7);
  assert_not_zero(transfer_rectangle.width_pixels);
  rectangle_constant.x_pixels_div_8 = transfer_rectangle.x_pixels >> 3;
  rectangle_constant.y_pixels_div_8 = transfer_rectangle.y_pixels >> 3;
  rectangle_constant.width_pixels_div_8_minus_1 =
      (transfer_rectangle.width_pixels >> 3) - 1;
  rectangle_constant_out = rectangle_constant;
  // 1 thread group = 64x8 host samples.
  uint32_t pixel_size_x = draw_resolution_scale_x()
                          << uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t pixel_size_y = draw_resolution_scale_y()
                          << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  group_count_x_out =
      (transfer_rectangle.width_pixels * pixel_size_x + 63) >> 6;
  group_count_y_out = (transfer_rectangle.height_pixels * pixel_size_y) >> 3;
}

void RenderTargetCache::GetResolveCopyRectanglesToDump(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out) const {
  rectangles_out.clear();
  assert_true(row_length <= pitch);
  row_length = std::min(row_length, pitch);
  if (!row_length || !rows) {
    return;
  }
  auto get_rectangles_in_extent = [&](uint32_t extent_start,
                                      uint32_t extent_end,
                                      uint32_t range_local_offset) {
    // Collect render targets owning ranges within the specified rectangle. The
    // first render target in the range may be before the lower_bound, only
    // being in the range with its tail.
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.cbegin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.cend(); ++it) {
      uint32_t range_global_start = std::max(it->first, extent_start);
      if (range_global_start >= extent_end) {
        break;
      }
      RenderTargetKey rt_key = it->second.render_target;
      if (rt_key.IsEmpty()) {
        continue;
      }
      // Merge with other render target ranges with the same current ownership,
      // but different depth ownership, since it's not relevant to resolving.
      while (it != ownership_ranges_.cend()) {
        auto it_next = std::next(it);
        if (it_next == ownership_ranges_.cend() ||
            it_next->first >= extent_end ||
            it_next->second.render_target != rt_key) {
          break;
        }
        it = it_next;
      }

      uint32_t range_local_start = range_local_offset +
                                   std::max(range_global_start, extent_start) -
                                   base;
      uint32_t range_local_end = range_local_offset +
                                 std::min(it->second.end_tiles, extent_end) -
                                 base;
      assert_true(range_local_start < range_local_end);

      uint32_t rows_start = range_local_start / pitch;
      uint32_t rows_end = (range_local_end + (pitch - 1)) / pitch;
      uint32_t row_first_start = range_local_start - rows_start * pitch;
      if (row_first_start >= row_length) {
        // The first row starts within the pitch padding.
        if (rows_start + 1 < rows_end) {
          // Multiple rows - start at the second.
          ++rows_start;
          row_first_start = 0;
        } else {
          // Single row - nothing to dump.
          continue;
        }
      }

      auto it_rt = render_targets_.find(rt_key);
      assert_true(it_rt != render_targets_.cend());
      assert_not_null(it_rt->second);
      // Don't include pitch padding in the last row.
      rectangles_out.emplace_back(
          it_rt->second, rows_start, rows_end - rows_start, row_first_start,
          std::min(pitch - (rows_end * pitch - range_local_end), row_length));
    }
  };
  uint32_t resolve_area_end = base + (rows - 1) * pitch + row_length;
  get_rectangles_in_extent(
      base, std::min(resolve_area_end, xenos::kEdramTileCount), 0);
  if (resolve_area_end > xenos::kEdramTileCount) {
    // The resolve area goes to the next EDRAM addressing period.
    get_rectangles_in_extent(
        0, std::min(resolve_area_end & (xenos::kEdramTileCount - 1), base),
        xenos::kEdramTileCount);
  }
}

bool RenderTargetCache::IsResolveSourceNativeOnly(uint32_t base,
                                                  uint32_t row_length,
                                                  uint32_t rows,
                                                  uint32_t pitch) const {
  if (!IsDrawResolutionScaled() || GetPath() != Path::kHostRenderTargets) {
    return false;
  }
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, rectangles);
  if (rectangles.empty()) {
    return false;
  }
  for (const ResolveCopyDumpRectangle& rectangle : rectangles) {
    assert_not_null(rectangle.render_target);
    if (!rectangle.render_target->key().scale_native) {
      return false;
    }
  }
  return true;
}

bool RenderTargetCache::PrepareHostRenderTargetsResolveClear(
    const draw_util::ResolveInfo& resolve_info,
    Transfer::Rectangle& clear_rectangle_out,
    RenderTarget*& depth_render_target_out,
    std::vector<Transfer>& depth_transfers_out,
    RenderTarget*& color_render_target_out,
    std::vector<Transfer>& color_transfers_out) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  uint32_t pitch_tiles_at_32bpp;
  uint32_t base_offset_tiles_at_32bpp;
  xenos::MsaaSamples msaa_samples;
  if (resolve_info.IsClearingDepth()) {
    pitch_tiles_at_32bpp = resolve_info.depth_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp = resolve_info.depth_edram_info.base_tiles -
                                 resolve_info.depth_original_base;
    msaa_samples = resolve_info.depth_edram_info.msaa_samples;
  } else if (resolve_info.IsClearingColor()) {
    pitch_tiles_at_32bpp = resolve_info.color_edram_info.pitch_tiles;
    base_offset_tiles_at_32bpp = resolve_info.color_edram_info.base_tiles -
                                 resolve_info.color_original_base;
    if (resolve_info.color_edram_info.format_is_64bpp) {
      assert_zero(pitch_tiles_at_32bpp & 1);
      pitch_tiles_at_32bpp >>= 1;
      assert_zero(base_offset_tiles_at_32bpp & 1);
      base_offset_tiles_at_32bpp >>= 1;
    }
    msaa_samples = resolve_info.color_edram_info.msaa_samples;
  } else {
    return false;
  }
  assert_true(msaa_samples <= xenos::MsaaSamples::k4X);
  if (msaa_samples > xenos::MsaaSamples::k4X) {
    // Safety check because a lot of code assumes up to 4x, including arrays
    // indexed by the sample count.
    XELOGE(
        "{}x MSAA requested by the guest in a resolve clear, Xenos only "
        "supports up to 4x",
        uint32_t(1) << uint32_t(msaa_samples));
    return false;
  }
  if (!pitch_tiles_at_32bpp) {
    return false;
  }
  uint32_t msaa_samples_x_log2 =
      uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t msaa_samples_y_log2 =
      uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  if (pitch_tiles_at_32bpp >
      ((xenos::kTexture2DCubeMaxWidthHeight << msaa_samples_x_log2) +
       (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples) {
    XELOGE(
        "Surface pitch in 80-sample groups {} at {}x MSAA larger than the "
        "maximum texture width {} specified by the guest in a resolve",
        pitch_tiles_at_32bpp, uint32_t(1) << uint32_t(msaa_samples),
        xenos::kTexture2DCubeMaxWidthHeight);
    return false;
  }
  uint32_t pitch_pixels =
      pitch_tiles_at_32bpp *
      (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2);
  bool scale_native = IsScaleNativeForPitch(pitch_tiles_at_32bpp, msaa_samples);
  uint32_t pitch_pixels_scaled =
      pitch_pixels * (scale_native ? 1 : draw_resolution_scale_x());
  uint32_t max_render_target_width = GetMaxRenderTargetWidth();
  if (pitch_pixels_scaled > max_render_target_width) {
    // TODO(Triang3l): If really needed for some game on some device, clamp the
    // pitch the same way as explained in the comment in Update.
    XELOGE(
        "Surface pitch aligned to EDRAM tiles and resolution-scaled {} larger "
        "than the maximum host render target width {} in a resolve",
        pitch_pixels_scaled, max_render_target_width);
    return false;
  }

  uint32_t render_target_height_pixels =
      GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples);
  uint32_t base_offset_rows_at_32bpp =
      base_offset_tiles_at_32bpp / pitch_tiles_at_32bpp;
  Transfer::Rectangle clear_rectangle;
  clear_rectangle.x_pixels = std::min(
      (base_offset_tiles_at_32bpp -
       base_offset_rows_at_32bpp * pitch_tiles_at_32bpp) *
              (xenos::kEdramTileWidthSamples >> msaa_samples_x_log2) +
          (uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) << 3),
      pitch_pixels);
  clear_rectangle.y_pixels = std::min(
      base_offset_rows_at_32bpp *
              (xenos::kEdramTileHeightSamples >> msaa_samples_y_log2) +
          (uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) << 3),
      render_target_height_pixels);
  clear_rectangle.width_pixels =
      std::min(uint32_t(resolve_info.coordinate_info.width_div_8) << 3,
               pitch_pixels - clear_rectangle.x_pixels);
  clear_rectangle.height_pixels =
      std::min(uint32_t(resolve_info.height_div_8) << 3,
               render_target_height_pixels - clear_rectangle.y_pixels);
  if (!clear_rectangle.width_pixels || !clear_rectangle.height_pixels) {
    // Outside the pitch / height (or initially specified as 0).
    return false;
  }

  // Change ownership of the tiles containing the area to be cleared, so the
  // up-to-date host render target for the cleared range will be the cleared
  // one.
  uint32_t clear_start_tiles_at_32bpp =
      ((clear_rectangle.y_pixels << msaa_samples_y_log2) /
       xenos::kEdramTileHeightSamples) *
          pitch_tiles_at_32bpp +
      (clear_rectangle.x_pixels << msaa_samples_x_log2) /
          xenos::kEdramTileWidthSamples;
  uint32_t clear_length_tiles_at_32bpp =
      (((clear_rectangle.y_pixels + clear_rectangle.height_pixels - 1)
        << msaa_samples_y_log2) /
       xenos::kEdramTileHeightSamples) *
          pitch_tiles_at_32bpp +
      ((clear_rectangle.x_pixels + clear_rectangle.width_pixels - 1)
       << msaa_samples_x_log2) /
          xenos::kEdramTileWidthSamples +
      1 - clear_start_tiles_at_32bpp;
  // Up to the range from the base in the current 11 tile index bits to the base
  // in the next 11 tile index bits after wrapping can be cleared.
  uint32_t depth_clear_start_tiles_base_relative = 0;
  uint32_t depth_clear_length_tiles = 0;
  if (resolve_info.IsClearingDepth()) {
    depth_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp, xenos::kEdramTileCount);
    depth_clear_length_tiles =
        std::min(clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp,
                 xenos::kEdramTileCount) -
        depth_clear_start_tiles_base_relative;
  }
  uint32_t color_clear_start_tiles_base_relative = 0;
  uint32_t color_clear_length_tiles = 0;
  if (resolve_info.IsClearingColor()) {
    color_clear_start_tiles_base_relative =
        std::min(clear_start_tiles_at_32bpp
                     << resolve_info.color_edram_info.format_is_64bpp,
                 xenos::kEdramTileCount);
    color_clear_length_tiles =
        std::min((clear_start_tiles_at_32bpp + clear_length_tiles_at_32bpp)
                     << resolve_info.color_edram_info.format_is_64bpp,
                 xenos::kEdramTileCount) -
        color_clear_start_tiles_base_relative;
  }
  if (depth_clear_length_tiles && color_clear_length_tiles) {
    // Prevent overlap - clear the depth only until the color, the color only
    // until the depth, in the current or the next 11 bits of the tile index.
    uint32_t depth_clear_start_tiles_wrapped =
        (resolve_info.depth_original_base +
         depth_clear_start_tiles_base_relative) &
        (xenos::kEdramTileCount - 1);
    uint32_t color_clear_start_tiles_wrapped =
        (resolve_info.color_original_base +
         color_clear_start_tiles_base_relative) &
        (xenos::kEdramTileCount - 1);
    depth_clear_length_tiles = std::min(
        depth_clear_length_tiles,
        ((color_clear_start_tiles_wrapped < depth_clear_start_tiles_wrapped)
             ? xenos::kEdramTileCount
             : 0) +
            color_clear_start_tiles_wrapped - depth_clear_start_tiles_wrapped);
    color_clear_length_tiles = std::min(
        color_clear_length_tiles,
        ((depth_clear_start_tiles_wrapped < color_clear_start_tiles_wrapped)
             ? xenos::kEdramTileCount
             : 0) +
            depth_clear_start_tiles_wrapped - color_clear_start_tiles_wrapped);
  }

  // A 4x surface drawn as 1x at double size (vulkan_msaa_4x_as_2x2) covers the
  // same EDRAM tiles with one pixel per sample: clear that key, with the
  // rectangle in samples, instead of moving the range to a 4x image for the
  // clear and back for the next draw (Reach: ~187 MB of transfers per frame).
  // The pitch in tiles stays; the tile range above is the same.
  if (msaa_4x_clears_as_2x2_ && msaa_samples == xenos::MsaaSamples::k4X &&
      !IsDrawResolutionScaled() &&
      pitch_pixels * 2 <= std::min(max_render_target_width,
                                   xenos::kTexture2DCubeMaxWidthHeight)) {
    msaa_samples = xenos::MsaaSamples::k1X;
    clear_rectangle.x_pixels <<= 1;
    clear_rectangle.y_pixels <<= 1;
    clear_rectangle.width_pixels <<= 1;
    clear_rectangle.height_pixels <<= 1;
    scale_native = IsScaleNativeForPitch(pitch_tiles_at_32bpp, msaa_samples);
  }

  RenderTargetKey depth_render_target_key;
  RenderTarget* depth_render_target = nullptr;
  if (depth_clear_length_tiles) {
    depth_render_target_key.base_tiles = resolve_info.depth_original_base;
    depth_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    depth_render_target_key.msaa_samples = msaa_samples;
    depth_render_target_key.is_depth = 1;
    depth_render_target_key.resource_format =
        resolve_info.depth_edram_info.format;
    depth_render_target_key.scale_native = uint32_t(scale_native);
    depth_render_target = GetOrCreateRenderTarget(depth_render_target_key);
    if (!depth_render_target) {
      // Failed to create the depth render target, don't clear it.
      depth_render_target_key = RenderTargetKey();
      depth_clear_length_tiles = 0;
    }
  }
  RenderTargetKey color_render_target_key;
  RenderTarget* color_render_target = nullptr;
  if (color_clear_length_tiles) {
    color_render_target_key.base_tiles = resolve_info.color_original_base;
    color_render_target_key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    color_render_target_key.msaa_samples = msaa_samples;
    color_render_target_key.is_depth = 0;
    color_render_target_key.resource_format = uint32_t(GetColorResourceFormat(
        xenos::ColorRenderTargetFormat(resolve_info.color_edram_info.format)));
    color_render_target_key.scale_native = uint32_t(scale_native);
    color_render_target = GetOrCreateRenderTarget(color_render_target_key);
    if (!color_render_target) {
      // Failed to create the color render target, don't clear it.
      color_render_target_key = RenderTargetKey();
      color_clear_length_tiles = 0;
    }
  }
  if (!depth_clear_length_tiles && !color_clear_length_tiles) {
    // Complete overlap, or failed to create all the render targets.
    return false;
  }

  clear_rectangle_out = clear_rectangle;
  depth_render_target_out = depth_render_target;
  depth_transfers_out.clear();
  if (depth_render_target) {
    // The clear writes the raw EDRAM words; its rectangle may be only a part
    // of the range, so accumulate.
    AddEdramStencilMaybeSetBits(
        depth_render_target_key.base_tiles +
            depth_clear_start_tiles_base_relative,
        depth_clear_length_tiles, resolve_info.rb_depth_clear);
    ChangeOwnership(
        depth_render_target_key, depth_clear_start_tiles_base_relative,
        depth_clear_length_tiles, &depth_transfers_out, &clear_rectangle);
  }
  color_render_target_out = color_render_target;
  color_transfers_out.clear();
  if (color_render_target) {
    AddEdramStencilMaybeSetBits(
        color_render_target_key.base_tiles +
            color_clear_start_tiles_base_relative,
        color_clear_length_tiles,
        resolve_info.rb_color_clear | resolve_info.rb_color_clear_lo);
    ChangeOwnership(
        color_render_target_key, color_clear_start_tiles_base_relative,
        color_clear_length_tiles, &color_transfers_out, &clear_rectangle);
  }
  // rt_cache_transfer_known_constants: the whole tile rows inside the clear
  // rectangle hold the clear word now.
  auto set_clear_constant = [&](RenderTarget* render_target,
                                uint32_t clear_start_tiles,
                                uint32_t clear_length_tiles, uint32_t word) {
    if (!render_target) {
      return;
    }
    render_target->ForgetConstant();
    const RenderTargetKey key = render_target->key();
    if (!cvars::rt_cache_transfer_known_constants || key.Is64bpp()) {
      return;
    }
    const uint32_t samples_x_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k4X);
    const uint32_t samples_y_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t pitch_tiles = key.GetPitchTiles();
    if (clear_rectangle.x_pixels ||
        ((clear_rectangle.x_pixels + clear_rectangle.width_pixels)
         << samples_x_log2) < pitch_tiles * xenos::kEdramTileWidthSamples) {
      return;
    }
    const uint32_t y0 = clear_rectangle.y_pixels << samples_y_log2;
    const uint32_t y1 = (clear_rectangle.y_pixels + clear_rectangle.height_pixels)
                        << samples_y_log2;
    const uint32_t row0 = xe::align(y0, xenos::kEdramTileHeightSamples) /
                          xenos::kEdramTileHeightSamples;
    const uint32_t row1 = y1 / xenos::kEdramTileHeightSamples;
    const uint32_t start =
        std::max(row0 * pitch_tiles, clear_start_tiles);
    const uint32_t end = std::min(row1 * pitch_tiles,
                                  clear_start_tiles + clear_length_tiles);
    if (start < end) {
      render_target->SetConstant(start, end, word, UINT32_MAX);
    }
  };
  set_clear_constant(depth_render_target, depth_clear_start_tiles_base_relative,
                     depth_clear_length_tiles, resolve_info.rb_depth_clear);
  set_clear_constant(color_render_target, color_clear_start_tiles_base_relative,
                     color_clear_length_tiles, resolve_info.rb_color_clear);
  return true;
}

RenderTargetCache::RenderTarget*
RenderTargetCache::PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
    xenos::ColorRenderTargetFormat color_format) {
  assert_true(GetPath() == Path::kHostRenderTargets);
  constexpr uint32_t kPitchTilesAt32bpp = 16;
  constexpr uint32_t kWidth =
      kPitchTilesAt32bpp * xenos::kEdramTileWidthSamples;
  if (kWidth * draw_resolution_scale_x() > GetMaxRenderTargetWidth()) {
    return nullptr;
  }
  // Same render target height is used for 32bpp and 64bpp to allow mixing them.
  constexpr uint32_t kHeightTileRows =
      xenos::kEdramTileCount / kPitchTilesAt32bpp;
  static_assert(
      kPitchTilesAt32bpp * kHeightTileRows == xenos::kEdramTileCount,
      "Using width of the render target for EDRAM snapshot restoration that is "
      "expected to result in the last row being fully utilized.");
  constexpr uint32_t kHeight = kHeightTileRows * xenos::kEdramTileHeightSamples;
  static_assert(
      kHeight <= xenos::kTexture2DCubeMaxWidthHeight,
      "Using width of the render target for EDRAM snapshot restoration that is "
      "expect to fully cover the EDRAM without exceeding the maximum guest "
      "render target height.");
  if (kHeight * draw_resolution_scale_y() > GetMaxRenderTargetHeight()) {
    return nullptr;
  }
  RenderTargetKey render_target_key;
  render_target_key.pitch_tiles_at_32bpp = kPitchTilesAt32bpp;
  render_target_key.resource_format =
      uint32_t(GetColorResourceFormat(color_format));
  RenderTarget* render_target = GetOrCreateRenderTarget(render_target_key);
  if (!render_target) {
    return nullptr;
  }
  // Change ownership, but don't transfer the contents - they will be replaced
  // anyway.
  SetEdramStencilMaybeSetBits(0, xenos::kEdramTileCount, 0xFF);
  ForgetAllRenderTargetConstants();
  ++ownership_ranges_version_;
  ownership_ranges_.clear();
  ownership_ranges_.emplace(
      std::piecewise_construct, std::forward_as_tuple(uint32_t(0)),
      std::forward_as_tuple(xenos::kEdramTileCount, render_target_key,
                            RenderTargetKey(), RenderTargetKey()));
  return render_target;
}

void RenderTargetCache::PixelShaderInterlockFullEdramBarrierPlaced() {
  assert_true(GetPath() == Path::kPixelShaderInterlock);
  // Clear ownership - any overlap of data written before the barrier is safe.
  ++ownership_ranges_version_;
  OwnershipRange empty_range(xenos::kEdramTileCount, RenderTargetKey(),
                             RenderTargetKey(), RenderTargetKey());
  if (ownership_ranges_.size() == 1) {
    // Do not reallocate map elements if not needed (either nothing drawn since
    // the last barrier, or all of the EDRAM is owned by one render target).
    // The ownership map contains no gaps - the first element should always be
    // at 0.
    assert_true(!ownership_ranges_.begin()->first);
    OwnershipRange& all_edram_range = ownership_ranges_.begin()->second;
    assert_true(all_edram_range.end_tiles == xenos::kEdramTileCount);
    all_edram_range = empty_range;
    return;
  }
  ownership_ranges_.clear();
  ownership_ranges_.emplace(0, empty_range);
}

RenderTargetCache::RenderTarget* RenderTargetCache::GetOrCreateRenderTarget(
    RenderTargetKey key) {
  assert_true(GetPath() == Path::kHostRenderTargets);
  auto& lookup = render_target_lookup_[(key.key * UINT32_C(0x9E3779B1)) >> 26];
  if (lookup.valid && lookup.key == key) return lookup.target;
  auto it_rt = render_targets_.find(key);
  RenderTarget* render_target;
  if (it_rt != render_targets_.end()) {
    render_target = it_rt->second;
  } else {
    render_target = CreateRenderTarget(key);
    uint32_t width = key.GetWidth();
    uint32_t height = GetRenderTargetHeight(key);
    if (render_target) {
      XELOGGPU(
          "Created a {}x{} {}xMSAA {} render target with guest format {} at "
          "EDRAM base {}",
          width, height, uint32_t(1) << uint32_t(key.msaa_samples),
          key.is_depth ? "depth" : "color", key.resource_format,
          key.base_tiles);
    } else {
      XELOGE(
          "Failed to create a {}x{} {}xMSAA {} render target with guest format "
          "{} at EDRAM base {}",
          width, height, uint32_t(1) << uint32_t(key.msaa_samples),
          key.is_depth ? "depth" : "color", key.resource_format,
          key.base_tiles);
    }
    // Insert even if failed to create, not to try to create again.
    render_targets_.emplace(key, render_target);
  }
  lookup = {key, render_target, true};
  return render_target;
}

bool RenderTargetCache::IsTransferValueConverted7e3And8888(
    RenderTargetKey source, RenderTargetKey dest) const {
  // Matched in-place reuse only.
  if (!cvars::value_convert_7e3_8888_reuse || source.is_depth ||
      dest.is_depth || source.base_tiles != dest.base_tiles ||
      source.pitch_tiles_at_32bpp != dest.pitch_tiles_at_32bpp ||
      source.msaa_samples != dest.msaa_samples) {
    return false;
  }
  auto is_7e3 = [](xenos::ColorRenderTargetFormat format) {
    return format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
           format == xenos::ColorRenderTargetFormat::
                         k_2_10_10_10_FLOAT_AS_16_16_16_16;
  };
  // 7e3 -> plain 8_8_8_8 only. The reverse and gamma dests stay bit-exact.
  if (!is_7e3(source.GetColorFormat()) ||
      dest.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
    return false;
  }
  // Decode only if the draw blends over the dest, so the bytes are actually
  // read.
  const RegisterFile& regs = register_file();
  bool dest_blends = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i])
            .color_base != dest.base_tiles) {
      continue;
    }
    auto blend = regs.Get<reg::RB_BLENDCONTROL>(
        reg::RB_BLENDCONTROL::rt_register_indices[i]);
    dest_blends = blend.color_destblend != xenos::BlendFactor::kZero ||
                  blend.alpha_destblend != xenos::BlendFactor::kZero;
    break;
  }
  return dest_blends;
}

bool RenderTargetCache::WouldOwnershipChangeRequireTransfers(
    RenderTargetKey dest, uint32_t start_tiles_base_relative,
    uint32_t length_tiles) const {
  // xenos::kEdramTileCount with length 0 is fine if both the start and the end
  // are clamped to xenos::kEdramTileCount.
  assert_true(start_tiles_base_relative <=
              (xenos::kEdramTileCount - uint32_t(length_tiles != 0)));
  assert_true(length_tiles <= xenos::kEdramTileCount);
  if (length_tiles == 0) {
    return false;
  }
  bool host_depth_encoding_different =
      dest.is_depth && GetPath() == Path::kHostRenderTargets &&
      IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  auto would_require_transfers_in_extent = [&](uint32_t extent_start,
                                               uint32_t extent_end) -> bool {
    // The map contains consecutive ranges, merged if the adjacent ones are the
    // same. Find the range starting at >= the start. A portion of the range
    // preceding it may be intersecting the render target's range (or even fully
    // contain it).
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start) {
        it = it_pre;
      }
    }
    for (; it != ownership_ranges_.end(); ++it) {
      if (it->first >= extent_end) {
        // Outside the touched extent already.
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Already owned by the needed render target - no need to transfer
        // anything.
        continue;
      }
      RenderTargetKey transfer_source = it->second.render_target;
      // Only perform the transfer when actually changing the latest owner, not
      // just the latest host depth owner - the transfer source is expected to
      // be different than the destination.
      if (!transfer_source.IsEmpty() && transfer_source != dest) {
        return true;
      }
    }
    return false;
  };
  // start_tiles_base_relative may already be in the next 11 bits - wrap the
  // start tile index to use the same code as if that was not the case.
  uint32_t start_tiles = (dest.base_tiles + start_tiles_base_relative) &
                         (xenos::kEdramTileCount - 1);
  uint32_t end_tiles = start_tiles + length_tiles;
  if (would_require_transfers_in_extent(
          start_tiles, std::min(end_tiles, xenos::kEdramTileCount))) {
    return true;
  }
  if (end_tiles > xenos::kEdramTileCount) {
    // The check extent goes to the next EDRAM addressing period.
    if (would_require_transfers_in_extent(
            0,
            std::min(end_tiles & (xenos::kEdramTileCount - 1), start_tiles))) {
      return true;
    }
  }
  return false;
}

void RenderTargetCache::AddEdramStencilMaybeSetBits(uint32_t start_tiles,
                                                    uint32_t length_tiles,
                                                    uint32_t bits) {
  bits &= 0xFF;
  if (!bits) {
    return;
  }
  length_tiles = std::min(length_tiles, uint32_t(xenos::kEdramTileCount));
  for (uint32_t i = 0; i < length_tiles; ++i) {
    edram_stencil_maybe_set_bits_[(start_tiles + i) &
                                  (xenos::kEdramTileCount - 1)] |=
        uint8_t(bits);
  }
}

void RenderTargetCache::SetEdramStencilMaybeSetBits(uint32_t start_tiles,
                                                    uint32_t length_tiles,
                                                    uint32_t bits) {
  ++edram_stencil_bits_version_;
  length_tiles = std::min(length_tiles, uint32_t(xenos::kEdramTileCount));
  for (uint32_t i = 0; i < length_tiles; ++i) {
    edram_stencil_maybe_set_bits_[(start_tiles + i) &
                                  (xenos::kEdramTileCount - 1)] =
        uint8_t(bits);
  }
}

uint32_t RenderTargetCache::GetEdramStencilMaybeSetBits(
    uint32_t start_tiles, uint32_t end_tiles) const {
  if (!cvars::rt_cache_edram_stencil_bit_map || start_tiles >= end_tiles) {
    return 0xFF;
  }
  uint32_t bits = 0;
  for (uint32_t i = start_tiles; i < end_tiles && bits != 0xFF; ++i) {
    bits |= edram_stencil_maybe_set_bits_[i & (xenos::kEdramTileCount - 1)];
  }
  return bits;
}

void RenderTargetCache::ChangeOwnership(
    RenderTargetKey dest, uint32_t start_tiles_base_relative,
    uint32_t length_tiles, std::vector<Transfer>* transfers_append_out,
    const Transfer::Rectangle* resolve_clear_cutout) {
  // xenos::kEdramTileCount with length 0 is fine if both the start and the end
  // are clamped to xenos::kEdramTileCount.
  assert_true(start_tiles_base_relative <=
              (xenos::kEdramTileCount - uint32_t(length_tiles != 0)));
  assert_true(length_tiles <= xenos::kEdramTileCount);
  if (length_tiles == 0) {
    return;
  }
  const size_t transfers_before =
      transfers_append_out ? transfers_append_out->size() : 0;
  const uint32_t transfers_back_end_before =
      transfers_before ? transfers_append_out->back().end_tiles : 0;
  uint32_t dest_pitch_tiles = dest.GetPitchTiles();
  bool dest_is_64bpp = dest.Is64bpp();
  // Native scale render targets are kept out of host depth tracking entirely
  // so the host depth buffer region only ever holds data at the global scale
  // and transfers never read host depth across scale classes. Ranges keep
  // their old scaled host owners, which is fine. Host depth is only used where
  // it still round trips to guest depth. Sub threshold depth just loses
  // float32 precision on round trips anyways.
  bool host_depth_encoding_different =
      dest.is_depth && !dest.scale_native &&
      GetPath() == Path::kHostRenderTargets &&
      IsHostDepthEncodingDifferent(dest.GetDepthFormat());
  // Tracks whether ownership_ranges_ was actually modified, for the claim
  // memo versioning - a walk where every range is already owned by `dest`
  // must not invalidate other slots' memos.
  bool ownership_mutated = false;
  auto change_ownership_in_extent = [&](uint32_t extent_start,
                                        uint32_t extent_end) {
    // The map contains consecutive ranges, merged if the adjacent ones are the
    // same. Find the range starting at >= the start. A portion of the range
    // preceding it may be intersecting the render target's range (or even fully
    // contain it) - split it into the untouched head and the claimed tail if
    // needed.
    auto it = ownership_ranges_.lower_bound(extent_start);
    if (it != ownership_ranges_.begin()) {
      auto it_pre = std::prev(it);
      if (it_pre->second.end_tiles > extent_start &&
          !it_pre->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Different render target overlapping the range - split the head.
        ownership_mutated = true;
        ownership_ranges_.emplace(extent_start, it_pre->second);
        it_pre->second.end_tiles = extent_start;
        // Let the next loop do the transfer and needed merging and splitting
        // starting from the added tail.
        it = std::next(it_pre);
      }
    }
    while (it != ownership_ranges_.end()) {
      if (it->first >= extent_end) {
        // Outside the touched extent already.
        break;
      }
      if (it->second.IsOwnedBy(dest, host_depth_encoding_different)) {
        // Already owned by the needed render target - no need to transfer
        // anything.
        ++it;
        continue;
      }
      // Take over the current range. Handle the tail - may be outside the range
      // (split in this case) or within it.
      ownership_mutated = true;
      if (it->second.end_tiles > extent_end) {
        // Split the tail.
        ownership_ranges_.emplace(extent_end, it->second);
        it->second.end_tiles = extent_end;
      }
      if (transfers_append_out) {
        RenderTargetKey transfer_source = it->second.render_target;
        // Only perform the copying when actually changing the latest owner, not
        // just the latest host depth owner - the transfer source is expected to
        // be different than the destination.
        if (!transfer_source.IsEmpty() && transfer_source != dest) {
          uint32_t transfer_end_tiles =
              std::min(it->second.end_tiles, extent_end);
          if (!resolve_clear_cutout ||
              Transfer::GetRangeRectangles(it->first, transfer_end_tiles,
                                           dest.base_tiles, dest_pitch_tiles,
                                           dest.msaa_samples, dest_is_64bpp,
                                           nullptr, resolve_clear_cutout)) {
            RenderTargetKey transfer_host_depth_source =
                host_depth_encoding_different
                    ? it->second.GetHostDepthRenderTarget(dest.GetDepthFormat())
                    : RenderTargetKey();
            if (transfer_host_depth_source == transfer_source) {
              // Same render target, don't provide a separate host depth source.
              transfer_host_depth_source = RenderTargetKey();
            }
            if (!transfers_append_out->empty() &&
                transfers_append_out->back().end_tiles == it->first &&
                transfers_append_out->back().source->key() == transfer_source &&
                ((transfers_append_out->back().host_depth_source == nullptr) ==
                 transfer_host_depth_source.IsEmpty()) &&
                (transfer_host_depth_source.IsEmpty() ||
                 transfers_append_out->back().host_depth_source->key() ==
                     transfer_host_depth_source)) {
              // Extend the last transfer if, for example, transferring color,
              // but host depth is different.
              transfers_append_out->back().end_tiles = transfer_end_tiles;
            } else {
              auto transfer_source_rt_it =
                  render_targets_.find(transfer_source);
              if (transfer_source_rt_it != render_targets_.end()) {
                assert_not_null(transfer_source_rt_it->second);
                auto transfer_host_depth_source_rt_it =
                    !transfer_host_depth_source.IsEmpty()
                        ? render_targets_.find(transfer_host_depth_source)
                        : render_targets_.end();
                if (transfer_host_depth_source.IsEmpty() ||
                    transfer_host_depth_source_rt_it != render_targets_.end()) {
                  assert_false(transfer_host_depth_source_rt_it !=
                                   render_targets_.end() &&
                               !transfer_host_depth_source_rt_it->second);
                  transfers_append_out->emplace_back(
                      it->first, transfer_end_tiles,
                      transfer_source_rt_it->second,
                      transfer_host_depth_source_rt_it != render_targets_.end()
                          ? transfer_host_depth_source_rt_it->second
                          : nullptr);
                }
              }
            }
          }
        }
      }
      // Claim the current range.
      it->second.render_target = dest;
      if (host_depth_encoding_different) {
        it->second.GetHostDepthRenderTarget(dest.GetDepthFormat()) = dest;
      }
      // Check if can merge with the next range after claiming.
      std::map<uint32_t, OwnershipRange>::iterator it_next;
      if (it != ownership_ranges_.end()) {
        it_next = std::next(it);
        if (it_next != ownership_ranges_.end() &&
            it_next->second.AreOwnersSame(it->second)) {
          // Merge with the next range.
          it->second.end_tiles = it_next->second.end_tiles;
          auto it_after = std::next(it_next);
          ownership_ranges_.erase(it_next);
          it_next = it_after;
        }
      } else {
        it_next = ownership_ranges_.end();
      }
      // Check if can merge with the previous range after claiming and merging
      // with the next (thus obtaining the correct end pointer).
      if (it != ownership_ranges_.begin()) {
        auto it_prev = std::prev(it);
        if (it_prev->second.AreOwnersSame(it->second)) {
          it_prev->second.end_tiles = it->second.end_tiles;
          ownership_ranges_.erase(it);
        }
      }
      it = it_next;
    }
  };
  // start_tiles_base_relative may already be in the next 11 bits - wrap the
  // start tile index to use the same code as if that was not the case.
  uint32_t start_tiles = (dest.base_tiles + start_tiles_base_relative) &
                         (xenos::kEdramTileCount - 1);
  uint32_t end_tiles = start_tiles + length_tiles;
  change_ownership_in_extent(start_tiles,
                             std::min(end_tiles, xenos::kEdramTileCount));
  if (end_tiles > xenos::kEdramTileCount) {
    // The ownership change extent goes to the next EDRAM addressing period.
    change_ownership_in_extent(
        0, std::min(end_tiles & (xenos::kEdramTileCount - 1), start_tiles));
  }
  if (ownership_mutated) {
    ++ownership_ranges_version_;
  }
  if (transfers_append_out && !transfers_append_out->empty() &&
      (transfers_append_out->size() > transfers_before ||
       transfers_append_out->back().end_tiles != transfers_back_end_before)) {
    // The transfers write the destination; and what each one copies may be a
    // known constant of its source right now, in guest command order.
    auto dest_it = render_targets_.find(dest);
    if (dest_it != render_targets_.end() && dest_it->second) {
      dest_it->second->ForgetConstant();
    }
    for (size_t i = transfers_before ? transfers_before - 1 : 0;
         i < transfers_append_out->size(); ++i) {
      Transfer& transfer = (*transfers_append_out)[i];
      transfer.constant_mask = 0;
      if (!cvars::rt_cache_transfer_known_constants || !transfer.source) {
        continue;
      }
      uint32_t relative_start =
          (transfer.start_tiles - transfer.source->key().base_tiles) &
          (xenos::kEdramTileCount - 1);
      transfer.source->GetConstant(
          relative_start,
          relative_start + (transfer.end_tiles - transfer.start_tiles),
          transfer.constant_word, transfer.constant_mask);
    }
  }
}

void RenderTargetCache::NoteDrawWritesRenderTargets(
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  RenderTarget* const set_by_update = constant_set_by_update_;
  constant_set_by_update_ = nullptr;
  RenderTarget* depth_rt = last_update_used_render_targets_[0];
  if (depth_rt && depth_rt != set_by_update &&
      ((normalized_depth_control.z_enable &&
        normalized_depth_control.z_write_enable) ||
       normalized_depth_control.stencil_enable)) {
    depth_rt->ForgetConstant();
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* color_rt = last_update_used_render_targets_[1 + i];
    if (color_rt && ((normalized_color_mask >> (4 * i)) & 0b1111)) {
      color_rt->ForgetConstant();
    }
  }
}

void RenderTargetCache::ForgetAllRenderTargetConstants() {
  for (auto& render_target_pair : render_targets_) {
    if (render_target_pair.second) {
      render_target_pair.second->ForgetConstant();
    }
  }
}

}  // namespace gpu
}  // namespace xe
