/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/backend.h"

#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace backend {

Backend::Backend() { std::memset(&machine_info_, 0, sizeof(machine_info_)); }
Backend::~Backend() = default;

bool Backend::Initialize(Processor* processor) {
  processor_ = processor;
  return true;
}

void* Backend::AllocThreadData() { return nullptr; }

void Backend::FreeThreadData(void* thread_data) {}

void (*preempt_yield_handler)(void* raw_context) = nullptr;

void (*spin_backoff_yield_handler)(void* raw_context) = nullptr;

void (*timebase_spin_handler)(void* raw_context) = nullptr;
std::atomic<uint64_t> frame_slack_until_ms{0};
std::atomic<uint32_t> spin_park_scale_q8{256};
std::atomic<uint32_t> frame_pace_comfortable_count{0};
std::atomic<uint32_t> frame_pace_tight_count{0};
std::atomic<int32_t> frame_pace_presenter_cpu{-1};
std::atomic<bool> thermal_hot{false};
std::atomic<uint64_t> guest_io_burst_until_ms{0};
std::atomic<uint32_t> frame_draw_count{UINT32_MAX};
std::atomic<bool> frame_pace_loading{false};
std::atomic<bool> frame_limiter_probe{false};

uint32_t Backend::ReservedLoad32(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint32_t*>(address));
}

uint64_t Backend::ReservedLoad64(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint64_t*>(address));
}

bool Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                              uint32_t value) {
  *context->TranslateVirtual<uint32_t*>(address) = xe::byte_swap(value);
  return true;
}

bool Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                              uint64_t value) {
  *context->TranslateVirtual<uint64_t*>(address) = xe::byte_swap(value);
  return true;
}

}  // namespace backend
}  // namespace cpu
}  // namespace xe
