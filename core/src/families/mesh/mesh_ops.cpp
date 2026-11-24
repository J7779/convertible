// Convertible core - mesh family: geometry toolbox (measures, transforms,
// triangulation, normals, cleanup, bookkeeping).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/mesh_ops.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cvt::mesh {

namespace {

constexpr std::uint32_t dropped = std::numeric_limits<std::uint32_t>::max();

bool finite(const ir::Vec3& v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct DVec3 {
  double x = 0, y = 0, z = 0;
};
DVec3 to_d(const ir::Vec3& v) { return {v.x, v.y, v.z}; }
DVec3 dsub(const DVec3& a, const DVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DVec3 dcross(const DVec3& a, const DVec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double ddot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double dlen(const DVec3& a) { return std::sqrt(ddot(a, a)); }

// Newell normal in double precision for the corners `idx` of `positions`.
DVec3 newell(const std::vector<ir::Vec3>& positions, const std::uint32_t* idx, std::size_t n) {
  DVec3 normal;
  for (std::size_t i = 0; i < n; ++i) {
    const ir::Vec3& a = positions[idx[i]];
    const ir::Vec3& b = positions[idx[(i + 1) % n]];
    normal.x += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
    normal.y += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
    normal.z += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
  }
  return normal;
}

std::uint32_t face_size_at(const ir::Mesh& mesh, std::size_t face) {
  return mesh.face_sizes.empty() ? 3u : mesh.face_sizes[face];
}

// Exact-position ids: vertices with bit-identical positions share an id.
std::vector<std::uint32_t> position_ids(const std::vector<ir::Vec3>& positions, std::size_t* count = nullptr) {
  std::vector<std::uint32_t> order(positions.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint32_t>(i);
  const auto key = [&](std::uint32_t i) {
    const ir::Vec3& p = positions[i];
    // +0.0 and -0.0 are the same point.
    const float x = p.x == 0.0f ? 0.0f : p.x;
    const float y = p.y == 0.0f ? 0.0f : p.y;
    const float z = p.z == 0.0f ? 0.0f : p.z;
    return std::array<std::uint32_t, 3>{std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y),
                                        std::bit_cast<std::uint32_t>(z)};
  };
  std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
  std::vector<std::uint32_t> ids(positions.size(), 0);
  std::uint32_t next = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (i > 0 && key(order[i]) != key(order[i - 1])) ++next;
    ids[order[i]] = next;
  }
  if (count != nullptr) *count = positions.empty() ? 0 : next + 1;
  return ids;
}

template <class T>
void permute_vertex_array(std::vector<T>& values, std::span<const std::uint32_t> old_to_new, std::size_t new_count,
                          const T& fill) {
  if (values.empty()) return;
  std::vector<T> out(new_count, fill);
  std::vector<bool> set(new_count, false);
  for (std::size_t old = 0; old < values.size() && old < old_to_new.size(); ++old) {
    const std::uint32_t target = old_to_new[old];
    if (target == dropped || target >= new_count || set[target]) continue;
    out[target] = values[old];
    set[target] = true;
  }
  values = std::move(out);
}

}  // namespace

// --------------------------------------------------------------------------- vector helpers

ir::Vec3 sub(const ir::Vec3& a, const ir::Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
ir::Vec3 add(const ir::Vec3& a, const ir::Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
ir::Vec3 mul(const ir::Vec3& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
float dot(const ir::Vec3& a, const ir::Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
ir::Vec3 cross(const ir::Vec3& a, const ir::Vec3& b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const ir::Vec3& a) noexcept { return std::sqrt(dot(a, a)); }
ir::Vec3 normalize(const ir::Vec3& a) noexcept {
  const double len = std::sqrt(static_cast<double>(a.x) * a.x + static_cast<double>(a.y) * a.y +
                               static_cast<double>(a.z) * a.z);
  if (!(len > 0.0) || !std::isfinite(len)) return {0, 0, 0};
  return {static_cast<float>(a.x / len), static_cast<float>(a.y / len), static_cast<float>(a.z / len)};
}
ir::Vec3 polygon_normal(std::span<const ir::Vec3> points) noexcept {
  double nx = 0, ny = 0, nz = 0;
  const std::size_t n = points.size();
  for (std::size_t i = 0; i < n; ++i) {
    const ir::Vec3& a = points[i];
    const ir::Vec3& b = points[(i + 1) % n];
    nx += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
    ny += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
    nz += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
  }
  return {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)};
}

// --------------------------------------------------------------------------- measures

float Bounds::diagonal() const noexcept {
  if (empty) return 0.0f;
  return length(size());
}
float Bounds::longest_side() const noexcept {
  if (empty) return 0.0f;
  const ir::Vec3 s = size();
  return std::max({s.x, s.y, s.z});
}

Bounds bounds(std::span<const ir::Vec3> points) {
  Bounds box;
  for (const ir::Vec3& p : points) {
    if (!finite(p)) continue;
    if (box.empty) {
      box.min = box.max = p;
      box.empty = false;
      continue;
    }
    box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
    box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
  }
  return box;
}
Bounds bounds(const ir::Mesh& mesh) { return bounds(std::span<const ir::Vec3>(mesh.positions)); }

std::size_t triangle_count(const ir::Mesh& mesh) noexcept {
  if (mesh.face_sizes.empty()) return mesh.indices.size() / 3;
  std::size_t count = 0;
  for (const std::uint32_t size : mesh.face_sizes) count += size >= 3 ? size - 2 : 0;
  return count;
}

double surface_area(const ir::Mesh& mesh) {
  if (!mesh.valid()) return 0.0;
  double area = 0.0;
  std::size_t base = 0;
  const std::size_t faces = mesh.face_count();
  for (std::size_t f = 0; f < faces; ++f) {
    const std::uint32_t n = face_size_at(mesh, f);
    area += 0.5 * dlen(newell(mesh.positions, mesh.indices.data() + base, n));
    base += n;
  }
  return area;
}

double signed_volume(const ir::Mesh& mesh) {
  if (!mesh.valid()) return 0.0;
  const std::vector<std::uint32_t> triangles = mesh.triangle_indices();
  double volume = 0.0;
  for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
    const DVec3 a = to_d(mesh.positions[triangles[i]]);
    const DVec3 b = to_d(mesh.positions[triangles[i + 1]]);
    const DVec3 c = to_d(mesh.positions[triangles[i + 2]]);
    volume += ddot(a, dcross(b, c));
  }
  return volume / 6.0;
}

// --------------------------------------------------------------------------- matrices

ir::Mat4 inverse(const ir::Mat4& matrix, bool* ok) noexcept {
  const float* m = matrix.m;
  double inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0.0 || !std::isfinite(det)) {
    if (ok != nullptr) *ok = false;
    return {};
  }
  ir::Mat4 out;
  for (int i = 0; i < 16; ++i) out.m[i] = static_cast<float>(inv[i] / det);
  if (ok != nullptr) *ok = true;
  return out;
}

float determinant3(const ir::Mat4& a) noexcept {
  return a.at(0, 0) * (a.at(1, 1) * a.at(2, 2) - a.at(1, 2) * a.at(2, 1)) -
         a.at(0, 1) * (a.at(1, 0) * a.at(2, 2) - a.at(1, 2) * a.at(2, 0)) +
         a.at(0, 2) * (a.at(1, 0) * a.at(2, 1) - a.at(1, 1) * a.at(2, 0));
}

ir::Mat4 translation(float x, float y, float z) noexcept {
  ir::Mat4 m;
  m.m[12] = x;
  m.m[13] = y;
  m.m[14] = z;
  return m;
}
ir::Mat4 scaling(float x, float y, float z) noexcept {
  ir::Mat4 m;
  m.m[0] = x;
  m.m[5] = y;
  m.m[10] = z;
  return m;
}
ir::Mat4 rotation(float degrees, float x, float y, float z) noexcept {
  const ir::Vec3 axis = normalize({x, y, z});
  if (axis.x == 0 && axis.y == 0 && axis.z == 0) return {};
  const double radians = static_cast<double>(degrees) * std::numbers::pi / 180.0;
  const double half = radians / 2.0;
  const double s = std::sin(half);
  return rotation_quaternion(static_cast<float>(axis.x * s), static_cast<float>(axis.y * s),
                             static_cast<float>(axis.z * s), static_cast<float>(std::cos(half)));
}
ir::Mat4 rotation_quaternion(float qx, float qy, float qz, float qw) noexcept {
  double x = qx, y = qy, z = qz, w = qw;
  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(norm > 0.0) || !std::isfinite(norm)) return {};
  x /= norm;
  y /= norm;
  z /= norm;
  w /= norm;
  ir::Mat4 m;
  m.m[0] = static_cast<float>(1 - 2 * (y * y + z * z));
  m.m[1] = static_cast<float>(2 * (x * y + z * w));
  m.m[2] = static_cast<float>(2 * (x * z - y * w));
  m.m[4] = static_cast<float>(2 * (x * y - z * w));
  m.m[5] = static_cast<float>(1 - 2 * (x * x + z * z));
  m.m[6] = static_cast<float>(2 * (y * z + x * w));
  m.m[8] = static_cast<float>(2 * (x * z + y * w));
  m.m[9] = static_cast<float>(2 * (y * z - x * w));
  m.m[10] = static_cast<float>(1 - 2 * (x * x + y * y));
  return m;
}
ir::Mat4 from_row_major(std::span<const float, 16> values) noexcept {
  ir::Mat4 m;
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) m.m[column * 4 + row] = values[static_cast<std::size_t>(row * 4 + column)];
  }
  return m;
}

