// The target picker (grouping, search, selection) and the formats browser (search).
// Both are checked against what the app reports it offers, never a fixed list.
import XCTest

final class PickerAndBrowserTests: E2ETestCase {

    func testTargetPickerGroupsSearchesAndSelects() {
        launch(with: [TestFile("notes.txt", text: "Some notes\n")])
        let state = itemState("notes.txt")
        reveal("notes.txt").buttons["targetPickerButton"].tap()
        waitFor(app.searchFields.firstMatch)
        waitFor(app.navigationBars["Convert to"])

        // Grouped by category; the source's own category (Documents for text) leads.
        let categories = Set(state.targets.map(\.category))
        XCTAssertTrue(categories.contains("encoding"), "\(categories)")
        let documents = app.staticTexts["DOCUMENTS"].exists ? app.staticTexts["DOCUMENTS"] : app.staticTexts["Documents"]
        let encodings = app.staticTexts["ENCODINGS"].exists ? app.staticTexts["ENCODINGS"] : app.staticTexts["Encodings"]
        if categories.contains("document") {
            XCTAssertTrue(documents.exists, "a section for the source's category")
            XCTAssertTrue(encodings.exists, "a section for encodings")
            XCTAssertLessThan(documents.frame.minY, encodings.frame.minY, "the source's category comes first")
        }

        // The current target is marked as selected.
        let current = element("target.\(state.target)")
        if current.exists { XCTAssertTrue(current.isSelected, "the chosen format is marked") }

        // Search narrows to ranked results; no match says so.
        let search = app.searchFields.firstMatch
        search.tap()
        search.typeText("base6")
        waitFor(element("target.base64"))
        XCTAssertFalse(element("target.hex").exists, "non-matching formats are hidden")
        search.typeText("zzzz")
        waitFor(app.staticTexts["No format matches “base6zzzz”."])
        let clear = search.buttons.firstMatch
        if clear.exists { clear.tap() } else { search.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 9)) }
        search.typeText("base64")
        element("target.base64").tap()
        waitForDisappearance(app.navigationBars["Convert to"])
        waitUntil(probe("uitest.item", in: "notes.txt"), "label CONTAINS '\"target\":\"base64\"'")
        XCTAssertEqual(reveal("notes.txt").buttons["targetPickerButton"].label, "Convert to Base64 text")
    }

    func testPickerLaunchScreenOpensForFirstFile() {
        var config = LaunchConfig()
        config.sampleFiles = true
        config.screen = "picker"
        launch(config)
        waitFor(app.searchFields.firstMatch)
        waitFor(element("pickerDoneButton")).tap()
        waitForDisappearance(element("pickerDoneButton"))
        XCTAssertTrue(card("Meeting notes.txt").exists)
    }

    func testFormatsBrowserSearch() {
        launch()
        open("formats")
        let search = waitFor(app.searchFields["Search formats"])
        let count = app.staticTexts.containing(NSPredicate(format: "label ENDSWITH ' formats'")).firstMatch
        waitFor(count)
        let total = Int(count.label.split(separator: " ").first ?? "") ?? 0
        XCTAssertGreaterThan(total, 10, count.label)

        search.tap()
        search.typeText("base64")
        let row = waitFor(app.staticTexts["Base64 text"].exists ? app.staticTexts["Base64 text"] : app.cells.staticTexts["Base64 text"])
        XCTAssertTrue(row.exists)
        let described = app.descendants(matching: .any).matching(NSPredicate(format: "label == 'Base64 text'")).firstMatch
        XCTAssertTrue(["Reads and writes", "Reads only", "Writes only"].contains(described.value as? String ?? ""),
                      "capabilities are announced: \(String(describing: described.value))")
        XCTAssertFalse(app.descendants(matching: .any).matching(NSPredicate(format: "label == 'Plain text'")).firstMatch.exists)

        search.typeText("zzzz")
        waitFor(app.staticTexts["No format matches “base64zzzz”."])
    }
}
