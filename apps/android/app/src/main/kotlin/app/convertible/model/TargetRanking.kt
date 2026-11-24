package app.convertible.model

/** A titled section of the target picker. [category] is null for the "Popular" section. */
data class TargetSection(val category: String?, val targets: List<TargetInfo>)

/**
 * Ordering of conversion targets for the picker. The core already returns targets most relevant first (same
 * category, then lossless, then popular); this keeps that order and arranges it into sections:
 *
 *  1. "Popular": the most popular targets (by catalogue popularity, core order breaking ties), at most [POPULAR];
 *     only when there are enough targets for sections to be worth it;
 *  2. the source's own category;
 *  3. the remaining categories, ordered by the rank of their most relevant target.
 *
 * Compress entries (`compress: true`) are excluded from the lists; the UI offers them through the Compress action.
 */
object TargetRanking {
    const val POPULAR = 5
    private const val SECTIONS_THRESHOLD = 8

    fun sections(targets: List<TargetInfo>, sourceCategory: String?, catalogue: Catalogue): List<TargetSection> {
        val list = targets.filter { !it.compress }
        if (list.isEmpty()) return emptyList()
        val out = ArrayList<TargetSection>()
        if (list.size >= SECTIONS_THRESHOLD && list.map { it.category }.distinct().size > 1) {
            val popular = popular(list, catalogue)
            if (popular.isNotEmpty()) out += TargetSection(null, popular)
        }
        val firstIndex = HashMap<String, Int>()
        list.forEachIndexed { i, t -> firstIndex.putIfAbsent(t.category, i) }
        val order = firstIndex.keys.sortedWith(
            compareBy<String> { if (it == sourceCategory) 0 else 1 }.thenBy { firstIndex.getValue(it) },
        )
        for (category in order) out += TargetSection(category, list.filter { it.category == category })
        return out
    }

    /** Most popular targets, core order breaking ties. */
    fun popular(targets: List<TargetInfo>, catalogue: Catalogue, limit: Int = POPULAR): List<TargetInfo> =
        targets.withIndex()
            .filter { !it.value.compress }
            .sortedWith(compareByDescending<IndexedValue<TargetInfo>> { catalogue.popularity(it.value.id) }.thenBy { it.index })
            .take(limit)
            .map { it.value }

    /** A few one-tap suggestions shown on the file card: the core's most relevant non-compress targets. */
    fun suggestions(targets: List<TargetInfo>, limit: Int = 3): List<TargetInfo> =
        targets.filter { !it.compress }.take(limit)

    /** The "Compress" choices for a file: the same-format re-encode first, then archive formats. */
    fun compressTargets(targets: List<TargetInfo>): List<TargetInfo> =
        targets.filter { it.compress } + targets.filter { !it.compress && it.category == "archive" }

    /**
     * Search by id, extension or name, case-insensitively. Exact id/extension matches first, then prefixes, then
     * word prefixes of the name, then substrings; core order breaks ties. An empty query returns everything.
     */
    fun search(targets: List<TargetInfo>, query: String): List<TargetInfo> {
        val q = query.trim().lowercase().removePrefix(".")
        if (q.isEmpty()) return targets.filter { !it.compress }
        return targets.withIndex()
            .filter { !it.value.compress }
            .mapNotNull { (index, t) -> score(t.id, t.extensions, t.name, q)?.let { Triple(it, index, t) } }
            .sortedWith(compareBy<Triple<Int, Int, TargetInfo>> { it.first }.thenBy { it.second })
            .map { it.third }
    }

    /** Same matching for catalogue formats (Formats browser). */
    fun searchFormats(formats: List<FormatInfo>, query: String): List<FormatInfo> {
        val q = query.trim().lowercase().removePrefix(".")
        if (q.isEmpty()) return formats
        return formats.withIndex()
            .mapNotNull { (index, f) ->
                (score(f.id, f.extensions, f.name, q) ?: if (f.mime.lowercase().contains(q)) 4 else null)
                    ?.let { Triple(it, index, f) }
            }
            .sortedWith(compareBy<Triple<Int, Int, FormatInfo>> { it.first }.thenBy { it.second })
            .map { it.third }
    }

    private fun score(id: String, extensions: List<String>, name: String, q: String): Int? {
        val idL = id.lowercase()
        val exts = extensions.map { it.lowercase() }
        val nameL = name.lowercase()
        return when {
            idL == q || q in exts -> 0
            idL.startsWith(q) || exts.any { it.startsWith(q) } -> 1
            nameL.startsWith(q) || nameL.split(' ', '-', '/', '(').any { it.startsWith(q) } -> 2
            idL.contains(q) || nameL.contains(q) -> 3
            else -> null
        }
    }
}
