package app.convertible.e2e.support

import android.graphics.Color
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.isRoot
import androidx.compose.ui.test.junit4.ComposeTestRule

/** Pixel checks on what Compose actually rendered (light vs dark theme). */
object Screenshots {
    /** Mean relative luminance (0 = black, 1 = white) of the app's window, sampled on a grid. */
    fun meanLuminance(compose: ComposeTestRule): Double {
        compose.waitForIdle()
        val bitmap = compose.onAllNodes(isRoot())[0].captureToImage().asAndroidBitmap()
        var sum = 0.0
        var n = 0
        val step = maxOf(4, minOf(bitmap.width, bitmap.height) / 40)
        for (y in 0 until bitmap.height step step) for (x in 0 until bitmap.width step step) {
            val c = bitmap.getPixel(x, y)
            sum += (0.2126 * Color.red(c) + 0.7152 * Color.green(c) + 0.0722 * Color.blue(c)) / 255.0
            n++
        }
        return sum / n
    }
}
