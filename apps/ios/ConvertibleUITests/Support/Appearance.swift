// Reads the rendered background from a screenshot, to tell light from dark.
import UIKit
import XCTest

enum Appearance {
    /// Mean relative luminance (0 = black, 1 = white) of a thin strip along the left
    /// edge of the window, between 30 % and 90 % of its height: always background.
    static func backgroundLuminance(of app: XCUIApplication) -> Double {
        guard let image = app.screenshot().image.cgImage else { return -1 }
        let width = image.width, height = image.height
        let bytesPerRow = width * 4
        var pixels = [UInt8](repeating: 0, count: bytesPerRow * height)
        let drawn = pixels.withUnsafeMutableBytes { raw -> Bool in
            guard let context = CGContext(data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8,
                                          bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard drawn else { return -1 }
        var total = 0.0, count = 0.0
        let x = max(2, width / 200)
        for y in stride(from: height * 3 / 10, to: height * 9 / 10, by: max(1, height / 100)) {
            let o = y * bytesPerRow + x * 4
            let r = Double(pixels[o]) / 255, g = Double(pixels[o + 1]) / 255, b = Double(pixels[o + 2]) / 255
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
            count += 1
        }
        return count > 0 ? total / count : -1
    }
}
