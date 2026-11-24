// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::scenes::read_fixture;

// ------------------------------------------------------------------------- 3DS

TEST_CASE("mesh/3ds: Blender and assimp files") {
  const ir::Mesh cube = read_fixture("3ds", "mesh/3ds/cube_blender.3ds");
  CHECK(mesh::triangle_count(cube) == 12);
  CHECK(cube.source_up_axis == "Z");
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK(cube.normals.size() == cube.positions.size());
  const ir::Mesh quad = read_fixture("3ds", "mesh/3ds/quad_blender.3ds");
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  REQUIRE(quad.materials[0].base_color_texture == 0);
  CHECK(quad.textures[0].name == "checker.png");
  CHECK(quad.texcoords.size() == quad.positions.size());
  const ir::Mesh scene = read_fixture("3ds", "mesh/3ds/two_objects_blender.3ds");
  CHECK(scene.objects.size() == 2);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
  CHECK(scene.find_material("Red") != nullptr);
  const ir::Mesh sphere = read_fixture("3ds", "mesh/3ds/sphere_assimp.3ds");
  CHECK(mesh::triangle_count(sphere) == 224);
}

TEST_CASE("mesh/3ds: round trips, smoothing, 65535-vertex splitting") {
  ir::Mesh scene = test::scenes::two_objects();
  scene.source_up_axis = "Y";
  scene.materials.push_back({"Red", {1, 0, 0, 1}, 0, 0.5f, {0, 0, 0}, -1, -1, false});
  scene.groups.push_back({"Box", "Red", 0, scene.objects[0].face_count});
  const ir::Mesh back = test::scenes::round_trip("3ds", scene);
  CHECK_MESSAGE(test::meshes_close(scene, back, 1e-6f), test::mesh_diff(scene, back, 1e-6f));
  CHECK(back.objects.size() == 2);
  REQUIRE(back.find_material("Red") != nullptr);
  CHECK(back.find_material("Red")->base_color.r == 1.0f);
  // A grid of 300 x 300 vertices needs two 3DS objects.
  ir::Mesh big;
  big.source_up_axis = "Y";
  constexpr int n = 300;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) big.positions.push_back({static_cast<float>(x), 0.0f, static_cast<float>(y)});
  }
  for (int y = 0; y + 1 < n; ++y) {
    for (int x = 0; x + 1 < n; ++x) {
      const auto i = static_cast<std::uint32_t>(y * n + x);
      big.indices.insert(big.indices.end(), {i, i + n, i + 1, i + 1, i + n, i + n + 1});
    }
  }
  const auto encoded = test::write_from("3ds", big);
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Mesh>("3ds", *encoded);
  REQUIRE_OK(decoded);
  CHECK(mesh::triangle_count(*decoded) == mesh::triangle_count(big));
  CHECK(decoded->objects.size() >= 2);
  CHECK_APPROX(mesh::surface_area(*decoded), mesh::surface_area(big), 1e-3);
  CHECK(test::fuzz_reader<ir::Mesh>("3ds", test::fixture("mesh/3ds/quad_blender.3ds")) > 0);
  CHECK(test::convert(test::fixture("mesh/3ds/two_objects_blender.3ds"), "", "3ds").ok());
}

// ------------------------------------------------------------------------- spec-built files cross-checked with assimp

