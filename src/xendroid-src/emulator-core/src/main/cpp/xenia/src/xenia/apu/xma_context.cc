/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/apu/xma_context.h"

#include <cmath>
#include <cstring>
#if XE_ARCH_ARM64
#include <arm_neon.h>
#endif

#include "xenia/apu/xma_decoder.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace xe {
namespace apu {

XmaContext::XmaContext()
    : work_completion_event_(
          xe::threading::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {}

void XmaContext::DumpRaw(AVFrame* frame, int id) {
  FILE* outfile =
      xe::filesystem::OpenFile(fmt::format("out{}.raw", id).c_str(), "ab");
  if (!outfile) {
    return;
  }
  size_t data_size = sizeof(float);
  for (int i = 0; i < frame->nb_samples; i++) {
    for (int ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
      fwrite(frame->data[ch] + data_size * i, 1, data_size, outfile);
    }
  }
  fclose(outfile);
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if XE_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel && samples[1] != nullptr) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask =
        _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask =
        _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#elif XE_ARCH_ARM64
  // As the AMD64 path: saturate, round to nearest (not truncate) and byte
  // swap 4 samples at a time; two channels are interleaved by the store.
  static_assert(kSamplesPerFrame % 4 == 0);
  const float32x4_t min_mm = vdupq_n_f32(-1.0f);
  const float32x4_t max_mm = vdupq_n_f32(1.0f);
  const float32x4_t scale_mm = vdupq_n_f32(scale);
  auto convert4 = [&](const float* in) {
    float32x4_t v = vminq_f32(vmaxq_f32(vld1q_f32(in), min_mm), max_mm);
    int16x4_t s = vqmovn_s32(vcvtnq_s32_f32(vmulq_f32(v, scale_mm)));
    return vreinterpret_s16_u8(vrev16_u8(vreinterpret_u8_s16(s)));
  };
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  if (is_two_channel && samples[1] != nullptr) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      int16x4x2_t pair = {{convert4(&in_channel_0[i]),
                           convert4(&in_channel_1[i])}};
      vst2_s16(&out[i * 2], pair);
    }
  } else {
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      vst1_s16(&out[i], convert4(&in_channel_0[i]));
    }
  }
#else
  // Match the AMD64 path: a frame flagged two-channel whose second channel
  // buffer is absent is emitted as mono rather than dereferencing null.
  const bool interleave_two = is_two_channel && samples[1] != nullptr;
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(interleave_two); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = xe::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(std::lrintf(scaled_sample));
      out[o++] = xe::byte_swap(sample);
    }
  }
#endif
}

}  // namespace apu
}  // namespace xe