// --------------------------------------------------------------------------- transforms

void flip_winding(ir::Mesh& mesh) {
  std::size_t base = 0;
  const std::size_t faces = mesh.face_count();
  for (std::size_t f = 0; f < faces; ++f) {
    const std::uint32_t n = face_size_at(mesh, f);
    if (base + n > mesh.indices.size()) break;
    std::reverse(mesh.indices.begin() + static_cast<std::ptrdiff_t>(base + 1),
                 mesh.indices.begin() + static_cast<std::ptrdiff_t>(base + n));
    base += n;
  }
}

void transform(ir::Mesh& mesh, const ir::Mat4& matrix) {
  if (matrix.is_identity()) return;
  for (ir::Vec3& p : mesh.positions) p = matrix.transform_point(p);
  if (!mesh.normals.empty()) {
    bool ok = false;
    const ir::Mat4 inv = inverse(matrix, &ok);
    ir::Mat4 normal_matrix;  // inverse transpose, 3x3 part
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) normal_matrix.m[c * 4 + r] = ok ? inv.m[r * 4 + c] : matrix.m[c * 4 + r];
    }
    for (ir::Vec3& n : mesh.normals) n = normalize(normal_matrix.transform_vector(n));
  }
  if (determinant3(matrix) < 0.0f) flip_winding(mesh);
}

void scale(ir::Mesh& mesh, float factor) {
  if (factor == 1.0f || !std::isfinite(factor)) return;
  for (ir::Vec3& p : mesh.positions) p = mul(p, factor);
  if (factor < 0.0f) {
    for (ir::Vec3& n : mesh.normals) n = mul(n, -1.0f);
    flip_winding(mesh);
  }
}

void center(ir::Mesh& mesh) {
  const Bounds box = bounds(mesh);
  if (box.empty) return;
  const ir::Vec3 c = box.center();
  for (ir::Vec3& p : mesh.positions) p = sub(p, c);
}

void scale_to_unit(ir::Mesh& mesh) {
  center(mesh);
  const float side = bounds(mesh).longest_side();
  if (side > 0.0f && std::isfinite(side)) scale(mesh, 1.0f / side);
}

void convert_up_axis(ir::Mesh& mesh, UpAxis from, UpAxis to) {
  if (from == to) return;
  const auto z_to_y = [](const ir::Vec3& v) { return ir::Vec3{v.x, v.z, -v.y}; };
  const auto y_to_z = [](const ir::Vec3& v) { return ir::Vec3{v.x, -v.z, v.y}; };
  const bool zy = from == UpAxis::Z;
  for (ir::Vec3& p : mesh.positions) p = zy ? z_to_y(p) : y_to_z(p);
  for (ir::Vec3& n : mesh.normals) n = zy ? z_to_y(n) : y_to_z(n);
  ir::Mat4 basis;  // the same rotation as a matrix, to conjugate object transforms
  if (zy) {
    basis.m[5] = 0;
    basis.m[6] = -1;
    basis.m[9] = 1;
    basis.m[10] = 0;
  } else {
    basis.m[5] = 0;
    basis.m[6] = 1;
    basis.m[9] = -1;
    basis.m[10] = 0;
  }
  const ir::Mat4 basis_inverse = inverse(basis);
  for (ir::MeshObject& object : mesh.objects) object.transform = basis * object.transform * basis_inverse;
}

void convert_units(ir::Mesh& mesh, double to_meters) {
  if (!(to_meters > 0.0) || !std::isfinite(to_meters)) return;
  if (mesh.unit_meters > 0.0 && mesh.unit_meters != to_meters) {
    const auto factor = static_cast<float>(mesh.unit_meters / to_meters);
    scale(mesh, factor);
    for (ir::MeshObject& object : mesh.objects) {
      object.transform.m[12] *= factor;
      object.transform.m[13] *= factor;
      object.transform.m[14] *= factor;
    }
  }
  mesh.unit_meters = to_meters;
}

// --------------------------------------------------------------------------- triangulation

