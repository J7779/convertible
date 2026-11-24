// Thin Swift binding over the core's C interface (convertible.h).
//
// Ownership rules honoured here:
//   - every `char*` the core returns is released with cvt_string_free()
//   - every `cvt_result*` is released with cvt_result_free(), after its
//     `const char*` and data views have been copied into Swift values
// All calls are synchronous in C; `convert`/`convertFile` run them on a
// background queue and wire Swift task cancellation to the progress callback.
import CConvertible
import Foundation

enum ConvertibleCore {
    /// Library version, e.g. "0.1.0".
    static var version: String { String(cString: cvt_version()) }

    // MARK: Catalogue

    /// Every format the core knows, most popular first.
    static func formats() -> [FormatInfo] {
        decodeOwned([FormatInfo].self, cvt_formats_json()) ?? []
    }

    /// Formats `source` (id or extension) converts to, most relevant first.
    static func targets(for source: String) -> [TargetFormat] {
        decodeOwned([TargetFormat].self, cvt_targets_json(source)) ?? []
    }

    /// Options understood by the conversion `source` → `target`.
    static func optionsSchema(source: String, target: String) -> [OptionSpec] {
        decodeOwned([OptionSpec].self, cvt_options_schema_json(source, target)) ?? []
    }

    /// Detects the format of in-memory content. `filename` may help (only its extension is used).
    static func detect(data: Data, filename: String? = nil) -> String? {
        let raw: UnsafeMutablePointer<CChar>? = data.withUnsafeBytes { buffer in
            let base = buffer.bindMemory(to: UInt8.self).baseAddress
            if let filename {
                return filename.withCString { cvt_detect(base, buffer.count, $0) }
            }
            return cvt_detect(base, buffer.count, nil)
        }
        return takeString(raw)
    }

    /// Detects the format of a file. The file is memory-mapped, so even large files are cheap.
    static func detect(fileAt url: URL) -> String? {
        withSecurityScope(url) {
            guard let data = try? Data(contentsOf: url, options: .alwaysMapped) else {
                return nil
            }
            return detect(data: data, filename: url.lastPathComponent)
        }
    }

    // MARK: Conversion

    /// Converts in-memory content. Runs on a background queue; cancelling the
    /// calling task stops the core at its next progress checkpoint.
    static func convert(data: Data, filename: String? = nil, source: String? = nil, target: String,
                        options: OptionValues = OptionValues(), limits: [String: Int] = [:],
                        progress: AsyncStream<ConversionProgress>.Continuation? = nil) async throws -> ConversionOutput {
        let optionsJSON = optionsString(options, limits: limits)
        return try await runCancellable(progress: progress) { callback, context in
            let result: OpaquePointer? = data.withUnsafeBytes { buffer in
                let base = buffer.bindMemory(to: UInt8.self).baseAddress
                return withOptionalCString(filename) { name in
                    withOptionalCString(source) { src in
                        target.withCString { tgt in
                            withOptionalCString(optionsJSON) { opts in
                                cvt_convert(base, buffer.count, name, src, tgt, opts, callback, context)
                            }
                        }
                    }
                }
            }
            return try consume(result) { result in
                let size = cvt_result_size(result)
                let bytes = cvt_result_data(result)
                let data = (bytes != nil && size > 0) ? Data(bytes: bytes!, count: size) : Data()
                return ConversionOutput(data: data,
                                        format: String(cString: cvt_result_format(result)),
                                        fileExtension: String(cString: cvt_result_extension(result)),
                                        mime: String(cString: cvt_result_mime(result)),
                                        warnings: warnings(of: result))
            }
        }
    }

    /// Converts a file on disk into `output` (its directory must exist). The input is
    /// memory-mapped by the core. Security-scoped URLs are accessed for the duration.
    static func convertFile(input: URL, output: URL, source: String? = nil, target: String,
                            options: OptionValues = OptionValues(), limits: [String: Int] = [:],
                            progress: AsyncStream<ConversionProgress>.Continuation? = nil) async throws -> FileConversionOutput {
        let optionsJSON = optionsString(options, limits: limits)
        let inputPath = input.path
        let outputPath = output.path
        return try await runCancellable(progress: progress) { callback, context in
            let scoped = input.startAccessingSecurityScopedResource()
            defer { if scoped { input.stopAccessingSecurityScopedResource() } }
            let result = inputPath.withCString { inPath in
                outputPath.withCString { outPath in
                    withOptionalCString(source) { src in
                        target.withCString { tgt in
                            withOptionalCString(optionsJSON) { opts in
                                cvt_convert_file(inPath, outPath, src, tgt, opts, callback, context)
                            }
                        }
                    }
                }
            }
            return try consume(result) { result in
                let size = (try? FileManager.default.attributesOfItem(atPath: outputPath)[.size] as? NSNumber)?.int64Value ?? 0
                return FileConversionOutput(url: output, size: size,
                                            format: String(cString: cvt_result_format(result)),
                                            fileExtension: String(cString: cvt_result_extension(result)),
                                            mime: String(cString: cvt_result_mime(result)),
                                            warnings: warnings(of: result))
            }
        }
    }

