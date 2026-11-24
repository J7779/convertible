// Real conversions through the real core, via the Swift binding.
import CryptoKit
import XCTest
@testable import Convertible

final class CoreBindingTests: XCTestCase {
    private var workDir: URL!

    override func setUpWithError() throws {
        workDir = FileManager.default.temporaryDirectory.appendingPathComponent("CoreBindingTests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: workDir)
    }

    private func text(_ data: Data) -> String { String(decoding: data, as: UTF8.self) }

    /// Id of the first target of `source` that satisfies `predicate` (catalogue is dynamic).
    private func target(from source: String, _ predicate: (TargetFormat) -> Bool) throws -> TargetFormat {
        try XCTUnwrap(ConvertibleCore.targets(for: source).first(where: predicate), "no matching target for \(source)")
    }

    // MARK: Catalogue

    func testVersionIsSemantic() {
        let parts = ConvertibleCore.version.split(separator: ".")
        XCTAssertEqual(parts.count, 3, ConvertibleCore.version)
        XCTAssertTrue(parts.allSatisfy { Int($0) != nil })
    }

    func testCatalogueIsWellFormed() {
        let formats = ConvertibleCore.formats()
        XCTAssertGreaterThan(formats.count, 10)
        XCTAssertEqual(Set(formats.map(\.id)).count, formats.count, "ids are unique")
        let popularity = formats.map(\.popularity)
        XCTAssertEqual(popularity, popularity.sorted(by: >), "most popular first")
        for format in formats {
            XCTAssertEqual(format.id, format.id.lowercased())
            XCTAssertTrue(FileCategory.order.contains(format.category), "known category for \(format.id)")
            XCTAssertTrue((0...100).contains(format.popularity))
        }
        let ids = Set(formats.map(\.id))
        XCTAssertTrue(ids.isSuperset(of: ["base64", "hex", "checksums", "txt"]))
    }

    func testTargetsAndUnknownSource() throws {
        let targets = ConvertibleCore.targets(for: "txt")
        XCTAssertTrue(targets.contains { $0.id == "base64" })
        XCTAssertEqual(targets.filter(\.compress).map(\.id), targets.contains { $0.id == "txt" } ? ["txt"] : [])
        // Extensions are accepted too.
        XCTAssertEqual(ConvertibleCore.targets(for: "b64").map(\.id), ConvertibleCore.targets(for: "base64").map(\.id))
        XCTAssertTrue(ConvertibleCore.targets(for: "no-such-format").isEmpty)
        XCTAssertTrue(ConvertibleCore.optionsSchema(source: "no-such-format", target: "base64").isEmpty)
        // Any file converts to the universal encodings.
        XCTAssertTrue(ConvertibleCore.targets(for: QueueModel.unknownSourceID).contains { $0.id == "checksums" })
    }

    func testOptionsSchemaDecodes() throws {
        let schema = ConvertibleCore.optionsSchema(source: "txt", target: "base64")
        XCTAssertFalse(schema.isEmpty)
        for spec in schema {
            XCTAssertFalse(spec.key.isEmpty)
            XCTAssertFalse(spec.label.isEmpty)
            XCTAssertTrue(["basic", "advanced"].contains(spec.category))
            if spec.type == .enum { XCTAssertFalse(spec.choices.isEmpty, spec.key) }
            if spec.type == .int { XCTAssertNotNil(spec.defaultValue?.intValue, spec.key) }
        }
    }

    func testDetect() {
        XCTAssertEqual(ConvertibleCore.detect(data: Data("hello world\n".utf8), filename: "notes.txt"), "txt")
        let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52])
        XCTAssertEqual(ConvertibleCore.detect(data: png, filename: nil), "png")
    }

    // MARK: Conversions

    func testBase64RoundTrip() async throws {
        let original = Data("Convertible: offline, private conversion. ✓ Ünïcödé\n".utf8)
        let encoded = try await ConvertibleCore.convert(data: original, filename: "a.txt", source: "txt", target: "base64",
                                                        options: OptionValues(values: ["line_width": .int(0)]))
        XCTAssertEqual(encoded.format, "base64")
        XCTAssertEqual(text(encoded.data).trimmingCharacters(in: .whitespacesAndNewlines), original.base64EncodedString())
        let decodeTarget = try target(from: "base64") { $0.id == "bin" }
        let decoded = try await ConvertibleCore.convert(data: encoded.data, filename: "a.b64", source: "base64", target: decodeTarget.id)
        XCTAssertEqual(decoded.data, original)
    }

    func testBinaryBase64RoundTripAllBytes() async throws {
        let original = Data((0...255).map(UInt8.init) + (0...255).reversed().map(UInt8.init))
        let encoded = try await ConvertibleCore.convert(data: original, source: "bin", target: "base64")
        XCTAssertEqual(Data(base64Encoded: text(encoded.data), options: .ignoreUnknownCharacters), original)
        let decoded = try await ConvertibleCore.convert(data: encoded.data, source: "base64", target: "bin")
        XCTAssertEqual(decoded.data, original)
    }

    func testHexKnownVectorAndRoundTrip() async throws {
        let encoded = try await ConvertibleCore.convert(data: Data("abc".utf8), filename: "abc.txt", target: "hex")
        XCTAssertEqual(text(encoded.data).trimmingCharacters(in: .whitespacesAndNewlines), "616263")
        let upper = try await ConvertibleCore.convert(data: Data([0xDE, 0xAD, 0xBE, 0xEF]), source: "bin", target: "hex",
                                                      options: OptionValues(values: ["uppercase": .bool(true)]))
        XCTAssertEqual(text(upper.data).trimmingCharacters(in: .whitespacesAndNewlines), "DEADBEEF")
        let back = try await ConvertibleCore.convert(data: upper.data, source: "hex", target: "bin")
        XCTAssertEqual(back.data, Data([0xDE, 0xAD, 0xBE, 0xEF]))
    }

    func testChecksumsMatchKnownVectors() async throws {
        let report = try await ConvertibleCore.convert(data: Data("abc".utf8), filename: "abc.txt", target: "checksums")
        let body = text(report.data).lowercased()
        XCTAssertTrue(body.contains("352441c2"), "CRC-32")
        XCTAssertTrue(body.contains("900150983cd24fb0d6963f7d28e17f72"), "MD5")
        XCTAssertTrue(body.contains("a9993e364706816aba3e25717850c26c9cd0d89d"), "SHA-1")
        XCTAssertTrue(body.contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256")
    }

    func testTextReencodingToUTF16() async throws {
        let original = "Grüße, 世界"
        let out = try await ConvertibleCore.convert(data: Data(original.utf8), filename: "g.txt", source: "txt", target: "txt",
                                                    options: OptionValues(values: ["encoding": .string("utf-16le"), "bom": .bool(true)]))
        XCTAssertEqual(Array(out.data.prefix(2)), [0xFF, 0xFE], "UTF-16LE byte order mark")
        XCTAssertEqual(String(data: out.data.dropFirst(2), encoding: .utf16LittleEndian), original)
        let back = try await ConvertibleCore.convert(data: out.data, filename: "g.txt", source: "txt", target: "txt")
        XCTAssertEqual(String(decoding: back.data, as: UTF8.self), original)
    }

    func testProgressIsReportedAndMonotonic() async throws {
        let (stream, continuation) = AsyncStream.makeStream(of: ConversionProgress.self)
        let collector = Task { () -> [ConversionProgress] in
            var all: [ConversionProgress] = []
            for await p in stream { all.append(p) }
            return all
        }
        _ = try await ConvertibleCore.convert(data: Data(count: 4 << 20), source: "bin", target: "base64", progress: continuation)
        let reports = await collector.value
        XCTAssertFalse(reports.isEmpty)
        let fractions = reports.map(\.fraction)
        XCTAssertEqual(fractions, fractions.sorted())
        XCTAssertTrue(fractions.allSatisfy { (0...1).contains($0) })
    }

    // MARK: Errors

    func testErrorStatuses() async {
        await assertStatus(.unknownFormat) {
            _ = try await ConvertibleCore.convert(data: Data("x".utf8), source: "txt", target: "no-such-format")
        }
        await assertStatus(.decode) {
            _ = try await ConvertibleCore.convert(data: Data("@@@ not hex @@@".utf8), source: "hex", target: "bin")
        }
        await assertStatus(.invalidArgument) {
            _ = try await ConvertibleCore.convert(data: Data("x".utf8), source: "txt", target: "base64",
                                                  options: OptionValues(values: ["line_width": .string("wide")]))
        }
        await assertStatus(.limit) {
            _ = try await ConvertibleCore.convert(data: Data(count: 4096), source: "bin", target: "base64",
                                                  limits: ["max_input_bytes": 1024])
        }
        await assertStatus(.io) {
            _ = try await ConvertibleCore.convertFile(input: self.workDir.appendingPathComponent("missing.bin"),
                                                      output: self.workDir.appendingPathComponent("out.b64"), target: "base64")
        }
    }

    func testFriendlyErrorMapping() {
        for status in CoreStatus.allCases where status != .ok {
            let friendly = FriendlyError.from(CoreError(status: status, message: ""))
            XCTAssertEqual(friendly.status, status)
            XCTAssertFalse(friendly.title.isEmpty)
            XCTAssertFalse(friendly.message.isEmpty)
        }
        let decode = FriendlyError.from(CoreError(status: .decode, message: "bad chunk 3"))
        XCTAssertEqual(decode.title, "This file couldn’t be read")
        XCTAssertEqual(decode.message, "Bad chunk 3.")
        XCTAssertEqual(FriendlyError.from(CancellationError()).status, .cancelled)
        XCTAssertEqual(FriendlyError.from(CocoaError(.fileNoSuchFile)).status, .io)
    }

    // MARK: Cancellation and files

    func testCancellationStopsAConversion() async throws {
        let input = workDir.appendingPathComponent("big.bin")
        try Data(count: 64 << 20).write(to: input)
        let output = workDir.appendingPathComponent("big.hex")
        let (stream, continuation) = AsyncStream.makeStream(of: ConversionProgress.self)
        let task = Task {
            try await ConvertibleCore.convertFile(input: input, output: output, source: "bin", target: "hex", progress: continuation)
        }
        // Cancel as soon as the core reports that it is working.
        for await _ in stream { task.cancel(); break }
        do {
            _ = try await task.value
            XCTFail("expected cancellation")
        } catch let error as CoreError {
            XCTAssertEqual(error.status, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: output.path), "no partial output is left behind")
    }

    func testAlreadyCancelledTaskDoesNotConvert() async {
        let task = Task { () -> CoreStatus? in
            withUnsafeCurrentTask { $0?.cancel() }
            do {
                _ = try await ConvertibleCore.convert(data: Data(count: 8 << 20), source: "bin", target: "hex")
                return .ok
            } catch let error as CoreError {
                return error.status
            } catch {
                return nil
            }
        }
        let status = await task.value
        XCTAssertEqual(status, .cancelled)
    }

    func testLargeFileThroughFileAPI() async throws {
        // 48 MiB of patterned bytes, converted from disk (memory-mapped) to checksums and base64.
        let input = workDir.appendingPathComponent("large.bin")
        var chunk = Data(count: 1 << 20)
        chunk.withUnsafeMutableBytes { raw in for i in 0..<raw.count { raw[i] = UInt8(truncatingIfNeeded: i &* 31 &+ 7) } }
        FileManager.default.createFile(atPath: input.path, contents: nil)
        let handle = try FileHandle(forWritingTo: input)
        var hasher = SHA256()
        for _ in 0..<48 { handle.write(chunk); hasher.update(data: chunk) }
        try handle.close()
        let expected = hasher.finalize().map { String(format: "%02x", $0) }.joined()

        let sums = workDir.appendingPathComponent("large.txt")
        let result = try await ConvertibleCore.convertFile(input: input, output: sums, source: "bin", target: "checksums")
        XCTAssertEqual(result.format, "checksums")
        XCTAssertTrue(try String(contentsOf: sums, encoding: .utf8).lowercased().contains(expected))
        XCTAssertEqual(result.size, Int64(try Data(contentsOf: sums).count))

        let b64 = workDir.appendingPathComponent("large.b64")
        let encoded = try await ConvertibleCore.convertFile(input: input, output: b64, source: "bin", target: "base64")
        XCTAssertGreaterThan(encoded.size, Int64(48 << 20) * 4 / 3)
        let decodedURL = workDir.appendingPathComponent("large.out")
        _ = try await ConvertibleCore.convertFile(input: b64, output: decodedURL, source: "base64", target: "bin")
        XCTAssertEqual(SHA256.hash(data: try Data(contentsOf: decodedURL, options: .alwaysMapped))
            .map { String(format: "%02x", $0) }.joined(), expected)
    }

    func testWarningsDecodeAsArray() async throws {
        // Characters outside Latin-1 cannot be represented: the core reports a warning.
        let out = try await ConvertibleCore.convert(data: Data("price: 5 € — 世界".utf8), filename: "w.txt", source: "txt",
                                                    target: "txt", options: OptionValues(values: ["encoding": .string("latin1")]))
        XCTAssertFalse(out.warnings.isEmpty)
    }

    private func assertStatus(_ expected: CoreStatus, file: StaticString = #filePath, line: UInt = #line,
                              _ body: () async throws -> Void) async {
        do {
            try await body()
            XCTFail("expected \(expected)", file: file, line: line)
        } catch let error as CoreError {
            XCTAssertEqual(error.status, expected, error.message, file: file, line: line)
            XCTAssertFalse(error.message.isEmpty, file: file, line: line)
        } catch {
            XCTFail("unexpected \(error)", file: file, line: line)
        }
    }
}
