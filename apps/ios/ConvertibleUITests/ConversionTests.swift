// Real conversions through the real core, started from the UI; the produced bytes are
// read back from the app and checked independently (Foundation, CryptoKit).
import CryptoKit
import XCTest

final class ConversionTests: E2ETestCase {

    func testBase64OutputDecodesToInput() {
        let input = Fixtures.bytes(count: 700)
        launch(with: [TestFile("blob.bin", input)])
        let result = convert("blob.bin", to: "base64")
        XCTAssertEqual(result.name, "blob.b64")
        let decoded = Data(base64Encoded: result.text, options: .ignoreUnknownCharacters)
        XCTAssertEqual(decoded, input, "Base64 output decodes to the exact input")
        XCTAssertEqual(result.size, result.data?.count)
    }

    func testHexOutputDecodesToInputAndUppercaseOptionChangesIt() {
        let input = Fixtures.bytes(count: 96, seed: 7)
        launch(with: [TestFile("sample.dat", input)])
        let lower = convert("sample.dat", to: "hex")
        XCTAssertEqual(Fixtures.parseHex(lower.text), input, "hex output decodes to the input")
        XCTAssertEqual(lower.text.trimmingCharacters(in: .whitespacesAndNewlines),
                       lower.text.lowercased().trimmingCharacters(in: .whitespacesAndNewlines))

        // The "uppercase" switch is rendered from the options schema; flipping it changes the bytes.
        reveal("sample.dat").buttons["convertAgainButton"].tap()
        waitForPhase("sample.dat", in: ["ready"])
        let toggle = waitFor(app.switches["option.uppercase"])
        toggle.switches.firstMatch.exists ? toggle.switches.firstMatch.tap() : toggle.tap()
        waitUntil(probe("uitest.item", in: "sample.dat"), "label CONTAINS '\"uppercase\":true'")
        reveal("sample.dat").buttons["convertButton"].tap()
        let upper = output(of: "sample.dat")
        XCTAssertEqual(Fixtures.parseHex(upper.text), input)
        XCTAssertTrue(upper.text.contains(Fixtures.hex(input.prefix(8), uppercase: true)), upper.text)
        XCTAssertNotEqual(upper.data, lower.data)
    }

    func testChecksumReportMatchesCryptoKit() {
        let input = Fixtures.bytes(count: 4096, seed: 99)
        launch(with: [TestFile("firmware.img", input)])
        let report = convert("firmware.img", to: "checksums").text.lowercased()
        XCTAssertTrue(report.contains(Fixtures.digestHex(SHA256.hash(data: input))), "SHA-256 matches CryptoKit")
        XCTAssertTrue(report.contains(Fixtures.digestHex(Insecure.SHA1.hash(data: input))), "SHA-1 matches CryptoKit")
        XCTAssertTrue(report.contains(Fixtures.digestHex(Insecure.MD5.hash(data: input))), "MD5 matches CryptoKit")
    }

    func testUTF16ReencodingWithByteOrderMark() {
        let text = "Grüße, 世界 — offline ✓\n"
        launch(with: [TestFile("greeting.txt", text: text)])
        chooseTarget("txt", for: "greeting.txt")
        // Encoding is a menu rendered from the schema; the byte order mark a switch.
        let encoding = waitFor(element("option.encoding"))
        encoding.buttons.firstMatch.tap()
        waitFor(app.buttons["utf-16le"]).tap()
        let bom = waitFor(app.switches["option.bom"])
        bom.switches.firstMatch.exists ? bom.switches.firstMatch.tap() : bom.tap()
        let probe = probe("uitest.item", in: "greeting.txt")
        waitUntil(probe, "label CONTAINS '\"encoding\":\"utf-16le\"' AND label CONTAINS '\"bom\":true'")
        reveal("greeting.txt").buttons["convertButton"].tap()
        let result = output(of: "greeting.txt")
        let bytes = result.data ?? Data()
        XCTAssertEqual(Array(bytes.prefix(2)), [0xFF, 0xFE], "UTF-16LE byte order mark")
        XCTAssertEqual(String(data: bytes.dropFirst(2), encoding: .utf16LittleEndian), text)
        XCTAssertEqual(bytes.count, 2 + text.utf16.count * 2)
    }

