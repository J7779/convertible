package app.convertible.ui.screens

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import app.convertible.R
import app.convertible.model.Catalogue
import app.convertible.model.TargetInfo
import app.convertible.model.TargetRanking
import app.convertible.state.QueueItem
import app.convertible.ui.components.FormatBadge
import app.convertible.ui.components.Icon
import app.convertible.ui.components.IconButton
import app.convertible.ui.components.Icons
import app.convertible.ui.components.Text
import app.convertible.ui.components.TextInput
import app.convertible.ui.components.focusRing
import app.convertible.ui.theme.AppTheme

/**
 * A modal sheet: slides up from the bottom on phones, a centred card on wide screens. Back and the scrim dismiss it.
 */
@Composable
fun Sheet(onDismiss: () -> Unit, tag: String, content: @Composable () -> Unit) {
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        val colors = AppTheme.colors
        val d = AppTheme.dimens
        val motion = AppTheme.motion
        val enter = remember { Animatable(0f) }
        LaunchedEffect(Unit) { enter.animateTo(1f, tween(motion.slow, easing = motion.decelerate)) }
        BoxWithConstraints(
            Modifier
                .fillMaxSize()
                .clickable(remember { MutableInteractionSource() }, null, onClick = onDismiss),
        ) {
            val wide = maxWidth >= 600.dp
            Box(
                Modifier
                    .align(if (wide) Alignment.Center else Alignment.BottomCenter)
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
                    .padding(top = d.space10)
                    .then(if (wide) Modifier.padding(d.space8).widthIn(max = 560.dp).heightIn(max = 720.dp) else Modifier.fillMaxWidth())
                    .graphicsLayer {
                        translationY = (1f - enter.value) * (if (wide) 40.dp.toPx() else size.height * 0.6f)
                        alpha = enter.value
                    }
                    .clip(if (wide) RoundedCornerShape(d.radiusXl) else RoundedCornerShape(topStart = d.radiusXl, topEnd = d.radiusXl))
                    .background(colors.surfaceElevated)
                    .border(1.dp, colors.border, if (wide) RoundedCornerShape(d.radiusXl) else RoundedCornerShape(topStart = d.radiusXl, topEnd = d.radiusXl))
                    .clickable(remember { MutableInteractionSource() }, null) { } // swallow clicks inside the sheet
                    .imePadding()
                    .testTag(tag),
            ) {
                Column {
                    if (!wide) {
                        Box(
                            Modifier
                                .align(Alignment.CenterHorizontally)
                                .padding(top = d.space4)
                                .size(width = 36.dp, height = 4.dp)
                                .clip(RoundedCornerShape(2.dp))
                                .background(colors.borderStrong),
                        )
                    }
                    content()
                }
            }
        }
    }
}

