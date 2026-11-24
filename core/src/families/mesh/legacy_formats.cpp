// Convertible core - mesh family: registration of dae, fbx.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_collada(Registry& registry);
void register_fbx(Registry& registry);

void register_legacy_formats(Registry& registry) {
  register_collada(registry);
  register_fbx(registry);
}

}  // namespace cvt::mesh
