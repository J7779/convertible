// View-model and state tests: queue logic, options from schema, target
// ordering/search, sizes and errors. Written against the dynamic catalogue.
import XCTest
@testable import Convertible

@MainActor
final class QueueModelTests: XCTestCase {
    private var store: FileStore!
    private var model: QueueModel!

    override func setUp() async throws {
        store = FileStore(root: FileManager.default.temporaryDirectory.appendingPathComponent("QueueTests-\(UUID().uuidString)"))
        model = QueueModel(catalog: .load(), store: store, maxConcurrent: 2)
    }

    override func tearDown() async throws {
        model.removeAll()
        try? FileManager.default.removeItem(at: store.root)
    }

    private func file(_ name: String, _ text: String) throws -> URL {
        try store.importData(Data(text.utf8), named: name)
    }

    func testAddingDetectsFormatAndPicksDefaultTarget() async throws {
        let ids = await model.add(urls: [try file("notes.txt", "hello\n")], owned: true)
        let item = try XCTUnwrap(model.item(ids[0]))
        XCTAssertEqual(item.phase, .ready)
        XCTAssertEqual(item.detectedID, "txt")
        XCTAssertEqual(item.source?.id, "txt")
        XCTAssertEqual(item.size, 6)
        XCTAssertEqual(item.sourceLabel, "TXT")
        XCTAssertFalse(item.targets.isEmpty)
        XCTAssertEqual(item.targetID, TargetCatalog.defaultTarget(item.targets)?.id)
        XCTAssertFalse(item.target?.compress ?? true, "the default is a real conversion, not a re-encode")
        XCTAssertFalse(item.schema.isEmpty)
    }

    func testUnknownBinaryFallsBackToUniversalTargets() async throws {
        let url = try store.importData(Data([0x00, 0xFF, 0x13, 0x37, 0x00, 0x01]), named: "blob.qqq")
        let ids = await model.add(urls: [url], owned: true)
        let item = try XCTUnwrap(model.item(ids[0]))
        XCTAssertEqual(item.coreSourceID, item.detectedID ?? QueueModel.unknownSourceID)
        XCTAssertTrue(item.targets.contains { $0.id == "checksums" })
        model.setTarget("hex", for: item.id)
        await model.convert(item.id)?.value
        let summary = try XCTUnwrap(model.item(item.id)?.phase.summary)
        XCTAssertEqual(try String(contentsOf: summary.outputURL, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines),
                       "00ff13370001")
    }

    func testConvertProducesResultFileAndSummary() async throws {
        var completions: [Bool] = []
        model.onItemCompleted = { completions.append($0) }
        let ids = await model.add(urls: [try file("abc.txt", "abc")], owned: true)
        model.setTarget("hex", for: ids[0])
        await model.convert(ids[0])?.value
        let item = try XCTUnwrap(model.item(ids[0]))
        let summary = try XCTUnwrap(item.phase.summary)
        XCTAssertEqual(summary.fileName, "abc.hex")
        XCTAssertEqual(summary.format, "hex")
        XCTAssertEqual(summary.inputSize, 3)
        XCTAssertEqual(summary.outputSize, Int64(try Data(contentsOf: summary.outputURL).count))
        XCTAssertTrue(try String(contentsOf: summary.outputURL, encoding: .utf8).hasPrefix("616263"))
        XCTAssertTrue(summary.outputURL.path.hasPrefix(store.results.path))
        XCTAssertEqual(completions, [true])

        // Convert again → back to ready, result file removed.
        model.convertAgain(ids[0])
        XCTAssertEqual(model.item(ids[0])?.phase, .ready)
        XCTAssertFalse(FileManager.default.fileExists(atPath: summary.outputURL.path))
    }

