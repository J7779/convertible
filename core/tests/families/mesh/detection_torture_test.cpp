// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "util/zip.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;

TEST_CASE("mesh/detection: every fixture is recognised from its content, with and without its name") {
  struct Expect {
    const char* fixture;
    const char* format;
    bool needs_name;  // content alone is ambiguous for this file
  };
  const Expect cases[] = {
      {"mesh/obj/cube_blender.obj", "obj", false},          {"mesh/obj/quad_blender.obj.zip", "obj.zip", false},
      {"mesh/stl/cube_blender_binary.stl", "stl", false},   {"mesh/stl/cube_blender_ascii.stl", "stl", false},
      {"mesh/stl/cube_solid_header_binary.stl", "stl", false}, {"mesh/ply/cube_plyfile_be.ply", "ply", false},
      {"mesh/off/cube_meshlab.off", "off", false},          {"mesh/gltf/cube_blender.glb", "glb", false},
      {"mesh/gltf/edge_cases_pygltflib.gltf", "gltf", false}, {"mesh/3mf/two_objects_trimesh.3mf", "3mf", false},
      {"mesh/amf/cubes_hand.amf", "amf", false},            {"mesh/amf/cubes_hand_zipped.amf", "amf", true},
      {"mesh/dae/cube_blender.dae", "dae", false},          {"mesh/fbx/cube_blender.fbx", "fbx", false},
      {"mesh/fbx/two_objects_assimp_ascii.fbx", "fbx", false}, {"mesh/3ds/cube_blender.3ds", "3ds", false},
      {"mesh/x3d/cube_blender.x3d", "x3d", false},          {"mesh/wrl/cube_meshlab.wrl", "wrl", false},
      {"mesh/usd/cube_blender.usda", "usda", false},        {"mesh/usd/two_objects_usdcore.usdz", "usdz", false},
      {"mesh/xyz/points_open3d.xyz", "xyz", false},         {"mesh/pcd/points_open3d_compressed.pcd", "pcd", false},
      {"mesh/las/points_laspy_v14_f7.las", "las", false},   {"mesh/vtk/sphere_meshio_51_binary.vtk", "vtk", false},
      {"mesh/msh/plate_gmsh41_ascii.msh", "msh", false},    {"mesh/x/cube_assimp.x", "x", false},
      {"mesh/lwo/cube_hand.lwo", "lwo", false},             {"mesh/md2/cube_hand.md2", "md2", false},
      {"mesh/md3/cube_hand.md3", "md3", false},             {"mesh/ms3d/cube_hand.ms3d", "ms3d", false},
      {"mesh/vox/shapes_pyvox.vox", "vox", false},
  };
  for (const Expect& e : cases) {
    INFO(e.fixture);
    const Bytes data = test::fixture(e.fixture);
    const std::string name(mesh::base_name(e.fixture));
    const auto named = detect_format(Registry::instance(), data, name);
    REQUIRE(named.has_value());
    CHECK(named->format->id == e.format);
    if (!e.needs_name) {
      const auto anonymous = detect_format(Registry::instance(), data, "");
      REQUIRE(anonymous.has_value());
      CHECK(anonymous->format->id == e.format);
    }
  }
  // Neighbours stay apart: a plain ZIP, an XHTML-ish document, CSV numbers, prose.
  util::ZipWriter zip;
  REQUIRE_OK(zip.add("readme.txt", "hello"));
  const auto plain = zip.finish();
  REQUIRE_OK(plain);
  for (const char* mesh_id : {"obj.zip", "3mf", "usdz", "amf"}) {
    const auto d = detect_format(Registry::instance(), *plain, "archive.zip");
    CHECK((!d.has_value() || d->format->id != mesh_id));
  }
  const auto csv = detect_format(Registry::instance(), test::bytes("1,2,3\n4,5,6\n7,8,9\n10,11,12\n"), "table.csv");
  CHECK((!csv.has_value() || csv->format->id != "xyz"));
  const auto prose = detect_format(Registry::instance(), test::bytes("vectors are fun\nfaces too\n"), "");
  CHECK((!prose.has_value() || prose->format->id != "obj"));
  CHECK(Registry::instance().validate().empty());
}

