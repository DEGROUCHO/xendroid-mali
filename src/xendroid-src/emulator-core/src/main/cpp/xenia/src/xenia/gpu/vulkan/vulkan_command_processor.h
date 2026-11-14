/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
#define XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/frame_interval_histogram.h"
#include "xenia/base/hash.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_float_constants.h"
#include "xenia/gpu/draw_memory_range.h"
#include "xenia/gpu/draw_sequence_census.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/gpu_frame_time.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_primitive_processor.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/vulkan/linked_type_descriptor_set_allocator.h"
#include "xenia/ui/vulkan/vulkan_descriptor_pool_chain.h"
#include "xenia/ui/vulkan/vulkan_gpu_completion_timeline.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

DECLARE_bool(log_gpu_frame_time_breakdown);

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor final : public CommandProcessor {
  friend class VulkanRenderTargetCache;
  friend class VulkanSharedMemory;
  friend class VulkanTextureCache;
 protected:
#define OVERRIDING_BASE_CMDPROCESSOR
#include "../pm4_command_processor_declare.h"
#undef OVERRIDING_BASE_CMDPROCESSOR
 public:
  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBufferCompute,
    kStorageBufferFragment,
    // Uniform buffer at binding 1 for shaders that keep binding 0 as push
    // constants for D3D-style root constants.
    kUniformBufferComputeB1,
    // Storage image at binding 0 for the resolve-to-texture fragment variant.
    kStorageImageFragment,
    kStorageImageCompute,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor,
                                      VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask,
                                      VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) =
        delete;
    ScratchBufferAcquisition& operator=(
        const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(
        ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      // Release what this one holds first, or the scratch buffer stays marked
      // used and the next acquisition fails (a draw retried in its loop).
      Release();
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() { Release(); }

    void Release() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
        buffer_ = VK_NULL_HANDLE;
        command_processor_ = nullptr;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                         kernel::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;

  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr) override;

  void RestoreEdramSnapshot(const void* snapshot) override;
  bool CaptureRuntimeSnapshot(snapshot::GpuState& out) override;
  bool RestoreRuntimeSnapshot(const snapshot::GpuState& state) override;
  bool RuntimeSnapshotHealthy() const override;
  bool RuntimeSnapshotSupported() const override;

  void PollCompletedSubmission() override;
  void ServiceMemoryRequests() override;
  bool HasMemoryProviders() const override;
  uint64_t depth_materialization_epoch_ = 0;

  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const ui::vulkan::VulkanProvider*>(
               graphics_system_->provider())
        ->vulkan_device();
  }
  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return *deferred_command_buffer_;
  }

  // Deferred commands replayed at the head of the submission's command
  // buffer, before deferred_command_buffer() - holds shared-memory uploads
  // hoisted out of render passes.
  DeferredCommandBuffer& deferred_setup_command_buffer() {
    assert_true(submission_open_);
    return deferred_setup_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  // The open render pass's pass-head slot (see
  // DeferredCommandBuffer::CmdPassHeadSlot), or null.
  DeferredCommandBuffer* pass_head_command_buffer() {
    return (in_render_pass_ && pass_head_slot_ != UINT32_MAX)
               ? &deferred_command_buffer_->GetPassHeadSlot(pass_head_slot_)
               : nullptr;
  }
  bool in_render_pass() const { return in_render_pass_; }
  // Source stages of the barrier before a pass head or the hoisted uploads
  // (see vulkan_pass_head_overlap).
  VkPipelineStageFlags GetPassHeadSourceStages() const;
  // Whether a pixel shader that reads or writes the shared memory may still
  // run (see fragment_shared_memory_submission_).
  bool FragmentSharedMemoryPending() const {
    return fragment_shared_memory_submission_ > GetCompletedSubmission();
  }
  uint64_t GetCurrentSubmission() const {
    return completion_timeline_.GetUpcomingSubmission();
  }
  uint64_t GetCompletedSubmission() const override {
    return completion_timeline_.GetCompletedSubmissionFromLastUpdate();
  }
  bool IsRenderTargetFramebufferInUse(
      const VulkanRenderTargetCache::Framebuffer* framebuffer) const;
  void ForgetRenderTargetFramebuffer(
      const VulkanRenderTargetCache::Framebuffer* framebuffer);
  void InvalidateCachedRenderTargetDescriptorSet(
      VkDescriptorSet descriptor_set);

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
                        const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }
  // RTTRACE one-shot diagnostic (cvar vulkan_rt_trace_request): whether the
  // current guest frame is being traced. When the cvar is empty this never
  // becomes true, so hot paths pay only for this bool read.
  bool IsRtTraceActive() const { return rt_trace_active_; }
  // vulkan_bindless_textures: the slots of destroyed views / samplers become
  // reusable once the current submission completes.
  bool bindless_textures() const { return bindless_set_ != VK_NULL_HANDLE; }
  void ReleaseBindlessImageView(VkImageView image_view);
  void ReleaseBindlessSampler(VkSampler sampler);

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(
      VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true);
  bool PushImageMemoryBarrier(
      VkImage image, const VkImageSubresourceRange& subresource_range,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      VkImageLayout old_layout, VkImageLayout new_layout,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true);
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass);

  // If [base_bytes, base_bytes + size_bytes) holds memexport output (which
  // under two-buffer routing lives in the host-imported buffer aliasing guest
  // RAM), copies it into the device-local buffer so a following device-buffer
  // read - a texture load - sees it. No-op for non-memexport ranges (already in
  // the device buffer) and when the host buffer is unavailable. Called by the
  // texture cache before loading a texture. Ends the render pass.
  // Returns whether it copied, so an upload of the same range can be skipped.
  bool EnsureMemexportRangeInDeviceBuffer(uint32_t base_bytes,
                                          uint32_t size_bytes);

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  // transfer_dest_view is the destination render target view for the transfer.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth,
      uint32_t msrtss_samples = 0, bool transfer_dest_is_integer = false);
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass(const char* byte_reason = "other");

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(
        transient_descriptor_layout)];
  }
  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex,
                                                      size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(
      size_t texture_count_pixel, size_t sampler_count_pixel,
      size_t texture_count_vertex, size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  // push_barrier false: the caller orders the previous usage itself (a load
  // recorded at the head of a render pass, where a pending barrier would end
  // the pass).
  ScratchBufferAcquisition AcquireScratchGpuBuffer(
      VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
      VkAccessFlags initial_access_mask, bool push_barrier = true);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline,
                                    bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  // Diagnostic (log_gpu_frame_time_breakdown): brackets commands recorded into
  // command_buffer with GPU timestamps, reported per kind in the frame stats
  // (1 = texture load). Returns UINT32_MAX when not timing.
  uint32_t BeginTimedSection(DeferredCommandBuffer& command_buffer,
                             uint8_t kind,
                             uint32_t texture_format = UINT32_MAX);
  void RecordResolveTextureHit(uint32_t format) {
    ++vk_frame_sync_stats_.resolve_texture[format & 63].avoided;
  }
  void EndTimedSection(DeferredCommandBuffer& command_buffer, uint32_t pair);
  // Like BindExternalComputePipeline, but into another command buffer replayed
  // before every command recorded since (a render pass head slot) - the
  // tracking stays valid because no compute work is recorded inside a pass.
  void BindExternalComputePipelineInto(DeferredCommandBuffer& command_buffer,
                                       VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  std::string GetTitleStateSuffix() const override;

  // Debug marker methods - public so subsystems can annotate their operations.
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  XE_FORCEINLINE
  void WriteRegister(uint32_t index, uint32_t value) override;
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers) override;
  void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                  uint32_t num_registers) override;

  // Family-segmented writes with common RegisterFile dependency tracking.
  void FetchWritten(uint32_t slot);
  void WriteShaderConstantsFromMem(uint32_t start_index, uint32_t* base,
                                   uint32_t num_registers);
  void WriteBoolLoopFromMem(uint32_t start_index, uint32_t* base,
                            uint32_t num_registers);
  void WriteFetchFromMem(uint32_t start_index, uint32_t* base,
                         uint32_t num_registers);
  void WritePossiblySpecialRegistersFromMem(uint32_t start_index,
                                            uint32_t* base,
                                            uint32_t num_registers);
  XE_NOINLINE
  void WriteRegisterRangeFromRing_WraparoundCase(xe::RingBuffer* ring,
                                                 uint32_t base,
                                                 uint32_t num_registers);

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;
  void PrepareForPause() override;
  void BeforeCpuMemoryWrite(uint32_t start, uint32_t length) override;
  bool DeferGuestFenceWrite(uint8_t* destination, uint32_t value) override;

  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  template <bool census = false>
  bool IssueDrawImpl(xenos::PrimitiveType prim_type, uint32_t index_count,
                     IndexBufferInfo* index_buffer_info,
                     bool major_mode_explicit);
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;
  bool IssueCopyImpl(const draw_util::ResolveInfo* captured_info = nullptr);
  enum DrawCensusBreak {
    kCensusShader,
    kCensusRt,
    kCensusRaster,
    kCensusTextures,
    kCensusFetch,
    kCensusIndex,
    kCensusBarrier,
    kCensusExcluded,
    kCensusBreakCount
  };
  bool IssueDrawCensus(xenos::PrimitiveType prim_type, uint32_t index_count,
                      IndexBufferInfo* index_buffer_info,
                      bool major_mode_explicit, uint32_t packet,
                      uint32_t viz_query_condition);
  void DrawCensusClose(DrawCensusBreak reason);
  void DrawCensusCloseStrictRun(DrawCensusBreak reason);
  void DrawCensusRecord(bool fast, bool eligible,
                        const void* vertex_translation,
                        const void* pixel_translation,
                        const PrimitiveProcessor::ProcessingResult& shape);
  void DrawCensusReport();
  // State candidates only: no resource memory versions are captured.
  struct DrawCensusState {
    // Compare values, including direct PM4 writes, rather than dirty bits.
    std::array<uint32_t, RegisterFile::kRegisterCount> registers = {};
    bool have_registers = false;
    bool recorded = false;
    bool query = false;
    uint32_t changes = 0;
    uint32_t consts_changed = 0;
    bool operands_changed = false;
    xenos::PrimitiveType primitive_type = {};
    xenos::PrimitiveType previous_primitive_type = {};
    uint32_t index_count = 0, previous_index_count = 0;
    uint32_t index_base = 0, previous_index_base = 0;
    uint32_t index_offset = 0, previous_index_offset = 0;
    bool major_mode_explicit = false, previous_major_mode_explicit = false;
    uint64_t length = 0;
    const void* vertex_shader = nullptr;
    const void* pixel_shader = nullptr;
    const void* vertex_translation = nullptr;
    const void* pixel_translation = nullptr;
    const std::atomic<VkPipeline>* pipeline = nullptr;
    VkPipeline pipeline_handle = VK_NULL_HANDLE;
    const void* layout = nullptr;
    VkDescriptorSet textures[2] = {};
    const VulkanRenderTargetCache::Framebuffer* framebuffer = nullptr;
    PrimitiveProcessor::ProcessingResult shape = {};
    DrawSequenceCensus mdi, texture_relaxed, instancing;
    uint64_t epoch = 0, submission = 0, pass = 0;
    uint64_t sampler_generation = 0, image_generation = 0;
    uint64_t report_ns = 0;
    struct Totals {
      uint64_t draws = 0, fast = 0, eligible = 0, excluded = 0;
      uint64_t grouped = 0, groups = 0, consts_changed = 0, consts_only = 0;
      uint64_t histogram[5] = {}, breaks[kCensusBreakCount] = {};
      uint64_t unique_index_ranges = 0, index_range_overflow = 0;
      uint64_t vertex_fetch_edges = 0, operand_edges = 0;
      uint64_t texture_change_edges = 0, texture_only_breaks = 0;
      uint64_t frames = 0;
    } totals;
    // Exact (base, byte length) set; saturation is explicit in the log.
    std::array<uint64_t, 4096> index_ranges = {};
    std::array<bool, 4096> index_ranges_used = {};
  } draw_census_;
  // log_draw_state_repeats bookkeeping.
  void CountRegisterChanges(uint32_t index, const uint32_t* src_guest_endian,
                            uint32_t count, bool fetch);
  void CountDrawRepeat(xenos::PrimitiveType prim_type, uint32_t index_count,
                       bool indexed);
  // CP-owned derived state. Each consumer is invalidated independently by
  // RegisterFile::DrawDirty; command-buffer/resource lifetime has its own epoch.
  // Effective settings are sampled when a submission opens, never per draw.
  struct DrawConfig {
    bool async_shader_skip_draws = {};
    bool async_shader_vs_interpreter = {};
    int32_t debug_draw_mode = {};
    bool gpu_allow_invalid_fetch_constants = {};
    bool log_draw_state_repeats = {};
    int32_t log_memexport_draws = {};
    uint32_t log_pixel_shader_paths = {};
    bool log_texture_clobber = {};
    int32_t log_vertex_shader_hotness = {};
    bool rt_cache_full_overwrite_await_pipeline = {};
    bool spirv_texture_exp_adjust_specialization = {};
    bool spirv_texture_sign_specialization = {};
    bool spirv_texture_sign_specialization_used = {};
    bool vulkan_allow_reverse_z = {};
    bool vulkan_async_skip_draws = {};
    bool vulkan_debug_disable_alpha_to_mask = {};
    bool vulkan_defer_descriptor_updates = {};
    bool vulkan_depth_4x_as_1x = {};
    bool vulkan_msaa_4x_as_2x2 = {};
    int32_t vulkan_fragment_shading_rate = {};
    bool vulkan_fragment_shading_rate_alpha_tested = {};
    bool vulkan_in_pass_resolve_debug_read_usage = {};
    bool vulkan_memexport_compute_barrier = {};
    bool vulkan_memexport_vs_compute = {};
    int32_t vulkan_mid_frame_submission_draws = {};
    bool vulkan_placeholder_pipelines = {};
    bool vulkan_relax_unused_attachment_ops = {};
    bool vulkan_render_area_draw_bound = {};
    bool vulkan_render_area_tight = {};
    bool vulkan_republish_draw_inputs = {};
    bool vulkan_reuse_index_buffer_binding = {};
    bool vulkan_unclipped_fallback = {};
    bool vulkan_alpha_tested_depth_prepass = {};
  } draw_config_;
  void RefreshDrawConfig();
  void RefreshDrawCensusConfig();
  bool draw_census_enabled_ = false;
  void ResetDrawBounds(bool draw_height_bound);
  struct DrawStateCache {
    bool valid = false;
    uint64_t epoch = 0, submission = 0;
    const void* vertex_shader = nullptr;
    const void* pixel_shader = nullptr;
    VulkanShader* effective_pixel_shader = nullptr;
    bool primitive_polygonal = false, rasterization = false;
    uint32_t interpolator_mask = 0, ps_param_gen_pos = UINT32_MAX;
    draw_util::ViewportInfo viewport;
    uint32_t scale_x = 0, scale_y = 0;
    uint32_t used_texture_mask = 0;
    PrimitiveProcessor::ProcessingResult shape;
    SpirvShaderTranslator::Modification vertex_shader_modification;
    SpirvShaderTranslator::Modification pixel_shader_modification;
    VulkanShader::VulkanTranslation* vertex_shader_translation = nullptr;
    VulkanShader::VulkanTranslation* pixel_shader_translation = nullptr;
    bool use_interpreter = false;
    uint32_t normalized_color_mask = 0;
    reg::RB_DEPTHCONTROL normalized_depth_control;
    draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
    bool apply_host_depth_polygon_offset = false;
    VulkanPipelineCache::Pipeline* pipeline = nullptr;
    // vulkan_alpha_tested_depth_prepass: the depth-only and the color (depth
    // equal, early tests) pipelines replacing |pipeline| once both are ready.
    VulkanPipelineCache::Pipeline* prepass_pipeline = nullptr;
    VulkanPipelineCache::Pipeline* prepass_color_pipeline = nullptr;
    // spirv_texture_sign_specialization - the pipeline depends on them.
    uint32_t texture_sign_classes = 0;
    bool texture_exp_adjust_zero = false;
  };
  DrawStateCache draw_state_cache_;
  struct VertexInputRange {
    DrawMemoryRange memory;
    uint32_t slot;
  } vertex_input_ranges_[96];
  const VulkanShader* vertex_input_shader_ = nullptr;
  uint64_t vertex_input_fetch_generation_ = 0;
  uint32_t vertex_input_count_ = 0;
  bool UpdateVertexInputRanges(const VulkanShader& shader);
  bool draw_dynamic_state_dirty_ = true;
  bool draw_system_state_dirty_ = true;
  void BindDrawPipeline(VulkanPipelineCache::Pipeline* pipeline,
                        VkPipeline current_pipeline);
  // vulkan_bindless_textures: one update-after-bind descriptor set with arrays
  // of every live guest texture view and sampler; draws only write the slots
  // of their bindings into the system constants.
  // Null descriptors past the slots the shaders index, so reads near the end
  // of a binding's table stay in its allocation.
  static constexpr uint32_t kBindlessTablePadding = 256;
  bool InitializeBindless();
  // Points every slot at a null view or sampler (with the texture cache's null
  // images): a slot read before or after it holds a live view stays a valid
  // descriptor. Unwritten slots of the partially bound arrays faulted the GPU
  // (GROUP_ERROR_FATAL, ~18/s in GTA IV, each costing the queue and frames).
  bool FillBindlessWithNull();
  void WriteBindlessSlot(uint32_t array, uint32_t slot, VkImageView image_view,
                         VkSampler sampler);
  void ShutdownBindless();
  uint32_t GetBindlessSlot(SpirvShaderTranslator::BindlessBinding array,
                           VkImageView image_view, VkSampler sampler);
  void UpdateBindlessSlots(const VulkanShader* vertex_shader,
                           const VulkanShader* pixel_shader,
                           bool vertex_ready, bool pixel_ready);
  struct BindlessArray {
    std::unordered_map<uint64_t, uint32_t> slots;
    // Reused oldest first, and only once no never-written slot is left.
    std::deque<uint32_t> free_slots;
    // Slot 0 stays the null view or sampler (FillBindlessWithNull).
    uint32_t next_slot = 1;
  };
  struct BindlessRetiredSlot {
    uint64_t submission;
    uint32_t array;
    uint32_t slot;
  };
  VkDescriptorSetLayout bindless_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
  VkSampler bindless_null_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSet bindless_empty_set_ = VK_NULL_HANDLE;
  BindlessArray bindless_arrays_[SpirvShaderTranslator::kBindlessBindingCount];
  std::deque<BindlessRetiredSlot> bindless_retired_;
  uint32_t bindless_overflow_logs_ = 0;
  // A slot was written during the current draw: the set is bound again after
  // the write, so no driver can keep a copy of the set from before it.
  bool bindless_slot_written_ = false;
  // vulkan_alpha_tested_depth_prepass: configures the two pipelines of the
  // split, or leaves them null if the draw doesn't qualify.
  void ConfigureAlphaTestedDepthPrepass(
      const VulkanShader& vertex_shader, VulkanShader& pixel_shader,
      VulkanShader::VulkanTranslation* vertex_shader_translation,
      SpirvShaderTranslator::Modification pixel_shader_modification,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      uint32_t interpolator_mask, uint32_t ps_param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask, bool apply_host_depth_polygon_offset,
      VulkanPipelineCache::Pipeline** prepass_pipeline_out,
      VulkanPipelineCache::Pipeline** color_pipeline_out);
  uint64_t alpha_tested_prepass_draws_ = 0;
  uint64_t alpha_tested_prepass_unready_ = 0;
  // Pipeline setups (draws with dirty state) per GuestDispatch interval.
  enum class AlphaTestedPrepassReject : uint32_t {
    kNoColorOrDepth,
    kAlreadyEarly,
    kShader,
    kDepthFunc,
    kStencil,
    kBlend,
    kVariant,
    kPipeline,
    kCount,
  };
  uint64_t alpha_tested_prepass_configured_ = 0;
  uint64_t alpha_tested_prepass_rejects_[size_t(
      AlphaTestedPrepassReject::kCount)] = {};
  // Bumped by anything outside register writes that may change what a draw
  // derives (resolves, swaps).
  uint64_t draw_state_epoch_ = 0;
  // Register change counting is on (fast path or its statistics).
  bool draw_state_tracking_ = false;
  uint64_t fast_path_draws_ = 0;
  // Diagnostic (log_gpu_frame_time_breakdown): host vertices/indices drawn.
  uint64_t stat_host_vertices_ = 0;
  uint64_t stat_host_draws_ = 0;
  uint64_t stat_frames_ = 0;
  uint64_t fast_path_draws_seen_ = 0;
  uint64_t fast_path_miss_[8] = {};
  // Diagnostic (log_pixel_shader_paths): draws by pixel shader depth / stencil
  // mode (8 = no pixel shader) and depth write.
  uint64_t pixel_shader_path_draws_[9][2] = {};
  uint32_t pixel_shader_path_frames_ = 0;
  uint32_t mem_stats_frames_ = 0;
  uint32_t fast_path_report_frames_ = 0;
  uint64_t draw_state_changes_ = 0;
  uint64_t draw_fetch_changes_ = 0;
  uint64_t last_draw_state_changes_ = UINT64_MAX;
  uint64_t last_draw_fetch_changes_ = 0;
  const void* last_draw_vs_ = nullptr;
  const void* last_draw_ps_ = nullptr;
  xenos::PrimitiveType last_draw_prim_type_ = xenos::PrimitiveType::kNone;
  uint32_t last_draw_index_count_ = UINT32_MAX;
  bool last_draw_indexed_ = false;
  // Depth surfaces whose existing native 4x contents or incompatible draw
  // state prevent changing to the sample-grid layout. Keys are depth base plus
  // the guest surface pitch.
  std::unordered_set<uint64_t> msaa_4x_unsafe_depth_surfaces_;
  // Used only to diagnose a no-PS prepass that was already rewritten before a
  // later draw reveals that the same surface is a regular multisampled scene.
  std::unordered_set<uint64_t> msaa_4x_samples_as_pixels_depth_surfaces_;
  // Depth4xAs1x diagnostics, reported every 120 presented guest frames while
  // the opt-in is enabled. Rejections are mutually exclusive first reasons.
  uint32_t msaa_4x_as_1x_stats_frames_ = 0;
  uint64_t msaa_4x_as_1x_stats_seen_ = 0;
  uint64_t msaa_4x_as_1x_stats_eligible_ = 0;
  uint64_t msaa_4x_as_1x_stats_rewritten_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_incompatible_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_history_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_cvar_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_depth_write_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_scaled_ = 0;
  uint64_t msaa_4x_as_1x_stats_reject_pitch_ = 0;
  // Whether the previous draw was rewritten to samples as pixels. Cached
  // per-draw state (shader modifications, pipeline) from one layout must not
  // be reused by a draw of the other.
  bool last_draw_samples_as_pixels_ = false;

  void InitializeTrace() override;

 private:
  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) {
      static_assert_size(*this, sizeof(key));
    }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const PipelineLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(
        VkPipelineLayout pipeline_layout,
        VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
        VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref,
        uint32_t texture_count_vertex, uint32_t sampler_count_vertex,
        uint32_t texture_count_pixel, uint32_t sampler_count_pixel)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(
              descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(
              descriptor_set_layout_textures_pixel_ref),
          texture_count_vertex_(texture_count_vertex),
          sampler_count_vertex_(sampler_count_vertex),
          texture_count_pixel_(texture_count_pixel),
          sampler_count_pixel_(sampler_count_pixel) {}
    VkPipelineLayout GetPipelineLayout() const override {
      return pipeline_layout_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }
    // The binding counts the texture set layouts were built for - lets the
    // draw path verify a set allocation against the layout it will use.
    uint32_t texture_count_vertex() const { return texture_count_vertex_; }
    uint32_t sampler_count_vertex() const { return sampler_count_vertex_; }
    uint32_t texture_count_pixel() const { return texture_count_pixel_; }
    uint32_t sampler_count_pixel() const { return sampler_count_pixel_; }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
    uint32_t texture_count_vertex_;
    uint32_t sampler_count_vertex_;
    uint32_t texture_count_pixel_;
    uint32_t sampler_count_pixel_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,
    kSwapApplyGammaDescriptorSetDest,

    kSwapApplyGammaDescriptorSetCount,
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionCompletionAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);

  // Streamed encoding: a worker thread replays the deferred command buffer
  // into its own primary command buffer while the guest commands of the same
  // submission are still being processed, so the driver's command encoding
  // runs on another core. Commands are handed over only when a render pass
  // has been closed (its render area patched and its pass-head slot filled).
  bool StreamEncodeBegin();
  void StreamEncodePublish(bool submission_end = false);
  void StreamEncodeAwaitIdle();
  void StreamEncodeThreadMain();
  void StreamEncodeShutdown();
  bool AwaitSubmissionAndUpdateCompleted(uint64_t submission) {
    if (!heartbeat_) {
      return completion_timeline_.AwaitSubmissionAndUpdateCompleted(submission);
    }
    hb_gpu_wait_.store(submission, std::memory_order_relaxed);
    const bool completed =
        completion_timeline_.AwaitSubmissionAndUpdateCompleted(submission);
    hb_gpu_wait_.store(0, std::memory_order_relaxed);
    return completed;
  }
  // Checks if ending a submission right now would not cause potentially more
  // delay than it would reduce - such as when there are unfinished graphics
  // pipeline creation requests.
  bool CanEndSubmissionImmediately() const;
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionCompletionAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ &&
           GetCompletedSubmission() + 1u >= GetCurrentSubmission();
  }

  void ClearTransientDescriptorPools();

  void SplitPendingBarrier();

  void DestroyScratchBuffer();

  // ZPD occlusion queries backend.
  // vkCmdBeginQuery is only valid inside a render pass, so segments split at
  // pass end and resume at the next pass begin. If BEGIN fires outside a pass,
  // segment_pending_begin waits for the next. Outside a render pass,
  // DiscardZPDQuery defers the slot release until the submission completes.
  // FSI queries clear a dedicated counter with vkCmdFillBuffer, so they may
  // need to open before a pass begins or split an active pass around the clear.

  void EnsureZPDQueryResources() override;
  void ShutdownZPDQueryResources() override {
    zpd_resolves_in_flight_.clear();
    zpd_deferred_releases_.clear();
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    zpd_fsi_counter_index_force_update_ = true;
    if (zpd_host_query_pool_) {
      zpd_host_query_pool_->Shutdown();
    }
  }

  bool IsZPDQueryPoolReady() const override;
  bool CanOpenZPDQuery() const override;

  QueryOpenResult OpenZPDQuery(ReportHandle report_handle,
                               bool can_close_submission) override;
  bool CloseZPDQuery(ReportHandle report_handle,
                     uint64_t& out_submission) override;
  bool DiscardZPDQuery() override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle,
                         uint64_t wait_for_submission) override;

  void UpdateDynamicState(
      const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      bool depth_bias_in_pixel_shader,
      const VulkanPipelineCache::DynamicState& pipeline_dynamic_state);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
      uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset);
  // *_bindings_ready is IssueDraw's per-draw readiness snapshot, the same one
  // the sampler collection used - re-reading bindings_ready() here would race
  // translation finishing mid-draw.
  bool UpdateBindings(const VulkanShader* vertex_shader,
                      const VulkanShader* pixel_shader,
                      bool vertex_bindings_ready, bool pixel_bindings_ready,
                      bool interpreter_placeholder = false,
                      bool placeholder_pixel_shader = false);
  struct TextureDescriptorCacheEntry;
  // A hit returns an immutable set and emits no writes. New sets become visible
  // to later draws only after UpdateBindings records every descriptor write.
  VkDescriptorSet GetTextureBindings(
      bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
      VkDescriptorSetLayout descriptor_set_layout,
      const VkDescriptorImageInfo* texture_image_info,
      const VkDescriptorImageInfo* sampler_image_info,
      VkWriteDescriptorSet* descriptor_set_writes_out,
      uint32_t& write_count_out, TextureDescriptorCacheEntry*& entry_out);

  // RTTRACE one-shot diagnostic (see IsRtTraceActive / cvar
  // vulkan_rt_trace_request). Builds the draw signature for the current draw
  // and aggregates it into the currently open run, flushing (logging) the
  // previous run first if the signature changed.
  void RtTraceRecordDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         bool pipeline_null,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask,
                         bool is_rasterization_done);
  // Logs (and closes) the currently open RTTRACE draw run, if any.
  void RtTraceFlushRun();

  // Rewrites an eligible host-render-target 4x depth-only draw as 1x with the
  // guest samples represented as a 2x2 grid of pixels. The caller restores
  // RB_SURFACE_INFO and resets RenderTargetCache's transient scale on exit.
  bool RewriteMsaa4xDepthSurfaceInfoForDraw(VulkanShader* pixel_shader);

  bool device_lost_ = false;

  // Rolling per-submission summary for diagnosing VK_ERROR_DEVICE_LOST.
  // Accumulated into submission_in_progress_ during a submission, snapshotted
  // into submission_history_ on successful submit, dumped on device-loss.
  struct SubmissionSummary {
    uint64_t submission_index = 0;
    uint64_t frame_index = 0;
    uint32_t draw_count = 0;
    uint32_t dispatch_count = 0;
    uint32_t resolve_count = 0;
    uint64_t last_vs_hash = 0;
    uint64_t last_ps_hash = 0;
    uint64_t last_render_pass_key = 0;
  };
  static constexpr size_t kSubmissionHistorySize = 16;
  std::array<SubmissionSummary, kSubmissionHistorySize> submission_history_{};
  size_t submission_history_next_ = 0;
  SubmissionSummary submission_in_progress_{};

  void LogRecentSubmissions(const char* context);

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkSemaphore> semaphores_free_;

  struct PendingQueryResolve {
    uint64_t submission = 0;
    uint32_t query_index = UINT32_MAX;
    uint32_t query_generation = 0;
    uint32_t scale_area = 1;
    bool uses_fsi_counter = false;
    ReportHandle report_handle = kInvalidReportHandle;
  };
  uint32_t zpd_active_query_index_ = UINT32_MAX;
  uint32_t zpd_active_query_generation_ = 0;
  bool zpd_active_query_is_fsi_ = false;
  bool zpd_fsi_counter_index_force_update_ = true;
  std::deque<PendingQueryResolve> zpd_resolves_in_flight_;
  // Fallback buffer for EDRAM descriptor binding 2.
  VkBuffer zpd_fsi_counter_sink_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory zpd_fsi_counter_sink_buffer_memory_ = VK_NULL_HANDLE;
  // Currently installed binding 2 buffer.
  VkBuffer zpd_fsi_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize zpd_fsi_counter_descriptor_range_ = 0;

  ui::vulkan::VulkanGPUCompletionTimeline completion_timeline_;
  // Per-frame GPU synchronization diagnostics (worker thread only), reported
  // alongside the log_gpu_frame_time_breakdown breakdown.
  struct VkFrameSyncStats {
    uint64_t frames = 0;
    uint64_t awaits = 0;    // blocking waits for own submissions
    uint64_t await_ns = 0;  // time inside those waits
    uint64_t submissions = 0;
    uint64_t resolves = 0;  // EDRAM resolves issued (churn magnitude)
    uint64_t memexport_awaits = 0;
    uint64_t readback_awaits = 0;
    // Host-side latency from vkQueueSubmit to the fence being observed
    // completed - includes GPU queue wait, render time and detection delay.
    uint64_t sub_latency_ns = 0;
    uint64_t sub_latency_max_ns = 0;
    uint64_t sub_completions = 0;
    // Awaits that actually blocked, and the sum over them of
    // (current submission - awaited submission) to see how many submissions
    // of headroom the throttle really has.
    uint64_t blocking_awaits = 0;
    uint64_t await_delta = 0;
    // GPU-side wall time (top-of-pipe to bottom-of-pipe timestamps) of
    // completed submissions, and the idle gaps between consecutive ones.
    uint64_t gpu_exec_ns = 0;
    uint64_t gpu_exec_max_ns = 0;
    uint64_t gpu_gap_ns = 0;
    uint64_t gpu_samples = 0;
    // GPU time inside resolve emission regions (see the pairs in IssueCopy).
    uint64_t resolve_gpu_ns = 0;
    uint64_t resolve_gpu_max_ns = 0;
    uint64_t resolve_gpu_samples = 0;
    uint64_t resolve_ts_dropped = 0;
    // Same, for texture loads (BeginTimedSection kinds 1-3: first load, CPU-
    // written, GPU-written source).
    uint64_t texload_gpu_ns = 0;
    uint64_t texload_gpu_max_ns = 0;
    uint64_t texload_gpu_samples = 0;
    uint64_t texload_kind_ns[3] = {};
    uint64_t texload_kind_samples[3] = {};
    struct ResolveTextureStats {
      uint64_t avoided = 0, reloaded = 0;
      uint64_t reload_ns = 0, copy_ns = 0;
    };
    std::array<ResolveTextureStats, 64> resolve_texture{};
    // Draw/pass structure counters.
    uint64_t draws = 0;
    uint64_t vertices = 0;
    uint64_t render_pass_begins = 0;
    uint64_t primary_buffer_splits = 0;
    uint64_t rb_uma_direct = 0;
    uint64_t rb_uma_first_use = 0;
    uint64_t last_report_ns = 0;
  };
  VkFrameSyncStats vk_frame_sync_stats_;
  // Command totals at the previous VkFrameSync report.
  DeferredCommandBuffer::CommandStats frame_sync_command_stats_prev_;

 public:
  VkFrameSyncStats& vk_frame_sync_stats() { return vk_frame_sync_stats_; }

 private:
  struct SubmitTimeRecord {
    uint64_t submission;
    uint64_t submit_ns;
    // Slot in frame_timestamp_pool_, UINT32_MAX if timestamps unavailable.
    uint32_t timestamp_slot;
    // Resolve timestamp pairs recorded in this submission.
    uint32_t resolve_slot_base;
    uint32_t resolve_pair_count;
    // Render-pass timestamp pairs recorded in this submission.
    uint32_t pass_slot_base;
    uint32_t pass_pair_count;
  };
  std::deque<SubmitTimeRecord> vk_submit_times_;
  // GPU timestamps around each submission (2 per slot). Diagnostics use mapped
  // readback; the optional UI reads queries only after completed fences, without
  // WAIT. Driver overhead of those completed-only host reads needs device testing
  // (see the Turnip/kgsl note about host polling while submissions are pending).
  static constexpr uint32_t kFrameTimestampSlots = 32;
  GpuFrameTimer gpu_frame_timer_;
  uint32_t frame_timestamp_valid_bits_ = 0;
  VkQueryPool frame_timestamp_pool_ = VK_NULL_HANDLE;
  // Draws recorded into the currently open submission, for
  // vulkan_mid_frame_submission_draws.
  uint32_t draws_since_submission_ = 0;
  VkBuffer frame_timestamp_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory frame_timestamp_buffer_memory_ = VK_NULL_HANDLE;
  VkDeviceSize frame_timestamp_buffer_size_ = 0;
  uint64_t* frame_timestamp_mapping_ = nullptr;
  uint64_t frame_timestamp_prev_end_ = 0;
  // GPU timestamps bracketing each resolve region, ring-buffered per
  // submission like the per-submission pair above (same no-host-query rule).
  static constexpr uint32_t kResolveTimestampPairsPerSubmission = 48;
  static constexpr uint32_t kResolveTimestampRingSubmissions = 32;
  // Pair kinds: 0 resolve, 1 first load, 2 CPU reload, 3 GPU reload, 4 mirror copy.
  uint8_t resolve_ts_kind_[kResolveTimestampPairsPerSubmission *
                           kResolveTimestampRingSubmissions] = {};
  uint8_t resolve_ts_texture_format_[kResolveTimestampPairsPerSubmission *
                                     kResolveTimestampRingSubmissions] = {};
  VkQueryPool resolve_timestamp_pool_ = VK_NULL_HANDLE;
  VkBuffer resolve_timestamp_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_timestamp_buffer_memory_ = VK_NULL_HANDLE;
  VkDeviceSize resolve_timestamp_buffer_size_ = 0;
  uint64_t* resolve_timestamp_mapping_ = nullptr;
  uint64_t resolve_ts_submission_ = 0;
  uint32_t resolve_ts_count_ = 0;
  // GPU timestamps bracketing each render pass, bucketed CPU-side by
  // framebuffer extent (bit 31 = ownership-transfer pass). Same ring/readback
  // pattern; timestamps written OUTSIDE the pass (before begin / after end).
  // Bit 30 = the time between a pass and the next one (compute, copies,
  // barriers), low bits = DispatchSite mask recorded in it.
  static constexpr uint32_t kPassTimestampGapKey = 0x40000000u;
  static constexpr uint32_t kPassTimestampPairsPerSubmission = 256;
  static constexpr uint32_t kPassTimestampRingSubmissions = 32;
  VkQueryPool pass_timestamp_pool_ = VK_NULL_HANDLE;
  VkBuffer pass_timestamp_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory pass_timestamp_buffer_memory_ = VK_NULL_HANDLE;
  VkDeviceSize pass_timestamp_buffer_size_ = 0;
  uint64_t* pass_timestamp_mapping_ = nullptr;
  uint64_t pass_ts_submission_ = 0;
  uint32_t pass_ts_count_ = 0;
  uint32_t pass_ts_open_pair_ = UINT32_MAX;
  uint64_t pass_ts_open_submission_ = 0;
  uint64_t pass_ts_dropped_ = 0;
  std::array<uint32_t, size_t(kPassTimestampPairsPerSubmission) *
                           kPassTimestampRingSubmissions>
      pass_ts_keys_{};
  std::array<uint32_t, size_t(kPassTimestampPairsPerSubmission) *
                           kPassTimestampRingSubmissions>
      pass_ts_draws_{};
  uint32_t pass_open_draws_ = 0;
  // Largest scissor corner seen among the draws of the currently open pass -
  // the region actually touched, as opposed to the full EDRAM-derived
  // framebuffer the render area is set to.
  uint32_t pass_open_scissor_w_ = 0;
  uint32_t pass_open_scissor_h_ = 0;
  // Same for the viewport, which bounds the draw even when the guest leaves
  // the scissor wide open.
  uint32_t pass_open_viewport_w_ = 0;
  uint32_t pass_open_viewport_h_ = 0;
  std::array<uint32_t, size_t(kPassTimestampPairsPerSubmission) *
                           kPassTimestampRingSubmissions>
      pass_ts_scissor_{};
  std::array<uint32_t, size_t(kPassTimestampPairsPerSubmission) *
                           kPassTimestampRingSubmissions>
      pass_ts_viewport_{};
  struct PassBucketStat {
    uint64_t ns = 0;
    uint64_t passes = 0;
    uint64_t draws = 0;
    uint32_t max_scissor_w = 0;
    uint32_t max_scissor_h = 0;
    uint32_t max_viewport_w = 0;
    uint32_t max_viewport_h = 0;
  };
  // Accumulated per bucket key since the last report.
  std::map<uint32_t, PassBucketStat> pass_bucket_stats_;
  // vulkan_memexport_batch: memory-export-only draws of one vertex shader
  // waiting to be dispatched together (workgroup Y = draw), with each draw's
  // constants laid out per SpirvShaderTranslator::MemExportBatchStride*.
  struct MemExportBatch {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount] = {};
    uint32_t set_count = 0;
    uint32_t size = 0;
    uint32_t capacity = 0;
    uint32_t float_stride = 0;
    uint32_t max_vertex_count = 0;
    std::vector<uint8_t> system, float_vertex, bool_loop, fetch;
  };
  MemExportBatch memexport_batch_;
  bool memexport_batch_flushing_ = false;
  // The batched constant arrays need kMemExportBatchMaxBytes of uniform range.
  bool memexport_batch_supported_ = false;
  uint64_t memexport_batched_draws_ = 0;
  uint64_t memexport_batch_dispatches_ = 0;
  void PushMemexportBeforeBarrier();
  void PushMemexportAfterBarrier();
  void AddToMemExportBatch(VkPipeline pipeline, VkPipelineLayout layout,
                           uint32_t set_count, uint32_t float_constant_count,
                           uint32_t vertex_count);
  void FlushMemExportBatch();
  void OpenPassTimestamp(uint32_t bucket_key);
  void ClosePassTimestamp();
  void CloseGapTimestamp();
  bool pass_ts_gap_open_ = false;
  bool submission_open_ = false;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::deque<std::pair<uint64_t, VkSemaphore>>
      submissions_in_flight_semaphores_;

  static constexpr uint32_t kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;

  // Streamed encoding (see StreamEncodeBegin). The command buffers are
  // submitted after the one with the setup commands.
  bool stream_encode_enabled_ = false;
  bool stream_encode_active_ = false;
  CommandBuffer stream_command_buffer_ = {};
  std::vector<CommandBuffer> stream_command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>>
      stream_command_buffers_submitted_;
  std::thread stream_encode_thread_;
  std::mutex stream_encode_mutex_;
  std::condition_variable stream_encode_work_cond_;
  std::condition_variable stream_encode_idle_cond_;
  // Guarded by stream_encode_mutex_.
  VkCommandBuffer stream_encode_target_ = VK_NULL_HANDLE;
  // Fixed under stream_encode_mutex_ for the submission: recording a resolve
  // tail may temporarily redirect the CP's recording command buffer.
  DeferredCommandBuffer* stream_encode_source_ = nullptr;
  size_t stream_encode_published_ = 0;
  size_t stream_encode_encoded_ = 0;
  bool stream_encode_busy_ = false;
  // The encode thread is blocked in stream_encode_work_cond_ (bionic's
  // pthread_cond_signal always enters the kernel, so only wake a sleeper).
  bool stream_encode_sleeping_ = false;
  bool stream_encode_stop_ = false;
  // Only accessed by the replaying thread while it's busy.
  bool stream_encode_pipeline_unready_ = false;

  DeferredCommandBuffer deferred_command_buffer_storage_;
  DeferredCommandBuffer* deferred_command_buffer_ =
      &deferred_command_buffer_storage_;
  DeferredCommandBuffer deferred_setup_command_buffer_;
  PassByteLedger pass_bytes_;
  bool pass_bytes_active_ = false;
  bool pass_bytes_resolve_scope_ = false;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout,
             size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ =
      VK_NULL_HANDLE;

  // Guards the layout maps below - GetPipelineLayout is called both from the
  // draw thread and from pipeline creation threads (for deferred translation).
  std::mutex pipeline_layouts_mutex_;
  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout,
                     PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageImage;
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_image_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>,
             size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>>
      constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet>
      texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey,
                     std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;
  struct TextureDescriptorCacheEntry {
    uint64_t hash = 0, frame = 0;
    uint64_t images = 0, samplers = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    TextureDescriptorSetLayoutKey layout_key;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool written = false;
    std::vector<VkDescriptorImageInfo> infos;
  };
  std::array<TextureDescriptorCacheEntry, 512> texture_descriptor_cache_{};

  // log_native_heap_census_at_frame.
  void NativeHeapCensus();
  std::unique_ptr<VulkanSharedMemory> shared_memory_;
  // In-pass vertex fetch ring: fetch constants of the current draw pointed at
  // ring copies, applied when UpdateBindings writes the fetch constants.
  struct VfetchRingPatch {
    uint32_t vfetch_index;
    uint32_t dword_0;
  };
  VfetchRingPatch vfetch_ring_patches_[96];
  uint32_t vfetch_ring_patch_count_ = 0;
  uint32_t vfetch_ring_patched_mask_[3] = {};
  bool vfetch_ring_patched_previous_draw_ = false;
  uint64_t vfetch_window_accepted_ = 0;
  uint64_t vfetch_window_second_ = 0;
  uint64_t vfetch_window_copy_failed_ = 0;

 public:
  void OnPrimaryBufferBatch() {
    CommandProcessor::OnPrimaryBufferBatch();
    // Unwatched CPU inputs may be revalidated in the new command batch.
    // A state-only census must not join across that observation boundary.
    if (draw_census_enabled_) DrawCensusClose(kCensusBarrier);
    RefreshDrawCensusConfig();
    if (shared_memory_) {
      shared_memory_->BeginCommandBatch();
    }
  }

 private:

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  std::unique_ptr<VulkanZPDQueryPool> zpd_host_query_pool_;

  // Deferred query slot releases for discards that happen outside a render
  // pass, where vkCmdEndQuery cannot be issued.  The slot is held until the
  // submission containing the stale BeginQuery completes on the GPU.
  struct DeferredQueryRelease {
    uint64_t submission;
    uint32_t query_index;
    uint32_t query_generation;
  };
  std::deque<DeferredQueryRelease> zpd_deferred_releases_;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;
  // Second set identical to the one above except binding 0 points at the
  // host-imported shared-memory buffer (aliasing guest RAM). Bound instead of
  // the device set for memexport-touching draws so their GPU-written geometry
  // stays coherent with the CPU. VK_NULL_HANDLE when the host buffer is
  // unavailable - routing is then disabled and every draw uses the device set.
  VkDescriptorSet shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  // Two-buffer memexport page tracking (data + methods), shared with the D3D12
  // backend as a class-body fragment so each backend gets its own non-virtual,
  // inlinable copy. Requires <array> and SharedMemory, both included above.