    func testFailedConversionMapsToFriendlyError() async throws {
        var completions: [Bool] = []
        model.onItemCompleted = { completions.append($0) }
        let ids = await model.add(urls: [try file("bad.txt", "plain text")], owned: true)
        let item = try XCTUnwrap(model.item(ids[0]))
        // Force a conversion that cannot succeed: an option value the core rejects.
        model.setTarget("base64", for: item.id)
        model.setOption("line_width", to: .string("wide"), for: item.id)
        await model.convert(item.id)?.value
        guard case .failed(let error) = model.item(item.id)?.phase else { return XCTFail("expected failure") }
        XCTAssertEqual(error.status, .invalidArgument)
        XCTAssertEqual(error.title, FriendlyError.title(for: .invalidArgument))
        XCTAssertEqual(completions, [false])
        XCTAssertTrue(model.item(item.id)?.phase.canStart ?? false, "a failed file can be retried")
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: store.results.path).isEmpty, "no result left behind")
    }

    func testCancelARunningConversion() async throws {
        let url = try store.newInboxURL(named: "big.bin")
        try Data(count: 96 << 20).write(to: url)
        let ids = await model.add(urls: [url], owned: true)
        model.setTarget("hex", for: ids[0])
        let task = try XCTUnwrap(model.convert(ids[0]))
        // Wait until it is converting (or already queued), then cancel.
        for _ in 0..<200 {
            if case .converting = model.item(ids[0])?.phase { break }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        model.cancel(ids[0])
        await task.value
        let phase = try XCTUnwrap(model.item(ids[0])?.phase)
        if case .finished = phase {
            throw XCTSkip("the conversion finished before it could be cancelled on this machine")
        }
        XCTAssertEqual(phase, .cancelled)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: store.results.path).isEmpty)
    }

    func testCancelAllAndQueuedItemsRespectLimit() async throws {
        let model = QueueModel(catalog: .load(), store: store, maxConcurrent: 1)
        var urls: [URL] = []
        for i in 0..<3 { urls.append(try file("n\(i).txt", String(repeating: "x", count: 1000 + i))) }
        let ids = await model.add(urls: urls, owned: true)
        let tasks = model.convertAll()
        XCTAssertEqual(tasks.count, 3)
        XCTAssertEqual(model.busyCount, 3)
        model.cancelAll()
        for t in tasks { await t.value }
        for id in ids {
            let phase = model.item(id)!.phase
            XCTAssertTrue(phase == .cancelled || phase.summary != nil, "\(phase)")
        }
        XCTAssertEqual(model.busyCount, 0)
        model.removeAll()
    }

    func testConvertAllConvertsEveryReadyItem() async throws {
        var urls: [URL] = []
        for i in 0..<4 { urls.append(try file("f\(i).txt", "file number \(i)")) }
        let ids = await model.add(urls: urls, owned: true)
        XCTAssertEqual(model.readyItems.count, 4)
        for t in model.convertAll() { await t.value }
        XCTAssertEqual(model.finishedItems.count, 4)
        XCTAssertTrue(model.readyItems.isEmpty)
        for id in ids { XCTAssertNotNil(model.item(id)?.phase.summary) }
    }

    func testGroupsShareOneControl() async throws {
        let a = try file("a.txt", "alpha"), b = try file("b.txt", "beta")
        let c = try store.importData(Data([0x00, 0x01, 0x02, 0xFE]), named: "c.bin")
        await model.add(urls: [a, b, c], owned: true)
        let groups = model.groups
        XCTAssertEqual(groups.count, 1)
        let group = try XCTUnwrap(groups.first)
        XCTAssertEqual(group.id, "txt")
        XCTAssertEqual(group.itemIDs.count, 2)
        XCTAssertEqual(group.label, "TXT")

        model.setTarget("checksums", forGroup: group.id)
        for id in group.itemIDs { XCTAssertEqual(model.item(id)?.targetID, "checksums") }
        XCTAssertEqual(model.groups.first?.commonTargetID, "checksums")
        XCTAssertNotEqual(model.items.last?.targetID, "checksums", "other formats are untouched")
    }

    func testRemoveDeletesOwnedCopiesButNotForeignFiles() async throws {
        let owned = try file("owned.txt", "mine")
        let foreignDir = FileManager.default.temporaryDirectory.appendingPathComponent("foreign-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: foreignDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: foreignDir) }
        let foreign = foreignDir.appendingPathComponent("theirs.txt")
        try Data("theirs".utf8).write(to: foreign)

        let ownedIDs = await model.add(urls: [owned], owned: true)
        let foreignIDs = await model.add(urls: [foreign], owned: false)
        model.remove(ownedIDs[0])
        model.remove(foreignIDs[0])
        XCTAssertTrue(model.items.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: owned.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: foreign.path))
    }

    func testChangingTargetRefreshesSchemaAndKeepsCompatibleValues() async throws {
        model.preferences = { (.small, false) }
        let ids = await model.add(urls: [try file("t.txt", "text")], owned: true)
        model.setTarget("txt", for: ids[0])
        var item = try XCTUnwrap(model.item(ids[0]))
        XCTAssertEqual(Set(item.schema.map(\.key)), Set(ConvertibleCore.optionsSchema(source: "txt", target: "txt").map(\.key)))
        model.setOption("encoding", to: .string("utf-16le"), for: ids[0])
        model.setTarget("base64", for: ids[0])
        item = try XCTUnwrap(model.item(ids[0]))
        let keys = Set(item.schema.map(\.key))
        for key in item.options.values.keys { XCTAssertTrue(keys.contains(key), "\(key) belongs to the new schema") }
    }

    func testCompressUsesSameFormatOrArchive() async throws {
        let ids = await model.add(urls: [try file("c.txt", "compress me")], owned: true)
        let item = try XCTUnwrap(model.item(ids[0]))
        let expected = TargetCatalog.compressTarget(item.targets)
        model.applyCompress(for: ids[0])
        XCTAssertEqual(model.item(ids[0])?.targetID, expected?.id ?? item.targetID)
    }

    func testAddInMemoryData() async throws {
        let ids = await model.add(data: Data("pasted".utf8), named: "Pasted text.txt")
        XCTAssertEqual(model.item(ids[0])?.detectedID, "txt")
        XCTAssertTrue(model.item(ids[0])?.ownsFile ?? false)
    }
}

