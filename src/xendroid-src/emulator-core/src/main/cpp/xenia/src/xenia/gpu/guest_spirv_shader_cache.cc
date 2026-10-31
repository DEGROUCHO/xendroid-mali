/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/guest_spirv_shader_cache.h"

#include <thread>

#include "xenia/base/assert.h"
#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/cvar.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/user_clip_planes.h"
#include "xenia/gpu/spirv_shader_translator.h"

DEFINE_bool(
    vulkan_early_fragment_tests_hint, true,
    "Host render target path: for pixel shaders that can't kill pixels or "
    "write depth, in draws where coverage doesn't depend on alpha (no alpha "
    "test, no alpha to coverage), force early fragment tests and leave out "
    "the alpha test code (the D3D12 and Metal kEarlyHint behavior). Without "
    "it every pixel shader writing color 0 may discard, so tile-based GPUs "
    "(Mali) can't reject hidden pixels before shading them. Off by default "
    "upstream because it faulted on Nvidia in one game.",
    "GPU");
UPDATE_from_bool(vulkan_early_fragment_tests_hint, 2026, 10, 5, 12, false);
DEFINE_int32(log_early_z_reasons, 0,
             "Diagnostic: every this many pixel shader modification lookups, "
             "log how many got early fragment tests and why the others "
             "didn't (shader kill, shader depth output, alpha test, alpha to "
             "coverage, float24 depth in the shader, polygon offset).",
             "GPU");
DEFINE_bool(
    vulkan_alpha_test_no_sample_mask, false,
    "Host render target path: for pixel shaders left on the late depth path "
    "(alpha test, pixel kill or depth output) in draws with alpha to "
    "coverage disabled, don't output gl_SampleMask. It would only be the "
    "no-op full coverage, but on Mali any coverage output forces late "
    "depth / stencil testing, so hidden pixels get shaded and then thrown "
    "away. The image is the same. Read per draw (live-toggleable).",
    "GPU");

DEFINE_bool(
    vulkan_early_tests_without_zs_writes, false,
    "Host render target path: draws whose pixel shader can kill pixels "
    "(pixel kill, alpha test) but that write neither depth nor stencil get "
    "early fragment tests too. The kill only drops color there, while on Mali "
    "a shader that can discard is otherwise tested late: occluded pixels are "
    "shaded, then rejected (GTA IV: late depth tests throw away ~40% of "
    "shaded quads). Off: no frame time gained in GTA IV, Reach or Black Ops "
    "(q608-q612), and occlusion queries then count the killed pixels as "
    "visible.",
    "GPU");
UPDATE_from_bool(vulkan_early_tests_without_zs_writes, 2026, 10, 26, 18, true);

DECLARE_bool(vulkan_software_clip_planes);
DECLARE_bool(vulkan_software_clip_pixel_mask);
DECLARE_bool(vulkan_software_clip_zero_plane_elision);
DECLARE_bool(shader_profiling);

