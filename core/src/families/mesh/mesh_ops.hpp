// Convertible core - mesh family: geometry toolbox for ir::Mesh.
//
// Every reader and writer of the family uses these operations, and they are
// unit-tested on their own. All functions are deterministic, never throw
// (std::bad_alloc aside) and keep a mesh `valid()` when it was valid on
// entry. Operations that change the faces keep `groups` and `objects` face
// ranges in step; operations that change the vertices keep every per-vertex
// array (normals, texcoords, colors, attributes) in step.
//
//   ir::Mesh mesh = ...;
//   mesh::triangulate(mesh);                    // ear clipping, concave polygons welcome
//   mesh::generate_normals(mesh, 30.0f);        // smooth, with a 30 degree crease angle
//   mesh::simplify(mesh, {.target_ratio = 0.25f});
//   const mesh::Bounds box = mesh::bounds(mesh);
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ir/mesh.hpp"

namespace cvt::mesh {

// --------------------------------------------------------------------------- measures

/// Axis-aligned bounding box of all positions.
struct Bounds {
  ir::Vec3 min{0, 0, 0};
  ir::Vec3 max{0, 0, 0};
  /// True when there were no (finite) positions.
  bool empty = true;

  [[nodiscard]] ir::Vec3 center() const noexcept {
    return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
  }
  [[nodiscard]] ir::Vec3 size() const noexcept { return {max.x - min.x, max.y - min.y, max.z - min.z}; }
  /// Length of the diagonal; 0 for an empty box.
  [[nodiscard]] float diagonal() const noexcept;
  /// Longest side; 0 for an empty box.
  [[nodiscard]] float longest_side() const noexcept;
};

/// Box of every finite entry of `positions` (referenced by faces or not).
[[nodiscard]] Bounds bounds(const ir::Mesh& mesh);
[[nodiscard]] Bounds bounds(std::span<const ir::Vec3> points);

/// Number of triangles after fan-splitting polygons (as Mesh::triangle_indices).
[[nodiscard]] std::size_t triangle_count(const ir::Mesh& mesh) noexcept;
/// Total area of all faces (polygons measured with their Newell area).
[[nodiscard]] double surface_area(const ir::Mesh& mesh);
/// Enclosed volume by the divergence theorem: exact for closed,
/// consistently wound meshes; positive when the winding is outward.
[[nodiscard]] double signed_volume(const ir::Mesh& mesh);

// --------------------------------------------------------------------------- vector helpers

[[nodiscard]] ir::Vec3 sub(const ir::Vec3& a, const ir::Vec3& b) noexcept;
[[nodiscard]] ir::Vec3 add(const ir::Vec3& a, const ir::Vec3& b) noexcept;
[[nodiscard]] ir::Vec3 mul(const ir::Vec3& a, float s) noexcept;
[[nodiscard]] float dot(const ir::Vec3& a, const ir::Vec3& b) noexcept;
[[nodiscard]] ir::Vec3 cross(const ir::Vec3& a, const ir::Vec3& b) noexcept;
[[nodiscard]] float length(const ir::Vec3& a) noexcept;
/// Unit vector, or (0, 0, 0) for a zero / non-finite input.
[[nodiscard]] ir::Vec3 normalize(const ir::Vec3& a) noexcept;
/// Newell normal of a polygon (not normalised; its length is twice the area).
[[nodiscard]] ir::Vec3 polygon_normal(std::span<const ir::Vec3> points) noexcept;

// --------------------------------------------------------------------------- matrices

/// Inverse of an affine/projective 4x4 matrix; identity when singular
/// (`ok`, when given, receives false then).
[[nodiscard]] ir::Mat4 inverse(const ir::Mat4& matrix, bool* ok = nullptr) noexcept;
/// Determinant of the upper-left 3x3 block (negative = mirroring).
[[nodiscard]] float determinant3(const ir::Mat4& matrix) noexcept;
[[nodiscard]] ir::Mat4 translation(float x, float y, float z) noexcept;
[[nodiscard]] ir::Mat4 scaling(float x, float y, float z) noexcept;
/// Rotation by `degrees` about the unit axis (x, y, z), right-handed.
[[nodiscard]] ir::Mat4 rotation(float degrees, float x, float y, float z) noexcept;
/// Rotation from a unit quaternion (x, y, z, w), the glTF convention.
[[nodiscard]] ir::Mat4 rotation_quaternion(float x, float y, float z, float w) noexcept;
/// Matrix from row-major values (how most text formats write matrices).
[[nodiscard]] ir::Mat4 from_row_major(std::span<const float, 16> values) noexcept;

// --------------------------------------------------------------------------- transforms

/// Apply a matrix to positions and (with the inverse transpose) normals.
/// Mirroring matrices also reverse the winding so faces keep facing out.
void transform(ir::Mesh& mesh, const ir::Mat4& matrix);
/// Uniform scale about the origin.
void scale(ir::Mesh& mesh, float factor);
/// Move the bounding-box centre to the origin.
void center(ir::Mesh& mesh);
/// Centre at the origin and scale so the longest side is 1.
void scale_to_unit(ir::Mesh& mesh);

enum class UpAxis { Y, Z };
/// Rotate between +Z-up and +Y-up conventions (right-handed, front = -Y in
/// Z-up becomes front = +Z in Y-up): Z->Y maps (x, y, z) to (x, z, -y).
void convert_up_axis(ir::Mesh& mesh, UpAxis from, UpAxis to);
/// Uniformly rescale so that one unit becomes `to_meters` metres, using
/// `mesh.unit_meters` as the current unit (nothing happens when it is 0);
/// sets `mesh.unit_meters = to_meters`.
void convert_units(ir::Mesh& mesh, double to_meters);

// --------------------------------------------------------------------------- triangulation

/// Ear-clipping triangulation of one simple polygon given by its corner
/// positions (any orientation, any plane, concave allowed). Returns
/// triangles as corner indices 0..n-1 (3 per triangle, n-2 triangles for a
/// proper polygon), wound like the input. Robust against collinear and
/// repeated corners and against self-intersecting input (falls back to
/// clipping the "best" remaining corner); never loops forever.
[[nodiscard]] std::vector<std::uint32_t> triangulate_polygon(std::span<const ir::Vec3> points);
/// Same for a polygon already projected to 2D (x, y of Vec2).
[[nodiscard]] std::vector<std::uint32_t> triangulate_polygon_2d(std::span<const ir::Vec2> points);
/// Replace every polygon by triangles (ear clipping); `face_sizes` becomes
/// empty and group/object face ranges are remapped.
void triangulate(ir::Mesh& mesh);

// --------------------------------------------------------------------------- normals

/// Compute per-vertex normals. `crease_degrees` <= 0 gives flat shading
/// (every face its own vertices), >= 180 fully smooth shading; in between,
/// faces meeting at a sharper angle keep separate normals. Smoothing works
/// across vertices that share a position, even when they were split for
/// texture seams. Vertices are split where needed (faces keep their shape).
void generate_normals(ir::Mesh& mesh, float crease_degrees = 180.0f);
/// Normals from smoothing groups (OBJ "s", 3DS bit masks): two faces that
/// meet at a vertex position share a smooth normal when their groups
/// match — equal values, or a common bit when `bitmask` is set; group 0
/// is flat. `face_groups` has one entry per face (missing entries = 0).
void generate_normals_by_groups(ir::Mesh& mesh, std::span<const std::uint32_t> face_groups, bool bitmask);
/// Normalise existing normals; replaces zero or non-finite ones.
void normalize_normals(ir::Mesh& mesh);

// --------------------------------------------------------------------------- cleanup

/// Merge vertices whose positions are within `tolerance` (0 = bit-exact)
/// and whose other attributes match (within `attribute_tolerance`).
/// With `positions_only` the other attributes are ignored (the first
/// vertex's values are kept). Returns the number of vertices removed.
std::size_t weld(ir::Mesh& mesh, float tolerance = 0.0f, bool positions_only = false,
                 float attribute_tolerance = 1e-6f);
/// Drop vertices no face references (point clouds are left alone).
/// Returns the number removed.
std::size_t remove_unused_vertices(ir::Mesh& mesh);
/// Drop faces with repeated corners or zero area and (when
/// `remove_duplicates`) faces that repeat another face's vertex set.
/// Returns the number of faces removed.
std::size_t remove_degenerate_faces(ir::Mesh& mesh, bool remove_duplicates = true, float area_epsilon = 0.0f);
/// Reverse the corner order of every face.
void flip_winding(ir::Mesh& mesh);
/// Make winding consistent across shared edges in each connected part, then
/// orient closed parts outward (positive volume). Returns the number of
/// faces flipped.
std::size_t fix_winding(ir::Mesh& mesh);
/// Reorder triangles for the post-transform vertex cache (Forsyth's
/// algorithm) and vertices in first-use order. Faces stay inside their
/// group/object. Triangulated meshes only (others are left alone).
void optimize_indices(ir::Mesh& mesh);
/// Snap positions to a grid of 2^bits steps across the bounding box
/// (bits 4..24), as quantised formats store them.
void quantize_positions(ir::Mesh& mesh, int bits);

// --------------------------------------------------------------------------- vertex/face bookkeeping

/// Keep vertex `old` as `old_to_new[old]` (or drop it when the entry is
/// UINT32_MAX); `new_count` vertices remain. When several old vertices map
/// to one new vertex, the first one's attributes win. Indices are remapped.
void remap_vertices(ir::Mesh& mesh, std::span<const std::uint32_t> old_to_new, std::size_t new_count);
/// After faces were replaced: old face f became new faces
/// [first_new[f], first_new[f + 1]); `first_new` has face_count + 1
/// entries. Updates group and object ranges.
void remap_face_ranges(ir::Mesh& mesh, std::span<const std::size_t> first_new);
/// Offsets of each face's first index (face_count + 1 entries).
[[nodiscard]] std::vector<std::size_t> face_offsets(const ir::Mesh& mesh);
/// The faces [first_face, first_face + count) as a standalone mesh with only
/// the vertices they use; materials and textures are copied, groups clipped
/// and rebased, objects dropped.
[[nodiscard]] ir::Mesh extract_faces(const ir::Mesh& mesh, std::size_t first_face, std::size_t count);
/// Append `part` (indices offset, groups/objects shifted, materials and
/// textures merged by name). Missing per-vertex arrays are filled with
/// defaults when only one side has them.
void append(ir::Mesh& into, const ir::Mesh& part);

/// One object of a scene ready for a node-based writer.
struct ObjectPart {
  std::string name;
  /// The object's geometry, in its LOCAL space when `restore_local` was
  /// requested (the inverse of `transform` applied), else in scene space.
  ir::Mesh mesh;
  /// Local-to-scene matrix (identity when not restored).
  ir::Mat4 transform;
};
/// Split a mesh into its objects (faces outside every object form a final
/// unnamed part). A mesh without objects gives one part.
[[nodiscard]] std::vector<ObjectPart> split_objects(const ir::Mesh& mesh, bool restore_local);

// --------------------------------------------------------------------------- simplification

struct SimplifyOptions {
  /// Fraction of triangles to keep (0..1]; used when target_triangles is 0.
  float target_ratio = 0.5f;
  /// Explicit triangle budget; 0 = use target_ratio.
  std::size_t target_triangles = 0;
  /// Keep open borders and attribute seams in place.
  bool preserve_boundary = true;
  /// Stop collapsing when the next collapse would move the surface by more
  /// than this (relative to the bounding-box diagonal); <= 0 = no limit.
  float max_relative_error = 0.0f;
};
/// Quadric error metric edge-collapse simplification (Garland-Heckbert)
/// with boundary/seam preservation and normal-flip prevention. Triangulates
/// first. Normals are recomputed when present. Returns false when
/// cancelled through `keep_going` (called with progress 0..1; the mesh is
/// then left valid but partly simplified).
bool simplify(ir::Mesh& mesh, const SimplifyOptions& options,
              const std::function<bool(double)>& keep_going = {});
/// The standard `quality` (1..100) as a triangle ratio: 90 and above keep
/// everything (1.0); below that the ratio falls smoothly to 0.02 at 1.
[[nodiscard]] float quality_to_ratio(int quality) noexcept;

}  // namespace cvt::mesh