final class OptionsFormTests: XCTestCase {
    private let schema: [OptionSpec] = [
        OptionSpec(key: "quality", label: "Quality", type: .int, defaultValue: .int(85), min: 1, max: 100),
        OptionSpec(key: "preset", label: "Preset", type: .enum, defaultValue: .string("balanced"), choices: ["high", "balanced", "small"]),
        OptionSpec(key: "level", label: "Level", type: .int, defaultValue: .int(6), min: 0, max: 9, category: "advanced"),
        OptionSpec(key: "strip_metadata", label: "Strip metadata", type: .bool, defaultValue: .bool(true)),
        OptionSpec(key: "mode", label: "Mode", type: .enum, defaultValue: .string("keep"), choices: ["keep", "lf"], category: "advanced"),
        OptionSpec(key: "scale", label: "Scale", type: .float, defaultValue: .float(1), min: 0.1, max: 4),
        OptionSpec(key: "background", label: "Background", type: .string, defaultValue: .string("ffffff")),
    ]

    func testControlsFromSchema() {
        let form = OptionsForm(schema: schema)
        XCTAssertEqual(form.preset?.key, "preset")
        XCTAssertEqual(form.presetChoices, [.high, .balanced, .small])
        XCTAssertEqual(form.basic.map(\.spec.key), ["quality", "strip_metadata", "scale", "background"])
        XCTAssertEqual(form.advanced.map(\.spec.key), ["level", "mode"])
        XCTAssertEqual(form.basic[0].control, .slider(min: 1, max: 100, step: 1))
        XCTAssertEqual(form.advanced[0].control, .stepper(min: 0, max: 9))
        XCTAssertEqual(form.basic[1].control, .toggle)
        XCTAssertEqual(form.advanced[1].control, .menu(["keep", "lf"]))
        XCTAssertEqual(form.basic[3].control, .textField)
        if case .slider(let lo, let hi, _) = form.basic[2].control {
            XCTAssertEqual(lo, 0.1); XCTAssertEqual(hi, 4)
        } else { XCTFail("float → slider") }
        XCTAssertTrue(OptionsForm(schema: []).isEmpty)
    }

