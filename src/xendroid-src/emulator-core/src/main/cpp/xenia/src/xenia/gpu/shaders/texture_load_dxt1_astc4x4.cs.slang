/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XE_NO_MSL
// DXT1 (BC1) -> ASTC 4x4 LDR transcoding for GPUs without BC support (Mali):
// 8 bits per texel instead of 32 for the decompressed RGBA8 fallback.
// - 4-color blocks: CEM 8 (RGB direct), 8-bit endpoints (the 565 endpoints
//   expanded exactly), 2-bit weights (0, 21, 43, 64 / 64 vs thirds) - within
//   2/255 of the exact BC1 palette.
// - 3-color / punch-through blocks: dual plane CEM 12 (RGBA direct) with the
//   alpha on the second plane, trit weights (0, 1/2, 1 exactly), 7-bit
//   endpoints - opaque texels within 1/255, alpha exact. Transparent texels
//   get the darker endpoint color instead of black.

#include "pixel_formats.xesli"
#include "texture_load.xesli"

// ASTC integer sequence encoding of 5 trits (t0 + 3*t1 + 9*t2 + 27*t3 +
// 81*t4) as 8 bits, 4 per dword.
static_const_xe uint kXeAstcTritEncode[61] = {
    0x04020100u, 0x09080605u, 0x1211100Au, 0x18161514u, 0x07031A19u, 0x1B17130Bu,
    0x200E0D0Cu, 0x25242221u, 0x2A292826u, 0x34323130u, 0x39383635u, 0x2B27233Au,
    0x2C3B3733u, 0x41402E2Du, 0x46454442u, 0x504A4948u, 0x55545251u, 0x5A595856u,
    0x534B4743u, 0x4D4C5B57u, 0x8281804Eu, 0x88868584u, 0x91908A89u, 0x96959492u,
    0x839A9998u, 0x97938B87u, 0x8E8D8C9Bu, 0xA4A2A1A0u, 0xA9A8A6A5u, 0xB2B1B0AAu,
    0xB8B6B5B4u, 0xA7A3BAB9u, 0xBBB7B3ABu, 0xC0AEADACu, 0xC5C4C2C1u, 0xCAC9C8C6u,
    0xD4D2D1D0u, 0xD9D8D6D5u, 0xCBC7C3DAu, 0xCCDBD7D3u, 0x6160CECDu, 0x66656462u,
    0x706A6968u, 0x75747271u, 0x7A797876u, 0x736B6763u, 0x6D6C7B77u, 0xE2E1E06Eu,
    0xE8E6E5E4u, 0xF1F0EAE9u, 0xF6F5F4F2u, 0xE3FAF9F8u, 0xF7F3EBE7u, 0xEEEDECFBu,
    0x3C1E1D1Cu, 0x5D5C3E3Du, 0x9E9D9C5Eu, 0xDCBEBDBCu, 0x3F1FDEDDu, 0xDFBF9F5Fu,
    0x007E7D7Cu
};

uint XeAstcTritBlock(uint index) {
  return (kXeAstcTritEncode[index >> 2u] >> ((index & 3u) << 3u)) & 0xFFu;
}

uint XeAstcReverseBits(uint v) {
  v = ((v >> 1u) & 0x55555555u) | ((v & 0x55555555u) << 1u);
  v = ((v >> 2u) & 0x33333333u) | ((v & 0x33333333u) << 2u);
  v = ((v >> 4u) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4u);
  v = ((v >> 8u) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8u);
  return (v >> 16u) | (v << 16u);
}

// A value of up to 25 bits placed at a bit position of a 128-bit block.
uint4_xe XeAstcPlace(uint position, uint value) {
  uint word = position >> 5u;
  uint shift = position & 31u;
  uint lo = value << shift;
  uint hi = shift != 0u ? (value >> (32u - shift)) : 0u;
  return uint4_xe(word == 0u ? lo : 0u,
                  word == 1u ? lo : (word == 0u ? hi : 0u),
                  word == 2u ? lo : (word == 1u ? hi : 0u),
                  word == 3u ? lo : (word == 2u ? hi : 0u));
}

uint XeAstcDequant7(uint v) { return (v << 1u) | (v >> 6u); }