TEST_CASE("mesh/triangulate: torture polygons") {
  std::uint32_t seed = 12345;
  const auto random = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
  };
  const auto shoelace = [](const std::vector<ir::Vec2>& p) {
    double a = 0;
    for (std::size_t i = 0; i < p.size(); ++i) a += static_cast<double>(p[i].x) * p[(i + 1) % p.size()].y - static_cast<double>(p[(i + 1) % p.size()].x) * p[i].y;
    return a / 2;
  };
  const auto check_polygon = [&](const std::vector<ir::Vec2>& polygon, double tolerance) {
    const std::vector<std::uint32_t> tris = mesh::triangulate_polygon_2d(polygon);
    REQUIRE(tris.size() == (polygon.size() - 2) * 3);
    const double expected = shoelace(polygon);
    double total = 0;
    for (std::size_t t = 0; t < tris.size(); t += 3) {
      for (int k = 0; k < 3; ++k) REQUIRE(tris[t + k] < polygon.size());
      const ir::Vec2 &a = polygon[tris[t]], &b = polygon[tris[t + 1]], &c = polygon[tris[t + 2]];
      const double area = ((static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
                           (static_cast<double>(c.x) - a.x) * (static_cast<double>(b.y) - a.y)) / 2;
      CHECK(area * expected >= -tolerance);  // oriented like the polygon (or flat)
      total += area;
    }
    CHECK_APPROX(total, expected, tolerance);
  };
  SUBCASE("random star-shaped polygons, both orientations") {
    for (int k = 0; k < 200; ++k) {
      const int n = 3 + static_cast<int>(random() * 60);
      std::vector<ir::Vec2> p;
      for (int i = 0; i < n; ++i) {
        const double angle = 2 * std::numbers::pi * i / n;
        const double r = 0.2 + random();
        p.push_back({static_cast<float>(r * std::cos(angle)), static_cast<float>(r * std::sin(angle))});
      }
      if (k % 2 == 1) std::reverse(p.begin(), p.end());
      check_polygon(p, 1e-4);
    }
  }
  SUBCASE("spiral") {
    std::vector<ir::Vec2> outer, inner;
    for (int i = 0; i <= 200; ++i) {
      const double t = i * 0.05;
      outer.push_back({static_cast<float>((1 + t) * std::cos(t)), static_cast<float>((1 + t) * std::sin(t))});
      inner.push_back({static_cast<float>((0.6 + t) * std::cos(t)), static_cast<float>((0.6 + t) * std::sin(t))});
    }
    std::vector<ir::Vec2> p = outer;
    p.insert(p.end(), inner.rbegin(), inner.rend());
    check_polygon(p, 1e-3);
  }
  SUBCASE("huge offsets and tiny scales") {
    for (const double scale : {1e-6, 1.0, 1e3}) {
      for (const double offset : {0.0, 1e5}) {
        std::vector<ir::Vec2> p;
        for (int i = 0; i < 12; ++i) {
          const double angle = 2 * std::numbers::pi * i / 12;
          const double r = (i % 2 == 0 ? 1.0 : 0.5) * scale;
          p.push_back({static_cast<float>(offset + r * std::cos(angle)), static_cast<float>(offset + r * std::sin(angle))});
        }
        const std::vector<std::uint32_t> tris = mesh::triangulate_polygon_2d(p);
        CHECK(tris.size() == 30);
      }
    }
  }
  SUBCASE("non-planar quads and polygons in every orientation") {
    const ir::Mesh star = test::scenes::concave_star();
    for (int k = 0; k < 24; ++k) {
      ir::Mesh m = star;
      mesh::transform(m, mesh::rotation(static_cast<float>(k * 37), static_cast<float>(random() - 0.5),
                                        static_cast<float>(random() - 0.5), static_cast<float>(random() - 0.5)));
      const double area = mesh::surface_area(m);
      mesh::triangulate(m);
      CHECK(m.indices.size() == 24);
      CHECK_APPROX(mesh::surface_area(m), area, 1e-4);
    }
    const std::vector<ir::Vec3> warped = {{0, 0, 0}, {1, 0, 0.3f}, {1, 1, 0}, {0, 1, 0.3f}};
    CHECK(mesh::triangulate_polygon(warped).size() == 6);
  }
  SUBCASE("deterministic") {
    const std::vector<ir::Vec2> p = {{0, 0}, {4, 0}, {4, 1}, {1, 1}, {1, 3}, {4, 3}, {4, 4}, {0, 4}};
    CHECK(mesh::triangulate_polygon_2d(p) == mesh::triangulate_polygon_2d(p));
    check_polygon(p, 1e-9);
  }
}
