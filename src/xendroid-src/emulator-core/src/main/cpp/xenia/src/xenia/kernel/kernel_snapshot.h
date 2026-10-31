/**
 ******************************************************************************
 * XenDroid runtime save state: kernel section capture / cold restore.        *
 ******************************************************************************
 */
#ifndef XENIA_KERNEL_KERNEL_SNAPSHOT_H_
#define XENIA_KERNEL_KERNEL_SNAPSHOT_H_

#include <string>
#include <vector>

#include "xenia/kernel/xobject.h"
#include "xenia/snapshot/kernel_format.h"

namespace xe {
namespace kernel {

class KernelState;
class XThread;

// Everything here runs with the snapshot barrier held (no guest or host
// writer active) and never changes the state of the running title on capture.
class KernelSnapshot {
 public:
  // Capability check for objects whose snapshot support depends on how the
  // title used them (sockets); |reason| names the first refusal.
  static bool ObjectsSnapshottable(KernelState* kernel_state,
                                   std::string& reason);

  // Captures handle table, guest threads (canonical CPU + resume point),
  // whitelisted objects and runtime VFS symlinks. |threads| are the
  // scheduler barrier's threads, every guest thread of the title must be in
  // it. Fails (with a reason in |error|) for anything not representable:
  // enumerators, TCP or busy sockets, open content packages, native frames...
  static bool Capture(KernelState* kernel_state,
                      const std::vector<XThread*>& threads,
                      snapshot::KernelImage& out, std::string& error);

  // Loads the modules of |image| that the fresh boot lacks (the title loaded
  // them with XexLoadImage after launch), as XexLoadImage does but without
  // DllMain: their memory comes from the snapshot. Must run BEFORE guest RAM
  // is restored, since loading writes the module image.
  static bool LoadRuntimeModules(KernelState* kernel_state,
                                 const snapshot::KernelImage& image,
                                 std::string& error);

  // Rebuilds the kernel state inside a freshly booted kernel whose only
  // guest thread is |fresh_main| (created suspended, never run). Guest RAM
  // must already be restored. Restored threads are created but NOT queued;
  // they are appended to |restored| and started by ResumeThreads().
  static bool Restore(KernelState* kernel_state, XThread* fresh_main,
                      const snapshot::KernelImage& image,
                      std::vector<object_ref<XThread>>& restored,
                      std::string& error);

  // Makes the restored threads runnable (call once everything else is ready).
  static bool ResumeThreads(std::vector<object_ref<XThread>>& restored,
                            std::string& error);

 private:
  static bool FindBlockedThunk(KernelState* ks, XThread* thread,
                               uint32_t& thunk_out, std::string& error);
  static bool ApplyThreadRecord(KernelState* ks, XThread* thread,
                                const snapshot::KernelThreadRecord& record,
                                std::string& error);
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_SNAPSHOT_H_
