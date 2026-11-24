// Turns an options schema into form controls, and resolves values.
import Foundation

enum Preset: String, CaseIterable, Identifiable, Codable {
    case high, balanced, small
    var id: String { rawValue }
    var title: String {
        switch self {
        case .high: return "High"
        case .balanced: return "Balanced"
        case .small: return "Small"
        }
    }
}

enum OptionControl: Equatable {
    case slider(min: Double, max: Double, step: Double)
    case stepper(min: Int, max: Int)
    /// A numeric field (typed, clamped): unbounded options and ranges too wide
    /// for a slider, such as a sample rate of 0...384000.
    case number(min: Double?, max: Double?, integer: Bool)
    case toggle
    case menu([String])
    case textField
}

struct OptionField: Identifiable, Equatable {
    let spec: OptionSpec
    let control: OptionControl
    var id: String { spec.key }
}

struct OptionsForm: Equatable {
    /// The "preset" option when the schema offers it (shown as High / Balanced / Small).
    let preset: OptionSpec?
    let basic: [OptionField]
    let advanced: [OptionField]

    static let presetKey = "preset"
    static let stripMetadataKey = "strip_metadata"

    init(schema: [OptionSpec]) {
        let presetSpec = schema.first { $0.key == Self.presetKey && $0.type == .enum }
        preset = presetSpec
        let fields = schema.filter { $0.key != presetSpec?.key }.map { OptionField(spec: $0, control: Self.control(for: $0)) }
        basic = fields.filter { !$0.spec.isAdvanced }
        advanced = fields.filter { $0.spec.isAdvanced }
    }

    var isEmpty: Bool { preset == nil && basic.isEmpty && advanced.isEmpty }

    /// Presets offered by the schema, in High / Balanced / Small order.
    var presetChoices: [Preset] {
        guard let preset else { return [] }
        return Preset.allCases.filter { preset.choices.contains($0.rawValue) }
    }

    static func control(for spec: OptionSpec) -> OptionControl {
        switch spec.type {
        case .bool:
            return .toggle
        case .enum:
            return .menu(spec.choices)
        case .string:
            return .textField
        case .int:
            guard let lo = spec.min, let hi = spec.max, hi > lo else {
                return .number(min: spec.min, max: spec.max, integer: true)
            }
            if hi - lo <= Double(stepperMaxSteps) { return .stepper(min: Int(lo), max: Int(hi)) }
            if hi - lo <= Double(sliderMaxSteps) { return .slider(min: lo, max: hi, step: 1) }
            return .number(min: lo, max: hi, integer: true)
        case .float:
            guard let lo = spec.min, let hi = spec.max, hi > lo else {
                return .number(min: spec.min, max: spec.max, integer: false)
            }
            return .slider(min: lo, max: hi, step: (hi - lo) / 100)
        }
    }

    /// Integer ranges up to this many steps use a stepper.
    static let stepperMaxSteps = 20
    /// Integer ranges up to this many steps use a slider; wider ones a number field
    /// (a slider over thousands of values cannot hit a particular one).
    static let sliderMaxSteps = 1000

    /// The effective value of an option: the chosen one or the schema default.
    static func value(of spec: OptionSpec, in values: OptionValues) -> OptionValue? {
        values[spec.key] ?? spec.defaultValue
    }

    /// Starting values for a conversion: the user's default preset and the
    /// strip-metadata preference where the schema has those options, plus any
    /// earlier choices whose keys the new schema still understands.
    static func initialValues(schema: [OptionSpec], defaultPreset: Preset, stripMetadata: Bool,
                              keeping previous: OptionValues = OptionValues()) -> OptionValues {
        var values = OptionValues()
        let keys = Set(schema.map(\.key))
        for (key, value) in previous.values where keys.contains(key) { values[key] = value }
        if let preset = schema.first(where: { $0.key == presetKey }), values[presetKey] == nil,
           preset.choices.contains(defaultPreset.rawValue) {
            values[presetKey] = .string(defaultPreset.rawValue)
        }
        if keys.contains(stripMetadataKey), values[stripMetadataKey] == nil {
            values[stripMetadataKey] = .bool(stripMetadata)
        }
        return values
    }

    /// Clamps a numeric value into the spec's bounds.
    static func clamp(_ value: Double, _ spec: OptionSpec) -> Double {
        var v = value
        if let lo = spec.min { v = max(v, lo) }
        if let hi = spec.max { v = min(v, hi) }
        return v
    }

    /// Human label for an enum choice ("utf-16le" stays, "keep" → "Keep").
    static func choiceLabel(_ choice: String) -> String {
        guard !choice.isEmpty, choice.allSatisfy({ $0.isLetter && $0.isLowercase }) else { return choice }
        return choice.prefix(1).uppercased() + choice.dropFirst()
    }
}
