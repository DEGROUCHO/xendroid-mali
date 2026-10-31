# FG research for XenDroid / MW2 (Mali-G720, no MV, no depth) - 2026-09-30
Tags: [V] verified from a fetched page or source, link given. [S] seen only in a search snippet or a page summary; re-check before relying on it. [H] hypothesis or estimate. No public ms-cost data exists for any of these on Mali-G720 (searched); every cost figure below is [H] until measured with VK timestamp queries on the phone.

Note: the earlier file framegen-2026-09.md ranks "LSFG with a proprietary Lossless.dll" first. That is proprietary, so it is excluded. LSFG stays out.

## 1) Ranked recommendations (most impactful first)
1. **Replace FSR3's depth-based disocclusion test with a flow-consistency test (forward-backward check).** FSR3's disocclusion mask is pure depth logic, so it cannot run without depth [S: raw source v1.1.4 summary]. Compute flow N-1 to N (already available) and N to N-1. At each mid-frame pixel, check |Ffw(x) + Fbw(x+Ffw(x))| > a*(|Ffw|+|Fbw|) + b (UnFlow-style test [H: unverified citation, arXiv 1711.07837]). Where the test fails, take the color from ONE source frame only, the one where the pixel is visible, and never blend there. Blending is what produces ghosts. Cost [H]: +0.4-1 ms. Cheaper variants: run the backward search only at coarse levels plus a +-1 px refinement, or derive the backward flow by splatting the inverted forward flow.
2. **Forward splat with a priority, then hole fill with an inpainting pyramid.** This is the structure of FSR3 passes 3/5 and 8/9 [V: frame-interpolation doc, link below]. Pack priority and vector into one uint and use atomicMax (R32_UINT storage-image atomics are mandatory in Vulkan [H: check the feature bit on the phone]). Without depth, the priority is larger |flow| first (closer objects move more) plus lower warped-color error [H]. Fill holes with a 4-5 level mip chain (valid-weighted average, then push-pull upsampling), which is the FSR3 inpainting idea [V]. Cost [H]: splat 0.2-0.4 ms, pyramid 0.2-0.4 ms at 720p.
3. **Kill jelly at the source: vector stability.** Seed each pair's search with the previous pair's flow, apply a 3x3 vector median (FFX OF does this [V]), refine sub-pixel with 2-3 Lucas-Kanade steps per 4x4/8x8 block (DIS idea, arXiv 1603.03590 [V]), and upsample edge-aware using color guidance. Cost [H]: under 0.5 ms.
4. **Per-pixel confidence and a fallback ladder.** Keep the gated candidate search. FSR3 picks between two vector sources by warped-color similarity (NormalizedDot3 bias) [S: raw source summary], which is the same gate idea. Add a final confidence = f(warped color error, consistency error). Low confidence means nearest-frame copy, never a blend. Add a scene-cut flag (FFX OF outputs one [V]) so the frame is simply repeated. HUD: detect pixels that are static over several frames (zero flow, equal color) and composite frame N's pixels on top of the result. No depth or HUD-less buffer is needed. Cost [H]: 0.1-0.2 ms.
5. **Decouple presentation and fix the cadence** (section 5): a dedicated 60 Hz presenter thread, Swappy or display timing, and a fixed guest cap (30 fps, for an even 2x) or a variable t.
6. **Measure first.** Put timestamp queries around every pass before adding anything. The analysis runs on the emulation queue, so each new pass competes with the game.

Not worth doing: FSR4/ML, RIFE and other neural nets, NFRU, MFRC, XeSS-FG, and any upscaler (you render 1x).

