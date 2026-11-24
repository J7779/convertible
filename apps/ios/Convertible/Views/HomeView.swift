import PhotosUI
import QuickLook
import SwiftUI
import UniformTypeIdentifiers

/// What the target picker sheet is choosing for.
enum PickerContext: Identifiable {
    case item(UUID)
    case group(String)
    var id: String {
        switch self {
        case .item(let id): return "item-\(id)"
        case .group(let id): return "group-\(id)"
        }
    }
}

struct HomeView: View {
    @EnvironmentObject private var queue: QueueModel
    @Environment(\.horizontalSizeClass) private var sizeClass

    @State private var importing = false
    @State private var dropTargeted = false
    @State private var picker: PickerContext?
    @State private var exportFiles: [ExportFile] = []
    @State private var exporting = false
    @State private var previewURL: URL?
    @State private var photoItems: [PhotosPickerItem] = []
    @State private var toast: Toast?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Space.s7) {
                header
                if queue.items.isEmpty {
                    DropZone(targeted: dropTargeted, wide: sizeClass == .regular,
                             choose: { importing = true }, photos: $photoItems, paste: paste)
                        .transition(.opacity.combined(with: .scale(scale: 0.98)))
                } else {
                    AddMoreBar(targeted: dropTargeted, choose: { importing = true }, photos: $photoItems, paste: paste)
                    ForEach(queue.groups) { group in
                        GroupBar(group: group, pick: { picker = .group(group.id) })
                    }
                    LazyVStack(spacing: Space.s4) {
                        ForEach(queue.items) { item in
                            FileCardView(item: item, actions: actions)
                                .transition(.asymmetric(insertion: .opacity.combined(with: .move(edge: .bottom)),
                                                        removal: .opacity.combined(with: .scale(scale: 0.96))))
                        }
                    }
                    .animation(Motion.spring, value: queue.items.map(\.id))
                }
            }
            .padding(.horizontal, sizeClass == .regular ? Space.s8 : Space.s5)
            .padding(.vertical, Space.s5)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity)
            .animation(Motion.standard(Motion.slow), value: queue.items.isEmpty)
        }
        .scrollDismissesKeyboard(.interactively)
        .background(Palette.background.ignoresSafeArea())
        .safeAreaInset(edge: .bottom) { ActionBar(save: { save(queue.finishedItems) }) }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .principal) {
                HStack(spacing: Space.s3) {
                    LogoMark().frame(width: 22, height: 22)
                    Text("Convertible").textStyle(.bodyStrong).foregroundStyle(Palette.textPrimary)
                }
                .accessibilityElement(children: .combine)
                .accessibilityAddTraits(.isHeader)
            }
            ToolbarItem(placement: .navigationBarLeading) {
                if !queue.items.isEmpty {
                    Menu {
                        Button(role: .destructive) { queue.removeAll() } label: {
                            Label("Clear all files", systemImage: "trash")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("More")
                    .accessibilityIdentifier("moreMenu")
                }
            }
        }
        .onDrop(of: [.item], isTargeted: $dropTargeted) { providers in
            Task {
                let urls = await queue.store.importProviders(providers)
                await queue.add(urls: urls, owned: true)
            }
            return true
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result {
                Task { await queue.add(urls: urls, owned: false) }
            }
        }
        .fileExporter(isPresented: $exporting, documents: exportFiles, contentType: .data) { result in
            if case .success(let urls) = result, !urls.isEmpty {
                show(Toast(text: urls.count == 1 ? "Saved to Files" : "Saved \(urls.count) files", symbol: "checkmark.circle.fill"))
            }
        }
        .sheet(item: $picker) { context in
            TargetPickerSheet(context: context)
        }
        .quickLookPreview($previewURL)
        .onChange(of: photoItems) { items in importPhotos(items) }
        .onChange(of: queue.items.first?.phase.kind) { kind in
            if kind == "ready", LaunchOptions.screen == "picker", picker == nil, let first = queue.items.first {
                picker = .item(first.id)
            }
        }
        .overlay(alignment: .top) { ToastView(toast: toast) }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: Space.s3) {
            if queue.items.isEmpty {
                Text("Convert anything.")
                    .textStyle(.display)
                    .foregroundStyle(Palette.textPrimary)
                    .accessibilityAddTraits(.isHeader)
                Text("Images, documents, data and more, converted right here on your device.")
                    .textStyle(.bodyLarge)
                    .foregroundStyle(Palette.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            PrivacyPill()
        }
        .padding(.top, queue.items.isEmpty ? Space.s5 : 0)
    }

    private var actions: CardActions {
        CardActions(
            pickTarget: { picker = .item($0) },
            save: { item in save([item]) },
            preview: { previewURL = $0 },
            savePhoto: { url in
                Task {
                    do {
                        try await PhotoLibrary.save(url)
                        show(Toast(text: "Saved to Photos", symbol: "checkmark.circle.fill"))
                    } catch {
                        show(Toast(text: error.localizedDescription, symbol: "exclamationmark.triangle.fill", isError: true))
                    }
                }
            }
        )
    }

    private func save(_ items: [QueueItem]) {
        exportFiles = items.compactMap { $0.phase.summary.map { ExportFile(url: $0.outputURL) } }
        exporting = !exportFiles.isEmpty
    }

    private func paste(_ providers: [NSItemProvider]) {
        Task {
            var urls = await queue.store.importProviders(providers)
            if urls.isEmpty, let text = UIPasteboard.general.string,
               let url = try? queue.store.importData(Data(text.utf8), named: "Pasted text.txt") {
                urls = [url]
            }
            await queue.add(urls: urls, owned: true)
        }
    }

    private func importPhotos(_ items: [PhotosPickerItem]) {
        guard !items.isEmpty else { return }
        photoItems = []
        Task {
            var urls: [URL] = []
            for (n, item) in items.enumerated() {
                guard let data = try? await item.loadTransferable(type: Data.self) else { continue }
                let ext = item.supportedContentTypes.first?.preferredFilenameExtension ?? "jpg"
                if let url = try? queue.store.importData(data, named: "Photo \(n + 1).\(ext)") { urls.append(url) }
            }
            await queue.add(urls: urls, owned: true)
        }
    }

    private func show(_ value: Toast) {
        withAnimation(Motion.spring) { toast = value }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.4) {
            if toast == value { withAnimation(Motion.standard()) { toast = nil } }
        }
    }
}

