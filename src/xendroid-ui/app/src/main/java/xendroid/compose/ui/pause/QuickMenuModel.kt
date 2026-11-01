package xendroid.compose.ui.pause

import androidx.compose.ui.graphics.vector.ImageVector
import kotlin.math.roundToInt

/** One row of the in-game menu. The host builds these from its state; the panel only draws
 *  them, and the controller path (D-pad, A, left/right) acts on the same objects. */
sealed interface QmItem {
    val title: String
    val subtitle: String?
    val icon: ImageVector?
}

/** A plain action row (Resume, Screenshot, Quit...). */
data class QmAction(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
    val primary: Boolean = false,
    val danger: Boolean = false,
    val onClick: () -> Unit,
) : QmItem

data class QmToggle(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
    val checked: Boolean,
    /** The switch was changed but only takes effect at the next launch: shown as an amber tag. */
    val nextLaunch: Boolean = false,
    val onToggle: (Boolean) -> Unit,
) : QmItem

/** A value picked along a range; [step] is what one left/right press moves it by. */
data class QmSlider(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
    val value: Float,
    val min: Float,
    val max: Float,
    val step: Float,
    val valueLabel: String,
    val onChange: (Float) -> Unit,
) : QmItem

/** A value picked from a short list: activating it moves to the next one. */
data class QmChoice(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
    val valueLabel: String,
    /** The choice only takes effect at the next launch: shown as an amber tag. */
    val nextLaunch: Boolean = false,
    val onNext: () -> Unit,
) : QmItem

/** A few options side by side (the frame-rate limit, the statistics box): tap one, or move
 *  with left/right; activating the row moves to the next. [subtitle] explains the selected one. */
data class QmSegmented(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
    val options: List<String>,
    val selected: Int,
    val onSelect: (Int) -> Unit,
) : QmItem

/** A group heading inside a tab. Not selectable. */
data class QmSection(override val title: String) : QmItem {
    override val subtitle: String? get() = null
    override val icon: ImageVector? get() = null
}

/** A line of guidance (where to find something, why a figure is missing). Not selectable. */
data class QmNote(
    override val title: String,
    override val subtitle: String? = null,
    override val icon: ImageVector? = null,
) : QmItem

/** Rows the highlight can land on. */
fun qmSelectable(item: QmItem): Boolean = item !is QmSection && item !is QmNote

/** Pure: the next selectable index from [index] going [delta] (+-1), wrapping; [index] itself
 *  when nothing is selectable. */
fun qmMoveSelection(items: List<QmItem>, index: Int, delta: Int): Int {
    if (items.none(::qmSelectable)) return index
    var i = index
    repeat(items.size) {
        i = qmWrap(i, delta, items.size)
        if (qmSelectable(items[i])) return i
    }
    return index
}

/** Pure: the first selectable index of a tab (0 when there is none). */
fun qmFirstSelectable(items: List<QmItem>): Int = items.indexOfFirst(::qmSelectable).coerceAtLeast(0)

data class QmTab(val title: String, val icon: ImageVector, val items: List<QmItem>)

/** A / Enter / tap on a row. */
fun qmActivate(item: QmItem) = when (item) {
    is QmAction -> item.onClick()
    is QmToggle -> item.onToggle(!item.checked)
    is QmChoice -> item.onNext()
    is QmSegmented -> if (item.options.isNotEmpty()) item.onSelect((item.selected + 1) % item.options.size) else Unit
    is QmSlider -> Unit                       // a slider only moves with left/right or a drag
    is QmSection, is QmNote -> Unit
}

/** Pure: the slider value after one left (-1) / right (+1) press, snapped to its step grid. */
fun qmSliderStep(value: Float, min: Float, max: Float, step: Float, dir: Int): Float {
    if (step <= 0f || max <= min) return value.coerceIn(min, max)
    val steps = ((value - min) / step).roundToInt() + dir
    return (min + steps * step).coerceIn(min, max)
}

/** Left/right on a row: true when the row used it (a slider or a set of options), false to
 *  let it switch tabs. */
fun qmAdjust(item: QmItem, dir: Int): Boolean {
    if (item is QmSegmented) {
        val next = (item.selected + dir).coerceIn(0, item.options.lastIndex.coerceAtLeast(0))
        if (next != item.selected) item.onSelect(next)
        return true
    }
    if (item !is QmSlider) return false
    val next = qmSliderStep(item.value, item.min, item.max, item.step, dir)
    if (next != item.value) item.onChange(next)
    return true
}

/** Pure: index after moving by [delta] in a list of [count], wrapping at both ends. */
fun qmWrap(index: Int, delta: Int, count: Int): Int =
    if (count <= 0) 0 else (((index + delta) % count) + count) % count
