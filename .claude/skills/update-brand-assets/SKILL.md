---
name: update-brand-assets
description: Change Convertible's brand assets in assets/brand and assets/fonts - logo SVG sources, generated icons for every platform, design tokens (tokens.json and tokens.css), the motion storyboard (animation.json), the README banner - and propagate them to the apps. Use when a colour, icon, logo, font or animation changes.
---

# Update brand assets

Source of truth: `assets/brand/README.md` (concept, files, palette, typography, motion).
Vector files and `tokens.json` are the sources; every raster is generated.

## Steps

1. **Edit only sources**: the SVGs in `assets/brand/` and `assets/brand/src/`, `tokens.json`,
   `animation.json`. Keep the construction rules (stroke 200, round start, square end, Iris to
   Coral gradient) and the contrast targets in the README (text AA 4.5:1, indicators 3:1);
   update the contrast table when colours change.
2. **Mirror the tokens**: keep `assets/brand/tokens.css` in step with `tokens.json`.
3. **Regenerate rasters** in a container (ImageMagick 7 and an SVG renderer such as `resvg`
   or `rsvg-convert`; never ImageMagick's own SVG renderer):
   ```sh
   docker run --rm --memory 6g -v "$PWD:/src" -w /src/assets/brand debian:trixie bash -c '
     apt-get update -qq && apt-get install -y -qq --no-install-recommends imagemagick librsvg2-bin python3 >/dev/null &&
     SVG_RENDERER=rsvg-convert ./build-icons.sh'
   ```
   Output PNGs carry no metadata chunks; rebuilding with the same renderer is byte-identical.
4. **Propagate to the apps**:
   - iOS: `python3 -I apps/ios/scripts/generate-assets.py` (colour sets from `tokens.json`); the
     app icon is a symlink to `assets/brand/icons/ios/AppIcon-1024.png`.
   - Android: copied at build time by the `generateBrandResources` Gradle task; just rebuild.
   - Web: copied by `apps/web/tools/assemble.mjs`; rebuild with `apps/web/build.sh`.
   - Desktop: embedded at build time; rebuild.
5. **Fonts**: only unmodified upstream files under the SIL OFL with their licence texts; update
   the table and SHA-256 list in `assets/fonts/README.md`, then `tools/gen-notices`.
6. **Check**: `git status` shows only intended files, the apps' tests still pass
   (they read the real `tokens.json` and `animation.json`), and `tools/privacy-audit` is clean
   (SVG editors like to embed absolute paths and user names in metadata: strip them).
