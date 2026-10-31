/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XMA_CONTEXT_NEW_H_
#define XENIA_APU_XMA_CONTEXT_NEW_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>

#include "xenia/apu/xma_context.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

// Forward declarations
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xe {
namespace apu {

struct kPacketInfo {
  uint8_t frame_count_;
  uint8_t current_frame_;
  uint32_t current_frame_size_;

  const bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

struct kPacketHandle {
  uint32_t buffer_index_ = 0;
  uint32_t packet_index_ = 0;
  bool is_valid_ = false;
};

// Opt-in diagnostics for the new XMA decoder. These are known component
// sizes only and must not be presented as the decoder's total heap use.
struct XmaContextMemorySnapshot {
  uint32_t id = 0;
  bool allocated = false;
  bool enabled = false;
  bool codec_context_present = false;
  bool codec_open = false;
  bool codec_private_size_known = false;
  bool owned_buffer_sizes_complete = true;
  int32_t sample_rate = 0;
  int32_t channels = 0;
  uint64_t fixed_context_bytes = 0;
  uint64_t codec_context_struct_bytes = 0;
  uint64_t codec_private_bytes = 0;
  uint64_t packet_struct_bytes = 0;
  uint64_t frame_struct_bytes = 0;
  // AVBufferRef window bytes for the packet, not backing-allocation size.
  uint64_t packet_capacity_bytes = 0;
  // Unique matching AVFrame AVBufferRef window bytes not counted via packet.
  uint64_t frame_unique_additional_bytes = 0;
  uint64_t q314_scratch_model_bytes = 0;
};

struct XmaOwnedBufferMemory {
  // AVBufferRef window bytes; not a generic backing-allocation census.
  uint64_t packet_capacity_bytes = 0;
  uint64_t frame_unique_additional_bytes = 0;
  bool complete = true;
};

struct XmaMemoryCensus {
  uint32_t context_slots = 0;
  uint32_t allocated_slots = 0;
  uint32_t enabled_slots = 0;
  uint32_t codec_contexts = 0;
  uint32_t open_codecs = 0;
  uint32_t open_mono = 0;
  uint32_t open_stereo = 0;
  uint32_t open_other_channels = 0;
  uint32_t open_unknown_rate = 0;
  uint32_t codec_private_known = 0;
  uint32_t codec_private_unknown = 0;
  uint32_t owned_buffer_incomplete = 0;
  uint64_t fixed_context_bytes = 0;
  uint64_t codec_context_struct_bytes = 0;
  uint64_t codec_private_bytes = 0;
  uint64_t packet_struct_bytes = 0;
  uint64_t frame_struct_bytes = 0;
  // Sums of known AVBufferRef window sizes, not backing allocation sizes.
  uint64_t packet_capacity_bytes = 0;
  uint64_t frame_unique_additional_bytes = 0;
  // q314 WMAPro scratch estimate; a model, not an XMAFrames allocation size.
  uint64_t q314_scratch_model_bytes = 0;
  std::array<std::array<uint32_t, 4>, 2> open_channel_rate_counts{};
};

// Shared by the snapshot implementation and its focused unit tests.
XmaOwnedBufferMemory GetXmaOwnedBufferMemory(const AVPacket* packet,
                                            const AVFrame* frame);
bool GetXmaCodecPrivateAllocationBytes(const AVCodecContext* context,
                                      const AVCodec* codec,
                                      uint64_t* bytes);
XmaContextMemorySnapshot BuildXmaContextMemorySnapshot(
    uint32_t id, bool allocated, bool enabled, uint64_t fixed_context_bytes,
    AVCodecContext* codec_context, const AVCodec* codec,
    const AVPacket* packet, const AVFrame* frame);
uint64_t GetQ314XmaScratchModelBytes(uint32_t channels);
void AccumulateXmaContextSnapshot(const XmaContextMemorySnapshot& snapshot,
                                 XmaMemoryCensus* census);

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

// Per-second census of Work() outcomes, see xma_context_new.cc.
void GetXmaBailStats(uint64_t* no_output, uint64_t* consume_empty,
                     uint64_t* no_space, uint64_t* decoded);

class XmaContextNew : public XmaContext {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  explicit XmaContextNew();
  ~XmaContextNew();

  int Setup(uint32_t id, Memory* memory, uint32_t guest_ptr);
  bool Work();
  XmaContextMemorySnapshot SnapshotMemoryStats();

  void Enable();
  void Clear();
  void Disable();
  void Release();

 private:
  void ClearLocked(XMA_CONTEXT_DATA* data);
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  // Convert sampling rate from ID to frequency.
  static int GetSampleRate(int id);
  // Get the containing packet number of the frame pointed to by the offset.
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);

  const kPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);

  const uint32_t GetAmountOfBitsToRead(const uint32_t remaining_stream_bits,
                                       const uint32_t frame_size);

  kPacketHandle GetPacketHandle(XMA_CONTEXT_DATA* data, uint32_t buffer_index,
                                uint32_t packet_index,
                                uint32_t current_input_packet_count);

  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data,
                               uint32_t next_packet_index,
                               uint32_t current_input_packet_count);

  // Single-buffer version of GetNextPacketReadOffset: scans `buffer`
  // from start_packet_index for the first packet whose frame_offset
  // header is valid (<= kMaxFrameSizeinBits).
  // Returns the bit offset of that frame in the buffer,
  // Or kBitsPerPacketHeader when no valid frame is found.
  const uint32_t GetNextPacketReadOffset(uint8_t* buffer,
                                         uint32_t next_packet_index,
                                         uint32_t current_input_packet_count);

  const uint32_t GetNextPacketReadOffset(XMA_CONTEXT_DATA* data,
                                         uint32_t next_packet_index,
                                         uint32_t current_input_packet_count);

  // Returns currently used buffer
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);

  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(RingBuffer* XE_RESTRICT output_rb,
               const XMA_CONTEXT_DATA* const XE_RESTRICT data);

  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  bool PreparePacket(const uint32_t frame_size, const uint32_t frame_padding,
                     size_t packet_size);

  RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  bool DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                    AVFrame* av_frame);

  // Re-reads context from guest memory and merges only decoder-owned fields,
  // preserving any game modifications made during decoding.
  void StoreContextMerged(const XMA_CONTEXT_DATA& data,
                          const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_;
  // first byte contains bit offset information
  std::array<uint8_t, 1 + 4096> xma_frame_;
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;

  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;

  // Loop subframe precision state.
  // Maximum subframe blocks to output from the current frame (loop end
  // truncation).  0 means no limit.
  uint8_t loop_frame_output_limit_ = 0;
  // When true, the next decoded frame should skip leading subframes per
  // loop_subframe_skip (loop start adjustment).
  bool loop_start_skip_pending_ = false;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XMA_CONTEXT_H_
