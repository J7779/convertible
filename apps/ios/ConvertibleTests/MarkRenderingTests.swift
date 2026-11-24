// Rendering regression tests for the brand mark.
import SwiftUI
import XCTest
@testable import Convertible

@MainActor
final class MarkRenderingTests: XCTestCase {
    /// The stroked mark must scale with its frame (a lazily stroked Path would be
    /// re-stroked at the unscaled width after a transform and overflow the frame).
    func testMarkScalesIntoItsFrame() {
        for side in [16.0, 22.0, 44.0, 160.0] {
            let rect = CGRect(x: 0, y: 0, width: side, height: side)
            let bounds = MarkStrokeShape().path(in: rect).boundingRect
            XCTAssertEqual(bounds.minX, side * 160 / 1024, accuracy: 0.01, "side \(side)")
            XCTAssertEqual(bounds.maxX, side * 864 / 1024, accuracy: 0.01, "side \(side)")
            XCTAssertEqual(bounds.maxY, side * 864 / 1024, accuracy: 0.01, "side \(side)")
            XCTAssertTrue(rect.contains(MarkSeedShape().path(in: rect).boundingRect))
        }
        let half = MarkStrokeShape(trimStart: 0, trimEnd: 0.5).path(in: CGRect(x: 0, y: 0, width: 44, height: 44)).boundingRect
        XCTAssertLessThan(half.maxY, 44 * 864 / 1024, "a half-drawn mark stops before the bottom arm")
    }

    /// Every progress value, including the tiny first ones, gives finite geometry
    /// (a NaN outline crashes Core Animation).
    func testProgressGeometryIsAlwaysFinite() {
        let rect = CGRect(x: 0, y: 0, width: 44, height: 44)
        var values: [CGFloat] = [0, 1e-9, 1e-6, 1e-4, 0.0004, 0.0006, 0.001, 0.01, 0.17198, 0.4338, 0.60049, 0.999999, 1]
        values += stride(from: 0, through: 1, by: 0.0137).map { CGFloat($0) }
        for p in values {
            let b = MarkStrokeShape(trimStart: 0, trimEnd: p).path(in: rect).boundingRect
            if !b.isNull {
                XCTAssertTrue(b.minX.isFinite && b.minY.isFinite && b.width.isFinite && b.height.isFinite, "progress \(p): \(b)")
            }
        }
        for s in [0.0, 1e-6, 0.5, 1.2] {
            let b = MarkSeedShape(scale: s).path(in: rect).boundingRect
            if !b.isNull { XCTAssertTrue(b.width.isFinite && b.minX.isFinite, "seed scale \(s)") }
        }
    }

    func testMarksRenderToImages() throws {
        let view = HStack {
            LogoMark().frame(width: 44, height: 44)
            ProgressMark(progress: 0.5).frame(width: 44, height: 44)
            SpinnerMark().frame(width: 44, height: 44)
        }
        let renderer = ImageRenderer(content: view)
        renderer.scale = 2
        let image = try XCTUnwrap(renderer.uiImage)
        XCTAssertEqual(image.size.height, 44, accuracy: 1)
    }
}