namespace xe {
namespace gpu {

GuestSpirvShaderCache::GuestSpirvShaderCache(
    Host& host, const RegisterFile& register_file,
    const RenderTargetCache& render_target_cache)
    : host_(host),
      register_file_(register_file),
      render_target_cache_(render_target_cache) {}

GuestSpirvShaderCache::~GuestSpirvShaderCache() { Shutdown(); }

bool GuestSpirvShaderCache::Initialize() {
  translator_ = host_.CreateTranslator();
  return translator_ != nullptr;
}

void GuestSpirvShaderCache::Shutdown() { translator_.reset(); }

std::unique_ptr<SpirvShaderTranslator>
GuestSpirvShaderCache::CreateWorkerTranslator() const {
  return host_.CreateTranslator();
}

uint32_t GuestSpirvShaderCache::GetUserClipPlaneMask() const {
  const auto clip = register_file_.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t mask = clip.clip_disable ? 0 : clip.ucp_ena;
  if (!mask || clip.ucp_cull_only_ena || !cvars::vulkan_software_clip_planes ||
      translator_->features().clip_distance) return mask;
  const auto* coefficients = &register_file_[XE_GPU_REG_PA_CL_UCP_0_X];
  const uint32_t nonzero = RemoveZeroUserClipPlanes(mask, coefficients);
  if (cvars::shader_profiling) {
    // Main-thread queries only. Log once per classification, bounded to 3 lines.
    static uint32_t seen_classes = 0;
    const uint32_t classification = !nonzero ? 1u : nonzero == mask ? 4u : 2u;
    if (!(seen_classes & classification)) {
      seen_classes |= classification;
      XELOGI("ShaderClipPlanes: enabled={:02X} nonzero={:02X} elision={} "
             "p0={:08X},{:08X},{:08X},{:08X} p1={:08X},{:08X},{:08X},{:08X} "
             "p2={:08X},{:08X},{:08X},{:08X} p3={:08X},{:08X},{:08X},{:08X} "
             "p4={:08X},{:08X},{:08X},{:08X} p5={:08X},{:08X},{:08X},{:08X}",
             mask, nonzero, cvars::vulkan_software_clip_zero_plane_elision,
             coefficients[0], coefficients[1], coefficients[2], coefficients[3],
             coefficients[4], coefficients[5], coefficients[6], coefficients[7],
             coefficients[8], coefficients[9], coefficients[10], coefficients[11],
             coefficients[12], coefficients[13], coefficients[14], coefficients[15],
             coefficients[16], coefficients[17], coefficients[18], coefficients[19],
             coefficients[20], coefficients[21], coefficients[22], coefficients[23]);
    }
  }
  return cvars::vulkan_software_clip_zero_plane_elision ? nonzero : mask;
}

uint64_t GuestSpirvShaderCache::GetVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, bool ps_param_gen_used) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  // Tessellation mode selects the domain shader spacing.
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    modification.vertex.tessellation_mode =
        regs.Get<reg::VGT_HOS_CNTL>().tess_mode;
  }

  // User clip planes.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = GetUserClipPlaneMask();
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);

  // Vertex kill via the kill flag (oPts.z). The "and" operator (kill only when
  // all vertices of the primitive request it) is emulated with a cull distance.
  // The "or" operator sets the position to NaN in the translator.
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  // For kPointListAsTriangleStrip the bit means output coordinates (only the
  // Vulkan fallback uses that host type). Otherwise it means output point size.
  if (host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                     xenos::PrimitiveType::kPointList);
  }

  return modification.value;
}

namespace {
// log_early_z_reasons counters, indexed by EarlyZReason.
enum EarlyZReason : uint32_t {
  kEarlyZTaken,
  kEarlyZFloat24,
  kEarlyZPolygonOffset,
  kEarlyZShaderKill,
  kEarlyZShaderDepth,
  kEarlyZAlphaTest,
  kEarlyZAlphaToCoverage,
  kEarlyZOther,
  // Subset of the late reasons above moved to early tests because the draw
  // writes neither depth nor stencil (vulkan_early_tests_without_zs_writes).
  kEarlyZKillNoZsWrite,
  kEarlyZReasonCount,
};
std::atomic<uint64_t> early_z_reasons[kEarlyZReasonCount];

void NoteEarlyZReason(EarlyZReason reason) {
  if (cvars::log_early_z_reasons <= 0) {
    return;
  }
  early_z_reasons[reason].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<uint64_t> lookups{0};
  if ((lookups.fetch_add(1, std::memory_order_relaxed) + 1) %
          uint64_t(cvars::log_early_z_reasons) !=
      0) {
    return;
  }
  uint64_t c[kEarlyZReasonCount];
  for (uint32_t i = 0; i < kEarlyZReasonCount; ++i) {
    c[i] = early_z_reasons[i].load(std::memory_order_relaxed);
  }
  XELOGI(
      "[EARLYZ] early={} float24={} polygon_offset={} shader_kill={} "
      "shader_depth={} alpha_test={} alpha_to_coverage={} other={} "
      "of_which_early_no_zs_write={}",
      c[kEarlyZTaken], c[kEarlyZFloat24], c[kEarlyZPolygonOffset],
      c[kEarlyZShaderKill], c[kEarlyZShaderDepth], c[kEarlyZAlphaTest],
      c[kEarlyZAlphaToCoverage], c[kEarlyZOther], c[kEarlyZKillNoZsWrite]);
}

void NoteLateZReason(const Shader& shader, const RegisterFile& regs) {
  if (cvars::log_early_z_reasons <= 0) {
    return;
  }
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  if (shader.kills_pixels()) {
    NoteEarlyZReason(kEarlyZShaderKill);
  } else if (shader.writes_depth()) {
    NoteEarlyZReason(kEarlyZShaderDepth);
  } else if (colorcontrol.alpha_to_mask_enable) {
    NoteEarlyZReason(kEarlyZAlphaToCoverage);
  } else if (colorcontrol.alpha_test_enable &&
             colorcontrol.alpha_func != xenos::CompareFunction::kAlways) {
    NoteEarlyZReason(kEarlyZAlphaTest);
  } else {
    NoteEarlyZReason(kEarlyZOther);
  }
}

// Whether the draw can change the depth or the stencil buffer.
bool DrawWritesDepthStencil(const RegisterFile& regs,
                            reg::RB_DEPTHCONTROL depth_control) {
  if (depth_control.z_enable && depth_control.z_write_enable) {
    return true;
  }
  if (!depth_control.stencil_enable) {
    return false;
  }
  bool ops_write =
      depth_control.stencilfail != xenos::StencilOp::kKeep ||
      depth_control.stencilzfail != xenos::StencilOp::kKeep ||
      depth_control.stencilzpass != xenos::StencilOp::kKeep;
  uint32_t write_mask =
      regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask;
  if (depth_control.backface_enable) {
    ops_write = ops_write ||
                depth_control.stencilfail_bf != xenos::StencilOp::kKeep ||
                depth_control.stencilzfail_bf != xenos::StencilOp::kKeep ||
                depth_control.stencilzpass_bf != xenos::StencilOp::kKeep;
    write_mask |= regs.Get<reg::RB_STENCILREFMASK>(
                          XE_GPU_REG_RB_STENCILREFMASK_BF)
                      .stencilwritemask;
  }
  return ops_write && write_mask;
}
}  // namespace

