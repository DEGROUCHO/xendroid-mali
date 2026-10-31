/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <limits>

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  timer_type_ = timer_type;
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

bool XTimer::Arm(uint64_t due_file_time, uint32_t guest_period_ms,
                 uint32_t routine, uint32_t routine_arg, XThread* thread) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  // Guests pass an absolute due time of 0 (the 1601 NT epoch) to mean "fire
  // immediately". Casting that to the host's steady_clock overflows int64 and
  // yields a far-future deadline the timer never reaches, so clamp a past-due
  // time here, in the guest domain where the comparison cannot overflow. The
  // clamped time is also what a snapshot counts periods from.
  uint64_t now_file_time = XSystemClock::to_file_time(XSystemClock::now());
  if (due_file_time < now_file_time) {
    due_file_time = now_file_time;
  }
  auto due_tp = date::clock_cast<WinSystemClock>(
      XSystemClock::from_file_time(due_file_time));

  armed_ = true;
  due_file_time_ = due_file_time;
  guest_period_ms_ = guest_period_ms;
  callback_thread_ = thread;
  callback_thread_id_ = thread ? thread->thread_id() : 0;
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(fire_count_->mutex);
    generation = ++fire_count_->generation;
    fire_count_->fires = 0;
  }

  // Issued when the timer fires, after the host signaled it. Captures values
  // by value to avoid racing with a future SetTimer() call.
  // The APC thread is looked up by id when the timer fires: the timer can
  // outlive it.
  auto fire_count = fire_count_;
  auto cb_kernel_state = kernel_state();
  auto cb_thread_id = callback_thread_id_;
  auto cb_routine = callback_routine_;
  auto cb_routine_arg = callback_routine_arg_;
  std::function<void()> callback = [fire_count, generation, cb_kernel_state,
                                    cb_thread_id, cb_routine,
                                    cb_routine_arg]() {
    {
      std::lock_guard<std::mutex> lock(fire_count->mutex);
      if (fire_count->generation == generation) {
        ++fire_count->fires;
      }
    }
    if (!cb_routine || !cb_thread_id || !cb_kernel_state) {
      return;
    }
    auto cb_thread = cb_kernel_state->GetThreadByID(cb_thread_id);
    if (!cb_thread) {
      return;
    }
    // Queue APC to call back routine with (arg, low, high).
    // It'll be executed on the thread that requested the timer.
    uint64_t time = xe::Clock::QueryGuestSystemTime();
    uint32_t time_low = static_cast<uint32_t>(time);
    uint32_t time_high = static_cast<uint32_t>(time >> 32);
    XELOGD("XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
           cb_routine, cb_routine_arg, time_low, time_high);
    cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
  };

  uint32_t period_ms = Clock::ScaleGuestDurationMillis(guest_period_ms);
  if (!period_ms) {
    return timer_->SetOnceAt(due_tp, std::move(callback));
  }
  return timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms),
                                std::move(callback));
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  // Any timer implementation uses absolute times eventually, convert as early
  // as possible for increased accuracy.
  uint64_t due_file_time;
  if (due_time < 0) {
    uint64_t now = XSystemClock::to_file_time(XSystemClock::now());
    uint64_t after = uint64_t(-(due_time + 1)) + 1;
    due_file_time = after > std::numeric_limits<uint64_t>::max() - now
                        ? std::numeric_limits<uint64_t>::max()
                        : now + after;
  } else {
    due_file_time = uint64_t(due_time);
  }

  bool result = Arm(due_file_time, period_ms, routine, routine_arg,
                    routine ? XThread::GetCurrentThread() : nullptr);

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  armed_ = false;
  {
    std::lock_guard<std::mutex> fire_lock(fire_count_->mutex);
    ++fire_count_->generation;
    fire_count_->fires = 0;
  }
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

bool XTimer::Save(ByteStream* stream) {
  std::lock_guard<std::mutex> lock(timer_lock_);
  // Only a frozen timer has a state no expiration can change while saving.
  if (!timer_ || timer_type_ > 1 || !frozen_) {
    return false;
  }
  bool armed = frozen_armed_;
  bool signaled = frozen_signaled_;
  uint64_t due = frozen_due_;
  uint32_t routine = 0, routine_arg = 0, thread_handle = 0;
  if (armed && callback_routine_) {
    // An APC whose thread has exited is never delivered (the callback drops
    // it): save the timer without it rather than refusing the snapshot.
    auto thread = kernel_state()->GetThreadByID(callback_thread_id_);
    if (thread && thread->handle()) {
      routine = callback_routine_;
      routine_arg = callback_routine_arg_;
      thread_handle = thread->handle();
    }
  }

  SaveObject(stream);
  stream->Write<uint32_t>(timer_type_);
  stream->Write<bool>(armed);
  stream->Write<bool>(signaled);
  stream->Write<uint64_t>(armed ? due : 0);
  stream->Write<uint32_t>(armed ? guest_period_ms_ : 0);
  stream->Write<uint32_t>(routine);
  stream->Write<uint32_t>(routine_arg);
  stream->Write<uint32_t>(thread_handle);
  XELOGD("XTimer {:08X} saved: type {} armed {} signaled {} period {} ms "
         "routine {:08X}",
         handle(), timer_type_, armed, signaled, guest_period_ms_, routine);
  return true;
}

