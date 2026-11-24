// Convertible core tests - mesh family: the canonical synthetic scenes.
//
// The same scenes were exported by independent software into the fixtures
// below tests/data/mesh/ (see SOURCES.md there), so the numbers here are
// what decoded fixtures are checked against:
//
//   unit cube      [-0.5, 0.5]^3, 8 vertices, 6 quads (12 triangles),
//                  area 6, volume 1
//   UV sphere      radius 1, 16 segments x 8 rings: 114 vertices, 96 quads
//                  + 32 triangles (224 triangles)
//   concave star   one flat 10-gon (5-pointed star, outer radius 1, inner
//                  0.4) in the z = 0 plane, facing +Z; area 1.1756
//   textured quad  [-1, 1]^2 at z = 0 facing +Z, uv 0..1, vertex colours
//                  red / green / blue / white, material "Checker" with an
//                  embedded 4x4 PNG texture
//   two objects    "Box" = unit cube translated (2, 0, 0); "Ball" = the UV
//                  sphere scaled 0.5 and translated (-2, 0, 0)
//   point cloud    10 x 10 grid in x, y over [0, 0.9], z = 0.1 * (x + y)
//                  coloured with sRGB bytes red = round(255 x / 0.9),
//                  green = round(255 y / 0.9), blue = 128 (stored linear
//                  in the IR, see families/mesh/common.hpp)
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "ir/mesh.hpp"
#include "support/fixtures.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace cvt::test::scenes {

inline ir::Mesh unit_cube() {
  ir::Mesh m;
  m.name = "Cube";
  m.positions = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                 {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
  m.indices = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
  m.face_sizes = {4, 4, 4, 4, 4, 4};
  return m;
}

/// Blender-style UV sphere: poles + (rings - 1) latitude loops.
inline ir::Mesh uv_sphere(int segments = 16, int rings = 8, float radius = 1.0f) {
  ir::Mesh m;
  m.name = "Sphere";
  m.positions.push_back({0, radius, 0});
  for (int r = 1; r < rings; ++r) {
    const double phi = std::numbers::pi * r / rings;
    for (int s = 0; s < segments; ++s) {
      const double theta = 2 * std::numbers::pi * s / segments;
      m.positions.push_back({static_cast<float>(radius * std::sin(phi) * std::cos(theta)),
                             static_cast<float>(radius * std::cos(phi)),
                             static_cast<float>(-radius * std::sin(phi) * std::sin(theta))});
    }
  }
  m.positions.push_back({0, -radius, 0});
  const auto ring = [&](int r, int s) { return static_cast<std::uint32_t>(1 + (r - 1) * segments + (s % segments)); };
  const auto bottom = static_cast<std::uint32_t>(m.positions.size() - 1);
  for (int s = 0; s < segments; ++s) {
    m.indices.insert(m.indices.end(), {0u, ring(1, s), ring(1, s + 1)});
    m.face_sizes.push_back(3);
  }
  for (int r = 1; r + 1 < rings; ++r) {
    for (int s = 0; s < segments; ++s) {
      m.indices.insert(m.indices.end(), {ring(r, s), ring(r + 1, s), ring(r + 1, s + 1), ring(r, s + 1)});
      m.face_sizes.push_back(4);
    }
  }
  for (int s = 0; s < segments; ++s) {
    m.indices.insert(m.indices.end(), {bottom, ring(rings - 1, s + 1), ring(rings - 1, s)});
    m.face_sizes.push_back(3);
  }
  return m;
}

inline ir::Mesh concave_star() {
  ir::Mesh m;
  m.name = "Star";
  for (int k = 0; k < 10; ++k) {
    const double angle = std::numbers::pi / 2 + k * std::numbers::pi / 5;
    const double r = k % 2 == 0 ? 1.0 : 0.4;
    m.positions.push_back({static_cast<float>(r * std::cos(angle)), static_cast<float>(r * std::sin(angle)), 0.0f});
  }
  for (std::uint32_t k = 0; k < 10; ++k) m.indices.push_back(k);
  m.face_sizes = {10};
  return m;
}

inline ir::Mesh textured_quad() {
  ir::Mesh m;
  m.name = "Quad";
  m.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  m.texcoords = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  m.colors = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
  m.indices = {0, 1, 2, 3};
  m.face_sizes = {4};
  ir::Material material;
  material.name = "Checker";
  material.base_color = {1, 1, 1, 1};
  material.roughness = 0.5f;
  m.materials.push_back(material);
  m.groups.push_back({"Quad", "Checker", 0, 1});
  return m;
}

inline ir::Mesh two_objects() {
  ir::Mesh box = unit_cube();
  ir::Mesh ball = uv_sphere();
  const ir::Mat4 box_transform = mesh::translation(2, 0, 0);
  const ir::Mat4 ball_transform = mesh::translation(-2, 0, 0) * mesh::scaling(0.5f, 0.5f, 0.5f);
  mesh::transform(box, box_transform);
  mesh::transform(ball, ball_transform);
  ir::Mesh scene;
  mesh::append(scene, box);
  const std::size_t box_faces = scene.face_count();
  mesh::append(scene, ball);
  scene.name = "Scene";
  scene.objects = {{"Box", 0, box_faces, box_transform},
                   {"Ball", box_faces, scene.face_count() - box_faces, ball_transform}};
  return scene;
}

inline ir::Mesh point_grid() {
  ir::Mesh m;
  m.point_cloud = true;
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      const float fx = 0.1f * x;
      const float fy = 0.1f * y;
      m.positions.push_back({fx, fy, 0.1f * (fx + fy)});
      m.colors.push_back(mesh::color_from_srgb(std::round(255.0 * x / 9.0), std::round(255.0 * y / 9.0), 128.0));
    }
  }
  return m;
}

/// Read a fixture with the registered reader; records a test failure and
/// returns an empty mesh when it cannot be read.
inline ir::Mesh read_fixture(std::string_view format, std::string_view fixture,
                             std::vector<std::string>* warnings = nullptr, std::string_view options = {}) {
  auto mesh = test::read_as<ir::Mesh>(format, test::fixture(fixture), options, warnings);
  if (!mesh) {
    FAIL_CHECK(fixture, ": ", mesh.error().message);
    return {};
  }
  return std::move(*mesh);
}

/// Write with `format` and read back.
inline ir::Mesh round_trip(std::string_view format, const ir::Mesh& source, std::string_view options = {}) {
  const auto encoded = test::write_from(format, source, options);
  if (!encoded) {
    FAIL_CHECK(format, " write: ", encoded.error().message);
    return {};
  }
  auto decoded = test::read_as<ir::Mesh>(format, *encoded);
  if (!decoded) {
    FAIL_CHECK(format, " read back: ", decoded.error().message);
    return {};
  }
  return std::move(*decoded);
}

/// Z-up fixture data rotated into the IR's Y-up frame.
inline ir::Mesh y_up(ir::Mesh mesh) {
  mesh::convert_up_axis(mesh, mesh::UpAxis::Z, mesh::UpAxis::Y);
  return mesh;
}

}  // namespace cvt::test::scenes
