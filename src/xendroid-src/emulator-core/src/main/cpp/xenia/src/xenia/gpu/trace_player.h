/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_TRACE_PLAYER_H_
#define XENIA_GPU_TRACE_PLAYER_H_

#include <atomic>
#include <string>
#include <vector>

#include "xenia/base/threading.h"
#include "xenia/gpu/trace_protocol.h"
#include "xenia/gpu/trace_reader.h"

namespace xe {
namespace gpu {

class GraphicsSystem;

enum class TracePlaybackMode {
  kUntilEnd,
  kBreakOnSwap,
};

class TracePlayer : public TraceReader {
 public:
  TracePlayer(GraphicsSystem* graphics_system);

  GraphicsSystem* graphics_system() const { return graphics_system_; }
  int current_frame_index() const { return current_frame_index_; }
  int current_command_index() const { return current_command_index_; }
  bool is_playing_trace() const { return playing_trace_; }
  const Frame* current_frame() const;

  // Only valid if playing_trace is true.
  // Scalar from 0-10000
  uint32_t playback_percent() const { return playback_percent_; }

  void SeekFrame(int target_frame);
  void SeekCommand(int target_command);

  void WaitOnPlayback();

  // XenDroid replay loop: plays one recorded frame again and again on the
  // command processor thread until `stop` is set. From the second iteration
  // guest memory is only rewritten (and invalidated) where the recorded
  // contents differ from what is already there, and from the fourth only the
  // reads seen to differ are even looked at, so the steady state costs the GPU
  // what the frame itself costs and the CPU little more than the packets.
  void LoopFrame(int frame_index, const std::atomic<bool>* stop);

 private:
  void PlayTrace(const uint8_t* trace_data, size_t trace_size,
                 TracePlaybackMode playback_mode, bool clear_caches);
  void PlayTraceOnThread(const uint8_t* trace_data, size_t trace_size,
                         TracePlaybackMode playback_mode, bool clear_caches,
                         uint32_t loop_pass = 0);

  GraphicsSystem* graphics_system_;
  int current_frame_index_;
  int current_command_index_;
  bool playing_trace_ = false;
  std::atomic<uint32_t> playback_percent_ = {0};
  std::unique_ptr<xe::threading::Event> playback_event_;
  std::vector<uint8_t> loop_scratch_;
  // Per memory read of the looped frame: whether its guest range was found
  // changed when the read came up again (a buffer rewritten within the frame).
  // Learned in loop passes 1 and 2; later passes skip all other reads without
  // decompressing them.
  struct LoopReadPage {
    uint32_t offset;
    uint32_t size;
    std::vector<uint8_t> data;
  };
  // Indexed by the order of the memory reads in the looped frame: the 4 KiB
  // pages of each read whose guest contents were found changed when the read
  // came up again (buffers rewritten within the frame), with their recorded
  // data, so steady passes restore them without decompressing anything.
  std::vector<std::vector<LoopReadPage>> loop_read_pages_;
  uint64_t loop_stat_pages_restored_ = 0;
  uint64_t loop_stat_pages_learned_ = 0;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_TRACE_PLAYER_H_