object_ref<XTimer> XTimer::RestoreRuntime(KernelState* kernel_state,
                                          ByteStream* stream) {
  auto timer = new XTimer(nullptr);
  timer->kernel_state_ = kernel_state;
  auto fail = [timer]() -> object_ref<XTimer> {
    timer->Release();
    return nullptr;
  };
  if (!timer->RestoreObject(stream)) {
    return fail();
  }
  uint32_t type = stream->Read<uint32_t>();
  bool armed = stream->Read<bool>();
  bool signaled = stream->Read<bool>();
  uint64_t due = stream->Read<uint64_t>();
  uint32_t period = stream->Read<uint32_t>();
  uint32_t routine = stream->Read<uint32_t>();
  uint32_t routine_arg = stream->Read<uint32_t>();
  uint32_t thread_handle = stream->Read<uint32_t>();
  if (stream->failed() || type > 1 || (!routine) != (!thread_handle) ||
      (routine && !armed)) {
    return fail();
  }
  object_ref<XThread> thread;
  if (thread_handle) {
    thread = kernel_state->object_table()->LookupObject<XThread>(thread_handle);
    if (!thread) {
      return fail();
    }
  }
  timer->Initialize(type);
  // Not reachable by the guest yet, so no lock. Armed and signaled by
  // CompleteRuntimeRestore once the guest clock is back.
  timer->restore_pending_ = true;
  timer->restore_signaled_ = signaled;
  timer->armed_ = armed;
  timer->due_file_time_ = due;
  timer->guest_period_ms_ = period;
  timer->callback_thread_ = thread.get();
  timer->callback_thread_id_ = thread ? thread->thread_id() : 0;
  timer->callback_routine_ = routine;
  timer->callback_routine_arg_ = routine_arg;
  return object_ref<XTimer>(timer);
}

bool XTimer::CompleteRuntimeRestore() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!restore_pending_) {
    return true;
  }
  restore_pending_ = false;
  // Arming clears the signaled state: signal after it.
  if (armed_ && !Arm(due_file_time_, guest_period_ms_, callback_routine_,
                     callback_routine_arg_, callback_thread_)) {
    return false;
  }
  return !restore_signaled_ || timer_->SignalForSnapshot();
}

void XTimer::FreezeForSnapshot() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!timer_ || frozen_) {
    return;
  }
  // Disarm returns only once no expiration callback is running, so the
  // signaled state and the fire count are consistent after it.
  timer_->Cancel();
  bool signaled = false;
  if (!timer_->PeekSignaledForSnapshot(signaled)) {
    signaled = false;
  }
  uint64_t fires;
  {
    std::lock_guard<std::mutex> fire_lock(fire_count_->mutex);
    fires = fire_count_->fires;
  }
  // The next expiration: a one-shot timer that fired is disarmed, a periodic
  // one moves on by the periods that elapsed.
  bool armed = armed_;
  uint64_t due = due_file_time_;
  if (armed) {
    if (!guest_period_ms_) {
      armed = fires == 0;
    } else {
      uint64_t step = uint64_t(guest_period_ms_) * 10000;
      due = fires > (std::numeric_limits<uint64_t>::max() - due) / step
                ? std::numeric_limits<uint64_t>::max()
                : due + fires * step;
    }
  }
  frozen_ = true;
  frozen_armed_ = armed;
  frozen_signaled_ = signaled;
  frozen_due_ = armed ? due : 0;
}

void XTimer::ThawAfterSnapshot() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!frozen_) {
    return;
  }
  frozen_ = false;
  if (!frozen_armed_) {
    armed_ = false;
    return;
  }
  // Arming clears the signaled state: restore it after. The APC thread is
  // resolved by id, the stored pointer may be stale.
  auto thread = callback_thread_id_
                    ? kernel_state()->GetThreadByID(callback_thread_id_)
                    : object_ref<XThread>();
  Arm(frozen_due_, guest_period_ms_, thread ? callback_routine_ : 0,
      callback_routine_arg_, thread.get());
  if (frozen_signaled_) {
    timer_->SignalForSnapshot();
  }
}

}  // namespace kernel
}  // namespace xe