namespace {

struct P2 {
  double x = 0, y = 0;
};

double cross2(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Point p inside or on triangle (a, b, c) of orientation `sign` (+1 ccw).
bool in_triangle(const P2& p, const P2& a, const P2& b, const P2& c, double sign, double eps) {
  return sign * cross2(a, b, p) >= -eps && sign * cross2(b, c, p) >= -eps && sign * cross2(c, a, p) >= -eps;
}

std::vector<std::uint32_t> ear_clip(const std::vector<P2>& pts) {
  const std::size_t n = pts.size();
  std::vector<std::uint32_t> out;
  if (n < 3) return out;
  out.reserve((n - 2) * 3);
  if (n == 3) return {0, 1, 2};

  double area2 = 0.0;
  double extent = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const P2& a = pts[i];
    const P2& b = pts[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
    extent = std::max({extent, std::abs(a.x), std::abs(a.y)});
  }
  const double sign = area2 < 0.0 ? -1.0 : 1.0;
  const double eps = std::max(extent * extent, 1e-300) * 1e-12;

  if (n == 4) {
    // Convex: same split as a fan; concave at corner 1 or 3: split there.
    const bool reflex1 = sign * cross2(pts[0], pts[1], pts[2]) < 0;
    const bool reflex3 = sign * cross2(pts[2], pts[3], pts[0]) < 0;
    if (reflex1 || reflex3) return {0, 1, 3, 1, 2, 3};
    return {0, 1, 2, 0, 2, 3};
  }

  std::vector<std::uint32_t> prev(n);
  std::vector<std::uint32_t> next(n);
  for (std::size_t i = 0; i < n; ++i) {
    prev[i] = static_cast<std::uint32_t>((i + n - 1) % n);
    next[i] = static_cast<std::uint32_t>((i + 1) % n);
  }
  std::size_t remaining = n;
  std::uint32_t current = 0;
  std::size_t stalled = 0;

  const auto is_ear = [&](std::uint32_t i) {
    const std::uint32_t a = prev[i];
    const std::uint32_t c = next[i];
    const double turn = sign * cross2(pts[a], pts[i], pts[c]);
    if (turn <= eps) return false;  // reflex or flat
    for (std::uint32_t k = next[c]; k != a; k = next[k]) {
      const P2& p = pts[k];
      // Corners that coincide with the triangle's own corners do not block it.
      const auto same = [&](std::uint32_t v) { return p.x == pts[v].x && p.y == pts[v].y; };
      if (same(a) || same(i) || same(c)) continue;
      if (in_triangle(p, pts[a], pts[i], pts[c], sign, eps)) return false;
    }
    return true;
  };
  const auto clip = [&](std::uint32_t i) {
    out.push_back(prev[i]);
    out.push_back(i);
    out.push_back(next[i]);
    next[prev[i]] = next[i];
    prev[next[i]] = prev[i];
    --remaining;
    current = next[i];
    stalled = 0;
  };

  while (remaining > 3) {
    if (is_ear(current)) {
      clip(current);
      continue;
    }
    current = next[current];
    if (++stalled < remaining) continue;
    // A full turn without an ear: the polygon is degenerate or self-
    // intersecting. Clip a flat corner first (keeps the shape), else the
    // most convex corner, else any corner.
    std::uint32_t best = current;
    double best_turn = -std::numeric_limits<double>::infinity();
    std::uint32_t flat = dropped;
    std::uint32_t k = current;
    for (std::size_t step = 0; step < remaining; ++step, k = next[k]) {
      const double turn = sign * cross2(pts[prev[k]], pts[k], pts[next[k]]);
      if (std::abs(turn) <= eps && flat == dropped) flat = k;
      if (turn > best_turn) {
        best_turn = turn;
        best = k;
      }
    }
    clip(flat != dropped ? flat : best);
  }
  out.push_back(prev[current]);
  out.push_back(current);
  out.push_back(next[current]);
  return out;
}

}  // namespace

std::vector<std::uint32_t> triangulate_polygon_2d(std::span<const ir::Vec2> points) {
  std::vector<P2> pts;
  pts.reserve(points.size());
  for (const ir::Vec2& p : points) pts.push_back({std::isfinite(p.x) ? p.x : 0.0, std::isfinite(p.y) ? p.y : 0.0});
  return ear_clip(pts);
}

std::vector<std::uint32_t> triangulate_polygon(std::span<const ir::Vec3> points) {
  const std::size_t n = points.size();
  if (n < 3) return {};
  if (n == 3) return {0, 1, 2};
  // Project onto the plane of the Newell normal with a basis (u, v) such
  // that a polygon counter-clockwise about the normal stays counter-clockwise.
  DVec3 normal;
  DVec3 centroid;
  for (std::size_t i = 0; i < n; ++i) {
    const ir::Vec3& a = points[i];
    const ir::Vec3& b = points[(i + 1) % n];
    normal.x += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
    normal.y += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
    normal.z += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
    centroid.x += a.x;
    centroid.y += a.y;
    centroid.z += a.z;
  }
  centroid = {centroid.x / static_cast<double>(n), centroid.y / static_cast<double>(n),
              centroid.z / static_cast<double>(n)};
  double len = dlen(normal);
  if (!(len > 0.0) || !std::isfinite(len)) {
    normal = {0, 0, 1};
    len = 1;
  }
  normal = {normal.x / len, normal.y / len, normal.z / len};
  const DVec3 helper = std::abs(normal.x) < 0.9 ? DVec3{1, 0, 0} : DVec3{0, 1, 0};
  DVec3 u = dcross(helper, normal);
  const double ulen = dlen(u);
  u = {u.x / ulen, u.y / ulen, u.z / ulen};
  const DVec3 v = dcross(normal, u);
  std::vector<P2> pts;
  pts.reserve(n);
  for (const ir::Vec3& p : points) {
    const DVec3 d = dsub(to_d(p), centroid);
    const double x = ddot(d, u);
    const double y = ddot(d, v);
    pts.push_back({std::isfinite(x) ? x : 0.0, std::isfinite(y) ? y : 0.0});
  }
  return ear_clip(pts);
}

void triangulate(ir::Mesh& mesh) {
  if (mesh.face_sizes.empty()) return;
  if (mesh.is_triangulated()) {
    mesh.face_sizes.clear();
    return;
  }
  const std::size_t faces = mesh.face_sizes.size();
  std::vector<std::uint32_t> triangles;
  triangles.reserve(mesh.indices.size() * 2);
  std::vector<std::size_t> first_new(faces + 1, 0);
  std::vector<ir::Vec3> corners;
  std::size_t base = 0;
  for (std::size_t f = 0; f < faces; ++f) {
    first_new[f] = triangles.size() / 3;
    const std::uint32_t n = mesh.face_sizes[f];
    if (base + n > mesh.indices.size()) break;
    const std::uint32_t* idx = mesh.indices.data() + base;
    if (n == 3) {
      triangles.insert(triangles.end(), idx, idx + 3);
    } else if (n > 3) {
      corners.clear();
      bool usable = true;
      for (std::uint32_t k = 0; k < n; ++k) {
        if (idx[k] >= mesh.positions.size()) usable = false;
        corners.push_back(usable ? mesh.positions[idx[k]] : ir::Vec3{});
      }
      if (usable) {
        for (const std::uint32_t corner : triangulate_polygon(corners)) triangles.push_back(idx[corner]);
      }
    }
    base += n;
  }
  first_new[faces] = triangles.size() / 3;
  for (std::size_t f = faces; f > 0; --f) {  // faces skipped by a break keep empty ranges
    if (first_new[f - 1] > first_new[f]) first_new[f - 1] = first_new[f];
  }
  mesh.indices = std::move(triangles);
  mesh.face_sizes.clear();
  remap_face_ranges(mesh, first_new);
}

// --------------------------------------------------------------------------- normals

void normalize_normals(ir::Mesh& mesh) {
  for (ir::Vec3& n : mesh.normals) {
    n = normalize(n);
    if (n.x == 0 && n.y == 0 && n.z == 0) n = {0, 1, 0};
  }
}

namespace {

// One new vertex per (old vertex, corner normal) pair.
void apply_corner_normals(ir::Mesh& mesh, const std::vector<DVec3>& corner_normal) {
  std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> made;
  std::vector<std::uint32_t> source;  // new vertex -> old vertex
  std::vector<ir::Vec3> normals;
  std::vector<std::uint32_t> new_indices(mesh.indices.size());
  for (std::size_t c = 0; c < mesh.indices.size(); ++c) {
    const DVec3& d = corner_normal[c];
    ir::Vec3 n{static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
    if (n.x == 0 && n.y == 0 && n.z == 0) n = {0, 1, 0};
    const auto key = std::make_tuple(mesh.indices[c], std::bit_cast<std::uint32_t>(n.x),
                                     std::bit_cast<std::uint32_t>(n.y), std::bit_cast<std::uint32_t>(n.z));
    const auto [it, inserted] = made.emplace(key, static_cast<std::uint32_t>(source.size()));
    if (inserted) {
      source.push_back(mesh.indices[c]);
      normals.push_back(n);
    }
    new_indices[c] = it->second;
  }
  const auto gather = [&](auto& values) {
    if (values.empty()) return;
    std::remove_reference_t<decltype(values)> out;
    out.reserve(source.size());
    for (const std::uint32_t old : source) out.push_back(values[old]);
    values = std::move(out);
  };
  gather(mesh.positions);
  gather(mesh.texcoords);
  gather(mesh.colors);
  for (ir::VertexAttribute& attribute : mesh.attributes) gather(attribute.values);
  mesh.normals = std::move(normals);
  mesh.indices = std::move(new_indices);
}

}  // namespace

void generate_normals(ir::Mesh& mesh, float crease_degrees) {
  if (mesh.point_cloud || mesh.positions.empty() || mesh.indices.empty() || !mesh.valid()) return;
  const std::size_t faces = mesh.face_count();
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  std::vector<DVec3> face_normal(faces);
  for (std::size_t f = 0; f < faces; ++f) {
    face_normal[f] = newell(mesh.positions, mesh.indices.data() + offsets[f], offsets[f + 1] - offsets[f]);
  }
  const auto unit = [](const DVec3& d) {
    const double l = dlen(d);
    return l > 0.0 && std::isfinite(l) ? DVec3{d.x / l, d.y / l, d.z / l} : DVec3{};
  };

  std::vector<DVec3> corner_normal(mesh.indices.size());
  const bool flat = crease_degrees <= 0.0f;
  if (flat) {
    for (std::size_t f = 0; f < faces; ++f) {
      for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) corner_normal[c] = unit(face_normal[f]);
    }
  } else {
    std::size_t position_count = 0;
    const std::vector<std::uint32_t> pid = position_ids(mesh.positions, &position_count);
    // Corners grouped by position id (CSR layout).
    std::vector<std::uint32_t> start(position_count + 1, 0);
    for (const std::uint32_t index : mesh.indices) ++start[pid[index] + 1];
    for (std::size_t i = 0; i < position_count; ++i) start[i + 1] += start[i];
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    std::vector<std::uint32_t> corner_face(mesh.indices.size());
    for (std::size_t f = 0; f < faces; ++f) {
      for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) corner_face[c] = static_cast<std::uint32_t>(f);
    }
    std::vector<std::uint32_t> incident(mesh.indices.size());
    for (std::size_t c = 0; c < mesh.indices.size(); ++c) incident[fill[pid[mesh.indices[c]]]++] = static_cast<std::uint32_t>(c);

