/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_RENDER_TARGET_CACHE_H_

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/gpu/draw_extent_estimator.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

DECLARE_bool(depth_transfer_not_equal_test);
DECLARE_bool(depth_float24_round);
DECLARE_bool(depth_float24_convert_in_pixel_shader);
DECLARE_bool(draw_resolution_scaled_texture_offsets);
DECLARE_bool(gamma_render_target_as_unorm16);
DECLARE_bool(native_2x_msaa);
DECLARE_bool(native_stencil_value_output);
DECLARE_int32(log_rt_cache_prebind);
DECLARE_bool(snorm16_render_target_full_range);

namespace xe {
namespace gpu {

namespace vulkan { class VulkanRenderTargetCache; }

class RenderTargetCache {
  // Runtime snapshot canonicalization must atomically publish or restore the
  // ownership map after backend transfers; no public mutable map is exposed.
  friend class vulkan::VulkanRenderTargetCache;
 public:
  enum : uint32_t {
    kRenderTargetHeightClassFull = 0,
    kRenderTargetHeightClassBaseCap = 1,
  };

  // RTTRACE diagnostic: log every EDRAM ownership claim made by Update().
  void SetRtTraceClaims(bool enabled) { rt_trace_claims_ = enabled; }

  // High-level emulation logic implementation path.
  enum class Path {
    // Approximate method using conventional host render targets and copying
    // ("transferring ownership" of tiles) between render targets to support
    // aliasing.
    //
    // May be irreparably inaccurate, completely at the mercy of the host API's
    // fixed-function output-merger, primarily because it has to perform
    // blending - and when using a different pixel format, it will behave
    // differently (the most important factor here is the range - it's clamped
    // for normalized formats, but not for floating-point ones).
    //
    // On a Direct3D 11-level device, formats which can be mapped directly
    // (disregarding things like blending internal precision details):
    // - 8_8_8_8
    // - 2_10_10_10
    // - 32_FLOAT
    // - 32_32_FLOAT
    // - D24S8
    // Can be mapped directly, but require handling in shaders:
    // - D24FS8 with truncated SV_DepthLessEqual output (or SV_Depth, which is
    //   suboptimal, as it prevents early depth / stencil from working). To
    //   support bit-exact reinterpretation to and from D24F for unmodified
    //   areas using pixel shader depth output without unrestricted depth range,
    //   0...1 of the guest depth should be mapped to 0...0.5 on the host in the
    //   viewport and conversion.
    // Can be mapped directly, but not supporting rare edge cases:
    // - 16_16_FLOAT, k_16_16_16_16_FLOAT - the Xenos float16 doesn't have
    //   special values.
    // Significant differences:
    // - 8_8_8_8_GAMMA - the piecewise linear gamma precision distribution
    //   encoding is very different from sRGB. Linear space blending can be
    //   obtained by promoting to R16G16B16A16_UNORM, but for compact storage,
    //   conversion in pixel shader output may be done, though it results in
    //   incorrect blending, especially visible on decals in 4D5307E6. Emulating
    //   by replacing the encoding with sRGB for render target writes and
    //   resolved texture reads could work for some games, but certain games,
    //   such as 4541080F, perform piecewise gamma encoding calculations in
    //   their code, and that produces noticeably incorrect results if the
    //   encoding is changed in guest texture memory.
    // - 2_10_10_10_FLOAT - ranges significantly different than in float16, much
    //   smaller RGB range, and alpha is fixed-point and has only 2 bits.
    // - 16_16, 16_16_16_16 - has -32 to 32 range, not -1 to 1 - need either to
    //   truncate the range for blending to work correctly, or divide by 32 in
    //   shaders breaking multiplication in blending.
    kHostRenderTargets,

    // Custom output-merger implementation, with full per-pixel and per-sample
    // control, however, only available on hosts with raster-ordered writes from
    // pixel shaders.
    kPixelShaderInterlock,
  };

  // Pixel shader interlock implementation helpers.

  // Appended to the format in the format constant via bitwise OR.
  enum : uint32_t {
    kPSIColorFormatFlag_64bpp_Shift = xenos::kColorRenderTargetFormatBits,
    // Requires clamping of blending sources and factors.
    kPSIColorFormatFlag_FixedPointColor_Shift,
    kPSIColorFormatFlag_FixedPointAlpha_Shift,

    kPSIColorFormatFlag_64bpp = uint32_t(1) << kPSIColorFormatFlag_64bpp_Shift,
    kPSIColorFormatFlag_FixedPointColor =
        uint32_t(1) << kPSIColorFormatFlag_FixedPointColor_Shift,
    kPSIColorFormatFlag_FixedPointAlpha =
        uint32_t(1) << kPSIColorFormatFlag_FixedPointAlpha_Shift,
  };

