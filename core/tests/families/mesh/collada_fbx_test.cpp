// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::scenes::read_fixture;

namespace {
const ir::MeshObject* object_named(const ir::Mesh& mesh, std::string_view name) {
  for (const ir::MeshObject& object : mesh.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}
}  // namespace

// ------------------------------------------------------------------------- COLLADA

TEST_CASE("mesh/dae: Blender exports (Z-up, polylists, materials, nodes)") {
  const ir::Mesh cube = read_fixture("dae", "mesh/dae/cube_blender.dae");
  CHECK(cube.source_up_axis == "Z");
  CHECK(mesh::triangle_count(cube) == 12);  // Blender's exporter triangulates
  CHECK(cube.normals.size() == cube.positions.size());
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));

  const ir::Mesh sphere = read_fixture("dae", "mesh/dae/sphere_blender.dae");
  CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-5f));
  const ir::Mesh star = read_fixture("dae", "mesh/dae/star_blender.dae");
  CHECK(mesh::triangle_count(star) == 8);
  CHECK_APPROX(mesh::surface_area(star), 1.17557, 1e-4);

  std::vector<std::string> warnings;
  const ir::Mesh quad = read_fixture("dae", "mesh/dae/quad_blender.dae", &warnings);
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  REQUIRE(quad.materials[0].base_color_texture == 0);
  CHECK(quad.textures[0].name == "checker.png");
  CHECK(quad.texcoords.size() == quad.positions.size());
  CHECK(!warnings.empty());

  const ir::Mesh scene = read_fixture("dae", "mesh/dae/two_objects_blender.dae");
  REQUIRE(scene.objects.size() == 2);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
  const ir::Material* red = scene.find_material("Red");
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->base_color.r, 0.8, 1e-3);
  CHECK(scene.unit_meters == 1.0);
}

TEST_CASE("mesh/dae: assimp export and round trips") {
  const ir::Mesh assimp = read_fixture("dae", "mesh/dae/two_objects_assimp.dae");
  CHECK(mesh::triangle_count(assimp) == 236);
  CHECK_APPROX(mesh::bounds(assimp).max.x, 2.5, 1e-5);
  for (const ir::Mesh& source : {test::scenes::uv_sphere(), test::scenes::concave_star(), test::scenes::textured_quad(),
                                 test::scenes::two_objects()}) {
    const ir::Mesh back = test::scenes::round_trip("dae", source);
    CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
    CHECK(back.face_count() == source.face_count());
    CHECK(back.objects.size() == source.objects.size());
    CHECK(back.colors.size() == (source.colors.empty() ? 0 : back.positions.size()));
    CHECK(back.materials.size() == source.materials.size());
  }
  const ir::Mesh scene = test::scenes::round_trip("dae", test::scenes::two_objects());
  REQUIRE(object_named(scene, "Ball") != nullptr);
  CHECK(object_named(scene, "Ball")->transform == test::scenes::two_objects().objects[1].transform);
  const auto tagged = test::write_from("dae", test::scenes::unit_cube());
  REQUIRE_OK(tagged);
  CHECK(test::str(*tagged).find("<created>1970-01-01T00:00:00Z</created>") != std::string::npos);
}

TEST_CASE("mesh/dae: hostile documents fail cleanly") {
  const std::string cyclic = R"(<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">)"
                             R"(<library_nodes><node id="a"><instance_node url="#a"/></node></library_nodes>)"
                             R"(<library_visual_scenes><visual_scene id="s"><node><instance_node url="#a"/></node></visual_scene></library_visual_scenes>)"
                             R"(<scene><instance_visual_scene url="#s"/></scene></COLLADA>)";
  CHECK_STATUS(test::read_as<ir::Mesh>("dae", test::bytes(cyclic)), Status::Decode);
  const std::string out_of_range =
      R"(<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1"><library_geometries><geometry id="g"><mesh>)"
      R"(<source id="p"><float_array id="pa" count="3">0 0 0</float_array><technique_common><accessor source="#pa" count="1" stride="3"/></technique_common></source>)"
      R"(<vertices id="v"><input semantic="POSITION" source="#p"/></vertices>)"
      R"(<triangles count="1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 0 5</p></triangles></mesh></geometry></library_geometries></COLLADA>)";
  CHECK_STATUS(test::read_as<ir::Mesh>("dae", test::bytes(out_of_range)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Mesh>("dae", test::fixture("mesh/dae/cube_blender.dae")) > 0);
  CHECK(test::convert(test::fixture("mesh/dae/two_objects_blender.dae"), "", "dae").ok());
}

