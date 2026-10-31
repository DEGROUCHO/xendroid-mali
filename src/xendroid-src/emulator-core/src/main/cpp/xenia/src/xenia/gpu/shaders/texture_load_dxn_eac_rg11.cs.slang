/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XE_NO_MSL
// DXN (BC5) -> EAC RG11 transcoding for GPUs without BC support (Mali): 8 bits
// per texel instead of 16 for the decompressed R8G8 fallback. Each BC4 channel
// is encoded as an EAC R11 block - the table minimizing the squared error over
// the BC4 palette, with the multiplier estimated from the range and a centered
// base codeword (~1000 candidate evaluations per channel, cheap enough for
// streaming). On smooth blocks (normal maps) the mean error is ~0.5/255;
// verified against Mesa's EAC decoder.

#include "pixel_formats.xesli"
#include "texture_load.xesli"

// Modifier magnitudes, 4 bits each: entries 0...3 are negative, 4...7
// positive.
static_const_xe uint kXeEACModifiers[16] = {
    0xE852F963u, 0xC962DA73u, 0xC741D852u, 0xC531D642u, 0xB752C863u,
    0xA862B973u, 0xA763B874u, 0xA742B853u, 0x9751A862u, 0x9741A852u,
    0x9731A842u, 0x9641A752u, 0x9632A743u, 0x9210A321u, 0x87539864u,
    0x86429753u
};

int XeEACModifier(uint table_word, uint index) {
  int magnitude = int((table_word >> (index << 2u)) & 15u);
  return index < 4u ? -magnitude : magnitude;
}

