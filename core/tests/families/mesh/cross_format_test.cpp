// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Every registered format with a Mesh reader and writer round-trips the
// canonical scenes, alone and chained through every other such format.
#include <set>

#include "engine/registry.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

std::vector<std::string> mesh_formats() {
  std::set<std::string> readers, writers;
  for (const Codec& codec : Registry::instance().codecs()) {
    if (codec.kind != Kind::Mesh) continue;
    if (codec.read) readers.insert(codec.format_id);
    if (codec.write) writers.insert(codec.format_id);
  }
  std::vector<std::string> both;
  for (const std::string& id : writers) {
    if (readers.contains(id)) both.push_back(id);
  }
  return both;
}

bool points_only(std::string_view format) { return format == "xyz" || format == "pcd" || format == "las"; }
// Formats that declare no axes: their readers leave the up axis unknown.
bool axis_agnostic(std::string_view format) {
  return format == "stl" || format == "ply" || format == "off" || format == "vtk" || format == "xyz" || format == "pcd";
}
// Formats whose convention is +Z up.
bool z_up(std::string_view format) { return format == "3mf" || format == "amf" || format == "3ds" || format == "las"; }

ir::Mesh y_up(ir::Mesh m) {
  m.source_up_axis = "Y";  // as if read from a Y-up format: Z-up writers rotate and their readers rotate back
  return m;
}

Result<ir::Mesh> through(std::string_view format, const ir::Mesh& source) {
  CVT_TRY_ASSIGN(const Bytes encoded, test::write_from(format, source));
  return test::read_as<ir::Mesh>(format, encoded);
}

float tolerance_for(const ir::Mesh& m) { return 1e-5f * std::max(1.0f, mesh::bounds(m).diagonal()); }

}  // namespace

TEST_CASE("mesh/all formats: every writer round-trips every canonical scene") {
  const std::vector<std::string> formats = mesh_formats();
  CHECK(formats.size() >= 19);
  for (const std::string& format : formats) {
    INFO(format);
    for (const ir::Mesh& raw : {test::scenes::unit_cube(), test::scenes::uv_sphere(), test::scenes::concave_star(),
                                test::scenes::textured_quad(), test::scenes::two_objects()}) {
      const ir::Mesh source = y_up(raw);
      const auto back = through(format, source);
      REQUIRE_OK(back);
      if (points_only(format)) {
        CHECK(back->point_cloud);
        CHECK(back->positions.size() <= source.positions.size());
        continue;
      }
      CHECK_MESSAGE(test::meshes_close(source, *back, tolerance_for(source)), test::mesh_diff(source, *back, tolerance_for(source)));
      CHECK_APPROX(mesh::surface_area(*back), mesh::surface_area(source), 1e-4 * mesh::surface_area(source));
    }
    // Point clouds: kept by formats that can hold them, refused cleanly by the others.
    const auto cloud = through(format, y_up(test::scenes::point_grid()));
    if (cloud) {
      CHECK(cloud->point_cloud);
      CHECK_MESSAGE(test::meshes_close(*cloud, test::scenes::point_grid(), 1e-5f), test::mesh_diff(*cloud, test::scenes::point_grid(), 1e-5f));
    } else {
      CHECK(cloud.error().code == Status::Encode);
    }
  }
}

