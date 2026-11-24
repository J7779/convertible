package app.convertible.ui.screens

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.convertible.R
import app.convertible.model.Categories
import app.convertible.model.FormatInfo
import app.convertible.model.TargetRanking
import app.convertible.ui.ConverterViewModel
import app.convertible.ui.components.Chip
import app.convertible.ui.components.FormatBadge
import app.convertible.ui.components.Icon
import app.convertible.ui.components.Icons
import app.convertible.ui.components.MarkSpinner
import app.convertible.ui.components.Text
import app.convertible.ui.components.TextInput
import app.convertible.ui.theme.AppTheme

/** Searchable catalogue of every format the core knows, by category, with read/write indicators. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun FormatsScreen(viewModel: ConverterViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val catalogue = state.catalogue
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    var query by rememberSaveable { mutableStateOf("") }
    var category by rememberSaveable { mutableStateOf<String?>(null) }
    val filtered = remember(catalogue, query, category) {
        TargetRanking.searchFormats(catalogue.formats, query).filter { category == null || it.category == category }
    }
    val grouped = remember(filtered, query) {
        if (query.isNotBlank()) listOf(null to filtered)
        else filtered.groupBy { it.category }.toList().sortedBy { Categories.rank(it.first) }.map { it.first as String? to it.second }
    }
    Column(Modifier.fillMaxSize().testTag("formats_screen")) {
        SubScreenBar(stringResource(R.string.formats_title), onBack)
        LazyColumn(
            Modifier.fillMaxSize().testTag("formats_list"),
            contentPadding = PaddingValues(bottom = d.space9),
        ) {
            item(key = "intro") {
                Column(Modifier.padding(horizontal = d.space6).widthIn(max = 760.dp)) {
                    Text(stringResource(R.string.formats_subtitle), AppTheme.type.body, colors.textSecondary)
                    Spacer(Modifier.size(d.space5))
                    TextInput(
                        value = query,
                        onChange = { query = it },
                        placeholder = stringResource(R.string.picker_search),
                        leading = Icons.Search,
                        onClear = { query = "" },
                        clearLabel = stringResource(R.string.picker_clear_search),
                        modifier = Modifier.testTag("formats_search"),
                    )
                }
            }
            item(key = "filters") {
                LazyRow(
                    contentPadding = PaddingValues(horizontal = d.space6, vertical = d.space4),
                    horizontalArrangement = Arrangement.spacedBy(d.space3),
                ) {
                    item { Chip(stringResource(R.string.formats_all), category == null, { category = null }) }
                    items(catalogue.categories, key = { it }) { c ->
                        Chip(
                            categoryName(c),
                            category == c,
                            { category = if (category == c) null else c },
                            leading = { Box(Modifier.size(8.dp).clip(RoundedCornerShape(50)).background(colors.category(c))) },
                        )
                    }
                }
            }
            if (catalogue.formats.isEmpty()) {
                item(key = "loading") { Box(Modifier.fillMaxWidth().padding(d.space10), contentAlignment = Alignment.Center) { MarkSpinner(size = 48.dp) } }
            } else if (filtered.isEmpty()) {
                item(key = "empty") {
                    Text(stringResource(R.string.formats_empty, query), AppTheme.type.body, colors.textSecondary, Modifier.padding(d.space8))
                }
            }
            grouped.forEach { (cat, formats) ->
                if (cat != null) {
                    // Pinned while its (possibly hundreds of) rows scroll under it.
                    stickyHeader(key = "h-$cat", contentType = "header") {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .background(colors.background)
                                .padding(start = d.space6, end = d.space6, top = d.space7, bottom = d.space3),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Box(Modifier.size(8.dp).clip(RoundedCornerShape(50)).background(colors.category(cat)))
                            Spacer(Modifier.width(d.space3))
                            Text(categoryName(cat).uppercase(), AppTheme.type.overline, colors.textSecondary, Modifier.semantics { heading() })
                            Spacer(Modifier.width(d.space3))
                            Text(formats.size.toString(), AppTheme.type.caption, colors.textDisabled)
                        }
                    }
                }
                items(formats, key = { "f-${it.id}" }, contentType = { "format" }) { FormatRow(it) }
            }
            item(key = "note") {
                Row(Modifier.padding(horizontal = d.space6, vertical = d.space7).widthIn(max = 760.dp)) {
                    Icon(Icons.Info, null, colors.textSecondary, size = d.iconMd)
                    Spacer(Modifier.width(d.space3))
                    Text(stringResource(R.string.formats_universal_note), AppTheme.type.label, colors.textSecondary)
                }
                Spacer(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom)))
            }
        }
    }
}

@Composable
private fun FormatRow(format: FormatInfo) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val description = stringResource(
        R.string.formats_row_description,
        format.name,
        format.extensions.joinToString(", ") { ".$it" },
        stringResource(if (format.readable) R.string.formats_can_read else R.string.formats_cannot_read),
        stringResource(if (format.writable) R.string.formats_can_write else R.string.formats_cannot_write),
    )
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .padding(horizontal = d.space6, vertical = d.space2)
            .testTag("format_${format.id}")
            .clearAndSetSemantics { contentDescription = description },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(116.dp)) { FormatBadge(format.label, format.category) }
        Spacer(Modifier.width(d.space3))
        Column(Modifier.weight(1f)) {
            Text(format.name, AppTheme.type.body, colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                format.extensions.joinToString("  ") { ".$it" },
                AppTheme.type.caption.copy(fontFamily = AppTheme.type.mono),
                colors.textSecondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Spacer(Modifier.width(d.space3))
        Capability(stringResource(R.string.formats_read), format.readable)
        Spacer(Modifier.width(d.space2))
        Capability(stringResource(R.string.formats_write), format.writable)
    }
}

@Composable
private fun Capability(label: String, available: Boolean) {
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    Row(
        Modifier
            .clip(RoundedCornerShape(d.radiusXs))
            .background(if (available) colors.successSubtle else Color.Transparent)
            .padding(horizontal = d.space3, vertical = d.space1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (available) {
            Icon(Icons.Check, null, colors.success, size = 12.dp)
            Spacer(Modifier.width(d.space1))
        }
        Text(label, AppTheme.type.caption, if (available) colors.success else colors.textDisabled)
    }
}
