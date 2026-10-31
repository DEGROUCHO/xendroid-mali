# XenDroid multi-game plan — big blocks first

Source: earlier notes.

Rules:
- **General** patches **in the core**, no per-title profiles.
- One build, one package test (3 games × 180 s).
- Everything that is in the packages is a cvar, so bisection is done via config.

Single harness: `W/work/multigame-core-audit/pkg_run.py TAG APK "giochi" [k=v,...] [sec]` (W = /path/to/xendroid-ultimate-mali/lab).
- Measures per game: FPS (timeline, median, p10), CPU per thread, Mali counters (`xd_gpuctr`), PSS and screenshots.
- `XD_TOUCH_AT`/`XD_TOUCH_PATH`: one-shot trace trigger.
- `finally`: restores the original global and q232.

## Findings 2026-10-05 (in order of weight)
1. **Bare global = root cause of the low FPS** on the new games. With the MW2/Halo package, BO goes from 5–10 to 30–46 FPS on a cold phone.
2. **Compilation trap**: `async_skip_draws=false` + lazy pipelines cause drops to 0.5–3 FPS on the first launch of each game. Fix: asynchronous skip + precreate.
3. **Thermal wall**: the `mali` cooling device goes to 19 (GPU frequency cap, root is needed to remove it) after ~90 s of BO, and the GPU drops from 1.1 to 0.42 G cycles/s. Sustained FPS = GPU work per frame.
4. **GPU anatomy of BO**:
   - ~31 M cycles and ~40 full-screen render passes per frame;
   - 480 MB of bandwidth per frame;
   - 68 M fragment threads;
   - late quads discarded at 99.6%.
   Signature of the bit-by-bit stencil transfers: Mali has no stencil export, so 8 passes per copy.
5. **Audio**: gaps (43 ms queue too short when the game slows down) and **clipping** of the 5.1 downmix, which gives the "blown speakers" effect.

## Build under test: q236 (sha256 19f22643…)
- **MW2 package as universal defaults**, migrated via `UPDATE_from_*` 2026-10-05 12h:
  - NaN fixup off;
  - JIT: GPR13, loop unrolling, VMX direct (NaN-safe), vconst pool;
  - fake occlusion;
  - asynchronous submit/swap and present from the UI thread; `cp_thread_nice` -10;
  - MW2 RT/descriptor flags; texture cache 96/256/3; early-Z; preswap; unwatch; mid-frame 0; swapchain 720; 60 fps; spin 0.
- **Excluded, with reason**:
  - ASTC/EAC: corrupted blocks on BO, default off until fixed;
  - in-pass resolve: effectively off in MW2 (reject=3);
  - readback none: Halo uses uma;
  - aniso 0 and FXAA/FSR: visual choices;
  - `cl`: title-specific.
- **Stencil bit-mask transfer** (new, cvar `rt_cache_stencil_bit_transfer_mask`): each depth RT tracks the stencil bits "possibly set to 1" (draw, clear, transfer) and transfers only draw those bits. Exact, no loss. Log: `StencilBitTransfers`.
- **Audio**: queue 8→16 blocks; peak limiter (instant attack, ~100 ms release) + soft-clip instead of a hard cut.

## Open issues
- **Light squares in regular files** (2×8) on clothing and walls in BO: present even with ASTC off, absent in q232. Likely sprites/particles with collapsed coordinates. Suspects: index preswap, repeated draw fast paths, unwatch of dynamic pages. Bisect via config if they persist in q236.
- **Black polygonal shadows in Mass Effect**: to be re-checked with q236.
- **ASTC/EAC transcoder**: block noise (RAM −280 MB pending).
- **Next GPU block**: from the RT trace, reduce the number of transfers and EDRAM render passes (bandwidth and heat).
- Coverage: Dante, Halo 3, GTA IV, Skate 3, MW2 (regression).

## Update 2026-10-05 evening
- Results q237:
  - BO 30 fps median in gameplay;
  - audio gaps from 0.44% down to 0.13%;
  - ME stuck on the menu;
  - Reach to be redone at 420 s (at 180 s it was still loading).
- GPU block: EDRAM transfers amount to about 35 M fragments per frame on BO, half of the work. The MW2 clear detector discards rectangles that do not cover the full pitch. "draw overwrite cutouts" are in progress (q238, in the source, NOT compiled).
- RAM block: PSS 2.3-2.7 GB.
  - VkMemory breakdown: shared memory 512 MB, RT 312 MB, texture 120 MB.
  - Plus about 200 MB of driver and 0.6-1.2 GB of native heap.
