package app.convertible.ui.theme

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.interaction.FocusInteraction
import androidx.compose.foundation.interaction.HoverInteraction
import androidx.compose.foundation.interaction.Interaction
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import kotlinx.coroutines.launch

/**
 * The app's press / hover feedback: a soft tint that eases in and out (no Material ripple). Components clip to
 * their shape before `clickable`, so the tint follows the shape. Keyboard focus gets a ring from [focusRing].
 */
class BrandIndication(private val tint: Color) : IndicationNodeFactory {
    override fun create(interactionSource: InteractionSource): DelegatableNode = Node(interactionSource, tint)

    override fun equals(other: Any?): Boolean = other is BrandIndication && other.tint == tint
    override fun hashCode(): Int = tint.hashCode()

    private class Node(private val source: InteractionSource, private val tint: Color) : Modifier.Node(), DrawModifierNode {
        private val alpha = Animatable(0f)
        private val easing = CubicBezierEasing(0.2f, 0f, 0f, 1f)

        override fun onAttach() {
            coroutineScope.launch {
                val pressed = ArrayList<PressInteraction.Press>()
                val hovered = ArrayList<HoverInteraction.Enter>()
                var focused = 0
                source.interactions.collect { i: Interaction ->
                    when (i) {
                        is PressInteraction.Press -> pressed += i
                        is PressInteraction.Release -> pressed -= i.press
                        is PressInteraction.Cancel -> pressed -= i.press
                        is HoverInteraction.Enter -> hovered += i
                        is HoverInteraction.Exit -> hovered -= i.enter
                        is FocusInteraction.Focus -> focused++
                        is FocusInteraction.Unfocus -> focused--
                    }
                    val target = when {
                        pressed.isNotEmpty() -> 0.12f
                        focused > 0 -> 0.08f
                        hovered.isNotEmpty() -> 0.05f
                        else -> 0f
                    }
                    launch { alpha.animateTo(target, tween(if (target > alpha.value) 80 else 220, easing = easing)) }
                }
            }
            coroutineScope.launch {
                androidx.compose.runtime.snapshotFlow { alpha.value }.collect { invalidateDraw() }
            }
        }

        override fun ContentDrawScope.draw() {
            drawContent()
            if (alpha.value > 0f) drawRect(tint.copy(alpha = alpha.value))
        }
    }
}
