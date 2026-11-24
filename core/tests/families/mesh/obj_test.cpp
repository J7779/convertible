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

TEST_CASE("mesh/obj: Blender cube, sphere and star") {
  const ir::Mesh cube = read_fixture("obj", "mesh/obj/cube_blender.obj");
  CHECK(cube.face_count() == 6);
  CHECK(cube.positions.size() == 24);  // flat normals split every corner
  CHECK(cube.normals.size() == 24);
  CHECK_APPROX(mesh::surface_area(cube), 6.0, 1e-5);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-5);
  CHECK_MESSAGE(test::meshes_close(cube, test::scenes::unit_cube(), 1e-5f), test::mesh_diff(cube, test::scenes::unit_cube(), 1e-5f));

  const ir::Mesh sphere = read_fixture("obj", "mesh/obj/sphere_blender.obj");
  CHECK(mesh::triangle_count(sphere) == 224);
  CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-5f),
                test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-5f));
  CHECK(mesh::signed_volume(sphere) > 3.0);

  const ir::Mesh star = read_fixture("obj", "mesh/obj/star_blender.obj");
  CHECK(star.face_sizes == std::vector<std::uint32_t>{10});
  CHECK_APPROX(mesh::surface_area(star), 1.17557, 1e-4);
}

TEST_CASE("mesh/obj: objects, groups and missing materials") {
  std::vector<std::string> warnings;
  const ir::Mesh scene = read_fixture("obj", "mesh/obj/two_objects_blender.obj", &warnings);
  REQUIRE(scene.objects.size() == 2);
  CHECK(scene.objects[0].name == "Box");
  CHECK(scene.objects[1].name == "Ball");
  const mesh::Bounds box = mesh::bounds(scene);
  CHECK_APPROX(box.min.x, -2.5, 1e-5);
  CHECK_APPROX(box.max.x, 2.5, 1e-5);
  CHECK(std::any_of(scene.groups.begin(), scene.groups.end(), [](const ir::MeshGroup& g) { return g.material == "Red"; }));
  CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.find(".mtl") != std::string::npos; }));
  CHECK_MESSAGE(test::meshes_close(scene, test::scenes::two_objects(), 1e-5f),
                test::mesh_diff(scene, test::scenes::two_objects(), 1e-5f));
}

TEST_CASE("mesh/obj: vertex colours (MeshLab) and trimesh normals") {
  const ir::Mesh cube = read_fixture("obj", "mesh/obj/cube_meshlab_colors.obj");
  REQUIRE(cube.colors.size() == cube.positions.size());
  REQUIRE(cube.positions.size() == 8);
  const std::vector<ir::Vec3> corners = test::scenes::unit_cube().positions;
  for (std::size_t i = 0; i < 8; ++i) {
    // Corner j of the generated cube was coloured red = bit 0, green = bit 1, blue = bit 2 of j.
    const auto j = static_cast<std::size_t>(std::find(corners.begin(), corners.end(), cube.positions[i]) - corners.begin());
    REQUIRE(j < 8);
    CHECK(cube.colors[i].r == ((j & 1) ? 1.0f : 0.0f));
    CHECK(cube.colors[i].g == ((j & 2) ? 1.0f : 0.0f));
    CHECK(cube.colors[i].b == ((j & 4) ? 1.0f : 0.0f));
  }
  const ir::Mesh sphere = read_fixture("obj", "mesh/obj/sphere_trimesh.obj");
  CHECK(sphere.positions.size() == 114);
  CHECK(sphere.normals.size() == 114);
  CHECK(mesh::triangle_count(sphere) == 224);
}

