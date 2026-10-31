#ifndef XENIA_GPU_VULKAN_RENDER_TARGET_RETIREMENT_H_
#define XENIA_GPU_VULKAN_RENDER_TARGET_RETIREMENT_H_

#include <cstdint>

namespace xe {
namespace gpu {
namespace vulkan {

constexpr uint64_t kRenderTargetRetirementIdleGraceFrames = 60;

// Match the complete backend RT identity in a cached framebuffer slot. A
// base-only match is insufficient because distinct depth/format/scale keys can
// share the same EDRAM base and dimensions.
template <typename Key>
bool FramebufferReferencesRenderTarget(const Key* attachment_keys,
                                       uint32_t attachment_used_mask,
                                       uint32_t attachment_count,
                                       const Key& render_target_key) {
  if (!attachment_keys || attachment_count > 32) {
    return true;
  }
  for (uint32_t i = 0; i < attachment_count; ++i) {
    if ((attachment_used_mask & (uint32_t(1) << i)) &&
        attachment_keys[i] == render_target_key) {
      return true;
    }
  }
  return false;
}

constexpr bool SubmissionReferenceComplete(bool known,
                                           uint64_t last_reference,
                                           uint64_t completed_submission) {
  return known && last_reference <= completed_submission;
}

constexpr bool ReferenceIdleForFrames(bool known, uint64_t last_reference_frame,
                                      uint64_t current_frame,
                                      uint64_t grace_frames) {
  return known && current_frame >= last_reference_frame &&
         current_frame - last_reference_frame >= grace_frames;
}

inline void RecordSubmissionReference(bool& known, uint64_t& last_reference,
                                      uint64_t submission) {
  if (!known || submission > last_reference) {
    last_reference = submission;
  }
  known = true;
}

// Explicit leases cover non-RTC code that keeps an image, view, descriptor,
// or framebuffer alias beyond the call that obtained it. Misbalanced or
// saturated counts become unknown and pin the object permanently.
class RenderTargetAliasLeases {
 public:
  bool Acquire() {
    if (count_ == ~uint32_t(0)) {
      known_ = false;
      return false;
    }
    ++count_;
    return true;
  }

  void Release() {
    if (!count_) {
      known_ = false;
      return;
    }
    --count_;
  }

  bool known() const { return known_; }
  uint32_t count() const { return count_; }

 private:
  uint32_t count_ = 0;
  bool known_ = true;
};

// Every "known" field must be established from live cache/queue state before
// retirement. Defaults deliberately fail closed for uncollected evidence.
struct RenderTargetRetirementEvidence {
  bool ownership_known = false;
  bool has_edram_owner = true;
  bool cpu_references_known = false;
  bool has_cpu_references = true;
  bool framebuffer_dependencies_known = false;
  bool has_framebuffer_dependencies = true;
  bool alias_leases_known = false;
  uint32_t external_alias_leases = 1;
  bool submission_fence_known = false;
  uint64_t last_gpu_reference_submission = ~uint64_t(0);
  uint64_t completed_submission = 0;
  bool idle_grace_known = false;
  uint64_t last_gpu_reference_frame = 0;
  uint64_t current_frame = 0;
  uint64_t idle_grace_frames = kRenderTargetRetirementIdleGraceFrames;
  bool current_recording_known = false;
  bool has_current_recording_reference = true;

  constexpr bool CanRetireBeforeFramebufferInvalidation() const {
    return ownership_known && !has_edram_owner && cpu_references_known &&
           !has_cpu_references && alias_leases_known &&
           external_alias_leases == 0 && submission_fence_known &&
           last_gpu_reference_submission <= completed_submission &&
           ReferenceIdleForFrames(idle_grace_known,
                                 last_gpu_reference_frame, current_frame,
                                 idle_grace_frames) &&
           current_recording_known && !has_current_recording_reference;
  }

  constexpr bool CanRetire() const {
    return CanRetireBeforeFramebufferInvalidation() &&
           framebuffer_dependencies_known && !has_framebuffer_dependencies;
  }
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_RENDER_TARGET_RETIREMENT_H_
