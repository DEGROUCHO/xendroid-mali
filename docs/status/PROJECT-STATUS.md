# Project status (updated 2026-10-07 evening)

Single reference document. The older notes are the history; to know where we are, read this one.

## Goal
Xbox 360 emulator (Android port of Xenia) on the Poco (Mali-G720, package `xendroid.compose`):
- more fps;
- less heat and fewer cores;
- less RAM (0.5–1 GB);
- no glitches.

Only general changes to the core, never fixes for a single title.

## Where the weight really goes (measured, Black Ops, q273 profile)
| Item | CPU share |
|---|---|
| 6 guest threads (game code translated by the JIT) | ~80% |
| of which active game waits (job queue scanning, polls with timeout 0, empty yields) | ~1/3 of the total ≈ 1.4 cores |
| GPU Commands thread | ~11% |
| Audio, encode, driver | ~9% |

**Conclusion:** the bottleneck for heat and power use is the CPU, in particular the active waits. RAM and GPU come after.

## Builds
| Build | What | Status |
|---|---|---|
| **q232** | daily build on the phone (`W/best-apk.txt` absent → `pkg_run` restores q232) | stable |
| q280 | Halo candidate fixes + spin park off | Halo still white |
| q281–q286 | segmented shared memory (16×32 MiB) + eviction | **closed, negative** (see below); code off by default |
| q287 | `Kernel.guest_spin_wfe`: WFE wait on the wake counter during empty yields | superseded (RUN31 interrupted: phone unplugged) |
| q288 | q287 + the same hook in the `NtWaitForSingleObjectEx` polls with timeout 0 + counter increment on every object signal | compiled |
| q289 | q288 + counter increment on every GPU write to guest memory (rptr, scratch/fence, MEM_WRITE, EVENT_WRITE, COND_WRITE) | compiled |
| **q290** | q289 + **Halo fix** `GPU.rt_cache_full_overwrite_await_pipeline=true` (default): a draw that caused ownership transfers to be skipped must really execute, so it waits for its pipeline instead of placeholder/skip | superseded by q291 |
| q291 | q290 + WFE fixes from code review (details below, track 1) | superseded by q292 |
| **q292** | q291 + working set log with distinct 2 MiB and 32 MiB blocks for 120 frames (`GPU.shared_memory_log_working_set=true`) | **to be tested: RUN33** |

## Tracks
### 1. CPU / heat (priority)
- **Spin park with fixed sleep** (q277–q280): fewer cores but −18% fps due to wake-up latency. Rejected.
- **New approach, `guest_spin_wfe`.** A global counter `xe::threading::guest_wake_generation` is incremented by:
  - every successful `stwcx.` (JIT);
  - every thread made ready (`LinkReadyLocked`);
  - every object signal (`WakeCooperativeWaiters`).

  A guest thread that yields with no other threads ready:
  - if the counter has changed, it returns immediately;
  - otherwise it waits with `LDAXR` + `WFE`. It wakes as soon as someone writes the counter, or at the latest after about 200 µs (event stream).

  Sources verified: FEX uses LDXR+WFE in its spins, Arm documents wake-up on a write to the monitor.
- **Test:** RUN33 (arms 3-4) compares Black Ops on q292 with WFE off and on. PASS = fewer cores at fps within about 5%.
- **Code review done, fixes applied in q291:**
  - its own requeue does not count as progress (without this the WFE never triggered);
  - the thread's own `stwcx.` are excluded, with a counter per guest thread in the `PPCContext`;
  - `yield(false)` in the wait with timeout 0;
  - increment also in software store-conditionals and in deferred Vulkan fences;
  - increments active only with the cvar on. Residual risk: a producer that writes guest memory with normal stores without a bump adds up to ~200 µs per wait round.

### 2. Halo 3 white in gameplay
- **Fixed:** menu and lobby. **White:** the gameplay scene (the HUD is visible).
- With 5 GPU features off (q265) the gameplay was correct.
- **Bisection:**
  - `rt_cache_draw_clear_chains=false` alone: still white.
  - Trio off in gameplay: under test (RUN31 arm 1).
  - `overwrite_cutouts` alone: to do.