#include "../command_processor_memexport.inc"

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ =
      VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ =
      VK_NULL_HANDLE;
  // Storage image descriptor set layout for compute shader destination.
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ =
      VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight>
      swap_descriptors_gamma_ramp_;
  // Sampled images for source.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Storage images for destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_dest_;

  // Gamma ramp application compute pipeline constants.
  struct ApplyGammaConstants {
    uint32_t size[2];
  };
  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  // Gamma pipelines that also compute FXAA luma in the alpha channel.
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ =
      VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;

  // FXAA post-processing.
  struct FxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };
  // Descriptor set layout for FXAA source (combined image sampler).
  VkDescriptorSetLayout fxaa_source_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkSampler fxaa_sampler_ = VK_NULL_HANDLE;

  // Resolve downscale compute shader for scaled resolution readback.
  // Downscales scaled resolve buffer data back to 1x resolution on the GPU,
  // avoiding expensive CPU-side downscaling and reducing transfer bandwidth.
  VkDescriptorSetLayout resolve_downscale_descriptor_set_layout_ =
      VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;

  // log_render_pass_breaks.
  void* render_pass_break_caller_ = nullptr;
  std::unordered_map<uint64_t, uint64_t> render_pass_break_counts_;
  // Diagnostic (log_render_pass_breaks): caller of the first barrier pushed
  // since the last submission of barriers, counted when they end a pass.
  void* first_barrier_pusher_ = nullptr;
  std::unordered_map<uint64_t, uint64_t> render_pass_barrier_pushers_;
  uint32_t render_pass_break_frames_ = 0;
  // Diagnostic (log_render_pass_breaks): passes that begin on the same
  // framebuffer the previous pass ended on, keyed like the break counts - the
  // breaks that only cost a store + reload of the same attachments.
  const void* render_pass_last_ended_framebuffer_ = nullptr;
  uint64_t render_pass_last_break_key_ = 0;
  std::unordered_map<uint64_t, uint64_t> render_pass_reentry_counts_;

  // Diagnostic only: follow one resolve through the next same-framebuffer
  // pass. The next resolve is a flush boundary, not a coalescing assumption.
  enum ResolveDeferReason : uint32_t {
    kResolveDeferWrite = 1,
    kResolveDeferRead = 2,
    kResolveDeferClear = 4,
    kResolveDeferReadback = 8,
    kResolveDeferEvent = 16,
    kResolveDeferUnknown = 32,
  };
  struct ResolveDeferCensus {
    const void* framebuffer = nullptr;
    bool active = false, recording = false, broke = false, reentered = false;
    uint32_t reasons = kResolveDeferUnknown, draws = 0;
    uint32_t address = 0, length = 0;
    uint64_t cpu_epoch = UINT64_MAX;
    std::array<uint64_t, xenos::kEdramTileCount / 64> tiles = {};
  } resolve_defer_census_;
  std::array<uint64_t, 64> resolve_defer_all_ = {}, resolve_defer_reentry_ = {};
  void ResolveDeferDescribe(const draw_util::ResolveInfo& info,
                            bool for_execution = false);
  bool CanDeferResolveReadback(const draw_util::ResolveInfo& info) const;
  void ResolveDeferFinish();
  void ResolveDeferPacket(uint32_t opcode);
  void ResolveDeferDraw(const VulkanShader& vertex_shader,
                         const VulkanShader* pixel_shader,
                         const IndexBufferInfo* indices);
  void ResolveDeferDrawWrites(reg::RB_DEPTHCONTROL depth, uint32_t color_mask);
  void ResolveDeferReport();
  enum class ResolveDeferStop : uint32_t {
    kPassEnd, kWrite, kDestination, kClear, kReadback, kEvent,
    kSubmission, kNextResolve, kResource, kCount
  };
  void FlushDeferredResolve(ResolveDeferStop reason);
  bool RecordDeferredResolve(const draw_util::ResolveInfo& info);
  std::unique_ptr<DeferredCommandBuffer> deferred_resolve_commands_;
  bool deferred_resolve_pending_ = false;
  bool deferred_resolve_recording_ = false;
  bool deferred_resolve_scratch_pinned_ = false;
  VkPipeline deferred_resolve_final_compute_pipeline_ = VK_NULL_HANDLE;
  ResolveDeferStop deferred_resolve_stop_ = ResolveDeferStop::kPassEnd;
  uint64_t deferred_resolve_queued_ = 0, deferred_resolve_executed_ = 0;
  uint64_t deferred_resolve_with_draws_ = 0, deferred_resolve_uma_queued_ = 0;
  bool deferred_resolve_has_draws_ = false;
  std::array<uint64_t, size_t(ResolveDeferStop::kCount)>
      deferred_resolve_stops_ = {}, deferred_resolve_rejected_ = {};

  // Vertex shaders with memory export translated to compute shaders, for
  // devices without vertexPipelineStoresAndAtomics, by (translation
  // modification, guest pipeline layout).
  struct MemExportComputePipelineKey {
    const void* translation;
    VkPipelineLayout pipeline_layout;
    bool operator==(const MemExportComputePipelineKey& other) const {
      return translation == other.translation &&
             pipeline_layout == other.pipeline_layout;
    }
    struct Hasher {
      size_t operator()(const MemExportComputePipelineKey& key) const {
        return std::hash<const void*>{}(key.translation) ^
               (std::hash<const void*>{}(
                    reinterpret_cast<const void*>(key.pipeline_layout))
                << 1);
      }
    };
  };
  std::unordered_map<MemExportComputePipelineKey, VkPipeline,
                     MemExportComputePipelineKey::Hasher>
      memexport_compute_pipelines_;
  // Null if the shader can't be converted.
  VkPipeline GetMemExportComputePipeline(
      VulkanShader& vertex_shader,
      SpirvShaderTranslator::Modification vertex_shader_modification,
      VkPipelineLayout pipeline_layout, bool batched);
  // Descriptor pool chain for resolve downscale shader (2 storage buffers per
  // set). Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
  std::unique_ptr<ui::vulkan::VulkanDescriptorPoolChain>
      resolve_downscale_descriptor_pool_chain_;
  // Intermediate buffer for downscaled output (device local, for compute).
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  // FXAA source texture - R16G16B16A16_SFLOAT for luma in alpha.
  static constexpr VkFormat kFxaaSourceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkDeviceMemory fxaa_source_memory_ = VK_NULL_HANDLE;
  VkImage fxaa_source_image_ = VK_NULL_HANDLE;
  VkImageView fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t fxaa_source_width_ = 0;
  uint32_t fxaa_source_height_ = 0;
  uint64_t fxaa_source_last_submission_ = 0;
  // Descriptor sets for FXAA - source as combined image sampler (for FXAA
  // read).
  std::array<VkDescriptorSet, kMaxFramesInFlight> fxaa_source_descriptors_;
  // Descriptor sets for FXAA - source as storage image (for gamma+luma write).
  std::array<VkDescriptorSet, kMaxFramesInFlight>
      fxaa_source_storage_descriptors_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  // Index buffer currently bound at offset 0 (VK_NULL_HANDLE if unknown or
  // bound at a non-zero offset).
  // log_vertex_shader_hotness state: ucode hash -> (indices, draws).
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>>
      vertex_shader_hotness_;
  uint32_t vertex_shader_hotness_frames_ = 0;
  // thermal_governor state.
  void ThermalGovernorPoll();
  uint64_t thermal_governor_last_ns_ = 0;
  uint32_t thermal_governor_rate_ = 0;
  bool thermal_governor_logged_ = false;
  // adaptive_shading_rate state: GPU busy time (union of the completed
  // submissions' timestamp spans) over a wall-clock window.
  void AdaptiveShadingPoll();
  GpuTimestampSpanUnion adaptive_shading_spans_;
  uint64_t adaptive_shading_busy_ns_ = 0;
  uint64_t adaptive_shading_window_ns_ = 0;
  uint32_t adaptive_shading_frames_ = 0;
  // Last submission with a draw whose pixel shader may read or write the
  // shared memory (vertex fetch, memory export, in-pass resolves).
  uint64_t fragment_shared_memory_submission_ = 0;
  uint32_t adaptive_shading_rate_ = 0;
  // Windows in a row calm enough for the next finer rate, how many a step down
  // needs (doubled when the coarser rate came back within 3 windows of one,
  // so it doesn't swing at a frame rate cap), and windows since the last
  // step down.
  uint32_t adaptive_shading_calm_windows_ = 0;
  uint32_t adaptive_shading_down_hold_ = 2;
  uint32_t adaptive_shading_since_down_ = UINT32_MAX;
  // GPU bound by adaptive_shading_rate's measure: the CPU waits like when hot.
  bool adaptive_gpu_bound_ = false;
  // log_guest_fps state.
  // log_frame_pacing state.
  FrameIntervalHistogram guest_swap_intervals_;
  uint64_t guest_interval_last_ns_ = 0;
  std::vector<float> guest_interval_ms_;
  uint64_t guest_fps_last_ns_ = 0;
  uint32_t guest_fps_frames_ = 0;
  uint64_t guest_fps_draws_ = 0;
  // Draws since the last swap, published for the frame pacer.
  uint32_t frame_draws_for_pacing_ = 0;
  VkBuffer bound_index_buffer_ = VK_NULL_HANDLE;
  VkIndexType bound_index_type_ = VK_INDEX_TYPE_UINT16;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Extended dynamic state (VK_EXT_extended_dynamic_state / state2 / state3).
  // Cached last-emitted values + dirty flags, mirroring the existing dynamic
  // state idiom. Initialized to invalid sentinels so the first draw of each
  // submission always emits. Only emitted when the matching capability bit in
  // the pipeline cache is set.
  VkPrimitiveTopology dynamic_primitive_topology_ = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  VkBool32 dynamic_primitive_restart_enable_ = VK_FALSE;
  VkCullModeFlags dynamic_cull_mode_ = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
  VkFrontFace dynamic_front_face_ = VK_FRONT_FACE_MAX_ENUM;
  VkBool32 dynamic_depth_test_enable_ = VK_FALSE;
  VkBool32 dynamic_depth_write_enable_ = VK_FALSE;
  VkCompareOp dynamic_depth_compare_op_ = VK_COMPARE_OP_MAX_ENUM;
  VkBool32 dynamic_stencil_test_enable_ = VK_FALSE;
  VkStencilOpState dynamic_stencil_op_front_ = {};
  VkStencilOpState dynamic_stencil_op_back_ = {};
  VkBool32 dynamic_depth_clamp_enable_ = VK_FALSE;
  VkPolygonMode dynamic_polygon_mode_ = VK_POLYGON_MODE_MAX_ENUM;
  VkBool32 dynamic_color_blend_enable_[xenos::kMaxColorRenderTargets] = {};
  VkColorBlendEquationEXT
      dynamic_color_blend_equation_[xenos::kMaxColorRenderTargets] = {};
  VkColorComponentFlags
      dynamic_color_write_mask_[xenos::kMaxColorRenderTargets] = {};
  bool dynamic_primitive_topology_update_needed_;
  bool dynamic_primitive_restart_enable_update_needed_;
  bool dynamic_cull_mode_update_needed_;
  bool dynamic_front_face_update_needed_;
  bool dynamic_depth_test_enable_update_needed_;
  bool dynamic_depth_write_enable_update_needed_;
  bool dynamic_depth_compare_op_update_needed_;
  bool dynamic_stencil_test_enable_update_needed_;
  bool dynamic_stencil_op_front_update_needed_;
  bool dynamic_stencil_op_back_update_needed_;
  bool dynamic_depth_clamp_enable_update_needed_;
  bool dynamic_polygon_mode_update_needed_;
  // vulkan_fragment_shading_rate: fragment size (width << 8 | height) of the
  // guest draw being issued, and the one last recorded.
  uint32_t draw_fragment_shading_rate_ = 0x0101;
  // vulkan_msaa_2x2_pixel_rate, latched with the shading rate capability.
  bool msaa_2x2_pixel_rate_ = false;
  uint32_t dynamic_fragment_shading_rate_ = 0x0101;
  bool dynamic_fragment_shading_rate_update_needed_ = true;
  uint64_t fragment_shading_rate_coarse_draws_ = 0;
  uint64_t fragment_shading_rate_draws_ = 0;
  bool dynamic_color_blend_enable_update_needed_;
  bool dynamic_color_blend_equation_update_needed_;
  bool dynamic_color_write_mask_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_pixel_;
  // Sampler cache state, independently tracking fetch values and lifetime.
  // A stage's current_samplers_* contents are reusable for the next draw if:
  // the same shader provides the binding list (pointers are stable - shaders
  // are only destroyed at pipeline cache shutdown), no sampler was destroyed
  // since (destroy generation), and per fetch slot, the fetch constant wasn't
  // written since the parameters were derived (per-stage dirty masks, bit set
  // = up to date, maintained in WriteRegister and committed after a draw's
  // sampler pass). The per-stage submission stamp tracks when UseSampler was
  // last called for every entry - it must run at least once per submission so
  // the texture cache's LRU eviction (which only frees samplers whose last
  // usage submission has fully completed) can never destroy a cached handle.
  uint32_t current_samplers_fetch_up_to_date_vertex_ = 0;
  uint32_t current_samplers_fetch_up_to_date_pixel_ = 0;
  const VulkanShader* current_samplers_shader_vertex_ = nullptr;
  const VulkanShader* current_samplers_shader_pixel_ = nullptr;
  uint64_t current_samplers_submission_vertex_ = 0;
  uint64_t current_samplers_submission_pixel_ = 0;
  uint64_t current_samplers_destroy_generation_ = ~uint64_t(0);

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_ = nullptr;
  // True when inside a render pass or dynamic rendering block.
  bool in_render_pass_ = false;
  uint32_t pass_head_slot_ = UINT32_MAX;
  // vulkan_relax_unused_attachment_ops statistics.
  uint64_t relaxed_attachment_ops_total_ = 0;
  uint64_t relaxed_attachment_ops_passes_ = 0;
  uint64_t render_pass_serial_ = 0;
  void RecordPassHeadSlot();

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is nullptr in this case).
  // current_guest_graphics_pipeline_ is a STABLE pointer to the pipeline
  // cache's atomic VkPipeline slot, not a handle value - with asynchronous
  // creation the handle may still be VK_NULL_HANDLE at record time, so the
  // bind de-dup compares slot pointers and the deferred command buffer
  // resolves the handle at replay.
  const std::atomic<VkPipeline>* current_guest_graphics_pipeline_;
  // The handle the last recorded deferred bind captured; the first draw after
  // the slot fills or swaps re-records with the new handle.
  VkPipeline current_guest_graphics_pipeline_handle_ = VK_NULL_HANDLE;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  VkDescriptorBufferInfo current_constant_buffer_infos_
      [SpirvShaderTranslator::kConstantBufferCount];
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  // Dynamic constant buffers: when true (gated at SetupContext on the device reporting
  // maxDescriptorSetUniformBuffersDynamic >= kConstantBufferCount and the
  // vulkan_dynamic_constant_buffers cvar), the kDescriptorSetConstants set uses
  // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: the descriptor base offset is 0
  // and the per-draw slice offset is supplied as a dynamic offset at bind time,
  // so the transient set is re-allocated and re-written ONLY when a binding's
  // backing VkBuffer changes (uniform_buffer_pool_ page rollover). Held constant
  // for the device lifetime; all of the layout binding type, the
  // VkWriteDescriptorSet type and the bind's pDynamicOffsets switch on it
  // together (a partial switch is a VUID violation).
  bool use_dynamic_constants_ = false;
  // Value-cache for the constants descriptor set (one shared set, 5 bindings).
  // Mirrors the sentinel-free validity discipline of the texture hash caches
  // above: constants_descriptor_set_buffers_[i] records the VkBuffer handle last
  // WRITTEN into binding i of the cached set in
  // current_graphics_descriptor_sets_[kDescriptorSetConstants]. When all five
  // still match the current_constant_buffer_infos_[i].buffer handles AND the
  // recorded ranges still match (no page rollover and no constant-count change),
  // the per-draw vkUpdateDescriptorSets + set allocation are skipped and only
  // fresh pDynamicOffsets are bound. constants_descriptor_set_valid_ is reset to
  // false at every point where the cached transient set can become invalid (new
  // frame / transient-pool reset / used->free reclaim) so a stale handle can
  // never let the GPU read recycled or freed descriptor memory. Only meaningful
  // when use_dynamic_constants_ is true.
  //
  // The range is tracked alongside the buffer because the dynamic path only
  // re-binds pDynamicOffsets per draw; the descriptor's range is baked at write
  // time. The float-constant buffers' sizes vary with the shader's float_count
  // (see UpdateBindings), and the upload pool returns the SAME page (only a new
  // offset) when the new size still fits, so a buffer-only rollover check would
  // keep a stale, too-small range and the shader's higher-index float constants
  // would read as clamped/zero -> wrong transforms -> flicker, with no VUID
  // (dynamicOffset + stale_range still fits the buffer). Comparing the range
  // forces the cheap re-write on a constant-count change (a shader switch, far
  // rarer than per-draw constant value changes).
  VkBuffer constants_descriptor_set_buffers_
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  VkDeviceSize constants_descriptor_set_ranges_
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  bool constants_descriptor_set_valid_ = false;
  VkDescriptorSet current_graphics_descriptor_sets_
      [SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  struct TextureDescriptorState {
    const VulkanShader* shader = nullptr;
    uint32_t textures = 0, samplers = 0;
    uint64_t images = 0, destroyed_samplers = 0, sampler_bindings = 0;
  } texture_descriptor_state_[2];
  uint64_t sampler_binding_generation_[2] = {};
  uint32_t samplers_used_fetch_mask_[2] = {};
  bool current_texture_descriptors_valid_vertex_ = false;
  bool current_texture_descriptors_valid_pixel_ = false;
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  DrawFloatConstants float_constants_[2];
  uint32_t bool_loop_constants_[40] = {};
  uint64_t bindings_fetch_generation_ = 0;

  // System shader constants, including user clip planes and tessellation
  // constants.
  SpirvShaderTranslator::SystemConstants system_constants_;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  // Device-local memexport output copied to guest RAM once its submission
  // completes (vulkan_memexport_ram_mirror), so a later partial CPU write to
  // such a page doesn't reload stale RAM over the GPU-only bytes.
  struct MemexportMirrorRange {
    uint64_t submission;
    uint32_t address;
    uint32_t length;
  };
  std::deque<MemexportMirrorRange> memexport_mirror_pending_;
  bool memexport_mirror_barrier_needed_ = false;
  uint64_t memexport_mirror_pages_copied_ = 0;
  uint64_t memexport_mirror_pages_skipped_ = 0;
  void QueueMemexportRamMirror(bool route_to_host);
  void ProcessMemexportRamMirror(uint64_t completed_submission);
  void MirrorMemexportPage(uint32_t page, uint64_t completed_submission);
  std::vector<uint32_t> memexport_mirror_pages_scratch_;
  // log_memexport_ram_mirror, reset every report.
  struct MemexportMirrorStats {
    uint64_t ranges = 0, pages = 0, copied = 0, bytes = 0;
    uint32_t frames = 0;
  } memexport_mirror_stats_;

  // Backend-agnostic resolve-to-guest-RAM copy decisions and read-watch
  // consumption tracking, shared with the D3D12 backend.
#include "../command_processor_resolve_readwatch.inc"
  // readback_resolve=uma: last submission that resolved each destination key,
  // so the direct host read only happens once that submission has retired.
  std::unordered_map<uint64_t, uint64_t> uma_readback_last_write_;
  // Per-backend trampoline from the memory read callback into the shared
  // MarkResolvePagesRead.
  static void ResolveReadCallbackThunk(void* context, uint32_t physical_address,
                                       uint32_t length);

  // Debug marker support for RenderDoc/debug tools.
  bool debug_markers_enabled_ = false;
  void UpdateDebugMarkersEnabled();

  // RTTRACE one-shot diagnostic state (cvar vulkan_rt_trace_request). All of
  // this is inert (never read or written) when the cvar is empty.
  struct RtTraceSignature {
    // Accumulated render targets bound for the draw (0 depth, 1..4 color),
    // type-erased (RenderTarget is protected in RenderTargetCache) - pointer
    // identity alone is enough to detect a rebind, since a given host
    // RenderTarget's key never changes after creation. See
    // VulkanRenderTargetCache::GetRtTraceAccumulatedRenderTargets /
    // FormatRtTraceKey.
    const void* rts[1 + xenos::kMaxColorRenderTargets] = {};
    uint32_t scissor_tl_x = 0, scissor_tl_y = 0, scissor_br_x = 0,
             scissor_br_y = 0;
    int32_t woff_x = 0, woff_y = 0;
    bool woff_en = false;
    float vp_xscale = 0.0f, vp_xoffset = 0.0f, vp_yscale = 0.0f,
          vp_yoffset = 0.0f;
    uint32_t vte_raw = 0;
    uint32_t cmask = 0;
    uint32_t zctl = 0;
    bool rast = false;
    xenos::PrimitiveType prim_type = xenos::PrimitiveType::kNone;
    bool pnull = false;
    bool operator!=(const RtTraceSignature& other) const {
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        if (rts[i] != other.rts[i]) return true;
      }
      return scissor_tl_x != other.scissor_tl_x ||
             scissor_tl_y != other.scissor_tl_y ||
             scissor_br_x != other.scissor_br_x ||
             scissor_br_y != other.scissor_br_y || woff_x != other.woff_x ||
             woff_y != other.woff_y || woff_en != other.woff_en ||
             vp_xscale != other.vp_xscale || vp_xoffset != other.vp_xoffset ||
             vp_yscale != other.vp_yscale || vp_yoffset != other.vp_yoffset ||
             vte_raw != other.vte_raw || cmask != other.cmask ||
             zctl != other.zctl || rast != other.rast ||
             prim_type != other.prim_type || pnull != other.pnull;
    }
  };
  bool rt_trace_active_ = false;
  // One-shot: once a traced frame has ended, never arms again.
  bool rt_trace_done_ = false;
  uint64_t rt_trace_frame_ = 0;
  uint32_t rt_trace_draw_count_ = 0;
  uint32_t rt_trace_run_count_ = 0;
  bool rt_trace_run_open_ = false;
  uint32_t rt_trace_run_first_draw_ = 0;
  uint32_t rt_trace_run_draws_ = 0;
  uint32_t rt_trace_run_prim_count_ = 0;
  RtTraceSignature rt_trace_run_sig_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