    func testLineWidthOptionChangesBase64Output() {
        let input = Fixtures.bytes(count: 300, seed: 3)
        launch(with: [TestFile("wrap.bin", input)])
        chooseTarget("base64", for: "wrap.bin")
        let wrapped = { () -> ResultOutput in
            self.reveal("wrap.bin").buttons["convertButton"].tap()
            return self.output(of: "wrap.bin")
        }()
        let lines = wrapped.text.split(separator: "\n")
        XCTAssertGreaterThan(lines.count, 1, "the default line width wraps 400 characters")

        reveal("wrap.bin").buttons["convertAgainButton"].tap()
        waitForPhase("wrap.bin", in: ["ready"])
        let slider = waitFor(app.sliders["option.line_width"])
        slider.adjust(toNormalizedSliderPosition: 0)
        waitUntil(probe("uitest.item", in: "wrap.bin"), "label CONTAINS '\"line_width\":0'")
        reveal("wrap.bin").buttons["convertButton"].tap()
        let single = output(of: "wrap.bin")
        XCTAssertEqual(single.text.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: "\n").count, 1)
        XCTAssertEqual(Data(base64Encoded: single.text.trimmingCharacters(in: .whitespacesAndNewlines)), input)
        XCTAssertNotEqual(single.data, wrapped.data)
    }

    func testSampleFilesConvertWithProgressAndSizes() {
        var config = LaunchConfig()
        config.sampleFiles = true
        launch(config)
        let names = ["Meeting notes.txt", "Release notes.txt", "payload.b64"]
        for name in names { waitForPhase(name, in: ["ready"]) }
        waitUntil(app.buttons["convertAllButton"], "label CONTAINS 'Convert 3 files'")
        tapConvertAll()
        for name in names { waitForPhase(name, in: ["finished"]) }
        for name in names {
            let sizes = reveal(name).descendants(matching: .any)["resultSizes"]
            waitFor(sizes)
            XCTAssertTrue(sizes.label.hasPrefix("Converted from "), sizes.label)
            XCTAssertTrue(sizes.label.contains(" to "), sizes.label)
            XCTAssertTrue(["smaller", "larger", "Same size", "New file"].contains { sizes.label.contains($0) }, sizes.label)
        }
        // The decoded payload is the sample's original text.
        if itemState("payload.b64").target == "bin" {
            XCTAssertEqual(output(of: "payload.b64").text, "Convertible runs entirely on your device.\n")
        }
        waitUntil(app.buttons["saveAllButton"], "label CONTAINS 'Save all 3 files'")
        attachScreenshot("Results")
    }

    func testProgressIsShownWhileConverting() {
        var config = LaunchConfig()
        config.largeSample = true
        launch(config)
        let name = "Camera dump.bin"
        waitForPhase(name, in: ["ready"])
        tapConvertAll()
        waitForPhase(name, in: ["queued", "converting"])
        // While busy: the card shows a cancel control and the bar the running count.
        let cancel = reveal(name).buttons["cancelButton"]
        if cancel.waitForExistence(timeout: 5) {
            XCTAssertTrue(app.buttons["cancelAllButton"].exists)
            XCTAssertTrue(app.staticTexts["Converting 1 file…"].exists)
        }
        waitForPhase(name, in: ["finished"], timeout: 180)
        let sizes = reveal(name).descendants(matching: .any)["resultSizes"]
        XCTAssertTrue(waitFor(sizes).label.contains("MB"), sizes.label)
        XCTAssertGreaterThan(output(of: name).size, 96 << 20, "the encoded result is larger than the 96 MiB input")
    }

    func testCompressActionWhenOffered() throws {
        launch(with: [TestFile("notes.txt", text: "Compress me\nplease\n")])
        let state = itemState("notes.txt")
        let compress = reveal("notes.txt").buttons["compressButton"]
        guard compress.exists else {
            XCTAssertNil(state.targets.first { $0.compress || $0.category == "archive" },
                         "a compress target exists, so the Compress action must be offered")
            throw XCTSkip("The catalogue offers no same-format or archive target for text")
        }
        compress.tap()
        let expected = state.targets.first { $0.compress }?.id ?? state.targets(in: "archive").first ?? ""
        waitUntil(probe("uitest.item", in: "notes.txt"), "label CONTAINS '\"target\":\"\(expected)\"'")
        XCTAssertFalse(compress.exists, "Compress hides once applied")
        reveal("notes.txt").buttons["convertButton"].tap()
        let result = output(of: "notes.txt")
        XCTAssertGreaterThan(result.size, 0)
    }

    func testSaveToFilesAndShareSheet() {
        launch(with: [TestFile("share.txt", text: "Share this\n")])
        _ = convert("share.txt", to: "base64")

        // Save: the system exporter appears; saving reports back with a toast.
        reveal("share.txt").buttons["resultSaveButton"].tap()
        let save = app.buttons["Save"]
        let move = app.buttons["Move"]
        XCTAssertTrue(save.waitForExistence(timeout: uiTimeout) || move.waitForExistence(timeout: 2), "the Files exporter appears")
        attachScreenshot("Save sheet")
        (save.exists ? save : move).tap()
        let saved = app.staticTexts["Saved to Files"]
        if !saved.waitForExistence(timeout: uiTimeout) {
            // Some simulator states ask where to save first; dismiss instead.
            let cancel = app.buttons["Cancel"]
            if cancel.exists { cancel.tap() }
        }

        // Share: the system share sheet appears and can be dismissed.
        reveal("share.txt").buttons["resultShareButton"].tap()
        let sheet = app.otherElements["ActivityListView"]
        let copy = app.buttons["Copy"].firstMatch
        let shown = sheet.waitForExistence(timeout: uiTimeout) || copy.waitForExistence(timeout: 2)
        XCTAssertTrue(shown, "the share sheet appears")
        attachScreenshot("Share sheet")
        let close = app.buttons["Close"].firstMatch
        if close.exists { close.tap() } else { app.swipeDown(velocity: .fast) }
    }
}
