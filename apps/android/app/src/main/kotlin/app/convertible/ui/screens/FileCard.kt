package app.convertible.ui.screens

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.convertible.R
import app.convertible.model.Catalogue
import app.convertible.model.OptionSpec
import app.convertible.model.OptionType
import app.convertible.model.OptionValue
import app.convertible.model.OptionKeys
import app.convertible.model.OptionsForm
import app.convertible.model.Preset
import app.convertible.model.SizeChange
import app.convertible.model.SizeFormat
import app.convertible.model.TargetRanking
import app.convertible.state.ImportProblem
import app.convertible.state.ItemStatus
import app.convertible.state.QueueItem
import app.convertible.ui.components.BrandSlider
import app.convertible.ui.components.Button
import app.convertible.ui.components.ButtonKind
import app.convertible.ui.components.Chip
import app.convertible.ui.components.FormatBadge
import app.convertible.ui.components.Icon
import app.convertible.ui.components.IconButton
import app.convertible.ui.components.Icons
import app.convertible.ui.components.MarkProgress
import app.convertible.ui.components.SegmentedControl
import app.convertible.ui.components.SwitchRow
import app.convertible.ui.components.Text
import app.convertible.ui.components.TextInput
import app.convertible.ui.components.focusRing
import app.convertible.ui.theme.AppTheme
import kotlin.math.roundToInt
import kotlin.math.roundToLong
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import app.convertible.model.ChoiceControl
import app.convertible.model.NumberControl

@Immutable
class CardActions(
    val onPickTarget: (Long) -> Unit,
    val onPickCompress: (Long) -> Unit,
    val onSetTarget: (Long, String) -> Unit,
    val onOption: (Long, String, OptionValue?) -> Unit,
    val onConvert: (Long) -> Unit,
    val onCancel: (Long) -> Unit,
    val onRemove: (Long) -> Unit,
    val onAgain: (Long) -> Unit,
    val onSave: (Long) -> Unit,
    val onShare: (Long) -> Unit,
    val onOpen: (Long) -> Unit,
)

/** One file in the queue: what it is, what it becomes, how, and how it went. */
@Composable
fun FileCard(item: QueueItem, catalogue: Catalogue, actions: CardActions, modifier: Modifier = Modifier) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val shape = RoundedCornerShape(d.radiusLg)
    val status = item.status
    val accentBorder = when (status) {
        is ItemStatus.Done -> colors.success.copy(alpha = 0.5f)
        is ItemStatus.Failed, is ItemStatus.ImportFailed -> colors.error.copy(alpha = 0.5f)
        else -> colors.border
    }
    Column(
        modifier
            .fillMaxWidth()
            .clip(shape)
            .background(colors.surface)
            .border(1.dp, accentBorder, shape)
            .animateContentSize(spring(dampingRatio = 0.9f, stiffness = 500f))
            .padding(d.space5)
            .testTag("card_${item.id}"),
    ) {
        Header(item, actions)
        when (status) {
            is ItemStatus.Importing -> {
                Spacer(Modifier.height(d.space4))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    MarkProgress(null, size = 24.dp)
                    Spacer(Modifier.width(d.space3))
                    Text(stringResource(R.string.card_importing), AppTheme.type.label, colors.textSecondary)
                }
            }
            is ItemStatus.ImportFailed -> {
                Spacer(Modifier.height(d.space4))
                ErrorBlock(
                    title = stringResource(if (status.reason == ImportProblem.NO_SPACE) R.string.error_import_no_space else R.string.error_import_unreadable),
                    detail = stringResource(if (status.reason == ImportProblem.NO_SPACE) R.string.error_import_no_space_detail else R.string.error_import_unreadable_detail),
                )
            }
            else -> {
                Spacer(Modifier.height(d.space5))
                TargetRow(item, actions)
                val configurable = status is ItemStatus.Ready || status is ItemStatus.Failed
                AnimatedVisibility(
                    visible = configurable && item.target != null && item.schema.isNotEmpty(),
                    enter = expandVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.standard)) + fadeIn(),
                    exit = shrinkVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.standard)) + fadeOut(),
                ) {
                    OptionsPanel(item, actions)
                }
                StatusArea(item, actions)
            }
        }
    }
}

