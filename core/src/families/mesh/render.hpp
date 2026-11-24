// Convertible core - mesh family: software preview renderer (Mesh -> Image).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "ir/mesh.hpp"

namespace cvt::mesh {

struct RenderOptions {
  /// Output width and height in pixels (square image).
  std::uint32_t size = 512;
  /// Camera direction: "iso", "front", "back", "left", "right", "top", "bottom".
  std::string view = "iso";
  /// Extra rotation of the camera around the model (degrees), added to the view.
  float yaw = 0.0f;
  float pitch = 0.0f;
  /// Turntable frames (1 = a still picture); the camera turns 360 degrees.
  std::uint32_t frames = 1;
  /// Delay of each turntable frame in milliseconds.
  std::uint32_t frame_delay_ms = 80;
  /// "transparent", "light", "dark" or six hexadecimal digits (RRGGBB).
  std::string background = "transparent";
  /// Samples per pixel along each axis (1..4) for anti-aliasing.
  std::uint32_t supersample = 2;
};

/// Render a shaded perspective view of `mesh`, auto-framed on its bounds.
/// Deterministic: the same input and options give the same pixels. Embedded
/// base-colour textures are decoded through `ctx.registry` when an image
/// reader for their type is registered; otherwise the material colour is
/// used. Fails with Status::Limit when the picture would exceed the pixel
/// limit and Status::InvalidArgument for a malformed background colour.
[[nodiscard]] Result<ir::Image> render_mesh(const ir::Mesh& mesh, const RenderOptions& options, const Context& ctx);

}  // namespace cvt::mesh
