package app.convertible

import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import app.convertible.ui.ConvertibleApp
import app.convertible.ui.ConverterViewModel
import app.convertible.ui.theme.Brand

class MainActivity : ComponentActivity() {
    private val viewModel: ConverterViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        // The manifest theme is the launch theme (splash); switch to the app theme before any view exists.
        setTheme(R.style.Theme_Convertible)
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // The splash shows only the background; remove it at once so the intro animation takes over seamlessly.
            splashScreen.setOnExitAnimationListener { view -> view.remove() }
        }
        enableEdgeToEdge()
        val (tokens, motion) = Brand.load(this)
        val coldStart = savedInstanceState == null
        if (coldStart) viewModel.handleIntent(intent)
        setContent {
            ConvertibleApp(
                viewModel = viewModel,
                tokens = tokens,
                storyboard = motion,
                playIntro = coldStart,
            )
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        viewModel.handleIntent(intent)
    }
}
