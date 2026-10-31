// Runtime snapshot protocol. No legacy TerminateTitle/quick_exit restore path.
#include "xenia/emulator.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include "xenia/apu/audio_system.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_snapshot.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/patcher/plugin_loader.h"
#include "xenia/snapshot/audio_state.h"
#include "xenia/snapshot/gpu_state.h"
#include "xenia/snapshot/state_stream.h"
#include "third_party/crypto/sha256.h"
#if __has_include("snapshot/build_identity.h")
#include "snapshot/build_identity.h"
#endif
DECLARE_bool(log_safepoint_pc);
DECLARE_bool(clock_no_scaling);
DECLARE_bool(allow_plugins);
DECLARE_string(xma_decoder);
namespace xe {
namespace {
std::vector<uint8_t>& Part(snapshot::Image& image, snapshot::Section section) {
  return image.sections[uint32_t(section) - 1];
}
const std::vector<uint8_t>& Part(const snapshot::Image& image, snapshot::Section section) {
  return image.sections[uint32_t(section) - 1];
}
bool DecodeClock(const snapshot::Image& image, uint64_t& ticks, uint64_t& time) {
  snapshot::Reader reader(Part(image, snapshot::Section::kClock));
  uint32_t version;
  return reader.U32(version) && version == 1 && reader.U64(ticks) &&
         reader.U64(time) && reader.remaining() == 0;
}
bool HashFile(sha256::SHA256& hash, const std::filesystem::path& path,
              const std::atomic<bool>& cancel) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::vector<char> buffer(1 << 20);
  while (file) {
    if (cancel.load(std::memory_order_relaxed)) return false;
    file.read(buffer.data(), buffer.size());
    hash.add(buffer.data(), size_t(file.gcount()));
  }
  return file.eof();
}
// Names, sizes and modification times of the launch media, in hashing order:
// a cheap check that a cached content hash still describes the same files.
bool MediaFiles(const std::filesystem::path& source, bool& directory,
                std::vector<std::filesystem::path>& files, std::string& key,
                std::string& error) {
  std::error_code ec;
  directory = std::filesystem::is_directory(source, ec);
  files.clear();
  if (directory) {
    std::filesystem::recursive_directory_iterator it(source, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (it->is_symlink(ec)) { error = "media symlink is not reproducible"; return false; }
      if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    if (ec) { error = "cannot enumerate launch media"; return false; }
    std::sort(files.begin(), files.end());
  } else {
    files.push_back(source);
  }
  key = source.string();
  for (const auto& path : files) {
    auto size = std::filesystem::file_size(path, ec);
    if (ec) { error = "cannot stat launch media"; return false; }
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec) { error = "cannot stat launch media"; return false; }
    key += '\n' + path.string() + '\t' + std::to_string(size) + '\t' +
           std::to_string(static_cast<long long>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(
                   time.time_since_epoch()).count()));
  }
  return true;
}
// Persistent cache of media content hashes, one "<sha256(key)> <hash>" line per
// title, so a game is read in full once instead of on every boot (a 7 GB image
// took 52-65 s of a CPU core on the phone, and a restore waited for it).
std::string HexBytes(const uint8_t* data, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    hex += kDigits[data[i] >> 4];
    hex += kDigits[data[i] & 15];
  }
  return hex;
}
std::string MediaKeyHex(const std::string& key) {
  sha256::SHA256 hash;
  hash.add(key.data(), key.size());
  std::array<uint8_t, 32> digest;
  hash.getHash(digest.data());
  return HexBytes(digest.data(), digest.size());
}
std::vector<std::string> ReadMediaHashCache(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream file(path);
  for (std::string line; std::getline(file, line);) {
    if (line.size() == 64 + 1 + 64 && line[64] == ' ') lines.push_back(line);
  }
  return lines;
}
bool LookupMediaHashCache(const std::filesystem::path& path, const std::string& key,
                          std::array<uint8_t, 32>& out) {
  if (path.empty()) return false;
  const std::string prefix = MediaKeyHex(key) + ' ';
  for (const auto& line : ReadMediaHashCache(path)) {
    if (line.compare(0, prefix.size(), prefix)) continue;
    for (size_t i = 0; i < out.size(); ++i) {
      unsigned value;
      if (std::sscanf(line.c_str() + prefix.size() + i * 2, "%2x", &value) != 1) return false;
      out[i] = uint8_t(value);
    }
    return true;
  }
  return false;
}
void StoreMediaHashCache(const std::filesystem::path& path, const std::string& key,
                         const std::array<uint8_t, 32>& hash) {
  if (path.empty()) return;
  const std::string key_hex = MediaKeyHex(key);
  std::vector<std::string> lines;
  for (auto& line : ReadMediaHashCache(path)) {
    if (line.compare(0, key_hex.size(), key_hex)) lines.push_back(std::move(line));
  }
  lines.push_back(key_hex + ' ' + HexBytes(hash.data(), hash.size()));
  // Keep the newest entries only; an evicted game is simply hashed again.
  if (lines.size() > 64) lines.erase(lines.begin(), lines.end() - 64);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  auto temp = path;
  temp += ".pending";
  {
    std::ofstream file(temp, std::ios::trunc);
    for (const auto& line : lines) file << line << '\n';
    if (!file.flush()) return;
  }
  std::filesystem::rename(temp, path, ec);
}
}

