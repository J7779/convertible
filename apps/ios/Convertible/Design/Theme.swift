// Design tokens (tokens.json) as SwiftUI values. Colours come from the asset
// catalogue (written from tokens.json by scripts/generate-assets.py), with light and dark variants.
import SwiftUI

enum Palette {
    static let background = Color("background")
    static let surface = Color("surface")
    static let surfaceElevated = Color("surfaceElevated")
    static let surfaceSunken = Color("surfaceSunken")
    static let border = Color("border")
    static let borderStrong = Color("borderStrong")
    static let textPrimary = Color("textPrimary")
    static let textSecondary = Color("textSecondary")
    static let textDisabled = Color("textDisabled")
    static let accent = Color("accent")
    static let accentText = Color("accentText")
    static let accentFill = Color("accentFill")
    static let accentFillPressed = Color("accentFillPressed")
    static let onAccent = Color("onAccent")
    static let accentSubtle = Color("accentSubtle")
    static let success = Color("success")
    static let successSubtle = Color("successSubtle")
    static let warning = Color("warning")
    static let warningSubtle = Color("warningSubtle")
    static let error = Color("error")
    static let errorSubtle = Color("errorSubtle")
    static let focusRing = Color("focusRing")
    static let scrim = Color("scrim")

    static func category(_ id: String) -> Color { Color("category-\(FileCategory.known(id))") }

    /// Flux, left to right, for horizontal elements.
    static let flux = LinearGradient(colors: [Color("flux-0"), Color("flux-1"), Color("flux-2")],
                                     startPoint: .leading, endPoint: .trailing)
}

enum Radius {
    static let xs: CGFloat = 4
    static let sm: CGFloat = 8
    static let md: CGFloat = 12
    static let lg: CGFloat = 16
    static let xl: CGFloat = 24
    static let xxl: CGFloat = 32
}

enum Space {
    static let s1: CGFloat = 2
    static let s2: CGFloat = 4
    static let s3: CGFloat = 8
    static let s4: CGFloat = 12
    static let s5: CGFloat = 16
    static let s6: CGFloat = 20
    static let s7: CGFloat = 24
    static let s8: CGFloat = 32
    static let s9: CGFloat = 40
    static let s10: CGFloat = 48
    static let s11: CGFloat = 64
}

enum Sizes {
    static let touchTarget: CGFloat = 44
    static let controlMd: CGFloat = 40
    static let controlLg: CGFloat = 48
}

/// The type scale from tokens.json, scaled with Dynamic Type.
enum TextStyle {
    case display, title1, title2, title3, bodyLarge, body, bodyStrong, label, caption, overline, mono, formatLabel

    private var spec: (size: CGFloat, weight: String, tracking: CGFloat, relative: Font.TextStyle, mono: Bool) {
        switch self {
        case .display: return (40, "SemiBold", -0.022, .largeTitle, false)
        case .title1: return (30, "SemiBold", -0.019, .title, false)
        case .title2: return (24, "SemiBold", -0.015, .title2, false)
        case .title3: return (20, "SemiBold", -0.011, .title3, false)
        case .bodyLarge: return (17, "Regular", -0.006, .body, false)
        case .body: return (15, "Regular", -0.003, .subheadline, false)
        case .bodyStrong: return (15, "Medium", -0.003, .subheadline, false)
        case .label: return (13, "Medium", 0, .footnote, false)
        case .caption: return (12, "Regular", 0.002, .caption, false)
        case .overline: return (11, "SemiBold", 0.06, .caption2, false)
        case .mono: return (13, "Regular", 0, .footnote, true)
        case .formatLabel: return (11, "Medium", 0.04, .caption2, true)
        }
    }

    var font: Font {
        let s = spec
        let name = s.mono ? "JetBrainsMonoNL-\(s.weight == "SemiBold" ? "Medium" : s.weight)" : "Inter-\(s.weight)"
        return Font.custom(name, size: s.size, relativeTo: s.relative)
    }

    var tracking: CGFloat { spec.size * spec.tracking }
    var uppercase: Bool { self == .overline || self == .formatLabel }
}

extension View {
    func textStyle(_ style: TextStyle) -> some View {
        font(style.font).tracking(style.tracking).textCase(style.uppercase ? .uppercase : nil)
    }

    /// A resting card: surface fill, hairline border, level-1 elevation.
    func card(padding: CGFloat = Space.s5) -> some View {
        modifier(CardModifier(padding: padding))
    }
}

private struct CardModifier: ViewModifier {
    let padding: CGFloat
    @Environment(\.colorScheme) private var scheme

    func body(content: Content) -> some View {
        content
            .padding(padding)
            .background(
                RoundedRectangle(cornerRadius: Radius.lg, style: .continuous)
                    .fill(scheme == .dark ? Palette.surface : Palette.surface)
            )
            .overlay(
                RoundedRectangle(cornerRadius: Radius.lg, style: .continuous)
                    .strokeBorder(Palette.border, lineWidth: 1)
            )
            .shadow(color: scheme == .dark ? .black.opacity(0.4) : Color(hex: 0x12141C, opacity: 0.08), radius: 1, x: 0, y: 1)
    }
}

/// The one primary action style: accent fill, white label, 48 pt tall.
struct PrimaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .textStyle(.bodyStrong)
            .foregroundStyle(Palette.onAccent)
            .padding(.horizontal, Space.s6)
            .frame(minHeight: Sizes.controlLg)
            .frame(maxWidth: .infinity)
            .background(
                RoundedRectangle(cornerRadius: Radius.md, style: .continuous)
                    .fill(configuration.isPressed ? Palette.accentFillPressed : Palette.accentFill)
            )
            .opacity(isEnabled ? 1 : 0.45)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(Motion.standard(Motion.fast), value: configuration.isPressed)
            .contentShape(Rectangle())
    }
}

/// Secondary actions: subtle surface with a border.
struct SecondaryButtonStyle: ButtonStyle {
    var compact = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .textStyle(.label)
            .foregroundStyle(Palette.textPrimary)
            .padding(.horizontal, compact ? Space.s4 : Space.s5)
            .frame(minHeight: Sizes.touchTarget)
            .background(
                RoundedRectangle(cornerRadius: Radius.md, style: .continuous)
                    .fill(configuration.isPressed ? Palette.surfaceSunken : Palette.surfaceElevated)
            )
            .overlay(
                RoundedRectangle(cornerRadius: Radius.md, style: .continuous)
                    .strokeBorder(Palette.border, lineWidth: 1)
            )
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(Motion.standard(Motion.fast), value: configuration.isPressed)
            .contentShape(Rectangle())
    }
}

/// Small format badge: round dot for a source, square dot for a target (echoing the mark).
struct FormatBadge: View {
    enum Shape { case source, target }
    let label: String
    let category: String
    var shape: Shape = .source

    var body: some View {
        let color = Palette.category(category)
        HStack(spacing: Space.s2) {
            Group {
                if shape == .source { Circle().fill(color) } else { Rectangle().fill(color) }
            }
            .frame(width: 7, height: 7)
            Text(label)
                .textStyle(.formatLabel)
                .foregroundStyle(color)
                .lineLimit(1)
        }
        .padding(.horizontal, Space.s3)
        .padding(.vertical, Space.s2)
        .background(Capsule().fill(color.opacity(0.12)))
        .overlay(Capsule().strokeBorder(color.opacity(0.35), lineWidth: 1))
    }
}
