package app.convertible.files

import android.content.ClipData
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.core.content.FileProvider
import app.convertible.state.ConversionOutput
import app.convertible.state.ImportException
import app.convertible.state.ImportProblem
import app.convertible.state.ImportedFile
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/** Copying between content URIs (Storage Access Framework, share/open-with) and the private workspace. */
class FileTransfers(private val context: Context, private val workspace: CacheWorkspace) {
    private val resolver: ContentResolver get() = context.contentResolver

    data class Description(val name: String, val size: Long)

    /** Display name and size from the provider (falls back to the last path segment / unknown size). */
    fun describe(uri: Uri): Description {
        var name: String? = null
        var size = -1L
        runCatching {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    val n = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val s = c.getColumnIndex(OpenableColumns.SIZE)
                    if (n >= 0 && !c.isNull(n)) name = c.getString(n)
                    if (s >= 0 && !c.isNull(s)) size = c.getLong(s)
                }
            }
        }
        val fallback = uri.lastPathSegment?.substringAfterLast('/')
        return Description(app.convertible.model.FileNames.sanitize(name ?: fallback), size)
    }

    /** Copies [uri] into the workspace as the input of item [itemId]. Runs on a background thread. */
    fun import(itemId: Long, uri: Uri, description: Description): ImportedFile {
        if (description.size > 0 && description.size > workspace.freeSpace()) {
            throw ImportException(ImportProblem.NO_SPACE, "not enough free space")
        }
        val dest = workspace.inputFile(itemId, description.name)
        try {
            val input = resolver.openInputStream(uri) ?: throw ImportException(ImportProblem.UNREADABLE, "no stream")
            input.use { src -> FileOutputStream(dest).use { out -> src.copyTo(out, BUFFER) } }
        } catch (e: ImportException) {
            dest.delete()
            throw e
        } catch (e: IOException) {
            dest.delete()
            val noSpace = e.message?.contains("ENOSPC") == true || e.message?.contains("No space", ignoreCase = true) == true
            throw ImportException(if (noSpace) ImportProblem.NO_SPACE else ImportProblem.UNREADABLE, e.message.orEmpty(), e)
        } catch (e: SecurityException) {
            dest.delete()
            throw ImportException(ImportProblem.UNREADABLE, e.message.orEmpty(), e)
        }
        return ImportedFile(dest.absolutePath, description.name, dest.length())
    }

    /** Writes an output to a document the user created (ACTION_CREATE_DOCUMENT). */
    fun saveTo(output: ConversionOutput, dest: Uri) {
        val out = resolver.openOutputStream(dest, "wt") ?: throw IOException("cannot open destination")
        out.use { o -> File(output.path).inputStream().use { it.copyTo(o, BUFFER) } }
    }

    /** Creates one document per output inside a folder the user picked (ACTION_OPEN_DOCUMENT_TREE). Returns the count saved. */
    fun saveAllTo(outputs: List<ConversionOutput>, tree: Uri): Int {
        val parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        var saved = 0
        for (output in outputs) {
            val doc = DocumentsContract.createDocument(resolver, parent, documentMime(output), output.name) ?: continue
            saveTo(output, doc)
            saved++
        }
        return saved
    }

    private fun uriFor(output: ConversionOutput): Uri =
        FileProvider.getUriForFile(context, "${context.packageName}.files", File(output.path))

    /** ACTION_SEND / ACTION_SEND_MULTIPLE chooser for outputs, with temporary read grants only. */
    fun shareIntent(outputs: List<ConversionOutput>, title: CharSequence): Intent? {
        if (outputs.isEmpty()) return null
        val uris = outputs.map { uriFor(it) }
        val mime = outputs.map { it.mime.ifEmpty { OCTET } }.distinct().singleOrNull() ?: "*/*"
        val send = if (uris.size == 1) {
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, uris[0])
        } else {
            Intent(Intent.ACTION_SEND_MULTIPLE).putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList(uris))
        }
        send.type = mime
        send.clipData = ClipData.newRawUri(outputs.first().name, uris[0]).also { clip ->
            uris.drop(1).forEach { clip.addItem(ClipData.Item(it)) }
        }
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        return withoutSelf(Intent.createChooser(send, title))
    }

    /** ACTION_VIEW chooser ("Open in…") for one output. */
    fun openIntent(output: ConversionOutput, title: CharSequence): Intent {
        val view = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uriFor(output), output.mime.ifEmpty { OCTET })
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        return withoutSelf(Intent.createChooser(view, title))
    }

    // Convertible itself is a share / open-with target; offering it for its own outputs would be a dead end.
    private fun withoutSelf(chooser: Intent): Intent = chooser.putExtra(
        Intent.EXTRA_EXCLUDE_COMPONENTS,
        arrayOf(android.content.ComponentName(context, app.convertible.MainActivity::class.java)),
    )

    companion object {
        private const val BUFFER = 256 * 1024
        const val OCTET = "application/octet-stream"

        /**
         * MIME type for creating a document named [ConversionOutput.name]. Document providers append the extension
         * they associate with the MIME type when it differs ("notes.b64" as text/plain becomes "notes.b64.txt"),
         * so the type is derived from the file's own extension and falls back to octet-stream.
         */
        fun documentMime(output: ConversionOutput): String {
            val ext = app.convertible.model.FileNames.extension(output.name).lowercase()
            return android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: OCTET
        }

        /** URIs carried by an incoming ACTION_SEND / ACTION_SEND_MULTIPLE / ACTION_VIEW intent. */
        fun incomingUris(intent: Intent?): List<Uri> {
            if (intent == null) return emptyList()
            val uris = LinkedHashSet<Uri>()
            when (intent.action) {
                Intent.ACTION_SEND -> streamExtra(intent)?.let { uris += it }
                Intent.ACTION_SEND_MULTIPLE -> uris += streamListExtra(intent)
                Intent.ACTION_VIEW -> intent.data?.let { uris += it }
                else -> return emptyList()
            }
            intent.clipData?.let { clip -> for (i in 0 until clip.itemCount) clip.getItemAt(i).uri?.let { uris += it } }
            // content:// only: file:// URIs from other apps could point the app at its own private files.
            return uris.filter { it.scheme == ContentResolver.SCHEME_CONTENT && it.authority != "${app.convertible.BuildConfig.APPLICATION_ID}.files" }
        }

        @Suppress("DEPRECATION")
        private fun streamExtra(intent: Intent): Uri? =
            if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            else intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri

        @Suppress("DEPRECATION")
        private fun streamListExtra(intent: Intent): List<Uri> =
            if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java).orEmpty()
            else intent.getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM).orEmpty()
    }
}
