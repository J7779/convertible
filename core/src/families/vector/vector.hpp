// Convertible core - vector family: internal registration entry points.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::vector {

/// svg, svgz (svg_*.cpp).
void register_svg(Registry& registry);
/// eps, ps, ai (PostScript-based) (ps_*.cpp).
void register_postscript(Registry& registry);
/// wmf, emf, wmz, emz (metafile_*.cpp).
void register_metafiles(Registry& registry);
/// dxf, hpgl/plt, cgm (cad_*.cpp).
void register_cad(Registry& registry);
/// Android VectorDrawable, TikZ, Lottie, PICT ... (misc_*.cpp).
void register_misc_vector(Registry& registry);

}  // namespace cvt::vector
