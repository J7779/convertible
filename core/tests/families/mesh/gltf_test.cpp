// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"

using namespace cvt;
using test::scenes::read_fixture;

namespace {

bool has_point(const ir::Mesh& mesh, ir::Vec3 p, float tolerance = 1e-5f) {
  return std::any_of(mesh.positions.begin(), mesh.positions.end(), [&](const ir::Vec3& q) {
    return std::abs(q.x - p.x) <= tolerance && std::abs(q.y - p.y) <= tolerance && std::abs(q.z - p.z) <= tolerance;
  });
}

const ir::MeshObject* object_named(const ir::Mesh& mesh, std::string_view name) {
  for (const ir::MeshObject& object : mesh.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}

// Structural validation of a GLB we wrote, following the Khronos validator's rules.
void check_valid_glb(const Bytes& glb) {
  util::ByteReader r(glb);
  REQUIRE(r.u32le() == 0x46546C67u);
  CHECK(r.u32le() == 2u);
  CHECK(r.u32le() == glb.size());
  const std::uint32_t json_length = r.u32le();
  CHECK(r.u32le() == 0x4E4F534Au);
  CHECK(json_length % 4 == 0);
  const std::string_view json_text = r.chars(json_length);
  const std::uint32_t bin_length = r.u32le();
  CHECK(r.u32le() == 0x004E4942u);
  CHECK(bin_length % 4 == 0);
  const ByteView bin = r.bytes(bin_length);
  REQUIRE(r.ok());
  CHECK(r.at_end());
  const auto root = util::parse_json(json_text);
  REQUIRE_OK(root);
  CHECK((*root)["asset"]["version"].as_string() == "2.0");
  CHECK((*root)["buffers"][0]["byteLength"].as_uint64() == bin_length);
  for (const Json& view : (*root)["bufferViews"].elements()) {
    CHECK(view["byteOffset"].as_uint64() % 4 == 0);
    CHECK(view["byteOffset"].as_uint64() + view["byteLength"].as_uint64() <= bin.size());
  }
  for (const Json& m : (*root)["meshes"].elements()) {
    for (const Json& primitive : m["primitives"].elements()) {
      const Json& position = (*root)["accessors"][primitive["attributes"]["POSITION"].as_uint64()];
      CHECK(position["min"].size() == 3);
      CHECK(position["max"].size() == 3);
      if (position["componentType"].as_int64() != 5126) continue;
      const Json& view = (*root)["bufferViews"][position["bufferView"].as_uint64()];
      util::ByteReader values(bin.subspan(view["byteOffset"].as_uint64(), view["byteLength"].as_uint64()));
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (std::uint64_t i = 0; i < position["count"].as_uint64(); ++i) {
        for (int c = 0; c < 3; ++c) {
          const float v = values.f32le();
          lo[c] = std::min(lo[c], v);
          hi[c] = std::max(hi[c], v);
        }
      }
      for (int c = 0; c < 3; ++c) {
        CHECK(static_cast<float>(position["min"][c].as_double()) == lo[c]);
        CHECK(static_cast<float>(position["max"][c].as_double()) == hi[c]);
      }
      if (const Json* normal = primitive["attributes"].find("NORMAL")) {
        const Json& n = (*root)["accessors"][normal->as_uint64()];
        const Json& nview = (*root)["bufferViews"][n["bufferView"].as_uint64()];
        util::ByteReader nr(bin.subspan(nview["byteOffset"].as_uint64(), nview["byteLength"].as_uint64()));
        for (std::uint64_t i = 0; i < n["count"].as_uint64(); ++i) {
          const float x = nr.f32le(), y = nr.f32le(), z = nr.f32le();
          CHECK_APPROX(std::sqrt(x * x + y * y + z * z), 1.0, 1e-5);
        }
      }
    }
  }
}

}  // namespace

TEST_CASE("mesh/gltf: Blender GLB files") {
  const ir::Mesh cube = read_fixture("glb", "mesh/gltf/cube_blender.glb");
  CHECK(mesh::triangle_count(cube) == 12);
  CHECK(cube.normals.size() == cube.positions.size());
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));
  CHECK(cube.unit_meters == 1.0);

  const ir::Mesh star = read_fixture("glb", "mesh/gltf/star_blender.glb");
  CHECK(mesh::triangle_count(star) == 8);
  CHECK_APPROX(mesh::surface_area(star), 1.17557, 1e-4);

  const ir::Mesh quad = read_fixture("glb", "mesh/gltf/quad_blender.glb");
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  CHECK_APPROX(quad.materials[0].roughness, 0.5, 1e-6);
  REQUIRE(quad.materials[0].base_color_texture == 0);
  CHECK(quad.textures[0].mime == "image/png");
  CHECK(quad.textures[0].data.size() == 81);
  REQUIRE(quad.texcoords.size() == quad.positions.size());
  for (std::size_t i = 0; i < quad.positions.size(); ++i) {
    CHECK(quad.texcoords[i].x == (quad.positions[i].x > 0 ? 1.0f : 0.0f));
    CHECK(quad.texcoords[i].y == (quad.positions[i].y > 0 ? 1.0f : 0.0f));
  }

  const ir::Mesh scene = read_fixture("glb", "mesh/gltf/two_objects_blender.glb");
  REQUIRE(scene.objects.size() == 2);
  const ir::MeshObject* box = object_named(scene, "Box");
  const ir::MeshObject* ball = object_named(scene, "Ball");
  REQUIRE(box != nullptr);
  REQUIRE(ball != nullptr);
  CHECK_APPROX(box->transform.m[12], 2.0, 1e-6);
  CHECK_APPROX(ball->transform.m[0], 0.5, 1e-6);
  CHECK_APPROX(ball->transform.m[12], -2.0, 1e-6);
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
  const ir::Material* red = scene.find_material("Red");
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->base_color.r, 0.8, 1e-5);
  CHECK_APPROX(red->metallic, 0.25, 1e-5);
  CHECK_APPROX(red->roughness, 0.6, 1e-5);
}

