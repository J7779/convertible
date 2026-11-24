// Convertible core - vector family: CAD and plotter formats (dxf, dwg detection, hpgl, cgm).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/vector/cad_internal.hpp"
#include "families/vector/vector.hpp"

namespace cvt::vector {

void register_cad(Registry& registry) {
  cad::register_dxf(registry);
  cad::register_dwg(registry);
  cad::register_hpgl(registry);
  cad::register_cgm(registry);
}

}  // namespace cvt::vector
