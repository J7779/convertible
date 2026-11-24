// Convertible core - mesh family.
// 3D models and point clouds: obj, stl, ply, off, gltf/glb, 3mf, dae, fbx,
// 3ds, x3d/wrl, amf, usd, lwo, md2/md3, ms3d, x, vox, xyz, pcd, las, vtk,
// msh, plus the Mesh -> Image preview renderer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/mesh/common.hpp"

namespace cvt {

void register_mesh_family(Registry& registry) {
  mesh::register_classic_formats(registry);
  mesh::register_point_cloud_formats(registry);
  mesh::register_scene_formats(registry);
  mesh::register_usd_x3d_formats(registry);
  mesh::register_legacy_formats(registry);
  mesh::register_game_formats(registry);
  mesh::register_mesh_bridges(registry);
}

}  // namespace cvt