    // MARK: Internals

    /// Shared state between the Swift task and the C progress callback.
    final class ProgressContext: @unchecked Sendable {
        private let lock = NSLock()
        private var cancelled = false
        private var lastFraction = -1.0
        private var lastStage = ""
        let continuation: AsyncStream<ConversionProgress>.Continuation?

        init(continuation: AsyncStream<ConversionProgress>.Continuation?) { self.continuation = continuation }

        func cancel() { lock.lock(); cancelled = true; lock.unlock() }
        var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }

        /// Forwards a report, coalescing tiny steps so the UI is not flooded.
        func report(_ raw: Double, _ stage: String) {
            guard let continuation else { return }
            lock.lock()
            // Defensive: keep the value finite, within 0...1 and never decreasing.
            let fraction = raw.isFinite ? min(max(raw, max(lastFraction, 0)), 1) : max(lastFraction, 0)
            let significant = stage != lastStage || fraction - lastFraction >= 0.005 || (fraction >= 1 && lastFraction < 1)
            if significant { lastFraction = fraction; lastStage = stage }
            lock.unlock()
            if significant { continuation.yield(ConversionProgress(fraction: min(max(fraction, 0), 1), stage: stage)) }
        }
    }

    private static let progressCallback: cvt_progress_fn = { fraction, stage, user in
        guard let user else { return 0 }
        let context = Unmanaged<ProgressContext>.fromOpaque(user).takeUnretainedValue()
        context.report(fraction, stage.map { String(cString: $0) } ?? "")
        return context.isCancelled ? 1 : 0
    }

    private static let workQueue = DispatchQueue(label: "app.convertible.core", qos: .userInitiated, attributes: .concurrent)

    private static func runCancellable<T>(
        progress: AsyncStream<ConversionProgress>.Continuation?,
        _ body: @escaping @Sendable (cvt_progress_fn, UnsafeMutableRawPointer) throws -> T
    ) async throws -> T {
        let context = ProgressContext(continuation: progress)
        defer { progress?.finish() }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<T, Error>) in
                if Task.isCancelled { context.cancel() }
                workQueue.async {
                    // The context outlives the synchronous call: it is retained by this closure.
                    let pointer = Unmanaged.passUnretained(context).toOpaque()
                    do {
                        let value = try body(progressCallback, pointer)
                        withExtendedLifetime(context) {}
                        continuation.resume(returning: value)
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            context.cancel()
        }
    }

    /// Reads a result, frees it, and throws for failures.
    private static func consume<T>(_ result: OpaquePointer?, _ read: (OpaquePointer) -> T) throws -> T {
        guard let result else { throw CoreError(status: .internalError, message: "The converter returned no result.") }
        defer { cvt_result_free(result) }
        let raw = cvt_result_status(result)
        let status = CoreStatus(rawValue: raw) ?? .internalError
        guard status == .ok else {
            throw CoreError(status: status, message: String(cString: cvt_result_message(result)))
        }
        return read(result)
    }

    private static func warnings(of result: OpaquePointer) -> [String] {
        let json = String(cString: cvt_result_warnings_json(result))
        return (try? JSONDecoder().decode([String].self, from: Data(json.utf8))) ?? []
    }

    static func optionsString(_ options: OptionValues, limits: [String: Int]) -> String? {
        if limits.isEmpty { return options.isEmpty ? nil : options.json() }
        var object: [String: Any] = [:]
        for (key, value) in options.values {
            switch value {
            case .int(let v): object[key] = v
            case .float(let v): object[key] = v
            case .bool(let v): object[key] = v
            case .string(let v): object[key] = v
            }
        }
        object["limits"] = limits
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    /// Takes ownership of a core string: copies it and frees the original.
    private static func takeString(_ raw: UnsafeMutablePointer<CChar>?) -> String? {
        guard let raw else { return nil }
        defer { cvt_string_free(raw) }
        return String(cString: raw)
    }

    private static func decodeOwned<T: Decodable>(_ type: T.Type, _ raw: UnsafeMutablePointer<CChar>?) -> T? {
        guard let text = takeString(raw) else { return nil }
        return try? JSONDecoder().decode(type, from: Data(text.utf8))
    }

    private static func withSecurityScope<T>(_ url: URL, _ body: () -> T) -> T {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        return body()
    }
}

private func withOptionalCString<T>(_ string: String?, _ body: (UnsafePointer<CChar>?) -> T) -> T {
    guard let string else { return body(nil) }
    return string.withCString { body($0) }
}
