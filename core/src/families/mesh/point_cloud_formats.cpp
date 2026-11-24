// Convertible core - mesh family: registration of xyz/pts, pcd, las, vtk, msh.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"

namespace cvt::mesh {

void register_point_cloud_files(Registry& registry);
void register_vtk(Registry& registry);
void register_gmsh(Registry& registry);

void register_point_cloud_formats(Registry& registry) {
  register_point_cloud_files(registry);
  register_vtk(registry);
  register_gmsh(registry);
}

}  // namespace cvt::mesh
