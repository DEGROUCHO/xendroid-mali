package xendroid.compose.settings

/** A named bundle of per-game overrides applied in one tap from the quick-preset row. */
data class GamePreset(
    val id: String,
    val title: String,
    val subtitle: String,
    /** "Section|name" -> raw value, validated against [SettingsSchema] on apply. */
    val values: Map<String, String>,
)

object PerGamePresets {
    private fun k(section: String, name: String) = "$section|$name"

    // Mali-safe baseline shared by all tiers: native 2x MSAA on (cheaper than the 4x
    // fallback), all tiler-friendly resolve optimizations on, no per-frame page-state
    // pass, correct half-pixel offset (disabling it breaks post-processing).
    val all: List<GamePreset> = listOf(
        GamePreset(
            id = "performance", title = "Performance", subtitle = "Max FPS",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "1",
                k("GPU", "draw_resolution_scale_y") to "1",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "0",
                k("GPU", "occlusion_query") to "fake",
                k("GPU", "vulkan_mid_frame_submission_draws") to "0",
                k("GPU", "texture_cache_memory_limit_soft") to "1024",
                k("GPU", "texture_cache_memory_limit_hard") to "2048",
                k("GPU", "clear_memory_page_state") to "false",
                k("GPU", "half_pixel_offset") to "true",
                k("Vulkan", "vulkan_in_pass_resolve") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_promote") to "true",
                k("Vulkan", "vulkan_resolve_to_texture") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_serve") to "true",
                k("Vulkan", "vulkan_async_skip_draws") to "true",
                k("Display", "postprocess_antialiasing") to "none",
                k("Display", "postprocess_scaling_and_sharpening") to "",
            ),
        ),
        GamePreset(
            id = "balanced", title = "Balanced", subtitle = "Good trade-off",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "2",
                k("GPU", "draw_resolution_scale_y") to "2",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "0",
                k("GPU", "occlusion_query") to "fake",
                k("GPU", "vulkan_mid_frame_submission_draws") to "0",
                k("GPU", "texture_cache_memory_limit_soft") to "1024",
                k("GPU", "texture_cache_memory_limit_hard") to "2048",
                k("GPU", "clear_memory_page_state") to "false",
                k("GPU", "half_pixel_offset") to "true",
                k("Vulkan", "vulkan_in_pass_resolve") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_promote") to "true",
                k("Vulkan", "vulkan_resolve_to_texture") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_serve") to "true",
                k("Vulkan", "vulkan_async_skip_draws") to "true",
                k("Display", "postprocess_antialiasing") to "none",
                k("Display", "postprocess_scaling_and_sharpening") to "",
                k("GPU", "anisotropic_override") to "3",
            ),
        ),
        GamePreset(
            id = "quality", title = "Quality", subtitle = "2x + FXAA + CAS",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "2",
                k("GPU", "draw_resolution_scale_y") to "2",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "0",
                k("GPU", "occlusion_query") to "fake",
                k("GPU", "vulkan_mid_frame_submission_draws") to "0",
                k("GPU", "texture_cache_memory_limit_soft") to "1024",
                k("GPU", "texture_cache_memory_limit_hard") to "2048",
                k("GPU", "clear_memory_page_state") to "false",
                k("GPU", "half_pixel_offset") to "true",
                k("Vulkan", "vulkan_in_pass_resolve") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_promote") to "true",
                k("Vulkan", "vulkan_resolve_to_texture") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_serve") to "true",
                k("Vulkan", "vulkan_async_skip_draws") to "true",
                k("Display", "postprocess_antialiasing") to "fxaa",
                k("Display", "postprocess_scaling_and_sharpening") to "cas",
                k("GPU", "anisotropic_override") to "5",
            ),
        ),
        GamePreset(
            id = "ultra", title = "Ultra", subtitle = "3x + FXAA extreme + CAS",
            values = mapOf(
                k("GPU", "draw_resolution_scale_x") to "3",
                k("GPU", "draw_resolution_scale_y") to "3",
                k("GPU", "native_2x_msaa") to "true",
                k("GPU", "framerate_limit") to "0",
                k("GPU", "occlusion_query") to "fake",
                k("GPU", "vulkan_mid_frame_submission_draws") to "0",
                k("GPU", "texture_cache_memory_limit_soft") to "1024",
                k("GPU", "texture_cache_memory_limit_hard") to "2048",
                k("GPU", "clear_memory_page_state") to "false",
                k("GPU", "half_pixel_offset") to "true",
                k("Vulkan", "vulkan_in_pass_resolve") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_promote") to "true",
                k("Vulkan", "vulkan_resolve_to_texture") to "true",
                k("Vulkan", "vulkan_resolve_to_texture_serve") to "true",
                k("Vulkan", "vulkan_async_skip_draws") to "true",
                k("Display", "postprocess_antialiasing") to "fxaa_extreme",
                k("Display", "postprocess_scaling_and_sharpening") to "cas",
                k("GPU", "anisotropic_override") to "5",
            ),
        ),
    )
}
