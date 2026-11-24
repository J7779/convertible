package app.convertible.e2e.support

import android.app.Instrumentation
import android.content.Context
import android.graphics.Bitmap
import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.services.storage.TestStorage
import androidx.test.uiautomator.UiDevice
import org.junit.rules.ExternalResource
import org.junit.rules.TestWatcher
import org.junit.runner.Description
import java.io.FileInputStream

object Device {
    val instrumentation: Instrumentation get() = InstrumentationRegistry.getInstrumentation()

    /** The app under test (the instrumentation runs in its process). */
    val app: Context get() = instrumentation.targetContext

    /** The test APK, which owns the fixture providers. */
    val testApk: Context get() = instrumentation.context

    val ui: UiDevice get() = UiDevice.getInstance(instrumentation)

    /** Runs a shell command as the shell user and returns its output. */
    fun shell(command: String): String {
        val pfd: ParcelFileDescriptor = instrumentation.uiAutomation.executeShellCommand(command)
        return FileInputStream(pfd.fileDescriptor).use { it.readBytes().toString(Charsets.UTF_8) }.also { pfd.close() }
    }

    fun setAnimatorScale(scale: Float) = shell("settings put global animator_duration_scale $scale")

    fun setFontScale(scale: Float) = shell("settings put system font_scale $scale")

    fun setNightMode(night: Boolean) = shell("cmd uimode night ${if (night) "yes" else "no"}")

    /** Emulates another screen (size in pixels, density in dpi) on the emulator's display. */
    fun setDisplay(widthPx: Int, heightPx: Int, densityDpi: Int) {
        shell("wm size ${widthPx}x$heightPx")
        shell("wm density $densityDpi")
    }

    fun resetDisplay() {
        shell("wm size reset")
        shell("wm density reset")
    }
}

/**
 * Puts global device state the tests change (animation and font scale, night mode, display size, rotation) back
 * to the defaults after every test, pass or fail, so tests never leak state into each other.
 */
class DeviceStateRule : ExternalResource() {
    override fun before() = reset()

    override fun after() = reset()

    private fun reset() {
        Device.setAnimatorScale(1f)
        Device.setFontScale(1f)
        Device.setNightMode(false)
        Device.resetDisplay()
        runCatching {
            Device.ui.setOrientationNatural()
            Device.ui.unfreezeRotation()
        }
        Device.instrumentation.waitForIdleSync()
    }
}

/**
 * Saves a screenshot of the whole screen when a test fails. Files go through the AndroidX test storage service and
 * end up in `app/build/outputs/connected_android_test_additional_output/` (git-ignored); `e2e.sh` copies them
 * next to the run's report.
 */
class FailureScreenshotRule : TestWatcher() {
    override fun failed(e: Throwable?, description: Description) {
        runCatching {
            val bitmap: Bitmap = Device.instrumentation.uiAutomation.takeScreenshot() ?: return
            val name = "${description.className.substringAfterLast('.')}-${description.methodName}.png"
            TestStorage().openOutputFile("screenshots/$name").use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
        runCatching {
            val name = "${description.className.substringAfterLast('.')}-${description.methodName}.xml"
            TestStorage().openOutputFile("screenshots/$name").use { out ->
                Device.ui.dumpWindowHierarchy(out)
            }
        }
    }
}