- **Hypothesis:** the HDR exposure (luminance RT chain 80x40 → 80x1 with feedback from the previous frame) reads untransferred pixels.
- **Cause identified in review (to be confirmed on the phone).** With a "fully overwritten" draw, the RT update changes ownership **without** transferring the old data. If the draw then does not really execute (async pipeline not ready, placeholder or skip), garbage pixels remain. Halo's HDR exposure, which depends on the previous frame, never recovers: permanent white.
  - Fix in q290: `GPU.rt_cache_full_overwrite_await_pipeline`. The log `RTCLEAR: full-overwrite draw awaits` confirms the case occurs.
- Discarded in review: half-pixel (in Halo `half_pixel_offset=true`); scissor and viewport are applied.

### 3. RAM (closed for now)
- **Segmented shared memory at 32 MiB:**
  - Black Ops in gameplay still holds 14 of 16 segments;
  - with eviction: −fps (the 16-case `switch` in the shaders), more cores, glitches.
  - The code stays off by default (`Vulkan.vulkan_shared_memory_segmented=false`).
- **Bugs found along the way:**
  - black screen, caused by two flags in the command processor (fixed);
  - Mali compiler crash with nested `switch`/`if`/`phi` (mask branches removed).
- **Real composition (BO q276, `meminfo` at 180 s), PSS 2013 MB:**
  - **GPU memory (GL mtrack) 1384 MB**: dense shared memory 512, texture heap ~312, render targets ~164, driver ~260;
  - guest RAM on the CPU side (ashmem) only **118 MB** resident: untouched pages cost nothing;
  - native heap 90 MB plus 312 MB in swap.
- **Why the AHB zero-copy (q261) saved nothing:** it removes the 512 MB GPU copy but makes all 512 MB of guest RAM resident (AHB allocated in full), which today weighs only 118. Balance ≈ 0. **Do not retry it.**
- **The real levers are in GPU memory:**
  - shared memory: fine granularity is needed on the pages really used by the GPU (MW2 ~55 MB hot, ~294 MB over the whole session), but without sparse binding on Mali the shader cost is high;
  - textures and RTs: to be measured per scene.
- **Next RAM step:** shared memory with 2 MiB pages and a BDA page table (pointers in the shaders, no `switch`), plus eviction.
  - Sources confirm that on Mali there are no lazy allocations for buffers and that AHB/dma-buf are allocated in full: BDA is the only route.
  - Only done if the RUN33 working set log shows few 2 MiB blocks per window.
  - Plan ready: `evidence/2026-10-07-sera/PIANO-BDA-shared-memory.md` (6 steps, estimate 9–14 person-days).
  - The reports are all in `evidence/2026-10-07-sera/`.

## Test queues armed
- `W/RESTORE-after-disconnect.sh`: when the phone is reconnected and the emulator is not running, it restores the original config + q232. Needed because the phone was unplugged during a run (it had q280 + test config).
- `W/RUN33-h3fix-wfe.sh`: starts after the restore. Each arm waits for Home + ≤41 °C + emulator closed, with no timeout.
  1. Halo q292 default (fix);
  2. Halo q292 trio off;
  3. BO q292 WFE off;
  4. BO q292 WFE on.

  All with the working set log on.

## Test rules (fixed)
- Battery ≤ 41 °C and launcher in the foreground. Thermal stop at 46.5 °C, never relaxed.
- One fixed announced queue, never modified while running. Each arm has a hypothesis and a PASS criterion.
- No live A/B. Tests up to gameplay.
- `game_inputs.next.json` (A-button spam every 5 s for Halo 3, BO, MW2, GTA IV) becomes `game_inputs.json` at the next queue.
- After each run, q232 and the global config are restored (md5 `3ef8a43878305e12637232e7e3fe27d5`).
- Never touch the phone during runs: a focus change stops the test.

