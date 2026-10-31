#ifndef XENIA_SNAPSHOT_KERNEL_MEMORY_VALIDATION_H_
#define XENIA_SNAPSHOT_KERNEL_MEMORY_VALIDATION_H_
#include "xenia/snapshot/kernel_format.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xsemaphore.h"
namespace xe::snapshot {
// Called after RAM apply but before constructing/registering any restored
// object. Reject invalid pointers before they reach Resume or rollback teardown.
// The callback validates the actual restored heap metadata, not live pointers.
// Object bodies only need to be committed: the host kernel writes them, and a
// game may keep a static one in a read-only section (Halo Reach semaphore).
template <typename WritableRange, typename CommittedRange>
bool ValidateKernelGuestMemory(const KernelImage& image, WritableRange writable,
                              CommittedRange committed, std::string& error) {
  auto check = [&](uint32_t address, uint64_t length, const char* label,
                   bool host_written = false) {
    if (!address || !length || uint64_t(address) + length > (uint64_t(1) << 32) ||
        !(host_written ? committed(address, length) : writable(address, length))) {
      error = std::string(label) + " (" + std::to_string(address) + "+" +
              std::to_string(length) + ")";
      return false;
    }
    return true;
  };
  const uint32_t prepad = sizeof(kernel::X_OBJECT_HEADER) + kernel::kGuestObjectPrePad;
  for (const auto& thread : image.threads) {
    if (!check(thread.guest_object, sizeof(kernel::X_KTHREAD), "thread guest object is not committed writable") ||
        !check(thread.pcr, sizeof(kernel::X_KPCR), "thread PCR is not committed writable") ||
        !check(thread.tls_static, thread.tls_total, "thread TLS is not committed writable") ||
        !check(thread.stack_limit, uint64_t(thread.stack_base) - thread.stack_limit,
               "thread stack is not committed writable")) return false;
    if (thread.tls_dynamic && !check(thread.tls_dynamic, 256, "thread dynamic TLS is not committed writable")) return false;
    if (thread.allocated_guest_object &&
        (thread.guest_object <= prepad || !check(thread.guest_object - prepad,
             uint64_t(prepad) + sizeof(kernel::X_KTHREAD), "thread object allocation header is invalid"))) return false;
  }
  for (const auto& object : image.objects) {
    ObjectPayloadHeader header;
    if (!ParseObjectPayloadHeader(object.payload, header)) { error = "invalid object header"; return false; }
    uint32_t native_size = sizeof(kernel::X_DISPATCH_HEADER);
    if (object.type == kObjMutant) native_size = sizeof(kernel::X_KMUTANT);
    if (object.type == kObjSemaphore) native_size = sizeof(kernel::X_KSEMAPHORE);
    if (header.guest_object && !check(header.guest_object, native_size, "object guest body is not committed", true)) {
      error += " type " + std::to_string(object.type);
      return false;
    }
    if (header.allocated_guest_object &&
        (header.guest_object <= prepad || !check(header.guest_object - prepad,
             uint64_t(prepad) + native_size, "object allocation header is invalid", true))) return false;
  }
  return true;
}
}
#endif
