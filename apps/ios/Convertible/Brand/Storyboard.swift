// The motion storyboard from assets/brand/animation.json (bundled as-is) and
// the maths to evaluate it: cubic-bezier easings and keyframe tracks.
import Foundation
import SwiftUI

/// CSS-style cubic-bezier timing curve with control points (x1, y1, x2, y2).
struct CubicBezier: Equatable {
    let x1, y1, x2, y2: Double

    init(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double) {
        self.x1 = x1; self.y1 = y1; self.x2 = x2; self.y2 = y2
    }

    init?(_ points: [Double]) {
        guard points.count == 4 else { return nil }
        self.init(points[0], points[1], points[2], points[3])
    }

    private func sample(_ t: Double, _ a: Double, _ b: Double) -> Double {
        // Bernstein form with P0 = 0 and P3 = 1.
        let u = 1 - t
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t
    }

    private func sampleDerivativeX(_ t: Double) -> Double {
        let u = 1 - t
        return 3 * u * u * x1 + 6 * u * t * (x2 - x1) + 3 * t * t * (1 - x2)
    }

    /// Eased progress for a time fraction `x` in 0...1.
    func value(at x: Double) -> Double {
        if x <= 0 { return 0 }
        if x >= 1 { return 1 }
        // Newton-Raphson, falling back to bisection.
        var t = x
        for _ in 0..<8 {
            let err = sample(t, x1, x2) - x
            if abs(err) < 1e-7 { return sample(t, y1, y2) }
            let d = sampleDerivativeX(t)
            if abs(d) < 1e-6 { break }
            t -= err / d
        }
        var lo = 0.0, hi = 1.0
        t = x
        for _ in 0..<40 {
            let v = sample(t, x1, x2)
            if abs(v - x) < 1e-7 { break }
            if v < x { lo = t } else { hi = t }
            t = (lo + hi) / 2
        }
        return sample(t, y1, y2)
    }

    var animation: (Double) -> Animation {
        { duration in Animation.timingCurve(x1, y1, x2, y2, duration: duration) }
    }
}

struct Storyboard: Decodable {
    struct Keyframe: Decodable, Equatable {
        let t: Double
        let value: Double
        let easing: String?
    }

    struct Track: Decodable {
        let property: String
        let keyframes: [Keyframe]
    }

    struct Layer: Decodable {
        let id: String
        let type: String?
        let geometry: String?
        let fill: String?
        let stroke: String?
        let pivot: [Double]?
        let children: [String]?
        let base: [String: Double]?
        let tracks: [Track]?
    }

    struct Timeline: Decodable {
        let durationMs: Double
        let loop: Bool
        let layers: [Layer]

        func layer(_ id: String) -> Layer? { layers.first { $0.id == id } }
    }

    let easings: [String: [Double]]
    let intro: Timeline
    let spinner: Timeline

    func easing(_ name: String?) -> CubicBezier? {
        guard let name, let points = easings[name] else { return nil }
        return CubicBezier(points)
    }

    /// Value of `property` of `layer` at `time` ms, following the storyboard conventions:
    /// hold the first value before the first keyframe and the last after the last one;
    /// a keyframe's easing shapes the segment to the next keyframe; no easing = hold.
    func value(_ property: String, of layer: Layer, at time: Double) -> Double {
        if let track = layer.tracks?.first(where: { $0.property == property }), !track.keyframes.isEmpty {
            return evaluate(track.keyframes, at: time)
        }
        if let base = layer.base?[property] { return base }
        switch property {
        case "opacity", "scale", "trimEnd": return 1
        default: return 0
        }
    }

    func evaluate(_ keyframes: [Keyframe], at time: Double) -> Double {
        guard let first = keyframes.first, let last = keyframes.last else { return 0 }
        if time <= first.t { return first.value }
        if time >= last.t { return last.value }
        for i in 0..<(keyframes.count - 1) {
            let a = keyframes[i], b = keyframes[i + 1]
            guard time >= a.t && time < b.t else { continue }
            guard let curve = easing(a.easing), b.t > a.t else { return a.value }
            let p = curve.value(at: (time - a.t) / (b.t - a.t))
            return a.value + (b.value - a.value) * p
        }
        return last.value
    }

    static let shared: Storyboard? = {
        guard let url = Bundle.main.url(forResource: "animation", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(Storyboard.self, from: data)
    }()
}

/// Brand easings from tokens.json, for SwiftUI animations.
enum Motion {
    static let standard = CubicBezier(0.2, 0, 0, 1)
    static let decelerate = CubicBezier(0.16, 1, 0.3, 1)
    static let inOut = CubicBezier(0.65, 0, 0.35, 1)
    static let emphasized = CubicBezier(0.7, 0, 0.2, 1)
    static let settle = CubicBezier(0.22, 0.61, 0.36, 1)

    static let fast = 0.14
    static let base = 0.22
    static let slow = 0.36
    static let slower = 0.56

    static func standard(_ duration: Double = base) -> Animation { .timingCurve(0.2, 0, 0, 1, duration: duration) }
    static func emphasized(_ duration: Double = slow) -> Animation { .timingCurve(0.7, 0, 0.2, 1, duration: duration) }
    static var spring: Animation { .spring(response: 0.38, dampingFraction: 0.82) }
}