@Composable
private fun Header(item: QueueItem, actions: CardActions) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val sourceLabel = item.source?.label ?: item.sourceId?.uppercase()
    val typeName = item.source?.name ?: if (item.sourceId != null) stringResource(R.string.card_source_unknown) else ""
    val size = if (item.size >= 0) SizeFormat.format(item.size) else ""
    val description = stringResource(R.string.card_file_description, item.name, typeName.ifEmpty { "…" }, size.ifEmpty { "…" })
    Row(verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f).semantics(mergeDescendants = true) { contentDescription = description }) {
            Text(
                item.name,
                AppTheme.type.bodyStrong,
                colors.textPrimary,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(d.space2))
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (sourceLabel != null) {
                    FormatBadge(sourceLabel, item.source?.category ?: "other", round = true)
                    Spacer(Modifier.width(d.space3))
                }
                Text(
                    listOf(typeName, size).filter { it.isNotEmpty() }.joinToString(" · "),
                    AppTheme.type.caption,
                    colors.textSecondary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (!item.status.isBusy) {
            IconButton(
                Icons.Close,
                stringResource(R.string.action_remove, item.name),
                { actions.onRemove(item.id) },
                Modifier.testTag("remove_${item.id}"),
            )
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TargetRow(item: QueueItem, actions: CardActions) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    if (item.targets.isEmpty() && item.status is ItemStatus.Ready) {
        Text(stringResource(R.string.card_no_targets), AppTheme.type.label, colors.textSecondary)
        return
    }
    val editable = item.status is ItemStatus.Ready || item.status is ItemStatus.Failed
    val target = item.target
    Column {
        Text(stringResource(R.string.card_convert_to).uppercase(), AppTheme.type.overline, colors.textSecondary)
        Spacer(Modifier.height(d.space3))
        if (target == null) {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(d.space3), verticalArrangement = Arrangement.spacedBy(d.space3)) {
                TargetRanking.suggestions(item.targets).forEach { t ->
                    Chip(
                        t.label,
                        selected = false,
                        onClick = { actions.onSetTarget(item.id, t.id) },
                        modifier = Modifier.testTag("suggest_${item.id}_${t.id}"),
                        leading = { Box(Modifier.size(6.dp).background(colors.category(t.category))) },
                    )
                }
                Chip(
                    stringResource(R.string.card_more_formats),
                    selected = false,
                    onClick = { actions.onPickTarget(item.id) },
                    modifier = Modifier.testTag("pick_target_${item.id}"),
                    leading = { Icon(Icons.Search, null, colors.textSecondary, size = d.iconSm) },
                )
                if (TargetRanking.compressTargets(item.targets).isNotEmpty()) {
                    Chip(
                        stringResource(R.string.action_compress),
                        selected = false,
                        onClick = { actions.onPickCompress(item.id) },
                        modifier = Modifier.testTag("compress_${item.id}"),
                        leading = { Icon(Icons.Compress, null, colors.textSecondary, size = d.iconSm) },
                    )
                }
            }
        } else {
            val interaction = remember { MutableInteractionSource() }
            val changeLabel = stringResource(R.string.card_change_format, target.name)
            Row(
                Modifier
                    .focusRing(interaction, RoundedCornerShape(d.radiusMd + d.focusRingOffset))
                    .fillMaxWidth()
                    .heightIn(min = d.touchTarget)
                    .clip(RoundedCornerShape(d.radiusMd))
                    .background(colors.surfaceSunken)
                    .border(1.dp, colors.border, RoundedCornerShape(d.radiusMd))
                    .clickable(interaction, androidx.compose.foundation.LocalIndication.current, enabled = editable, role = Role.Button) {
                        if (target.compress) actions.onPickCompress(item.id) else actions.onPickTarget(item.id)
                    }
                    .semantics { contentDescription = changeLabel }
                    .padding(horizontal = d.space4, vertical = d.space3)
                    .testTag("pick_target_${item.id}"),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FormatBadge(target.label, target.category, round = false, large = true)
                Spacer(Modifier.width(d.space4))
                Column(Modifier.weight(1f)) {
                    Text(
                        if (target.compress) stringResource(R.string.picker_compress_same, target.label) else target.name,
                        AppTheme.type.body,
                        colors.textPrimary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    if (target.lossy) Text(stringResource(R.string.card_lossy), AppTheme.type.caption, colors.warning)
                }
                if (editable) Icon(Icons.ChevronDown, null, colors.textSecondary, size = d.iconMd)
            }
        }
    }
}

@Composable
private fun OptionsPanel(item: QueueItem, actions: CardActions) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val basic = OptionsForm.basic(item.schema)
    val advanced = OptionsForm.advanced(item.schema)
    var showAdvanced by rememberSaveable(item.id) { mutableStateOf(false) }
    val expandedLabel = stringResource(R.string.state_expanded)
    val collapsedLabel = stringResource(R.string.state_collapsed)
    Column(Modifier.padding(top = d.space5), verticalArrangement = Arrangement.spacedBy(d.space4)) {
        if (OptionsForm.hasPreset(item.schema)) {
            val spec = item.schema.first { it.key == OptionKeys.PRESET }
            val current = Preset.from((OptionsForm.valueOf(spec, item.values) as? OptionValue.Text)?.value) ?: Preset.BALANCED
            Column {
                Text(stringResource(R.string.quality_title), AppTheme.type.label, colors.textSecondary)
                Spacer(Modifier.height(d.space3))
                SegmentedControl(
                    options = Preset.entries.filter { p -> spec.choices.isEmpty() || p.id in spec.choices },
                    selected = current,
                    label = { presetLabel(it) },
                    onSelect = { actions.onOption(item.id, OptionKeys.PRESET, OptionValue.Text(it.id)) },
                    testTagPrefix = "preset_${item.id}_",
                )
            }
        }
        basic.forEach { OptionField(it, item, actions) }
        if (advanced.isNotEmpty()) {
            val interaction = remember { MutableInteractionSource() }
            val rotation by androidx.compose.animation.core.animateFloatAsState(if (showAdvanced) 180f else 0f, tween(AppTheme.motion.base), label = "chevron")
            Row(
                Modifier
                    .focusRing(interaction, RoundedCornerShape(d.radiusMd))
                    .fillMaxWidth()
                    .heightIn(min = d.touchTarget)
                    .clip(RoundedCornerShape(d.radiusMd))
                    .clickable(interaction, androidx.compose.foundation.LocalIndication.current, role = Role.Button) { showAdvanced = !showAdvanced }
                    .semantics { stateDescription = if (showAdvanced) expandedLabel else collapsedLabel }
                    .padding(horizontal = d.space2)
                    .testTag("advanced_${item.id}"),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Sliders, null, colors.accentText, size = d.iconMd)
                Spacer(Modifier.width(d.space3))
                Text(stringResource(R.string.card_advanced), AppTheme.type.bodyStrong, colors.accentText)
                Spacer(Modifier.width(d.space3))
                Text(pluralStringResource(R.plurals.card_advanced_count, advanced.size, advanced.size), AppTheme.type.caption, colors.textSecondary, Modifier.weight(1f))
                Icon(Icons.ChevronDown, null, colors.textSecondary, Modifier.rotate(rotation), size = d.iconMd)
            }
            AnimatedVisibility(
                visible = showAdvanced,
                enter = expandVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.standard)) + fadeIn(),
                exit = shrinkVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.standard)) + fadeOut(),
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(d.space4)) { advanced.forEach { OptionField(it, item, actions) } }
            }
        }
    }
}

