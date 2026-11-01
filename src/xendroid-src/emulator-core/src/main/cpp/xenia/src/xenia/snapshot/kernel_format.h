// XenDroid runtime snapshot: kernel section (typed, bounded, fully validated).
//
// Pure data + (de)serialisation, no dependency on kernel classes, so it can be
// fuzzed on the host. The runtime side (emulator_snapshot.cc) fills / consumes
// it. Object payloads are the legacy XObject::Save() bytes, but they are only
// ever parsed through a length-bounded stream and only for the whitelisted
// types below; Validate() proves structure before anything is torn down.
#ifndef XENIA_SNAPSHOT_KERNEL_FORMAT_H_
#define XENIA_SNAPSHOT_KERNEL_FORMAT_H_
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "xenia/snapshot/ppc_state.h"
#include "xenia/snapshot/state_file.h"
#include "xenia/snapshot/socket_state.h"
namespace xe::snapshot {

// Mirrors XObject::Type (asserted in emulator_snapshot.cc).
enum KernelObjectType : uint32_t {
  kObjEnumerator = 1,
  kObjEvent = 2,
  kObjFile = 3,
  kObjIOCompletion = 4,
  kObjModule = 5,
  kObjMutant = 6,
  kObjNotifyListener = 7,
  kObjSemaphore = 8,
  kObjSocket = 10,
  kObjSymbolicLink = 11,
  kObjThread = 12,
  kObjTimer = 13,
};
constexpr uint32_t kGuestHandleBase = 0xF8000000u;  // XObject::kHandleBase
constexpr uint32_t kHostHandleBase = 0x01000000u;   // XObject::kHandleHostBase
constexpr uint32_t kMaxKernelTableCapacity = 1u << 22;
constexpr uint32_t kMaxKernelObjects = 1u << 18;
constexpr uint32_t kMaxKernelThreads = 4096;
constexpr uint32_t kMaxObjectHandles = 32;
constexpr uint32_t kMaxObjectPayload = 1u << 20;
constexpr uint32_t kMaxKernelString = 4096;
constexpr uint32_t kMaxKernelSymlinks = 1024;
constexpr uint32_t kMaxKernelTrampolines = 4096;
constexpr int32_t kMaxHandleRefCount = 1 << 24;

enum ThreadResumeKind : uint32_t {
  // Parked at a JIT block head: continue at resume_pc.
  kResumeCanonicalPc = 1,
  // Blocked in a restartable kernel wait: resume_pc is the import thunk, the
  // wait is simply executed again with the saved argument registers.
  kResumeReenterExtern = 2,
  // Already exited, kept alive by handles the title holds: restored as an
  // object only, never resumed (resume_pc is 0).
  kResumeExited = 3,
};

struct KernelSlot {
  uint32_t handle = 0;
  int32_t ref_count = 0;
  bool operator==(const KernelSlot&) const = default;
};

// Modules. The cold boot recreates those loaded at launch; the restore loads
// the ones the title loaded later (XexLoadImage) and rebinds them to their
// saved handle, loader entry and guest header (version 2; 0 in version 1).
struct KernelBootObject {
  uint32_t handle = 0;
  uint32_t type = 0;
  uint32_t guest_object = 0;
  std::string name;
  uint32_t hmodule = 0;
  uint32_t xex_header = 0;
  bool operator==(const KernelBootObject&) const = default;
};

// A kernel module's GetProcAddress trampoline (v3). Trampolines are generated
// lazily when the title asks for an export, so a fresh boot has none of them.
struct KernelTrampoline {
  std::string module;
  uint32_t ordinal = 0;
  uint32_t address = 0;
  bool operator==(const KernelTrampoline&) const = default;
};

struct KernelThreadRecord {
  uint32_t thread_id = 0;
  uint32_t main_thread = 0;
  uint32_t allocated_guest_object = 0;
  uint32_t guest_object = 0;
  std::vector<uint32_t> handles;
  std::string name;
  uint32_t tls_static = 0, tls_dynamic = 0, tls_total = 0, pcr = 0;
  uint32_t stack_alloc_base = 0, stack_alloc_size = 0;
  uint32_t stack_base = 0, stack_limit = 0;
  int32_t priority = 0, base_priority = 0, boost = 0;
  uint32_t creation_stack_size = 0, xapi_startup = 0, start_address = 0;
  uint32_t start_context = 0, creation_flags = 0, guest_process = 0;
  uint32_t resume_kind = 0, resume_pc = 0;
  std::vector<uint8_t> cpu_state;  // PpcState::Encode()
};

struct KernelObjectRecord {
  uint32_t type = 0;
  std::string name;               // XObject name (named-object table)
  std::vector<uint8_t> payload;   // legacy XObject::Save() bytes
};

// Header of every legacy payload (XObject::SaveObject): parsed, never trusted.
struct ObjectPayloadHeader {
  uint32_t allocated_guest_object = 0;
  uint32_t guest_object = 0;
  std::vector<uint32_t> handles;
};
inline bool ParseObjectPayloadHeader(std::span<const uint8_t> payload,
                                     ObjectPayloadHeader& out,
                                     size_t* consumed = nullptr) {
  Reader r(payload);
  ObjectPayloadHeader h;
  uint32_t count;
  if (!r.U32(h.allocated_guest_object) || !r.U32(h.guest_object) ||
      !r.U32(count) || count == 0 || count > kMaxObjectHandles ||
      uint64_t(count) * 4 > r.remaining()) {
    return false;
  }
  h.handles.resize(count);
  for (auto& handle : h.handles) {
    if (!r.U32(handle)) return false;
  }
  if (consumed) *consumed = r.offset();
  out = std::move(h);
  return true;
}

// Strict parser of the legacy per-type payload tail (after the object header).
// The runtime later feeds the same bytes to the legacy Restore() functions,
// so everything those would read must be provably in range here. On success
// |mutant_owner| is the thread handle the object refers to: the owner of a
// mutant, the APC thread of a timer (else 0).
inline bool ValidateLegacyPayloadTail(uint32_t type,
                                      std::span<const uint8_t> tail,
                                      uint32_t& mutant_owner) {
  Reader r(tail);
  mutant_owner = 0;
  auto flag = [&r](uint32_t& v) {
    uint8_t b[1];
    if (!r.Bytes(b) || b[0] > 1) return false;
    v = b[0];
    return true;
  };
  auto str = [&r]() {
    uint32_t n;
    return r.U32(n) && n <= kMaxKernelString && r.Skip(n);
  };
  switch (type) {
    case kObjEnumerator: {
      // XEnumerator saves: kind, layout, then items (raw for kind 0 and the
      // XMP playlists 2, the achievement fields and three strings each for 1).
      uint32_t kind, per, item_size, extra, count, current;
      if (!r.U32(kind) || !r.U32(per) || !r.U32(item_size) || !r.U32(extra))
        return false;
      if (kind == 0 || kind == 2) {
        uint32_t bytes;
        if (!r.U32(count) || !r.U32(current) || !r.U32(bytes) ||
            current > count || uint64_t(count) * item_size != bytes ||
            bytes != r.remaining() || !r.Skip(bytes))
          return false;
      } else if (kind == 1) {
        uint32_t flags, field;
        if (!r.U32(flags) || !r.U32(count) || !r.U32(current) ||
            count > kMaxKernelString)
          return false;
        auto u16str = [&r]() {
          uint32_t n;
          return r.U32(n) && n <= kMaxKernelString && r.Skip(uint64_t(n) * 2);
        };
        for (uint32_t i = 0; i < count; ++i) {
          for (int f = 0; f < 6; ++f)
            if (!r.U32(field)) return false;
          if (!u16str() || !u16str() || !u16str()) return false;
        }
      } else {
        return false;
      }
      break;
    }
    case kObjSocket: {
      uint32_t size; SocketState state;
      if (!r.U32(size) || size > 20000 || size != r.remaining() ||
          !SocketState::Decode(tail.subspan(4), state)) return false;
      return true;
    }
    case kObjTimer: {
      uint32_t type, armed, signaled, period, routine, routine_arg, thread;
      uint64_t due;
      if (!r.U32(type) || type > 1 || !flag(armed) || !flag(signaled) ||
          !r.U64(due) || !r.U32(period) || !r.U32(routine) ||
          !r.U32(routine_arg) || !r.U32(thread))
        return false;
      // A disarmed timer keeps no schedule; only an armed one has an APC.
      if (!armed && (due || period || routine || routine_arg || thread))
        return false;
      if ((routine != 0) != (thread != 0) || (routine & 3)) return false;
      mutant_owner = thread;
      break;
    }
    case kObjEvent: {
      uint32_t signaled, manual;
      if (!flag(signaled) || !flag(manual)) return false;
      break;
    }
    case kObjSemaphore: {
      uint32_t maximum, free_count;
      if (!r.U32(maximum) || !r.U32(free_count) || !maximum ||
          maximum > 0x7FFFFFFFu || free_count > maximum)
        return false;
      break;
    }
    case kObjMutant: {
      uint32_t owner, recursion;
      if (!r.U32(owner) || !r.U32(recursion) || (owner != 0) != (recursion != 0) ||
          recursion > 65536)
        return false;
      mutant_owner = owner;
      break;
    }
    case kObjNotifyListener: {
      uint64_t mask, count;
      uint32_t is_system, max_version;
      if (!r.U64(mask) || !r.U32(is_system) || !r.U32(max_version) ||
          !r.U64(count) || count > 4096 || count * 8 > r.remaining())
        return false;
      if (!r.Skip(size_t(count) * 8)) return false;
      break;
    }
    case kObjIOCompletion: {
      // Queued notifications: key context, APC context, status, byte count.
      uint32_t count;
      if (!r.U32(count) || count > 1024 || uint64_t(count) * 16 != r.remaining())
        return false;
      if (!r.Skip(size_t(count) * 16)) return false;
      break;
    }
    case kObjSymbolicLink:
      if (!str() || !str()) return false;
      break;
    case kObjFile: {
      uint64_t position;
      uint32_t access, is_directory, is_synchronous;
      if (!str() || !r.U64(position) || !r.U32(access) ||
          !flag(is_directory) || !flag(is_synchronous))
        return false;
      // Optional (XFile::Save): deleted while open, then its contents.
      if (r.remaining()) {
        uint32_t deleted;
        uint64_t size;
        if (!flag(deleted)) return false;
        if (deleted &&
            (!r.U64(size) || size != r.remaining() || !r.Skip(size_t(size))))
          return false;
      }
      break;
    }
    default:
      return false;
  }
  return r.remaining() == 0;
}

struct KernelImage {
  // Boot determinism fingerprint, compared with the fresh cold boot.
  uint32_t kernel_guest_globals = 0;
  uint32_t timestamp_bundle = 0;
  uint32_t next_thread_id = 0;
  uint32_t table_capacity = 0;  // guest object table capacity (entries)
  uint32_t table_cursor = 0;    // allocation cursor (last_free_entry_)
  std::vector<KernelBootObject> boot_objects;
  std::vector<KernelSlot> slots;
  std::vector<KernelThreadRecord> threads;
  std::vector<KernelObjectRecord> objects;
  std::vector<std::pair<std::string, std::string>> symlinks;
  std::vector<KernelTrampoline> trampolines;  // v3, by module then address

