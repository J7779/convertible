// Inputs built in the test itself, so every expected byte is known up front.
import Compression
import CryptoKit
import UIKit

enum Fixtures {
    /// Deterministic pseudo-random bytes (all 256 values occur).
    static func bytes(count: Int, seed: UInt32 = 0x2545F491) -> Data {
        var state = seed
        var out = Data(capacity: count)
        for _ in 0..<count {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5
            out.append(UInt8(truncatingIfNeeded: state))
        }
        return out
    }

    static func hex(_ data: Data, uppercase: Bool = false) -> String {
        data.map { String(format: uppercase ? "%02X" : "%02x", $0) }.joined()
    }

    static func digestHex<D: Digest>(_ digest: D) -> String {
        digest.map { String(format: "%02x", $0) }.joined()
    }

    /// Parses "61 62 63", "616263" or multi-line hex.
    static func parseHex(_ text: String) -> Data? {
        let digits = text.filter { !$0.isWhitespace }
        guard digits.count % 2 == 0 else { return nil }
        var out = Data(capacity: digits.count / 2)
        var index = digits.startIndex
        while index < digits.endIndex {
            let next = digits.index(index, offsetBy: 2)
            guard let byte = UInt8(digits[index..<next], radix: 16) else { return nil }
            out.append(byte)
            index = next
        }
        return out
    }

    /// A small opaque PNG drawn with UIKit.
    static func png(width: Int = 24, height: Int = 16) -> Data {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: width, height: height), format: format)
        return renderer.pngData { context in
            UIColor(red: 0.42, green: 0.36, blue: 0.91, alpha: 1).setFill()
            context.fill(CGRect(x: 0, y: 0, width: width, height: height))
            UIColor(red: 0.98, green: 0.45, blue: 0.36, alpha: 1).setFill()
            context.fill(CGRect(x: 0, y: 0, width: width / 2, height: height / 2))
        }
    }

    /// 0.25 s of a 440 Hz sine, 16-bit mono PCM WAV.
    static func wav(sampleRate: Int = 8000, seconds: Double = 0.25) -> Data {
        let frames = Int(Double(sampleRate) * seconds)
        var pcm = Data(capacity: frames * 2)
        for i in 0..<frames {
            let sample = Int16(sin(2 * .pi * 440 * Double(i) / Double(sampleRate)) * 12000)
            withUnsafeBytes(of: sample.littleEndian) { pcm.append(contentsOf: $0) }
        }
        var out = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { out.append(contentsOf: $0) } }
        out.append(contentsOf: Array("RIFF".utf8)); u32(UInt32(36 + pcm.count))
        out.append(contentsOf: Array("WAVE".utf8))
        out.append(contentsOf: Array("fmt ".utf8)); u32(16); u16(1); u16(1)
        u32(UInt32(sampleRate)); u32(UInt32(sampleRate * 2)); u16(2); u16(16)
        out.append(contentsOf: Array("data".utf8)); u32(UInt32(pcm.count))
        out.append(pcm)
        return out
    }

    /// The first entry of a ZIP archive (stored or deflated), decompressed.
    static func firstZipEntry(_ zip: Data) -> (name: String, data: Data)? {
        let bytes = [UInt8](zip)
        guard bytes.count > 30, bytes[0] == 0x50, bytes[1] == 0x4B, bytes[2] == 0x03, bytes[3] == 0x04 else { return nil }
        func u16(_ o: Int) -> Int { Int(bytes[o]) | Int(bytes[o + 1]) << 8 }
        func u32(_ o: Int) -> Int { u16(o) | u16(o + 2) << 16 }
        let flags = u16(6), method = u16(8)
        var compressed = u32(18)
        let size = u32(22), nameLength = u16(26), extraLength = u16(28)
        let nameStart = 30, dataStart = 30 + nameLength + extraLength
        guard bytes.count >= dataStart else { return nil }
        let name = String(decoding: bytes[nameStart..<nameStart + nameLength], as: UTF8.self)
        var expected = size
        if flags & 0x08 != 0 || compressed == 0 {
            // Sizes follow in the data descriptor; take them from the central directory.
            guard let central = centralEntrySizes(bytes) else { return nil }
            compressed = central.compressed
            expected = central.size
        }
        guard bytes.count >= dataStart + compressed else { return nil }
        let payload = Data(bytes[dataStart..<dataStart + compressed])
        switch method {
        case 0: return (name, payload)
        case 8: return inflate(payload, expectedSize: expected).map { (name, $0) }
        default: return nil
        }
    }

    private static func centralEntrySizes(_ bytes: [UInt8]) -> (compressed: Int, size: Int)? {
        guard bytes.count > 46 else { return nil }
        for o in 0..<(bytes.count - 46) where bytes[o] == 0x50 && bytes[o + 1] == 0x4B && bytes[o + 2] == 0x01 && bytes[o + 3] == 0x02 {
            func u32(_ p: Int) -> Int { Int(bytes[p]) | Int(bytes[p + 1]) << 8 | Int(bytes[p + 2]) << 16 | Int(bytes[p + 3]) << 24 }
            return (u32(o + 20), u32(o + 24))
        }
        return nil
    }

    /// Raw DEFLATE through the system Compression framework.
    static func inflate(_ data: Data, expectedSize: Int) -> Data? {
        let capacity = max(expectedSize, 1)
        var out = Data(count: capacity)
        let written = out.withUnsafeMutableBytes { dst in
            data.withUnsafeBytes { src in
                compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, capacity,
                                          src.bindMemory(to: UInt8.self).baseAddress!, data.count, nil, COMPRESSION_ZLIB)
            }
        }
        guard written == expectedSize else { return nil }
        return out.prefix(written)
    }
}
