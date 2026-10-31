/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shared_memory.h"

#include <dlfcn.h>
#include <unwind.h>

#include <algorithm>
#include <cstring>
#include <chrono>

#include "xenia/base/assert.h"
#include "xenia/base/cp_wait_stats.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(gpu_allow_invalid_upload_range);
DECLARE_uint32(log_frame_spikes_ms);
DECLARE_bool(memexport_enable);
DECLARE_bool(log_texture_clobber);
DECLARE_int32(log_render_pass_breaks);
DECLARE_bool(shared_memory_zero_copy);
DECLARE_string(readback_resolve);
DECLARE_bool(log_cp_wait_stats);
DECLARE_bool(shared_memory_unwatch_dynamic_pages);
DECLARE_bool(vulkan_pass_head_overlap);

DEFINE_bool(log_depth_materialize_callers, false,
            "With vulkan_texture_from_render_target, add the call stacks "
            "(module offsets) that materialized depth surfaces to the "
            "VkRTTexture log line.",
            "GPU");

namespace {
struct MaterializeBacktrace {
  uintptr_t pcs[8];
  int count = 0;
};
_Unwind_Reason_Code CollectMaterializeFrame(_Unwind_Context* context,
                                            void* arg) {
  auto& trace = *static_cast<MaterializeBacktrace*>(arg);
  if (trace.count >= int(xe::countof(trace.pcs))) {
    return _URC_END_OF_STACK;
  }
  trace.pcs[trace.count++] = _Unwind_GetIP(context);
  return _URC_NO_REASON;
}
}  // namespace

DEFINE_bool(vulkan_sparse_shared_memory, true,
            "Enable sparse binding for shared memory emulation. Disabling it "
            "increases video memory usage - a 512 MB buffer is created - but "
            "allows graphics debuggers that don't support sparse binding to "
            "work.",
            "Vulkan");

DEFINE_bool(vulkan_hoist_shmem_uploads, true,
            "Record shared-memory uploads whose pages were not invalidated "
            "since the current submission opened at the start of the "
            "submission's command buffer instead of breaking the current "
            "render pass (expensive on tile-based GPUs).",
            "Vulkan");

DEFINE_bool(vulkan_hoist_unused_shmem_uploads, true,
            "With vulkan_hoist_shmem_uploads, also move uploads of pages the "
            "CPU changed while the submission was recording to its start, as "
            "long as no recorded command accesses them - instead of ending "
            "the render pass for each (the main cost on tile-based GPUs).",
            "Vulkan");

DEFINE_uint32(vulkan_vfetch_inpass_ring_mb, 0,
              "Size of the ring (placed in the host-mapped shared memory "
              "buffer past the guest memory) that holds CPU copies of vertex "
              "buffers the guest rewrote after an earlier draw of the open "
              "render pass read them. The draw reads the copy instead of "
              "ending the pass for the upload (a full tile store and reload "
              "on tile-based GPUs). 0 disables. Off: on BO such in-pass rewrites "
              "are only ~0.3 per frame (the pass breaks attributed to shared "
              "memory are resolves), not worth 16 MB.",
              "Vulkan");

