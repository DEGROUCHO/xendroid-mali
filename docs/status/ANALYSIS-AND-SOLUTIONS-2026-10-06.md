# Data analysis and solutions — 2026-10-06 (autonomous work, phone unplugged)

This document collects what all the data says (MW2, Halo, Black Ops, Reach, Mass Effect), what I changed in the core, and what to launch when the phone is plugged back in.

## 1. What really limits the FPS (from the data)

### 1.1 The GPU, then heat

- **MW2.** When cold, the GPU takes about 14.8 ms per frame:
  - main passes 8.5-9 ms;
  - resolve 1.9 ms;
  - texture loading 1.2-1.5 ms;
  - post-processing 1.3 ms;
  - bloom 0.7 ms;
  - transfer 0.5 ms.

  After 2-3 minutes the phone goes from 40.8 to 44.7 °C. The Mali cooling device (mali=19) then limits the GPU to about 0.42 G cycles/s, versus 1.1 when cold: compute throughput drops by about 2.6x and the FPS collapse (MW2 from 57 to 27, Reach from 30 to 14).
- **Black Ops.** The EDRAM transfers were worth about **35 M fragments per frame**, roughly half of the fragment work (Mali counters: 63-75 M fragment threads per frame).
- **Reach (menu).** About **94 M fragments per frame** of transfers, of which 77 M are the 1x↔4x MSAA round trip around the Xbox SDK clears.
- **Conclusion.** Sustained FPS depend on the energy spent per frame. Reducing GPU work per frame is the only lever that helps both FPS and heat. EDRAM transfers are the most "useless" part of the work, and so the most worthwhile to eliminate.

### 1.2 Why transfers are expensive on Mali

- The Mali-G720 has no `VK_EXT_shader_stencil_export`, `maintenance8` or fragment shader interlock (verified with `cmd gpu vkjson`).
- Each copy to a depth buffer therefore costs **9 passes**: 1 for depth plus 8, one per stencil bit.
- The Xbox 360 does clears with rectangles drawn through a 4x MSAA view of the same EDRAM, often in strips (first a 32 px strip, then the rest). The emulator must therefore copy 1x→4x, draw the rectangle and copy back 4x→1x: 18 passes for a single clear.

### 1.3 RAM: 2.3-2.7 GB against a target of 0.5-1 GB

Measured with `log_vk_memory` on Reach:

| Item | Size | Note |
|---|---|---|
| Shared memory | 512 MB | Mali has no sparse binding; zero-copy fails because host memory import is missing. |
| Render target images | 312 MB (24) | |
| Textures | 120 MB | |
| Upload pool | about 30 MB | |
| Driver allocations | about 200 MB | |
| Native heap | 0.6-1.2 GB | |

- The big lever remains the AHB path for shared memory (512 MB), but it is risky and must be tested on the phone.

### 1.4 Audio

- Since q237 the gaps drop from 0.44% to 0.13% and clipping is handled by the limiter. Mass Effect has 0.01%. Latency is about 85 ms (queue of 16).

## 2. What I did in the core (all general, no per-game profiles)

Every change has a cvar to turn it off. The builds are in `W/work/multigame-core-audit/qNNN-test.apk`.

| Build | What it adds | Expected effect, with its source |
|---|---|---|
| q238 | **Draw overwrite cutouts** (`rt_cache_draw_overwrite_cutouts`): copies skip the rectangle that the clear overwrites. Active only if the draw actually runs. | Measured: it triggers, but strip-style clears limit its effect. |
| q239 | **Retarget of depth-only clears** (`rt_cache_retarget_depth_only_clears`): a 4x depth-only clear becomes a clear of the 1x surface, which keeps the stencil. | Measured on BO: shadow map round trip from 15.4 to 5.1 M per frame. |
| q240 | **Clear chains** (`rt_cache_draw_clear_chains`): copies are deferred while clears continue, then done only outside the cleared area. | To be measured. |
| q241 | Retarget also of **stencil-only** clears, and of clears that also write color (the color part stays a draw). | Reach: 77 M per frame of round trip in the menu; to be measured. |
| q242 | Chains **per slot**, which also absorb the retargeted clears. Fixes the BO case "strip + retargeted clear" that was worth about 11 M per frame. | BO estimate: from about 24 to about 13 M per frame of transfers. |
| q243 | **Texture sign specialization** (`spirv_texture_sign_specialization`), from the Xendroid-Plus fork. | Source: −7.9% GPU measured on Adreno, same image. |
| q244 | Sign slots chosen by use (`_used`) and **exp adjust** specialization to zero. | Source: −0.4 and −0.2 ms on Forza. |
| q245 | JIT: **direct branches** (`a64_near_branches`), with re-emission if a function is too large. | Source: code 4 to 9% smaller, 3 to 7% fewer cycles. |
| q246 | JIT: local variable layout done **only once**. Fixes a latent bug: re-emitting (vconst pool, near branches) corrupted local variables, as in the Crysis 3 crash in the other fork. | Correctness. |
| q247 | **Fast 21-bit rounding** (`spirv_fast_precision_rounding`): 5 instructions instead of 12. | **Verified by me on all 2³² inputs: 0 differences.** Source: −6% instructions in pixel shaders. |
| q249 | **Chains per aspect**: depth and stencil each have their own covered area. A depth-only clear plus a stencil-only clear (separate, as Reach does every frame) completely eliminate the initial 9-pass copy. The transfer function uses different cutouts for the depth pass and for the stencil passes. | Reach menu: about 7.8 M per frame for each of these cases; to be measured. |
| q250 | **Render pass area shrunk to what is drawn** (`vulkan_render_area_shrink`, new, on by default). RT images are as tall as the whole EDRAM: 2528 rows versus the 608 BO uses. On Mali the pass area decides which tiles are loaded and written. Also fixed a gap: "emplace" clears were not counted in the area, and now a pass with one of those clears keeps the full area. | Main hypothesis for bandwidth: BO moves 480-690 MB per frame with about 40 passes. To be measured (A/B in the script). |
| q248 | **lwarx/stwcx. without ABA** (granule generation checked inside the exclusive window). | Source: NFS Most Wanted from about 40% crashes to 0 out of 14. The same pattern (lock-free stack) is used by many games. |