    const bool full = crease_degrees >= 180.0f;
    const double cos_crease = std::cos(static_cast<double>(std::min(crease_degrees, 180.0f)) * std::numbers::pi / 180.0);
    for (std::size_t p = 0; p < position_count; ++p) {
      const std::uint32_t begin = start[p];
      const std::uint32_t end = start[p + 1];
      if (full) {
        DVec3 sum;
        for (std::uint32_t k = begin; k < end; ++k) {
          const DVec3& fn = face_normal[corner_face[incident[k]]];
          sum = {sum.x + fn.x, sum.y + fn.y, sum.z + fn.z};
        }
        const DVec3 n = unit(sum);
        for (std::uint32_t k = begin; k < end; ++k) corner_normal[incident[k]] = n;
        continue;
      }
      if (end - begin <= 64) {
        for (std::uint32_t k = begin; k < end; ++k) {
          const DVec3 own = unit(face_normal[corner_face[incident[k]]]);
          DVec3 sum;
          for (std::uint32_t j = begin; j < end; ++j) {
            const DVec3& fn = face_normal[corner_face[incident[j]]];
            if (ddot(own, unit(fn)) >= cos_crease - 1e-9) sum = {sum.x + fn.x, sum.y + fn.y, sum.z + fn.z};
          }
          const DVec3 n = unit(sum);
          corner_normal[incident[k]] = (n.x == 0 && n.y == 0 && n.z == 0) ? own : n;
        }
        continue;
      }
      // Very busy vertex: greedy clustering keeps this linear-ish.
      std::vector<std::pair<DVec3, DVec3>> clusters;  // seed (unit), sum
      std::vector<std::size_t> cluster_of(end - begin);
      for (std::uint32_t k = begin; k < end; ++k) {
        const DVec3& fn = face_normal[corner_face[incident[k]]];
        const DVec3 own = unit(fn);
        std::size_t chosen = clusters.size();
        for (std::size_t c = 0; c < clusters.size() && c < 32; ++c) {
          if (ddot(clusters[c].first, own) >= cos_crease) {
            chosen = c;
            break;
          }
        }
        if (chosen == clusters.size()) {
          if (clusters.size() >= 32) chosen = clusters.size() - 1;
          else clusters.push_back({own, DVec3{}});
        }
        DVec3& sum = clusters[chosen].second;
        sum = {sum.x + fn.x, sum.y + fn.y, sum.z + fn.z};
        cluster_of[k - begin] = chosen;
      }
      for (std::uint32_t k = begin; k < end; ++k) {
        const auto& cluster = clusters[cluster_of[k - begin]];
        const DVec3 n = unit(cluster.second);
        corner_normal[incident[k]] = (n.x == 0 && n.y == 0 && n.z == 0) ? cluster.first : n;
      }
    }
  }

  apply_corner_normals(mesh, corner_normal);
}

