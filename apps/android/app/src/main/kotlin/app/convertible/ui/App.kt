package app.convertible.ui

import android.app.Activity
import android.graphics.Color as AndroidColor
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.PredictiveBackHandler
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.convertible.R
import app.convertible.brand.BrandTokens
import app.convertible.brand.MotionSpec
import app.convertible.state.ThemeMode
import app.convertible.ui.components.Icon
import app.convertible.ui.components.Icons
import app.convertible.ui.components.MarkGeometry
import app.convertible.ui.components.Text
import app.convertible.ui.components.drawStoryboard
import app.convertible.ui.screens.FormatsScreen
import app.convertible.ui.screens.HomeScreen
import app.convertible.ui.screens.LicencesScreen
import app.convertible.ui.screens.SettingsScreen
import app.convertible.ui.theme.AppTheme
import app.convertible.ui.theme.Brand
import app.convertible.ui.theme.ConvertibleTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

enum class Screen { HOME, FORMATS, SETTINGS, LICENCES }

@Composable
fun ConvertibleApp(viewModel: ConverterViewModel, tokens: BrandTokens, storyboard: MotionSpec, playIntro: Boolean) {
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    val dark = when (settings.theme) {
        ThemeMode.SYSTEM -> isSystemInDarkTheme()
        ThemeMode.DARK -> true
        ThemeMode.LIGHT -> false
    }
    val context = LocalContext.current
    val reducedMotion = remember { Brand.reducedMotion(context) }

    // System bar icons follow the app theme, which may differ from the system one.
    DisposableEffect(dark) {
        (context as? ComponentActivity)?.let { activity ->
            val style = if (dark) {
                SystemBarStyle.dark(AndroidColor.TRANSPARENT)
            } else {
                SystemBarStyle.light(AndroidColor.TRANSPARENT, AndroidColor.TRANSPARENT)
            }
            activity.enableEdgeToEdge(style, style)
        }
        onDispose { }
    }

    ConvertibleTheme(dark = dark, tokens = tokens, storyboard = storyboard, reducedMotion = reducedMotion) {
        var introVisible by rememberSaveable { mutableStateOf(playIntro) }
        Box(Modifier.fillMaxSize().background(AppTheme.colors.background)) {
            Navigator(viewModel)
            MessageBanner(viewModel, Modifier.align(Alignment.TopCenter))
            AnimatedVisibility(
                visible = introVisible,
                enter = fadeIn(tween(0)),
                exit = fadeOut(tween(AppTheme.motion.base, easing = AppTheme.motion.standard)),
            ) {
                IntroOverlay(onFinished = { introVisible = false })
            }
        }
    }
}

/**
 * A minimal stack navigator with predictive back: while the back gesture is in progress the top screen shrinks
 * and slides with the finger over the screen below it; releasing completes the pop, cancelling springs back.
 */
@Composable
private fun Navigator(viewModel: ConverterViewModel) {
    var stack by rememberSaveable { mutableStateOf(listOf(Screen.HOME.name)) }
    val holder = rememberSaveableStateHolder()
    val scope = rememberCoroutineScope()
    val motion = AppTheme.motion
    val back = remember { Animatable(0f) }
    val enter = remember { Animatable(1f) }
    var transitioning by remember { mutableStateOf(false) }

    fun push(screen: Screen) {
        if (stack.last() == screen.name) return
        stack = stack + screen.name
        scope.launch {
            enter.snapTo(0f)
            enter.animateTo(1f, tween(motion.slow, easing = motion.decelerate))
        }
    }

    fun popAnimated() {
        if (stack.size <= 1 || transitioning) return
        transitioning = true
        scope.launch {
            back.animateTo(1f, tween(motion.base, easing = motion.accelerate))
            stack = stack.dropLast(1)
            back.snapTo(0f)
            transitioning = false
        }
    }

    PredictiveBackHandler(enabled = stack.size > 1) { events ->
        transitioning = true
        try {
            events.collect { back.snapTo(it.progress) }
            back.animateTo(1f, tween(motion.fast, easing = motion.standard))
            stack = stack.dropLast(1)
            back.snapTo(0f)
        } catch (e: CancellationException) {
            back.animateTo(0f, tween(motion.base, easing = motion.standard))
            throw e
        } finally {
            transitioning = false
        }
    }

    val screens = stack.map { Screen.valueOf(it) }
    val top = screens.last()
    val below = screens.getOrNull(screens.size - 2)

    @Composable
    fun render(screen: Screen) {
        holder.SaveableStateProvider(screen.name) {
            when (screen) {
                Screen.HOME -> HomeScreen(viewModel, onOpenFormats = { push(Screen.FORMATS) }, onOpenSettings = { push(Screen.SETTINGS) })
                Screen.FORMATS -> FormatsScreen(viewModel, onBack = ::popAnimated)
                Screen.SETTINGS -> SettingsScreen(viewModel, onBack = ::popAnimated, onOpenLicences = { push(Screen.LICENCES) })
                Screen.LICENCES -> LicencesScreen(onBack = ::popAnimated)
            }
        }
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val width = constraints.maxWidth.toFloat()
        val progress = back.value
        val entering = enter.value
        if (below != null && (progress > 0f || entering < 1f)) {
            Box(
                Modifier
                    .fillMaxSize()
                    .graphicsLayer {
                        val k = if (progress > 0f) progress else 1f - entering
                        val s = 0.97f + 0.03f * (1f - k)
                        scaleX = s
                        scaleY = s
                        alpha = 0.6f + 0.4f * (if (progress > 0f) progress else 1f - entering)
                    },
            ) { render(below) }
        }
        val radius = (24f * progress).dp
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer {
                    if (progress > 0f) {
                        val s = 1f - 0.1f * progress
                        scaleX = s
                        scaleY = s
                        translationX = width * 0.12f * progress
                        alpha = 1f - 0.35f * progress
                    } else if (entering < 1f && below != null) {
                        translationX = width * 0.08f * (1f - entering)
                        alpha = entering
                    }
                }
                .clip(RoundedCornerShape(radius))
                .background(AppTheme.colors.background),
        ) { render(top) }
    }
}

