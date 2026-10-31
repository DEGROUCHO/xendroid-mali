# 60 fps plan (26/10)

Goal: Reach, GTA IV and BO locked at 60 fps, i.e. ≤16.6 ms per frame for both GPU and CPU (CP thread and guest threads), even while moving.
We work one block at a time, starting from the most expensive, and each block is finished completely before testing (no tests of half levers).

## q618 (27/10, prepared without the phone, to be tested with lab/test/queue-1027-final.sh)
- GPU in series: in the counters binning + fragments + compute = GPU active (GTA 1,01, Reach 1.06). q618 narrows the barriers (head pass, shared memory by ranges, textures only at fragment, memexport) so that they overlap: first check = GPUCTR with sum/GPU below 1.0.
- Adaptive shading 2x1 only with a saturated GPU (2x2 below 29 fps), CPU idle while the GPU is the bottleneck.
- BO is limited by heat (37-38 fps with or without bindless, SoC 93-95 °C): the lever is the energy per frame, not the CP.
- After the verdict: GTA untiling remains the biggest lever for GTA.

## Final build q617 (26/10 evening)
- GTA IV: bindless (quirk) → 37-45 fps standing still, ~41 while moving (q607 31-33); now GPU-limited (27 ms: G-buffer 3 times for predicated tiling, resolve, shadows).
- Reach and BO: q607 path (bindless cost 10% in Reach and did not help in BO).
- Next levers, in order: GTA untiling (block 2-bis), merged/direct resolves (block 2), Reach stalls with the emulator swap.

## Where we are (today's measurements, q607)

| Game | fps | GPU ms/frame | Bottleneck | Notes |
|---|---|---|---|---|
| Reach | 30 | 32.3 | GPU | main pass 14.5 ms (43 passes); outside the passes ~13 ms |
| GTA IV | 32 (34-38 while moving) | 27.0 | GPU when still, CPU while moving | G-buffer 8.0 ms; shadows 7.3 ms; resolve 7.5 ms |
| BO | 40 | 16.1 | CPU (CP ~22 ms) | GPU already at 60 |

Mali counters (GTA q587, Reach q587):
- Fragments 48-55% of GPU time, compute 28-40%, binning 18%.
- The **late ZS discards 99%** of the quads it tests: GTA 41% of the shaded quads, Reach 29%, BO 32%.
- No overlap between the queues: the GPU does one thing at a time.

## Blocks in order of yield

### 1. Hidden pixels shaded for nothing (early-Z): CLOSED with no gain (q608-q612)
Verdict 26/10 15:05: the quads discarded by late ZS drop (GTA 294 → 135 M/s) but GPU ms and fps do not change in any game; pre-pass off. The bottleneck was not there.
- q608: draws with kill / alpha test **without** depth/stencil writes → early fragment tests (identical by construction).
- q609: opaque draws with kill that **write** depth → two draws.
  1. Depth only, color mask 0: only the computation that decides the kill remains.
  2. Color with depth EQUAL, early tests, no write.
- Expected: late ZS kills drop to almost zero; fragments −20/−40% on games with foliage (mostly GTA).
- Verdict on three things: GPU ms, counters (GTA-EZ2-prof versus GTA-Z0-prof) and images.

### 2. Resolves without the round trip (GTA 7.5 ms, Reach ~6 ms)
**Cause.** Every resolve runs in series:
1. close the pass;
2. barrier;
3. compute writing the guest memory (tiled) and a linear scratch;
4. barrier;
5. copy scratch → texture image;
6. barrier;
7. reopen the pass.

GTA does the G-buffer at 2x in three strips (256 + 256 + 208 rows, predicated tiling). Each strip has 4-5 consecutive resolves with no draw in between: 16 per frame at 0.34 ms, for only 1.3 MB each, so they are bubbles and not bandwidth.

