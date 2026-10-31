/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"
#include "xenia/base/xxhash.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/poll_spin_detector.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

DEFINE_bool(
    guest_scheduler_stats, false,
    "Log guest scheduler counters once a second: blocked-waiter re-poll rate, "
    "fiber switches, forced preemptions, and how long offloaded blocking calls "
    "queue behind the single I/O worker.",
    "Kernel");

DEFINE_bool(
    guest_spin_wfe, false,
    "Guest threads that yield with nothing else ready park their core (ARM64 "
    "WFE) until a guest atomic store or a thread wake-up happens, at most "
    "~200 us, instead of spinning. Wakes on the store itself, so it "
    "adds no fixed sleep. JIT change: needs restart.",
    "Kernel");
DEFINE_int32(
    guest_timebase_spin_park_us, 100,
    "Under the cooperative scheduler, a guest thread reading the time base in "
    "a tight burst (a job system polling for work with a timed loop and no "
    "pause hint or kernel call) yields to another ready fiber, or sleeps its "
    "dispatch thread this long, but only while the frame limiter reports "
    "slack (the presenting thread had to wait for its slot: needs "
    "guest_swap_pacing). Turns idle spinning at full clock into sleep without "
    "costing frames; with no slack nothing changes. Never at DISPATCH_LEVEL "
    "or under the global lock. 0 disables.",
    "Kernel");
DEFINE_int32(
    guest_poll_spin_park_us, 100,
    "Under the cooperative scheduler, a guest thread polling in a tight loop "
    "with nothing to do - NtYieldExecution with no other thread to run, or a "
    "zero-timeout wait that times out, repeated back to back with its "
    "callee-saved registers unchanged for guest_poll_spin_park_after_us (a "
    "job system waiting for work or for a batch to finish) - sleeps its "
    "dispatch thread this long per poll, but only while the frame limiter "
    "reports slack (needs guest_swap_pacing). Waits shorter than the "
    "threshold and loops that advance any state are never touched. 0 "
    "disables.",
    "Kernel");
DEFINE_int32(
    guest_spin_sample_park_us, 2000,
    "Under the cooperative scheduler, the longest sleep (microseconds) taken "
    "at a quantum end by a guest thread found spinning in place by state "
    "sampling: at each quantum end its full register and stack fingerprint "
    "is compared with the ones seen at its recent quantum ends, and three "
    "repeats in the last 16 mean it is going around the same states without "
    "advancing any (any kind of busy wait, with or without kernel calls: a "
    "spin on a memory word with a pause hint, a poll loop). Once it has been "
    "found in place at two quantum ends in a row it sleeps at each further "
    "one, for at most 1/guest_poll_spin_wait_share of how long that has "
    "lasted, only while the frame limiter reports slack, and only until "
    "something else becomes runnable on its CPU. 0 = detect and count only "
    "(log_guest_sync_stats), never sleep.",
    "Kernel");
DEFINE_bool(guest_poll_spin_stack_fingerprint, true,
            "Include 1 KiB of the caller frames in what must be unchanged "
            "between two polls for guest_poll_spin_park_us. Off: only the "
            "callee-saved registers, the stack pointer and the call site "
            "(for wait loops that keep a changing value, like a time, in a "
            "local).",
            "Kernel");
DEFINE_int32(guest_poll_spin_gap_us, 60,
             "Longest time between two polls of a guest thread for them to "
             "count as the same poll loop (guest_poll_spin_park_us). The "
             "polls must also find the thread's registers and caller frames "
             "unchanged, which is what tells a wait from work.",
             "Kernel");
DEFINE_int32(guest_poll_spin_tight_gap_us, 12,
             "Polls of a guest thread at most this far apart count as one "
             "wait for guest_poll_spin_park_us even if the thread's registers "
             "changed in between (a wait loop that counts its turns or reads "
             "the time: no work fits in so short a gap). The sleeps stay "
             "limited by guest_poll_spin_wait_share. 0 = only loops whose "
             "state is unchanged.",
             "Kernel");
DEFINE_int32(guest_poll_spin_wait_share, 8,
             "A guest_poll_spin_park_us sleep lasts at most 1/N of how long "
             "the wait has already gone on (the ~50 us the host adds to a "
             "short sleep included), so whatever the wait is for is noticed "
             "at most that share of its length late - also when nothing ties "
             "it to the frame, as while loading. 0 = no such limit.",
             "Kernel");
DEFINE_int32(guest_poll_spin_park_after_us, 300,
             "How long an unchanged guest poll loop must have been spinning "
             "before guest_poll_spin_park_us sleeps start.",
             "Kernel");
DEFINE_bool(log_guest_sync_stats, false,
            "Log every 5 seconds the per-second rates of guest yields, "
            "zero-timeout waits, event pulses and of the spin parks taken "
            "(GuestSyncStats line).",
            "Kernel");
DEFINE_int32(
    guest_threads_placement, 1,
    "Android/Linux: keep the guest CPU dispatch threads that do little work, "
    "or mostly spin in place, on the host's lower-clocked cores while frames "
    "reach their slot with spare time (needs guest_swap_pacing), so the fast "
    "cores and their clock are left to the threads that set the frame time. "
    "Re-evaluated every second from each thread's CPU time; everything is "
    "released as soon as frames run short of spare time, with a growing "
    "hold-off if that follows a restriction. The thread that presents frames "
    "is never restricted. 0 = off, 1 = only when the slow cores have at "
    "least guest_threads_placement_min_capacity_percent of the capacity of "
    "the fastest (cpu_capacity), 2 = also when their capacity is unknown.",
    "Kernel");
DEFINE_uint32(guest_threads_critical_mask, 0,
              "Experimental: guest CPU dispatch bitmask (bits 0-5) to give "
              "nice -10, without lowering an inherited higher priority. "
              "Applied at dispatch startup; 0 disables.",
              "Kernel");
DEFINE_int32(guest_threads_placement_min_capacity_percent, 45,
             "See guest_threads_placement.", "Kernel");
DEFINE_int32(guest_threads_placement_enter_percent, 38,
             "guest_threads_placement: a dispatch thread whose work (CPU time "
             "not spent spinning in place) is below this share of a core "
             "goes to the slow cores.",
             "Kernel");
DEFINE_int32(guest_threads_placement_leave_percent, 65,
             "guest_threads_placement: a dispatch thread on the slow cores "
             "whose work rises above this share of a core is released.",
             "Kernel");
DEFINE_int32(guest_threads_placement_budget_percent, 60,
             "guest_threads_placement: share of the slow cores that the "
             "threads kept on them may use in total.",
             "Kernel");
DEFINE_int32(
    guest_threads_spin_uclamp_max, 0,
    "guest_threads_placement: for a dispatch thread kept on the slow cores "
    "that spins in place for most of its CPU time, also cap the clock it can "
    "request (scheduler utilization clamp, 1-1024; 1024 is no cap) if the "
    "kernel lets an app do it. Probed once, safely. 0 = don't.",
    "Kernel");
DEFINE_bool(log_guest_threads_uclamp_probe, false,
            "Log at start whether this kernel lets the app set the "
            "utilization clamp of its own threads (sched_setattr).",
            "Kernel");
DEFINE_int32(
    guest_scheduler_idle_spin_us, 80,
    "While frames are not reaching their slot with spare time (the game is "
    "below its frame rate target, or unpaced), a guest CPU dispatch thread "
    "with nothing to run keeps checking for work for up to this long before "
    "sleeping, and whoever hands it work skips the wake-up call: a job "
    "system that kicks its workers dozens of times a frame pays a thread "
    "wake-up (tens of microseconds each) on every kick otherwise, and that "
    "latency is what its frame time is made of. Also while the game loads "
    "from storage. Adaptive per CPU: skipped while it doesn't pay off. Never "
    "while gameplay frames have spare time or the thermal governor reports a "
    "hot device - then sleeping at once is right. 0 disables.",
    "Kernel");

namespace xe {
namespace kernel {

// Logical CPU index of the host thread currently executing, or -1 on any
// non-dispatch thread. Set by each CPU's RunLoop.
static thread_local int t_current_cpu = -1;

int GuestScheduler::CurrentDispatchCpu() { return t_current_cpu; }

void GuestScheduler::WakeCpu(Cpu& cpu) {
  if (cpu.ready_event) {
    cpu.ready_event->Set();
  }
#if defined(__linux__)
  if (cpu.fiber_parked.load()) {
    cpu.park_futex.fetch_add(1);
    syscall(SYS_futex, &cpu.park_futex, FUTEX_WAKE_PRIVATE, 1, nullptr,
            nullptr, 0);
  }
#endif
}

bool GuestScheduler::ParkCurrentDispatchThread(XThread* self, int64_t ns) {
  if (ns <= 0) {
    return false;
  }
#if defined(__linux__)
  const int cpu_index = self ? self->scheduler_links().cpu : -1;
  if (cpu_index >= 0 && cpu_index < kMaxCpus) {
    Cpu& cpu = cpus_[cpu_index];
    // What is ready on this CPU already is something the caller could not
    // give way to (it tried just before: fibers of a lower priority). Only
    // what becomes ready from now on ends the sleep.
    const uint32_t ready_before =
        cpu.ready_summary.load(std::memory_order_relaxed);
    const uint32_t futex_value = cpu.park_futex.load();
    // Marked parked like an idle dispatch thread, so whoever readies a fiber
    // here or signals one of its blocked waiters wakes it (WakeCpu). A wake
    // that raced these stores is caught by the re-check below, or at worst
    // the sleep runs its (short) course.
    cpu.fiber_parked.store(true);
    cpu.parked.store(true);
    // Sequentially consistent loads: a relaxed load may be satisfied before
    // the stores above are visible (ARM64), and a waker that checked
    // fiber_parked in between would skip the futex wake.
    bool woken = cpu.ready_summary.load() != ready_before ||
                 cpu.repoll_now.load();
    if (!woken) {
      timespec timeout;
      timeout.tv_sec = time_t(ns / 1000000000);
      timeout.tv_nsec = long(ns % 1000000000);
      // 0: woken (or spuriously back); EAGAIN: a wake came in between.
      woken = syscall(SYS_futex, &cpu.park_futex, FUTEX_WAIT_PRIVATE,
                      futex_value, &timeout, nullptr, 0) == 0 ||
              errno == EAGAIN;
    }
    cpu.parked.store(false);
    cpu.fiber_parked.store(false);
    return woken;
  }
#endif
  xe::threading::NanoSleep(ns);
  return false;
}

// A configured spin park length as regulated by the frame pacer.
static int64_t ScaledSpinParkNs(int64_t park_us) {
  return park_us * 1000 *
         int64_t(xe::cpu::backend::spin_park_scale_q8.load(
             std::memory_order_relaxed)) /
         256;
}

// Off a dispatch thread there is no CPU to index, so the caller must bail.
static bool OnDispatchThread(const char* what) {
  if (t_current_cpu >= 0) {
    return true;
  }
  XELOGW("GuestScheduler: {} called off a dispatch thread, ignoring", what);
  return false;
}

// Clamps a thread's priority to the ready-queue index range [0, 31].
static int ClampPriority(int32_t priority) {
  return priority < 0 ? 0 : (priority > 31 ? 31 : priority);
}

// Safepoints that may decline to preempt before one is forced through anyway.
// A guest spinning at DISPATCH_LEVEL passes safepoints at roughly the loop
// rate, so this is a short wait in wall-clock terms, and the alternative is an
// unbounded livelock when the holder it spins on is co-resident.
static constexpr uint32_t kMaxIrqlPreemptDefers = 4096;
// Reporting threshold for the lock case, which is never forced.
static constexpr uint32_t kLockPreemptDeferReport = 65536;

// JIT safepoint handler. The cold path cleared the flag, so the deferred
// cases re-set it to retry at the next safepoint.
static void PreemptCurrentFiber(void* /*raw_context*/) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    return;
  }
  auto* context = self->thread_state()->context();
  auto& links = self->scheduler_links();
  // A co-resident fiber would re-enter the recursive lock on this host thread,
  // silently breaking mutual exclusion, so this one is never forced. Report a
  // fiber stuck here instead - it means guest code is spinning under the global
  // lock, which the lock's own holder has to resolve.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    context->preempt_requested = 1;
    if (++links.preempt_defers_lock == kLockPreemptDeferReport) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' has declined {} preemptions "
          "holding the global critical region; co-resident fibers cannot run",
          self->thread_id(), self->thread_name(), kLockPreemptDeferReport);
    }
    return;
  }
  links.preempt_defers_lock = 0;
  // At DISPATCH_LEVEL the console masks the decrementer, but it also runs the
  // lock holder on another core. Here the holder may be a fiber queued behind
  // this one, so honoring the mask indefinitely livelocks. Defer a bounded
  // number of times, then switch anyway - IRQL still orders guest APCs.
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  bool forced_at_irql = false;
  if (kpcr->current_irql >= 2) {
    if (++links.preempt_defers_irql < kMaxIrqlPreemptDefers) {
      context->preempt_requested = 1;
      return;
    }
    forced_at_irql = true;
    self->kernel_state()->guest_scheduler()->NoteForcedPreempt();
    if (!links.forced_preempt_logged) {
      links.forced_preempt_logged = true;
      XELOGW(
          "GuestScheduler: forcing preemption of tid={:08X} '{}' at IRQL {} "
          "after {} declined safepoints (first time for this thread)",
          self->thread_id(), self->thread_name(), uint32_t(kpcr->current_irql),
          links.preempt_defers_irql);
    }
  }
  links.preempt_defers_irql = 0;
  if (kpcr->current_irql < 2 &&
      self->kernel_state()->guest_scheduler()->ParkForSnapshot(self)) {
    return;
  }
  // Involuntary quantum end, so no yield to a lower-priority thread - except
  // on the forced path, where the whole point is to reach a holder the strict
  // priority order would keep queued behind us.
  auto* scheduler = self->kernel_state()->guest_scheduler();
  const bool other_ran = scheduler->YieldCurrentThread(true, forced_at_irql);
  if (!forced_at_irql) {
    scheduler->NoteSpinSample(self, other_ran);
  }
}