TEST_CASE("mesh/gltf: pygltflib edge cases (strip, fan, sparse, stride, TRS, matrix)") {
  const ir::Mesh m = read_fixture("gltf", "mesh/gltf/edge_cases_pygltflib.gltf");
  CHECK(mesh::triangle_count(m) == 6);
  REQUIRE(m.objects.size() == 3);
  CHECK(object_named(m, "StripNode") != nullptr);
  CHECK(object_named(m, "FanNode") != nullptr);
  CHECK(object_named(m, "SparseNode") != nullptr);
  CHECK(has_point(m, {5, 2, 0}));    // fan corner (1,0,0) scaled 2, rotated 90 degrees, moved +5 x
  CHECK(has_point(m, {3, 2, 0}));    // fan corner (1,1,0)
  CHECK(has_point(m, {2, 12, 1}));   // sparse substitution + parent matrix + child translation
  CHECK_FALSE(has_point(m, {1, 11, 1}));
  REQUIRE(m.texcoords.size() == m.positions.size());  // normalized UNSIGNED_SHORT
  // Strip triangles keep a consistent (+Z facing) winding.
  for (std::size_t t = 0; t < m.indices.size(); t += 3) {
    const ir::Vec3 n = mesh::cross(mesh::sub(m.positions[m.indices[t + 1]], m.positions[m.indices[t]]),
                                   mesh::sub(m.positions[m.indices[t + 2]], m.positions[m.indices[t]]));
    CHECK(n.z > 0.0f);
  }
}