**Plan, in a single build:**
- **2a. Merged resolve chain** (like the particles in q607): consecutive resolves with no draw in between accumulate and launch together with the first other work. The barriers become one before and one after, the dispatches run one after another without flushing, the copies into the textures end up in a single transfer block.
- **2b. Resolve written directly into the texture image** (storage image, formats 8888/16161616/32): no scratch, no copy. The guest memory is written only if something reads it from the CPU or as a non-texture, reusing the on-demand materialization that already exists.
- **2c. Reach: the resolves that today go down the dump path** (resolve+dump 2.6 ms, plus the texture loads that follow them, ~3 ms) move to the direct path. First the reason for the rejection (`log_resolve_details`) must be read within the same block test, not in a separate run.

**Expected:** GTA −4/−5 ms, Reach −3/−5 ms.

### 2-bis. GTA: predicated tiling drawn only once ("untiling")
**Measurement (GTA-TR1).** The G-buffer, 2x with 4 targets, has about 1030 draws and is repeated identically for 3 strips: scissor 256/256/208 rows, window offset 0/−256/−512, same sequences 717+8+159+… draws. Therefore:
- about 2000 draws per frame out of 6865 are repetitions (CP CPU −30% on GTA, and it is the CPU that causes the drops while moving);
- G-buffer vertices and binning done 3 times;
- 3 sets of load/store and 16 small resolves instead of 4 large ones.

**Plan.**
1. Recognize the strip sequence: bin select change, window offset that drops by one strip, the strip's scissor, the same PM4 packets re-executed.
2. Draw only the first repetition, with full-screen scissor, on host virtual render targets as tall as the screen (outside the EDRAM tile model).
3. Skip the following repetitions.
4. Serve each strip's resolves from the correct rows of the full image.

Risks: clears and resolves at the end of a strip, state that changes between strips, reconstructed depth. It is the biggest project for GTA (CPU and GPU), to start after block 2.

### 3. Pass breaks (Reach 43 main passes and 58 gaps between passes, GTA 41)
**Cost:** "no-dispatch gaps" 2.4 ms on Reach and 1.35 ms on GTA, plus depth+color load/store at every break.

**Known cause (log 24/10):** the depth is saved and reloaded only because the render pass closes all attachments when the color changes.

**Plan:** pass graph at the end of submission (already in PASS-REWRITE-PROJECT.md, steps B1-B3), with the depth staying open across color changes. After block 2, because those resolves are half of the breaks.

### 4. BO CPU (CP ~22 ms) and GTA CPU drops while moving
**BO.** Profile q557:
- deferred command buffer replay 21%;
- Mali driver 20%;
- memcpy 19%;
- PrimitiveProcessor 10%;
- mprotect 8%.

The parallel encoder exists and was rejected: it must not be reopened without understanding why it lost.

First step: profile the memcpy (who copies the 19% of the CP) and the driver (which vkCmd are expensive). From there, one big lever only.

**GTA while moving.** Spikes of +12 ms when entering new zones, with texture loads but without pipeline compilation. To be examined after block 2, with `--move`.

### 4-bis. Guest RAM read in place (the core of hakuX)
- Guest RAM is allocated as an AHardwareBuffer, imported into Vulkan and read by the GPU without upload. It removes memcpy, RequestRange and upload, and about 400 MB of RAM.
- On 2/10 it lost 10%: the guest rewrote pages that the GPU was still reading, because we submit commands at the end of the frame.
- It must be redone with early command submission (mid-frame submission) and a first-write barrier that waits only when the GPU is actually reading, as in hakuX, plus cache clean-up for the black dots.

### 5. Afterwards: re-measure the main pass
If after 1-3 the main pass remains the bottleneck, the counters (fragments versus bandwidth) are reviewed before deciding anything on the shaders. Four rounds on the translator have already given only −3% real.

## Plan rules
- Every block: complete code → one build → a queue of 3-4 runs (images and GPU ms, counters if needed) → verdict in the log and here → daily build if it wins.
- The reference measurements are today's q607 runs; no repeated control runs.