bool Emulator::RuntimeSnapshotCapability(std::string& reason, bool allow_paused) const {
#if !(XE_PLATFORM_xendroid || XE_PLATFORM_ANDROID || XE_PLATFORM_MAC)
  reason = "runtime snapshots are not implemented for this memory backend";
  return false;
#else
#ifndef XENIA_SNAPSHOT_BUILD_ID
  reason = "runtime build identity missing (requires configured CMake build)";
  return false;
#endif
  if (!runtime_snapshot_boot_enabled_ || !cvars::savestate_enable || !cvars::log_safepoint_pc) {
    reason = "savestate_enable must be enabled before boot with canonical JIT safepoints"; return false;
  }
  if (!cvars::guest_scheduler) { reason = "runtime snapshots require cooperative guest threads"; return false; }
  if (cvars::clock_no_scaling) { reason = "host clock mode cannot restore guest time"; return false; }
  // Permission to load plugins doesn't imply there is plugin state to save.
  // Check the live loader, including if permission changed after a plugin booted.
  if (plugin_loader_ && plugin_loader_->IsAnyPluginLoaded()) {
    reason = "loaded title plugins cannot currently be snapshotted"; return false;
  }
  if (runtime_snapshot_failed_stopped_) { reason = "runtime is stopped after a failed snapshot transaction"; return false; }
  if (!main_thread_ || !title_id_ || !kernel_state_ || !graphics_system_ || !audio_system_) {
    reason = "no running title/subsystems"; return false;
  }
  if (!allow_paused && (paused_.load() || graphics_system_->is_paused() || audio_system_->is_paused())) {
    reason = "resume lifecycle pause before requesting a snapshot"; return false;
  }
  if (active_gpu_backend_ != "vulkan") { reason = "runtime snapshots currently require Vulkan"; return false; }
  if (cvars::xma_decoder != "new") { reason = "runtime snapshots require new XMA decoder (audio restarts history)"; return false; }
  if (!graphics_system_->command_processor()->RuntimeSnapshotSupported() ||
      !graphics_system_->command_processor()->RuntimeSnapshotHealthy() || !memory_->RuntimeSnapshotHealthy()) {
    reason = "GPU scale/backend unsupported or a snapshot subsystem is unhealthy"; return false;
  }
  if (kernel_state_->content_manager() && kernel_state_->content_manager()->OpenPackageCount()) {
    reason = "mounted content packages cannot currently be snapshotted"; return false;
  }
  for (const auto& object : kernel_state_->object_table()->GetAllObjects()) {
    using Type = kernel::XObject::Type;
    switch (object->type()) {
      case Type::Module: case Type::Thread: case Type::Event: case Type::File:
      case Type::Mutant: case Type::NotifyListener: case Type::Semaphore:
      case Type::Socket: case Type::IOCompletion:
      case Type::SymbolicLink: case Type::Timer: break;
      case Type::Enumerator:
        if (static_cast<const kernel::XEnumerator*>(object.get())->snapshottable()) break;
        reason = "an enumerator with state other than its items is alive";
        return false;
      default:
        reason = "unsupported kernel object is alive (type " + std::to_string(uint32_t(object->type())) + ")";
        return false;
    }
  }
  if (!kernel::KernelSnapshot::ObjectsSnapshottable(kernel_state_.get(), reason)) return false;
  if (last_launch_path_.empty()) { reason = "title has no reproducible launch media"; return false; }
  reason.clear();
  return true;
#endif
}