  static void PutString(std::vector<uint8_t>& out, const std::string& s) {
    Put32(out, uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
  static bool GetString(Reader& r, std::string& s) {
    uint32_t n;
    if (!r.U32(n) || n > kMaxKernelString || n > r.remaining()) return false;
    s.resize(n);
    return n == 0 || r.Bytes(std::span<uint8_t>(
                         reinterpret_cast<uint8_t*>(s.data()), n));
  }
  static void PutBytes(std::vector<uint8_t>& out,
                       const std::vector<uint8_t>& b) {
    Put32(out, uint32_t(b.size()));
    out.insert(out.end(), b.begin(), b.end());
  }
  static bool GetBytes(Reader& r, uint32_t limit, std::vector<uint8_t>& b) {
    uint32_t n;
    if (!r.U32(n) || n > limit || n > r.remaining()) return false;
    b.resize(n);
    return n == 0 || r.Bytes(b);
  }

  std::vector<uint8_t> Encode() const {
    std::vector<uint8_t> out;
    Put32(out, 3);  // kernel section version
    for (uint32_t v : {kernel_guest_globals, timestamp_bundle, next_thread_id,
                       table_capacity, table_cursor})
      Put32(out, v);
    Put32(out, uint32_t(boot_objects.size()));
    for (const auto& o : boot_objects) {
      Put32(out, o.handle);
      Put32(out, o.type);
      Put32(out, o.guest_object);
      PutString(out, o.name);
      Put32(out, o.hmodule);
      Put32(out, o.xex_header);
    }
    Put32(out, uint32_t(slots.size()));
    for (const auto& s : slots) {
      Put32(out, s.handle);
      Put32(out, uint32_t(s.ref_count));
    }
    Put32(out, uint32_t(threads.size()));
    for (const auto& t : threads) {
      for (uint32_t v :
           {t.thread_id, t.main_thread, t.allocated_guest_object,
            t.guest_object})
        Put32(out, v);
      Put32(out, uint32_t(t.handles.size()));
      for (uint32_t h : t.handles) Put32(out, h);
      PutString(out, t.name);
      for (uint32_t v :
           {t.tls_static, t.tls_dynamic, t.tls_total, t.pcr, t.stack_alloc_base,
            t.stack_alloc_size, t.stack_base, t.stack_limit})
        Put32(out, v);
      for (int32_t v : {t.priority, t.base_priority, t.boost})
        Put32(out, uint32_t(v));
      for (uint32_t v :
           {t.creation_stack_size, t.xapi_startup, t.start_address,
            t.start_context, t.creation_flags, t.guest_process, t.resume_kind,
            t.resume_pc})
        Put32(out, v);
      PutBytes(out, t.cpu_state);
    }
    Put32(out, uint32_t(objects.size()));
    for (const auto& o : objects) {
      Put32(out, o.type);
      PutString(out, o.name);
      PutBytes(out, o.payload);
    }
    Put32(out, uint32_t(symlinks.size()));
    for (const auto& l : symlinks) {
      PutString(out, l.first);
      PutString(out, l.second);
    }
    Put32(out, uint32_t(trampolines.size()));
    for (const auto& t : trampolines) {
      PutString(out, t.module);
      Put32(out, t.ordinal);
      Put32(out, t.address);
    }
    return out;
  }

  // Structural decode only (every length bounded by remaining bytes and a
  // hard cap). Call Validate() afterwards for the semantic checks.
  static bool Decode(std::span<const uint8_t> bytes, KernelImage& out) {
    Reader r(bytes);
    KernelImage img;
    uint32_t version, n;
    if (!r.U32(version) || version < 1 || version > 3) return false;
    for (uint32_t* v : {&img.kernel_guest_globals, &img.timestamp_bundle,
                        &img.next_thread_id, &img.table_capacity,
                        &img.table_cursor})
      if (!r.U32(*v)) return false;
    // Smallest possible encodings bound the counts before any allocation.
    if (!r.U32(n) || n > kMaxKernelObjects || uint64_t(n) * 16 > r.remaining())
      return false;
    img.boot_objects.resize(n);
    for (auto& o : img.boot_objects) {
      if (!r.U32(o.handle) || !r.U32(o.type) || !r.U32(o.guest_object) ||
          !GetString(r, o.name))
        return false;
      if (version >= 2 && (!r.U32(o.hmodule) || !r.U32(o.xex_header)))
        return false;
    }
    if (!r.U32(n) || n > kMaxKernelTableCapacity ||
        uint64_t(n) * 8 > r.remaining())
      return false;
    img.slots.resize(n);
    for (auto& s : img.slots) {
      uint32_t rc;
      if (!r.U32(s.handle) || !r.U32(rc)) return false;
      s.ref_count = int32_t(rc);
    }
    if (!r.U32(n) || n > kMaxKernelThreads || uint64_t(n) * 60 > r.remaining())
      return false;
    img.threads.resize(n);
    for (auto& t : img.threads) {
      uint32_t hcount;
      if (!r.U32(t.thread_id) || !r.U32(t.main_thread) ||
          !r.U32(t.allocated_guest_object) || !r.U32(t.guest_object) ||
          !r.U32(hcount) || hcount > kMaxObjectHandles ||
          uint64_t(hcount) * 4 > r.remaining())
        return false;
      t.handles.resize(hcount);
      for (auto& h : t.handles)
        if (!r.U32(h)) return false;
      if (!GetString(r, t.name)) return false;
      for (uint32_t* v : {&t.tls_static, &t.tls_dynamic, &t.tls_total, &t.pcr,
                          &t.stack_alloc_base, &t.stack_alloc_size,
                          &t.stack_base, &t.stack_limit})
        if (!r.U32(*v)) return false;
      for (int32_t* v : {&t.priority, &t.base_priority, &t.boost}) {
        uint32_t raw;
        if (!r.U32(raw)) return false;
        *v = int32_t(raw);
      }
      for (uint32_t* v :
           {&t.creation_stack_size, &t.xapi_startup, &t.start_address,
            &t.start_context, &t.creation_flags, &t.guest_process,
            &t.resume_kind, &t.resume_pc})
        if (!r.U32(*v)) return false;
      if (!GetBytes(r, 1u << 16, t.cpu_state)) return false;
    }
    if (!r.U32(n) || n > kMaxKernelObjects || uint64_t(n) * 12 > r.remaining())
      return false;
    img.objects.resize(n);
    for (auto& o : img.objects) {
      if (!r.U32(o.type) || !GetString(r, o.name) ||
          !GetBytes(r, kMaxObjectPayload, o.payload))
        return false;
    }
    if (!r.U32(n) || n > kMaxKernelSymlinks || uint64_t(n) * 8 > r.remaining())
      return false;
    img.symlinks.resize(n);
    for (auto& l : img.symlinks)
      if (!GetString(r, l.first) || !GetString(r, l.second)) return false;
    if (version >= 3) {
      if (!r.U32(n) || n > kMaxKernelTrampolines ||
          uint64_t(n) * 12 > r.remaining())
        return false;
      img.trampolines.resize(n);
      for (auto& t : img.trampolines)
        if (!GetString(r, t.module) || !r.U32(t.ordinal) || !r.U32(t.address))
          return false;
    }
    if (r.remaining()) return false;
    out = std::move(img);
    return true;
  }

  // Production preflight must validate semantics before publishing an image.
  // Decode alone only establishes structural bounds and accepts invalid fields.
  static bool DecodeValidated(std::span<const uint8_t> bytes, KernelImage& out,
                              std::string& error) {
    KernelImage decoded;
    if (!Decode(bytes, decoded)) { error = "kernel section decoding failed"; return false; }
    if (!decoded.Validate(error)) return false;
    out = std::move(decoded);
    return true;
  }

  static bool IsGuestHandle(uint32_t handle, uint32_t capacity) {
    if (handle < kGuestHandleBase || (handle & 3)) return false;
    return ((handle & ~kGuestHandleBase) >> 2) < capacity;
  }
  static bool AllowedObjectType(uint32_t type) {
    switch (type) {
      case kObjEnumerator:
      case kObjEvent:
      case kObjFile:
      case kObjIOCompletion:
      case kObjMutant:
      case kObjNotifyListener:
      case kObjSemaphore:
      case kObjSocket:
      case kObjSymbolicLink:
      case kObjTimer:
        return true;
      default:
        return false;
    }
  }

  // Semantic validation, no side effects. |error| names the first problem.
  bool Validate(std::string& error) const {
    auto bad = [&error](const char* msg) {
      error = msg;
      return false;
    };
    if (!table_capacity || table_capacity > kMaxKernelTableCapacity)
      return bad("kernel table capacity out of range");
    if (table_cursor > table_capacity) return bad("kernel table cursor");
    if (threads.empty()) return bad("snapshot has no guest thread");
    std::set<uint32_t> slot_handles;
    for (const auto& s : slots) {
      if (!IsGuestHandle(s.handle, table_capacity))
        return bad("slot handle out of table");
      if (s.ref_count < 0 || s.ref_count > kMaxHandleRefCount)
        return bad("slot reference count out of range");
      if (!slot_handles.insert(s.handle).second)
        return bad("duplicate slot handle");
    }
    std::set<uint32_t> owned;  // handles owned by some record
    auto claim = [&](uint32_t handle) {
      return IsGuestHandle(handle, table_capacity) &&
             slot_handles.count(handle) && owned.insert(handle).second;
    };
    for (const auto& b : boot_objects) {
      if (!claim(b.handle)) return bad("boot object handle");
      if (b.type != kObjModule) return bad("boot object type");
    }
    std::set<uint32_t> thread_ids, thread_handles;
    uint32_t main_count = 0, max_id = 0;
    for (const auto& t : threads) {
      if (!t.thread_id || !thread_ids.insert(t.thread_id).second)
        return bad("thread id");
      max_id = std::max(max_id, t.thread_id);
      if (t.main_thread > 1) return bad("thread main flag");
      main_count += t.main_thread;
      if (t.handles.empty() || t.handles.size() > kMaxObjectHandles)
        return bad("thread handle list");
      for (uint32_t h : t.handles) {
        if (!claim(h)) return bad("thread handle");
        thread_handles.insert(h);
      }
      if (!t.guest_object || (t.guest_object & 3)) return bad("thread kthread");
      if (!t.pcr || !t.tls_static || !t.stack_base || !t.stack_limit ||
          t.stack_limit >= t.stack_base)
        return bad("thread stack/pcr/tls");
      if (t.resume_kind != kResumeCanonicalPc &&
          t.resume_kind != kResumeReenterExtern &&
          t.resume_kind != kResumeExited)
        return bad("thread resume kind");
      if (t.resume_kind == kResumeExited ? t.resume_pc != 0 || t.main_thread
                                         : !t.resume_pc || (t.resume_pc & 3))
        return bad("thread resume pc");
      PpcState cpu;
      if (!PpcState::Decode(t.cpu_state, cpu) || cpu.thread_id != t.thread_id)
        return bad("thread cpu state");
    }
    if (main_count != 1) return bad("snapshot needs exactly one main thread");
    if (next_thread_id < max_id) return bad("next thread id");
    for (const auto& o : objects) {
      if (!AllowedObjectType(o.type)) return bad("unsupported object type");
      ObjectPayloadHeader h;
      size_t header_size = 0;
      if (!ParseObjectPayloadHeader(o.payload, h, &header_size))
        return bad("object payload header");
      if (h.allocated_guest_object > 1) return bad("object allocation flag");
      uint32_t owner = 0;
      if (!ValidateLegacyPayloadTail(
              o.type, std::span<const uint8_t>(o.payload).subspan(header_size),
              owner)) {
        error = "object payload body (type " + std::to_string(o.type) + ")";
        return false;
      }
      if (owner && !thread_handles.count(owner))
        return bad("referenced thread is not a snapshot thread");
      for (uint32_t handle : h.handles) {
        // Socket handles are guest-visible raw slot offsets. The table
        // inventory uses canonical handles for every slot; claim that same
        // slot without changing the raw handle restored to the guest.
        if (o.type == kObjSocket) {
          if ((handle & 3) || (handle >> 2) >= table_capacity)
            return bad("socket handle");
          handle |= kGuestHandleBase;
        }
        if (!claim(handle)) return bad("object handle");
      }
    }
    if (owned.size() != slot_handles.size()) return bad("orphan kernel slot");
    if (owned.size() > kMaxKernelObjects) return bad("too many objects");
    for (const auto& l : symlinks)
      if (l.first.empty() || l.second.empty()) return bad("empty symlink");
    for (size_t i = 0; i < trampolines.size(); ++i) {
      const auto& t = trampolines[i];
      if (t.module.empty() || t.ordinal > 0xFFFF || (t.address & 7) ||
          (i && t.module == trampolines[i - 1].module &&
           t.address <= trampolines[i - 1].address))
        return bad("kernel trampoline");
    }
    return true;
  }
};

}  // namespace xe::snapshot
#endif