uint64_t GuestSpirvShaderCache::GetPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool apply_polygon_offset_in_shader) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  SpirvShaderTranslator::Modification modification(
      translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  // Depth/stencil mode, blend pre-multiply and the color target mask are host
  // render target path state. In the FSI path the shader runs the EDRAM ROP
  // from the system constants, so these stay at their defaults.
  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Whether this draw is native res due to a scale threshold, or without
    // resolution scaling, whether it draws guest 4x samples as 2x2 pixels.
    modification.pixel.resolution_scale_native =
        uint32_t(render_target_cache_.IsDrawResolutionScaled()
                     ? render_target_cache_.IsDrawScaleNative()
                     : render_target_cache_.draw_samples_as_pixels());

    using DepthStencilMode =
        SpirvShaderTranslator::Modification::DepthStencilMode;
    if (host_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      NoteEarlyZReason(kEarlyZFloat24);
      modification.pixel.depth_stencil_mode =
          apply_polygon_offset_in_shader
              ? (host_.depth_float24_round()
                     ? DepthStencilMode::kFloat24RoundingPolygonOffset
                     : DepthStencilMode::kFloat24TruncatingPolygonOffset)
              : (host_.depth_float24_round()
                     ? DepthStencilMode::kFloat24Rounding
                     : DepthStencilMode::kFloat24Truncating);
    } else {
      if (apply_polygon_offset_in_shader) {
        NoteEarlyZReason(kEarlyZPolygonOffset);
        modification.pixel.depth_stencil_mode =
            DepthStencilMode::kPolygonOffset;
      } else {
        // kEarlyHint triggers a GPU fault on nvidia (Alan Wake gameplay), so
        // it's opt-in.
        if (cvars::vulkan_early_fragment_tests_hint &&
            shader.implicit_early_z_write_allowed() &&
            (!shader.writes_color_target(0) ||
             !draw_util::DoesCoverageDependOnAlpha(
                 regs.Get<reg::RB_COLORCONTROL>()))) {
          NoteEarlyZReason(kEarlyZTaken);
          modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
        } else {
          NoteLateZReason(shader, regs);
          // A 4x draw stored as 2x2 host pixels has one sample per host
          // pixel: without alpha to coverage it has no coverage to output,
          // and the shader may run once per guest pixel.
          if ((cvars::vulkan_alpha_test_no_sample_mask ||
               render_target_cache_.draw_samples_as_pixels()) &&
              !regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable) {
            modification.pixel.depth_stencil_mode =
                DepthStencilMode::kNoAlphaToCoverage;
          } else {
            modification.pixel.depth_stencil_mode =
                DepthStencilMode::kNoModifiers;
          }
          // The kill can't undo depth / stencil writes the draw doesn't make,
          // so the tests may run before the shader. The alpha test and alpha
          // to coverage stay in the shader (unlike kEarlyHint): with early
          // tests the coverage output still limits the color writes. Without
          // alpha to coverage the full-coverage output is dropped.
          if (cvars::vulkan_early_tests_without_zs_writes &&
              !shader.writes_depth() &&
              !modification.pixel.param_gen_enable &&
              modification.pixel.dynamic_addressable_register_count < 126 &&
              !DrawWritesDepthStencil(regs, normalized_depth_control)) {
            NoteEarlyZReason(kEarlyZKillNoZsWrite);
            if (!regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable) {
              modification.pixel.depth_stencil_mode =
                  DepthStencilMode::kNoAlphaToCoverage;
            }
            modification.pixel.param_gen_interpolator =
                SpirvShaderTranslator::Modification::
                    kEarlyTestsWithKillMarker;
          }
        }
      }
    }

    // MIN/MAX blend ignores fixed-function factors on the host, but the Xbox
    // 360 applies them. When the destination factor is ONE we pre-multiply the
    // shader output by the source factor. Only RT0 is supported for now.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;

    if (shader.writes_color_target(0)) {
      auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[0]);
      if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.color_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
      if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_a_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
    }

    modification.pixel.color_targets_used =
        (((normalized_color_mask >> 0) & 0xF) ? 1 : 0) |
        (((normalized_color_mask >> 4) & 0xF) ? 2 : 0) |
        (((normalized_color_mask >> 8) & 0xF) ? 4 : 0) |
        (((normalized_color_mask >> 12) & 0xF) ? 8 : 0);
  }

  // Manual barycentric interpolation for precision, where the host supports it
  // (D3D12 barycentrics). Vulkan leaves this at the default.
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto prim = regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type;
  if (cvars::vulkan_software_clip_planes && !translator_->features().clip_distance &&
      translator_->features().sample_rate_shading &&
      GetUserClipPlaneMask() && !clip.ucp_cull_only_ena &&
      !shader.uses_register_dynamic_addressing() &&
      (prim == xenos::PrimitiveType::kTriangleList ||
       prim == xenos::PrimitiveType::kTriangleStrip ||
       prim == xenos::PrimitiveType::kTriangleFan) &&
      xe::bit_count(interpolator_mask) <= 13 &&
      render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // Color draws have attachments, so host 2x sample layout is unambiguous.
    // Depth-only draws retain the proven sample-frequency fallback.
    modification.pixel.dynamic_addressable_register_count =
        cvars::vulkan_software_clip_pixel_mask && modification.pixel.color_targets_used
            ? 126 : 127;
  }

  modification.pixel.precise_interpolation =
      host_.precise_interpolation_supported() ? 1 : 0;

  return modification.value;
}

