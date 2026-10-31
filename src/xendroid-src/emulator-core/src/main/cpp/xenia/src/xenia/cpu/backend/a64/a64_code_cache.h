/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_

#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache : public CodeCacheBase<A64CodeCache> {
 public:
  ~A64CodeCache() override;

  static std::unique_ptr<A64CodeCache> Create();

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

  // CRTP hooks for CodeCacheBase.
  void FillCode(void* write_address, size_t size);
  void FlushCodeRange(void* address, size_t size);

  // Writes one simpleperf "generic JIT symbols" map entry per placed
  // function (perf-<pid>.map) so profilers can attribute guest code by name
  // instead of showing an anonymous "unknown" DSO. Gated by the a64_perf_map
  // cvar; Linux/Android only.
  void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
                    void* code_execute_address, size_t code_size);

  // a64_patchable_guest_calls. call_sites are (callee guest address, offset of
  // its BL in this function's code). Rewrites every known BL to the function
  // just placed at guest_address, then binds this function's own sites to
  // callees that already have code and records them all, so a later placement
  // of a callee rewrites them. Only BL (+-128 MiB) is ever written: concurrent
  // modification of a B/BL is architecturally allowed, and an unpatched BL
  // still reaches the stub, which does the regular lookup.
  void ResolveGuestCallSites(
      uint32_t guest_address, uint8_t* code_execute_address,
      const std::vector<std::pair<uint32_t, uint32_t>>& call_sites);

  // Virtual for platform-specific overrides (_win.cc / _posix.cc).
  virtual UnwindReservation RequestUnwindReservation(uint8_t* entry_address) {
    return UnwindReservation();
  }
  virtual void PlaceCode(uint32_t guest_address, void* machine_code,
                         const EmitFunctionInfo& func_info,
                         void* code_execute_address,
                         UnwindReservation unwind_reservation) {}

 protected:
  A64CodeCache() = default;

  const uint8_t* PlacedCodeForGuest(uint32_t guest_address) const;
  void PatchCallSite(uint8_t* site_execute, const uint8_t* target_execute);
  std::mutex guest_call_sites_mutex_;
  // Callee guest address -> BL offsets from the execute base.
  std::unordered_map<uint32_t, std::vector<uint32_t>> guest_call_sites_;

  std::mutex perf_map_mutex_;
  FILE* perf_map_file_ = nullptr;
  bool perf_map_open_failed_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
