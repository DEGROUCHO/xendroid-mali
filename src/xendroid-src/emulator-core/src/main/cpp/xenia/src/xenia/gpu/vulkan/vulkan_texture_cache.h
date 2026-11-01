/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/hash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/ui/vulkan/vulkan_descriptor_pool_chain.h"
#include "xenia/ui/vulkan/vulkan_mem_alloc.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

struct PassByteCatalog;

class VulkanTextureCache final : public TextureCache {
 public:
  struct ResolveTextureCopy {
    struct Target {
      Texture* texture;
      VkBufferImageCopy region;
      bool publish;
    };
    VkDeviceSize size = 0;
    // Host bits for RB_COPY_DEST_INFO of the resolve filling the buffer.
    uint32_t linear_dest_flags = 0;
    std::vector<Target> targets;
  };
  // The caller holds the shared-memory watch lock from preparation through
  // publication: a concurrent CPU write must not be hidden by publication.
  ResolveTextureCopy PrepareResolveTextureCopy(
      const draw_util::ResolveInfo& info, bool scaled,
      const global_unique_lock_type& lock);
  void CompleteResolveTextureCopy(const ResolveTextureCopy& copy,
                                  VkBuffer buffer,
                                  const draw_util::ResolveInfo& info,
                                  bool scaled,
                                  const global_unique_lock_type& lock);

  // Independent R32 images retain depth until the synchronous provider runs.
  struct DepthSurfaceTarget {
    Texture* texture;
    VkFramebuffer framebuffer;
    uint32_t width, height;
  };
  std::vector<DepthSurfaceTarget> PrepareDepthSurfaceTargets(
      const draw_util::ResolveInfo& info, VkRenderPass render_pass,
      const global_unique_lock_type& lock);
  VkImageView PrepareDepthSurfaceSource(const DepthSurfaceTarget& target);
  VkImageView GetDepthSurfaceStencilSource(const DepthSurfaceTarget& target);
  void BeginDepthSurfaceTarget(const DepthSurfaceTarget& target);
  void CompleteDepthSurfaceTarget(const DepthSurfaceTarget& target);
  void PublishDepthSurfaceTarget(const DepthSurfaceTarget& target,
                                const global_unique_lock_type& lock);

  // Diagnostic (vulkan_debug_texture_dump): writes the recorded texture loads
  // (guest bytes at load time and now, the shared memory copy the load shader
  // read, and the decoded load output) to `dir`. The GPU must be idle.
  void DebugWriteTextureDumps(const std::filesystem::path& dir);
  bool HasPendingDebugTextureDumps() const {
    return !debug_texture_dumps_written_ && !debug_texture_dumps_.empty();
  }
  // Diagnostic (vulkan_debug_texture_audit): reads back the host images of the
  // textures sampled in the last submissions and the guest bytes they were
  // loaded from. Record needs an open submission, write needs the GPU idle.
  bool DebugTextureAuditPending() const;
  void DebugRecordTextureAudit();
  void DebugWriteTextureAudit(const std::filesystem::path& dir);

