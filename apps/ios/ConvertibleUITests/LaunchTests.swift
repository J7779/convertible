// Cold launch: the logo intro, skipping it, Reduce Motion, and launch time.
import XCTest

final class LaunchTests: E2ETestCase {

    private var home: XCUIElement { app.buttons["chooseFilesButton"] }

    func testColdLaunchPlaysIntroThenShowsUsableHome() {
        var config = LaunchConfig()
        config.skipIntro = false
        launch(config)
        let intro = element("introOverlay")
        // The intro covers the home screen while it plays (1.8 s), then goes away by itself.
        if intro.exists {
            XCTAssertFalse(home.isHittable, "the intro covers the home screen")
        }
        waitForDisappearance(intro, timeout: 8)
        XCTAssertTrue(waitFor(home).isHittable)
        XCTAssertTrue(element("privacyStatement").exists)
        XCTAssertTrue(element("dropZone").exists)
    }

    func testTappingTheIntroSkipsIt() {
        var config = LaunchConfig()
        config.skipIntro = false
        launch(config)
        let intro = element("introOverlay")
        if intro.exists && intro.isHittable {
            intro.tap()
        }
        waitForDisappearance(intro, timeout: 3)
        XCTAssertTrue(waitFor(home).isHittable)
    }

    func testReducedMotionIntroEndsQuickly() {
        var config = LaunchConfig()
        config.skipIntro = false
        config.reduceMotion = true
        launch(config)
        // With Reduce Motion the intro is a still mark shown for 0.6 s.
        waitForDisappearance(element("introOverlay"), timeout: 3)
        XCTAssertTrue(waitFor(home).isHittable)
    }

    func testLaunchPerformance() {
        // Measured without the probes or the intro: the time to a running, drawn app.
        app.launchArguments = ["-ConvertibleSkipIntro"]
        measure(metrics: [XCTApplicationLaunchMetric()]) {
            app.launch()
        }
    }
}
