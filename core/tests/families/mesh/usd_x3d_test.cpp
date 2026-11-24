// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/zip.hpp"

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

// ------------------------------------------------------------------------- USD

TEST_CASE("mesh/usda: Blender exports") {
  const ir::Mesh cube = read_fixture("usda", "mesh/usd/cube_blender.usda");
  CHECK(cube.source_up_axis == "Z");
  CHECK(cube.unit_meters == 1.0);
  CHECK(cube.face_count() == 6);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  std::vector<std::string> warnings;
  const ir::Mesh quad = read_fixture("usda", "mesh/usd/quad_blender.usda", &warnings);
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  CHECK(quad.materials[0].base_color_texture == 0);
  CHECK(quad.texcoords.size() == quad.positions.size());
  CHECK(quad.colors.size() == quad.positions.size());
  CHECK(!warnings.empty());
  const ir::Mesh scene = read_fixture("usda", "mesh/usd/two_objects_blender.usda");
  CHECK(scene.objects.size() == 2);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
}

TEST_CASE("mesh/usda: usd-core stage (Z-up, centimetres, xform ops, indexed primvars, preview material)") {
  ir::Mesh m = read_fixture("usda", "mesh/usd/two_objects_usdcore.usda");
  CHECK(m.unit_meters == 0.01);
  CHECK(m.source_up_axis == "Z");
  mesh::convert_units(m, 1.0);
  CHECK_MESSAGE(test::meshes_close(m, test::scenes::two_objects(), 1e-4f), test::mesh_diff(m, test::scenes::two_objects(), 1e-4f));
  REQUIRE(object_named(m, "Geom") != nullptr);
  const ir::Material* green = m.find_material("Green");
  REQUIRE(green != nullptr);
  CHECK_APPROX(green->base_color.g, 0.7, 1e-6);
  CHECK_APPROX(green->roughness, 0.25, 1e-6);
  CHECK_APPROX(green->metallic, 0.5, 1e-6);
  CHECK(m.texcoords.size() == m.positions.size());
}

TEST_CASE("mesh/usdz: text packages read with their textures; binary crates are refused clearly") {
  const ir::Mesh m = read_fixture("usdz", "mesh/usd/two_objects_usdcore.usdz");
  REQUIRE(m.textures.size() == 1);
  CHECK(m.textures[0].data.size() == 81);
  CHECK(m.textures[0].mime == "image/png");
  const auto crate = test::read_as<ir::Mesh>("usdz", test::fixture("mesh/usd/cube_crate.usdz"));
  REQUIRE_STATUS(crate, Status::Decode);
  CHECK(crate.error().message.find("usdc") != std::string::npos);
  const test::Converted detected = test::convert(test::fixture("mesh/usd/cube_crate.usdz"), "", "obj");
  CHECK(detected.status == Status::Decode);
}

