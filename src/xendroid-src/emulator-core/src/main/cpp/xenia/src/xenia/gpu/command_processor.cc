/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/command_processor.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include <time.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <signal.h>
#include <unwind.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#endif

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/frame_stats.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cp_wait_stats.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/sampler_info.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos_zpd_report.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"

DEFINE_bool(log_cp_heartbeat, false,
            "Every second, log the command processor's ring pointers, primary "
            "buffer executions, draws, the WAIT_REG_MEM it is in (if any) and "
            "the process RSS, from a separate thread (diagnostic for hangs "
            "with no swaps).",
            "GPU");
DEFINE_bool(log_cp_wait_stats, false,
            "Log GPU command processor blocking time by cause and global mutex "
            "contention, per guest frame, every 5 seconds.",
            "GPU");
DEFINE_int32(thread_placement, 0,
             "CPU placement of the emulator's threads, applied while running "
             "(re-applied every 120 frames for new threads). 0: scheduler "
             "decides. 1: the GPU command processor alone on the fastest core, "
             "every other thread on the remaining cores. 2: as 1, plus the GPU "
             "stream encoder alone on the second fastest core.",
             "GPU");
DEFINE_int32(cp_thread_nice, -10,
             "Nice value for the GPU command processor thread (applied at "
             "start and whenever it changes); 0 leaves it unchanged.",
             "GPU");
UPDATE_from_int32(cp_thread_nice, 2026, 10, 5, 12, 0);
DEFINE_uint32(log_frame_spikes_ms, 0,
              "Diagnostic: log every guest frame (swap to swap on the GPU "
              "command processor thread) longer than this many milliseconds, "
              "with what happened during it: texture creations, pipeline "
              "creations, shared memory upload bytes, and the command "
              "processor's blocking time by cause (with log_cp_wait_stats). "
              "0 disables.",
              "GPU");

namespace xe::cp_wait_stats {

namespace {
// Frame spike bookkeeping (CP thread only). spike_prev_ns mirrors ns[] at the
// previous swap; Frame() zeroes it together with ns[] every report period.
thread_local uint64_t spike_last_swap = 0;
thread_local uint64_t spike_prev_ns[kCount] = {};
thread_local uint64_t spike_prev_textures = 0, spike_prev_texture_bytes = 0,
                      spike_prev_pipelines = 0, spike_prev_upload = 0;

void FrameSpikes(uint64_t now) {
  const uint32_t threshold_ms = cvars::log_frame_spikes_ms;
  if (!threshold_ms) {
    spike_last_swap = 0;
    return;
  }
  const uint64_t textures = textures_created.load(std::memory_order_relaxed);
  const uint64_t texture_bytes =
      texture_bytes_created.load(std::memory_order_relaxed);
  const uint64_t pipelines = pipelines_created.load(std::memory_order_relaxed);
  const uint64_t upload = upload_bytes.load(std::memory_order_relaxed);
  if (spike_last_swap &&
      now - spike_last_swap >= uint64_t(threshold_ms) * 1000000ull) {
    auto wait_ms = [](Category category) {
      return double(ns[category] - spike_prev_ns[category]) / 1e6;
    };
    XELOGI(
        "FrameSpike: {:.1f} ms | textures created {} ({:.2f} MB), pipelines "
        "{}, uploads {:.2f} MB | CP waits ms: fence={:.1f} presenter={:.1f} "
        "pipeline={:.1f} encode={:.1f} throttle={:.1f} "
        "ring={:.1f} pending={:.1f} refresh_incl={:.1f}",
        double(now - spike_last_swap) / 1e6, textures - spike_prev_textures,
        double(texture_bytes - spike_prev_texture_bytes) / 1048576.0,
        pipelines - spike_prev_pipelines,
        double(upload - spike_prev_upload) / 1048576.0, wait_ms(kFence),
        wait_ms(kPresenter), wait_ms(kPipeline), wait_ms(kEncode),
        wait_ms(kThrottle), wait_ms(kRing),
        wait_ms(kPending), wait_ms(kRefreshTotal));
  }
  spike_last_swap = now;
  for (unsigned i = 0; i < kCount; ++i) {
    spike_prev_ns[i] = ns[i];
  }
  spike_prev_textures = textures;
  spike_prev_texture_bytes = texture_bytes;
  spike_prev_pipelines = pipelines;
  spike_prev_upload = upload;
}
}  // namespace

void UpdateNice() {
  static thread_local int32_t applied = 0;
  const int32_t value = cvars::cp_thread_nice;
  if (value == applied) {
    return;
  }
  // 0 at startup leaves the inherited priority; later changes (0 included)
  // are applied for live A/B tests.
  applied = value;
  const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  errno = 0;
  const int result = setpriority(PRIO_PROCESS, tid, value);
  const int error = result == -1 ? errno : 0;
  XELOGI("CPNice: tid={} requested={} result={} errno={} now={}", tid, value,
         result, error, getpriority(PRIO_PROCESS, tid));
}

void ApplyThreadPlacement() {
  static thread_local int32_t applied = 0;
  static thread_local uint32_t frames_since_apply = 0;
  const int32_t policy = cvars::thread_placement;
  if (policy == applied && (!policy || ++frames_since_apply < 120)) {
    return;
  }
  const bool changed = policy != applied;
  applied = policy;
  frames_since_apply = 0;
  // CPUs by cpuinfo_max_freq, fastest first.
  int cpus[64];
  uint32_t freqs[64];
  int cpu_count = 0;
  for (int cpu = 0; cpu < 64; ++cpu) {
    char path[96];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    FILE* file = fopen(path, "r");
    if (!file) {
      break;
    }
    unsigned int freq = 0;
    if (fscanf(file, "%u", &freq) != 1) {
      freq = 0;
    }
    fclose(file);
    cpus[cpu_count] = cpu;
    freqs[cpu_count] = freq;
    ++cpu_count;
  }
  if (cpu_count < 3) {
    return;
  }
  for (int i = 1; i < cpu_count; ++i) {
    for (int j = i; j > 0 && freqs[j] > freqs[j - 1]; --j) {
      std::swap(freqs[j], freqs[j - 1]);
      std::swap(cpus[j], cpus[j - 1]);
    }
  }
  const int reserved = policy >= 2 ? 2 : (policy >= 1 ? 1 : 0);
  cpu_set_t cp_set, encode_set, other_set;
  CPU_ZERO(&cp_set);
  CPU_ZERO(&encode_set);
  CPU_ZERO(&other_set);
  if (reserved) {
    CPU_SET(cpus[0], &cp_set);
    CPU_SET(cpus[reserved >= 2 ? 1 : 0], &encode_set);
    for (int i = reserved; i < cpu_count; ++i) {
      CPU_SET(cpus[i], &other_set);
    }
    if (reserved < 2) {
      encode_set = other_set;
    }
  } else {
    for (int i = 0; i < cpu_count; ++i) {
      CPU_SET(cpus[i], &cp_set);
    }
    encode_set = cp_set;
    other_set = cp_set;
  }
  const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    return;
  }
  int count = 0, failed = 0;
  while (dirent* entry = readdir(dir)) {
    const pid_t tid = pid_t(atoi(entry->d_name));
    if (tid <= 0) {
      continue;
    }
    const cpu_set_t* set = &other_set;
    if (tid == self) {
      set = &cp_set;
    } else {
      char path[64], comm[32] = {};
      snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
      if (FILE* file = fopen(path, "r")) {
        if (!fgets(comm, sizeof(comm), file)) {
          comm[0] = 0;
        }
        fclose(file);
      }
      if (!strncmp(comm, "GPU Encode", 10)) {
        set = &encode_set;
      }
    }
    if (sched_setaffinity(tid, sizeof(cpu_set_t), set)) {
      ++failed;
    } else {
      ++count;
    }
  }
  closedir(dir);
  if (changed) {
    XELOGI("ThreadPlacement: policy={} cp_cpu={} threads={} failed={}",
           policy, reserved ? cpus[0] : -1, count, failed);
  }
}

void Frame() {
  static thread_local uint64_t last = 0, frames = 0;
  static thread_local uint64_t prev_count[2] = {}, prev_ns[2] = {};
  static thread_local uint64_t prev_wakes = 0;
  const uint64_t now = Now();
  FrameSpikes(now);
  if (!cvars::log_cp_wait_stats) {
    last = 0;
    return;
  }
  const bool baseline = !last;
  if (!baseline) {
    ++frames;
    if (now - last < 5000000000ull) {
      return;
    }
  }
  uint64_t counts[2], waits[2];
  for (unsigned i = 0; i < 2; ++i) {
    counts[i] = mutex_count[i].load(std::memory_order_relaxed);
    waits[i] = mutex_ns[i].load(std::memory_order_relaxed);
  }
  const uint64_t wakes = cp_wakes.load(std::memory_order_relaxed);
  if (!baseline && frames) {
    const double f = double(frames);
    const double us = 1000.0 * f;
    XELOGI(
        "CPWait: per frame (us): frame={:.0f} ring={:.0f} fence={:.0f} "
        "presenter={:.0f} pipeline={:.0f} encode={:.0f} "
        "pause={:.0f} "
        "throttle={:.0f} pending={:.0f} | refresh_incl={:.0f} | mutex CP "
        "n={:.1f} us={:.0f} others n={:.1f} us={:.0f} CP wakes={:.1f}",
        double(now - last) / us, ns[kRing] / us, ns[kFence] / us,
        ns[kPresenter] / us, ns[kPipeline] / us, ns[kEncode] / us,
        ns[kPause] / us, ns[kThrottle] / us, ns[kPending] / us,
        ns[kRefreshTotal] / us, (counts[0] - prev_count[0]) / f,
        (waits[0] - prev_ns[0]) / us, (counts[1] - prev_count[1]) / f,
        (waits[1] - prev_ns[1]) / us, (wakes - prev_wakes) / f);
  }
  if (!baseline && frames) {
    // Longest global mutex holders since the last report.
    struct Top {
      uintptr_t key;
      uint64_t count, ns;
    } top[8] = {};
    for (HoldSite& entry : hold_sites) {
      uintptr_t key = entry.site.load(std::memory_order_relaxed);
      if (!key) {
        continue;
      }
      Top t = {key, entry.count.exchange(0, std::memory_order_relaxed),
               entry.ns.exchange(0, std::memory_order_relaxed)};
      for (Top& slot : top) {
        if (t.ns > slot.ns) {
          std::swap(t, slot);
        }
      }
    }
    std::string sites;
    for (const Top& t : top) {
      if (!t.ns) {
        break;
      }
      void* address = reinterpret_cast<void*>(t.key & ~uintptr_t(1));
      Dl_info info = {};
      uintptr_t offset = uintptr_t(address);
      if (dladdr(address, &info) && info.dli_fbase) {
        offset -= uintptr_t(info.dli_fbase);
      }
      sites += fmt::format(" {}0x{:X} n={:.2f} us={:.0f};", (t.key & 1) ? "CP:" : "",
                           offset, double(t.count) / double(frames),
                           double(t.ns) / 1000.0 / double(frames));
    }
    if (!sites.empty()) {
      XELOGI("LockHold: per frame, holds >= 50 us by site:{}", sites);
    }
    if (ns[kFence]) {
      const double us = 1000.0 * double(frames);
      XELOGI(
          "CPFence: per frame (us): regmem={:.0f} idle={:.0f} cpfault={:.0f} "
          "frame={:.0f} other={:.0f}",
          fence_site_ns[kFenceRegMem] / us, fence_site_ns[kFenceIdle] / us,
          fence_site_ns[kFenceCpFault] / us, fence_site_ns[kFenceFrame] / us,
          fence_site_ns[kFenceOther] / us);
    }
  }
  for (auto& value : fence_site_ns) {
    value = 0;
  }
  for (unsigned i = 0; i < 2; ++i) {
    prev_count[i] = counts[i];
    prev_ns[i] = waits[i];
  }
  prev_wakes = wakes;
  for (auto& value : ns) {
    value = 0;
  }
  for (auto& value : spike_prev_ns) {
    value = 0;
  }
  frames = 0;
  last = now;
}

}  // namespace xe::cp_wait_stats

