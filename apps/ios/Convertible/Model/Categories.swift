// File categories of the core catalogue: display names, order and symbols.
import Foundation

enum FileCategory {
    /// Category ids in the order the core documents them.
    static let order = ["image", "audio", "video", "document", "ebook", "spreadsheet", "presentation", "data",
                        "archive", "font", "subtitle", "model3d", "geo", "pim", "code", "encoding", "vector", "other"]

    static func known(_ id: String) -> String { order.contains(id) ? id : "other" }

    static func title(_ id: String) -> String {
        switch id {
        case "image": return "Images"
        case "audio": return "Audio"
        case "video": return "Video"
        case "document": return "Documents"
        case "ebook": return "E-books"
        case "spreadsheet": return "Spreadsheets"
        case "presentation": return "Presentations"
        case "data": return "Data"
        case "archive": return "Archives"
        case "font": return "Fonts"
        case "subtitle": return "Subtitles"
        case "model3d": return "3D models"
        case "geo": return "Maps & GPS"
        case "pim": return "Calendars & contacts"
        case "code": return "Code"
        case "encoding": return "Encodings"
        case "vector": return "Vector graphics"
        default: return "Other"
        }
    }

    /// SF Symbol for a category.
    static func symbol(_ id: String) -> String {
        switch id {
        case "image": return "photo"
        case "audio": return "waveform"
        case "video": return "film"
        case "document": return "doc.text"
        case "ebook": return "book.closed"
        case "spreadsheet": return "tablecells"
        case "presentation": return "rectangle.on.rectangle"
        case "data": return "curlybraces"
        case "archive": return "archivebox"
        case "font": return "textformat"
        case "subtitle": return "captions.bubble"
        case "model3d": return "cube"
        case "geo": return "map"
        case "pim": return "person.crop.rectangle"
        case "code": return "chevron.left.forwardslash.chevron.right"
        case "encoding": return "number"
        case "vector": return "scribble.variable"
        default: return "doc"
        }
    }

    static func rank(_ id: String) -> Int { order.firstIndex(of: id) ?? order.count }
}
