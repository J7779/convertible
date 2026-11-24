// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::scenes::read_fixture;
using test::scenes::y_up;

// ------------------------------------------------------------------------- STL

TEST_CASE("mesh/stl: binary and ASCII files from Blender, numpy-stl and by hand") {
  const ir::Mesh binary = read_fixture("stl", "mesh/stl/cube_blender_binary.stl");
  CHECK(binary.positions.size() == 8);  // welded
  CHECK(mesh::triangle_count(binary) == 12);
  CHECK_APPROX(mesh::signed_volume(binary), 1.0, 1e-5);
  const ir::Mesh ascii = read_fixture("stl", "mesh/stl/cube_blender_ascii.stl");
  CHECK_MESSAGE(test::meshes_close(binary, ascii, 1e-6f), test::mesh_diff(binary, ascii, 1e-6f));

  const ir::Mesh sphere = y_up(read_fixture("stl", "mesh/stl/sphere_blender_binary.stl"));
  CHECK(sphere.positions.size() == 114);
  CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-5f));
  const ir::Mesh numpy = read_fixture("stl", "mesh/stl/sphere_numpystl_ascii.stl");
  CHECK_MESSAGE(test::meshes_close(numpy, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(numpy, test::scenes::uv_sphere(), 1e-5f));

  // A binary file whose header starts with "solid" is still binary.
  const ir::Mesh solid = read_fixture("stl", "mesh/stl/cube_solid_header_binary.stl");
  CHECK(mesh::triangle_count(solid) == 12);
  CHECK_APPROX(mesh::surface_area(solid), 6.0, 1e-5);
}

TEST_CASE("mesh/stl: VisCAM facet colours become vertex colours") {
  const ir::Mesh cube = read_fixture("stl", "mesh/stl/cube_viscam_colors.stl");
  REQUIRE(cube.colors.size() == cube.positions.size());
  CHECK(cube.positions.size() == 20);  // 8 red + 6 green + 6 blue corners
  const ir::Color first = cube.colors[cube.indices[0]];
  CHECK(first.r == 1.0f);
  CHECK(first.g == 0.0f);
  const ir::Color last = cube.colors[cube.indices.back()];
  CHECK(last.b == 1.0f);
  // Our writer keeps them (VisCAM convention).
  const ir::Mesh back = test::scenes::round_trip("stl", cube);
  REQUIRE(back.colors.size() == back.positions.size());
  CHECK(back.colors[back.indices[0]].r == 1.0f);
}

TEST_CASE("mesh/stl: round trips, options and the compress path") {
  for (const ir::Mesh& source : {test::scenes::unit_cube(), test::scenes::uv_sphere(), test::scenes::concave_star()}) {
    for (const char* options : {"", R"({"binary":false})"}) {
      const ir::Mesh back = test::scenes::round_trip("stl", source, options);
      CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
      CHECK_APPROX(mesh::surface_area(back), mesh::surface_area(source), 1e-5);
    }
  }
  const auto binary = test::write_from("stl", test::scenes::uv_sphere());
  const auto ascii = test::write_from("stl", test::scenes::uv_sphere(), R"({"binary":false})");
  REQUIRE_OK(binary);
  REQUIRE_OK(ascii);
  CHECK(binary->size() == 84 + 224 * 50);
  CHECK(test::str(*ascii).starts_with("solid model\n"));
  CHECK(binary->size() < ascii->size());
  const auto small = test::write_from("stl", test::scenes::uv_sphere(64, 32), R"({"quality":40})");
  const auto full = test::write_from("stl", test::scenes::uv_sphere(64, 32));
  REQUIRE_OK(small);
  REQUIRE_OK(full);
  CHECK(small->size() <= full->size());
  // Engine: detect and compress.
  const test::Converted same = test::convert(test::fixture("mesh/stl/sphere_numpystl_ascii.stl"), "", "stl");
  REQUIRE(same.ok());
  CHECK(same.data.size() == 84 + 224 * 50);
}

