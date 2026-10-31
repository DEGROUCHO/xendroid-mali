package xendroid.compose.ui.pause

import xendroid.compose.l10n

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** One saved state of the running game: the state file, its picture (if any), when it
 *  was saved and its size. */
data class SavestateEntry(val file: File, val thumbnail: File?, val savedAt: Long, val bytes: Long)

/**
 * Picks the saved state to resume from, newest first, each with the picture taken when it
 * was saved. An in-window Surface like the other in-game panels (a Dialog would trip the
 * focus-loss pause). [selected] is driven by the host for the controller: rows 0 until
 * entries.size, then Cancel. Y (or the bin icon) arms a delete, a second press confirms;
 * [deleteArmed] is the armed row or -1.
 */
@Composable
fun SavestatePickerPanel(
    entries: List<SavestateEntry>,
    selected: Int,
    deleteArmed: Int,
    onLoad: (Int) -> Unit,
    onDelete: (Int) -> Unit,
    onSelect: (Int) -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val dateFormat = SimpleDateFormat("EEE d MMM yyyy, HH:mm", xendroid.compose.UiLanguage.locale)
    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.6f))
            .pointerInput(Unit) { awaitPointerEventScope { while (true) awaitPointerEvent() } },
        contentAlignment = Alignment.Center,
    ) {
        val compact = maxHeight < 400.dp
        Surface(
            modifier = Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth()
                .heightIn(max = maxHeight - if (compact) 16.dp else 48.dp)
                .padding(horizontal = if (compact) 8.dp else 24.dp),
            shape = MaterialTheme.shapes.large,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.padding(if (compact) 12.dp else 20.dp)) {
                Text(l10n("Resume from a saved state", "Riprendi da uno stato salvato"), style = MaterialTheme.typography.titleMedium)
                val totalMb = entries.sumOf { it.bytes } shr 20
                Text(
                    l10n("${entries.size} ${if (entries.size == 1) "state" else "states"} · $totalMb MB  ·  A load, Y delete, B cancel",
                        "${entries.size} ${if (entries.size == 1) "stato" else "stati"} · $totalMb MB  ·  A carica, Y elimina, B annulla"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 2.dp, bottom = 8.dp),
                )
                val listState = rememberLazyListState()
                LaunchedEffect(selected) {
                    if (selected in entries.indices) listState.animateScrollToItem(selected)
                }
                LazyColumn(
                    state = listState,
                    modifier = Modifier.weight(1f, fill = false),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    itemsIndexed(entries, key = { _, e -> e.file.path }) { i, entry ->
                        SavestateRow(
                            entry = entry,
                            date = dateFormat.format(Date(entry.savedAt)),
                            selected = i == selected,
                            deleteArmed = i == deleteArmed,
                            onLoad = { onSelect(i); onLoad(i) },
                            onDelete = { onSelect(i); onDelete(i) },
                        )
                    }
                }
                val cancelSelected = selected == entries.size
                val shape = MaterialTheme.shapes.medium
                Box(
                    Modifier
                        .padding(top = 10.dp)
                        .fillMaxWidth()
                        .heightIn(min = 44.dp)
                        .clip(shape)
                        .background(
                            if (cancelSelected) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.surfaceVariant
                        )
                        .clickable { onCancel() },
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        l10n("Cancel", "Annulla"),
                        color = if (cancelSelected) MaterialTheme.colorScheme.onPrimary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun SavestateRow(
    entry: SavestateEntry,
    date: String,
    selected: Boolean,
    deleteArmed: Boolean,
    onLoad: () -> Unit,
    onDelete: () -> Unit,
) {
    val shape = MaterialTheme.shapes.medium
    Row(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(
                if (selected) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
            )
            .then(
                if (selected) Modifier.border(2.dp, MaterialTheme.colorScheme.primary, shape)
                else Modifier
            )
            .clickable { onLoad() }
            .padding(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(width = 160.dp, height = 90.dp)
                .clip(MaterialTheme.shapes.small)
                .background(Color.Black),
            contentAlignment = Alignment.Center,
        ) {
            if (entry.thumbnail != null) {
                AsyncImage(
                    model = entry.thumbnail,
                    contentDescription = null,
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            } else {
                Icon(Icons.Filled.Refresh, contentDescription = null, tint = Color.Gray)
            }
        }
        Column(
            Modifier
                .weight(1f)
                .padding(horizontal = 12.dp)
        ) {
            Text(
                date.replaceFirstChar { it.titlecase(xendroid.compose.UiLanguage.locale) },
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                if (deleteArmed) l10n("Press Y again (or the trash icon) to delete it", "Premi di nuovo Y (o il cestino) per eliminarlo")
                else "${entry.bytes shr 20} MB",
                style = MaterialTheme.typography.bodySmall,
                color = if (deleteArmed) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        IconButton(onClick = onDelete) {
            Icon(
                Icons.Filled.Delete,
                contentDescription = l10n("Delete", "Elimina"),
                tint = if (deleteArmed) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}
