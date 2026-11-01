/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xe_aaudio_audio_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"

DEFINE_uint32(
    apu_aaudio_buffer_bursts, 4,
    "Depth of the Android audio buffer, in device bursts. Higher rides out "
    "emulator slowdowns without dropping audio, at the cost of latency.",
    "APU");
DEFINE_bool(apu_aaudio_dynamic_rate, true,
            "Slow playback toward 0.9x as the audio queue drains, so a guest "
            "that cannot keep real time bends pitch instead of popping.",
            "APU");
DEFINE_uint32(
    apu_aaudio_rebuffer_blocks, 3,
    "After the audio queue runs dry (and at start), play silence until this "
    "many guest blocks (5.3 ms each) are queued again, then fade back in. "
    "0 restores the old per-block concealment, which turns a guest slightly "
    "below real time into a click every few blocks.",
    "APU");
DEFINE_string(apu_dump_pcm, "",
              "Debug: write every submitted guest audio frame, raw, to this "
              "file (16-byte header XPCM, channels, rate, samples per channel; "
              "then the frames as the guest gave them: 5.1 sequential big "
              "endian float for games, interleaved stereo for the media "
              "player). Stops at 256 MB.",
              "APU");
DEFINE_bool(apu_aaudio_log_stats, false,
            "Log Android audio callback statistics (gaps, queue depth, "
            "underruns) once a second.",
            "APU");