DEFINE_bool(vulkan_shared_memory_host_visible, true,
            "On unified-memory GPUs (Adreno, integrated), allocate the "
            "shared-memory buffer from a host-visible "
            "cached memory type and map it so resolve readback "
            "(readback_resolve=uma) can read guest memory directly without a "
            "GPU staging copy. Measured on Adreno 830 / Turnip in "
            "TDU2: +11.6% fps (22.6 -> 25.2, p<0.001), from dropping ~153 "
            "staging copies per frame. Pass breaks shift (resolve readback "
            "breaks out, texture-upload breaks in) but net out, and per-"
            "submission GPU time barely moves - the win is bandwidth, not "
            "pass structure.",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

VulkanSharedMemory::VulkanSharedMemory(
    VulkanCommandProcessor& command_processor, Memory& memory,
    TraceWriter& trace_writer,
    VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() { Shutdown(true); }

bool VulkanSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Zero-copy: alias guest RAM as the buffer. On success the buffer is a single
  // fully-resident non-sparse allocation backed by guest RAM, so the normal
  // sparse/device-local paths below are skipped.
  if (cvars::shared_memory_zero_copy && TryInitializeZeroCopy()) {
    last_usage_ = Usage::kTransferDestination;
    last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);
    upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
        vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                  size_t(1) << page_size_log2()));
    return true;
  }

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
      VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // When a dedicated transfer queue is used for resolve readback, the buffer is
  // read from both the graphics/compute and transfer families, so it must be
  // shared concurrently between them (no compression penalty for a buffer).
  const uint32_t transfer_family = vulkan_device->queue_family_transfer();
  const uint32_t concurrent_queue_families[2] = {
      vulkan_device->queue_family_graphics_compute(), transfer_family};

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (transfer_family != UINT32_MAX) {
    buffer_create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_create_info.queueFamilyIndexCount = 2;
    buffer_create_info.pQueueFamilyIndices = concurrent_queue_families;
  } else {
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices = nullptr;
  }
  // readback_resolve=uma reads guest RAM through this buffer's host mapping,
  // and only the dense buffer below can be mapped, so uma overrides sparse.
  const bool needs_host_mapping = cvars::readback_resolve == "uma";
  if (needs_host_mapping && cvars::vulkan_sparse_shared_memory) {
    XELOGI(
        "Shared memory: sparse binding disabled so the buffer can be "
        "host-mapped for readback_resolve=uma");
  }
  if (cvars::vulkan_sparse_shared_memory &&
      !needs_host_mapping &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) ==
        VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                        &buffer_memory_requirements);
      if (xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                   vulkan_device->memory_types().device_local,
                               &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        xe::bit_scan_forward(
            std::max(uint64_t(buffer_memory_requirements.alignment),
                     uint64_t(1)),
            &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(
              std::max(allocation_size_log2,
                       std::max(kHostGpuMemoryOptimalSparseAllocationLog2,
                                kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        XELOGE(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
             kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    XELOGGPU(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    // The in-pass vertex fetch ring lives past the guest memory.
    const uint32_t vfetch_ring_reserve =
        cvars::vulkan_vfetch_inpass_ring_mb
            ? std::min(cvars::vulkan_vfetch_inpass_ring_mb, UINT32_C(64)) << 20
            : 0;
    buffer_create_info.size = VkDeviceSize(kBufferSize) + vfetch_ring_reserve;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan buffer",
             kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                      &buffer_memory_requirements);
    const ui::vulkan::VulkanDevice::MemoryTypes& memory_types =
        vulkan_device->memory_types();
    const uint32_t buffer_memory_type_bits =
        buffer_memory_requirements.memoryTypeBits;
    // Map for direct CPU readback when a host-visible type is available, so
    // resolve readback needs no staging copy. Prefer cached-coherent, accept
    // cached non-coherent; never map uncached (CPU reads are slower than
    // staging). Test this buffer's own candidate types - a whole-device test
    // fails on Adreno, whose LAZILY_ALLOCATED type is not host-visible.
    const bool is_uma =
        memory_types.device_local &&
        (memory_types.device_local & memory_types.host_visible) ==
            memory_types.device_local;
    bool buffer_host_visible = false;
    bool buffer_host_coherent = false;
    if (cvars::vulkan_shared_memory_host_visible) {
      const uint32_t cached_coherent = buffer_memory_type_bits &
                                       memory_types.device_local &
                                       memory_types.host_visible &
                                       memory_types.host_cached &
                                       memory_types.host_coherent;
      const uint32_t cached = buffer_memory_type_bits &
                              memory_types.device_local &
                              memory_types.host_visible &
                              memory_types.host_cached;
      if (xe::bit_scan_forward(cached_coherent, &buffer_memory_type_)) {
        buffer_host_visible = true;
        buffer_host_coherent = true;
      } else if (xe::bit_scan_forward(cached, &buffer_memory_type_)) {
        buffer_host_visible = true;
      }
    }
    XELOGI(
        "Shared memory: host-map decision: cvar={} is_uma={} "
        "type_bits={:#x} device_local={:#x} host_visible={:#x} "
        "host_cached={:#x} host_coherent={:#x} -> host_visible={} coherent={}",
        cvars::vulkan_shared_memory_host_visible, is_uma,
        buffer_memory_type_bits, memory_types.device_local,
        memory_types.host_visible, memory_types.host_cached,
        memory_types.host_coherent, buffer_host_visible, buffer_host_coherent);

    if (!buffer_host_visible &&
        !xe::bit_scan_forward(
            buffer_memory_type_bits & memory_types.device_local,
            &buffer_memory_type_)) {
      XELOGE(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last =
        &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize =
        buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext =
          &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(
              &buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr,
                             &buffer_memory) != VK_SUCCESS) {
      XELOGE(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }

    // Persistently map the buffer for direct CPU readback (readback_resolve=uma)
    // when it landed on a host-visible type.
    if (buffer_host_visible) {
      void* mapped_data;
      if (dfn.vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0,
                          &mapped_data) == VK_SUCCESS) {
        host_mapped_data_ = static_cast<uint8_t*>(mapped_data);
        host_mapped_coherent_ = buffer_host_coherent;
        vfetch_ring_size_ = vfetch_ring_reserve;
        vfetch_ring_memory_size_ = buffer_memory_allocate_info.allocationSize;
        XELOGGPU(
            "Shared memory: buffer is host-mapped for direct readback "
            "(readback_resolve=uma available), coherent={}",
            buffer_host_coherent ? "yes" : "no");
      } else {
        XELOGW(
            "Shared memory: failed to map the host-visible buffer; direct "
            "readback (uma) disabled");
      }
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                size_t(1) << page_size_log2()));

  // Second (host-imported) buffer for memexport-touching draws on the
  // device-local path, so their output stays coherent with the CPU.
  TryInitializeHostBuffer();

  return true;
}

uint32_t VulkanSharedMemory::GetDenseShaderBufferCountLog2() const {
  const uint32_t max_storage_buffer_range =
      command_processor_.GetVulkanDevice()->properties().maxStorageBufferRange;
  if (max_storage_buffer_range >= (512u << 20)) {
    return 0;
  }
  if (max_storage_buffer_range >= (256u << 20)) {
    return 1;
  }
  return 2;
}

uint32_t VulkanSharedMemory::GetShaderBufferCount() const {
  return 1u << GetDenseShaderBufferCountLog2();
}

bool VulkanSharedMemory::CanPublishDepthSurface(uint32_t length) const {
  return host_mapped_data_ && !zero_copy_ &&
         CPUReadRangeEpoch(0, 1) != UINT64_MAX &&
         depth_provider_stats_.pending_bytes + length <= (64u << 20);
}

void VulkanSharedMemory::DepthReadCallback(
    void* context, global_unique_lock_type& lock, uint32_t start, uint32_t length) {
  auto& shared = *static_cast<VulkanSharedMemory*>(context);
  if (shared.command_processor_.IsWorkerThread()) {
    // CP physical aliases must use PrepareForCpuAccess BEFORE dereferencing.
    // An unexpected CP fault cannot run Vulkan/allocators inside SIGSEGV, nor
    // wait for itself. Fail closed instead of exposing stale guest bytes.
    for (const auto& s : shared.depth_surfaces_)
      if (s.pending && s.start < uint64_t(start) + length &&
          uint64_t(s.start) + s.length > start) std::terminate();
    return;
  }
  shared.AwaitDepthSurfaces(lock, start, length);
}

void VulkanSharedMemory::AwaitDepthSurfaces(
    global_unique_lock_type& lock, uint32_t start, uint32_t length) {
  assert_true(lock.owns_lock());
  if (command_processor_.IsWorkerThread()) {
    lock.unlock();
    MaterializeDepthSurfaces(start, length);
    lock.lock();
    return;
  }
  depth_cpu_waiters_.fetch_add(1, std::memory_order_relaxed);
  struct Waiter {
    std::atomic<uint32_t>& count;
    ~Waiter() { count.fetch_sub(1, std::memory_order_release); }
  } waiter{depth_cpu_waiters_};
  for (;;) {
    bool pending = false;
    for (auto& surface : depth_surfaces_) {
      if (surface.pending && surface.start < uint64_t(start) + length &&
          uint64_t(surface.start) + surface.length > start) {
        surface.requested = pending = true;
      }
    }
    if (!pending) return;
    // No Vulkan, allocator, CP function queue or driver wait in the fault
    // callback. The worker polls this flag even inside WAIT_REG_MEM/IBs.
    depth_request_pending_.store(true, std::memory_order_release);
    lock.unlock();
    while (depth_request_pending_.load(std::memory_order_acquire))
      std::this_thread::yield();
    lock.lock();
    // Another resolve may have published while this thread was unlocked.
  }
}

void VulkanSharedMemory::PrepareForCpuAccess(uint32_t start, uint32_t length) {
  auto lock = global_critical_region::AcquireDirect();
  AwaitDepthSurfaces(lock, start, length);
}

void VulkanSharedMemory::ServiceDepthRequests() {
  if (!depth_request_pending_.load(std::memory_order_acquire) ||
      depth_materializing_) return;
  auto lock = global_critical_region::AcquireDirect();
  // Materialize scans without keeping iterators over GPU completion callbacks.
  // Clearing the flag while holding the lock cannot lose a CPU request.
  for (;;) {
    auto it = std::find_if(depth_surfaces_.begin(), depth_surfaces_.end(),
        [](const auto& s) { return s.pending && s.requested; });
    if (it == depth_surfaces_.end()) break;
    const uint32_t start = it->start, length = it->length;
    lock.unlock();
    MaterializeDepthSurfaces(start, length);
    lock.lock();
  }
  depth_request_pending_.store(false, std::memory_order_release);
}

void VulkanSharedMemory::MaterializeDepthSurfaces(uint32_t start,
                                                 uint32_t length,
                                                 bool for_gpu) {
  if (!length || start >= kBufferSize) return;
  // Every RequestRange comes here; most ranges are far from any resolve.
  if (command_processor_.IsWorkerThread() &&
      (start >= depth_pending_hi_ ||
       uint64_t(start) + length <= depth_pending_lo_)) {
    return;
  }
  auto lock = global_critical_region::AcquireDirect();
  if (depth_materializing_) return;
  for (size_t i = 0; i < depth_surfaces_.size(); ++i) {
    if (!depth_surfaces_[i].pending || depth_surfaces_[i].start >= uint64_t(start) + length ||
        uint64_t(depth_surfaces_[i].start) + depth_surfaces_[i].length <= start) continue;
    if (for_gpu && depth_surfaces_[i].gpu_materialized) continue;
    // Non-CP access must use AwaitDepthSurfaces with a once-held lock. Physical
    // faults have already run that provider before invalidation reaches here.
    if (!command_processor_.IsWorkerThread()) std::terminate();
    if (cvars::log_depth_materialize_callers)
      NoteMaterializeCaller(depth_surfaces_[i].start, depth_surfaces_[i].length,
                            start, length);
    depth_materializing_ = true;
    const uint32_t base = depth_surfaces_[i].start, size = depth_surfaces_[i].length;
    if (for_gpu) {
      // Recorded before the GPU work that needs the bytes; guest RAM stays
      // stale behind the record's CPU protection.
      lock.unlock();
      const bool recorded = depth_surfaces_[i].materialize(false);
      lock.lock();
      if (!recorded) std::terminate();
      depth_surfaces_[i].gpu_materialized = true;
      depth_surfaces_[i].materialize = {};
      depth_surfaces_[i].discard = {};
      MakeRangeValid(base, size, true);
      ++depth_provider_stats_.gpu_materializations;
      depth_materializing_ = false;
      continue;
    }
    // Only the CP can retire records. CPU writers still see a pending
    // provider and wait; no vector references survive the unlocked GPU drain.
    lock.unlock();
    bool completed;
    if (depth_surfaces_[i].gpu_materialized) {
      // Already in the stream: just finish it.
      completed = command_processor_.EndSubmission(false) &&
                  command_processor_.AwaitAllQueueOperationsCompletion() &&
                  command_processor_.BeginSubmission(false);
    } else {
      completed = depth_surfaces_[i].materialize(true);
    }
    lock.lock();
    if (!completed) std::terminate();
    // Compute has completed with a shader->host barrier. Other guest threads
    // cannot overwrite these still-protected pages before this commit.
    // Through ReadHostMapped: on non-coherent host-visible memory (Mali) the
    // mapping must be invalidated before the CPU reads what the compute wrote.
    if (!ReadHostMapped(base, size, memory().physical_membase() + base)) {
      std::terminate();
    }
    depth_surfaces_[i].pending = false;
    depth_surfaces_[i].materialize = {};
    depth_surfaces_[i].discard = {};
    MakeRangeValid(base, size, true);
    ++depth_provider_stats_.materializations;
    depth_provider_stats_.cpu_accesses += depth_surfaces_[i].requested;
    depth_provider_stats_.materialized_bytes += size;
    depth_provider_stats_.pending_bytes -= size;
    depth_materializing_ = false;
  }
  // PrepareForPause and explicit CPU reads may satisfy a waiter without
  // entering ServiceDepthRequests; wake it before the CP can park.
  if (std::none_of(depth_surfaces_.begin(), depth_surfaces_.end(),
      [](const auto& s) { return s.pending && s.requested; }))
    depth_request_pending_.store(false, std::memory_order_release);
}

void VulkanSharedMemory::NoteMaterializeCaller(uint32_t record_start,
                                               uint32_t record_length,
                                               uint32_t request_start,
                                               uint32_t request_length) {
  MaterializeBacktrace trace;
  _Unwind_Backtrace(CollectMaterializeFrame, &trace);
  Dl_info info;
  const uintptr_t base =
      dladdr(reinterpret_cast<void*>(&CollectMaterializeFrame), &info)
          ? reinterpret_cast<uintptr_t>(info.dli_fbase)
          : 0;
  // Frames 0-1 are this function and MaterializeDepthSurfaces.
  std::array<uint32_t, 6> stack = {};
  for (int i = 2; i < trace.count && i - 2 < int(stack.size()); ++i) {
    stack[i - 2] = uint32_t(trace.pcs[i] - base);
  }
  for (auto& slot : materialize_callers_) {
    if (!slot.count) {
      slot = {stack, 1, record_start, record_length, request_start,
              request_length};
      return;
    }
    if (slot.stack == stack) {
      ++slot.count;
      return;
    }
  }
}

std::string VulkanSharedMemory::TakeMaterializeCallers() {
  std::string result;
  for (auto& slot : materialize_callers_) {
    if (!slot.count) {
      continue;
    }
    result += fmt::format(" {}:", slot.count);
    for (uint32_t pc : slot.stack) {
      if (pc) result += fmt::format("{:X}/", pc);
    }
    result += fmt::format("[rec {:08X}+{:X} req {:08X}+{:X}]", slot.record_start,
                          slot.record_length, slot.request_start,
                          slot.request_length);
    slot = {};
  }
  return result;
}

void VulkanSharedMemory::OnCpuRangeAccess(uint32_t start, uint32_t length) {
  PrepareForCpuAccess(start, length);
}

bool VulkanSharedMemory::OnRangeRequested(uint32_t start, uint32_t length) {
  MaterializeDepthSurfaces(start, length, true);
  return true;
}

bool VulkanSharedMemory::IsRangeValidInSurface(uint32_t start,
                                              uint32_t length) const {
  if (!length || start >= kBufferSize || length > kBufferSize - start) return false;
  auto lock = global_critical_region::AcquireDirect();
  for (const auto& s : depth_surfaces_) {
    if (s.pending && !s.gpu_materialized && start >= s.start &&
        uint64_t(start) + length <= uint64_t(s.start) + s.length) return true;
  }
  return false;
}

bool VulkanSharedMemory::DiscardOverwrittenDepthSurface(
    uint32_t start, uint32_t length, uint64_t geometry) {
  auto lock = global_critical_region::AcquireDirect();
  if (depth_materializing_) return false;
  for (auto& s : depth_surfaces_) {
    // A requested record has a CPU waiter that must see the older resolve.
    if (!s.pending || s.requested || s.start != start || s.length != length ||
        s.geometry != geometry) continue;
    // The buffer pages stay surface-only (invalid): the caller publishes the
    // new resolve over them, or its compute fallback re-requests the range.
    s.pending = false;
    s.materialize = {};
    if (s.discard) s.discard();
    s.discard = {};
    s.gpu_materialized = false;
    ++depth_provider_stats_.discarded;
    depth_provider_stats_.discarded_bytes += length;
    depth_provider_stats_.pending_bytes -= length;
    return true;
  }
  return false;
}

uint32_t VulkanSharedMemory::DiscardDepthSurfacesWithin(uint32_t start,
                                                        uint32_t length) {
  auto lock = global_critical_region::AcquireDirect();
  if (depth_materializing_) return 0;
  uint32_t discarded = 0;
  for (auto& s : depth_surfaces_) {
    if (!s.pending || s.requested || s.start < start ||
        uint64_t(s.start) + s.length > uint64_t(start) + length) {
      continue;
    }
    s.pending = false;
    s.materialize = {};
    if (s.discard) s.discard();
    s.discard = {};
    s.gpu_materialized = false;
    ++depth_provider_stats_.discarded;
    depth_provider_stats_.discarded_bytes += s.length;
    depth_provider_stats_.pending_bytes -= s.length;
    ++discarded;
  }
  return discarded;
}

void VulkanSharedMemory::PublishDepthSurface(
    uint32_t start, uint32_t length, uint64_t geometry,
    std::function<bool(bool wait)> materialize,
    std::function<void()> discard) {
  auto lock = global_critical_region::AcquireDirect();
  assert_true(CanPublishDepthSurface() && length && start < kBufferSize &&
              length <= kBufferSize - start);
  // RequestRange has materialized previous overlapping providers and padding.
  for (const auto& s : depth_surfaces_)
    assert_true(!s.pending || uint64_t(start) + length <= s.start ||
                uint64_t(s.start) + s.length <= start);
  depth_surfaces_.erase(std::remove_if(depth_surfaces_.begin(), depth_surfaces_.end(),
      [](const auto& s) { return !s.pending; }), depth_surfaces_.end());
  if (!depth_read_callback_)
    depth_read_callback_ = memory().RegisterPhysicalMemoryAccessProvider(DepthReadCallback, this);
  depth_surfaces_.push_back(
      {start, length, geometry, std::move(materialize), std::move(discard)});
  depth_pending_lo_ = UINT32_MAX;
  depth_pending_hi_ = 0;
  for (const auto& s : depth_surfaces_) {
    depth_pending_lo_ = std::min(depth_pending_lo_, s.start);
    depth_pending_hi_ = std::max(depth_pending_hi_, s.start + s.length);
  }
  MakeRangeValidInSurfaceOnly(start, length);
  memory().EnablePhysicalMemoryAccessCallbacks(start, length, true, true);
  ++depth_provider_stats_.published;
  depth_provider_stats_.published_bytes += length;
  depth_provider_stats_.pending_bytes += length;
}

VulkanSharedMemory::DepthProviderStats
VulkanSharedMemory::TakeDepthProviderStats() {
  auto lock = global_critical_region::AcquireDirect();
  const auto stats = depth_provider_stats_;
  depth_provider_stats_ = {};
  depth_provider_stats_.pending_bytes = stats.pending_bytes;
  return stats;
}

bool VulkanSharedMemory::IsHostMapped() const {
  return host_mapped_data_ != nullptr;
}

VulkanSharedMemory::BufferSliceList VulkanSharedMemory::GetBufferSlices(
    uint32_t guest_address, uint32_t length) {
  BufferSliceList slices;
  if (!length || guest_address >= kBufferSize ||
      length > kBufferSize - guest_address) {
    return slices;
  }

  if (buffer_ != VK_NULL_HANDLE) {
    slices.slices[slices.count++] =
        {buffer_, guest_address, guest_address, length};
  }
  return slices;
}

bool VulkanSharedMemory::CreateImportedGuestRamBuffer(
    VkBuffer& out_buffer, VkDeviceMemory& out_memory) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties =
      vulkan_device->vkGetMemoryHostPointerPropertiesEXT();
  if (!vulkan_device->extensions().ext_EXT_external_memory_host ||
      !get_host_pointer_properties) {
    XELOGI(
        "Shared memory host import: VK_EXT_external_memory_host not available");
    return false;
  }

  void* const guest_ram = memory().TranslatePhysical(0);
  if (!guest_ram) {
    XELOGE("Shared memory host import: guest RAM is null");
    return false;
  }

  // The host pointer and the imported size must both be aligned to the import
  // granularity.
  VkDeviceSize import_alignment =
      vulkan_device->properties().minImportedHostPointerAlignment;
  if (import_alignment == 0) {
    import_alignment = 1;
  }
  if ((reinterpret_cast<uintptr_t>(guest_ram) % import_alignment) != 0 ||
      (VkDeviceSize(kBufferSize) % import_alignment) != 0) {
    XELOGI(
        "Shared memory host import: guest RAM 0x{:X} not aligned to import "
        "granularity {}",
        reinterpret_cast<uintptr_t>(guest_ram), import_alignment);
    return false;
  }

  // Which memory types can back this exact host pointer.
  VkMemoryHostPointerPropertiesEXT host_pointer_properties = {
      VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (get_host_pointer_properties(
          device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
          guest_ram, &host_pointer_properties) != VK_SUCCESS) {
    XELOGI(
        "Shared memory host import: vkGetMemoryHostPointerPropertiesEXT "
        "failed");
    return false;
  }

  // A plain non-sparse buffer to bind the imported memory to. It must declare
  // the external handle type it will be bound to, or vkBindBufferMemory is
  // invalid (VUID-vkBindBufferMemory-memory-02985).
  const uint32_t transfer_family = vulkan_device->queue_family_transfer();
  const uint32_t concurrent_queue_families[2] = {
      vulkan_device->queue_family_graphics_compute(), transfer_family};
  VkExternalMemoryBufferCreateInfo external_memory_buffer_create_info;
  external_memory_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  external_memory_buffer_create_info.pNext = nullptr;
  external_memory_buffer_create_info.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = &external_memory_buffer_create_info;
  buffer_create_info.flags = 0;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (transfer_family != UINT32_MAX) {
    buffer_create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
    buffer_create_info.queueFamilyIndexCount = 2;
    buffer_create_info.pQueueFamilyIndices = concurrent_queue_families;
  } else {
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_create_info.queueFamilyIndexCount = 0;
    buffer_create_info.pQueueFamilyIndices = nullptr;
  }
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to create the buffer");
    return false;
  }

  VkMemoryRequirements buffer_memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer,
                                    &buffer_memory_requirements);

  // The memory type must satisfy the buffer, accept the host pointer, and be
  // host-visible + host-coherent so guest and GPU observe the same bytes.
  const uint32_t memory_type_bits = buffer_memory_requirements.memoryTypeBits &
                                    host_pointer_properties.memoryTypeBits &
                                    vulkan_device->memory_types().host_visible &
                                    vulkan_device->memory_types().host_coherent;
  uint32_t memory_type;
  if (!xe::bit_scan_forward(memory_type_bits, &memory_type)) {
    XELOGI(
        "Shared memory host import: no host-visible coherent memory type "
        "accepts the guest RAM pointer");
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  VkImportMemoryHostPointerInfoEXT import_info;
  import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
  import_info.pNext = nullptr;
  import_info.handleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  import_info.pHostPointer = guest_ram;

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = &import_info;
  memory_allocate_info.allocationSize = kBufferSize;
  memory_allocate_info.memoryTypeIndex = memory_type;

  VkDeviceMemory imported_memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr,
                           &imported_memory) != VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to import {} MB of guest RAM",
           kBufferSize >> 20);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  if (dfn.vkBindBufferMemory(device, buffer, imported_memory, 0) !=
      VK_SUCCESS) {
    XELOGE("Shared memory host import: failed to bind imported memory");
    dfn.vkFreeMemory(device, imported_memory, nullptr);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return false;
  }

  out_buffer = buffer;
  out_memory = imported_memory;
  // The import pins guest RAM - a later mprotect on the alias would fail the
  // next submit with EFAULT.
  memory().SetPhysicalAliasSkipHostProtect(true);
  return true;
}

bool VulkanSharedMemory::TryInitializeZeroCopy() {
  VkDeviceMemory buffer_memory;
  if (!CreateImportedGuestRamBuffer(buffer_, buffer_memory)) {
    buffer_ = VK_NULL_HANDLE;
    return false;
  }
  buffer_memory_.push_back(buffer_memory);
  zero_copy_ = true;
  XELOGI("Shared memory: using zero-copy guest RAM aliasing");
  return true;
}

void VulkanSharedMemory::TryInitializeHostBuffer() {
  if (!cvars::memexport_enable) {
    return;
  }
  // A second, host-imported (guest RAM) buffer used only for memexport-touching
  // draws while the main buffer stays fast device-local. Non-sparse, so it can
  // accept host memory where the sparse buffer can't.
  if (!CreateImportedGuestRamBuffer(host_buffer_, host_buffer_memory_)) {
    // Without it memexport output stays device-local and the CPU never sees it.
    XELOGW(
        "Shared memory: no host buffer for memexport - memexport_enable is set "
        "but the import failed, games reading exported data on the CPU will "
        "misbehave");
    host_buffer_ = VK_NULL_HANDLE;
    host_buffer_memory_ = VK_NULL_HANDLE;
    return;
  }
  XELOGI("Shared memory: host buffer for memexport ranges ready");
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  MaterializeDepthSurfaces(0, kBufferSize);
  {
    auto lock = global_critical_region::AcquireDirect();
    if (depth_read_callback_) {
      memory().UnregisterPhysicalMemoryAccessProvider(depth_read_callback_);
      depth_read_callback_ = nullptr;
    }
    depth_request_pending_.store(false, std::memory_order_release);
    lock.unlock();
    while (depth_cpu_waiters_.load(std::memory_order_acquire))
      std::this_thread::yield();
    lock.lock();
    depth_surfaces_.clear();
    depth_pending_lo_ = UINT32_MAX;
    depth_pending_hi_ = 0;
  }
  setup_upload_batches_.clear();
  setup_upload_batch_count_ = 0;
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (host_mapped_data_ != nullptr && !buffer_memory_.empty()) {
    dfn.vkUnmapMemory(device, buffer_memory_.front());
    host_mapped_data_ = nullptr;
    host_mapped_coherent_ = false;
  }

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();
  zero_copy_ = false;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         host_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         host_buffer_memory_);
  // No import left to pin the alias.
  memory().SetPhysicalAliasSkipHostProtect(false);

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::ClearCache() {
  MaterializeDepthSurfaces(0, kBufferSize);
  FlushSetupUploads();
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::FlushSetupUploads() {
  auto& setup = command_processor_.deferred_setup_command_buffer();
  for (size_t i = 0; i < setup_upload_batch_count_; ++i) {
    auto& batch = setup_upload_batches_[i];
    if (setup.pass_bytes_enabled()) {
      PassByteLedger::Event tag; tag.kind = PassByteLedger::kTag;
      tag.category = PassByteLedger::kUpload; setup.AnnotatePassBytes(tag);
    }
    setup.CmdVkCopyBuffer(batch.source, batch.destination,
                          uint32_t(batch.regions.size()), batch.regions.data());
    batch.regions.clear();
  }
  setup_upload_batch_count_ = 0;
}

void VulkanSharedMemory::EndSubmission() {
  FlushSetupUploads();
  upload_buffer_pool_->FlushWrites();
}

void VulkanSharedMemory::Use(Usage usage,
                             std::pair<uint32_t, uint32_t> written_range) {
  // Guest draws declare kGuestDrawReadWrite without a range up front for
  // in-pass resolves; those request their destination range themselves, so
  // the declaration alone must not materialize every pending surface (it did
  // so at the draw right after each publish, idling the GPU every time).
  if (usage != Usage::kRead && !depth_surfaces_.empty() &&
      (written_range.second || usage != Usage::kGuestDrawReadWrite)) {
    MaterializeDepthSurfaces(written_range.second ? written_range.first : 0,
                            written_range.second ? written_range.second
                                                 : kBufferSize,
                            true);
  }
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second =
      std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (usage == Usage::kComputeWrite) {
    // Without a range the whole buffer may be written.
    if (!written_range.second || vertex_debt_ranges_.size() >= 64) {
      vertex_debt_all_ = true;
      vertex_debt_ranges_.clear();
    } else if (!vertex_debt_all_ &&
               std::none_of(vertex_debt_ranges_.cbegin(),
                            vertex_debt_ranges_.cend(),
                            [&written_range](const auto& written) {
                              // The same resolve again, every frame.
                              return written.first <= written_range.first &&
                                     written.second >= uint64_t(
                                         written_range.first) +
                                         written_range.second;
                            })) {
      vertex_debt_ranges_.emplace_back(
          written_range.first, written_range.first + written_range.second);
      vertex_debt_start_ =
          std::min(vertex_debt_start_, uint64_t(written_range.first));
      vertex_debt_end_ = std::max(
          vertex_debt_end_,
          uint64_t(written_range.first) + written_range.second);
    }
  }

  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    constexpr VkPipelineStageFlags kVertexStages =
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    if (cvars::vulkan_pass_head_overlap) {
      // Earlier pixel shaders accessed the shared memory only if flagged.
      if (!command_processor_.FragmentSharedMemoryPending()) {
        src_stage_mask &=
            ~VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
      if (usage == Usage::kRead &&
          (dst_stage_mask & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) {
        dst_stage_mask &=
            ~VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        fragment_debt_ = true;
      }
    }
    if (cvars::vulkan_pass_head_overlap && usage == Usage::kRead &&
        last_usage_ == Usage::kComputeWrite) {
      // The vertex stages get the writes in SettleVertexReads, if they read
      // them.
      dst_stage_mask &= ~kVertexStages;
      dst_access_mask &= ~VkAccessFlags(VK_ACCESS_INDEX_READ_BIT);
      vertex_debt_ = true;
    } else if (last_usage_ != usage &&
               (dst_stage_mask & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)) {
      // A whole-buffer barrier into the vertex stages.
      ClearVertexDebt();
    }
    if (last_usage_ != usage &&
        (dst_stage_mask & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) {
      fragment_debt_ = false;
    }
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    if (cvars::log_render_pass_breaks > 0 &&
        command_processor_.in_render_pass()) {
      ++pass_diag_stats_.use_in_pass[uint32_t(last_usage_ == usage
                                                  ? usage
                                                  : last_usage_) &
                                     3][uint32_t(usage) & 3]
                                    [last_written_range_.second ? 1 : 0];
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask,
        dst_access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        false);
    // Memexport-touching draws are routed to the host-imported buffer, so their
    // writes and the reads that consume them go through host_buffer_, not
    // buffer_. Mirror the same ordering barrier onto it so those GPU
    // write->read dependencies are synchronized too.
    if (host_buffer_ != VK_NULL_HANDLE) {
      command_processor_.PushBufferMemoryBarrier(
          host_buffer_, offset, size, src_stage_mask, dst_stage_mask,
          src_access_mask, dst_access_mask, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
    }
  }
  last_written_range_ = written_range;
}

void VulkanSharedMemory::SettleFragmentReads() {
  if (!fragment_debt_) {
    return;
  }
  // Every stage that may have written it before the last barrier into kRead.
  const VkPipelineStageFlags src_stages =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
      guest_shader_pipeline_stages_;
  const VkAccessFlags src_access =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  for (VkBuffer buffer : {buffer_, host_buffer_}) {
    if (buffer != VK_NULL_HANDLE) {
      command_processor_.PushBufferMemoryBarrier(
          buffer, 0, VK_WHOLE_SIZE, src_stages,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, src_access,
          VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
    }
  }
  fragment_debt_ = false;
}

void VulkanSharedMemory::SettleVertexReads(
    const std::pair<uint32_t, uint32_t>* ranges, size_t range_count) {
  if (!vertex_debt_) {
    return;
  }
  bool overlap = vertex_debt_all_;
  for (size_t i = 0; !overlap && i < range_count; ++i) {
    const uint64_t start = ranges[i].first;
    const uint64_t end = start + ranges[i].second;
    if (start >= vertex_debt_end_ || vertex_debt_start_ >= end) {
      continue;
    }
    for (const auto& written : vertex_debt_ranges_) {
      if (start < written.second && written.first < end) {
        overlap = true;
        break;
      }
    }
  }
  if (!overlap) {
    return;
  }
  // The compute writes were made available by the barrier into kRead.
  const VkPipelineStageFlags vertex_stages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  const VkAccessFlags vertex_access =
      VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  command_processor_.PushBufferMemoryBarrier(
      buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      vertex_stages, VK_ACCESS_SHADER_WRITE_BIT, vertex_access,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  if (host_buffer_ != VK_NULL_HANDLE) {
    command_processor_.PushBufferMemoryBarrier(
        host_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vertex_stages, VK_ACCESS_SHADER_WRITE_BIT, vertex_access,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  ClearVertexDebt();
}

bool VulkanSharedMemory::WriteBackGpuWrittenToGuest(uint32_t& pages_written) {
  MaterializeDepthSurfaces(0, kBufferSize);
  pages_written = 0;
  // An imported buffer is guest RAM already.
  if (zero_copy_) return true;
  const auto runs = GetGpuWrittenRanges();
  if (runs.empty()) return true;
  if (!IsHostMapped()) {
    XELOGE("Snapshot: {} GPU-written ranges cannot be read back (buffer not host-mapped)",
           runs.size());
    return false;
  }
  uint8_t* guest = memory().physical_membase();
  for (const auto& run : runs) {
    // Whole pages in one copy; a partly GPU-written page only its GPU bytes.
    const uint32_t page_size = uint32_t(1) << page_size_log2();
    uint32_t copy_start = run.first;
    const uint32_t run_end = run.first + run.second;
    for (uint32_t page = run.first; page <= run_end; page += page_size) {
      std::pair<uint32_t, uint32_t> ranges[kGpuWrittenPartialRanges];
      const uint32_t count =
          page < run_end ? GpuWrittenPageRanges(page, ranges) : 0;
      if (count == 1 && ranges[0].first == page &&
          ranges[0].second == page + page_size) {
        continue;
      }
      if (page > copy_start &&
          !ReadHostMapped(copy_start, page - copy_start, guest + copy_start)) {
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) {
        if (!ReadHostMapped(ranges[i].first, ranges[i].second - ranges[i].first,
                            guest + ranges[i].first)) {
          return false;
        }
      }
      copy_start = page + page_size;
    }
    pages_written += run.second >> 12;
  }
  return true;
}

bool VulkanSharedMemory::ReadHostMapped(uint32_t guest_address, uint32_t length,
                                        void* dest, bool gpu_ordered) {
  MaterializeDepthSurfaces(guest_address, length, gpu_ordered);
  if (!dest || !length || guest_address >= kBufferSize ||
      length > kBufferSize - guest_address) {
    return false;
  }

  if (host_mapped_data_ == nullptr || !length) {
    return false;
  }
  if (uint64_t(guest_address) + length > uint64_t(kBufferSize)) {
    return false;
  }
  if (!host_mapped_coherent_) {
    // Cached but not coherent: pull the GPU-written bytes out of DRAM into the
    // CPU cache before reading them.
    const ui::vulkan::VulkanDevice* const vulkan_device =
        command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDeviceSize atom_size = vulkan_device->properties().nonCoherentAtomSize;
    VkMappedMemoryRange range;
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = nullptr;
    range.memory = buffer_memory_.front();
    range.offset = (VkDeviceSize(guest_address) / atom_size) * atom_size;
    range.size = std::min(xe::round_up(uint64_t(guest_address) + length,
                                       uint64_t(atom_size)),
                          uint64_t(kBufferSize)) -
                 range.offset;
    dfn.vkInvalidateMappedMemoryRanges(vulkan_device->device(), 1, &range);
  }
  memory::vastcpy(static_cast<uint8_t*>(dest), host_mapped_data_ + guest_address,
                  length);
  return true;
}

bool VulkanSharedMemory::InitializeTraceSubmitDownloads() {
  MaterializeDepthSurfaces(0, kBufferSize);
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(),
          download_page_count << page_size_log2(),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, trace_download_buffer_,
          trace_download_buffer_memory_)) {
    XELOGE(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }

  struct TraceDownloadBatch {
    VkBuffer source = VK_NULL_HANDLE;
    std::vector<VkBufferCopy> regions;
  };
  std::vector<TraceDownloadBatch> download_batches;
  VkDeviceSize download_buffer_offset = 0;
  for (const std::pair<uint32_t, uint32_t>& download_range :
       trace_download_ranges()) {
    if (!download_range.second || download_range.first >= kBufferSize ||
        download_range.second > kBufferSize - download_range.first) {
      XELOGE("Shared memory: invalid trace download range {:08X}+{:08X}",
             download_range.first, download_range.second);
      ResetTraceDownload();
      return false;
    }
    const BufferSliceList source_slices =
        GetBufferSlices(download_range.first, download_range.second);
    if (source_slices.empty()) {
      XELOGE("Shared memory: failed to resolve trace range {:08X}-{:08X}",
             download_range.first,
             download_range.first + download_range.second);
      ResetTraceDownload();
      return false;
    }
    for (const BufferSlice& source_slice : source_slices) {
      if (download_batches.empty() ||
          download_batches.back().source != source_slice.buffer) {
        download_batches.emplace_back();
        download_batches.back().source = source_slice.buffer;
      }
      VkBufferCopy& download_region =
          download_batches.back().regions.emplace_back();
      download_region.srcOffset = source_slice.buffer_offset;
      download_region.dstOffset = download_buffer_offset +
                                  (source_slice.guest_offset -
                                   download_range.first);
      download_region.size = source_slice.length;
    }
    download_buffer_offset += download_range.second;
  }

  Use(Usage::kRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  command_processor_.InsertDebugMarker(
      "Trace Download: %u KB, %zu ranges",
      download_page_count << page_size_log2() >> 10,
      trace_download_ranges().size());
  for (const TraceDownloadBatch& batch : download_batches) {
    command_buffer.CmdVkCopyBuffer(batch.source, trace_download_buffer_,
                                   uint32_t(batch.regions.size()),
                                   batch.regions.data());
  }

  command_processor_.PushBufferMemoryBarrier(
      trace_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);

  return true;
}

void VulkanSharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_memory_) {
    return;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* download_mapping;
  if (dfn.vkMapMemory(device, trace_download_buffer_memory_, 0, VK_WHOLE_SIZE,
                      0, &download_mapping) == VK_SUCCESS) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) +
              download_buffer_offset);
      // Submission packed the ranges consecutively into the download buffer.
      download_buffer_offset += download_range.second;
    }
    dfn.vkUnmapMemory(device, trace_download_buffer_memory_);
  } else {
    XELOGE(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize =
      length_allocations << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) !=
      VK_SUCCESS) {
    XELOGE("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations
                        << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::RangeUploadWouldBreakPass(uint32_t start,
                                                   uint32_t length,
                                                   uint32_t& span_start_out,
                                                   uint32_t& span_length_out) {
  if (!vfetch_ring_size_ || !command_processor_.in_render_pass()) {
    return false;
  }
  std::pair<uint32_t, uint32_t> ranges[16];
  bool cpu_sourced;
  uint32_t count =
      GetInvalidPageRanges(start, length, ranges, uint32_t(xe::countof(ranges)),
                           cpu_sourced);
  if (!count) {
    return false;
  }
  if (count == UINT32_MAX) {
    ++pass_diag_stats_.window_overflow;
    return false;
  }
  if (!cpu_sourced) {
    ++pass_diag_stats_.window_gpu_written;
    return false;
  }
  // Mirrors UploadRanges: these uploads keep the pass open by themselves.
  if (cvars::vulkan_hoist_shmem_uploads) {
    if (!AnyPageInvalidatedSinceSubmissionOpen(ranges, count)) {
      ++pass_diag_stats_.window_hoistable;
      return false;
    }
    if (cvars::vulkan_hoist_unused_shmem_uploads &&
        !AnyPageUsedSinceSubmissionOpen(ranges, count)) {
      ++pass_diag_stats_.window_hoistable;
      return false;
    }
  }
  if (command_processor_.pass_head_command_buffer() &&
      !AnyPageUsedInCurrentPass(ranges, count)) {
    ++pass_diag_stats_.window_pass_head;
    return false;
  }
  ++pass_diag_stats_.window_break;
  span_start_out = ranges[0].first << page_size_log2();
  span_length_out = (ranges[count - 1].first + ranges[count - 1].second -
                     ranges[0].first)
                    << page_size_log2();
  return true;
}

uint32_t VulkanSharedMemory::CopyRangeToVfetchRing(uint32_t start,
                                                   uint32_t length) {
  OnCpuRangeAccess(start, length);
  if (!vfetch_ring_size_ || !length || !host_mapped_data_) {
    return UINT32_MAX;
  }
  uint32_t size = xe::align(length, UINT32_C(256));
  if (size > vfetch_ring_size_ / 4) {
    ++vfetch_ring_full_;
    return UINT32_MAX;
  }
  auto* heap = memory().GetPhysicalHeap();
  if (heap->QueryRangeAccess(start, start + length - 1) ==
      xe::memory::PageAccess::kNoAccess) {
    return UINT32_MAX;
  }
  // Allocations are in submission order: free those the GPU has finished.
  uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!vfetch_ring_allocations_.empty() &&
         vfetch_ring_allocations_.front().submission <= completed) {
    vfetch_ring_allocations_.pop_front();
  }
  uint32_t offset = vfetch_ring_head_;
  if (offset + size > vfetch_ring_size_) {
    offset = 0;
  }
  for (const VfetchRingAllocation& allocation : vfetch_ring_allocations_) {
    if (offset < allocation.end && allocation.start < offset + size) {
      ++vfetch_ring_full_;
      return UINT32_MAX;
    }
  }
  uint32_t buffer_offset = kBufferSize + offset;
  std::memcpy(host_mapped_data_ + buffer_offset,
              memory().TranslatePhysical<const uint8_t*>(start), length);
  if (!host_mapped_coherent_ && !buffer_memory_.empty()) {
    ui::vulkan::util::FlushMappedMemoryRange(
        command_processor_.GetVulkanDevice(), buffer_memory_[0],
        buffer_memory_type_, buffer_offset, vfetch_ring_memory_size_, length);
  }
  vfetch_ring_allocations_.push_back(
      {offset, offset + size, command_processor_.GetCurrentSubmission()});
  vfetch_ring_head_ = offset + size;
  ++vfetch_ring_copies_;
  vfetch_ring_bytes_ += length;
  if (cvars::log_render_pass_breaks > 0 && !(vfetch_ring_copies_ & 1023)) {
    XELOGI("[VFRING] {} draws kept their pass, {:.1f} MB copied, {} ring full",
           vfetch_ring_copies_, double(vfetch_ring_bytes_) / 1048576.0,
           vfetch_ring_full_);
  }
  return buffer_offset;
}

bool VulkanSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  // Snapshots and pass-head uploads must not move a consumer or an overwrite
  // of a logically published resolve ahead of its captured GPU commands.
  command_processor_.FlushDeferredResolve(
      VulkanCommandProcessor::ResolveDeferStop::kResource);

  if (!num_upload_ranges) {
    return true;
  }
  for (uint32_t i = 0; i < num_upload_ranges; ++i)
    MaterializeDepthSurfaces(upload_page_ranges[i].first << page_size_log2(),
                            upload_page_ranges[i].second << page_size_log2(),
                            true);

  if (zero_copy_) {
    // The buffer aliases guest RAM - nothing to copy, just mark the pages valid
    // so RequestRange stops asking to upload them.
    for (uint32_t i = 0; i < num_upload_ranges; ++i) {
      trace_writer_.WriteMemoryRead(
          upload_page_ranges[i].first << page_size_log2(),
          upload_page_ranges[i].second << page_size_log2());
      MakeRangeValid(upload_page_ranges[i].first << page_size_log2(),
                     upload_page_ranges[i].second << page_size_log2(), false);
    }
    return true;
  }

  // Ranges holding memexport output live in host_buffer_ (guest RAM) and may
  // still be being written by the GPU, so reading them with the CPU below races
  // those writes. Refresh those on the GPU and upload only what is left.
  cpu_upload_ranges_.clear();
  for (uint32_t i = 0; i < num_upload_ranges; ++i) {
    uint32_t range_base = upload_page_ranges[i].first << page_size_log2();
    uint32_t range_size = upload_page_ranges[i].second << page_size_log2();
    if (command_processor_.EnsureMemexportRangeInDeviceBuffer(range_base,
                                                              range_size)) {
      MakeRangeValid(range_base, range_size, false);
      continue;
    }
    cpu_upload_ranges_.push_back(upload_page_ranges[i]);
  }
  if (cpu_upload_ranges_.empty()) {
    return true;
  }
  upload_page_ranges = cpu_upload_ranges_.data();
  num_upload_ranges = uint32_t(cpu_upload_ranges_.size());
  if (cvars::log_frame_spikes_ms) {
    uint64_t upload_pages = 0;
    for (uint32_t i = 0; i < num_upload_ranges; ++i) {
      upload_pages += upload_page_ranges[i].second;
    }
    cp_wait_stats::upload_bytes.fetch_add(upload_pages << page_size_log2(),
                                          std::memory_order_relaxed);
  }

  auto& range_front = upload_page_ranges[0];
  auto& range_back = upload_page_ranges[num_upload_ranges - 1];

  // Copies of pages never invalidated while this submission was recording
  // can't have been read by an already-recorded command, so they can execute
  // at the head of the submission's command buffer without breaking the
  // current render pass.
  bool hoist = false;
  if (cvars::vulkan_hoist_shmem_uploads) {
    hoist = !AnyPageInvalidatedSinceSubmissionOpen(upload_page_ranges,
                                                   num_upload_ranges);
    if (!hoist && cvars::vulkan_hoist_unused_shmem_uploads) {
      hoist = !AnyPageUsedSinceSubmissionOpen(upload_page_ranges,
                                              num_upload_ranges);
    }
  }

  // Inside a render pass whose commands haven't used these pages yet, the copy
  // can execute right before the pass instead of ending it.
  DeferredCommandBuffer* pass_head_command_buffer = nullptr;
  if (!hoist) {
    pass_head_command_buffer = command_processor_.pass_head_command_buffer();
    if (pass_head_command_buffer &&
        AnyPageUsedInCurrentPass(upload_page_ranges, num_upload_ranges)) {
      pass_head_command_buffer = nullptr;
    }
    if (pass_head_command_buffer) {
      // Later uploads of these pages must not be hoisted before this one. At
      // the head it precedes a load moved there too, so it's no mid-pass write.
      MarkPagesUsedHard(upload_page_ranges, num_upload_ranges, false);
      ++pass_head_upload_count_;
    }
  }
  if ((pass_head_command_buffer || hoist) &&
      cvars::log_render_pass_breaks > 0) {
    static uint32_t pass_head_logged = 0;
    if (pass_head_command_buffer && (++pass_head_logged & 255) == 0) {
      XELOGI("[PASSHEAD] {} uploads moved to pass heads",
             pass_head_upload_count_);
    }
  }
  if (!hoist && !pass_head_command_buffer &&
      cvars::log_render_pass_breaks > 0) {
    // Diagnostic: which requests end render passes, and what they upload.
    static uint32_t upload_breaks_logged = 0;
    if (upload_breaks_logged < 4000) {
      ++upload_breaks_logged;
      Dl_info dl_info = {};
      uintptr_t base = 0;
      if (dladdr(reinterpret_cast<void*>(&cvars::log_render_pass_breaks),
                 &dl_info)) {
        base = reinterpret_cast<uintptr_t>(dl_info.dli_fbase);
      }
      XELOGI("[UPBRK] caller={:X} first={:08X} pages={} ranges={} inval={}",
             uintptr_t(debug_request_caller_) - base,
             range_front.first << page_size_log2(),
             range_back.first + range_back.second - range_front.first,
             num_upload_ranges,
             AnyPageInvalidatedSinceSubmissionOpen(upload_page_ranges,
                                                   num_upload_ranges));
    }
  }
  if (!hoist && !pass_head_command_buffer) {
    MarkPagesUsedHard(upload_page_ranges, num_upload_ranges);
    // upload_page_ranges are sorted - the bounds give the barrier range.
    Use(Usage::kTransferDestination,
        std::make_pair(
            range_front.first << page_size_log2(),
            (range_back.first + range_back.second - range_front.first)
                << page_size_log2()));
    // Submit barriers (may end render pass) before pushing debug marker so
    // EndRenderPass is not inside the SharedMem Upload marker.
    command_processor_.SubmitBarriers(true);

    uint32_t total_upload_bytes =
        (range_back.first + range_back.second - range_front.first)
        << page_size_log2();
    command_processor_.PushDebugMarker(
        "UploadRanges (SharedMem): 0x%08X-0x%08X (%u KB, %u ranges)",
        range_front.first << page_size_log2(),
        (range_back.first + range_back.second) << page_size_log2(),
        total_upload_bytes / 1024, num_upload_ranges);
  }
  DeferredCommandBuffer& command_buffer =
      hoist ? command_processor_.deferred_setup_command_buffer()
            : (pass_head_command_buffer
                   ? *pass_head_command_buffer
                   : command_processor_.deferred_command_buffer());
  if (hoist && command_buffer.empty()) {
    // Once per submission: order the head copies after shared-memory writes
    // from previous submissions.
    VkMemoryBarrier barrier;
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.pNext = nullptr;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    command_buffer.CmdVkPipelineBarrier(
        command_processor_.GetPassHeadSourceStages(),
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  VkBuffer upload_destination_previous = VK_NULL_HANDLE;

  auto flush_upload_regions = [&]() {
    if (upload_regions_.empty()) {
      return;
    }
    assert_true(upload_buffer_previous != VK_NULL_HANDLE &&
                upload_destination_previous != VK_NULL_HANDLE);
    if (cvars::log_texture_clobber) {
      static uint32_t logged = 0;
      for (const auto& r : upload_regions_) {
        if (r.dstOffset >= 0x17100000 || r.dstOffset + r.size <= 0x16F30000)
          continue;
        if (logged++ >= 4096) break;
        XELOGI("[POOL-U] s={} cb={:X} pos={} place={} dst={:08X}+{}",
               submission_current, reinterpret_cast<uintptr_t>(&command_buffer),
               command_buffer.size_elements(),
               hoist ? "SETUP" : (pass_head_command_buffer ? "HEAD" : "BODY"),
               uint32_t(r.dstOffset), r.size);
      }
    }
    if (hoist) {
      if (!setup_upload_batch_count_ ||
          setup_upload_batches_[setup_upload_batch_count_ - 1].source !=
              upload_buffer_previous ||
          setup_upload_batches_[setup_upload_batch_count_ - 1].destination !=
              upload_destination_previous) {
        if (setup_upload_batch_count_ == setup_upload_batches_.size()) {
          setup_upload_batches_.push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, {}});
        }
        auto& batch = setup_upload_batches_[setup_upload_batch_count_++];
        batch.source = upload_buffer_previous;
        batch.destination = upload_destination_previous;
      }
      auto& regions =
          setup_upload_batches_[setup_upload_batch_count_ - 1].regions;
      for (const auto& region : upload_regions_) {
        if (!regions.empty() &&
            regions.back().srcOffset + regions.back().size == region.srcOffset &&
            regions.back().dstOffset + regions.back().size == region.dstOffset) {
          regions.back().size += region.size;
        } else {
          regions.push_back(region);
        }
      }
    } else {
      if (command_buffer.pass_bytes_enabled()) {
        PassByteLedger::Event tag; tag.kind = PassByteLedger::kTag;
        tag.category = PassByteLedger::kUpload; command_buffer.AnnotatePassBytes(tag);
      }
      command_buffer.CmdVkCopyBuffer(upload_buffer_previous,
                                     upload_destination_previous,
                                     uint32_t(upload_regions_.size()),
                                     upload_regions_.data());
    }
    upload_regions_.clear();
  };

  auto queue_upload_to_guest = [&](VkBuffer source_buffer,
                                   VkDeviceSize source_offset,
                                   const BufferSliceList& destination_slices,
                                   VkDeviceSize copy_size) {
    if (copy_size > UINT32_MAX || destination_slices.empty()) {
      return false;
    }
    VkDeviceSize destination_bytes = 0;
    for (const BufferSlice& slice : destination_slices) {
      destination_bytes += slice.length;
    }
    if (destination_bytes != copy_size) {
      return false;
    }
    VkDeviceSize source_bytes_queued = 0;
    for (const BufferSlice& slice : destination_slices) {
      if (upload_buffer_previous != source_buffer ||
          upload_destination_previous != slice.buffer) {
        flush_upload_regions();
        upload_buffer_previous = source_buffer;
        upload_destination_previous = slice.buffer;
      }
      VkBufferCopy& region = upload_regions_.emplace_back();
      region.srcOffset = source_offset + source_bytes_queued;
      region.dstOffset = slice.buffer_offset;
      region.size = slice.length;
      source_bytes_queued += slice.length;
    }
    return source_bytes_queued == copy_size;
  };

  // Uploads one span of pages that is known host-readable.
  auto upload_span = [&](uint32_t upload_range_start,
                         uint32_t upload_range_length) -> bool {
    const uint32_t span_start = upload_range_start << page_size_log2();
    const uint32_t span_size = upload_range_length << page_size_log2();
    upload_chunks_.clear();
    // Allocate staging first: pool growth and driver allocation must not keep
    // guest pages protected while no snapshot is being copied yet.
    while (upload_range_length) {
      auto& chunk = upload_chunks_.emplace_back();
      chunk.page_first = upload_range_start;
      chunk.mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), chunk.buffer, chunk.offset, chunk.size);
      if (!chunk.mapping || !chunk.size ||
          chunk.size > (VkDeviceSize(upload_range_length) << page_size_log2()) ||
          (chunk.size & ((VkDeviceSize(1) << page_size_log2()) - 1))) {
        XELOGE("Shared memory: Failed to get a page-aligned Vulkan upload buffer");
        return false;
      }
      chunk.destination = GetBufferSlices(
          upload_range_start << page_size_log2(), uint32_t(chunk.size));
      if (chunk.destination.empty()) {
        XELOGE("Shared memory: failed to allocate upload destination at {:08X}",
               upload_range_start << page_size_log2());
        return false;
      }
      uint32_t pages = uint32_t(chunk.size >> page_size_log2());
      upload_range_start += pages;
      upload_range_length -= pages;
    }
    // One prearm per readable span, independent of staging-pool boundaries.
    // Never revalidate individual chunks after this: a write fault racing an
    // earlier copy must leave ALL its invalidated pages invalid.
    MakeRangeValid(span_start, span_size, false);
    for (const auto& chunk : upload_chunks_) {
      const uint32_t guest_offset = chunk.page_first << page_size_log2();
      auto* source = memory().TranslatePhysical<uint8_t*>(guest_offset);
      // Hash before copying so an unwatched write cannot certify newer bytes
      // than the snapshot. The next primary batch re-verifies those pages.
      SetPageUploadHashes(chunk.page_first,
                          uint32_t(chunk.size >> page_size_log2()), source);
      if (chunk.size > 8192) {
        memory::vastcpy(chunk.mapping, source, uint32_t(chunk.size));
        swcache::WriteFence();
      } else {
        std::memcpy(chunk.mapping, source, size_t(chunk.size));
      }
      if (!queue_upload_to_guest(chunk.buffer, chunk.offset,
                                 chunk.destination, chunk.size)) {
        XELOGE("Shared memory: failed to queue upload destination at {:08X}",
               guest_offset);
        // Include uncopied chunks in the rollback. Their validity was also
        // published by the span prearm, but no snapshot will reach the GPU.
        MemoryInvalidationCallback(span_start, span_size, true);
        return false;
      }
    }
    return true;
  };

  // Marks decommitted pages valid without copying them: the buffer keeps its
  // last-uploaded contents, which is what the console's GPU would have read
  // out of physical RAM.
  auto skip_span = [&](uint32_t page_first, uint32_t page_end) {
    MakeRangeValid(page_first << page_size_log2(),
                   (page_end - page_first) << page_size_log2(), false);
    MarkPagesValidUnreadable(page_first, page_end);
    XELOGW(
        "Vulkan shared memory: skipped upload of {} decommitted page(s) at "
        "{:08X}, keeping last-uploaded contents",
        page_end - page_first, page_first << page_size_log2());
  };

  // Pages the guest decommitted are PROT_NONE host-side, so they can be
  // neither copied nor refused: a refusal is retried by every draw that still
  // references the memory, and games legally free memory that live textures
  // point at (the console GPU reads physical RAM, ignoring CPU page tables).
  for (unsigned int i = 0; i < num_upload_ranges; ++i) {
    const uint32_t upload_range_start = upload_page_ranges[i].first;
    const uint32_t upload_range_length = upload_page_ranges[i].second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());
    if (!upload_range_length) {
      continue;
    }

    auto* heap = memory().GetPhysicalHeap();
    if (cvars::gpu_allow_invalid_upload_range ||
        heap->QueryRangeAccess(
            upload_range_start << page_size_log2(),
            (upload_range_start + upload_range_length - 1)
                << page_size_log2()) != xe::memory::PageAccess::kNoAccess) {
      if (!upload_span(upload_range_start, upload_range_length)) {
        successful = false;
        break;
      }
      continue;
    }

    uint32_t span_start = UINT32_MAX;
    uint32_t skipped_start = UINT32_MAX;
    for (uint32_t page = upload_range_start;
         page <= upload_range_start + upload_range_length; ++page) {
      const uint32_t page_addr = page << page_size_log2();
      const bool readable =
          page < upload_range_start + upload_range_length &&
          heap->QueryRangeAccess(page_addr, page_addr) !=
              xe::memory::PageAccess::kNoAccess;
      if (readable) {
        if (skipped_start != UINT32_MAX) {
          skip_span(skipped_start, page);
          skipped_start = UINT32_MAX;
        }
        if (span_start == UINT32_MAX) {
          span_start = page;
        }
      } else {
        if (span_start != UINT32_MAX) {
          if (!upload_span(span_start, page - span_start)) {
            successful = false;
            break;
          }
          span_start = UINT32_MAX;
        }
        if (page < upload_range_start + upload_range_length &&
            skipped_start == UINT32_MAX) {
          skipped_start = page;
        }
      }
    }
    if (!successful) {
      break;
    }
    if (skipped_start != UINT32_MAX) {
      skip_span(skipped_start, upload_range_start + upload_range_length);
    }
  }
  flush_upload_regions();
  if (!hoist && !pass_head_command_buffer) {
    command_processor_.PopDebugMarker();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage,
                                       VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |=
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanSharedMemory::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         trace_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         trace_download_buffer_memory_);
  ReleaseTraceDownloadRanges();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
