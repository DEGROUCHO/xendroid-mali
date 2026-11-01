// SPDX-License-Identifier: WTFPL
#include "xenia/game_quirks.h"

#include <array>
#include <string>
#include <variant>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

namespace xe {
namespace game_quirks {

using QuirkValue = std::variant<bool, int64_t, double, const char*>;

struct Quirk {
  uint32_t title_id;
  const char* cvar;
  QuirkValue value;
  const char* note;
};

// Entries apply at per-game-config priority, so a user's own per-game config
// still overrides them.
static const Quirk kQuirks[] = {
    // Ninja Gaiden 2: the opening FMV renders as three time-skewed vertical
    // slices when the restore half of the saverest inline is active. Every
    // emitted expansion verifies correct, so the current reading is a latent
    // guest-side race between the slice decode workers that the faster
    // epilogues expose. Keep the save half inlined, call the restores.
    {0x544307D5, "inline_gprlr_saverest_parts", int64_t(1),
     "FMV slice desync with inlined restores"},
    {0x4D5307D1, "spirv_multiply_zero_test_on_bits", true,
     "ir3 cannot compile fmadz"},
    {0x494707D4, "vulkan_in_pass_resolve", true, "in-pass resolves tuned here"},
    {0x49470804, "vulkan_in_pass_resolve", true, "in-pass resolves tuned here"},
    {0x494707D4, "network_enabled", false, "hangs on a blocking recvfrom"},
    {0x49470804, "network_enabled", false, "hangs on a blocking recvfrom"},
    {0x4E4D083A, "spirv_multiply_zero_test_on_bits", true,
     "ir3 cannot compile fmadz"},
    // Halo: Reach keeps distinct data in the samples of its 4x surfaces and
    // reinterprets them at other sample counts (16_16 4x as 8_8_8_8 1x):
    // single-sample storage turns all lit geometry into per-pixel noise.
    {0x4D53085B, "vulkan_msaa_single_sample_storage", false,
     "per-sample data in 4x surfaces"},
    // WWE 2K14 checks that a ranged physical allocation starts exactly at the
    // low end of the range it asked for; with the 4 KiB offset it crashes as a
    // match starts (menus work). Same fix as xenia-canary's compatibility
    // report for this title.
    {0x545408B2, "ignore_offset_for_ranged_allocations", true,
     "crash as a match starts"},
    // No vulkan_bindless_textures for GTA IV: with it Mali raises ~15 GPU
    // faults a second (GROUP_ERROR_FATAL, translation faults; the MediaTek
    // driver recreates the queue), shown as black frames and swapchain
    // rebuilds (29/10, LC-1..LC-15); without it GTA ran 32-40 fps when cool.
};

// Same path/priority as a per-game config file.
static bool ApplyOne(const Quirk& quirk) {
  if (!cvar::ConfigVars) {
    return false;
  }
  auto it = cvar::ConfigVars->find(quirk.cvar);
  if (it == cvar::ConfigVars->end()) {
    XELOGW("Game quirk for {:08X} references unknown cvar '{}'", quirk.title_id,
           quirk.cvar);
    return false;
  }
  auto* config_var = static_cast<cvar::IConfigVar*>(it->second);
  std::visit(
      [config_var](auto&& value) {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, const char*>) {
          toml::value<std::string> node{std::string(value)};
          config_var->LoadGameConfigValue(&node);
        } else {
          toml::value<V> node{value};
          config_var->LoadGameConfigValue(&node);
        }
      },
      quirk.value);
  return true;
}

size_t Apply(uint32_t title_id) {
  size_t applied = 0;
  for (const auto& quirk : kQuirks) {
    if (quirk.title_id != title_id) {
      continue;
    }
    if (ApplyOne(quirk)) {
      XELOGI("Game quirk for {:08X}: {} ({})", title_id, quirk.cvar,
             quirk.note);
      ++applied;
    }
  }
  return applied;
}

}  // namespace game_quirks
}  // namespace xe
