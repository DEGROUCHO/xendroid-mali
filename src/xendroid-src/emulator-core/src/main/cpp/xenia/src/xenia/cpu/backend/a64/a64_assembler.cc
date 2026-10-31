/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_assembler.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

#include "third_party/capstone/include/capstone/aarch64.h"
#include "third_party/capstone/include/capstone/capstone.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/reset_scope.h"
#include "xenia/base/string.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/hir/block.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/processor.h"

DECLARE_path(log_file);
DECLARE_bool(a64_fold_guest_address);
DECLARE_bool(a64_compact_mmio_check);
DECLARE_bool(emit_inline_mmio_checks);
DEFINE_string(jit_dump_functions, "",
              "Comma-separated hexadecimal guest function entry addresses. "
              "Dump optimized HIR, ARM64 hex/disassembly and static codegen "
              "statistics next to the log at compilation; empty disables.",
              "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using xe::cpu::hir::HIRBuilder;

namespace {
bool IsJitDumpTarget(uint32_t address) {
  std::string_view list(cvars::jit_dump_functions);
  while (!list.empty()) {
    size_t end = list.find(',');
    auto entry = list.substr(0, end);
    list = end == std::string_view::npos ? std::string_view{}
                                         : list.substr(end + 1);
    auto first = entry.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      continue;
    }
    entry = entry.substr(first, entry.find_last_not_of(" \t") - first + 1);
    if (entry.size() >= 2 && entry[0] == '0' &&
        (entry[1] == 'x' || entry[1] == 'X')) {
      entry.remove_prefix(2);
    }
    uint32_t parsed = 0;
    auto result = std::from_chars(entry.data(), entry.data() + entry.size(),
                                  parsed, 16);
    if (result.ec == std::errc{} && result.ptr == entry.data() + entry.size() &&
        parsed == address) {
      return true;
    }
  }
  return false;
}
}  // namespace

A64Assembler::A64Assembler(A64Backend* backend)
    : Assembler(backend), a64_backend_(backend), capstone_handle_(0) {
  if (cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &capstone_handle_) != CS_ERR_OK) {
    assert_always("Failed to initialize capstone for ARM64");
  }
  cs_option(capstone_handle_, CS_OPT_DETAIL, CS_OPT_OFF);
}

A64Assembler::~A64Assembler() {
  emitter_.reset();

  if (capstone_handle_) {
    cs_close(&capstone_handle_);
  }
}

bool A64Assembler::Initialize() {
  if (!Assembler::Initialize()) {
    return false;
  }

  emitter_.reset(new A64Emitter(a64_backend_, &allocator_));

  return true;
}

void A64Assembler::Reset() {
  string_buffer_.Reset();
  Assembler::Reset();
}

bool A64Assembler::Assemble(GuestFunction* function, HIRBuilder* builder,
                            uint32_t debug_info_flags,
                            std::unique_ptr<FunctionDebugInfo> debug_info) {
  SCOPE_profile_cpu_f("cpu");

  // Reset when we leave.
  xe::make_reset_scope(this);

  const bool dump_jit = IsJitDumpTarget(function->address());
  StringBuffer optimized_hir;
  if (dump_jit) {
    // Capture before Emit assigns physical offsets to local spill slots.
    builder->Dump(&optimized_hir);
  }

  // Lower HIR -> ARM64.
  void* machine_code = nullptr;
  size_t code_size = 0;
  if (!emitter_->Emit(function, builder, debug_info_flags, debug_info.get(),
                      &machine_code, &code_size, &function->source_map())) {
    return false;
  }

  if (dump_jit) {
    DumpJitFunction(function, builder, optimized_hir.buffer(), machine_code,
                    code_size);
  }

  // Stash generated machine code.
  if (debug_info_flags & DebugInfoFlags::kDebugInfoDisasmMachineCode) {
    DumpMachineCode(machine_code, code_size, function->source_map(),
                    &string_buffer_);
    debug_info->set_machine_code_disasm(xe_strdup(string_buffer_.buffer()));
    string_buffer_.Reset();
  }

  function->set_debug_info(std::move(debug_info));
  static_cast<A64Function*>(function)->Setup(
      reinterpret_cast<uint8_t*>(machine_code), code_size);

  // Install into indirection table (supports code cache at any 64-bit VA).
  reinterpret_cast<A64CodeCache*>(backend_->code_cache())
      ->AddIndirection64(function->address(),
                         reinterpret_cast<uint64_t>(machine_code));

  return true;
}