bool Emulator::RuntimeSnapshotIdentity(snapshot::Identity& out, std::string& error) const {
#ifndef XENIA_SNAPSHOT_BUILD_ID
  error = "snapshot build identity unavailable";
  return false;
#else
  snapshot::Identity identity;
  identity.title_id = title_id_.value_or(0);
  identity.guest_page_size = uint32_t(memory::page_size());
  // States stay loadable across builds and settings, so one saved point per
  // game serves every later build. Bump kStateCompat whenever the layout or
  // meaning of any saved section changes; each section also validates its own
  // version, and settings that shape saved state (render target path and
  // scale, audio queue) are checked by their restore, which rolls back.
  static constexpr char kStateCompat[] = "xenia-runtime-state/2026-10-14";
  sha256::SHA256 build;
  build.add(kStateCompat, sizeof(kStateCompat) - 1);
  build.getHash(identity.build_hash.data());
  identity.config_hash = {};
  if (!RuntimeMediaHash(last_launch_path_, identity.media_hash, error)) return false;
  out = identity;
  return true;
#endif
}

void Emulator::ReleaseRuntimeSnapshot(bool resume) {
  if (!runtime_snapshot_frozen_) return;
  if (!resume) {
    runtime_snapshot_failed_stopped_ = true;
    // Keep every writer barrier held. Cold restore tears down these stopped
    // subsystems explicitly; resuming partial state is forbidden.
    return;
  }
  ForEachRuntimeTimer([](kernel::XTimer* timer) { timer->ThawAfterSnapshot(); });
  kernel_state_->ReleaseSnapshotHostWriters();
  graphics_system_->Resume();
  audio_system_->Resume();
  kernel_state_->guest_scheduler()->EndSnapshot();
  runtime_snapshot_frozen_ = false;
}

void Emulator::ForEachRuntimeTimer(const std::function<void(kernel::XTimer*)>& fn) {
  if (!kernel_state_) return;
  for (const auto& object : kernel_state_->object_table()->GetAllObjects()) {
    if (object->type() == kernel::XObject::Type::Timer) {
      fn(static_cast<kernel::XTimer*>(object.get()));
    }
  }
}