// endpoints: c0 in 0:15, c1 in 16:31 (565); codes: 2 bits per texel, raster.
uint4_xe XeDXT1BlockToASTC4x4(uint endpoints, uint codes) {
  uint2_xe end_8in10 = XeDXTColorEndpointsToBGR8In10(endpoints);
  uint3_xe rgb0 = (uint3_xe(end_8in10.x, end_8in10.x, end_8in10.x) >>
                   uint3_xe(20u, 10u, 0u)) & 0xFFu;
  uint3_xe rgb1 = (uint3_xe(end_8in10.y, end_8in10.y, end_8in10.y) >>
                   uint3_xe(20u, 10u, 0u)) & 0xFFu;
  uint4_xe block;
  if ((endpoints & 0xFFFFu) > (endpoints >> 16u)) {
    // 4 colors. Weight of the second endpoint, 0...3, per texel.
    uint weights = XeDXTHighColorWeights(codes);
    if (rgb1.x + rgb1.y + rgb1.z < rgb0.x + rgb0.y + rgb0.z) {
      // CEM 8 would apply blue contraction - swap the endpoints instead.
      uint3_xe rgb_swap = rgb0;
      rgb0 = rgb1;
      rgb1 = rgb_swap;
      weights = ~weights;
    }
    // Block mode: 4x4 weight grid, range 0...3, one plane; 1 partition, CEM 8.
    block = uint4_xe(0x042u | (8u << 13u), 0u, 0u, 0u);
    block |= XeAstcPlace(17u, rgb0.x | (rgb1.x << 8u) | (rgb0.y << 16u));
    block |= XeAstcPlace(41u, rgb1.y | (rgb0.z << 8u) | (rgb1.z << 16u));
    // Weights are stored from bit 127 downwards.
    block.w |= XeAstcReverseBits(weights);
    return block;
  }
  // 3 colors (00 = c0, 01 = c1, 10 = middle) and punch-through (11).
  uint3_xe q0 = rgb0 >> 1u;
  uint3_xe q1 = rgb1 >> 1u;
  uint sum0 = XeAstcDequant7(q0.x) + XeAstcDequant7(q0.y) + XeAstcDequant7(q0.z);
  uint sum1 = XeAstcDequant7(q1.x) + XeAstcDequant7(q1.y) + XeAstcDequant7(q1.z);
  // Transparent texels take the darker endpoint's color.
  uint dark_weight = sum0 <= sum1 ? 0u : 2u;
  uint alpha0 = 0u;
  uint alpha1 = 127u;
  bool swap_ends = sum1 < sum0;
  if (swap_ends) {
    uint3_xe q_swap = q0;
    q0 = q1;
    q1 = q_swap;
    alpha0 = 127u;
    alpha1 = 0u;
  }
  // Block mode: 4x4 grid, range 0...2, dual plane; 1 partition, CEM 12.
  block = uint4_xe(0x451u | (12u << 13u), 0u, 0u, 0u);
  block |= XeAstcPlace(17u, q0.x | (q1.x << 7u) | (q0.y << 14u));
  block |= XeAstcPlace(38u, q1.y | (q0.z << 7u) | (q1.z << 14u));
  block |= XeAstcPlace(59u, alpha0 | (alpha1 << 7u));
  // Color component selector: alpha on the second plane.
  block |= XeAstcPlace(74u, 3u);
  // Interleaved (color, alpha) trit weights, 5 per 8-bit trit block.
  uint stream_lo = 0u;
  uint stream_hi = 0u;
  uint group = 0u;
  uint group_count = 0u;
  uint group_scale = 1u;
  uint stream_bits = 0u;
  uint i;
  unroll_xe for (i = 0u; i < 32u; ++i) {
    uint code = (codes >> ((i >> 1u) << 1u)) & 3u;
    uint weight;
    if ((i & 1u) == 0u) {
      weight = code == 0u ? 0u : (code == 1u ? 2u : (code == 2u ? 1u
                                                                  : dark_weight));
    } else {
      weight = code == 3u ? 0u : 2u;
    }
    if (swap_ends) {
      weight = 2u - weight;
    }
    group += weight * group_scale;
    group_scale *= 3u;
    ++group_count;
    if (group_count == 5u || i == 31u) {
      uint trit_bits = XeAstcTritBlock(group);
      // The last group has 2 trits - 4 bits.
      uint bit_count = group_count == 5u ? 8u : 4u;
      trit_bits &= (1u << bit_count) - 1u;
      if (stream_bits < 32u) {
        stream_lo |= trit_bits << stream_bits;
        if (stream_bits + bit_count > 32u) {
          stream_hi |= trit_bits >> (32u - stream_bits);
        }
      } else {
        stream_hi |= trit_bits << (stream_bits - 32u);
      }
      stream_bits += bit_count;
      group = 0u;
      group_count = 0u;
      group_scale = 1u;
    }
  }
  // 52 weight bits from bit 127 down to bit 76.
  block.w |= XeAstcReverseBits(stream_lo);
  block.z |= XeAstcReverseBits(stream_hi);
  return block;
}

XE_TEXTURE_LOAD_ENTRY(16)
{
  // 1 thread = 4 DXT1 blocks to 4 ASTC 4x4 blocks.
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
      XeTextureLoadSourceAddress(load_info, block_index, 3u);
  uint4_xe blocks_01 = XeEndianSwap32(
      guest_byte_buffer_align16_load16_xe(xe_texture_load_source, block_offset_guest),
      load_info.endian_32);
  block_offset_guest += XeTextureLoadLocalXAddressXor(2u, 3u,
                                                      load_info.is_tiled);
  uint4_xe blocks_23 = XeEndianSwap32(
      guest_byte_buffer_align16_load16_xe(xe_texture_load_source, block_offset_guest),
      load_info.endian_32);
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host,
      XeDXT1BlockToASTC4x4(blocks_01.x, blocks_01.y));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x10u,
      XeDXT1BlockToASTC4x4(blocks_01.z, blocks_01.w));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x20u,
      XeDXT1BlockToASTC4x4(blocks_23.x, blocks_23.y));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x30u,
      XeDXT1BlockToASTC4x4(blocks_23.z, blocks_23.w));
}
entry_code_end_compute_xe
