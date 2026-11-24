// Codable models for the JSON the core returns, plus status and option values.
import Foundation

/// One entry of `cvt_formats_json`.
struct FormatInfo: Codable, Hashable, Identifiable, Sendable {
    let id: String
    let name: String
    let description: String
    let category: String
    let extensions: [String]
    let mime: String
    let readable: Bool
    let writable: Bool
    let popularity: Int

    init(id: String, name: String, description: String = "", category: String, extensions: [String],
         mime: String = "", readable: Bool = true, writable: Bool = true, popularity: Int = 0) {
        self.id = id; self.name = name; self.description = description; self.category = category
        self.extensions = extensions; self.mime = mime; self.readable = readable; self.writable = writable
        self.popularity = popularity
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? id
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        category = try c.decodeIfPresent(String.self, forKey: .category) ?? "other"
        extensions = try c.decodeIfPresent([String].self, forKey: .extensions) ?? []
        mime = try c.decodeIfPresent(String.self, forKey: .mime) ?? ""
        readable = try c.decodeIfPresent(Bool.self, forKey: .readable) ?? false
        writable = try c.decodeIfPresent(Bool.self, forKey: .writable) ?? false
        popularity = try c.decodeIfPresent(Int.self, forKey: .popularity) ?? 0
    }
}

/// One entry of `cvt_targets_json`.
struct TargetFormat: Codable, Hashable, Identifiable, Sendable {
    let id: String
    let name: String
    let category: String
    let extensions: [String]
    let mime: String
    let lossy: Bool
    let compress: Bool

    init(id: String, name: String, category: String, extensions: [String] = [], mime: String = "",
         lossy: Bool = false, compress: Bool = false) {
        self.id = id; self.name = name; self.category = category; self.extensions = extensions
        self.mime = mime; self.lossy = lossy; self.compress = compress
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? id
        category = try c.decodeIfPresent(String.self, forKey: .category) ?? "other"
        extensions = try c.decodeIfPresent([String].self, forKey: .extensions) ?? []
        mime = try c.decodeIfPresent(String.self, forKey: .mime) ?? ""
        lossy = try c.decodeIfPresent(Bool.self, forKey: .lossy) ?? false
        compress = try c.decodeIfPresent(Bool.self, forKey: .compress) ?? false
    }

    /// Default file extension for output files.
    var defaultExtension: String { extensions.first ?? id }
}

/// A JSON scalar as found in option defaults and option values.
enum OptionValue: Codable, Hashable, Sendable {
    case int(Int)
    case float(Double)
    case bool(Bool)
    case string(String)

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let i = try? c.decode(Int.self) { self = .int(i) }
        else if let d = try? c.decode(Double.self) { self = .float(d) }
        else if let s = try? c.decode(String.self) { self = .string(s) }
        else { throw DecodingError.dataCorruptedError(in: c, debugDescription: "Unsupported option value") }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .int(let v): try c.encode(v)
        case .float(let v): try c.encode(v)
        case .bool(let v): try c.encode(v)
        case .string(let v): try c.encode(v)
        }
    }

    var intValue: Int? {
        switch self {
        case .int(let v): return v
        case .float(let v): return Int(v.rounded())
        default: return nil
        }
    }
    var doubleValue: Double? {
        switch self {
        case .int(let v): return Double(v)
        case .float(let v): return v
        default: return nil
        }
    }
    var boolValue: Bool? { if case .bool(let v) = self { return v } else { return nil } }
    var stringValue: String? {
        switch self {
        case .string(let v): return v
        case .int(let v): return String(v)
        case .float(let v): return String(v)
        case .bool(let v): return v ? "true" : "false"
        }
    }
}

/// One entry of `cvt_options_schema_json`.
struct OptionSpec: Codable, Hashable, Identifiable, Sendable {
    enum Kind: String, Codable, Sendable { case int, float, bool, `enum`, string }

    let key: String
    let label: String
    let type: Kind
    let defaultValue: OptionValue?
    let min: Double?
    let max: Double?
    let choices: [String]
    let description: String
    let category: String

    var id: String { key }
    var isAdvanced: Bool { category == "advanced" }

    enum CodingKeys: String, CodingKey {
        case key, label, type, min, max, choices, description, category
        case defaultValue = "default"
    }

    init(key: String, label: String, type: Kind, defaultValue: OptionValue?, min: Double? = nil, max: Double? = nil,
         choices: [String] = [], description: String = "", category: String = "basic") {
        self.key = key; self.label = label; self.type = type; self.defaultValue = defaultValue
        self.min = min; self.max = max; self.choices = choices; self.description = description; self.category = category
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        key = try c.decode(String.self, forKey: .key)
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? key
        type = (try? c.decode(Kind.self, forKey: .type)) ?? .string
        defaultValue = try? c.decodeIfPresent(OptionValue.self, forKey: .defaultValue)
        min = try? c.decodeIfPresent(Double.self, forKey: .min)
        max = try? c.decodeIfPresent(Double.self, forKey: .max)
        choices = (try? c.decodeIfPresent([String].self, forKey: .choices)) ?? []
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        category = try c.decodeIfPresent(String.self, forKey: .category) ?? "basic"
    }
}

/// `enum cvt_status` from the C interface.
enum CoreStatus: Int32, Sendable, CaseIterable {
    case ok = 0
    case invalidArgument = 1
    case unknownFormat = 2
    case unsupportedConversion = 3
    case decode = 4
    case encode = 5
    case io = 6
    case cancelled = 7
    case limit = 8
    case internalError = 9
}

/// A failed conversion: the status and the core's own description.
struct CoreError: Error, Equatable, Sendable, LocalizedError {
    let status: CoreStatus
    let message: String

    var errorDescription: String? { message.isEmpty ? "Conversion failed (\(status))." : message }
}

/// A progress report from the core's callback.
struct ConversionProgress: Equatable, Sendable {
    let fraction: Double
    let stage: String
}

/// Result of an in-memory conversion.
struct ConversionOutput: Sendable {
    let data: Data
    let format: String
    let fileExtension: String
    let mime: String
    let warnings: [String]
}

/// Result of a file conversion (the bytes are in the output file).
struct FileConversionOutput: Sendable {
    let url: URL
    let size: Int64
    let format: String
    let fileExtension: String
    let mime: String
    let warnings: [String]
}

/// Option values chosen for one conversion, serialised as a flat JSON object.
struct OptionValues: Equatable, Sendable {
    var values: [String: OptionValue] = [:]

    subscript(key: String) -> OptionValue? {
        get { values[key] }
        set { values[key] = newValue }
    }

    var isEmpty: Bool { values.isEmpty }

    /// Deterministic JSON (sorted keys), e.g. `{"line_width":0,"padding":false}`.
    func json() -> String {
        guard !values.isEmpty else { return "" }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard let data = try? encoder.encode(values), let text = String(data: data, encoding: .utf8) else { return "" }
        return text
    }
}
