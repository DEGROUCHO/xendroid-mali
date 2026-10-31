/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/cpu/ppc/ppc_emit.h"

DECLARE_bool(ppc_float_load_store_keep_snan);
DECLARE_bool(ppc_float_load_store_keep_snan_fast);

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

static void SetF64Bits(PPCContext* ctx, int reg, uint64_t bits) {
  std::memcpy(&ctx->f[reg], &bits, sizeof(bits));
}
static uint64_t GetF64Bits(PPCContext* ctx, int reg) {
  uint64_t bits;
  std::memcpy(&bits, &ctx->f[reg], sizeof(bits));
  return bits;
}
static bool IsDoubleNaN(uint64_t bits) {
  return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
         (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

#if XE_ARCH_ARM64
static uint32_t RunPackSingle(TestFunction& test, uint64_t input,
                              unsigned int rounding_mode) {
  uint32_t result = 0;
  test.Run(
      [&](PPCContext* ctx) {
        test.processors.front()->backend()->SetGuestRoundingMode(ctx,
                                                                  rounding_mode);
        SetF64Bits(ctx, 1, input);
      },
      [&](PPCContext* ctx) { result = static_cast<uint32_t>(ctx->r[3]); });
  return result;
}

static uint64_t RunUnpackSingle(TestFunction& test, uint32_t input,
                                unsigned int rounding_mode) {
  uint64_t result = 0;
  test.Run(
      [&](PPCContext* ctx) {
        test.processors.front()->backend()->SetGuestRoundingMode(ctx,
                                                                  rounding_mode);
        ctx->r[1] = input;
      },
      [&](PPCContext* ctx) { result = GetF64Bits(ctx, 3); });
  return result;
}

static uint64_t NextTestBits(uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545F4914F6CDD1Dull;
}
#endif

// double -> single (stfs). PowerPC keeps the signaling bit; the host convert
// would quiet it. The trailing inf case is the documented payload-truncation
// behavior (top 23 fraction bits zero -> single fraction zero -> infinity),
// which matches Xenon's stfs.
TEST_CASE("PACK_SINGLE_KEEP_NAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* sbits = ppc::PackSingleKeepNaN(b, LoadFPR(b, 1));
    StoreGPR(b, 3, b.ZeroExtend(sbits, INT64_TYPE));
    b.Return();
  });

  // {double input bits, expected single bits}
  const std::vector<std::pair<uint64_t, uint32_t>> cases = {
      {0x0000000000000000ull, 0x00000000u},  // +0
      {0x8000000000000000ull, 0x80000000u},  // -0
      {0x0000000000000001ull, 0x00000000u},  // smallest double subnormal
      {0x36A0000000000000ull, 0x00000001u},  // smallest single subnormal
      {0x380FFFFFC0000000ull, 0x007FFFFFu},  // largest single subnormal
      {0x3810000000000000ull, 0x00800000u},  // smallest single normal
      {0x47EFFFFFE0000000ull, 0x7F7FFFFFu},  // largest finite single
      {0x3FF0000000000000ull, 0x3F800000u},  // 1.0
      {0x3FF0000010000000ull, 0x3F800000u},  // halfway, even result
      {0x3FF0000010000001ull, 0x3F800001u},  // just above halfway
      {0xBFF0000010000000ull, 0xBF800000u},  // negative halfway, even result
      {0xC000000000000000ull, 0xC0000000u},  // -2.0
      {0x7FF0000000000000ull, 0x7F800000u},  // +inf
      {0xFFF0000000000000ull, 0xFF800000u},  // -inf
      {0x7FF8000000000000ull, 0x7FC00000u},  // qnan stays quiet
      {0x7FF4000000000000ull, 0x7FA00000u},  // snan stays signaling
      {0xFFF4000000000000ull, 0xFFA00000u},  // -snan stays signaling
      {0x7FF0000000000001ull, 0x7F800000u},  // snan payload truncated -> inf
  };
  for (const auto& c : cases) {
    INFO("double 0x" << std::hex << c.first);
    test.Run([&](PPCContext* ctx) { SetF64Bits(ctx, 1, c.first); },
             [&](PPCContext* ctx) {
               REQUIRE(static_cast<uint32_t>(ctx->r[3]) == c.second);
             });
  }
}

