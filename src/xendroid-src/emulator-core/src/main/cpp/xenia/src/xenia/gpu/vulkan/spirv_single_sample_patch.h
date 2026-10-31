/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_SPIRV_SINGLE_SAMPLE_PATCH_H_
#define XENIA_GPU_VULKAN_SPIRV_SINGLE_SAMPLE_PATCH_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xe {
namespace gpu {
namespace vulkan {

// For vulkan_msaa_single_sample_storage: rewrites a compiled shader that
// fetches the samples of multisampled 2D textures into one fetching the same
// texels of single-sampled ones. Every 2D multisampled OpTypeImage becomes
// single-sampled, and every OpImageFetch with a Sample operand reads lod 0
// instead. Nothing else changes - coordinates, addressing and stores stay as
// compiled, so each sample the shader asked for is the pixel's one value.
// Returns false, leaving |patched_out| unspecified, if the module isn't SPIR-V
// this can rewrite (no signed 32-bit integer type to make the lod constant of,
// or a fetch with image operands other than Sample); true with the module
// unchanged if it fetches nothing multisampled.
inline bool PatchSpirvMultisampledFetchesToSingleSample(
    const uint32_t* code, size_t size_bytes,
    std::vector<uint32_t>& patched_out) {
  constexpr uint32_t kMagic = 0x07230203;
  constexpr uint16_t kOpTypeInt = 21;
  constexpr uint16_t kOpTypeImage = 25;
  constexpr uint16_t kOpConstant = 43;
  constexpr uint16_t kOpImageFetch = 95;
  constexpr uint32_t kDim2D = 1;
  constexpr uint32_t kImageOperandsLod = 0x2;
  constexpr uint32_t kImageOperandsSample = 0x40;
  const size_t word_count = size_bytes / sizeof(uint32_t);
  if (word_count < 5 || code[0] != kMagic) {
    return false;
  }
  patched_out.clear();
  patched_out.reserve(word_count + 4);
  patched_out.insert(patched_out.end(), code, code + 5);
  const uint32_t lod_constant_id = code[3];
  bool lod_constant_emitted = false;
  bool lod_constant_used = false;
  size_t offset = 5;
  while (offset < word_count) {
    const uint32_t first = code[offset];
    const uint16_t opcode = uint16_t(first & 0xFFFF);
    const uint32_t length = first >> 16;
    if (!length || offset + length > word_count) {
      return false;
    }
    const size_t out_offset = patched_out.size();
    patched_out.insert(patched_out.end(), code + offset,
                       code + offset + length);
    uint32_t* instruction = patched_out.data() + out_offset;
    if (opcode == kOpTypeImage && length >= 8 && instruction[3] == kDim2D &&
        instruction[6] == 1) {
      // Result, sampled type, dim, depth, arrayed, MS, sampled, format.
      instruction[6] = 0;
    } else if (opcode == kOpImageFetch && length >= 6 &&
               (instruction[5] & kImageOperandsSample)) {
      // Result type, result, image, coordinate, operands mask, operands.
      if (instruction[5] != kImageOperandsSample || length != 7) {
        return false;
      }
      instruction[5] = kImageOperandsLod;
      instruction[6] = lod_constant_id;
      lod_constant_used = true;
    } else if (opcode == kOpTypeInt && length == 4 && instruction[2] == 32 &&
               instruction[3] == 1 && !lod_constant_emitted) {
      // Right after the type it needs; constants may be declared anywhere in
      // the types section.
      patched_out.push_back((uint32_t(4) << 16) | kOpConstant);
      patched_out.push_back(code[offset + 1]);
      patched_out.push_back(lod_constant_id);
      patched_out.push_back(0);
      lod_constant_emitted = true;
    }
    offset += length;
  }
  if (lod_constant_used && !lod_constant_emitted) {
    return false;
  }
  // The new id (unused if nothing was patched - still valid).
  patched_out[3] = lod_constant_id + 1;
  return true;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_SPIRV_SINGLE_SAMPLE_PATCH_H_
