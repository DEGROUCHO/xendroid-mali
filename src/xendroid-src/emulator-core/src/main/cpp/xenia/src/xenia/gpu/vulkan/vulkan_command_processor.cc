/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <set>
#include <dlfcn.h>
#include <unordered_map>

#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/base/cp_wait_stats.h"
#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <pthread.h>
#include <system_error>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/frame_stats.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_fsi_system_constants.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/vulkan/vulkan_compatibility.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(vulkan_texture_from_render_target);
DECLARE_bool(log_texture_clobber);
DECLARE_bool(vulkan_in_pass_resolve_debug_read_usage);
DECLARE_bool(log_gpu_frame_time_breakdown);
DECLARE_bool(vulkan_classic_pass_finalize);
DECLARE_bool(vulkan_fold_clears_to_load_op);
DECLARE_bool(vulkan_store_lookahead);
DECLARE_bool(log_cp_frame_breakdown);
DECLARE_int32(frame_generation);
DECLARE_bool(log_vk_memory);
DEFINE_int32(log_pass_bytes, 0,
             "Capture this many complete guest frames of logical pass traffic. "
             "Zero disables capture; no GPU commands are changed.", "Vulkan");

DEFINE_bool(
    rt_cache_full_overwrite_await_pipeline, true,
    "When a draw's render target update skipped the previous contents because "
    "the draw overwrites them entirely, wait for the draw's real pipeline "
    "instead of skipping it or using a placeholder (which would leave stale "
    "pixels, e.g. a white screen from broken HDR exposure feedback).",
    "GPU");
DECLARE_string(debug_ab_feature);
DECLARE_bool(vulkan_async_skip_draws);
DECLARE_bool(spirv_texture_sign_specialization);
DECLARE_bool(spirv_texture_sign_specialization_used);
DECLARE_bool(spirv_texture_exp_adjust_specialization);
DEFINE_int32(log_guest_frame_intervals, 0,
             "Report native guest IssueSwap interval percentiles every N "
             "frames (1..600), including stalls. 0 disables. One sample per "
             "guest frame; independent of per-draw profiling and host repaints.",
             "GPU");

DEFINE_bool(vulkan_unclipped_fallback, true,
            "Without host clip / cull distances, draw user-clip-plane draws "
            "that software clipping can't handle unclipped instead of dropping "
            "them.",
            "GPU");
DEFINE_bool(vulkan_render_area_draw_bound, true,
            "With vulkan_render_area_shrink: bound each guest draw's rows in "
            "the render area by the render target cache's estimate for it "
            "(scissor, viewport or vertices) rather than by the scissor alone.",
            "GPU");
DEFINE_bool(vulkan_render_area_tight, true,
            "With vulkan_render_area_shrink: also bound the rows of draws with "
            "clipping disabled (screen-space clears with an 8192 scissor) by "
            "the render target cache's estimate - from their vertices, run on "
            "the CPU only when every index and vertex read is CPU-authoritative, "
            "otherwise the scissor - and shrink passes that only clear.",
            "GPU");
DEFINE_bool(
    vulkan_bindless_textures, false,
    "Guest textures and samplers through one update-after-bind descriptor set "
    "holding every live texture view and sampler (descriptor indexing), with "
    "the shaders reading their slots from the system constants. Removes the "
    "per-draw texture descriptor set allocation, writes and binds (and the "
    "driver work of re-binding them). Latched at GPU initialization; needs "
    "dynamically indexed sampled image arrays and update-after-bind. A win "
    "where the command processor is the limit (GTA IV +25-35%, a game quirk "
    "there), a loss where the GPU is (Reach -10%: the indexed texture fetches "
    "cost shader time) and no gain in Black Ops - off by default.",
    "Vulkan");
UPDATE_from_bool(vulkan_bindless_textures, 2026, 10, 26, 21, true);
DEFINE_bool(
    vulkan_alpha_tested_depth_prepass, false,
    "Host render target path: an opaque draw whose pixel shader can kill "
    "pixels (pixel kill, alpha test) and that writes depth (less-equal / "
    "greater-equal, no blending, no stencil writes) is drawn twice: first "
    "depth only (color writes off, so the compiler keeps only what the kill "
    "needs), then color with the depth test 'equal', depth writes off and "
    "early fragment tests. A shader that can discard and writes depth is "
    "tested after shading on Mali, so every hidden pixel of foliage, fences "
    "or hair runs the whole shader; the second draw shades only the visible "
    "ones. Same image: same invariant positions, same kill in both draws. "
    "Off: no gain measured (2026-10-26: GTA IV 357 draws/frame split, GPU "
    "27.8 against 27.0 ms; Black Ops 122, 16 ms either way).",
    "GPU");
DEFINE_string(live_cvar_file, "",
              "Test harness: flat TOML (key = value) of scalar cvars applied "
              "while the game runs, re-read when its mtime changes (checked "
              "every 30 frames). Values go to the game-config layer, never "
              "saved. Empty disables it.",
              "GPU");

DEFINE_bool(
    render_area_dirty_extent, false,
    "Shrink each render pass's area to the region its draws actually touch.\n"
    "Host render targets span the whole EDRAM range for their pitch, so a "
    "narrow one is thousands of rows tall. Measured on Adreno 650 / Turnip "
    "this changes nothing (the driver skips the empty tiles). See "
    "vulkan_render_area_shrink, which enables the same by default.",
    "GPU");
DEFINE_bool(
    vulkan_render_area_shrink, true,
    "Like render_area_dirty_extent (either enables it), on by default: a "
    "1040-wide host render target is 2528 rows tall while the guest draws "
    "608, and on Mali the render area bounds the tiles a pass loads and "
    "stores. Only ever shrinks to the union of the pass's draw scissors and "
    "clear rectangles, so it's always safe.",
    "GPU");

DEFINE_path(
    vulkan_guest_output_capture_request, "",
    "Path to an opt-in one-shot request file. When it appears after a guest "
    "frame is submitted, the Vulkan guest output is captured as a PNG beside "
    "the request file. Empty disables the diagnostic.",
    "Vulkan");

DEFINE_int32(
    vulkan_guest_output_capture_sequence, 1,
    "With vulkan_guest_output_capture_request: the number of consecutive "
    "guest output frames to capture, also written as raw RGBX files "
    "(guest-output-seq-<frame>.rgbx) for offline frame interpolation "
    "evaluation.",
    "Vulkan");

DEFINE_path(
    vulkan_rt_trace_request, "",
    "Path to an opt-in one-shot request file. When it exists at a guest "
    "frame boundary, the next complete frame's render target bindings, "
    "ownership transfers, resolve clears, resolves and draw runs are logged "
    "with the RTTRACE prefix, then the file is removed. Empty disables the "
    "diagnostic.",
    "Vulkan");

DEFINE_int32(
    vulkan_mid_frame_submission_draws, 0,
    "If greater than 0, end and submit the current command buffer after this "
    "many draws instead of only at the swap, so the GPU overlaps the frame's "
    "rendering with the building of the rest of its command stream. 0 keeps "
    "one submission per frame. Splitting closes the current render pass, so "
    "values that are too small hurt tiled GPUs; try roughly half the title's "
    "per-frame draw count.",
    "GPU");
UPDATE_from_int32(vulkan_mid_frame_submission_draws, 2026, 7, 24, 12, 0);
UPDATE_from_int32(vulkan_mid_frame_submission_draws, 2026, 10, 5, 12, 1300);

DEFINE_bool(
    vulkan_republish_draw_inputs, true,
    "Retain shared memory accesses of a draw or resolve being prepared "
    "across every render pass begin or reopen, so later pass-head work "
    "cannot overtake accesses recorded in the pass.",
    "Vulkan");

DEFINE_int32(vulkan_frames_in_flight, 3,
             "Guest frames the GPU may lag behind the command processor "
             "(1-3). Opening a frame awaits the completion of the frame this "
             "many frames before it.",
             "Vulkan");

DEFINE_bool(
    vulkan_memexport_compute_barrier, true,
    "Before each vertex shader memory export run as compute, make all earlier "
    "shared memory writes (other exports, uploads, resolves) visible to it. "
    "Exports can read data an earlier export wrote.",
    "Vulkan");

DEFINE_int32(log_memexport_dispatches, 0,
             "Diagnostic: log this many vertex shader memory exports run as "
             "compute (shader, vertices, read and written ranges), then stop. "
             "Can be set live.",
             "GPU");

DEFINE_bool(
    vulkan_memexport_batch, true,
    "Run consecutive memory-export-only draws of one vertex shader (same "
    "textures) as one compute dispatch, one workgroup row per draw, instead "
    "of one dispatch each. Halo Reach updates ~120 particle emitters per "
    "frame with one 16-vertex export each: alone, each runs as a single "
    "warp on one shader core (~70 us, ~8 ms of GPU time per frame).",
    "Vulkan");

DEFINE_bool(
    vulkan_memexport_ram_mirror, true,
    "When memory export output stays in the device-local shared memory buffer "
    "(no VK_EXT_external_memory_host, as on Mali), copy each exported range to "
    "guest RAM once its submission has completed, for the pages the CPU hasn't "
    "written since. Otherwise a later partial CPU write to such a page reloads "
    "the whole page from RAM that never received the exported bytes (Black Ops "
    "texture blocks).",
    "Vulkan");
DEFINE_bool(
    vulkan_memexport_ram_mirror_dedup, true,
    "Copy each page of the completed memory export ranges to guest RAM once "
    "per pass instead of once per queued range: every memexport draw queues "
    "its stream's whole range, so draws exporting to the same buffer (Halo "
    "Reach particles) copied the same pages again and again with the same "
    "bytes.",
    "Vulkan");
// Default since q644: Halo Reach copied 24-34 MB/frame for 0.1-0.3 MB of
// exported pages; with the dedup 30.0 fps and 57 instead of 309 frames over
// 36 ms in gameplay at the same heat (REACH-MX0b/MX1b).
UPDATE_from_bool(vulkan_memexport_ram_mirror_dedup, 2026, 11, 1, 12, false);
DEFINE_bool(
    log_memexport_ram_mirror, false,
    "Log the memexport RAM mirror's work per frame every 5 seconds: ranges "
    "completed, pages visited, pages copied and bytes copied.",
    "Vulkan");

DEFINE_bool(
    vulkan_memexport_vs_compute, true,
    "On devices without vertexPipelineStoresAndAtomics (Mali), run the memory "
    "export of vertex shaders as a compute shader instead of dropping it. "
    "Games write streamed texture data this way (Modern Warfare 2).",
    "Vulkan");
DEFINE_uint32(vulkan_command_pool_spares, 0,
              "Idle command pools kept for reuse per kind (setup, stream); "
              "completed ones beyond this are destroyed (0 keeps all). Tried "
              "at 2 on BO: GPU memory went up and 140 pools per second were "
              "recreated, so it is off.",
              "Vulkan");
DEFINE_uint32(log_native_heap_census_at_frame, 0,
              "Diagnostic: at this guest frame, walk every live native heap "
              "allocation once (malloc_iterate, all threads' allocations "
              "paused for the walk) and log sizes, the classes of C++ objects "
              "(vtable of libe.so or the Mali driver) and the largest blocks. "
              "0 disables.",
              "GPU");
DEFINE_bool(
    vulkan_depth_4x_as_1x, false,
    "Opt-in approximation for eligible host-render-target 4x depth-only "
    "(RB_MODECONTROL=kDepthOnly) draws: store the four samples as a 2x2 grid "
    "in a 1x depth image. Enable before a surface's first 4x use; surfaces "
    "already touched in native 4x mode stay native. Requires depth writes, no "
    "pixel shader, stencil, alpha-to-coverage, or resolution scaling, and a "
    "doubled pitch of at most 4096. A no-pixel-shader prepass may be rewritten "
    "before a later draw reveals that the surface is a multisampled scene; "
    "sample locations can shift by up to a quarter guest pixel. Disabled by "
    "default; validate each game's image before enabling.",
    "Vulkan");
DEFINE_bool(
    vulkan_msaa_4x_as_2x2, true,
    "Store guest 4x MSAA render targets (color and depth) as 1x images of "
    "double width and height, one host pixel per guest sample in the EDRAM "
    "order. A 4x surface and a 1x view of the same EDRAM (games drawing at "
    "half resolution with 4x MSAA and reading the samples back as pixels) "
    "become the same image, so ownership transfers between them disappear. "
    "Pixel shaders run per sample, with positions and texture derivatives in "
    "guest pixels. Requires no "
    "resolution scaling; supersedes vulkan_depth_4x_as_1x.",
    "Vulkan");
// Default since q575: Reach +30% fps (GPU 51 -> 38 ms), Dante 2 px grid gone
// (single-sample storage of samples read back as pixels), BO image identical.
UPDATE_from_bool(vulkan_msaa_4x_as_2x2, 2026, 10, 25, 17, false);
DEFINE_bool(
    vulkan_msaa_2x2_pixel_rate, true,
    "With vulkan_msaa_4x_as_2x2 and vulkan_fragment_shading_rate_dynamic: run "
    "the pixel shader of a 4x draw kept as 2x2 host pixels once per 2x2 block, "
    "that is once per guest pixel like the Xbox 360's MSAA, instead of once "
    "per sample. Depth, stencil and coverage stay per sample. Shaders writing "
    "depth keep the per-sample rate. Latched at GPU initialization.",
    "Vulkan");
// Default since q589: GTA IV GPU 32.0 -> 30.5 ms/frame, Reach and Black Ops
// unchanged (few shaded 4x draws), same images.
UPDATE_from_bool(vulkan_msaa_2x2_pixel_rate, 2026, 10, 26, 0, false);
DEFINE_bool(
    vulkan_msaa_2x2_alpha_to_coverage, true,
    "With vulkan_msaa_4x_as_2x2: keep 4x draws with alpha to coverage in the "
    "2x2 host pixel storage too, testing each host pixel (guest sample) "
    "against its alpha to coverage threshold in the pixel shader, instead of "
    "moving the surface to a true 4x image and back for them.",
    "Vulkan");
DEFINE_bool(
    vulkan_msaa_4x_clears_as_2x2, true,
    "With vulkan_msaa_4x_as_2x2: resolve clears of 4x surfaces clear the 1x "
    "double-size image too, instead of moving the range to a 4x image and "
    "back.",
    "Vulkan");
// Default since q592: with alpha to coverage kept in 2x2 (q591) the last GTA IV
// surface moving to 4x and back (C@224) was its resolve clears; gone, same
// image. Reach q577 neutral.
UPDATE_from_bool(vulkan_msaa_4x_clears_as_2x2, 2026, 10, 26, 1, false);
DEFINE_int32(
    vulkan_fragment_shading_rate, 0,
    "With vulkan_fragment_shading_rate_dynamic: run the pixel shaders of "
    "depth-tested guest draws once per 2x1 (1), 1x2 (2) or 2x2 (3) pixels "
    "instead of per pixel. Coverage, depth and stencil stay per sample, so "
    "geometry edges stay sharp; texture and lighting detail inside surfaces "
    "gets coarser. Draws without the depth test (HUD, text, post-processing) "
    "and shaders writing depth always keep the full rate. 0 = full rate. "
    "Read per draw.",
    "Vulkan");
DEFINE_bool(
    thermal_governor, true,
    "Android: read the system thermal headroom every 2 seconds and, before "
    "the system throttles the GPU clock, coarsen the pixel shading rate of "
    "depth-tested draws (needs vulkan_fragment_shading_rate_dynamic): 2x1 "
    "from thermal_governor_headroom_1, 2x2 from thermal_governor_headroom_2, "
    "back one step 0.05 below each threshold. On the test phone the headroom "
    "is about (skin temperature - 27) / 30 and the system halves the GPU "
    "clock near 0.55-0.60. A ThermalGovernor line is logged when the rate "
    "changes.",
    "Vulkan");
DEFINE_bool(
    adaptive_shading_rate, false,
    "Shade depth-tested draws at 2x1 (as vulkan_fragment_shading_rate=1) "
    "only while the GPU is the bottleneck: busy above "
    "adaptive_shading_rate_busy_up of the time over one second, back to the "
    "full rate below adaptive_shading_rate_busy_down. While the GPU is the "
    "bottleneck the CPU also waits like on a hot phone (no spinning for late "
    "frames, light threads on the slow cores): the frame rate is the GPU's, "
    "and the heat saved keeps the GPU clock up. Still GPU bound under "
    "adaptive_shading_rate_2x2_below_fps at 2x1: 2x2. Games limited by the "
    "CPU or by their own frame cap keep the full rate. Combined with the "
    "thermal governor and vulkan_fragment_shading_rate (the coarsest wins). "
    "Off by default: q619 flashed whole black frames and stuttered in GTA IV "
    "while walking (27/10), with vulkan_pass_head_overlap.",
    "Vulkan");
DEFINE_double(adaptive_shading_rate_busy_up, 0.92,
              "GPU busy fraction from which adaptive_shading_rate shades at "
              "2x1.",
              "Vulkan");
DEFINE_double(adaptive_shading_rate_busy_down, 0.72,
              "GPU busy fraction below which adaptive_shading_rate returns to "
              "the full rate (2x1 takes ~15% off the GPU time, so the gap "
              "keeps it from flipping back and forth).",
              "Vulkan");
DEFINE_int32(adaptive_shading_rate_2x2_below_fps, 29,
             "Guest frame rate under which adaptive_shading_rate goes from 2x1 "
             "to 2x2 while the GPU stays the bottleneck (0 = 2x1 at most).",
             "Vulkan");
DEFINE_double(thermal_governor_headroom_1, 0.50,
              "Thermal headroom (1.0 = severe throttling) from which the "
              "thermal governor shades at 2x1.",
              "Vulkan");
DEFINE_double(thermal_governor_headroom_2, 0.55,
              "Thermal headroom from which the thermal governor shades at 2x2.",
              "Vulkan");
DEFINE_bool(
    vulkan_fragment_shading_rate_alpha_tested, false,
    "Also use vulkan_fragment_shading_rate for draws whose coverage depends "
    "on the pixel shader (alpha test, alpha to coverage, pixel kill): their "
    "cutout edges then follow the coarser rate.",
    "Vulkan");
DEFINE_bool(vulkan_relax_unused_attachment_ops, true,
            "With dynamic rendering, when a render pass closes, mark the "
            "attachments none of its draws or clears wrote with storeOp NONE "
            "so a tile-based GPU does not write their tiles back (the depth "
            "buffer of a pass drawing with depth writes off, the color "
            "buffers of a depth-only pass).",
            "Vulkan");
DEFINE_bool(vulkan_relax_unused_attachment_loads, true,
            "With vulkan_relax_unused_attachment_ops and "
            "VK_EXT_load_store_op_none: attachments a pass neither wrote nor "
            "depth / stencil tested are not loaded into the tiles either.",
            "Vulkan");
DEFINE_bool(
    log_relaxed_attachment_ops, false,
    "Log the running count of attachment load / store operations "
    "vulkan_relax_unused_attachment_ops skipped, every 16384 passes; with "
    "classic pass experiments also log per-pass op counts.",
    "Vulkan");
UPDATE_from_bool(log_relaxed_attachment_ops, 2026, 10, 24, 12, true);
DEFINE_int32(log_render_pass_breaks, 0,
             "Diagnostic: every this many guest frames, log which code paths "
             "ended render passes (return addresses in libe.so to symbolize "
             "offline) and conservative resolve deferral candidates. 0 = off.",
             "GPU");
DEFINE_bool(vulkan_debug_disable_alpha_to_mask, false,
            "Diagnostic: ignore the guest's alpha-to-mask (alpha to coverage "
            "is emulated as a 2x2 dither pattern when the host rasterizes "
            "MSAA targets single-sampled). For A/B of dot-pattern artifacts.",
            "Vulkan");
DEFINE_bool(log_frame_pacing, false,
            "Log percentiles of the intervals between guest swaps every 5 "
            "seconds (FramePace lines): p50/p90/p99, max, frames over 25 and "
            "50 ms. For measuring stutter, not just average FPS.",
            "GPU");
DEFINE_bool(log_guest_fps, false,
            "Diagnostic: log the guest frame rate and draws per frame every "
            "second, without GPU timestamps (unlike "
            "log_gpu_frame_time_breakdown, which can serialize tiled GPUs).",
            "GPU");
DEFINE_bool(log_gpu_time, false,
            "Diagnostic: time each guest frame on the GPU with Vulkan "
            "timestamps (as the detailed overlay does) and add it to the "
            "GuestFps line. GPU time / frame time >= 0.9 means GPU-bound.",
            "GPU");
DEFINE_int32(log_vertex_shader_hotness, 0,
             "Diagnostic: every this many guest frames, log the vertex shaders "
             "(ucode hashes) with the most indices drawn per frame. 0 = off.",
             "GPU");
DEFINE_bool(vulkan_reuse_index_buffer_binding, true,
            "Keep the index buffer bound across draws indexing the same buffer "
            "at different offsets, selecting the indices with firstIndex "
            "instead of rebinding it for every draw.",
            "Vulkan");
DEFINE_uint32(log_pass_sequence_every, 0,
              "With log_gpu_frame_time_breakdown: every N submissions, log "
              "the render passes of 2 consecutive submissions in order (size, "
              "draws, touched scissor/viewport, GPU time, and the GPU gap "
              "before each pass, where resolves/texture loads run).",
              "GPU");
DEFINE_bool(log_draw_state_repeats, false,
            "Diagnostic: with log_cp_frame_breakdown, report how many draws "
            "repeat the previous draw's state (only float constants and "
            "VGT_INDX_OFFSET changed), for fast-path planning.",
            "GPU");
DEFINE_bool(log_draw_census, false,
            "Diagnostic: report exact state-candidate run lengths for "
            "multi-draw, same-geometry instancing, and relaxed texture "
            "bindings. Allows per-draw constants, vertex fetches and index "
            "operands. Not a memory-dependency proof or a timing mode.",
            "GPU");
DEFINE_bool(vulkan_resolve_defer, true,
            "Record independent host-target resolves at their original point "
            "and execute them after the open pass ends. UMA reads of previous "
            "submissions stay at the original point; first-use drains, "
            "zero-copy, clears, active queries and tracing remain immediate.",
            "Vulkan");
UPDATE_from_bool(vulkan_resolve_defer, 2026, 10, 23, 19, false);

DEFINE_int32(vulkan_bindless_debug_null, 0,
             "Diagnostic: point these bindless bindings at the null view or "
             "sampler (slot 0): 1 vertex shader textures, 2 pixel shader "
             "textures, 4 samplers (combinable).",
             "Vulkan");
DEFINE_bool(vulkan_bindless_write_idle, false,
            "Diagnostic: write the bindless descriptor set only with no "
            "submission pending on the GPU (waits for it), from the command "
            "processor thread.",
            "Vulkan");
DEFINE_bool(vulkan_defer_descriptor_updates, true,
            "Record guest draw descriptor set writes into the deferred command "
            "stream so vkUpdateDescriptorSets runs at replay time (on the "
            "stream encoder thread when vulkan_stream_encode is on) instead "
            "of on the command processor thread.",
            "Vulkan");
UPDATE_from_bool(vulkan_defer_descriptor_updates, 2026, 10, 5, 12, false);

DEFINE_bool(vulkan_split_submission_only_when_idle, false,
            "With submit_on_primary_buffer_end, end the submission at the end "
            "of a primary buffer only if the guest hasn't already queued more "
            "commands. When it has, the command processor is the bottleneck: "
            "the split only makes it wait for the stream encoder, and the "
            "GPU receives the work with the next submission anyway.",
            "Vulkan");

DEFINE_bool(vulkan_stream_encode, true,
            "Replay the recorded commands of every closed render pass into the "
            "Vulkan command buffer on a separate thread while the rest of the "
            "submission is still being processed, moving the driver's command "
            "encoding off the GPU command processor thread.",
            "Vulkan");
DEFINE_bool(vulkan_stream_encode_with_fold, false,
            "Keep vulkan_stream_encode with vulkan_fold_clears_to_load_op: "
            "each closed render pass is replayed on the encoder thread except "
            "a trailing clear-only pass, held until the next pass shows "
            "whether it absorbs its clears. Otherwise the whole submission is "
            "encoded on the command processor thread at its end. Off: on Mali "
            "(mt6899) the driver encodes 2.2x slower on the encoder thread and "
            "the concurrent thread slows the CP itself (q517, 24/10).",
            "Vulkan");
DEFINE_bool(
    vulkan_pass_head_overlap, false,
    "Pass heads (uploads and texture loads recorded before a render pass) and "
    "the submission's hoisted uploads wait only for the stages that can touch "
    "the shared memory and the scratch buffer - vertex input, guest vertex "
    "shaders, compute, transfer, and pixel shaders only while one that reads "
    "or writes the shared memory may still run - instead of every earlier "
    "command. The next pass's uploads and geometry then overlap the previous "
    "pass's fragment work: on Mali (q587 counters) binning, fragment and "
    "compute ran strictly one after another (their active cycles summed to "
    "the GPU active cycles). Off by default: q619 flashed whole black frames "
    "in GTA IV while walking (27/10), with adaptive_shading_rate.",
    "Vulkan");
DEFINE_bool(vulkan_pass_head_shmem_uploads, true,
            "Record shared memory uploads requested inside a render pass, for "
            "pages no command of that pass has used yet, into a slot executed "
            "right before the pass instead of ending the pass for them.",
            "Vulkan");
DEFINE_bool(shared_memory_unwatch_dynamic_pages, true,
            "Leave shared memory pages the CPU rewrites every frame without "
            "an access violation watch (no mprotect per upload, no guest page "
            "fault per rewrite), checking them against their upload hash once "
            "per ring buffer batch instead.",
            "GPU");
UPDATE_from_bool(shared_memory_unwatch_dynamic_pages, 2026, 10, 5, 12, false);
DEFINE_bool(shared_memory_draw_input_range_epochs, true,
            "Per-draw input caches (vertex and index ranges) check the epochs "
            "of their own 64 KiB regions instead of the global epoch, which "
            "every invalidation anywhere in guest memory advances: a cached "
            "vertex range is re-requested only when memory near it changed.",
            "GPU");
UPDATE_from_bool(shared_memory_draw_input_range_epochs, 2026, 10, 25, 3, false);
DEFINE_bool(vulkan_texture_descriptor_cache_per_submission, true,
            "Scope the texture descriptor set value cache to one submission: "
            "a texture used only by an earlier submission of the frame may be "
            "destroyed when that submission completes and its image view "
            "handle value reused, which a content hash cannot detect.",
            "Vulkan");
DEFINE_int32(debug_draw_mode, 0,
             "Diagnostic: 0 = normal, 1 = record all state but skip the draw "
             "commands, 2 = draw at most 3 vertices per draw (keeps the "
             "per-draw GPU overhead, drops the geometry work).",
             "GPU");
DEFINE_int32(log_memexport_draws, 0,
             "Diagnostic: log this many memory export draws with their export "
             "ranges and whether the host drops them.",
             "GPU");
DEFINE_uint32(log_pixel_shader_paths, 0,
              "Diagnostic: every this many frames, log the draws per frame by "
              "pixel shader depth / stencil mode (none = no pixel shader, "
              "early = early fragment tests, late = driver-chosen tests with "
              "coverage output, noa2c = late without coverage output, f24 / "
              "polyoff = depth written by the shader) and how many of them "
              "write depth.",
              "GPU");

DEFINE_bool(
    vulkan_allow_reverse_z, true,
    "Pass viewports with minDepth > maxDepth (reverse depth ranges, used by "
    "many games for inverse-Z) directly to the driver. Vulkan allows this, "
    "but some drivers mishandle it (the Direct3D 12 backend always "
    "normalizes, citing drivers where it works and drivers where it "
    "doesn't). Disable to normalize the range and compensate in the NDC "
    "transform like the D3D12 backend does.",
    "Vulkan");

DEFINE_bool(
    vulkan_dynamic_constant_buffers, true,
    "Bind the guest constant buffers as VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC "
    "so only the per-draw dynamic offsets vary; the constants descriptor set is "
    "re-allocated and re-written only when an upload-pool page rolls over. "
    "Default on since 2026-10-25 (Mali ABAB: same image), but never used on the "
    "Qualcomm proprietary driver: on Qualcomm proprietary Vulkan drivers (Adreno 650/740/830 "
    "tested, 2026-06) shaders read ZEROS through the dynamic-offset uniform "
    "descriptors, breaking all rendering (black output / garbage), proven by "
    "RenderDoc pixel-history bisection; Mesa/Turnip reads them correctly. "
    "Safe to enable on Turnip for a small command-processor win. Has no "
    "effect on devices that report maxDescriptorSetUniformBuffersDynamic < 7 "
    "(the guest constant-buffer count).",
    "Vulkan");
UPDATE_from_bool(vulkan_dynamic_constant_buffers, 2026, 10, 25, 3, false);

DECLARE_bool(gpu_debug_markers);
DECLARE_bool(submit_on_primary_buffer_end);
DECLARE_bool(vulkan_placeholder_pipelines);

DEFINE_bool(
    vulkan_dynamic_rendering, true,
    "Use VK_KHR_dynamic_rendering instead of traditional render passes. "
    "May improve or worsen performance depending on driver. Requires Vulkan "
    "1.3 or VK_KHR_dynamic_rendering extension support.",
    "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_extreme_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        SpirvShaderTranslator::kConstantBufferCount*
            kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageImage = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         2 * kLinkedTypeDescriptorPoolSetCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(
    VulkanGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      completion_timeline_(static_cast<const ui::vulkan::VulkanProvider*>(
                               graphics_system->provider())
                               ->vulkan_device(),
                           "cp"),
      deferred_command_buffer_storage_(*this),
      deferred_setup_command_buffer_(*this, 64 * 1024),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_image_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageImage, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures,
          uint32_t(xe::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::UpdateDebugMarkersEnabled() {
  // Enable debug markers if the CVAR is set or RenderDoc is detected.
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void VulkanCommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_->CmdVkBeginDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_buffer_->CmdVkEndDebugUtilsLabelEXT();
}

void VulkanCommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_->CmdVkInsertDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::BeforeCpuMemoryWrite(uint32_t start,
                                                   uint32_t length) {
  if (shared_memory_ && shared_memory_->has_depth_surface_records()) {
    shared_memory_->PrepareForCpuAccess(start, length);
    shared_memory_->MemoryInvalidationCallback(start, length, true);
  }
}

void VulkanCommandProcessor::PrepareForPause() {
  if (shared_memory_)
    shared_memory_->MaterializeDepthSurfaces(0, SharedMemory::kBufferSize);
}

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  shared_memory_->InvalidateAllPages();
}

void VulkanCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                      uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
}

void VulkanCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking,
                                           std::move(completion_callback));
}

bool VulkanCommandProcessor::RuntimeSnapshotSupported() const {
  return render_target_cache_ && render_target_cache_->draw_resolution_scale_x() == 1 &&
         render_target_cache_->draw_resolution_scale_y() == 1;
}

bool VulkanCommandProcessor::RuntimeSnapshotHealthy() const {
  return render_target_cache_ && render_target_cache_->RuntimeSnapshotHealthy();
}

bool VulkanCommandProcessor::CaptureRuntimeSnapshot(snapshot::GpuState& out) {
  if (draw_census_enabled_) DrawCensusClose(kCensusBarrier);
  if (!CommandProcessor::CaptureRuntimeSnapshot(out)) return false;
  // Resolve output that only lives in the shared memory buffer must reach
  // guest RAM, or a restore re-uploads stale bytes (garbage textures).
  if (shared_memory_) {
    if (!AwaitAllQueueOperationsCompletion()) return false;
    // Everything has completed: the saved guest RAM must hold every fence.
    uint32_t pages = 0;
    if (!shared_memory_->WriteBackGpuWrittenToGuest(pages)) return false;
    XELOGI("Snapshot: {} GPU-written pages copied to guest RAM", pages);
  }
  out.scale_x = render_target_cache_->draw_resolution_scale_x();
  out.scale_y = render_target_cache_->draw_resolution_scale_y();
  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    out.backend = 2;
    return render_target_cache_->CaptureRuntimeHostTargets(out.edram);
  }
  out.backend = 1;
  return render_target_cache_->CaptureRuntimeEdram(out.edram);
}

bool VulkanCommandProcessor::RestoreRuntimeSnapshot(const snapshot::GpuState& state) {
  if (draw_census_enabled_) DrawCensusClose(kCensusBarrier);
  uint32_t backend = render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets ? 2 : 1;
  if (state.backend != backend || state.scale_x != render_target_cache_->draw_resolution_scale_x() ||
      state.scale_y != render_target_cache_->draw_resolution_scale_y()) {
    XELOGE("GPU restore: render target path {} scale {}x{} differs from the snapshot ({} {}x{})",
           backend, render_target_cache_->draw_resolution_scale_x(),
           render_target_cache_->draw_resolution_scale_y(), state.backend, state.scale_x,
           state.scale_y);
    return false;
  }
  if (!CommandProcessor::RestoreRuntimeSnapshot(state)) return false;
  InvalidateGpuMemory();
  const bool targets = backend == 2 ? render_target_cache_->RestoreRuntimeHostTargets(state.edram)
                                    : render_target_cache_->RestoreRuntimeEdram(state.edram);
  if (!targets) XELOGE("GPU restore: EDRAM / render target contents could not be restored");
  return targets;
}

void VulkanCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {}

bool VulkanCommandProcessor::HasMemoryProviders() const {
  return shared_memory_ && shared_memory_->has_depth_surface_records();
}

void VulkanCommandProcessor::ServiceMemoryRequests() {
  if (shared_memory_) shared_memory_->ServiceDepthRequests();
}

void VulkanCommandProcessor::PollCompletedSubmission() {
  ServiceMemoryRequests();
  // Strict ZPD can skip unnecessary work here that can wait for the next full
  // CheckSubmissionCompletionAndDeviceLoss and it's not needed for retirement.
  if (device_lost_) {
    return;
  }
  if (!AwaitSubmissionAndUpdateCompleted(GetCompletedSubmission())) {
    return;
  }
  PumpQueryResolves();
}

std::string VulkanCommandProcessor::GetTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  switch (render_target_cache_->GetPath()) {
    case RenderTargetCache::Path::kHostRenderTargets:
      suffix << " - FBO";
      break;
    case RenderTargetCache::Path::kPixelShaderInterlock:
      suffix << " - FSI";
      break;
    default:
      break;
  }
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }
  if (cvars::log_gpu_time) {
    SetGpuTimingEnabled(true);
  }

  // Check if debug markers should be enabled (CVAR or RenderDoc detection).
  UpdateDebugMarkersEnabled();
  if (debug_markers_enabled_) {
    XELOGI("GPU debug markers enabled for RenderDoc/debug tools");
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      xe::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                         size_t(16384)),
                size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_empty_) != VK_SUCCESS) {
    XELOGE("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  // Dynamic constant buffers capability gate, resolved once before the constants layout is
  // created and held constant for the device lifetime. The Vulkan-guaranteed
  // minimum for maxDescriptorSetUniformBuffersDynamic is 8, so this passes on
  // all conformant hardware; if it ever fails, the binding stays plain
  // UNIFORM_BUFFER and the original per-draw write+bind path runs unchanged.
  // Never on the Qualcomm proprietary driver (reads zeros, see the cvar).
  use_dynamic_constants_ =
      cvars::vulkan_dynamic_constant_buffers &&
      device_properties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
      device_properties.maxDescriptorSetUniformBuffersDynamic >=
          SpirvShaderTranslator::kConstantBufferCount;
  memexport_batch_supported_ =
      device_properties.maxUniformBufferRange >=
      SpirvShaderTranslator::kMemExportBatchMaxBytes;
  const VkDescriptorType constants_descriptor_type =
      use_dynamic_constants_ ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding =
        descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = constants_descriptor_type;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferSystem]
          .stageFlags =
      // Visible to the vertex/domain stages for user clip planes and to the
      // control/eval/vertex stages for tessellation, plus the usual guest
      // stages.
      guest_shader_stages | guest_shader_vertex_stages_ |
      (device_properties.tessellationShader
           ? (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
           : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatVertex]
          .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatPixel]
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferBoolLoop]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFetch]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(xe::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_constants_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: storage buffer for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings =
      &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferFragment)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the fragment shader");
    return false;
  }
  // Transient: storage image for the resolve-to-texture fragment variant.
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageImageFragment)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage image "
        "bound to the fragment shader");
    return false;
  }
  descriptor_set_layout_binding_transient.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageImageCompute)]) != VK_SUCCESS) {
    XELOGE("Failed to create compute storage-image descriptor layout");
    return false;
  }
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.binding = 1;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kUniformBufferComputeB1)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a uniform buffer "
        "bound to binding 1 in the compute shader");
    return false;
  }

  shared_memory_ = std::make_unique<VulkanSharedMemory>(
      *this, *memory_, trace_writer_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Read-watch consumption tracking for resolves.
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  primitive_processor_ = std::make_unique<VulkanPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive processor");
    return false;
  }

  const uint32_t shared_memory_binding_count =
      shared_memory_->GetShaderBufferCount();
  uint32_t shared_memory_binding_count_log2 = 0;
  while ((UINT32_C(1) << shared_memory_binding_count_log2) <
         shared_memory_binding_count) {
    ++shared_memory_binding_count_log2;
  }

  // Requires the transient descriptor set layouts.
  // Get draw resolution scale and clamp based on device capabilities
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);
  // Without sparse binding (Mali), the texture cache commits the scaled
  // resolve address space as disjoint lazily allocated spans instead, so
  // resolution scaling doesn't depend on it.
  if (!TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, true, 0)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    XELOGW(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  // Shared memory, EDRAM, and ZPD FSI counter descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  VkDescriptorSetLayoutBinding
      shared_memory_and_edram_descriptor_set_layout_bindings[3] = {};
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags =
      guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers =
      nullptr;
  VkDescriptorSetLayoutCreateInfo
      shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount =
        1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1]
        .pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
    // ZPD FSI counter.
    shared_memory_and_edram_descriptor_set_layout_bindings[2].binding = 2;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].descriptorCount =
        1;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[2]
        .pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 3;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info,
          nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  // Before the pipeline cache, whose translators follow the latched mode, and
  // the texture cache, which bounds its sampler count by the slots.
  if (!InitializeBindless()) {
    XELOGE("Failed to initialize the bindless texture descriptor set");
    return false;
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_,
      guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }
  // Same condition as SpirvShaderTranslator::Features::samples_as_pixels_coarse
  // (the shading rate is dynamic only with these capabilities).
  msaa_2x2_pixel_rate_ =
      cvars::vulkan_msaa_2x2_pixel_rate &&
      pipeline_cache_->dynamic_state_capabilities().fragment_shading_rate;

  // Requires the transient descriptor set layouts.
  // Use the same draw resolution scale as render target cache
  texture_cache_ = VulkanTextureCache::Create(
      *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }
  texture_cache_->SetTraceWriter(&trace_writer_);
  if (bindless_set_ != VK_NULL_HANDLE && !FillBindlessWithNull()) {
    XELOGE("Failed to fill the bindless texture descriptor set");
    return false;
  }

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = draw_resolution_scale_x;
  zpd_draw_resolution_scale_y_ = draw_resolution_scale_y;

  const VkDeviceSize zpd_fsi_counter_sink_range =
      sizeof(uint32_t) * kZPDQueryPoolCapacity;
  if (edram_fragment_shader_interlock) {
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, zpd_fsi_counter_sink_range,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            zpd_fsi_counter_sink_buffer_,
            zpd_fsi_counter_sink_buffer_memory_)) {
      XELOGE("Failed to create the ZPD FSI counter sink buffer");
      return false;
    }
  }

  // Shared memory, EDRAM, and ZPD FSI counter common bindings. A second set
  // (binding 0 -> host-imported buffer) is allocated for memexport routing when
  // the host buffer is available.
  const bool create_host_shared_memory_set =
      shared_memory_->host_buffer() != VK_NULL_HANDLE;
  const uint32_t shared_memory_set_count =
      create_host_shared_memory_set ? 2u : 1u;
  VkDescriptorPoolSize descriptor_pool_sizes[1] = {};
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_set_count *
      (shared_memory_binding_count +
       2u * uint32_t(edram_fragment_shader_interlock));
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = shared_memory_set_count;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool =
      shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(
          device, &descriptor_set_allocate_info,
          &shared_memory_and_edram_descriptor_set_) != VK_SUCCESS) {
    XELOGE(
        "Failed to allocate the Vulkan descriptor set for shared memory "
        "and EDRAM");
    return false;
  }
  if (create_host_shared_memory_set) {
    if (dfn.vkAllocateDescriptorSets(
            device, &descriptor_set_allocate_info,
            &shared_memory_host_and_edram_descriptor_set_) != VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the host-imported Vulkan descriptor set for "
          "shared memory memexport routing");
      return false;
    }
  }
  // Writes binding 0 (shared memory, split across shared_memory_binding_count
  // sub-ranges) and, under FSI, binding 1 (EDRAM) and binding 2 (ZPD FSI
  // counter placeholder) of one shared-memory/EDRAM set. shared_memory_buffer
  // selects the device-local buffer or the host-imported buffer.
  auto write_shared_memory_and_edram_set = [&](VkDescriptorSet set,
                                               VkBuffer shared_memory_buffer) {
    VkDescriptorBufferInfo shared_memory_descriptor_buffers_info[
        VulkanSharedMemory::kMaximumBufferSlices];
    uint32_t shared_memory_binding_range =
        SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
    for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
      VkDescriptorBufferInfo& info = shared_memory_descriptor_buffers_info[i];
      info.buffer = shared_memory_buffer;
      info.offset = shared_memory_binding_range * i;
      info.range = shared_memory_binding_range;
    }
    // The in-pass vertex fetch ring past the guest memory is read through the
    // single binding.
    if (shared_memory_binding_count == 1 &&
        shared_memory_buffer == shared_memory_->buffer()) {
      shared_memory_descriptor_buffers_info[0].range +=
          shared_memory_->vfetch_ring_size();
    }
    VkWriteDescriptorSet write_descriptor_sets[3];
    VkWriteDescriptorSet& write_shared_memory = write_descriptor_sets[0];
    write_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_shared_memory.pNext = nullptr;
    write_shared_memory.dstSet = set;
    write_shared_memory.dstBinding = 0;
    write_shared_memory.dstArrayElement = 0;
    write_shared_memory.descriptorCount = shared_memory_binding_count;
    write_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_shared_memory.pImageInfo = nullptr;
    write_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
    write_shared_memory.pTexelBufferView = nullptr;
    VkDescriptorBufferInfo edram_descriptor_buffer_info;
    VkDescriptorBufferInfo zpd_fsi_counter_descriptor_buffer_info;
    if (edram_fragment_shader_interlock) {
      edram_descriptor_buffer_info.buffer =
          render_target_cache_->edram_buffer();
      edram_descriptor_buffer_info.offset = 0;
      edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
      VkWriteDescriptorSet& write_edram = write_descriptor_sets[1];
      write_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_edram.pNext = nullptr;
      write_edram.dstSet = set;
      write_edram.dstBinding = 1;
      write_edram.dstArrayElement = 0;
      write_edram.descriptorCount = 1;
      write_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_edram.pImageInfo = nullptr;
      write_edram.pBufferInfo = &edram_descriptor_buffer_info;
      write_edram.pTexelBufferView = nullptr;
      // ZPD FSI counter - kept valid until the real counter buffer is ready.
      zpd_fsi_counter_descriptor_buffer_info.buffer =
          zpd_fsi_counter_sink_buffer_;
      zpd_fsi_counter_descriptor_buffer_info.offset = 0;
      zpd_fsi_counter_descriptor_buffer_info.range = zpd_fsi_counter_sink_range;
      VkWriteDescriptorSet& write_zpd_fsi_counter = write_descriptor_sets[2];
      write_zpd_fsi_counter.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_zpd_fsi_counter.pNext = nullptr;
      write_zpd_fsi_counter.dstSet = set;
      write_zpd_fsi_counter.dstBinding = 2;
      write_zpd_fsi_counter.dstArrayElement = 0;
      write_zpd_fsi_counter.descriptorCount = 1;
      write_zpd_fsi_counter.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_zpd_fsi_counter.pImageInfo = nullptr;
      write_zpd_fsi_counter.pBufferInfo =
          &zpd_fsi_counter_descriptor_buffer_info;
      write_zpd_fsi_counter.pTexelBufferView = nullptr;
    }
    dfn.vkUpdateDescriptorSets(
        device, 1 + 2 * uint32_t(edram_fragment_shader_interlock),
        write_descriptor_sets, 0, nullptr);
  };
  write_shared_memory_and_edram_set(shared_memory_and_edram_descriptor_set_,
                                   shared_memory_->buffer());
  if (create_host_shared_memory_set) {
    write_shared_memory_and_edram_set(
        shared_memory_host_and_edram_descriptor_set_,
        shared_memory_->host_buffer());
  }
  if (edram_fragment_shader_interlock) {
    zpd_fsi_counter_descriptor_buffer_ = zpd_fsi_counter_sink_buffer_;
    zpd_fsi_counter_descriptor_range_ = zpd_fsi_counter_sink_range;
  }

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize =
      kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size =
      kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage =
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode =
      VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info,
                         nullptr, &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(
        device, gamma_ramp_buffer_,
        &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local &
         vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex)) ||
        xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex))) {
      VkMemoryAllocateInfo*
          gamma_ramp_host_visible_buffer_memory_allocate_info_last =
              &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo
          gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext =
            nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image =
            VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer =
            gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(
              device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
              nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_,
                                   gamma_ramp_buffer_memory_,
                                   0) == VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0,
                              VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      XELOGE("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      XELOGE("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0,
                        VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      XELOGE("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format =
      VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset =
        kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2 + 1]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings =
      &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_uniform_texel_buffer_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }
  // FXAA source descriptor set layout (combined image sampler for linear
  // filtering).
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &fxaa_source_descriptor_set_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA source descriptor set layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes =
      swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type =
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount =
        gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  // Destination storage images for compute shader output.
  // Also includes storage image for writing to FXAA source (gamma+luma output).
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount =
        kMaxFramesInFlight * 2;  // dest + FXAA source storage
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  // FXAA source combined image samplers.
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info,
                                 nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    XELOGE("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_dest_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation destination image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &fxaa_source_descriptor_set_layout_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source image descriptor sets");
      return false;
    }
  }
  // Allocate storage image descriptor sets for writing to FXAA source.
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_storage_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source storage descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView =
        &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0,
                               nullptr);
  }

  // Gamma ramp application compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetDest] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_push_constant_range;
  swap_apply_gamma_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_push_constant_range.offset = 0;
  swap_apply_gamma_push_constant_range.size = sizeof(ApplyGammaConstants);
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
          &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipelines.
  VkShaderModule swap_apply_gamma_table_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_cs,
          sizeof(shaders::apply_gamma_table_cs));
  VkShaderModule swap_apply_gamma_pwl_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::apply_gamma_pwl_cs,
                                           sizeof(shaders::apply_gamma_pwl_cs));
  VkShaderModule swap_apply_gamma_table_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  VkShaderModule swap_apply_gamma_pwl_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_table_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_table_fxaa_luma_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_fxaa_luma_shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to create the gamma ramp application compute shader modules");
    if (swap_apply_gamma_table_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_pwl_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_table_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
    }
    if (swap_apply_gamma_pwl_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.stage.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.stage.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.stage =
      VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_pipeline_create_info.stage.pName = "main";
  swap_apply_gamma_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_create_info.layout =
      swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;

  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_create_result =
      dfn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                   &swap_apply_gamma_pipeline_create_info,
                                   nullptr, &swap_apply_gamma_pwl_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
  if (swap_apply_gamma_pipeline_256_entry_table_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result !=
          VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application compute pipelines");
    return false;
  }

  // FXAA sampler (linear filtering, clamp to edge).
  VkSamplerCreateInfo fxaa_sampler_create_info;
  fxaa_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  fxaa_sampler_create_info.pNext = nullptr;
  fxaa_sampler_create_info.flags = 0;
  fxaa_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  fxaa_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.mipLodBias = 0.0f;
  fxaa_sampler_create_info.anisotropyEnable = VK_FALSE;
  fxaa_sampler_create_info.maxAnisotropy = 1.0f;
  fxaa_sampler_create_info.compareEnable = VK_FALSE;
  fxaa_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  fxaa_sampler_create_info.minLod = 0.0f;
  fxaa_sampler_create_info.maxLod = 0.0f;
  fxaa_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  fxaa_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &fxaa_sampler_create_info, nullptr,
                          &fxaa_sampler_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA sampler");
    return false;
  }

  // FXAA pipeline layout.
  // set=0: destination storage image (rgb10_a2)
  // set=1: source combined image sampler (FXAA source with luma in alpha)
  std::array<VkDescriptorSetLayout, 2> fxaa_descriptor_set_layouts = {
      swap_descriptor_set_layout_storage_image_,
      fxaa_source_descriptor_set_layout_,
  };
  VkPushConstantRange fxaa_push_constant_range;
  fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_push_constant_range.offset = 0;
  fxaa_push_constant_range.size = sizeof(FxaaConstants);
  VkPipelineLayoutCreateInfo fxaa_pipeline_layout_create_info;
  fxaa_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  fxaa_pipeline_layout_create_info.pNext = nullptr;
  fxaa_pipeline_layout_create_info.flags = 0;
  fxaa_pipeline_layout_create_info.setLayoutCount =
      uint32_t(fxaa_descriptor_set_layouts.size());
  fxaa_pipeline_layout_create_info.pSetLayouts =
      fxaa_descriptor_set_layouts.data();
  fxaa_pipeline_layout_create_info.pushConstantRangeCount = 1;
  fxaa_pipeline_layout_create_info.pPushConstantRanges =
      &fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &fxaa_pipeline_layout_create_info,
                                 nullptr,
                                 &fxaa_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA pipeline layout");
    return false;
  }

  // FXAA compute pipelines.
  VkShaderModule fxaa_shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs));
  VkShaderModule fxaa_extreme_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::fxaa_extreme_cs,
                                           sizeof(shaders::fxaa_extreme_cs));
  if (fxaa_shader_module == VK_NULL_HANDLE ||
      fxaa_extreme_shader_module == VK_NULL_HANDLE) {
    XELOGE("Failed to create the FXAA compute shader modules");
    if (fxaa_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
    }
    if (fxaa_extreme_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo fxaa_pipeline_create_info;
  fxaa_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  fxaa_pipeline_create_info.pNext = nullptr;
  fxaa_pipeline_create_info.flags = 0;
  fxaa_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fxaa_pipeline_create_info.stage.pNext = nullptr;
  fxaa_pipeline_create_info.stage.flags = 0;
  fxaa_pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_pipeline_create_info.stage.pName = "main";
  fxaa_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  fxaa_pipeline_create_info.layout = fxaa_pipeline_layout_;
  fxaa_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  fxaa_pipeline_create_info.basePipelineIndex = -1;

  fxaa_pipeline_create_info.stage.module = fxaa_shader_module;
  VkResult fxaa_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_pipeline_);
  fxaa_pipeline_create_info.stage.module = fxaa_extreme_shader_module;
  VkResult fxaa_extreme_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_extreme_pipeline_);
  dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
  dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
  if (fxaa_pipeline_create_result != VK_SUCCESS ||
      fxaa_extreme_pipeline_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA compute pipelines");
    return false;
  }

  // Resolve downscale compute shader for scaled resolution readback.
  // Descriptor set layout: binding 0 = source storage buffer,
  //                        binding 1 = destination storage buffer.
  {
    std::array<VkDescriptorSetLayoutBinding, 2> resolve_downscale_bindings;
    // Source buffer (readonly).
    resolve_downscale_bindings[0].binding = 0;
    resolve_downscale_bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[0].descriptorCount = 1;
    resolve_downscale_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[0].pImmutableSamplers = nullptr;
    // Destination buffer (writeonly).
    resolve_downscale_bindings[1].binding = 1;
    resolve_downscale_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[1].descriptorCount = 1;
    resolve_downscale_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo resolve_downscale_set_layout_create_info;
    resolve_downscale_set_layout_create_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    resolve_downscale_set_layout_create_info.pNext = nullptr;
    resolve_downscale_set_layout_create_info.flags = 0;
    resolve_downscale_set_layout_create_info.bindingCount =
        uint32_t(resolve_downscale_bindings.size());
    resolve_downscale_set_layout_create_info.pBindings =
        resolve_downscale_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(
            device, &resolve_downscale_set_layout_create_info, nullptr,
            &resolve_downscale_descriptor_set_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale descriptor set layout");
      return false;
    }

    // Pipeline layout with push constants.
    VkPushConstantRange resolve_downscale_push_constant_range;
    resolve_downscale_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_push_constant_range.offset = 0;
    resolve_downscale_push_constant_range.size =
        sizeof(ResolveDownscaleConstants);

    VkPipelineLayoutCreateInfo resolve_downscale_pipeline_layout_create_info;
    resolve_downscale_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_downscale_pipeline_layout_create_info.pNext = nullptr;
    resolve_downscale_pipeline_layout_create_info.flags = 0;
    resolve_downscale_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_downscale_pipeline_layout_create_info.pSetLayouts =
        &resolve_downscale_descriptor_set_layout_;
    resolve_downscale_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_downscale_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_downscale_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_downscale_pipeline_layout_create_info, nullptr,
            &resolve_downscale_pipeline_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale pipeline layout");
      return false;
    }

    // Compute pipeline.
    VkShaderModule resolve_downscale_shader_module =
        ui::vulkan::util::CreateShaderModule(
            vulkan_device, shaders::resolve_downscale_cs,
            sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_shader_module == VK_NULL_HANDLE) {
      XELOGE("Failed to create the resolve downscale shader module");
      return false;
    }

    VkComputePipelineCreateInfo resolve_downscale_pipeline_create_info;
    resolve_downscale_pipeline_create_info.sType =
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.pNext = nullptr;
    resolve_downscale_pipeline_create_info.flags = 0;
    resolve_downscale_pipeline_create_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.stage.pNext = nullptr;
    resolve_downscale_pipeline_create_info.stage.flags = 0;
    resolve_downscale_pipeline_create_info.stage.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_pipeline_create_info.stage.module =
        resolve_downscale_shader_module;
    resolve_downscale_pipeline_create_info.stage.pName = "main";
    resolve_downscale_pipeline_create_info.stage.pSpecializationInfo = nullptr;
    resolve_downscale_pipeline_create_info.layout =
        resolve_downscale_pipeline_layout_;
    resolve_downscale_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
    resolve_downscale_pipeline_create_info.basePipelineIndex = -1;

    VkResult resolve_downscale_pipeline_result = dfn.vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &resolve_downscale_pipeline_create_info,
        nullptr, &resolve_downscale_pipeline_);
    dfn.vkDestroyShaderModule(device, resolve_downscale_shader_module, nullptr);
    if (resolve_downscale_pipeline_result != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale compute pipeline");
      return false;
    }

    // Descriptor pool chain for resolve downscale shader.
    // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
    // Each pool has 64 sets with 2 storage buffers each.
    VkDescriptorPoolSize resolve_downscale_pool_size;
    resolve_downscale_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_pool_size.descriptorCount = 128;  // 64 sets * 2 buffers
    resolve_downscale_descriptor_pool_chain_ =
        std::make_unique<ui::vulkan::VulkanDescriptorPoolChain>(
            vulkan_device, 0, 64, &resolve_downscale_pool_size, 1,
            resolve_downscale_descriptor_set_layout_);
  }

  // Initialize the ZPD occlusion query pool and resources.
  zpd_host_query_pool_ = std::make_unique<VulkanZPDQueryPool>();
  EnsureZPDQueryResources();

  // Per-submission GPU timestamps for the frame time breakdown diagnostics.
  uint32_t timestamp_family_count = 0;
  const auto& instance_functions = vulkan_device->vulkan_instance()->functions();
  instance_functions.vkGetPhysicalDeviceQueueFamilyProperties(
      vulkan_device->physical_device(), &timestamp_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> timestamp_families(timestamp_family_count);
  instance_functions.vkGetPhysicalDeviceQueueFamilyProperties(
      vulkan_device->physical_device(), &timestamp_family_count, timestamp_families.data());
  const uint32_t timestamp_family = vulkan_device->queue_family_graphics_compute();
  if (timestamp_family < timestamp_family_count) {
    frame_timestamp_valid_bits_ = timestamp_families[timestamp_family].timestampValidBits;
  }
  if (frame_timestamp_valid_bits_ &&
      vulkan_device->properties().timestampPeriod > 0.0f) {
    VkQueryPoolCreateInfo timestamp_pool_create_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    timestamp_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    timestamp_pool_create_info.queryCount = kFrameTimestampSlots * 2;
    if (dfn.vkCreateQueryPool(device, &timestamp_pool_create_info, nullptr,
                              &frame_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, kFrameTimestampSlots * 4 * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              frame_timestamp_buffer_, frame_timestamp_buffer_memory_, nullptr,
              &frame_timestamp_buffer_size_) ||
          dfn.vkMapMemory(device, frame_timestamp_buffer_memory_, 0,
                          VK_WHOLE_SIZE, 0,
                          reinterpret_cast<void**>(
                              &frame_timestamp_mapping_)) != VK_SUCCESS) {
        XELOGW(
            "VulkanCommandProcessor: No readback buffer for frame timestamps; "
            "GPU execution time reporting disabled");
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               frame_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               frame_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               frame_timestamp_pool_);
        frame_timestamp_mapping_ = nullptr;
      }
    }
  }
  // Per-resolve GPU timestamps, ring-buffered per submission; only meaningful
  // alongside the per-submission pair above.
  if (cvars::log_gpu_frame_time_breakdown && frame_timestamp_mapping_) {
    VkQueryPoolCreateInfo resolve_ts_pool_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    resolve_ts_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    resolve_ts_pool_info.queryCount = kResolveTimestampPairsPerSubmission *
                                      kResolveTimestampRingSubmissions * 2;
    if (dfn.vkCreateQueryPool(device, &resolve_ts_pool_info, nullptr,
                              &resolve_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, resolve_ts_pool_info.queryCount * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              resolve_timestamp_buffer_, resolve_timestamp_buffer_memory_,
              nullptr, &resolve_timestamp_buffer_size_) ||
          dfn.vkMapMemory(device, resolve_timestamp_buffer_memory_, 0,
                          VK_WHOLE_SIZE, 0,
                          reinterpret_cast<void**>(
                              &resolve_timestamp_mapping_)) != VK_SUCCESS) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               resolve_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(
            dfn.vkFreeMemory, device, resolve_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               resolve_timestamp_pool_);
        resolve_timestamp_mapping_ = nullptr;
      }
    }
  }
  // Per-render-pass timestamps, bucketed by framebuffer extent.
  if (cvars::log_gpu_frame_time_breakdown && frame_timestamp_mapping_) {
    VkQueryPoolCreateInfo pass_ts_pool_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    pass_ts_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    pass_ts_pool_info.queryCount =
        kPassTimestampPairsPerSubmission * kPassTimestampRingSubmissions * 2;
    if (dfn.vkCreateQueryPool(device, &pass_ts_pool_info, nullptr,
                              &pass_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, pass_ts_pool_info.queryCount * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              pass_timestamp_buffer_, pass_timestamp_buffer_memory_, nullptr,
              &pass_timestamp_buffer_size_) ||
          dfn.vkMapMemory(
              device, pass_timestamp_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
              reinterpret_cast<void**>(&pass_timestamp_mapping_)) !=
              VK_SUCCESS) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               pass_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               pass_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               pass_timestamp_pool_);
        pass_timestamp_mapping_ = nullptr;
      }
    }
  }

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));
  // ZPD FSI counter uses UINT32_MAX as its skip sentinel outside query draws.
  system_constants_.zpd_fsi_counter_index = UINT32_MAX;
  zpd_fsi_counter_index_force_update_ = true;

  stream_encode_enabled_ = cvars::vulkan_stream_encode;
  if (stream_encode_enabled_) {
    stream_encode_stop_ = false;
    stream_encode_thread_ =
        std::thread(&VulkanCommandProcessor::StreamEncodeThreadMain, this);
    deferred_command_buffer_->SetBeforeReallocate(
        [this]() { StreamEncodeAwaitIdle(); });
    XELOGI("VulkanCommandProcessor: streamed command encoding enabled");
  }

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  // Provider closures still need every cache, pool and pipeline below.
  if (shared_memory_) shared_memory_->MaterializeDepthSurfaces(0, SharedMemory::kBufferSize);
  AwaitAllQueueOperationsCompletion();
  StreamEncodeShutdown();

  ResetResolveReadWatch();

  ShutdownZPDQueryResources();
  zpd_host_query_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (frame_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, frame_timestamp_buffer_memory_);
    frame_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         frame_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         frame_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         frame_timestamp_pool_);
  vk_submit_times_.clear();
  frame_timestamp_prev_end_ = 0;
  if (resolve_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, resolve_timestamp_buffer_memory_);
    resolve_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         resolve_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         resolve_timestamp_pool_);
  resolve_ts_count_ = 0;
  if (pass_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, pass_timestamp_buffer_memory_);
    pass_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         pass_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         pass_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         pass_timestamp_pool_);
  pass_ts_count_ = 0;
  pass_ts_open_pair_ = UINT32_MAX;
  pass_bucket_stats_.clear();

  DestroyScratchBuffer();

  for (const auto& memexport_compute_pipeline : memexport_compute_pipelines_) {
    if (memexport_compute_pipeline.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, memexport_compute_pipeline.second, nullptr);
    }
  }
  memexport_compute_pipelines_.clear();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);

  // FXAA cleanup.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device,
                                         fxaa_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                         fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         fxaa_source_memory_);
  fxaa_source_width_ = 0;
  fxaa_source_height_ = 0;
  fxaa_source_last_submission_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         fxaa_source_descriptor_set_layout_);

  // Resolve downscale cleanup.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      resolve_downscale_descriptor_set_layout_);
  resolve_downscale_descriptor_pool_chain_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                           gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorPool, device,
      shared_memory_and_edram_descriptor_pool_);
  shared_memory_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  // Both sets are freed with the pool - drop the routing handle so it stays
  // null (routing disabled) until the pool is recreated.
  shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  ResetMemexportPages();
  zpd_fsi_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  zpd_fsi_counter_descriptor_range_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         zpd_fsi_counter_sink_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         zpd_fsi_counter_sink_buffer_memory_);

  texture_cache_.reset();
  // After the texture cache, which releases its views and samplers.
  ShutdownBindless();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(
        device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair :
       descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second,
                                     nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(
        dfn.vkDestroyDescriptorSetLayout, device,
        descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device, descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_->Reset();
  deferred_setup_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();
  for (const auto& command_buffer_pair : stream_command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  stream_command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer :
       stream_command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  stream_command_buffers_writable_.clear();
  stream_encode_active_ = false;

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::CountRegisterChanges(
    uint32_t index, const uint32_t* src_guest_endian, uint32_t count,
    bool fetch) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t reg = index + i;
    // Not draw state: the index offset (what a repeat fast path would push),
    // CP scratch / coherency registers and event initiators.
    if (reg == XE_GPU_REG_VGT_INDX_OFFSET ||
        reg == XE_GPU_REG_VGT_EVENT_INITIATOR ||
        (reg >= XE_GPU_REG_SCRATCH_REG0 && reg < XE_GPU_REG_SCRATCH_REG0 + 8) ||
        reg == XE_GPU_REG_COHER_STATUS_HOST ||
        reg == XE_GPU_REG_COHER_BASE_HOST || reg == XE_GPU_REG_COHER_SIZE_HOST) {
      continue;
    }
    if (register_file_->values[reg] !=
        xe::load_and_swap<uint32_t>(src_guest_endian + i)) {
      ++draw_state_changes_;
      if (fetch) {
        ++draw_fetch_changes_;
      }
    }
  }
}

void VulkanCommandProcessor::CountDrawRepeat(xenos::PrimitiveType prim_type,
                                             uint32_t index_count,
                                             bool indexed) {
  auto& s = frame_time_stats_;
  ++s.rep_draws;
  const bool nostate = draw_state_changes_ == last_draw_state_changes_;
  const bool fetchonly =
      (draw_state_changes_ - draw_fetch_changes_) ==
      (last_draw_state_changes_ - last_draw_fetch_changes_);
  const bool sameshaders = active_vertex_shader() == last_draw_vs_ &&
                           active_pixel_shader() == last_draw_ps_;
  s.rep_nostate += nostate;
  s.rep_fetchonly += fetchonly;
  s.rep_sameshaders += sameshaders;
  s.rep_sameshape += nostate && sameshaders && prim_type == last_draw_prim_type_ &&
                     index_count == last_draw_index_count_ &&
                     indexed == last_draw_indexed_;
  last_draw_state_changes_ = draw_state_changes_;
  last_draw_fetch_changes_ = draw_fetch_changes_;
  last_draw_vs_ = active_vertex_shader();
  last_draw_ps_ = active_pixel_shader();
  last_draw_prim_type_ = prim_type;
  last_draw_index_count_ = index_count;
  last_draw_indexed_ = indexed;
}

// Non-context registers a resolve epilogue writes that don't observe its
// result on the host: the coherency request (only logged by MakeCoherent; any
// draw reading the destination flushes the deferred resolve first), the
// unimplemented WAIT_UNTIL, and shader GPR allocation.
static bool IsResolveDeferNeutralRegister(uint32_t index) {
  switch (index) {
    case XE_GPU_REG_COHER_SIZE_HOST:
    case XE_GPU_REG_COHER_BASE_HOST:
    case XE_GPU_REG_COHER_STATUS_HOST:
    case XE_GPU_REG_WAIT_UNTIL:
    case XE_GPU_REG_SQ_GPR_MANAGEMENT:
      return true;
    default:
      return false;
  }
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index < 0x2000 && !IsResolveDeferNeutralRegister(index)) {
    FlushDeferredResolve(ResolveDeferStop::kEvent);
    if (resolve_defer_census_.active) {
      resolve_defer_census_.reasons |= kResolveDeferEvent;
    }
  }
  if (index >= RegisterFile::kRegisterCount) {
    CommandProcessor::WriteRegister(index, value);
    return;
  }
  const bool changed = register_file_->values[index] != value;
  if (draw_state_tracking_ && changed &&
      !(index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
        index <= XE_GPU_REG_SHADER_CONSTANT_511_W)) {
    const uint32_t guest_value = xe::byte_swap(value);
    CountRegisterChanges(index, &guest_value, 1,
        index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
        index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5);
  }
  // Side-effect registers must execute even for identical values.
  CommandProcessor::WriteRegister(index, value);
}

void VulkanCommandProcessor::FetchWritten(uint32_t slot) {
  current_constant_buffers_up_to_date_ &=
      ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
  const uint32_t keep = ~(UINT32_C(1) << slot);
  current_samplers_fetch_up_to_date_vertex_ &= keep;
  current_samplers_fetch_up_to_date_pixel_ &= keep;
  if (texture_cache_) texture_cache_->TextureFetchConstantWritten(slot);
}

void VulkanCommandProcessor::WriteShaderConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                               num_registers);
  register_file_->MarkWritten(start_index, num_registers);
}

void VulkanCommandProcessor::WriteBoolLoopFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (draw_state_tracking_)
    CountRegisterChanges(start_index, base, num_registers, false);
  for (uint32_t i = 0; i < num_registers; ++i)
    register_file_->Store(start_index + i, xe::load_and_swap<uint32_t>(base + i));
}

void VulkanCommandProcessor::WriteFetchFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (draw_state_tracking_)
    CountRegisterChanges(start_index, base, num_registers, true);
  for (uint32_t i = 0; i < num_registers; ++i)
    register_file_->Store(start_index + i, xe::load_and_swap<uint32_t>(base + i));

}

void VulkanCommandProcessor::WritePossiblySpecialRegistersFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  // Context and tail registers have no MMIO side effects. Keep range tracking
  // inline here rather than invoking the base per-register dispatcher.
  if (start_index >= 0x2000) {
    if (draw_state_tracking_)
      CountRegisterChanges(start_index, base, num_registers, false);
    for (uint32_t i = 0; i < num_registers; ++i)
      register_file_->Store(start_index + i, xe::load_and_swap<uint32_t>(base + i));
    return;
  }
  for (uint32_t i = 0; i < num_registers; ++i)
    WriteRegister(start_index + i, xe::load_and_swap<uint32_t>(base + i));
}

void VulkanCommandProcessor::WriteRegistersFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (start_index >= RegisterFile::kRegisterCount) return;
  uint32_t end = start_index + std::min(
      num_registers, uint32_t(RegisterFile::kRegisterCount) - start_index);
  // Split only at side-effect and constant family boundaries. No callbacks.
  while (start_index < end) {
    uint32_t limit;
    if (start_index < XE_GPU_REG_SHADER_CONSTANT_000_X) {
      limit = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_000_X));
      WritePossiblySpecialRegistersFromMem(start_index, base, limit-start_index);
    } else if (start_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
      limit = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_511_W + 1));
      WriteShaderConstantsFromMem(start_index, base, limit-start_index);
    } else if (start_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
      limit = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1));
      WriteFetchFromMem(start_index, base, limit-start_index);
    } else if (start_index < XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) {
      limit = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031));
      WritePossiblySpecialRegistersFromMem(start_index, base, limit-start_index);
    } else if (start_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
      limit = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1));
      WriteBoolLoopFromMem(start_index, base, limit-start_index);
    } else {
      limit = end;
      WritePossiblySpecialRegistersFromMem(start_index, base, limit-start_index);
    }
    base += limit - start_index;
    start_index = limit;
  }
}

void VulkanCommandProcessor::WriteRegisterRangeFromRing(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  if (XE_LIKELY(!range.second)) {
    WriteRegistersFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);
    ring->EndRead(range);
  } else {
    WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}

XE_NOINLINE
void VulkanCommandProcessor::WriteRegisterRangeFromRing_WraparoundCase(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  // Callers (ExecutePacketType0/3) guarantee the ring holds the whole range;
  // BeginRead clamps to capacity, which would make the second-half copy read
  // out of bounds if this were ever violated.
  assert_true(size_t(range.first_length) + size_t(range.second_length) ==
              num_registers * sizeof(uint32_t));
  uint32_t num_first = uint32_t(range.first_length / sizeof(uint32_t));
  WriteRegistersFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      num_first);
  WriteRegistersFromMem(
      base + num_first,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - num_first);
  ring->EndRead(range);
}
void VulkanCommandProcessor::SparseBindBuffer(
    VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
    VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds,
                              binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

namespace {
// Applies cvars from live_cvar_file for in-game A/B tests. Command processor
// thread only. Scalars only: a string cvar may be read concurrently elsewhere.
void PollLiveCvars() {
  static uint32_t last_poll_frame = 0;
  static int64_t last_mtime_ns = -1;
  uint32_t frame = logging::GetFrameNumber();
  if (frame - last_poll_frame < 30 && last_mtime_ns != -1) {
    return;
  }
  last_poll_frame = frame;
  struct stat info = {};
  if (stat(cvars::live_cvar_file.c_str(), &info) != 0 ||
      !S_ISREG(info.st_mode)) {
    return;
  }
  int64_t mtime_ns =
      int64_t(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
  if (mtime_ns == last_mtime_ns) {
    return;
  }
  last_mtime_ns = mtime_ns;
  try {
    toml::table table = toml::parse_file(cvars::live_cvar_file);
    std::string applied, rejected;
    for (const auto& [key, node] : table) {
      std::string name(key.str());
      cvar::IConfigVar* config_var = nullptr;
      if (cvar::ConfigVars) {
        auto it = cvar::ConfigVars->find(name);
        if (it != cvar::ConfigVars->end()) {
          config_var = it->second;
        }
      }
      std::string value;
      if (node.is_boolean()) {
        value = *node.value<bool>() ? "true" : "false";
      } else if (node.is_integer()) {
        value = std::to_string(*node.value<int64_t>());
      } else if (node.is_floating_point()) {
        value = std::to_string(*node.value<double>());
      }
      if (!config_var || value.empty()) {
        rejected += " " + name;
        continue;
      }
      config_var->LoadGameConfigValue(&node);
      applied += " " + name + "=" + value;
    }
    XELOGI("LiveCvar: frame={} applied:{}{}{}", frame, applied,
           rejected.empty() ? "" : " | rejected:", rejected);
  } catch (const std::exception& e) {
    XELOGW("LiveCvar: failed to parse {}: {}", cvars::live_cvar_file,
           e.what());
  }
}
}  // namespace

namespace {
// log_native_heap_census_at_frame. Filled by the malloc_iterate callback while
// the allocator is disabled, so it must not allocate.
struct NativeHeapCensusState {
  struct Range {
    uintptr_t begin;
    uintptr_t end;
  };
  Range libraries[2];  // libe.so, libGLES_mali.so
  static constexpr uint32_t kBuckets = 48;
  uint64_t bucket_count[kBuckets];
  uint64_t bucket_bytes[kBuckets];
  static constexpr uint32_t kTypeSlots = 16384;
  uintptr_t type_key[kTypeSlots];
  uint64_t type_count[kTypeSlots];
  uint64_t type_bytes[kTypeSlots];
  uint64_t type_overflow;
  static constexpr uint32_t kLarge = 64;
  uintptr_t large_base[kLarge];
  size_t large_size[kLarge];
  uint64_t large_head[kLarge][2];
  uint64_t total_count;
  uint64_t total_bytes;
  uint64_t spirv_count, spirv_bytes, zstd_count, zstd_bytes;
};

void NativeHeapCensusCallback(uintptr_t base, size_t size, void* arg) {
  NativeHeapCensusState& c = *static_cast<NativeHeapCensusState*>(arg);
  ++c.total_count;
  c.total_bytes += size;
  uint32_t bucket = size ? uint32_t(63 - __builtin_clzll(size)) : 0;
  bucket = std::min(bucket, NativeHeapCensusState::kBuckets - 1);
  ++c.bucket_count[bucket];
  c.bucket_bytes[bucket] += size;
  if (size < 8) {
    return;
  }
  const uintptr_t first = *reinterpret_cast<const uintptr_t*>(base);
  const uint32_t first32 = uint32_t(first);
  if (first32 == 0x07230203u) {
    ++c.spirv_count;
    c.spirv_bytes += size;
  } else if (first32 == 0xFD2FB528u) {
    ++c.zstd_count;
    c.zstd_bytes += size;
  }
  const uintptr_t untagged = first & UINT64_C(0x00FFFFFFFFFFFFFF);
  for (const NativeHeapCensusState::Range& library : c.libraries) {
    if (untagged < library.begin || untagged >= library.end) {
      continue;
    }
    uint32_t hash = uint32_t((untagged >> 3) * UINT64_C(0x9E3779B97F4A7C15) >>
                             32) &
                    (NativeHeapCensusState::kTypeSlots - 1);
    bool counted = false;
    for (uint32_t probe = 0; probe < 128; ++probe) {
      uint32_t slot = (hash + probe) & (NativeHeapCensusState::kTypeSlots - 1);
      if (c.type_key[slot] == untagged) {
        ++c.type_count[slot];
        c.type_bytes[slot] += size;
        counted = true;
        break;
      }
      if (!c.type_key[slot]) {
        c.type_key[slot] = untagged;
        c.type_count[slot] = 1;
        c.type_bytes[slot] = size;
        counted = true;
        break;
      }
    }
    if (!counted) {
      ++c.type_overflow;
    }
    break;
  }
  if (size >= 65536) {
    uint32_t smallest = 0;
    for (uint32_t i = 1; i < NativeHeapCensusState::kLarge; ++i) {
      if (c.large_size[i] < c.large_size[smallest]) {
        smallest = i;
      }
    }
    if (size > c.large_size[smallest]) {
      c.large_base[smallest] = base;
      c.large_size[smallest] = size;
      c.large_head[smallest][0] = reinterpret_cast<const uint64_t*>(base)[0];
      c.large_head[smallest][1] =
          size >= 16 ? reinterpret_cast<const uint64_t*>(base)[1] : 0;
    }
  }
}
}  // namespace

void VulkanCommandProcessor::NativeHeapCensus() {
  using IterateFn = int (*)(uintptr_t, size_t,
                            void (*)(uintptr_t, size_t, void*), void*);
  using ToggleFn = void (*)();
  auto iterate =
      reinterpret_cast<IterateFn>(dlsym(RTLD_DEFAULT, "malloc_iterate"));
  auto disable =
      reinterpret_cast<ToggleFn>(dlsym(RTLD_DEFAULT, "malloc_disable"));
  auto enable =
      reinterpret_cast<ToggleFn>(dlsym(RTLD_DEFAULT, "malloc_enable"));
  if (!iterate || !disable || !enable) {
    XELOGW("HeapCensus: malloc_iterate unavailable");
    return;
  }
  auto state = std::make_unique<NativeHeapCensusState>();
  std::memset(state.get(), 0, sizeof(NativeHeapCensusState));
  static constexpr uint32_t kMaxHeapRanges = 4096;
  auto heap_ranges =
      std::make_unique<NativeHeapCensusState::Range[]>(kMaxHeapRanges);
  uint32_t heap_range_count = 0;
  const char* const library_names[2] = {"/libe.so", "/libGLES_mali.so"};
  for (auto& library : state->libraries) {
    library.begin = UINTPTR_MAX;
    library.end = 0;
  }
  FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) {
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long begin, end;
    if (std::sscanf(line, "%llx-%llx", &begin, &end) != 2) {
      continue;
    }
    if ((std::strstr(line, "[anon:scudo:") ||
         std::strstr(line, "[anon:libc_malloc]")) &&
        heap_range_count < kMaxHeapRanges) {
      heap_ranges[heap_range_count++] = {uintptr_t(begin), uintptr_t(end)};
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (std::strstr(line, library_names[i])) {
        state->libraries[i].begin =
            std::min(state->libraries[i].begin, uintptr_t(begin));
        state->libraries[i].end =
            std::max(state->libraries[i].end, uintptr_t(end));
      }
    }
  }
  std::fclose(maps);
  auto start_time = std::chrono::steady_clock::now();
  disable();
  for (uint32_t i = 0; i < heap_range_count; ++i) {
    iterate(heap_ranges[i].begin, heap_ranges[i].end - heap_ranges[i].begin,
            NativeHeapCensusCallback, state.get());
  }
  enable();
  const NativeHeapCensusState& c = *state;
  XELOGI("HeapCensus: {} allocations, {:.1f} MB, {} heap mappings, {} us",
         c.total_count, double(c.total_bytes) / 1048576.0, heap_range_count,
         std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start_time)
             .count());
  for (uint32_t i = 0; i < NativeHeapCensusState::kBuckets; ++i) {
    if (c.bucket_bytes[i] >= (UINT64_C(1) << 20)) {
      XELOGI("HeapCensus: size 2^{}: {} allocations, {:.1f} MB", i,
             c.bucket_count[i], double(c.bucket_bytes[i]) / 1048576.0);
    }
  }
  XELOGI("HeapCensus: SPIR-V {} / {:.1f} MB, zstd {} / {:.1f} MB, "
         "type table overflow {}",
         c.spirv_count, double(c.spirv_bytes) / 1048576.0, c.zstd_count,
         double(c.zstd_bytes) / 1048576.0, c.type_overflow);
  // Largest types by bytes (selection, the table is small enough).
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < NativeHeapCensusState::kTypeSlots; ++i) {
    if (c.type_key[i]) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(), [&c](uint32_t a, uint32_t b) {
    return c.type_bytes[a] > c.type_bytes[b];
  });
  Dl_info self_info = {};
  dladdr(reinterpret_cast<void*>(&NativeHeapCensusCallback), &self_info);
  for (size_t i = 0; i < std::min(order.size(), size_t(40)); ++i) {
    uint32_t slot = order[i];
    uintptr_t key = c.type_key[slot];
    bool in_libe = key >= c.libraries[0].begin && key < c.libraries[0].end;
    XELOGI("HeapCensus: type {}+0x{:X}: {} objects, {:.2f} MB",
           in_libe ? "libe" : "mali",
           in_libe ? key - uintptr_t(self_info.dli_fbase)
                   : key - c.libraries[1].begin,
           c.type_count[slot], double(c.type_bytes[slot]) / 1048576.0);
  }
  for (uint32_t i = 0; i < NativeHeapCensusState::kLarge; ++i) {
    if (c.large_size[i]) {
      XELOGI("HeapCensus: large {:.2f} MB head {:016X} {:016X}",
             double(c.large_size[i]) / 1048576.0, c.large_head[i][0],
             c.large_head[i][1]);
    }
  }
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                       uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  FlushDeferredResolve(ResolveDeferStop::kEvent);
  SCOPE_profile_cpu_f("gpu");
  // Finish before reporting: no candidate crosses a guest frame boundary.
  if (resolve_defer_census_.active) {
    resolve_defer_census_.reasons |= kResolveDeferEvent;
    ResolveDeferFinish();
  }
  // Also refreshed here: the trace player executes packets without primary
  // buffer batches, where the census is normally switched on.
  RefreshDrawCensusConfig();
  if (cvars::log_draw_census) {
    DrawCensusClose(kCensusBarrier);
    DrawCensusReport();
  } else if (draw_census_.have_registers || draw_census_.report_ns) {
    draw_census_ = {};
  }
  cp_wait_stats::UpdateNice();
  cp_wait_stats::ApplyThreadPlacement();
  cp_wait_stats::Frame();

  // Count one presented guest frame for the debug overlay's FPS / frame-time
  // stats (the game's real frame rate, independent of the host present path).
  xe::RecordGuestPresent();
  if (cvars::log_frame_pacing) {
    guest_swap_intervals_.Record("guest-swap");
  }
  if (gpu_timing_enabled.load(std::memory_order_relaxed)) {
    gpu_frame_timer_.OnFrame(GpuTimingNowNs());
  }
  if (cvars::log_render_pass_breaks > 0) {
    static uint32_t pass_diag_frames = 0;
    if (++pass_diag_frames >= uint32_t(cvars::log_render_pass_breaks)) {
      pass_diag_frames = 0;
      VulkanSharedMemory::PassDiagStats& d = shared_memory_->pass_diag_stats();
      XELOGI("[VFWIN] accepted {} second-window {} copy-failed {} | would "
             "break {} hoistable {} pass-head {} gpu-written {} overflow {}",
             vfetch_window_accepted_, vfetch_window_second_,
             vfetch_window_copy_failed_, d.window_break, d.window_hoistable,
             d.window_pass_head, d.window_gpu_written, d.window_overflow);
      std::string use_text;
      static const char* const kUsageNames[] = {"R", "GRW", "CW", "TD"};
      for (uint32_t from = 0; from < 4; ++from) {
        for (uint32_t to = 0; to < 4; ++to) {
          for (uint32_t written = 0; written < 2; ++written) {
            if (d.use_in_pass[from][to][written]) {
              use_text += fmt::format(" {}->{}{}={}", kUsageNames[from],
                                      kUsageNames[to], written ? "+w" : "",
                                      d.use_in_pass[from][to][written]);
            }
          }
        }
      }
      XELOGI("[USEBAR] in-pass Use barriers:{}", use_text);
    }
  }
  if (cvars::log_native_heap_census_at_frame) {
    static uint32_t census_frames = 0;
    if (++census_frames == cvars::log_native_heap_census_at_frame) {
      NativeHeapCensus();
    }
  }
  if (++msaa_4x_as_1x_stats_frames_ >= 120) {
    // 2x2 is the default: its counters only with the diagnostic fps log.
    if (cvars::vulkan_depth_4x_as_1x ||
        (cvars::vulkan_msaa_4x_as_2x2 && cvars::log_guest_fps)) {
      XELOGI(
          "{}: 120 frames seen={} eligible={} rewritten={} "
          "rejected[incompatible={},history={},cvar={},depth={},scaled={},"
          "pitch={}]",
          cvars::vulkan_msaa_4x_as_2x2 ? "Msaa4xAs2x2" : "Depth4xAs1x",
          msaa_4x_as_1x_stats_seen_, msaa_4x_as_1x_stats_eligible_,
          msaa_4x_as_1x_stats_rewritten_,
          msaa_4x_as_1x_stats_reject_incompatible_,
          msaa_4x_as_1x_stats_reject_history_,
          msaa_4x_as_1x_stats_reject_cvar_,
          msaa_4x_as_1x_stats_reject_depth_write_,
          msaa_4x_as_1x_stats_reject_scaled_,
          msaa_4x_as_1x_stats_reject_pitch_);
    }
    msaa_4x_as_1x_stats_frames_ = 0;
    msaa_4x_as_1x_stats_seen_ = 0;
    msaa_4x_as_1x_stats_eligible_ = 0;
    msaa_4x_as_1x_stats_rewritten_ = 0;
    msaa_4x_as_1x_stats_reject_incompatible_ = 0;
    msaa_4x_as_1x_stats_reject_history_ = 0;
    msaa_4x_as_1x_stats_reject_cvar_ = 0;
    msaa_4x_as_1x_stats_reject_depth_write_ = 0;
    msaa_4x_as_1x_stats_reject_scaled_ = 0;
    msaa_4x_as_1x_stats_reject_pitch_ = 0;
  }
  if (cvars::log_guest_frame_intervals > 0) {
    const uint64_t now = FrameStatsNow();
    if (guest_interval_last_ns_) {
      guest_interval_ms_.push_back(float(double(now - guest_interval_last_ns_) / 1e6));
    }
    guest_interval_last_ns_ = now;
    const size_t count = guest_interval_ms_.size();
    if (count >= size_t(std::clamp(cvars::log_guest_frame_intervals, 1, 600))) {
      std::sort(guest_interval_ms_.begin(), guest_interval_ms_.end());
      const auto percentile = [&](size_t p) { return guest_interval_ms_[(count - 1) * p / 100]; };
      const size_t late = size_t(std::count_if(
          guest_interval_ms_.begin(), guest_interval_ms_.end(),
          [](float ms) { return ms > 16.7f; }));
      XELOGI("GuestFrameIntervals: n={} p50={:.2f} p90={:.2f} p99={:.2f} max={:.2f} ms over16.7={}/{}",
             count, percentile(50), percentile(90), percentile(99),
             guest_interval_ms_.back(), late, count);
      guest_interval_ms_.clear();
    }
  } else {
    guest_interval_last_ns_ = 0;
    guest_interval_ms_.clear();
  }
  shared_memory_->LogDeclaredWorkingSetFrame();
  if (cvars::log_vk_memory) {
    static uint32_t vk_memory_log_frames = 0;
    if (++vk_memory_log_frames >= 600) {
      vk_memory_log_frames = 0;
      ui::vulkan::VulkanDevice::LogMemoryAccounting();
      texture_cache_->LogMemoryAccounting();
      XELOGI("ShaderModules: graphics created={} ({:.1f} MB of SPIR-V), "
             "compressed SPIR-V kept {:.1f} MB, guest pipeline slots={}",
             VulkanShader::CreatedShaderModuleCount(),
             double(VulkanShader::CreatedShaderModuleSpirvBytes()) / 1048576.0,
             double(VulkanShader::CompressedSpirvBytes()) / 1048576.0,
             pipeline_cache_->pipeline_slot_count());
    }
  }
  ++draw_state_epoch_;
  // Swap breakdown (log_cp_frame_breakdown): where the CP thread spends the
  // swap - prologue, presenter guest output wait, refresh recording, submits.
  static uint64_t sb_frames = 0, sb_pre = 0, sb_wait = 0, sb_rec = 0,
                  sb_sub_in = 0, sb_sub_end = 0;
  uint64_t sb_t0 = cvars::log_cp_frame_breakdown ? FrameStatsNow() : 0;
  uint64_t sb_l0 = 0, sb_l1 = 0, sb_l2 = 0;
  if (shared_memory_) {
    shared_memory_->unwatch_enabled_ =
        cvars::shared_memory_unwatch_dynamic_pages !=
        AbFlipped(AbFeature::kUnwatch);
    shared_memory_->draw_input_range_epochs_ =
        cvars::shared_memory_draw_input_range_epochs;
    shared_memory_->OnFrameEndUnwatch();
  }
  if (cvars::log_gpu_frame_time_breakdown && ++stat_frames_ >= 60) {
    XELOGI("DrawStats: {:.0f} host vertices/frame over {:.0f} draws/frame "
           "({:.0f} per draw)",
           double(stat_host_vertices_) / 60.0, double(stat_host_draws_) / 60.0,
           stat_host_draws_ ? double(stat_host_vertices_) /
                                  double(stat_host_draws_)
                            : 0.0);
    stat_host_vertices_ = 0;
    stat_host_draws_ = 0;
    stat_frames_ = 0;
  }
  if (draw_state_tracking_ && ++fast_path_report_frames_ >= 60) {
    const double fr = double(fast_path_report_frames_);
    XELOGI("CpFastPath: {:.1f}% of {:.0f} draws/frame took the fast path; "
           "misses/frame: invalid={:.0f} epoch={:.0f} regs={:.0f} "
           "shaders={:.0f} prim={:.0f} submission={:.0f} shape={:.0f}",
           fast_path_draws_seen_
               ? 100.0 * double(fast_path_draws_) / double(fast_path_draws_seen_)
               : 0.0,
           double(fast_path_draws_seen_) / fr, fast_path_miss_[2] / fr,
           fast_path_miss_[3] / fr, fast_path_miss_[4] / fr,
           fast_path_miss_[5] / fr, fast_path_miss_[6] / fr,
           fast_path_miss_[7] / fr, fast_path_miss_[0] / fr);
    XELOGI("VkDroppedDraws: {:.1f} guest draws/frame dropped at replay "
           "(pipeline still being created)",
           double(deferred_command_buffer_->TakeDroppedDraws()) / fr);
    fast_path_draws_ = 0;
    fast_path_draws_seen_ = 0;
    fast_path_report_frames_ = 0;
    std::memset(fast_path_miss_, 0, sizeof(fast_path_miss_));
  }
  if (cvars::log_pixel_shader_paths &&
      ++pixel_shader_path_frames_ >= cvars::log_pixel_shader_paths) {
    const double fr = double(pixel_shader_path_frames_);
    // Draws per frame by mode, "/" draws among them with depth writes.
    auto p = [&](uint32_t i) {
      return fmt::format("{:.0f}/{:.0f}",
                         (pixel_shader_path_draws_[i][0] +
                          pixel_shader_path_draws_[i][1]) / fr,
                         pixel_shader_path_draws_[i][1] / fr);
    };
    XELOGI("PsPath: draws/frame (with depth write): none={} early={} late={} "
           "noa2c={} f24={} polyoff={}",
           p(8), p(1), p(0), p(7), p(2) + "+" + p(3), p(4) + "+" + p(5) + "+" + p(6));
    std::memset(pixel_shader_path_draws_, 0, sizeof(pixel_shader_path_draws_));
    pixel_shader_path_frames_ = 0;
  }
  if (cvars::log_cp_frame_breakdown && ++mem_stats_frames_ >= 60) {
    const double fr = double(mem_stats_frames_);
    SharedMemory& sm = *shared_memory_;
    PrimitiveProcessor& pp = *primitive_processor_;
    XELOGI("CpMem: requests={:.0f}/frame slow={:.0f} uploads={:.0f} "
           "time={:.2f}ms hashes={:.0f} hashKB={:.0f} | prim cache tx={:.0f} "
           "hits={:.0f} small={:.0f} types none={:.0f} dma={:.0f} conv={:.0f} "
           "builtin={:.0f} t4={:.0f} t5={:.0f} | preswap hits={:.0f} "
           "misses={:.0f} stale={:.0f} idx={:.0f} time={:.2f}ms "
           "compare hits={:.0f} copies={:.0f}",
           sm.stat_requests_ / fr, sm.stat_request_slow_ / fr,
           sm.stat_request_uploads_ / fr, sm.stat_request_ns_ / fr / 1e6,
           sm.stat_mark_hashes_ / fr, sm.stat_mark_hash_bytes_ / fr / 1024.0,
           pp.stat_cache_tx_ / fr, pp.stat_cache_hits_ / fr,
           pp.stat_cache_small_ / fr, pp.stat_type_[0] / fr,
           pp.stat_type_[1] / fr, pp.stat_type_[2] / fr, pp.stat_type_[3] / fr,
           pp.stat_type_[4] / fr, pp.stat_type_[5] / fr,
           pp.stat_preswap_hits_ / fr, pp.stat_preswap_misses_ / fr,
           pp.stat_preswap_stale_ / fr, pp.stat_preswap_indices_ / fr, pp.stat_preswap_ns_ / fr / 1e6,
           pp.stat_preswap_compare_hits_ / fr, pp.stat_preswap_compare_copies_ / fr);
    pp.stat_preswap_indices_ = pp.stat_preswap_ns_ = 0;
    pp.stat_preswap_compare_hits_ = pp.stat_preswap_compare_copies_ = 0;
    pp.stat_preswap_hits_ = pp.stat_preswap_misses_ = pp.stat_preswap_stale_ = 0;
    sm.stat_requests_ = sm.stat_request_slow_ = sm.stat_request_uploads_ = 0;
    sm.stat_request_ns_ = sm.stat_mark_hashes_ = sm.stat_mark_hash_bytes_ = 0;
    pp.stat_cache_tx_ = pp.stat_cache_hits_ = pp.stat_cache_small_ = 0;
    std::memset(pp.stat_type_, 0, sizeof(pp.stat_type_));
    mem_stats_frames_ = 0;
  }
  draw_state_tracking_ = cvars::log_draw_state_repeats;

  if (render_target_cache_) {
    render_target_cache_->LogResolveDetailsOnFrameEnd();
  }

  if (cvars::log_render_pass_breaks > 0 &&
      ++render_pass_break_frames_ >=
          uint32_t(cvars::log_render_pass_breaks)) {
    Dl_info dl_info = {};
    uintptr_t base = 0;
    if (dladdr(reinterpret_cast<void*>(&cvars::log_render_pass_breaks),
               &dl_info)) {
      base = reinterpret_cast<uintptr_t>(dl_info.dli_fbase);
    }
    std::vector<std::pair<uint64_t, uint64_t>> sorted(
        render_pass_break_counts_.begin(), render_pass_break_counts_.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < std::min(sorted.size(), size_t(16)); ++i) {
      uint64_t direct = sorted[i].first & 0xFFFFFFFFull;
      uint64_t outer = sorted[i].first >> 32;
      text += fmt::format(" {:X}/{:X}={:.1f}", (direct - base) & 0xFFFFFFFFull,
                          outer ? ((outer - base) & 0xFFFFFFFFull) : 0,
                          double(sorted[i].second) / render_pass_break_frames_);
    }
    XELOGI("[RPBREAK] frames={} per-frame (direct/outer offsets):{}",
           render_pass_break_frames_, text);
    ResolveDeferReport();
    std::string pushers;
    for (const auto& [pusher, count] : render_pass_barrier_pushers_) {
      pushers += fmt::format(
          " {:X}={:.1f}", pusher ? (pusher - base) & 0xFFFFFFFFull : 0,
          double(count) / render_pass_break_frames_);
    }
    XELOGI("[RPBARRIER] passes ended by pending barriers, by first pusher:{}",
           pushers);
    {
      std::vector<std::pair<uint64_t, uint64_t>> reentries(
          render_pass_reentry_counts_.begin(),
          render_pass_reentry_counts_.end());
      std::sort(reentries.begin(), reentries.end(),
                [](const auto& a, const auto& b) {
                  return a.second > b.second;
                });
      std::string reentry_text;
      for (size_t i = 0; i < std::min(reentries.size(), size_t(12)); ++i) {
        uint64_t direct = reentries[i].first & 0xFFFFFFFFull;
        uint64_t outer = reentries[i].first >> 32;
        reentry_text += fmt::format(
            " {:X}/{:X}={:.1f}", (direct - base) & 0xFFFFFFFFull,
            outer ? ((outer - base) & 0xFFFFFFFFull) : 0,
            double(reentries[i].second) / render_pass_break_frames_);
      }
      XELOGI("[RPREENTRY] same framebuffer resumed after the break:{}",
             reentry_text);
      render_pass_reentry_counts_.clear();
    }
    render_pass_barrier_pushers_.clear();
    render_pass_break_counts_.clear();
    render_pass_break_frames_ = 0;
  }

  if (cvars::log_vertex_shader_hotness > 0 &&
      ++vertex_shader_hotness_frames_ >=
          uint32_t(cvars::log_vertex_shader_hotness)) {
    std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> hot(
        vertex_shader_hotness_.begin(), vertex_shader_hotness_.end());
    std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) {
      return a.second.first > b.second.first;
    });
    uint64_t total = 0;
    for (const auto& h : hot) {
      total += h.second.first;
    }
    const double f = double(vertex_shader_hotness_frames_);
    XELOGI("VsHot: {} shaders, {:.0f} indices/frame", hot.size(), total / f);
    for (size_t i = 0; i < std::min(hot.size(), size_t(20)); ++i) {
      XELOGI("VsHot: {:016X} {:.0f} indices/frame {:.1f} draws/frame",
             hot[i].first, hot[i].second.first / f, hot[i].second.second / f);
    }
    vertex_shader_hotness_.clear();
    vertex_shader_hotness_frames_ = 0;
  }
  if (cvars::thermal_governor) {
    ThermalGovernorPoll();
  } else {
    thermal_governor_rate_ = 0;
    xe::cpu::backend::thermal_hot.store(adaptive_gpu_bound_,
                                        std::memory_order_relaxed);
  }
  if (cvars::adaptive_shading_rate) {
    AdaptiveShadingPoll();
  } else {
    adaptive_shading_rate_ = 0;
    adaptive_shading_window_ns_ = 0;
    adaptive_shading_calm_windows_ = 0;
    adaptive_gpu_bound_ = false;
  }
  xe::cpu::backend::frame_draw_count.store(frame_draws_for_pacing_,
                                           std::memory_order_relaxed);
  frame_draws_for_pacing_ = 0;
  if (cvars::log_guest_fps) {
    ++guest_fps_frames_;
    const uint64_t now = FrameStatsNow();
    if (!guest_fps_last_ns_) {
      guest_fps_last_ns_ = now;
      guest_fps_frames_ = 0;
      guest_fps_draws_ = 0;
    } else if (now - guest_fps_last_ns_ >= 1000000000ull) {
      XELOGI("GuestFps: {:.2f} fps, {:.0f} draws/frame, gpu {:.1f} ms",
             double(guest_fps_frames_) * 1e9 / double(now - guest_fps_last_ns_),
             guest_fps_frames_ ? double(guest_fps_draws_) / guest_fps_frames_
                               : 0.0,
             GetGpuFrameTimeMs());
      // Compute dispatches per frame by site: dispatches/thousand groups.
      static const char* const kSiteNames[] = {
          "other", "swap",  "memexport", "copy",     "resolve_direct",
          "mat_depth", "resolve", "resolve_clear", "depth_store", "tex_load", "dump"};
      static_assert(std::size(kSiteNames) ==
                    size_t(DeferredCommandBuffer::DispatchSite::kCount));
      auto& ds = DeferredCommandBuffer::dispatch_stats();
      std::string sites;
      const double frames = std::max(guest_fps_frames_, uint32_t(1));
      for (size_t i = 0; i < std::size(kSiteNames); ++i) {
        if (ds.dispatches[i]) {
          sites += fmt::format(" {}={:.1f}/{:.1f}k", kSiteNames[i],
                               ds.dispatches[i] / frames,
                               ds.groups[i] / frames / 1000.0);
        }
      }
      ds = {};
      XELOGI("GuestDispatch:{} | memexport batched {:.1f} draws/fr in {:.1f} "
             "dispatches | alpha-tested depth prepass {:.1f} draws/fr "
             "(not ready {:.1f}; set up {} rejected nocolor={} early={} "
             "shader={} zfunc={} stencil={} blend={} variant={} "
             "pipeline={})",
             sites, memexport_batched_draws_ / frames,
             memexport_batch_dispatches_ / frames,
             alpha_tested_prepass_draws_ / frames,
             alpha_tested_prepass_unready_ / frames,
             alpha_tested_prepass_configured_,
             alpha_tested_prepass_rejects_[0], alpha_tested_prepass_rejects_[1],
             alpha_tested_prepass_rejects_[2], alpha_tested_prepass_rejects_[3],
             alpha_tested_prepass_rejects_[4], alpha_tested_prepass_rejects_[5],
             alpha_tested_prepass_rejects_[6],
             alpha_tested_prepass_rejects_[7]);
      alpha_tested_prepass_configured_ = 0;
      std::fill(std::begin(alpha_tested_prepass_rejects_),
                std::end(alpha_tested_prepass_rejects_), uint64_t(0));
      memexport_batched_draws_ = 0;
      memexport_batch_dispatches_ = 0;
      alpha_tested_prepass_draws_ = 0;
      alpha_tested_prepass_unready_ = 0;
      guest_fps_last_ns_ = now;
      guest_fps_frames_ = 0;
      guest_fps_draws_ = 0;
    }
  }

  if (cvars::log_gpu_frame_time_breakdown) {
    auto& s = vk_frame_sync_stats_;
    s.frames++;
    const uint64_t now = FrameStatsNow();
    if (!s.last_report_ns) {
      s.last_report_ns = now;
    } else if (now - s.last_report_ns >= 1000000000ull) {
      const double f = static_cast<double>(s.frames);
      XELOGI(
          "VkFrameSync: {} frames | per frame: awaits={:.1f} await={:.1f}ms "
          "submissions={:.1f} resolves={:.1f} memexport_awaits={:.1f} "
          "readback_awaits={:.1f} "
          "| submit->fence avg={:.1f}ms max={:.1f}ms | blocking={:.1f} "
          "delta avg={:.2f} | gpu exec avg={:.1f}ms max={:.1f}ms "
          "gap avg={:.1f}ms | resolve_ms={:.2f} avg={:.3f} max={:.2f} "
          "dropped={} | draws={:.0f} rp_begins={:.0f} splits={:.1f} "
          "verts={:.0f}",
          s.frames, s.awaits / f, s.await_ns / f / 1e6, s.submissions / f,
          s.resolves / f, s.memexport_awaits / f, s.readback_awaits / f,
          s.sub_completions
              ? s.sub_latency_ns / static_cast<double>(s.sub_completions) / 1e6
              : 0.0,
          s.sub_latency_max_ns / 1e6, s.blocking_awaits / f,
          s.blocking_awaits
              ? s.await_delta / static_cast<double>(s.blocking_awaits)
              : 0.0,
          s.gpu_samples
              ? s.gpu_exec_ns / static_cast<double>(s.gpu_samples) / 1e6
              : 0.0,
          s.gpu_exec_max_ns / 1e6,
          s.gpu_samples
              ? s.gpu_gap_ns / static_cast<double>(s.gpu_samples) / 1e6
              : 0.0,
          s.resolve_gpu_ns / f / 1e6,
          s.resolve_gpu_samples
              ? s.resolve_gpu_ns /
                    static_cast<double>(s.resolve_gpu_samples) / 1e6
              : 0.0,
          s.resolve_gpu_max_ns / 1e6, s.resolve_ts_dropped, s.draws / f,
          s.render_pass_begins / f, s.primary_buffer_splits / f,
          s.vertices / f);
      {
        // What the draws cost in recorded commands (the GPU front end executes
        // every one of them).
        DeferredCommandBuffer::CommandStats cs =
            deferred_command_buffer_->GetCommandStats();
        const DeferredCommandBuffer::CommandStats& p =
            frame_sync_command_stats_prev_;
        XELOGI(
            "VkCmdStats per frame: draws={:.0f} pipelines={:.0f} "
            "descsets={:.0f} pushconst={:.0f} vp/scissor={:.0f} "
            "dynstate={:.0f} barriers={:.0f} ib/vb={:.0f}",
            (cs.draws - p.draws) / f, (cs.pipeline_binds - p.pipeline_binds) / f,
            (cs.descriptor_set_binds - p.descriptor_set_binds) / f,
            (cs.push_constants - p.push_constants) / f,
            (cs.viewports_scissors - p.viewports_scissors) / f,
            (cs.other_dynamic_state - p.other_dynamic_state) / f,
            (cs.barriers - p.barriers) / f,
            (cs.index_vertex_buffer_binds - p.index_vertex_buffer_binds) / f);
        frame_sync_command_stats_prev_ = cs;
      }
      // Per-render-pass-bucket GPU time (key: WxH, bit31 = ownership transfer).
      if (!pass_bucket_stats_.empty()) {
        static const char* const kGapSiteNames[] = {
            "other", "swap", "memexport", "copy", "resolve_direct",
            "mat_depth", "resolve", "resolve_clear", "depth_store",
            "tex_load", "dump"};
        static_assert(std::size(kGapSiteNames) ==
                      size_t(DeferredCommandBuffer::DispatchSite::kCount));
        for (const auto& kv : pass_bucket_stats_) {
          const uint32_t key = kv.first;
          if (key & kPassTimestampGapKey) {
            std::string sites;
            for (size_t i = 0; i < std::size(kGapSiteNames); ++i) {
              if (key & (uint32_t(1) << i)) {
                sites += sites.empty() ? "" : "+";
                sites += kGapSiteNames[i];
              }
            }
            XELOGI("VkPassTime: gap {} : {:.2f}ms/fr ({:.1f}/fr, {:.3f}ms ea)",
                   sites.empty() ? "no-dispatch" : sites,
                   kv.second.ns / f / 1e6, kv.second.passes / f,
                   kv.second.passes ? kv.second.ns /
                                          static_cast<double>(kv.second.passes) /
                                          1e6
                                    : 0.0);
            continue;
          }
          const bool transfer = (key & 0x80000000u) != 0;
          XELOGI(
              "VkPassTime: {}{}x{} : {:.2f}ms/fr ({:.1f}pass {:.0f}draw/fr, "
              "{:.3f}ms ea) scissor<={}x{} viewport<={}x{}",
              transfer ? "xfer " : "", (key >> 16) & 0x7FFF, key & 0xFFFF,
              kv.second.ns / f / 1e6, kv.second.passes / f, kv.second.draws / f,
              kv.second.passes
                  ? kv.second.ns / static_cast<double>(kv.second.passes) / 1e6
                  : 0.0,
              kv.second.max_scissor_w, kv.second.max_scissor_h,
              kv.second.max_viewport_w, kv.second.max_viewport_h);
        }
        pass_bucket_stats_.clear();
      }
      if (pass_ts_dropped_) {
        XELOGI("VkPassTime: {} pass pairs dropped (raise pool)",
               pass_ts_dropped_);
        pass_ts_dropped_ = 0;
      }
      XELOGI(
          "VkTexLoadTime: {:.2f}ms/fr over {:.1f} loads/fr ({:.3f}ms avg, "
          "{:.3f}ms max) | first {:.2f}ms/{:.1f} cpu {:.2f}ms/{:.1f} gpu "
          "{:.2f}ms/{:.1f}",
          s.texload_gpu_ns / f / 1e6, s.texload_gpu_samples / f,
          s.texload_gpu_samples
              ? s.texload_gpu_ns / double(s.texload_gpu_samples) / 1e6
              : 0.0,
          s.texload_gpu_max_ns / 1e6, s.texload_kind_ns[0] / f / 1e6,
          s.texload_kind_samples[0] / f, s.texload_kind_ns[1] / f / 1e6,
          s.texload_kind_samples[1] / f, s.texload_kind_ns[2] / f / 1e6,
          s.texload_kind_samples[2] / f);
      for (uint32_t format = 0; format < 64; ++format) {
        const auto& r = s.resolve_texture[format];
        if (r.avoided || r.reloaded || r.copy_ns || r.reload_ns) {
          XELOGI(
              "VkResolveTexture fmt={}: avoided {:.2f}/fr reloaded {:.2f}/fr "
              "reload {:.3f}ms/fr copy {:.3f}ms/fr",
              format, r.avoided / f, r.reloaded / f, r.reload_ns / f / 1e6,
              r.copy_ns / f / 1e6);
        }
      }
      s = VkFrameSyncStats();
      s.last_report_ns = now;
    }
  }

  if (cvars::log_memexport_ram_mirror) {
    MemexportMirrorStats& m = memexport_mirror_stats_;
    ++m.frames;
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
      last_report = now;
      const double f = double(m.frames);
      XELOGI(
          "MemexportMirror: per frame ranges {:.1f} pages visited {:.1f} "
          "copied {:.1f} bytes {:.0f} KB (dedup {})",
          m.ranges / f, m.pages / f, m.copied / f, m.bytes / f / 1024.0,
          cvars::vulkan_memexport_ram_mirror_dedup);
      m = MemexportMirrorStats();
    }
  }

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    return;
  }

  // Obtaining the actual front buffer size to pass to RefreshGuestOutput,
  // resolution-scaled if it's a resolve destination, or not otherwise.
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  xenos::TextureFormat frontbuffer_format;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format);
  if (swap_texture_view == VK_NULL_HANDLE) {
    return;
  }

  auto aspect = graphics_system_->GetScaledAspectRatio();

  const uint64_t guest_output_capture_frame = frame_current_;
  const uint64_t sb_t1 = sb_t0 ? FrameStatsNow() : 0;
  bool guest_output_refreshed = presenter->RefreshGuestOutput(
      frontbuffer_width_scaled, frontbuffer_height_scaled, aspect.first,
      aspect.second,
      [this, frontbuffer_width_scaled, frontbuffer_height_scaled,
       frontbuffer_format, swap_texture_view, sb_t0, &sb_l0, &sb_l1, &sb_l2](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        if (sb_t0) sb_l0 = FrameStatsNow();
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context = static_cast<
            ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
            context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn =
            vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index =
            uint32_t(frame_current_ % kMaxFramesInFlight);

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format ==
                xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_image_ != VK_NULL_HANDLE &&
              (fxaa_source_width_ != frontbuffer_width_scaled ||
               fxaa_source_height_ != frontbuffer_height_scaled)) {
            // Need to resize the FXAA source texture.
            if (GetCompletedSubmission() < fxaa_source_last_submission_) {
              // Still in use - defer destruction.
              destroy_memory_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_memory_);
              destroy_images_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_image_);
              destroy_image_views_.emplace_back(fxaa_source_last_submission_,
                                                fxaa_source_image_view_);
            } else {
              dfn.vkDestroyImageView(device, fxaa_source_image_view_, nullptr);
              dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
              dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
            }
            fxaa_source_image_ = VK_NULL_HANDLE;
            fxaa_source_image_view_ = VK_NULL_HANDLE;
            fxaa_source_memory_ = VK_NULL_HANDLE;
            fxaa_source_width_ = 0;
            fxaa_source_height_ = 0;
            fxaa_source_last_submission_ = 0;
          }
          if (fxaa_source_image_ == VK_NULL_HANDLE) {
            // Create the FXAA source texture.
            VkImageCreateInfo fxaa_source_image_create_info;
            fxaa_source_image_create_info.sType =
                VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            fxaa_source_image_create_info.pNext = nullptr;
            fxaa_source_image_create_info.flags = 0;
            fxaa_source_image_create_info.imageType = VK_IMAGE_TYPE_2D;
            fxaa_source_image_create_info.format = kFxaaSourceFormat;
            fxaa_source_image_create_info.extent.width =
                frontbuffer_width_scaled;
            fxaa_source_image_create_info.extent.height =
                frontbuffer_height_scaled;
            fxaa_source_image_create_info.extent.depth = 1;
            fxaa_source_image_create_info.mipLevels = 1;
            fxaa_source_image_create_info.arrayLayers = 1;
            fxaa_source_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
            fxaa_source_image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            fxaa_source_image_create_info.usage =
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            fxaa_source_image_create_info.sharingMode =
                VK_SHARING_MODE_EXCLUSIVE;
            fxaa_source_image_create_info.queueFamilyIndexCount = 0;
            fxaa_source_image_create_info.pQueueFamilyIndices = nullptr;
            fxaa_source_image_create_info.initialLayout =
                VK_IMAGE_LAYOUT_UNDEFINED;
            if (dfn.vkCreateImage(device, &fxaa_source_image_create_info,
                                  nullptr, &fxaa_source_image_) != VK_SUCCESS) {
              XELOGE("Failed to create the FXAA source image");
              use_fxaa = false;
            } else {
              VkMemoryRequirements fxaa_source_memory_requirements;
              dfn.vkGetImageMemoryRequirements(
                  device, fxaa_source_image_, &fxaa_source_memory_requirements);
              VkMemoryAllocateInfo fxaa_source_memory_allocate_info;
              fxaa_source_memory_allocate_info.sType =
                  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
              fxaa_source_memory_allocate_info.pNext = nullptr;
              fxaa_source_memory_allocate_info.allocationSize =
                  fxaa_source_memory_requirements.size;
              fxaa_source_memory_allocate_info.memoryTypeIndex =
                  ui::vulkan::util::ChooseMemoryType(
                      vulkan_device->memory_types(),
                      fxaa_source_memory_requirements.memoryTypeBits,
                      ui::vulkan::util::MemoryPurpose::kDeviceLocal);
              if (fxaa_source_memory_allocate_info.memoryTypeIndex ==
                      UINT32_MAX ||
                  dfn.vkAllocateMemory(
                      device, &fxaa_source_memory_allocate_info, nullptr,
                      &fxaa_source_memory_) != VK_SUCCESS) {
                XELOGE("Failed to allocate FXAA source image memory");
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else if (dfn.vkBindImageMemory(device, fxaa_source_image_,
                                               fxaa_source_memory_,
                                               0) != VK_SUCCESS) {
                XELOGE("Failed to bind FXAA source image memory");
                dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                fxaa_source_memory_ = VK_NULL_HANDLE;
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else {
                VkImageViewCreateInfo fxaa_source_image_view_create_info;
                fxaa_source_image_view_create_info.sType =
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                fxaa_source_image_view_create_info.pNext = nullptr;
                fxaa_source_image_view_create_info.flags = 0;
                fxaa_source_image_view_create_info.image = fxaa_source_image_;
                fxaa_source_image_view_create_info.viewType =
                    VK_IMAGE_VIEW_TYPE_2D;
                fxaa_source_image_view_create_info.format = kFxaaSourceFormat;
                fxaa_source_image_view_create_info.components.r =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.g =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.b =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.a =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseMipLevel = 0;
                fxaa_source_image_view_create_info.subresourceRange.levelCount =
                    1;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseArrayLayer = 0;
                fxaa_source_image_view_create_info.subresourceRange.layerCount =
                    1;
                if (dfn.vkCreateImageView(
                        device, &fxaa_source_image_view_create_info, nullptr,
                        &fxaa_source_image_view_) != VK_SUCCESS) {
                  XELOGE("Failed to create the FXAA source image view");
                  dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                  fxaa_source_memory_ = VK_NULL_HANDLE;
                  dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                  fxaa_source_image_ = VK_NULL_HANDLE;
                  use_fxaa = false;
                } else {
                  fxaa_source_width_ = frontbuffer_width_scaled;
                  fxaa_source_height_ = frontbuffer_height_scaled;
                }
              }
            }
          }
        }

        // FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref =
            use_pwl_gamma_ramp ? gamma_ramp_pwl_current_frame_
                               : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable =
              sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize =
              kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size = use_pwl_gamma_ramp
                                         ? kGammaRampSizePWL
                                         : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) +
              gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little &&
              use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                    gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry =
                  gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(
                gamma_ramp_frame_upload,
                use_pwl_gamma_ramp
                    ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                    : static_cast<const void*>(gamma_ramp_256_entry_table()),
                gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer =
              gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            InsertDebugMarker("Gamma Ramp Upload: %u bytes",
                              static_cast<uint32_t>(gamma_ramp_size));
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_->CmdVkCopyBuffer(gamma_ramp_upload_buffer_,
                                                     gamma_ramp_buffer_, 1,
                                                     &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref =
              gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        // Track FXAA source texture submission.
        if (use_fxaa) {
          fxaa_source_last_submission_ = GetCurrentSubmission();
        }

        // Determine the destination image for gamma application.
        // If FXAA is enabled, gamma writes to the FXAA source texture.
        // Otherwise, it writes directly to the guest output.
        VkImage apply_gamma_dest_image =
            use_fxaa ? fxaa_source_image_ : vulkan_context.image();
        VkImageView apply_gamma_dest_image_view =
            use_fxaa ? fxaa_source_image_view_ : vulkan_context.image_view();

        // Transition destination image for compute shader storage image write.
        if (use_fxaa) {
          // FXAA source - transition from undefined (we'll be overwriting).
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        } else if (vulkan_context.image_ever_written_previously()) {
          // Guest output - insert a barrier after the last presenter's usage.
          // Will be overwriting all the contents, so oldLayout can be
          // UNDEFINED.
          PushImageMemoryBarrier(
              apply_gamma_dest_image,
              ui::vulkan::util::InitializeSubresourceRange(),
              ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
              VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL);
        } else {
          // Guest output - first write to the image, just transition.
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        }

        // End the current render pass before inserting barriers.
        SubmitBarriers(true);

        PushDebugMarker("Apply Gamma Ramp: %s%s",
                        use_pwl_gamma_ramp ? "PWL" : "256-entry table",
                        use_fxaa ? " (with FXAA luma)" : "");

        // Select the appropriate gamma pipeline.
        VkPipeline gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          gamma_pipeline = use_fxaa ? swap_apply_gamma_pwl_fxaa_luma_pipeline_
                                    : swap_apply_gamma_pwl_pipeline_;
        } else {
          gamma_pipeline =
              use_fxaa ? swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_
                       : swap_apply_gamma_256_entry_table_pipeline_;
        }
        deferred_command_buffer_->CmdVkBindPipeline(
            VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline);

        // Update the source descriptor set with the swap texture.
        VkDescriptorSet swap_descriptor_source =
            swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType =
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo =
            &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;

        // Update the destination descriptor set with the destination image.
        // When FXAA is enabled, use a separate descriptor set for writing to
        // FXAA source to avoid the issue where both passes would use the same
        // descriptor set (which gets updated twice before submission).
        VkDescriptorSet swap_descriptor_dest =
            use_fxaa ? fxaa_source_storage_descriptors_[swap_frame_index]
                     : swap_descriptors_dest_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_dest_image_info;
        swap_descriptor_dest_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_dest_image_info.imageView = apply_gamma_dest_image_view;
        swap_descriptor_dest_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet swap_descriptor_dest_write;
        swap_descriptor_dest_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_dest_write.pNext = nullptr;
        swap_descriptor_dest_write.dstSet = swap_descriptor_dest;
        swap_descriptor_dest_write.dstBinding = 0;
        swap_descriptor_dest_write.dstArrayElement = 0;
        swap_descriptor_dest_write.descriptorCount = 1;
        swap_descriptor_dest_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        swap_descriptor_dest_write.pImageInfo =
            &swap_descriptor_dest_image_info;
        swap_descriptor_dest_write.pBufferInfo = nullptr;
        swap_descriptor_dest_write.pTexelBufferView = nullptr;

        std::array<VkWriteDescriptorSet, 2> swap_descriptor_writes = {
            swap_descriptor_source_write, swap_descriptor_dest_write};
        dfn.vkUpdateDescriptorSets(device,
                                   uint32_t(swap_descriptor_writes.size()),
                                   swap_descriptor_writes.data(), 0, nullptr);

        // Set push constants.
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = frontbuffer_width_scaled;
        apply_gamma_constants.size[1] = frontbuffer_height_scaled;
        deferred_command_buffer_->CmdVkPushConstants(
            swap_apply_gamma_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(apply_gamma_constants), &apply_gamma_constants);

        // Bind descriptor sets.
        std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount>
            swap_descriptor_sets{};
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
            swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                         uint32_t(use_pwl_gamma_ramp)];
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] =
            swap_descriptor_source;
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetDest] =
            swap_descriptor_dest;
        // TODO(Triang3l): Red / blue swap without imageViewFormatSwizzle.
        deferred_command_buffer_->CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_pipeline_layout_,
            0, uint32_t(swap_descriptor_sets.size()),
            swap_descriptor_sets.data(), 0, nullptr);

        // Dispatch compute shader. Local size is 16x8.
        uint32_t group_count_x = (frontbuffer_width_scaled + 15) / 16;
        uint32_t group_count_y = (frontbuffer_height_scaled + 7) / 8;
        ++submission_in_progress_.dispatch_count;
        deferred_command_buffer_->CmdVkDispatch(group_count_x, group_count_y, 1,
                                                 DeferredCommandBuffer::DispatchSite::kSwap);

        PopDebugMarker();

        // Apply FXAA if enabled.
        if (use_fxaa) {
          PushDebugMarker("FXAA: %s",
                          swap_post_effect == SwapPostEffect::kFxaaExtreme
                              ? "Extreme"
                              : "Standard");

          // Transition FXAA source from storage image write to sampled read.
          // Transition guest output to storage image write.
          PushImageMemoryBarrier(
              fxaa_source_image_,
              ui::vulkan::util::InitializeSubresourceRange(),
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          if (vulkan_context.image_ever_written_previously()) {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          } else {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);

          // Bind FXAA pipeline.
          deferred_command_buffer_->CmdVkBindPipeline(
              VK_PIPELINE_BIND_POINT_COMPUTE,
              swap_post_effect == SwapPostEffect::kFxaaExtreme
                  ? fxaa_extreme_pipeline_
                  : fxaa_pipeline_);

          // Update FXAA source descriptor (combined image sampler).
          VkDescriptorSet fxaa_source_descriptor =
              fxaa_source_descriptors_[swap_frame_index];
          VkDescriptorImageInfo fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_image_info.sampler = fxaa_sampler_;
          fxaa_source_descriptor_image_info.imageView = fxaa_source_image_view_;
          fxaa_source_descriptor_image_info.imageLayout =
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          VkWriteDescriptorSet fxaa_source_descriptor_write;
          fxaa_source_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_source_descriptor_write.pNext = nullptr;
          fxaa_source_descriptor_write.dstSet = fxaa_source_descriptor;
          fxaa_source_descriptor_write.dstBinding = 0;
          fxaa_source_descriptor_write.dstArrayElement = 0;
          fxaa_source_descriptor_write.descriptorCount = 1;
          fxaa_source_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          fxaa_source_descriptor_write.pImageInfo =
              &fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_write.pBufferInfo = nullptr;
          fxaa_source_descriptor_write.pTexelBufferView = nullptr;

          // Update FXAA destination descriptor (guest output image).
          VkDescriptorSet fxaa_dest_descriptor =
              swap_descriptors_dest_[swap_frame_index];
          VkDescriptorImageInfo fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_image_info.sampler = VK_NULL_HANDLE;
          fxaa_dest_descriptor_image_info.imageView =
              vulkan_context.image_view();
          fxaa_dest_descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet fxaa_dest_descriptor_write;
          fxaa_dest_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_dest_descriptor_write.pNext = nullptr;
          fxaa_dest_descriptor_write.dstSet = fxaa_dest_descriptor;
          fxaa_dest_descriptor_write.dstBinding = 0;
          fxaa_dest_descriptor_write.dstArrayElement = 0;
          fxaa_dest_descriptor_write.descriptorCount = 1;
          fxaa_dest_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          fxaa_dest_descriptor_write.pImageInfo =
              &fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_write.pBufferInfo = nullptr;
          fxaa_dest_descriptor_write.pTexelBufferView = nullptr;

          std::array<VkWriteDescriptorSet, 2> fxaa_descriptor_writes = {
              fxaa_source_descriptor_write, fxaa_dest_descriptor_write};
          dfn.vkUpdateDescriptorSets(device,
                                     uint32_t(fxaa_descriptor_writes.size()),
                                     fxaa_descriptor_writes.data(), 0, nullptr);

          // Set FXAA push constants.
          FxaaConstants fxaa_constants;
          fxaa_constants.size[0] = frontbuffer_width_scaled;
          fxaa_constants.size[1] = frontbuffer_height_scaled;
          fxaa_constants.size_inv[0] =
              1.0f / static_cast<float>(frontbuffer_width_scaled);
          fxaa_constants.size_inv[1] =
              1.0f / static_cast<float>(frontbuffer_height_scaled);
          deferred_command_buffer_->CmdVkPushConstants(
              fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(fxaa_constants), &fxaa_constants);

          // Bind FXAA descriptor sets.
          std::array<VkDescriptorSet, 2> fxaa_descriptor_sets = {
              fxaa_dest_descriptor, fxaa_source_descriptor};
          deferred_command_buffer_->CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, fxaa_pipeline_layout_, 0,
              uint32_t(fxaa_descriptor_sets.size()),
              fxaa_descriptor_sets.data(), 0, nullptr);

          // Dispatch FXAA compute shader.
          ++submission_in_progress_.dispatch_count;
          deferred_command_buffer_->CmdVkDispatch(group_count_x, group_count_y,
                                                 1, DeferredCommandBuffer::DispatchSite::kSwap);

          PopDebugMarker();
        }

        // Insert the release barrier - transition from GENERAL to the
        // presenter's expected layout.
        PushImageMemoryBarrier(
            vulkan_context.image(),
            ui::vulkan::util::InitializeSubresourceRange(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
            VK_ACCESS_SHADER_WRITE_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
            VK_IMAGE_LAYOUT_GENERAL,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        if (sb_t0) sb_l1 = FrameStatsNow();
        EndSubmission(true);
        if (sb_t0) sb_l2 = FrameStatsNow();
        return true;
      });

  const uint64_t sb_t2 = sb_t0 ? FrameStatsNow() : 0;
  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
  if (sb_t0) {
    const uint64_t sb_t3 = FrameStatsNow();
    sb_pre += sb_t1 - sb_t0;
    if (sb_l2) {
      // Presenter time outside the refresh callback (image acquire / locks).
      sb_wait += (sb_t2 - sb_t1) - (sb_l2 - sb_l0);
      sb_rec += sb_l1 - sb_l0;
      sb_sub_in += sb_l2 - sb_l1;
    } else {
      sb_wait += sb_t2 - sb_t1;
    }
    sb_sub_end += sb_t3 - sb_t2;
    if (++sb_frames >= 60) {
      const double f = double(sb_frames) * 1e6;
      XELOGI("SwapBreak: pre={:.2f}ms presenter={:.2f}ms rec={:.2f}ms "
             "submit={:.2f}ms end={:.2f}ms",
             sb_pre / f, sb_wait / f, sb_rec / f, sb_sub_in / f,
             sb_sub_end / f);
      sb_frames = sb_pre = sb_wait = sb_rec = sb_sub_in = sb_sub_end = 0;
    }
  }

  // Consume the request after the refresh submission and presenter work have
  // returned, so readback runs without an open GPU command buffer or presenter
  // paint/mailbox locks. A marker is removed before capture, including on
  // failure, to keep this diagnostic one-shot.
  if (!cvars::live_cvar_file.empty()) {
    PollLiveCvars();
  }
  static bool guest_output_capture_request_consumed = false;
  static int32_t guest_output_capture_sequence_left = 0;
  const std::filesystem::path& capture_request_path =
      cvars::vulkan_guest_output_capture_request;
  auto write_guest_output_sequence_frame = [&](const ui::RawImage& image) {
    const std::filesystem::path raw_path =
        capture_request_path.parent_path() /
        ("guest-output-seq-" + std::to_string(guest_output_capture_frame) +
         ".rgbx");
    FILE* raw_file = xe::filesystem::OpenFile(raw_path, "wb");
    if (!raw_file) {
      XELOGE("[GUEST-OUTPUT-SEQ] status=error frame={} reason=open_failed",
             guest_output_capture_frame);
      return;
    }
    for (uint32_t y = 0; y < image.height; ++y) {
      std::fwrite(image.data.data() + image.stride * y, 1,
                  size_t(image.width) * 4, raw_file);
    }
    std::fclose(raw_file);
    XELOGI("[GUEST-OUTPUT-SEQ] status=ok frame={} width={} height={} path={}",
           guest_output_capture_frame, image.width, image.height, raw_path);
  };
  if (guest_output_capture_sequence_left > 0) {
    // The frames after the captured one (raw only - fast, so the guest's frame
    // time isn't inflated much).
    --guest_output_capture_sequence_left;
    ui::RawImage image;
    if (guest_output_refreshed && presenter->CaptureGuestOutput(image)) {
      write_guest_output_sequence_frame(image);
    }
  }
  if (!guest_output_capture_request_consumed &&
      !capture_request_path.empty()) {
    std::error_code request_error;
    if (std::filesystem::exists(capture_request_path, request_error)) {
      guest_output_capture_request_consumed = true;
      std::error_code remove_error;
      std::filesystem::remove(capture_request_path, remove_error);
      if (remove_error) {
        XELOGW(
            "[GUEST-OUTPUT-CAPTURE] status=warn frame={} phase=consume "
            "request_path={} error={}",
            guest_output_capture_frame, capture_request_path,
            remove_error.message());
      }

      if (!guest_output_refreshed) {
        XELOGE(
            "[GUEST-OUTPUT-CAPTURE] status=error frame={} phase=refresh "
            "request_path={} reason=refresh_failed",
            guest_output_capture_frame, capture_request_path);
      } else {
        ui::RawImage image;
        if (!presenter->CaptureGuestOutput(image)) {
          XELOGE(
              "[GUEST-OUTPUT-CAPTURE] status=error frame={} phase=readback "
              "request_path={} reason=capture_failed",
              guest_output_capture_frame, capture_request_path);
        } else {
          const std::filesystem::path output_path =
              capture_request_path.parent_path() /
              ("guest-output-pre-present-frame-" +
               std::to_string(guest_output_capture_frame) + ".png");
          FILE* output_file = xe::filesystem::OpenFile(output_path, "wb");
          if (!output_file) {
            XELOGE(
                "[GUEST-OUTPUT-CAPTURE] status=error frame={} phase=open "
                "path={} reason=open_failed",
                guest_output_capture_frame, output_path);
          } else {
            const auto write_callback = [](void* context, void* data,
                                           int size) {
              std::fwrite(data, 1, static_cast<size_t>(size),
                          static_cast<FILE*>(context));
            };
            const int write_result = stbi_write_png_to_func(
                write_callback, output_file, static_cast<int>(image.width),
                static_cast<int>(image.height), 4, image.data.data(),
                static_cast<int>(image.stride));
            const bool output_error = std::ferror(output_file) != 0;
            const int close_result = std::fclose(output_file);
            if (write_result == 0 || output_error || close_result != 0) {
              XELOGE(
                  "[GUEST-OUTPUT-CAPTURE] status=error frame={} "
                  "phase=png path={} encode_result={} io_error={} "
                  "close_result={}",
                  guest_output_capture_frame, output_path, write_result,
                  output_error, close_result);
            } else {
              if (cvars::vulkan_guest_output_capture_sequence > 1) {
                write_guest_output_sequence_frame(image);
                guest_output_capture_sequence_left =
                    cvars::vulkan_guest_output_capture_sequence - 1;
              }
              XELOGI(
                  "[GUEST-OUTPUT-CAPTURE] status=ok frame={} width={} "
                  "height={} path={}",
                  guest_output_capture_frame, image.width, image.height,
                  output_path);
            }
          }
        }
      }
      if (texture_cache_ && texture_cache_->HasPendingDebugTextureDumps()) {
        if (AwaitAllQueueOperationsCompletion()) {
          texture_cache_->DebugWriteTextureDumps(
              capture_request_path.parent_path() / "texdump");
        } else {
          XELOGE("[TEXDUMP] status=error reason=wait");
        }
      }
      if (texture_cache_ && texture_cache_->DebugTextureAuditPending()) {
        // Read back the textures the captured frame sampled, in a submission
        // of its own.
        if (BeginSubmission(false)) {
          texture_cache_->DebugRecordTextureAudit();
          EndSubmission(false);
        }
        if (AwaitAllQueueOperationsCompletion()) {
          texture_cache_->DebugWriteTextureAudit(
              capture_request_path.parent_path() / "texaudit");
        } else {
          XELOGE("[TEXAUDIT] status=error reason=wait");
        }
      }
    } else if (request_error) {
      guest_output_capture_request_consumed = true;
      XELOGW(
          "[GUEST-OUTPUT-CAPTURE] status=error frame={} phase=request "
          "request_path={} reason=stat_failed error={}",
          guest_output_capture_frame, capture_request_path,
          request_error.message());
    }
  }
}

bool VulkanCommandProcessor::DeferGuestFenceWrite(uint8_t* destination,
                                                  uint32_t value) {
  if (cvars::log_texture_clobber) {
    static uint32_t logged = 0;
    if (logged++ < 4096) {
      XELOGI("[POOL-F] s={} done_cached={} open={} pending={} dst={:X}",
             GetCurrentSubmission(), GetCompletedSubmission(), submission_open_,
             memexport_await_pending_, reinterpret_cast<uintptr_t>(destination));
    }
  }
  return false;
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  FlushDeferredResolve(ResolveDeferStop::kEvent);
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (cvars::submit_on_primary_buffer_end && submission_open_ &&
      !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
    if (cvars::vulkan_split_submission_only_when_idle) {
      const uint32_t write_ptr_index =
          write_ptr_index_.load(std::memory_order_acquire);
      if (write_ptr_index != 0xBAADF00D &&
          write_ptr_index * sizeof(uint32_t) != reader_.write_offset()) {
        // More commands are already queued - keep the submission open.
        return;
      }
    }
    ++vk_frame_sync_stats_.primary_buffer_splits;
    EndSubmission(false);
  }
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE &&
         offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset +
                 other_buffer_memory_barrier.size <=
             offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset &&
        other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  if ((cvars::log_render_pass_breaks > 0 || pass_bytes_active_) &&
      pending_barriers_buffer_memory_barriers_.empty() &&
      pending_barriers_image_memory_barriers_.empty()) {
    first_barrier_pusher_ = __builtin_return_address(0);
  }
  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    VkImageLayout old_layout, VkImageLayout new_layout,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask && old_layout == new_layout &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask &
          subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount !=
             VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount !=
             VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask ==
            subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount ==
            subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount ==
            subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  if ((cvars::log_render_pass_breaks > 0 || pass_bytes_active_) &&
      pending_barriers_buffer_memory_barriers_.empty() &&
      pending_barriers_image_memory_barriers_.empty()) {
    first_barrier_pusher_ = __builtin_return_address(0);
  }
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

void VulkanCommandProcessor::PushMemexportBeforeBarrier() {
  // Earlier pixel shaders touched the shared memory only if flagged
  // (vulkan_pass_head_overlap): the export needn't wait for their passes.
  VkPipelineStageFlags src_stages = guest_shader_pipeline_stages_ |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (cvars::vulkan_pass_head_overlap && !FragmentSharedMemoryPending()) {
    src_stages &= ~VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  }
  PushBufferMemoryBarrier(
      shared_memory_->buffer(), 0, VK_WHOLE_SIZE, src_stages,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
}

void VulkanCommandProcessor::PushMemexportAfterBarrier() {
  PushBufferMemoryBarrier(
      shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
          VK_ACCESS_TRANSFER_READ_BIT,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
}

void VulkanCommandProcessor::AddToMemExportBatch(
    VkPipeline pipeline, VkPipelineLayout layout, uint32_t set_count,
    uint32_t float_constant_count, uint32_t vertex_count) {
  MemExportBatch& batch = memexport_batch_;
  bool joinable = batch.size && batch.pipeline == pipeline &&
                  batch.set_count == set_count && batch.size < batch.capacity;
  for (uint32_t i = 0; joinable && i < set_count; ++i) {
    joinable = i == SpirvShaderTranslator::kDescriptorSetConstants ||
               batch.sets[i] == current_graphics_descriptor_sets_[i];
  }
  if (batch.size && !joinable) {
    FlushMemExportBatch();
  }
  if (!batch.size) {
    batch.pipeline = pipeline;
    batch.layout = layout;
    batch.set_count = set_count;
    std::memcpy(batch.sets, current_graphics_descriptor_sets_,
                sizeof(VkDescriptorSet) * set_count);
    batch.float_stride =
        SpirvShaderTranslator::MemExportBatchStrideFloat(float_constant_count);
    batch.capacity =
        SpirvShaderTranslator::MemExportBatchSize(float_constant_count);
    batch.max_vertex_count = 0;
    batch.system.resize(size_t(batch.capacity) *
                        SpirvShaderTranslator::kMemExportBatchStrideSystem);
    batch.float_vertex.resize(size_t(batch.capacity) * batch.float_stride);
    batch.bool_loop.resize(size_t(batch.capacity) *
                           SpirvShaderTranslator::kMemExportBatchStrideBoolLoop);
    batch.fetch.resize(size_t(batch.capacity) *
                       SpirvShaderTranslator::kMemExportBatchStrideFetch);
  }
  // The same data UpdateBindings uploaded for this draw.
  const uint32_t draw = batch.size;
  std::memcpy(batch.system.data() +
                  size_t(draw) *
                      SpirvShaderTranslator::kMemExportBatchStrideSystem,
              &system_constants_,
              sizeof(SpirvShaderTranslator::SystemConstants));
  std::memcpy(batch.float_vertex.data() + size_t(draw) * batch.float_stride,
              float_constants_[0].data(),
              sizeof(float) * 4 * float_constant_count);
  std::memcpy(batch.bool_loop.data() +
                  size_t(draw) *
                      SpirvShaderTranslator::kMemExportBatchStrideBoolLoop,
              bool_loop_constants_, sizeof(uint32_t) * (8 + 32));
  std::memcpy(
      batch.fetch.data() +
          size_t(draw) * SpirvShaderTranslator::kMemExportBatchStrideFetch,
      &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
      sizeof(uint32_t) * 6 * 32);
  batch.max_vertex_count = std::max(batch.max_vertex_count, vertex_count);
  ++batch.size;
  ++memexport_batched_draws_;
}

void VulkanCommandProcessor::FlushMemExportBatch() {
  MemExportBatch& batch = memexport_batch_;
  if (!batch.size) {
    return;
  }
  memexport_batch_flushing_ = true;
  // Ordered after everything recorded before the first draw of the batch,
  // like a single export.
  PushMemexportBeforeBarrier();
  SubmitBarriers(true);
  EndRenderPass();

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const VkDeviceSize uniform_buffer_alignment =
      vulkan_device->properties().minUniformBufferOffsetAlignment;
  VkDescriptorBufferInfo buffer_infos[SpirvShaderTranslator::kConstantBufferCount];
  struct Part {
    uint32_t constant_buffer;
    const std::vector<uint8_t>* data;
    uint32_t stride;
  };
  const Part parts[] = {
      {SpirvShaderTranslator::kConstantBufferSystem, &batch.system,
       SpirvShaderTranslator::kMemExportBatchStrideSystem},
      {SpirvShaderTranslator::kConstantBufferFloatVertex, &batch.float_vertex,
       batch.float_stride},
      {SpirvShaderTranslator::kConstantBufferBoolLoop, &batch.bool_loop,
       SpirvShaderTranslator::kMemExportBatchStrideBoolLoop},
      {SpirvShaderTranslator::kConstantBufferFetch, &batch.fetch,
       SpirvShaderTranslator::kMemExportBatchStrideFetch},
  };
  bool constants_written = true;
  for (const Part& part : parts) {
    // The whole declared array is bound; only the used draws are filled.
    const size_t size = size_t(batch.capacity) * part.stride;
    VkDescriptorBufferInfo& info = buffer_infos[part.constant_buffer];
    uint8_t* mapping = uniform_buffer_pool_->Request(
        frame_current_, size, size_t(uniform_buffer_alignment), info.buffer,
        info.offset);
    if (!mapping) {
      constants_written = false;
      break;
    }
    info.range = VkDeviceSize(size);
    std::memcpy(mapping, part.data->data(), size_t(batch.size) * part.stride);
  }
  buffer_infos[SpirvShaderTranslator::kConstantBufferFloatPixel] =
      current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatPixel];

  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  const VkDescriptorType constants_descriptor_type =
      use_dynamic_constants_ ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  if (constants_written) {
    if (!constants_transient_descriptors_free_.empty()) {
      constants_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      constants_descriptor_count.type = constants_descriptor_type;
      constants_descriptor_count.descriptorCount =
          SpirvShaderTranslator::kConstantBufferCount;
      constants_set = transient_descriptor_allocator_uniform_buffer_.Allocate(
          descriptor_set_layout_constants_, &constants_descriptor_count, 1);
    }
  }
  if (constants_set != VK_NULL_HANDLE) {
    constants_transient_descriptors_used_.emplace_back(frame_current_,
                                                       constants_set);
    VkDescriptorBufferInfo
        write_infos[SpirvShaderTranslator::kConstantBufferCount];
    VkWriteDescriptorSet writes[SpirvShaderTranslator::kConstantBufferCount];
    uint32_t dynamic_offsets[SpirvShaderTranslator::kConstantBufferCount];
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
         ++i) {
      write_infos[i] = buffer_infos[i];
      dynamic_offsets[i] = uint32_t(buffer_infos[i].offset);
      if (use_dynamic_constants_) {
        // The slice offset is the dynamic offset, as in UpdateBindings.
        write_infos[i].offset = 0;
      }
      VkWriteDescriptorSet& write = writes[i];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.pNext = nullptr;
      write.dstSet = constants_set;
      write.dstBinding = i;
      write.dstArrayElement = 0;
      write.descriptorCount = 1;
      write.descriptorType = constants_descriptor_type;
      write.pImageInfo = nullptr;
      write.pBufferInfo = &write_infos[i];
      write.pTexelBufferView = nullptr;
    }
    if (draw_config_.vulkan_defer_descriptor_updates) {
      deferred_command_buffer_->CmdUpdateDescriptorSets(
          SpirvShaderTranslator::kConstantBufferCount, writes);
    } else {
      vulkan_device->functions().vkUpdateDescriptorSets(
          vulkan_device->device(), SpirvShaderTranslator::kConstantBufferCount,
          writes, 0, nullptr);
    }
    VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount];
    std::memcpy(sets, batch.sets, sizeof(VkDescriptorSet) * batch.set_count);
    sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_set;
    BindExternalComputePipeline(batch.pipeline);
    deferred_command_buffer_->CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_COMPUTE, batch.layout, 0, batch.set_count, sets,
        use_dynamic_constants_ ? SpirvShaderTranslator::kConstantBufferCount
                               : 0,
        dynamic_offsets);
    deferred_command_buffer_->CmdVkDispatch(
        (batch.max_vertex_count +
         (SpirvShaderTranslator::kMemExportComputeGroupSize - 1)) /
            SpirvShaderTranslator::kMemExportComputeGroupSize,
        batch.size, 1, DeferredCommandBuffer::DispatchSite::kMemExport);
    PushMemexportAfterBarrier();
    ++memexport_batch_dispatches_;
  } else {
    XELOGE("Memexport batch: failed to upload the constants of {} draws",
           batch.size);
  }
  batch.size = 0;
  memexport_batch_flushing_ = false;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  assert_true(submission_open_);
  if (memexport_batch_.size && !memexport_batch_flushing_) {
    FlushMemExportBatch();
  }
  if (cvars::log_render_pass_breaks > 0 || pass_bytes_active_) {
    render_pass_break_caller_ = __builtin_return_address(0);
  }
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass(force_end_render_pass ? "forced" : "pending_barrier");
    }
    return false;
  }
  if (cvars::log_render_pass_breaks > 0 && in_render_pass_) {
    ++render_pass_barrier_pushers_[uint64_t(
        reinterpret_cast<uintptr_t>(first_barrier_pusher_))];
  }
  if (deferred_resolve_pending_ &&
      deferred_resolve_stop_ == ResolveDeferStop::kPassEnd) {
    deferred_resolve_stop_ = ResolveDeferStop::kResource;
  }
  EndRenderPass(force_end_render_pass ? "forced" : "pending_barrier");
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend();
       ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_->CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask
                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() +
            it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() +
            it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::OpenPassTimestamp(uint32_t bucket_key) {
  if (!pass_timestamp_mapping_) {
    return;
  }
  if (pass_ts_gap_open_) {
    CloseGapTimestamp();
  }
  if (pass_ts_submission_ != GetCurrentSubmission()) {
    pass_ts_submission_ = GetCurrentSubmission();
    pass_ts_count_ = 0;
  }
  if (pass_ts_count_ >= kPassTimestampPairsPerSubmission) {
    ++pass_ts_dropped_;
    return;
  }
  const uint32_t pair =
      uint32_t(pass_ts_submission_ % kPassTimestampRingSubmissions) *
          kPassTimestampPairsPerSubmission +
      pass_ts_count_;
  pass_ts_keys_[pair] = bucket_key;
  deferred_command_buffer_->CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timestamp_pool_, pair * 2);
  pass_ts_open_pair_ = pair;
  pass_ts_open_submission_ = pass_ts_submission_;
  pass_open_draws_ = 0;
  pass_open_scissor_w_ = 0;
  pass_open_scissor_h_ = 0;
  pass_open_viewport_w_ = 0;
  pass_open_viewport_h_ = 0;
}

void VulkanCommandProcessor::CloseGapTimestamp() {
  pass_ts_gap_open_ = false;
  if (pass_ts_open_pair_ == UINT32_MAX) {
    return;
  }
  // A gap left open by the previous submission is simply abandoned.
  if (pass_ts_open_submission_ == GetCurrentSubmission()) {
    deferred_command_buffer_->CmdVkWriteTimestamp(
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timestamp_pool_,
        pass_ts_open_pair_ * 2 + 1);
    pass_ts_keys_[pass_ts_open_pair_] =
        kPassTimestampGapKey |
        (DeferredCommandBuffer::dispatch_site_mask() & 0xFFFFu);
    pass_ts_draws_[pass_ts_open_pair_] = 0;
    pass_ts_scissor_[pass_ts_open_pair_] = 0;
    pass_ts_viewport_[pass_ts_open_pair_] = 0;
    ++pass_ts_count_;
  }
  pass_ts_open_pair_ = UINT32_MAX;
}

void VulkanCommandProcessor::ClosePassTimestamp() {
  if (pass_ts_open_pair_ == UINT32_MAX || pass_ts_gap_open_) {
    return;
  }
  // A pass that spanned a submission split has its begin in a prior
  // submission's already-copied range; abandon it rather than corrupt this
  // submission's pair count (its begin query is simply never copied).
  if (pass_ts_open_submission_ != GetCurrentSubmission()) {
    pass_ts_open_pair_ = UINT32_MAX;
    ++pass_ts_dropped_;
    return;
  }
  deferred_command_buffer_->CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timestamp_pool_,
      pass_ts_open_pair_ * 2 + 1);
  pass_ts_draws_[pass_ts_open_pair_] = pass_open_draws_;
  pass_ts_scissor_[pass_ts_open_pair_] =
      (std::min(pass_open_scissor_w_, 0xFFFFu) << 16) |
      std::min(pass_open_scissor_h_, 0xFFFFu);
  pass_ts_viewport_[pass_ts_open_pair_] =
      (std::min(pass_open_viewport_w_, 0xFFFFu) << 16) |
      std::min(pass_open_viewport_h_, 0xFFFFu);
  ++pass_ts_count_;
  pass_ts_open_pair_ = UINT32_MAX;
  // Time until the next pass begins: compute, copies and barriers.
  DeferredCommandBuffer::dispatch_site_mask() = 0;
  OpenPassTimestamp(kPassTimestampGapKey);
  pass_ts_gap_open_ = pass_ts_open_pair_ != UINT32_MAX;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    // The same framebuffer means a compatible pass (its key only drops the
    // discard bits), and loadOp only matters at the begin.
    if (current_render_pass_ != VK_NULL_HANDLE &&
        current_framebuffer_ == framebuffer) {
      // Track the latest loadOp so a resume (ZPD) doesn't discard again.
      current_render_pass_ = render_pass;
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is. Also closes the fork's pass timestamp.
  EndRenderPass("rt_change");

  // Outside any pass now: load the textures resolves overwrote, so the draws
  // sampling them later don't end the pass we're about to begin.
  if (texture_cache_ && texture_cache_->HasEagerReloads()) {
    texture_cache_->FlushEagerResolvedReloads();
    SubmitBarriers(false);
  }

  if (cvars::log_render_pass_breaks > 0) {
    if (resolve_defer_census_.active && !resolve_defer_census_.recording) {
      if (framebuffer == resolve_defer_census_.framebuffer &&
          framebuffer == render_pass_last_ended_framebuffer_ &&
          resolve_defer_census_.broke) {
        resolve_defer_census_.reentered = true;
      } else {
        ResolveDeferFinish();
      }
    }
    if (framebuffer == render_pass_last_ended_framebuffer_) {
      ++render_pass_reentry_counts_[render_pass_last_break_key_];
    }
    render_pass_last_ended_framebuffer_ = nullptr;
  }
  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;
  ++vk_frame_sync_stats_.render_pass_begins;
  // Identify each pass bucket once by the guest render targets behind it.
  if (cvars::log_gpu_frame_time_breakdown) {
    const uint32_t bucket = (uint32_t(framebuffer->host_extent.width) << 16) |
                            uint32_t(framebuffer->host_extent.height);
    static std::set<uint32_t> logged_buckets;
    if (logged_buckets.emplace(bucket).second) {
      XELOGI("VkPassId: {}x{} <- {}", framebuffer->host_extent.width,
             framebuffer->host_extent.height,
             render_target_cache_->GetLastUpdateRenderTargetsDebugName());
    }
  }
  OpenPassTimestamp((uint32_t(framebuffer->host_extent.width) << 16) |
                    framebuffer->host_extent.height);

  if (use_dynamic_rendering) {
    // Use dynamic rendering - construct VkRenderingInfo from render targets.
    // The PSI ("accuracy") path emulates EDRAM via SSBOs and uses no real
    // attachments (mirrors the zero-attachment fsi_render_pass_).
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};
    uint32_t color_attachment_count = 0;

    bool has_depth = false;
    bool has_stencil = false;
    if (render_target_cache_->GetPath() ==
        RenderTargetCache::Path::kHostRenderTargets) {
      render_target_cache_->GetLastUpdateRenderingAttachments(
          color_attachments, &color_attachment_count, &depth_attachment,
          &stencil_attachment);
      has_depth =
          depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      has_stencil = stencil_attachment.sType ==
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments =
        color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        has_stencil ? &stencil_attachment : nullptr;

    RecordPassHeadSlot();
    deferred_command_buffer_->CmdVkBeginRendering(
        &rendering_info,
        render_target_cache_->GetPath() ==
                RenderTargetCache::Path::kHostRenderTargets
            ? render_target_cache_->GetLastUpdateMsrtssSampleCount()
            : 0);
    render_target_cache_->ConsumeLastUpdateLoadDontCare();
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    RecordPassHeadSlot();
    deferred_command_buffer_->CmdVkBeginRenderPass(
        &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE,
        cvars::vulkan_fold_clears_to_load_op);
    if (cvars::vulkan_fold_clears_to_load_op || cvars::vulkan_store_lookahead) {
      deferred_command_buffer_->SetClassicRenderPassAttachments(
          framebuffer->attachment_images, framebuffer->attachment_extents);
    }
  }
  in_render_pass_ = true;
  // First in the pass (before any query), so a full clear can fold into the
  // load op: the deferred clear and the attachments' pending constant rows.
  render_target_cache_->OnGuestRenderPassBegun();

  // Resume any pending ZPD segment now that the pass is open.
  OpenQuerySegment(false);
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth,
    uint32_t msrtss_samples, bool transfer_dest_is_integer) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass &&
        current_framebuffer_ == framebuffer) {
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is. Also closes the fork's pass timestamp.
  EndRenderPass("transfer_change");

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;
  ++vk_frame_sync_stats_.render_pass_begins;
  // Bit 31 tags EDRAM ownership-transfer passes.
  OpenPassTimestamp(0x80000000u |
                    (uint32_t(framebuffer->host_extent.width) << 16) |
                    framebuffer->host_extent.height);

  if (use_dynamic_rendering) {
    // Use dynamic rendering for transfers - construct VkRenderingInfo from
    // the transfer destination.
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = msrtss_samples
                                         ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                         : VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      // Stencil uses the same attachment.
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      // Must agree with the layout the usage barrier actually recorded, which
      // is RENDERING_LOCAL_READ when local-read attachments are enabled.
      color_attachment.imageLayout = render_target_cache_->color_draw_layout();
      color_attachment.resolveMode =
          !msrtss_samples ? VK_RESOLVE_MODE_NONE
                          : (transfer_dest_is_integer
                                 ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                 : VK_RESOLVE_MODE_AVERAGE_BIT);
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments =
        transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment =
        transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        transfer_dest_is_depth ? &stencil_attachment : nullptr;

    RecordPassHeadSlot();
    deferred_command_buffer_->CmdVkBeginRendering(&rendering_info,
                                                  msrtss_samples);
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    RecordPassHeadSlot();
    deferred_command_buffer_->CmdVkBeginRenderPass(
        &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE,
        cvars::vulkan_fold_clears_to_load_op);
    if (cvars::vulkan_fold_clears_to_load_op || cvars::vulkan_store_lookahead) {
      deferred_command_buffer_->SetClassicRenderPassAttachments(
          framebuffer->attachment_images, framebuffer->attachment_extents);
    }
  }
  in_render_pass_ = true;

  OpenQuerySegment(false);
}

VkPipelineStageFlags VulkanCommandProcessor::GetPassHeadSourceStages() const {
  if (!cvars::vulkan_pass_head_overlap) {
    return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  }
  // The scratch buffer is also an index buffer (vertex input).
  VkPipelineStageFlags stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                guest_shader_pipeline_stages_ |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (!FragmentSharedMemoryPending()) {
    stages &= ~VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  }
  return stages;
}

void VulkanCommandProcessor::RecordPassHeadSlot() {
  pass_head_slot_ = UINT32_MAX;
  if (cvars::vulkan_pass_head_shmem_uploads != AbFlipped(AbFeature::kPassHead)) {
    pass_head_slot_ =
        deferred_command_buffer_->CmdPassHeadSlot(GetPassHeadSourceStages());
    if (cvars::log_texture_clobber) {
      static uint32_t logged = 0;
      if (logged++ < 4096) {
        XELOGI("[POOL-H] s={} root={:X} anchor_after={} head={:X}",
               GetCurrentSubmission(),
               reinterpret_cast<uintptr_t>(deferred_command_buffer_),
               deferred_command_buffer_->size_elements(),
               reinterpret_cast<uintptr_t>(
                   &deferred_command_buffer_->GetPassHeadSlot(pass_head_slot_)));
      }
    }
    shared_memory_->OnRenderPassBegin();
    if (texture_cache_) {
      texture_cache_->OnRenderPassBegin(++render_pass_serial_);
    }
    // The repeat fast path skips shared memory requests of a draw memoized
    // before this pass began - they wouldn't be recorded as used in it.
    ++draw_state_epoch_;
  }
}

void VulkanCommandProcessor::EndRenderPass(const char* byte_reason) {
  assert_true(submission_open_);
  if (!in_render_pass_) {
    render_pass_break_caller_ = nullptr;
    return;
  }
  if (resolve_defer_census_.active) {
    if (resolve_defer_census_.recording) {
      resolve_defer_census_.broke = true;
    } else {
      // A real pass boundary already exists here: a delayed resolve would
      // have to execute before the operation that caused this boundary.
      ResolveDeferFinish();
    }
  }
  if (pass_bytes_active_) {
    PassByteLedger::Event close;
    close.kind = PassByteLedger::kClose;
    close.reason = pass_bytes_resolve_scope_ ? "resolve" : byte_reason;
    Dl_info info = {};
    uintptr_t base = dladdr(reinterpret_cast<void*>(&cvars::log_pass_bytes), &info)
                         ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0;
    close.direct = (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - base) & 0xFFFFFFFFull;
    close.outer = render_pass_break_caller_ ?
        (reinterpret_cast<uintptr_t>(render_pass_break_caller_) - base) & 0xFFFFFFFFull : 0;
    close.pusher = !pending_barriers_.empty() && first_barrier_pusher_ ?
        (reinterpret_cast<uintptr_t>(first_barrier_pusher_) - base) & 0xFFFFFFFFull : 0;
    deferred_command_buffer_->AnnotatePassBytes(close);
  }
  if (cvars::log_render_pass_breaks > 0) {
    // Key: the caller of SubmitBarriers when it ended the pass, plus the
    // direct caller of EndRenderPass.
    uint64_t key =
        (uint64_t(reinterpret_cast<uintptr_t>(__builtin_return_address(0))) &
         0xFFFFFFFFull) |
        (uint64_t(reinterpret_cast<uintptr_t>(render_pass_break_caller_))
         << 32);
    ++render_pass_break_counts_[key];
    render_pass_break_caller_ = nullptr;
    render_pass_last_ended_framebuffer_ = current_framebuffer_;
    render_pass_last_break_key_ = key;
  }
  render_pass_break_caller_ = nullptr;
  // Close native Vulkan occlusion queries before ending the pass. FSI counter
  // segments don't use vkCmdBeginQuery / vkCmdEndQuery and can stay logically
  // open across render passes.
  if (GetZPDMode() != ZPDMode::kFake && zpd_active_segment_.segment_active &&
      !zpd_active_query_is_fsi_) {
    CloseQuerySegment();
    if (zpd_active_segment_.logical_active) {
      zpd_active_segment_.segment_pending_begin = true;
    }
  }
  // Shrink the render area to what the pass's draws actually touched, now that
  // they have all been recorded. A host render target spans the whole EDRAM
  // range for its pitch (a 2-tile-wide one is 8192 rows tall), and on a tiler
  // the render area drives tile binning and GMEM load/store - not just
  // clipping - so the unshrunk area costs far more than the draws do.
  // Shrink first: a clear folded into loadOp CLEAR below must match the final
  // render area (a lazy clear is clipped to it), not the full extent.
  if (cvars::render_area_dirty_extent || cvars::vulkan_render_area_shrink) {
    // Rounded up to a coarse bin-friendly quantum rather than the exact
    // vkGetRenderAreaGranularity: a non-aligned render area is legal, the
    // alignment only helps the driver's binning, and the win here is 8192 rows
    // becoming ~135 - the rounding is noise against that.
    constexpr uint32_t kRenderAreaAlign = 32;
    deferred_command_buffer_->ShrinkRenderAreaToDrawn(kRenderAreaAlign,
                                                     kRenderAreaAlign);
  }
  // vulkan_lazy_constant_rows: trim the pass's lazy clears to its final area;
  // the cache keeps what's outside as pending constant rows.
  {
    DeferredCommandBuffer::LazyClearResult
        lazy_results[DeferredCommandBuffer::kMaxLazyClears];
    const uint32_t lazy_count =
        deferred_command_buffer_->ClipLazyClears(lazy_results);
    if (lazy_count) {
      render_target_cache_->OnLazyClearsClipped(lazy_results, lazy_count);
    }
  }
  if (cvars::vulkan_fold_clears_to_load_op) {
    deferred_command_buffer_->FoldClassicClears(*render_target_cache_);
  }
  if (cvars::vulkan_classic_pass_finalize) {
    relaxed_attachment_ops_total_ +=
        deferred_command_buffer_->FinalizeClassicRenderPass(
            *render_target_cache_,
            GetVulkanDevice()->extensions().ext_EXT_load_store_op_none ||
                GetVulkanDevice()->extensions().ext_KHR_load_store_op_none);
  }
  if (cvars::vulkan_relax_unused_attachment_ops) {
    const uint32_t relaxed = deferred_command_buffer_->RelaxUnusedAttachmentOps(
        cvars::vulkan_relax_unused_attachment_loads &&
        (GetVulkanDevice()->extensions().ext_EXT_load_store_op_none ||
         GetVulkanDevice()->extensions().ext_KHR_load_store_op_none));
    relaxed_attachment_ops_total_ += relaxed;
    ++relaxed_attachment_ops_passes_;
    if (cvars::log_relaxed_attachment_ops &&
        !(relaxed_attachment_ops_passes_ & 0x3FFF)) {
      XELOGI("VkAttachmentOps: {} load/store operations skipped over {} passes",
             relaxed_attachment_ops_total_, relaxed_attachment_ops_passes_);
    }
  }
  // Done with the pass's render area: shrinking and the op rewrites above.
  deferred_command_buffer_->EndRenderAreaTracking();
  // Use current_render_pass_ to determine which end command to use.
  // VK_NULL_HANDLE means we used dynamic rendering, otherwise traditional.
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_->CmdVkEndRendering();
  } else {
    deferred_command_buffer_->CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
  pass_head_slot_ = UINT32_MAX;
  ClosePassTimestamp();
  if (deferred_resolve_pending_) {
    // All capture resources belong to this submission. Append before any
    // streaming publish, caller barrier, resource reuse or submission close.
    deferred_command_buffer_->Append(*deferred_resolve_commands_);
    current_external_compute_pipeline_ =
        deferred_resolve_final_compute_pipeline_;
    if (deferred_resolve_scratch_pinned_) scratch_buffer_used_ = false;
    deferred_resolve_scratch_pinned_ = false;
    deferred_resolve_pending_ = false;
    render_target_cache_->ReleaseDeferredResolveSources();
    ++deferred_resolve_executed_;
    deferred_resolve_with_draws_ += uint32_t(deferred_resolve_has_draws_);
    deferred_resolve_has_draws_ = false;
    ++deferred_resolve_stops_[size_t(deferred_resolve_stop_)];
  }
  if (stream_encode_active_) {
    // The pass is final now - its render area and pass-head slot included.
    StreamEncodePublish();
  }
}

bool VulkanCommandProcessor::IsRenderTargetFramebufferInUse(
    const VulkanRenderTargetCache::Framebuffer* framebuffer) const {
  return framebuffer && current_framebuffer_ == framebuffer;
}

void VulkanCommandProcessor::ForgetRenderTargetFramebuffer(
    const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  assert_false(framebuffer && current_framebuffer_ == framebuffer);
  if (resolve_defer_census_.active &&
      resolve_defer_census_.framebuffer == framebuffer) {
    resolve_defer_census_.reasons |= kResolveDeferUnknown;
    ResolveDeferFinish();
  }
  if (render_pass_last_ended_framebuffer_ == framebuffer) {
    render_pass_last_ended_framebuffer_ = nullptr;
  }
}

void VulkanCommandProcessor::InvalidateCachedRenderTargetDescriptorSet(
    VkDescriptorSet descriptor_set) {
  if (descriptor_set == VK_NULL_HANDLE) {
    return;
  }
  for (uint32_t i = 0;
       i < uint32_t(SpirvShaderTranslator::kDescriptorSetCount); ++i) {
    if (current_graphics_descriptor_sets_[i] != descriptor_set) {
      continue;
    }
    current_graphics_descriptor_sets_[i] = VK_NULL_HANDLE;
    const uint32_t bit = uint32_t(1) << i;
    current_graphics_descriptor_set_values_up_to_date_ &= ~bit;
    current_graphics_descriptor_sets_bound_up_to_date_ &= ~bit;
    if (i == SpirvShaderTranslator::kDescriptorSetTexturesVertex) {
      current_texture_descriptors_valid_vertex_ = false;
    } else if (i == SpirvShaderTranslator::kDescriptorSetTexturesPixel) {
      current_texture_descriptors_valid_pixel_ = false;
    }
  }
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    bool is_storage_buffer =
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageBufferFragment;
    bool is_storage_image =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageImageFragment ||
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageImageCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator&
        transient_descriptor_allocator =
            is_storage_image
                ? transient_descriptor_allocator_storage_image_
                : (is_storage_buffer
                       ? transient_descriptor_allocator_storage_buffer_
                       : transient_descriptor_allocator_uniform_buffer_);
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type =
        is_storage_image
            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (is_storage_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    descriptor_count.descriptorCount = 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout),
        &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

namespace {
template <typename Handle>
uint64_t BindlessHandleKey(Handle handle) {
  uint64_t key = 0;
  std::memcpy(&key, &handle, sizeof(handle));
  return key;
}
}  // namespace

bool VulkanCommandProcessor::InitializeBindless() {
  if (!SpirvShaderTranslator::Features(GetVulkanDevice()).bindless_textures) {
    return true;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  using Translator = SpirvShaderTranslator;
  VkDescriptorSetLayoutBinding bindings[Translator::kBindlessBindingCount];
  VkDescriptorBindingFlags binding_flags[Translator::kBindlessBindingCount];
  for (uint32_t i = 0; i < Translator::kBindlessBindingCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i == Translator::kBindlessBindingSamplers
                                     ? VK_DESCRIPTOR_TYPE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount =
        Translator::kBindlessSlotCounts[i] + kBindlessTablePadding;
    bindings[i].stageFlags = guest_shader_vertex_stages_ |
                             VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].pImmutableSamplers = nullptr;
    binding_flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
  }
  VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  binding_flags_info.bindingCount = Translator::kBindlessBindingCount;
  binding_flags_info.pBindingFlags = binding_flags;
  VkDescriptorSetLayoutCreateInfo layout_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.pNext = &binding_flags_info;
  layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layout_info.bindingCount = Translator::kBindlessBindingCount;
  layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
                                      &bindless_set_layout_) != VK_SUCCESS) {
    bindless_set_layout_ = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorPoolSize pool_sizes[2];
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  pool_sizes[0].descriptorCount =
      Translator::kBindlessSlotCounts[Translator::kBindlessBindingImages2DArray] +
      Translator::kBindlessSlotCounts[Translator::kBindlessBindingImages3D] +
      Translator::kBindlessSlotCounts[Translator::kBindlessBindingImagesCube] +
      3 * kBindlessTablePadding;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  pool_sizes[1].descriptorCount =
      Translator::kBindlessSlotCounts[Translator::kBindlessBindingSamplers] +
      kBindlessTablePadding;
  VkDescriptorPoolCreateInfo pool_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  // The bindless set and an empty one bound as the pixel textures set.
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr,
                                 &bindless_pool_) != VK_SUCCESS) {
    bindless_pool_ = VK_NULL_HANDLE;
    ShutdownBindless();
    return false;
  }
  VkDescriptorSetAllocateInfo allocate_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate_info.descriptorPool = bindless_pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &bindless_set_layout_;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &bindless_set_) !=
      VK_SUCCESS) {
    bindless_set_ = VK_NULL_HANDLE;
    ShutdownBindless();
    return false;
  }
  allocate_info.pSetLayouts = &descriptor_set_layout_empty_;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info,
                                   &bindless_empty_set_) != VK_SUCCESS) {
    bindless_empty_set_ = VK_NULL_HANDLE;
    bindless_set_ = VK_NULL_HANDLE;
    ShutdownBindless();
    return false;
  }
  XELOGI("Vulkan: bindless guest textures ({} 2D, {} 3D, {} cube, {} samplers)",
         Translator::kBindlessSlotCounts[0], Translator::kBindlessSlotCounts[1],
         Translator::kBindlessSlotCounts[2], Translator::kBindlessSlotCounts[3]);
  return true;
}

void VulkanCommandProcessor::ShutdownBindless() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // The sets are freed with their pool.
  bindless_set_ = VK_NULL_HANDLE;
  bindless_empty_set_ = VK_NULL_HANDLE;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device,
                                         bindless_null_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         bindless_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device, bindless_set_layout_);
  for (BindlessArray& array : bindless_arrays_) {
    array = BindlessArray();
  }
  bindless_retired_.clear();
}

bool VulkanCommandProcessor::FillBindlessWithNull() {
  using Translator = SpirvShaderTranslator;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr,
                          &bindless_null_sampler_) != VK_SUCCESS) {
    bindless_null_sampler_ = VK_NULL_HANDLE;
    return false;
  }
  const VkImageView null_views[] = {
      texture_cache_->GetNullImageView(xenos::FetchOpDimension::k2D),
      texture_cache_->GetNullImageView(xenos::FetchOpDimension::k3DOrStacked),
      texture_cache_->GetNullImageView(xenos::FetchOpDimension::kCube),
  };
  static_assert(Translator::kBindlessBindingImages2DArray == 0 &&
                Translator::kBindlessBindingImages3D == 1 &&
                Translator::kBindlessBindingImagesCube == 2 &&
                Translator::kBindlessBindingSamplers == 3);
  std::vector<VkDescriptorImageInfo> image_infos;
  for (uint32_t array = 0; array < Translator::kBindlessBindingCount;
       ++array) {
    const bool is_sampler = array == Translator::kBindlessBindingSamplers;
    VkDescriptorImageInfo image_info;
    image_info.sampler = is_sampler ? bindless_null_sampler_ : VK_NULL_HANDLE;
    image_info.imageView = is_sampler ? VK_NULL_HANDLE : null_views[array];
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!is_sampler && image_info.imageView == VK_NULL_HANDLE) {
      return false;
    }
    image_infos.assign(
        Translator::kBindlessSlotCounts[array] + kBindlessTablePadding,
        image_info);
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_set_;
    write.dstBinding = array;
    write.dstArrayElement = 0;
    write.descriptorCount = uint32_t(image_infos.size());
    write.descriptorType = is_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER
                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = image_infos.data();
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }
  return true;
}

void VulkanCommandProcessor::WriteBindlessSlot(uint32_t array, uint32_t slot,
                                               VkImageView image_view,
                                               VkSampler sampler) {
  const bool is_sampler =
      array == SpirvShaderTranslator::kBindlessBindingSamplers;
  VkDescriptorImageInfo image_info;
  image_info.sampler = sampler;
  image_info.imageView = image_view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = bindless_set_;
  write.dstBinding = array;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = is_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER
                                    : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  // In stream order, on the thread that records the binds and draws (the
  // stream encoder), like the per-draw sets: the set is never written while
  // another thread records a bind of it.
  if (cvars::vulkan_bindless_write_idle) {
    if (GetCurrentSubmission() > 1) {
      CheckSubmissionCompletionAndDeviceLoss(GetCurrentSubmission() - 1);
    }
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    vulkan_device->functions().vkUpdateDescriptorSets(
        vulkan_device->device(), 1, &write, 0, nullptr);
  } else if (draw_config_.vulkan_defer_descriptor_updates) {
    deferred_command_buffer_->CmdUpdateDescriptorSets(1, &write);
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    vulkan_device->functions().vkUpdateDescriptorSets(
        vulkan_device->device(), 1, &write, 0, nullptr);
  }
}

uint32_t VulkanCommandProcessor::GetBindlessSlot(
    SpirvShaderTranslator::BindlessBinding array, VkImageView image_view,
    VkSampler sampler) {
  const bool is_sampler =
      array == SpirvShaderTranslator::kBindlessBindingSamplers;
  const uint64_t key = is_sampler ? BindlessHandleKey(sampler)
                                  : BindlessHandleKey(image_view);
  if (!key) {
    // An overflowed sampler: any written slot is better than an unwritten one.
    return 0;
  }
  BindlessArray& bindless_array = bindless_arrays_[array];
  auto it = bindless_array.slots.find(key);
  if (it != bindless_array.slots.end()) {
    return it->second;
  }
  uint32_t slot;
  if (bindless_array.next_slot <
      SpirvShaderTranslator::kBindlessSlotCounts[array]) {
    slot = bindless_array.next_slot++;
  } else if (!bindless_array.free_slots.empty()) {
    slot = bindless_array.free_slots.front();
    bindless_array.free_slots.pop_front();
  } else {
    if (bindless_overflow_logs_ < 16) {
      ++bindless_overflow_logs_;
      XELOGE("Vulkan: bindless array {} is full ({} slots)", uint32_t(array),
             SpirvShaderTranslator::kBindlessSlotCounts[array]);
    }
    return 0;
  }
  WriteBindlessSlot(uint32_t(array), slot, image_view, sampler);
  bindless_slot_written_ = true;
  bindless_array.slots.emplace(key, slot);
  return slot;
}

void VulkanCommandProcessor::ReleaseBindlessImageView(VkImageView image_view) {
  if (bindless_set_ == VK_NULL_HANDLE) {
    return;
  }
  const uint64_t key = BindlessHandleKey(image_view);
  for (uint32_t array = 0;
       array < SpirvShaderTranslator::kBindlessBindingSamplers; ++array) {
    auto it = bindless_arrays_[array].slots.find(key);
    if (it != bindless_arrays_[array].slots.end()) {
      // The view and its image memory go away now: the slot must not keep
      // their address (a GPU read of it is a translation fault).
      if (texture_cache_) {
        WriteBindlessSlot(
            array, it->second,
            texture_cache_->GetNullImageView(
                array == SpirvShaderTranslator::kBindlessBindingImages3D
                    ? xenos::FetchOpDimension::k3DOrStacked
                    : array == SpirvShaderTranslator::kBindlessBindingImagesCube
                          ? xenos::FetchOpDimension::kCube
                          : xenos::FetchOpDimension::k2D),
            VK_NULL_HANDLE);
        bindless_slot_written_ = true;
      }
      bindless_retired_.push_back(
          {GetCurrentSubmission(), array, it->second});
      bindless_arrays_[array].slots.erase(it);
    }
  }
}

void VulkanCommandProcessor::ReleaseBindlessSampler(VkSampler sampler) {
  if (bindless_set_ == VK_NULL_HANDLE) {
    return;
  }
  BindlessArray& array =
      bindless_arrays_[SpirvShaderTranslator::kBindlessBindingSamplers];
  auto it = array.slots.find(BindlessHandleKey(sampler));
  if (it != array.slots.end()) {
    if (texture_cache_ && bindless_null_sampler_ != VK_NULL_HANDLE) {
      WriteBindlessSlot(SpirvShaderTranslator::kBindlessBindingSamplers,
                        it->second, VK_NULL_HANDLE, bindless_null_sampler_);
      bindless_slot_written_ = true;
    }
    bindless_retired_.push_back(
        {GetCurrentSubmission(),
         uint32_t(SpirvShaderTranslator::kBindlessBindingSamplers),
         it->second});
    array.slots.erase(it);
  }
}

void VulkanCommandProcessor::UpdateBindlessSlots(
    const VulkanShader* vertex_shader, const VulkanShader* pixel_shader,
    bool vertex_ready, bool pixel_ready) {
  using Translator = SpirvShaderTranslator;
  constexpr uint32_t kTextureWords =
      Translator::kBindlessMaxTextureBindingsPerStage / 2;
  constexpr uint32_t kSamplerWords =
      Translator::kBindlessMaxSamplerBindingsPerStage / 2;
  uint32_t texture_slots[2][kTextureWords] = {};
  uint32_t sampler_slots[2][kSamplerWords] = {};
  const VulkanShader* shaders[2] = {vertex_shader, pixel_shader};
  const bool ready[2] = {vertex_ready, pixel_ready};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (!shaders[stage] || !ready[stage]) {
      continue;
    }
    const std::vector<VulkanShader::TextureBinding>& textures =
        shaders[stage]->GetTextureBindingsAfterTranslation();
    uint32_t texture_count =
        std::min(uint32_t(textures.size()),
                 Translator::kBindlessMaxTextureBindingsPerStage);
    for (uint32_t i = 0; i < texture_count; ++i) {
      const VulkanShader::TextureBinding& binding = textures[i];
      Translator::BindlessBinding array;
      switch (binding.dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          array = Translator::kBindlessBindingImages3D;
          break;
        case xenos::FetchOpDimension::kCube:
          array = Translator::kBindlessBindingImagesCube;
          break;
        default:
          array = Translator::kBindlessBindingImages2DArray;
          break;
      }
      uint32_t slot =
          (cvars::vulkan_bindless_debug_null & (stage ? 2 : 1))
              ? 0
              : GetBindlessSlot(
                    array,
                    texture_cache_->GetActiveBindingOrNullImageView(
                        binding.fetch_constant, binding.dimension,
                        bool(binding.is_signed)),
                    VK_NULL_HANDLE);
      texture_slots[stage][i >> 1] |= slot << ((i & 1) * 16);
    }
    const std::vector<std::pair<VulkanTextureCache::SamplerParameters,
                                VkSampler>>& samplers =
        stage ? current_samplers_pixel_ : current_samplers_vertex_;
    uint32_t sampler_count =
        std::min(uint32_t(samplers.size()),
                 Translator::kBindlessMaxSamplerBindingsPerStage);
    for (uint32_t i = 0; i < sampler_count; ++i) {
      uint32_t slot = (cvars::vulkan_bindless_debug_null & 4)
                          ? 0
                          : GetBindlessSlot(Translator::kBindlessBindingSamplers,
                                            VK_NULL_HANDLE, samplers[i].second);
      sampler_slots[stage][i >> 1] |= slot << ((i & 1) * 16);
    }
  }
  Translator::SystemConstants& constants = system_constants_;
  uint32_t* targets[4] = {
      constants.texture_slots_vertex, constants.texture_slots_pixel,
      constants.sampler_slots_vertex, constants.sampler_slots_pixel};
  const uint32_t* sources[4] = {texture_slots[0], texture_slots[1],
                                sampler_slots[0], sampler_slots[1]};
  const size_t sizes[4] = {
      sizeof(uint32_t) * kTextureWords, sizeof(uint32_t) * kTextureWords,
      sizeof(uint32_t) * kSamplerWords, sizeof(uint32_t) * kSamplerWords};
  bool changed = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (std::memcmp(targets[i], sources[i], sizes[i])) {
      std::memcpy(targets[i], sources[i], sizes[i]);
      changed = true;
    }
  }
  if (changed) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << Translator::kConstantBufferSystem);
  }
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(
    bool is_vertex, size_t texture_count, size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing =
      descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanCommandProcessor::GetPipelineLayout(size_t texture_count_pixel,
                                          size_t sampler_count_pixel,
                                          size_t texture_count_vertex,
                                          size_t sampler_count_vertex) {
  if (bindless_set_layout_ != VK_NULL_HANDLE) {
    // One layout for every guest pipeline: the textures are in the global set.
    texture_count_pixel = 0;
    sampler_count_pixel = 0;
    texture_count_vertex = 0;
    sampler_count_vertex = 0;
  }
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  // Called from the draw thread and from pipeline creation threads (deferred
  // translation), and reads/writes the shared layout maps +
  // GetTextureDescriptor SetLayout (only called from here), so serialize the
  // whole lookup+create.
  std::lock_guard<std::mutex> layouts_lock(pipeline_layouts_mutex_);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex,
                                    sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel,
                                    sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout
      descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts
      [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
          descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  if (bindless_set_layout_ != VK_NULL_HANDLE) {
    descriptor_set_layout_textures_vertex = bindless_set_layout_;
  }
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount =
      uint32_t(xe::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &pipeline_layout) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(
          pipeline_layout, descriptor_set_layout_textures_vertex,
          descriptor_set_layout_textures_pixel, uint32_t(texture_count_vertex),
          uint32_t(sampler_count_vertex), uint32_t(texture_count_pixel),
          uint32_t(sampler_count_pixel)));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition
VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
    VkAccessFlags initial_access_mask, bool push_barrier) {
  FlushDeferredResolve(ResolveDeferStop::kResource);
  if (deferred_resolve_recording_) deferred_resolve_scratch_pinned_ = true;
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    if (push_barrier) {
      PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE,
                              scratch_buffer_last_stage_mask_,
                              initial_stage_mask,
                              scratch_buffer_last_access_mask_,
                              initial_access_mask);
    }
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // Used for texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    XELOGE(
        "VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer",
        size >> 20);
    return ScratchBufferAcquisition();
  }

  if (GetCompletedSubmission() >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_,
                                    scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_,
                                   scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_stage_mask;
  scratch_buffer_last_access_mask_ = initial_access_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(
    VkPipeline pipeline, bool keep_dynamic_depth_bias,
    bool keep_dynamic_blend_constants, bool keep_dynamic_stencil_mask_ref) {
  FlushDeferredResolve(ResolveDeferStop::kResource);
  draw_dynamic_state_dirty_ = true;
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  // External (transfer/resolve) pipelines bake all extended dynamic state
  // statically, invalidating the guest draw's dynamic EDS state. Re-dirty every
  // EDS flag so the next guest draw re-emits it (UpdateDynamicState only emits
  // when the matching capability is active, so this is inert when EDS is off).
  dynamic_cull_mode_update_needed_ = true;
  dynamic_front_face_update_needed_ = true;
  dynamic_primitive_topology_update_needed_ = true;
  dynamic_primitive_restart_enable_update_needed_ = true;
  dynamic_depth_test_enable_update_needed_ = true;
  dynamic_depth_write_enable_update_needed_ = true;
  dynamic_depth_compare_op_update_needed_ = true;
  dynamic_stencil_test_enable_update_needed_ = true;
  dynamic_stencil_op_front_update_needed_ = true;
  dynamic_stencil_op_back_update_needed_ = true;
  dynamic_depth_clamp_enable_update_needed_ = true;
  dynamic_polygon_mode_update_needed_ = true;
  dynamic_fragment_shading_rate_update_needed_ = true;
  dynamic_color_blend_enable_update_needed_ = true;
  dynamic_color_blend_equation_update_needed_ = true;
  dynamic_color_write_mask_update_needed_ = true;
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = nullptr;
  current_guest_graphics_pipeline_handle_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

VkPipeline VulkanCommandProcessor::GetMemExportComputePipeline(
    VulkanShader& vertex_shader,
    SpirvShaderTranslator::Modification vertex_shader_modification,
    VkPipelineLayout pipeline_layout, bool batched) {
  SpirvShaderTranslator::Modification modification = vertex_shader_modification;
  modification.vertex.host_vertex_shader_type =
      Shader::HostVertexShaderType::kMemExportCompute;
  modification.vertex.interpolator_mask = 0;
  modification.vertex.output_point_parameters = 0;
  modification.vertex.user_clip_plane_count = 0;
  // Meaning "batched" for kMemExportCompute (no clip planes in compute).
  modification.vertex.user_clip_plane_cull = batched ? 1 : 0;
  modification.vertex.vertex_kill_and = 0;
  modification.vertex.tessellation_mode = xenos::TessellationMode(0);
  auto translation = static_cast<VulkanShader::VulkanTranslation*>(
      vertex_shader.GetOrCreateTranslation(modification.value));
  MemExportComputePipelineKey key{translation, pipeline_layout};
  auto it = memexport_compute_pipelines_.find(key);
  if (it != memexport_compute_pipelines_.end()) {
    return it->second;
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (pipeline_cache_->EnsureShadersTranslated(translation, nullptr)) {
    VkShaderModule shader_module = translation->GetOrCreateShaderModule();
    if (shader_module != VK_NULL_HANDLE) {
      VkComputePipelineCreateInfo pipeline_create_info = {};
      pipeline_create_info.sType =
          VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipeline_create_info.stage.sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipeline_create_info.stage.module = shader_module;
      pipeline_create_info.stage.pName = "main";
      pipeline_create_info.layout = pipeline_layout;
      const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
      if (vulkan_device->functions().vkCreateComputePipelines(
              vulkan_device->device(), VK_NULL_HANDLE, 1, &pipeline_create_info,
              nullptr, &pipeline) != VK_SUCCESS) {
        pipeline = VK_NULL_HANDLE;
      }
    }
  }
  XELOGI("Memexport compute: VS {:016X} modification {:016X} -> {}",
         vertex_shader.ucode_data_hash(), modification.value,
         pipeline != VK_NULL_HANDLE ? "created" : "FAILED");
  memexport_compute_pipelines_.emplace(key, pipeline);
  return pipeline;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  FlushDeferredResolve(ResolveDeferStop::kResource);
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline);
  current_external_compute_pipeline_ = pipeline;
}

uint32_t VulkanCommandProcessor::BeginTimedSection(
    DeferredCommandBuffer& command_buffer, uint8_t kind,
    uint32_t texture_format) {
  if (kind == 3 && texture_format < 64) {
    ++vk_frame_sync_stats_.resolve_texture[texture_format].reloaded;
  }
  if (!resolve_timestamp_mapping_) {
    return UINT32_MAX;
  }
  if (resolve_ts_submission_ != GetCurrentSubmission()) {
    resolve_ts_submission_ = GetCurrentSubmission();
    resolve_ts_count_ = 0;
  }
  if (resolve_ts_count_ >= kResolveTimestampPairsPerSubmission) {
    ++vk_frame_sync_stats_.resolve_ts_dropped;
    return UINT32_MAX;
  }
  uint32_t pair =
      uint32_t(resolve_ts_submission_ % kResolveTimestampRingSubmissions) *
          kResolveTimestampPairsPerSubmission +
      resolve_ts_count_++;
  resolve_ts_kind_[pair] = kind;
  resolve_ts_texture_format_[pair] = uint8_t(texture_format);
  command_buffer.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     resolve_timestamp_pool_, pair * 2);
  return pair;
}

void VulkanCommandProcessor::EndTimedSection(
    DeferredCommandBuffer& command_buffer, uint32_t pair) {
  if (pair == UINT32_MAX) {
    return;
  }
  command_buffer.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     resolve_timestamp_pool_, pair * 2 + 1);
}

void VulkanCommandProcessor::BindExternalComputePipelineInto(
    DeferredCommandBuffer& command_buffer, VkPipeline pipeline) {
  if (&command_buffer == deferred_command_buffer_) {
    BindExternalComputePipeline(pipeline);
    return;
  }
  // Always bound: the cached pipeline may have been bound in the main stream
  // after this command buffer's replay point.
  command_buffer.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    draw_dynamic_state_dirty_ = true;
    dynamic_viewport_ = viewport;
    deferred_command_buffer_->CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    draw_dynamic_state_dirty_ = true;
    dynamic_scissor_ = scissor;
    deferred_command_buffer_->CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           const uint32_t* host_address,
                                           uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

void VulkanCommandProcessor::RtTraceFlushRun() {
  if (!rt_trace_run_open_) {
    return;
  }
  rt_trace_run_open_ = false;
  ++rt_trace_run_count_;
  if (rt_trace_run_count_ > 4000) {
    if (rt_trace_run_count_ == 4001) {
      XELOGI("RTTRACE truncated");
    }
    return;
  }
  const RtTraceSignature& sig = rt_trace_run_sig_;
  XELOGI(
      "RTTRACE run first_draw={} count={} rts=[d:{} c0:{} c1:{} c2:{} c3:{}] "
      "scissor=[{},{},{},{}] woff=[{},{},{}] vp=[{},{},{},{},{}] "
      "cmask=0x{:X} zctl=0x{:X} rast={} prim={}x{} pnull={}",
      rt_trace_run_first_draw_, rt_trace_run_draws_,
      VulkanRenderTargetCache::FormatRtTraceKey(sig.rts[0], true),
      VulkanRenderTargetCache::FormatRtTraceKey(sig.rts[1], true),
      VulkanRenderTargetCache::FormatRtTraceKey(sig.rts[2], true),
      VulkanRenderTargetCache::FormatRtTraceKey(sig.rts[3], true),
      VulkanRenderTargetCache::FormatRtTraceKey(sig.rts[4], true),
      sig.scissor_tl_x, sig.scissor_tl_y, sig.scissor_br_x, sig.scissor_br_y,
      sig.woff_x, sig.woff_y, uint32_t(sig.woff_en), sig.vp_xscale,
      sig.vp_xoffset, sig.vp_yscale, sig.vp_yoffset, sig.vte_raw, sig.cmask,
      sig.zctl, uint32_t(sig.rast), uint32_t(sig.prim_type),
      rt_trace_run_prim_count_, uint32_t(sig.pnull));
}

void VulkanCommandProcessor::RtTraceRecordDraw(
    xenos::PrimitiveType prim_type, uint32_t index_count, bool pipeline_null,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool is_rasterization_done) {
  const RegisterFile& regs = *register_file_;
  RtTraceSignature sig;
  render_target_cache_->GetRtTraceAccumulatedRenderTargets(sig.rts);
  auto scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  sig.scissor_tl_x = scissor_tl.tl_x;
  sig.scissor_tl_y = scissor_tl.tl_y;
  sig.scissor_br_x = scissor_br.br_x;
  sig.scissor_br_y = scissor_br.br_y;
  auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  sig.woff_x = window_offset.window_x_offset;
  sig.woff_y = window_offset.window_y_offset;
  sig.woff_en =
      regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable != 0;
  sig.vp_xscale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  sig.vp_xoffset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  sig.vp_yscale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  sig.vp_yoffset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  sig.vte_raw = regs.Get<reg::PA_CL_VTE_CNTL>().value;
  sig.cmask = normalized_color_mask;
  sig.zctl = normalized_depth_control.value;
  sig.rast = is_rasterization_done;
  sig.prim_type = prim_type;
  sig.pnull = pipeline_null;

  ++rt_trace_draw_count_;
  if (!rt_trace_run_open_ || sig != rt_trace_run_sig_) {
    RtTraceFlushRun();
    rt_trace_run_open_ = true;
    rt_trace_run_sig_ = sig;
    rt_trace_run_first_draw_ = rt_trace_draw_count_;
    rt_trace_run_draws_ = 0;
    rt_trace_run_prim_count_ = index_count;
  }
  ++rt_trace_run_draws_;
}

bool VulkanCommandProcessor::RewriteMsaa4xDepthSurfaceInfoForDraw(
    VulkanShader* pixel_shader) {
  if (render_target_cache_->GetPath() !=
      RenderTargetCache::Path::kHostRenderTargets) {
    return false;
  }

  const RegisterFile& regs = *register_file_;
  reg::RB_SURFACE_INFO surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface_info.msaa_samples != xenos::MsaaSamples::k4X) {
    return false;
  }
  ++msaa_4x_as_1x_stats_seen_;
  const uint32_t guest_surface_pitch = uint32_t(surface_info.surface_pitch);

  if (draw_config_.vulkan_msaa_4x_as_2x2) {
    // Every compatible 4x draw, color and depth, with or without a pixel
    // shader. The 1x key at double pitch covers the same EDRAM bytes as the 4x
    // one, so a surface reached by both kinds of draws stays exact: the two
    // keys trade ownership through the regular transfers.
    // Alpha to coverage of a 2x2 draw is tested per host pixel (sample) in
    // the pixel shader.
    if (!cvars::vulkan_msaa_2x2_alpha_to_coverage &&
        regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable &&
        !draw_config_.vulkan_debug_disable_alpha_to_mask) {
      ++msaa_4x_as_1x_stats_reject_incompatible_;
      return false;
    }
    if (render_target_cache_->IsDrawResolutionScaled()) {
      ++msaa_4x_as_1x_stats_reject_scaled_;
      return false;
    }
    if (guest_surface_pitch * 2 > xenos::kTexture2DCubeMaxWidthHeight) {
      ++msaa_4x_as_1x_stats_reject_pitch_;
      return false;
    }
    ++msaa_4x_as_1x_stats_eligible_;
    surface_info.msaa_samples = xenos::MsaaSamples::k1X;
    surface_info.surface_pitch = guest_surface_pitch * 2;
    register_file_->Store(XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);
    render_target_cache_->SetDrawSamplesAsPixels(true);
    ++msaa_4x_as_1x_stats_rewritten_;
    return true;
  }

  const uint64_t depth_surface_key =
      (uint64_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base) << 32) |
      uint64_t(guest_surface_pitch);
  const bool was_samples_as_pixels =
      msaa_4x_samples_as_pixels_depth_surfaces_.count(depth_surface_key) != 0;
  const reg::RB_DEPTHCONTROL depth_control =
      regs.Get<reg::RB_DEPTHCONTROL>();
  const reg::RB_COLORCONTROL color_control =
      regs.Get<reg::RB_COLORCONTROL>();
  const bool depth_only_mode =
      regs.Get<reg::RB_MODECONTROL>().edram_mode ==
      xenos::EdramMode::kDepthOnly;

  // A surface used by an incompatible 4x draw must remain in its native sample
  // representation. Exclude color+depth mode even if its pixel shader was
  // optimized away: this path is specifically for explicit depth-only draws.
  if (pixel_shader || !depth_only_mode || depth_control.stencil_enable ||
      color_control.alpha_to_mask_enable) {
    ++msaa_4x_as_1x_stats_reject_incompatible_;
    if (msaa_4x_unsafe_depth_surfaces_.insert(depth_surface_key).second &&
        was_samples_as_pixels) {
      XELOGW(
          "Depth4xAs1x: surface base={} pitch={} became a regular 4x scene "
          "after a no-PS prepass was stored as 1x sample pixels; later draws "
          "stay 4x, but earlier sample coverage may differ",
          regs.Get<reg::RB_DEPTH_INFO>().depth_base,
          uint32_t(surface_info.surface_pitch));
    }
    return false;
  }

  if (msaa_4x_unsafe_depth_surfaces_.count(depth_surface_key)) {
    ++msaa_4x_as_1x_stats_reject_history_;
    return false;
  }

  // A surface must enter the samples-as-pixels layout on its first 4x depth
  // use. If it was touched natively first (including while the opt-in is off),
  // its existing sample values may already be packed as MSAA and cannot be
  // safely migrated by merely changing the render-target key.
  if (!was_samples_as_pixels) {
    if (!draw_config_.vulkan_depth_4x_as_1x) {
      ++msaa_4x_as_1x_stats_reject_cvar_;
      msaa_4x_unsafe_depth_surfaces_.insert(depth_surface_key);
      return false;
    }
    if (!depth_control.z_enable || !depth_control.z_write_enable) {
      ++msaa_4x_as_1x_stats_reject_depth_write_;
      msaa_4x_unsafe_depth_surfaces_.insert(depth_surface_key);
      return false;
    }
    if (render_target_cache_->IsDrawResolutionScaled()) {
      ++msaa_4x_as_1x_stats_reject_scaled_;
      msaa_4x_unsafe_depth_surfaces_.insert(depth_surface_key);
      return false;
    }
    if (guest_surface_pitch * 2 > 4096) {
      ++msaa_4x_as_1x_stats_reject_pitch_;
      msaa_4x_unsafe_depth_surfaces_.insert(depth_surface_key);
      return false;
    }
  }

  ++msaa_4x_as_1x_stats_eligible_;
  surface_info.msaa_samples = xenos::MsaaSamples::k1X;
  surface_info.surface_pitch = guest_surface_pitch * 2;
  register_file_->Store(XE_GPU_REG_RB_SURFACE_INFO, surface_info.value);
  render_target_cache_->SetDrawSamplesAsPixels(true);
  ++msaa_4x_as_1x_stats_rewritten_;
  if (msaa_4x_samples_as_pixels_depth_surfaces_.insert(depth_surface_key)
          .second) {
    static uint32_t logged_surfaces = 0;
    if (logged_surfaces < 32) {
      ++logged_surfaces;
      XELOGI(
          "Depth4xAs1x: surface base={} pitch={} uses 1x with a 2x2 sample "
          "grid (quarter-pixel location tradeoff)",
          regs.Get<reg::RB_DEPTH_INFO>().depth_base,
          guest_surface_pitch);
    }
  }
  return true;
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                       uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  if (!cvars::gpu_untile_predicated_tiling) {
    return IssueDrawImpl(prim_type, index_count, index_buffer_info,
                         major_mode_explicit);
  }
  // Untiling: inside a tiling pass, draw at the screen rows of the host render
  // targets (window offset removed), and let the geometry of the first strip
  // cover the whole screen (the guest masks it out of the other strips, see
  // ExecutePacketType3_EVENT_WRITE_EXT). The registers are restored after the
  // draw: the guest keeps seeing its own values.
  RegisterFile& regs = *register_file_;
  // Resolves arrive here as copy-mode draws: they are never dropped and keep
  // the guest's window offset (IssueCopy reads it to address the strip's
  // rows). Dropping them (an earlier version) left the rows below the first strip with
  // the previous frame's contents: a ghosted lower two thirds in GTA IV.
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueDrawImpl(prim_type, index_count, index_buffer_info,
                         major_mode_explicit);
  }
  const uint32_t saved_window_offset =
      regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  const uint32_t saved_window_scissor_br =
      regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t saved_screen_scissor_br =
      regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
  const uint32_t saved_screen_scissor_tl =
      regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL];
  uint32_t signature = 0;
  if (UntileTiled()) {
    UntileSyncPhase();
    reg::PA_SC_WINDOW_OFFSET window_offset;
    window_offset.value = saved_window_offset;
    if (!window_offset.window_x_offset && window_offset.window_y_offset <= 0) {
      const uint32_t row_offset = uint32_t(-window_offset.window_y_offset);
      if (row_offset &&
          (!untile_strip_height_ || row_offset < untile_strip_height_)) {
        untile_strip_height_ = row_offset;
      }
      // Diagnostic: the strip's own draws (not the predicated
      // geometry): offset, scissors and what they write, in sampled frames.
      static uint32_t untile_logged = 0;
      if (untile_logged < 600 && (row_offset || untile_resolve_seen_) &&
          frame_current_ % 512 == 0) {
        ++untile_logged;
        reg::PA_SC_WINDOW_SCISSOR_TL wtl;
        wtl.value = regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
        reg::PA_SC_WINDOW_SCISSOR_BR wbr;
        wbr.value = saved_window_scissor_br;
        reg::PA_SC_SCREEN_SCISSOR_TL stl;
        stl.value = saved_screen_scissor_tl;
        reg::PA_SC_SCREEN_SCISSOR_BR sbr;
        sbr.value = saved_screen_scissor_br;
        XELOGI(
            "UNTILE-DRAW f={} rows={} last_resolve={} resolved={} {} "
            "wscissor {},{}-{},{} wod={} sscissor {},{}-{},{} cmask={:08X} "
            "depthctl={:08X} edram={:08X} n={}",
            frame_current_, row_offset, untile_last_resolve_rows_,
            untile_resolve_seen_,
            (row_offset && row_offset != untile_last_resolve_rows_) ? "SKIP"
                                                                    : "draw",
            uint32_t(wtl.tl_x), uint32_t(wtl.tl_y), uint32_t(wbr.br_x),
            uint32_t(wbr.br_y), uint32_t(wtl.window_offset_disable),
            int32_t(stl.tl_x), int32_t(stl.tl_y), int32_t(sbr.br_x),
            int32_t(sbr.br_y), regs.values[XE_GPU_REG_RB_COLOR_MASK],
            regs.values[XE_GPU_REG_RB_DEPTHCONTROL],
            regs.values[XE_GPU_REG_RB_MODECONTROL], index_count);
      }
      // A later strip before its first resolve: its geometry was drawn over
      // the whole screen in the first strip; what still arrives here is not
      // predicated, like the depth clear GTA IV draws at the top of every
      // strip (GTA-TR1), which would wipe those rows. The draws after the
      // strip's resolves stay: they initialize the rows just copied (the
      // EDRAM state for the next strip), as on the console.
      if (row_offset && row_offset != untile_last_resolve_rows_) {
        return true;
      }
      signature = 1 + row_offset;
      window_offset.window_y_offset = 0;
      regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = window_offset.value;
      if (!row_offset && !untile_resolve_seen_) {
        reg::PA_SC_WINDOW_SCISSOR_BR window_br;
        window_br.value = saved_window_scissor_br;
        untile_strip_bottom_ =
            untile_strip_height_
                ? untile_strip_height_
                : std::max(untile_strip_bottom_, uint32_t(window_br.br_y));
        // A scissor that ends above the strip's bottom is the game's own.
        if (window_br.br_y == untile_strip_bottom_) {
          signature |= UINT32_C(0x80000000);
          window_br.br_y = xenos::kTexture2DCubeMaxWidthHeight;
          regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = window_br.value;
          reg::PA_SC_SCREEN_SCISSOR_BR screen_br;
          screen_br.value = saved_screen_scissor_br;
          if (uint32_t(screen_br.br_y) == untile_strip_bottom_) {
            screen_br.br_y = int32_t(xenos::kTexture2DCubeMaxWidthHeight);
            regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = screen_br.value;
          }
        }
      }
    }
  }
  if (signature != untile_last_signature_) {
    // The cached draw state was derived from other effective values.
    untile_last_signature_ = signature;
    regs.draw_dirty |= RegisterFile::kAllDraw;
  }
  const bool result = IssueDrawImpl(prim_type, index_count, index_buffer_info,
                                    major_mode_explicit);
  regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = saved_window_offset;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = saved_window_scissor_br;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = saved_screen_scissor_br;
  regs.values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL] = saved_screen_scissor_tl;
  return result;
}

bool VulkanCommandProcessor::IssueDrawCensus(
    xenos::PrimitiveType prim_type, uint32_t index_count,
    IndexBufferInfo* index_buffer_info, bool major_mode_explicit,
    uint32_t packet, uint32_t viz_query_condition) {
  auto& s = draw_census_;
  ++s.totals.draws;
  s.recorded = false;
  s.query = (packet & 1) || viz_query_condition ||
            register_file_->Get<reg::PA_SC_VIZ_QUERY>().viz_query_ena ||
            zpd_active_segment_.logical_active ||
            zpd_active_segment_.segment_active ||
            zpd_active_segment_.segment_pending_begin;
  s.changes = 0;
  s.consts_changed = 0;
  s.operands_changed = false;
  s.primitive_type = prim_type;
  s.major_mode_explicit = major_mode_explicit;
  s.index_count = index_count;
  s.index_base = index_buffer_info ? index_buffer_info->guest_base : 0;
  s.index_offset = register_file_->values[XE_GPU_REG_VGT_INDX_OFFSET];
  for (uint32_t i = 0x2000; i < RegisterFile::kRegisterCount; ++i) {
    const uint32_t value = register_file_->values[i];
    const uint32_t previous = s.registers[i];
    // Keep the old fetch type until every word in its six-word group is read.
    if (!s.have_registers || value == previous) {
      continue;
    }
    if ((i >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
         i <= XE_GPU_REG_SHADER_CONSTANT_511_W) ||
        (i >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
         i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31)) {
      ++s.consts_changed;
    } else if (i >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
               i <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
      const uint32_t first = i - (i - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) % 6;
      const uint32_t texture_type = uint32_t(xenos::FetchConstantType::kTexture);
      s.changes |= 1u << (((register_file_->values[first] & 3) == texture_type ||
                          (s.registers[first] & 3) == texture_type)
                             ? kCensusTextures : kCensusFetch);
    } else if (i == XE_GPU_REG_VGT_DMA_BASE ||
               i == XE_GPU_REG_VGT_INDX_OFFSET ||
               (i == XE_GPU_REG_VGT_DMA_SIZE &&
                !((value ^ previous) & UINT32_C(0xFF000000))) ||
               (i == XE_GPU_REG_VGT_DRAW_INITIATOR &&
                !((value ^ previous) & UINT32_C(0xFFFF)))) {
      // Index address/count and guest base vertex belong to the draw record.
      // DMA swap mode, topology, source selection and index format remain
      // state.
      s.operands_changed = true;
    } else if ((i >= XE_GPU_REG_SQ_PROGRAM_CNTL &&
                i <= XE_GPU_REG_SQ_WRAPPING_1) ||
               (i >= XE_GPU_REG_SQ_CF_RD_BASE &&
                i <= XE_GPU_REG_SQ_VS_PROGRAM) ||
               i == XE_GPU_REG_SQ_VS_CONST || i == XE_GPU_REG_SQ_PS_CONST ||
               i == XE_GPU_REG_SQ_CF_PROGRAM_SIZE) {
      s.changes |= 1u << kCensusShader;
    } else if (i >= XE_GPU_REG_RB_SURFACE_INFO &&
               i <= XE_GPU_REG_RB_COLOR3_INFO) {
      s.changes |= 1u << kCensusRt;
    } else if ((i >= XE_GPU_REG_VGT_MAX_VTX_INDX &&
                i <= XE_GPU_REG_VGT_INDX_OFFSET) ||
               (i >= XE_GPU_REG_VGT_DMA_BASE &&
                i <= XE_GPU_REG_VGT_IMMED_DATA) ||
               (i >= XE_GPU_REG_VGT_OUTPUT_PATH_CNTL &&
                i <= XE_GPU_REG_VGT_GROUP_VECT_1_FMT_CNTL)) {
      s.changes |= 1u << kCensusIndex;
    } else {
      s.changes |= 1u << kCensusRaster;
    }
  }
  if (s.have_registers && (s.vertex_shader != active_vertex_shader() ||
                          s.pixel_shader != active_pixel_shader())) {
    s.changes |= 1u << kCensusShader;
  }
  std::copy(std::begin(register_file_->values), std::end(register_file_->values),
            s.registers.begin());
  s.have_registers = true;
  s.vertex_shader = active_vertex_shader();
  s.pixel_shader = active_pixel_shader();
  s.totals.consts_changed += s.consts_changed;
  if (texture_cache_->texture_became_outdated()) {
    s.changes |= 1u << kCensusTextures;
  }
  if (index_buffer_info) {
    const uint64_t key = (uint64_t(index_buffer_info->guest_base) << 32) |
                        uint32_t(index_buffer_info->length);
    if (s.totals.unique_index_ranges == s.index_ranges.size()) {
      ++s.totals.index_range_overflow;
    } else {
      size_t slot = size_t(XXH64(&key, sizeof(key), 0)) &
                    (s.index_ranges.size() - 1);
      while (s.index_ranges_used[slot] && s.index_ranges[slot] != key) {
        slot = (slot + 1) & (s.index_ranges.size() - 1);
      }
      if (!s.index_ranges_used[slot]) {
        s.index_ranges_used[slot] = true;
        s.index_ranges[slot] = key;
        ++s.totals.unique_index_ranges;
      }
    }
  }
  const bool result = IssueDrawImpl<true>(prim_type, index_count,
                                        index_buffer_info, major_mode_explicit);
  if (!s.recorded) {
    ++s.totals.excluded;
    DrawCensusClose(kCensusExcluded);
  }
  return result;
}

void VulkanCommandProcessor::DrawCensusClose(DrawCensusBreak reason) {
  auto& s = draw_census_;
  s.mdi.Close();
  s.texture_relaxed.Close();
  s.instancing.Close();
  DrawCensusCloseStrictRun(reason);
}

void VulkanCommandProcessor::DrawCensusCloseStrictRun(DrawCensusBreak reason) {
  auto& s = draw_census_;
  if (!s.length) {
    return;
  }
  const uint64_t n = s.length;
  const uint32_t bin = n == 1 ? 0 : n < 4 ? 1 : n < 8 ? 2 : n < 16 ? 3 : 4;
  ++s.totals.histogram[bin];
  ++s.totals.breaks[reason];
  if (n > 1) {
    s.totals.grouped += n;
    ++s.totals.groups;
  }
  s.length = 0;
}

void VulkanCommandProcessor::DrawCensusRecord(
    bool fast, bool eligible, const void* vertex_translation,
    const void* pixel_translation,
    const PrimitiveProcessor::ProcessingResult& shape) {
  auto& s = draw_census_;
  s.recorded = true;
  s.totals.fast += fast;
  if (!eligible || s.query) {
    ++s.totals.excluded;
    DrawCensusClose(kCensusExcluded);
    return;
  }
  ++s.totals.eligible;
  bool ordering_boundary = false;
  if (s.length) {
    if (s.vertex_translation != vertex_translation ||
        s.pixel_translation != pixel_translation ||
        s.pipeline != current_guest_graphics_pipeline_ ||
        s.pipeline_handle != current_guest_graphics_pipeline_handle_ ||
        s.layout != current_guest_graphics_pipeline_layout_) {
      s.changes |= 1u << kCensusShader;
    }
    if (s.primitive_type != s.previous_primitive_type ||
        s.major_mode_explicit != s.previous_major_mode_explicit ||
        s.shape.host_primitive_type != shape.host_primitive_type ||
        s.shape.host_index_format != shape.host_index_format ||
        s.shape.host_shader_index_endian != shape.host_shader_index_endian ||
        s.shape.host_primitive_reset_enabled !=
            shape.host_primitive_reset_enabled) {
      s.changes |= 1u << kCensusIndex;
    }
    if (s.framebuffer != current_framebuffer_) {
      s.changes |= 1u << kCensusRt;
    }
    if (s.sampler_generation != texture_cache_->sampler_destroy_generation() ||
        s.image_generation != texture_cache_->image_view_destroy_generation() ||
        texture_cache_->texture_became_outdated() ||
        s.textures[0] !=
            current_graphics_descriptor_sets_
                [SpirvShaderTranslator::kDescriptorSetTexturesVertex] ||
        s.textures[1] !=
            current_graphics_descriptor_sets_
                [SpirvShaderTranslator::kDescriptorSetTexturesPixel]) {
      s.changes |= 1u << kCensusTextures;
    }
    ordering_boundary = s.epoch != draw_state_epoch_ ||
                        s.submission != GetCurrentSubmission() ||
                        s.pass != render_pass_serial_;
  }
  // These are state candidates, not permission to delay a memory observation.
  // Keep exact histograms for the selected rule and for two alternatives.
  const uint32_t state_changes = s.changes & ~(1u << kCensusFetch);
  const bool relaxed =
      !ordering_boundary && !(state_changes & ~(1u << kCensusTextures));
  const bool compatible = relaxed && !(state_changes & (1u << kCensusTextures));
  const bool same_geometry =
      compatible && s.index_count == s.previous_index_count &&
      s.index_base == s.previous_index_base &&
      s.index_offset == s.previous_index_offset &&
      s.shape.guest_index_base == shape.guest_index_base &&
      s.shape.host_draw_vertex_count == shape.host_draw_vertex_count;
  if (s.length) {
    s.totals.vertex_fetch_edges += bool(s.changes & (1u << kCensusFetch));
    s.totals.operand_edges += s.operands_changed ||
                              s.index_count != s.previous_index_count ||
                              s.index_base != s.previous_index_base ||
                              s.index_offset != s.previous_index_offset;
    s.totals.texture_change_edges += bool(s.changes & (1u << kCensusTextures));
    s.totals.texture_only_breaks += relaxed && !compatible;
    if (ordering_boundary) {
      DrawCensusClose(kCensusBarrier);
    } else if (!compatible) {
      // First differing group is the single primary reason.
      uint32_t reason = 0;
      while (!(state_changes & (1u << reason))) {
        ++reason;
      }
      // Keep the relaxed-texture run open across a texture-only break.
      // Closing the legacy run must not close all three independent rules.
      DrawCensusCloseStrictRun(DrawCensusBreak(reason));
    } else if (s.consts_changed && !s.changes && !s.operands_changed &&
               s.index_count == s.previous_index_count &&
               s.index_base == s.previous_index_base &&
               s.index_offset == s.previous_index_offset) {
      // A continuation, not a break: constants alone are allowed to vary.
      ++s.totals.consts_only;
    }
  }
  s.mdi.Add(compatible);
  s.texture_relaxed.Add(relaxed);
  s.instancing.Add(same_geometry);
  ++s.length;
  s.vertex_translation = vertex_translation;
  s.pixel_translation = pixel_translation;
  s.pipeline = current_guest_graphics_pipeline_;
  s.pipeline_handle = current_guest_graphics_pipeline_handle_;
  s.layout = current_guest_graphics_pipeline_layout_;
  s.textures[0] = current_graphics_descriptor_sets_
      [SpirvShaderTranslator::kDescriptorSetTexturesVertex];
  s.textures[1] = current_graphics_descriptor_sets_
      [SpirvShaderTranslator::kDescriptorSetTexturesPixel];
  s.framebuffer = current_framebuffer_;
  s.shape = shape;
  s.previous_primitive_type = s.primitive_type;
  s.previous_major_mode_explicit = s.major_mode_explicit;
  s.previous_index_count = s.index_count;
  s.previous_index_base = s.index_base;
  s.previous_index_offset = s.index_offset;
  s.sampler_generation = texture_cache_->sampler_destroy_generation();
  s.image_generation = texture_cache_->image_view_destroy_generation();
  s.epoch = draw_state_epoch_;
  s.submission = GetCurrentSubmission();
  s.pass = render_pass_serial_;
}

void VulkanCommandProcessor::DrawCensusReport() {
  auto& s = draw_census_;
  ++s.totals.frames;
  // Clock is sampled at swaps only, never per draw.
  const uint64_t now = FrameStatsNow();
  if (!s.report_ns) {
    s.report_ns = now;
    // Start the first complete reporting window at this swap.
    s.totals = {};
    s.index_ranges_used.fill(false);
    s.mdi.ResetWindow();
    s.texture_relaxed.ResetWindow();
    s.instancing.ResetWindow();
    return;
  }
  if (now - s.report_ns < UINT64_C(1000000000)) {
    return;
  }
  const auto& t = s.totals;
  const double f = t.draws ? double(t.grouped) / double(t.draws) : 0.0;
  const double l = t.groups ? double(t.grouped) / double(t.groups) : 0.0;
  const double q = t.draws ? double(t.grouped - t.groups) / double(t.draws) : 0.0;
  XELOGI(
      "DrawCensus: draws={} fast={} eligible={} excluded={} "
      "seq_len_hist=[1:{},2-3:{},4-7:{},8-15:{},16+:{}] "
      "breaks={{shader:{},rt:{},raster:{},textures:{},fetch:{},index:{},"
      "barrier:{},excluded:{}}} consts_only={} consts_changed_per_draw={:.2f} "
      "unique_index_ranges={} index_ranges_untracked={} "
      "f={:.6f} L={:.3f} q={:.6f} state_only=1 interval_ms={:.1f}",
      t.draws, t.fast, t.eligible, t.excluded,
      t.histogram[0], t.histogram[1], t.histogram[2], t.histogram[3], t.histogram[4],
      t.breaks[0], t.breaks[1], t.breaks[2], t.breaks[3], t.breaks[4], t.breaks[5],
      t.breaks[6], t.breaks[7], t.consts_only,
      t.draws ? double(t.consts_changed) / double(t.draws) : 0.0,
      t.unique_index_ranges, t.index_range_overflow, f, l, q,
      double(now - s.report_ns) / 1e6);
  const auto& properties = GetVulkanDevice()->properties();
  const uint32_t device_limit =
      properties.multiDrawIndirect && properties.drawIndirectFirstInstance
          ? properties.maxDrawIndirectCount
          : 1;
  const auto report_sequences = [&](const char* rule,
                                    const DrawSequenceCensus& sequences) {
    std::string lengths;
    for (const auto& entry : sequences.lengths()) {
      if (!lengths.empty()) lengths += ',';
      lengths += fmt::format("{}:{}", entry.first, entry.second);
    }
    XELOGI(
        "DrawBatchCensus: rule={} state_only=1 memory_versions=0 "
        "frames={} draws={} candidates={} sequences={} max_length={} "
        "lengths=[{}] removed_cap4={} removed_cap16={} removed_cap64={} "
        "device_limit={} removed_device_limit={} "
        "draws_per_frame={:.3f} emitted_cap4_per_frame={:.3f}",
        rule, t.frames, t.draws, sequences.draws(), sequences.sequences(),
        sequences.max_length(), lengths, sequences.Removed(4),
        sequences.Removed(16), sequences.Removed(64), device_limit,
        sequences.Removed(device_limit),
        t.frames ? double(t.draws) / double(t.frames) : 0.0,
        t.frames ? double(t.draws - sequences.Removed(4)) / double(t.frames)
                 : 0.0);
  };
  report_sequences("mdi_same_textures", s.mdi);
  report_sequences("mdi_texture_arrays_upper_bound", s.texture_relaxed);
  report_sequences("instancing_same_geometry", s.instancing);
  XELOGI(
      "DrawBatchCensusEdges: vertex_fetch={} index_operands={} "
      "texture_changes={} texture_only_breaks={} "
      "state_only=1 memory_versions=0",
      t.vertex_fetch_edges, t.operand_edges, t.texture_change_edges,
      t.texture_only_breaks);
  s.totals = {};
  s.index_ranges_used.fill(false);
  s.mdi.ResetWindow();
  s.texture_relaxed.ResetWindow();
  s.instancing.ResetWindow();
  s.report_ns = now;
}

void VulkanCommandProcessor::ThermalGovernorPoll() {
  const uint64_t now = FrameStatsNow();
  if (thermal_governor_last_ns_ &&
      now - thermal_governor_last_ns_ < UINT64_C(2000000000)) {
    return;
  }
  thermal_governor_last_ns_ = now;
  float headroom = std::numeric_limits<float>::quiet_NaN();
#if defined(__ANDROID__)
  // (XE_PLATFORM_ANDROID is not defined in this fork.)
  // AThermal_getThermalHeadroom is API 31; resolved at runtime so the library
  // still loads on API 29.
  struct AThermalManagerOpaque;
  static void* android_library = dlopen("libandroid.so", RTLD_NOW);
  static auto acquire_manager =
      android_library ? reinterpret_cast<AThermalManagerOpaque* (*)()>(
                            dlsym(android_library, "AThermal_acquireManager"))
                      : nullptr;
  static auto get_headroom =
      android_library
          ? reinterpret_cast<float (*)(AThermalManagerOpaque*, int)>(
                dlsym(android_library, "AThermal_getThermalHeadroom"))
          : nullptr;
  static AThermalManagerOpaque* manager =
      acquire_manager ? acquire_manager() : nullptr;
  static bool thermal_api_logged = false;
  if (!thermal_api_logged) {
    thermal_api_logged = true;
    XELOGI("ThermalGovernor: libandroid={} acquire={} headroom_fn={} "
           "manager={}",
           android_library != nullptr, acquire_manager != nullptr,
           get_headroom != nullptr, manager != nullptr);
  }
  if (manager && get_headroom) {
    headroom = get_headroom(manager, 0);
  }
#endif
  uint32_t rate = thermal_governor_rate_;
  if (headroom == headroom) {
    const float step_1 = float(cvars::thermal_governor_headroom_1);
    const float step_2 = float(cvars::thermal_governor_headroom_2);
    if (headroom >= step_2) {
      rate = 3;
    } else if (headroom >= step_1) {
      // Between the thresholds: up from full rate, down from 2x2 only once
      // 0.05 below the upper threshold.
      if (rate == 0 || headroom < step_2 - 0.05f) {
        rate = 1;
      }
    } else if (headroom < step_1 - 0.05f) {
      rate = 0;
    } else if (rate == 3) {
      rate = 1;
    }
  }
  if (rate != thermal_governor_rate_ || !thermal_governor_logged_) {
    thermal_governor_logged_ = true;
    XELOGI("ThermalGovernor: headroom={:.3f} shading_rate={}", headroom, rate);
  }
  thermal_governor_rate_ = rate;
  xe::cpu::backend::thermal_hot.store(rate != 0 || adaptive_gpu_bound_,
                                      std::memory_order_relaxed);
}

void VulkanCommandProcessor::AdaptiveShadingPoll() {
  const uint64_t now = FrameStatsNow();
  ++adaptive_shading_frames_;
  if (!adaptive_shading_window_ns_) {
    adaptive_shading_window_ns_ = now;
    adaptive_shading_busy_ns_ = 0;
    adaptive_shading_frames_ = 0;
    return;
  }
  const uint64_t wall_ns = now - adaptive_shading_window_ns_;
  if (wall_ns < UINT64_C(1000000000)) {
    return;
  }
  const double busy = double(adaptive_shading_busy_ns_) / double(wall_ns);
  const double fps = double(adaptive_shading_frames_) * 1e9 / double(wall_ns);
  adaptive_shading_window_ns_ = now;
  adaptive_shading_busy_ns_ = 0;
  adaptive_shading_frames_ = 0;
  // 0 full rate, 1 2x1, 3 2x2 (vulkan_fragment_shading_rate values), one step
  // per window either way. 2x2 only after a whole window at 2x1 still GPU
  // bound under the frame rate floor.
  uint32_t rate = adaptive_shading_rate_;
  if (adaptive_shading_since_down_ != UINT32_MAX) {
    ++adaptive_shading_since_down_;
  }
  if (busy >= cvars::adaptive_shading_rate_busy_up) {
    adaptive_shading_calm_windows_ = 0;
    if (!rate) {
      rate = 1;
    } else if (rate == 1 &&
               fps < double(cvars::adaptive_shading_rate_2x2_below_fps)) {
      rate = 3;
    }
    if (rate != adaptive_shading_rate_ && adaptive_shading_since_down_ <= 3) {
      adaptive_shading_down_hold_ =
          std::min(adaptive_shading_down_hold_ * 2, uint32_t(16));
    }
  } else if (busy < cvars::adaptive_shading_rate_busy_down && rate) {
    if (++adaptive_shading_calm_windows_ >= adaptive_shading_down_hold_) {
      rate = rate == 3 ? 1 : 0;
      adaptive_shading_calm_windows_ = 0;
      adaptive_shading_since_down_ = 0;
    }
  } else {
    adaptive_shading_calm_windows_ = 0;
  }
  if (adaptive_shading_since_down_ != UINT32_MAX &&
      adaptive_shading_since_down_ >= 30) {
    // Stable for a while: steps down are quick again.
    adaptive_shading_down_hold_ = 2;
    adaptive_shading_since_down_ = UINT32_MAX;
  }
  const bool gpu_bound = rate != 0;
  if (gpu_bound != adaptive_gpu_bound_) {
    adaptive_gpu_bound_ = gpu_bound;
    xe::cpu::backend::thermal_hot.store(
        gpu_bound || (cvars::thermal_governor && thermal_governor_rate_ != 0),
        std::memory_order_relaxed);
  }
  if (rate != adaptive_shading_rate_) {
    XELOGI(
        "AdaptiveShading: gpu_busy={:.2f} fps={:.1f} shading_rate={} "
        "down_hold={}",
        busy, fps, rate, adaptive_shading_down_hold_);
    adaptive_shading_rate_ = rate;
  }
}

// XenDroid diagnostic: IssueDrawImpl has many silent failure exits. Each
// distinct exit is logged once (source line), so a rejected draw can be traced
// to its cause without a debugger.
static bool XdDrawFail(int line) {
  static std::atomic<uint64_t> seen_low{0}, seen_high{0};
  static std::atomic<int> first_line{0};
  int expected = 0;
  first_line.compare_exchange_strong(expected, line);
  uint32_t bit = uint32_t(line - first_line.load()) & 127;
  std::atomic<uint64_t>& seen = bit < 64 ? seen_low : seen_high;
  uint64_t mask = uint64_t(1) << (bit & 63);
  if (!(seen.fetch_or(mask) & mask)) {
    XELOGE("IssueDraw: rejected at vulkan_command_processor.cc:{}", line);
  }
  return false;
}
bool VulkanCommandProcessor::UpdateVertexInputRanges(const VulkanShader& shader) {
  if (vertex_input_shader_ == &shader &&
      vertex_input_fetch_generation_ == register_file_->fetch_generation)
    return true;
  vertex_input_shader_ = nullptr;
  vertex_input_count_ = 0;
  const RegisterFile& regs = *register_file_;
  const auto& map = shader.constant_register_map();
  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t remaining = map.vertex_fetch_bitmap[i], j;
    while (xe::bit_scan_forward(remaining, &j)) {
      remaining = xe::clear_lowest_bit(remaining);
      const uint32_t vfetch_index = i * 32 + j;
      const auto vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (draw_config_.gpu_allow_invalid_fetch_constants) {
            break;
          }
          XELOGW(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
              "type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with "
              "--gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return XdDrawFail(__LINE__);
        default:
          // Type is kTexture (2) or kInvalidTexture (3) - completely wrong
          // for vertex data
          if (draw_config_.gpu_allow_invalid_fetch_constants) {
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has wrong type {} "
                "(texture fetch constant in vertex slot) - allowing due to "
                "--gpu_allow_invalid_fetch_constants=true. This will likely "
                "crash or produce garbage!",
                vfetch_index, vfetch_constant.dword_0,
                vfetch_constant.dword_1,
                static_cast<uint32_t>(vfetch_constant.type));
            break;
          }
          XELOGW(
              "Vertex fetch constant {} ({:08X} {:08X}) is completely "
              "invalid! Type={} - this slot contains a texture fetch "
              "constant (type 2), not a vertex fetch constant (type 0). "
              "This may indicate the shader is reading from the wrong fetch "
              "constant index, or the game has a bug.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1,
              static_cast<uint32_t>(vfetch_constant.type));
          return XdDrawFail(__LINE__);
      }

      auto& range = vertex_input_ranges_[vertex_input_count_++];
      range.memory.Set(vfetch_constant.address << 2, vfetch_constant.size << 2);
      range.slot = vfetch_index;
    }
  }
  vertex_input_shader_ = &shader;
  vertex_input_fetch_generation_ = register_file_->fetch_generation;
  return true;
}

void VulkanCommandProcessor::ResetDrawBounds(bool draw_height_bound) {
    if (draw_height_bound) {
      deferred_command_buffer_->SetDrawHeightBound(UINT32_MAX);
    }
    deferred_command_buffer_->SetDrawAttachmentUse(
        DeferredCommandBuffer::kDrawAttachmentUseAll);
}

void VulkanCommandProcessor::ConfigureAlphaTestedDepthPrepass(
    const VulkanShader& vertex_shader, VulkanShader& pixel_shader,
    VulkanShader::VulkanTranslation* vertex_shader_translation,
    SpirvShaderTranslator::Modification pixel_shader_modification,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    uint32_t interpolator_mask, uint32_t ps_param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, bool apply_host_depth_polygon_offset,
    VulkanPipelineCache::Pipeline** prepass_pipeline_out,
    VulkanPipelineCache::Pipeline** color_pipeline_out) {
  *prepass_pipeline_out = nullptr;
  *color_pipeline_out = nullptr;
  auto reject = [this](AlphaTestedPrepassReject reason) {
    ++alpha_tested_prepass_rejects_[size_t(reason)];
  };
  using DepthStencilMode =
      SpirvShaderTranslator::Modification::DepthStencilMode;
  const RegisterFile& regs = *register_file_;
  const VulkanRenderTargetCache::RenderPassKey render_pass_key =
      render_target_cache_->last_update_render_pass_key();
  // Only draws left on late tests because the shader may kill: no depth
  // output, float24 conversion or polygon offset, no software clipping. Alpha
  // to coverage is fine: both draws compute the same sample mask, the
  // depth-only one writes depth to those samples, and in the color one it
  // still limits the color writes.
  if (render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kHostRenderTargets ||
      !(render_pass_key.depth_and_color_used & 1) || !normalized_color_mask) {
    reject(AlphaTestedPrepassReject::kNoColorOrDepth);
    return;
  }
  if (pixel_shader_modification.pixel.depth_stencil_mode ==
          DepthStencilMode::kEarlyHint ||
      pixel_shader_modification.IsEarlyTestsWithKill()) {
    reject(AlphaTestedPrepassReject::kAlreadyEarly);
    return;
  }
  if (pixel_shader.writes_depth() || apply_host_depth_polygon_offset ||
      vertex_shader.memexport_eM_written() ||
      pixel_shader.memexport_eM_written() ||
      (pixel_shader_modification.pixel.depth_stencil_mode !=
           DepthStencilMode::kNoModifiers &&
       pixel_shader_modification.pixel.depth_stencil_mode !=
           DepthStencilMode::kNoAlphaToCoverage) ||
      pixel_shader_modification.pixel.param_gen_enable ||
      pixel_shader_modification.pixel.dynamic_addressable_register_count >=
          126) {
    reject(AlphaTestedPrepassReject::kShader);
    return;
  }
  // Depth written with a test that a second identical fragment passes too
  // (less / greater would reject the equal depth the first draw left).
  if (!normalized_depth_control.z_enable ||
      !normalized_depth_control.z_write_enable ||
      (normalized_depth_control.zfunc != xenos::CompareFunction::kLessEqual &&
       normalized_depth_control.zfunc !=
           xenos::CompareFunction::kGreaterEqual)) {
    reject(AlphaTestedPrepassReject::kDepthFunc);
    return;
  }
  reg::RB_DEPTHCONTROL color_depth_control = normalized_depth_control;
  color_depth_control.zfunc = xenos::CompareFunction::kEqual;
  color_depth_control.z_write_enable = 0;
  // Stencil only tested: both draws see the same stencil. Written with the
  // "always" function (deferred renderers marking materials, GTA IV): the
  // depth-only draw makes the writes exactly like the original draw (same
  // depth test, same kill), and the color draw, whose test would always pass,
  // leaves stencil alone.
  if (normalized_depth_control.stencil_enable) {
    auto keep = [](xenos::StencilOp op) {
      return op == xenos::StencilOp::kKeep;
    };
    bool stencil_ops_write =
        !keep(normalized_depth_control.stencilfail) ||
        !keep(normalized_depth_control.stencilzfail) ||
        !keep(normalized_depth_control.stencilzpass) ||
        (normalized_depth_control.backface_enable &&
         (!keep(normalized_depth_control.stencilfail_bf) ||
          !keep(normalized_depth_control.stencilzfail_bf) ||
          !keep(normalized_depth_control.stencilzpass_bf)));
    uint32_t stencil_write_mask =
        regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask |
        (normalized_depth_control.backface_enable
             ? regs.Get<reg::RB_STENCILREFMASK>(
                       XE_GPU_REG_RB_STENCILREFMASK_BF)
                   .stencilwritemask
             : 0);
    if (stencil_ops_write && stencil_write_mask) {
      if (normalized_depth_control.stencilfunc !=
              xenos::CompareFunction::kAlways ||
          (normalized_depth_control.backface_enable &&
           normalized_depth_control.stencilfunc_bf !=
               xenos::CompareFunction::kAlways)) {
        reject(AlphaTestedPrepassReject::kStencil);
        return;
      }
      color_depth_control.stencil_enable = 0;
    }
  }
  // Opaque: with blending, the hidden fragments of the draw itself would be
  // blended in before the ones covering them.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!((normalized_color_mask >> (i * 4)) & 0xF)) {
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
      reject(AlphaTestedPrepassReject::kBlend);
      return;
    }
  }

  SpirvShaderTranslator::Modification color_modification =
      pipeline_cache_->GetCurrentPixelShaderModification(
          pixel_shader, interpolator_mask, ps_param_gen_pos,
          color_depth_control, normalized_color_mask, false);
  if (!color_modification.IsEarlyTestsWithKill()) {
    // vulkan_early_tests_without_zs_writes is off.
    reject(AlphaTestedPrepassReject::kVariant);
    return;
  }
  // No color outputs: everything but what decides the kill is dead code.
  SpirvShaderTranslator::Modification prepass_modification =
      pipeline_cache_->GetCurrentPixelShaderModification(
          pixel_shader, interpolator_mask, ps_param_gen_pos,
          normalized_depth_control, 0, false);
  auto* color_translation = static_cast<VulkanShader::VulkanTranslation*>(
      pixel_shader.GetOrCreateTranslation(color_modification.value));
  auto* prepass_translation = static_cast<VulkanShader::VulkanTranslation*>(
      pixel_shader.GetOrCreateTranslation(prepass_modification.value));
  if (!pipeline_cache_->CanCreatePipelineAsync(true) &&
      (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                 color_translation) ||
       !pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                 prepass_translation))) {
    reject(AlphaTestedPrepassReject::kPipeline);
    return;
  }
  VulkanPipelineCache::Pipeline* color_pipeline;
  VulkanPipelineCache::Pipeline* prepass_pipeline;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, prepass_translation,
          primitive_processing_result, normalized_depth_control, 0,
          render_pass_key, false, &prepass_pipeline) ||
      !pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, color_translation,
          primitive_processing_result, color_depth_control,
          normalized_color_mask, render_pass_key, false, &color_pipeline)) {
    reject(AlphaTestedPrepassReject::kPipeline);
    return;
  }
  ++alpha_tested_prepass_configured_;
  *prepass_pipeline_out = prepass_pipeline;
  *color_pipeline_out = color_pipeline;
}

void VulkanCommandProcessor::BindDrawPipeline(
    VulkanPipelineCache::Pipeline* pipeline, VkPipeline current_pipeline) {
  // Bind the handle observed when this draw's bindings were decided
  // (current_pipeline, loaded above), not the slot at replay time: the
  // recorded descriptor sets cover only that pipeline's layout. A draw
  // recorded while the slot was empty is dropped at replay even if the
  // async creation finishes in between. De-dup on the handle, so the first
  // draw after the slot fills (or swaps placeholder to real) re-records.
  if (current_guest_graphics_pipeline_ != &pipeline->pipeline ||
      current_guest_graphics_pipeline_handle_ != current_pipeline) {
    deferred_command_buffer_->CmdVkBindPipelineDeferred(
        VK_PIPELINE_BIND_POINT_GRAPHICS, current_pipeline);
    current_guest_graphics_pipeline_ = &pipeline->pipeline;
    current_guest_graphics_pipeline_handle_ = current_pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }
  auto pipeline_layout = static_cast<const PipelineLayout*>(
      pipeline->pipeline_layout.load(std::memory_order_acquire));
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible). The two texture sets
      // are the highest enum indices, so a change of the vertex texture set
      // layout breaks compatibility for the pixel texture set too; a pixel-only
      // change breaks only the pixel set.
      //
      // Previously the unconditional clear of the texture values-up-to-date
      // bits in UpdateBindings made this case safe even though the computed
      // descriptor_sets_kept was discarded. Now that the value cache may keep
      // those bits set, this block must actively invalidate the values bit,
      // the bound bit, and the value-cache hash for every texture set whose
      // layout is no longer compatible - otherwise a set written under layout A
      // could be skipped and bound under an incompatible layout B.
      uint32_t descriptor_sets_kept =
          uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
      // Invalidate every texture set at or above the lowest changed index
      // (the cascade: incompatibility for set N breaks all sets >= N). This
      // supersedes upstream's `bound_up_to_date_ &= (1<<descriptor_sets_kept)-1`
      // masking: in this block descriptor_sets_kept never drops below
      // kDescriptorSetTexturesVertex, so the per-set clears below cover the same
      // bound bits AND add the value invalidation the texture-set cache needs.
      if (descriptor_sets_kept <=
          uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
        current_graphics_descriptor_sets_bound_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
        current_texture_descriptors_valid_vertex_ = false;
      }
      if (descriptor_sets_kept <=
          uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
        current_graphics_descriptor_sets_bound_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
        current_texture_descriptors_valid_pixel_ = false;
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state. The transient pool has NOT reset within the frame
      // (that path resets the descriptor validity at frame open), so the cached
      // texture sets are still valid memory; clearing the bound bits forces a
      // re-bind of the still-valid sets without a re-write. Do not invalidate
      // the descriptor cache here - the value cache stays usable across an intra-frame
      // submission/command-buffer restart.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }
}

template <bool census>
bool VulkanCommandProcessor::IssueDrawImpl(
    xenos::PrimitiveType prim_type, uint32_t index_count,
    IndexBufferInfo* index_buffer_info, bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  struct SurfaceInfoRestore {
    RegisterFile& registers;
    RenderTargetCache& render_target_cache;
    uint32_t surface_info;
    ~SurfaceInfoRestore() {
      registers.Store(XE_GPU_REG_RB_SURFACE_INFO, surface_info);
      render_target_cache.SetDrawSamplesAsPixels(false);
    }
  } surface_info_restore{*register_file_, *render_target_cache_,
                         register_file_->values[XE_GPU_REG_RB_SURFACE_INFO]};

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }
  if (!BeginSubmission(true)) return XdDrawFail(__LINE__);
  const uint64_t depth_materialization_epoch = depth_materialization_epoch_;
  if (draw_config_.log_draw_state_repeats) {
    CountDrawRepeat(prim_type, index_count, index_buffer_info != nullptr);
  }
  if (draw_phase_stats_enabled_) FrameStatsPhaseStart();
  SharedMemory::PendingAccessScope pending_access_scope(
      draw_config_.vulkan_republish_draw_inputs ? shared_memory_.get() : nullptr);
  register_file_->SynchronizeExternalWrites();
  const uint32_t fetch_slots_written = register_file_->fetch_dirty_slots;
  uint32_t fetch_slots = fetch_slots_written, fetch_slot;
  while (xe::bit_scan_forward(fetch_slots, &fetch_slot)) {
    fetch_slots = xe::clear_lowest_bit(fetch_slots);
    FetchWritten(fetch_slot);
  }
  register_file_->fetch_dirty_slots = 0;
  DrawStateCache& cached = draw_state_cache_;
  const bool cache_valid = cached.valid;
  uint32_t draw_dirty = register_file_->draw_dirty;
  if (!cache_valid || cached.vertex_shader != active_vertex_shader() ||
      cached.pixel_shader != active_pixel_shader()) {
    draw_dirty |= RegisterFile::kAllDraw;
  } else if (cached.epoch != draw_state_epoch_ ||
             cached.submission != GetCurrentSubmission()) {
    draw_dirty |= RegisterFile::kRenderTargets | RegisterFile::kPipeline |
                  RegisterFile::kRaster | RegisterFile::kSystem | RegisterFile::kFetch;
  }
  bool fast = !(draw_dirty & ~RegisterFile::kFetch);
  uint32_t fast_miss = fast ? 0 : 1;
  // A failed or skipped operation never publishes derived state as reusable.
  cached.valid = false;
  // The signs of the textures of the first fetch constants the pixel shader
  // fetches, for the specialization constants of its pipeline
  // (spirv_texture_sign_specialization) - from the fetch constants, like the
  // swizzled signs the texture cache gives the shaders. Where it binds no
  // texture (taking the signs as unsigned), the classes made known (unsigned
  // or gamma) give the same zeros. The pipeline depends on them, so a change
  // of the fetch constants changing them ends the fast path.
  uint32_t texture_sign_classes = cached.texture_sign_classes;
  // Whether every texture of the pixel shader has a zero exponent adjustment
  // (spirv_texture_exp_adjust_specialization; word 3 bits 13:18).
  bool texture_exp_adjust_zero = cached.texture_exp_adjust_zero;
  if ((draw_dirty & RegisterFile::kShader) ||
      (fetch_slots_written & cached.used_texture_mask)) {
    texture_sign_classes = 0;
    texture_exp_adjust_zero = false;
    Shader* specialization_pixel_shader = active_pixel_shader();
    // The first draw of a new pixel shader comes here before the analysis
    // below: analyze it now, or that draw gets a pipeline specialized for all
    // textures unsigned (and one more pipeline to create).
    if (specialization_pixel_shader &&
        !specialization_pixel_shader->is_ucode_analyzed() &&
        (draw_config_.spirv_texture_sign_specialization ||
         draw_config_.spirv_texture_exp_adjust_specialization)) {
      pipeline_cache_->AnalyzeShaderUcode(*specialization_pixel_shader);
    }
    if (specialization_pixel_shader &&
        specialization_pixel_shader->is_ucode_analyzed()) {
      if (draw_config_.spirv_texture_sign_specialization) {
        // The slots the translator gives the fetch constants.
        std::array<uint8_t, 32> texture_sign_class_slots;
        SpirvShaderTranslator::GetTextureSignClassSlots(
            *specialization_pixel_shader,
            draw_config_.spirv_texture_sign_specialization_used,
            texture_sign_class_slots);
        for (const Shader::TextureBinding& texture_binding :
             specialization_pixel_shader->texture_bindings()) {
          const uint32_t fetch_constant = texture_binding.fetch_constant;
          const uint32_t slot = texture_sign_class_slots[fetch_constant];
          if (slot >=
              SpirvShaderTranslator::kTextureSignClassFetchConstantCount) {
            continue;
          }
          texture_sign_classes |=
              SpirvShaderTranslator::GetTextureSignClass(
                  texture_util::SwizzleSigns(regs.GetTextureFetch(fetch_constant)))
              << (2 * slot);
        }
      }
      if (draw_config_.spirv_texture_exp_adjust_specialization) {
        texture_exp_adjust_zero = true;
        for (const Shader::TextureBinding& texture_binding :
             specialization_pixel_shader->texture_bindings()) {
          // This same predicate also folds the fetch-constant LOD bias.
          const auto& fetch = regs.GetTextureFetch(texture_binding.fetch_constant);
          if (fetch.exp_adjust || fetch.lod_bias) {
            texture_exp_adjust_zero = false;
            break;
          }
        }
      }
    }
    if (texture_sign_classes != cached.texture_sign_classes ||
        texture_exp_adjust_zero != cached.texture_exp_adjust_zero) {
      draw_dirty |= RegisterFile::kShader | RegisterFile::kPipeline;
      fast = false;
    }
  }
  pipeline_cache_->SetTextureSignClasses(texture_sign_classes);
  pipeline_cache_->SetTextureExpAdjustZero(texture_exp_adjust_zero);
  ++fast_path_draws_seen_;
  // Re-armed at the end of a successfully recorded draw only.

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw. Matches the Direct3D 12 backend.
    // TODO(Triang3l): Do something so memexport still works in this case maybe?
    // Unlikely that zero would even really be legal though.
    return true;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return XdDrawFail(__LINE__);
  }
  if (!vertex_shader->is_ucode_analyzed())
    pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  // TODO(Triang3l): If the shader uses memory export, but
  // vertexPipelineStoresAndAtomics is not supported, convert the vertex shader
  // to a compute shader and dispatch it after the draw if the draw doesn't use
  // tessellation.
  const bool memexport_used_vertex =
      vertex_shader->memexport_eM_written() != 0 &&
      device_properties.vertexPipelineStoresAndAtomics;
  if (draw_config_.log_memexport_draws > 0 && (vertex_shader->memexport_eM_written() ||
                                          (active_pixel_shader() &&
                                           active_pixel_shader()->memexport_eM_written()))) {
    // Diagnostic: memory export draws, and whether the host drops them.
    static uint32_t memexport_draws_logged = 0;
    static uint64_t memexport_vs_dropped_total = 0, memexport_draws_total = 0;
    ++memexport_draws_total;
    bool vs_dropped = vertex_shader->memexport_eM_written() &&
                      !device_properties.vertexPipelineStoresAndAtomics;
    memexport_vs_dropped_total += vs_dropped ? 1 : 0;
    if (memexport_draws_logged < uint32_t(draw_config_.log_memexport_draws) ||
        memexport_draws_total % 2000 == 0) {
      ++memexport_draws_logged;
      std::vector<draw_util::MemExportRange> log_ranges;
      if (vertex_shader->memexport_eM_written()) {
        draw_util::AddMemExportRanges(regs, *vertex_shader, log_ranges);
      }
      Shader* log_pixel_shader = active_pixel_shader();
      if (log_pixel_shader && log_pixel_shader->memexport_eM_written()) {
        draw_util::AddMemExportRanges(regs, *log_pixel_shader, log_ranges);
      }
      std::string ranges_text;
      for (const draw_util::MemExportRange& range : log_ranges) {
        ranges_text += fmt::format(" {:08X}+{}", range.base_address_dwords << 2,
                                   range.size_bytes);
      }
      XELOGI(
          "[MEMEXPORT] n={} total={} vs_dropped_total={} vs={:016X} vs_eM={:X} "
          "ps_eM={:X} vs_dropped={} prim={} count={} indexed={} ranges:{}",
          memexport_draws_logged, memexport_draws_total,
          memexport_vs_dropped_total, vertex_shader->ucode_data_hash(),
          vertex_shader->memexport_eM_written(),
          log_pixel_shader ? log_pixel_shader->memexport_eM_written() : 0,
          vs_dropped, uint32_t(prim_type), index_count,
          index_buffer_info != nullptr, ranges_text);
    }
  }
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  // Without vertex pipeline stores, the export is done by the vertex shader
  // translated to a compute shader, dispatched with one invocation per guest
  // vertex. Only non-indexed draws - the compute shader autogenerates indices.
  const bool memexport_vs_compute =
      draw_config_.vulkan_memexport_vs_compute && !memexport_used_vertex &&
      vertex_shader->memexport_eM_written() != 0 &&
      !device_properties.vertexPipelineStoresAndAtomics &&
      index_buffer_info == nullptr && index_count != 0;
  if (memexport_vs_compute) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = cached.primitive_polygonal;
  bool is_rasterization_done = cached.rasterization;
  VulkanShader* pixel_shader = cached.effective_pixel_shader;
  if (draw_dirty & RegisterFile::kShader) {
    primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
    is_rasterization_done =
        draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
    pixel_shader = nullptr;
    if (is_rasterization_done) {
      // See xenos::EdramMode for explanation why the pixel shader is only used
      // when it's kColorDepth here.
      if (edram_mode == xenos::EdramMode::kColorDepth) {
        pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
        if (pixel_shader) {
          if (!pixel_shader->is_ucode_analyzed())
            pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
          if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                               regs)) {
            pixel_shader = nullptr;
          }
        }
      }
    } else {
      // Disabling pixel shader for this case is also required by the pipeline
      // cache.
      if (!memexport_used_vertex && !memexport_vs_compute) {
        // This draw has no effect.
        return true;
      }
    }
    cached.primitive_polygonal = primitive_polygonal;
    cached.rasterization = is_rasterization_done;
    cached.effective_pixel_shader = pixel_shader;
  }
  if (!is_rasterization_done && !memexport_used_vertex && !memexport_vs_compute)
    return true;
  const bool memexport_used_pixel = pixel_shader &&
                                    pixel_shader->memexport_eM_written() != 0 &&
                                    device_properties.fragmentStoresAndAtomics;
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  if (resolve_defer_census_.active) {
    ResolveDeferDraw(*vertex_shader, pixel_shader, index_buffer_info);
  }

  if (deferred_resolve_pending_) {
    ResolveDeferDrawWrites(
        draw_util::GetNormalizedDepthControl(regs),
        pixel_shader ? draw_util::GetNormalizedColorMask(
                           regs, pixel_shader->writes_color_targets()) : 0);
    const uint32_t reasons = resolve_defer_census_.reasons;
    if (reasons) {
      FlushDeferredResolve(
          (reasons & kResolveDeferWrite) ? ResolveDeferStop::kWrite
          : (reasons & kResolveDeferRead) ? ResolveDeferStop::kDestination
          : (reasons & kResolveDeferEvent) ? ResolveDeferStop::kEvent
                                         : ResolveDeferStop::kResource);
    }
  }

  uint32_t ps_param_gen_pos = cached.ps_param_gen_pos;
  uint32_t interpolator_mask = cached.interpolator_mask;
  if (draw_dirty & RegisterFile::kShader) {
    ps_param_gen_pos = UINT32_MAX;
    interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(
                          regs.Get<reg::SQ_PROGRAM_CNTL>(),
                          regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                   : 0;
    cached.ps_param_gen_pos = ps_param_gen_pos;
    cached.interpolator_mask = interpolator_mask;
  }
  if (!memexport_ranges_.empty()) draw_dirty |= RegisterFile::kAllDraw;
  const bool depth_4x_as_1x =
      RewriteMsaa4xDepthSurfaceInfoForDraw(pixel_shader);
  if (depth_4x_as_1x && resolve_defer_census_.active) {
    resolve_defer_census_.reasons |= kResolveDeferUnknown;
  }
  if (depth_4x_as_1x) {
    FlushDeferredResolve(ResolveDeferStop::kResource);
    draw_dirty |= RegisterFile::kAllDraw;
  } else if (last_draw_samples_as_pixels_) {
    draw_dirty |= RegisterFile::kAllDraw;
  }
  last_draw_samples_as_pixels_ = depth_4x_as_1x;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool use_interpreter = false;
  bool drop_until_ready = false;
  uint32_t normalized_color_mask;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset{};
  bool apply_host_depth_polygon_offset = false;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  // One readiness snapshot per draw, taken where the sampler collection reads
  // it. Re-reading bindings_ready() later would claim binding counts for a
  // stage whose sampler vector was collected as empty.
  bool stage_bindings_ready[2] = {false, false};
  for (uint32_t i = 0; i < 2; ++i) {
    FrameStatsPhase(0);
    if (i && !BeginSubmission(true)) {
      return XdDrawFail(__LINE__);
    }
    FrameStatsPhase(8);

    // Process primitives.
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return XdDrawFail(__LINE__);
    }
    FrameStatsPhase(9);
    if (i || !cache_valid ||
        GetCurrentSubmission() != cached.submission ||
        primitive_processing_result.host_vertex_shader_type != cached.shape.host_vertex_shader_type ||
        primitive_processing_result.host_primitive_type != cached.shape.host_primitive_type ||
        primitive_processing_result.index_buffer_type != cached.shape.index_buffer_type ||
        primitive_processing_result.host_index_format != cached.shape.host_index_format ||
        primitive_processing_result.host_shader_index_endian != cached.shape.host_shader_index_endian ||
        primitive_processing_result.host_primitive_reset_enabled != cached.shape.host_primitive_reset_enabled ||
        primitive_processing_result.tessellation_mode != cached.shape.tessellation_mode) {
      draw_dirty |= RegisterFile::kAllDraw;
      fast = false;
    }
    stat_host_vertices_ += primitive_processing_result.host_draw_vertex_count;
    ++stat_host_draws_;
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    // TODO(Triang3l): Geometry-type-specific vertex shader, vertex shader as
    // compute.
    // Skip unsupported host vertex shader types (but allow tessellation types
    // through - they will be handled in pipeline creation or rejected there if
    // not fully supported yet). Both AsTriangleStrip fallbacks are needed on
    // hosts without geometry shader support (MoltenVK / MoltenVK-like); the
    // SPIR-V translator and pipeline cache both handle them end-to-end.
    if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(
            primitive_processing_result.host_vertex_shader_type)) {
      return XdDrawFail(__LINE__);
    }

    if (!(draw_dirty & (RegisterFile::kShader | RegisterFile::kPipeline))) {
      normalized_depth_control = cached.normalized_depth_control;
      normalized_color_mask = cached.normalized_color_mask;
      apply_host_depth_polygon_offset = cached.apply_host_depth_polygon_offset;
      host_depth_polygon_offset = cached.host_depth_polygon_offset;
      vertex_shader_modification = cached.vertex_shader_modification;
      pixel_shader_modification = cached.pixel_shader_modification;
      vertex_shader_translation = cached.vertex_shader_translation;
      pixel_shader_translation = cached.pixel_shader_translation;
      use_interpreter = cached.use_interpreter;
      drop_until_ready = false;
    } else {
      normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

      // Compute which color render targets are used.
      normalized_color_mask =
          pixel_shader ? draw_util::GetNormalizedColorMask(
                             regs, pixel_shader->writes_color_targets())
                       : 0;
      apply_host_depth_polygon_offset =
          pixel_shader && !pixel_shader->writes_depth() &&
          render_target_cache_->GetPath() ==
              RenderTargetCache::Path::kHostRenderTargets &&
          draw_util::GetHostDepthPolygonOffsetIfNeeded(
              regs, primitive_polygonal, normalized_depth_control,
              normalized_color_mask, host_depth_polygon_offset);

      // Shader modifications.
      vertex_shader_modification =
          pipeline_cache_->GetCurrentVertexShaderModification(
              *vertex_shader, primitive_processing_result.host_vertex_shader_type,
              interpolator_mask, ps_param_gen_pos != UINT32_MAX);
      pixel_shader_modification =
          pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                             *pixel_shader, interpolator_mask, ps_param_gen_pos,
                             normalized_depth_control, normalized_color_mask,
                             apply_host_depth_polygon_offset)
                       : SpirvShaderTranslator::Modification(0);

      // Make the software-clipping decision once for the VS/PS pair. Reserved
      // register-count values identify static shaders; decode to zero allocation.
      if (pixel_shader_modification.pixel.dynamic_addressable_register_count >= 126) {
        const auto& v = vertex_shader_modification.vertex;
        if (!v.dynamic_addressable_register_count && !v.user_clip_plane_cull &&
            !v.vertex_kill_and && v.user_clip_plane_count &&
            v.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex) {
          vertex_shader_modification.vertex.dynamic_addressable_register_count = 255;
        } else {
          pixel_shader_modification.pixel.dynamic_addressable_register_count = 0;
        }
      }
      // vulkan_unclipped_fallback: without host clip / cull distances, a draw
      // that software clipping can't take (a pixel shader with dynamic register
      // addressing or too many interpolators, quad or rectangle lists, ...) was
      // rejected by ArePipelineRequirementsMet and dropped every frame (GTA IV).
      // Draw it without the user planes instead - extra geometry beyond a
      // reflection plane rather than missing geometry.
      if (draw_config_.vulkan_unclipped_fallback) {
        auto& v = vertex_shader_modification.vertex;
        const auto& props = GetVulkanDevice()->properties();
        if (v.user_clip_plane_count && v.dynamic_addressable_register_count != 255 &&
            (v.user_clip_plane_cull ? !props.shaderCullDistance
                                    : !props.shaderClipDistance)) {
          v.user_clip_plane_count = 0;
          v.user_clip_plane_cull = 0;
        }
      }

      // Translate the shaders now to obtain the sampler bindings.
      vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
          vertex_shader->GetOrCreateTranslation(
              vertex_shader_modification.value));
      pixel_shader_translation =
          pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                             pixel_shader->GetOrCreateTranslation(
                                 pixel_shader_modification.value))
                       : nullptr;
      // Decide whether the ucode interpreter can stand in for the real VS while
      // it translates+compiles in the background: a plain (non-tessellated,
      // non-expanded) vertex shader with static control flow and no texture fetch
      // or memory export, on a device matching the interpreter's assumptions
      // (single shared-memory binding, full 32-bit indices). When eligible, VS
      // translation is deferred to the background creation thread.
      use_interpreter =
          draw_config_.vulkan_placeholder_pipelines &&
          draw_config_.async_shader_vs_interpreter &&
          !vertex_shader_translation->is_translated() &&
          active_vertex_shader_ucode_address_ != 0 &&
          device_properties.fullDrawIndexUint32 &&
          SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
              device_properties.maxStorageBufferRange) == 0 &&
          primitive_processing_result.host_vertex_shader_type ==
              Shader::HostVertexShaderType::kVertex &&
          primitive_processing_result.host_primitive_type !=
              xenos::PrimitiveType::kPointList &&
          primitive_processing_result.host_primitive_type !=
              xenos::PrimitiveType::kRectangleList &&
          primitive_processing_result.host_primitive_type !=
              xenos::PrimitiveType::kQuadList &&
          vertex_shader->texture_bindings().empty() &&
          !vertex_shader->uses_subroutine_calls() &&
          vertex_shader->memexport_eM_written() == 0 &&
          vertex_shader->constant_register_map().loop_bitmap == 0;
      bool async_available =
          pipeline_cache_->CanCreatePipelineAsync(pixel_shader != nullptr);
      // A draw with no placeholder to render it now: the interpreter can't stand
      // in AND the real VS isn't translated yet (so no real-VS + no-op-PS
      // placeholder either). These are the draws async_shader_skip_draws governs.
      // Interpreter-eligible draws and draws whose VS is already translated
      // always have a placeholder and never translate on the draw thread.
      bool no_placeholder = draw_config_.vulkan_placeholder_pipelines &&
                            async_available && !use_interpreter &&
                            !vertex_shader_translation->is_translated();
      // The draw thread translates only for a no-placeholder draw that isn't
      // being skipped (so it can render via a real-VS placeholder).
      bool translate_here =
          !async_available || (no_placeholder && !draw_config_.async_shader_skip_draws) ||
          (!draw_config_.vulkan_placeholder_pipelines && !draw_config_.vulkan_async_skip_draws);
      // Strict draws consume binding metadata below. Finish translation before
      // taking that snapshot, so samplers and the final pipeline layout agree.
      if (translate_here) {
        if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                      pixel_shader_translation)) {
          return XdDrawFail(__LINE__);
        }
      }
      drop_until_ready = draw_config_.vulkan_placeholder_pipelines && no_placeholder &&
                         draw_config_.async_shader_skip_draws;
    }
    if (draw_config_.log_pixel_shader_paths) {
      // Index 8 = no pixel shader (depth-only or rasterization disabled).
      const uint32_t path =
          pixel_shader
              ? uint32_t(pixel_shader_modification.pixel.depth_stencil_mode)
              : 8;
      ++pixel_shader_path_draws_[path][normalized_depth_control.z_write_enable];
    }

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // A stage's
    // previous parameters and handles are reused when the binding list (the
    // shader) is the same and no sampler was destroyed since; parameters are
    // re-derived only for fetch slots written since the last draw, and
    // UseSampler (whose at-least-once-per-submission contract keeps handles
    // safe from the LRU eviction) is skipped when the stage's last full
    // UseSampler pass already happened in the current submission.
    uint32_t samplers_overflowed_count = 0;
    const bool sampler_cache_handles_valid =
        current_samplers_destroy_generation_ ==
            texture_cache_->sampler_destroy_generation();
    bool sampler_stage_validated[2] = {false, false};
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>&
          shader_samplers =
              j ? current_samplers_pixel_ : current_samplers_vertex_;
      const VulkanShader*& cached_samplers_shader =
          j ? current_samplers_shader_pixel_ : current_samplers_shader_vertex_;
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        // Downstream consumers (texture set hash, descriptor writes) iterate
        // the whole vector - it must match this draw's (empty) binding list.
        shader_samplers.clear();
        cached_samplers_shader = nullptr;
        continue;
      }
      // Don't read a shader's sampler bindings while a creation thread is still
      // populating them (async draws don't translate here). A placeholder draw
      // that ends up using this shader binds no samplers for it anyway.
      if (!i) {
        stage_bindings_ready[j] = shader->bindings_ready();
      }
      if (!stage_bindings_ready[j]) {
        if (!i) {
          shader_samplers.clear();
        }
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i && sampler_cache_handles_valid &&
          cached_samplers_shader == shader &&
          shader_samplers.size() == shader_sampler_bindings.size()) {
        // Cache hit for this stage.
        const uint32_t fetch_up_to_date =
            j ? current_samplers_fetch_up_to_date_pixel_
              : current_samplers_fetch_up_to_date_vertex_;
        const bool submission_changed =
            (j ? current_samplers_submission_pixel_
               : current_samplers_submission_vertex_) !=
            GetCurrentSubmission();
        if (!submission_changed &&
            (fetch_up_to_date & samplers_used_fetch_mask_[j]) == samplers_used_fetch_mask_[j]) {
          sampler_stage_validated[j] = true;
          continue;
        }
        ++sampler_binding_generation_[j];
        for (size_t k = 0; k < shader_sampler_bindings.size(); ++k) {
          std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
              shader_sampler_pair = shader_samplers[k];
          bool need_use_sampler =
              submission_changed ||
              shader_sampler_pair.second == VK_NULL_HANDLE;
          if (!(fetch_up_to_date &
                (uint32_t(1) << shader_sampler_bindings[k].fetch_constant))) {
            VulkanTextureCache::SamplerParameters parameters =
                texture_cache_->GetSamplerParameters(
                    shader_sampler_bindings[k]);
            if (parameters != shader_sampler_pair.first) {
              shader_sampler_pair.first = parameters;
              shader_sampler_pair.second = VK_NULL_HANDLE;
              need_use_sampler = true;
            }
          }
          if (need_use_sampler) {
            bool sampler_overflowed;
            VkSampler shader_sampler = texture_cache_->UseSampler(
                shader_sampler_pair.first, sampler_overflowed);
            shader_sampler_pair.second = shader_sampler;
            if (shader_sampler == VK_NULL_HANDLE) {
              if (!sampler_overflowed) {
                return XdDrawFail(__LINE__);
              }
              ++samplers_overflowed_count;
            }
          }
        }
        sampler_stage_validated[j] = true;
        continue;
      }
      ++sampler_binding_generation_[j];
      // Full pass: cache missed, or the i == 1 overflow retry
      // (where the parameters derived at i == 0 are kept, but every sampler
      // must be revalidated with UseSampler in the possibly new submission).
      if (!i) {
        shader_samplers.clear();
        shader_samplers.reserve(shader_sampler_bindings.size());
        samplers_used_fetch_mask_[j] = 0;
        for (const VulkanShader::SamplerBinding& shader_sampler_binding :
             shader_sampler_bindings) {
          samplers_used_fetch_mask_[j] |= UINT32_C(1) << shader_sampler_binding.fetch_constant;
          shader_samplers.emplace_back(
              texture_cache_->GetSamplerParameters(shader_sampler_binding),
              VK_NULL_HANDLE);
        }
      }
      // Record the binding-list identity before the UseSampler loop so a
      // mid-loop failure return can't leave the vector holding this shader's
      // parameters while the cached key still names the previous shader
      // (the stamps below are only committed on full success, so the next
      // draw re-derives dirty slots and UseSamplers null handles as needed).
      cached_samplers_shader = shader;
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
               shader_sampler_pair : shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler = texture_cache_->UseSampler(
            shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return XdDrawFail(__LINE__);
          }
          ++samplers_overflowed_count;
        }
      }
      sampler_stage_validated[j] = true;
    }
    if (!samplers_overflowed_count) {
      // The sampler pass completed with every needed UseSampler call made in
      // the current submission - commit the cache stamps. Only stages
      // actually validated this draw get their dirty masks cleared; a skipped
      // stage's cached parameters may still reference fetch slots that were
      // written and not re-derived.
      {
        const uint64_t submission_now = GetCurrentSubmission();
        if (sampler_stage_validated[0]) {
          current_samplers_fetch_up_to_date_vertex_ = ~uint32_t(0);
          current_samplers_submission_vertex_ = submission_now;
        }
        if (sampler_stage_validated[1]) {
          current_samplers_fetch_up_to_date_pixel_ = ~uint32_t(0);
          current_samplers_submission_pixel_ = submission_now;
        }
        current_samplers_destroy_generation_ =
            texture_cache_->sampler_destroy_generation();
      }
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(
            samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionCompletionAndDeviceLoss(sampler_overflow_await_submission);
  }

  // Unclipped ownership bounds depend on vertex data and the index offset.
  const bool refresh_unclipped_rt =
      is_rasterization_done &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kHostRenderTargets &&
      regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  const uint32_t previous_rt_height =
      refresh_unclipped_rt ? render_target_cache_->last_update_height_used()
                           : 0;
  if (refresh_unclipped_rt) {
    draw_dirty |= RegisterFile::kRenderTargets;
    fast = false;
    fast_miss = 7;
  }
  if (!fast) {
    // fast_miss 0 here = primitive shape differs.
    ++fast_path_miss_[fast_miss & 7];
  }
  if (use_interpreter) FlushDeferredResolve(ResolveDeferStop::kResource);
  FrameStatsPhase(1);
  // Ownership follows the render-target group and host lifetime epoch.
  const bool update_render_targets = bool(draw_dirty & RegisterFile::kRenderTargets);
  if (update_render_targets) {
    // Set up the render targets - this may perform dispatches and draws.
    render_target_cache_->SetUpdatePixelShader(pixel_shader);
    render_target_cache_->SetUpdateDrawMayBeSkipped(
        drop_until_ready && draw_config_.rt_cache_full_overwrite_await_pipeline);
    if (!render_target_cache_->Update(is_rasterization_done,
                                      normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      return XdDrawFail(__LINE__);
    }
    FrameStatsPhase(2);
    if (resolve_defer_census_.active &&
        (render_target_cache_->last_update_draw_replaced_by_clear() ||
         render_target_cache_->last_update_depth_replaced_by_clear() ||
         render_target_cache_->HasPendingDrawPassTransfers() ||
         render_target_cache_->HasPendingOutOfPassTransfers())) {
      resolve_defer_census_.reasons |= kResolveDeferUnknown;
    }
    if (refresh_unclipped_rt && draw_config_.log_draw_state_repeats &&
        render_target_cache_->last_update_height_used() > previous_rt_height) {
      static uint64_t last_extent_probe_frame = UINT64_MAX;
      if (last_extent_probe_frame != frame_current_) {
        last_extent_probe_frame = frame_current_;
        XELOGW("[FAST-RT-EXTENT] frame={} old_h={} new_h={} pending={}",
               frame_current_, previous_rt_height,
               render_target_cache_->last_update_height_used(),
               render_target_cache_->HasPendingOutOfPassTransfers() ||
                   render_target_cache_->HasPendingDrawPassTransfers());
      }
    }
    if (render_target_cache_->last_update_draw_replaced_by_clear()) {
      // The draw was a whole-range depth clear, already performed by the render
      // target cache as a clear of the render target holding the range.
      return true;
    }
    if (render_target_cache_->last_update_depth_replaced_by_clear()) {
      // The depth / stencil part of a clear was performed by the render target
      // cache, and the render targets are set up for color only.
      normalized_depth_control.z_enable = 0;
      normalized_depth_control.stencil_enable = 0;
    }
  }
  // rt_cache_transfer_known_constants: the render targets this draw may write
  // no longer hold what earlier clears left in them.
  render_target_cache_->NoteDrawWritesRenderTargets(normalized_depth_control,
                                                    normalized_color_mask);

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VulkanPipelineCache::Pipeline* pipeline = cached.pipeline;
  VulkanPipelineCache::Pipeline* prepass_pipeline = cached.prepass_pipeline;
  VulkanPipelineCache::Pipeline* prepass_color_pipeline =
      cached.prepass_color_pipeline;
  if (draw_dirty & (RegisterFile::kShader | RegisterFile::kPipeline |
                    RegisterFile::kRenderTargets)) {
    if (!pipeline_cache_->ConfigurePipeline(
            vertex_shader_translation, pixel_shader_translation,
            primitive_processing_result, normalized_depth_control,
            normalized_color_mask,
            render_target_cache_->last_update_render_pass_key(),
            use_interpreter, &pipeline)) {
      XELOGE("IssueDraw: ConfigurePipeline failed for VS={:016X} PS={:016X}",
             vertex_shader->ucode_data_hash(),
             pixel_shader ? pixel_shader->ucode_data_hash() : 0);
      return XdDrawFail(__LINE__);
    }
    prepass_pipeline = nullptr;
    prepass_color_pipeline = nullptr;
    if (draw_config_.vulkan_alpha_tested_depth_prepass && pixel_shader &&
        is_rasterization_done && !use_interpreter) {
      ConfigureAlphaTestedDepthPrepass(
          *vertex_shader, *pixel_shader, vertex_shader_translation,
          pixel_shader_modification, primitive_processing_result,
          interpolator_mask, ps_param_gen_pos, normalized_depth_control,
          normalized_color_mask, apply_host_depth_polygon_offset,
          &prepass_pipeline, &prepass_color_pipeline);
    }
  }
  // The cache keeps the plain pipeline: the split is chosen again per draw,
  // only once both of its pipelines are really created (never a placeholder,
  // which may run anything) and while no occlusion query is counting.
  VulkanPipelineCache::Pipeline* const plain_pipeline = pipeline;
  VkPipeline prepass_handle = VK_NULL_HANDLE;
  if (prepass_pipeline) {
    VkPipeline color_handle =
        prepass_color_pipeline->pipeline.load(std::memory_order_acquire);
    prepass_handle = prepass_pipeline->pipeline.load(std::memory_order_acquire);
    if (color_handle != VK_NULL_HANDLE && prepass_handle != VK_NULL_HANDLE &&
        color_handle != prepass_color_pipeline->placeholder_pipeline.load(
                            std::memory_order_acquire) &&
        prepass_handle != prepass_pipeline->placeholder_pipeline.load(
                              std::memory_order_acquire) &&
        prepass_pipeline->pipeline_layout.load(std::memory_order_acquire) ==
            prepass_color_pipeline->pipeline_layout.load(
                std::memory_order_acquire) &&
        !zpd_active_segment_.logical_active) {
      pipeline = prepass_color_pipeline;
      ++alpha_tested_prepass_draws_;
    } else {
      prepass_handle = VK_NULL_HANDLE;
      ++alpha_tested_prepass_unready_;
    }
  }

  if (drop_until_ready &&
      render_target_cache_->last_update_fully_overwritten_mask()) {
    static uint32_t full_overwrite_drops = 0;
    if (++full_overwrite_drops <= 32) {
      XELOGW("RTCLEAR: full-overwrite draw dropped until its pipeline is "
             "ready - previous render target contents lost ({})",
             full_overwrite_drops);
    }
  }
  if (drop_until_ready) {
    // Non-interpretable draw whose shaders weren't translated yet - it has been
    // queued for background creation; skip drawing it until the real pipeline
    // is ready (a later frame renders it). Never translate/compile on the draw
    // thread for these.
    XELOGI(
        "Draw skipped (no interpreter placeholder, real pipeline not ready): "
        "VS {:016X}, PS {:016X}",
        vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return true;
  }
  // If async mode is active, this may be a placeholder pipeline. The real
  // pipeline will be swapped in by the creation thread when ready.
  VkPipeline current_pipeline =
      pipeline->pipeline.load(std::memory_order_acquire);
  // The render target update dropped the previous contents of the targets
  // this draw was proven to overwrite entirely. A skipped or placeholder draw
  // would leave stale or garbage pixels there for good (feedback effects like
  // HDR exposure then never recover), so this rare draw waits for its real
  // pipeline instead.
  if (draw_config_.rt_cache_full_overwrite_await_pipeline &&
      render_target_cache_->last_update_fully_overwritten_mask() &&
      (current_pipeline == VK_NULL_HANDLE ||
       current_pipeline ==
           pipeline->placeholder_pipeline.load(std::memory_order_acquire))) {
    static uint32_t full_overwrite_awaits = 0;
    if (++full_overwrite_awaits <= 32) {
      XELOGI("RTCLEAR: full-overwrite draw awaits its pipeline ({}): VS "
             "{:016X}, PS {:016X}",
             full_overwrite_awaits, vertex_shader->ucode_data_hash(),
             pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    }
    pipeline_cache_->AwaitPipelineCompletion();
    current_pipeline = pipeline->pipeline.load(std::memory_order_acquire);
  }
  if (current_pipeline == VK_NULL_HANDLE &&
      !draw_config_.vulkan_placeholder_pipelines && !draw_config_.vulkan_async_skip_draws) {
    // Deferred commands snapshot the handle at record time. Waiting only at
    // EndSubmission cannot repair a recorded NULL handle; finish creation
    // before recording the draw and its descriptors.
    pipeline_cache_->AwaitPipelineCompletion();
    current_pipeline = pipeline->pipeline.load(std::memory_order_acquire);
    if (current_pipeline == VK_NULL_HANDLE) {
      XELOGE("Strict draw: pipeline creation failed for VS={:016X} PS={:016X}",
             vertex_shader->ucode_data_hash(),
             pixel_shader ? pixel_shader->ucode_data_hash() : 0);
      return XdDrawFail(__LINE__);
    }
  }
  if (rt_trace_active_) {
    RtTraceRecordDraw(prim_type, index_count,
                      current_pipeline == VK_NULL_HANDLE,
                      normalized_depth_control, normalized_color_mask,
                      is_rasterization_done);
  }
  if (draw_config_.vulkan_placeholder_pipelines &&
      current_pipeline == VK_NULL_HANDLE) {
    // Placeholder mode: real pipeline not created yet and no placeholder -
    // skip this draw rather than stalling the draw thread.
    XELOGI("Draw skipped (pipeline not ready yet): VS {:016X}, PS {:016X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return true;
  }
  // Deferred-bind mode (cvar off): current_pipeline may legitimately be
  // VK_NULL_HANDLE here - fall through and record a deferred bind of the
  // stable slot pointer; EndSubmission waits (or replay drops it under
  // vulkan_async_skip_draws). pipeline_layout is never null (the slot was
  // created with at least the minimal layout).
  // The interpreter reads the guest ucode from shared memory by its program
  // address. A cached interpreter placeholder reused for an inline
  // (IM_LOAD_IMMEDIATE, address 0) shader can't be fed, so skip until the real
  // pipeline is ready rather than interpret from address 0.
  if (active_vertex_shader_ucode_address_ == 0 &&
      current_pipeline ==
          pipeline->placeholder_pipeline.load(std::memory_order_acquire) &&
      pipeline->uses_interpreter.load(std::memory_order_acquire)) {
    return true;
  }

  // Push debug marker with Xbox 360 draw context for RenderDoc annotation.
  // Done early so texture loads appear nested under the draw that uses them.
  if (debug_markers_enabled_) {
    char label[draw_util::kDebugMarkerLabelMaxLength];
    draw_util::FormatDrawDebugMarker(
        label, sizeof(label), prim_type, primitive_processing_result,
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    PushDebugMarker("%s", label);
  }

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  // Only read after-translation state for stages ready at the draw's snapshot.
  uint32_t used_texture_mask =
      (stage_bindings_ready[0]
           ? vertex_shader->GetUsedTextureMaskAfterTranslation()
           : 0) |
      (pixel_shader != nullptr && stage_bindings_ready[1]
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  FrameStatsPhase(3);
  // Same fetch constants, same submission and no texture memory written since
  // the previous draw requested them: the bindings, loads and usage
  // transitions are all still current.
  const bool textures_requested =
      bool(fetch_slots_written & used_texture_mask) ||
      cached.epoch != draw_state_epoch_ || cached.submission != GetCurrentSubmission() ||
      used_texture_mask != cached.used_texture_mask ||
      texture_cache_->texture_became_outdated();
  const uint32_t vertex_texture_mask =
      stage_bindings_ready[0]
          ? vertex_shader->GetUsedTextureMaskAfterTranslation()
          : 0;
  if (textures_requested) {
    texture_cache_->RequestTextures(used_texture_mask, vertex_texture_mask);
  } else {
    texture_cache_->EnsureTexturesSampled(used_texture_mask,
                                          vertex_texture_mask);
  }
  FrameStatsPhase(4);

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  // With asynchronous creation the handle may still be VK_NULL_HANDLE here -
  // a deferred bind of the stable slot pointer is recorded instead of
  // stalling. The pipeline cache's EndSubmission at the submission boundary
  // either waits for creation to complete (default) or lets the replay drop
  // the draws of still-unready pipelines (vulkan_async_skip_draws). The
  // de-dup compares slot POINTERS, not handle values.
  // Internal transfer draws may require this binding again later.
  BindDrawPipeline(pipeline, current_pipeline);

  bool host_render_targets_used = render_target_cache_->GetPath() ==
                                  RenderTargetCache::Path::kHostRenderTargets;

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info = cached.viewport;

  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.

  // Resolution scale of this draw, which may be 1x1 because of
  // draw_resolution_scale_threshold, which the divisors have to match too.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();
  // ZPD segments can't mix scales. The resolved sample count is divided by
  // one scale area per segment. Split before the FSI counter index goes
  // into system constants.
  uint32_t zpd_scale_area =
      draw_resolution_scale_x * draw_resolution_scale_y;
  if (depth_4x_as_1x) {
    // The scale of four represents four guest samples, not four extra guest
    // pixels. Keep the raw coverage count for query normalization.
    zpd_scale_area >>= 2;
  }
  UpdateZPDScale(std::max(zpd_scale_area, 1u));
  const bool raster_changed = update_render_targets ||
      (draw_dirty & RegisterFile::kRaster) ||
      cached.scale_x != draw_resolution_scale_x || cached.scale_y != draw_resolution_scale_y;
  if (raster_changed) {
    draw_util::GetViewportInfoArgs gviargs{};
    gviargs.Setup(
        draw_resolution_scale_x, draw_resolution_scale_y,
        draw_resolution_scale_x > 1
            ? (depth_4x_as_1x
                   ? divisors::MagicDiv(draw_resolution_scale_x)
                   : texture_cache_->draw_resolution_scale_x_divisor())
            : divisors::MagicDiv(1),
        draw_resolution_scale_y > 1
            ? (depth_4x_as_1x
                   ? divisors::MagicDiv(draw_resolution_scale_y)
                   : texture_cache_->draw_resolution_scale_y_divisor())
            : divisors::MagicDiv(1),
        false, device_properties.maxViewportDimensions[0],
        device_properties.maxViewportDimensions[1],
        draw_config_.vulkan_allow_reverse_z, normalized_depth_control,
        host_render_targets_used &&
            render_target_cache_->depth_float24_convert_in_pixel_shader(),
        host_render_targets_used, pixel_shader && pixel_shader->writes_depth());
    gviargs.SetupRegisterValues(regs);

    draw_util::GetHostViewportInfo(&gviargs, viewport_info);
    cached.viewport = viewport_info;
    cached.scale_x = draw_resolution_scale_x;
    cached.scale_y = draw_resolution_scale_y;
  }
  // The shading rate of this draw (see vulkan_fragment_shading_rate).
  draw_fragment_shading_rate_ = 0x0101;
  const int32_t fragment_shading_rate_setting =
      std::max({draw_config_.vulkan_fragment_shading_rate,
                int32_t(thermal_governor_rate_),
                int32_t(adaptive_shading_rate_)});
  if (msaa_2x2_pixel_rate_ && render_target_cache_->draw_samples_as_pixels() &&
      pixel_shader &&
      SpirvShaderTranslator::IsSamplesAsPixelsCoarseAllowed(
          pixel_shader_modification, pixel_shader->writes_depth())) {
    // Matches SpirvShaderTranslator::IsDrawSamplesAsPixelsCoarse.
    draw_fragment_shading_rate_ = 0x0202;
  } else if (fragment_shading_rate_setting > 0 && host_render_targets_used &&
      normalized_depth_control.z_enable && pixel_shader &&
      !pixel_shader->writes_depth()) {
    bool coverage_from_shader = pixel_shader->kills_pixels();
    if (!coverage_from_shader) {
      auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
      coverage_from_shader =
          rb_colorcontrol.alpha_to_mask_enable ||
          (rb_colorcontrol.alpha_test_enable &&
           rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways);
    }
    if (!coverage_from_shader ||
        draw_config_.vulkan_fragment_shading_rate_alpha_tested) {
      switch (fragment_shading_rate_setting) {
        case 1:
          draw_fragment_shading_rate_ = 0x0201;
          break;
        case 2:
          draw_fragment_shading_rate_ = 0x0102;
          break;
        default:
          draw_fragment_shading_rate_ = 0x0202;
          break;
      }
    }
  }
  // Emission is invalidated separately by external draws and submission opens.
  draw_dynamic_state_dirty_ |= raster_changed ||
      bool(draw_dirty & RegisterFile::kPipeline) ||
      dynamic_fragment_shading_rate_ != draw_fragment_shading_rate_;
  UpdateDynamicState(viewport_info, primitive_polygonal,
                     normalized_depth_control, draw_resolution_scale_x,
                     draw_resolution_scale_y, apply_host_depth_polygon_offset,
                     pipeline->dynamic_state);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kVertex;

  // Draw operands are patched every time; the register-derived image only
  // changes with its dependency group, textures, scale or query slot.
  draw_system_state_dirty_ |= raster_changed || textures_requested ||
      bool(draw_dirty & (RegisterFile::kSystem | RegisterFile::kShader)) ||
      zpd_fsi_counter_index_force_update_;
  UpdateSystemConstantValues(
      primitive_polygonal, primitive_processing_result, shader_32bit_index_dma,
      viewport_info, used_texture_mask, normalized_depth_control,
      normalized_color_mask,
      apply_host_depth_polygon_offset ? &host_depth_polygon_offset : nullptr);

  // Whether we bound the placeholder pipeline (vs the real one). Derived from
  // the handle actually bound, so it's consistent with current_pipeline even if
  // the creation thread swaps in the real pipeline mid-draw (the is_placeholder
  // flag is cleared a few instructions later, so reading it separately can
  // disagree).
  bool bound_is_placeholder =
      current_pipeline ==
      pipeline->placeholder_pipeline.load(std::memory_order_acquire);
  // The interpreter placeholder (interpreter VS + no-op PS) also needs its
  // ucode location + full float constants fed to it.
  bool interpreter_placeholder =
      bound_is_placeholder &&
      pipeline->uses_interpreter.load(std::memory_order_acquire);
  // Any placeholder draw binds the no-op placeholder pixel shader (which never
  // samples), so its pixel textures/samplers must not be bound - the
  // placeholder pipeline's layout has none.
  bool placeholder_pixel_shader = bound_is_placeholder;
  {
    uint32_t ucode_base_dwords = 0, cf_instr_count = 0;
    if (interpreter_placeholder) {
      uint32_t ucode_address = active_vertex_shader_ucode_address_;
      ucode_base_dwords = ucode_address >> 2;
      cf_instr_count = vertex_shader->cf_pair_index_bound() * 2;
      shared_memory_->RequestRange(
          ucode_address,
          uint32_t(vertex_shader->ucode_dword_count()) * sizeof(uint32_t));
    }
    if (system_constants_.interpreter_ucode_base_dwords != ucode_base_dwords ||
        system_constants_.interpreter_cf_instr_count != cf_instr_count) {
      system_constants_.interpreter_ucode_base_dwords = ucode_base_dwords;
      system_constants_.interpreter_cf_instr_count = cf_instr_count;
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
    }
  }

  // Two-buffer memexport routing: producer and geometry-consumer draws use the
  // host-imported buffer (aliasing guest RAM) so memexport output stays CPU
  // coherent (fixing the page false-sharing clobber) and consumers read it
  // directly. Only texture-sampled ranges are copied into the device buffer
  // (EnsureMemexportRangeInDeviceBuffer). Inert without the host buffer.
  bool route_to_host = false;
  if (shared_memory_host_and_edram_descriptor_set_ != VK_NULL_HANDLE) {
    route_to_host =
        memexport_used_vertex || memexport_used_pixel ||
        (any_memexport_pages_written_ &&
         ((primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
           IsMemexportRange(
               primitive_processing_result.guest_index_base,
               primitive_processing_result.guest_draw_vertex_count *
                   uint32_t(sizeof(uint32_t)))) ||
          VertexFetchInMemexportRange(regs, *vertex_shader)));
  }
  {
    // Select this draw's shared-memory set, forcing a re-bind only if it
    // changed. Both sets are pre-written, so the values-up-to-date bits stay
    // set (never cleared here - the flush asserts on them); only the bound-set
    // tracking is touched.
    VkDescriptorSet shared_memory_set =
        route_to_host ? shared_memory_host_and_edram_descriptor_set_
                      : shared_memory_and_edram_descriptor_set_;
    if (shared_memory_set == VK_NULL_HANDLE) {
      XELOGE("Failed to bind the current shared-memory descriptor");
      return XdDrawFail(__LINE__);
    }
    if (current_graphics_descriptor_sets_
            [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] !=
        shared_memory_set) {
      current_graphics_descriptor_sets_
          [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
              shared_memory_set;
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          ~(UINT32_C(1)
            << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram);
    }
  }

  FrameStatsPhase(5);
  // In-pass vertex fetch window: vertex data the CPU rewrote after an earlier
  // draw of the open render pass read it can't be uploaded without ending the
  // pass (a full tile store and reload on Mali). The rewritten pages are copied
  // into the ring past the guest memory, and this draw's vertex fetches read
  // that span from the copy (system constants window), the rest as usual.
  vfetch_ring_patched_mask_[0] = 0;
  vfetch_ring_patched_mask_[1] = 0;
  vfetch_ring_patched_mask_[2] = 0;
  uint32_t vfetch_window_start = 0, vfetch_window_size = 0;
  uint32_t vfetch_window_delta = 0;
  if (shared_memory_->vfetch_ring_size() && in_render_pass_ &&
      !interpreter_placeholder && memexport_ranges_.empty() &&
      shared_memory_->GetShaderBufferCount() == 1) {
    const Shader::ConstantRegisterMap& window_constant_map =
        vertex_shader->constant_register_map();
    for (uint32_t i = 0;
         i < xe::countof(window_constant_map.vertex_fetch_bitmap); ++i) {
      uint32_t window_bits_remaining =
          window_constant_map.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(window_bits_remaining, &j)) {
        window_bits_remaining = xe::clear_lowest_bit(window_bits_remaining);
        xenos::xe_gpu_vertex_fetch_t vfetch_constant =
            regs.GetVertexFetch(i * 32 + j);
        if (vfetch_constant.type != xenos::FetchConstantType::kVertex ||
            !vfetch_constant.size) {
          continue;
        }
        uint32_t span_start, span_length;
        if (!shared_memory_->RangeUploadWouldBreakPass(
                vfetch_constant.address << 2, vfetch_constant.size << 2,
                span_start, span_length)) {
          continue;
        }
        if (!vfetch_window_size) {
          uint32_t ring_address =
              shared_memory_->CopyRangeToVfetchRing(span_start, span_length);
          if (ring_address == UINT32_MAX) {
            ++vfetch_window_copy_failed_;
            continue;
          }
          vfetch_window_start = span_start >> 2;
          vfetch_window_size = span_length >> 2;
          vfetch_window_delta = (ring_address - span_start) >> 2;
        } else if (span_start < (vfetch_window_start << 2) ||
                   span_start + span_length >
                       ((vfetch_window_start + vfetch_window_size) << 2)) {
          // Would need a second window - this one ends the pass as before.
          ++vfetch_window_second_;
          continue;
        }
        ++vfetch_window_accepted_;
        vfetch_ring_patched_mask_[i] |= UINT32_C(1) << j;
      }
    }
  }
  if (system_constants_.vfetch_window_start != vfetch_window_start ||
      system_constants_.vfetch_window_size != vfetch_window_size ||
      system_constants_.vfetch_window_delta != vfetch_window_delta) {
    system_constants_.vfetch_window_start = vfetch_window_start;
    system_constants_.vfetch_window_size = vfetch_window_size;
    system_constants_.vfetch_window_delta = vfetch_window_delta;
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader, stage_bindings_ready[0],
                      stage_bindings_ready[1], interpreter_placeholder,
                      placeholder_pixel_shader)) {
    return XdDrawFail(__LINE__);
  }
  FrameStatsPhase(10);

  // Cached address plan; memory validity and usage lifetime are independent
  // from fetch register identity. RequestRange remains the residency authority.
  if (!UpdateVertexInputRanges(*vertex_shader)) return XdDrawFail(__LINE__);
  for (uint32_t i = 0; i < vertex_input_count_; ++i) {
    auto& range = vertex_input_ranges_[i];
    if (vfetch_ring_patched_mask_[range.slot >> 5] &
        (UINT32_C(1) << (range.slot & 31))) {
      shared_memory_->MarkRangeUsedInSubmission(range.memory.address, range.memory.length);
    } else if (!range.memory.Request(*shared_memory_)) {
      return XdDrawFail(__LINE__);
    }
  }

  FrameStatsPhase(11);
  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords
                                          << 2;
    // Host-routed producers write output to host_buffer_ (guest RAM), not the
    // device buffer, so this upload is redundant. It also drops the draw when
    // the guest committed only part of the declared capacity, so skip it.
    if (!route_to_host &&
        !shared_memory_->RequestRange(memexport_range_base_bytes,
                                      memexport_range.size_bytes)) {
      XELOGE(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return XdDrawFail(__LINE__);
    }
    memexport_extent_start =
        std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end,
                 memexport_range_base_bytes + memexport_range.size_bytes);
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start,
                       memexport_extent_end - memexport_extent_start));
  } else {
    // With in-pass resolves, fragment shaders may write shared memory inside
    // any guest pass - declare the write usage up front so no barrier is
    // needed at the resolve point.
    shared_memory_->Use(
        (render_target_cache_->local_read_attachments() &&
         !draw_config_.vulkan_in_pass_resolve_debug_read_usage)
            ? VulkanSharedMemory::Usage::kGuestDrawReadWrite
            : VulkanSharedMemory::Usage::kRead);
  }
  if (shared_memory_->vertex_read_debt()) {
    // Shared memory reads of the vertex stages: vertex fetch and guest DMA
    // indices (also read by the vertex shader for 32-bit or tessellation).
    std::pair<uint32_t, uint32_t>
        vertex_reads[xe::countof(vertex_input_ranges_) + 1];
    size_t vertex_read_count = 0;
    for (uint32_t i = 0; i < vertex_input_count_; ++i) {
      vertex_reads[vertex_read_count++] = std::make_pair(
          vertex_input_ranges_[i].memory.address,
          vertex_input_ranges_[i].memory.length);
    }
    if (primitive_processing_result.index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
      vertex_reads[vertex_read_count++] = std::make_pair(
          primitive_processing_result.guest_index_base,
          primitive_processing_result.guest_draw_vertex_count *
              uint32_t(sizeof(uint32_t)));
    }
    shared_memory_->SettleVertexReads(vertex_reads, vertex_read_count);
  }
  if (pixel_shader && !pixel_shader->vertex_bindings().empty()) {
    shared_memory_->SettleFragmentReads();
  }
  if (render_target_cache_->local_read_attachments() ||
      (pixel_shader && (pixel_shader->memexport_eM_written() ||
                        !pixel_shader->vertex_bindings().empty()))) {
    fragment_shared_memory_submission_ = GetCurrentSubmission();
  }

  if (memexport_vs_compute) {
    VkPipelineLayout memexport_pipeline_layout =
        current_guest_graphics_pipeline_layout_->GetPipelineLayout();
    VkPipeline memexport_pipeline = GetMemExportComputePipeline(
        *vertex_shader, vertex_shader_modification, memexport_pipeline_layout,
        false);
    bool memexport_vs_textures =
        !vertex_shader->GetTextureBindingsAfterTranslation().empty() ||
        !vertex_shader->GetSamplerBindingsAfterTranslation().empty();
    if (memexport_pipeline != VK_NULL_HANDLE &&
        (!memexport_vs_textures || stage_bindings_ready[0])) {
      uint32_t memexport_set_count_batch =
          memexport_vs_textures
              ? SpirvShaderTranslator::kDescriptorSetTexturesVertex + 1
              : SpirvShaderTranslator::kDescriptorSetConstants + 1;
      // A draw that only exports joins the pending batch of its shader; the
      // batch is dispatched before any other GPU work (SubmitBarriers).
      VkPipeline memexport_batch_pipeline = VK_NULL_HANDLE;
      if (cvars::vulkan_memexport_batch && memexport_batch_supported_ &&
          !is_rasterization_done && !route_to_host &&
          draw_config_.vulkan_memexport_compute_barrier &&
          !draw_config_.log_texture_clobber) {
        memexport_batch_pipeline = GetMemExportComputePipeline(
            *vertex_shader, vertex_shader_modification,
            memexport_pipeline_layout, true);
      }
      if (memexport_batch_pipeline != VK_NULL_HANDLE) {
        AddToMemExportBatch(
            memexport_batch_pipeline, memexport_pipeline_layout,
            memexport_set_count_batch,
            vertex_shader->constant_register_map().float_count, index_count);
      } else {
        // Outside the render pass, after the shared memory barrier above.
        // Usage tracking inserts no barrier between two exports that both keep
        // kGuestDrawReadWrite, yet one export may read what an earlier one wrote
        // (streamed texture data decoded in stages) - order them explicitly.
        if (draw_config_.vulkan_memexport_compute_barrier) {
          PushMemexportBeforeBarrier();
        }
        SubmitBarriers(true);
        EndRenderPass();
        BindExternalComputePipeline(memexport_pipeline);
        uint32_t memexport_set_count =
            memexport_vs_textures
                ? SpirvShaderTranslator::kDescriptorSetTexturesVertex + 1
                : SpirvShaderTranslator::kDescriptorSetConstants + 1;
        uint32_t memexport_dynamic_offsets
            [SpirvShaderTranslator::kConstantBufferCount];
        uint32_t memexport_dynamic_offset_count = 0;
        if (use_dynamic_constants_) {
          for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
               ++i) {
            memexport_dynamic_offsets[i] =
                uint32_t(current_constant_buffer_infos_[i].offset);
          }
          memexport_dynamic_offset_count =
              SpirvShaderTranslator::kConstantBufferCount;
        }
        deferred_command_buffer_->CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, memexport_pipeline_layout, 0,
            memexport_set_count, current_graphics_descriptor_sets_,
            memexport_dynamic_offset_count, memexport_dynamic_offsets);
        // One-run probe: identical GPU state and bindings, restore A for the game.
        static uint32_t bo_probes = 32;  // Disable the old draining A/B probe.
        if (draw_config_.log_texture_clobber &&
            vertex_shader->ucode_data_hash() == UINT64_C(0xA4827826E2589916) &&
            bo_probes < 32 && !is_rasterization_done && !memexport_vs_textures &&
            !route_to_host &&
            !shared_memory_->is_zero_copy() &&
            memexport_extent_start < memexport_extent_end &&
            memexport_extent_end <= SharedMemory::kBufferSize &&
            memexport_extent_end - memexport_extent_start <= (1u << 20)) {
          const auto* vd = GetVulkanDevice();
          const auto& vf = vd->functions();
          VkDevice dev = vd->device();
          VkBuffer probe = VK_NULL_HANDLE;
          VkDeviceMemory probe_memory = VK_NULL_HANDLE;
          uint32_t start = memexport_extent_start;
          VkDeviceSize n = memexport_extent_end - start;
          if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
                  vd, 3 * n,
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  ui::vulkan::util::MemoryPurpose::kReadback,
                  probe, probe_memory)) {
            return XdDrawFail(__LINE__);
          }
          ++bo_probes;
          const uint32_t base = regs.Get<reg::SQ_VS_CONST>().base;
          for (uint32_t c : vertex_shader->memexport_stream_constants()) {
            auto s = regs.GetMemExportStream(base + c);
            XELOGI("BO-PROBE stream c={} base={:08X} count={} format={} endian={} "
                   "num={} raw={:08X}/{:08X}/{:08X}/{:08X}",
                   c, uint32_t(s.base_address) << 2, uint32_t(s.index_count),
                   uint32_t(s.format), uint32_t(s.endianness), uint32_t(s.num_format),
                   s.dword_0, s.dword_1, s.dword_2, s.dword_3);
          }
          const auto& cm = vertex_shader->constant_register_map();
          for (uint32_t i = 0; i < xe::countof(cm.vertex_fetch_bitmap); ++i) {
            uint32_t bits = cm.vertex_fetch_bitmap[i], j;
            while (xe::bit_scan_forward(bits, &j)) {
              bits = xe::clear_lowest_bit(bits);
              auto f = regs.GetVertexFetch(i * 32 + j);
              uint64_t lo = uint64_t(f.address) << 2;
              uint64_t hi = lo + (uint64_t(f.size) << 2);
              XELOGI("BO-PROBE fetch={} begin={:08X} end={:08X} overlap={}",
                     i * 32 + j, lo, hi,
                     lo < memexport_extent_end && hi > start);
            }
          }
          XELOGI("BO-PROBE sys count={} base={} flags={:08X} offsets={}/{}/{}/{}/{}",
                 system_constants_.vertex_index_count, system_constants_.vertex_base_index,
                 system_constants_.flags,
                 current_constant_buffer_infos_[0].offset,
                 current_constant_buffer_infos_[1].offset,
                 current_constant_buffer_infos_[2].offset,
                 current_constant_buffer_infos_[3].offset,
                 current_constant_buffer_infos_[4].offset);
          auto order = [&]() {
            VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                              VK_ACCESS_HOST_READ_BIT;
            deferred_command_buffer_->CmdVkPipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &b, 0, nullptr, 0, nullptr);
          };
          auto copy = [&](VkBuffer src, VkBuffer dst, VkDeviceSize from,
                          VkDeviceSize to) {
            VkBufferCopy r = {from, to, n};
            deferred_command_buffer_->CmdVkCopyBuffer(src, dst, 1, &r);
            order();
          };
          auto dispatch = [&]() {
            deferred_command_buffer_->CmdVkDispatch(
                (index_count + SpirvShaderTranslator::kMemExportComputeGroupSize - 1) /
                    SpirvShaderTranslator::kMemExportComputeGroupSize, 1, 1,
                DeferredCommandBuffer::DispatchSite::kMemExport);
            order();
          };
          order();
          copy(shared_memory_->buffer(), probe, start, 0);     // initial
          dispatch();
          copy(shared_memory_->buffer(), probe, start, n);     // A
          copy(probe, shared_memory_->buffer(), 0, start);     // restore initial
          dispatch();
          copy(shared_memory_->buffer(), probe, start, 2 * n); // B
          copy(probe, shared_memory_->buffer(), n, start);     // retain A
          PopDebugMarker();
          for (const auto& r : memexport_ranges_) {
            shared_memory_->RangeWrittenByGpu(r.base_address_dwords << 2,
                                              r.size_bytes, true);
          }
          QueueMemexportRamMirror(false);
          if (!AwaitAllQueueOperationsCompletion()) {
            // Do not free a buffer whose commands may still be in flight.
            return XdDrawFail(__LINE__);
          }
          void* mapped = nullptr;
          VkResult mr = vf.vkMapMemory(dev, probe_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
          if (mr == VK_SUCCESS) {
            VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            r.memory = probe_memory;
            r.size = VK_WHOLE_SIZE;
            mr = vf.vkInvalidateMappedMemoryRanges(dev, 1, &r);
            if (mr == VK_SUCCESS) {
              const auto* a = static_cast<const uint8_t*>(mapped) + n;
              const auto* b = a + n;
              uint32_t changed = 0, first = UINT32_MAX;
              for (uint32_t i = 0; i < uint32_t(n); ++i) {
                if (a[i] != b[i]) {
                  ++changed;
                  first = std::min(first, i);
                }
              }
              XELOGI("BO-PROBE n={} range={:08X}+{} A={:016X} B={:016X} "
                     "changed={} first={:08X}",
                     bo_probes, start, n, XXH3_64bits(a, size_t(n)),
                     XXH3_64bits(b, size_t(n)), changed, first);
            }
            vf.vkUnmapMemory(dev, probe_memory);
          }
          vf.vkDestroyBuffer(dev, probe, nullptr);
          vf.vkFreeMemory(dev, probe_memory, nullptr);
          return mr == VK_SUCCESS ? true : XdDrawFail(__LINE__);
        }
        if (draw_config_.log_texture_clobber) {
          // Does the export overwrite what its own vertex fetches read (an
          // in-place transform)? Parallel compute invocations would then race,
          // unlike the guest's in-order vertex processing.
          static uint32_t overlap_logged = 0;
          const auto& export_map = vertex_shader->constant_register_map();
          for (uint32_t i = 0;
               overlap_logged < 64 && i < xe::countof(export_map.vertex_fetch_bitmap);
               ++i) {
            uint32_t bits = export_map.vertex_fetch_bitmap[i], j;
            while (overlap_logged < 64 && xe::bit_scan_forward(bits, &j)) {
              bits = xe::clear_lowest_bit(bits);
              xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(i * 32 + j);
              const uint64_t fetch_begin = uint64_t(f.address) << 2;
              const uint64_t fetch_end = fetch_begin + (uint64_t(f.size) << 2);
              for (const draw_util::MemExportRange& r : memexport_ranges_) {
                const uint64_t out_begin = uint64_t(r.base_address_dwords) << 2;
                const uint64_t out_end = out_begin + r.size_bytes;
                ++overlap_logged;
                XELOGI("[MEMEXPORT] vs {:016X} fetch {} {:08X}-{:08X} out "
                       "{:08X}-{:08X} overlap={} count={}",
                       vertex_shader->ucode_data_hash(), i * 32 + j, fetch_begin,
                       fetch_end, out_begin, out_end,
                       fetch_begin < out_end && fetch_end > out_begin,
                       index_count);
              }
            }
          }
        }
        uint32_t memexport_vertex_count = index_count;
        if (cvars::log_memexport_dispatches > 0) {
          --cvars::log_memexport_dispatches;
          std::string reads, writes;
          for (uint32_t i = 0; i < vertex_input_count_; ++i) {
            const DrawMemoryRange& m = vertex_input_ranges_[i].memory;
            reads += fmt::format(" {:08X}+{:X}", m.address, m.length);
          }
          for (const draw_util::MemExportRange& r : memexport_ranges_) {
            writes += fmt::format(" {:08X}+{:X}", r.base_address_dwords << 2,
                                  r.size_bytes);
          }
          XELOGI("MemexportTrace: f={} vs={:016X} mod={:X} n={} tex={} raster={} "
                 "reads{} | writes{}",
                 frame_current_, vertex_shader->ucode_data_hash(),
                 vertex_shader_modification.value, index_count,
                 uint32_t(memexport_vs_textures),
                 uint32_t(is_rasterization_done), reads, writes);
        }
        if (draw_config_.log_texture_clobber) {
          static uint32_t logged = 0;
          for (const auto& r : memexport_ranges_) {
            if (logged++ >= 4096) break;
            XELOGI("[POOL-E] s={} cb={:X} pos={} vs={:016X} dst={:08X}+{}",
                   GetCurrentSubmission(),
                   reinterpret_cast<uintptr_t>(deferred_command_buffer_),
                   deferred_command_buffer_->size_elements(),
                   vertex_shader->ucode_data_hash(), r.base_address_dwords << 2,
                   r.size_bytes);
          }
        }
        deferred_command_buffer_->CmdVkDispatch(
            (memexport_vertex_count +
             (SpirvShaderTranslator::kMemExportComputeGroupSize - 1)) /
                SpirvShaderTranslator::kMemExportComputeGroupSize,
            1, 1, DeferredCommandBuffer::DispatchSite::kMemExport);
        // Following reads of the exported data wait via the shared memory usage
        // tracking - kGuestDrawReadWrite includes the compute stage here. That
        // tracking doesn't order a texture load (compute) recorded while the
        // usage is still kGuestDrawReadWrite, so make the export visible to
        // every later shared memory read explicitly.
        if (draw_config_.vulkan_memexport_compute_barrier) {
          PushMemexportAfterBarrier();
        }
      }
    } else {
      static uint32_t memexport_compute_failures_logged = 0;
      if (memexport_compute_failures_logged < 16) {
        ++memexport_compute_failures_logged;
        XELOGW(
            "Memexport compute: skipped VS {:016X} (pipeline={}, textures={}, "
            "bindings_ready={})",
            vertex_shader->ucode_data_hash(),
            memexport_pipeline != VK_NULL_HANDLE, memexport_vs_textures,
            stage_bindings_ready[0]);
      }
    }
    if (!is_rasterization_done) {
      // Nothing to rasterize - the export was the whole draw.
      PopDebugMarker();
      for (const draw_util::MemExportRange& memexport_range :
           memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(
            memexport_range.base_address_dwords << 2,
            memexport_range.size_bytes, !route_to_host);
      }
      QueueMemexportRamMirror(route_to_host);
      return true;
    }
  }

  FrameStatsPhase(6);
  // Whether the guest draw below really writes its pixels - only then may the
  // render target transfers skip the rectangle it overwrites
  // (rt_cache_draw_overwrite_cutouts). Draws recorded without the real
  // pipeline are dropped at replay or use a placeholder.
  const bool draw_will_execute =
      is_rasterization_done && current_pipeline != VK_NULL_HANDLE &&
      current_pipeline !=
          pipeline->placeholder_pipeline.load(std::memory_order_acquire);
  // A clear draw chain only keeps the rectangle if the draw writes it.
  if (!draw_will_execute && rt_trace_active_) {
    XELOGI("RTTRACE draw_not_executing rast={} pipeline_null={}",
           uint32_t(is_rasterization_done),
           uint32_t(current_pipeline == VK_NULL_HANDLE));
  }
  render_target_cache_->ConfirmDrawClearChain(draw_will_execute);
  // Transfer draws (in or out of the guest pass) replace the guest pipeline,
  // dynamic state and bindings - remember to restore them before the draw.
  const bool restore_guest_state_after_transfers =
      depth_materialization_epoch != depth_materialization_epoch_ ||
      render_target_cache_->HasPendingOutOfPassTransfers() ||
      render_target_cache_->HasPendingDrawPassTransfers() ||
      render_target_cache_->HasDeferredClearForPass();
  // Cutout transfers that can't be done in the guest pass - before entering it.
  if (render_target_cache_->HasPendingOutOfPassTransfers()) {
    render_target_cache_->PerformPendingOutOfPassTransfers(draw_will_execute);
  }
  // After all commands that may dispatch, copy or insert barriers, submit the
  // barriers (may end the render pass), and (re)enter the render pass before
  // drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());
  // rt_cache_defer_retarget_clears: the depth clear (and the transfers around
  // it) the previous clear draw left for this pass - first thing in it, so a
  // full clear can become the pass's load op.
  if (render_target_cache_->HasDeferredClearForPass()) {
    render_target_cache_->EncodeDeferredClear();
    // Re-enters only if the clear had to end the pass after all.
    SubmitBarriersAndEnterRenderTargetCacheRenderPass(
        render_target_cache_->last_update_render_pass(),
        render_target_cache_->last_update_framebuffer());
  }

  // Encode render-target transfers that the render target cache queued for
  // execution inside this draw's render pass (avoids breaking the pass on
  // tile-based GPUs). If they can't be merged into the active pass, fall back
  // to performing them in a separate pass and re-enter the guest pass. Either
  // way the transfers change pipeline / dynamic / binding state, so re-emit it
  // before the actual guest draw below.
  if (render_target_cache_->HasPendingDrawPassTransfers()) {
    if (!render_target_cache_->EncodePendingDrawPassTransfers(
            draw_will_execute)) {
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        return XdDrawFail(__LINE__);
      }
      SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          render_target_cache_->last_update_render_pass(),
          render_target_cache_->last_update_framebuffer());
    }
  }
  if (restore_guest_state_after_transfers) {
    // Re-bind the guest pipeline (deferred, EDS-aware, with descriptor-set
    // invalidation) - the transfer draws bound their own external pipelines and
    // cleared the guest pipeline/layout state. Out-of-pass cutout transfers
    // alone did not restore it, so the draw ran with the transfer's state
    // (Halo 3 white screen with rt_cache_draw_overwrite_cutouts).
    BindDrawPipeline(pipeline, current_pipeline);
    UpdateDynamicState(viewport_info, primitive_polygonal,
                       normalized_depth_control, draw_resolution_scale_x,
                       draw_resolution_scale_y, apply_host_depth_polygon_offset,
                       pipeline->dynamic_state);
    if (!UpdateBindings(vertex_shader, pixel_shader, stage_bindings_ready[0],
                        stage_bindings_ready[1], interpreter_placeholder,
                        placeholder_pixel_shader)) {
      return XdDrawFail(__LINE__);
    }
  }

  // Inside its render pass now: no later load of these textures in this pass
  // may be moved before the pass.
  texture_cache_->MarkTexturesUsedInRenderPass(used_texture_mask);
  if (resolve_defer_census_.active) {
    if (restore_guest_state_after_transfers || interpreter_placeholder ||
        placeholder_pixel_shader || draw_config_.debug_draw_mode) {
      resolve_defer_census_.reasons |= kResolveDeferUnknown;
    }
    ResolveDeferDrawWrites(normalized_depth_control, normalized_color_mask);
  }
  if (deferred_resolve_pending_ && draw_config_.debug_draw_mode != 1) {
    deferred_resolve_has_draws_ = true;
  }

  // Track for device-lost diagnostics.
  ++submission_in_progress_.draw_count;
  submission_in_progress_.last_vs_hash = vertex_shader->ucode_data_hash();
  submission_in_progress_.last_ps_hash =
      pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  submission_in_progress_.last_render_pass_key =
      render_target_cache_->last_update_render_pass_key().key;

  // Draw.
  vk_frame_sync_stats_.vertices +=
      primitive_processing_result.host_draw_vertex_count;
  // vulkan_render_area_draw_bound: the render area only needs the rows the
  // render target cache estimated for this draw (from the scissor, the
  // viewport or the vertices - the same bound its ownership claims use), not
  // the whole scissor, which games often leave at 8192.
  // Only a bound from the scissor or the viewport (clipping enabled) is a real
  // limit of what the host rasterizes. With clipping disabled the estimate
  // comes from running the vertex shader on the CPU over guest memory, which
  // misses vertices the GPU wrote (memexport, resolves) - good enough for
  // ownership transfers, but cutting the render area with it can drop pixels.
  const bool draw_height_bound =
      draw_config_.vulkan_render_area_draw_bound &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kHostRenderTargets &&
      render_target_cache_->last_update_height_used() != UINT32_MAX &&
      (draw_config_.vulkan_render_area_tight ||
       !register_file_->Get<reg::PA_CL_CLIP_CNTL>().clip_disable);
  if (draw_height_bound) {
    const uint32_t scale_y = render_target_cache_->IsDrawScaleNative()
                                 ? 1
                                 : render_target_cache_->draw_resolution_scale_y();
    deferred_command_buffer_->SetDrawHeightBound(
        render_target_cache_->last_update_height_used() * scale_y);
  }
  if (draw_config_.vulkan_relax_unused_attachment_ops &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kHostRenderTargets) {
    // Exactly what the pipeline enables (see GetCurrentStateDescription): the
    // depth test and write follow z_enable, stencil its own enable.
    uint32_t attachment_use = 0;
    if (normalized_depth_control.stencil_enable ||
        (normalized_depth_control.z_enable &&
         normalized_depth_control.z_write_enable)) {
      attachment_use |= 0x1;
    }
    if (normalized_depth_control.stencil_enable ||
        normalized_depth_control.z_enable) {
      attachment_use |= 0x100;
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if ((normalized_color_mask >> (i * 4)) & 0xF) {
        attachment_use |= uint32_t(2) << i;
      }
    }
    deferred_command_buffer_->SetDrawAttachmentUse(attachment_use);
  }
  // vulkan_alpha_tested_depth_prepass: the depth-only draw goes first with
  // its own pipeline and depth state, then the color pipeline is back. Same
  // pipeline layout, so the bound descriptors and push constants stay valid.
  auto begin_depth_prepass = [&]() {
    BindDrawPipeline(prepass_pipeline, prepass_handle);
    draw_dynamic_state_dirty_ = true;
    UpdateDynamicState(viewport_info, primitive_polygonal,
                       normalized_depth_control, draw_resolution_scale_x,
                       draw_resolution_scale_y, apply_host_depth_polygon_offset,
                       prepass_pipeline->dynamic_state);
  };
  auto end_depth_prepass = [&]() {
    BindDrawPipeline(pipeline, current_pipeline);
    draw_dynamic_state_dirty_ = true;
    UpdateDynamicState(viewport_info, primitive_polygonal,
                       normalized_depth_control, draw_resolution_scale_x,
                       draw_resolution_scale_y, apply_host_depth_polygon_offset,
                       pipeline->dynamic_state);
  };
  // A draw failing below must not leave its bound or attachment use set for
  // the next draw (it may not set them: a cut render area, wrong load ops).
  if (draw_config_.debug_draw_mode == 1) {
    // Diagnostic: all state recorded, no draw.
  } else if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    if (prepass_handle != VK_NULL_HANDLE) {
      begin_depth_prepass();
      deferred_command_buffer_->CmdVkDraw(
          primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
      end_depth_prepass();
    }
    deferred_command_buffer_->CmdVkDraw(
        draw_config_.debug_draw_mode == 2
            ? std::min(primitive_processing_result.host_draw_vertex_count,
                       uint32_t(3))
            : primitive_processing_result.host_draw_vertex_count,
        1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer.first = route_to_host ? shared_memory_->host_buffer()
                                         : shared_memory_->buffer();
        index_buffer.second = primitive_processing_result.guest_index_base;
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        ResetDrawBounds(draw_height_bound);
        return XdDrawFail(__LINE__);
    }
    VkIndexType index_type = primitive_processing_result.host_index_format ==
                                     xenos::IndexFormat::kInt16
                                 ? VK_INDEX_TYPE_UINT16
                                 : VK_INDEX_TYPE_UINT32;
    uint32_t index_size = index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    uint32_t first_index = 0;
    if (draw_config_.vulkan_reuse_index_buffer_binding &&
        !(index_buffer.second % index_size) &&
        index_buffer.second / index_size <= UINT32_MAX) {
      // Most draws index the same buffer (the shared memory) at different
      // offsets - keep it bound at 0 and select the indices with firstIndex
      // instead of rebinding for every draw.
      if (bound_index_buffer_ != index_buffer.first ||
          bound_index_type_ != index_type) {
        deferred_command_buffer_->CmdVkBindIndexBuffer(index_buffer.first, 0,
                                                      index_type);
        bound_index_buffer_ = index_buffer.first;
        bound_index_type_ = index_type;
      }
      first_index = uint32_t(index_buffer.second / index_size);
    } else {
      deferred_command_buffer_->CmdVkBindIndexBuffer(
          index_buffer.first, index_buffer.second, index_type);
      bound_index_buffer_ = VK_NULL_HANDLE;
    }
    if (prepass_handle != VK_NULL_HANDLE) {
      begin_depth_prepass();
      deferred_command_buffer_->CmdVkDrawIndexed(
          primitive_processing_result.host_draw_vertex_count, 1, first_index,
          0, 0);
      end_depth_prepass();
    }
    deferred_command_buffer_->CmdVkDrawIndexed(
        draw_config_.debug_draw_mode == 2
            ? std::min(primitive_processing_result.host_draw_vertex_count,
                       uint32_t(3))
            : primitive_processing_result.host_draw_vertex_count,
        1, first_index, 0, 0);
  }
  ResetDrawBounds(draw_height_bound);
  // Also consume a discard granted to a draw encoded into an open pass.
  render_target_cache_->ConsumeLastUpdateLoadDontCare();
  if (current_render_pass_ != VK_NULL_HANDLE &&
      current_framebuffer_ == render_target_cache_->last_update_framebuffer()) {
    // The open classic pass is now resumed with the compatible LOAD variant.
    current_render_pass_ = render_target_cache_->last_update_render_pass();
  }

  // Pop debug marker for draw call.
  PopDebugMarker();

  // Invalidate textures in memexported memory and watch for changes.
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes,
                                      !route_to_host);
  }
  QueueMemexportRamMirror(route_to_host);

  if (route_to_host && !memexport_ranges_.empty()) {
    // Producer draw: output landed in host_buffer_ (guest RAM), already CPU
    // coherent, so no readback. Just record the written pages. Consumers then
    // route to host_buffer_ (geometry) or copy their own range into the device
    // buffer on demand (texture loads).
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      MarkMemexportPagesWritten(memexport_range.base_address_dwords << 2,
                                memexport_range.size_bytes);
    }
  }

  // Optionally split the frame into multiple submissions (every
  // vulkan_mid_frame_submission_draws real draws) so the GPU starts rendering
  // while the rest of the frame's command stream is still being built, instead
  // of receiving the whole frame at swap time and idling until then. Counted
  // here, at the tail of IssueDraw, so only genuine rasterizing draws are
  // counted (kCopy/empty/no-VS paths return earlier). EndSubmission(false)
  // closes the render pass and submits without closing the frame; the next
  // IssueDraw's BeginSubmission(true) reopens the submission, and the same call
  // resets draws_since_submission_ to 0. Guard mirrors OnPrimaryBufferEnd().
  ++draws_since_submission_;
  ++vk_frame_sync_stats_.draws;
  ++guest_fps_draws_;
  ++frame_draws_for_pacing_;
  if (draw_config_.log_vertex_shader_hotness > 0) {
    auto& hot = vertex_shader_hotness_[vertex_shader->ucode_data_hash()];
    hot.first += index_count;
    ++hot.second;
  }
  if (pass_ts_open_pair_ != UINT32_MAX) {
    ++pass_open_draws_;
    pass_open_scissor_w_ =
        std::max(pass_open_scissor_w_,
                 uint32_t(std::max(0, dynamic_scissor_.offset.x)) +
                     dynamic_scissor_.extent.width);
    pass_open_scissor_h_ =
        std::max(pass_open_scissor_h_,
                 uint32_t(std::max(0, dynamic_scissor_.offset.y)) +
                     dynamic_scissor_.extent.height);
    pass_open_viewport_w_ = std::max(
        pass_open_viewport_w_,
        uint32_t(std::max(0.0f, dynamic_viewport_.x + dynamic_viewport_.width)));
    pass_open_viewport_h_ =
        std::max(pass_open_viewport_h_,
                 uint32_t(std::max(0.0f, dynamic_viewport_.y +
                                             std::abs(dynamic_viewport_.height))));
  }
  // Capture the draw's actual pass/submission before an automatic split.
  if constexpr (census) {
    DrawCensusRecord(
        fast,
        draw_will_execute && !vertex_shader->memexport_eM_written() &&
            (!pixel_shader || !pixel_shader->memexport_eM_written()) &&
            !interpreter_placeholder && !placeholder_pixel_shader &&
            !restore_guest_state_after_transfers &&
            !render_target_cache_->last_update_depth_replaced_by_clear() &&
            !render_target_cache_->last_update_draw_replaced_by_clear() &&
            !render_target_cache_->last_update_fully_overwritten_mask() &&
            !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable &&
            trace_state_ == TraceState::kDisabled &&
            draw_config_.debug_draw_mode == 0 && !route_to_host &&
            !shader_32bit_index_dma &&
            current_guest_graphics_pipeline_handle_ != VK_NULL_HANDLE &&
            render_target_cache_->GetPath() ==
                RenderTargetCache::Path::kHostRenderTargets &&
            primitive_processing_result.index_buffer_type ==
                PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
            primitive_processing_result.host_primitive_type ==
                xenos::PrimitiveType::kTriangleList &&
            primitive_processing_result.host_vertex_shader_type ==
                Shader::HostVertexShaderType::kVertex,
        vertex_shader_translation, pixel_shader_translation,
        primitive_processing_result);
  }
  if (draw_config_.vulkan_mid_frame_submission_draws > 0 &&
      draws_since_submission_ >=
          uint32_t(draw_config_.vulkan_mid_frame_submission_draws) &&
      submission_open_ &&
      (!scratch_buffer_used_ || deferred_resolve_scratch_pinned_) &&
      CanEndSubmissionImmediately()) {
    // A captured tail reserves scratch but must not suppress a natural split;
    // EndSubmission flushes it and releases the reservation before reuse.
    EndSubmission(false);
  }

  cached.vertex_shader = active_vertex_shader();
  cached.pixel_shader = active_pixel_shader();
  cached.shape = primitive_processing_result;
  cached.vertex_shader_modification = vertex_shader_modification;
  cached.pixel_shader_modification = pixel_shader_modification;
  cached.vertex_shader_translation = vertex_shader_translation;
  cached.pixel_shader_translation = pixel_shader_translation;
  cached.use_interpreter = use_interpreter;
  cached.normalized_color_mask = normalized_color_mask;
  cached.normalized_depth_control = normalized_depth_control;
  cached.host_depth_polygon_offset = host_depth_polygon_offset;
  cached.apply_host_depth_polygon_offset = apply_host_depth_polygon_offset;
  cached.pipeline = plain_pipeline;
  cached.prepass_pipeline = prepass_pipeline;
  cached.prepass_color_pipeline = prepass_color_pipeline;
  cached.texture_sign_classes = texture_sign_classes;
  cached.texture_exp_adjust_zero = texture_exp_adjust_zero;
  cached.used_texture_mask = used_texture_mask;
  cached.epoch = draw_state_epoch_;
  cached.submission = GetCurrentSubmission();
  cached.valid = stage_bindings_ready[0] && (!pixel_shader || stage_bindings_ready[1]) &&
      memexport_ranges_.empty() && !interpreter_placeholder &&
      !placeholder_pixel_shader &&
      !render_target_cache_->last_update_depth_replaced_by_clear() &&
      !render_target_cache_->last_update_draw_replaced_by_clear() &&
      !render_target_cache_->last_update_fully_overwritten_mask();
  register_file_->draw_dirty = 0;
  if (fast) ++fast_path_draws_;

  FrameStatsPhase(7);
  return true;
}

bool VulkanCommandProcessor::EnsureMemexportRangeInDeviceBuffer(
    uint32_t base_bytes, uint32_t size_bytes) {
  // Readers of the device buffer need memexport output copied across from
  // host_buffer_ (guest RAM), where it actually lives. Doing it on the GPU
  // keeps it ordered against the writes that produced it, which a CPU read
  // cannot be.
  if (shared_memory_host_and_edram_descriptor_set_ == VK_NULL_HANDLE ||
      !size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  if (!IsMemexportRange(base_bytes, size_bytes)) {
    return false;
  }
  VkBuffer host_buffer = shared_memory_->host_buffer();
  VkBuffer device_buffer = shared_memory_->buffer();
  if (host_buffer == VK_NULL_HANDLE) {
    return false;
  }

  const VkPipelineStageFlags read_stages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkAccessFlags read_access = VK_ACCESS_INDEX_READ_BIT |
                                    VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT;

  // Order the writes on host_buffer_ before the transfer read (the producers
  // may have run several draws ago, so barrier host_buffer_ explicitly rather
  // than relying on Use's last-usage tracking), and prior device-buffer reads
  // before the transfer write into it. Resolves write host_buffer_ by transfer,
  // not just memexport shaders, so both source masks are needed.
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  PushBufferMemoryBarrier(
      host_buffer, VkDeviceSize(base_bytes), VkDeviceSize(size_bytes),
      guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes), read_stages,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_access,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
  SubmitBarriers(true);

  VkBufferCopy copy_region;
  copy_region.srcOffset = base_bytes;
  copy_region.dstOffset = base_bytes;
  copy_region.size = size_bytes;
  deferred_command_buffer_->CmdVkCopyBuffer(host_buffer, device_buffer, 1,
                                           &copy_region);

  // Make the copied data visible to the following read.
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes),
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_stages,
                          VK_ACCESS_TRANSFER_WRITE_BIT, read_access);
  return true;
}

void VulkanCommandProcessor::ResolveReadCallbackThunk(void* context,
                                                      uint32_t physical_address,
                                                      uint32_t length) {
  static_cast<VulkanCommandProcessor*>(context)->MarkResolvePagesRead(
      physical_address, length);
}

void VulkanCommandProcessor::FlushDeferredResolve(ResolveDeferStop reason) {
  if (!deferred_resolve_pending_) return;
  assert_true(in_render_pass_);
  deferred_resolve_stop_ = reason;
  // EndRenderPass appends the captured commands after the actual end command,
  // before any caller's pending barriers. Never use the pre-pass head slot.
  EndRenderPass("deferred_resolve");
}

bool VulkanCommandProcessor::RecordDeferredResolve(
    const draw_util::ResolveInfo& info) {
  assert_true(in_render_pass_ && !deferred_resolve_pending_);
  if (!deferred_resolve_commands_) {
    deferred_resolve_commands_ = std::make_unique<DeferredCommandBuffer>(*this,
                                                                       4096);
  }
  deferred_resolve_commands_->Reset();
  deferred_resolve_commands_->SetPassBytesEnabled(pass_bytes_active_);
  auto* const main_commands = deferred_command_buffer_;
  const auto main_render_pass = current_render_pass_;
  const auto* const main_framebuffer = current_framebuffer_;
  const uint32_t main_head_slot = pass_head_slot_;
  const bool main_stream_encode = stream_encode_active_;
  const VkPipeline main_compute = current_external_compute_pipeline_;
  const auto shared_usage = shared_memory_->usage();
  VulkanSharedMemory::ReadDebt shared_read_debt =
      shared_memory_->SaveReadDebt();

  // Record while the original ResolveInfo, owners, CPU memory snapshots and
  // texture-cache generations are still current. Publication (including q477)
  // happens NOW, exactly like immediate submission; all GPU consumers flush
  // before consuming the destination, including cache hits and page uploads.
  // Only command execution moves. No resource or scratch pointer is re-read at
  // flush time. UMA CPU reads, if any, still run at the original point and
  // observe only older submissions. A first-use readback drain is excluded.
  deferred_resolve_recording_ = true;
  // No bind may be inherited from the main stream: a later pass-head upload
  // can change its compute state before this tail is finally executed.
  current_external_compute_pipeline_ = VK_NULL_HANDLE;
  deferred_command_buffer_ = deferred_resolve_commands_.get();
  in_render_pass_ = false;
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  pass_head_slot_ = UINT32_MAX;
  stream_encode_active_ = false;
  const bool result = IssueCopyImpl(&info);
  render_target_cache_->RestoreDeferredResolveSourceUsage();
  shared_memory_->Use(shared_usage);
  SubmitBarriers(true);
  shared_memory_->RestoreReadDebt(std::move(shared_read_debt));
  // Complete all tail writes before restoring the main stream's usage state.
  // This also covers the EDRAM dump, the q477 texture copy and earlier writes
  // whose range-specific tracking was consumed while recording the tail.
  VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  memory_barrier.dstAccessMask =
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  deferred_command_buffer_->CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
  deferred_resolve_final_compute_pipeline_ = current_external_compute_pipeline_;
  current_external_compute_pipeline_ = main_compute;
  deferred_command_buffer_ = main_commands;
  current_render_pass_ = main_render_pass;
  current_framebuffer_ = main_framebuffer;
  in_render_pass_ = true;
  pass_head_slot_ = main_head_slot;
  stream_encode_active_ = main_stream_encode;
  deferred_resolve_recording_ = false;
  // AcquireScratchGpuBuffer's RAII release ran while capturing; reserve that
  // same buffer until its captured uses have actually joined the main stream.
  if (deferred_resolve_scratch_pinned_) scratch_buffer_used_ = true;
  deferred_resolve_pending_ = true;
  deferred_resolve_stop_ = ResolveDeferStop::kPassEnd;
  ++deferred_resolve_queued_;
  if (!result) FlushDeferredResolve(ResolveDeferStop::kResource);
  return result;
}

bool VulkanCommandProcessor::CanDeferResolveReadback(
    const draw_util::ResolveInfo& info) const {
  if (shared_memory_->is_zero_copy()) return false;
  const auto mode = GetReadbackResolveMode();
  if (mode == ReadbackResolveMode::kDisabled) return true;
  if (mode != ReadbackResolveMode::kUma ||
      texture_cache_->IsDrawResolutionScaled() ||
      !shared_memory_->IsHostMapped()) {
    return false;
  }
  const auto it = uma_readback_last_write_.find(MakeReadbackResolveKey(
      info.copy_dest_extent_start, info.copy_dest_extent_length));
  // UMA's first write drains the CURRENT resolve, so it cannot be captured.
  // Every later UMA read/skip is evaluated while recording at the ORIGINAL
  // point: it reads an older completed submission, never this unsubmitted
  // tail. Keep that CPU action and last_write update exactly where they were.
  return it != uma_readback_last_write_.end() && it->second != 0;
}

void VulkanCommandProcessor::ResolveDeferDescribe(
    const draw_util::ResolveInfo& info, bool for_execution) {
  auto& c = resolve_defer_census_;
  // GetResolveInfo returns early for an empty rectangle, before initializing
  // destination and EDRAM fields. Keep the initial unknown classification.
  if (!info.coordinate_info.width_div_8 || !info.height_div_8) return;
  c.address = info.copy_dest_extent_start;
  c.length = info.copy_dest_extent_length;
  c.cpu_epoch = c.length
                    ? shared_memory_->CPUReadRangeEpoch(c.address, c.length)
                    : UINT64_MAX;
  c.reasons &= ~kResolveDeferUnknown;
  if (!c.length ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kHostRenderTargets ||
      (!for_execution && GetZPDMode() != ZPDMode::kFake) ||
      trace_state_ != TraceState::kDisabled ||
      register_file_->Get<reg::PA_SC_VIZ_QUERY>().viz_query_ena ||
      zpd_active_segment_.logical_active || zpd_active_segment_.segment_active ||
      c.cpu_epoch == UINT64_MAX) {
    c.reasons |= kResolveDeferUnknown;
  }
  if (info.IsClearingColor() || info.IsClearingDepth()) {
    // Delaying a clear also changes what subsequent draws read, including
    // depth/stencil and blending, even when they don't write the source.
    c.reasons |= kResolveDeferClear;
  }
  const bool readback_hazard =
      for_execution ? !CanDeferResolveReadback(info)
                    : (GetReadbackResolveMode() != ReadbackResolveMode::kDisabled ||
                       shared_memory_->is_zero_copy());
  if (readback_hazard) {
    // Preserve the q478 diagnostic model when the optimization is off.
    // Execution admits only disabled readback or UMA's older-submission path.
    c.reasons |= kResolveDeferReadback;
  }
  uint32_t base, width, rows, pitch;
  info.GetCopyEdramTileSpan(base, width, rows, pitch);
  for (uint32_t y = 0; y < rows; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t tile =
          (base + y * pitch + x) & (xenos::kEdramTileCount - 1);
      c.tiles[tile >> 6] |= uint64_t(1) << (tile & 63);
    }
  }
}

void VulkanCommandProcessor::ResolveDeferFinish() {
  auto& c = resolve_defer_census_;
  if (!c.active) return;
  if (!c.broke || !c.reentered || !c.draws) c.reasons |= kResolveDeferUnknown;
  // Observe the existing write-watch epoch without installing extra watches
  // or changing residency/protection as a side effect of the diagnostic.
  if (c.length &&
      shared_memory_->CPUReadRangeEpoch(c.address, c.length) != c.cpu_epoch) {
    c.reasons |= kResolveDeferUnknown;
  }
  ++resolve_defer_all_[c.reasons];
  if (c.reentered) ++resolve_defer_reentry_[c.reasons];
  c = {};
}

void VulkanCommandProcessor::ResolveDeferPacket(uint32_t opcode) {
  // Only state-setting packets and nested command streams may be crossed.
  // All memory/event/query/unknown packets remain observation hazards.
  switch (opcode) {
    case xenos::PM4_NOP:
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD:
    case xenos::PM4_DRAW_INDX:
    case xenos::PM4_DRAW_INDX_2:
    case xenos::PM4_SET_CONSTANT:
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS:
    // The resolve epilogue: shader loads, state invalidation, the cache flush
    // event and the wait on the coherency request don't observe the
    // destination on the host (the CP-side bookkeeping of a deferred resolve
    // stays at its original point; only its GPU commands move).
    case xenos::PM4_IM_LOAD:
    case xenos::PM4_IM_LOAD_IMMEDIATE:
    case xenos::PM4_INVALIDATE_STATE:
    case xenos::PM4_EVENT_WRITE:
    case xenos::PM4_WAIT_REG_MEM:
      break;
    default:
      resolve_defer_census_.reasons |= kResolveDeferEvent;
      FlushDeferredResolve(ResolveDeferStop::kEvent);
  }
}

void VulkanCommandProcessor::ResolveDeferDraw(
    const VulkanShader& vertex_shader, const VulkanShader* pixel_shader,
    const IndexBufferInfo* indices) {
  auto& c = resolve_defer_census_;
  if (register_file_->Get<reg::PA_SC_VIZ_QUERY>().viz_query_ena ||
      zpd_active_segment_.logical_active || zpd_active_segment_.segment_active) {
    c.reasons |= kResolveDeferEvent;
  }
  if (c.reasons & (kResolveDeferWrite | kResolveDeferRead)) return;
  // Physical memory is periodic at 512 MiB; use circular intervals and wide
  // lengths so neither aliases nor overflowing endpoints hide dependencies.
  auto overlaps = [&c](uint32_t address, uint64_t length) {
    constexpr uint32_t mask = SharedMemory::kBufferSize - 1;
    return c.length && length &&
           (length >= SharedMemory::kBufferSize ||
            ((address - c.address) & mask) < c.length ||
            ((c.address - address) & mask) < length);
  };
  if (indices && overlaps(indices->guest_base, indices->length)) {
    c.reasons |= kResolveDeferRead;
  }
  const auto& map = vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t remaining = map.vertex_fetch_bitmap[i], bit;
    while (xe::bit_scan_forward(remaining, &bit)) {
      remaining = xe::clear_lowest_bit(remaining);
      const auto fetch = register_file_->GetVertexFetch(i * 32 + bit);
      if (fetch.type != xenos::FetchConstantType::kVertex) {
        c.reasons |= kResolveDeferUnknown;
      } else if (overlaps(fetch.address << 2, uint64_t(fetch.size) << 2)) {
        c.reasons |= kResolveDeferRead;
      }
    }
  }
  uint32_t textures = 0;
  for (const Shader* shader : {&vertex_shader, pixel_shader}) {
    if (!shader) continue;
    for (const auto& binding : shader->texture_bindings()) {
      textures |= uint32_t(1) << binding.fetch_constant;
    }
    if (shader->memexport_eM_written()) c.reasons |= kResolveDeferUnknown;
  }
  bool unknown_texture = false;
  if (texture_cache_->ResolveDeferTextureRead(textures, c.address, c.length,
                                             unknown_texture)) {
    c.reasons |= kResolveDeferRead;
  }
  if (unknown_texture) c.reasons |= kResolveDeferUnknown;
  for (const auto& range : memexport_ranges_) {
    if (overlaps(range.base_address_dwords << 2, range.size_bytes)) {
      c.reasons |= kResolveDeferRead;
    }
  }
}

void VulkanCommandProcessor::ResolveDeferDrawWrites(
    reg::RB_DEPTHCONTROL depth, uint32_t color_mask) {
  auto& c = resolve_defer_census_;
  ++c.draws;
  if (c.reasons & (kResolveDeferWrite | kResolveDeferRead)) return;
  draw_util::Scissor scissor;
  draw_util::GetScissor(*register_file_, scissor);
  if (!scissor.extent[0] || !scissor.extent[1]) return;
  const auto surface = register_file_->Get<reg::RB_SURFACE_INFO>();
  auto overlaps = [&](uint32_t base, bool wide) {
    const uint32_t sx =
        uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k4X);
    const uint32_t sy =
        uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t pitch = xenos::GetSurfacePitchTiles(
        surface.surface_pitch, surface.msaa_samples, wide);
    // Round OUT to EDRAM tiles. Scissor alone is conservative even with
    // clipping disabled or vertices produced by the GPU; CPU VS estimates
    // must never be used to certify a resolve as safe.
    const uint32_t x0 = (scissor.offset[0] << sx) / 80;
    const uint32_t x1 =
        ((scissor.offset[0] + scissor.extent[0]) * (1u << sx) + 79) / 80;
    const uint32_t y0 = (scissor.offset[1] << sy) / 16;
    const uint32_t y1 =
        ((scissor.offset[1] + scissor.extent[1]) * (1u << sy) + 15) / 16;
    for (uint32_t y = y0; y < y1; ++y) {
      for (uint32_t x = x0 << wide; x < (x1 << wide); ++x) {
        const uint32_t tile =
            (base + y * pitch + x) & (xenos::kEdramTileCount - 1);
        if (c.tiles[tile >> 6] & (uint64_t(1) << (tile & 63))) return true;
      }
    }
    return false;
  };
  const auto z = register_file_->Get<reg::RB_DEPTH_INFO>();
  bool hit =
      (depth.stencil_enable || (depth.z_enable && depth.z_write_enable)) &&
      overlaps(z.depth_base, false);
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets && !hit; ++i) {
    if (!((color_mask >> (i * 4)) & 15)) continue;
    const auto color = register_file_->Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    hit = overlaps(color.color_base,
                   xenos::IsColorRenderTargetFormat64bpp(color.color_format));
  }
  if (hit) c.reasons |= kResolveDeferWrite;
}

void VulkanCommandProcessor::ResolveDeferReport() {
  for (uint32_t reentry = 0; reentry < 2; ++reentry) {
    const auto& counts = reentry ? resolve_defer_reentry_ : resolve_defer_all_;
    uint64_t total = 0, writes = 0, reads = 0, other = 0, gpu_clean = 0;
    std::array<uint64_t, 4> details = {};
    for (uint32_t reason = 0; reason < counts.size(); ++reason) {
      total += counts[reason];
      writes += (reason & kResolveDeferWrite) ? counts[reason] : 0;
      reads += (reason & kResolveDeferRead) ? counts[reason] : 0;
      other += (reason & ~uint32_t(3)) ? counts[reason] : 0;
      gpu_clean += (reason & 3) ? 0 : counts[reason];
      for (uint32_t i = 0; i < details.size(); ++i) {
        if (reason & (4u << i)) details[i] += counts[reason];
      }
    }
    const double f = double(render_pass_break_frames_);
    XELOGI(
        "[RDEFER] {} per-frame: total={:.2f} candidate={:.2f} "
        "gpu-clean={:.2f} write={:.2f} dest-use={:.2f} other={:.2f} "
        "(clear={:.2f} readback={:.2f} event={:.2f} unknown={:.2f}); "
        "reasons overlap; scissor rounded out to EDRAM tiles",
        reentry ? "reentry" : "mid-pass", total / f, counts[0] / f,
        gpu_clean / f, writes / f, reads / f, other / f, details[0] / f,
        details[1] / f, details[2] / f, details[3] / f);
  }
  const double frames = double(render_pass_break_frames_);
  std::string stops, rejected;
  static const char* const names[] = {
      "pass-end", "source-write", "dest-use", "clear", "readback", "event",
      "submission", "next-resolve", "resource"};
  for (size_t i = 0; i < size_t(ResolveDeferStop::kCount); ++i) {
    stops += fmt::format(" {}={:.2f}", names[i],
                          deferred_resolve_stops_[i] / frames);
    rejected += fmt::format(" {}={:.2f}", names[i],
                             deferred_resolve_rejected_[i] / frames);
  }
  XELOGI("[RDEFER] execution per-frame: queued={:.2f} executed={:.2f} "
         "with-draws={:.2f} uma={:.2f}; flush:{}; rejected:{} "
         "(recorded, not GPU-retired)",
         deferred_resolve_queued_ / frames,
         deferred_resolve_executed_ / frames,
         deferred_resolve_with_draws_ / frames,
         deferred_resolve_uma_queued_ / frames, stops, rejected);
  deferred_resolve_queued_ = deferred_resolve_executed_ = 0;
  deferred_resolve_with_draws_ = deferred_resolve_uma_queued_ = 0;
  deferred_resolve_stops_.fill(0);
  deferred_resolve_rejected_.fill(0);
  resolve_defer_all_.fill(0);
  resolve_defer_reentry_.fill(0);
}

bool VulkanCommandProcessor::IssueCopy() {
  FlushDeferredResolve(ResolveDeferStop::kNextResolve);
  ++draw_state_epoch_;
  const uint64_t fs_copy_begin = FrameStatsBegin();
  ResolveDeferFinish();
  // Depth resolves go to the surface path when it's on (no compute at all),
  // which a deferred recording would skip.
  const bool depth_to_surface =
      RenderTargetShortcut(cvars::vulkan_texture_from_render_target) &&
      register_file_->Get<reg::RB_COPY_CONTROL>().copy_src_select >=
          xenos::kMaxColorRenderTargets;
  // Untiling: the resolves and clears of a strip address its screen rows.
  uint32_t untile_rows = 0;
  if (cvars::gpu_untile_predicated_tiling && UntileTiled()) {
    UntileSyncPhase();
    untile_resolve_seen_ = true;
    const reg::PA_SC_WINDOW_OFFSET window_offset =
        register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    if (!window_offset.window_x_offset && window_offset.window_y_offset < 0) {
      untile_rows = uint32_t(-window_offset.window_y_offset);
      if (!untile_strip_height_ || untile_rows < untile_strip_height_) {
        untile_strip_height_ = untile_rows;
      }
    }
    untile_last_resolve_rows_ = untile_rows;
  }
  render_target_cache_->set_untile_row_offset_pixels(untile_rows);
  // A deferred resolve runs later, without the row offset.
  const bool try_defer = cvars::vulkan_resolve_defer && in_render_pass_ &&
                         frame_open_ && !device_lost_ && !depth_to_surface &&
                         !untile_rows;
  if ((cvars::log_render_pass_breaks > 0 || try_defer) && in_render_pass_) {
    resolve_defer_census_.active = true;
    resolve_defer_census_.recording = true;
    resolve_defer_census_.framebuffer = current_framebuffer_;
  }
  draw_util::ResolveInfo info = {};
  const draw_util::ResolveInfo* captured = nullptr;
  bool copy_result = true;
  bool defer = false;
  if (try_defer) {
    copy_result = render_target_cache_->GetResolveInfoForDefer(*memory_, info);
    if (copy_result) {
      captured = &info;
      ResolveDeferDescribe(info, true);
      const uint32_t reasons = resolve_defer_census_.reasons;
      ResolveDeferStop rejected = ResolveDeferStop::kResource;
      if (reasons & kResolveDeferClear) rejected = ResolveDeferStop::kClear;
      else if (reasons & kResolveDeferReadback)
        rejected = ResolveDeferStop::kReadback;
      // A single immutable segment, contained in the current submission.
      // Existing barriers, scratch users, queries, traces and clear chains
      // retain the immediate path; none may be crossed speculatively.
      defer = !reasons && !scratch_buffer_used_ &&
              pending_barriers_.empty() &&
              pending_barriers_buffer_memory_barriers_.empty() &&
              pending_barriers_image_memory_barriers_.empty() &&
              !current_pending_barrier_.src_stage_mask &&
              !current_pending_barrier_.dst_stage_mask &&
              render_target_cache_->BeginDeferredResolve(info);
      if (!defer) ++deferred_resolve_rejected_[size_t(rejected)];
    }
  }
  if (copy_result) {
    if (defer && GetReadbackResolveMode() == ReadbackResolveMode::kUma) {
      ++deferred_resolve_uma_queued_;
    }
    copy_result = defer ? RecordDeferredResolve(info) : IssueCopyImpl(captured);
  }
  resolve_defer_census_.recording = false;
  if (!copy_result && resolve_defer_census_.active) {
    resolve_defer_census_.reasons |= kResolveDeferUnknown;
  }
  if (fs_copy_begin) {
    frame_time_stats_.copies++;
    frame_time_stats_.copy_ns += FrameStatsNow() - fs_copy_begin;
  }
  render_target_cache_->set_untile_row_offset_pixels(0);
  return copy_result;
}

bool VulkanCommandProcessor::IssueCopyImpl(
    const draw_util::ResolveInfo* captured_info) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!BeginSubmission(true)) {
    return false;
  }
  SharedMemory::PendingAccessScope pending_access_scope(
      cvars::vulkan_republish_draw_inputs ? shared_memory_.get() : nullptr);

  // Push debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PushDebugMarker("IssueCopy (Resolve)");
  }

  // Bracket the resolve emission region with GPU timestamps (BOTTOM_OF_PIPE:
  // the region begins and ends at full barriers already, so the wait-for-idle
  // Turnip implies is ~free here).
  uint32_t resolve_ts_pair = UINT32_MAX;
  if (resolve_timestamp_mapping_) {
    if (resolve_ts_submission_ != GetCurrentSubmission()) {
      resolve_ts_submission_ = GetCurrentSubmission();
      resolve_ts_count_ = 0;
    }
    if (resolve_ts_count_ < kResolveTimestampPairsPerSubmission) {
      resolve_ts_pair =
          uint32_t(resolve_ts_submission_ % kResolveTimestampRingSubmissions) *
              kResolveTimestampPairsPerSubmission +
          resolve_ts_count_;
      resolve_ts_kind_[resolve_ts_pair] = 0;
      deferred_command_buffer_->CmdVkWriteTimestamp(
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resolve_timestamp_pool_,
          resolve_ts_pair * 2);
    } else {
      ++vk_frame_sync_stats_.resolve_ts_dropped;
    }
  }

  uint32_t written_address, written_length;
  reg::RB_COPY_DEST_INFO copy_dest_info;
  bool is_scaled;
  pass_bytes_resolve_scope_ = pass_bytes_active_;
  const bool resolve_succeeded = render_target_cache_->Resolve(
      *memory_, *shared_memory_, *texture_cache_, written_address,
      written_length, &copy_dest_info, &is_scaled, captured_info);
  pass_bytes_resolve_scope_ = false;
  if (resolve_ts_pair != UINT32_MAX) {
    // Always close an opened pair - a WAIT_BIT results copy over a written
    // begin with no end would hang the GPU.
    deferred_command_buffer_->CmdVkWriteTimestamp(
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resolve_timestamp_pool_,
        resolve_ts_pair * 2 + 1);
    ++resolve_ts_count_;
  }
  if (!resolve_succeeded) {
    if (debug_markers_enabled_) {
      PopDebugMarker();
    }
    return false;
  }
  ++submission_in_progress_.resolve_count;
  ++vk_frame_sync_stats_.resolves;

  // The resolve wrote the device buffer. Drop any stale memexport marks so the
  // output isn't overwritten with guest RAM by a later texture load.
  ClearMemexportPages(written_address, written_length);

  ReadbackResolveMode readback_mode = GetReadbackResolveMode();
  const bool zero_copy = shared_memory_->is_zero_copy();
  // Readback lands in guest RAM: the host buffer in two-buffer mode, or buffer_
  // itself in zero-copy mode, since it already aliases guest RAM.
  VkBuffer resolve_host_buffer =
      zero_copy ? shared_memory_->buffer() : shared_memory_->host_buffer();
  // uma reads the host mapping directly, so it needs no imported buffer - the
  // copy branches below check resolve_host_buffer themselves. Gating the whole
  // block on it disabled readback outright wherever guest RAM cannot be
  // imported (no VK_EXT_external_memory_host, i.e. every Adreno).
  const bool uma_direct = readback_mode == ReadbackResolveMode::kUma &&
                          !texture_cache_->IsDrawResolutionScaled() &&
                          shared_memory_->IsHostMapped();
  if (readback_mode != ReadbackResolveMode::kDisabled && written_length > 0 &&
      resolve_host_buffer == VK_NULL_HANDLE && !uma_direct) {
    static bool readback_unavailable_logged = false;
    if (!readback_unavailable_logged) {
      readback_unavailable_logged = true;
      XELOGW(
          "Resolve readback is enabled but guest RAM is neither host-imported "
          "nor host-mapped - readback will not happen");
    }
  }
  // A depth surface published for this resolve (vulkan_texture_from_render_
  // target) keeps guest RAM coherent itself: its pages are protected and the
  // first CPU access materializes them. Reading it back would materialize it
  // right away, with a GPU wait, for every resolve.
  const bool provider_published =
      written_length > 0 &&
      shared_memory_->IsRangeValidInSurface(written_address, written_length);
  if (readback_mode != ReadbackResolveMode::kDisabled && written_length > 0 &&
      !provider_published &&
      (resolve_host_buffer != VK_NULL_HANDLE || uma_direct) &&
      IsResolveDestinationResident(written_address, written_length)) {
    bool stall_after_copy;
    if (!DecideResolveHostCopy(readback_mode, written_address, written_length,
                               cvars::readback_resolve_sync,
                               stall_after_copy)) {
      // some mode: the range has not been read since its last resolve.
      PopDebugMarker();
      return true;
    }

    if (!shared_memory_->RequestRange(written_address, written_length)) {
      PopDebugMarker();
      return false;
    }
    // UMA: shared memory is host-mapped, so the CPU reads the resolved bytes
    // straight out of it - no device->host copy at all. Serves the same role as
    // upstream's zero_copy on devices that cannot import guest RAM. Kept
    // selectable against kFast/kAll so the two designs can be compared.
    if (uma_direct) {
      ++vk_frame_sync_stats_.rb_uma_direct;
      const uint64_t resolve_submission = GetCurrentSubmission();
      const uint64_t resolve_key =
          MakeReadbackResolveKey(written_address, written_length);
      auto& last_write = uma_readback_last_write_[resolve_key];
      bool do_read;
      if (!last_write) {
        ++vk_frame_sync_stats_.rb_uma_first_use;
        // Seed guest RAM.
        // Make the just-recorded write host-visible and drain to read it now.
        // Guest shader stages included: in-pass resolves write shared memory
        // from the fragment stage, not just compute/transfer.
        PushBufferMemoryBarrier(
            shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
            guest_shader_pipeline_stages_ |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, false);
        SubmitBarriers(true);
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE("VulkanCommandProcessor: uma resolve readback drain failed");
          PopDebugMarker();
          return true;
        }
        do_read = true;
      } else {
        // Read the previous resolve's bytes, but only once that submission has
        // retired, so the CPU never races the GPU writing the same region.
        // Cached completed-submission value - no blocking poll (vkGetFenceStatus
        // stalls on Turnip). Staleness is bounded by frames-in-flight.
        do_read = GetCompletedSubmission() >= last_write;
      }
      if (do_read) {
        InsertDebugMarker("Resolve Readback (uma): 0x%08X, %u bytes",
                          written_address, written_length);
        shared_memory_->ReadHostMapped(
            written_address, written_length,
            memory_->TranslatePhysical(written_address), true);
      }
      last_write = resolve_submission;
      PopDebugMarker();
      return true;
    }

    if (!texture_cache_->IsDrawResolutionScaled()) {
      if (zero_copy) {
        // The non-scaled resolve already wrote buffer_ (guest RAM) in place, so
        // it is coherent with the CPU - nothing to read back.
        PopDebugMarker();
        return true;
      }
      // Non-scaled: copy the resolved range straight from the device buffer
      // into host_buffer_ (guest RAM).
      VkBuffer resolve_device_buffer = shared_memory_->buffer();
      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      // Order prior memexport and resolve writes to host_buffer_ before this
      // copy.
      PushBufferMemoryBarrier(
          resolve_host_buffer, VkDeviceSize(written_address),
          VkDeviceSize(written_length),
          guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);
      SubmitBarriers(true);
      InsertDebugMarker("Resolve Sync (guest RAM): 0x%08X, %u bytes",
                        written_address, written_length);
      VkBufferCopy copy_region = {};
      copy_region.srcOffset = written_address;
      copy_region.dstOffset = written_address;
      copy_region.size = written_length;
      deferred_command_buffer_->CmdVkCopyBuffer(
          resolve_device_buffer, resolve_host_buffer, 1, &copy_region);
      // Make the copy visible to within-frame consumers reading host_buffer_
      // (route_to_host draws sampling guest RAM as index, vertex or texture,
      // and EnsureMemexportRangeInDeviceBuffer copying out of it). Use()'s
      // mirror barrier is keyed on the device buffer, so it does not cover this
      // transfer write.
      PushBufferMemoryBarrier(
          resolve_host_buffer, VkDeviceSize(written_address),
          VkDeviceSize(written_length), VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
              VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
      if (stall_after_copy) {
        // host_buffer_ is HOST_COHERENT, so waiting makes the copy visible to
        // the guest CPU before a later read races it.
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE(
              "VulkanCommandProcessor: Failed to complete queue operations for "
              "resolve sync");
        }
      }
      PopDebugMarker();
      return true;
    }
    // Scaled: GPU compute downscale into host_buffer_ (guest RAM).
    // Get scaled resolve buffer (works for both sparse and simple buffer modes)
    VkBuffer scaled_buffer = texture_cache_->GetCurrentScaledResolveBuffer();
    if (scaled_buffer == VK_NULL_HANDLE) {
      XELOGE("VulkanCommandProcessor: No scaled resolve buffer available");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    uint32_t scale_x = texture_cache_->draw_resolution_scale_x();
    uint32_t scale_y = texture_cache_->draw_resolution_scale_y();
    uint32_t scale_area = scale_x * scale_y;

    assert_true(scale_x >= 1 &&
                scale_x <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
    assert_true(scale_y >= 1 &&
                scale_y <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
    assert_true(scale_x > 1 || scale_y > 1);

    // Texel size from the normalized copy_dest_info, not a re-read of
    // RB_COPY_DEST_INFO - for depth the register can hold a different size.
    uint32_t pixel_size_log2 =
        draw_util::GetResolveDownscalePixelSizeLog2(copy_dest_info);
    if (pixel_size_log2 > 3) {
      // 128bpp - the tiled scaled addressing reversal in the downscale shader
      // does not handle it.
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to a 128bpp "
          "destination - not supported by the downscale shader");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    // The scaled addressing is periodic per guest group, so the written extent
    // must be group-aligned for the per-tile reversal to be valid.
    uint32_t group_bytes_log2 = pixel_size_log2 <= 2 ? 7 : 6;
    if (written_address & ((uint32_t(1) << group_bytes_log2) - 1)) {
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
          "destination is not aligned to the scaled addressing group size",
          written_address);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    uint32_t tile_size_1x = (32u * 32u) << pixel_size_log2;
    uint32_t tile_count = written_length / tile_size_1x;
    if (tile_count == 0) {
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    // Only whole 32x32 tiles are downscaled - truncate a partial tail so stale
    // data is not copied to the guest.
    uint32_t readback_length = tile_count * tile_size_1x;

    // Bind the source at the written extent (the range starts earlier, at the
    // destination base) and skip if the extent isn't fully inside the range.
    uint64_t scaled_start = uint64_t(written_address) * scale_area;
    uint64_t scaled_readback_length = uint64_t(readback_length) * scale_area;
    uint64_t range_start_scaled =
        texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    uint64_t range_length_scaled =
        texture_cache_->GetCurrentScaledResolveRangeLengthScaled();
    if (!range_length_scaled || scaled_start < range_start_scaled ||
        scaled_start + scaled_readback_length >
            range_start_scaled + range_length_scaled) {
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
          "written extent is not within the current scaled resolve range",
          written_address);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Calculate offset within the buffer using the buffer's base address.
    // GetCurrentScaledResolveBufferBaseOffset() returns:
    // - For sparse buffers: buffer_index << 30 (same as D3D12's 1GB chunks)
    // - For simple buffers: the buffer's range_start_scaled
    uint64_t buffer_base =
        texture_cache_->GetCurrentScaledResolveBufferBaseOffset();
    if (scaled_start < buffer_base) {
      XELOGE(
          "VulkanCommandProcessor: Scaled address {} is before buffer start {}",
          scaled_start, buffer_base);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    uint64_t source_offset = scaled_start - buffer_base;

    // The downscale compute writes 1x data straight into host_buffer_ (guest
    // RAM) at the resolved range.
    VkBuffer dest_buffer = resolve_host_buffer;
    VkDeviceSize dest_offset = written_address;

    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    // Ensure intermediate buffer for GPU downscaling is large enough
    uint32_t downscale_buffer_size = AlignReadbackBufferSize(readback_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      // Clean up old buffer
      if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE(
              "VulkanCommandProcessor: Failed to wait for GPU before "
              "destroying old downscale buffer");
          if (debug_markers_enabled_) {
            PopDebugMarker();
          }
          return true;
        }
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_size_ = 0;
      }

      VkBufferCreateInfo buffer_info = {};
      buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      buffer_info.size = downscale_buffer_size;
      buffer_info.usage =
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      if (dfn.vkCreateBuffer(device, &buffer_info, nullptr,
                             &resolve_downscale_buffer_) != VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to create {} MB downscale buffer",
            downscale_buffer_size >> 20);
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      VkMemoryRequirements memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, resolve_downscale_buffer_,
                                        &memory_requirements);

      const uint32_t memory_type_index = ui::vulkan::util::ChooseMemoryType(
          vulkan_device->memory_types(), memory_requirements.memoryTypeBits,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal);

      if (memory_type_index == UINT32_MAX) {
        XELOGE(
            "VulkanCommandProcessor: Failed to find memory type for downscale "
            "buffer");
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      VkMemoryAllocateInfo memory_info = {};
      memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      memory_info.allocationSize = memory_requirements.size;
      memory_info.memoryTypeIndex = memory_type_index;

      if (dfn.vkAllocateMemory(device, &memory_info, nullptr,
                               &resolve_downscale_buffer_memory_) !=
          VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to allocate downscale buffer "
            "memory");
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      if (dfn.vkBindBufferMemory(device, resolve_downscale_buffer_,
                                 resolve_downscale_buffer_memory_,
                                 0) != VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to bind downscale buffer memory");
        dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    // Allocate descriptor set for source and destination buffers.
    // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
    VkDescriptorSet descriptor_set =
        resolve_downscale_descriptor_pool_chain_->Allocate(
            GetCurrentSubmission());
    if (descriptor_set == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanCommandProcessor: Failed to allocate resolve downscale "
          "descriptor set from pool chain");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Ensure submission is open
    if (!BeginSubmission(true)) {
      XELOGE(
          "VulkanCommandProcessor: Failed to begin submission for scaled "
          "resolve readback");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Update descriptor set with buffer bindings
    // Bind source buffer at offset 0 to avoid alignment issues - offset is
    // passed via push constants and applied in the shader
    std::array<VkDescriptorBufferInfo, 2> buffer_infos;
    buffer_infos[0].buffer = scaled_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = VK_WHOLE_SIZE;
    // Destination buffer (intermediate device-local buffer)
    buffer_infos[1].buffer = resolve_downscale_buffer_;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = readback_length;

    std::array<VkWriteDescriptorSet, 2> descriptor_writes;
    descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[0].pNext = nullptr;
    descriptor_writes[0].dstSet = descriptor_set;
    descriptor_writes[0].dstBinding = 0;
    descriptor_writes[0].dstArrayElement = 0;
    descriptor_writes[0].descriptorCount = 1;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[0].pImageInfo = nullptr;
    descriptor_writes[0].pBufferInfo = &buffer_infos[0];
    descriptor_writes[0].pTexelBufferView = nullptr;

    descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[1].pNext = nullptr;
    descriptor_writes[1].dstSet = descriptor_set;
    descriptor_writes[1].dstBinding = 1;
    descriptor_writes[1].dstArrayElement = 0;
    descriptor_writes[1].descriptorCount = 1;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[1].pImageInfo = nullptr;
    descriptor_writes[1].pBufferInfo = &buffer_infos[1];
    descriptor_writes[1].pTexelBufferView = nullptr;

    dfn.vkUpdateDescriptorSets(device, uint32_t(descriptor_writes.size()),
                               descriptor_writes.data(), 0, nullptr);

    // End any active render pass and submit barriers
    SubmitBarriers(true);

    // Barrier for source buffer - ensure resolve copy compute shader writes
    // are complete before readback compute shader reads.
    VkBufferMemoryBarrier source_barrier = {};
    source_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    source_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.buffer = scaled_buffer;
    source_barrier.offset = 0;
    source_barrier.size = VK_WHOLE_SIZE;
    deferred_command_buffer_->CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &source_barrier,
        0, nullptr);

    PushDebugMarker("Resolve Downscale: 0x%08X, %u bytes -> %u bytes",
                    written_address, uint32_t(scaled_readback_length),
                    readback_length);

    // Bind compute pipeline
    BindExternalComputePipeline(resolve_downscale_pipeline_);

    // Push constants
    ResolveDownscaleConstants constants;
    constants.scale_x = scale_x;
    constants.scale_y = scale_y;
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = static_cast<uint32_t>(source_offset);
    // Optionally sample from center of scaled block instead of top-left.
    constants.half_pixel_offset = (cvars::readback_resolve_half_pixel_offset &&
                                   (scale_x > 1 || scale_y > 1))
                                      ? 1
                                      : 0;
    deferred_command_buffer_->CmdVkPushConstants(
        resolve_downscale_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(constants), &constants);

    // Bind descriptor set
    deferred_command_buffer_->CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_COMPUTE, resolve_downscale_pipeline_layout_, 0,
        1, &descriptor_set, 0, nullptr);

    // Dispatch compute shader - one thread group per 32x32 tile
    ++submission_in_progress_.dispatch_count;
    deferred_command_buffer_->CmdVkDispatch(tile_count, 1, 1, DeferredCommandBuffer::DispatchSite::kCopy);

    // Barriers before copy: downscale compute-write -> transfer-read, and order
    // prior writes to host_buffer_ (memexport shader writes and earlier resolve
    // copies) before this copy.
    VkBufferMemoryBarrier pre_copy_barriers[2] = {};
    pre_copy_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_copy_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre_copy_barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre_copy_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[0].buffer = resolve_downscale_buffer_;
    pre_copy_barriers[0].offset = 0;
    pre_copy_barriers[0].size = readback_length;
    pre_copy_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_copy_barriers[1].srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    pre_copy_barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre_copy_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[1].buffer = dest_buffer;
    pre_copy_barriers[1].offset = dest_offset;
    pre_copy_barriers[1].size = readback_length;
    deferred_command_buffer_->CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
            guest_shader_pipeline_stages_,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 2, pre_copy_barriers, 0,
        nullptr);

    // Copy the downscaled data into host_buffer_ (guest RAM).
    VkBufferCopy copy_region = {};
    copy_region.srcOffset = 0;
    copy_region.dstOffset = dest_offset;
    copy_region.size = readback_length;
    deferred_command_buffer_->CmdVkCopyBuffer(resolve_downscale_buffer_,
                                             dest_buffer, 1, &copy_region);
    // Make the copy visible to within-frame consumers reading host_buffer_
    // (route_to_host draws sampling guest RAM as index, vertex or texture, and
    // EnsureMemexportRangeInDeviceBuffer copying out of it).
    PushBufferMemoryBarrier(
        dest_buffer, dest_offset, VkDeviceSize(readback_length),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    PopDebugMarker();

    if (stall_after_copy) {
      // host_buffer_ is HOST_COHERENT, so waiting makes the copy visible to the
      // guest CPU before a later read races it.
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE(
            "VulkanCommandProcessor: Failed to complete queue operations for "
            "scaled resolve sync");
      }
    }
  }

  // Pop debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PopDebugMarker();
  }

  return true;
}

void VulkanCommandProcessor::EnsureZPDQueryResources() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  bool can_recreate =
      !zpd_active_segment_.logical_active &&
      !zpd_active_segment_.segment_active &&
      zpd_active_query_index_ == UINT32_MAX && !zpd_active_query_is_fsi_ &&
      !zpd_host_query_pool_->has_pending_resolve_batch() &&
      zpd_resolves_in_flight_.empty() && zpd_deferred_releases_.empty();

  bool initialize_fsi_counter = render_target_cache_->GetPath() ==
                                RenderTargetCache::Path::kPixelShaderInterlock;

  zpd_host_query_pool_->EnsureInitialized(GetVulkanDevice(),
                                          kZPDQueryPoolCapacity, can_recreate,
                                          initialize_fsi_counter);

  if (initialize_fsi_counter &&
      zpd_host_query_pool_->fsi_counter_initialized()) {
    VkBuffer fsi_counter_buffer = zpd_host_query_pool_->fsi_counter_buffer();
    VkDeviceSize fsi_counter_range =
        sizeof(uint32_t) * zpd_host_query_pool_->capacity();
    if (zpd_fsi_counter_descriptor_buffer_ != fsi_counter_buffer ||
        zpd_fsi_counter_descriptor_range_ != fsi_counter_range) {
      VkDescriptorBufferInfo fsi_counter_descriptor_buffer_info;
      fsi_counter_descriptor_buffer_info.buffer = fsi_counter_buffer;
      fsi_counter_descriptor_buffer_info.offset = 0;
      fsi_counter_descriptor_buffer_info.range = fsi_counter_range;

      // Mirror binding 2 to the host-imported routing set too, so a routed FSI
      // draw reads the same counter.
      VkWriteDescriptorSet fsi_counter_descriptor_writes[2];
      uint32_t fsi_counter_descriptor_write_count = 0;
      for (VkDescriptorSet set :
           {shared_memory_and_edram_descriptor_set_,
            shared_memory_host_and_edram_descriptor_set_}) {
        if (set == VK_NULL_HANDLE) {
          continue;
        }
        VkWriteDescriptorSet& fsi_counter_descriptor_write =
            fsi_counter_descriptor_writes[fsi_counter_descriptor_write_count++];
        fsi_counter_descriptor_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fsi_counter_descriptor_write.pNext = nullptr;
        fsi_counter_descriptor_write.dstSet = set;
        fsi_counter_descriptor_write.dstBinding = 2;
        fsi_counter_descriptor_write.dstArrayElement = 0;
        fsi_counter_descriptor_write.descriptorCount = 1;
        fsi_counter_descriptor_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fsi_counter_descriptor_write.pImageInfo = nullptr;
        fsi_counter_descriptor_write.pBufferInfo =
            &fsi_counter_descriptor_buffer_info;
        fsi_counter_descriptor_write.pTexelBufferView = nullptr;
      }

      const ui::vulkan::VulkanDevice::Functions& dfn =
          GetVulkanDevice()->functions();
      dfn.vkUpdateDescriptorSets(GetVulkanDevice()->device(),
                                 fsi_counter_descriptor_write_count,
                                 fsi_counter_descriptor_writes, 0, nullptr);

      zpd_fsi_counter_descriptor_buffer_ = fsi_counter_buffer;
      zpd_fsi_counter_descriptor_range_ = fsi_counter_range;
    }
  } else if (!IsZPDQueryPoolReady() && cvars::occlusion_query_log) {
    XELOGI(
        "ZPD/Vulkan: FSI counter resources unavailable; keeping counter index "
        "sentinel active");
  }
  zpd_fsi_counter_index_force_update_ = true;
}

bool VulkanCommandProcessor::IsZPDQueryPoolReady() const {
  if (!zpd_host_query_pool_) {
    return false;
  }
  if (!render_target_cache_ ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kPixelShaderInterlock) {
    return zpd_host_query_pool_->is_initialized();
  }
  VkDeviceSize fsi_counter_range =
      sizeof(uint32_t) * zpd_host_query_pool_->capacity();
  return zpd_host_query_pool_->fsi_counter_initialized() &&
         zpd_fsi_counter_descriptor_buffer_ ==
             zpd_host_query_pool_->fsi_counter_buffer() &&
         zpd_fsi_counter_descriptor_range_ == fsi_counter_range;
}

bool VulkanCommandProcessor::CanOpenZPDQuery() const {
  if (!submission_open_) {
    return false;
  }
  bool use_fsi_counter_path =
      render_target_cache_ &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kPixelShaderInterlock;
  return use_fsi_counter_path || in_render_pass_;
}

CommandProcessor::QueryOpenResult VulkanCommandProcessor::OpenZPDQuery(
    ReportHandle report_handle, bool can_close_submission) {
  bool use_fsi_counter_path =
      zpd_host_query_pool_->fsi_counter_initialized() &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kPixelShaderInterlock;

  if (!BeginSubmission(true)) {
    return QueryOpenResult::kFailed;
  }

  if (!use_fsi_counter_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  bool retried_after_submission_flip = false;
  while (true) {
    bool is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();

    if (is_pool_exhausted) {
      PumpQueryResolves();
      is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();
    }

    if (is_pool_exhausted &&
        (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt)) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (is_pool_exhausted && !zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    if (wait_for == 0) {
      break;
    }

    if (submission_open_ && wait_for == GetCurrentSubmission()) {
      if (retried_after_submission_flip || !can_close_submission ||
          !CanEndSubmissionImmediately()) {
        return QueryOpenResult::kDeferred;
      }

      VkRenderPass saved_render_pass = VK_NULL_HANDLE;
      const VulkanRenderTargetCache::Framebuffer* saved_framebuffer = nullptr;
      if (in_render_pass_) {
        saved_render_pass = current_render_pass_;
        saved_framebuffer = current_framebuffer_;
        EndRenderPass();
      }
      if (!EndSubmission(false)) {
        return QueryOpenResult::kFailed;
      }
      if (!BeginSubmission(true)) {
        return QueryOpenResult::kFailed;
      }

      if (saved_framebuffer) {
        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        zpd_active_segment_.segment_pending_begin = false;
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
        if (!in_render_pass_) {
          return QueryOpenResult::kDeferred;
        }
      }

      retried_after_submission_flip = true;
      continue;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }
      if (!AwaitSubmissionAndUpdateCompleted(wait_for)) {
        return QueryOpenResult::kFailed;
      }
      PumpQueryResolves();
    }

    break;
  }

  if (!use_fsi_counter_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  if (!zpd_host_query_pool_->AcquireQueryIndex(zpd_active_query_index_,
                                               zpd_active_query_generation_)) {
    return QueryOpenResult::kFailed;
  }

  zpd_active_query_is_fsi_ = use_fsi_counter_path;

  // FSI queries don't use Vulkan occlusion queries at all.
  // While the segment is open, the translated pixel shader accumulates passed
  // MSAA samples into one counter slot selected via zpd_fsi_counter_index.
  // Clear the slot here so a recycled index never inherits old counts.
  if (zpd_active_query_is_fsi_) {
    bool fsi_counter_cleared = false;
    if (zpd_host_query_pool_->fsi_counter_initialized()) {
      if (in_render_pass_) {
        VkRenderPass saved_render_pass = current_render_pass_;
        const VulkanRenderTargetCache::Framebuffer* saved_framebuffer =
            current_framebuffer_;
        EndRenderPass();
        zpd_host_query_pool_->ClearFSICounter(*deferred_command_buffer_,
                                              zpd_active_query_index_);

        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        zpd_active_segment_.segment_pending_begin = false;
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
        fsi_counter_cleared = in_render_pass_;
      } else {
        zpd_host_query_pool_->ClearFSICounter(*deferred_command_buffer_,
                                              zpd_active_query_index_);
        fsi_counter_cleared = true;
      }
    }
    if (!fsi_counter_cleared) {
      zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                              zpd_active_query_generation_);
      zpd_active_query_index_ = UINT32_MAX;
      zpd_active_query_generation_ = 0;
      zpd_active_query_is_fsi_ = false;
      zpd_fsi_counter_index_force_update_ = true;
      return QueryOpenResult::kFailed;
    }
    zpd_fsi_counter_index_force_update_ = true;
    return QueryOpenResult::kOpened;
  }

  zpd_host_query_pool_->BeginQuery(*deferred_command_buffer_,
                                   zpd_active_query_index_);
  return QueryOpenResult::kOpened;
}

bool VulkanCommandProcessor::CloseZPDQuery(ReportHandle report_handle,
                                           uint64_t& out_submission) {
  if (!zpd_active_query_is_fsi_ && !in_render_pass_) {
    XELOGW("ZPD: Split segment requested outside render pass");
    return false;
  }

  if (zpd_active_query_is_fsi_) {
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
  } else {
    zpd_host_query_pool_->EndQuery(*deferred_command_buffer_,
                                   zpd_active_query_index_);
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, false);
  }

  PendingQueryResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.query_index = zpd_active_query_index_;
  resolve.query_generation = zpd_active_query_generation_;
  resolve.scale_area = GetZPDScaleArea();
  resolve.uses_fsi_counter = zpd_active_query_is_fsi_;
  resolve.report_handle = report_handle;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  bool closed_fsi_counter = zpd_active_query_is_fsi_;
  zpd_active_query_is_fsi_ = false;
  if (closed_fsi_counter) {
    zpd_fsi_counter_index_force_update_ = true;
  }
  return true;
}

bool VulkanCommandProcessor::DiscardZPDQuery() {
  if (zpd_active_query_is_fsi_) {
    // The slot counter may be dirty if draws ran between OpenZPDQuery and
    // here, but the next OpenZPDQuery clears it before any new shader adds.
    if (cvars::occlusion_query_log) {
      XELOGI("ZPD/Vulkan: Discarding FSI counter segment index={}",
             zpd_active_query_index_);
    }
    zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                            zpd_active_query_generation_);
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    zpd_fsi_counter_index_force_update_ = true;
    return true;
  }

  if (!in_render_pass_) {
    // vkCmdEndQuery is invalid outside a render pass for occlusion queries.
    // Defer the release until the submission containing the stale BeginQuery
    // completes on the GPU.
    XELOGW("ZPD: Discard segment requested outside render pass");
    zpd_deferred_releases_.push_back({GetCurrentSubmission(),
                                      zpd_active_query_index_,
                                      zpd_active_query_generation_});
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    return true;
  }

  // Inside a render pass, EndQuery must be issued before releasing the slot.
  zpd_host_query_pool_->EndQuery(*deferred_command_buffer_,
                                 zpd_active_query_index_);
  zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                          zpd_active_query_generation_);
  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  zpd_active_query_is_fsi_ = false;
  return true;
}

void VulkanCommandProcessor::PumpQueryResolves() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (completed == 0) {
    return;
  }

  // Drain deferred releases first.
  while (!zpd_deferred_releases_.empty()) {
    auto& entry = zpd_deferred_releases_.front();
    if (entry.submission > completed) {
      break;
    }
    zpd_host_query_pool_->ReleaseQueryIndex(entry.query_index,
                                            entry.query_generation);
    zpd_deferred_releases_.pop_front();
  }

  // Invalidate CPU cache before reading results on non-coherent memory.
  if (!zpd_resolves_in_flight_.empty() &&
      zpd_resolves_in_flight_.front().submission <= completed) {
    zpd_host_query_pool_->InvalidateReadback();
  }

  while (!zpd_resolves_in_flight_.empty()) {
    if (zpd_resolves_in_flight_.front().submission > completed) {
      break;
    }
    PendingQueryResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (zpd_host_query_pool_->GenerationMatches(resolve.query_index,
                                                resolve.query_generation)) {
      uint64_t raw_samples = zpd_host_query_pool_->GetQueryReadbackValue(
          resolve.query_index, resolve.uses_fsi_counter);
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD/Vulkan: Resolved {} segment index={} samples={}",
               resolve.uses_fsi_counter ? "FSI" : "native", resolve.query_index,
               raw_samples);
      }
      zpd_host_query_pool_->ReleaseQueryIndex(resolve.query_index,
                                              resolve.query_generation);
      OnZPDQueryResolved(resolve.report_handle, raw_samples,
                         resolve.scale_area);
    }
  }
}

bool VulkanCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                               uint64_t wait_for_submission) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  PumpQueryResolves();

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return true;
  }
  if (it->second.pending_segments == 0 && it->second.ended) {
    return true;
  }
  if (wait_for_submission == 0) {
    return false;
  }

  // Ensure the submission is flushed.
  if (wait_for_submission >= GetCurrentSubmission()) {
    if (!submission_open_) {
      return false;
    }
    if (!CanEndSubmissionImmediately()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Async: Draining Vulkan async pipeline creation for strict "
            "retirement");
      }
      pipeline_cache_->AwaitPipelineCompletion();
    }
    EndRenderPass();
    if (!EndSubmission(false)) {
      return false;
    }
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    if (!AwaitSubmissionAndUpdateCompleted(wait_for_submission)) return false;
  }

  PumpQueryResolves();

  it = logical_zpd_reports_.find(report_handle);
  return it == logical_zpd_reports_.end() ||
         (it->second.pending_segments == 0 && it->second.ended);
}

void VulkanCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();
  if (shared_memory_->IsHostMapped()) {
    // As for savestates, GPU-written pages must reach guest RAM before any
    // texture reads are recorded from it, including later in this frame.
    if (!AwaitAllQueueOperationsCompletion()) {
      return;
    }
    uint32_t pages_written;
    if (!shared_memory_->WriteBackGpuWrittenToGuest(pages_written)) {
      XELOGE("Trace: failed to synchronize GPU-written memory to guest RAM");
      return;
    }
  }
  texture_cache_->TraceWriteBoundTextures(UINT32_MAX);

  if (!BeginSubmission(true)) {
    return;
  }
  // TODO(Triang3l): Write the EDRAM.
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

void VulkanCommandProcessor::LogRecentSubmissions(const char* context) {
  XELOGE(
      "VulkanCommandProcessor: recent submission history ({}) - oldest first, "
      "current in-progress last:",
      context);
  // Walk the ring starting from the oldest entry.
  for (size_t i = 0; i < kSubmissionHistorySize; ++i) {
    const SubmissionSummary& s =
        submission_history_[(submission_history_next_ + i) %
                            kSubmissionHistorySize];
    if (!s.submission_index) {
      continue;
    }
    XELOGE(
        "  sub {:>5} frame {:>5}: draws={} dispatches={} resolves={} "
        "VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
  if (submission_open_) {
    const SubmissionSummary& s = submission_in_progress_;
    XELOGE(
        "  sub {:>5} frame {:>5} (in-progress): draws={} dispatches={} "
        "resolves={} VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
}

void VulkanCommandProcessor::QueueMemexportRamMirror(bool route_to_host) {
  if (route_to_host || memexport_ranges_.empty() ||
      !cvars::vulkan_memexport_ram_mirror ||
      shared_memory_->is_zero_copy() || !shared_memory_->IsHostMapped()) {
    return;
  }
  const uint64_t submission = GetCurrentSubmission();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    memexport_mirror_pending_.push_back(
        {submission, memexport_range.base_address_dwords << 2,
         memexport_range.size_bytes});
  }
  memexport_mirror_barrier_needed_ = true;
}

void VulkanCommandProcessor::ProcessMemexportRamMirror(
    uint64_t completed_submission) {
  const uint32_t page_size = 4096;
  // The guest's write watches take the global lock, so under it a page still
  // marked GPU-written has not been written by the CPU since the export.
  auto global_lock = global_critical_region::Acquire();
  // vulkan_memexport_ram_mirror_dedup: the pages of every completed range,
  // each visited once (sorted, unique).
  const bool dedup = cvars::vulkan_memexport_ram_mirror_dedup;
  memexport_mirror_pages_scratch_.clear();
  while (!memexport_mirror_pending_.empty() &&
         memexport_mirror_pending_.front().submission <= completed_submission) {
    const MemexportMirrorRange range = memexport_mirror_pending_.front();
    memexport_mirror_pending_.pop_front();
    ++memexport_mirror_stats_.ranges;
    const uint32_t end = uint32_t(std::min(uint64_t(range.address) + range.length,
                                           uint64_t(SharedMemory::kBufferSize)));
    if (dedup) {
      for (uint32_t page = range.address & ~(page_size - 1); page < end;
           page += page_size) {
        memexport_mirror_pages_scratch_.push_back(page);
      }
      continue;
    }
    for (uint32_t page = range.address & ~(page_size - 1); page < end;
         page += page_size) {
      MirrorMemexportPage(page, completed_submission);
    }
  }
  if (dedup && !memexport_mirror_pages_scratch_.empty()) {
    std::sort(memexport_mirror_pages_scratch_.begin(),
              memexport_mirror_pages_scratch_.end());
    uint32_t previous = UINT32_MAX;
    for (uint32_t page : memexport_mirror_pages_scratch_) {
      if (page != previous) {
        MirrorMemexportPage(page, completed_submission);
        previous = page;
      }
    }
  }
  if (cvars::log_texture_clobber) {
    static uint64_t last_logged_total = 0;
    const uint64_t total =
        memexport_mirror_pages_copied_ + memexport_mirror_pages_skipped_;
    if (total >= last_logged_total + 32) {
      last_logged_total = total;
      XELOGI("[MEMEXPORT] RAM mirror: {} pages copied, {} skipped",
             memexport_mirror_pages_copied_, memexport_mirror_pages_skipped_);
    }
  }
}

void VulkanCommandProcessor::MirrorMemexportPage(
    uint32_t page, uint64_t completed_submission) {
  const uint32_t page_size = 4096;
  ++memexport_mirror_stats_.pages;
  // A newer export to this page still in flight would be read half-done -
  // its own completion copies the page.
  bool newer_in_flight = false;
  for (const MemexportMirrorRange& pending : memexport_mirror_pending_) {
    if (pending.submission > completed_submission &&
        pending.address < page + page_size &&
        uint64_t(pending.address) + pending.length > page) {
      newer_in_flight = true;
      break;
    }
  }
  if (newer_in_flight ||
      !shared_memory_->IsRangeGpuWritten(page, page_size)) {
    ++memexport_mirror_pages_skipped_;
    return;
  }
  // Only the bytes the GPU wrote: the rest of a partly exported page is
  // CPU-owned and may be newer in RAM than in the buffer.
  std::pair<uint32_t, uint32_t>
      ranges[SharedMemory::kGpuWrittenPartialRanges];
  const uint32_t count = shared_memory_->GpuWrittenPageRanges(page, ranges);
  bool copied = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t first = std::max(ranges[i].first, page);
    const uint32_t last = std::min(ranges[i].second, page + page_size);
    if (first < last &&
        shared_memory_->ReadHostMapped(first, last - first,
                                       memory_->TranslatePhysical(first))) {
      copied = true;
      memexport_mirror_stats_.bytes += last - first;
    }
  }
  if (copied) {
    ++memexport_mirror_pages_copied_;
    ++memexport_mirror_stats_.copied;
  }
}

void VulkanCommandProcessor::CheckSubmissionCompletionAndDeviceLoss(
    uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  if (cvars::log_gpu_frame_time_breakdown || !vk_submit_times_.empty()) {
    const uint64_t completed_before = GetCompletedSubmission();
    const uint64_t t0 = FrameStatsNow();
    AwaitSubmissionAndUpdateCompleted(await_submission);
    const uint64_t t1 = FrameStatsNow();
    if (!cvars::log_gpu_frame_time_breakdown && cvars::log_cp_frame_breakdown) {
      frame_time_stats_.gpu_wait_ns += t1 - t0;
    }
    if (await_submission) {
      vk_frame_sync_stats_.awaits++;
      vk_frame_sync_stats_.await_ns += t1 - t0;
      if (await_submission > completed_before) {
        // The await actually blocked; record how far behind the GPU was.
        vk_frame_sync_stats_.blocking_awaits++;
        vk_frame_sync_stats_.await_delta +=
            GetCurrentSubmission() - await_submission;
      }
    }
    const uint64_t completed = GetCompletedSubmission();
    bool timestamps_invalidated = false;
    while (!vk_submit_times_.empty() &&
           vk_submit_times_.front().submission <= completed) {
      const SubmitTimeRecord& record = vk_submit_times_.front();
      const uint64_t latency = t1 - record.submit_ns;
      vk_frame_sync_stats_.sub_latency_ns += latency;
      vk_frame_sync_stats_.sub_latency_max_ns =
          std::max(vk_frame_sync_stats_.sub_latency_max_ns, latency);
      vk_frame_sync_stats_.sub_completions++;
      if (record.timestamp_slot != UINT32_MAX && frame_timestamp_mapping_) {
        if (cvars::log_gpu_frame_time_breakdown && !timestamps_invalidated) {
          // The readback memory type is not guaranteed to be host-coherent.
          const ui::vulkan::VulkanDevice* const vulkan_device =
              GetVulkanDevice();
          VkMappedMemoryRange invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          invalidate_range.memory = frame_timestamp_buffer_memory_;
          invalidate_range.offset = 0;
          invalidate_range.size = VK_WHOLE_SIZE;
          vulkan_device->functions().vkInvalidateMappedMemoryRanges(
              vulkan_device->device(), 1, &invalidate_range);
          timestamps_invalidated = true;
        }
        // UI collection reads only after the existing submission fence completed.
        // No WAIT bit, no GPU copy command, no host query while work is pending.
        uint64_t host_timestamps[4] = {};
        const uint64_t* timestamp_values =
            frame_timestamp_mapping_ + record.timestamp_slot * 4;
        if (!cvars::log_gpu_frame_time_breakdown) {
          const auto* vd = GetVulkanDevice();
          const VkResult result = vd->functions().vkGetQueryPoolResults(vd->device(), frame_timestamp_pool_,
              record.timestamp_slot * 2, 2, sizeof(host_timestamps), host_timestamps,
              2 * sizeof(uint64_t),
              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
          if (result != VK_SUCCESS) host_timestamps[1] = host_timestamps[3] = 0;
          timestamp_values = host_timestamps;
        }
        const uint64_t ts_top = timestamp_values[0];
        const uint64_t ts_bottom = timestamp_values[2];
        const double period_ns =
            double(GetVulkanDevice()->properties().timestampPeriod);
        const bool available = timestamp_values[1] && timestamp_values[3];
        if (available) {
          const uint64_t timestamp_delta = frame_timestamp_valid_bits_ >= 64 ?
              ts_bottom - ts_top : (ts_bottom - ts_top) &
                  ((uint64_t(1) << frame_timestamp_valid_bits_) - 1);
          const uint64_t exec_ns = uint64_t(timestamp_delta * period_ns);
          gpu_frame_timer_.AddTimestampSpan(ts_top, ts_bottom,
              frame_timestamp_valid_bits_, period_ns);
          if (cvars::adaptive_shading_rate) {
            adaptive_shading_busy_ns_ += uint64_t(
                adaptive_shading_spans_.Add(ts_top, ts_bottom,
                                            frame_timestamp_valid_bits_) *
                period_ns);
          }
          vk_frame_sync_stats_.gpu_exec_ns += exec_ns;
          vk_frame_sync_stats_.gpu_exec_max_ns =
              std::max(vk_frame_sync_stats_.gpu_exec_max_ns, exec_ns);
          vk_frame_sync_stats_.gpu_samples++;
        } else {
          gpu_frame_timer_.MarkUnavailable();
        }
        if (available && frame_timestamp_prev_end_ && ts_top > frame_timestamp_prev_end_) {
          vk_frame_sync_stats_.gpu_gap_ns +=
              uint64_t((ts_top - frame_timestamp_prev_end_) * period_ns);
        }
        if (available) frame_timestamp_prev_end_ = ts_bottom;
        if (record.resolve_pair_count && resolve_timestamp_mapping_) {
          VkMappedMemoryRange resolve_invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          resolve_invalidate_range.memory = resolve_timestamp_buffer_memory_;
          const VkDeviceSize atom =
              GetVulkanDevice()->properties().nonCoherentAtomSize;
          const VkDeviceSize first_byte =
              VkDeviceSize(record.resolve_slot_base) * 2 * sizeof(uint64_t);
          const VkDeviceSize end_byte =
              first_byte + VkDeviceSize(record.resolve_pair_count) *
                               2 * sizeof(uint64_t);
          resolve_invalidate_range.offset = first_byte / atom * atom;
          resolve_invalidate_range.size =
              std::min(xe::round_up(end_byte, atom),
                       resolve_timestamp_buffer_size_) -
              resolve_invalidate_range.offset;
          GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
              GetVulkanDevice()->device(), 1, &resolve_invalidate_range);
          for (uint32_t i = 0; i < record.resolve_pair_count; ++i) {
            const uint64_t r0 =
                resolve_timestamp_mapping_[(record.resolve_slot_base + i) * 2];
            const uint64_t r1 =
                resolve_timestamp_mapping_[(record.resolve_slot_base + i) * 2 +
                                           1];
            if (r1 > r0) {
              const uint64_t resolve_ns = uint64_t((r1 - r0) * period_ns);
              const uint8_t ts_kind =
                  resolve_ts_kind_[record.resolve_slot_base + i];
              const uint32_t format =
                  resolve_ts_texture_format_[record.resolve_slot_base + i];
              if (format < 64) {
                auto& stat = vk_frame_sync_stats_.resolve_texture[format];
                if (ts_kind == 3) stat.reload_ns += resolve_ns;
                if (ts_kind == 4) stat.copy_ns += resolve_ns;
              }
              if (ts_kind == 4) {
                continue;  // Counted separately from the enclosing resolve.
              }
              if (ts_kind >= 1 && ts_kind <= 3) {
                vk_frame_sync_stats_.texload_kind_ns[ts_kind - 1] += resolve_ns;
                vk_frame_sync_stats_.texload_kind_samples[ts_kind - 1]++;
                vk_frame_sync_stats_.texload_gpu_ns += resolve_ns;
                vk_frame_sync_stats_.texload_gpu_max_ns = std::max(
                    vk_frame_sync_stats_.texload_gpu_max_ns, resolve_ns);
                vk_frame_sync_stats_.texload_gpu_samples++;
              } else {
                vk_frame_sync_stats_.resolve_gpu_ns += resolve_ns;
                vk_frame_sync_stats_.resolve_gpu_max_ns = std::max(
                    vk_frame_sync_stats_.resolve_gpu_max_ns, resolve_ns);
                vk_frame_sync_stats_.resolve_gpu_samples++;
              }
            }
          }
        }
        if (record.pass_pair_count && pass_timestamp_mapping_) {
          VkMappedMemoryRange pass_invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          pass_invalidate_range.memory = pass_timestamp_buffer_memory_;
          const VkDeviceSize atom =
              GetVulkanDevice()->properties().nonCoherentAtomSize;
          const VkDeviceSize first_byte =
              VkDeviceSize(record.pass_slot_base) * 2 * sizeof(uint64_t);
          const VkDeviceSize end_byte =
              first_byte + VkDeviceSize(record.pass_pair_count) *
                               2 * sizeof(uint64_t);
          pass_invalidate_range.offset = first_byte / atom * atom;
          pass_invalidate_range.size =
              std::min(xe::round_up(end_byte, atom),
                       pass_timestamp_buffer_size_) -
              pass_invalidate_range.offset;
          GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
              GetVulkanDevice()->device(), 1, &pass_invalidate_range);
          const bool log_pass_sequence =
              cvars::log_pass_sequence_every &&
              (record.submission % cvars::log_pass_sequence_every) < 2;
          uint64_t pass_sequence_prev_end = 0;
          for (uint32_t i = 0; i < record.pass_pair_count; ++i) {
            const uint32_t pair = record.pass_slot_base + i;
            const uint64_t p0 = pass_timestamp_mapping_[pair * 2];
            const uint64_t p1 = pass_timestamp_mapping_[pair * 2 + 1];
            if (log_pass_sequence && p1 > p0) {
              const uint32_t key = pass_ts_keys_[pair];
              XELOGI(
                  "VkPassSeq sub={} #{} {}{}x{} draws={} sc={}x{} vp={}x{} "
                  "gpu={:.3f}ms gap_before={:.3f}ms",
                  record.submission, i,
                  (key & kPassTimestampGapKey) ? "gap "
                  : (key & 0x80000000u)        ? "xfer "
                                               : "",
                  (key >> 16) & 0x7FFF, key & 0xFFFF, pass_ts_draws_[pair],
                  pass_ts_scissor_[pair] >> 16, pass_ts_scissor_[pair] & 0xFFFF,
                  pass_ts_viewport_[pair] >> 16,
                  pass_ts_viewport_[pair] & 0xFFFF, (p1 - p0) * period_ns / 1e6,
                  pass_sequence_prev_end && p0 > pass_sequence_prev_end
                      ? (p0 - pass_sequence_prev_end) * period_ns / 1e6
                      : 0.0);
            }
            if (p1 > p0) {
              pass_sequence_prev_end = p1;
            }
            if (p1 > p0) {
              auto& bucket = pass_bucket_stats_[pass_ts_keys_[pair]];
              bucket.ns += uint64_t((p1 - p0) * period_ns);
              bucket.passes++;
              bucket.draws += pass_ts_draws_[pair];
              bucket.max_scissor_w =
                  std::max(bucket.max_scissor_w, pass_ts_scissor_[pair] >> 16);
              bucket.max_scissor_h = std::max(bucket.max_scissor_h,
                                              pass_ts_scissor_[pair] & 0xFFFF);
              bucket.max_viewport_w = std::max(bucket.max_viewport_w,
                                               pass_ts_viewport_[pair] >> 16);
              bucket.max_viewport_h = std::max(
                  bucket.max_viewport_h, pass_ts_viewport_[pair] & 0xFFFF);
            }
          }
        }
      }
      vk_submit_times_.pop_front();
    }
  } else if (cvars::log_cp_frame_breakdown) {
    const uint64_t t0 = FrameStatsNow();
    AwaitSubmissionAndUpdateCompleted(await_submission);
    frame_time_stats_.gpu_wait_ns += FrameStatsNow() - t0;
  } else {
    AwaitSubmissionAndUpdateCompleted(await_submission);
  }

  if (device_lost_) return;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  if (vulkan_device->IsLost()) {
    XELOGE(
        "VulkanCommandProcessor: device lost observed in submission-check - "
        "awaiting submission {}, current {}, completed {}, in-flight {}, "
        "frame {} (frame_open: {}, submission_open: {})",
        await_submission, GetCurrentSubmission(), GetCompletedSubmission(),
        command_buffers_submitted_.size(), frame_current_, frame_open_,
        submission_open_);
    LogRecentSubmissions("submission-check");
    device_lost_ = true;
    graphics_system_->OnHostGpuLossFromAnyThread(true);
    return;
  }

  const uint64_t completed_submission = GetCompletedSubmission();
  if (!memexport_mirror_pending_.empty()) {
    ProcessMemexportRamMirror(completed_submission);
  }

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission =
        submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > completed_submission) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > completed_submission) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }
  while (!stream_command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = stream_command_buffers_submitted_.front();
    if (command_buffer_pair.first > completed_submission) {
      break;
    }
    stream_command_buffers_writable_.push_back(command_buffer_pair.second);
    stream_command_buffers_submitted_.pop_front();
  }
  {
    // Reuse is LIFO from the back, so the front holds the longest idle pools.
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    const size_t spares = cvars::vulkan_command_pool_spares;
    static size_t pools_destroyed = 0;
    for (auto* writable :
         {&command_buffers_writable_, &stream_command_buffers_writable_}) {
      if (spares && writable->size() > spares) {
        size_t excess = writable->size() - spares;
        for (size_t i = 0; i < excess; ++i) {
          dfn.vkDestroyCommandPool(device, (*writable)[i].pool, nullptr);
        }
        writable->erase(writable->begin(), writable->begin() + excess);
        pools_destroyed += excess;
      }
    }
    if (cvars::log_vk_memory) {
      static uint32_t pools_log_counter = 0;
      if (!(++pools_log_counter & 4095)) {
        XELOGI("CmdPools: setup idle {} in flight {}, stream idle {} in "
               "flight {}, destroyed {}",
               command_buffers_writable_.size(),
               command_buffers_submitted_.size(),
               stream_command_buffers_writable_.size(),
               stream_command_buffers_submitted_.size(), pools_destroyed);
      }
    }
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(completed_submission);

  while (!bindless_retired_.empty() &&
         bindless_retired_.front().submission <= completed_submission) {
    const BindlessRetiredSlot& retired = bindless_retired_.front();
    bindless_arrays_[retired.array].free_slots.push_back(retired.slot);
    bindless_retired_.pop_front();
  }

  // Reclaim descriptor pools that the GPU has finished using.
  if (resolve_downscale_descriptor_pool_chain_) {
    resolve_downscale_descriptor_pool_chain_->Reclaim(completed_submission);
  }

  PumpQueryResolves();

  // Destroy objects scheduled for destruction.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

void VulkanCommandProcessor::RefreshDrawCensusConfig() {
  draw_census_enabled_ = cvars::log_draw_census;
}

void VulkanCommandProcessor::RefreshDrawConfig() {
  DrawConfig next{};
  next.async_shader_skip_draws = cvars::async_shader_skip_draws;
  next.async_shader_vs_interpreter = cvars::async_shader_vs_interpreter;
  next.debug_draw_mode = cvars::debug_draw_mode;
  next.gpu_allow_invalid_fetch_constants = cvars::gpu_allow_invalid_fetch_constants;
  next.log_draw_state_repeats = cvars::log_draw_state_repeats;
  next.log_memexport_draws = cvars::log_memexport_draws;
  next.log_pixel_shader_paths = cvars::log_pixel_shader_paths;
  next.log_texture_clobber = cvars::log_texture_clobber;
  next.log_vertex_shader_hotness = cvars::log_vertex_shader_hotness;
  next.rt_cache_full_overwrite_await_pipeline = cvars::rt_cache_full_overwrite_await_pipeline;
  next.spirv_texture_exp_adjust_specialization = cvars::spirv_texture_exp_adjust_specialization;
  next.spirv_texture_sign_specialization = cvars::spirv_texture_sign_specialization;
  next.spirv_texture_sign_specialization_used = cvars::spirv_texture_sign_specialization_used;
  next.vulkan_allow_reverse_z = cvars::vulkan_allow_reverse_z;
  next.vulkan_async_skip_draws = cvars::vulkan_async_skip_draws;
  next.vulkan_debug_disable_alpha_to_mask = cvars::vulkan_debug_disable_alpha_to_mask;
  next.vulkan_defer_descriptor_updates = cvars::vulkan_defer_descriptor_updates;
  next.vulkan_depth_4x_as_1x = cvars::vulkan_depth_4x_as_1x;
  next.vulkan_msaa_4x_as_2x2 = cvars::vulkan_msaa_4x_as_2x2;
  next.vulkan_fragment_shading_rate = cvars::vulkan_fragment_shading_rate;
  next.vulkan_fragment_shading_rate_alpha_tested = cvars::vulkan_fragment_shading_rate_alpha_tested;
  next.vulkan_in_pass_resolve_debug_read_usage = cvars::vulkan_in_pass_resolve_debug_read_usage;
  next.vulkan_memexport_compute_barrier = cvars::vulkan_memexport_compute_barrier;
  next.vulkan_memexport_vs_compute = cvars::vulkan_memexport_vs_compute;
  next.vulkan_mid_frame_submission_draws = cvars::vulkan_mid_frame_submission_draws;
  next.vulkan_placeholder_pipelines = cvars::vulkan_placeholder_pipelines;
  next.vulkan_relax_unused_attachment_ops = cvars::vulkan_relax_unused_attachment_ops;
  next.vulkan_render_area_draw_bound = cvars::vulkan_render_area_draw_bound;
  next.vulkan_render_area_tight = cvars::vulkan_render_area_tight;
  next.vulkan_alpha_tested_depth_prepass =
      cvars::vulkan_alpha_tested_depth_prepass;
  next.vulkan_republish_draw_inputs = cvars::vulkan_republish_draw_inputs;
  next.vulkan_reuse_index_buffer_binding = cvars::vulkan_reuse_index_buffer_binding;
  next.vulkan_unclipped_fallback = cvars::vulkan_unclipped_fallback;
  draw_config_ = next;
  if (render_target_cache_) {
    render_target_cache_->SetMsaa4xClearsAs2x2(
        next.vulkan_msaa_4x_as_2x2 && cvars::vulkan_msaa_4x_clears_as_2x2);
  }
  RefreshDrawStatsConfig();
  register_file_->InvalidateDerivedState();
  draw_state_cache_.valid = false;
  vertex_input_shader_ = nullptr;
  for (auto& input : vertex_input_ranges_) input.memory.epoch = UINT64_MAX;
  draw_dynamic_state_dirty_ = draw_system_state_dirty_ = true;
  current_samplers_fetch_up_to_date_vertex_ = 0;
  current_samplers_fetch_up_to_date_pixel_ = 0;
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  RefreshDrawConfig();

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  // vulkan_frames_in_flight below kMaxFramesInFlight awaits a newer frame.
  const uint32_t frames_in_flight = uint32_t(std::clamp(
      cvars::vulkan_frames_in_flight, 1, int32_t(kMaxFramesInFlight)));
  uint64_t await_submission =
      is_opening_frame
          ? closed_frame_submissions_[(frame_current_ + kMaxFramesInFlight -
                                       frames_in_flight) %
                                      kMaxFramesInFlight]
          : 0;
  {
    cp_wait_stats::FenceSiteScope fence_site(cp_wait_stats::kFenceFrame);
    CheckSubmissionCompletionAndDeviceLoss(await_submission);
  }
  const uint64_t completed_submission = GetCompletedSubmission();
  if (!memexport_mirror_pending_.empty()) {
    ProcessMemexportRamMirror(completed_submission);
  }
  if (device_lost_ || completed_submission < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    pass_bytes_active_ = cvars::log_pass_bytes > 0;
    pass_bytes_.Reset();
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                       kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
          completed_submission) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    submission_in_progress_ = SubmissionSummary{};
    submission_in_progress_.submission_index = GetCurrentSubmission();
    submission_in_progress_.frame_index = frame_current_;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_->Reset();
    deferred_setup_command_buffer_.Reset();
    deferred_command_buffer_->SetPassBytesEnabled(pass_bytes_active_);
    deferred_setup_command_buffer_.SetPassBytesEnabled(pass_bytes_active_);
    // Clear merging and store lookahead patch earlier begins at submission
    // end. Keep the whole stream writable until then, before any replay.
    if (stream_encode_enabled_ &&
        (!cvars::vulkan_fold_clears_to_load_op ||
         cvars::vulkan_stream_encode_with_fold) &&
        !cvars::vulkan_store_lookahead) {
      stream_encode_active_ = StreamEncodeBegin();
    }
    if (shared_memory_) {
      shared_memory_->OnGpuSubmissionOpened();
    }

    // Reset cached state of the command buffer.
    bound_index_buffer_ = VK_NULL_HANDLE;
    draw_dynamic_state_dirty_ = true;
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    dynamic_primitive_topology_update_needed_ = true;
    dynamic_primitive_restart_enable_update_needed_ = true;
    dynamic_cull_mode_update_needed_ = true;
    dynamic_front_face_update_needed_ = true;
    dynamic_depth_test_enable_update_needed_ = true;
    dynamic_depth_write_enable_update_needed_ = true;
    dynamic_depth_compare_op_update_needed_ = true;
    dynamic_stencil_test_enable_update_needed_ = true;
    dynamic_stencil_op_front_update_needed_ = true;
    dynamic_stencil_op_back_update_needed_ = true;
    dynamic_depth_clamp_enable_update_needed_ = true;
    dynamic_polygon_mode_update_needed_ = true;
    dynamic_fragment_shading_rate_update_needed_ = true;
    dynamic_color_blend_enable_update_needed_ = true;
    dynamic_color_blend_equation_update_needed_ = true;
    dynamic_color_write_mask_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    pass_head_slot_ = UINT32_MAX;
    // Fresh deferred command buffer: abandon any open pass-timestamp pair
    // without emitting an end (it would land in the new, unrelated buffer).
    pass_ts_open_pair_ = UINT32_MAX;
    current_guest_graphics_pipeline_ = nullptr;
    current_guest_graphics_pipeline_handle_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    if (cvars::vulkan_texture_descriptor_cache_per_submission) {
      current_texture_descriptors_valid_vertex_ = false;
      current_texture_descriptors_valid_pixel_ = false;
      current_graphics_descriptor_set_values_up_to_date_ &=
          ~((UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
            (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
    }

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Conservatively re-derive all cached sampler parameters in the new
    // frame (cheap - one derivation pass per stage; the handles still go
    // through their per-submission UseSampler revalidation independently).
    current_samplers_fetch_up_to_date_vertex_ = 0;
    current_samplers_fetch_up_to_date_pixel_ = 0;

    // Log guest ZPD report stats every 100 frames.
    if (GetZPDMode() != ZPDMode::kFake && cvars::occlusion_query_log &&
        zpd_host_query_pool_ && zpd_host_query_pool_->capacity() &&
        frame_current_ - zpd_stats_.last_log_frame >= 100) {
      XELOGI(
          "Occlusion Query Stats (last 100 frames): "
          "LogicalBegun={}, LogicalEnded={}, SegBegun={}, SegEnded={}, "
          "PoolExhausted={}, Failed={}, Wraps={}, SameSlotReuse={}",
          zpd_stats_.logical_begun, zpd_stats_.logical_ended,
          zpd_stats_.segments_begun, zpd_stats_.segments_ended,
          zpd_stats_.pool_exhausted, zpd_stats_.failed,
          zpd_stats_.counter_wraps, zpd_stats_.same_slot_reuse);

      zpd_stats_.Reset(frame_current_);
    }

    // Reset bindings that depend on transient data.
    std::memset(current_graphics_descriptor_sets_, 0,
                sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    VkDescriptorSet submission_shared_memory_set =
        shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
            submission_shared_memory_set;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1)
        << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;
    // MANDATORY: the transient descriptor pool resets at the frame boundary, so
    // every cached texture set written in the previous frame is now invalid.
    // Invalidate in lockstep with the values bitmask reset and the
    // current_graphics_descriptor_sets_ memset above, otherwise a first-draw
    // whose content hash equals the previous frame's could skip the rewrite and
    // sample recycled transient-pool memory (the #1 corruption path).
    current_texture_descriptors_valid_vertex_ = false;
    current_texture_descriptors_valid_pixel_ = false;
    // Same MANDATORY invalidation for the constants value cache: the cached
    // transient set and the page buffers it references are reclaimable at the
    // frame boundary (the current_graphics_descriptor_sets_ memset above already
    // dropped the cached handle), so any stale per-binding VkBuffer must not let
    // a first draw skip the rewrite.
    constants_descriptor_set_valid_ = false;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(
                                             used_transient_descriptor.layout)]
          .push_back(used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(
          used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(
          used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it =
            texture_transient_descriptor_sets_free_
                .emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(used_transient_descriptor_set.layout),
                    std::forward_as_tuple())
                .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_ ||
         !pipeline_cache_->IsCreatingPipelines();
}

bool VulkanCommandProcessor::StreamEncodeBegin() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (stream_command_buffers_writable_.empty()) {
    CommandBuffer command_buffer;
    VkCommandPoolCreateInfo command_pool_create_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool_create_info.queueFamilyIndex =
        vulkan_device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                &command_buffer.pool) != VK_SUCCESS) {
      XELOGE("Failed to create a Vulkan command pool for streamed encoding");
      return false;
    }
    VkCommandBufferAllocateInfo command_buffer_allocate_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_buffer_allocate_info.commandPool = command_buffer.pool;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                     &command_buffer.buffer) != VK_SUCCESS) {
      XELOGE("Failed to allocate a Vulkan command buffer for streamed encoding");
      dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      return false;
    }
    stream_command_buffers_writable_.push_back(command_buffer);
  }
  CommandBuffer command_buffer = stream_command_buffers_writable_.back();
  if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
    XELOGE("Failed to reset a Vulkan command pool for streamed encoding");
    return false;
  }
  VkCommandBufferBeginInfo command_buffer_begin_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(command_buffer.buffer,
                               &command_buffer_begin_info) != VK_SUCCESS) {
    XELOGE("Failed to begin a Vulkan command buffer for streamed encoding");
    return false;
  }
  stream_command_buffer_ = command_buffer;
  std::lock_guard<std::mutex> lock(stream_encode_mutex_);
  assert_false(stream_encode_busy_);
  stream_encode_target_ = command_buffer.buffer;
  stream_encode_source_ = deferred_command_buffer_;
  stream_encode_published_ = 0;
  stream_encode_encoded_ = 0;
  stream_encode_pipeline_unready_ = false;
  return true;
}

void VulkanCommandProcessor::StreamEncodePublish(bool submission_end) {
  if (!stream_encode_active_) {
    return;
  }
  size_t recorded = deferred_command_buffer_->size_elements();
  if (cvars::vulkan_fold_clears_to_load_op) {
    // Only this thread advances the published offset. Clear-only passes merge
    // into their successors before replay; a trailing one waits for the next
    // pass unless the submission ends.
    const size_t hold = deferred_command_buffer_->MergeClassicClearPasses(
        *render_target_cache_, stream_encode_published_);
    if (!submission_end && hold != SIZE_MAX) {
      recorded = std::min(recorded, hold);
    }
  }
  {
    std::lock_guard<std::mutex> lock(stream_encode_mutex_);
    if (recorded <= stream_encode_published_) {
      return;
    }
    stream_encode_published_ = recorded;
    if (!stream_encode_sleeping_) {
      // Busy encoding - it rechecks published under the lock when done.
      return;
    }
  }
  stream_encode_work_cond_.notify_one();
}

void VulkanCommandProcessor::StreamEncodeAwaitIdle() {
  if (!stream_encode_enabled_) {
    return;
  }
  std::unique_lock<std::mutex> lock(stream_encode_mutex_);
  cp_wait_stats::Scope encode_wait(cp_wait_stats::kEncode);
  stream_encode_idle_cond_.wait(lock, [this]() {
    return !stream_encode_busy_ &&
           (stream_encode_target_ == VK_NULL_HANDLE ||
            stream_encode_encoded_ >= stream_encode_published_);
  });
}

void VulkanCommandProcessor::StreamEncodeThreadMain() {
  pthread_setname_np(pthread_self(), "GPU Encode");
  PinCurrentThreadToFastCores("GPU Encode");
  std::unique_lock<std::mutex> lock(stream_encode_mutex_);
  while (true) {
    stream_encode_sleeping_ = true;
    stream_encode_work_cond_.wait(lock, [this]() {
      return stream_encode_stop_ ||
             (stream_encode_target_ != VK_NULL_HANDLE &&
              stream_encode_encoded_ < stream_encode_published_);
    });
    stream_encode_sleeping_ = false;
    if (stream_encode_stop_) {
      break;
    }
    VkCommandBuffer target = stream_encode_target_;
    auto* const source = stream_encode_source_;
    size_t begin = stream_encode_encoded_;
    size_t end = stream_encode_published_;
    stream_encode_busy_ = true;
    lock.unlock();
    source->ExecuteRange(target, begin, end, stream_encode_pipeline_unready_);
    lock.lock();
    stream_encode_encoded_ = end;
    stream_encode_busy_ = false;
    if (stream_encode_encoded_ >= stream_encode_published_) {
      stream_encode_idle_cond_.notify_all();
    }
  }
}

void VulkanCommandProcessor::StreamEncodeShutdown() {
  if (!stream_encode_thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(stream_encode_mutex_);
    stream_encode_stop_ = true;
  }
  stream_encode_work_cond_.notify_one();
  stream_encode_thread_.join();
  deferred_command_buffer_->SetBeforeReallocate(nullptr);
  stream_encode_enabled_ = false;
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  FlushDeferredResolve(ResolveDeferStop::kSubmission);
  if (resolve_defer_census_.active) {
    if (resolve_defer_census_.recording) {
      resolve_defer_census_.reasons |= kResolveDeferUnknown;
    } else {
      ResolveDeferFinish();
    }
  }
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (submission_open_) {
    render_target_cache_->FlushPendingClearTransfers();
  }
  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr,
                                &semaphore) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex =
          vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        XELOGE("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    EndRenderPass(is_swap ? "swap" : "submission");

    if (memexport_mirror_barrier_needed_) {
      // Make this submission's memory export and resolve writes visible to
      // host reads.
      memexport_mirror_barrier_needed_ = false;
      PushBufferMemoryBarrier(
          shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
          guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
      SubmitBarriers(true);
    }

    PassByteCatalog pass_byte_catalog;
    if (pass_bytes_active_) {
      render_target_cache_->DescribePassBytes(pass_byte_catalog);
      texture_cache_->DescribePassBytes(pass_byte_catalog);
    }
    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();
    if (pass_bytes_active_) {
      // Descriptions were captured before retirement. Read finalized pass
      // arguments after all patches/tail appends and the setup upload flush.
      // Setup uploads execute first, independently of their recording time.
      deferred_setup_command_buffer_.CollectPassBytes(pass_byte_catalog, pass_bytes_);
      deferred_command_buffer_->CollectPassBytes(pass_byte_catalog, pass_bytes_);
    }

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount =
          uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty()
              ? sparse_buffer_bind_infos_temp_.data()
              : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(
                vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result = dfn.vkQueueBindSparse(
            queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        XELOGE("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(
          sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }
    // Can't cross command buffer boundaries. Close the active segment first.
    CloseQuerySegment();

    SubmitBarriers(true);

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer,
                                 &command_buffer_begin_info) != VK_SUCCESS) {
      XELOGE("Failed to begin a Vulkan command buffer");
      return false;
    }
    uint32_t fs_timestamp_slot = UINT32_MAX;
    if (frame_timestamp_mapping_ &&
        (cvars::log_gpu_frame_time_breakdown || gpu_timing_enabled.load() ||
         cvars::adaptive_shading_rate)) {
      fs_timestamp_slot =
          uint32_t(GetCurrentSubmission() % kFrameTimestampSlots);
      dfn.vkCmdResetQueryPool(command_buffer.buffer, frame_timestamp_pool_,
                              fs_timestamp_slot * 2, 2);
      dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              frame_timestamp_pool_, fs_timestamp_slot * 2);
      if (resolve_timestamp_mapping_) {
        // Reset this submission's resolve-pair range before the deferred
        // buffer (which contains the writes) replays.
        dfn.vkCmdResetQueryPool(
            command_buffer.buffer, resolve_timestamp_pool_,
            uint32_t(GetCurrentSubmission() % kResolveTimestampRingSubmissions) *
                kResolveTimestampPairsPerSubmission * 2,
            kResolveTimestampPairsPerSubmission * 2);
      }
      if (pass_timestamp_mapping_) {
        dfn.vkCmdResetQueryPool(
            command_buffer.buffer, pass_timestamp_pool_,
            uint32_t(GetCurrentSubmission() % kPassTimestampRingSubmissions) *
                kPassTimestampPairsPerSubmission * 2,
            kPassTimestampPairsPerSubmission * 2);
      }
    }
    // Submission boundary for asynchronously created pipelines: by default
    // this waits until every pipeline referenced by the recorded deferred
    // binds has finished creation, so no draw is lost. With
    // vulkan_async_skip_draws (or during the startup storage preload) it
    // doesn't block, and deferred binds that still resolve to VK_NULL_HANDLE
    // at replay drop their draws. Also persists the driver pipeline cache
    // (throttled) and flushes the shader/pipeline storage files.
    pipeline_cache_->EndSubmission();
    if (!deferred_setup_command_buffer_.empty()) {
      deferred_setup_command_buffer_.Execute(command_buffer.buffer);
      VkMemoryBarrier setup_barrier;
      setup_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      setup_barrier.pNext = nullptr;
      setup_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      setup_barrier.dstAccessMask =
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      dfn.vkCmdPipelineBarrier(
          command_buffer.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &setup_barrier, 0, nullptr,
          0, nullptr);
    }
    // The commands recorded after the guest ones go to the end of the last
    // command buffer of the submission.
    VkCommandBuffer tail_command_buffer = command_buffer.buffer;
    if (stream_encode_active_) {
      StreamEncodePublish(true);
      StreamEncodeAwaitIdle();
      tail_command_buffer = stream_command_buffer_.buffer;
      if (cvars::vulkan_fold_clears_to_load_op) {
        deferred_command_buffer_->LogClassicPassOps();
      }
    } else {
      if (cvars::vulkan_fold_clears_to_load_op) {
        deferred_command_buffer_->MergeClassicClearPasses(
            *render_target_cache_);
      }
      if (cvars::vulkan_store_lookahead) {
        deferred_command_buffer_->ApplyClassicStoreLookahead(
            *render_target_cache_);
      }
      if (cvars::vulkan_fold_clears_to_load_op ||
          cvars::vulkan_store_lookahead) {
        deferred_command_buffer_->LogClassicPassOps();
      }
      deferred_command_buffer_->Execute(command_buffer.buffer);
    }

    // Record ZPD resolves before submitting.
    if (zpd_host_query_pool_) {
      zpd_host_query_pool_->RecordResolveBatch(tail_command_buffer);
    }

    if (fs_timestamp_slot != UINT32_MAX) {
      dfn.vkCmdWriteTimestamp(tail_command_buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              frame_timestamp_pool_, fs_timestamp_slot * 2 + 1);
      if (cvars::log_gpu_frame_time_breakdown) {
        dfn.vkCmdCopyQueryPoolResults(tail_command_buffer, frame_timestamp_pool_, fs_timestamp_slot * 2,
            2, frame_timestamp_buffer_,
            fs_timestamp_slot * 4 * sizeof(uint64_t), 2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
                VK_QUERY_RESULT_WAIT_BIT);
      }
      if (resolve_timestamp_mapping_ &&
          resolve_ts_submission_ == GetCurrentSubmission() &&
          resolve_ts_count_) {
        // Copy only the pairs actually written this submission - reset-but-
        // unwritten queries under WAIT_BIT would hang the GPU.
        const uint32_t resolve_ts_base =
            uint32_t(GetCurrentSubmission() % kResolveTimestampRingSubmissions) *
            kResolveTimestampPairsPerSubmission;
        dfn.vkCmdCopyQueryPoolResults(tail_command_buffer, resolve_timestamp_pool_, resolve_ts_base * 2,
            resolve_ts_count_ * 2, resolve_timestamp_buffer_,
            resolve_ts_base * 2 * sizeof(uint64_t), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      if (pass_timestamp_mapping_ &&
          pass_ts_submission_ == GetCurrentSubmission() && pass_ts_count_) {
        // Only fully-closed pairs are counted in pass_ts_count_; a pass still
        // open across the split keeps its begin query but is not copied here.
        const uint32_t pass_ts_base =
            uint32_t(GetCurrentSubmission() % kPassTimestampRingSubmissions) *
            kPassTimestampPairsPerSubmission;
        dfn.vkCmdCopyQueryPoolResults(tail_command_buffer, pass_timestamp_pool_, pass_ts_base * 2,
            pass_ts_count_ * 2, pass_timestamp_buffer_,
            pass_ts_base * 2 * sizeof(uint64_t), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      if (cvars::log_gpu_frame_time_breakdown) {
        VkMemoryBarrier fs_timestamp_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        fs_timestamp_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fs_timestamp_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        dfn.vkCmdPipelineBarrier(tail_command_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                 &fs_timestamp_barrier, 0, nullptr, 0, nullptr);
      }
    }

    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      XELOGE("Failed to end a Vulkan command buffer");
      return false;
    }
    if (stream_encode_active_ &&
        dfn.vkEndCommandBuffer(stream_command_buffer_.buffer) != VK_SUCCESS) {
      XELOGE("Failed to end the streamed Vulkan command buffer");
      return false;
    }

    const uint64_t submission_index = GetCurrentSubmission();

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount =
          uint32_t(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask =
          current_submission_wait_stage_masks_.data();
    }
    // Setup commands first, then the streamed guest commands.
    const VkCommandBuffer submit_command_buffers[2] = {
        command_buffer.buffer, stream_command_buffer_.buffer};
    submit_info.commandBufferCount = stream_encode_active_ ? 2 : 1;
    submit_info.pCommandBuffers = submit_command_buffers;
    VkResult submit_result = completion_timeline_.AcquireFenceAndSubmit(
        vulkan_device->queue_family_graphics_compute(), 0, 1, &submit_info);
    if (submit_result != VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to submit a Vulkan command buffer - "
          "VkResult: {} (0x{:08X}), submission: {} (completed: {}, in-flight: "
          "{}), frame: {} (frame_open: {}, is_closing_frame: {}), "
          "wait_semaphores: {}, draw_resolution_scale: {}x{}",
          static_cast<int32_t>(submit_result),
          static_cast<uint32_t>(submit_result), GetCurrentSubmission(),
          GetCompletedSubmission(), command_buffers_submitted_.size(),
          frame_current_, frame_open_, is_closing_frame,
          submit_info.waitSemaphoreCount,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_x()
                               : 0,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_y()
                               : 0);
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        XELOGE(
            "VK_ERROR_DEVICE_LOST - GPU crashed or hung. This may be caused by "
            "an invalid shader, out-of-bounds memory access, or driver bug.");
      }
      LogRecentSubmissions("submit-failure");
      if (vulkan_device->IsLost() && !device_lost_) {
        device_lost_ = true;
        graphics_system_->OnHostGpuLossFromAnyThread(true);
      }
      return false;
    }
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_index,
                                                     semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_index, command_buffer);
    command_buffers_writable_.pop_back();
    if (stream_encode_active_) {
      stream_command_buffers_submitted_.emplace_back(submission_index,
                                                     stream_command_buffer_);
      stream_command_buffers_writable_.pop_back();
      stream_command_buffer_ = {};
      stream_encode_active_ = false;
      std::lock_guard<std::mutex> lock(stream_encode_mutex_);
      stream_encode_target_ = VK_NULL_HANDLE;
    }

    submission_history_[submission_history_next_] = submission_in_progress_;
    submission_history_next_ =
        (submission_history_next_ + 1) % kSubmissionHistorySize;

    // Mark descriptor pool chains with submission index for reclaim tracking.
    if (resolve_downscale_descriptor_pool_chain_) {
      resolve_downscale_descriptor_pool_chain_->EndSubmission(submission_index);
    }

    submission_open_ = false;
    draws_since_submission_ = 0;
    vk_frame_sync_stats_.submissions++;
    if (cvars::log_gpu_frame_time_breakdown || fs_timestamp_slot != UINT32_MAX) {
      uint32_t resolve_base = 0;
      uint32_t resolve_pairs = 0;
      if (resolve_timestamp_mapping_ &&
          resolve_ts_submission_ == submission_index) {
        resolve_pairs = resolve_ts_count_;
        resolve_base =
            uint32_t(submission_index % kResolveTimestampRingSubmissions) *
            kResolveTimestampPairsPerSubmission;
      }
      uint32_t pass_base = 0;
      uint32_t pass_pairs = 0;
      if (pass_timestamp_mapping_ && pass_ts_submission_ == submission_index) {
        pass_pairs = pass_ts_count_;
        pass_base =
            uint32_t(submission_index % kPassTimestampRingSubmissions) *
            kPassTimestampPairsPerSubmission;
      }
      vk_submit_times_.push_back({submission_index, FrameStatsNow(),
                                  fs_timestamp_slot, resolve_base,
                                  resolve_pairs, pass_base, pass_pairs});
      resolve_ts_count_ = 0;
      pass_ts_count_ = 0;
    }

    // Process any ZPD resolves that completed with this submission.
    // Block if strict mode has a pending result waiting on the guest sentinel.
    PumpQueryResolves();
    PumpPendingRetire();
  }

  if (is_closing_frame) {
    if (pass_bytes_active_) {
      for (const auto& line : pass_bytes_.Finish(frame_current_).lines) XELOGI("{}", line);
      if (cvars::log_pass_bytes > 0) --cvars::log_pass_bytes;
      pass_bytes_active_ = false;
    }
    if (cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }

    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] =
        GetCurrentSubmission() - 1;

    // RTTRACE one-shot diagnostic: arm/disarm at guest frame boundaries.
    // Checked at most once per frame boundary - when the cvar is empty this
    // is a single std::filesystem::path::empty() call.
    if (rt_trace_active_) {
      RtTraceFlushRun();
      XELOGI("RTTRACE end frame={} draws={} runs={}", rt_trace_frame_,
             rt_trace_draw_count_, rt_trace_run_count_);
      rt_trace_active_ = false;
      pm4_trace_active_ = false;
      render_target_cache_->SetRtTraceClaims(false);
      rt_trace_done_ = true;
    } else if (!rt_trace_done_ && !cvars::vulkan_rt_trace_request.empty()) {
      std::error_code rt_trace_exists_error;
      if (std::filesystem::exists(cvars::vulkan_rt_trace_request,
                                  rt_trace_exists_error)) {
        std::error_code rt_trace_remove_error;
        std::filesystem::remove(cvars::vulkan_rt_trace_request,
                                rt_trace_remove_error);
        rt_trace_active_ = true;
        pm4_trace_active_ = true;
        pm4_trace_skipped_ = 0;
        render_target_cache_->SetRtTraceClaims(true);
        rt_trace_frame_ = frame_current_;
        rt_trace_draw_count_ = 0;
        rt_trace_run_count_ = 0;
        rt_trace_run_open_ = false;
        XELOGI("RTTRACE begin frame={}", frame_current_);
      }
    }

    if (cache_clear_requested_ &&
        (!shared_memory_ || !shared_memory_->depth_materializing()) &&
        AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();
      assert_true(stream_command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer :
           stream_command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      stream_command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_descriptor_cache_ = {};
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();
  // MANDATORY: all transient texture sets are destroyed here, so the value
  // cache for them is no longer valid.
  current_texture_descriptors_valid_vertex_ = false;
  current_texture_descriptors_valid_pixel_ = false;

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  // MANDATORY: the uniform-buffer transient allocator is Reset() below,
  // destroying every constants set, so the value cache (the cached set handle in
  // current_graphics_descriptor_sets_[kDescriptorSetConstants] and the
  // per-binding VkBuffer handles) is no longer valid.
  constants_descriptor_set_valid_ = false;
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_storage_image_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count =
      pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count =
      pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask &&
      !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >=
          pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset =
      pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset =
      pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         scratch_buffer_memory_);
}

static bool DrawStencilOpsEqual(const VkStencilOpState& a,
                                const VkStencilOpState& b) {
  return a.failOp == b.failOp && a.passOp == b.passOp &&
         a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
}

void VulkanCommandProcessor::UpdateDynamicState(
    const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
    bool depth_bias_in_pixel_shader,
    const VulkanPipelineCache::DynamicState& pipeline_dynamic_state) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (!draw_dynamic_state_dirty_) return;
  draw_dynamic_state_dirty_ = false;
  const RegisterFile& regs = *register_file_;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Scale the scissor to match the render target resolution scale
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    if (depth_bias_in_pixel_shader) {
      depth_bias_constant_factor = 0.0f;
      depth_bias_slope_factor = 0.0f;
    } else {
      draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal,
                                               depth_bias_slope_factor,
                                               depth_bias_constant_factor);
      depth_bias_constant_factor *=
          regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                  xenos::DepthRenderTargetFormat::kD24S8
              ? draw_util::kD3D10PolygonOffsetFactorUnorm24
              : draw_util::kD3D10PolygonOffsetFactorFloat24;
      // With non-square resolution scaling, make sure the worst-case impact is
      // reverted (slope only along the scaled axis), thus max. Per-draw scale
      // so native draws get the guest bias as is. More bias is better than less
      // bias; less bias means Z fighting with the background is more likely.
      depth_bias_slope_factor *=
          xenos::kPolygonOffsetScaleSubpixelUnit *
          float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    }
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_,
                    &depth_bias_constant_factor, sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_slope_factor_, &depth_bias_slope_factor,
                    sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_->CmdVkSetDepthBias(
          dynamic_depth_bias_constant_factor_, 0.0f,
          dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants,
                    sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_->CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
          stencil_ref_mask_front_reg =
              (pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back)
                  ? XE_GPU_REG_RB_STENCILREFMASK_BF
                  : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ !=
          stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ !=
          stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ !=
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ =
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ !=
          stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ ==
          dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_->CmdVkSetStencilCompareMask(
            VK_STENCIL_FACE_FRONT_AND_BACK,
            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ ==
          dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_->CmdVkSetStencilWriteMask(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_->CmdVkSetStencilReference(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilReference(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_->CmdVkSetStencilReference(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // Extended dynamic state (VK_EXT_extended_dynamic_state / state2 / state3).
  // The pipeline cache resolved the exact Vulkan values these draw must use
  // (matching what a static pipeline would have baked) into
  // pipeline_dynamic_state; emit only the fields whose capability bit is set,
  // with dirty tracking so redundant setters aren't recorded.
  const VulkanPipelineCache::DynamicStateCapabilities& eds_caps =
      pipeline_cache_->dynamic_state_capabilities();
  if (eds_caps.fragment_shading_rate) {
    dynamic_fragment_shading_rate_update_needed_ |=
        dynamic_fragment_shading_rate_ != draw_fragment_shading_rate_;
    if (dynamic_fragment_shading_rate_update_needed_) {
      dynamic_fragment_shading_rate_ = draw_fragment_shading_rate_;
      deferred_command_buffer_->CmdVkSetFragmentShadingRateKHR(
          dynamic_fragment_shading_rate_ >> 8,
          dynamic_fragment_shading_rate_ & 0xFF);
      dynamic_fragment_shading_rate_update_needed_ = false;
    }
    ++fragment_shading_rate_draws_;
    fragment_shading_rate_coarse_draws_ +=
        uint64_t(draw_fragment_shading_rate_ != 0x0101);
    if (!(fragment_shading_rate_draws_ & 0xFFFFF)) {
      XELOGI("VkShadingRate: {} of the last {} guest draws shaded coarsely",
             fragment_shading_rate_coarse_draws_, uint64_t(0x100000));
      fragment_shading_rate_coarse_draws_ = 0;
    }
  }
  if (eds_caps.extended_dynamic_state) {
    const VulkanPipelineCache::DynamicState& ds = pipeline_dynamic_state;

    // Primitive topology (EDS1 core).
    dynamic_primitive_topology_update_needed_ |=
        dynamic_primitive_topology_ != ds.primitive_topology;
    if (dynamic_primitive_topology_update_needed_) {
      dynamic_primitive_topology_ = ds.primitive_topology;
      deferred_command_buffer_->CmdVkSetPrimitiveTopology(
          dynamic_primitive_topology_);
      dynamic_primitive_topology_update_needed_ = false;
    }
    // Primitive restart enable (EDS2 core).
    dynamic_primitive_restart_enable_update_needed_ |=
        dynamic_primitive_restart_enable_ != ds.primitive_restart_enable;
    if (dynamic_primitive_restart_enable_update_needed_) {
      dynamic_primitive_restart_enable_ = ds.primitive_restart_enable;
      deferred_command_buffer_->CmdVkSetPrimitiveRestartEnable(
          dynamic_primitive_restart_enable_);
      dynamic_primitive_restart_enable_update_needed_ = false;
    }
    // Cull mode (EDS1 core).
    dynamic_cull_mode_update_needed_ |= dynamic_cull_mode_ != ds.cull_mode;
    if (dynamic_cull_mode_update_needed_) {
      dynamic_cull_mode_ = ds.cull_mode;
      deferred_command_buffer_->CmdVkSetCullMode(dynamic_cull_mode_);
      dynamic_cull_mode_update_needed_ = false;
    }
    // Front face (EDS1 core).
    dynamic_front_face_update_needed_ |= dynamic_front_face_ != ds.front_face;
    if (dynamic_front_face_update_needed_) {
      dynamic_front_face_ = ds.front_face;
      deferred_command_buffer_->CmdVkSetFrontFace(dynamic_front_face_);
      dynamic_front_face_update_needed_ = false;
    }

    // Depth / stencil dynamic state. The pipeline cache disables the whole
    // extended-dynamic-state capability on the fragment-shader-interlock path
    // (where these aren't fixed-function), so reaching here implies a render
    // pass with real depth / stencil state.
    {
      // Depth test enable (EDS1 core).
      dynamic_depth_test_enable_update_needed_ |=
          dynamic_depth_test_enable_ != ds.depth_test_enable;
      if (dynamic_depth_test_enable_update_needed_) {
        dynamic_depth_test_enable_ = ds.depth_test_enable;
        deferred_command_buffer_->CmdVkSetDepthTestEnable(
            dynamic_depth_test_enable_);
        dynamic_depth_test_enable_update_needed_ = false;
      }
      // Depth write enable (EDS1 core).
      dynamic_depth_write_enable_update_needed_ |=
          dynamic_depth_write_enable_ != ds.depth_write_enable;
      if (dynamic_depth_write_enable_update_needed_) {
        dynamic_depth_write_enable_ = ds.depth_write_enable;
        deferred_command_buffer_->CmdVkSetDepthWriteEnable(
            dynamic_depth_write_enable_);
        dynamic_depth_write_enable_update_needed_ = false;
      }
      // Depth compare op (EDS1 core).
      dynamic_depth_compare_op_update_needed_ |=
          dynamic_depth_compare_op_ != ds.depth_compare_op;
      if (dynamic_depth_compare_op_update_needed_) {
        dynamic_depth_compare_op_ = ds.depth_compare_op;
        deferred_command_buffer_->CmdVkSetDepthCompareOp(
            dynamic_depth_compare_op_);
        dynamic_depth_compare_op_update_needed_ = false;
      }
      // Stencil test enable (EDS1 core).
      dynamic_stencil_test_enable_update_needed_ |=
          dynamic_stencil_test_enable_ != ds.stencil_test_enable;
      if (dynamic_stencil_test_enable_update_needed_) {
        dynamic_stencil_test_enable_ = ds.stencil_test_enable;
        deferred_command_buffer_->CmdVkSetStencilTestEnable(
            dynamic_stencil_test_enable_);
        dynamic_stencil_test_enable_update_needed_ = false;
      }
      // Stencil ops (EDS1 core), tracked per face. memcmp because
      // VkStencilOpState bundles the (dynamic) compare/write masks and
      // reference, which are set separately and must be ignored here.
      dynamic_stencil_op_front_update_needed_ |=
          !DrawStencilOpsEqual(dynamic_stencil_op_front_, ds.stencil_front);
      if (dynamic_stencil_op_front_update_needed_) {
        dynamic_stencil_op_front_ = ds.stencil_front;
        deferred_command_buffer_->CmdVkSetStencilOp(
            VK_STENCIL_FACE_FRONT_BIT, ds.stencil_front.failOp,
            ds.stencil_front.passOp, ds.stencil_front.depthFailOp,
            ds.stencil_front.compareOp);
        dynamic_stencil_op_front_update_needed_ = false;
      }
      dynamic_stencil_op_back_update_needed_ |=
          !DrawStencilOpsEqual(dynamic_stencil_op_back_, ds.stencil_back);
      if (dynamic_stencil_op_back_update_needed_) {
        dynamic_stencil_op_back_ = ds.stencil_back;
        deferred_command_buffer_->CmdVkSetStencilOp(
            VK_STENCIL_FACE_BACK_BIT, ds.stencil_back.failOp,
            ds.stencil_back.passOp, ds.stencil_back.depthFailOp,
            ds.stencil_back.compareOp);
        dynamic_stencil_op_back_update_needed_ = false;
      }
    }

    // EDS3 sub-features.
    if (eds_caps.depth_clamp_enable) {
      dynamic_depth_clamp_enable_update_needed_ |=
          dynamic_depth_clamp_enable_ != ds.depth_clamp_enable;
      if (dynamic_depth_clamp_enable_update_needed_) {
        dynamic_depth_clamp_enable_ = ds.depth_clamp_enable;
        deferred_command_buffer_->CmdVkSetDepthClampEnableEXT(
            dynamic_depth_clamp_enable_);
        dynamic_depth_clamp_enable_update_needed_ = false;
      }
    }
    if (eds_caps.polygon_mode) {
      dynamic_polygon_mode_update_needed_ |=
          dynamic_polygon_mode_ != ds.polygon_mode;
      if (dynamic_polygon_mode_update_needed_) {
        dynamic_polygon_mode_ = ds.polygon_mode;
        deferred_command_buffer_->CmdVkSetPolygonModeEXT(dynamic_polygon_mode_);
        dynamic_polygon_mode_update_needed_ = false;
      }
    }
    {
      // Per-render-target blend state. Emit only for the attachments present in
      // the render pass (ds.color_rts_used), as a single contiguous range
      // covering [0, highest used] so the array is dense for the setter.
      uint32_t color_rts_used = ds.color_rts_used;
      uint32_t attachment_count =
          color_rts_used ? (32 - xe::lzcnt(color_rts_used)) : 0;
      if (attachment_count) {
        if (eds_caps.color_blend_enable) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (dynamic_color_blend_enable_[i] != ds.color_blend_enable[i]) {
              dynamic_color_blend_enable_[i] = ds.color_blend_enable[i];
              changed = true;
            }
          }
          dynamic_color_blend_enable_update_needed_ |= changed;
          if (dynamic_color_blend_enable_update_needed_) {
            deferred_command_buffer_->CmdVkSetColorBlendEnableEXT(
                0, attachment_count, dynamic_color_blend_enable_);
            dynamic_color_blend_enable_update_needed_ = false;
          }
        }
        if (eds_caps.color_blend_equation) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (std::memcmp(&dynamic_color_blend_equation_[i],
                            &ds.color_blend_equation[i],
                            sizeof(VkColorBlendEquationEXT)) != 0) {
              dynamic_color_blend_equation_[i] = ds.color_blend_equation[i];
              changed = true;
            }
          }
          dynamic_color_blend_equation_update_needed_ |= changed;
          if (dynamic_color_blend_equation_update_needed_) {
            deferred_command_buffer_->CmdVkSetColorBlendEquationEXT(
                0, attachment_count, dynamic_color_blend_equation_);
            dynamic_color_blend_equation_update_needed_ = false;
          }
        }
        if (eds_caps.color_write_mask) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (dynamic_color_write_mask_[i] != ds.color_write_mask[i]) {
              dynamic_color_write_mask_[i] = ds.color_write_mask[i];
              changed = true;
            }
          }
          dynamic_color_write_mask_update_needed_ |= changed;
          if (dynamic_color_write_mask_update_needed_) {
            deferred_command_buffer_->CmdVkSetColorWriteMaskEXT(
                0, attachment_count, dynamic_color_write_mask_);
            dynamic_color_write_mask_update_needed_ = false;
          }
        }
      }
    }
  }
  draw_dynamic_state_dirty_ = false;
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  bool operands_dirty = false;
  const auto& operand = primitive_processing_result;
  const bool expansion = operand.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      operand.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  const uint32_t count = expansion ? operand.guest_draw_vertex_count
                                   : operand.host_draw_vertex_count;
  if (shader_32bit_index_dma || operand.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    operands_dirty |= system_constants_.vertex_index_load_address != operand.guest_index_base;
    system_constants_.vertex_index_load_address = operand.guest_index_base;
  }
  operands_dirty |= system_constants_.vertex_index_count != count;
  system_constants_.vertex_index_count = count;
  operands_dirty |= system_constants_.vertex_index_endian != operand.host_shader_index_endian;
  system_constants_.vertex_index_endian = operand.host_shader_index_endian;
  const int32_t base_index = register_file_->Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  operands_dirty |= system_constants_.vertex_base_index != base_index;
  system_constants_.vertex_base_index = base_index;
  operands_dirty |= system_constants_.tessellation_vertex_index_offset != uint32_t(base_index);
  system_constants_.tessellation_vertex_index_offset = uint32_t(base_index);
  if (operands_dirty)
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  if (!draw_system_state_dirty_) return;
  draw_system_state_dirty_ = false;
  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  // Resolution scale of this draw.
  // 1x1 with draw_resolution_scale_threshold (FSI only).
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  // The color info registers are also read by the non-FSI gamma flag and color
  // exponent bias below.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing. When gamma is stored as unorm16, the host render target
  // holds linear values (blended in linear space) and the linear -> gamma
  // encode happens on the EDRAM store, so the pixel shader must not pre-encode.
  if (!edram_fragment_shader_interlock &&
      !render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (!edram_fragment_shader_interlock) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
          color_infos[i].color_format ==
              xenos::ColorRenderTargetFormat::
                  k_2_10_10_10_FLOAT_AS_16_16_16_16) {
        flags |= SpirvShaderTranslator::kSysFlag_ClampColor0To7e3 << i;
      }
    }
  }
  if (edram_fragment_shader_interlock) {
    // Fragment shader interlock (EDRAM ROP) flag bits and EDRAM constants. The
    // ZPD occlusion counter slot is selected here from Vulkan-specific query
    // state and passed into the shared helper.
    uint32_t zpd_fsi_counter_index = UINT32_MAX;
    if (zpd_active_query_index_ != UINT32_MAX && zpd_active_query_is_fsi_ &&
        zpd_host_query_pool_->fsi_counter_initialized()) {
      zpd_fsi_counter_index = zpd_active_query_index_;
    }
    dirty |= zpd_fsi_counter_index_force_update_;
    zpd_fsi_counter_index_force_update_ = false;
    WriteFragmentShaderInterlockSystemConstants(
        system_constants_, flags, dirty, regs, primitive_polygonal,
        normalized_depth_control, normalized_color_mask,
        draw_resolution_scale_x, draw_resolution_scale_y,
        zpd_fsi_counter_index);
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // User clip planes, for vertex and domain shaders.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t user_clip_plane_mask = pipeline_cache_->GetUserClipPlaneMask();
  if (user_clip_plane_mask) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = user_clip_plane_mask;
    uint32_t user_clip_plane_index;
    while (xe::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining =
          xe::clear_lowest_bit(user_clip_planes_remaining);
      // Validate plane index is within bounds (0-5).
      assert(user_clip_plane_index < 6);
      if (user_clip_plane_index >= 6) {
        continue;
      }
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs,
                      4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                    4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  } else {
    constexpr float kZeroPlanes[6][4] = {};
    if (std::memcmp(system_constants_.user_clip_planes, kZeroPlanes,
                    sizeof(system_constants_.user_clip_planes))) {
      dirty = true;
      std::memset(system_constants_.user_clip_planes, 0,
                  sizeof(system_constants_.user_clip_planes));
    }
  }

  // Tessellation constants. The factor range has 1.0 added per Xbox 360 docs.
  // fractional_even partitioning needs a minimum of 2.0.
  {
    float tess_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    float tess_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    dirty |= system_constants_.tessellation_factor_range[0] != tess_min;
    dirty |= system_constants_.tessellation_factor_range[1] != tess_max;
    system_constants_.tessellation_factor_range[0] = tess_min;
    system_constants_.tessellation_factor_range[1] = tess_max;
    uint32_t tess_vie = static_cast<uint32_t>(
        primitive_processing_result.host_shader_index_endian);
    uint32_t tess_vio = regs[XE_GPU_REG_VGT_INDX_OFFSET];
    uint32_t tess_vmin = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
    uint32_t tess_vmax = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
    dirty |= system_constants_.tessellation_vertex_index_endian != tess_vie;
    dirty |= system_constants_.tessellation_vertex_index_offset != tess_vio;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[0] != tess_vmin;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[1] != tess_vmax;
    system_constants_.tessellation_vertex_index_endian = tess_vie;
    system_constants_.tessellation_vertex_index_offset = tess_vio;
    system_constants_.tessellation_vertex_index_min_max[0] = tess_vmin;
    system_constants_.tessellation_vertex_index_min_max[1] = tess_vmax;
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min !=
             point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max !=
             point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] !=
             point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] !=
             point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1)
                                    << texture_signs_shift;
      dirty |=
          (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      uint32_t texture_integer_scale_bits =
          texture_cache_->GetActiveIntegerScaleBits(texture_index);
      dirty |= system_constants_.texture_integer_scale_bits[texture_index] !=
               texture_integer_scale_bits;
      system_constants_.texture_integer_scale_bits[texture_index] =
          texture_integer_scale_bits;
    }
  }

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint =
          system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle =
          texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle)
                                         << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1)
                                      << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) !=
               texture_swizzle_shifted;
      texture_swizzles_uint = (texture_swizzles_uint & ~texture_swizzle_mask) |
                              texture_swizzle_shifted;
    }
  }

  // Textures resolution scaled - which textures are from scaled resolve
  // operations.
  {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    dirty |= system_constants_.textures_resolved != textures_resolved;
    system_constants_.textures_resolved = textures_resolved;
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;

  // Alpha to coverage.
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  if (draw_config_.vulkan_debug_disable_alpha_to_mask) {
    alpha_to_mask = 0;
  }
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format ==
                 xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  if (!edram_fragment_shader_interlock && host_depth_polygon_offset) {
    draw_util::HostDepthPolygonOffset polygon_offset =
        *host_depth_polygon_offset;
    float scale_factor =
        float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    polygon_offset.front_scale *= scale_factor;
    polygon_offset.back_scale *= scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale !=
             polygon_offset.front_scale;
    system_constants_.edram_poly_offset_front_scale =
        polygon_offset.front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset !=
             polygon_offset.front_offset;
    system_constants_.edram_poly_offset_front_offset =
        polygon_offset.front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale !=
             polygon_offset.back_scale;
    system_constants_.edram_poly_offset_back_scale = polygon_offset.back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset !=
             polygon_offset.back_offset;
    system_constants_.edram_poly_offset_back_offset =
        polygon_offset.back_offset;
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader,
                                            bool vertex_bindings_ready,
                                            bool pixel_bindings_ready,
                                            bool interpreter_placeholder,
                                            bool placeholder_pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  if (bindless_set_ != VK_NULL_HANDLE) {
    // The placeholder shaders never sample.
    UpdateBindlessSlots(vertex_shader, pixel_shader,
                        vertex_bindings_ready && !interpreter_placeholder,
                        pixel_bindings_ready && !placeholder_pixel_shader);
    if (bindless_slot_written_) {
      bindless_slot_written_ = false;
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
    }
  }

  RegisterFile& dirty_regs = *register_file_;
  const VulkanShader* shaders[2] = {vertex_shader, pixel_shader};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const auto& interval = dirty_regs.float_dirty[stage];
    const uint64_t* bitmap = shaders[stage]
        ? shaders[stage]->constant_register_map().float_bitmap : nullptr;
    if (float_constants_[stage].Update(shaders[stage], bitmap,
            !stage && interpreter_placeholder,
            regs.values + XE_GPU_REG_SHADER_CONSTANT_000_X + stage * 1024,
            interval.first, interval.end)) {
      current_constant_buffers_up_to_date_ &= ~(UINT32_C(1) <<
          (stage ? SpirvShaderTranslator::kConstantBufferFloatPixel
                 : SpirvShaderTranslator::kConstantBufferFloatVertex));
    }
    dirty_regs.float_dirty[stage].Clear();
  }
  const uint32_t float_constant_count_vertex = float_constants_[0].count();
  const uint32_t float_constant_count_pixel = float_constants_[1].count();
  if (!dirty_regs.bool_loop_dirty.empty()) {
    const auto& interval = dirty_regs.bool_loop_dirty;
    std::memcpy(bool_loop_constants_ + interval.first,
                regs.values + XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + interval.first,
                sizeof(uint32_t) * (interval.end - interval.first));
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
    dirty_regs.bool_loop_dirty.Clear();
  }
  if (bindings_fetch_generation_ != dirty_regs.fetch_generation) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    bindings_fetch_generation_ = dirty_regs.fetch_generation;
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) !=
      kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SpirvShaderTranslator::SystemConstants),
          uniform_buffer_alignment, buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_,
                  sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      // The interpreter indexes all 256 float constants by raw index, so upload
      // the full contiguous register file rather than the packed subset.
      size_t float_constants_size =
          interpreter_placeholder
              ? sizeof(float) * 4 * 256
              : sizeof(float) * 4 *
                    std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      std::memcpy(mapping, float_constants_[0].data(), float_constants_size);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      std::memcpy(mapping, float_constants_[1].data(), float_constants_size);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, kBoolLoopConstantsSize, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, bool_loop_constants_, kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, kFetchConstantsSize, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                  kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch;
    }
  }

  // Textures and samplers. Only read a stage's binding lists when it was ready
  // at the draw's snapshot - a creation thread may still be populating them,
  // and the snapshot keeps the counts matching the collected sampler vectors.
  // Binding the reference is fine; only size()/iteration touch it.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex =
      vertex_bindings_ready ? uint32_t(samplers_vertex.size()) : 0;
  uint32_t texture_count_vertex =
      vertex_bindings_ready ? uint32_t(textures_vertex.size()) : 0;
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader && pixel_bindings_ready) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // Placeholder pipelines were built with a layout matching only what their
  // placeholder shaders access, so don't bind texture sets they lack (which
  // could otherwise happen if the real shaders finished translating mid-draw).
  // The no-op placeholder PS never samples; the interpreter VS never samples.
  // (Both flags stay false when vulkan_placeholder_pipelines is off.)
  if (placeholder_pixel_shader) {
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  if (interpreter_placeholder) {
    sampler_count_vertex = 0;
    texture_count_vertex = 0;
  }
  if (bindless_set_ != VK_NULL_HANDLE) {
    // Only slots in the system constants, written above.
    sampler_count_vertex = 0;
    texture_count_vertex = 0;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }

  // The descriptor write and the set-hash below both rely on this; a shorter
  // vector would write fewer image infos than the count the set layout was
  // chosen by.
  assert_true(!sampler_count_vertex ||
              current_samplers_vertex_.size() == sampler_count_vertex);
  assert_true(!sampler_count_pixel ||
              current_samplers_pixel_.size() == sampler_count_pixel);

  // Event-driven descriptor invalidation: texture cache reports slots whose
  // view/layout changed; sampler passes publish a monotonically increasing
  // generation. No hashing, vector comparison, or map lookup on unchanged draws.
  const uint32_t textures_changed = texture_cache_->texture_bindings_changed();
  const uint64_t image_generation = texture_cache_->image_view_destroy_generation();
  const uint64_t sampler_generation = texture_cache_->sampler_destroy_generation();
  const VulkanShader* binding_shaders[2] = {vertex_shader, pixel_shader};
  const uint32_t texture_counts[2] = {texture_count_vertex, texture_count_pixel};
  const uint32_t sampler_counts[2] = {sampler_count_vertex, sampler_count_pixel};
  const bool ready[2] = {vertex_bindings_ready, pixel_bindings_ready};
  bool* valid[2] = {&current_texture_descriptors_valid_vertex_,
                    &current_texture_descriptors_valid_pixel_};
  uint32_t used_texture_masks = 0;
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const uint32_t used = binding_shaders[stage] && ready[stage] && texture_counts[stage]
        ? binding_shaders[stage]->GetUsedTextureMaskAfterTranslation() : 0;
    used_texture_masks |= used;
    TextureDescriptorState& state = texture_descriptor_state_[stage];
    if (!*valid[stage] || state.shader != binding_shaders[stage] ||
        state.textures != texture_counts[stage] || state.samplers != sampler_counts[stage] ||
        (textures_changed & used) || state.images != image_generation ||
        state.destroyed_samplers != sampler_generation ||
        state.sampler_bindings != sampler_binding_generation_[stage]) {
      current_graphics_descriptor_set_values_up_to_date_ &= ~(UINT32_C(1) <<
          (stage ? SpirvShaderTranslator::kDescriptorSetTexturesPixel
                 : SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      state = {binding_shaders[stage], texture_counts[stage], sampler_counts[stage],
               image_generation, sampler_generation, sampler_binding_generation_[stage]};
    }
  }
  texture_cache_->ResetTextureBindingsChanged(used_texture_masks);

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  uint32_t descriptor_sets_needed =
      (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  if (bindless_set_ != VK_NULL_HANDLE) {
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesVertex] = bindless_set_;
    // Every set of the layout bound, the pixel one to an empty set: none of
    // the layout's sets keeps a stale handle from an earlier bind.
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
            bindless_empty_set_;
    current_graphics_descriptor_set_values_up_to_date_ |=
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
    descriptor_sets_needed |=
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  if ((current_graphics_descriptor_sets_bound_up_to_date_ & descriptor_sets_needed) ==
      descriptor_sets_needed) {
    return true;
  }

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  // Texture sets are allocated from the pipeline layout, so its baked counts
  // must match the writes below. A mismatch means the pipeline still carries
  // the minimal layout from untranslated shaders, and writing into the
  // resulting zero-binding set crashes the driver. Fail the draw instead.
  if ((write_vertex_textures &&
       (current_guest_graphics_pipeline_layout_->texture_count_vertex() !=
            texture_count_vertex ||
        current_guest_graphics_pipeline_layout_->sampler_count_vertex() !=
            sampler_count_vertex)) ||
      (write_pixel_textures &&
       (current_guest_graphics_pipeline_layout_->texture_count_pixel() !=
            texture_count_pixel ||
        current_guest_graphics_pipeline_layout_->sampler_count_pixel() !=
            sampler_count_pixel))) {
    XELOGE(
        "UpdateBindings: pipeline layout counts (VS {}t/{}s, PS {}t/{}s) do "
        "not match the draw's binding counts (VS {}t/{}s, PS {}t/{}s) - "
        "skipping the draw",
        current_guest_graphics_pipeline_layout_->texture_count_vertex(),
        current_guest_graphics_pipeline_layout_->sampler_count_vertex(),
        current_guest_graphics_pipeline_layout_->texture_count_pixel(),
        current_guest_graphics_pipeline_layout_->sampler_count_pixel(),
        texture_count_vertex, sampler_count_vertex, texture_count_pixel,
        sampler_count_pixel);
    return false;
  }
  // When a needed texture set is skipped (not re-written this draw), the cached
  // VkDescriptorSet must still be a real, allocated set - never the frame-open
  // VK_NULL_HANDLE memset value - or the bind loop below would bind a null
  // handle. A missed hash-validity reset would surface here as a fail-fast.
  assert_true(write_vertex_textures ||
              !(texture_count_vertex || sampler_count_vertex) ||
              current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetTexturesVertex] !=
                  VK_NULL_HANDLE);
  assert_true(write_pixel_textures ||
              !(texture_count_pixel || sampler_count_pixel) ||
              current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetTexturesPixel] !=
                  VK_NULL_HANDLE);
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex
                             : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding :
         textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          texture_cache_->GetActiveBindingImageLayout(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding :
         *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          texture_cache_->GetActiveBindingImageLayout(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet,
             SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  TextureDescriptorCacheEntry* texture_entries[2] = {};
  // Dynamic-constants descriptor buffer infos. These must outlive the single
  // deferred vkUpdateDescriptorSets below (each write_constants.pBufferInfo
  // points into this array), so they live at the same scope as
  // write_descriptor_sets rather than inside the rewrite branch.
  VkDescriptorBufferInfo constants_buffer_infos_dynamic
      [SpirvShaderTranslator::kConstantBufferCount];
  assert_not_zero(
      current_graphics_descriptor_set_values_up_to_date_ &
      (UINT32_C(1)
       << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  // The values bit was cleared at the upload site (above) whenever ANY of the 5
  // buffers' content changed, which is what forces the per-draw re-bind below.
  // With UNIFORM_BUFFER_DYNAMIC we DECOUPLE the descriptor WRITE from the bind:
  // the set is re-allocated and re-written only when a binding's backing
  // VkBuffer changed (upload-pool page rollover); otherwise the same set is
  // re-bound with fresh dynamic offsets. With plain UNIFORM_BUFFER (gate off)
  // the original unconditional alloc+write+bind runs.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    bool rewrite_needed = true;
    if (use_dynamic_constants_) {
      rewrite_needed = !constants_descriptor_set_valid_;
      for (uint32_t i = 0;
           !rewrite_needed && i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        // Re-write on page rollover (buffer changed) OR a range change. The
        // descriptor's range is baked at write time and only the dynamic offset
        // is re-bound per draw, so a stale (too small) range from a prior
        // shader with fewer float constants would silently clamp the current
        // shader's reads. See constants_descriptor_set_ranges_ in the header.
        if (current_constant_buffer_infos_[i].buffer !=
                constants_descriptor_set_buffers_[i] ||
            current_constant_buffer_infos_[i].range !=
                constants_descriptor_set_ranges_[i]) {
          rewrite_needed = true;
        }
      }
    }
    if (rewrite_needed) {
      const VkDescriptorType constants_descriptor_type =
          use_dynamic_constants_ ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      VkDescriptorSet constants_descriptor_set;
      if (!constants_transient_descriptors_free_.empty()) {
        constants_descriptor_set = constants_transient_descriptors_free_.back();
        constants_transient_descriptors_free_.pop_back();
      } else {
        VkDescriptorPoolSize constants_descriptor_count;
        // For UNIFORM_BUFFER_DYNAMIC this type is NOT present in the shared
        // allocator's pool sizes (kept at UNIFORM_BUFFER for the compute uniform
        // path), so LinkedTypeDescriptorSetAllocator::Allocate falls back to a
        // dedicated VkDescriptorPool per allocation. That is spec-legal, and
        // bounded: with the dynamic path we allocate only on page rollover
        // (0-2x/frame), not per draw, so the dedicated-pool churn is rare. Do
        // NOT retype the shared pool to fix this - the compute path needs the
        // UNIFORM_BUFFER pool size.
        constants_descriptor_count.type = constants_descriptor_type;
        constants_descriptor_count.descriptorCount =
            SpirvShaderTranslator::kConstantBufferCount;
        constants_descriptor_set =
            transient_descriptor_allocator_uniform_buffer_.Allocate(
                descriptor_set_layout_constants_, &constants_descriptor_count,
                1);
        if (constants_descriptor_set == VK_NULL_HANDLE) {
          return false;
        }
      }
      constants_transient_descriptors_used_.emplace_back(
          frame_current_, constants_descriptor_set);
      // For UNIFORM_BUFFER_DYNAMIC the descriptor base offset MUST be 0: the
      // per-buffer slice offset is supplied as a dynamic offset at bind time, so
      // baking it into the descriptor as well would double-count it. The range
      // stays the per-buffer data size (<= 4 KiB, well below the
      // maxUniformBufferRange spec floor of 16384). With plain UNIFORM_BUFFER
      // the original {buffer, slice_offset, range} info is used directly.
      // (constants_buffer_infos_dynamic is declared at function scope above so
      // it outlives the deferred vkUpdateDescriptorSets.)
      if (use_dynamic_constants_) {
        for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
             ++i) {
          assert_true(current_constant_buffer_infos_[i].range <=
                      VkDeviceSize(GetVulkanDevice()
                                       ->properties()
                                       .maxUniformBufferRange));
          constants_buffer_infos_dynamic[i].buffer =
              current_constant_buffer_infos_[i].buffer;
          constants_buffer_infos_dynamic[i].offset = 0;
          constants_buffer_infos_dynamic[i].range =
              current_constant_buffer_infos_[i].range;
        }
      }
      // Consecutive bindings updated via a single VkWriteDescriptorSet must have
      // identical stage flags, but for the constants they vary.
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        VkWriteDescriptorSet& write_constants =
            write_descriptor_sets[write_descriptor_set_count++];
        write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_constants.pNext = nullptr;
        write_constants.dstSet = constants_descriptor_set;
        write_constants.dstBinding = i;
        write_constants.dstArrayElement = 0;
        write_constants.descriptorCount = 1;
        write_constants.descriptorType = constants_descriptor_type;
        write_constants.pImageInfo = nullptr;
        write_constants.pBufferInfo =
            use_dynamic_constants_ ? &constants_buffer_infos_dynamic[i]
                                   : &current_constant_buffer_infos_[i];
        write_constants.pTexelBufferView = nullptr;
      }
      write_descriptor_set_bits |=
          UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
      current_graphics_descriptor_sets_
          [SpirvShaderTranslator::kDescriptorSetConstants] =
              constants_descriptor_set;
      if (use_dynamic_constants_) {
        for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
             ++i) {
          constants_descriptor_set_buffers_[i] =
              current_constant_buffer_infos_[i].buffer;
          constants_descriptor_set_ranges_[i] =
              current_constant_buffer_infos_[i].range;
        }
        constants_descriptor_set_valid_ = true;
      }
    } else {
      // Dynamic skip-rewrite path: the cached set's bindings still reference the
      // current page buffers, so only the dynamic offsets (bound below) change.
      // Re-set the values bit (do NOT push to the used list - the set was
      // already accounted for when written) so an unchanged subsequent draw
      // keeps the bit and skips even the re-bind, matching the texture-cache
      // no-rebind behavior. A content change will clear it again at the upload
      // site, re-triggering the rebind with fresh offsets.
      assert_true(current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetConstants] !=
                  VK_NULL_HANDLE);
      write_descriptor_set_bits |=
          UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    }
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = 0;
    VkDescriptorSet texture_set = GetTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset,
        write_textures, texture_descriptor_set_write_count, texture_entries[0]);
    if (texture_set == VK_NULL_HANDLE) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesVertex] = texture_set;
    current_texture_descriptors_valid_vertex_ = true;
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = 0;
    VkDescriptorSet texture_set = GetTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset,
        write_textures, texture_descriptor_set_write_count, texture_entries[1]);
    if (texture_set == VK_NULL_HANDLE) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesPixel] = texture_set;
    current_texture_descriptors_valid_pixel_ = true;
  }
  // Write.
  if (write_descriptor_set_count) {
    for (uint32_t i = 0; i < write_descriptor_set_count; ++i) {
      if (write_descriptor_sets[i].dstSet == VK_NULL_HANDLE) {
        XELOGE(
            "UpdateBindings: write {} of {} has a null destination set (type "
            "{}, binding {}, count {}) - skipping the update",
            i, write_descriptor_set_count,
            uint32_t(write_descriptor_sets[i].descriptorType),
            write_descriptor_sets[i].dstBinding,
            write_descriptor_sets[i].descriptorCount);
        return false;
      }
    }
    if (draw_config_.vulkan_defer_descriptor_updates) {
      deferred_command_buffer_->CmdUpdateDescriptorSets(
          write_descriptor_set_count, write_descriptor_sets.data());
    } else {
      dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,
                                 write_descriptor_sets.data(), 0, nullptr);
    }
  }
  for (auto* entry : texture_entries) {
    if (entry) entry->written = true;
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |=
      write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed &
      ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (
      xe::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        xe::tzcnt(~(descriptor_sets_remaining |
                    ((UINT32_C(1) << descriptor_set_index) - 1)));
    // When the constants set is bound as UNIFORM_BUFFER_DYNAMIC, supply one
    // dynamic offset per binding (in (set, binding) order). pDynamicOffsets
    // covers the dynamic descriptors of ALL sets in the bound range; the
    // constants set is the ONLY dynamic set in the pipeline layout
    // (shared-memory/edram is STORAGE_BUFFER, textures are SAMPLED_IMAGE/
    // SAMPLER), so any run that includes it contributes exactly these 5 offsets
    // and every other set in the run contributes zero - dynamicOffsetCount is
    // always kConstantBufferCount and the array maps 1:1 onto the constants
    // bindings (set index kDescriptorSetConstants comes before the texture sets,
    // so no earlier set in the run can be dynamic). Each offset is the aligned
    // slice offset from uniform_buffer_pool_->Request() (a multiple of
    // minUniformBufferOffsetAlignment), with slice_offset + range <= page size
    // guaranteed by the pool, so descriptor.offset(0) + dynamicOffset + range <=
    // buffer size holds.
    uint32_t dynamic_offset_count = 0;
    const uint32_t* dynamic_offsets = nullptr;
    uint32_t constants_dynamic_offsets
        [SpirvShaderTranslator::kConstantBufferCount];
    if (use_dynamic_constants_ &&
        descriptor_set_index <=
            SpirvShaderTranslator::kDescriptorSetConstants &&
        SpirvShaderTranslator::kDescriptorSetConstants <
            descriptor_set_mask_tzcnt) {
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        constants_dynamic_offsets[i] =
            uint32_t(current_constant_buffer_infos_[i].offset);
      }
      dynamic_offset_count = SpirvShaderTranslator::kConstantBufferCount;
      dynamic_offsets = constants_dynamic_offsets;
    }
    // TODO(Triang3l): Bind to compute for memexport emulation without vertex
    // shader memory stores.
    deferred_command_buffer_->CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(),
        descriptor_set_index, descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index,
        dynamic_offset_count, dynamic_offsets);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &=
        ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

VkDescriptorSet VulkanCommandProcessor::GetTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout,
    const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out, uint32_t& write_count_out,
    TextureDescriptorCacheEntry*& entry_out) {
  assert_true(frame_open_);
  write_count_out = 0;
  // Hash selects a slot only. Compare the complete descriptor contents and
  // layout, including destruction generations (Vulkan may recycle handles).
  uint64_t hash = std::hash<VkDescriptorSetLayout>{}(descriptor_set_layout);
  auto mix = [&](uint64_t value) {
    hash = (hash ^ value) * UINT64_C(0x9E3779B185EBCA87);
  };
  mix(texture_count);
  mix(sampler_count);
  for (uint32_t i = 0; i < texture_count; ++i) {
    mix(std::hash<VkImageView>{}(texture_image_info[i].imageView));
    mix(texture_image_info[i].imageLayout);
  }
  for (uint32_t i = 0; i < sampler_count; ++i)
    mix(std::hash<VkSampler>{}(sampler_image_info[i].sampler));
  auto& entry = texture_descriptor_cache_[(hash ^ (hash >> 32)) & 511];
  const uint64_t images = texture_cache_->image_view_destroy_generation();
  const uint64_t samplers = texture_cache_->sampler_destroy_generation();
  bool hit = entry.written && entry.hash == hash &&
             entry.layout == descriptor_set_layout && entry.images == images &&
             entry.samplers == samplers &&
             entry.layout_key.texture_count == texture_count &&
             entry.layout_key.sampler_count == sampler_count;
  for (uint32_t i = 0; hit && i < texture_count; ++i)
    hit = entry.infos[i].imageView == texture_image_info[i].imageView &&
          entry.infos[i].imageLayout == texture_image_info[i].imageLayout;
  for (uint32_t i = 0; hit && i < sampler_count; ++i)
    hit =
        entry.infos[texture_count + i].sampler == sampler_image_info[i].sampler;
  if (hit) {
    entry.frame = frame_current_;
    entry_out = &entry;
    return entry.set;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it = texture_transient_descriptor_sets_free_.find(
      texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(),
        texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  if (entry.set != VK_NULL_HANDLE) {
    // Retirement owns the evicted set until its LAST use completed, not its
    // creation frame. The queue may be unsorted; a recent front only delays
    // reclamation and can never cause a premature rewrite.
    auto& retired = texture_transient_descriptor_sets_used_.emplace_back();
    retired.frame = entry.frame;
    retired.layout = entry.layout_key;
    retired.set = entry.set;
  }
  entry.hash = hash;
  entry.frame = frame_current_;
  entry.images = images;
  entry.samplers = samplers;
  entry.layout = descriptor_set_layout;
  entry.layout_key = texture_descriptor_set_layout_key;
  entry.set = texture_descriptor_set;
  entry.written = false;
  entry.infos.resize(texture_count + sampler_count);
  for (uint32_t i = 0; i < texture_count; ++i)
    entry.infos[i] = texture_image_info[i];
  for (uint32_t i = 0; i < sampler_count; ++i)
    entry.infos[texture_count + i] = sampler_image_info[i];
  entry_out = &entry;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  write_count_out = descriptor_set_write_count;
  return texture_descriptor_set;
}

#define COMMAND_PROCESSOR VulkanCommandProcessor
#define XE_GPU_DRAW_CENSUS
#define XE_GPU_RESOLVE_DEFER_CENSUS
#include "../pm4_command_processor_implement.h"
#undef XE_GPU_RESOLVE_DEFER_CENSUS
#undef XE_GPU_DRAW_CENSUS
#undef COMMAND_PROCESSOR
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
