// Ordering, grouping and search for the target-format picker and the formats browser.
import Foundation

struct TargetSection: Identifiable, Equatable {
    let id: String        // category id, or "results" for a search
    let title: String
    let targets: [TargetFormat]
}

enum TargetCatalog {
    /// Groups targets by category: the source's own category first, then the other
    /// categories by their most popular member. Inside a category the most popular
    /// formats come first; ties keep the core's relevance order.
    /// With a query, returns one ranked "Results" section instead.
    static func sections(targets: [TargetFormat], sourceCategory: String?, popularity: [String: Int],
                         query: String = "") -> [TargetSection] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty {
            let found = search(targets, query: trimmed, popularity: popularity)
            return found.isEmpty ? [] : [TargetSection(id: "results", title: "Results", targets: found)]
        }
        let indexed = Array(targets.enumerated())
        var groups: [String: [(offset: Int, element: TargetFormat)]] = [:]
        for entry in indexed { groups[entry.element.category, default: []].append(entry) }

        func sortedMembers(_ members: [(offset: Int, element: TargetFormat)]) -> [TargetFormat] {
            members.sorted { a, b in
                // The same-format ("compress") entry leads its own category.
                if a.element.compress != b.element.compress { return a.element.compress }
                let pa = popularity[a.element.id] ?? 0, pb = popularity[b.element.id] ?? 0
                return pa != pb ? pa > pb : a.offset < b.offset
            }.map(\.element)
        }

        let ordered = groups.keys.sorted { a, b in
            if a == sourceCategory { return true }
            if b == sourceCategory { return false }
            let pa = groups[a]!.map { popularity[$0.element.id] ?? 0 }.max() ?? 0
            let pb = groups[b]!.map { popularity[$0.element.id] ?? 0 }.max() ?? 0
            if pa != pb { return pa > pb }
            let fa = groups[a]!.first!.offset, fb = groups[b]!.first!.offset
            return fa < fb
        }
        return ordered.map { TargetSection(id: $0, title: FileCategory.title($0), targets: sortedMembers(groups[$0]!)) }
    }

    /// Ranked search over id, extensions and name (case- and diacritic-insensitive).
    static func search(_ targets: [TargetFormat], query: String, popularity: [String: Int]) -> [TargetFormat] {
        let q = normalize(query.hasPrefix(".") ? String(query.dropFirst()) : query)
        guard !q.isEmpty else { return targets }
        let scored: [(score: Int, offset: Int, target: TargetFormat)] = targets.enumerated().compactMap { offset, t in
            guard let score = matchScore(id: t.id, name: t.name, extensions: t.extensions, query: q) else { return nil }
            return (score, offset, t)
        }
        return scored.sorted { a, b in
            if a.score != b.score { return a.score < b.score }
            let pa = popularity[a.target.id] ?? 0, pb = popularity[b.target.id] ?? 0
            return pa != pb ? pa > pb : a.offset < b.offset
        }.map(\.target)
    }

    /// 0 exact id/extension, 1 id or name prefix, 2 extension prefix, 3 word prefix in the name,
    /// 4 substring anywhere; nil when not matching.
    static func matchScore(id: String, name: String, extensions: [String], query q: String) -> Int? {
        let id = normalize(id), name = normalize(name)
        let exts = extensions.map(normalize)
        if id == q || exts.contains(q) { return 0 }
        if id.hasPrefix(q) || name.hasPrefix(q) { return 1 }
        if exts.contains(where: { $0.hasPrefix(q) }) { return 2 }
        let words = name.split(whereSeparator: { !$0.isLetter && !$0.isNumber })
        if words.contains(where: { $0.hasPrefix(q) }) { return 3 }
        if id.contains(q) || name.contains(q) || exts.contains(where: { $0.contains(q) }) { return 4 }
        return nil
    }

    static func normalize(_ s: String) -> String {
        s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
    }

    /// The best default target for a new file: the first entry that is not the
    /// file's own format (the core lists the most relevant first).
    static func defaultTarget(_ targets: [TargetFormat]) -> TargetFormat? {
        targets.first(where: { !$0.compress }) ?? targets.first
    }

    /// The target a "Compress" action uses: the same-format re-encode when the
    /// core offers it, otherwise an archive target (zip first).
    static func compressTarget(_ targets: [TargetFormat]) -> TargetFormat? {
        if let same = targets.first(where: \.compress) { return same }
        let archives = targets.filter { $0.category == "archive" }
        return archives.first(where: { $0.id == "zip" }) ?? archives.first
    }
}

struct FormatSection: Identifiable, Equatable {
    let id: String
    let title: String
    let formats: [FormatInfo]
}

enum FormatsBrowser {
    /// The whole catalogue grouped by category in the documented order, most popular first.
    static func sections(_ formats: [FormatInfo], query: String = "") -> [FormatSection] {
        let q = TargetCatalog.normalize(query.trimmingCharacters(in: .whitespacesAndNewlines))
        let matching = q.isEmpty ? formats : formats.filter {
            TargetCatalog.matchScore(id: $0.id, name: $0.name, extensions: $0.extensions, query: q) != nil
                || TargetCatalog.normalize(FileCategory.title($0.category)).hasPrefix(q)
        }
        let grouped = Dictionary(grouping: matching) { FileCategory.known($0.category) }
        return grouped.keys.sorted { FileCategory.rank($0) < FileCategory.rank($1) }.map { key in
            FormatSection(id: key, title: FileCategory.title(key),
                          formats: grouped[key]!.sorted { $0.popularity != $1.popularity ? $0.popularity > $1.popularity : $0.id < $1.id })
        }
    }
}
