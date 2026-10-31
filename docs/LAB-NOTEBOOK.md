# XenDroid — registry of what already exists

**Rule: before designing, writing or testing anything, search here and with `bash work/cerca.sh "word"`.**
If the idea is already in this registry: do not rewrite it. Read where it is documented, check whether it still works (it may have regressed) and restart from there.
Whoever adds or removes a mechanism or a tool, or closes a path, updates this file at the same time.

Created on 10 October 2026, after work that already existed was redone in a single evening: the measurement method of 3 October, the per-pass budget, the clear chains of 6 October, the frame tracer.

## 0. Historical documents with the same purpose (read them: they contain things not copied here)

- `research/KNOWLEDGE-SHEET.md` — knowledge sheet created on 2 October: where the time goes, things already tested that must NOT be repeated (JIT allocator with 13 registers, asynchronous encoder/swap/submit, ADPF hint = -2 FPS, userfaultfd not available, zero-copy AHB, PanVK slower than the stock driver), sources already covered, research of 2 and 3 October (Mali/Arm, discarded draws, thermal: "the only supported lever is less energy per frame or an external heatsink").
- `docs/status/ANALYSIS-AND-SOLUTIONS-2026-10-06.md` — table of builds q238-q250 (cutout, retarget, chains, shrink), RAM, audio.
- Measurement method, MW2 per-pass budget, CPU profile (3 October).

Warnings taken from there: `log_gpu_frame_time_breakdown` and `log_cp_frame_breakdown` add a clock read for every draw (37% of the CP thread in one profile): **never profile the CPU nor measure FPS with those logs enabled**; they are only useful for the GPU per-pass breakdown.

## 1. Before starting work (checklist)

1. `bash work/cerca.sh "<keywords>"`: does a cvar, a tool or a failed attempt already exist?
2. Read the matching entry below and the document it cites.
3. If it exists: measure whether it still works (section 5) before touching code.
4. Write down the question, the success criterion, what will be done if it succeeds and if it fails.
5. Only after that: code, then a single test.

## 2. Measurement and diagnostic tools that ALREADY EXIST

