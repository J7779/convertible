// The conversion queue: files, their detected formats, chosen targets and
// options, and the conversion lifecycle. UI-independent and unit-tested.
import Foundation

/// The catalogue of formats, loaded once from the core.
struct FormatCatalog {
    let formats: [FormatInfo]
    let byID: [String: FormatInfo]
    let popularity: [String: Int]

    init(formats: [FormatInfo]) {
        self.formats = formats
        byID = Dictionary(formats.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        popularity = byID.mapValues(\.popularity)
    }

    static func load() -> FormatCatalog { FormatCatalog(formats: ConvertibleCore.formats()) }

    func format(_ id: String?) -> FormatInfo? { id.flatMap { byID[$0] } }
}

struct ConversionSummary: Equatable {
    let outputURL: URL
    let format: String
    let mime: String
    let inputSize: Int64
    let outputSize: Int64
    let warnings: [String]

    var fileName: String { outputURL.lastPathComponent }
    var change: SizeChange { SizeChange(before: inputSize, after: outputSize) }
}

enum ItemPhase: Equatable {
    case detecting
    case ready
    case queued
    case converting(ConversionProgress)
    case finished(ConversionSummary)
    case failed(FriendlyError)
    case cancelled

    /// Waiting for or running a conversion.
    var isBusy: Bool {
        switch self {
        case .queued, .converting: return true
        default: return false
        }
    }

    /// Ready to be configured and started (again).
    var canStart: Bool {
        switch self {
        case .ready, .failed, .cancelled: return true
        default: return false
        }
    }

    var summary: ConversionSummary? { if case .finished(let s) = self { return s } else { return nil } }

    /// The phase without its payload, for animating state changes (not progress ticks).
    var kind: String {
        switch self {
        case .detecting: return "detecting"
        case .ready: return "ready"
        case .queued: return "queued"
        case .converting: return "converting"
        case .finished: return "finished"
        case .failed: return "failed"
        case .cancelled: return "cancelled"
        }
    }
}

struct QueueItem: Identifiable, Equatable {
    let id = UUID()
    let url: URL
    let name: String
    var size: Int64
    /// The file is a private copy (drag and drop, paste, system inbox) and is deleted with the item.
    let ownsFile: Bool
    var detectedID: String?
    var source: FormatInfo?
    var targets: [TargetFormat] = []
    var targetID: String?
    var schema: [OptionSpec] = []
    var options = OptionValues()
    var phase: ItemPhase = .detecting

    var target: TargetFormat? { targets.first { $0.id == targetID } }
    var form: OptionsForm { OptionsForm(schema: schema) }
    var sourceCategory: String { source?.category ?? "other" }
    /// Label for the source badge, e.g. "TXT".
    var sourceLabel: String { (source?.id ?? detectedID ?? ((name as NSString).pathExtension.isEmpty ? "file" : (name as NSString).pathExtension)).uppercased() }
    var compressTarget: TargetFormat? { TargetCatalog.compressTarget(targets) }
    /// Source id given to the core: the detected one, or "bin" (raw bytes) for unknown files.
    var coreSourceID: String { detectedID ?? QueueModel.unknownSourceID }
}

/// A set of queued files sharing one source format, configurable together.
struct SourceGroup: Identifiable, Equatable {
    let id: String          // source format id
    let label: String
    let itemIDs: [UUID]
    let targets: [TargetFormat]
    /// The target all members share, if they share one.
    let commonTargetID: String?
}

@MainActor
final class QueueModel: ObservableObject {
    /// Formats the core cannot identify are converted from their raw bytes.
    nonisolated static let unknownSourceID = "bin"

    @Published private(set) var items: [QueueItem] = []
    let catalog: FormatCatalog
    let store: FileStore

    /// Called after each conversion ends: true on success, false on failure (not on cancel).
    var onItemCompleted: ((Bool) -> Void)?
    /// Current preferences, read whenever a target is chosen.
    var preferences: () -> (preset: Preset, stripMetadata: Bool) = { (.balanced, true) }

    /// Running (or waiting) conversions, keyed by item; the token tells a stale run from the current one.
    private var runs: [UUID: (token: UUID, task: Task<Void, Never>)] = [:]
    private let limiter: ConcurrencyLimiter

    init(catalog: FormatCatalog = .load(), store: FileStore = .shared, maxConcurrent: Int = 2) {
        self.catalog = catalog
        self.store = store
        limiter = ConcurrencyLimiter(limit: maxConcurrent)
    }

    // MARK: Queries

    func item(_ id: UUID) -> QueueItem? { items.first { $0.id == id } }
    private func index(_ id: UUID) -> Int? { items.firstIndex { $0.id == id } }

