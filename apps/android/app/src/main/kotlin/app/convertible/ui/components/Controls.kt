package app.convertible.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.convertible.ui.theme.AppTheme
import kotlin.math.roundToInt

/** Text in the app's type scale and colours. */
@Composable
fun Text(
    text: String,
    style: TextStyle,
    color: Color,
    modifier: Modifier = Modifier,
    maxLines: Int = Int.MAX_VALUE,
    overflow: TextOverflow = TextOverflow.Clip,
    textAlign: androidx.compose.ui.text.style.TextAlign? = null,
) {
    BasicText(
        text = text,
        style = if (textAlign != null) style.copy(color = color, textAlign = textAlign) else style.copy(color = color),
        modifier = modifier,
        maxLines = maxLines,
        overflow = overflow,
    )
}

/** A visible keyboard-focus ring (focusRing token, 2 dp, offset 2 dp) around a component of [shape]. */
fun Modifier.focusRing(interaction: MutableInteractionSource, shape: Shape): Modifier = composed {
    val focused by interaction.collectIsFocusedAsState()
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    if (focused) this.border(d.focusRingWidth, colors.focusRing, shape).padding(d.focusRingWidth) else this.padding(d.focusRingWidth)
}

/** A panel: surface colour, hairline border, large radius. */
@Composable
fun Panel(
    modifier: Modifier = Modifier,
    color: Color = AppTheme.colors.surface,
    borderColor: Color = AppTheme.colors.border,
    shape: Shape = RoundedCornerShape(AppTheme.dimens.radiusLg),
    content: @Composable () -> Unit,
) {
    Box(
        modifier
            .clip(shape)
            .background(color)
            .border(1.dp, borderColor, shape),
    ) { content() }
}

enum class ButtonKind { PRIMARY, SECONDARY, GHOST, DANGER }

/**
 * The app's button: 48 dp tall, medium radius, springy press scale. PRIMARY is the one clear action of a view.
 */
@Composable
fun Button(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: ButtonKind = ButtonKind.SECONDARY,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    compact: Boolean = false,
    contentDescription: String? = null,
) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed && !AppTheme.motion.reduced) 0.97f else 1f, spring(stiffness = 900f), label = "press")
    val shape = RoundedCornerShape(d.radiusMd)
    val (bg, fg, border) = when (kind) {
        ButtonKind.PRIMARY -> Triple(if (pressed) colors.accentFillPressed else colors.accentFill, colors.onAccent, Color.Transparent)
        ButtonKind.SECONDARY -> Triple(colors.surfaceElevated, colors.textPrimary, colors.border)
        ButtonKind.GHOST -> Triple(Color.Transparent, colors.accentText, Color.Transparent)
        ButtonKind.DANGER -> Triple(colors.errorSubtle, colors.error, Color.Transparent)
    }
    val bgColor by animateColorAsState(if (enabled) bg else colors.surfaceSunken, tween(AppTheme.motion.fast), label = "bg")
    val fgColor = if (enabled) fg else colors.textDisabled
    Box(
        modifier
            .focusRing(interaction, RoundedCornerShape(d.radiusMd + d.focusRingOffset))
            .scale(scale)
            .defaultMinSize(minHeight = if (compact) d.controlMd else d.controlLg, minWidth = d.touchTarget)
            .clip(shape)
            .background(bgColor)
            .border(1.dp, if (enabled) border else Color.Transparent, shape)
            .clickable(
                interactionSource = interaction,
                indication = androidx.compose.foundation.LocalIndication.current,
                enabled = enabled,
                role = Role.Button,
                onClick = onClick,
            )
            .then(if (contentDescription != null) Modifier.semantics { this.contentDescription = contentDescription } else Modifier)
            .padding(horizontal = if (compact) d.space5 else d.space6, vertical = d.space3),
        contentAlignment = Alignment.Center,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
            if (icon != null) {
                Icon(icon, null, fgColor, size = d.iconMd)
                Spacer(Modifier.width(d.space3))
            }
            Text(text, if (compact) AppTheme.type.label else AppTheme.type.bodyStrong, fgColor, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

/** A square-ish icon-only button with a 48 dp touch target. */
@Composable
fun IconButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    tint: Color = AppTheme.colors.textSecondary,
    enabled: Boolean = true,
) {
    val d = AppTheme.dimens
    val interaction = remember { MutableInteractionSource() }
    val shape = RoundedCornerShape(d.radiusMd)
    Box(
        modifier
            .focusRing(interaction, RoundedCornerShape(d.radiusMd + d.focusRingOffset))
            .size(d.touchTarget - d.focusRingWidth * 2)
            .clip(shape)
            .clickable(
                interactionSource = interaction,
                indication = androidx.compose.foundation.LocalIndication.current,
                enabled = enabled,
                role = Role.Button,
                onClick = onClick,
            )
            .semantics { this.contentDescription = contentDescription },
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, null, if (enabled) tint else AppTheme.colors.textDisabled, size = d.iconLg)
    }
}