| Tool | How to enable | What it gives | Documented in |
|---|---|---|---|
| Mali hardware counters per second | automatic in the runner (`gpuctr.txt`, binary `/data/local/tmp/xd_gpuctr`) | GPU cycles, shaded quads, input and visible primitives, tiles, MB read/written | 3 October |
| GPU time breakdown per render pass class | `GPU.log_gpu_frame_time_breakdown=true` | lines `VkPassTime: [xfer ]LxA : ms/frame (pass, draw)` and `VkFrameSync` every second; separates buffer-to-buffer copies (`xfer`) from drawing | 3 October |
| Pass sequence of two frames | as above + `GPU.log_pass_sequence_every=N` | `VkPassSeq`: each pass in order with GPU ms and gaps | 3 October |
| Single-frame tracer (render target) | `Vulkan.vulkan_rt_trace_request="<file>"` + `XD_TOUCH_AT`/`XD_TOUCH_PATH` in the runner | `RTTRACE` lines (claim, xfer, rclear, draw_chain, run, resolve) and `RTCLEAR` (why a rectangle is not a clear) | 6-7 October |
| Transfer summary | `GPU.log_transfer_summary=N` | transfers grouped by source/destination/aspect | 10 Oct evening |
| Discarded draws | always in the log (`VkDroppedDraws`) | if > 0 the measured GPU work is FALSE (pipeline being created, `vulkan_async_skip_draws`) | 3 October |
| CPU profile | `XD_SIMPLEPERF_AT`, APK `-prof`, `agg.py`, `callers.py` | threads, symbols, call chains | `analysis/prova-profilo-q338.sh` |
| JIT dump of a function | `CPU.jit_dump_functions="A|B"` | `jitdump_<addr>.txt` | `cmp.py` |
| Hot switching of cvars | `GPU.live_cvar_file` + `XD_LIVE_AB` | alternating variants within the same run (only cvars read on every draw/frame) | `pkg_run_c.py` |
| CPU frequencies per cluster | automatic (`cpufreq-start/end.txt`, `freq.py`) | average GHz and % at maximum frequency | 10 Oct |
| Actual CPU/GPU/SoC temperature and thermal headroom | `adb shell dumpsys thermalservice` (CPU, GPU, SOC, SKIN lines) and `adb shell cmd thermalservice headroom 10` | chip degrees (not battery) and headroom 0..1 before throttling; verified available on this phone on 10 Oct (at rest: SoC 38.2 °C, headroom 0.06) | 10 Oct evening |
| GPU frame recorder (.xtr) | recorder build: uncomment `#define XE_FORCE_TRACE_WRITER 1` in `gpu/trace_writer.h`, compile, then comment it out again. In the test: `GPU.trace_gpu_prefix="<folder>/"`, `GPU.trace_gpu_frame_request="<signal-file>"`, `XD_TOUCH_AT=<second> XD_TOUCH_PATH=<signal-file>` | one file `<title>_<n>.xtr` (~100 MB) with one gameplay frame; `TraceRecord` lines in the log. **Never create the destination folder from adb** (the app could not write to it) | 11 October |
| Looping frame replayer (from q343) | `analysis/replay.sh TAG APK <path of .xtr on the phone> [DIAG] [seconds]` (starts the app with the trace in place of the game; `XD_EXTRA_PATHS` in the runner) | identical scene on every loop, no game CPU; `TraceReplay`, `GuestFps` lines, Mali counters. Ready trace: `analysis/xtr/bo1-gameplay-11700.xtr` (on the phone in `files/compose/xtr/`). **Open limits: textures partly corrupted, 7 loops/s** (see section 7) | 11 Oct (R1-R3) |
| Chip temperature and thermal headroom during a test | `analysis/termo.sh FILE SECONDS` in parallel with the runner | every 10 s: battery, system headroom, CPU/GPU/SKIN. Data 11 Oct: in Black Ops the chip sits at 92-96 C from the first seconds; headroom ≈ (chassis T - 27)/30; the phone throttles the GPU toward 43-45 C of chassis temperature (headroom 0.53-0.60) | 11 Oct (T1-T3) |
| Thermal headroom read by the emulator | `Vulkan.thermal_governor=true` (thresholds `thermal_governor_headroom_1/2`) | line `ThermalGovernor: headroom=... shading_rate=...` every 2 s; above the thresholds it switches shading to 2x1 / 2x2 (requires `vulkan_fragment_shading_rate_dynamic=true`) | 11 Oct |
| Offline trace / log comparison | `xtrmem.py` (reads within the frame against the initial snapshot), xxhash check of textures (11 Oct, R5) | says whether the recorded bytes are the expected ones | 11 Oct |
| Which guest kernel calls consume CPU, per thread | `python3 analysis/perthread_exports.py <perf-report.txt.gz>` | for each thread: % of its time inside each export (NtWaitForSingleObjectEx, NtPulseEvent, NtYieldExecution...) and in the JIT. Data on Black Ops (profile P3): CPU 1 21% in NtWait, CPU 5 14% NtWait + 7% NtYield, CPU 0 47% in NtPulseEvent | 11 Oct morning |
| Frequency of the game's wait calls | `Kernel.log_guest_sync_stats=true` (from q352) | `GuestSyncStats` line every 5 s: yields (empty), zero-timeout waits (expired), pulses (with someone waiting), rests taken, frame slack | 11 Oct morning |
| Individual frame timings | `GPU.log_guest_frame_intervals=150` | line `GuestFrameIntervals` every N frames: p50/p90/p99/max in ms. Black Ops reference at 30 FPS (U2): p50 33.3, p90 34.2, p99 35-37 | 11 Oct morning |
| Startup and loading durations | `python3 analysis/tempi_carica.py <run folder> ...` | phases seen in the GuestFps lines: normal Black Ops `L~11 S20 L21-34 S...`; the wrong spin park (T2) gave `L32 S20 L136` | 11 Oct morning |
| CPU profile with simpleperf | `XD_SIMPLEPERF_AT=<second> XD_SIMPLEPERF_SECS=15` + APK `-prof` (mk_package without NODEBUG) + `--symdir <qNNN-symfs>` | `perf.data`; from 11 Oct the runner starts at the first sample >= the requested second (before, a multiple of 10 was needed: P4 was lost because of this) | 11 Oct morning |
| Wakeups per second per thread | `python3 analysis/cambi_contesto.py <perf-report.txt.gz>` (profile with `--trace-offcpu`, already active in the runner) | wakeups/s, % running and average duration of each stretch per thread. Black Ops reference without a cap (P3): Guest CPU 0 8978/s, workers 3200/s, total ~24000/s | 11 Oct morning |
| SoC temperature from the command line | `tools/xd_soc_temp.sh` (since 3 Oct) | whole degrees of the SoC from `dumpsys thermalservice` | already existed; `termo.sh` of 11 Oct does the same with thermal headroom and time |
| Where a thread loses wall-clock time (real computation, waits, sleeps and who requested them) | `python3 analysis/attese.py <perf-report.txt.gz> [thread...]` (profile with `--trace-offcpu`) | per thread: % running per category and % off-CPU per cause, with frequency and average duration of the sleeps | 11 Oct afternoon |
| Who works while a thread waits (idle time, others' work, waiting on the GPU thread) and how many threads work at the same instant | `python3 analysis/chi_lavora.py <perf-report.txt.gz> [thread...]` | Black Ops (P3): on average 1.27 threads working, 26.5 % of the time nobody; for the main thread 21 % of the wait is idle time and 18 % is waiting on our GPU thread | 11 Oct evening, knowledge sheet |
| Pulse → worker wake-up chain | `python3 analysis/catena.py <perf-report.txt.gz>` | empty sleeps of the dispatch threads (how many per second, median, p90), delay from the pulse, sleeps inside pulse and NtWait | 11 Oct afternoon |
| Hot comparison of the latency package | `prova-W.sh X1` (files `live2/*.toml`, summary `fasi2.py`) | FPS per block with and without each piece, within the same run | plan, 11 Oct |
| Simulated tests on the Mac of the control logic (stepped limiter with measurement windows, rest adjustment, empty-wait detector, thread governor) | `bash analysis/host-tests/run.sh` (clang++, uses the source headers) | `ALL PASS` or the list of failed cases; re-run on every change to those four headers | 11 Oct evening |
| Which cores each thread runs on | `simpleperf dump -i perf.data | python3 analysis/core_per_thread.py`: samples per thread for cpu0-3 / cpu4-6 / cpu7 | says whether the threads in active wait sit on the fast cores. P3: non-critical threads 60-65 % on the fast cores | 11 Oct afternoon |
| Summary of an .xtr trace on the Mac | `python3 analysis/xtrstat.py file.xtr` | command count, PM4 opcodes, memory read | 11 Oct |
| Exact point where a draw is rejected | automatic since q342: line `IssueDraw: rejected at vulkan_command_processor.cc:<line>` (once per exit) | cause of a `Failed in backend` | 11 Oct |

> Note 11 Oct: to save disk space the symbol files (`*-symfs/libe.so.gz`), the `perf.data.gz` files and the large logs of old tests have been gzip-compressed in place: run `gunzip` before using them.

- **Translator test bench on the Mac** (11 Oct evening): `analysis/hostjit/` — `python3 gen.py && ninja -C build`. `build/ppctests` (170.000 tests of Xenia's PowerPC instructions, 23 s), `build/bench` (12 programs: translated/native time and checksum; `cmp.py` to compare loops), `fuzz/run.sh` (random programs, comparison between configurations), `fuzz/run_fp_edges.sh` (FP edge cases). All accept `--cvar=value`. **Every change to `xenia/cpu` must be run through these four before compiling the APK.** Limit: Apple cores have memory renaming, so gains that come from removing loads after stores do not show up there.

## 3. Measurement rules already established (do not rediscover them)

- FPS between different runs are NOT comparable (same build 25 → 46 FPS). Compare the **GPU work per frame** from the counters (3 October).
- Always check the **input primitives per frame** and `VkDroppedDraws`: if they change, you are not drawing the same thing (3 October).
- Above ~42.5 °C battery temperature the GPU is throttled to ~422 MHz: from there FPS ≈ 422 / (millions of cycles per frame) (10 October).
- Black Ops' automatic driving does not always reach the same point: before comparing two runs, check draws and primitives per frame (10 October).
- **Do not judge by FPS (11 Oct evening)**: verdict on the work per frame at equal scene. `analysis/lavoro.py TAG…` (CPU ms and GPU Mcycles per frame and per draw) and `fasce.py [-v] TAG…` (10 s intervals grouped by draw/frame: compares two tests only on the equivalent intervals).
- **Always outdoors** (11 Oct): never measure with the character inside the bar/room, replay included (the trace `41560855_11840.xtr` is in the bar: record one outdoors). Skip cutscenes and menus: Dante is in gameplay only from ~180 s (`prova-W.sh W4-dante-gioco`).
- Always note fan on/off: the tests of 11 Oct 22:29–22:37 were without the fan and are not comparable.
- The runner's "nominal" seconds are longer than the real ones; for the phases use `fasi2.py`, which reads the real times (10 October).
- "Sticky" maps (per-tile stencil bits) cannot be tested hot: only at startup (10 October).
- The on-screen buttons cover the right edge of the image: screenshots of that area are not conclusive until they are hidden (10 October).

## 3b. Test protocol (from the test-method research, section 7; the numbers are proposals to be replaced with those from the first A/A test)

- One question per test, written BEFORE with: hypothesis, single variable changed, expected effect on the work metrics, success rule, what is done if it succeeds and if it fails.
- Start only with battery ≤ 37.0 °C and stable; test invalid if 41.0 °C is reached; duration ≤ 3 minutes; fan always in the same state, noted.
- Variants switchable hot: alternating blocks A B B A… within the same run, pairwise comparison (ratio of medians), A/A test to learn the noise. Variants read at startup: at least 3 valid runs per arm, a weaker test.
- Main metrics are those immune to heat: GPU cycles, quads, external bytes, number of passes and transfers PER FRAME; times and FPS are secondary. Check input primitives and discarded draws.
- A win only if the effect exceeds the A/A noise (and at least 3%), repeats in 2 runs out of 3, does not worsen the minimums and the image is identical. Otherwise "no detectable effect": no gain is declared.
- Stop doing: comparing separate game runs, reading FPS from tests not temperature-controlled, running long tests to decide.
- Method goal (2-4 days, not yet done): **looped replay of a recorded frame** (.xtr format, TracePlayer already in the source; missing: the loop, the measurement and the entry into the Android app). This is what Dolphin (FifoCI), PCSX2 (gsrunner), DuckStation do: identical scene, no game CPU, a few seconds per test.
- Every new switch has a deadline: within 30 days it either becomes the default and disappears, or the code is removed.

- **Operating rules set on the night of 10-11 October (they apply to the whole project):**
  1. Before any work: read this registry and run `bash work/cerca.sh "word"`. Never redo existing work.
  2. One test at a time. Before running it, write the question and success criterion; afterwards, the conclusion in the log (including failures) with `log.sh`.
  3. Start below 38 C battery, minimum useful duration; note fan yes/no.
  5. Every measured improvement goes into the source as standard behavior for all games (no per-game profile) AND into the daily build on the phone (`multigame-core-audit/best-apk.txt`, with the new UI). A performance/heat improvement is not switched off because of a graphics defect: the defect is noted in section 7 and fixed afterwards.
  6. Tests do not require manual play: automatic commands (`game_inputs.json`) for in-game tests, frame replayer for GPU work comparisons.
  7. The test phone: no root, no system changes, nothing that could damage it. Shizuku is available if a tool requires it.
  9. Promoting a build: only after the multi-game test; install and CHECK the adb result (never `adb install ... | tail` followed by `&&`: the pipe hides the failure). `best-apk.txt` must always state what is actually on the phone.
  8. At the end of each work block: update the registry, the log and the index.

## 4. Renderer mechanisms ALREADY implemented

| Mechanism | Cvar | Since | Status |
|---|---|---|---|
| Skipping transfers to buffers overwritten by a full clear | `rt_cache_skip_transfers_for_full_clears`, `rt_cache_dont_care_load_for_full_clears` | 5 Oct | on |
| Cutout of the rectangle that a clear overwrites | `rt_cache_draw_overwrite_cutouts` | q238 | on |
| Depth/stencil clears through another MSAA view redirected to the owning buffer | `rt_cache_retarget_full_depth_clears`, `rt_cache_retarget_depth_only_clears` | q239/q241 | on |
| Clear chains (per slot, per aspect), absorb the redirected clears | `rt_cache_draw_clear_chains` | q240/q242/q249 | on; **had regressed** for depth, restored in q337 |
| Clears of new depth owners | `rt_cache_clear_new_depth_owners`, `vulkan_defer_new_depth_owner_transfers` | 5–6 Oct | on |
| Render pass area reduced to what is drawn | `vulkan_render_area_shrink`, `vulkan_render_area_draw_bound` | q250 | on |
| Render target height capped | `render_target_height_cap` | 6–7 Oct | on (768) |
| Transfers inside the game's pass | `vulkan_in_pass_transfers` | Oct | on |
| Direct and to-texture resolves | `vulkan_direct_host_resolve`, `vulkan_resolve_to_texture*` | Sep–Oct | on |
| In-pass resolves | `vulkan_in_pass_resolve` | Oct | **off**, suspected of black rectangles |
| Stencil: skip if never used, per-buffer mask, per-tile map | `rt_cache_skip_unused_stencil_transfers`, `rt_cache_stencil_bit_transfer_mask`, `rt_cache_edram_stencil_bit_map` | Oct, q332 | on; the map alone skips nothing on Black Ops |
| Stencil not dirtied by color draws (heuristic) | `rt_cache_color_draws_dirty_stencil=false` | q335 | **off**; −19% GPU cycles in a static scene before q337, to be re-measured later |
| Unused attachments not loaded/stored | `vulkan_relax_unused_attachment_ops` | q325 | on; no measurable effect |
| Full-screen quads as overwrite | `rt_cache_full_overwrite_quads` | q338 | on; does not trigger on Black Ops (alpha test active) |
| Single-sample MSAA buffers | `vulkan_msaa_single_sample_storage` (+ `vulkan_msaa_single_sample_resolve`) | q332 | **on by default in the source since the evening of 10 Oct (not yet compiled)**; −29% GPU cycles in game; the right-hand band is more visible |
| Variable-rate shading | `vulkan_fragment_shading_rate_dynamic` + `vulkan_fragment_shading_rate` | q332 | off; never measured in a heavy scene |
| Anisotropic cap | `anisotropic_cap` | q328 | 4x |
| Adaptive limiter 60→30 | `framerate_limit_adaptive` | q336, fixed in q350 | **on since q350**. Halves only if delivered frames are < 87% of the limit for 3 s; returns to 60 if the work (excluding the pacing sleep) is within 80% of 16.7 ms for 6 s. Black Ops: 30.1 FPS with p10 30.0 instead of 30.7 with p10 25.2. Mistake not to repeat: in q349 the pacing sleep was counted as work and pushed Dante to 30 |
| Frame wait without spinning | `guest_swap_pacing` | q331 | **on since q350** (required by the adaptive limiter) |
| Single-sample MSAA buffers | `vulkan_msaa_single_sample_storage` | q332 | **on since q340**: Black Ops −25/−30% GPU cycles, Dante −14% |
| Variable-rate shading as dynamic state | `vulkan_fragment_shading_rate_dynamic` + `vulkan_fragment_shading_rate` | q332 | dynamic **on since q350**, rate 0 (full) except when the governor intervenes; on the replayer 2x2 = −18/21% GPU cycles, 2x1 = −15% |
| Thermal governor | `thermal_governor`, `thermal_governor_headroom_1/2` (0.50 / 0.55) | q347, fixed q348 | **on since q350**: reads the thermal headroom every 2 s and switches to 2x1 / 2x2 when the chassis exceeds ~42 / ~43.5 C |
| Rest for threads that read the time base in bursts | `guest_timebase_spin_park_us` (100) | q351 | on in the candidate; measured on Black Ops at 30 FPS: CPU 4.32 -> 3.52 cores, workers 0.75 -> 0.41, FPS p10 29.9. Not yet in the daily build (q350) |
| Rest for repeated polls that do not change state (empty yield, expired zero-timeout wait, pulse with no one waiting); footprint = callee-saved registers + 1 KiB of the callers' stack; adaptive threshold 300 us - 5 ms | `guest_poll_spin_park_us` (100), `guest_poll_spin_park_after_us` (300) | q352 | on in the candidate, **never tested on the phone** |
| General sampling detector for active waits (footprint r0-r31, CTR, LR, 1 KiB of stack at the end of each time slice; 3 repetitions out of 16 = spinning) | `guest_spin_sample_park_us` (0 = count only; N = maximum sleep in us) | q352 | count only (`spin_samples` in `GuestSyncStats`); **never tested** |
| Frame slack required for rests to be allowed | `guest_spin_park_min_slack_ms` (5) | q352 (fixed at 2 ms in q351) | requires `guest_swap_pacing`; without slack the wait cycles stay at full speed |
| Rests adjusted to each frame's slack (sleep scale 0.25x-4x, window that closes when the current frame overruns) | `guest_spin_park_adaptive`, `guest_spin_park_max_scale_percent` | q353 | on in the candidate, **never tested**. Same idea as RPCS3 "Max CPU Preempt Count" (PR 12614), but with automatic tuning |
| Thread governor: light guest threads or ones that spin on the slow cores (2.1 GHz) while frames have slack | `guest_threads_placement` (0/1/2), thresholds `guest_threads_placement_*_percent` | q353 | on in the candidate, **never tested**. The data motivating it (profile P3): Black Ops' non-critical threads spend 60-65 % of their time on the fast cores. Different from `thread_placement` 1/2 and `guest_threads_efficient_cores`, which are static and were only tested for FPS |
| Frequency cap (uclamp) for spinning threads | `guest_threads_spin_uclamp_max` (0), probe `log_guest_threads_uclamp_probe` | q353 | off; it is not known whether the phone's kernel allows it (test `W7`) |
| Menu switches applied to the game in progress | `live_cvar_file`, default `<storage>/ui-live.toml`, deleted at every startup | q353 + ui14 | **never tested** |
| Pulses with no rest and no waiter delay; braking of pulse-only cycles in `NotePollSpin` | `pulse_event_skip_without_waiters` (now **true**) | q266, default changed in q354 | **never tested under this condition**. On 7 Oct it had been left off (+0.8 cores, same FPS) when the limit was the GPU |
| Global lock: retry for 40 us before the futex | `global_mutex_spin_us` (now **40**) | b109 (1 Oct), default changed in q354 | never tested on Black Ops; on MW2 on 2 Oct it gave no FPS (GPU limit) |
| Idle dispatch threads stay alert for 80 us when frames are late (adaptive, never on a hot phone) | `guest_scheduler_idle_spin_us` | q354 | never tested |
| Lock-free response to zero-timeout polls on an unsignaled event (per-thread memo of the last handle + handle table generation + lock-free state read) | `wait_poll_fast_path` | q356 | never tested |
| Three-step limiter (60 / 40 / 30) + screen refresh rate vote (`requested_present_rate` JNI) | `framerate_limit_adaptive_mid` (0 in the core; the UI writes 40 if the screen has a mode that is a multiple of 40) | q356 + ui15 | never tested |
| Poll pause capped at 1/8 of the wait duration | `guest_poll_spin_wait_share` (8) | q357 | tested only in simulation |
| Recognition of non-gameplay phases (few draws per frame, bursts of file reads) | `guest_spin_park_gameplay_min_draws` (96) | q357 | never tested |
| Limiter measurement windows at full speed to decide the step-ups | automatic with `framerate_limit_adaptive` | q357 | tested only in simulation |
| Lock-free check of the resolve destination instead of `QueryRegionInfo` | `gpu_resolve_watch_fastpath` (false) | 9 Oct | **never tested on the phone** (it was in the 9 Oct queue, not run); now in test `X1` (`live2/x-rwf.toml`) |
| GPU command thread stays alert for 80 us before sleeping when frames are late | `gpu_stall_spin_us` | q358 | never tested |
| Interruptible pauses: the dispatch thread sleeps on a futex per CPU and wakes as soon as something becomes runnable on its core | automatic (all pauses go through `ParkCurrentDispatchThread`) | q358 | never tested |
| Sampling detector on by default, with pause capped at 1/8 of the "standing still" time | `guest_spin_sample_park_us` (now **2000**) | q358 | tested only in simulation; it is the mechanism that covers wait cycles without kernel calls (MW2) |
| Guest registers pinned to host registers in loops without calls (HIR opcodes `LOAD_PINNED`/`STORE_PINNED`, x28-x26 and v31-v29 removed from the allocator only where needed; the stores into the context remain) | `a64_loop_register_pinning`, `a64_loop_pinned_gprs`, `a64_loop_pinned_vecs` | q359 | on in the candidate; verified on the Mac (bench, 48.000 random functions); gain not measurable on the Mac (memory renaming); **phone (hostjit bench, core 7, 11 Oct 20:13): average -14%, strings -58%, crc32 -33%, interpreter -24%**; in games still to be measured |
| Single-precision arithmetic done in single precision when the operands are provably single | `a64_single_precision_narrowing` | q359 | **phone (bench, core 7): quaternions -39%, matrices -17%, average -6%**; on the Mac quaternions -27%, matrices -11%; bit-for-bit identical except two cases where it is more faithful to PowerPC |
| Correct decoding of the `mtfsfi` immediate (bug also present in xenia-canary) | `ppc_mtfsfi_exact_immediate` | q359 | on; effect on games unknown |
| After a long jump (longjmp) the host stack returns to the frame of the arrival function (before: to that of the function it had called, and only for jumps over 3+ frames) | `a64_longjmp_target_frame` | q360 | on; verified on the Mac with `hostjit/build/longjmp` (300.000 jumps at 1-3 levels); never on the phone |
| Leaf functions that read or write LR (hand-made setjmp/longjmp) are no longer inlined | inside `inline_leaf_calls` | q360 | fix, no option |
| 64-bit constants with more 1-fields than 0-fields (sign-extended addresses): MOVN+MOVK, 2 instructions instead of 4 | no option (xbyak `MvImm`) | q361 | -1.4% code in the bench; times unchanged on the Mac |
| Direct bit branches (tbz/tbnz) in functions up to 6 KiB of guest code, re-emitted if they do not reach | `a64_near_bit_branches`, `a64_near_bit_branch_max_guest_bytes` | q362 | Mac: virtual calls -8.5%; **phone (bench): no measurable effect** |
| Indirect calls: indirection table bases read from the backend context with an ldp, outer form moved to the tail (14 instructions + 2 taken branches -> 8) | `a64_indirection_bases_in_context` | q363 | Mac: virtual calls -9%; never on the phone |
| Comparison computed next to the conditional branch that uses it (the backend fuses them: `cmp + b.cond` or `cbz/cbnz`) | `a64_sink_compare_to_branch` | q371 | -63 instructions in the bench; on the Mac `virtual` gets worse due to code layout: **the phone decides** (nosink variant) |
| Up to 5 integer registers pinned per loop (before 3; limit 6) | `a64_loop_pinned_gprs` (now **5**) | q369 | in the `hash` loop the re-reads from the context disappear; Mac unchanged; **to be measured on the phone** (bench: pin3/pin6 variants) |
| Simplification knows the possible bits after a rotate-and-mask (rlwinm) and removes useless masks and truncate/extend pairs (rlwimi, mullw) | `simplify_rotate_mask_word_bits` | q370 | fixes a loss introduced with ROTATE_MASK_WORD; rlwimi from 8 to 3 instructions in the bench |
| Single precision recognized across blocks and loops (forward analysis: which guest FP registers surely contain a single at each read) | `a64_single_precision_across_blocks` | q368 | Mac: matrix product -35%; single operations left in double 60 -> 12 in the bench; never on the phone |
| No thread-switch point at the entry of leaf functions that do not write LR | `preempt_check_skip_leaf_entry` | q368 | less code, times unchanged on the Mac |
| Guest calls: return address taken from the register instead of the frame (-2 instructions per call) | `a64_call_return_address_in_register` | q367 | Mac: together with the next one, calls -10.7%, virtual -9%; never on the phone |
| Guest calls: no long-jump check after each call; synchronization goes through a helper only when `ResolveFunction` recognizes the jump | `a64_longjmp_sync_at_resolve` | q367 | test `hostjit/build/longjmp` 5/5; never on the phone |
| Floating-point loads and stores with byte swapping in vector registers, without going through integer registers (lfs/lfd/stfs/stfd) | `a64_float_memory_domain` | q366 | Mac: quaternions -6%, matrices +2%; **to be measured on the phone** (`hostjit/telefono-bench.sh`, nodom variant) |
| Limiter: during the probe the step returns to the full limit and delivered frames are counted (before it looked at work time, which in a game with its own sync wait is always full: Dante stayed at 40) | inside `framerate_limit_adaptive` | q364 | new logic test (Dante case) ALL PASS; to be confirmed on the phone |
| A burst of file reads counts as loading only if the frame does not arrive with 1/8 of the period in slack (audio/texture streaming no longer turns off rests and governor) | `guest_loading_ignores_streaming` | q365 | to be measured (Black Ops: CPU at 30 FPS) |
| Frame recorder and replayer | see section 2 | q341–q347 | measurement tool; `ExecutePacket(ptr,count)` made virtual |
| Texture sign specialization, exp adjust, fast rounding | `spirv_texture_sign_specialization`, `spirv_fast_precision_rounding` | q243–q247 | on |

JIT and CPU: package q319–q323 (10 October), CAS, DSE, scalar loops (10 October), `docs/status/ANALYSIS-AND-SOLUTIONS-2026-10-06.md` (direct branches, local layouts, lwarx/stwcx).

## 5. How to notice a regression (do this for every candidate build)

One Black Ops run with `GPU.log_gpu_frame_time_breakdown=true`: note below the GPU ms per frame in the `xfer` passes and the total, together with draws and primitives per frame. If the `xfer` ms go up compared with the previous row, something in the chains/cutouts has regressed.

| Date | Build | Scene (draws, primitives per frame) | GPU `xfer` ms per frame | total GPU ms per frame | Notes |
|---|---|---|---|---|---|
| — | — | — | — | — | to be filled in at the next test |

### Multi-game table (one test per game for each promoted build; FPS in game window, GPU = active Mali cycles)

| Date | Build | Fan | Game (seconds) | FPS avg / p10 | Draws/frame | CPU cores | GPU Gcycles/s | M cycles/frame | T end | Image |
|---|---|---|---|---|---|---|---|---|---|---|
| 11 Oct | q340-ui12 | yes | Black Ops (200) | 30.7 / 25.2 | 4097 | 4.41 | 0.728 | 23.7 | 36.3 | ok, strip on the right |
| 11 Oct | q340-ui12 | no | Dante (180) | 60.0 / 58.0 | 311 | 2.37 | 0.538 | 9.0 | 37.7 | ok |
| 11 Oct | q340-ui12 | no | Halo 3 (170, menu) | 30.0 / 29.5 | 2263 | 1.63 | 1.186 | 39.5 | 41.1 | white background (known) |
| 11 Oct | q340-ui12 | no | GTA IV (240) | 21.9 / 21.1 | 1263 | 2.64 | 0.424 (throttled) | 19.4 | 45.3 | ok |
| 11 Oct | q350-ui12 | no | Dante (180) | 60.0 / 56.2 | 501 | 2.34 | 0.571 | 9.5 | 42.0 | ok |
| 11 Oct | q350-ui12 | no | Black Ops (185) | 30.1 / 30.0 | 2307 | 4.32 | 0.640 | 21.3 | 43.6 | ok, strip on the right |
| 11 Oct | q350-ui12 | no | GTA IV (200) | 30.0 / 22.9 | 1263 | 2.46 | 0.799 | 26.6 | 44.7 | not checked by eye |
| 11 Oct | q350-ui12 | no | Halo 3 (170, menu) | 30.0 / 29.5 | 2160 | 1.59 | 1.236 | 41.2 | 41.8 | white background (known) |

## 6. Dead ends or failed paths (do not reopen without new data)

| What | Outcome | Where |
|---|---|---|
| Shared memory BDA/AHB/segmented, vertex ring, scudo purge, heapprofd, pipeline eviction, late-Z sample-mask, `robustBufferAccess` off | closed | 7–9 Oct |
| WFE / spin park / Pulse throttle on Black Ops | closed (they lose FPS or do not change the cost per frame) | 7 Oct |
| Making the game believe the surface is 1x | breaks the clears across 4x views | 10 October |
| "Marker" pass for stencil | worse (+12% discarded quads): Mali runs the stencil test after the shader when there is a discard | 10 Oct evening; code removed |
| Per-tile stencil map as the only fix | skips 1.000 passes out of 450.000 | 10 Oct evening |
| `guest_swap_pacing` as a heat fix on Dante | −3% cores | 10 Oct evening |
| Recognizing Black Ops' post-processing quads as clears | impossible: alpha test active | 10 Oct evening |
| Newer or custom Mali drivers for Mali-G720 | do not exist (driver research) | driver research |
| `NtPulseEvent` / `NtWaitForSingleObjectEx` (15% CPU on Black Ops) | already optimized (skip without waiter, targeted wake); no new lever | profile P3, 10 Oct evening |

- **Note of 11 Oct on the rows "WFE / spin park / Pulse throttle" and "NtPulseEvent / NtWaitForSingleObjectEx: no new lever" above**: those closures hold for the condition in which they were measured (GPU saturated: 31 M cycles per frame). From q340 Black Ops is no longer GPU-limited; the analysis of profile P3 shows that the sleeps inside the pulses, the global lock contention and the worker wake-ups are what make the frame last 32 ms. Reopened with this new data (latency package of q358), to be measured with `X1`.
- **Spin park with the 30 FPS limit** (11 Oct, T2): `coop_spin_backoff_interval=16` + `guest_spin_park_us=200` + adaptive limiter -> Black Ops on a black screen after the intro, as in q277. Closed under this condition too.
- **Variable-rate shading as a large GPU saving** (11 Oct, R7 on the replayer): 2x2 removes only 18-21% of the GPU cycles, 2x1 15%: the 3D world with depth testing is a small part of the frame. Useful only as a step of the thermal governor.
- **Looking for other large wastes in Black Ops' GPU passes** (11 Oct, R8): frame of 46 passes, ~14 ms at full clock, spread out (scene 3.1 ms in 8 passes, shadows 0.8, post 0.6, final 0.6, transfers 0.75, tiling ~5.3). No dominant item left.

- **WFE with the 30 limit, measuring temperature** (11 Oct, T4): same temperature and same cores. Closed.
- **Turning off `apu_performance_hint` to lower the frequencies** (11 Oct, T5): no effect on temperature and frequencies. Closed.

## 7. Known open defects

- **Strip of the last 64 pixels on the right in Black Ops** (column [960.1024) of the scene): absent in q232, present from q280 onward; with single-sample buffers it becomes stretched garbage. Cause not found. Historical candidates: q238–q250.
- Halo 3 white, black shadows in Mass Effect, magenta HDR in Reach (6-7 October).
- **Frame replayer (11 Oct)**: (a) some textures come out corrupted (colored blocks) in the playback of `bo1-gameplay-11700.xtr`: texture memory not fully captured or read back wrongly; until it is solved the GPU work per frame of the playback (27.5 M cycles) is NOT comparable with the game; (b) 7 loops/s because each loop decompresses and compares 205 MiB on the GPU thread: cache the decompressed blocks or skip unchanged reads without decompressing.
- **Heat, main cause (11 Oct)**: in Black Ops the chip is at 92-96 C from the first seconds; the CPU uses 4-4.7 cores and the game's worker threads (Guest CPU 2/3, functions sub_82544ED8 / sub_82545138 / sub_825462A0 + syscalls) consume MORE when frames are fewer (0.50 -> 0.93 cores with the 30 limit): active waits. With the adaptive limiter + pacing + thermal governor the 30 FPS stay fixed up to 47 C, but the temperature does not stabilize (38 -> 47 C in 340 s, charging via USB). To do: reduce active waits without blocking the game (WFE and park only on NtYieldExecution, to be measured with termo.sh).
- **Replayer, textures** (updated 11 Oct morning): the main cause was in the replayer, not in the recording: the registers were copied again at every loop without notifying the backend, which then discarded as redundant the writes of the fetch constants and left wrong textures bound. Fixed in q352 (from the second loop on the registers are not restored), TO BE VERIFIED with a screenshot of the playback of `xtr/bo1-gameplay-11840.xtr`. Offline fact established: for the pages marked "written by the GPU" (Black Ops uses memexport) guest RAM and the shared Vulkan buffer diverge (of 340 compressed textures, 118 valid in RAM, 187 valid in the buffer, 45 in neither): open question whether a reload of those textures in the game would read old data. Tools: `xtrtex.py` (decodes one trace texture to PNG), `xtrsnap.py` (same addresses from the initial snapshot).
- **METHOD PRIORITY (11 Oct night): fix the "same frame" test**. Today the replay stays stuck on ONE frame, with glitches: not acceptable. It must always restart from the exact same frame and then let the game RUN (a frame sequence recorded outdoors, or a deterministic start of the real game, or a saved state of the emulator). Success criterion: A/A test within 1-2%, no glitches.
- **Source batch after q371, NOT compiled (11 Oct, 22:58)**: `a64_float_memory_domain_doubles` (false), `a64_shared_cold_paths` (true; Halo 3 −16.3% host code), `guest_poll_spin_tight_gap_us` (12) + `polls:` counters in GuestSyncStats, preempt test in the hostjit bench. Verified on the Mac (controlli.sh, host-tests). UI switches and build q372 are missing.
- **Black Ops empty polls (11 Oct night)**: ~800.000/s, `poll_parks=0` even with `loading=0`: the state changes at every loop, the footprint detectors do not trigger. q371 = q358 in CPU ms per frame at equal scene; `guest_loading_ignores_streaming` no measurable effect.
- **Candidate q371-ui23 (11 Oct), TESTED on the phone with fan (W1bv, W1cv): works, no CPU gain in Black Ops; not promoted** (multi-game test missing). Original entry: never started, replaces q370: `analysis/q371-ui23.apk` + `multigame-core-audit/q371-prof.apk` + `q371-symfs/` (fingerprint in `q371-ui.log`). q370 plus the comparison next to the jump; ui22 panel.
- **Candidate q370 (11 Oct, late evening), never started, superseded by q371**: `analysis/q370-ui21.apk` + `multigame-core-audit/q370-prof.apk` + `q370-symfs/` (fingerprint in `q370-ui.log`). q368 plus 5 registers pinned per loop (q369) and simplification after rlwinm (q370); ui21 panel.
- **Candidate q368 (11 Oct, 21:48), never started, superseded by q370**: `analysis/q368-ui20.apk` + `multigame-core-audit/q368-prof.apk` + `q368-symfs/` (fingerprint in `q368-ui.log`). q367 plus single precision across blocks and no thread-switch point in leaves; ui20 panel. Bench on the Mac: 1.94x native (this morning 2.27x). Five checks passed, FP edge cases in 3 block forms. **The phone has q365 with the configuration of an interrupted test**: on reconnection `prova-W.sh RIPRISTINA`.
- **Dead ends on the evening of 11 Oct**: redesign of the stack points (cap -6% Mac / -11% phone on the call program alone: not worth the risk); latency package to go above 30 FPS in Black Ops (the game is locked to 30 by the GPU: 20-25 ms per frame); double byte swap in copies (already eliminated upstream).
- **Candidate q367 (11 Oct, 21:35), never started, superseded by q368**: `analysis/q367-ui19.apk` + `multigame-core-audit/q367-prof.apk` + `q367-symfs/` (fingerprint in `q367-ui.log`). q365 plus FP loads/stores in the vector domain (q366) and the two cuts on calls (q367); ui19 panel. Five checks on the Mac passed. **The phone has q365 with the configuration of an interrupted test (new translator off)**: on reconnection `prova-W.sh RIPRISTINA`, then Dante (limiter fix), Black Ops (CPU with streaming fix), `hostjit/telefono-bench.sh`, R1 (declared resolution).
- **Candidate q365 (11 Oct, 20:44), partly tested on the phone, superseded by q367**: `analysis/q365-ui17.apk` (libe `7f5f225ee86b59a5`) + `multigame-core-audit/q365-prof.apk` + `q365-symfs/`. Compared with q361: direct bit jumps (q362), lightened indirect calls (q363), **limiter with probe at open limit** (q364: fix of the Dante 60->40 FPS regression), **streaming not swapped for loading** (q365: in Black Ops rests and governor were off 93% of the time). First measurements on the phone (fan on): Black Ops steady 30 FPS (locked to 30 by the GPU), GTA IV outdoors 24 FPS (first valid measurement: the previous ones were in the room), translator bench 2.49x native (was ~3.05x). NOT yet promoted.
- **Candidate q361 (11 Oct, 20:06), superseded by q365**: `analysis/q361-ui17.apk` (libe `0734028ec8b24d39`) + `multigame-core-audit/q361-prof.apk` + `q361-symfs/`. q360 plus the short 64-bit constants (MOVN+MOVK for sign-extended addresses: -1.4% code in the bench). Five checks on the Mac passed. Tests: `prova-W.sh`. q358-ui16 remains as fallback; q359/q360 in the Trash.
- **Candidate q360 (11 Oct, 19:58), never started, superseded by q361**: `analysis/q360-ui17.apk` (libe `3955b938d72c3f89`) + `multigame-core-audit/q360-prof.apk` + `q360-symfs/`. q359 plus two correctness fixes: correct frame after a long jump (`a64_longjmp_target_frame`) and no inlining of leaves that touch LR. Five checks on the Mac passed (`hostjit/controlli.sh`). Tests: `prova-W.sh`. q358-ui16 remains as fallback; q359 in the Trash.
- **Candidate q359 (11 Oct, 19:41), never started, superseded by q360 twenty minutes later**: `analysis/q359-ui17.apk` (libe `016231fe5e99f869`) + `multigame-core-audit/q359-prof.apk` + `q359-symfs/`. Everything in q358 plus three translator changes verified on the Mac with the `hostjit` bench (141.346 instruction tests unchanged, 12/12 programs, fuzz and floating-point edge cases with zero differences): registers pinned in loops, single arithmetic in single, `mtfsfi` fix. ui17 panel (entry "Faster translation of cycles and floating point"). Tests: `prova-W.sh` (W0, X1, W1, then **J1** = new translator on/off). q358-ui16 remains as fallback.
- **Candidate q358 (11 Oct, evening), never started, superseded by q359**: `analysis/q358-ui16.apk` (libe `ce6cd7a5bd776eec`) + `multigame-core-audit/q358-prof.apk` + `q358-symfs/`. Regulated rests, thread governor, latency package, 60/40/30 limiter, ui15 panel with immediate effect. Tests: `prova-W.sh` (first `W0`, then `X1`). The intermediate APKs q353/q354 are in the Trash.
- **Black Ops is limited by latencies, not by computation** (offline analysis of profile P3, 11 Oct; details in `research/KNOWLEDGE-SHEET.md`, section 2026-10-11): 10.4 ms of real code per frame on the main thread and 9.4 on the rendering one, within a 32 ms frame. This is the reason for the latency package. To be confirmed with test `X1`.
- **Candidate q353 (11 Oct afternoon), superseded by q356**: `analysis/q353-ui14.apk` + `multigame-core-audit/q353-prof.apk` + `q353-symfs/`. q352 plus rests adjusted to the slack, thread governor on the slow cores, immediate-effect switches (ui14, 130 tests). Tests: `prova-W.sh`.
- **Candidate q352 (11 Oct morning), never started**: `analysis/q352-ui13.apk` + `multigame-core-audit/q352-prof.apk`. Contains the rests for waiting threads (time base + polls), the slack threshold, the correction of the limiter's hysteresis, the GuestSyncStats counters and the new Performance panel (ui13, never seen on screen). The phone still has `q350-ui12`.
- **Structure of Black Ops' waits (from the listings of 6 October)**: `sub_82545448` = run jobs while predicate(arg) holds; without a job it restarts immediately. `sub_82544ED8` = looks for a job in 8 queues and, if it finds none, calls a yield (`0x824CFD60`). `sub_82545138` = worker loop, with two reads of the time base per job. One thread (Guest CPU 0) spends half its time in NtPulseEvent, which inside sleeps 60-100 us when no one is waiting (`pulse_event_skip_without_waiters`, off because on 7 Oct removing it gave +0.8 cores: to retry now that the waits go to rest).


## 2026-10-12T00:02:30.103264+02:00 — work stopped
Savestate q375-ui25 (APK e92dad4e48923e99 / native ba486e59cf67fc44), socketUDPoffline supported with validated raw/alias handles; profile60/40/30 toggleOFF respected, plugin permission never disabled. Host test PASS, runtime save/restore NOT validated. SAV4 interrupted before save, phone disconnected during cleanup: daily/base NOT verified, first command on reconnection `prova-W.sh RIPRISTINA`. No queue/build/lock. Cleanup only of cache and nativebackupidentici: 8.566.673.408 bytes effective, data/knowledge/tools preserved; .cxx.o/.a still present.

## 2026-10-12 — savestate, Timer and ISO fingerprint
- BO outdoors (q375): capability rejected for **XTimer** (type 13). q376 adds XTimer: **capability ok=true** on BO (first time).
- Save "stuck" = `RuntimeSnapshotIdentity` was computing SHA-256 of the entire ISO at every save/restore before the barrier. q377+: hash once in the background at launch (only with savestate_enable), reused until the file names/sizes/dates change.
- Timers: frozen during capture (Disarm waits for the end of the callbacks), armed after the guest clock is restored, APC thread resolved by id. kObjTimer=13 format validated (host test with fuzz PASS).
- Candidate q379-ui25, never tested on the phone. Proposed graphics config (not installed).

## 8. Update 12 → 22 October (2026-10-22)
Rule (22/10): **before any work read this section + `cerca.sh`; after every verdict add a row here, successful or failed.** A daily log alone is not enough.

### 8a. Worked (kept in the source)
| What | Build | Measured data |
|---|---|---|
| BO and Dante savestates (save + restore) | q401 | restore OK, savestates fixed on the phone |
| Dante black blocks = Arm erratum 3786926 (dynamic rendering r49p1) → classic render passes on Mali < r52 | q446-q447 | blocks 40 → 3-6 frames (detector floor), BO no cost (59.0 vs 59.6) |
| DONT_CARE "one-time" also consumed in classic passes | q447 | part of the block fix |
| Halo 3 white (guest state after transfer cutout) | q416 | 4 clean runs |
| Remove JIT context forwarding | q457 | host code −1.4%, 0 lab regressions |
| Daily build q460 (= q423 + q429-q460) | q460 | BO street 38-39.5 vs 35.1 fps, Dante blocks solved, −7.000 dead lines, library −20% |
| Dirty-group draw path + bitmap shared-memory coherence + JIT arena | q464 | CP 10.9 → 10.4 on the replayer |
| Function-level JIT register cache | q471 | Mac bench −7/−10%, in game within noise |
| LOD bias folded to zero (spec constant) | q475 | −2.6% GPU cycles on replayer, same bandwidth |
| Depth resolve via compute | – | −7.5% GPU cycles |
| Depth transfers deferred (`vulkan_defer_new_depth_owner_transfers`) | – | −98% pixels transferred, −5.5% GPU cycles |

### 8b. Failed or neutral (do NOT reopen without new data)
| What | Build | Why |
|---|---|---|
| Guest spin park (fixed sleep) | q277-279 | BO −18% fps: blind sleep relative to job arrival |
| WFE / pulse without sleep / critical thread priority | q429, q461, q462 | no gain at equal draws; q461 removed |
| CP pinned to the 3.25 GHz core (`thread_placement=1`) | q468 | BO −7% |
| Resolve inside the render pass (v1 and v2) | q465, q469 | v1 bandwidth +17%, cycles +28%; v2 neutral. AFBC is not involved |
| Merging draws MDI/instancing | q474 | CP −4% but GPU +10%, bandwidth +39% on Mali |
| JIT third round (calls/constants), unroll | – | crumbs, not integrated |
| Pixel shader translator, rounds 2-4 | rw-malioc2/3 | 0% (top 10 PS = 67% arithmetic, spill F454 unchanged): track closed |
| Vertex fetch vector on split shared memory | q479 | cancelled: on the phone the binding is unique (2 GiB) |
| Internal driver memory (pool) | rw-gpuram | duplicated q296/q298 |
| CP reuse across frames | cp-redesign | BO is GPU-limited even in the street: heat only |
| Dante dither pattern: alpha-to-mask | RET-a2moff | off: dither pattern still at 79% |
| Attachments not written with storeOp NONE (`vulkan_relax_unused_attachment_ops`) | q325 | on, no measurable effect |
| Texture sign specialization 16 slots | q476 | malioc PS −23% but replayer −0.5% cycles, −3% bandwidth: the PS are not the limit |
| Render pass ops ANGLE-style on classic passes (finalize, clear→LOAD_OP_CLEAR, store lookahead) | q483 | on BO 0.13 ops changed/pass, 0 clears folded, 0 stores removed: BO clears with quads, the lookahead stops at the first draw; suspends the CP→encoder streaming. Code off |
| Image→image resolve only for identity resolves | rw-resolveimg | ceiling 28.7 MB/frame (<30): depth/MSAA/fmt5 excluded. No code |
| Precise shared-memory barriers (Use) | analysis 23/10 | the 20 'breaks from Use' are the resolves themselves (CW→CW 17/frame): only 2-3 avoidable. Not implemented |
| Zero-copy depth stage 1 (q485-q486) | q486 | never triggers on BO: fallback_schema on all resolves (format/state conditions to be fixed) |

### 8a+ (22/10) Worked
- Replayer: cause of corrupted textures found and fixed in the writer (download offset never incremented + RAM reads above the GPU snapshot); new trace to be recorded.
- Byte budget `Vulkan.log_pass_bytes=N` (q482-q484): BO stable frame resolve 98.7 MB, copies 52,4, ownership 33.1 (load/store passes counted from q484).

### 8c. Under measurement (updated 23/10)
q486 ABAB 23/10: resolve_defer+resolve_texture_copy on = bandwidth −5.5%, cycles +1.4% (attribution in progress). q486 (= everything): zero-copy depth `vulkan_texture_from_render_target` (ceiling 46 MB/frame), resolve_defer, resolve_texture_copy — queue-final.sh → FINAL.tsv. Old line:
q476 texture sign 16 slots (malioc PS −23%), q477 unified resolve→texture copy, q480 deferral of resolves (estimate −51/77 MB/frame), q481 vector vertex fetch with single binding (malioc VS −15.7%), "all together" run q481. Queues SGN/RTX/DFR/VTX/ALL in `lab/test`.

- **23/10 evening, zero-copy depth q492 (stencil captured):** triggers 100% of the time (76 resolves/s served from the surface, 0 fallback) but **materializes 100%** and the replay collapses to 18.95 loops/s (vs ~57): every shadow resolve at the same address was materializing the previous frame's one, with the GPU stalled (EndSubmission + wait) at every materialization. **Lesson:** a lazy provider must be DISCARDED when it is overwritten, never materialized; and no materialization must stall the GPU on the hot path. Fix in q493 (discard by identical geometry), under measurement.

- **23/10 night:** q488 (mip tail upstream) ABAB = bandwidth −7% confirmed, cycles neutral (the −9% between runs was not true). q494: the zero-copy was materializing because of `Use(kGuestDrawReadWrite)` without a range at every draw (fix q496); 13 of the passes dedicated to transfers/frame come from the clear chains → `rt_cache_clear_chain_in_pass` (q495/q496) under measurement. **Lesson:** before writing a fix, put a counter in that says who is really calling (the geometry discard of q493 was right but useless: the culprit was elsewhere).

- **24/10 night, pass breaks:** clear chains inside the open pass (q495-q498, `rt_cache_clear_chain_in_pass`): ABAB −1.5% cycles; with the sources prepared at the start of the chain, pass closures 46 → 40/frame and bytes −7% (diag q498). Under measurement q499: depth clears retargeted at the head of the game pass (`rt_cache_defer_retarget_clears`) and zero-copy without readback. Causes still mapped: 3 texture loads mid-scene (RequestTextures, pass head rejected), resolves.

- **24/10 night, pass rewrite set (q506):** logical budget 579 → 432 MB (−25%), pass closures 48 → 34; ABAB on the phone: cycles −2.7% (noisy), real bandwidth unchanged, **CPU −22%**. **Lesson:** the logical budget overestimates, AFBC already makes the removed areas nearly free; judge by gpu_ext/cycles/CPU.

- **24/10 morning, zero-copy depth+color (q512) PASSES:** the cause of the stall was the UMA readback of the resolves, which materialized on the CPU side (fence wait 26.7 ms/frame) → materialization on the GPU side in stream order (wait 0). ABAB: cycles **−10.9%**, real bandwidth **−12.6%** (258 → 225 MB/frame), fps and CPU unchanged; BO in game 37-42 fps. Promoted as default in q516 (migration 2026-10-24-18), daily build after Dante. Remaining: half of the resolves still materialize on the GPU (inexact overlap / destination texture missing) → diag q515.

### 8d. Open defects (updated)
- **Replayer: corrupted textures**: CAUSE FOUND 22/10 (download offset never incremented + RAM reads above the GPU snapshot), writer fix in the source; a new trace is needed.
- **Dante dither pattern** (grid of 2 px columns; also visible on the sea at the piazzetta): cause UNKNOWN; MSRTSS cannot be tested from a savestate, a normal startup is needed.
- BO square artifacts at the bar: non-deterministic, probable cause weak (GPU lagging).

## 9. The tally (2026-10-22): what all the verdicts say together
1. **On the Mali GPU only memory traffic counts, not shader computation.** All the real GPU gains came from removing bytes: single-sample MSAA buffers q340 (cycles 31.2 → 22-23.7 M/frame, −27%; bandwidth 13.9 → 9.3 GB/s), clear chains q337 (−13%), depth resolve via compute (−7.5%), deferred depth transfers (−5.5%). Everything that adds passes or bandwidth collapses: in-pass resolve +28%, draw merging +10% (bandwidth +39%). Four rounds on the shader translator: about −3% real in total (q475 −2.6%, q476 −0.5% despite malioc −23%). ⇒ **stop work on shaders**; work on bytes per frame (260 MB/frame, 46 pass breaks).
2. **BO in the street is not limited by the GPU but by the CP.** GPUDIAG-q463: CP busy 100% of the interval (stall 0.0), GPU ratio 0.72-0,79, GPU with ~25% headroom; cost ~4.9-5.7 µs/draw × ~5.000 draws. The CP was discarded because "the GPU ceiling is 47-51 fps even with an infinite CP", but in the street we are at 38-43: reaching the ceiling is worth **+20-25% fps**. It is the largest lever available on BO and has never been attacked with a rewrite (cp-redesign: estimates only).
3. **Halo 3 is GPU-limited at 28.8 fps** (33.6 ms GPU out of 34.5; q448). The game furthest from 60 among those measured, and we have never looked at its GPU structure (formats, passes, bandwidth).
4. **The BO bench (replayer) is the bar scene** (2.261 draws, GPU-limited) **with corrupted textures**: it measures the pass bytes well, says nothing about the CP in the street.
5. Heat: a few degrees are worth ~8% fps (q423 35.5 °C vs 37.9 °C); the hidden thermal ceiling of the GPU halves the fps.

## 10. Update 24 October evening

**New tools (they exist, do not rewrite them)**
| Tool | Where | What it does |
|---|---|---|
| `xdtest.py` | test scripts folder | the ONLY entry for an in-game test from a savestate (BO/DANTE/REACH): blocks a build that is not the latest, missing APK, malformed cvars, profiler on a run not `-prof`, another runner active; automatic quotes on strings; waits for the build in progress; thermal gate; measures only the lines of the run (TAG/xe-run.log); standard row in TESTS.tsv + NIGHT.tsv with fps, **maxframe med/worst** (stability), GPU, CP/prim/vf, mprotect, preswap, reqcache |
| `TEST-RULES.md` | project root | 14 rules born from mistakes (quotes, accumulated xe.log, no profiler on checks, never rewrite a queue that has started, never force-stop blindly, exact file not `head -1`...) |
| `DIRECTION.md` | project root | the 3 tracks (CPU memory coherence, GPU/heat, Reach restoration) with the status of each lever |
| `reach-shot.py` | test dir | start from ISO with input, screenshot at N s (+20 s), xe-tail, no savestate |
| `queue-replay.sh` with `XD_REPLAY_GZ/NAME/SIZE` | test dir | replay of any trace (default BO 11840) |
| Reach trace | `analysis/xtr/reach-4D53085B_17156.xtr.gz` (173 MB raw) | Reach gameplay frame recorded with q537r (to be replayed: GPU track) |
| Recorder build | `q537r-ui26.apk` | XE_FORCE_TRACE_WRITER; cvars `GPU.trace_gpu_prefix='…'`, `GPU.trace_gpu_frame_request='…'` BETWEEN QUOTES |
| Decrypted Reach image | `research/reach-4D53085B/default.image.bin` (base 0x82000000, 0x1AC0000) + Ghidra project `proj/` (Xenon, -noanalysis) + `import.sh`/`decomp.sh` | targeted decompilation (DecompAt.java) |
| Tiling census | CpFrame `tiles: seldraws selchg ext` (with log_cp_frame_breakdown) | counts draws under SET_BIN_SELECT and SCREEN_EXT; diagnostic to remove once Reach has answered |
| `stale=` in the CpMem preswap | with log_cp_frame_breakdown | preswap miss for changed data (the others = collisions) |
| Profile per thread/function | `tools-pass/threadcpu.py`, `cpprof.py` | now wait for a NEW "RuntimeState: restored" line (xe.log accumulates); cpprof writes the error if simpleperf does not record |

**Mechanisms put into the code (q533-q544, on by default)**
- Fault block `shm_fault_invalidation_block_pages` 64 → **16** (PROMOTED, migration 2026-10-24-20; ABAB BO mprotect −30%, slow frames −2/−4 ms; Dante CP 6.3→5.5 ms, mprotect −52%).
- Preswap index cache **4-way, 8192 entries, LRU** (q541): BO misses 1700→400, prim 6→4.6 ms.
- `RangeEpochs` at **64 KiB** with summation for ≤16 regions (q539).
- 7e3 clamp on host render targets without FSI (`kSysFlag_ClampColor0..3To7e3`, q537): hardware fidelity, does NOT fix the Reach corruption.
- Savestate: result reported correctly right after retry (q536, + JNI `if (ok) result.clear()`); rollback retried for 15 s, without rollback only for a GPU barrier, otherwise it fails cleanly (q540); kernel object bodies written by the host: committed memory is enough (q538) or even just a guest heap, with a warning (q544); log of `NtFreeVirtualMemory` in the image heap 0x8xxxxxxx (q544); details of the rejected object in the message (q537/q542).

**Dead ends tonight (do not reopen without new data)**
- Fault block of **4** pages (fault 119/frame, worse).
- `shared_memory_request_cache` (hits 1313/frame but RequestRange 4.2 ms vs 3.3-3.8): stays off, to be removed.
- Unwatch score 1/max 32 alone (no gain).
- Encoder in parallel with fold (confirmed loser).
- Reach corruption: it is NOT 1b8a3f9c, nor the pass rewrites, nor the zero-copy, nor xenia-edge (e62ab581 already done differently by us), nor half precision, nor the 7e3 clamp alone. Remaining: transcodings (to be tested from the savestate).
- BO does not use predicated tiling (XDroid360's TILE-1 useless for BO).
- Throttling the spins of BO's job system (already closed on 7/10; 4.7 guest cores out of 5.66 are that).
- Web research: timeouts at 900 and 1800 s.

## 11. Update 25 October
Rule (25/10): **never redo work already done**. Every task starts with
"read `docs/LAB-NOTEBOOK.md` (§1-3, §6, §8b, §9-11) and search with `bash tools/dev/cerca.sh "word"`" and reports
a section "Already done and reused". This file remains the only register: no duplicates.

**Measurement (new since 25/10)**
- `xdtest.py` always turns on `log_cp_frame_breakdown`: inflated CP; performance verdicts with `XD_LEAN=1`.
- Alternating pairs of runs skewed by the heat from reinstalling: compare blocks of one build, or reverse the order.

**What worked**
- Reach savestate (q546-q570): DLLs loaded at runtime, heap reset only for the first module, delegated I/O, partial GPU pages copied only in the bytes written (q569), protection armed inside the lock (q570).
- Reach rainbow artifact = `vulkan_msaa_single_sample_storage` → Reach quirk in `game_quirks.cc` (q573): Reach 20.7 fps clean; the other games keep single-sample storage.
- CP levers promoted (q554/q555): dynamic constants, validated volatile preswap, per-range epoch, mprotect coalescing (+3-6% BO, at the edge of noise).

**Under measurement / open**
- q574 `Vulkan.vulkan_msaa_4x_as_2x2` (default false; 4x surfaces as 1x at double pitch, same EDRAM layout: this is NOT the
  §6 conclusion "make it look like 1x", which changed the layout). REACH72/73: clean image, ownership 1419 → 187 MB/frame,
  total 3062 → 1522, but fps 22 → 7-8 in 40 s, every CP phase ~2x, resolve copy 0.7 → 2-7 ms, core 1.8 instead of 3. To be understood; still open:
  the clears across 4x views (§6) as a suspect for the residual transfers.
  **Log analysis (25/10 evening, no phone): an accumulation in the 2x2 code is NOT proven.**
  - REACH72 before the ledger: **26.5 fps, exec 38 ms** versus REACH71 (q573, same ledger) 20.3 fps / 49 ms: the 2x2 lightens the GPU.
  - Then the CP grows 18 → 48 ms, but **all** phases grow by the same factor ~1.8 (xlat, pipe, state, vf, rec, which the 2x2 does not
    touch) and the untracked-time/ana ratio stays ~20 as in REACH71: the CPU runs slower, no data structure grows.
    Per-frame counts constant (transfers 62/frame, pipeline creates 0 after startup, resolves 21-33/s).
  - REACH73 was **swapping**: lmkd was killing apps at startup ("free is low"), emulator with 219 MB in swap, RSS 1.48 GB versus 2.28
    (REACH70), mprotect 0.88 → 3.32 ms with the same number of calls (mmap_lock contention). Run contaminated, not a verdict on the 2x2.
  - The 2x2 CP still costs more on a cool CPU (~+15-25%: FlushDeferredResolve + `kAllDraw` on ~110 4x draws/frame).
  - Hypotheses for the drop over time (to be measured): lowered CPU frequencies (FPSGO when GPU-bound, hidden Mali thermal cap). Tool: `xdtest.py` now writes `TAG/env.tsv` every 10 s (policy0/4/7 frequencies, SOC °C, Mali cap,
    MemAvailable, emulator swap) and puts in the line `env ... [GPU-CAP] [EMU-SWAP]`. Next test: REACH 2x2 LEAN cold
    with clean memory + default LEAN control.
  **VERDICT queue 1025-q (25/10 16:25-16:28, q574, LEAN, same scene from the savestate, SoC 80-98 °C in both):**
  REACH74 2x2 = **25.6-26.1 fps stable for ~55 s**, GPU 38.2 ms, core 3.02; REACH75 default = 19.4-20.3 fps, GPU 51.0 ms, core 3.03.
  **+30% fps, GPU -25%, same cores = -23% CPU per frame**, image identical to the default (no rainbow artifact, `resumed.png`).
  No drop over time: the REACH72/73 drop came from the CP breakdown being on + heat + swap. GPU memory the same (GL mtrack
  1.236 versus 1.254 GB). Before promoting it to default: BO and Dante with the 2x2 on.
  **PROMOTED TO DEFAULT (q575, 25/10 evening):** DANTE35-2x2 dither pattern 0/40 at 60 fps; BO76-2x2lean vs BO77-lean (same
  start 36.5 °C): median ~45 versus ~38.6 fps, final 38.8 versus 34,8, GPU 16.7 versus 17.6 ms, ~2 4x draws/frame rewritten,
  image identical. Migration `UPDATE_from_bool(vulkan_msaa_4x_as_2x2, 2026, 10, 25, 17, false)`; statistics line only with
  `log_guest_fps`. **To watch:** the overlay shows RAM +330-420 MB with the 2x2 (BO 2.06 versus 1.73 GB, Reach 2.05 versus 1.63);
  Reach GL mtrack the same → probably non-GPU memory or swap, to be measured with meminfo. Reach quirk storage=false left in place.
- BO default 30-31 fps (BO71 q573, BO72 q574) versus 36 of q567 and of the --live runs q571/q572: regression since q569 or measurement.
  BO74-lean (q574, battery start 37.9 °C, SoC 94-97 °C): 38-39 fps at the start, then drops to 26-32 during the run (draws ~5100,
  GPU 21 ms): the final triplet measures heat, not the code. Not a verdict (the protocol asks for a start ≤34 °C).
- **Dante dither pattern, new data (25/10 evening, offline):** the native guest capture DANTE31-cap frame 363 (1280×720, the image
  the presenter samples: `VulkanPresenter::CaptureGuestOutput`) does NOT have the 2 px guest peak (Nyquist/bands 0.9 horiz, 1.1 vert),
  while all the screen screenshots of DANTE30/31 have it (72-88× the floor) with period 3.77 px **on both axes**
  = 2 px guest (uniform scale 1,884, vertical crop at 90% from `present_safe_area_y`). BO72/BO71/REACH73: no peak. Dante presentation config:
  no FXAA/CAS/FSR, bilinear + `postprocess_dither`. A single frame can be among the ~20% clean ones →
  decisive test: `--capture` with `Vulkan.vulkan_guest_output_capture_sequence=40` (40 raw frames, `tools-pass/retinoseq.py`,
  xdtest writes `retino guest N/40`): N≈32 = rendering, 0 = presentation.
- **Dante dither pattern, VERDICT DANTE33-seq (q574, 25/10 16:32): it is in the guest RENDERING.** 36/40 raw presenter frames with
  Nyquist peak ~300× (horiz) / ~145× (vert); the first 4 (455-458) clean, then all. Signature: in every 2×2 guest block the pixel in
  top-left has the right brightness (35), the other three ~23 (-1/3). Dante uses 4x color surfaces (`RTCOPY msaa=2`, base 720):
  = 4x surface re-read sample by sample (sample 0 = top-left) with single-sample storage, same
  mechanism as the Reach rainbow artifact. The DANTE31 frame 363 capture was among the clean frames. Tests: queue `queue-1025-r.sh`
  (DANTE34-ssoff = real MSAA storage, DANTE35-2x2).
- **DANTE DITHER PATTERN SOLVED (queue 1025-r, q574, 25/10 16:35):** DANTE34-ssoff (`vulkan_msaa_single_sample_storage=false`) and
  DANTE35-2x2 (`vulkan_msaa_4x_as_2x2=true`): **0/40** guest frames with the dither pattern (DANTE33 default 36/40), screen 99.7 → 2.6
  (threshold 10), 60 fps locked, GPU 13.0 / 13.2 / 13.5 ms, core 2.50 / 2.52 / 2.52: zero cost. Cause = single-sample MSAA storage
  (ours, 10/10) on the 4x surfaces that Dante re-reads per sample, like the Reach rainbow artifact. The line below is superseded.
- Dante dither pattern: (historical) remains UNKNOWN. Summary: spectral peak at period 2 px guest (`retinovid.py`), ~80% of frames, follows the
  scene timeline, it was already there at q419 from a normal startup. Ruled out: alpha-to-mask (RET-a2moff 79%), single_sample_storage=false,
  depth_4x_as_1x, transcodes, skip_top_mip, resolve_to_texture, unwatch, shader cache, fast path (q428), f24ps (does not solve it).
  **MSRTSS (`vulkan_msaa_single_sample_resolve=true`) CANNOT be tested on this phone**: restoring with a different RT config now
  works (REACH44 q571), but on Mali < r52 MSRTSS does not turn on (it needs dynamic rendering, which is off for the 8×8 erratum q446/q447):
  forcing it gives a false measurement. Minimal test in its place: retinovid on
  q574, then `vulkan_msaa_single_sample_storage=false` + `native_2x_msaa=true` from the savestate.

**BO CP on q576 (25/10 17:15, BO80 DWARF call graph + `tools-pass/driverentry.py`, new)**
- **Historical CORRECTION:** the "f32→f16 conversion `stdlibp_neon_f32x16_to_f16x16`" (10/01, q557) does NOT exist: it is the only symbol
  exported nearby (0x8f47bc) and simpleperf attributes to it the internal code that follows. The sampled addresses fall inside
  `vkUpdateDescriptorSets` (0x92c2c4…) and other entry points. Attribute the driver per entry point with `research/mali-r49p1/entries.tsv`.
- Driver = 20% of CP, 17.3% from the replay (`DeferredCommandBuffer::ExecuteRange` in `EndSubmission`). Per entry point (% of CP):
  vkCmdDrawIndexed 6,6, vkUpdateDescriptorSets ~4,5, vkCmdBindPipeline 1,4, vkInvalidateMappedMemoryRanges 1,1,
  vkCmdBindDescriptorSets 0,9, vkCmdBeginRenderPass 0,7, vkCmdCopyBufferToImage 0,6, vkCmdDispatch 0.6.
- Inclusive: EndSubmission 22%, PrimitiveProcessor::Process 10%, DrawMemoryRange::Request 10%, EnableAccessCallbacks 8.9% (mprotect
  self 7.6%), UploadRanges 5.3%, GetPreswappedIndexBuffer 5.7%, RequestTextures 3%, memexport mirror 2.5%. No dominant entry.
- `cpprof.py` now always reports the CP thread and re-reads an existing .data (`CPPROF_REPORT_ONLY=1`).
- Secondary command buffers per pass: NOT the lever (q517: the driver encodes 2.2× slower on another thread on mt6899).

**Reach, pass sequence (REACH72 frame 235, 91 passes) and the "depth between color changes" lever (25/10 evening)**
- Pattern repeated 5 times/frame (passes 20-33): E (depth 1200x2192 fmt130) alone → E+B (color 1200x2192 fmt37) → J (depth
  800x768 fmt129, resolve closing) → E… E is loaded and saved twice per loop instead of once (~26 MB logical/loop, ~130 MB/frame).
- **Discarded as a big lever:** since 02/10 (b114, q141) AFBC is active on the 1x RTs, depth 130 included; the bytes removed would be
  already compressed (same lesson as q506). Only the 4x MSAA RTs were NOT compressed → this explains the 2x2 gain (4x → 1x with AFBC).
- Next: AFBC census on q576 (`GPU.log_image_compression=true`, queue `queue-1025-u.sh`: REACH77-afbc, BO81-afbc) to
  find what remains uncompressed (e.g. images with STORAGE_BIT).

- **25/10 18:10 enumerator savestate (q578-q582):** GTA IV keeps XAM enumerators alive (ContentEnumerator, XAchievementEnumerator). All types saved: payload = object header + type (0 raw elements → returns as XStaticUntypedEnumerator, 1 achievements with strings, 2 XMP playlists); XTitleEnumerator saved as already-formatted records. Validator in `snapshot/kernel_format.h`. q578 (static only) and q579 (+contents) were not enough, q580 superseded, not tested. Verdict: GTA-R1 restore OK (q582).
- **25/10 18:20 `vulkan_msaa_4x_clears_as_2x2` (q577) NEUTRAL:** REACH78 ledger ownership 187 → 167 MB/frame but total 1522 → 1589 (different frame), REACH79 lean 27 fps / GPU 36 ms versus 26 / 38 of REACH74 (noise), image clean. Stays false; candidate for pruning.
- **25/10 18:20 GTA IV pass log (GTA-L1, q582, road):** 1393 MB logical/frame (load/store 426, resolve 176, ownership 229, copy 135), 69 closures, 6828 draws, 28-29 fps with ledger. Host RT 1280x2048 (depth fmt 130 + color fmt 97 read as texture ~10 times in pass 1). Next: CPU profile (GTA-P1-prof) to know whether it limits GPU or the guest thread.
- **25/10 18:20 GTA IV/RAGE research:** no public analysis of frame 360, no xenia 2024-26 issue/PR on GTA IV performance; canary patch 545407F2 (Unlock FPS, Skip Legal, Disable blur, Aspect) all cosmetic or visual cuts → nothing to take. canary 9c30b16 (indexed tessellated strips → lists, Reach filmed faces) ALREADY present in ours (`primitive_processor.cc:838`).
- **25/10 18:55 compute dispatch per site (q583, GuestDispatch) and mprotect cost (q584, ShmProtectSplit):** the largest compute entry in all games = `DumpRenderTargets` (RT → EDRAM buffer for the non-direct resolves): BO 30.9k, Reach 54k, GTA 28.9k workgroups/frame; GTA also mat_depth 40k (2 depth materializations/frame), resolve_direct 22k. Next: why those resolves are not direct (reasons `LogResolveSiteOnce`). BO85: CP arm 86-88/frame at 24 µs (2.1 ms), unblocks on faults 47/frame at 36-41 µs (1.8 ms guest), CP 1-page probe 3.7-9 µs → half of it system cost in play, half guest mapping. Dynamic unwatch covers only ~95-105 pages versus ~750 invalidations/frame: find out why rewritten pages do not become candidates (texture_watched? gpu_written? multi-frame ring?) and widening it = eliminating arm+fault. Arm per caller to be measured (shared_memory.cc 1256 gpu-written, 1313/1352 upload, 1417 rewatch, 1674 WatchCPUReadRange, vulkan_shared_memory 661, readwatch resolve).
- **25/10 21:50 q585 `shm_unwatch_release_exclusion` (default false):** cause of the minimal coverage of the dynamic unwatch in BO = `texture_watched_pages_` set by WatchCPUReadRange (preswap/index conversion) and WatchMemoryRange (textures) and never cleared. Queue Z (BO87-90, GTA-R3): arm 90→30/frame, drop 48→25-30, cpu_read_uncached 8-10/frame, rewatches 18-25/frame, invalidations hash 23→50/frame; BO and GTA images clean. fps: off 41 (31 °C) / 35 (36.6 °C), on 37 (33 °C) / 41 (37.5 °C) → confused by heat.
- **25/10 21:50 q586 `vulkan_direct_host_resolve_2x2` (default false):** REACH81/GTA-S1 sites: all resolves outside the direct path for "color-format-or-msaa" or "owner-kind"; hypothesis = the 1x key of the 2x2 versus 4x resolves. Variants `resolve_host_color[_full][_uint]_*_4xmsaa[_scaled]_2x2_cs` (define `XE_RESOLVE_HOST_COLOR_SOURCE_2X2`, 2D fetch of the texel = EDRAM sample coordinate), MSAA index 3 in the tables, reasons now split color-format / color-msaa. Generator `tools/build/gen_android_spirv.py` updated (the 90 old variants regenerated identical).
- **25/10 23:00 q587 PROMOTED `shm_unwatch_release_exclusion` + `vulkan_direct_host_resolve_2x2` (default true, migration):** BO arm mprotect -67%, faults -45%, +5-8% fps over 3 pairs; direct 2x2 resolves compare the storage format (_AS_ = base): GTA EDRAM dump 11 → 1/frame, same bytes. Daily build = q587.
- **26/10 00:00 q588/q589 `vulkan_msaa_2x2_pixel_rate` PROMOTED (default true from q589):** 4x draws in 2x2 colored once per 2x2 block (= per guest pixel as on the 360) with dynamic shading rate; derivatives/LOD bias not doubled. GTA GPU 32.0 → 30.5 ms/frame, Reach/BO unchanged, images identical.
- **26/10 00:20 q590 `vulkan_resolve_texture_copy_covering` (default true) NEUTRAL but clean:** GTA-RC2 fmt-6 still 9 reloads/frame (3.5 ms): the real rejects are `no_texture` (67/200) and `not_full` (20/200), not the 320x184 coverage. GTA-C2 / REACH92 images clean. Stays (correct, zero cost).
- **26/10 00:05 GTA ping-pong ownership (GTA-X1, TransferSummary):** 5.8 ms/frame of transfers (17 passes) = surfaces D@0 / C@224 / C@184 jumping between the 1x (2x2) and 4x keys because draws with alpha-to-coverage were rejected by the 2x2 (`reject_incompatible`). Fix in q591: A2C emulated per host pixel inside the 2x2 (`vulkan_msaa_2x2_alpha_to_coverage`).
- **26/10 00:40 q591 `vulkan_msaa_2x2_alpha_to_coverage` PROMOTED (default true, daily build = q591):** A2C of 4x draws in 2x2 tested per host pixel in the shader (guest sample ((x&1)<<1)|(y&1), thresholds 0.75/0.25/0.5/1 − offset/16, dither from the guest pixel), coarse shading only for changes with nothing per-sample (`IsSamplesAsPixelsCoarseAllowed`: kEarlyHint/kNoAlphaToCoverage, no float24 depth/polygon offset/software clip); 2x2 draws without A2C go to kNoAlphaToCoverage. Msaa4xAs2x2 reject 0 over 38k draws; D@0 p8 and C@184 p8 1x↔4x ping-pong gone. GTA-A2 GPU 30.6 ms versus 32.8 (GTA-C2), REACH93 GPU 36.1 versus 38.5 (REACH92) and 27 fps versus 26, BO99 = BO97-V at equal heat; images clean (GTA hair, Reach foliage). Also included `vulkan_resolve_texture_copy_partial` (not_full 0).
- **26/10 00:48 `vulkan_msaa_4x_clears_as_2x2` → default in q592:** GTA-K1/K2 (q591 + cvar): C@224 p7 m0↔m2 also gone (they were the resolve clears), GPU 30.3 ms, image clean.
- **26/10 00:50 GTA remaining transfers (GTA-K1):** 62% format reinterpretations at the same base (D↔C, f0↔f3/f14), 31% 2x MSAA surfaces (m1, pitch 1280), 7% same format. Depth materializations 2/frame: 04DBF000 1280x1280 (depth resolve sampled as k_32_FLOAT fmt 36 → no target for the deferred resolve → normal resolve that materializes the old one first) and 03F20000 (smaller deferred resolve inside a larger pending one). GTA 049E8000 1280x720 fmt 6 reloaded about every frame (1625 loads, 3.5 ms/frame): diagnostics in q592.
- **26/10 01:05 q592 PROMOTED (clears_as_2x2 default + ResolveTextureReload diagnostics):** GTA-RT1 texture load 3.9 → 0.85 ms/frame (16.8 → 8.7 loads, thanks to the partial copy of q591); fmt-6 reloads 9 → 1/frame (049E8000 = legitimate format reinterpretation). REACH94/BO100 images clean.
- **26/10 01:50 q593-q595 `vulkan_resolve_full_overwrite_discard` PROMOTED (default true, daily build = q595):** a resolve that rewrites every byte of its aligned range (origin, the whole pitch, whole tile rows, extent = range in pages) discards the pending surfaces inside it instead of materializing them, even when recorded at the end of the pass (q595; q593 alone did not trigger). GTA mat_depth 2.0/40.0k → 1.0/14.4k workgroups/frame (the 04DBF000 1280x1280 materialization of 6.5 MB is gone), GTA-D2 32.5 fps (versus 30-31 at equal diagnostics), GTA-D3 GPU 29.0 ms; REACH96/BO102 images clean. 03F20000 remains (1280x256 deferred resolve inside a pending 1280x720 one).
- **26/10 08:45 GTA RT trace of one frame (GTA-TR1, new `xdtest.py --rttrace`):** GTA uses predicated tiling (3 strips 1280x256 2x MSAA, G-buffer D + 3 C, resolve with clear at the end of each strip), rebuilds the 1x depth by drawing an 8888 color at 0/16/0 and reusing it as D24FS8 (real C→D transfer, 1 + 8 stencil passes, Mali without stencil export), and the lights really use the stencil (mark/light pairs). Its depth clears are zctl 0x8777 rectangles with alpha test on but no pixel shader → rejected as "alpha test".
- **26/10 09:00 q596 `rt_cache_full_overwrite_alpha_without_pixel_shader` PROMOTED (default true):** without an effective pixel shader the alpha test/A2C cannot discard anything (they live in the PS), so the rectangle is a full clear. GTA-SA1: xfer pass 1280x2048 2.67 → 1.64 ms/frame (5 → 3.9 passes), stencil-bit draws 58 → 50/frame, GPU 28.0 ms; GTA/Reach/BO images clean (GTA shadows present).
- **26/10 09:15 q597 `rt_cache_transfer_known_constants` PROMOTED (default true, daily build = q597):** each RT remembers the EDRAM word left by a clear (resolve clear over whole tile rows, exact depth/stencil rectangle over the whole range, even depth-only or stencil-only) until a draw/transfer/clear/restore overwrites it; the constant is captured at the Transfer when it is decided (guest order), and the Vulkan execution does it as a clear of the destination (MakeResolveClearAttachment = same bit reinterpretation; 7e3↔8888 only for 0; no 64bpp). About 5 GTA transfers/frame become clears: C@720 8888→7e3 (1280x720) and the 1x→2x D of the G-buffer strip (data + stencil 0x81) are gone. D@1024/1536→C 2x remain because the game clears only half the range. GTA/Reach/BO images clean. Small GTA gain (thermal noise).
- **26/10 10:05 q599 `vulkan_stencil_transfer_copy` PROMOTED + q601 library fix (daily build = q601):** GTA-SC2 (q601): image clean (shadows, stencil lights), GPU 27.0 ms (q597 27.2-29), 31.5-32.9 fps. q601: the library scan crashed on every new ISO (XexModule::ReadImage read a null kernel_state from the metadata reader, regression q548) → NFS Most Wanted now in the library (8 games), NFS runs at 44-51 fps. q600 `debug_cp_skip_draws` (diagnostic) never tested: useless, the BO79 profile already shows guest JIT as the cost. Canary patches: GTA IV 'Unlock FPS' only removes the limit at 67 (GTA is not locked at 30: 31-33 fps), Reach has no fps patch (30-tick engine). Next: measure the dips while moving (savestate runs are with the scene still), not the average.
- **26/10 10:40 tests IN MOVEMENT (new `xdtest.py --move`: left stick forward + camera turning with xd-pad.dex, `log_frame_spikes_ms=45`):** GTA-MV1 34-38 fps on the road, 44 frames >45 ms in 90 s (burst of ~20 frames at 45-70 ms entering a heavy zone: 6300 draws, 24 fps, frame 42 ms versus GPU 30 → +12 ms on the CPU side); no pipeline spikes (0), very few textures, CP never waiting. REACH-MV1 fixed 26 fps, GPU 37-40 ms INDEPENDENT of draws (1700-3600): fixed GPU cost per frame, not geometry. AFBC census (REACH77-afbc/GTA-L1, left without a verdict): depth fmt 130/129 and RT colors 'driver default' = compressed; only ASTC uncompressed (obvious) → no lever there. REACH91 (q589) VkPassTime: passes ~21 ms out of 39 → ~18 ms/frame OUTSIDE the passes, never attributed (dump 5/34k wg, resolve_direct 31, memexport 119 dispatches, barriers). q602: `VkPassTime: gap <siti>` measures them (log_pass_sequence_every had only gap_before per pass, never summed).
- **26/10 11:05 q607 `vulkan_memexport_batch` PROMOTED (default true, daily build = q607):** q602 gap timing: Reach 9.1 ms/frame of GPU in the memexport gaps. q606 trace: 123 consecutive exports/frame of the same particle shader 838B50F94967ACAD (`loop i15` cycle, 16 vertices = 1 warp on 1 core, ~70 us each) reading/writing the same pool 03A43100+8C000. Ruled out: barriers (REACH-MB5 without barriers 8.0-8.5 ms → they are worth ~1 ms), per-interval barriers q603/q605 (the reads always overlap: removed). Solution: 'merged' variant of the memexport compute (constant blocks as an array per draw, gl_WorkGroupID.y = draw), batch dispatched at the first other work (SubmitBarriers). REACH-MB6: 120 draws in 10.7 dispatches/frame, memexport gap 9.1 → 1.9 ms, GPU 36.8 → 32.3 ms/frame; image clean (differences only at the edges = camera moved between runs). BO-MB3 clean (0 memexport in gameplay). GTA next: resolve_direct gap 5.2-5.6 ms/frame (16/frame).
- **26/10 14:20 early-Z q608/q609 (queue O) — incomplete lever, NOT promoted:** Mali counters recorded (GTA-Z0/REACH88 q587): the late ZS discards 99% of the quads it tests = GTA 41% of the shaded quads, Reach 29%, BO 32%. q608 `vulkan_early_tests_without_zs_writes` (draws with kill and no depth/stencil writes → early tests): only 10% of the late draws (EARLYZ of_which_early_no_zs_write 110k out of 1.08M). q609 `vulkan_alpha_tested_depth_prepass` (draws with kill that write depth → depth-only + color EQUAL early): **0 draws split** because the GTA G-buffer writes stencil ALWAYS+REPLACE (zctl 0x708767) and was excluded. Results: GTA-EZ1 27.4 ms (q607 27.0), REACH-EZ1 32.3 (=), BO-EZ1 18.4 (16,1, +3 °C start); GTA/BO images clean. GTA-EZ2-prof: counters at zero (120 s window, run ended earlier → `--secs 150`). Fix in q610 (stencil ALWAYS written by the pre-pass only, reject counters per reason), queue P.
- **26/10 14:20 GTA predicated tiling measured (GTA-TR1):** the G-buffer (~1030 draws) is re-executed identically in 3 strips (window offset 0/−256/−512, scissor 256/256/208): ~2000 draws/frame out of 6865 are repeats. "Untiling" project in docs/status/PLAN-60FPS.md (block 2-bis).
- **26/10 15:05 early-Z q610-q612 — VERDICT: no reliable gain, not promoted (daily build stays q607).** q610 (stencil ALWAYS ok) BO 122 draws/frame split, GTA 3; q611 (+A2C) GTA 357 split: GPU 27.8 ms (q607 27.0), BO 15.9/16.4 (=16.1), REACH-EZ5 28.7 then REACH-DS1 (q612) 31.1 → the −11% was noise. GTA counters q610: late ZS 135 M quads/s (q587 294 M) but GPU ms unchanged: the quads discarded late were not the bottleneck. Pre-pass turned off by default in q612 (green dots on a BO bush in BO-EZ5). `vulkan_same_depth_stencil_ops` (q612, idea from the Mali quirk list of hakuX/PPSSPP/ARMSX2): BO-DS1 still has the square artifacts (indeed many, + colored noise on the windows) → NOT the cause of the square artifacts. Queue R interrupted after 2 runs (phone unavailable).
- **26/10 15:05 hakuX studied (rfandango/hakuX, GPL-3, emulator of the FIRST Xbox, not 360):** its core is the guest RAM allocated as AHardwareBuffer and read by the GPU in place (zero copies) + a barrier at the first CPU write to pages the GPU reads + cache cleaning (DC CVAC). We did it on 2/10 (q167/q168): −400 MB but 39 fps versus 43 due to CP waits on faults (the GPU reads one frame later, the guest rewrites the pages) and black dots. To resume only with early command submission (not at end of frame). Path for the BO/GTA CPU (memcpy upload 19% of CP).
- **26/10 16:15 GTA predicated tiling: EXACT MECHANISM (GTA-TL1, q613 PM4 trace of one frame):** 3 phases of 5378 identical packets (same IB): bin_select 80000003 (strip 0 + bit 31 = extension pass), then C (strip 1), then 30 (strip 2). In pass 0 every draw is followed by SET_BIN_MASK 80000000 + EVENT_WRITE_EXT type 1A (SCREEN_EXT_RPT) at 1F95B4x9 (+0x10 per draw) + EVENT_WRITE 19 (SCREEN_EXT_INIT). Between the strips the guest (D3D) reads the rectangles and **rewrites in the command buffer** the SET_BIN_MASK in front of every draw (same packet 1F8CF48C: FFFFFFFF in pass 0, 8000003F in strip 1). We write fake full-screen extents (`ExecutePacketType3_EVENT_WRITE_EXT`, "Let us hope we can fake this") → mask 3F = all strips → ~1020 draws redone 3 times. Real extents: CPU (DrawExtentEstimator::EstimateVertexBounds runs the VS per vertex, only for small draws) too expensive on ~1000 large draws/frame; GPU impossible on Mali (no writes from the vertex shader) + mid-frame wait. Remaining path: untiling in the RT cache (pass 0 on an image as tall as the screen, strips 1-2 skipped, resolves served from the right rows). Tools: `pm4_trace_active_` with --rttrace (PM4T lines).
- **26/10 17:30 bindless (q614/q615, `vulkan_bindless_textures`) — IN PROGRESS, NOT promoted, has glitches:** an update-after-bind set with all views/samplers, 16-bit slots in the system constants. q614: GTA in gameplay 41-47 fps on the first run and 36-41 on the second (q607 31-33), BO 39-47 with gaps, Reach 24-29; flickers are visible in GTA and Reach is broken, then recovers (pipelines recompiled, but defects remain). q615 (binding limit 32→64 + warning) does NOT fix the defects; GTA-T1 q615 ~29 fps (to understand: heat?). Verified in code: the only load of system constants in UpdateBindings, sampler order equal to the bindings, external pipelines zero the guest layout (sets rebound). To look into: capture with a warm cache (queue T: XD_ACTION_DELAY=60), recycled slots, 3D/cube views. Daily build stays q607. Queue T (GTA/Reach/BO q615) launched, results in lab/test/queue-1026-t.log.
- **26/10 20:30 q616 — bindless definitive + early-Z cleanup (final round, queue F):**
  - **Bindless:** slot writes now go to the deferred stream (`CmdUpdateDescriptorSets`, like the per-draw sets with `vulkan_defer_descriptor_updates`). Before, they went on the CP thread while the "GPU Encode" thread recorded the binds of the same set (stream encode active).
    - The set is rebound after every new slot, so no driver keeps a copy from before the write.
    - Never-used slots are consumed before recycled ones; recycled slots go in FIFO order (it was LIFO before: the slot just freed came straight back into use).
    - GTA-BL2 profile (q614): GPU Commands thread at 0.42 cores and 37-39 fps → with bindless GTA is limited by the GPU, no longer by the CP.
  - **Turned off** `vulkan_early_tests_without_zs_writes` and `vulkan_same_depth_stencil_ops`, with migration 2026-10-26-18.
    - They had been on since q608/q612 without promotion and without gain.
    - With early tests, occlusion queries count pixels discarded by the kill as visible: a direct suspect for the flickers of lights and halos.
    - The alpha-test pre-pass (q609-q611) stays off; the code is to be pruned in a separate round.
  - **Phone config vs defaults:** every performance difference has its own migration, so at runtime the code defaults apply.
  - **Third GPU lever not applied:** the Reach resolves in the dump path are structural rejects (owner-kind = depth source read as color, converted formats, endianness). New shaders are needed, not verifiable in one round.
  - **xdtest:** the entry check waits for adb instead of crashing (that was why REACH-T1 and BO-T1 did not start).
- **26/10 21:15 VERDICT queue F (q616, warm cache, LEAN) → q617 final build:**
  - **Fps (median in gameplay):**
    - GTA-F1: 37.6 (35.9 p25) versus 27.8-30.6 of q587/q612.
    - GTA-F2 in movement: 40.6.
    - REACH-F1: 26.0 versus **29.5 of q607** (REACH-MB6); q614 also gave 26,8, so **a real −10%**.
    - BO-F1: 38.0 versus 39-41 of q609/q612.
  - **Images correct in all three:** GTA clean, Reach intact; the BO square artifacts are the known ones, present before too.
  - **Bindless:** wins where the limit is the CP (GTA); loses where it is the GPU (Reach: indexed fetches with the slot read from the constants = shader time). In BO it does not help.
  - **q617:** `vulkan_bindless_textures` turned off by default (migration 2026-10-26-21), turned on by the GTA IV quirk (545407F2).
    - Reach and BO go back exactly to the path and SPIR-V of q607, with the pipeline cache already warm.
    - GTA keeps the q616 path that was tested, with the cache warmed by F1/F2.
  - **Reach stalls:** at the end of the REACH-F1 run, ~1 s now and then. No pipeline compiling; the game's RENDER thread runs at the same lr (820FB040/826C5998) seen also in the stalls of q607/q614, emulator swap 280-540 MB. Pre-existing phenomenon, to investigate (memory/swap).
- **26/10 21:40 q617 DAILY build (installed and verified on the phone with the reference config):**
  - GTA-G1: quirk active, median 43.3 fps.
  - REACH-G1: without bindless, 25.8 fps, but recompiled 1927 pipelines, with SoC 94-98 °C and swap 674 MB. The cost of bindless on Reach remains uncertain (0/−8%) and per the rule (a lever that helps one game and hurts another → only on that game) it stays only on GTA.
  - BO-H1: without bindless, 36.5 with compilation of 2022 pipelines. Run done to warm the phone's cache.
  - BO square artifacts and colored dots present even without bindless: known problem, non-deterministic.
  - **Next:** GTA untiling (now GPU-limited), resolves, Reach stalls/swap, pruning of the dead alpha-test pre-pass.
- **27/10 q618 — build prepared without the phone (no measurement on the device; queue ready in `lab/test/queue-1027-final.sh`):**
  - **GPU overlap (`vulkan_pass_head_overlap`, default true).** Starting data, all GPUCTR runs: binQ+mainQ+compQ = active GPU (GTA 1,01, Reach 1.06), i.e. binning, fragments and compute always in series. Causes found in the code:
    - pass head (uploads and texture loads in front of the pass) enclosed between barriers from/to `ALL_COMMANDS` → now they only wait for vertex input, vertex shader, compute and transfer (`GetPassHeadSourceStages`); the fragment stage is entered only if a pixel shader still in flight has touched shared memory (vfetch in the PS, memexport PS, in-pass resolve: `fragment_shared_memory_submission_`). Same rule for the barrier of uploads issued at the start of submission, the texture load at the head and the barrier before the memexport;
    - shared memory: after a compute write (resolve, materializations) the vertices no longer wait; every draw passes the intervals it reads (vfetch + guest DMA indices) to `SettleVertexReads`, which places the barrier only if they overlap the written intervals (more than 64 intervals = everything). Pixel shaders do not wait for writes except for draws with vfetch in the PS (`SettleFragmentReads`); the sources drop the fragment stage if no PS in flight touches memory;
    - textures: resolve→texture copy, depth surfaces and sampling transitions reach only the fragment stage; the vertex shader that samples them adds FS→VS (dependency chain, `vertex_visible`).
    - Risk: one extra pass break if a draw in the middle of a pass reads vertex data written by a compute (render-to-vertex-buffer). Check: `Vulkan.vulkan_pass_head_overlap=false`.
  - **Adaptive shading (`adaptive_shading_rate`, default true):** GPU occupancy from per-submission timestamps (union of intervals, 1 s window); ≥0.92 → 2x1, below 0.72 → full, one step per window; still saturated at 2x1 and below 29 fps → 2x2. It combines with the thermal regulator (the coarser one wins). `AdaptiveShading:` line in the log. While the GPU is saturated the CPU waits as on a hot phone (`thermal_hot`: no per-frame spin for late frames in the CP and the scheduler, light threads on the slow cores). Switch in the Performance menu.
  - **Controller:** true analog triggers 0-255 (before: 0 or 255: GTA's gas/brake), vibration forwarded to the controller (XInputSetState → JNI `controller_rumble` → device VibratorManager, API 31+), `requestUnbufferedDispatch` also for joysticks, circular dead zone (before: per axis), Xbox button = opens/closes the menu, Share = screenshot, state read under lock. **Bug fixed:** PC mode (WASD+mouse) started on and discarded all controller input until the screen was touched; now it follows the device in use (WASD/mouse turn it on, controller turns it off, F8 by hand).
  - **UI translated (English and Italian):** game menu, settings, library, profiles, content, saves, control editor, patches, updates (about 630 strings, UI tests green).
  - Evaluated and NOT done: GTA untiling and GTA resolves drawn into textures (publishing in partial strips, too risky without images), Reach duplicate dumps (no data), JIT (lab to rebuild, historically crumbs), PGO (profile q573, regenerated only on the phone). BO: 37-38 fps with and without bindless in all runs q593-q617 at SoC 93-95 °C → limited by heat, not by the CP.
- **27/10 q619 — q618 refined offline (still without the phone; queue `lab/test/queue-1027-final.sh` now on q619):**
  - **Synchronization bug fixed.** The deferred resolve (`RecordDeferredResolve`), recorded while the pass is open and executed at the end of the pass, could reset the visibility debts of the main flow (`ClearVertexDebt` or `fragment_debt_` inside the queue). A draw in the same pass would have read vertices written by a compute without a barrier. Now `SaveReadDebt`/`RestoreReadDebt` around the recording: the queue ends with a full barrier, so its writes do not count as debt.
  - **CPU per draw.** `SettleVertexReads` runs only with an open debt (before, it built the array at every draw), skips draws outside the compute-write box, and does not queue the same resolve repeated every frame. Before, it could do up to 96×64 comparisons per draw.
  - **Adaptive shading without oscillations.** It goes up immediately and goes down only after 2 calm windows in a row. If the coarser level returns within 3 windows of a step down, the wait doubles up to 16 s, and it returns to 2 after 30 s stable. Typical case: Reach at the cap of 30, where 2x2 ↔ 2x1 would have pulsed every 2 s. In the log, `down_hold=` on the `AdaptiveShading:` line.
  - **Verified and already in place:** reading the timestamps adds no waits (same submission waited on as before); `thermal_hot` with GPU-bound touches only the guest spinners, not the CP; pass-head loads have the per-image WAR barrier from sampling (FS included); vibration stops with the menu open.
  - **Evaluated and NOT done: GTA merged resolve chain (plan 2a).** The GTA-TR1 trace shows chains of 2-3 resolves, with a draw or a clear in between and the same address written twice in the same strip (0x03F20000: there the WAW barrier is needed). Estimated gain ≤ ~1 ms, against the risk of corrupted G-buffer textures that cannot be seen. GTA's real lever remains the untiling (2-bis: ~2000 repeated draws/frame).
- **27/10 evening, q619 on the phone: REJECTED for now (black flicker in GTA).** Flickering and stuttering are visible while walking. FLK-A (q619 default, `--move`, 8 s video analyzed with `lab/test/flicker-analyze.py`): 25 whole BLACK frames alternating with the image (brightness 17 → 0 → 17) and 78 duplicated frames; fps 32.9-37.4, 34 frames over 45 ms, SoC up to 96 °C. Adaptive shading changes at most once/s (3 changes in the whole run, and an inconsistent measurement: gpu_busy=0.27 at 29.6 fps), so it does not explain 25 flashes in 8 s. Main suspect: `vulkan_pass_head_overlap`. Comparison to do: FLK-B (q619 with `Vulkan.vulkan_pass_head_overlap=false`) and FLK-R (q617), `lab/test/flicker-queue2.sh`. Not started: battery at 7% (the xdtest gate requires ≥25%). Daily stays q617; reference config put back identical (md5 b80ca5b9).
- **27/10 evening, q620 = q619 with `vulkan_pass_head_overlap` and `adaptive_shading_rate` turned off by default.** GPU path again identical to q617; controller, Italian UI and CPU fixes remain. Precedent for the same symptom: the bindless flickers in GTA (q614, fixed in q616). To verify with `lab/test/flicker-queue2.sh` (FLK-C q620 versus FLK-R q617, video in motion) as soon as the battery exceeds 25% (now 8%, slow charging from the Mac).
- **27/10 evening, verdict on GTA black flicker (video in motion, `flicker-analyze.py`, distinct frames):** q619 (FLK-A) 6 black + 12 dips; **q617 daily build (FLK-R) 29 black + 68 dips**; q620 (FLK-C) 0 black + 4 dips. So the black does NOT come from q618/q619: it is already in the q617 daily build. Suspect: GTA's bindless (quirk since q616/q617; same symptom as q614). Re-read of the bindless code: slot release and set flags (UPDATE_AFTER_BIND + PARTIALLY_BOUND + UPDATE_UNUSED_WHILE_PENDING) correct, every view that reaches bindless is released. Decisive test (FLK-D: q620 with `config/545407F2.config.toml` → `vulkan_bindless_textures=false`, the per-game file overrides the quirk) did not reach the game (launch timeout, battery at 9%): to redo.
- **27/10 20:15 FLK-D (q620 GTA, bindless off via the per-game file): 0 black frames and 0 dips** over 148 distinct frames, versus q617 with bindless 29 black and 68 dips; fps 26-30 (SoC 65-96 °C, EMU-SWAP). **Cause of the black flicker = GTA's bindless.** Read and corrected: descriptor layout (SHADER_READ_ONLY, same as the use), order RequestTextures → UpdateBindlessSlots, release of views and samplers, set flags. Bug not found. **q621: GTA's bindless quirk removed** (GTA goes back to per-draw sets); bindless stays in the code, off, until the defect is understood.
- **27/10 20:20 FLK-E (q621, no per-game file): 0 black frames, 0 dips** over 256 distinct frames; GTA 26.6-30.2 fps (SoC 78-98 °C). Black flicker resolved by removing GTA's bindless. Next: find the bindless defect (it brings GTA back to 37-45) with a targeted capture, then put it back.
- **27/10 20:30 daily build = q621** (best-apk.txt), reference config put back identical (md5 0e94687b).
- **29/10 00:18 bindless on q621 (global cvar, GTA in motion, video):** BL-1 bindless 0 black / 0 dips, 36-40 fps; BL-2 + `vulkan_stream_encode=false` 0/0, 28-31 fps (CP-limited); BL-3 + `vulkan_defer_descriptor_updates=false` 0/0, 37-38 fps. Without bindless (FLK-E) 26-30. The black was seen only on q617 (29) and q619 (6, overlapping barriers and adaptive shading on), never on q620/q621 (5 runs). Candidate change with no switch: since q618 the draws with cached textures pass the VS textures through `TransitionTextureForGuestShader` (before they skipped the transition). → q622 puts back GTA's bindless quirk; verification: q617 control + 3 q622 runs.
- **29/10 00:35 q622 check (GTA bindless put back):** R2-q617 control defective again (14 black, 20 dips); q622 V1/V2/V3 **0 black**, fps 32-37 (GPU-CAP 21, SoC up to 98 °C), but in V1 one frame without the character and the road lights (deferred lighting missing), then normal. Without bindless never seen. Ruled out: slot exhaustion (no "bindless array full" in the logs: slot reuse does not happen in these runs), sampler eviction (last use updated once per submission), RequestTextures/slot order, depth surfaces (their barriers return to the correct state). GPU counter: q619's overlapping barriers without any additional overlap (queue/gpu sum 1.054 versus 0.97 of GTA-BL2) → they stay off, closed.
- **q623:** the draws with cached textures (RequestTextures skipped) return to the sampled state every texture left in another use, not just those of the VS (q618). In q617 there was no transition: it is the difference that matches the drop of the blacks from 14-29 to 0. `EnsureTexturesSampled` log of the first 32 cases as proof.
- **29/10 01:00 REAL CAUSE of the black flicker: fatal GPU errors with bindless.** q623 W1-W3: the black returns (W1 21 blacks) and `EnsureTexturesSampled` never fires → transitions hypothesis refuted. In all runs the black coincides with `VulkanPresenter: Failed to create a swapchain (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)` after "swapchain outdated" (≥2 per run). The logcat (`lab/test/logcat-run.sh`) shows the chain: `mali: GROUP_ERROR_FATAL` (status ..d002c1) → `[MALI_ERROR_BARRIER_DEBUG] cpu queue starts with error` → `[MTK_SUPPRESS_DEVICE_LOST] recreating vkqueue`. **LC-1 GTA with bindless: 1067 fatal errors and 1144 queues recreated in one minute (from the restore onward); LC-2 GTA without bindless: 0; LC-3 Reach: 0.** GROUP_ERROR_FATAL = invalid GPU access (07/10: robust OFF + invalid buffers/descriptors). The MediaTek driver hides the device lost; in-flight submissions are lost (black frames, swapchain to recreate, stalls of ~5 s). First hypothesis: never-written slots of the PARTIALLY_BOUND arrays read by the shaders (slot 0 for stages not ready, samplers in overflow, empty 3D/cube arrays). → q624: all slots filled at startup with valid null views and samplers.
- **29/10 01:35 bisection of the bindless GPU errors (GTA, logcat, q624 with pre-filled slots):** LC-4 997 fatal errors/min; with `vulkan_defer_descriptor_updates=false` 746, `vulkan_stream_encode=false` 953, `vulkan_memexport_batch=false` 724, `vulkan_lazy_constant_rows=false` 795 → no mechanism turns them off; never-written slots were not the cause. Status 0x..c1 = Mali CSF TRANSLATION_FAULT exception (unmapped GPU address). Also ruled out: driver limits (500000 descriptors), layout, view type (2D_ARRAY/3D/CUBE consistent with the shader), system constants (full range), memexport compute (binds the set). → q625: when a view or a sampler is destroyed, its slot returns to the null view/sampler (before, the address of the freed memory remained).
- **29/10 01:50 q625 (null slots on destruction): LC-5 606 errors/min** → not that either. New hypothesis consistent with TRANSLATION_FAULT: a slot index of one array (2D, up to 16384) used on a smaller array (3D/cube 1024) = read beyond the descriptor table. It happens if CPU and shaders disagree on the texture list: the lists are taken from the FIRST successful translation of a shader (assumed: they do not depend on the change). → q626: in the shader the index is masked to the size of its own array (powers of 2) and a `Bindless: shader ... modification ...` log flags the modifications whose lists differ from the first.
- **29/10 02:05 q626 (masked index + different-lists log): LC-6 844 errors/min, no modification with different lists** → indices and order ruled out. → q627 (experiment): bindless arrays 4096/256/256/1024 instead of 16384/1024/1024/4096, to see whether it is the table size.
- **29/10 02:20 q627 (arrays 4096/256/256/1024): LC-7 737 errors/min** → size is not the issue; arrays back to 16384/1024/1024/4096. → q628: diagnostic cvar `vulkan_bindless_write_idle` (set written only while the GPU is idle), to test whether the driver mishandles set updates while the GPU uses it.
- **29/10 02:30 q628 with `vulkan_bindless_write_idle`: LC-8 732 errors/min** → set updates while the GPU works are not involved. → q629: slot 0 reserved for the null view/sampler; diagnostic cvar `vulkan_bindless_debug_null` (1 VS texture, 2 PS texture, 4 sampler → null slot) to find the stage that reads unmapped memory.
- **29/10 02:50 q629 diagnostics:** debug_null 1 (VS) 703, 2 (PS) 630, 4 (sampler) 757, **7 (all null slots) 1045 errors/min** → the errors do not depend on the content nor on the descriptor indices: just using the bindless table is enough. Hypothesis: prefetch of the descriptor table beyond the end of the allocation (unmapped page). → q630: 256 null padding descriptors appended to every binding (layout and pool), the shaders index only the real part.
- **29/10 03:10 q630 (256 padding nulls): LC-9 974/min** → not the prefetch beyond the end. Device features already enabled (dynamic indexing, UAB, partially bound, unused-while-pending). → q631: bindless index/descriptors decorated NonUniform (SPV_EXT_descriptor_indexing, feature shaderSampledImageArrayNonUniformIndexing), cvar `vulkan_bindless_nonuniform` (default true): hypothesis of the 'uniform' path of the Mali compiler.
- **29/10 03:35 q631 NonUniform: LC-10 960/min** → not the compiler's uniform path either. Null images clean at the first submission (even with bindless). Layout and binds for contiguous ranges correct; with bindless the pixel set (index 3) is declared in the layout but never bound. → q632: empty set always bound at slot 3 (hypothesis: the driver preloads the tables of all sets in the layout, even unbound ones).
- **29/10 03:55 q632 (empty set in place of the pixel set): LC-11 867/min** → not that either. **GTA bindless: cause of the GPU errors not found** after 11 hypotheses ruled out (never-written slots, slots of destroyed views, indices out of the array, order of the lists per modification, table size, prefetch beyond the end, updates while the GPU is in flight, NonUniform, content (all null: 1045), unbound pixel set, CP mechanisms: defer/stream encoder/memexport batch/lazy rows). The errors occur ONLY with bindless (LC-2 GTA without: 0; Reach LC-3: 0; BO LC-12: 0, 60 fps in the scene). → q633: GTA's bindless quirk removed again; the robustness measures (pre-filled nulls, null slots on destruction, empty set, diagnostic cvars) stay, inert with bindless off.
- **29/10 04:20 CAUSE of the bindless GPU errors FOUND.** q634, GTA: bindless turned on by the **global cvar** (no quirk) → LC-14 **0 errors** (32-35 fps), LC-13 (+ constant index) 0. The same bindless turned on by the **GTA quirk** → 600-1000 errors/min (LC-1..LC-11). The translator read the cvar at every creation (`CreateTranslator` → `Features`), the command processor fixes it once in `InitializeBindless`; the quirk is a game config value, cleared and reapplied on title load/restore, and the main translator is created in `pipeline_cache_->Initialize()` BEFORE `InitializeBindless`. Shaders translated for the other layout with the bindless set bound → descriptors read where the set does not have them → TRANSLATION_FAULT, whatever the content in the slots (explains all the negative tests). → q635: `InitializeBindless` before the pipeline cache; each translator takes `features.bindless_textures` from the command processor; GTA's bindless quirk put back.
- **29/10 04:35 CORRECTION of the previous entry:** with the global cvar (xdtest `Vulkan.vulkan_bindless_textures=true`) bindless was NOT activated (the "Vulkan: bindless guest textures" line is missing from the logs of BL-1..3, LC-13, LC-14: only the quirk turns it on). So BL-1..3 and LC-13/14 were WITHOUT bindless (0 errors, 32-40 fps), and q635 with the quirk still has 1110 errors/min. Conclusions: (1) bindless on this Mali always causes GPU errors, cause not found; (2) the "+25-35%" of bindless was largely thermal noise: GTA without bindless does 32-40 fps on a less hot phone, 26-28 at SoC 86-97 °C. → q636: quirk removed for good; the order fix (bindless fixed before the pipeline cache, translators following the command processor) and the robustness measures remain, inert.
- **29/10 05:05 daily build = q636** (GTA without bindless, 0 GPU errors in LC-16, 33-40 fps; Reach image clean but 15 fps with GPU at 65 ms/frame: Mali thermal cap 21 active for hours even at 43-51 °C, plus EMU-SWAP; BO 37-38 fps with the historical square artifacts). All of last night's fps measurements are under the cap: comparable only with each other.
- **29/10 05:10 final check of daily build q636, GTA in motion with video:** FIN-1 0 black / 0 dips, 41.8-42.1 fps; FIN-2 0/0, 33-39 fps; 0 swapchain recreations (before, 2-5 per run with the black). Mali cap 21 remained even at SoC 35 °C after 25 min idle: probably the MIUI policy while charging, not the temperature.
