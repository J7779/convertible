import SwiftUI

enum AppSection: String, Hashable, CaseIterable, Identifiable {
    case convert, formats, settings
    var id: String { rawValue }
    var title: String {
        switch self {
        case .convert: return "Convert"
        case .formats: return "Formats"
        case .settings: return "Settings"
        }
    }
    var symbol: String {
        switch self {
        case .convert: return "arrow.triangle.2.circlepath"
        case .formats: return "square.grid.2x2"
        case .settings: return "gearshape"
        }
    }
}

struct RootView: View {
    @EnvironmentObject private var settings: AppSettings
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var showIntro = !LaunchOptions.skipIntro && !UIAccessibility.isVoiceOverRunning

    var body: some View {
        ZStack {
            Group {
                if sizeClass == .regular {
                    SplitLayout()
                } else {
                    PhoneLayout()
                }
            }
            if showIntro {
                IntroOverlay { withAnimation(Motion.standard(Motion.slow)) { showIntro = false } }
                    .transition(.opacity)
                    .zIndex(1)
            }
        }
        .tint(Palette.accentText)
        .preferredColorScheme(settings.theme.colorScheme)
    }
}

/// The intro on the app background. Tap (or any key/VoiceOver activation) skips it.
private struct IntroOverlay: View {
    let onFinished: () -> Void

    var body: some View {
        ZStack {
            Palette.background.ignoresSafeArea()
            IntroAnimationView(onFinished: onFinished)
                .frame(width: 160, height: 160)
        }
        .contentShape(Rectangle())
        .onTapGesture(perform: onFinished)
        .accessibilityElement()
        .accessibilityLabel("Convertible")
        .accessibilityAddTraits(.isButton)
        .accessibilityHint("Skips the intro")
        .accessibilityAction { onFinished() }
        .accessibilityIdentifier("introOverlay")
    }
}

/// iPhone: one stack, Formats and Settings reachable from the toolbar.
private struct PhoneLayout: View {
    @State private var path: [AppSection] = AppSection(rawValue: LaunchOptions.screen ?? "").map { $0 == .convert ? [] : [$0] } ?? []

    var body: some View {
        NavigationStack(path: $path) {
            HomeView()
                .navigationDestination(for: AppSection.self) { section in
                    switch section {
                    case .formats: FormatsBrowserView()
                    case .settings: SettingsView()
                    case .convert: HomeView()
                    }
                }
                .toolbar {
                    ToolbarItemGroup(placement: .navigationBarTrailing) {
                        NavigationLink(value: AppSection.formats) {
                            Image(systemName: AppSection.formats.symbol)
                        }
                        .accessibilityLabel("Supported formats")
                        .accessibilityIdentifier("formatsButton")
                        NavigationLink(value: AppSection.settings) {
                            Image(systemName: AppSection.settings.symbol)
                        }
                        .accessibilityLabel("Settings")
                        .accessibilityIdentifier("settingsButton")
                    }
                }
        }
    }
}

/// iPad (regular width): sidebar with sections, detail on the right.
private struct SplitLayout: View {
    @State private var selection: AppSection? = AppSection(rawValue: LaunchOptions.screen ?? "") ?? .convert
    @State private var visibility: NavigationSplitViewVisibility = .all

    var body: some View {
        NavigationSplitView(columnVisibility: $visibility) {
            List(selection: $selection) {
                ForEach(AppSection.allCases) { section in
                    Label(section.title, systemImage: section.symbol)
                        .textStyle(.bodyStrong)
                        .tag(section)
                        .accessibilityIdentifier("sidebar.\(section.rawValue)")
                }
            }
            .navigationTitle("Convertible")
            .tint(Palette.accentFill)
            .scrollContentBackground(.hidden)
            .background(Palette.surfaceSunken)
        } detail: {
            NavigationStack {
                switch selection ?? .convert {
                case .convert: HomeView()
                case .formats: FormatsBrowserView()
                case .settings: SettingsView()
                }
            }
        }
        .navigationSplitViewStyle(.balanced)
    }
}
