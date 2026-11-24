# Convertible brand assets

Everything needed to put the Convertible identity on iOS, Android, the web and the desktop apps.
Vector files in this directory are the sources. Every raster is generated from them by `build-icons.sh`.

## Concept

The mark is a capital C that changes format as it is drawn. It starts as a round dot at the top right,
sweeps through a quarter circle, then turns a hard right angle and ends in a square cut.
One stroke, one thing becoming another: round in, square out. The colour follows the same idea.
The round arm is Iris, the square arm is Coral, and the two blend only in the stem between them.

Construction, on the 1024 unit canvas of `logo.svg`:

| Element | Value |
| --- | --- |
| Stroke weight ("the bar") | 200 |
| Centreline | `M772 260 H512 A252 252 0 0 0 260 512 V764 H864` |
| Round start | circle, centre 772,260, radius 100 |
| Outer / inner corner radius, top left | 352 / 152 |
| Bottom left and both ends of the bottom arm | square, no rounding |
| Gradient | vertical, Iris at y 360, Orchid at y 512, Coral at y 664, padded beyond |
| Bounds | x 160 to 872, y 160 to 864 |

The round end overshoots the square end by 8 units so both arms look the same length.

## Files

| File | Use |
| --- | --- |
| `logo.svg` | Primary mark. Works on dark and on mid-tone backgrounds. |
| `logo-dark.svg` | Same tones as `logo.svg`, named for use in dark/light pairs on dark backgrounds. |
| `logo-light.svg` | Deeper tones for white and light backgrounds (each colour at least 3:1 on white). |
| `logo-mono.svg` | Single colour, `currentColor`. For one-colour print, watermarks, tinted UI. |
| `logo-small.svg` | Heavier cut drawn on a 16 px grid. Use below 40 px. |
| `logo-wordmark.svg` | Mark plus "Convertible" (outlined Inter Display SemiBold). Follows `prefers-color-scheme`. |
| `logo-wordmark-dark.svg`, `logo-wordmark-light.svg` | Fixed versions for dark and for light backgrounds. |
| `logo-animated.svg` | 1.8 s intro animation, CSS only, ends on the static mark. |
| `spinner.svg` | 1.4 s looping "converting" indicator, CSS only. |
| `animation.json` | Machine-readable storyboard for the intro, the spinner and determinate progress. |
| `tokens.json`, `tokens.css` | Design tokens. The JSON is the source of truth, the CSS mirrors it. |
| `readme-banner.svg`, `readme-banner.png` | README hero (1280 x 400, PNG at 2x). A self-contained dark card, so it reads on both GitHub themes. |
| `social-preview.png` | 1280 x 640 repository card. Source: `src/social-preview.svg`. |
| `src/` | Icon sources (see `build-icons.sh`). |
| `icons/` | Generated icons, plus the hand-written `favicon.svg` and the Android vector layers. |

### Which icon goes where

| Platform | Files |
| --- | --- |
| iOS | `icons/ios/AppIcon-1024.png` (opaque, square corners; the system applies the mask). |
| Android | `icons/android/ic_launcher_{background,foreground,monochrome}.xml` into `res/drawable/`, `ic_launcher.xml` into `res/mipmap-anydpi-v26/` (also as `ic_launcher_round.xml`). The `.svg` twins are the editable sources. Play Store listing: `icons/icon-512.png`. |
| Web / PWA | `icons/favicon.svg`, `icons/favicon.ico`, `icons/icon-180.png` (apple-touch-icon), `icons/icon-192.png` and `icons/icon-512.png` (purpose `any`), `icons/icon-maskable-192.png` and `icons/icon-maskable-512.png` (purpose `maskable`). |
| Windows | `icons/icon.ico` (16 to 256, rounded tile). |
| macOS | `icons/icon.icns` (tile with the standard margin and shadow). |
| Linux | `icons/desktop/icon-{16,24,32,48,64,128,256,512}.png` (rounded tile, transparent corners) for the hicolor theme and window icons. |
| Anywhere a square, full-bleed icon is needed | `icons/icon-{16,24,32,48,64,128,180,192,256,384,512,1024}.png`. |

