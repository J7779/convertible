// The error path, and settings that persist across launches.
import XCTest

final class ErrorTests: E2ETestCase {

    func testUndecodableFileShowsFriendlyErrorAndRecovers() {
        // Named as hexadecimal text but not hex: decoding it to raw bytes must fail.
        let name = "broken.hex"
        launch(with: [TestFile(name, text: "@@@ this is not hexadecimal @@@\n")])
        XCTAssertEqual(itemState(name).detected, "hex")
        chooseTarget("bin", for: name)
        reveal(name).buttons["convertButton"].tap()
        waitForPhase(name, in: ["failed"])
        let banner = waitFor(reveal(name).descendants(matching: .any)["errorBanner"])
        XCTAssertTrue(banner.label.contains("This file couldn’t be read"), banner.label)
        XCTAssertFalse(banner.label.contains("cvt_"), "no internal names leak into the message")
        XCTAssertFalse(probe("uitest.output", in: name).exists)

        // The file can be retried with another target, which works.
        let retry = reveal(name).buttons["convertButton"]
        XCTAssertEqual(retry.label, "Try again")
        chooseTarget("base64", for: name)
        reveal(name).buttons["convertButton"].tap()
        let result = output(of: name)
        XCTAssertEqual(Data(base64Encoded: result.text, options: .ignoreUnknownCharacters),
                       Data("@@@ this is not hexadecimal @@@\n".utf8))
        XCTAssertFalse(element("errorBanner").exists)
    }
}

final class SettingsTests: E2ETestCase {

    /// Picks `choice` in a menu-style picker in the settings form.
    private func choose(_ choice: String, in identifier: String) {
        let picker = waitFor(element(identifier))
        (picker.buttons.firstMatch.exists ? picker.buttons.firstMatch : picker).tap()
        waitFor(app.buttons[choice].firstMatch).tap()
        waitUntil(element(identifier), "label CONTAINS %@ OR value CONTAINS %@".replacingOccurrences(of: "%@", with: "'\(choice)'"))
    }

    private func settingText(_ identifier: String) -> String {
        let e = waitFor(element(identifier))
        return "\(e.label) \(e.value as? String ?? "")"
    }

    func testSettingsPersistAcrossRelaunch() {
        launch()
        open("settings")
        XCTAssertTrue(settingText("themePicker").contains("System"))
        XCTAssertTrue(settingText("defaultPresetPicker").contains("Balanced"))
        let strip = waitFor(app.switches["stripMetadataToggle"])
        XCTAssertEqual(strip.value as? String, "1", "metadata is stripped by default")

        choose("Dark", in: "themePicker")
        choose("Small", in: "defaultPresetPicker")
        (strip.switches.firstMatch.exists ? strip.switches.firstMatch : strip).tap()
        waitUntil(strip, "value == '0'")

        // Relaunch without resetting: the three choices are still there.
        var config = LaunchConfig()
        config.resetSettings = false
        launch(config)
        open("settings")
        XCTAssertTrue(settingText("themePicker").contains("Dark"))
        XCTAssertTrue(settingText("defaultPresetPicker").contains("Small"))
        XCTAssertEqual(app.switches["stripMetadataToggle"].value as? String, "0")
        XCTAssertTrue(element("coreVersion").exists)

        // And a clean launch is back to the defaults.
        launch()
        open("settings")
        XCTAssertTrue(settingText("themePicker").contains("System"))
    }

    func testThemeChoiceOverridesSystemAppearance() {
        launch()
        open("settings")
        choose("Dark", in: "themePicker")
        XCTAssertLessThan(Appearance.backgroundLuminance(of: app), 0.35, "dark theme")
        attachScreenshot("Settings, dark theme")
        XCUIDevice.shared.appearance = .dark
        choose("Light", in: "themePicker")
        XCTAssertGreaterThan(Appearance.backgroundLuminance(of: app), 0.65, "light theme while the system is dark")
        attachScreenshot("Settings, light theme")
    }

    func testPrivacyAndLicencesScreens() {
        launch()
        open("settings")
        waitFor(element("privacyLink")).tap()
        waitFor(app.staticTexts["Everything happens on this device"])
        app.navigationBars.buttons.element(boundBy: 0).tap()
        waitFor(element("licensesLink")).tap()
        waitFor(element("license.convertible")).tap()
        waitFor(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'MIT'")).firstMatch)
    }
}