Shader::Translation* GuestSpirvShaderCache::EnsureTranslation(
    SpirvShader& shader, uint64_t modification) {
  if (!shader.is_ucode_analyzed()) {
    StringBuffer ucode_disasm;
    shader.AnalyzeUcode(ucode_disasm);
  }
  return shader.GetOrCreateTranslation(modification);
}

Shader::Translation* GuestSpirvShaderCache::TranslateSpirv(
    SpirvShaderTranslator& translator, Shader::Translation& translation,
    bool use_try_claim) {
  if (!translation.is_translated()) {
    bool should_translate = true;
    if (use_try_claim) {
      should_translate = translation.TryClaimTranslation();
      if (!should_translate) {
        // Another thread is translating this same modification - wait for it.
        while (!translation.is_translated()) {
          std::this_thread::yield();
        }
      }
    }
    if (should_translate && !translator.TranslateAnalyzedShader(translation)) {
      return nullptr;
    }
  }
  return translation.is_valid() ? &translation : nullptr;
}

Shader::Translation* GuestSpirvShaderCache::EnsureAndTranslate(
    SpirvShader& shader, uint64_t modification) {
  Shader::Translation* translation = EnsureTranslation(shader, modification);
  return TranslateSpirv(*translator_, *translation, /*use_try_claim=*/false);
}

bool GuestSpirvShaderCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    uint64_t vertex_shader_modification, uint64_t pixel_shader_modification,
    GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  SpirvShaderTranslator::Modification vertex_mod(vertex_shader_modification);
  SpirvShaderTranslator::Modification pixel_mod(pixel_shader_modification);
  // The *AsTriangleStrip host vertex shader types are the fallback for when
  // geometry shaders are unsupported. There geometry_shader_type is kNone and
  // this is not called. output_point_parameters means coordinates, not size,
  // for those, so reject them.
  if (vertex_mod.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      vertex_mod.vertex.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    XELOGE(
        "GetGeometryShaderKey: AsTriangleStrip vertex shader types should not "
        "be used with geometry shaders");
    return false;
  }
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = xe::bit_count(vertex_mod.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_mod.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_mod.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_mod.vertex.vertex_kill_and;
  key.has_point_size = vertex_mod.vertex.output_point_parameters;
  key.has_point_coordinates = pixel_mod.pixel.param_gen_point;
  key_out = key;
  return true;
}

}  // namespace gpu
}  // namespace xe