// single -> double (lfs). Widening never drops NaN-ness, so the only fixup is
// keeping the signaling bit the host convert would otherwise quiet.
TEST_CASE("UNPACK_SINGLE_KEEP_NAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    Value* sbits = b.Truncate(LoadGPR(b, 1), INT32_TYPE);
    StoreFPR(b, 3, ppc::UnpackSingleKeepNaN(b, sbits));
    b.Return();
  });

  // {single input bits, expected double bits}
  const std::vector<std::pair<uint32_t, uint64_t>> cases = {
      {0x00000000u, 0x0000000000000000ull},  // +0
      {0x80000000u, 0x8000000000000000ull},  // -0
      {0x00000001u, 0x36A0000000000000ull},  // smallest single subnormal
      {0x007FFFFFu, 0x380FFFFFC0000000ull},  // largest single subnormal
      {0x00800000u, 0x3810000000000000ull},  // smallest single normal
      {0x7F7FFFFFu, 0x47EFFFFFE0000000ull},  // largest finite single
      {0x3F800000u, 0x3FF0000000000000ull},  // 1.0
      {0xC0000000u, 0xC000000000000000ull},  // -2.0
      {0x7F800000u, 0x7FF0000000000000ull},  // +inf
      {0xFF800000u, 0xFFF0000000000000ull},  // -inf
      {0x7FC00000u, 0x7FF8000000000000ull},  // qnan stays quiet
      {0x7FA00000u, 0x7FF4000000000000ull},  // snan stays signaling
      {0xFFA00000u, 0xFFF4000000000000ull},  // -snan stays signaling
  };
  for (const auto& c : cases) {
    INFO("single 0x" << std::hex << c.first);
    test.Run([&](PPCContext* ctx) { ctx->r[1] = c.first; },
             [&](PPCContext* ctx) { REQUIRE(GetF64Bits(ctx, 3) == c.second); });
  }
}

