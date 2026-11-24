package app.convertible.ui.screens

import android.app.Activity
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContract
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.draganddrop.dragAndDropTarget
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draganddrop.DragAndDropEvent
import androidx.compose.ui.draganddrop.DragAndDropTarget
import androidx.compose.ui.draganddrop.mimeTypes
import androidx.compose.ui.draganddrop.toAndroidDragEvent
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.convertible.R
import app.convertible.state.ItemStatus
import app.convertible.state.QueueAction
import app.convertible.state.QueueState
import app.convertible.state.queueActions
import app.convertible.state.SourceGroup
import app.convertible.ui.ConverterViewModel
import app.convertible.ui.UiMessage
import app.convertible.ui.components.Button
import app.convertible.ui.components.ButtonKind
import app.convertible.ui.components.FormatBadge
import app.convertible.ui.components.Icon
import app.convertible.ui.components.IconButton
import app.convertible.ui.components.Icons
import app.convertible.ui.components.StaticMark
import app.convertible.ui.components.Text
import app.convertible.ui.startSafely
import app.convertible.ui.theme.AppTheme

/** What the target picker is open for: one file, or every file of one source format. */
private sealed interface PickerFor {
    data class Item(val id: Long) : PickerFor
    data class Group(val sourceId: String) : PickerFor
    data class Compress(val id: Long) : PickerFor
}

/** ACTION_CREATE_DOCUMENT with a per-call file name and MIME type. */
private class CreateDocumentFor : ActivityResultContract<Pair<String, String>, Uri?>() {
    override fun createIntent(context: Context, input: Pair<String, String>): Intent =
        Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(input.second)
            .putExtra(Intent.EXTRA_TITLE, input.first)

