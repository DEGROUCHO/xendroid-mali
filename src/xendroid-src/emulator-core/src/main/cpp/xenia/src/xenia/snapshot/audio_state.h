// XenDroid runtime snapshot: audio section (client table + XMA decoder).
//
// Host driver queues and FFmpeg overlap/history are rebuilt empty on restore.
// This is an explicit audio restart policy, not sample-exact continuity; guest
// XMA context RAM is preserved, but it does not contain all decoder history.
// Callers must freeze guest writers AND pause both audio workers first.
#ifndef XENIA_SNAPSHOT_AUDIO_STATE_H_
#define XENIA_SNAPSHOT_AUDIO_STATE_H_
#include <algorithm>
#include <set>
#include "xenia/snapshot/state_file.h"
namespace xe::snapshot {
struct AudioState {
  static constexpr uint32_t kMaxClients = 8;
  static constexpr uint32_t kXmaContexts = 320;
  static constexpr uint32_t kXmaRegisters = (0xFFFF + 1) / 4;
  struct Client {
    uint32_t index = 0, callback = 0, callback_arg = 0, wrapped_callback_arg = 0;
    bool operator==(const Client&) const = default;
  };
  std::vector<Client> clients;
  uint32_t queued_frames = 0;
  std::vector<uint32_t> xma_registers;  // kXmaRegisters words
  std::vector<uint32_t> xma_allocated;  // ascending context ids
  std::vector<uint32_t> xma_enabled;    // ascending context ids (subset)

  bool Validate() const {
    if (queued_frames < 4 || queued_frames > 64 || clients.size() > kMaxClients || xma_registers.size() != kXmaRegisters) return false;
    std::set<uint32_t> seen;
    for (const auto& c : clients)
      if (c.index >= kMaxClients || !seen.insert(c.index).second || !c.callback) return false;
    uint32_t prev = 0;
    bool first = true;
    for (uint32_t id : xma_allocated) {
      if (id >= kXmaContexts || (!first && id <= prev)) return false;
      prev = id; first = false;
    }
    std::set<uint32_t> alloc(xma_allocated.begin(), xma_allocated.end());
    prev = 0; first = true;
    for (uint32_t id : xma_enabled) {
      if (!alloc.count(id) || (!first && id <= prev)) return false;
      prev = id; first = false;
    }
    return true;
  }
  std::vector<uint8_t> Encode() const {
    if (!Validate()) return {};
    std::vector<uint8_t> out;
    Put32(out, 1);
    Put32(out, queued_frames);
    Put32(out, uint32_t(clients.size()));
    for (const auto& c : clients) {
      Put32(out, c.index); Put32(out, c.callback); Put32(out, c.callback_arg); Put32(out, c.wrapped_callback_arg);
    }
    for (uint32_t w : xma_registers) Put32(out, w);
    Put32(out, uint32_t(xma_allocated.size()));
    for (uint32_t id : xma_allocated) Put32(out, id);
    Put32(out, uint32_t(xma_enabled.size()));
    for (uint32_t id : xma_enabled) Put32(out, id);
    return out;
  }
  static bool Decode(std::span<const uint8_t> bytes, AudioState& out) {
    Reader r(bytes);
    AudioState s;
    uint32_t version, n;
    if (!r.U32(version) || version != 1 || !r.U32(s.queued_frames) || !r.U32(n) ||
        n > kMaxClients || uint64_t(n) * 16 > r.remaining())
      return false;
    s.clients.resize(n);
    for (auto& c : s.clients)
      if (!r.U32(c.index) || !r.U32(c.callback) || !r.U32(c.callback_arg) || !r.U32(c.wrapped_callback_arg)) return false;
    if (uint64_t(kXmaRegisters) * 4 > r.remaining()) return false;
    s.xma_registers.resize(kXmaRegisters);
    for (auto& w : s.xma_registers)
      if (!r.U32(w)) return false;
    for (auto* ids : {&s.xma_allocated, &s.xma_enabled}) {
      if (!r.U32(n) || n > kXmaContexts || uint64_t(n) * 4 > r.remaining()) return false;
      ids->resize(n);
      for (auto& id : *ids)
        if (!r.U32(id)) return false;
    }
    if (r.remaining() || !s.Validate()) return false;
    out = std::move(s);
    return true;
  }
};
}  // namespace xe::snapshot
#endif
