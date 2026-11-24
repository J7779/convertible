// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>

#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;

TEST_CASE("mesh/ops: measures of the canonical scenes") {
  const ir::Mesh cube = test::scenes::unit_cube();
  CHECK_APPROX(mesh::surface_area(cube), 6.0, 1e-9);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-9);
  CHECK(mesh::triangle_count(cube) == 12);
  const ir::Mesh sphere = test::scenes::uv_sphere();
  CHECK(sphere.positions.size() == 114);
  CHECK(mesh::triangle_count(sphere) == 224);
  CHECK(mesh::signed_volume(sphere) > 3.0);  // outward winding (sphere volume 4.19, faceted less)
  const ir::Mesh star = test::scenes::concave_star();
  CHECK_APPROX(mesh::surface_area(star), 1.17557, 1e-4);
}

TEST_CASE("mesh/ops: ear clipping keeps the area of concave polygons") {
  ir::Mesh star = test::scenes::concave_star();
  const double area = mesh::surface_area(star);
  mesh::triangulate(star);
  CHECK(star.face_sizes.empty());
  CHECK(star.indices.size() == 24);
  CHECK_APPROX(mesh::surface_area(star), area, 1e-5);
  // Every triangle faces +Z like the polygon.
  for (std::size_t i = 0; i < star.indices.size(); i += 3) {
    const ir::Vec3 n = mesh::cross(mesh::sub(star.positions[star.indices[i + 1]], star.positions[star.indices[i]]),
                                   mesh::sub(star.positions[star.indices[i + 2]], star.positions[star.indices[i]]));
    CHECK(n.z > 0.0f);
  }
}