 private:
  class VulkanTexture;
  struct DebugTextureDump {
    TextureKey key;
    std::string metadata;
    std::vector<uint8_t> cpu_base_at_load;
    std::vector<uint8_t> cpu_mips_at_load;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    uint32_t readback_memory_type = UINT32_MAX;
    VkDeviceSize readback_memory_size = 0;
    // Readback layout: [decoded scratch][shared base][shared mips].
    VkDeviceSize scratch_size = 0;
    VkDeviceSize shared_base_size = 0;
    VkDeviceSize shared_mips_size = 0;
    uint32_t later_loads = 0;
    std::string later_load_kinds;
    // Distinct raw fetch constants and sampler bindings seen when sampling.
    uint64_t sampler_uses = 0;
    std::vector<std::array<uint32_t, 10>> sampler_variants;
  };
  // Mutable because sampler parameter lookup (const) records sampling info.
  mutable std::vector<DebugTextureDump> debug_texture_dumps_;
  struct DebugTextureAudit {
    std::string metadata;
    size_t guest_offset = 0;
    size_t guest_size = 0;
  };
  std::vector<DebugTextureAudit> debug_texture_audits_;
  std::vector<uint8_t> debug_texture_audit_guest_;
  VkBuffer debug_texture_audit_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory debug_texture_audit_memory_ = VK_NULL_HANDLE;
  uint32_t debug_texture_audit_memory_type_ = UINT32_MAX;
  VkDeviceSize debug_texture_audit_memory_size_ = 0;
  VkDeviceSize debug_texture_audit_used_size_ = 0;
  bool debug_texture_audit_recorded_ = false;
  bool debug_texture_audit_written_ = false;
  VkDeviceSize debug_texture_dump_bytes_ = 0;
  bool debug_texture_dumps_written_ = false;
  // All candidates, including compatible aliases at the same guest base.
  // Removed by the image destructor before it can become a dangling pointer.
  std::unordered_multimap<uint32_t, VulkanTexture*> resolve_textures_;
  uint32_t ResolveTextureBytesPerPixel(const TextureKey& key) const;
  uint32_t ResolveTextureLinearDestFlags(const TextureKey& key) const;
  // Whether the texture's mips are generated on the host from its base level
  // (vulkan_generate_texture_mips) instead of loaded from guest memory.
  bool ShouldGenerateTextureMips(const TextureKey& key) const;
  // texture_skip_top_mip_min_size: the host image starts at guest mip 1.
  bool SkipsTopMip(const TextureKey& key) const;
  mutable std::unordered_map<VkFormat, bool> mip_generation_format_support_;