Sizes of 32 px and below are rendered from the 16 px grid cut, so the bars land on whole pixels.

## Usage

Do

- Keep clear space of one bar (the stroke weight, 28% of the mark height) on every side. In the wordmark the same rule applies around the whole lockup.
- Use the gradient mark on Ink or other dark neutrals, `logo-light.svg` on white and light neutrals, and `logo-mono.svg` wherever only one colour is available.
- Use `logo-small.svg` (or the small icon cuts) below 40 px. Minimum size is 16 px for the mark and 96 px wide for the wordmark.
- Scale uniformly.

Don't

- Don't rotate, mirror, skew, outline or add shadows and glows to the mark.
- Don't recolour the gradient, reverse it, or run it along a different axis. Round is Iris, square is Coral.
- Don't round the square corner or square off the round end. The contrast between them is the idea.
- Don't place the gradient mark on saturated or busy backgrounds. Use the mono mark there.
- Don't rebuild the wordmark in another typeface or change the spacing between mark and word.
- Don't use the Coral brand colour for error states. The UI has its own `error` token.

## Palette

Brand

| Name | Hex | Role | Contrast |
| --- | --- | --- | --- |
| Iris | `#7C6CFF` | Round arm, accent | 4.96:1 on Ink, 3.86:1 on white |
| Orchid | `#D662B0` | Midpoint of the gradient | 5.68:1 on Ink |
| Coral | `#FF6452` | Square arm | 6.56:1 on Ink, 2.92:1 on white (use Coral Deep on white) |
| Ink | `#0D0F16` | Dark background | |
| Paper | `#F6F6FA` | Light background | |
| Iris Deep / Orchid Deep / Coral Deep | `#6654F0` / `#C94FA3` / `#F04A36` | Gradient on light backgrounds | 5.13 / 4.09 / 3.66:1 on white |

The signature gradient ("Flux") is Iris, Orchid, Coral at 0%, 50%, 100%. It runs top to bottom in the mark and left to right in horizontal UI such as progress bars.
The icon background is a vertical gradient from `#1B1E2C` to `#0A0B11` with a soft Iris glow from the top left.

UI colours (full list in `tokens.json`, ratios under `contrast`)

| Token | Dark | Ratio | Light | Ratio |
| --- | --- | --- | --- | --- |
| background | `#0D0F16` | | `#F6F6FA` | |
| surface | `#151823` | | `#FFFFFF` | |
| surfaceElevated | `#1D2130` | | `#FFFFFF` | |
| border | `#2B3045` | decorative | `#E1E3EC` | decorative |
| borderStrong | `#646C8E` | 3.11:1 on elevated | `#858BA3` | 3.13:1 on background |
| textPrimary | `#F3F4F9` | 17.43 / 16.11 / 14.57 | `#12141C` | 17.05 / 18.38 |
| textSecondary | `#A9AFC6` | 8.78 / 8.12 / 7.34 | `#545A72` | 6.32 / 6.81 |
| textDisabled | `#676D87` | 3.75 (exempt) | `#9499AE` | 2.62 (exempt) |
| accent (graphics) | `#7C6CFF` | 4.96 / 4.59 / 4.15 | `#6654F0` | 4.76 / 5.13 |
| accentText | `#A79DFF` | 8.12 / 7.51 / 6.79 | `#5746E0` | 5.80 / 6.25 |
| accentFill + onAccent `#FFFFFF` | `#6C5AF3` | 4.78 | `#5746E0` | 6.25 |
| success | `#3DD68C` | 10.21 / 9.43 / 8.53 | `#137C47` | 4.87 / 5.25 |
| warning | `#F5B83D` | 10.76 / 9.95 / 9.00 | `#8F5B00` | 5.32 / 5.73 |
| error | `#FF6B81` | 6.99 / 6.46 / 5.85 | `#C42547` | 5.26 / 5.67 |
| focusRing | `#A79DFF` | 8.12 / 7.51 / 6.79 | `#5746E0` | 5.80 / 6.25 |

