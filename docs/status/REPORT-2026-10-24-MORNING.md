# Night report 23→24 October

## In short
Tonight I rewrote the parts of Xenia designed for desktop GPUs that break the frame on Mali. On a Black Ops frame (replay, byte budget):

| | Start of night (q492) | q498 | **q503** |
|---|---|---|---|
| Pass breaks per frame | 48 | 40 | **34** |
| Logical MB per frame | 579 | 539 | **487 (−16%)** |
| Re-entries (save and reload the same image) | — | 205 MB | **68 MB (−67%)** |
| Texture loads moved to the head of the pass | — | — | **294 of 300** |

**q506 (whole set, diag at 02:23):** logical MB **432 (−25% from the start)**, load/store **115 MB (−44%)**, depth rows never reread 62 → 10 MB, transfers back to 33 MB.

Measured on the phone with ABAB: clear chains inside the pass = **−1.5% GPU cycles, −1.4% bandwidth**; mip tail fix = **−7% bandwidth**.

**ABAB q506 on the phone (02:23-02:50), default vs whole set:** GPU cycles **−2.7%** (noisy), real bandwidth **unchanged** (261 → 262 MB/frame), **CPU −22%** (1.17 → 0.91 cores). Honest lesson: the −25% of "logical" bytes does not become real bandwidth, because Mali's AFBC compression already made the removed areas (clears, empty rows) almost free. The real gain of these pieces is the CPU (fewer passes/barriers/transfers) and a few % of GPU.

**Where the real bandwidth is:** in uncompressed buffers — resolves that write guest memory and texture loads that read it back. This is the zero-copy route (resolve → texture without going through memory): ABAB of the depth zero-copy queued tonight; if it pays off, the same follows for color.

## What I rewrote (all behind cvars, off until ABAB confirms)
1. **Clear chains inside the pass** (`rt_cache_clear_chain_in_pass`): ownership transfers go into the already-open pass instead of opening a separate one; the sources are prepared at the start of the chain.
2. **Depth clear in the game's pass** (`rt_cache_defer_retarget_clears`): the "retargeted" clear goes to the head of the next draw's pass; consecutive rectangles (strip + remainder) are merged as the old mechanism did (in q503 the failure to merge had inflated transfers 33 → 103 MB: fixed in q504).
3. **Folding into loadOp CLEAR** (`vulkan_fold_clears_to_load_op`): now triggers because the clear is the first command of the game's pass → the depth is not reloaded.
4. **Texture loads at the head of the pass** (`vulkan_pass_head_loads_ignore_reads`, `vulkan_pass_head_mip_blits`): only pages written in the pass count, not those read; upload at the head does not count as a write; mips generated with blit too.
5. **Transfer scissor** (`vulkan_transfer_scissor_bounds`): transfers draw only within their rectangles, and the stencil clear is counted → the pass area shrinks (it used to stay full: 1040x2528, 560x8192).
6. **Lazy constant rows** (`vulkan_lazy_constant_rows`): the depth clear covers the whole EDRAM zone (2048 rows on the shadow map) but only 1024 are drawn: rows outside the area remain "pending clear value" and are actually written only if someone reads them.
7. **Zero-copy depth**: two causes of materialization removed at 100% (usage declaration without interval, resolve readback); one remains (different resolves to the same address) → still outside the set.

## What remains (from the q503 budget)
- Resolves: ~55 MB of re-entries over 10 breaks. Largely inherent: the next draw reads the resolve result right away.
- Depth rows never reread: 62 MB in q503, attacked by transfer scissor + lazy rows (q504-q506).

## Real in-game (03:40, q506 with everything on)
- **Black Ops:** 32-37 fps with ~5050 draws per frame, GPU 16.7 ms → the GPU could sustain ~59 fps: **in game BO is limited by the CPU (command processor)**, not the GPU. Image clean.
- **Dante:** 112 fps, GPU 8.3 ms. Blocks in 6% of frames (previous runs 4-5%: normal). The black dither pattern on the sea/walls is the known defect, not new.
- Lazy rows (ABAB): cycles −4% (noisy).
- Queued: BO and Dante with defaults in the same session, for direct FPS comparison.

## Locked 60 fps in game
I cannot say yet: the BO replay already runs at ~58-60 fps; the real in-game test (BO + Dante) with everything on is queued right after the ABAB. I promote to the daily build only what wins ABAB and image check (script ready: `session-2026-10-13-audit/promote-q503.py`, to be extended to scissor and lazy rows if they pass).

## New tools
`lab/test/tools-pass/`: `passreport.py` (full report of one frame from the budget), `abab.py` (ABAB verdict), `sym.sh` (symbols per build).

## Update 07:30
**Promoted in the code (q511, build ready):** clear chains inside the pass, depth clear in the game's pass, folding into loadOp CLEAR, texture loads at the head of the pass (reads + mip blit), transfer scissor, lazy constant rows — all active by default with config migration. Interface identical to the daily q471-nxe (same files and same UI code): direct replacement. **It becomes the daily build only after the ongoing verification** (ABAB q492 vs q511 + BO/Dante), on hold because the phone is at 45 °C with 43% battery.

**Zero-copy (resolve → texture without memory):** it is the first lever that lowers **real bandwidth (−15/−16% per frame)** and cycles (−6%). However, until now it slowed everything down (replay 23 fps, BO 18 fps). Found the cause with the command processor wait statistics: 25-27 ms per frame of GPU wait, due to the resolve readback forcing the end of every submission. Fixed in **q512** (queued after the verification). If it holds, it is the next real leap.

**Real BO in game:** it is CPU-limited (5000 draws/frame, GPU ~17 ms); the set gave CPU −22% in the replay but +1-2 fps in game (within noise). For locked 60 fps in BO the CPU cost per draw must come down: this is the next front.

**Dante:** 112 fps; blocks 4-6% as before (no worsening); the dither pattern is the known defect.

**09:50 — q511 installed as the daily build** (on request). Partial verification before stopping for battery: compared with the start of the night −4.5% GPU cycles, −4% bandwidth, −22% CPU.

**11:00 — zero-copy q512: the stall is gone.** Command processor GPU wait 26.7 → **0 ms/frame**; frame 17.2 ms (~58 fps, the replay is at the limit of 60) instead of the 23 fps of q510. No materialization on the CPU side (0), 122/s on the GPU side. Two causes remain for which half of the resolves are still written to memory (a resolve touching an address already published without covering it exactly; a classic resolve when the destination texture does not exist): finer diagnosis in q513. ABAB and BO in game in progress.

**11:30 — zero-copy promoted.** ABAB q512: GPU cycles **−10.9%**, real bandwidth **−12.6%** (258 → 225 MB/frame), same fps; BO in game 37-42 fps. Image: the square artifacts on the wall are there even with defaults (known defect). Active by default in the code from **q516** (with config migration).

**11:45 — found a brake on the critical thread in game.** Promoting the clear folding (q511), the "stream encoder" switched itself off silently: all the Vulkan encoding of the frame went back onto the command processor thread, which in BO is exactly the bottleneck. Fixed in **q517**: the encoder works in parallel again (it only holds back a clear-only pass until the next pass decides), identical output.

**Queued (queue 16, starts when the phone is reconnected):** command processor CPU time q516 vs q517, BO in-game profile on q517, ABAB, diagnosis of the residual zero-copy, Dante. If q517 holds it becomes the daily build (it contains everything: pass set + zero-copy + parallel encoder).
