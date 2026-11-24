// Convertible core - vector family.
// Vector graphics (svg, eps, emf/wmf, ...) and rasterisation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/vector/vector.hpp"

namespace cvt {

void register_vector_family(Registry& registry) {
  vector::register_svg(registry);
  vector::register_postscript(registry);
  vector::register_metafiles(registry);
  vector::register_cad(registry);
  vector::register_misc_vector(registry);
}

}  // namespace cvt
