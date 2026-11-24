import SwiftUI

/// Quality preset plus the conversion's options, rendered generically from the schema.
struct OptionsFormView: View {
    @EnvironmentObject private var queue: QueueModel
    let item: QueueItem
    @State private var showAdvanced = false

    var body: some View {
        let form = item.form
        VStack(alignment: .leading, spacing: Space.s4) {
            if !form.presetChoices.isEmpty {
                VStack(alignment: .leading, spacing: Space.s2) {
                    Text("Quality").textStyle(.overline).foregroundStyle(Palette.textSecondary)
                    Picker("Quality", selection: presetBinding) {
                        ForEach(form.presetChoices) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .accessibilityIdentifier("presetControl")
                }
            }
            ForEach(form.basic) { field in
                OptionFieldView(field: field, value: binding(for: field.spec))
            }
            if !form.advanced.isEmpty {
                DisclosureGroup(isExpanded: $showAdvanced) {
                    VStack(alignment: .leading, spacing: Space.s4) {
                        ForEach(form.advanced) { field in
                            OptionFieldView(field: field, value: binding(for: field.spec))
                        }
                    }
                    .padding(.top, Space.s3)
                } label: {
                    Text("Advanced")
                        .textStyle(.label)
                        .foregroundStyle(Palette.accentText)
                }
                .accessibilityIdentifier("advancedOptionsToggle")
            }
        }
    }

    private var presetBinding: Binding<Preset> {
        Binding(
            get: { Preset(rawValue: item.options[OptionsForm.presetKey]?.stringValue ?? "") ?? .balanced },
            set: { queue.setOption(OptionsForm.presetKey, to: .string($0.rawValue), for: item.id) }
        )
    }

    private func binding(for spec: OptionSpec) -> Binding<OptionValue?> {
        Binding(
            get: { OptionsForm.value(of: spec, in: item.options) },
            set: { queue.setOption(spec.key, to: $0, for: item.id) }
        )
    }
}

/// One option control: switch, stepper, slider, menu or text field.
struct OptionFieldView: View {
    let field: OptionField
    @Binding var value: OptionValue?

    var body: some View {
        let spec = field.spec
        VStack(alignment: .leading, spacing: Space.s1) {
            control(spec)
            if !spec.description.isEmpty {
                Text(spec.description)
                    .textStyle(.caption)
                    .foregroundStyle(Palette.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .accessibilityIdentifier("option.\(spec.key)")
    }

    @ViewBuilder private func control(_ spec: OptionSpec) -> some View {
        switch field.control {
        case .toggle:
            Toggle(isOn: Binding(get: { value?.boolValue ?? false }, set: { value = .bool($0) })) {
                Text(spec.label).textStyle(.body).foregroundStyle(Palette.textPrimary)
            }
            .tint(Palette.accentFill)
            .frame(minHeight: Sizes.touchTarget)
        case .stepper(let lo, let hi):
            let current = value?.intValue ?? lo
            Stepper(value: Binding(get: { current }, set: { value = .int($0) }), in: lo...max(lo, hi)) {
                HStack {
                    Text(spec.label).textStyle(.body).foregroundStyle(Palette.textPrimary)
                    Spacer()
                    Text("\(current)").textStyle(.mono).foregroundStyle(Palette.textSecondary)
                }
            }
            .frame(minHeight: Sizes.touchTarget)
        case .slider(let lo, let hi, let step):
            let current = value?.doubleValue ?? lo
            VStack(alignment: .leading, spacing: Space.s1) {
                HStack {
                    Text(spec.label).textStyle(.body).foregroundStyle(Palette.textPrimary)
                    Spacer()
                    Text(spec.type == .int ? "\(Int(current))" : String(format: "%.2f", current))
                        .textStyle(.mono)
                        .foregroundStyle(Palette.textSecondary)
                }
                // Continuous slider (no tick marks); values are snapped to the step here.
                Slider(value: Binding(get: { current },
                                      set: { raw in
                                          let snapped = lo + (((raw - lo) / step).rounded() * step)
                                          value = spec.type == .int ? .int(Int(snapped.rounded())) : .float(OptionsForm.clamp(snapped, spec))
                                      }),
                       in: lo...max(lo + step, hi)) {
                    Text(spec.label)
                }
                .tint(Palette.accentFill)
                .accessibilityValue(spec.type == .int ? "\(Int(current))" : String(format: "%.2f", current))
            }
        case .menu(let choices):
            HStack {
                Text(spec.label).textStyle(.body).foregroundStyle(Palette.textPrimary)
                Spacer()
                Picker(spec.label, selection: Binding(get: { value?.stringValue ?? choices.first ?? "" },
                                                      set: { value = .string($0) })) {
                    ForEach(choices, id: \.self) { Text(OptionsForm.choiceLabel($0)).tag($0) }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
            .frame(minHeight: Sizes.touchTarget)
            .accessibilityElement(children: .contain)
        case .textField:
            VStack(alignment: .leading, spacing: Space.s1) {
                Text(spec.label).textStyle(.body).foregroundStyle(Palette.textPrimary)
                TextField(spec.label, text: Binding(get: { value?.stringValue ?? "" }, set: { value = .string($0) }))
                    .textStyle(.mono)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .padding(.horizontal, Space.s4)
                    .frame(minHeight: Sizes.touchTarget)
                    .background(RoundedRectangle(cornerRadius: Radius.sm).fill(Palette.surfaceSunken))
            }
        }
    }
}