/**
 * A format badge in its category colour. Source formats get a round dot, targets a square one, echoing the mark.
 */
@Composable
fun FormatBadge(label: String, category: String, modifier: Modifier = Modifier, round: Boolean = true, large: Boolean = false) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val tint = colors.category(category)
    Row(
        modifier
            .clip(RoundedCornerShape(d.radiusSm))
            .background(tint.copy(alpha = if (colors.isDark) 0.14f else 0.10f))
            .padding(horizontal = if (large) d.space4 else d.space3, vertical = if (large) d.space3 else d.space2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(if (large) 8.dp else 6.dp).clip(if (round) CircleShape else RoundedCornerShape(0.dp)).background(tint))
        Spacer(Modifier.width(if (large) d.space3 else d.space2 + d.space1))
        Text(label.uppercase(), if (large) AppTheme.type.monoText.copy(fontWeight = androidx.compose.ui.text.font.FontWeight.Medium) else AppTheme.type.formatLabel, tint, maxLines = 1)
    }
}

/** A selectable pill (filters, suggestions). */
@Composable
fun Chip(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    leading: (@Composable () -> Unit)? = null,
) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val interaction = remember { MutableInteractionSource() }
    val shape = RoundedCornerShape(d.radiusXxl)
    val bg by animateColorAsState(if (selected) colors.accentSubtle else colors.surface, tween(AppTheme.motion.fast), label = "chip")
    Row(
        modifier
            .focusRing(interaction, RoundedCornerShape(d.radiusXxl))
            .heightIn(min = d.touchTarget - d.focusRingWidth * 2)
            .clip(shape)
            .background(bg)
            .border(1.dp, if (selected) colors.accent else colors.border, shape)
            .selectable(selected, interactionSource = interaction, indication = androidx.compose.foundation.LocalIndication.current, role = Role.Button, onClick = onClick)
            .padding(horizontal = d.space5, vertical = d.space3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(d.space3))
        }
        Text(text, AppTheme.type.label, if (selected) colors.accentText else colors.textPrimary, maxLines = 1)
    }
}

