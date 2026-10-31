package xendroid.compose.settings

import xendroid.compose.l10n

import xendroid.compose.settings.Setting.*

private fun desc(n: String) = SettingDescriptions.byName[n] ?: ""
private fun b(s: String, n: String, t: String, d: Boolean) = Setting.Bool(s, n, t, d, desc(n))
private fun i(s: String, n: String, t: String, d: Int, lo: Int, hi: Int) =
    Setting.IntRange(s, n, t, d, lo, hi, desc(n))
private fun l(s: String, n: String, t: String, d: String, vararg o: Pair<String, String>) =
    Setting.ListChoice(s, n, t, d, o.map { ListOption(it.first, it.second) }, desc(n))

object SettingsSchema {

    val categories: List<SettingsCategory> = listOf(

        SettingsCategory("Vulkan", listOf(
            b("Vulkan", "vulkan_sparse_shared_memory", l10n("Sparse shared memory", "Memoria condivisa sparsa"), true),
            b("Vulkan", "vulkan_log_debug_messages", l10n("Log debug messages", "Registra messaggi di debug"), false),
            b("Vulkan", "vulkan_validation", l10n("Validation layer", "Layer di validazione"), false),
            b("Vulkan", "vulkan_renderdoc_capture", l10n("RenderDoc capture layer", "Layer di acquisizione RenderDoc"), false),
            b("Vulkan", "vulkan_allow_present_mode_immediate", l10n("Allow present mode: immediate", "Consenti modalità di presentazione: immediata"), true),
            b("Vulkan", "vulkan_allow_present_mode_mailbox", l10n("Allow present mode: mailbox", "Consenti modalità di presentazione: mailbox"), true),
            b("Vulkan", "vulkan_allow_present_mode_fifo_relaxed", l10n("Allow present mode: relaxed FIFO", "Consenti modalità di presentazione: FIFO rilassata"), true),
            b("Vulkan", "vulkan_async_skip_draws", l10n("Skip async draws", "Salta draw asincroni"), true),
            b("Vulkan", "vulkan_placeholder_pipelines", l10n("Placeholder pipelines", "Pipeline sostitutive"), false),
            b("Vulkan", "vulkan_dynamic_pipeline_state", l10n("Extended dynamic state", "Stato dinamico esteso"), true),
            // A slider, not a dropdown: 0 = synchronous, 1..5 = explicit. The native cvar also
            // accepts -1 (auto), but a 0..5 slider is the intended UX.
            i("Vulkan", "vulkan_pipeline_creation_threads", l10n("Pipeline creation threads", "Thread di creazione pipeline"), 4, 0, 5),
            Action("Vulkan", "vulkan_lib_path", l10n("Custom Vulkan driver", "Driver Vulkan personalizzato"), "", desc("vulkan_lib_path")),
            b("Vulkan", "adrenotools_force_max_clocks", l10n("Force maximum GPU clocks (adrenotools)", "Forza frequenze GPU massime (adrenotools)"), false),
            // 'sysmem' forces untiled rendering (much slower) but avoids a class of Adreno GPU
            // hangs. Applied at vkCreateInstance, so it takes effect on the next game launch.
            l("Vulkan", "turnip_debug", l10n("Turnip debug mode", "Modalità debug Turnip"), "sysmem",
                "" to l10n("None (no TU_DEBUG flag, GMEM)", "Nessuno (nessun flag TU_DEBUG, GMEM)"), "sysmem" to l10n("sysmem (no tiling, slower)", "sysmem (senza tile, più lento)"),
                "sysmem,nolrz" to l10n("sysmem + nolrz (LRZ off, performance diagnostics)", "sysmem + nolrz (LRZ disattivato, diagnostica prestazioni)"),
                "sysmem,noubwc" to l10n("sysmem + noubwc (UBWC off, performance diagnostics)", "sysmem + noubwc (UBWC disattivato, diagnostica prestazioni)")),
            b("Vulkan", "vulkan_in_pass_resolve", l10n("EDRAM resolve in pass", "Resolve EDRAM nel pass"), false),
            b("Vulkan", "vulkan_resolve_to_texture_promote", l10n("Resolve to texture: promote", "Resolve su texture: promote"), true),
            b("Vulkan", "vulkan_resolve_to_texture", l10n("Resolve to texture: store", "Resolve su texture: store"), true),
            b("Vulkan", "vulkan_resolve_to_texture_serve", l10n("Resolve to texture: skip upload", "Resolve su texture: salta upload"), true),
            // Master toggle gates cross-draw descriptor-set reuse; the edge toggle (only when
            // reuse is on) picks edge's bitmask gate vs the content-hash gate for A/B.
            b("Vulkan", "vulkan_cache_texture_descriptors", l10n("Texture descriptor cache", "Cache dei descrittori texture"), true),
            b("Vulkan", "vulkan_texture_descriptor_reuse_edge", l10n("Texture descriptor gate: edge (off = hash)", "Gate dei descrittori texture: edge (disattivo = hash)"), false),
            // Emulator-wide GPU load / heat controls (not per-game). The two "next launch"
            // switches are latched by the core at GPU start; the rest are read while a game runs.
            b("Vulkan", "vulkan_fragment_shading_rate_dynamic", l10n("GPU load: variable-rate shading (from next launch)", "Carico GPU: shading a frequenza variabile (dal prossimo avvio)"), true),
            b("Vulkan", "thermal_governor", l10n("Heat: coarser shading when the phone heats up", "Calore: shading più grossolano quando il telefono si scalda"), true),
            l("Vulkan", "vulkan_fragment_shading_rate", l10n("GPU load: shading rate of 3D draws", "Carico GPU: frequenza di shading dei draw 3D"), "0",
                "0" to l10n("Full", "Piena"), "1" to l10n("2x1 (lighter)", "2x1 (più leggera)"), "2" to l10n("1x2 (lighter)", "1x2 (più leggera)"), "3" to l10n("2x2 (lightest)", "2x2 (la più leggera)")),
            b("Vulkan", "vulkan_fragment_shading_rate_alpha_tested", l10n("GPU load: coarse shading also on cut-out draws", "Carico GPU: shading grossolano anche sui draw ritagliati"), false),
            b("Vulkan", "vulkan_msaa_single_sample_storage", l10n("GPU load: single-sample MSAA targets (from next launch)", "Carico GPU: target MSAA a campione singolo (dal prossimo avvio)"), true),
            b("Vulkan", "vulkan_msaa_single_sample_resolve", l10n("GPU load: antialiasing inside each pass (from next launch)", "Carico GPU: antialiasing dentro ogni pass (dal prossimo avvio)"), false),
            b("Vulkan", "vulkan_relax_unused_attachment_ops", l10n("GPU load: skip load/store of unused targets", "Carico GPU: salta load/store dei target inutilizzati"), true),
        )),

        SettingsCategory("Video", listOf(
            b("Console", "widescreen", l10n("16:9 aspect ratio", "Formato 16:9"), true),
            l("Console", "video_standard", l10n("Video standard", "Standard video"), "1",
                "1" to "NTSC", "2" to "NTSC-J", "3" to "PAL-60"),
            l("Console", "internal_display_resolution", l10n("Display mode reported to the game", "Modalità di visualizzazione comunicata al gioco"), "8",
                "0" to "640x480", "1" to "640x576", "2" to "720x480", "3" to "720x576",
                "4" to "800x600", "5" to "848x480", "6" to "1024x768", "7" to "1152x864",
                "8" to "1280x720", "9" to "1280x768", "10" to "1280x960", "11" to "1280x1024",
                "12" to "1360x768", "13" to "1440x900", "14" to "1680x1050", "15" to "1920x540",
                "16" to "1920x1080"),
            b("Console", "use_50Hz_mode", l10n("Use 50 Hz mode", "Usa modalità 50 Hz"), false),
            l("Video", "avpack", l10n("AV cable", "Cavo AV"), "8",
                "0" to l10n("Component PAL-60 (SD)", "Componente PAL-60 (SD)"), "1" to l10n("Unused", "Non usato"), "2" to "SCART PAL-60",
                "3" to l10n("Component 480p (HD)", "Componente 480p (HD)"), "4" to "HDMI+A", "5" to l10n("Composite/S-Video PAL-60", "Composito/S-Video PAL-60"),
                "6" to "VGA", "7" to "TV PAL-60", "8" to "HDMI"),
            b("Video", "interlaced", l10n("Interlaced", "Interlacciato"), false),
            b("Video", "enable_3d_mode", l10n("Enable 3D mode", "Attiva modalità 3D"), false),
        )),

        SettingsCategory("UI", listOf(
            b("UI", "show_achievement_notification", l10n("Show achievement notifications", "Mostra notifica obiettivi"), true),
            b("UI", "storage_selection_dialog", l10n("Storage selection dialog", "Finestra di scelta archiviazione"), false),
            b("UI", "headless", l10n("Headless mode", "Modalità headless"), true),
            b("UI", "android_soft_keyboard", l10n("Android keyboard for text input in games", "Tastiera Android per l'inserimento di testo nei giochi"), true),
            b("UI", "android_message_box", l10n("Android window for game messages", "Finestra Android per i messaggi del gioco"), true),
        )),

        SettingsCategory("Storage", listOf(
            b("Storage", "mount_scratch", l10n("Mount scratch", "Monta scratch"), false),
            b("Storage", "mount_cache", l10n("Mount cache", "Monta cache"), false),
        )),

        SettingsCategory("Kernel", listOf(
            b("Kernel", "staging_mode", l10n("Staging mode", "Modalità staging"), false),
            // Takes effect on the next game launch.
            b("Kernel", "guest_scheduler", l10n("Game scheduler (cooperative fibers)", "Scheduler del gioco (fiber cooperativi)"), true),
            // Only meaningful with the guest scheduler on.
            i("Kernel", "guest_scheduler_quantum_us", l10n("Scheduler quantum (µs)", "Quantum dello scheduler (µs)"), 1000, 250, 8000),
            b("Kernel", "guest_scheduler_stats", l10n("Log scheduler statistics", "Registra statistiche dello scheduler"), false),
            b("Logging", "log_high_frequency_kernel_calls", l10n("Log high-frequency kernel calls", "Registra chiamate kernel ad alta frequenza"), false),
            l("Kernel", "kernel_display_gamma_type", l10n("Display gamma type", "Tipo di gamma del display"), "2",
                "0" to l10n("linear", "lineare"), "1" to "sRGB (CRT)", "2" to "BT.709 (HDTV)"),
            b("Kernel", "ignore_thread_affinities", l10n("Ignore thread affinities", "Ignora affinità dei thread"), true),
            b("Kernel", "kernel_pix", l10n("Kernel PIX", "PIX del kernel"), false),
            b("Kernel", "kernel_cert_monitor", l10n("Kernel certification monitor", "Monitor di certificazione del kernel"), false),
            b("Kernel", "ignore_thread_priorities", l10n("Ignore thread priorities", "Ignora priorità dei thread"), true),
            b("Kernel", "allow_incompatible_title_update", l10n("Allow incompatible title updates", "Consenti aggiornamenti del titolo incompatibili"), true),
            b("Kernel", "apply_title_update", l10n("Apply title update", "Applica aggiornamento del titolo"), true),
            b("Kernel", "kernel_debug_monitor", l10n("Kernel debug monitor", "Monitor di debug del kernel"), false),
            b("Kernel", "network_enabled", l10n("Network", "Rete"), true),
        )),

        SettingsCategory("Controller", listOf(
            b("HID", "show_touch_overlay", l10n("Show on-screen controller", "Mostra controller a schermo"), true),
        )),

        SettingsCategory("HID", listOf(
            l("HID", "hid", l10n("HID backend", "Backend HID"), "android",
                "android" to "Android", "nop" to l10n("None", "Nessuno")),
        )),

        SettingsCategory("Memory", listOf(
            i("Memory", "mmap_address_high", l10n("High mmap address", "Indirizzo mmap alto"), 8, 2, 63),
            b("Memory", "scribble_heap", l10n("Fill heap with garbage values", "Riempi heap con valori spazzatura"), false),
            b("Memory", "protect_zero", l10n("Protect zero page", "Proteggi pagina zero"), true),
            b("Memory", "writable_executable_memory", l10n("Writable and executable memory", "Memoria scrivibile ed eseguibile"), true),
            b("Memory", "protect_on_release", l10n("Protect on release", "Proteggi alla liberazione"), false),
            b("Memory", "ignore_offset_for_ranged_allocations", l10n("Ignore offset for ranged allocations", "Ignora offset per allocazioni con intervallo"), false),
        )),

        SettingsCategory("XConfig", listOf(
            l("Console", "user_language", l10n("User language", "Lingua utente"), "1",
                "1" to "en", "2" to "ja", "3" to "de", "4" to "fr", "5" to "es", "6" to "it",
                "7" to "ko", "8" to "zh", "9" to "pt", "11" to "pl", "12" to "ru", "13" to "sv",
                "14" to "tr", "15" to "nb", "16" to "nl", "17" to "zh"), // value 10 skipped; 8 & 17 both zh
            l("Console", "user_country", l10n("User country", "Paese utente"), "103", *userCountryOptions()),
        )),

        SettingsCategory("Display", listOf(
            b("Display", "present_letterbox", l10n("Letterbox on present", "Letterbox in presentazione"), true),
            b("Display", "postprocess_dither", l10n("Post-process dithering", "Dithering di post-elaborazione"), true),
            l("Display", "postprocess_scaling_and_sharpening", l10n("Scaling and sharpening", "Scala e nitidezza"), "",
                "bilinear" to l10n("Bilinear", "Bilineare"), "cas" to "CAS", "fsr" to "FSR"),  // "" => bilinear (no selection)
            b("Display", "present_render_pass_clear", l10n("Render pass clear on present", "Pulizia render pass in presentazione"), true),
            l("Display", "postprocess_antialiasing", "Antialiasing", "",
                "none" to l10n("None", "Nessuno"), "fxaa" to "FXAA", "fxaa_extreme" to l10n("Extreme FXAA", "FXAA estremo")), // "" => none
            // host_present_from_non_ui_thread intentionally NOT exposed: forced true on Android
            // (false black-screens the app), so there is no valid user choice to make.
            b("Display", "show_debug_overlay", l10n("Show debug overlay", "Mostra overlay di debug"), false),
            l("Display", "frame_generation", l10n("Frame generation (60 fps)", "Generazione di fotogrammi (60 fps)"), "0",
                "0" to l10n("Off", "Disattivata"), "2" to l10n("On", "Attivata")),
        )),

        SettingsCategory("GPU", listOf(
            // A dropdown, not a slider: only a few values are meaningful, and "unlimited"
            // needs to be an explicit choice rather than the bottom of a range.
            l("GPU", "framerate_limit", l10n("Frame rate limit", "Limite frame rate"), "60",
                "60" to "60 FPS", "30" to "30 FPS", "45" to "45 FPS",
                "90" to "90 FPS", "120" to "120 FPS", "0" to l10n("Unlimited", "Illimitato")),
            b("GPU", "guest_display_refresh_cap", l10n("Limit game display refresh (VSync)", "Limita aggiornamento display del gioco (VSync)"), true),
            // Emulator-wide: the presenting guest thread sleeps up to its frame slot instead of
            // spinning on the command ring while the limiter stalls the GPU thread.
            b("Video", "guest_swap_pacing", l10n("Heat: sleep, don't spin, at the frame limit", "Calore: dormi, non girare a vuoto, al limite di frame"), true),
            b("GPU", "framerate_limit_adaptive", l10n("Heat: steady 30 FPS when 60 can't be held", "Calore: 30 FPS stabili quando 60 non si regge"), true),
            i("Kernel", "guest_timebase_spin_park_us", l10n("Heat: pause for waiting game threads (µs, 0 = off)", "Calore: pausa per i thread del gioco in attesa (µs, 0 = disattivato)"), 100, 0, 1000),
            i("Kernel", "guest_poll_spin_park_us", l10n("Heat: pause for polling game threads (µs, 0 = off)", "Calore: pausa per i thread del gioco che controllano (µs, 0 = disattivato)"), 100, 0, 1000),
            i("Kernel", "guest_poll_spin_tight_gap_us", l10n("Heat: detect tight polling loops (µs, 0 = off)", "Calore: riconosci i cicli di controllo stretti (µs, 0 = disattivato)"), 12, 0, 60),
            b("Video", "guest_spin_park_adaptive", l10n("Heat: adjust pauses to each frame's free time", "Calore: regola le pause sul tempo libero di ogni fotogramma"), true),
            i("Kernel", "guest_threads_placement", l10n("Heat: light threads on low-power cores (0 = off)", "Calore: thread leggeri sui core a basso consumo (0 = disattivato)"), 1, 0, 2),
            b("CPU", "pulse_event_skip_without_waiters", l10n("Speed: event pulses don't block the game thread", "Velocità: gli impulsi di evento non bloccano il thread del gioco"), true),
            i("CPU", "global_mutex_spin_us", l10n("Speed: short busy-wait on kernel lock before sleeping (µs)", "Velocità: breve attesa attiva sul lock del kernel prima di dormire (µs)"), 40, 0, 200),
            i("Kernel", "guest_scheduler_idle_spin_us", l10n("Speed: idle threads stay active if frames are late (µs)", "Velocità: i thread inattivi restano attivi se i fotogrammi sono in ritardo (µs)"), 80, 0, 500),
            b("Kernel", "wait_poll_fast_path", l10n("Speed: lock-free answer to event polls", "Velocità: risposta senza lock per i controlli di evento"), true),
            i("GPU", "gpu_stall_spin_us", l10n("Speed: GPU thread stays active if frames are late (µs)", "Velocità: il thread GPU resta attivo se i fotogrammi sono in ritardo (µs)"), 80, 0, 500),
            i("Kernel", "guest_spin_sample_park_us", l10n("Heat: pause for threads spinning in place (max µs, 0 = off)", "Calore: pausa per i thread che girano a vuoto sul posto (µs massimi, 0 = disattivato)"), 2000, 0, 8000),
            b("CPU", "a64_loop_register_pinning", l10n("Speed: loop counters and pointers stay in CPU registers", "Velocità: contatori e puntatori dei cicli restano nei registri della CPU"), true),
            b("CPU", "a64_single_precision_narrowing", l10n("Speed: single-precision math without conversions", "Velocità: calcoli in precisione singola senza conversioni"), true),
            b("CPU", "a64_single_precision_across_blocks", l10n("Speed: single-precision math across branches and loops", "Velocità: calcoli in precisione singola anche tra salti e cicli"), true),
            b("CPU", "preempt_check_skip_leaf_entry", l10n("Speed: no thread-switch check on entry to small leaf functions", "Velocità: nessun controllo di cambio thread all'ingresso delle piccole funzioni foglia"), true),
            b("CPU", "simplify_rotate_mask_word_bits", l10n("Speed: no leftover masks after bit-field operations", "Velocità: nessuna maschera residua dopo le operazioni sui campi di bit"), true),
            b("CPU", "a64_sink_compare_to_branch", l10n("Speed: compare and branch emitted as a pair", "Velocità: confronto e salto emessi come una coppia"), true),
            b("CPU", "a64_float_memory_domain", l10n("Speed: floating-point loads and stores stay in vector registers", "Velocità: load e store in virgola mobile restano nei registri vettoriali"), true),
            b("CPU", "a64_float_memory_domain_doubles", l10n("Use vector memory operations for double precision too", "Usa operazioni vettoriali sulla memoria anche in doppia precisione"), false),
            b("a64", "a64_shared_cold_paths", l10n("Speed: shared routines for rare paths in translated code", "Velocità: routine condivise per i percorsi rari nel codice tradotto"), true),
            b("a64", "a64_indirection_bases_in_context", l10n("Speed: shorter indirect calls between game functions", "Velocità: chiamate indirette più corte tra le funzioni del gioco"), true),
            b("a64", "a64_near_bit_branches", l10n("Speed: direct bit-test branches in small functions", "Velocità: salti diretti di test sui bit nelle funzioni piccole"), true),
            b("a64", "a64_call_return_address_in_register", l10n("Speed: calls between game functions keep the return address in a register", "Velocità: le chiamate tra funzioni del gioco tengono l'indirizzo di ritorno in un registro"), true),
            b("CPU", "a64_longjmp_sync_at_resolve", l10n("Speed: no long-jump check after each call", "Velocità: nessun controllo di long-jump dopo ogni chiamata"), true),
            b("Video", "guest_loading_ignores_streaming", l10n("Heat: streaming during gameplay is not treated as loading", "Calore: lo streaming durante il gioco non viene trattato come caricamento"), true),
            b("Kernel", "savestate_enable", l10n("Save states (from next launch)", "Stati istantanei (dal prossimo avvio)"), false),
            b("GPU", "store_shaders", l10n("Save shaders", "Salva shader"), true),
            b("GPU", "resolve_resolution_scale_fill_half_pixel_offset", l10n("Resolve scale: fill half-pixel offset", "Scala di resolve: colma offset di mezzo pixel"), true),
            // uma = no copy, the CPU reads host-mapped shared memory directly; the only mode
            // that works on Adreno, which cannot import guest RAM. Retired some/full parse as uma.
            l("GPU", "readback_resolve", l10n("Resolve readback", "Readback dei resolve"), "uma",
                "uma" to l10n("UMA (direct mapping, no copy)", "UMA (mappa diretta, senza copia)"),
                "fast" to l10n("Fast (copies resolves read by the CPU)", "Veloce (copia i resolve letti dalla CPU)"), "all" to l10n("All (copies every resolve)", "Tutti (copia ogni resolve)"),
                "none" to l10n("None (off)", "Nessuno (disattivato)")),
            // 'fake' fabricates a result (fastest; some effects may look slightly wrong);
            // 'fast'/'fast-alt' issue async queries without stalling; 'strict' stalls the
            // command thread until the GPU answers. Live (SetZPDMode) -- no relaunch needed.
            l("GPU", "occlusion_query", l10n("Occlusion query mode", "Modalità query di occlusione"), "fast",
                "fake" to l10n("Fake (no GPU queries, fastest)", "Finta (nessuna query GPU, la più veloce)"), "fast" to l10n("Fast (asynchronous query, no stall)", "Veloce (query asincrona, senza stallo)"),
                "fast-alt" to l10n("Fast-alt (asynchronous, exact zeros)", "Veloce-alt (asincrona, zeri precisi)"), "strict" to l10n("Strict (real query, with stall)", "Rigorosa (query reale, con stallo)")),
            // Mid-frame command-buffer split: end+submit every N draws so the GPU overlaps
            // rendering with CPU command-building instead of idling until swap. 0 = off;
            // too-small values hurt tiled GPUs.
            i("GPU", "vulkan_mid_frame_submission_draws", l10n("Mid-frame submission (draws, 0 = off)", "Invio a metà fotogramma (draw, 0 = disattivato)"), 0, 0, 4096),
            b("GPU", "snorm16_render_target_full_range", l10n("snorm16 render targets, full range", "Render target snorm16, intervallo pieno"), true),
            // min <= the real TOML default (96); a higher floor would silently coerce the default up.
            i("GPU", "texture_cache_memory_limit_soft", l10n("Texture cache soft limit (MB)", "Limite morbido cache texture (MB)"), 96, 32, 4096),
            b("GPU", "native_2x_msaa", l10n("Native 2x MSAA", "MSAA 2x nativo"), true),
            l("GPU", "render_target_path", l10n("Render target path", "Percorso render target"), "performance",
                "performance" to l10n("Performance", "Prestazioni"), "accuracy" to l10n("Accuracy", "Precisione")),
            b("GPU", "half_pixel_offset", l10n("Half-pixel offset", "Offset di mezzo pixel"), true),
            // Upstream accuracy features that only a few titles need but cost shader performance
            // in every title. Off = pre-upstream behaviour.
            b("GPU", "texture_gradient_exp_bias", l10n("Accuracy: per-axis gradient LOD bias", "Precisione: bias LOD del gradiente per asse"), false),
            b("GPU", "texture_integer_num_format", l10n("Accuracy: integer num_format texture scaling", "Precisione: scala delle texture con num_format intero"), false),
            b("GPU", "accurate_resolve_number_formats", l10n("Accuracy: resolve number formats and gamma", "Precisione: formati numerici e gamma dei resolve"), false),
            b("GPU", "resolve_copy_dest_number_packing", l10n("Accuracy: full resolve destination packing", "Precisione: impacchettamento delle destinazioni di resolve completo"), false),
            b("GPU", "log_ringbuffer_kickoff_initiator_bts", l10n("Log ringbuffer kickoff BT initiators", "Registra gli initiator BT del kickoff del ringbuffer"), false),
            i("GPU", "texture_cache_memory_limit_hard", l10n("Texture cache hard limit (MB)", "Limite rigido cache texture (MB)"), 256, 128, 4096),
            i("GPU", "gpu_stall_spin_iterations", l10n("GPU stall busy-wait iterations", "Iterazioni di attesa attiva in stallo GPU"), 0, 0, 512),
            b("GPU", "gpu_allow_invalid_fetch_constants", l10n("Allow invalid fetch constants", "Consenti costanti di fetch non valide"), true),
            b("GPU", "log_guest_driven_gpu_register_written_values", l10n("Log game GPU register writes", "Registra le scritture dei registri GPU del gioco"), false),
            b("GPU", "trace_gpu_stream", l10n("Trace GPU stream", "Traccia lo stream GPU"), false),
            b("GPU", "force_convert_quad_lists_to_triangle_lists", l10n("Convert quad lists to triangle lists", "Converti liste di quad in liste di triangoli"), false),
            b("GPU", "execute_unclipped_draw_vs_on_cpu_with_scissor", l10n("Vertex shaders of unclipped draws on CPU (scissor)", "Vertex shader dei draw non ritagliati su CPU (scissor)"), false),
            b("GPU", "mrt_edram_used_range_clamp_to_min", l10n("Minimum used EDRAM range clamp (MRT)", "Limite minimo dell'intervallo EDRAM usato (MRT)"), true),
            b("GPU", "execute_unclipped_draw_vs_on_cpu", l10n("Vertex shaders of unclipped draws on CPU", "Vertex shader dei draw non ritagliati su CPU"), true),
            b("GPU", "readback_memexport", l10n("Memexport readback", "Readback di memexport"), false),
            b("GPU", "force_convert_triangle_fans_to_lists", l10n("Convert triangle fans to lists", "Converti triangle fan in liste"), false),
            b("GPU", "non_seamless_cube_map", l10n("Non-seamless cube maps", "Cube map non continua"), true),
            b("GPU", "depth_float24_round", l10n("Float24 depth rounding", "Arrotondamento profondità float24"), false),
            b("GPU", "clear_memory_page_state", l10n("Clear memory page state", "Pulisci stato delle pagine di memoria"), false),
            b("GPU", "depth_transfer_not_equal_test", l10n("Depth transfer inequality test", "Test di disuguaglianza nel trasferimento di profondità"), true),
            b("GPU", "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend", l10n("Vertex shaders of unclipped draws on CPU (PSI backend)", "Vertex shader dei draw non ritagliati su CPU (backend PSI)"), true),
            l("GPU", "draw_resolution_scale_x", l10n("Resolution scale, width (real)", "Scala di risoluzione, larghezza (reale)"), "1",
                "1" to "1x", "2" to "2x", "3" to "3x"),
            l("GPU", "draw_resolution_scale_y", l10n("Resolution scale, height (real)", "Scala di risoluzione, altezza (reale)"), "1",
                "1" to "1x", "2" to "2x", "3" to "3x"),
            b("GPU", "draw_resolution_scaled_texture_offsets", l10n("Texture offsets scaled with resolution", "Offset delle texture scalati con la risoluzione"), true),
            l("GPU", "gpu", "Backend GPU", "vulkan",
                "vulkan" to "vulkan", "null" to "null"),
            b("GPU", "depth_float24_convert_in_pixel_shader", l10n("Convert float24 depth in pixel shader", "Converti profondità float24 nello shader dei pixel"), false),
            b("GPU", "value_convert_7e3_8888_reuse", "value_convert_7e3_8888_reuse", true),
            b("GPU", "rt_cache_skip_transfers_for_full_clears", l10n("Skip EDRAM copies before full clears", "Salta copie EDRAM prima delle pulizie complete"), true),
            b("GPU", "rt_cache_dont_care_load_for_full_clears", l10n("Don't reload targets before full clears", "Non ricaricare i target prima delle pulizie complete"), true),
            l("GPU", "anisotropic_override", l10n("Anisotropic filtering", "Filtraggio anisotropico"), "-1",
                "-1" to l10n("Game default", "Predefinito del gioco"), "0" to l10n("Off", "Disattivato"), "1" to "1x", "2" to "2x",
                "3" to "4x", "4" to "8x", "5" to "16x"),
            l("GPU", "anisotropic_cap", l10n("GPU load: anisotropic filtering cap", "Carico GPU: limite del filtraggio anisotropico"), "3",
                "2" to "2x", "3" to "4x", "4" to "8x", "5" to l10n("No limit", "Nessun limite")),
            b("GPU", "rt_cache_edram_stencil_bit_map", l10n("GPU load: stencil bit tracking per tile", "Carico GPU: tracciamento dei bit stencil per tile"), true),
        )),

        SettingsCategory("CPU", listOf(
            b("CPU", "validate_hir", l10n("Validate HIR", "Valida HIR"), false),
            b("CPU", "trace_function_references", l10n("Trace function references", "Traccia riferimenti alle funzioni"), false),
            b("CPU", "trace_function_coverage", l10n("Trace function coverage", "Traccia copertura delle funzioni"), false),
            b("CPU", "store_all_context_values", l10n("Store all context values", "Salva tutti i valori di contesto"), false),
            b("CPU", "break_condition_truncate", l10n("Truncate breakpoint condition", "Tronca la condizione di breakpoint"), true),
            b("CPU", "clock_source_raw", l10n("Raw clock source", "Sorgente di clock grezza"), false),
            b("CPU", "break_on_unimplemented_instructions", l10n("Break on unimplemented instructions", "Breakpoint su istruzioni non implementate"), true),
            b("CPU", "break_on_start", l10n("Break on start", "Breakpoint all'avvio"), false),
            b("CPU", "inline_mmio_access", l10n("Inline MMIO access", "Accesso MMIO inline"), true),
            b("CPU", "clock_no_scaling", l10n("Unscaled clock", "Clock senza scala"), false),
            b("CPU", "disable_context_promotion", l10n("Disable context promotion", "Disattiva promozione del contesto"), false),
            // Elides real guest wall time; turn off per-title if a game depends on it.
            b("CPU", "collapse_memory_delay_spins", l10n("Collapse memory wait countdowns", "Comprimi i conti alla rovescia delle attese in memoria"), true),
            b("CPU", "log_delay_collapse_rejects", l10n("Log wait-collapse rejects", "Registra gli scarti della compressione delle attese"), false),
            b("CPU", "log_safepoint_pc", l10n("Log safepoint addresses (stall diagnosis)", "Registra gli indirizzi dei safepoint (diagnosi dei blocchi)"), false),
        )),

        SettingsCategory("Logging", listOf(
            b("Logging", "log_string_format_kernel_calls", l10n("Log string formatting kernel calls", "Registra chiamate kernel di formattazione stringhe"), false),
            l("Logging", "log_level", l10n("Log level", "Livello di log"), "2",
                "0" to l10n("Error", "Errore"), "1" to l10n("Warning", "Avviso"), "2" to "Info", "3" to "Debug"),
            b("Logging", "flush_log", l10n("Write log to disk", "Scrivi il log su disco"), true),
            i("Logging", "log_sessions_keep", l10n("Archived log sessions to keep", "Sessioni di log archiviate da conservare"), 4, 1, 16),
            Action("Logging", "dump_session_logs", l10n("Export session logs to Downloads", "Esporta i log della sessione in Download"), "", desc("dump_session_logs")),
        )),

        SettingsCategory("Content", listOf(
            l("Content", "license_mask", l10n("License mask", "Maschera licenze"), "0",
                "0" to l10n("None", "Nessuna"), "1" to l10n("Full version", "Versione completa"), "-1" to l10n("All", "Tutte")),
        )),

        SettingsCategory("General", listOf(
            i("General", "time_scalar", l10n("Time multiplier", "Moltiplicatore del tempo"), 1, 1, 8),
            b("General", "allow_plugins", l10n("Allow plugins", "Consenti plugin"), false),
            b("General", "apply_patches", l10n("Apply patches", "Applica patch"), true),
        )),

        SettingsCategory("APU", listOf(
            b("APU", "apu_pump_topup", l10n("Audio buffer top-up", "Riempimento del buffer audio"), true),
            i("Console", "xmp_default_volume", l10n("Default XMP volume", "Volume predefinito XMP"), 70, 0, 100),
            b("APU", "ffmpeg_verbose", l10n("Verbose FFmpeg", "FFmpeg dettagliato"), false),
            b("APU", "mute", l10n("Mute", "Silenzia"), false),
            i("APU", "apu_max_queued_frames", l10n("Maximum queued frames", "Frame in coda massimi"), 8, 4, 64),
            i("APU", "apu_aaudio_buffer_bursts", l10n("Audio buffer depth", "Profondità del buffer audio"), 4, 2, 8),
            b("APU", "enable_xmp", l10n("Enable XMP", "Abilita XMP"), true),
            l("APU", "xma_decoder", l10n("XMA decoder", "Decoder XMA"), "new",
                "new" to l10n("New", "Nuovo"), "old" to l10n("Old", "Vecchio"), "master" to "master", "fake" to l10n("Fake", "Finto")),
            b("APU", "use_dedicated_xma_thread", l10n("Use dedicated XMA thread", "Usa thread XMA dedicato"), true),
            l("APU", "apu", "Backend APU", "aaudio",
                "nop" to "Nessuno", "aaudio" to "aaudio", "opensles" to "opensles"),
        )),
    )

