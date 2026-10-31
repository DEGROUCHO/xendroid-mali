# Goals scoreboard: session 2026-10-07 night
**Goals (minimum):** RAM −500 MB, CPU −15%, GPU −15%, all global (valid for every game).

**Reference:** Black Ops q293, 240 s, same diag:
- RUN35: PSS 2070 MB, fps 26,3, cores 4,54, GPU 0.846 Gcyc/s, bandwidth 14.5 GB/s;
- RUN36: PSS 2135 MB, fps 21,1, cores 4,46, GPU 0.70 Gcyc/s, bandwidth 11.0 GB/s.

Per frame, with the two q293 runs:
- CPU about 170-211 ms-core/frame;
- GPU about 32-33 Mcyc/frame;
- bandwidth about 520-550 MB/frame;
- 46 render passes broken up per frame.

**Warning:** BO scenes vary between runs (draws/frame from 2300 to 4000), so the fps are not comparable. For CPU and GPU, what counts are the per-frame measurements and the broken-up render passes, which are deterministic.

| Build | PSS BO (MB) | Δ RAM | RP broken up/frame | Bandwidth MB/frame | ms-core CPU/frame | Notes |
|---|---|---|---|---|---|---|
| q293 | 2070-2135 | 0 | ~46 | 520-550 | 170-211 | reference |
| q294 | 2031 (meminfo) | **−40/−104** | ~46 | 536 | 219 | textures 280→168 MB, purge useless |
| q295 GTA | 1670 (meminfo) / 1630 (harness), versus q293 GTA 1911/1866 | **−241** (but different scene: 1266 draws/frame versus 5119) | – | 1440 | 170 | GL mtrack 1152 versus 1329 |
| q295 | 1981 (meminfo) / 1934 (harness) | **−90/−200** | ~46 | 670 (different scene) | 198 | disassembled −15 MB, "verified" hoist does not trigger (the hash is missing) |

**Distance from the goals (q295)**
- **RAM:** about 300-400 MB missing.
- **GPU:** 0%. The target is the 20 pass breaks per frame from uploads; RUN39 fixes the hash and the blocks.
- **CPU:** 0%. Next step: a fresh profile.
