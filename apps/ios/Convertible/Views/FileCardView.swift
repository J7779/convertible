import SwiftUI

/// One queued file: format badges and target, options, progress, result or error.
struct FileCardView: View {
    @EnvironmentObject private var queue: QueueModel
    let item: QueueItem
    let actions: CardActions

    var body: some View {
        VStack(alignment: .leading, spacing: Space.s4) {
            headerRow
            switch item.phase {
            case .detecting:
                EmptyView()
            case .ready, .cancelled, .failed:
                if case .failed(let error) = item.phase { ErrorBanner(error: error) }
                if case .cancelled = item.phase {
                    Label("Cancelled", systemImage: "stop.circle")
                        .textStyle(.label)
                        .foregroundStyle(Palette.textSecondary)
                }
                configuration
            case .queued, .converting:
                progressRow
            case .finished(let summary):
                ResultSection(item: item, summary: summary, actions: actions)
            }
        }
        .card()
        .animation(Motion.standard(), value: item.phase.kind)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("queueCard.\(item.name)")
    }

    // MARK: Header

    private var headerRow: some View {
        HStack(alignment: .top, spacing: Space.s4) {
            leadingIcon
                .frame(width: 44, height: 44)
            VStack(alignment: .leading, spacing: Space.s2) {
                Text(item.phase.summary?.fileName ?? item.name)
                    .textStyle(.mono)
                    .foregroundStyle(Palette.textPrimary)
                    .lineLimit(2)
                    .truncationMode(.middle)
                    .accessibilityIdentifier("fileName")
                HStack(spacing: Space.s3) {
                    Text(SizeText.string(item.size))
                        .textStyle(.caption)
                        .foregroundStyle(Palette.textSecondary)
                        .monospacedDigit()
                    if item.phase != .detecting {
                        FormatBadge(label: item.sourceLabel, category: item.sourceCategory, shape: .source)
                            .accessibilityLabel("Detected format \(item.source?.name ?? item.sourceLabel)")
                    } else {
                        Text("Detecting…").textStyle(.caption).foregroundStyle(Palette.textSecondary)
                    }
                }
            }
            Spacer(minLength: 0)
            if !item.phase.isBusy {
                Button { queue.remove(item.id) } label: {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(Palette.textSecondary)
                        .frame(width: Sizes.touchTarget, height: Sizes.touchTarget)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Remove \(item.name)")
                .accessibilityIdentifier("removeButton")
                .padding(.top, -Space.s3)
                .padding(.trailing, -Space.s3)
            }
        }
    }

    @ViewBuilder private var leadingIcon: some View {
        switch item.phase {
        case .converting(let progress):
            ProgressMark(progress: progress.fraction)
        case .queued:
            SpinnerMark()
        case .finished:
            ProgressMark(progress: 1, celebrate: true)
        default:
            ZStack {
                RoundedRectangle(cornerRadius: Radius.md, style: .continuous)
                    .fill(Palette.category(item.sourceCategory).opacity(0.14))
                Image(systemName: FileCategory.symbol(item.sourceCategory))
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(Palette.category(item.sourceCategory))
            }
            .accessibilityHidden(true)
        }
    }

    // MARK: Configuration

    @ViewBuilder private var configuration: some View {
        if item.targets.isEmpty {
            Text("No conversions are available for this file.")
                .textStyle(.body)
                .foregroundStyle(Palette.textSecondary)
        } else {
            targetRow
            OptionsFormView(item: item)
            HStack(spacing: Space.s3) {
                if let compress = item.compressTarget, item.targetID != compress.id {
                    Button { queue.applyCompress(for: item.id) } label: {
                        Label(compress.compress ? "Compress" : "Compress to \(compress.id.uppercased())",
                              systemImage: "arrow.down.right.and.arrow.up.left")
                    }
                    .buttonStyle(SecondaryButtonStyle(compact: true))
                    .accessibilityIdentifier("compressButton")
                }
                Spacer(minLength: 0)
                Button { queue.convert(item.id) } label: {
                    Label(item.phase == .ready ? "Convert" : "Try again", systemImage: "arrow.right")
                }
                .buttonStyle(SecondaryButtonStyle(compact: true))
                .disabled(item.target == nil)
                .accessibilityIdentifier("convertButton")
            }
        }
    }

    private var targetRow: some View {
        Button { actions.pickTarget(item.id) } label: {
            HStack(spacing: Space.s3) {
                Image(systemName: "arrow.right")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Palette.textSecondary)
                if let target = item.target {
                    FormatBadge(label: target.compress ? "\(target.id) · same" : target.id,
                                category: target.category, shape: .target)
                    Text(target.name)
                        .textStyle(.body)
                        .foregroundStyle(Palette.textPrimary)
                        .lineLimit(1)
                } else {
                    Text("Choose a format").textStyle(.body).foregroundStyle(Palette.textSecondary)
                }
                Spacer(minLength: Space.s2)
                Image(systemName: "chevron.up.chevron.down")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Palette.textSecondary)
            }
            .padding(.horizontal, Space.s4)
            .frame(minHeight: Sizes.touchTarget)
            .background(RoundedRectangle(cornerRadius: Radius.md, style: .continuous).fill(Palette.surfaceSunken))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Convert to \(item.target?.name ?? "no format chosen")")
        .accessibilityHint("Opens the list of formats")
        .accessibilityIdentifier("targetPickerButton")
    }

    // MARK: Progress

