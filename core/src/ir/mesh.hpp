// Convertible core - Mesh IR: polygonal 3D geometry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
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

/// A 4x4 affine transform, column-major (the glTF/OpenGL layout):
/// `m[column * 4 + row]`, translation in `m[12]`, `m[13]`, `m[14]`.
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  friend bool operator==(const Mat4&, const Mat4&) = default;

  [[nodiscard]] static constexpr Mat4 identity() noexcept { return {}; }
  [[nodiscard]] constexpr bool is_identity() const noexcept { return *this == Mat4{}; }
  [[nodiscard]] constexpr float at(int row, int column) const noexcept { return m[column * 4 + row]; }
  [[nodiscard]] constexpr Vec3 transform_point(const Vec3& p) const noexcept {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
  }
  /// Direction transform (no translation); normals need the inverse transpose.
  [[nodiscard]] constexpr Vec3 transform_vector(const Vec3& v) const noexcept {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z, m[1] * v.x + m[5] * v.y + m[9] * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z};
  }
  friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
      for (int row = 0; row < 4; ++row) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[c * 4 + k];
        r.m[c * 4 + row] = sum;
      }
    }
    return r;
  }
};

/// An image used by a material. `data` holds the encoded file (PNG, JPEG,
/// ...) when it was embedded in, or supplied together with, the source;
/// otherwise only `uri` (the reference as written in the source) is known.
struct Texture {
  std::string name;
  /// Relative reference from the source file ("textures/wood.png"); may be empty.
  std::string uri;
  /// Media type of `data` ("image/png"), empty when unknown.
  std::string mime;
  /// Encoded image bytes; empty when the image was not available.
  std::vector<std::byte> data;
};

/// A metallic-roughness PBR material (the glTF model). Formats with
/// Phong-style materials map diffuse to `base_color`, shininess to
/// `roughness`, and dissolve/transparency to `base_color.a`.
struct Material {
  /// Unique within one mesh; groups refer to materials by this name.
  std::string name;
  /// Linear RGBA multiplier (alpha = opacity).
  Color base_color;
  float metallic = 0.0f;
  float roughness = 1.0f;
  /// Linear emitted colour (black = none).
  Vec3 emissive{0, 0, 0};
  /// Index into `Mesh::textures`, or -1.
  int base_color_texture = -1;
  /// Index into `Mesh::textures` of a tangent-space normal map, or -1.
  int normal_texture = -1;
  bool double_sided = false;
  friend bool operator==(const Material&, const Material&) = default;
};

/// One object (node) of a scene. Geometry is ALWAYS stored flattened in
/// scene space: `transform` has already been applied to the positions and
/// normals of the object's faces. It records the object's original
/// local-to-scene matrix so that writers of node-based formats (glTF, 3MF,
/// COLLADA, FBX, ...) can restore it by applying its inverse.
struct MeshObject {
  std::string name;
  /// Faces [first_face, first_face + face_count) belong to this object.
  std::size_t first_face = 0;
  std::size_t face_count = 0;
  Mat4 transform;
};

/// A named per-vertex scalar channel that has no dedicated field
/// (PLY/PCD/LAS properties such as intensity or classification).
struct VertexAttribute {
  std::string name;
  /// Same length as `Mesh::positions`.
  std::vector<float> values;
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

  // ---- additions (all optional; defaults describe a plain mesh) ----------

  /// Material definitions; `MeshGroup::material` names one of them.
  std::vector<Material> materials;
  /// Images referenced by `Material::*_texture`.
  std::vector<Texture> textures;
  /// Scene objects (see MeshObject); empty for a single unnamed object.
  /// Objects do not overlap.
  std::vector<MeshObject> objects;
  /// Extra per-vertex scalar channels.
  std::vector<VertexAttribute> attributes;
  /// True for a point cloud: the positions are the data and there are no
  /// faces (`indices` and `face_sizes` are empty).
  bool point_cloud = false;
  /// Length of one coordinate unit in metres (0.001 for millimetres);
  /// 0 = unknown / unitless.
  double unit_meters = 0.0;
  /// Up axis the source declared ("Z", "Y", "X", "-Z"...); the coordinates
  /// above have already been converted to +Y up. Empty when the source does
  /// not declare one (STL, PLY, ...): the coordinates are then as stored.
  std::string source_up_axis;
  /// Descriptive fields of the source (title, author, tool, comment,
  /// copyright, ...). Writers drop them when `strip_metadata` is on.
  std::map<std::string, std::string> metadata;

  /// The material a group refers to, or nullptr.
  [[nodiscard]] const Material* find_material(std::string_view material_name) const noexcept {
    if (material_name.empty()) return nullptr;
    for (const Material& material : materials) {
      if (material.name == material_name) return &material;
    }
    return nullptr;
  }

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
    for (const VertexAttribute& attribute : attributes) {
      if (attribute.values.size() != n) return false;
    }
    for (const Material& material : materials) {
      if (material.base_color_texture >= static_cast<long long>(textures.size()) ||
          material.normal_texture >= static_cast<long long>(textures.size())) {
        return false;
      }
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
