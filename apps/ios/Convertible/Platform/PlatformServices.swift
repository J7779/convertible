// Small platform helpers: haptics, saving images to Photos, exporting files.
import Photos
import SwiftUI
import UIKit
import UniformTypeIdentifiers

enum Haptics {
    @MainActor static func success() { UINotificationFeedbackGenerator().notificationOccurred(.success) }
    @MainActor static func failure() { UINotificationFeedbackGenerator().notificationOccurred(.error) }
    @MainActor static func selection() { UISelectionFeedbackGenerator().selectionChanged() }
}

enum PhotoLibrary {
    /// Image formats the Photos library accepts.
    static func canSave(mime: String, url: URL) -> Bool {
        let type = UTType(mimeType: mime) ?? UTType(filenameExtension: url.pathExtension)
        guard let type else { return false }
        let accepted: [UTType] = [.jpeg, .png, .heic, .heif, .gif, .tiff, .webP, .bmp]
        return accepted.contains { type.conforms(to: $0) }
    }

    enum SaveError: LocalizedError {
        case denied
        var errorDescription: String? {
            "Convertible isn’t allowed to add photos. You can change this in Settings › Privacy › Photos."
        }
    }

    /// Asks for add-only access at this moment (never earlier) and saves the image.
    static func save(_ url: URL) async throws {
        let status = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard status == .authorized || status == .limited else { throw SaveError.denied }
        try await PHPhotoLibrary.shared().performChanges {
            PHAssetCreationRequest.forAsset().addResource(with: .photo, fileURL: url, options: nil)
        }
    }
}

/// A converted file handed to `fileExporter`. The bytes are read from disk lazily.
struct ExportFile: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }
    static var writableContentTypes: [UTType] { [.data, .item] }

    let url: URL

    init(url: URL) { self.url = url }

    init(configuration: ReadConfiguration) throws {
        throw CocoaError(.featureUnsupported)
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        let wrapper = try FileWrapper(url: url, options: [])
        wrapper.preferredFilename = url.lastPathComponent
        return wrapper
    }

    var contentType: UTType { UTType(filenameExtension: url.pathExtension) ?? .data }
}