    override fun parseResult(resultCode: Int, intent: Intent?): Uri? =
        if (resultCode == Activity.RESULT_OK) intent?.data else null
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun HomeScreen(viewModel: ConverterViewModel, onOpenFormats: () -> Unit, onOpenSettings: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val activity = androidx.activity.compose.LocalActivity.current
    val noApp = stringResource(R.string.message_no_app)

    val pickFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) viewModel.addUris(uris)
    }
    var pendingSave by rememberSaveable { androidx.compose.runtime.mutableLongStateOf(-1L) }
    val createDocument = rememberLauncherForActivityResult(CreateDocumentFor()) { uri ->
        if (uri != null && pendingSave >= 0) viewModel.save(pendingSave, uri)
        pendingSave = -1L
    }
    val pickFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) viewModel.saveAll(uri)
    }
    var picker by remember { mutableStateOf<PickerFor?>(null) }

    val actions = remember(viewModel) {
        CardActions(
            onPickTarget = { picker = PickerFor.Item(it) },
            onPickCompress = { picker = PickerFor.Compress(it) },
            onSetTarget = { id, target -> viewModel.store.setTarget(id, target) },
            onOption = { id, key, value -> viewModel.store.setOption(id, key, value) },
            onConvert = { viewModel.store.convert(it) },
            onCancel = { viewModel.store.cancel(it) },
            onRemove = { viewModel.store.remove(it) },
            onAgain = { viewModel.store.reset(it) },
            onSave = { id ->
                val output = (viewModel.store.item(id)?.status as? ItemStatus.Done)?.output
                if (output != null) {
                    pendingSave = id
                    runCatching { createDocument.launch(output.name to app.convertible.files.FileTransfers.documentMime(output)) }
                        .onFailure { viewModel.notify(UiMessage(noApp, isError = true)) }
                }
            },
            onShare = { id -> activity?.startSafely(viewModel.shareIntent(listOf(id))) { viewModel.notify(UiMessage(noApp, true)) } },
            onOpen = { id -> activity?.startSafely(viewModel.openIntent(id)) { viewModel.notify(UiMessage(noApp, true)) } },
        )
    }
    val chooseFiles = {
        runCatching { pickFiles.launch(arrayOf("*/*")) }.onFailure { viewModel.notify(UiMessage(noApp, true)) }
        Unit
    }

    // Drag and drop from other apps (split screen, tablets, desktop windowing).
    var dragActive by remember { mutableStateOf(false) }
    val dropTarget = remember(activity) {
        object : DragAndDropTarget {
            override fun onEntered(event: DragAndDropEvent) { dragActive = true }
            override fun onExited(event: DragAndDropEvent) { dragActive = false }
            override fun onEnded(event: DragAndDropEvent) { dragActive = false }
            override fun onDrop(event: DragAndDropEvent): Boolean {
                dragActive = false
                val dragEvent = event.toAndroidDragEvent()
                activity?.requestDragAndDropPermissions(dragEvent)
                val clip: ClipData = dragEvent.clipData ?: return false
                val uris = (0 until clip.itemCount).mapNotNull { clip.getItemAt(it).uri }
                if (uris.isEmpty()) return false
                viewModel.addUris(uris)
                return true
            }
        }
    }

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .dragAndDropTarget(shouldStartDragAndDrop = { it.mimeTypes().isNotEmpty() }, target = dropTarget)
            .testTag("home"),
    ) {
        val screenWidth = maxWidth
        val wide = screenWidth >= 840.dp
        if (wide) {
            Row(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Top)),
            ) {
                Column(
                    Modifier
                        .width((screenWidth * 0.4f).coerceIn(340.dp, 460.dp))
                        .fillMaxHeight()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = AppTheme.dimens.space8),
                ) {
                    TopBar(onOpenFormats, onOpenSettings, Modifier.padding(vertical = AppTheme.dimens.space4))
                    Spacer(Modifier.height(AppTheme.dimens.space7))
                    Hero()
                    Spacer(Modifier.height(AppTheme.dimens.space7))
                    DropZone(compact = false, dragActive = dragActive, onChoose = chooseFiles)
                    Spacer(Modifier.height(AppTheme.dimens.space6))
                    PrivacyNote()
                    Spacer(Modifier.height(AppTheme.dimens.space8))
                    if (!state.isEmpty) {
                        QueueActions(state, viewModel, onSaveAll = { runCatching { pickFolder.launch(null) } }, vertical = true)
                        Spacer(Modifier.height(AppTheme.dimens.space8))
                    }
                    Spacer(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom)))
                }
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .padding(top = AppTheme.dimens.space4, end = AppTheme.dimens.space4)
                        .clip(RoundedCornerShape(topStart = AppTheme.dimens.radiusXl, topEnd = AppTheme.dimens.radiusXl))
                        .background(AppTheme.colors.surfaceSunken),
                ) {
                    if (state.isEmpty) {
                        EmptyQueue(Modifier.align(Alignment.Center))
                    } else {
                        QueueList(state, actions, onGroupPick = { picker = PickerFor.Group(it) }, header = {}, columns = if (screenWidth >= 1200.dp) 2 else 1)
                    }
                }
            }
        } else {
            Column(Modifier.fillMaxSize()) {
                Box(Modifier.weight(1f)) {
                    QueueList(
                        state = state,
                        actions = actions,
                        onGroupPick = { picker = PickerFor.Group(it) },
                        columns = 1,
                        header = {
                            TopBar(onOpenFormats, onOpenSettings)
                            if (state.isEmpty) {
                                Spacer(Modifier.height(AppTheme.dimens.space8))
                                Hero()
                                Spacer(Modifier.height(AppTheme.dimens.space7))
                            } else {
                                Spacer(Modifier.height(AppTheme.dimens.space4))
                            }
                            DropZone(compact = !state.isEmpty, dragActive = dragActive, onChoose = chooseFiles)
                            Spacer(Modifier.height(AppTheme.dimens.space5))
                            PrivacyNote()
                            Spacer(Modifier.height(AppTheme.dimens.space6))
                        },
                    )
                }
                AnimatedVisibility(
                    visible = state.convertibleCount > 0 || state.activeCount > 0 || state.doneItems.isNotEmpty(),
                    enter = slideInVertically(tween(AppTheme.motion.slow, easing = AppTheme.motion.decelerate)) { it } + fadeIn(),
                    exit = slideOutVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.accelerate)) { it } + fadeOut(),
                ) {
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .background(AppTheme.colors.surface)
                            .border(1.dp, AppTheme.colors.border, RoundedCornerShape(topStart = AppTheme.dimens.radiusXl, topEnd = AppTheme.dimens.radiusXl))
                            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom + WindowInsetsSides.Horizontal))
                            .padding(horizontal = AppTheme.dimens.space6, vertical = AppTheme.dimens.space5),
                    ) {
                        QueueActions(state, viewModel, onSaveAll = { runCatching { pickFolder.launch(null) } }, vertical = false)
                    }
                }
            }
        }
    }

    when (val p = picker) {
        is PickerFor.Item -> state.items.firstOrNull { it.id == p.id }?.let { item ->
            TargetPicker(
                title = stringResource(R.string.picker_title, item.source?.label ?: item.name),
                targets = item.targets,
                sourceCategory = item.source?.category,
                catalogue = state.catalogue,
                selected = item.target?.id,
                onPick = { viewModel.store.setTarget(item.id, it.id); picker = null },
                onDismiss = { picker = null },
            )
        } ?: run { picker = null }
        is PickerFor.Group -> state.groups.firstOrNull { it.sourceId == p.sourceId }?.let { group ->
            TargetPicker(
                title = stringResource(R.string.picker_title, pluralLabel(group)),
                targets = group.targets,
                sourceCategory = state.items.firstOrNull { it.sourceId == group.sourceId }?.source?.category,
                catalogue = state.catalogue,
                selected = group.commonTarget?.id,
                onPick = { viewModel.store.setTargetForSource(group.sourceId, it.id); picker = null },
                onDismiss = { picker = null },
            )
        } ?: run { picker = null }
        is PickerFor.Compress -> state.items.firstOrNull { it.id == p.id }?.let { item ->
            CompressPicker(item = item, onPick = { viewModel.store.setTarget(item.id, it.id); picker = null }, onDismiss = { picker = null })
        } ?: run { picker = null }
        null -> Unit
    }
}

