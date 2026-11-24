// Xcode's accessibility audit on the main screens. Launched without the debug probes,
// so the audit sees exactly what users get.
import XCTest

final class AccessibilityAuditTests: E2ETestCase {

    private func launchPlain(samples: Bool = false, screen: String? = nil) {
        var config = LaunchConfig()
        config.hooks = false
        config.sampleFiles = samples
        config.screen = screen
        launch(config)
    }

    /// Runs the audit; issues are reported with the offending element.
    private func audit(_ screen: String, file: StaticString = #filePath, line: UInt = #line) throws {
        attachScreenshot("Audited: \(screen)")
        try app.performAccessibilityAudit { issue in
            // Known system-component findings that the app cannot change are listed here,
            // each with the reason; everything else fails the test.
            return Self.isSystemComponentIssue(issue)
        }
    }

    static func isSystemComponentIssue(_ issue: XCUIAccessibilityAuditIssue) -> Bool {
        guard let element = issue.element else { return false }
        // The system search field and navigation-bar back buttons are drawn by UIKit.
        if element.elementType == .searchField { return true }
        return false
    }

    func testAuditHomeEmpty() throws {
        launchPlain()
        waitFor(app.buttons["chooseFilesButton"])
        try audit("Home")
    }

    func testAuditQueueAndResults() throws {
        launchPlain(samples: true)
        let convert = waitFor(app.buttons["convertAllButton"])
        try audit("Queue")
        convert.tap()
        waitFor(app.buttons["saveAllButton"], timeout: conversionTimeout)
        try audit("Results")
    }

    func testAuditTargetPicker() throws {
        launchPlain(samples: true, screen: "picker")
        waitFor(app.searchFields.firstMatch)
        try audit("Target picker")
    }

    func testAuditFormatsBrowser() throws {
        launchPlain(screen: "formats")
        waitFor(app.searchFields["Search formats"])
        try audit("Formats")
    }

    func testAuditSettings() throws {
        launchPlain(screen: "settings")
        waitFor(element("themePicker"))
        try audit("Settings")
    }
}
