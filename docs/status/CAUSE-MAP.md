# Cause map — XenDroid on POCO X7 Pro (Mali-G720 MC7, Dimensity 8400)

One single document with the cause of every problem. Rules:
- Every cause has a status: **PROVEN** (an intervention removes it and puts it back), **PROBABLE** (consistent clues, proof missing), **UNKNOWN**.
- A cause changes only with a new measurement, written here with date and run.
- The performance bottleneck is always decided **with the same criterion** (below), never by feel.

## Fixed criterion for performance
GPU time per frame (union of the GPU work intervals measured with timestamps, the overlay's "GPU time") divided by the frame time (1000/fps):
- **≥ 0.9 → GPU limit.** More fps only by removing work from the GPU.
- **< 0.9 → CPU limit.** The per-thread profile says which thread is saturated (≈1.0 core).
- **Heat** = total CPU cores + GPU load. It must be measured separately: a game can be GPU-limited and still heat up because of the CPU.

Why the culprit seemed to keep changing: the bottleneck has **really moved**. At the end of September it was the GPU Commands thread (CPU); we optimized it and now BO is GPU-limited. I was not stating it with a fixed criterion, and it looked like ping-pong.

## Performance and heat
| Game | FPS limit | Status | Measurement |
|---|---|---|---|
| Black Ops at the bar | **GPU** (20.8 ms GPU out of 21.3 ms frame ≈ 98%) | PROBABLE | q375/q419, 47-50 fps; replayer ~21-22 Mcycles/frame |
| Black Ops in the street | **CP** (CP busy 100% of the interval, stall 0; GPU ratio 0.72-0.79) | PROVEN (GPUDIAG-q463, scorecard q459-q461) | 37-43 fps, ~5.000 draws, 4.9-5.7 µs/draw; GPU ceiling 47-51 fps |
| Black Ops heat | **Guest CPU**: 5 threads ~0.8 cores each, mostly active waiting (yield ~75-116k/s) | PROVEN (per-thread profile q419) | 5.4-5.6 cores |
| Dante | to be measured with the criterion | UNKNOWN | 95-117 fps |

## Graphics bugs
| Bug | Cause | Status | Evidence |
|---|---|---|---|
| Dante: 8×8 black blocks (applies to all games on Mali < r52) | **Dynamic rendering in the Mali r49p1 driver** (Arm erratum 3786926; ANGLE disables it on ARM < r52 for "various bugs"). `vkCmdBeginRendering` has an internal cache keyed by a SHA-1 of the attachments and compares only the hash (driver RE, verified). It is NOT the renderArea (noshrink 41%) and NOT the destruction of the views (q445 keep-views on: 44-54%). | **PROVEN** (classic render passes: norp2 3%, q446 6%+6% versus 40-56% in 20+ runs, same fps). Fix q446: classic render passes on Mali < r52 (default for everyone). Residual 6%: hypothesis DONT_CARE replayed in the classic path (fix q447, under test) or clear-only passes (PPSSPP) | 2026-10-17: q446-1 6% 88-99 fps, q446-2 6%, q445-noshrink 41%, q445 44/54% |
| BO: square artifacts on textures | Maybe the same family (GPU lagging) | weakly PROBABLE | drain at every fence: clean scene (1 run); memexport wait only: squares present |
| Dante: dither pattern (grid of 2 px columns) | Not alpha-to-mask (off: 79% as always) | **UNKNOWN** | RET-a2moff 2026-10-17. MSRTSS cannot be proven from a savestate (GPU restore fails if the render target path changes): a normal boot up to the scene is needed |
| Halo 3 white screen | Guest state not restored after the transfer cutouts outside passes | PROVEN (fixed q416) | 4 runs |