  static constexpr uint32_t AddPSIColorFormatFlags(
      xenos::ColorRenderTargetFormat format) {
    uint32_t format_flags = uint32_t(format);
    if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
        format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
      format_flags |= kPSIColorFormatFlag_64bpp;
    }
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
        format == xenos::ColorRenderTargetFormat::k_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
      format_flags |= kPSIColorFormatFlag_FixedPointColor |
                      kPSIColorFormatFlag_FixedPointAlpha;
    } else if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               format == xenos::ColorRenderTargetFormat::
                             k_2_10_10_10_FLOAT_AS_16_16_16_16) {
      format_flags |= kPSIColorFormatFlag_FixedPointAlpha;
    }
    return format_flags;
  }

  static void GetPSIColorFormatInfo(xenos::ColorRenderTargetFormat format,
                                    uint32_t write_mask, float& clamp_rgb_low,
                                    float& clamp_alpha_low,
                                    float& clamp_rgb_high,
                                    float& clamp_alpha_high,
                                    uint32_t& keep_mask_low,
                                    uint32_t& keep_mask_high);

  virtual ~RenderTargetCache();

  virtual Path GetPath() const = 0;

  // Resolution scaling on the EDRAM side is performed by multiplying the EDRAM
  // tile size by the resolution scale.
  // Note: Only integer scaling factors are provided because fractional ones,
  // even with 0.5 granularity, cause significant issues in addition to the ones
  // already present with integer scaling. 1.5 (from 1280x720 to 1920x1080) may
  // be useful, but it would cause pixel coverage issues with odd dimensions of
  // screen-space geometry, most importantly 1x1 that is often the final step in
  // reduction algorithms such as average luminance computation in HDR. A
  // single-pixel quad, either 0...1 without half-pixel offset or 0.5...1.5 with
  // it (covers only the first pixel according the top-left rule), with 1.5x
  // resolution scaling, would become 0...1.5 (only the first pixel covered) or
  // 0.75...2.25 (only the second). The workaround used in Xenia for 2x and 3x
  // resolution scaling for filling the gap caused by the half-pixel offset
  // becoming whole-pixel - stretching the second column / row of pixels into
  // the first - will not work in this case, as for one-pixel primitives without
  // half-pixel offset (covering only the first pixel, but not the second, with
  // 1.5x), it will actually cause the pixel to be erased with 1.5x scaling. As
  // within one pass there can be geometry both with and without the half-pixel
  // offset (not only depending on PA_SU_VTX_CNTL::PIX_CENTER, but also with the
  // half-pixel offset possibly reverted manually), the emulator can't decide
  // whether the stretching workaround actually needs to be used. So, with 1.5x,
  // depending on how the game draws its screen-space effects and on whether the
  // workaround is used, in some cases, nothing will just be drawn to the first
  // pixel, while in other cases, the effect will be drawn to it, but the
  // stretching workaround will replace it with the undefined value in the
  // second pixel. Also, with 1.5x, rounding of integer coordinates becomes
  // complicated, also in part due to the half-pixel offset. Odd texture sizes
  // would need to be rounded down, as according to the top-left rule, a 1.5x1.5
  // quad at the 0 or 0.75 origin (after the scaling) will cover only 1 pixel -
  // so, if the resulting texture was 2x2 rather than 1x1, undefined pixels
  // would participate in filtering. However, 1x1 scissor rounded to 1x1, with
  // the half-pixel offset of vertices, would cause the entire 0.75...2.25 quad
  // to be discarded.
  uint32_t draw_resolution_scale_x() const { return draw_resolution_scale_x_; }
  uint32_t draw_resolution_scale_y() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x() > 1 || draw_resolution_scale_y() > 1;
  }

  // Whether surfaces with this pitch render native per the scale threshold.
  // Compares the tile-aligned width, so it's a pure function of key fields.
  bool IsScaleNativeForPitch(uint32_t pitch_tiles_at_32bpp,
                             xenos::MsaaSamples msaa_samples) const;
  // Same for RB_SURFACE_INFO
  bool IsDrawScaleNative() const;
  // The rows the last Update estimated the draw to use (guest pixels, from the
  // scissor, the viewport or the vertices) - also the bound of its host pixels.
  uint32_t last_update_height_used() const { return last_update_height_used_; }
  // Scale of the current draw, the global scale or 1x1 under the threshold.
  // Everything per-draw must use these so a draw never mixes scales.
  // Quietly assuming the global scale all but promises a bunch of mixed-
  // space artifacts (trust me).
  uint32_t GetDrawScaleX() const {
    return (IsDrawScaleNative() ? 1 : draw_resolution_scale_x())
           << uint32_t(draw_samples_as_pixels_);
  }
  uint32_t GetDrawScaleY() const {
    return (IsDrawScaleNative() ? 1 : draw_resolution_scale_y())
           << uint32_t(draw_samples_as_pixels_);
  }

  // Virtual (both the common code and the implementation may do something
  // here), don't call from destructors (does work not needed for shutdown
  // also).
  virtual void ClearCache();

  virtual void BeginFrame();

  virtual bool Update(bool is_rasterization_done,
                      reg::RB_DEPTHCONTROL normalized_depth_control,
                      uint32_t normalized_color_mask,
                      const Shader& vertex_shader);

  // Stencil bits the current draw may set to 1 in its depth / stencil render
  // target (see RenderTarget::stencil_maybe_set_bits).
  static uint32_t GetDrawStencilMaybeSetBits(
      const RegisterFile& regs, reg::RB_DEPTHCONTROL normalized_depth_control);

  // The pixel shader of the draw passed to the next Update, used to prove that
  // a clear-like draw overwrites every sample of its render targets (it must
  // not kill pixels). Null if the draw has no pixel shader.
  void SetUpdatePixelShader(const Shader* pixel_shader) {
    update_pixel_shader_ = pixel_shader;
  }
  // rt_cache_transfer_known_constants: called for every draw after the render
  // targets are set up - the render targets it may write forget their
  // constants (except the one its own clear has just set).
  void NoteDrawWritesRenderTargets(reg::RB_DEPTHCONTROL normalized_depth_control,
                                   uint32_t normalized_color_mask);
  // An async draw with no usable shader may be skipped after Update. Its
  // coverage cannot justify discarding the previous render target contents.
  void SetUpdateDrawMayBeSkipped(bool may_be_skipped) {
    update_draw_may_be_skipped_ = may_be_skipped;
  }
  // For a guest 4x depth draw, treat its samples as a 2x2 grid of 1x pixels.
  // The matching RB_SURFACE_INFO is rewritten for the duration of that draw.
  void SetDrawSamplesAsPixels(bool samples_as_pixels) {
    draw_samples_as_pixels_ = samples_as_pixels;
  }
  bool draw_samples_as_pixels() const { return draw_samples_as_pixels_; }
  // vulkan_msaa_4x_as_2x2: resolve clears of 4x surfaces also target the 1x
  // double-size key, so they don't take the range back to a 4x image.
  void SetMsaa4xClearsAs2x2(bool enabled) { msaa_4x_clears_as_2x2_ = enabled; }

  // Returns bits where 0 is whether a depth render target is currently bound on
  // the host and 1... are whether the same applies to color render targets, and
  // formats (resource formats, but if needed, with gamma taken into account) of
  // each.
  uint32_t GetLastUpdateBoundRenderTargets(
      uint32_t* depth_and_color_formats_out = nullptr) const;

 protected:
  // Set on the first rasterized guest draw with the stencil test enabled.
  bool stencil_test_ever_enabled_ = false;

  // rt_cache_edram_stencil_bit_map: for each EDRAM tile, the bits that may be
  // 1 in the low byte of the 32-bit words it holds - the stencil when those
  // words are depth / stencil samples. A property of the EDRAM contents, not
  // of the render target holding them, so ownership transfers (which preserve
  // the words) don't change it. Every write of guest draws and clears
  // accumulates into it; a proven whole-range stencil overwrite replaces it.
  uint8_t edram_stencil_maybe_set_bits_[xenos::kEdramTileCount] = {};
  // |start_tiles| may be past the first EDRAM addressing period.
  void AddEdramStencilMaybeSetBits(uint32_t start_tiles, uint32_t length_tiles,
                                   uint32_t bits);
  void SetEdramStencilMaybeSetBits(uint32_t start_tiles, uint32_t length_tiles,
                                   uint32_t bits);
  // Draws repeat the same claim thousands of times a frame: an accumulation
  // identical to the slot's previous one, with no replacement in between,
  // changes nothing.
  struct EdramStencilBitsMemo {
    uint32_t start_tiles = UINT32_MAX;
    uint32_t length_tiles = 0;
    uint32_t bits = 0;
    uint64_t version = 0;
  };
  EdramStencilBitsMemo
      edram_stencil_bits_memos_[1 + xenos::kMaxColorRenderTargets];
  // Bumped by every replacement (accumulations only ever add).
  uint64_t edram_stencil_bits_version_ = 1;
  // All bits if the map is disabled.
  uint32_t GetEdramStencilMaybeSetBits(uint32_t start_tiles,
                                       uint32_t end_tiles) const;
  RenderTargetCache(const RegisterFile& register_file, const Memory& memory,
                    TraceWriter* trace_writer, uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y)
      : register_file_(register_file),
        draw_resolution_scale_x_(draw_resolution_scale_x),
        draw_resolution_scale_y_(draw_resolution_scale_y),
        draw_extent_estimator_(register_file, memory, trace_writer) {
    assert_not_zero(draw_resolution_scale_x);
    assert_not_zero(draw_resolution_scale_y);
  }

  void SetDrawExtentInputReadValidator(
      DrawExtentEstimator::InputReadValidator validator, void* context) {
    draw_extent_estimator_.SetInputReadValidator(validator, context);
  }

  const RegisterFile& register_file() const { return register_file_; }

  // Render targets (0 - depth, 1... - color) of the last Update that the draw
  // overwrites entirely, whole host image included - no need to load them.
  uint32_t last_update_full_host_overwrite() const {
    return last_update_full_host_overwrite_;
  }
  // Whether the last Update performed the draw as a clear of another render
  // target (rt_cache_retarget_full_depth_clears), so the draw must be skipped.
  // Render targets of the last Update that skipped their ownership transfers
  // because the draw was proven to overwrite them entirely - the draw must
  // really execute, or the previous contents are lost.
  uint32_t last_update_fully_overwritten_mask() const {
    return last_update_fully_overwritten_mask_;
  }
  bool last_update_draw_replaced_by_clear() const {
    return last_update_clear_retarget_render_target_ != nullptr &&
           !last_update_clear_retarget_color_draw_;
  }
  // Whether the last Update performed only the depth / stencil part of a
  // depth / stencil and color clear as a clear of another render target - the
  // render targets are set up for color only, and the draw must be done with
  // depth / stencil disabled (the tests always pass for the clear).
  bool last_update_depth_replaced_by_clear() const {
    return last_update_clear_retarget_render_target_ != nullptr &&
           last_update_clear_retarget_color_draw_;
  }

  virtual bool IsGammaFormatHostStorageSeparate() const = 0;

  // Call last in implementation-specific initialization (when things like path
  // are initialized by the implementation).
  void InitializeCommon();
  // May be called from the destructor, or from the implementation shutdown to
  // destroy all render targets before destroying what they depend on in the
  // implementation.
  void DestroyAllRenderTargets(bool shutting_down);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // For host render targets, implemented via transfer of ownership of EDRAM
  // 80x16-sample tiles between host render targets. When a range is
  // transferred, its data is copied, bit-exactly from the guest's perspective
  // (when dangerous, such as because of non-propagated NaN, primarily in the
  // float16 case, by drawing to an integer view of the render target texture),
  // from the previous host render target to the new one, by drawing rectangles
  // with a pixel shader converting the previous host render target to a guest
  // bit pattern, reinterpreting it in the new format. If depth is emulated with
  // float32, this may lead to loss of data - specifically for depth, both guest
  // format ownership and float32 ownership are tracked, and to let color data
  // overwrite depth data, loading during ownership transfer is done from
  // intersections of the current guest ownership ranges and float32 ownership
  // ranges. Ownership transfer happens when a render target is needed - based
  // on the current viewport; or, if no viewport is available, ownership of the
  // rest of the EDRAM is transferred.

  union RenderTargetKey {
    uint32_t key;
    struct {
      uint32_t base_tiles : xenos::kEdramBaseTilesBits;  // 11
      // At 4x MSAA (2 horizontal samples), max. align(8192 * 2, 80) / 80 = 205.
      // For pitch at 64bpp, multiply by 2 (or use GetPitchTiles).
      uint32_t pitch_tiles_at_32bpp : 8;                          // 19
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 21
      uint32_t is_depth : 1;                                      // 22
      // Ignoring the blending precision.
      uint32_t resource_format : xenos::kRenderTargetFormatBits;  // 26
      // 1 if this render target is kept at native guest resolution because of
      // draw_resolution_scale_threshold. Native and scaled targets at the same
      // base are distinct objects and ownership can be transferred. One bit is
      // enough. The only classes are the global scale and 1x1. Keys never
      // carry arbitrary scales.
      uint32_t scale_native : 1;  // 27
      // Host image height class: full height or render_target_height_cap.
      // Different classes are distinct objects, like
      // scale_native, so their allocation extents remain immutable.
      uint32_t height_class : 2;  // Bits 27-28 (29 total key bits used)
    };

    RenderTargetKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const RenderTargetKey& render_target_key) const {
        return std::hash<uint32_t>{}(render_target_key.key);
      }
    };
    bool operator==(const RenderTargetKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const RenderTargetKey& other_key) const {
      return !(*this == other_key);
    }

    bool IsEmpty() const {
      // Meaningless when pitch_tiles_at_32bpp == 0, but for comparison
      // purposes, only treat everything being 0 as a special case.
      return key == 0;
    }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      assert_false(is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      assert_true(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
    bool Is64bpp() const {
      if (is_depth) {
        return false;
      }
      return xenos::IsColorRenderTargetFormat64bpp(GetColorFormat());
    }
    const char* GetFormatName() const {
      return is_depth ? xenos::GetDepthRenderTargetFormatName(GetDepthFormat())
                      : xenos::GetColorRenderTargetFormatName(GetColorFormat());
    }

    uint32_t GetPitchTiles() const {
      return pitch_tiles_at_32bpp << uint32_t(Is64bpp());
    }
    static constexpr uint32_t GetWidth(uint32_t pitch_tiles_at_32bpp,
                                       xenos::MsaaSamples msaa_samples) {
      return pitch_tiles_at_32bpp *
             (xenos::kEdramTileWidthSamples >>
              uint32_t(msaa_samples >= xenos::MsaaSamples::k4X));
    }
    uint32_t GetWidth() const {
      return GetWidth(pitch_tiles_at_32bpp, msaa_samples);
    }

    std::string GetDebugName() const {
      return fmt::format("RT @ {}t, <{}t>, {}xMSAA, {}{}", base_tiles,
                         GetPitchTiles(), uint32_t(1) << uint32_t(msaa_samples),
                         GetFormatName(), scale_native ? ", native" : "");
    }
  };

  // The scale this render target is actually created and drawn at.
  uint32_t GetKeyScaleX(RenderTargetKey key) const {
    return key.scale_native ? 1 : draw_resolution_scale_x();
  }
  uint32_t GetKeyScaleY(RenderTargetKey key) const {
    return key.scale_native ? 1 : draw_resolution_scale_y();
  }

  class RenderTarget {
   public:
    virtual ~RenderTarget() = default;
    // Exclusive ownership, plus no point in moving (only allocated via new).
    RenderTarget(const RenderTarget& render_target) = delete;
    RenderTarget& operator=(const RenderTarget& render_target) = delete;
    RenderTarget(RenderTarget&& render_target) = delete;
    RenderTarget& operator=(RenderTarget&& render_target) = delete;
    RenderTargetKey key() const { return key_; }

    // Depth / stencil only: stencil bits that may be 1 anywhere in the render
    // target, accumulated from stencil-writing draws, clears and ownership
    // transfers into it. Transfers that write stencil bit by bit (no stencil
    // export) skip the bits that are 0 in their source.
    uint32_t stencil_maybe_set_bits() const { return stencil_maybe_set_bits_; }
    void AddStencilMaybeSetBits(uint32_t bits) {
      stencil_maybe_set_bits_ |= bits & 0xFF;
    }

    // render_target_height_cap: the last frame a draw needed this full-height
    // render target's rows beyond the cap. A full-height render target only
    // keeps the base from using a capped one while such draws are recent.
    uint64_t last_full_height_need_frame = 0;

    // rt_cache_transfer_known_constants: every sample of the tiles
    // [start, end) relative to the base holds the guest EDRAM word in the bits
    // of the mask, as a clear left it in guest command order. Anything else
    // writing the render target forgets it.
    bool GetConstant(uint32_t start_tiles_relative,
                     uint32_t end_tiles_relative, uint32_t& word,
                     uint32_t& mask) const {
      if (!constant_mask_ || start_tiles_relative < constant_start_tiles_ ||
          end_tiles_relative > constant_end_tiles_) {
        return false;
      }
      word = constant_word_;
      mask = constant_mask_;
      return true;
    }
    void SetConstant(uint32_t start_tiles_relative,
                     uint32_t end_tiles_relative, uint32_t word,
                     uint32_t mask) {
      constant_start_tiles_ = start_tiles_relative;
      constant_end_tiles_ = end_tiles_relative;
      constant_word_ = word & mask;
      constant_mask_ = start_tiles_relative < end_tiles_relative ? mask : 0;
    }
    void ForgetConstant() { constant_mask_ = 0; }

   protected:
    RenderTarget(RenderTargetKey key) : key_(key) {}

   private:
    RenderTargetKey key_;
    uint32_t stencil_maybe_set_bits_ = 0;
    uint32_t constant_start_tiles_ = 0;
    uint32_t constant_end_tiles_ = 0;
    uint32_t constant_word_ = 0;
    uint32_t constant_mask_ = 0;
  };

  struct Transfer {
    uint32_t start_tiles;
    uint32_t end_tiles;
    RenderTarget* source;
    RenderTarget* host_depth_source;
    // rt_cache_transfer_known_constants: the EDRAM word the source holds in
    // the whole range (the bits of constant_mask), taken when the transfer
    // was decided, in guest command order.
    uint32_t constant_word = 0;
    uint32_t constant_mask = 0;
    Transfer(uint32_t start_tiles, uint32_t end_tiles, RenderTarget* source,
             RenderTarget* host_depth_source)
        : start_tiles(start_tiles),
          end_tiles(end_tiles),
          source(source),
          host_depth_source(host_depth_source) {
      assert_true(start_tiles < end_tiles);
    }
    struct Rectangle {
      uint32_t x_pixels;
      uint32_t y_pixels;
      uint32_t width_pixels;
      uint32_t height_pixels;
    };
    static constexpr uint32_t kMaxRectanglesWithoutCutout = 3;
    static constexpr uint32_t kMaxCutoutBorderRectangles = 4;
    static constexpr uint32_t kMaxRectanglesWithCutout =
        kMaxRectanglesWithoutCutout * kMaxCutoutBorderRectangles;
    // Cutout can be specified for resolve clears - not to transfer areas that
    // will be cleared to a single value anyway.
    static uint32_t GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles,
                                       uint32_t base_tiles,
                                       uint32_t pitch_tiles,
                                       xenos::MsaaSamples msaa_samples,
                                       bool is_64bpp, Rectangle* rectangles_out,
                                       const Rectangle* cutout = nullptr);
    uint32_t GetRectangles(uint32_t base_tiles, uint32_t pitch_tiles,
                           xenos::MsaaSamples msaa_samples, bool is_64bpp,
                           Rectangle* rectangles_out,
                           const Rectangle* cutout = nullptr) const {
      return GetRangeRectangles(start_tiles, end_tiles, base_tiles, pitch_tiles,
                                msaa_samples, is_64bpp, rectangles_out, cutout);
    }
    bool AreSourcesSame(const Transfer& other_transfer) const {
      return source == other_transfer.source &&
             host_depth_source == other_transfer.host_depth_source;
    }

   private:
    static uint32_t AddRectangle(const Rectangle& rectangle,
                                 Rectangle* rectangles_out,
                                 const Rectangle* cutout = nullptr);
  };

  // Render targets (0 - depth, 1... - color) of the last Update whose pixels
  // inside last_update_draw_cutout() the draw provably overwrites entirely
  // (rt_cache_draw_overwrite_cutouts) - their ownership transfers may skip
  // that rectangle.
  uint32_t last_update_draw_cutout_mask() const {
    return last_update_draw_cutout_mask_;
  }
  const Transfer::Rectangle& last_update_draw_cutout() const {
    return last_update_draw_cutout_;
  }
  // The draw covers exactly the pixels of last_update_draw_cutout() (edges on
  // pixel boundaries) - nothing around it is written, so the transfers of the
  // pixels around may also be done after the draw (clear chains).
  bool last_update_draw_cutout_exact() const {
    return last_update_draw_cutout_exact_;
  }
  // The aspects (bit 0 - depth, bit 1 - stencil) of the depth render target
  // the draw writes entirely inside last_update_draw_cutout(), reading neither
  // aspect - whether or not it's fully overwritten (then last_update_draw_
  // cutout_mask() has bit 0 too).
  uint32_t last_update_draw_cutout_depth_aspects() const {
    return last_update_draw_cutout_depth_aspects_;
  }

  union HostDepthStoreRectangleConstant {
    uint32_t constant;
    struct {
      // - 1 because the maximum is 0x1FFF / 8, not 0x2000 / 8.
      uint32_t x_pixels_div_8 : xenos::kResolveSizeBits - 1 -
          xenos::kResolveAlignmentPixelsLog2;
      uint32_t y_pixels_div_8 : xenos::kResolveSizeBits - 1 -
          xenos::kResolveAlignmentPixelsLog2;
      uint32_t width_pixels_div_8_minus_1 : xenos::kResolveSizeBits - 1 -
          xenos::kResolveAlignmentPixelsLog2;
    };
    HostDepthStoreRectangleConstant() : constant(0) {
      static_assert_size(*this, sizeof(constant));
    }
  };

  union HostDepthStoreRenderTargetConstant {
    uint32_t constant;
    struct {
      uint32_t pitch_tiles : xenos::kEdramPitchTilesBits;
      uint32_t resolution_scale_x : 3;
      uint32_t resolution_scale_y : 3;
      // Whether 2x MSAA is supported natively rather than through 4x.
      uint32_t msaa_2x_supported : 1;
    };
    HostDepthStoreRenderTargetConstant() : constant(0) {
      static_assert_size(*this, sizeof(constant));
    }
  };

  struct HostDepthStoreConstants {
    HostDepthStoreRectangleConstant rectangle;
    HostDepthStoreRenderTargetConstant render_target;
  };

  struct ResolveCopyDumpRectangle {
    RenderTarget* render_target;
    // If rows == 1:
    //   Row row_first span:
    //     [row_first_start, row_last_end)
    // If rows > 1:
    //   Row row_first + row span:
    //     [row_first_start, row_length_used)
    //   Rows [row_first + 1, row_first + rows - 1) span:
    //     [row * pitch, row * pitch + row_length_used)
    //   Row row_first + rows - 1 span:
    //     [row * pitch, row * pitch + row_last_end)
    uint32_t row_first;
    uint32_t rows;
    uint32_t row_first_start;
    uint32_t row_last_end;
    ResolveCopyDumpRectangle(RenderTarget* render_target, uint32_t row_first,
                             uint32_t rows, uint32_t row_first_start,
                             uint32_t row_last_end)
        : render_target(render_target),
          row_first(row_first),
          rows(rows),
          row_first_start(row_first_start),
          row_last_end(row_last_end) {}
    struct Dispatch {
      // Base plus offset may exceed the EDRAM tile count in case of EDRAM
      // addressing wrapping.
      uint32_t offset;
      uint32_t width_tiles;
      uint32_t height_tiles;
    };
    static constexpr uint32_t kMaxDispatches = 3;
    uint32_t GetDispatches(uint32_t pitch_tiles, uint32_t row_length_used,
                           Dispatch* dispatches_out) const {
      if (!rows) {
        return 0;
      }
      // If the first and / or the last rows have the same X spans as the middle
      // part, merge them with it.
      uint32_t dispatch_count = 0;
      if (rows == 1 || row_first_start) {
        Dispatch& dispatch_first = dispatches_out[dispatch_count++];
        dispatch_first.offset = row_first * pitch_tiles + row_first_start;
        dispatch_first.width_tiles =
            (rows == 1 ? row_last_end : row_length_used) - row_first_start;
        dispatch_first.height_tiles = 1;
        if (rows == 1) {
          return dispatch_count;
        }
      }
      uint32_t mid_row_first = row_first + 1;
      uint32_t mid_rows = rows - 2;
      if (!row_first_start) {
        --mid_row_first;
        ++mid_rows;
      }
      if (row_last_end == row_length_used) {
        ++mid_rows;
      }
      if (mid_rows) {
        Dispatch& dispatch_mid = dispatches_out[dispatch_count++];
        dispatch_mid.offset = mid_row_first * pitch_tiles;
        dispatch_mid.width_tiles = row_length_used;
        dispatch_mid.height_tiles = mid_rows;
      }
      if (row_last_end != row_length_used) {
        Dispatch& dispatch_last = dispatches_out[dispatch_count++];
        dispatch_last.offset = (row_first + rows - 1) * pitch_tiles;
        dispatch_last.width_tiles = row_last_end;
        dispatch_last.height_tiles = 1;
      }
      return dispatch_count;
    }
  };

  virtual uint32_t GetMaxRenderTargetWidth() const = 0;
  virtual uint32_t GetMaxRenderTargetHeight() const = 0;

  // Returns the height of a render target that's needed and can be created,
  // taking guest and host limits into account. EDRAM base and 32bpp/64bpp are
  // not taken into account, the same height is used for all render targets even
  // if the implementation supports mixed-size render targets, so the
  // implementation can freely disable individual render targets and let the
  // other ones use the newly available space without restarting the whole
  // render pass (on Vulkan, the actually used height is specified in
  // VkFramebuffer).
  uint32_t GetRenderTargetHeight(uint32_t pitch_tiles_at_32bpp,
                                 xenos::MsaaSamples msaa_samples) const;
  // The height of the host image of a render target - capped according to
  // RenderTargetKey::height_class.
  uint32_t GetRenderTargetHeight(RenderTargetKey key) const;
  uint32_t last_update_height_used_ = UINT32_MAX;
  // render_target_height_cap bookkeeping (frames, and the reasons a draw's
  // render targets got the full or the capped height).
  uint64_t height_cap_frame_ = 0;
  uint64_t height_cap_capped_draws_ = 0;
  uint64_t height_cap_too_tall_draws_ = 0;
  uint64_t height_cap_sticky_draws_ = 0;
  uint32_t height_cap_too_tall_logged_ = 0;

  virtual RenderTarget* CreateRenderTarget(RenderTargetKey key) = 0;

  // Backends opting in must preserve transfers outside the proven clear.
  virtual bool SupportsNewOwnerClearTransfers() const { return false; }

  // Whether depth buffer is encoded differently on the host, thus after
  // aliasing naively, precision may be lost - host depth must only be
  // overwritten if the new guest value is different than the current host depth
  // when converted to the guest format (this catches the usual case of
  // overwriting the depth buffer for clearing it mostly). 534507D6 intro
  // cutscene, for example, has a good example of corruption that happens if
  // this is not handled - the upper 1280x384 pixels are rendered in a very
  // "striped" way if the depth precision is lost (if this is made always return
  // false).
  virtual bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const = 0;

  void ResetAccumulatedRenderTargets() {
    are_accumulated_render_targets_valid_ = false;
  }
  RenderTarget* const* last_update_accumulated_render_targets() const {
    assert_true(GetPath() == Path::kHostRenderTargets);
    return last_update_accumulated_render_targets_;
  }
  // The render targets the last draw writes (not updated when the draw is
  // replaced by a clear entirely).
  RenderTarget* const* last_update_used_render_targets() const {
    return last_update_used_render_targets_;
  }
  // Forgets the constants of all render targets (contents replaced wholesale).
  void ForgetAllRenderTargetConstants();

  const std::vector<Transfer>* last_update_transfers() const {
    assert_true(GetPath() == Path::kHostRenderTargets);
    return last_update_transfers_;
  }

  // With rt_cache_retarget_full_depth_clears, the render target the last
  // Update replaced the draw with a clear of (nullptr if the draw is to be
  // done normally), the rectangle in its pixels and the guest depth / stencil
  // clear value. The draw must be skipped and the clear performed instead.
  // Newly claimed owners also require last_update_transfers(), cutting out
  // the clear rectangle while preserving its untouched surroundings.
  RenderTarget* last_update_clear_retarget_render_target() const {
    return last_update_clear_retarget_render_target_;
  }
  const Transfer::Rectangle& last_update_clear_retarget_rectangle() const {
    return last_update_clear_retarget_rectangle_;
  }
  uint64_t last_update_clear_retarget_value() const {
    return last_update_clear_retarget_value_;
  }
  // Set in a depth resolve clear value (above the 32-bit guest value) when only
  // the depth must be cleared, keeping the stencil
  // (rt_cache_retarget_depth_only_clears).
  static constexpr uint64_t kResolveClearDepthOnlyBit = uint64_t(1) << 63;
  // Likewise for clearing only the stencil, keeping the depth.
  static constexpr uint64_t kResolveClearStencilOnlyBit = uint64_t(1) << 62;

  HostDepthStoreRenderTargetConstant GetHostDepthStoreRenderTargetConstant(
      uint32_t pitch_tiles, bool msaa_2x_supported) const {
    HostDepthStoreRenderTargetConstant constant;
    constant.pitch_tiles = pitch_tiles;
    // 3 bits for each.
    assert_true(draw_resolution_scale_x() <= 7);
    assert_true(draw_resolution_scale_y() <= 7);
    constant.resolution_scale_x = draw_resolution_scale_x();
    constant.resolution_scale_y = draw_resolution_scale_y();
    constant.msaa_2x_supported = uint32_t(msaa_2x_supported);
    return constant;
  }
  void GetHostDepthStoreRectangleInfo(
      const Transfer::Rectangle& transfer_rectangle,
      xenos::MsaaSamples msaa_samples,
      HostDepthStoreRectangleConstant& rectangle_constant_out,
      uint32_t& group_count_x_out, uint32_t& group_count_y_out) const;

  // Returns mappings between ranges within the specified tile rectangle (not
  // render target texture rectangle - textures may have any pitch they need)
  // from ResolveInfo::GetCopyEdramTileSpan and render targets owning them to
  // rectangles_out.
  void GetResolveCopyRectanglesToDump(
      uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch,
      std::vector<ResolveCopyDumpRectangle>& rectangles_out) const;

  // True if everything owning the resolve source region is native scale, in
  // which case the resolve can be done at 1x1 into shared memory instead of
  // duplicating pixels into the scaled layout (which ruins linear filtering).
  // Decided from actual ownership rather than resolve dimensions since one
  // range can span both classes.
  bool IsResolveSourceNativeOnly(uint32_t base, uint32_t row_length,
                                 uint32_t rows, uint32_t pitch) const;

  // Sets up the needed render targets and transfers to perform a clear in a
  // resolve operation via a host render target clear. resolve_info is expected
  // to be obtained via draw_util::GetResolveInfo. Returns whether any clears
  // need to be done (false in both empty and error cases).
  // TODO(Triang3l): Try to defer clears until the first draw in the next pass
  // (if it uses one or both render targets being cleared) for tile-based GPUs.
  bool PrepareHostRenderTargetsResolveClear(
      const draw_util::ResolveInfo& resolve_info,
      Transfer::Rectangle& clear_rectangle_out,
      RenderTarget*& depth_render_target_out,
      std::vector<Transfer>& depth_transfers_out,
      RenderTarget*& color_render_target_out,
      std::vector<Transfer>& color_transfers_out);

  // For restoring EDRAM contents from frame traces, obtains or creates a render
  // target at base 0 with of 1280 (only 1 sample and color because copying
  // between MSAA render targets and buffers is not possible in Direct3D 12, and
  // depth may require additional format conversions, not needed really) and
  // transfers ownership of the entire EDRAM to that render target. If a
  // full-EDRAM render target can't be created (for instance, due to size
  // limitations on the host), nullptr is returned.
  RenderTarget* PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
      xenos::ColorRenderTargetFormat color_format);

  // For pixel shader interlock.

  virtual void RequestPixelShaderInterlockBarrier() {}

  // To be called by the implementation when interlocked writes to all of the
  // EDRAM memory are committed with a memory barrier.
  void PixelShaderInterlockFullEdramBarrierPlaced();

  // Whether a 7e3 -> 8_8_8_8 in-place reuse should decode (HDR float to LDR
  // unorm) instead of bit-reinterpreting. Bit-reinterpret is hardware-accurate,
  // so decode applies only where the reinterpreted bytes are actually seen: a
  // matched same-base/pitch/MSAA reuse, 7e3 -> plain 8_8_8_8, whose draw blends
  // over the dest. Everything else (overwrite, reverse, gamma, cvar off) stays
  // bit-exact.
  bool IsTransferValueConverted7e3And8888(RenderTargetKey source,
                                          RenderTargetKey dest) const;

 private:
  const RegisterFile& register_file_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  DrawExtentEstimator draw_extent_estimator_;

  const Shader* update_pixel_shader_ = nullptr;
  bool update_draw_may_be_skipped_ = false;
  uint32_t skipped_full_overwrite_logged_ = 0;
  bool draw_samples_as_pixels_ = false;
  bool msaa_4x_clears_as_2x2_ = false;
  uint32_t full_overwrite_logged_ = 0;
  // Subset of the fully overwritten render targets of the last Update whose
  // whole host image (including tile padding and all EDRAM rows) the draw
  // covers, so their previous contents don't need to be loaded at all.
  uint32_t last_update_full_host_overwrite_ = 0;
  // See last_update_draw_cutout_mask(). The draw's pixels: set by
  // GetFullyOverwrittenRenderTargets for the render targets it writes
  // completely, whatever the coverage of the claimed range.
  uint32_t last_draw_overwrite_mask_ = 0;
  Transfer::Rectangle last_draw_overwrite_rect_ = {};
  bool last_draw_overwrite_exact_ = false;
  bool last_update_draw_cutout_exact_ = false;
  uint32_t last_draw_overwrite_depth_aspects_ = 0;
  uint32_t last_update_draw_cutout_depth_aspects_ = 0;
  uint32_t last_update_draw_cutout_mask_ = 0;
  uint32_t last_update_fully_overwritten_mask_ = 0;
  Transfer::Rectangle last_update_draw_cutout_ = {};
  // Bits (0 - depth, 1... - color) of the render targets used by the current
  // draw that it provably overwrites entirely within the range claimed for it
  // (a clear rectangle), so the previous contents of that range don't need to
  // be transferred into them.
  uint32_t GetFullyOverwrittenRenderTargets(
      uint32_t rts_used_bits, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, const Shader& vertex_shader,
      uint32_t pitch_pixels, uint32_t height_used);
  // Guest depth / stencil value (depth << 8 | stencil) written everywhere by
  // the last full clear GetFullyOverwrittenRenderTargets accepted for depth,
  // if it's known exactly (a constant depth of 0 or 1).
  bool last_full_clear_depth_value_valid_ = false;
  // The last draw GetFullyOverwrittenRenderTargets checked covers the whole
  // claimed range (whatever it writes there).
  bool last_draw_covers_claimed_range_ = false;
  // rt_cache_transfer_known_constants: the render target whose constant the
  // last Update set from its draw, not to be forgotten by that same draw.
  RenderTarget* constant_set_by_update_ = nullptr;
  // The draw writes the depth but leaves the stencil untouched
  // (rt_cache_retarget_depth_only_clears) - only the depth of
  // last_full_clear_depth_value_ is written.
  bool last_full_clear_depth_only_ = false;
  // The draw writes all stencil bits but leaves the depth untouched.
  bool last_full_clear_stencil_only_ = false;
  uint32_t last_full_clear_depth_value_ = 0;
  // Pixels [x0, y0, x1, y1) written with last_full_clear_depth_value_.
  uint32_t last_clear_rect_[4] = {};
  // See last_update_clear_retarget_render_target().
  RenderTarget* last_update_clear_retarget_render_target_ = nullptr;
  Transfer::Rectangle last_update_clear_retarget_rectangle_ = {};
  uint64_t last_update_clear_retarget_value_ = 0;
  bool last_update_clear_retarget_color_draw_ = false;
  uint32_t clear_retarget_logged_ = 0;

  // For host render targets.

  struct OwnershipRange {
    uint32_t end_tiles;
    // Need to store keys, not pointers to render targets themselves, because
    // ownership transfer is also what's used to determine when to place
    // barriers with pixel shader interlock, and in this case there are no host
    // render targets.
    // Render target this range is last used by.
    RenderTargetKey render_target;
    // Last host-side depth render targets that used this range even if it has
    // been used by a different render target since then, only used if the
    // respective format has a different encoding on the host. They are tracked
    // separately, overwritten if the host value converted to the guest format
    // becomes out of sync with the guest value. Even if the host uses float32
    // to emulate both unorm24 and float24 (Vulkan on AMD), the unorm24 and
    // float24 render targets are tracked separately from each other, so
    // switching between unorm24 and float24 for the same depth data (clearing
    // of most render targets is done through unorm24 without a viewport - very
    // common) is not destructive as well (f32tof24(host_f32) == guest_f24 does
    // not imply f32tou24(host_f32) == guest_u24, thus aliasing float24 with
    // unorm24 through the same float32 buffer will drop the precision of the
    // float32 value to that of an unorm24 with a totally wrong value). If the
    // range hasn't been used yet (render_target.IsEmpty() == true), these are
    // empty too.
    RenderTargetKey host_depth_render_target_unorm24;
    RenderTargetKey host_depth_render_target_float24;
    OwnershipRange(uint32_t end_tiles, RenderTargetKey render_target,
                   RenderTargetKey host_depth_render_target_unorm24,
                   RenderTargetKey host_depth_render_target_float24)
        : end_tiles(end_tiles),
          render_target(render_target),
          host_depth_render_target_unorm24(host_depth_render_target_unorm24),
          host_depth_render_target_float24(host_depth_render_target_float24) {}
    const RenderTargetKey& GetHostDepthRenderTarget(
        xenos::DepthRenderTargetFormat resource_format) const {
      assert_true(
          resource_format == xenos::DepthRenderTargetFormat::kD24S8 ||
              resource_format == xenos::DepthRenderTargetFormat::kD24FS8,
          "Illegal resource format");
      return resource_format == xenos::DepthRenderTargetFormat::kD24S8
                 ? host_depth_render_target_unorm24
                 : host_depth_render_target_float24;
    }
    RenderTargetKey& GetHostDepthRenderTarget(
        xenos::DepthRenderTargetFormat resource_format) {
      return const_cast<RenderTargetKey&>(
          const_cast<const OwnershipRange*>(this)->GetHostDepthRenderTarget(
              resource_format));
    }
    bool IsOwnedBy(RenderTargetKey key,
                   bool host_depth_encoding_different) const {
      if (render_target != key) {
        // Last time used for something else. If it's a depth render target with
        // different host depth encoding, might have been overwritten by color,
        // or by a depth render target of a different format.
        return false;
      }
      if (host_depth_encoding_different && !key.is_depth &&
          GetHostDepthRenderTarget(key.GetDepthFormat()) != key) {
        // Depth encoding is the same, but different addressing is needed.
        return false;
      }
      return true;
    }
    bool AreOwnersSame(const OwnershipRange& other_range) const {
      return render_target == other_range.render_target &&
             host_depth_render_target_unorm24 ==
                 other_range.host_depth_render_target_unorm24 &&
             host_depth_render_target_float24 ==
                 other_range.host_depth_render_target_float24;
    }
  };

  xenos::ColorRenderTargetFormat GetColorResourceFormat(
      xenos::ColorRenderTargetFormat format) const {
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        !IsGammaFormatHostStorageSeparate()) {
      return xenos::ColorRenderTargetFormat::k_8_8_8_8;
    }
    return xenos::GetStorageColorFormat(format);
  }

  RenderTarget* GetOrCreateRenderTarget(RenderTargetKey key);

  // Checks if changing ownership of the range to the specified render target
  // would require transferring data - primarily for barrier placement on the
  // pixel shader interlock path (where transfers do not involve copying, but
  // barriers are still needed before accessing ranges written before the
  // barrier and addressed by different target-independent rasterization pixel
  // positions.
  bool WouldOwnershipChangeRequireTransfers(RenderTargetKey dest,
                                            uint32_t start_tiles_base_relative,
                                            uint32_t length_tiles) const;
  // Updates ownership_ranges_, adds the transfers needed for the ownership
  // change to transfers_append_out if it's not null.
  void ChangeOwnership(
      RenderTargetKey dest, uint32_t start_tiles_base_relative,
      uint32_t length_tiles, std::vector<Transfer>* transfers_append_out,
      const Transfer::Rectangle* resolve_clear_cutout = nullptr);

  // If failed to create, may contain nullptr to prevent attempting to create a
  // render target twice.
  std::unordered_map<RenderTargetKey, RenderTarget*, RenderTargetKey::Hasher>
      render_targets_;
  struct RenderTargetLookup {
    RenderTargetKey key;
    RenderTarget* target = nullptr;
    bool valid = false;
  };
  std::array<RenderTargetLookup, 64> render_target_lookup_{};

 protected:
  // RTTRACE diagnostic lookup (may return nullptr).
  RenderTarget* FindRenderTargetForTrace(RenderTargetKey key) const {
    auto it = render_targets_.find(key);
    return it != render_targets_.end() ? it->second : nullptr;
  }

  // Exact ownership-map query used by fail-closed backend retirement.
  // known_out is false until InitializeCommon has installed the map.
  bool IsRenderTargetOwned(RenderTargetKey key, bool* known_out) const;
  bool HasRetainedRenderTargetReference(
      const RenderTarget* render_target) const;
  virtual bool HasExternalRenderTargetReferences(
      const RenderTarget* render_target) const {
    return false;
  }
  auto& GetRenderTargetsForRetirement() { return render_targets_; }

 private:

  // Map of host render targets currently containing the most up-to-date version
  // of the tile. Has no gaps, unused parts are represented by empty render
  // target keys.
  // TODO(Triang3l): Pool allocator (or a custom red-black tree with one even),
  // since standard containers use dynamic allocation for elements, though
  // changes to this throughout a frame are pretty rare.
  std::map<uint32_t, OwnershipRange> ownership_ranges_;

  // Monotonic version of ownership_ranges_, bumped on every actual mutation
  // of the map (claims, splits, interlock barriers, destruction). Lets
  // Update() skip ChangeOwnership walks that would provably be no-ops: once
  // `dest` has claimed [base, base + length), re-claiming any sub-extent with
  // an unchanged map walks the ranges and changes nothing.
  uint64_t ownership_ranges_version_ = 0;
  // Per-render-target-slot memo of the last claim made by Update()'s
  // ownership loop (0 is depth, color starting from 1). Valid only while
  // version matches ownership_ranges_version_.
  struct OwnershipClaimMemo {
    RenderTargetKey dest;
    uint32_t length_tiles = 0;
    uint64_t version = ~uint64_t(0);
  };
  OwnershipClaimMemo
      ownership_claim_memos_[1 + xenos::kMaxColorRenderTargets];

  // Render targets actually used by the draw call with the last successful
  // update. 0 is depth, color starting from 1, nullptr if not bound.
  // Only valid for non-pixel-shader-interlock paths.
  RenderTarget* last_update_used_render_targets_[1 +
                                                     xenos::kMaxColorRenderTargets] = {};
  // Render targets used by the draw call with the last successful update or
  // previous updates, unless a different or a totally new one was bound (or
  // surface info was changed), to avoid unneeded render target switching (which
  // is especially undesirable on tile-based GPUs) in the implementation if
  // simply disabling depth / stencil test or color writes and then re-enabling
  // (58410954 does this often with color). Must also be used to determine
  // whether it's safe to enable depth / stencil or writing to a specific color
  // render target in the pipeline for this draw call.
  // Only valid for non-pixel-shader-interlock paths.
  RenderTarget* last_update_accumulated_render_targets_[1 +
      xenos::kMaxColorRenderTargets] = {};
  // If false, the next update must copy last_update_used_render_targets_ to
  // last_update_accumulated_render_targets_ - it's not beneficial or even
  // incorrect to keep the previously bound render targets.
  bool are_accumulated_render_targets_valid_ = false;
  // rt_cache_prebind_learned_color: per depth render target, the color render
  // targets last added (in an empty slot) to a pass already holding it.
  struct PrebindHint {
    RenderTargetKey depth_key;
    RenderTargetKey color_keys[xenos::kMaxColorRenderTargets];
    uint8_t misses[xenos::kMaxColorRenderTargets] = {};
  };
  static constexpr size_t kPrebindHintCount = 8;
  PrebindHint prebind_hints_[kPrebindHintCount];
  size_t prebind_hint_next_ = 0;
  // Accumulated color slots (bits 1...4) bound by a hint and not yet written
  // by a draw, and the hint they came from.
  uint32_t prebind_pending_slots_ = 0;
  PrebindHint* prebind_pending_hint_ = nullptr;
  // The hint entry may be reused for another depth render target meanwhile.
  RenderTargetKey prebind_pending_depth_key_;
  uint64_t prebind_stats_[3] = {};  // Prebound, used later, unused.
  int32_t prebind_logs_ = 0;
  bool PrebindLogAllowed() {
    return prebind_logs_ < cvars::log_rt_cache_prebind && ++prebind_logs_;
  }
  PrebindHint* FindPrebindHint(RenderTargetKey depth_key, bool create);
  void LearnPrebindHint(RenderTarget* const* used_rts);
  void FinishPrebindPass();
  void PrebindLearnedColor(uint32_t pitch_tiles_at_32bpp,
                           xenos::MsaaSamples msaa_samples);
  bool rt_trace_claims_ = false;
  // After an update (for simplicity, even an unsuccessful update invalidates
  // this), contains needed ownership transfer sources for each of the current
  // render targets. They are reordered so for one source, all transfers are
  // consecutive in the array.
  std::vector<Transfer>
      last_update_transfers_[1 + xenos::kMaxColorRenderTargets];
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_RENDER_TARGET_CACHE_H_
