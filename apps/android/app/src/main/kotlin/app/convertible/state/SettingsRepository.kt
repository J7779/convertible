package app.convertible.state

import android.content.Context
import app.convertible.model.Preset
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** App preferences (theme, default preset, strip metadata). No history or file data is ever stored. */
class SettingsRepository(context: Context) {
    private val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    private val _settings = MutableStateFlow(read())
    val settings: StateFlow<AppSettings> = _settings.asStateFlow()

    private fun read() = AppSettings(
        theme = runCatching { ThemeMode.valueOf(prefs.getString(KEY_THEME, null) ?: "") }.getOrDefault(ThemeMode.SYSTEM),
        defaultPreset = Preset.from(prefs.getString(KEY_PRESET, null)) ?: Preset.BALANCED,
        stripMetadata = prefs.getBoolean(KEY_STRIP, true),
    )

    fun update(transform: (AppSettings) -> AppSettings) {
        val next = transform(_settings.value)
        _settings.value = next
        prefs.edit()
            .putString(KEY_THEME, next.theme.name)
            .putString(KEY_PRESET, next.defaultPreset.id)
            .putBoolean(KEY_STRIP, next.stripMetadata)
            .apply()
    }

    private companion object {
        const val KEY_THEME = "theme"
        const val KEY_PRESET = "default_preset"
        const val KEY_STRIP = "strip_metadata"
    }
}