void generate_normals_by_groups(ir::Mesh& mesh, std::span<const std::uint32_t> face_groups, bool bitmask) {
  if (mesh.point_cloud || mesh.positions.empty() || mesh.indices.empty() || !mesh.valid()) return;
  const std::size_t faces = mesh.face_count();
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  std::vector<DVec3> face_normal(faces);
  for (std::size_t f = 0; f < faces; ++f) {
    face_normal[f] = newell(mesh.positions, mesh.indices.data() + offsets[f], offsets[f + 1] - offsets[f]);
  }
  const auto group_of = [&](std::size_t f) { return f < face_groups.size() ? face_groups[f] : 0u; };
  const auto unit = [](const DVec3& d) {
    const double l = dlen(d);
    return l > 0.0 && std::isfinite(l) ? DVec3{d.x / l, d.y / l, d.z / l} : DVec3{};
  };
  std::size_t position_count = 0;
  const std::vector<std::uint32_t> pid = position_ids(mesh.positions, &position_count);
  std::vector<std::uint32_t> start(position_count + 1, 0);
  for (const std::uint32_t index : mesh.indices) ++start[pid[index] + 1];
  for (std::size_t i = 0; i < position_count; ++i) start[i + 1] += start[i];
  std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
  std::vector<std::uint32_t> corner_face(mesh.indices.size());
  for (std::size_t f = 0; f < faces; ++f) {
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) corner_face[c] = static_cast<std::uint32_t>(f);
  }
  std::vector<std::uint32_t> incident(mesh.indices.size());
  for (std::size_t c = 0; c < mesh.indices.size(); ++c) incident[fill[pid[mesh.indices[c]]]++] = static_cast<std::uint32_t>(c);
  std::vector<DVec3> corner_normal(mesh.indices.size());
  std::map<std::uint32_t, DVec3> sums;
  for (std::size_t p = 0; p < position_count; ++p) {
    const std::uint32_t begin = start[p];
    const std::uint32_t end = start[p + 1];
    const bool pairwise = bitmask && end - begin <= 256;
    if (!pairwise) {
      sums.clear();
      for (std::uint32_t k = begin; k < end; ++k) {
        const std::uint32_t f = corner_face[incident[k]];
        DVec3& sum = sums[group_of(f)];
        sum = {sum.x + face_normal[f].x, sum.y + face_normal[f].y, sum.z + face_normal[f].z};
      }
    }
    for (std::uint32_t k = begin; k < end; ++k) {
      const std::uint32_t f = corner_face[incident[k]];
      const std::uint32_t g = group_of(f);
      DVec3 n = face_normal[f];
      if (g != 0) {
        if (pairwise) {
          n = {};
          for (std::uint32_t j = begin; j < end; ++j) {
            const std::uint32_t other = corner_face[incident[j]];
            if ((group_of(other) & g) == 0) continue;
            n = {n.x + face_normal[other].x, n.y + face_normal[other].y, n.z + face_normal[other].z};
          }
        } else {
          n = sums[g];
        }
      }
      const DVec3 u = unit(n);
      corner_normal[incident[k]] = (u.x == 0 && u.y == 0 && u.z == 0) ? unit(face_normal[f]) : u;
    }
  }
  apply_corner_normals(mesh, corner_normal);
}

// --------------------------------------------------------------------------- cleanup

void remap_vertices(ir::Mesh& mesh, std::span<const std::uint32_t> old_to_new, std::size_t new_count) {
  permute_vertex_array(mesh.positions, old_to_new, new_count, ir::Vec3{});
  permute_vertex_array(mesh.normals, old_to_new, new_count, ir::Vec3{0, 1, 0});
  permute_vertex_array(mesh.texcoords, old_to_new, new_count, ir::Vec2{});
  permute_vertex_array(mesh.colors, old_to_new, new_count, ir::Color{});
  for (ir::VertexAttribute& attribute : mesh.attributes) {
    permute_vertex_array(attribute.values, old_to_new, new_count, 0.0f);
  }
  for (std::uint32_t& index : mesh.indices) {
    if (index < old_to_new.size()) index = old_to_new[index];
  }
}

std::size_t weld(ir::Mesh& mesh, float tolerance, bool positions_only, float attribute_tolerance) {
  const std::size_t n = mesh.positions.size();
  if (n < 2) return 0;
  const bool has_n = !mesh.normals.empty() && !positions_only;
  const bool has_t = !mesh.texcoords.empty() && !positions_only;
  const bool has_c = !mesh.colors.empty() && !positions_only;
  const bool has_a = !mesh.attributes.empty() && !positions_only;
  const auto close = [attribute_tolerance](float a, float b) {
    return a == b || std::abs(a - b) <= attribute_tolerance;
  };
  const auto same_attributes = [&](std::uint32_t a, std::uint32_t b) {
    if (has_n) {
      const ir::Vec3& x = mesh.normals[a];
      const ir::Vec3& y = mesh.normals[b];
      if (!close(x.x, y.x) || !close(x.y, y.y) || !close(x.z, y.z)) return false;
    }
    if (has_t) {
      const ir::Vec2& x = mesh.texcoords[a];
      const ir::Vec2& y = mesh.texcoords[b];
      if (!close(x.x, y.x) || !close(x.y, y.y)) return false;
    }
    if (has_c) {
      const ir::Color& x = mesh.colors[a];
      const ir::Color& y = mesh.colors[b];
      if (!close(x.r, y.r) || !close(x.g, y.g) || !close(x.b, y.b) || !close(x.a, y.a)) return false;
    }
    if (has_a) {
      for (const ir::VertexAttribute& attribute : mesh.attributes) {
        if (!close(attribute.values[a], attribute.values[b])) return false;
      }
    }
    return true;
  };

  std::vector<std::uint32_t> old_to_new(n, dropped);
  std::size_t kept = 0;
  const double cell = tolerance > 0.0f ? static_cast<double>(tolerance) : 0.0;
  const auto cell_of = [cell](float v) -> std::int64_t {
    if (!std::isfinite(v)) return 0;
    const double c = std::floor(static_cast<double>(v) / cell);
    return static_cast<std::int64_t>(std::clamp(c, -1e15, 1e15));
  };
  const auto hash3 = [](std::int64_t x, std::int64_t y, std::int64_t z) {
    std::uint64_t h = static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return h;
  };
  // representative new index -> old index of its first vertex
  std::vector<std::uint32_t> representative;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
  buckets.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const ir::Vec3& p = mesh.positions[i];
    const auto v = static_cast<std::uint32_t>(i);
    std::uint32_t found = dropped;
    if (cell == 0.0) {
      const float x = p.x == 0.0f ? 0.0f : p.x;  // fold -0
      const float y = p.y == 0.0f ? 0.0f : p.y;
      const float z = p.z == 0.0f ? 0.0f : p.z;
      const std::uint64_t key = hash3(std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y),
                                      std::bit_cast<std::uint32_t>(z));
      auto& bucket = buckets[key];
      for (const std::uint32_t candidate : bucket) {
        const ir::Vec3& q = mesh.positions[representative[candidate]];
        if (q.x == p.x && q.y == p.y && q.z == p.z && same_attributes(representative[candidate], v)) {
          found = candidate;
          break;
        }
      }
      if (found == dropped) {
        found = static_cast<std::uint32_t>(kept++);
        representative.push_back(v);
        bucket.push_back(found);
      }
    } else {
      const std::int64_t cx = cell_of(p.x), cy = cell_of(p.y), cz = cell_of(p.z);
      const double tol2 = static_cast<double>(tolerance) * tolerance;
      for (std::int64_t dx = -1; dx <= 1 && found == dropped; ++dx) {
        for (std::int64_t dy = -1; dy <= 1 && found == dropped; ++dy) {
          for (std::int64_t dz = -1; dz <= 1 && found == dropped; ++dz) {
            const auto it = buckets.find(hash3(cx + dx, cy + dy, cz + dz));
            if (it == buckets.end()) continue;
            for (const std::uint32_t candidate : it->second) {
              const ir::Vec3& q = mesh.positions[representative[candidate]];
              const double ex = static_cast<double>(q.x) - p.x;
              const double ey = static_cast<double>(q.y) - p.y;
              const double ez = static_cast<double>(q.z) - p.z;
              if (ex * ex + ey * ey + ez * ez <= tol2 && same_attributes(representative[candidate], v)) {
                found = candidate;
                break;
              }
            }
          }
        }
      }
      if (found == dropped) {
        found = static_cast<std::uint32_t>(kept++);
        representative.push_back(v);
        buckets[hash3(cx, cy, cz)].push_back(found);
      }
    }
    old_to_new[i] = found;
  }
  if (kept == n) return 0;
  remap_vertices(mesh, old_to_new, kept);
  return n - kept;
}