    var readyItems: [QueueItem] { items.filter { $0.phase.canStart && $0.target != nil && !isFinished($0) } }
    var busyCount: Int { items.filter { $0.phase.isBusy }.count }
    var finishedItems: [QueueItem] { items.filter { $0.phase.summary != nil } }
    var hasActiveWork: Bool { busyCount > 0 }
    private func isFinished(_ item: QueueItem) -> Bool { item.phase.summary != nil }

    /// Groups of two or more idle files with the same detected source format.
    var groups: [SourceGroup] {
        var order: [String] = []
        var members: [String: [QueueItem]] = [:]
        for item in items where item.phase.canStart && item.detectedID != nil {
            let key = item.coreSourceID
            if members[key] == nil { order.append(key) }
            members[key, default: []].append(item)
        }
        return order.compactMap { key in
            guard let group = members[key], group.count >= 2, let first = group.first else { return nil }
            let targetIDs = Set(group.map(\.targetID))
            return SourceGroup(id: key, label: first.sourceLabel, itemIDs: group.map(\.id), targets: first.targets,
                               commonTargetID: targetIDs.count == 1 ? group.first?.targetID : nil)
        }
    }

    // MARK: Adding files

    /// Adds files and detects their formats. Returns the new item ids once detection finished.
    @discardableResult
    func add(urls: [URL], owned: Bool) async -> [UUID] {
        var ids: [UUID] = []
        for url in urls {
            let item = QueueItem(url: url, name: url.lastPathComponent, size: Self.fileSize(url), ownsFile: owned)
            items.append(item)
            ids.append(item.id)
        }
        for id in ids {
            guard let item = item(id) else { continue }
            let url = item.url
            let detected = await Task.detached(priority: .userInitiated) { () -> (String?, [TargetFormat]) in
                let id = ConvertibleCore.detect(fileAt: url)
                let targets = ConvertibleCore.targets(for: id ?? QueueModel.unknownSourceID)
                return (id, targets)
            }.value
            guard let i = index(id) else { continue }
            items[i].detectedID = detected.0
            items[i].source = catalog.format(detected.0)
            items[i].targets = detected.1
            items[i].phase = .ready
            if let target = TargetCatalog.defaultTarget(detected.1) {
                applyTarget(target.id, at: i)
            }
        }
        return ids
    }

    /// Adds in-memory content (for example pasted text) as a file.
    @discardableResult
    func add(data: Data, named name: String) async -> [UUID] {
        guard let url = try? store.importData(data, named: name) else { return [] }
        return await add(urls: [url], owned: true)
    }

    // MARK: Configuration

    func setTarget(_ targetID: String, for id: UUID) {
        guard let i = index(id), items[i].phase.canStart || items[i].phase.summary != nil else { return }
        if items[i].phase.summary != nil { resetResult(at: i) }
        applyTarget(targetID, at: i)
    }

    /// One control for many files: sets the target of every idle file with this source format.
    func setTarget(_ targetID: String, forGroup sourceID: String) {
        for i in items.indices where items[i].phase.canStart && items[i].coreSourceID == sourceID
            && items[i].targets.contains(where: { $0.id == targetID }) {
            applyTarget(targetID, at: i)
        }
    }

    func setOption(_ key: String, to value: OptionValue?, for id: UUID) {
        guard let i = index(id), items[i].phase.canStart else { return }
        items[i].options[key] = value
    }

    /// Sets the quality preset of every idle file in a group that offers it.
    func setPreset(_ preset: Preset, forGroup sourceID: String) {
        for i in items.indices where items[i].phase.canStart && items[i].coreSourceID == sourceID
            && items[i].form.presetChoices.contains(preset) {
            items[i].options[OptionsForm.presetKey] = .string(preset.rawValue)
        }
    }

    /// "Compress": same format re-encoded at the Small preset, or an archive.
    func applyCompress(for id: UUID) {
        guard let i = index(id), let target = items[i].compressTarget else { return }
        if items[i].phase.summary != nil { resetResult(at: i) }
        applyTarget(target.id, at: i)
        if items[i].form.presetChoices.contains(.small) {
            items[i].options[OptionsForm.presetKey] = .string(Preset.small.rawValue)
        }
    }

    private func applyTarget(_ targetID: String, at i: Int) {
        let source = items[i].coreSourceID
        let schema = ConvertibleCore.optionsSchema(source: source, target: targetID)
        let prefs = preferences()
        items[i].targetID = targetID
        items[i].schema = schema
        items[i].options = OptionsForm.initialValues(schema: schema, defaultPreset: prefs.preset,
                                                     stripMetadata: prefs.stripMetadata, keeping: items[i].options)
    }

    // MARK: Converting

