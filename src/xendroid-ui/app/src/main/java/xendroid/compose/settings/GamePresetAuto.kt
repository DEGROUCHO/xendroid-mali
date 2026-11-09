package xendroid.compose.settings

private fun k(section: String, name: String) = "$section|$name"

/**
 * Adreno only (kgsl). Not isCustomDriverSupported: custom drivers also load on Mali (kbase),
 * and Mali must keep the performance preset.
 */
fun hostIsAdreno(): Boolean = runCatching { java.io.File("/dev/kgsl-3d0").exists() }.getOrDefault(false)

/**
 * Builds the "Auto" quick preset from the detected GPU vendor.
 *
 * Adreno -> quality-oriented: 2x/2x resolution, MSAA on, fast occlusion queries,
 * FXAA, CAS sharpening, 16x anisotropic filtering, uncapped framerate.
 * Mali/other/emulator -> performance-oriented: 1x/1x, native 2x MSAA on (cheaper than
 * the 4x fallback), fake occlusion queries, no AA, bilinear scaling, 60 fps cap. Texture
 * cache, resolve and async-draw policy follow the core defaults.
 */
fun autoPreset(isAdreno: Boolean): GamePreset =
    if (isAdreno) {
        GamePreset(
            id = "auto",
            title = "Auto",
            subtitle = "Adreno: quality",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "2",
                k("GPU", "draw_resolution_scale_y") to "2",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "0",
                k("GPU", "occlusion_query") to "fast",
                k("GPU", "anisotropic_override") to "5",
                k("Display", "postprocess_antialiasing") to "fxaa",
                k("Display", "postprocess_scaling_and_sharpening") to "cas",
            ),
        )
    } else {
        GamePreset(
            id = "auto",
            title = "Auto",
            subtitle = "Mali/other: performance",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "1",
                k("GPU", "draw_resolution_scale_y") to "1",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "60",
                k("GPU", "occlusion_query") to "fake",
                k("GPU", "vulkan_mid_frame_submission_draws") to "0",
                k("GPU", "clear_memory_page_state") to "false",
                k("GPU", "half_pixel_offset") to "true",
                k("Display", "postprocess_antialiasing") to "none",
                k("Display", "postprocess_scaling_and_sharpening") to "",
            ),
        )
    }