    func testInitialValuesApplyPreferences() {
        let values = OptionsForm.initialValues(schema: schema, defaultPreset: .small, stripMetadata: false)
        XCTAssertEqual(values["preset"], .string("small"))
        XCTAssertEqual(values["strip_metadata"], .bool(false))
        XCTAssertNil(values["quality"])

        let kept = OptionsForm.initialValues(schema: schema, defaultPreset: .small, stripMetadata: true,
                                             keeping: OptionValues(values: ["quality": .int(40), "gone": .bool(true)]))
        XCTAssertEqual(kept["quality"], .int(40))
        XCTAssertNil(kept["gone"])

        let noPreset = OptionsForm.initialValues(schema: [schema[0]], defaultPreset: .high, stripMetadata: true)
        XCTAssertTrue(noPreset.isEmpty)
    }

    func testValueResolutionAndJSON() {
        var values = OptionValues()
        XCTAssertEqual(OptionsForm.value(of: schema[0], in: values), .int(85))
        values["quality"] = .int(70)
        values["strip_metadata"] = .bool(false)
        values["preset"] = .string("high")
        XCTAssertEqual(OptionsForm.value(of: schema[0], in: values), .int(70))
        XCTAssertEqual(values.json(), #"{"preset":"high","quality":70,"strip_metadata":false}"#)
        XCTAssertEqual(OptionValues().json(), "")
        XCTAssertEqual(OptionsForm.clamp(9, schema[5]), 4)
        XCTAssertEqual(OptionsForm.choiceLabel("keep"), "Keep")
        XCTAssertEqual(OptionsForm.choiceLabel("utf-16le"), "utf-16le")
    }

    func testOptionsJSONWithLimits() throws {
        let json = try XCTUnwrap(ConvertibleCore.optionsString(OptionValues(values: ["padding": .bool(false)]),
                                                               limits: ["max_input_bytes": 10]))
        XCTAssertEqual(json, #"{"limits":{"max_input_bytes":10},"padding":false}"#)
        XCTAssertNil(ConvertibleCore.optionsString(OptionValues(), limits: [:]))
    }

    func testSchemaFromCoreRendersEveryOption() {
        // Every option of every conversion the catalogue offers for text maps to a control.
        for target in ConvertibleCore.targets(for: "txt") {
            let specs = ConvertibleCore.optionsSchema(source: "txt", target: target.id)
            let form = OptionsForm(schema: specs)
            XCTAssertEqual(form.basic.count + form.advanced.count + (form.preset == nil ? 0 : 1), specs.count, target.id)
        }
    }

    func testDecodingSchemaJSON() throws {
        let json = #"[{"key":"quality","label":"Quality","type":"int","default":85,"min":1,"max":100,"choices":[],"description":"Higher is better.","category":"basic"},{"key":"lossless","label":"Lossless","type":"bool","default":false,"min":null,"max":null,"choices":[],"description":"","category":"advanced"}]"#
        let specs = try JSONDecoder().decode([OptionSpec].self, from: Data(json.utf8))
        XCTAssertEqual(specs[0].defaultValue, .int(85))
        XCTAssertEqual(specs[0].max, 100)
        XCTAssertEqual(specs[1].defaultValue, .bool(false))
        XCTAssertNil(specs[1].min)
        XCTAssertTrue(specs[1].isAdvanced)
    }

    func testWideAndUnboundedRangesUseANumberField() {
        let rate = OptionSpec(key: "sample_rate", label: "Sample rate", type: .int, defaultValue: .int(0), min: 0, max: 384_000)
        XCTAssertEqual(OptionsForm.control(for: rate), .number(min: 0, max: 384_000, integer: true))
        let bitrate = OptionSpec(key: "bitrate_kbps", label: "Bitrate", type: .int, defaultValue: .int(0), min: 0, max: 1000)
        XCTAssertEqual(OptionsForm.control(for: bitrate), .slider(min: 0, max: 1000, step: 1))
        let open = OptionSpec(key: "width", label: "Width", type: .int, defaultValue: .int(0), min: 0)
        XCTAssertEqual(OptionsForm.control(for: open), .number(min: 0, max: nil, integer: true))
        let gamma = OptionSpec(key: "gamma", label: "Gamma", type: .float, defaultValue: .float(1))
        XCTAssertEqual(OptionsForm.control(for: gamma), .number(min: nil, max: nil, integer: false))
    }

    func testNewerCoreJSONStillDecodes() throws {
        // Unknown fields are ignored, a new additive "role" is read, missing fields get
        // defaults, and an entry of an unexpected shape is skipped, not fatal.
        let json = #"[{"id":"zip","name":"ZIP","category":"archive","extensions":["zip"],"mime":"application/zip","lossy":false,"compress":false,"role":"archive","future":{"x":1}},{"id":"png"},42,{"name":"no id"},{"id":"tar.gz","category":"archive","role":"extract"}]"#
        let targets = try JSONDecoder().decode(LossyArray<TargetFormat>.self, from: Data(json.utf8)).elements
        XCTAssertEqual(targets.map(\.id), ["zip", "png", "tar.gz"])
        XCTAssertEqual(targets[0].role, "archive")
        XCTAssertNil(targets[1].role)
        XCTAssertEqual(targets[1].category, "other")
        XCTAssertEqual(TargetCatalog.compressTarget(targets)?.id, "zip")
        XCTAssertNil(TargetCatalog.compressTarget([targets[2]]), "an extract target never compresses")
    }
}

final class TargetCatalogTests: XCTestCase {
    private let targets = [
        TargetFormat(id: "png", name: "PNG image", category: "image", extensions: ["png"], compress: true),
        TargetFormat(id: "jpg", name: "JPEG image", category: "image", extensions: ["jpg", "jpeg"], lossy: true),
        TargetFormat(id: "webp", name: "WebP image", category: "image", extensions: ["webp"]),
        TargetFormat(id: "pdf", name: "PDF document", category: "document", extensions: ["pdf"]),
        TargetFormat(id: "base64", name: "Base64 text", category: "encoding", extensions: ["b64", "base64"]),
        TargetFormat(id: "zip", name: "ZIP archive", category: "archive", extensions: ["zip"]),
    ]
    private let popularity = ["png": 95, "jpg": 95, "webp": 80, "pdf": 98, "base64": 40, "zip": 90]

    func testSectionsPutSourceCategoryFirstThenPopular() {
        let sections = TargetCatalog.sections(targets: targets, sourceCategory: "image", popularity: popularity)
        XCTAssertEqual(sections.map(\.id), ["image", "document", "archive", "encoding"])
        XCTAssertEqual(sections[0].targets.map(\.id), ["png", "jpg", "webp"], "same-format first, then popularity, ties keep core order")
        XCTAssertEqual(sections[0].title, "Images")
        XCTAssertEqual(sections.flatMap(\.targets).count, targets.count)
    }

    func testSearchRanksExactThenPrefixThenContains() {
        XCTAssertEqual(TargetCatalog.search(targets, query: "jpeg", popularity: popularity).map(\.id), ["jpg"])
        XCTAssertEqual(TargetCatalog.search(targets, query: ".PNG", popularity: popularity).first?.id, "png")
        XCTAssertEqual(TargetCatalog.search(targets, query: "image", popularity: popularity).map(\.id), ["png", "jpg", "webp"])
        XCTAssertEqual(TargetCatalog.search(targets, query: "b", popularity: popularity).first?.id, "base64")
        XCTAssertTrue(TargetCatalog.search(targets, query: "mp3", popularity: popularity).isEmpty)
        let found = TargetCatalog.sections(targets: targets, sourceCategory: nil, popularity: popularity, query: "doc")
        XCTAssertEqual(found.map(\.id), ["results"])
        XCTAssertEqual(found[0].targets.map(\.id), ["pdf"])
        XCTAssertTrue(TargetCatalog.sections(targets: targets, sourceCategory: nil, popularity: popularity, query: "zzz").isEmpty)
    }

    func testDefaultAndCompressTargets() {
        XCTAssertEqual(TargetCatalog.defaultTarget(targets)?.id, "jpg")
        XCTAssertEqual(TargetCatalog.compressTarget(targets)?.id, "png")
        XCTAssertEqual(TargetCatalog.compressTarget(Array(targets.dropFirst()))?.id, "zip")
        XCTAssertNil(TargetCatalog.compressTarget([targets[4]]))
        XCTAssertNil(TargetCatalog.defaultTarget([]))
    }

    func testRealTargetsAreGroupedWithoutLoss() {
        let catalog = FormatCatalog.load()
        let real = ConvertibleCore.targets(for: "txt")
        let sections = TargetCatalog.sections(targets: real, sourceCategory: catalog.format("txt")?.category, popularity: catalog.popularity)
        XCTAssertEqual(Set(sections.flatMap(\.targets).map(\.id)), Set(real.map(\.id)))
        XCTAssertEqual(sections.first?.id, catalog.format("txt")?.category)
        XCTAssertEqual(TargetCatalog.search(real, query: "base64", popularity: catalog.popularity).first?.id, "base64")
    }

    func testFormatsBrowserGroupsWholeCatalogue() {
        let formats = ConvertibleCore.formats()
        let sections = FormatsBrowser.sections(formats)
        XCTAssertEqual(sections.flatMap(\.formats).count, formats.count)
        XCTAssertEqual(sections.map(\.id), sections.map(\.id).sorted { FileCategory.rank($0) < FileCategory.rank($1) })
        let encodings = FormatsBrowser.sections(formats, query: "hex")
        XCTAssertTrue(encodings.flatMap(\.formats).contains { $0.id == "hex" })
        XCTAssertTrue(FormatsBrowser.sections(formats, query: "Encodings").contains { $0.id == "encoding" })
        // A search puts the best match first, whatever its category.
        if let pdf = formats.first(where: { $0.id == "pdf" }) {
            XCTAssertEqual(FormatsBrowser.sections(formats, query: ".pdf").first?.formats.first?.id, pdf.id)
        }
        // The category filter keeps one category.
        let images = FormatsBrowser.sections(formats, category: "image")
        XCTAssertEqual(images.map(\.id), ["image"])
        XCTAssertEqual(images[0].formats.count, FormatsBrowser.categories(formats).first { $0.id == "image" }?.count)
    }

    func testLargeCatalogueSearchIsFast() {
        let formats = ConvertibleCore.formats()
        measure { _ = FormatsBrowser.sections(formats, query: "mp") }
    }
}

final class FormattingTests: XCTestCase {
    func testSizeText() {
        XCTAssertTrue(SizeText.string(0).hasPrefix("0"), SizeText.string(0))
        XCTAssertTrue(SizeText.string(512).contains("512"))
        XCTAssertTrue(SizeText.string(1_500_000).contains("1.5"))
        XCTAssertTrue(SizeText.string(1_500_000).contains("MB"))
        XCTAssertEqual(SizeText.string(-5), SizeText.string(0))
    }

    func testSizeChange() {
        XCTAssertEqual(SizeChange(before: 1000, after: 380).summary, "62% smaller")
        XCTAssertEqual(SizeChange(before: 1000, after: 1334).summary, "33% larger")
        XCTAssertEqual(SizeChange(before: 1000, after: 1000).summary, "Same size")
        XCTAssertEqual(SizeChange(before: 100_000, after: 99_999).summary, "Same size")
        XCTAssertEqual(SizeChange(before: 0, after: 10).summary, "New file")
        XCTAssertTrue(SizeChange(before: 10, after: 5).isSmaller)
        XCTAssertEqual(SizeChange(before: 200, after: 50).savedFraction, 0.75, accuracy: 1e-9)
    }

    func testOutputNames() {
        XCTAssertEqual(FileNaming.outputName(for: "report.final.txt", extension: "b64"), "report.final.b64")
        XCTAssertEqual(FileNaming.outputName(for: "README", extension: "hex"), "README.hex")
        XCTAssertEqual(FileNaming.outputName(for: ".profile", extension: "txt"), ".profile.txt")
        XCTAssertEqual(FileStore.sanitize("a/b:c.txt"), "a_b_c.txt")
        XCTAssertEqual(FileStore.sanitize("  "), "file")
    }
}

final class MotionTests: XCTestCase {
    func testCubicBezier() {
        let linear = CubicBezier(0, 0, 1, 1)
        for x in stride(from: 0.0, through: 1.0, by: 0.1) { XCTAssertEqual(linear.value(at: x), x, accuracy: 1e-4) }
        let standard = CubicBezier(0.2, 0, 0, 1)
        XCTAssertEqual(standard.value(at: 0), 0)
        XCTAssertEqual(standard.value(at: 1), 1)
        XCTAssertGreaterThan(standard.value(at: 0.5), 0.5, "decelerating curve")
        let overshoot = CubicBezier(0.34, 1.56, 0.64, 1)
        XCTAssertGreaterThan((0...100).map { overshoot.value(at: Double($0) / 100) }.max()!, 1, "overshoots")
        // CSS "ease" at x = 0.5 is ≈ 0.8024.
        XCTAssertEqual(CubicBezier(0.25, 0.1, 0.25, 1).value(at: 0.5), 0.8024, accuracy: 1e-3)
    }

    func testStoryboardLoadsAndMatchesReferenceTiming() throws {
        let sb = try XCTUnwrap(Storyboard.shared, "animation.json is bundled")
        XCTAssertEqual(sb.intro.durationMs, 1800)
        XCTAssertEqual(sb.spinner.durationMs, 1400)
        let stage = try XCTUnwrap(sb.intro.layer("stage"))
        XCTAssertEqual(sb.value("scale", of: stage, at: 0), 0.86, accuracy: 1e-9)
        XCTAssertEqual(sb.value("rotationDeg", of: stage, at: 0), -8, accuracy: 1e-9)
        XCTAssertEqual(sb.value("scale", of: stage, at: 1350), 1.03, accuracy: 1e-9)

        // End state equals the static logo.
        let end = sb.intro.durationMs
        XCTAssertEqual(sb.value("scale", of: stage, at: end), 1)
        XCTAssertEqual(sb.value("rotationDeg", of: stage, at: end), 0)
        XCTAssertEqual(sb.value("trimEnd", of: sb.intro.layer("trace")!, at: end), 1)
        XCTAssertEqual(sb.value("opacity", of: sb.intro.layer("ghost")!, at: end), 0)
        XCTAssertEqual(sb.value("opacity", of: sb.intro.layer("rippleRound")!, at: end), 0)
        XCTAssertEqual(sb.value("opacity", of: sb.intro.layer("rippleSquare")!, at: end), 0)
        XCTAssertEqual(sb.value("scale", of: sb.intro.layer("seed")!, at: end), 1)

        // Holds before the first keyframe; trace has not started at 200 ms.
        XCTAssertEqual(sb.value("trimEnd", of: sb.intro.layer("trace")!, at: 200), 0)
        let mid = sb.value("trimEnd", of: sb.intro.layer("trace")!, at: 750)
        XCTAssertTrue(mid > 0 && mid < 1)

        // Spinner: drawn at 630 ms, erasing from 770 ms.
        let runner = try XCTUnwrap(sb.spinner.layer("runner"))
        XCTAssertEqual(sb.value("trimEnd", of: runner, at: 630), 1)
        XCTAssertEqual(sb.value("trimStart", of: runner, at: 700), 0)
        XCTAssertEqual(sb.value("trimStart", of: runner, at: 1400), 1)
        XCTAssertEqual(sb.value("opacity", of: sb.spinner.layer("track")!, at: 0), 0.16, accuracy: 1e-9)
    }

    func testMarkGeometry() {
        let length = MarkGeometry.centerline.boundingRect
        XCTAssertEqual(length.minX, 260, accuracy: 0.5)
        XCTAssertEqual(length.maxX, 864, accuracy: 0.5)
        XCTAssertEqual(length.minY, 260, accuracy: 0.5)
        XCTAssertEqual(length.maxY, 764, accuracy: 0.5)
        let outline = MarkGeometry.stroke(from: 0, to: 1).boundingRect
        XCTAssertEqual(outline.minX, 160, accuracy: 1)
        XCTAssertEqual(outline.maxY, 864, accuracy: 1)
        XCTAssertTrue(MarkGeometry.stroke(from: 0.5, to: 0.5).isEmpty)
    }
}
