// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

TEST_CASE("mesh/off: decodes a MeshLab cube with colours") {
  const auto mesh = test::read_as<ir::Mesh>("off", test::fixture("mesh/off/cube_meshlab.off"));
  REQUIRE_OK(mesh);
  CHECK(mesh->positions.size() == 8);
  CHECK(mesh::triangle_count(*mesh) == 12);
  CHECK_APPROX(mesh::surface_area(*mesh), 6.0, 1e-5);
  CHECK_APPROX(std::abs(mesh::signed_volume(*mesh)), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(*mesh, test::scenes::unit_cube(), 1e-5f),
                test::mesh_diff(*mesh, test::scenes::unit_cube(), 1e-5f));
  REQUIRE(mesh->colors.size() == 8);
  CHECK(mesh->colors[0].a == 1.0f);
}

TEST_CASE("mesh/off: decodes a trimesh UV sphere") {
  const auto mesh = test::read_as<ir::Mesh>("off", test::fixture("mesh/off/sphere_trimesh.off"));
  REQUIRE_OK(mesh);
  CHECK(mesh->positions.size() == 114);
  CHECK(mesh::triangle_count(*mesh) == 224);
  const mesh::Bounds box = mesh::bounds(*mesh);
  CHECK_APPROX(box.max.y, 1.0, 1e-5);
  CHECK_APPROX(box.min.y, -1.0, 1e-5);
}

TEST_CASE("mesh/off: hand-written variants") {
  SUBCASE("counts on the keyword line, comments, polygon") {
    const auto mesh = test::read_as<ir::Mesh>("off", test::bytes("OFF 4 1 0 # square\n0 0 0\n1 0 0\n1 1 0\n0 1 0\n4 0 1 2 3\n"));
    REQUIRE_OK(mesh);
    CHECK(mesh->face_sizes == std::vector<std::uint32_t>{4});
  }
  SUBCASE("NOFF normals and float colours") {
    const auto mesh = test::read_as<ir::Mesh>(
        "off", test::bytes("CNOFF\n3 1 0\n0 0 0 0 0 1 1 0 0 1\n1 0 0 0 0 1 0 1 0 1\n0 1 0 0 0 1 0 0 1 1\n3 0 1 2\n"));
    REQUIRE_OK(mesh);
    REQUIRE(mesh->normals.size() == 3);
    CHECK(mesh->normals[0].z == 1.0f);
    REQUIRE(mesh->colors.size() == 3);
    CHECK(mesh->colors[0].r == 1.0f);
    CHECK(mesh->colors[0].g == 0.0f);
  }
  SUBCASE("vertices only is a point cloud") {
    const auto mesh = test::read_as<ir::Mesh>("off", test::bytes("OFF\n2 0 0\n0 0 0\n1 1 1\n"));
    REQUIRE_OK(mesh);
    CHECK(mesh->point_cloud);
  }
}

TEST_CASE("mesh/off: round trip keeps shape, colours and polygons") {
  for (const ir::Mesh& source : {test::scenes::unit_cube(), test::scenes::uv_sphere(), test::scenes::concave_star(),
                                 test::scenes::textured_quad()}) {
    const auto encoded = test::write_from("off", source);
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Mesh>("off", *encoded);
    REQUIRE_OK(decoded);
    CHECK_MESSAGE(test::meshes_close(source, *decoded, 0.0f), test::mesh_diff(source, *decoded, 0.0f));
    CHECK(decoded->face_count() == source.face_count());
    CHECK(decoded->colors.size() == (source.colors.empty() ? 0u : decoded->positions.size()));
  }
}

TEST_CASE("mesh/off: options") {
  const ir::Mesh quad = test::scenes::textured_quad();
  const auto plain = test::write_from("off", quad, R"({"include_colors":false,"include_normals":false,"include_uvs":false})");
  REQUIRE_OK(plain);
  CHECK(test::str(*plain).starts_with("OFF\n"));
  const auto full = test::write_from("off", quad);
  REQUIRE_OK(full);
  CHECK(test::str(*full).starts_with("STCNOFF\n"));
  const auto triangulated = test::write_from("off", quad, R"({"triangulate":true})");
  REQUIRE_OK(triangulated);
  const auto back = test::read_as<ir::Mesh>("off", *triangulated);
  REQUIRE_OK(back);
  CHECK(back->face_count() == 2);
  const auto rough = test::write_from("off", test::scenes::uv_sphere(), R"({"precision":3})");
  const auto exact = test::write_from("off", test::scenes::uv_sphere());
  REQUIRE_OK(rough);
  REQUIRE_OK(exact);
  CHECK(rough->size() < exact->size());
}

TEST_CASE("mesh/off: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Mesh>("off", test::bytes("OFF\n3 1 0\n0 0 0\n1 0 0\n0 1 0\n3 0 1 7\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("off", test::bytes("OFF\n999999999 1 0\n0 0 0\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("off", test::bytes("PLY\n")), Status::Decode);
  const Bytes valid = test::fixture("mesh/off/cube_meshlab.off");
  CHECK(test::fuzz_reader<ir::Mesh>("off", valid) > 0);
}

TEST_CASE("mesh/off: detection and engine conversion") {
  const Bytes valid = test::fixture("mesh/off/cube_meshlab.off");
  const test::Converted same = test::convert(valid, "", "off", "", "cube.off");
  CHECK(same.ok());
  CHECK(same.format_id == "off");
}
