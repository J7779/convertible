// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::mesh {
Result<Bytes> lzf_decompress(ByteView input, std::size_t expected);
Bytes lzf_compress(ByteView input);
}  // namespace cvt::mesh

using namespace cvt;
using test::scenes::read_fixture;

namespace {
const ir::VertexAttribute* attribute(const ir::Mesh& m, std::string_view name) {
  for (const ir::VertexAttribute& a : m.attributes) {
    if (a.name == name) return &a;
  }
  return nullptr;
}
bool colors_close(const std::vector<ir::Color>& a, const std::vector<ir::Color>& b, float tolerance) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i].r - b[i].r) > tolerance || std::abs(a[i].g - b[i].g) > tolerance || std::abs(a[i].b - b[i].b) > tolerance) {
      return false;
    }
  }
  return true;
}
}  // namespace

TEST_CASE("mesh/xyz: Open3D point lists (xyz, xyzn, xyzrgb, pts)") {
  const ir::Mesh grid = test::scenes::point_grid();
  const ir::Mesh plain = test::scenes::read_fixture("xyz", "mesh/xyz/points_open3d.xyz");
  CHECK(plain.point_cloud);
  CHECK_MESSAGE(test::meshes_close(plain, grid, 1e-6f), test::mesh_diff(plain, grid, 1e-6f));
  // The extension decides between normals and float colours; give the reader the file name.
  const auto with_name = [](std::string_view fixture, std::string_view filename) {
    const test::Converted c = test::convert(test::fixture(fixture), "xyz", "ply", R"({"binary":false})", filename);
    REQUIRE(c.ok());
    return *test::read_as<ir::Mesh>("ply", c.data);
  };
  const ir::Mesh normals = with_name("mesh/xyz/points_open3d.xyzn", "points.xyzn");
  REQUIRE(normals.normals.size() == 100);
  CHECK(normals.normals[5].z == 1.0f);
  const ir::Mesh rgb = with_name("mesh/xyz/points_open3d.xyzrgb", "points.xyzrgb");
  CHECK(colors_close(rgb.colors, grid.colors, 1e-3f));
  const ir::Mesh pts = test::scenes::read_fixture("xyz", "mesh/xyz/points_open3d.pts");
  CHECK(pts.colors == grid.colors);
  REQUIRE(attribute(pts, "intensity") != nullptr);
}

TEST_CASE("mesh/pcd: Open3D ascii, binary and LZF-compressed files") {
  const ir::Mesh grid = test::scenes::point_grid();
  for (const char* name : {"mesh/pcd/points_open3d_ascii.pcd", "mesh/pcd/points_open3d_binary.pcd", "mesh/pcd/points_open3d_compressed.pcd"}) {
    const ir::Mesh m = read_fixture("pcd", name);
    CHECK(m.point_cloud);
    CHECK_MESSAGE(test::meshes_close(m, grid, 1e-6f), test::mesh_diff(m, grid, 1e-6f));
    CHECK(m.colors == grid.colors);
    REQUIRE(m.normals.size() == 100);
    CHECK(m.normals[0].z == 1.0f);
  }
}

TEST_CASE("mesh/pcd: round trips in every encoding, LZF codec") {
  ir::Mesh grid = test::scenes::point_grid();
  grid.attributes.push_back({"intensity", std::vector<float>(100, 7.0f)});
  for (const char* data : {"ascii", "binary", "binary_compressed"}) {
    const ir::Mesh back = test::scenes::round_trip("pcd", grid, std::format(R"({{"data":"{}"}})", data));
    CHECK_MESSAGE(test::meshes_close(back, grid, 0.0f), test::mesh_diff(back, grid, 0.0f));
    CHECK(back.colors == grid.colors);
    REQUIRE(attribute(back, "intensity") != nullptr);
  }
  const Bytes text = test::random_bytes(3000, 7);
  Bytes repetitive;
  for (int i = 0; i < 4000; ++i) repetitive.push_back(static_cast<std::byte>("abcabcabd"[i % 9]));
  for (const Bytes& data : {text, repetitive, Bytes{}}) {
    const Bytes packed = mesh::lzf_compress(data);
    const auto unpacked = mesh::lzf_decompress(packed, data.size());
    REQUIRE_OK(unpacked);
    CHECK(*unpacked == data);
  }
  CHECK(mesh::lzf_compress(repetitive).size() < repetitive.size() / 4);
  CHECK_FALSE(mesh::lzf_decompress(test::bytes("\xE0\xFF\xFF"), 100).has_value());
  CHECK(test::fuzz_reader<ir::Mesh>("pcd", test::fixture("mesh/pcd/points_open3d_compressed.pcd")) > 0);
}

