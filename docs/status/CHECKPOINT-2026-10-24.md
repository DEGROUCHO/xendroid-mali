# Status check — 24 October 2026, afternoon

## Where we are (measured numbers)
| | Start 23/10 (q492) | Today (q516/q517) |
|---|---|---|
| Pass breaks per frame (BO replay) | 48 | 34 |
| Logical bytes per frame | 579 MB | ~361 MB |
| **Real bandwidth per frame** (Mali counters) | ~258 MB | **~225 MB (−12.6%)** |
| **GPU cycles per frame** | ~21.2 M | **~18.9 M (−10.9%)** |
| Total CPU (replay) | 1.17 cores | 0.91 cores (−22%) |
| BO in game | 32-37 fps | 37-42 fps (GPU 16 ms, CPU-limited) |
| Dante in game | ~112 fps | ~112 fps |

Mali counters with zero-copy, per frame: reads 130 MB (textures 68, buffers read by shaders 53, tiler 12), writes 93 MB. Textures are already AFBC-compressed.

## FINDING from this afternoon: what really slows BO down in game
Measured in game (BO, 5395 draws/frame): the command processor spends **half of its time** in two phases the replay did not show:
- **vertex fetch 6.5-7.3 ms/frame** and **indices 6.0-6.4 ms/frame** (3 times the per-draw cost of the replay);
- inside: **mprotect 346 times per frame = 7.3 ms** (21 µs each: it is the "Windows-style page protection" compatibility layer you suspected), 207 memory uploads per frame, **5276 index buffers converted by the CPU per frame** (preswap).
The replay uses almost static memory, which is why replay gains did not move BO in game. Under test: backoff of rewritten pages (q520/q521), then index swap on the GPU.

## What limits it now
- **BO in game is limited by the command processor (CPU)**, not by the GPU: ~5000 draws/frame, ~3.6 µs/draw on the critical thread. The GPU could sustain ~60 fps (16 ms).
- Found today: since q511 the Vulkan encoding had moved back onto the critical thread (folding the clears switched off the parallel encoder). **q517** puts it back in parallel — under measurement.

## What worked (in the code, active by default for all games)
1. Pass rewrite set (q511): clear chains inside the pass, depth clear in the game's pass, folding into loadOp CLEAR, textures loaded at the head of the pass, transfer scissors, lazy constant rows → passes 48→34, CPU −22%.
2. Zero-copy depth+color (q516): resolve drawn directly into the texture, without going through memory → bandwidth −12.6%, cycles −10.9%. The stall that was slowing it (27 ms/frame wait) was removed in q512.
3. Mip tail fix (q488): bandwidth −7%.
4. q517: parallel encoder also with clear folding (under measurement).

## Failed or neutral (do NOT reopen without new data)
Resolve inside the render pass (bandwidth +17%), MDI draw merge (bandwidth +39% on Mali), CP pinned to one core (−7%), guest thread priority/sleep (−18%), pixel shader translator (0%: shaders are not the limit), store lookahead and ANGLE-style ops on BO (0 effects), reuse of the CP across frames, precise shared-memory barriers (2-3 avoidable breaks out of 20).
- Lesson: removed "logical" bytes do not become real bandwidth if Mali was already compressing them (AFBC, Transaction Elimination). The real bandwidth lies in uncompressed buffers (resolves to memory, texture reads).
- Lesson from today: promoting one feature can silently switch off another → the promotion script now lists these cases.

## Next big steps (in order)
1. **BO critical thread**: q517 (parallel encoder) → then an in-game profile to decide on the "two-stage command processor" (moving state derivation to a second thread; estimated ~30% of the work per draw).
2. **Residual zero-copy**: half of the resolves are still written to memory (inexact overlaps, missing destination texture) + ~4 resolves/frame in formats not covered (diagnosis q515/q518 queued).
3. **"load/store" reads 53 MB/frame**: find out what generates them (vertex fetch from shared memory, constants, compute).

