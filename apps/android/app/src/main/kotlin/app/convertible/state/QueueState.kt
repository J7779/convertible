package app.convertible.state

import app.convertible.core.CoreError
import app.convertible.model.Catalogue
import app.convertible.model.FormatInfo
import app.convertible.model.OptionSpec
import app.convertible.model.OptionValue
import app.convertible.model.Preset
import app.convertible.model.TargetInfo

/** A source file copied into the app's private workspace. */
data class ImportedFile(val path: String, val name: String, val size: Long)

/** A finished conversion's output file in the workspace. */
data class ConversionOutput(
    val path: String,
    val name: String,
    val size: Long,
    val format: String,
    val extension: String,
    val mime: String,
    val warnings: List<String>,
)

sealed interface ItemStatus {
    /** Being copied into the workspace and identified. */
    data object Importing : ItemStatus

    /** Could not be read from its source (e.g. the providing app revoked access, or no space left). */
    data class ImportFailed(val reason: ImportProblem, val detail: String) : ItemStatus

    data object Ready : ItemStatus
    data object Queued : ItemStatus
    data class Converting(val fraction: Float, val stage: String) : ItemStatus
    data class Done(val output: ConversionOutput) : ItemStatus
    data class Failed(val error: CoreError) : ItemStatus

    val isBusy: Boolean get() = this is Importing || this is Queued || this is Converting
}

enum class ImportProblem { UNREADABLE, NO_SPACE }

data class QueueItem(
    val id: Long,
    val name: String,
    val size: Long,
    val inputPath: String? = null,
    /** Detected source format id ("bin" when unrecognised: any file still converts to the universal encodings). */
    val sourceId: String? = null,
    val source: FormatInfo? = null,
    val targets: List<TargetInfo> = emptyList(),
    val target: TargetInfo? = null,
    val schema: List<OptionSpec> = emptyList(),
    /** Options the user changed on this file; everything else uses defaults. */
    val values: Map<String, OptionValue> = emptyMap(),
    val status: ItemStatus = ItemStatus.Importing,
) {
    val canConvert: Boolean
        get() = target != null && inputPath != null &&
            (status is ItemStatus.Ready || status is ItemStatus.Failed)
}

/** Several files with the same source format: one control sets the target for all of them. */
data class SourceGroup(val sourceId: String, val label: String, val itemIds: List<Long>, val targets: List<TargetInfo>, val commonTarget: TargetInfo?)

data class QueueState(
    val catalogue: Catalogue = Catalogue.EMPTY,
    val coreVersion: String = "",
    val items: List<QueueItem> = emptyList(),
) {
    val isEmpty: Boolean get() = items.isEmpty()
    val convertibleCount: Int get() = items.count { it.canConvert }
    val activeCount: Int get() = items.count { it.status is ItemStatus.Queued || it.status is ItemStatus.Converting }
    val doneItems: List<QueueItem> get() = items.filter { it.status is ItemStatus.Done }

    /** Groups of 2+ files sharing a source format that can still be configured. */
    val groups: List<SourceGroup>
        get() = items
            .filter { it.sourceId != null && it.inputPath != null && !it.status.isBusy && it.status !is ItemStatus.Done }
            .groupBy { it.sourceId!! }
            .filterValues { it.size >= 2 }
            .map { (sourceId, members) ->
                val common = members.map { it.target?.id }.distinct().singleOrNull()
                SourceGroup(
                    sourceId = sourceId,
                    label = members.first().source?.label ?: sourceId.uppercase(),
                    itemIds = members.map { it.id },
                    targets = members.first().targets,
                    commonTarget = common?.let { id -> members.first().targets.firstOrNull { it.id == id } },
                )
            }
}

enum class ThemeMode { SYSTEM, DARK, LIGHT }

data class AppSettings(
    val theme: ThemeMode = ThemeMode.SYSTEM,
    val defaultPreset: Preset = Preset.BALANCED,
    val stripMetadata: Boolean = true,
)
