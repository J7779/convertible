// Shared base for the end-to-end suite: clean launches, waits, screenshots on failure
// and access to the debug probes the app exposes under `-ConvertibleUITestHooks`.
import XCTest

/// A file handed to the app at launch (through `CONVERTIBLE_UITEST_FILES`).
struct TestFile {
    let name: String
    let data: Data

    init(_ name: String, _ data: Data) {
        self.name = name
        self.data = data
    }

    init(_ name: String, text: String) {
        self.init(name, Data(text.utf8))
    }
}

/// How to start the app for one test.
struct LaunchConfig {
    var skipIntro = true
    var hooks = true
    var resetSettings = true
    var sampleFiles = false
    var largeSample = false
    /// Size of the large sample in MiB (the app's default is 96).
    var largeSampleMiB: Int?
    var autoConvert = false
    var screen: String?
    var reduceMotion = false
    /// A UIContentSizeCategory raw value, e.g. `UICTContentSizeCategoryAccessibilityXL`.
    var contentSize: String?
    var files: [TestFile] = []
}

class E2ETestCase: XCTestCase {
    var app: XCUIApplication!

    /// Generous default for anything that involves a conversion; the waits return as
    /// soon as the condition holds.
    let conversionTimeout: TimeInterval = 60
    let uiTimeout: TimeInterval = 10

    var isPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    override func setUpWithError() throws {
        try super.setUpWithError()
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        XCUIDevice.shared.appearance = .light
        app = XCUIApplication()
    }

    override func tearDownWithError() throws {
        if let app, app.state != .notRunning { app.terminate() }
        XCUIDevice.shared.orientation = .portrait
        XCUIDevice.shared.appearance = .light
        try super.tearDownWithError()
    }

    /// Every failure carries a screenshot and the element tree at that moment.
    override func record(_ issue: XCTIssue) {
        var issue = issue
        let screenshot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        screenshot.name = "Screen at failure"
        screenshot.lifetime = .keepAlways
        issue.add(screenshot)
        if let app, app.state == .runningForeground {
            let tree = XCTAttachment(string: app.debugDescription)
            tree.name = "Element tree at failure"
            tree.lifetime = .keepAlways
            issue.add(tree)
        }
        super.record(issue)
    }

    // MARK: Launching

    @discardableResult
    func launch(_ config: LaunchConfig = LaunchConfig()) -> XCUIApplication {
        if app.state != .notRunning { app.terminate() }
        var arguments: [String] = []
        if config.hooks { arguments.append("-ConvertibleUITestHooks") }
        if config.hooks && config.resetSettings { arguments.append("-ConvertibleUITestResetSettings") }
        if config.hooks && config.reduceMotion { arguments.append("-ConvertibleUITestReduceMotion") }
        if config.skipIntro { arguments.append("-ConvertibleSkipIntro") }
        if config.sampleFiles { arguments.append("-ConvertibleSampleFiles") }
        if config.largeSample { arguments.append("-ConvertibleLargeSample") }
        if config.autoConvert { arguments.append("-ConvertibleAutoConvert") }
        if let screen = config.screen { arguments += ["-ConvertibleScreen", screen] }
        if let size = config.contentSize { arguments += ["-UIPreferredContentSizeCategoryName", size] }
        app.launchArguments = arguments

        var environment: [String: String] = [:]
        if !config.files.isEmpty {
            let entries = config.files.map { ["name": $0.name, "base64": $0.data.base64EncodedString()] }
            let json = try! JSONSerialization.data(withJSONObject: entries)
            environment["CONVERTIBLE_UITEST_FILES"] = String(decoding: json, as: UTF8.self)
        }
        if let mib = config.largeSampleMiB { environment["CONVERTIBLE_UITEST_LARGE_MIB"] = String(mib) }
        app.launchEnvironment = environment
        app.launch()
        return app
    }

    /// Launches with the given files queued and waits until each one is detected.
    @discardableResult
    func launch(with files: [TestFile], configure: (inout LaunchConfig) -> Void = { _ in }) -> XCUIApplication {
        var config = LaunchConfig()
        config.files = files
        configure(&config)
        launch(config)
        for file in files { waitForPhase(file.name, in: ["ready"]) }
        return app
    }

