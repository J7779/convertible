package app.convertible.files

import app.convertible.model.FileNames
import app.convertible.state.Workspace
import java.io.File

/**
 * Private scratch space under the app cache: `in/<item>/` holds the copied source, `out/<item>/` the result.
 * Nothing here outlives the session: [clear] runs when the queue is discarded and on the next start.
 */
class CacheWorkspace(val root: File) : Workspace {
    fun inputFile(itemId: Long, name: String): File =
        File(File(root, "in/$itemId").apply { mkdirs() }, FileNames.sanitize(name))

    override fun outputPath(itemId: Long, name: String): String {
        val dir = File(root, "out/$itemId").apply { mkdirs() }
        return File(dir, FileNames.sanitize(name)).absolutePath
    }

    override fun rename(path: String, newName: String): String {
        val from = File(path)
        val to = File(from.parentFile, FileNames.sanitize(newName))
        if (to.exists()) to.delete()
        return if (from.renameTo(to)) to.absolutePath else path
    }

    override fun size(path: String): Long = File(path).length()

    override fun deleteItem(itemId: Long) {
        File(root, "in/$itemId").deleteRecursively()
        File(root, "out/$itemId").deleteRecursively()
    }

    override fun delete(path: String) {
        val file = File(path)
        // Only ever delete inside the workspace.
        if (file.canonicalPath.startsWith(root.canonicalPath + File.separator)) file.delete()
    }

    fun contains(file: File): Boolean = file.canonicalPath.startsWith(root.canonicalPath + File.separator)

    fun clear() {
        root.deleteRecursively()
    }

    fun freeSpace(): Long = (root.takeIf { it.exists() } ?: root.parentFile ?: root).usableSpace
}
