// Convertible core - mesh family: registration of 3ds, lwo, md2, md3, ms3d, x, vox.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_3ds(Registry& registry);
void register_game_readers(Registry& registry);

void register_game_formats(Registry& registry) {
  register_3ds(registry);
  register_game_readers(registry);
}

}  // namespace cvt::mesh
