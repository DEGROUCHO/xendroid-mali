/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

#include "xenia/base/hash.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/vulkan/render_target_retirement.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/single_layout_descriptor_set_pool.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

DECLARE_bool(vulkan_depth_unorm24);
DECLARE_bool(vulkan_normalize_dontcare_keys);
DECLARE_bool(vulkan_consume_dontcare);

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

struct PassByteCatalog;
class DeferredCommandBuffer;

struct DeferredLazyClearResult;

class VulkanRenderTargetCache final : public RenderTargetCache {
 public:
  enum : uint32_t {
    kColorRTViewModeGuest = 0,
    kColorRTViewModeTransferAll = 1,
    kColorRTViewModeR32UintFirst = 2,
  };

  union RenderPassKey {
    struct {
      // If emulating 2x as 4x, this is still 2x for simplicity of using this
      // field to make guest-related decisions. Render pass objects are not very
      // expensive, and their dependencies can't be shared between 2x-as-4x and
      // true 4x MSAA passes (framebuffers because render target cache render
      // targets are different for 2x and 4x guest MSAA, pipelines because the
      // sample mask will have 2 samples excluded for 2x-as-4x).
      // This has effect only on the attachments, but even in cases when there
      // are no attachments, it can be used to pass the sample count between
      // subsystems, for instance, to specify the desired number of samples to
      // use when there are no attachments in pipelines.
      // Also, without attachments, using separate render passes for different
      // sample counts ensures that if the variableMultisampleRate feature is
      // not supported, no draws with different rasterization sample counts end
      // up in one render pass.
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 2
      // << 0 is depth, << 1...4 is color.
      uint32_t depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;  // 7
      // 0 for unused attachments.
      // If VK_FORMAT_D24_UNORM_S8_UINT is not supported, this must be kD24FS8
      // even for kD24S8.
      xenos::DepthRenderTargetFormat depth_format
          : xenos::kDepthRenderTargetFormatBits;  // 8
      xenos::ColorRenderTargetFormat color_0_view_format
          : xenos::kColorRenderTargetFormatBits;  // 12
      xenos::ColorRenderTargetFormat color_1_view_format
          : xenos::kColorRenderTargetFormatBits;  // 16
      xenos::ColorRenderTargetFormat color_2_view_format
          : xenos::kColorRenderTargetFormatBits;  // 20
      xenos::ColorRenderTargetFormat color_3_view_format
          : xenos::kColorRenderTargetFormatBits;    // 24
      // 0: guest formats, 1: transfer formats for all slots, 2-5: R32_UINT
      // alias view for color slot (mode - kColorRTViewModeR32UintFirst).
      uint32_t color_rts_view_mode : 3;  // 27
      // << 0 is depth, << 1...4 is color. Indicates that the attachment is
      // fully overwritten before guest drawing in the render pass, so its prior
      // contents can be discarded (loadOp = DONT_CARE).
      uint32_t depth_and_color_load_dont_care
          : 1 + xenos::kMaxColorRenderTargets;  // 32
    };
    uint32_t key = 0;
    struct Hasher {
      size_t operator()(const RenderPassKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const RenderPassKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const RenderPassKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const RenderPassKey& other_key) const {
      return key < other_key.key;
    }
  };
  static_assert_size(RenderPassKey, sizeof(uint32_t));

