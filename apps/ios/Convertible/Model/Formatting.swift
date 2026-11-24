// Sizes, savings and friendly error texts.
import Foundation

enum SizeText {
    static func string(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        formatter.allowedUnits = [.useBytes, .useKB, .useMB, .useGB, .useTB]
        formatter.isAdaptive = true
        formatter.allowsNonnumericFormatting = false
        return formatter.string(fromByteCount: max(0, bytes))
    }
}

/// Before → after comparison for a finished conversion.
struct SizeChange: Equatable {
    let before: Int64
    let after: Int64

    /// Fraction saved: positive when the output is smaller, negative when larger.
    var savedFraction: Double {
        guard before > 0 else { return 0 }
        return Double(before - after) / Double(before)
    }

    /// Whole percent, rounded half away from zero.
    var percent: Int { Int((abs(savedFraction) * 100).rounded()) }

    var summary: String {
        if before == after || (percent == 0 && before > 0) { return "Same size" }
        guard before > 0 else { return "New file" }
        return after < before ? "\(percent)% smaller" : "\(percent)% larger"
    }

    var isSmaller: Bool { after < before }
}

/// A conversion failure as shown to people.
struct FriendlyError: Equatable, Error {
    let title: String
    let message: String
    let status: CoreStatus?

    static func from(_ error: Error) -> FriendlyError {
        if let core = error as? CoreError {
            return FriendlyError(title: title(for: core.status),
                                 message: core.message.isEmpty ? fallbackMessage(for: core.status) : sentence(core.message),
                                 status: core.status)
        }
        if error is CancellationError {
            return FriendlyError(title: title(for: .cancelled), message: fallbackMessage(for: .cancelled), status: .cancelled)
        }
        let ns = error as NSError
        if ns.domain == NSCocoaErrorDomain {
            return FriendlyError(title: title(for: .io), message: ns.localizedDescription, status: .io)
        }
        return FriendlyError(title: title(for: .internalError), message: ns.localizedDescription, status: .internalError)
    }

    static func title(for status: CoreStatus) -> String {
        switch status {
        case .ok: return "Done"
        case .invalidArgument: return "Check the settings"
        case .unknownFormat: return "Unrecognised format"
        case .unsupportedConversion: return "Can’t convert to this format"
        case .decode: return "This file couldn’t be read"
        case .encode: return "Couldn’t write this format"
        case .io: return "File access problem"
        case .cancelled: return "Cancelled"
        case .limit: return "Too large to convert"
        case .internalError: return "Something went wrong"
        }
    }

    static func fallbackMessage(for status: CoreStatus) -> String {
        switch status {
        case .ok: return ""
        case .invalidArgument: return "One of the options has a value the converter does not accept."
        case .unknownFormat: return "Convertible doesn’t recognise this kind of file."
        case .unsupportedConversion: return "There is no way to turn this file into the chosen format. Try another one."
        case .decode: return "The file may be damaged or use features that aren’t supported."
        case .encode: return "The content can’t be represented in the chosen format."
        case .io: return "The file couldn’t be read or the result couldn’t be saved."
        case .cancelled: return "The conversion was stopped."
        case .limit: return "The file exceeds a safety limit for size or complexity on this device."
        case .internalError: return "An unexpected error occurred inside the converter."
        }
    }

    /// Capitalises the first letter and ends with a full stop.
    static func sentence(_ text: String) -> String {
        var t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let first = t.first else { return t }
        t = first.uppercased() + t.dropFirst()
        if let last = t.last, !".!?".contains(last) { t += "." }
        return t
    }
}

enum FileNaming {
    /// "report.final.txt" + "b64" → "report.final.b64"
    static func outputName(for inputName: String, extension ext: String) -> String {
        let base = (inputName as NSString).deletingPathExtension
        let stem = base.isEmpty ? (inputName.isEmpty ? "converted" : inputName) : base
        return ext.isEmpty ? stem : "\(stem).\(ext)"
    }
}
