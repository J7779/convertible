package app.convertible.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.convertible.BuildConfig
import app.convertible.R
import app.convertible.model.Preset
import app.convertible.state.ThemeMode
import app.convertible.ui.ConverterViewModel
import app.convertible.ui.components.Icon
import app.convertible.ui.components.Icons
import app.convertible.ui.components.SectionLabel
import app.convertible.ui.components.SegmentedControl
import app.convertible.ui.components.StaticMark
import app.convertible.ui.components.SwitchRow
import app.convertible.ui.components.Text
import app.convertible.ui.components.focusRing
import app.convertible.ui.theme.AppTheme

@Composable
fun SettingsScreen(viewModel: ConverterViewModel, onBack: () -> Unit, onOpenLicences: () -> Unit) {
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    val state by viewModel.state.collectAsStateWithLifecycle()
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    Column(Modifier.fillMaxSize().testTag("settings_screen")) {
        SubScreenBar(stringResource(R.string.settings_title), onBack)
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))
                .padding(horizontal = d.space6)
                .padding(bottom = d.space9),
        ) {
            Column(Modifier.widthIn(max = 680.dp), verticalArrangement = Arrangement.spacedBy(d.space4)) {
                SectionLabel(stringResource(R.string.settings_appearance), Modifier.padding(top = d.space4))
                Group {
                    Text(stringResource(R.string.settings_theme), AppTheme.type.body, colors.textPrimary)
                    Spacer(Modifier.height(d.space3))
                    SegmentedControl(
                        options = listOf(ThemeMode.SYSTEM, ThemeMode.DARK, ThemeMode.LIGHT),
                        selected = settings.theme,
                        label = {
                            when (it) {
                                ThemeMode.SYSTEM -> stringResource(R.string.theme_system)
                                ThemeMode.DARK -> stringResource(R.string.theme_dark)
                                ThemeMode.LIGHT -> stringResource(R.string.theme_light)
                            }
                        },
                        onSelect = { mode -> viewModel.updateSettings { it.copy(theme = mode) } },
                        testTagPrefix = "theme_",
                    )
                }

                SectionLabel(stringResource(R.string.settings_defaults), Modifier.padding(top = d.space6))
                Group {
                    Text(stringResource(R.string.settings_default_quality), AppTheme.type.body, colors.textPrimary)
                    Text(stringResource(R.string.settings_default_quality_hint), AppTheme.type.caption, colors.textSecondary)
                    Spacer(Modifier.height(d.space3))
                    SegmentedControl(
                        options = Preset.entries.toList(),
                        selected = settings.defaultPreset,
                        label = { presetLabel(it) },
                        onSelect = { p -> viewModel.updateSettings { it.copy(defaultPreset = p) } },
                        testTagPrefix = "default_preset_",
                    )
                    Spacer(Modifier.height(d.space4))
                    SwitchRow(
                        title = stringResource(R.string.settings_strip_metadata),
                        subtitle = stringResource(R.string.settings_strip_metadata_hint),
                        checked = settings.stripMetadata,
                        onChange = { v -> viewModel.updateSettings { it.copy(stripMetadata = v) } },
                        modifier = Modifier.testTag("strip_metadata"),
                    )
                }

                SectionLabel(stringResource(R.string.settings_privacy), Modifier.padding(top = d.space6))
                Group {
                    Row(verticalAlignment = Alignment.Top) {
                        Icon(Icons.Shield, null, colors.success, size = d.iconLg)
                        Spacer(Modifier.width(d.space4))
                        Text(stringResource(R.string.settings_privacy_body), AppTheme.type.body, colors.textPrimary)
                    }
                }

                SectionLabel(stringResource(R.string.settings_about), Modifier.padding(top = d.space6))
                Group {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        StaticMark(size = 40.dp)
                        Spacer(Modifier.width(d.space4))
                        Column {
                            Text(stringResource(R.string.app_name), AppTheme.type.title3, colors.textPrimary)
                            Text(stringResource(R.string.settings_source), AppTheme.type.caption, colors.textSecondary)
                        }
                    }
                    Spacer(Modifier.height(d.space5))
                    InfoRow(stringResource(R.string.settings_app_version), BuildConfig.VERSION_NAME, "app_version")
                    InfoRow(stringResource(R.string.settings_core_version), state.coreVersion.ifEmpty { "…" }, "core_version")
                    Spacer(Modifier.height(d.space3))
                    val interaction = remember { MutableInteractionSource() }
                    Row(
                        Modifier
                            .focusRing(interaction, RoundedCornerShape(d.radiusMd))
                            .fillMaxWidth()
                            .heightIn(min = d.touchTarget)
                            .clip(RoundedCornerShape(d.radiusMd))
                            .clickable(interaction, androidx.compose.foundation.LocalIndication.current, role = Role.Button, onClick = onOpenLicences)
                            .padding(horizontal = d.space3)
                            .testTag("open_licences"),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(stringResource(R.string.settings_licences), AppTheme.type.body, colors.accentText, Modifier.weight(1f))
                        Icon(Icons.ChevronRight, null, colors.textSecondary, size = d.iconMd)
                    }
                }
            }
        }
    }
}

@Composable
private fun Group(content: @Composable () -> Unit) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusLg))
            .background(colors.surface)
            .border(1.dp, colors.border, RoundedCornerShape(d.radiusLg))
            .padding(d.space5),
    ) { content() }
}

@Composable
private fun InfoRow(label: String, value: String, tag: String) {
    val d = AppTheme.dimens
    Row(Modifier.fillMaxWidth().padding(horizontal = d.space3, vertical = d.space3), verticalAlignment = Alignment.CenterVertically) {
        Text(label, AppTheme.type.body, AppTheme.colors.textSecondary, Modifier.weight(1f))
        Text(value, AppTheme.type.monoText, AppTheme.colors.textPrimary, Modifier.testTag(tag))
    }
}