## Tools
- **Build:** `tools/build/build.sh qNNN`, then copy to `W/qNNN-test.apk`.
- **Test:** `W/pkg_run.py`; scripts `W/RUN*.sh`.
- W = `/path/to/xendroid-ultimate-mali/lab/build/multigame-core-audit`

## 2026-10-07 night — challenge: −500 MB RAM, −15% CPU and GPU
**Rule:** do not re-attach the 512 MB shared memory, neither segmented nor BDA. The half-finished BDA branch is parked in `evidence/2026-10-07-notte/bda-parked/`: 16 files that do not compile, sources reverted to q293.

**RUN36: real memory map (BO q293, 240 s, PSS 2135 MB)**
- **Vulkan:** shared 512, texture 280, render targets 164, other ~140.
- **Native heap, from smaps:** scudo primary 82 MB resident + 135 swap; scudo secondary 47 + 145.
- **heapprofd:** only 220 MB really alive out of 518 allocated. So about 190 MB is **freed but retained** memory from scudo, which ends up in swap and counts toward PSS.
- The 128 MB piece in `Setup` is virtual.

**q294 = q293 + two global levers**
- `GPU.native_heap_purge_interval_swaps=300`: `mallopt(M_PURGE_ALL)` every 300 frames, with the log `HeapPurge`.
- `GPU.texture_skip_top_mip_min_size=512`: compressed (DXT) 2D or cube textures with guest mips and side ≥512 are saved from mip 1. They use ¼ of the memory and ¼ of the bandwidth, and mip 0 is never loaded. Resolves, 3D and generated mips remain excluded.
  - Approximation: fetches with explicit LOD and `mip_min_level` > 0 read a blurrier level.
- Apply script: `evidence/2026-10-07-notte/apply_q294.py`.

**RUN37** (`W/RUN37-q294-ram.sh`): BO q294, GTA q294, GTA q293 (baseline), with `log_vk_memory` + `log_render_pass_breaks=600` for the next GPU cut.
- **q256 BO history:** about 46 render pass breaks per frame, 22 of them from barriers at a single point.

**Already tried, do not redo:**
- `Pulse` throttle (q266-268);
- WFE (q291-293): BO did not reduce the cores;
- spin park (q277-280);
- RT eviction: probably all 28 BO RTs are used every frame, so it was not implemented.

**RUN37, arm 1: BO q294** (240 s, same diagnostics as RUN36)
- fps 20,8, cores 4,56, bandwidth 11.1 GB/s, PSS 2031 MB (q293 RUN36: 2135).
- **Textures: 280 → 168 MB (−112 MB). The mip 0 skip works.**
- **Heap purge: useless.** It costs 3-4 ms every 300 frames and native swap rises to 329 MB. The native heap is **live** memory, not freed: `mallinfo` goes from 510 to 729 MB during the match.
  - The "220 MB alive" of RUN36 was wrong: in the heapprofd trace the *disconnected* flag is 1, so the profiler detached after startup.
  - Next step: heapprofd with 64 MB shmem and 512 KB sampling, to find out who allocates 729 MB.
- **Render passes broken in BO q294** (per frame, q294 symbols):
  - pending barriers from `VulkanSharedMemory::Use`: 20.3. They are uploads of pages already read in the current pass (UPBRK log: callers 1C82600 = IssueDrawImpl, 291 cases; 194BE4C, 117 cases; all `inval=true`);
  - other sources: `DeferredCommandBuffer::WriteCommand` 16,9, `LoadTextureData` 3,9, RT `SetUsage` about 8.
  - Uploads at the head of the pass are already active (8960 moved). The "verified" case is missing: `vulkan_hoist_verified_shmem_uploads` exists (default false) and **has never been tested**. Candidate for the GPU cut.

**RUN37, GTA IV:** q294 vs q293.
- PSS 1998 vs 1911 (noise), GL mtrack 1306 vs 1329, fps 9.3 vs 10.2.
- In GTA the mip skip gains little: there are few large compressed textures.

