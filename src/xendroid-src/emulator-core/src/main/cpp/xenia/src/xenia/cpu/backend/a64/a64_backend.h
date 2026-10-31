/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
#define XENIA_CPU_BACKEND_A64_A64_BACKEND_H_

#include <atomic>
#include <memory>

#include "xenia/base/bit_map.h"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/backend.h"

namespace xe {
class Exception;
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache;

typedef void* (*HostToGuestThunk)(void* target, void* arg0, void* arg1);
typedef void* (*GuestToHostThunk)(void* target, void* arg0, void* arg1);
typedef void (*ResolveFunctionThunk)();

// Place guest trampolines in an address range that the HV normally occupies.
static constexpr uint32_t GUEST_TRAMPOLINE_BASE = 0x80000000;
static constexpr uint32_t GUEST_TRAMPOLINE_END = 0x80040000;
static constexpr uint32_t GUEST_TRAMPOLINE_MIN_LEN = 8;
static constexpr uint32_t MAX_GUEST_TRAMPOLINES =
    (GUEST_TRAMPOLINE_END - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;

// Xenon reservation granule is one 128 byte cache line.
static constexpr uint32_t A64_RESERVE_GRANULE_SHIFT = 7;
// A generation counter per granule, hashed. stwcx. bumps its granule to kill
// other threads' reservations. Colliding granules only cost a spurious failure.
static constexpr uint32_t A64_RESERVE_NUM_ENTRIES = 1u << 20;
static constexpr uint32_t A64_RESERVE_ENTRY_MASK = A64_RESERVE_NUM_ENTRIES - 1;

struct ReserveHelper {
  std::atomic<uint32_t> generations[A64_RESERVE_NUM_ENTRIES];

  ReserveHelper() {
    for (auto& generation : generations) {
      generation.store(0, std::memory_order_relaxed);
    }
  }
};

struct A64BackendStackpoint {
  uint64_t host_stack_;
  unsigned guest_stack_;
  unsigned guest_return_address_;
};

// Depth to resume at after a guest longjmp to |guest_sp| into the function
// spanning [target_start, target_end); 0 if no host frames are to be dropped.
uint32_t FindStackpointSyncDepth(const A64BackendStackpoint* stackpoints,
                                 uint32_t current_depth, uint32_t guest_sp,
                                 uint32_t target_start, uint32_t target_end);

enum : uint32_t {
  kA64BackendFPCRModeBit = 0,
  kA64BackendHasReserveBit = 1,
  kA64BackendNJMOn = 2,
  kA64BackendNonIEEEMode = 3,
};

// Located prior to the context register (x20) in memory.
struct A64BackendContext {
  // Scratch vectors for helper routines.
  // Using uint8_t[16] instead of NEON intrinsic types to avoid including
  // arm_neon.h in the header.
  alignas(16) uint8_t helper_scratch_v128s[4][16];
  union {
    uint64_t helper_scratch_u64s[8];
    uint32_t helper_scratch_u32s[16];
  };
  ReserveHelper* reserve_helper_;
  uint64_t cached_reserve_value_;
  uint64_t* guest_tick_count;
  A64BackendStackpoint* stackpoints;
  // address of the live reservation, and its granule generation when taken
  uint32_t reserve_address;
  uint32_t reserve_generation;
  unsigned int current_stackpoint_depth;
  unsigned int pending_stackpoint_sync_depth;
  unsigned int fpcr_fpu;
  unsigned int fpcr_vmx;
  // bit 0 = 0 if fpcr is fpu, else it is vmx
  // bit 1 = got reserve
  unsigned int flags;
  unsigned int Ox1000;  // constant 0x1000
  // Which cached value the host FPCR holds right now for this guest thread:
  // 0 = unknown, 1 = fpcr_fpu, 2 = fpcr_vmx. Every FPCR write in generated
  // code and in the thunks keeps it current, so a mode switch to the mode
  // already in effect skips the (serializing) system register write.
  uint8_t fpcr_live_mode;
  // a64_indirection_bases_in_context: what an indirect guest call adds to the
  // guest address to reach its indirection table slot, and to the slot's
  // value to reach the code (the same for every thread; here so that one ldp
  // from x19 fetches both).
  uint64_t indirection_table_bias;
  uint64_t execute_base;
  // a64_longjmp_sync_at_resolve: where the stack synchronization helper
  // resumes (the return site ResolveFunction found), set with
  // pending_stackpoint_sync_depth.
  uint64_t pending_sync_target;
  // a64_shared_cold_paths: the routines every function's rarely taken paths
  // go through (the same for every thread; here so that generated code, which
  // doesn't know how far from them it will be placed, reaches them with one
  // load from x19).
  uint64_t cold_preempt_thunk;
  uint64_t cold_stackpoint_overflow_thunk;
  uint64_t cold_call_lookup_thunk;
  uint64_t cold_external_lookup_thunk;
};
// ThreadState reserves 256 bytes ahead of the PPC context for this.
static_assert(sizeof(A64BackendContext) <= 256);

// Default FPCR for FPU mode (round to nearest, no flush to zero).
constexpr unsigned int DEFAULT_FPU_FPCR = 0;
// Default FPCR for VMX mode (flush to zero, preserve NaN payloads).
constexpr unsigned int DEFAULT_VMX_FPCR = (1 << 24);  // FZ

class A64Backend : public Backend {
 public:
  static constexpr uint32_t kForceReturnAddress = 0x9FFF0000u;

  explicit A64Backend();
  ~A64Backend() override;

  A64CodeCache* code_cache() const { return code_cache_.get(); }
  uintptr_t emitter_data() const { return emitter_data_; }
  bool extended_gpr_allocator() const { return extended_gpr_allocator_; }

  std::string name() const override { return "a64"; }

  HostToGuestThunk host_to_guest_thunk() const { return host_to_guest_thunk_; }
  GuestToHostThunk guest_to_host_thunk() const { return guest_to_host_thunk_; }
  // Without the q4-q31 save (a64_extern_thunk_no_vec), for CallExtern.
  GuestToHostThunk guest_to_host_thunk_no_vec() const {
    return guest_to_host_thunk_no_vec_;
  }
  ResolveFunctionThunk resolve_function_thunk() const {
    return resolve_function_thunk_;
  }
  void* synchronize_guest_and_host_stack_helper() const {
    return synchronize_guest_and_host_stack_helper_;
  }
  // The same, resuming at A64BackendContext::pending_sync_target instead of
  // the address in x8.
  void* synchronize_stack_and_resume_helper() const {
    return synchronize_stack_and_resume_helper_;
  }

  bool Initialize(Processor* processor) override;

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override;

  std::unique_ptr<Assembler> CreateAssembler() override;

  std::unique_ptr<GuestFunction> CreateGuestFunction(Module* module,
                                                     uint32_t address) override;

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override;

  void InstallBreakpoint(Breakpoint* breakpoint) override;
  void InstallBreakpoint(Breakpoint* breakpoint, Function* fn) override;
  void UninstallBreakpoint(Breakpoint* breakpoint) override;
  void InitializeBackendContext(void* ctx) override;
  void DeinitializeBackendContext(void* ctx) override;
  void PrepareForReentry(void* ctx) override;

  A64BackendContext* BackendContextForGuestContext(void* ctx) {
    return reinterpret_cast<A64BackendContext*>(
        reinterpret_cast<intptr_t>(ctx) - sizeof(A64BackendContext));
  }

  uint32_t CreateGuestTrampoline(GuestTrampolineProc proc, void* userdata1,
                                 void* userdata2, bool long_term) override;
  void FreeGuestTrampoline(uint32_t trampoline_addr) override;
  void SetGuestRoundingMode(void* ctx, unsigned int mode) override;
  bool PopulatePseudoStacktrace(GuestPseudoStackTrace* st) override;

  uint32_t ReservedLoad32(ppc::PPCContext* context, uint32_t address) override;
  uint64_t ReservedLoad64(ppc::PPCContext* context, uint32_t address) override;
  bool ReservedStore32(ppc::PPCContext* context, uint32_t address,
                       uint32_t value) override;
  bool ReservedStore64(ppc::PPCContext* context, uint32_t address,
                       uint64_t value) override;

  bool trace_instr_available() const override;
  bool trace_data_available() const override;
  bool trace_func_available() const override;
  bool trace_instr_enabled() const override;
  void set_trace_instr_enabled(bool value) override;
  bool trace_data_enabled() const override;
  void set_trace_data_enabled(bool value) override;
  bool trace_func_enabled() const override;
  void set_trace_func_enabled(bool value) override;

  void RecordMMIOExceptionForGuestInstruction(void* host_address);

 private:
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);

  // Register count, thunk ABI and frame layout must share one init-time mode.
  bool extended_gpr_allocator_ = false;
  uintptr_t capstone_handle_ = 0;

  std::unique_ptr<A64CodeCache> code_cache_;
  uintptr_t emitter_data_ = 0;

  HostToGuestThunk host_to_guest_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_no_vec_ = nullptr;
  ResolveFunctionThunk resolve_function_thunk_ = nullptr;
  void* synchronize_guest_and_host_stack_helper_ = nullptr;
  void* synchronize_stack_and_resume_helper_ = nullptr;
  void* cold_preempt_thunk_ = nullptr;
  void* cold_stackpoint_overflow_thunk_ = nullptr;
  void* cold_call_lookup_thunk_ = nullptr;
  void* cold_external_lookup_thunk_ = nullptr;

 public:
  void* try_acquire_reservation_helper_ = nullptr;
  void* reserved_store_32_helper = nullptr;
  void* reserved_store_64_helper = nullptr;

 private:
  alignas(64) ReserveHelper reserve_helper_;
  BitMap guest_trampoline_address_bitmap_;
  uint8_t* guest_trampoline_memory_ = nullptr;
  bool guest_trampolines_sub4gb_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
