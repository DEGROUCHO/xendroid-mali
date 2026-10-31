package xendroid.compose.ui.pause

import xendroid.compose.l10n

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.ui.text.style.TextAlign
import xendroid.compose.ui.theme.XdColors
import xendroid.compose.ui.theme.XdType
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * The in-game menu: a tab rail on the left, the selected tab's rows on the right. An
 * in-window Surface rather than a Dialog: a Dialog takes window focus and trips the host's
 * focus-loss pause. [tabIndex] and [selected] are driven by the host because the D-pad
 * arrives as hat axes that never reach a composable; touch reports back through the rows'
 * own callbacks and [onSelectTab].
 */
@Composable
fun QuickMenuPanel(
    tabs: List<QmTab>,
    tabIndex: Int,
    selected: Int,
    gameName: String,
    statusLine: String,
    onSelectTab: (Int) -> Unit,
    onSelectRow: (Int) -> Unit,
    onClose: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val tab = tabs.getOrNull(tabIndex) ?: return
    // A short settle-in so the menu does not just pop over the game.
    val appear = remember { Animatable(0f) }
    LaunchedEffect(Unit) { appear.animateTo(1f, tween(140)) }
    Box(
        modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.62f * appear.value))
            // Swallow taps meant for the game surface underneath.
            .pointerInput(Unit) { awaitPointerEventScope { while (true) awaitPointerEvent() } },
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = Modifier
                .widthIn(max = 760.dp)
                .fillMaxWidth()
                .fillMaxHeight()
                .padding(horizontal = 16.dp, vertical = 10.dp)
                .graphicsLayer {
                    alpha = appear.value
                    val k = 0.96f + 0.04f * appear.value
                    scaleX = k; scaleY = k
                },
            shape = RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surfaceContainer,
            contentColor = MaterialTheme.colorScheme.onSurface,
            border = androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        ) {
            Column {
                Header(gameName, statusLine, onClose)
                Row(Modifier.fillMaxSize()) {
                    TabRail(tabs, tabIndex, onSelectTab)
                    Box(
                        Modifier
                            .fillMaxHeight()
                            .width(1.dp)
                            .background(MaterialTheme.colorScheme.outlineVariant)
                    )
                    val listState = rememberLazyListState()
                    // Controller navigation: keep the highlighted row on screen.
                    LaunchedEffect(tabIndex, selected) {
                        if (selected in tab.items.indices) {
                            // Bring the section heading along when the row is the first under it.
                            val top = if (selected > 0 && tab.items[selected - 1] is QmSection) selected - 1
                            else selected
                            listState.animateScrollToItem(top)
                        }
                    }
                    LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize().padding(horizontal = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                        contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp),
                    ) {
                        itemsIndexed(tab.items) { i, item ->
                            QmRow(item, focused = i == selected, onFocus = { onSelectRow(i) })
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Header(gameName: String, statusLine: String, onClose: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .padding(start = 18.dp, end = 6.dp)
            .height(46.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .clip(RoundedCornerShape(6.dp))
                .background(MaterialTheme.colorScheme.primaryContainer)
                .padding(horizontal = 8.dp, vertical = 2.dp),
        ) {
            Text(l10n("PAUSED", "IN PAUSA"), style = XdType.label, color = MaterialTheme.colorScheme.onPrimaryContainer)
        }
        Text(
            gameName.ifBlank { "XenDroid" },
            style = XdType.title,
            maxLines = 1, overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = 12.dp).weight(1f),
        )
        if (statusLine.isNotEmpty()) {
            Text(
                statusLine,
                style = XdType.caption.copy(fontFeatureSettings = "tnum"),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                modifier = Modifier.padding(horizontal = 10.dp),
            )
        }
        Box(
            Modifier.size(40.dp).clip(RoundedCornerShape(20.dp)).clickable(onClick = onClose),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Filled.Close, contentDescription = "Resume", modifier = Modifier.size(20.dp))
        }
    }
}

@Composable
private fun TabRail(tabs: List<QmTab>, tabIndex: Int, onSelectTab: (Int) -> Unit) {
    Column(
        Modifier.width(168.dp).fillMaxHeight().padding(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        tabs.forEachIndexed { i, t ->
            val on = i == tabIndex
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 44.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (on) MaterialTheme.colorScheme.primaryContainer else Color.Transparent)
                    .clickable { onSelectTab(i) }
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val tint = if (on) MaterialTheme.colorScheme.onPrimaryContainer
                else MaterialTheme.colorScheme.onSurfaceVariant
                Icon(t.icon, contentDescription = null, tint = tint, modifier = Modifier.size(20.dp))
                Text(
                    t.title, color = tint,
                    style = XdType.row.copy(fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium),
                    modifier = Modifier.padding(start = 10.dp),
                )
            }
        }
        Spacer(Modifier.weight(1f))
        Text(
            "LB / RB  change tab\nD-pad  move and adjust\nA  select    B  resume",
            style = XdType.caption,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
            modifier = Modifier.padding(start = 6.dp, bottom = 2.dp),
        )
    }
}