#if !defined(NDEBUG)

#define XE_ENABLE_GPU_REG_WRITE_LOGGING 1
#endif
DEFINE_bool(
    log_guest_driven_gpu_register_written_values, false,
    "Only does anything in debug builds, if set will log every write to a gpu "
    "register done by a guest. Does not log writes that are done by the CP on "
    "its own, just ones the guest makes or instructs it to make.",
    "GPU");

DEFINE_uint32(
    gpu_stall_spin_iterations, 0,
    "How many times the command processor polls the ring buffer with a cheap "
    "yield before parking on the write-pointer event when the guest has "
    "produced no commands. The spin exists to dodge wake latency when the "
    "guest is about to kick again immediately, but on ARM64 the yield is a "
    "WFE that does not actually park on a busy SoC, so every iteration burns "
    "a core at full clock. 0 parks immediately; the event is signalled by "
    "UpdateWritePointer, so no wake-up is missed either way.",
    "GPU");
UPDATE_from_uint32(gpu_stall_spin_iterations, 2026, 10, 5, 12, 32);

DEFINE_int32(
    gpu_stall_spin_us, 80,
    "While frames are not reaching their slot with spare time, the command "
    "processor keeps checking the ring for up to this long before parking on "
    "the write-pointer event when the guest has produced no commands, and "
    "the guest thread that kicks the ring finds it awake. A game that waits "
    "for the processor to reach a fence pays a thread wake-up (tens of "
    "microseconds) on every kick otherwise. Never while frames have spare "
    "time or the thermal governor reports a hot device. 0 disables.",
    "GPU");
DEFINE_uint32(
    gpu_rptr_writeback_interval_dwords, 512,
    "Publish the ring buffer read pointer to the guest (CP_RB_RPTR_ADDR) "
    "every time this many dwords of the primary ring have been executed, "
    "not only once the whole pending range is done. A title waiting for ring "
    "space (GTA IV's main thread polls it) can then queue more commands while "
    "the rest of a large batch still executes, as on the console, whose CP "
    "updates the read pointer as it consumes. 0 = only at the end.",
    "GPU");
DEFINE_bool(disassemble_pm4, false,
            "Only does anything in debug builds, if set will disassemble and "
            "log all PM4 packets sent to the CP.",
            "GPU");

DEFINE_bool(
    log_ringbuffer_kickoff_initiator_bts, false,
    "Only does anything in debug builds, if set will log the pseudo-stacktrace "
    "of the guest thread that wrote the new read position.",
    "GPU");

DEFINE_bool(clear_memory_page_state, false,
            "Refresh state of memory pages to enable gpu written data. "
            "Uses mostly lock-free double-buffering for minimal overhead. "
            "(Enable if rendering breaks, at a minor performance cost)",
            "GPU");
UPDATE_from_bool(clear_memory_page_state, 2026, 8, 1, 12, true);

DEFINE_bool(
    log_gpu_frame_time_breakdown, false,
    "Log a once-per-second breakdown of where the GPU command processor "
    "thread spends each guest frame (swap-to-swap): PM4 execution, draw "
    "processing, the swap itself, and idle stall waiting for ring writes. "
    "Diagnostic aid for frame pacing investigations.",
    "GPU");

DEFINE_bool(
    log_cp_frame_breakdown, false,
    "Log a once-per-second breakdown of the GPU command processor thread per "
    "guest frame (draw sub-phases, resolves, fence waits, ring stall, thread "
    "CPU time) using CPU clocks only - no GPU timestamps.",
    "GPU");

DEFINE_int32(debug_ab_frames, 0,
             "Diagnostic (needs log_cp_frame_breakdown): flip an A/B variant "
             "every this many guest frames and log each window (AbWin), so "
             "a feature can be compared under the same thermal conditions. "
             "0 = off.",
             "GPU");
DEFINE_string(debug_ab_feature, "",
              "Feature inverted in A/B variant 1: vfetch_lock, desc_cache, "
              "skip_fetch.",
              "GPU");
DEFINE_string(
    occlusion_query, "fake",
    "Controls hardware occlusion query behavior for EVENT_WRITE_ZPD.\n"
    "Used for effects like lens flares, object culling, and auto-exposure.\n"
    "Titles that use QueryBatch are not currently supported and fall back to\n"
    "fake mode, regardless of this setting.\n"
    " fake: Write a fake result without asking the GPU. Safe for most games,\n"
    "       though some effects may look slightly wrong.\n"
    " fast: Ask the GPU but don't wait for the answer. Writes a cached\n"
    "       result immediately and updates it when the GPU catches up.\n"
    "       Cached results bias toward visible when guessing. (default)\n"
    " fast-alt: Variant of fast mode that keeps cached zero results for\n"
    "           unresolved reports. May improve effects relying on precise\n"
    "           visibility, but may be less stable for occlusion culling.\n"
    " strict: Ask the GPU and wait for the real result before continuing.\n"
    "         Most accurate, but may be somewhat less performant.",
    "GPU");
UPDATE_from_string(occlusion_query, 2026, 10, 5, 12, "fast");

DEFINE_string(
    readback_resolve, "uma",
    "Controls which render-to-texture resolves are copied back into guest "
    "RAM.\n"
    " uma: No copy - the CPU reads the host-mapped shared memory directly. "
    "The default, and the only mode that works on unified-memory GPUs "
    "(Adreno). Needs host-mapped shared memory and no resolution scaling.\n"
    " fast: Copy only resolves the CPU reads back\n"
    " all: Copy every resolve\n"
    " none: Disable readback completely (improves performance).\n",
    "GPU");

DEFINE_bool(
    memexport_enable, false,
    "Make memory export output visible to the CPU by routing the draws that "
    "write it to a buffer aliasing guest RAM. Needed by games that read "
    "exported data on the CPU. Disabling it keeps the output in device-local "
    "memory, which is faster for the draws that consume it on the GPU. Applies "
    "at title launch. Off by default here: the host buffer needs "
    "VK_EXT_external_memory_host, which Turnip does not expose, so the import "
    "fails and the output stays device-local either way.",
    "GPU");

DEFINE_bool(
    memexport_await_fences, false,
    "Wait for the GPU to finish outstanding memory export before signalling a "
    "fence the guest reads, so exported data is in guest RAM by the time the "
    "guest looks at it. Disabling it avoids the stall but games reading "
    "exported data on the CPU may see stale contents. Needs memexport_enable. "
    "Off by default here: without the host buffer the wait is a full GPU stall "
    "per fence that cannot make the output visible anyway.",
    "GPU");

DEFINE_bool(
    precise_interpolation, true,
    "Manually interpolate pixel shader inputs with barycentric coordinates to "
    "exactly match the guest and avoid hardware interpolation precision "
    "differences. Fixes noise artifacts in games like Perfect Dark and Tenchu "
    "Z "
    "that do exact equality comparisons on interpolated values. Requires "
    "fragment shader barycentric support (VK_KHR_fragment_shader_barycentric "
    "on "
    "Vulkan, SV_Barycentrics on Direct3D 12).",
    "GPU");

namespace xe {
namespace gpu {

// This should be written completely differently with support for different
// types.
void SaveGPUSetting(GPUSetting setting, uint64_t value) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      OVERRIDE_bool(clear_memory_page_state, static_cast<bool>(value));
      break;
    case GPUSetting::MemexportAwaitFences:
      OVERRIDE_bool(memexport_await_fences, static_cast<bool>(value));
      break;
  }
}

bool GetGPUSetting(GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      return cvars::clear_memory_page_state;
    case GPUSetting::MemexportAwaitFences:
      return cvars::memexport_await_fences;
    default:
      return false;
  }
}

// Both mode getters are called on the GPU command processor's per-packet/
// per-draw paths; parsing the cvar string each call cost ~5% of that thread
// in profiles. Parse once, cache the enum, and have the setters (the only
// way the modes change at runtime) refresh the cache.
namespace {
std::atomic<int> readback_resolve_mode_cache{-1};
std::atomic<int> zpd_mode_cache{-1};

ReadbackResolveMode ParseReadbackResolveMode(const std::string& mode) {
  if (mode == "all") {
    return ReadbackResolveMode::kAll;
  } else if (mode == "uma") {
    return ReadbackResolveMode::kUma;
  } else if (mode == "none") {
    return ReadbackResolveMode::kDisabled;
  } else if (mode == "fast") {
    return ReadbackResolveMode::kFast;
  } else {
    // Retired values (some/full) and typos: fall back to the default rather
    // than to a mode that copies nothing on unified-memory GPUs.
    static bool logged = false;
    if (!logged) {
      logged = true;
      XELOGW("Unknown readback_resolve value '{}' - using uma", mode);
    }
    return ReadbackResolveMode::kUma;
  }
}

ReadbackResolveMode ParseReadbackResolveMode() {
  return ParseReadbackResolveMode(cvars::readback_resolve);
}

void SetReadbackResolveCvar(const std::string& mode) {
  OVERRIDE_string(readback_resolve, mode);
  readback_resolve_mode_cache.store(int(ParseReadbackResolveMode(mode)),
                                    std::memory_order_relaxed);
}

ZPDMode ParseZPDMode(const std::string& mode) {
  if (mode == "fake") {
    return ZPDMode::kFake;
  } else if (mode == "strict") {
    return ZPDMode::kStrict;
  } else if (mode == "fast") {
    return ZPDMode::kFast;
  } else if (mode == "fast-alt") {
    return ZPDMode::kFastAlt;
  } else {
    // Default to "fake" for any unrecognized value.
    return ZPDMode::kFake;
  }
}

ZPDMode ParseZPDMode() { return ParseZPDMode(cvars::occlusion_query); }
}  // namespace

ReadbackResolveMode GetReadbackResolveMode() {
  int cached = readback_resolve_mode_cache.load(std::memory_order_relaxed);
  if (XE_UNLIKELY(cached < 0)) {
    cached = int(ParseReadbackResolveMode(cvars::readback_resolve));
    readback_resolve_mode_cache.store(cached, std::memory_order_relaxed);
  }
  return ReadbackResolveMode(cached);
}