**Code reading: what holds the native heap** (from reading the code)
1. `Shader::ucode_disassembly_`: disassembly text saved for every shader, about 20-100 KB each. Only the dump reads it. **Fixed in q295.**
2. `Translation` objects per modification, never destroyed: SPIR-V zstd, 2-10 KB each.
3. JIT `source_map_`: 12 bytes per instruction, 10-30 MB.
4. `guest_call_sites_`: 15-25 MB.

**Code reading: "verified" hoist.** Safe with `shared_memory_unwatch_dynamic_pages=false` (our config). The hole exists only with unwatch on. The global config forces it to false, so it must be turned on in the config.

**q295** = q294 + disassembly not retained + purge default 0 + `vulkan_hoist_verified_shmem_uploads` default true (`evidence/2026-10-07-notte/apply_q295.py`). Test: **RUN38** (BO + GTA).

**RUN38, BO q295:** fps 24,0, cores 4,76, bandwidth 16.1 GB/s (different scene: 2447 draws/frame), PSS 1934 (harness) / 1981 (meminfo).
- Native heap from 729 to 714 MB: the disassembly was worth little.
- **The "verified" hoist does not trigger:** barriers from `VulkanSharedMemory::Use` stay at 20 per frame.
  - Cause: uploads are blocks of 48-64 pages, because a CPU write invalidates 256 KB (`shm_fault_invalidation_block_pages=64`).
  - Pages read in full require the upload hash, which today is computed only for unwatched pages (`shm_hash_only_unwatched_pages=true`).
- Goals board: `GOALS.md`, updated after every run.

**Chained queues**
- **RUN39:** BO q295 + `shm_hash_only_unwatched_pages=false` + `shm_fault_invalidation_block_pages=16`.
- **RUN40:** BO q295 with simpleperf at 120 s for 15 s (fresh CPU profile).
- **RUN41:** BO q295 with corrected heapprofd (`pkg_run_heap.py`: shmem 64 MB, sampling 256 KB, timeout 5 s).

**Keep-awake:** `scratchpad/keepawake.sh` sends SHIFT every 45 s when the launcher is present.

**Driver: report**
- Command pools: two per each in-flight submission, reset without `RELEASE_RESOURCES`, never destroyed.
- Transient descriptor pools: pages of 32768 sets, freed only with the cache clear.
- Upload pools: 2 MB pages, plus the 12 MB index pool, with 8 MB of retained headroom per pool.
- **Pipeline eviction was already tried in q254 and discarded** (−60 MB, stalls up to 314 ms): do not redo it.

**q296** = q295 + descriptor pool of 8192 sets per page + `Vulkan.vulkan_command_pool_spares=2` (destroys excess idle command pools; `CmdPools` log with `log_vk_memory`). Script: `evidence/2026-10-07-notte/apply_q296.py`.

**Note on CPU (important):** a core in WFE stays "busy" for the operating system, because user time keeps counting.
- WFE reduces consumption but **not** the harness's "cores": this is why the WFE test in RUN35 showed no drop in cores.
- To lower the measured cores you have to sleep in the kernel, with the latency already seen (−18% fps with spin park).
- For the CPU, therefore, the ms-core per frame metric matters, and real work must be removed: the fresh profile comes from RUN40.
- `WakeForSignal` is already optimized (it wakes only the parked CPUs).

**RUN39, BO q295 + hash of all pages + 16-page block:** thermal stop at 180 s, fps 21,4, cores 4,61, PSS 1981.
- Barriers from `Use`: 18.9 per frame vs 20. **The "verified" hoist does not work:** the reread pages were really rewritten in the same pass.
- **Major GPU offender:** about 20 pass breaks per frame. Each one unloads and reloads the tiles (about 30 MB on a 720p 2xMSAA target), so together they account for almost all of the frame's bandwidth (~550 MB).
- **Remedy in design:** a versioned overflow ring. The CPU writes the rewritten pages into a new area and only the following draws are remapped there (indices with bind offset, vertices with the fetch constant address in the UBO, textures with the load constant), so the pass stays open. The code points are being mapped.

