// iPhone and iPad layouts, light and dark, Dynamic Type, rotation.
import XCTest

final class LayoutTests: E2ETestCase {

    func testPhoneLayoutUsesOneStackWithToolbarNavigation() throws {
        guard !isPad else { throw XCTSkip("iPhone layout; this device is an iPad") }
        launch()
        XCTAssertFalse(element("sidebar.convert").exists, "no sidebar on iPhone")
        waitFor(app.buttons["formatsButton"]).tap()
        waitFor(app.navigationBars["Formats"])
        app.navigationBars.buttons.element(boundBy: 0).tap()
        waitFor(app.buttons["settingsButton"]).tap()
        waitFor(app.navigationBars["Settings"])
        app.navigationBars.buttons.element(boundBy: 0).tap()
        waitFor(app.buttons["chooseFilesButton"])
    }

    func testPadLayoutUsesSplitViewWithSidebar() throws {
        guard isPad else { throw XCTSkip("iPad split view; this device is an iPhone") }
        XCUIDevice.shared.orientation = .landscapeLeft
        var config = LaunchConfig()
        config.sampleFiles = true
        launch(config)
        let sidebar = waitFor(element("sidebar.convert"))
        let queue = waitFor(card("Meeting notes.txt"))
        XCTAssertLessThan(sidebar.frame.maxX, queue.frame.minX + 1, "sidebar and detail side by side")
        XCTAssertFalse(app.buttons["formatsButton"].exists, "sections live in the sidebar, not the toolbar")
        element("sidebar.formats").tap()
        waitFor(app.searchFields["Search formats"])
        XCTAssertTrue(element("sidebar.convert").isHittable, "the sidebar stays visible")
        element("sidebar.settings").tap()
        waitFor(element("themePicker"))
        element("sidebar.convert").tap()
        waitFor(card("Meeting notes.txt"))
        attachScreenshot("iPad split view")
    }

    func testLightAndDarkAppearanceFollowTheSystem() {
        var config = LaunchConfig()
        config.sampleFiles = true
        launch(config)
        waitForPhase("Meeting notes.txt", in: ["ready"])
        XCTAssertGreaterThan(Appearance.backgroundLuminance(of: app), 0.65, "light appearance")
        attachScreenshot("Light")
        XCUIDevice.shared.appearance = .dark
        let dark = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in Appearance.backgroundLuminance(of: self.app) < 0.35 },
                                             object: nil)
        XCTAssertEqual(XCTWaiter().wait(for: [dark], timeout: uiTimeout), .completed, "dark appearance")
        XCTAssertTrue(reveal("Meeting notes.txt").buttons["targetPickerButton"].isHittable)
        attachScreenshot("Dark")
    }

    func testAccessibilityTextSizeKeepsPrimaryControlsUsable() {
        var config = LaunchConfig()
        config.contentSize = "UICTContentSizeCategoryAccessibilityXL"
        launch(config)
        let window = app.windows.firstMatch.frame
        let choose = app.buttons["chooseFilesButton"]
        scrollIntoView(choose)
        assertUnclipped(choose, in: window)

        launch(with: [TestFile("big type.txt", text: "Large text\n")]) { $0.contentSize = "UICTContentSizeCategoryAccessibilityXL" }
        let convert = waitFor(app.buttons["convertAllButton"])
        assertUnclipped(convert, in: window)
        let picker = reveal("big type.txt").buttons["targetPickerButton"]
        assertUnclipped(picker, in: window)
        attachScreenshot("Accessibility XL")
        convert.tap()
        waitForPhase("big type.txt", in: ["finished"])
        let save = reveal("big type.txt").buttons["resultSaveButton"]
        scrollIntoView(save)
        assertUnclipped(save, in: window)
    }

    func testRotationKeepsTheQueueUsable() {
        var config = LaunchConfig()
        config.sampleFiles = true
        launch(config)
        waitForPhase("Meeting notes.txt", in: ["ready"])
        XCUIDevice.shared.orientation = .landscapeLeft
        let landscape = XCTNSPredicateExpectation(predicate: NSPredicate { _, _ in
            let f = self.app.windows.firstMatch.frame
            return f.width > f.height
        }, object: nil)
        XCTAssertEqual(XCTWaiter().wait(for: [landscape], timeout: uiTimeout), .completed)
        let convert = waitFor(app.buttons["convertAllButton"])
        XCTAssertTrue(convert.isHittable)
        attachScreenshot("Landscape")
        convert.tap()
        waitForPhase("Meeting notes.txt", in: ["finished"])
        XCUIDevice.shared.orientation = .portrait
        XCTAssertTrue(reveal("Meeting notes.txt").buttons["resultSaveButton"].isHittable)
    }

    // MARK: Helpers

    private func scrollIntoView(_ element: XCUIElement) {
        waitFor(element)
        var tries = 0
        while !element.isHittable && tries < 8 {
            app.scrollViews.firstMatch.swipeUp(velocity: .slow)
            tries += 1
        }
    }

    private func assertUnclipped(_ element: XCUIElement, in window: CGRect, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(element.isHittable, "\(element) can be tapped", file: file, line: line)
        let frame = element.frame
        XCTAssertGreaterThanOrEqual(frame.minX, window.minX - 0.5, "\(element.identifier) inside the window", file: file, line: line)
        XCTAssertLessThanOrEqual(frame.maxX, window.maxX + 0.5, "\(element.identifier) inside the window", file: file, line: line)
        XCTAssertGreaterThanOrEqual(frame.height, 44, "\(element.identifier) keeps a 44 pt touch target", file: file, line: line)
        XCTAssertFalse(element.label.isEmpty, file: file, line: line)
    }
}
