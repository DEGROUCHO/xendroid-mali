/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_REGISTER_FILE_H_
#define XENIA_GPU_REGISTER_FILE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

struct RegisterInfo {
  enum class Type {
    kDword,
    kFloat,
  };
  Type type;
  const char* name;
};

class RegisterFile {
 public:
  RegisterFile();

  static const RegisterInfo* GetRegisterInfo(uint32_t index);
  static bool IsValidRegister(uint32_t index);
  static constexpr size_t kRegisterCount = 0x5003;
  uint32_t values[kRegisterCount];

  // CP-owned dependencies. Resource memory/lifetime invalidation is separate.
  enum DrawDirty : uint32_t {
    kShader = 1u << 0, kPipeline = 1u << 1, kRenderTargets = 1u << 2,
    kRaster = 1u << 3, kSystem = 1u << 4, kFetch = 1u << 5,
    kPrimitive = 1u << 6, kAllDraw = (1u << 7) - 1,
  };
  struct DirtyInterval {
    uint32_t first = UINT32_MAX, end = 0;
    void Include(uint32_t begin, uint32_t limit) {
      if (begin < first) first = begin;
      if (limit > end) end = limit;
    }
    bool empty() const { return first >= end; }
    void Clear() { first = UINT32_MAX; end = 0; }
  };
  uint32_t draw_dirty = kAllDraw;
  DirtyInterval float_dirty[2] = {{0, 256}, {0, 256}};
  DirtyInterval bool_loop_dirty = {0, 40};
  uint32_t fetch_dirty_slots = UINT32_MAX;
  uint64_t fetch_generation = 1;

  // All guest CP stores, including PM4's non-virtual stores, enter here.
  // Side effects (scratch/coherency/gamma) remain with CommandProcessor.
  bool Store(uint32_t index, uint32_t value) {
    uint32_t previous = values[index];
    if (previous == value) return false;
    values[index] = value;
    uint32_t dependencies = draw_dependencies_[index];
    if (dependencies & kPartialDrawOperand) {
      // Only these two operands mix draw counts/addresses with pipeline bits.
      // The ordinary context-store path uses the dependency byte directly.
      const uint32_t mask = index == XE_GPU_REG_VGT_DRAW_INITIATOR
                                ? UINT32_C(0xFFFF)
                                : UINT32_C(0xC0000000);
      if (!((previous ^ value) & mask)) return true;
      dependencies &= kAllDraw;
    }
    if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
        index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
      MarkWritten(index, 1);
    } else {
      draw_dirty |= dependencies;
    }
    return true;
  }
  void MarkWritten(uint32_t first, uint32_t count) {
    if (!count || first >= kRegisterCount) return;
    const uint32_t end = first + std::min(count, uint32_t(kRegisterCount) - first);
    constexpr uint32_t kFloatBegin = XE_GPU_REG_SHADER_CONSTANT_000_X;
    constexpr uint32_t kFloatEnd = XE_GPU_REG_SHADER_CONSTANT_511_W + 1;
    if (first < kFloatEnd && end > kFloatBegin) {
      uint32_t begin_vec = (std::max(first, kFloatBegin) - kFloatBegin) / 4;
      uint32_t end_vec = (std::min(end, kFloatEnd) - kFloatBegin + 3) / 4;
      if (begin_vec < 256)
        float_dirty[0].Include(begin_vec, std::min(end_vec, 256u));
      if (end_vec > 256)
        float_dirty[1].Include(begin_vec > 256 ? begin_vec - 256 : 0,
                               end_vec - 256);
      if (first >= kFloatBegin && end <= kFloatEnd) return;
    }
    constexpr uint32_t kBoolBegin = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
    constexpr uint32_t kBoolEnd = XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1;
    if (first < kBoolEnd && end > kBoolBegin) {
      bool_loop_dirty.Include(std::max(first, kBoolBegin) - kBoolBegin,
                             std::min(end, kBoolEnd) - kBoolBegin);
      if (first >= kBoolBegin && end <= kBoolEnd) return;
    }
    for (uint32_t index = first; index < end; ++index) {
      const uint32_t dependencies = draw_dependencies_[index] & kAllDraw;
      if (dependencies) draw_dirty |= dependencies;
      if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
        ++fetch_generation;
        fetch_dirty_slots |= uint32_t(1) <<
            ((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
      }
    }
  }
  void InvalidateDerivedState() {
    draw_dirty = kAllDraw;
    fetch_dirty_slots = UINT32_MAX;
    float_dirty[0].Include(0, 256);
    float_dirty[1].Include(0, 256);
    bool_loop_dirty.Include(0, 40);
    ++fetch_generation;
  }

  // MMIO normally touches display/ring registers only. Unknown context writes
  // are rare: publish one full invalidation without racing the CP-owned masks.
  void StoreExternal(uint32_t index, uint32_t value) {
    values[index] = value;
    if (index >= 0x2000)
      external_write_pending_.store(true, std::memory_order_release);
  }
  void SynchronizeExternalWrites() {
    if (external_write_pending_.load(std::memory_order_acquire) &&
        external_write_pending_.exchange(false, std::memory_order_acq_rel))
      InvalidateDerivedState();
  }

  const uint32_t& operator[](uint32_t reg) const { return values[reg]; }
  uint32_t& operator[](uint32_t reg) { return values[reg]; }

  template <typename T>
  T Get(uint32_t reg) const {
    return xe::memory::Reinterpret<T>(values[reg]);
  }
  template <typename T>
  T Get(Register reg) const {
    return Get<T>(static_cast<uint32_t>(reg));
  }
  template <typename T>
  T Get() const {
    return Get<T>(T::register_index);
  }

  xenos::xe_gpu_vertex_fetch_t GetVertexFetch(uint32_t index) const {
    assert_true(index < 96);
    xenos::xe_gpu_vertex_fetch_t fetch;
    std::memcpy(&fetch,
                &values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                        (sizeof(fetch) / sizeof(uint32_t)) * index],
                sizeof(fetch));
    return fetch;
  }

  xenos::xe_gpu_texture_fetch_t GetTextureFetch(uint32_t index) const {
    assert_true(index < 32);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch,
                &values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                        (sizeof(fetch) / sizeof(uint32_t)) * index],
                sizeof(fetch));
    return fetch;
  }

  xenos::xe_gpu_memexport_stream_t GetMemExportStream(
      uint32_t float_constant_index) const {
    assert_true(float_constant_index < 512);
    xenos::xe_gpu_memexport_stream_t stream;
    std::memcpy(
        &stream,
        &values[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * float_constant_index],
        sizeof(stream));
    return stream;
  }
 private:
  std::atomic<bool> external_write_pending_{false};
  static constexpr uint8_t kPartialDrawOperand = 1u << 7;
  static const std::array<uint8_t, kRegisterCount> draw_dependencies_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_REGISTER_FILE_H_
