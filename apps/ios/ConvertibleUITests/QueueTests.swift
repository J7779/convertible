// Importing files into the queue: detection badges, groups of the same format,
// "set all files of this format", removing files.
import XCTest

final class QueueTests: E2ETestCase {
    private let samples = ["Meeting notes.txt", "Release notes.txt", "payload.b64"]

    private func launchSamples() {
        var config = LaunchConfig()
        config.sampleFiles = true
        launch(config)
        for name in samples { waitForPhase(name, in: ["ready"]) }
    }

    func testSampleFilesAreQueuedWithFormatBadges() {
        launchSamples()
        XCTAssertFalse(element("dropZone").exists, "the empty state gives way to the queue")
        XCTAssertTrue(app.buttons["addFilesButton"].exists)
        for name in samples {
            let card = reveal(name)
            let badge = card.descendants(matching: .any)["sourceBadge"]
            XCTAssertTrue(badge.exists, "\(name) shows its detected format")
            XCTAssertTrue(badge.label.hasPrefix("Detected format "), badge.label)
            let state = itemState(name)
            XCTAssertFalse(state.detected.isEmpty, "\(name) was detected")
            XCTAssertFalse(state.target.isEmpty, "\(name) has a default target")
            XCTAssertTrue(card.buttons["targetPickerButton"].label.hasPrefix("Convert to "))
        }
        XCTAssertEqual(itemState("Meeting notes.txt").detected, "txt")
        XCTAssertEqual(itemState("payload.b64").detected, "base64")
        XCTAssertTrue(reveal("Meeting notes.txt").descendants(matching: .any)["sourceBadge"].label.contains("Plain text"))
    }

    func testFilesGivenAtLaunchAreDetectedFromTheirContent() {
        let files = [
            TestFile("unknown.qqq", Data([0x00, 0xFF, 0x13, 0x37, 0x00, 0x01])),
            TestFile("hello.txt", text: "hello\n"),
        ]
        launch(with: files)
        XCTAssertEqual(itemState("hello.txt").detected, "txt")
        // Unrecognised bytes still offer the universal encodings.
        let unknown = itemState("unknown.qqq")
        XCTAssertTrue(unknown.targetIDs.contains("checksums"), "\(unknown.targetIDs)")
        XCTAssertTrue(unknown.targetIDs.contains("base64"), "\(unknown.targetIDs)")
    }

    func testSetAllFilesOfThisFormat() {
        launchSamples()
        let group = waitFor(element("groupBar.txt"))
        XCTAssertTrue(group.staticTexts["2 TXT files"].exists)
        let button = group.buttons["groupTargetButton"]
        button.tap()
        waitFor(app.navigationBars["Convert 2 TXT files to"])
        pick("hex")
        for name in ["Meeting notes.txt", "Release notes.txt"] {
            waitUntil(probe("uitest.item", in: name), "label CONTAINS '\"target\":\"hex\"'")
            XCTAssertEqual(reveal(name).buttons["targetPickerButton"].label, "Convert to Hexadecimal text")
        }
        XCTAssertNotEqual(itemState("payload.b64").target, "hex", "other formats are left alone")
        waitUntil(element("groupTargetButton"), "label CONTAINS 'Hexadecimal text'")

        // Converting the group: every TXT result decodes back to its sample text.
        tapConvertAll()
        let meeting = output(of: "Meeting notes.txt")
        XCTAssertEqual(Fixtures.parseHex(meeting.text).map { String(decoding: $0, as: UTF8.self) },
                       "Agenda\n1. Offline conversion\n2. Privacy first\n3. Ship it\n")
    }

    func testRemoveOneFileThenClearAll() {
        launchSamples()
        reveal("payload.b64").buttons["removeButton"].tap()
        waitForDisappearance(card("payload.b64"))
        XCTAssertTrue(card("Meeting notes.txt").exists)

        let more = waitFor(app.buttons["moreMenu"])
        more.tap()
        waitFor(app.buttons["Clear all files"]).tap()
        waitFor(element("dropZone"))
        XCTAssertFalse(card("Meeting notes.txt").exists)
        XCTAssertFalse(app.buttons["convertAllButton"].exists)
    }
}
