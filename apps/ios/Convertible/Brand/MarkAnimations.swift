// Native renderings of the brand motion: the intro, the indeterminate spinner
// and the determinate progress mark, all driven by animation.json.
import SwiftUI

/// Draws one frame of a storyboard timeline into a Canvas context.
struct StoryboardRenderer {
    let storyboard: Storyboard
    let timeline: Storyboard.Timeline
    let colors: [Color]

    private var flux: Gradient { Gradient(colors: colors) }

    func draw(_ context: GraphicsContext, size: CGSize, time: Double, opacityScale: Double = 1) {
        var ctx = context
        let fit = MarkGeometry.fit(CGRect(origin: .zero, size: size))
        ctx.concatenate(fit)
        ctx.opacity = opacityScale
        let drawn = Set(timeline.layers.flatMap { $0.children ?? [] })
        for layer in timeline.layers where !drawn.contains(layer.id) {
            draw(layer, in: ctx, time: time)
        }
    }

    private func draw(_ layer: Storyboard.Layer, in context: GraphicsContext, time: Double) {
        var ctx = context
        let opacity = storyboard.value("opacity", of: layer, at: time)
        guard opacity > 0.001 else { return }
        ctx.opacity *= opacity
        let scale = storyboard.value("scale", of: layer, at: time)
        let rotation = storyboard.value("rotationDeg", of: layer, at: time)
        if let pivot = layer.pivot, pivot.count == 2, scale != 1 || rotation != 0 {
            ctx.translateBy(x: pivot[0], y: pivot[1])
            ctx.rotate(by: .degrees(rotation))
            ctx.scaleBy(x: scale, y: scale)
            ctx.translateBy(x: -pivot[0], y: -pivot[1])
        } else if layer.pivot == nil, scale != 1 {
            ctx.scaleBy(x: scale, y: scale)
        }
        guard scale > 0.0001 else { return }

        switch layer.type {
        case "group":
            for childID in layer.children ?? [] {
                if let child = timeline.layer(childID) { draw(child, in: ctx, time: time) }
            }
        case "stroke":
            let start = storyboard.value("trimStart", of: layer, at: time)
            let end = storyboard.value("trimEnd", of: layer, at: time)
            let path = MarkGeometry.stroke(from: start, to: end)
            ctx.fill(path, with: shading(layer.stroke))
        case "shape":
            let path: Path
            switch layer.geometry {
            case "endSquare": path = MarkGeometry.endSquare
            default: path = MarkGeometry.seed
            }
            ctx.fill(path, with: shading(layer.fill))
        default:
            break
        }
    }

    private func shading(_ paint: String?) -> GraphicsContext.Shading {
        switch paint {
        case "iris": return .color(colors[0])
        case "coral": return .color(colors[2])
        default:
            return .linearGradient(flux, startPoint: CGPoint(x: 0, y: MarkGeometry.gradientStartY),
                                   endPoint: CGPoint(x: 0, y: MarkGeometry.gradientEndY))
        }
    }
}

/// The 1.8 s logo intro. Calls `onFinished` at the end; tap to skip.
struct IntroAnimationView: View {
    var onFinished: () -> Void
    @Environment(\.colorScheme) private var scheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var start = Date()
    @State private var done = false

    var body: some View {
        Group {
            if reduceMotion || Storyboard.shared == nil {
                LogoMark()
            } else if let storyboard = Storyboard.shared {
                TimelineView(.animation(paused: done)) { timeline in
                    let elapsed = timeline.date.timeIntervalSince(start) * 1000
                    Canvas { context, size in
                        StoryboardRenderer(storyboard: storyboard, timeline: storyboard.intro,
                                           colors: MarkGeometry.fluxColors(scheme))
                            .draw(context, size: size, time: min(elapsed, storyboard.intro.durationMs))
                    }
                }
            }
        }
        .onAppear {
            start = Date()
            let duration = reduceMotion ? 0.6 : (Storyboard.shared?.intro.durationMs ?? 1800) / 1000
            DispatchQueue.main.asyncAfter(deadline: .now() + duration) {
                guard !done else { return }
                done = true
                onFinished()
            }
        }
    }
}

/// Indeterminate "converting" spinner (1.4 s loop); a slow pulse with reduced motion.
struct SpinnerMark: View {
    @Environment(\.colorScheme) private var scheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var pulse = false

    var body: some View {
        Group {
            if reduceMotion || Storyboard.shared == nil {
                LogoMark()
                    .opacity(pulse ? 0.45 : 1)
                    .onAppear {
                        withAnimation(Motion.inOut.animation(0.8).repeatForever(autoreverses: true)) { pulse = true }
                    }
            } else if let storyboard = Storyboard.shared {
                TimelineView(.animation) { timeline in
                    let ms = timeline.date.timeIntervalSinceReferenceDate * 1000
                    let t = ms.truncatingRemainder(dividingBy: storyboard.spinner.durationMs)
                    Canvas { context, size in
                        StoryboardRenderer(storyboard: storyboard, timeline: storyboard.spinner,
                                           colors: MarkGeometry.fluxColors(scheme))
                            .draw(context, size: size, time: t)
                    }
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .accessibilityElement()
        .accessibilityLabel("Working")
    }
}

/// Determinate progress: the mark's stroke trimmed to `progress`, over a faint track.
/// At 1 it is the finished logo; `celebrate` plays the landing ripple once.
struct ProgressMark: View {
    var progress: Double
    var celebrate = false
    @Environment(\.colorScheme) private var scheme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var ripple = false

    var body: some View {
        let style = MarkGeometry.fluxStyle(scheme)
        ZStack {
            Group {
                MarkStrokeShape().fill(style)
                MarkSeedShape().fill(style)
            }
            .opacity(0.16)
            if celebrate && !reduceMotion {
                MarkEndSquareShape(scale: ripple ? 1.6 : 1)
                    .fill(MarkGeometry.fluxColors(scheme)[2])
                    .opacity(ripple ? 0 : 0.45)
            }
            MarkStrokeShape(trimStart: 0, trimEnd: progress)
                .fill(style)
                .animation(reduceMotion ? nil : Motion.standard(Motion.base), value: progress)
            MarkSeedShape(scale: progress > 0 ? 1 : 0)
                .fill(style)
                .animation(reduceMotion ? nil : Motion.standard(Motion.base), value: progress > 0)
        }
        .aspectRatio(1, contentMode: .fit)
        .onAppear { startRipple() }
        .onChange(of: celebrate) { _ in startRipple() }
        .accessibilityElement()
        .accessibilityLabel(progress >= 1 ? "Done" : "Progress")
        .accessibilityValue("\(Int((progress * 100).rounded())) percent")
    }

    private func startRipple() {
        guard celebrate, !reduceMotion else { return }
        ripple = false
        withAnimation(Motion.decelerate.animation(0.48)) { ripple = true }
    }
}