@Composable
private fun pluralLabel(group: SourceGroup): String = stringResource(R.string.group_title, group.itemIds.size, group.label)

@Composable
private fun TopBar(onOpenFormats: () -> Unit, onOpenSettings: () -> Unit, modifier: Modifier = Modifier) {
    val d = AppTheme.dimens
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StaticMark(size = 30.dp)
        Spacer(Modifier.width(d.space3))
        Text(stringResource(R.string.app_name), AppTheme.type.title3, AppTheme.colors.textPrimary, Modifier.semantics { heading() })
        Spacer(Modifier.weight(1f))
        IconButton(Icons.Formats, stringResource(R.string.nav_formats), onOpenFormats, Modifier.testTag("nav_formats"))
        IconButton(Icons.Settings, stringResource(R.string.nav_settings), onOpenSettings, Modifier.testTag("nav_settings"))
    }
}

@Composable
private fun Hero() {
    Column {
        Text(stringResource(R.string.home_title), AppTheme.type.title1, AppTheme.colors.textPrimary, Modifier.semantics { heading() })
        Spacer(Modifier.height(AppTheme.dimens.space3))
        Text(stringResource(R.string.home_subtitle), AppTheme.type.bodyLarge, AppTheme.colors.textSecondary)
    }
}

@Composable
private fun DropZone(compact: Boolean, dragActive: Boolean, onChoose: () -> Unit) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    val border by animateColorAsState(if (dragActive) colors.accent else colors.borderStrong.copy(alpha = 0.7f), tween(AppTheme.motion.fast), label = "dropBorder")
    val bg by animateColorAsState(if (dragActive) colors.accentSubtle else colors.surface, tween(AppTheme.motion.fast), label = "dropBg")
    val shape = RoundedCornerShape(d.radiusXl)
    Box(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(bg)
            .drawBehind {
                val stroke = 1.5.dp.toPx()
                drawRoundRect(
                    color = border,
                    style = Stroke(width = stroke, pathEffect = PathEffect.dashPathEffect(floatArrayOf(8.dp.toPx(), 6.dp.toPx()))),
                    cornerRadius = CornerRadius(d.radiusXl.toPx()),
                    topLeft = androidx.compose.ui.geometry.Offset(stroke / 2, stroke / 2),
                    size = androidx.compose.ui.geometry.Size(size.width - stroke, size.height - stroke),
                )
            }
            .testTag("drop_zone"),
    ) {
        if (compact) {
            Row(
                Modifier.fillMaxWidth().padding(d.space4),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    stringResource(if (dragActive) R.string.drop_active else R.string.drop_title_touch),
                    AppTheme.type.bodyStrong,
                    colors.textPrimary,
                    Modifier.weight(1f).padding(start = d.space3),
                )
                Button(stringResource(R.string.action_add_more), onChoose, kind = ButtonKind.SECONDARY, icon = Icons.Plus, compact = true, modifier = Modifier.testTag("choose_files"))
            }
        } else {
            Column(
                Modifier.fillMaxWidth().padding(horizontal = d.space7, vertical = d.space9),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                FormatOrbit()
                Spacer(Modifier.height(d.space7))
                Text(
                    stringResource(if (dragActive) R.string.drop_active else R.string.drop_title_touch),
                    AppTheme.type.title3,
                    colors.textPrimary,
                    textAlign = TextAlign.Center,
                )
                Spacer(Modifier.height(d.space3))
                Text(stringResource(R.string.drop_body), AppTheme.type.body, colors.textSecondary, textAlign = TextAlign.Center)
                Spacer(Modifier.height(d.space7))
                Button(
                    stringResource(R.string.action_choose_files),
                    onChoose,
                    kind = ButtonKind.PRIMARY,
                    icon = Icons.Plus,
                    modifier = Modifier.widthIn(min = 200.dp).testTag("choose_files"),
                )
            }
        }
    }
}

