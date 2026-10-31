/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
#define XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_

#include <cstdint>
#include <optional>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {

class DrawExtentEstimator {
 public:
  enum class InputReadKind : uint8_t { kIndexBuffer, kVertexFetch };
  using InputReadValidator = bool (*)(void* context, uint32_t address,
                                     uint32_t length, InputReadKind kind);

  static constexpr uint32_t kInputReadRejectedFailureReason = 7;

  DrawExtentEstimator(const RegisterFile& register_file, const Memory& memory,
                      TraceWriter* trace_writer)
      : register_file_(register_file),
        memory_(memory),
        trace_writer_(trace_writer),
        shader_interpreter_(register_file, memory) {
    shader_interpreter_.SetTraceWriter(trace_writer);
  }

  void SetInputReadValidator(InputReadValidator validator, void* context) {
    input_read_validator_ = validator;
    input_read_validator_context_ = context;
    shader_interpreter_.SetMemoryReadValidator(
        validator ? &DrawExtentEstimator::ValidateVertexFetchRead : nullptr,
        validator ? this : nullptr);
  }

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader,
                              uint32_t y_scale = 1,
                              bool* input_reads_valid_out = nullptr);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y,
                        const Shader& vertex_shader, uint32_t y_scale = 1);

  // Executes the vertex shader on the CPU for every vertex of a small draw and
  // returns the screen-space bounds of the positions (viewport transform,
  // window offset and half-pixel offset applied) and the range of the
  // normalized depth. False if the draw can't be evaluated, has too many
  // vertices, reads rejected guest memory, or any vertex is killed or has a
  // non-positive W.
  bool EstimateVertexBounds(const Shader& vertex_shader, uint32_t max_vertices,
                            float& min_x_out, float& min_y_out,
                            float& max_x_out, float& max_y_out,
                            float& min_z_out, float& max_z_out,
                            float* xs_out = nullptr, float* ys_out = nullptr,
                            uint32_t* failure_reason_out = nullptr);

 private:
  bool ValidateInputRead(uint32_t address, uint32_t length,
                         InputReadKind kind) const {
    return !input_read_validator_ ||
           input_read_validator_(input_read_validator_context_, address,
                                 length, kind);
  }
  static bool ValidateVertexFetchRead(void* context, uint32_t address,
                                      uint32_t length) {
    return static_cast<DrawExtentEstimator*>(context)->ValidateInputRead(
        address, length, InputReadKind::kVertexFetch);
  }

  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_mask_ = 0;
      vertex_kill_.reset();
    }

    uint32_t position_mask() const { return position_mask_; }
    const float* position() const { return position_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    uint32_t position_mask_ = 0;
    float position_[4] = {};
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const Memory& memory_;
  TraceWriter* trace_writer_;
  InputReadValidator input_read_validator_ = nullptr;
  void* input_read_validator_context_ = nullptr;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_EXTENT_ESTIMATOR_H_
