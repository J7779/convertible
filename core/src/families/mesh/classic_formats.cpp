// Convertible core - mesh family: registration of off, obj (+ obj.zip), stl, ply.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_off(Registry& registry);
void register_obj(Registry& registry);
void register_stl(Registry& registry);
void register_ply(Registry& registry);

void register_classic_formats(Registry& registry) {
  register_obj(registry);
  register_stl(registry);
  register_ply(registry);
  register_off(registry);
}

}  // namespace cvt::mesh
