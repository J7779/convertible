// Convertible core - mesh family: registration of usda, usdz, x3d, wrl.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_x3d_vrml(Registry& registry);
void register_usd(Registry& registry);

void register_usd_x3d_formats(Registry& registry) {
  register_x3d_vrml(registry);
  register_usd(registry);
}

}  // namespace cvt::mesh