void SetReadbackResolveMode(const std::string& mode) {
  OVERRIDE_string(readback_resolve, mode);
  readback_resolve_mode_cache.store(int(ParseReadbackResolveMode(mode)),
                                    std::memory_order_relaxed);
}

ZPDMode GetZPDMode() {
  int cached = zpd_mode_cache.load(std::memory_order_relaxed);
  if (XE_UNLIKELY(cached < 0)) {
    cached = int(ParseZPDMode(cvars::occlusion_query));
    zpd_mode_cache.store(cached, std::memory_order_relaxed);
  }
  return ZPDMode(cached);
}

static void SetZPDModeCvar(const std::string& mode) {
  OVERRIDE_string(occlusion_query, mode);
  zpd_mode_cache.store(int(ParseZPDMode(mode)), std::memory_order_relaxed);
}

void InvalidateConfigModeCaches() {
  // Per-game config load (config::ReadGameConfig) writes the raw cvar strings
  // directly via IConfigVar::LoadConfigValue and does NOT go through the
  // Set*Mode setters, so the parsed-enum caches would otherwise keep the
  // values parsed from the global config and silently ignore every per-game
  // occlusion_query / readback_resolve override. Reset to -1 so the next
  // Get*Mode re-parses the now-updated cvar.
  readback_resolve_mode_cache.store(-1, std::memory_order_relaxed);
  zpd_mode_cache.store(-1, std::memory_order_relaxed);
}

using namespace xe::gpu::xenos;

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   kernel::KernelState* kernel_state)
    : reader_(nullptr, 0),
      memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(xe::threading::Event::CreateAutoResetEvent(false)),
      pause_resume_event_(xe::threading::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
  assert_not_null(pause_resume_event_);
  // Parse and cache readback resolve mode once
  cached_readback_resolve_mode_ = ParseReadbackResolveMode();
  // Parse and cache ZPD mode once.
  cached_zpd_mode_ = ParseZPDMode();
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state_->GetIdleProcess()));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  // Already stopped if drained early during relaunch (stopped again at
  // teardown).
  if (!worker_thread_) {
    return;
  }

  EndTracing();

  worker_running_ = false;
  // Snapshot/lifecycle pause parks inside a queued worker callback. Stop must
  // release that callback before waiting for the worker to exit.
  Resume();
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  if (completion_callback) {
    completion_callback();
  }
}

void CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

void CommandProcessor::RestoreRegisters(uint32_t first_register,
                                        const uint32_t* register_values,
                                        uint32_t register_count,
                                        bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    XELOGW(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register,
                 register_count);
  }
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
  // Restore may reproduce identical registers while host objects were reset.
  register_file_->InvalidateDerivedState();
}