// Spin-backoff hook. A collapsed guest spin-wait calls this instead of
// burning its dispatch CPU: the producer it waits on may be a fiber queued
// behind the caller on this same thread, which only a yield can let run. A
// plain voluntary yield, not a quantum end, so a preempted-at-head fiber
// keeps its slice semantics.
static void SpinBackoffYieldFiber(void* /*raw_context*/) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    // Guest code on a non-fiber thread (interpreter, early init).
    for (uint32_t n = 0; n < 8; ++n) {
#if XE_ARCH_ARM64
      __asm__ __volatile__("isb sy" ::: "memory");
#endif
    }
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  auto* context = self->thread_state()->context();
  if (context->preempt_requested) {
    // The original trigger: give way as asked, no idle accounting.
    scheduler->YieldCurrentThread(false);
    return;
  }
  // Only spins that a preemption could interrupt may give way on their own:
  // not under the global lock (a co-resident fiber would re-enter it on this
  // host thread), not at DISPATCH_LEVEL (guest per-processor data, like job
  // queues, is protected by raising IRQL - no other thread may run on this
  // processor until it drops).
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return;
  }
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  if (kpcr->current_irql >= 2) {
    return;
  }
  scheduler->NoteIdleSpin(scheduler->YieldCurrentThread(false));
}

static std::atomic<uint64_t> guest_sync_counters[GuestSyncStats::kCounterCount];
static std::atomic<uint64_t> guest_sync_log_ms{0};

void GuestSyncStats::Add(Counter counter) {
  if (cvars::log_guest_sync_stats) {
    guest_sync_counters[counter].fetch_add(1, std::memory_order_relaxed);
  }
}

void GuestSyncStats::LogIfDue() {
  if (!cvars::log_guest_sync_stats) {
    return;
  }
  const uint64_t now_ms = Clock::QueryHostUptimeMillis();
  uint64_t last_ms = guest_sync_log_ms.load(std::memory_order_relaxed);
  if (now_ms - last_ms < 5000 ||
      !guest_sync_log_ms.compare_exchange_strong(last_ms, now_ms)) {
    return;
  }
  uint64_t counts[kCounterCount];
  for (uint32_t i = 0; i < kCounterCount; ++i) {
    counts[i] = guest_sync_counters[i].exchange(0, std::memory_order_relaxed);
  }
  if (!last_ms) {
    return;
  }
  const double seconds = double(now_ms - last_ms) / 1000.0;
  XELOGI(
      "GuestSyncStats: per second: yields={:.0f} (idle {:.0f}) "
      "zero_waits={:.0f} (timed out {:.0f}) pulses={:.0f} (with waiters "
      "{:.0f}) poll_parks={:.0f} timebase_parks={:.0f} spin_samples={:.0f} "
      "(parked {:.0f}) pulse_throttles={:.0f} idle_spins={:.0f} (hit {:.0f}) "
      "polls: fresh={:.0f} gap={:.0f} new_state={:.0f} same_state={:.0f} "
      "tight={:.0f} | slack={} park_scale={:.2f} loading={} probe={}",
      counts[kYield] / seconds, counts[kIdleYield] / seconds,
      counts[kZeroWait] / seconds, counts[kZeroWaitTimeout] / seconds,
      counts[kPulse] / seconds, counts[kPulseWithWaiters] / seconds,
      counts[kPollPark] / seconds, counts[kTimebasePark] / seconds,
      counts[kSpinSampleHit] / seconds, counts[kSpinSamplePark] / seconds,
      counts[kPulseThrottle] / seconds, counts[kIdleSpin] / seconds,
      counts[kIdleSpinHit] / seconds, counts[kPollFresh] / seconds,
      counts[kPollGapTooLong] / seconds, counts[kPollNewState] / seconds,
      counts[kPollSameState] / seconds, counts[kPollTight] / seconds,
      now_ms < xe::cpu::backend::frame_slack_until_ms.load(
                   std::memory_order_relaxed)
          ? 1
          : 0,
      double(xe::cpu::backend::spin_park_scale_q8.load(
          std::memory_order_relaxed)) /
          256.0,
      xe::cpu::backend::frame_pace_loading.load(std::memory_order_relaxed)
          ? 1
          : 0,
      xe::cpu::backend::frame_limiter_probe.load(std::memory_order_relaxed)
          ? 1
          : 0);
}

static void TimebaseSpinPark(void* /*raw_context*/) {
  const int32_t park_us = cvars::guest_timebase_spin_park_us;
  if (park_us <= 0) {
    return;
  }
  const uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (now_ms >= xe::cpu::backend::frame_slack_until_ms.load(
                    std::memory_order_relaxed)) {
    // No slack in the frame: the poll loop keeps its full speed.
    return;
  }
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    return;
  }
  // Same conditions as a collapsed spin backoff giving way on its own.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return;
  }
  auto* context = self->thread_state()->context();
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  if (kpcr->current_irql >= 2) {
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  if (!scheduler->YieldCurrentThread(false)) {
    GuestSyncStats::Add(GuestSyncStats::kTimebasePark);
    if (scheduler->ParkCurrentDispatchThread(self,
                                             ScaledSpinParkNs(park_us))) {
      // Something became runnable on this CPU meanwhile.
      scheduler->YieldCurrentThread(false);
    }
  }
}

// Raw host ticks per us for the watchdog's deadline math, 0 if unusable.
static double CalibrateTicksPerUs() {
  // Median of three short samples. A single busy-spin sample can be stretched
  // by a migration between big and little cores or a DVFS transition mid-loop,
  // and the accept range below is far too wide to catch a merely skewed one.
  double samples[3] = {};
  for (double& sample : samples) {
    uint64_t qpc_freq = Clock::host_tick_frequency_platform();
    uint64_t qpc0 = Clock::host_tick_count_platform();
    uint64_t tsc0 = Clock::host_tick_count_raw();
    uint64_t qpc_end = qpc0 + qpc_freq / 2000;  // ~0.5 ms
    while (Clock::host_tick_count_platform() < qpc_end) {
    }
    uint64_t qpc1 = Clock::host_tick_count_platform();
    uint64_t tsc1 = Clock::host_tick_count_raw();
    double secs = qpc1 > qpc0 ? double(qpc1 - qpc0) / double(qpc_freq) : 0.0;
    sample = secs > 0.0 ? double(tsc1 - tsc0) / (secs * 1e6) : 0.0;
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  if (samples[1] > samples[2]) {
    std::swap(samples[1], samples[2]);
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  double per_us = samples[1];
  // Spans an x86 TSC at 1-6 GHz and an ARM64 generic timer at 1-100 MHz.
  if (per_us < 0.5 || per_us > 100000.0) {
    return 0.0;
  }
  return per_us;
}

GuestScheduler::GuestScheduler(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

GuestScheduler::~GuestScheduler() { Shutdown(); }

bool GuestScheduler::enabled() { return cvars::guest_scheduler; }

int GuestScheduler::DispatchCpuOf(uint8_t guest_cpu) const {
  // Wrap rather than fold to 0: an out-of-range guest CPU is already unusual,
  // and folding every one of them onto CPU 0 stacks them on the dispatch
  // thread the main thread already uses.
  return guest_cpu % kMaxCpus;
}

int GuestScheduler::CpuOf(XThread* thread) const {
  return DispatchCpuOf(thread->guest_object<X_KTHREAD>()->current_cpu);
}

void GuestScheduler::EnsureStarted() {
  xe::threading::guest_wake_generation_enabled = cvars::guest_spin_wfe;
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  xe::cpu::backend::preempt_yield_handler = &PreemptCurrentFiber;
  xe::cpu::backend::spin_backoff_yield_handler = &SpinBackoffYieldFiber;
  xe::cpu::backend::timebase_spin_handler = &TimebaseSpinPark;
  // Not in the ctor, which runs before per-title cvar overrides are applied.
  double ticks_per_us = CalibrateTicksPerUs();
  ticks_per_us_ = ticks_per_us;
  quantum_ticks_ =
      static_cast<uint64_t>(ticks_per_us * cvars::guest_scheduler_quantum_us);
  if (quantum_ticks_) {
    XELOGI("GuestScheduler: preemption slice = {} us ({} ticks)",
           uint32_t(cvars::guest_scheduler_quantum_us), quantum_ticks_);
  } else {
    // Priority and wake preemption still work, they raise the flag directly.
    XELOGW(
        "GuestScheduler: no timeslice preemption ({}), a fiber that never "
        "yields or waits can hog its CPU",
        ticks_per_us > 0.0 ? "guest_scheduler_quantum_us is 0"
                           : "host tick counter did not calibrate");
  }

  for (int i = 0; i < kMaxCpus; ++i) {
    cpus_[i].ready_event = xe::threading::Event::CreateAutoResetEvent(false);
  }
  if (quantum_ticks_) {
    watchdog_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    watchdog_thread_ =
        xe::threading::Thread::Create(params, [this]() { WatchdogLoop(); });
    watchdog_thread_->set_name("Guest Scheduler Watchdog");
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    xe::threading::Thread::CreationParameters params;
    cpus_[i].host_thread =
        xe::threading::Thread::Create(params, [this, i]() { RunLoop(i); });
    cpus_[i].host_thread->set_name(std::string("Guest CPU ") +
                                   std::to_string(i));
  }
}

void GuestScheduler::Shutdown() {
  // The threads are about to be terminated; releasing them into the ready
  // lists would let them run guest code first.
  // Publish stop before releasing the barrier: blocked waiters must not
  // repoll in the gap and resume guest code during cold teardown.
  shutting_down_.store(true);
  EndSnapshot(false);
  if (!started_.load() && !io_started_.load()) {
    return;
  }
  if (stopped_.load()) {
    return;
  }
  for (Cpu& cpu : cpus_) {
    if (cpu.ready_event) {
      WakeCpu(cpu);
    }
  }
  if (io_event_) {
    io_event_->Set();
  }
  if (watchdog_event_) {
    watchdog_event_->Set();
  }
  for (Cpu& cpu : cpus_) {
    if (!cpu.host_thread) {
      continue;
    }
    // Join before reset(), which only closes the handle. A spinning fiber
    // only leaves via the preempt flag, so keep raising it until the loop
    // drains.
    int waited_ms = 0;
    while (xe::threading::Wait(cpu.host_thread.get(), false,
                               std::chrono::milliseconds(50)) ==
           xe::threading::WaitResult::kTimeout) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        for (int i = 0; i < kMaxCpus; ++i) {
          if (XThread* running = cpus_[i].current_thread) {
            running->thread_state()->context()->preempt_requested = 1;
          }
        }
      }
      for (int i = 0; i < kMaxCpus; ++i) {
        if (cpus_[i].ready_event) {
          WakeCpu(cpus_[i]);
        }
      }
      waited_ms += 50;
      if (waited_ms % 2000 == 0) {
        XELOGW(
            "GuestScheduler: shutdown has waited {} ms for a dispatch thread, "
            "its fiber is not reaching a safepoint",
            waited_ms);
      }
    }
    cpu.host_thread.reset();
  }
  if (watchdog_thread_) {
    xe::threading::Wait(watchdog_thread_.get(), false);
    watchdog_thread_.reset();
  }
  // After the dispatch threads, so no fiber is still watching a BlockingCall.
  if (io_thread_) {
    xe::threading::Wait(io_thread_.get(), false);
    io_thread_.reset();
  }
  // Everything still linked is unreachable now that the dispatch threads are
  // gone. Reclaim each thread so a relaunch does not leak it and its stack.
  std::vector<XThread*> leftovers;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto drain = [&leftovers](XThread*& head, XThread*& tail) {
      for (XThread* t = head; t;) {
        auto& links = t->scheduler_links();
        XThread* next = links.ready_next;
        links.queued = false;
        links.blocked = false;
        links.suspended = false;
        links.ready_next = nullptr;
        leftovers.push_back(t);
        t = next;
      }
      head = nullptr;
      tail = nullptr;
    };
    for (Cpu& cpu : cpus_) {
      for (int prio = 0; prio < 32; ++prio) {
        drain(cpu.ready_head[prio], cpu.ready_tail[prio]);
      }
      cpu.ready_summary.store(0, std::memory_order_relaxed);
      drain(cpu.blocked_head, cpu.blocked_tail);
      drain(cpu.suspended_head, cpu.suspended_tail);
      if (cpu.exited_thread) {
        leftovers.push_back(cpu.exited_thread);
        cpu.exited_thread = nullptr;
      }
      cpu.yield_to_other = nullptr;
      cpu.current_thread = nullptr;
      cpu.has_blocked.store(false, std::memory_order_relaxed);
    }
  }
  if (!leftovers.empty()) {
    XELOGI("GuestScheduler: reclaiming {} parked fibers on shutdown",
           leftovers.size());
  }
  for (XThread* t : leftovers) {
    // A parked waiter's registration would otherwise dangle on the object.
    XObject::AbandonCooperativeWait(t);
    t->ReclaimExited();
  }
  stopped_.store(true);
}

void GuestScheduler::EnqueueReady(XThread* thread, int cpu_index,
                                  bool yield_to_other) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    // The single gate for every "make it runnable" request, so a state that
    // already owns its wake-up is a silent no-op. Blocked and suspended move
    // via RereadyBlocked and ResumeThread, all three lists sharing ready_next.
    if (links.snapshot_parked || links.blocked || links.suspended) {
      return;
    }
    // A running fiber's context is not saved until it yields, so only the
    // dispatch thread that owns it, links.cpu, may re-queue it.
    if (links.running && links.cpu != t_current_cpu) {
      return;
    }
    if (links.queued) {
      return;
    }
    links.queued = true;
    links.cpu = cpu_index;
    bool at_head = links.preempted;
    links.preempted = false;
    LinkReadyLocked(cpus_[cpu_index], thread, at_head);
    if (yield_to_other) {
      cpus_[cpu_index].yield_to_other = thread;
    }
  }
  // Only a parked dispatch thread needs the syscall.
  if (cpus_[cpu_index].parked.load() && cpus_[cpu_index].ready_event) {
    WakeCpu(cpus_[cpu_index]);
  }
}

