package app.convertible.ui

import android.app.Application
import android.content.Intent
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import app.convertible.R
import app.convertible.core.CoreApi
import app.convertible.core.NativeCore
import app.convertible.files.CacheWorkspace
import app.convertible.files.FileTransfers
import app.convertible.state.AppSettings
import app.convertible.state.ConverterStore
import app.convertible.state.ItemStatus
import app.convertible.state.QueueState
import app.convertible.state.SettingsRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** A short message for the transient banner. */
data class UiMessage(val text: String, val isError: Boolean = false)

/**
 * Owns the queue for the lifetime of the screen (survives rotation and other configuration changes); conversions
 * run in [viewModelScope] on background threads. When the user leaves the app for good the queue is cancelled and
 * every copied or converted file is deleted.
 */
class ConverterViewModel(app: Application) : AndroidViewModel(app) {
    private val settingsRepo = SettingsRepository(app)
    val settings: StateFlow<AppSettings> = settingsRepo.settings

    private val workspace = CacheWorkspace(File(app.cacheDir, WORK_DIR)).also { it.clear() } // leftovers of a killed session
    private val transfers = FileTransfers(app, workspace)
    private val core: CoreApi = NativeCore()

    val store = ConverterStore(core, workspace, viewModelScope, Dispatchers.IO, { settingsRepo.settings.value })
    val state: StateFlow<QueueState> = store.state

    private val _messages = MutableSharedFlow<UiMessage>(extraBufferCapacity = 8)
    val messages: SharedFlow<UiMessage> = _messages

    init {
        store.start()
    }

    fun updateSettings(transform: (AppSettings) -> AppSettings) = settingsRepo.update(transform)

    fun addUris(uris: List<Uri>) {
        // One coroutine for the batch keeps the queue in the order the files were picked.
        viewModelScope.launch {
            for (uri in uris) {
                val description = withContext(Dispatchers.IO) { transfers.describe(uri) }
                store.add(description.name, description.size) { id -> transfers.import(id, uri, description) }
            }
        }
    }

    /** Imports the files of an ACTION_SEND / SEND_MULTIPLE / VIEW intent. */
    fun handleIntent(intent: Intent?) {
        val uris = FileTransfers.incomingUris(intent)
        if (uris.isNotEmpty()) addUris(uris)
    }

    fun save(id: Long, dest: Uri) {
        val output = (store.item(id)?.status as? ItemStatus.Done)?.output ?: return
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { runCatching { transfers.saveTo(output, dest) }.isSuccess }
            val app = getApplication<Application>()
            _messages.emit(
                if (ok) UiMessage(app.getString(R.string.message_saved, output.name))
                else UiMessage(app.getString(R.string.message_save_failed), isError = true),
            )
        }
    }

    fun saveAll(tree: Uri) {
        val outputs = state.value.doneItems.mapNotNull { (it.status as? ItemStatus.Done)?.output }
        if (outputs.isEmpty()) return
        viewModelScope.launch {
            val saved = withContext(Dispatchers.IO) { runCatching { transfers.saveAllTo(outputs, tree) }.getOrDefault(-1) }
            val app = getApplication<Application>()
            _messages.emit(
                if (saved >= 0) UiMessage(app.resources.getQuantityString(R.plurals.message_saved_all, saved, saved))
                else UiMessage(app.getString(R.string.message_save_failed), isError = true),
            )
        }
    }

    fun shareIntent(ids: List<Long>): Intent? {
        val outputs = ids.mapNotNull { (store.item(it)?.status as? ItemStatus.Done)?.output }
        return transfers.shareIntent(outputs, getApplication<Application>().getString(R.string.action_share))
    }

    fun openIntent(id: Long): Intent? {
        val output = (store.item(id)?.status as? ItemStatus.Done)?.output ?: return null
        return transfers.openIntent(output, getApplication<Application>().getString(R.string.action_open_in))
    }

    fun notify(message: UiMessage) {
        _messages.tryEmit(message)
    }

    override fun onCleared() {
        store.cancelAll()
        workspace.clear()
    }

    companion object {
        const val WORK_DIR = "work"
    }
}