@Composable
fun presetLabel(preset: Preset): String = when (preset) {
    Preset.HIGH -> stringResource(R.string.preset_high)
    Preset.BALANCED -> stringResource(R.string.preset_balanced)
    Preset.SMALL -> stringResource(R.string.preset_small)
}

/** One option rendered generically from its schema entry. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun OptionField(spec: OptionSpec, item: QueueItem, actions: CardActions) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val value = OptionsForm.valueOf(spec, item.values)
    val set: (OptionValue?) -> Unit = { actions.onOption(item.id, spec.key, it) }
    val tag = "option_${item.id}_${spec.key}"
    when (spec.type) {
        OptionType.BOOL -> SwitchRow(
            title = spec.label,
            subtitle = spec.description.ifEmpty { null },
            checked = (value as? OptionValue.BoolValue)?.value ?: false,
            onChange = { set(OptionValue.BoolValue(it)) },
            modifier = Modifier.testTag(tag),
        )
        OptionType.INT, OptionType.FLOAT -> {
            val number = when (value) {
                is OptionValue.IntValue -> value.value.toDouble()
                is OptionValue.FloatValue -> value.value
                else -> spec.min ?: 0.0
            }
            val integer = spec.type == OptionType.INT
            Column(Modifier.testTag(tag)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(spec.label, AppTheme.type.body, colors.textPrimary, Modifier.weight(1f))
                    Text(
                        if (integer) number.roundToInt().toString() else String.format(androidx.compose.ui.platform.LocalConfiguration.current.locales[0], "%.2f", number),
                        AppTheme.type.monoText,
                        colors.accentText,
                    )
                }
                val setNumber = { v: Double -> set(if (integer) OptionValue.IntValue(v.roundToLong()) else OptionValue.FloatValue(v)) }
                if (spec.min != null && spec.max != null && OptionsForm.numberControl(spec) == NumberControl.SLIDER) {
                    BrandSlider(
                        value = number,
                        min = spec.min,
                        max = spec.max,
                        integer = integer,
                        label = spec.label,
                        onChange = setNumber,
                    )
                } else {
                    // Wide or open ranges (bit rates, sample rates, sizes): a typed field with -/+ steps; the
                    // value is clamped to the schema's bounds by OptionSpec.coerce when it is sent.
                    val step = OptionsForm.step(spec)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        IconButton(Icons.Minus, stringResource(R.string.option_decrease, spec.label), { setNumber(clampTo(spec, number - step)) })
                        TextInput(
                            value = if (integer) number.roundToLong().toString() else number.toString(),
                            onChange = { text -> text.trim().toDoubleOrNull()?.let { setNumber(clampTo(spec, it)) } },
                            placeholder = spec.label,
                            keyboardType = androidx.compose.ui.text.input.KeyboardType.Number,
                            modifier = Modifier.weight(1f).testTag("${tag}_field"),
                        )
                        IconButton(Icons.Plus, stringResource(R.string.option_increase, spec.label), { setNumber(clampTo(spec, number + step)) })
                    }
                }
                if (spec.description.isNotEmpty()) Text(spec.description, AppTheme.type.caption, colors.textSecondary)
            }
        }
        OptionType.ENUM -> Column(Modifier.testTag(tag)) {
            Text(spec.label, AppTheme.type.body, colors.textPrimary)
            Spacer(Modifier.height(d.space3))
            val selected = (value as? OptionValue.Text)?.value
            when (OptionsForm.choiceControl(spec)) {
                ChoiceControl.SEGMENTED ->
                    SegmentedControl(spec.choices, selected ?: spec.choices.first(), { it }, { set(OptionValue.Text(it)) })
                ChoiceControl.CHIPS ->
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(d.space3), verticalArrangement = Arrangement.spacedBy(d.space3)) {
                        spec.choices.forEach { choice ->
                            Chip(choice, selected = choice == selected, onClick = { set(OptionValue.Text(choice)) })
                        }
                    }
                ChoiceControl.MENU -> ChoiceMenu(spec, selected, { set(OptionValue.Text(it)) }, Modifier.testTag("${tag}_menu"))
            }
            if (spec.description.isNotEmpty()) {
                Spacer(Modifier.height(d.space2))
                Text(spec.description, AppTheme.type.caption, colors.textSecondary)
            }
        }
        OptionType.STRING -> Column(Modifier.testTag(tag)) {
            Text(spec.label, AppTheme.type.body, colors.textPrimary)
            Spacer(Modifier.height(d.space3))
            TextInput(
                value = (value as? OptionValue.Text)?.value.orEmpty(),
                onChange = { set(OptionValue.Text(it)) },
                placeholder = spec.description.ifEmpty { spec.label },
            )
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun StatusArea(item: QueueItem, actions: CardActions) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    if (item.status is ItemStatus.Ready && item.target == null) return // nothing to do until a format is chosen
    Spacer(Modifier.height(d.space5))
    val baseMs = AppTheme.motion.base
    val fastMs = AppTheme.motion.fast
    AnimatedContent(
        targetState = item.status,
        contentKey = { it::class },
        transitionSpec = { fadeIn(tween(baseMs)) togetherWith fadeOut(tween(fastMs)) },
        label = "status",
    ) { status ->
        when (status) {
            is ItemStatus.Ready -> Button(
                stringResource(R.string.action_convert),
                { actions.onConvert(item.id) },
                Modifier.fillMaxWidth().testTag("convert_${item.id}"),
                // The queue's "Convert N files" is the screen's one primary action.
                kind = ButtonKind.SECONDARY,
                enabled = item.canConvert,
            )
            is ItemStatus.Queued, is ItemStatus.Converting -> {
                val converting = status as? ItemStatus.Converting
                val fraction = converting?.fraction
                Row(
                    Modifier.fillMaxWidth().semantics(mergeDescendants = true) { liveRegion = LiveRegionMode.Polite },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    MarkProgress(if (converting == null) null else fraction, size = 44.dp, modifier = Modifier.testTag("progress_${item.id}"))
                    Spacer(Modifier.width(d.space4))
                    Column(Modifier.weight(1f)) {
                        Text(
                            when {
                                converting == null -> stringResource(R.string.card_waiting)
                                converting.stage.isEmpty() -> stringResource(R.string.card_starting)
                                else -> converting.stage
                            },
                            AppTheme.type.bodyStrong,
                            colors.textPrimary,
                        )
                        if (converting != null) {
                            Text(stringResource(R.string.card_progress, (converting.fraction * 100).roundToInt()), AppTheme.type.monoText, colors.textSecondary)
                        }
                    }
                    Button(stringResource(R.string.action_cancel), { actions.onCancel(item.id) }, Modifier.testTag("cancel_${item.id}"), compact = true, kind = ButtonKind.SECONDARY)
                }
            }
            is ItemStatus.Done -> Column {
                val output = status.output
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.semantics(mergeDescendants = true) { liveRegion = LiveRegionMode.Polite }) {
                    MarkProgress(1f, size = 44.dp, done = true)
                    Spacer(Modifier.width(d.space4))
                    Column(Modifier.weight(1f)) {
                        Text(output.name, AppTheme.type.monoText, colors.textPrimary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                stringResource(R.string.card_size_change, SizeFormat.format(item.size), SizeFormat.format(output.size)),
                                AppTheme.type.caption,
                                colors.textSecondary,
                            )
                            Spacer(Modifier.width(d.space3))
                            SizeChangeBadge(SizeChange.of(item.size, output.size))
                        }
                    }
                }
                if (output.warnings.isNotEmpty()) {
                    Spacer(Modifier.height(d.space4))
                    Warnings(output.warnings)
                }
                Spacer(Modifier.height(d.space5))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(d.space3), verticalArrangement = Arrangement.spacedBy(d.space3)) {
                    Button(stringResource(R.string.action_save), { actions.onSave(item.id) }, Modifier.testTag("save_${item.id}"), kind = ButtonKind.SECONDARY, icon = Icons.Save, compact = true)
                    Button(stringResource(R.string.action_share), { actions.onShare(item.id) }, Modifier.testTag("share_${item.id}"), icon = Icons.Share, compact = true)
                    Button(stringResource(R.string.action_open_in), { actions.onOpen(item.id) }, Modifier.testTag("open_${item.id}"), icon = Icons.Open, compact = true)
                    Button(stringResource(R.string.action_convert_again), { actions.onAgain(item.id) }, Modifier.testTag("again_${item.id}"), kind = ButtonKind.GHOST, icon = Icons.Again, compact = true)
                }
            }
            is ItemStatus.Failed -> Column(Modifier.semantics(mergeDescendants = true) { liveRegion = LiveRegionMode.Polite }) {
                ErrorBlock(errorTitle(status.error), errorDetail(status.error), cancelled = status.error is app.convertible.core.CoreError.Cancelled)
                Spacer(Modifier.height(d.space4))
                Button(
                    stringResource(if (status.error is app.convertible.core.CoreError.Cancelled) R.string.action_convert else R.string.action_try_again),
                    { actions.onConvert(item.id) },
                    Modifier.fillMaxWidth().testTag("retry_${item.id}"),
                    kind = ButtonKind.SECONDARY,
                    enabled = item.canConvert,
                )
            }
            else -> Unit
        }
    }
}

@Composable
private fun SizeChangeBadge(change: SizeChange) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val (text, fg, bg) = when (change) {
        is SizeChange.Smaller -> Triple(stringResource(R.string.card_smaller, change.percent), colors.success, colors.successSubtle)
        is SizeChange.Larger -> Triple(stringResource(R.string.card_larger, change.percent), colors.textSecondary, colors.surfaceSunken)
        SizeChange.Same -> Triple(stringResource(R.string.card_same_size), colors.textSecondary, colors.surfaceSunken)
    }
    Text(
        text,
        AppTheme.type.caption,
        fg,
        Modifier.clip(RoundedCornerShape(d.radiusXs)).background(bg).padding(horizontal = d.space3, vertical = d.space1),
    )
}

@Composable
private fun Warnings(warnings: List<String>) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    var open by rememberSaveable { mutableStateOf(false) }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusMd))
            .background(colors.warningSubtle.copy(alpha = 0.7f))
            .clickable(role = Role.Button) { open = !open }
            .padding(horizontal = d.space4, vertical = d.space3)
            .testTag("warnings"),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Info, null, colors.warning, size = d.iconSm)
            Spacer(Modifier.width(d.space3))
            Text(
                if (open || warnings.size == 1) warnings.first() else pluralStringResource(R.plurals.card_warnings, warnings.size, warnings.size),
                AppTheme.type.caption,
                colors.textPrimary,
                Modifier.weight(1f),
                maxLines = if (open) Int.MAX_VALUE else 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (open) {
            warnings.drop(1).forEach {
                Spacer(Modifier.height(d.space2))
                Text(it, AppTheme.type.caption, colors.textPrimary, Modifier.padding(start = d.iconSm + d.space3))
            }
        }
    }
}

@Composable
private fun ErrorBlock(title: String, detail: String, cancelled: Boolean = false) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusMd))
            .background(if (cancelled) colors.surfaceSunken else colors.errorSubtle)
            .padding(d.space4),
    ) {
        Icon(if (cancelled) Icons.Stop else Icons.Error, null, if (cancelled) colors.textSecondary else colors.error, size = d.iconMd)
        Spacer(Modifier.width(d.space4))
        Column {
            Text(title, AppTheme.type.bodyStrong, if (cancelled) colors.textPrimary else colors.error)
            if (detail.isNotEmpty()) {
                Spacer(Modifier.height(d.space1))
                Text(detail, AppTheme.type.label, colors.textPrimary)
            }
        }
    }
}

private fun clampTo(spec: OptionSpec, v: Double): Double {
    var x = v
    spec.min?.let { if (x < it) x = it }
    spec.max?.let { if (x > it) x = it }
    return x
}

/** An enum with many choices: a field showing the value that opens a sheet listing every choice. */
@Composable
private fun ChoiceMenu(spec: OptionSpec, selected: String?, onChoose: (String) -> Unit, modifier: Modifier = Modifier) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    var open by rememberSaveable { mutableStateOf(false) }
    val current = selected ?: spec.choices.firstOrNull().orEmpty()
    val shape = RoundedCornerShape(d.radiusMd)
    val description = stringResource(R.string.option_choose, spec.label, current)
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = d.touchTarget)
            .clip(shape)
            .background(colors.surfaceSunken)
            .clickable(role = Role.DropdownList, onClickLabel = description) { open = true }
            .semantics { contentDescription = description }
            .padding(horizontal = d.space4, vertical = d.space3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(current, AppTheme.type.body, colors.textPrimary, Modifier.weight(1f), maxLines = 1)
        Icon(Icons.ChevronDown, null, colors.textSecondary, size = d.iconMd)
    }
    if (open) {
        Sheet(onDismiss = { open = false }, tag = "choice_menu") {
            Column {
                Row(Modifier.fillMaxWidth().padding(start = d.space6, end = d.space3, top = d.space4), verticalAlignment = Alignment.CenterVertically) {
                    Text(spec.label, AppTheme.type.title3, colors.textPrimary, Modifier.weight(1f).semantics { heading() })
                    IconButton(Icons.Close, stringResource(R.string.picker_close), { open = false })
                }
                androidx.compose.foundation.lazy.LazyColumn(
                    Modifier.fillMaxWidth().heightIn(max = 480.dp),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(d.space4),
                ) {
                    items(spec.choices.size, key = { spec.choices[it] }) { i ->
                        val choice = spec.choices[i]
                        val isSelected = choice == current
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .heightIn(min = 48.dp)
                                .clip(shape)
                                .background(if (isSelected) colors.accentSubtle else androidx.compose.ui.graphics.Color.Transparent)
                                .clickable(role = Role.RadioButton) {
                                    onChoose(choice)
                                    open = false
                                }
                                .semantics { this.selected = isSelected }
                                .padding(horizontal = d.space3)
                                .testTag("choice_$choice"),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(choice, AppTheme.type.body, if (isSelected) colors.accentText else colors.textPrimary, Modifier.weight(1f))
                            if (isSelected) Icon(Icons.Check, null, colors.accentText, size = d.iconMd)
                        }
                    }
                }
            }
        }
    }
}