// BC4 block (endpoints in bits 0:15 of X, 3-bit codes from bit 16 of X) to an
// EAC R11 block, returned as its 8 bytes in memory order. The error is
// evaluated over the BC4 palette entries actually used, weighted by their
// texel counts, for each of the 16 tables with the multiplier estimated from
// the range and the centered base codeword.
uint2_xe XeBC4BlockToEACR11(uint2_xe bc4) {
  int alpha0 = int(bc4.x & 0xFFu);
  int alpha1 = int((bc4.x >> 8u) & 0xFFu);
  uint codes_01 = (bc4.x >> 16u) | ((bc4.y & 0xFFu) << 16u);
  uint codes_23 = bc4.y >> 8u;
  // Texel counts of the 8 codes, 8 bits each.
  uint counts_lo = 0u;
  uint counts_hi = 0u;
  uint i;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    uint code = ((i < 8u ? codes_01 : codes_23) >> ((i & 7u) * 3u)) & 7u;
    if (code < 4u) {
      counts_lo += 1u << (code << 3u);
    } else {
      counts_hi += 1u << ((code - 4u) << 3u);
    }
  }
  // Palette in 11-bit units, 16 bits each (as XeDXT5RowToA8 decodes it).
  uint4_xe palette = uint4_xe(0u, 0u, 0u, 0u);
  int lo = 2047;
  int hi = 0;
  unroll_xe for (i = 0u; i < 8u; ++i) {
    int value;
    if (i == 0u) {
      value = alpha0;
    } else if (i == 1u) {
      value = alpha1;
    } else if (alpha0 > alpha1) {
      value = (alpha0 * int(8u - i) + alpha1 * int(i - 1u)) / 7;
    } else if (i < 6u) {
      value = (alpha0 * int(6u - i) + alpha1 * int(i - 1u)) / 5;
    } else {
      value = i == 6u ? 0 : 255;
    }
    value = (value * 2047 + 127) / 255;
    uint count = ((i < 4u ? counts_lo : counts_hi) >> ((i & 3u) << 3u)) &
                 0xFFu;
    if (count != 0u) {
      lo = min(lo, value);
      hi = max(hi, value);
    }
    palette[i >> 1u] |= uint(value) << ((i & 1u) << 4u);
  }
  int best_error = 0x7FFFFFFF;
  uint best_header = 0u;
  uint best_selection = 0u;
  uint table;
  dont_unroll for (table = 0u; table < 16u; ++table) {
    uint table_word = kXeEACModifiers[table];
    int modifier_min = XeEACModifier(table_word, 3u);
    int modifier_max = XeEACModifier(table_word, 7u);
    int span = modifier_max - modifier_min;
    int multiplier = min((hi - lo + span * 4) / (span * 8), 15);
    int scale = multiplier != 0 ? multiplier * 8 : 1;
    int base =
        min(max(lo + hi - (modifier_min + modifier_max) * scale, 0) / 16, 255);
    int error = 0;
    uint selection = 0u;
    unroll_xe for (i = 0u; i < 8u; ++i) {
      int count = int(((i < 4u ? counts_lo : counts_hi) >> ((i & 3u) << 3u)) &
                      0xFFu);
      int target = int((palette[i >> 1u] >> ((i & 1u) << 4u)) & 0xFFFFu);
      int entry_best_error = 0x7FFFFFFF;
      uint entry_best_index = 0u;
      uint index;
      unroll_xe for (index = 0u; index < 8u; ++index) {
        int difference =
            clamp(base * 8 + 4 + XeEACModifier(table_word, index) * scale, 0,
                  2047) -
            target;
        int entry_error = difference * difference;
        if (entry_error < entry_best_error) {
          entry_best_error = entry_error;
          entry_best_index = index;
        }
      }
      error += entry_best_error * count;
      selection |= entry_best_index << (i * 3u);
    }
    if (error < best_error) {
      best_error = error;
      best_header = uint(base) | ((uint(multiplier) << 4u | table) << 8u);
      best_selection = selection;
    }
  }
  // Texel (x, y) index at bit ((3 - y) + (3 - x) * 4) * 3 of the big-endian
  // 48-bit index field.
  uint indices_lo = 0u;
  uint indices_hi = 0u;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    uint code = ((i < 8u ? codes_01 : codes_23) >> ((i & 7u) * 3u)) & 7u;
    uint index = (best_selection >> (code * 3u)) & 7u;
    uint position = ((3u - (i >> 2u)) + (3u - (i & 3u)) * 4u) * 3u;
    if (position < 32u) {
      indices_lo |= index << position;
      if (position > 29u) {
        indices_hi |= index >> (32u - position);
      }
    } else {
      indices_hi |= index << (position - 32u);
    }
  }
  // Bytes: base, multiplier | table, then the indices from bit 47 down.
  return uint2_xe(
      best_header | (((indices_hi >> 8u) & 0xFFu) << 16u) |
          ((indices_hi & 0xFFu) << 24u),
      (indices_lo >> 24u) | (((indices_lo >> 16u) & 0xFFu) << 8u) |
          (((indices_lo >> 8u) & 0xFFu) << 16u) |
          ((indices_lo & 0xFFu) << 24u));
}

XE_TEXTURE_LOAD_ENTRY(16)
{
  // 1 thread = 4 DXN blocks to 4 EAC RG11 blocks.
  XeTextureLoadInfo load_info = XeTextureLoadGetInfo(pass_push_consts_xe);
  uint3_xe block_index = in_global_thread_id_xe << uint3_xe(2u, 0u, 0u);
  dont_flatten_xe
  if (any(greater_than_equal_xe(block_index.xy, load_info.size_blocks.xy))) {
    return;
  }
  uint block_offset_host =
      load_info.host_offset +
      uint(XeTextureHostLinearOffset(int3_xe(block_index), load_info.host_pitch,
                                     load_info.size_blocks.y, 16u));
  uint block_offset_guest =
      XeTextureLoadSourceAddress(load_info, block_index, 4u);
  uint j;
  dont_unroll for (j = 0u; j < 4u; ++j) {
    // block_index.x is a multiple of 4 - the low X bits of the address are 0.
    uint4_xe guest_block = XeEndianSwap32(
        guest_byte_buffer_align16_load16_xe(
            xe_texture_load_source,
            block_offset_guest +
                XeTextureLoadLocalXAddressXor(j, 4u, load_info.is_tiled)),
        load_info.endian_32);
    byte_buffer_align16_store16_xe(
        xe_texture_load_dest, block_offset_host + (j << 4u),
        uint4_xe(XeBC4BlockToEACR11(guest_block.xy),
                 XeBC4BlockToEACR11(guest_block.zw)));
  }
}
entry_code_end_compute_xe