void GuestScheduler::MarkReady(XThread* thread) {
  assert_not_null(thread);
  // Don't re-enqueue a terminated thread, or a stray Resume could revive a
  // zombie.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  EnqueueReady(thread, CpuOf(thread));
}

void GuestScheduler::ResumeThread(XThread* thread) {
  assert_not_null(thread);
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    if (links.suspended) {
      Cpu& cpu = cpus_[links.cpu];
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      links.suspended = false;
      links.ready_next = nullptr;
    }
  }
  // Only enqueues if it was never queued, e.g. created suspended.
  MarkReady(thread);
}

bool GuestScheduler::ParkSuspended(XThread* thread, int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  // Re-read under the lock, a Resume racing the dispatcher's check would have
  // found us not yet parked and parking anyway would strand the thread.
  // Termination overrides suspension, run it so it can exit.
  if (thread->suspend_count() == 0 ||
      links.terminate_pending.load(std::memory_order_relaxed)) {
    return false;
  }
  // Clearing running last, so it is never both unowned and unlisted.
  links.suspended = true;
  links.cpu = cpu_index;
  links.ready_next = nullptr;
  links.quantum_deadline_tick = 0;
  Cpu& cpu = cpus_[cpu_index];
  LinkTailLocked(cpu.suspended_head, cpu.suspended_tail, thread);
  links.running = false;
  return true;
}

XThread* GuestScheduler::HighestReadyExcept(const Cpu& cpu, XThread* except) {
  uint32_t summary = cpu.ready_summary.load(std::memory_order_relaxed);
  while (summary) {
    int level = 31 - xe::lzcnt(summary);
    summary &= ~(uint32_t(1) << level);
    XThread* head = cpu.ready_head[level];
    if (head != except) {
      return head;
    }
    // Its successor outranks anything on a lower level.
    if (except->scheduler_links().ready_next) {
      return except->scheduler_links().ready_next;
    }
  }
  return nullptr;
}

XThread* GuestScheduler::DequeueReady(int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  Cpu& cpu = cpus_[cpu_index];
  if (cpu.ready_summary.load(std::memory_order_relaxed) == 0) {
    return nullptr;
  }
  // Strict priority alone lets a high-priority yield-spinner deadlock on the
  // lower-priority co-resident it depends on, so a voluntary yield opts out.
  XThread* yielder = cpu.yield_to_other;
  cpu.yield_to_other = nullptr;

  // Highest set bit = highest ready priority.
  int level =
      31 - xe::lzcnt(cpu.ready_summary.load(std::memory_order_relaxed));
  XThread* thread = cpu.ready_head[level];
  if (yielder && thread == yielder) {
    if (XThread* other = HighestReadyExcept(cpu, yielder)) {
      // |other| may sit mid-list, so unlink it generally rather than as a head.
      int other_level = other->scheduler_links().queued_prio;
      UnlinkLocked(cpu.ready_head[other_level], cpu.ready_tail[other_level],
                   other);
      if (!cpu.ready_head[other_level]) {
        ClearReadyLevel(cpu, other_level);
      }
      auto& other_links = other->scheduler_links();
      other_links.ready_next = nullptr;
      other_links.queued = false;
      other_links.running = true;
      return other;
    }
  }

  auto& links = thread->scheduler_links();
  cpu.ready_head[level] = links.ready_next;
  if (!cpu.ready_head[level]) {
    cpu.ready_tail[level] = nullptr;
    ClearReadyLevel(cpu, level);
  }
  links.ready_next = nullptr;
  links.queued = false;
  // Owned from here, not from SwitchTo, because in between it is in no list and
  // a concurrent MarkReady would queue it onto another CPU.
  links.running = true;
  return thread;
}

void GuestScheduler::LinkTailLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  if (tail) {
    tail->scheduler_links().ready_next = thread;
  } else {
    head = thread;
  }
  tail = thread;
}

void GuestScheduler::LinkHeadLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  thread->scheduler_links().ready_next = head;
  head = thread;
  if (!tail) {
    tail = thread;
  }
}

void GuestScheduler::LinkReadyLocked(Cpu& cpu, XThread* thread, bool at_head) {
  auto& links = thread->scheduler_links();
  // A yielding fiber re-queues itself here - that must not count as progress
  // for its own idle check (NoteIdleSpin), or it would never park.
  if (thread != XThread::GetCurrentFiberThread()) {
    xe::threading::BumpGuestWakeGeneration();
  }
  int prio = ClampPriority(thread->priority());
  links.queued_prio = prio;
  links.ready_next = nullptr;
  if (at_head) {
    LinkHeadLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  } else {
    LinkTailLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  }
  cpu.ready_summary.store(
      cpu.ready_summary.load(std::memory_order_relaxed) | (uint32_t(1) << prio),
      std::memory_order_relaxed);
  // Outranking the running fiber flags it, so its next JIT safepoint yields
  // and the dispatcher picks us.
  XThread* running = cpu.current_thread;
  if (running && running != thread &&
      prio > ClampPriority(running->priority())) {
    running->scheduler_links().preempted = true;
    running->thread_state()->context()->preempt_requested = 1;
  }
}

void GuestScheduler::UnlinkLocked(XThread*& head, XThread*& tail,
                                  XThread* thread) {
  XThread** link = &head;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = thread->scheduler_links().ready_next;
      if (tail == thread) {
        tail = prev;
      }
      return;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
}

void GuestScheduler::RequeueForPriority(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  if (!links.queued || links.cpu < 0) {
    return;
  }
  Cpu& cpu = cpus_[links.cpu];
  int old = links.queued_prio;
  UnlinkLocked(cpu.ready_head[old], cpu.ready_tail[old], thread);
  if (!cpu.ready_head[old]) {
    ClearReadyLevel(cpu, old);
  }
  LinkReadyLocked(cpu, thread, false);
}

bool GuestScheduler::ForgetThread(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  // A thread that ever ran has a live fiber stack and one a dispatch thread
  // owns is about to be switched to, so neither may be freed.
  const bool reclaimable = !links.has_run && !links.running;
  if (links.cpu >= 0) {
    Cpu& cpu = cpus_[links.cpu];
    if (links.queued) {
      int prio = links.queued_prio;
      UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
      if (!cpu.ready_head[prio]) {
        ClearReadyLevel(cpu, prio);
      }
    } else if (links.blocked) {
      UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
    } else if (links.suspended) {
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
    }
  }
  links.queued = false;
  links.blocked = false;
  links.suspended = false;
  links.ready_next = nullptr;
  // Drop every raw pointer a CPU may still hold to it. A fiber detaching itself
  // keeps current_thread, which SwitchTo clears on the way out.
  for (Cpu& cpu : cpus_) {
    if (cpu.yield_to_other == thread) {
      cpu.yield_to_other = nullptr;
    }
    if (cpu.exited_thread == thread) {
      cpu.exited_thread = nullptr;
    }
    if (cpu.current_thread == thread && !links.running) {
      cpu.current_thread = nullptr;
    }
  }
  return reclaimable;
}

bool GuestScheduler::TerminateThread(XThread* thread) {
  int wake_cpu = -1;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    links.terminate_pending.store(true, std::memory_order_relaxed);
    if (stopped_.load() || !started_.load()) {
      // No dispatcher will ever run it again, detach it and let the caller
      // free the stack, parked frames and all.
      if (links.cpu >= 0) {
        Cpu& cpu = cpus_[links.cpu];
        if (links.queued) {
          int prio = links.queued_prio;
          UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
          if (!cpu.ready_head[prio]) {
            ClearReadyLevel(cpu, prio);
          }
        } else if (links.blocked) {
          UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
        } else if (links.suspended) {
          UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
        }
      }
      links.queued = false;
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      assert_false(links.running);
      return !links.running;
    }
    if (links.running) {
      // Force it to a safepoint, where ExitIfTerminated ends it.
      thread->thread_state()->context()->preempt_requested = 1;
      return false;
    }
    if (links.snapshot_parked) {
      // Parked by the snapshot barrier, in no list. Termination overrides it:
      // queue it so it wakes inside ParkForSnapshot and exits at
      // ExitIfTerminated without running any more guest code.
      links.snapshot_parked = false;
      links.snapshot_pc = 0;
      links.ready_next = nullptr;
      links.queued = true;
      LinkReadyLocked(cpus_[links.cpu], thread, true);
      wake_cpu = links.cpu;
    } else if (links.blocked || links.suspended) {
      // Termination overrides a wait or suspend. Dispatch it so it exits on
      // its own stack and the idle loop reclaims it.
      Cpu& cpu = cpus_[links.cpu];
      if (links.blocked) {
        UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
      } else {
        UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      }
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      links.queued = true;
      LinkReadyLocked(cpus_[links.cpu], thread, true);
      wake_cpu = links.cpu;
    } else if (!links.queued && !links.has_run) {
      // Created suspended and never queued, nothing is on its stack.
      return true;
    }
    // A queued thread diverts at its resume point, and one that already
    // exited or crashed is the dispatcher's to reclaim.
  }
  if (wake_cpu >= 0 && cpus_[wake_cpu].parked.load() &&
      cpus_[wake_cpu].ready_event) {
    WakeCpu(cpus_[wake_cpu]);
  }
  return false;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  auto& links = next->scheduler_links();
  if (!links.has_run) {
    links.has_run = true;
    dispatched_any_.store(true);
    XELOGI("GuestScheduler: first run tid={:08X} '{}'", next->thread_id(),
           next->thread_name());
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    assert_true(links.running);
    cpus_[t_current_cpu].switch_seq.fetch_add(1, std::memory_order_relaxed);
    cpus_[t_current_cpu].current_thread = next;
    // Grant a fresh slice only if the previous one was consumed. A preempted
    // thread resumes with its remainder, so its quantum end still arrives.
    if (!links.quantum_deadline_tick) {
      links.quantum_deadline_tick =
          Clock::host_tick_count_raw() + quantum_ticks_;
    }
    cpus_[t_current_cpu].quantum_deadline_tick = links.quantum_deadline_tick;
  }
  stats_.switches.fetch_add(1, std::memory_order_relaxed);
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  // A flag raised while this fiber was off-CPU is stale, the dispatcher
  // already served it. A raise racing this clear is restored by the watchdog.
  next->thread_state()->context()->preempt_requested =
      snapshot_active_.load(std::memory_order_acquire) ? 1 : 0;
  next->fiber()->SwitchTo();
  // Back on the idle fiber.
  {
    std::lock_guard<std::mutex> lock(lock_);
    links.running = false;
    cpus_[t_current_cpu].current_thread = nullptr;
  }
  XThread::SetCurrentThread(nullptr);
}

void GuestScheduler::ReportGlobalLockHazard() {
  static constexpr size_t kMaxReports = 32;
  static constexpr size_t kMaxFrames = 32;

  XThread* self = XThread::GetCurrentThread();
  uint32_t tid = self ? self->thread_id() : 0;
  const char* name = self ? self->thread_name().c_str() : "?";

  cpu::StackWalker* stack_walker =
      kernel_state_->processor() ? kernel_state_->processor()->stack_walker()
                                 : nullptr;
  if (!stack_walker) {
    if (!global_lock_hazard_saturated_.exchange(true)) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the "
          "global critical region (no stack walker to name the shim).",
          tid, name);
    }
    return;
  }

  uint64_t frame_pcs[kMaxFrames] = {};
  uint64_t stack_hash = 0;
  size_t frame_count =
      stack_walker->CaptureStackTrace(frame_pcs, 0, kMaxFrames, &stack_hash);
  if (!frame_count) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(global_lock_hazard_mutex_);
    if (!global_lock_hazard_stacks_.insert(stack_hash).second) {
      return;  // Already reported.
    }
    if (global_lock_hazard_stacks_.size() >= kMaxReports) {
      global_lock_hazard_saturated_.store(true, std::memory_order_relaxed);
    }
  }

  cpu::StackFrame frames[kMaxFrames] = {};
  stack_walker->ResolveStack(frame_pcs, frames, frame_count);

  uint32_t guest_lr = self ? uint32_t(self->thread_state()->context()->lr) : 0;
  // The region is a scoped lock, so the acquiring shim is an ancestor frame.
  XELOGW(
      "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the global "
      "critical region (guest lr={:08X}). A co-resident fiber can now re-enter "
      "the recursive lock. Yield path host stack:",
      tid, name, guest_lr);
  for (size_t i = 0; i < frame_count; ++i) {
    cpu::StackFrame& frame = frames[i];
    if (frame.type == cpu::StackFrame::Type::kHost) {
      XELOGW("  #{:02} host  {:016X} {}", i, frame.host_pc,
             frame.host_symbol.name[0] ? frame.host_symbol.name : "?");
    } else {
      XELOGW("  #{:02} guest {:016X} pc={:08X}", i, frame.host_pc,
             frame.guest_pc);
    }
  }
}

void GuestScheduler::YieldToScheduler() {
  if (!OnDispatchThread("YieldToScheduler")) {
    return;
  }
  if (!global_lock_hazard_saturated_.load(std::memory_order_relaxed) &&
      xe::global_critical_region::is_held_by_current_thread()) {
    ReportGlobalLockHazard();
  }
  cpus_[t_current_cpu].idle_fiber->SwitchTo();
}

void GuestScheduler::ExitIfTerminated() {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self || !self->scheduler_links().terminate_pending.load(
                   std::memory_order_relaxed)) {
    return;
  }
  // The wait registration may be newer than the one Terminate abandoned.
  XObject::AbandonCooperativeWait(self);
  // A park or dispatch since the terminate may have overwritten this.
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_TERMINATED;
  NotifyThreadExited(self);
  YieldToScheduler();  // never returns
}

