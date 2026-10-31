/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/bit_stream.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

namespace xe {

BitStream::BitStream(uint8_t* buffer, size_t size_in_bits)
    : buffer_(buffer), size_bits_(size_in_bits) {}

BitStream::~BitStream() = default;

void BitStream::SetOffset(size_t offset_bits) {
  assert_false(offset_bits > size_bits_);
  offset_bits_ = std::min(offset_bits, size_bits_);
}

size_t BitStream::BitsRemaining() { return size_bits_ - offset_bits_; }

bool BitStream::IsOffsetValid(size_t num_bits) {
  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  if (rel_offset_bits && int32_t(num_bits - 8 - rel_offset_bits) < 0) {
    return false;
  }
  return true;
}

uint64_t BitStream::Peek(size_t num_bits) {
  // FYI: The reason we can't copy more than 57 bits is:
  // 57 = 7 * 8 + 1 - that can only span a maximum of 8 bytes.
  // We can't read in 9 bytes (easily), so we limit it.
  assert_false(num_bits > 57);
  assert_false(offset_bits_ + num_bits > size_bits_);

  size_t offset_bytes = std::min(offset_bits_ >> 3, (size_bits_ - 64) >> 3);
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  // offset -->
  // ..[junk]..| target bits |....[junk].............
  uint64_t bits = *(uint64_t*)(buffer_ + offset_bytes);

  // We need the data in little endian.
  // TODO: Have a flag specifying endianness of data?
  bits = xe::byte_swap(bits);

  // Shift right
  // .....[junk]........| target bits |
  bits >>= 64 - (rel_offset_bits + num_bits);

  // AND with mask
  // ...................| target bits |
  bits &= (1ULL << num_bits) - 1;

  return bits;
}

uint64_t BitStream::Read(size_t num_bits) {
  uint64_t val = Peek(num_bits);
  Advance(num_bits);

  return val;
}

// TODO: This is totally not tested!
bool BitStream::Write(uint64_t val, size_t num_bits) {
  assert_false(num_bits > 57);
  assert_false(offset_bits_ + num_bits >= size_bits_);

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  // Construct a mask
  uint64_t mask = (1ULL << num_bits) - 1;
  mask <<= 64 - (rel_offset_bits + num_bits);
  mask = ~mask;

  // Shift the value left into position.
  val <<= 64 - (rel_offset_bits + num_bits);

  // offset ----->
  // ....[junk]...| target bits w/ junk |....[junk]......
  uint64_t bits = *(uint64_t*)(buffer_ + offset_bytes);

  // AND with mask
  // ....[junk]...| target bits (0) |........[junk]......
  bits &= mask;

  // OR with val
  // ....[junk]...| target bits (val) |......[junk]......
  bits |= val;

  // Store into the bitstream.
  *(uint64_t*)(buffer_ + offset_bytes) = bits;

  // Advance the bitstream forward.
  Advance(num_bits);

  return true;
}

size_t BitStream::Copy(uint8_t* dest_buffer, size_t num_bits) {
  const size_t initial_offset_bits = offset_bits_;
  const size_t initial_rel_offset_bits = initial_offset_bits & 7;

  // SetOffset keeps offset_bits_ within size_bits_. Use subtraction rather
  // than offset + num_bits so a hostile length cannot overflow size_t.
  if (num_bits > size_bits_ - initial_offset_bits) {
    assert_false(num_bits > size_bits_ - initial_offset_bits);
    return initial_rel_offset_bits;
  }
  if (!num_bits) {
    return initial_rel_offset_bits;
  }

  size_t bits_left = num_bits;
  size_t out_offset_bytes = 0;

  // Copy the initial partial source byte to the corresponding destination
  // positions. Preserve destination bits outside the copied range.
  if (initial_rel_offset_bits) {
    const size_t chunk =
        std::min(bits_left, size_t(8) - initial_rel_offset_bits);
    const size_t shift = 8 - initial_rel_offset_bits - chunk;
    const uint8_t chunk_mask =
        static_cast<uint8_t>((uint16_t(1) << chunk) - 1);
    const uint8_t mask = static_cast<uint8_t>(chunk_mask << shift);
    const uint8_t source = buffer_[initial_offset_bits >> 3];
    const uint8_t value = static_cast<uint8_t>((source >> shift) & chunk_mask);
    dest_buffer[0] = static_cast<uint8_t>((dest_buffer[0] & ~mask) |
                                          (value << shift));

    Advance(chunk);
    bits_left -= chunk;
    if (chunk == size_t(8) - initial_rel_offset_bits) {
      ++out_offset_bytes;
    }
  }

  // Once byte-aligned, copy complete bytes without reading past the source.
  if (bits_left >= 8) {
    const size_t full_bytes = bits_left >> 3;
    const size_t source_offset_bytes = offset_bits_ >> 3;
    std::memcpy(dest_buffer + out_offset_bytes,
                buffer_ + source_offset_bytes, full_bytes);
    const size_t copied_bits = full_bytes * 8;
    Advance(copied_bits);
    out_offset_bytes += full_bytes;
    bits_left -= copied_bits;
  }

  // The remaining source bits are at the high end of one byte.
  if (bits_left) {
    const uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - bits_left));
    const uint8_t source = buffer_[offset_bits_ >> 3];
    dest_buffer[out_offset_bytes] = static_cast<uint8_t>(
        (dest_buffer[out_offset_bytes] & ~mask) | (source & mask));
    Advance(bits_left);
  }

  return initial_rel_offset_bits;
}

void BitStream::Advance(size_t num_bits) { SetOffset(offset_bits_ + num_bits); }

}  // namespace xe