struct CardActions {
    let pickTarget: (UUID) -> Void
    let save: (QueueItem) -> Void
    let preview: (URL) -> Void
    let savePhoto: (URL) -> Void
}

/// "Files never leave this device."
struct PrivacyPill: View {
    var body: some View {
        Label {
            Text("Files never leave this device")
        } icon: {
            Image(systemName: "lock.shield.fill")
        }
        .textStyle(.label)
        .foregroundStyle(Palette.success)
        .padding(.horizontal, Space.s4)
        .padding(.vertical, Space.s2 + 2)
        .background(Capsule().fill(Palette.successSubtle))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("privacyStatement")
    }
}

/// The large empty-state target: choose, drop, paste or pick photos.
private struct DropZone: View {
    let targeted: Bool
    let wide: Bool
    let choose: () -> Void
    @Binding var photos: [PhotosPickerItem]
    let paste: ([NSItemProvider]) -> Void

    var body: some View {
        VStack(spacing: Space.s6) {
            ZStack {
                Circle().fill(Palette.accentSubtle).frame(width: 96, height: 96)
                LogoMark().frame(width: 52, height: 52)
            }
            .scaleEffect(targeted ? 1.08 : 1)
            VStack(spacing: Space.s3) {
                Text(targeted ? "Drop to add" : (wide ? "Drop files here" : "Add files to convert"))
                    .textStyle(.title3)
                    .foregroundStyle(Palette.textPrimary)
                Text(wide ? "or choose them from Files. Add as many as you like." : "Choose from Files, Photos or the clipboard. Add as many as you like.")
                    .textStyle(.body)
                    .foregroundStyle(Palette.textSecondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button(action: choose) {
                Label("Choose files", systemImage: "folder")
            }
            .buttonStyle(PrimaryButtonStyle())
            .frame(maxWidth: 320)
            .keyboardShortcut("o", modifiers: .command)
            .accessibilityIdentifier("chooseFilesButton")
            HStack(spacing: Space.s3) {
                PhotosPicker(selection: $photos, matching: .any(of: [.images, .videos]), photoLibrary: .shared()) {
                    Label("Photos", systemImage: "photo.on.rectangle")
                }
                .buttonStyle(SecondaryButtonStyle())
                .accessibilityIdentifier("photosButton")
                PasteButton(supportedContentTypes: [.item], payloadAction: paste)
                    .labelStyle(.titleAndIcon)
                    .buttonBorderShape(.roundedRectangle(radius: Radius.md))
                    .tint(Palette.accentFill)
                    .accessibilityIdentifier("pasteButton")
            }
        }
        .padding(.vertical, wide ? Space.s11 : Space.s9)
        .padding(.horizontal, Space.s6)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: Radius.xl, style: .continuous)
                .fill(targeted ? Palette.accentSubtle : Palette.surface)
        )
        .overlay(
            RoundedRectangle(cornerRadius: Radius.xl, style: .continuous)
                .strokeBorder(targeted ? Palette.accent : Palette.borderStrong.opacity(0.6),
                              style: StrokeStyle(lineWidth: targeted ? 2 : 1.5, dash: [7, 6]))
        )
        .animation(Motion.spring, value: targeted)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("dropZone")
    }
}