bool GuestScheduler::YieldCurrentThread(bool quantum_end, bool to_lower) {
  if (!OnDispatchThread("YieldCurrentThread")) {
    return false;
  }
  // An externally terminated thread stops here.
  ExitIfTerminated();
  XThread* self = XThread::GetCurrentThread();
  auto& links = self->scheduler_links();
  // Nothing to yield to: skip the dispatcher round trip, which would otherwise
  // enqueue and immediately resume this same fiber. Sleep(0) spin-waits drive
  // this hundreds of thousands of times a second.
  //
  // Costs at most a quantum of latency - every condition below is re-armed
  // elsewhere (ready_summary, repoll_now, preempt_requested, the watchdog).
  // The reads are lock-free and may be stale; a stale "idle" just means the
  // next call takes the slow path.
  if (!quantum_end && !links.preempted && self->suspend_count() == 0 &&
      !self->thread_state()->context()->preempt_requested) {
    const Cpu& cpu = cpus_[t_current_cpu];
    if (cpu.ready_summary.load(std::memory_order_relaxed) == 0 &&
        !cpu.repoll_now.load(std::memory_order_relaxed)) {
      return false;  // nothing else ran, which is exactly what we report
    }
  }
  // A slice cut short by a higher-priority thread is not a quantum end, that
  // thread re-runs at the head instead.
  if (quantum_end && !links.preempted) {
    self->OnQuantumEnd();
  }
  // Only a preemption keeps the remaining slice, anything else consumed it.
  if (!links.preempted) {
    links.quantum_deadline_tick = 0;
  }
  int cpu_index = t_current_cpu;
  uint64_t seq_before =
      cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed);
  // Re-queue on the current CPU, not the affinity CPU, because our context is
  // not saved until the yield below and another CPU must not grab it yet.
  EnqueueReady(self, t_current_cpu, to_lower);
  YieldToScheduler();
  // Terminated while queued.
  ExitIfTerminated();
  // One dispatch is our own resume, more means another fiber ran in between.
  // A migration to another CPU counts as scheduling activity outright.
  return t_current_cpu != cpu_index ||
         cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed) -
                 seq_before >
             1;
}

void GuestScheduler::NoteIdleSpin(bool other_ran) {
  if (!cvars::guest_spin_wfe) {
    return;
  }
  // Nothing else was ready on this dispatch thread. If no guest atomic
  // store and no wake-up happened since this thread's previous idle report,
  // the loop has nothing new to find: park the core until one does (an
  // enqueue's stwcx. or a signal wakes it at once) or the timer event
  // stream ticks. Any change returns immediately for the loop to rescan.
  // Per guest thread (fibers share dispatch threads): the generation minus
  // this thread's own store-conditional bumps, so a scan that takes and
  // releases its own locks still parks when nobody else did anything.
  auto* ppc_context = cpu::ThreadState::Get()->context();
  const uint32_t generation =
      xe::threading::guest_wake_generation.load(std::memory_order_acquire);
  const uint32_t others = generation - ppc_context->wake_self_bumps;
  if (other_ran || others != ppc_context->wake_seen_others) {
    ppc_context->wake_seen_others = others;
    return;
  }
  // Wait for the global generation to move past its current value; a bump
  // by this thread can't happen while it's parked here.
  xe::threading::WaitGuestWakeGeneration(generation);
  ppc_context->wake_seen_others =
      xe::threading::guest_wake_generation.load(std::memory_order_acquire) -
      ppc_context->wake_self_bumps;
}

// XXH3 of up to 1 KiB of guest stack above the stack pointer: the frames of
// the callers, where the registers of the outer functions are saved. A value
// that never repeats if the thread is not on its normal stack.
static uint64_t GuestStackFingerprint(cpu::ppc::PPCContext* context) {
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  const uint32_t stack_pointer = uint32_t(context->r[1]);
  const uint32_t stack_base = kpcr->stack_base_ptr;
  const uint32_t stack_end = kpcr->stack_end_ptr;
  if (stack_pointer >= stack_end && stack_pointer < stack_base) {
    return XXH3_64bits(
        context->TranslateVirtualGPR<const uint8_t*>(stack_pointer),
        std::min(stack_base - stack_pointer, uint32_t(1024)));
  }
  return Clock::host_tick_count_raw() * UINT64_C(0x9E3779B97F4A7C15);
}

void GuestScheduler::NoteSpinSample(XThread* self, bool other_ran) {
  const int32_t max_park_us = cvars::guest_spin_sample_park_us;
  if (max_park_us <= 0 && !cvars::log_guest_sync_stats &&
      cvars::guest_threads_placement <= 0) {
    return;
  }
  auto* context = self->thread_state()->context();
  auto& links = self->scheduler_links();
  // Per guest CPU tallies for the placement governor. The thread's own CPU,
  // not a thread-local: the fiber may have just resumed on another host
  // thread.
  Cpu* sampled_cpu =
      links.cpu >= 0 && links.cpu < kMaxCpus ? &cpus_[links.cpu] : nullptr;
  if (sampled_cpu) {
    sampled_cpu->spin_samples.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t fingerprint = XXH3_64bits(context->r, sizeof(context->r));
  fingerprint = (fingerprint ^ context->ctr) * UINT64_C(0x9E3779B97F4A7C15);
  fingerprint = (fingerprint ^ context->lr) * UINT64_C(0x9E3779B97F4A7C15);
  fingerprint ^= GuestStackFingerprint(context);
  const bool share_limited = cvars::guest_poll_spin_wait_share > 0;
  const SpinSampleDetector::Result sample = links.spin_sampler.OnQuantumEnd(
      fingerprint,
      std::max(uint32_t(cvars::guest_scheduler_quantum_us), uint32_t(1)),
      uint32_t(std::max(cvars::guest_poll_spin_wait_share, 1)),
      share_limited ? 50 : 0, share_limited ? 20 : 0);
  if (sample.spinning) {
    GuestSyncStats::Add(GuestSyncStats::kSpinSampleHit);
    if (sampled_cpu) {
      sampled_cpu->spin_sample_hits.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (max_park_us <= 0 || other_ran || !sample.park_limit_us) {
    return;
  }
  if (Clock::QueryHostUptimeMillis() >=
      xe::cpu::backend::frame_slack_until_ms.load(std::memory_order_relaxed)) {
    return;
  }
  // As long as the frame pacer allows, but never more than a share of how
  // long the thread has been found in place.
  GuestSyncStats::Add(GuestSyncStats::kSpinSamplePark);
  if (ParkCurrentDispatchThread(
          self, std::min(ScaledSpinParkNs(max_park_us),
                         int64_t(sample.park_limit_us) * 1000))) {
    // Something became runnable on this CPU meanwhile (the caller, the
    // quantum-end handler, is where a fiber gives way).
    YieldCurrentThread(false);
  }
}

// Per dispatch thread, for the fiber that polled last on it (NotePollSpin).
static thread_local PollSpinDetector t_poll_spin;

void GuestScheduler::NotePollSpin(bool is_yield, bool other_ran,
                                  uint32_t key, bool pulse) {
  const int32_t park_us = cvars::guest_poll_spin_park_us;
  if (park_us <= 0 && !pulse) {
    return;
  }
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    return;
  }
  PollSpinDetector& spin = t_poll_spin;
  // What the poll loop looks like from here: where it polls from, what it
  // polls, and where compiled code keeps its loop state across a call - the
  // callee-saved registers (r14-r31), the stack pointer, and the frames of
  // the callers, where the registers of the outer functions are saved (1 KiB
  // above the stack pointer covers several frames). A loop doing work between
  // polls advances a counter or a pointer there; one that only waits comes
  // back with all of it unchanged.
  auto* context = self->thread_state()->context();
  uint64_t hash = context->lr ^ (uint64_t(key) << 32) ^ uint64_t(is_yield);
  for (uint32_t i = 14; i < 32; ++i) {
    hash = (hash ^ context->r[i]) * UINT64_C(0x9E3779B97F4A7C15);
    hash ^= hash >> 29;
  }
  hash = (hash ^ context->r[1]) * UINT64_C(0x9E3779B97F4A7C15);
  if (cvars::guest_poll_spin_stack_fingerprint) {
    hash ^= GuestStackFingerprint(context);
  }
  PollSpinDetector::Params params;
  static const uint64_t ticks_per_us =
      std::max(Clock::host_tick_frequency_raw() / 1000000, uint64_t(1));
  params.ticks_per_us = ticks_per_us;
  params.gap_us = uint32_t(std::max(cvars::guest_poll_spin_gap_us, 1));
  params.tight_gap_us =
      uint32_t(std::max(cvars::guest_poll_spin_tight_gap_us, 0));
  params.base_after_us =
      uint32_t(std::max(cvars::guest_poll_spin_park_after_us, 0));
  params.park_enabled = park_us > 0;
  // Without the limit a sleep may be as long as the whole wait so far.
  params.wait_share =
      uint32_t(std::max(cvars::guest_poll_spin_wait_share, 1));
  if (cvars::guest_poll_spin_wait_share <= 0) {
    params.sleep_overhead_us = 0;
    params.min_park_us = 0;
  }
  params.slack = Clock::QueryHostUptimeMillis() <
                 xe::cpu::backend::frame_slack_until_ms.load(
                     std::memory_order_relaxed);
  const PollSpinDetector::Action action = spin.OnPoll(
      self, hash, other_ran, pulse, key, Clock::host_tick_count_raw(), params);
  static_assert(GuestSyncStats::kPollTight - GuestSyncStats::kPollFresh ==
                uint32_t(PollSpinDetector::Kind::kTight));
  GuestSyncStats::Add(GuestSyncStats::Counter(
      GuestSyncStats::kPollFresh + uint32_t(spin.last_kind())));
  switch (action) {
    case PollSpinDetector::Action::kNone:
      break;
    case PollSpinDetector::Action::kThrottlePulse: {
      // A thread that only pulses an event nobody waits on, over and over,
      // used to be held back by the sleep inside every host pulse. Kept for
      // such a loop now that single pulses don't sleep, whatever the frame's
      // slack.
      if (xe::global_critical_region::is_held_by_current_thread()) {
        break;
      }
      const uint64_t before = Clock::host_tick_count_raw();
      // With the timer slack about what a host pulse used to take (~100 us).
      const bool woken = ParkCurrentDispatchThread(self, 40000);
      GuestSyncStats::Add(GuestSyncStats::kPulseThrottle);
      spin.NoteSlept(Clock::host_tick_count_raw() - before);
      if (woken) {
        // Something became runnable on this CPU meanwhile: give way, if this
        // fiber may (not above passive level). Nothing is touched after.
        auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
        if (kpcr->current_irql < 2) {
          YieldCurrentThread(false);
        }
        return;
      }
      break;
    }
    case PollSpinDetector::Action::kPark: {
      if (xe::global_critical_region::is_held_by_current_thread()) {
        break;
      }
      if (!is_yield) {
        // A wait poll has not let the other fibers of this dispatch thread
        // run.
        auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
        if (kpcr->current_irql >= 2) {
          break;
        }
        // Another fiber ran (or this one moved to another dispatch thread,
        // whose state is not the one referenced here): nothing more to do or
        // to touch.
        if (YieldCurrentThread(false)) {
          return;
        }
      }
      // As long as the frame pacer allows, but never more than a share of
      // how long this wait has already lasted.
      const bool woken = ParkCurrentDispatchThread(
          self, std::min(ScaledSpinParkNs(park_us),
                         int64_t(spin.park_limit_us()) * 1000));
      GuestSyncStats::Add(GuestSyncStats::kPollPark);
      spin.NoteParked(Clock::host_tick_count_raw());
      if (woken) {
        // Something became runnable on this CPU meanwhile: give way (a wait
        // poll got here below DISPATCH_LEVEL, a yield poll is a yield
        // anyway). Nothing is touched after.
        YieldCurrentThread(false);
        return;
      }
      break;
    }
  }
}

void GuestScheduler::CreditPollSpinGap(uint64_t host_ticks) {
  // Time the poller itself spent asleep inside a kernel call (the host event
  // pulse sleeps) is not time its loop spent doing something else.
  PollSpinDetector& spin = t_poll_spin;
  if (spin.owner() && spin.owner() == XThread::GetCurrentFiberThread()) {
    spin.NoteSlept(host_ticks);
  }
}

void GuestScheduler::SpinYield(std::chrono::milliseconds host_sleep) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    // The holder we spin on may be a fiber queued behind us on this same
    // dispatch thread, so yielding the host thread would never let it run.
    auto* scheduler = self->kernel_state()->guest_scheduler();
    if (host_sleep.count()) {
      // Parking rather than re-queueing, so a lone fiber idles instead of
      // spinning its dispatch thread at full speed.
      scheduler->BlockCurrentThread();
    } else {
      scheduler->YieldCurrentThread(false);
    }
    return;
  }
  if (host_sleep.count()) {
    xe::threading::Sleep(host_sleep);
  } else {
    xe::threading::MaybeYield();
  }
}

void GuestScheduler::EnsureIoWorker() {
  std::call_once(io_once_, [this]() {
    io_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    io_thread_ =
        xe::threading::Thread::Create(params, [this]() { IoWorkerLoop(); });
    io_thread_->set_name("Guest I/O");
    io_started_.store(true);
  });
}

bool GuestScheduler::CurrentThreadOffloadsBlockingCalls() {
  if (!enabled() || !XThread::GetCurrentFiberThread()) {
    return false;
  }
  // The offloaded call can need the global critical region itself, and only
  // this fiber can release it, so holding it means running inline.
  return !xe::global_critical_region::is_held_by_current_thread();
}

void GuestScheduler::WaitOnFence(xe::threading::Fence& fence) {
  XThread* self = enabled() ? XThread::GetCurrentFiberThread() : nullptr;
  if (!self) {
    fence.Wait();
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kFence,
                                   nullptr, 0);
  while (!fence.TryWait()) {
    // The signaler touches the fence on this stack, terminate must not free
    // it.
    scheduler->BlockCurrentThread(0, 0, false, false);
  }
  self->clear_cooperative_wait_shape();
}

