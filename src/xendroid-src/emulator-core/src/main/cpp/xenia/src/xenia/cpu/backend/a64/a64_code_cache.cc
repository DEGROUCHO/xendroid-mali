/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_cache.h"

#include <cstdint>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/function.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_PLATFORM_xendroid
#include "../aarch64_disasm.h"
#endif
#if defined(__linux__)
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <filesystem>
#include <string>
#endif

DECLARE_path(log_file);

DEFINE_bool(a64_perf_map, false,
            "Write a simpleperf/perf 'generic JIT symbols' map file "
            "(perf-<pid>.map) mapping generated code to guest function "
            "names, so profilers can attribute JIT samples. For profiling "
            "only: it writes a file per launch and a line per compiled "
            "function.",
            "CPU");
UPDATE_from_bool(a64_perf_map, 2026, 10, 17, 20, true);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64CodeCache::~A64CodeCache() {
  if (perf_map_file_) {
    std::fclose(perf_map_file_);
  }
}

#if defined(__linux__)
// The map used to be written by default into the app's private directory,
// one file per launch, which nothing ever deleted: remove those from earlier
// processes once.
static size_t RemoveStalePerfMaps() {
  char process_name[256] = {0};
  if (FILE* cmdline = std::fopen("/proc/self/cmdline", "rb")) {
    std::fread(process_name, 1, sizeof(process_name) - 1, cmdline);
    std::fclose(cmdline);
  }
  if (char* colon = std::strchr(process_name, ':')) {
    *colon = '\0';
  }
  const std::string own = "perf-" + std::to_string(getpid()) + ".map";
  size_t removed = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(
           std::filesystem::path("/data/data") / process_name, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 9 && name.compare(0, 5, "perf-") == 0 &&
        name.compare(name.size() - 4, 4, ".map") == 0 && name != own &&
        std::filesystem::remove(entry.path(), ec)) {
      ++removed;
    }
  }
  if (removed) {
    XELOGI("Removed {} stale JIT perf maps", removed);
  }
  return removed;
}
#endif

void A64CodeCache::OnCodePlaced(uint32_t guest_address,
                                GuestFunction* function_info,
                                void* code_execute_address, size_t code_size) {
#if defined(__linux__)
  static const size_t stale_perf_maps_removed = RemoveStalePerfMaps();
  (void)stale_perf_maps_removed;
  if (!cvars::a64_perf_map) {
    return;
  }
  std::lock_guard<std::mutex> lock(perf_map_mutex_);
  if (!perf_map_file_) {
    if (perf_map_open_failed_) {
      return;
    }
    // Next to the log first (the app's external storage on Android): adb can
    // pull it from a profileable (non-debuggable) build. Then where simpleperf
    // looks on the device (/data/data/<package>/perf-<pid>.map); the package
    // name is the process name up to the ':' (this runs in the :emu process).
    // Then the standalone-program locations.
    char process_name[256] = {0};
    if (FILE* cmdline = std::fopen("/proc/self/cmdline", "rb")) {
      std::fread(process_name, 1, sizeof(process_name) - 1, cmdline);
      std::fclose(cmdline);
    }
    if (char* colon = std::strchr(process_name, ':')) {
      *colon = '\0';
    }
    const int pid = getpid();
    char path[512];
    path[0] = '\0';
    if (!cvars::log_file.empty()) {
      std::snprintf(path, sizeof(path), "%s/perf-%d.map",
                    cvars::log_file.parent_path().c_str(), pid);
      perf_map_file_ = std::fopen(path, "we");
      if (!perf_map_file_) {
        XELOGW("JIT perf map: can't create {}: errno {}", path, errno);
      }
    }
    if (!perf_map_file_) {
      std::snprintf(path, sizeof(path), "/data/data/%s/perf-%d.map",
                    process_name, pid);
      perf_map_file_ = std::fopen(path, "we");
    }
    if (!perf_map_file_) {
      std::snprintf(path, sizeof(path), "/data/local/tmp/perf-%d.map", pid);
      perf_map_file_ = std::fopen(path, "we");
    }
    if (!perf_map_file_) {
      std::snprintf(path, sizeof(path), "/tmp/perf-%d.map", pid);
      perf_map_file_ = std::fopen(path, "we");
    }
    if (!perf_map_file_) {
      perf_map_open_failed_ = true;
      XELOGW("JIT perf map: no writable location");
      return;
    }
    XELOGI("JIT perf map: {}", path);
  }
  const uint64_t address = reinterpret_cast<uint64_t>(code_execute_address);
  if (function_info && !function_info->name().empty()) {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx %s\n", address,
                 code_size, function_info->name().c_str());
  } else if (guest_address) {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx sub_%08X\n", address,
                 code_size, guest_address);
  } else {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx a64_host_code\n",
                 address, code_size);
  }
  std::fflush(perf_map_file_);
