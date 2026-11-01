/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_texture_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/ui_samplers.h"
#include "xenia/ui/vulkan/vulkan_mem_alloc.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(tiled_shared_memory);
DECLARE_bool(vulkan_pass_head_overlap);
DECLARE_bool(vulkan_resolve_texture_copy);
DECLARE_bool(vulkan_texture_from_render_target);
DECLARE_bool(vulkan_color_surface_resolve);
DECLARE_bool(log_image_compression);
DECLARE_int32(log_render_pass_breaks);

DEFINE_bool(
    vulkan_dxt1_to_astc, true,
    "Without BC texture support (Mali), transcode DXT1 textures to ASTC 4x4 "
    "(8 bits per texel) on load instead of decompressing them to RGBA8 (32 "
    "bits per texel). Opaque texels within 2/255 of the exact palette, alpha "
    "exact; punch-through transparent texels keep the darker endpoint's color "
    "instead of black.",
    "GPU");
UPDATE_from_bool(vulkan_dxt1_to_astc, 2026, 10, 6, 18, false);

DEFINE_bool(
    vulkan_dxt5_to_astc, true,
    "Without BC texture support (Mali), transcode DXT4/5 textures to ASTC 4x4 "
    "(8 bits per texel) on load instead of decompressing them to RGBA8 (32 "
    "bits per texel). Color within ~5/255; alpha exact in blocks with constant "
    "alpha, otherwise the nearest of 4 levels between the block's minimum and "
    "maximum alpha (BC3 has 6 or 8).",
    "GPU");
UPDATE_from_bool(vulkan_dxt5_to_astc, 2026, 10, 6, 18, false);

DEFINE_bool(
    vulkan_dxn_to_eac, true,
    "Without BC texture support (Mali), transcode DXN (BC5, normal maps) "
    "textures to EAC RG11 (8 bits per texel) on load instead of decompressing "
    "them to R8G8 (16 bits per texel). Mean error ~0.4/255 on smooth blocks.",
    "GPU");
UPDATE_from_bool(vulkan_dxn_to_eac, 2026, 10, 6, 18, false);

DEFINE_bool(
    vulkan_texture_load_persistent_source, true,
    "Texture loading: bind the whole shared memory buffer persistently as the "
    "load shader source (fork optimization). False uses the upstream per-load "
    "source descriptors (offset to the base / mip page).",
    "Vulkan");

DEFINE_int32(
    vulkan_max_mip_levels_below_min, -1,
    "Limit texture sampling to this many mip levels below the first allowed "
    "one (sampler maxLod). -1 = no limit.",
    "Vulkan");

DEFINE_int32(
    vulkan_debug_fixed_mip_level, -1,
    "Diagnostic: sample exactly this absolute mip level for textures whose "
    "first allowed level is zero and which have mips. Disables anisotropic "
    "and trilinear filtering for this comparison; -1 leaves behavior intact.",
    "Vulkan");

DEFINE_int32(
    vulkan_generate_texture_mips, 0,
    "Generate texture mips on the host by downsampling the base level instead "
    "of loading them from guest memory. Fixes titles whose guest mip data is "
    "never written (Modern Warfare 2 texture streaming: noisy/black textures "
    "at a distance). 0 = off, 1 = block-compressed textures decompressed on "
    "the host (DXT/DXN), 2 = all textures with a base level.",
    "Vulkan");

DEFINE_bool(
    vulkan_debug_texture_dump, false,
    "Diagnostic: record the first loads of block-compressed textures with mips "
    "(guest bytes, the shared memory bytes the load shader read and its "
    "decoded output) and write them next to the guest output capture.",
    "Vulkan");
DEFINE_bool(
    vulkan_debug_texture_audit, false,
    "Diagnostic: at the guest output capture, read back the host images of "
    "the textures sampled in the captured frame together with the guest bytes "
    "they come from, and whether those bytes changed since the load, and "
    "write them next to the capture (texaudit/).",
    "Vulkan");
DEFINE_int32(vulkan_debug_texture_audit_max_mb, 160,
             "Maximum host image readback size of vulkan_debug_texture_audit.",
             "Vulkan");
DEFINE_int32(vulkan_debug_texture_dump_max, 48,
             "Maximum number of textures recorded by vulkan_debug_texture_dump.",
             "Vulkan");
DEFINE_int32(vulkan_debug_texture_dump_min_size, 128,
             "Minimum width and height of textures recorded by "
             "vulkan_debug_texture_dump.",
             "Vulkan");

DEFINE_bool(
    vulkan_debug_force_base_mip, false,
    "Diagnostic: clamp every sampler to its first allowed mip level "
    "(maxLod = minLod + 0.25) to check whether smaller mips load correctly.",
    "Vulkan");

DEFINE_string(
    vulkan_debug_skip_texture_formats, "",
    "Diagnostic: comma-separated guest texture format indices (xenos::"
    "TextureFormat, e.g. 20 = k_DXT4_5) to leave unloaded (bound as null "
    "textures), to identify which format renders incorrectly. Empty = off.",
    "Vulkan");

DEFINE_bool(
    vulkan_fast_sampler_filterability, true,
    "Derive host linear-filterability for samplers directly from the fetch "
    "constant instead of building the full texture binding info (texture key "
    "+ host format pair) per sampler per draw. Results are identical; disable "
    "for A/B debugging of texture filtering issues.",
    "Vulkan");

DEFINE_bool(log_texture_clobber, false,
            "Diagnostic: log (up to 64) texture loads that read pages the GPU "
            "once wrote (memexport/resolve) but which were since invalidated "
            "by the CPU and are re-read from guest RAM ([TEXCLOB]).",
            "GPU");
DEFINE_uint32(texture_skip_top_mip_min_size, 256,
              "Block-compressed 2D/cube textures with guest mips whose larger "
              "side is at least this many texels are stored on the host from "
              "guest mip 1: half the width and height, a quarter of the "
              "memory and of the sampling bandwidth, the base level is never "
              "loaded. Resolve targets and host-generated mips are excluded. "
              "0 disables.",
              "GPU");

DEFINE_uint32(vulkan_texture_heap_block_mb, 64,
              "Preferred texture allocator block size in MiB (16..256). "
              "Zero preserves the VMA default. Does not change cache capacity, "
              "texture formats or resolution.", "Vulkan");
UPDATE_from_uint32(vulkan_texture_heap_block_mb, 2026, 10, 5, 12, 0);

DEFINE_uint32(log_inpass_texture_loads, 0,
              "Diagnostic: every N frames, log which textures were loaded "
              "while a render pass was open (each such load ends the pass), "
              "with the cause of their invalidation. 0 disables.",
              "GPU");

DEFINE_bool(log_inpass_texture_source_hash, false,
            "Diagnostic: hash texture source bytes for the in-pass texture "
            "load report. Disabled avoids the per-load XXH3 cost.",
            "GPU");

DEFINE_bool(vulkan_pass_head_mip_blits, true,
            "With vulkan_pass_head_texture_loads, also move loads whose mips "
            "are generated by blits (vulkan_generate_texture_mips) to the head "
            "of the render pass.",
            "GPU");
UPDATE_from_bool(vulkan_pass_head_mip_blits, 2026, 10, 24, 12, false);
DEFINE_bool(vulkan_pass_head_loads_ignore_reads, true,
            "With vulkan_pass_head_texture_loads, let a texture load move to "
            "the head of the render pass when the pass only read its pages "
            "(changed contents still keep it in place), instead of ending the "
            "pass for its barrier.",
            "GPU");
UPDATE_from_bool(vulkan_pass_head_loads_ignore_reads, 2026, 10, 24, 12, false);
DEFINE_int32(log_pass_head_texture_rejects, 0,
             "Every N texture data requests, log why texture loads were not "
             "moved to the head of the current render pass (0 = off).",
             "GPU");
DEFINE_bool(vulkan_resolve_texture_copy_covering, true,
            "With vulkan_resolve_texture_copy, also fill a texture from a "
            "resolve whose 8-pixel aligned rectangle covers it and more "
            "(a 320x184 resolve of a 320x180 texture) instead of reloading "
            "it from guest memory.",
            "GPU");
DEFINE_bool(vulkan_resolve_texture_copy_partial, true,
            "With vulkan_resolve_texture_copy, also fill the part of a color "
            "texture a resolve writes when the resolve covers only a part of "
            "it (an offset or a base inside the texture), as long as the rest "
            "of the texture is up to date, instead of reloading the whole "
            "texture from guest memory.",
            "GPU");
DEFINE_bool(vulkan_resolve_texture_copy_norm16, false,
            "With vulkan_resolve_texture_copy, also fill k_16_16 and "
            "k_16_16_16_16 textures that the device samples as half floats "
            "(no filterable norm16, as on Mali) straight from the resolve, "
            "converting in the resolve shader exactly like their loader, "
            "instead of reloading them from guest memory.",
            "GPU");
DEFINE_uint32(log_resolve_texture_copy_rejects, 0,
              "Diagnostic: every this many resolves, log why the textures at "
              "their destinations were not filled straight from the resolve "
              "(vulkan_resolve_texture_copy) and get reloaded from guest "
              "memory instead. 0 disables.",
              "GPU");
DEFINE_bool(vulkan_pass_head_texture_loads, true,
            "Record a texture load needed inside an open render pass at the "
            "head of that pass (before its first draw) instead of ending the "
            "pass for it, when no draw of the pass may have sampled the "
            "texture and no command of the pass touched its guest memory. "
            "Each avoided break saves a tile store and reload.",
            "Vulkan");
UPDATE_from_bool(vulkan_pass_head_texture_loads, 2026, 10, 5, 12, false);

DECLARE_bool(log_depth_materialize_callers);

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_128bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_128bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_16bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_16bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_32bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_32bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_64bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_64bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_8bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_8bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_bgrg8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_ctx1_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_depth_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_depth_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_depth_unorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxn_eac_rg11_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxn_rg8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt1_astc4x4_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt1_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt3_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt3a_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt3aas1111_argb4_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt5_astc4x4_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt5_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_dxt5a_r8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_gbgr8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r4g4b4a4_a4r4g4b4_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g6b5_b5g6r5_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rg16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rg16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rg16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rg16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rgba16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rgba16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rgba16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/texture_load_rgba16_unorm_float_scaled_cs.h"
}  // namespace shaders

static_assert(VK_FORMAT_UNDEFINED == VkFormat(0),
              "Assuming that skipping a VkFormat in an initializer results in "
              "VK_FORMAT_UNDEFINED");