Safety fixes:
- `Resolve()` now runs the pending transfers first. Otherwise, after a discarded draw, the resolve could read stale data; the other fork documented the same bug.
- The test script now supports a duration per game, the trace time per game and **key presses per game** (`game_inputs.json`): Reach presses A every 10 s to reach the campaign.

Touch controls (in the same APK):
- the finger takes the nearest control;
- LT/RT/LB/RB are touched across the whole bar;
- you can slide between A/B/X/Y;
- the left stick is "floating".

Everything is adjusted in the controls editor.

## 3. What to launch when the phone is plugged back in (one command)

```bash
bash /path/to/xendroid-ultimate-mali/lab/build/multigame-core-audit/RUN-WHEN-PHONE-BACK.sh
```

The script (now a 6-arm A/B):
- turns on keepawake;
- waits for Home and the battery at 41 °C or less;
- tests the latest build (q250):
  1. BO with everything on, 100 s trace, pass-break log;
  2. BO with all the new features off;
  3. BO without the pass-area shrink;
  4. BO without the clear chains (to see whether they add pass breaks);
  5. Reach on, 420 s, key presses to enter the campaign;
  6. Reach with everything off (A/B, also for the **pink menu**).
- at the end restores q232 and the original global setting.

What to look at in the results:
- `tools/analysis/perframe.py <game dir>`: GPU work per frame independent of clock (fragment threads, bandwidth per frame), i.e. the right comparison even when the phone heats up;
- `tools/analysis/xfercost.py <session.log>`: the transfers of the traced frame, with how much is saved by cutouts and chains;
- ON vs OFF screenshots: they must be identical, apart from motion.

## 4. Next blocks, in order of value

1. **"Samples as pixels" for 4x depth-only draws** (`vulkan_depth_4x_as_1x` from the other fork). Gears of War 3 went from 25 to 30 fps and the transferred tiles from 36.7k to 6.3k per frame. It covers real draws, not just clears.
2. **Retarget of color clears** via the constant color of the pixel shader, evaluated with the CPU interpreter. Eliminates the 2x↔4x color round trip of BO and Reach.
3. **Stencil written with 1 pass plus a buffer copy** for 1x depth: Mali allows `TRANSFER_DST` on D24S8 and D32S8. Each transfer to a 1x depth would go from 9 passes to 1 plus 2 small copies.
4. **RAM:** the AHB path for the 512 MB of shared memory, and RT images sized to the height actually used.
5. **JIT** (from the other fork): exact and cheap NaNs (`a64_fpu_nan_fixup_result_check`), fast and exact lfs/stfs. On their hottest thread they were worth −38% instructions.
6. **Textures:** coalesced untile and direct loading into the image (+3.7% and +4.5% FPS on Forza). The tools are needed to regenerate the SPIR-V shaders.

## 5. External source used