    private var progressRow: some View {
        let progress: ConversionProgress? = { if case .converting(let p) = item.phase { return p } else { return nil } }()
        return HStack(spacing: Space.s4) {
            VStack(alignment: .leading, spacing: Space.s2) {
                HStack(spacing: Space.s2) {
                    Text(progress.map { $0.stage.isEmpty ? "Converting" : $0.stage } ?? "Waiting")
                        .textStyle(.bodyStrong)
                        .foregroundStyle(Palette.textPrimary)
                    if let target = item.target {
                        Text("→ \(target.id.uppercased())").textStyle(.formatLabel).foregroundStyle(Palette.textSecondary)
                    }
                }
                ProgressBar(fraction: progress?.fraction ?? 0)
                Text(progress.map { "\(Int(($0.fraction * 100).rounded()))%" } ?? "Queued")
                    .textStyle(.caption)
                    .monospacedDigit()
                    .foregroundStyle(Palette.textSecondary)
                    .contentTransition(.numericText())
            }
            Button(role: .cancel) { queue.cancel(item.id) } label: {
                Text("Cancel")
            }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .accessibilityLabel("Cancel converting \(item.name)")
            .accessibilityIdentifier("cancelButton")
        }
        .accessibilityElement(children: .contain)
    }
}

/// Thin determinate bar in the Flux gradient.
struct ProgressBar: View {
    let fraction: Double
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Palette.surfaceSunken)
                Capsule().fill(Palette.flux)
                    .frame(width: max(6, geo.size.width * (fraction.isFinite ? min(max(fraction, 0), 1) : 0)))
                    .animation(reduceMotion ? nil : Motion.standard(Motion.base), value: fraction)
            }
        }
        .frame(height: 6)
        .accessibilityHidden(true)
    }
}

private struct ErrorBanner: View {
    let error: FriendlyError

    var body: some View {
        HStack(alignment: .top, spacing: Space.s3) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Palette.error)
            VStack(alignment: .leading, spacing: Space.s1) {
                Text(error.title).textStyle(.bodyStrong).foregroundStyle(Palette.error)
                Text(error.message)
                    .textStyle(.body)
                    .foregroundStyle(Palette.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(Space.s4)
        .background(RoundedRectangle(cornerRadius: Radius.md, style: .continuous).fill(Palette.errorSubtle))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("errorBanner")
    }
}

/// A finished conversion: sizes, warnings and what to do with the result.
private struct ResultSection: View {
    @EnvironmentObject private var queue: QueueModel
    let item: QueueItem
    let summary: ConversionSummary
    let actions: CardActions
    @State private var showWarnings = false

    var body: some View {
        VStack(alignment: .leading, spacing: Space.s4) {
            HStack(spacing: Space.s3) {
                Text(SizeText.string(summary.inputSize))
                    .foregroundStyle(Palette.textSecondary)
                Image(systemName: "arrow.right").font(.caption.weight(.semibold)).foregroundStyle(Palette.textSecondary)
                Text(SizeText.string(summary.outputSize))
                    .foregroundStyle(Palette.textPrimary)
                Text(summary.change.summary)
                    .textStyle(.label)
                    .foregroundStyle(summary.change.isSmaller ? Palette.success : Palette.textSecondary)
                    .padding(.horizontal, Space.s3)
                    .padding(.vertical, Space.s1)
                    .background(Capsule().fill(summary.change.isSmaller ? Palette.successSubtle : Palette.surfaceSunken))
            }
            .textStyle(.bodyStrong)
            .monospacedDigit()
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Converted from \(SizeText.string(summary.inputSize)) to \(SizeText.string(summary.outputSize)), \(summary.change.summary)")
            .accessibilityIdentifier("resultSizes")

            if !summary.warnings.isEmpty {
                DisclosureGroup(isExpanded: $showWarnings) {
                    VStack(alignment: .leading, spacing: Space.s2) {
                        ForEach(summary.warnings, id: \.self) { warning in
                            Text("• \(warning)").textStyle(.caption).foregroundStyle(Palette.textSecondary)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, Space.s2)
                } label: {
                    Label(summary.warnings.count == 1 ? "1 note" : "\(summary.warnings.count) notes", systemImage: "info.circle")
                        .textStyle(.label)
                        .foregroundStyle(Palette.warning)
                }
                .accessibilityIdentifier("warnings")
            }

            ViewThatFits(in: .horizontal) {
                HStack(spacing: Space.s3) { buttons }
                VStack(alignment: .leading, spacing: Space.s3) {
                    HStack(spacing: Space.s3) { primaryButtons }
                    HStack(spacing: Space.s3) { secondaryButtons }
                }
            }
        }
    }

    @ViewBuilder private var buttons: some View {
        primaryButtons
        secondaryButtons
    }

    @ViewBuilder private var primaryButtons: some View {
        Button { actions.save(item) } label: { Label("Save", systemImage: "square.and.arrow.down") }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .accessibilityLabel("Save \(summary.fileName) to Files")
            .accessibilityIdentifier("resultSaveButton")
        ShareLink(item: summary.outputURL) { Label("Share", systemImage: "square.and.arrow.up") }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .accessibilityIdentifier("resultShareButton")
        if PhotoLibrary.canSave(mime: summary.mime, url: summary.outputURL) {
            Button { actions.savePhoto(summary.outputURL) } label: { Label("Photos", systemImage: "photo") }
                .buttonStyle(SecondaryButtonStyle(compact: true))
                .accessibilityLabel("Save to Photos")
                .accessibilityIdentifier("resultPhotosButton")
        }
    }

    @ViewBuilder private var secondaryButtons: some View {
        Button { actions.preview(summary.outputURL) } label: { Label("Preview", systemImage: "eye") }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .accessibilityIdentifier("resultPreviewButton")
        Button { queue.convertAgain(item.id) } label: { Label("Again", systemImage: "arrow.counterclockwise") }
            .buttonStyle(SecondaryButtonStyle(compact: true))
            .accessibilityLabel("Convert again")
            .accessibilityIdentifier("convertAgainButton")
    }
}
