/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XE_NO_MSL
// DXT4/5 (BC3) -> ASTC 4x4 LDR transcoding for GPUs without BC support (Mali):
// 8 bits per texel instead of 32 for the decompressed RGBA8 fallback.
// - Blocks with constant alpha (opaque surfaces): CEM 12 (RGBA direct), one
//   plane, 8-bit endpoints, 2-bit weights - color as in the DXT1 transcoder,
//   alpha exact.
// - Blocks with varying alpha: dual plane CEM 12 with the alpha on the second
//   plane, 2-bit weights, endpoints quantized to 48 levels (trit + 4 bits, 45
//   bits for 8 values). Color within ~5/255. Alpha endpoints are the block's
//   minimum and maximum (0 and 255 are exact), texels take the nearest of 4
//   levels instead of the 6 or 8 of BC3.
// Verified against Mesa's ASTC decoder on random and edge-case BC3 blocks.

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

static_const_xe uint kXeAstcQuant48Unquantize[12] = {
    0xEF10FF00u, 0xCF30DF20u, 0xAE51BE41u, 0x8E719E61u, 0xEA15FA05u, 0xC936D926u,
    0xA956B946u, 0x88779867u, 0xE41BF40Bu, 0xC43BD42Bu, 0xA35CB34Cu, 0x837C936Cu
};
static_const_xe uint kXeAstcQuant48Quantize[64] = {
    0x10000000u, 0x10101010u, 0x20202010u, 0x02022020u, 0x12020202u, 0x12121212u,
    0x22222212u, 0x04042222u, 0x04040404u, 0x14141414u, 0x24242414u, 0x06062424u,
    0x06060606u, 0x16161616u, 0x26262616u, 0x08082626u, 0x08080808u, 0x18181818u,
    0x28281818u, 0x0A282828u, 0x0A0A0A0Au, 0x1A1A1A1Au, 0x2A2A1A1Au, 0x0C2A2A2Au,
    0x0C0C0C0Cu, 0x1C1C1C0Cu, 0x2C2C1C1Cu, 0x0E2C2C2Cu, 0x0E0E0E0Eu, 0x1E1E1E0Eu,
    0x2E2E1E1Eu, 0x2E2E2E2Eu, 0x2F2F2F2Fu, 0x1F1F2F2Fu, 0x0F1F1F1Fu, 0x0F0F0F0Fu,
    0x2D2D2D0Fu, 0x1D1D2D2Du, 0x0D1D1D1Du, 0x0D0D0D0Du, 0x2B2B2B0Du, 0x1B1B2B2Bu,
    0x1B1B1B1Bu, 0x0B0B0B0Bu, 0x2929290Bu, 0x19192929u, 0x19191919u, 0x09090909u,
    0x27270909u, 0x17272727u, 0x17171717u, 0x07070707u, 0x25250707u, 0x15252525u,
    0x15151515u, 0x05050505u, 0x23230505u, 0x13232323u, 0x13131313u, 0x03030313u,
    0x21210303u, 0x11212121u, 0x11111111u, 0x01010111u
};

uint XeAstcQuant48Unquantize(uint code) {
  return (kXeAstcQuant48Unquantize[code >> 2u] >> ((code & 3u) << 3u)) & 0xFFu;
}

uint XeAstcQuant48Quantize(uint value) {
  return (kXeAstcQuant48Quantize[value >> 2u] >> ((value & 3u) << 3u)) & 0xFFu;
}

// ISE bits of up to 5 values, trit + 4 bits each (the trits of absent values
// must be 0).
uint XeAstcQuant48Group(uint q0, uint q1, uint q2, uint q3, uint q4) {
  uint t = XeAstcTritBlock((q0 >> 4u) + 3u * (q1 >> 4u) + 9u * (q2 >> 4u) +
                           27u * (q3 >> 4u) + 81u * (q4 >> 4u));
  return (q0 & 15u) | ((t & 3u) << 4u) | ((q1 & 15u) << 6u) |
         (((t >> 2u) & 3u) << 10u) | ((q2 & 15u) << 12u) |
         (((t >> 4u) & 1u) << 16u) | ((q3 & 15u) << 17u) |
         (((t >> 5u) & 3u) << 21u) | ((q4 & 15u) << 23u) |
         (((t >> 7u) & 1u) << 27u);
}

