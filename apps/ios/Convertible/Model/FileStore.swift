// Temporary storage for imported copies and conversion results. Everything
// lives under the app's temporary directory and is removed when no longer
// needed and at every launch; nothing is kept between sessions.
import Foundation
import UniformTypeIdentifiers

struct FileStore {
    let root: URL

    static let shared = FileStore(root: FileManager.default.temporaryDirectory.appendingPathComponent("Convertible", isDirectory: true))

    var inbox: URL { root.appendingPathComponent("Inbox", isDirectory: true) }
    var results: URL { root.appendingPathComponent("Results", isDirectory: true) }

    /// Removes everything left from earlier sessions, including files the
    /// system copied into Documents/Inbox for "Open in".
    func resetAll() {
        let fm = FileManager.default
        try? fm.removeItem(at: root)
        if let documents = fm.urls(for: .documentDirectory, in: .userDomainMask).first {
            try? fm.removeItem(at: documents.appendingPathComponent("Inbox", isDirectory: true))
        }
    }

    /// A fresh, unique directory holding one file named `name`.
    func newFileURL(in parent: URL, named name: String) throws -> URL {
        let dir = parent.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent(Self.sanitize(name), isDirectory: false)
    }

    func newInboxURL(named name: String) throws -> URL { try newFileURL(in: inbox, named: name) }
    func newResultURL(named name: String) throws -> URL { try newFileURL(in: results, named: name) }

    /// Deletes a file this store created, together with its unique directory.
    func discard(_ url: URL) {
        let fm = FileManager.default
        let dir = url.deletingLastPathComponent()
        if dir.path.hasPrefix(root.path) && dir.standardizedFileURL != root.standardizedFileURL {
            try? fm.removeItem(at: dir)
        } else if url.path.contains("/Documents/Inbox/") {
            try? fm.removeItem(at: url)
        }
    }

    func owns(_ url: URL) -> Bool {
        url.standardizedFileURL.path.hasPrefix(root.standardizedFileURL.path) || url.path.contains("/Documents/Inbox/")
    }

    /// Copies a file into the inbox (used for files whose original goes away,
    /// such as drag-and-drop and paste payloads).
    func importCopy(of url: URL, named name: String? = nil) throws -> URL {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let destination = try newInboxURL(named: name ?? url.lastPathComponent)
        try FileManager.default.copyItem(at: url, to: destination)
        return destination
    }

    /// Writes in-memory data (for example pasted text) into the inbox.
    func importData(_ data: Data, named name: String) throws -> URL {
        let destination = try newInboxURL(named: name)
        try data.write(to: destination, options: .atomic)
        return destination
    }

    /// Imports the payloads of drag-and-drop or paste item providers as files.
    func importProviders(_ providers: [NSItemProvider]) async -> [URL] {
        var urls: [URL] = []
        for provider in providers {
            if let url = await importProvider(provider) { urls.append(url) }
        }
        return urls
    }

    private func importProvider(_ provider: NSItemProvider) async -> URL? {
        let types = provider.registeredTypeIdentifiers
        // Prefer a real file; then any data type; plain text strings last.
        let preferred = types.first { id in
            guard let type = UTType(id) else { return false }
            return type.conforms(to: .data) && !type.conforms(to: .plainText)
        } ?? types.first { UTType($0)?.conforms(to: .data) ?? false } ?? types.first
        guard let typeID = preferred else { return nil }
        let suggested = provider.suggestedName
        let store = self
        return await withCheckedContinuation { continuation in
            _ = provider.loadFileRepresentation(forTypeIdentifier: typeID) { url, _ in
                guard let url else { continuation.resume(returning: nil); return }
                var name = suggested ?? url.lastPathComponent
                if (name as NSString).pathExtension.isEmpty, (url.lastPathComponent as NSString).pathExtension.isEmpty == false {
                    name += "." + (url.lastPathComponent as NSString).pathExtension
                } else if (name as NSString).pathExtension.isEmpty, let ext = UTType(typeID)?.preferredFilenameExtension {
                    name += "." + ext
                }
                // The provider deletes its file when this handler returns: copy it now.
                continuation.resume(returning: try? store.importCopy(of: url, named: name))
            }
        }
    }

    /// Removes path separators and control characters from a file name.
    static func sanitize(_ name: String) -> String {
        let cleaned = name.map { ch -> Character in
            (ch == "/" || ch == ":" || ch.unicodeScalars.contains { $0.properties.generalCategory == .control }) ? "_" : ch
        }
        let result = String(cleaned).trimmingCharacters(in: .whitespacesAndNewlines)
        if result.isEmpty || result == "." || result == ".." { return "file" }
        return result
    }
}