std::size_t remove_unused_vertices(ir::Mesh& mesh) {
  if (mesh.point_cloud) return 0;
  const std::size_t n = mesh.positions.size();
  std::vector<bool> used(n, false);
  for (const std::uint32_t index : mesh.indices) {
    if (index < n) used[index] = true;
  }
  std::vector<std::uint32_t> old_to_new(n, dropped);
  std::size_t kept = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (used[i]) old_to_new[i] = static_cast<std::uint32_t>(kept++);
  }
  if (kept == n) return 0;
  remap_vertices(mesh, old_to_new, kept);
  return n - kept;
}

std::vector<std::size_t> face_offsets(const ir::Mesh& mesh) {
  const std::size_t faces = mesh.face_count();
  std::vector<std::size_t> offsets(faces + 1, 0);
  for (std::size_t f = 0; f < faces; ++f) offsets[f + 1] = offsets[f] + face_size_at(mesh, f);
  return offsets;
}

void remap_face_ranges(ir::Mesh& mesh, std::span<const std::size_t> first_new) {
  if (first_new.empty()) return;
  const std::size_t old_faces = first_new.size() - 1;
  const auto map = [&](std::size_t& first, std::size_t& count) {
    const std::size_t begin = std::min(first, old_faces);
    const std::size_t end = std::min(first + count, old_faces);
    first = first_new[begin];
    count = end >= begin ? first_new[end] - first_new[begin] : 0;
  };
  for (ir::MeshGroup& group : mesh.groups) map(group.first_face, group.face_count);
  for (ir::MeshObject& object : mesh.objects) map(object.first_face, object.face_count);
  std::erase_if(mesh.groups, [](const ir::MeshGroup& g) { return g.face_count == 0; });
}

std::size_t remove_degenerate_faces(ir::Mesh& mesh, bool remove_duplicates, float area_epsilon) {
  const std::size_t faces = mesh.face_count();
  if (faces == 0 || !mesh.valid()) return 0;
  const bool polygons = !mesh.face_sizes.empty();
  std::vector<std::uint32_t> indices;
  std::vector<std::uint32_t> sizes;
  indices.reserve(mesh.indices.size());
  std::vector<std::size_t> first_new(faces + 1, 0);
  std::unordered_set<std::string> seen;
  std::vector<std::uint32_t> face;
  std::size_t base = 0;
  std::size_t kept = 0;
  for (std::size_t f = 0; f < faces; ++f) {
    first_new[f] = kept;
    const std::uint32_t n = face_size_at(mesh, f);
    face.assign(mesh.indices.begin() + static_cast<std::ptrdiff_t>(base),
                mesh.indices.begin() + static_cast<std::ptrdiff_t>(base + n));
    base += n;
    // Drop consecutive repeats (a polygon may survive them).
    std::vector<std::uint32_t> cleaned;
    for (std::size_t k = 0; k < face.size(); ++k) {
      if (cleaned.empty() || cleaned.back() != face[k]) cleaned.push_back(face[k]);
    }
    while (cleaned.size() > 1 && cleaned.back() == cleaned.front()) cleaned.pop_back();
    if (cleaned.size() < 3) continue;
    if (!polygons && cleaned.size() != 3) continue;
    {
      std::vector<std::uint32_t> sorted = cleaned;
      std::sort(sorted.begin(), sorted.end());
      if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) continue;  // pinched
    }
    const double area = 0.5 * dlen(newell(mesh.positions, cleaned.data(), cleaned.size()));
    if (!(area > static_cast<double>(area_epsilon))) continue;
    if (remove_duplicates) {
      const auto smallest = std::min_element(cleaned.begin(), cleaned.end());
      std::vector<std::uint32_t> canonical(smallest, cleaned.end());
      canonical.insert(canonical.end(), cleaned.begin(), smallest);
      std::string key(reinterpret_cast<const char*>(canonical.data()), canonical.size() * sizeof(std::uint32_t));
      if (!seen.insert(std::move(key)).second) continue;
    }
    indices.insert(indices.end(), cleaned.begin(), cleaned.end());
    if (polygons) sizes.push_back(static_cast<std::uint32_t>(cleaned.size()));
    ++kept;
  }
  first_new[faces] = kept;
  const std::size_t removed = faces - kept;
  if (removed == 0 && indices.size() == mesh.indices.size()) return 0;
  mesh.indices = std::move(indices);
  mesh.face_sizes = std::move(sizes);
  remap_face_ranges(mesh, first_new);
  return removed;
}