 public:
  // Sampler parameters that can be directly converted to a host sampler or used
  // for checking whether samplers bindings are up to date.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Force the border color alpha to 1.0 (only meaningful with a border
      // clamp mode).
      uint32_t force_bc_w_to_max : 1;  // 23
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // can be used to limit fetching to mip_min_level.
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const {
      return value == parameters.value;
    }
    bool operator!=(const SamplerParameters& parameters) const {
      return value != parameters.value;
    }
  };

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  static std::unique_ptr<VulkanTextureCache> Create(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages) {
    std::unique_ptr<VulkanTextureCache> texture_cache(new VulkanTextureCache(
        register_file, shared_memory, draw_resolution_scale_x,
        draw_resolution_scale_y, command_processor,
        guest_shader_pipeline_stages));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~VulkanTextureCache();

  void CompletedSubmissionUpdated(uint64_t completed_submission_index) override;
  void BeginSubmission(uint64_t new_submission_index) override;

  // Owner-thread diagnostic; includes textures, auxiliary images and spans.
  void LogMemoryAccounting() const;

  // Must be called within a frame - creates and untiles textures needed by
  // shaders, and enqueues transitioning them into the sampled usage. This may
  // bind compute pipelines (notifying the command processor about that), and
  // also since it may insert deferred barriers, before flushing the barriers
  // preceding host GPU work.
  void RequestTextures(uint32_t used_texture_mask) override;
  // The same, knowing which of the textures the vertex shader samples.
  void RequestTextures(uint32_t used_texture_mask,
                       uint32_t vertex_texture_mask) {
    vertex_texture_mask_ = vertex_texture_mask;
    RequestTextures(used_texture_mask);
    vertex_texture_mask_ = UINT32_MAX;
  }
  // The image view bound where a guest texture is missing, by array type.
  VkImageView GetNullImageView(xenos::FetchOpDimension dimension) const {
    switch (dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        return null_image_view_3d_;
      case xenos::FetchOpDimension::kCube:
        return null_image_view_cube_;
      default:
        return null_image_view_2d_array_;
    }
  }
  // For a draw whose textures are current (RequestTextures skipped): any of
  // them left in another usage since goes back to guest sampling, and the
  // ones the vertex shader samples become visible to the vertex stages.
  void EnsureTexturesSampled(uint32_t used_texture_mask,
                             uint32_t vertex_texture_mask);

  // Diagnostic only, including resident bindings that need no reload.
  bool ResolveDeferTextureRead(uint32_t used_texture_mask, uint32_t address,
                               uint32_t length, bool& unknown) const;

  // Loads the textures queued by GPU-write invalidations (see
  // TextureCache::HasEagerReloads) and transitions them back for guest shader
  // use. Must be called outside a render pass, before the barriers preceding
  // the next one are submitted.
  void FlushEagerResolvedReloads();

  // Layout the active binding's image currently resides in: GENERAL for
  // promoted resolve destinations, SHADER_READ_ONLY_OPTIMAL otherwise (and for
  // the null views). Descriptor writes must match it or sampling is undefined.
  VkImageLayout GetActiveBindingImageLayout(uint32_t fetch_constant_index,
                                            xenos::FetchOpDimension dimension,
                                            bool is_signed) const;
  VkImageView GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                              xenos::FetchOpDimension dimension,
                                              bool is_signed);

  // Descriptor set (kStorageBufferCompute layout) binding the whole shared
  // memory buffer for compute load/store, or VK_NULL_HANDLE if the buffer
  // doesn't fit in maxStorageBufferRange. When valid, the byte offset into the
  // buffer must be supplied via push constants. Shared with resolve.
  VkDescriptorSet shared_memory_persistent_descriptor_set() const {
    return shared_memory_persistent_descriptor_set_;
  }

  // Fetch constants whose host image views have changed since the bits were
  // last reset, for reusing texture descriptor sets across draws when bindings
  // are unchanged. Accumulated whenever UpdateTextureBindingsImpl rewrites
  // bindings.
  uint32_t texture_bindings_changed() const {
    return texture_bindings_changed_;
  }
  void ResetTextureBindingsChanged(uint32_t mask) {
    texture_bindings_changed_ &= ~mask;
  }

  SamplerParameters GetSamplerParameters(
      const VulkanShader::SamplerBinding& binding) const;

  // Must be called for every used sampler at least once in a single submission,
  // and a submission must be open for this to be callable.
  // Returns:
  // - The sampler, if obtained successfully - and increases its last usage
  //   submission index - and has_overflown_out = false.
  // - VK_NULL_HANDLE and has_overflown_out = true if there's a total sampler
  //   count overflow in a submission that potentially hasn't completed yet.
  // - VK_NULL_HANDLE and has_overflown_out = false in case of a general failure
  //   to create a sampler.
  VkSampler UseSampler(SamplerParameters parameters, bool& has_overflown_out);
  // Returns the submission index to await (may be the current submission in
  // case of an overflow within a single submission - in this case, it must be
  // ended, and a new one must be started) in case of sampler count overflow, so
  // samplers may be freed, and UseSamplers may take their slots.
  uint64_t GetSubmissionToAwaitOnSamplerOverflow(
      uint32_t overflowed_sampler_count) const;

  // Incremented whenever any VkSampler is destroyed (LRU eviction in
  // UseSampler, cache teardown). Lets the command processor's cross-draw
  // sampler cache detect that cached handles may no longer be valid.
  uint64_t sampler_destroy_generation() const {
    return sampler_destroy_generation_;
  }

  // Incremented whenever a texture's VkImageViews are destroyed. Image view
  // handle values are reused like sampler ones, so the command processor's
  // texture descriptor set reuse must not match a cached set across this.
  uint64_t image_view_destroy_generation() const {
    return image_view_destroy_generation_;
  }

  // Returns the 2D view of the front buffer texture (for fragment shader
  // reading - the barrier will be pushed in the command processor if needed),
  // or VK_NULL_HANDLE in case of failure. May call LoadTextureData.
  VkImageView RequestSwapTexture(uint32_t& width_scaled_out,
                                 uint32_t& height_scaled_out,
                                 xenos::TextureFormat& format_out);

  // Scaled resolve buffer management (for use by VulkanRenderTargetCache)
  // Span of the scaled resolve address space (when sparse binding is
  // unavailable)
  struct ScaledResolveBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint64_t size = 0;
    uint64_t range_start_scaled = 0;
    uint64_t range_length_scaled = 0;
  };

  // Sparse buffer wrapper for overlapping 2GB windows
  class ScaledResolveSparseBuffer {
   public:
    explicit ScaledResolveSparseBuffer(VkBuffer buffer) : buffer_(buffer) {}

    VkBuffer buffer() const { return buffer_; }

   private:
    VkBuffer buffer_ = VK_NULL_HANDLE;
  };

  // Constants for sparse scaled resolve
  static constexpr uint32_t kScaledResolveHeapSizeLog2 = 24;  // 16MB heaps
  static constexpr uint32_t kScaledResolveHeapSize =
      uint32_t(1) << kScaledResolveHeapSizeLog2;
  static constexpr uint64_t kScaledResolveSparseBufferSize =
      uint64_t(2) << 30;  // 2GB per buffer

  // Public scaled resolve buffer methods for use by VulkanRenderTargetCache
  bool EnsureScaledResolveMemoryCommittedPublic(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) {
    return EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                              length_scaled_alignment_log2);
  }

  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                     uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);

  void DescribePassBytes(PassByteCatalog& catalog) const;

  VkBuffer GetCurrentScaledResolveBuffer() const;

  // Returns the base scaled address that the current buffer starts at.
  // For sparse buffers: buffer N starts at N GB (N << 30)
  // For simple buffers: returns the buffer's range_start_scaled
  uint64_t GetCurrentScaledResolveBufferBaseOffset() const {
    if (sparse_scaled_resolve_supported_) {
      return uint64_t(scaled_resolve_current_buffer_index_) << 30;
    }
    if (scaled_resolve_current_buffer_index_ < scaled_resolve_buffers_.size()) {
      return scaled_resolve_buffers_[scaled_resolve_current_buffer_index_]
          .range_start_scaled;
    }
    return 0;
  }

  size_t GetScaledResolveCurrentBufferIndex() const {
    return scaled_resolve_current_buffer_index_;
  }

  const ScaledResolveBuffer* GetScaledResolveBufferInfo(size_t index) const {
    if (index < scaled_resolve_buffers_.size()) {
      return &scaled_resolve_buffers_[index];
    }
    return nullptr;
  }

  // Get the current scaled resolve range (set by MakeScaledResolveRangeCurrent)
  uint64_t GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  uint64_t GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }

 protected:
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  void LogInPassTextureLoad(const VulkanTexture& texture, bool load_base,
                            bool load_mips);
  void OnTextureDataRequest(Texture& texture, bool base_outdated,
                            bool mips_outdated) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  bool EnsureScaledResolveMemoryCommitted(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) override;

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    // Do NOT add integer formats to this - they are not filterable, can only be
    // read with ImageFetch, not ImageSample! Games that fetch fixed-point
    // formats are handled after sampling by scaling the normalized host value
    // back to the guest integer range (see GetIntegerScaleBits). Keep these as
    // sampled float/normalized views.
    VkFormat format;
    // Whether the format is block-compressed on the host (the host block size
    // matches the guest format block size in this case), and isn't decompressed
    // on load.
    bool block_compressed;

    // Set up dynamically based on what's supported by the device.
    bool linear_filterable;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Mapping of Xenos swizzle components to Vulkan format components.
    uint32_t swizzle;
    // Whether the unsigned and the signed formats are compatible for one image
    // and the same image data (on a portability subset device, this should also
    // take imageViewFormatReinterpretation into account).
    bool unsigned_signed_compatible;
  };

  class VulkanTexture final : public Texture {
   public:
    enum class Usage {
      kUndefined,
      kTransferDestination,
      kGuestShaderSampled,
      kSwapSampled,
      kDepthSurfaceAttachment,
      kDepthSurfaceComputeRead,
    };

   public:
    // In kGuestShaderSampled: whether the barrier into it also covered the
    // guest vertex stages (vulkan_pass_head_overlap leaves them out for
    // textures only pixel shaders sample).
    bool vertex_visible = true;
    bool resolve_copy_pending_sample = false;
    // log_resolve_texture_copy_rejects: what the resolves at this texture's
    // base did since its last load (0 none, 1 published, 2 published but the
    // range was not valid, 3 copied without publishing, 0x80 | reason
    // rejected) and how many there were.
    uint8_t resolve_copy_last = 0;
    uint8_t resolve_copy_count = 0;
    bool depth_surface_renderable = false;
    // vulkan_color_surface_resolve: a renderable k_8_8_8_8 image (RGBA8 view,
    // no stencil image) instead of the R32 depth one.
    bool surface_is_color = false;
    bool depth_surface_pending_sample = false;
    VkImageView depth_surface_attachment_view = VK_NULL_HANDLE;
    // The guest stencil next to the R32 depth, so the provider can rebuild
    // the exact 24.8 words without the source render target.
    VkImage depth_surface_stencil_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_surface_stencil_memory = VK_NULL_HANDLE;
    VkImageView depth_surface_stencil_view = VK_NULL_HANDLE;
    VkFramebuffer depth_surface_framebuffer = VK_NULL_HANDLE;
    bool EnsureDepthSurfaceFramebuffer(VkRenderPass render_pass);
    // Takes ownership of the image and its memory.
    // track_usage: if false, texture won't participate in LRU cache eviction.
    explicit VulkanTexture(VulkanTextureCache& texture_cache,
                           const TextureKey& key, VkImage image,
                           VmaAllocation allocation, bool track_usage = true);
    ~VulkanTexture();

    VkImage image() const { return image_; }
    const VulkanTexture* wrapper_3d_as_2d_for_debug() const {
      return texture_3d_as_2d_.get();
    }

    Usage usage() const { return usage_; }
    // vulkan_debug_texture_audit bookkeeping.
    uint64_t debug_source_hash = 0;
    bool debug_source_hashed = false;
    // log_inpass_texture_loads: hash of the base (or mips) source at the last
    // load, to count reloads whose source bytes did not change.
    mutable uint64_t inpass_log_source_hash = 0;
    uint32_t debug_loads = 0;
    uint64_t debug_last_load_submission = 0;
    // Doesn't transition (the caller must insert the barrier).
    Usage SetUsage(Usage new_usage) {
      Usage old_usage = usage_;
      usage_ = new_usage;
      return old_usage;
    }

    VkImageView GetView(bool is_signed, uint32_t host_swizzle,
                        bool is_array = true);

    // For 3D textures sampled as 2D - creates a 2D copy of slice 0.
    VkImageView GetOrCreate3DAs2DImageView(bool is_signed,
                                           uint32_t host_swizzle);

   private:
    union ViewKey {
      uint32_t key;
      struct {
        uint32_t is_signed_separate_view : 1;
        uint32_t host_swizzle : 12;
        uint32_t is_array : 1;
      };

      ViewKey() : key(0) { static_assert_size(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const ViewKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const ViewKey& other_key) const {
        return key == other_key.key;
      }
      bool operator!=(const ViewKey& other_key) const {
        return !(*this == other_key);
      }
    };

    static constexpr VkComponentSwizzle GetComponentSwizzle(
        uint32_t texture_swizzle, uint32_t component_index) {
      xenos::XE_GPU_TEXTURE_SWIZZLE texture_component_swizzle =
          xenos::XE_GPU_TEXTURE_SWIZZLE(
              (texture_swizzle >> (3 * component_index)) & 0b111);
      if (texture_component_swizzle ==
          xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
        // The portability subset requires all swizzles to be IDENTITY, return
        // IDENTITY specifically, not R, G, B, A.
        return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
      switch (texture_component_swizzle) {
        case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
          return VK_COMPONENT_SWIZZLE_R;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
          return VK_COMPONENT_SWIZZLE_G;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
          return VK_COMPONENT_SWIZZLE_B;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
          return VK_COMPONENT_SWIZZLE_A;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
          return VK_COMPONENT_SWIZZLE_ZERO;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
          return VK_COMPONENT_SWIZZLE_ONE;
        default:
          // An invalid value.
          return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
    }

    VkImage image_;
    VmaAllocation allocation_;

    Usage usage_ = Usage::kUndefined;

    std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views_;
    struct ViewLookup {
      uint32_t key = UINT32_MAX;
      VkImageView view = VK_NULL_HANDLE;
    };
    std::array<ViewLookup, 8> view_lookup_{};

    // For 3D textures sampled as 2D - cached 2D texture loaded from slice 0.
    // Uses a modified key (depth=1) with 3D tiling to read from guest memory.
    std::unique_ptr<VulkanTexture> texture_3d_as_2d_;
    VkImageView image_view_3d_as_2d_unsigned_ = VK_NULL_HANDLE;
    VkImageView image_view_3d_as_2d_signed_ = VK_NULL_HANDLE;
  };

  struct VulkanTextureBinding {
    VkImageView image_view_unsigned;
    VkImageView image_view_signed;

    VulkanTextureBinding() { Reset(); }

    void Reset() {
      image_view_unsigned = VK_NULL_HANDLE;
      image_view_signed = VK_NULL_HANDLE;
    }
  };

  struct Sampler {
    VkSampler sampler;
    uint64_t last_usage_submission;
    std::pair<const SamplerParameters, Sampler>* used_previous;
    std::pair<const SamplerParameters, Sampler>* used_next;
  };

  uint64_t sampler_destroy_generation_ = 0;
  uint64_t image_view_destroy_generation_ = 0;

  static constexpr bool AreDimensionsCompatible(
      xenos::FetchOpDimension binding_dimension,
      xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  explicit VulkanTextureCache(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages);

  bool Initialize();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  void GetTextureUsageMasks(VulkanTexture::Usage usage,
                            VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask, VkImageLayout& layout);
  // Orders transfer writes before guest sampling, for the vertex stages too
  // only if vertex_stages.
  void TransitionTextureForGuestShader(VulkanTexture& texture,
                                       bool vertex_stages);
  // The stages a write must reach before guest sampling.
  VkPipelineStageFlags GetGuestSampledStages(bool vertex_stages) const;
  // RequestTextures: the textures the vertex shader samples (all if unknown).
  uint32_t vertex_texture_mask_ = UINT32_MAX;
  std::vector<Texture*> eager_reload_scratch_;

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  // Sparse scaled resolve helper functions
  bool InitializeSparseScaledResolve();
  void ShutdownSparseScaledResolve();
  size_t GetScaledResolveSparseBufferCount() const;
  std::array<size_t, 2> GetPossibleScaledResolveBufferIndices(
      uint64_t address_scaled) const;
  bool EnsureScaledResolveMemoryCommittedSparse(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2);
  bool MakeScaledResolveRangeCurrentSparse(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2);
  void BindHeapToOverlappingBuffers(uint32_t heap_index, VkDeviceMemory heap);

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Using the Vulkan Memory Allocator because texture count in games is
  // naturally pretty much unbounded, while Vulkan implementations, especially
  // on Windows versions before 10, may have an allocation count limit as low as
  // 4096.
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  HostFormatPair host_formats_[64];

  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_scaled_{};

  // Persistent descriptor binding the whole shared memory buffer
  // (kStorageBufferCompute layout) for compute load/store, so per-operation
  // transient descriptors don't need to be allocated and written. Only created
  // when the buffer fits in maxStorageBufferRange; the byte offset into the
  // buffer is passed via push constants instead. Used as the source of texture
  // loads here, and shared as the destination of resolves in the render target
  // cache.
  VkDescriptorPool shared_memory_persistent_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_persistent_descriptor_set_ = VK_NULL_HANDLE;

  // Accumulated mask of fetch constants whose host image views were rewritten,
  // consumed by the command processor to decide texture descriptor set reuse.
  uint32_t texture_bindings_changed_ = 0;
  struct SamplerParameterMemo {
    uint32_t binding = UINT32_MAX;
    xenos::xe_gpu_texture_fetch_t fetch{};
    int32_t aniso_override = 0, aniso_cap = 0;
    uint32_t options = 0;
    SamplerParameters parameters;
  };
  mutable std::array<SamplerParameterMemo, 512> sampler_parameter_memos_{};

  // If both images can be placed in the same allocation, it's one allocation,
  // otherwise it's two separate.
  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;
  bool null_images_cleared_ = false;
  // Set by OnTextureDataRequest for the load that follows it: the texture is
  // not bound in the current render pass and no command of the pass touched
  // its guest memory, so the load may be recorded at the head of the pass.
  bool pass_head_load_allowed_ = false;
  std::array<uint32_t, 8> pass_head_verdicts_ = {};
  uint32_t pass_head_verdict_requests_ = 0;
  std::pair<uint32_t, uint32_t> pass_head_byte_ranges_[2] = {};
  uint32_t pass_head_byte_range_count_ = 0;
  uint32_t pass_head_loads_total_ = 0;
  uint64_t pass_head_loads_ = 0;
  std::array<VulkanTextureBinding, xenos::kTextureFetchConstantCount>
      vulkan_texture_bindings_;

  uint32_t sampler_max_count_;

  xenos::AnisoFilter max_anisotropy_;

  std::unordered_map<SamplerParameters, Sampler, SamplerParameters::Hasher>
      samplers_;
  struct SamplerLookup {
    uint64_t generation = UINT64_MAX;
    SamplerParameters parameters;
    std::pair<const SamplerParameters, Sampler>* node = nullptr;
  };
  std::array<SamplerLookup, 256> sampler_lookup_{};
  std::pair<const SamplerParameters, Sampler>* sampler_used_first_ = nullptr;
  std::pair<const SamplerParameters, Sampler>* sampler_used_last_ = nullptr;

  // Scaled resolve storage without sparse binding: disjoint spans sorted by
  // range_start_scaled, each a whole number of kScaledResolveHeapSize granules
  // backed by its own allocation, so every scaled address lives in exactly one
  // buffer. A range crossing spans merges them (GPU copy into a new span).
  std::vector<ScaledResolveBuffer> scaled_resolve_buffers_;
  // Merged-away spans, destroyed once the submission that last could
  // reference them has completed.
  std::vector<std::pair<uint64_t, ScaledResolveBuffer>>
      scaled_resolve_retired_buffers_;
  uint64_t scaled_resolve_span_bytes_ = 0;
  bool EnsureScaledResolveSpan(uint64_t start_scaled, uint64_t end_scaled);
  void ReleaseRetiredScaledResolveBuffers(uint64_t completed_submission);
  // Current scaled resolve range tracking
  uint64_t scaled_resolve_current_range_start_scaled_ = 0;
  uint64_t scaled_resolve_current_range_length_scaled_ = 0;
  size_t scaled_resolve_current_buffer_index_ = SIZE_MAX;

  // Sparse scaled resolve (overlapping 2GB windows)
  bool sparse_scaled_resolve_supported_ = false;
  // 2GB overlapping sparse buffers - buffer N covers [N GB ... (N+2) GB)
  // For 3x3 scale (4.5GB), need 4 buffers: 0:[0-2GB), 1:[1-3GB), 2:[2-4GB),
  // 3:[3-4.5GB)
  static constexpr size_t kMaxScaledResolveSparseBuffers =
      (uint64_t(SharedMemory::kBufferSize) * kMaxDrawResolutionScaleAlongAxis *
           kMaxDrawResolutionScaleAlongAxis -
       1) >>
      30;
  std::array<std::unique_ptr<ScaledResolveSparseBuffer>,
             kMaxScaledResolveSparseBuffers>
      scaled_resolve_sparse_buffers_;
  // 16MB heaps that can be mapped to multiple buffer regions
  std::vector<VkDeviceMemory> scaled_resolve_heaps_;
  uint32_t scaled_resolve_heap_count_ = 0;
  // Memory type for sparse allocations
  uint32_t scaled_resolve_memory_type_ = UINT32_MAX;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_