## 2) Options table
| Option | What | License | No MV/depth? | Mobile cost [H] unless noted | Link |
|---|---|---|---|---|---|
| FSR3 FG v1.1.4 (FidelityFX SDK) | OF + 9-pass interpolation | MIT [V] | Partly (see Q1 below) | No public ms numbers; Mob-FGSR (MV+depth, Snapdragon 8 Gen 3) ~11-13 ms per earlier note | https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-interpolation/ |
| FFX Optical Flow (inside FSR3) | 8x8 blocks, color only, R16G16_SINT, 3 passes x 7 iterations, median, scene-change flag | MIT | Yes, color only [V] | Needs wave ops and MSAD4 (DX12 HLSL); port to GLSL; 1-2 ms at 720p | https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/ |
| Arm ASR | FSR2-derived temporal upscaler | MIT [V] | No (needs MV+depth); upscaling only | Tuned for Immortalis-G720 | https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/introducing-arm-accuracy-super-resolution |
| Arm NFRU | Neural frame-rate upscaling, open models | Open models; MIT per earlier note [S] | No (color+depth+MV+HW flow); needs Mali G2-Ultra NX hardware, not G720 [S] | Not runnable | https://www.androidauthority.com/arm-mali-g2-ultra-deep-dive-3706351/ |
| Qualcomm SGSR 1/2 | Upscalers (1 spatial, 2 temporal) | BSD-3-Clause [S] | SGSR1 yes, SGSR2 needs MV; not FG | SGSR1 cheap; irrelevant at 1x | https://github.com/SnapdragonGameStudios/snapdragon-gsr |
| QCOM_frame_extrapolation | Adreno GL extension, takes 2 previous frames | Extension spec | Yes, but Adreno only [V per earlier note] | n/a on Mali | https://registry.khronos.org/OpenGL/extensions/QCOM/QCOM_frame_extrapolation.txt |
| Intel XeSS-FG | Needs MV + depth, DX12, Intel hardware | Intel Simplified Software License, binaries only, no reverse engineering [V] | No | n/a | https://www.phoronix.com/news/Intel-XeSS-3-SDK-GitHub |
| DIS optical flow (Kroeger 2016) | Patch-based inverse search + refinement | Paper; reference-code license unknown, OpenCV's DIS is Apache-2.0 [H: unverified] | Yes | 300-600 Hz on ONE CPU core at 1024x436 (paper) [V] | https://arxiv.org/abs/1603.03590 |
| RIFE via ncnn-Vulkan | Neural interpolation | MIT [H: unverified] | Yes | ~10 fps at 720p on Android = ~100 ms [S, Medium] | https://allenkuo.medium.com/building-a-high-performance-ai-frame-interpolation-pipeline-on-android-with-vulkan-ncnn-rife-8f279cef51cd |
| Mob-FGSR (SIGGRAPH 2024) | Mobile FG and SR with splatting and occlusion logic | Code on GitHub, license not stated on page [S] | No, needs depth+MV | Reference for splat and hole-fill design | https://mob-fgsr.github.io/ |
| ANVIL (arXiv 2603.26835) | Uses H.264 decoder MVs on an NPU | Paper | No, video playback only | 12.8 ms at 1080p, Snapdragon NPU | https://arxiv.org/abs/2603.26835 |
| Snap888/lsfg-vk | Pyramidal LK, bidirectional warp, UI mask | MIT, brand new, 0 stars [earlier note V] | Yes | Unproven on Mali; recipe only | per earlier note |

## 3) Answers
### Q1. FSR 3.1 FG without MV or depth
- [V] Official pipeline, 9 passes: setup, interpolated-frame depth estimate, game-MV field (priority = camera distance + color similarity), MV inpainting pyramid, optical-flow field (priority = magnitude + color match), disocclusion mask, interpolation, color inpainting pyramid, inpainting. Source: https://github.com/AzagraMac/FidelityFX-SDK-FSR3/blob/master/docs/techniques/frame-interpolation.md (mirror of the docs). The official page is https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/.
- [S] From a summary of the v1.1.4 raw files, `ffx_frameinterpolation_disocclusion_mask.h` compares dilated current depth and reconstructed previous depth (depth-only test, unusable for us), while `ffx_frameinterpolation.h` does not read depth. It picks between the game-MV and OF samples by color similarity, and the disocclusion factors modulate t (fall back to the other frame). Verify by reading both files.
- **Usable without depth or MV:** the Optical Flow component, the OF-vector splatting pass (5), the interpolation pass (run with OF only), the color inpainting pyramid and inpainting (8-9). **Not usable:** passes 2-4 and 6 (depth or game MV). Replace 6 with recommendation 1.
- The SDK expects AMD's wrapper and swapchain surfaces, so copy the structure and algorithms and write new GLSL. The shaders assume wave ops, and Valhall subgroups are 16 wide [H].
- No FSR3 FG port to Android, Vulkan mobile or Mali (searches return only LSFG ports), so there are no measured costs.