Ratios are WCAG 2.1 contrast against background / surface / surfaceElevated (dark) and background / surface (light).
All text colours meet AA (4.5:1) and all non-text indicators meet 3:1. Disabled text is exempt by the standard.

File categories each have a dark-theme and a light-theme accent under `category` in `tokens.json`
(image, audio, video, document, ebook, spreadsheet, presentation, data, archive, font, subtitle, model3d, geo, pim, code, encoding, vector, other).
Every one is at least 7.4:1 on the dark surfaces and at least 4.7:1 on the light ones, so they can be used for labels as well as icons.
In conversion chips the source format gets a round dot and the target a square one, echoing the mark.

## Typography

- UI: Inter (Regular 400, Medium 500, SemiBold 600), bundled in `assets/fonts/`. The desktop app loads these files directly. Mobile and web may use them or fall back to the system font through the stack in `tokens.json`.
- Format labels, file names, code: JetBrains Mono NL (Regular, Medium), the build without ligatures so `->` and `!=` in file names stay literal.
- Both are licensed under the SIL Open Font License 1.1. The licence texts sit next to the font files.
- The type scale, weights and tracking are in `tokens.json` under `typography`.

## Motion

Intro (`logo-animated.svg`, 1800 ms, plays once)

1. 0 to 380 ms. The round dot pops in at the top right with a small overshoot, `cubic-bezier(0.34, 1.56, 0.64, 1)`. A round ripple expands behind it and fades (60 to 600 ms).
2. 120 to 900 ms. A ghost of the stroke at 26% opacity runs ahead along the path, `cubic-bezier(0.4, 0, 0.2, 1)`.
3. 260 to 1240 ms. The full-colour stroke follows from the dot, around the curve, through the square corner, to the end of the bottom arm. Same easing. The gradient is fixed to the canvas, so the stroke turns from Iris to Coral as it descends.
4. 1180 to 1720 ms. A square ripple expands from the square end as the stroke lands, `cubic-bezier(0.16, 1, 0.3, 1)`.
5. Throughout, the whole mark settles from 86% scale and -8 degrees to rest (0 to 1200 ms, `cubic-bezier(0.22, 0.61, 0.36, 1)`), then gives a 3% bump on landing (1200 to 1350 to 1800 ms).

The last frame is identical to `logo.svg`.

Spinner (`spinner.svg`, 1400 ms loop). Over a faint track of the mark, the stroke draws from the round start to the square end (0 to 630 ms), holds, then erases from the start (770 to 1400 ms), both with `cubic-bezier(0.65, 0, 0.35, 1)`. The dot pops in at the start of each loop and shrinks away as the erase begins.

Determinate progress uses the same geometry: the stroke is trimmed to the progress fraction, so a finished conversion is the finished logo.

With reduced motion enabled the intro shows the static mark and the spinner becomes a slow opacity pulse.

`animation.json` holds the exact geometry, layers, keyframe times, values and cubic-bezier control points for native implementations, with notes for SwiftUI, Compose, Dear ImGui and CSS.

## Regenerating the rasters

```sh
./build-icons.sh
```

The script needs ImageMagick 7 and one SVG renderer: `resvg`, `rsvg-convert`, Inkscape, or Python with `resvg_py` or `cairosvg`.
The first one found is used, and `SVG_RENDERER=...` forces a choice. ImageMagick's own SVG renderer is deliberately not used,
because it renders the gradients in these files incorrectly.
On macOS `iconutil` builds `icon.icns`; elsewhere a small Python fallback writes the same container.
Output PNGs carry no metadata chunks, so rebuilding with the same renderer gives identical files.

Edit only the SVG sources. `tokens.css` must be kept in step with `tokens.json` by hand or by script when tokens change.