std::size_t fix_winding(ir::Mesh& mesh) {
  const std::size_t faces = mesh.face_count();
  if (faces == 0 || !mesh.valid()) return 0;
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  const std::vector<std::uint32_t> pid = position_ids(mesh.positions);
  // Undirected edge -> incident (face, forward?) records.
  struct EdgeUse {
    std::uint32_t face;
    bool forward;  // traversed from the smaller to the larger position id
  };
  std::unordered_map<std::uint64_t, std::vector<EdgeUse>> edges;
  edges.reserve(mesh.indices.size());
  for (std::size_t f = 0; f < faces; ++f) {
    const std::size_t n = offsets[f + 1] - offsets[f];
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint32_t a = pid[mesh.indices[offsets[f] + k]];
      const std::uint32_t b = pid[mesh.indices[offsets[f] + (k + 1) % n]];
      if (a == b) continue;
      const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
      edges[key].push_back({static_cast<std::uint32_t>(f), a < b});
    }
  }
  std::vector<int> flip(faces, -1);  // -1 unvisited, 0 keep, 1 flip
  std::vector<std::uint32_t> component(faces, 0);
  std::vector<std::uint32_t> queue;
  std::uint32_t components = 0;
  for (std::size_t seed = 0; seed < faces; ++seed) {
    if (flip[seed] != -1) continue;
    flip[seed] = 0;
    component[seed] = components;
    queue.assign(1, static_cast<std::uint32_t>(seed));
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const std::uint32_t f = queue[head];
      const std::size_t n = offsets[f + 1] - offsets[f];
      for (std::size_t k = 0; k < n; ++k) {
        const std::uint32_t a = pid[mesh.indices[offsets[f] + k]];
        const std::uint32_t b = pid[mesh.indices[offsets[f] + (k + 1) % n]];
        if (a == b) continue;
        const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
        const auto& uses = edges[key];
        if (uses.size() != 2) continue;  // border or non-manifold: no propagation
        const bool mine = (a < b) != (flip[f] == 1);
        for (const EdgeUse& use : uses) {
          if (use.face == f || flip[use.face] != -1) continue;
          // Consistent neighbours traverse the shared edge in opposite directions.
          flip[use.face] = use.forward == mine ? 1 : 0;
          component[use.face] = components;
          queue.push_back(use.face);
        }
      }
    }
    ++components;
  }
  // Per component: minimise changes, then orient closed parts outward.
  std::vector<std::size_t> flipped(components, 0), total(components, 0);
  for (std::size_t f = 0; f < faces; ++f) {
    total[component[f]]++;
    if (flip[f] == 1) flipped[component[f]]++;
  }
  std::vector<bool> invert(components, false);
  for (std::uint32_t c = 0; c < components; ++c) invert[c] = flipped[c] * 2 > total[c];
  std::vector<double> volume(components, 0.0);
  std::vector<bool> closed(components, true);
  for (const auto& [key, uses] : edges) {
    if (uses.size() != 2) {
      for (const EdgeUse& use : uses) closed[component[use.face]] = false;
    }
  }
  for (std::size_t f = 0; f < faces; ++f) {
    const bool reversed = (flip[f] == 1) != invert[component[f]];
    const std::size_t n = offsets[f + 1] - offsets[f];
    const DVec3 a = to_d(mesh.positions[mesh.indices[offsets[f]]]);
    for (std::size_t k = 1; k + 1 < n; ++k) {
      const DVec3 b = to_d(mesh.positions[mesh.indices[offsets[f] + k]]);
      const DVec3 c = to_d(mesh.positions[mesh.indices[offsets[f] + k + 1]]);
      const double v = ddot(a, dcross(b, c));
      volume[component[f]] += reversed ? -v : v;
    }
  }
  for (std::uint32_t c = 0; c < components; ++c) {
    if (closed[c] && volume[c] < 0.0) invert[c] = !invert[c];
  }
  std::size_t changed = 0;
  for (std::size_t f = 0; f < faces; ++f) {
    if ((flip[f] == 1) == invert[component[f]]) continue;
    std::reverse(mesh.indices.begin() + static_cast<std::ptrdiff_t>(offsets[f] + 1),
                 mesh.indices.begin() + static_cast<std::ptrdiff_t>(offsets[f + 1]));
    ++changed;
  }
  return changed;
}

void quantize_positions(ir::Mesh& mesh, int bits) {
  bits = std::clamp(bits, 4, 24);
  const Bounds box = bounds(mesh);
  if (box.empty) return;
  const double steps = std::ldexp(1.0, bits) - 1.0;
  const ir::Vec3 size = box.size();
  const auto snap = [steps](float v, float lo, float extent) {
    if (!(extent > 0.0f) || !std::isfinite(v)) return v;
    const double t = std::round((static_cast<double>(v) - lo) / extent * steps);
    return static_cast<float>(lo + t / steps * extent);
  };
  for (ir::Vec3& p : mesh.positions) {
    p = {snap(p.x, box.min.x, size.x), snap(p.y, box.min.y, size.y), snap(p.z, box.min.z, size.z)};
  }
}

// --------------------------------------------------------------------------- extract / append / split

ir::Mesh extract_faces(const ir::Mesh& mesh, std::size_t first_face, std::size_t count) {
  ir::Mesh out;
  out.name = mesh.name;
  out.materials = mesh.materials;
  out.textures = mesh.textures;
  out.attributes.reserve(mesh.attributes.size());
  out.unit_meters = mesh.unit_meters;
  out.source_up_axis = mesh.source_up_axis;
  out.metadata = mesh.metadata;
  const std::size_t faces = mesh.face_count();
  first_face = std::min(first_face, faces);
  count = std::min(count, faces - first_face);
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  std::vector<std::uint32_t> source;
  const bool polygons = !mesh.face_sizes.empty();
  for (std::size_t f = first_face; f < first_face + count; ++f) {
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
      const std::uint32_t old = mesh.indices[c];
      const auto [it, inserted] = remap.emplace(old, static_cast<std::uint32_t>(source.size()));
      if (inserted) source.push_back(old);
      out.indices.push_back(it->second);
    }
    if (polygons) out.face_sizes.push_back(mesh.face_sizes[f]);
  }
  const auto gather = [&](const auto& from, auto& to) {
    if (from.empty()) return;
    to.reserve(source.size());
    for (const std::uint32_t old : source) to.push_back(from[old]);
  };
  gather(mesh.positions, out.positions);
  gather(mesh.normals, out.normals);
  gather(mesh.texcoords, out.texcoords);
  gather(mesh.colors, out.colors);
  for (const ir::VertexAttribute& attribute : mesh.attributes) {
    ir::VertexAttribute copy;
    copy.name = attribute.name;
    gather(attribute.values, copy.values);
    out.attributes.push_back(std::move(copy));
  }
  for (const ir::MeshGroup& group : mesh.groups) {
    const std::size_t begin = std::max(group.first_face, first_face);
    const std::size_t end = std::min(group.first_face + group.face_count, first_face + count);
    if (end <= begin) continue;
    ir::MeshGroup clipped = group;
    clipped.first_face = begin - first_face;
    clipped.face_count = end - begin;
    out.groups.push_back(std::move(clipped));
  }
  if (mesh.point_cloud && count == 0 && first_face == 0) {
    out.positions = mesh.positions;
    out.normals = mesh.normals;
    out.texcoords = mesh.texcoords;
    out.colors = mesh.colors;
    out.attributes = mesh.attributes;
    out.point_cloud = true;
  }
  return out;
}

namespace {

// Per-vertex smooth normals without splitting vertices (used to fill in a
// missing normal array when meshes are combined).
void fill_vertex_normals(ir::Mesh& mesh) {
  std::vector<DVec3> sums(mesh.positions.size());
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  for (std::size_t f = 0; f + 1 < offsets.size(); ++f) {
    const DVec3 n = newell(mesh.positions, mesh.indices.data() + offsets[f], offsets[f + 1] - offsets[f]);
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
      DVec3& s = sums[mesh.indices[c]];
      s = {s.x + n.x, s.y + n.y, s.z + n.z};
    }
  }
  mesh.normals.resize(mesh.positions.size());
  for (std::size_t i = 0; i < sums.size(); ++i) {
    const ir::Vec3 n = normalize({static_cast<float>(sums[i].x), static_cast<float>(sums[i].y),
                                  static_cast<float>(sums[i].z)});
    mesh.normals[i] = (n.x == 0 && n.y == 0 && n.z == 0) ? ir::Vec3{0, 1, 0} : n;
  }
}

}  // namespace

