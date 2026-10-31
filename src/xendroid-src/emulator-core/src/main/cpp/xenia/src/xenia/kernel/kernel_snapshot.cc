/**
 ******************************************************************************
 * XenDroid runtime save state: kernel section capture / cold restore.        *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_snapshot.h"
#include "xenia/snapshot/kernel_memory_validation.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/snapshot/ppc_state.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe {
namespace kernel {

// Defined in xthread.cc.
extern uint32_t next_xthread_id_;

static_assert(uint32_t(XObject::Type::Event) == snapshot::kObjEvent);
static_assert(uint32_t(XObject::Type::File) == snapshot::kObjFile);
static_assert(uint32_t(XObject::Type::IOCompletion) ==
              snapshot::kObjIOCompletion);
static_assert(uint32_t(XObject::Type::Module) == snapshot::kObjModule);
static_assert(uint32_t(XObject::Type::Mutant) == snapshot::kObjMutant);
static_assert(uint32_t(XObject::Type::NotifyListener) ==
              snapshot::kObjNotifyListener);
static_assert(uint32_t(XObject::Type::Enumerator) == snapshot::kObjEnumerator);
static_assert(uint32_t(XObject::Type::Semaphore) == snapshot::kObjSemaphore);
static_assert(uint32_t(XObject::Type::Socket) == snapshot::kObjSocket);
static_assert(uint32_t(XObject::Type::SymbolicLink) ==
              snapshot::kObjSymbolicLink);
static_assert(uint32_t(XObject::Type::Thread) == snapshot::kObjThread);
static_assert(uint32_t(XObject::Type::Timer) == snapshot::kObjTimer);
static_assert(XObject::kHandleBase == snapshot::kGuestHandleBase);
static_assert(XObject::kHandleHostBase == snapshot::kHostHandleBase);

namespace {

const char* TypeName(XObject::Type type) {
  switch (type) {
    case XObject::Type::Enumerator:
      return "Enumerator";
    case XObject::Type::Event:
      return "Event";
    case XObject::Type::File:
      return "File";
    case XObject::Type::IOCompletion:
      return "IOCompletion";
    case XObject::Type::Module:
      return "Module";
    case XObject::Type::Mutant:
      return "Mutant";
    case XObject::Type::NotifyListener:
      return "NotifyListener";
    case XObject::Type::Semaphore:
      return "Semaphore";
    case XObject::Type::Session:
      return "Session";
    case XObject::Type::Socket:
      return "Socket";
    case XObject::Type::SymbolicLink:
      return "SymbolicLink";
    case XObject::Type::Thread:
      return "Thread";
    case XObject::Type::Timer:
      return "Timer";
    case XObject::Type::Device:
      return "Device";
    default:
      return "Undefined";
  }
}

// Kernel calls whose wait can simply be executed again after a restore: they
// have no side effect before the wait begins and take every input from the
// argument registers / guest memory.
bool RestartableWaitExport(const char* name, uint8_t wait_kind) {
  using Kind = XThread::CooperativeWaitKind;
  const Kind kind = static_cast<Kind>(wait_kind);
  auto is = [name](const char* candidate) {
    return std::strcmp(name, candidate) == 0;
  };
  // Polls its port and parks until a notification arrives; it dequeues only
  // after the wait succeeded, so restarting it has no lost side effect.
  if (is("NtRemoveIoCompletion")) return true;
  switch (kind) {
    case Kind::kSingle:
      return is("KeWaitForSingleObject") || is("NtWaitForSingleObjectEx");
    case Kind::kMultiAny:
    case Kind::kMultiAll:
      return is("KeWaitForMultipleObjects") ||
             is("NtWaitForMultipleObjectsEx");
    case Kind::kDelay:
      return is("KeDelayExecutionThread");
    default:
      return false;
  }
}

// Finds the import thunk a thread is blocked in by decoding the call
// instruction that produced its link register: `bl`, `bla` or `bctrl`.
}  // namespace

bool KernelSnapshot::ObjectsSnapshottable(KernelState* ks, std::string& reason) {
  for (const auto& object : ks->object_table()->GetAllObjects()) {
    if (object->type() != XObject::Type::Socket) continue;
    std::string why;
    if (!static_cast<XSocket*>(object.get())->RuntimeSnapshotPossible(why)) {
      reason = fmt::format("socket {:08X}: {}", object->handle(), why);
      return false;
    }
  }
  return true;
}

bool KernelSnapshot::FindBlockedThunk(KernelState* ks, XThread* thread,
                                      uint32_t& thunk_out, std::string& error) {
  auto* context = thread->thread_state_->context();
  const uint32_t lr = uint32_t(context->lr);
  auto* processor = ks->processor();
  if (!lr || (lr & 3) || !processor->LookupModule(lr)) {
    error = fmt::format("thread {:08X}: link register {:08X} is not guest code",
                        thread->thread_id_, lr);
    return false;
  }
  const uint32_t insn =
      xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(lr - 4));
  uint32_t thunk = 0;
  if ((insn & 0xFC000003u) == 0x48000001u) {  // bl
    int32_t displacement = int32_t(insn & 0x03FFFFFCu);
    if (displacement & 0x02000000) {
      displacement -= 0x04000000;
    }
    thunk = uint32_t(int64_t(lr) - 4 + displacement);
  } else if ((insn & 0xFC000003u) == 0x48000003u) {  // bla
    int32_t target = int32_t(insn & 0x03FFFFFCu);
    if (target & 0x02000000) {
      target -= 0x04000000;
    }
    thunk = uint32_t(target);
  } else if (insn == 0x4E800421u) {  // bctrl
    thunk = uint32_t(context->ctr);
  } else {
    error = fmt::format(
        "thread {:08X}: blocked, but {:08X} is not a call instruction ({:08X})",
        thread->thread_id_, lr - 4, insn);
    return false;
  }
  // Games also reach the import through a small wrapper they call with bl:
  // register-only argument setup, then a plain b (tail call) to the thunk. The
  // registers at the thunk are the ones the wait still holds, and its return
  // goes to the wrapper's caller, so restarting at the thunk is equivalent.
  cpu::Function* function = processor->QueryFunction(thunk);
  for (int hops = 0; hops < 4 &&
       (!function || function->behavior() != cpu::Function::Behavior::kExtern);
       ++hops) {
    uint32_t pc = thunk;
    bool followed = false;
    for (int step = 0; step < 8 && processor->LookupModule(pc); ++step, pc += 4) {
      const uint32_t op =
          xe::load_and_swap<uint32_t>(ks->memory()->TranslateVirtual(pc));
      if ((op & 0xFC000003u) == 0x48000000u) {  // b (relative, no link)
        int32_t displacement = int32_t(op & 0x03FFFFFCu);
        if (displacement & 0x02000000) displacement -= 0x04000000;
        thunk = uint32_t(int64_t(pc) + displacement);
        followed = true;
        break;
      }
      const uint32_t primary = op >> 26;
      const bool register_only =
          primary == 14 || primary == 15 ||  // addi, addis (li, lis)
          primary == 24 || primary == 25 ||  // ori, oris
          primary == 21 ||                   // rlwinm
          (primary == 31 && ((op >> 1) & 0x3FF) == 444);  // or (mr)
      if (!register_only) break;
    }
    if (!followed) break;
    function = processor->QueryFunction(thunk);
  }
  if (!function || function->behavior() != cpu::Function::Behavior::kExtern) {
    error = fmt::format("thread {:08X}: {:08X} is not a kernel import thunk",
                        thread->thread_id_, thunk);
    return false;
  }
  auto* guest_function = static_cast<cpu::GuestFunction*>(function);
  const cpu::Export* export_data = guest_function->export_data();
  if (!export_data ||
      !RestartableWaitExport(export_data->name,
                             thread->scheduler_links_.wait_kind)) {
    error = fmt::format(
        "thread {:08X} '{}' is blocked in {} (wait kind {}), which cannot be "
        "restarted",
        thread->thread_id_, thread->thread_name_,
        export_data ? export_data->name : "<unknown>",
        uint32_t(thread->scheduler_links_.wait_kind));
    return false;
  }
  thunk_out = thunk;
  return true;
}

bool KernelSnapshot::Capture(KernelState* ks,
                             const std::vector<XThread*>& barrier_threads,
                             snapshot::KernelImage& out, std::string& error) {
  using namespace snapshot;
  KernelImage img;

  if (auto* content = ks->content_manager()) {
    if (content->OpenPackageCount()) {
      error = "a content package is mounted; its host state is not saved";
      return false;
    }
  }

  img.kernel_guest_globals = ks->kernel_guest_globals_;
  img.timestamp_bundle = ks->ke_timestamp_bundle_ptr_;
  img.next_thread_id = next_xthread_id_;

  std::set<XThread*> barrier(barrier_threads.begin(), barrier_threads.end());
  std::set<XThread*> seen_threads;
  std::vector<XObject*> plain_objects;
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    img.table_capacity = table.table_capacity_;
    img.table_cursor = table.last_free_entry_;
    std::set<XObject*> seen;
    for (uint32_t slot = 0; slot < table.table_capacity_; ++slot) {
      auto& entry = table.table_[slot];
      if (!entry.object) {
        continue;
      }
      XObject* object = entry.object;
      img.slots.push_back(
          {XObject::kHandleBase + (slot << 2), entry.handle_ref_count});
      if (!seen.insert(object).second) {
        continue;
      }
      switch (object->type()) {
        case XObject::Type::Module: {
          KernelBootObject boot;
          boot.handle = object->handle();
          boot.type = snapshot::kObjModule;
          boot.guest_object = object->guest_object();
          auto* module = static_cast<XModule*>(object);
          boot.name = module->path();
          boot.hmodule = module->hmodule_ptr();
          if (module->module_type() == XModule::ModuleType::kUserModule)
            boot.xex_header =
                static_cast<UserModule*>(module)->guest_xex_header();
          img.boot_objects.push_back(std::move(boot));
          break;
        }
        case XObject::Type::Thread: {
          auto* thread = static_cast<XThread*>(object);
          if (!thread->is_guest_thread()) {
            error = "a host thread is registered as a guest object";
            return false;
          }
          if (!barrier.count(thread)) {
            error = fmt::format("guest thread {:08X} is not held by the barrier",
                                thread->thread_id_);
            return false;
          }
          seen_threads.insert(thread);
          break;
        }
        case XObject::Type::Enumerator:
          if (!static_cast<XEnumerator*>(object)->snapshottable()) {
            error = fmt::format(
                "enumerator {:08X} keeps state other than its items",
                object->handle());
            return false;
          }
          plain_objects.push_back(object);
          break;
        case XObject::Type::Event:
        case XObject::Type::File:
        case XObject::Type::IOCompletion:
        case XObject::Type::Mutant:
        case XObject::Type::NotifyListener:
        case XObject::Type::Semaphore:
        case XObject::Type::Socket:
        case XObject::Type::SymbolicLink:
        case XObject::Type::Timer:
          plain_objects.push_back(object);
          break;
        default:
          error = fmt::format(
              "unsupported kernel object {} (handle {:08X}) is alive",
              TypeName(object->type()), object->handle());
          return false;
      }
    }
    if (seen_threads.size() != barrier.size()) {
      error = "the barrier holds a thread that is not in the object table";
      return false;
    }
  }

  // Plain objects: legacy payload through a length-bounded stream.
  std::vector<uint8_t> scratch(snapshot::kMaxObjectPayload);
  for (XObject* object : plain_objects) {
    ByteStream stream(scratch.data(), scratch.size());
    if (!object->Save(&stream) || stream.failed()) {
      error = fmt::format("{} object {:08X} cannot be serialised",
                          TypeName(object->type()), object->handle());
      return false;
    }
    KernelObjectRecord record;
    record.type = uint32_t(object->type());
    record.name = object->name_;
    record.payload.assign(scratch.begin(), scratch.begin() + stream.offset());
    img.objects.push_back(std::move(record));
  }

  // Threads.
  for (XThread* thread : barrier_threads) {
    auto& links = thread->scheduler_links_;
    if (thread->reentry_nested_guest_depth_) {
      error = fmt::format(
          "thread {:08X} is inside a nested guest call (native frames)",
          thread->thread_id_);
      return false;
    }
    if (!thread->pending_mutant_acquires_.empty() || !thread->thread_state_) {
      error = fmt::format("thread {:08X} has not started yet", thread->thread_id_);
      return false;
    }
    KernelThreadRecord record;
    record.thread_id = thread->thread_id_;
    record.main_thread = thread->main_thread_ ? 1 : 0;
    record.allocated_guest_object = thread->allocated_guest_object_ ? 1 : 0;
    record.guest_object = thread->guest_object_ptr_;
    record.handles = thread->handles_;
    record.name = thread->thread_name_;
    record.tls_static = thread->tls_static_address_;
    record.tls_dynamic = thread->tls_dynamic_address_;
    record.tls_total = thread->tls_total_size_;
    record.pcr = thread->pcr_address_;
    record.stack_alloc_base = thread->stack_alloc_base_;
    record.stack_alloc_size = thread->stack_alloc_size_;
    record.stack_base = thread->stack_base_;
    record.stack_limit = thread->stack_limit_;
    record.priority = thread->priority_;
    record.base_priority = thread->base_priority_;
    record.boost = thread->boost_amount_;
    record.creation_stack_size = thread->creation_params_.stack_size;
    record.xapi_startup = thread->creation_params_.xapi_thread_startup;
    record.start_address = thread->creation_params_.start_address;
    record.start_context = thread->creation_params_.start_context;
    record.creation_flags = thread->creation_params_.creation_flags;
    record.guest_process = thread->creation_params_.guest_process;
    if (links.snapshot_parked) {
      record.resume_kind = snapshot::kResumeCanonicalPc;
      record.resume_pc = links.snapshot_pc;
    } else if (links.blocked) {
      record.resume_kind = snapshot::kResumeReenterExtern;
      if (!FindBlockedThunk(ks, thread, record.resume_pc, error)) {
        return false;
      }
    } else {
      error = fmt::format("thread {:08X} is neither parked nor blocked",
                          thread->thread_id_);
      return false;
    }
    record.cpu_state = snapshot::PpcState::Capture(
                           *thread->thread_state_->context(), record.resume_pc)
                           .Encode();
    img.threads.push_back(std::move(record));
  }

  for (auto& link : ks->file_system()->GetSymbolicLinks()) {
    img.symlinks.push_back(std::move(link));
  }
  std::sort(img.symlinks.begin(), img.symlinks.end());
  for (const auto& kernel_module : ks->kernel_modules_) {
    auto lock = kernel_module->global_critical_region_.Acquire();
    const size_t first = img.trampolines.size();
    for (const auto& [ordinal, address] : kernel_module->guest_trampoline_map_)
      img.trampolines.push_back({kernel_module->name(), ordinal, address});
    std::sort(img.trampolines.begin() + first, img.trampolines.end(),
              [](const auto& a, const auto& b) { return a.address < b.address; });
  }
  std::sort(img.slots.begin(), img.slots.end(),
            [](const KernelSlot& a, const KernelSlot& b) {
              return a.handle < b.handle;
            });

  if (!img.Validate(error)) {
    error = "kernel capture self-check failed: " + error;
    return false;
  }
  out = std::move(img);
  return true;
}

bool KernelSnapshot::ApplyThreadRecord(
    KernelState* ks, XThread* thread,
    const snapshot::KernelThreadRecord& record, std::string& error) {
  snapshot::PpcState cpu;
  if (!snapshot::PpcState::Decode(record.cpu_state, cpu)) {
    error = "thread cpu state does not decode";
    return false;
  }
  thread->main_thread_ = record.main_thread != 0;
  thread->thread_name_ = record.name;
  thread->priority_ = record.priority;
  thread->base_priority_ = record.base_priority;
  thread->boost_amount_ = record.boost;
  thread->creation_params_.stack_size = record.creation_stack_size;
  thread->creation_params_.xapi_thread_startup = record.xapi_startup;
  thread->creation_params_.start_address = record.start_address;
  thread->creation_params_.start_context = record.start_context;
  thread->creation_params_.creation_flags = record.creation_flags;
  thread->creation_params_.guest_process = record.guest_process;
  if (!thread->thread_state_) {
    thread->thread_state_ =
        new cpu::ThreadState(ks->processor(), thread->thread_id_,
                             thread->stack_base_, thread->pcr_address_);
  }
  auto* context = thread->thread_state_->context();
  context->kernel_state = ks;
  if (!cpu.Apply(*context)) {
    error = fmt::format("thread {:08X}: cpu state does not apply",
                        record.thread_id);
    return false;
  }
  return true;
}

bool KernelSnapshot::LoadRuntimeModules(KernelState* ks,
                                        const snapshot::KernelImage& img,
                                        std::string& error) {
  // GetProcAddress trampolines are generated on demand, so the fresh boot has
  // none of those the title asked for: guest code calling one ran into zeroed
  // memory (Halo Reach: the JIT scanned a function ending before its start
  // and allocated 16 GiB). Generated again in address order per module, they
  // land at the same addresses.
  for (const auto& t : img.trampolines) {
    auto module = ks->GetKernelModule(t.module);
    const uint32_t regenerated =
        module ? module->GetProcAddressByOrdinal(uint16_t(t.ordinal)) : 0;
    if (regenerated != t.address) {
      error = fmt::format(
          "{} trampoline for ordinal {:X} is at {:08X}, the snapshot has {:08X}",
          t.module, t.ordinal, regenerated, t.address);
      return false;
    }
  }
  if (!img.trampolines.empty())
    XELOGI("KernelSnapshot: regenerated {} kernel trampolines",
           img.trampolines.size());
  for (const auto& boot : img.boot_objects) {
    if (ks->GetModule(boot.name)) continue;
    auto module = ks->LoadUserModule(boot.name, false);
    if (!module) {
      error = fmt::format("module {} of the snapshot cannot be loaded",
                          boot.name);
      return false;
    }
    ks->ApplyTitleUpdate(module);
    if (XFAILED(ks->FinishLoadingUserModule(module, false))) {
      error = fmt::format("module {} of the snapshot cannot be finished",
                          boot.name);
      return false;
    }
    // DllMain ran in the saved session.
    module->is_attached_ = module->is_dll_module() && module->entry_point();
    XELOGI("KernelSnapshot: loaded runtime module {}", boot.name);
    // Owned like an XexLoadImage reference, released by XexUnloadImage.
    module.release();
  }
  return true;
}

bool KernelSnapshot::Restore(KernelState* ks, XThread* fresh_main,
                             const snapshot::KernelImage& img,
                             std::vector<object_ref<XThread>>& restored,
                             std::string& error) {
  using namespace snapshot;
  if (!img.Validate(error)) {
    return false;
  }
  if (!snapshot::ValidateKernelGuestMemory(img, [ks](uint32_t address, uint64_t length) {
        auto* heap = ks->memory()->LookupHeap(address);
        return heap && heap->IsRangeCommittedWritableRelaxed(address, length);
      }, [ks](uint32_t address, uint64_t length) {
        auto* heap = ks->memory()->LookupHeap(address);
        if (heap && heap->IsRangeCommittedWritableRelaxed(address, length, false)) return true;
        if (!heap) return false;
        // The host kernel writes object bodies, and guest memory stays host
        // accessible after a decommit: a title may decommit the page holding a
        // static object it still uses (Halo Reach semaphore in its .data).
        HeapAllocationInfo info = {};
        if (heap->QueryRegionInfo(address & ~0xFFFu, &info))
          XELOGW("RuntimeState: guest object at {:08X}+{} in a page that is not committed (region {:08X}+{:X} state {:X} protect {:X}, allocation {:08X}+{:X})",
                 address, length, info.base_address, info.region_size, info.state, info.protect,
                 info.allocation_base, info.allocation_size);
        return true;
      }, error)) return false;
  if (img.kernel_guest_globals != ks->kernel_guest_globals_ ||
      img.timestamp_bundle != ks->ke_timestamp_bundle_ptr_) {
    error = fmt::format(
        "boot divergence: kernel globals {:08X}/{:08X} vs snapshot "
        "{:08X}/{:08X}",
        ks->kernel_guest_globals_, ks->ke_timestamp_bundle_ptr_,
        img.kernel_guest_globals, img.timestamp_bundle);
    return false;
  }
  const KernelThreadRecord* main_record = nullptr;
  for (const auto& t : img.threads) {
    if (t.main_thread) {
      main_record = &t;
    }
  }
  if (!main_record) {
    error = "the snapshot has no main thread (it had already exited)";
    return false;
  }

  // 0. Modules the title loaded after launch were loaded again by
  //    LoadRuntimeModules under fresh handles: move each to its saved handle
  //    (the loader entry in guest memory stores it). Every module then points
  //    at the saved loader entry and guest header, which the restored memory
  //    holds; the fresh boot's allocations are gone with its heaps.
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    for (const auto& boot : img.boot_objects) {
      if (!boot.hmodule) continue;  // version 1 image
      XModule* module = nullptr;
      uint32_t from = 0;
      for (uint32_t slot = 0; slot < table.table_capacity_; ++slot) {
        XObject* object = table.table_[slot].object;
        if (object && object->type() == XObject::Type::Module &&
            static_cast<XModule*>(object)->path() == boot.name) {
          module = static_cast<XModule*>(object);
          from = slot;
          break;
        }
      }
      if (!module) continue;  // step 1 reports it
      const uint32_t to = (boot.handle & ~XObject::kHandleBase) >> 2;
      if (from != to) {
        if (module->handles_.size() != 1) {
          error = fmt::format("module {} has {} handles", boot.name,
                              module->handles_.size());
          return false;
        }
        if (to >= table.table_capacity_ &&
            !table.Resize(std::max(img.table_capacity, to + 1), false)) {
          error = "cannot grow the kernel object table";
          return false;
        }
        if (table.table_[to].object) {
          error = fmt::format("module {} handle {:08X} is already in use",
                              boot.name, boot.handle);
          return false;
        }
        table.table_[to] = table.table_[from];
        table.table_[from] = {};
        XELOGI("KernelSnapshot: module {} handle {:08X} -> {:08X}", boot.name,
               module->handles_[0], boot.handle);
        module->handles_[0] = boot.handle;
      }
      module->hmodule_ptr_ = boot.hmodule;
      if (module->module_type() == XModule::ModuleType::kUserModule)
        static_cast<UserModule*>(module)->guest_xex_header_ = boot.xex_header;
    }
  }

  // 1. The fresh boot must contain exactly the modules recorded as boot
  //    objects plus the suspended main thread, otherwise guest state would
  //    reference host objects that do not exist.
  //    Kernel startup itself touches guest dispatcher objects (KeSetEvent on
  //    UsbdBootEnumerationDoneEvent creates a native Event at the first
  //    handle on every boot). The snapshot holds the same native object with
  //    its saved state: drop the fresh one so step 5 recreates it.
  std::vector<uint32_t> boot_natives;
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    std::set<uint32_t> boot_matched;
    for (uint32_t slot = 0; slot < table.table_capacity_; ++slot) {
      XObject* object = table.table_[slot].object;
      if (!object) continue;
      const uint32_t handle = XObject::kHandleBase + (slot << 2);
      if (object == fresh_main) {
        continue;
      }
      if (object->type() != XObject::Type::Module &&
          object->handles().size() == 1 && object->handles()[0] == handle &&
          !object->allocated_guest_object_ && object->guest_object()) {
        bool saved = false;
        for (const auto& record : img.objects) {
          ObjectPayloadHeader h;
          if (record.type == uint32_t(object->type()) &&
              ParseObjectPayloadHeader(record.payload, h) &&
              !h.allocated_guest_object &&
              h.guest_object == object->guest_object() &&
              h.handles.size() == 1 && h.handles[0] == handle) {
            saved = true;
            break;
          }
        }
        if (saved) {
          boot_natives.push_back(handle);
          continue;
        }
      }
      if (object->type() != XObject::Type::Module) {
        error = fmt::format(
            "boot divergence: unexpected {} object {:08X} after boot",
            TypeName(object->type()), handle);
        return false;
      }
      bool matched = false;
      for (const auto& boot : img.boot_objects) {
        if (boot.handle == object->handle() &&
            boot.guest_object == object->guest_object() &&
            boot.name == static_cast<XModule*>(object)->path()) {
          matched = true;
          boot_matched.insert(boot.handle);
          break;
        }
      }
      if (!matched) {
        error = fmt::format("boot divergence: module {:08X} ({}) differs",
                            handle, static_cast<XModule*>(object)->path());
        return false;
      }
    }
    if (boot_matched.size() != img.boot_objects.size()) {
      error = "boot divergence: a module of the snapshot was not loaded";
      return false;
    }
  }
  for (uint32_t handle : boot_natives) {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    // The kernel's own reference: the slot goes away with the object.
    table.table_[(handle & ~XObject::kHandleBase) >> 2].handle_ref_count = 0;
    table.RemoveHandle(handle);
  }

  // 2. Reuse the fresh main thread for the snapshot's main thread.
  // The boot creates the main thread under one handle. Handles the title
  // opened to it later (a duplicated thread handle) come from the snapshot.
  if (fresh_main->handles_.size() != 1 ||
      std::find(main_record->handles.begin(), main_record->handles.end(),
                fresh_main->handles_[0]) == main_record->handles.end()) {
    error = fmt::format(
        "boot divergence: main thread handle {:08X} ({} handles), snapshot "
        "{:08X} ({} handles)",
        fresh_main->handles_.empty() ? 0 : fresh_main->handles_[0],
        fresh_main->handles_.size(), main_record->handles[0],
        main_record->handles.size());
    return false;
  }
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    for (uint32_t handle : main_record->handles) {
      if (handle == fresh_main->handles_[0]) continue;
      const uint32_t slot = (handle & ~XObject::kHandleBase) >> 2;
      if (slot >= table.table_capacity_ &&
          !table.Resize(std::max(img.table_capacity, slot + 1), false)) {
        error = "cannot grow the kernel object table";
        return false;
      }
      if (table.table_[slot].object) {
        error = fmt::format("main thread handle {:08X} is already in use",
                            handle);
        return false;
      }
      table.table_[slot].object = fresh_main;
      fresh_main->Retain();
    }
    fresh_main->handles_ = main_record->handles;
  }
  // Its KTHREAD, PCR, TLS and stack live in guest memory, which is the
  // snapshot's now (heaps and contents): take the saved addresses, as the
  // shell threads below do. The fresh boot's own allocations are gone.
  if (fresh_main->guest_object_ptr_ != main_record->guest_object ||
      fresh_main->pcr_address_ != main_record->pcr ||
      fresh_main->tls_static_address_ != main_record->tls_static ||
      fresh_main->stack_base_ != main_record->stack_base ||
      fresh_main->stack_limit_ != main_record->stack_limit) {
    XELOGI(
        "KernelSnapshot: main thread rebased: kthread {:08X}->{:08X} pcr "
        "{:08X}->{:08X} tls {:08X}->{:08X} stack {:08X}-{:08X}->{:08X}-{:08X}",
        fresh_main->guest_object_ptr_, main_record->guest_object,
        fresh_main->pcr_address_, main_record->pcr,
        fresh_main->tls_static_address_, main_record->tls_static,
        fresh_main->stack_limit_, fresh_main->stack_base_,
        main_record->stack_limit, main_record->stack_base);
  }
  fresh_main->guest_object_ptr_ = main_record->guest_object;
  fresh_main->tls_static_address_ = main_record->tls_static;
  fresh_main->tls_dynamic_address_ = main_record->tls_dynamic;
  fresh_main->tls_total_size_ = main_record->tls_total;
  fresh_main->pcr_address_ = main_record->pcr;
  fresh_main->stack_alloc_base_ = main_record->stack_alloc_base;
  fresh_main->stack_alloc_size_ = main_record->stack_alloc_size;
  fresh_main->stack_base_ = main_record->stack_base;
  fresh_main->stack_limit_ = main_record->stack_limit;
  if (fresh_main->fiber_) {
    // Never ran: just a prepared stack and entry. Balance the Retain() that
    // XThread::Create() took for the fiber.
    fresh_main->fiber_.reset();
    fresh_main->fiber_exit_event_.reset();
    fresh_main->Release();
  }
  // Thread ids are process-global and keep increasing across in-process
  // relaunch. Rebind the never-run main thread rather than require its new
  // host allocation id to match the saved guest id.
  ks->UnregisterThread(fresh_main);
  // The processor's thread registry is keyed by id too: move the entry, or
  // destroying the thread later looks up an id it never saw.
  ks->processor()->OnThreadDestroyed(fresh_main->thread_id_);
  delete fresh_main->thread_state_;
  fresh_main->thread_state_ = nullptr;
  fresh_main->thread_id_ = main_record->thread_id;
  ks->RegisterThread(fresh_main);
  if (!ApplyThreadRecord(ks, fresh_main, *main_record, error)) {
    return false;
  }
  ks->processor()->OnThreadCreated(fresh_main->handle(),
                                   fresh_main->thread_state(), fresh_main);
  fresh_main->allocated_guest_object_ = main_record->allocated_guest_object != 0;

  // 3. Table geometry.
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    if (table.table_capacity_ < img.table_capacity &&
        !table.Resize(img.table_capacity, false)) {
      error = "cannot grow the kernel object table";
      return false;
    }
    table.last_free_entry_ = img.table_cursor;
  }

  // 4. Other threads (shells, registered under their saved handles).
  auto place = [ks](XObject* object, const std::vector<uint32_t>& handles) {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    for (uint32_t handle : handles) {
      const uint32_t slot = (handle & ~XObject::kHandleBase) >> 2;
      if (slot >= table.table_capacity_ || table.table_[slot].object) {
        return false;
      }
      table.table_[slot].object = object;
      object->Retain();
    }
    object->handles_ = handles;
    return true;
  };
  for (const auto& record : img.threads) {
    if (&record == main_record) {
      continue;
    }
    auto* thread = new XThread(nullptr);
    object_ref<XThread> holder(thread);
    thread->kernel_state_ = ks;
    thread->allocated_guest_object_ = record.allocated_guest_object != 0;
    thread->guest_object_ptr_ = record.guest_object;
    thread->thread_id_ = record.thread_id;
    thread->tls_static_address_ = record.tls_static;
    thread->tls_dynamic_address_ = record.tls_dynamic;
    thread->tls_total_size_ = record.tls_total;
    thread->pcr_address_ = record.pcr;
    thread->stack_alloc_base_ = record.stack_alloc_base;
    thread->stack_alloc_size_ = record.stack_alloc_size;
    thread->stack_base_ = record.stack_base;
    thread->stack_limit_ = record.stack_limit;
    if (!place(thread, record.handles)) {
      error = fmt::format("thread handle {:08X} is already in use",
                          record.handles[0]);
      return false;
    }
    ks->RegisterThread(thread);
    if (!ApplyThreadRecord(ks, thread, record, error)) {
      return false;
    }
    restored.push_back(std::move(holder));
  }

  // 5. Plain objects through the legacy restore functions, each over a
  //    stream bounded to its own payload that must be consumed exactly.
  for (const auto& record : img.objects) {
    std::vector<uint8_t> payload = record.payload;
    ByteStream stream(payload.data(), payload.size());
    object_ref<XObject> object;
    switch (static_cast<XObject::Type>(record.type)) {
      case XObject::Type::Enumerator:
        object = XEnumerator::Restore(ks, &stream);
        break;
      case XObject::Type::Event:
        object = XEvent::Restore(ks, &stream);
        break;
      case XObject::Type::File:
        object = XFile::Restore(ks, &stream);
        break;
      case XObject::Type::IOCompletion:
        object = XIOCompletion::Restore(ks, &stream);
        break;
      case XObject::Type::Mutant:
        object = XMutant::RestoreRuntime(ks, &stream);
        break;
      case XObject::Type::NotifyListener:
        object = XNotifyListener::Restore(ks, &stream);
        break;
      case XObject::Type::Semaphore:
        object = XSemaphore::Restore(ks, &stream);
        break;
      case XObject::Type::Socket:
        object = XSocket::RestoreRuntime(ks, &stream);
        break;
      case XObject::Type::SymbolicLink:
        object = XSymbolicLink::Restore(ks, &stream);
        break;
      case XObject::Type::Timer:
        object = XTimer::RestoreRuntime(ks, &stream);
        break;
      default:
        break;
    }
    if (!object || stream.failed() || stream.offset() != payload.size()) {
      error = fmt::format("cannot restore a {} object",
                          TypeName(static_cast<XObject::Type>(record.type)));
      return false;
    }
    if (record.type == uint32_t(XObject::Type::File) &&
        !static_cast<XFile*>(object.get())->file()) {
      error = "a file of the snapshot could not be reopened";
      return false;
    }
    if (!record.name.empty()) {
      object->name_ = record.name;
      ks->object_table()->AddNameMapping(record.name, object->handle());
    }
  }

  // 6. Handle reference counts, then verify that every slot has its object.
  {
    auto& table = ks->object_table_;
    auto lock = table.global_critical_region_.Acquire();
    for (const auto& slot_record : img.slots) {
      const uint32_t slot = (slot_record.handle & ~XObject::kHandleBase) >> 2;
      if (slot >= table.table_capacity_ || !table.table_[slot].object) {
        error = fmt::format("handle {:08X} has no restored object",
                            slot_record.handle);
        return false;
      }
      table.table_[slot].handle_ref_count = slot_record.ref_count;
    }
  }

  // 7. Runtime VFS symlinks and the thread id counter.
  for (const auto& link : img.symlinks) {
    std::string existing;
    if (ks->file_system()->IsSymbolicLinkRegistered(link.first)) {
      if (ks->file_system()->FindSymbolicLink(link.first, existing) &&
          existing != link.second) {
        error = fmt::format("VFS link {} points elsewhere after boot",
                            link.first);
        return false;
      }
      continue;
    }
    if (!ks->file_system()->RegisterSymbolicLink(link.first, link.second)) {
      error = fmt::format("cannot register VFS link {}", link.first);
      return false;
    }
  }
  next_xthread_id_ = std::max(next_xthread_id_, img.next_thread_id);

  // 8. Fibers, last: they retain their thread and only start executing when
  //    ResumeThreads() queues them.
  for (auto& thread : restored) {
    const KernelThreadRecord* record = nullptr;
    for (const auto& r : img.threads) {
      if (r.thread_id == thread->thread_id_) record = &r;
    }
    if (!thread->CreateRestoredFiber(record->resume_pc)) {
      error = fmt::format("cannot create the fiber of thread {:08X}",
                          thread->thread_id_);
      return false;
    }
    ks->processor()->OnThreadCreated(
        thread->handle(), thread->thread_state(), thread.get());
  }
  if (!fresh_main->CreateRestoredFiber(main_record->resume_pc)) {
    error = "cannot create the fiber of the main thread";
    return false;
  }
  restored.push_back(retain_object(fresh_main));
  return true;
}

bool KernelSnapshot::ResumeThreads(std::vector<object_ref<XThread>>& restored,
                                   std::string& error) {
  // Validate and refresh every thread before making the first one runnable.
  // A failed precondition must not leave a partially resumed title.
  for (auto& thread : restored) {
    if (!thread->fiber_ || !thread->RefreshRestoredHostPointers()) {
      error = fmt::format("cannot resume thread {:08X}", thread->thread_id_);
      return false;
    }
  }
  for (auto& thread : restored)
    thread->kernel_state()->guest_scheduler()->MarkReady(thread.get());
  restored.clear();
  return true;
}

}  // namespace kernel
}  // namespace xe
