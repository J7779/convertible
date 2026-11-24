#!/usr/bin/env bash
#
# Regenerates every raster asset in this directory from the SVG sources.
#
#   ./build-icons.sh
#
# Requirements
#   - ImageMagick 7 (`magick`) for PNG clean-up and .ico assembly.
#   - One SVG renderer. The first one found is used, in this order:
#       resvg, rsvg-convert, inkscape, python3 + resvg_py, python3 + cairosvg.
#     Force a specific one with SVG_RENDERER=resvg|rsvg|inkscape|py-resvg|py-cairosvg.
#     ImageMagick's built-in SVG renderer is deliberately not used: it gets the
#     gradients in these files wrong.
#   - `iconutil` (macOS) for icon.icns. Without it a python3 fallback writes the same file.
#
# Sources (edit these, never the rasters):
#   src/icon.svg            full-bleed app icon, 48 px and up
#   src/icon-small.svg      full-bleed app icon drawn on a 16 px grid, 16 to 32 px
#   src/icon-rounded.svg    rounded tile with transparent corners, 48 px and up
#   icons/favicon.svg       rounded tile on the 16 px grid, 16 to 32 px (also shipped as is)
#   src/icon-maskable.svg   PWA maskable icon
#   src/icon-macos.svg      macOS tile with margin and shadow
#   src/social-preview.svg  repository card
#   readme-banner.svg       README hero banner

set -euo pipefail
cd "$(dirname "$0")"

have() { command -v "$1" >/dev/null 2>&1; }

if ! have magick; then
  echo "error: ImageMagick 7 (magick) is required" >&2
  exit 1
fi

pick_renderer() {
  if have resvg; then echo resvg
  elif have rsvg-convert; then echo rsvg
  elif have inkscape; then echo inkscape
  elif have python3 && python3 -c 'import resvg_py' >/dev/null 2>&1; then echo py-resvg
  elif have python3 && python3 -c 'import cairosvg' >/dev/null 2>&1; then echo py-cairosvg
  else echo none
  fi
}

RENDERER="${SVG_RENDERER:-$(pick_renderer)}"
if [ "$RENDERER" = "none" ]; then
  echo "error: no SVG renderer found. Install one of:" >&2
  echo "         resvg            (cargo install resvg, or your package manager)" >&2
  echo "         rsvg-convert     (librsvg)" >&2
  echo "         inkscape" >&2
  echo "         python3 -m pip install resvg_py   (or cairosvg)" >&2
  exit 1
fi
echo "SVG renderer: $RENDERER"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# rasterise <svg> <width> <height> <raw png>
rasterise() {
  case "$RENDERER" in
    resvg)
      resvg -w "$2" -h "$3" "$1" "$4" ;;
    rsvg)
      rsvg-convert -w "$2" -h "$3" "$1" -o "$4" ;;
    inkscape)
      inkscape "$1" --export-type=png --export-filename="$4" -w "$2" -h "$3" >/dev/null 2>&1 ;;
    py-resvg)
      python3 -c 'import sys, resvg_py
open(sys.argv[4], "wb").write(bytes(resvg_py.svg_to_bytes(svg_path=sys.argv[1], width=int(sys.argv[2]), height=int(sys.argv[3]))))' "$1" "$2" "$3" "$4" ;;
    py-cairosvg)
      python3 -c 'import sys, cairosvg
cairosvg.svg2png(url=sys.argv[1], write_to=sys.argv[4], output_width=int(sys.argv[2]), output_height=int(sys.argv[3]))' "$1" "$2" "$3" "$4" ;;
    *)
      echo "error: unknown SVG_RENDERER '$RENDERER'" >&2
      exit 1 ;;
  esac
}

# Strip every ancillary chunk (timestamps, software tags, profiles) so output is reproducible.
CLEAN=(-strip -define png:exclude-chunk=all -define png:compression-level=9)

# render <svg> <width> <height> <png>          RGBA
render() {
  rasterise "$1" "$2" "$3" "$TMP/raw.png"
  magick "$TMP/raw.png" "${CLEAN[@]}" -define png:color-type=6 "$4"
}

# render_opaque <svg> <width> <height> <png>   RGB, no alpha channel
render_opaque() {
  rasterise "$1" "$2" "$3" "$TMP/raw.png"
  magick "$TMP/raw.png" -background '#0D0F16' -alpha remove -alpha off "${CLEAN[@]}" -define png:color-type=2 "$4"
}

mkdir -p icons/ios icons/desktop

echo "App icons (full bleed)"
for s in 16 24 32; do
  render_opaque src/icon-small.svg "$s" "$s" "icons/icon-$s.png"
done
for s in 48 64 128 180 192 256 384 512 1024; do
  render_opaque src/icon.svg "$s" "$s" "icons/icon-$s.png"
done

echo "iOS"
render_opaque src/icon.svg 1024 1024 icons/ios/AppIcon-1024.png

echo "PWA maskable"
for s in 192 512; do
  render_opaque src/icon-maskable.svg "$s" "$s" "icons/icon-maskable-$s.png"
done

echo "Desktop tiles (rounded, transparent corners)"
for s in 16 24 32; do
  render icons/favicon.svg "$s" "$s" "icons/desktop/icon-$s.png"
done
for s in 48 64 128 256 512; do
  render src/icon-rounded.svg "$s" "$s" "icons/desktop/icon-$s.png"
done

echo "favicon.ico, icon.ico"
render icons/favicon.svg 48 48 "$TMP/favicon-48.png"
magick icons/desktop/icon-16.png icons/desktop/icon-32.png "$TMP/favicon-48.png" icons/favicon.ico
magick icons/desktop/icon-16.png icons/desktop/icon-24.png icons/desktop/icon-32.png \
       icons/desktop/icon-48.png icons/desktop/icon-64.png icons/desktop/icon-128.png \
       icons/desktop/icon-256.png icons/icon.ico

echo "icon.icns"
SET="$TMP/icon.iconset"
mkdir -p "$SET"
for s in 16 32 128 256 512; do
  d=$((s * 2))
  render src/icon-macos.svg "$s" "$s" "$SET/icon_${s}x${s}.png"
  render src/icon-macos.svg "$d" "$d" "$SET/icon_${s}x${s}@2x.png"
done
if have iconutil; then
  iconutil -c icns "$SET" -o icons/icon.icns
elif have python3; then
  python3 - "$SET" icons/icon.icns <<'PY'
import struct, sys
src, out = sys.argv[1], sys.argv[2]
entries = [("icp4", "icon_16x16.png"), ("icp5", "icon_32x32.png"), ("ic07", "icon_128x128.png"),
           ("ic08", "icon_256x256.png"), ("ic09", "icon_512x512.png"), ("ic10", "icon_512x512@2x.png"),
           ("ic11", "icon_16x16@2x.png"), ("ic12", "icon_32x32@2x.png"), ("ic13", "icon_128x128@2x.png"),
           ("ic14", "icon_256x256@2x.png")]
body = b""
for tag, name in entries:
    data = open(f"{src}/{name}", "rb").read()
    body += tag.encode("ascii") + struct.pack(">I", len(data) + 8) + data
open(out, "wb").write(b"icns" + struct.pack(">I", len(body) + 8) + body)
PY
else
  echo "warning: neither iconutil nor python3 found; icons/icon.icns was not rebuilt" >&2
fi

echo "Social preview and README banner"
render_opaque src/social-preview.svg 1280 640 social-preview.png
render readme-banner.svg 2560 800 readme-banner.png

echo "Done."
