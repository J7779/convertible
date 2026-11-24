import SwiftUI

/// Searchable catalogue of every format the core knows.
struct FormatsBrowserView: View {
    @EnvironmentObject private var queue: QueueModel
    @State private var query = ""

    var body: some View {
        let sections = FormatsBrowser.sections(queue.catalog.formats, query: query)
        List {
            Section {
                HStack(spacing: Space.s5) {
                    Legend(symbol: "arrow.down.doc", text: "Reads")
                    Legend(symbol: "arrow.up.doc", text: "Writes")
                    Spacer()
                    Text("\(queue.catalog.formats.count) formats")
                        .textStyle(.label)
                        .foregroundStyle(Palette.textSecondary)
                        .monospacedDigit()
                }
                .listRowBackground(Color.clear)
                Text("Any file, even one that can’t be read, can still be turned into encodings such as Base64, hex or checksums.")
                    .textStyle(.caption)
                    .foregroundStyle(Palette.textSecondary)
                    .listRowBackground(Color.clear)
            }
            if sections.isEmpty {
                Text("No format matches “\(query)”.")
                    .textStyle(.body)
                    .foregroundStyle(Palette.textSecondary)
                    .listRowBackground(Color.clear)
            }
            ForEach(sections) { section in
                Section {
                    ForEach(section.formats) { format in
                        FormatRow(format: format).listRowBackground(Palette.surface)
                    }
                } header: {
                    HStack(spacing: Space.s3) {
                        Image(systemName: FileCategory.symbol(section.id)).foregroundStyle(Palette.category(section.id))
                        Text(section.title).textStyle(.overline).foregroundStyle(Palette.textSecondary)
                        Text("\(section.formats.count)").textStyle(.caption).foregroundStyle(Palette.textSecondary)
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .background(Palette.background.ignoresSafeArea())
        .searchable(text: $query, prompt: "Search formats")
        .navigationTitle("Formats")
        .accessibilityIdentifier("formatsBrowser")
    }
}

private struct Legend: View {
    let symbol: String
    let text: String
    var body: some View {
        Label(text, systemImage: symbol)
            .textStyle(.label)
            .foregroundStyle(Palette.textSecondary)
    }
}

private struct FormatRow: View {
    let format: FormatInfo

    var body: some View {
        HStack(spacing: Space.s4) {
            VStack(alignment: .leading, spacing: Space.s1) {
                Text(format.name).textStyle(.body).foregroundStyle(Palette.textPrimary)
                Text(format.extensions.prefix(4).map { ".\($0)" }.joined(separator: " "))
                    .textStyle(.mono)
                    .foregroundStyle(Palette.textSecondary)
                    .lineLimit(1)
            }
            Spacer(minLength: Space.s3)
            Capability(symbol: "arrow.down.doc", active: format.readable)
            Capability(symbol: "arrow.up.doc", active: format.writable)
        }
        .frame(minHeight: Sizes.touchTarget)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(format.name)
        .accessibilityValue(accessibilityCapabilities)
    }

    private var accessibilityCapabilities: String {
        switch (format.readable, format.writable) {
        case (true, true): return "Reads and writes"
        case (true, false): return "Reads only"
        case (false, true): return "Writes only"
        case (false, false): return "Recognised; converts to encodings"
        }
    }
}

private struct Capability: View {
    let symbol: String
    let active: Bool
    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 15, weight: .medium))
            .foregroundStyle(active ? Palette.success : Palette.textDisabled.opacity(0.6))
            .frame(width: 24)
    }
}