    /// Starts converting one file. Returns the task so callers (and tests) can await it.
    @discardableResult
    func convert(_ id: UUID) -> Task<Void, Never>? {
        guard let i = index(id), items[i].phase.canStart, items[i].target != nil else { return nil }
        items[i].phase = .queued
        let token = UUID()
        let task = Task { [weak self] in
            guard let self else { return }
            await self.run(id, token: token)
        }
        runs[id] = (token, task)
        return task
    }

    /// Starts every file that is ready. Returns their tasks.
    @discardableResult
    func convertAll() -> [Task<Void, Never>] {
        readyItems.compactMap { convert($0.id) }
    }

    func cancel(_ id: UUID) {
        runs[id]?.task.cancel()
        if let i = index(id), case .queued = items[i].phase {
            items[i].phase = .cancelled
        }
    }

    func cancelAll() {
        for item in items where item.phase.isBusy { cancel(item.id) }
    }

    /// Back to the ready state so the file can be converted again (possibly to another target).
    func convertAgain(_ id: UUID) {
        guard let i = index(id) else { return }
        resetResult(at: i)
        items[i].phase = .ready
    }

    func remove(_ id: UUID) {
        runs[id]?.task.cancel()
        runs[id] = nil
        guard let i = index(id) else { return }
        let item = items.remove(at: i)
        if let summary = item.phase.summary { store.discard(summary.outputURL) }
        if item.ownsFile { store.discard(item.url) }
    }

    func removeAll() {
        for item in items { remove(item.id) }
    }

    private func resetResult(at i: Int) {
        if let summary = items[i].phase.summary {
            store.discard(summary.outputURL)
            items[i].phase = .ready
        }
    }

    private func run(_ id: UUID, token: UUID) async {
        await limiter.acquire()
        defer { Task { await limiter.release() } }
        guard runs[id]?.token == token, let snapshot = item(id), let target = snapshot.target else { return }
        if Task.isCancelled {
            finish(id, token: token, phase: .cancelled)
            return
        }
        update(id) { $0.phase = .converting(ConversionProgress(fraction: 0, stage: "Starting")) }

        let (stream, continuation) = AsyncStream.makeStream(of: ConversionProgress.self, bufferingPolicy: .bufferingNewest(1))
        let progressTask = Task { [weak self] in
            for await report in stream {
                guard self?.runs[id]?.token == token else { continue }
                self?.update(id) { item in
                    if case .converting = item.phase { item.phase = .converting(report) }
                }
            }
        }

        var outputURL: URL?
        do {
            let url = try store.newResultURL(named: FileNaming.outputName(for: snapshot.name, extension: target.defaultExtension))
            outputURL = url
            let output = try await ConvertibleCore.convertFile(input: snapshot.url, output: url, source: snapshot.coreSourceID,
                                                               target: target.id, options: snapshot.options, progress: continuation)
            await progressTask.value
            var finalURL = output.url
            if !output.fileExtension.isEmpty, output.fileExtension != url.pathExtension {
                let renamed = url.deletingPathExtension().appendingPathExtension(output.fileExtension)
                if (try? FileManager.default.moveItem(at: url, to: renamed)) != nil { finalURL = renamed }
            }
            if Task.isCancelled || item(id) == nil {
                store.discard(finalURL)
                finish(id, token: token, phase: .cancelled)
                return
            }
            let summary = ConversionSummary(outputURL: finalURL, format: output.format, mime: output.mime,
                                            inputSize: snapshot.size, outputSize: output.size, warnings: output.warnings)
            finish(id, token: token, phase: .finished(summary))
            onItemCompleted?(true)
        } catch {
            continuation.finish()
            await progressTask.value
            if let outputURL { store.discard(outputURL) }
            let friendly = FriendlyError.from(error)
            if friendly.status == .cancelled || Task.isCancelled {
                finish(id, token: token, phase: .cancelled)
            } else {
                finish(id, token: token, phase: .failed(friendly))
                onItemCompleted?(false)
            }
        }
    }

    private func finish(_ id: UUID, token: UUID, phase: ItemPhase) {
        guard runs[id]?.token == token else { return }
        runs[id] = nil
        update(id) { $0.phase = phase }
    }

    private func update(_ id: UUID, _ change: (inout QueueItem) -> Void) {
        guard let i = index(id) else { return }
        change(&items[i])
    }

    static func fileSize(_ url: URL) -> Int64 {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let values = try? url.resourceValues(forKeys: [.fileSizeKey, .totalFileSizeKey])
        return Int64(values?.fileSize ?? values?.totalFileSize ?? 0)
    }
}

/// Limits how many conversions run at the same time.
actor ConcurrencyLimiter {
    private let limit: Int
    private var running = 0
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(limit: Int) { self.limit = max(1, limit) }

    func acquire() async {
        if running < limit {
            running += 1
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    func release() {
        if waiters.isEmpty {
            running = max(0, running - 1)
        } else {
            waiters.removeFirst().resume()
        }
    }
}
