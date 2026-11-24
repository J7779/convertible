package app.convertible.state

import app.convertible.core.ConversionOutcome
import app.convertible.core.CoreApi
import app.convertible.core.CoreError
import app.convertible.model.Catalogue
import app.convertible.model.FileNames
import app.convertible.model.OptionValue
import app.convertible.model.OptionsForm
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/** Where the store keeps files. Implemented over the app cache on Android and in memory/temp dirs in tests. */
interface Workspace {
    /** A fresh path for an output file of item [itemId] (its directory exists). */
    fun outputPath(itemId: Long, name: String): String

    /** Renames a file within its directory; returns the new path (or the old one if renaming failed). */
    fun rename(path: String, newName: String): String

    fun size(path: String): Long

    /** Deletes all files of item [itemId] (input and outputs). */
    fun deleteItem(itemId: Long)

    fun delete(path: String)
}

/** Why an import failed, thrown by importers. */
class ImportException(val problem: ImportProblem, message: String, cause: Throwable? = null) : Exception(message, cause)

/**
 * The conversion queue: platform-free state logic shared by the view-model and the unit tests.
 *
 * Everything that touches the core runs on [io]; conversions use at most [maxParallel] threads of it. All public
 * functions may be called from the main thread and return immediately. State is a single immutable [QueueState]
 * published through [state].
 */
