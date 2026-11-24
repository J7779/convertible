// User preferences. Only these three choices are stored (in UserDefaults);
// no files, file names or conversion history are ever persisted.
import SwiftUI

enum ThemeChoice: String, CaseIterable, Identifiable {
    case system, dark, light
    var id: String { rawValue }
    var title: String {
        switch self {
        case .system: return "System"
        case .dark: return "Dark"
        case .light: return "Light"
        }
    }
    var colorScheme: ColorScheme? {
        switch self {
        case .system: return nil
        case .dark: return .dark
        case .light: return .light
        }
    }
}

@MainActor
final class AppSettings: ObservableObject {
    private let defaults: UserDefaults

    @Published var theme: ThemeChoice { didSet { defaults.set(theme.rawValue, forKey: Keys.theme) } }
    @Published var defaultPreset: Preset { didSet { defaults.set(defaultPreset.rawValue, forKey: Keys.preset) } }
    @Published var stripMetadata: Bool { didSet { defaults.set(stripMetadata, forKey: Keys.strip) } }

    private enum Keys {
        static let theme = "theme"
        static let preset = "defaultPreset"
        static let strip = "stripMetadata"
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        theme = ThemeChoice(rawValue: defaults.string(forKey: Keys.theme) ?? "") ?? .system
        defaultPreset = Preset(rawValue: defaults.string(forKey: Keys.preset) ?? "") ?? .balanced
        stripMetadata = defaults.object(forKey: Keys.strip) as? Bool ?? true
    }
}
