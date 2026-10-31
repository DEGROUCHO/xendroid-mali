package xendroid.compose

/** Configured PRESENT target; never derive this from transient measured FPS. */
internal object DisplayRatePolicy {
    fun target(frameCap: Int, frameGeneration: Int, generatedRate: Int): Float {
        if (frameCap <= 0) return 0f // unlimited / invalid: leave selection to Android
        return if (frameGeneration >= 2) generatedRate.coerceIn(10, 240).toFloat()
            else frameCap.toFloat()
    }

    /** The optional native reader is authoritative; zero explicitly clears the vote. */
    fun resolve(live: Double?, boot: Float): Float =
        live?.takeIf { it.isFinite() && it >= 0.0 && it <= Float.MAX_VALUE }
            ?.toFloat() ?: boot
}