#endif
}

void A64CodeCache::FillCode(void* write_address, size_t size) {
  // Fill with BRK #0 (0xD4200000), 4-byte aligned.
  constexpr uint32_t kBrk0 = 0xD4200000;
  auto* p = reinterpret_cast<uint32_t*>(write_address);
  auto* end =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(write_address) + size);
  for (; p < end; ++p) {
    *p = kBrk0;
  }
}

void A64CodeCache::FlushCodeRange(void* address, size_t size) {
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), address, size);
#else


#if XE_PLATFORM_xendroid
    //XELOGI("ASM:\n{}", aarch64_disasm(reinterpret_cast<uint64_t>(address),reinterpret_cast<uint32_t*>(address),size/4));
#endif
    __builtin___clear_cache(
      reinterpret_cast<char*>(address),
      reinterpret_cast<char*>(static_cast<uint8_t*>(address) + size));
#endif
}

void A64CodeCache::ResolveGuestCallSites(
    uint32_t guest_address, uint8_t* code_execute_address,
    const std::vector<std::pair<uint32_t, uint32_t>>& call_sites) {
  std::lock_guard<std::mutex> lock(guest_call_sites_mutex_);
  if (guest_address) {
    auto it = guest_call_sites_.find(guest_address);
    if (it != guest_call_sites_.end()) {
      for (uint32_t site_offset : it->second) {
        PatchCallSite(generated_code_execute_base_ + site_offset,
                      code_execute_address);
      }
    }
  }
  for (const auto& site : call_sites) {
    uint8_t* site_execute = code_execute_address + site.second;
    guest_call_sites_[site.first].push_back(
        uint32_t(site_execute - generated_code_execute_base_));
    // Holding the lock, a callee placed concurrently either is visible here
    // or patches this site when it resolves its own placement.
    const uint8_t* target = PlacedCodeForGuest(site.first);
    if (target) {
      PatchCallSite(site_execute, target);
    }
  }
}

const uint8_t* A64CodeCache::PlacedCodeForGuest(uint32_t guest_address) const {
  if (!indirection_table_base_ || guest_address < kIndirectionTableBase) {
    return nullptr;
  }
  const uint64_t slot_offset = ((guest_address - kIndirectionTableBase) / 4) * 4;
  if (slot_offset + 4 > kIndirectionTableSize) {
    return nullptr;
  }
  const uint32_t slot = __atomic_load_n(
      reinterpret_cast<const uint32_t*>(indirection_table_base_ + slot_offset),
      __ATOMIC_ACQUIRE);
  if (slot == indirection_default_value_) {
    return nullptr;
  }
  if (!encoded_indirection_) {
    return reinterpret_cast<const uint8_t*>(uintptr_t(slot));
  }
  if (slot & kIndirectionExternalTag) {
    // Not placed code (a host target outside the cache).
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(execute_base_address() + slot);
}

void A64CodeCache::PatchCallSite(uint8_t* site_execute,
                                 const uint8_t* target_execute) {
  const int64_t delta = int64_t(uintptr_t(target_execute)) -
                        int64_t(uintptr_t(site_execute));
  if ((delta & 3) || delta < -(int64_t(1) << 27) ||
      delta >= (int64_t(1) << 27)) {
    return;
  }
  const uint32_t bl = 0x94000000u | (uint32_t(delta >> 2) & 0x03FFFFFFu);
  uint8_t* site_write =
      generated_code_write_base_ + (site_execute - generated_code_execute_base_);
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  const bool jit_write_toggle =
      generated_code_execute_base_ == generated_code_write_base_;
  if (jit_write_toggle) {
    pthread_jit_write_protect_np(0);
  }
#endif
  __atomic_store_n(reinterpret_cast<uint32_t*>(site_write), bl,
                   __ATOMIC_RELEASE);
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  if (jit_write_toggle) {
    pthread_jit_write_protect_np(1);
  }
#endif
  FlushCodeRange(site_write, 4);
  if (site_write != site_execute) {
    FlushCodeRange(site_execute, 4);
  }
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
