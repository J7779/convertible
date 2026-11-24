// Convertible core - mesh family: registration of gltf, glb, 3mf, amf.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_gltf(Registry& registry);
void register_3mf_and_amf(Registry& registry);

void register_scene_formats(Registry& registry) {
  register_gltf(registry);
  register_3mf_and_amf(registry);
}

}  // namespace cvt::mesh
