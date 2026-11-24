// Convertible core - Mesh IR: polygonal 3D geometry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cvt::ir {

struct Vec2 {
  float x = 0, y = 0;
  friend bool operator==(const Vec2&, const Vec2&) = default;
};
struct Vec3 {
  float x = 0, y = 0, z = 0;
  friend bool operator==(const Vec3&, const Vec3&) = default;
};
/// Linear RGBA, each component 0..1.
struct Color {
  float r = 1, g = 1, b = 1, a = 1;
  friend bool operator==(const Color&, const Color&) = default;
};

/// A named run of consecutive faces (OBJ group/object, material range, ...).
struct MeshGroup {
  std::string name;
  /// Material name when the source has one; purely informational.
  std::string material;
  /// Index of the first face and number of faces in the group.
  std::size_t first_face = 0;
  std::size_t face_count = 0;
};

/// An indexed polygon mesh with a single index space: one index selects the
/// position, normal, texture coordinate and colour of a vertex. Readers of
/// formats with per-attribute indices (OBJ) must duplicate vertices.
///
/// Coordinates are right-handed, +Y up, unitless (readers convert, e.g. from
/// +Z up, and say so in a warning when the source declares its axes).
struct Mesh {
  /// Object name; may be empty.
  std::string name;
  /// Vertex positions.
  std::vector<Vec3> positions;
  /// Per-vertex unit normals; either empty or the same length as `positions`.
  std::vector<Vec3> normals;
  /// Per-vertex texture coordinates (origin bottom-left); empty or same
  /// length as `positions`.
  std::vector<Vec2> texcoords;
  /// Per-vertex colours; empty or same length as `positions`.
  std::vector<Color> colors;
  /// Vertex indices of all faces, concatenated. Counter-clockwise winding
  /// when seen from the front.
  std::vector<std::uint32_t> indices;
  /// Number of vertices of each face (>= 3). EMPTY means the mesh is fully
  /// triangulated: every three consecutive `indices` form one face.
  std::vector<std::uint32_t> face_sizes;
  /// Optional partition of the faces; groups do not overlap.
  std::vector<MeshGroup> groups;

  [[nodiscard]] bool is_triangulated() const noexcept {
    if (face_sizes.empty()) return true;
    for (const std::uint32_t size : face_sizes) {
      if (size != 3) return false;
    }
    return true;
  }
  [[nodiscard]] std::size_t face_count() const noexcept {
    return face_sizes.empty() ? indices.size() / 3 : face_sizes.size();
  }
  /// True when attribute arrays have consistent lengths, face sizes add up
  /// to the index count and every index is in range.
  [[nodiscard]] bool valid() const noexcept {
    const std::size_t n = positions.size();
    if ((!normals.empty() && normals.size() != n) || (!texcoords.empty() && texcoords.size() != n) ||
        (!colors.empty() && colors.size() != n)) {
      return false;
    }
    if (face_sizes.empty()) {
      if (indices.size() % 3 != 0) return false;
    } else {
      std::size_t total = 0;
      for (const std::uint32_t size : face_sizes) {
        if (size < 3) return false;
        total += size;
      }
      if (total != indices.size()) return false;
    }
    for (const std::uint32_t index : indices) {
      if (index >= n) return false;
    }
    return true;
  }
  /// Triangle index list: `indices` itself when already triangulated,
  /// otherwise each polygon split as a fan around its first vertex (exact
  /// for convex polygons). Group face ranges are NOT remapped.
  [[nodiscard]] std::vector<std::uint32_t> triangle_indices() const {
    if (face_sizes.empty()) return indices;
    std::vector<std::uint32_t> triangles;
    triangles.reserve(indices.size());
    std::size_t base = 0;
    for (const std::uint32_t size : face_sizes) {
      if (base + size > indices.size()) break;
      for (std::uint32_t k = 1; k + 1 < size; ++k) {
        triangles.push_back(indices[base]);
        triangles.push_back(indices[base + k]);
        triangles.push_back(indices[base + k + 1]);
      }
      base += size;
    }
    return triangles;
  }
};

}  // namespace cvt::ir
