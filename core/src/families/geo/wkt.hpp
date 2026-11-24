// Convertible core - geo family: Well-Known Text and Well-Known Binary
// geometry codecs (OGC Simple Features / ISO SQL-MM, plus the PostGIS
// EWKT / EWKB extensions), shared by the wkt/wkb formats, CSV with
// geometry columns and the Geo -> Table bridge.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "ir/geo.hpp"
#include "util/bytes.hpp"

namespace cvt::geo {

/// WKT of one geometry: "POINT (1 2)", "POLYGON Z ((...))", "LINESTRING EMPTY".
/// `decimals` < 0 writes the shortest exact numbers; `with_z` / `with_m`
/// add Z / M ordinates when any coordinate has them (missing ones are 0).
[[nodiscard]] std::string geometry_to_wkt(const ir::Geometry& geometry, int decimals = -1, bool with_z = true,
                                          bool with_m = false);

/// Parses one WKT geometry (2D, Z, M, ZM, EMPTY, every simple-feature type,
/// TRIANGLE / TIN / POLYHEDRALSURFACE as polygons) starting at `text`,
/// with an optional EWKT "SRID=n;" prefix stored in `*srid`. `consumed`,
/// when given, receives the number of characters used, so that several
/// geometries can be read in a row; otherwise trailing text is an error.
[[nodiscard]] Result<ir::Geometry> geometry_from_wkt(std::string_view text, int* srid = nullptr,
                                                     std::size_t* consumed = nullptr);

/// Well-Known Binary. ISO type codes (1001 = Point Z, ...) unless `srid`
/// is given, which produces PostGIS EWKB (type flags plus the SRID).
[[nodiscard]] Bytes geometry_to_wkb(const ir::Geometry& geometry, bool little_endian = true,
                                    std::optional<int> srid = std::nullopt);

/// Parses one WKB / EWKB geometry from the start of `data` (either byte
/// order, ISO or EWKB type codes). `consumed` receives the bytes used.
[[nodiscard]] Result<ir::Geometry> geometry_from_wkb(ByteView data, std::size_t* consumed = nullptr,
                                                     int* srid = nullptr);

}  // namespace cvt::geo