### Q2. FSR 4
- FSR4 source was published by mistake in FidelityFX SDK 2.0 (Aug 2025) with an MIT header and then pulled [V: https://www.tomshardware.com/pc-components/gpus/amd-accidentally-marks-fsr-4-open-source-source-code-reveals-potential-support-for-older-radeon-gpus]. AMD calls it a mistake [S: VideoCardz headline, page not readable]. This accidental leak is excluded.
- AMD said at CES 2026 that open-sourcing the library is a "long-term plan", keeping the core technology closed [V: https://www.pcgamer.com/hardware/graphics-cards/amd-suggests-that-fsr-4-will-eventually-be-open-sourced-bar-the-core-technology-as-part-of-its-long-term-plan/].
- The official FSR SDK 2.2 (2026-03-23) ships FSR Upscaling 4.1 and Redstone features. ML features need Radeon RX 9000, DX12, Windows and SM 6.4/6.6. Only the analytical FSR3 fallback is vendor-neutral. The page calls the code "binaries and limited source", with no license stated [V: https://gpuopen.com/learn/amd-fsr-sdk-2-2-now-available/].
- Older FSR (3.1.5 and before) is MIT [S: same PC Gamer page family]. **Conclusion:** nothing of FSR4 is legally or technically usable on Mali. ML FG also needs game MV and depth.

### Q4. System-level interpolation on Poco X7 Pro
- [V] MediaTek MFRC is a per-game SDK. The game must integrate it and feed color (with or without UI separation), game motion and depth. The SDK is obtained by contacting MediaTek, and no whitelist is documented: https://developer.mediatek.com/MFRC. Partner example: https://www.mediatek.com/tek-talk-blogs/mediatek-and-nte-neverness-to-everness-announce-deep-collaboration-to-advance-mobile-gaming-performance.
- [V] The Poco X7 Pro FAQ only confirms Game Turbo and 120 Hz modes for named games: https://www.mi.com/global/support/faq/details/KA-528038/. No Reddit or XDA evidence of a game frame-insertion toggle was found on the Poco X7 Pro. Xiaomi is replacing Game Turbo with "Game Space" and per-game profiles [S: https://www.gizmochina.com/2026/07/27/xiaomi-game-space-replaces-game-turbo-upgrade-features/].
- [S] Chinese press says the Redmi Turbo 4 (twin of the Poco X7 Pro) has a separate gaming display chip that interpolates Genshin from 45 to 90 fps: https://news.qq.com/rain/a/20250106A05LVC00. That the Poco X7 Pro has this chip could not be confirmed.
- [S] On iQOO, display-chip MEMC does not work in third-party video players, and users ask for OTA whitelisting, so display-chip interpolation there is per-package: https://community.iqoo.com/in/thread/148358.
- **Verdict [H]:** renaming the package cannot enable MFRC, because it lives in the app's SDK, not the OS. It might enable a display-chip or OS-level interpolation only if the phone has one and a package whitelist exists, and neither is verified. A 15-minute test costs little: `adb shell getprop | grep -iE 'memc|mfrc|frc|pixelworks|iris'`, `adb shell dumpsys display`, search /vendor and /product/etc for `*game*.xml` or `*frc*`, then rename the app to a Game Turbo game package and look for a frame-insertion switch. Do not plan the FG around it.

### Q5. Frame pacing with Swappy and Vulkan
- [V] Swappy uses VK_GOOGLE_display_timing (present timestamps), a VkFence wait to avoid buffer stuffing, and the Choreographer. It needs API 24+. `SwappyVk_setSwapIntervalNS(1e9/60)` sets the interval. It has auto swap-interval and auto-pipeline modes. On multi-rate panels it offers 120/90/60/45/40/30 and calls `ANativeWindow_setFrameRate`: https://developer.android.com/games/sdk/frame-pacing. Likely Apache-2.0 (AOSP gamesdk) [H, not checked].
- [V] VK_GOOGLE_display_timing is implemented by the Android platform, not the GPU driver (Android 10+; swapchain and ANativeWindow are not exposed to drivers): https://source.android.com/docs/core/graphics/implement-vulkan. So Mali r49p1 support does not matter. VK_EXT_present_timing needs Android 17 (API 37), so it is probably unavailable on HyperOS now: https://developer.android.com/games/develop/vulkan/frame-pacing-extensions.
- **Integration advice [H]:**
  1. Run a dedicated presenter thread with its own 60 Hz clock, separate from the guest and emulation threads.
  2. Call `SwappyVk_queuePresent` for every displayed frame, real and generated.
  3. Set the swap interval to 1e9/60. At 120 Hz that is 2 vsyncs per frame.
  4. Turn auto swap-interval OFF. Otherwise Swappy drops to 30 when one frame runs long.
  5. Use FIFO with 3 images, not MAILBOX.
  6. Call `ANativeWindow_setFrameRate(60, FIXED_SOURCE)` and test whether HyperOS drops the panel to 60 Hz or stays at 120.
  7. Validate with `vkGetPastPresentationTimingGOOGLE` (actual vs desired present time).
  8. Cadence: guest 35-45 fps against a 60 Hz output is not an integer ratio, so a fixed 2x judders. Either cap the guest at 30 fps (exact 2x) or interpolate at a variable t taken from guest timestamps. The "reproject the vector to position t" step already in the code supports a variable t.

### Q6. Artifact reduction without depth
- Forward-backward check for occlusion, bidirectional flow, forward splatting with priority (Softmax splatting, arXiv 2003.05534), depth-aware warping (DAIN, 1904.00830), hole filling by pyramid (FSR3 [V]). The arXiv IDs were not re-checked [H].
- Combined extra cost on Mali-G720 is [H] about 1.5-3 ms on top of the current passes at 720p. This is a guess; the game is GPU-bound, so measure every pass.