// block: guest DXT5 block after the endian swap - alpha endpoints and codes in
// XY, color endpoints in Z, color codes in W.
uint4_xe XeDXT5BlockToASTC4x4(uint4_xe guest_block) {
  uint2_xe end_8in10 = XeDXTColorEndpointsToBGR8In10(guest_block.z);
  uint3_xe rgb0 = (uint3_xe(end_8in10.x, end_8in10.x, end_8in10.x) >>
                   uint3_xe(20u, 10u, 0u)) & 0xFFu;
  uint3_xe rgb1 = (uint3_xe(end_8in10.y, end_8in10.y, end_8in10.y) >>
                   uint3_xe(20u, 10u, 0u)) & 0xFFu;
  // Weight of the second endpoint, 0...3, per texel - DXT5 color is always in
  // the 4-color mode.
  uint color_weights = XeDXTHighColorWeights(guest_block.w);
  uint2_xe alpha_end = (guest_block.xx >> uint2_xe(0u, 8u)) & 0xFFu;
  uint alpha_weights_01 = XeDXT5HighAlphaWeights(
      alpha_end, (guest_block.x >> 16u) | ((guest_block.y & 0xFFu) << 16u));
  uint alpha_weights_23 =
      XeDXT5HighAlphaWeights(alpha_end, guest_block.y >> 8u);
  // Alpha of the texels, 4 per row.
  uint4_xe alpha_rows = uint4_xe(
      XeDXT5RowToA8(alpha_end, alpha_weights_01),
      XeDXT5RowToA8(alpha_end, alpha_weights_01 >> 12u),
      XeDXT5RowToA8(alpha_end, alpha_weights_23),
      XeDXT5RowToA8(alpha_end, alpha_weights_23 >> 12u));
  uint alpha_min = 255u;
  uint alpha_max = 0u;
  uint i;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    uint alpha = (alpha_rows[i >> 2u] >> ((i & 3u) << 3u)) & 0xFFu;
    alpha_min = min(alpha_min, alpha);
    alpha_max = max(alpha_max, alpha);
  }
  uint4_xe block;
  if (alpha_min == alpha_max) {
    if (rgb1.x + rgb1.y + rgb1.z < rgb0.x + rgb0.y + rgb0.z) {
      // CEM 12 would apply blue contraction - swap the endpoints instead.
      uint3_xe rgb_swap = rgb0;
      rgb0 = rgb1;
      rgb1 = rgb_swap;
      color_weights = ~color_weights;
    }
    // Block mode: 4x4 weight grid, range 0...3, one plane; 1 partition,
    // CEM 12.
    block = uint4_xe(0x042u | (12u << 13u), 0u, 0u, 0u);
    block |= XeAstcPlace(17u, rgb0.x | (rgb1.x << 8u) | (rgb0.y << 16u));
    block |= XeAstcPlace(41u, rgb1.y | (rgb0.z << 8u) | (rgb1.z << 16u));
    block |= XeAstcPlace(65u, alpha_min | (alpha_min << 8u));
    // Weights are stored from bit 127 downwards.
    block.w |= XeAstcReverseBits(color_weights);
    return block;
  }
  uint3_xe q0 = uint3_xe(XeAstcQuant48Quantize(rgb0.x),
                         XeAstcQuant48Quantize(rgb0.y),
                         XeAstcQuant48Quantize(rgb0.z));
  uint3_xe q1 = uint3_xe(XeAstcQuant48Quantize(rgb1.x),
                         XeAstcQuant48Quantize(rgb1.y),
                         XeAstcQuant48Quantize(rgb1.z));
  uint qa0 = XeAstcQuant48Quantize(alpha_min);
  uint qa1 = XeAstcQuant48Quantize(alpha_max);
  int alpha_end0 = int(XeAstcQuant48Unquantize(qa0));
  int alpha_range = int(XeAstcQuant48Unquantize(qa1)) - alpha_end0;
  // Nearest of the 4 levels between the quantized alpha endpoints.
  uint alpha_weights = 0u;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    int alpha = int((alpha_rows[i >> 2u] >> ((i & 3u) << 3u)) & 0xFFu);
    int level = alpha_range > 0
                    ? ((alpha - alpha_end0) * 6 + alpha_range) /
                          (2 * alpha_range)
                    : 0;
    alpha_weights |= uint(clamp(level, 0, 3)) << (i << 1u);
  }
  uint sum0 = XeAstcQuant48Unquantize(q0.x) + XeAstcQuant48Unquantize(q0.y) +
              XeAstcQuant48Unquantize(q0.z);
  uint sum1 = XeAstcQuant48Unquantize(q1.x) + XeAstcQuant48Unquantize(q1.y) +
              XeAstcQuant48Unquantize(q1.z);
  if (sum1 < sum0) {
    // Avoid blue contraction by swapping the endpoints, alpha included.
    uint3_xe q_swap = q0;
    q0 = q1;
    q1 = q_swap;
    uint qa_swap = qa0;
    qa0 = qa1;
    qa1 = qa_swap;
    color_weights = ~color_weights;
    alpha_weights = ~alpha_weights;
  }
  // Block mode: 4x4 grid, range 0...3, dual plane; 1 partition, CEM 12.
  block = uint4_xe(0x442u | (12u << 13u), 0u, 0u, 0u);
  // Endpoints: R0 R1 G0 G1 B0 B1 A0 A1, 45 bits from bit 17.
  block |= XeAstcPlace(17u, XeAstcQuant48Group(q0.x, q1.x, q0.y, q1.y, q0.z));
  block |= XeAstcPlace(45u, XeAstcQuant48Group(q1.z, qa0, qa1, 0u, 0u));
  // Color component selector: alpha on the second plane.
  block |= XeAstcPlace(62u, 3u);
  // Interleaved (color, alpha) weights, 4 bits per texel, from bit 127 down.
  uint weights_lo = 0u;
  uint weights_hi = 0u;
  unroll_xe for (i = 0u; i < 8u; ++i) {
    weights_lo |= (((color_weights >> (i << 1u)) & 3u) |
                   (((alpha_weights >> (i << 1u)) & 3u) << 2u)) << (i << 2u);
    weights_hi |= (((color_weights >> ((i + 8u) << 1u)) & 3u) |
                   (((alpha_weights >> ((i + 8u) << 1u)) & 3u) << 2u))
                  << (i << 2u);
  }
  block.w |= XeAstcReverseBits(weights_lo);
  block.z |= XeAstcReverseBits(weights_hi);
  return block;
}

XE_TEXTURE_LOAD_ENTRY(16)
{
  // 1 thread = 4 DXT5 blocks to 4 ASTC 4x4 blocks.
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
    byte_buffer_align16_store16_xe(xe_texture_load_dest,
                                   block_offset_host + (j << 4u),
                                   XeDXT5BlockToASTC4x4(guest_block));
  }
}
entry_code_end_compute_xe
