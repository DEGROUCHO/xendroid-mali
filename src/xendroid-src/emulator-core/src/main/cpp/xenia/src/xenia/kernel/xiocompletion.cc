/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xiocompletion.h"

#include "xenia/base/clock.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/base/byte_stream.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XIOCompletion::XIOCompletion(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {
  notification_semaphore_ = threading::Semaphore::Create(0, kMaxNotifications);
  assert_not_null(notification_semaphore_);
}

XIOCompletion::~XIOCompletion() = default;

void XIOCompletion::QueueNotification(IONotification& notification) {
  {
    std::unique_lock<std::mutex> lock(notification_lock_);
    notifications_.push(notification);
    notification_semaphore_->Release(1, nullptr);
  }
  kernel_state()->guest_scheduler()->WakeAll();
}

bool XIOCompletion::WaitForNotification(uint64_t wait_ticks,
                                        IONotification* notify) {
  auto ms = std::chrono::milliseconds(TimeoutTicksToMs(wait_ticks));

  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    // Acquire at zero timeout and yield between polls, rather than blocking
    // the dispatch host thread.
    auto* scheduler = kernel_state()->guest_scheduler();
    uint64_t deadline_ms = Clock::QueryHostUptimeMillis() + ms.count();
    while (true) {
      auto poll = threading::Wait(notification_semaphore_.get(), false,
                                  std::chrono::milliseconds(0));
      if (poll == threading::WaitResult::kSuccess) {
        break;
      }
      if (Clock::QueryHostUptimeMillis() >= deadline_ms) {
        return false;
      }
      scheduler->BlockCurrentThread();
    }
  } else {
    auto res = threading::Wait(notification_semaphore_.get(), false, ms);
    if (res != threading::WaitResult::kSuccess) {
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(notification_lock_);
  assert_false(notifications_.empty());

  std::memcpy(notify, &notifications_.front(), sizeof(IONotification));
  notifications_.pop();

  return true;
}

bool XIOCompletion::Save(ByteStream* stream) {
  SaveObject(stream);
  std::unique_lock<std::mutex> lock(notification_lock_);
  std::queue<IONotification> pending = notifications_;
  stream->Write<uint32_t>(uint32_t(pending.size()));
  while (!pending.empty()) {
    const IONotification& notification = pending.front();
    stream->Write<uint32_t>(notification.key_context);
    stream->Write<uint32_t>(notification.apc_context);
    stream->Write<uint32_t>(notification.status);
    stream->Write<uint32_t>(notification.num_bytes);
    pending.pop();
  }
  return true;
}

object_ref<XIOCompletion> XIOCompletion::Restore(KernelState* kernel_state,
                                                 ByteStream* stream) {
  auto port = new XIOCompletion(kernel_state);
  port->RestoreObject(stream);
  uint32_t count = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < count && !stream->failed(); ++i) {
    IONotification notification;
    notification.key_context = stream->Read<uint32_t>();
    notification.apc_context = stream->Read<uint32_t>();
    notification.status = stream->Read<uint32_t>();
    notification.num_bytes = stream->Read<uint32_t>();
    port->notifications_.push(notification);
    port->notification_semaphore_->Release(1, nullptr);
  }
  return object_ref<XIOCompletion>(port);
}

}  // namespace kernel
}  // namespace xe