/** The 1.8 s logo intro from animation.json; tap or any key skips it. Reduced motion shows the static mark. */
@Composable
private fun IntroOverlay(onFinished: () -> Unit) {
    val spec = AppTheme.storyboard
    val colors = AppTheme.colors
    val reduced = AppTheme.motion.reduced
    val geo = remember(spec) { MarkGeometry(spec) }
    var time by remember { mutableFloatStateOf(if (reduced) spec.intro.durationMs.toFloat() else 0f) }
    val focus = remember { FocusRequester() }
    val skipLabel = stringResource(R.string.intro_skip)
    val description = stringResource(R.string.intro_description)
    LaunchedEffect(Unit) {
        runCatching { focus.requestFocus() }
        if (reduced) {
            delay(500)
        } else {
            val start = withFrameMillis { it }
            val duration = spec.intro.durationMs
            while (true) {
                val now = withFrameMillis { it }
                time = (now - start).toFloat()
                if (time >= duration) break
            }
            delay(120)
        }
        onFinished()
    }
    val fade = remember { Animatable(if (reduced) 0f else 1f) }
    LaunchedEffect(reduced) { if (reduced) fade.animateTo(1f, tween(140)) }
    Box(
        Modifier
            .fillMaxSize()
            .background(colors.background)
            .testTag("intro")
            .focusRequester(focus)
            .focusable()
            .onKeyEvent { if (it.type == KeyEventType.KeyDown) onFinished(); true }
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onFinished)
            .semantics {
                contentDescription = description
                onClick(label = skipLabel) { onFinished(); true }
            },
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.foundation.Canvas(Modifier.size(168.dp).graphicsLayer { alpha = fade.value }) {
            drawStoryboard(spec.intro, time.toDouble(), geo, colors)
        }
    }
}

/** Transient confirmation / error pill above the navigation bar. */
@Composable
private fun MessageBanner(viewModel: ConverterViewModel, modifier: Modifier) {
    var message by remember { mutableStateOf<UiMessage?>(null) }
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(viewModel) {
        viewModel.messages.collect {
            message = it
            visible = true
            delay(3200)
            visible = false
        }
    }
    val colors = AppTheme.colors
    val d = AppTheme.dimens
    AnimatedVisibility(
        visible = visible,
        modifier = modifier,
        enter = slideInVertically(tween(AppTheme.motion.slow, easing = AppTheme.motion.decelerate)) { -it / 2 } + fadeIn(tween(AppTheme.motion.base)),
        exit = slideOutVertically(tween(AppTheme.motion.base, easing = AppTheme.motion.accelerate)) { -it / 2 } + fadeOut(tween(AppTheme.motion.fast)),
    ) {
        val m = message ?: return@AnimatedVisibility
        Row(
            Modifier
                .windowInsetsPadding(WindowInsets.statusBars)
                .padding(d.space7)
                .widthIn(max = 560.dp)
                .fillMaxWidth()
                .clip(RoundedCornerShape(d.radiusLg))
                .background(if (m.isError) colors.errorSubtle else colors.surfaceElevated)
                .border(1.dp, colors.borderStrong.copy(alpha = 0.4f), RoundedCornerShape(d.radiusLg))
                .clickable { visible = false }
                .semantics { liveRegion = LiveRegionMode.Polite }
                .padding(horizontal = d.space6, vertical = d.space5),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(if (m.isError) Icons.Error else Icons.Check, null, if (m.isError) colors.error else colors.success, size = d.iconMd)
            Spacer(Modifier.width(d.space4))
            Text(m.text, AppTheme.type.body, colors.textPrimary)
        }
    }
}

/** Starts an activity, reporting a friendly message when nothing can handle it. */
fun Activity.startSafely(intent: android.content.Intent?, onFailure: () -> Unit) {
    if (intent == null) return
    try {
        startActivity(intent)
    } catch (e: android.content.ActivityNotFoundException) {
        onFailure()
    }
}
