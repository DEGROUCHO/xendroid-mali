/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <memory>
#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume);
  X_STATUS Cancel();

  // Runtime snapshot: type, armed due time / period, signaled state and the
  // APC routine with its thread (restored before plain objects).
  bool Save(ByteStream* stream) override;
  static object_ref<XTimer> RestoreRuntime(KernelState* kernel_state,
                                           ByteStream* stream);
  // Arms a restored timer. Called once the guest clock is restored, which
  // happens after the kernel objects: the due time is a guest clock time.
  bool CompleteRuntimeRestore();
  // Stops the host timer for a capture (no expiration can change the timer
  // or guest memory until thawed) and fixes the state Save() writes.
  void FreezeForSnapshot();
  // Re-arms a frozen timer at its pending due time, keeping its state.
  void ThawAfterSnapshot();

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return timer_.get(); }

 private:
  // Counts host expirations of the current arming; shared with the host
  // callback so an expiration racing a destroy or re-arm stays harmless.
  struct FireCount {
    std::mutex mutex;
    uint64_t generation = 0;
    uint64_t fires = 0;
  };

  // Arms at an absolute guest system time (NT file time).
  bool Arm(uint64_t due_file_time, uint32_t guest_period_ms, uint32_t routine,
           uint32_t routine_arg, XThread* thread);

  std::unique_ptr<xe::threading::Timer> timer_;
  std::mutex timer_lock_;
  std::shared_ptr<FireCount> fire_count_ = std::make_shared<FireCount>();

  uint32_t timer_type_ = 0;
  bool restore_pending_ = false;
  bool frozen_ = false;
  bool frozen_armed_ = false;
  bool frozen_signaled_ = false;
  uint64_t frozen_due_ = 0;
  bool restore_signaled_ = false;
  bool armed_ = false;
  uint64_t due_file_time_ = 0;
  uint32_t guest_period_ms_ = 0;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_thread_id_ = 0;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