void A64Assembler::DumpJitFunction(GuestFunction* function, HIRBuilder* builder,
                                  const char* optimized_hir, void* machine_code,
                                  size_t code_size) {
  StringBuffer dump;
  std::set<uint32_t> guest_pcs;
  size_t hir_memory = 0, context_accesses = 0, spill_loads = 0, spill_stores = 0;
  for (auto* block = builder->first_block(); block; block = block->next) {
    for (auto* i = block->instr_head; i; i = i->next) {
      switch (i->GetOpcodeNum()) {
        case hir::OPCODE_SOURCE_OFFSET:
          guest_pcs.insert(static_cast<uint32_t>(i->src1.offset));
          break;
        case hir::OPCODE_LOAD_LOCAL:
          ++spill_loads;
          break;
        case hir::OPCODE_STORE_LOCAL:
          ++spill_stores;
          break;
        case hir::OPCODE_LOAD_CONTEXT:
        case hir::OPCODE_STORE_CONTEXT:
          ++context_accesses;
          break;
        case hir::OPCODE_LOAD:
        case hir::OPCODE_STORE:
        case hir::OPCODE_LOAD_OFFSET:
        case hir::OPCODE_STORE_OFFSET:
        case hir::OPCODE_LOAD_MMIO:
        case hir::OPCODE_STORE_MMIO:
        case hir::OPCODE_LVL:
        case hir::OPCODE_LVR:
        case hir::OPCODE_STVL:
        case hir::OPCODE_STVR:
        case hir::OPCODE_RESERVED_LOAD:
        case hir::OPCODE_RESERVED_STORE:
        case hir::OPCODE_ATOMIC_COMPARE_EXCHANGE:
          ++hir_memory;
          break;
        default:
          break;
      }
    }
  }
  const size_t guest_span = function->end_address() >= function->address()
      ? (uint64_t(function->end_address()) - function->address()) / 4 + 1 : 0;
  dump.AppendFormat("guest {:08X}-{:08X}, host {}, bytes={}\n",
                    function->address(), function->end_address(), machine_code,
                    code_size);
  dump.AppendFormat("a64_fold_guest_address={} a64_compact_mmio_check={} "
                    "emit_inline_mmio_checks={}\n",
                    cvars::a64_fold_guest_address, cvars::a64_compact_mmio_check,
                    cvars::emit_inline_mmio_checks);
  dump.Append("\n===== OPTIMIZED HIR (after register allocation) =====\n");
  dump.Append(optimized_hir ? optimized_hir : "(empty)\n");
  dump.Append("\n===== ARM64 (offset, guest PC, bytes in memory order, disassembly) =====\n");
  size_t decoded = 0, calls = 0, memory = 0, byteswaps = 0, stack_accesses = 0;
  size_t fpcr_writes = 0, source_index = 0;
  uint32_t guest_pc = 0;
  // Per-guest-PC host stats: pc -> {host insns, memory sites, byteswap sites}.
  // Every 4-byte slot counts as one host insn (decoded or .word fallback).
  struct PcStat {
    size_t insns = 0;
    size_t mem = 0;
    size_t rev = 0;
  };
  std::map<uint32_t, PcStat> pcstats;
  const auto& source = function->source_map();
  const auto* bytes = static_cast<const uint8_t*>(machine_code);
  // cs_malloc supports iter decoding with every Capstone detail configuration.
  cs_insn* insn = capstone_handle_ ? cs_malloc(capstone_handle_) : nullptr;
  for (size_t offset = 0; offset + 4 <= code_size; offset += 4) {
    while (source_index < source.size() &&
           source[source_index].code_offset <= offset) {
      guest_pc = source[source_index++].guest_address;
    }
    const uint32_t insn_pc = guest_pc;
    bool insn_is_memory = false, insn_is_rev = false;
    dump.AppendFormat("{:08X} {:08X} {:02X}{:02X}{:02X}{:02X}  ", offset,
                      guest_pc, bytes[offset], bytes[offset + 1],
                      bytes[offset + 2], bytes[offset + 3]);
    const uint8_t* cursor = bytes + offset;
    size_t remaining = 4;
    uint64_t pc = reinterpret_cast<uint64_t>(machine_code) + offset;
    if (insn && cs_disasm_iter(capstone_handle_, &cursor, &remaining, &pc, insn)) {
      ++decoded;
      std::string_view mnemonic(insn->mnemonic), operands(insn->op_str);
      calls += mnemonic == "bl" || mnemonic == "blr";
      bool is_memory = mnemonic.substr(0, 2) == "ld" ||
                       mnemonic.substr(0, 2) == "st" ||
                       mnemonic.substr(0, 3) == "cas" ||
                       mnemonic.substr(0, 3) == "swp";
      memory += is_memory;
      stack_accesses +=
          is_memory && operands.find("[sp") != std::string_view::npos;
      byteswaps += mnemonic.substr(0, 3) == "rev";
      fpcr_writes +=
          mnemonic == "msr" &&
          (operands.find("fpcr") != std::string_view::npos ||
           operands.find("FPCR") != std::string_view::npos);
      insn_is_memory = is_memory;
      insn_is_rev = mnemonic.substr(0, 3) == "rev";
      dump.AppendFormat("{} {}\n", insn->mnemonic, insn->op_str);
    } else {
      // Never silently lose bytes after an unsupported instruction.
      uint32_t word;
      std::memcpy(&word, bytes + offset, 4);
      dump.AppendFormat(".word 0x{:08X}\n", word);
    }
    PcStat& st = pcstats[insn_pc];
    st.insns += 1;
    st.mem += insn_is_memory ? 1 : 0;
    st.rev += insn_is_rev ? 1 : 0;
  }
  if (insn) {
    cs_free(insn, 1);
  }
  // Normally code_size is a multiple of four; retain any unexpected tail bytes.
  for (size_t offset = code_size & ~size_t(3); offset < code_size; ++offset) {
    dump.AppendFormat("{:08X} byte {:02X}\n", offset, bytes[offset]);
  }
  size_t guest_calls = emitter_->guest_call_sites();
  size_t helper_branches = emitter_->helper_branch_sites();
  dump.AppendFormat("\n===== STATIC STATS (sites, NOT execution counts) =====\n"
                    "host_instructions={} decoded={} guest_span_instructions={} "
                    "guest_source_pcs={} host_per_guest_span={:.3f} "
                    "host_per_guest_source={:.3f}\n",
                    code_size / 4, decoded, guest_span, guest_pcs.size(),
                    guest_span ? double(code_size / 4) / guest_span : 0.0,
                    guest_pcs.empty() ? 0.0
                                      : double(code_size / 4) / guest_pcs.size());
  dump.AppendFormat("host_link_call_sites={} guest_link_call_sites={} "
                    "native_helper_call_sites={} native_helper_branch_sites={}\n"
                    "host_memory_sites={} host_byteswap_sites={} host_sp_memory_sites={} "
                    "host_fpcr_writes={}\n"
                    "hir_guest_memory_sites={} hir_context_accesses={} "
                    "hir_spill_loads={} hir_spill_stores={} local_slots={} stack_bytes={}\n",
                    calls, guest_calls, decoded == code_size / 4 && calls >= guest_calls
                        ? std::to_string(calls - guest_calls + helper_branches)
                        : "unknown (incomplete decode)",
                    helper_branches,
                    memory, byteswaps, stack_accesses, fpcr_writes, hir_memory,
                    context_accesses, spill_loads, spill_stores, builder->locals().size(),
                    emitter_->stack_size());
  dump.Append("Includes prolog, epilog and cold paths; excludes shared thunks/callee "
              "bodies. Link calls are BL/BLR; helper totals also include stack-sync BR. "
              "Memory and REV counts cover decoded instructions only. "
              "HIR locals include allocator spills/call preservation; SP accesses "
              "also include scratch/frame traffic. Guest PC attribution outside "
              "the body may be approximate. guest_span is inclusive scanner extent.\n");
  dump.Append("\n===== PER-GUEST-PC HOST STATS =====\n");
  for (const auto& entry : pcstats) {
    dump.AppendFormat("pcstat {:08X} insns={} mem={} rev={}\n", entry.first,
                      entry.second.insns, entry.second.mem,
                      entry.second.rev);
  }
  // One file per guest entry. Serialize recompilations of the same entry.
  static std::mutex dump_mutex;
  std::lock_guard<std::mutex> lock(dump_mutex);
  auto dir = cvars::log_file.empty() ? xe::filesystem::GetExecutableFolder()
                                    : cvars::log_file.parent_path();
  auto path = dir / fmt::format("jitdump_{:08X}.txt", function->address());
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (file) {
    bool written = fwrite(dump.buffer(), 1, dump.length(), file) == dump.length();
    written = fclose(file) == 0 && written;
    if (written) {
      XELOGI("jit_dump_functions: wrote {}", xe::path_to_utf8(path));
      return;
    }
  }
  XELOGW("jit_dump_functions: cannot write {}; logging dump instead",
         xe::path_to_utf8(path));
  // Keep each log entry bounded (the logging buffer may truncate long messages).
  std::string_view lines(dump.buffer(), dump.length());
  while (!lines.empty()) {
    size_t count = std::min(lines.find('\n'), size_t(512));
    count = std::min(count, lines.size());
    XELOGI("jit {:08X}: {}", function->address(), lines.substr(0, count));
    lines.remove_prefix(count);
    if (!lines.empty() && lines.front() == '\n') lines.remove_prefix(1);
  }
}

