// Debug-only hooks for the XCUITest end-to-end suite (ConvertibleUITests).
//
// Everything here is inert unless the app is a Debug build AND was launched with
// `-ConvertibleUITestHooks`; Release builds compile `isEnabled` to a constant `false`.
// With the hooks on:
//   - `-ConvertibleUITestResetSettings` clears the stored settings before they load,
//     so every test starts from a clean state;
//   - `-ConvertibleUITestReduceMotion` makes the intro behave as with Reduce Motion on;
//   - the environment variable `CONVERTIBLE_UITEST_FILES` (a JSON array of
//     {"name": ..., "base64": ...}) queues those exact files at launch;
//   - `CONVERTIBLE_UITEST_LARGE_MIB` sets the size of the `-ConvertibleLargeSample` file;
//   - each queued file and each finished result carry an invisible accessibility
//     element (`uitest.item`, `uitest.output`) whose label holds the item's state and
//     the result's bytes, so the UI test can verify real output without leaving the app.
import Foundation
import SwiftUI

enum UITestHooks {
    static let isEnabled: Bool = {
        #if DEBUG
        return ProcessInfo.processInfo.arguments.contains("-ConvertibleUITestHooks")
        #else
        return false
        #endif
    }()

    private static func flag(_ name: String) -> Bool { isEnabled && ProcessInfo.processInfo.arguments.contains(name) }

    static var reduceMotion: Bool { flag("-ConvertibleUITestReduceMotion") }

    static var largeSampleMiB: Int {
        guard isEnabled, let raw = ProcessInfo.processInfo.environment["CONVERTIBLE_UITEST_LARGE_MIB"],
              let value = Int(raw), value > 0 else { return 96 }
        return min(value, 1024)
    }

    /// Runs before any state is created.
    static func prepare() {
        guard flag("-ConvertibleUITestResetSettings") else { return }
        for key in ["theme", "defaultPreset", "stripMetadata"] { UserDefaults.standard.removeObject(forKey: key) }
    }

    /// The files given in `CONVERTIBLE_UITEST_FILES`, written into the inbox.
    static func importFiles(into store: FileStore) -> [URL] {
        guard isEnabled, let json = ProcessInfo.processInfo.environment["CONVERTIBLE_UITEST_FILES"],
              let entries = try? JSONDecoder().decode([FileEntry].self, from: Data(json.utf8)) else { return [] }
        return entries.compactMap { entry in
            guard let data = Data(base64Encoded: entry.base64) else { return nil }
            return try? store.importData(data, named: entry.name)
        }
    }

    private struct FileEntry: Decodable {
        let name: String
        let base64: String
    }

    /// Results up to this size are exposed byte for byte; larger ones by size only.
    static let maxExposedBytes = 256 * 1024

    static func itemDescription(_ item: QueueItem) -> String {
        let payload: [String: Any] = [
            "name": item.name,
            "detected": item.detectedID ?? "",
            "target": item.targetID ?? "",
            "phase": item.phase.kind,
            "options": item.options.values.mapValues { value -> Any in
                switch value {
                case .int(let v): return v
                case .float(let v): return v
                case .bool(let v): return v
                case .string(let v): return v
                }
            },
            "targets": item.targets.map { ["id": $0.id, "category": $0.category, "compress": $0.compress] },
        ]
        let data = (try? JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])) ?? Data()
        return String(decoding: data, as: UTF8.self)
    }

    static func outputDescription(_ summary: ConversionSummary) -> String {
        if summary.outputSize > maxExposedBytes { return "name:\(summary.fileName)|size:\(summary.outputSize)" }
        guard let data = try? Data(contentsOf: summary.outputURL) else { return "unreadable" }
        return "name:\(summary.fileName)|size:\(data.count)|base64:\(data.base64EncodedString())"
    }
}

/// An invisible element carrying test data. Only ever created when the hooks are on.
struct UITestProbe: View {
    let identifier: String
    let text: String

    var body: some View {
        Color.clear
            .frame(width: 1, height: 1)
            .accessibilityElement()
            .accessibilityLabel(text)
            .accessibilityIdentifier(identifier)
    }
}