TEST_CASE("mesh/las: laspy files (1.2 format 3, 1.4 format 7 with extra bytes)") {
  const ir::Mesh grid = test::scenes::point_grid();
  for (const char* name : {"mesh/las/points_laspy_v12_f3.las", "mesh/las/points_laspy_v14_f7.las"}) {
    ir::Mesh m = read_fixture("las", name);
    CHECK(m.source_up_axis == "Z");
    mesh::convert_up_axis(m, mesh::UpAxis::Y, mesh::UpAxis::Z);  // back to the file's axes
    mesh::transform(m, mesh::translation(-1000.0f, -2000.0f, 0.0f));
    CHECK_MESSAGE(test::meshes_close(m, grid, 2e-3f), test::mesh_diff(m, grid, 2e-3f));
    CHECK(m.colors == grid.colors);
    REQUIRE(attribute(m, "intensity") != nullptr);
    CHECK(attribute(m, "intensity")->values[7] == 70.0f);
    REQUIRE(attribute(m, "classification") != nullptr);
    CHECK(attribute(m, "classification")->values[0] == 2.0f);
    REQUIRE(attribute(m, "gps_time") != nullptr);
    CHECK(attribute(m, "gps_time")->values[3] == 1.5f);
  }
}

TEST_CASE("mesh/las: round trips (1.2 and 1.4), compress path, hostile headers") {
  ir::Mesh grid = test::scenes::point_grid();
  grid.source_up_axis = "Y";
  grid.attributes.push_back({"intensity", std::vector<float>(100, 300.0f)});
  for (const char* version : {"1.2", "1.4"}) {
    const ir::Mesh back = test::scenes::round_trip("las", grid, std::format(R"({{"las_version":"{}"}})", version));
    CHECK_MESSAGE(test::meshes_close(back, grid, 1e-5f), test::mesh_diff(back, grid, 1e-5f));
    CHECK(back.colors == grid.colors);
    REQUIRE(attribute(back, "intensity") != nullptr);
    CHECK(attribute(back, "intensity")->values[0] == 300.0f);
  }
  CHECK(test::convert(test::fixture("mesh/las/points_laspy_v14_f7.las"), "", "las").ok());
  Bytes laz = test::fixture("mesh/las/points_laspy_v12_f3.las");
  laz[104] = std::byte{0x83};  // compressed bit set
  CHECK_STATUS(test::read_as<ir::Mesh>("las", laz), Status::Decode);
  Bytes huge = test::fixture("mesh/las/points_laspy_v12_f3.las");
  huge[107] = huge[108] = huge[109] = std::byte{0xFF};
  CHECK_STATUS(test::read_as<ir::Mesh>("las", huge), Status::Decode);
  CHECK(test::fuzz_reader<ir::Mesh>("las", test::fixture("mesh/las/points_laspy_v14_f7.las")) > 0);
}

TEST_CASE("mesh/xyz: round trips and detection") {
  const ir::Mesh grid = test::scenes::point_grid();
  const ir::Mesh back = test::scenes::round_trip("xyz", grid);
  CHECK_MESSAGE(test::meshes_close(back, grid, 0.0f), test::mesh_diff(back, grid, 0.0f));
  CHECK(back.colors == grid.colors);
  // Meshes become their vertices.
  const auto cube = test::write_from("xyz", test::scenes::unit_cube());
  REQUIRE_OK(cube);
  const std::string lines = test::str(*cube);
  CHECK(std::count(lines.begin(), lines.end(), '\n') == 8);
  CHECK(test::convert(test::fixture("mesh/xyz/points_open3d.xyz"), "", "xyz", "", "points.xyz").ok());
  const test::Converted prose = test::convert(test::bytes("The quick brown fox\njumps over\nthe lazy dog\n"), "", "ply");
  CHECK_FALSE(prose.ok());
  CHECK(test::fuzz_reader<ir::Mesh>("xyz", test::fixture("mesh/xyz/points_open3d.pts")) > 0);
}

