/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Include the FFmpeg libavcodec / libavutil C headers before this file.
// The implementation is shared with the offline API contract test so the test
// exercises the exact AVPacket/AVBufferRef ownership decisions used by XMA.
namespace xe {
namespace apu {
namespace xma_packet_staging {

inline bool SourceFrameBitsValid(size_t source_size_bits,
                                 size_t source_offset_bits,
                                 size_t frame_size_bits) {
  return source_offset_bits <= source_size_bits &&
         frame_size_bits <= source_size_bits - source_offset_bits;
}

inline bool CheckedPacketSize(uint32_t frame_size_bits,
                              uint32_t frame_padding_bits,
                              size_t source_capacity,
                              size_t stage_capacity,
                              size_t* packet_size_out) {
  if (!packet_size_out ||
      frame_size_bits >
          std::numeric_limits<uint32_t>::max() - frame_padding_bits) {
    return false;
  }

  const uint64_t bits = uint64_t(frame_size_bits) + frame_padding_bits;
  const uint64_t packet_bytes = 1 + bits / 8 + (bits % 8 != 0);
  if (packet_bytes > source_capacity ||
      stage_capacity < AV_INPUT_BUFFER_PADDING_SIZE ||
      packet_bytes > stage_capacity - AV_INPUT_BUFFER_PADDING_SIZE ||
      packet_bytes > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  *packet_size_out = static_cast<size_t>(packet_bytes);
  return true;
}

inline bool PrepareOwnedPacket(AVPacket* packet, const uint8_t* source,
                               size_t source_capacity, size_t packet_size,
                               size_t stage_capacity) {
  if (!packet || !source || packet_size > source_capacity ||
      stage_capacity < AV_INPUT_BUFFER_PADDING_SIZE ||
      packet_size > stage_capacity - AV_INPUT_BUFFER_PADDING_SIZE ||
      packet_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  AVBufferRef* reusable_buffer = nullptr;
  if (packet->buf && av_buffer_get_ref_count(packet->buf) == 1 &&
      packet->buf->size >= stage_capacity) {
    // Keep the unique allocation while using av_packet_unref to clear any
    // other per-packet metadata. Detaching first prevents unref from dropping
    // the AVBufferRef that is about to be reused.
    reusable_buffer = packet->buf;
    packet->buf = nullptr;
  }
  av_packet_unref(packet);

  if (reusable_buffer) {
    packet->buf = reusable_buffer;
  } else {
    packet->buf = av_buffer_allocz(stage_capacity);
    if (!packet->buf) {
      return false;
    }
  }

  packet->data = packet->buf->data;
  packet->size = static_cast<int>(packet_size);
  std::memcpy(packet->data, source, packet_size);
  std::memset(packet->data + packet_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  return true;
}

}  // namespace xma_packet_staging
}  // namespace apu
}  // namespace xe