void GuestScheduler::RunBlockingHostCallOffloaded(
    const std::function<void()>& fn) {
  {
    // Loading detector: three blocking calls within 100 ms. Approximate under
    // concurrency, which is fine for what it is used for.
    static std::atomic<uint64_t> window_start_ms{0};
    static std::atomic<uint32_t> window_calls{0};
    const uint64_t io_now_ms = Clock::QueryHostUptimeMillis();
    if (io_now_ms - window_start_ms.load(std::memory_order_relaxed) >= 100) {
      window_start_ms.store(io_now_ms, std::memory_order_relaxed);
      window_calls.store(1, std::memory_order_relaxed);
    } else if (window_calls.fetch_add(1, std::memory_order_relaxed) + 1 >= 3) {
      xe::cpu::backend::guest_io_burst_until_ms.store(
          io_now_ms + 1000, std::memory_order_relaxed);
    }
  }
  // A save state is waiting for every thread: start no new host I/O, or a
  // title that streams continuously (Dante) is always caught mid-read. Wait
  // in a restartable shape before anything is done; a capture here re-enters
  // this kernel call on restore.
  if (XThread* waiter = XThread::GetCurrentFiberThread()) {
    if (snapshot_active_.load(std::memory_order_acquire)) {
      waiter->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                         nullptr, 0);
      while (snapshot_active_.load(std::memory_order_acquire)) {
        BlockCurrentThread(0, 0, false, false);
      }
      waiter->clear_cooperative_wait_shape();
    }
  }
  EnsureIoWorker();
  BlockingCall call;
  call.fn = &fn;
  call.queued_ns = Clock::host_tick_count_raw();
  {
    std::lock_guard<std::mutex> lock(io_lock_);
    io_queue_.push(&call);
  }
  io_event_->Set();
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kIoOffload,
                                     nullptr, 0);
  }
  while (!call.done.load(std::memory_order_acquire)) {
    // The worker writes |call| on this stack, terminate must not free it.
    BlockCurrentThread(0, 0, false, false);
  }
  if (self) {
    self->clear_cooperative_wait_shape();
  }
}

void GuestScheduler::IoWorkerLoop() {
  while (!shutting_down_.load()) {
    BlockingCall* call = nullptr;
    {
      std::lock_guard<std::mutex> lock(io_lock_);
      if (!io_queue_.empty()) {
        call = io_queue_.front();
        io_queue_.pop();
      }
    }
    if (!call) {
      xe::threading::Wait(io_event_.get(), false);
      continue;
    }
    uint64_t started = Clock::host_tick_count_raw();
    (*call->fn)();
    uint64_t finished = Clock::host_tick_count_raw();
    // Raw ticks, converted only at report time.
    uint64_t queued_for = started - call->queued_ns;
    stats_.io_calls.fetch_add(1, std::memory_order_relaxed);
    stats_.io_queue_ns.fetch_add(queued_for, std::memory_order_relaxed);
    stats_.io_run_ns.fetch_add(finished - started, std::memory_order_relaxed);
    uint64_t prev_max = stats_.io_queue_max_ns.load(std::memory_order_relaxed);
    while (queued_for > prev_max &&
           !stats_.io_queue_max_ns.compare_exchange_weak(
               prev_max, queued_for, std::memory_order_relaxed)) {
    }
    call->done.store(true, std::memory_order_release);
    // Wake the parked caller instead of leaving it to the backoff timer.
    WakeAll();
  }
}

void GuestScheduler::WakeAll() {
  if (!started_.load()) {
    return;
  }
  // Skip the lock when no CPU has a blocked waiter. A stale hint costs at
  // most one backoff interval.
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  // Ask each CPU with a blocked waiter to re-poll, preempting its runner only
  // when a waiter outranks it. An equal-priority preempt would head-requeue
  // the runner past ready threads on every signal and starve them.
  {
    std::lock_guard<std::mutex> lock(lock_);
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (!cpu.blocked_head) {
        continue;
      }
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running &&
          cpu.max_blocked_prio > ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load(std::memory_order_relaxed) &&
        cpus_[i].parked.load() && cpus_[i].ready_event) {
      WakeCpu(cpus_[i]);
    }
  }
}

void GuestScheduler::WakeForSignal(const XObject* object,
                                   XThread* sole_waiter) {
  if (!started_.load()) {
    return;
  }
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  bool wake[kMaxCpus] = {};
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (sole_waiter) {
      // Only the permit FIFO's front can take the permit; every other waiter
      // of this object would wake, poll, fail MayAcquire and re-park.
      auto& links = sole_waiter->scheduler_links();
      if (!links.blocked || links.cpu < 0) {
        // Not parked yet: it re-checks the (already bumped) signal epoch on
        // its own dispatch pass before it parks, so there is nothing to wake.
        return;
      }
      Cpu& cpu = cpus_[links.cpu];
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running && ClampPriority(sole_waiter->priority()) >
                         ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
      wake[links.cpu] = true;
    } else {
      // Wake the CPUs hosting a waiter whose wait includes this object. The
      // walk is bounded by the blocked population, which the empty-yield fast
      // path keeps small.
      for (int i = 0; i < kMaxCpus; ++i) {
        Cpu& cpu = cpus_[i];
        if (!cpu.blocked_head) {
          continue;
        }
        bool any_watcher = false;
        int watcher_prio = 0;
        for (XThread* t = cpu.blocked_head; t;
             t = t->scheduler_links().ready_next) {
          auto& links = t->scheduler_links();
          bool watches = t->cooperative_wait_object() == object;
          if (!watches) {
            for (uint8_t j = 0; j < links.wait_gate_count; ++j) {
              if (links.wait_gate_objects[j] == object) {
                watches = true;
                break;
              }
            }
          }
          if (watches) {
            int prio = ClampPriority(t->priority());
            watcher_prio = any_watcher ? std::max(watcher_prio, prio) : prio;
            any_watcher = true;
          }
        }
        if (!any_watcher) {
          continue;
        }
        cpu.repoll_now.store(true, std::memory_order_relaxed);
        XThread* running = cpu.current_thread;
        if (running && watcher_prio > ClampPriority(running->priority())) {
          running->scheduler_links().preempted = true;
          running->thread_state()->context()->preempt_requested = 1;
        }
        wake[i] = true;
      }
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (wake[i] && cpus_[i].parked.load() && cpus_[i].ready_event) {
      WakeCpu(cpus_[i]);
    }
  }
}

void GuestScheduler::NotifyThreadExited(XThread* thread) {
  if (!OnDispatchThread("NotifyThreadExited")) {
    return;
  }
  XELOGI("GuestScheduler: exited tid={:08X} '{}'", thread->thread_id(),
         thread->thread_name());
  // This CPU's dispatch loop reclaims it, since we can't drop the last handle
  // while running on its fiber.
  cpus_[t_current_cpu].exited_thread = thread;
}

void GuestScheduler::BlockCurrentThread(uint64_t deadline_ms,
                                        uint32_t wait_epoch, bool alertable,
                                        bool interruptible) {
  if (!OnDispatchThread("BlockCurrentThread")) {
    return;
  }
  if (interruptible) {
    ExitIfTerminated();
  }
  XThread* self = XThread::GetCurrentThread();
  int cpu_index = t_current_cpu;
  // Gate only types whose every satisfying transition calls
  // WakeCooperativeWaiters, anything else polls every pass.
  XObject* wait_object = self->cooperative_wait_object();
  bool gated = false;
  if (wait_object) {
    switch (wait_object->type()) {
      case XObject::Type::Event:
      case XObject::Type::Semaphore:
      case XObject::Type::Mutant:
        gated = true;
        break;
      default:
        break;
    }
  } else if (self->cooperative_wait_set_count()) {
    // Multi-wait: gated on the summed epoch of its whole set. Without this it
    // re-readied on every pass, which is most of the scheduler's churn in
    // titles that park worker pools on WaitForMultipleObjects.
    gated = true;
  } else if (deadline_ms && !alertable &&
             static_cast<XThread::CooperativeWaitKind>(
                 self->scheduler_links().wait_kind) ==
                 XThread::CooperativeWaitKind::kDelay) {
    // Pure timed sleep: only the clock can end it, so park until the deadline
    // instead of waking every kPollBackoffMs. Keyed to the delay wait kind -
    // a timed wait whose objects could not be tracked (a 9+ object
    // WaitMultiple) also arrives here objectless with a deadline, and gating
    // that would leave its signals unseen until the deadline or backstop.
    gated = true;
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    // A signal between the caller's failed poll and this park bumped the epoch
    // but walked the blocked list before we joined it, so nothing would re-poll
    // us before the backstop. Compare under the walk's own lock and poke this
    // CPU on a mismatch; returning to re-poll instead livelocks when the epoch
    // churns faster than a park roundtrip.
    if (gated) {
      uint32_t epoch_now = 0;
      bool epoch_gated = false;
      if (wait_object) {
        epoch_now = wait_object->cooperative_signal_epoch();
        epoch_gated = true;
      } else if (self->cooperative_wait_set_count()) {
        epoch_now = self->cooperative_wait_set_epoch();
        epoch_gated = true;
      }
      if (epoch_gated && epoch_now != wait_epoch) {
        cpus_[cpu_index].repoll_now.store(true, std::memory_order_relaxed);
      }
    }
    auto& links = self->scheduler_links();
    // Park self (running, in no list) on this CPU's blocked list.
    links.blocked = true;
    links.preempted = false;
    links.cpu = cpu_index;
    links.ready_next = nullptr;
    links.wait_gated = gated;
    links.wait_alertable = alertable;
    links.wait_epoch = wait_epoch;
    links.wait_deadline_ms = deadline_ms;
    // A wait consumes the slice.
    links.quantum_deadline_tick = 0;
    Cpu& cpu = cpus_[cpu_index];
    LinkTailLocked(cpu.blocked_head, cpu.blocked_tail, self);
    int prio = ClampPriority(self->priority());
    if (prio > cpu.max_blocked_prio) {
      cpu.max_blocked_prio = prio;
    }
    // Timed need of this waiter: the poll cadence for ungated and alertable
    // waits, a gated deadline, nothing for a quiet gated wait.
    uint64_t due = gated ? deadline_ms : 0;
    if (!gated || alertable) {
      uint64_t cadence = Clock::QueryHostUptimeMillis() + kPollBackoffMs;
      if (!due || cadence < due) {
        due = cadence;
      }
    }
    if (due && due < cpu.next_timed_repoll_ms) {
      cpu.next_timed_repoll_ms = due;
    }
    // seq_cst pairs with the wake pre-filters' loads: epoch-read-then-store
    // here vs bump-then-load there, so at least one side always sees the other.
    cpu.has_blocked.store(true);
  }
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_WAITING;
  YieldToScheduler();
  // Terminated while parked, TerminateThread re-readied us to exit here.
  if (interruptible) {
    ExitIfTerminated();
  }
}

void GuestScheduler::RereadyBlocked(int cpu_index, bool poked) {
  uint32_t wake_mask = 0;
  stats_.repolls.fetch_add(1, std::memory_order_relaxed);
  // Missed-wake tripwire, logged after the lock is released.
  uint64_t rescued = 0;
  bool warn = false;
  uint64_t warn_count = 0;
  uint32_t warn_tid = 0;
  uint32_t warn_handle = 0;
  {
    std::lock_guard<std::mutex> lock(lock_);
    Cpu& cpu = cpus_[cpu_index];
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    bool force_all = now_ms >= cpu.next_force_repoll_ms;
    if (force_all) {
      cpu.next_force_repoll_ms = now_ms + kRepollBackstopMs;
    }
    // Earliest timed need among the waiters kept parked, the backstop bounds
    // it. Waiters re-readied below re-park through BlockCurrentThread, which
    // lowers it again before this CPU can sleep.
    uint64_t next_due = cpu.next_force_repoll_ms;
    XThread* kept_head = nullptr;
    XThread* kept_tail = nullptr;
    int kept_max_prio = -1;
    XThread* t = cpu.blocked_head;
    while (t) {
      auto& links = t->scheduler_links();
      XThread* next = links.ready_next;
      // Skip a gated waiter whose wait cannot have resolved yet.
      if (links.wait_gated && !force_all) {
        // Three gate kinds: a single object's epoch, a multi-wait's summed
        // epoch, or (neither) a pure timed sleep that only the deadline below
        // can resolve.
        XObject* obj = t->cooperative_wait_object();
        bool may_have_resolved = false;
        if (obj) {
          may_have_resolved =
              obj->cooperative_signal_epoch() != links.wait_epoch;
        } else if (links.wait_gate_count) {
          may_have_resolved = t->cooperative_wait_set_epoch() != links.wait_epoch;
        }
        if (!may_have_resolved &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms) &&
            !(links.wait_alertable && t->HasPendingUserApc())) {
          links.ready_next = nullptr;
          LinkTailLocked(kept_head, kept_tail, t);
          int prio = ClampPriority(t->priority());
          if (prio > kept_max_prio) {
            kept_max_prio = prio;
          }
          if (links.wait_deadline_ms && links.wait_deadline_ms < next_due) {
            next_due = links.wait_deadline_ms;
          }
          if (links.wait_alertable && now_ms + kPollBackoffMs < next_due) {
            // APCs inserted without a WakeAll are only found by polling.
            next_due = now_ms + kPollBackoffMs;
          }
          t = next;
          continue;
        }
      }
      // Only the backstop reached this waiter, on a pass no wake poked, yet
      // its gate's epoch moved: the signal's own wake never re-readied it.
      if (force_all && !poked && links.wait_gated) {
        XObject* obj = t->cooperative_wait_object();
        bool epoch_moved = false;
        if (obj) {
          epoch_moved = obj->cooperative_signal_epoch() != links.wait_epoch;
        } else if (links.wait_gate_count) {
          epoch_moved =
              t->cooperative_wait_set_epoch() != links.wait_epoch;
        }
        if (epoch_moved &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms)) {
          ++rescued;
          backstop_rescue_tid_ = t->thread_id();
          backstop_rescue_handle_ =
              links.wait_handle_count ? links.wait_handles[0] : 0;
        }
      }
      links.blocked = false;
      links.queued = true;
      stats_.rereadied.fetch_add(1, std::memory_order_relaxed);
      // Its current guest CPU, not the one it blocked on, since
      // KeSetAffinityThread may have moved it while blocked.
      int target = CpuOf(t);
      links.cpu = target;
      bool at_head = links.preempted;
      links.preempted = false;
      LinkReadyLocked(cpus_[target], t, at_head);
      if (target != cpu_index) {
        wake_mask |= uint32_t(1) << target;
      }
      t = next;
    }
    cpu.blocked_head = kept_head;
    cpu.blocked_tail = kept_tail;
    cpu.max_blocked_prio = kept_max_prio;
    cpu.next_timed_repoll_ms = next_due;
    cpu.has_blocked.store(kept_head != nullptr, std::memory_order_relaxed);
    if (rescued) {
      stats_.backstop_rescues.fetch_add(rescued, std::memory_order_relaxed);
      backstop_rescue_count_ += rescued;
    }
    if (!backstop_rescue_window_ms_) {
      backstop_rescue_window_ms_ = now_ms;
    }
    if (now_ms - backstop_rescue_window_ms_ >= 5000) {
      if (backstop_rescue_count_ >= kBackstopRescueWarnThreshold) {
        warn = true;
        warn_count = backstop_rescue_count_;
        warn_tid = backstop_rescue_tid_;
        warn_handle = backstop_rescue_handle_;
      }
      backstop_rescue_window_ms_ = now_ms;
      backstop_rescue_count_ = 0;
    }
  }
  if (warn) {
    XELOGW(
        "GuestScheduler: {} gated waits were only re-readied by the {}ms "
        "backstop in the last 5s despite a signal having arrived - wakes are "
        "being lost (last: tid={:08X} handle={:08X})",
        warn_count, kRepollBackstopMs, warn_tid, warn_handle);
  }
  // Wake any other dispatch thread that received a ready fiber (this one runs).
  for (int i = 0; i < kMaxCpus; ++i) {
    if ((wake_mask & (uint32_t(1) << i)) && cpus_[i].parked.load() &&
        cpus_[i].ready_event) {
      WakeCpu(cpus_[i]);
    }
  }
}

