// Long conversions: cancel one, cancel all, convert again, and survive a trip to the
// background. Uses the app's large-sample hook (patterned bytes, made at launch).
import XCTest

final class CancellationTests: E2ETestCase {
    private let large = "Camera dump.bin"

    /// The large sample, at a size that keeps a conversion busy for a few seconds.
    private func launchLarge(mib: Int = 512, extra: [TestFile] = []) {
        var config = LaunchConfig()
        config.largeSample = true
        config.largeSampleMiB = mib
        config.files = extra
        launch(config)
        waitForPhase(large, in: ["ready"], timeout: 60)
        for file in extra { waitForPhase(file.name, in: ["ready"]) }
    }

    func testCancelDuringLongConversionThenConvertAgain() {
        launchLarge()
        chooseTarget("hex", for: large)
        reveal(large).buttons["convertButton"].tap()
        let cancel = reveal(large).buttons["cancelButton"]
        waitFor(cancel)
        waitForPhase(large, in: ["converting"])
        cancel.tap()
        waitForPhase(large, in: ["cancelled"])
        XCTAssertTrue(reveal(large).staticTexts["Cancelled"].exists)
        XCTAssertFalse(probe("uitest.output", in: large).exists, "no result after cancelling")

        // Converting again after a cancel runs to completion. A cheap target keeps it short.
        chooseTarget("checksums", for: large)
        let again = reveal(large).buttons["convertButton"]
        XCTAssertEqual(again.label, "Try again")
        again.tap()
        let result = output(of: large, timeout: 180)
        XCTAssertTrue(result.text.lowercased().contains("sha-256") || result.size > 0)
    }

    func testCancelAllStopsEveryRunningConversionThenConvertAgain() {
        launchLarge(extra: [TestFile("Second dump.bin", Fixtures.bytes(count: 1024))])
        chooseTarget("hex", for: large)
        tapConvertAll()
        let cancelAll = waitFor(app.buttons["cancelAllButton"])
        waitForPhase(large, in: ["converting"])
        cancelAll.tap()
        waitForPhase(large, in: ["cancelled"])
        waitForDisappearance(app.buttons["cancelAllButton"])

        // The bottom bar offers to convert what is left; this time it runs to the end.
        chooseTarget("checksums", for: large)
        tapConvertAll()
        waitForPhase(large, in: ["finished"], timeout: 180)
        XCTAssertEqual(itemState("Second dump.bin").phase, "finished")
    }

    func testBackgroundAndForegroundDuringConversion() {
        launchLarge(mib: 256)
        chooseTarget("base64", for: large)
        reveal(large).buttons["convertButton"].tap()
        waitForPhase(large, in: ["converting"])

        XCUIDevice.shared.press(.home)
        let background = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "state == %d OR state == %d",
                                   XCUIApplication.State.runningBackground.rawValue,
                                   XCUIApplication.State.runningBackgroundSuspended.rawValue),
            object: app)
        XCTAssertEqual(XCTWaiter().wait(for: [background], timeout: uiTimeout), .completed)

        app.activate()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: uiTimeout))
        // The conversion either finished in the background or resumes and finishes now.
        let result = output(of: large, timeout: 240)
        XCTAssertGreaterThan(result.size, 256 << 20)
        XCTAssertTrue(reveal(large).buttons["resultSaveButton"].exists)
    }
}
