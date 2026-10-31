# Web research — 01/10/2026 evening

## Topic 1

**The priority is to eliminate work per draw.** From the measured data: 6400 × 2.7 µs ≈ **17.3 ms/frame on the command processor**; for 60 fps, 16.7 ms are needed for the entire critical path.
**V = verified in the sources; H = hypothesis for XenDroid.** The estimates below are exploratory, not additive, and do not automatically equal fps gains.

1. **Descriptor cache, deduplication and bulk allocations — highest priority.**
   **V:** [PPSSPP](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/Vulkan/VulkanRenderManager.cpp) deduplicates consecutive payloads and allocates 16 sets together; [Dolphin](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Vulkan/StateTracker.cpp) updates via dirty flags.
   **US/H:** avoid identical rewrites and allocations on the path of the 6400 draws. **Estimate:** 0–5 ms CPU/frame; 0.3–0.8 µs eliminated per draw would amount to 1.9–5.1 ms.
   **Cost/risk:** medium; invalidation of resources and recycling only after GPU completion. [ARM 2025, §4.9](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695) recommends preallocated sets and dynamic offsets.

2. **Descriptor buffers: a concrete 2026 development from Eden.**
   **V:** [Eden PR #4251, merged 5 August 2026](https://git.eden-emu.dev/eden-emu/eden/pulls/4251) introduces block rings, payload reuse and binding only when it changes; [DXVK 2.7](https://github.com/doitsujin/dxvk/releases/tag/v2.7) documents a reduction in CPU overhead, with regressions on some drivers.
   **US/H:** possible alternative to update/allocate per draw. **Estimate:** 0–4 ms CPU/frame if descriptors dominate; requires `VK_EXT_descriptor_buffer` and `bufferDeviceAddress`.
   **Cost/risk:** high. [G720 r53 Linux, 2025](https://gist.github.com/nyanmisaka/ce03b5f61cb02389a97094b5172f5bf0) exposes the extension: it does not prove that it is available on the Android 1.3.278 driver in use.

3. **Batching of compatible draws / multi-draw.**
   **V:** [ARM 2025, §3.2 and §10.14](https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695) recommends batching and documents MDI to reduce CPU overhead; [RPCS3](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/VK/vkutils/device.cpp) enables `VK_EXT_multi_draw` when available.
   **US/H:** group consecutive draws with compatible state and resources. **Estimate:** 0–3 ms CPU/frame; depends on the share of MW2 that can actually be aggregated.
   **Cost/risk:** very high: guest registers, constants, queries and memory dependencies can prevent batching; the order must be preserved.

4. **Asynchronous recording/submission: distinguish the two operations.**
   **V:** [Eden](https://github.com/eden-emulator/mirror/blob/master/src/video_core/renderer_vulkan/vk_scheduler.cpp) records via a worker; [Dolphin](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Vulkan/CommandBufferManager.cpp) can move the submission; [Strato/Skyline](https://github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/command_scheduler.h) uses command pools per thread.
   **US/H:** overlapping translation and Vulkan calls could shorten the critical path. **Estimate:** 0–2 ms/frame, with possible regression due to guest contention and the thermal limit.
   **Cost/risk:** high; few large blocks, separate pools, precise synchronization. The [Khronos sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html) measures +15%, but with 8 threads available: not transferable to your case.

5. **Native push descriptors: useful only with adequate support and limits.**
   **V:** [Strato](https://github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/trait_manager.cpp) enables them conditionally; the [G720 r53 Linux](https://gist.github.com/nyanmisaka/ce03b5f61cb02389a97094b5172f5bf0) declares `maxPushDescriptors=32`.
   **US/H:** avoid allocations for small, highly variable layouts. **Estimate:** 0–1.5 ms CPU/frame; already-cached sets may turn out better.
   **Cost/risk:** medium. [Winlator compat_layer](https://github.com/leegao/compat_layer) emulates missing push descriptors: it is a compatibility measure, with no CPU advantage demonstrated for us.

6. **Selective dynamic state and driver-version workarounds.**
   **V:** [Strato](https://github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/trait_manager.cpp) disables dynamic vertex bindings on ARM before r42 and flags SPIR-V access-chain problems; [Eden](https://github.com/eden-emulator/mirror/blob/master/docs/user/Settings.md) documents Mali problems with EDS3.
   **US/H:** reduce pipeline variants by using only verified states. **Estimate:** 0–1 ms CPU/frame, in addition to smaller compilation spikes.
   **Cost/risk:** medium; artifacts and device loss. The Vulkan version **does not identify** the rXX driver revision.

7. **Persistent pipeline cache and compilation outside gameplay.**
   **V:** [PS3Native, community Android fork, September 2026](https://github.com/thecrixux/PS3Native/releases) documents a shared Vulkan cache and multiple workers during loading, then reduced.
   **US/H:** useful for frame-time stability; does not eliminate the ordinary cost of the 6400 draws. **Estimate:** 0–1 ms average with a warm cache, potentially larger benefits on spikes.
   **Cost/risk:** medium; cache tied to driver/UUID and concurrent workers. [Original RPCS3-Android](https://github.com/RPCS3-Android/rpcs3-android) has been archived since 9 April 2025.

8. **Preserve AFBC by avoiding unnecessary mutable images.**
   **V:** [Strato/Skyline](https://github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/trait_manager.cpp) considers `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` expensive on ARM because it can disable AFBC.
   **US/H:** relevant with expanded RGBA8 textures and thermal/bandwidth pressure. **Exploratory estimate:** 0–10% of GPU time, ≈0 on CPU cost per draw; only if compression is currently lost.
   **Cost/risk:** medium; aliasing and format reinterpretation. AFBC does not add hardware BCn support.

9. **Alternative G720 Android driver: an experimental beta exists, not a solution validated for us.**
   **V:** [PanVK G720 0.1.0-beta.2, 24 September 2026](https://github.com/wonderkast02/panvk-g720-kbase-csf/releases/tag/0.1.0-beta.2) uses stock Kbase/CSF; the project declares tests on MT6899/G720 **MC8**, API 35+, without Vulkan conformance.
   **US/H:** MC7, native Android presentation and XenDroid/MW2 remain unproven; the advertised tests include X11. **Gain:** not estimable without a relevant benchmark.
   **Cost/risk:** very high: loader/WSI integration, GPU crashes and regressions. [Mesa upstream](https://docs.mesa3d.org/drivers/panfrost.html) lists G720, but this does not certify compatibility with Kbase Android.

**Limit of the research:** for Citron and Sudachi no primary change specific to G720/G925 with a measured gain per draw was found. No source found demonstrates stable native 60 or 120 without artifacts on XenDroid/MW2.
## Topic 2

As of **1 October 2026**, no public patch with controlled proof of **MW2 at stable native 60 on Mali** was found. Order below: probable impact on this case; **V = verified in the source/documentation**, **H = hypothesis for US**. The upstream measurements are reported by their authors.

1. **Command processor: eliminate repeated work per draw — highest priority.**
   **V:** XenDroid contains descriptor/sampler caches, suppression of identical fetches and PM4 writes for ranges: `vulkan_cache_texture_descriptors`, `vulkan_cache_sampler_parameters`, `vulkan_skip_redundant_fetch_constant_writes`, `vulkan_fast_register_ranges`. [Source](https://raw.githubusercontent.com/rfandango/XenDroid/main/emulator-core/src/main/cpp/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc).
   **US/H:** 6400×2.7 µs ≈17.3 ms/frame: priority comparison with the fork in use. **Conditional estimate:** saving 0.2–0.5 µs/draw is worth 1.3–3.2 ms of CP work; no phone A/B test of MW2 found. **Cost/risk:** medium; incorrect invalidations produce stale textures.

2. **A64 JIT: more registers, immediate constants and fewer VMX operations.**
   **V:** Canary documents `02d2cb5` (v8–v15 allocatable), `39706a0` (constants without scratch memory), `f2a90ce` (`mov+dup→movi`); XenDroid adds `0feaeed`, hardware denormal flush when supported. [Canary](https://github.com/xenia-canary/xenia-canary/releases/tag/395219c_canary_experimental), [XenDroid commit](https://github.com/rfandango/XenDroid/commit/0feaeed).
   **US/H:** with 79% of guest time in the JIT, relevance is high if they are missing. **Scenario estimate:** −10% of JIT time means −7.9% of guest time, not automatically +7.9% fps. **Cost/risk:** medium-high; FPCR, precision and ABI.

3. **Intermediate submissions: documented phone gain, uncertain transferability.**
   **V:** `vulkan_mid_frame_submission_draws=1300` is associated with **Forza 20→30 fps on Odin3/Adreno830**. [Upstream report](https://github.com/rfandango/XenDroid/blob/main/GAME_COMPAT.md).
   **US/H:** can overlap GPU and command construction; does not reduce CP cycles. **Estimate:** not quantifiable without measuring GPU bubbles. **Cost/risk:** low for comparison; overly frequent submissions interrupt passes and penalize the Mali tiler. The current source already uses 1300 as the default.

4. **Lighter resolve shaders: another real result on phone.**
   **V:** Geometry Wars/Adreno650: regression **25→13 fps** with larger resolve shaders; separate variant `accurate_resolve_number_formats=false`, then about **22 fps when warm**. Changing only two runtime cvars was not enough. [Investigation, August 2026](https://github.com/rfandango/XenDroid/blob/main/docs/gw-gpu-bottleneck-investigation.md).
   **US/H:** interesting if MW2 spends much time in resolves. **Estimate:** indeterminate; no Mali/MW2 evidence. **Cost/risk:** medium; lower accuracy for some formats/gamma.

5. **MW2: resolution and MSAA, with precise hashes/TUs.**
   **V:** official **TU0** patch, hash `A19F21C38721F84F`: **1024×600→1280×720**, MSAA **2→0**, meant to address flickering during upscaling. [Patch](https://github.com/xenia-canary/game-patches/blob/main/patches/41560817%20-%20Call%20of%20Duty%20-%20Modern%20Warfare%202%20%28TU0%29.patch.toml).
   **US/H:** this is not a "low resolution" patch: it increases the pixels by **50%**. Conceptually separating AA and resolution is useful. **Estimate:** no documented fps gain; a lower pixel count does not eliminate the 6400 draws. **Cost/risk:** medium; visual quality and addresses differ across TUs; no speed-up TU verified.

6. **Shared memory: upstream zero-copy ruled out by the project constraints.**
   **V:** Edge imports guest RAM through `VK_EXT_external_memory_host`; without the extension it falls back to the copied path. There is also selective updating of the page-state bitmaps. [Import](https://raw.githubusercontent.com/has207/xenia-edge/edge/src/xenia/gpu/vulkan/vulkan_shared_memory.cc), [page-state](https://raw.githubusercontent.com/has207/xenia-edge/edge/src/xenia/gpu/shared_memory.cc).
   **US/H:** the bitmaps do not automatically eliminate `mprotect`; no public patch found with a Mali A/B for those 2 ms. **Limit estimate:** eliminating all 2 ms, if entirely critical, would take roughly 38–46 to **41–51 fps**. **Cost/risk:** high, CPU/GPU coherence.

7. **MW2 cvars: a useful lead, Xbox 360 effectiveness still to be demonstrated.**
   **V:** an IW4X/MW2 catalog of 1731 DVARs exists, but it concerns the PC context. [Catalog](https://github.com/back1ply/iw4x-toolkit).
   **US/H:** candidates to verify in the XEX: `r_dof_enable`, `r_distortion`, `r_glow`, `r_specular`, `r_picmip`; they are not validated 360 settings. **Estimate:** not quantifiable. **Cost/risk:** medium; absent names, protected flags or ignored values. No MW2/Android preset with A/B found.

8. **An MW2 recomp really exists: a strategic alternative.**
   **V:** `PaulCombal/mw2-recompiled`: v0.1.0 **27 September**, v0.1.1 **30 September**; Windows/Linux **x86-64**, Vulkan, disc **1.0.557 without TU**. XenonRecomp generates C++, but does not provide the runtime. [Release](https://github.com/PaulCombal/mw2-recompiled/releases), [architecture/build](https://github.com/PaulCombal/mw2-recompiled/blob/master/docs/building.md), [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).
   **US/H:** possible basis for ARM64, not a ready Android port. **Estimate:** high potential but no defensible fps prediction. **Cost/risk:** very high; runtime, SIMD, renderer and Mali driver remain substantial work.

9. **Fork status: choose verifiable sources, do not rename the emulator.**
   **V:** Canary has A64; [XenDroid](https://github.com/rfandango/XenDroid) originates from [aX360e](https://github.com/aenu1/ax360e) and is rebased on [Edge](https://github.com/has207/xenia-edge). [X360 Mobile](https://github.com/Ashnar2602/X360-Mobile---OFFICIAL) does not publish the core; [Eugengtt](https://github.com/Eugengtt/Xendroid-Fork) declares overlay/limiter, without controlled MW2 benchmarks.
   **US:** Edge/XenDroid are the comparable sources. **Estimate:** no gain attributable to the fork change alone. **Cost/risk:** regressions; proprietary optimizations cannot be transferred for audit.

10. **Community: compatibility is growing, evidence of stable 60 is insufficient.**
    **Verifiable reports:** Pixel9a/MaliG715 MW2 about **20 fps with defects**; Thor announces 30 but specifies **15–20, peaks 30**; RP6/Turnip36 reports flickering and drops to **10–20**. [Pixel](https://www.reddit.com/r/EmulationOnAndroid/comments/1vgaz5b/xendroid_xbox_360_emulator_games_tested_on_mali/), [Thor](https://www.reddit.com/r/AynThor/comments/1voz85a/mw2_fonctionnel_xendroid_turnipgpu/), [RP6, September](https://www.reddit.com/r/retroid/comments/1wq6le6/xendroid_help_needed/).
    **US:** useful as a catalogue of symptoms; no estimable gain nor proof of 120 without artifacts. **Cost/risk:** low to consult, high to extrapolate fps without build/scene/duration.


## Topic 3

**Priority: thermal efficiency and coordinated scheduling.** From the measured data, native 60 requires saving **5.1–9.6 ms/frame**; the draws alone cost about **17.3 ms**, above the 16.7 ms budget. These are calculations on the data provided: no source shows that scheduling alone is enough.
**V = verified in the source; H = hypothesis for XenDroid.** The estimates below are exploratory, not additive.

1. **ADPF: deadlines for critical threads, efficiency for secondary ones.**
   **V:** public API, no root needed; it influences CPU frequency and placement. `setPreferPowerEfficiency` is available from API 35, but does not reserve cores. [Documentation](https://developer.android.com/reference/android/os/PerformanceHintManager.Session).
   **For us/H:** separate sessions for the command processor and for guest work synchronized to the frame; budget referred to **native** frames, not to the 8.33 ms of the interpolated presentation. Efficient mode only for work outside the critical path.
   **Figures:** MediaTek demo: **+8.5 fps, −12% power consumption, −25% FPS deviation**, including load adaptation; historical source from 2024. **Our estimate:** 0–10% FPS. **Cost/risk:** medium integration; HyperOS HAL to be verified; excessive boost can worsen thermals. [Measurements](https://developer.android.com/stories/games/mediatek-adpf).

2. **Blocking waits and selective idle skipping.**
   **V:** Dolphin uses a `BlockingLoop` and suspends polling when the GPU is idle; it preserves CPU/GPU synchronization. [Code](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/BlockingLoop.h), [FIFO](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/Fifo.cpp).
   **For us/H:** look for host spins and guest wait loops in the JIT; the **79% JIT does not demonstrate useful work**, while the 10% futex does not demonstrate energy waste.
   **Estimate:** 5–15% less power consumption **if** a consistent spin exists; no quantifiable gain before classifying it. **Cost/risk:** high for guest idle skipping; late wakeups, desynchronization and deadlocks.

3. **Coordinated affinity + nice: reserve capacity for the command processor.**
   **V:** AetherSX2/NetherSX2 order and pin EE/GS/VU onto the most powerful cores; it is a policy for a few main threads. [Project FAQ](https://github.com/Trixarian/NetherSX2-classic/blob/main/assets/faq.html).
   **For us/H:** try CP on a fast core, excluding that core from the masks of the guests and of the controllable workers; guests on the other seven. This differs from the pin already tested, which left the contention in place.
   **Nice without root:** AOSP Android 16 sets `RLIMIT_NICE=40`, allowing down to −20; HyperOS needs to be verified. Moderate candidates: CP −4, guest 0, secondary workers +10. [AOSP](https://android.googlesource.com/platform/system/core/+/refs/heads/android16-release/rootdir/init.rc).
   **Estimate:** 0–10% FPS, regression also possible. **Cost/risk:** medium; affinity does not exclude the OS/driver, nice does not guarantee placement, and the guests may become the new bottleneck.

4. **Frame pacing separated between emulation and presentation.**
   **V:** Android recommends Swappy/Frame Rate API for consistent pacing and refresh; 120 Hz requests can be limited by the system for thermal reasons. [Guide](https://developer.android.com/games/optimize/display-refresh-rate-change).
   **For us/H:** produce guest frames at the native cadence and interpolate them to the display cadence, with blocking waits and bounded queues; avoid useless duplicate presentations.
   **Estimate:** 0–5% power consumption at equal FPS, mainly less jitter; larger savings only if redundant work exists today. **Cost/risk:** medium; added latency or irregular interpolation pacing.

5. **Direct uclamp: a possible experiment, not exclusive control of the cores.**
   **V:** Linux exposes min/max 0–1024 via `sched_setattr`; on the normal path, requests on one's own task do not necessarily require privileges. Cgroups and global limits can reduce its effect. [Android kernel](https://android.googlesource.com/kernel/common/+/e5f0a698b34ed76002dc5cff3804a61c80233a7a/kernel/sched/syscalls.c), [documentation](https://docs.kernel.org/scheduler/sched-util-clamp.html).
   **For us/H:** compare a moderate minimum on the CP with ADPF; verify availability, return values and actual effect on HyperOS. Avoid a minimum of 1024 on all guests.
   **Estimate:** 0–5% FPS. **Cost/risk:** medium; more heat, interference with the Power HAL. `uclamp_max` is not a hard limit on the CPU clock and does not control the Mali clock.

6. **Thermal Headroom API: early load adaptation.**
   **V:** Android 16 adds listeners and thresholds; headroom 1 indicates severe throttling. The estimate follows slow, sometimes shallow sensors: it is not equivalent to the SoC temperature. [Current API](https://developer.android.com/reference/android/os/PowerManager).
   **For us/H:** use hysteresis to reduce the cost of motion estimation/interpolation first; if it returns NaN, use Thermal Status. The generic guide prescribes polling every 10 s, while the current reference indicates about 1 s.
   **Estimate:** no direct increase; possible improvement in sustained stability. **Cost/risk:** low–medium, OEM support and signal delay.

7. **HyperOS Performance/Game Turbo and Android Game Mode.**
   **V:** Xiaomi confirms for **Poco X7 Pro** that Game Turbo increases heat under load and suggests balanced mode. [Specific source](https://www.mi.com/uk/support/faq/details/KA-530033/).
   **For us/H:** sustained comparison Turbo/performance/balanced: at 95–98 °C what matters is which policy preserves more useful work after throttling. Game Mode communicates preferences; it does not guarantee big cores. [API, updated 2026](https://developer.android.com/games/optimize/adpf/gamemode/gamemode-api).
   **Estimate:** 0–10% less power consumption with balanced, FPS possibly lower too. **Cost/risk:** low; no public A/B found on XenDroid/X7 Pro, possible OEM limit on 120 Hz presentation.

8. **MediaTek MAGT/FPSGO: vendor integration, access not demonstrated.**
   **V:** MAGT requires contacting MediaTek for the SDK/manual; MediaTek declares **−20% power and over +10 fps** in Naraka Mobile, via integrated adaptation. [Official portal](https://developer.mediatek.com/MAGT).
   **For us/H:** ADPF is the first public path; rootless access by XenDroid to MediaTek FPSGO/GameMode or to a Mali clock limit is **not verified**.
   **Expected gain:** not estimable without SDK and OEM support. **Cost/risk:** high, commercial dependency and the impossibility of directly transferring native game numbers.

9. **What to take from other emulators.**
   **V:** Eden, a yuzu fork, integrated ADPF, nice and affinity groups on **8 August 2026**; refactor regressions are reported, with no proof that they depend on ADPF. [PR/code](https://git.eden-emu.dev/eden-emu/eden/pulls/4254/files), [issue](https://git.eden-emu.dev/eden-emu/eden/issues/4370).
   Winlator/Bannerlator exposes per-process affinity and reports spinning guest workers; GameHub on X7 Pro has reports of **30–60 fps**, without scheduler A/B. [Issue](https://github.com/The412Banner/Bannerlator/issues/350), [Reddit 2025](https://www.reddit.com/r/EmulationOnAndroid/comments/1njigyi/gamehub_510_on_mali_gpu/).
   **For us:** useful as implementation references; **gain not transferable**. Low cost to study, high risk in copying policies without measuring CP, guest runnable wait, p95 frametime and power consumption at stabilized temperature.
## Topic 4

**Priority: wider invalidation and hybrid tracking; zero-copy mainly requires cached CPU RAM and correct synchronization.**
V = verified in the sources; H = hypothesis for XenDroid. The estimates are not benchmarks on the Poco and are not additive.
From the measured data: eliminating 2 ms entirely from the critical path would ideally take 38–46 to **41–51 fps**; for 60, **5.1–9.6 ms/frame** are needed.

1. **Adaptive coarse invalidation + merging of protections.**
   **V:** [Xenia desktop](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/shared_memory.cc#L392) widens up to **256 KiB**, avoiding data written by the GPU: historical benchmark in the comment, **64 KiB → 3 ms**, **256 KiB → 0.7 ms** on an i7-3770, another game. [RPCS3](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/texture_cache.h#L844) merges ranges to unprotect and preserves overlaps.
   **H/US:** first verify how much XenDroid already inherits; widen only heavily rewritten CPU-owned ranges, keeping GPU-owned ones precise.
   **Estimated gain:** **0.5–1.5 ms/frame** if the ~100 calls decrease. **Cost/risk:** medium; more texture uploads and invalidations, particularly expensive with decompressed BCn.

2. **Zero-copy by allocating shareable RAM first: AHardwareBuffer BLOB.**
   **V:** [Vita3K, current code](https://github.com/Vita3K/Vita3K/blob/master/vita3k/renderer/src/vulkan/renderer.cpp#L1165) allocates BLOB, imports into Vulkan and links the CPU pointer to the guest memory; prefers coherent+cached. [NDK](https://developer.android.com/ndk/reference/group/a-hardware-buffer) defines BLOB as a byte buffer; [Khronos](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ANDROID_external_memory_android_hardware_buffer.html) allows import, without guaranteeing host-visible Vulkan memory.
   **H/US:** the absence of Mali export does not prevent this path; the guest backing must be created there, Xbox aliases preserved and the effective cacheability verified. Successful import ≠ CPU pointer coherence.
   **Estimated gain:** time of the eliminated copies minus sync/waits; hypothetical example **16 MiB at 10–20 GB/s = 0.84–1.68 ms per copy**. **Cost/risk:** high; CPU writes while the GPU reads, mapping/aliasing, texture cache still required.

3. **Hash/compare only on consumed ranges, with different treatment for dynamic and static resources.**
   **V:** [Cemu](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/HW/Latte/Core/LatteBufferCache.cpp) hashes **1 KiB** blocks, merges uploads and handles streamout separately. [Dolphin](https://dolphin-emu.org/blog/2021/11/13/dolphin-progress-report-september-and-october-2021/) documents texture hashing; its [fastmem, 2026 update](https://dolphin-emu.org/blog/2026/03/12/dolphin-progress-report-release-2603/) uses fault/backpatch for translation/MMIO, not a general GPU dirty tracker.
   **H/US:** full hash or snapshot comparison for small/frequently modified ranges; protections for large static textures. Verify at consumption, even multiple times in the same frame.
   **Estimated gain:** **0–1.5 ms/frame**; reading 8 MiB at 20 GB/s ideally costs **0.42 ms**; comparison with a snapshot roughly doubles traffic.
   **Cost/risk:** medium; hash collisions, inconsistent snapshots and bandwidth/energy. Scanning all 512 MiB already costs **26.8 ms** in the same model.

4. **Software write-tracking in the JIT, limited to pages observed by the GPU.**
   **V:** [yuzu, historical source preserved](https://github.com/NicolasArvani/yuzu/blob/master/src/core/memory.cpp) combines `RasterizerCachedMemory` pages, fastmem protections and per-core write collection. "Cached" here indicates rasterizer state, not hardware cacheability.
   **H/US:** per-thread dirty bitmap, checking of stores toward GPU ranges, coverage also of HLE/memcpy and aliases; collection at the consumption points.
   **Estimated gain:** up to the **known 2 ms**, minus JIT overhead; possible regression. **Cost/risk:** high, given the 79% in JIT; bitmaps of **16 KiB/thread** for 512 MiB with 4 KiB logical pages, races and unintercepted stores.

5. **Direct import from `/dev/dma_heap/system`, keeping memory cached.**
   **V:** [AOSP, updated June 2026](https://source.android.com/docs/core/architecture/kernel/dma-buf-heaps) distinguishes cached `system` from `system-uncached` and requires ueventd/SELinux permissions; accessibility from the app remains unverified. [Vulkan dma-buf](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_dma_buf.html) requires handle-type compatibility: a dma-buf fd is not automatically `OPAQUE_FD`.
   **V:** [DMA_BUF_IOCTL_SYNC](https://docs.kernel.org/driver-api/dma-buf.html) bounds CPU access, **does not replace fences**; the standard interface contains no offset/length. The [Android system heap](https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/drivers/dma-buf/heaps/system_heap.c) synchronizes the scatterlists of the attachments.
   **H/US:** a single 512 MiB buffer can make every sync expensive; smaller chunks reduce the area but complicate binding/aliasing.
   **Estimated gain:** same limit as the eliminated copies, possible negative balance. **Cost/risk:** high; no ioctl cost in µs verified on the driver; uncached guest RAM can heavily penalize the six threads.

6. **`DC CVAC` from EL0: selective cleaning, only with a valid coherence contract.**
   **V:** [Linux ARM64 6.6](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/include/asm/sysreg.h#L740) normally enables UCI; [the trap handler](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/kernel/traps.c#L556) can emulate/promote the operations. Availability without traps on the Poco not verified.
   **H/US:** cleaning only dirty lines could avoid full scans; a barrier and GPU synchronization are needed. `CVAC` does not invalidate GPU caches nor resolve CPU reads after GPU writes.
   **Estimated gain:** indeterminate relative to the driver sync; **16 MiB/64 B = 262.144 operations**, 512 MiB = **8.39 million**. **Cost/risk:** high; no general substitute for the dma-buf API.

7. **Soft-dirty and PAGEMAP_SCAN: no ready alternative on the given kernel.**
   **V:** upstream ARM64 6.6 lacks soft-dirty support, confirmed in the [2025 selftests/backports](https://www.spinics.net/lists/stable/msg881964.html); where supported, [clear_refs](https://docs.kernel.org/admin-guide/mm/soft-dirty.html) re-protects and causes kernel faults. [PAGEMAP_SCAN, 2026 manual](https://man7.org/linux/man-pages/man2/PAGEMAP_SCAN.2const.html) arrives in **6.7** and `PAGE_IS_WRITTEN` requires async uffd-WP.
   **US/estimated gain:** **0 under the current constraints**. **Cost/risk:** kernel patch/backport; backporting only the ioctl does not remove the uffd-WP dependency.

8. **MTE: does not offer a hardware bitmap of writes.**
   **V:** [ARM64 Linux documentation](https://docs.kernel.org/6.6/arch/arm64/memory-tagging-extension.html): tags every **16 B**, mismatches on accesses; ASYNC reports without address, `PROT_MTE` limited to anonymous/tmpfs/memfd.
   **H/US:** distinguishing stores would require different pointers/tags in the JIT and fault handling; incompatible with normal dma-buf mappings.
   **Estimated gain:** **none demonstrated**. **Cost/risk:** very high; MTE support on the device not verified, greater complexity than software tracking.
## Topic 5

As of **1 October 2026**, no source consulted demonstrates **60→120 stable and without artifacts on Mali-G720 MC7**. For US the most concrete path is to improve internal FG, HUD and pacing; the existing ports are references to compare against.

**Constraint calculated from the measured data:** 4.7 ms of motion estimation × 60 = **282 ms GPU/s**, before interpolation. 2× FG must run once per native pair: it produces 76–92 fps from 38–46, or 120 from a sustained 60 **with FG active**.

1. **Reduce the cost of our motion estimation — maximum impact.**
   **Verified:** LSFG Android exposes *flow scale* and a Performance mode; Arm documents FP16 advantages. [Android port](https://github.com/FrankBarretta/LSFG-Android), [Arm](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/arm-immortalis-g715-developer-overview).
   **For US, proposal:** luminance, pyramidal search at half width/height, selective refinement on edges; reuse the flow for the pair. **Estimate:** ME 4.7→2–3 ms if the per-pixel cost prevails; **risk:** small objects and fast movements get worse; FP16 requires feature verification.

2. **Android pacing at 120 Hz — necessary for stability.**
   **Verified:** Swappy supports Vulkan and controls interval/pipeline; `VK_GOOGLE_display_timing` offers scheduling and feedback; `VK_EXT_present_timing` was presented in December 2025. [Swappy](https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions), [Khronos](https://www.khronos.org/blog/vk-ext-present-timing-the-journey-to-state-of-the-art-frame-pacing-in-vulkan).
   **For US, proposal:** a single final scheduler at **8.33 ms**, native production at 16.67 ms, alternation of generated/real frames, no accumulation of expired frames; Swappy on the final stream or an own scheduler. **Estimate:** fewer duplications/jitter, 0 additional native fps; **risk:** extensions to verify on the driver; buffering and waiting for the next frame increase latency.

3. **Separate HUD; static masks as a first step.**
   **Verified:** FSR3 provides a separate UI, UI callbacks, or a comparison of the scene without/with HUD. [AMD documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation-swap-chain/).
   **For US, proposal:** exclude the crosshair, ammo and text from the warp; better solution: intercept the HUD draws and recompose them after FG. **Estimate:** strong reduction of HUD distortions, 0 native fps; **cost/risk:** MW2-specific identification; copying rectangles from the original frame also freezes the background; static masks do not follow the minimap/menus.

4. **Bidirectional flow, occlusions and fallback for low confidence.**
   **Verified:** FSR interpolates from the two endpoints, handles disocclusions and inpainting; Samsung documents pyramidal bidirectional estimation. [AMD shader](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation/ffx_frameinterpolation.h), [Samsung Research](https://research.samsung.com/blog/Enhanced-Bi-directional-Motion-Estimation-for-Video-Frame-Interpolation).
   **For US, proposal:** forward/backward consistency + photometric error; choose the endpoint visible in disocclusions, locally repeat an original frame when both are uncertain, reset on cuts. **Estimate:** fewer trails/halos, qualitative gain not quantifiable; **cost/risk:** a second search and masks; the fallback introduces small stutters; flashes/explosions can distort the confidence.

5. **LSFG 3/3.1 and lsfg-vk Android — useful comparison, no Mali guarantee.**
   **Verified:** LSFG3 came out in January 2025; the producer declares −40% GPU load at 2× compared with LSFG2. The Android port uses LSFG3.1/3.1P, MediaProjection and an overlay, declaring operation on Adreno 7xx+. [Official announcements](https://steamcommunity.com/app/993090/announcements/?l=english), [Port](https://github.com/FrankBarretta/LSFG-Android).
   **For US:** evaluate direct integration into the renderer. The fork offers AHardwareBuffer import, but keeps an internal device and `waitIdle`. **Estimate:** potential 2× visual; G720 time unknown; −40% not transferable to our ME. **Cost/risk:** synchronization, proprietary shaders from a purchased DLL, additional capture/composition in the overlay. [AHB fork](https://github.com/FrankBarretta/lsfg-vk-android).

6. **MediaTek MFRC — vendor alternative more relevant to our SoC.**
   **Verified:** documentation published in December 2025: Vulkan, color buffer required; separate HUD, motion vectors and depth optional; 60→120 declared. SDK available by contacting MediaTek. [FAQ](https://developer.mediatek.com/MFRC/695150639a8bb35dac3c0609.html), [SDK access](https://developer.mediatek.com/MFRC/695151be9a8bb35dac3c060b.html).
   **For US:** the minimum input matches what we already have. **Estimate:** possible 2× visual, performance not estimable before the SDK; **cost/risk:** access/licence and Dimensity 8400 compatibility to be confirmed. The "up to 40%" saving is a MediaTek claim, not a prediction for XenDroid.

7. **FSR3 frame interpolation — open code, demanding integration.**
   **Verified:** supports Vulkan, FG separable from upscaling, optical flow and UI handling; MIT license. [AMD GPUOpen](https://gpuopen.com/fidelityfx-super-resolution-3/).
   **For US:** mainly a reference for disocclusions/inpainting; full integration requires coherent depth, camera and motion vector data, not automatically equivalent to block vectors. **Estimate:** possibly superior quality and 2× visual, no reliable ms estimate; **cost/risk:** recovering guest data, adapting mobile shaders/backend; async compute does not guarantee savings on the same GPU.

8. **Arm ASR and NFRU — distinguish upscaling from FG.**
   **Verified:** ASR derives from FSR2 and is an upscaler; the new Arm SDK includes **NFRU Android/Vulkan**, an intermediate frame, depth/MV/camera and an emulated ML layer where native extensions are missing. [ASR](https://github.com/arm/accuracy-super-resolution), [NFRU SDK](https://github.com/arm/neural-graphics-sdk-for-game-engines).
   **For US:** NFRU can be studied today, but is not a ready FG feature of the G720. **Estimate:** theoretical 2× visual; MC7 speed unknown. **Cost/risk:** guest inputs and inference. Arm publishes **2.0–2.9 ms at 2400×1080 on Immortalis-G720 for ASR**, not FG nor our MC7. [Measurements](https://github.com/arm/accuracy-super-resolution-generic-library#performance).

9. **GameNative/GameHub/Winlator FG — copy the architecture, verify the algorithm.**
   **Verified:** GameNative contains LSFG on real presentation and a native path in the renderer, in addition to synchronization via Choreographer. [Code](https://github.com/utkarshdalal/GameNative/blob/master/app/src/main/java/app/gamenative/utils/LsfgVkManager.kt).
   **For US:** a concrete reference for integrating FG inside XenDroid. **Estimate:** elimination of the capture overhead compared with the overlay, unknown ms; **cost/risk:** adaptation of the presentation. [GameHub/Winlator 2026 reports](https://www.reddit.com/r/EmulationOnAndroid/comments/1t97aju/framegen_and_input_latency_test_30fps_capped_vs/) are testimonials, not proof of LSFG/FSR equivalence or Mali stability.

10. **Qualcomm, Samsung and OEM insertion — low direct usefulness for US.**
    **Verified:** AFME is Adreno technology; Samsung ENSS/NFG describes Vulkan inference without copies and results on Exynos 2600. [Qualcomm 2025](https://www.qualcomm.com/developer/blog/2025/08/optimize-performance-and-graphics-for-adreno-gpu-low-power-gaming), [Samsung](https://semiconductor.samsung.com/news-events/tech-blog/enss-the-next-frontier-in-mobile-ai-graphics/).
    Xiaomi limits Smart Frame Rate on the F7 Pro to supported games; on OnePlus, users report similar limits even with emulators. [Xiaomi](https://www.mi.com/global/product/poco-f7-pro/), [Reddit 2026](https://www.reddit.com/r/EmulationOnAndroid/comments/1q5sp2p/helpdiscussion_about_onepluss_frame_interpolation/). **Cautious estimate:** no gain to be counted on the Poco X7 Pro; **risk/cost:** hardware, SDK and OEM integrations are not automatically transferable.

**Proposal:** reduced ME → HUD → bidirectional confidence → single scheduler; validate at thermal steady state with FG active, GPU p95/p99 times and the intervals actually shown, including fast rotations, reloads, explosions and menus. The fallback can avoid distortions by accepting repeats: "120 without artifacts" remains a criterion to be demonstrated.
## Topic 6

**The most solid numerical gains found are Dolphin's fastmem and FEX's call/return. For US, polling for contention and the thermal limit is the place to start.** None of the cited benchmarks measures MW2 on XenDroid.
The order below is by potential impact on this case. The estimates indicate **reduction of guest CPU time**, are conditional on the hotspots, and **do not add up**.

1. **Spin-loop: recognize waits and suspend with notified wake-up.**
   **Verified:** [RPCS3 GETLLAR](https://github.com/RPCS3/rpcs3/pull/15623) dynamically detects long waits and coordinates notifications between threads; it concerns SPU, not Xenon.
   [RPCS3 ARM64 microtest, February 2026](https://github.com/RPCS3/rpcs3/pull/18151): busy-spin 3604 mW, WFE 2235 mW, idle baseline 2216 mW; no fps measurement associated.
   **For US:** polling could be hidden in the 79% JIT and take resources from the command processor. WFE saves energy; a notified suspension can also free the core.
   **Estimate:** 0–25% guest CPU, only if the polling really weighs. **Cost/risk:** high; lost wake-ups, deadlocks, latency. [ARM](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/multi-threaded-applications-arm) documents why YIELD is often equivalent to NOP and an isolated WFE is problematic.

2. **Register allocation: fewer spills and fewer PPC context saves.**
   **Verified:** [Dolphin JitArm64](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/JitArm64/JitArm64_RegCache.cpp) implements register caching; [Cemu](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocator.cpp) offers an IML allocator to study. No isolated quantitative A/B verified.
   **For US:** analyze liveness, spills in loops and registers preserved across helpers; 128 guest VMX registers make host register pressure particularly important.
   **Estimate:** 0–15% guest CPU if spill/context traffic is significant. **Cost/risk:** high; aliasing, state visible in exceptions, correctness at calls.

3. **Predictable calls/returns, block linking and indirect call caching.**
   **Verified:** [FEX 2508, August 2025](https://fex-emu.com/FEX-2508/) reports **about +39% fps in Cyberpunk 2077**, attributed mainly to hardware call/return prediction; the release contains other optimizations.
   **For US:** directly link known destinations; for repetitive indirect ones, evaluate a per-site cache, with a guest-address check and fallback.
   **Estimate:** 0–15% guest CPU if dispatch and branch misprediction weigh. **Cost/risk:** medium-high; LR/CTR, unconventional returns, invalidation.
   [Dolphin 2603](https://dolphin-emu.org/blog/2026/03/12/dolphin-progress-report-release-2603/) also shows the limit: larger blocks can worsen compilation and invalidation.

4. **Fastmem RAM, specialized MMIO and minimal big-endian conversion.**
   **Verified:** [Dolphin 2603, March 2026](https://dolphin-emu.org/blog/2026/03/12/dolphin-progress-report-release-2603/): page-table fastmem brings Cars 2 **37→57 fps (+54%)**, but Spider-Man 2 **56→52**; external benchmarks, not on our device.
   [Canary A64](https://raw.githubusercontent.com/xenia-canary/xenia-canary/canary_experimental/src/xenia/cpu/backend/a64/a64_sequences.cc) already emits REV/REV16/REV32: hardware byte-swap is an existing basis.
   **For US:** look for redundant RAM checks/helpers and MMIO that keeps generating faults; specialize the identified accesses. CPU fastmem remains useful even with a separate copy to the GPU.
   **Estimate:** 0–10% guest CPU if the RAM path is already direct; potentially greater if it often goes through helpers. **Cost/risk:** high; aliasing, accesses across page boundaries, distinguishing MMIO faults from GPU protections.
   Box64/FEX normally translate little-endian→little-endian: they do not demonstrate gains on our endian-swap.

5. **VMX128→NEON: eliminate hot fallbacks and useless FPCR transitions.**
   **Verified:** [Canary A64](https://raw.githubusercontent.com/xenia-canary/xenia-canary/canary_experimental/src/xenia/cpu/backend/a64/a64_sequences.cc) contains NEON operations, vector helpers and dot products with FP64 intermediates; [Wunkolo](https://github.com/xenia-project/xenia/pull/2259) documents MOVI/FMOV optimizations and lane operations, without percentages.
   **For US:** profile individual sequences, permutations and FPU/VMX boundaries before shortening the code.
   **Estimate:** 0–10% guest CPU. **Cost/risk:** medium-high; rounding, NaN, denormals and lane order can change visible results.

6. **Selective HLE of memcpy/memset/strcmp.**
   **Verified:** [FEX 2509](https://fex-emu.com/FEX-2509/) documents library forwarding to native ARM64 libraries; no isolated benchmark verified for these three functions.
   **For US:** recognize only MW2 routines measured as hot; byte copies do not require endian-swap.
   **Illustrative estimate:** hotspot equal to 10% guest CPU, accelerated 4× → **7.5% time saved**. **Cost/risk:** medium; ABI, protections/dirty tracking, memory boundaries; host strcmp may read beyond what is needed.

7. **XenonRecomp: reference for global optimization and HLE, separate investment.**
   **Verified:** [README](https://github.com/hedge-dev/XenonRecomp#optimizations) attributes to the transformation of registers into local variables **several milliseconds of frametime saved** and about 20 MB of executable removed in Unleashed Recompiled.
   **For US:** qualitative evidence of the value of eliminating context stores/save-restore; it does not measure the ARM64 JIT.
   **Estimate:** not quantifiable for MW2. **Cost/risk:** very high; static recompilation and title-specific integration.

**Constraint from the measured data:** 6400×2.7 µs ≈ **17.28 ms CPU/frame** in the command processor, already above the 16.67 ms of 60 fps. Reducing the guest may relieve contention and heat, but native 60 also requires margin there; these references do not prove interpolated 120 without artifacts.