constexpr VulkanTextureCache::HostFormatPair
    VulkanTextureCache::kBestHostFormats[64] = {
        // k_1_REVERSE
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_1
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_8
        {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
         {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_1_5_5_5
        // Red and blue swapped in the load shader for simplicity.
        {{kLoadShaderIndexR5G5B5A1ToB5G5R5A1, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_5_6_5
        // Red and blue swapped in the load shader for simplicity.
        {{kLoadShaderIndexR5G6B5ToB5G6R5, VK_FORMAT_R5G6B5_UNORM_PACK16},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_6_5_5
        // On the host, green bits in blue, blue bits in green.
        {{kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
          VK_FORMAT_R5G6B5_UNORM_PACK16},
         {kLoadShaderIndexUnknown},
         XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
        // k_8_8_8_8
        {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
         {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_2_10_10_10
        // VK_FORMAT_A2B10G10R10_SNORM_PACK32 is optional.
        {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
         {kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_8_A
        {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
         {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_8_B
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_8_8
        {{kLoadShaderIndex16bpb, VK_FORMAT_R8G8_UNORM},
         {kLoadShaderIndex16bpb, VK_FORMAT_R8G8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
         true},
        // k_Cr_Y1_Cb_Y0_REP
        // VK_FORMAT_G8B8G8R8_422_UNORM (added in
        // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
        // optional.
        {{kLoadShaderIndex32bpb, VK_FORMAT_G8B8G8R8_422_UNORM, true},
         {kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_Y1_Cr_Y0_Cb_REP
        // VK_FORMAT_B8G8R8G8_422_UNORM (added in
        // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
        // optional.
        {{kLoadShaderIndex32bpb, VK_FORMAT_B8G8R8G8_422_UNORM, true},
         {kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_16_16_EDRAM
        // Not usable as a texture, also has -32...32 range.
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_8_8_8_8_A
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_4_4_4_4
        // Components swapped in the load shader for simplicity.
        {{kLoadShaderIndexRGBA4ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_10_11_11
        // TODO(Triang3l): 16_UNORM/SNORM are optional, convert to float16
        // instead.
        {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
         {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_11_11_10
        // TODO(Triang3l): 16_UNORM/SNORM are optional, convert to float16
        // instead.
        {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
         {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_DXT1
        // VK_FORMAT_BC1_RGBA_UNORM_BLOCK is optional.
        {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_DXT2_3
        // VK_FORMAT_BC2_UNORM_BLOCK is optional.
        {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_DXT4_5
        // VK_FORMAT_BC3_UNORM_BLOCK is optional.
        {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_16_16_16_16_EDRAM
        // Not usable as a texture, also has -32...32 range.
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_24_8
        {{kLoadShaderIndexDepthUnorm, VK_FORMAT_R32_SFLOAT},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_24_8_FLOAT
        {{kLoadShaderIndexDepthFloat, VK_FORMAT_R32_SFLOAT},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_16
        // VK_FORMAT_R16_UNORM and VK_FORMAT_R16_SNORM are optional.
        {{kLoadShaderIndex16bpb, VK_FORMAT_R16_UNORM},
         {kLoadShaderIndex16bpb, VK_FORMAT_R16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_16_16
        // VK_FORMAT_R16G16_UNORM and VK_FORMAT_R16G16_SNORM are optional.
        {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_UNORM},
         {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
         true},
        // k_16_16_16_16
        // VK_FORMAT_R16G16B16A16_UNORM and VK_FORMAT_R16G16B16A16_SNORM are
        // optional.
        {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_UNORM},
         {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_16_EXPAND
        {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
         {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_16_16_EXPAND
        {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
         {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
         true},
        // k_16_16_16_16_EXPAND
        {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
         {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_16_FLOAT
        {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
         {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_16_16_FLOAT
        {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
         {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
         true},
        // k_16_16_16_16_FLOAT
        {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
         {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_32
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_32_32
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_32_32_32_32
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_32_FLOAT
        {{kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
         {kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
         true},
        // k_32_32_FLOAT
        {{kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
         {kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
         true},
        // k_32_32_32_32_FLOAT
        {{kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
         {kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_32_AS_8
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_32_AS_8_8
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_16_MPEG
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_16_16_MPEG
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_8_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_32_AS_8_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_32_AS_8_8_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_16_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_16_MPEG_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_16_16_MPEG_INTERLACED
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_DXN
        // VK_FORMAT_BC5_UNORM_BLOCK is optional.
        {{kLoadShaderIndex128bpb, VK_FORMAT_BC5_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_8_8_8_8_AS_16_16_16_16
        {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
         {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_DXT1_AS_16_16_16_16
        // VK_FORMAT_BC1_RGBA_UNORM_BLOCK is optional.
        {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_DXT2_3_AS_16_16_16_16
        // VK_FORMAT_BC2_UNORM_BLOCK is optional.
        {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_DXT4_5_AS_16_16_16_16
        // VK_FORMAT_BC3_UNORM_BLOCK is optional.
        {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_2_10_10_10_AS_16_16_16_16
        // VK_FORMAT_A2B10G10R10_SNORM_PACK32 is optional.
        {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
         {kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_SNORM_PACK32},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
         true},
        // k_10_11_11_AS_16_16_16_16
        // TODO(Triang3l): 16_UNORM/SNORM are optional, convert to float16
        // instead.
        {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
         {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_11_11_10_AS_16_16_16_16
        // TODO(Triang3l): 16_UNORM/SNORM are optional, convert to float16
        // instead.
        {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
         {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_32_32_32_FLOAT
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
        // k_DXT3A
        {{kLoadShaderIndexDXT3A, VK_FORMAT_R8_UNORM},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_DXT5A
        // VK_FORMAT_BC4_UNORM_BLOCK is optional.
        {{kLoadShaderIndex64bpb, VK_FORMAT_BC4_UNORM_BLOCK, true},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
        // k_CTX1
        {{kLoadShaderIndexCTX1, VK_FORMAT_R8G8_UNORM},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
        // k_DXT3A_AS_1_1_1_1
        {{kLoadShaderIndexDXT3AAs1111ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_8_8_8_8_GAMMA_EDRAM
        // Not usable as a texture.
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
        // k_2_10_10_10_FLOAT_EDRAM
        // Not usable as a texture.
        {{kLoadShaderIndexUnknown},
         {kLoadShaderIndexUnknown},
         xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

// Vulkan requires 2x1 (4:2:2) subsampled images to have an even width.
// Always decompressing them to RGBA8, which is required to be linear-filterable
// as UNORM and SNORM.

constexpr VulkanTextureCache::HostFormatPair
    VulkanTextureCache::kHostFormatGBGRUnaligned = {
        {kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
        {kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_SNORM, false, true},
        xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
        true};

constexpr VulkanTextureCache::HostFormatPair
    VulkanTextureCache::kHostFormatBGRGUnaligned = {
        {kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
        {kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_SNORM, false, true},
        xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
        true};

VulkanTextureCache::~VulkanTextureCache() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  for (const std::pair<const SamplerParameters, Sampler>& sampler_pair :
       samplers_) {
    command_processor_.ReleaseBindlessSampler(sampler_pair.second.sampler);
    dfn.vkDestroySampler(device, sampler_pair.second.sampler, nullptr);
  }
  ++sampler_destroy_generation_;
  samplers_.clear();
  COUNT_profile_set("gpu/texture_cache/vulkan/samplers", 0);
  sampler_used_last_ = nullptr;
  sampler_used_first_ = nullptr;

  if (null_image_view_3d_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_3d_, nullptr);
  }
  if (null_image_view_cube_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_cube_, nullptr);
  }
  if (null_image_view_2d_array_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_2d_array_, nullptr);
  }
  if (null_image_3d_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, null_image_3d_, nullptr);
  }
  if (null_image_2d_array_cube_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, null_image_2d_array_cube_, nullptr);
  }
  for (VkDeviceMemory null_images_memory : null_images_memory_) {
    if (null_images_memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, null_images_memory, nullptr);
    }
  }
  for (VkPipeline load_pipeline : load_pipelines_scaled_) {
    if (load_pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, load_pipeline, nullptr);
    }
  }
  for (VkPipeline load_pipeline : load_pipelines_) {
    if (load_pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, load_pipeline, nullptr);
    }
  }
  if (load_pipeline_layout_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineLayout(device, load_pipeline_layout_, nullptr);
  }
  if (shared_memory_persistent_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(
        device, shared_memory_persistent_descriptor_pool_, nullptr);
  }

  // Textures memory is allocated using the Vulkan Memory Allocator, destroy all
  // textures before destroying VMA.
  DestroyAllTextures(true);

  // Clean up sparse scaled resolve resources (heaps and sparse buffers)
  ShutdownSparseScaledResolve();

  // Clean up simple scaled resolve buffers before destroying VMA
  // The command processor should ensure all GPU operations are complete
  // before the texture cache is destroyed
  for (ScaledResolveBuffer& buffer : scaled_resolve_buffers_) {
    if (buffer.buffer != VK_NULL_HANDLE) {
      vmaDestroyBuffer(vma_allocator_, buffer.buffer, buffer.allocation);
    }
  }
  scaled_resolve_buffers_.clear();
  for (auto& retired : scaled_resolve_retired_buffers_) {
    vmaDestroyBuffer(vma_allocator_, retired.second.buffer,
                     retired.second.allocation);
  }
  scaled_resolve_retired_buffers_.clear();

  if (vma_allocator_ != VK_NULL_HANDLE) {
    vmaDestroyAllocator(vma_allocator_);
  }
}

void VulkanTextureCache::BeginSubmission(uint64_t new_submission_index) {
  if (!scaled_resolve_retired_buffers_.empty()) {
    ReleaseRetiredScaledResolveBuffers(
        command_processor_.GetCompletedSubmission());
  }
  TextureCache::BeginSubmission(new_submission_index);

  if (!null_images_cleared_) {
    VkImage null_images[] = {null_image_2d_array_cube_, null_image_3d_};
    VkImageSubresourceRange null_image_subresource_range(
        ui::vulkan::util::InitializeSubresourceRange());
    for (size_t i = 0; i < xe::countof(null_images); ++i) {
      command_processor_.PushImageMemoryBarrier(
          null_images[i], null_image_subresource_range, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    }
  command_processor_.SubmitBarriers(true);
    DeferredCommandBuffer& command_buffer =
        command_processor_.deferred_command_buffer();
    // TODO(Triang3l): Find the return value for invalid texture fetch constants
    // on the real hardware.
    VkClearColorValue null_image_clear_color;
    null_image_clear_color.float32[0] = 0.0f;
    null_image_clear_color.float32[1] = 0.0f;
    null_image_clear_color.float32[2] = 0.0f;
    null_image_clear_color.float32[3] = 0.0f;
    for (size_t i = 0; i < xe::countof(null_images); ++i) {
      command_buffer.CmdVkClearColorImage(
          null_images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          &null_image_clear_color, 1, &null_image_subresource_range);
    }
    for (size_t i = 0; i < xe::countof(null_images); ++i) {
      command_processor_.PushImageMemoryBarrier(
          null_images[i], null_image_subresource_range,
          VK_PIPELINE_STAGE_TRANSFER_BIT, guest_shader_pipeline_stages_,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
    }
    null_images_cleared_ = true;
  }
}

void VulkanTextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  // A synchronous provider may drain in the middle of another draw's input
  // preparation; its already selected texture bindings remain live until the
  // guest draw is recorded. Normal completion will run eviction next time.
  if (command_processor_.shared_memory_->depth_materializing()) return;
  TextureCache::CompletedSubmissionUpdated(completed_submission_index);
}

VkPipelineStageFlags VulkanTextureCache::GetGuestSampledStages(
    bool vertex_stages) const {
  return (vertex_stages || !cvars::vulkan_pass_head_overlap)
             ? guest_shader_pipeline_stages_
             : VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void VulkanTextureCache::EnsureTexturesSampled(uint32_t used_texture_mask,
                                              uint32_t vertex_texture_mask) {
  uint32_t index;
  while (xe::bit_scan_forward(used_texture_mask, &index)) {
    used_texture_mask &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    const bool vertex_stages = (vertex_texture_mask >> index) & 1;
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (!texture) {
        continue;
      }
      auto& vulkan_texture = *static_cast<VulkanTexture*>(texture);
      // Left in another usage since the draw that requested it (a resolve,
      // load or depth surface pass that didn't end the cached state).
      if (vulkan_texture.usage() != VulkanTexture::Usage::kGuestShaderSampled ||
          (vertex_stages && !vulkan_texture.vertex_visible)) {
        static uint32_t logged = 0;
        if (logged < 32) {
          ++logged;
          XELOGI(
              "EnsureTexturesSampled: cached draw found texture {:08X} fmt {} "
              "{}x{} in usage {} (vertex {})",
              vulkan_texture.key().base_page << 12,
              uint32_t(vulkan_texture.key().format),
              vulkan_texture.key().GetWidth(), vulkan_texture.key().GetHeight(),
              uint32_t(vulkan_texture.usage()), uint32_t(vertex_stages));
        }
        TransitionTextureForGuestShader(vulkan_texture, vertex_stages);
      }
    }
  }
}

void VulkanTextureCache::TransitionTextureForGuestShader(
    VulkanTexture& texture, bool vertex_stages) {
  texture.MarkAsUsed();
  if (texture.depth_surface_pending_sample) {
    texture.depth_surface_pending_sample = false;
    if (!texture.base_outdated_lockless() && !texture.mips_outdated_lockless())
      command_processor_.render_target_cache_->RecordDepthSurfaceTextureHit(
          uint64_t(texture.key().GetWidth()) * texture.key().GetHeight() * 4);
  }
  if (texture.resolve_copy_pending_sample) {
    texture.resolve_copy_pending_sample = false;
    command_processor_.RecordResolveTextureHit(uint32_t(texture.key().format));
  }
  VulkanTexture::Usage new_usage = VulkanTexture::Usage::kGuestShaderSampled;
  VkPipelineStageFlags dst_stage_mask;
  VkAccessFlags dst_access_mask;
  VkImageLayout new_layout;
  GetTextureUsageMasks(new_usage, dst_stage_mask, dst_access_mask, new_layout);
  VulkanTexture::Usage old_usage = texture.SetUsage(new_usage);
  if (old_usage != new_usage) {
    VkPipelineStageFlags src_stage_mask;
    VkAccessFlags src_access_mask;
    VkImageLayout old_layout;
    GetTextureUsageMasks(old_usage, src_stage_mask, src_access_mask,
                         old_layout);
    dst_stage_mask = GetGuestSampledStages(vertex_stages);
    texture.vertex_visible =
        dst_stage_mask == guest_shader_pipeline_stages_;
    command_processor_.PushImageMemoryBarrier(
        texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
        src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        old_layout, new_layout);
  } else if (vertex_stages && !texture.vertex_visible) {
    // Sampled by pixel shaders so far: the write already reached the fragment
    // stage (and was made available), the chain carries it on to the vertex
    // stages.
    command_processor_.PushImageMemoryBarrier(
        texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, guest_shader_pipeline_stages_,
        0, dst_access_mask, new_layout, new_layout, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
    texture.vertex_visible = true;
  }
}

void VulkanTextureCache::FlushEagerResolvedReloads() {
  TakeEagerReloads(eager_reload_scratch_);
  for (Texture* texture : eager_reload_scratch_) {
    auto& vulkan_texture = *static_cast<VulkanTexture*>(texture);
    if (!texture->base_outdated_lockless() &&
        !texture->mips_outdated_lockless()) {
      continue;
    }
    if (LoadTextureData(*texture)) {
      TransitionTextureForGuestShader(vulkan_texture, false);
    }
  }
  eager_reload_scratch_.clear();
}

bool VulkanTextureCache::ResolveDeferTextureRead(
    uint32_t used_texture_mask, uint32_t address, uint32_t length,
    bool& unknown) const {
  auto overlaps = [address, length](uint32_t start, uint64_t size) {
    constexpr uint32_t mask = SharedMemory::kBufferSize - 1;
    return length && size &&
           (size >= SharedMemory::kBufferSize ||
            ((start - address) & mask) < length ||
            ((address - start) & mask) < size);
  };
  uint32_t slot;
  while (xe::bit_scan_forward(used_texture_mask, &slot)) {
    used_texture_mask = xe::clear_lowest_bit(used_texture_mask);
    TextureKey key;
    BindingInfoFromFetchConstant(register_file().GetTextureFetch(slot), key,
                                 nullptr);
    if (!key.is_valid) {
      unknown = true;
      continue;
    }
    const auto layout = key.GetGuestLayout();
    if ((key.base_page &&
         overlaps(key.base_page << 12, layout.base.level_data_extent_bytes)) ||
        (key.mip_page &&
         overlaps(key.mip_page << 12, layout.mips_total_extent_bytes))) {
      return true;
    }
  }
  return false;
}

void VulkanTextureCache::RequestTextures(uint32_t used_texture_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  TextureCache::RequestTextures(used_texture_mask);

  // Transition the textures into the needed usage.
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (xe::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    const bool vertex_stages = (vertex_texture_mask_ >> index) & 1;
    if (binding->texture != nullptr) {
      TransitionTextureForGuestShader(
          *static_cast<VulkanTexture*>(binding->texture), vertex_stages);
    }
    if (binding->texture_signed != nullptr) {
      TransitionTextureForGuestShader(
          *static_cast<VulkanTexture*>(binding->texture_signed),
          vertex_stages);
    }
  }
}

VkImageLayout VulkanTextureCache::GetActiveBindingImageLayout(
    uint32_t fetch_constant_index, xenos::FetchOpDimension dimension,
    bool is_signed) const {
  return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageView VulkanTextureCache::GetActiveBindingOrNullImageView(
    uint32_t fetch_constant_index, xenos::FetchOpDimension dimension,
    bool is_signed) {
  VkImageView image_view = VK_NULL_HANDLE;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(dimension, binding->key.dimension)) {
    // Check for 3D texture sampled as 2D.
    bool force_special_view =
        (dimension == xenos::FetchOpDimension::k2D &&
         binding->key.dimension == xenos::DataDimension::k3D);

    if (force_special_view) {
      // Get the appropriate texture for signed/unsigned.
      // Respect swizzled_signs from fetch constant, not just shader request.
      Texture* texture = nullptr;
      bool use_signed =
          is_signed && texture_util::IsAnySignSigned(binding->swizzled_signs);
      if (use_signed && IsSignedVersionSeparateForFormat(binding->key)) {
        texture = binding->texture_signed;
      } else {
        texture = binding->texture;
      }
      if (texture) {
        image_view =
            static_cast<VulkanTexture*>(texture)->GetOrCreate3DAs2DImageView(
                use_signed, binding->host_swizzle);
      }
    } else {
      const VulkanTextureBinding& vulkan_binding =
          vulkan_texture_bindings_[fetch_constant_index];
      image_view = is_signed ? vulkan_binding.image_view_signed
                             : vulkan_binding.image_view_unsigned;
    }
  }
  if (image_view != VK_NULL_HANDLE) {
    return image_view;
  }
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return null_image_view_3d_;
    case xenos::FetchOpDimension::kCube:
      return null_image_view_cube_;
    default:
      return null_image_view_2d_array_;
  }
}

VulkanTextureCache::SamplerParameters VulkanTextureCache::GetSamplerParameters(
    const VulkanShader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch =
      regs.GetTextureFetch(binding.fetch_constant);

  uint32_t subres_width_minus_1, subres_base_page, subres_mip_page;
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &subres_width_minus_1, nullptr, nullptr, &subres_base_page,
      &subres_mip_page, &mip_min_level, &mip_max_level);
  const uint32_t binding_key = uint32_t(binding.fetch_constant) |
                               (uint32_t(binding.mag_filter) << 5) |
                               (uint32_t(binding.min_filter) << 7) |
                               (uint32_t(binding.mip_filter) << 9) |
                               (uint32_t(binding.aniso_filter) << 11);
  auto& memo = sampler_parameter_memos_[((binding_key ^ subres_base_page) *
                                         UINT32_C(0x9E3779B1)) >>
                                        23];
  const int32_t aniso_override = cvars::anisotropic_override;
  const int32_t aniso_cap = cvars::anisotropic_cap;
  const uint32_t options =
      uint32_t(cvars::vulkan_fast_sampler_filterability) |
      (uint32_t(cvars::gpu_allow_invalid_fetch_constants) << 1);
  const bool memoizable = !mip_min_level && !HasPendingDebugTextureDumps();
  if (memoizable && memo.binding == binding_key &&
      memo.aniso_override == aniso_override && memo.aniso_cap == aniso_cap &&
      memo.options == options &&
      !std::memcmp(&memo.fetch, &fetch, sizeof(fetch))) {
    return memo.parameters;
  }
  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y,
                                          fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
    parameters.force_bc_w_to_max = fetch.force_bc_w_to_max;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  xenos::TextureFilter mag_filter =
      binding.mag_filter == xenos::TextureFilter::kUseFetchConst
          ? fetch.mag_filter
          : binding.mag_filter;
  parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter min_filter =
      binding.min_filter == xenos::TextureFilter::kUseFetchConst
          ? fetch.min_filter
          : binding.min_filter;
  parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter mip_filter =
      binding.mip_filter == xenos::TextureFilter::kUseFetchConst
          ? fetch.mip_filter
          : binding.mip_filter;
  parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;

  xenos::AnisoFilter aniso_filter =
      binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
          ? fetch.aniso_filter
          : binding.aniso_filter;
  parameters.mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  parameters.mip_min_level = mip_min_level;
  if (parameters.mip_min_level) {
    TextureKey texture_key;
    BindingInfoFromFetchConstant(fetch, texture_key, nullptr);
    if (texture_key.is_valid && SkipsTopMip(texture_key)) {
      --parameters.mip_min_level;
    }
  }
  bool has_mips = mip_max_level > mip_min_level;
  if (!debug_texture_dumps_.empty() && !debug_texture_dumps_written_) {
    // Diagnostic: remember how the dumped textures are actually sampled.
    for (DebugTextureDump& dump : debug_texture_dumps_) {
      if (dump.key.base_page != subres_base_page ||
          dump.key.mip_page != subres_mip_page) {
        continue;
      }
      ++dump.sampler_uses;
      std::array<uint32_t, 10> variant;
      std::memcpy(variant.data(), &fetch, sizeof(uint32_t) * 6);
      variant[6] = uint32_t(binding.mip_filter) |
                   (uint32_t(binding.min_filter) << 4) |
                   (uint32_t(binding.mag_filter) << 8) |
                   (uint32_t(binding.aniso_filter) << 12);
      variant[7] = mip_min_level | (mip_max_level << 8) |
                   (uint32_t(parameters.mip_base_map) << 16);
      variant[8] = binding.fetch_constant;
      variant[9] = 0;
      if (dump.sampler_variants.size() < 8 &&
          std::find(dump.sampler_variants.begin(), dump.sampler_variants.end(),
                    variant) == dump.sampler_variants.end()) {
        dump.sampler_variants.push_back(variant);
      }
    }
  }
  // Apply anisotropic override, but only for mipmapped textures
  // that are already using bilinear/trilinear filtering.
  if (cvars::anisotropic_override > -1 && cvars::anisotropic_override < 6 &&
      has_mips && !parameters.mip_base_map && parameters.mag_linear &&
      parameters.min_linear &&
      (mip_filter == xenos::TextureFilter::kPoint ||
       mip_filter == xenos::TextureFilter::kLinear)) {
    aniso_filter = xenos::AnisoFilter(cvars::anisotropic_override);
  } else if (cvars::anisotropic_cap >= int32_t(xenos::AnisoFilter::kMax_2_1) &&
             cvars::anisotropic_cap < int32_t(xenos::AnisoFilter::kMax_16_1)) {
    aniso_filter =
        std::min(aniso_filter, xenos::AnisoFilter(cvars::anisotropic_cap));
  }
  parameters.aniso_filter = std::min(aniso_filter, max_anisotropy_);

  // Fall back to point sampling for formats the device can't linearly filter.
  // Anisotropy implies linear filtering, so disable it on those too.
  if (parameters.mag_linear || parameters.min_linear || parameters.mip_linear ||
      parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    bool linear_filterable;
    if (cvars::vulkan_fast_sampler_filterability) {
      // Mirrors the validation in TextureCache::BindingInfoFromFetchConstant
      // and the host format selection in GetHostFormatPair without building
      // the full TextureKey - filterability depends only on the fetch
      // constant's validity, the base format, the width parity (for the
      // packed 4:2:2 formats) and the swizzled signs. The diagnostic logging
      // of the full path is intentionally skipped here; flip the cvar off to
      // get it back.
      linear_filterable = false;
      bool fetch_type_valid =
          fetch.type == xenos::FetchConstantType::kTexture ||
          (fetch.type == xenos::FetchConstantType::kInvalidTexture &&
           cvars::gpu_allow_invalid_fetch_constants);
      // A 1D texture wider than 8192 is valid, laid out in rows of 8192
      // (BindingInfoFromFetchConstant), so its width is even.
      const bool wide_1d =
          fetch.dimension == xenos::DataDimension::k1D &&
          subres_width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight;
      if (fetch_type_valid &&
          (subres_base_page != 0 || subres_mip_page != 0) &&
          !(fetch.dimension == xenos::DataDimension::k1D &&
            (fetch.tiled || fetch.packed_mips))) {
        xenos::TextureFormat format = GetBaseFormat(fetch.format);
        bool width_odd = !wide_1d && ((subres_width_minus_1 + 1) & 1) != 0;
        const HostFormatPair& host_format_pair =
            (format == xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP && width_odd)
                ? kHostFormatGBGRUnaligned
                : ((format == xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP &&
                    width_odd)
                       ? kHostFormatBGRGUnaligned
                       : host_formats_[uint32_t(format)]);
        uint8_t texture_swizzled_signs = texture_util::SwizzleSigns(fetch);
        linear_filterable =
            !((texture_util::IsAnySignNotSigned(texture_swizzled_signs) &&
               !host_format_pair.format_unsigned.linear_filterable) ||
              (texture_util::IsAnySignSigned(texture_swizzled_signs) &&
               !host_format_pair.format_signed.linear_filterable));
      }
    } else {
      linear_filterable = true;
      TextureKey texture_key;
      uint8_t texture_swizzled_signs;
      BindingInfoFromFetchConstant(fetch, texture_key, &texture_swizzled_signs);
      if (texture_key.is_valid) {
        const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
        if ((texture_util::IsAnySignNotSigned(texture_swizzled_signs) &&
             !host_format_pair.format_unsigned.linear_filterable) ||
            (texture_util::IsAnySignSigned(texture_swizzled_signs) &&
             !host_format_pair.format_signed.linear_filterable)) {
          linear_filterable = false;
        }
      } else {
        linear_filterable = false;
      }
    }
    if (!linear_filterable) {
      parameters.mag_linear = 0;
      parameters.min_linear = 0;
      parameters.mip_linear = 0;
      parameters.aniso_filter = xenos::AnisoFilter::kDisabled;
    }
  }

  if (memoizable) {
    memo.binding = binding_key;
    memo.fetch = fetch;
    memo.aniso_override = aniso_override;
    memo.aniso_cap = aniso_cap;
    memo.options = options;
    memo.parameters = parameters;
  }
  return parameters;
}

VkSampler VulkanTextureCache::UseSampler(SamplerParameters parameters,
                                         bool& has_overflown_out) {
  assert_true(command_processor_.submission_open());
  uint64_t submission_current = command_processor_.GetCurrentSubmission();

  auto& lookup =
      sampler_lookup_[(parameters.value * UINT32_C(0x9E3779B1)) >> 24];
  std::pair<const SamplerParameters, Sampler>* node = nullptr;
  if (lookup.generation == sampler_destroy_generation_ &&
      lookup.parameters == parameters) {
    node = lookup.node;
  } else {
    auto it = samplers_.find(parameters);
    if (it != samplers_.end()) {
      node = &*it;
      lookup = {sampler_destroy_generation_, parameters, node};
    }
  }
  if (node) {
    std::pair<const SamplerParameters, Sampler>& sampler = *node;
    assert_true(sampler.second.last_usage_submission <= submission_current);
    // This is called very frequently, don't relink unless needed for caching.
    if (sampler.second.last_usage_submission < submission_current) {
      // Move to the front of the LRU queue.
      sampler.second.last_usage_submission = submission_current;
      if (sampler.second.used_next) {
        if (sampler.second.used_previous) {
          sampler.second.used_previous->second.used_next =
              sampler.second.used_next;
        } else {
          sampler_used_first_ = sampler.second.used_next;
        }
        sampler.second.used_next->second.used_previous =
            sampler.second.used_previous;
        sampler.second.used_previous = sampler_used_last_;
        sampler.second.used_next = nullptr;
        sampler_used_last_->second.used_next = &sampler;
        sampler_used_last_ = &sampler;
      }
    }
    has_overflown_out = false;
    return sampler.second.sampler;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // See if an existing sampler can be destroyed to create space for the new
  // one.
  if (samplers_.size() >= sampler_max_count_) {
    assert_not_null(sampler_used_first_);
    if (!sampler_used_first_) {
      has_overflown_out = false;
      return VK_NULL_HANDLE;
    }
    if (sampler_used_first_->second.last_usage_submission >
        command_processor_.GetCompletedSubmission()) {
      has_overflown_out = true;
      return VK_NULL_HANDLE;
    }
    auto it_reuse = samplers_.find(sampler_used_first_->first);
    // The command processor's cross-draw sampler cache relies on this
    // invariant: a sampler whose last usage is in the still-open current
    // submission can never reach this point, because the completed submission
    // index is always behind the open one (the eviction predicate above
    // requires last_usage_submission <= completed).
    assert_true(sampler_used_first_->second.last_usage_submission <
                command_processor_.GetCurrentSubmission());
    command_processor_.ReleaseBindlessSampler(
        sampler_used_first_->second.sampler);
    dfn.vkDestroySampler(device, sampler_used_first_->second.sampler, nullptr);
    ++sampler_destroy_generation_;
    if (sampler_used_first_->second.used_next) {
      sampler_used_first_->second.used_next->second.used_previous =
          sampler_used_first_->second.used_previous;
    } else {
      sampler_used_last_ = sampler_used_first_->second.used_previous;
    }
    sampler_used_first_ = sampler_used_first_->second.used_next;
    assert_true(it_reuse != samplers_.end());
    if (it_reuse != samplers_.end()) {
      // This destroys the Sampler object.
      samplers_.erase(it_reuse);
      COUNT_profile_set("gpu/texture_cache/vulkan/samplers", samplers_.size());
    } else {
      has_overflown_out = false;
      return VK_NULL_HANDLE;
    }
  }

  // Create a new sampler and make it the least recently used.
  // The values are normalized, and unsupported ones are excluded, in
  // GetSamplerParameters.
  VkSamplerCreateInfo sampler_create_info = {};
  sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  if (vulkan_device->properties().nonSeamlessCubeMap &&
      cvars::non_seamless_cube_map) {
    sampler_create_info.flags |=
        VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
  }
  sampler_create_info.magFilter =
      parameters.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_create_info.minFilter =
      parameters.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_create_info.mipmapMode = parameters.mip_linear
                                       ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  static constexpr VkSamplerAddressMode kAddressModeMap[] = {
      // kRepeat
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      // kMirroredRepeat
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
      // kClampToEdge
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      // kMirrorClampToEdge
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      // kClampToHalfway
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      // kMirrorClampToHalfway
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      // kClampToBorder
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      // kMirrorClampToBorder
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
  };
  sampler_create_info.addressModeU =
      kAddressModeMap[uint32_t(parameters.clamp_x)];
  sampler_create_info.addressModeV =
      kAddressModeMap[uint32_t(parameters.clamp_y)];
  sampler_create_info.addressModeW =
      kAddressModeMap[uint32_t(parameters.clamp_z)];
  // LOD biasing is performed in shaders.
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    sampler_create_info.anisotropyEnable = VK_TRUE;
    sampler_create_info.maxAnisotropy =
        float(UINT32_C(1) << (uint32_t(parameters.aniso_filter) -
                              uint32_t(xenos::AnisoFilter::kMax_1_1)));
  }
  sampler_create_info.minLod = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    assert_false(parameters.mip_linear);
    sampler_create_info.maxLod = sampler_create_info.minLod + 0.25f;
  } else {
    sampler_create_info.maxLod = VK_LOD_CLAMP_NONE;
  }
  if (cvars::vulkan_debug_force_base_mip) {
    // Diagnostic: sample only the first allowed mip level.
    sampler_create_info.maxLod = sampler_create_info.minLod + 0.25f;
  } else if (cvars::vulkan_max_mip_levels_below_min >= 0) {
    // Limit sampling to N levels below the first allowed one.
    sampler_create_info.maxLod =
        std::min(sampler_create_info.maxLod,
                 sampler_create_info.minLod +
                     float(cvars::vulkan_max_mip_levels_below_min) + 0.25f);
  }
  if (cvars::vulkan_debug_fixed_mip_level >= 0 &&
      parameters.mip_min_level == 0 && !parameters.mip_base_map) {
    const float fixed_lod = float(cvars::vulkan_debug_fixed_mip_level);
    sampler_create_info.minLod = fixed_lod;
    sampler_create_info.maxLod = fixed_lod;
    sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_create_info.anisotropyEnable = VK_FALSE;
    sampler_create_info.maxAnisotropy = 1.0f;
  }
  // The two YCbCr border colors are not expressible as fixed Vulkan border
  // color enums. Use a custom border color when supported, otherwise fall back
  // to transparent black (matching the alpha at least).
  VkSamplerCustomBorderColorCreateInfoEXT custom_border_color = {
      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
  const bool custom_border_color_supported =
      vulkan_device->properties().customBorderColors &&
      vulkan_device->properties().customBorderColorWithoutFormat;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
    case xenos::BorderColor::k_ACBCRY_Black:
      if (custom_border_color_supported) {
        float* const color = custom_border_color.customBorderColor.float32;
        if (parameters.border_color == xenos::BorderColor::k_ACBYCR_Black) {
          // (Cr, Y, Cb) unsigned.
          color[0] = 0.5f;
          color[1] = 0.0f;
          color[2] = 0.5f;
        } else {
          // (Y, Cr, Cb) unsigned.
          color[0] = 0.0f;
          color[1] = 0.5f;
          color[2] = 0.5f;
        }
        color[3] = parameters.force_bc_w_to_max ? 1.0f : 0.0f;
        custom_border_color.format = VK_FORMAT_UNDEFINED;
        custom_border_color.pNext = sampler_create_info.pNext;
        sampler_create_info.pNext = &custom_border_color;
        sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
      } else {
        sampler_create_info.borderColor =
            parameters.force_bc_w_to_max
                ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      }
      break;
    default:
      sampler_create_info.borderColor =
          parameters.force_bc_w_to_max
              ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
              : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      break;
  }
  VkSampler vulkan_sampler;
  if (dfn.vkCreateSampler(device, &sampler_create_info, nullptr,
                          &vulkan_sampler) != VK_SUCCESS) {
    XELOGE(
        "VulkanTextureCache: Failed to create the sampler for parameters "
        "0x{:08X}",
        parameters.value);
    has_overflown_out = false;
    return VK_NULL_HANDLE;
  }
  std::pair<const SamplerParameters, Sampler>& new_sampler =
      *(samplers_
            .emplace(std::piecewise_construct,
                     std::forward_as_tuple(parameters), std::forward_as_tuple())
            .first);
  COUNT_profile_set("gpu/texture_cache/vulkan/samplers", samplers_.size());
  new_sampler.second.sampler = vulkan_sampler;
  new_sampler.second.last_usage_submission = submission_current;
  new_sampler.second.used_previous = sampler_used_last_;
  new_sampler.second.used_next = nullptr;
  if (sampler_used_last_) {
    sampler_used_last_->second.used_next = &new_sampler;
  } else {
    sampler_used_first_ = &new_sampler;
  }
  sampler_used_last_ = &new_sampler;
  lookup = {sampler_destroy_generation_, parameters, &new_sampler};
  return vulkan_sampler;
}

uint64_t VulkanTextureCache::GetSubmissionToAwaitOnSamplerOverflow(
    uint32_t overflowed_sampler_count) const {
  if (!overflowed_sampler_count) {
    return 0;
  }
  std::pair<const SamplerParameters, Sampler>* sampler_used =
      sampler_used_first_;
  if (!sampler_used_first_) {
    return 0;
  }
  for (uint32_t samplers_remaining = overflowed_sampler_count - 1;
       samplers_remaining; --samplers_remaining) {
    std::pair<const SamplerParameters, Sampler>* sampler_used_next =
        sampler_used->second.used_next;
    if (!sampler_used_next) {
      break;
    }
    sampler_used = sampler_used_next;
  }
  return sampler_used->second.last_usage_submission;
}

VkImageView VulkanTextureCache::RequestSwapTexture(
    uint32_t& width_scaled_out, uint32_t& height_scaled_out,
    xenos::TextureFormat& format_out) {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    XELOGE(
        "RequestSwapTexture: invalid front buffer fetch constant (valid {}, "
        "base page {:X}, dimension {})",
        uint32_t(key.is_valid), key.base_page, uint32_t(key.dimension));
    return nullptr;
  }
  VulkanTexture* texture =
      static_cast<VulkanTexture*>(FindOrCreateTexture(key));
  if (!texture) {
    XELOGE(
        "RequestSwapTexture: failed to find or create the swap texture "
        "(base page {:X}, format {})",
        key.base_page, uint32_t(key.format));
    return VK_NULL_HANDLE;
  }
  VkImageView texture_view = texture->GetView(
      false, GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key)),
      false);
  if (texture_view == VK_NULL_HANDLE) {
    XELOGE("RequestSwapTexture: failed to get the swap texture image view");
    return VK_NULL_HANDLE;
  }
  if (!LoadTextureData(*texture)) {
    XELOGE("Failed to load texture data for swap texture");
    return VK_NULL_HANDLE;
  }
  texture->MarkAsUsed();
  VulkanTexture::Usage old_usage =
      texture->SetUsage(VulkanTexture::Usage::kSwapSampled);
  if (old_usage != VulkanTexture::Usage::kSwapSampled) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    VkImageLayout old_layout, new_layout;
    GetTextureUsageMasks(old_usage, src_stage_mask, src_access_mask,
                         old_layout);
    GetTextureUsageMasks(VulkanTexture::Usage::kSwapSampled, dst_stage_mask,
                         dst_access_mask, new_layout);
    command_processor_.PushImageMemoryBarrier(
        texture->image(), ui::vulkan::util::InitializeSubresourceRange(),
        src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        old_layout, new_layout);
  }
  // Only texture->key, not the result of BindingInfoFromFetchConstant, contains
  // whether the texture is scaled.
  key = texture->key();
  width_scaled_out =
      key.GetWidth() * (key.scaled_resolve ? draw_resolution_scale_x() : 1);
  height_scaled_out =
      key.GetHeight() * (key.scaled_resolve ? draw_resolution_scale_y() : 1);
  format_out = key.format;
  return texture_view;
}

bool VulkanTextureCache::IsScaledResolveSupportedForFormat(
    TextureKey key) const {
  // Check if the format has a valid host format pair, meaning we can handle it
  const HostFormatPair& host_format_pair = GetHostFormatPair(key);
  return host_format_pair.format_unsigned.format != VK_FORMAT_UNDEFINED;
}

bool VulkanTextureCache::IsSignedVersionSeparateForFormat(
    TextureKey key) const {
  const HostFormatPair& host_format_pair = GetHostFormatPair(key);
  if (host_format_pair.format_unsigned.format == VK_FORMAT_UNDEFINED ||
      host_format_pair.format_signed.format == VK_FORMAT_UNDEFINED) {
    // Just one signedness.
    return false;
  }
  return !host_format_pair.unsigned_signed_compatible;
}

uint32_t VulkanTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return GetHostFormatPair(key).swizzle;
}

uint32_t VulkanTextureCache::GetMaxHostTextureWidthHeight(
    xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return device_properties.maxImageDimension2D;
    case xenos::DataDimension::k3D:
      return device_properties.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      return device_properties.maxImageDimensionCube;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t VulkanTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return device_properties.maxImageArrayLayers;
    case xenos::DataDimension::k3D:
      return device_properties.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      // Not requesting the imageCubeArray feature, and the Xenos doesn't
      // support cube map arrays.
      return 6;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

void VulkanTextureCache::DescribePassBytes(PassByteCatalog& c) const {
  auto describe = [&](const VulkanTexture& texture) {
    auto key = texture.key();
    const auto& pair = GetHostFormatPair(key);
    const auto& host = key.signed_separate || pair.format_unsigned.format == VK_FORMAT_UNDEFINED
                           ? pair.format_signed : pair.format_unsigned;
    PassByteCatalog::Image info;
    info.image = PassByteCatalog::Id(texture.image());
    info.format = host.format; info.bpp = PassByteCatalog::FormatBytes(host.format);
    info.width = key.GetWidth(); info.height = key.GetHeight();
    info.mip_levels = key.mip_max_level + 1;
    info.layers = key.GetDepthOrArraySize();
    if (key.scaled_resolve) {
      info.width *= draw_resolution_scale_x(); info.height *= draw_resolution_scale_y();
    }
    if (SkipsTopMip(key)) {
      --info.mip_levels;
      info.width = std::max(1u, info.width >> 1); info.height = std::max(1u, info.height >> 1);
    }
    if (host.block_compressed) info.block_w = info.block_h = 4;
    c.images[info.image] = info;
  };
  ForEachTextureForDebug([&](const Texture& texture) {
    const auto& vk = static_cast<const VulkanTexture&>(texture);
    describe(vk);
    if (vk.wrapper_3d_as_2d_for_debug()) describe(*vk.wrapper_3d_as_2d_for_debug());
  });
}

std::unique_ptr<TextureCache::Texture> VulkanTextureCache::CreateTexture(
    TextureKey key) {
  VkFormat formats[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
  const HostFormatPair& host_format = GetHostFormatPair(key);
  if (host_format.format_signed.format == VK_FORMAT_UNDEFINED) {
    // Only the unsigned format may be available, if at all.
    formats[0] = host_format.format_unsigned.format;
  } else if (host_format.format_unsigned.format == VK_FORMAT_UNDEFINED) {
    // Only the signed format may be available, if at all.
    formats[0] = host_format.format_signed.format;
  } else {
    // Both unsigned and signed formats are available.
    if (IsSignedVersionSeparateForFormat(key)) {
      formats[0] = key.signed_separate ? host_format.format_signed.format
                                       : host_format.format_unsigned.format;
    } else {
      // Same format for unsigned and signed, or compatible formats.
      formats[0] = host_format.format_unsigned.format;
      if (host_format.format_signed.format !=
          host_format.format_unsigned.format) {
        assert_not_zero(host_format.unsigned_signed_compatible);
        formats[1] = host_format.format_signed.format;
      }
    }
  }
  if (formats[0] == VK_FORMAT_UNDEFINED) {
    // TODO(Triang3l): If there's no best format, set that a format unsupported
    // by the emulator completely is used to report at the end of the frame.
    return nullptr;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();

  VkImageCreateInfo image_create_info;
  VkImageCreateInfo* image_create_info_last = &image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  if (formats[1] != VK_FORMAT_UNDEFINED) {
    image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  if (key.dimension == xenos::DataDimension::kCube) {
    image_create_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  image_create_info.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  image_create_info.format = formats[0];
  image_create_info.extent.width = key.GetWidth();
  image_create_info.extent.height = key.GetHeight();
  if (key.scaled_resolve) {
    image_create_info.extent.width *= draw_resolution_scale_x();
    image_create_info.extent.height *= draw_resolution_scale_y();
  }
  image_create_info.extent.depth = is_3d ? depth_or_array_size : 1;
  image_create_info.mipLevels = key.mip_max_level + 1;
  if (SkipsTopMip(key)) {
    image_create_info.extent.width =
        std::max(image_create_info.extent.width >> 1, UINT32_C(1));
    image_create_info.extent.height =
        std::max(image_create_info.extent.height >> 1, UINT32_C(1));
    image_create_info.mipLevels = key.mip_max_level;
  }
  image_create_info.arrayLayers = is_3d ? 1 : depth_or_array_size;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  // For scaled resolve textures with mips, we need transfer source to generate
  // mip levels via blit from the base level.
  if ((key.scaled_resolve && key.mip_max_level > 0) ||
      ShouldGenerateTextureMips(key) || cvars::vulkan_debug_texture_audit) {
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  bool depth_surface_renderable = false;
  // Resolve destinations the render target cache can draw straight into.
  const bool color_surface = cvars::vulkan_color_surface_resolve &&
                             key.format == xenos::TextureFormat::k_8_8_8_8 &&
                             formats[0] == VK_FORMAT_R8G8B8A8_UNORM;
  if (cvars::vulkan_texture_from_render_target &&
      ((key.format == xenos::TextureFormat::k_24_8_FLOAT &&
        formats[0] == VK_FORMAT_R32_SFLOAT &&
        formats[1] == VK_FORMAT_UNDEFINED) ||
       color_surface) &&
      !key.scaled_resolve &&
      key.dimension == xenos::DataDimension::k2DOrStacked &&
      !key.mip_max_level && depth_or_array_size == 1) {
    VkImageFormatProperties properties;
    const auto& ifn = vulkan_device->vulkan_instance()->functions();
    const VkImageUsageFlags usage =
        image_create_info.usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ifn.vkGetPhysicalDeviceImageFormatProperties(
            vulkan_device->physical_device(), formats[0], VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, usage, image_create_info.flags,
            &properties) == VK_SUCCESS &&
        image_create_info.extent.width <= properties.maxExtent.width &&
        image_create_info.extent.height <= properties.maxExtent.height &&
        (properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
      image_create_info.usage = usage;
      depth_surface_renderable = true;
    }
  }
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat image_view_formats[2] = {formats[0], formats[1]};
  uint32_t image_view_format_count = formats[1] != VK_FORMAT_UNDEFINED ? 2 : 1;
  VkImageFormatListCreateInfo image_format_list_create_info;
  if (image_view_format_count > 1 &&
      vulkan_device->extensions().ext_1_2_KHR_image_format_list) {
    image_create_info_last->pNext = &image_format_list_create_info;
    image_create_info_last =
        reinterpret_cast<VkImageCreateInfo*>(&image_format_list_create_info);
    image_format_list_create_info.sType =
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    image_format_list_create_info.pNext = nullptr;
    image_format_list_create_info.viewFormatCount = image_view_format_count;
    image_format_list_create_info.pViewFormats = image_view_formats;
  }

  VkImageCompressionControlEXT compression_control;
  vulkan_device->ChainDefaultImageCompression(image_create_info,
                                              compression_control);

  VmaAllocationCreateInfo allocation_create_info = {};
  allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image;
  VmaAllocation allocation;
  VkResult allocation_result = vmaCreateImage(
      vma_allocator_, &image_create_info, &allocation_create_info,
      &image, &allocation, nullptr);
  if (allocation_result != VK_SUCCESS && depth_surface_renderable) {
    // Added attachment usage is optional. Allocation failure must keep the
    // original texture path available, rather than turn it into a null image.
    image_create_info.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    depth_surface_renderable = false;
    allocation_result = vmaCreateImage(
        vma_allocator_, &image_create_info, &allocation_create_info,
        &image, &allocation, nullptr);
  }
  if (allocation_result != VK_SUCCESS) {
    XELOGE(
        "VulkanTextureCache: vmaCreateImage failed (format {}, {}x{}x{}, "
        "guest format {})",
        uint32_t(image_create_info.format), image_create_info.extent.width,
        image_create_info.extent.height, image_create_info.extent.depth,
        uint32_t(key.format));
    return nullptr;
  }

  if (cvars::log_image_compression) {
    vulkan_device->LogImageCompression(2, image, image_create_info,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
  }
  auto vulkan_texture = new VulkanTexture(*this, key, image, allocation);
  vulkan_texture->depth_surface_renderable = depth_surface_renderable;
  vulkan_texture->surface_is_color = depth_surface_renderable && color_surface;
  if (ResolveTextureBytesPerPixel(key)) {
    resolve_textures_.emplace(key.base_page << 12, vulkan_texture);
  }
  return std::unique_ptr<Texture>(vulkan_texture);
}

void VulkanTextureCache::DebugWriteTextureDumps(
    const std::filesystem::path& dir) {
  if (!HasPendingDebugTextureDumps()) {
    return;
  }
  shared_memory().PrepareForCpuAccess(0, SharedMemory::kBufferSize);
  debug_texture_dumps_written_ = true;
  std::error_code dir_error;
  std::filesystem::create_directories(dir, dir_error);
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  Memory& memory = shared_memory().guest_memory();
  FILE* index_file = std::fopen((dir / "index.jsonl").string().c_str(), "wb");
  uint32_t written = 0;
  for (size_t i = 0; i < debug_texture_dumps_.size(); ++i) {
    DebugTextureDump& dump = debug_texture_dumps_[i];
    void* mapped = nullptr;
    bool mapped_ok = dfn.vkMapMemory(device, dump.readback_memory, 0,
                                     VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    if (mapped_ok) {
      ui::vulkan::util::InvalidateMappedMemoryRange(
          vulkan_device, dump.readback_memory, dump.readback_memory_type, 0,
          dump.readback_memory_size);
    }
    // Sections, in order: cpu_base_load, cpu_mips_load, cpu_base_now,
    // cpu_mips_now, gpu_shared_base, gpu_shared_mips, gpu_decoded.
    std::filesystem::path bin_path = dir / fmt::format("tex-{:03}.bin", i);
    bool bin_ok = false;
    if (FILE* bin_file = std::fopen(bin_path.string().c_str(), "wb")) {
      std::fwrite(dump.cpu_base_at_load.data(), 1, dump.cpu_base_at_load.size(),
                  bin_file);
      std::fwrite(dump.cpu_mips_at_load.data(), 1, dump.cpu_mips_at_load.size(),
                  bin_file);
      if (dump.shared_base_size) {
        std::fwrite(memory.TranslatePhysical<const uint8_t*>(
                        uint32_t(dump.key.base_page) << 12),
                    1, size_t(dump.shared_base_size), bin_file);
      }
      std::fwrite(memory.TranslatePhysical<const uint8_t*>(
                      uint32_t(dump.key.mip_page) << 12),
                  1, size_t(dump.shared_mips_size), bin_file);
      if (mapped_ok) {
        const uint8_t* mapped_bytes = static_cast<const uint8_t*>(mapped);
        std::fwrite(mapped_bytes + dump.scratch_size, 1,
                    size_t(dump.shared_base_size + dump.shared_mips_size),
                    bin_file);
        std::fwrite(mapped_bytes, 1, size_t(dump.scratch_size), bin_file);
      }
      bin_ok = std::ferror(bin_file) == 0;
      bin_ok = std::fclose(bin_file) == 0 && bin_ok;
    }
    if (mapped_ok) {
      dfn.vkUnmapMemory(device, dump.readback_memory);
    }
    std::string sampler_json;
    for (size_t v = 0; v < dump.sampler_variants.size(); ++v) {
      const std::array<uint32_t, 10>& variant = dump.sampler_variants[v];
      sampler_json += fmt::format(
          "{}[{},{},{},{},{},{},{},{},{}]", v ? "," : "", variant[0],
          variant[1], variant[2], variant[3], variant[4], variant[5],
          variant[6], variant[7], variant[8]);
    }
    std::string live_json = "{\"alive\":false}";
    if (const Texture* live = FindTextureForDebug(dump.key)) {
      live_json = fmt::format(
          "{{\"alive\":true,\"base_outdated\":{},\"mips_outdated\":{}}}",
          live->base_outdated_lockless(), live->mips_outdated_lockless());
    }
    std::string heap_json;
    if (VirtualHeap* physical_heap = memory.GetPhysicalHeap()) {
      for (uint32_t page : {uint32_t(dump.key.base_page),
                            uint32_t(dump.key.mip_page)}) {
        HeapAllocationInfo heap_info = {};
        physical_heap->QueryRegionInfo(page << 12, &heap_info);
        heap_json += fmt::format(
            "{}[{},{},{},{},{},{}]", heap_json.empty() ? "" : ",",
            heap_info.base_address, heap_info.allocation_base,
            heap_info.allocation_size, heap_info.region_size, heap_info.state,
            heap_info.protect);
      }
    }
    if (index_file) {
      std::string line = fmt::format(
          "{{\"index\":{},\"file\":\"tex-{:03}.bin\",\"ok\":{},"
          "\"gpu_ok\":{},\"base_size\":{},\"mips_size\":{},"
          "\"scratch_size\":{},\"later_loads\":{},\"later_load_kinds\":\"{}\","
          "\"sampler_uses\":{},\"live\":{},\"samplers\":[{}],"
          "\"heap\":[{}],{}}}\n",
          i, i, bin_ok ? "true" : "false", mapped_ok ? "true" : "false",
          dump.shared_base_size, dump.shared_mips_size, dump.scratch_size,
          dump.later_loads, dump.later_load_kinds, dump.sampler_uses,
          live_json, sampler_json, heap_json, dump.metadata);
      std::fwrite(line.data(), 1, line.size(), index_file);
    }
    written += bin_ok ? 1 : 0;
    dfn.vkDestroyBuffer(device, dump.readback_buffer, nullptr);
    dfn.vkFreeMemory(device, dump.readback_memory, nullptr);
  }
  if (index_file) {
    std::fclose(index_file);
  }
  XELOGI("[TEXDUMP] status={} textures={} written={} dir={}",
         index_file ? "ok" : "error", debug_texture_dumps_.size(), written,
         dir.string());
  debug_texture_dumps_.clear();
}

namespace {
uint32_t DebugAuditTexelSize(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_SFLOAT:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
      return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    default:
      return 0;
  }
}
}  // namespace

bool VulkanTextureCache::DebugTextureAuditPending() const {
  return cvars::vulkan_debug_texture_audit && !debug_texture_audit_written_;
}

void VulkanTextureCache::DebugRecordTextureAudit() {
  shared_memory().PrepareForCpuAccess(0, SharedMemory::kBufferSize);
  if (!cvars::vulkan_debug_texture_audit || debug_texture_audit_recorded_) {
    return;
  }
  debug_texture_audit_recorded_ = true;
  // Called in a submission opened just for the audit, right after the
  // captured frame's submissions.
  uint64_t current_submission = current_submission_index_for_debug();
  uint64_t min_submission = current_submission > 3 ? current_submission - 3 : 0;
  std::vector<VulkanTexture*> candidates;
  ForEachTextureForDebug([&](const Texture& texture) {
    const TextureKey& key = texture.key();
    // 3D and cube textures too: their first slice / face is read back.
    if (texture.last_usage_submission_index() < min_submission ||
        key.scaled_resolve) {
      return;
    }
    const VulkanTexture& vulkan_texture =
        static_cast<const VulkanTexture&>(texture);
    if (vulkan_texture.usage() == VulkanTexture::Usage::kUndefined) {
      return;
    }
    candidates.push_back(const_cast<VulkanTexture*>(&vulkan_texture));
  });
  // Most recently used first so a size cap drops the stalest.
  std::sort(candidates.begin(), candidates.end(),
            [](const VulkanTexture* a, const VulkanTexture* b) {
              return a->last_usage_submission_index() >
                     b->last_usage_submission_index();
            });

  struct Planned {
    VulkanTexture* texture;
    VkFormat format;
    uint32_t texel_size;
    uint32_t level_first;
    VkDeviceSize offset;
    VkDeviceSize size;
  };
  std::vector<Planned> planned;
  VkDeviceSize max_bytes =
      VkDeviceSize(std::max(cvars::vulkan_debug_texture_audit_max_mb, 1))
      << 20;
  VkDeviceSize total = 0;
  for (VulkanTexture* texture : candidates) {
    const TextureKey& key = texture->key();
    const HostFormatPair& host_format = GetHostFormatPair(key);
    VkFormat format;
    if (host_format.format_signed.format == VK_FORMAT_UNDEFINED) {
      format = host_format.format_unsigned.format;
    } else if (host_format.format_unsigned.format == VK_FORMAT_UNDEFINED) {
      format = host_format.format_signed.format;
    } else if (IsSignedVersionSeparateForFormat(key)) {
      format = key.signed_separate ? host_format.format_signed.format
                                   : host_format.format_unsigned.format;
    } else {
      format = host_format.format_unsigned.format;
    }
    uint32_t texel_size = DebugAuditTexelSize(format);
    if (!texel_size) {
      continue;
    }
    // Host image size and levels (texture_skip_top_mip_min_size stores guest
    // mip 1 as host level 0).
    const uint32_t mip_skip = SkipsTopMip(key) ? 1 : 0;
    uint32_t width = std::max(key.GetWidth() >> mip_skip, UINT32_C(1));
    uint32_t height = std::max(key.GetHeight() >> mip_skip, UINT32_C(1));
    const uint32_t host_max_level = key.mip_max_level - mip_skip;
    uint32_t level_first = 0;
    while (level_first < host_max_level &&
           (std::max(width, height) >> level_first) > 512) {
      ++level_first;
    }
    VkDeviceSize size = 0;
    for (uint32_t level = level_first; level <= host_max_level; ++level) {
      size += xe::align(VkDeviceSize(std::max(width >> level, UINT32_C(1))) *
                            std::max(height >> level, UINT32_C(1)) * texel_size,
                        VkDeviceSize(16));
    }
    if (total + size > max_bytes) {
      continue;
    }
    planned.push_back({texture, format, texel_size, level_first, total, size});
    total += size;
  }
  if (planned.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback,
          debug_texture_audit_buffer_, debug_texture_audit_memory_,
          &debug_texture_audit_memory_type_,
          &debug_texture_audit_memory_size_)) {
    XELOGE("[TEXAUDIT] status=error reason=alloc bytes={}", total);
    return;
  }
  debug_texture_audit_used_size_ = total;

  VkImageSubresourceRange all_levels = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                        VK_REMAINING_MIP_LEVELS, 0,
                                        VK_REMAINING_ARRAY_LAYERS};
  for (const Planned& p : planned) {
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
    GetTextureUsageMasks(p.texture->usage(), stage, access, layout);
    command_processor_.PushImageMemoryBarrier(
        p.texture->image(), all_levels, stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
        access, VK_ACCESS_TRANSFER_READ_BIT, layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  Memory& memory = shared_memory().guest_memory();
  size_t changed_not_outdated = 0;
  for (size_t i = 0; i < planned.size(); ++i) {
    const Planned& p = planned[i];
    VulkanTexture& texture = *p.texture;
    const TextureKey& key = texture.key();
    const uint32_t mip_skip = SkipsTopMip(key) ? 1 : 0;
    uint32_t width = std::max(key.GetWidth() >> mip_skip, UINT32_C(1));
    uint32_t height = std::max(key.GetHeight() >> mip_skip, UINT32_C(1));
    std::vector<VkBufferImageCopy> regions;
    std::string levels_json;
    VkDeviceSize offset = p.offset;
    for (uint32_t level = p.level_first; level <= key.mip_max_level - mip_skip;
         ++level) {
      uint32_t level_width = std::max(width >> level, UINT32_C(1));
      uint32_t level_height = std::max(height >> level, UINT32_C(1));
      VkBufferImageCopy& region = regions.emplace_back();
      region.bufferOffset = offset;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      region.imageOffset = {0, 0, 0};
      region.imageExtent = {level_width, level_height, 1};
      levels_json += fmt::format("{}[{},{},{},{}]",
                                 levels_json.empty() ? "" : ",", level, offset,
                                 level_width, level_height);
      offset += xe::align(
          VkDeviceSize(level_width) * level_height * p.texel_size,
          VkDeviceSize(16));
    }
    command_buffer.CmdVkCopyImageToBuffer(
        texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        debug_texture_audit_buffer_, uint32_t(regions.size()), regions.data());

    // Guest bytes the first stored level is loaded from, as they are now.
    uint32_t source_page = key.base_page ? key.base_page : key.mip_page;
    uint32_t source_size =
        key.base_page ? texture.GetGuestBaseSize() : texture.GetGuestMipsSize();
    const uint8_t* source_bytes =
        memory.TranslatePhysical<const uint8_t*>(source_page << 12);
    uint64_t hash_now = XXH3_64bits(source_bytes, source_size);
    bool changed = texture.debug_source_hashed &&
                   hash_now != texture.debug_source_hash;
    bool base_outdated = texture.base_outdated_lockless();
    bool mips_outdated = texture.mips_outdated_lockless();
    if (changed && !(key.base_page ? base_outdated : mips_outdated)) {
      ++changed_not_outdated;
    }
    DebugTextureAudit audit;
    audit.guest_offset = debug_texture_audit_guest_.size();
    if (debug_texture_audit_guest_.size() + source_size <= (size_t(96) << 20)) {
      debug_texture_audit_guest_.insert(debug_texture_audit_guest_.end(),
                                        source_bytes,
                                        source_bytes + source_size);
      audit.guest_size = source_size;
    }
    const texture_util::TextureGuestLayout& guest_layout =
        texture.guest_layout();
    audit.metadata = fmt::format(
        "\"index\":{},\"format\":{},\"format_name\":\"{}\",\"width\":{},"
        "\"height\":{},\"pitch\":{},\"tiled\":{},\"packed_mips\":{},"
        "\"endianness\":{},\"signed_separate\":{},\"base_page\":{},"
        "\"mip_page\":{},\"mip_max_level\":{},\"packed_level\":{},"
        "\"base_row_pitch_bytes\":{},\"vk_format\":{},\"texel_size\":{},"
        "\"levels\":[{}],\"hashed\":{},\"changed\":{},\"base_outdated\":{},"
        "\"mips_outdated\":{},\"loads\":{},\"last_load_submission\":{},"
        "\"last_usage_submission\":{},\"current_submission\":{},"
        "\"generate_mips\":{},\"usage\":{},\"host_memory\":{},"
        "\"guest_offset\":{},\"guest_size\":{},\"dimension\":{},"
        "\"depth\":{},\"mip_skip\":{}",
        i, uint32_t(key.format), FormatInfo::GetName(key.format), width,
        height, uint32_t(key.pitch), uint32_t(key.tiled),
        uint32_t(key.packed_mips), uint32_t(key.endianness),
        uint32_t(key.signed_separate), uint32_t(key.base_page),
        uint32_t(key.mip_page), uint32_t(key.mip_max_level),
        guest_layout.packed_level, guest_layout.base.row_pitch_bytes,
        uint32_t(p.format), p.texel_size, levels_json,
        texture.debug_source_hashed, changed, base_outdated, mips_outdated,
        texture.debug_loads, texture.debug_last_load_submission,
        texture.last_usage_submission_index(), current_submission,
        ShouldGenerateTextureMips(key), uint32_t(texture.usage()),
        texture.GetHostMemoryUsage(), audit.guest_offset, audit.guest_size,
        uint32_t(key.dimension), key.GetDepthOrArraySize(),
        SkipsTopMip(key) ? 1 : 0);
    debug_texture_audits_.push_back(std::move(audit));
  }
  for (const Planned& p : planned) {
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
    GetTextureUsageMasks(p.texture->usage(), stage, access, layout);
    command_processor_.PushImageMemoryBarrier(
        p.texture->image(), all_levels, VK_PIPELINE_STAGE_TRANSFER_BIT, stage,
        VK_ACCESS_TRANSFER_READ_BIT, access,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
  }
  command_processor_.PushBufferMemoryBarrier(
      debug_texture_audit_buffer_, 0, VK_WHOLE_SIZE,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  command_processor_.SubmitBarriers(true);
  XELOGI(
      "[TEXAUDIT] phase=record candidates={} recorded={} bytes={} "
      "changed_not_outdated={} textures_total={}",
      candidates.size(), planned.size(), total, changed_not_outdated,
      [this]() {
        size_t count = 0;
        ForEachTextureForDebug([&](const Texture&) { ++count; });
        return count;
      }());
}

void VulkanTextureCache::DebugWriteTextureAudit(
    const std::filesystem::path& dir) {
  if (debug_texture_audit_written_) {
    return;
  }
  debug_texture_audit_written_ = true;
  std::error_code dir_error;
  std::filesystem::create_directories(dir, dir_error);
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  bool images_ok = false;
  if (debug_texture_audit_memory_ != VK_NULL_HANDLE) {
    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, debug_texture_audit_memory_, 0, VK_WHOLE_SIZE,
                        0, &mapped) == VK_SUCCESS) {
      ui::vulkan::util::InvalidateMappedMemoryRange(
          vulkan_device, debug_texture_audit_memory_,
          debug_texture_audit_memory_type_, 0,
          debug_texture_audit_memory_size_);
      if (FILE* file =
              std::fopen((dir / "images.bin").string().c_str(), "wb")) {
        std::fwrite(mapped, 1, size_t(debug_texture_audit_used_size_), file);
        images_ok = std::ferror(file) == 0;
        images_ok = std::fclose(file) == 0 && images_ok;
      }
      dfn.vkUnmapMemory(device, debug_texture_audit_memory_);
    }
    dfn.vkDestroyBuffer(device, debug_texture_audit_buffer_, nullptr);
    dfn.vkFreeMemory(device, debug_texture_audit_memory_, nullptr);
    debug_texture_audit_buffer_ = VK_NULL_HANDLE;
    debug_texture_audit_memory_ = VK_NULL_HANDLE;
  }
  if (FILE* file = std::fopen((dir / "guest.bin").string().c_str(), "wb")) {
    std::fwrite(debug_texture_audit_guest_.data(), 1,
                debug_texture_audit_guest_.size(), file);
    std::fclose(file);
  }
  if (FILE* file = std::fopen((dir / "audit.jsonl").string().c_str(), "wb")) {
    for (const DebugTextureAudit& audit : debug_texture_audits_) {
      std::string line = "{" + audit.metadata + "}\n";
      std::fwrite(line.data(), 1, line.size(), file);
    }
    std::fclose(file);
  }
  XELOGI("[TEXAUDIT] status={} textures={} dir={}",
         images_ok ? "ok" : "error", debug_texture_audits_.size(),
         dir.string());
  debug_texture_audits_.clear();
  debug_texture_audit_guest_.clear();
  debug_texture_audit_guest_.shrink_to_fit();
}

uint32_t VulkanTextureCache::ResolveTextureBytesPerPixel(
    const TextureKey& key) const {
  if (!key.base_page || !key.tiled || key.packed_mips || key.mip_max_level ||
      key.GetWidth() > (uint32_t(key.pitch) << 5) ||
      key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.GetDepthOrArraySize() != 1) {
    return 0;
  }
  const HostFormatPair& pair = GetHostFormatPair(key);
  const HostFormat& format =
      key.signed_separate || pair.format_unsigned.format == VK_FORMAT_UNDEFINED
          ? pair.format_signed
          : pair.format_unsigned;
  // Check the actual device fallback, not the nominal guest format. Norm16
  // conversion loaders (Mali) aren't raw bits: the resolve shader converts
  // like them only with vulkan_resolve_texture_copy_norm16.
  switch (format.load_shader) {
    case kLoadShaderIndex8bpb:
      return 1;
    case kLoadShaderIndex16bpb:
      return 2;
    case kLoadShaderIndex32bpb:
      return 4;
    case kLoadShaderIndex64bpb:
      return 8;
    case kLoadShaderIndex128bpb:
      return 16;
    case kLoadShaderIndexDepthUnorm:
    case kLoadShaderIndexDepthFloat:
      return 4;
    case kLoadShaderIndexRG16UNormToFloat:
    case kLoadShaderIndexRG16SNormToFloat:
      return cvars::vulkan_resolve_texture_copy_norm16 ? 4 : 0;
    case kLoadShaderIndexRGBA16UNormToFloat:
    case kLoadShaderIndexRGBA16SNormToFloat:
      return cvars::vulkan_resolve_texture_copy_norm16 ? 8 : 0;
    default:
      return 0;
  }
}

uint32_t VulkanTextureCache::ResolveTextureLinearDestFlags(
    const TextureKey& key) const {
  // Host bits of the resolve's RB_COPY_DEST_INFO (resolve.xesli,
  // XeResolveLinearWord): the loader's norm16 -> half float conversion.
  const HostFormatPair& pair = GetHostFormatPair(key);
  const HostFormat& format =
      key.signed_separate || pair.format_unsigned.format == VK_FORMAT_UNDEFINED
          ? pair.format_signed
          : pair.format_unsigned;
  switch (format.load_shader) {
    case kLoadShaderIndexRG16UNormToFloat:
    case kLoadShaderIndexRGBA16UNormToFloat:
      return UINT32_C(0x40000000);
    case kLoadShaderIndexRG16SNormToFloat:
    case kLoadShaderIndexRGBA16SNormToFloat:
      return UINT32_C(0x20000000);
    default:
      return 0;
  }
}

namespace {
// log_resolve_texture_copy_rejects: why a texture at a resolve's destination
// was not filled straight from the resolve (it then reloads from guest memory).
enum ResolveCopyReject : uint32_t {
  kRcNoTexture, kRcFormat, kRcScaled, kRcPitch, kRcEndian, kRcTiling3D,
  kRcBaseDelta, kRcNotFull, kRcSize, kRcArrayOrEndian, kRcTaken, kRcCount,
};
uint64_t resolve_copy_rejects[kRcCount];
uint32_t resolve_copy_reject_format[2];
void NoteResolveCopy(ResolveCopyReject reason) {
  if (!cvars::log_resolve_texture_copy_rejects) return;
  ++resolve_copy_rejects[reason];
  static uint64_t calls = 0;
  if (++calls % cvars::log_resolve_texture_copy_rejects) return;
  const auto& r = resolve_copy_rejects;
  XELOGI("ResolveCopyRejects: taken={} no_texture={} format={} (last "
         "texture {} vs resolve {}) scaled={} pitch={} endian={} tiling3d={} "
         "base_delta={} not_full={} size={} array_or_endian={}",
         r[kRcTaken], r[kRcNoTexture], r[kRcFormat],
         resolve_copy_reject_format[0], resolve_copy_reject_format[1],
         r[kRcScaled], r[kRcPitch], r[kRcEndian], r[kRcTiling3D],
         r[kRcBaseDelta], r[kRcNotFull], r[kRcSize], r[kRcArrayOrEndian]);
}
}  // namespace

VulkanTextureCache::ResolveTextureCopy
VulkanTextureCache::PrepareResolveTextureCopy(
    const draw_util::ResolveInfo& info, bool scaled,
    const global_unique_lock_type& lock) {
  assert_true(lock.owns_lock());
  ResolveTextureCopy copy;
  if (!cvars::vulkan_resolve_texture_copy ||
      info.copy_dest_info.copy_dest_array ||
      uint32_t(info.copy_dest_info.copy_dest_endian) >= 4) {
    NoteResolveCopy(kRcArrayOrEndian);
    return copy;
  }
  // The most specific reason a texture at this base was skipped.
  ResolveCopyReject reject = kRcNoTexture;
  const uint32_t base = info.copy_dest_base_unadjusted & 0x1FFFFFFF;
  const auto format =
      GetBaseFormat(xenos::TextureFormat(info.copy_dest_info.copy_dest_format));
  const bool depth = format == xenos::TextureFormat::k_24_8 ||
                     format == xenos::TextureFormat::k_24_8_FLOAT;
  const uint32_t width = info.coordinate_info.width_div_8 << 3;
  const uint32_t height = info.height_div_8 << 3;
  const uint32_t sx = scaled ? draw_resolution_scale_x() : 1;
  const uint32_t sy = scaled ? draw_resolution_scale_y() : 1;
  if (!width || !height) return copy;
  for (const auto& entry : resolve_textures_) {
    VulkanTexture& texture = *entry.second;
    const TextureKey& key = texture.key();
    if (key.format != format || bool(key.scaled_resolve) != scaled ||
        key.pitch != info.copy_dest_coordinate_info.pitch_aligned_div_32 ||
        uint32_t(key.endianness) !=
            uint32_t(info.copy_dest_info.copy_dest_endian) ||
        texture.force_load_3d_tiling()) {
      if (entry.first == base) {
        texture.resolve_copy_last = 0x80;
        ++texture.resolve_copy_count;
        if (key.format != format) {
          reject = kRcFormat;
          resolve_copy_reject_format[0] = uint32_t(key.format);
          resolve_copy_reject_format[1] = uint32_t(format);
        } else if (bool(key.scaled_resolve) != scaled) {
          reject = kRcScaled;
        } else if (key.pitch !=
                   info.copy_dest_coordinate_info.pitch_aligned_div_32) {
          reject = kRcPitch;
        } else if (uint32_t(key.endianness) !=
                   uint32_t(info.copy_dest_info.copy_dest_endian)) {
          reject = kRcEndian;
        } else {
          reject = kRcTiling3D;
        }
      }
      continue;
    }
    uint32_t x = info.copy_dest_x0, y = info.copy_dest_y0;
    // Partial resolves keep a texture current when the rest of it already is.
    const bool partial_ok =
        depth || (cvars::vulkan_resolve_texture_copy_partial &&
                  ResolveTextureBytesPerPixel(key) == 4);
    if (base != entry.first) {
      // Preserve depth strip resolves. A tiled base delta is not a linear
      // row offset: only whole base-granularity blocks admit this translation.
      if (!partial_ok || scaled || base < entry.first ||
          base - entry.first >= texture.GetGuestBaseSize()) {
        continue;
      }
      reject = kRcBaseDelta;
      const uint32_t gx = xenos::GetTextureTiledXBaseGranularityLog2(false, 2);
      const uint32_t gy = xenos::GetTextureTiledYBaseGranularityLog2(false, 2);
      const uint32_t row_bytes = (uint32_t(key.pitch) << (5 + gy)) * 4;
      const uint32_t block_bytes = UINT32_C(4) << (gx + gy);
      if (!row_bytes || (base - entry.first) % row_bytes % block_bytes)
        continue;
      x += ((base - entry.first) % row_bytes / block_bytes) << gx;
      y += ((base - entry.first) / row_bytes) << gy;
    }
    // Resolve rectangles are 8-pixel aligned: a 320x184 resolve fills all of
    // a 320x180 texture (the copy region below is clamped to the texture).
    const bool full = !x && !y && width >= key.GetWidth() &&
                      height >= key.GetHeight() &&
                      (cvars::vulkan_resolve_texture_copy_covering ||
                       (width == key.GetWidth() &&
                        height == key.GetHeight()));
    if (x + width > (uint32_t(key.pitch) << 5) ||
        uint64_t(entry.first) + texture.GetGuestBaseSize() >
            UINT32_C(0x20000000)) {
      continue;
    }
    if ((!full && (scaled || !partial_ok ||
                   (!depth && (!texture.loaded_once() ||
                               texture.base_outdated(lock))))) ||
        x >= key.GetWidth() || y >= key.GetHeight()) {
      reject = kRcNotFull;
      texture.resolve_copy_last = 0x80 | kRcNotFull;
      ++texture.resolve_copy_count;
      if (cvars::log_resolve_texture_copy_rejects) {
        static uint32_t logged = 0;
        if (logged < 40) {
          ++logged;
          XELOGI("ResolveCopyNotFull: base {:08X} tex {:08X} {}x{} fmt {} "
                 "mips {} once {} outdated {} | resolve at {},{} {}x{} fmt {}",
                 base, entry.first, key.GetWidth(), key.GetHeight(),
                 uint32_t(key.format), uint32_t(key.mip_max_level),
                 uint32_t(texture.loaded_once()),
                 uint32_t(texture.base_outdated(lock)), x, y, width, height,
                 uint32_t(format));
        }
      }
      continue;
    }
    const uint32_t bpp = ResolveTextureBytesPerPixel(key);
    if (!bpp) continue;
    // One linear buffer per resolve: every target takes the same conversion.
    const uint32_t linear_dest_flags = ResolveTextureLinearDestFlags(key);
    if (!copy.targets.empty() && linear_dest_flags != copy.linear_dest_flags) {
      continue;
    }
    const VkDeviceSize size = VkDeviceSize(width) * sx * height * sy * bpp;
    if (size > command_processor_.GetVulkanDevice()
                   ->properties()
                   .maxStorageBufferRange) {
      reject = kRcSize;
      continue;
    }
    ResolveTextureCopy::Target target = {};
    target.texture = &texture;
    target.publish =
        full ||
        (depth && !x && !y && width >= key.GetWidth() &&
         height >= key.GetHeight()) ||
        (partial_ok && texture.loaded_once() && !texture.base_outdated(lock));
    target.region.bufferRowLength = width * sx;
    target.region.bufferImageHeight = height * sy;
    target.region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    target.region.imageOffset = {int32_t(x * sx), int32_t(y * sy), 0};
    target.region.imageExtent = {std::min(width, key.GetWidth() - x) * sx,
                                 std::min(height, key.GetHeight() - y) * sy, 1};
    texture.MarkAsUsed();
    copy.size = size;
    copy.linear_dest_flags = linear_dest_flags;
    copy.targets.push_back(target);
  }
  if (copy.targets.empty() && reject == kRcNoTexture &&
      cvars::log_resolve_texture_copy_rejects) {
    static uint32_t logged = 0;
    if (logged < 40) {
      ++logged;
      XELOGI("ResolveCopyNoTexture: base {:08X} at {},{} {}x{} fmt {} pitch {}",
             base, info.copy_dest_x0, info.copy_dest_y0, width, height,
             uint32_t(format),
             uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5);
    }
  }
  NoteResolveCopy(copy.targets.empty() ? reject : kRcTaken);
  return copy;
}

void VulkanTextureCache::CompleteResolveTextureCopy(
    const ResolveTextureCopy& copy, VkBuffer buffer,
    const draw_util::ResolveInfo& info, bool scaled,
    const global_unique_lock_type& lock) {
  assert_true(lock.owns_lock());
  // All aliases lose their old watches first. Only the exact images filled
  // below are republished, with new watches, under this same recursive lock.
  MarkRangeAsResolved(info.copy_dest_extent_start, info.copy_dest_extent_length,
                      scaled);
  if (buffer == VK_NULL_HANDLE || copy.targets.empty()) return;
  auto& commands = command_processor_.deferred_command_buffer();
  const uint32_t timer = command_processor_.BeginTimedSection(
      commands, 4, uint32_t(copy.targets.front().texture->key().format));
  command_processor_.PushBufferMemoryBarrier(
      buffer, 0, copy.size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);
  for (const auto& target : copy.targets) {
    auto& texture = *static_cast<VulkanTexture*>(target.texture);
    const auto old =
        texture.SetUsage(VulkanTexture::Usage::kTransferDestination);
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
    GetTextureUsageMasks(old, stage, access, layout);
    // Equal-layout writes also need a dependency (two resolves without a draw).
    command_processor_.PushImageMemoryBarrier(
        texture.image(), ui::vulkan::util::InitializeSubresourceRange(), stage,
        VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT,
        layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
  }
  command_processor_.SubmitBarriers(true);
  for (const auto& target : copy.targets) {
    auto& texture = *static_cast<VulkanTexture*>(target.texture);
    auto* region = commands.CmdCopyBufferToImageEmplace(
        buffer, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    *region = target.region;
    ++texture.resolve_copy_count;
    texture.resolve_copy_last = target.publish ? 2 : 3;
    if (target.publish && texture.MakeUpToDateAndWatch(lock)) {
      texture.set_loaded_once();
      texture.resolve_copy_pending_sample = true;
      texture.resolve_copy_last = 1;
    }
    // Leave the texture ready before the next guest pass without counting a
    // sample: only TransitionTextureForGuestShader at the consumer counts it.
    // Pixel shaders only: the next pass's geometry needn't wait for the copy.
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
    GetTextureUsageMasks(VulkanTexture::Usage::kGuestShaderSampled, stage,
                         access, layout);
    stage = GetGuestSampledStages(false);
    texture.vertex_visible = stage == guest_shader_pipeline_stages_;
    command_processor_.PushImageMemoryBarrier(
        texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
        VK_PIPELINE_STAGE_TRANSFER_BIT, stage, VK_ACCESS_TRANSFER_WRITE_BIT,
        access, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
    texture.SetUsage(VulkanTexture::Usage::kGuestShaderSampled);
  }
  command_processor_.SubmitBarriers(true);
  command_processor_.EndTimedSection(commands, timer);
}

bool VulkanTextureCache::VulkanTexture::EnsureDepthSurfaceFramebuffer(
    VkRenderPass render_pass) {
  if (depth_surface_framebuffer != VK_NULL_HANDLE) return true;
  if (!depth_surface_renderable) return false;
  const auto& cache = static_cast<const VulkanTextureCache&>(texture_cache());
  const auto* device = cache.command_processor_.GetVulkanDevice();
  const auto& dfn = device->functions();
  if (depth_surface_attachment_view == VK_NULL_HANDLE) {
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image();
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format =
        surface_is_color ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32_SFLOAT;
    view.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device->device(), &view, nullptr,
                             &depth_surface_attachment_view) != VK_SUCCESS)
      return false;
  }
  if (surface_is_color) {
    // The texture image alone: the guest bytes are its texels.
    VkFramebufferCreateInfo framebuffer = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &depth_surface_attachment_view;
    framebuffer.width = key().GetWidth();
    framebuffer.height = key().GetHeight();
    framebuffer.layers = 1;
    return dfn.vkCreateFramebuffer(device->device(), &framebuffer, nullptr,
                                   &depth_surface_framebuffer) == VK_SUCCESS;
  }
  if (depth_surface_stencil_view == VK_NULL_HANDLE) {
    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8_UINT;
    image_info.extent = {key().GetWidth(), key().GetHeight(), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            depth_surface_stencil_image, depth_surface_stencil_memory)) {
      return false;
    }
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = depth_surface_stencil_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8_UINT;
    view.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    if (dfn.vkCreateImageView(device->device(), &view, nullptr,
                              &depth_surface_stencil_view) != VK_SUCCESS)
      return false;
  }
  VkImageView attachments[] = {depth_surface_attachment_view,
                               depth_surface_stencil_view};
  VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer.renderPass = render_pass;
  framebuffer.attachmentCount = 2;
  framebuffer.pAttachments = attachments;
  framebuffer.width = key().GetWidth();
  framebuffer.height = key().GetHeight();
  framebuffer.layers = 1;
  return dfn.vkCreateFramebuffer(device->device(), &framebuffer, nullptr,
                                 &depth_surface_framebuffer) == VK_SUCCESS;
}

std::vector<VulkanTextureCache::DepthSurfaceTarget>
VulkanTextureCache::PrepareDepthSurfaceTargets(
    const draw_util::ResolveInfo& info, VkRenderPass render_pass,
    const global_unique_lock_type& lock) {
  assert_true(lock.owns_lock());
  std::vector<DepthSurfaceTarget> result;
  const auto range = resolve_textures_.equal_range(
      info.copy_dest_base_unadjusted & 0x1FFFFFFF);
  // Diagnostic (log_depth_materialize_callers): the first failing condition of
  // each candidate when no target is found.
  uint32_t candidates = 0, first_reject = 0;
  for (auto it = range.first; it != range.second; ++it) {
    auto& texture = *it->second;
    const auto& key = texture.key();
    if (cvars::log_depth_materialize_callers) {
      const bool d = info.IsCopyingDepth();
      uint32_t r = 0;
      if (key.format != (d ? xenos::TextureFormat::k_24_8_FLOAT
                           : xenos::TextureFormat::k_8_8_8_8)) r = 1;
      else if (static_cast<const VulkanTexture&>(texture).surface_is_color == d) r = 2;
      else if (!key.tiled) r = 3;
      else if (key.scaled_resolve) r = 4;
      else if (key.mip_max_level) r = 5;
      else if (key.dimension != xenos::DataDimension::k2DOrStacked ||
               key.GetDepthOrArraySize() != 1) r = 6;
      else if (texture.force_load_3d_tiling()) r = 7;
      else if (key.pitch != info.copy_dest_coordinate_info.pitch_aligned_div_32) r = 8;
      else if (uint32_t(key.endianness) !=
               uint32_t(info.copy_dest_info.copy_dest_endian)) r = 9;
      else if (key.GetWidth() != (info.coordinate_info.width_div_8 << 3) ||
               key.GetHeight() != (info.height_div_8 << 3)) r = 10;
      else if ((key.base_page << 12) != info.copy_dest_base) r = 11;
      else if (uint64_t(key.base_page << 12) + texture.GetGuestBaseSize() >
                   uint64_t(info.copy_dest_extent_start) +
                       info.copy_dest_extent_length ||
               (key.base_page << 12) < info.copy_dest_extent_start) r = 12;
      ++candidates;
      if (r && !first_reject) {
        first_reject = r | (uint32_t(key.format) << 8) | (key.mip_max_level << 16);
        static uint32_t logged = 0;
        if (logged < 24) {
          ++logged;
          XELOGI("VkRTTexture: no target: dest={:08X} {}x{} pitch32={} endian={} "
                 "depth={} | cand reason={} fmt={} {}x{} pitch32={} mips={} "
                 "endian={} base={:08X} size={:X} extent={:08X}+{:X}",
                 info.copy_dest_base, info.coordinate_info.width_div_8 << 3,
                 info.height_div_8 << 3,
                 info.copy_dest_coordinate_info.pitch_aligned_div_32,
                 uint32_t(info.copy_dest_info.copy_dest_endian), d, r,
                 uint32_t(key.format), key.GetWidth(), key.GetHeight(),
                 key.pitch, key.mip_max_level, uint32_t(key.endianness),
                 key.base_page << 12, texture.GetGuestBaseSize(),
                 info.copy_dest_extent_start, info.copy_dest_extent_length);
        }
      }
    }
    // Full 2D coverage only. Strip updates, mips, 3D/cube, format and tiling
    // aliases stay on the ordinary loader. Endian transformations cancel only
    // for an exact match between the resolve and the texture fetch.
    const bool depth = info.IsCopyingDepth();
    if (key.format != (depth ? xenos::TextureFormat::k_24_8_FLOAT
                             : xenos::TextureFormat::k_8_8_8_8) ||
        static_cast<const VulkanTexture&>(texture).surface_is_color == depth ||
        !key.tiled || key.scaled_resolve || key.mip_max_level ||
        key.dimension != xenos::DataDimension::k2DOrStacked ||
        key.GetDepthOrArraySize() != 1 || texture.force_load_3d_tiling() ||
        key.pitch != info.copy_dest_coordinate_info.pitch_aligned_div_32 ||
        uint32_t(key.endianness) !=
            uint32_t(info.copy_dest_info.copy_dest_endian) ||
        key.GetWidth() != (info.coordinate_info.width_div_8 << 3) ||
        key.GetHeight() != (info.height_div_8 << 3) ||
        (key.base_page << 12) != info.copy_dest_base ||
        uint64_t(key.base_page << 12) + texture.GetGuestBaseSize() >
            uint64_t(info.copy_dest_extent_start) + info.copy_dest_extent_length ||
        (key.base_page << 12) < info.copy_dest_extent_start ||
        !texture.EnsureDepthSurfaceFramebuffer(render_pass)) continue;
    texture.MarkAsUsed();
    result.push_back({&texture, texture.depth_surface_framebuffer,
                       key.GetWidth(), key.GetHeight()});
  }
  if (cvars::log_depth_materialize_callers && result.empty() && !candidates) {
    static uint32_t logged_none = 0;
    if (logged_none < 24) {
      ++logged_none;
      XELOGI("VkRTTexture: no target: dest={:08X} {}x{} depth={} | no texture "
             "at the address", info.copy_dest_base,
             info.coordinate_info.width_div_8 << 3, info.height_div_8 << 3,
             info.IsCopyingDepth());
    }
  }
  return result;
}

VkImageView VulkanTextureCache::PrepareDepthSurfaceSource(
    const DepthSurfaceTarget& target) {
  auto& texture = *static_cast<VulkanTexture*>(target.texture);
  texture.MarkAsUsed();
  VkPipelineStageFlags stage;
  VkAccessFlags access;
  VkImageLayout layout;
  GetTextureUsageMasks(texture.SetUsage(VulkanTexture::Usage::kDepthSurfaceComputeRead),
                       stage, access, layout);
  // Record the compute reader so later guest sampling or overwrites include
  // it in their source execution dependency.
  command_processor_.PushImageMemoryBarrier(texture.image(),
      ui::vulkan::util::InitializeSubresourceRange(), stage,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, access, VK_ACCESS_SHADER_READ_BIT,
      layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  return texture.depth_surface_attachment_view;
}

void VulkanTextureCache::BeginDepthSurfaceTarget(
    const DepthSurfaceTarget& target) {
  auto& texture = *static_cast<VulkanTexture*>(target.texture);
  texture.depth_surface_pending_sample = false;
  VkPipelineStageFlags stage;
  VkAccessFlags access;
  VkImageLayout layout;
  GetTextureUsageMasks(texture.SetUsage(VulkanTexture::Usage::kDepthSurfaceAttachment),
                       stage, access, layout);
  command_processor_.PushImageMemoryBarrier(
      texture.image(), ui::vulkan::util::InitializeSubresourceRange(), stage,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, access,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, layout,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, false);
  // Only the provider's compute reads the stencil image; the draw rewrites
  // all of it, so the previous contents are discarded.
  if (texture.depth_surface_stencil_image == VK_NULL_HANDLE) return;
  command_processor_.PushImageMemoryBarrier(
      texture.depth_surface_stencil_image,
      ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, false);
}

void VulkanTextureCache::CompleteDepthSurfaceTarget(
    const DepthSurfaceTarget& target) {
  auto& texture = *static_cast<VulkanTexture*>(target.texture);
  VkPipelineStageFlags stage;
  VkAccessFlags access;
  VkImageLayout layout;
  GetTextureUsageMasks(VulkanTexture::Usage::kGuestShaderSampled,
                       stage, access, layout);
  stage = GetGuestSampledStages(false);
  texture.vertex_visible = stage == guest_shader_pipeline_stages_;
  command_processor_.PushImageMemoryBarrier(
      texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, stage,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, access,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layout);
  texture.SetUsage(VulkanTexture::Usage::kGuestShaderSampled);
  if (texture.depth_surface_stencil_image == VK_NULL_HANDLE) return;
  command_processor_.PushImageMemoryBarrier(
      texture.depth_surface_stencil_image,
      ui::vulkan::util::InitializeSubresourceRange(),
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkImageView VulkanTextureCache::GetDepthSurfaceStencilSource(
    const DepthSurfaceTarget& target) {
  const auto& texture = *static_cast<VulkanTexture*>(target.texture);
  // Color surfaces have none; any valid view fills the unused binding.
  return texture.depth_surface_stencil_view != VK_NULL_HANDLE
             ? texture.depth_surface_stencil_view
             : texture.depth_surface_attachment_view;
}

void VulkanTextureCache::PublishDepthSurfaceTarget(
    const DepthSurfaceTarget& target, const global_unique_lock_type& lock) {
  assert_true(lock.owns_lock());
  auto& texture = *static_cast<VulkanTexture*>(target.texture);
  // Aliases were invalidated before publishing the provider. Only the images
  // actually drawn here may accept surface-only validity.
  if (texture.MakeUpToDateAndWatch(lock, true)) {
    texture.set_loaded_once();
    texture.resolve_copy_pending_sample = true;
    texture.depth_surface_pending_sample = true;
  }
}

bool VulkanTextureCache::SkipsTopMip(const TextureKey& key) const {
  uint32_t min_size = cvars::texture_skip_top_mip_min_size;
  if (!min_size || !key.mip_max_level || !key.mip_page || key.scaled_resolve ||
      key.dimension == xenos::DataDimension::k3D ||
      std::max(key.GetWidth(), key.GetHeight()) < min_size) {
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (format_info->block_width < 4 || format_info->block_height < 4) {
    return false;
  }
  return !ShouldGenerateTextureMips(key);
}

bool VulkanTextureCache::ShouldGenerateTextureMips(
    const TextureKey& key) const {
  int32_t mode = cvars::vulkan_generate_texture_mips;
  if (mode <= 0 || !key.mip_page || !key.mip_max_level) {
    return false;
  }
  // Log once per format, dimension and reason why guest mips are used instead.
  auto reject = [&key](uint32_t reason) {
    static std::unordered_set<uint32_t> logged;
    uint32_t id = uint32_t(key.format) | (uint32_t(key.dimension) << 8) |
                  (reason << 12);
    if (logged.size() < 256 && logged.insert(id).second) {
      XELOGI("[TEXGEN] guest mips kept: reason={} format={} dimension={} "
             "{}x{} levels={} base_page={:05X}",
             reason, FormatInfo::GetName(key.format),
             uint32_t(key.dimension), key.GetWidth(), key.GetHeight(),
             uint32_t(key.mip_max_level), uint32_t(key.base_page));
    }
    return false;
  };
  if (key.scaled_resolve) {
    return reject(1);
  }
  // Textures without a base (the guest samples from mip 1, as streaming does
  // while the high resolution level isn't resident) generate from mip 1.
  if (key.mip_max_level < (key.base_page ? 1u : 2u)) {
    return reject(3);
  }
  if (key.dimension != xenos::DataDimension::k2DOrStacked &&
      key.dimension != xenos::DataDimension::kCube) {
    return reject(4);
  }
  const HostFormatPair& host_format_pair = GetHostFormatPair(key);
  if (mode == 1) {
    const FormatInfo* format_info = FormatInfo::Get(key.format);
    if (format_info->block_width <= 1 ||
        host_format_pair.format_unsigned.block_compressed ||
        host_format_pair.format_signed.block_compressed) {
      return false;
    }
  }
  // Every host format the image may be created with must be blittable with
  // linear filtering.
  for (VkFormat format : {host_format_pair.format_unsigned.format,
                          host_format_pair.format_signed.format}) {
    if (format == VK_FORMAT_UNDEFINED) {
      continue;
    }
    auto it = mip_generation_format_support_.find(format);
    if (it == mip_generation_format_support_.end()) {
      const ui::vulkan::VulkanDevice* const vulkan_device =
          command_processor_.GetVulkanDevice();
      VkFormatProperties format_properties;
      vulkan_device->vulkan_instance()
          ->functions()
          .vkGetPhysicalDeviceFormatProperties(vulkan_device->physical_device(),
                                               format, &format_properties);
      constexpr VkFormatFeatureFlags kRequired =
          VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      it = mip_generation_format_support_
               .emplace(format, (format_properties.optimalTilingFeatures &
                                 kRequired) == kRequired)
               .first;
    }
    if (!it->second) {
      return reject(6);
    }
  }
  return true;
}

void VulkanTextureCache::OnTextureDataRequest(Texture& texture,
                                              bool base_outdated,
                                              bool mips_outdated) {
  pass_head_load_allowed_ = false;
  // log_pass_head_texture_rejects: 0 no pass, 1 not in a pass (nothing to
  // break), 2 scaled, 3 not 2D, 4 3D tiling, 5 bound in this pass, 6 pages
  // used by the pass; 7 eligible (mip blits/debug may still refuse later).
  auto note = [this](uint32_t reason) {
    if (cvars::log_pass_head_texture_rejects > 0) {
      ++pass_head_verdicts_[reason];
    }
  };
  if (!cvars::vulkan_pass_head_texture_loads) {
    return;
  }
  if (!command_processor_.pass_head_command_buffer()) {
    note(command_processor_.in_render_pass() ? 0 : 1);
    return;
  }
  const VulkanTexture& vulkan_texture =
      static_cast<const VulkanTexture&>(texture);
  const TextureKey& key = vulkan_texture.key();
  if (key.scaled_resolve) {
    note(2);
    return;
  }
  if (key.dimension != xenos::DataDimension::k2DOrStacked) {
    note(3);
    return;
  }
  if (vulkan_texture.force_load_3d_tiling()) {
    note(4);
    return;
  }
  if (vulkan_texture.bound_render_pass_serial() ==
      current_render_pass_serial()) {
    note(5);
    return;
  }
  // Everything the load may read (the base and the mips, requested or not).
  std::pair<uint32_t, uint32_t> byte_ranges[2];
  uint32_t byte_range_count = 0;
  if (key.base_page && vulkan_texture.GetGuestBaseSize()) {
    byte_ranges[byte_range_count++] = std::make_pair(
        uint32_t(key.base_page) << 12,
        xe::align(vulkan_texture.GetGuestBaseSize(), UINT32_C(16)));
  }
  if (key.mip_page && vulkan_texture.GetGuestMipsSize()) {
    byte_ranges[byte_range_count++] = std::make_pair(
        uint32_t(key.mip_page) << 12,
        xe::align(vulkan_texture.GetGuestMipsSize(), UINT32_C(16)));
  }
  // Pages written (resolves, memexport) or read by the pass so far: the data
  // at the pass head may differ from what the load must see.
  // vulkan_pass_head_loads_ignore_reads: only pages whose contents changed in
  // the pass matter to a load; pages the pass merely read hold the same data
  // at its head. Rechecked after the load's own upload (LoadTextureData).
  if (cvars::vulkan_pass_head_loads_ignore_reads) {
    pass_head_byte_range_count_ = byte_range_count;
    std::copy(byte_ranges, byte_ranges + byte_range_count,
              pass_head_byte_ranges_);
    pass_head_load_allowed_ =
        byte_range_count && !shared_memory().AnyByteRangeWrittenInCurrentPass(
                                byte_ranges, byte_range_count);
  } else {
    pass_head_load_allowed_ =
        byte_range_count && !shared_memory().AnyByteRangeUsedInCurrentPass(
                                byte_ranges, byte_range_count);
  }
  note(pass_head_load_allowed_ ? 7 : 6);
  if (cvars::log_pass_head_texture_rejects > 0 &&
      ++pass_head_verdict_requests_ >=
          uint32_t(cvars::log_pass_head_texture_rejects)) {
    XELOGI("[TEXHEAD] requests={} no_pass_head_in_pass={} outside_pass={} "
           "scaled={} not_2d={} tiling_3d={} bound_in_pass={} pages_used={} "
           "eligible={} loaded_at_head={}",
           pass_head_verdict_requests_, pass_head_verdicts_[0],
           pass_head_verdicts_[1], pass_head_verdicts_[2],
           pass_head_verdicts_[3], pass_head_verdicts_[4],
           pass_head_verdicts_[5], pass_head_verdicts_[6],
           pass_head_verdicts_[7], pass_head_loads_total_);
    pass_head_verdicts_ = {};
    pass_head_verdict_requests_ = 0;
    pass_head_loads_total_ = 0;
  }
}

void VulkanTextureCache::LogInPassTextureLoad(const VulkanTexture& texture,
                                              bool load_base, bool load_mips) {
  struct Stat {
    uint32_t in_pass = 0;
    uint32_t total = 0;
    // Reloads of an already loaded texture whose source bytes are identical to
    // the previous load (invalidation without a data change).
    uint32_t same_source = 0;
    uint64_t total_bytes = 0;
    uint64_t in_pass_bytes = 0;
  };
  static std::unordered_map<std::string, Stat> stats;
  static uint32_t first_frame = UINT32_MAX;
  uint32_t frame = logging::GetFrameNumber();
  if (first_frame == UINT32_MAX) {
    first_frame = frame;
  }
  const TextureKey& key = texture.key();
  std::string name = fmt::format(
      "{}:{:08X} {}x{} {}{}{} {}", FormatInfo::GetName(key.format),
      key.base_page << 12, key.GetWidth(), key.GetHeight(),
      key.tiled ? "t" : "l", load_base ? "B" : "", load_mips ? "M" : "",
      !texture.loaded_once()
          ? "first"
          : (texture.last_invalidated_by_gpu() ? "gpu" : "cpu"));
  Stat& stat = stats[name];
  const uint64_t loaded_bytes =
      (load_base ? uint64_t(texture.GetGuestBaseSize()) : 0) +
      (load_mips ? uint64_t(texture.GetGuestMipsSize()) : 0);
  ++stat.total;
  stat.total_bytes += loaded_bytes;
  if (command_processor_.in_render_pass()) {
    ++stat.in_pass;
    stat.in_pass_bytes += loaded_bytes;
  }
  if (cvars::log_inpass_texture_source_hash) {
    uint32_t source_page = key.base_page ? key.base_page : key.mip_page;
    uint32_t source_size = key.base_page ? texture.GetGuestBaseSize()
                                         : texture.GetGuestMipsSize();
    if (source_page && source_size) {
      uint64_t source_hash = XXH3_64bits(
          shared_memory().guest_memory().TranslatePhysical<const uint8_t*>(
              source_page << 12),
          source_size);
      if (texture.loaded_once() &&
          source_hash == texture.inpass_log_source_hash) {
        ++stat.same_source;
      }
      texture.inpass_log_source_hash = source_hash;
    }
  }
  uint32_t frames = frame - first_frame;
  if (frames < cvars::log_inpass_texture_loads) {
    return;
  }
  std::vector<std::pair<std::string, Stat>> sorted(stats.begin(), stats.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return a.second.in_pass != b.second.in_pass
               ? a.second.in_pass > b.second.in_pass
               : a.second.total > b.second.total;
  });
  uint32_t in_pass = 0, total = 0;
  uint32_t total_first = 0, total_cpu = 0, total_gpu = 0, total_same = 0;
  uint64_t first_bytes = 0, cpu_bytes = 0, gpu_bytes = 0, in_pass_bytes = 0;
  for (const auto& entry : sorted) {
    in_pass += entry.second.in_pass;
    total += entry.second.total;
    total_same += entry.second.same_source;
    in_pass_bytes += entry.second.in_pass_bytes;
    const std::string& entry_name = entry.first;
    if (entry_name.size() >= 5 &&
        entry_name.compare(entry_name.size() - 5, 5, "first") == 0) {
      total_first += entry.second.total;
      first_bytes += entry.second.total_bytes;
    } else if (entry_name.size() >= 3 &&
               entry_name.compare(entry_name.size() - 3, 3, "gpu") == 0) {
      total_gpu += entry.second.total;
      gpu_bytes += entry.second.total_bytes;
    } else {
      total_cpu += entry.second.total;
      cpu_bytes += entry.second.total_bytes;
    }
  }
  constexpr double kBytesPerMiB = 1048576.0;
  std::string text = fmt::format(
      " | hash={} kinds: first={:.2f} cpu={:.2f} gpu={:.2f} "
      "same-source={:.2f} distinct={} bytes MiB/frame "
      "first={:.3f} cpu={:.3f} gpu={:.3f} in-pass={:.3f}",
      cvars::log_inpass_texture_source_hash ? "on" : "off",
      double(total_first) / frames, double(total_cpu) / frames,
      double(total_gpu) / frames, double(total_same) / frames, sorted.size(),
      double(first_bytes) / frames / kBytesPerMiB,
      double(cpu_bytes) / frames / kBytesPerMiB,
      double(gpu_bytes) / frames / kBytesPerMiB,
      double(in_pass_bytes) / frames / kBytesPerMiB);
  for (size_t i = 0; i < std::min(sorted.size(), size_t(24)); ++i) {
    text += fmt::format(
        " | {} in={:.2f} all={:.2f} same={:.2f} bytes-MiB/frame={:.3f} "
        "in-pass-MiB/frame={:.3f}",
        sorted[i].first,
        double(sorted[i].second.in_pass) / frames,
        double(sorted[i].second.total) / frames,
        double(sorted[i].second.same_source) / frames,
        double(sorted[i].second.total_bytes) / frames / kBytesPerMiB,
        double(sorted[i].second.in_pass_bytes) / frames / kBytesPerMiB);
  }
  XELOGI("TexLoadInPass: frames={} per-frame in-pass={:.2f} all={:.2f} "
         "moved-to-pass-head={:.2f}{}",
         frames, double(in_pass) / frames, double(total) / frames,
         double(pass_head_loads_) / frames, text);
  pass_head_loads_ = 0;
  stats.clear();
  first_frame = frame;
}

bool VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                                               bool load_base,
                                                               bool load_mips) {
  command_processor_.FlushDeferredResolve(
      VulkanCommandProcessor::ResolveDeferStop::kResource);

  VulkanTexture& vulkan_texture = static_cast<VulkanTexture&>(texture);
  TextureKey texture_key = vulkan_texture.key();
  vulkan_texture.resolve_copy_pending_sample = false;
  if (cvars::log_resolve_texture_copy_rejects && vulkan_texture.loaded_once() &&
      ResolveTextureBytesPerPixel(texture_key)) {
    // A resolve-capable texture loaded again: what the resolves at its base
    // did since the previous load.
    static uint32_t logged = 0;
    if (logged < 200) {
      ++logged;
      XELOGI("ResolveTextureReload: base {:08X} {}x{} fmt {} pitch {} "
             "resolves {} last {:02X} base_load {} mips_load {} "
             "range_valid {} in_surface {}",
             texture_key.base_page << 12, texture_key.GetWidth(),
             texture_key.GetHeight(), uint32_t(texture_key.format),
             uint32_t(texture_key.pitch) << 5,
             vulkan_texture.resolve_copy_count,
             vulkan_texture.resolve_copy_last, uint32_t(load_base),
             uint32_t(load_mips),
             uint32_t(shared_memory().IsRangeValid(
                 texture_key.base_page << 12,
                 vulkan_texture.GetGuestBaseSize())),
             uint32_t(shared_memory().IsRangeValidInSurface(
                 texture_key.base_page << 12,
                 vulkan_texture.GetGuestBaseSize())));
    }
  }
  vulkan_texture.resolve_copy_last = 0;
  vulkan_texture.resolve_copy_count = 0;
  vulkan_texture.depth_surface_pending_sample = false;
  if (cvars::log_texture_clobber) {
    // A texture reloaded from guest RAM over pages the GPU wrote earlier but
    // no longer owns: a CPU write invalidated a GPU-written (memexport or
    // resolve) page whose data never reached guest RAM.
    static uint32_t logged = 0;
    auto check = [&](bool load, uint32_t page, uint32_t size, char what) {
      if (!load || !page || !size || logged >= 64 || texture_key.scaled_resolve) {
        return;
      }
      bool history = false;
      bool ever = shared_memory().IsRangeEverGpuWritten(page << 12, size, &history);
      if (!history || !ever || shared_memory().IsRangeGpuWritten(page << 12, size)) {
        return;
      }
      ++logged;
      XELOGW("[TEXCLOB] {} {}x{} fmt={} tiled={} packed={} mml={} addr={:08X} "
             "size={} once={}",
             what, texture_key.GetWidth(), texture_key.GetHeight(),
             uint32_t(texture_key.format), uint32_t(texture_key.tiled),
             uint32_t(texture_key.packed_mips), uint32_t(texture_key.mip_max_level),
             page << 12, size, uint32_t(vulkan_texture.loaded_once()));
    };
    check(load_base, texture_key.base_page, vulkan_texture.GetGuestBaseSize(), 'B');
    check(load_mips, texture_key.mip_page, vulkan_texture.GetGuestMipsSize(), 'M');
  }
  if (cvars::log_inpass_texture_loads) {
    LogInPassTextureLoad(vulkan_texture, load_base, load_mips);
  }
  // Host-generated mips are derived from the base level, so the base is
  // (re)loaded whenever any part of the texture is outdated.
  bool generate_mips = ShouldGenerateTextureMips(texture_key);
  if (generate_mips) {
    if (texture_key.base_page) {
      if (!load_base) {
        // Not requested by the caller - the load still reads it.
        shared_memory().MarkRangeUsedInSubmission(
            texture_key.base_page << 12, vulkan_texture.GetGuestBaseSize());
      }
      load_base = true;
    } else {
      if (!load_mips) {
        shared_memory().MarkRangeUsedInSubmission(
            texture_key.mip_page << 12, vulkan_texture.GetGuestMipsSize());
      }
      load_mips = true;
    }
  }
  // Guest mip N lives in host level N - mip_skip; the guest base is not stored.
  const uint32_t mip_skip = SkipsTopMip(texture_key) ? 1 : 0;
  if (mip_skip) {
    if (!load_mips) {
      return true;
    }
    load_base = false;
  }
  if (cvars::vulkan_debug_texture_audit && !texture_key.scaled_resolve) {
    ++vulkan_texture.debug_loads;
    vulkan_texture.debug_last_load_submission =
        command_processor_.GetCurrentSubmission();
    // Hash what the texture's first stored level is loaded from.
    if (texture_key.base_page ? load_base : load_mips) {
      uint32_t source_page =
          texture_key.base_page ? texture_key.base_page : texture_key.mip_page;
      uint32_t source_size = texture_key.base_page
                                 ? vulkan_texture.GetGuestBaseSize()
                                 : vulkan_texture.GetGuestMipsSize();
      vulkan_texture.debug_source_hash = XXH3_64bits(
          shared_memory().guest_memory().TranslatePhysical<const uint8_t*>(
              source_page << 12),
          source_size);
      vulkan_texture.debug_source_hashed = true;
    }
  }

  if (cvars::log_render_pass_breaks > 0 && !texture_key.scaled_resolve) {
    // Diagnostic: textures reloaded frame after frame, and whether their
    // source bytes actually changed.
    static uint32_t texload_logged = 0;
    if (texload_logged < 20000) {
      ++texload_logged;
      uint32_t source_page =
          texture_key.base_page ? texture_key.base_page : texture_key.mip_page;
      uint32_t source_size = texture_key.base_page
                                 ? vulkan_texture.GetGuestBaseSize()
                                 : vulkan_texture.GetGuestMipsSize();
      XELOGI("[TEXLOAD] base=0x{:08X} mip=0x{:08X} {}x{} fmt={} b={} m={} "
             "size={} hash={:016X}",
             texture_key.base_page << 12, texture_key.mip_page << 12,
             texture_key.GetWidth(), texture_key.GetHeight(),
             uint32_t(texture_key.format), load_base ? 1 : 0,
             load_mips ? 1 : 0, source_size,
             XXH3_64bits(shared_memory()
                             .guest_memory()
                             .TranslatePhysical<const uint8_t*>(source_page
                                                                << 12),
                         source_size));
    }
  }

  // Get the pipeline.
  const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
  bool host_format_is_signed;
  if (IsSignedVersionSeparateForFormat(texture_key)) {
    host_format_is_signed = bool(texture_key.signed_separate);
  } else {
    host_format_is_signed =
        host_format_pair.format_unsigned.load_shader == kLoadShaderIndexUnknown;
  }
  const HostFormat& host_format = host_format_is_signed
                                      ? host_format_pair.format_signed
                                      : host_format_pair.format_unsigned;
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }
  VkPipelineLayout load_pipeline_layout = load_pipeline_layout_;
  VkPipeline pipeline =
      texture_key.scaled_resolve
          ? load_pipelines_scaled_[load_shader]
          : load_pipelines_[load_shader];
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }
  VulkanSharedMemory& vulkan_shared_memory =
      static_cast<VulkanSharedMemory&>(shared_memory());
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  // Memexport-generated textures have their source in the host-imported buffer
  // (guest RAM); the load below reads the device buffer, so copy the sampled
  // range across first. A no-op for normal textures and non-memexport ranges.
  // Scaled-resolve textures read from separate scaled buffers, not shared
  // memory, so they are unaffected.
  if (!texture_key.scaled_resolve) {
    if (load_base) {
      command_processor_.EnsureMemexportRangeInDeviceBuffer(
          texture_key.base_page << 12, vulkan_texture.GetGuestBaseSize());
    }
    if (load_mips) {
      command_processor_.EnsureMemexportRangeInDeviceBuffer(
          texture_key.mip_page << 12, vulkan_texture.GetGuestMipsSize());
    }
  }

  // Get the guest layout.
  const texture_util::TextureGuestLayout& guest_layout =
      vulkan_texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  // Whether the host image is 3D (determines depth vs array layer layout).
  bool is_3d = dimension == xenos::DataDimension::k3D;
  // Whether to use 3D tiling when reading from guest memory.
  // For 3D-as-2D wrappers, the host image is 2D but we need 3D tiling.
  bool is_3d_tiling = is_3d || vulkan_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  // Load the guest's resolved mips from the scaled buffer, else generate them.
  uint32_t level_last_for_blit_gen = 0;
  if (level_last > 0 && ScaledResolveMipsNeedGeneration(texture)) {
    level_last_for_blit_gen = level_last;
    level_last = 0;  // Only load base level from buffer
  } else if (generate_mips) {
    if (texture_key.base_page) {
      level_last_for_blit_gen = texture_key.mip_max_level;
      level_last = 0;
    } else if (vulkan_texture.guest_layout().packed_level > 1) {
      // Only mip 1 is loaded from guest memory, the rest is derived from it.
      level_last_for_blit_gen = texture_key.mip_max_level;
      level_last = 1;
      static uint32_t mips_only_logged = 0;
      if (mips_only_logged < 40) {
        ++mips_only_logged;
        XELOGI("[TEXGEN] mips-only texture {}x{} {} mip_page={:05X} levels "
               "1-{}: loading mip 1, generating the rest",
               texture_key.GetWidth(), texture_key.GetHeight(),
               FormatInfo::GetName(texture_key.format),
               uint32_t(texture_key.mip_page), texture_key.mip_max_level);
      }
    } else {
      // Mip 1 is in the packed tail - load the guest mips as usual.
      generate_mips = false;
      static uint32_t packed_logged = 0;
      if (packed_logged < 20) {
        ++packed_logged;
        XELOGI("[TEXGEN] guest mips kept: reason=7 (mips-only, mip 1 packed) "
               "format={} {}x{} levels={}",
               FormatInfo::GetName(texture_key.format), texture_key.GetWidth(),
               texture_key.GetHeight(), uint32_t(texture_key.mip_max_level));
      }
    }
  }
  // The first level generated by blitting from the level above it.
  const uint32_t blit_gen_first_level = level_last + 1;
  assert_true(level_first <= level_last || level_last_for_blit_gen > 0);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t texture_resolution_scale_x =
      texture_key.scaled_resolve ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y =
      texture_key.scaled_resolve ? draw_resolution_scale_y() : 1;

  // The loop counter can mean two things depending on whether the packed mip
  // tail is stored as mip 0, because in this case, it would be ambiguous since
  // both the base and the mips would be on "level 0", but stored in separate
  // places.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Packed mip tail is the level 0 - may need to load mip tails for the base,
    // the mips, or both.
    // Loop iteration 0 - base packed mip tail.
    // Loop iteration 1 - mips packed mip tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    // Packed mip tail is not the level 0.
    // Loop iteration is the actual level being loaded.
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // Get the host layout and the buffer.
  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread =
      UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  VkDeviceSize host_buffer_size = 0;
  struct HostLayout {
    VkDeviceSize offset_bytes;
    VkDeviceSize slice_size_bytes;
    uint32_t x_pitch_blocks;
    uint32_t y_pitch_blocks;
  };
  HostLayout host_layout_base;
  // Indexing is the same as for guest stored mips:
  // 1...min(level_last, level_packed) if level_packed is not 0, or only 0 if
  // level_packed == 0.
  HostLayout host_layout_mips[xenos::kTextureMaxMips];
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    HostLayout& level_host_layout =
        is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_guest_x_extent_texels_unscaled;
    uint32_t level_guest_y_extent_texels_unscaled;
    uint32_t level_guest_z_extent_texels;
    if (level == level_packed) {
      // Loading the packed tail for the base or the mips - load the whole tail
      // to copy regions out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_guest_x_extent_texels_unscaled =
          guest_layout_packed.x_extent_blocks * block_width;
      level_guest_y_extent_texels_unscaled =
          guest_layout_packed.y_extent_blocks * block_height;
      level_guest_z_extent_texels = guest_layout_packed.z_extent;
    } else {
      level_guest_x_extent_texels_unscaled =
          std::max(width >> level, UINT32_C(1));
      level_guest_y_extent_texels_unscaled =
          std::max(height >> level, UINT32_C(1));
      level_guest_z_extent_texels = std::max(depth >> level, UINT32_C(1));
    }
    level_host_layout.x_pitch_blocks = xe::round_up(
        (level_guest_x_extent_texels_unscaled * texture_resolution_scale_x +
         (host_block_width - 1)) /
            host_block_width,
        host_x_blocks_per_thread);
    level_host_layout.y_pitch_blocks =
        (level_guest_y_extent_texels_unscaled * texture_resolution_scale_y +
         (host_block_height - 1)) /
        host_block_height;
    level_host_layout.slice_size_bytes =
        VkDeviceSize(load_shader_info.bytes_per_host_block) *
        level_host_layout.x_pitch_blocks * level_host_layout.y_pitch_blocks *
        level_guest_z_extent_texels;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }
  // Inside an open render pass, record the whole load in the pass head slot
  // (replayed before the pass begins) instead of ending the pass. Every barrier
  // goes straight into that slot - a pending barrier would end the pass at the
  // next draw - and the texture ends in its sampled state, so the draw needs
  // no barrier either. Simple single-path loads only.
  DeferredCommandBuffer* pass_head_command_buffer = nullptr;
  // An upload of these pages in the pass body (they were read earlier in the
  // pass) must stay before the load.
  if (pass_head_load_allowed_ && cvars::vulkan_pass_head_loads_ignore_reads &&
      shared_memory().AnyByteRangeWrittenInCurrentPass(
          pass_head_byte_ranges_, pass_head_byte_range_count_)) {
    pass_head_load_allowed_ = false;
  }
  // Mip blits record their level barriers in the same command buffer, so at
  // the head they end nothing either (vulkan_pass_head_mip_blits).
  if (pass_head_load_allowed_ &&
      (level_last_for_blit_gen == 0 || cvars::vulkan_pass_head_mip_blits) &&
      !texture_key.scaled_resolve && !cvars::vulkan_debug_texture_dump &&
      !cvars::vulkan_debug_texture_audit) {
    pass_head_command_buffer = command_processor_.pass_head_command_buffer();
  }
  pass_head_load_allowed_ = false;

  VulkanCommandProcessor::ScratchBufferAcquisition scratch_buffer_acquisition(
      command_processor_.AcquireScratchGpuBuffer(
          host_buffer_size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT, !pass_head_command_buffer));
  VkBuffer scratch_buffer = scratch_buffer_acquisition.buffer();
  if (scratch_buffer == VK_NULL_HANDLE) {
    return false;
  }

  // Begin loading.
  // TODO(Triang3l): Going from one descriptor to another on per-array-layer
  // or even per-8-depth-slices level to stay within maxStorageBufferRange.
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Bind the whole shared memory buffer persistently when possible (passing the
  // texture's byte offset via guest_offset) instead of allocating and writing
  // per-load source descriptors. Scaled resolve textures read from separate
  // scaled buffers, so they always use transient descriptors.
  const bool use_persistent_source =
      cvars::vulkan_texture_load_persistent_source &&
      shared_memory_persistent_descriptor_set_ != VK_NULL_HANDLE &&
      !texture_key.scaled_resolve;
  std::array<VkWriteDescriptorSet, 3> write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  VkDescriptorSet descriptor_set_dest =
      command_processor_.AllocateSingleTransientDescriptor(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::
              kStorageBufferCompute);
  if (!descriptor_set_dest) {
    return false;
  }
  VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;
  {
    write_descriptor_set_dest_buffer_info.buffer = scratch_buffer;
    write_descriptor_set_dest_buffer_info.offset = 0;
    write_descriptor_set_dest_buffer_info.range = host_buffer_size;
    VkWriteDescriptorSet& write_descriptor_set_dest =
        write_descriptor_sets[write_descriptor_set_count++];
    write_descriptor_set_dest.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
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
  }
  // TODO(Triang3l): Use a single 512 MB shared memory binding if possible.
  VkDescriptorSet descriptor_set_source_base = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_source_mips = VK_NULL_HANDLE;
  VkDescriptorBufferInfo write_descriptor_set_source_base_buffer_info;
  VkDescriptorBufferInfo write_descriptor_set_source_mips_buffer_info;
  if (level_first == 0) {
    if (use_persistent_source) {
      descriptor_set_source_base = shared_memory_persistent_descriptor_set_;
    } else {
      descriptor_set_source_base =
          command_processor_.AllocateSingleTransientDescriptor(
              VulkanCommandProcessor::SingleTransientDescriptorLayout ::
                  kStorageBufferCompute);
      if (!descriptor_set_source_base) {
        return false;
      }
    }
    if (texture_key.scaled_resolve) {
      // For scaled textures, read from scaled resolve buffers
      uint32_t guest_address = texture_key.base_page << 12;
      uint32_t guest_size = vulkan_texture.GetGuestBaseSize();

      // Ensure the scaled buffer exists
      if (EnsureScaledResolveMemoryCommitted(guest_address, guest_size)) {
        // Make the range current
        if (MakeScaledResolveRangeCurrent(guest_address, guest_size)) {
          VkBuffer scaled_buffer = GetCurrentScaledResolveBuffer();
          if (scaled_buffer != VK_NULL_HANDLE) {
            // Calculate offset within the scaled buffer
            uint32_t draw_resolution_scale_area =
                draw_resolution_scale_x() * draw_resolution_scale_y();
            uint64_t scaled_offset =
                uint64_t(guest_address) * draw_resolution_scale_area;
            uint64_t buffer_relative_offset =
                scaled_offset - GetCurrentScaledResolveBufferBaseOffset();

            write_descriptor_set_source_base_buffer_info.buffer = scaled_buffer;
            write_descriptor_set_source_base_buffer_info.offset =
                buffer_relative_offset;
            // Align because shaders use up to 16-byte loads for multiple
            // blocks at once.
            write_descriptor_set_source_base_buffer_info.range = xe::align(
                guest_size * draw_resolution_scale_area, uint32_t(16));
          } else {
            XELOGE(
                "Scaled resolve texture load: Failed to get current scaled "
                "buffer for texture at 0x{:08X}",
                guest_address);
            return false;
          }
        } else {
          XELOGE(
              "Scaled resolve texture load: Failed to make range current for "
              "texture at 0x{:08X}",
              guest_address);
          return false;
        }
      } else {
        XELOGE(
            "Scaled resolve texture load: Failed to ensure scaled memory for "
            "texture at 0x{:08X}",
            guest_address);
        return false;
      }
    } else if (!use_persistent_source) {
      // Regular unscaled texture - use shared memory
      const uint32_t guest_address = texture_key.base_page << 12;
      const uint32_t guest_size = vulkan_texture.GetGuestBaseSize();
      const VulkanSharedMemory::BufferSliceList source_slices =
          vulkan_shared_memory.GetBufferSlices(guest_address, guest_size);
      if (source_slices.count != 1) {
        return false;
      }
      write_descriptor_set_source_base_buffer_info.buffer =
          source_slices.slices[0].buffer;
      write_descriptor_set_source_base_buffer_info.offset =
          source_slices.slices[0].buffer_offset;
      // Align (primarily the last row of linear textures) because shaders use
      // up to 16-byte loads for multiple blocks at once.
      write_descriptor_set_source_base_buffer_info.range =
          std::min<VkDeviceSize>(
              xe::align<VkDeviceSize>(guest_size, 16),
              SharedMemory::kBufferSize - guest_address);
    }
    if (!use_persistent_source) {
      VkWriteDescriptorSet& write_descriptor_set_source_base =
          write_descriptor_sets[write_descriptor_set_count++];
      write_descriptor_set_source_base.sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_descriptor_set_source_base.pNext = nullptr;
      write_descriptor_set_source_base.dstSet = descriptor_set_source_base;
      write_descriptor_set_source_base.dstBinding = 0;
      write_descriptor_set_source_base.dstArrayElement = 0;
      write_descriptor_set_source_base.descriptorCount = 1;
      write_descriptor_set_source_base.descriptorType =
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_descriptor_set_source_base.pImageInfo = nullptr;
      write_descriptor_set_source_base.pBufferInfo =
          &write_descriptor_set_source_base_buffer_info;
      write_descriptor_set_source_base.pTexelBufferView = nullptr;
    }
  }
  // Set up the mips source: scaled resolve buffer or shared memory.
  if (level_last != 0) {
    if (use_persistent_source) {
      descriptor_set_source_mips = shared_memory_persistent_descriptor_set_;
    } else {
      descriptor_set_source_mips =
          command_processor_.AllocateSingleTransientDescriptor(
              VulkanCommandProcessor::SingleTransientDescriptorLayout ::
                  kStorageBufferCompute);
      if (!descriptor_set_source_mips) {
        return false;
      }
      if (texture_key.scaled_resolve) {
        // Scaled resolved mips live in the scaled buffer, like the base.
        uint32_t guest_address = texture_key.mip_page << 12;
        uint32_t guest_size = vulkan_texture.GetGuestMipsSize();
        if (EnsureScaledResolveMemoryCommitted(guest_address, guest_size) &&
            MakeScaledResolveRangeCurrent(guest_address, guest_size)) {
          VkBuffer scaled_buffer = GetCurrentScaledResolveBuffer();
          if (scaled_buffer != VK_NULL_HANDLE) {
            uint32_t draw_resolution_scale_area =
                draw_resolution_scale_x() * draw_resolution_scale_y();
            uint64_t scaled_offset =
                uint64_t(guest_address) * draw_resolution_scale_area;
            uint64_t buffer_relative_offset =
                scaled_offset - GetCurrentScaledResolveBufferBaseOffset();
            write_descriptor_set_source_mips_buffer_info.buffer = scaled_buffer;
            write_descriptor_set_source_mips_buffer_info.offset =
                buffer_relative_offset;
            write_descriptor_set_source_mips_buffer_info.range = xe::align(
                guest_size * draw_resolution_scale_area, uint32_t(16));
          } else {
            XELOGE(
                "Scaled resolve texture load: Failed to get current scaled "
                "buffer for mips at 0x{:08X}",
                guest_address);
            return false;
          }
        } else {
          XELOGE(
              "Scaled resolve texture load: Failed to make range current for "
              "mips at 0x{:08X}",
              guest_address);
          return false;
        }
      } else {
        // Regular unscaled texture - use shared memory
        const VulkanSharedMemory::BufferSliceList source_slices =
            vulkan_shared_memory.GetBufferSlices(
                texture_key.mip_page << 12,
                vulkan_texture.GetGuestMipsSize());
        if (source_slices.count != 1) {
          return false;
        }
        write_descriptor_set_source_mips_buffer_info.buffer =
            source_slices.slices[0].buffer;
        write_descriptor_set_source_mips_buffer_info.offset =
            texture_key.mip_page << 12;
        // Align (primarily the last row of a linear packed mip tail) because
        // shaders use up to 16-byte loads for multiple blocks at once.
        write_descriptor_set_source_mips_buffer_info.range =
            xe::align(vulkan_texture.GetGuestMipsSize(), uint32_t(16));
      }
      VkWriteDescriptorSet& write_descriptor_set_source_mips =
          write_descriptor_sets[write_descriptor_set_count++];
      write_descriptor_set_source_mips.sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_descriptor_set_source_mips.pNext = nullptr;
      write_descriptor_set_source_mips.dstSet = descriptor_set_source_mips;
      write_descriptor_set_source_mips.dstBinding = 0;
      write_descriptor_set_source_mips.dstArrayElement = 0;
      write_descriptor_set_source_mips.descriptorCount = 1;
      write_descriptor_set_source_mips.descriptorType =
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_descriptor_set_source_mips.pImageInfo = nullptr;
      write_descriptor_set_source_mips.pBufferInfo =
          &write_descriptor_set_source_mips_buffer_info;
      write_descriptor_set_source_mips.pTexelBufferView = nullptr;
    }
  }
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,
                               write_descriptor_sets.data(), 0, nullptr);
  }
  if (!pass_head_command_buffer) {
    vulkan_shared_memory.Use(VulkanSharedMemory::Usage::kRead);
  }

  // Submit the copy buffer population commands.

  DeferredCommandBuffer& command_buffer =
      pass_head_command_buffer ? *pass_head_command_buffer
                               : command_processor_.deferred_command_buffer();
  if (pass_head_command_buffer) {
    ++pass_head_loads_total_;
    ++pass_head_loads_;
    // The shared memory writes (uploads, resolves) before the pass, and the
    // previous users of the scratch buffer, before the load shader.
    VkMemoryBarrier head_barrier;
    head_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    head_barrier.pNext = nullptr;
    head_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    head_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    command_buffer.CmdVkPipelineBarrier(
        command_processor_.GetPassHeadSourceStages(),
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &head_barrier, 0, nullptr,
        0, nullptr);
  }

  // Debug annotation for texture load operation
  command_processor_.PushDebugMarker(
      "Texture Load: %ux%ux%u fmt:%s signed:%d scaled:%d", width, height,
      depth_or_array_size, FormatInfo::GetName(guest_format),
      host_format_is_signed ? 1 : 0, texture_key.scaled_resolve ? 1 : 0);

  // Opened before the first dispatch, once any pass the load ends has ended.
  uint32_t timed_section = UINT32_MAX;
  bool timed_section_open = false;
  command_processor_.BindExternalComputePipelineInto(command_buffer, pipeline);

  command_buffer.CmdVkBindDescriptorSets(
      VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_layout,
      kLoadDescriptorSetIndexDestination, 1, &descriptor_set_dest, 0, nullptr);

  VkDescriptorSet descriptor_set_source_current = VK_NULL_HANDLE;

  LoadConstants load_constants;
  // 3 bits for each.
  assert_true(texture_resolution_scale_x <= 7);
  assert_true(texture_resolution_scale_y <= 7);
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) |
      (texture_resolution_scale_x << 4) | (texture_resolution_scale_y << 7);

  uint32_t guest_x_blocks_per_group_log2 =
      load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;

    VkDescriptorSet descriptor_set_source =
        is_base ? descriptor_set_source_base : descriptor_set_source_mips;
    if (descriptor_set_source_current != descriptor_set_source) {
      descriptor_set_source_current = descriptor_set_source;
      command_buffer.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE, load_pipeline_layout,
          kLoadDescriptorSetIndexSource, 1, &descriptor_set_source, 0, nullptr);
    }

    // With the whole buffer bound persistently, guest_offset is relative to the
    // buffer origin; with a per-load source descriptor, it's already offset to
    // the texture's base or mip page, so it stays relative to that.
    load_constants.guest_offset =
        use_persistent_source
            ? ((is_base ? texture_key.base_page : texture_key.mip_page) << 12)
            : 0;
    if (!is_base) {
      load_constants.guest_offset +=
          guest_layout.mip_offsets_bytes[level] *
          (texture_resolution_scale_x * texture_resolution_scale_y);
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    load_constants.guest_pitch_aligned =
        level_guest_layout.row_pitch_bytes / bytes_per_block;
    load_constants.guest_z_stride_block_rows_aligned =
        level_guest_layout.z_slice_stride_block_rows;
    assert_true(dimension != xenos::DataDimension::k3D ||
                !(load_constants.guest_z_stride_block_rows_aligned &
                  (xenos::kTextureTileWidthHeight - 1)));

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      // This is the packed mip tail, containing not only the specified level,
      // but also other levels at different offsets - load the entire needed
      // extents.
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, UINT32_C(1));
      level_height = std::max(height >> level, UINT32_C(1));
      level_depth = std::max(depth >> level, UINT32_C(1));
    }
    load_constants.size_blocks[0] = (level_width + (block_width - 1)) /
                                    block_width * texture_resolution_scale_x;
    load_constants.size_blocks[1] = (level_height + (block_height - 1)) /
                                    block_height * texture_resolution_scale_y;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants.size_blocks[0] +
         ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] +
         ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    // TODO(Triang3l): host_offset relative to the storage buffer origin.
    const HostLayout& level_host_layout =
        is_base ? host_layout_base : host_layout_mips[level];
    load_constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    load_constants.host_pitch = load_shader_info.bytes_per_host_block *
                                level_host_layout.x_pitch_blocks;

    command_buffer.CmdVkPushConstants(load_pipeline_layout,
                                      VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(load_constants), &load_constants);

    uint32_t level_array_slice_stride_bytes_scaled =
        level_guest_layout.array_slice_stride_bytes *
        (texture_resolution_scale_x * texture_resolution_scale_y);
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (slice != 0) {
        command_buffer.CmdVkPushConstants(
            load_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            offsetof(LoadConstants, guest_offset),
            sizeof(load_constants.guest_offset), &load_constants.guest_offset);
        command_buffer.CmdVkPushConstants(
            load_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            offsetof(LoadConstants, host_offset),
            sizeof(load_constants.host_offset), &load_constants.host_offset);
      }
      if (!pass_head_command_buffer) {
        command_processor_.SubmitBarriers(true);
      }
      if (!timed_section_open) {
        timed_section_open = true;
        timed_section = command_processor_.BeginTimedSection(
            command_buffer,
            !vulkan_texture.loaded_once()
                ? 1
                : (vulkan_texture.last_invalidated_by_gpu() ? 3 : 2),
            uint32_t(guest_format));
      }
      // Debug: show dispatch info with source offset
      command_processor_.InsertDebugMarker(
          "Dispatch: guest_off=0x%X host_off=0x%X groups=%ux%ux%u",
          load_constants.guest_offset, load_constants.host_offset,
          group_count_x, group_count_y, load_constants.size_blocks[2]);
      if (cvars::log_texture_clobber && !texture_key.scaled_resolve) {
        static uint32_t logged = 0;
        const uint32_t source =
            (is_base ? texture_key.base_page : texture_key.mip_page) << 12;
        const uint32_t size = is_base ? vulkan_texture.GetGuestBaseSize()
                                     : vulkan_texture.GetGuestMipsSize();
        if (source < 0x17100000 && uint64_t(source) + size > 0x16F30000 &&
            logged++ < 4096) {
          XELOGI("[POOL-L] s={} cb={:X} pos={} place={} tex={:X} part={} "
                 "src={:08X}+{} level={} fmt={} gpu={}",
                 command_processor_.GetCurrentSubmission(),
                 reinterpret_cast<uintptr_t>(&command_buffer),
                 command_buffer.size_elements(),
                 pass_head_command_buffer ? "HEAD" : "BODY",
                 reinterpret_cast<uintptr_t>(&texture), is_base ? "B" : "M",
                 source, size, level, uint32_t(texture_key.format),
                 shared_memory().IsRangeGpuWritten(source, size));
        }
      }
      if (command_buffer.pass_bytes_enabled()) {
        // Byte ledger: the load reads guest memory into this texture and
        // touches no render target, so it must not end their provenance.
        PassByteLedger::Event tag;
        tag.kind = PassByteLedger::kTag;
        tag.category = PassByteLedger::kCopy;
        command_buffer.AnnotatePassBytes(tag);
      }
      command_buffer.CmdVkDispatch(group_count_x, group_count_y,
                                   load_constants.size_blocks[2],
                                   DeferredCommandBuffer::DispatchSite::kTextureLoad);
      load_constants.guest_offset += level_array_slice_stride_bytes_scaled;
      load_constants.host_offset +=
          uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  // Submit copying from the copy buffer to the host texture.
  if (pass_head_command_buffer) {
    VkPipelineStageFlags scratch_src_stage_mask =
        scratch_buffer_acquisition.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkAccessFlags scratch_src_access_mask =
        scratch_buffer_acquisition.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferMemoryBarrier scratch_barrier;
    scratch_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    scratch_barrier.pNext = nullptr;
    scratch_barrier.srcAccessMask = scratch_src_access_mask;
    scratch_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    scratch_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    scratch_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    scratch_barrier.buffer = scratch_buffer;
    scratch_barrier.offset = 0;
    scratch_barrier.size = VK_WHOLE_SIZE;
    vulkan_texture.MarkAsUsed();
    VulkanTexture::Usage head_old_usage =
        vulkan_texture.SetUsage(VulkanTexture::Usage::kTransferDestination);
    VkPipelineStageFlags image_src_stage_mask;
    VkAccessFlags image_src_access_mask;
    VkImageLayout image_old_layout;
    GetTextureUsageMasks(head_old_usage, image_src_stage_mask,
                         image_src_access_mask, image_old_layout);
    VkImageMemoryBarrier image_barrier;
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.pNext = nullptr;
    image_barrier.srcAccessMask = image_src_access_mask;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.oldLayout = image_old_layout;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = vulkan_texture.image();
    image_barrier.subresourceRange =
        ui::vulkan::util::InitializeSubresourceRange();
    command_buffer.CmdVkPipelineBarrier(
        scratch_src_stage_mask | image_src_stage_mask,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &scratch_barrier, 1,
        &image_barrier);
  } else {
  command_processor_.PushBufferMemoryBarrier(
      scratch_buffer, 0, VK_WHOLE_SIZE,
      scratch_buffer_acquisition.SetStageMask(VK_PIPELINE_STAGE_TRANSFER_BIT),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      scratch_buffer_acquisition.SetAccessMask(VK_ACCESS_TRANSFER_READ_BIT),
      VK_ACCESS_TRANSFER_READ_BIT);
  vulkan_texture.MarkAsUsed();
  VulkanTexture::Usage texture_old_usage =
      vulkan_texture.SetUsage(VulkanTexture::Usage::kTransferDestination);
  if (texture_old_usage != VulkanTexture::Usage::kTransferDestination) {
    VkPipelineStageFlags texture_src_stage_mask, texture_dst_stage_mask;
    VkAccessFlags texture_src_access_mask, texture_dst_access_mask;
    VkImageLayout texture_old_layout, texture_new_layout;
    GetTextureUsageMasks(texture_old_usage, texture_src_stage_mask,
                         texture_src_access_mask, texture_old_layout);
    GetTextureUsageMasks(VulkanTexture::Usage::kTransferDestination,
                         texture_dst_stage_mask, texture_dst_access_mask,
                         texture_new_layout);
    command_processor_.PushImageMemoryBarrier(
        vulkan_texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
        texture_src_stage_mask, texture_dst_stage_mask, texture_src_access_mask,
        texture_dst_access_mask, texture_old_layout, texture_new_layout);
  } else {
    // Re-uploading a texture already written by a transfer in this submission
    // without being sampled in between (the guest modified it again): the
    // previous copy's writes must be made available before writing the image
    // again, or the copies race (WRITE_AFTER_WRITE hazard reported by
    // synchronization validation). skip_if_equal must be false - this barrier
    // is intentionally all-equal and would otherwise be elided.
    command_processor_.PushImageMemoryBarrier(
        vulkan_texture.image(), ui::vulkan::util::InitializeSubresourceRange(),
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
  }
  command_processor_.SubmitBarriers(true);
  }
  command_processor_.InsertDebugMarker(
      "Texture Upload: %ux%ux%u fmt:%s mips:%u-%u", width, height,
      depth_or_array_size, FormatInfo::GetName(guest_format), level_first,
      level_last);
  VkBufferImageCopy* copy_regions = command_buffer.CmdCopyBufferToImageEmplace(
      scratch_buffer, vulkan_texture.image(),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level_last - level_first + 1);
  for (uint32_t level = level_first; level <= level_last; ++level) {
    VkBufferImageCopy& copy_region = copy_regions[level - level_first];
    const HostLayout& level_host_layout =
        level != 0 ? host_layout_mips[std::min(level, level_packed)]
                   : host_layout_base;
    copy_region.bufferOffset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format,
                                       level, level_offset_blocks_x,
                                       level_offset_blocks_y, level_offset_z);
      uint32_t level_offset_host_blocks_x =
          texture_resolution_scale_x * level_offset_blocks_x;
      uint32_t level_offset_host_blocks_y =
          texture_resolution_scale_y * level_offset_blocks_y;
      if (!host_format.block_compressed) {
        level_offset_host_blocks_x *= block_width;
        level_offset_host_blocks_y *= block_height;
      }
      copy_region.bufferOffset +=
          load_shader_info.bytes_per_host_block *
          (level_offset_host_blocks_x +
           level_host_layout.x_pitch_blocks *
               (level_offset_host_blocks_y + level_host_layout.y_pitch_blocks *
                                                 VkDeviceSize(level_offset_z)));
    }
    copy_region.bufferRowLength =
        level_host_layout.x_pitch_blocks * host_block_width;
    copy_region.bufferImageHeight =
        level_host_layout.y_pitch_blocks * host_block_height;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = level - mip_skip;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = array_size;
    copy_region.imageOffset.x = 0;
    copy_region.imageOffset.y = 0;
    copy_region.imageOffset.z = 0;
    // The image mip is scale-then-reduce (max((dim*scale)>>level,1)) while the
    // buffer footprint (bufferRowLength/bufferImageHeight) is
    // reduce-then-scale, so for the deepest mips of scaled textures the mip can
    // be a row or column larger than the buffer holds. Clamp the copy extent to
    // the footprint so the GPU never reads past the buffer.
    copy_region.imageExtent.width = std::min(
        std::max((width * texture_resolution_scale_x) >> level, UINT32_C(1)),
        copy_region.bufferRowLength);
    copy_region.imageExtent.height = std::min(
        std::max((height * texture_resolution_scale_y) >> level, UINT32_C(1)),
        copy_region.bufferImageHeight);
    copy_region.imageExtent.depth = std::max(depth >> level, UINT32_C(1));
  }

  if (cvars::vulkan_debug_texture_dump && !debug_texture_dumps_written_ &&
      !texture_key.scaled_resolve) {
    // Diagnostic: record what the load read and produced, to tell whether a
    // wrong mip comes from the guest data, the shared memory copy or the load.
    DebugTextureDump* existing_dump = nullptr;
    for (DebugTextureDump& dump : debug_texture_dumps_) {
      if (dump.key == texture_key) {
        existing_dump = &dump;
        break;
      }
    }
    uint32_t dump_min_size =
        uint32_t(std::max(cvars::vulkan_debug_texture_dump_min_size, 1));
    if (existing_dump) {
      ++existing_dump->later_loads;
      if (existing_dump->later_load_kinds.size() < 64) {
        existing_dump->later_load_kinds +=
            load_base ? (load_mips ? "B" : "b") : "m";
      }
    } else if ((load_mips || level_last_for_blit_gen) &&
               (level_last >= 1 || level_last_for_blit_gen) &&
               block_width > 1 &&
               !host_format.block_compressed &&
               dimension == xenos::DataDimension::k2DOrStacked &&
               width >= dump_min_size && height >= dump_min_size &&
               debug_texture_dumps_.size() <
                   size_t(std::max(cvars::vulkan_debug_texture_dump_max, 0)) &&
               debug_texture_dump_bytes_ < (VkDeviceSize(256) << 20)) {
      DebugTextureDump dump;
      dump.key = texture_key;
      dump.scratch_size = host_buffer_size;
      dump.shared_base_size = load_base ? vulkan_texture.GetGuestBaseSize() : 0;
      dump.shared_mips_size = vulkan_texture.GetGuestMipsSize();
      VkDeviceSize readback_size =
          dump.scratch_size + dump.shared_base_size + dump.shared_mips_size;
      if (ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback, dump.readback_buffer,
              dump.readback_memory, &dump.readback_memory_type,
              &dump.readback_memory_size)) {
        Memory& memory = shared_memory().guest_memory();
        if (dump.shared_base_size) {
          const uint8_t* base_bytes = memory.TranslatePhysical<const uint8_t*>(
              texture_key.base_page << 12);
          dump.cpu_base_at_load.assign(base_bytes,
                                       base_bytes + dump.shared_base_size);
        }
        const uint8_t* mips_bytes = memory.TranslatePhysical<const uint8_t*>(
            texture_key.mip_page << 12);
        dump.cpu_mips_at_load.assign(mips_bytes,
                                     mips_bytes + dump.shared_mips_size);
        VkBufferCopy scratch_copy = {0, 0, dump.scratch_size};
        command_buffer.CmdVkCopyBuffer(scratch_buffer, dump.readback_buffer, 1,
                                       &scratch_copy);
        auto copy_shared_range = [&](uint32_t guest_address,
                                     uint32_t guest_size,
                                     VkDeviceSize destination_offset) {
          const VulkanSharedMemory::BufferSliceList slices =
              vulkan_shared_memory.GetBufferSlices(guest_address, guest_size);
          uint64_t next_guest_address = guest_address;
          uint64_t copied_size = 0;
          for (const VulkanSharedMemory::BufferSlice& slice : slices) {
            if (!slice.length || slice.guest_offset != next_guest_address) {
              return false;
            }
            VkBufferCopy copy = {
                slice.buffer_offset,
                destination_offset + (slice.guest_offset - guest_address),
                slice.length};
            command_buffer.CmdVkCopyBuffer(slice.buffer, dump.readback_buffer,
                                           1, &copy);
            next_guest_address += slice.length;
            copied_size += slice.length;
          }
          return copied_size == guest_size;
        };
        const bool shared_dump_copied =
            (!dump.shared_base_size ||
             copy_shared_range(texture_key.base_page << 12,
                               dump.shared_base_size, dump.scratch_size)) &&
            copy_shared_range(
                texture_key.mip_page << 12, dump.shared_mips_size,
                dump.scratch_size + dump.shared_base_size);
        if (!shared_dump_copied) {
          XELOGW("Texture dump skipped: shared-memory slices are incomplete");
        }
        command_processor_.PushBufferMemoryBarrier(
            dump.readback_buffer, 0, VK_WHOLE_SIZE,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        std::string levels_json;
        // Only levels loaded from guest memory have copy regions.
        for (uint32_t level = 1; level <= level_last; ++level) {
          const texture_util::TextureGuestLayout::Level& guest_level =
              guest_layout.mips[level];
          const HostLayout& host_level =
              host_layout_mips[std::min(level, level_packed)];
          const VkBufferImageCopy& copy_region =
              copy_regions[level - level_first];
          levels_json += fmt::format(
              "{}{{\"level\":{},\"guest_offset\":{},\"row_pitch_bytes\":{},"
              "\"z_stride_block_rows\":{},\"x_extent_blocks\":{},"
              "\"y_extent_blocks\":{},\"extent_bytes\":{},"
              "\"host_offset\":{},\"host_x_pitch_blocks\":{},"
              "\"host_y_pitch_blocks\":{},\"copy_offset\":{},"
              "\"copy_row_length\":{},\"copy_image_height\":{},"
              "\"copy_width\":{},\"copy_height\":{}}}",
              level > 1 ? "," : "", level, guest_layout.mip_offsets_bytes[level],
              guest_level.row_pitch_bytes, guest_level.z_slice_stride_block_rows,
              guest_level.x_extent_blocks, guest_level.y_extent_blocks,
              guest_level.level_data_extent_bytes, host_level.offset_bytes,
              host_level.x_pitch_blocks, host_level.y_pitch_blocks,
              copy_region.bufferOffset, copy_region.bufferRowLength,
              copy_region.bufferImageHeight, copy_region.imageExtent.width,
              copy_region.imageExtent.height);
        }
        dump.metadata = fmt::format(
            "\"format\":{},\"format_name\":\"{}\",\"width\":{},\"height\":{},"
            "\"pitch\":{},\"tiled\":{},\"packed_mips\":{},\"endianness\":{},"
            "\"base_page\":{},\"mip_page\":{},\"mip_max_level\":{},"
            "\"packed_level\":{},\"array_size\":{},\"is_3d_tiling\":{},"
            "\"load_base\":{},\"load_mips\":{},\"load_shader\":{},"
            "\"host_vk_format\":{},\"bytes_per_host_block\":{},"
            "\"base_row_pitch_bytes\":{},\"base_x_extent_blocks\":{},"
            "\"base_y_extent_blocks\":{},\"base_host_offset\":{},"
            "\"base_host_x_pitch_blocks\":{},\"base_host_y_pitch_blocks\":{},"
            "\"levels\":[{}]",
            uint32_t(guest_format), FormatInfo::GetName(guest_format), width,
            height, uint32_t(texture_key.pitch), uint32_t(texture_key.tiled),
            uint32_t(texture_key.packed_mips),
            uint32_t(texture_key.endianness), uint32_t(texture_key.base_page),
            uint32_t(texture_key.mip_page),
            uint32_t(texture_key.mip_max_level), level_packed, array_size,
            uint32_t(is_3d_tiling), uint32_t(load_base), uint32_t(load_mips),
            uint32_t(load_shader), uint32_t(host_format.format),
            load_shader_info.bytes_per_host_block,
            guest_layout.base.row_pitch_bytes,
            guest_layout.base.x_extent_blocks,
            guest_layout.base.y_extent_blocks,
            load_base ? host_layout_base.offset_bytes : VkDeviceSize(0),
            load_base ? host_layout_base.x_pitch_blocks : 0,
            load_base ? host_layout_base.y_pitch_blocks : 0, levels_json);
        debug_texture_dump_bytes_ += readback_size;
        debug_texture_dumps_.push_back(std::move(dump));
      }
    }
  }

  // Generate mip levels for scaled resolve textures via blit.
  if (level_last_for_blit_gen > 0) {
    command_processor_.InsertDebugMarker(
        "Mip Generation: %ux%u levels 1-%u", width * texture_resolution_scale_x,
        height * texture_resolution_scale_y, level_last_for_blit_gen);

    VkImage image = vulkan_texture.image();
    uint32_t scaled_width = width * texture_resolution_scale_x;
    uint32_t scaled_height = height * texture_resolution_scale_y;

    // Generate each mip level by blitting from the previous level.
    for (uint32_t level = blit_gen_first_level;
         level <= level_last_for_blit_gen; ++level) {
      uint32_t src_width = std::max(scaled_width >> (level - 1), UINT32_C(1));
      uint32_t src_height = std::max(scaled_height >> (level - 1), UINT32_C(1));
      uint32_t src_depth = std::max(depth >> (level - 1), UINT32_C(1));
      uint32_t dst_width = std::max(scaled_width >> level, UINT32_C(1));
      uint32_t dst_height = std::max(scaled_height >> level, UINT32_C(1));
      uint32_t dst_depth = std::max(depth >> level, UINT32_C(1));
      // VkImageBlit offsets are int32_t - ensure dimensions fit.
      assert_true(src_width <= INT32_MAX && src_height <= INT32_MAX &&
                  src_depth <= INT32_MAX);
      assert_true(dst_width <= INT32_MAX && dst_height <= INT32_MAX &&
                  dst_depth <= INT32_MAX);

      // Transition source mip (level - 1) to TRANSFER_SRC_OPTIMAL.
      {
        VkImageMemoryBarrier src_barrier = {};
        src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_barrier.image = image;
        src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        src_barrier.subresourceRange.baseMipLevel = level - 1;
        src_barrier.subresourceRange.levelCount = 1;
        src_barrier.subresourceRange.baseArrayLayer = 0;
        src_barrier.subresourceRange.layerCount = array_size;
        command_buffer.CmdVkPipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &src_barrier);
      }

      // Blit from level - 1 to level.
      VkImageBlit blit_region = {};
      blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      blit_region.srcSubresource.mipLevel = level - 1;
      blit_region.srcSubresource.baseArrayLayer = 0;
      blit_region.srcSubresource.layerCount = array_size;
      blit_region.srcOffsets[0] = {0, 0, 0};
      blit_region.srcOffsets[1] = {static_cast<int32_t>(src_width),
                                   static_cast<int32_t>(src_height),
                                   static_cast<int32_t>(src_depth)};
      blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      blit_region.dstSubresource.mipLevel = level;
      blit_region.dstSubresource.baseArrayLayer = 0;
      blit_region.dstSubresource.layerCount = array_size;
      blit_region.dstOffsets[0] = {0, 0, 0};
      blit_region.dstOffsets[1] = {static_cast<int32_t>(dst_width),
                                   static_cast<int32_t>(dst_height),
                                   static_cast<int32_t>(dst_depth)};

      command_buffer.CmdVkBlitImage(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1, &blit_region, VK_FILTER_LINEAR);
    }

    // Transition all mip levels to the final layout (TRANSFER_DST for now,
    // will be transitioned to shader read when used).
    // Level 0 to level_last_for_blit_gen - 1 are in TRANSFER_SRC_OPTIMAL.
    // Level level_last_for_blit_gen is in TRANSFER_DST_OPTIMAL (no change
    // needed).
    if (level_last_for_blit_gen > 0) {
      VkImageMemoryBarrier final_barrier = {};
      final_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      final_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      final_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      final_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      final_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      final_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      final_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      final_barrier.image = image;
      final_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      final_barrier.subresourceRange.baseMipLevel = blit_gen_first_level - 1;
      final_barrier.subresourceRange.levelCount =
          level_last_for_blit_gen - (blit_gen_first_level - 1);
      final_barrier.subresourceRange.baseArrayLayer = 0;
      final_barrier.subresourceRange.layerCount = array_size;
      command_buffer.CmdVkPipelineBarrier(
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
          nullptr, 0, nullptr, 1, &final_barrier);
    }
  }

  command_processor_.EndTimedSection(command_buffer, timed_section);

  if (pass_head_command_buffer) {
    // Leave the texture sampled by guest shaders, as the draw needing it would
    // transition it - there, the barrier would end the pass.
    VkPipelineStageFlags sampled_stage_mask;
    VkAccessFlags sampled_access_mask;
    VkImageLayout sampled_layout;
    GetTextureUsageMasks(VulkanTexture::Usage::kGuestShaderSampled,
                         sampled_stage_mask, sampled_access_mask,
                         sampled_layout);
    vulkan_texture.SetUsage(VulkanTexture::Usage::kGuestShaderSampled);
    vulkan_texture.vertex_visible = true;
    VkImageMemoryBarrier sampled_barrier;
    sampled_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sampled_barrier.pNext = nullptr;
    sampled_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sampled_barrier.dstAccessMask = sampled_access_mask;
    sampled_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sampled_barrier.newLayout = sampled_layout;
    sampled_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sampled_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sampled_barrier.image = vulkan_texture.image();
    sampled_barrier.subresourceRange =
        ui::vulkan::util::InitializeSubresourceRange();
    command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        sampled_stage_mask, 0, 0, nullptr, 0,
                                        nullptr, 1, &sampled_barrier);
  }

  command_processor_.PopDebugMarker();
  return true;
}

void VulkanTextureCache::UpdateTextureBindingsImpl(
    uint32_t fetch_constant_mask) {
  // These image views are about to change - let the command processor know the
  // texture descriptor sets referencing them need to be rewritten.
  texture_bindings_changed_ |= fetch_constant_mask;
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (xe::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    VulkanTextureBinding& vulkan_binding =
        vulkan_texture_bindings_[binding_index];
    vulkan_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    if (IsSignedVersionSeparateForFormat(binding->key)) {
      if (binding->texture &&
          texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
        vulkan_binding.image_view_unsigned =
            static_cast<VulkanTexture*>(binding->texture)
                ->GetView(false, binding->host_swizzle);
      }
      if (binding->texture_signed &&
          texture_util::IsAnySignSigned(binding->swizzled_signs)) {
        vulkan_binding.image_view_signed =
            static_cast<VulkanTexture*>(binding->texture_signed)
                ->GetView(true, binding->host_swizzle);
      }
    } else {
      VulkanTexture* texture = static_cast<VulkanTexture*>(binding->texture);
      if (texture) {
        if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
          vulkan_binding.image_view_unsigned =
              texture->GetView(false, binding->host_swizzle);
        }
        if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
          vulkan_binding.image_view_signed =
              texture->GetView(true, binding->host_swizzle);
        }
      }
    }
  }
}

VulkanTextureCache::VulkanTexture::VulkanTexture(
    VulkanTextureCache& texture_cache, const TextureKey& key, VkImage image,
    VmaAllocation allocation, bool track_usage)
    : Texture(texture_cache, key, track_usage),
      image_(image),
      allocation_(allocation) {
  VmaAllocationInfo allocation_info;
  vmaGetAllocationInfo(texture_cache.vma_allocator_, allocation_,
                       &allocation_info);
  SetHostMemoryUsage(uint64_t(allocation_info.size));
}

VulkanTextureCache::VulkanTexture::~VulkanTexture() {
  const VulkanTextureCache& vulkan_texture_cache =
      static_cast<const VulkanTextureCache&>(texture_cache());
  const ui::vulkan::VulkanDevice* const vulkan_device =
      vulkan_texture_cache.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ++const_cast<VulkanTextureCache&>(vulkan_texture_cache)
        .image_view_destroy_generation_;
  auto& candidates =
      const_cast<VulkanTextureCache&>(vulkan_texture_cache).resolve_textures_;
  auto range = candidates.equal_range(key().base_page << 12);
  for (auto it = range.first; it != range.second;) {
    if (it->second == this) {
      it = candidates.erase(it);
    } else {
      ++it;
    }
  }
  if (depth_surface_framebuffer != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, depth_surface_framebuffer, nullptr);
  }
  if (depth_surface_attachment_view != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, depth_surface_attachment_view, nullptr);
  }
  if (depth_surface_stencil_view != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, depth_surface_stencil_view, nullptr);
  }
  if (depth_surface_stencil_image != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, depth_surface_stencil_image, nullptr);
  }
  if (depth_surface_stencil_memory != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, depth_surface_stencil_memory, nullptr);
  }
  VulkanCommandProcessor& command_processor =
      const_cast<VulkanTextureCache&>(vulkan_texture_cache).command_processor_;
  for (const auto& view_pair : views_) {
    command_processor.ReleaseBindlessImageView(view_pair.second);
    dfn.vkDestroyImageView(device, view_pair.second, nullptr);
  }
  // Clean up 3D-as-2D image views. The texture_3d_as_2d_ wrapper will clean
  // itself up via unique_ptr destructor.
  if (image_view_3d_as_2d_unsigned_ != VK_NULL_HANDLE) {
    command_processor.ReleaseBindlessImageView(image_view_3d_as_2d_unsigned_);
    dfn.vkDestroyImageView(device, image_view_3d_as_2d_unsigned_, nullptr);
  }
  if (image_view_3d_as_2d_signed_ != VK_NULL_HANDLE) {
    command_processor.ReleaseBindlessImageView(image_view_3d_as_2d_signed_);
    dfn.vkDestroyImageView(device, image_view_3d_as_2d_signed_, nullptr);
  }
  // texture_3d_as_2d_ is a unique_ptr and will destroy its image/allocation.
  vmaDestroyImage(vulkan_texture_cache.vma_allocator_, image_, allocation_);
}

VkImageView VulkanTextureCache::VulkanTexture::GetView(bool is_signed,
                                                       uint32_t host_swizzle,
                                                       bool is_array) {
  xenos::DataDimension dimension = key().dimension;
  if (dimension == xenos::DataDimension::k3D ||
      dimension == xenos::DataDimension::kCube) {
    is_array = false;
  }

  const VulkanTextureCache& vulkan_texture_cache =
      static_cast<const VulkanTextureCache&>(texture_cache());

  ViewKey view_key;

  const HostFormatPair& host_format_pair =
      vulkan_texture_cache.GetHostFormatPair(key());
  VkFormat format = (is_signed ? host_format_pair.format_signed
                               : host_format_pair.format_unsigned)
                        .format;
  if (format == VK_FORMAT_UNDEFINED) {
    return VK_NULL_HANDLE;
  }
  // If not distinguishing between unsigned and signed formats for the same
  // image, don't create two views. As this happens within an image, no need to
  // care about whether unsigned and signed images are separate - if they are
  // (or if there are only unsigned or only signed images), this image will have
  // either all views unsigned or all views signed.
  view_key.is_signed_separate_view =
      is_signed && (host_format_pair.format_signed.format !=
                    host_format_pair.format_unsigned.format);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      vulkan_texture_cache.command_processor_.GetVulkanDevice();

  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  view_key.host_swizzle = host_swizzle;

  view_key.is_array = uint32_t(is_array);

  auto& lookup = view_lookup_[(view_key.key * UINT32_C(0x9E3779B1)) >> 29];
  if (lookup.key == view_key.key) return lookup.view;
  auto it = views_.find(view_key);
  if (it != views_.end()) {
    lookup = {view_key.key, it->second};
    return it->second;
  }

  // Create a new view.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image();
  view_create_info.format = format;
  view_create_info.components.r = GetComponentSwizzle(host_swizzle, 0);
  view_create_info.components.g = GetComponentSwizzle(host_swizzle, 1);
  view_create_info.components.b = GetComponentSwizzle(host_swizzle, 2);
  view_create_info.components.a = GetComponentSwizzle(host_swizzle, 3);
  view_create_info.subresourceRange =
      ui::vulkan::util::InitializeSubresourceRange();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      view_create_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
      break;
    case xenos::DataDimension::kCube:
      view_create_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
      break;
    default:
      if (is_array) {
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      } else {
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_create_info.subresourceRange.layerCount = 1;
      }
      break;
  }
  VkImageView view;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view) !=
      VK_SUCCESS) {
    XELOGE(
        "VulkanTextureCache: Failed to create an image view for Vulkan format "
        "{} ({}signed) with swizzle 0x{:3X}",
        uint32_t(format), is_signed ? "" : "un", host_swizzle);
    return VK_NULL_HANDLE;
  }
  views_.emplace(view_key, view);
  lookup = {view_key.key, view};
  return view;
}

VkImageView VulkanTextureCache::VulkanTexture::GetOrCreate3DAs2DImageView(
    bool is_signed, uint32_t host_swizzle) {
  if (!cvars::gpu_3d_to_2d_texture) {
    return VK_NULL_HANDLE;
  }

  // Return cached view if available.
  VkImageView& cached_view =
      is_signed ? image_view_3d_as_2d_signed_ : image_view_3d_as_2d_unsigned_;
  if (cached_view != VK_NULL_HANDLE) {
    return cached_view;
  }

  VulkanTextureCache& vulkan_texture_cache =
      static_cast<VulkanTextureCache&>(texture_cache());

  // Create the 2D texture wrapper if it doesn't exist.
  if (!texture_3d_as_2d_) {
    const HostFormatPair& host_format_pair =
        vulkan_texture_cache.GetHostFormatPair(key());
    VkFormat format = host_format_pair.format_unsigned.format;
    if (format == VK_FORMAT_UNDEFINED) {
      return VK_NULL_HANDLE;
    }

    VkImageCreateInfo image_create_info = {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = format;
    image_create_info.extent.width = key().GetWidth();
    image_create_info.extent.height = key().GetHeight();
    if (key().scaled_resolve) {
      image_create_info.extent.width *=
          vulkan_texture_cache.draw_resolution_scale_x();
      image_create_info.extent.height *=
          vulkan_texture_cache.draw_resolution_scale_y();
    }
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation_create_info = {};
    allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage image_2d;
    VmaAllocation allocation_2d;
    if (vmaCreateImage(vulkan_texture_cache.vma_allocator_, &image_create_info,
                       &allocation_create_info, &image_2d, &allocation_2d,
                       nullptr) != VK_SUCCESS) {
      XELOGE("VulkanTexture: Failed to create 3D-as-2D image");
      return VK_NULL_HANDLE;
    }

    // Create a modified key for the 2D wrapper with depth=1 and
    // mip_max_level=0. Keep dimension as k3D so guest layout uses 3D tiling
    // math to correctly read slice 0 from the 3D-tiled guest memory.
    TextureKey key_2d = key();
    key_2d.depth_or_array_size_minus_1 = 0;
    key_2d.mip_max_level = 0;

    // Create the wrapper first so LoadTextureData can work with it.
    texture_3d_as_2d_.reset(new VulkanTexture(vulkan_texture_cache, key_2d,
                                              image_2d, allocation_2d, false));

    if (!vulkan_texture_cache.LoadTextureData(*texture_3d_as_2d_)) {
      XELOGE("VulkanTexture: Failed to load 3D-as-2D texture data");
      texture_3d_as_2d_.reset();
      return VK_NULL_HANDLE;
    }

    // LoadTextureData leaves the texture in TRANSFER_DST state.
    // Transition to shader read state.
    DeferredCommandBuffer& command_buffer =
        vulkan_texture_cache.command_processor_.deferred_command_buffer();
    VkImageMemoryBarrier barrier_to_shader = {};
    barrier_to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier_to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier_to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier_to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier_to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier_to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_to_shader.image = texture_3d_as_2d_->image();
    barrier_to_shader.subresourceRange =
        ui::vulkan::util::InitializeSubresourceRange();
    command_buffer.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier_to_shader);
    texture_3d_as_2d_->SetUsage(Usage::kGuestShaderSampled);
  }

  // Create the image view.
  const ui::vulkan::VulkanDevice* const vulkan_device =
      vulkan_texture_cache.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const HostFormatPair& host_format_pair =
      vulkan_texture_cache.GetHostFormatPair(key());
  VkFormat format = (is_signed ? host_format_pair.format_signed
                               : host_format_pair.format_unsigned)
                        .format;
  if (format == VK_FORMAT_UNDEFINED) {
    return VK_NULL_HANDLE;
  }

  VkImageViewCreateInfo view_create_info = {};
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.image = texture_3d_as_2d_->image();
  // Use 2D_ARRAY to match shader expectations (Dim = 2D, Arrayed = 1).
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view_create_info.format = format;
  view_create_info.components.r = GetComponentSwizzle(host_swizzle, 0);
  view_create_info.components.g = GetComponentSwizzle(host_swizzle, 1);
  view_create_info.components.b = GetComponentSwizzle(host_swizzle, 2);
  view_create_info.components.a = GetComponentSwizzle(host_swizzle, 3);
  view_create_info.subresourceRange =
      ui::vulkan::util::InitializeSubresourceRange();
  view_create_info.subresourceRange.layerCount = 1;

  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &cached_view) !=
      VK_SUCCESS) {
    XELOGE("VulkanTexture: Failed to create 3D-as-2D image view");
    return VK_NULL_HANDLE;
  }

  return cached_view;
}

VulkanTextureCache::VulkanTextureCache(
    const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
    VulkanCommandProcessor& command_processor,
    VkPipelineStageFlags guest_shader_pipeline_stages)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x,
                   draw_resolution_scale_y),
      command_processor_(command_processor),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

void VulkanTextureCache::LogMemoryAccounting() const {
  if (vma_allocator_ == VK_NULL_HANDLE) {
    return;
  }
  VmaTotalStatistics stats = {};
  vmaCalculateStatistics(vma_allocator_, &stats);
  const auto& total = stats.total.statistics;
  XELOGI("TextureHeap: blocks={} {:.1f} MiB allocations={} {:.1f} MiB "
         "unused={:.1f} MiB largest_free={:.1f} MiB",
         total.blockCount, double(total.blockBytes) / 1048576.0,
         total.allocationCount, double(total.allocationBytes) / 1048576.0,
         double(total.blockBytes - total.allocationBytes) / 1048576.0,
         double(stats.total.unusedRangeSizeMax) / 1048576.0);

  struct FormatUsage {
    uint64_t main_bytes = 0, wrapper_bytes = 0, scaled_bytes = 0;
    uint32_t count = 0, wrappers = 0;
  } formats[64] = {};
  uint64_t bc3_fallback_bytes = 0, bc3_other_bytes = 0;
  uint64_t bc3_rgba_payload = 0, bc3_astc_payload = 0;
  uint32_t bc3_fallback_count = 0;
  auto account = [&](const Texture& texture, bool wrapper) {
    const TextureKey& key = texture.key();
    const uint32_t format = uint32_t(key.format);
    if (format >= xe::countof(formats)) {
      return;
    }
    auto& usage = formats[format];
    const uint64_t bytes = texture.GetHostMemoryUsage();
    if (wrapper) {
      ++usage.wrappers;
      usage.wrapper_bytes += bytes;
    } else {
      ++usage.count;
      usage.main_bytes += bytes;
    }
    if (key.scaled_resolve) {
      usage.scaled_bytes += bytes;
    }
    if (key.format != xenos::TextureFormat::k_DXT4_5 &&
        key.format != xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16) {
      return;
    }
    const auto& pair = GetHostFormatPair(key);
    const auto& host = key.signed_separate ? pair.format_signed
                                         : pair.format_unsigned;
    // Candidate: ordinary 2D/layer/cube BC3 expanded to four-channel RGBA8.
    // Keep 3D, scaled resolve, signed and any other host representation out.
    if (key.scaled_resolve || key.signed_separate ||
        (!wrapper && key.dimension == xenos::DataDimension::k3D) ||
        (host.format != VK_FORMAT_R8G8B8A8_UNORM &&
         host.format != VK_FORMAT_R8G8B8A8_SRGB)) {
      bc3_other_bytes += bytes;
      return;
    }
    ++bc3_fallback_count;
    bc3_fallback_bytes += bytes;
    for (uint32_t mip = 0; mip <= key.mip_max_level; ++mip) {
      const uint64_t w = std::max(key.GetWidth() >> mip, uint32_t(1));
      const uint64_t h = std::max(key.GetHeight() >> mip, uint32_t(1));
      const uint64_t layers = wrapper ? 1 : key.GetDepthOrArraySize();
      bc3_rgba_payload += w * h * layers * 4;
      bc3_astc_payload += ((w + 3) / 4) * ((h + 3) / 4) * layers * 16;
    }
  };
  ForEachTextureForDebug([&](const Texture& texture) {
    account(texture, false);
    const auto* wrapper = static_cast<const VulkanTexture&>(texture)
                              .wrapper_3d_as_2d_for_debug();
    if (wrapper) {
      account(*wrapper, true);
    }
  });
  for (uint32_t format = 0; format < xe::countof(formats); ++format) {
    const auto& usage = formats[format];
    if (!usage.count && !usage.wrappers) {
      continue;
    }
    XELOGI("TextureResidency: guest_format={} count={} main={:.2f} MiB "
           "wrappers={} wrapper={:.2f} MiB scaled_subset={:.2f} MiB",
           format, usage.count, double(usage.main_bytes) / 1048576.0,
           usage.wrappers, double(usage.wrapper_bytes) / 1048576.0,
           double(usage.scaled_bytes) / 1048576.0);
  }
  XELOGI("Bc3Residency: rgba_fallback_count={} allocation={:.2f} MiB "
         "rgba_payload={:.2f} MiB astc_payload_estimate={:.2f} MiB "
         "excluded={:.2f} MiB (payload excludes VMA alignment and block retention)",
         bc3_fallback_count, double(bc3_fallback_bytes) / 1048576.0,
         double(bc3_rgba_payload) / 1048576.0,
         double(bc3_astc_payload) / 1048576.0,
         double(bc3_other_bytes) / 1048576.0);
}

bool VulkanTextureCache::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();
  VulkanSharedMemory& vulkan_shared_memory =
      static_cast<VulkanSharedMemory&>(shared_memory());

  // Vulkan Memory Allocator.

  uint32_t block_mb = cvars::vulkan_texture_heap_block_mb;
  if (block_mb && (block_mb < 16 || block_mb > 256)) {
    XELOGW("Texture heap block size {} MiB outside 16..256; using VMA default",
           block_mb);
    block_mb = 0;
  }
  vma_allocator_ = ui::vulkan::CreateVmaAllocator(
      vulkan_device, true, VkDeviceSize(block_mb) * 1024 * 1024);
  XELOGI("TextureHeap: preferred block {} MiB (0=VMA default)", block_mb);
  if (vma_allocator_ == VK_NULL_HANDLE) {
    return false;
  }

  // Image formats.

  // Initialize to the best formats.
  for (size_t i = 0; i < xe::countof(host_formats_); ++i) {
    host_formats_[i] = kBestHostFormats[i];
  }

  // Check format support and switch to fallbacks if needed.
  constexpr VkFormatFeatureFlags kLinearFilterFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  VkFormatProperties r16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16_UNORM,
                                          &r16_unorm_properties);
  VkFormatProperties r16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R16_SNORM,
                                          &r16_snorm_properties);
  VkFormatProperties r16g16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_R16G16_UNORM, &r16g16_unorm_properties);
  VkFormatProperties r16g16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_R16G16_SNORM, &r16g16_snorm_properties);
  VkFormatProperties r16g16b16a16_unorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                          VK_FORMAT_R16G16B16A16_UNORM,
                                          &r16g16b16a16_unorm_properties);
  VkFormatProperties r16g16b16a16_snorm_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                          VK_FORMAT_R16G16B16A16_SNORM,
                                          &r16g16b16a16_snorm_properties);
  VkFormatProperties format_properties;
  // TODO(Triang3l): k_2_10_10_10 signed -> filterable R16G16B16A16_SFLOAT
  // (enough storage precision, possibly unwanted filtering precision change).
  // k_Cr_Y1_Cb_Y0_REP, k_Y1_Cr_Y0_Cb_REP.
  HostFormatPair& host_format_gbgr =
      host_formats_[uint32_t(xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP)];
  assert_true(host_format_gbgr.format_unsigned.format ==
              VK_FORMAT_G8B8G8R8_422_UNORM_KHR);
  assert_true(host_format_gbgr.format_signed.format ==
              VK_FORMAT_R8G8B8A8_SNORM);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_G8B8G8R8_422_UNORM_KHR, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    host_format_gbgr.format_unsigned.load_shader = kLoadShaderIndexGBGR8ToRGB8;
    host_format_gbgr.format_unsigned.format = VK_FORMAT_R8G8B8A8_UNORM;
    host_format_gbgr.format_unsigned.block_compressed = false;
    host_format_gbgr.unsigned_signed_compatible = true;
  }
  HostFormatPair& host_format_bgrg =
      host_formats_[uint32_t(xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP)];
  assert_true(host_format_bgrg.format_unsigned.format ==
              VK_FORMAT_B8G8R8G8_422_UNORM_KHR);
  assert_true(host_format_bgrg.format_signed.format ==
              VK_FORMAT_R8G8B8A8_SNORM);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_B8G8R8G8_422_UNORM_KHR, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    host_format_bgrg.format_unsigned.load_shader = kLoadShaderIndexBGRG8ToRGB8;
    host_format_bgrg.format_unsigned.format = VK_FORMAT_R8G8B8A8_UNORM;
    host_format_bgrg.format_unsigned.block_compressed = false;
    host_format_bgrg.unsigned_signed_compatible = true;
  }
  // TODO(Triang3l): k_10_11_11 -> filterable R16G16B16A16_SFLOAT (enough
  // storage precision, possibly unwanted filtering precision change).
  // TODO(Triang3l): k_11_11_10 -> filterable R16G16B16A16_SFLOAT (enough
  // storage precision, possibly unwanted filtering precision change).
  // S3TC.
  // Not checking the textureCompressionBC feature because its availability
  // means that all BC formats are supported, however, the device may expose
  // some BC formats without this feature. Xenia doesn't use BC6H and BC7 at
  // all, and has fallbacks for each used format.
  // TODO(Triang3l): Raise the host texture memory usage limit if S3TC has to be
  // decompressed.
  // TODO(Triang3l): S3TC -> 5551 or 4444 as an option.
  // TODO(Triang3l): S3TC -> ETC2 / EAC (a huge research topic).
  {
    // Which S3TC / ASTC formats the driver can sample with linear filtering
    // (without BC, DXT textures are decompressed to 4-8x larger RGBA8).
    const VkFormat probe_formats[] = {
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC4_UNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK,      VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK};
    const char* probe_names[] = {"BC1", "BC2", "BC3", "BC4",
                                 "BC5", "ASTC4x4", "ETC2A8"};
    std::string probe_text;
    for (size_t i = 0; i < xe::countof(probe_formats); ++i) {
      VkFormatProperties probe_properties;
      ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                              probe_formats[i],
                                              &probe_properties);
      probe_text += fmt::format(
          " {}={}", probe_names[i],
          (probe_properties.optimalTilingFeatures & kLinearFilterFeatures) ==
                  kLinearFilterFeatures
              ? 1
              : 0);
    }
    XELOGI("VulkanTextureCache: compressed format support:{}", probe_text);
  }
  HostFormatPair& host_format_dxt1 =
      host_formats_[uint32_t(xenos::TextureFormat::k_DXT1)];
  assert_true(host_format_dxt1.format_unsigned.format ==
              VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    VkFormatProperties astc_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astc_properties);
    constexpr VkFormatFeatureFlags kAstcFeatures =
        kLinearFilterFeatures | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (cvars::vulkan_dxt1_to_astc &&
        (astc_properties.optimalTilingFeatures & kAstcFeatures) ==
            kAstcFeatures) {
      host_format_dxt1.format_unsigned.load_shader =
          kLoadShaderIndexDXT1ToASTC4x4;
      host_format_dxt1.format_unsigned.format =
          VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      host_format_dxt1.format_unsigned.block_compressed = true;
      XELOGI("VulkanTextureCache: DXT1 textures transcoded to ASTC 4x4");
    } else {
      host_format_dxt1.format_unsigned.load_shader =
          kLoadShaderIndexDXT1ToRGBA8;
      host_format_dxt1.format_unsigned.format = VK_FORMAT_R8G8B8A8_UNORM;
      host_format_dxt1.format_unsigned.block_compressed = false;
    }
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT1_AS_16_16_16_16)] =
        host_format_dxt1;
  }
  HostFormatPair& host_format_dxt2_3 =
      host_formats_[uint32_t(xenos::TextureFormat::k_DXT2_3)];
  assert_true(host_format_dxt2_3.format_unsigned.format ==
              VK_FORMAT_BC2_UNORM_BLOCK);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_BC2_UNORM_BLOCK, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    host_format_dxt2_3.format_unsigned.load_shader =
        kLoadShaderIndexDXT3ToRGBA8;
    host_format_dxt2_3.format_unsigned.format = VK_FORMAT_R8G8B8A8_UNORM;
    host_format_dxt2_3.format_unsigned.block_compressed = false;
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16)] =
        host_format_dxt2_3;
  }
  HostFormatPair& host_format_dxt4_5 =
      host_formats_[uint32_t(xenos::TextureFormat::k_DXT4_5)];
  assert_true(host_format_dxt4_5.format_unsigned.format ==
              VK_FORMAT_BC3_UNORM_BLOCK);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_BC3_UNORM_BLOCK, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    VkFormatProperties astc_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astc_properties);
    constexpr VkFormatFeatureFlags kAstcFeatures =
        kLinearFilterFeatures | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (cvars::vulkan_dxt5_to_astc &&
        (astc_properties.optimalTilingFeatures & kAstcFeatures) ==
            kAstcFeatures) {
      host_format_dxt4_5.format_unsigned.load_shader =
          kLoadShaderIndexDXT5ToASTC4x4;
      host_format_dxt4_5.format_unsigned.format =
          VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      host_format_dxt4_5.format_unsigned.block_compressed = true;
      XELOGI("VulkanTextureCache: DXT4/5 textures transcoded to ASTC 4x4");
    } else {
      host_format_dxt4_5.format_unsigned.load_shader =
          kLoadShaderIndexDXT5ToRGBA8;
      host_format_dxt4_5.format_unsigned.format = VK_FORMAT_R8G8B8A8_UNORM;
      host_format_dxt4_5.format_unsigned.block_compressed = false;
    }
    host_formats_[uint32_t(xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16)] =
        host_format_dxt4_5;
  }
  HostFormatPair& host_format_dxn =
      host_formats_[uint32_t(xenos::TextureFormat::k_DXN)];
  assert_true(host_format_dxn.format_unsigned.format ==
              VK_FORMAT_BC5_UNORM_BLOCK);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_BC5_UNORM_BLOCK, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    VkFormatProperties eac_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, &eac_properties);
    constexpr VkFormatFeatureFlags kEacFeatures =
        kLinearFilterFeatures | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (cvars::vulkan_dxn_to_eac &&
        (eac_properties.optimalTilingFeatures & kEacFeatures) ==
            kEacFeatures) {
      host_format_dxn.format_unsigned.load_shader =
          kLoadShaderIndexDXNToEACRG11;
      host_format_dxn.format_unsigned.format =
          VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
      host_format_dxn.format_unsigned.block_compressed = true;
      XELOGI("VulkanTextureCache: DXN textures transcoded to EAC RG11");
    } else {
      host_format_dxn.format_unsigned.load_shader = kLoadShaderIndexDXNToRG8;
      host_format_dxn.format_unsigned.format = VK_FORMAT_R8G8_UNORM;
      host_format_dxn.format_unsigned.block_compressed = false;
    }
  }
  HostFormatPair& host_format_dxt5a =
      host_formats_[uint32_t(xenos::TextureFormat::k_DXT5A)];
  assert_true(host_format_dxt5a.format_unsigned.format ==
              VK_FORMAT_BC4_UNORM_BLOCK);
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_BC4_UNORM_BLOCK, &format_properties);
  if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) !=
      kLinearFilterFeatures) {
    host_format_dxt5a.format_unsigned.load_shader = kLoadShaderIndexDXT5AToR8;
    host_format_dxt5a.format_unsigned.format = VK_FORMAT_R8_UNORM;
    host_format_dxt5a.format_unsigned.block_compressed = false;
  }
  // Diagnostic: make the listed guest texture formats unloadable (bound as
  // null textures) to identify which format renders incorrectly.
  if (!cvars::vulkan_debug_skip_texture_formats.empty()) {
    std::string skip_list = cvars::vulkan_debug_skip_texture_formats;
    size_t skip_pos = 0;
    while (skip_pos < skip_list.size()) {
      size_t skip_end = skip_list.find(',', skip_pos);
      if (skip_end == std::string::npos) {
        skip_end = skip_list.size();
      }
      int skip_format =
          std::atoi(skip_list.substr(skip_pos, skip_end - skip_pos).c_str());
      if (skip_format >= 0 && skip_format < 64) {
        HostFormatPair& skip_pair = host_formats_[uint32_t(skip_format)];
        skip_pair.format_unsigned.load_shader = kLoadShaderIndexUnknown;
        skip_pair.format_unsigned.format = VK_FORMAT_UNDEFINED;
        skip_pair.format_signed.load_shader = kLoadShaderIndexUnknown;
        skip_pair.format_signed.format = VK_FORMAT_UNDEFINED;
        XELOGW("VulkanTextureCache: debug-skipping guest texture format {}",
               skip_format);
      }
      skip_pos = skip_end + 1;
    }
  }
  // Guest fixed-point 16-bit textures are filterable. Optional host UNORM /
  // SNORM formats must support filtering as well as sampling, otherwise a
  // guest linear sampler is invalid (Mali exposes non-filterable RG16/RGBA16).
  // Use the existing SFLOAT conversion fallback when either feature is absent.
  constexpr VkFormatFeatureFlags norm16_required_features =
      kLinearFilterFeatures;
  HostFormatPair& host_format_16 =
      host_formats_[uint32_t(xenos::TextureFormat::k_16)];
  assert_true(host_format_16.format_unsigned.format == VK_FORMAT_R16_UNORM);
  if ((r16_unorm_properties.optimalTilingFeatures & norm16_required_features) !=
      norm16_required_features) {
    host_format_16.format_unsigned.load_shader =
        kLoadShaderIndexR16UNormToFloat;
    host_format_16.format_unsigned.format = VK_FORMAT_R16_SFLOAT;
  }
  assert_true(host_format_16.format_signed.format == VK_FORMAT_R16_SNORM);
  if ((r16_snorm_properties.optimalTilingFeatures & norm16_required_features) !=
      norm16_required_features) {
    host_format_16.format_signed.load_shader = kLoadShaderIndexR16SNormToFloat;
    host_format_16.format_signed.format = VK_FORMAT_R16_SFLOAT;
  }
  host_format_16.unsigned_signed_compatible =
      (host_format_16.format_unsigned.format == VK_FORMAT_R16_UNORM &&
       host_format_16.format_signed.format == VK_FORMAT_R16_SNORM) ||
      (host_format_16.format_unsigned.format == VK_FORMAT_R16_SFLOAT &&
       host_format_16.format_signed.format == VK_FORMAT_R16_SFLOAT);
  HostFormatPair& host_format_16_16 =
      host_formats_[uint32_t(xenos::TextureFormat::k_16_16)];
  assert_true(host_format_16_16.format_unsigned.format ==
              VK_FORMAT_R16G16_UNORM);
  if ((r16g16_unorm_properties.optimalTilingFeatures &
       norm16_required_features) != norm16_required_features) {
    host_format_16_16.format_unsigned.load_shader =
        kLoadShaderIndexRG16UNormToFloat;
    host_format_16_16.format_unsigned.format = VK_FORMAT_R16G16_SFLOAT;
  }
  assert_true(host_format_16_16.format_signed.format == VK_FORMAT_R16G16_SNORM);
  if ((r16g16_snorm_properties.optimalTilingFeatures &
       norm16_required_features) != norm16_required_features) {
    host_format_16_16.format_signed.load_shader =
        kLoadShaderIndexRG16SNormToFloat;
    host_format_16_16.format_signed.format = VK_FORMAT_R16G16_SFLOAT;
  }
  host_format_16_16.unsigned_signed_compatible =
      (host_format_16_16.format_unsigned.format == VK_FORMAT_R16G16_UNORM &&
       host_format_16_16.format_signed.format == VK_FORMAT_R16G16_SNORM) ||
      (host_format_16_16.format_unsigned.format == VK_FORMAT_R16G16_SFLOAT &&
       host_format_16_16.format_signed.format == VK_FORMAT_R16G16_SFLOAT);
  HostFormatPair& host_format_16_16_16_16 =
      host_formats_[uint32_t(xenos::TextureFormat::k_16_16_16_16)];
  assert_true(host_format_16_16_16_16.format_unsigned.format ==
              VK_FORMAT_R16G16B16A16_UNORM);
  if ((r16g16b16a16_unorm_properties.optimalTilingFeatures &
       norm16_required_features) != norm16_required_features) {
    host_format_16_16_16_16.format_unsigned.load_shader =
        kLoadShaderIndexRGBA16UNormToFloat;
    host_format_16_16_16_16.format_unsigned.format =
        VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  assert_true(host_format_16_16_16_16.format_signed.format ==
              VK_FORMAT_R16G16B16A16_SNORM);
  if ((r16g16b16a16_snorm_properties.optimalTilingFeatures &
       norm16_required_features) != norm16_required_features) {
    host_format_16_16_16_16.format_signed.load_shader =
        kLoadShaderIndexRGBA16SNormToFloat;
    host_format_16_16_16_16.format_signed.format =
        VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  host_format_16_16_16_16.unsigned_signed_compatible =
      (host_format_16_16_16_16.format_unsigned.format ==
           VK_FORMAT_R16G16B16A16_UNORM &&
       host_format_16_16_16_16.format_signed.format ==
           VK_FORMAT_R16G16B16A16_SNORM) ||
      (host_format_16_16_16_16.format_unsigned.format ==
           VK_FORMAT_R16G16B16A16_SFLOAT &&
       host_format_16_16_16_16.format_signed.format ==
           VK_FORMAT_R16G16B16A16_SFLOAT);

  // Normalize format information structures.
  for (size_t i = 0; i < xe::countof(host_formats_); ++i) {
    HostFormatPair& host_format = host_formats_[i];
    // load_shader_index is left uninitialized for the tail (non-existent
    // formats), kLoadShaderIndexUnknown may be non-zero, and format support may
    // be disabled by setting the format to VK_FORMAT_UNDEFINED.
    if (host_format.format_unsigned.format == VK_FORMAT_UNDEFINED) {
      host_format.format_unsigned.load_shader = kLoadShaderIndexUnknown;
    }
    assert_false(host_format.format_unsigned.load_shader ==
                     kLoadShaderIndexUnknown &&
                 host_format.format_unsigned.format != VK_FORMAT_UNDEFINED);
    if (host_format.format_unsigned.load_shader == kLoadShaderIndexUnknown) {
      host_format.format_unsigned.format = VK_FORMAT_UNDEFINED;
      // Surely known it's unsupported with these two conditions.
      host_format.format_unsigned.linear_filterable = false;
    }
    if (host_format.format_signed.format == VK_FORMAT_UNDEFINED) {
      host_format.format_signed.load_shader = kLoadShaderIndexUnknown;
    }
    assert_false(host_format.format_signed.load_shader ==
                     kLoadShaderIndexUnknown &&
                 host_format.format_signed.format != VK_FORMAT_UNDEFINED);
    if (host_format.format_signed.load_shader == kLoadShaderIndexUnknown) {
      host_format.format_signed.format = VK_FORMAT_UNDEFINED;
      // Surely known it's unsupported with these two conditions.
      host_format.format_signed.linear_filterable = false;
    }

    // Check if the formats are supported and are linear-filterable.
    if (host_format.format_unsigned.format != VK_FORMAT_UNDEFINED) {
      ifn.vkGetPhysicalDeviceFormatProperties(
          physical_device, host_format.format_unsigned.format,
          &format_properties);
      if (format_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        host_format.format_unsigned.linear_filterable =
            (format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      } else {
        host_format.format_unsigned.format = VK_FORMAT_UNDEFINED;
        host_format.format_unsigned.load_shader = kLoadShaderIndexUnknown;
        host_format.format_unsigned.linear_filterable = false;
      }
    }
    if (host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
      ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                              host_format.format_signed.format,
                                              &format_properties);
      if (format_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        host_format.format_signed.linear_filterable =
            (format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      } else {
        host_format.format_signed.format = VK_FORMAT_UNDEFINED;
        host_format.format_signed.load_shader = kLoadShaderIndexUnknown;
        host_format.format_signed.linear_filterable = false;
      }
    }

    // Log which formats are not supported or supported via fallbacks.
    const HostFormatPair& best_host_format = kBestHostFormats[i];
    const char* guest_format_name =
        FormatInfo::GetName(xenos::TextureFormat(i));
    if (best_host_format.format_unsigned.format != VK_FORMAT_UNDEFINED) {
      assert_not_null(guest_format_name);
      if (host_format.format_unsigned.format != VK_FORMAT_UNDEFINED) {
        if (host_format.format_unsigned.format !=
            best_host_format.format_unsigned.format) {
          XELOGGPU(
              "VulkanTextureCache: Format {} (unsigned) is supported via a "
              "fallback format (using the Vulkan format {} instead of the "
              "preferred {})",
              guest_format_name, uint32_t(host_format.format_unsigned.format),
              uint32_t(best_host_format.format_unsigned.format));
        }
      } else {
        XELOGGPU(
            "VulkanTextureCache: Format {} (unsigned) is not supported by the "
            "device (preferred Vulkan format is {})",
            guest_format_name,
            uint32_t(best_host_format.format_unsigned.format));
      }
    }
    if (best_host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
      assert_not_null(guest_format_name);
      if (host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
        if (host_format.format_signed.format !=
            best_host_format.format_signed.format) {
          XELOGGPU(
              "VulkanTextureCache: Format {} (signed) is supported via a "
              "fallback format (using the Vulkan format {} instead of the "
              "preferred {})",
              guest_format_name, uint32_t(host_format.format_signed.format),
              uint32_t(best_host_format.format_signed.format));
        }
      } else {
        XELOGGPU(
            "VulkanTextureCache: Format {} (signed) is not supported by the "
            "device (preferred Vulkan format is {})",
            guest_format_name, uint32_t(best_host_format.format_signed.format));
      }
    }

    // Signednesses with different load shaders must have the data loaded
    // differently, therefore can't share the image even if the format is the
    // same. Also, if there's only one version, simplify the logic - there can't
    // be compatibility between two formats when one of them is undefined.
    if (host_format.format_unsigned.format != VK_FORMAT_UNDEFINED &&
        host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
      if (host_format.format_unsigned.load_shader ==
          host_format.format_signed.load_shader) {
        if (host_format.format_unsigned.format ==
            host_format.format_signed.format) {
          // Same format after all the fallbacks - force compatibilty.
          host_format.unsigned_signed_compatible = true;
        }
      } else {
        host_format.unsigned_signed_compatible = false;
      }
      // Formats within the same compatibility class must have the same block
      // size, though the fallbacks are configured incorrectly if that's not the
      // case (since such formats just can't be in one compatibility class).
      assert_false(host_format.unsigned_signed_compatible &&
                   host_format.format_unsigned.block_compressed !=
                       host_format.format_signed.block_compressed);
      if (host_format.unsigned_signed_compatible &&
          host_format.format_unsigned.block_compressed !=
              host_format.format_signed.block_compressed) {
        host_format.unsigned_signed_compatible = false;
      }
    } else {
      host_format.unsigned_signed_compatible = false;
    }
  }

  // Load pipeline layout.

  VkDescriptorSetLayout load_descriptor_set_layouts[kLoadDescriptorSetCount] =
      {};
  VkDescriptorSetLayout load_descriptor_set_layout_storage_buffer =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::
              kStorageBufferCompute);
  assert_true(load_descriptor_set_layout_storage_buffer != VK_NULL_HANDLE);
  load_descriptor_set_layouts[kLoadDescriptorSetIndexDestination] =
      load_descriptor_set_layout_storage_buffer;
  load_descriptor_set_layouts[kLoadDescriptorSetIndexSource] =
      load_descriptor_set_layout_storage_buffer;
  VkPushConstantRange load_pipeline_layout_push_constant_range;
  load_pipeline_layout_push_constant_range.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  load_pipeline_layout_push_constant_range.offset = 0;
  load_pipeline_layout_push_constant_range.size = sizeof(LoadConstants);
  VkPipelineLayoutCreateInfo load_pipeline_layout_create_info;
  load_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  load_pipeline_layout_create_info.pNext = nullptr;
  load_pipeline_layout_create_info.flags = 0;
  load_pipeline_layout_create_info.setLayoutCount = kLoadDescriptorSetCount;
  load_pipeline_layout_create_info.pSetLayouts = load_descriptor_set_layouts;
  load_pipeline_layout_create_info.pushConstantRangeCount = 1;
  load_pipeline_layout_create_info.pPushConstantRanges =
      &load_pipeline_layout_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &load_pipeline_layout_create_info,
                                 nullptr, &load_pipeline_layout_)) {
    XELOGE("VulkanTexture: Failed to create the texture load pipeline layout");
    return false;
  }

  // If the whole shared memory buffer fits within maxStorageBufferRange, create
  // a persistent descriptor set binding it for texture load sources, so
  // per-load transient source descriptors don't need to be allocated and
  // written. The texture's byte offset is passed via guest_offset instead, like
  // on Direct3D
  // 12. When the buffer doesn't fit, the per-load sub-range descriptors are
  // used.
  if (device_properties.maxStorageBufferRange >= SharedMemory::kBufferSize) {
    VulkanSharedMemory::BufferSliceList shared_memory_slices =
        vulkan_shared_memory.GetBufferSlices(0, SharedMemory::kBufferSize);
    if (shared_memory_slices.count != 1) {
      return false;
    }
    VkDescriptorPoolSize shared_memory_persistent_pool_size;
    shared_memory_persistent_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_persistent_pool_size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo shared_memory_persistent_pool_create_info;
    shared_memory_persistent_pool_create_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    shared_memory_persistent_pool_create_info.pNext = nullptr;
    shared_memory_persistent_pool_create_info.flags = 0;
    shared_memory_persistent_pool_create_info.maxSets = 1;
    shared_memory_persistent_pool_create_info.poolSizeCount = 1;
    shared_memory_persistent_pool_create_info.pPoolSizes =
        &shared_memory_persistent_pool_size;
    if (dfn.vkCreateDescriptorPool(
            device, &shared_memory_persistent_pool_create_info, nullptr,
            &shared_memory_persistent_descriptor_pool_) == VK_SUCCESS) {
      VkDescriptorSetAllocateInfo shared_memory_persistent_set_allocate_info;
      shared_memory_persistent_set_allocate_info.sType =
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      shared_memory_persistent_set_allocate_info.pNext = nullptr;
      shared_memory_persistent_set_allocate_info.descriptorPool =
          shared_memory_persistent_descriptor_pool_;
      shared_memory_persistent_set_allocate_info.descriptorSetCount = 1;
      shared_memory_persistent_set_allocate_info.pSetLayouts =
          &load_descriptor_set_layout_storage_buffer;
      if (dfn.vkAllocateDescriptorSets(
              device, &shared_memory_persistent_set_allocate_info,
              &shared_memory_persistent_descriptor_set_) == VK_SUCCESS) {
        VkDescriptorBufferInfo shared_memory_persistent_buffer_info;
        shared_memory_persistent_buffer_info.buffer =
            shared_memory_slices.slices[0].buffer;
        shared_memory_persistent_buffer_info.offset =
            shared_memory_slices.slices[0].buffer_offset;
        shared_memory_persistent_buffer_info.range = SharedMemory::kBufferSize;
        VkWriteDescriptorSet shared_memory_persistent_write;
        shared_memory_persistent_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shared_memory_persistent_write.pNext = nullptr;
        shared_memory_persistent_write.dstSet =
            shared_memory_persistent_descriptor_set_;
        shared_memory_persistent_write.dstBinding = 0;
        shared_memory_persistent_write.dstArrayElement = 0;
        shared_memory_persistent_write.descriptorCount = 1;
        shared_memory_persistent_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        shared_memory_persistent_write.pImageInfo = nullptr;
        shared_memory_persistent_write.pBufferInfo =
            &shared_memory_persistent_buffer_info;
        shared_memory_persistent_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &shared_memory_persistent_write,
                                   0, nullptr);
      } else {
        dfn.vkDestroyDescriptorPool(
            device, shared_memory_persistent_descriptor_pool_, nullptr);
        shared_memory_persistent_descriptor_pool_ = VK_NULL_HANDLE;
        shared_memory_persistent_descriptor_set_ = VK_NULL_HANDLE;
      }
    }
  }

  // Load pipelines, only the ones needed for the formats that will be used.

  bool load_shaders_needed[kLoadShaderCount] = {};
  for (size_t i = 0; i < xe::countof(host_formats_); ++i) {
    const HostFormatPair& host_format = host_formats_[i];
    if (host_format.format_unsigned.load_shader != kLoadShaderIndexUnknown) {
      load_shaders_needed[host_format.format_unsigned.load_shader] = true;
    }
    if (host_format.format_signed.load_shader != kLoadShaderIndexUnknown) {
      load_shaders_needed[host_format.format_signed.load_shader] = true;
    }
  }
  if (kHostFormatGBGRUnaligned.format_unsigned.load_shader !=
      kLoadShaderIndexUnknown) {
    load_shaders_needed[kHostFormatGBGRUnaligned.format_unsigned.load_shader] =
        true;
  }
  if (kHostFormatGBGRUnaligned.format_signed.load_shader !=
      kLoadShaderIndexUnknown) {
    load_shaders_needed[kHostFormatGBGRUnaligned.format_signed.load_shader] =
        true;
  }
  if (kHostFormatBGRGUnaligned.format_unsigned.load_shader !=
      kLoadShaderIndexUnknown) {
    load_shaders_needed[kHostFormatBGRGUnaligned.format_unsigned.load_shader] =
        true;
  }
  if (kHostFormatBGRGUnaligned.format_signed.load_shader !=
      kLoadShaderIndexUnknown) {
    load_shaders_needed[kHostFormatBGRGUnaligned.format_signed.load_shader] =
        true;
  }

  std::pair<const uint32_t*, size_t> load_shader_code[kLoadShaderCount] = {};
  load_shader_code[kLoadShaderIndex8bpb] = std::make_pair(
      shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs));
  load_shader_code[kLoadShaderIndex16bpb] = std::make_pair(
      shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs));
  load_shader_code[kLoadShaderIndex32bpb] = std::make_pair(
      shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs));
  load_shader_code[kLoadShaderIndex64bpb] = std::make_pair(
      shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs));
  load_shader_code[kLoadShaderIndex128bpb] = std::make_pair(
      shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs));
  load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
      std::make_pair(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs,
                     sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs));
  load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] =
      std::make_pair(shaders::texture_load_r5g6b5_b5g6r5_cs,
                     sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs));
  load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
      std::make_pair(
          shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
          sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs));
  load_shader_code[kLoadShaderIndexRGBA4ToARGB4] =
      std::make_pair(shaders::texture_load_r4g4b4a4_a4r4g4b4_cs,
                     sizeof(shaders::texture_load_r4g4b4a4_a4r4g4b4_cs));
  load_shader_code[kLoadShaderIndexGBGR8ToRGB8] =
      std::make_pair(shaders::texture_load_gbgr8_rgb8_cs,
                     sizeof(shaders::texture_load_gbgr8_rgb8_cs));
  load_shader_code[kLoadShaderIndexBGRG8ToRGB8] =
      std::make_pair(shaders::texture_load_bgrg8_rgb8_cs,
                     sizeof(shaders::texture_load_bgrg8_rgb8_cs));
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] =
      std::make_pair(shaders::texture_load_r10g11b11_rgba16_cs,
                     sizeof(shaders::texture_load_r10g11b11_rgba16_cs));
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
      std::make_pair(shaders::texture_load_r10g11b11_rgba16_snorm_cs,
                     sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs));
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] =
      std::make_pair(shaders::texture_load_r11g11b10_rgba16_cs,
                     sizeof(shaders::texture_load_r11g11b10_rgba16_cs));
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
      std::make_pair(shaders::texture_load_r11g11b10_rgba16_snorm_cs,
                     sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs));
  load_shader_code[kLoadShaderIndexR16UNormToFloat] =
      std::make_pair(shaders::texture_load_r16_unorm_float_cs,
                     sizeof(shaders::texture_load_r16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexR16SNormToFloat] =
      std::make_pair(shaders::texture_load_r16_snorm_float_cs,
                     sizeof(shaders::texture_load_r16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexRG16UNormToFloat] =
      std::make_pair(shaders::texture_load_rg16_unorm_float_cs,
                     sizeof(shaders::texture_load_rg16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexRG16SNormToFloat] =
      std::make_pair(shaders::texture_load_rg16_snorm_float_cs,
                     sizeof(shaders::texture_load_rg16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexRGBA16UNormToFloat] =
      std::make_pair(shaders::texture_load_rgba16_unorm_float_cs,
                     sizeof(shaders::texture_load_rgba16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexRGBA16SNormToFloat] =
      std::make_pair(shaders::texture_load_rgba16_snorm_float_cs,
                     sizeof(shaders::texture_load_rgba16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexDXT1ToRGBA8] =
      std::make_pair(shaders::texture_load_dxt1_rgba8_cs,
                     sizeof(shaders::texture_load_dxt1_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXT1ToASTC4x4] =
      std::make_pair(shaders::texture_load_dxt1_astc4x4_cs,
                     sizeof(shaders::texture_load_dxt1_astc4x4_cs));
  load_shader_code[kLoadShaderIndexDXT5ToASTC4x4] =
      std::make_pair(shaders::texture_load_dxt5_astc4x4_cs,
                     sizeof(shaders::texture_load_dxt5_astc4x4_cs));
  load_shader_code[kLoadShaderIndexDXNToEACRG11] =
      std::make_pair(shaders::texture_load_dxn_eac_rg11_cs,
                     sizeof(shaders::texture_load_dxn_eac_rg11_cs));
  load_shader_code[kLoadShaderIndexDXT3ToRGBA8] =
      std::make_pair(shaders::texture_load_dxt3_rgba8_cs,
                     sizeof(shaders::texture_load_dxt3_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXT5ToRGBA8] =
      std::make_pair(shaders::texture_load_dxt5_rgba8_cs,
                     sizeof(shaders::texture_load_dxt5_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXNToRG8] =
      std::make_pair(shaders::texture_load_dxn_rg8_cs,
                     sizeof(shaders::texture_load_dxn_rg8_cs));
  load_shader_code[kLoadShaderIndexDXT3A] = std::make_pair(
      shaders::texture_load_dxt3a_cs, sizeof(shaders::texture_load_dxt3a_cs));
  load_shader_code[kLoadShaderIndexDXT3AAs1111ToARGB4] =
      std::make_pair(shaders::texture_load_dxt3aas1111_argb4_cs,
                     sizeof(shaders::texture_load_dxt3aas1111_argb4_cs));
  load_shader_code[kLoadShaderIndexDXT5AToR8] =
      std::make_pair(shaders::texture_load_dxt5a_r8_cs,
                     sizeof(shaders::texture_load_dxt5a_r8_cs));
  load_shader_code[kLoadShaderIndexCTX1] = std::make_pair(
      shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs));
  load_shader_code[kLoadShaderIndexDepthUnorm] =
      std::make_pair(shaders::texture_load_depth_unorm_cs,
                     sizeof(shaders::texture_load_depth_unorm_cs));
  load_shader_code[kLoadShaderIndexDepthFloat] =
      std::make_pair(shaders::texture_load_depth_float_cs,
                     sizeof(shaders::texture_load_depth_float_cs));
  std::pair<const uint32_t*, size_t> load_shader_code_scaled[kLoadShaderCount] =
      {};
  if (IsDrawResolutionScaled()) {
    load_shader_code_scaled[kLoadShaderIndex8bpb] =
        std::make_pair(shaders::texture_load_8bpb_scaled_cs,
                       sizeof(shaders::texture_load_8bpb_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndex16bpb] =
        std::make_pair(shaders::texture_load_16bpb_scaled_cs,
                       sizeof(shaders::texture_load_16bpb_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndex32bpb] =
        std::make_pair(shaders::texture_load_32bpb_scaled_cs,
                       sizeof(shaders::texture_load_32bpb_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndex64bpb] =
        std::make_pair(shaders::texture_load_64bpb_scaled_cs,
                       sizeof(shaders::texture_load_64bpb_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndex128bpb] =
        std::make_pair(shaders::texture_load_128bpb_scaled_cs,
                       sizeof(shaders::texture_load_128bpb_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
        std::make_pair(
            shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs,
            sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR5G6B5ToB5G6R5] =
        std::make_pair(shaders::texture_load_r5g6b5_b5g6r5_scaled_cs,
                       sizeof(shaders::texture_load_r5g6b5_b5g6r5_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
        std::make_pair(
            shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs,
            sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexRGBA4ToARGB4] = std::make_pair(
        shaders::texture_load_r4g4b4a4_a4r4g4b4_scaled_cs,
        sizeof(shaders::texture_load_r4g4b4a4_a4r4g4b4_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16] = std::make_pair(
        shaders::texture_load_r10g11b11_rgba16_scaled_cs,
        sizeof(shaders::texture_load_r10g11b11_rgba16_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
        std::make_pair(
            shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs,
            sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16] = std::make_pair(
        shaders::texture_load_r11g11b10_rgba16_scaled_cs,
        sizeof(shaders::texture_load_r11g11b10_rgba16_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
        std::make_pair(
            shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs,
            sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR16UNormToFloat] =
        std::make_pair(shaders::texture_load_r16_unorm_float_scaled_cs,
                       sizeof(shaders::texture_load_r16_unorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexR16SNormToFloat] =
        std::make_pair(shaders::texture_load_r16_snorm_float_scaled_cs,
                       sizeof(shaders::texture_load_r16_snorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexRG16UNormToFloat] = std::make_pair(
        shaders::texture_load_rg16_unorm_float_scaled_cs,
        sizeof(shaders::texture_load_rg16_unorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexRG16SNormToFloat] = std::make_pair(
        shaders::texture_load_rg16_snorm_float_scaled_cs,
        sizeof(shaders::texture_load_rg16_snorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexRGBA16UNormToFloat] =
        std::make_pair(
            shaders::texture_load_rgba16_unorm_float_scaled_cs,
            sizeof(shaders::texture_load_rgba16_unorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexRGBA16SNormToFloat] =
        std::make_pair(
            shaders::texture_load_rgba16_snorm_float_scaled_cs,
            sizeof(shaders::texture_load_rgba16_snorm_float_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexDepthUnorm] =
        std::make_pair(shaders::texture_load_depth_unorm_scaled_cs,
                       sizeof(shaders::texture_load_depth_unorm_scaled_cs));
    load_shader_code_scaled[kLoadShaderIndexDepthFloat] =
        std::make_pair(shaders::texture_load_depth_float_scaled_cs,
                       sizeof(shaders::texture_load_depth_float_scaled_cs));
  }

  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    if (!load_shaders_needed[i]) {
      continue;
    }
    const std::pair<const uint32_t*, size_t>& current_load_shader_code =
        load_shader_code[i];
    assert_not_null(current_load_shader_code.first);
    load_pipelines_[i] = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, load_pipeline_layout_, current_load_shader_code.first,
        current_load_shader_code.second);
    if (load_pipelines_[i] == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanTextureCache: Failed to create the texture loading pipeline "
          "for shader {}",
          i);
      return false;
    }
    if (IsDrawResolutionScaled()) {
      const std::pair<const uint32_t*, size_t>&
          current_load_shader_code_scaled = load_shader_code_scaled[i];
      if (current_load_shader_code_scaled.first) {
        load_pipelines_scaled_[i] = ui::vulkan::util::CreateComputePipeline(
            vulkan_device, load_pipeline_layout_,
            current_load_shader_code_scaled.first,
            current_load_shader_code_scaled.second);
        if (load_pipelines_scaled_[i] == VK_NULL_HANDLE) {
          XELOGE(
              "VulkanTextureCache: Failed to create the resolution-scaled "
              "texture loading pipeline for shader {}",
              i);
          return false;
        }
      }
    }
  }

  // Null images as a replacement for unneeded bindings and for bindings for
  // which the real image hasn't been created.
  // TODO(Triang3l): Use VK_EXT_robustness2 null descriptors.

  VkImageCreateInfo null_image_create_info;
  null_image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  null_image_create_info.pNext = nullptr;
  null_image_create_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  null_image_create_info.imageType = VK_IMAGE_TYPE_2D;
  // Four components to return (0, 0, 0, 0).
  // TODO(Triang3l): Find the return value for invalid texture fetch constants
  // on the real hardware.
  null_image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  null_image_create_info.extent.width = 1;
  null_image_create_info.extent.height = 1;
  null_image_create_info.extent.depth = 1;
  null_image_create_info.mipLevels = 1;
  null_image_create_info.arrayLayers = 6;
  null_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  null_image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  null_image_create_info.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  null_image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  null_image_create_info.queueFamilyIndexCount = 0;
  null_image_create_info.pQueueFamilyIndices = nullptr;
  null_image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device, &null_image_create_info, nullptr,
                        &null_image_2d_array_cube_) != VK_SUCCESS) {
    XELOGE(
        "VulkanTextureCache: Failed to create the null 2D array and cube "
        "image");
    return false;
  }

  null_image_create_info.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  null_image_create_info.imageType = VK_IMAGE_TYPE_3D;
  null_image_create_info.arrayLayers = 1;
  if (dfn.vkCreateImage(device, &null_image_create_info, nullptr,
                        &null_image_3d_) != VK_SUCCESS) {
    XELOGE("VulkanTextureCache: Failed to create the null 3D image");
    return false;
  }

  VkMemoryRequirements null_image_memory_requirements_2d_array_cube_;
  dfn.vkGetImageMemoryRequirements(
      device, null_image_2d_array_cube_,
      &null_image_memory_requirements_2d_array_cube_);
  VkMemoryRequirements null_image_memory_requirements_3d_;
  dfn.vkGetImageMemoryRequirements(device, null_image_3d_,
                                   &null_image_memory_requirements_3d_);
  uint32_t null_image_memory_type_common = ui::vulkan::util::ChooseMemoryType(
      vulkan_device->memory_types(),
      null_image_memory_requirements_2d_array_cube_.memoryTypeBits &
          null_image_memory_requirements_3d_.memoryTypeBits,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal);
  if (null_image_memory_type_common != UINT32_MAX) {
    // Place both null images in one memory allocation because maximum total
    // memory allocation count is limited.
    VkDeviceSize null_image_memory_offset_3d_ =
        xe::align(null_image_memory_requirements_2d_array_cube_.size,
                  std::max(null_image_memory_requirements_3d_.alignment,
                           VkDeviceSize(1)));
    VkMemoryAllocateInfo null_image_memory_allocate_info;
    null_image_memory_allocate_info.sType =
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    null_image_memory_allocate_info.pNext = nullptr;
    null_image_memory_allocate_info.allocationSize =
        null_image_memory_offset_3d_ + null_image_memory_requirements_3d_.size;
    null_image_memory_allocate_info.memoryTypeIndex =
        null_image_memory_type_common;
    if (dfn.vkAllocateMemory(device, &null_image_memory_allocate_info, nullptr,
                             &null_images_memory_[0]) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to allocate the memory for the null "
          "images");
      return false;
    }
    if (dfn.vkBindImageMemory(device, null_image_2d_array_cube_,
                              null_images_memory_[0], 0) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to bind the memory to the null 2D array "
          "and cube image");
      return false;
    }
    if (dfn.vkBindImageMemory(device, null_image_3d_, null_images_memory_[0],
                              null_image_memory_offset_3d_) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to bind the memory to the null 3D image");
      return false;
    }
  } else {
    // Place each null image in separate allocations.
    const uint32_t null_image_memory_type_2d_array_cube =
        ui::vulkan::util::ChooseMemoryType(
            vulkan_device->memory_types(),
            null_image_memory_requirements_2d_array_cube_.memoryTypeBits,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal);
    const uint32_t null_image_memory_type_3d =
        ui::vulkan::util::ChooseMemoryType(
            vulkan_device->memory_types(),
            null_image_memory_requirements_3d_.memoryTypeBits,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal);
    if (null_image_memory_type_2d_array_cube == UINT32_MAX ||
        null_image_memory_type_3d == UINT32_MAX) {
      XELOGE(
          "VulkanTextureCache: Failed to get the memory types for the null "
          "images");
      return false;
    }

    VkMemoryAllocateInfo null_image_memory_allocate_info;
    VkMemoryAllocateInfo* null_image_memory_allocate_info_last =
        &null_image_memory_allocate_info;
    null_image_memory_allocate_info.sType =
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    null_image_memory_allocate_info.pNext = nullptr;
    null_image_memory_allocate_info.allocationSize =
        null_image_memory_requirements_2d_array_cube_.size;
    null_image_memory_allocate_info.memoryTypeIndex =
        null_image_memory_type_2d_array_cube;
    VkMemoryDedicatedAllocateInfo null_image_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      null_image_memory_allocate_info_last->pNext =
          &null_image_memory_dedicated_allocate_info;
      null_image_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(
              &null_image_memory_dedicated_allocate_info);
      null_image_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      null_image_memory_dedicated_allocate_info.pNext = nullptr;
      null_image_memory_dedicated_allocate_info.image =
          null_image_2d_array_cube_;
      null_image_memory_dedicated_allocate_info.buffer = VK_NULL_HANDLE;
    }
    if (dfn.vkAllocateMemory(device, &null_image_memory_allocate_info, nullptr,
                             &null_images_memory_[0]) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to allocate the memory for the null 2D "
          "array and cube image");
      return false;
    }
    if (dfn.vkBindImageMemory(device, null_image_2d_array_cube_,
                              null_images_memory_[0], 0) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to bind the memory to the null 2D array "
          "and cube image");
      return false;
    }

    null_image_memory_allocate_info.allocationSize =
        null_image_memory_requirements_3d_.size;
    null_image_memory_allocate_info.memoryTypeIndex = null_image_memory_type_3d;
    null_image_memory_dedicated_allocate_info.image = null_image_3d_;
    if (dfn.vkAllocateMemory(device, &null_image_memory_allocate_info, nullptr,
                             &null_images_memory_[1]) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to allocate the memory for the null 3D "
          "image");
      return false;
    }
    if (dfn.vkBindImageMemory(device, null_image_3d_, null_images_memory_[1],
                              0) != VK_SUCCESS) {
      XELOGE(
          "VulkanTextureCache: Failed to bind the memory to the null 3D image");
      return false;
    }
  }

  VkImageViewCreateInfo null_image_view_create_info;
  null_image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  null_image_view_create_info.pNext = nullptr;
  null_image_view_create_info.flags = 0;
  null_image_view_create_info.image = null_image_2d_array_cube_;
  null_image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  null_image_view_create_info.format = null_image_create_info.format;
  // TODO(Triang3l): Find the return value for invalid texture fetch constants
  // on the real hardware.
  // Micro-optimization if this has any effect on the host GPU at all, use only
  // constant components instead of the real texels. The image will be cleared
  // to (0, 0, 0, 0) anyway.
  VkComponentSwizzle null_image_view_swizzle =
      device_properties.imageViewFormatSwizzle ? VK_COMPONENT_SWIZZLE_ZERO
                                               : VK_COMPONENT_SWIZZLE_IDENTITY;
  null_image_view_create_info.components.r = null_image_view_swizzle;
  null_image_view_create_info.components.g = null_image_view_swizzle;
  null_image_view_create_info.components.b = null_image_view_swizzle;
  null_image_view_create_info.components.a = null_image_view_swizzle;
  null_image_view_create_info.subresourceRange =
      ui::vulkan::util::InitializeSubresourceRange(
          VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1);
  if (dfn.vkCreateImageView(device, &null_image_view_create_info, nullptr,
                            &null_image_view_2d_array_) != VK_SUCCESS) {
    XELOGE("VulkanTextureCache: Failed to create the null 2D array image view");
    return false;
  }
  null_image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  null_image_view_create_info.subresourceRange.layerCount = 6;
  if (dfn.vkCreateImageView(device, &null_image_view_create_info, nullptr,
                            &null_image_view_cube_) != VK_SUCCESS) {
    XELOGE("VulkanTextureCache: Failed to create the null cube image view");
    return false;
  }
  null_image_view_create_info.image = null_image_3d_;
  null_image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
  null_image_view_create_info.subresourceRange.layerCount = 1;
  if (dfn.vkCreateImageView(device, &null_image_view_create_info, nullptr,
                            &null_image_view_3d_) != VK_SUCCESS) {
    XELOGE("VulkanTextureCache: Failed to create the null 3D image view");
    return false;
  }

  null_images_cleared_ = false;

  // Samplers.

  // Some MoltenVK devices have a maximum of 2048, 1024, or even 96 samplers,
  // below Vulkan's minimum requirement of 4000.
  // Assuming that the current VulkanTextureCache is the only one on this
  // VkDevice (true in a regular emulation scenario), so taking over all the
  // allocation slots exclusively.
  // Also leaving a few slots for use by things like overlay applications.
  sampler_max_count_ = device_properties.maxSamplerAllocationCount -
                       ui::vulkan::UISamplers::kSamplerCount - 16;
  if (command_processor_.bindless_textures()) {
    // Every live sampler needs a slot in the bindless array; the least recently
    // used one is evicted beyond that.
    sampler_max_count_ = std::min(
        sampler_max_count_,
        SpirvShaderTranslator::kBindlessSlotCounts
            [SpirvShaderTranslator::kBindlessBindingSamplers]);
  }

  if (device_properties.samplerAnisotropy) {
    max_anisotropy_ = xenos::AnisoFilter(
        uint32_t(xenos::AnisoFilter::kMax_1_1) +
        (31 -
         xe::lzcnt(uint32_t(std::min(
             16.0f, std::max(1.0f, device_properties.maxSamplerAnisotropy))))));
  } else {
    max_anisotropy_ = xenos::AnisoFilter::kDisabled;
  }

  // Initialize sparse scaled resolve buffers if draw resolution scaling is
  // enabled and sparse binding is supported.
  if (!InitializeSparseScaledResolve()) {
    return false;
  }

  return true;
}

const VulkanTextureCache::HostFormatPair& VulkanTextureCache::GetHostFormatPair(
    TextureKey key) const {
  if (key.format == xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP &&
      (key.GetWidth() & 1)) {
    return kHostFormatGBGRUnaligned;
  }
  if (key.format == xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP &&
      (key.GetWidth() & 1)) {
    return kHostFormatBGRGUnaligned;
  }
  return host_formats_[uint32_t(key.format)];
}

void VulkanTextureCache::GetTextureUsageMasks(VulkanTexture::Usage usage,
                                              VkPipelineStageFlags& stage_mask,
                                              VkAccessFlags& access_mask,
                                              VkImageLayout& layout) {
  stage_mask = 0;
  access_mask = 0;
  layout = VK_IMAGE_LAYOUT_UNDEFINED;
  switch (usage) {
    case VulkanTexture::Usage::kUndefined:
      // For UNDEFINED layout, use TOP_OF_PIPE as source stage (wait for
      // nothing) with no access mask (discarding old contents).
      stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      break;
    case VulkanTexture::Usage::kDepthSurfaceComputeRead:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      break;
    case VulkanTexture::Usage::kDepthSurfaceAttachment:
      stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      break;
    case VulkanTexture::Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      break;
    case VulkanTexture::Usage::kGuestShaderSampled:
      stage_mask = guest_shader_pipeline_stages_;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      break;
    case VulkanTexture::Usage::kSwapSampled:
      // Read by the presentation gamma pass, which is a compute shader here
      // (a fragment shader upstream).
      stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      break;
  }
}

xenos::ClampMode VulkanTextureCache::NormalizeClampMode(
    xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No GL_CLAMP (clamp to half edge, half border) equivalent in Vulkan, but
    // there's no Direct3D 9 equivalent anyway, and too weird to be suitable for
    // intentional real usage.
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToEdge ||
      clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    // No equivalents for anything other than kMirrorClampToEdge in Vulkan.
    return command_processor_.GetVulkanDevice()
                   ->properties()
                   .samplerMirrorClampToEdge
               ? xenos::ClampMode::kMirrorClampToEdge
               : xenos::ClampMode::kMirroredRepeat;
  }
  return clamp_mode;
}

bool VulkanTextureCache::EnsureScaledResolveMemoryCommitted(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return true;
  }

  // Dispatch to sparse implementation if supported
  if (sparse_scaled_resolve_supported_) {
    return EnsureScaledResolveMemoryCommittedSparse(
        start_unscaled, length_unscaled, length_scaled_alignment_log2);
  }

  if (length_unscaled == 0) {
    return true;
  }

  if (start_unscaled > SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  uint32_t draw_resolution_scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits =
      (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  return EnsureScaledResolveSpan(start_scaled, start_scaled + length_scaled);
}

bool VulkanTextureCache::EnsureScaledResolveSpan(uint64_t start_scaled,
                                                 uint64_t end_scaled) {
  constexpr uint64_t kGranuleMask = uint64_t(kScaledResolveHeapSize) - 1;
  uint64_t granule_start = start_scaled & ~kGranuleMask;
  uint64_t granule_end = (end_scaled + kGranuleMask) & ~kGranuleMask;
  // Spans overlapping the granules of the range: [first, last).
  size_t span_count = scaled_resolve_buffers_.size();
  size_t first = 0;
  while (first < span_count &&
         scaled_resolve_buffers_[first].range_start_scaled +
                 scaled_resolve_buffers_[first].range_length_scaled <=
             granule_start) {
    ++first;
  }
  size_t last = first;
  while (last < span_count &&
         scaled_resolve_buffers_[last].range_start_scaled < granule_end) {
    ++last;
  }
  if (last - first == 1) {
    const ScaledResolveBuffer& span = scaled_resolve_buffers_[first];
    if (span.range_start_scaled <= start_scaled &&
        span.range_start_scaled + span.range_length_scaled >= end_scaled) {
      return true;
    }
  }
  uint64_t new_start = granule_start, new_end = granule_end;
  if (last > first) {
    const ScaledResolveBuffer& span_last = scaled_resolve_buffers_[last - 1];
    new_start =
        std::min(new_start, scaled_resolve_buffers_[first].range_start_scaled);
    new_end = std::max(
        new_end, span_last.range_start_scaled + span_last.range_length_scaled);
    if (!command_processor_.submission_open()) {
      XELOGE(
          "VulkanTextureCache: Can't merge scaled resolve spans outside a "
          "submission");
      return false;
    }
  }

  ScaledResolveBuffer new_span;
  new_span.size = new_end - new_start;
  new_span.range_start_scaled = new_start;
  new_span.range_length_scaled = new_span.size;
  VkBufferCreateInfo buffer_create_info = {};
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.size = new_span.size;
  buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo allocation_create_info = {};
  allocation_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  VkResult result = vmaCreateBuffer(vma_allocator_, &buffer_create_info,
                                    &allocation_create_info, &new_span.buffer,
                                    &new_span.allocation, nullptr);
  if (result != VK_SUCCESS) {
    XELOGE(
        "VulkanTextureCache: Failed to create a {} MB scaled resolve span: {}",
        new_span.size >> 20, int(result));
    return false;
  }

  uint64_t old_bytes = 0;
  if (last > first) {
    // Carry the merged spans' contents over, ordered after everything already
    // recorded that accesses them.
    command_processor_.SubmitBarriers(true);
    DeferredCommandBuffer& command_buffer =
        command_processor_.deferred_command_buffer();
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                        &barrier, 0, nullptr, 0, nullptr);
    uint64_t submission_current = command_processor_.GetCurrentSubmission();
    for (size_t i = first; i < last; ++i) {
      const ScaledResolveBuffer& old_span = scaled_resolve_buffers_[i];
      VkBufferCopy region;
      region.srcOffset = 0;
      region.dstOffset = old_span.range_start_scaled - new_start;
      region.size = old_span.range_length_scaled;
      command_buffer.CmdVkCopyBuffer(old_span.buffer, new_span.buffer, 1,
                                     &region);
      old_bytes += old_span.size;
      scaled_resolve_retired_buffers_.emplace_back(submission_current,
                                                   old_span);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                            VK_ACCESS_MEMORY_WRITE_BIT;
    command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                        1, &barrier, 0, nullptr, 0, nullptr);
  }
  scaled_resolve_buffers_.erase(scaled_resolve_buffers_.begin() + first,
                                scaled_resolve_buffers_.begin() + last);
  scaled_resolve_buffers_.insert(scaled_resolve_buffers_.begin() + first,
                                 new_span);
  scaled_resolve_span_bytes_ += new_span.size - old_bytes;
  XELOGI(
      "VulkanTextureCache: scaled resolve span 0x{:X}-0x{:X} ({} merged), {} "
      "spans, {} MB total",
      new_start, new_end, last - first, scaled_resolve_buffers_.size(),
      scaled_resolve_span_bytes_ >> 20);

  // Spans were replaced - keep the current buffer pointing at the current
  // range.
  scaled_resolve_current_buffer_index_ = SIZE_MAX;
  if (scaled_resolve_current_range_length_scaled_) {
    for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
      const ScaledResolveBuffer& span = scaled_resolve_buffers_[i];
      if (span.range_start_scaled <=
              scaled_resolve_current_range_start_scaled_ &&
          span.range_start_scaled + span.range_length_scaled >=
              scaled_resolve_current_range_start_scaled_ +
                  scaled_resolve_current_range_length_scaled_) {
        scaled_resolve_current_buffer_index_ = i;
        break;
      }
    }
  }
  return true;
}

void VulkanTextureCache::ReleaseRetiredScaledResolveBuffers(
    uint64_t completed_submission) {
  auto it = scaled_resolve_retired_buffers_.begin();
  while (it != scaled_resolve_retired_buffers_.end()) {
    if (it->first <= completed_submission) {
      vmaDestroyBuffer(vma_allocator_, it->second.buffer,
                       it->second.allocation);
      it = scaled_resolve_retired_buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool VulkanTextureCache::MakeScaledResolveRangeCurrent(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  // Dispatch to sparse implementation if supported
  if (sparse_scaled_resolve_supported_) {
    return MakeScaledResolveRangeCurrentSparse(start_unscaled, length_unscaled,
                                               length_scaled_alignment_log2);
  }

  // Simple non-overlapping buffer implementation (fallback)
  // First ensure the memory is committed (creates buffers if needed)
  if (!EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                          length_scaled_alignment_log2)) {
    return false;
  }

  const uint32_t draw_resolution_scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits =
      (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  uint64_t end_scaled = start_scaled + length_scaled;

  // Find which buffer contains this entire range (not just the start)
  for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
    const ScaledResolveBuffer& buffer = scaled_resolve_buffers_[i];
    if (start_scaled >= buffer.range_start_scaled &&
        end_scaled <=
            (buffer.range_start_scaled + buffer.range_length_scaled)) {
      scaled_resolve_current_buffer_index_ = i;
      scaled_resolve_current_range_start_scaled_ = start_scaled;
      scaled_resolve_current_range_length_scaled_ = length_scaled;
      return true;
    }
  }

  return false;
}

VkBuffer VulkanTextureCache::GetCurrentScaledResolveBuffer() const {
  if (sparse_scaled_resolve_supported_) {
    if (scaled_resolve_current_buffer_index_ < kMaxScaledResolveSparseBuffers &&
        scaled_resolve_sparse_buffers_[scaled_resolve_current_buffer_index_]) {
      return scaled_resolve_sparse_buffers_
          [scaled_resolve_current_buffer_index_]
              ->buffer();
    }
    return VK_NULL_HANDLE;
  }
  if (scaled_resolve_current_buffer_index_ >= scaled_resolve_buffers_.size()) {
    return VK_NULL_HANDLE;
  }
  return scaled_resolve_buffers_[scaled_resolve_current_buffer_index_].buffer;
}

// Sparse scaled resolve helper functions

size_t VulkanTextureCache::GetScaledResolveSparseBufferCount() const {
  // Each buffer is 2GB, and buffer N covers [N GB .. (N+2) GB)
  // So for an address space of X bytes, we need ceil((X-1) / 1GB) buffers
  // (or 0 if X <= 2GB, but we handle that with max)
  uint64_t address_space = uint64_t(SharedMemory::kBufferSize) *
                           draw_resolution_scale_x() *
                           draw_resolution_scale_y();
  // Number of buffers needed: ceil((address_space - 1) / 1GB)
  // For 3x3 (4.5GB): (4.5GB - 1) / 1GB = 3.5 -> 4 buffers
  // For 2x2 (2GB): (2GB - 1) / 1GB = 0.99 -> 1 buffer
  // For 1x1 (512MB): (512MB - 1) / 1GB = 0 -> but we need at least 1
  if (address_space <= kScaledResolveSparseBufferSize) {
    return 1;
  }
  return size_t((address_space - 1) >> 30);
}

std::array<size_t, 2> VulkanTextureCache::GetPossibleScaledResolveBufferIndices(
    uint64_t address_scaled) const {
  // Given an address, find which buffers could contain it.
  // Buffer N covers [N GB .. (N+2) GB), so address A could be in:
  // - Buffer floor(A/1GB) if it exists (address is in [N GB .. (N+1) GB) part)
  // - Buffer floor(A/1GB) - 1 if it exists (address is in [(N-1)+1 GB .. N+1
  // GB) part)
  size_t gb_index = size_t(address_scaled >> 30);
  size_t buffer_count = GetScaledResolveSparseBufferCount();
  size_t max_buffer_index = buffer_count > 0 ? buffer_count - 1 : 0;

  // First possible buffer: the one starting at this GB
  size_t buffer_a = std::min(gb_index, max_buffer_index);
  // Second possible buffer: the one starting 1GB earlier (if it exists)
  size_t buffer_b = gb_index > 0 ? std::min(gb_index - 1, max_buffer_index)
                                 : max_buffer_index + 1;  // Invalid

  return {buffer_a, buffer_b};
}

bool VulkanTextureCache::InitializeSparseScaledResolve() {
  if (!IsDrawResolutionScaled()) {
    return true;
  }

  if (!cvars::tiled_shared_memory) {
    XELOGI("VulkanTextureCache: Sparse scaled resolve disabled by CVAR");
    return true;
  }

  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  if (!device_properties.sparseBinding ||
      !device_properties.sparseResidencyBuffer) {
    XELOGI(
        "VulkanTextureCache: Sparse binding not supported, using simple "
        "buffers");
    return true;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  size_t buffer_count = GetScaledResolveSparseBufferCount();
  XELOGI(
      "VulkanTextureCache: Creating {} sparse scaled resolve buffers (2GB "
      "each)",
      buffer_count);

  // Create the sparse buffers
  for (size_t i = 0; i < buffer_count; ++i) {
    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_create_info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                               VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    buffer_create_info.size = kScaledResolveSparseBufferSize;
    buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    VkResult result =
        dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer);
    if (result != VK_SUCCESS) {
      XELOGE("VulkanTextureCache: Failed to create sparse buffer {}: error {}",
             i, static_cast<int>(result));
      ShutdownSparseScaledResolve();
      return true;  // Fall back to simple buffers
    }

    scaled_resolve_sparse_buffers_[i] =
        std::make_unique<ScaledResolveSparseBuffer>(buffer);
  }

  // Get memory requirements for the sparse buffers
  VkMemoryRequirements memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device,
                                    scaled_resolve_sparse_buffers_[0]->buffer(),
                                    &memory_requirements);

  // Find a suitable device-local memory type
  if (!xe::bit_scan_forward(memory_requirements.memoryTypeBits &
                                vulkan_device->memory_types().device_local,
                            &scaled_resolve_memory_type_)) {
    XELOGE(
        "VulkanTextureCache: Failed to find suitable memory type for sparse "
        "buffers");
    ShutdownSparseScaledResolve();
    return true;
  }

  // Calculate the number of heaps we might need
  uint64_t address_space = uint64_t(SharedMemory::kBufferSize) *
                           draw_resolution_scale_x() *
                           draw_resolution_scale_y();
  uint32_t max_heap_count =
      uint32_t((address_space + kScaledResolveHeapSize - 1) >>
               kScaledResolveHeapSizeLog2);
  scaled_resolve_heaps_.resize(max_heap_count, VK_NULL_HANDLE);
  scaled_resolve_heap_count_ = 0;

  sparse_scaled_resolve_supported_ = true;
  XELOGI(
      "VulkanTextureCache: Sparse scaled resolve initialized with {} buffers, "
      "max {} heaps",
      buffer_count, max_heap_count);

  return true;
}

void VulkanTextureCache::ShutdownSparseScaledResolve() {
  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Free all heaps
  for (VkDeviceMemory heap : scaled_resolve_heaps_) {
    if (heap != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, heap, nullptr);
    }
  }
  scaled_resolve_heaps_.clear();
  scaled_resolve_heap_count_ = 0;

  // Destroy sparse buffers
  for (auto& buffer : scaled_resolve_sparse_buffers_) {
    if (buffer) {
      dfn.vkDestroyBuffer(device, buffer->buffer(), nullptr);
      buffer.reset();
    }
  }

  scaled_resolve_memory_type_ = UINT32_MAX;
  sparse_scaled_resolve_supported_ = false;
}

void VulkanTextureCache::BindHeapToOverlappingBuffers(uint32_t heap_index,
                                                      VkDeviceMemory heap) {
  // Calculate the address this heap covers
  uint64_t heap_address = uint64_t(heap_index) << kScaledResolveHeapSizeLog2;

  // Find which buffers this heap should be bound to
  auto [buffer_a, buffer_b] =
      GetPossibleScaledResolveBufferIndices(heap_address);

  size_t buffer_count = GetScaledResolveSparseBufferCount();

  // Bind to the first possible buffer
  if (buffer_a < buffer_count && scaled_resolve_sparse_buffers_[buffer_a]) {
    uint64_t offset_in_buffer =
        heap_address - (uint64_t(buffer_a) << 30);  // buffer_a * 1GB

    VkSparseMemoryBind bind = {};
    bind.resourceOffset = offset_in_buffer;
    bind.size = kScaledResolveHeapSize;
    bind.memory = heap;
    bind.memoryOffset = 0;
    bind.flags = 0;

    command_processor_.SparseBindBuffer(
        scaled_resolve_sparse_buffers_[buffer_a]->buffer(), 1, &bind,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }

  // Bind to the second possible buffer (if different and valid)
  if (buffer_b < buffer_count && buffer_b != buffer_a &&
      scaled_resolve_sparse_buffers_[buffer_b]) {
    uint64_t offset_in_buffer =
        heap_address - (uint64_t(buffer_b) << 30);  // buffer_b * 1GB

    VkSparseMemoryBind bind = {};
    bind.resourceOffset = offset_in_buffer;
    bind.size = kScaledResolveHeapSize;
    bind.memory = heap;
    bind.memoryOffset = 0;
    bind.flags = 0;

    command_processor_.SparseBindBuffer(
        scaled_resolve_sparse_buffers_[buffer_b]->buffer(), 1, &bind,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }
}

bool VulkanTextureCache::EnsureScaledResolveMemoryCommittedSparse(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (length_unscaled == 0) {
    return true;
  }

  if (start_unscaled > SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  uint32_t draw_resolution_scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits =
      (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  uint64_t end_scaled = start_scaled + length_scaled;

  // Calculate which heaps we need
  uint32_t heap_first = uint32_t(start_scaled >> kScaledResolveHeapSizeLog2);
  uint32_t heap_last = uint32_t((end_scaled - 1) >> kScaledResolveHeapSizeLog2);

  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // Allocate and bind any heaps we need
  for (uint32_t heap_index = heap_first; heap_index <= heap_last;
       ++heap_index) {
    if (heap_index >= scaled_resolve_heaps_.size()) {
      XELOGE("VulkanTextureCache: Heap index {} out of range (max {})",
             heap_index, scaled_resolve_heaps_.size());
      return false;
    }

    if (scaled_resolve_heaps_[heap_index] == VK_NULL_HANDLE) {
      // Allocate a new heap
      VkMemoryAllocateInfo allocate_info = {};
      allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocate_info.allocationSize = kScaledResolveHeapSize;
      allocate_info.memoryTypeIndex = scaled_resolve_memory_type_;

      VkDeviceMemory heap;
      VkResult result =
          dfn.vkAllocateMemory(device, &allocate_info, nullptr, &heap);
      if (result != VK_SUCCESS) {
        XELOGE("VulkanTextureCache: Failed to allocate heap {}: error {}",
               heap_index, static_cast<int>(result));
        return false;
      }

      scaled_resolve_heaps_[heap_index] = heap;
      ++scaled_resolve_heap_count_;

      // Bind this heap to all overlapping buffers
      BindHeapToOverlappingBuffers(heap_index, heap);
    }
  }

  return true;
}

bool VulkanTextureCache::MakeScaledResolveRangeCurrentSparse(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  // First ensure the memory is committed
  if (!EnsureScaledResolveMemoryCommittedSparse(start_unscaled, length_unscaled,
                                                length_scaled_alignment_log2)) {
    return false;
  }

  uint32_t draw_resolution_scale_area =
      draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits =
      (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  uint64_t end_scaled = start_scaled + length_scaled;

  // Find a buffer that contains the entire range
  // Each buffer is 2GB and covers [buffer_index GB .. (buffer_index + 2) GB)
  auto [buffer_a, buffer_b] =
      GetPossibleScaledResolveBufferIndices(start_scaled);
  auto [end_buffer_a, end_buffer_b] = GetPossibleScaledResolveBufferIndices(
      end_scaled > 0 ? end_scaled - 1 : 0);

  // Check if buffer_a can contain both start and end
  size_t chosen_buffer = SIZE_MAX;
  if (buffer_a == end_buffer_a || buffer_a == end_buffer_b) {
    chosen_buffer = buffer_a;
  } else if (buffer_b == end_buffer_a || buffer_b == end_buffer_b) {
    chosen_buffer = buffer_b;
  }

  if (chosen_buffer == SIZE_MAX ||
      chosen_buffer >= GetScaledResolveSparseBufferCount()) {
    XELOGE("VulkanTextureCache: No buffer can contain range [{:X}, {:X})",
           start_scaled, end_scaled);
    return false;
  }

  scaled_resolve_current_buffer_index_ = chosen_buffer;
  // Store the current range (matching simple buffer mode behavior)
  scaled_resolve_current_range_start_scaled_ = start_scaled;
  scaled_resolve_current_range_length_scaled_ = length_scaled;
  return true;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
