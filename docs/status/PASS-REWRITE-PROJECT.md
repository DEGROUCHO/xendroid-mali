# Render pass layer rewrite for tile-based GPUs (Mali)

Status: plan, 23/10/2026. References: journal `session-2026-10-13-audit/AUDIT-2026-10-16.md` (22-23/10), `LAB-NOTEBOOK.md` §8-9.

## 1. Why
Black Ops, replay frame (q486, logical bytes before AFBC compression; the chip measures ~260 MB real):

| Item | Logical MB/frame |
|---|---|
| Render pass, loads | 197 (of which **174 re-entries**: images saved and immediately reloaded) |
| Render pass, stores | 197 |
| Resolve (compute RT → shared memory) | 99 |
| Texture reload from resolves (compute) | 52 |
| Ownership transfers | 33 |

Mali counters (average over 64 s): GPU time = fragments 64% + binning 11% + **compute 29%**, sum ~102% ⇒ **the queues never overlap**. 32% of the shaded quads are discarded by the late depth test; 99% of the quads are "non-opaque" (no Forward Pixel Kill).

46 passes/frame. Causes of the closures: resolve mid-pass 17 (+3 re-entries on the same framebuffer), RT changes/transfers ~23, pending barriers (mostly those of the resolves), swaps.

Patches inside Xenia's structure have yielded only crumbs (ANGLE-style ops ~0, direct resolve <30 MB, precise barriers 2-3 breaks): every patch hits the next limit of the same structure.

## 2. The structural cause
Xenia is designed for desktop GPUs (immediate rendering, huge bandwidth): it decides when to open/close passes **while** it receives the commands, one at a time, without knowing what comes next; every resolve immediately runs a compute toward the copy of guest RAM (512 MB), and the texture is reloaded from there with another compute. On a tiler, every closure = saving and reloading the entire tile memory, and every compute in between = serialized GPU.