bool Emulator::CaptureRuntimeImage(snapshot::Image& out, std::string& error,
                                   bool include_memory) {
#if !(XE_PLATFORM_xendroid || XE_PLATFORM_ANDROID || XE_PLATFORM_MAC)
  error = "unsupported memory backend"; return false;
#else
  auto timeout = std::chrono::milliseconds(cvars::savestate_barrier_timeout_ms);
  using SnapClock = std::chrono::steady_clock;
  const auto t_begin = SnapClock::now();
  auto t_parked = t_begin, t_gpu = t_begin, t_kernel = t_begin, t_memory = t_begin;
  auto ms = [](SnapClock::time_point a, SnapClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  // The command processor can't stop while it waits (WAIT_REG_MEM) inside an
  // indirect buffer for a value only a parked guest thread would write. Let
  // the game run on briefly and try again before giving up.
  bool gpu_ok = false;
  snapshot::GpuState gpu;
  for (uint32_t attempt = 0;; ++attempt) {
    audio_system_->Pause();  // Drain its nested guest callbacks before guest park.
    if (!kernel_state_->guest_scheduler()->BeginSnapshot(timeout, true, &error)) {
      audio_system_->Resume(); return false;
    }
    if (!kernel_state_->FreezeSnapshotHostWriters(timeout)) {
      kernel_state_->guest_scheduler()->EndSnapshot(); audio_system_->Resume();
      error = "kernel host writer queue did not quiesce"; return false;
    }
    runtime_snapshot_frozen_ = true;
    t_parked = SnapClock::now();
    // Host timer expirations run outside the guest barrier and queue APCs into
    // guest memory: stop them for the whole capture.
    ForEachRuntimeTimer([](kernel::XTimer* timer) { timer->FreezeForSnapshot(); });
    gpu = {};
    gpu_ok = graphics_system_->PauseForRuntimeSnapshot([&] {
      return graphics_system_->command_processor()->CaptureRuntimeSnapshot(gpu);
    }, timeout);
    if (gpu_ok) {
      error.clear();  // A failed earlier attempt must not fail the result.
      break;
    }
    error = "GPU snapshot failed or command worker could not reach its barrier";
    bool healthy = graphics_system_->command_processor()->RuntimeSnapshotHealthy();
    ReleaseRuntimeSnapshot(healthy);
    if (!healthy || attempt >= 4) return false;
    XELOGW("RuntimeState: GPU barrier not reached (attempt {}), retrying", attempt + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
  }
  graphics_system_->CaptureSnapshotInterrupts(gpu);
  Part(out, snapshot::Section::kGpu) = gpu.Encode();
  t_gpu = SnapClock::now();
  snapshot::AudioState audio;
  if (!audio_system_->CaptureRuntimeSnapshot(audio, error)) return false;
  Part(out, snapshot::Section::kAudio) = audio.Encode();
  snapshot::KernelImage kernel;
  if (!kernel::KernelSnapshot::Capture(kernel_state_.get(),
        kernel_state_->guest_scheduler()->SnapshotThreads(), kernel, error)) return false;
  Part(out, snapshot::Section::kKernel) = kernel.Encode();
  // CPU records mirror kernel thread records so the CPU section is never a
  // dummy payload and every section is semantically verified before restore.
  auto& cpu = Part(out, snapshot::Section::kCpu);
  snapshot::Put32(cpu, 1); snapshot::Put32(cpu, uint32_t(kernel.threads.size()));
  for (const auto& thread : kernel.threads) {
    snapshot::Put32(cpu, thread.thread_id); snapshot::Put32(cpu, uint32_t(thread.cpu_state.size()));
    cpu.insert(cpu.end(), thread.cpu_state.begin(), thread.cpu_state.end());
  }
  t_kernel = SnapClock::now();
  if (include_memory &&
      !memory_->CaptureRuntimeSnapshot(Part(out, snapshot::Section::kMemory))) {
    error = "guest memory capture failed";
    if (!memory_->RuntimeSnapshotHealthy()) ReleaseRuntimeSnapshot(false);
    return false;
  }
  t_memory = SnapClock::now();
  uint64_t ticks, time;
  if (!Clock::CaptureRuntimeSnapshot(ticks, time)) { error = "guest clock capture failed"; return false; }
  auto& clock = Part(out, snapshot::Section::kClock);
  snapshot::Put32(clock, 1); snapshot::Put64(clock, ticks); snapshot::Put64(clock, time);
  if (!kernel_state_->SnapshotHostWritersRemainQuiescent()) {
    error = "a kernel host callback remained after writer park"; return false;
  }
  bool valid = ValidateRuntimeImage(out, error, include_memory);
  XELOGI("RuntimeState: capture park {:.0f} ms, GPU+audio {:.0f} ms, kernel {:.0f} ms, "
         "memory {:.0f} ms ({} MB), validate {:.0f} ms",
         ms(t_begin, t_parked), ms(t_parked, t_gpu), ms(t_gpu, t_kernel),
         ms(t_kernel, t_memory), Part(out, snapshot::Section::kMemory).size() >> 20,
         ms(t_memory, SnapClock::now()));
  return valid;
#endif
}

bool Emulator::ValidateRuntimeImage(const snapshot::Image& image, std::string& error,
                                    bool check_memory) const {
#if !(XE_PLATFORM_xendroid || XE_PLATFORM_ANDROID || XE_PLATFORM_MAC)
  error = "unsupported memory backend"; return false;
#else
  snapshot::KernelImage kernel;
  snapshot::GpuState gpu;
  snapshot::AudioState audio;
  uint64_t ticks, time;
  if (!snapshot::KernelImage::DecodeValidated(Part(image, snapshot::Section::kKernel), kernel, error)) return false;
  if (!snapshot::GpuState::Decode(Part(image, snapshot::Section::kGpu), gpu) ||
      !snapshot::AudioState::Decode(Part(image, snapshot::Section::kAudio), audio) ||
      !DecodeClock(image, ticks, time) ||
      (check_memory &&
       !memory_->ValidateRuntimeSnapshot(Part(image, snapshot::Section::kMemory)))) {
    error = "snapshot section semantic validation failed"; return false;
  }
  snapshot::Reader reader(Part(image, snapshot::Section::kCpu));
  uint32_t version, count;
  if (!reader.U32(version) || version != 1 || !reader.U32(count) || count != kernel.threads.size()) {
    error = "snapshot CPU inventory disagrees with kernel"; return false;
  }
  for (const auto& thread : kernel.threads) {
    uint32_t id, size;
    if (!reader.U32(id) || id != thread.thread_id || !reader.U32(size) || size != thread.cpu_state.size()) {
      error = "snapshot CPU thread record mismatch"; return false;
    }
    std::vector<uint8_t> state(size);
    if (!reader.Bytes(state) || state != thread.cpu_state) { error = "snapshot CPU state mismatch"; return false; }
  }
  if (reader.remaining()) { error = "trailing CPU state bytes"; return false; }
  return true;
#endif
}

bool Emulator::ColdRestoreRuntimeImage(snapshot::Image& image, std::string& error) {
#if !(XE_PLATFORM_xendroid || XE_PLATFORM_ANDROID || XE_PLATFORM_MAC)
  error = "unsupported memory backend"; return false;
#else
  snapshot::KernelImage kernel;
  snapshot::GpuState gpu;
  snapshot::AudioState audio;
  uint64_t ticks, time;
  if (!snapshot::KernelImage::DecodeValidated(Part(image, snapshot::Section::kKernel), kernel, error) ||
      !snapshot::GpuState::Decode(Part(image, snapshot::Section::kGpu), gpu) ||
      !snapshot::AudioState::Decode(Part(image, snapshot::Section::kAudio), audio) ||
      !DecodeClock(image, ticks, time)) { error = "restore image decoding failed"; return false; }
  auto launch_path = last_launch_path_;
  // Only cooperative fibers are supported. They are already held outside the
  // ready queues; no pthread cancellation or best-effort join is involved.
  relaunching_ = true;
  // Cleared on every exit: a failed restore must not leave later Shutdown()
  // calls on the relaunch path.
  struct ClearRelaunching {
    bool& flag;
    ~ClearRelaunching() { flag = false; }
  } clear_relaunching{relaunching_};
  if (kernel_state_) {
    kernel_state_->ShutdownDispatchThread();
    if (runtime_snapshot_frozen_) kernel_state_->ReleaseSnapshotHostWriters();
    kernel_state_->guest_scheduler()->Shutdown();
    auto threads = kernel_state_->object_table()->GetObjectsByType<kernel::XThread>(kernel::XObject::Type::Thread);
    // Owned CP/APU/vblank host workers are drained by Shutdown(), never
    // asynchronously killed through the guest object-table inventory.
    for (auto& thread : threads)
      if (thread->is_guest_thread() && thread->fiber()) thread->Terminate(0);
  }
  runtime_snapshot_frozen_ = false;
  Shutdown();
  auto status = Setup(nullptr, nullptr, require_cpu_backend_, nullptr, nullptr, nullptr);
  if (XFAILED(status)) { error = "cold restore core setup failed"; return false; }
  MountStandardDrives();
  if (XFAILED(SetupSubsystems())) { error = "cold restore subsystem setup failed"; return false; }
  audio_system_->Pause();
  std::vector<kernel::object_ref<kernel::XThread>> restored;
  bool hook_applied = false;
  kernel_state_->set_launch_hook([&](kernel::XThread* fresh_main) {
    if (!kernel_state_->FreezeSnapshotHostWriters(std::chrono::milliseconds(cvars::savestate_barrier_timeout_ms))) {
      error = "fresh kernel host writer barrier failed"; return false;
    }
    runtime_snapshot_frozen_ = true;
    // The fresh command processor first loads the title's shader storage,
    // which takes seconds on a cold boot: give it far longer than a park.
    // With a cold pipeline cache (rebuilt after a crash) Halo Reach's
    // precreation took over 90 s and the restore failed.
    const auto gpu_timeout = std::max(std::chrono::milliseconds(300000),
        std::chrono::milliseconds(cvars::savestate_barrier_timeout_ms));
    if (!graphics_system_->PauseForRuntimeSnapshot([&] {
          return graphics_system_->command_processor()->RestoreRuntimeSnapshot(gpu);
        }, gpu_timeout)) {
      error = "GPU restore failed"; return false;
    }
    graphics_system_->RestoreSnapshotInterrupts(gpu);
    const bool memory_applied =
        kernel::KernelSnapshot::LoadRuntimeModules(kernel_state_.get(), kernel, error) &&
        memory_->RestoreRuntimeSnapshot(Part(image, snapshot::Section::kMemory));
    // Free the decoded guest RAM (567 MB in Halo Reach) before the title
    // resumes: holding it until the restore returned pushed the phone into the
    // low memory killer.
    std::vector<uint8_t>().swap(Part(image, snapshot::Section::kMemory));
    if (!memory_applied ||
        !kernel::KernelSnapshot::Restore(kernel_state_.get(), fresh_main, kernel, restored, error) ||
        !audio_system_->RestoreRuntimeSnapshot(audio, error) ||
        !Clock::RestoreRuntimeSnapshot(ticks, time)) {
      if (error.empty()) error = "cold restore subsystem apply failed";
      return false;
    }
    bool timers_armed = true;
    ForEachRuntimeTimer([&](kernel::XTimer* timer) {
      timers_armed &= timer->CompleteRuntimeRestore();
    });
    if (!timers_armed) { error = "a restored timer could not be armed"; return false; }
    hook_applied = true;
    return true;
  });
  const bool launched = !XFAILED(LaunchPath(launch_path));
  // The hook captures this frame by reference. A launch that fails before
  // the module starts never consumes it, and a later boot would call it.
  if (kernel_state_) kernel_state_->set_launch_hook(nullptr);
  if (!launched || !hook_applied) {
    if (error.empty()) error = "cold title boot or snapshot launch hook failed";
    return false;
  }
  runtime_snapshot_failed_stopped_ = false;
  // All restored threads remain unqueued until RAM/kernel/GPU/APU/clock agree.
  if (!kernel::KernelSnapshot::ResumeThreads(restored, error)) {
    runtime_snapshot_failed_stopped_ = true;
    return false;
  }
  kernel_state_->ReleaseSnapshotHostWriters();
  graphics_system_->Resume(); audio_system_->Resume();
  runtime_snapshot_frozen_ = false;
  paused_.store(false);
  relaunching_ = false;
  return true;
#endif
}

bool Emulator::StreamRuntimeImage(snapshot::ImageFileWriter& writer,
                                  const snapshot::Image& image, std::string& error) {
  // Every core is idle while the guest is parked: compress guest RAM on
  // several of them, a few MB at a time, straight into the file.
  const unsigned workers =
      std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
  for (uint32_t i = 0; i < snapshot::kSectionCount; ++i) {
    if (i + 1 != uint32_t(snapshot::Section::kMemory)) {
      if (!writer.WriteSection(i, image.sections[i], error)) return false;
      continue;
    }
    if (!writer.BeginStreamSection(i, workers, error)) return false;
    const bool captured = memory_->CaptureRuntimeSnapshot(
        [&writer](const uint8_t* data, size_t size) { return writer.Append(data, size); });
    std::string end_error;
    const bool ended = writer.EndStreamSection(end_error);
    if (!captured) {
      error = memory_->RuntimeSnapshotHealthy() ? "guest memory capture failed"
                                                : "guest memory capture left protections unsafe";
      return false;
    }
    if (!ended) {
      error = end_error;
      return false;
    }
  }
  return true;
}

bool Emulator::SaveRuntimeState(const std::filesystem::path& path, std::string& error) {
  std::unique_lock lock(runtime_snapshot_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { error = "snapshot operation already running"; return false; }
  snapshot::Image image;
  if (!RuntimeSnapshotCapability(error) || !RuntimeSnapshotIdentity(image.identity, error)) return false;
  using SnapClock = std::chrono::steady_clock;
  auto ms = [](SnapClock::time_point a, SnapClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t_begin = SnapClock::now();
  bool ok = false;
  snapshot::ImageFileWriter writer;
  try {
    // Small sections first; guest RAM is streamed while the barrier is held,
    // so it is never copied whole into another buffer.
    ok = CaptureRuntimeImage(image, error, false) &&
         writer.Begin(path, image.identity, 1, error) &&
         StreamRuntimeImage(writer, image, error);
  } catch (const std::exception& ex) { error = std::string("snapshot capture exception: ") + ex.what(); }
  ReleaseRuntimeSnapshot(!runtime_snapshot_failed_stopped_ && memory_->RuntimeSnapshotHealthy() &&
                          graphics_system_->command_processor()->RuntimeSnapshotHealthy());
  const auto t_resumed = SnapClock::now();
  snapshot::FileStats stats;
  // The flush to storage happens with the title running again.
  ok = ok && writer.Commit(true, &stats, error);
  if (ok) {
    XELOGI("RuntimeState: saved {} MB as {} MB; title held {:.0f} ms, flush {:.0f} ms",
           stats.raw_bytes >> 20, stats.stored_bytes >> 20, ms(t_begin, t_resumed),
           ms(t_resumed, SnapClock::now()));
  }
  return ok;
}

bool Emulator::RestoreRuntimeState(const std::filesystem::path& path, std::string& error) {
  std::unique_lock lock(runtime_snapshot_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) { error = "snapshot operation already running"; return false; }
  snapshot::Identity identity;
  snapshot::Image desired, rollback;
  using SnapClock = std::chrono::steady_clock;
  auto ms = [](SnapClock::time_point a, SnapClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t_begin = SnapClock::now();
  // A missing slot is the common case: report it before anything else.
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    error = "no saved state for this game";
    return false;
  }
  snapshot::FileStats stats;
  // Complete malformed/mismatched-file preflight before parking any writer.
  if (!RuntimeSnapshotCapability(error) || !RuntimeSnapshotIdentity(identity, error) ||
      !snapshot::ReadImageFile(path, identity, desired, &stats, error) ||
      !ValidateRuntimeImage(desired, error)) return false;
  const auto t_read = SnapClock::now();
  // The running title is kept as a rollback in a file next to the slot (not
  // in RAM next to the image being restored), removed once it isn't needed.
  const auto rollback_path = path.parent_path() / ".rollback.xds";
  struct RemoveFile {
    std::filesystem::path path;
    ~RemoveFile() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } remove_rollback{rollback_path};
  try {
    // States that can't be captured (a guest thread in a wait that can't be
    // restarted, the command processor waiting inside an indirect buffer) are
    // usually brief: let the game run and try again for a while.
    const auto rollback_deadline = SnapClock::now() + std::chrono::seconds(15);
    bool saved = false;
    for (;;) {
      snapshot::ImageFileWriter writer;
      rollback.identity = identity;
      saved = CaptureRuntimeImage(rollback, error, false) &&
              writer.Begin(rollback_path, identity, 1, error) &&
              StreamRuntimeImage(writer, rollback, error) &&
              writer.Commit(false, nullptr, error);
      rollback = {};
      if (saved) break;
      ReleaseRuntimeSnapshot(!runtime_snapshot_failed_stopped_ && memory_->RuntimeSnapshotHealthy() &&
                              graphics_system_->command_processor()->RuntimeSnapshotHealthy());
      if (runtime_snapshot_failed_stopped_ || SnapClock::now() >= rollback_deadline) break;
      XELOGW("RuntimeState: rollback not capturable yet ({}), retrying", error);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!saved) {
      // Only a command processor that can't reach its barrier is safe to tear
      // down from a running game; a guest thread stuck in a wait that can't be
      // restarted blocks the teardown, so keep the running game instead.
      if (runtime_snapshot_failed_stopped_ ||
          error.find("GPU snapshot failed") == std::string::npos) {
        error = "could not keep the running game as a rollback: " + error;
        return false;
      }
      XELOGW("RuntimeState: restoring without a rollback ({})", error);
      std::error_code ec;
      std::filesystem::remove(rollback_path, ec);
    }
    const auto t_rollback = SnapClock::now();
    if (ColdRestoreRuntimeImage(desired, error)) {
      XELOGI("RuntimeState: restored {} MB from {} MB; read {:.0f} ms, rollback "
             "{:.0f} ms, cold restore {:.0f} ms",
             stats.raw_bytes >> 20, stats.stored_bytes >> 20, ms(t_begin, t_read),
             ms(t_read, t_rollback), ms(t_rollback, SnapClock::now()));
      error.clear();  // Warnings of a skipped rollback aren't a failure.
      return true;
    }
    auto original = error;
    desired = {};
    std::string rollback_error = "no rollback";
    if (saved &&
        snapshot::ReadImageFile(rollback_path, identity, rollback, nullptr, rollback_error) &&
        ColdRestoreRuntimeImage(rollback, rollback_error)) {
      error = "restore aborted; previous runtime restored: " + original; return false;
    }
    error = "restore and rollback failed; runtime stopped: " + original + "; " + rollback_error;
  } catch (const std::exception& ex) { error = std::string("restore exception; runtime stopped: ") + ex.what(); }
  runtime_snapshot_failed_stopped_ = true;
  return false;
}
bool Emulator::RuntimeMediaHash(const std::filesystem::path& launch_path,
                                std::array<uint8_t, 32>& out,
                                std::string& error) const {
  auto source = launch_path;
  auto ext = source.extension().string();
  if (ext == ".xex" || ext == ".XEX" || ext == ".elf") source = source.parent_path();
  // Held while hashing: a save that arrives during the background hash waits
  // for it instead of reading the media a second time.
  std::lock_guard lock(media_hash_mutex_);
  bool directory = false;
  std::vector<std::filesystem::path> files;
  std::string key;
  if (!MediaFiles(source, directory, files, key, error)) return false;
  if (!media_hash_key_.empty() && key == media_hash_key_) {
    out = media_hash_;
    return true;
  }
  const auto cache_path = cache_root_.empty()
                              ? std::filesystem::path()
                              : cache_root_ / "runtime-media-hashes.txt";
  if (LookupMediaHashCache(cache_path, key, media_hash_)) {
    media_hash_key_ = std::move(key);
    out = media_hash_;
    return true;
  }
  sha256::SHA256 media;
  std::error_code ec;
  if (directory) {
    for (const auto& path : files) {
      auto relative = std::filesystem::relative(path, source, ec).generic_string();
      auto size = std::filesystem::file_size(path, ec);
      uint64_t n = relative.size();
      if (ec) { error = "cannot stat launch media"; return false; }
      media.add(&n, sizeof(n)); media.add(relative.data(), relative.size());
      media.add(&size, sizeof(size));
      if (!HashFile(media, path, media_hash_cancel_)) { error = "cannot hash launch media"; return false; }
    }
  } else if (!HashFile(media, source, media_hash_cancel_)) {
    error = "cannot hash launch media"; return false;
  }
  media.getHash(media_hash_.data());
  StoreMediaHashCache(cache_path, key, media_hash_);
  media_hash_key_ = std::move(key);
  out = media_hash_;
  return true;
}

void Emulator::StartRuntimeMediaHash() {
  if (!runtime_snapshot_boot_enabled_ || last_launch_path_.empty()) return;
  StopRuntimeMediaHash();
  media_hash_cancel_.store(false, std::memory_order_relaxed);
  media_hash_thread_ = std::thread([this, path = last_launch_path_] {
    std::array<uint8_t, 32> hash;
    std::string error;
    auto begin = std::chrono::steady_clock::now();
    bool ok = RuntimeMediaHash(path, hash, error);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - begin).count();
    if (ok) {
      XELOGI("RuntimeState: launch media hashed in {} ms", ms);
    } else if (!media_hash_cancel_.load(std::memory_order_relaxed)) {
      XELOGW("RuntimeState: launch media hash failed: {}", error);
    }
  });
}

void Emulator::StopRuntimeMediaHash() {
  media_hash_cancel_.store(true, std::memory_order_relaxed);
  if (media_hash_thread_.joinable()) media_hash_thread_.join();
  media_hash_cancel_.store(false, std::memory_order_relaxed);
}

}  // namespace xe