class ConverterStore(
    private val core: CoreApi,
    private val workspace: Workspace,
    private val scope: CoroutineScope,
    private val io: CoroutineDispatcher,
    private val settings: () -> AppSettings,
    maxParallel: Int = 2,
) {
    private val _state = MutableStateFlow(QueueState())
    val state: StateFlow<QueueState> = _state.asStateFlow()

    private val convertDispatcher = io.limitedParallelism(maxParallel)
    private val jobs = ConcurrentHashMap<Long, Job>()
    private val cancelFlags = ConcurrentHashMap<Long, AtomicBoolean>()
    private val nextId = AtomicLong(1)

    /** Loads the format catalogue and core version (once, in the background). */
    fun start() {
        scope.launch(io) {
            val catalogue = Catalogue.parse(core.formatsJson())
            val version = core.version
            _state.update { s ->
                // Items imported before the catalogue arrived get their format info now.
                s.copy(
                    catalogue = catalogue,
                    coreVersion = version,
                    items = s.items.map { it.copy(source = it.source ?: catalogue[it.sourceId]) },
                )
            }
        }
    }

    /**
     * Adds a file. [importer] copies it into the workspace (it receives the new item's id) and runs on [io];
     * it may throw [ImportException]. Returns the item id.
     */
    fun add(displayName: String, sizeHint: Long, importer: suspend (Long) -> ImportedFile): Long {
        val id = nextId.getAndIncrement()
        _state.update { it.copy(items = it.items + QueueItem(id = id, name = displayName, size = sizeHint)) }
        scope.launch(io) {
            val imported = try {
                importer(id)
            } catch (e: ImportException) {
                updateItem(id) { it.copy(status = ItemStatus.ImportFailed(e.problem, e.message.orEmpty())) }
                return@launch
            } catch (e: Exception) {
                updateItem(id) { it.copy(status = ItemStatus.ImportFailed(ImportProblem.UNREADABLE, e.message.orEmpty())) }
                return@launch
            }
            val detected = runCatching { core.detectFile(imported.path, imported.name) }.getOrNull()
            val sourceId = detected ?: UNKNOWN_SOURCE
            val targets = Catalogue.parseTargets(core.targetsJson(sourceId))
            // Files with the same source format share the target already chosen for their siblings.
            val sibling = _state.value.items.firstOrNull { it.id != id && it.sourceId == sourceId && it.target != null }
            updateItem(id) { item ->
                item.copy(
                    name = imported.name,
                    size = imported.size,
                    inputPath = imported.path,
                    sourceId = sourceId,
                    source = _state.value.catalogue[sourceId],
                    targets = targets,
                    status = ItemStatus.Ready,
                )
            }
            sibling?.target?.let { t -> if (targets.any { it.id == t.id }) applyTarget(id, t.id) }
        }
        return id
    }

    /** Chooses the output format of one file and loads its options schema. */
    fun setTarget(id: Long, targetId: String) {
        scope.launch(io) { applyTarget(id, targetId) }
    }

    /** Chooses the output format for every configurable file whose source format is [sourceId]. */
    fun setTargetForSource(sourceId: String, targetId: String) {
        val ids = _state.value.items.filter { it.sourceId == sourceId && !it.status.isBusy && it.inputPath != null }.map { it.id }
        scope.launch(io) { ids.forEach { applyTarget(it, targetId) } }
    }

    private fun applyTarget(id: Long, targetId: String) {
        val item = item(id) ?: return
        if (item.status.isBusy) return
        val target = item.targets.firstOrNull { it.id == targetId } ?: return
        val schema = Catalogue.parseOptions(core.optionsSchemaJson(item.sourceId ?: UNKNOWN_SOURCE, target.id))
        if (item.status is ItemStatus.Done) discardOutput(item)
        updateItem(id) {
            // Keep values that the new schema still understands.
            val kept = it.values.filterKeys { key -> schema.any { spec -> spec.key == key } }
            it.copy(target = target, schema = schema, values = kept, status = ItemStatus.Ready)
        }
    }

    /** Sets (or with null, resets to default) one option of a file. */
    fun setOption(id: Long, key: String, value: OptionValue?) {
        updateItem(id) { item ->
            if (item.status.isBusy) return@updateItem item
            val spec = item.schema.firstOrNull { it.key == key } ?: return@updateItem item
            val values = if (value == null) item.values - key else spec.coerce(value)?.let { item.values + (key to it) } ?: item.values
            item.copy(values = values)
        }
    }

    /** Sets one option on every configurable file whose schema has it (used by group controls). */
    fun setOptionForAll(ids: Collection<Long>, key: String, value: OptionValue?) = ids.forEach { setOption(it, key, value) }

    fun convertAll() {
        _state.value.items.filter { it.canConvert }.forEach { convert(it.id) }
    }

    /** Starts (or restarts after a failure) the conversion of one file. */
    fun convert(id: Long) {
        val item = item(id) ?: return
        if (!item.canConvert) return
        val target = item.target ?: return
        val input = item.inputPath ?: return
        val flag = AtomicBoolean(false)
        cancelFlags[id] = flag
        updateItem(id) { it.copy(status = ItemStatus.Queued) }
        val prefs = settings()
        val options = OptionsForm.toJson(OptionsForm.resolve(item.schema, item.values, prefs.defaultPreset, prefs.stripMetadata))
        val job = scope.launch(convertDispatcher) {
            if (flag.get()) return@launch
            updateItem(id) { it.copy(status = ItemStatus.Converting(0f, "")) }
            val plannedName = FileNames.outputName(item.name, target.extensions.firstOrNull() ?: target.id)
            val outPath = workspace.outputPath(id, plannedName)
            val outcome = try {
                core.convertFile(input, outPath, item.sourceId, target.id, options) { fraction, stage ->
                    if (flag.get() || !isActive) {
                        true
                    } else {
                        updateItem(id) { current ->
                            if (current.status is ItemStatus.Converting) {
                                current.copy(status = ItemStatus.Converting(fraction.toFloat().coerceIn(0f, 1f), stage))
                            } else {
                                current
                            }
                        }
                        false
                    }
                }
            } catch (e: Exception) {
                ConversionOutcome.Failure(CoreError.Bridge(e.message ?: e.javaClass.simpleName))
            }
            when (outcome) {
                // Cancelled while the core was in a stage without progress callbacks: it still finished, but the
                // user asked to stop, so the result is discarded rather than shown as done.
                is ConversionOutcome.Success if flag.get() -> {
                    workspace.delete(outPath)
                    finish(id, flag, ItemStatus.Failed(CoreError.Cancelled("")))
                }
                is ConversionOutcome.Success -> {
                    // Prefer the core's recommended extension for the produced format.
                    val finalName = if (outcome.extension.isNotEmpty() &&
                        !FileNames.extension(plannedName).equals(outcome.extension, ignoreCase = true)
                    ) {
                        FileNames.outputName(item.name, outcome.extension)
                    } else {
                        plannedName
                    }
                    val finalPath = if (finalName != plannedName) workspace.rename(outPath, finalName) else outPath
                    val output = ConversionOutput(
                        path = finalPath,
                        name = finalPath.substringAfterLast('/'),
                        size = workspace.size(finalPath),
                        format = outcome.format.ifEmpty { target.id },
                        extension = outcome.extension,
                        mime = outcome.mime.ifEmpty { target.mime },
                        warnings = outcome.warnings,
                    )
                    finish(id, flag, ItemStatus.Done(output))
                }
                is ConversionOutcome.Failure -> {
                    workspace.delete(outPath)
                    val error = if (flag.get() && outcome.error !is CoreError.Cancelled) CoreError.Cancelled("") else outcome.error
                    finish(id, flag, ItemStatus.Failed(error))
                }
            }
        }
        jobs[id] = job
        job.invokeOnCompletion { cause ->
            jobs.remove(id, job)
            if (cause != null) {
                // Cancelled before the body ran (cancel while queued, or the scope ended).
                updateItem(id) { if (it.status.isBusy) it.copy(status = ItemStatus.Failed(CoreError.Cancelled(""))) else it }
            }
        }
    }

    private fun finish(id: Long, flag: AtomicBoolean, status: ItemStatus) {
        cancelFlags.remove(id, flag)
        updateItem(id) { it.copy(status = status) }
    }

    /** Cancels a queued or running conversion. The core stops at its next progress callback. */
    fun cancel(id: Long) {
        cancelFlags[id]?.set(true)
        val item = item(id) ?: return
        if (item.status is ItemStatus.Queued) {
            jobs[id]?.cancel()
            updateItem(id) { it.copy(status = ItemStatus.Failed(CoreError.Cancelled(""))) }
        }
    }

    fun cancelAll() {
        _state.value.items.filter { it.status is ItemStatus.Queued || it.status is ItemStatus.Converting }.forEach { cancel(it.id) }
    }

    /** Back to Ready with the same target and options ("Convert again"); drops the previous output. */
    fun reset(id: Long) {
        val item = item(id) ?: return
        if (item.status.isBusy) return
        if (item.status is ItemStatus.Done) discardOutput(item)
        updateItem(id) { it.copy(status = if (it.inputPath != null) ItemStatus.Ready else it.status) }
    }

    fun remove(id: Long) {
        cancel(id)
        jobs[id]?.cancel()
        _state.update { s -> s.copy(items = s.items.filterNot { it.id == id }) }
        scope.launch(io) { workspace.deleteItem(id) }
    }

    /** Removes every file that is not being converted. */
    fun clearFinished() {
        _state.value.items.filter { !it.status.isBusy }.forEach { remove(it.id) }
    }

    fun item(id: Long): QueueItem? = _state.value.items.firstOrNull { it.id == id }

    private fun discardOutput(item: QueueItem) {
        (item.status as? ItemStatus.Done)?.output?.path?.let { workspace.delete(it) }
    }

    private inline fun updateItem(id: Long, crossinline transform: (QueueItem) -> QueueItem) {
        _state.update { s ->
            val index = s.items.indexOfFirst { it.id == id }
            if (index < 0) {
                s
            } else {
                val updated = transform(s.items[index])
                if (updated === s.items[index]) s else s.copy(items = s.items.toMutableList().also { it[index] = updated })
            }
        }
    }

    companion object {
        /** Source id for files the core does not recognise; they still convert to the universal encodings. */
        const val UNKNOWN_SOURCE = "bin"
    }
}