## News from the internet (24/10)
- **Xendroid-Plus** (XenDroid fork with our same structure): ~400 commits since August. All 114 relevant ones were checked against our source: 32 integrated right away (GPU fixes for mip/3D/resolve, A64 JIT, kernel), the lightweight thunk for kernel calls ported by hand (−56 memory accesses per call), texture-load deduplication ported. Many others we already had in our own, broader form (vector constant pool, compare+branch, truncations, viewport/primitive cache). Build **q519**.
- **Mali and Vulkan CPU side**: on the G720 (CSF) secondary command buffers are native and Arm no longer advises against using them → a route for having the encoder encode passes *while* they are being recorded. On r49p1 descriptor buffers and the graphics pipeline library are missing; we already use dynamic UBOs (Arm's advice no. 1).
- **Mali driver**: no driver update is available; PanVK-kbase (open driver for G720) is ~7% slower with artifacts and often falls back to the stock driver → not worth it.
- xenia-edge: work on interface/XAM/GPU correctness, nothing on the CPU cost per draw. Original XenDroid stalled since August.

## Verdict q517 (parallel encoder) — replay
Command processor CPU −1 ms/frame but it waits 2.2 ms for the encoder at the end of the submission and draws slow down: margin 4.5 → 3.4 ms (**~1 ms worse**). Probable cause: the encoder receives each pass only when it closes (the main pass has ~1500 draws). In-game verdict on BO pending; structural fix = secondary command buffer per pass.

## 14:50 — Why mprotect is so costly in game (new)
- Microbenchmark on the phone: an isolated mprotect costs **3 µs**; in BO in game **16-19 µs**. With other threads doing mmap/munmap in parallel it rises to **50-107 µs**: it is **contention on the kernel memory lock** (mmap_lock) with other threads of the process.
- The kernel does not allow profiling its own code from the shell → **q523**: internal sampler (`log_syscall_sampler_us`) that shows how long each thread stays blocked in mprotect/mmap/munmap/madvise/page fault.
- **q524**: when the game changes a page's protection and asks for the previous one, Xenia on Linux was re-reading all of /proc/self/maps (hundreds of µs with the lock held) and, with active watching, returned "read-only" to the game. It now takes it from its own page table (faster and correct, default on), with a counter of how often it happened.
- In queue 19: BO with the sampler, BO with mprotect merging (`shm_coalesce_watch_mprotect`, never tried in game), Dante, ABAB q516 vs q524.
- The "hook" libraries in the APK (adrenotools) are only needed for Adreno drivers: on Mali they cost nothing.

---
## Evening status (19:00)
**Done**
- Savestates, 3 generic bugs fixed (all games): restore during a load now possible (q534); restore succeeded but reported as failed (q536); detailed message about the object blocking the cold restore (q537). Reach save with q537: OK.
- Reach, corruption: commit Xendroid-Plus 1b8a3f9c cleared (q531 without it is corrupted just the same). Found that on Mali (no FSI) the HDR buffer 7e3 was written without the hardware limits → clamp like the 360 (q537), fixed but **does not** remove the noise. Under test: transcodes off, pass rewrites off.
- BO, CPU: invalidation block on faults 16 pages instead of 64 → 38-40 fps against 32-35 in two controls (mprotect 242 → 162/frame). To be isolated and confirmed on BO+Dante (queue 21), then default for all.
- BO does not use predicated tiling (XDroid360's TILE-1 is useless for BO).
- Reach: decrypted executable (28 MB) imported into Ghidra Xenon; recorder build q537r ready to record a GPU frame of Reach and replay it in a loop (repeatable method for the 88 ms of GPU).

**News from the web**
- xenia-canary: only XAM/kernel fixes. XDroid360 0.2.0 (Adreno): pass splice, tile merge, direct resolves, resolve→texture (≈ our zero-copy). Their lesson = ours: fewer passes for the same work does not reduce the GPU.
- Reach compatibility issue #981 (PC): texture shading fixed in xenia-edge in May 2026; occlusion queries important (lens flare).
- Existing static recompilations: Skate 3 (native renderer) and GTA IV (preliminary): useful as function maps, not as code.

**Queued on the phone**: Reach restore + tex/off screenshot → Reach trace (recording + replay) → queue 21 (fault block 16/4 BO+Dante + BO CPU profile) → queues 19, 17, 18.

---
## Night status (20:15, stop)
Promoted the invalidation block at 16 (fewer mprotect and fewer drops, BO and Dante). In code: 4-way index cache (miss −75%). Request cache and block 4 rejected. Reach restore: freed region found, bug hypothesis in NtFreeVirtualMemory/Release (q544 verifies it). Reach trace recorded, to be replayed. New: DIRECTION.md, TEST-RULES.md, xdtest.py. Everything stopped; next chain ready (`chain-next.sh`).