namespace xe {
namespace apu {
namespace aaudio {

static constexpr uint32_t kStatsIntervalMs = 1000;

// Sink depth, in blocks, below which playback slows to buy the producer time.
// Just above apu_aaudio_rebuffer_blocks, so a refilled queue is held there.
static constexpr uint32_t kRateControlDepth = 4;

AAudioAudioDriver::AAudioAudioDriver(Memory* memory,
                                     xe::threading::Semaphore* semaphore,
                                     uint32_t frequency, uint32_t channels,
                                     bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion),
      channel_samples_(channels == 6 ? 256 : 768),
      submit_samples_(channels * (channels == 6 ? 256 : 768)),
      host_block_samples_(host_frame_channels_ *
                          (channels == 6 ? 256 : 768)) {
  assert_true(channels == 6 || channels == 2);
  last_block_.resize(host_block_samples_, 0.0f);
}

AAudioAudioDriver::~AAudioAudioDriver() {
  assert_true(!frames_queued_.Size());
  assert_true(!frames_unused_.Size());
}

bool AAudioAudioDriver::Initialize() {
  for (int i = 0; i < 2; i++) {
    frames_unused_.Push(new float[submit_samples_]);
  }

  {
    std::unique_lock<std::mutex> stream_guard(stream_mutex_);
    if (!BuildStream()) {
      return false;
    }
  }

  // Worker that rebuilds the stream on output-device changes.
  recovery_thread_ = std::thread(&AAudioAudioDriver::RecoveryThreadMain, this);
  return true;
}

bool AAudioAudioDriver::BuildStream() {
  // Open on the current default device (no setDeviceId); fall back to SHARED if
  // the new device won't grant an exclusive MMAP stream.
  const aaudio_sharing_mode_t modes[] = {AAUDIO_SHARING_MODE_EXCLUSIVE,
                                         AAUDIO_SHARING_MODE_SHARED};
  for (aaudio_sharing_mode_t mode : modes) {
    aaudio_result_t result = AAudio_createStreamBuilder(&builder_);
    if (result != AAUDIO_OK) {
      XELOGE("AAudio_createStreamBuilder failed: {}", result);
      return false;
    }

    AAudioStreamBuilder_setFormat(builder_, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(builder_, frame_frequency_);
    AAudioStreamBuilder_setChannelCount(builder_, host_frame_channels_);
    AAudioStreamBuilder_setFramesPerDataCallback(builder_, channel_samples_);
    AAudioStreamBuilder_setDataCallback(builder_, AudioCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder_, AudioErrorCallback, this);
    AAudioStreamBuilder_setPerformanceMode(builder_,
                                           AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder_, mode);
    // A low-latency stream is otherwise granted the shallowest buffer the
    // device allows. AAudio requires capacity >= twice the callback size.
    AAudioStreamBuilder_setBufferCapacityInFrames(
        builder_, channel_samples_ * cvars::apu_aaudio_buffer_bursts);

    result = AAudioStreamBuilder_openStream(builder_, &stream_);
    if (result != AAUDIO_OK) {
      XELOGE("AAudioStreamBuilder_openStream ({}) failed: {}",
             mode == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared",
             result);
      AAudioStreamBuilder_delete(builder_);
      builder_ = nullptr;
      continue;
    }

    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      XELOGE("AAudioStream_requestStart failed: {}", result);
      AAudioStream_close(stream_);
      stream_ = nullptr;
      AAudioStreamBuilder_delete(builder_);
      builder_ = nullptr;
      continue;
    }

    // Size against the larger of burst and granted callback: a device can
    // burst at 96 yet call back for 256, leaving under two callbacks of
    // headroom if only the burst is used.
    const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    const int32_t callback_frames =
        AAudioStream_getFramesPerDataCallback(stream_);
    const int32_t drain = std::max(
        burst, callback_frames > 0 ? callback_frames
                                   : static_cast<int32_t>(channel_samples_));
    if (drain > 0) {
      AAudioStream_setBufferSizeInFrames(
          stream_, drain * cvars::apu_aaudio_buffer_bursts);
    }

    // Requested and granted config frequently differ.
    XELOGI(
        "AAudio: {} stream, rate {}, {} ch, burst {}, callback {} (asked {}), "
        "buffer {}/{} frames, perf mode {}",
        mode == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared",
        AAudioStream_getSampleRate(stream_),
        AAudioStream_getChannelCount(stream_), burst,
        AAudioStream_getFramesPerDataCallback(stream_), channel_samples_,
        AAudioStream_getBufferSizeInFrames(stream_),
        AAudioStream_getBufferCapacityInFrames(stream_),
        AAudioStream_getPerformanceMode(stream_));

    stream_initialized_ = true;
    return true;
  }
  return false;
}

void AAudioAudioDriver::Pause() {
  std::unique_lock<std::mutex> stream_guard(stream_mutex_);
  if (stream_initialized_ && stream_) {
    AAudioStream_requestPause(stream_);
  }
}

void AAudioAudioDriver::Resume() {
  std::unique_lock<std::mutex> stream_guard(stream_mutex_);
  // Resuming at the rate the drained queue asked for would play slow; block
  // position and resampler history carry over, so playback stays continuous.
  rate_ = 1.0f;
  conceal_gain_ = 1.0f;
  gap_blocks_ = 0;
  rebuffering_ = true;
  stat_rate_milli_.store(1000, std::memory_order_relaxed);
  if (stream_initialized_ && stream_) {
    AAudioStream_requestStart(stream_);
  }
}

size_t AAudioAudioDriver::GetQueuedFrameCount() {
  return frames_queued_.Size();
}

void AAudioAudioDriver::SetVolume(float volume) {
  // AAudio has no per-stream volume; applied to the samples in the callback.
  driver_volume_.store(std::clamp(volume, 0.0f, 1.0f),
                       std::memory_order_relaxed);
}

aaudio_data_callback_result_t AAudioAudioDriver::AudioCallback(
    AAudioStream* stream,
    void* userdata,
    void* audioData,
    int32_t numFrames) {
  SCOPE_profile_cpu_f("apu");

  auto driver = static_cast<AAudioAudioDriver*>(userdata);
  float* output_buffer = reinterpret_cast<float*>(audioData);

  // setFramesPerDataCallback is only a request, renegotiated on the
  // shared-mode fallback and on every rebuild. The conversion below uses
  // channel_samples_ as its source stride, so it must run at that size
  // whatever this callback was handed.
  if (numFrames != static_cast<int32_t>(driver->channel_samples_)) {
    driver->stat_unexpected_frames_.store(numFrames, std::memory_order_relaxed);
  }

  driver->stat_callbacks_.fetch_add(1, std::memory_order_relaxed);

  // Queue depth steers the resample rate, slewed to avoid zipper noise: a
  // producer below real time bends pitch instead of gapping.
  const uint32_t depth_now = driver->frames_queued_.Size();
  // Only once the sink is close to running dry, and always below the depth the
  // producer refills to (AudioSystem's top-up line) - bending pitch at a depth
  // the producer is content with leaves the rate permanently modulated.
  float rate_target = 1.0f;
  if (cvars::apu_aaudio_dynamic_rate && depth_now < kRateControlDepth) {
    rate_target =
        std::max(0.90f, 1.0f - 0.05f * float(kRateControlDepth - depth_now));
  }
  driver->rate_ +=
      std::clamp(rate_target - driver->rate_, -0.003f, 0.003f);
  driver->stat_rate_milli_.store(
      static_cast<uint32_t>(driver->rate_ * 1000.0f + 0.5f),
      std::memory_order_relaxed);

  int32_t frames_done = 0;
  uint32_t releases = 0;
  bool gapped = false;
  while (frames_done < numFrames) {
    while (driver->resample_frac_ >= 1.0f) {
      driver->resample_frac_ -= 1.0f;
      if (driver->last_block_pos_ >= driver->channel_samples_) {
        driver->LoadNextBlock(releases, gapped);
      }
      driver->prev_l_ = driver->cur_l_;
      driver->prev_r_ = driver->cur_r_;
      driver->cur_l_ = driver->last_block_[driver->last_block_pos_ * 2 + 0];
      driver->cur_r_ = driver->last_block_[driver->last_block_pos_ * 2 + 1];
      driver->last_block_pos_++;
    }
    const float f = driver->resample_frac_;
    output_buffer[frames_done * 2 + 0] =
        driver->prev_l_ + f * (driver->cur_l_ - driver->prev_l_);
    output_buffer[frames_done * 2 + 1] =
        driver->prev_r_ + f * (driver->cur_r_ - driver->prev_r_);
    frames_done++;
    driver->resample_frac_ += driver->rate_;
  }

  // One tick per block consumed, so pacing holds when the callback size is
  // not a whole block. Tick once on underrun to keep the guest engine running.
  if (releases == 0 && gapped) {
    releases = 1;
  }
  for (uint32_t i = 0; i < releases; ++i) {
    driver->semaphore_->Release(1, nullptr);
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioAudioDriver::LoadNextBlock(uint32_t& releases, bool& gapped) {
  const uint32_t depth = frames_queued_.Size();
  stat_queue_depth_sum_.fetch_add(depth, std::memory_order_relaxed);
  if (depth > stat_queue_depth_max_.load(std::memory_order_relaxed)) {
    stat_queue_depth_max_.store(depth, std::memory_order_relaxed);
  }
  const uint32_t rebuffer = cvars::apu_aaudio_rebuffer_blocks;
  if (rebuffering_ && depth >= std::max(rebuffer, 1u)) {
    rebuffering_ = false;
  }
  float* buffer = rebuffering_ ? nullptr : frames_queued_.Pop();
  if (!buffer) {
    stat_gaps_.fetch_add(1, std::memory_order_relaxed);
    gapped = true;
    if (rebuffer && !rebuffering_) {
      stat_rebuffers_.fetch_add(1, std::memory_order_relaxed);
      rebuffering_ = true;
    }
    ConcealNextBlock();
    last_block_pos_ = 0;
    return;
  }
  if (frame_channels_ == 6) {
    conversion::sequential_6_BE_to_interleaved_2_LE(last_block_.data(), buffer,
                                                    channel_samples_);
  } else {
    // Media player: already interleaved host endian stereo.
    std::memcpy(last_block_.data(), buffer,
                host_block_samples_ * sizeof(float));
  }
  ApplyGainAndClamp();
  ApplyFadeIn();
  last_block_valid_ = true;
  last_block_pos_ = 0;
  gap_blocks_ = 0;
  conceal_gain_ = 1.0f;
  // Cannot fail: the ring holds every buffer ever allocated (SubmitFrame).
  frames_unused_.Push(buffer);
  ++releases;
}

void AAudioAudioDriver::ConcealNextBlock() {
  // Nothing to repeat yet: startup, or straight after a mute.
  if (!last_block_valid_) {
    std::memset(last_block_.data(), 0, host_block_samples_ * sizeof(float));
    return;
  }

  // Jitter buffer on: fade the last block out once, then silence until the
  // queue has refilled - no repeated block, so no buzz at the block rate.
  if (cvars::apu_aaudio_rebuffer_blocks) {
    const int32_t frames = static_cast<int32_t>(channel_samples_);
    for (int32_t f = 0; f < frames; ++f) {
      const float g = 1.0f - static_cast<float>(f + 1) / frames;
      last_block_[f * 2 + 0] *= g;
      last_block_[f * 2 + 1] *= g;
    }
    // The faded block is played once; then plain silence.
    last_block_valid_ = false;
    gap_blocks_++;
    fade_in_pending_ = true;
    return;
  }

  // Repeat the last block, decaying it in place: held flat it would buzz at
  // the block rate, decayed it fades out instead of slamming to silence.
  conceal_gain_ *= 0.6f;
  if (conceal_gain_ < 0.002f) {
    std::memset(last_block_.data(), 0, host_block_samples_ * sizeof(float));
    last_block_valid_ = false;
    gap_blocks_++;
    fade_in_pending_ = true;
    return;
  }
  const int32_t frames = static_cast<int32_t>(channel_samples_);
  const float step = frames > 0 ? (0.6f - 1.0f) / frames : 0.0f;
  for (int32_t f = 0; f < frames; ++f) {
    const float g = 1.0f + step * f;
    last_block_[f * 2 + 0] *= g;
    last_block_[f * 2 + 1] *= g;
  }
  gap_blocks_++;
  fade_in_pending_ = true;
}

void AAudioAudioDriver::ApplyGainAndClamp() {
  const uint32_t master = std::min<uint32_t>(cvars::volume, 100);
  const float gain =
      driver_volume_.load(std::memory_order_relaxed) * (master / 100.0f);

  // The 5.1->2.0 fold peaks at ~2.9 gain, so loud content (gunfire,
  // explosions) exceeds full scale, and hard clipping it sounds like blown
  // speakers. Limit instead: the gain drops within 32 frames to keep the block
  // peak at kLimit and recovers over ~100 ms, ramped across the block so it
  // doesn't zipper; the residual overshoot of the attack is soft-clipped.
  // Content below kLimit passes untouched. Count the samples that were over full
  // scale before limiting.
  constexpr float kLimit = 0.89f;
  // Above kLimit: only the attack ramp's overshoot reaches the soft clip, so
  // loud content below the limit is not saturated (that was audible as grit).
  constexpr float kKnee = 0.9f;
  // Gain drops over the first 32 frames instead of across the whole block,
  // so a peak early in the block is not let through at the old gain.
  constexpr uint32_t kAttackSamples = 64;
  constexpr float kReleasePerBlock = 0.05f;
  float peak = 0.0f;
  uint32_t clipped = 0;
  for (uint32_t i = 0; i < host_block_samples_; ++i) {
    float a = std::abs(last_block_[i] * gain);
    peak = std::max(peak, a);
    clipped += a > 1.0f;
  }
  const float start_gain = limiter_gain_;
  const float wanted = peak > kLimit ? kLimit / peak : 1.0f;
  limiter_gain_ = wanted < limiter_gain_
                      ? wanted
                      : std::min(wanted, limiter_gain_ + kReleasePerBlock);
  const uint32_t ramp = limiter_gain_ < start_gain
                            ? std::min(kAttackSamples, host_block_samples_)
                            : host_block_samples_;
  const float gain_step = (limiter_gain_ - start_gain) / float(std::max(ramp, 1u));
  float g = start_gain;
  for (uint32_t i = 0; i < host_block_samples_; ++i) {
    float s = last_block_[i] * gain * g;
    // Stereo pairs share a gain: step every second sample.
    if (i < ramp && (i & 1)) {
      g += 2.0f * gain_step;
    }
    float a = std::abs(s);
    if (a > kKnee) {
      // Smoothly saturates towards 1.0 instead of a hard corner.
      a = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
      s = std::copysign(a, s);
    }
    last_block_[i] = s;
  }
  if (clipped) {
    stat_clipped_.fetch_add(clipped, std::memory_order_relaxed);
  }
}

void AAudioAudioDriver::ApplyFadeIn() {
  if (!fade_in_pending_) {
    return;
  }
  fade_in_pending_ = false;
  // Ramp the first real block back in so the recovery edge is a slope, not a
  // step. 64 frames is ~1.3ms: inaudible as a level change, enough to kill the
  // click.
  const int32_t ramp = 64;
  for (int32_t f = 0; f < ramp; ++f) {
    const float g = static_cast<float>(f) / ramp;
    last_block_[f * 2 + 0] *= g;
    last_block_[f * 2 + 1] *= g;
  }
}

void AAudioAudioDriver::LogAndResetStats() {
  const uint64_t callbacks = stat_callbacks_.exchange(0, std::memory_order_relaxed);
  if (!callbacks) {
    return;
  }
  const uint64_t gaps = stat_gaps_.exchange(0, std::memory_order_relaxed);
  const uint64_t rebuffers =
      stat_rebuffers_.exchange(0, std::memory_order_relaxed);
  const uint64_t depth_sum =
      stat_queue_depth_sum_.exchange(0, std::memory_order_relaxed);
  const uint32_t depth_max =
      stat_queue_depth_max_.exchange(0, std::memory_order_relaxed);
  const int32_t odd_frames =
      stat_unexpected_frames_.exchange(0, std::memory_order_relaxed);
  const uint64_t clipped = stat_clipped_.exchange(0, std::memory_order_relaxed);
  const uint64_t played = (callbacks - gaps) * host_block_samples_;

  int32_t xruns = -1;
  {
    std::unique_lock<std::mutex> stream_guard(stream_mutex_);
    if (stream_initialized_ && stream_) {
      xruns = AAudioStream_getXRunCount(stream_);
    }
  }

  XELOGI(
      "AAudio: {} cb, {} gaps ({:.1f}%), {} rebuffers, queue avg {:.2f} max {}, rate {:.3f}, "
      "xruns {}, clipped {} ({:.3f}%){}",
      callbacks, gaps, 100.0 * double(gaps) / double(callbacks), rebuffers,
      double(depth_sum) / double(callbacks), depth_max,
      stat_rate_milli_.load(std::memory_order_relaxed) / 1000.0, xruns,
      clipped,
      played ? 100.0 * double(clipped) / double(played) : 0.0,
      odd_frames ? fmt::format(", UNEXPECTED framesPerCallback {}", odd_frames)
                 : "");
}

void AAudioAudioDriver::AudioErrorCallback(
    AAudioStream* stream,
    void* userdata,
    aaudio_result_t error) {
  auto driver = static_cast<AAudioAudioDriver*>(userdata);
  // A route change (headphones/BT) disconnects the stream; it must be reopened
  // on the new default device. AAudio forbids closing the stream from this
  // callback, so just flag a request for recovery_thread_.
  XELOGW("AAudio stream error: {} - requesting stream rebuild", error);
  {
    std::lock_guard<std::mutex> lk(driver->recovery_mutex_);
    driver->restart_requested_ = true;
  }
  driver->recovery_cv_.notify_one();
}

void AAudioAudioDriver::RecoveryThreadMain() {
  bool retry_pending = false;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(recovery_mutex_);
      auto wake = [this] { return restart_requested_ || recovery_quit_; };
      // A failed rebuild leaves no stream to re-trigger us, so poll to retry.
      // The callback cannot log from a realtime thread, so publish for it.
      if (retry_pending) {
        recovery_cv_.wait_for(lk, std::chrono::milliseconds(250), wake);
      } else {
        recovery_cv_.wait_for(lk, std::chrono::milliseconds(kStatsIntervalMs),
                              wake);
      }
      if (recovery_quit_) {
        return;
      }
      if (!restart_requested_) {
        lk.unlock();
        if (cvars::apu_aaudio_log_stats) {
          LogAndResetStats();
        }
        continue;
      }
      restart_requested_ = false;
    }
    retry_pending = !RestartStream();
  }
}

bool AAudioAudioDriver::RestartStream() {
  std::unique_lock<std::mutex> stream_guard(stream_mutex_);
  XELOGW("AAudio: rebuilding stream on the current default output device");
  // Safe here (not the callback thread): close() blocks until the data callback
  // returns.
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }
  if (builder_) {
    AAudioStreamBuilder_delete(builder_);
    builder_ = nullptr;
  }
  stream_initialized_ = false;

