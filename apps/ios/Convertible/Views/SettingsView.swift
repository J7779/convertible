import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var settings: AppSettings

    private var appVersion: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "1.0"
        let build = info?["CFBundleVersion"] as? String ?? "1"
        return "\(version) (\(build))"
    }

    var body: some View {
        Form {
            Section {
                Picker("Appearance", selection: $settings.theme) {
                    ForEach(ThemeChoice.allCases) { Text($0.title).tag($0) }
                }
                .accessibilityIdentifier("themePicker")
            } header: { header("Appearance") }

            Section {
                Picker("Default quality", selection: $settings.defaultPreset) {
                    ForEach(Preset.allCases) { Text($0.title).tag($0) }
                }
                .accessibilityIdentifier("defaultPresetPicker")
                Toggle("Strip metadata", isOn: $settings.stripMetadata)
                    .tint(Palette.accentFill)
                    .accessibilityIdentifier("stripMetadataToggle")
            } header: { header("Conversion") } footer: {
                Text("Removes location, camera and author details from converted files where the format carries them.")
                    .textStyle(.caption)
            }

            Section {
                NavigationLink { PrivacyView() } label: { Label("Privacy", systemImage: "lock.shield") }
                    .accessibilityIdentifier("privacyLink")
                NavigationLink { LicensesView() } label: { Label("Open-source licences", systemImage: "doc.text") }
                    .accessibilityIdentifier("licensesLink")
            } header: { header("About") }

            Section {
                LabeledContent("App version", value: appVersion)
                LabeledContent("Core version", value: ConvertibleCore.version)
                    .accessibilityIdentifier("coreVersion")
            } footer: {
                VStack(alignment: .leading, spacing: Space.s3) {
                    HStack(spacing: Space.s3) {
                        LogoMark().frame(width: 20, height: 20)
                        Text("Convertible").textStyle(.label)
                    }
                    Text("Offline, private file conversion. Free and open source.").textStyle(.caption)
                }
                .padding(.top, Space.s5)
            }
        }
        .textStyle(.body)
        .scrollContentBackground(.hidden)
        .background(Palette.background.ignoresSafeArea())
        .navigationTitle("Settings")
    }

    private func header(_ text: String) -> some View {
        Text(text).textStyle(.overline).foregroundStyle(Palette.textSecondary)
    }
}

struct PrivacyView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Space.s6) {
                PrivacyPill()
                ForEach(points, id: \.0) { point in
                    HStack(alignment: .top, spacing: Space.s4) {
                        Image(systemName: point.0)
                            .font(.title3)
                            .foregroundStyle(Palette.accentText)
                            .frame(width: 32)
                        VStack(alignment: .leading, spacing: Space.s2) {
                            Text(point.1).textStyle(.bodyStrong).foregroundStyle(Palette.textPrimary)
                            Text(point.2).textStyle(.body).foregroundStyle(Palette.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
            }
            .padding(Space.s5)
            .frame(maxWidth: 640, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .background(Palette.background.ignoresSafeArea())
        .navigationTitle("Privacy")
    }

    private let points: [(String, String, String)] = [
        ("iphone", "Everything happens on this device",
         "Files are converted by the app itself. Nothing is uploaded, and the app never connects to the internet."),
        ("person.crop.circle.badge.xmark", "No accounts, no tracking",
         "There is no sign-in, no analytics, no advertising and no crash reporting."),
        ("clock.arrow.circlepath", "No history",
         "Converted files live in a temporary folder until you save them. Recent conversions are kept in memory only and are gone when the app closes."),
        ("hand.raised", "Minimal permissions",
         "The app only sees the files you give it. It asks to add to your photo library only when you choose to save an image there."),
        ("eye.slash", "Metadata stripped by default",
         "Where a format carries details such as location or author, they are removed unless you turn this off in Settings."),
    ]
}

struct LicensesView: View {
    private struct Notice: Identifiable {
        let id: String
        let title: String
        let resource: String
    }

    private let notices = [
        Notice(id: "convertible", title: "Convertible (MIT)", resource: "LICENSE"),
        Notice(id: "inter", title: "Inter typeface (SIL OFL 1.1)", resource: "Inter-LICENSE.txt"),
        Notice(id: "jetbrains", title: "JetBrains Mono (SIL OFL 1.1)", resource: "JetBrainsMono-LICENSE.txt"),
    ]

    var body: some View {
        List {
            Section {
                Text("Convertible is open source. It uses no third-party code beyond the components listed here.")
                    .textStyle(.body)
                    .foregroundStyle(Palette.textSecondary)
                    .listRowBackground(Color.clear)
            }
            ForEach(notices) { notice in
                NavigationLink {
                    ScrollView {
                        Text(text(of: notice.resource))
                            .textStyle(.mono)
                            .foregroundStyle(Palette.textPrimary)
                            .textSelection(.enabled)
                            .padding(Space.s5)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .background(Palette.background.ignoresSafeArea())
                    .navigationTitle(notice.title)
                    .navigationBarTitleDisplayMode(.inline)
                } label: {
                    Text(notice.title).textStyle(.body)
                }
                .listRowBackground(Palette.surface)
                .accessibilityIdentifier("license.\(notice.id)")
            }
        }
        .scrollContentBackground(.hidden)
        .background(Palette.background.ignoresSafeArea())
        .navigationTitle("Licences")
    }

    private func text(of resource: String) -> String {
        let name = (resource as NSString).deletingPathExtension
        let ext = (resource as NSString).pathExtension
        guard let url = Bundle.main.url(forResource: name, withExtension: ext.isEmpty ? nil : ext),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return "Licence text unavailable." }
        return text
    }
}