/** A small illustration: the mark with a round source chip on one side and a square target chip on the other. */
@Composable
private fun FormatOrbit() {
    val d = AppTheme.dimens
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(d.space4)) {
        FormatBadge("txt", "document", round = true)
        Icon(Icons.Arrow, null, AppTheme.colors.textDisabled, size = d.iconMd)
        StaticMark(size = 56.dp)
        Icon(Icons.Arrow, null, AppTheme.colors.textDisabled, size = d.iconMd)
        FormatBadge("b64", "encoding", round = false)
    }
}

@Composable
private fun PrivacyNote() {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusMd))
            .background(colors.successSubtle.copy(alpha = if (colors.isDark) 0.6f else 0.8f))
            .padding(horizontal = d.space5, vertical = d.space4)
            .testTag("privacy_note"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(Icons.Shield, null, colors.success, size = d.iconMd)
        Spacer(Modifier.width(d.space4))
        Text(stringResource(R.string.privacy_line), AppTheme.type.label, colors.textPrimary)
    }
}

@Composable
private fun EmptyQueue(modifier: Modifier) {
    Column(modifier.padding(AppTheme.dimens.space9), horizontalAlignment = Alignment.CenterHorizontally) {
        Icon(Icons.File, null, AppTheme.colors.textDisabled, size = 40.dp)
        Spacer(Modifier.height(AppTheme.dimens.space5))
        Text(stringResource(R.string.drop_title), AppTheme.type.bodyLarge, AppTheme.colors.textSecondary, textAlign = TextAlign.Center)
    }
}