void GuestScheduler::RunLoop(int cpu_index) {
  t_current_cpu = cpu_index;
  Cpu& cpu = cpus_[cpu_index];
#if defined(__linux__)
  cpu.host_tid.store(int32_t(syscall(SYS_gettid)), std::memory_order_relaxed);
  if (cvars::guest_threads_critical_mask & (uint32_t(1) << cpu_index)) {
    const int before = getpriority(PRIO_PROCESS, 0);
    const int requested = std::min(before, -10);
    errno = 0;
    const int result = setpriority(PRIO_PROCESS, 0, requested);
    const int error = result == -1 ? errno : 0;
    XELOGI("GuestNice: cpu={} before={} requested={} result={} errno={} now={}",
           cpu_index, before, requested, result, error,
           getpriority(PRIO_PROCESS, 0));
  }
#endif
  // Adopt this host thread's stack as this CPU's idle fiber.
  cpu.idle_fiber = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: CPU {} dispatch loop started", cpu_index);

  while (!shutting_down_.load()) {
    if (cpu_index == 0) {
      ReportStatsIfDue();
    }
    // Re-poll blocked waiters on a timer even while other fibers run, or a
    // busy fiber that rarely waits would starve them. The timer runs at what
    // the parked waiters actually need, a wake skips it entirely.
    uint64_t now = Clock::QueryHostUptimeMillis();
    bool poked = cpu.repoll_now.exchange(false, std::memory_order_relaxed);
    // While a snapshot barrier is up, blocked waiters stay blocked: a
    // successful re-poll would run guest code and mutate the state being
    // captured. EndSnapshot re-pokes every CPU.
    if ((poked || now >= cpu.next_timed_repoll_ms) &&
        !snapshot_active_.load(std::memory_order_acquire)) {
      RereadyBlocked(cpu_index, poked);
    }

    XThread* next = DequeueReady(cpu_index);
    if (next) {
      // Honor an affinity change that landed while it was queued or running
      // here. Safe now, an off-CPU thread's context is saved.
      int home = CpuOf(next);
      auto& links = next->scheduler_links();
      if (home != cpu_index &&
          !links.terminate_pending.load(std::memory_order_relaxed)) {
        {
          std::lock_guard<std::mutex> lock(lock_);
          links.running = false;
          links.queued = true;
          links.cpu = home;
          bool at_head = links.preempted;
          links.preempted = false;
          LinkReadyLocked(cpus_[home], next, at_head);
        }
        if (cpus_[home].parked.load() && cpus_[home].ready_event) {
          WakeCpu(cpus_[home]);
        }
        XELOGD("GuestScheduler: migrated tid={:08X} to CPU {}",
               next->thread_id(), home);
        continue;
      }
      // A suspend landing while the thread ran or was queued takes effect here.
      if (next->suspend_count() > 0 && ParkSuspended(next, cpu_index)) {
        continue;
      }
      cpu.exited_thread = nullptr;
      SwitchTo(next);
      if (cpu.exited_thread) {
        // On the idle fiber with the exited fiber parked on its final yield, so
        // reclaiming never frees a stack still in use.
        XThread* dead = cpu.exited_thread;
        cpu.exited_thread = nullptr;
        dead->ReclaimExited();
      }
      continue;
    }

    // Nothing ready. While the game is short of its frame rate, how fast work
    // handed to this CPU starts is frame time: look out for it a little longer
    // before sleeping (see guest_scheduler_idle_spin_us). Not parked, so the
    // thread that hands the work over skips the wake-up call too.
    if (const int32_t idle_spin_us = cvars::guest_scheduler_idle_spin_us;
        idle_spin_us > 0 && dispatched_any_.load(std::memory_order_relaxed) &&
        !xe::cpu::backend::thermal_hot.load(std::memory_order_relaxed) &&
        !snapshot_active_.load(std::memory_order_relaxed) &&
        (now >= xe::cpu::backend::frame_slack_until_ms.load(
                    std::memory_order_relaxed) ||
         now < xe::cpu::backend::guest_io_burst_until_ms.load(
                   std::memory_order_relaxed))) {
      // Spins that don't pay off are spaced out: one attempt in 16 parks.
      if (cpu.idle_spin_score || ++cpu.idle_spin_skipped >= 16) {
        cpu.idle_spin_skipped = 0;
        GuestSyncStats::Add(GuestSyncStats::kIdleSpin);
        const uint64_t spin_end =
            Clock::host_tick_count_raw() +
            uint64_t(double(idle_spin_us) * ticks_per_us_);
        bool found = false;
        do {
          if (cpu.ready_summary.load(std::memory_order_relaxed) != 0 ||
              cpu.repoll_now.load(std::memory_order_relaxed)) {
            found = true;
            break;
          }
#if XE_ARCH_ARM64
          for (uint32_t n = 0; n < 4; ++n) {
            __asm__ __volatile__("isb sy" ::: "memory");
          }
#endif
        } while (Clock::host_tick_count_raw() < spin_end &&
                 !shutting_down_.load(std::memory_order_relaxed));
        if (found) {
          GuestSyncStats::Add(GuestSyncStats::kIdleSpinHit);
          cpu.idle_spin_score = std::min(cpu.idle_spin_score + 1, uint32_t(8));
          continue;
        }
        if (cpu.idle_spin_score) {
          --cpu.idle_spin_score;
        }
      }
    }

    // Nothing ready, so sleep until the next re-poll if waiters are blocked (a
    // MarkReady wakes us sooner), otherwise idle until something is runnable.
    // Park before re-checking the queues, so a wake that saw parked still
    // false is caught here instead of slept through.
    cpu.parked.store(true);
    bool have_blocked;
    bool have_work;
    {
      std::lock_guard<std::mutex> lock(lock_);
      have_blocked = cpu.blocked_head != nullptr;
      have_work = cpu.ready_summary.load(std::memory_order_relaxed) != 0 ||
                  cpu.repoll_now.load(std::memory_order_relaxed);
    }
    if (have_work) {
      cpu.parked.store(false);
      continue;
    }
    if (!have_blocked) {
      if (dispatched_any_.load()) {
        xe::threading::Wait(cpu.ready_event.get(), false);
        cpu.parked.store(false);
        continue;
      }
      // Nothing has ever run, so poll instead of sleeping forever and say so.
      xe::threading::Wait(cpu.ready_event.get(), false,
                          std::chrono::seconds(1));
      cpu.parked.store(false);
      bool warned = false;
      if (!dispatched_any_.load() &&
          never_dispatched_warned_.compare_exchange_strong(warned, true)) {
        XELOGW(
            "GuestScheduler: no guest fiber dispatched after 1s, every guest "
            "thread is unqueued (created suspended and never resumed?)");
      }
      continue;
    }
    now = Clock::QueryHostUptimeMillis();
    uint64_t due = cpu.next_timed_repoll_ms;
    uint64_t sleep_ms = due > now ? due - now : 0;
    if (snapshot_active_.load(std::memory_order_acquire) && sleep_ms < 5) {
      sleep_ms = 5;  // Re-polls are suspended; do not spin on a past due time.
    }
    xe::threading::Wait(cpu.ready_event.get(), false,
                        std::chrono::milliseconds(sleep_ms));
    cpu.parked.store(false);
    stats_.idle_wakes.fetch_add(1, std::memory_order_relaxed);
  }
  XELOGI("GuestScheduler: CPU {} dispatch loop exited (shutting_down={})",
         cpu_index, shutting_down_.load());
}


// Wait shapes whose blocked fiber can be recreated by re-running the wait.
// Whether the specific kernel call is restartable is checked by the caller.
static bool SnapshotWaitKindSupported(uint8_t kind) {
  switch (static_cast<XThread::CooperativeWaitKind>(kind)) {
    case XThread::CooperativeWaitKind::kSingle:
    case XThread::CooperativeWaitKind::kMultiAny:
    case XThread::CooperativeWaitKind::kMultiAll:
    case XThread::CooperativeWaitKind::kDelay:
      return true;
    default:
      return false;
  }
}

bool GuestScheduler::BeginSnapshot(std::chrono::milliseconds timeout,
                                   bool allow_blocked, std::string* error) {
  auto fail = [error](std::string message) {
    if (error) {
      *error = std::move(message);
    }
    return false;
  };
  if (!enabled() || !started_.load() || stopped_.load()) {
    return fail("the cooperative guest scheduler is not running");
  }
  if (timeout.count() <= 0) {
    return fail("invalid snapshot barrier timeout");
  }
  if (XThread::GetCurrentFiberThread()) {
    return fail("the snapshot barrier must be requested from a host thread");
  }
  auto threads = kernel_state_->object_table()->GetObjectsByType<XThread>();
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (snapshot_active_.load()) {
      return fail("a snapshot barrier is already active");
    }
    for (auto& ref : threads) {
      auto* thread = ref.get();
      if (!thread->is_guest_thread()) continue;
      auto& links = thread->scheduler_links();
      // A native frame cannot be recreated from PPC context alone.
      if (!thread->fiber()) {
        return fail(fmt::format("guest thread {:08X} has no cooperative fiber",
                                thread->thread_id()));
      }
      if (links.suspended || thread->suspend_count()) {
        return fail(fmt::format("guest thread {:08X} '{}' is suspended",
                                thread->thread_id(), thread->thread_name()));
      }
      if (links.terminate_pending.load()) {
        return fail(fmt::format("guest thread {:08X} is terminating",
                                thread->thread_id()));
      }
      if (links.blocked) {
        // An offloaded file read completes within milliseconds: the parking
        // loop below waits for it (it keeps the thread pending until the I/O
        // ends) instead of failing a save of a streaming title (Halo Reach).
        const bool transient =
            static_cast<XThread::CooperativeWaitKind>(links.wait_kind) ==
            XThread::CooperativeWaitKind::kIoOffload;
        if (!allow_blocked ||
            (!transient && !SnapshotWaitKindSupported(links.wait_kind))) {
          return fail(fmt::format(
              "guest thread {:08X} '{}' is blocked in an unsupported wait "
              "(kind {})",
              thread->thread_id(), thread->thread_name(),
              uint32_t(links.wait_kind)));
        }
      } else if (!links.running && !links.queued) {
        return fail(fmt::format("guest thread {:08X} '{}' is not schedulable",
                                thread->thread_id(), thread->thread_name()));
      }
    }
    for (auto& ref : threads) {
      auto* thread = ref.get();
      if (!thread->is_guest_thread()) continue;
      thread->Retain();
      snapshot_threads_.push_back(thread);
      thread->scheduler_links().snapshot_pc = 0;
    }
    if (snapshot_threads_.empty()) {
      return fail("no guest thread is alive");
    }
    snapshot_active_.store(true, std::memory_order_release);
    for (auto* thread : snapshot_threads_) {
      if (!thread->scheduler_links().blocked) {
        thread->thread_state()->context()->preempt_requested = 1;
      }
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string pending;
  while (std::chrono::steady_clock::now() < deadline) {
    bool all = true;
    pending.clear();
    {
      std::lock_guard<std::mutex> guard(lock_);
      for (auto* thread : snapshot_threads_) {
        auto& links = thread->scheduler_links();
        if (links.running) {
          all = false;
          thread->thread_state()->context()->preempt_requested = 1;
        } else if (links.snapshot_parked) {
          continue;
        } else if (links.blocked) {
          if (!SnapshotWaitKindSupported(links.wait_kind)) {
            pending = fmt::format(
                "guest thread {:08X} '{}' blocked in an unsupported wait "
                "(kind {})",
                thread->thread_id(), thread->thread_name(),
                uint32_t(links.wait_kind));
            all = false;
            break;
          }
        } else {
          all = false;
          thread->thread_state()->context()->preempt_requested = 1;
        }
        if (!all && pending.empty()) {
          pending = fmt::format(
              "guest thread {:08X} '{}' did not reach a safepoint "
              "(last_safepoint_pc={:08X})",
              thread->thread_id(), thread->thread_name(),
              uint32_t(thread->thread_state()->context()->last_safepoint_pc));
        }
      }
    }
    if (all) {
      // A host callback may create a new guest thread during the drain.
      // Reject an inventory change instead of claiming untracked writers are
      // frozen.
      auto current = kernel_state_->object_table()->GetObjectsByType<XThread>();
      size_t guest_count = 0;
      bool unknown = false;
      for (auto& ref : current) {
        if (!ref->is_guest_thread()) continue;
        ++guest_count;
        if (std::find(snapshot_threads_.begin(), snapshot_threads_.end(),
                      ref.get()) == snapshot_threads_.end()) {
          unknown = true;
          break;
        }
      }
      if (!unknown && guest_count == snapshot_threads_.size()) {
        return true;
      }
      EndSnapshot();
      return fail("the guest thread inventory changed while parking");
    }
    for (auto& cpu : cpus_) {
      if (cpu.ready_event) WakeCpu(cpu);
    }
    xe::threading::Sleep(std::chrono::milliseconds(1));
  }
  EndSnapshot();
  return fail(pending.empty() ? "timed out parking the guest threads"
                              : "timed out: " + pending);
}

std::vector<XThread*> GuestScheduler::SnapshotThreads() {
  std::lock_guard<std::mutex> guard(lock_);
  return snapshot_active_.load() ? snapshot_threads_ : std::vector<XThread*>();
}

bool GuestScheduler::ParkForSnapshot(XThread* thread) {
  if (!snapshot_active_.load(std::memory_order_acquire)) return false;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!snapshot_active_.load()) return false;
    if (std::find(snapshot_threads_.begin(), snapshot_threads_.end(), thread) ==
        snapshot_threads_.end()) return false;
    auto& links = thread->scheduler_links();
    const uint32_t pc = thread->thread_state()->context()->last_safepoint_pc;
    // pc is only meaningful with the JIT cvar log_safepoint_pc (savestate
    // forces it on); without it the thread keeps running and the barrier
    // times out with a clear message.
    if (!pc || (pc & 3) || !links.running || links.terminate_pending.load()) {
      return false;
    }
    links.snapshot_pc = pc;
    links.snapshot_parked = true;
  }
  // No ready/blocked list owns this fiber until EndSnapshot. SwitchTo clears
  // links.running only once its native register save completed on the idle
  // fiber.
  YieldToScheduler();
  ExitIfTerminated();
  return true;
}

