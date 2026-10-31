#ifndef XENIA_SNAPSHOT_SOCKET_STATE_H_
#define XENIA_SNAPSHOT_SOCKET_STATE_H_
#include <vector>
#include "xenia/snapshot/state_file.h"
namespace xe::snapshot {
// Only unconnected IPv4 datagram sockets with empty queues are reconstructible.
struct SocketOption {
  uint32_t level = 0, name = 0;
  std::vector<uint8_t> bytes;
  bool Validate() const {
    if (level != 0xFFFF || bytes.empty() || bytes.size() > 256) return false;
    switch (name) {
      case 0x0004: case 0x0020: case 0x1001: case 0x1002:
        return bytes.size() == 4;
      case 0x0080: return bytes.size() == 8; // POSIX struct linger.
      case 0x1005: case 0x1006: return bytes.size() == 16; // POSIX timeval.
      default: return false;
    }
  }
};
struct SocketState {
  uint32_t protocol = 17, flags = 0, address = 0, port = 0;
  uint32_t recv_timeout = 0, send_timeout = 0, last_error = 0;
  std::vector<SocketOption> options;
  // flags: secure, explicit bind, broadcast, guest nonblocking.
  bool Validate() const {
    if ((protocol != 17 && protocol != 254) || (flags & ~15u) ||
        port > 65535 || ((flags & 2) && !port) || options.size() > 64)
      return false;
    for (const auto& o : options)
      if (!o.Validate()) return false;
    return true;
  }
  std::vector<uint8_t> Encode() const {
    std::vector<uint8_t> b;
    for (uint32_t v : {1u, protocol, flags, address, port, recv_timeout,
                       send_timeout, last_error, uint32_t(options.size())}) Put32(b, v);
    for (const auto& o : options) {
      Put32(b, o.level); Put32(b, o.name); Put32(b, uint32_t(o.bytes.size()));
      b.insert(b.end(), o.bytes.begin(), o.bytes.end());
    }
    return b;
  }
  static bool Decode(std::span<const uint8_t> bytes, SocketState& out) {
    Reader r(bytes); SocketState s; uint32_t version, count;
    if (!r.U32(version) || version != 1 || !r.U32(s.protocol) ||
        !r.U32(s.flags) || !r.U32(s.address) || !r.U32(s.port) ||
        !r.U32(s.recv_timeout) || !r.U32(s.send_timeout) ||
        !r.U32(s.last_error) || !r.U32(count) || count > 64) return false;
    for (uint32_t i = 0; i < count; ++i) {
      SocketOption o; uint32_t size;
      if (!r.U32(o.level) || !r.U32(o.name) || !r.U32(size) ||
          !size || size > 256 || size > r.remaining()) return false;
      o.bytes.resize(size);
      if (!r.Bytes(o.bytes)) return false;
      s.options.push_back(std::move(o));
    }
    if (r.remaining() || !s.Validate()) return false;
    out = std::move(s); return true;
  }
};
}
#endif