void append(ir::Mesh& into, const ir::Mesh& part_in) {
  if (part_in.positions.empty() && part_in.indices.empty()) return;
  ir::Mesh part = part_in;
  if (into.positions.empty() && into.indices.empty() && into.groups.empty() && into.objects.empty()) {
    std::string name = into.name.empty() ? part.name : into.name;
    auto metadata = into.metadata;
    const double unit = into.unit_meters;
    const std::string axis = into.source_up_axis;
    into = std::move(part);
    into.name = std::move(name);
    if (!metadata.empty()) into.metadata = std::move(metadata);
    if (unit > 0.0) into.unit_meters = unit;
    if (!axis.empty()) into.source_up_axis = axis;
    return;
  }
  // Per-vertex arrays: fill the side that lacks one.
  if (into.normals.empty() != part.normals.empty()) {
    if (into.normals.empty() && !into.positions.empty()) fill_vertex_normals(into);
    if (part.normals.empty() && !part.positions.empty()) fill_vertex_normals(part);
  }
  if (into.texcoords.empty() != part.texcoords.empty()) {
    if (into.texcoords.empty()) into.texcoords.assign(into.positions.size(), ir::Vec2{});
    if (part.texcoords.empty()) part.texcoords.assign(part.positions.size(), ir::Vec2{});
  }
  if (into.colors.empty() != part.colors.empty()) {
    if (into.colors.empty()) into.colors.assign(into.positions.size(), ir::Color{});
    if (part.colors.empty()) part.colors.assign(part.positions.size(), ir::Color{});
  }
  // Named channels: union by name, missing values are 0.
  for (const ir::VertexAttribute& attribute : part.attributes) {
    const bool known = std::any_of(into.attributes.begin(), into.attributes.end(),
                                   [&](const ir::VertexAttribute& a) { return a.name == attribute.name; });
    if (!known) into.attributes.push_back({attribute.name, std::vector<float>(into.positions.size(), 0.0f)});
  }
  for (ir::VertexAttribute& attribute : into.attributes) {
    const auto found = std::find_if(part.attributes.begin(), part.attributes.end(),
                                    [&](const ir::VertexAttribute& a) { return a.name == attribute.name; });
    if (found != part.attributes.end() && found->values.size() == part.positions.size()) {
      attribute.values.insert(attribute.values.end(), found->values.begin(), found->values.end());
    } else {
      attribute.values.resize(attribute.values.size() + part.positions.size(), 0.0f);
    }
  }
  // Textures and materials.
  const int texture_offset = static_cast<int>(into.textures.size());
  for (const ir::Texture& texture : part.textures) into.textures.push_back(texture);
  std::map<std::string, std::string> renamed;
  for (ir::Material material : part.materials) {
    if (material.base_color_texture >= 0) material.base_color_texture += texture_offset;
    if (material.normal_texture >= 0) material.normal_texture += texture_offset;
    const ir::Material* existing = into.find_material(material.name);
    if (existing != nullptr) {
      ir::Material compare = material;
      compare.base_color_texture = existing->base_color_texture == -1 ? compare.base_color_texture
                                                                       : compare.base_color_texture;
      if (*existing == material) continue;
      std::string fresh;
      for (int k = 2;; ++k) {
        fresh = material.name + "_" + std::to_string(k);
        if (into.find_material(fresh) == nullptr) break;
      }
      renamed[material.name] = fresh;
      material.name = fresh;
    }
    into.materials.push_back(std::move(material));
  }
  // Faces.
  const std::size_t face_offset = into.face_count();
  const bool into_polygons = !into.face_sizes.empty();
  const bool part_polygons = !part.face_sizes.empty();
  if (into_polygons || part_polygons) {
    if (!into_polygons) into.face_sizes.assign(into.indices.size() / 3, 3u);
    if (part_polygons) {
      into.face_sizes.insert(into.face_sizes.end(), part.face_sizes.begin(), part.face_sizes.end());
    } else {
      into.face_sizes.insert(into.face_sizes.end(), part.indices.size() / 3, 3u);
    }
  }
  const auto vertex_offset = static_cast<std::uint32_t>(into.positions.size());
  for (const std::uint32_t index : part.indices) into.indices.push_back(index + vertex_offset);
  into.positions.insert(into.positions.end(), part.positions.begin(), part.positions.end());
  into.normals.insert(into.normals.end(), part.normals.begin(), part.normals.end());
  into.texcoords.insert(into.texcoords.end(), part.texcoords.begin(), part.texcoords.end());
  into.colors.insert(into.colors.end(), part.colors.begin(), part.colors.end());
  for (ir::MeshGroup group : part.groups) {
    group.first_face += face_offset;
    if (const auto it = renamed.find(group.material); it != renamed.end()) group.material = it->second;
    into.groups.push_back(std::move(group));
  }
  for (ir::MeshObject object : part.objects) {
    object.first_face += face_offset;
    into.objects.push_back(std::move(object));
  }
  into.point_cloud = into.point_cloud && part.point_cloud;
  for (const auto& [key, value] : part.metadata) into.metadata.emplace(key, value);
}

std::vector<ObjectPart> split_objects(const ir::Mesh& mesh, bool restore_local) {
  std::vector<ObjectPart> parts;
  const std::size_t faces = mesh.face_count();
  if (mesh.objects.empty() || mesh.point_cloud) {
    ObjectPart part;
    part.name = mesh.name;
    part.mesh = mesh;
    part.mesh.objects.clear();
    parts.push_back(std::move(part));
    return parts;
  }
  std::vector<bool> covered(faces, false);
  for (const ir::MeshObject& object : mesh.objects) {
    const std::size_t first = std::min(object.first_face, faces);
    const std::size_t count = std::min(object.face_count, faces - first);
    ObjectPart part;
    part.name = object.name;
    part.mesh = extract_faces(mesh, first, count);
    part.mesh.name = object.name;
    for (std::size_t f = first; f < first + count; ++f) covered[f] = true;
    if (restore_local && !object.transform.is_identity()) {
      bool ok = false;
      const ir::Mat4 inv = inverse(object.transform, &ok);
      if (ok) {
        transform(part.mesh, inv);
        part.transform = object.transform;
      }
    }
    parts.push_back(std::move(part));
  }
  // Faces outside every object, as runs.
  std::size_t f = 0;
  while (f < faces) {
    if (covered[f]) {
      ++f;
      continue;
    }
    std::size_t end = f;
    while (end < faces && !covered[end]) ++end;
    ObjectPart rest;
    rest.mesh = extract_faces(mesh, f, end - f);
    parts.push_back(std::move(rest));
    f = end;
  }
  return parts;
}

}  // namespace cvt::mesh