void GuestScheduler::EndSnapshot(bool requeue) {
  std::vector<XThread*> release;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!snapshot_active_.exchange(false)) return;
    release.swap(snapshot_threads_);
    for (auto* thread : release) {
      auto& links = thread->scheduler_links();
      if (!links.snapshot_parked) continue;
      links.snapshot_parked = false;
      links.snapshot_pc = 0;
      if (!requeue) continue;
      // Always requeue on the same CPU: the native fiber is pinned there.
      links.queued = true;
      LinkReadyLocked(cpus_[links.cpu], thread, true);
    }
    // Blocked waiters were not re-polled during the barrier.
    for (auto& cpu : cpus_) {
      cpu.next_timed_repoll_ms = 0;
      cpu.repoll_now.store(true, std::memory_order_relaxed);
    }
  }
  for (auto& cpu : cpus_) if (cpu.ready_event) WakeCpu(cpu);
  for (auto* thread : release) thread->Release();
}

void GuestScheduler::NoteForcedPreempt() {
  stats_.forced_preempts.fetch_add(1, std::memory_order_relaxed);
}

void GuestScheduler::ReportStatsIfDue() {
  if (!cvars::guest_scheduler_stats) {
    return;
  }
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (now_ms - stats_last_report_ms_ < 1000) {
    return;
  }
  stats_last_report_ms_ = now_ms;
  auto take = [](std::atomic<uint64_t>& v) {
    return v.exchange(0, std::memory_order_relaxed);
  };
  uint64_t repolls = take(stats_.repolls);
  uint64_t rereadied = take(stats_.rereadied);
  uint64_t backstop_rescues = take(stats_.backstop_rescues);
  uint64_t idle_wakes = take(stats_.idle_wakes);
  uint64_t switches = take(stats_.switches);
  uint64_t forced = take(stats_.forced_preempts);
  uint64_t io_calls = take(stats_.io_calls);
  uint64_t io_queue = take(stats_.io_queue_ns);
  uint64_t io_run = take(stats_.io_run_ns);
  uint64_t io_queue_max = take(stats_.io_queue_max_ns);
  double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  auto to_us = [ticks_per_us](uint64_t ticks) {
    return uint64_t(double(ticks) / ticks_per_us);
  };
  XELOGI(
      "GuestScheduler: repolls {}/s (rereadied {}, backstop rescues {}), idle "
      "wakes {}, switches "
      "{}, forced preempts {} | io {} calls, queued avg {} us max {} us, ran "
      "avg {} us",
      repolls, rereadied, backstop_rescues, idle_wakes, switches, forced,
      io_calls,
      io_calls ? to_us(io_queue / io_calls) : 0, to_us(io_queue_max),
      io_calls ? to_us(io_run / io_calls) : 0);
}

// Names what a fiber is parked on, for the no-progress dump.
static const char* WaitObjectKind(XObject* object) {
  if (!object) {
    return "none";
  }
  switch (object->type()) {
    case XObject::Type::Event:
      return "event";
    case XObject::Type::Semaphore:
      return "semaphore";
    case XObject::Type::Mutant:
      return "mutant";
    case XObject::Type::Thread:
      return "thread";
    case XObject::Type::Timer:
      return "timer";
    default:
      return "other";
  }
}

namespace {
const char* CooperativeWaitKindName(uint8_t kind) {
  switch (static_cast<XThread::CooperativeWaitKind>(kind)) {
    case XThread::CooperativeWaitKind::kSingle:
      return "single";
    case XThread::CooperativeWaitKind::kMultiAny:
      return "multi-any";
    case XThread::CooperativeWaitKind::kMultiAll:
      return "multi-all";
    case XThread::CooperativeWaitKind::kDelay:
      return "delay";
    case XThread::CooperativeWaitKind::kFence:
      return "fence";
    case XThread::CooperativeWaitKind::kIoOffload:
      return "io-offload";
    default:
      return "none";
  }
}

// "multi-any[4] handles=F8000030,F8000034,..." - the handles cross-reference
// against the signal ring dumped alongside.
std::string FormatWaitShape(const XThread::SchedulerLinks& links) {
  std::string out = CooperativeWaitKindName(links.wait_kind);
  if (!links.wait_handle_count) {
    return out;
  }
  out += fmt::format("[{}] handles=", links.wait_handle_count);
  uint32_t shown = links.wait_handle_count < 8 ? links.wait_handle_count : 8;
  for (uint32_t i = 0; i < shown; ++i) {
    out += fmt::format("{}{:08X}", i ? "," : "", links.wait_handles[i]);
  }
  if (links.wait_handle_count > shown) {
    out += ",...";
  }
  return out;
}
}  // namespace

void GuestScheduler::ReportNoProgress() {
  XELOGW(
      "GuestScheduler: no guest frame presented in {} watchdog ticks while the "
      "dispatch threads keep switching. Every fiber below is waiting on "
      "something none of them is producing:",
      no_progress_ticks_);
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  std::lock_guard<std::mutex> lock(lock_);
  for (int i = 0; i < kMaxCpus; ++i) {
    Cpu& cpu = cpus_[i];
    if (XThread* running = cpu.current_thread) {
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "  CPU {} running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} ready_summary={:#x}",
          i, running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql),
          uint32_t(context->preempt_requested),
          cpu.ready_summary.load(std::memory_order_relaxed));
    } else {
      XELOGW("  CPU {} idle, ready_summary={:#x}", i,
             cpu.ready_summary.load(std::memory_order_relaxed));
    }
    // Parked fibers are the interesting half: the cycle is whatever they are
    // all waiting for.
    int listed = 0;
    for (XThread* t = cpu.blocked_head; t && listed < 8;
         t = t->scheduler_links().ready_next, ++listed) {
      auto& links = t->scheduler_links();
      XObject* obj = t->cooperative_wait_object();
      auto* context = t->thread_state()->context();
      int64_t due_in =
          links.wait_deadline_ms
              ? int64_t(links.wait_deadline_ms) - int64_t(now_ms)
              : -1;
      XELOGW(
          "    blocked tid={:08X} '{}' last_safepoint={:08X} lr={:08X} on {} "
          "obj={} wait={} gated={} alertable={} epoch={} deadline_in_ms={}",
          t->thread_id(), t->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          WaitObjectKind(obj), static_cast<const void*>(obj),
          FormatWaitShape(links), links.wait_gated ? 1 : 0,
          links.wait_alertable ? 1 : 0, links.wait_epoch, due_in);
    }
    // Ready-but-not-running fibers matter too: a queue that never drains
    // means the CPU is rotating between the same few spinners.
    for (int prio = 31; prio >= 0; --prio) {
      for (XThread* t = cpu.ready_head[prio]; t;
           t = t->scheduler_links().ready_next) {
        auto* context = t->thread_state()->context();
        XELOGW("    ready   tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
               "prio={}",
               t->thread_id(), t->thread_name(),
               uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
               prio);
      }
    }
  }
  // The other half of the picture: what was actually signalled recently. A
  // handle the fibers above wait on that never appears here names the producer
  // that stopped; one that appears repeatedly while a waiter stays parked
  // points at the wake being lost instead of never sent.
  auto signals = XObject::RecentCooperativeSignals(64);
  if (signals.empty()) {
    XELOGW("  recent cooperative signals: none recorded");
    return;
  }
  XELOGW("  last {} cooperative signals (oldest first):", signals.size());
  for (const auto& rec : signals) {
    XELOGW(
        "    #{} handle={:08X} type={} by_tid={:08X} lr={:08X} uptime_ms={}",
        rec.seq, rec.handle, uint32_t(rec.type), rec.signaler_thread,
        rec.signaler_lr, rec.uptime_ms);
  }
}

void GuestScheduler::WatchdogLoop() {
  // Microseconds, not milliseconds: an integer division to ms floors every
  // sub-millisecond quantum to the same 1 ms tick, so the setting would stop
  // meaning anything below 1000. The host still adds wakeup slack, so a short
  // quantum is a target rather than a guarantee.
  uint64_t period_us = cvars::guest_scheduler_quantum_us;
  if (period_us < kMinWatchdogPeriodUs) {
    period_us = kMinWatchdogPeriodUs;
  }
  while (!shutting_down_.load()) {
    if (period_us >= 1000) {
      // Event wait so Shutdown's Set is observed immediately.
      xe::threading::Wait(watchdog_event_.get(), false,
                          std::chrono::milliseconds(period_us / 1000));
    } else {
      // Wait only takes milliseconds, so a sub-millisecond period sleeps
      // instead. Shutdown latency is then bounded by the period itself.
      xe::threading::PreciseSleep(std::chrono::microseconds(period_us));
    }
    if (shutting_down_.load()) {
      break;
    }
    PlacementGovernorTick();
    // No-progress detection, outside lock_ (ReportNoProgress takes it).
    // Only meaningful once something has been dispatched, so a title still
    // loading is not reported.
    uint32_t frame = xe::logging::GetFrameNumber();
    if (frame != last_frame_number_ || !dispatched_any_.load()) {
      last_frame_number_ = frame;
      no_progress_ticks_ = 0;
      no_progress_reported_ = false;
    } else if (++no_progress_ticks_ >= kNoProgressReportTicks &&
               !no_progress_reported_) {
      no_progress_reported_ = true;
      ReportNoProgress();
    }

    uint64_t now = Clock::host_tick_count_raw();
    std::lock_guard<std::mutex> lock(lock_);
    for (int i = 0; i < kMaxCpus; ++i) {
      XThread* running = cpus_[i].current_thread;
      if (running && now >= cpus_[i].quantum_deadline_tick) {
        running->thread_state()->context()->preempt_requested = 1;
      }
      // Stall detector: a dispatch count that has not moved for a whole
      // window separates the wedge modes - flag still set means the fiber
      // never reaches a safepoint, flag cleared means it yields but makes no
      // progress.
      uint64_t seq = cpus_[i].switch_seq.load(std::memory_order_relaxed);
      if (!running || seq != stall_last_seq_[i]) {
        stall_last_seq_[i] = seq;
        stall_ticks_[i] = 0;
        stall_reported_[i] = false;
        continue;
      }
      if (++stall_ticks_[i] < kStallReportTicks || stall_reported_[i]) {
        continue;
      }
      stall_reported_[i] = true;
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "GuestScheduler: CPU {} has not switched fibers in {} watchdog "
          "ticks. Running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} irql_defers={} lock_defers={} "
          "ready_summary={:#x}",
          i, stall_ticks_[i], running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql),
          uint32_t(context->preempt_requested),
          running->scheduler_links().preempt_defers_irql,
          running->scheduler_links().preempt_defers_lock,
          cpus_[i].ready_summary.load(std::memory_order_relaxed));
    }
  }
}

#if defined(__linux__)
namespace {

// The host's cores by clock: the lower-clocked cluster(s) and everything.
struct HostCpuTopology {
  bool has_slow_cores = false;
  // Capacity of the slow cores is known and large enough to run guest code.
  bool capacity_ok = false;
  bool capacity_known = false;
  cpu_set_t slow_set;
  cpu_set_t all_set;
  int cpu_count = 0;
  int slow_count = 0;
  // Speed of the slowest slow core relative to the fastest core, in percent.
  uint32_t slow_speed_percent = 100;
};

uint32_t ReadCpuSysfsValue(int cpu, const char* leaf) {
  char path[96];
  std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu,
                leaf);
  FILE* file = std::fopen(path, "r");
  if (!file) {
    return 0;
  }
  unsigned int value = 0;
  if (std::fscanf(file, "%u", &value) != 1) {
    value = 0;
  }
  std::fclose(file);
  return value;
}