    // MARK: Waiting

    @discardableResult
    func waitFor(_ element: XCUIElement, timeout: TimeInterval? = nil, _ message: @autoclosure () -> String = "",
                 file: StaticString = #filePath, line: UInt = #line) -> XCUIElement {
        let found = element.waitForExistence(timeout: timeout ?? uiTimeout)
        XCTAssertTrue(found, "Timed out waiting for \(element). \(message())", file: file, line: line)
        return element
    }

    func waitUntil(_ element: XCUIElement, _ format: String, timeout: TimeInterval? = nil, _ message: @autoclosure () -> String = "",
                   file: StaticString = #filePath, line: UInt = #line) {
        let expectation = XCTNSPredicateExpectation(predicate: NSPredicate(format: format), object: element)
        let result = XCTWaiter().wait(for: [expectation], timeout: timeout ?? uiTimeout)
        XCTAssertEqual(result, .completed, "Timed out waiting for \(format) on \(element). \(message())", file: file, line: line)
    }

    func waitForDisappearance(_ element: XCUIElement, timeout: TimeInterval? = nil,
                              file: StaticString = #filePath, line: UInt = #line) {
        waitUntil(element, "exists == false", timeout: timeout, file: file, line: line)
    }

    // MARK: Queue cards and probes

    func element(_ identifier: String) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    func card(_ name: String) -> XCUIElement { element("queueCard.\(name)") }

