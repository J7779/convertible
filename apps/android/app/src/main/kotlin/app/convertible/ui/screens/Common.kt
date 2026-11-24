package app.convertible.ui.screens

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.convertible.R
import app.convertible.core.CoreError
import app.convertible.model.ErrorKind
import app.convertible.ui.components.IconButton
import app.convertible.ui.components.Icons
import app.convertible.ui.components.Text
import app.convertible.ui.theme.AppTheme

/** Top bar of secondary screens: back button and title, below the status bar. */
@Composable
fun SubScreenBar(title: String, onBack: () -> Unit) {
    val d = AppTheme.dimens
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
            .heightIn(min = 64.dp)
            .padding(horizontal = d.space3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(Icons.Back, stringResource(R.string.action_back), onBack, tint = AppTheme.colors.textPrimary)
        Spacer(Modifier.width(d.space2))
        Text(
            title,
            AppTheme.type.title3,
            AppTheme.colors.textPrimary,
            Modifier.semantics { heading() },
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun categoryName(category: String): String = when (category) {
    "image" -> stringResource(R.string.category_image)
    "audio" -> stringResource(R.string.category_audio)
    "video" -> stringResource(R.string.category_video)
    "document" -> stringResource(R.string.category_document)
    "ebook" -> stringResource(R.string.category_ebook)
    "spreadsheet" -> stringResource(R.string.category_spreadsheet)
    "presentation" -> stringResource(R.string.category_presentation)
    "data" -> stringResource(R.string.category_data)
    "archive" -> stringResource(R.string.category_archive)
    "font" -> stringResource(R.string.category_font)
    "subtitle" -> stringResource(R.string.category_subtitle)
    "model3d" -> stringResource(R.string.category_model3d)
    "geo" -> stringResource(R.string.category_geo)
    "pim" -> stringResource(R.string.category_pim)
    "code" -> stringResource(R.string.category_code)
    "encoding" -> stringResource(R.string.category_encoding)
    "vector" -> stringResource(R.string.category_vector)
    "other" -> stringResource(R.string.category_other)
    else -> category.replaceFirstChar { it.uppercase() }
}

@Composable
fun errorTitle(error: CoreError): String = when (ErrorKind.of(error)) {
    ErrorKind.INVALID_SETTINGS -> stringResource(R.string.error_invalid_settings)
    ErrorKind.UNRECOGNISED -> stringResource(R.string.error_unrecognised)
    ErrorKind.UNSUPPORTED -> stringResource(R.string.error_unsupported)
    ErrorKind.DAMAGED -> stringResource(R.string.error_damaged)
    ErrorKind.CANNOT_WRITE -> stringResource(R.string.error_cannot_write)
    ErrorKind.FILE_ACCESS -> stringResource(R.string.error_file_access)
    ErrorKind.CANCELLED -> stringResource(R.string.error_cancelled)
    ErrorKind.TOO_LARGE -> stringResource(R.string.error_too_large)
    ErrorKind.INTERNAL -> stringResource(R.string.error_internal)
}

/** The core's own message, made presentable (capitalised, with a full stop). Falls back for empty messages. */
@Composable
fun errorDetail(error: CoreError): String {
    val raw = error.detail.trim()
    if (raw.isEmpty()) {
        return if (ErrorKind.of(error) == ErrorKind.TOO_LARGE) stringResource(R.string.error_too_large_detail) else ""
    }
    val sentence = raw.replaceFirstChar { it.uppercase() }
    return if (sentence.last() in ".!?") sentence else "$sentence."
}
