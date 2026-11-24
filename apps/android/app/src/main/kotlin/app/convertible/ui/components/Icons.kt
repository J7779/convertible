package app.convertible.ui.components

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/** A small hand-drawn line icon set on a 24-unit grid (stroke 1.75, round caps), tinted at the call site. */
object Icons {
    private fun icon(name: String, vararg paths: String): ImageVector {
        val builder = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
        for (d in paths) {
            builder.addPath(
                pathData = PathParser().parsePathString(d).toNodes(),
                stroke = SolidColor(Color.Black),
                strokeLineWidth = 1.75f,
                strokeLineCap = StrokeCap.Round,
                strokeLineJoin = StrokeJoin.Round,
            )
        }
        return builder.build()
    }

    val Plus by lazy { icon("plus", "M12 5v14M5 12h14") }
    val Close by lazy { icon("close", "M6.5 6.5l11 11M17.5 6.5l-11 11") }
    val Settings by lazy {
        icon("settings", "M4 7h9M17 7h3M4 17h3M11 17h9", "M15 5a2 2 0 1 0 0 4a2 2 0 1 0 0-4z", "M9 15a2 2 0 1 0 0 4a2 2 0 1 0 0-4z")
    }
    val Formats by lazy { icon("formats", "M4.5 4.5h6v6h-6z", "M13.5 4.5h6v6h-6z", "M4.5 13.5h6v6h-6z", "M16.5 13.5a3 3 0 1 0 0.01 0z") }
    val Search by lazy { icon("search", "M11 4.5a6.5 6.5 0 1 0 0 13a6.5 6.5 0 1 0 0-13z", "M16 16l4 4") }
    val Back by lazy { icon("back", "M14.5 5.5L8 12l6.5 6.5") }
    val ChevronDown by lazy { icon("chevron_down", "M6.5 9.5L12 15l5.5-5.5") }
    val ChevronRight by lazy { icon("chevron_right", "M9.5 6.5L15 12l-5.5 5.5") }
    val Check by lazy { icon("check", "M5 12.5l4.5 4.5L19 7.5") }
    val Share by lazy { icon("share", "M12 3.5v11", "M8 7.5l4-4 4 4", "M5.5 12.5v7h13v-7") }
    val Save by lazy { icon("save", "M12 4v11", "M8 11l4 4 4-4", "M5 19.5h14") }
    val Open by lazy { icon("open", "M13.5 4.5h6v6", "M19.5 4.5l-8 8", "M17.5 14v5.5h-13v-13H10") }
    val Again by lazy { icon("again", "M19 12a7 7 0 1 1-2.05-4.95", "M19 4.5v4h-4") }
    val Remove by lazy { icon("remove", "M5 7h14", "M10 7V4.5h4V7", "M7 7l1 12.5h8L17 7") }
    val Warning by lazy { icon("warning", "M12 4L21 19.5H3z", "M12 10v4.5", "M12 17.2v0.1") }
    val Error by lazy { icon("error", "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 1 0 0-17z", "M12 8v5", "M12 16v0.1") }
    val Shield by lazy { icon("shield", "M12 3.5l7 2.8v5.4c0 4.3-2.9 7.3-7 8.8-4.1-1.5-7-4.5-7-8.8V6.3z", "M9 12l2.2 2.2L15.5 10") }
    val Arrow by lazy { icon("arrow", "M5 12h14", "M13.5 6.5L19 12l-5.5 5.5") }
    val Compress by lazy { icon("compress", "M4.5 9.5h5v-5", "M19.5 9.5h-5v-5", "M4.5 14.5h5v5", "M19.5 14.5h-5v5") }
    val Info by lazy { icon("info", "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 1 0 0-17z", "M12 11v5", "M12 8v0.1") }
    val File by lazy { icon("file", "M6.5 3.5h7.5l4 4v13h-11.5z", "M13.5 3.5v4.5h4.5") }
    val Stop by lazy { icon("stop", "M7.5 7.5h9v9h-9z") }
    val Sliders by lazy { icon("sliders", "M6 4v16M12 4v16M18 4v16", "M4 14h4M10 8h4M16 16h4") }
}

@Composable
fun Icon(
    vector: ImageVector,
    contentDescription: String?,
    tint: Color,
    modifier: Modifier = Modifier,
    size: Dp = 24.dp,
) {
    Image(
        painter = rememberVectorPainter(vector),
        contentDescription = contentDescription,
        colorFilter = ColorFilter.tint(tint),
        modifier = modifier.size(size),
    )
}