  // Four bits per aspect: depth, stencil, then color 0..3. Zero inherits the
  // original pass's ops. Keep pipeline/framebuffer compatibility keys at 32
  // bits.
  struct RenderPassOps {
    uint32_t key = 0;
    enum Load : uint32_t { kLoadDefault, kLoadNone, kLoadClear, kLoadDontCare };
    enum Store : uint32_t { kStoreDefault, kStoreNone, kStoreDontCare };
    Load load(uint32_t aspect) const { return Load((key >> (aspect * 4)) & 3); }
    Store store(uint32_t aspect) const {
      return Store((key >> (aspect * 4 + 2)) & 3);
    }
    uint32_t ChangedOpCount() const {
      uint32_t count = 0;
      for (uint32_t aspect = 0; aspect < 6; ++aspect) {
        count += load(aspect) != kLoadDefault;
        count += store(aspect) != kStoreDefault;
      }
      return count;
    }
    void SetLoad(uint32_t aspect, Load op) {
      key = (key & ~(3u << (aspect * 4))) | (uint32_t(op) << (aspect * 4));
    }
    void SetStore(uint32_t aspect, Store op) {
      key = (key & ~(3u << (aspect * 4 + 2))) |
            (uint32_t(op) << (aspect * 4 + 2));
    }
    VkAttachmentLoadOp LoadOp(uint32_t aspect,
                              VkAttachmentLoadOp original) const {
      switch (load(aspect)) {
        case kLoadNone:
          return VK_ATTACHMENT_LOAD_OP_NONE_EXT;
        case kLoadClear:
          return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case kLoadDontCare:
          return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        default:
          return original;
      }
    }
    VkAttachmentStoreOp StoreOp(uint32_t aspect) const {
      switch (store(aspect)) {
        case kStoreNone:
          return VK_ATTACHMENT_STORE_OP_NONE;
        case kStoreDontCare:
          return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        default:
          return VK_ATTACHMENT_STORE_OP_STORE;
      }
    }
  };

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D host_extent{};
    VkImage attachment_images[1 + xenos::kMaxColorRenderTargets] = {};
    VkExtent2D attachment_extents[1 + xenos::kMaxColorRenderTargets] = {};
    RenderTargetKey attachment_keys[1 + xenos::kMaxColorRenderTargets] = {};
    uint32_t attachment_used_mask = 0;
    uint64_t last_gpu_reference_submission = 0;
    bool last_gpu_reference_submission_known = false;
    Framebuffer() = default;
    Framebuffer(VkFramebuffer framebuffer, const VkExtent2D& host_extent,
                const RenderTargetKey* attachment_keys,
                uint32_t attachment_used_mask)
        : framebuffer(framebuffer),
          host_extent(host_extent),
          attachment_used_mask(attachment_used_mask) {
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        if (attachment_used_mask & (uint32_t(1) << i)) {
          this->attachment_keys[i] = attachment_keys[i];
        }
      }
    }
    void MarkGpuReference(uint64_t submission) {
      RecordSubmissionReference(last_gpu_reference_submission_known,
                                last_gpu_reference_submission, submission);
    }
  };

  VulkanRenderTargetCache(const RegisterFile& register_file,
                          const Memory& memory, TraceWriter& trace_writer,
                          uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y,
                          VulkanCommandProcessor& command_processor);
  ~VulkanRenderTargetCache();

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();
  void DescribePassBytes(PassByteCatalog& catalog) const;
  // Debug names of the guest render targets backing the last update, for
  // identifying a render pass bucket in the frame-time breakdown.
  std::string GetLastUpdateRenderTargetsDebugName() const;


  // Called once per guest frame (from IssueSwap) to aggregate and, once per
  // second, log the resolve classification when log_resolve_details is set.
  void LogResolveDetailsOnFrameEnd();

  Path GetPath() const override { return path_; }

  VkBuffer edram_buffer() const { return edram_buffer_; }
  bool CaptureRuntimeEdram(std::vector<uint8_t>& bytes);
  bool RestoreRuntimeEdram(std::span<const uint8_t> bytes);
  // Canonical RGBA8 image, raster ordered, 1280x2048. All host RT owners
  // (including MSAA/depth) transfer their guest-visible bits into it.
  bool CaptureRuntimeHostTargets(std::vector<uint8_t>& bytes);
  bool RestoreRuntimeHostTargets(std::span<const uint8_t> bytes);
  bool RuntimeSnapshotHealthy() const { return snapshot_quarantine_.empty(); }

  // Performs the resolve to a shared memory area according to the current
  // register values, and also clears the render targets if needed. Must be in a
  // frame for calling. copy_dest_info_out, if not null, receives the
  // destination info with the format normalized to the xenos::TextureFormat
  // the copy was actually performed with (only meaningful when a nonzero
  // length was written).
  // written_scaled_out: whether the data went to the scaled resolve address
  // space rather than shared memory (native resolves don't).
  bool Resolve(const Memory& memory, VulkanSharedMemory& shared_memory,
               VulkanTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out,
               reg::RB_COPY_DEST_INFO* copy_dest_info_out = nullptr,
               bool* written_scaled_out = nullptr,
               const draw_util::ResolveInfo* captured_info = nullptr);

  // Only provably zero stencil is eligible: every byte is reconstructible
  // from the immutable R32 image, so no mutable RT lease/COW is needed.
  bool EnsureDepthSurfacePipeline();
  bool EnsureDepthMaterializePipeline();
  bool EnsureColorSurfacePipeline();
  bool EnsureColorMaterializePipeline();
  bool DrawColorSurfaceResolve(const draw_util::ResolveInfo& info,
                               VulkanTextureCache& texture_cache,
                               const global_unique_lock_type& lock,
                               std::vector<VulkanTextureCache::DepthSurfaceTarget>& targets);
  bool DrawDepthSurfaceResolve(const draw_util::ResolveInfo& info,
                              VulkanTextureCache& texture_cache,
                              const global_unique_lock_type& lock,
                              std::vector<VulkanTextureCache::DepthSurfaceTarget>& targets);
  void DiscardSurfaceOverwrittenByResolve(const draw_util::ResolveInfo& info,
                                          VulkanSharedMemory& shared_memory,
                                          bool recording);
  bool TryDeferredDepthSurfaceResolve(const draw_util::ResolveInfo& info,
                                     VulkanSharedMemory& shared_memory,
                                     VulkanTextureCache& texture_cache);
  // wait: complete the GPU work (the CPU reads the bytes next); otherwise the
  // compute is only recorded before the GPU work that needs them.
  bool MaterializeDepthSurface(const draw_util::ResolveInfo& info,
                               VulkanTextureCache::DepthSurfaceTarget target,
                               VulkanSharedMemory& shared_memory,
                               VulkanTextureCache& texture_cache, bool wait);
  // Descriptor sets of recorded materializations, freed once completed.
  std::vector<std::pair<uint64_t, size_t>> materialize_descriptors_in_flight_;
  VkPipelineLayout depth_materialize_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_materialize_pipeline_ = VK_NULL_HANDLE;
  // vulkan_stencil_transfer_copy: the stencil of a k_8_8_8_8 color range taken
  // over by a 1x depth render target at the same base and pitch is computed
  // into a buffer and copied into the stencil aspect, instead of one transfer
  // draw per stencil bit (no stencil export).
  bool EnsureStencilCopyPipeline();
  VkPipelineLayout stencil_copy_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline stencil_copy_pipeline_ = VK_NULL_HANDLE;
  std::vector<Transfer> stencil_copy_transfers_;
  uint64_t stencil_copies_ = 0;
  VkRenderPass depth_surface_render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_surface_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_surface_pipeline_ = VK_NULL_HANDLE;
  bool depth_surface_pipeline_attempted_ = false;
  // vulkan_color_surface_resolve: 8_8_8_8 render target -> k_8_8_8_8 texture.
  VkRenderPass color_surface_render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout color_surface_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline color_surface_pipeline_ = VK_NULL_HANDLE;
  bool color_surface_pipeline_attempted_ = false;
  VkPipeline color_materialize_pipeline_ = VK_NULL_HANDLE;
  void RecordDepthSurfaceTextureHit(uint64_t payload) {
    ++depth_surface_stats_.texture_hits;
    depth_surface_stats_.estimated_bytes_saved += int64_t(payload) * 4;
  }
  struct DepthSurfaceStats {
    uint64_t last_report_ns = 0;
    uint64_t surface_resolves = 0, snapshots = 0, texture_hits = 0;
    uint64_t resolves_without_compute = 0, demand_computes = 0;
    int64_t estimated_bytes_saved = 0;
    uint64_t surface_read_bytes = 0, surface_write_bytes = 0;
    uint64_t fallback_schema = 0, fallback_msaa = 0;
    uint64_t fallback_owner = 0, fallback_texture = 0, fallback_resource = 0;
    uint64_t fallback_stencil = 0;
  } depth_surface_stats_;

  bool GetResolveInfoForDefer(const Memory& memory,
                            draw_util::ResolveInfo& info);
  // The saved owners are also retirement roots until the tail is appended.
  bool BeginDeferredResolve(const draw_util::ResolveInfo& info);
  void RestoreDeferredResolveSourceUsage();
  void ReleaseDeferredResolveSources() { deferred_resolve_sources_.clear(); }

  bool Update(bool is_rasterization_done,
              reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask,
              const Shader& vertex_shader) override;
  // depth_and_color_load_dont_care only selects loadOp - it is not part of
  // what a framebuffer or pipeline IS (render passes differing only in loadOp
  // are compatible). Strip it from cache keys, or toggling a discard bit
  // yields a different Framebuffer* (and pipeline) and the pointer-identity
  // check ends the pass for nothing. The bits stay in
  // last_update_render_pass_key_, so real pass begins keep their DONT_CARE.
  RenderPassKey NormalizeKeyForCacheLookup(RenderPassKey key) const {
    if (cvars::vulkan_normalize_dontcare_keys) {
      key.depth_and_color_load_dont_care = 0;
    }
    return key;
  }

  // Binding information for the last successful update.
  // Whether the last Update performed the draw as a clear of another render
  // target - the draw must be skipped.
  using RenderTargetCache::last_update_depth_replaced_by_clear;
  using RenderTargetCache::last_update_draw_replaced_by_clear;
  using RenderTargetCache::last_update_fully_overwritten_mask;
  RenderPassKey last_update_render_pass_key() const {
    return last_update_render_pass_key_;
  }
  // A discard is a one-shot permission: once a pass has begun (or a draw was
  // encoded into an already-open pass) with it, reopening the pass for a
  // later draw that skipped Update (fast path) must load, not discard again
  // what the earlier draws wrote.
  void ConsumeLastUpdateLoadDontCare() {
    if (cvars::vulkan_consume_dontcare) {
      DropLastUpdateLoadDontCare(UINT32_MAX);
    }
  }
  VkRenderPass last_update_render_pass() const {
    return last_update_render_pass_;
  }
  const Framebuffer* last_update_framebuffer() const {
    if (retirement_tracking_enabled_) {
      MarkLastUpdateFramebufferReference();
    }
    return last_update_framebuffer_;
  }
  // Call on the GPU/CP thread before publishing any RT-derived handles.
  bool AcquireExternalRenderTargetAliasLease(RenderTarget* render_target);
  // Release on the GPU/CP thread after every external alias is dropped.
  void ReleaseExternalRenderTargetAliasLease(RenderTarget* render_target);
  // Render-target transfers that were queued by the last Update() to be encoded
  // inside the active guest render pass (in-pass transfers).
  bool HasPendingDrawPassTransfers() const {
    return pending_draw_pass_transfer_mask_ != 0;
  }
  // Encodes the queued transfers in the currently-active guest render pass.
  // Returns false if they can't be encoded in-pass (the caller must then
  // FlushPendingDrawPassTransfers and re-enter the pass).
  // draw_will_execute: the guest draw is recorded with a ready pipeline right
  // after, so the transfers may skip the rectangle it overwrites
  // (rt_cache_draw_overwrite_cutouts).
  bool EncodePendingDrawPassTransfers(bool draw_will_execute = false);
  // Falls back to performing the queued transfers in their own render pass(es),
  // ending the active pass. Always clears the queue. Without the draw cutout -
  // also performs the pending out-of-pass cutout transfers.
  bool FlushPendingDrawPassTransfers();
  // Transfers with a draw cutout that can't be done inside the guest pass,
  // deferred by Update() until the draw's pipeline is known. Must be called
  // before entering the guest render pass; with draw_will_execute false (or
  // from FlushPendingDrawPassTransfers) the whole ranges are transferred.
  // rt_cache_defer_retarget_clears: the clear Update kept for the guest pass
  // being entered. EncodeDeferredClear records it there (after the pass has
  // begun), FlushDeferredClear in a pass of its own as before.
  bool HasDeferredClearForPass() const {
    return deferred_clear_render_target_ && deferred_clear_in_pass_;
  }
  void EncodeDeferredClear();
  // vulkan_lazy_constant_rows: the pass's lazy clears clipped to its render
  // area at the end of the pass (DeferredCommandBuffer::ClipLazyClears).
  void OnLazyClearsClipped(const DeferredLazyClearResult* results,
                           uint32_t count);
  // The guest render pass of last_update_framebuffer_ has just begun: record
  // the pending constant rows of its attachments as lazy clears.
  void OnGuestRenderPassBegun();
  void FlushDeferredClear();
  bool HasPendingOutOfPassTransfers() const {
    return pending_out_of_pass_mask_ != 0;
  }
  void PerformPendingOutOfPassTransfers(bool draw_will_execute);
  // rt_cache_draw_clear_chains: whether the draw that started or extended the
  // clear chain really writes its pixels (decided once its pipeline is known).
  // If not, the chain is ended without that draw's rectangle.
  void ConfirmDrawClearChain(bool draw_will_execute);
  // Performs the transfers deferred by a chain of clear draws, around the
  // union of their rectangles. Before anything that may read or write the
  // render targets of the chain other than another clear of the chain.
  void FlushDrawClearChain();
  // Materialize pixels surviving a chain of proven depth-only clears before
  // a resolve or submission boundary. Does not re-clear the target.
  void FlushPendingClearTransfers();

  // For VK_KHR_dynamic_rendering: fills in attachment info structures.
  // Returns the number of color attachments (may be less than max if trailing
  // slots are unused). depth_attachment and stencil_attachment are filled if
  // depth is used (check key.depth_and_color_used & 0b1).
  void GetLastUpdateRenderingAttachments(
      VkRenderingAttachmentInfo* color_attachments,
      uint32_t* color_attachment_count_out,
      VkRenderingAttachmentInfo* depth_attachment,
      VkRenderingAttachmentInfo* stencil_attachment) const;

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    // TODO(Triang3l): Not float16 condition.
    return GetPath() == Path::kHostRenderTargets &&
           !cvars::snorm16_render_target_full_range;
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    // TODO(Triang3l): Not float16 condition.
    return GetPath() == Path::kHostRenderTargets &&
           !cvars::snorm16_render_target_full_range;
  }

  bool depth_unorm24_vulkan_format_supported() const {
    // The device-support half is cached at Initialize; the cvar is read live so
    // vulkan_depth_unorm24 can be overridden per-game (the per-game config loads
    // before any guest depth render target is created). false forces the
    // float32 depth-emulation path.
    return depth_unorm24_vulkan_format_supported_ && cvars::vulkan_depth_unorm24;
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }
  bool gamma_render_target_as_unorm16() const {
    return gamma_render_target_as_unorm16_;
  }

  // Color attachment usage for guest passes; switches to the
  // RENDERING_LOCAL_READ layout (+ fragment-stage input-attachment access)
  // when in-pass resolves are enabled and supported.
  bool local_read_attachments() const { return local_read_attachments_; }
  VkPipelineStageFlags color_draw_stage_mask() const {
    return color_draw_stage_mask_;
  }
  VkAccessFlags color_draw_access_mask() const {
    return color_draw_access_mask_;
  }
  VkImageLayout color_draw_layout() const { return color_draw_layout_; }

  bool msaa_2x_attachments_supported() const {
    return msaa_2x_attachments_supported_;
  }
  bool msaa_2x_no_attachments_supported() const {
    return msaa_2x_no_attachments_supported_;
  }
  // vulkan_msaa_single_sample_storage (latched at initialization): the host
  // images of multisampled guest render targets are single-sampled and
  // everything drawing to them rasterizes one sample per pixel. The guest
  // layout (the render target keys, the EDRAM addressing, the transfer and
  // resolve coordinate math) is unchanged - a pixel's samples just all read
  // the same host texel.
  bool msaa_single_sample_storage() const {
    return msaa_single_sample_storage_;
  }
  // vulkan_msaa_single_sample_resolve on top of it: passes still rasterize the
  // guest sample count (VK_EXT_multisampled_render_to_single_sampled) and
  // resolve into the single-sampled images when they end. Without it, the
  // pipelines drawing to render targets rasterize one sample.
  bool msaa_single_sample_rasterization() const {
    return msaa_single_sample_storage_ && !msaa_single_sample_resolve_;
  }
  // The rasterizationSamples of a multisampled-render-to-single-sampled pass
  // over render targets of the guest sample count, 0 if it isn't one.
  uint32_t GetMsrtssSampleCount(xenos::MsaaSamples msaa_samples) const {
    if (!msaa_single_sample_resolve_ ||
        msaa_samples == xenos::MsaaSamples::k1X) {
      return 0;
    }
    return (msaa_samples == xenos::MsaaSamples::k2X &&
            msaa_2x_attachments_supported_)
               ? 2
               : 4;
  }
  uint32_t GetLastUpdateMsrtssSampleCount() const {
    return GetMsrtssSampleCount(last_update_render_pass_key_.msaa_samples);
  }
  bool IsMsaa2xSupported(bool subpass_has_attachments) const {
    return subpass_has_attachments ? msaa_2x_attachments_supported_
                                   : msaa_2x_no_attachments_supported_;
  }

  // Returns the render pass object, or VK_NULL_HANDLE if failed to create.
  // A render pass managed by the render target cache may be ended and resumed
  // at any time (to allow for things like copying and texture loading).
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key,
                                              RenderPassOps ops);
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key) {
    return GetHostRenderTargetsRenderPass(key, RenderPassOps{});
  }
  bool GetHostRenderPassKey(VkRenderPass pass, RenderPassKey& key) const {
    auto it = render_pass_keys_.find(pass);
    if (it == render_pass_keys_.end()) return false;
    key = it->second;
    return true;
  }
  VkRenderPass GetFragmentShaderInterlockRenderPass() const {
    assert_true(GetPath() == Path::kPixelShaderInterlock);
    return fsi_render_pass_;
  }

  VkFormat GetDepthVulkanFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormat GetColorVulkanFormat(xenos::ColorRenderTargetFormat format) const;
  // Vulkan format of the attachment view described by a pass key and slot.
  VkFormat GetColorRenderPassVulkanFormat(RenderPassKey key,
                                          uint32_t color_slot) const;
  VkFormat GetColorOwnershipTransferVulkanFormat(
      xenos::ColorRenderTargetFormat format,
      bool* is_integer_out = nullptr) const;

  // RTTRACE diagnostic helper: formats a render target's key as
  // "base/pitch/fmt/msaa@ptr" (msaa_and_pointer true, for draw signatures - a
  // host pointer suffix distinguishes same-key host image identity) or
  // "base/pitch/fmt/depth" (false, for transfer/clear logging). Returns "-"
  // for a null render target. rt is type-erased (RenderTarget is protected)
  // so callers outside this class hierarchy, like VulkanCommandProcessor, can
  // hold and pass the identity without naming the type.
  static std::string FormatRtTraceKey(const void* rt, bool msaa_and_pointer);

  // RTTRACE diagnostic: opaque identities of the last update's accumulated
  // render targets (0 depth, 1..4 color; see last_update_accumulated_render_
  // targets), for a caller that can't name the protected RenderTarget type.
  // Pass entries to FormatRtTraceKey; compare with == / != for rebind checks.
  void GetRtTraceAccumulatedRenderTargets(
      const void** out_rts_1_plus_max_color_render_targets) const {
    RenderTarget* const* rts = last_update_accumulated_render_targets();
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      out_rts_1_plus_max_color_render_targets[i] = rts[i];
    }
  }

 protected:
  bool IsGammaFormatHostStorageSeparate() const override;

  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool SupportsNewOwnerClearTransfers() const override;

  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  class VulkanRenderTarget;
  void NotePassBytesRead(DeferredCommandBuffer& commands, const RenderTarget* source,
                         uint64_t pixels, bool resolve, uint64_t buffer_write = 0);
  struct FramebufferKey;
  bool HasPendingRenderTargetReference(
      const VulkanRenderTarget* render_target) const;
  void MarkLastUpdateFramebufferReference() const;
  bool FramebufferReferencesRenderTarget(
      const Framebuffer& framebuffer,
      const RenderTargetKey& render_target_key) const;
  bool EvictFramebuffersForRenderTarget(
      const VulkanRenderTarget& render_target,
      uint64_t completed_submission, uint64_t current_submission,
      bool submission_open);
  void RetireUnownedRenderTargets();
  uint64_t last_retirement_scan_submission_ = ~uint64_t(0);
  // Latched at backend construction. Enabling retirement after untracked GPU
  // use makes old submission references unknowable, so it fails closed until
  // the backend is recreated.
  bool retirement_tracking_enabled_ = false;
  bool retirement_tracking_enabled() const {
    return retirement_tracking_enabled_;
  }
  bool HasExternalRenderTargetReferences(
      const RenderTarget* render_target) const override;

  static bool ValidateDrawExtentInputRead(
      void* context, uint32_t address, uint32_t length,
      DrawExtentEstimator::InputReadKind kind);

  enum class EdramBufferUsage {
    // There's no need for combined fragment and compute usages.
    // With host render targets, the usual usage sequence is as follows:
    // - Optionally compute writes - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Optionally fragment reads - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Compute writes - copying from host render targets during resolving.
    // - Compute reads - writing to the shared memory during resolving.
    // With the render backend implementation based on fragment shader
    // interlocks, it's:
    // - Fragment reads and writes - depth / stencil and color operations.
    // - Compute reads - writing to the shared memory during resolving.
    // So, fragment reads and compute reads normally don't follow each other,
    // and there's no need to amortize the cost of a read > read barrier in an
    // exceptional situation by using a wider barrier in the normal scenario.

    // Host depth copy storing.
    kFragmentRead,
    // Fragment shader interlock depth / stencil and color operations.
    kFragmentReadWrite,
    // Resolve - copying to the shared memory.
    kComputeRead,
    // Resolve - copying from host render targets.
    kComputeWrite,
    // Trace recording.
    kTransferRead,
    // Trace playback.
    kTransferWrite,
  };

  enum class EdramBufferModificationStatus {
    // The values are ordered by how strong the barrier conditions are.
    // No uncommitted shader writes.
    kUnmodified,
    // Need to commit before the next fragment shader interlock usage with
    // overlap.
    kViaFragmentShaderInterlock,
    // Need to commit before any next fragment shader interlock usage.
    kViaUnordered,
  };

  enum ResolveCopyDescriptorSet : uint32_t {
    // Never changes.
    kResolveCopyDescriptorSetEdram,
    // Shared memory or a region in it.
    kResolveCopyDescriptorSetDest,

    kResolveCopyDescriptorSetUnused,
    kResolveCopyDescriptorSetLinear,
    kResolveCopyDescriptorSetCount,
  };

  struct ResolveCopyShaderCode {
    const uint32_t* unscaled;
    size_t unscaled_size_bytes;
    const uint32_t* scaled;
    size_t scaled_size_bytes;
  };

  struct DirectHostResolveShaderCode {
    const uint32_t* code;
    size_t size_bytes;
    const char* debug_name;
  };

  static constexpr size_t kDirectHostResolveBppCount = 2;     // 32, 64
  // 1, 2, 4, and 4 kept as a 1x image of twice the size (samples as texels).
  static constexpr size_t kDirectHostResolveMsaaCount = 4;
  static constexpr size_t kDirectHostResolveMsaa2x2Index = 3;
  static constexpr size_t kDirectHostResolveScaledCount = 2;  // false, true
  static constexpr size_t kDirectHostResolveSourceUintCount = 2;
  static constexpr size_t kDirectHostResolveFullDestCount =
      5;  // 8, 16, 32, 64, 128

  static void GetEdramBufferUsageMasks(EdramBufferUsage usage,
                                       VkPipelineStageFlags& stage_mask_out,
                                       VkAccessFlags& access_mask_out);
  void UseEdramBuffer(EdramBufferUsage new_usage);
  void MarkEdramBufferModified(
      EdramBufferModificationStatus modification_status =
          EdramBufferModificationStatus::kViaUnordered);
  void CommitEdramBufferShaderWrites(
      EdramBufferModificationStatus commit_status =
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;

  Path path_ = Path::kHostRenderTargets;

  // Cached SPIR-V version based on device capabilities.
  unsigned int spirv_version_;

  // Accessible in fragment and compute shaders.
  VkDescriptorSetLayout descriptor_set_layout_storage_buffer_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_x2_ =
      VK_NULL_HANDLE;

  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      descriptor_set_pool_sampled_image_;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      descriptor_set_pool_sampled_image_x2_;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      descriptor_set_pool_input_attachment_;

  VkDeviceMemory edram_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  bool TransferRuntimeHostTargets(std::vector<uint8_t>& bytes, bool restore);
  struct SnapshotQuarantine { VkBuffer buffer; VkDeviceMemory memory; };
  std::vector<SnapshotQuarantine> snapshot_quarantine_;
  EdramBufferUsage edram_buffer_usage_;
  struct DeferredResolveSource {
    VulkanRenderTarget* target;
    VkPipelineStageFlags stages;
    VkAccessFlags access;
    VkImageLayout layout;
  };
  std::vector<DeferredResolveSource> deferred_resolve_sources_;
  EdramBufferUsage deferred_resolve_edram_usage_;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPool edram_storage_buffer_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet edram_storage_buffer_descriptor_set_;

  VkPipelineLayout resolve_copy_pipeline_layout_ = VK_NULL_HANDLE;
  static const ResolveCopyShaderCode
      kResolveCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  // Same layout, but the full-resolve entries drop destination number format
  // and PWL gamma handling. Selected by accurate_resolve_number_formats.
  static const ResolveCopyShaderCode kResolveCopyShadersFastFormats[size_t(
      draw_util::ResolveCopyShaderIndex::kCount)];
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_pipelines_{};
  // Unscaled variants for fully native resolves. Only created with resolution
  // scaling, otherwise the main set is already unscaled. A separate set
  // because the scaled shaders can't just run at 1x1, their push constants
  // and destination address space assume the scaled resolve buffer.
  VkPipelineLayout resolve_copy_native_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_native_pipelines_{};

  VkPipelineLayout direct_host_resolve_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout direct_host_resolve_pipeline_layout_depth_ = VK_NULL_HANDLE;
  static const DirectHostResolveShaderCode kDirectHostResolveColorShaders
      [kDirectHostResolveBppCount][kDirectHostResolveMsaaCount]
      [kDirectHostResolveScaledCount][kDirectHostResolveSourceUintCount];
  static const DirectHostResolveShaderCode kDirectHostResolveColorFullShaders
      [kDirectHostResolveMsaaCount][kDirectHostResolveScaledCount]
      [kDirectHostResolveSourceUintCount][kDirectHostResolveFullDestCount];
  static const DirectHostResolveShaderCode
      kDirectHostResolveDepthShaders[kDirectHostResolveMsaaCount]
                                    [kDirectHostResolveScaledCount];
  VkPipeline direct_host_resolve_pipelines_
      [kDirectHostResolveBppCount][kDirectHostResolveMsaaCount]
      [kDirectHostResolveScaledCount][kDirectHostResolveSourceUintCount] = {};
  VkPipeline direct_host_color_full_resolve_pipelines_
      [kDirectHostResolveMsaaCount][kDirectHostResolveScaledCount]
      [kDirectHostResolveSourceUintCount][kDirectHostResolveFullDestCount] = {};
  VkPipeline
      direct_host_depth_resolve_pipelines_[kDirectHostResolveMsaaCount]
                                          [kDirectHostResolveScaledCount] = {};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool>
      direct_host_resolve_constants_pool_;

  // On the fragment shader interlock path, the render pass key is used purely
  // for passing parameters to pipeline setup - there's always only one render
  // pass.
  RenderPassKey last_update_render_pass_key_;
  VkRenderPass last_update_render_pass_ = VK_NULL_HANDLE;
  // The pitch is not used on the fragment shader interlock path.
  uint32_t last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  // The attachments are not used on the fragment shader interlock path.
  const RenderTarget*
      last_update_framebuffer_attachments_[1 + xenos::kMaxColorRenderTargets] =
          {};
  const Framebuffer* last_update_framebuffer_ = VK_NULL_HANDLE;

  bool local_read_attachments_ = false;
  bool r32_uint_input_attachment_supported_ = false;
  // Stencil bit transfer draws (no stencil export) drawn and skipped by
  // rt_cache_stencil_bit_transfer_mask, logged every 8192.
  uint64_t stencil_bit_draws_done_ = 0;
  uint64_t stencil_bit_draws_skipped_ = 0;
  uint64_t stencil_bit_draws_log_next_ = 8192;
  uint64_t inpass_attempts_ = 0;
  uint64_t inpass_taken_ = 0;
  uint64_t inpass_conversions_ = 0;
  uint64_t inpass_conversion_bytes_ = 0;
  uint64_t inpass_format_hist_[16] = {};
  static constexpr uint32_t kInPassRejectReasons = 24;
  const char* inpass_reject_reasons_[kInPassRejectReasons] = {};
  uint64_t inpass_reject_counts_[kInPassRejectReasons] = {};
  uint32_t inpass_reject_reason_count_ = 0;
  uint32_t inpass_combos_[16] = {};
  uint32_t inpass_combo_count_ = 0;
  bool use_dynamic_rendering_ = false;
  VkPipelineStageFlags color_draw_stage_mask_ =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkAccessFlags color_draw_access_mask_ =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkImageLayout color_draw_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  // For host render targets.

  // Can only be destroyed when framebuffers referencing it are destroyed!
  class VulkanRenderTarget final : public RenderTarget {
    friend class VulkanRenderTargetCache;
   public:
    static constexpr VkPipelineStageFlags kColorDrawStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    static constexpr VkAccessFlags kColorDrawAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kColorDrawLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    static constexpr VkPipelineStageFlags kDepthDrawStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr VkAccessFlags kDepthDrawAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kDepthDrawLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Takes ownership of the Vulkan objects passed to the constructor.
    VulkanRenderTarget(RenderTargetKey key,
                       VulkanRenderTargetCache& render_target_cache,
                       VkImage image, VkDeviceMemory memory,
                       VkDeviceSize allocation_size,
                       VkImageView view_depth_color,
                       VkImageView view_depth_stencil, VkImageView view_stencil,
                       VkImageView view_color_transfer_separate,
                       size_t descriptor_set_index_transfer_source,
                       bool transfer_src_enabled)
        : RenderTarget(key),
          render_target_cache_(render_target_cache),
          image_(image),
          memory_(memory),
          allocation_size_(allocation_size),
          view_depth_color_(view_depth_color),
          view_depth_stencil_(view_depth_stencil),
          view_stencil_(view_stencil),
          view_color_transfer_separate_(view_color_transfer_separate),
          descriptor_set_index_transfer_source_(
              descriptor_set_index_transfer_source),
          transfer_src_enabled_(transfer_src_enabled) {}
    ~VulkanRenderTarget();

    // vulkan_stencil_transfer_copy: created with the transfer destination
    // usage, so the stencil aspect can be copied into.
    bool stencil_copy_destination = false;

    // Defined out of line after VulkanCommandProcessor is complete. Handle
    // getters call it only when tracking was enabled at backend construction.
    void MarkGpuReference() const;
    void MarkGpuReference(uint64_t submission) const;
    void MarkGpuReference(uint64_t submission, uint64_t frame) const;
    bool gpu_reference_submission_known() const {
      return gpu_reference_submission_known_;
    }
    uint64_t last_gpu_reference_submission() const {
      return last_gpu_reference_submission_;
    }
    bool gpu_reference_frame_known() const {
      return last_gpu_reference_frame_ != 0;
    }
    uint64_t last_gpu_reference_frame() const {
      return last_gpu_reference_frame_;
    }
    VkDeviceSize allocation_size() const { return allocation_size_; }

    // Any non-RTC cache that retains one of this RT's handles must hold a lease.
    bool AcquireExternalAliasLease() {
      return external_alias_leases_.Acquire();
    }
    void ReleaseExternalAliasLease() { external_alias_leases_.Release(); }
    bool external_alias_leases_known() const {
      return external_alias_leases_.known();
    }
    uint32_t external_alias_lease_count() const {
      return external_alias_leases_.count();
    }

    VkImage image() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return image_;
    }
    bool transfer_src_enabled() const { return transfer_src_enabled_; }

    VkImageView view_depth_color() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return view_depth_color_;
    }
    VkImageView view_depth_stencil() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return view_depth_stencil_;
    }
    VkImageView view_color_transfer_separate() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return view_color_transfer_separate_;
    }
    VkImageView view_color_transfer() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return view_color_transfer_separate_ != VK_NULL_HANDLE
                 ? view_color_transfer_separate_
                 : view_depth_color_;
    }
    void SetDescriptorSetIndexInputAttachment(size_t index) {
      descriptor_set_index_input_attachment_ = index;
    }
    void SetDescriptorSetIndexInputAttachmentUint(size_t index) {
      descriptor_set_index_input_attachment_uint_ = index;
    }
    VkDescriptorSet GetDescriptorSetInputAttachment(bool is_uint = false) const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      return render_target_cache_.descriptor_set_pool_input_attachment_->Get(
          is_uint ? descriptor_set_index_input_attachment_uint_
                  : descriptor_set_index_input_attachment_);
    }
    bool HasDescriptorSetInputAttachment(bool is_uint = false) const {
      return (is_uint ? descriptor_set_index_input_attachment_uint_
                      : descriptor_set_index_input_attachment_) != SIZE_MAX;
    }
    VkDescriptorSet GetDescriptorSetTransferSource() const {
      if (render_target_cache_.retirement_tracking_enabled()) {
        MarkGpuReference();
      }
      ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
          key().is_depth
              ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
              : *render_target_cache_.descriptor_set_pool_sampled_image_;
      return descriptor_set_pool.Get(descriptor_set_index_transfer_source_);
    }

    void GetDrawUsage(VkPipelineStageFlags* stage_mask_out,
                      VkAccessFlags* access_mask_out,
                      VkImageLayout* layout_out) const {
      bool is_depth = key().is_depth;
      if (stage_mask_out) {
        *stage_mask_out = is_depth
                              ? kDepthDrawStageMask
                              : render_target_cache_.color_draw_stage_mask();
      }
      if (access_mask_out) {
        *access_mask_out = is_depth
                               ? kDepthDrawAccessMask
                               : render_target_cache_.color_draw_access_mask();
      }
      if (layout_out) {
        *layout_out = is_depth ? kDepthDrawLayout
                               : render_target_cache_.color_draw_layout();
      }
    }
    VkPipelineStageFlags current_stage_mask() const {
      return current_stage_mask_;
    }
    VkAccessFlags current_access_mask() const { return current_access_mask_; }
    VkImageLayout current_layout() const { return current_layout_; }
    void SetUsage(VkPipelineStageFlags stage_mask, VkAccessFlags access_mask,
                  VkImageLayout layout) {
      current_stage_mask_ = stage_mask;
      current_access_mask_ = access_mask;
      current_layout_ = layout;
    }

    uint32_t temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(uint32_t index) {
      temporary_sort_index_ = index;
    }

   private:
    VulkanRenderTargetCache& render_target_cache_;
    size_t descriptor_set_index_input_attachment_ = SIZE_MAX;
    size_t descriptor_set_index_input_attachment_uint_ = SIZE_MAX;

    VkImage image_;
    VkDeviceMemory memory_;
    VkDeviceSize allocation_size_ = 0;
    mutable uint64_t last_gpu_reference_submission_ = 0;
    // Frame zero is never used by the command processor (it starts at one),
    // so it represents an unknown last-use frame without another bool.
    mutable uint64_t last_gpu_reference_frame_ = 0;
    mutable bool gpu_reference_submission_known_ = false;
    RenderTargetAliasLeases external_alias_leases_;

    // TODO(Triang3l): Per-format drawing views for mutable formats with EDRAM
    // aliasing without transfers.
    VkImageView view_depth_color_;
    // Optional views.
    VkImageView view_depth_stencil_;
    VkImageView view_stencil_;
    VkImageView view_color_transfer_separate_;

    // 2 sampled images for depth / stencil, 1 sampled image for color.
    size_t descriptor_set_index_transfer_source_;
    bool transfer_src_enabled_;

    VkPipelineStageFlags current_stage_mask_ = 0;
    VkAccessFlags current_access_mask_ = 0;
    VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Temporary storage for indices in operations like transfers and dumps.
    uint32_t temporary_sort_index_ = 0;
  };

  struct FramebufferKey {
    RenderPassKey render_pass_key;

    // Same as RenderTargetKey::pitch_tiles_at_32bpp.
    uint32_t pitch_tiles_at_32bpp : 8;  // 8
    // [0, 2047].
    uint32_t depth_base_tiles : xenos::kEdramBaseTilesBits;    // 19
    uint32_t color_0_base_tiles : xenos::kEdramBaseTilesBits;  // 30

    uint32_t color_1_base_tiles : xenos::kEdramBaseTilesBits;  // 43
    uint32_t color_2_base_tiles : xenos::kEdramBaseTilesBits;  // 54

    uint32_t color_3_base_tiles : xenos::kEdramBaseTilesBits;  // 75
    // Exact RenderTargetKey::height_class shared by all attachments.
    uint32_t height_class : 2;  // 76-77

    // Including all the padding, for a stable hash.
    FramebufferKey() { Reset(); }
    FramebufferKey(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
    }
    FramebufferKey& operator=(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
      return *this;
    }
    bool operator==(const FramebufferKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    using Hasher = xe::hash::XXHasher<FramebufferKey>;
    void Reset() { std::memset(this, 0, sizeof(*this)); }
  };

  enum TransferUsedDescriptorSet : uint32_t {
    // Ordered from the least to the most frequently changed.
    kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTextures,
    // Mutually exclusive with kTransferUsedDescriptorSetDepthStencilTextures.
    kTransferUsedDescriptorSetColorTexture,

    kTransferUsedDescriptorSetCount,

    kTransferUsedDescriptorSetHostDepthBufferBit =
        uint32_t(1) << kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetDepthStencilTextures,
    kTransferUsedDescriptorSetColorTextureBit =
        uint32_t(1) << kTransferUsedDescriptorSetColorTexture,
  };

  // 32-bit push constants (for simplicity of size calculation and to avoid
  // std140 packing issues).
  enum TransferUsedPushConstantDword : uint32_t {
    kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddress,
    // Changed 8 times per transfer.
    kTransferUsedPushConstantDwordStencilMask,

    kTransferUsedPushConstantDwordCount,

    kTransferUsedPushConstantDwordHostDepthAddressBit =
        uint32_t(1) << kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddressBit =
        uint32_t(1) << kTransferUsedPushConstantDwordAddress,
    kTransferUsedPushConstantDwordStencilMaskBit =
        uint32_t(1) << kTransferUsedPushConstantDwordStencilMask,
  };

  enum class TransferPipelineLayoutIndex {
    kColor,
    kDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthTexture,
    kColorAndHostDepthBuffer,
    kDepthAndHostDepthTexture,
    kDepthAndHostDepthBuffer,

    kCount,
  };

  struct TransferPipelineLayoutInfo {
    uint32_t used_descriptor_sets;
    uint32_t used_push_constant_dwords;
  };

  static const TransferPipelineLayoutInfo
      kTransferPipelineLayoutInfos[size_t(TransferPipelineLayoutIndex::kCount)];

  // rt_cache_transfer_known_constants: transfers done as clears.
  uint64_t constant_transfer_clears_ = 0;

  enum class TransferMode : uint32_t {
    kColorToDepth,
    kColorToColor,

    kDepthToDepth,
    kDepthToColor,

    kColorToStencilBit,
    kDepthToStencilBit,

    // Two-source modes, using the host depth if it, when converted to the guest
    // format, matches what's in the owner source (not modified, keep host
    // precision), or the guest data otherwise (significantly modified, possibly
    // cleared). Stencil for FragStencilRef is always taken from the guest
    // source.

    kColorAndHostDepthToDepth,
    // When using different source and destination depth formats.
    kDepthAndHostDepthToDepth,

    // If host depth is fetched, but it's the same image as the destination,
    // it's copied to the EDRAM buffer (but since it's just a scratch buffer,
    // with tiles laid out linearly with the same pitch as in the original
    // render target; also no swapping of 40-sample columns as opposed to the
    // host render target - this is done only for the color source) and fetched
    // from there instead of the host depth texture.
    kColorAndHostDepthCopyToDepth,
    kDepthAndHostDepthCopyToDepth,

    kCount,
  };

  enum class TransferOutput {
    kColor,
    kDepth,
    kStencilBit,
  };

  struct TransferModeInfo {
    TransferOutput output;
    TransferPipelineLayoutIndex pipeline_layout;
  };

  static const TransferModeInfo kTransferModes[size_t(TransferMode::kCount)];

  union TransferShaderKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
      uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
      // Always 1x when the host depth is a copy from a buffer rather than an
      // image, not to create the same pipeline for different MSAA sample counts
      // as it doesn't matter in this case.
      xenos::MsaaSamples host_depth_source_msaa_samples
          : xenos::kMsaaSamplesBits;
      uint32_t source_resource_format : xenos::kRenderTargetFormatBits;
      // Decode (not bit-reinterpret) a 7e3 <-> 8_8_8_8 reuse. See
      // IsTransferValueConverted7e3And8888.
      uint32_t value_convert : 1;
      // Scale classes of the two sides. The shader bakes each side's tile
      // size and conversion between the scale spaces.
      uint32_t dest_scale_native : 1;
      uint32_t source_scale_native : 1;

      // Last bits because this affects the pipeline layout - after sorting,
      // only change it as fewer times as possible. Depth buffers have an
      // additional stencil texture.
      static_assert(size_t(TransferMode::kCount) <= (size_t(1) << 4));
      TransferMode mode : 4;
    };

    TransferShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TransferShaderKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TransferShaderKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const TransferShaderKey& other_key) const {
      return key < other_key.key;
    }
  };

  struct TransferPipelineKey {
    RenderPassKey render_pass_key;
    TransferShaderKey shader_key;

    TransferPipelineKey(RenderPassKey render_pass_key,
                        TransferShaderKey shader_key)
        : render_pass_key(render_pass_key), shader_key(shader_key) {}

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        XXH3_state_t hash_state;
        XXH3_64bits_reset(&hash_state);
        XXH3_64bits_update(&hash_state, &key.render_pass_key,
                           sizeof(key.render_pass_key));
        XXH3_64bits_update(&hash_state, &key.shader_key,
                           sizeof(key.shader_key));
        return static_cast<size_t>(XXH3_64bits_digest(&hash_state));
      }
    };
    bool operator==(const TransferPipelineKey& other_key) const {
      return render_pass_key == other_key.render_pass_key &&
             shader_key == other_key.shader_key;
    }
    bool operator!=(const TransferPipelineKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const TransferPipelineKey& other_key) const {
      if (render_pass_key != other_key.render_pass_key) {
        return render_pass_key < other_key.render_pass_key;
      }
      return shader_key < other_key.shader_key;
    }
  };

  union TransferAddressConstant {
    uint32_t constant;
    struct {
      // All in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
      // Destination base in tiles minus source base in tiles (not vice versa
      // because this is a transform of the coordinate system, not addresses
      // themselves).
      // + 1 bit because this is a signed difference between two EDRAM bases.
      // 0 for host_depth_source_is_copy (ignored in this case anyway as
      // destination == source anyway).
      int32_t source_to_dest : xenos::kEdramBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) {
      static_assert_size(*this, sizeof(constant));
    }
    bool operator==(const TransferAddressConstant& other_constant) const {
      return constant == other_constant.constant;
    }
    bool operator!=(const TransferAddressConstant& other_constant) const {
      return !(*this == other_constant);
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    TransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer,
                       const TransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      // TODO(Triang3l): See if it may be better to sort by the source in the
      // first place, especially when reading the same data multiple times (like
      // to write the stencil bits after depth) for better read locality.
      // Sort by the shader key primarily to reduce pipeline state (context)
      // switches.
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      // Host depth render targets are changed rarely if they exist, won't save
      // many binding changes, ignore them for simplicity (their existence is
      // caught by the shader key change).
      assert_not_null(transfer.source);
      assert_not_null(other_invocation.transfer.source);
      uint32_t source_index =
          static_cast<const VulkanRenderTarget*>(transfer.source)
              ->temporary_sort_index();
      uint32_t other_source_index = static_cast<const VulkanRenderTarget*>(
                                        other_invocation.transfer.source)
                                        ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(
        const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  struct TransferRectanglePlan {
    std::array<Transfer::Rectangle, Transfer::kMaxRectanglesWithCutout>
        rectangles;
    uint32_t rectangle_count = 0;
  };

  union DumpPipelineKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples msaa_samples : 2;
      uint32_t resource_format : 4;
      // Last bit because this affects the pipeline - after sorting, only change
      // it at most once. Depth buffers have an additional stencil SRV.
      uint32_t is_depth : 1;
      // Dumping to the scaled EDRAM layout duplicates this native render
      // target's guest pixels.
      uint32_t source_scale_native : 1;
      // source_scale_native only.
      // Address the EDRAM buffer with the plain 1x1 tile layout.
      uint32_t native_layout : 1;
    };

    DumpPipelineKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const DumpPipelineKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const DumpPipelineKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const DumpPipelineKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const DumpPipelineKey& other_key) const {
      return key < other_key.key;
    }

    xenos::ColorRenderTargetFormat GetColorFormat() const {
      assert_false(is_depth);
      return xenos::ColorRenderTargetFormat(resource_format);
    }
    xenos::DepthRenderTargetFormat GetDepthFormat() const {
      assert_true(is_depth);
      return xenos::DepthRenderTargetFormat(resource_format);
    }
  };

  // There's no strict dependency on the group size in dumping, for simplicity
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). Using 8x16 since that's 128 - the minimum required
  // group size on Vulkan, and the maximum number of lanes in a subgroup on
  // Vulkan.
  static constexpr uint32_t kDumpSamplesPerGroupX = 8;
  static constexpr uint32_t kDumpSamplesPerGroupY = 16;

  union DumpPitches {
    uint32_t pitches;
    struct {
      // Both in tiles.
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
    };
    DumpPitches() : pitches(0) { static_assert_size(*this, sizeof(pitches)); }
    bool operator==(const DumpPitches& other_pitches) const {
      return pitches == other_pitches.pitches;
    }
    bool operator!=(const DumpPitches& other_pitches) const {
      return !(*this == other_pitches);
    }
  };

  union DumpOffsets {
    uint32_t offsets;
    struct {
      // May be beyond the EDRAM tile count in case of EDRAM addressing
      // wrapping, thus + 1 bit.
      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;
      uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
    };
    DumpOffsets() : offsets(0) { static_assert_size(*this, sizeof(offsets)); }
    bool operator==(const DumpOffsets& other_offsets) const {
      return offsets == other_offsets.offsets;
    }
    bool operator!=(const DumpOffsets& other_offsets) const {
      return !(*this == other_offsets);
    }
  };

  enum DumpDescriptorSet : uint32_t {
    // Never changes. Same in both color and depth pipeline layouts, keep the
    // first for pipeline layout compatibility, to only have to set it once.
    kDumpDescriptorSetEdram,
    // One resolve may need multiple sources. Different descriptor set layouts
    // for color and depth.
    kDumpDescriptorSetSource,

    kDumpDescriptorSetCount,
  };

  enum DumpPushConstant : uint32_t {
    // May be different for different sources.
    kDumpPushConstantPitches,
    // May be changed multiple times for the same source.
    kDumpPushConstantOffsets,

    kDumpPushConstantCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    DumpPipelineKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle,
                   const DumpPipelineKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      // Sort by the pipeline key primarily to reduce pipeline state (context)
      // switches.
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      assert_not_null(rectangle.render_target);
      uint32_t render_target_index =
          static_cast<const VulkanRenderTarget*>(rectangle.render_target)
              ->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle =
          other_invocation.rectangle;
      uint32_t other_render_target_index =
          static_cast<const VulkanRenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  // Returns the framebuffer object, or VK_NULL_HANDLE if failed to create.
  const Framebuffer* GetHostRenderTargetsFramebuffer(
      RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
      const RenderTarget* const* depth_and_color_render_targets);

  VkShaderModule GetTransferShader(TransferShaderKey key);
  // With sample-rate shading, returns a pointer to one pipeline. Without
  // sample-rate shading, returns a pointer to as many pipelines as there are
  // samples. If there was a failure to create a pipeline, returns nullptr.
  VkPipeline const* GetTransferPipelines(TransferPipelineKey key);

  // In-pass fragment resolve (VK_KHR_dynamic_rendering_local_read); the color
  // slot is the current pass's color attachment being resolved.
  VkPipeline GetResolveInPassPipeline(RenderPassKey render_pass_key,
                                      uint32_t color_slot, bool is_64bpp);

  // Selects the transfer mode for one ownership transfer from the source/dest
  // aspects. Shared by PerformTransfersAndResolveClears and the draw-pass
  // transfer preflight so the two paths can't disagree on the mode.
  static TransferMode GetTransferMode(bool is_stencil_bit_pass,
                                      bool dest_is_depth, bool source_is_depth,
                                      bool has_host_depth_source,
                                      bool host_depth_source_is_copy);

  // Do ownership transfers for render targets - each render target / vector may
  // be null / empty in case there's nothing to do for them.
  // resolve_clear_rectangle is expected to be provided by
  // PrepareHostRenderTargetsResolveClear which should do all the needed size
  // bound checks.
  // When in_current_render_pass is true, the transfers are encoded inside the
  // already-active guest render pass / framebuffer (last_update_*), so the
  // barrier and render-pass enter/exit setup is skipped - the caller is
  // responsible for having issued the source barriers and entered the pass.
  bool PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr,
      bool in_current_render_pass = false,
      // Without resolve clears: whether the stencil bit draws (and the
      // stencil clear before them) skip stencil_cutout instead of
      // resolve_clear_rectangle, which the depth and color draws skip (the
      // per-aspect clear chains). stencil_cutout may be null - no cutout.
      bool stencil_cutout_separate = false,
      const Transfer::Rectangle* stencil_cutout = nullptr);

  // Queuing of transfers for in-pass execution.
  void ClearPendingDrawPassTransfers();
  // Clears discard bits of the last update; render passes bake loadOp, so
  // without dynamic rendering also switches to the compatible LOAD pass.
  void DropLastUpdateLoadDontCare(uint32_t mask);
  bool CanQueueDrawPassTransfers(uint32_t render_target_index,
                                 RenderTarget* const* render_targets,
                                 const std::vector<Transfer>& transfers) const;
  // The checks of CanQueueDrawPassTransfers for a pass whose attachments are
  // told by is_attachment.
  bool CanRunTransfersInGuestPass(
      uint32_t render_target_index, RenderTarget* dest_rt,
      const std::vector<Transfer>& transfers,
      const std::function<bool(const RenderTarget*)>& is_attachment) const;
  // rt_cache_clear_chain_in_pass: a flushed clear chain's transfers into slot
  // render_target_index can run inside the guest pass that is still open.
  // The vkCmdClearAttachments of a resolve clear value for dest_vulkan_rt
  // (colorAttachment 0 for color - the caller sets the real index).
  VkClearAttachment MakeResolveClearAttachment(VulkanRenderTarget& dest_vulkan_rt,
                                               uint64_t clear_value);
  bool CanFlushClearChainInCurrentPass(uint32_t render_target_index,
                                       RenderTarget* dest_rt,
                                       const std::vector<Transfer>& transfers);
  bool PreflightTransferPipelines(RenderPassKey render_pass_key,
                                  uint32_t render_target_index,
                                  RenderTarget* dest_rt,
                                  const std::vector<Transfer>& transfers);
  // log_in_pass_transfer_rejects: index 0 = accepted, otherwise the reason
  // CanQueueDrawPassTransfers sent the transfers to a pass of their own.
  mutable std::array<uint32_t, 20> in_pass_transfer_verdicts_ = {};
  uint32_t in_pass_transfer_frames_ = 0;
  // Callers (module offsets) of transfers that got a pass of their own.
  std::array<std::pair<uint32_t, uint32_t>, 8> own_pass_transfer_callers_ = {};
  // Fills one plan per transfer; returns false (and clears output) if any
  // transfer yields zero rectangles, so on success plans match transfers 1:1.
  bool BuildTransferRectanglePlans(
      RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
      std::vector<TransferRectanglePlan>& transfer_rectangles_out) const;
  // True only when the queued transfers provably overwrite the entire target,
  // making a DONT_CARE load of its prior contents safe.
  bool PendingDrawPassTransfersFullyOverwriteTarget(
      uint32_t render_target_index, RenderTarget* render_target,
      const std::vector<Transfer>& transfers) const;
  // Verifies every queued transfer can build its pipeline for render_pass_key.
  bool PreflightPendingDrawPassTransfers(RenderPassKey render_pass_key);
  void PreparePendingDrawPassTransferBarriers();
  // Moves the transfer sources of the clear chains in slot_mask that aren't
  // among bound_render_targets to the fragment shader read state.
  void PrepareClearChainSourceBarriers(uint32_t slot_mask,
                                       RenderTarget* const* bound_render_targets);
  // Moves the sources of the transfers that aren't among the 1 + 4
  // bound_render_targets to the fragment shader read state.
  void PrepareTransferSourceBarriers(const std::vector<Transfer>& transfers,
                                     RenderTarget* const* bound_render_targets);

  // RTTRACE diagnostic: logs one "RTTRACE xfer" line per Transfer targeting
  // render target slot `slot` (0 depth, 1..4 color), reflecting a decision
  // (in-pass queuing / full-overwrite) already made by the caller - never
  // changes it. No-op if transfers is empty. Only called when
  // command_processor_.IsRtTraceActive().
  void LogRtTraceTransfers(uint32_t slot, RenderTarget* dest,
                           const std::vector<Transfer>& transfers,
                           bool in_pass, bool full_overwrite);

  VkPipeline GetDumpPipeline(DumpPipelineKey key);

  // source_2x2: a 4x resolve of a 1x image of twice the size.
  VkPipeline GetDirectHostResolvePipeline(bool is_64bpp,
                                          xenos::MsaaSamples msaa_samples,
                                          bool scaled, bool source_is_uint,
                                          bool source_2x2 = false);
  VkPipeline GetDirectHostColorFullResolvePipeline(
      xenos::MsaaSamples msaa_samples, bool scaled, bool source_is_uint,
      draw_util::ResolveCopyShaderIndex copy_shader, bool source_2x2 = false);
  VkPipeline GetDirectHostDepthResolvePipeline(xenos::MsaaSamples msaa_samples,
                                               bool scaled);
  bool TryInPassResolveCopy(
      const draw_util::ResolveInfo& resolve_info,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
      uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
      VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
      uint32_t& written_address_out, uint32_t& written_length_out);
  bool TryDirectHostResolveCopy(
      const draw_util::ResolveInfo& resolve_info,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      draw_util::ResolveCopyShaderIndex copy_shader, uint32_t dump_base,
      uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
      VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache,
      uint32_t& written_address_out, uint32_t& written_length_out);
  bool CaptureSourceRenderTarget(const draw_util::ResolveInfo& resolve_info,
                                 uint32_t dump_base,
                                 uint32_t dump_row_length_used,
                                 uint32_t dump_rows, uint32_t dump_pitch);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_ - with the plain 1x1
  // tile layout if native_layout is set.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         bool native_layout);

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_unorm24_vulkan_format_supported_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_attachments_supported_ = false;
  bool msaa_single_sample_storage_ = false;
  bool msaa_single_sample_resolve_ = false;
  bool msaa_2x_no_attachments_supported_ = false;

  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<uint64_t, VkRenderPass> render_passes_;
  std::unordered_map<VkRenderPass, RenderPassKey> render_pass_keys_;

  std::unordered_map<FramebufferKey, Framebuffer, FramebufferKey::Hasher>
      framebuffers_;

  // Set 0 - EDRAM storage buffer, set 1 - source depth sampled image (and
  // unused stencil from the transfer descriptor set), HostDepthStoreConstants
  // passed via push constants.
  VkPipelineLayout host_depth_store_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X) + 1] =
      {};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool>
      transfer_vertex_buffer_pool_;
  VkShaderModule transfer_passthrough_vertex_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_pipeline_layouts_[size_t(
      TransferPipelineLayoutIndex::kCount)] = {};
  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<TransferShaderKey, VkShaderModule,
                     TransferShaderKey::Hasher>
      transfer_shaders_;
  // With sample-rate shading, one pipeline per entry. Without sample-rate
  // shading, one pipeline per sample per entry. VK_NULL_HANDLE if failed to
  // create.
  std::unordered_map<TransferPipelineKey, std::array<VkPipeline, 4>,
                     TransferPipelineKey::Hasher>
      transfer_pipelines_;

  // In-pass fragment resolve objects, only created in local-read mode.
  VkDescriptorSetLayout descriptor_set_layout_input_attachment_ =
      VK_NULL_HANDLE;
  VkPipelineLayout resolve_inpass_pipeline_layout_ = VK_NULL_HANDLE;
  // Bit 0: 64bpp dest, bit 1: multisampled source.
  // [is_64bpp | msaa<<1]
  VkShaderModule resolve_inpass_shaders_[4] = {};
  // 1x R32_UINT input attachment -> raw R32_FLOAT bits.
  VkShaderModule resolve_inpass_r32_uint_shaders_[1] = {};
  VkShaderModule resolve_inpass_vertex_shader_ = VK_NULL_HANDLE;
  // Bits 0-31: RenderPassKey, 32-33: color slot, 34: 64bpp dest,
  std::unordered_map<uint64_t, VkPipeline> resolve_inpass_pipelines_;

  VkPipelineLayout dump_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout dump_pipeline_layout_depth_ = VK_NULL_HANDLE;
  // Compute pipelines for copying host render target contents to the EDRAM
  // buffer. VK_NULL_HANDLE if failed to create.
  std::unordered_map<DumpPipelineKey, VkPipeline, DumpPipelineKey::Hasher>
      dump_pipelines_;

  // Temporary storage for Resolve.
  std::vector<Transfer> clear_transfers_[2];

  // log_resolve_details aggregation, reported once per second on frame end.
  struct ResolveDetailsBucket {
    uint64_t key;
    uint32_t count;
    uint64_t bytes;
  };
  struct ResolveDetailsStats {
    static constexpr size_t kMaxBuckets = 48;
    ResolveDetailsBucket buckets[kMaxBuckets] = {};
    size_t bucket_count = 0;
    uint32_t frames = 0;
    uint32_t copies = 0;
    uint32_t clears_only = 0;
    uint64_t total_bytes = 0;
    uint32_t dest_distinct = 0;
    uint32_t dest_repeats_in_frame = 0;
    uint32_t dest_repeats_cross_frame = 0;
    uint64_t last_report_ns = 0;
  };
  ResolveDetailsStats resolve_details_stats_;
  std::vector<uint32_t> resolve_dests_current_frame_;
  std::vector<uint32_t> resolve_dests_previous_frame_;
  void RecordResolveDetails(const draw_util::ResolveInfo& resolve_info,
                            bool copied, bool direct_host);

  // At most one depth owner is pending, and only until a non-clear operation.
  RenderTarget* pending_clear_render_target_ = nullptr;
  Transfer::Rectangle pending_clear_cutout_ = {};
  std::vector<Transfer> pending_clear_transfers_;
  // rt_cache_defer_retarget_clears: a retargeted depth clear and the transfers
  // around its rectangle, waiting for the guest pass of the next draw on that
  // render target (on a tiler, a pass of their own stores the target and the
  // guest pass loads it right back).
  RenderTarget* deferred_clear_render_target_ = nullptr;
  uint64_t deferred_clear_value_ = 0;
  Transfer::Rectangle deferred_clear_rectangle_;
  std::vector<Transfer> deferred_clear_transfers_;
  // Update decided the clear runs inside the guest pass about to begin.
  bool deferred_clear_in_pass_ = false;
  bool CanKeepDeferredClear() const;
  // vulkan_lazy_constant_rows: rectangles (host pixels) of 1x depth render
  // targets that hold a clear value only logically - the image is stale
  // there. Every reader materializes them first; a guest pass on the target
  // re-records them as lazy clears and keeps what it doesn't draw.
  struct PendingConstantRows {
    RenderTarget* render_target;
    uint64_t clear_value;
    std::vector<VkRect2D> rects;
  };
  std::vector<PendingConstantRows> pending_constant_rows_;
  struct LazyClearOwner {
    RenderTarget* render_target = nullptr;
    uint64_t clear_value = 0;
  };
  std::array<LazyClearOwner, 1 + xenos::kMaxColorRenderTargets>
      lazy_clear_owners_ = {};
  struct LazyRowsStats {
    uint64_t lazy_clears = 0, kept_rows_rects = 0, kept_pixels = 0;
    uint64_t materialized[4] = {};
  } lazy_rows_stats_;
  bool LazyConstantRowsEligible(const RenderTarget* render_target) const;
  PendingConstantRows* FindPendingConstantRows(const RenderTarget* render_target);
  bool HasPendingConstantRows(const RenderTarget* render_target) const;
  // Records a clear that doesn't widen the render area (slot 0, depth).
  void EmitLazyClear(RenderTarget* render_target, uint64_t clear_value,
                     const VkRect2D& rect);
  // Out of a pass: clears the pending rectangles for real. reason: 0 transfer
  // destination, 1 transfer source, 2 resolve/dump, 3 external.
  void MaterializePendingConstantRows(RenderTarget* render_target,
                                      uint32_t reason);
  // Before a resolve or dump reads the rectangles' render targets.
  void MaterializePendingConstantRowsForDump(
      uint32_t base, uint32_t pitch,
      const std::vector<ResolveCopyDumpRectangle>& rectangles);
  static bool TryMergeClearRectangles(const Transfer::Rectangle& a,
                                      const Transfer::Rectangle& b,
                                      Transfer::Rectangle& merged);

  // Temporary storage for PerformTransfersAndResolveClears.
  std::vector<TransferInvocation> current_transfer_invocations_;

  // Transfers queued by Update() to be encoded inside the active guest render
  // pass instead of breaking the pass. Index 0 is depth, 1...4 are color.
  std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_render_targets_ = {};
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
      pending_draw_pass_transfers_;
  uint32_t pending_draw_pass_transfer_mask_ = 0;
  uint32_t pending_draw_pass_full_overwrite_mask_ = 0;
  // rt_cache_draw_overwrite_cutouts: render targets (of the in-pass and the
  // out-of-pass queues) whose transfers may skip pending_draw_cutout_ if the
  // draw executes.
  uint32_t pending_draw_cutout_mask_ = 0;
  // rt_cache_draw_clear_chains: transfers into render targets that consecutive
  // clear draws (exact screen-aligned rectangles writing every bit) overwrite,
  // deferred until the chain ends, then done only around the union of the
  // rectangles the draws really wrote.
  // One chain per binding slot (0 - depth, 1... - color), each with its own
  // render target, deferred transfers and union of written rectangles; a
  // retargeted clear of the slot's render target (rt_cache_retarget_*) also
  // extends it.
  // Per slot and aspect (0 - depth or color, 1 - stencil of the depth slot):
  // the union of the rectangles known to be written, and the union with the
  // latest draw's until it's confirmed (awaiting_mask, bit 2 * slot + aspect).
  struct DrawClearChain {
    static constexpr uint32_t kSlots = 1 + xenos::kMaxColorRenderTargets;
    uint32_t mask = 0;
    std::array<RenderTarget*, kSlots> render_targets = {};
    std::array<std::vector<Transfer>, kSlots> transfers;
    std::array<std::array<bool, 2>, kSlots> has_cutout = {};
    std::array<std::array<Transfer::Rectangle, 2>, kSlots> cutout = {};
    uint32_t awaiting_mask = 0;
    std::array<std::array<Transfer::Rectangle, 2>, kSlots> candidate = {};
    std::array<uint32_t, kSlots> draws = {};
  };
  DrawClearChain draw_clear_chain_;
  // Ends the chains of the slots in slot_mask.
  void FlushDrawClearChainSlots(uint32_t slot_mask);
  static bool MergeClearRectangle(bool has_a, const Transfer::Rectangle& a,
                                  const Transfer::Rectangle& b,
                                  Transfer::Rectangle& merged);
  uint32_t draw_cutout_logged_ = 0;
  uint32_t draw_extent_input_rejections_logged_ = 0;
  Transfer::Rectangle pending_draw_cutout_ = {};
  std::array<RenderTarget*, 1 + xenos::kMaxColorRenderTargets>
      pending_out_of_pass_render_targets_ = {};
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
      pending_out_of_pass_transfers_;
  uint32_t pending_out_of_pass_mask_ = 0;
  // Performs the transfers, the render targets in cutout_mask without the
  // pixels of the cutout (sources entirely inside it dropped).
  void PerformTransfersWithCutout(RenderTarget* const* render_targets,
                                  const std::vector<Transfer>* transfers,
                                  uint32_t cutout_mask,
                                  const Transfer::Rectangle& cutout,
                                  bool in_current_render_pass);

  // Temporary storage for DumpRenderTargets.
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;

  // For pixel (fragment) shader interlock.

  VkRenderPass fsi_render_pass_ = VK_NULL_HANDLE;
  Framebuffer fsi_framebuffer_;

  VkPipelineLayout resolve_fsi_clear_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_32bpp_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_64bpp_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_