const HostCpuTopology& GetHostCpuTopology() {
  static const HostCpuTopology topology = [] {
    HostCpuTopology t;
    CPU_ZERO(&t.slow_set);
    CPU_ZERO(&t.all_set);
    uint32_t max_freq[64] = {};
    uint32_t capacity[64] = {};
    uint32_t top_freq = 0, top_capacity = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
      max_freq[cpu] = ReadCpuSysfsValue(cpu, "cpufreq/cpuinfo_max_freq");
      if (!max_freq[cpu]) {
        break;
      }
      capacity[cpu] = ReadCpuSysfsValue(cpu, "cpu_capacity");
      top_freq = std::max(top_freq, max_freq[cpu]);
      top_capacity = std::max(top_capacity, capacity[cpu]);
      t.cpu_count = cpu + 1;
      CPU_SET(cpu, &t.all_set);
    }
    uint32_t slow_min_capacity = UINT32_MAX;
    uint32_t slow_min_freq = UINT32_MAX;
    bool capacity_known = top_capacity != 0;
    for (int cpu = 0; cpu < t.cpu_count; ++cpu) {
      // Below 85% of the fastest clock.
      if (uint64_t(max_freq[cpu]) * 100 < uint64_t(top_freq) * 85) {
        CPU_SET(cpu, &t.slow_set);
        ++t.slow_count;
        slow_min_freq = std::min(slow_min_freq, max_freq[cpu]);
        if (capacity[cpu]) {
          slow_min_capacity = std::min(slow_min_capacity, capacity[cpu]);
        } else {
          capacity_known = false;
        }
      }
    }
    t.has_slow_cores = t.slow_count >= 2 && t.slow_count < t.cpu_count;
    t.capacity_known = capacity_known && t.has_slow_cores;
    if (t.has_slow_cores) {
      t.slow_speed_percent =
          t.capacity_known
              ? uint32_t(uint64_t(slow_min_capacity) * 100 / top_capacity)
              : uint32_t(uint64_t(slow_min_freq) * 100 / top_freq);
      t.slow_speed_percent = std::clamp(t.slow_speed_percent, 10u, 100u);
      t.capacity_ok =
          t.capacity_known &&
          t.slow_speed_percent >=
              uint32_t(std::max(
                  cvars::guest_threads_placement_min_capacity_percent, 1));
    }
    XELOGI(
        "ThreadPlacement: host cpus={} slow={} (speed {}% of the fastest, "
        "capacity {}) usable={}",
        t.cpu_count, t.slow_count, t.slow_speed_percent,
        t.capacity_known ? "known" : "unknown", t.capacity_ok);
    return t;
  }();
  return topology;
}

// CPU time used so far by a thread of this process, 0 if it can't be read.
uint64_t ThreadCpuTimeNs(int32_t tid) {
  if (tid <= 0) {
    return 0;
  }
  // The kernel's per-thread CPU clock id (MAKE_THREAD_CPUCLOCK(tid,
  // CPUCLOCK_SCHED)), as pthread_getcpuclockid builds it.
  const clockid_t clock = clockid_t((~uint32_t(tid) << 3) | 2u | 4u);
  timespec time = {};
  if (clock_gettime(clock, &time)) {
    return 0;
  }
  return uint64_t(time.tv_sec) * UINT64_C(1000000000) + uint64_t(time.tv_nsec);
}

// sched_setattr with a utilization clamp, as an app may or may not be allowed
// to: the syscall can be filtered (seccomp raises SIGSYS, fatal by default),
// so the one-time probe runs it under a handler that turns the trap into
// ENOSYS.
struct HostSchedAttr {
  uint32_t size;
  uint32_t sched_policy;
  uint64_t sched_flags;
  int32_t sched_nice;
  uint32_t sched_priority;
  uint64_t sched_runtime;
  uint64_t sched_deadline;
  uint64_t sched_period;
  uint32_t sched_util_min;
  uint32_t sched_util_max;
};
constexpr uint64_t kSchedFlagKeepAll = 0x08 | 0x10;
constexpr uint64_t kSchedFlagUtilClampMax = 0x40;
#ifndef __NR_sched_setattr
#define __NR_sched_setattr 274
#endif
#ifndef __NR_sched_getattr
#define __NR_sched_getattr 275
#endif

std::atomic<int> uclamp_probe_trapped{0};

void UclampProbeSigsys(int /*signal*/, siginfo_t* /*info*/, void* raw) {
  uclamp_probe_trapped.store(1, std::memory_order_relaxed);
#if defined(__aarch64__)
  // Resume after the filtered syscall with an error as its result.
  static_cast<ucontext_t*>(raw)->uc_mcontext.regs[0] = uint64_t(-ENOSYS);
#else
  (void)raw;
#endif
}

// 0 if the calling thread could set its own clamp, else the errno.
int UclampSupportErrno() {
  static const int result = [] {
#if defined(__aarch64__)
    struct sigaction action = {};
    struct sigaction previous = {};
    action.sa_sigaction = &UclampProbeSigsys;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSYS, &action, &previous)) {
      return errno ? errno : EINVAL;
    }
    HostSchedAttr attr = {};
    int error = 0;
    if (syscall(__NR_sched_getattr, 0, &attr, sizeof(attr), 0)) {
      error = errno ? errno : EINVAL;
    }
    if (!error && !uclamp_probe_trapped.load(std::memory_order_relaxed)) {
      HostSchedAttr request = {};
      request.size = sizeof(request);
      request.sched_flags = kSchedFlagKeepAll | kSchedFlagUtilClampMax;
      request.sched_util_max = 1024;
      if (syscall(__NR_sched_setattr, 0, &request, 0)) {
        error = errno ? errno : EINVAL;
      }
    }
    sigaction(SIGSYS, &previous, nullptr);
    if (uclamp_probe_trapped.load(std::memory_order_relaxed)) {
      error = ENOSYS;
    }
    XELOGI("ThreadPlacement: utilization clamp probe: {} (errno {})",
           error ? "not available" : "available", error);
    return error;
#else
    return ENOSYS;
#endif
  }();
  return result;
}

bool SetThreadUclampMax(int32_t tid, uint32_t util_max) {
  if (tid <= 0 || UclampSupportErrno()) {
    return false;
  }
  HostSchedAttr request = {};
  request.size = sizeof(request);
  request.sched_flags = kSchedFlagKeepAll | kSchedFlagUtilClampMax;
  request.sched_util_max = std::min(util_max, uint32_t(1024));
  return !syscall(__NR_sched_setattr, tid, &request, 0);
}

}  // namespace
#endif  // __linux__

void GuestScheduler::ApplyPlacement(uint32_t restricted_mask,
                                    uint32_t clamped_mask,
                                    const char* reason) {
#if defined(__linux__)
  Placement& p = placement_;
  ThreadPlacementPolicy::State& applied = p.policy;
  const HostCpuTopology& topology = GetHostCpuTopology();
  const bool changed = restricted_mask != applied.restricted_mask ||
                       clamped_mask != applied.clamped_mask;
  for (int i = 0; i < kMaxCpus; ++i) {
    const uint32_t bit = uint32_t(1) << i;
    const int32_t tid = cpus_[i].host_tid.load(std::memory_order_relaxed);
    if (tid <= 0) {
      restricted_mask &= ~bit;
      clamped_mask &= ~bit;
      continue;
    }
    const bool restrict = (restricted_mask & bit) != 0;
    // Re-asserted when unchanged too: the kernel resets masks when the app's
    // cpuset changes.
    if (restrict || (applied.restricted_mask & bit)) {
      if (sched_setaffinity(tid, sizeof(cpu_set_t),
                            restrict ? &topology.slow_set
                                     : &topology.all_set)) {
        if (!p.affinity_failed) {
          p.affinity_failed = true;
          XELOGW("ThreadPlacement: sched_setaffinity failed (errno {}), "
                 "placement disabled",
                 errno);
        }
        restricted_mask &= ~bit;
      }
    }
    const bool clamp = restrict && (clamped_mask & bit);
    if (clamp != ((applied.clamped_mask & bit) != 0)) {
      if (!SetThreadUclampMax(
              tid, clamp ? uint32_t(std::clamp(
                               cvars::guest_threads_spin_uclamp_max, 1, 1024))
                         : 1024)) {
        clamped_mask &= ~bit;
      }
    }
  }
  if (changed) {
    XELOGI(
        "ThreadPlacement: slow cores={:#04x} clock capped={:#04x} ({}) | "
        "use% {} {} {} {} {} {} | work% {} {} {} {} {} {}",
        restricted_mask, clamped_mask, reason, p.util_percent[0],
        p.util_percent[1], p.util_percent[2], p.util_percent[3],
        p.util_percent[4], p.util_percent[5], p.work_percent[0],
        p.work_percent[1], p.work_percent[2], p.work_percent[3],
        p.work_percent[4], p.work_percent[5]);
  }
  ThreadPlacementPolicy::NoteApplied(applied, restricted_mask, clamped_mask,
                                     Clock::QueryHostUptimeMillis());
#else
  (void)restricted_mask;
  (void)clamped_mask;
  (void)reason;
#endif
}

void GuestScheduler::PlacementGovernorTick() {
#if defined(__linux__)
  static_assert(ThreadPlacementPolicy::kThreads == kMaxCpus);
  Placement& p = placement_;
  const uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (!p.last_eval_ms) {
    p.last_eval_ms = now_ms;
    if (cvars::log_guest_threads_uclamp_probe) {
      UclampSupportErrno();
    }
    return;
  }
  namespace backend = xe::cpu::backend;
  // A limiter probe can't wait for the next decision: the threads go back at
  // once (the decision after it keeps them there while it lasts).
  if (p.policy.restricted_mask &&
      backend::frame_limiter_probe.load(std::memory_order_relaxed)) {
    ApplyPlacement(0, 0, "limiter probe");
  }
  const uint64_t elapsed_ms = now_ms - p.last_eval_ms;
  if (elapsed_ms < 1000) {
    return;
  }
  p.last_eval_ms = now_ms;

  ThreadPlacementPolicy::Inputs in;
  in.now_ms = now_ms;
  in.elapsed_ms = elapsed_ms;
  // What each dispatch thread used of a core, and how much of that was work.
  for (int i = 0; i < kMaxCpus; ++i) {
    const int32_t tid = cpus_[i].host_tid.load(std::memory_order_relaxed);
    const uint64_t cpu_time_ns = ThreadCpuTimeNs(tid);
    const uint64_t used_ns =
        p.cpu_time_ns[i] && cpu_time_ns > p.cpu_time_ns[i]
            ? cpu_time_ns - p.cpu_time_ns[i]
            : 0;
    p.cpu_time_ns[i] = cpu_time_ns;
    const uint32_t samples =
        cpus_[i].spin_samples.load(std::memory_order_relaxed);
    const uint32_t hits =
        cpus_[i].spin_sample_hits.load(std::memory_order_relaxed);
    const uint32_t window_samples = samples - p.spin_samples[i];
    const uint32_t window_hits = hits - p.spin_sample_hits[i];
    p.spin_samples[i] = samples;
    p.spin_sample_hits[i] = hits;
    const uint32_t util = uint32_t(
        std::min(used_ns / (elapsed_ms * 10000), uint64_t(100)));
    // Too few quantum ends to tell (the thread mostly sleeps): all work.
    const uint32_t spin_percent =
        window_samples >= 20
            ? std::min(window_hits * 100 / window_samples, uint32_t(100))
            : 0;
    p.util_percent[i] = util;
    p.work_percent[i] = util * (100 - spin_percent) / 100;
    in.present[i] = tid > 0;
    in.util_percent[i] = p.util_percent[i];
    in.work_percent[i] = p.work_percent[i];
  }
  const uint32_t comfortable =
      backend::frame_pace_comfortable_count.load(std::memory_order_relaxed);
  const uint32_t tight =
      backend::frame_pace_tight_count.load(std::memory_order_relaxed);
  in.comfortable_frames = comfortable - p.comfortable_frames;
  in.tight_frames = tight - p.tight_frames;
  p.comfortable_frames = comfortable;
  p.tight_frames = tight;

  const int32_t mode = cvars::guest_threads_placement;
  const HostCpuTopology& topology = GetHostCpuTopology();
  if (mode <= 0 || p.affinity_failed || !topology.has_slow_cores ||
      !(topology.capacity_ok || (mode >= 2 && !topology.capacity_known))) {
    if (p.policy.restricted_mask) {
      ApplyPlacement(0, 0, "off");
    }
    p.policy = ThreadPlacementPolicy::State();
    return;
  }

  in.presenter =
      backend::frame_pace_presenter_cpu.load(std::memory_order_relaxed);
  in.hot = backend::thermal_hot.load(std::memory_order_relaxed);
  in.loading = backend::frame_pace_loading.load(std::memory_order_relaxed);
  in.probe = backend::frame_limiter_probe.load(std::memory_order_relaxed);
  in.enter_percent = uint32_t(
      std::clamp(cvars::guest_threads_placement_enter_percent, 0, 100));
  in.leave_percent = uint32_t(
      std::clamp(cvars::guest_threads_placement_leave_percent, 0, 100));
  in.budget_percent = uint32_t(
      std::clamp(cvars::guest_threads_placement_budget_percent, 0, 100));
  in.slow_core_count = uint32_t(topology.slow_count);
  in.slow_speed_percent = topology.slow_speed_percent;
  in.clamp_spinners = cvars::guest_threads_spin_uclamp_max > 0;
  const ThreadPlacementPolicy::Decision decision =
      ThreadPlacementPolicy::Decide(in, p.policy);
  if (decision.apply) {
    ApplyPlacement(decision.restricted_mask, decision.clamped_mask,
                   decision.reason);
  }
#endif
}

}  // namespace kernel
}  // namespace xe
