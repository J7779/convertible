package app.convertible.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import app.convertible.R
import app.convertible.ui.components.Button
import app.convertible.ui.components.ButtonKind
import app.convertible.ui.components.Text
import app.convertible.ui.theme.AppTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private data class Notice(val name: String, val licence: String, val detail: String, val asset: String?)

private const val APACHE_NOTICE = "Licensed under the Apache License, Version 2.0 (the \"License\"); you may not use these files " +
    "except in compliance with the License. You may obtain a copy of the License at " +
    "https://www.apache.org/licenses/LICENSE-2.0. Unless required by applicable law or agreed to in writing, software " +
    "distributed under the License is distributed on an \"AS IS\" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, " +
    "either express or implied. See the License for the specific language governing permissions and limitations under the License."

private val NOTICES = listOf(
    Notice("Convertible", "MIT", "The app and its conversion engine.", "licenses/Convertible-LICENSE.txt"),
    Notice("Inter", "SIL Open Font License 1.1", "Interface typeface, by Rasmus Andersson.", "licenses/Inter-LICENSE.txt"),
    Notice("JetBrains Mono", "SIL Open Font License 1.1", "Typeface for file names and formats, by JetBrains.", "licenses/JetBrainsMono-LICENSE.txt"),
    Notice("AndroidX (Activity, Compose, Core, Lifecycle)", "Apache License 2.0", "Copyright The Android Open Source Project. $APACHE_NOTICE", null),
    Notice("Kotlin standard library and kotlinx.coroutines", "Apache License 2.0", "Copyright JetBrains s.r.o. and Kotlin Programming Language contributors. $APACHE_NOTICE", null),
)

@Composable
fun LicencesScreen(onBack: () -> Unit) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    Column(Modifier.fillMaxSize().testTag("licences_screen")) {
        SubScreenBar(stringResource(R.string.licences_title), onBack)
        LazyColumn(
            Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)),
            contentPadding = PaddingValues(start = d.space6, end = d.space6, bottom = d.space10),
        ) {
            item { Text(stringResource(R.string.licences_intro), AppTheme.type.body, colors.textSecondary, Modifier.padding(bottom = d.space6).widthIn(max = 680.dp)) }
            items(NOTICES) { notice ->
                LicenceCard(notice)
                Spacer(Modifier.height(d.space4))
            }
            item { Spacer(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Bottom))) }
        }
    }
}

@Composable
private fun LicenceCard(notice: Notice) {
    val d = AppTheme.dimens
    val colors = AppTheme.colors
    val context = LocalContext.current
    var open by rememberSaveable(notice.name) { mutableStateOf(false) }
    val text by produceState<String?>(null, open, notice.asset) {
        value = if (open && notice.asset != null) {
            withContext(Dispatchers.IO) { runCatching { context.assets.open(notice.asset).bufferedReader().use { it.readText() } }.getOrNull() }
        } else {
            null
        }
    }
    Column(
        Modifier
            .widthIn(max = 680.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(d.radiusLg))
            .background(colors.surface)
            .border(1.dp, colors.border, RoundedCornerShape(d.radiusLg))
            .padding(d.space5),
    ) {
        Text(notice.name, AppTheme.type.bodyStrong, colors.textPrimary, Modifier.semantics { heading() })
        Text(notice.licence, AppTheme.type.label, colors.accentText)
        Spacer(Modifier.height(d.space3))
        Text(notice.detail, AppTheme.type.label, colors.textSecondary)
        if (notice.asset != null) {
            Spacer(Modifier.height(d.space3))
            Button(
                stringResource(if (open) R.string.licence_hide else R.string.licence_show),
                { open = !open },
                kind = ButtonKind.GHOST,
                compact = true,
            )
            text?.let {
                Text(
                    it.trim(),
                    AppTheme.type.caption.copy(fontFamily = AppTheme.type.mono),
                    colors.textSecondary,
                    Modifier
                        .clip(RoundedCornerShape(d.radiusMd))
                        .background(colors.surfaceSunken)
                        .padding(d.space4),
                )
            }
        }
    }
}
