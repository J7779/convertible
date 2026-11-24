// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>

#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/scenes.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

// Distance from p to triangle (a, b, c).
double point_triangle(const ir::Vec3& p, const ir::Vec3& a, const ir::Vec3& b, const ir::Vec3& c) {
  const auto d = [](const ir::Vec3& x) { return std::array<double, 3>{x.x, x.y, x.z}; };
  const auto P = d(p), A = d(a), B = d(b), C = d(c);
  const auto sub = [](auto x, auto y) { return std::array<double, 3>{x[0] - y[0], x[1] - y[1], x[2] - y[2]}; };
  const auto dot = [](auto x, auto y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  const auto ab = sub(B, A), ac = sub(C, A), ap = sub(P, A);
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  std::array<double, 3> closest{};
  const auto at = [&](double s, double t) {
    return std::array<double, 3>{A[0] + ab[0] * s + ac[0] * t, A[1] + ab[1] * s + ac[1] * t, A[2] + ab[2] * s + ac[2] * t};
  };
  if (d1 <= 0 && d2 <= 0) {
    closest = A;
  } else {
    const auto bp = sub(P, B);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    const auto cp = sub(P, C);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    const double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    if (d3 >= 0 && d4 <= d3) closest = B;
    else if (d6 >= 0 && d5 <= d6) closest = C;
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) closest = at(d1 / (d1 - d3), 0);
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) closest = at(0, d2 / (d2 - d6));
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
      const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      closest = {B[0] + (C[0] - B[0]) * w, B[1] + (C[1] - B[1]) * w, B[2] + (C[2] - B[2]) * w};
    } else {
      const double denom = 1.0 / (va + vb + vc);
      closest = at(vb * denom, vc * denom);
    }
  }
  const auto e = sub(P, closest);
  return std::sqrt(dot(e, e));
}

// One-sided Hausdorff-style distance: max over sample points of `from`
// (vertices and face centroids) to the surface of `to`.
double surface_distance(const ir::Mesh& from, const ir::Mesh& to) {
  std::vector<ir::Vec3> samples = from.positions;
  const auto tri = from.triangle_indices();
  for (std::size_t i = 0; i + 2 < tri.size(); i += 3) {
    const ir::Vec3 &a = from.positions[tri[i]], &b = from.positions[tri[i + 1]], &c = from.positions[tri[i + 2]];
    samples.push_back({(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3});
  }
  const auto target = to.triangle_indices();
  double worst = 0;
  for (const ir::Vec3& p : samples) {
    double best = 1e30;
    for (std::size_t i = 0; i + 2 < target.size(); i += 3) {
      best = std::min(best, point_triangle(p, to.positions[target[i]], to.positions[target[i + 1]], to.positions[target[i + 2]]));
    }
    worst = std::max(worst, best);
  }
  return worst;
}

ir::Mesh grid(int n) {
  ir::Mesh m;
  for (int y = 0; y <= n; ++y) {
    for (int x = 0; x <= n; ++x) {
      const float fx = static_cast<float>(x) / n, fy = static_cast<float>(y) / n;
      m.positions.push_back({fx, 0.05f * std::sin(6.0f * fx) * std::cos(5.0f * fy), fy});
      m.texcoords.push_back({fx, fy});
    }
  }
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const auto i = static_cast<std::uint32_t>(y * (n + 1) + x);
      m.indices.insert(m.indices.end(), {i, i + static_cast<std::uint32_t>(n + 1), i + 1, i + 1, i + static_cast<std::uint32_t>(n + 1),
                                         i + static_cast<std::uint32_t>(n + 2)});
    }
  }
  return m;
}

}  // namespace

TEST_CASE("mesh/simplify: meets the triangle target with a small surface error") {
  ir::Mesh sphere = test::scenes::uv_sphere(64, 32);
  mesh::triangulate(sphere);
  const ir::Mesh original = sphere;
  const std::size_t before = mesh::triangle_count(sphere);
  REQUIRE(mesh::simplify(sphere, {.target_ratio = 0.25f}));
  const std::size_t after = mesh::triangle_count(sphere);
  CHECK(after <= before / 4 + 2);
  CHECK(after >= before / 4 - before / 20);
  CHECK(sphere.valid());
  const double diagonal = mesh::bounds(original).diagonal();
  CHECK(surface_distance(sphere, original) < 0.02 * diagonal);
  CHECK(surface_distance(original, sphere) < 0.02 * diagonal);
  CHECK_APPROX(mesh::signed_volume(sphere), mesh::signed_volume(original), 0.03 * mesh::signed_volume(original));
  // Still closed and outward: every edge shared by two faces.
  CHECK(mesh::fix_winding(sphere) == 0);
}

TEST_CASE("mesh/simplify: open borders stay in place") {
  ir::Mesh plane = grid(24);
  const ir::Mesh original = plane;
  REQUIRE(mesh::simplify(plane, {.target_ratio = 0.2f}));
  CHECK(mesh::triangle_count(plane) < mesh::triangle_count(original) / 3);
  // Every border vertex of the original is still present (borders are locked).
  std::size_t border = 0;
  for (const ir::Vec3& p : original.positions) {
    if (p.x != 0 && p.x != 1 && p.z != 0 && p.z != 1) continue;
    ++border;
    const bool found = std::any_of(plane.positions.begin(), plane.positions.end(), [&](const ir::Vec3& q) { return q == p; });
    CHECK(found);
  }
  CHECK(border == 96);
  CHECK_APPROX(mesh::surface_area(plane), mesh::surface_area(original), 0.01 * mesh::surface_area(original));
  REQUIRE(plane.texcoords.size() == plane.positions.size());
  for (std::size_t i = 0; i < plane.positions.size(); ++i) {
    CHECK_APPROX(plane.texcoords[i].x, plane.positions[i].x, 0.05);  // uv interpolated along collapses
  }
}