void CommandProcessor::RestoreGammaRamp(
    const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
    const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
    uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb,
              sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

bool CommandProcessor::IsWorkerThread() const {
  return kernel::XThread::IsInThread(worker_thread_.get());
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (!has_pending_fns_.load(std::memory_order_acquire) &&
      kernel::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    std::lock_guard<std::mutex> lock(pending_fns_mutex_);
    pending_fns_.push(std::move(fn));
    has_pending_fns_.store(true, std::memory_order_release);
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

void CommandProcessor::ClearReadbackBuffers() {}

void CommandProcessor::SetReadbackResolveMode(ReadbackResolveMode mode) {
  if (cached_readback_resolve_mode_ == mode) {
    return;
  }
  // Update cached value
  cached_readback_resolve_mode_ = mode;
  // Update cvar string for UI display
  const char* mode_str = "fast";
  switch (mode) {
    case ReadbackResolveMode::kDisabled:
      mode_str = "none";
      break;
    case ReadbackResolveMode::kAll:
      mode_str = "all";
      break;
    case ReadbackResolveMode::kUma:
      mode_str = "uma";
      break;
    default:
      break;
  }
  SetReadbackResolveCvar(mode_str);

  // Save to per-game config if a title is loaded
  uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;
  if (title_id != 0) {
    toml::table config_table = config::LoadGameConfig(title_id);

    if (!config_table.contains("GPU")) {
      config_table.insert("GPU", toml::table{});
    }

    auto* gpu_table = config_table["GPU"].as_table();
    if (gpu_table) {
      gpu_table->insert_or_assign("readback_resolve", mode_str);
    }

    config::SaveGameConfig(title_id, config_table);
  }
}

void CommandProcessor::SetZPDMode(ZPDMode mode) {
  if (cached_zpd_mode_ == mode) {
    return;
  }
  // Close any active query segment before the mode changes so that a
  // BeginQuery recorded under the old mode gets a matching EndQuery.
  // Without this, switching to kFake mid-frame would cause EndRenderPass
  // to skip CloseQuerySegment, leaving the query dangling.
  if (zpd_active_segment_.segment_active) {
    CloseQuerySegment();
  }
  cached_zpd_mode_ = mode;
  const char* mode_str = "fake";
  switch (mode) {
    case ZPDMode::kFast:
      mode_str = "fast";
      break;
    case ZPDMode::kFastAlt:
      mode_str = "fast-alt";
      break;
    case ZPDMode::kStrict:
      mode_str = "strict";
      break;
    default:
      break;
  }
  SetZPDModeCvar(mode_str);

  // Save to per-game config if a title is loaded.
  uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;
  if (title_id != 0) {
    toml::table config_table = config::LoadGameConfig(title_id);

    if (!config_table.contains("GPU")) {
      config_table.insert("GPU", toml::table{});
    }

    auto* gpu_table = config_table["GPU"].as_table();
    if (gpu_table) {
      gpu_table->insert_or_assign("occlusion_query", mode_str);
    }

    config::SaveGameConfig(title_id, config_table);
  }
}

void CommandProcessor::SetDesiredSwapPostEffect(
    SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() {
    swap_post_effect_actual_ = swap_post_effect;
  });
}

uint64_t CommandProcessor::FrameStatsNow() {
  // The draw breakdown reads this ~14 times per draw (70k per frame in Black
  // Ops): steady_clock through the vDSO took about half of the command
  // processor thread with the breakdown on (simpleperf, q555), inflating every
  // CP measurement. Only differences of its values are used.
  return Clock::QueryRawNanoseconds();
}

uint64_t CommandProcessor::FrameStatsThreadCpuNow() {
#if defined(__ANDROID__)
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  }
#endif
  return 0;
}

void CommandProcessor::RefreshDrawStatsConfig() {
  draw_phase_stats_enabled_ = cvars::log_cp_frame_breakdown;
  if (!draw_phase_stats_enabled_) frame_stats_phase_mark_ = 0;
  draw_stats_enabled_ = draw_phase_stats_enabled_ || cvars::log_gpu_frame_time_breakdown;
  draw_ab_enabled_ = cvars::debug_ab_frames > 0;
}

uint64_t CommandProcessor::FrameStatsBegin() {
  return draw_stats_enabled_ ? FrameStatsNow() : 0;
}

void CommandProcessor::FrameStatsPhaseStart() {
  frame_stats_phase_mark_ = draw_phase_stats_enabled_ ? FrameStatsNow() : 0;
}

void CommandProcessor::FrameStatsEndDraw(uint64_t begin_ns) {
  if (heartbeat_) {
    hb_draws_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!begin_ns) {
    if (draw_ab_enabled_) {
      ++ab_window_.draws;
    }
    return;
  }
  const uint64_t draw_ns = FrameStatsNow() - begin_ns;
  frame_time_stats_.draws++;
  if (bin_select_ != 0xFFFFFFFFFFFFFFFFull) {
    frame_time_stats_.tile_draws++;
  }
  frame_time_stats_.draw_ns += draw_ns;
  ab_window_.draws++;
  ab_window_.draw_ns += draw_ns;
}

void CommandProcessor::AbOnSwap(uint64_t now_ns) {
  if (cvars::debug_ab_frames <= 0) {
    return;
  }
  AbWindow& w = ab_window_;
  if (!w.last_swap_ns) {
    w = AbWindow();
    w.last_swap_ns = now_ns;
    w.thread_cpu_start_ns = FrameStatsThreadCpuNow();
    return;
  }
  w.interval_ns += now_ns - w.last_swap_ns;
  w.last_swap_ns = now_ns;
  if (++w.frames < uint32_t(cvars::debug_ab_frames)) {
    return;
  }
  const uint64_t cpu_ns = FrameStatsThreadCpuNow();
  const double f = double(w.frames);
  XELOGI("AbWin: v={} frames={} interval={:.2f}ms cpu={:.2f}ms draw={:.2f}ms "
         "draws={:.0f}",
         ab_variant_ ? 1 : 0, w.frames, w.interval_ns / f / 1e6,
         (cpu_ns - w.thread_cpu_start_ns) / f / 1e6, w.draw_ns / f / 1e6,
         w.draws / f);
  ab_variant_ = !ab_variant_;
  ab_active_feature_ = AbFeature::kNone;
  if (ab_variant_) {
    const std::string& feature = cvars::debug_ab_feature;
    if (feature == "vfetch_lock") {
      ab_active_feature_ = AbFeature::kVfetchLock;
    } else if (feature == "desc_cache") {
      ab_active_feature_ = AbFeature::kDescCache;
    } else if (feature == "skip_fetch") {
      ab_active_feature_ = AbFeature::kSkipFetch;
    } else if (feature == "fast_path") {
      ab_active_feature_ = AbFeature::kFastPath;
    } else if (feature == "unwatch") {
      ab_active_feature_ = AbFeature::kUnwatch;
    } else if (feature == "pass_head") {
      ab_active_feature_ = AbFeature::kPassHead;
    }
  }
  const uint64_t keep_swap = w.last_swap_ns;
  w = AbWindow();
  w.last_swap_ns = keep_swap;
  w.thread_cpu_start_ns = cpu_ns;
}

void CommandProcessor::FrameStatsEndSwap(uint64_t begin_ns) {
  if (!begin_ns) {
    // Lean A/B windows (interval and thread CPU only) without the per-draw
    // timers, which cost the CP thread ~15%.
    if (cvars::debug_ab_frames > 0) {
      AbOnSwap(FrameStatsNow());
    }
    return;
  }
  auto& s = frame_time_stats_;
  const uint64_t now = FrameStatsNow();
  AbOnSwap(now);
  s.swap_ns += now - begin_ns;
  s.frames++;
  if (s.last_swap_ns) {
    const uint64_t interval = now - s.last_swap_ns;
    s.interval_ns += interval;
    s.interval_max_ns = std::max(s.interval_max_ns, interval);
  }
  s.last_swap_ns = now;
  if (!s.last_report_ns) {
    s.last_report_ns = now;
    s.last_report_thread_cpu_ns = FrameStatsThreadCpuNow();
  } else if (now - s.last_report_ns >= 1000000000ull && s.frames) {
    const double f = static_cast<double>(s.frames);
    const uint64_t thread_cpu_ns = FrameStatsThreadCpuNow();
    XELOGI(
        "GpuFrame: {} frames, interval avg={:.1f}ms max={:.1f}ms | per frame: "
        "exec={:.1f}ms draws={:.0f} draw={:.1f}ms swap={:.1f}ms "
        "stall={:.1f}ms",
        s.frames, s.interval_ns / f / 1e6, s.interval_max_ns / 1e6,
        s.exec_ns / f / 1e6, s.draws / f, s.draw_ns / f / 1e6,
        s.swap_ns / f / 1e6, s.stall_ns / f / 1e6);
    if (cvars::log_cp_frame_breakdown) {
      // Phases: 0 analysis, 8 submission, 9 primitives, 1 translations+
      // samplers, 2 render targets, 3 pipeline, 4 textures, 5 state+constants,
      // 10 UpdateBindings, 11 vertex fetch requests, 6 memexport+rest, 7 render
      // pass+recording.
      if (s.rep_draws) {
        const double d = double(s.rep_draws);
        XELOGI(
            "CpRepeat: draws={:.0f}/frame nostate={:.1f}% fetchonly={:.1f}% "
            "sameshaders={:.1f}% sameshape={:.1f}%",
            d / f, 100.0 * s.rep_nostate / d, 100.0 * s.rep_fetchonly / d,
            100.0 * s.rep_sameshaders / d, 100.0 * s.rep_sameshape / d);
      }
      XELOGI(
          "CpFrame: cpu={:.1f}ms gpuwait={:.1f}ms copies={:.1f} copy={:.2f}ms "
          "waitregmem={:.1f} polls={:.1f} {:.2f}ms "
          "| phases ms: ana={:.2f} sub={:.2f} prim={:.2f} xlat={:.2f} rt={:.2f} "
          "pipe={:.2f} tex={:.2f} state={:.2f} ub={:.2f} vf={:.2f} mx={:.2f} "
          "rec={:.2f} | tiles: seldraws={:.0f} selchg={:.1f} ext={:.0f}",
          (thread_cpu_ns && s.last_report_thread_cpu_ns)
              ? (thread_cpu_ns - s.last_report_thread_cpu_ns) / f / 1e6
              : 0.0,
          s.gpu_wait_ns / f / 1e6, s.copies / f, s.copy_ns / f / 1e6,
          s.wait_reg_mems / f, s.wait_reg_mem_polls / f,
          s.wait_reg_mem_ns / f / 1e6,
          s.phase_ns[0] / f / 1e6, s.phase_ns[8] / f / 1e6,
          s.phase_ns[9] / f / 1e6, s.phase_ns[1] / f / 1e6,
          s.phase_ns[2] / f / 1e6, s.phase_ns[3] / f / 1e6,
          s.phase_ns[4] / f / 1e6, s.phase_ns[5] / f / 1e6,
          s.phase_ns[10] / f / 1e6, s.phase_ns[11] / f / 1e6,
          s.phase_ns[6] / f / 1e6, s.phase_ns[7] / f / 1e6,
          s.tile_draws / f, s.bin_select_changes / f, s.screen_extents / f);
    }
    const uint64_t keep_swap = s.last_swap_ns;
    s = FrameTimeStats();
    s.last_swap_ns = keep_swap;
    s.last_report_ns = now;
    s.last_report_thread_cpu_ns = thread_cpu_ns;
  }
}

void CommandProcessor::ThrottlePresentation() {
  // Host frame rate limiting based on framerate_limit cvar.
  const uint32_t framerate_limit = cvars::framerate_limit;

  if (framerate_limit == 0) {
    // No host frame limiting
    return;
  }

  const uint64_t tick_freq = Clock::guest_tick_frequency();
  uint32_t effective_limit = framerate_limit;
  if (cvars::framerate_limit_adaptive && framerate_limit >= 40) {
    // See AdaptiveFrameLimiter: the limit steps down while the game doesn't
    // deliver it (to framerate_limit_adaptive_mid if the game holds that,
    // else to half) and back up when a frame's work fits a faster period
    // with margin.
    const uint64_t now_ticks = Clock::QueryGuestTickCount();
    // Time since the slot of the previous frame, minus the time the game's
    // presenting thread was put to sleep waiting for its slot
    // (guest_swap_pacing): the work of this frame.
    uint64_t work_ticks = now_ticks - last_swap_time_;
    const uint64_t pacing_sleep_ticks =
        presentation_pacing_sleep_ticks_.exchange(0,
                                                  std::memory_order_relaxed);
    work_ticks -= std::min(work_ticks, pacing_sleep_ticks);
    const AdaptiveFrameLimiter::Result adaptive = adaptive_limiter_.OnFrame(
        now_ticks, work_ticks, tick_freq, framerate_limit,
        cvars::framerate_limit_adaptive_mid);
    if (adaptive.change == AdaptiveFrameLimiter::Change::kLowered) {
      XELOGI("Frame limiter: {} FPS not sustained, limiting to {}",
             adaptive.previous_limit, adaptive.limit);
    } else if (adaptive.change == AdaptiveFrameLimiter::Change::kRaised) {
      XELOGI("Frame limiter: back to {} FPS", adaptive.limit);
    }
    effective_limit = adaptive.limit;
    xe::cpu::backend::frame_limiter_probe.store(adaptive.probe,
                                                std::memory_order_relaxed);
    // The intermediate rate needs a display mode it divides: ask for it while
    // it is in use, and leave the choice alone otherwise.
    xe::SetRequestedPresentRate(
        adaptive_limiter_.level() == 1 ? float(adaptive.limit) : -1.0f);
  } else {
    adaptive_limiter_.Reset();
    xe::cpu::backend::frame_limiter_probe.store(false,
                                                std::memory_order_relaxed);
    presentation_pacing_sleep_ticks_.store(0, std::memory_order_relaxed);
    xe::SetRequestedPresentRate(-1.0f);
  }
  presentation_effective_limit_.store(effective_limit,
                                      std::memory_order_release);
  const double target_duration_ms =
      1000.0 / static_cast<double>(effective_limit);

  const uint64_t target_duration_ticks = static_cast<uint64_t>(
      target_duration_ms * static_cast<double>(tick_freq) / 1000.0);

  // Spin until target duration has elapsed
  while (true) {
    const uint64_t current_time = Clock::QueryGuestTickCount();
    const uint64_t time_delta = current_time - last_swap_time_;

    if (time_delta >= target_duration_ticks) {
      // If we've fallen behind by more than 2 frames, reset to catch up
      if (time_delta > target_duration_ticks * 2) {
        last_swap_time_ = current_time;
      } else {
        last_swap_time_ += target_duration_ticks;
      }
      presentation_slot_ticks_.store(last_swap_time_,
                                     std::memory_order_release);
      presentation_swaps_throttled_.fetch_add(1, std::memory_order_release);
      return;
    }

    const double elapsed_ms = static_cast<double>(time_delta) /
                              (static_cast<double>(tick_freq) / 1000.0);

    const double remaining_ms = target_duration_ms - elapsed_ms;
#if XE_PLATFORM_WIN32
    // Sleep 90% of remaining, spin the rest for accuracy
    const uint64_t sleep_ns =
        static_cast<uint64_t>(remaining_ms * 1000000.0 * 0.90);
    if (sleep_ns > 0) {
      xe::threading::NanoSleep(sleep_ns);
    }
#elif XE_PLATFORM_MAC
    // Darwin's nanosleep oversleeps by 100-500us; NanoSleepPrecise spins the
    // tail so 60Hz targets don't miss their frame budget.
    const uint64_t sleep_ns = static_cast<uint64_t>(remaining_ms * 1000000.0);
    if (sleep_ns > 0) {
      xe::threading::NanoSleepPrecise(sleep_ns);
    }
#else
    const uint64_t sleep_ns = static_cast<uint64_t>(remaining_ms * 1000000.0);
    if (sleep_ns > 0) {
      cp_wait_stats::Scope throttle_wait(cp_wait_stats::kThrottle);
      xe::threading::NanoSleep(sleep_ns);
    }
#endif
  }
}

void CommandProcessor::PinCurrentThreadToFastCores(const char* thread_label) {}

void CommandProcessor::WorkerThreadMain() {
  cp_wait_stats::is_cp = true;
  cp_wait_stats::UpdateNice();
  PinCurrentThreadToFastCores("GPU Commands");
  if (!SetupContext()) {
    xe::FatalError("Unable to setup command processor internal state");
    return;
  }

  heartbeat_ = cvars::log_cp_heartbeat;
#if defined(__ANDROID__)
  hb_cp_tid_ = int(gettid());
#endif
  std::thread heartbeat_thread;
  if (heartbeat_) {
    hb_stop_ = false;
    heartbeat_thread = std::thread([this] { HeartbeatThreadMain(); });
  }

  while (worker_running_) {
    ServiceMemoryRequests();
    while (has_pending_fns_.load(std::memory_order_acquire)) {
      std::function<void()> fn;
      {
        cp_wait_stats::Scope pending_wait(cp_wait_stats::kPending);
        std::lock_guard<std::mutex> lock(pending_fns_mutex_);
        pending_wait.Stop();
        if (pending_fns_.empty()) {
          has_pending_fns_.store(false, std::memory_order_release);
          break;
        }
        fn = std::move(pending_fns_.front());
        pending_fns_.pop();
        if (pending_fns_.empty()) {
          has_pending_fns_.store(false, std::memory_order_release);
        }
      }
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "xe::gpu::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      const uint64_t fs_stall_begin = FrameStatsBegin();
      cp_wait_stats::Scope ring_wait(cp_wait_stats::kRing);
      {
        cp_wait_stats::FenceSiteScope fence_site(cp_wait_stats::kFenceIdle);
        PrepareForWait();
      }
      uint32_t loop_count = 0;
      if (const int32_t stall_spin_us = cvars::gpu_stall_spin_us;
          stall_spin_us > 0 &&
          !xe::cpu::backend::thermal_hot.load(std::memory_order_relaxed) &&
          Clock::QueryHostUptimeMillis() >=
              xe::cpu::backend::frame_slack_until_ms.load(
                  std::memory_order_relaxed)) {
        // Frames short of time: what the guest sends next is most likely
        // what the frame waits for. Stay awake for it a little.
        static const uint64_t ticks_per_us = std::max(
            Clock::host_tick_frequency_raw() / 1000000, uint64_t(1));
        const uint64_t spin_end = Clock::host_tick_count_raw() +
                                  uint64_t(stall_spin_us) * ticks_per_us;
        do {
          write_ptr_index = write_ptr_index_.load();
          if (!(write_ptr_index == 0xBAADF00D ||
                read_ptr_index_ == write_ptr_index) ||
              has_pending_fns_.load(std::memory_order_acquire) ||
              !worker_running_) {
            break;
          }
#if XE_ARCH_ARM64
          for (uint32_t n = 0; n < 4; ++n) {
            __asm__ __volatile__("isb sy" ::: "memory");
          }
#endif
        } while (Clock::host_tick_count_raw() < spin_end);
      }
      do {
        if (worker_running_ &&
            !has_pending_fns_.load(std::memory_order_acquire) &&
            !(write_ptr_index == 0xBAADF00D ||
              read_ptr_index_ == write_ptr_index)) {
          // Found during the spin above.
          break;
        }
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > cvars::gpu_stall_spin_iterations) {
          constexpr int wait_time_ms = 2;
          xe::threading::Wait(write_ptr_index_event_.get(), true,
                              std::chrono::milliseconds(wait_time_ms));
        } else {
          xe::threading::MaybeYield();
        }
        ServiceMemoryRequests();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ &&
               !has_pending_fns_.load(std::memory_order_acquire) &&
               (write_ptr_index == 0xBAADF00D ||
                read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      ring_wait.Stop();
      if (fs_stall_begin) {
        const uint64_t fs_stall_ns = FrameStatsNow() - fs_stall_begin;
        frame_time_stats_.stall_ns += fs_stall_ns;
      }
      if (!worker_running_ ||
          has_pending_fns_.load(std::memory_order_acquire)) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    const uint64_t fs_exec_begin = FrameStatsBegin();
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);
    if (heartbeat_) {
      hb_read_ptr_.store(read_ptr_index_, std::memory_order_relaxed);
      hb_primary_execs_.fetch_add(1, std::memory_order_relaxed);
    }
    if (fs_exec_begin) {
      frame_time_stats_.exec_ns += FrameStatsNow() - fs_exec_begin;
    }

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    // Keep in mind that the gpu also updates the cpu-side copy if the write
    // pointer and read pointer would be equal
    if (read_ptr_writeback_ptr_) {
      BeforeCpuMemoryWrite(read_ptr_writeback_ptr_, sizeof(uint32_t));
      xe::store_and_swap<uint32_t>(
          memory_->TranslatePhysical(read_ptr_writeback_ptr_), read_ptr_index_);
      xe::threading::BumpGuestWakeGeneration();
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  if (heartbeat_thread.joinable()) {
    hb_stop_ = true;
    heartbeat_thread.join();
  }
  ShutdownContext();
}

#if defined(__ANDROID__)
namespace {
// Heartbeat backtrace of the command processor thread while it is stuck: a
// signal makes it unwind its own stack (diagnostic only, not signal safe in
// theory, fine in practice for a thread blocked in a syscall).
constexpr int kHeartbeatBacktraceMax = 40;
uintptr_t heartbeat_backtrace[kHeartbeatBacktraceMax];
std::atomic<int> heartbeat_backtrace_count{-1};
_Unwind_Reason_Code HeartbeatUnwind(_Unwind_Context* context, void* arg) {
  int& n = *static_cast<int*>(arg);
  if (n >= kHeartbeatBacktraceMax) return _URC_END_OF_STACK;
  heartbeat_backtrace[n++] = uintptr_t(_Unwind_GetIP(context));
  return _URC_NO_REASON;
}
void HeartbeatBacktraceSignal(int) {
  int n = 0;
  _Unwind_Backtrace(HeartbeatUnwind, &n);
  heartbeat_backtrace_count.store(n, std::memory_order_release);
}
}  // namespace
#endif

void CommandProcessor::HeartbeatThreadMain() {
  xe::threading::set_name("GPU Heartbeat");
  uint64_t last_execs = 0, last_draws = 0;
  uint32_t last_counter = counter_;
  uint32_t idle_beats = 0;
#if defined(__ANDROID__)
  const int backtrace_signal = SIGRTMIN + 10;
  struct sigaction action = {};
  action.sa_handler = HeartbeatBacktraceSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  sigaction(backtrace_signal, &action, nullptr);
#endif
  while (!hb_stop_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
#if defined(__ANDROID__)
    // Stuck in a packet for 3 beats (no primary buffer returned, no draw):
    // log where the CP thread is, every 5 beats.
    const bool cp_idle =
        hb_primary_execs_.load(std::memory_order_relaxed) == last_execs &&
        hb_draws_.load(std::memory_order_relaxed) == last_draws &&
        read_ptr_index_ != write_ptr_index_.load();
    idle_beats = cp_idle ? idle_beats + 1 : 0;
    if (idle_beats >= 3 && idle_beats % 5 == 3 && hb_cp_tid_.load()) {
      heartbeat_backtrace_count.store(-1, std::memory_order_relaxed);
      syscall(SYS_tgkill, getpid(), hb_cp_tid_.load(), backtrace_signal);
      for (int i = 0; i < 100 && heartbeat_backtrace_count.load(
                                     std::memory_order_acquire) < 0;
           ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const int frames =
          heartbeat_backtrace_count.load(std::memory_order_acquire);
      std::string trace;
      for (int i = 0; i < frames; ++i) {
        Dl_info info = {};
        const uintptr_t pc = heartbeat_backtrace[i];
        if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
          const char* base = strrchr(info.dli_fname, '/');
          trace += fmt::format(" {}+{:X}", base ? base + 1 : info.dli_fname,
                               pc - uintptr_t(info.dli_fbase));
        } else {
          trace += fmt::format(" {:X}", pc);
        }
      }
      XELOGI("CpHeartbeat: CP stuck, backtrace ({} frames):{}", frames, trace);
    }
#endif
    long rss_pages = 0;
#if defined(__linux__)
    if (FILE* statm = fopen("/proc/self/statm", "r")) {
      long size_pages = 0;
      if (fscanf(statm, "%ld %ld", &size_pages, &rss_pages) != 2) rss_pages = 0;
      fclose(statm);
    }
#endif
    const uint64_t execs = hb_primary_execs_.load(std::memory_order_relaxed);
    const uint64_t draws = hb_draws_.load(std::memory_order_relaxed);
    // read_ptr_index_ and counter_ are read racily: diagnostic only.
    const uint32_t counter = counter_;
    XELOGI(
        "CpHeartbeat: rptr={} (after exec {}) wptr={} primary execs +{} "
        "draws +{} wait_reg_mem=0x{:08X} ref=0x{:08X} vblanks +{} cp_paused={} "
        "gs_paused={} pending_fns={} opcode=0x{:02X} at {} gpu_wait={} "
        "cp_wait={} rss={} MB",
        read_ptr_index_, hb_read_ptr_.load(std::memory_order_relaxed),
        write_ptr_index_.load(), execs - last_execs, draws - last_draws,
        hb_wait_addr_.load(std::memory_order_relaxed),
        hb_wait_ref_.load(std::memory_order_relaxed), counter - last_counter,
        paused_.load(), graphics_system_ && graphics_system_->is_paused(),
        has_pending_fns_.load(),
        int(hb_opcode_.load(std::memory_order_relaxed)) - 1,
        hb_opcode_offset_.load(std::memory_order_relaxed),
        hb_gpu_wait_.load(std::memory_order_relaxed),
        cp_wait_stats::cp_current_wait.load(std::memory_order_relaxed),
        uint64_t(rss_pages) * 4096 >> 20);
    last_execs = execs;
    last_draws = draws;
    last_counter = counter;
  }
}

bool CommandProcessor::PauseForRuntimeSnapshot(std::function<bool()> operation,
                                               std::chrono::milliseconds timeout) {
  auto expected = SnapshotRequestState::kNone;
  if (paused_.load() || timeout.count() <= 0 ||
      !snapshot_request_state_.compare_exchange_strong(
          expected, SnapshotRequestState::kPending)) {
    XELOGE("GPU snapshot: CP paused, request pending, or invalid timeout");
    return false;
  }
  struct Request {
    std::mutex mutex;
    bool started = false, cancelled = false, ok = false;
    std::unique_ptr<threading::Event> done =
        threading::Event::CreateManualResetEvent(false);
  };
  auto request = std::make_shared<Request>();
  CallInThread([this, request, operation = std::move(operation)] {
    {
      std::lock_guard lock(request->mutex);
      if (request->cancelled) return;
      request->started = true;
    }
    try { request->ok = operation(); }
    catch (...) { request->ok = false; }
    paused_.store(true);
    request->done->Set();
    threading::Wait(pause_resume_event_.get(), false);
  });
  // Wake WAIT_REG_MEM even if the guest requested a long polling interval.
  write_ptr_index_event_->Set();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (threading::Wait(request->done.get(), false,
                        std::chrono::milliseconds(1)) !=
         threading::WaitResult::kSuccess) {
    if (snapshot_request_state_.load(std::memory_order_acquire) !=
            SnapshotRequestState::kBlockedInNestedBuffer &&
        std::chrono::steady_clock::now() < deadline) {
      continue;
    }
    std::lock_guard lock(request->mutex);
    if (!request->started) {
      request->cancelled = true;
      const auto state = snapshot_request_state_.exchange(
          SnapshotRequestState::kNone);
      if (state == SnapshotRequestState::kBlockedInNestedBuffer) {
        XELOGE("GPU snapshot: WAIT_REG_MEM in indirect/replay buffer; "
               "continuation is not serialized, snapshot cancelled");
      } else {
        XELOGE("GPU snapshot: command processor did not reach its barrier in time");
      }
      return false;
    }
    break;
  }
  // A started operation owns references into the caller's image, so a timeout
  // must never let it outlive the caller. Vulkan completion is handled inside
  // the operation and leaves the outer transaction stopped on GPU failure.
  threading::Wait(request->done.get(), false);
  snapshot_request_state_.store(SnapshotRequestState::kNone,
                                std::memory_order_release);
  return request->ok;
}

bool CommandProcessor::CaptureRuntimeSnapshot(snapshot::GpuState& out) {
  std::copy(std::begin(register_file_->values), std::end(register_file_->values),
            out.registers.begin());
  out.ring = {primary_buffer_ptr_, primary_buffer_size_, read_ptr_index_,
              read_ptr_update_freq_, read_ptr_writeback_ptr_, write_ptr_index_.load()};
  for (uint32_t i = 0; i < 256; ++i) out.gamma[i] = gamma_ramp_256_entry_table_[i].value;
  for (uint32_t i = 0; i < 128; ++i)
    for (uint32_t j = 0; j < 3; ++j) out.gamma_pwl[i][j] = gamma_ramp_pwl_rgb_[i][j].value;
  out.gamma_component = gamma_ramp_rw_component_;
  out.bin_select = bin_select_; out.bin_mask = bin_mask_; out.counter = counter_;
  out.vertex_ucode_address = active_vertex_shader_ucode_address_;
  out.vertex_shader = active_vertex_shader_ ? active_vertex_shader_->ucode_data() : std::vector<uint32_t>{};
  out.pixel_shader = active_pixel_shader_ ? active_pixel_shader_->ucode_data() : std::vector<uint32_t>{};
  return true;
}

bool CommandProcessor::RestoreRuntimeSnapshot(const snapshot::GpuState& state) {
  if (!state.Validate()) {
    XELOGE("GPU restore: snapshot GPU state failed validation");
    return false;
  }
  RestoreRegisters(0, state.registers.data(), uint32_t(state.registers.size()), true);
  primary_buffer_ptr_ = state.ring[0]; primary_buffer_size_ = state.ring[1];
  read_ptr_index_ = state.ring[2]; read_ptr_update_freq_ = state.ring[3];
  read_ptr_writeback_ptr_ = state.ring[4]; write_ptr_index_.store(state.ring[5]);
  std::array<reg::DC_LUT_30_COLOR, 256> gamma;
  std::array<std::array<reg::DC_LUT_PWL_DATA, 3>, 128> pwl;
  for (uint32_t i = 0; i < 256; ++i) gamma[i].value = state.gamma[i];
  for (uint32_t i = 0; i < 128; ++i)
    for (uint32_t j = 0; j < 3; ++j) pwl[i][j].value = state.gamma_pwl[i][j];
  RestoreGammaRamp(gamma.data(), pwl[0].data(), state.gamma_component);
  bin_select_ = state.bin_select; bin_mask_ = state.bin_mask; counter_ = state.counter;
  active_vertex_shader_ = state.vertex_shader.empty() ? nullptr :
      LoadShader(xenos::ShaderType::kVertex, state.vertex_shader.data(), uint32_t(state.vertex_shader.size()));
  active_pixel_shader_ = state.pixel_shader.empty() ? nullptr :
      LoadShader(xenos::ShaderType::kPixel, state.pixel_shader.data(), uint32_t(state.pixel_shader.size()));
  active_vertex_shader_ucode_address_ = state.vertex_ucode_address;
  const bool shaders = (state.vertex_shader.empty() || active_vertex_shader_) &&
                       (state.pixel_shader.empty() || active_pixel_shader_);
  if (!shaders) XELOGE("GPU restore: the active shaders could not be reloaded");
  return shaders;
}

void CommandProcessor::Pause() {
  if (paused_.exchange(true)) {
    return;
  }

  threading::Fence fence;
  CallInThread([this, &fence]() {
    PrepareForPause();
    fence.Signal();
    // Event, not thread Suspend(): a Resume() landing before the worker
    // suspends would be lost forever; an early event Set() is remembered.
    cp_wait_stats::Scope pause_wait(cp_wait_stats::kPause);
    threading::Wait(pause_resume_event_.get(), false);
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_.exchange(false)) {
    return;
  }

  pause_resume_event_->Set();
}

bool CommandProcessor::Save(ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  ResetZPDState();
  return true;
}

void CommandProcessor::ShutdownContext() { ResetZPDState(); }

void CommandProcessor::Pm4TraceLog(uint32_t opcode, uint32_t count,
                                   bool predicated) {
  switch (opcode) {
    case xenos::PM4_DRAW_INDX:
    case xenos::PM4_DRAW_INDX_2:
    case xenos::PM4_DRAW_INDX_BIN:
    case xenos::PM4_DRAW_INDX_2_BIN:
    case xenos::PM4_EVENT_WRITE:
    case xenos::PM4_EVENT_WRITE_SHD:
    case xenos::PM4_EVENT_WRITE_EXT:
    case xenos::PM4_EVENT_WRITE_ZPD:
    case xenos::PM4_SET_BIN_MASK:
    case xenos::PM4_SET_BIN_SELECT:
    case xenos::PM4_SET_BIN_MASK_LO:
    case xenos::PM4_SET_BIN_MASK_HI:
    case xenos::PM4_SET_BIN_SELECT_LO:
    case xenos::PM4_SET_BIN_SELECT_HI:
    case xenos::PM4_SET_BIN_BASE_OFFSET:
    case xenos::PM4_COND_EXEC:
    case xenos::PM4_COND_WRITE:
    case xenos::PM4_REG_RMW:
    case xenos::PM4_MEM_WRITE:
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD:
    case xenos::PM4_VIZ_QUERY:
    case xenos::PM4_WAIT_REG_MEM:
    case xenos::PM4_XE_SWAP:
      break;
    default:
      ++pm4_trace_skipped_;
      return;
  }
  uint32_t words[4] = {};
  uint32_t word_count = std::min(count, uint32_t(4));
  if (reader_.read_count() >= word_count * sizeof(uint32_t)) {
    const uint32_t* payload =
        reinterpret_cast<const uint32_t*>(reader_.read_ptr());
    for (uint32_t i = 0; i < word_count; ++i) {
      words[i] = xe::byte_swap(payload[i]);
    }
  }
  XELOGI(
      "PM4T a={:08X} op={:02X} n={} p={} sel={:X} mask={:X} "
      "d={:08X} {:08X} {:08X} {:08X} others={}",
      GuestReadPtrOffset(-4), opcode, count, uint32_t(predicated), bin_select_,
      bin_mask_, words[0], words[1], words[2], words[3], pm4_trace_skipped_);
  pm4_trace_skipped_ = 0;
}

uint32_t CommandProcessor::GuestReadPtrOffset(int32_t offset) const {
  return uint32_t(reader_.read_ptr() -
                  reinterpret_cast<uintptr_t>(memory_->physical_membase()) +
                  offset);
}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);

  std::memset(kernel_state_->memory()->TranslatePhysical(primary_buffer_ptr_),
              0, primary_buffer_size_);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr,
                                                  uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

XE_NOINLINE XE_COLD void CommandProcessor::LogKickoffInitator(uint32_t value) {
  cpu::backend::GuestPseudoStackTrace st;

  if (logging::ShouldLog(LogLevel::Debug) &&
      kernel_state_->processor()->backend()->PopulatePseudoStacktrace(&st)) {
    logging::LoggerBatch<LogLevel::Debug> log_initiator{};

    log_initiator("Updating read ptr to {}, initiator stacktrace below\n",
                  value);

    for (uint32_t i = 0; i < st.count; ++i) {
      log_initiator("\t{:08X}\n", st.return_addrs[i]);
    }

    if (st.truncated_flag) {
      log_initiator("\t(Truncated stacktrace to {} entries)\n",
                    cpu::backend::MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES);
    }
    log_initiator.submit('d');
  }
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  XE_UNLIKELY_IF(cvars::log_ringbuffer_kickoff_initiator_bts) {
    LogKickoffInitator(value);
  }
  write_ptr_index_ = value;
  write_ptr_index_event_->SetBoostPriority();
}

void CommandProcessor::LogRegisterSet(uint32_t register_index, uint32_t value) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    const RegisterInfo* reginfo = RegisterFile::GetRegisterInfo(register_index);

    if (!reginfo) {
      XELOGD("Unknown_Reg{:04X} <- {:08X}\n", register_index, value);
    } else {
      XELOGD("{} <- {:08X}\n", reginfo->name, value);
    }
  }
#endif
}

void CommandProcessor::LogRegisterSets(uint32_t base_register_index,
                                       const uint32_t* values,
                                       uint32_t n_values) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    auto target = logging::internal::GetThreadBuffer();

    auto target_ptr = target.first;

    size_t total_size = 0;

    size_t rem_size = target.second;

    for (uint32_t i = 0; i < n_values; ++i) {
      uint32_t register_index = base_register_index + i;

      uint32_t value = xe::load_and_swap<uint32_t>(&values[i]);

      const RegisterInfo* reginfo =
          RegisterFile::GetRegisterInfo(register_index);

      if (!reginfo) {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size,
                                       "Unknown_Reg{:04X} <- {:08X}\n",
                                       register_index, value);
        target_ptr = tmpres.out;
        rem_size -= tmpres.size;
        total_size += tmpres.size;

      } else {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size, "{} <- {:08X}\n",
                                       reginfo->name, value);
        rem_size -= tmpres.size;
        target_ptr = tmpres.out;
        total_size += tmpres.size;
      }
    }
    logging::internal::AppendLogLine(LogLevel::Debug, 'd', total_size);
  }