TEST_CASE("mesh/gltf: embedded and external buffers, trimesh and assimp files") {
  const ir::Mesh embedded = read_fixture("gltf", "mesh/gltf/quad_pygltflib_embedded.gltf");
  CHECK(embedded.textures.size() == 1);
  CHECK(mesh::triangle_count(embedded) == 2);
  std::vector<std::string> warnings;
  const auto separate = test::read_as<ir::Mesh>("gltf", test::fixture("mesh/gltf/quad_pygltflib_separate.gltf"), {}, &warnings);
  CHECK_STATUS(separate, Status::Decode);
  CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.find("quad.bin") != std::string::npos; }));

  const ir::Mesh sphere = read_fixture("glb", "mesh/gltf/sphere_trimesh_colors.glb");
  CHECK(sphere.positions.size() == 114);
  REQUIRE(sphere.colors.size() == 114);
  CHECK(sphere.colors[0].r == 1.0f);
  CHECK_APPROX(sphere.colors[0].g, 128.0 / 255.0, 1e-6);
  const ir::Mesh assimp = read_fixture("glb", "mesh/gltf/two_objects_assimp.glb");
  CHECK(mesh::triangle_count(assimp) == 236);
  CHECK_APPROX(mesh::bounds(assimp).max.x, 2.5, 1e-5);
}

TEST_CASE("mesh/gltf: round trips are lossless and our GLB is valid") {
  for (const char* format : {"glb", "gltf"}) {
    for (const ir::Mesh& source : {test::scenes::unit_cube(), test::scenes::uv_sphere(), test::scenes::concave_star(),
                                   test::scenes::two_objects(), test::scenes::point_grid()}) {
      const ir::Mesh back = test::scenes::round_trip(format, source);
      CHECK_MESSAGE(test::meshes_close(source, back, 1e-6f), test::mesh_diff(source, back, 1e-6f));
      CHECK(back.objects.size() == source.objects.size());
      CHECK(back.point_cloud == source.point_cloud);
    }
  }
  ir::Mesh quad = test::scenes::textured_quad();
  quad.textures.push_back({"checker.png", "", "image/png", {}});
  quad.textures.back().data = test::scenes::read_fixture("glb", "mesh/gltf/quad_blender.glb").textures[0].data;
  quad.materials[0].base_color_texture = 0;
  quad.materials[0].metallic = 0.25f;
  const auto glb = test::write_from("glb", quad);
  REQUIRE_OK(glb);
  check_valid_glb(*glb);
  const auto back = test::read_as<ir::Mesh>("glb", *glb);
  REQUIRE_OK(back);
  REQUIRE(back->materials.size() == 1);
  CHECK(back->materials[0] == quad.materials[0]);
  CHECK(back->textures[0].data == quad.textures[0].data);
  CHECK(back->texcoords == quad.texcoords);
  CHECK(back->colors == quad.colors);
  const auto scene = test::write_from("glb", test::scenes::two_objects());
  REQUIRE_OK(scene);
  check_valid_glb(*scene);
  const auto again = test::read_as<ir::Mesh>("glb", *scene);
  REQUIRE_OK(again);
  REQUIRE(again->objects.size() == 2);
  CHECK(again->objects[1].transform == test::scenes::two_objects().objects[1].transform);
}

TEST_CASE("mesh/gltf: options (quantize, quality, strip metadata)") {
  const ir::Mesh sphere = test::scenes::uv_sphere(64, 32);
  const auto plain = test::write_from("glb", sphere);
  const auto quantized = test::write_from("glb", sphere, R"({"quantize":true})");
  REQUIRE_OK(plain);
  REQUIRE_OK(quantized);
  CHECK(quantized->size() < plain->size());
  const auto back = test::read_as<ir::Mesh>("glb", *quantized);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::meshes_close(sphere, *back, 1e-4f), test::mesh_diff(sphere, *back, 1e-4f));
  const auto small = test::write_from("glb", sphere, R"({"quality":30})");
  REQUIRE_OK(small);
  CHECK(small->size() <= plain->size());
  ir::Mesh tagged = test::scenes::unit_cube();
  tagged.metadata["copyright"] = "CC0";
  const auto stripped = test::write_from("gltf", tagged);
  const auto kept = test::write_from("gltf", tagged, R"({"strip_metadata":false})");
  REQUIRE_OK(stripped);
  REQUIRE_OK(kept);
  CHECK(test::str(*stripped).find("CC0") == std::string::npos);
  CHECK(test::str(*kept).find("CC0") != std::string::npos);
}