    func probe(_ identifier: String, in name: String) -> XCUIElement {
        card(name).descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    /// Scrolls the main list until the card for `name` is on screen.
    @discardableResult
    func reveal(_ name: String, file: StaticString = #filePath, line: UInt = #line) -> XCUIElement {
        let target = card(name)
        let scroll = app.scrollViews.firstMatch
        for direction in [true, false] {
            for _ in 0..<8 {
                if target.exists && target.isHittable { return target }
                if direction { scroll.swipeUp(velocity: .slow) } else { scroll.swipeDown(velocity: .slow) }
            }
        }
        XCTAssertTrue(target.exists, "card \(name) not found", file: file, line: line)
        return target
    }

    func itemState(_ name: String, file: StaticString = #filePath, line: UInt = #line) -> ItemState {
        let element = waitFor(probe("uitest.item", in: name), file: file, line: line)
        guard let state = ItemState(label: element.label) else {
            XCTFail("unreadable item probe: \(element.label)", file: file, line: line)
            return ItemState.empty
        }
        return state
    }

    func waitForPhase(_ name: String, in phases: Set<String>, timeout: TimeInterval? = nil,
                      file: StaticString = #filePath, line: UInt = #line) {
        let probe = probe("uitest.item", in: name)
        let alternatives = phases.map { "label CONTAINS '\"phase\":\"\($0)\"'" }.joined(separator: " OR ")
        waitUntil(probe, "exists == true AND (\(alternatives))", timeout: timeout ?? conversionTimeout,
                  "waiting for \(name) to be \(phases.sorted())", file: file, line: line)
    }

    /// Waits for the file to finish converting and returns the result's bytes, read back
    /// from the app through the `uitest.output` probe.
    func output(of name: String, timeout: TimeInterval? = nil, file: StaticString = #filePath, line: UInt = #line) -> ResultOutput {
        waitForPhase(name, in: ["finished", "failed", "cancelled"], timeout: timeout, file: file, line: line)
        let state = itemState(name, file: file, line: line)
        XCTAssertEqual(state.phase, "finished", "conversion of \(name) did not finish", file: file, line: line)
        let element = waitFor(probe("uitest.output", in: name), file: file, line: line)
        guard let result = ResultOutput(label: element.label) else {
            XCTFail("unreadable output probe: \(element.label.prefix(200))", file: file, line: line)
            return ResultOutput(name: "", size: 0, data: nil)
        }
        return result
    }

    // MARK: Actions

    /// Opens the card's target picker, searches for `id` and picks it.
    func chooseTarget(_ id: String, for name: String, file: StaticString = #filePath, line: UInt = #line) {
        let card = reveal(name, file: file, line: line)
        card.buttons["targetPickerButton"].tap()
        pick(id, file: file, line: line)
        waitUntil(probe("uitest.item", in: name), "label CONTAINS '\"target\":\"\(id)\"'", file: file, line: line)
    }

    /// In an open target picker: search for `id` and pick it.
    func pick(_ id: String, file: StaticString = #filePath, line: UInt = #line) {
        let search = waitFor(app.searchFields.firstMatch, file: file, line: line)
        search.tap()
        search.typeText(id)
        let row = waitFor(element("target.\(id)"), file: file, line: line)
        row.tap()
        waitForDisappearance(element("pickerDoneButton"), file: file, line: line)
    }

    /// Starts the one ready file (bottom bar) or all of them.
    func tapConvertAll(file: StaticString = #filePath, line: UInt = #line) {
        let button = waitFor(app.buttons["convertAllButton"], file: file, line: line)
        button.tap()
    }

    /// Converts a single file to `target` and returns the result.
    func convert(_ name: String, to target: String, file: StaticString = #filePath, line: UInt = #line) -> ResultOutput {
        chooseTarget(target, for: name, file: file, line: line)
        reveal(name, file: file, line: line).buttons["convertButton"].tap()
        return output(of: name, file: file, line: line)
    }

    // MARK: Navigation (iPhone toolbar or iPad sidebar)

    func open(_ section: String, file: StaticString = #filePath, line: UInt = #line) {
        if isPad {
            waitFor(element("sidebar.\(section)"), file: file, line: line).tap()
        } else if section == "convert" {
            let back = app.navigationBars.buttons.element(boundBy: 0)
            if back.exists { back.tap() }
        } else {
            waitFor(app.buttons["\(section)Button"], file: file, line: line).tap()
        }
    }

    // MARK: Screenshots

    func attachScreenshot(_ name: String) {
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}

/// The JSON the app puts on each card's `uitest.item` probe.
struct ItemState {
    let name: String
    let detected: String
    let target: String
    let phase: String
    let options: [String: Any]
    let targets: [(id: String, category: String, compress: Bool)]

    static let empty = ItemState(name: "", detected: "", target: "", phase: "", options: [:], targets: [])

    init(name: String, detected: String, target: String, phase: String, options: [String: Any],
         targets: [(id: String, category: String, compress: Bool)]) {
        self.name = name; self.detected = detected; self.target = target; self.phase = phase
        self.options = options; self.targets = targets
    }

    init?(label: String) {
        guard let object = try? JSONSerialization.jsonObject(with: Data(label.utf8)) as? [String: Any] else { return nil }
        name = object["name"] as? String ?? ""
        detected = object["detected"] as? String ?? ""
        target = object["target"] as? String ?? ""
        phase = object["phase"] as? String ?? ""
        options = object["options"] as? [String: Any] ?? [:]
        targets = (object["targets"] as? [[String: Any]] ?? []).map {
            ($0["id"] as? String ?? "", $0["category"] as? String ?? "", $0["compress"] as? Bool ?? false)
        }
    }

    var targetIDs: [String] { targets.map(\.id) }
    func targets(in category: String) -> [String] { targets.filter { $0.category == category && !$0.compress }.map(\.id) }
}

/// A finished result as exposed by the `uitest.output` probe.
struct ResultOutput {
    let name: String
    let size: Int
    /// The exact bytes (nil when the result is larger than the probe's limit).
    let data: Data?

    init(name: String, size: Int, data: Data?) {
        self.name = name; self.size = size; self.data = data
    }

    init?(label: String) {
        var fields: [String: String] = [:]
        for part in label.split(separator: "|", maxSplits: 2) {
            guard let colon = part.firstIndex(of: ":") else { return nil }
            fields[String(part[..<colon])] = String(part[part.index(after: colon)...])
        }
        guard let size = fields["size"].flatMap(Int.init) else { return nil }
        self.name = fields["name"] ?? ""
        self.size = size
        self.data = fields["base64"].flatMap { Data(base64Encoded: $0) }
        if fields["base64"] != nil && data == nil { return nil }
    }

    var text: String { data.map { String(decoding: $0, as: UTF8.self) } ?? "" }
}
