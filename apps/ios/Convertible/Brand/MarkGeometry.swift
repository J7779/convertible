// Geometry of the Convertible mark on its 1024-unit canvas (see animation.json).
import SwiftUI

enum MarkGeometry {
    static let canvas: CGFloat = 1024
    static let strokeWidth: CGFloat = 200
    static let gradientStartY: CGFloat = 360
    static let gradientEndY: CGFloat = 664

    /// Centerline "M772 260 H512 A252 252 0 0 0 260 512 V764 H864".
    static let centerline: Path = {
        var p = Path()
        p.move(to: CGPoint(x: 772, y: 260))
        p.addLine(to: CGPoint(x: 512, y: 260))
        p.addArc(center: CGPoint(x: 512, y: 512), radius: 252,
                 startAngle: .degrees(270), endAngle: .degrees(180), clockwise: true)
        p.addLine(to: CGPoint(x: 260, y: 764))
        p.addLine(to: CGPoint(x: 864, y: 764))
        return p
    }()

    static let strokeStyle = StrokeStyle(lineWidth: strokeWidth, lineCap: .butt, lineJoin: .miter, miterLimit: 4)

    static let seedCenter = CGPoint(x: 772, y: 260)
    static let seedRadius: CGFloat = 100
    // Built as plain CGPaths: SwiftUI keeps `Path(ellipseIn:)`/`Path(_ rect:)` as primitives,
    // which some OS versions draw wrongly after `.applying(_:)`.
    static let seed = Path(CGPath(ellipseIn: CGRect(x: 672, y: 160, width: 200, height: 200), transform: nil))
    static let endSquare = Path(CGPath(rect: CGRect(x: 664, y: 664, width: 200, height: 200), transform: nil))

    /// Applies a transform through CGPath, so the result is always plain path elements.
    static func transformed(_ path: Path, _ transform: CGAffineTransform) -> Path {
        // An empty path would come back with NaN bounds, which Core Animation rejects.
        guard !path.isEmpty else { return Path() }
        var t = transform
        return Path(path.cgPath.copy(using: &t) ?? path.cgPath)
    }

    /// The stroked outline of the centerline between two trim fractions.
    static func stroke(from start: CGFloat, to end: CGFloat) -> Path {
        // A (near) zero-length piece has no direction; stroking it can yield NaN points.
        guard start.isFinite, end.isFinite, end - start > 0.0005 else { return Path() }
        // Materialise the outline through CGPath: a lazily stroked SwiftUI Path would be
        // re-stroked after `.applying(_:)` with the unscaled 200-unit line width.
        let trimmed = Path(centerline.trimmedPath(from: start, to: end).cgPath)
        return Path(trimmed.strokedPath(strokeStyle).cgPath)
    }

    /// Transform that fits the 1024 canvas, centred, into `rect`.
    static func fit(_ rect: CGRect) -> CGAffineTransform {
        let s = min(rect.width, rect.height) / canvas
        let dx = rect.minX + (rect.width - canvas * s) / 2
        let dy = rect.minY + (rect.height - canvas * s) / 2
        return CGAffineTransform(translationX: dx, y: dy).scaledBy(x: s, y: s)
    }

    /// Flux gradient stops: brand tones on dark, deeper tones on light backgrounds.
    static func fluxColors(_ scheme: ColorScheme) -> [Color] {
        scheme == .dark
            ? [Color(hex: 0x7C6CFF), Color(hex: 0xD662B0), Color(hex: 0xFF6452)]
            : [Color(hex: 0x6654F0), Color(hex: 0xC94FA3), Color(hex: 0xF04A36)]
    }

    /// The gradient as a SwiftUI shape style over a square frame holding the canvas.
    static func fluxStyle(_ scheme: ColorScheme) -> LinearGradient {
        LinearGradient(colors: fluxColors(scheme),
                       startPoint: UnitPoint(x: 0.5, y: gradientStartY / canvas),
                       endPoint: UnitPoint(x: 0.5, y: gradientEndY / canvas))
    }
}

/// A trimmed stroke of the mark, as a fillable (and animatable) shape.
struct MarkStrokeShape: Shape {
    var trimStart: CGFloat = 0
    var trimEnd: CGFloat = 1

    var animatableData: AnimatablePair<CGFloat, CGFloat> {
        get { AnimatablePair(trimStart, trimEnd) }
        set { trimStart = newValue.first; trimEnd = newValue.second }
    }

    func path(in rect: CGRect) -> Path {
        MarkGeometry.transformed(MarkGeometry.stroke(from: max(0, trimStart), to: min(1, trimEnd)), MarkGeometry.fit(rect))
    }
}

/// The round start of the mark, scaled about its own centre.
struct MarkSeedShape: Shape {
    var scale: CGFloat = 1
    var animatableData: CGFloat {
        get { scale }
        set { scale = newValue }
    }

    func path(in rect: CGRect) -> Path {
        let c = MarkGeometry.seedCenter
        let t = CGAffineTransform(translationX: c.x, y: c.y).scaledBy(x: scale, y: scale).translatedBy(x: -c.x, y: -c.y)
        return MarkGeometry.transformed(MarkGeometry.seed, t.concatenating(MarkGeometry.fit(rect)))
    }
}

/// The square end of the mark scaled about its centre (used for the landing ripple).
struct MarkEndSquareShape: Shape {
    var scale: CGFloat = 1
    var animatableData: CGFloat {
        get { scale }
        set { scale = newValue }
    }

    func path(in rect: CGRect) -> Path {
        let t = CGAffineTransform(translationX: 764, y: 764).scaledBy(x: scale, y: scale).translatedBy(x: -764, y: -764)
        return MarkGeometry.transformed(MarkGeometry.endSquare, t.concatenating(MarkGeometry.fit(rect)))
    }
}

/// The static logo.
struct LogoMark: View {
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        ZStack {
            MarkStrokeShape().fill(MarkGeometry.fluxStyle(scheme))
            MarkSeedShape().fill(MarkGeometry.fluxStyle(scheme))
        }
        .aspectRatio(1, contentMode: .fit)
        .accessibilityHidden(true)
    }
}

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(.sRGB, red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255, opacity: opacity)
    }
}