/// Compact add row shown above a non-empty queue.
private struct AddMoreBar: View {
    let targeted: Bool
    let choose: () -> Void
    @Binding var photos: [PhotosPickerItem]
    let paste: ([NSItemProvider]) -> Void

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: Space.s3) { buttons }
            VStack(alignment: .leading, spacing: Space.s3) { buttons }
        }
        .padding(Space.s3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: Radius.lg, style: .continuous)
                .fill(targeted ? Palette.accentSubtle : Color.clear)
        )
        .overlay(
            RoundedRectangle(cornerRadius: Radius.lg, style: .continuous)
                .strokeBorder(targeted ? Palette.accent : Palette.border, style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
        )
        .animation(Motion.spring, value: targeted)
    }

    @ViewBuilder private var buttons: some View {
        Button(action: choose) { Label("Add files", systemImage: "plus") }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .keyboardShortcut("o", modifiers: .command)
            .accessibilityIdentifier("addFilesButton")
        PhotosPicker(selection: $photos, matching: .any(of: [.images, .videos]), photoLibrary: .shared()) {
            Label("Photos", systemImage: "photo.on.rectangle")
        }
        .buttonStyle(SecondaryButtonStyle(compact: true))
        PasteButton(supportedContentTypes: [.item], payloadAction: paste)
            .labelStyle(.iconOnly)
            .buttonBorderShape(.roundedRectangle(radius: Radius.md))
            .tint(Palette.accentFill)
    }
}

/// One control for several files of the same format.
private struct GroupBar: View {
    @EnvironmentObject private var queue: QueueModel
    let group: SourceGroup
    let pick: () -> Void