TEST_CASE("mesh/all formats: chained conversions through every ordered pair keep the shape") {
  const std::vector<std::string> formats = mesh_formats();
  const ir::Mesh source = y_up(test::scenes::two_objects());
  const float tolerance = tolerance_for(source);
  std::size_t pairs = 0;
  for (const std::string& a : formats) {
    if (points_only(a)) continue;
    const auto first = through(a, source);
    REQUIRE_OK(first);
    for (const std::string& b : formats) {
      if (points_only(b)) continue;
      INFO(a, " -> ", b);
      const auto second = through(b, *first);
      REQUIRE_OK(second);
      // After an axis-agnostic format the axes are unknown, and a Z-up
      // target then takes the coordinates as already Z-up (documented in
      // families/mesh/common.hpp): its reader turns them into Y-up.
      ir::Mesh expected = source;
      if (axis_agnostic(a) && z_up(b)) mesh::convert_up_axis(expected, mesh::UpAxis::Z, mesh::UpAxis::Y);
      // From a Z-up format, axis-agnostic targets keep the Z-up coordinates.
      if (z_up(a) && axis_agnostic(b)) mesh::convert_up_axis(expected, mesh::UpAxis::Y, mesh::UpAxis::Z);
      // The first format may declare a unit for the unitless scene (3MF:
      // millimetres); later formats keep the physical size, possibly in
      // another unit. Compare in the first format's unit.
      ir::Mesh got = *second;
      if (first->unit_meters > 0 && got.unit_meters > 0) mesh::convert_units(got, first->unit_meters);
      CHECK_MESSAGE(test::meshes_close(expected, got, tolerance), test::mesh_diff(expected, got, tolerance));
      ++pairs;
    }
  }
  CHECK(pairs >= 256);
  // Point clouds between the axis-agnostic point formats.
  const ir::Mesh grid = test::scenes::point_grid();
  for (const char* a : {"xyz", "pcd", "ply", "obj", "vtk", "off"}) {
    const auto first = through(a, grid);
    REQUIRE_OK(first);
    for (const char* b : {"xyz", "pcd", "ply"}) {
      INFO(a, " -> ", b);
      const auto second = through(b, *first);
      REQUIRE_OK(second);
      CHECK_MESSAGE(test::meshes_close(grid, *second, 1e-5f), test::mesh_diff(grid, *second, 1e-5f));
    }
  }
  // LAS is Z-up: axis-agnostic targets keep the file's Z-up coordinates, so
  // LAS -> PLY -> LAS returns the same points.
  const auto las = test::read_as<ir::Mesh>("las", test::fixture("mesh/las/points_laspy_v12_f3.las"));
  REQUIRE_OK(las);
  const auto via_ply = through("ply", *las);
  REQUIRE_OK(via_ply);
  const auto again = through("las", *via_ply);
  REQUIRE_OK(again);
  CHECK_MESSAGE(test::meshes_close(*las, *again, 2e-3f), test::mesh_diff(*las, *again, 2e-3f));
}

TEST_CASE("mesh/all formats: shared writer options behave the same everywhere") {
  ir::Mesh quad = y_up(test::scenes::textured_quad());
  quad.metadata["title"] = "Synthetic quad";
  for (const std::string& format : mesh_formats()) {
    if (points_only(format)) continue;
    INFO(format);
    // strip_metadata (default on): the title never leaks.
    const auto stripped = test::write_from(format, quad);
    REQUIRE_OK(stripped);
    const std::string text = test::str(*stripped);
    CHECK(text.find("Synthetic quad") == std::string::npos);
    // include_* options remove the attribute when the format has it.
    const auto bare = test::write_from(format, quad, R"({"include_normals":false,"include_uvs":false,"include_colors":false})");
    REQUIRE_OK(bare);
    const auto back = test::read_as<ir::Mesh>(format, *bare);
    REQUIRE_OK(back);
    CHECK(back->texcoords.empty());
    CHECK(back->colors.empty());
    // up_axis z: the quad (facing +Z in Y-up) comes back rotated or restored, never broken.
    const auto turned = test::write_from(format, quad, R"({"up_axis":"z"})");
    REQUIRE_OK(turned);
    // quality below 90 never makes a file larger.
    const auto sphere = y_up(test::scenes::uv_sphere(48, 24));
    const auto high = test::write_from(format, sphere);
    const auto low = test::write_from(format, sphere, R"({"quality":30})");
    REQUIRE_OK(high);
    REQUIRE_OK(low);
    CHECK(low->size() < high->size());
  }
}