    val allSettings: List<Setting> = categories.flatMap { it.settings }
    val byKey: Map<String, Setting> = allSettings.associateBy { it.key }
}

// user_country: values 1..109 with 17 and 94 skipped (107 entries). Order matches
// arrays.xml (es_arr_xconfig_user_country). Default 103 = US.
private val USER_COUNTRY_ISO: List<String> = listOf(
    "AE","AL","AM","AR","AT","AU","AZ","BE","BG","BH","BN","BO","BR","BY","BZ","CA",
    "CH","CL","CN","CO","CR","CZ","DE","DK","DO","DZ","EC","EE","EG","ES","FI","FO",
    "FR","GB","GE","GR","GT","HK","HN","HR","HU","ID","IE","IL","IN","IQ","IR","IS",
    "IT","JM","JO","JP","KE","KG","KR","KW","KZ","LB","LI","LT","LU","LV","LY","MA",
    "MC","MK","MN","MO","MV","MX","MY","NI","NL","NO","NZ","OM","PA","PE","PH","PK",
    "PL","PR","PT","PY","QA","RO","RU","SA","SE","SG","SI","SK","SV","SY","TH","TN",
    "TR","TT","TW","UA","US","UY","UZ","VE","VN","YE","ZA",
)

private fun userCountryOptions(): Array<Pair<String, String>> {
    val out = ArrayList<Pair<String, String>>(107)
    var iso = 0
    var value = 1
    while (iso < USER_COUNTRY_ISO.size) {
        if (value == 17 || value == 94) { value++; continue }
        out += value.toString() to USER_COUNTRY_ISO[iso]
        value++; iso++
    }
    return out.toTypedArray()
}
