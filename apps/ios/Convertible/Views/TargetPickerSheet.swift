import SwiftUI

/// Searchable list of target formats, grouped by category.
struct TargetPickerSheet: View {
    @EnvironmentObject private var queue: QueueModel
    @Environment(\.dismiss) private var dismiss
    let context: PickerContext
    @State private var query = ""
    /// Sections are computed off the body (sources have up to ~400 targets and the
    /// queue publishes progress many times a second while files convert).
    @State private var sections: [TargetSection]?

    private struct Key: Equatable { let query: String; let targets: [TargetFormat]; let source: String? }

    private var targets: [TargetFormat] {
        switch context {
        case .item(let id): return queue.item(id)?.targets ?? []
        case .group(let id): return queue.groups.first { $0.id == id }?.targets ?? []
        }
    }

    private var selectedID: String? {
        switch context {
        case .item(let id): return queue.item(id)?.targetID
        case .group(let id): return queue.groups.first { $0.id == id }?.commonTargetID
        }
    }

    private var sourceCategory: String? {
        switch context {
        case .item(let id): return queue.item(id)?.source?.category
        case .group(let id): return queue.groups.first { $0.id == id }?.itemIDs.first.flatMap { queue.item($0)?.source?.category }
        }
    }

    var body: some View {
        let shown = sections ?? TargetCatalog.sections(targets: targets, sourceCategory: sourceCategory,
                                                       popularity: queue.catalog.popularity, query: query)
        NavigationStack {
            List {
                if shown.isEmpty {
                    Text(query.isEmpty ? "No formats available." : "No format matches “\(query)”.")
                        .textStyle(.body)
                        .foregroundStyle(Palette.textSecondary)
                        .listRowBackground(Color.clear)
                }
                ForEach(shown) { section in
                    Section {
                        ForEach(section.targets) { target in
                            Button { choose(target) } label: { TargetRow(target: target, selected: target.id == selectedID) }
                                .accessibilityIdentifier("target.\(target.id)")
                                .listRowBackground(Palette.surface)
                        }
                    } header: {
                        Text(section.title).textStyle(.overline).foregroundStyle(Palette.textSecondary)
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollContentBackground(.hidden)
            .background(Palette.background)
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Search formats")
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }.accessibilityIdentifier("pickerDoneButton")
                }
            }
        }
        .task(id: Key(query: query, targets: targets, source: sourceCategory)) {
            if sections != nil, !query.isEmpty { try? await Task.sleep(nanoseconds: 80_000_000) }
            if Task.isCancelled { return }
            sections = TargetCatalog.sections(targets: targets, sourceCategory: sourceCategory,
                                              popularity: queue.catalog.popularity, query: query)
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .accessibilityIdentifier("targetPicker")
    }

    private var title: String {
        if case .group(let id) = context, let group = queue.groups.first(where: { $0.id == id }) {
            return "Convert \(group.itemIDs.count) \(group.label) files to"
        }
        return "Convert to"
    }

    private func choose(_ target: TargetFormat) {
        Haptics.selection()
        switch context {
        case .item(let id): queue.setTarget(target.id, for: id)
        case .group(let id): queue.setTarget(target.id, forGroup: id)
        }
        dismiss()
    }
}

private struct TargetRow: View {
    let target: TargetFormat
    let selected: Bool

    var body: some View {
        HStack(spacing: Space.s4) {
            Rectangle()
                .fill(Palette.category(target.category))
                .frame(width: 10, height: 10)
            VStack(alignment: .leading, spacing: Space.s1) {
                Text(target.name).textStyle(.body).foregroundStyle(Palette.textPrimary)
                HStack(spacing: Space.s3) {
                    Text(target.extensions.prefix(3).map { ".\($0)" }.joined(separator: " "))
                        .textStyle(.mono)
                        .foregroundStyle(Palette.textSecondary)
                    if target.compress {
                        Tag(text: "Same format", color: Palette.accentText)
                    }
                    if target.lossy {
                        Tag(text: "Lossy", color: Palette.warning)
                    }
                }
            }
            Spacer(minLength: Space.s3)
            if selected {
                Image(systemName: "checkmark").font(.body.weight(.semibold)).foregroundStyle(Palette.accentText)
            }
        }
        .frame(minHeight: Sizes.touchTarget)
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

struct Tag: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .textStyle(.caption)
            .foregroundStyle(color)
            .padding(.horizontal, Space.s2 + 2)
            .padding(.vertical, 1)
            .overlay(Capsule().strokeBorder(color.opacity(0.5), lineWidth: 1))
    }
}