/** Searchable target list grouped by category, from `cvt_targets_json`. */
@Composable
fun TargetPicker(
    title: String,
    targets: List<TargetInfo>,
    sourceCategory: String?,
    catalogue: Catalogue,
    selected: String?,
    onPick: (TargetInfo) -> Unit,
    onDismiss: () -> Unit,
) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    var query by rememberSaveable { mutableStateOf("") }
    Sheet(onDismiss, tag = "target_picker") {
        Column(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom))) {
            Row(Modifier.fillMaxWidth().padding(start = d.space6, end = d.space3, top = d.space4), verticalAlignment = Alignment.CenterVertically) {
                Text(title, AppTheme.type.title3, colors.textPrimary, Modifier.weight(1f).semantics { heading() }, maxLines = 2, overflow = TextOverflow.Ellipsis)
                IconButton(Icons.Close, stringResource(R.string.picker_close), onDismiss)
            }
            TextInput(
                value = query,
                onChange = { query = it },
                placeholder = stringResource(R.string.picker_search),
                leading = Icons.Search,
                modifier = Modifier.padding(horizontal = d.space6, vertical = d.space4).testTag("picker_search"),
                onClear = { query = "" },
                clearLabel = stringResource(R.string.picker_clear_search),
            )
            val sections = remember(targets, query, sourceCategory, catalogue) {
                if (query.isBlank()) TargetRanking.sections(targets, sourceCategory, catalogue)
                else listOf(app.convertible.model.TargetSection(null, TargetRanking.search(targets, query)))
            }
            val empty = sections.all { it.targets.isEmpty() }
            if (empty) {
                Text(
                    stringResource(R.string.picker_no_results, query),
                    AppTheme.type.body,
                    colors.textSecondary,
                    Modifier.padding(d.space8).fillMaxWidth(),
                )
            } else {
                LazyColumn(
                    Modifier.fillMaxWidth().heightIn(max = 560.dp).testTag("picker_list"),
                    contentPadding = PaddingValues(start = d.space4, end = d.space4, bottom = d.space6),
                ) {
                    sections.forEachIndexed { index, section ->
                        if (query.isBlank()) {
                            item(key = "h$index") {
                                Text(
                                    (section.category?.let { categoryName(it) } ?: stringResource(R.string.picker_popular)).uppercase(),
                                    AppTheme.type.overline,
                                    colors.textSecondary,
                                    Modifier.padding(start = d.space3, top = d.space5, bottom = d.space3).semantics { heading() },
                                )
                            }
                        }
                        items(section.targets, key = { "$index-${it.id}" }) { t ->
                            TargetRow(t, t.id == selected) { onPick(t) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun TargetRow(target: TargetInfo, isSelected: Boolean, onClick: () -> Unit) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val interaction = remember { MutableInteractionSource() }
    Row(
        Modifier
            .focusRing(interaction, RoundedCornerShape(d.radiusMd))
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .clip(RoundedCornerShape(d.radiusMd))
            .background(if (isSelected) colors.accentSubtle else androidx.compose.ui.graphics.Color.Transparent)
            .clickable(interaction, androidx.compose.foundation.LocalIndication.current, role = Role.Button, onClick = onClick)
            .semantics { selected = isSelected }
            .padding(horizontal = d.space3, vertical = d.space3)
            .testTag("target_${target.id}"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(116.dp)) { FormatBadge(target.label, target.category, round = false) }
        Spacer(Modifier.width(d.space3))
        Column(Modifier.weight(1f)) {
            Text(target.name, AppTheme.type.body, colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            val ext = target.extensions.joinToString("  ") { ".$it" }
            if (ext.isNotEmpty()) Text(ext, AppTheme.type.caption.copy(fontFamily = AppTheme.type.mono), colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (target.lossy) {
            Text(stringResource(R.string.card_lossy), AppTheme.type.caption, colors.warning, Modifier.padding(horizontal = d.space3))
        }
        if (isSelected) Icon(Icons.Check, null, colors.accentText, size = d.iconMd)
    }
}

/** The Compress action: re-encode in the same format (smaller), or pack into an archive format. */
@Composable
fun CompressPicker(item: QueueItem, onPick: (TargetInfo) -> Unit, onDismiss: () -> Unit) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val choices = TargetRanking.compressTargets(item.targets)
    Sheet(onDismiss, tag = "compress_picker") {
        Column(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom)).padding(bottom = d.space6)) {
            Row(Modifier.fillMaxWidth().padding(start = d.space6, end = d.space3, top = d.space4, bottom = d.space4), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    stringResource(R.string.picker_compress_title, item.name),
                    AppTheme.type.title3,
                    colors.textPrimary,
                    Modifier.weight(1f).semantics { heading() },
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                IconButton(Icons.Close, stringResource(R.string.picker_close), onDismiss)
            }
            Column(Modifier.padding(horizontal = d.space4)) {
                choices.forEach { t ->
                    TargetRow(if (t.compress) t.copy(name = stringResource(R.string.picker_compress_same, t.label)) else t, t.id == item.target?.id) { onPick(t) }
                }
            }
            Spacer(Modifier.height(d.space2))
        }
    }
}