#endif
}

void CommandProcessor::HandleSpecialRegisterWrite(uint32_t index,
                                                  uint32_t value) {
  RegisterFile& regs = *register_file_;
  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      // A REG4 value that is not one of the known per-frame callbacks is
      // interesting once, but "known" is a hardcoded list from one title:
      // any other game's callback looks rare on every single write. Log the
      // first sighting of each distinct value, so an install is still
      // reported without spending a formatted log line per frame forever.
      static std::atomic<uint32_t> scratch_log_budget{64};
      bool rare_install =
          scratch_reg == 4 && value != 0x0BADF00D && value != 0x83744BF8 &&
          value != 0x837C59C8;
      if (rare_install) {
        static std::mutex seen_mutex;
        static std::set<uint32_t> seen_installs;
        std::lock_guard<std::mutex> lock(seen_mutex);
        rare_install = seen_installs.insert(value).second;
      }
      if (rare_install ||
          (scratch_log_budget.load() > 0 && scratch_log_budget-- > 0)) {
        XELOGI("CP: scratch writeback REG{} value={:08X} -> phys {:08X}{}",
               scratch_reg, value, mem_addr,
               rare_install ? "  <== RARE INSTALL" : "");
      }
      BeforeCpuMemoryWrite(mem_addr, sizeof(uint32_t));
      xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      xe::threading::BumpGuestWakeGeneration();
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_MEM_REGs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        regs.Store(index, regs.values[index] | UINT32_C(0x80000000));
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color =
              regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl]
                                 [gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index =
              (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
              ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask =
            regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(
            XE_GPU_REG_DC_LUT_RW_INDEX,
            xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}
void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // chrispy: rearrange check order, place set after checks

  if (XE_LIKELY(index < RegisterFile::kRegisterCount)) {
    register_file_->Store(index, value);

    // quick pre-test
    // todo: figure out just how unlikely this is. if very (it ought to be,
    // theres a ton of registers other than these) make this predicate
    // branchless and mark with unlikely, then make HandleSpecialRegisterWrite
    // noinline yep, its very unlikely. these ORS here are meant to be bitwise
    // ors, so that we do not do branching evaluation of the conditions (we will
    // almost always take all of the branches)

    unsigned expr = (index - XE_GPU_REG_SCRATCH_REG0 < 8) |
                    (index == XE_GPU_REG_COHER_STATUS_HOST) |
                    ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
                     (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));
    // chrispy: reordered for msvc branch probability (assumes if is taken and
    // else is not)
    if (XE_LIKELY(expr == 0)) {
      XE_MSVC_REORDER_BARRIER();

    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  } else {
    XELOGW("CommandProcessor::WriteRegister index out of bounds: {}", index);
    return;
  }
}
void CommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    this->WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                  uint32_t base,
                                                  uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = ring->ReadAndSwap<uint32_t>();
    WriteRegister(base + i, data);
  }
}