**RUN38, GTA q295:** PSS 1630 (harness) / 1670 (meminfo), GL mtrack 1152 (vs 1306-1329). The scene, however, is different (1266 draws/frame vs 5000-7000).

**Decision: fewer tests.** RUN40 and RUN41 stopped in WAIT, before any install (config verified 3ef8…27d5). In their place **a single arm, RUN41b** (`W/RUN41b-q296-diag.sh`): BO q296 + heapprofd + simpleperf + `log_vk_memory` + the config trials `texture_skip_top_mip_min_size=256` and `vulkan_upload_pool_retain_mb=2`.
- **Rule from now on:** tests only for candidate builds, with the diagnostics inside the same arm.

**RUN41b, BO q296 diag** (thermal stop at 180 s). PSS at 180 s per build (BO, meminfo): q293 2096, q294 1958, q295 1916, q296 1936.
- **RAM config that works:** textures 168 → 136 MB with the mip skip from 256; upload pool 88 → 72 MB with the 2 MB margin.
- **Driver pools in q296: they get worse.** GL rises from ~1226 to 1268 MB, and the command pool cap recreated 25.500 pools in 3 minutes. Removed in q298.
- **heapprofd always detaches** (disconnected=1) because the guest threads run on fiber stacks: **do not use it anymore**. In its place, the heap census inside the app (`GPU.log_native_heap_census_at_frame`): `malloc_iterate` over the scudo maps, histogram by size, vtables of libe and Mali, largest blocks.
- **CPU profile q296** (BO, 15 s), in CPU ms per second:
  - guest 6523, of which 72% JIT; job wait functions ~15% (about 0.95 cores) and kernel shims (yield, wait, mutex, nanosleep) ~15%;
  - CP 1022: `RequestRange` 10.5%, memcpy 6.9%, `UpdateBindings` 6.3%, `XXH3` 5.6% (the "verified" hoist, useless).

**q297/q298: in-pass vertex ring** (`evidence/2026-10-07-notte/apply_q297.py` + `apply_q297b.py`)
- The shared memory buffer becomes 512 + 16 MB.
- When the upload of a vertex buffer would break the pass (same rules as the hoist), the CPU copies the current bytes into the ring and the fetch constant of that draw is patched (dword_0 = offset in the ring | type). The `RequestRange` of that draw is skipped.
- The descriptor covers 512 + 16 MB (a single binding: `maxStorageBufferRange` is 2 GB on Mali).
- The ring is freed when the submission completes; if it is full, it falls back to the normal path. Log: `[VFRING]`.
- q298 = q297 + RAM levers as defaults (mip from 256, pool margin 2 MB) + "verified" hoist off + heap census + no changes to the driver pools.
- **RUN42:** BO + GTA on q298, single test.

**RUN42, BO q298:** fps 21,0, cores 4,34, no crash.
- **Ring v1 ineffective:** barriers from `Use` stay at 20.4 per frame and there is no `[VFRING]` line. BO's dynamic vertex buffer is about 4.4 MB (`0x15A60000`, 1104 pages) and only one 192 KB block (`0x15E80000`, 48 pages) is rewritten; the fetch constant covers the whole buffer, so the copy exceeded the limit.
- **Heap census** (at frame 2400): 562 MB alive in 280.821 allocations.
  - 1 block of 128 MB: `eh_frame`, virtual.
  - **5 idle JIT translators** of ~25 MB each (~120 MB): 8 MB HIR arenas with pointers, 8 MB source maps, 4 MB A64 code buffers (`kMaxCodeSize`), 4.5 MB blocks.
  - SPIR-V zstd: 1269 translations, 31 MB.
  - The census brings swapped pages back into RAM and inflates PSS (2144 MB at 180 s): **use it only for diagnosis, never in measurement runs.**
- **Web research (Reddit/GitHub):** nothing applicable right away. PanVK for G720 is experimental (MT6899 MC8, ours is MC7); xenia-android and xeo are archived; no fps numbers for the 360 on Mali; the AnTuTu trick is only reported by the community.

