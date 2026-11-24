// Convertible core - vector family: DXF reader/writer entry points (private to cad_*).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::cad {

/// Read a DXF drawing. Options: space (model|paper), layers (comma list),
/// max_page_size (px, 0 = true scale).
[[nodiscard]] Result<Scene> read_dxf(ReadContext& ctx);
/// Only the 3D faces of a DXF (Scene::mesh), with default options.
[[nodiscard]] Result<Scene> read_dxf_for_mesh(ReadContext& ctx);
/// Write a scene. Options: version (r12|2000|2018), units, fills.
[[nodiscard]] Result<Bytes> write_dxf(const Scene& scene, WriteContext& ctx);

}  // namespace cvt::vector::cad