TEST_CASE("mesh/simplify: seams do not crack, groups survive, cancellation works") {
  ir::Mesh cube = test::scenes::uv_sphere(32, 16);
  mesh::generate_normals(cube, 0.0f);  // every face its own normals: all vertices are seams
  const std::size_t faces = mesh::triangle_count(cube);
  REQUIRE(mesh::simplify(cube, {.target_ratio = 0.5f}));
  CHECK(mesh::triangle_count(cube) == faces);  // nothing may move without cracking
  ir::Mesh scene = test::scenes::two_objects();
  mesh::simplify(scene, {.target_ratio = 0.5f});
  REQUIRE(scene.objects.size() == 2);
  CHECK(scene.objects[0].face_count + scene.objects[1].face_count == scene.face_count());
  CHECK(scene.valid());
  ir::Mesh big = test::scenes::uv_sphere(64, 32);
  CHECK_FALSE(mesh::simplify(big, {.target_ratio = 0.1f}, [](double) { return false; }));
  CHECK(big.valid());
}

TEST_CASE("mesh/simplify: degenerate inputs never crash") {
  for (ir::Mesh m : {ir::Mesh{}, test::scenes::concave_star(), test::scenes::point_grid()}) {
    mesh::simplify(m, {.target_ratio = 0.1f});
  }
  ir::Mesh bowtie;  // two triangles sharing one vertex, plus a duplicate face
  bowtie.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}};
  bowtie.indices = {0, 1, 2, 0, 3, 4, 0, 1, 2};
  mesh::simplify(bowtie, {.target_ratio = 0.1f});
  CHECK(bowtie.valid());
  ir::Mesh same = test::scenes::uv_sphere();
  mesh::triangulate(same);
  const ir::Mesh copy = same;
  mesh::simplify(same, {.target_ratio = 1.0f});
  CHECK(same.indices == copy.indices);
}

TEST_CASE("mesh/simplify: quality drives the writers") {
  const ir::Mesh sphere = test::scenes::uv_sphere(64, 32);
  std::size_t previous_size = std::numeric_limits<std::size_t>::max();
  std::size_t previous_triangles = std::numeric_limits<std::size_t>::max();
  for (const int quality : {90, 75, 50, 20}) {
    const auto encoded = test::write_from("ply", sphere, std::format(R"({{"quality":{}}})", quality));
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Mesh>("ply", *encoded);
    REQUIRE_OK(decoded);
    const std::size_t triangles = mesh::triangle_count(*decoded);
    CHECK(triangles < previous_triangles);
    CHECK(encoded->size() < previous_size);
    previous_triangles = triangles;
    previous_size = encoded->size();
  }
  CHECK(mesh::quality_to_ratio(90) == 1.0f);
  CHECK(mesh::quality_to_ratio(100) == 1.0f);
  CHECK(mesh::quality_to_ratio(50) < mesh::quality_to_ratio(75));
  CHECK(mesh::quality_to_ratio(1) >= 0.02f);
  // The preset counts as a quality choice too.
  const auto small = test::write_from("stl", sphere, R"({"preset":"small"})");
  const auto high = test::write_from("stl", sphere);
  REQUIRE_OK(small);
  REQUIRE_OK(high);
  CHECK(small->size() < high->size());
}

TEST_CASE("mesh/ops: vertex cache optimisation keeps the mesh and improves locality") {
  ir::Mesh m = grid(30);
  // Scramble the triangle order deterministically.
  const std::size_t faces = m.indices.size() / 3;
  for (std::size_t i = 0; i < faces; ++i) {
    const std::size_t j = (i * 7919) % faces;
    for (int k = 0; k < 3; ++k) std::swap(m.indices[i * 3 + k], m.indices[j * 3 + k]);
  }
  const auto acmr = [](const ir::Mesh& mesh) {
    std::vector<std::uint32_t> fifo;
    std::size_t misses = 0;
    for (const std::uint32_t v : mesh.indices) {
      if (std::find(fifo.begin(), fifo.end(), v) == fifo.end()) {
        ++misses;
        fifo.push_back(v);
        if (fifo.size() > 16) fifo.erase(fifo.begin());
      }
    }
    return static_cast<double>(misses) / static_cast<double>(mesh.indices.size() / 3);
  };
  const double before = acmr(m);
  const ir::Mesh copy = m;
  mesh::optimize_indices(m);
  CHECK(acmr(m) < before * 0.75);
  CHECK(mesh::triangle_count(m) == mesh::triangle_count(copy));
  CHECK_APPROX(mesh::surface_area(m), mesh::surface_area(copy), 1e-6);
  CHECK(test::meshes_close(m, copy, 0.0f));
}

TEST_CASE("mesh/simplify: flat-shaded models still simplify through the writers") {
  ir::Mesh faceted = test::scenes::uv_sphere(48, 24);
  mesh::generate_normals(faceted, 0.0f);  // every corner has its own normal
  const auto full = test::write_from("glb", faceted);
  const auto small = test::write_from("glb", faceted, R"({"quality":40})");
  REQUIRE_OK(full);
  REQUIRE_OK(small);
  const auto a = test::read_as<ir::Mesh>("glb", *full);
  const auto b = test::read_as<ir::Mesh>("glb", *small);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(mesh::triangle_count(*b) < mesh::triangle_count(*a) / 2);
  CHECK(b->normals.size() == b->positions.size());
  CHECK(small->size() < full->size() / 2);
}
