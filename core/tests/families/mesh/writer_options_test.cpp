// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

TEST_CASE("mesh/options: merge_objects, optimize and compression_level") {
  ir::Mesh scene = test::scenes::two_objects();
  scene.source_up_axis = "Y";
  for (const char* format : {"glb", "3mf", "dae", "fbx", "usda", "x3d"}) {
    INFO(format);
    const auto merged = test::write_from(format, scene, R"({"merge_objects":true})");
    REQUIRE_OK(merged);
    const auto back = test::read_as<ir::Mesh>(format, *merged);
    REQUIRE_OK(back);
    CHECK(back->objects.size() <= 1);
    CHECK(mesh::triangle_count(*back) == mesh::triangle_count(scene));
  }
  // optimize (default on) merges exact duplicate vertices; off keeps them.
  ir::Mesh split = test::scenes::unit_cube();
  mesh::generate_normals(split, 0.0f);
  split.normals.clear();  // 24 vertices, 8 distinct positions
  const auto optimized = test::write_from("ply", split);
  const auto raw = test::write_from("ply", split, R"({"optimize":false})");
  REQUIRE_OK(optimized);
  REQUIRE_OK(raw);
  CHECK(test::read_as<ir::Mesh>("ply", *optimized)->positions.size() == 8);
  CHECK(test::read_as<ir::Mesh>("ply", *raw)->positions.size() == 24);
  // compression_level of the 3MF package.
  const ir::Mesh sphere = test::scenes::uv_sphere(48, 24);
  const auto stored = test::write_from("3mf", sphere, R"({"compression_level":0})");
  const auto packed = test::write_from("3mf", sphere, R"({"compression_level":9})");
  REQUIRE_OK(stored);
  REQUIRE_OK(packed);
  CHECK(packed->size() < stored->size() / 2);
  // Out-of-range and invalid values: clamped or refused by the engine.
  CHECK_OK(test::write_from("obj", sphere, R"({"precision":99})"));
  CHECK_STATUS(test::write_from("obj", sphere, R"({"up_axis":"sideways"})"), Status::InvalidArgument);
}
