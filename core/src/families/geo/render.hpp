// Convertible core - geo family: an in-house map sketch renderer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/geo.hpp"
#include "ir/image.hpp"

namespace cvt::geo {

using Rgba = std::array<std::uint8_t, 4>;

/// "#rrggbb", "#rrggbbaa", "rrggbb" or "#rgb"; nullopt when malformed.
[[nodiscard]] std::optional<Rgba> parse_color(std::string_view text);

struct RenderOptions {
  std::uint32_t width = 1024;
  std::uint32_t height = 0;  ///< 0 = from the aspect ratio of the data
  Rgba background{247, 246, 242, 255};
  Rgba line{43, 108, 176, 255};
  Rgba fill{43, 108, 176, 64};
  Rgba point{197, 48, 48, 255};
  Rgba graticule_color{208, 206, 200, 255};
  double line_width = 2.5;  ///< pixels
  double point_radius = 5;  ///< pixels
  bool graticule = true;
};

/// Draws the features on a plain background with an optional graticule:
/// polygons filled and outlined, lines stroked, points as dots, all
/// anti-aliased, fitted to the data's bounding box with a margin (Web
/// Mercator between +-85 degrees of latitude, otherwise plate carree).
/// Feature styles override the default colours. No tiles, no network, no
/// text. Coordinates must be WGS 84.
[[nodiscard]] Result<ir::Image> render_map(const ir::Geo& geo, const RenderOptions& options, const Context& ctx);

}  // namespace cvt::geo