TEST_CASE("mesh/usd: round trips (usda and aligned usdz)") {
  for (const char* format : {"usda", "usdz"}) {
    for (const ir::Mesh& source : {test::scenes::uv_sphere(), test::scenes::concave_star(), test::scenes::textured_quad(),
                                   test::scenes::two_objects()}) {
      const ir::Mesh back = test::scenes::round_trip(format, source);
      CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
      CHECK(back.face_count() == source.face_count());
      CHECK(back.objects.size() == source.objects.size());
      CHECK(back.materials.size() == source.materials.size());
      CHECK(back.texcoords == source.texcoords);
    }
  }
  // usdz: every file's data starts on a 64-byte boundary, textures travel along.
  ir::Mesh quad = test::scenes::textured_quad();
  quad.textures.push_back({"checker.png", "", "image/png", read_fixture("glb", "mesh/gltf/quad_blender.glb").textures[0].data});
  quad.materials[0].base_color_texture = 0;
  const auto usdz = test::write_from("usdz", quad);
  REQUIRE_OK(usdz);
  auto zip = util::ZipReader::open(*usdz);
  REQUIRE_OK(zip);
  REQUIRE(zip->size() == 2);
  for (const util::ZipEntry& entry : zip->entries()) {
    util::ByteReader r(*usdz);
    r.seek(entry.local_header_offset + 26);
    const std::uint16_t name = r.u16le(), extra = r.u16le();
    CHECK((entry.local_header_offset + 30 + name + extra) % 64 == 0);
    CHECK(entry.method == 0);
  }
  const auto back = test::read_as<ir::Mesh>("usdz", *usdz);
  REQUIRE_OK(back);
  REQUIRE(back->textures.size() == 1);
  CHECK(back->textures[0].data == quad.textures[0].data);
  CHECK(test::fuzz_reader<ir::Mesh>("usda", test::fixture("mesh/usd/quad_blender.usda")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("usdz", test::fixture("mesh/usd/two_objects_usdcore.usdz")) > 0);
  CHECK(test::convert(test::fixture("mesh/usd/two_objects_usdcore.usda"), "", "usdz").ok());
}

// ------------------------------------------------------------------------- X3D / VRML

TEST_CASE("mesh/x3d: Blender, MeshLab and assimp files") {
  const ir::Mesh cube = read_fixture("x3d", "mesh/x3d/cube_blender.x3d");
  CHECK(cube.face_count() == 6);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  // Blender's X3D exporter turns the scene half a turn around Y.
  ir::Mesh scene = read_fixture("x3d", "mesh/x3d/two_objects_blender.x3d");
  mesh::transform(scene, mesh::rotation(180.0f, 0, 1, 0));
  CHECK(scene.objects.size() == 2);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
  CHECK(scene.find_material("MA_Red") != nullptr);
  const ir::Mesh quad = read_fixture("x3d", "mesh/x3d/quad_blender.x3d");
  CHECK(mesh::triangle_count(quad) == 2);
  const ir::Mesh meshlab = read_fixture("x3d", "mesh/x3d/cube_meshlab.x3d");
  CHECK(mesh::triangle_count(meshlab) == 12);
  CHECK(meshlab.colors.size() == meshlab.positions.size());
  const ir::Mesh sphere = read_fixture("x3d", "mesh/x3d/sphere_assimp.x3d");
  CHECK(mesh::triangle_count(sphere) == 224);
  CHECK(mesh::signed_volume(sphere) > 3.0);
}

TEST_CASE("mesh/wrl: MeshLab VRML 2.0 and primitives") {
  const ir::Mesh cube = read_fixture("wrl", "mesh/wrl/cube_meshlab.wrl");
  CHECK(mesh::triangle_count(cube) == 12);
  CHECK_APPROX(std::abs(mesh::signed_volume(cube)), 1.0, 1e-5);
  REQUIRE(cube.colors.size() == cube.positions.size());
  const auto shapes = test::read_as<ir::Mesh>(
      "wrl", test::bytes("#VRML V2.0 utf8\n# primitives\nDEF Base Transform { translation 0 1 0 children [\n"
                         " Shape { geometry Box { size 2 2 2 } }\n"
                         " Transform { translation 3 0 0 children [ Shape { appearance Appearance { material Material { diffuseColor 1 0 0 } }"
                         " geometry Sphere { radius 1 } } ] }\n"
                         " Transform { translation -3 0 0 children Shape { geometry Cone { bottomRadius 1 height 2 } } }\n"
                         "] }\nROUTE a.b TO c.d\n"));
  REQUIRE_OK(shapes);
  CHECK_APPROX(mesh::bounds(*shapes).max.x, 4.0, 1e-5);
  CHECK_APPROX(mesh::bounds(*shapes).min.y, 0.0, 1e-5);
  CHECK(mesh::signed_volume(*shapes) > 8.0 + 3.9);
  CHECK_STATUS(test::read_as<ir::Mesh>("wrl", test::bytes("#VRML V1.0 ascii\nSeparator { }\n")), Status::Decode);
  const auto cyclic = test::read_as<ir::Mesh>("wrl", test::bytes("#VRML V2.0 utf8\nDEF A Group { children [ USE A ] }\n"));
  CHECK_FALSE(cyclic.has_value() && !cyclic->positions.empty());
}

TEST_CASE("mesh/x3d, wrl: round trips") {
  for (const char* format : {"x3d", "wrl"}) {
    for (const ir::Mesh& source : {test::scenes::uv_sphere(), test::scenes::concave_star(), test::scenes::textured_quad(),
                                   test::scenes::two_objects()}) {
      const ir::Mesh back = test::scenes::round_trip(format, source);
      CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
      CHECK(back.face_count() == source.face_count());
      CHECK(back.objects.size() == source.objects.size());
      CHECK(back.texcoords == source.texcoords);
      CHECK(back.colors.size() == source.colors.size());
    }
  }
  CHECK(test::fuzz_reader<ir::Mesh>("x3d", test::fixture("mesh/x3d/cube_blender.x3d")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("wrl", test::fixture("mesh/wrl/cube_meshlab.wrl")) > 0);
  CHECK(test::convert(test::fixture("mesh/wrl/cube_meshlab.wrl"), "", "x3d").ok());
  CHECK(test::convert(test::fixture("mesh/x3d/cube_meshlab.x3d"), "", "wrl").ok());
}