[Xendroid-Plus by phforner0](https://github.com/phforner0/Xendroid-Fork-main), PR #2-#16: a XenDroid fork optimized for Forza on Adreno 825, with documented A/B measurements. I only brought over **exact** changes or ones with identical results, adapted to our tree and verified by compiling. I also checked their bug list: several were already fixed by us (atomic flags for translations, blocking timers).

## 6. Evening 2026-10-06, phone connected: measurements and new levers

### 6.1 What was measured
- **EDRAM copies (BO, RT trace):** about 40 M raw fragments per frame, about 11-13 M effective after cutouts and chains (q250 and q252). The chains remove up to 98% of the shadow map copy. `xfercost.py` now also prints how much the chains save.
- **Render pass interruptions (q252, BO):** about 46 per frame, of which 15 immediately resume the same framebuffer, i.e. they write and reload the same tiles:
  - compute-based resolves: 6.8 per frame;
  - texture loads mid-pass: 3.5;
  - barrier changes: 1.9;
  - resolve dumps: 1.0;
  - swaps: 1.0.
- **Pixel shader paths (BO, per frame):**
  - 1617 draws without pixel shader (shadows and depth);
  - 652 with early-Z;
  - 318 with "late" testing (alpha test or kill).
- **CPU (BO):** about 4.2-4.7 cores in total.
  - The 6 guest threads (JIT-translated code) use about 3.5 of them; the GPU command processor only 0.24.
  - On MW2 (profile b217) the guest threads spend 86% in JIT code, so it is real work and not waiting.
- **RAM (BO, meminfo):** PSS about 2.2-2.6 GB, plus about 0.5 GB in swap.
  - GPU memory (GL mtrack) 1.65 GB: shared memory 512 MB, textures 312 MB (256 allocated plus fragmentation), render targets 367 MB, driver about 240 MB.
  - **Native heap from 0.73 to 1.08 GB, growing with the pipelines:** about 0.4-0.9 MB per pipeline created, 1174 pipelines at the end of the match.
  - BO pipeline cache: 171 MB on disk, rewritten 9 times in 3 minutes.
- **Non-comparable tests:** on BO the ON and OFF arms are not in the same scene at the same second. Loading is slower on the first start of each build because the pipelines are recompiled, and the videos run at 60 fps. From now on the GPU features are compared with the live A/B within the same match (`XD_LIVE_AB` in `pkg_run.py`, analysis with `tools/analysis/liveab_pkg.py`).

### 6.2 New builds

| Build | What it adds |
|---|---|
| q251 | `[RPREENTRY]` diagnostics: the interruptions followed by the same framebuffer. |
| q252 | **Fix:** the clear detector counted "color clear" also for rectangles with depth or stencil test active, such as a sky or a light volume. This skipped necessary copies: it is the main suspect for Reach's pink menu. Now the color only counts if the depth and stencil tests always pass. |
| q254 | `VkHostMemory`: driver CPU memory for pipelines, pipeline cache and shader modules, measured with a counting allocator. New cvar `vulkan_pipeline_evict_after_frames` (0 = off): destroys pipelines unused for N frames and recreates them synchronously from the VkPipelineCache on first reuse. No skipped draws, no placeholders. |
| q255 | **JIT, all exact:** vector constants of `vpkd3d128` and `lvlx128` loaded with one `ldr` from the pool instead of 4 instructions each; 32-bit splat from the pool when 2 `mov`s would be needed; logical immediates for 64-bit AND/OR/XOR (`and x, x, #-16` in one instruction, identical check to xbyak on 63 million values); guest addresses `[membase, w, uxtw]` and compact MMIO check on by default with the new cvars `a64_guest_address_uxtw` and `a64_mmio_check_compact`. |

### 6.3 Being tested (RUN2-q252.sh, then RUN3-q255.sh automatically)
- **RUN2 (q252):**
  - BO on and off;
  - BO with the resolve inside the pass;
  - BO with compute resolves that also write the texture;
  - Reach on and off, 420 s, for the pink menu.
- **RUN3 (q255):**
  - BO live A/B (cutouts, chains, retarget, area);
  - BO memory;
  - BO with eviction;
  - BO with texture loads at the start of the pass;
  - BO without the two JIT cvars;
  - BO with a 180 s simpleperf profile, to see the JIT hotspots on BO.

### 6.4 q256 results (live A/B, same match)
- **New GPU features** (cutouts, chains, retarget, pass area), measured on aligned frames: about **+10% fps**, bandwidth per frame about −9%, GPU cycles per frame about −4%. The fragment threads do not go down, so the estimates made on the traced frame were too optimistic.
  - A first calculation gave −20%. It was an analysis error: `fps_timeline` starts 5-10 s after launch. Now the phases are aligned on frame numbers (`LiveCvar: frame=N`).
- **RAM:**
  - render targets from 367 to 164 MB with `render_target_height_cap=768`, image correct;
  - driver CPU memory (VkHostMemory): pipelines 18 MB, pipeline cache 175 MB, shader modules 79 MB;
  - plus 79 MB of our own SPIR-V, which q258 frees.
- **Build:**
  - q257 = q256 + "compare with zero + branch" fusion in the JIT (`a64_fuse_zero_compare_branch`);
  - q258 = q257 + SPIR-V released after module creation (`vulkan_release_spirv_after_module`).