TEST_CASE("mesh/ops: triangulation of awkward polygons terminates with n-2 triangles") {
  SUBCASE("collinear corners") {
    const std::vector<ir::Vec3> pts = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}};
    CHECK(mesh::triangulate_polygon(pts).size() == 9);
  }
  SUBCASE("repeated corners") {
    const std::vector<ir::Vec3> pts = {{0, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    CHECK(mesh::triangulate_polygon(pts).size() == 9);
  }
  SUBCASE("self-intersecting bow tie") {
    const std::vector<ir::Vec3> pts = {{0, 0, 0}, {1, 1, 0}, {1, 0, 0}, {0, 1, 0}};
    CHECK(mesh::triangulate_polygon(pts).size() == 6);
  }
  SUBCASE("all points equal") {
    const std::vector<ir::Vec3> pts(7, ir::Vec3{1, 2, 3});
    CHECK(mesh::triangulate_polygon(pts).size() == 15);
  }
  SUBCASE("clockwise comb in a tilted plane") {
    std::vector<ir::Vec3> pts;
    for (int k = 0; k < 20; ++k) pts.push_back({static_cast<float>(k), (k % 2) ? 3.0f : 1.0f, 0.0f});
    pts.push_back({19, 0, 0});
    pts.push_back({0, 0, 0});
    std::reverse(pts.begin(), pts.end());
    ir::Mesh m;
    m.positions = pts;
    for (std::uint32_t k = 0; k < pts.size(); ++k) m.indices.push_back(k);
    m.face_sizes = {static_cast<std::uint32_t>(pts.size())};
    mesh::transform(m, mesh::rotation(37.0f, 1, 2, 3));
    const double area = mesh::surface_area(m);
    mesh::triangulate(m);
    CHECK(m.indices.size() == (pts.size() - 2) * 3);
    CHECK_APPROX(mesh::surface_area(m), area, area * 1e-5);
  }
}

TEST_CASE("mesh/ops: normals, flat and smooth with crease") {
  ir::Mesh cube = test::scenes::unit_cube();
  mesh::generate_normals(cube, 30.0f);
  CHECK(cube.positions.size() == 24);  // corners split: three faces meet at 90 degrees
  CHECK(cube.normals.size() == 24);
  ir::Mesh smooth = test::scenes::unit_cube();
  mesh::generate_normals(smooth, 180.0f);
  CHECK(smooth.positions.size() == 8);
  CHECK_APPROX(mesh::length(smooth.normals[0]), 1.0, 1e-6);
  ir::Mesh sphere = test::scenes::uv_sphere();
  mesh::generate_normals(sphere, 60.0f);
  CHECK(sphere.positions.size() == 114);  // smooth everywhere below the crease
  for (std::size_t i = 0; i < sphere.positions.size(); ++i) {
    CHECK(mesh::dot(sphere.normals[i], mesh::normalize(sphere.positions[i])) > 0.95f);
  }
}

TEST_CASE("mesh/ops: weld, unused vertices, degenerate faces, winding") {
  ir::Mesh cube = test::scenes::unit_cube();
  mesh::generate_normals(cube, 0.0f);
  cube.normals.clear();
  CHECK(mesh::weld(cube) == 16);
  CHECK(cube.positions.size() == 8);
  ir::Mesh m = test::scenes::unit_cube();
  m.positions.push_back({9, 9, 9});
  CHECK(mesh::remove_unused_vertices(m) == 1);
  m.indices.insert(m.indices.end(), {4, 4, 4, 5, 0, 3, 2, 1});  // degenerate + duplicate of face 0
  m.face_sizes.insert(m.face_sizes.end(), {4, 4});
  CHECK(mesh::remove_degenerate_faces(m) == 2);
  CHECK(m.face_count() == 6);
  ir::Mesh flipped = test::scenes::uv_sphere();
  mesh::flip_winding(flipped);
  for (std::size_t f = 0; f < 10; ++f) std::swap(flipped.indices[f * 3 + 1], flipped.indices[f * 3 + 2]);
  mesh::triangulate(flipped);
  mesh::fix_winding(flipped);
  CHECK(mesh::signed_volume(flipped) > 3.0);
}

TEST_CASE("mesh/ops: transforms, axes, units, objects") {
  ir::Mesh m = test::scenes::unit_cube();
  mesh::transform(m, mesh::translation(1, 2, 3));
  CHECK_APPROX(mesh::bounds(m).center().z, 3.0, 1e-6);
  mesh::convert_up_axis(m, mesh::UpAxis::Y, mesh::UpAxis::Z);
  CHECK_APPROX(mesh::bounds(m).center().z, 2.0, 1e-6);
  mesh::convert_up_axis(m, mesh::UpAxis::Z, mesh::UpAxis::Y);
  CHECK_APPROX(mesh::bounds(m).center().y, 2.0, 1e-6);
  m.unit_meters = 0.001;
  mesh::convert_units(m, 1.0);
  CHECK_APPROX(mesh::bounds(m).size().x, 0.001, 1e-9);
  ir::Mesh mirrored = test::scenes::unit_cube();
  mesh::transform(mirrored, mesh::scaling(-1, 1, 1));
  CHECK(mesh::signed_volume(mirrored) > 0.0);

  const ir::Mesh scene = test::scenes::two_objects();
  REQUIRE(scene.objects.size() == 2);
  const auto parts = mesh::split_objects(scene, true);
  REQUIRE(parts.size() == 2);
  CHECK(parts[0].name == "Box");
  CHECK_MESSAGE(test::meshes_close(parts[0].mesh, test::scenes::unit_cube(), 1e-6f),
                test::mesh_diff(parts[0].mesh, test::scenes::unit_cube(), 1e-6f));
  CHECK_MESSAGE(test::meshes_close(parts[1].mesh, test::scenes::uv_sphere(), 1e-6f),
                test::mesh_diff(parts[1].mesh, test::scenes::uv_sphere(), 1e-6f));
  bool ok = false;
  const ir::Mat4 inv = mesh::inverse(parts[1].transform, &ok);
  CHECK(ok);
  CHECK((parts[1].transform * inv).is_identity() || std::abs((parts[1].transform * inv).m[0] - 1.0f) < 1e-6f);
}

TEST_CASE("mesh/ops: colour transfer functions") {
  CHECK_APPROX(mesh::srgb_to_linear(0.5f), 0.21404, 1e-5);
  CHECK_APPROX(mesh::linear_to_srgb(mesh::srgb_to_linear(0.3f)), 0.3, 1e-6);
  const auto bytes = mesh::color_to_srgb8(mesh::color_from_srgb(12, 128, 255, 77));
  CHECK(bytes == std::array<std::uint8_t, 4>{12, 128, 255, 77});
}

TEST_CASE("mesh/common: number formatting and parsing are exact") {
  std::string out;
  mesh::append_number(out, 0.1f);
  CHECK(out == "0.1");
  out.clear();
  mesh::append_number(out, -0.0f);
  CHECK(out == "0");
  out.clear();
  mesh::append_number(out, 3.14159265, 3);
  CHECK(out == "3.14");
  for (const char* text : {"0.1", "-2.5e-3", "123456789012", "1e30", "7", ".5", "-0.000123456789"}) {
    double fast = 0;
    REQUIRE(mesh::parse_double(text, fast));
    CHECK(fast == *util::parse_double(text));
  }
  double bad = 0;
  CHECK_FALSE(mesh::parse_double("1.2.3", bad));
  CHECK_FALSE(mesh::parse_double("", bad));
}