void CommandProcessor::WriteALURangeFromRing(xe::RingBuffer* ring,
                                             uint32_t base,
                                             uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_times);
}

void CommandProcessor::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_times);
}

void CommandProcessor::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_times);
}

void CommandProcessor::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_times);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_times);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index,
                                            uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}
XE_NOINLINE
void CommandProcessor::WriteOneRegisterFromRing(uint32_t base,
                                                uint32_t num_times) {
  for (uint32_t m = 0; m < num_times; m++) {
    uint32_t reg_data = reader_.ReadAndSwap<uint32_t>();
    uint32_t target_index = base;
    WriteRegister(target_index, reg_data);
  }
}
void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = xe::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  XELOGGPU("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
           base_host + size_host, size_host, action);

  // Mark coherent.
  register_file_->Store(XE_GPU_REG_COHER_STATUS_HOST, 0);
}

void CommandProcessor::PrepareForWait() {
  ServiceMemoryRequests();
  trace_writer_.Flush();
  // Only refresh completion if there is a strict ZPD retire pending so
  // PumpPendingRetire sees the latest progress without adding extra overhead.
  if (zpd_pending_retire_handle_ != kInvalidReportHandle) {
    PollCompletedSubmission();
  }
  // Give strict ZPD a chance to retire a pending report before the guest's
  // loop polls again.
  PumpPendingRetire();
}

