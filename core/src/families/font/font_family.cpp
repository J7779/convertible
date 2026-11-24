// Convertible core - font family.
// Fonts: sfnt (ttf, otf, ttc/otc), woff, woff2, eot, Mac suitcases, bare CFF,
// PostScript Type 1 (pfa, pfb, afm), bitmap fonts (bdf, pcf, psf, fnt, fon,
// otb) and SVG fonts, with outline conversion, subsetting and specimens.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/font/font.hpp"

namespace cvt {

void register_font_family(Registry& registry) {
  font::register_sfnt_formats(registry);
  font::register_type1_formats(registry);
  font::register_bitmap_formats(registry);
  font::register_svg_font(registry);
  font::register_font_bridges(registry);
}

}  // namespace cvt