TEST_CASE("mesh/lwo, md2, md3, ms3d: hand-built cubes agree with assimp's decoding") {
  struct Case {
    const char* format;
    const char* file;
    const char* reference;
  };
  for (const Case& c : {Case{"lwo", "mesh/lwo/cube_hand.lwo", "mesh/lwo/cube_hand_assimp_reference.obj"},
                        Case{"md2", "mesh/md2/cube_hand.md2", "mesh/md2/cube_hand_assimp_reference.obj"},
                        Case{"md3", "mesh/md3/cube_hand.md3", "mesh/md3/cube_hand_assimp_reference.obj"},
                        Case{"ms3d", "mesh/ms3d/cube_hand.ms3d", "mesh/ms3d/cube_hand_assimp_reference.obj"}}) {
    INFO(c.format);
    const ir::Mesh ours = read_fixture(c.format, c.file);
    const ir::Mesh theirs = read_fixture("obj", c.reference);
    CHECK(mesh::triangle_count(ours) == 12);
    CHECK(mesh::triangle_count(theirs) == 12);
    CHECK_APPROX(mesh::surface_area(ours), 6.0, 1e-3);
    CHECK_APPROX(mesh::signed_volume(ours), 1.0, 1e-3);  // closed, facing outward
    CHECK_MESSAGE(test::meshes_close(ours, test::scenes::unit_cube(), 2e-3f), test::mesh_diff(ours, test::scenes::unit_cube(), 2e-3f));
    CHECK_APPROX(mesh::bounds(theirs).size().x, 1.0, 1e-3);
  }
  const ir::Mesh ms3d = read_fixture("ms3d", "mesh/ms3d/cube_hand.ms3d");
  REQUIRE(ms3d.find_material("Blue") != nullptr);
  CHECK_APPROX(ms3d.find_material("Blue")->base_color.b, 0.9, 1e-6);
  CHECK(ms3d.normals.size() == ms3d.positions.size());
  CHECK(ms3d.texcoords.size() == ms3d.positions.size());
  const ir::Mesh lwo = read_fixture("lwo", "mesh/lwo/cube_hand.lwo");
  REQUIRE(lwo.find_material("Default") != nullptr);
  CHECK_APPROX(lwo.find_material("Default")->base_color.g, 0.4, 1e-6);
  CHECK(lwo.face_count() == 6);
  const ir::Mesh md2 = read_fixture("md2", "mesh/md2/cube_hand.md2");
  CHECK(md2.texcoords.size() == md2.positions.size());
  CHECK(read_fixture("md3", "mesh/md3/cube_hand.md3").normals.size() > 0);
  for (const char* format : {"lwo", "md2", "md3", "ms3d"}) {
    const std::string name = std::format("mesh/{0}/cube_hand.{0}", format);
    CHECK(test::fuzz_reader<ir::Mesh>(format, test::fixture(name)) > 0);
    CHECK(test::convert(test::fixture(name), "", "obj").ok());
  }
}

TEST_CASE("mesh/md2: hostile headers fail cleanly") {
  Bytes md2 = test::fixture("mesh/md2/cube_hand.md2");
  md2[24] = std::byte{0xFF};  // num_xyz huge
  md2[25] = std::byte{0xFF};
  CHECK_FALSE(test::read_as<ir::Mesh>("md2", md2).has_value());
  Bytes md3 = test::fixture("mesh/md3/cube_hand.md3");
  md3[84] = std::byte{0x7F};  // num_surfaces
  CHECK_FALSE(test::read_as<ir::Mesh>("md3", md3).has_value());
}

// ------------------------------------------------------------------------- DirectX

TEST_CASE("mesh/x: assimp DirectX text files") {
  const ir::Mesh cube = read_fixture("x", "mesh/x/cube_assimp.x");
  CHECK(mesh::triangle_count(cube) == 12);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  const ir::Mesh quad = read_fixture("x", "mesh/x/quad_assimp.x");
  REQUIRE(quad.materials.size() == 1);  // assimp writes the material without a name
  CHECK(quad.materials[0].base_color_texture == 0);
  REQUIRE(quad.textures.size() == 1);
  CHECK(quad.textures[0].name == "checker.png");
  CHECK(quad.texcoords.size() == quad.positions.size());
  CHECK(test::fuzz_reader<ir::Mesh>("x", test::fixture("mesh/x/cube_assimp.x")) > 0);
  CHECK_STATUS(test::read_as<ir::Mesh>("x", test::bytes("xof 0302bin 0032....")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("x", test::bytes("xof 0302txt 0032\nMesh {\n 3;\n 0;0;0;,\n")), Status::Decode);
}

// ------------------------------------------------------------------------- MagicaVoxel

TEST_CASE("mesh/vox: greedy meshing of a py-vox-io model") {
  const ir::Mesh m = read_fixture("vox", "mesh/vox/shapes_pyvox.vox");
  // 9 voxels in two closed shapes: a 3x2x1 bar and a 1x1x3 column.
  CHECK_APPROX(mesh::signed_volume(m), 9.0, 1e-5);
  CHECK_APPROX(mesh::surface_area(m), 36.0, 1e-5);
  CHECK(m.face_count() == 12);  // every side of both boxes merged into one quad
  CHECK(m.colors.size() == m.positions.size());
  const mesh::Bounds box = mesh::bounds(m);
  CHECK(box.size().x <= 4.0f);  // inside the 4 x 4 x 4 model
  CHECK(box.size().y <= 4.0f);
  // Two different palette colours.
  bool different = false;
  for (const ir::Color& c : m.colors) different = different || !(c == m.colors[0]);
  CHECK(different);
  CHECK(test::fuzz_reader<ir::Mesh>("vox", test::fixture("mesh/vox/shapes_pyvox.vox")) > 0);
  CHECK(test::convert(test::fixture("mesh/vox/shapes_pyvox.vox"), "", "glb").ok());
}
