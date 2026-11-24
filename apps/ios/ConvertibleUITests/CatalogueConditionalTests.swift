// Tests that run only when the core's catalogue can convert images, audio or archives.
// Today's core may only recognise these formats; each test asks the app what it offers
// for a real sample and skips with the reason when there is nothing to convert to.
import XCTest

final class CatalogueConditionalTests: E2ETestCase {

    /// Queues `file`, returns the first non-encoding target in `category` or skips.
    private func target(for file: TestFile, in category: String, what: String) throws -> String {
        launch(with: [file])
        let state = itemState(file.name)
        guard let id = state.targets(in: category).first else {
            throw XCTSkip("The catalogue offers no \(what) target for \(file.name) (detected as '\(state.detected)'); "
                          + "this test runs once the \(what) converters are in the core")
        }
        return id
    }

    func testImageConversionWhenImageFormatsArePresent() throws {
        let png = Fixtures.png(width: 24, height: 16)
        let file = TestFile("swatch.png", png)
        let id = try target(for: file, in: "image", what: "image")
        let result = convert(file.name, to: id)
        let bytes = try XCTUnwrap(result.data, "small images are exposed byte for byte")
        XCTAssertGreaterThan(bytes.count, 0)
        XCTAssertNotEqual(bytes, png)
        // Formats UIKit can decode are checked for their pixel size too.
        if let image = UIImage(data: bytes) {
            XCTAssertEqual(image.size.width * image.scale, 24)
            XCTAssertEqual(image.size.height * image.scale, 16)
        }
        attachScreenshot("Image result")
    }

    func testAudioConversionWhenAudioFormatsArePresent() throws {
        let file = TestFile("tone.wav", Fixtures.wav())
        let id = try target(for: file, in: "audio", what: "audio")
        let result = convert(file.name, to: id)
        XCTAssertGreaterThan(result.size, 0)
        if id == "flac", let bytes = result.data { XCTAssertEqual(Array(bytes.prefix(4)), Array("fLaC".utf8)) }
        if id == "wav", let bytes = result.data { XCTAssertEqual(Array(bytes.prefix(4)), Array("RIFF".utf8)) }
    }

    func testArchiveConversionWhenArchiveFormatsArePresent() throws {
        let text = String(repeating: "Convertible keeps files on the device. ", count: 40)
        let file = TestFile("pack me.txt", text: text)
        let id = try target(for: file, in: "archive", what: "archive")
        let result = convert(file.name, to: id)
        let bytes = try XCTUnwrap(result.data)
        if id == "zip" {
            let entry = try XCTUnwrap(Fixtures.firstZipEntry(bytes), "a readable ZIP entry")
            XCTAssertEqual(entry.data, Data(text.utf8), "the archived file is the input")
            XCTAssertTrue(entry.name.hasSuffix(".txt"), entry.name)
        } else {
            XCTAssertGreaterThan(bytes.count, 0)
        }
    }
}