## 3. What we are building: two new blocks
### Block A — Surface cache (first)
A surface = a live host render target (key: EDRAM base, pitch, format, samples). A resolve **does nothing**: it records "the memory [dest, dest+len) = view of surface S in rectangle R with conversion C" (guest format, endian, exp_bias, MSAA sampling). When a texture is requested from that memory:
- if conversion C can be expressed as a view/swizzle/direct sampling → the texture directly uses the image of S (zero bytes copied);
- otherwise → a draw RT→image of the texture in the right format (a single write, AFBC intact), done **outside the pass** and only when really needed.
Guest memory is materialized (today's compute, delayed) only if someone reads it in another way: CPU, memexport, incompatible texture, trace/savestate, readback. Copy-on-write if S is about to be rewritten while a texture is using it.
Expected effect: removes much of the 99 + 52 MB and above all the **compute (29% of GPU time)**; resolves stop closing passes.
Prerequisite already done: step 1 depth (q485-q486), which however only accepts dest k_24_8_FLOAT while BO resolves the depth as **raw k_8_8_8_8, endian 8in32** → first task: exact conversion of that format.

### Block B — Frame planner (after)
The deferred command buffer already records everything; the planner moves the decisions to the end of the submission:
1. Each pass becomes a node with: attachments, first/last write, reads (texture, transfer, resolve), external work preceding it (uploads, texture loads, compute).
2. **Merge**: consecutive passes on the same framebuffer separated only by work that does not touch their attachments → a single pass, the external work moved before it.
3. **Ops**: load/store decided knowing the future (DONT_CARE if the next access is a full overwrite, CLEAR in place of full-screen clear-quads).
4. **Compute kept off critical points**: texture loads and materializations grouped at the head of the submission or between passes already separated for other reasons.
Block B makes useful what does not trigger today (ANGLE-style ops, lookahead) because it knows what happens next.

## 4. Working rules
- New, clean code, separate modules (`surface_cache.{h,cc}`, `frame_planner.{h,cc}`), behind cvars default off until it wins.
- Every step buildable and measured on the replay (byte budget `log_pass_bytes`, Mali counters, Mcycles/frame) + BO and Dante for the image.
- No visual compromise; fallback to the current path for every case not proven safe.
- Every verdict in the journal and in the REGISTRO.

## 5. Steps
| # | What | Success measure |
|---|---|---|
| A1 | BO depth resolves (raw k_8_8_8_8, 8in32) served from the surface | counters: resolves served >0, materializations ~0; budget −40 MB logical; compute ↓ |
| A2 | Color resolve (scene 2x, fmt5/exp_bias, RGBA8) | resolve+copies −80 MB logical; compute ≤15% of GPU time |
| A3 | Resolves that no longer close passes | breaks from resolves 17 → ~0 |
| B1 | Pass graph at the end of the submission (read-only, log) | the graph reproduces the current 46 passes |
| B2 | Pass merge + moving external work | passes/frame 46 → ≤20; re-entries 174 → <50 MB |
| B3 | Ops decided with the future | avoidable stores/loads measured by the budget |

## 6. In parallel (small, already queued)
Early-Z on BO: 32% of the quads are wasted. Diagnostics `log_early_z_reasons` (q488) + `vulkan_alpha_test_no_sample_mask`.

## 7. Status at 24/10 night
Measurements on the BO replay (diag q498, logical bytes frame 3): closures **48 → 40**, bytes **579 → 539 (−7%)**. ABAB chains inside the pass: cycles −1.5%, bandwidth −1.4%.

| Done | What | Cvar (default off until ABAB wins) | Status |
|---|---|---|---|
| A1 | Zero-copy depth: materializations removed (Use without interval, resolve readback) | `vulkan_texture_from_render_target` | q499+, to be measured |
| B-step | Clear chains: transfers inside the open pass, sources prepared at the start of the chain | `rt_cache_clear_chain_in_pass` | measured −1.5% |
| B-step | Depth clears retargeted to the head of the game's pass (+ transfers around them) | `rt_cache_defer_retarget_clears` | q503, to be measured |
| B3 | Clear folded into the head as loadOp CLEAR (now possible) | `vulkan_fold_clears_to_load_op` | q503, to be measured |
| B-step | Texture loads at the head of the pass: read-only pages do not block; upload at the head does not count as a write; mips via blit | `vulkan_pass_head_loads_ignore_reads`, `vulkan_pass_head_mip_blits` | q503, to be measured |

Remaining re-entries (q498, 205 MB/18 breaks): textures mid-scene 63 MB/3, shadow map clears 42/1, resolves (inherent: the next draw reads the result) ~52/9, other.

**Next step (after the q503 measurement): lazy constant-value rows.** The game's clear goes through a 4x MSAA view and covers ~2048 rows of the shadow map (base 0x4c0) while only 1024 are drawn (scene: 608 of 2528; shadows pitch 560: 512). `vulkan_render_area_shrink` shrinks the area to the draws, but a full-image clear brings it back to full → the store saves all rows. Plan: the clear writes only the rows of the pass area; for the rest it records (RT, rows, value). On-demand materialization: inside a pass (ClearAttachments before the first draw that touches rows in that zone), outside the pass for resolves/transfers/views/dump. Estimated gain ~12 MB logical/frame on the shadow map alone.

Promotion: `session-2026-10-13-audit/promote-q503.py` (default + UPDATE_from_bool, also turns off `log_relaxed_attachment_ops`), to be used only after ABAB + BO/Dante image check.

**Ceiling (q498 budget):** of the 204 MB logical stores/frame, 83.9 MB are only reloaded (by merging passes: −168 MB, 31% of the frame), 62.4 MB have no consumer (depth rows outside the drawn area, saved by passes with no draw or with a full-image clear), 36.9 go to resolves and 20.8 to texture reads (inherent).

## 8. CPU front in game (24/10 morning) — preparation, no code until the profile arrives
- BO in game is limited by the command processor (CP): ~5000 draws/frame, GPU ~17 ms, 32-37 fps.
- Only per-phase data left (replay q463, 2293 draws): CP 11-13 ms/frame, draw 8.3 ms = **3.6 µs/draw**. Phases ms/frame:
  binding 1.40 · primitive 1.30 · texture 0.93 · vertex fetch 0.90 · state 0.52 · recording 0.48 · translations/sampler 0.47 · analysis 0.44 · RT 0.43 · pipeline 0.33 · submission 0.23 · memexport 0.17. The rest (~4 ms) is PM4 (Type0/Type3), swap and copies.
- Time is spread over many phases: micro-optimizations yield crumbs. Already in the code: dynamic UBOs, descriptor writes deferred to the encoder, event-based texture invalidation, fast-path memo.
- Remaining structural lever: **two-stage CP** (option C). The CP keeps only what must observe guest memory at the right moment (PM4, primitives/indices, vertex fetch, texture requests, resolves, fences). Binding, state, pipeline and translations move to the encoder (today at 9%) via a log of register writes into a shadow register file, without copying snapshots per draw. Movable share estimated from q463: ~3 ms out of 8.3 (~35% of the work per draw).
- **Condition to start:** the in-game profile of queue 13 (CpFrame phases + CPU per thread) must confirm that the CP is at 100% and the encoder has headroom. If the movable share in game is <25% of the CP, it is not done.
