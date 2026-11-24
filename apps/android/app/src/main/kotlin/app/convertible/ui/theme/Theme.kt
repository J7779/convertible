package app.convertible.ui.theme

import android.content.Context
import android.provider.Settings
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Easing
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.text.selection.LocalTextSelectionColors
import androidx.compose.foundation.text.selection.TextSelectionColors
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.LineHeightStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import app.convertible.R
import app.convertible.brand.BrandTokens
import app.convertible.brand.MotionSpec
import app.convertible.brand.TypeStyleToken

/** Tokens and storyboard, parsed once per process from the packaged copies of assets/brand. */
object Brand {
    @Volatile private var cached: Pair<BrandTokens, MotionSpec>? = null

    fun load(context: Context): Pair<BrandTokens, MotionSpec> = cached ?: synchronized(this) {
        cached ?: run {
            val assets = context.applicationContext.assets
            val tokens = BrandTokens.parse(assets.open("brand/tokens.json").bufferedReader().use { it.readText() })
            val motion = MotionSpec.parse(assets.open("brand/animation.json").bufferedReader().use { it.readText() })
            (tokens to motion).also { cached = it }
        }
    }

    /** "Remove animations" (animator duration scale 0) is Android's reduced-motion setting. */
    fun reducedMotion(context: Context): Boolean =
        Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
}

private fun c(argb: Long) = Color(argb.toInt())

/** UI colours of one theme, straight from `color.dark` / `color.light`. */
@Immutable
class Palette(val isDark: Boolean, private val tokens: BrandTokens) {
    private fun t(name: String) = c(tokens.color(isDark, name))
    val background = t("background")
    val surface = t("surface")
    val surfaceElevated = t("surfaceElevated")
    val surfaceSunken = t("surfaceSunken")
    val border = t("border")
    val borderStrong = t("borderStrong")
    val textPrimary = t("textPrimary")
    val textSecondary = t("textSecondary")
    val textDisabled = t("textDisabled")
    val accent = t("accent")
    val accentText = t("accentText")
    val accentFill = t("accentFill")
    val accentFillHover = t("accentFillHover")
    val accentFillPressed = t("accentFillPressed")
    val onAccent = t("onAccent")
    val accentSubtle = t("accentSubtle")
    val success = t("success")
    val successSubtle = t("successSubtle")
    val warning = t("warning")
    val warningSubtle = t("warningSubtle")
    val error = t("error")
    val errorSubtle = t("errorSubtle")
    val focusRing = t("focusRing")
    val scrim = t("scrim")

    /** The Flux gradient for this theme (deeper stops on light backgrounds). */
    val fluxStops: List<Pair<Float, Color>> =
        (if (isDark) tokens.gradient else tokens.gradientOnLight).map { it.first to c(it.second) }

    fun fluxHorizontal(): Brush = Brush.horizontalGradient(*fluxStops.toTypedArray())

    fun category(category: String): Color = c(tokens.category(isDark, category))

    /** Brand colour substitution for marks drawn on light backgrounds (Iris -> Iris Deep, ...). */
    fun markColor(argb: Long): Color {
        if (isDark) return c(argb)
        val b = tokens.brand
        return when (argb) {
            b["iris"] -> c(b.getValue("irisDeep"))
            b["orchid"] -> c(b.getValue("orchidDeep"))
            b["coral"] -> c(b.getValue("coralDeep"))
            else -> c(argb)
        }
    }
}

@Immutable
class Typography(tokens: BrandTokens) {
    val sans = FontFamily(
        Font(R.font.inter_regular, FontWeight.Normal),
        Font(R.font.inter_medium, FontWeight.Medium),
        Font(R.font.inter_semibold, FontWeight.SemiBold),
    )
    val mono = FontFamily(
        Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
        Font(R.font.jetbrains_mono_medium, FontWeight.Medium),
    )

    private fun style(token: TypeStyleToken?): TextStyle {
        val tk = token ?: TypeStyleToken(15.0, 22.0, 400, 0.0, mono = false, uppercase = false)
        return TextStyle(
            fontFamily = if (tk.mono) mono else sans,
            fontSize = tk.size.sp,
            lineHeight = tk.lineHeight.sp,
            fontWeight = FontWeight(tk.weight),
            letterSpacing = tk.letterSpacingEm.em,
            lineHeightStyle = LineHeightStyle(LineHeightStyle.Alignment.Center, LineHeightStyle.Trim.None),
        )
    }