TEST_CASE("mesh/stl: damaged input fails cleanly") {
  Bytes truncated = test::fixture("mesh/stl/cube_blender_binary.stl");
  truncated.resize(200);
  CHECK_STATUS(test::read_as<ir::Mesh>("stl", truncated), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("stl", test::bytes("solid x\n facet normal 0 0 1\n outer loop\n vertex 0 0\n")),
               Status::Decode);
  Bytes bomb(84, std::byte{0});
  bomb[80] = std::byte{0xFF};
  bomb[81] = std::byte{0xFF};
  bomb[82] = std::byte{0xFF};
  bomb[83] = std::byte{0x7F};
  CHECK_FALSE(test::read_as<ir::Mesh>("stl", bomb).has_value());
  CHECK(test::fuzz_reader<ir::Mesh>("stl", test::fixture("mesh/stl/cube_blender_binary.stl")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("stl", test::fixture("mesh/stl/cube_blender_ascii.stl")) > 0);
}

// ------------------------------------------------------------------------- PLY

TEST_CASE("mesh/ply: Blender ASCII and binary files") {
  const ir::Mesh cube = read_fixture("ply", "mesh/ply/cube_blender_ascii.ply");
  CHECK(cube.face_count() == 6);
  CHECK(cube.normals.size() == cube.positions.size());
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  const ir::Mesh sphere = y_up(read_fixture("ply", "mesh/ply/sphere_blender_binary.ply"));
  CHECK(sphere.positions.size() == 114);
  CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-5f));
  const ir::Mesh star = read_fixture("ply", "mesh/ply/star_blender_ascii.ply");
  CHECK(star.face_sizes == std::vector<std::uint32_t>{10});
  const ir::Mesh quad = y_up(read_fixture("ply", "mesh/ply/quad_blender_colors.ply"));
  REQUIRE(quad.colors.size() == 4);
  REQUIRE(quad.texcoords.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    const ir::Vec3& p = quad.positions[i];
    const ir::Color& c = quad.colors[i];
    // Blender stores 254 for a full channel (its own sRGB rounding).
    if (p.x < 0 && p.y < 0) CHECK((c.r > 0.98f && c.g == 0.0f && c.b == 0.0f));
    if (p.x > 0 && p.y > 0) CHECK((c.r == 0.0f && c.g == 0.0f && c.b > 0.98f));
    CHECK(quad.texcoords[i].x == (p.x > 0 ? 1.0f : 0.0f));
    CHECK(quad.texcoords[i].y == (p.y > 0 ? 1.0f : 0.0f));
  }
}

TEST_CASE("mesh/ply: big-endian, odd types, lists, attributes (plyfile)") {
  std::vector<std::string> warnings;
  const ir::Mesh cube = read_fixture("ply", "mesh/ply/cube_plyfile_be.ply", &warnings);
  CHECK(cube.positions.size() == 8);
  CHECK(cube.face_count() == 6);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-9);
  REQUIRE(cube.colors.size() == 8);
  CHECK(cube.colors[1].r == 1.0f);
  CHECK(cube.colors[1].g == 0.0f);
  REQUIRE(cube.attributes.size() == 2);
  CHECK(cube.attributes[0].name == "label");
  CHECK(cube.attributes[0].values[0] == -4.0f);
  CHECK(cube.attributes[1].values[3] == 0.75f);
  CHECK(cube.metadata.at("comment") == "synthetic unit cube");
}

TEST_CASE("mesh/ply: point clouds and per-face colours") {
  const ir::Mesh points = read_fixture("ply", "mesh/ply/points_plyfile_ascii.ply");
  CHECK(points.point_cloud);
  CHECK_MESSAGE(test::meshes_close(points, test::scenes::point_grid(), 1e-6f),
                test::mesh_diff(points, test::scenes::point_grid(), 1e-6f));
  REQUIRE(points.colors.size() == 100);
  CHECK(points.colors == test::scenes::point_grid().colors);
  REQUIRE(points.attributes.size() == 1);
  CHECK(points.attributes[0].values[42] == 42.0f);

  const ir::Mesh faces = read_fixture("ply", "mesh/ply/cube_meshlab_face_colors.ply");
  CHECK(mesh::triangle_count(faces) == 12);
  REQUIRE(faces.colors.size() == faces.positions.size());
  CHECK(faces.positions.size() == 20);
}

TEST_CASE("mesh/ply: round trips in all encodings keep everything") {
  ir::Mesh rich = test::scenes::textured_quad();
  rich.attributes.push_back({"quality", {0.5f, 1.5f, 2.5f, 3.5f}});
  for (const char* options : {"", R"({"binary":false})", R"({"big_endian":true})"}) {
    for (const ir::Mesh& source : {test::scenes::uv_sphere(), test::scenes::concave_star(), rich, test::scenes::point_grid()}) {
      const ir::Mesh back = test::scenes::round_trip("ply", source, options);
      CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
      CHECK(back.face_count() == source.face_count());
      CHECK(back.texcoords == source.texcoords);
      CHECK(back.attributes.size() == source.attributes.size());
      CHECK(back.colors.size() == source.colors.size());
    }
  }
  const auto ascii = test::write_from("ply", test::scenes::uv_sphere(), R"({"binary":false})");
  const auto binary = test::write_from("ply", test::scenes::uv_sphere());
  REQUIRE_OK(ascii);
  REQUIRE_OK(binary);
  CHECK(test::str(*binary).starts_with("ply\nformat binary_little_endian 1.0\n"));
}

TEST_CASE("mesh/ply: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Mesh>("ply", test::bytes("ply\nformat ascii 1.0\nelement vertex 99999999999\nproperty float x\n"
                                                          "property float y\nproperty float z\nend_header\n0 0 0\n")),
               Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("ply", test::bytes("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n"
                                                          "property float y\nproperty float z\nelement face 1\nproperty "
                                                          "list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 9\n")),
               Status::Decode);
  CHECK(test::fuzz_reader<ir::Mesh>("ply", test::fixture("mesh/ply/cube_plyfile_be.ply")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("ply", test::fixture("mesh/ply/cube_blender_ascii.ply")) > 0);
  const test::Converted detected = test::convert(test::fixture("mesh/ply/sphere_blender_binary.ply"), "", "ply");
  CHECK(detected.ok());
}