@Composable
private fun QmRow(item: QmItem, focused: Boolean, onFocus: () -> Unit) {
    val scheme = MaterialTheme.colorScheme
    if (item is QmSection) {
        Text(
            item.title.uppercase(), style = XdType.label, color = scheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 6.dp, top = 8.dp, bottom = 2.dp),
        )
        return
    }
    val shape = RoundedCornerShape(12.dp)
    val primary = item is QmAction && item.primary
    val danger = item is QmAction && item.danger
    val note = item is QmNote
    // One meaning per colour: green = on / go, red = destructive, blue = information.
    val accent = when {
        danger -> XdColors.bad
        note -> XdColors.info
        item is QmToggle && !item.checked -> scheme.onSurfaceVariant
        else -> XdColors.good
    }
    val container = when {
        primary -> scheme.primary
        note -> Color.Transparent
        focused -> scheme.surfaceContainerHighest
        else -> scheme.surfaceContainerLow
    }
    val titleColor = when {
        primary -> scheme.onPrimary
        danger -> XdColors.bad
        else -> scheme.onSurface
    }
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clip(shape)
            .background(container)
            // The highlight is an outline as well as a tint, so it never depends on colour alone.
            .border(
                if (focused) 2.dp else 1.dp,
                when {
                    focused -> scheme.primary
                    note -> Color.Transparent
                    else -> scheme.outlineVariant.copy(alpha = 0.6f)
                },
                shape,
            )
            .clickable(enabled = item !is QmSlider && !note) { onFocus(); qmActivate(item) }
            .padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Leading icon: same box on every row, so titles start on one vertical line.
        Box(
            Modifier
                .size(36.dp)
                .background(
                    if (primary) scheme.onPrimary.copy(alpha = 0.14f) else accent.copy(alpha = 0.14f),
                    RoundedCornerShape(10.dp)),
            contentAlignment = Alignment.Center,
        ) {
            item.icon?.let {
                Icon(it, contentDescription = null,
                    tint = if (primary) scheme.onPrimary else accent, modifier = Modifier.size(20.dp))
            }
        }
        Column(Modifier.weight(1f).padding(start = 12.dp, end = 8.dp)) {
            Text(item.title, color = titleColor, style = XdType.row,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            item.subtitle?.let {
                Text(
                    it,
                    color = if (primary) scheme.onPrimary.copy(alpha = 0.8f) else scheme.onSurfaceVariant,
                    style = XdType.caption, maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
        }
        // Trailing control: sliders share one width and every value sits in one right-aligned
        // column, so a tab reads as a table.
        when (item) {
            is QmToggle -> {
                if (item.nextLaunch) {
                    Text(l10n("NEXT LAUNCH", "DAL PROSSIMO AVVIO"), style = XdType.label, color = XdColors.caution,
                        maxLines = 1, modifier = Modifier.padding(end = 10.dp))
                }
                Switch(
                    checked = item.checked,
                    onCheckedChange = { onFocus(); item.onToggle(it) },
                )
            }
            is QmSlider -> {
                Slider(
                    value = item.value.coerceIn(item.min, item.max),
                    valueRange = item.min..item.max,
                    onValueChange = { onFocus(); item.onChange(it) },
                    modifier = Modifier.width(190.dp),
                )
                Text(item.valueLabel, color = scheme.onSurface, style = XdType.value,
                    textAlign = TextAlign.End, maxLines = 1, modifier = Modifier.width(62.dp))
            }
            is QmChoice -> {
                if (item.nextLaunch) {
                    Text("NEXT LAUNCH", style = XdType.label, color = XdColors.caution,
                        modifier = Modifier.padding(end = 10.dp))
                }
                Text(item.valueLabel, color = scheme.onSurface, style = XdType.value,
                    textAlign = TextAlign.End, maxLines = 1)
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = null,
                    tint = scheme.onSurfaceVariant, modifier = Modifier.size(20.dp))
            }
            is QmAction -> if (!primary) {
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = null,
                    tint = if (danger) XdColors.bad else scheme.onSurfaceVariant,
                    modifier = Modifier.size(20.dp))
            }
            is QmSection, is QmNote -> Unit
        }
    }
}