    val display = style(tokens.type["display"])
    val title1 = style(tokens.type["title1"])
    val title2 = style(tokens.type["title2"])
    val title3 = style(tokens.type["title3"])
    val bodyLarge = style(tokens.type["bodyLarge"])
    val body = style(tokens.type["body"])
    val bodyStrong = style(tokens.type["bodyStrong"])
    val label = style(tokens.type["label"])
    val caption = style(tokens.type["caption"])
    /** Upper-case at the call site (textTransform in the tokens). */
    val overline = style(tokens.type["overline"])
    val monoText = style(tokens.type["mono"])
    /** Upper-case at the call site. */
    val formatLabel = style(tokens.type["formatLabel"])
}

/** Spacing, radii and sizes from the token scales. */
@Immutable
class Dimens(private val tokens: BrandTokens) {
    private fun s(key: String): Dp = (tokens.space[key] ?: 0.0).dp
    private fun r(key: String): Dp = (tokens.radius[key] ?: 0.0).dp
    val space1 = s("1"); val space2 = s("2"); val space3 = s("3"); val space4 = s("4"); val space5 = s("5")
    val space6 = s("6"); val space7 = s("7"); val space8 = s("8"); val space9 = s("9"); val space10 = s("10")
    val space11 = s("11"); val space12 = s("12")
    val radiusXs = r("xs"); val radiusSm = r("sm"); val radiusMd = r("md"); val radiusLg = r("lg")
    val radiusXl = r("xl"); val radiusXxl = r("xxl")
    val controlSm = (tokens.size["controlSm"] ?: 32.0).dp
    val controlMd = (tokens.size["controlMd"] ?: 40.0).dp
    val controlLg = (tokens.size["controlLg"] ?: 48.0).dp
    /** Android minimum touch target (tokens say 44; the platform guideline is 48 dp). */
    val touchTarget = maxOf((tokens.size["touchTarget"] ?: 44.0), 48.0).dp
    val iconSm = (tokens.size["iconSm"] ?: 16.0).dp
    val iconMd = (tokens.size["iconMd"] ?: 20.0).dp
    val iconLg = (tokens.size["iconLg"] ?: 24.0).dp
    val focusRingWidth = (tokens.size["focusRingWidth"] ?: 2.0).dp
    val focusRingOffset = (tokens.size["focusRingOffset"] ?: 2.0).dp
}

/** Durations and easings from `motion` in the tokens. */
@Immutable
class Motion(private val tokens: BrandTokens, val reduced: Boolean) {
    private fun d(key: String, fallback: Int) = if (reduced) 0 else tokens.durations[key] ?: fallback
    private fun e(key: String): Easing = tokens.easings[key]?.let { CubicBezierEasing(it[0].toFloat(), it[1].toFloat(), it[2].toFloat(), it[3].toFloat()) }
        ?: CubicBezierEasing(0.2f, 0f, 0f, 1f)
    val instant = d("instant", 80)
    val fast = d("fast", 140)
    val base = d("base", 220)
    val slow = d("slow", 360)
    val slower = d("slower", 560)
    val standard = e("standard")
    val decelerate = e("decelerate")
    val accelerate = e("accelerate")
    val inOut = e("inOut")
    val emphasized = e("emphasized")
    val settle = e("settle")
    val overshoot = e("overshoot")
}

@Immutable
class Theme(
    val colors: Palette,
    val type: Typography,
    val dimens: Dimens,
    val motion: Motion,
    val tokens: BrandTokens,
    val storyboard: MotionSpec,
)

val LocalTheme = staticCompositionLocalOf<Theme> { error("ConvertibleTheme is missing") }

object AppTheme {
    val colors: Palette @Composable get() = LocalTheme.current.colors
    val type: Typography @Composable get() = LocalTheme.current.type
    val dimens: Dimens @Composable get() = LocalTheme.current.dimens
    val motion: Motion @Composable get() = LocalTheme.current.motion
    val storyboard: MotionSpec @Composable get() = LocalTheme.current.storyboard
}

@Composable
fun ConvertibleTheme(
    dark: Boolean,
    tokens: BrandTokens,
    storyboard: MotionSpec,
    reducedMotion: Boolean,
    content: @Composable () -> Unit,
) {
    val theme = remember(dark, tokens, storyboard, reducedMotion) {
        Theme(Palette(dark, tokens), Typography(tokens), Dimens(tokens), Motion(tokens, reducedMotion), tokens, storyboard)
    }
    val selection = remember(theme) { TextSelectionColors(theme.colors.accent, theme.colors.accent.copy(alpha = 0.3f)) }
    val indication = remember(theme) { BrandIndication(theme.colors.textPrimary) }
    CompositionLocalProvider(
        LocalTheme provides theme,
        LocalTextSelectionColors provides selection,
        LocalIndication provides indication,
        content = content,
    )
}