    var body: some View {
        let first = group.itemIDs.first.flatMap { queue.item($0) }
        let target = group.commonTargetID.flatMap { id in group.targets.first { $0.id == id } }
        VStack(alignment: .leading, spacing: Space.s4) {
            HStack(spacing: Space.s3) {
                Image(systemName: "square.stack.3d.up.fill").foregroundStyle(Palette.accentText)
                Text("\(group.itemIDs.count) \(group.label) files")
                    .textStyle(.bodyStrong)
                    .foregroundStyle(Palette.textPrimary)
                Spacer(minLength: Space.s3)
                Button(action: pick) {
                    HStack(spacing: Space.s2) {
                        Text(target.map { "All to \($0.id.uppercased())" } ?? "Set all…")
                        Image(systemName: "chevron.down").font(.caption.weight(.semibold))
                    }
                }
                .buttonStyle(SecondaryButtonStyle(compact: true))
                .accessibilityLabel("Convert all \(group.label) files to \(target?.name ?? "a format")")
                .accessibilityIdentifier("groupTargetButton")
            }
            if let first, !first.form.presetChoices.isEmpty, group.commonTargetID != nil {
                Picker("Quality for all", selection: Binding(
                    get: { Preset(rawValue: first.options[OptionsForm.presetKey]?.stringValue ?? "") ?? .balanced },
                    set: { queue.setPreset($0, forGroup: group.id) })) {
                    ForEach(first.form.presetChoices) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
            }
        }
        .card(padding: Space.s4)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("groupBar.\(group.id)")
    }
}

/// Bottom bar with the one primary action.
private struct ActionBar: View {
    @EnvironmentObject private var queue: QueueModel
    let save: () -> Void

    var body: some View {
        let ready = queue.readyItems.count
        let busy = queue.busyCount
        let finished = queue.finishedItems.count
        if ready > 0 || busy > 0 || finished > 1 {
            HStack(spacing: Space.s3) {
                if busy > 0 {
                    SpinnerMark().frame(width: 28, height: 28)
                    Text(busy == 1 ? "Converting 1 file…" : "Converting \(busy) files…")
                        .textStyle(.bodyStrong)
                        .foregroundStyle(Palette.textPrimary)
                        .lineLimit(1)
                    Spacer(minLength: Space.s3)
                    Button("Cancel all", role: .destructive) { queue.cancelAll() }
                        .buttonStyle(SecondaryButtonStyle(compact: true))
                        .accessibilityIdentifier("cancelAllButton")
                } else if ready > 0 {
                    if finished > 1 {
                        Button(action: save) { Label("Save all", systemImage: "square.and.arrow.down") }
                            .buttonStyle(SecondaryButtonStyle(compact: true))
                            .accessibilityIdentifier("saveAllButton")
                    }
                    Button { queue.convertAll() } label: {
                        Label(ready == 1 ? "Convert" : "Convert \(ready) files", systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(PrimaryButtonStyle())
                    .keyboardShortcut(.return, modifiers: .command)
                    .accessibilityIdentifier("convertAllButton")
                } else {
                    Button(action: save) { Label("Save all \(finished) files", systemImage: "square.and.arrow.down") }
                        .buttonStyle(PrimaryButtonStyle())
                        .accessibilityIdentifier("saveAllButton")
                }
            }
            .padding(.horizontal, Space.s5)
            .padding(.vertical, Space.s4)
            .frame(maxWidth: 760)
            .frame(maxWidth: .infinity)
            .background(.regularMaterial)
            .overlay(alignment: .top) { Rectangle().fill(Palette.border).frame(height: 1) }
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }
}

struct Toast: Equatable {
    let id = UUID()
    let text: String
    let symbol: String
    var isError = false
}

private struct ToastView: View {
    let toast: Toast?

    var body: some View {
        if let toast {
            Label(toast.text, systemImage: toast.symbol)
                .textStyle(.bodyStrong)
                .foregroundStyle(toast.isError ? Palette.error : Palette.success)
                .padding(.horizontal, Space.s5)
                .padding(.vertical, Space.s4)
                .background(Capsule().fill(Palette.surfaceElevated))
                .overlay(Capsule().strokeBorder(Palette.border, lineWidth: 1))
                .shadow(color: .black.opacity(0.18), radius: 16, y: 6)
                .padding(.top, Space.s3)
                .padding(.horizontal, Space.s5)
                .transition(.move(edge: .top).combined(with: .opacity))
                .accessibilityAddTraits(.updatesFrequently)
                .onAppear { UIAccessibility.post(notification: .announcement, argument: toast.text) }
        }
    }
}