TEST_CASE("mesh/gltf: hostile input fails cleanly") {
  const std::string cyclic = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"children":[1]},{"children":[0]}]})";
  CHECK_FALSE(test::read_as<ir::Mesh>("gltf", test::bytes(cyclic)).has_value());
  const std::string past_end = R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
                               R"("accessors":[{"bufferView":0,"componentType":5126,"count":1000000,"type":"VEC3"}],)"
                               R"("bufferViews":[{"buffer":0,"byteLength":12}],"buffers":[{"byteLength":12,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAA"}]})";
  CHECK_STATUS(test::read_as<ir::Mesh>("gltf", test::bytes(past_end)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("gltf", test::bytes(R"({"asset":{"version":"1.0"}})")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Mesh>("glb", test::fixture("mesh/gltf/cube_blender.glb")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("gltf", test::fixture("mesh/gltf/edge_cases_pygltflib.gltf")) > 0);
  CHECK(test::convert(test::fixture("mesh/gltf/cube_blender.glb"), "", "glb").ok());
  CHECK(test::convert(test::fixture("mesh/gltf/edge_cases_pygltflib.gltf"), "", "obj").ok());
}

// ------------------------------------------------------------------------- 3MF / AMF

TEST_CASE("mesh/3mf: assimp and trimesh files") {
  const ir::Mesh assimp = read_fixture("3mf", "mesh/3mf/two_objects_assimp.3mf");
  CHECK(mesh::triangle_count(assimp) == 236);
  CHECK(assimp.source_up_axis == "Z");
  CHECK_APPROX(mesh::bounds(assimp).size().x, 5.0, 1e-5);
  const ir::Mesh trimesh = read_fixture("3mf", "mesh/3mf/two_objects_trimesh.3mf");
  CHECK(mesh::triangle_count(trimesh) == 236);
  REQUIRE(trimesh.objects.size() == 2);
  CHECK_APPROX(mesh::bounds(trimesh).min.x, -2.5, 1e-5);
  CHECK_APPROX(mesh::bounds(trimesh).max.x, 2.5, 1e-5);
}

TEST_CASE("mesh/3mf: components, build transforms, base materials and colour groups") {
  const ir::Mesh m = read_fixture("3mf", "mesh/3mf/components_materials_hand.3mf");
  CHECK(m.unit_meters == 0.001);
  CHECK(mesh::triangle_count(m) == 36);
  const mesh::Bounds box = mesh::bounds(m);
  CHECK_APPROX(box.min.x, -30.0, 1e-4);
  CHECK_APPROX(box.max.x, 30.0, 1e-4);
  CHECK_APPROX(box.min.y, 0.0, 1e-4);
  CHECK_APPROX(box.max.y, 15.0, 1e-4);
  CHECK_APPROX(box.min.z, -10.0, 1e-4);
  CHECK_APPROX(box.max.z, 0.0, 1e-4);
  REQUIRE(m.find_material("Red") != nullptr);
  REQUIRE(m.find_material("Blue") != nullptr);
  CHECK(m.find_material("Red")->base_color.r == 1.0f);
  CHECK(m.colors.size() == m.positions.size());
  CHECK(m.metadata.at("Title") == "Synthetic cubes");
  REQUIRE(m.objects.size() == 2);
  CHECK(m.objects[0].name == "Pair");
  CHECK(m.objects[1].name == "Painted");
}

TEST_CASE("mesh/3mf: round trips keep objects, transforms, materials, colours and units") {
  // A Y-up source (as from glTF) is rotated to Z-up on the way out and back on the way in.
  ir::Mesh scene = test::scenes::two_objects();
  scene.source_up_axis = "Y";
  const ir::Mesh back = test::scenes::round_trip("3mf", scene);
  CHECK_MESSAGE(test::meshes_close(scene, back, 1e-5f), test::mesh_diff(scene, back, 1e-5f));
  REQUIRE(back.objects.size() == 2);
  CHECK_APPROX(back.objects[0].transform.m[12], 2.0, 1e-6);
  ir::Mesh colored = test::scenes::textured_quad();
  colored.unit_meters = 0.001;
  const ir::Mesh colored_back = test::scenes::round_trip("3mf", colored);
  REQUIRE(colored_back.colors.size() == colored_back.positions.size());
  CHECK(colored_back.unit_meters == 0.001);
  const ir::Mesh materials = test::scenes::round_trip("3mf", test::scenes::read_fixture("3mf", "mesh/3mf/components_materials_hand.3mf"));
  CHECK(materials.find_material("Blue") != nullptr);
  // Units: metres in, millimetres out.
  ir::Mesh metres = test::scenes::unit_cube();
  metres.unit_meters = 1.0;
  const ir::Mesh mm = test::scenes::round_trip("3mf", metres, R"({"unit":"mm"})");
  CHECK_APPROX(mesh::bounds(mm).size().x, 1000.0, 1e-3);
  CHECK(mm.unit_meters == 0.001);
  CHECK(test::fuzz_reader<ir::Mesh>("3mf", test::fixture("mesh/3mf/components_materials_hand.3mf")) > 0);
  CHECK(test::convert(test::fixture("mesh/3mf/two_objects_trimesh.3mf"), "", "3mf").ok());
  CHECK(test::convert(test::fixture("mesh/3mf/two_objects_trimesh.3mf"), "", "stl").ok());
}

TEST_CASE("mesh/3mf: hostile packages fail cleanly") {
  // A component that refers to itself.
  const std::string model = R"(<?xml version="1.0"?><model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">)"
                            R"(<resources><object id="1"><components><component objectid="1"/></components></object></resources>)"
                            R"(<build><item objectid="1"/></build></model>)";
  util::ZipWriter zip;
  REQUIRE_OK(zip.add("3D/3dmodel.model", model));
  const auto package = zip.finish();
  REQUIRE_OK(package);
  CHECK_STATUS(test::read_as<ir::Mesh>("3mf", *package), Status::Decode);
}

TEST_CASE("mesh/amf: plain and zipped files with constellations") {
  for (const char* name : {"mesh/amf/cubes_hand.amf", "mesh/amf/cubes_hand_zipped.amf"}) {
    const ir::Mesh m = read_fixture("amf", name);
    CHECK(mesh::triangle_count(m) == 24);
    CHECK_APPROX(mesh::bounds(m).min.x, -3.5, 1e-5);
    CHECK_APPROX(mesh::bounds(m).max.x, 3.5, 1e-5);
    CHECK(m.find_material("Red") != nullptr);
    CHECK(m.colors.size() == m.positions.size());
    CHECK(m.objects.size() == 2);
  }
  ir::Mesh sphere = test::scenes::uv_sphere();
  sphere.source_up_axis = "Y";
  const ir::Mesh back = test::scenes::round_trip("amf", sphere);
  CHECK_MESSAGE(test::meshes_close(sphere, back, 0.0f), test::mesh_diff(sphere, back, 0.0f));
  const auto plain = test::write_from("amf", sphere);
  const auto zipped = test::write_from("amf", sphere, R"({"compress":true})");
  REQUIRE_OK(plain);
  REQUIRE_OK(zipped);
  CHECK(zipped->size() < plain->size() / 2);
  CHECK(test::fuzz_reader<ir::Mesh>("amf", test::fixture("mesh/amf/cubes_hand.amf")) > 0);
  CHECK(test::convert(test::fixture("mesh/amf/cubes_hand.amf"), "", "amf").ok());
}
