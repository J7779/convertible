import SwiftUI

@main
struct ConvertibleApp: App {
    @StateObject private var settings = AppSettings()
    @StateObject private var queue = QueueModel()

    init() {
        UITestHooks.prepare()
        // Nothing survives a session: clear copies and results left from the last run.
        FileStore.shared.resetAll()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(settings)
                .environmentObject(queue)
                .onOpenURL { url in
                    // "Open in" / share sheet / Files "Open with": the URL is security-scoped
                    // (opened in place) or a copy in Documents/Inbox that we own.
                    Task { await queue.add(urls: [url], owned: FileStore.shared.owns(url)) }
                }
                .task { await configure() }
        }
    }

    @MainActor
    private func configure() async {
        queue.preferences = { [weak settings] in
            (settings?.defaultPreset ?? .balanced, settings?.stripMetadata ?? true)
        }
        queue.onItemCompleted = { success in
            if success { Haptics.success() } else { Haptics.failure() }
        }
        await LaunchOptions.apply(to: queue)
    }
}

/// Launch arguments used for screenshots and UI tests:
///   -ConvertibleSkipIntro         start without the logo intro
///   -ConvertibleSampleFiles       queue a few small sample files
///   -ConvertibleLargeSample       queue a large binary file (to watch progress)
///   -ConvertibleAutoConvert       start converting the queued samples
///   -ConvertibleScreen <name>     open "formats", "settings" or "picker" (first file's target picker)
enum LaunchOptions {
    static var arguments: [String] { ProcessInfo.processInfo.arguments }
    static var skipIntro: Bool { arguments.contains("-ConvertibleSkipIntro") }
    static var screen: String? {
        guard let i = arguments.firstIndex(of: "-ConvertibleScreen"), i + 1 < arguments.count else { return nil }
        return arguments[i + 1]
    }

    @MainActor
    static func apply(to queue: QueueModel) async {
        var urls: [URL] = []
        let store = queue.store
        if arguments.contains("-ConvertibleSampleFiles") {
            let samples: [(String, String)] = [
                ("Meeting notes.txt", "Agenda\n1. Offline conversion\n2. Privacy first\n3. Ship it\n"),
                ("Release notes.txt", "Convertible 1.0 converts files on your device. Nothing is uploaded.\n"),
                ("payload.b64", "Q29udmVydGlibGUgcnVucyBlbnRpcmVseSBvbiB5b3VyIGRldmljZS4K\n"),
            ]
            for (name, text) in samples {
                if let url = try? store.importData(Data(text.utf8), named: name) { urls.append(url) }
            }
        }
        if arguments.contains("-ConvertibleLargeSample"),
           let url = try? store.newInboxURL(named: "Camera dump.bin") {
            let mebibytes = UITestHooks.largeSampleMiB
            let written = await Task.detached(priority: .userInitiated) { () -> Bool in
                FileManager.default.createFile(atPath: url.path, contents: nil)
                guard let handle = try? FileHandle(forWritingTo: url) else { return false }
                var chunk = Data(count: 1 << 20)
                chunk.withUnsafeMutableBytes { raw in
                    for i in 0..<raw.count { raw[i] = UInt8(truncatingIfNeeded: i &* 2654435761 >> 13) }
                }
                for _ in 0..<mebibytes { handle.write(chunk) }
                try? handle.close()
                return true
            }.value
            if written { urls.append(url) }
        }
        urls += UITestHooks.importFiles(into: store)
        guard !urls.isEmpty else { return }
        await queue.add(urls: urls, owned: true)
        if arguments.contains("-ConvertibleAutoConvert") { queue.convertAll() }
    }
}