// frsp is arithmetic, not data movement: PowerPC quiets signaling NaNs. It
// lowers to a plain double->single->double convert, so the NaN-preserving fix
// must stay scoped to lfs/stfs and leave this path quieting.
TEST_CASE("FRSP_QUIETS_SNAN", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Convert(LoadFPR(b, 1), FLOAT32_TYPE), FLOAT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { SetF64Bits(ctx, 1, 0x7FF4000000000000ull); },
           [](PPCContext* ctx) {
             uint64_t out = GetF64Bits(ctx, 3);
             INFO("frsp(snan) -> 0x" << std::hex << out);
             REQUIRE(IsDoubleNaN(out));
             REQUIRE((out & (1ull << 51)) != 0);  // quieted
           });
}
#if XE_ARCH_ARM64
// Differentially exercise the fast ARM64 lowering against the existing
// generic HIR implementation. Include all guest rounding/flush modes, exact
// subnormal and midpoint boundaries, NaNs with varied payloads, and a fixed
// randomized bit-pattern corpus.
TEST_CASE("LFS_STFS_FAST_MATCHES_GENERIC_HIR", "[instr][arm64]") {
  struct RestoreCvars {
    bool keep_snan;
    bool fast;
    ~RestoreCvars() {
      cvars::ppc_float_load_store_keep_snan = keep_snan;
      cvars::ppc_float_load_store_keep_snan_fast = fast;
    }
  } restore{cvars::ppc_float_load_store_keep_snan,
            cvars::ppc_float_load_store_keep_snan_fast};

  cvars::ppc_float_load_store_keep_snan = true;
  cvars::ppc_float_load_store_keep_snan_fast = false;
  TestFunction pack_generic([](HIRBuilder& b) {
    Value* sbits = ppc::PackSingleKeepNaN(b, LoadFPR(b, 1));
    StoreGPR(b, 3, b.ZeroExtend(sbits, INT64_TYPE));
    b.Return();
  });
  TestFunction unpack_generic([](HIRBuilder& b) {
    Value* sbits = b.Truncate(LoadGPR(b, 1), INT32_TYPE);
    StoreFPR(b, 3, ppc::UnpackSingleKeepNaN(b, sbits));
    b.Return();
  });
  // Force the generic functions to compile while their cvar selection is set.
  RunPackSingle(pack_generic, 0x3FF0000000000000ull, 0);
  RunUnpackSingle(unpack_generic, 0x3F800000u, 0);

  cvars::ppc_float_load_store_keep_snan_fast = true;
  TestFunction pack_fast([](HIRBuilder& b) {
    Value* sbits = ppc::PackSingleKeepNaN(b, LoadFPR(b, 1));
    StoreGPR(b, 3, b.ZeroExtend(sbits, INT64_TYPE));
    b.Return();
  });
  TestFunction unpack_fast([](HIRBuilder& b) {
    Value* sbits = b.Truncate(LoadGPR(b, 1), INT32_TYPE);
    StoreFPR(b, 3, ppc::UnpackSingleKeepNaN(b, sbits));
    b.Return();
  });
  // Force the fast functions to compile before changing any cvars again.
  RunPackSingle(pack_fast, 0x3FF0000000000000ull, 0);
  RunUnpackSingle(unpack_fast, 0x3F800000u, 0);

  const std::vector<uint64_t> pack_edges = {
      0x0000000000000000ull, 0x8000000000000000ull,
      0x0000000000000001ull, 0x36A0000000000000ull,
      0x380FFFFFC0000000ull, 0x3810000000000000ull,
      0x47EFFFFFE0000000ull, 0x3FF0000010000000ull,
      0x3FF0000010000001ull, 0xBFF0000010000000ull,
      0x7FF8000000000000ull, 0x7FF4000000000000ull,
      0xFFF4000000000000ull, 0x7FF0000000000001ull};
  const std::vector<uint32_t> unpack_edges = {
      0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu,
      0x00800000u, 0x7F7FFFFFu, 0x7F800000u, 0xFF800000u,
      0x7FC12345u, 0x7FA12345u, 0xFFA12345u};

  uint64_t state = 0xD1B54A32D192ED03ull;
  for (unsigned int mode = 0; mode < 8; ++mode) {
    for (uint64_t input : pack_edges) {
      INFO("stfs mode " << mode << ", double bits 0x" << std::hex << input);
      REQUIRE(RunPackSingle(pack_fast, input, mode) ==
              RunPackSingle(pack_generic, input, mode));
    }
    for (uint32_t input : unpack_edges) {
      INFO("lfs mode " << mode << ", single bits 0x" << std::hex << input);
      REQUIRE(RunUnpackSingle(unpack_fast, input, mode) ==
              RunUnpackSingle(unpack_generic, input, mode));
    }
    for (int i = 0; i < 16; ++i) {
      uint64_t input = NextTestBits(state);
      INFO("random stfs mode " << mode << ", double bits 0x" << std::hex
                               << input);
      REQUIRE(RunPackSingle(pack_fast, input, mode) ==
              RunPackSingle(pack_generic, input, mode));

      uint32_t single_input = static_cast<uint32_t>(NextTestBits(state));
      INFO("random lfs mode " << mode << ", single bits 0x" << std::hex
                              << single_input);
      REQUIRE(RunUnpackSingle(unpack_fast, single_input, mode) ==
              RunUnpackSingle(unpack_generic, single_input, mode));
    }
  }
}
#endif  // XE_ARCH_ARM64