@Composable
private fun QueueList(
    state: QueueState,
    actions: CardActions,
    onGroupPick: (String) -> Unit,
    columns: Int,
    header: @Composable () -> Unit,
) {
    val d = AppTheme.dimens
    val listState = rememberLazyListState()
    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxSize().testTag("queue"),
        contentPadding = PaddingValues(start = d.space6, end = d.space6, bottom = d.space8),
        verticalArrangement = Arrangement.spacedBy(d.space4),
    ) {
        item(key = "header") {
            Column(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top)).widthIn(max = 720.dp)) {
                header()
            }
        }
        if (state.groups.isNotEmpty()) {
            items(state.groups, key = { "group-" + it.sourceId }) { group ->
                GroupBar(group, onPick = { onGroupPick(group.sourceId) }, catalogueCategory = state.catalogue[group.sourceId]?.category ?: "other")
            }
        }
        val rows = state.items.chunked(columns)
        items(rows, key = { row -> row.joinToString("-") { it.id.toString() } }) { row ->
            Row(Modifier.animateItem(), horizontalArrangement = Arrangement.spacedBy(d.space4)) {
                row.forEach { item ->
                    Box(Modifier.weight(1f)) {
                        FileCard(item, state.catalogue, actions)
                    }
                }
                repeat(columns - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }
    }
}

@Composable
private fun GroupBar(group: SourceGroup, onPick: () -> Unit, catalogueCategory: String) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusLg))
            .background(colors.accentSubtle)
            .padding(horizontal = d.space5, vertical = d.space3)
            .testTag("group_${group.sourceId}"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FormatBadge(group.label, catalogueCategory)
        Spacer(Modifier.width(d.space4))
        Text(
            if (group.commonTarget != null) stringResource(R.string.group_applied, group.commonTarget.label)
            else stringResource(R.string.group_title, group.itemIds.size, group.label),
            AppTheme.type.label,
            colors.textPrimary,
            Modifier.weight(1f),
        )
        Button(stringResource(R.string.group_set_all), onPick, kind = ButtonKind.GHOST, compact = true, modifier = Modifier.testTag("group_set_all"))
    }
}

@Composable
private fun QueueActions(state: QueueState, viewModel: ConverterViewModel, onSaveAll: () -> Unit, vertical: Boolean) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val active = state.activeCount
    val convertible = state.convertibleCount
    val done = state.doneItems
    val noApp = stringResource(R.string.message_no_app)
    val activity = androidx.activity.compose.LocalActivity.current
    // The phone's bottom bar has room for two buttons; queueActions() picks them by importance.
    val actions = state.queueActions(limit = if (vertical) Int.MAX_VALUE else 2)
    val buttons: List<@Composable (Modifier) -> Unit> = actions.map { action ->
        when (action) {
            QueueAction.CANCEL_ALL -> { m: Modifier ->
                Button(stringResource(R.string.action_cancel_all), { viewModel.store.cancelAll() }, m.testTag("cancel_all"), kind = ButtonKind.SECONDARY, icon = Icons.Stop)
            }
            QueueAction.CONVERT_ALL -> { m: Modifier ->
                Button(
                    pluralStringResource(R.plurals.action_convert_n, convertible, convertible),
                    { viewModel.store.convertAll() },
                    m.testTag("convert_all"),
                    kind = ButtonKind.PRIMARY,
                )
            }
            QueueAction.SAVE_ALL -> { m: Modifier ->
                Button(stringResource(R.string.action_save_all), onSaveAll, m.testTag("save_all"), kind = if (convertible == 0 && active == 0) ButtonKind.PRIMARY else ButtonKind.SECONDARY, icon = Icons.Save)
            }
            QueueAction.SHARE_ALL -> { m: Modifier ->
                Button(
                    stringResource(R.string.action_share_all),
                    { activity?.startSafely(viewModel.shareIntent(done.map { it.id })) { viewModel.notify(UiMessage(noApp, true)) } },
                    m.testTag("share_all"),
                    kind = ButtonKind.SECONDARY,
                    icon = Icons.Share,
                )
            }
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(d.space3)) {
        if (active > 0) {
            Text(
                stringResource(R.string.converting_summary, state.items.count { it.status is ItemStatus.Converting }, active),
                AppTheme.type.caption,
                colors.textSecondary,
            )
        }
        if (vertical) {
            buttons.forEach { it(Modifier.fillMaxWidth()) }
        } else {
            Row(horizontalArrangement = Arrangement.spacedBy(d.space3)) {
                buttons.forEach { it(Modifier.weight(1f)) }
            }
        }
    }
}