void A64Assembler::DumpMachineCode(
    void* machine_code, size_t code_size,
    const std::vector<SourceMapEntry>& source_map, StringBuffer* str) {
  if (source_map.empty()) {
    return;
  }
  auto source_map_index = 0;
  uint32_t next_code_offset = source_map[0].code_offset;

  const uint8_t* code_ptr = reinterpret_cast<uint8_t*>(machine_code);
  size_t remaining_code_size = code_size;
  uint64_t address = uint64_t(machine_code);
  cs_insn insn = {0};
  while (remaining_code_size &&
         cs_disasm_iter(capstone_handle_, &code_ptr, &remaining_code_size,
                        &address, &insn)) {
    // Look up source offset.
    auto code_offset =
        uint32_t(code_ptr - reinterpret_cast<uint8_t*>(machine_code));
    if (code_offset >= next_code_offset &&
        source_map_index < source_map.size()) {
      auto& source_map_entry = source_map[source_map_index];
      str->AppendFormat("{:08X} ", source_map_entry.guest_address);
      ++source_map_index;
      next_code_offset = source_map_index < source_map.size()
                             ? source_map[source_map_index].code_offset
                             : UINT_MAX;
    } else {
      str->Append("         ");
    }

    str->AppendFormat("{:08X}      {:<6} {}\n", uint32_t(insn.address),
                      insn.mnemonic, insn.op_str);
  }
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