TEST_CASE("mesh/obj.zip: materials and textures travel with the model") {
  const ir::Mesh quad = read_fixture("obj.zip", "mesh/obj/quad_blender.obj.zip");
  REQUIRE(quad.materials.size() == 1);
  CHECK(quad.materials[0].name == "Checker");
  REQUIRE(quad.materials[0].base_color_texture == 0);
  REQUIRE(quad.textures.size() == 1);
  CHECK(quad.textures[0].mime == "image/png");
  CHECK(quad.textures[0].data.size() == 81);
  REQUIRE(quad.texcoords.size() == 4);
  REQUIRE(quad.colors.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    const ir::Vec3& p = quad.positions[i];
    CHECK(quad.texcoords[i].x == (p.x > 0 ? 1.0f : 0.0f));
    CHECK(quad.texcoords[i].y == (p.y > 0 ? 1.0f : 0.0f));
  }
  // Our zip writer keeps all of it.
  const auto zipped = test::write_from("obj.zip", quad);
  REQUIRE_OK(zipped);
  const auto back = test::read_as<ir::Mesh>("obj.zip", *zipped);
  REQUIRE_OK(back);
  REQUIRE(back->textures.size() == 1);
  CHECK(back->textures[0].data == quad.textures[0].data);
  CHECK(back->materials[0].base_color == quad.materials[0].base_color);
  CHECK_APPROX(back->materials[0].roughness, quad.materials[0].roughness, 1e-3);
}

TEST_CASE("mesh/obj: hand-written syntax variants") {
  SUBCASE("negative indices, v/vt, v//vn, continuation, comments") {
    const auto mesh = test::read_as<ir::Mesh>("obj", test::bytes("# test\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\n"
                                                              "vt 1 1\nvt 0 1\nf -4/-4 -3/-3 \\\n -2/-2 -1/-1\n"));
    REQUIRE_OK(mesh);
    CHECK(mesh->face_sizes == std::vector<std::uint32_t>{4});
    CHECK(mesh->texcoords.size() == 4);
  }
  SUBCASE("smoothing groups generate normals") {
    const auto mesh = test::read_as<ir::Mesh>(
        "obj", test::bytes("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\ns 1\nf 1 2 3\nf 1 3 4\ns off\nf 1 2 5\n"));
    REQUIRE_OK(mesh);
    CHECK(mesh->normals.size() == mesh->positions.size());
  }
  SUBCASE("vertices only") {
    const auto mesh = test::read_as<ir::Mesh>("obj", test::bytes("v 0 0 0\nv 1 0 0\nv 0 0 1\n"));
    REQUIRE_OK(mesh);
    CHECK(mesh->point_cloud);
  }
}

TEST_CASE("mesh/obj: round trips keep shape, polygons, uvs, normals and colours") {
  for (const ir::Mesh& source : {test::scenes::unit_cube(), test::scenes::uv_sphere(), test::scenes::concave_star(),
                                 test::scenes::textured_quad(), test::scenes::two_objects(), test::scenes::point_grid()}) {
    const auto encoded = test::write_from("obj", source);
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Mesh>("obj", *encoded);
    REQUIRE_OK(decoded);
    CHECK_MESSAGE(test::meshes_close(source, *decoded, 0.0f), test::mesh_diff(source, *decoded, 0.0f));
    CHECK(decoded->face_count() == source.face_count());
    CHECK(decoded->texcoords.size() == (source.texcoords.empty() ? 0 : decoded->positions.size()));
    CHECK(decoded->objects.size() == source.objects.size());
  }
  const auto rough = test::write_from("obj", test::scenes::uv_sphere(), R"({"precision":4})");
  const auto exact = test::write_from("obj", test::scenes::uv_sphere());
  REQUIRE_OK(rough);
  REQUIRE_OK(exact);
  CHECK(rough->size() < exact->size());
}

TEST_CASE("mesh/obj: damaged input fails cleanly") {
  CHECK_STATUS(test::read_as<ir::Mesh>("obj", test::bytes("v 0 0 0\nf 1 2 3\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("obj", test::bytes("v 0 0\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Mesh>("obj", test::bytes("f 1/a/2 1 1\n")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Mesh>("obj", test::fixture("mesh/obj/cube_blender.obj")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("obj.zip", test::fixture("mesh/obj/quad_blender.obj.zip")) > 0);
}

TEST_CASE("mesh/obj: detection") {
  const test::Converted obj = test::convert(test::fixture("mesh/obj/sphere_blender.obj"), "", "obj");
  CHECK(obj.ok());
  const test::Converted zip = test::convert(test::fixture("mesh/obj/quad_blender.obj.zip"), "", "obj.zip");
  CHECK(zip.ok());
  CHECK_FALSE(test::convert(test::bytes("vowels and verbs\nfor the people\n"), "", "obj").ok());
}