  // Shutting down: tear down only, don't reopen.
  if (shutting_down_.load(std::memory_order_acquire)) {
    return true;
  }

  // The new stream's data callback resumes the pacing semaphore on its own, so
  // the AudioSystem worker recovers without changes.
  if (BuildStream()) {
    XELOGI("AAudio: stream rebuilt; audio output restored");
    return true;
  }
  XELOGE("AAudio: stream rebuild failed; will retry");
  return false;
}

void AAudioAudioDriver::SubmitFrame(float* samples) {
  float* output_frame = frames_unused_.Pop();
  if (!output_frame) {
    output_frame = new float[submit_samples_];
  }

  std::memcpy(output_frame, samples, submit_samples_ * sizeof(float));

  if (!cvars::apu_dump_pcm.empty() && !dump_done_) {
    if (!dump_file_) {
      dump_file_ = std::fopen(cvars::apu_dump_pcm.c_str(), "wb");
      if (!dump_file_) {
        XELOGE("AAudio: cannot open apu_dump_pcm {}", cvars::apu_dump_pcm);
        dump_done_ = true;
      } else {
        // Big buffer: the audio worker must not stall on storage.
        std::setvbuf(dump_file_, nullptr, _IOFBF, 1 << 20);
        const uint32_t header[4] = {0x4D435058u /* "XPCM" */, frame_channels_,
                                    frame_frequency_, channel_samples_};
        std::fwrite(header, sizeof(header), 1, dump_file_);
      }
    }
    if (dump_file_) {
      std::fwrite(samples, sizeof(float), submit_samples_, dump_file_);
      dump_bytes_ += submit_samples_ * sizeof(float);
      if (dump_bytes_ >= (256ull << 20)) {
        std::fclose(dump_file_);
        dump_file_ = nullptr;
        dump_done_ = true;
        XELOGI("AAudio: apu_dump_pcm complete ({} MB)", dump_bytes_ >> 20);
      }
    }
  }

  if (!frames_queued_.Push(output_frame)) {
    // Queue full (the worker never queues this deep): drop the frame. Freed
    // here, not recycled, so at most FrameRing::kSize buffers ever exist and
    // the callback's push into frames_unused_ cannot fail.
    delete[] output_frame;
  }
}

void AAudioAudioDriver::Shutdown() {
  // Stop and join the recovery worker before tearing down the stream, so no
  // rebuild is in flight while we do.
  shutting_down_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(recovery_mutex_);
    recovery_quit_ = true;
  }
  recovery_cv_.notify_one();
  if (recovery_thread_.joinable()) {
    recovery_thread_.join();
  }

  {
    std::unique_lock<std::mutex> stream_guard(stream_mutex_);
    if (stream_) {
      AAudioStream_requestStop(stream_);
      AAudioStream_close(stream_);
      stream_ = nullptr;
    }

    if (builder_) {
      AAudioStreamBuilder_delete(builder_);
      builder_ = nullptr;
    }

    stream_initialized_ = false;
  }

  if (dump_file_) {
    std::fclose(dump_file_);
    dump_file_ = nullptr;
  }

  // The stream is closed: no callback runs any more.
  while (float* frame = frames_unused_.Pop()) {
    delete[] frame;
  }
  while (float* frame = frames_queued_.Pop()) {
    delete[] frame;
  }
}

}  // namespace aaudio
}  // namespace apu
}  // namespace xe