void CommandProcessor::ReturnFromWait() {}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(
      0, reinterpret_cast<const uint32_t*>(register_file_->values),
      RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(),
                               gamma_ramp_pwl_rgb(), gamma_ramp_rw_component_);
}

CommandProcessor::PendingZPDSlot CommandProcessor::GetPendingZPDSlot(
    uint32_t slot_base, uint32_t end_record) const {
  PendingZPDSlot pending_slot;

  for (const auto& report_pair : logical_zpd_reports_) {
    const ZPDReport& report = report_pair.second;
    if (!report.ended || report.pending_segments == 0 ||
        report.slot_base != slot_base) {
      continue;
    }

    // Wait on the oldest unresolved report for this slot first.
    if (pending_slot.report_handle == kInvalidReportHandle ||
        report_pair.first < pending_slot.report_handle) {
      pending_slot.report_handle = report_pair.first;
    }

    // Slot reuse needs to be handled carefully in fast mode. Keep the biggest
    // cached delta, not the newest one. A stale zero is a lot more dangerous
    // than a stale nonzero.
    if (report.has_cached_delta) {
      if (!pending_slot.has_cached_delta ||
          report.cached_delta > pending_slot.cached_delta) {
        pending_slot.cached_delta = report.cached_delta;
      }
      pending_slot.has_cached_delta = true;
    }

    if (report.end_record) {
      auto report_cache_it =
          fast_zpd_report_cached_values_.find(report.end_record);
      if (report_cache_it != fast_zpd_report_cached_values_.end()) {
        if (!pending_slot.has_cached_delta ||
            report_cache_it->second > pending_slot.cached_delta) {
          pending_slot.cached_delta = report_cache_it->second;
        }
        pending_slot.has_cached_delta = true;
      }
    }
  }

  auto end_record_cache_it = fast_zpd_report_cached_values_.find(end_record);
  if (end_record_cache_it != fast_zpd_report_cached_values_.end()) {
    if (!pending_slot.has_cached_delta ||
        end_record_cache_it->second > pending_slot.cached_delta) {
      pending_slot.cached_delta = end_record_cache_it->second;
    }
    pending_slot.has_cached_delta = true;
  }

  return pending_slot;
}

bool CommandProcessor::BeginZPDReport(uint32_t report_address) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  // Track any delta to carry forward if the same slot is immediately reused.
  uint32_t carried_cached_delta = 0;
  bool has_carried_cached_delta = false;
  uint32_t carried_from_slot_base = 0;

  if (zpd_active_segment_.logical_active) {
    // New BEGIN while a prior report is open. Hardware has one register for
    // the query address, so a new BEGIN implicitly ends the prior one.
    if (zpd_active_segment_.end_record) {
      EndZPDReport(zpd_active_segment_.end_record, true);
    } else {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD: BeginZPDReport forcing close without end record "
            "handle={}",
            zpd_active_segment_.report_handle);
      }

      carried_from_slot_base = zpd_active_segment_.slot_base;

      auto dying_report =
          logical_zpd_reports_.find(zpd_active_segment_.report_handle);
      // Carry prior delta forward so the slot doesn't briefly look occluded.
      if (dying_report != logical_zpd_reports_.end() &&
          dying_report->second.has_cached_delta) {
        carried_cached_delta = dying_report->second.cached_delta;
        has_carried_cached_delta = true;
      }

      if (zpd_active_segment_.segment_active) {
        // Deactivate the segment before DiscardZPDQuery so that
        // EndSubmission -> CloseQuerySegment does not re-enter and
        // issue a second EndQuery on the same slot.
        zpd_active_segment_.segment_active = false;
        if (DiscardZPDQuery()) {
          zpd_stats_.segments_ended++;
        } else {
          zpd_stats_.failed++;
        }
      }
      logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
      zpd_active_segment_ = {};
    }
  }

  uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
  uint32_t begin_record = XenosZPDReport::GetBeginRecordBase(slot_base);
  uint32_t end_record = XenosZPDReport::GetEndRecordBase(slot_base);
  if (!slot_base) {
    return false;
  }

  // Resolve same slot hazards before invalidating pending writes from the prior
  // lifetime. For finished strict queries with unpolled completion, refresh now
  // and drain, avoiding unnecessary AwaitQueryResolve blocking.
  if (GetZPDMode() == ZPDMode::kStrict) {
    PollCompletedSubmission();
  } else {
    PumpQueryResolves();
  }

  PendingZPDSlot pending_slot = GetPendingZPDSlot(slot_base, end_record);

  if (pending_slot.report_handle != kInvalidReportHandle) {
    zpd_stats_.same_slot_reuse++;
    if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
      if (pending_slot.has_cached_delta) {
        carried_cached_delta = pending_slot.cached_delta;
        has_carried_cached_delta = true;
        carried_from_slot_base = slot_base;
      }
    } else {
      while (pending_slot.report_handle != kInvalidReportHandle) {
        auto report_it = logical_zpd_reports_.find(pending_slot.report_handle);
        if (report_it == logical_zpd_reports_.end()) {
          break;
        }

        uint64_t wait_for_submission =
            report_it->second.last_segment_end_submission;

        bool wait_succeeded =
            AwaitQueryResolve(pending_slot.report_handle, wait_for_submission);

        if (!wait_succeeded) {
          if (pending_slot.cached_delta != 0) {
            carried_cached_delta = pending_slot.cached_delta;
            has_carried_cached_delta = true;
            carried_from_slot_base = slot_base;
          }
          break;
        }

        PumpQueryResolves();

        pending_slot = GetPendingZPDSlot(slot_base, end_record);
      }
    }
  }

  // Bump slot sequence — invalidates pending writes from prior lifetime.
  uint64_t slot_sequence_id = ++zpd_slot_sequences_[slot_base];

  // By default, BEGIN drops the cached value so an orphaned END doesn't replay
  // something from a prior lifetime. The alternate fast path keeps it around
  // long enough for an async zero to help the next unresolved write.
  if (GetZPDMode() != ZPDMode::kFastAlt) {
    auto cache_it = fast_zpd_report_cached_values_.find(end_record);
    if (cache_it != fast_zpd_report_cached_values_.end() &&
        cache_it->second == 0) {
      fast_zpd_report_cached_values_.erase(cache_it);
    }
  }

  ReportHandle report_handle = zpd_next_report_handle_++;
  if (report_handle == kInvalidReportHandle) {
    report_handle = zpd_next_report_handle_++;
  }

  ZPDReport& logical = logical_zpd_reports_[report_handle];
  logical.slot_base = slot_base;
  logical.slot_sequence_id = slot_sequence_id;
  logical.begin_record = begin_record;
  logical.end_record = end_record;
  logical.begin_value = zpd_slot_values_[slot_base];
  logical.accumulated_samples = 0;
  logical.first_segment_end_submission = 0;
  logical.last_segment_end_submission = 0;
  logical.pending_segments = 0;
  logical.cached_delta = 0;
  logical.has_cached_delta = false;
  logical.ended = false;

  if (slot_base == carried_from_slot_base && has_carried_cached_delta) {
    logical.cached_delta = carried_cached_delta;
    logical.has_cached_delta = true;
  }

  zpd_active_segment_.report_handle = report_handle;
  zpd_active_segment_.slot_base = slot_base;
  zpd_active_segment_.begin_record = begin_record;
  zpd_active_segment_.end_record = end_record;
  zpd_active_segment_.segment_active = false;
  // Opens lazily. OpenQuerySegment will open it at the next valid opportunity.
  zpd_active_segment_.segment_pending_begin = true;
  zpd_active_segment_.logical_active = true;

  zpd_stats_.logical_begun++;
  OpenQuerySegment(true);
  return true;
}

// Guest END closes the logical lifetime, but the final value may still depend
// on in flight query segments.
bool CommandProcessor::EndZPDReport(uint32_t report_address,
                                    bool guest_forced_end) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  CommandProcessor::ReportHandle report_handle =
      zpd_active_segment_.report_handle;
  uint32_t stored_end_record = zpd_active_segment_.end_record;
  uint32_t report_record_base = XenosZPDReport::GetRecordBase(report_address);
  if (!report_record_base) {
    report_record_base = stored_end_record;
  }

  if (zpd_active_segment_.segment_active) {
    CloseQuerySegment();
  }

  zpd_active_segment_.segment_pending_begin = false;

  if (!report_record_base) {
    logical_zpd_reports_.erase(report_handle);
    if (cvars::occlusion_query_log) {
      XELOGI(
          "ZPD: EndZPDReport dropping handle={} with unknown record "
          "base forced={}",
          report_handle, guest_forced_end);
    }
    zpd_active_segment_ = {};
    return false;
  }

  bool resolved_immediately = false;
  uint32_t begin_record = 0;
  uint32_t begin_value = 0;
  uint32_t final_value = 0;
  uint32_t cached_delta = 0;
  bool has_cached_delta = false;

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    zpd_active_segment_ = {};
    return false;
  }

  ZPDReport& logical = it->second;
  logical.ended = true;
  logical.end_record = report_record_base;
  begin_record = logical.begin_record;
  begin_value = logical.begin_value;

  if (logical.pending_segments == 0) {
    resolved_immediately = true;
    // Segments were already normalized as they resolved.
    final_value = static_cast<uint32_t>(
        std::min<uint64_t>(logical.accumulated_samples, UINT32_MAX));

    cached_delta = final_value;
    has_cached_delta = true;
    logical.cached_delta = cached_delta;
    logical.has_cached_delta = true;
    if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
        !fast_zpd_report_cached_values_.count(report_record_base)) {
      fast_zpd_report_cached_values_.clear();
    }
    fast_zpd_report_cached_values_[report_record_base] = cached_delta;
    final_value = cached_delta;
  } else {
    if (logical.has_cached_delta) {
      cached_delta = logical.cached_delta;
      has_cached_delta = true;
    }
    auto cache_it = fast_zpd_report_cached_values_.find(report_record_base);
    if (cache_it != fast_zpd_report_cached_values_.end()) {
      cached_delta = cache_it->second;
      has_cached_delta = true;
    }
  }

  if (resolved_immediately) {
    CommitZPDReport(logical, final_value);
    logical_zpd_reports_.erase(it);
  }

  bool has_cross_slot_end =
      stored_end_record && stored_end_record != report_record_base;
  if (has_cross_slot_end) {
    WriteZPDReport(0, stored_end_record, 0, begin_value, false);
  }

  if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
    bool write_begin = begin_record && report_record_base &&
                       begin_record != report_record_base;
    // Unknown still means visible in fast mode. Reusing cached zeroes can help
    // flares stop shining through walls, but it also tends to break occlusion
    // culling, so only do it in the alternate fast path.
    uint32_t speculative = cached_delta;
    if (!resolved_immediately) {
      speculative = 1;
      if (has_cached_delta &&
          (cached_delta != 0 || GetZPDMode() == ZPDMode::kFastAlt)) {
        speculative = cached_delta;
      }
    }
    WriteZPDReport(begin_record, report_record_base, begin_value, speculative,
                   write_begin);
  } else if (!resolved_immediately) {
    PumpQueryResolves();

    // Recheck after the drain. The report may have resolved synchronously if
    // all segments were already complete by the time we got here.
    if (!logical_zpd_reports_.count(report_handle)) {
      // OnZPDQueryResolved already committed and erased the report; nothing
      // left to defer.
    } else if (zpd_pending_retire_handle_ != report_handle) {
      zpd_pending_retire_handle_ = report_handle;
      zpd_pending_retire_stalls_ = 0;
      zpd_pending_retire_start_ms_ = Clock::QueryHostUptimeMillis();
    }
  }

  zpd_stats_.logical_ended++;
  zpd_active_segment_ = {};
  return true;
}