**q299: windowed ring v2** (`apply_q299.py`)
- Copy only the span of the rewritten pages.
- The system constants (ex padding offset 20-31) carry `vfetch_window_start`, `vfetch_window_size` and `vfetch_window_delta` as dwords.
- In the vertex fetch: `address - start < size` → it reads at `address + delta`. The bounds check stays on the original address.
- Covered ranges skip the upload but stay marked as used (`MarkRangeUsedInSubmission`), so no hoist bypasses the draw.
- One window per draw; interpreter, memexport and multiple bindings are excluded.

**q300** = q299 + `CPU.jit_translators_kept=1` (`apply_q300.py`): `TypePool::Release` destroys the idle translators beyond the first, estimated at about −100 MB of native heap.
- **Test:** RUN43b, BO + GTA on q300. RUN42 (GTA arm) and RUN43 stopped in WAIT, config verified.
- **Keep-awake:** stopped manually.

**RUN43c, BO q300** (thermal stop at 230 s)
- **Ring v2 works but covers little:** 2048 draws kept the pass (282 MB copied, ring full once), but barriers from `Use` stay at 22.5 per frame.
- **The cap on JIT translators is harmful:** resident native heap 437 MB, PSS at 120 s 2218 MB, fps 18.6 with p10 10.8 (stutters), 5.05 cores. Scudo does not return freed blocks and recreating 25 MB translators costs time. **Back to 0 (keep all) in q301.**
- The Dante arm of RUN43c was stopped in WAIT.
- **Note:** the `UPBRK` lines also count uploads outside the pass; the reliable metric is `RPBARRIER`.

**Finding about the daily config** (q232 + original config): it has **off** the optimizations that all tests use (DIAG):
- `vulkan_dxt1/dxt5_to_astc`, `vulkan_dxn_to_eac` (−280 MB GPU in q181);
- `a64_vconst_pool`, `a64_fold_guest_address`, `a64_compact_mmio_check`, `texture_fetch_constant_memo`, `vulkan_shader_load_cache`, texture heap in 16 MB blocks.

The ASTC/EAC transcodes have `UPDATE_from` at 2026-10-06 and the config has `defaults_date` 2026080113, so with new builds they turn back on by themselves; the JIT flags do not.
- **So the real "before" is q232 + daily config.** RUN44 measures it (arms 3 and 4).

**q301** = q300 + `jit_translators_kept=0` + `[VFWIN]` diagnostics (window discard reasons) and `[USEBAR]` (`Use()` transitions inside a pass).
- **RUN44:** BO q301 150 s, Dante q301 300 s, BO and Dante q232-daily (baseline).
- Dante inputs: A every 5 s + START at 20/35/50/70/90 s.
- **Decision:** tests only on BO + Dante; BO shorter (150 s).

**Analysis before RUN45** (understand before launching). In BO q300 the passes close about 51 times per frame (RPBREAK with q300 symbols):
- `TryDirectHostResolveCopy` → `CmdVkDispatch` 18.2;
- `SubmitBarriersAndEnterRenderTargetCacheRenderPass` (RT change) about 19;
- `LoadTextureDataFromResidentMemoryImpl` 5.5;
- `DumpRenderTargets`/`Resolve` 3.8.

The 22.5 barriers "from `Use`" are **the resolves**, not vertex uploads: the ring aimed at the wrong target, and vertices rewritten inside a pass are ~0.3 per frame. **q302 = q301 with the ring off by default.**

The in-pass resolve exists (`vulkan_in_pass_resolve`, off): it excludes GAMMA (suspected of the black rectangles in MW2) and multisample sources ("they come out dark, cause unknown"), and counts the discard reasons.

**RUN45:** BO and Dante on q302 with the in-pass resolve on, then BO and Dante on q232 with the daily config (the real baseline).

**STOP 2026-10-08 night.** The Dante arm of RUN45 was in progress (it restores itself); the RUN45 queue is stopped and base arms 3-4 were not run.