/** A segmented control with a sliding indicator. */
@Composable
fun <T> SegmentedControl(
    options: List<T>,
    selected: T,
    label: @Composable (T) -> String,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    testTagPrefix: String? = null,
) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val index = options.indexOf(selected).coerceAtLeast(0)
    BoxWithConstraints(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusMd))
            .background(colors.surfaceSunken)
            .border(1.dp, colors.border, RoundedCornerShape(d.radiusMd))
            .padding(d.space1 + d.space1),
    ) {
        val segment = maxWidth / options.size
        val offset by animateDpAsState(segment * index, spring(dampingRatio = 0.82f, stiffness = 600f), label = "segment")
        Box(
            Modifier
                .offset { androidx.compose.ui.unit.IntOffset(offset.roundToPx(), 0) }
                .width(segment)
                .height(d.controlMd)
                .clip(RoundedCornerShape(d.radiusSm))
                .background(colors.surfaceElevated)
                .border(1.dp, colors.borderStrong.copy(alpha = 0.5f), RoundedCornerShape(d.radiusSm)),
        )
        Row(Modifier.fillMaxWidth().selectableGroup()) {
            options.forEachIndexed { i, option ->
                val interaction = remember { MutableInteractionSource() }
                val isSelected = i == index
                Box(
                    Modifier
                        .weight(1f)
                        .height(d.controlMd)
                        .clip(RoundedCornerShape(d.radiusSm))
                        .selectable(isSelected, interactionSource = interaction, indication = androidx.compose.foundation.LocalIndication.current, role = Role.RadioButton) { onSelect(option) }
                        .then(if (testTagPrefix != null) Modifier.testTag("$testTagPrefix$i") else Modifier),
                    contentAlignment = Alignment.Center,
                ) {
                    val focused by interaction.collectIsFocusedAsState()
                    if (focused) Box(Modifier.matchParentSize().border(d.focusRingWidth, colors.focusRing, RoundedCornerShape(d.radiusSm)))
                    Text(
                        label(option),
                        AppTheme.type.label,
                        if (isSelected) colors.textPrimary else colors.textSecondary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/** On/off switch drawn in brand colours; the whole row should usually be the toggle target (see [SwitchRow]). */
@Composable
fun BrandSwitch(checked: Boolean, modifier: Modifier = Modifier) {
    val colors = AppTheme.colors
    val track by animateColorAsState(if (checked) colors.accentFill else colors.surfaceSunken, tween(AppTheme.motion.base), label = "track")
    val thumbOffset by animateDpAsState(if (checked) 20.dp else 0.dp, spring(dampingRatio = 0.7f, stiffness = 700f), label = "thumb")
    Box(
        modifier
            .width(48.dp)
            .height(28.dp)
            .clip(CircleShape)
            .background(track)
            .border(1.dp, if (checked) Color.Transparent else colors.borderStrong, CircleShape)
            .padding(4.dp),
    ) {
        Box(
            Modifier
                .offset { androidx.compose.ui.unit.IntOffset(thumbOffset.roundToPx(), 0) }
                .size(20.dp)
                .clip(CircleShape)
                .background(if (checked) colors.onAccent else colors.textSecondary),
        )
    }
}

/** A labelled switch row; the whole row toggles (role Switch for screen readers). */
@Composable
fun SwitchRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit, modifier: Modifier = Modifier, subtitle: String? = null) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val interaction = remember { MutableInteractionSource() }
    Row(
        modifier
            .focusRing(interaction, RoundedCornerShape(d.radiusMd))
            .fillMaxWidth()
            .heightIn(min = d.touchTarget)
            .clip(RoundedCornerShape(d.radiusMd))
            .toggleable(checked, interactionSource = interaction, indication = androidx.compose.foundation.LocalIndication.current, role = Role.Switch, onValueChange = onChange)
            .padding(horizontal = d.space3, vertical = d.space3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        androidx.compose.foundation.layout.Column(Modifier.weight(1f)) {
            Text(title, AppTheme.type.body, colors.textPrimary)
            if (subtitle != null) Text(subtitle, AppTheme.type.caption, colors.textSecondary)
        }
        Spacer(Modifier.width(d.space5))
        BrandSwitch(checked)
    }
}

/**
 * A slider for numeric options. Drag or tap the track; arrow keys step; TalkBack can set the value directly.
 */
@Composable
fun BrandSlider(
    value: Double,
    min: Double,
    max: Double,
    integer: Boolean,
    onChange: (Double) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val interaction = remember { MutableInteractionSource() }
    val span = (max - min).takeIf { it > 0 } ?: 1.0
    val fraction = ((value - min) / span).toFloat().coerceIn(0f, 1f)
    val step = if (integer) maxOf(1.0, (span / 100.0).roundToInt().toDouble()) else span / 100.0
    fun snap(v: Double): Double {
        val clamped = v.coerceIn(min, max)
        return if (integer) clamped.roundToInt().toDouble() else clamped
    }
    val density = LocalDensity.current
    BoxWithConstraints(
        modifier
            .fillMaxWidth()
            .height(d.touchTarget)
            .focusRing(interaction, RoundedCornerShape(d.radiusSm))
            .focusable(interactionSource = interaction)
            .onKeyEvent { e ->
                if (e.type != KeyEventType.KeyDown) return@onKeyEvent false
                when (e.key) {
                    Key.DirectionRight, Key.DirectionUp -> { onChange(snap(value + step)); true }
                    Key.DirectionLeft, Key.DirectionDown -> { onChange(snap(value - step)); true }
                    else -> false
                }
            }
            .semantics {
                contentDescription = label
                stateDescription = if (integer) value.roundToInt().toString() else String.format(java.util.Locale.getDefault(), "%.2f", value)
                progressBarRangeInfo = ProgressBarRangeInfo(value.toFloat(), min.toFloat()..max.toFloat())
                setProgress { target ->
                    onChange(snap(target.toDouble()))
                    true
                }
            },
    ) {
        val trackWidth = maxWidth
        val widthPx = with(density) { trackWidth.toPx() }
        val thumb = 22.dp
        val thumbPx = with(density) { thumb.toPx() }
        fun valueAt(x: Float): Double {
            val usable = (widthPx - thumbPx).coerceAtLeast(1f)
            val f = ((x - thumbPx / 2f) / usable).coerceIn(0f, 1f)
            return snap(min + f * span)
        }
        Box(
            Modifier
                .fillMaxWidth()
                .fillMaxHeight()
                .pointerInput(min, max, widthPx) {
                    detectTapGestures { onChange(valueAt(it.x)) }
                }
                .pointerInput(min, max, widthPx) {
                    detectDragGestures { change, _ -> onChange(valueAt(change.position.x)) }
                },
            contentAlignment = Alignment.CenterStart,
        ) {
            Box(Modifier.padding(horizontal = thumb / 2).fillMaxWidth().height(4.dp).clip(CircleShape).background(colors.surfaceSunken).border(1.dp, colors.border, CircleShape))
            Box(
                Modifier
                    .padding(start = thumb / 2)
                    .width((trackWidth - thumb) * fraction)
                    .height(4.dp)
                    .clip(CircleShape)
                    .background(colors.fluxHorizontal()),
            )
            Box(
                Modifier
                    .offset(x = (trackWidth - thumb) * fraction)
                    .size(thumb)
                    .clip(CircleShape)
                    .background(colors.surfaceElevated)
                    .border(2.dp, colors.accent, CircleShape),
            )
        }
    }
}

/** Single-line text input with an optional leading icon. */
@Composable
fun TextInput(
    value: String,
    onChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
    leading: ImageVector? = null,
    keyboardType: KeyboardType = KeyboardType.Text,
    imeAction: ImeAction = ImeAction.Done,
    onClear: (() -> Unit)? = null,
    clearLabel: String = "",
) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(d.radiusMd)
    BasicTextField(
        value = value,
        onValueChange = onChange,
        singleLine = true,
        interactionSource = interaction,
        textStyle = AppTheme.type.body.copy(color = colors.textPrimary),
        cursorBrush = SolidColor(colors.accent),
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType, imeAction = imeAction),
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = d.touchTarget),
        decorationBox = { inner ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = d.touchTarget)
                    .clip(shape)
                    .background(colors.surfaceSunken)
                    .border(if (focused) d.focusRingWidth else 1.dp, if (focused) colors.focusRing else colors.border, shape)
                    .padding(start = d.space4, end = d.space2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (leading != null) {
                    Icon(leading, null, colors.textSecondary, size = d.iconMd)
                    Spacer(Modifier.width(d.space3))
                }
                Box(Modifier.weight(1f).padding(vertical = d.space3)) {
                    if (value.isEmpty()) Text(placeholder, AppTheme.type.body, colors.textSecondary, maxLines = 1)
                    inner()
                }
                if (onClear != null && value.isNotEmpty()) IconButton(Icons.Close, clearLabel, onClear)
            }
        },
    )
}

/** Overline section heading. */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text.uppercase(),
        AppTheme.type.overline,
        AppTheme.colors.textSecondary,
        modifier.semantics { heading() },
    )
}

/** Row helper with standard spacing. */
@Composable
fun SpacedRow(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(AppTheme.dimens.space3), verticalAlignment = Alignment.CenterVertically, content = content)
}

/** Keeps a column of content readable on wide screens. */
fun Modifier.readableWidth(max: Dp = 720.dp): Modifier = this.widthIn(max = max)