void CommandProcessor::OpenQuerySegment(bool can_close_submission) {
  if (GetZPDMode() == ZPDMode::kFake || zpd_force_fake_fallback_ ||
      !zpd_active_segment_.logical_active ||
      !zpd_active_segment_.segment_pending_begin || !CanOpenZPDQuery()) {
    return;
  }

  EnsureZPDQueryResources();

  if (!IsZPDQueryPoolReady()) {
    zpd_stats_.failed++;
    zpd_force_fake_fallback_ = true;
    logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
    zpd_active_segment_ = {};
    return;
  }

  // Frees any slots from completed submissions before asking for new ones.
  PumpQueryResolves();

  QueryOpenResult open_result =
      OpenZPDQuery(zpd_active_segment_.report_handle, can_close_submission);
  switch (open_result) {
    case QueryOpenResult::kOpened:
      break;
    case QueryOpenResult::kDeferred:
      return;
    case QueryOpenResult::kPoolExhausted: {
      zpd_stats_.pool_exhausted++;
      if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
        // Fast mode favors forward progress over accuracy. Keep a minimal
        // accumulated value instead of waiting for a slot to become available.
        auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
        if (it != logical_zpd_reports_.end()) {
          it->second.accumulated_samples =
              std::max<uint64_t>(it->second.accumulated_samples, uint64_t{1});
        }
        zpd_active_segment_.segment_pending_begin = false;
        return;
      }
      zpd_stats_.failed++;
      return;
    }
    case QueryOpenResult::kFailed:
    default:
      zpd_stats_.failed++;
      return;
  }

  zpd_active_segment_.segment_active = true;
  zpd_active_segment_.segment_pending_begin = false;
  zpd_stats_.segments_begun++;
}

// Closes the active host segment without ending the logical report.
// BeginQuery/EndQuery can't cross D3D12 command list or Vulkan render pass
// boundaries. The result accumulates across all pieces.
void CommandProcessor::CloseQuerySegment() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_active_segment_.segment_active) {
    return;
  }

  uint64_t submission = 0;
  if (!CloseZPDQuery(zpd_active_segment_.report_handle, submission)) {
    zpd_active_segment_.segment_active = false;
    zpd_active_segment_.scale_area = 0;
    zpd_active_segment_.segment_pending_begin =
        zpd_active_segment_.logical_active;
    zpd_stats_.failed++;
    return;
  }

  auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
  if (it != logical_zpd_reports_.end()) {
    // Lets PumpPendingRetire drain early segments without blocking on the
    // final segment's submission.
    if (it->second.pending_segments == 0) {
      it->second.first_segment_end_submission = submission;
    }
    it->second.pending_segments++;
    it->second.last_segment_end_submission = submission;
  }

  zpd_active_segment_.segment_active = false;
  zpd_active_segment_.scale_area = 0;

  zpd_active_segment_.segment_pending_begin =
      zpd_active_segment_.logical_active;
  zpd_stats_.segments_ended++;
}

void CommandProcessor::UpdateZPDScale(uint32_t scale_area) {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_active_segment_.logical_active) {
    return;
  }
  if (zpd_active_segment_.segment_active && zpd_active_segment_.scale_area &&
      zpd_active_segment_.scale_area != scale_area) {
    // Draw scale changed in the middle of a report, so close the segment so
    // normalization divides correctly, and start a fresh one for this draw.
    CloseQuerySegment();
    OpenQuerySegment(false);
  }
  zpd_active_segment_.scale_area = scale_area;
}

void CommandProcessor::OnZPDQueryResolved(ReportHandle report_handle,
                                          uint64_t raw_samples,
                                          uint32_t scale_area) {
  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return;
  }

  ZPDReport& logical = it->second;

  if (logical.pending_segments) {
    logical.pending_segments--;
  }

  logical.accumulated_samples += NormalizeSampleCount(raw_samples, scale_area);

  if (logical.ended && logical.pending_segments == 0) {
    uint32_t final_value = static_cast<uint32_t>(
        std::min<uint64_t>(logical.accumulated_samples, UINT32_MAX));

    logical.cached_delta = final_value;
    logical.has_cached_delta = true;
    if (logical.end_record) {
      if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
          !fast_zpd_report_cached_values_.count(logical.end_record)) {
        fast_zpd_report_cached_values_.clear();
      }
      fast_zpd_report_cached_values_[logical.end_record] = final_value;
    }
    if (IsZPDReportCurrent(logical)) {
      CommitZPDReport(logical, final_value);
    }
    logical_zpd_reports_.erase(it);
  }
}

void CommandProcessor::PumpPendingRetire() {
  ReportHandle handle_to_await = zpd_pending_retire_handle_;
  if (handle_to_await == kInvalidReportHandle) {
    return;
  }

  auto logical_report = logical_zpd_reports_.find(handle_to_await);
  if (logical_report == logical_zpd_reports_.end()) {
    // If the report is already gone it retired through another path.
    // Clear so we don't spin on a handle that no longer exists.
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  uint64_t wait_for_submission =
      logical_report->second.last_segment_end_submission;
  uint64_t first_submission =
      logical_report->second.first_segment_end_submission;

  // Early segments can be retired here and, in the best case, the report
  // fully resolves without any wait.
  if (first_submission != 0 && first_submission < wait_for_submission &&
      first_submission <= GetCompletedSubmission()) {
    PumpQueryResolves();
    logical_report = logical_zpd_reports_.find(handle_to_await);
    if (logical_report == logical_zpd_reports_.end()) {
      zpd_pending_retire_handle_ = kInvalidReportHandle;
      zpd_pending_retire_stalls_ = 0;
      return;
    }
    wait_for_submission = logical_report->second.last_segment_end_submission;
  }

  if (AwaitQueryResolve(handle_to_await, wait_for_submission)) {
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  if (wait_for_submission == 0 ||
      GetCompletedSubmission() >= wait_for_submission) {
    ++zpd_pending_retire_stalls_;
  }

  // Abandon if the deadline has elapsed or the stall limit has been reached.
  // Both are checked to account for varied guest polling behavior.
  bool deadline_exceeded =
      (Clock::QueryHostUptimeMillis() - zpd_pending_retire_start_ms_ >=
       kStrictZPDRetireDeadlineMs);
  if (deadline_exceeded ||
      zpd_pending_retire_stalls_ >= kStrictZPDRetireMaxStalls) {
    if (cvars::occlusion_query_log) {
      XELOGI("ZPD: PumpPendingRetire {} handle={}, abandoning",
             deadline_exceeded ? "deadline exceeded" : "stall limit reached",
             handle_to_await);
    }
    // Write the cached delta to guest memory to avoid a sudden occlusion flash.
    if (IsZPDReportCurrent(logical_report->second)) {
      uint32_t fallback_delta = logical_report->second.cached_delta
                                    ? logical_report->second.cached_delta
                                    : 1;
      CommitZPDReport(logical_report->second, fallback_delta);
    }
    logical_zpd_reports_.erase(logical_report);
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
  }
}

void CommandProcessor::WriteZPDReport(uint32_t begin_record,
                                      uint32_t end_record, uint32_t begin_value,
                                      uint32_t delta_value,
                                      bool write_begin_record) {
  if (begin_record && write_begin_record)
    BeforeCpuMemoryWrite(begin_record, sizeof(xenos::xe_gpu_depth_sample_counts));
  if (end_record)
    BeforeCpuMemoryWrite(end_record, sizeof(xenos::xe_gpu_depth_sample_counts));
  xenos::xe_gpu_depth_sample_counts* begin =
      begin_record
          ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
                begin_record)
          : nullptr;
  if (!end_record) {
    return;
  }
  xenos::xe_gpu_depth_sample_counts* end =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
          end_record);

  XenosZPDReport::WriteReportDelta(begin, end, begin_value, delta_value,
                                   write_begin_record);
}

void CommandProcessor::CommitZPDReport(ZPDReport& report,
                                       uint32_t delta_value) {
  uint32_t end_record =
      report.end_record ? report.end_record
                        : XenosZPDReport::GetEndRecordBase(report.slot_base);
  WriteZPDReport(report.begin_record, end_record, report.begin_value,
                 delta_value, report.begin_record != 0);

  // Advance running total so the next BeginReport on this slot picks up
  // the correct begin_value.
  uint32_t saturated_delta = XenosZPDReport::SaturateSampleCount(delta_value);
  uint32_t end_value = report.begin_value + saturated_delta;
  if (saturated_delta > UINT32_MAX - report.begin_value) {
    zpd_stats_.counter_wraps++;
  }
  zpd_slot_values_[report.slot_base] = end_value;
}

bool CommandProcessor::IsZPDReportCurrent(const ZPDReport& report) const {
  auto seq_it = zpd_slot_sequences_.find(report.slot_base);
  uint64_t current_seq =
      seq_it != zpd_slot_sequences_.end() ? seq_it->second : 0;
  return current_seq == report.slot_sequence_id;
}

uint32_t CommandProcessor::NormalizeSampleCount(uint64_t samples,
                                                uint32_t scale_area) {
  if (samples == 0) {
    return 0;
  }

  uint64_t scale = scale_area;
  // Round, don't truncate. 1 guest sample at 2x = 4 host samples, need >= 1.
  uint64_t normalized = scale <= 1 ? samples : (samples + (scale >> 1)) / scale;

  return static_cast<uint32_t>(std::min<uint64_t>(normalized, UINT32_MAX));
}

#define COMMAND_PROCESSOR CommandProcessor
#include "pm4_command_processor_implement.h"
}  // namespace gpu
}  // namespace xe