TEST_CASE("mesh/vtk: meshio files (4.2 ASCII, 4.2 and 5.1 binary)") {
  const ir::Mesh cube = read_fixture("vtk", "mesh/vtk/cube_meshio_42_ascii.vtk");
  CHECK(cube.face_count() == 6);
  CHECK_APPROX(mesh::signed_volume(cube), 1.0, 1e-9);
  REQUIRE(attribute(cube, "height") != nullptr);
  for (const char* name : {"mesh/vtk/sphere_meshio_42_binary.vtk", "mesh/vtk/sphere_meshio_51_binary.vtk"}) {
    const ir::Mesh sphere = read_fixture("vtk", name);
    CHECK(mesh::triangle_count(sphere) == 224);
    CHECK_MESSAGE(test::meshes_close(sphere, test::scenes::uv_sphere(), 1e-6f),
                  test::mesh_diff(sphere, test::scenes::uv_sphere(), 1e-6f));
  }
}

TEST_CASE("mesh/vtk: round trips in ASCII and binary") {
  ir::Mesh quad = test::scenes::textured_quad();
  quad.attributes.push_back({"temperature", {1, 2, 3, 4}});
  for (const char* options : {"", R"({"binary":true})"}) {
    for (const ir::Mesh& source : {test::scenes::uv_sphere(), quad, test::scenes::point_grid()}) {
      const ir::Mesh back = test::scenes::round_trip("vtk", source, options);
      CHECK_MESSAGE(test::meshes_close(source, back, 0.0f), test::mesh_diff(source, back, 0.0f));
      CHECK(back.texcoords == source.texcoords);
      CHECK(back.attributes.size() == source.attributes.size());
      CHECK(back.colors.size() == source.colors.size());
    }
  }
  CHECK(test::fuzz_reader<ir::Mesh>("vtk", test::fixture("mesh/vtk/sphere_meshio_51_binary.vtk")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("vtk", test::fixture("mesh/vtk/cube_meshio_42_ascii.vtk")) > 0);
  CHECK(test::convert(test::fixture("mesh/vtk/cube_meshio_42_ascii.vtk"), "", "vtk").ok());
}

TEST_CASE("mesh/msh: Gmsh 2.2 (ASCII, binary) and 4.1 surface meshes") {
  for (const char* name : {"mesh/msh/plate_gmsh22_ascii.msh", "mesh/msh/plate_gmsh22_binary.msh", "mesh/msh/plate_gmsh41_ascii.msh",
                           "mesh/msh/plate_quads_gmsh41.msh"}) {
    const ir::Mesh m = read_fixture("msh", name);
    CHECK_APPROX(mesh::surface_area(m), 2.0, 1e-9);
    const mesh::Bounds box = mesh::bounds(m);
    CHECK(box.max.x == 2.0f);
    CHECK(box.max.y == 1.0f);
    CHECK(m.face_count() > 4);
  }
  const ir::Mesh named = read_fixture("msh", "mesh/msh/plate_gmsh22_ascii.msh");
  REQUIRE(named.groups.size() == 1);
  CHECK(named.groups[0].name == "Plate");
  const ir::Mesh quads = read_fixture("msh", "mesh/msh/plate_quads_gmsh41.msh");
  CHECK_FALSE(quads.is_triangulated());
  CHECK(test::fuzz_reader<ir::Mesh>("msh", test::fixture("mesh/msh/plate_gmsh22_binary.msh")) > 0);
  CHECK(test::fuzz_reader<ir::Mesh>("msh", test::fixture("mesh/msh/plate_gmsh41_ascii.msh")) > 0);
  CHECK(test::convert(test::fixture("mesh/msh/plate_gmsh41_ascii.msh"), "", "stl").ok());
}