// ------------------------------------------------------------------------- FBX

TEST_CASE("mesh/fbx: Blender binary files (deflated arrays, transforms, materials, embedded texture)") {
  // Blender writes centimetres (UnitScaleFactor 1); compare in metres.
  const auto metres = [](ir::Mesh m) {
    CHECK(m.unit_meters == 0.01);
    mesh::convert_units(m, 1.0);
    return m;
  };
  const ir::Mesh cube = metres(read_fixture("fbx", "mesh/fbx/cube_blender.fbx"));
  CHECK(cube.face_count() == 6);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-4);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  const ir::Mesh sphere = metres(read_fixture("fbx", "mesh/fbx/sphere_blender.fbx"));
  CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-5f));
  const ir::Mesh star = read_fixture("fbx", "mesh/fbx/star_blender.fbx");
  CHECK(star.face_sizes == std::vector<std::uint32_t>{10});

  const ir::Mesh quad = read_fixture("fbx", "mesh/fbx/quad_blender.fbx");
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  REQUIRE(quad.materials[0].base_color_texture == 0);
  CHECK(quad.textures[0].data.size() == 81);
  CHECK(quad.textures[0].mime == "image/png");
  REQUIRE(quad.texcoords.size() == quad.positions.size());
  REQUIRE(quad.colors.size() == quad.positions.size());

  const ir::Mesh scene = metres(read_fixture("fbx", "mesh/fbx/two_objects_blender.fbx"));
  REQUIRE(scene.objects.size() == 2);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-4f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-4f));
  const ir::Material* red = scene.find_material("Red");
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->base_color.r, 0.8, 1e-3);
}

TEST_CASE("mesh/fbx: assimp ASCII and binary files") {
  const ir::Mesh ascii = read_fixture("fbx", "mesh/fbx/two_objects_assimp_ascii.fbx");
  CHECK(mesh::triangle_count(ascii) == 236);
  CHECK_APPROX(mesh::bounds(ascii).size().x, 5.0, 1e-4);
  const ir::Mesh binary = read_fixture("fbx", "mesh/fbx/cube_assimp_binary.fbx");
  CHECK(mesh::triangle_count(binary) == 12);
  CHECK_APPROX(std::abs(mesh::signed_volume(binary)), 1.0, 1e-4);
}

TEST_CASE("mesh/fbx: ASCII 7.4 output round trips") {
  for (const ir::Mesh& source : {test::scenes::uv_sphere(), test::scenes::concave_star(), test::scenes::textured_quad(),
                                 test::scenes::two_objects()}) {
    const ir::Mesh back = test::scenes::round_trip("fbx", source);
    CHECK_MESSAGE(test::meshes_close(source, back, 1e-5f), test::mesh_diff(source, back, 1e-5f));
    CHECK(back.face_count() == source.face_count());
    CHECK(back.objects.size() == source.objects.size());
    CHECK(back.texcoords.size() == (source.texcoords.empty() ? 0 : back.positions.size()));
    CHECK(back.materials.size() == source.materials.size());
  }
  ir::Mesh metres = test::scenes::unit_cube();
  metres.unit_meters = 1.0;
  const ir::Mesh back = test::scenes::round_trip("fbx", metres);
  CHECK(back.unit_meters == 1.0);
  const auto text = test::write_from("fbx", test::scenes::unit_cube());
  REQUIRE_OK(text);
  CHECK(test::str(*text).starts_with("; FBX 7.4.0 project file"));
  CHECK(test::str(*text).find("Convertible") == std::string::npos);  // strip_metadata by default
}

TEST_CASE("mesh/fbx: hostile input fails cleanly") {
  Bytes bomb = test::fixture("mesh/fbx/cube_blender.fbx");
  // Truncations and corruptions anywhere.
  CHECK(test::fuzz_reader<ir::Mesh>("fbx", bomb) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("fbx", test::fixture("mesh/fbx/two_objects_assimp_ascii.fbx"), 64) > 0);
  // A node whose end offset points backwards.
  Bytes broken = bomb;
  broken[27] = std::byte{1};
  broken[28] = broken[29] = broken[30] = std::byte{0};
  CHECK_FALSE(test::read_as<ir::Mesh>("fbx", broken).has_value());
  CHECK_STATUS(test::read_as<ir::Mesh>("fbx", test::bytes("; FBX 7.4.0\nObjects: {\n Geometry: 1, \"x\", \"Mesh\" {\n")),
               Status::Decode);
  CHECK(test::convert(test::fixture("mesh/fbx/two_objects_blender.fbx"), "", "glb").ok());
}
