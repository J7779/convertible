// Importing through the system document picker. e2e.sh puts a file into the
// simulator's "On My iPhone/iPad" storage and tells the test its name and content
// (TEST_RUNNER_E2E_SEEDED_FILE / TEST_RUNNER_E2E_SEEDED_TEXT); without them the test skips.
import XCTest

final class DocumentPickerTests: E2ETestCase {

    func testChooseFilesThroughTheSystemPicker() throws {
        let environment = ProcessInfo.processInfo.environment
        guard let name = environment["E2E_SEEDED_FILE"], let text = environment["E2E_SEEDED_TEXT"] else {
            throw XCTSkip("No file was seeded into the simulator's Files storage (run through apps/ios/e2e.sh)")
        }
        launch()
        waitFor(app.buttons["chooseFilesButton"]).tap()

        // The picker opens on Recents or Browse; go to the device's local storage.
        let browse = app.buttons["Browse"].firstMatch
        if browse.waitForExistence(timeout: uiTimeout) { browse.tap() }
        let local = app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH 'On My'")).firstMatch
        if local.waitForExistence(timeout: uiTimeout) && local.isHittable { local.tap() }

        let stem = (name as NSString).deletingPathExtension
        let entry = app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", stem)).firstMatch
        if !entry.waitForExistence(timeout: uiTimeout) {
            attachScreenshot("Picker without the seeded file")
            let cancel = app.buttons["Cancel"].firstMatch
            if cancel.exists { cancel.tap() }
            throw XCTSkip("The seeded file is not visible in the picker on this simulator")
        }
        entry.tap()
        let open = app.buttons["Open"].firstMatch
        if open.waitForExistence(timeout: 3) && open.isEnabled { open.tap() }

        waitForPhase(name, in: ["ready"])
        XCTAssertEqual(itemState(name).detected, "txt")
        let result = convert(name, to: "base64")
        XCTAssertEqual(Data(base64Encoded: result.text, options: .ignoreUnknownCharacters), Data(text.utf8),
                       "the picked file was read in place through its security-scoped URL")
    }
}
