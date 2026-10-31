#ifndef XENIA_SNAPSHOT_MUTANT_OWNERSHIP_H_
#define XENIA_SNAPSHOT_MUTANT_OWNERSHIP_H_
#include <atomic>
#include <memory>
#include "xenia/base/threading.h"
namespace xe::snapshot {
// Reconstruct host bookkeeping from already-restored guest RAM. This never
// acquires the guest mutant, inserts guest list links or changes signal_state.
// The real kernel restore and the focused host regression use this same code.
template <typename Owner>
bool RestoreMutantHostOwnership(Owner* owner, uint32_t recursion,
                               std::atomic<Owner*>& host_owner,
                               uint32_t& host_recursion,
                               std::unique_ptr<threading::Semaphore>& free_signal) {
  if ((owner != nullptr) != (recursion != 0) || recursion > 65536) return false;
  auto semaphore = threading::Semaphore::Create(owner ? 0 : 1, 1);
  if (!semaphore) return false;
  free_signal = std::move(semaphore);
  host_recursion = recursion;
  host_owner.store(owner, std::memory_order_release);
  return true;
}
}
#endif
